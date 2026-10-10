// UdpServer.cpp — 통신 층(원본 `internal/netstr` 자리). 설명은 `UdpServer.h` 머리.
//
// ★ 10-08 조각 3 — 아래 몸통은 옛 `Server.cpp` 의 `OnPacket` 앞부분(풀기·머리·순번)·접속 제어 다섯 갈래와
//   `EmitNow`(머리·순번·되받기·XOR)를 옮긴 것이다. 주석도 그대로 옮겼다(`Docs/LAYERS_DESIGN.md` 조각 3).
#include "nox/UdpServer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "nox/ByteBuffer.h"
#include "nox/JoinToken.h"
#include "nox/Messages.h"
#include "nox/MsgLen.h"
#include "nox/Opcode.h"
#include "nox/Wire.h"

namespace nox {

// ★ 10-08 조각 4 — 처음 보는 주소에 세션 번호를 매긴다. 게임은 이 번호로만 사람을 가른다(`Docs/LAYERS_DESIGN.md` 조각 4).
//   지운 뒤 같은 주소로 다시 오면 새 번호다.
CUdpSession& CUdpServer::SessionFor(const std::string& addr) {
    auto it = _sessions.find(addr);
    if (it != _sessions.end()) return it->second;
    CUdpSession s;
    s.addr = addr;
    s.sessionId = _nextSessionId++;
    _addrOf.emplace(s.sessionId, addr);
    return _sessions.emplace(addr, s).first->second;
}

int64_t CUdpServer::OpenSession(const std::string& addr) {
    CUdpSession& s = SessionFor(addr);
    s.announced = true;   // 소켓 없이 게임에 세우는 사람 — 게임이 지운다
    return s.sessionId;
}

int64_t CUdpServer::SessionIdOf(const std::string& addr) const {
    auto it = _sessions.find(addr);
    return it == _sessions.end() ? 0 : it->second.sessionId;
}

const std::string& CUdpServer::AddrOf(int64_t sessionId) const {
    static const std::string kNone;
    auto it = _addrOf.find(sessionId);
    return it == _addrOf.end() ? kNone : it->second;
}

void CUdpServer::RequestDisconnectSession(int64_t sessionId) {
    auto it = _addrOf.find(sessionId);
    if (it == _addrOf.end()) return;
    _sessions.erase(it->second);
    _addrOf.erase(it);
}

void CUdpServer::OnRequest(const NetworkRequest& r, std::vector<OutPacket>& out) {
    switch (r.type) {
        case NetworkRequest::Type::SEND_MSG:
            RequestSendMsg(r.sessionId, r.reliable, r.stream, r.body.data(), r.body.size(), out);
            break;
        case NetworkRequest::Type::DISCONNECT_SESSION:
            RequestDisconnectSession(r.sessionId);
            break;
        case NetworkRequest::Type::RESPOND_JOIN:
            RespondJoin(r.sessionId, r.accept, r.body, out);
            break;
        case NetworkRequest::Type::SET_SESSION_TIMEOUT:
            RequestSetSessionTimeout(r.sessionId, r.timeoutMs);
            break;
    }
}

void CUdpServer::CloseAll(std::vector<OutPacket>& out) {
    const uint8_t msg[1] = { static_cast<uint8_t>(Op::SERVER_CLOSE) };
    for (auto& kv : _sessions) {
        if (!kv.second.keyed) continue;
        SendOn(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        ++_stats.serverCloseSent;
    }
}

void CUdpServer::RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg,
                             std::vector<OutPacket>& out) {
    auto a = _addrOf.find(sessionId);
    if (a == _addrOf.end()) { ++_stats.joinRespondNoSession; return; }   // 답하는 사이 걷혔다
    CUdpSession& s = _sessions.at(a->second);
    if (accept) {
        const uint8_t msg[1] = {static_cast<uint8_t>(Op::SERVER_JOIN_OK)};
        SendOn(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        return;
    }
    ++_stats.joinRejected;
    if (!rejectMsg.empty()) SendOn(s, /*reliable=*/false, /*stream=*/0, rejectMsg.data(), rejectMsg.size(), out);
}

void CUdpServer::RequestSetSessionTimeout(int64_t sessionId, uint32_t timeoutMs) {
    auto it = _addrOf.find(sessionId);
    if (it == _addrOf.end()) return;
    CUdpSession& s = _sessions.at(it->second);
    s.timeoutMs = timeoutMs;
    ArmIdle(s);   // ★ 조각 5b — 한도가 줄면 더 이른 마감을 건다
}

void CUdpServer::DebugTouch(int64_t sessionId) {
    auto it = _addrOf.find(sessionId);
    if (it == _addrOf.end()) return;
    CUdpSession& s = _sessions.at(it->second);
    s.heard = true;
    s.lastSeenMs = _nowMs;
    ArmIdle(s);
}

CUdpStats CUdpServer::Stats() const {
    CUdpStats s = _stats;
    s.reliablePending = reliablePending();
    s.timerCount = _timers.size();
    return s;
}

void CUdpServer::PushTimer(const Timer& t) {
    _timers.push_back(t);
    std::push_heap(_timers.begin(), _timers.end(), TimerLater);
}

bool CUdpServer::NextTimerDue(uint32_t& dueMs) const {
    if (_timers.empty()) return false;
    dueMs = _timers.front().dueMs;
    return true;
}

void CUdpServer::ArmIdle(CUdpSession& s) {
    if (!IdleWatched(s)) return;
    const uint32_t due = s.lastSeenMs + IdleLimitOf(s) + 1;   // 조용한 시간이 한도를 「넘는」 첫 ms(옛 `quiet > limit`)
    // 걸린 마감이 더 이르거나 같으면 그대로 둔다 — 그 항목을 꺼낼 때 받은 시각으로 다시 셈한다
    if (s.idleArmed && static_cast<int32_t>(due - s.idleDueMs) >= 0) return;
    s.idleArmed = true;
    s.idleDueMs = due;
    Timer t;
    t.dueMs = due;
    t.sessionId = s.sessionId;
    t.kind = TimerKind::IDLE;
    PushTimer(t);
}

size_t CUdpServer::OnDatagram(const std::string& from, const uint8_t* data, size_t n,
                              std::vector<OutPacket>& out, std::vector<NetworkEvent>& events) {
    CUdpSession& s = SessionFor(from);
    s.lastSeenMs = _nowMs;   // ★ 조각 4c — 접속 전 상대 걷어내기의 근거(`Tick`)
    s.heard = true;          // ★ 조각 5a — 시간 초과 감시도 이 시각으로 센다(접속 제어만 실린 조각 — 핑 등 — 도 「살아 있다」)
    ArmIdle(s);              // ★ 조각 5b — 처음 보거나 처음 받은 세션이면 마감을 건다(걸려 있으면 그대로 — 꺼낼 때 다시 셈)

    // 받은 것이 암호문이면 먼저 푼다. 사본을 뜬다 — 부르는 쪽 버퍼를 건드리지 않는다.
    // ⚠ 덤프 재생일 때는 이미 평문이라 풀면 안 된다 (SetInboundPlain 주석 참조).
    std::vector<uint8_t> buf(data, data + n);
    if (s.keyed && !_inboundPlain) ApplyXor(buf.data(), buf.size(), s.xorKey);

    // ★ 10-10 토큰 작업 2 — 키 받기 전 데이터그램 끝의 입장 토큰 꼬리(`… 토큰 51 · NXT1`)를 쪼개기 전에 뗀다
    //   (`Docs/TOKEN_DESIGN.md`). 녹스 패킷엔 길이 칸이 없어 남겨 두면 꼬리 바이트를 다음 메시지로 잘못 읽는다 — 길이 모르는 번호에서
    //   멈춰 답을 안 하거나(아래 「깨끗하게 쪼개지는지」), 핑 같은 접속 제어로 처리한다. 둘째 자리 `00` 에서 끝나면 티가 안 나기도 한다
    //   (10-10 처음 짠 시험이 그래서 못 잡았다 — `proto_test` `TestJoinTrailer`).
    //   클라 DLL 이 접속 시도(`0x0e`)와 CONNECT(`ff 00 00`)에 붙인다. 키 받은 뒤의 암호문은 보지 않는다.
    std::vector<uint8_t> joinToken;
    if (!s.keyed) {
        const uint8_t* tok = nullptr;
        size_t kept = 0;
        if (FindJoinTrailer(buf.data(), buf.size(), kWireHeaderSize + 1, tok, kept)) {
            joinToken.assign(tok, tok + kJoinTokenSize);
            buf.resize(kept);
            ++_stats.joinTrailers;
        }
    }

    WireHeader head;
    if (!ReadWireHeader(buf.data(), buf.size(), head)) return 0;

    // ★ B-1b — 클라가 **신뢰**로 보냈으면 그 순번을 적어 둔다. 답할 때 실어 주는 것이
    //   정답지의 되받기다(StreamSeq::recvSeq 주석). 안 실으면 클라가 영원히 재전송한다.
    if (head.reliable) {
        StreamSeq& rq = s.seq[head.stream & 0x7F];
        rq.recvSeq = head.seq;
        rq.recvSeqSeen = true;
        // 되받기 기준 스트림. ⚠ **접속 전 패킷(`0xff` = anyID · 스트림 127)은 빼야 한다** —
        //   그건 아직 연결이 없는 상태의 `SERVER_CONNECT` 라 정품은 되받기 대상으로 안 본다.
        //   ⭕ 근거 — 정품 녹음 idx 14~15: 클라가 `ff 00 00` 을 보낸 직후 서버가
        //     `70 70 74`(XOR 풀면 `00 00 04`) 로 **seq 0** 을 보낸다. 우리는 1 을 보내고 있었고
        //     그것이 `replay_connect` 에 마지막으로 남은 1건이었다.
        if ((head.stream & 0x7F) != 0x7F) {
            s.lastRelStream = static_cast<int>(head.stream & 0x7F);
        }
        ++_stats.reliableHeard;
        if ((head.stream & 0x7F) < 8) ++_stats.recvRelStream[head.stream & 0x7F];
    }
    // ★ 10-08 조각 3b — 클라의 **비신뢰** 조각 머리 순번은 「서버가 보낸 신뢰 가운데 다음에 받을 순번」이다.
    //   바뀔 때만 처리한다(원본 `conn.go:659-665` — `seq != dst.seqRecv` 이면 `SetCurrent(seq)`).
    //   정품 녹음 — 클라는 서버 신뢰 0·1·2 를 받은 뒤로 비신뢰 머리에 늘 3 을 싣는다(`CAP_GEN3` 접속 59552).
    // ★ 10-09 조각 5b — 「그 순번부터 다시」로 표시된 것은 다음 틱을 안 기다리고 여기서 바로 보낸다(`Resend`).
    if (!head.reliable && static_cast<int>(head.seq) != s.lastAckSeen) {
        s.lastAckSeen = head.seq;
        Ack(s, head.seq);
        Resend(s, out);
    }

    const uint8_t* body = buf.data() + kWireHeaderSize;
    const size_t bodyLen = buf.size() - kWireHeaderSize;

    // ★ 10-08 조각 3 — **앞에서부터 0x20(`CLIENT_ACCEPT`) 미만은 접속 제어라 여기서 처리하고,
    //   처음 0x20 이상이 나오는 자리부터 끝까지는 게임 메시지라 게임에 넘긴다.**
    //   원본 — 첫 번호가 0x20 이상이면 조각째 게임으로(`netstr/conn.go:709-712`), 미만이면 `processStreamOp`(`:704`).
    //     단 접속 직후 첫 신뢰 조각 `[1f ACCEPTED 순번][20 CLIENT_ACCEPT …]` 은 앞을 먹고 `0x20` 부터 게임에 넘긴다
    //     (`readXxx` `conn.go:397-411`). 정품 클라가 실제로 이렇게 보낸다(정품 녹음 `replay_connect`).
    //   ⚠ 처음엔 첫 번호 하나로 조각째 갈랐다가 위 조각의 `CLIENT_ACCEPT` 를 못 넘겨 녹음 대조가 깨졌다(10-08).
    //   ⚠ 전에는 메시지마다 갈랐다 — 게임 조각 안에 섞인 0x20 미만 번호(핑 등)도 받아 처리했다. 이제는 게임 몫이다(원본 쪽).
    //   빈 몸통도 게임으로 넘긴다(전처럼 「메시지 0개」로 끝난다).
    if (bodyLen == 0 || body[0] >= static_cast<uint8_t>(Op::CLIENT_ACCEPT)) {
        NetworkEvent ev;
        ev.type = NetworkEvent::Type::RECEIVED;
        ev.sessionId = s.sessionId;
        s.announced = true;
        ev.addr = from;
        ev.body.assign(body, body + bodyLen);
        events.push_back(std::move(ev));
        return 0;
    }

    // ★ 먼저 **끝까지 깨끗하게 쪼개지는지** 본다. 안 되면 아무 답도 만들지 않는다.
    //   (게임 조각에도 같은 검사가 있다 — `Server::OnGameMessages` 의 같은 주석 참조)
    const auto endsAtZero = [](size_t cnt, uint8_t op) {
        return cnt > 1 && op == static_cast<uint8_t>(Op::SERVER_CONNECT);
    };
    {
        MessageIter probe(body, bodyLen, /*fromClient=*/true);
        while (probe.Next()) {
            if (endsAtZero(probe.count(), probe.op())) break;
        }
        if (probe.stopped()) {
            ++_stats.stopped;
            const uint8_t sop = static_cast<uint8_t>(probe.stopOp());
            ++_stats.stoppedByOp[sop];
            if (_stopSampleShown.insert(sop).second) {
                const size_t rest = probe.stopRest();
                const size_t base = (bodyLen >= rest) ? bodyLen - rest : 0;
                // ★ 10-09 — 한 줄을 다 만든 뒤 한 번에 찍는다(나눠 찍으면 게임 스레드의 줄이 그 사이에 낀다)
                char b[96];
                std::snprintf(b, sizeof(b), "[멈춤표본] op=%02x 남은 %zu |", sop, rest);
                std::string line = b;
                for (size_t q = 0; q < rest && q < 8; ++q) {
                    std::snprintf(b, sizeof(b), " %02x", body[base + q]);
                    line += b;
                }
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
            }
            return 0;
        }
    }

    MessageIter it(body, bodyLen, /*fromClient=*/true);
    size_t count = 0;
    _lastFragOps.clear();   // 진단 — 이 조각에서 무엇을 읽었는지 남긴다
    while (it.Next()) {
        ++count;
        if (endsAtZero(count, it.op())) { ++_stats.connectTailStops; break; }
        if (it.op() >= static_cast<uint8_t>(Op::CLIENT_ACCEPT)) {
            // 여기부터 끝까지 게임 몫(위 주석 · `readXxx`). 이 메시지부터는 게임이 센다.
            const uint8_t* rest = it.body() - 1;   // 이 메시지의 오프코드 자리(`MessageIter::Next`)
            NetworkEvent ev;
            ev.type = NetworkEvent::Type::RECEIVED;
            ev.sessionId = s.sessionId;
            s.announced = true;
            ev.addr = from;
            ev.body.assign(rest, body + bodyLen);
            events.push_back(std::move(ev));
            --count;
            break;
        }
        ++_stats.seen[it.op()];
        if (_lastFragOps.size() < 16) _lastFragOps.push_back(it.op());

        switch (static_cast<Op>(it.op())) {
            // 핑에 그대로 되돌린다. 값은 클라의 시각이라 **손대지 않는다.**
            // 실측 = `01 04 06 <4바이트>` 에 `01 00 08 <같은 4바이트>` 로 답했다.
            // 스트림은 받은 것을 그대로 쓴다(핑·퐁은 스트림 1 을 쓰더라).
            case Op::CLIENT_PING: {
                if (it.bodyLen() != 4) break;
                uint8_t msg[5];
                msg[0] = static_cast<uint8_t>(Op::CLIENT_PONG);
                std::memcpy(msg + 1, it.body(), 4);
                SendOn(s, /*reliable=*/false, head.stream, msg, sizeof(msg), out);
                ++_stats.handled;
                break;
            }

            // ★ 10-08 조각 3b — 클라의 `0x1f ACCEPTED 순번` — 그 순번도 「다음에 받을 순번」이다(원본 `processStreamOp`
            //   `conn.go:493-512` — 바뀌면 `SetCurrent`). 정품 클라는 키를 받은 뒤 첫 신뢰 조각 맨 앞에 싣는다.
            case Op::ACCEPTED: {
                if (it.bodyLen() < 1) break;
                const uint8_t seq = it.body()[0];
                if (static_cast<int>(seq) != s.lastAckSeen) {
                    s.lastAckSeen = seq;
                    Ack(s, seq);
                    Resend(s, out);   // ★ 조각 5b — 표시된 것은 바로
                }
                ++_stats.handled;
                break;
            }

            // 클라가 접속 자체를 닫는다(`0x0a`). 게임이 「나감」으로 표시하고 Tick 이 걷어낸다
            // (게임 쪽 `0xae OUTGOING_CLIENT` 와 같은 길 — `Server::OnNetworkEvent` 주석).
            case Op::CLIENT_CLOSE: {
                NetworkEvent ev;
                ev.type = NetworkEvent::Type::DISCONNECTED;
                ev.reason = DisconnectReason::CLIENT_CLOSE;
                ev.sessionId = s.sessionId;
                s.announced = true;
                ev.addr = from;
                events.push_back(std::move(ev));
                ++_stats.handled;
                break;
            }

            case Op::SERVER_DISCOVER: {
                // 이미 붙은 상대가 다시 찾을 리 없다. 상태를 지켜야 밀린 바이트에
                // 속지 않는다.
                if (s.keyed) break;
                if (_infoBody.empty()) break;
                MsgServerDiscover req;
                Reader r(it.body(), it.bodyLen());
                if (!req.Decode(r)) break;

                // 원본을 그대로 두고 토큰 자리만 바꾼다.
                std::vector<uint8_t> msg;
                msg.reserve(1 + _infoBody.size());
                msg.push_back(static_cast<uint8_t>(Op::SERVER_INFO));
                msg.insert(msg.end(), _infoBody.begin(), _infoBody.end());
                if (_infoBody.size() >= kInfoTokenOffset + 4) {
                    uint8_t* t = msg.data() + 1 + kInfoTokenOffset;
                    t[0] = static_cast<uint8_t>(req.token);
                    t[1] = static_cast<uint8_t>(req.token >> 8);
                    t[2] = static_cast<uint8_t>(req.token >> 16);
                    t[3] = static_cast<uint8_t>(req.token >> 24);
                }
                SendOn(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
                ++_stats.handled;
                break;
            }

            case Op::SERVER_TRY_JOIN: {
                if (s.keyed) break;   // 이미 붙은 상대 — 게임에 안 묻는다
                // 지금은 조건 없이 받아준다. 거절 사유(중복 일련번호·인원 참·버전)는
                // 세션 관리를 만들 때 붙인다.
                // ★ 10-09 조각 5c — 받아 줄지는 게임이 정한다(`JOIN_REQUESTED` → 게임 `OnJoinRequested` → `RespondJoin`).
                //   위 「조건 없이」는 이제 게임 쪽 몫이다. 이 세션은 게임에 넘긴 것으로 치지 않는다(`announced` 그대로) —
                //   접속 시도만 한 상대는 계속 통신 층이 걷는다(조각 4c).
                NetworkEvent ev;
                ev.type = NetworkEvent::Type::JOIN_REQUESTED;
                ev.sessionId = s.sessionId;
                ev.addr = from;
                ev.body.assign(it.body(), it.body() + it.bodyLen());
                ev.token = joinToken;
                ++_stats.joinRequested;
                events.push_back(std::move(ev));
                ++_stats.handled;
                break;
            }

            case Op::SERVER_CONNECT: {
                // 이미 키를 준 상대가 다시 보내면 재전송이다. 번호를 새로 매기지 않는다.
                // ★ 다만 **답은 한다** — 정품은 이 두 번째 요청에 0x04 를 하나 돌려준다
                //   (3바이트·비신뢰·본문 0). 09-05 정품 덤프의 자리 그대로다.
                //   무엇인지는 아직 모른다(PROTOCOL.md 도 「미상」).
                if (s.keyed) {
                    const uint8_t m4[1] = {static_cast<uint8_t>(Op::CODE4)};
                    SendOn(s, /*reliable=*/false, /*stream=*/0, m4, sizeof(m4), out);
                    ++_stats.handled;
                    break;
                }
                // ★ 10-08 조각 3b — 접속 전 패킷(서버 찾기·접속 시도)이 머리 순번 0 으로 이 값을 이미 바꿔 놓았다.
                //   원본은 접속 때 사람 연결을 새로 만들며 `seqRecv = -1` 로 시작한다(`netstr.go:257-263`) — 그래야
                //   접속 뒤 클라의 첫 「다음은 0」이 접속 수락을 바로 다시 보내게 한다.
                s.lastAckSeen = -1;
                NetworkEvent ev;
                ev.type = NetworkEvent::Type::CONNECTED;
                ev.sessionId = s.sessionId;
                s.announced = true;
                ev.addr = from;
                ev.token = joinToken;
                if (_scriptAt < _script.size()) {
                    // 덤프 재생 — 그때 서버가 정한 값들을 그대로 쓴다.
                    //   프레임·오브젝트 번호는 게임 것이라 이벤트에 실어 넘긴다.
                    const AcceptScript& a = _script[_scriptAt++];
                    s.connectId = a.connectId;
                    s.xorKey = a.xorKey;
                    ev.scripted = true;
                    ev.frame = a.frame;
                    ev.netCode = a.netCode;
                } else {
                    s.connectId = _nextConnectId++;
                    s.xorKey = _xorKey;
                }

                uint8_t msg[8];
                size_t k = 0;
                msg[k++] = static_cast<uint8_t>(Op::ACCEPTED);
                msg[k++] = 0;                      // 스트림을 받아들였다는 표시
                msg[k++] = static_cast<uint8_t>(Op::SERVER_ACCEPT);
                msg[k++] = static_cast<uint8_t>(s.connectId);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 8);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 16);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 24);
                msg[k++] = s.xorKey;

                // ★ 이 조각까지는 평문이다. 키는 이 안에 실려 나간다.
                SendOn(s, /*reliable=*/true, /*stream=*/0, msg, k, out);
                s.keyed = true;   // 다음 조각부터 뒤집는다
                ++_stats.handled;
                events.push_back(std::move(ev));
                break;
            }

            default:
                break;   // 접속 제어 조각 안의 그 밖 번호 — 세기만 했다.
        }
    }
    return count;
}

void CUdpServer::RequestSendMsg(int64_t sessionId, bool reliable, uint8_t stream,
                                const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    auto a = _addrOf.find(sessionId);
    if (a == _addrOf.end()) { ++_stats.sendNoSession; return; }   // 지운 세션 — 보낼 곳이 없다
    SendOn(_sessions.at(a->second), reliable, stream, msg, n, out);
}

void CUdpServer::SendOn(CUdpSession& s, bool reliable, uint8_t stream,
                        const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    OutPacket p;
    p.to = s.addr;
    p.data.resize(kWireHeaderSize + n);
    WireHeader h;
    h.reliable = reliable;
    h.stream = stream;

    // 순번 규칙 — StreamSeq 주석 참조. 신뢰는 쓰고 올리고, 비신뢰는 직전 값을 싣는다.
    StreamSeq& q = s.seq[stream & 0x7F];
    {   // 스트림별로 갈라 센다 — 합계만으로는 「보낸 스트림 ↔ 답한 스트림」 어긋남을 못 본다
        const uint8_t st = static_cast<uint8_t>(stream & 0x7F);
        if (st < 8) { if (reliable) ++_stats.sendRelStream[st]; else ++_stats.sendPlainStream[st]; }
    }
    if (reliable) {
        h.seq = q.next;
        q.last = q.next;
        ++q.next;
    } else if (_ackRecvSeq && _ackAnyStream && (stream & 0x7F) == 0 &&
               s.lastRelStream >= 0 && s.seq[s.lastRelStream].recvSeqSeen) {
        // ★★★ 2026-09-18 — **되받기는 「스트림 0 비신뢰」에만 싣는다.**
        //
        //   ⭕ 정품 녹음으로 확정했다(`_evidence/20260905_server_bidir.txt` idx 24~45 ·
        //     XOR 0x70 을 풀고 읽는다) —
        //       idx=29  `71 70` → 비신뢰·**스트림 1**·seq **0**   ← 스트림 1 엔 늘 0
        //       idx=32  `70 75` → 비신뢰·**스트림 0**·seq **5**   ← 그 직전 클라 신뢰가 4
        //       idx=37  `70 77` → 비신뢰·**스트림 0**·seq **7**   ← 그 직전 클라 신뢰가 6
        //     ⇒ **스트림 0 에만 「받은 순번 + 1」**. 우리 값 규칙은 맞았고 **자리가 틀렸다.**
        //
        //   ⭕ 왜 스트림 0 인가 — 클라의 수신 루프(`netstr/conn.go:643~650`)에
        //     **스트림 1 패킷이 안 온다.** 실측 — 스트림 1 에 `ack=4` 를 2,743 번 실었는데
        //     클라가 본 비신뢰 seq 분포는 `2:12 3:9389` 로 **4 가 0 건**이었다.
        //
        //   ⚠ 한때 **모든 스트림**에 실었다가 `replay_connect` 가 깨졌다 —
        //     정품이 스트림 1 에 싣는 `0` 자리에 우리가 5·7 을 넣었기 때문이다.
        //
        //   값 규칙 — `ackOlder(N+1)` 이 N 이하를 지운다(`send_reliable.go:178~199`).
        const StreamSeq& rq = s.seq[s.lastRelStream];
        const uint8_t ack = static_cast<uint8_t>(rq.recvSeq + 1);
        // ✖ **정정 09-21 — 여기서 재던 「다름」은 뜻이 없었다.**
        //   `ack`(클라가 보낸 신뢰 순번 +1)과 `q.last`(우리가 스트림 0 에 마지막으로 쓴 순번)는
        //   **서로 다른 것**이라 거의 늘 다르다. 실측 `되받기 7874 · 다름 7866` 은 결함이 아니라
        //   **엉뚱한 둘을 견준 숫자**였고, 그게 「B-1b 가 아직 안 됐다」로 읽혔다(B-1b 는 09-18 에 닫혔다).
        // ⭕ 뜻이 있는 것은 **되받기 값이 올라가나**다 — 안 올라가면 클라의 신뢰 순번이 멈춘 것이고,
        //   그러면 클라의 재전송 줄이 안 비워진다. 그래서 **직전에 보낸 되받기 값과** 견준다.
        if (_stats.ackSeqApplied != 0 && ack != _stats.ackLastSent) ++_stats.ackSeqChanged;
        _stats.ackLastSent = ack;
        ++_stats.ackSeqApplied;
        if (ack < 32) ++_stats.ackValueHist[ack];
        h.seq = ack;
    } else if (false) {   // ⚠ 아래는 **죽은 가지**다. 지우지 않고 남긴 까닭은 주석에.
        // ★ 옛 B-1b — 비신뢰 답에 **클라가 보낸 마지막 신뢰 순번**을 싣되 **스트림을 안 가렸다.**
        //   ✖ 09-18 폐기 — 스트림 1 에도 실려 `replay_connect` 가 깨졌다.
        //     정품은 스트림 1 에 **늘 0** 을 싣는다(위 가지의 정품 녹음 근거).
        //   기록으로 남기는 값 규칙은 위 가지와 같다.
        // ★★ **받은 순번 + 1** 을 싣는다. 그대로 실으면 안 된다 —
        //   정답지 `SetCurrent(seq)` 는 `retry(seq)` + `ackOlder(seq)` 인데
        //   `retry` 가 **그 순번을 다시 보내라고 표시**하고, `ackOlder` 는
        //   `it.seq >= seq` 를 **남긴다**(send_reliable.go:165~199).
        //   즉 N 을 돌려주면 N 번 패킷이 지워지기는커녕 재전송 예약된다.
        //   N+1 을 주어야 N 이하가 전부 지워진다.
        //   09-18 실측 — N 으로 37번 답했는데 준비 20 그대로였다(대조군과 동일).
        const uint8_t ack = static_cast<uint8_t>(q.recvSeq + 1);
        if (ack != q.last) ++_stats.ackSeqDiffer;
        ++_stats.ackSeqApplied;
        // ★ 2026-09-18 — **보낸 ack 값 자체를 센다.** 클라 쪽 로그에 0·2·3 만 찍히고
        //   4 가 안 보여서, 「우리가 4 를 안 보내나」와 「보내는데 클라가 안 읽나」를 갈라야 한다.
        //   값이 한두 개뿐이면 `recvSeq` 가 안 올라가는 것이다.
        if (ack < 32) ++_stats.ackValueHist[ack];
        h.seq = ack;
    } else {
        h.seq = q.last;
    }
    p.data[0] = h.Byte0();
    p.data[1] = h.seq;
    std::memcpy(p.data.data() + kWireHeaderSize, msg, n);

    // ★ 10-08 조각 3b — 신뢰 조각은 확인이 올 때까지 들고 있는다(원본 `reliableSender.add`·`sendAtSeq` — 보낸 뒤 2초가 재시도 시각).
    if (reliable && _reliableResend) {
        CUdpSession::PendingReliable pr;
        pr.wire = p.data;   // XOR 전
        pr.seq = h.seq;
        pr.retryAt = _nowMs + kReliableRetryMs;
        Timer t;            // ★ 10-09 조각 5b — 그 재시도 시각에 꺼낼 타이머(버린 조각의 것은 꺼낼 때 못 찾아 버린다)
        t.dueMs = pr.retryAt;
        t.sessionId = s.sessionId;
        t.kind = TimerKind::RESEND;
        t.seq = h.seq;
        s.reliableQueue.push_back(std::move(pr));
        if (s.reliableQueue.size() > kReliableQueueMax) {
            s.reliableQueue.erase(s.reliableQueue.begin());
            ++_stats.reliableQueueDropped;
        }
        PushTimer(t);
    }

    // 키를 준 뒤로는 조각 전체가 뒤집힌다 — 머리도 포함이다.
    // (덤프의 `70 70 74` 가 XOR 0x70 을 풀면 `00 00 04` 인 것이 그 증거다)
    if (s.keyed) ApplyXor(p.data.data(), p.data.size(), s.xorKey);
    out.push_back(std::move(p));
}

// 원본 `SetCurrent(seq)` = `retry(seq)` + `ackOlder(seq)`(`send_reliable.go:183-239`).
//   retry — 그 순번이 아직 있으면 「다시 보낼 것」으로 표시한다(클라가 그것부터 못 받았다는 뜻).
//   ackOlder — 그보다 앞 순번은 받았으니 지운다. 원본 그대로 바이트 크기로 견준다(순번이 255 를 넘어 돌 때의 처리는 원본도 없다 — `FIXME`).
void CUdpServer::Ack(CUdpSession& s, uint8_t seq) {
    for (auto& p : s.reliableQueue)
        if (p.seq == seq) { p.shouldSend = true; ++_stats.reliableRetryMarked; }
    const size_t before = s.reliableQueue.size();
    s.reliableQueue.erase(std::remove_if(s.reliableQueue.begin(), s.reliableQueue.end(),
                                         [seq](const CUdpSession::PendingReliable& p) { return p.seq < seq; }),
                          s.reliableQueue.end());
    _stats.reliableAcked += before - s.reliableQueue.size();
}

// 「그 순번부터 다시」로 표시된 것을 내보낸다. 원본은 프레임마다 `FlushAndPoll` → `Resend` 를 부른다(`send.go:74`) —
//   ★ 10-09 조각 5b 부터는 표시한 그 자리(`OnDatagram`)에서 바로 부른다(네트워크 스레드에는 프레임이 없다).
void CUdpServer::Resend(CUdpSession& s, std::vector<OutPacket>& out) {
    for (auto& p : s.reliableQueue)
        if (p.shouldSend) ResendOne(s, p, out);
}

void CUdpServer::ResendOne(CUdpSession& s, CUdpSession::PendingReliable& p, std::vector<OutPacket>& out) {
    p.shouldSend = false;
    p.retryAt = _nowMs + kReliableRetryMs;
    OutPacket o;
    o.to = s.addr;
    o.data = p.wire;
    // 원본도 다시 보낼 때 그때의 키로 뒤집는다 — 키를 주기 전에 평문으로 나간 접속 수락도 다시 보낼 때는 뒤집힌다
    // (`conn.go:748-760` `writeTo` · 정품 녹음에서 다시 보낸 접속 수락이 키로 뒤집혀 있다).
    if (s.keyed) ApplyXor(o.data.data(), o.data.size(), s.xorKey);
    out.push_back(std::move(o));
    ++_stats.reliableResent;
    Timer t;
    t.dueMs = p.retryAt;
    t.sessionId = s.sessionId;
    t.kind = TimerKind::RESEND;
    t.seq = p.seq;
    PushTimer(t);
}

// 재시도 시각이 된 조각 — 아직 보관에 있고 재시도 시각이 이 항목과 같을 때만 다시 보낸다.
//   못 찾으면 확인이 와서 지웠거나(`Ack`) · 한도를 넘어 버렸거나 · 그 사이 다시 보내 새 항목이 걸린 것이다.
void CUdpServer::OnResendTimer(const Timer& t, std::vector<OutPacket>& out) {
    auto a = _addrOf.find(t.sessionId);
    if (a == _addrOf.end()) return;
    CUdpSession& s = _sessions.at(a->second);
    for (auto& p : s.reliableQueue)
        if (p.seq == t.seq && p.retryAt == t.dueMs) { ResendOne(s, p, out); return; }
}

// ★ 10-08 조각 4c — 게임에 한 번도 안 넘긴 상대(서버 찾기·접속 시도만 한 상대)는 여기서 걷어낸다.
//   전에는 게임 「자리 비우기」가 30초 뒤 걷어내며 `TIMEOUT_NOTIFICATION` 을 보내고 「조용해서 내보냄」을 찍었다.
//   게임에 넘긴 상대는 게임이 지운다(`RequestDisconnectSession`).
// ★ 10-09 조각 5a — 게임에 넘긴 상대가 한도 동안 조용하면 `DISCONNECTED`(TIMEOUT)를 한 번 넘긴다. 지우는 것은 게임
//   (그 사람에게 `TIMEOUT_NOTIFICATION` 을 보낸 뒤 — 원본 `server.go:445-451`). 옛 게임 「자리 비우기」의 판정을 옮겼다.
// ★ 10-09 조각 5b — 마감에 꺼낸다. 그 사이에 받았으면(마감이 밀렸으면) 새 마감으로 다시 건다.
void CUdpServer::OnIdleTimer(const Timer& t, std::vector<NetworkEvent>& events) {
    auto a = _addrOf.find(t.sessionId);
    if (a == _addrOf.end()) return;                                   // 지운 세션
    CUdpSession& s = _sessions.at(a->second);
    if (!s.idleArmed || s.idleDueMs != t.dueMs) return;              // 더 이른 마감이 따로 걸렸다 — 이 항목은 버림
    s.idleArmed = false;
    if (!IdleWatched(s)) return;
    const uint32_t due = s.lastSeenMs + IdleLimitOf(s) + 1;
    if (static_cast<int32_t>(_nowMs - due) < 0) { ArmIdle(s); return; }
    if (!s.announced) {
        _sessions.erase(a->second);
        _addrOf.erase(a);
        ++_stats.unannouncedSwept;
        return;
    }
    s.timeoutNotified = true;
    ++_stats.sessionTimedOut;
    NetworkEvent ev;
    ev.type = NetworkEvent::Type::DISCONNECTED;
    ev.reason = DisconnectReason::TIMEOUT;
    ev.sessionId = s.sessionId;
    ev.addr = s.addr;
    events.push_back(std::move(ev));
}

void CUdpServer::Tick(uint32_t nowMs, std::vector<OutPacket>& out, std::vector<NetworkEvent>& events) {
    if (!_clockSet) {
        // 시계를 처음 잡는다 — 그 전에 보낸 조각은 지금부터 2초를 센다.
        //   ★ 조각 4c — 그 전에 본 상대도 지금부터 센다 · ★ 조각 5b — 그 전에 건 타이머는 버리고 다시 건다.
        _clockSet = true;
        _nowMs = nowMs;
        _timers.clear();
        for (auto& kv : _sessions) {
            CUdpSession& s = kv.second;
            s.lastSeenMs = nowMs;
            s.idleArmed = false;
            ArmIdle(s);
            for (auto& p : s.reliableQueue) {
                p.retryAt = nowMs + kReliableRetryMs;
                Timer t;
                t.dueMs = p.retryAt;
                t.sessionId = s.sessionId;
                t.kind = TimerKind::RESEND;
                t.seq = p.seq;
                PushTimer(t);
            }
        }
    }
    _nowMs = nowMs;
    // ★ 10-09 조각 5b — 마감이 지난 타이머만 꺼낸다(옛 「1초마다 모든 세션 훑기」 — 원본 `MaybeSendReliable` `send_reliable.go:16-28` 꼴).
    //   꺼내며 새로 거는 마감은 늘 지금보다 뒤라 이 고리는 끝난다.
    while (!_timers.empty() && static_cast<int32_t>(nowMs - _timers.front().dueMs) >= 0) {
        std::pop_heap(_timers.begin(), _timers.end(), TimerLater);
        const Timer t = _timers.back();
        _timers.pop_back();
        switch (t.kind) {
            case TimerKind::IDLE:   OnIdleTimer(t, events); break;
            case TimerKind::RESEND: OnResendTimer(t, out); break;
        }
    }
}

}  // namespace nox
