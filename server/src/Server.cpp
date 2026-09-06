// Server.cpp — 접속 절차 세 자리에 답한다.
//
//   0x0C SERVER_DISCOVER  →  0x0D SERVER_INFO      (녹음 재생, 토큰만 바꿔서)
//   0x0E SERVER_TRY_JOIN  →  0x14 SERVER_JOIN_OK   (지금은 무조건 받아준다)
//   0x00 SERVER_CONNECT   →  0x1F ACCEPTED + 0x01 SERVER_ACCEPT (여기서 XOR 키를 준다)
//
// ★ 마지막 자리가 **한 조각에 메시지 둘**이다. 예전에 이 10바이트를 통째로
//   "무늬"로 외웠는데, 표대로 쪼개보니 두 메시지가 붙은 것이었다.
#include "nox/Server.h"

#include <cmath>
#include <cstring>

#include "nox/Messages.h"
#include "nox/Opcode.h"

namespace nox {

void Server::SetInfoTemplate(const uint8_t* body, size_t n) {
    infoBody_.assign(body, body + n);
}

void Server::SetJoinTemplate(const uint8_t* body, size_t n) {
    joinBody_.assign(body, body + n);
}

void Server::SetUseMapTemplate(const uint8_t* body, size_t n) {
    useMapBody_.assign(body, body + n);
}

namespace {

void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

Session& Server::SessionFor(const std::string& addr) {
    auto it = sessions_.find(addr);
    if (it != sessions_.end()) return it->second;
    Session s;
    s.addr = addr;
    return sessions_.emplace(addr, s).first->second;
}

void Server::Emit(Session& s, bool reliable, uint8_t stream,
                  const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    OutPacket p;
    p.to = s.addr;
    p.data.resize(kWireHeaderSize + n);
    WireHeader h;
    h.reliable = reliable;
    h.stream = stream;

    // 순번 규칙 — StreamSeq 주석 참조. 신뢰는 쓰고 올리고, 비신뢰는 직전 값을 싣는다.
    StreamSeq& q = s.seq[stream & 0x7F];
    if (reliable) {
        h.seq = q.next;
        q.last = q.next;
        ++q.next;
    } else {
        h.seq = q.last;
    }
    p.data[0] = h.Byte0();
    p.data[1] = h.seq;
    std::memcpy(p.data.data() + kWireHeaderSize, msg, n);

    // 키를 준 뒤로는 조각 전체가 뒤집힌다 — 머리도 포함이다.
    // (덤프의 `70 70 74` 가 XOR 0x70 을 풀면 `00 00 04` 인 것이 그 증거다)
    if (s.keyed) ApplyXor(p.data.data(), p.data.size(), s.xorKey);
    out.push_back(std::move(p));
}

size_t Server::OnPacket(const std::string& from, const uint8_t* data, size_t n,
                        std::vector<OutPacket>& out) {
    Session& s = SessionFor(from);
    ++s.recvPackets;

    // 받은 것이 암호문이면 먼저 푼다. 사본을 뜬다 — 부르는 쪽 버퍼를 건드리지 않는다.
    // ⚠ 덤프 재생일 때는 이미 평문이라 풀면 안 된다 (SetInboundPlain 주석 참조).
    std::vector<uint8_t> buf(data, data + n);
    if (s.keyed && !inboundPlain_) ApplyXor(buf.data(), buf.size(), s.xorKey);

    WireHeader head;
    if (!ReadWireHeader(buf.data(), buf.size(), head)) return 0;

    const uint8_t* body = buf.data() + kWireHeaderSize;
    const size_t bodyLen = buf.size() - kWireHeaderSize;

    // ★ 먼저 **끝까지 깨끗하게 쪼개지는지** 본다. 안 되면 아무 답도 만들지 않는다.
    //
    // 길이를 모르는 메시지를 만나면 그 뒤를 못 읽는데, 반쯤 읽은 채로 답을 만들면
    // **밀린 바이트에서 우연히 나온 오프코드에 답한다.** 실제로 그렇게 해서
    // 서버 찾기가 10건에서 75건으로 불어난 적이 있다.
    //
    // 「모르면 안 읽는다」가 「읽은 척한다」보다 낫다. 가변 메시지를 하나씩
    // 옮길수록 여기를 통과하는 조각이 늘어난다.
    {
        MessageIter probe(body, bodyLen, /*fromClient=*/true);
        while (probe.Next()) {}
        if (probe.stopped()) {
            ++stopped_;
            ++stoppedByOp_[static_cast<uint8_t>(probe.stopOp())];
            return 0;
        }
    }

    MessageIter it(body, bodyLen, /*fromClient=*/true);
    size_t count = 0;
    while (it.Next()) {
        ++count;
        ++seen_[it.op()];

        switch (static_cast<Op>(it.op())) {
            // 핑에 그대로 되돌린다. 값은 클라의 시각이라 **손대지 않는다.**
            // 실측 = `01 04 06 <4바이트>` 에 `01 00 08 <같은 4바이트>` 로 답했다.
            // 스트림은 받은 것을 그대로 쓴다(핑·퐁은 스트림 1 을 쓰더라).
            case Op::CLIENT_PING: {
                if (it.bodyLen() != 4) break;
                uint8_t msg[5];
                msg[0] = static_cast<uint8_t>(Op::CLIENT_PONG);
                std::memcpy(msg + 1, it.body(), 4);
                Emit(s, /*reliable=*/false, head.stream, msg, sizeof(msg), out);
                ++handled_;
                break;
            }

            // ★ 클라 입력 — 매 틱 온다. 트래픽에서 가장 잦은 메시지다(32,450건).
            //
            //   본문 = 길이 1B + [코드 4B + 데이터]*
            //   코드는 4바이트를 먹고 **첫 바이트가 무엇을 눌렀나**를 가리킨다.
            //   실측 `3f 0a | 01 00 00 00 63 | 01 00 00 00 63` — 방향(1) 두 번, 값 0x63.
            //
            // 데이터 크기 (noxnet/msg_player_input.go 의 `CtrlCode.DataSize()`):
            //   방향(1)·앞뒤좌우(2~5)·주문끝(28) = 1바이트,
            //   주문시전(29 CastQueuedSpell·30 CastMostRecentSpell) = 4바이트, 그 밖 0.
            //
            // ⚠ 2026-09-07 정정 — 여기 `19·21·22` 라고 적혀 있었다. **옮겨 적기 오류였다.**
            //   19=Inventory·21=SpellGestureDown·22=SpellGestureLeft 는 전부 크기 0 이고,
            //   1바이트짜리는 28, 4바이트짜리는 29·30 이다.
            //   ⚠ 09-06 캡처로는 못 가린다 — 그 여섯 코드가 **한 건도 안 잡혔다.**
            //   근거는 opennox 소스뿐이니, 주문 몸짓·시전을 실제로 담은 캡처로 다시 확인할 것.
            //
            // ⚠ 아직 못 읽는 자리 — 코드 4바이트의 **위 3바이트**. 서버 덤프에서 방향(1) 항목의
            //   36.8%(54,254건)가 셋째·넷째 바이트에 0 아닌 값을 담고 있다.
            //   opennox 도 첫 바이트만 읽고 버린다. 무엇인지 미상 (`tools/dumpscan -inputs`).
            case Op::PLAYER_INPUT: {
                if (!s.inGame || it.bodyLen() < 1) break;
                const uint8_t* q = it.body() + 1;
                size_t qn = it.bodyLen() - 1;
                if (qn > static_cast<size_t>(it.body()[0])) qn = it.body()[0];
                while (qn >= 4) {
                    const uint8_t code = q[0];
                    q += 4; qn -= 4;
                    size_t dataLen = 0;
                    switch (code) {
                        case 1: case 2: case 3: case 4: case 5: case 28: dataLen = 1; break;
                        case 29: case 30: dataLen = 4; break;
                        default: dataLen = 0; break;
                    }
                    if (qn < dataLen) break;
                    if (code == 1 && dataLen == 1) {
                        s.facing = q[0];          // 바라보는 쪽
                    } else if (code >= 2 && code <= 5 && dataLen == 1) {
                        // 걸음 — 바라보는 쪽을 기준으로 민다.
                        // 방향은 한 바퀴를 256으로 나눈 값이다(1바이트).
                        // 속도는 아직 실측 못 했다. 눈으로 확인할 만한 값으로 둔다.
                        int dir = s.facing;
                        if (code == 3) dir += 128;        // 뒤
                        else if (code == 4) dir += 192;   // 왼
                        else if (code == 5) dir += 64;    // 오른
                        const double rad = (dir & 0xFF) * 3.14159265358979 / 128.0;
                        s.x += static_cast<int>(kStepPerTick * std::cos(rad));
                        s.y += static_cast<int>(kStepPerTick * std::sin(rad));
                        if (s.x < 0) s.x = 0;
                        if (s.y < 0) s.y = 0;
                        if (s.x > kMaxCoord) s.x = kMaxCoord;
                        if (s.y > kMaxCoord) s.y = kMaxCoord;
                        ++moves_;
                    }
                    q += dataLen; qn -= dataLen;
                }
                break;
            }

            // ★ 주문 시전 — 2026-09-06 캡처로 짜임을 확정했다(표본 83개 전수).
            //
            //   79 │ 주문번호 1 │ 0 으로 채운 19 │ 대상 플래그 1
            //
            // 주문번호는 opennox `spell` 패키지 ID 와 그대로 같다 (0x1B = 27 = 파이어볼).
            // ⚠ **겨눈 쪽이 여기 없다** — 가운데 19바이트가 표본 전수에서 0 이었다.
            //    대상도 좌표도 안 실린다. 겨눈 쪽은 따로 오는 마우스(0xAC)와
            //    바라보는 쪽(PLAYER_INPUT 코드 1)에 있다.
            // 마지막 자리는 주문마다 갈렸다 — 남에게 쏘는 것(파이어볼·연쇄번개)이 0,
            // 자기에게 거는 것(점멸·치유·보호막)이 1. 보호막만 18건 중 1건이 0 이라
            // 「자기 대상 여부」로 단정하지는 않는다.
            //
            // 지금은 받아서 세는 데까지다. **효과를 되쏘는 것은 아직 못 한다** —
            // 효과 메시지(0x80 계열)의 짜임을 아직 모른다.
            case Op::TRY_SPELL: {
                if (!s.inGame || it.bodyLen() != kTrySpellBodyLen) break;
                ++spells_[it.body()[0]];
                if (it.body()[kTrySpellBodyLen - 1]) ++spellsOnSelf_;
                break;
            }

            case Op::SERVER_DISCOVER: {
                // 이미 붙은 상대가 다시 찾을 리 없다. 상태를 지켜야 밀린 바이트에
                // 속지 않는다.
                if (s.keyed) break;
                if (infoBody_.empty()) break;
                MsgServerDiscover req;
                Reader r(it.body(), it.bodyLen());
                if (!req.Decode(r)) break;

                // 원본을 그대로 두고 토큰 자리만 바꾼다.
                std::vector<uint8_t> msg;
                msg.reserve(1 + infoBody_.size());
                msg.push_back(static_cast<uint8_t>(Op::SERVER_INFO));
                msg.insert(msg.end(), infoBody_.begin(), infoBody_.end());
                if (infoBody_.size() >= kInfoTokenOffset + 4) {
                    uint8_t* t = msg.data() + 1 + kInfoTokenOffset;
                    t[0] = static_cast<uint8_t>(req.token);
                    t[1] = static_cast<uint8_t>(req.token >> 8);
                    t[2] = static_cast<uint8_t>(req.token >> 16);
                    t[3] = static_cast<uint8_t>(req.token >> 24);
                }
                Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
                ++handled_;
                break;
            }

            case Op::SERVER_TRY_JOIN: {
                if (s.keyed) break;
                // 지금은 조건 없이 받아준다. 거절 사유(중복 일련번호·인원 참·버전)는
                // 세션 관리를 만들 때 붙인다.
                const uint8_t msg[1] = {static_cast<uint8_t>(Op::SERVER_JOIN_OK)};
                Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
                ++handled_;
                break;
            }

            case Op::SERVER_CONNECT: {
                // 이미 키를 준 상대가 다시 보내면 재전송이다. 번호를 새로 매기지 않는다.
                if (s.keyed) break;
                if (scriptAt_ < script_.size()) {
                    // 덤프 재생 — 그때 서버가 정한 값들을 그대로 쓴다.
                    const AcceptScript& a = script_[scriptAt_++];
                    s.connectId = a.connectId;
                    s.xorKey = a.xorKey;
                    s.frame = a.frame;
                    s.netCode = a.netCode;
                } else {
                    s.connectId = nextConnectId_++;
                    s.xorKey = xorKey_;
                    s.frame = frame_;
                    s.netCode = netCode_;
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
                Emit(s, /*reliable=*/true, /*stream=*/0, msg, k, out);
                s.keyed = true;   // 다음 조각부터 뒤집는다
                ++handled_;
                break;
            }

            // 클라가 「받았다」고 알려오면, 서버가 게임에 필요한 것을 한꺼번에 내려준다.
            // 216바이트 묶음(메시지 여섯) → USE_MAP 순서다. 순서를 바꾸면 순번이 어긋난다.
            case Op::CLIENT_ACCEPT: {
                if (!joinBody_.empty()) {
                    std::vector<uint8_t> msg = joinBody_;
                    if (msg.size() > kJoinNetCodeB + 1) {
                        PutU32(msg.data() + kJoinFrameA, s.frame);
                        PutU32(msg.data() + kJoinFrameB, s.frame);
                        PutU16(msg.data() + kJoinNetCodeA, s.netCode);
                        PutU16(msg.data() + kJoinNetCodeB, s.netCode);
                    }
                    // ★ 플레이어 정보는 지어내지 않는다 — 방금 받은 것을 그대로 옮긴다.
                    if (msg.size() >= kJoinPlayerInfo + kJoinPlayerInfoLen &&
                        it.bodyLen() >= kJoinPlayerInfoLen) {
                        std::memcpy(msg.data() + kJoinPlayerInfo, it.body(),
                                    kJoinPlayerInfoLen);
                    }
                    Emit(s, /*reliable=*/true, /*stream=*/0, msg.data(), msg.size(), out);
                    ++handled_;
                }
                // 여기서부터 게임에 들어온 것으로 본다 — 매 틱 상태를 받는다.
                s.inGame = true;
                if (s.x == 0 && s.y == 0) { s.x = 1000; s.y = 1000; }
                if (s.netCode == 0) s.netCode = 713;

                if (!useMapBody_.empty()) {
                    std::vector<uint8_t> msg = useMapBody_;
                    if (msg.size() > kUseMapFrame + 3)
                        PutU32(msg.data() + kUseMapFrame, s.frame);
                    Emit(s, /*reliable=*/true, /*stream=*/0, msg.data(), msg.size(), out);
                    ++handled_;
                }
                break;
            }

            default:
                break;   // 아직 답하지 않는다. 세기만 했다.
        }
    }
    // 여기까지 왔으면 앞의 훑기에서 이미 끝까지 읽혔다. 다시 셀 것이 없다.
    return count;
}

// ── 틱 ───────────────────────────────────────────────────
//
// 매 틱 나가는 것 = `0x27 TIMESTAMP`(프레임) + `0xa4 UPDATE_STREAM`(누가 어디 있나).
// 실측에서도 이 둘이 거의 1:1 로 붙어 나갔다(30,097 대 30,087).
//
// ★ 상태 갱신의 짜임은 왕복 검사로 굳혀 두었다 — 덤프 30,087건을 뜯어 다시 짜면
//   원본과 바이트가 같다(nox_verify). 그러니 여기서는 **무엇을 담을지**만 정하면 된다.
int Server::Tick(uint32_t nowMs, std::vector<OutPacket>& out) {
    if (!tickStarted_) {
        tickStarted_ = true;
        lastTickMs_ = nowMs;
        return 0;
    }
    if (nowMs < lastTickMs_) lastTickMs_ = nowMs;   // 시계가 되감겼다
    int ticks = 0;
    while (nowMs - lastTickMs_ >= tickMs_) {
        lastTickMs_ += tickMs_;
        ++frame_;
        ++ticks;
    }
    if (ticks == 0) return 0;

    // 게임에 들어와 있는 사람에게만 보낸다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;

        // ① 시각 — 본문 2바이트(LE16). 클라는 이 값으로 프레임을 맞춘다.
        {
            uint8_t msg[3];
            msg[0] = static_cast<uint8_t>(Op::TIMESTAMP);
            msg[1] = static_cast<uint8_t>(frame_);
            msg[2] = static_cast<uint8_t>(frame_ >> 8);
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }

        // ② 상태 갱신 — 머리는 「보는 사람」, 항목에는 **게임에 있는 모두**를 싣는다.
        //    이것이 M 의 「서로를 본다」다. 관심영역(멀리 있는 것은 빼기)은 나중에.
        UpdateMsg m;
        m.headAlias = 0xff;                  // 머리는 전체 번호로
        m.headNetCode = s.netCode;
        m.headType = kPlayerType;
        m.x = s.x;
        m.y = s.y;
        m.flags = 0;
        m.headAnim = 0;

        for (auto& kv2 : sessions_) {
            Session& o = kv2.second;
            if (!o.inGame) continue;

            ObjEntry e;
            // 별칭은 **클라와 똑같은 규칙**으로 잡는다. 한 칸만 어긋나면
            // 그때부터 클라가 엉뚱한 것을 그린다(Alias.h 참조).
            const int alias = s.aliases.Assign(o.netCode, kPlayerType, frame_);
            if (alias > 0) {
                e.alias = static_cast<uint8_t>(alias);
            } else {
                e.alias = 0xff;              // 자리가 없으면 전체 번호로
                e.netCode = o.netCode;
            }
            e.type = kPlayerType;
            e.absolute = true;               // 항목마다 절대 좌표 — 앞의 것에 안 얽힌다
            e.x = o.x;
            e.y = o.y;
            e.extra = 1;                     // 플레이어는 복합이라 방향·애니 1바이트
            e.extraBytes[0] = o.facing;
            m.objects.push_back(e);
        }

        uint8_t body[512];
        body[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        const size_t n = EncodeUpdate(m, body + 1, sizeof(body) - 1);
        if (n) Emit(s, /*reliable=*/false, /*stream=*/0, body, 1 + n, out);
    }
    return ticks;
}

}  // namespace nox
