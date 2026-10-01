// Server.cpp — 접속 절차 세 자리에 답한다.
//
//   0x0C SERVER_DISCOVER  →  0x0D SERVER_INFO      (녹음 재생, 토큰만 바꿔서)
//   0x0E SERVER_TRY_JOIN  →  0x14 SERVER_JOIN_OK   (지금은 무조건 받아준다)
//   0x00 SERVER_CONNECT   →  0x1F ACCEPTED + 0x01 SERVER_ACCEPT (여기서 XOR 키를 준다)
//
// ★ 마지막 자리가 **한 조각에 메시지 둘**이다. 예전에 이 10바이트를 통째로
//   "무늬"로 외웠는데, 표대로 쪼개보니 두 메시지가 붙은 것이었다.
#include "nox/Server.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nox/Messages.h"
#include "nox/Opcode.h"
#include "nox/ThingFlags.h"
#include "nox/JoinBundle.h"
#include "nox/SpellNames.h"
#include "nox/DirVectors.h"

namespace nox {

// 정의는 아래 틱 앞에 있다. 접속 처리(첫 세계 스냅샷)에서 먼저 부르므로 여기서 알린다.
static bool PatchPreludePlayer(const std::vector<uint8_t>& frag, uint16_t netCode,
                               int x, int y, std::vector<uint8_t>& out,
                               bool forceHudOn = false);

// ★★ 첫 세계 스냅샷을 받아 두고, **그 머리 좌표를 스폰 자리로 삼는다.**
//   서두가 찍힌 맵의 그 자리라야 사람이 스냅샷 속 다른 오브젝트들과 같은 동네에 선다.
void Server::SetFirstSnapshot(const std::vector<uint8_t>& v) {
    firstSnapshot_ = v;
    // 조각 안을 훑어 `0xa4` 를 찾는다. 앞에 `0x27 TIMESTAMP` 같은 게 붙어 있다.
    size_t pos = 0;
    while (pos < firstSnapshot_.size()) {
        const uint8_t op = firstSnapshot_[pos];
        if (op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const UpdateMsg um =
                DecodeUpdate(firstSnapshot_.data() + pos + 1, firstSnapshot_.size() - pos - 1, nullptr);
            // ⚠ 09-22 — **`--spawn-at` 으로 못 박았으면 덮어쓰지 않는다.**
            //   전에는 녹음 조각의 자리가 이겨서 그 스위치가 **조용히 무시됐다**
            //   (「손으로 지정 — (2930,2668)」이라 찍어 놓고 실제로는 2782,3504 에 섰다)
            if (um.ok && um.consumed > 0 && !spawnForced_) { spawnX_ = um.x; spawnY_ = um.y; }
            return;
        }
        const size_t msz = MessageSize(firstSnapshot_.data() + pos, firstSnapshot_.size() - pos,
                                       /*fromClient=*/false);
        if (msz == 0) return;            // 모르는 것 — 기본값을 쓴다
        pos += msz;
    }
}

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

// ★ 09-28 진단 — 환경변수 `NOX_TRACE_OUT=<파일>` 이면 **받는 사람별로 나가는 메시지**를 한 줄씩 적는다.
//   가르려는 것 — 「맨몸인데 남 화면엔 로브」(겉모습 알림이 무엇이 언제 갔나) · 「남이 접속·끊길 때 내 화면이 검게 깜빡」
//   (그때 나에게 무엇이 갔나). 매 틱 나가는 시야 갱신(a4)·시각(27·28)은 뺀다. `cc`(순번 싼 것)는 속을 풀어 적는다.
//   켜지 않으면 아무것도 안 한다(파일을 여는 것도 첫 호출 한 번)
static FILE* TraceOutFile() {
    static FILE* f = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char* p = std::getenv("NOX_TRACE_OUT");
        if (p && *p) f = std::fopen(p, "a");
    }
    return f;
}
static void TraceOutMsgs(FILE* f, const std::string& to, uint32_t frame, const uint8_t* m, size_t n, int depth) {
    size_t pos = 0;
    while (pos < n) {
        const uint8_t op = m[pos];
        const size_t sz = MessageSize(m + pos, n - pos, /*fromClient=*/false);
        if (op == static_cast<uint8_t>(Op::SEQ_IMPORTANT) && pos + 4 <= n) {   // cc 번호2 길이1 속…
            const size_t inner = m[pos + 3];
            if (depth < 2 && pos + 4 + inner <= n) TraceOutMsgs(f, to, frame, m + pos + 4, inner, depth + 1);
            pos += 4 + inner;
            continue;
        }
        if (sz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {   // a4 는 가변 — RenumberSeq 와 같이 풀어 잰다
            const UpdateMsg um = DecodeUpdate(m + pos + 1, n - pos - 1, nullptr);
            if (um.ok && um.consumed > 0) { pos += 1 + um.consumed; continue; }
        }
        if (sz == 0) { std::fprintf(f, "%u\t%s\t?? %02x (남은 %zu)\n", frame, to.c_str(), op, n - pos); return; }
        if (op != static_cast<uint8_t>(Op::UPDATE_STREAM) && op != 0x27 && op != 0x28) {
            std::fprintf(f, "%u\t%s\t%s", frame, to.c_str(), OpName(static_cast<Op>(op)));
            for (size_t i = 0; i < sz && i < 12; ++i) std::fprintf(f, " %02x", m[pos + i]);
            std::fprintf(f, "\n");
        }
        pos += sz;
    }
}

// ★ 09-30 한 틱 한 데이터그램 — 정답지는 원격 사람에게 **프레임마다 조각 하나**만 보낸다(`server.go:474~475` `FlushAndPoll` →
//   `network_lists.go:177~227` `sendPoll`): 시각 `27` → 갱신 `a4` → Kind1(`ab` 중요 확인 · 소리 등 넣은 차례) → `aa`+중요.
//   `ab` 도 받는 자리에서 곧바로 안 보내고 Kind1 에 넣는다(`server/network.go:145~155`).
//   ✖ 우리는 이것들을 따로 보냈다 — 09-29 같은 자동 클라 2분 대조 `ab` 7,871 · `27` 2,772 · `a4` 2,761 ↔ 대조군 2,522(평균 29.8B)
//   그래서 비신뢰 · 스트림 0 · 게임 메시지(첫 번호 ≥ 0x20) · 게임 안 사람이면 그 사람 묶음 칸에 담고, 틱 끝 `FlushBundles` 가 낸다.
//   나머지 갈래는 곧바로(`EmitNow`) — 갈래마다 따로 센다(`BundleStat::Direct*`)
void Server::Emit(Session& s, bool reliable, uint8_t stream,
                  const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    if (reliable || n == 0) { EmitNow(s, reliable, stream, msg, n, out); return; }
    BundleStat why;
    if ((stream & 0x7F) != 0)      why = BundleStat::DirectStream;
    else if (msg[0] < 0x20)        why = BundleStat::DirectCtrl;
    else if (!s.inGame)            why = BundleStat::DirectNotInGame;
    else if (s.importantImmediate) why = BundleStat::DirectTest;
    else {
        std::vector<uint8_t>* slot = &s.bundleMisc;
        BundleStat in = BundleStat::InMisc;
        if (msg[0] == static_cast<uint8_t>(Op::TIMESTAMP))          { slot = &s.bundleTs;  in = BundleStat::InTs; }
        else if (msg[0] == static_cast<uint8_t>(Op::UPDATE_STREAM)) { slot = &s.bundleUpd; in = BundleStat::InUpd; }
        slot->insert(slot->end(), msg, msg + n);
        if (slot == &s.bundleMisc) s.bundleMiscEnds.push_back(static_cast<uint32_t>(s.bundleMisc.size()));
        ++bundleStat_[static_cast<size_t>(in)];
        return;
    }
    ++bundleStat_[static_cast<size_t>(why)];
    EmitNow(s, reliable, stream, msg, n, out);
}

// 틱 끝 — 사람마다 묶음 칸 넷을 정답지 차례로 한 조각에(되받기 값·XOR 은 `EmitNow` 가 조각마다 한 번). 칸이 다 비었으면 안 낸다.
// ★ 09-30 조각 2 — 몸통 한도 2048(`kBundleMaxPayload`)을 넘으면 **그 틱 안에서 끊는다** — 조각마다 같은 `27` 로 시작하고
//   단위(갱신 하나 · 잡다 메시지 하나하나 · 중요 칸 하나)를 차례로 채우다 안 들어가면 새 조각을 연다.
//   ⚖ 정답지는 넘친 Kind1 을 **버린다**(`netlist.go:63~75`) — 우리는 버리지 않는다(`ab` 가 사라지면 클라 재전송이 는다 ·
//     정품도 버리는지는 확인 불가 · 플랜 「할 일 3」) · 갱신이 넘칠 때 새 조각을 `27` 부터 다시 쓰는 것은 정답지 `AddToMsgListSrv`(`netlist.go:233~266`)와 같은 꼴
//   한 단위가 `27` 과 합쳐 혼자 한도를 넘으면 메시지는 못 가르므로 그대로 낸다(`OverSize`)
void Server::FlushBundles(std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        const size_t total = s.bundleTs.size() + s.bundleUpd.size() + s.bundleMisc.size() + s.bundleImp.size();
        if (total == 0) continue;
        // 단위 — (시작, 길이) 를 칸 차례로
        struct Unit { const uint8_t* p; size_t n; };
        std::vector<Unit> units;
        if (!s.bundleUpd.empty()) units.push_back({ s.bundleUpd.data(), s.bundleUpd.size() });
        uint32_t from = 0;
        for (uint32_t end : s.bundleMiscEnds) { units.push_back({ s.bundleMisc.data() + from, end - from }); from = end; }
        if (!s.bundleImp.empty()) units.push_back({ s.bundleImp.data(), s.bundleImp.size() });
        std::vector<uint8_t> pkt;
        pkt.reserve(total < kBundleMaxPayload ? total : kBundleMaxPayload);
        pkt.insert(pkt.end(), s.bundleTs.begin(), s.bundleTs.end());
        const size_t prefix = pkt.size();
        int sent = 0;
        auto emit = [&]() {
            if (pkt.size() > kBundleMaxPayload) ++bundleStat_[static_cast<size_t>(BundleStat::OverSize)];
            EmitNow(s, /*reliable=*/false, /*stream=*/0, pkt.data(), pkt.size(), out);
            ++bundleStat_[static_cast<size_t>(BundleStat::Datagrams)];
            ++sent;
        };
        for (const Unit& u : units) {
            if (pkt.size() > prefix && pkt.size() + u.n > kBundleMaxPayload) {
                emit();
                pkt.resize(prefix);                            // 다음 조각도 같은 `27` 로
            }
            pkt.insert(pkt.end(), u.p, u.p + u.n);
        }
        if (pkt.size() > prefix || sent == 0) emit();          // 남은 것 · 시각만 있는 틱도 한 조각
        if (sent > 1) {
            ++bundleStat_[static_cast<size_t>(BundleStat::Overflow)];
            bundleStat_[static_cast<size_t>(BundleStat::OverExtra)] += static_cast<unsigned long long>(sent - 1);
        }
        s.bundleTs.clear();
        s.bundleUpd.clear();
        s.bundleMisc.clear();
        s.bundleMiscEnds.clear();
        s.bundleImp.clear();
    }
}

void Server::EmitNow(Session& s, bool reliable, uint8_t stream,
                     const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    if (FILE* tf = TraceOutFile()) { TraceOutMsgs(tf, s.addr, frame_, msg, n, 0); std::fflush(tf); }
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
        if (st < 8) { if (reliable) ++sendRelStream_[st]; else ++sendPlainStream_[st]; }
    }
    if (reliable) {
        h.seq = q.next;
        q.last = q.next;
        ++q.next;
    } else if (ackRecvSeq_ && ackAnyStream_ && (stream & 0x7F) == 0 &&
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
        if (ackSeqApplied_ != 0 && ack != ackLastSent_) ++ackSeqChanged_;
        ackLastSent_ = ack;
        ++ackSeqApplied_;
        if (ack < 32) ++ackValueHist_[ack];
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
        if (ack != q.last) ++ackSeqDiffer_;
        ++ackSeqApplied_;
        // ★ 2026-09-18 — **보낸 ack 값 자체를 센다.** 클라 쪽 로그에 0·2·3 만 찍히고
        //   4 가 안 보여서, 「우리가 4 를 안 보내나」와 「보내는데 클라가 안 읽나」를 갈라야 한다.
        //   값이 한두 개뿐이면 `recvSeq` 가 안 올라가는 것이다.
        if (ack < 32) ++ackValueHist_[ack];
        h.seq = ack;
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

// `0xcc SEQ_IMPORTANT` 로 싸서 내보낸다 — 머리 `cc | 번호 2 | 속길이 1` 뒤에 속 메시지.
//
// 실측 원문: `cc 20 00 07 | 6a cb 02 00 00 00 00`  (속 7바이트짜리 REPORT_CLIENT_STATUS)
void Server::EmitNewPlayer(Session& to, const Session& who, uint8_t status,
                           std::vector<OutPacket>& out) {
    // ★★ **녹음이 없으면 조립한다** (2026-09-10 감사에서 잡음).
    //   전에는 여기서 그냥 되돌아갔다. 독립 모드에는 `joinBody_` 가 비어 있어
    //   **남을 소개하는 `2d` 가 한 번도 안 나갔다** — 접속 묶음 안 자기 소개 하나뿐이었다.
    //   소개가 없으면 클라에 그 사람의 playerInfo 가 없어 이름·외모가 안 서고,
    //   상태 알림(`6a`)도 빈손이면 되돌아간다(09-09 기록).
    std::vector<uint8_t> np;
    // ★ 09-28 사람 판 — 「남에게는 모두 컨저러·Jack 색으로 소개된다」. 정답지는 **그 사람의 플레이어 정보 97B** 를 싣는다
    //   (`nox_xxx_netNewPlayerMakePacket_4DDA90` `player.go:240` `*pl.Info()` — 이름·직업·여성·머리/피부/옷 색).
    //   우리는 녹음 `2d` 를 그대로 써서 정보 자리가 녹음 속 Jack(컨저러 0x02)이었다. 자기 접속 묶음은 이미 받은 97B 로 덮는다(`:3591~3596`) —
    //   남을 소개할 때도 같게 덮는다. 97B 가 없는 사람(시험의 `DebugEnterGame`)만 녹음 그대로 간다
    const bool realInfo = who.info97.size() >= kJoinPlayerInfoLen;
    if (joinBody_.size() >= kJoinNewPlayerAt + kJoinNewPlayerLen) {
        np.assign(joinBody_.begin() + kJoinNewPlayerAt,
                  joinBody_.begin() + kJoinNewPlayerAt + kJoinNewPlayerLen);
        if (realInfo && np.size() >= 3 + kJoinPlayerInfoLen) {
            std::memcpy(np.data() + 3, who.info97.data(), kJoinPlayerInfoLen);
            ++newPlayerInfo_[1];
        } else {
            ++newPlayerInfo_[0];
        }
    } else if (realInfo) {
        np = BuildNewPlayer(who.netCode, who.info97.data());
        ++newPlayerInfo_[2];
    } else {
        return;   // 재료가 없다 — 지어내지 않는다
    }
    PutU16(np.data() + 1, who.netCode);
    // ★★★ **이름을 사람마다 갈라 준다.** 서두가 녹음본이라 모두가 같은 "Jack" 이었다 —
    //   이름·외모가 똑같으니 클라가 **같은 사람으로 보고 하나만 그렸을** 수 있다.
    //   (2026-09-09: 자리·좌표 모드·덧붙이·간격을 다 기각하고 남은 후보가 「누구냐」였다.)
    //   이름은 3번 자리부터 UTF-16LE 다 — 실측 `2d <번호2> 4a 00 61 00 63 00 6b 00`("Jack").
    //   첫 글자만 번호에 따라 바꾼다. 길이·짜임은 그대로라 안전하다.
    //   ★ 09-28 — **녹음 이름일 때만** 바꾼다. 진짜 정보를 덮었으면 그 사람 이름 그대로다(정답지도 안 바꾼다)
    if (!realInfo && np.size() > 4) np[3] = static_cast<uint8_t>('A' + (who.netCode % 26));
    // ★ 상태는 4바이트지만 우리가 쓰는 값은 하위 한 바이트뿐이다(0x423 안에 든다).
    if (np.size() > kNewPlayerStatusAt + 3) {
        np[kNewPlayerStatusAt] = status;
        np[kNewPlayerStatusAt + 1] = 0;
        np[kNewPlayerStatusAt + 2] = 0;
        np[kNewPlayerStatusAt + 3] = 0;
    }
    // ★★ 09-28 사람 판 — 「평상복을 입었는데 남 화면엔 맨몸」. `2d` 의 104·108 자리는 **갑옷·무기 비트**이고,
    //   받는 클라는 그 값으로 그 사람 겉모습을 **덮어쓴다**(`network_client.go:315~316` `pl.ArmorEquip = …` — `|=` 아님).
    //   정답지 서버는 **지금 입은 것**을 싣는다(`player.go:233~234` `nox_xxx_netNewPlayerMakePacket_4DDA90`).
    //   우리는 녹음(컨저러 Jack · 둘 다 0)을 그대로 보내, 누가 들어올 때마다 모두를 다시 소개하면서 **남의 차림을 0 으로 지웠다**
    if (np.size() >= 112) {
        PutU32(np.data() + 104, ArmorEquipOf(who));
        PutU32(np.data() + 108, WeaponEquipOf(who));
        ++newPlayerGearBits_;
    }
    EmitSeq(to, np.data(), np.size(), out);
    ++handled_;
}

// `6a REPORT_CLIENT_STATUS <번호2> <상태4>` 한 통. 받는 쪽 한 명에게.
//
// 실측 원문: `cc 20 00 07 | 6a cb 02 00 00 00 00`
// 녹음본 조각의 `cc` 번호를 이 세션 것으로 다시 매긴다. 자세한 까닭은 Server.h 선언 자리에.
void Server::RenumberSeq(Session& s, std::vector<uint8_t>& frag) {
    size_t pos = 0;
    while (pos < frag.size()) {
        const uint8_t op = frag[pos];
        size_t msz = MessageSize(frag.data() + pos, frag.size() - pos, /*fromClient=*/false);
        // ⚠ `a4` 는 길이표에 없다(가변). 이걸 안 다루면 걸음이 **첫 갱신에서 멈춰**
        //   뒤의 `cc` 를 하나도 못 고친다 — 09-09 에 실제로 그렇게 헛돌았다.
        if (msz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const UpdateMsg um = DecodeUpdate(frag.data() + pos + 1, frag.size() - pos - 1, nullptr);
            if (um.ok && um.consumed > 0) msz = 1 + um.consumed;
        }
        if (msz == 0) break;   // 모르는 것 — 여기서 멈춘다. 뒤를 억지로 고치면 더 나쁘다
        if (op == static_cast<uint8_t>(Op::SEQ_IMPORTANT) && msz >= 4) {
            frag[pos + 1] = static_cast<uint8_t>(s.seqImpId);
            frag[pos + 2] = static_cast<uint8_t>(s.seqImpId >> 8);
            ++s.seqImpId;
        }
        pos += msz;
    }
}

void Server::EmitClientStatus(Session& to, uint16_t netCode, uint32_t status,
                              std::vector<OutPacket>& out) {
    const uint8_t st[7] = {
        static_cast<uint8_t>(Op::REPORT_CLIENT_STATUS),
        static_cast<uint8_t>(netCode),
        static_cast<uint8_t>(netCode >> 8),
        static_cast<uint8_t>(status),
        static_cast<uint8_t>(status >> 8),
        static_cast<uint8_t>(status >> 16),
        static_cast<uint8_t>(status >> 24) };
    EmitSeq(to, st, sizeof(st), out);
    ++handled_;
}

void Server::BroadcastClientStatus(uint16_t netCode, uint32_t status,
                                   std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        EmitClientStatus(o, netCode, status, out);
    }
}

void Server::Broadcast(bool viaSeq, const uint8_t* msg, size_t n,
                       std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        if (viaSeq) EmitSeq(o, msg, n, out);
        else        Emit(o, /*reliable=*/false, /*stream=*/0, msg, n, out);
    }
}

void Server::EmitSeq(Session& s, const uint8_t* msg, size_t n,
                     std::vector<OutPacket>& out) {
    if (n == 0 || n > 0xFF) return;
    std::vector<uint8_t> buf;
    buf.reserve(4 + n);
    buf.push_back(static_cast<uint8_t>(Op::SEQ_IMPORTANT));
    buf.push_back(static_cast<uint8_t>(s.seqImpId));
    buf.push_back(static_cast<uint8_t>(s.seqImpId >> 8));
    buf.push_back(static_cast<uint8_t>(n));
    buf.insert(buf.end(), msg, msg + n);
    ++s.seqImpId;
    if (s.importantImmediate) {   // 시험용 — Session::importantImmediate 주석
        // 껍데기가 신뢰를 맡으므로 패킷 자체는 비신뢰다 (Server.h 의 EmitSeq 주석 참조).
        Emit(s, /*reliable=*/false, /*stream=*/0, buf.data(), buf.size(), out);
        return;
    }
    // ★ 바로 안 보낸다 — 대기열에 쌓고 FlushImportant 가 준비 뒤에 싣고, 확인이 올 때까지 다시 싣는다.
    //   (Session::importantQ 주석 — 한 번만 보내던 것이 접속 직후 순번 구멍을 만들어 뒤 cc 가 전부 막혔다)
    if (s.importantQ.size() >= kImportantQueueMax) {
        // 정답지는 넘치면 그 사람을 끊는다(`nox_xxx_importantCheckRate_4E52B0` → playerKickDueToRate).
        // 여기서는 끊지 않고 알리기만 한다 — 끊기 경로는 아직 옮기지 않았다.
        std::printf("[중요] %s 대기열 %zu 개로 넘침 — 새 순번 %u 버림\n", s.addr.c_str(),
                    s.importantQ.size(), static_cast<unsigned>(s.seqImpId - 1));
        return;
    }
    Session::PendingImportant p;
    p.wire = std::move(buf);
    s.importantQ.push_back(std::move(p));
}

// 중요 대기열에 **봉투 없이** 싣는다 — 정답지 `nox_xxx_netSendPacket0_4E5420`(GAME3_3.c:1762, 마지막 인자 0).
//   순번이 없으니 다시 실으면 클라가 **또 처리한다**(정답지도 같다). 사라짐·그늘처럼 두 번 받아도 같은 것만 쓴다.
void Server::EmitImportantRaw(Session& s, const uint8_t* msg, size_t n,
                              std::vector<OutPacket>& out) {
    if (n == 0 || n > 0xFF) return;
    if (s.importantImmediate) {   // 시험용 — Session::importantImmediate 주석
        Emit(s, /*reliable=*/false, /*stream=*/0, msg, n, out);
        return;
    }
    if (s.importantQ.size() >= kImportantQueueMax) {
        std::printf("[중요] %s 대기열 %zu 개로 넘침 — 맨 메시지 0x%02x 버림\n", s.addr.c_str(),
                    s.importantQ.size(), msg[0]);
        return;
    }
    Session::PendingImportant p;
    p.wire.assign(msg, msg + n);
    s.importantQ.push_back(std::move(p));
}

// 중요 메시지 한 조각 — 정답지 `nox_xxx_netImportant_4E5770`(GAME3_3.c:1938)를 옮겼다.
//   ① 준비 전(CLIENT_READY 전)이면 아무것도 안 싣는다(1966줄 `v3[3680] & 0x10`).
//   ② 처음 싣는 것은 다 싣는다 · 이미 실은 것은 기다림(+132)이 끝났을 때 **틱당 1개까지**만 다시 싣는다
//      (2001줄 `v14 >= byte(1565124+12*pl)` — 그 값은 처음 1, `nox_xxx_playerResetImportantCtr_4E4F40`).
//   ③ 기다림 = FPS × 2 / 전송률(1) = 60틱(2039줄 · 1565125 자리 초깃값 2).
//   ④ 머리 `aa` 는 조각마다 한 번(2011~2022줄 — 호스트가 아닌 사람에게는 `aa` 1바이트).
//   ⑤ 확인은 조각의 시각으로 온다 — 클라가 같은 조각의 `27` 로 localFrame 을 잡아 `ab <그 프레임>` 을 돌려준다
//      (`network_client.go:302~318`). ★ 09-30 — 이제 **묶음의 맨 끝 칸**에 `aa`+ 만 쓴다 — 같은 묶음 머리의 `27`(이 틱 `tickFrame`)이 그 시각이다
//      (정답지 `sendPoll` 이 `27`·`a4`·Kind1 뒤에 `netImportant_4E5770` 을 붙인다 · `network_lists.go:216~224`).
//      ✖ 전에는 따로 한 조각이라 제 `27` 을 **따로 붙였다**
void Server::FlushImportant(Session& s, std::vector<OutPacket>& out) {
    (void)out;
    if (!s.importantReady || s.importantQ.empty()) return;
    std::vector<uint8_t> pkt;
    pkt.reserve(kImportantPacketBudget + 8);
    pkt.push_back(static_cast<uint8_t>(Op::IMPORTANT));
    const size_t headLen = pkt.size();
    int resentThisTick = 0;
    for (auto& p : s.importantQ) {
        if (p.sent) {
            if (p.wait > 0) { --p.wait; continue; }
            if (resentThisTick >= 1) continue;
        }
        if (pkt.size() + p.wire.size() > kImportantPacketBudget) break;   // 다음 틱에
        if (p.sent) { ++resentThisTick; ++s.importantResent; }
        pkt.insert(pkt.end(), p.wire.begin(), p.wire.end());
        p.sent = true;
        p.sentFrame = s.tickFrame;
        p.wait = kImportantResendTicks;
    }
    if (pkt.size() == headLen) return;
    s.bundleImp.insert(s.bundleImp.end(), pkt.begin(), pkt.end());
    ++bundleStat_[static_cast<size_t>(BundleStat::InImp)];
}

// 불려 나온 것의 값 — **종류마다 다르다** (2026-09-10 정본화, ⭕ monster.bin).
//
// 전에는 넷 다 상수 하나였다(체력 40 · 피해 8 · 사거리 40 · 속도 5).
// 이제 표에서 찾는다 — 임프는 체력 25 에 원거리 5, 박쥐는 체력 35 에 근접 4 다.
//
// ⚠ 닿는 거리 — 파일의 `MELEE_ATTACK_RANGE` 는 몬스터 기준이라 **사람 몸 반지름을
//   더해야** 실제로 닿는다. 안 더하면 박쥐(15)가 사람 몸(10) 안으로 못 들어와
//   영영 다가가기만 한다.
// ⚠ 속도가 사람보다 느리다(임프 3.75 · 박쥐 3.28 대 사람 달리기 7). 정본이 그렇다 —
//   도망가는 사람은 못 잡는다. 퀘스트에서 몬스터가 안 닿으면 이 줄을 먼저 볼 것.
struct PetTuning {
    int      health;
    int      damage;
    double   reach;
    double   speed;
    uint32_t delay;
};

static PetTuning TuningFor(uint16_t type) {
    const MonsterStats* m = FindMonster(type);
    if (!m) return { kPetHealth, kPetDamage, kPetReach, kPetSpeed, kMeleeCooldownTicks };
    PetTuning t;
    t.health = m->health;
    // 근접이 비어 있으면 원거리형이다. 지금 발사체 피해를 아는 것은 임프뿐.
    t.damage = (m->meleeDamage > 0) ? m->meleeDamage
             : ((m->wireType == 1328) ? kImpShotDamage : kPetDamage);
    t.reach  = (m->meleeRange > 0.0) ? m->meleeRange + kPlayerRadius : kPetReach;
    t.speed  = (m->speed > 0) ? m->speed / kThingSpeedDivisor : 0.0;   // 0 = 못 움직인다
    t.delay  = static_cast<uint32_t>((m->minDelay > 0) ? m->minDelay : kDefaultMonsterDelay);
    return t;
}

// 공격 주문과 그 위력. **2026-09-10 정본화** — 근거는 `GameBalance.h`.
//
// **어느 주문이 공격이냐**는 이름표에서 갈랐다(`SpellNames.cpp`, 기계로 뽑은 것).
//
// ★ 파이어볼 ⭕ — thing.bin 의 `COLLIDE = SparkExplosionCollide <n>` 인자를 반으로.
//   레벨은 주문을 얼마나 충전했나인데 **와이어에 안 실린다**(`0x79` 본문 21B 가
//   주문 번호와 0 채움뿐). 지금은 최대 세기로 본다 — 아레나 캐릭은 레벨 10 이다.
// ★ 번개·에너지볼트 ⭕ — 파일 값은 **프레임당** 피해다. 우리는 한 방으로 주므로
//   한 번 시전이 얼마나 이어지는지를 곱해야 한다 —
//   `ARENA TargetedSpellLifetime = 45` 프레임(⭕ gamedata.bin)을 쓴다.
constexpr uint8_t kAssumedSpellLevel = 5;      // ⚠ 와이어에 없어 최대로 가정

// 이 주문이 날리는 것의 와이어 종류 번호.
//
// ⚠ **실측이 아니라 이름 짐작이다** (09-07 정정, 까닭은 Server.h 의 ProjectileType 주석).
// 항목 하나를 채운다 — **처음 보는 것은 전체 번호로, 그 뒤로는 별칭으로.**
//
// ★★ 2026-09-07 에 찾은 결함이다. 전에는 첫 등장부터 별칭으로 보냈다.
//    그러면 클라는 **그 별칭이 무엇인지 알 길이 없다** — 별칭에는 종류가 안 실린다.
//
//    「서버가 `0xa5 NEW_ALIAS` 로 알려 주면 되지 않나」 싶지만 **방향이 반대다** —
//    실측에서 `0xa5` 는 서버 SEND **0건**, 클라→서버로만 왔다(0907 덤프 187건,
//    클라 덤프 839건). 별칭은 **클라가 스스로 굴리고 서버에 알려 주는 것**이고,
//    서버는 같은 규칙으로 따라 굴린다(Alias.h). 그러니 클라가 (번호,종류)를
//    이미 알고 있어야 같은 자리를 고른다. **그 앎은 전체 번호로 온 첫 등장에서 온다.**
// ⚠ 사람 종류 번호는 **두 군데**에 적혀 있다 — 여기(`kPlayerType`)와 뜯는 쪽
//   (`ThingFlags.cpp` 의 `ThingIsPlayer`). 어긋나면 짜는 길이와 뜯는 길이가 갈려
//   **사람 뒤 항목이 전부 밀린다.** 그래서 여기서 못 박는다.
static_assert(kPlayerType == 713, "ThingIsPlayer() 의 713 과 같이 고칠 것");

namespace {
// nox_xxx_xferIndexedDirection_509E20 — 방향(0~255) → 행(x+3y+4). blob_587000 정수 방향표 192088 · 문턱 6(230092).
const uint8_t kDirRow[256] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};
// 갱신 항목 방향 4비트 — 정답지 짜는 쪽 그대로(`GAME4_1.c:4302~4309` 소환수 · `:4471~4478` 사람):
//   xferIndexedDirection 행(x+3y+4, 가운데 4 는 없음) → 4 이하면 그대로 · 넘으면 −1 → 0~7 을 위 4비트에.
//   클라는 4 이상이면 +1 해서 9칸 그림표 자리로 되돌린다(GAME2_3.c:1677~1679).
inline uint8_t WireDirNibble(uint8_t facing) {
    const uint8_t row = kDirRow[facing];
    return static_cast<uint8_t>(row <= 3 ? row : row - 1);
}
// nox_server_testTwoPointsAndDirection_4E6E50 의 표 — blob_587000 202504, [행][구역]. 맨손은 &1 을 본다.
const uint8_t kDirSector[9][16] = {
    { 0, 9, 6, 0, 5, 1, 4, 0, 10, 8, 2, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 0, 4, 5, 6, 0, 8, 9, 10, 0, 0, 0, 0, 0 },
    { 0, 5, 10, 0, 6, 4, 2, 0, 9, 1, 8, 0, 0, 0, 0, 0 },
    { 0, 8, 4, 0, 1, 9, 5, 0, 2, 10, 6, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 4, 8, 0, 2, 6, 10, 0, 1, 5, 9, 0, 0, 0, 0, 0 },
    { 0, 10, 5, 0, 9, 8, 1, 0, 6, 2, 4, 0, 0, 0, 0, 0 },
    { 0, 2, 1, 0, 8, 10, 9, 0, 4, 6, 5, 0, 0, 0, 0, 0 },
    { 0, 6, 9, 0, 10, 2, 8, 0, 5, 4, 1, 0, 0, 0, 0, 0 },
};
// sub_4E6CE0 — 두 점의 상대 구역(GAME3_3.c:2424)
int RegionCode(float fx, float fy, float tx, float ty) {
    const float dx = tx - fx, dy = ty - fy;
    const float a = dx * 0.41304299f - dy;
    const double b = static_cast<double>(dx) * 2.4210529 - dy;
    const float c = dx * -2.4210529f - dy;
    const float d = dx * -0.41304299f - dy;
    const int code = (a < 0.0f ? 0 : 8) | (b < 0.0 ? 0 : 4) | (c < 0.0f ? 0 : 2) | (d >= 0.0f ? 1 : 0);
    switch (code) {
        case 0: return 2;
        case 2: return 6;
        case 3: return 4;
        case 4: return 10;
        case 0xB: return 5;
        case 0xC: return 8;
        case 0xD: return 9;
        case 0xF: return 1;
        default: return 0;
    }
}
uint8_t TestTwoPointsAndDirection(float fx, float fy, uint8_t dir, float tx, float ty) {
    return kDirSector[kDirRow[dir]][RegionCode(fx, fy, tx, ty)];
}
}  // namespace

// ★ `playerAnim` — 사람 항목의 **자세 번호**(0 IDLE · 4 WALK). 사람이 아니면 안 쓴다.
//   ⭕ 번호표는 소스 확증(`draw_debug.go:294`). 걸을 때 4 를 보내야 다리가 움직인다.
// ★★ 2026-09-16 전수조사 ⓐ — `complexAnim` 인자를 더했다.
//   정답지는 **복합 안에서 다시 셋으로** 가른다(`GAME4_1.c:4539~4548`, 클래스 비트로) —
//     `& 2`(MONSTER) → `netSendComplexObject_518960`   덧붙이 하위 4비트 = **애니 칸**
//     `& 4`(PLAYER)  → `netPlayerObjSend_518C30`       자세는 **둘째 바이트**
//     그 밖          → `netSendPhantomPlrMb_5187E0`    덧붙이 = `16 * 방향니블` → **하위 4비트 0**
//   화살(`MISSILE+COMPLEX`, MONSTER 아님)은 셋째 갈래다. 여기에 `kCreatureAnimIdle`(8)을
//   넣으면 안 된다 — 그래서 크리처 기본값은 그대로 두고 **부르는 쪽이 0 을 주게** 했다.
static void FillEntry(AliasTable& tbl, uint16_t netCode, uint16_t type,
                      uint32_t frame, int x, int y, uint8_t facing, ObjEntry& e,
                      bool complex = true, uint8_t playerAnim = 0,
                      int forceDir = -1, int playerFrame = -1,
                      uint8_t complexAnim = kCreatureAnimIdle) {
    // ★★★ **정정(2026-09-09) — 따라 굴리지 않고 클라가 알려 준 것만 쓴다.**
    //   전에는 `Generate`/`Assign` 으로 클라와 같은 규칙을 흉내 냈는데 어긋났다.
    //   별칭으로 보내면 아무것도 안 그려졌고, 그래서 매 틱 전체번호로 다시 소개하다가
    //   클라를 1분 만에 무너뜨렸다. 이제 `0xa5 NEW_ALIAS` 로 **받은 자리만** 쓴다.
    //   못 찾으면 전체 번호(`ff`)로 소개한다 — 그걸 받은 클라가 별칭을 정해 알려 준다.
    const int alias = tbl.FindByObject(netCode, type, frame);
    if (alias > 0) {
        e.alias = static_cast<uint8_t>(alias);
    } else {
        e.alias = 0xff;          // 처음이다 — 번호와 종류를 실어 보낸다
        e.netCode = netCode;
    }
    e.type = type;
    e.absolute = true;           // 항목마다 절대 좌표 — 앞의 것에 안 얽힌다
    e.x = x;
    e.y = y;
    // ⚠ 복합이 아니면 **한 바이트도 붙지 않는다.** 깃발이 그렇다(종류표에서 확인).
    //   붙이면 클라가 그 바이트를 다음 항목의 시작으로 읽어 그 뒤가 다 어긋난다.
    e.extra = complex ? 1 : 0;
    if (complex) {
        // ★★★ **덧붙이 한 바이트는 「방향 4비트 + 애니 4비트」다** (2026-09-09 원본 대조).
        //
        //   클라(`GAME2_3.c:1580`)는 이 바이트를 이렇게 쪼갠다 —
        //     방향 = (값 >> 4) & 7      → 오브젝트+297
        //     애니 = 값 & 0x0F          → 오브젝트+276  (Flags&4 면 다음 한 바이트에서)
        //   그리고 그리는 쪽은 **그 애니가 없으면 아무것도 안 그리고 되돌아간다**
        //   (`drawable_monster.go`: `if ani.Cnt40 == 0 { return 1 }`).
        //
        //   ⚠ 여기에 **방향값(0~255)을 통째로** 싣고 있었다. 그러면 애니 번호가
        //     방향에 따라 0~15 로 아무렇게나 정해진다 — 그래서 **소환수와 몬스터가
        //     화면에 하나도 안 그려졌다**(2026-09-09 실측: 갱신에 25개인데 화면엔 사람 둘뿐).
        //
        //   애니 번호표는 둘로 갈린다(`draw_debug.go` 의 `debugStates` / `debugStates2`) —
        //     사람(클래스 0x4)  0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · 4 WALK …
        //     몬스터(0x40000)   8 IDLE · 9 DIE · 10 DEAD · 11 HURT · 12 MOVE …
        //   몬스터 쪽 8=IDLE 은 `drawable_monster.go:144` 의 `if dr.AnimInd != 8 { // IDLE` 로 재확인.
        e.extraBytes[0] = static_cast<uint8_t>(
            (WireDirNibble(facing) << 4) | (complexAnim & 0x0F));
        // ★ 최상위 비트(0x80)는 값이 아니라 **신호**다 — 서 있으면 클라가
        //   「스프라이트 프레임이 한 바이트 더 온다」로 읽는다(같은 자리).
        //   위 식은 방향을 **위 4비트 중 3비트**에만 넣으므로 이 비트가 늘 0 이다.
        //   그래서 전에 있던 `if (facing & 0x80) extra = 2` 는 필요 없어졌다 —
        //   방향값을 통째로 싣던 시절의 뒷수습이었다.
        //
        // ★★★ 2026-09-18 ⑬ — **화살·쇠뇌살에는 스프라이트 프레임을 한 바이트 더 싣는다.**
        //
        //   증상 — 화살이 어느 쪽으로 날든 **늘 같은 그림**이다.
        //   ⭕ 근거 넷 —
        //     ⑴ 클라에 계수기를 심어 재니 화살이 쓰는 **프레임 번호가 늘 0**(17,345 회 전부),
        //        그런데 화살 그림 표는 **32장**이다(`client__draw__arrowdraw.c` 로컬 계수기)
        //        ⚠ 처음엔 표의 첫 워드를 장수로 읽어 「12장」이 나왔는데 그 자리는
        //          **늘 12 인 상수**였다(`parse.c:21`). 장수는 `base+8` 의 1바이트(:23)
        //     ⑵ 정답지 `netSendPhantomPlrMb_5187E0`(GAME4_1.c:4198~4203)이 싣는 값이
        //        **`Direction1 >> 3`**(0~31) — 32장과 정확히 맞는다
        //     ⑶ 그 갈래 조건 `SubClass & 0x30` 은 무기 서브클래스의
        //        **`ARROW`(0x10) | `BOLT`(0x20)** 다(`subclass_weapon.go` 이름 순서)
        //     ⑷ `--dump-thing` 실측 — `MercArcherArrow` 서브클래스가 **`ARROW`** 라 참이고,
        //        `Fireball` 은 `MISSILE_COUNTERSPELL`(0x01) 이라 거짓이다
        //   ⚠ **한 번 헛짚었다** — ⑶ 을 확인하기 전에 「미사일 서브클래스엔 0x30 이 없다」고
        //     보고 되돌렸었다. 서브클래스 값은 **클래스마다 다르게 읽힌다**(화살은 WEAPON 쪽).
        //     `--dump-thing` 을 그때 만들었으면 한 번에 갈렸다.
        //   ⇒ 사람 쪽에 이미 있던 「0x80 켜고 프레임 한 바이트」 길과 같은 자리다
        //     (클라가 읽는 곳도 같다 — `GAME2_3.c:1681`).
        if (type != kPlayerType && playerFrame >= 0) {
            e.extraBytes[0] |= 0x80;
            e.extraBytes[1] = static_cast<uint8_t>(playerFrame);
            e.extra = 2;
        }
        // ★★★ 사람 항목 — 정품 **원바이트**로 확정했다(2026-09-08):
        //
        //     [첫 소개·전체번호]  … ff <번호2> <종류2> <좌표4> │ 40 00 │ …   덧붙이 **2**
        //     [그 뒤·별칭]        … cb <dx> <dy>              │ 40    │ …   덧붙이 **1**
        //
        //   ⑴ 값은 **늘 `0x40`** 이다. 방향값이 아니다 — 정품은 서 있든 움직이든 0x40 이었다.
        //   ⑵ **첫 소개일 때만 둘째 바이트가 붙는다.** 별칭으로 보내면서 둘째 바이트를
        //      같이 실으면 클라가 그것을 다음 항목의 시작으로 읽어 그 뒤가 밀린다.
        //      전에는 사람이면 **늘 2** 로 보내 그 자리가 어긋나 있었다.
        //   ⚠ 방향을 어디에 싣는지는 아직 모른다 — 여기가 아니다.
        if (type == kPlayerType) {
            // ★★★ **정정(2026-09-11) — 사람 항목은 별칭이든 아니든 늘 두 바이트다.**
            //
            //   ⭕ 근거 — 클라 디코더 `GAME2_3.c:1685` —
            //       if (*(uint8_t*)(v19 + 112) & 4) { v28 = *++v14; }  // 자세를 **별도 바이트**로
            //       else                            { v28 = v20 & 0xF; }
            //     `+112` 은 `ObjClass` 다(`client/drawable.go:572` — `ObjClass // 28, 112`).
            //     그리고 **사람은 `Class & 0x4`** 다(`draw_debug.go:87` — 그 가지에서만
            //     사람 자세표 `debugStates`(IDLE/DIE/DEAD/JUMP/WALK…)를 쓴다).
            //     ★ 즉 사람이면 **자세가 늘 별도 한 바이트로 온다.** 별칭 여부와 무관하다 —
            //       클래스는 오브젝트 속성이지 어떻게 줄여 보냈나와 상관이 없다.
            //
            //   ⚠ 위 실측 주석의 「별칭이면 덧붙이 1」과 부딪힌다. 1 로 보내면 클라는
            //     **다음 항목의 첫 바이트를 자세로 먹고** 그 뒤가 밀린다. 사람이 마지막
            //     항목이면 밀림이 화면에 안 드러나므로 실측 때 1 로 읽혔을 수 있다.
            //
            //   ★ **하위 4비트에 자세를 싣던 앞 시도는 화면을 못 바꿨다**(2026-09-11 실측):
            //     서 있는 채로 자세만 0↔2 로 바꿔 찍었더니 다른 픽셀 9,476·4,672 로
            //     **대조군(같은 자세 두 장) 8,200 범위 안**이었다. `else` 가지를 안 타는 것이다.
            // ★★★ **되돌림(2026-09-11 실측) — 자세는 하위 4비트가 맞다.**
            //
            //   위 추론(사람이 `Class & 4` 라 자세가 별도 바이트)은 **화면이 반박했다.**
            //   서 있는 채로 자세만 바꿔 **캐릭터 자리(95×135)** 만 재니 —
            //
            //     보낸 바이트                        화면
            //     `0x40`(1B) · `0x40 02` · `0x40 04`  회색   ← 셋이 **픽셀 단위로 같다**
            //     `0x42`(1B) · `0x44`(1B) · `0x40 FF` 파랑
            //
            //   ⑴ **1바이트 `0x42`·`0x44` 는 `0x40` 과 뚜렷이 다르다**(1,568 픽셀, 잡음 바닥 396)
            //      → 하위 4비트가 실제로 화면을 바꾼다
            //   ⑵ **2바이트로 보낸 `02`·`04` 는 아무 효과가 없다** → 클라가 그 바이트를
            //      자세로 안 읽는다. `0xFF` 만 달랐는데, `0xff` 는 **다음 항목의 「전체 번호」
            //      표식**이라 그 뒤를 통째로 다르게 읽은 것으로 보인다
            //
            //   ⚠ 즉 **사람의 `ObjClass` 에는 비트 2(0x4)가 안 켜져 있다.**
            //     `draw_debug.go:87` 의 `Class()&0x4` 가지는 그리기 디버그의 갈래일 뿐,
            //     와이어 길이를 정하는 `+112 & 4` 와 같은 비트라는 보장이 없었다.
            //     ★ 정답지 읽기도 **화면으로 확인하기 전까지는 가설**이다.
            // ★★★ **다시 2바이트로** (2026-09-11 두 번째 판정).
            //
            //   앞서 2바이트를 한 번 넣었다 뺐는데, 그때 실험이 **반쪽**이었다 —
            //   `--anim-pose` 를 스냅샷 경로에만 걸고 **주기 갱신은 자세 0 을 계속 덮어쓰던**
            //   상태로 쟀다. 두 경로 다 걸고 다시 재니 「하위 4비트가 먹는다」던 숫자가
            //   전부 **토글 잡음**이었음이 드러났다(자세4 vs 자세0 = 216, 대조군 488).
            //
            //   ⭕ 2바이트를 받쳐 주는 근거 —
            //     · 클라 `GAME2_3.c:1685` — `ObjClass & 4` 면 자세를 **별도 바이트**로 읽는다
            //     · `+112` 이 `ObjClass` (`opennox/src/client/drawable.go:572`)
            //     · **`verify_update`(정품 녹음 뜯고 다시 짜기)가 2바이트로도 통과한다**
            //   ⚠ 위 코드는 `ff` 일 때 둘째 바이트를 **0 으로 고정**하고 별칭일 때는 아예
            //     안 붙였다. 그러면 자세는 늘 IDLE 이고, 별칭 항목에서는 **다음 항목의 첫
            //     바이트를 자세로 먹는다.** 그것이 「걷기 모션 없음」의 모습과 맞는다.
            // ★★★ **바라보는 쪽을 싣는다** (2026-09-12 배선). 전에는 `0x40` 고정이었다.
            //
            //   ⚠ `0x40` 은 「값 없음」이 아니었다 — `(0x40>>4)&7 = 4` 이고 클라는 4 이상이면
            //     +1 하므로(`GAME2_3.c:1677-1679`) **방향 5 로 못 박혀 있었다.** 어느 쪽을
            //     보든 늘 같은 쪽을 보고 서 있었던 것이다.
            //
            //   ⭕ 근거 다섯 —
            //     ⑴ 자리 — 클라가 `(값>>4)&7` 로 읽는다 (`GAME2_3.c:1677-1679`)
            //     ⑵ 짜임 — **정품 서버 빌더**가 소환수(`COMPLEX_OBJ`)와 사람(`PLAYER_OBJ`)에
            //        **같은 자리**를 쓴다 (`opennox/src/network_lists.go:290-300` —
            //        `b1[off] = b2[9]` 뒤에 사람만 `b2[11]`(자세)이 하나 더 붙는다)
            //     ⑶ 왜 4 를 건너뛰나 — 클라의 방향 그림 배열이 **`Frames [9]`** 다
            //        (`opennox/src/client/drawable_player.go:14`). 9칸 중 4번을 비우고
            //        0,1,2,3,5,6,7,8 여덟 자리를 쓴다
            //     ⑷ 변환 — `(facing>>5)&7`. **우리 소환수 가지가 이미 쓰던 식**이다(위 339행)
            //        ❌ **정정(2026-09-13 묶음 3-a ④)** — 이 식은 틀렸다. 9칸 자리는 각도 팔분이 아니라
            //          **(x+3y+4) 격자**다(`GAME4_1.c:4471~4478` xferIndexedDirection → 0 = 왼쪽 위 · 5 = 동쪽).
            //          화면 증거: 서버 방향 10(동쪽)을 `(10>>5)&7 = 0` 으로 보내니 캐릭터가 **왼쪽 위로** 주먹을 뻗었다
            //          (`_evidence/b3a_punch_exp2` f_29·f_49). 09-12 실측은 「바뀐다」만 쟀지 「맞는 쪽」을 안 쟀다.
            //          → WireDirNibble(정답지 표 kDirRow) 로 바꿨다.
            //     ⑸ 값 — `facing` 은 살아 있다. 클라가 `PLAYER_INPUT` 코드 1 로 보내고
            //        (`Server.cpp:1592`) 걸음도 이 방향으로 민다(`:1597-1601`)
            //   ⭕ 실측(2026-09-12) — `--anim-dir 0` 두 장이 **196 픽셀**(잡음 바닥)인데
            //     `0` vs `6` 은 **1,992·1,996 픽셀**로 열 배다. 경계상자도 캐릭터 자리다
            //
            // ⚠ **하위 4비트는 0 으로 둔다.** 사람은 자세를 **둘째 바이트**로 받으므로
            //   (`ObjClass & 4` 가지) 하위 4비트를 안 읽는다. 소환수가 거기에
            //   `kCreatureAnimIdle` 을 넣는 것과 다르다 — 사람에 그걸 넣으면 안 된다.
            e.extraBytes[0] = static_cast<uint8_t>(WireDirNibble(facing) << 4);
            e.extraBytes[1] = static_cast<uint8_t>(playerAnim);  // 자세 — 별도 바이트
            e.extra = 2;
            // ★ 2026-09-13 묶음 3-a — **애니 칸 바이트.** 정답지 `nox_xxx_netPlayerObjSend_518C30`(GAME4_1.c:4447~4453)은
            //   공격(상태 1) 같은 상태에서 +236 칸 번호를 b2[10] 에 싣고, 짜는 쪽(`network_lists.go:290~299`)이
            //   그 값이 ff 가 아니면 방향 바이트에 0x80 을 켜고 **방향 · 칸 · 자세** 세 바이트로 보낸다.
            //   클라는 0x80 이면 칸을 읽어 `spriteSetFrameMB` 에 넣는다(GAME2_3.c:1680~1682).
            if (playerFrame >= 0) {
                e.extraBytes[2] = e.extraBytes[1];
                e.extraBytes[1] = static_cast<uint8_t>(playerFrame);
                e.extraBytes[0] |= 0x80;
                e.extra = 3;
            }
            // ★ 진단 — 방향 3비트를 못 박는다. **덧붙이가 클라에 닿는지**를 가르는 시험이다.
            if (forceDir >= 0) {
                e.extraBytes[0] = static_cast<uint8_t>(((forceDir & 0x07) << 4) | (e.extraBytes[0] & 0x80));
            }
        }
    }
}

// 별칭 항목의 좌표를 상대(직전 항목 기준)로 바꾼다 — 못 하면 전체번호+절대로 후퇴.
//
// ★★ 클라는 `00 <별칭> <절대4>` 를 못 읽는다(2026-09-09 사람·2026-09-10 정적 실측 —
//   둘 다 화면이 검어지고 클라가 죽었다). 규약은 「별칭이면 상대 · `ff` 면 절대」 둘뿐.
//   base 는 「직전 항목의 실제 자리」 — 디코더(UpdateStream.cpp)가 절대 항목에서
//   기준을 리셋하고 상대에서 더해 가는 것과 같은 축이다.
// ⚠ 별칭 임대 연장(Alias.h)을 넣기 전에는 임대가 2초에 끝나 다들 `ff` 로 나갔기
//   때문에 이 지뢰가 안 밟혔다 — 연장 뒤로는 별칭이 유지되므로 **별칭을 쓰는 모든
//   항목**(투사체·깃발·소환수·정적)이 이 길을 지나야 한다.
// ★★ 2026-09-16 전수조사 ⓒ — **좌표 관문 0~6000.**
//
//   ⭕ 정답지 빌더 `sub_4DF9B0`(`network_lists.go:276~279`) — 절대 좌표 항목의 x·y 가
//     6000 을 넘으면 **`return 0`** 으로 그 항목을 통째로 버린다.
//   ⭕ 정답지 클라 디코더(`GAME2_3.c:1653~1665`) — 쌓인 좌표가 음수이거나 6000 을 넘으면
//     **그 자리에서 실패로 되돌아간다**(`return &a1[-v14]`). 즉 **항목 하나가 갱신 한 통을 죽인다.**
//
//   ⚠ 맵 오브젝트는 안전하다 — 157개 맵 전수 확인(`scratchpad/p31/coordrange.cpp`),
//     관문 밖 0개 · 최대 x 5808 · y 5827. **위험한 것은 움직이는 것**이다(화살·놓인 것).
//     덫이 맵 가장자리에서 바깥으로 쏘면 화살이 6000 을 넘을 수 있다.
//
//   ⚠ **정답지와 두 자리 다르다 — 일부러 그렇게 했다.**
//     ⑴ 정답지 빌더는 **절대 좌표일 때만** 보는데 우리는 **상대일 때도** 본다 —
//        클라의 관문은 「쌓인 좌표」에 걸리므로 두 갈래 다 위험하다. 정답지 서버가 느슨해도
//        되는 까닭은 그쪽 오브젝트가 맵을 안 벗어나기 때문이다
//     ⑵ 정답지는 `uint16` 이라 음수를 못 보지만 우리는 `int` 라 **음수도 막는다**
static bool CoordInRange(int x, int y) {
    return x >= 0 && x <= 6000 && y >= 0 && y <= 6000;
}

static void Relativize(ObjEntry& e, uint16_t netCode, int wx, int wy,
                       int& baseX, int& baseY) {
    if (e.alias != 0xff) {
        const int dx = wx - baseX, dy = wy - baseY;
        if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127) {
            e.absolute = false;
            e.dx = static_cast<int8_t>(dx);
            e.dy = static_cast<int8_t>(dy);
        } else {
            e.alias = 0xff;
            e.netCode = netCode;
        }
    }
    baseX = wx;
    baseY = wy;
}

// 시야 반경 안 정적 오브젝트를 **가까운 순 상한**으로 갱신에 담는다 (P4-b).
//
// ★ 상한 없이는 EncodeUpdate 가 512B 를 넘겨 **0 을 돌려주고 갱신이 통째로 무산**된다.
//   스냅샷 조립이 그렇게 죽었다(2026-09-10 pa1 실측: 스냅샷이 387B —
//   `27 | a4 | cc 꼬리` 로 **a4 본문이 텅 비어** 사람 몸까지 안 실렸고 화면이 검었다).
//   estate 스폰 주변은 반경 700 안 정적이 40개를 넘는다.
//
// ★★ **별칭이면 좌표도 상대(직전 항목 기준)라야 한다** (2026-09-10 ao3 실측).
//   `00 <별칭> <절대4>` 꼴은 클라가 못 읽는다 — 사람 항목에서 이미 밟은 지뢰
//   (「별칭으로 보냈더니 화면이 검어졌다」, 갱신 루프의 2026-09-09 기록)를 정적이
//   그대로 다시 밟아, 접속 8초에 화면이 검어지고 20초에 클라가 죽었다.
//   상대차가 ±127 을 넘는 자리만 전체번호(ff)+절대로 후퇴한다 — `ff` 는 절대라야
//   한다는 규약도 실측으로 닫혀 있다(ff+상대 기각 기록).
//   base 는 「직전 항목의 실제 자리」로 이어 굴린다 — 사람 루프의 baseX/baseY 와 같은 축.
//
// ★★★ **정적·아이템은 「반경에 새로 들 때 한 번」만 소개하고 다시는 안 싣는다**
//   (2026-09-10 ao1~ao6 여섯 런 + 정품 전수 대조로 확정).
//
//   정품 갱신 1,879건의 항목엔 정적·아이템 **별칭 항목이 0건**이다 — 항상 갱신은
//   `a4 01 <좌표> 00 00 | cb <dx dy> 40 | 끝` 으로 **사람뿐**이고, 아이템은 첫 소개
//   (스냅샷·큰 갱신)에 1번 실리고 끝이다. 갱신 항목은 「움직인 것」의 목록이라,
//   아이템을 매 틱 항목에 넣으면(별칭+상대라도, 문법이 전수 유효해도) 클라 게임
//   루프가 첫 프레임에 멎는다 — ao1~ao6 이 전부 그 죽음이었다.
//   여기서는 introAt(이 사람에게 소개한 물체)에 없는 것만 ff 로 1번 소개한다.
// ★ 09-28 — `only` 가 있으면 그 번호들만(시야 반경이 꺼진 판에서 게임 중 새로 놓인 물건 — 버리기·되돌리기)
static void AppendAoiStatics(const std::vector<Server::WorldStatic>& statics,
                             AliasTable& tbl, std::map<uint16_t, uint32_t>& introAt,
                             uint32_t frame, int sx, int sy,
                             int& baseX, int& baseY, UpdateMsg& m,
                             const std::set<uint16_t>* only = nullptr) {
    struct Near { double d2; const Server::WorldStatic* w; };
    std::vector<Near> near_;
    for (const auto& w : statics) {
        if (only && !only->count(w.netCode)) continue;
        if (introAt.count(w.netCode)) continue;   // 이미 소개했다 — 다시는 안 싣는다
        const double dx = w.x - sx, dy = w.y - sy;
        const double d2 = dx * dx + dy * dy;
        if (d2 > kAoiRadius * kAoiRadius) continue;
        near_.push_back({ d2, &w });
    }
    if (near_.size() > kAoiMaxPerUpdate) {
        std::partial_sort(near_.begin(), near_.begin() + kAoiMaxPerUpdate, near_.end(),
                          [](const Near& a, const Near& b) { return a.d2 < b.d2; });
        near_.resize(kAoiMaxPerUpdate);
    }
    for (const auto& nr : near_) {
        ObjEntry e;
        FillEntry(tbl, nr.w->netCode, nr.w->type, frame, nr.w->x, nr.w->y, 0, e,
                  /*complex=*/ThingIsComplex(nr.w->type));
        e.alias = 0xff;                    // 소개는 늘 전체번호+절대 — 별칭이 있어도
        e.netCode = nr.w->netCode;         // (별칭 재사용은 「움직이는 것」의 규약이다)
        e.absolute = true;
        introAt[nr.w->netCode] = frame;
        baseX = nr.w->x;
        baseY = nr.w->y;
        m.objects.push_back(e);
    }
}

// 소개해 둔 정적 중 **시야에서 벗어난 것**을 골라낸다.
//
// ★ 왜 필요한가 — 지금까지는 「반경에 들 때 한 번 소개」만 있고 나갈 때가 없었다.
//   그래서 클라는 한 번 본 것을 영영 들고 있었고, 멀리 걸어가도 안 지웠다.
//   정품은 `32 OBJECT_OUT_OF_SIGHT` 로 지운다 — 2026-09-10 대조에서 서버→클라
//   방향으로 **156건**(`_evidence/CAP_server_0906.txt`). 지어낸 메시지가 아니다.
//
// ⚠ 골라낸 것은 `introAt` 에서 지운다. 그래야 되돌아왔을 때 다시 소개된다.
static void CollectAoiExits(std::map<uint16_t, uint32_t>& introAt,
                            const std::vector<Server::WorldStatic>& statics,
                            int sx, int sy, std::vector<uint16_t>& gone) {
    for (const auto& w : statics) {
        auto it = introAt.find(w.netCode);
        if (it == introAt.end()) continue;          // 아직 소개도 안 했다
        const double dx = w.x - sx, dy = w.y - sy;
        if (dx * dx + dy * dy <= kAoiDropRadius * kAoiDropRadius) continue;
        gone.push_back(w.netCode);
        introAt.erase(it);
    }
}

// 세계에서 정적 하나를 뺀다. **지우는 길이 아예 없었다**(계획서 벽②).
//
// ⚠ `staticIntroAt` 에서도 지운다 — 안 지우면 「이미 소개했다」가 영영 남아,
//   같은 번호가 나중에 다시 놓일 때(되돌려 떨구기) 아무에게도 소개되지 않는다.
// ★ 09-28 — 버린 물건의 사라질 프레임(0 = 안 사라짐). 버리기(`drop_4ED790`)는 물건별 함수(+712 · thing.bin `DROP`)를 부르고,
//   그 함수들은 모두 먼저 `dropDefault_4ED290` 을 부른다.
//   ① dropDefault — 협동(2048)·퀘스트(4096)가 아니고 RESPAWN 깃발·FLAG 클래스·Glyph 가 아니면 **10초**(`GAME3_3.c:6440~6444`)
//   ② 그 뒤 25초로 **다시 건다**(`unitSetDecayTime_511660` 은 앞 시계를 빼고 새로 넣는다 `GAME4_1.c:3033~3035`) —
//      갑옷·무기: 협동·퀘스트 아니고 「아이템 되살아남」이면(`GAME4_3.c:7761~7764·5183~5186`) · 물약: 협동·퀘스트 아니면(`GAME3_3.c:6877`) ·
//      음식: 협동 아니면(`:6893`)
//   ⭕ 정품 녹음 09-28 `orig_arena_1851` — 기본 로브 6043→6793 · 맵에서 주운 갑옷 6013→6765 · 죽을 때 떨어진 넷 3468→4219 (모두 약 750 = 25×30)
//   ⚠ 「아이템 되살아남」(`sub_409F40(2)` — 설정 `ItemRespawn`)은 우리에게 설정이 없다 — 녹음에서 켜져 있었으므로 켠 것으로 둔다
//   ⚠ 안 옮긴 것 — SHADOW 깃발(0x10000)이면 시계를 안 거는 막이(`:3032`) · 글리프·덫·왕관 버리기 함수
uint32_t Server::DropDecayFrame(DropFn fn, DecayExempt ex) {
    constexpr bool kItemRespawnOn = true;
    const bool coop = (gameSettings_.gameFlags & 0x800u) != 0;
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    auto count = [&](DecayGate g) { ++decayGate_[static_cast<size_t>(g)]; };
    if (fn == DropFn::NotPorted) { count(DecayGate::NotPorted); return 0; }
    DecayGate g = DecayGate::Armed;                          // ① dropDefault
    if (coop || quest) g = DecayGate::CoopOrQuest;
    else if (ex == DecayExempt::Respawn) g = DecayGate::Respawn;
    else if (ex == DecayExempt::FlagClass) g = DecayGate::FlagClass;
    else if (ex == DecayExempt::Glyph) g = DecayGate::Glyph;
    uint32_t at = (g == DecayGate::Armed) ? frame_ + 10u * kServerTickRate : 0;
    bool again25 = false;                                    // ② 물건별 함수
    switch (fn) {
        case DropFn::ArmorWeapon: again25 = !coop && !quest && kItemRespawnOn; break;
        case DropFn::Potion:      again25 = !coop && !quest; break;
        case DropFn::Food:        again25 = !coop; break;
        default: break;
    }
    if (again25) { at = frame_ + 25u * kServerTickRate; g = DecayGate::Armed25; }
    count(g);
    return at;
}

// ★ 09-28 — `nox_xxx_decay_511750`(`GAME4_1.c:3100~3124`) — 사라질 프레임이 된 바닥 물건을 지운다.
//   정답지 목록은 때 순으로 늘어서 있어 첫 「아직」에서 멈추지만, 우리는 바닥 물건을 다 훑는다(결과는 같다).
//   ✖ 09-28 정정 — 처음엔 줍기처럼 `31 DESTROY_OBJECT` 를 모두에게 보냈다. 정품 녹음(`orig_arena_1851`)에서 사라질 때 온 것은
//     **`33 OBJECT_IN_SHADOWS`** 를 그것을 알던 사람에게 — 지운 오브젝트는 사람별 갱신에서 「더는 안 보임」 갈래를 타고(`GAME4_1.c:4560~4565`)
//     물건(유닛 아님)이면 33 이다(`nox_xxx_netObjectInShadows_528A90` — 맨 메시지 · 중요). 녹음 — 죽을 때 떨어진 넷 · 버린 둘 모두 두 클라에 33
//   들린 것은 애초에 `statics_` 에 없다(줍기가 뺀다)
void Server::DecayStatics(std::vector<OutPacket>& out) {
    for (size_t i = statics_.size(); i-- > 0;) {
        const WorldStatic& w = statics_[i];
        if (w.decayFrame == 0 || frame_ < w.decayFrame) continue;
        const uint16_t nc = w.netCode;
        const uint8_t msg[3] = { static_cast<uint8_t>(Op::OBJECT_IN_SHADOWS), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
        for (auto& kv : sessions_)
            if (kv.second.inGame && kv.second.staticIntroAt.count(nc)) { EmitImportantRaw(kv.second, msg, sizeof(msg), out); ++decayShadowSent_; }
        TakeStatic(nc);
        ++decayExpired_;
    }
}

bool Server::TakeStatic(uint16_t netCode) {
    size_t at = statics_.size();
    for (size_t i = 0; i < statics_.size(); ++i)
        if (statics_[i].netCode == netCode) { at = i; break; }
    if (at == statics_.size()) return false;
    statics_.erase(statics_.begin() + static_cast<long>(at));
    for (auto& kv : sessions_) kv.second.staticIntroAt.erase(netCode);
    freshStatics_.erase(netCode);   // ★ 09-28 — 게임 중 새로 놓인 것 표에서도
    return true;
}

// 이 주문이 불러내는 것의 와이어 종류 번호.
//
// ⚠ **실측이 아니라 이름 짝짓기다** (Server.h 의 SummonType 주석 참조).
//   `SpellNames.cpp` 의 `SPELL_SUMMON_*` 과 종류표의 같은 이름을 맞춘 것이고,
//   **소환 표본이 덤프에 없어 대조한 적이 없다.** 열둘이 COMPLEX 인 것만 표에서 확인했다.
uint16_t Server::SummonType(uint8_t spell) {
    switch (spell) {
        case 75: return 1357;  // SUMMON_BAT                → Bat
        case 76: return 1366;  // SUMMON_BLACK_BEAR         → BlackBear
        case 77: return 1365;  // SUMMON_BEAR               → Bear
        case 78: return 1382;  // SUMMON_BEHOLDER           → Beholder
        case 79: return 1348;  // SUMMON_BOMBER             → Bomber
        case 80: return 1371;  // SUMMON_CARNIVOROUS_PLANT  → CarnivorousPlant
        case 81: return 1355;  // SUMMON_ALBINO_SPIDER      → AlbinoSpider
        case 82: return 1356;  // SUMMON_SMALL_ALBINO_SPIDER→ SmallAlbinoSpider
        case 83: return 1317;  // SUMMON_EVIL_CHERUB        → EvilCherub
        case 84: return 1337;  // SUMMON_EMBER_DEMON        → EmberDemon
        case 85: return 1325;  // SUMMON_GHOST              → Ghost
        case 86: return 1358;  // SUMMON_GIANT_LEECH        → GiantLeech
        // ★ 87 은 표에 없었다 — **컨저러 캐릭터를 만들어 실제로 눌러 보고서야 알았다.**
        //   2026-09-09: 컨저러 주문칸 A 가 `0x79` 로 87 을 보냈는데 여기가 0 을 돌려줘
        //   시전 수만 오르고 **소환수가 하나도 안 생겼다.**
        //   번호 = `dumpscan -things <thing.bin> -find Imp` 의 색인 1327 + 1.
        //   **와이어 번호는 색인 +1** 이다(genthings.go 의 실측 주석). Bat 색인 1356 → 1357 로 검산했다.
        case 87: return 1328;  // SUMMON_IMP                → Imp
        default: return 0;
    }
}

// 사거리 안에서 가장 가까운 다른 사람.
std::string Server::NearestOther(const std::string& self, double range) const {
    auto me = sessions_.find(self);
    if (me == sessions_.end() || !me->second.inGame) return {};

    std::string best;
    double bestD2 = range * range;
    for (const auto& kv : sessions_) {
        if (kv.first == self) continue;
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;   // 죽어 있는 사람은 안 맞는다
        const double dx = static_cast<double>(o.x - me->second.x);
        const double dy = static_cast<double>(o.y - me->second.y);
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = kv.first; }
    }
    return best;
}

Target Server::NearestTarget(const std::string& friendOwner, double x, double y,
                             double range) const {
    Target best;
    double bestD2 = range * range;

    for (const auto& kv : sessions_) {
        if (kv.first == friendOwner) continue;          // 나 자신은 안 때린다
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;
        const double dx = o.x - x, dy = o.y - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best.addr = kv.first; best.pet = -1; }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (pets_[i].owner == friendOwner) continue;    // 내 편은 안 때린다
        const double dx = pets_[i].x - x, dy = pets_[i].y - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best.addr.clear(); best.pet = static_cast<int>(i); }
    }
    return best;
}

// ★ 10-01 — **죽음 함수**(`monsterCallDieFn_50A3D0` `GAME4_1.c:471~504` · 들어오는 길 = 체력 0 `GAME3_3.c:7158~7171` · 죽음 버프 끝).
//   ✖ 전에는 여기서 `pets_` 에서 지우고 `31` 을 보내 **시체 없이 바로 사라졌다**(10-01 사람 판 4 — 곰·거머리·임프).
//   원본 — 죽음 깃발 0x8000(이미 켜져 있으면 다시 안 죽음 `:7163`) · 쌓임을 비우고 31(죽음)·30(죽어감 — 맨 위) · 버프 끔 ·
//   썩음 시간(소환수 +1440 0x80 → 10~20초 · 퀘스트 5~8초 · 그 밖 없음 `:481~489`) · 주인에게 `6d`
//   ⚠ 주인은 **안 지운다**(원본 `:504` 는 지움) — 우리는 주인 없음을 「맵 몬스터」로도 쓰므로 시체가 맵 몬스터로 섞인다 → 죽음 표시로 거른다(플랜 결정 ①)
//   ⚠ 안 옮김 — 들고 있던 것 떨구기(`:505~508` · 소환수는 가방 없음) · 감시 비트·부하 넘기기(`:500~503`)
//   실제 지우기는 `RemovePet` — 썩음 프레임 · 31 의 DESTROY_WHEN_DEAD(조각 2) → `FlushDeadPets`(AI 돌기 뒤 한꺼번에)
void Server::KillPet(size_t idx, std::vector<OutPacket>& out) {
    if (idx >= pets_.size()) return;
    if (pets_[idx].dead) return;                             // `GAME3_3.c:7163` — 시체를 또 때려도 6d·죽음 소리·퀘스트 셈이 두 번 안 나게
    // ★ 09-26 M4a — DIE(+60 · `nox_xxx_mobGenericDeath_544C40` `GAME4_3.c:9801~9804`) — 죽은 자리에서 모두에게.
    //   부르는 자리 둘(체력 0 · 죽음 버프 끝) 모두 죽음이라 여기서 낸다 · 지우기 전에(자리·종류가 필요)
    MonsterSound(pets_[idx], 14);
    const uint16_t netCode = pets_[idx].netCode;
    const bool wild = pets_[idx].owner.empty();   // 주인 없는 것 = 맵의 몬스터
    {
        Creature& k = pets_[idx];                            // ⚠ 아래 퀘스트 웨이브가 `pets_` 에 넣기 전에 다 적는다(참조가 깨진다)
        k.dead = true;
        k.health = 0;
        k.flagsSet |= 0x8000;                                // DEAD(`GAME3_3.c:7164`)
        k.flagsClear |= 0x80;                                // MISSILE_HIT 끔(`GAME4_1.c:476~478`)
        AIClearStack(k);                                     // `monsterClearActionStack_50A3A0`
        AIPush(k, 31);
        AIPush(k, 30);
        ClearBuffs(Target{ std::string(), static_cast<int>(idx) });   // `unitClearBuffs_4FF580`(`:480`)
        // 썩음 시간 — `randomInt` 는 양끝 포함
        uint32_t secs = 0;
        if (!wild) secs = 10 + NextRandom(11);               // 소환수(+1440 0x80) 10~20초
        else if (questTotal_ > 0 || questStage_ > 0) secs = 5 + NextRandom(4);   // 퀘스트(4096) 5~8초
        k.decayFrame = secs ? frame_ + secs * kServerTickRate : 0;   // 아레나 맵 몬스터는 안 썩음
        ++corpseStat_[0];
    }
    // ★ 10-01 — 주인이 사람이면 `6d`(번호2 · Packet1) — `monsterCallDieFn_50A3D0`(`GAME4_1.c:491~498`) → `4D9200`(`GAME3_2.c:4617~4626`).
    //   클라는 제 소환수 목록에서 뺀다(`cdecode.c:1528~1552`) · 짝 = `ReportAcquireCreature` 의 `6c`
    //   ⚠ 안 옮김 — 미니맵 장부 지우기 `netUnmarkMinimapObj_417300` · 종류 윗비트(GameFlag28)
    if (!wild) {
        auto oi = sessions_.find(pets_[idx].owner);
        if (oi != sessions_.end() && oi->second.inGame) {
            const uint8_t lose[3] = { static_cast<uint8_t>(Op::REPORT_LOSE_CREATURE),
                                      static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8) };
            EmitSeq(oi->second, lose, sizeof(lose), out);
            ++summonOwnSent_[2];
        }
    }
    ++petsKilled_;
    if (wild && questTotal_ > 0) {
        ++questKilled_;
        // 스테이지를 다 잡았다 — 다음 스테이지, 아니면 판끝.
        if (!roundOver_ && questKilled_ >= questTotal_) {
            if (questStage_ + 1 < kQuestStageCount) {
                ++questStage_;
                SpawnQuestStage(&out);   // 다음 웨이브가 밀려온다
            } else {
                roundOver_ = true;
                restartAtFrame_ = frame_ + kRoundRestartTicks;
                SendNotice("Quest complete", out);
            }
        }
    }
}

// ★ 10-01 — 실제 지우기(전에 `KillPet` 끝에 있던 것). 부르는 곳은 `FlushDeadPets` 하나.
// ★ `0x31 DESTROY_OBJECT` 는 실측이다 — `31 d6 02`(번호 2바이트, 모두 3바이트).
//   09-06 덤프에 54건 있었다. **이걸 안 보내면 화면에 시체가 영원히 남는다.**
void Server::RemovePet(size_t idx, std::vector<OutPacket>& out) {
    if (idx >= pets_.size()) return;
    const uint16_t netCode = pets_[idx].netCode;
    pets_.erase(pets_.begin() + static_cast<long>(idx));
    uint8_t msg[3];
    msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
    msg[1] = static_cast<uint8_t>(netCode);
    msg[2] = static_cast<uint8_t>(netCode >> 8);
    for (auto& kv : sessions_) {
        if (kv.second.inGame) {
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }
    }
}

// ★ 10-01 — 썩음 프레임이 된 시체(`unitSetDecayTime_511660` 의 썩는 목록)와 지울 표시(`delayedDeleteObject_4E5CC0`)를 한꺼번에 지운다.
//   AI 를 다 돈 뒤 한 번 — 도는 도중에 지우면 `pets_` 순번이 밀린다(전에는 그래서 「크기가 바뀌면 그 걸음을 접는다」를 뒀다)
void Server::FlushDeadPets(std::vector<OutPacket>& out) {
    size_t corpses = 0;
    for (size_t i = pets_.size(); i-- > 0;) {
        const Creature& k = pets_[i];
        if (k.removeMe) { RemovePet(i, out); ++corpseStat_[2]; continue; }
        if (k.dead && k.decayFrame != 0 && frame_ >= k.decayFrame) { RemovePet(i, out); ++corpseStat_[1]; continue; }
        if (k.dead) ++corpses;
    }
    if (corpses > corpseStat_[3]) corpseStat_[3] = corpses;
}

// ★ 09-20 — 투사체의 **주인 사슬 뿌리가 몬스터인가**. 한 곳에서만 센다(두 갈래가 같은 셈을 쓴다).
//   크리처가 쏜 것만 참이다 — 덫 화살은 `ownerPet` 이 −1 이라 거짓이고, 그래서 맵 몬스터에게 적이 된다(⑮)
bool Server::MissileRootIsMonster(const MissileObj& m) const {
    if (!m.owner.empty()) return false;                    // 사람이 쏜 것
    if (m.ownerPet < 0 || static_cast<size_t>(m.ownerPet) >= pets_.size()) return false;
    return pets_[static_cast<size_t>(m.ownerPet)].owner.empty();   // 맵 몬스터가 쏜 것
}
bool Server::OwnersEnemy(const std::string& a, bool aMonsterRoot, const std::string& b, bool bMonsterRoot) const {
    // `server/object.go:1667~1741` 가운데 지금 있는 것들 — 같은 주인 → 적 아님 · 같은 팀 → 적 아님 ·
    //   한쪽이라도 팀 있음 → 적 · **둘 다 몬스터** → 적 아님 · 그 밖 → 적
    // ★★ 09-20 고침 — 전에는 「주인 없음 = 맵 몬스터」로 뭉쳐서, **덫 화살이 몬스터를 못 때렸다**
    //   (⑮ 실측 45건 전부 막힘). 정답지는 주인 사슬의 **뿌리 종류**로 가른다 — 주인 없는 화살의 뿌리는
    //   화살 자신(투사체)이라 `1737`(둘 다 몬스터)에 안 걸리고 `1740` 에서 적이 된다
    if (a.empty() && b.empty()) {
        if (aMonsterRoot && bMonsterRoot) { ++wildBothMonster_; return false; }   // 맵 몬스터끼리
        ++wildRootNotMonster_;
        return true;                                       // 덫 화살·환경 피해 등
    }
    if (a.empty() || b.empty()) return true;               // 사람(또는 사람 소환수) 대 맵 몬스터
    if (a == b) return false;                              // 같은 주인
    return IsEnemyTo(a, b);                                // 사람 대 사람(퀘스트 규칙은 IsEnemyTo 주석)
}

// ── 묶음 4-b1 — damageDefaultProc 의 버프 갈래 도우미 ─────────────────────────
// a2(때린 이) — 소환수가 때렸으면 그 소환수, 아니면 사람(by). 둘 다 없으면 없음(주인 없는 불·독 등)
Target Server::DamageAttacker(const std::string& by, const AttackSrc* src) const {
    if (src && src->byPet >= 0 && static_cast<size_t>(src->byPet) < pets_.size()) return Target{ std::string(), src->byPet };
    if (!by.empty() && sessions_.count(by)) return Target{ by, -1 };
    return Target{};
}

// 감전(22) 되갚기(`GAME3_2.c:7101~7116`) — 무기(a3)로 근접(sub_4E1400)해 때린 유닛에게 ShockDamage[4] 전기(9) · 사람이면 상태 23 · 버프 끔(소리 135 → 10)
//   sub_4E1400(a2, a3): 무기 클래스이고 원거리 SUBCLASS(0x47F0000) 아님 → 1 · 투사체면 0
void Server::DamageShockBack(const Target& victim, const Buffs& vb, const Target& attacker, const AttackSrc* src, std::vector<OutPacket>& out) {
    if (attacker.none() || !vb.Has(22) || !src || !src->obj || src->missile) return;
    if (!attacker.addr.empty()) {
        const Session& a = sessions_.at(attacker.addr);
        if (WeaponEquipOf(a) & 0x47F0000) return;          // 지팡이·원거리 무기
    }
    BuffOff(victim, 22);
    const int back = static_cast<int>(static_cast<float>(BalanceFloatInd("ShockDamage", 4)));
    const std::string victimOwner = !victim.addr.empty() ? victim.addr : pets_[static_cast<size_t>(victim.pet)].owner;
    // ★ 09-20 — 되갚는 쪽(맞은 이)이 **맵 몬스터면 뿌리가 몬스터**라고 알려야
    //   몬스터끼리 서로 때리지 않는다(편 거르기 `OwnersEnemy`)
    AttackSrc backSrc;
    backSrc.rootIsMonster = victimOwner.empty();
    // ★ 4-c3b1 — 때린 것 = a1(감전 건 쪽) 자신(a3 = 0 · `GAME3_2.c:7118`) · 그쪽이 소환수(몬스터)면 소리 모음 갈래라 4-c3b3
    HurtTarget(attacker, back, out, victimOwner, 9, &backSrc, HitterUnit(victim));   // (a2+716)(a2, a1, 0, 피해, 9)
    if (!attacker.addr.empty()) {
        auto it = sessions_.find(attacker.addr);
        if (it != sessions_.end()) PlayerSetState(it->second, 23, frame_);
    }
}

// 불·전기 보호(`checkFireProtect_4DFE40` · `checkElectrProtect_4DFF40`) — 아이템 몫(최대 0.5, 아이템 5) + 버프 세기 표 · 합 최대 0.6 · 곱한 뒤 0 이면 1
int Server::DamageProtect(const Buffs& b, uint8_t dtype, int amount) const {
    auto protect = [&](int buff, const char* table) {
        const float items = 0.0f;                           // ⚠ 입은 것 마법부여 합(sub_4DFD10 · 4DFD80) — 아이템 5
        float r = items;
        if (b.Has(buff)) r = static_cast<float>(BalanceFloatInd(table, static_cast<int>(b.power[buff]) - 1) + items);
        if (r > 0.60000002f) r = 0.60000002f;
        return r;
    };
    if (dtype == 1 || dtype == 12 || dtype == 7) {
        const float v = protect(17, "FireSpellProtection");
        amount = static_cast<int>(static_cast<float>((1.0 - v) * static_cast<double>(amount)));
        if (!amount) amount = 1;
    }
    if (dtype == 9 || dtype == 17) {
        const float v = protect(20, "ElectricitySpellProtection");
        amount = static_cast<int>(static_cast<float>((1.0 - v) * static_cast<double>(amount)));
        if (!amount) amount = 1;
        // ⚠ 사람 +160 = 2 · NPC 몬스터 +2094 = 2(쓰는 곳 미확인 — 관측대기)
    }
    return amount;
}

// 흡혈(13, `GAME3_2.c:7268~7287`) — 때린 이가 흡혈이면 VampirismCoeff[세기−1] × 피해(최소 1)를 채우고 `a2 <때린 이 x,y> <맞은 이 x,y> <채운 양>` 을 맞은 자리 화면 안에
void Server::DamageVampirism(const Target& attacker, const Target& victim, int amount, std::vector<OutPacket>& out) {
    const Buffs* ab = BuffsOf(attacker);
    if (!ab || !ab->Has(13)) return;
    const float heal = static_cast<float>(BalanceFloatInd("VampirismCoeff", static_cast<int>(ab->power[13]) - 1) * static_cast<double>(amount));
    uint16_t v32 = static_cast<uint16_t>(static_cast<int>(heal));
    if (v32 < 1u) v32 = 1;
    UnitAdjustHP(attacker, v32, out);
    float ax = 0, ay = 0, vx = 0, vy = 0;
    auto posOf = [&](const Target& t, float& x, float& y) {
        if (!t.addr.empty()) { x = static_cast<float>(sessions_.at(t.addr).posX); y = static_cast<float>(sessions_.at(t.addr).posY); }
        else { x = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); y = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y); }
    };
    posOf(attacker, ax, ay);
    posOf(victim, vx, vy);
    const uint16_t p[4] = { static_cast<uint16_t>(static_cast<int>(ax)), static_cast<uint16_t>(static_cast<int>(ay)),
                            static_cast<uint16_t>(static_cast<int>(vx)), static_cast<uint16_t>(static_cast<int>(vy)) };
    uint8_t msg[11] = { static_cast<uint8_t>(Op::FX_VAMPIRISM) };
    for (int k = 0; k < 4; ++k) { msg[1 + 2 * k] = static_cast<uint8_t>(p[k]); msg[2 + 2 * k] = static_cast<uint8_t>(p[k] >> 8); }
    msg[9] = static_cast<uint8_t>(v32);
    msg[10] = static_cast<uint8_t>(v32 >> 8);
    SendFxAllCli(vx, vy, msg, sizeof(msg), out);           // netSendVampFx_523270 — 자리 = 맞은 이
}

// nox_xxx_unitAdjustHP_4EE460(`GAME3_3.c:7097`) — 최대 아래일 때만 채움(최대로 자름) · **알림은 없다**(unitSetHP 는 동기 비트만).
//   사람 `43` 은 다음 틱 PlayerReportAnything(+10 ≠) 이, `42` 는 갱신이 음수만 보낸다 — 회복은 `42` 로 안 나간다 · 소환수는 주인 체력 알림 41
void Server::UnitAdjustHP(const Target& t, int dv, std::vector<OutPacket>& out) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end() || !it->second.inGame) return;
        Session& s = it->second;
        if (s.health >= s.maxHealth) return;
        int after = s.health + dv;
        if (after > s.maxHealth) after = s.maxHealth;
        s.health = static_cast<uint16_t>(after);
        MarkDirty(s.netCode, PendingReport::What::UnitHealth);
        (void)out;
        return;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return;
    Creature& k = pets_[static_cast<size_t>(t.pet)];
    const int maxHp = TuningFor(k.type).health;
    if (k.health >= maxHp) return;
    int after = k.health + dv;
    if (after > maxHp) after = maxHp;
    k.health = static_cast<uint16_t>(after);
    MarkDirty(k.netCode, PendingReport::What::UnitHealth);   // mobInformOwnerHP_4EE4C0
}

// collSysAddCollision_548630 — 같은 짝(순서 무관)은 한 번만
void Server::AddContact(const ContactRef& a, const ContactRef& b) const {
    auto same = [](const ContactRef& x, const ContactRef& y) { return x.kind == y.kind && x.addr == y.addr && x.idx == y.idx; };
    for (const auto& pr : contacts_)
        if ((same(pr.first, a) && same(pr.second, b)) || (same(pr.first, b) && same(pr.second, a))) return;
    contacts_.push_back({ a, b });
}

// nox_xxx_collide_4FDF90(`GAME4.c:2966`) — a1 이 a2 에 닿았다
//   ① a1 감전(22) · a2 가 DEAD|ALLOW_OVERLAP 아닌 유닛 · 적 → 버프 끄고(소리 135 → 10) ShockDamage[세기−1] 전기(9)를 a2 에(때린 이 = a1)
//   ② a2 가 유닛·몬스터 생성기(0x20006)이고 DEAD|DESTROYED 아니고 같은 편 아니면 a1 투명(0) 끔
//   ③ a1 사람 · a2 몬스터 생성기(0x20000) → a1 투명 끔(생성기는 8 — 지금 없음)
// ★ 09-23 nine-defects 1-d — `nox_xxx_collideDamage_4E9430`(`GAME3_3.c:3803~3822`) — **장애물의 닿음 피해**.
//   정답지 `nox_xxx_collide_548740` 은 부딪힌 짝마다 a.collide(b) 를 부른다. 맵에 놓인 불(`FlameImmobile` 류)의 닿음 함수가
//   `DamageCollide <양> <종류>` 이고, 그 몫은 —
//     · 맞는 쪽이 **체력이 있으면**(+556) · 피해 = 양 >> 1, 단 양이 1 이면 **홀수 프레임에만 1**
//     · 때린 이 = `findParentChainPlayer(불)` — 주인 없는 불은 **자기 자신**(우리 말로 주인 없음) · 종류는 thing.bin 그대로
//   전에는 부딪힘 짝에서 버프 몫(4FDF90)만 불러 **맵 불의 피해가 한 번도 없었다**(사람 판 「불에 들어가도 안 죽음 · 피해 0」)
//   ⚠ 겹쳐도 되는(ALLOW_OVERLAP) 불도 짝으로 적힌다 — `BodyVsBody` 가 밀기 판단보다 먼저 `AddContact` 한다
//   ⚠ 소환수의 닿음 함수(MonsterCollide)는 아직 없다. 사람의 것(PlayerCollide)은 ★ 2-b2 에서 광전사 갈래 · ★ 09-26 M2 에서 죽음 넘기기(LABEL_26)
void Server::ContactCollide(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out) {
    if (a1.kind == 1) {                                      // ★ 2-b2 — 사람의 닿음 함수 `PlayerCollide`
        auto it = sessions_.find(a1.addr);
        if (it != sessions_.end() && it->second.inGame) PlayerCollide(it->second, a2.kind == 0 ? nullptr : &a2, out);   // ★ 09-26 M2
        return;
    }
    if (a1.kind != 3 || a1.idx < 0 || static_cast<size_t>(a1.idx) >= obstacles_.size()) return;
    const PhysBody& o = obstacles_[static_cast<size_t>(a1.idx)];
    if (o.contactDmgAmount == 0) return;
    Target t;
    if (a2.kind == 1) {
        auto it = sessions_.find(a2.addr);
        // 죽은 사람은 부르지 않는다. ⚠ 정답지는 체력 자료(+556)만 보므로 죽은 것도 부를 수 있다 — 다만 우리는
        //   죽은 사람을 부딪힘에서 이미 뺀다(`movers` 의 collide = 살아 있음)라 이 줄로 갈리는 경우는 없다
        if (it == sessions_.end() || it->second.respawnAtFrame != 0) return;
        t = Target{ a2.addr, -1 };
    } else if (a2.kind == 2) {
        if (a2.idx < 0 || static_cast<size_t>(a2.idx) >= pets_.size()) return;
        t = Target{ std::string(), a2.idx };
    } else {
        return;                                              // 장애물끼리 — 체력이 없다
    }
    const int dmg = (o.contactDmgAmount == 1 && (frame_ & 1u)) ? 1 : (o.contactDmgAmount >> 1);
    ++contactDamageHits_;
    // ★ 4-c3b2 — 때린 것 = 불 자신(a3 = a1 · `GAME3_3.c:3820`)
    HurtTarget(t, dmg, out, std::string(), o.contactDmgType, nullptr, HitterObj(o.wireType));   // 주인 없는 불 — 피해 임자 없음
}

void Server::CollideBuffs(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out) {
    if (a1.kind == 3) return;                              // 장애물은 버프가 없다
    const Target t1 = (a1.kind == 1) ? Target{ a1.addr, -1 } : Target{ std::string(), a1.idx };
    const Target t2 = (a2.kind == 1) ? Target{ a2.addr, -1 } : Target{ std::string(), a2.idx };
    const Buffs* b1 = BuffsOf(t1);
    if (!b1) return;
    const bool unit2 = (a2.kind == 1 || a2.kind == 2) && BuffsOf(t2) != nullptr;
    bool dead2 = false;
    if (a2.kind == 1 && unit2) dead2 = sessions_.at(a2.addr).respawnAtFrame != 0;   // DEAD(ALLOW_OVERLAP 은 죽음과 함께)
    auto owner = [&](const Target& t) -> std::string { return !t.addr.empty() ? t.addr : pets_[static_cast<size_t>(t.pet)].owner; };
    const std::string own1 = owner(t1);
    if (b1->Has(22) && unit2 && !dead2) {
        const std::string own2 = owner(t2);
        if (OwnersEnemy(own1, own1.empty(), own2, own2.empty())) {
            const int power = b1->power[22];
            BuffOff(t1, 22);
            const int dmg = static_cast<int>(static_cast<float>(BalanceFloatInd("ShockDamage", power - 1)));
            AttackSrc src;
            if (a1.kind == 1) { src.x = static_cast<float>(sessions_.at(a1.addr).posX); src.y = static_cast<float>(sessions_.at(a1.addr).posY); }
            else { src.x = static_cast<float>(pets_[static_cast<size_t>(a1.idx)].x); src.y = static_cast<float>(pets_[static_cast<size_t>(a1.idx)].y); src.byPet = a1.idx; }
            src.rootIsMonster = own1.empty();                  // 09-20 — 되갚는 쪽이 맵 몬스터면 뿌리가 몬스터
            HurtTarget(t2, dmg, out, own1, 9, &src, HitterUnit(t1));   // (a2+716)(a2, a1, a1, 피해, 9) · ★ 4-c3b1 때린 것 = a1
            if (!BuffsOf(t1)) return;                          // 되갚다 사라진 소환수
        }
    }
    if (unit2 && !dead2) {
        const std::string own2 = owner(t2);
        const bool sameTeam = (!own1.empty() && own1 == own2) ||
                              (a1.kind == 1 && a2.kind == 1 && sessions_.at(a1.addr).team != 0 &&
                               sessions_.at(a1.addr).team == sessions_.at(a2.addr).team);
        if (!sameTeam) BuffOff(t1, 0);
    }
}

// 피해 함수(+716) — 정답지 이식(묶음 3-e, 2026-09-13).
//   ⚠ 옮기지 않은 갈래(다른 조각): 방패·대검·지팡이 막기(3-e2 — 상태 15/16 · 18~21) · 되받아치기 버프 27 · 불/전기 보호 · 흡혈 ·
//     전기 방패 22 · 마법 방패 26(주문 4) · 입은 것 닳기·방어 마법 효과(아이템 5) · 스켈레톤 막기(몬스터 행동 21 · AI 6)
// ★ 4-c3b2b — 돌려주는 값은 Server.h 선언 주석(정답지 피해 함수의 참/거짓 · 줄마다 대조)
bool Server::HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                        const std::string& by, uint8_t dtype, const AttackSrc* src, HitterRef hitter) {
    if (t.none()) return false;
    // ⚠ 정답지는 0 피해도 피해 함수를 끝까지 부르고 대개 참이다(맵 불 짝수 프레임 — 위 1-d). 우리는 여기서 멈추므로 참으로 둔다
    if (dmg <= 0) return true;
    // 진단 임프(`--debug-own-pet`)는 맞지 않는다 — 판정 사진을 찍기 전에 몬스터에게 죽었다(09-13, 갱신에 프레임 1920~2010 만 실림 — 약 3초).
    if (debugOwnPetNetCode_ != 0 && !debugPetWild_ && t.pet >= 0 &&
        pets_[static_cast<size_t>(t.pet)].netCode == debugOwnPetNetCode_) return true;   // 진단 — 무적처럼 참
    // ★ **닿은 피해를 여기서 센다.** `hits_` 는 「투사체가 명중한 횟수」만 세는데,
    //   판정 요약이 그것을 「맞음」이라 찍어 왔다. 그래서 근접으로 피해가 122건 오간
    //   실클라 런이 「맞음 0」으로 나와 **싸움이 안 붙은 것으로 오판**했다(09-10).
    //   갈라 세야 「안 맞았다」와 「투사체만 안 맞았다」가 구분된다.
    ++hurts_;
    ++damageStops_[t.pet < 0 ? "→사람갈래" : "→크리처갈래"];
    const bool byWild = by.empty();                        // 주인 없는 것이 때림
    const bool quest = (questTotal_ > 0 || questStage_ > 0);   // gameFlags 4096

    if (t.pet < 0) {
        auto vi = sessions_.find(t.addr);
        if (vi == sessions_.end()) { ++damageStops_["세션없음"]; return false; };
        Session& s = vi->second;
        // ── nox_server_handler_PlayerDamage_4E17B0 ──
        if (!s.inGame || s.respawnAtFrame != 0) { ++damageStops_["죽음·미입장"]; return false; }   // +16 & 2 · DEAD(`GAME3_3.c:122~124` 0)
        if (s.buffs.Has(23)) { ++invulnBlocked_; ++damageStops_["무적"]; return true; }   // 버프 23(`:125~129` 1)
        if (s.observer) { ++damageStops_["관전"]; return false; }        // 3680 & 1(`:161~163` 0)
        // ── 죽음 알림 기록(+304 갈래 · +300 종류, 묶음 3-d · `GAME3_3.c:164~246`) — 부를 때마다 0 에서 다시 ──
        //   때린 이(a2)가 있으면: 무기·투사체(a3)가 있고 때린 이와 다르면 갈래 1·그 종류 /
        //   없고 피해 종류가 10(맨손)·2(망치)면 갈래 1·때린 이의 종류(사람 713 · 몬스터 그 종류). 막혀도 이 기록은 남는다
        s.killKind = 0;
        // ── 되받아치기 방패(27, `GAME3_3.c:173~204`) — 때린 것(a3 없으면 a2)이 앞쪽이면 ──
        //   투사체면 되받아치고(projectileReflect · 주인 바꿈) 막음 · 아니면 종류 16·17(맨손 마법·?)만 막음 · 종류 16 이면 점 효과 132
        //   ⚠ 옛 자리표시 투사체(shots_)는 오브젝트가 없어 되받아치기 없이 막기만 — 진짜 투사체는 4-c
        if (s.buffs.Has(27)) {
            // v9 = a3 ? a3 : a2 — 무기·투사체 자리(src), 없으면 때린 사람 자리
            float vx9 = 0, vy9 = 0;
            bool have9 = false, missile9 = false;
            if (src) { vx9 = src->x; vy9 = src->y; have9 = true; missile9 = src->missile; }
            else if (!by.empty() && sessions_.count(by)) { vx9 = static_cast<float>(sessions_.at(by).posX); vy9 = static_cast<float>(sessions_.at(by).posY); have9 = true; }
            if (have9 && (missile9 || dtype == 16 || dtype == 17) &&
                (TestTwoPointsAndDirection(static_cast<float>(s.posX), static_cast<float>(s.posY), s.facingPrev, vx9, vy9) & 1)) {
                if (dtype == 16) {
                    const uint16_t fx = static_cast<uint16_t>(static_cast<int>(s.posX)), fy = static_cast<uint16_t>(static_cast<int>(s.posY));
                    const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_VIOLET_SPARKS), static_cast<uint8_t>(fx), static_cast<uint8_t>(fx >> 8),
                                             static_cast<uint8_t>(fy), static_cast<uint8_t>(fy >> 8) };
                    SendFxAllCli(static_cast<float>(s.posX), static_cast<float>(s.posY), msg, sizeof(msg), out);
                }
                { ++damageStops_["정면막기"]; return false; }            // 소리 122 → 10(`:191·202` 0)
            }
        }
        if (src) {
            if (src->obj) {
                s.killKind = 1;
                s.killType = src->objType;
            } else if (dtype == 10 || dtype == 2) {
                s.killKind = 1;
                s.killType = src->byMonsterType ? src->byMonsterType : kPlayerType;
            }
        }
        // ── 막기(묶음 3-e2 · `GAME3_3.c:206~330`) — 때린 것이 있고 · 종류 15 아님 · 주먹/유성/독구름 아님 · **앞쪽**이면 ──
        //   ① 방패(ArmorEquip 0x3000000)를 **올린 상태(16)** 면 전기(9·17) 말고 다 막는다(M_LABEL_78) — 투사체는 되받아친다
        //   ② 아니면 대검(0x400): 투사체·종류 0·11 을 가만(13)·막기(18~20) 중에 막는다 → 18~20 중 하나로
        //   ③ 아니면 지팡이(0x7FF8000): 투사체 아닌 종류 0·11 을 같은 상태에서 막는다 → 21
        //   ⚠ 되받아치기(projectileReflect)·막기 소리·막을 때 아이템 닳기는 투사체 4 · 소리 10 · 아이템 5
        if (src && dtype != 15 &&
            (TestTwoPointsAndDirection(static_cast<float>(s.posX), static_cast<float>(s.posY), s.facingPrev, src->x, src->y) & 1)) {
            const bool shield = (ArmorEquipOf(s) & 0x3000000) != 0;
            const uint8_t st = s.moveState;
            const bool guardState = (st == 13 || st == 18 || st == 19 || st == 20);
            if (st == 16 && shield) {
                if (dtype != 9 && dtype != 17) {
                    std::printf("[막기] 프레임 %u %s 방패로 막음(종류 %u · 피해 %d)\n", frame_, s.addr.c_str(), dtype, dmg);
                    std::fflush(stdout);
                    { ++damageStops_["방패막기"]; return false; }        // `:338` 0
                }
            } else {
                const uint32_t weap = WeaponEquipOf(s);
                bool toArmor = false;                      // LABEL_120 으로
                if (weap & 0x400) {
                    if (src->missile || dtype == 0 || dtype == 11) {
                        if (!guardState) {
                            toArmor = true;
                        } else {
                            PlayerSetState(s, static_cast<uint8_t>(18 + NextRandom(3)), frame_);   // randomInt(18, 20)
                            std::printf("[막기] 프레임 %u %s 대검으로 막음 → 상태 %u\n", frame_, s.addr.c_str(), s.moveState);
                            std::fflush(stdout);
                            { ++damageStops_["대검막기"]; return false; }   // `:288` 0
                        }
                    }
                }
                if (!toArmor && (weap & 0x7FF8000) && (dtype == 0 || dtype == 11) && !src->missile && guardState) {
                    PlayerSetState(s, 21, frame_);
                    std::printf("[막기] 프레임 %u %s 지팡이로 막음\n", frame_, s.addr.c_str());
                    std::fflush(stdout);
                    { ++damageStops_["지팡이막기"]; return false; }      // `:322` 0
                }
            }
        }
        // 방어 — 종류마다 방어값(+228)을 먹는 정도가 다르고, 깎인 피해의 소수부는 +84 에 이월된다
        const float armor = ArmorValueOf(s);   // ★ 09-29 — 입은 갑옷의 합(`53E300`) · 전에는 직업 표
        auto carry = [&](float f) {
            const float a3 = static_cast<float>(static_cast<double>(f) + static_cast<double>(s.dmgRemainder));
            const int v = static_cast<int>(a3);            // nox_float2int = (int)
            s.dmgRemainder = static_cast<float>(static_cast<double>(a3) - static_cast<double>(v));
            return v;
        };
        int hp;
        switch (dtype) {
            case 0: case 3: case 7: case 8: case 10: case 11:
                hp = carry(static_cast<float>((1.0 - static_cast<double>(armor)) * static_cast<double>(dmg)));
                break;
            case 2:
                hp = carry(static_cast<float>((1.0 - static_cast<double>(armor) * 0.5) * static_cast<double>(dmg)));
                break;
            case 9: case 17:
                // sub_4E2220 — 1 + 금속 갑옷 몫. 기본 장비엔 금속이 없어 1(아이템 5 에서 채움)
                hp = carry(static_cast<float>(1.0 * static_cast<double>(dmg)));
                break;
            default:                                       // 1·12·4·5·6·14·15·16 · 그 밖 — 그대로
                hp = dmg;
                break;
        }
        // 기록이 비었으면 갈래 2·피해 종류 — 정답지 switch 가 이름 붙인 종류(0~12·14~17)만 적는다(13·18 넘는 것은 default 라 안 적음)
        if (s.killKind == 0 && (dtype <= 12 || (dtype >= 14 && dtype <= 17))) {
            s.killKind = 2;
            s.killType = dtype;
        }
        if (dmg > 0 && hp == 0) hp = 1;                    // `v33 > 0 && !a1`
        if (quest) {                                       // × sub_4E40B0(blob 202032 = 1.0)
            const int v44 = hp;
            hp = static_cast<int>(static_cast<float>(1.0 * static_cast<double>(hp)));
            if (v44 > 0 && hp < 1) hp = 1;
        }
        // ── nox_xxx_damageDefaultProc_4E0B30(`GAME3_2.c:7017`) ──
        if (s.buffs.Has(23)) { ++invulnBlocked_; ++damageStops_["무적2"]; return true; }   // 무적(소리 71 → 10 · `GAME3_2.c:7064~7069` 1)
        // 편 거르기 — 친선 피해 설정(게임플레이 깃발 1, 기본 꺼짐)이 꺼져 있으면 때린 쪽 주인이 적이 아닐 때 무시.
        //   단 자기가 자기를 때린 것은 퀘스트가 아니면 들어간다(`a1 != v12 || gameFlags 4096`).
        if (!OwnersEnemy(s.addr, false, by, byWild && src && src->rootIsMonster) && (by != s.addr || quest)) { ++damageStops_["편아님"]; return true; };   // `:7100` 1
        ++damageStops_["편검사통과"];
        const Target attacker = DamageAttacker(by, src);
        DamageShockBack(Target{ s.addr, -1 }, s.buffs, attacker, src, out);   // 감전(22) 되갚기
        hp = DamageProtect(s.buffs, dtype, hp);            // 불·전기 보호(버프 17·20 + 아이템 5)
        // 맞으면 투명(0)이 풀린다 — 때린 이가 없으면 종류 12 일 때만(+528/+532 때린 자리는 몬스터 AI 7)
        if (!attacker.none() || dtype == 12) BuffOff(Target{ s.addr, -1 }, 0);
        // ⚠ 입은 것 방어 효과 itemApplyDefendEffect2 · 무기 앞 피해 효과 itemApplyPreDamageEffect(아이템 5)
        s.lastDamageType = dtype;                          // +524(`GAME3_2.c:7234`)
        // ★ 09-23 nine-defects 4-c2 — 맞는 소리(`:7251~7267` → 사람은 thing.bin `DAMAGESOUND = PlayerDamageSound`) — +524·+536 뒤 · 흡혈 앞
        PlayerDamageSound(s, hitter);
        DamageVampirism(attacker, Target{ s.addr, -1 }, hp, out);   // 흡혈(13)
        // ⚠ 공 들고 맞음(gameballOnPlayerDamage 8) · 퀘스트/코옵 sub_4FB050(8) · 몬스터 원한 sub_532880(7)
        // 한 대에 20 넘게 맞으면 움찔(상태 30 · 공격 중 1 · 방패 올리는 중 15 는 아님)
        if (hp >= 20 && s.moveState != kPlayerStateAttack && s.moveState != 15) PlayerSetState(s, 30, frame_);
        // ★ 마법 방패(26) — 정답지 `damageDefaultProc` LABEL_137 자리.
        //   독(5)은 안 막고, 죽음(15)을 자기가 자기에게 건 것도 안 막는다.
        if (s.buffs.Has(26) && dtype != 5 && !(dtype == 15 && by == s.addr)) {
            ShieldReduceDamage(Target{ s.addr, -1 }, hp, dtype, src, out);
            if (hp == 0) { ++damageStops_["마법방패"]; return false; }    // 막았다 — 아예 안 맞은 것이 된다(`:7325~7326` 0)
        }
        // +520 마지막으로 때린 것(`GAME3_2.c:7229~7233`) — a3 가 있으면 그것(주인 = a2), 없으면 a2. 사슬의 사람 = `by`(소환수면 주인)
        Session::LastHit hit;
        if (src) {
            const bool a2Monster = src->byMonsterType != 0;
            const bool a2Player = !a2Monster && !byWild;
            if (src->obj) {
                hit.cls = 3;
                hit.ownerCls = a2Monster ? 2 : (a2Player ? 1 : 0);
            } else {
                hit.cls = a2Monster ? 2 : (a2Player ? 1 : 0);
            }
            hit.monsterType = src->byMonsterType;
            hit.player = by;
        }
        // ★ 2026-09-15 7-c — `src` 가 없으면 **빈 hit 를 넘기지 않는다.**
        //   전에는 빈 것을 넘겨 `Damage` 가 `if (hit)` 가지를 타면서
        //   **때린 사람(`by`)이 통째로 사라졌다** — 죽음 알림의 죽인이도 늘 0 이었다.
        //   정답지 `GAME3_2.c:7229~7233` 은 **a3 가 없으면 a2**(때린 이)를 넣는다 —
        //   그 자리가 `Damage` 의 else 가지(`cls = 1 · player = by`)다
        ++damageStops_["사람갈래끝→Damage"];
        NoteMonsterStruck(src);                            // ★ 09-29 B2 — `sub_532880`(`GAME3_2.c:7301~7315`) — 사람을 때린 몬스터의 도발 시계
        Damage(t.addr, hp, out, by, src ? &hit : nullptr);   // unitDamageClear_4EE5E0
        return true;                                       // `:7330` 1
    }

    const size_t i = static_cast<size_t>(t.pet);
    // ★ 조각 J — 크리처 경로도 사람 경로와 **짝을 맞춰** 이탈구에 이름을 붙인다.
    //   무적 관문이 4곳인데 계수기는 3곳이었다 — 빠진 하나가 여기였고,
    //   그 옆 일곱도 전부 침묵이었다. 「소환수가 왜 안 맞나」를 물을 눈이 없었다.
    if (i >= pets_.size()) { ++damageStops_["칸밖(소환수)"]; return false; };
    Creature& k = pets_[i];
    const auto tb = typeBodies_.find(k.type);
    const PhysBody* body = (tb != typeBodies_.end()) ? &tb->second : nullptr;
    int amount = dmg;
    // ── 몬스터 피해 함수(thing.bin DAMAGE) ──
    if (body) {
        switch (body->damageFn) {
            case kDmgMechGolem: if (dtype == 9 || dtype == 17) amount = 2 * dmg; break;   // sub_4E24E0
            case kDmgFlammable: if (dtype == 1 || dtype == 12 || dtype == 7) amount = 9999999; break;   // 4E2520
            case kDmgBlackPowder:                                                          // 4E2560
                if (dtype != 1 && dtype != 12 && dtype != 2 && dtype != 0) { ++damageStops_["화약아님(소환수)"]; return false; }   // `GAME3_3.c:680~681` 0
                amount = 999999;
                break;
            default: break;                                // Default · Stone · Skeleton(막기는 AI 6) · Player
        }
    }
    // ── nox_xxx_damageDefaultProc_4E0B30 ──
    if (k.buffs.Has(23)) { ++invulnBlocked_; ++damageStops_["무적(소환수)"]; return true; }   // 무적(소리 71 → 10 · `:7064~7069` 1)
    // ⚠ 몬스터 +2188·불 종류 +1440 깃발 · 죽은 좀비 갈래는 몬스터 AI(7)
    // ★ 09-20 — 때린 쪽 **뿌리가 몬스터인가**를 `src->byMonsterType` 으로 가른다.
    //   주인 없는 덫 화살은 몬스터가 아니므로 맵 몬스터에게 **적**이다(정답지 `object.go:1740`)
    const bool byMonsterRoot = byWild && src && src->rootIsMonster;
    if (!OwnersEnemy(k.owner, k.owner.empty(), by, byMonsterRoot)) { ++damageStops_["편아님(소환수)"]; return true; }   // 편 거르기(자기가 자기를 때리는 경우는 없음 · `:7100` 1)
    const Target attacker = DamageAttacker(by, src);
    DamageShockBack(t, k.buffs, attacker, src, out);       // 감전(22) 되갚기 — 되갚는 이는 사람(무기)이라 소환수 목록은 안 바뀐다
    int v15 = dtype;
    if (body) {
        const uint32_t sub = body->subclass;
        if ((sub & 0x200) && v15 == 5) { ++damageStops_["독면역(소환수)"]; return true; }  // 독 면역(`:7130~7131` 1)
        if (sub & 0x400) {                                 // 불 면역 — 1·12 무시 · 7 은 절반
            if (v15 == 1 || v15 == 12) { ++damageStops_["불면역(소환수)"]; return true; }   // `:7134~7135` 1
            if (v15 == 7) amount /= 2;
        } else if (sub & 0x800) {                          // 전기 면역
            if (v15 == 9 || v15 == 17) { ++damageStops_["전기면역(소환수)"]; return true; }   // `:7142~7146` 1
        }
    }
    amount = DamageProtect(pets_[i].buffs, static_cast<uint8_t>(v15), amount);   // 불·전기 보호(버프 17·20 + 아이템 5)
    if (!attacker.none() || v15 == 12) BuffOff(t, 0);      // 맞으면 투명 풀림
    k.lastDamageType = static_cast<uint8_t>(v15);          // ★ 4-c3b2b — +524(`GAME3_2.c:7234` — 면역 관문 뒤 · 맞는 소리 앞)
    // ★ 09-23 nine-defects 4-c3a — 맞는 소리(`GAME3_2.c:7251~7267`) — 몬스터는 DAMAGESOUND 가 없어 기본 `532E20`(thing.bin 에
    //   `DAMAGESOUND` 를 적은 종류는 NewPlayer 하나뿐 — 실측). 흡혈(`:7269`) 앞
    PetDamageSound(k, static_cast<uint8_t>(v15), hitter);
    DamageVampirism(attacker, t, amount, out);             // 흡혈(13)
    // ⚠ 마법 방패(26) 줄이기(4-d) · 몬스터 원한(7)
    k.lastDamageFrame = frame_;                            // +536(`GAME3_2.c:7236` — 몬스터도 같은 자리)
    // ★ 09-29 B2 — 같은 자리(`:7229~7246`) — +520 마지막 때린 쪽(주인 유닛으로 푼다) · +528 때린 것의 지난 자리 · 몬스터면 INJURED(0x200)
    //   · 때린 것 자리 — 발사체 등 때린 것(a3)이 있으면 그 지난 자리(`src`) · 없으면 때린 이 자리 · 둘 다 없으면 (0,0)(`:7185~7212`)
    k.injured = true;
    k.lastAttacker = Creature::SeenRef{};
    if (!attacker.addr.empty()) k.lastAttacker.addr = attacker.addr;
    else if (attacker.pet >= 0 && static_cast<size_t>(attacker.pet) < pets_.size()) k.lastAttacker.petNet = pets_[static_cast<size_t>(attacker.pet)].netCode;
    {
        float fx = 0, fy = 0;
        if (src) { fx = src->x; fy = src->y; }
        else if (!TargetXY(attacker, fx, fy)) { fx = 0; fy = 0; }
        k.hitFromX = fx; k.hitFromY = fy;
    }
    ++b2Stat_[static_cast<size_t>(B2Stat::Injured)];
    NoteMonsterStruck(src);                                // `sub_532880`(`:7301~7315`) — 때린 몬스터의 도발 시계
    // unitDamageClear_4EE5E0
    if (k.health > amount) {
        k.health = static_cast<uint16_t>(k.health - amount);
        // ★ R4 — 불려 나온 것의 체력이 바뀌었다. **주인에게만** 흘러간다
        //   (`FlushPending` 이 `IsEnemyTo` 로 거른다 — 남의 소환수는 적이라 안 간다).
        MarkDirty(k.netCode, PendingReport::What::UnitHealth);
        ++damageStops_["깎임(소환수)"];
        return true;
    }
    KillPet(i, out);
    return true;                                           // `:7330` 1
}

bool Server::Damage(const std::string& addr, int amount, std::vector<OutPacket>& out,
                    const std::string& by, const Session::LastHit* hit, bool ignoreInvuln) {
    auto it = sessions_.find(addr);
    if (it == sessions_.end()) { ++damageStops_["세션없음2"]; return false; }
    Session& s = it->second;
    // ★ 조각 I — **여기가 빠져 있던 이탈구다.** `amount <= 0` 은 「방어가 다 깎았다」는 뜻인데
    //   세지 않아 「안 맞았다」와 구별이 안 됐다. 실측에서 화살 14대 중 13대가 여기로 샜다.
    if (!s.inGame || s.respawnAtFrame != 0) { ++damageStops_["죽음·미입장2"]; return false; }
    if (amount <= 0) { ++damageStops_["방어로0"]; return false; }
    // ★ **부활 직후 무적** — 살아나자마자 또 죽는 것을 막는다.
    //
    //   `kRespawnInvulnTicks`(150 = 5초)는 ◑ 오픈녹스 `playerRespawn` 재구현 값이다.
    //   전에는 **상수만 있고 아무도 안 봤다**(`GameBalance.h:372`, 참조 0건) — 그래서
    //   스폰 자리에 서 있던 사람이 되살아나는 즉시 다시 맞았다.
    //
    //   ⚠ 「없음」을 프레임 0 으로 나타내지 않는다 — 0 은 진짜 프레임이라 첫 틱에
    //     되살아난 사람이 영영 무적이 된다. 오늘 이 함정을 두 번 밟았다(`closing`·`hasAim`).
    if (!ignoreInvuln && s.buffs.Has(23)) {                // 버프 23(4-a4) · unitDamageClear(DEATH 버프)는 안 본다
        ++invulnBlocked_;
        ++damageStops_["무적3"];
        return false;
    }
    // +520 마지막으로 때린 것(3-d) — 피해 함수가 넘긴 것, 없으면 `by`(사람 · 무기 없음)로
    if (hit) {
        s.lastHit = *hit;
    } else {
        s.lastHit = Session::LastHit{};
        if (!by.empty()) { s.lastHit.cls = 1; s.lastHit.player = by; }
    }

    const int before = s.health;
    int after = before - amount;
    if (after < 0) after = 0;
    s.health = static_cast<uint16_t>(after);
    // ★ R4 — 체력이 바뀌었다. 「알려야 할 것」에 올린다(보낼지는 틱 끝에서 가른다).
    if (after != before) MarkDirty(s.netCode, PendingReport::What::UnitHealth);
    // ★ 조각 I — 깎인 것도 센다. **이탈구 합 + 깎임 = 피해** 가 되어야 셈이 닫힌다.
    //   안 닫히면 내가 이탈구를 빠뜨린 것이다(= 계측이 아직 반쪽이라는 지문).
    ++damageStops_[after != before ? "깎임" : "0으로깎임"];

    // ①② 09-14 갱신 알림 정답지화 — `42` 증감·`43` 절대값을 여기서 곧바로 보내던 것을 걷어냈다.
    //    `42` 는 갱신 스트림이 **보는 사람마다** 마지막 피해(+536) 뒤 2틱 넘어 **음수만** 보낸다(ReportHealthDeltaTo · Packet0)
    //    `43` 은 다음 틱 PlayerReportAnything 이 +10 과 다를 때 보낸다(netSendPlrHealthToTeam_4D86E0 · Packet1 · 본인만)
    s.lastDamageFrame = frame_;                            // +536 = gameFrame()(`GAME3_2.c:7236`)

    if (s.health > 0) return false;

    // ★ 죽음 알림 `a9 0e` — diePlayer_54D2B0 가 소리·DEAD 깃발보다 먼저 보낸다(`GAME5.c:5051`, 묶음 3-d)
    SendKillInform(s, out);

    // ★ 7-c — 알림 바로 뒤가 점수 셈 자리다(`GAME5.c:5051` 알림 → `:5078` 셈).
    //   이 서버는 늘 아레나(깃발 256)라 `playerUpdateScore_54D980` 갈래만 탄다
    DeathScoring(s, out);   // 8-f — 모드로 갈린다(전에는 늘 아레나였다)

    // ★ 09-28 B4b — 퀘스트가 아니면 가방을 몸 둘레에 떨군다(`GAME5.c:5093~5095` — 점수 셈 뒤 · 죽음 알림 `netNotifyPlayerDied` 앞).
    //   정품 녹음 18:55:55.9 도 죽은 쪽에 `4d` 넷 → `e8` 차례다. 사용자 결정 09-28 「정할 것 셋 다 원본대로」
    //   DEAD 깃발(+16 0x8000)은 정답지가 이보다 먼저 세운다(`:5065~5067`) — 벗을 때 남에게 안 알리는 갈래가 그 깃발을 본다(`DropCarriedToWorld` dead)
    if (!(questTotal_ > 0 || questStage_ > 0)) DropAllItems(s, out);

    // ③ 죽음 — 껍데기 안. 실측 `e8 d8 02`.
    //    ★ **모두에게.** 남이 쓰러지는 것을 봐야 판이 판이 된다.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::PLAYER_DIED);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
    }
    // ★ 2026-09-13 묶음 3-c — 죽음은 **상태**다(diePlayer_54D2B0 · `GAME5.c:5066~5068`: DEAD 깃발 → 상태 3).
    //   전에는 45틱 뒤 저절로 되살렸다(자리표시). 정답지는 상태 3 → 30틱 → 4 → 15틱 → **클릭해야** 부활(object_update.go:242~275).
    //   respawnAtFrame 은 이제 「죽은 프레임」(DEAD 깃발 겸) — 0 이면 산 것.
    s.respawnAtFrame = frame_ ? frame_ : 1;
    PlayerSetState(s, 3, frame_);
    DropFlag(addr, out);   // 들고 있었으면 그 자리에 떨군다
    DropBall(addr, out);   // 8-d — 공도 같이 떨군다
    // `GAME5.c:5098~5104` — setUnitBuffFlags(0) · 능력 끄기(7-d 에서 옮김) · +3600 = 0 · playerCancelSpells · unitClearBuffs
    SetBuffFlags(Target{ addr, -1 }, 0);
    s.assistSet = false;
    AbilityCancelAll(s, out);                              // 7-d — `GAME5.c:5100` 능력 끄기(`playerCancelAbils_4FC180`)
    PlayerCancelSpells(addr, out);                         // playerCancelSpells_4FEAE0(`GAME5.c:5103`, 4-a3)
    ClearBuffs(Target{ addr, -1 });
    // ⚠ 옮기지 않은 것 — 죽는 소리(10)
    //   · 모드별 점수 정답지(8 — 지금은 아래 우리 점수) · 능력 끄기(7)
    //   (09-28 B4b — 가진 것 전부 떨구기는 위 `DropAllItems` 로 옮김)

    // ④ 판 끝 판정 — 묶음 8-c 가 정답지 규칙으로 옮겼다(아래 `RoundCheckLimit`)
    RoundCheckLimit(out);
    return true;
}

// ── 묶음 8-c 판 끝 알림 ───────────────────────────────────────────────
//
// 정답지 `GAME4_1.c:198~221`(점수 한도로 끝나는 자리) ·
//        `nox_xxx_netSendDMWinner_4D8B90`·`netSendDMTeamWinner_4D8BF0`(`GAME3_2.c:4293·4313`).
//
// ⭕ 규칙(정답지 그대로) —
//   ① **협동팀(깃발 512)이면 판이 안 끝난다**
//   ② 한도는 `servGamedataGet(모드 번호)` — ⭕ 우리는 접속 때 그 값을 **와이어로 보내고 있었다**
//      (묶음 8-a 의 `af[18] = 15`). 그래서 `GameSettings::limitValue` 를 쓴다.
//      ⚠ 전에는 **우리가 지어낸 `kArenaKillLimit`(10)** 이었다 — 실측값으로 바꿨다
//   ③ 한도가 0 이면 안 끝난다
//   ④ **팀을 먼저 본다.** 한 팀이라도 한도에 닿으면 `0x59`(팀 승자)를 보내고 끝
//   ⑤ 아니면 사람을 본다. 관전자는 빼고, 닿은 **첫 사람**에게 `0x58`(혼자 승자)
//
// ⭕ 두 와이어의 짜임은 같다 — `<머리> <승자 번호 u16> <까닭 u8> <프레임 u32>` **8바이트**.
//   · 승자 번호 0 이면 무승부(팀 쪽은 "HL_Tie", 사람 쪽은 "DM_Tie")
//   · 까닭 1 이면 「시간 초과」를 앞에 붙인다. **점수 한도로 끝날 때는 0**(정답지가 0 을 넘긴다)
//   · 프레임은 **맵을 받은 프레임보다 커야** 클라가 안 버린다(`받은 값 <= 저장값`이면 통째로 버림)
void Server::SendRoundWinner(uint8_t op, uint16_t who, uint8_t reason, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        uint8_t msg[8];
        msg[0] = op;
        msg[1] = static_cast<uint8_t>(who);
        msg[2] = static_cast<uint8_t>(who >> 8);
        msg[3] = reason;
        PutU32(msg + 4, o.tickFrame);   // 받는 사람마다 그 사람의 시계
        EmitSeq(o, msg, sizeof(msg), out);
    }
}

void Server::RoundCheckLimit(std::vector<OutPacket>& out) {
    if (roundOver_) return;
    // ⭕ 묶음 8-g 에서 정답지를 다시 읽고 고친 것 — `checkVictory_509A60` 은 **1024(탈락전)를 먼저 보고
    //   거기서 되돌아간다**(`GAME4_1.c:156~195`). 즉 탈락전은 **한도 판정을 아예 안 탄다**.
    //   탈락전의 끝은 `CheckVictorySurvivors`(살아남은 쪽 하나)와 시간 초과뿐이다
    if (gameSettings_.gameFlags & 0x400u) return;        // ⓪ 탈락전(1024)
    if (gameSettings_.gameFlags & 0x200u) return;        // ① 협동팀(512)
    const int32_t limit = static_cast<int32_t>(gameSettings_.limitValue);
    if (limit == 0) return;                              // ③
    for (const Team& t : teams_) {                       // ④ 팀이 먼저다
        if (t.score < limit) continue;
        roundOver_ = true;
        restartAtFrame_ = frame_ + kRoundRestartTicks;
        SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_TEAM_WINNER), t.id, 0, out);
        return;
    }
    for (auto& kv : sessions_) {                         // ⑤ 그다음 사람
        Session& o = kv.second;
        if (!o.inGame || o.observer) continue;           // +3680 & 1
        if (o.score < limit) continue;
        roundOver_ = true;
        restartAtFrame_ = frame_ + kRoundRestartTicks;
        SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER), o.netCode, 0, out);
        return;
    }
}

// 판 시계 — 짜임은 실측이다(PROTOCOL.md 「판 시계」 절).
//
//   d3 | 번호 4 | 남은 시간 4(밀리초, 부호 있음) | 지금 틱 4
//
// 한도가 없으면 남은 시간이 **음수로 계속 내려간다** — 실측이 그랬다.
// 지금 누가 어디 있나 — 한 줄로. 하네스가 이 줄을 읽어 걸어갈 각을 잰다.
std::string Server::PositionsLine() const {
    std::string s = "[자리]";
    char buf[96];
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame) continue;
        // ★ 체력·마나를 같이 찍는다 (2026-09-11). 안 찍어서 **마나가 얼마인지 아무도 몰랐고**,
        //   화면의 빈 마나칸을 보고 「클라에게 안 알린 탓」이라고 잘못 짚었다.
        //   시전은 `cost > s.mana` 면 **카운터도 안 올리고 조용히 안 일어난다** —
        //   그래서 「주문이 안 나간다」와 「마나가 없다」가 로그에서 구분이 안 됐다.
        std::snprintf(buf, sizeof(buf), " 사람 %u=%d,%d,%u,%u 체력%u/%u 마나%u/%u",
                      (unsigned)o.netCode, o.x, o.y, (unsigned)o.facing, (unsigned)o.team,
                      (unsigned)o.health, (unsigned)o.maxHealth,
                      (unsigned)o.mana, (unsigned)o.maxMana);
        s += buf;
    }
    for (const auto& f : flags_) {
        // 팀 = 지금자리 · 들렸나 · 집자리. 몰이가 「가서 들고 → 집으로」 하려면 셋 다 있어야 한다.
        std::snprintf(buf, sizeof(buf), " 깃발 %u=%d,%d,%u,%d,%d", (unsigned)f.team,
                      (int)f.x, (int)f.y, (unsigned)(f.carrier.empty() ? 0 : 1),
                      (int)f.homeX, (int)f.homeY);
        s += buf;
    }
    for (const auto& k : pets_) {
        std::snprintf(buf, sizeof(buf), " 몸 %u=%d,%d",
                      (unsigned)k.netCode, (int)k.x, (int)k.y);
        s += buf;
    }
    // ★ **바닥에 놓인 물건** — 하네스가 여기까지 걸어가 줍기 조작을 시험한다.
    //   ⚠ 열두 개가 다 나오면 줄이 길어진다. **첫 사람에게 가장 가까운 셋**만 —
    //     「어디로 걸어갈까」에 필요한 것은 그것뿐이다.
    if (!statics_.empty()) {
        int px = 0, py = 0;
        bool have = false;
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            px = kv.second.x; py = kv.second.y; have = true; break;
        }
        if (have) {
            std::vector<std::pair<double, const WorldStatic*>> near_;
            near_.reserve(statics_.size());
            for (const auto& w : statics_) {
                const double dx = w.x - px, dy = w.y - py;
                near_.push_back({ dx * dx + dy * dy, &w });
            }
            const size_t k = near_.size() < 3 ? near_.size() : 3;
            std::partial_sort(near_.begin(), near_.begin() + k, near_.end(),
                              [](const std::pair<double, const WorldStatic*>& a,
                                 const std::pair<double, const WorldStatic*>& b) {
                                  return a.first < b.first;
                              });
            for (size_t i = 0; i < k; ++i) {
                std::snprintf(buf, sizeof(buf), " 물건 %u=%d,%d,%u",
                              (unsigned)near_[i].second->netCode,
                              near_[i].second->x, near_[i].second->y,
                              (unsigned)near_[i].second->type);
                s += buf;
            }
        }
    }
    return s;
}

// 둘이 서로 적인가 — 선언부(`Server.h`)에 정품 규칙과 근거를 적어 두었다.
bool Server::IsEnemyTo(const Session& a, const Session& b) const {
    if (&a == &b) return false;                       // 자기 자신
    // ➖ 퀘스트는 협동이므로 **사람끼리는 아군**으로 본다(우리 값 — 정품 확인 못 함).
    if (questTotal_ > 0 || questStage_ > 0) return false;
    if (a.team != 0 && a.team == b.team) return false;   // 같은 팀
    if (a.team != 0 || b.team != 0) return true;         // 한쪽이라도 팀이 있으면 적
    return true;                                          // 팀 없는 사람끼리 — 아레나는 전원 적
}

bool Server::IsEnemyTo(const std::string& a, const std::string& b) const {
    const auto ia = sessions_.find(a);
    const auto ib = sessions_.find(b);
    if (ia == sessions_.end() || ib == sessions_.end()) return false;
    return IsEnemyTo(ia->second, ib->second);
}

// ★★★ **바뀌었으니 알려라** — 정품 더티 비트 틀(`GAME4_2.c:7136`)을 옮긴 것.
//
//   정품은 오브젝트에 **플레이어별 비트**를 세우는데, 우리는 **세션에 목록**을 쌓는다
//   (까닭은 `Session::pending` 주석). 「누구에게 보낼까」는 **보낼 때** 가른다 —
//   정품도 `netUpdateObjectSpecial` 안에서 그때 `unitIsEnemyTo` 를 묻는다.
//   그 사이 팀이 바뀔 수 있으므로 **미리 거르지 않는다.**
void Server::MarkDirty(uint16_t netCode, PendingReport::What what) {
    if (netCode == 0) return;
    const PendingReport pr{netCode, what};
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        // 같은 것이 이미 있으면 안 넣는다 — 한 틱에 값이 여러 번 바뀌어도 한 번만 보낸다.
        // ⚠ 한 틱 목록은 짧으므로 선형 탐색으로 충분하다.
        if (std::find(s.pending.begin(), s.pending.end(), pr) != s.pending.end()) continue;
        s.pending.push_back(pr);
    }
}

// 쌓인 것을 보내고 목록을 비운다 — 정품의 「보낸 뒤 비트 끄기」에 해당한다.
void Server::FlushPending(std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) { s.pending.clear(); continue; }
        for (const auto& pr : s.pending) {
            // ★★ 마법부여 — `0x5A REPORT_ENCHANT`
            //   ⭕ 짜임 `netReportEnchant_4D8F90`(`GAME3_2.c:4496-4503`):
            //       v3[0] = 90;                                   // 0x5A
            //       *(uint16_t*)&v3[1] = netGetUnitCodeServ(a2);  // 넷코드
            //       *(uint32_t*)&v3[3] = a2[85];                  // **비트 묶음 uint32**
            //       netSendPacket1(a1, v3, 7, 0, 1);              // 총 7 · **중요**
            //   ⭕ 비트 자리 — `(Buffs & (1 << v))`(`client/drawable.go:739`) ·
            //       `ENCHANT_INVULNERABLE = 23`(`server/spells_enchant.go:60`)
            //   ★ 우리는 부활 무적을 이 비트로 알린다. 정품 더티 비트 `0x800000` 자리다.
            if (pr.what == PendingReport::What::Enchant) {
                const Buffs* who = nullptr;                   // 4-a4 — 사람·불려 나온 것의 버프 비트(+340)
                for (const auto& kv2 : sessions_) {
                    if (kv2.second.inGame && kv2.second.netCode == pr.netCode) { who = &kv2.second.buffs; break; }
                }
                if (!who) for (const auto& k : pets_) if (k.netCode == pr.netCode) { who = &k.buffs; break; }
                if (!who) continue;
                const uint32_t bits = who->bits;
                uint8_t msg[7] = {
                    static_cast<uint8_t>(Op::REPORT_ENCHANTMENT),
                    static_cast<uint8_t>(pr.netCode),
                    static_cast<uint8_t>(pr.netCode >> 8),
                    static_cast<uint8_t>(bits),
                    static_cast<uint8_t>(bits >> 8),
                    static_cast<uint8_t>(bits >> 16),
                    static_cast<uint8_t>(bits >> 24),
                };
                EmitSeq(s, msg, sizeof(msg), out);
                ++enchantSent_;
                if (bits & (1u << 29)) ++enchantSentAnti_;   // ★ 09-24 H3 — 반마법 비트가 실린 알림
                continue;
            }
            if (pr.what != PendingReport::What::UnitHealth) continue;   // 나머지는 아직
            // 그 번호의 임자를 찾는다 — **사람과 불려 나온 것 둘 다** 본다.
            uint16_t hp = 0;
            bool found = false;
            for (const auto& kv2 : sessions_) {
                if (kv2.second.inGame && kv2.second.netCode == pr.netCode) {
                    // ⭕ **적이 아닐 때만** 보낸다 — `GAME4_2.c:7152-7157`
                    if (IsEnemyTo(s, kv2.second)) break;
                    hp = kv2.second.health;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (const auto& k : pets_) {
                    if (k.netCode != pr.netCode) continue;
                    // ★★ **불려 나온 것은 「주인이 같을 때만」 아군이다.**
                    //   ⭕ 정품 `IsEnemyTo`(`opennox/src/server/object.go:1717-1721`) —
                    //     `own1 := obj.FindOwnerChainPlayer()` · `own2 := …` ·
                    //     `if own1 == own2 { return false }` — **주인을 따라 올라가** 견준다.
                    //   즉 **내 소환수 체력은 나에게 오고, 남의 것·맵 몬스터는 적이라 안 온다.**
                    //   ➖ 정품의 `SubClass & 0x20`(중립 몬스터) 갈래는 3모드에 안 쓴다.
                    if (k.owner != kv.first) break;      // 남의 것이거나 주인 없는 맵 몬스터
                    hp = k.health;
                    found = true;
                    break;
                }
            }
            if (!found) continue;
            // ⭕ 짜임 — `netReportUnitCurrentHP_4D8620`(`GAME3_2.c:4022-4038`):
            //     [0]=65(0x41) · [1..2]=넷코드 · **[3]=체력 >> 1** · 총 4 · `a6=1`(중요)
            //   체력을 **2로 나눠 한 바이트**에 담는다(최대 510 까지 표현된다).
            uint8_t msg[4] = {
                static_cast<uint8_t>(Op::REPORT_HEALTH),
                static_cast<uint8_t>(pr.netCode),
                static_cast<uint8_t>(pr.netCode >> 8),
                static_cast<uint8_t>(hp >> 1),
            };
            EmitSeq(s, msg, sizeof(msg), out);
            ++unitHealthSent_;
        }
        s.pending.clear();
    }
}

// 퀘스트를 연다 — **스테이지 1 부터.** `count` 는 이제 안 쓴다(스테이지 표가 정한다).
//
// ★★ 전에는 「몬스터 N 마리 세우고 다 잡으면 끝」이었다. 정품 NoxQuest 는 웨이브가
//   밀려오고 스테이지가 오른다(사용자 결정, 2026-09-11). 스테이지 표는 `GameBalance.h`.
void Server::StartQuest(int count, int centerX, int centerY, double spread) {
    (void)count;
    questCenterX_ = centerX;
    questCenterY_ = centerY;
    questSpread_ = spread;
    questStage_ = 0;
    questKilled_ = 0;
    roundOver_ = false;
    SpawnQuestStage(nullptr);   // 첫 스테이지 (알림은 판이 뜬 뒤 첫 틱에서)
}

// 지금 스테이지의 몬스터를 세운다. `out` 이 있으면 스테이지 알림도 보낸다.
void Server::SpawnQuestStage(std::vector<OutPacket>* out) {
    if (questStage_ >= kQuestStageCount) return;
    // ⚠ `QuestStage`(타입)는 멤버 함수 `QuestStage()` 와 이름이 겹쳐 클래스 안에서 가려진다.
    //   `auto` 로 받아 그 충돌을 피한다.
    const auto& st = kQuestStages[questStage_];
    // ⚠ 한 웨이브 상한(⭕ gamedata Normal=5). 스테이지 표가 그보다 크면 잘라 세운다.
    const int n = st.count < kWaveMaxActive ? st.count : kWaveMaxActive;
    questTotal_ = n;
    questKilled_ = 0;
    for (int i = 0; i < n; ++i) {
        const double ang = 2.0 * 3.14159265358979 * i / (n > 0 ? n : 1);
        Creature k;
        k.owner.clear();                 // 주인이 없다 — 아무 편도 아니다
        k.aggression = 0.5f;             // ★ 09-29 B2 — 맵 몬스터 공격 성향 기본(objectMonsterInit unit_monster.go:124) · 소환수는 따라와 명령의 0.83
        k.type = st.monster;
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.x = questCenterX_ + questSpread_ * std::cos(ang);
        k.y = questCenterY_ + questSpread_ * std::sin(ang);
        k.health = static_cast<uint16_t>(TuningFor(k.type).health);
        pets_.push_back(k);
    }
    std::printf("[스테이지] %d/%d — %s (%d마리)\n",
                questStage_ + 1, kQuestStageCount, st.label, n);
    std::fflush(stdout);
    if (out) SendNotice(st.label, *out);
}

// 이 사람이 지금 겨누고 있는 쪽(라디안).
//
// 겨눈 자리가 있고 아직 싱싱하면 **거기를 향해**, 아니면 몸이 바라보는 쪽으로.
// ⚠ 겨눈 자리가 제 몸과 겹치면 방향이 안 나온다 — 그때도 몸 방향으로 되돌아간다.
double Server::AimRadians(const Session& s) const {
    const bool fresh = s.hasAim && (frame_ - s.aimAtFrame <= kAimStaleTicks);
    if (fresh) {
        const double dx = s.aimX - s.x, dy = s.aimY - s.y;
        if (dx * dx + dy * dy > 1.0) return std::atan2(dy, dx);
    }
    return s.facing * 3.14159265358979 / 128.0;
}

// 한 사람이 한 말을 **모두에게.** 머리는 서버가 새로 짓는다 —
// 말한 이의 번호와 그때 좌표로. 글자만 받은 것에서 가져온다.
//
// ⚠ 클라가 준 머리를 그대로 쓰면 **남의 번호로 말하기**가 된다.
void Server::BroadcastChat(const Session& who, const uint8_t* text, size_t n,
                           std::vector<OutPacket>& out) {
    if (n == 0 || n > kChatMaxBytes) return;
    std::vector<uint8_t> msg;
    msg.reserve(11 + n);
    msg.push_back(static_cast<uint8_t>(Op::TEXT_MESSAGE));
    msg.push_back(static_cast<uint8_t>(who.netCode));
    msg.push_back(static_cast<uint8_t>(who.netCode >> 8));
    msg.push_back(0x02);                                  // 깃발 = UTF-8 (한 글자 한 바이트)
    msg.push_back(static_cast<uint8_t>(who.x));
    msg.push_back(static_cast<uint8_t>(who.x >> 8));
    msg.push_back(static_cast<uint8_t>(who.y));
    msg.push_back(static_cast<uint8_t>(who.y >> 8));
    msg.push_back(static_cast<uint8_t>(n));
    msg.push_back(0); msg.push_back(0);                   // 미상 2 — 표본 전수가 0
    msg.insert(msg.end(), text, text + n);
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        // ★ 09-30 — 채팅 중계는 묶지 않고 곧바로 — 정답지도 사람마다 `SendUnreliableMsg` 뒤 `conn.Flush()`(`network_server.go:149~151`)
        EmitNow(kv.second, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
        ++bundleStat_[static_cast<size_t>(BundleStat::DirectChat)];
    }
}

// 글자 한 줄 — `0xa8 TEXT_MESSAGE`. 짜임은 실측이고 **문구만 우리 것**이다.
void Server::SendNotice(const char* text, std::vector<OutPacket>& out) {
    const size_t n = std::strlen(text);
    if (n == 0 || n > 200) return;
    std::vector<uint8_t> msg;
    msg.reserve(11 + n);
    msg.push_back(static_cast<uint8_t>(Op::TEXT_MESSAGE));
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        Session& s = kv.second;
        msg.resize(1);
        msg.push_back(static_cast<uint8_t>(s.netCode));
        msg.push_back(static_cast<uint8_t>(s.netCode >> 8));
        msg.push_back(0x02);                       // 깃발 = UTF-8 (그래야 한 글자 한 바이트)
        msg.push_back(static_cast<uint8_t>(s.x));
        msg.push_back(static_cast<uint8_t>(s.x >> 8));
        msg.push_back(static_cast<uint8_t>(s.y));
        msg.push_back(static_cast<uint8_t>(s.y >> 8));
        msg.push_back(static_cast<uint8_t>(n));
        msg.push_back(0); msg.push_back(0);        // 지속 = 0
        msg.insert(msg.end(), text, text + n);
        Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
    }
}

void Server::StartCtf(int redX, int redY, int blueX, int blueY) {
    flags_.clear();
    caps_[0] = caps_[1] = caps_[2] = 0;

    Flag red;
    red.team = 1;
    red.type = kFlagTypeRed;
    red.netCode = nextShotNetCode_++;
    red.homeX = red.x = redX;
    red.homeY = red.y = redY;
    flags_.push_back(red);

    Flag blue;
    blue.team = 2;
    blue.type = kFlagTypeBlue;
    blue.netCode = nextShotNetCode_++;
    blue.homeX = blue.x = blueX;
    blue.homeY = blue.y = blueY;
    flags_.push_back(blue);

    roundOver_ = false;   // 깃발 판을 다시 차릴 수 있게. 전에는 안 되돌려 한 번 끝나면 끝이었다
    AssignTeams();
}

// 팀을 번갈아 나눈다. ⚠ 진짜 녹스가 이렇게 나누는지는 모른다(➖ 우리 값).
//
// ★★ 전에는 여기가 **두 군데 틀렸다** (2026-09-11 감사).
//   ① 「들어온 차례로」라고 주석에 적혀 있었지만 실제로는 `sessions_` 를 그냥 돌았는데,
//      그 열쇠가 **접속 주소 문자열**이라 순서가 **사전순**이었다. 포트 번호가 어떻게
//      뽑히느냐에 따라 A·B 팀이 뒤집혔다 — 같은 시험을 두 번 돌리면 결과가 달랐다.
//   ② **두 명이 다 들어온 그 순간 한 번만** 불렸다. 셋째부터는 `team == 0` 으로 남아
//      `UpdateFlags` 의 팀 검사에 걸려 **깃발 판정에서 통째로 빠졌다.**
//
// 고침 — `joinOrder`(접속한 차례)로 정렬해 나누고, **사람이 들어올 때마다 다시 부른다.**
void Server::AssignTeams() {
    std::vector<Session*> ins;
    for (auto& kv : sessions_) {
        if (kv.second.inGame) ins.push_back(&kv.second);
    }
    std::sort(ins.begin(), ins.end(), [](const Session* a, const Session* b) {
        return a->joinOrder < b->joinOrder;
    });
    uint8_t next = 1;
    for (Session* s : ins) {
        s->team = next;
        next = (next == 1) ? 2 : 1;
    }
}

// 문과 오벨리스크를 매 틱 돌본다 (P5).
//
// ★ **문은 다가가면 열린다** — 클라가 요청을 안 보낸다.
//   근거: 정품 덤프에 문 관련 **클라→서버 메시지가 0건**이고, 서버→클라 `0xb2` 만
//   201건 흐른다. 즉 여는 판단은 서버가 한다.
//   ⚠ 여는 거리와 각 값은 ➖ 우리 값이다(`Server.h` 주석 참조).
//
// ★ **오벨리스크는 옆에 서면 마나를 채운다** — gamedata 에 Regen 키가 0개이고
//   오벨리스크 항목도 없다. 정품 온라인의 마나 회복 수단이 이것으로 보인다.
//   ⚠ 채우는 속도는 ➖ 우리 값. P4 캠페인에서 정품 시계열이 잡히면 그 값으로 고친다.
// ★ 09-20 ⑩ — 판·문·오벨리스크를 와이어에 실을 때 쓰는 번호. 정적 그림이면 윗비트를 세운다.
//   ⚠ 문·오벨리스크는 **화면으로 확인 못 했다** — 판만 갈렸다(확인표 ⑩ 절 「아직 못 본 것」).
uint16_t Server::PropWireCode(const WorldProp& p) const {
    return static_cast<uint16_t>(p.staticLookup ? (p.netCode | 0x8000u) : p.netCode);
}

void Server::UpdateProps(std::vector<OutPacket>& out) {
    if (props_.empty()) return;
    for (auto& p : props_) {
        // ── 묶음 9-c 밟는 판 ─────────────────────────────────────────
        //
        // 정답지 — 닿음 `nox_xxx_collideTrigger_54FCD0`(`GAME5.c:6481`) ·
        //   주기 `nox_xxx_updateTrigger_53B1B0`(`GAME4_3.c:5415`) ·
        //   보내기 `nox_xxx_netSpriteUpdate_518AE0`(`GAME4_1.c:4350~4353`).
        //
        // ★★ **오프코드 이름과 실제가 다르다.** `0xB4`(`PENTAGRAM_ACTIVATE`)를 보내는 자리는
        //   정답지 전체에 하나뿐이고 그 조건은 **종류 == `PressurePlate`** 다.
        //   오각별(`TeleportPentagram`)은 같은 함수의 **다른 갈래**로 `0x39` 를 보낸다.
        // ⭕ thing.bin 실측 — `PressurePlate` 와이어 **734** · **상자 45×45** ·
        //   `CLASS = TRIGGER+IMMOBILE` · `FLAGS = BELOW+ALLOW_OVERLAP`(그래서 **밟고 지나간다**).
        //   맵 실측 **1,849개**(58맵). 보이지 않는 `Trigger`(735)는 **아무것도 안 보낸다**
        if (p.kind == PropKind::Plate) {
            if (!p.enabled) {                          // `[0] &= 0xF6`
                p.trigPressed = false;
                p.trigWasEnabled = false;
                continue;
            }
            // 닿음 — 질량이 있는 것이 판 상자(45×45) 위에 올라와 있나.
            //   ⚠ 갈래·편 마스크(`[11]`·`[12]`·`+52`·`+53`)는 **맵 초기자료**라 우리는 못 읽는다
            if (p.trigState != 5) {
                const double half = kPlateHalf;
                auto on = [&](double ox, double oy, double r) {
                    const double dx = ox - p.x, dy = oy - p.y;
                    const double cx = std::max(-half, std::min(half, dx));
                    const double cy = std::max(-half, std::min(half, dy));
                    const double ex = dx - cx, ey = dy - cy;
                    return ex * ex + ey * ey <= r * r;
                };
                for (auto& kv : sessions_) {
                    const Session& o = kv.second;
                    if (!o.inGame || o.observer) continue;
                    if (o.moveState == 3 || o.moveState == 4) continue;   // 죽은 것은 질량이 없다
                    if (on(o.posX, o.posY, kPlayerRadius)) { p.trigPressed = true; break; }
                }
                if (!p.trigPressed)
                    for (const auto& k : pets_) {
                        if (k.health == 0) continue;
                        if (on(k.x, k.y, kPlayerRadius)) { p.trigPressed = true; break; }
                    }
            }
            // 주기 — `updateTrigger_53B1B0`. `PressurePlate` 는 뗄 때까지 기다리는 값이 **0** 이다
            const uint8_t before = p.animFrame;
            if (!p.trigWasEnabled) { p.trigState = 0; p.trigReleaseFrame = frame_; p.animFrame = 0; }
            if (p.trigState != 0) {
                if (p.trigState == 1 && frame_ > p.trigReleaseFrame && !p.trigPressed) {
                    p.animFrame = 0;                                  // 뗐다
                    p.trigState = p.trigOneShot ? 5 : 0;
                    // 10-b PlaySound: release sound from thing.bin UPDATE arg 2 (v1[10])
                    if (p.sndReleased) PlaySoundAt(p.sndReleased, static_cast<float>(p.x), static_cast<float>(p.y));
                    // ⛔ 스크립트 9(`scriptCallByEventBlock`)·떼는 소리는 안 옮겼다
                }
            } else if (p.trigPressed) {
                p.animFrame = 1;                                      // 눌렸다
                p.trigState = 1;
                p.trigReleaseFrame = frame_;
                // 10-b PlaySound: press sound from thing.bin UPDATE arg 1 (v1[9])
                if (p.sndPressed) PlaySoundAt(p.sndPressed, static_cast<float>(p.x), static_cast<float>(p.y));                          // v3 = 0
                // ⛔ 스크립트 8·누르는 소리는 안 옮겼다
            }
            p.trigPressed = false;                                    // `[0] &= 0xFE`
            p.trigWasEnabled = true;                                  // `[0] |= 8`
            // ⚠ **정답지는 `갱신자료[0] & 1`(눌림 비트)를 싣는데** 그 비트는 주기 끝에 지워진다 —
            //   분배자가 닿음과 주기 사이 어느 자리에서 도는지에 달려 있다. 우리 틀에서는
            //   **애니 칸(0·1)** 을 싣는다. 클라가 쓰는 값이 같다(빛 켜기 + 그림 칸).
            //   ⇒ 정품 화면(④)에서 판이 깜빡이지 않는지 볼 것
            // ⚠ 문 갈래와 같이 **바뀔 때만** 보낸다(정답지는 더러움 비트라 같은 값도 다시 간다)
            if (p.animFrame != before) {
                // ★★ 09-20 ⑩ — **윗비트(0x8000)를 세운다.** 근거는 `WorldProp::staticLookup` 주석.
                //   밟는 판은 `CLASS = TRIGGER+IMMOBILE` 이라 정적 쪽이고, 실측으로 갈렸다
                //   (안 세움 → 「그림 못 찾음」 · 세움 → 「그림 찾음」)
                const uint16_t nc = PropWireCode(p);
                const uint8_t msg[4] = { static_cast<uint8_t>(Op::PENTAGRAM_ACTIVATE),
                                         static_cast<uint8_t>(nc),
                                         static_cast<uint8_t>(nc >> 8),
                                         p.animFrame };
                Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                if (p.animFrame) ++plateOn_; else ++plateOff_;
            }
            continue;
        }

        double best = 1e18;
        Session* nearest = nullptr;
        for (auto& kv : sessions_) {
            Session& s = kv.second;
            if (!s.inGame || s.respawnAtFrame != 0) continue;
            const double dx = p.x - s.x, dy = p.y - s.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; nearest = &s; }
        }
        if (!nearest) continue;

        if (p.kind == PropKind::Door) {
            // ★ 09-22 — **부딪히면 연다**(정답지 `DoorCollide`). 부딪힘 = 두 몸이 겹침이라
            //   거리는 **사람 반지름 + 그 문 종류의 반지름**이다. 종류를 모르면 42(10+32)
            double openAt = kDoorOpenDist;
            {
                const auto tb = typeBodies_.find(p.type);
                if (tb != typeBodies_.end() && tb->second.r > 0.0f)
                    openAt = kBodyRadius + static_cast<double>(tb->second.r);
            }
            // ★ 09-22 — 「문 0」이 **안 열린 것**인지 **근처에 간 적이 없는 것**인지 갈라야 한다.
            //   0 옆에 지나간 양을 같이 읽는다 — 사람이 문에 가장 가까이 간 거리
            {
                const double d = std::sqrt(best);
                if (d < doorNearestDist_) doorNearestDist_ = d;
            }
            // ★★★ 09-22 묶음 9 — **문은 사람이 밀어서 돈다**(정답지 `GAME5.c:2193~2207`):
            //       각속도 += (사람 반지름 − 거리) × **사람 무게**(+120) · 부호는 문선의 어느 쪽인가
            //       그 뒤 용수철 목록(`sub_548B60`)이 걸음마다 각도를 돌린다
            //   ⇒ 「휙 열린다」가 아니라 **밀린 만큼 돈다.** 사용자가 짚은 자리다
            // ⚠⚠ **문 중심이 아니라 「문짝 선분」까지의 거리다.** 여기를 틀리면 문이 떤다 —
            //   중심은 문이 돌아도 안 움직이니 밀림이 **영영 안 끝나고**, 미는 부호가 뒤집혀
            //   각도가 오르내린다(실측: 미세각 216 ↔ 236 왕복).
            //   정답지 `GAME5.c:2143~2166` — 문짝은 문 자리에서 방향표 값 **×2**(길이 32)만큼 뻗은 선분이고,
            //   부딪힘은 **사람 원(반지름 10) 대 그 선분**이다. 문이 비키면 거리가 멀어져 저절로 멈춘다
            {
                const int idx = (static_cast<int>(p.fine) + 160) & 255;
                const double th = idx * (2.0 * 3.14159265358979323846 / 256.0);
                const double dirx = std::floor(std::cos(th) * 16.0 + 0.5);   // 반지름 16 표
                const double diry = std::floor(std::sin(th) * 16.0 + 0.5);
                const double ex = 2.0 * dirx, ey = 2.0 * diry;               // 문짝 끝(×2)
                // 선분 [문자리 → 문짝끝] 위에서 사람에게 가장 가까운 점
                const double px = nearest->posX - p.x, py = nearest->posY - p.y;
                const double len2 = ex * ex + ey * ey;
                double t = (len2 > 0.0) ? (px * ex + py * ey) / len2 : 0.0;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                const double qx = px - t * ex, qy = py - t * ey;
                const double dist = std::sqrt(qx * qx + qy * qy);
                const double depth = kBodyRadius - dist;                     // 사람 반지름 − 거리
                if (depth > 0.0) {
                    p.lastPushFrame = frame_;                                // +44 — 잠김과 상관없이 닿으면 적는다(`GAME5.c:2170`)
                    // ★ 09-29 Z1-A — 밀어서 돌리는 것은 **안 잠긴 문만**(`GAME5.c:2191~2215`):
                    //   팀 문이 닫혀 있고 다른 팀이면 못 민다 — 1초에 한 번 「GateLockedMechanism」(+136)
                    //   그 밖에는 잠김(+1)이 0 일 때만 돈다 · 마법 잠금(+508)은 우리에게 SPELL_LOCK 이 없어 늘 비어 있다
                    const bool closedNow = ((32 * static_cast<int>(p.fine) / 256) & 31) == static_cast<int>(p.angle);
                    if (p.team != 0 && closedNow && nearest->team != p.team) {
                        ++doorTouch_[static_cast<size_t>(DoorTouch::TeamBlocked)];
                        if (frame_ > p.teamMsgFrame) {
                            p.teamMsgFrame = frame_ + kServerTickRate;
                            SendPriMsg(*nearest, "objcoll.c:GateLockedMechanism", out);
                        }
                    } else if (p.lock != 0) {
                        ++doorTouch_[static_cast<size_t>(DoorTouch::SpinLocked)];
                    } else {
                        const double cross = dirx * py - diry * px;
                        const double push = depth * PlayerMassOf(*nearest);
                        p.spin += (cross <= 0.0) ? push : -push;
                    }
                    // ★ 09-29 Z1-A — 닿음 짝(사람, 문) — `collSysAddCollision_548630(사람, 문)`(`GAME5.c:2169`) 뒤
                    //   닿음 처리가 a.collide(b) → b.collide(a) 로 부른다: 사람 쪽(광전사 갈래) → 문 쪽(`collideDoor_4E8AC0`).
                    //   버프 몫(`4FDF90`)은 문이 유닛·생성기가 아니라 할 일이 없다
                    //   ⚠ 정답지는 이 짝을 이동 적분의 닿음 처리에서 부른다 — 우리는 문 갱신에서 곧바로 부른다(같은 틱 · 이동 뒤)
                    const size_t di = static_cast<size_t>(&p - props_.data());
                    const ContactRef doorRef{ 4, std::string(), static_cast<int>(di) };
                    PlayerCollide(*nearest, &doorRef, out);
                    DoorCollide(di, *nearest, out);
                }
            }
            // 용수철 — 각속도로 미세각(0~255)을 돌린다. 한 걸음에 최대 4(정답지 `sub_548B60`),
            //   걸음이 다섯이니 틱당 최대 20. 속도는 걸음마다 0 으로 지운다(정답지도 `+32` 를 지운다)
            const uint8_t coarseBefore = static_cast<uint8_t>((32 * static_cast<int>(p.fine) / 256) & 31);
            const int closedFineI = (static_cast<int>(p.angle) * 8) & 255;
            // ★★ **경첩 한계** — 정답지 `sub_548B60`(`GAME5.c:2248·2258`):
            //     거친각이 `(닫힘 + 12) % 32` 면 **더 안 돈다** · 반대쪽은 `닫힘 − 12`
            //   ⇒ 문은 닫힘에서 **±12 눈금**까지만 열린다. 이 한계가 떨림도 같이 막는다 —
            //     한계에서 서니 미는 부호가 뒤집히는 구간까지 안 들어간다
            //   속도 제한(걸음당 최대 4 · `2×속도+0.5`)도 정답지 그대로다
            //   ⭕ 09-22 — **시험이 본다.** 보통 판에서는 문짝이 먼저 비켜 이 가지에 안 닿아
            //     한때 미검증으로 뒀는데, **경첩 자리에 서면** 문이 돌아도 사람이 선분 끝에
            //     붙어 있어 밀림이 안 끝나고 그때 한계가 일한다 — **미세각 96·거친각 12 에서 딱 선다.**
            //     빼 보면 거친각 19 까지 돌아 **회전문**이 된다(돌연변이로 확인)
            const int limPlus  = (static_cast<int>(p.angle) + 12) & 31;
            const int limMinus = (static_cast<int>(p.angle) - 12 + 32) & 31;
            for (int st = 0; st < kMoveSubSteps && p.spin != 0.0; ++st) {
                int step = static_cast<int>(2.0 * p.spin + 0.5);
                int mag = step < 0 ? -step : step;
                if (mag > 4) mag = 4;
                const int coarseCur = (32 * static_cast<int>(p.fine) / 256) & 31;
                if (p.spin > 0.01) {
                    if (coarseCur == limPlus) break;                 // 경첩 끝
                    p.fine = static_cast<uint8_t>((p.fine + mag) & 255);
                } else if (p.spin < -0.01) {
                    if (coarseCur == limMinus) break;                // 반대쪽 끝
                    p.fine = static_cast<uint8_t>((p.fine - mag) & 255);
                } else {
                    break;
                }
            }
            p.spin = 0.0;
            // 저절로 닫힘 — 민 지 0.5초가 지나면 닫힘 각도 쪽으로 2씩(정답지 `updateDoor` + `sub_548860`)
            {
                const uint8_t closedFine = static_cast<uint8_t>((static_cast<int>(p.angle) * 8) & 255);
                if (p.fine != closedFine && frame_ > p.lastPushFrame + kServerTickRate / 2) {
                    const int diff = (static_cast<int>(p.fine) - static_cast<int>(closedFine) + 256) & 255;
                    p.fine = static_cast<uint8_t>((p.fine + (diff >= 128 ? 2 : -2)) & 255);
                    if (((static_cast<int>(p.fine) - static_cast<int>(closedFine) + 256) & 255) < 2)
                        p.fine = closedFine;                       // 지나치지 않게 딱 맞춘다
                }
            }
            const uint8_t coarseNow = static_cast<uint8_t>((32 * static_cast<int>(p.fine) / 256) & 31);
            const bool want = (coarseNow != p.angle);
            if (coarseNow == coarseBefore) continue;   // 거친각이 안 바뀌면 안 보낸다
            if (want && !p.open) ++doorsOpened_;
            p.open = want;
            // ★★ 09-22 사용자 보고 — **끼익 소리가 없다**(정품 기억과 대조). 실제로 안 보내고 있었다.
            //   정답지 `nox_xxx_updateDoor_53AC50`(`GAME4_3.c:5236·5257`) — 재질로 갈리고 기본은
            //   **열 때 237 · 닫힐 때 239**(쇠 241/243 · 열쇠문 247/248 · 창살 1014/1015).
            //   ⚠ 우리는 아직 재질을 안 읽어 나무 기본만 낸다 — Estate 의 문 둘은 둘 다 나무다
            //   ⚠ 소리는 **닫힘을 떠날 때**와 **닫힘에 닿을 때** 한 번씩이다(정답지 `updateDoor` 의 두 갈래).
            //     각도가 도는 내내 내면 안 된다
            if (coarseBefore == p.angle && coarseNow != p.angle)
                PlaySoundAt(237, static_cast<float>(p.x), static_cast<float>(p.y));
            else if (coarseBefore != p.angle && coarseNow == p.angle)
                PlaySoundAt(239, static_cast<float>(p.x), static_cast<float>(p.y));
            const uint16_t dnc = PropWireCode(p);
            uint8_t full[4] = {
                static_cast<uint8_t>(Op::DOOR_ANGLE),
                static_cast<uint8_t>(dnc),
                static_cast<uint8_t>(dnc >> 8),
                coarseNow,                             // **지금 각도**를 그대로 보낸다
            };
            Broadcast(/*viaSeq=*/false, full, sizeof(full), out);
            // ★ 09-21 — 방금 받은 사람은 「이 상태를 안다」고 적는다(아래 되맞춤이 또 보내지 않게).
            //   ⚠ **세계를 받은 지 1초가 안 된 사람은 안 적는다** — 그 창에서는 클라가 알림을 버린다
            //   (09-20 실측 `RECV … b2 8e 0f 18` 뒤 무반응). 그런 사람은 되맞춤이 한 번 더 보낸다
            {
                const size_t pi = static_cast<size_t>(&p - props_.data());
                for (auto& kv : sessions_) {
                    Session& o = kv.second;
                    if (!o.inGame || o.doorSyncFrom == 0 || frame_ < o.doorSyncFrom) continue;
                    if (o.doorTold.size() != props_.size()) o.doorTold.assign(props_.size(), 0);
                    o.doorTold[pi] = want ? 1 : 0;
                }
            }
        } else {
            // ★★ 09-22 — **마나석은 잔량이 있다.** 정답지 `nox_xxx_updateObelisk_53C580` —
            //     쓸 때   (`:6454`) 틱마다 잔량 1 감소 · 마나 1 증가. 잔량이 1 아래면 더 안 준다(`:6412`)
            //     안 쓸 때(`:6477~6489`) **fps/2 프레임마다** 잔량 1 회복, **최대 50**
            //   ⇒ 가득(50)이면 1.7초 만에 바닥나고, 다시 차는 데 25초.
            //   ⚠ **한쪽만 넣으면 안 된다** — 회복 없이 소모만 넣으면 마나석이 영영 죽는다
            // ★ 09-28 사람 판 — 「마나석 옆에 있으면 소리가 계속 난다」. 정답지는 둘레 50 안에 사람이 **있기만 해도**
            //   회복하지 않는다(`GAME4_3.c:6373` v23 = 0 → `:6472~6474` 회복 건너뜀 — 마나가 가득이든 잔량이 0 이든).
            //   전에는 「줄 게 없으면 회복」이라 잔량 0 인 마나석 옆에서 0.5초마다 1 차고 → 곧바로 주고 → 소리 230 이 되풀이됐다
            //   ⚠ 정답지는 둘레 안 사람을 **모두** 채우고 벽 가림(`mapCheck_537110`)·팀도 본다 — 우리는 가장 가까운 한 사람만(옛 그대로)
            const bool someoneNear = best <= kObeliskDist * kObeliskDist;
            const bool inUse = someoneNear && nearest->mana < nearest->maxMana && p.charge >= 1;
            const uint8_t before = p.charge;
            if (!someoneNear) {
                // 회복 — 0.5초마다 1 (둘레에 아무도 없을 때만)
                if ((frame_ % (kServerTickRate / 2)) == 0 && p.charge < kObeliskFullCharge) ++p.charge;
                if (p.charge != before && (p.charge % 8) == 0) {
                    const uint16_t onc2 = PropWireCode(p);
                    uint8_t ob2[4] = { static_cast<uint8_t>(Op::OBELISK_CHARGE),
                                       static_cast<uint8_t>(onc2), static_cast<uint8_t>(onc2 >> 8), p.charge };
                    Broadcast(/*viaSeq=*/true, ob2, sizeof(ob2), out);
                }
                continue;
            }
            if (!inUse) { ++obeliskNearIdle_; continue; }   // 옆에 있으나 줄 것 없음 — 회복도 소리도 없다
            if (frame_ < p.nextManaFrame) continue;
            p.nextManaFrame = frame_ + kObeliskEveryTicks;
            --p.charge;
            PlayerManaAdd(*nearest, kObeliskManaPerTick);   // playerManaAdd_4EEB80 — `45` 는 다음 틱 PlayerReportAnything(+4 ≠ +6)
            ++obeliskMana_;
            // ★★ 09-22 사용자 보고 — **마나 찰 때 소리가 없다**. 실제로 안 보내고 있었다.
            //   정답지 `nox_xxx_updateObelisk_53C580`(`GAME4_3.c:6461~6464`) — 소리 **230** 을
            //   **`fps/2` 프레임에 한 번**(30틱이면 15프레임 = 0.5초) 마나석 자리에서 낸다.
            //   ⚠ 마나를 주는 주기(`kObeliskEveryTicks`)와 **다른 주기**다 — 그래서 따로 센다
            if (frame_ >= p.nextManaSndFrame) {
                p.nextManaSndFrame = frame_ + (kServerTickRate / 2);
                PlaySoundAt(230, static_cast<float>(p.x), static_cast<float>(p.y));
            }
            // ★ 09-22 — 이제 **지금 잔량**을 보낸다(전에는 늘 가득 0x32). 정답지는 잔량이 8 의
            //   배수가 될 때만 알린다(`:6456` `if (!(*v24 % 8))`) — 매 틱 보내면 와이어가 넘친다
            if ((p.charge % 8) == 0) {
                const uint16_t onc = PropWireCode(p);
                uint8_t ob[4] = {
                    static_cast<uint8_t>(Op::OBELISK_CHARGE),
                    static_cast<uint8_t>(onc),
                    static_cast<uint8_t>(onc >> 8),
                    p.charge,
                };
                Broadcast(/*viaSeq=*/true, ob, sizeof(ob), out);
            }
        }
    }

    // ── 09-21 문 되맞춤 — **나중에 들어온 사람에게도 문 상태가 간다** ─────────────
    //
    // 위 갈래는 문이 **바뀔 때만** 알린다. 그래서 A 가 문 앞에 서서 문이 열린 **뒤에** 들어온 B 는
    // 그 문을 **닫힌 것으로** 본다(서버는 열렸다고 알아 지나가지는데 그림은 닫혀 있다).
    // 접속 직후 0.2초 만에 나간 알림을 클라가 버리는 것도 같은 구멍이다(확인표 ② 의 ⚠).
    // ⭕ 대조군이 갈랐다 — 같은 클라로 opennox 서버에 붙으면 접속 15초 안에 `b2` 가 **한 번 온다**
    //   (우리는 0). 정답지는 더러움 비트로 **사람마다** 보낸다.
    // 방식 — 사람마다 「이 문을 어떤 상태로 아나」(`doorTold` · 기본 닫힘 = 클라의 기본)를 들고,
    //   **세계를 받은 지 1초 뒤부터** 지금 상태와 다르면 **한 번** 보낸다. 같으면 안 보낸다 —
    //   「안 바뀌면 다시 안 보낸다」는 그대로다(매 틱 보내면 클라가 죽는다).
    if ((frame_ % kDoorSyncEveryTicks) != 0) return;
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.snapshotIn >= 0) continue;          // 아직 세계를 안 받았다
        if (o.doorSyncFrom == 0) o.doorSyncFrom = frame_ + kDoorSyncGraceTicks;
        if (frame_ < o.doorSyncFrom) continue;
        if (o.doorTold.size() != props_.size()) o.doorTold.assign(props_.size(), 0);
        for (size_t i = 0; i < props_.size(); ++i) {
            const WorldProp& p = props_[i];
            // ★★ 09-21 — **마나석도 같은 결함이었다**(사용자가 화면으로 짚음):
            //   「마나를 안 뽑았는데 불이 꺼져 있다」. 클라는 마나석 밝기를 `b3` 로만 정하는데
            //   (`cdecode.c:2236` — 밝기 = 16×값/10 · 그림칸 = 8×값/50), 우리는 **마나를 줄 때만** 보냈다.
            //   ⇒ **아무도 안 쓴 마나석은 영영 어두운 채**로 남는다. 대조군은 접속 직후 한 번 준다
            //   (같은 15초에 `b3` 우리 5 : 대조군 1 — 우리가 더 많이 보내는데 **필요한 때를 놓쳤다**).
            // ⭕ 09-22 — 잔량을 넣었으니 여기도 **지금 잔량**을 보낸다
            //   (옛 주석의 「잔량을 넣으면 그때 여기도 같이 고칠 것」을 지금 지킨 자리다)
            if (p.kind == PropKind::Obelisk) {
                if (o.doorTold[i] != 0) continue;              // 이미 알려 줬다
                const uint16_t onc = PropWireCode(p);
                const uint8_t ob[4] = { static_cast<uint8_t>(Op::OBELISK_CHARGE),
                                        static_cast<uint8_t>(onc), static_cast<uint8_t>(onc >> 8),
                                        p.charge };
                EmitSeq(o, ob, sizeof(ob), out);
                o.doorTold[i] = 1;
                ++obeliskTold_;
                continue;
            }
            if (p.kind != PropKind::Door) continue;
            const uint8_t cur = p.open ? 1 : 0;
            if (o.doorTold[i] == cur) continue;
            const uint16_t dnc = PropWireCode(p);
            const uint8_t full[4] = {
                static_cast<uint8_t>(Op::DOOR_ANGLE),
                static_cast<uint8_t>(dnc),
                static_cast<uint8_t>(dnc >> 8),
                static_cast<uint8_t>(p.open ? DoorOpenAngle(p.angle) : p.angle),
            };
            Emit(o, /*reliable=*/false, /*stream=*/0, full, sizeof(full), out);
            o.doorTold[i] = cur;
            ++doorResync_;
        }
    }
}

// 깃발 판 승자 — `0x57 REPORT_FLAG_WINNER`, 총 8바이트.
//
// ⭕ **짜임을 오픈녹스 클라 원본에서 확정했다** (2026-09-11, `cdecode.c:982-1023`).
//   우리 덤프 표본은 0건인데, 그 까닭은 **정품 녹음에 CTF 판이 한 판도 없어서**다.
//
//     57 | 팀 ID 2 | 끝난 까닭 1 | 프레임 4
//
// ★ `0x58`(데스매치 승자)과 **딱 한 필드만 다르다** — `0x58` 은 +1 이 사람 번호
//   (`netClearHighBit` → `playerInfoGetByID`), `0x57` 은 +1 이 **팀 ID**(`getTeamByID`).
//   나머지 세 자리는 완전히 같다.
// · 팀이 0 이거나 없는 팀이면 클라가 무승부(`CTF_Tie`)로 띄운다.
// · 끝난 까닭 1 이면 「시간 초과」를 앞에 붙인다. 0 이 보통 승리.
// ★★ **프레임이 문지기다** — `cdecode.c:1000` 의 `if (프레임 <= 저장값) return 8;`.
//   `0x58` 에서 0 을 실어 매번 버려졌던 그 사고와 같은 자리다. 받는 사람의 `tickFrame` 을 싣는다.
void Server::SendFlagWinner(uint8_t team, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        uint8_t msg[8];
        msg[0] = static_cast<uint8_t>(Op::REPORT_FLAG_WINNER);
        msg[1] = team;
        msg[2] = 0;
        msg[3] = 0;                       // 보통 승리 (1 이면 시간 초과)
        PutU32(msg + 4, o.tickFrame);
        EmitSeq(o, msg, sizeof(msg), out);
    }
}

// 깃발 상태 HUD — `0xd8 REPORT_FLAG_STATUS`, 총 6바이트.
//
// ⭕ **짜임 확정** (2026-09-11, `cdecode.c:2659-2664` · `gui_ctf.c:25-66` · `GAME2_1.c:4159-4177`).
//   그동안 「짜임 미상」으로 **아예 안 보냈다** — 깃발 HUD 가 안 돌던 것이 이것이다.
//
//     d8 | 상태 1 | 팀 칸 1 | 깃발 번호 1 | 든 사람 번호 2
//
// · 상태 — `0`=제자리 · `1`=누가 들었음 · `2`=밖에 나감
// · 팀 칸 — **1~16** (클라 GUI 자식 ID `8810+n`)
// · 깃발 번호 — **1 또는 2**
// · 든 사람 번호 — 안 들렸으면 0
// ⚠ **바뀔 때만 보낸다.** 매 틱 보내면 안 된다 — `0x5e` 를 매 틱 보내다가
//   정품의 270배가 되어 클라가 죽은 적이 있다(2026-09-08). 상태 지문이 달라진
//   깃발만 내보낸다.
void Server::SendFlagStatus(std::vector<OutPacket>& out) {
    for (auto& f : flags_) {
        if (f.team < 1 || f.team > 2) continue;
        uint16_t carrier = 0;
        uint8_t state = f.atHome ? 0 : 2;      // 집에 없고 안 들렸으면 「밖」
        if (!f.carrier.empty()) {
            auto it = sessions_.find(f.carrier);
            if (it != sessions_.end()) { carrier = it->second.netCode; state = 1; }
        }
        const uint32_t mark = (static_cast<uint32_t>(state) << 16) | carrier;
        if (f.sentMark == mark) continue;      // 안 바뀌었으면 안 보낸다
        f.sentMark = mark;

        uint8_t msg[6];
        msg[0] = static_cast<uint8_t>(Op::REPORT_FLAG_STATUS);
        msg[1] = state;
        msg[2] = f.team;                       // 팀 칸 (1~16 중 1·2 만 쓴다)
        msg[3] = f.team;                       // 깃발 번호 (1 또는 2)
        msg[4] = static_cast<uint8_t>(carrier);
        msg[5] = static_cast<uint8_t>(carrier >> 8);
        Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
        ++flagStatusSent_;
    }
}

// 깃발 알림 — `0xa9 INFORM` 의 갈래 5·6·7.
// 속 짜임은 opennox `InformTeamPlayer` 그대로다: PlayerID 4 + TeamID 4.
void Server::SendFlagInform(InformCode code, uint16_t playerNetCode, uint8_t team,
                            std::vector<OutPacket>& out) {
    // ★★ **본문 길이는 갈래가 정한다** — 늘 10바이트로 보내면 안 된다(2026-09-10 감사).
    //   `Wire.h` 의 길이표: 깃발 회수(4)는 본문 **4**, 줍기·놓기·득점(5·6·7)은 **8**.
    //   회수에 8을 실으면 남는 4바이트를 클라가 **다음 메시지의 시작으로 읽어** 그 뒤가
    //   통째로 밀린다. 우리가 보내는 넷 중 회수 하나만 어긋나 있었다.
    uint8_t msg[10];
    msg[0] = static_cast<uint8_t>(Op::INFORM);
    msg[1] = static_cast<uint8_t>(code);
    const uint32_t pid = playerNetCode;
    msg[2] = static_cast<uint8_t>(pid);
    msg[3] = static_cast<uint8_t>(pid >> 8);
    msg[4] = msg[5] = 0;
    msg[6] = team;
    msg[7] = msg[8] = msg[9] = 0;
    const size_t body = (code == InformCode::FlagRetrieve) ? 4 : 8;
    const size_t n = 2 + body;
    for (auto& kv : sessions_) {
        if (kv.second.inGame) {
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, n, out);
        }
    }
}

void Server::DropFlag(const std::string& who, std::vector<OutPacket>& out) {
    for (auto& f : flags_) {
        if (f.carrier != who) continue;
        auto it = sessions_.find(who);
        if (it != sessions_.end()) { f.x = it->second.x; f.y = it->second.y; }
        f.carrier.clear();
        f.atHome = false;
        SendFlagInform(InformCode::FlagDrop,
                       it == sessions_.end() ? 0 : it->second.netCode,
                       f.team, out);
    }
}

void Server::UpdateFlags(std::vector<OutPacket>& out) {
    if (flags_.empty()) return;
    // ★ 깃발 HUD 는 **상태가 바뀐 것만** 나간다(`SendFlagStatus` 안에서 지문 대조).
    //   그동안 짜임을 몰라 아예 안 보냈다 — 「깃발 HUD 가 안 돈다」가 이것이었다.
    SendFlagStatus(out);
    // ★ **판이 끝났으면 더 세지 않는다.** 2026-09-09 실측 — 깃발을 둘 다
    //   닿는 자리에 두고 돌렸더니 득점이 매 틱 올라 **1378:1378** 이 됐다.
    //   한도(3)에서 `roundOver_` 는 섰는데 세는 것을 안 멈춘 탓이다.
    if (roundOver_) return;

    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame || s.respawnAtFrame != 0 || s.team == 0) continue;

        for (auto& f : flags_) {
            const double dx = f.x - s.x, dy = f.y - s.y;
            const double d2 = dx * dx + dy * dy;
            const bool near = d2 <= kFlagTouchRange * kFlagTouchRange;

            if (f.team != s.team) {
                // 적 깃발 — 바닥에 있고 가까우면 든다.
                if (f.carrier.empty() && near) {
                    f.carrier = kv.first;
                    f.atHome = false;
                    SendFlagInform(InformCode::FlagPickup, s.netCode, s.team, out);
                }
                // ★ **든 깃발은 든 사람을 따라다닌다.** 안 그러면 화면에서 깃발이
                //   집에 그대로 붙어 있어 「들었는데 왜 안 따라오나」가 된다(2026-09-10).
                if (f.carrier == kv.first) { f.x = s.x; f.y = s.y; }
            } else {
                // 내 깃발 — 바닥에 나와 있고 가까우면 제자리로 돌린다.
                if (f.carrier.empty() && !f.atHome && near) {
                    f.x = f.homeX; f.y = f.homeY; f.atHome = true;
                    SendFlagInform(InformCode::FlagRetrieve, s.netCode, s.team, out);
                }
            }
        }

        // 적 깃발을 들고 **내 깃발이 제자리에 있을 때** 내 진영에 닿으면 득점.
        // ⚠ 「내 깃발이 제자리에 있어야 한다」는 CTF 의 흔한 규칙이지만 **실측은 아니다.**
        Flag* mine = nullptr;
        Flag* held = nullptr;
        for (auto& f : flags_) {
            if (f.team == s.team) mine = &f;
            if (f.carrier == kv.first) held = &f;
        }
        if (held && mine && mine->atHome) {
            const double dx = mine->homeX - s.x, dy = mine->homeY - s.y;
            if (dx * dx + dy * dy <= kFlagTouchRange * kFlagTouchRange) {
                held->carrier.clear();
                held->x = held->homeX; held->y = held->homeY; held->atHome = true;
                if (s.team < 3) ++caps_[s.team];
                SendFlagInform(InformCode::FlagCapture, s.netCode, s.team, out);
                if (!roundOver_ && caps_[s.team] >= kCaptureLimit) {
                    roundOver_ = true;
                    restartAtFrame_ = frame_ + kRoundRestartTicks;
                    SendFlagWinner(s.team, out);
                }
            }
        }
    }

    // 들려 있는 깃발은 든 사람을 따라간다.
    for (auto& f : flags_) {
        if (f.carrier.empty()) continue;
        auto it = sessions_.find(f.carrier);
        if (it == sessions_.end() || !it->second.inGame) { f.carrier.clear(); continue; }
        f.x = it->second.x;
        f.y = it->second.y;
        f.atHome = false;
    }
}

// 판 시계 한 통을 만든다. 보내는 곳이 둘이라 따로 뺐다 —
// 주기 발송(`SendTimer`)과 **클라가 `0xd4` 로 물어봤을 때의 즉답**.
// ★★ 2026-09-21 — **도장은 「받는 사람의 시계」로 찍는다.**
//
// 전에는 전역 `frame_` 을 찍었다. 그런데 클라의 시계는 우리가 `27 TIMESTAMP` 로 보내는
// **접속마다 따로 세는 `tickFrame`** 을 따른다(Session::tickFrame 주석). 둘은 같지 않다 —
// 실측: 요청한 클라의 시계 **1847**, 우리 도장 **1849**(차 −2).
//
// 클라는 `d3` 을 받으면 도장이 낡았는지 본다(`cdecode.c:2638`):
//     if ((unsigned)(gameFrame() - 도장) >= 30)  →  `d4` 로 다시 달라고 한다
// **부호 없는 뺄셈**이라 도장이 클라 시계보다 **1 만 앞서도**(1847−1849 = 0xFFFFFFFE) 낡은 것이 된다.
// 우리 답이 또 앞서 있으니 **무한 되풀이** — 둘이 붙은 한 판에 `시계요청 9979` · `5525`.
// 주기 발송(`SendTimer`)의 `d3` 하나하나가 그런 사슬을 새로 낳아 **초당 30 → 150 으로 가속**했다.
//
// ⚠ 단일 클라 주행에서는 안 보였다(그 사람 시계가 마침 도장보다 뒤가 아니었다) —
//   「한 명으로 돌려 본 것」은 이 결함을 못 잡는다.
void Server::BuildTimerStatus(uint8_t msg[13], uint32_t recipientFrame) const {
    // 남은 시간도 **같은 도장에서** 낸다 — 정품은 시계가 하나라 (남은시간 ÷ 도장) 이 늘 같은 비다
    // (실측 −35.6). 도장만 그 사람 시계로 바꾸고 남은 시간을 전역으로 두면 그 짝이 어긋난다.
    const int32_t remain = -static_cast<int32_t>(recipientFrame * tickMs_);
    msg[0] = static_cast<uint8_t>(Op::TIMER_STATUS);
    msg[1] = msg[2] = msg[3] = msg[4] = 0;      // 표본 전수가 0 이었다
    const uint32_t r = static_cast<uint32_t>(remain);
    msg[5] = static_cast<uint8_t>(r);
    msg[6] = static_cast<uint8_t>(r >> 8);
    msg[7] = static_cast<uint8_t>(r >> 16);
    msg[8] = static_cast<uint8_t>(r >> 24);
    msg[9]  = static_cast<uint8_t>(recipientFrame);
    msg[10] = static_cast<uint8_t>(recipientFrame >> 8);
    msg[11] = static_cast<uint8_t>(recipientFrame >> 16);
    msg[12] = static_cast<uint8_t>(recipientFrame >> 24);
}

void Server::SendTimer(std::vector<OutPacket>& out) {
    // ★ 09-21 — 사람마다 **그 사람의 시계**로 도장을 찍는다(`BuildTimerStatus` 주석).
    //   전에는 메시지를 한 번 짓고 모두에게 같은 것을 보냈다 — 전역 `frame_` 도장이라
    //   시계가 뒤인 사람에게는 늘 「낡은」 것으로 읽혔다
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        uint8_t msg[13];
        BuildTimerStatus(msg, ClockOf(kv.second));
        EmitSeq(kv.second, msg, sizeof(msg), out);
    }
}

size_t Server::OnPacket(const std::string& from, const uint8_t* data, size_t n,
                        std::vector<OutPacket>& out) {
    Session& s = SessionFor(from);
    ++s.recvPackets;
    s.lastSeenFrame = frame_;   // 끊긴 세션을 걷어내는 근거. Tick 의 「자리 비우기」 참조

    // 받은 것이 암호문이면 먼저 푼다. 사본을 뜬다 — 부르는 쪽 버퍼를 건드리지 않는다.
    // ⚠ 덤프 재생일 때는 이미 평문이라 풀면 안 된다 (SetInboundPlain 주석 참조).
    std::vector<uint8_t> buf(data, data + n);
    if (s.keyed && !inboundPlain_) ApplyXor(buf.data(), buf.size(), s.xorKey);

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
        ++reliableHeard_;
        if ((head.stream & 0x7F) < 8) ++recvRelStream_[head.stream & 0x7F];
    }

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
            const uint8_t sop = static_cast<uint8_t>(probe.stopOp());
            ++stoppedByOp_[sop];
            // ★ 진단 — **멈춘 자리의 바이트를 그대로 남긴다**(오프코드마다 첫 한 번만).
            //   「어느 오프코드에서 멈췄나」만으로는 못 고친다. `0xc9` 를 하위 명령별 표까지
            //   넣었는데 여전히 버렸다 — **그 하위 명령이 표에 없는 값**일 수 있고,
            //   그건 바이트를 봐야 안다. (2026-09-11)
            if (stopSampleShown_.insert(sop).second) {
                const size_t rest = probe.stopRest();
                const size_t base = (bodyLen >= rest) ? bodyLen - rest : 0;
                std::printf("[멈춤표본] op=%02x 남은 %zu |", sop, rest);
                for (size_t q = 0; q < rest && q < 8; ++q)
                    std::printf(" %02x", body[base + q]);
                std::printf("\n");
                std::fflush(stdout);
            }
            return 0;
        }
    }

    MessageIter it(body, bodyLen, /*fromClient=*/true);
    size_t count = 0;
    lastFragOps_.clear();   // 진단 — 이 조각에서 무엇을 읽었는지 남긴다
    while (it.Next()) {
        ++count;
        // ★ 09-29 ③ 와이어 대조(같은 자동 클라 · 서버만 바꿔) — **게임 패킷 안의 0x00 은 거기서 읽기를 끝낸다.**
        //   정답지는 첫 번호가 0x20 미만인 패킷만 접속 제어로 보고(`netstr/conn.go:704~708` `processStreamOp` — 첫 번호 하나만),
        //   게임 패킷은 `sdecode` 로 넘기는데 0x00 은 거기서 모르는 번호라 −1 → 그 패킷 나머지를 버린다(`network_server.go:62~67` · `sdecode.c:473~474`).
        //   ✖ 우리는 클라 중요 메시지(`aa`) 뒤의 **0 채움 바이트 하나하나**를 「접속 요청 재전송」으로 읽고 매번 `0x04`(「이미 접속함」
        //     `codeAlreadyJoined4`)로 답했다 — 2분 판에 **47,082건**(패킷 하나에 7개 안팎 · 대조군 0건 · `_evidence/_runs/wc0929_*.cap.txt`)
        if (count > 1 && it.op() == static_cast<uint8_t>(Op::SERVER_CONNECT)) { ++connectTailStops_; break; }
        ++seen_[it.op()];
        if (lastFragOps_.size() < 16) lastFragOps_.push_back(it.op());

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

            // ★★ 신뢰 메시지 되받기 — 클라가 0xAA 로 보내면 0xAB 로 번호를 그대로
            //    돌려줘야 한다. 안 하면 클라는 답이 올 때까지 **영원히 재전송**하고
            //    로딩에서 안 넘어간다 — 화면이 검은 채로 멈춘다.
            //    2026-09-07 실측: 내 서버엔 IMPORTANT 1,378건 · 정품엔 0건,
            //    CLIENT_READY 도 71번(정품 2번) 재전송하고 있었다.
            // ★★★ **별칭은 클라가 정해서 알려 준다.** `a5 <별칭> <번호2> <종류2> …`
            //   방향이 클라→서버뿐이다(실측: 서버 SEND 0건). 그동안 이걸 **안 받고**
            //   같은 규칙으로 따라 굴리기만 했는데, 실제로는 어긋나서 별칭으로 보내면
            //   아무것도 안 그려졌다. 받아서 그대로 쓰면 그럴 일이 없다.
            case Op::NEW_ALIAS: {
                if (it.bodyLen() < 5) break;
                const uint8_t* b = it.body();
                const uint8_t alias = b[0];
                const uint16_t nc = static_cast<uint16_t>(b[1] | (b[2] << 8));
                const uint16_t ty = static_cast<uint16_t>(b[3] | (b[4] << 8));
                s.aliases.Set(alias, nc, ty, s.tickFrame + kAliasTTL);
                ++aliasHeard_;
                break;
            }

            // ★ 준비 — 이때부터 중요 메시지를 싣는다(정답지 `GAME3_2.c:5602` 상태 비트 0x10).
            // ⭕ **2026-09-18 — 「약 62틱마다 되풀이 · 까닭 미상」이 닫혔다.**
            //   그건 **클라의 신뢰 재전송**이었다(`netstr/send_reliable.go` 2초 주기).
            //   클라는 `CLIENT_READY` 를 **두 번만** 부르는데(접속당 2ms 간격 2회 —
            //   정품 녹음 idx 26·30 에서도 같다) 우리 되받기가 **엉뚱한 스트림**에 실려
            //   클라 큐에서 안 지워졌고, 그래서 영원히 다시 왔다.
            //   ⇒ 되받기를 **스트림 0 비신뢰**로 옮기니 **18~25 → 2** 로 떨어졌다(`Server::Emit`).
            //   ⚠ 이 되풀이가 **영구 무적의 뿌리**이기도 했다 — 아래 `GameServerReadyMB` 가
            //     올 때마다 5초 무적을 다시 걸었기 때문이다.
            case Op::CLIENT_READY:
                ++readyHeard_;                             // 1-a — 갈래 셋 중 하나(삼키지 않게 따로)
                if (!s.importantReady) {
                    s.importantReady = true;
                    std::printf("[중요] 프레임 %u %s 준비 — 대기 %zu 개를 싣기 시작\n",
                                frame_, s.addr.c_str(), s.importantQ.size());
                }
                GameServerReadyMB(s, out);                 // network_server.go:99 → nox_xxx_gameServerReadyMB_4DD180
                break;

            // ★ 중요 메시지 확인 — `ab <프레임4>`. 그 프레임 조각에 실었던 것을 지운다
            //   (정답지 `nox_net_importantACK_4E55A0` · `network_server.go:223`).
            //   하위 16비트로 맞춘다 — 우리가 싣는 시각이 `27`(16비트)이고 클라가 윗자리를 제 시계로 채운다.
            case Op::IMPORTANT_ACK: {
                if (it.bodyLen() < 4) break;
                const uint8_t* b = it.body();
                const uint32_t fr = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
                const size_t before = s.importantQ.size();
                s.importantQ.erase(std::remove_if(s.importantQ.begin(), s.importantQ.end(),
                    [fr](const Session::PendingImportant& p) {
                        return p.sent && ((p.sentFrame ^ fr) & 0xFFFF) == 0;
                    }), s.importantQ.end());
                if (s.importantAcked == 0 && before != s.importantQ.size())
                    std::printf("[중요] 프레임 %u %s 첫 확인(시각 %u) — %zu 개 지움, 남은 %zu\n", frame_,
                                s.addr.c_str(), static_cast<unsigned>(fr & 0xFFFF),
                                before - s.importantQ.size(), s.importantQ.size());
                s.importantAcked += static_cast<uint32_t>(before - s.importantQ.size());
                ++handled_;
                break;
            }

            // ★ 10-01 대체 무기 알림 `e0 <번호2>`(몸 2 · `MsgLen.cpp` 클라→서버 갈래) — `ReportSecondaryWeapon` 머리 주석
            case Op::REPORT_SECONDARY_WEAPON: {
                if (it.bodyLen() < 2) break;
                const uint8_t* b = it.body();
                ReportSecondaryWeapon(s, static_cast<uint16_t>(b[0] | (b[1] << 8)), out);
                ++handled_;
                break;
            }

            case Op::IMPORTANT: {
                if (it.bodyLen() < 4) break;
                uint8_t msg[5];
                msg[0] = static_cast<uint8_t>(Op::IMPORTANT_ACK);
                std::memcpy(msg + 1, it.body(), 4);
                // ★ 09-30 — **스트림 0** 으로(= 묶음). 정답지는 `ab` 를 Kind1 에 넣어 그 프레임 묶음에 싣고(`server/network.go:154`),
                //   서버→클라 머리 첫 바이트는 서버 쪽 번호다(`internal/netstr/conn.go:239~240` `hdr[0] = byte(ns.ind)`).
                //   받은 머리를 되쓰는 것은 0x20 미만 접속 제어 답(퐁 등 · `conn.go:704~708`)뿐이다.
                //   ✖ 전에는 받은 스트림(클라 번호 1)으로 돌려줘 묶음에 못 들고 따로 나갔다 — 09-30 자동 클라 2분 `곧바로 스트림 8084`(ab 8,039 + 퐁 45)
                Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
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
                    // ★ 조작이 하나라도 왔다 — 정답지 ImplB 는 목록이 비었으면 상태를 안 건드린다(`object_update.go:485`).
                    s.inputThisTick = true;
                    if (code == 1 && dataLen == 1) {
                        // 바라보는 쪽 — 정답지 CCOrientation → Direction2. ★ 2026-09-13 묶음 3-a 부터 틱의 ImplB 에서 갈아 끼운다
                        //   (정답지는 조작 목록을 ImplA 힘·공격 **뒤에** 읽는다 — 받자마자 바꾸면 한 틱 이르다).
                        s.facingInput = q[0];
                        s.hasFacingInput = true;
                    } else if (code >= 2 && code <= 5 && dataLen == 1) {
                        // 걸음 — ★ 2026-09-13 부터 **바로 옮기지 않는다.** 「이번 틱에 이동 입력이 왔다」만 적고
                        //   틱의 걷기 물리(UpdatePlayerMovement)가 상태를 걷기/달리기로 바꿔 힘을 준다.
                        //   ⭕ 앞·뒤·왼·오른(2~5)은 **힘의 방향을 안 바꾼다** — 정답지는 코드마다 깃발(+Field60)만
                        //     다르게 켜고 힘은 늘 바라보는 쪽으로 준다(`object_update.go:519~534` · `:192`).
                        //     전에 뒤·왼·오른을 128·192·64 로 돌려 민 것은 **우리가 지어낸 규칙**이었다.
                        s.moveInputThisTick = true;
                        // 클라는 걸었다고 믿으니 서버 집계는 그대로 센다.
                        ++moves_;
                    } else if (code == 6) {
                        ++code6Seen_;   // 진단(sq1): 코드6 도달 수 — 성사 15 vs 클라 송신 231 을 가른다
                        // 왼버튼 = `CCAction`(6). 근접 공격이다.
                        // 근거는 둘 — 09-07 조작 실측과 opennox 의 CtrlCode 표.
                        //
                        // ★★★ **첫 왼클릭은 공격이 아니라 「관전 풀기」다.**
                        //   접속 직후 클라는 관전이라 HUD 가 없다. 정품은 이 클릭에
                        //   `6a REPORT_CLIENT_STATUS <내번호2> 00 00 00 00` 을 신뢰로 보낸다
                        //   (2026-09-09 정품 대조군 와이어). 그전에는 내가 이걸 안 보내
                        //   관전이 영영 안 풀렸다 — 화면 색이 관전 해제 앞뒤로 978 → 978 로
                        //   **한 톨도 안 변했다**(정품은 826 → 1220 으로 HUD 가 떴다).
                        if (s.observer) {
                            s.observer = false;
                            // ★★★ **순서가 중요하다 — 상태를 먼저 갈아 끼우고 알린다.**
                            //   오픈녹스 원본에서 `6a` 처리는 마지막에
                            //   `nox_client_onClientStatusA(playerInfo->Field3680)` 을 부르는데,
                            //   그 필드는 **`2d NEW_PLAYER` 의 112번 자리가 채운다**
                            //   (`nox_xxx_netNewPlayerMakePacket_4DDA90`: `buf[112] = Field3680 & 0x423`).
                            //   즉 `6a` 는 **저장돼 있는 값을 읽어** GUI 를 켤지 정한다.
                            //   내가 서두에서 베껴 쓰던 값이 `01`(= HUD 숨김)이라,
                            //   `6a` 를 먼저 보내면 옛 값을 읽어 그대로 숨긴 채로 끝났다.
                            // ⚠ 09-19 시험 — 이 한 줄이 아직 필요한지 본다(정답지는 안 보낸다)
                            if (!noSecondNewPlayer_) EmitNewPlayer(s, s, 0, out);
                            for (auto& kv2 : sessions_) {
                                Session& o = kv2.second;
                                if (!o.inGame || o.addr == s.addr) continue;
                                EmitNewPlayer(o, s, 0, out);   // 남들에게도 「이제 논다」
                            }
                            // ★★★ **모두에게 보낸다 — 본인에게만이 아니다.**
                            //   `2d` 가 세운 1번 비트(관전)는 OR 라 안 꺼지고,
                            //   남의 몸을 안 그리게 막는 것이 바로 그 비트다
                            //   (`DrawPlayer`: 자기가 아니면 `Field3680 & 1` 이면 되돌아간다).
                            //   본인에게만 보내던 탓에 **먼저 온 쪽 화면에서만 상대가 사라졌다.**
                            BroadcastClientStatus(s.netCode, 0, out);
                        }
                        // ⚠ 코드 6 은 **누르는 순간 한 번만** 온다(2026-09-09 실측 — 180초 눌러 조각 10,885 개 중 2건).
                        // ★★ 2026-09-13 묶음 3-a·3-b — **정답지로.** 클릭은 「이번 틱에 CCAction 이 왔다」만 적는다.
                        //   틱의 ImplB(PlayerInputAttack)가 기력(무기)·쉬는 시간(Field0)을 보고 상태 1 로 넣고,
                        //   ImplA(PlayerAttack)가 맨손은 애니 끝 · 무기는 애니 한가운데 틱에 판정한다.
                        //   전에는 여기서 바로 때리고 15틱 쉬었다(kMeleeCooldownTicks · 우리 값) — 걷어냈다.
                        else {
                            s.actionThisTick = true;
                        }
                    } else if (code == 7) {
                        // ★ 09-23 nine-defects 3-a — `CCJump` · 데이터 없음. 「이번 틱에 왔다」만 적고 틱의 ImplB 가 뛴다(`object_update.go:550~571`)
                        //   ⭕ 7 은 **원본 기준** — opennox libs `noxnet/msg_player_input.go:211` · `player/ctrlevent_codes.go:12`
                        //     `CCJump = CtrlCode(7)`(Go 모듈 저장소 `~/go/pkg/mod/github.com/opennox/libs@…` — 이 리포 밖이라 3-a 때는 못 찾았다).
                        //   ⚠ **와이어 실측은 아직** — 사람 판에서 휠(점프)을 눌러 판끝 `[점프] 코드7 받음` 이 오르면 확정
                        ++jumpCodes_;
                        s.jumpThisTick = true;
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
            // ── 줍기 ──────────────────────────────────────────────────
            //
            // ★ 이 갈래가 **`switch` 까지 오지도 못했다.** `0x73` 의 길이가 `-1` 이라
            //   조각을 훑는 걸음이 거기서 멈추고 **조각을 통째로 버렸기 때문**이다
            //   (`MsgLen.cpp` 를 채워 풀었다). 사람은 눌렀는데 로그에도 안 남았다.
            //
            // 보내는 것 둘 —
            //   `4b <번호2><종류2>`(5B) 또는 수식 붙는 것이면 `4c <번호2><종류2><수식4>`(9B)
            //     → **집는 이에게만.** 첫 필드는 「집는 이」가 아니라 **물건 번호**다
            //       (정품 `0x4c` 표본 10건: 사람은 713 인데 실린 값이 716·717·718…).
            //   `31 <번호2>`(3B) → **보고 있던 모두에게.** 바닥에서 사라지는 것.
            // ── 충돌 요청(묶음 3-d) — `7b <번호2>` · Server::TryCollide 주석 ──
            case Op::TRY_COLLIDE: {
                if (!s.inGame || it.bodyLen() != 2) break;
                TryCollide(s, static_cast<uint16_t>(it.body()[0] | (it.body()[1] << 8)), out);
                ++handled_;
                break;
            }

            case Op::VOTE: {
                // ── 묶음 8-e 투표 — `sdecode.c:315`(`0xEE`)
                //   몸 — 갈래 0~3 은 **51바이트**(갈래 1 + 이름 와이드 25자) · 갈래 4·5 는 **1바이트**
                //   갈래 0·2 는 퀘스트면 **3**, 아니면 **0** · 1·3 은 늘 **1** · 4·5 는 **2**
                if (!s.inGame || it.bodyLen() < 1) break;
                const uint8_t code = it.body()[0];
                const bool quest = questTotal_ > 0 || questStage_ > 0;
                std::string targetName;
                if (code <= 3) {
                    if (it.bodyLen() != 51) break;
                    // 이름은 **와이드 2바이트씩** 25자. 아스키 자리만 추려 읽는다
                    for (int k = 0; k < 25; ++k) {
                        const uint8_t lo = it.body()[1 + 2 * k], hi = it.body()[2 + 2 * k];
                        if (lo == 0 && hi == 0) break;
                        targetName.push_back(static_cast<char>(lo));
                    }
                } else if (it.bodyLen() != 1) {
                    break;
                }
                switch (code) {
                    case 0: VoteCast(s, quest ? 3 : 0, targetName, out); break;
                    case 1: VoteCast(s, 1, targetName, out); break;
                    case 2: VoteWithdraw(s, quest ? 3 : 0, targetName, out); break;
                    case 3: VoteWithdraw(s, 1, targetName, out); break;
                    case 4: VoteCast(s, 2, std::string(), out); break;
                    case 5: VoteWithdraw(s, 2, std::string(), out); break;
                    default: break;
                }
                ++handled_;
                break;
            }

            case Op::TRY_ABILITY: {
                // ── 묶음 7-d 능력 쓰기 — `network_server.go:91`(`0x7A`)
                //   **몸 1바이트** — `<능력 번호>`. 채팅 모드가 아니고 `+3680 & 3`(관전)이 아니어야 한다
                ++abilHeard_;                      // ★ 받은 것 자체를 먼저 센다
                if (!s.inGame || it.bodyLen() != 1) { ++abilBadPacket_; break; }
                if (s.observer) { ++abilObserver_; }
                else AbilityDo(s, it.body()[0], out);
                ++handled_;
                break;
            }

            case Op::TRY_CREATURE_COMMAND: {
                // ── 묶음 6-i 소환수 명령 — `sdecode.c:193`(`0x78`)
                //   **몸 3바이트** — `<번호 u16><명령 u8>`. 번호가 **0 이면 내 것 전부**에게 준다
                //   ⚠ 정답지는 사람 자료 `+3680 & 1`(명령 금지)일 때 통째로 무시한다 — 그 깃발은 아직 없다
                if (!s.inGame || it.bodyLen() != 3) break;
                const uint16_t nc = static_cast<uint16_t>(it.body()[0] | (it.body()[1] << 8));
                OrderUnit(s, nc, it.body()[2]);
                ++handled_;
                break;
            }

            case Op::TRY_GET: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t want = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                ++tryGets_;

                // 세계에 있나 — 없으면 조용히 넘긴다(남이 먼저 주웠을 수 있다).
                size_t at = statics_.size();
                for (size_t i = 0; i < statics_.size(); ++i)
                    if (statics_[i].netCode == want) { at = i; break; }
                if (at == statics_.size()) { ++handled_; break; }

                const WorldStatic w = statics_[at];
                const double dx = w.x - s.x, dy = w.y - s.y;
                if (dx * dx + dy * dy > kPickupReach * kPickupReach) {
                    ++pickTooFar_;
                    ++handled_;
                    break;      // 손이 안 닿는다
                }
                if (s.inventory.size() >= kInventoryGuard) { ++handled_; break; }

                // ── 묶음 5-h — **금은 가방에 안 들어간다**(`pickupGold_4F3A60`) ──
                //
                // 사람이면 **금액을 더하고 물건을 없앤다**. 사람이 아니면(몬스터) 보통 줍기로 간다.
                // ⚠ 안 옮긴 것 — 줄글(`GoldPickup` 문자열 + 금액)과 소리 307 은 묶음 10.
                if (w.goldPickup) {
                    // 10-g PlaySound 307 TreasurePickup — pickupGold_4F3A60 rings it on both paths
                    PlaySoundAt(307, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
                    PlayerAddGold(s, w.goldAmount, out);
                    {   // 모두에게 — 바닥에서 사라진다
                        uint8_t msg[3];
                        msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                        msg[1] = static_cast<uint8_t>(w.netCode);
                        msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                        Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                    }
                    TakeStatic(w.netCode);
                    ++goldPicked_;
                    ++handled_;
                    break;
                }

                // ── 묶음 5-c — 물약은 **줍는 순간이 다르다**(`nox_xxx_pickupPotion_4F37D0`) ──
                //
                // `PICKUP = PotionPickup` 인 것만 이 길로 온다(thing.bin 실측 38종).
                //   ① **직업이 못 쓰는 것이면 아예 못 줍는다** — 글 `pickup.c:ObjectEquipClassFail`
                //   ② 마실 수 있으면 **그 자리에서 마시고 없어진다**(가방에 안 들어간다)
                //   ③ 못 마시면 그냥 가방으로 — 아래 5-a 규칙을 그대로 탄다
                if (w.potionPickup) {
                    const int r = PickupPotion(s, w, out);
                    if (r < 0) { ++pickClassFail_; ++handled_; break; }   // 못 줍는다
                    if (r > 0) {                                          // 그 자리에서 마셨다
                        {   // 모두에게 — 바닥에서 사라진다
                            uint8_t msg[3];
                            msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                            msg[1] = static_cast<uint8_t>(w.netCode);
                            msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                            Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                        }
                        TakeStatic(w.netCode);
                        ++drankOnPickup_;
                        ++handled_;
                        break;
                    }
                }

                // ★ 09-29 — 화살통 줍기(`nox_xxx_pickupAmmo_4F3B00` `GAME3_3.c:10831~10903`) — 비트 0x82(화살통·수리검)면 가방에서
                //   같은 종류 · WEAPON · 같은 비트 · 수식 넷이 같고 · 무한 아님 · 최대 합 ≤ 250 인 첫 것에 **합친다**(화살 수·최대를 더함) →
                //   그것의 충전 알림 · 바닥 것은 지움 · 줍는 소리(`53A6C0`). 합칠 것이 없으면 무기 줍기(`sub_53A720`)로 — 아래 무기 갈래
                //   ⚠ 합칠 때는 무게·개수 관문(`pickupDefault`)을 안 탄다 — 정답지 그대로
                if (w.pickupFn == PickupFn::Ammo && (w.equipBit & 0x82)) {
                    Session::Carried* into = nullptr;
                    for (size_t k = s.inventory.size(); k-- > 0;) {
                        Session::Carried& c2 = s.inventory[k];
                        if (c2.type != w.type || !c2.weaponClass || c2.wandClass || !(c2.equipBit & w.equipBit)) continue;
                        bool same = true;
                        for (int m = 0; m < 4; ++m) if (c2.mods[m] != w.mods[m]) same = false;
                        if (same && static_cast<int>(w.chargesMax) + static_cast<int>(c2.chargesMax) <= 250) { into = &c2; break; }
                    }
                    if (into) {
                        into->charges = static_cast<uint8_t>(into->charges + w.charges);
                        into->chargesMax = static_cast<uint8_t>(into->chargesMax + w.chargesMax);
                        SendCharges(s, *into, out);
                        {   // 모두에게 — 바닥에서 사라진다(`delayedDeleteObject`)
                            uint8_t msg[3];
                            msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                            msg[1] = static_cast<uint8_t>(w.netCode);
                            msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                            Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                        }
                        TakeStatic(w.netCode);
                        PlayWeaponPickupSound(s, w.pickupSound);
                        ++weaponPick_[static_cast<size_t>(WeaponPick::AmmoMerged)];
                        ++handled_;
                        break;
                    }
                }
                // ★ 09-29 — 무기 줍기(`sub_53A720` `GAME4_3.c:5089~5095`) — 협동·퀘스트가 아니면 **직업이 못 쓰는 무기는 아예 못 줍는다**
                //   (글 `weapon.c:WeaponEquipClassFail` + 소리 925 kind 2 · 정의가 없으면 못 쓰는 것 — `57B3D0`).
                //   ⚠ 안 옮긴 것 — 같은 무기 거절(`CannotPickupDuplicateWeapon` — 게임 설정 비트 2 · 우리 설정 없음) · 퀘스트의 자연의 힘 지팡이 개수(`:5048~5057`)
                if ((w.pickupFn == PickupFn::Weapon || w.pickupFn == PickupFn::Ammo) && !(questTotal_ > 0 || questStage_ > 0)) {
                    const auto d = weaponDef_.find(w.type);
                    const uint8_t classes = (d == weaponDef_.end()) ? 0 : d->second.classes;
                    if (!(classes & (1u << static_cast<uint8_t>(s.charClass)))) {
                        SendPriMsg(s, "weapon.c:WeaponEquipClassFail", out);
                        PlaySoundAt(925, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
                        ++weaponPick_[static_cast<size_t>(WeaponPick::ClassFail)];
                        ++handled_;
                        break;
                    }
                }

                // ★ 09-29 — 갑옷 줍기(`pickupArmor_53E7F0` `GAME4_3.c:7612~7624`) — 협동·퀘스트가 아니면 **직업이 못 입는 갑옷은 아예 못 줍는다**
                //   (글 `armor.c:ArmorEquipClassFail` + 소리 925 kind 2) · 정의가 없으면 못 입는 것(`57B3D0`)
                //   ⚠ 안 옮긴 것 — 같은 갑옷 거절(`CannotPickupDuplicateArmor` — 게임 설정 비트 2 · `sub_409F40` — 우리 설정 없음)
                if (w.equipBit != 0 && !w.isWeapon && !w.weaponClass && !(questTotal_ > 0 || questStage_ > 0)) {
                    const auto d = armorDef_.find(w.type);
                    const uint8_t classes = (d == armorDef_.end()) ? 0 : d->second.classes;
                    if (!(classes & (1u << static_cast<uint8_t>(s.charClass)))) {
                        SendPriMsg(s, "armor.c:ArmorEquipClassFail", out);
                        PlaySoundAt(925, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
                        ++armorGate_[static_cast<size_t>(ArmorGate::PickupClassFail)];
                        ++handled_;
                        break;
                    }
                }

                // ── 묶음 5-a — 줍기 규칙(`nox_xxx_pickupDefault_4F31E0`) ──────────
                //
                // ① **들 밑천이 0 이면 아예 못 든다**(`object_pickup.go:23·68`).
                //    ⚠⚠ 밑천은 thing.bin 의 `CARRYCAPACITY = 100` 이 **아니다**. 그건 오브젝트를
                //    만들 때의 기본값이고(`server/object.go:330`), 사람은 접속 때 `plrReadVals_4EEDC0` 가
                //    **힘에서 다시 계산해 `+490` 에 덮어쓴다**(`GAME3_3.c:7704`) — 위저드 1650.
                //    `Session::carryCap` 주석에 근거를 적어 두었다.
                if (s.carryCap == 0) { ++pickNoCap_; ++handled_; break; }

                // ② **무게 한도** — `무게 > 밑천 × 2 − 이미 든 무게` 면 거절하고 글을 낸다
                //    (`object_pickup.go:70~79`). 한도가 밑천의 **두 배**인 것이 정답지 식이다.
                {
                    int carried = 0;
                    for (const auto& c2 : s.inventory) carried += static_cast<int>(c2.weight);
                    if (static_cast<int>(w.weight) > static_cast<int>(s.carryCap) * 2 - carried) {
                        SendPriMsg(s, "pickup.c:CarryingTooMuch", out);
                        ++pickTooHeavy_;
                        ++handled_;
                        break;
                    }
                }

                // ③ **먹는 것은 같은 종류를 셋까지**(퀘스트·협동은 아홉 — `object_pickup.go:81~92`).
                if (w.isFood) {
                    int same = 0;
                    for (const auto& c2 : s.inventory) if (c2.type == w.type) ++same;
                    const bool quest = (questTotal_ > 0 || questStage_ > 0);   // gameFlags 4096|8192
                    if (same >= (quest ? 9 : 3)) {
                        SendPriMsg(s, "pickup.c:MaxSameItem", out);
                        ++pickTooMany_;
                        ++handled_;
                        break;
                    }
                }
                // ⚠ 안 옮긴 갈래 — 물건에 **편**이 붙어 있으면 남의 편은 못 줍고 알림글 16 이 간다
                //   (`object_pickup.go:54~64`). 우리 `WorldStatic` 에는 편 칸이 없다(맵 물건은 편이 없다).
                //   깃발·편 물건이 생기는 묶음 8 에서 붙인다.

                // ① 집는 이에게 — 무엇을 들었는지
                if (w.modifiable) {
                    uint8_t msg[9];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_MODIFIABLE_PICKUP);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    msg[3] = static_cast<uint8_t>(w.type);
                    msg[4] = static_cast<uint8_t>(w.type >> 8);
                    // 수식 네 칸 — **빈 칸은 `ff`(-1)** 다. 정품 표본이 그렇다
                    //   (`ff ff ff ff` 도 있고 `b9 ca ff ff` 처럼 둘만 붙은 것도 있다).
                    //   맵에 놓인 물건에 수식이 붙어 있는지는 아직 안 읽는다 → 전부 빈 칸.
                    msg[5] = msg[6] = msg[7] = msg[8] = 0xFF;
                    EmitSeq(s, msg, sizeof(msg), out);
                } else {
                    uint8_t msg[5];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_PICKUP);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    msg[3] = static_cast<uint8_t>(w.type);
                    msg[4] = static_cast<uint8_t>(w.type >> 8);
                    EmitSeq(s, msg, sizeof(msg), out);
                }

                // ② 모두에게 — 바닥에서 사라진다
                {
                    uint8_t msg[3];
                    msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                }

                // ③ 세계에서 뺀다. **지우는 길이 아예 없었다**(계획서 벽②).
                TakeStatic(w.netCode);

                Session::Carried c = CarriedFromStatic(w);   // ★ 09-30 — 칸 옮기기를 함수로(시험 물건 주기도 같은 것을 쓴다)
                s.inventory.push_back(c);
                ++picked_;
                // ④ ★ 09-28 사람 판 — 「빈 부위 갑옷을 주우면 원래 저절로 입었다」
                //   무기 — `sub_53A720`(`GAME4_3.c:5099~5104`): 손이 비었고(+104 == 0) 화살통(무기 비트 2)이 아니면 무기 들기 `53A420(a4, 0)`(실패 글·소리 없음)
                //   갑옷 — `pickupArmor_53E7F0`(`:7628~7663`): 같은 부위(SUBCLASS 같음 — `armorHaveSameSubclass_53E7B0`)를 입은 게 없으면 입기 `53E650`.
                //          방패(SUBCLASS & 2)는 두 손 무기(0x7FFE40C)를 들었으면 안 입는다(`:7657~7661`)
                //   ⚠ 안 옮긴 것 — `sub_419E60`(사람마다 깃발 · 뜻 미확인 — 늘 꺼진 것으로 둔다) · 갑옷 줍기 소리(804~816)
                //   (★ 09-29 — 같은 부위 바꿔 입기(`:7634~7711`)는 갑옷 입기 `53E650` 이식과 함께 옮김 — 아래)
                {
                    const uint16_t pnc = c.netCode;
                    if (c.weaponClass) {
                        // ★ 09-29 — `sub_53A720` `:5099~5125` 나머지 — 화살통이면 활·석궁(0xC)을 들었고 든 화살통(2)이 없을 때 저절로 든다 ·
                        //   저절로 안 들었으면 충전 알림(지팡이 0x47F0000 → 쓰기 짐 108·109 = 우리 충전 · WEAPON 0x82 → (화살 수, 최대)) · 줍는 소리
                        //   ⚠ 무기 줍기 함수를 타는 것(WeaponPickup·AmmoPickup)만 — 가방 항목의 `pickupFn` 으로 가른다
                        bool v9 = false;
                        if (s.weaponNet == 0 && c.equipBit != 2 && PlayerEquipWeapon(s, pnc, /*failSound=*/false, out)) { v9 = true; ++autoEquip_[0]; }
                        if (c.equipBit == 2) {
                            const uint32_t f = WeaponEquipOf(s);
                            if ((f & 0xC) && !(f & 2) && PlayerEquipWeapon(s, pnc, /*failSound=*/false, out)) {
                                v9 = true;
                                ++weaponPick_[static_cast<size_t>(WeaponPick::QuiverAutoEquip)];
                            }
                        }
                        const bool weaponPickFn = (c.pickupFn == PickupFn::Weapon || c.pickupFn == PickupFn::Ammo);
                        if (!v9 && weaponPickFn) {
                            const bool wandCh = c.wandClass && (c.subclass & 0x47F0000);
                            const bool ammoCh = !c.wandClass && (c.subclass & 0x82);
                            if (wandCh || ammoCh) {
                                for (auto& c3 : s.inventory) if (c3.netCode == pnc) { SendCharges(s, c3, out); break; }
                                ++weaponPick_[static_cast<size_t>(WeaponPick::ChargesReport)];
                            }
                        }
                        if (weaponPickFn) PlayWeaponPickupSound(s, c.pickupSound);
                    } else if (IsArmorItem(c)) {
                        // ★ 09-29 — `pickupArmor_53E7F0` `:7628~7711` 그대로 · 입기는 `53E650(a4, 0)`(실패 소리 없음 — 직업·힘은 거기서 다시 봄)
                        //   같은 부위를 입었으면 — 그것이 운동화·로브·나무/강철 방패가 아니면 안 입음 · 로브면 로브에 수식이 없을 때만 바꿔 입음
                        //   방패가 아니면 입음(같은 부위면 바꿔 입음) · 방패면 — 입은 방패가 없으면 두 손 무기를 안 들었을 때 · 나무 방패를 입었으면 그게 수식 없을 때 ·
                        //   강철 방패를 입었으면 새것이 나무면 「입은 것 수식 없음 · 새것 수식 있음」 · 새것이 강철이면 입은 것 수식 없을 때
                        //   ⚠ `sub_419E60`(사람 깃발 · 뜻 미확인)은 늘 꺼진 것으로 · 부위를 못 읽은 것(SUBCLASS 0)은 안 입음(원본엔 없는 안전판)
                        const uint16_t kSneak = kItemStreetSneakers, kRobe = kItemWizardRobe, kWood = kItemWoodenShield, kSteel = NamedType("SteelShield");
                        auto noMods = [](const Session::Carried& x) { for (int m = 0; m < 4; ++m) if (x.mods[m] != 0xff) return false; return true; };
                        const Session::Carried* same = nullptr;
                        for (const auto& x : s.inventory)
                            if (x.netCode != pnc && x.equipped && IsArmorItem(x) && x.subclass == c.subclass) { same = &x; break; }
                        int decide = -1;                                  // -1 아직 · 0 안 입음 · 1 입음
                        if (c.subclass == 0) { decide = 0; ++autoEquip_[4]; }
                        else if (same) {
                            const uint16_t st = same->type;
                            if (st != kWood && st != kSteel && st != kSneak && st != kRobe) { decide = 0; ++autoEquip_[2]; }
                            else if (st == kRobe) { decide = noMods(*same) ? 1 : 0; if (!decide) ++autoEquip_[2]; }
                        }
                        if (decide < 0) {
                            if (!(c.subclass & 2)) decide = 1;
                            else if (!same) { decide = (WeaponEquipOf(s) & 0x7FFE40C) ? 0 : 1; if (!decide) ++autoEquip_[3]; }
                            else if (same->type == kWood) decide = noMods(*same) ? 1 : 0;
                            else if (same->type == kSteel) {
                                if (c.type == kWood) decide = (noMods(*same) && !noMods(c)) ? 1 : 0;
                                else if (c.type == kSteel) decide = noMods(*same) ? 1 : 0;
                                else decide = 0;
                            } else decide = 0;
                            if (!decide && same) ++autoEquip_[2];
                        }
                        if (decide == 1 && PlayerEquipArmor(s, pnc, /*failSound=*/false, out)) ++autoEquip_[1];
                    }
                }
                ++handled_;
                break;
            }

            // ── 버리기 ────────────────────────────────────────────────
            //
            // ★ 09-28 사람 판 — 「아이템이 안 버려진다」. `72 TRY_DROP <번호2> <x2> <y2>` 를 받는 곳이 **아예 없었다.**
            //   정답지 `sdecode.c:99~116` → `drop_4ED810`(`GAME3_3.c:6600~6648`) → `drop_4ED790` → `dropDefault_4ED290`(`:6370~6466`)
            //     ① 관전(+3680 & 3)이면 안 받음 ② 가방에서 번호로(`equipedItemByCode_4F7920`)
            //     ③ 목표를 사람 자리에서 **75 안으로** 당김(`:6625~6628`) ④ 벽에 막히면 글 `drop.c:DropNotAllowed` + 소리 925(`:6644~6646`)
            //     ⑤ `sub_4ED0C0`(`:6304~6365`) — 벗기(갑옷 `53E430`·무기 `53A140` 을 a3 = 0 · a4 = 1 → **본인 알림 없이 남에게만**) →
            //        본인에게 `4d REPORT_DROP <번호2> <종류2>`(`netReportDrop_4D8B50` `GAME3_2.c:4282~4290`) → 가방에서 뺌
            //     ⑥ 그 자리에 세계로(`createAt_4DAA50`) — 소개는 시야 갱신이 한다(TakeStatic 이 소개 기록을 지웠으니 다시 소개된다)
            //   ⚠ 안 옮긴 것 — (+280·+284)·(+16 & 2) 막이(뜻 미확인) · 못 버리는 물건(`53EBF0`·`53EC80` → CantDropThat) ·
            //     물건별 버리기 함수(a2[178] — 음식·보물·깃발·글리프) ·
            //     소리 821(CLASS 0x40) · 무게 넘침 깃발(+3656) 다시 셈 · 횃불·등불 버프 끄기
            case Op::TRY_DROP: {
                if (!s.inGame || it.bodyLen() != 6) break;
                auto gate = [&](DropGate g) { ++dropGate_[static_cast<size_t>(g)]; ++handled_; };
                const uint8_t* b = it.body();
                const uint16_t nc = static_cast<uint16_t>(b[0] | (b[1] << 8));
                if (s.observer) { gate(DropGate::Observer); break; }
                size_t at = s.inventory.size();
                for (size_t i = 0; i < s.inventory.size(); ++i)
                    if (s.inventory[i].netCode == nc) { at = i; break; }
                if (at == s.inventory.size()) { gate(DropGate::NotInBag); break; }
                // ③ 75 안으로
                const double px = s.posX, py = s.posY;
                double tx = static_cast<double>(b[2] | (b[3] << 8)), ty = static_cast<double>(b[4] | (b[5] << 8));
                const double dx = tx - px, dy = ty - py, d = std::sqrt(dx * dx + dy * dy);
                if (d > 75.0) { tx = dx * 75.0 / d + px; ty = dy * 75.0 / d + py; }
                // ④ 벽
                if (!MapTraceRay(static_cast<float>(px), static_cast<float>(py), static_cast<float>(tx), static_cast<float>(ty), 0)) {
                    SendPriMsg(s, "drop.c:DropNotAllowed", out);
                    PlaySoundAt(925, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
                    gate(DropGate::Wall);
                    break;
                }
                // ⑤⑥ 벗기·`4d`·가방에서 빼기·세계에 놓기·사라짐 시계 — 죽을 때 떨어뜨리기(B4b)와 함께 쓴다
                DropCarriedToWorld(s, at, tx, ty, /*dead=*/false, out);
                gate(DropGate::Dropped);
                break;
            }

            // ── 쓰기 ──────────────────────────────────────────────────
            //
            // ★ **무슨 일이 나는지가 파일에 있다** — `thing.bin` 의 `USE = PotionUse 50`.
            //   빨강·파랑 물약이 둘 다 50, 작은 물약 3, 음식 10. 지어낸 숫자가 아니다.
            // ⚠ 숫자를 회복량으로 읽어도 되는 갈래는 `PotionUse`·`ConsumeUse` 뿐이라
            //   나머지(`ReadUse` 70종·`WandUse`…)는 **아무 일도 안 일어난다.**
            //   「안 된다」를 「0 만큼 된다」로 꾸미지 않는다.
            case Op::TRY_USE: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                size_t at = s.inventory.size();
                for (size_t i = 0; i < s.inventory.size(); ++i)
                    if (s.inventory[i].netCode == nc) { at = i; break; }
                if (at == s.inventory.size()) { ++handled_; break; }

                const Session::Carried c = s.inventory[at];
                // ★ 묶음 5-d — **무엇을 부를지는 `USE` 의 앞말이 정한다**(`UseFn`).
                //   전에는 무엇이든 물약 길로 보냈다 — 사과·버섯·사이다가 조용히 아무 일도 안 했다.
                bool spent = false;
                switch (c.useFn) {
                    case UseFn::Potion:         spent = UsePotion(s, c, out); break;
                    case UseFn::Consume:        spent = UseConsume(s, c, out); break;
                    case UseFn::Mushroom:       spent = UseMushroom(s, out); break;
                    case UseFn::ConsumeConfuse: spent = UseCiderConfuse(s, c, out); break;
                    case UseFn::WandCast:       spent = UseWandCast(s, s.inventory[at], out); break;   // 5-e — 충전만 깎고 안 없어짐
                    default: break;             // 아직 안 옮긴 갈래(책·완드·활) — 아무 일도 안 난다
                }
                if (spent) {
                    // 쓴 것은 없어진다. 세계에 되돌려 놓지 않는다.
                    s.inventory.erase(s.inventory.begin() + static_cast<long>(at));
                    ++used_;
                } else {
                    ++useUnknown_;
                }
                ++handled_;
                break;
            }

            // ── 입기·벗기 ─────────────────────────────────────────────
            //
            // ⭕ `60 REPORT_EQUIP`(3B, 물건 번호)은 정품 표본 **22건**으로 확인했다
            //   (`60 cc 02` — `4c` 로 준 물건 716 을 그대로 가리킨다).
            // ⚠ **남들에게 겉모습을 알리는 `0x4F`~`0x52` 는 아직 안 보낸다.**
            //   길이는 알지만(6·10) 짜임 안에 **뜻 모르는 u32 한 칸**이 있고
            //   읽을 수 있는 정품 덤프 전부에서 **표본이 0건**이다.
            //   모르는 칸을 채워 보내는 것은 이 클라에게 두 번 당한 짓이라 하지 않는다.
            //   → 실클라로 한 번 입어 보면 표본이 생긴다. 그때 켠다.
            case Op::TRY_EQUIP:
            case Op::TRY_DEQUIP: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const bool wear = (static_cast<Op>(it.op()) == Op::TRY_EQUIP);
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                // ★ 09-27 B3 — 정답지 해독(`server__network__sdecode.c:160~192`): 관전(+3680 & 3 — 우리는 &1 관전만 들고 있다)이면 안 받는다 ·
                //   가방에서 번호로 찾는다(`equipedItemByCode_4F7920`) · 입기 = 무기 들기 `53A420` 또는 갑옷 입기 `53E650`(`GAME3_3.c:10557~10565`).
                //   무기 클래스(WEAPON·WAND)면 무기 쪽만 탄다 — 갑옷 입기는 제 클래스 관문(ARMOR)에서 거절하므로 결과가 같다.
                //   ⚠ 갑옷·그 밖은 아직 옛 켜고 끄기(갑옷 입기 `53E650`·벗기 `53E430` 이식은 계획 밖 — 미결)
                if (s.observer) { ++handled_; break; }
                Session::Carried* c = nullptr;
                for (auto& x : s.inventory) if (x.netCode == nc) { c = &x; break; }
                if (!c) { ++handled_; break; }
                if (c->weaponClass) {
                    if (wear) PlayerEquipWeapon(s, nc, /*failSound=*/true, out);   // `playerTryEquip_4F2F70` → 53A420(a1, it, 1, 1)
                    // ★ 09-27 B4 — 공격 중(상태 1)에 석궁(무기 SUBCLASS 8 — 우리 장비 비트도 CrossBow = 8)을 벗기려 하면 요청을 버린다(`sdecode.c:185~186`)
                    else if (s.moveState == kPlayerStateAttack && (c->equipBit & 0x8)) ++dequipGate_[static_cast<size_t>(DequipGate::CrossbowAttacking)];
                    else PlayerDequipWeapon(s, nc, out);                           // `playerTryDequip_4F2FB0` → 53A140(a1, it, 1, 1)
                    ++handled_;
                    break;
                }
                // ★ 09-29 — 갑옷 입기 `53E650(a1, 것, 1, 1)` · 벗기 `53E430(a1, 것, 1, 1)`(`GAME3_3.c:10557~10571`) — 갑옷이 아니면 둘 다 0(아무 일 없음).
                //   ✖ 전에는 옛 켜고 끄기 — 직업·힘 관문 없음 · 같은 부위를 둘 입음 · 방어값은 직업 표(입어도 안 바뀜)
                //   (09-28 사람 판 「워해머를 든 채 방패를 다시 입으면 입혀진다」의 두 손 무기 내려놓기는 입기 끝 `sub_53E600` 으로 옮김)
                if (wear) PlayerEquipArmor(s, nc, /*failSound=*/true, out);
                else PlayerDequipArmor(s, nc, out);
                ++handled_;
                break;
            }

            // ── 인벤토리가 꽉 찼다고 클라가 되돌린 것 ─────────────────
            //
            // ★ **이건 클라가 보낸다.** 우리가 준 물건을 못 받겠다는 뜻이라
            //   **도로 그 자리에 떨궈야** 한다. 안 그러면 물건이 세상에서 사라진다.
            case Op::INVENTORY_FAIL: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                for (size_t i = 0; i < s.inventory.size(); ++i) {
                    if (s.inventory[i].netCode != nc) continue;
                    WorldStatic back;
                    back.netCode = nc;
                    back.type = s.inventory[i].type;
                    back.modifiable = s.inventory[i].modifiable;
                    back.useAmount = s.inventory[i].useAmount;
                    back.equipBit = s.inventory[i].equipBit;
                    back.isWeapon = s.inventory[i].isWeapon;
                    back.weaponClass = s.inventory[i].weaponClass;   // ★ 09-27 B3
                    back.wandClass = s.inventory[i].wandClass; back.subclass = s.inventory[i].subclass;   // ★ 09-27 B5
                    back.weight = s.inventory[i].weight;
                    back.isFood = s.inventory[i].isFood;
                    back.foodBits = s.inventory[i].foodBits;
                    back.useFn = s.inventory[i].useFn;
                    back.charges = s.inventory[i].charges; back.chargesMax = s.inventory[i].chargesMax;
                    back.wandSpell = s.inventory[i].wandSpell; back.wandCooldown = s.inventory[i].wandCooldown;
                    back.decayExempt = s.inventory[i].decayExempt;   // ★ 09-28 — 들고 간다(사라짐 시계는 버릴 때만 건다)
                    back.dropFn = s.inventory[i].dropFn;
                    for (int m = 0; m < 4; ++m) back.mods[m] = s.inventory[i].mods[m];
                    back.pickupFn = s.inventory[i].pickupFn; back.pickupSound = s.inventory[i].pickupSound;   // ★ 09-29
                    back.potionPickup = (back.pickupFn == PickupFn::Potion);
                    back.bowReload = s.inventory[i].bowReload;
                    back.x = s.x;
                    back.y = s.y;
                    statics_.push_back(back);
                    freshStatics_.insert(nc);   // ★ 09-28 — 시야 반경이 꺼진 판에서도 소개되게(버리기와 같은 사정)
                    s.inventory.erase(s.inventory.begin() + static_cast<long>(i));
                    ++dropped_;
                    break;
                }
                ++handled_;
                break;
            }

            // ── 마우스 조준 ───────────────────────────────────────────
            //
            // ⭕ 짜임은 실측이다 — 본문 4바이트 = **월드 좌표 x2·y2**.
            //   정품 클라 표본이 **18,700건 넘는다**(CTF 실전 포함). 우리는 그동안
            //   **안 읽고 버렸다** — 그래서 투사체가 늘 몸이 바라보는 쪽으로만 났다.
            //
            // ⚠ 아주 자주 온다(한 판에 수천 건). 담아만 두고 계산은 쏠 때 한 번 한다.
            case Op::MOUSE: {
                if (!s.inGame || it.bodyLen() != 4) break;
                const uint8_t* b = it.body();
                s.aimX = static_cast<int16_t>(b[0] | (b[1] << 8));
                s.aimY = static_cast<int16_t>(b[2] | (b[3] << 8));
                s.aimAtFrame = frame_;
                s.hasAim = true;
                ++aims_;
                ++handled_;
                break;
            }

            // ── 채팅 ──────────────────────────────────────────────────
            //
            // ★ 이 갈래가 없어서 **사람이 친 말이 아무 데도 안 갔다.** 서버는
            //   `SendNotice` 로 보내기만 했다(퀘스트 전멸 알림 한 줄).
            //
            // ⭕ **보내는 짜임은 확정이다** — 정품 표본 26건이 전수 같은 꼴이다:
            //     `cb 02 02 20 0c cc 09 fa 00 00` + 글자
            //      번호2 · 깃발1(0x02=UTF-8) · x2 · y2 · 크기1 · 미상2
            //   x,y 는 **말하는 사람의 그때 좌표**를 따라간다(표본에서 말할 때마다 바뀐다).
            //
            // ⚠ **받는 짜임은 표본이 없다** (2026-09-10 전수). 「클라→서버 277건」으로
            //   보이던 것은 조각을 잘못 쪼갠 잡음이었다 — 277건이 전부 **같은 바이트**이고
            //   전부 조각 한가운데(자리14/30)다. `0xd8` 15,700건과 같은 지문이다.
            //   그래서 꼴은 `Wire.h` 의 규칙(opennox `msg_text.go`)에만 근거한다.
            //
            // ★★ 그래서 **되돌릴 때 클라가 준 머리를 안 쓴다.** 글자만 꺼내고
            //   번호·좌표는 **서버가 아는 값으로 새로 짓는다.** 까닭 둘 —
            //     ① 받는 짜임이 표본 없이 소스 근거뿐이라 머리를 믿을 수 없다
            //     ② 머리를 그대로 실으면 **남의 번호로 말하기**가 된다
            case Op::TEXT_MESSAGE: {
                if (!s.inGame || it.bodyLen() < 10) break;
                const uint8_t* b = it.body();
                size_t sz = b[7];
                if ((b[2] & 0x02) == 0 && (b[2] & 0x08) == 0) sz *= 2;   // UTF-16 이면 두 배
                if (sz > it.bodyLen() - 10) sz = it.bodyLen() - 10;
                if (sz == 0 || sz > kChatMaxBytes) { ++handled_; break; }
                BroadcastChat(s, b + 10, sz, out);
                ++chats_;
                ++handled_;
                break;
            }

            // ── 클라가 판 시계를 물어봤다 ────────────────────────────
            //
            // ★ 이 갈래는 **조각을 죽이던 자리**다. 길이표가 `-1`(미상)이라 조각을 훑는
            //   걸음이 여기서 멈추고 **그 조각을 통째로 버렸다** — 같이 실린 정상
            //   메시지까지 같이 죽었다. 09-10 실클라 런의 `[버림]` 228건이 전수 이것이다.
            //   본문 0 은 실측이다(정품 클라 표본 336건이 전수 「남은 1」 = 홑 `d4`).
            //
            // 답은 주기 발송과 **같은 내용을 그 사람에게만** 보낸다.
            case Op::REQUEST_TIMER_STATUS: {
                if (!s.inGame) break;
                // ★ 09-21 진단 — 클라는 `d3` 의 프레임 도장이 **제 시계보다 30 이상 어긋나면**
                //   「낡았다」며 이 요청을 다시 보낸다(`cdecode.c:2638` — **부호 없는 뺄셈**이라
                //   도장이 클라 시계보다 **앞서도** 낡은 것으로 친다). 답이 또 어긋나면 **무한 되풀이**다.
                //   실측 — 한 판에 `시계요청 9979`. 도장(`frame_`)과 클라 시계(`tickFrame`)의 차를
                //   **값이 바뀔 때만** 찍는다(횟수로 자르면 사건 전에 예산이 바닥난다).
                {
                    const long long diff = static_cast<long long>(s.tickFrame) - static_cast<long long>(frame_);
                    if (!s.dbgTimerDiffSet || diff != s.dbgTimerDiff) {
                        std::printf("[시계요청] %s 프레임 %u · 그 사람 시계 %u · 차(시계−도장) %lld\n",
                                    s.addr.c_str(), frame_, s.tickFrame, diff);
                        std::fflush(stdout);
                        s.dbgTimerDiff = diff;
                        s.dbgTimerDiffSet = true;
                    }
                }
                uint8_t msg[13];
                BuildTimerStatus(msg, ClockOf(s));
                EmitSeq(s, msg, sizeof(msg), out);
                ++timerAsks_;
                ++handled_;
                break;
            }

            // ── 정상 종료 알림 ──────────────────────────────────────
            //
            // ⚠ **정품 클라가 이걸 안 보낼 수도 있다** — 코드 주석에 「정품 클라도 `0a` 도
            //   `ae` 도 없이 프로세스가 죽는다」는 실측이 남아 있다. 그래서 이건
            //   **타임아웃(30초)을 대신하는 것이 아니라 앞당기는 것**이다.
            //   실제로 오는지는 `[처음]` 진단 줄로 관측한다.
            //
            // ⚠ 여기서 세션을 지우면 **지금 잡고 있는 `s` 참조가 죽는다.**
            //   표시만 하고 걷어내기는 Tick 의 「자리 비우기」가 한다 — 나감 알림
            //   (`2e`·`31`)과 깃발 떨구기가 이미 거기 있다.
            // 클라가 나간다 — 두 가지 길로 온다.
            //
            //   `0x0a CLIENT_CLOSE`     접속 자체를 닫는다
            //   `0xae OUTGOING_CLIENT`  **판에서 빠진다** (클라 원본 `network.go:77`)
            //
            // ★ `0xae` 는 `SendReliable` + `WaitServerResponse` 로 온다 — **답을 기다린다.**
            //   전에는 길이를 2 로 잘못 알아 조각째 버렸고(2026-09-11 63건), 답이 없으니
            //   클라가 깨끗하게 못 나갔다. 그것이 「재접속 안 됨」으로 보였다.
            case Op::OUTGOING_CLIENT:
            case Op::CLIENT_CLOSE: {
                s.closing = true;
                ++handled_;
                break;
            }

            case Op::TRY_SPELL: {
                // ★★ 2026-09-13 묶음 4-a — 정답지 받기 그대로(sdecode:211 MSG_TRY_SPELL · 본문 21).
                //   u32 × 5 주문(글리프면 여럿) · u8 대상 깃발(0 이 아니면 자기에게). 여기서는 **넣기만** 한다 —
                //   음소를 3틱마다 하나씩 따라가다 끝나야 검사·마나·효과가 난다(SpellCastByBook · PlayerSpell).
                //   ⚠ 전에는 받자마자 마나를 깎고 `70` 을 중요로 보내고 효과를 냈다(자리표시). `70` 은 Kind1(틱 목록)이다.
                if (!s.inGame || it.bodyLen() != kTrySpellBodyLen) break;
                uint32_t sp[5] = {};
                for (int k = 0; k < 5; ++k) {
                    const auto* b = it.body() + 4 * k;
                    sp[k] = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                            (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
                }
                const bool targetSelf = it.body()[20] != 0;
                bool ok = true;
                if (s.observer) {
                    SendPriMsg(s, "GeneralPrint:NoSpellWarningGeneral", out);   // 3680 & 1
                    ok = false;
                }
                // 3680 & 2(몬스터 지켜보기 — "ConjureNoSpellWarning1") 는 관전 기능(8)
                // ★ 09-23 nine-defects 3-b — 협동(2048)이 아니면 **떠 있는 동안(+16 & 0x4000) 시전 거절** · 알림 없음(`sdecode.c:221~226`)
                if (!(gameSettings_.gameFlags & 0x800u) && (PlayerObjFlags(s) & 0x4000)) {
                    if (ok) ++spellRefusedAirborne_;
                    ok = false;
                }
                if (ok) {                                  // 게임 깃발 128(Chat 모드) 아님
                    int count = 0;
                    for (int k = 0; k < 5; ++k) if (sp[k]) ++count;
                    const bool quest = (questTotal_ > 0 || questStage_ > 0);
                    bool friendGate = false;               // 「주문 하나 · 공격 주문 · 커서 오브젝트가 친구」면 넣지도 않는다
                    if (count == 1 && sp[0] < 137 && (SpellFlags(static_cast<uint8_t>(sp[0])) & 0x20) && !quest) {
                        const Target cur = FindObjectAtCursor(s);
                        if (!cur.none() && !TargetIsEnemy(s, cur)) friendGate = true;
                    }
                    if (!friendGate && !SpellByBookInsert(s, sp, count, 3, targetSelf, out) && count == 1) {
                        for (int k = 0; k < 5; ++k) if (sp[k]) ReportSpellStat(s, sp[k], 0, out);
                    }
                }
                ++handled_;
                break;
            }

            // 10-e SYSOP_PW 0xBC -> SYSOP_RESULT 0xBD (network_server.go:191~205)
            //   body is a 20-byte UTF-16 password; empty server password always fails.
            case Op::SYSOP_PW: {
                if (it.bodyLen() < 20) break;
                std::string got;
                for (size_t k = 0; k + 1 < 20; k += 2) {
                    const uint16_t c = static_cast<uint16_t>(it.body()[k] | (it.body()[k + 1] << 8));
                    if (c == 0) break;
                    if (c < 0x80) got.push_back(static_cast<char>(c));
                }
                const bool okPw = !sysopPassword_.empty() && sysopPassword_ == got;
                if (okPw) s.sysop = true;
                const uint8_t res[2] = { static_cast<uint8_t>(Op::SYSOP_RESULT),
                                         static_cast<uint8_t>(okPw ? 1 : 0) };
                EmitImportantRaw(s, res, sizeof(res), out);
                ++handled_;
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
                // ★ 다만 **답은 한다** — 정품은 이 두 번째 요청에 0x04 를 하나 돌려준다
                //   (3바이트·비신뢰·본문 0). 09-05 정품 덤프의 자리 그대로다.
                //   무엇인지는 아직 모른다(PROTOCOL.md 도 「미상」).
                if (s.keyed) {
                    const uint8_t m4[1] = {static_cast<uint8_t>(Op::CODE4)};
                    Emit(s, /*reliable=*/false, /*stream=*/0, m4, sizeof(m4), out);
                    ++handled_;
                    break;
                }
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
                    // ★★ 서두가 있으면 그 바로 앞 프레임을 준다 — 시계가 한 줄로 이어지게.
                    //   전역 프레임을 주면 늦게 붙은 사람은 서두보다 앞서서, 그 뒤 갱신이
                    //   전부 과거로 보여 버려진다(2026-09-08 둘째 클라 검은 화면의 원인).
                    s.frame = preludeFirstFrame_ ? preludeFirstFrame_ - 1 : frame_;
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
                ++acceptHeard_;                            // 1-a — 갈래 셋 중 하나
                // ★★ 기본값을 **묶음을 만들기 전에** 채운다.
                //    예전에는 Emit 뒤에 채워서 접속 묶음의 오브젝트 번호가 0 으로 나갔다.
                //    클라는 「네 캐릭터 번호는 0」을 받으니 카메라를 붙일 대상이 없어
                //    화면이 검은 채로 멈춘다(2026-09-07, 정품과 바이트 대조로 잡음).
                // ★ 스폰 자리 — 예전엔 (1000,1000) 이라는 지어낸 값이었다.
                //   estate.map 에서 정품이 쓰는 자리를 덤프에서 읽어 넣는다
                //   (2026-09-07 레퍼런스 상태방송 머리: x=2304 y=3498).
                // ★ 스폰 자리는 **맵마다 다르다**. 지금은 덤프의 맵을 그대로 쓰므로
                //   그 덤프에서 정품이 쓴 자리를 그대로 쓴다.
                //   09-05 덤프(bludeath.map)의 상태갱신 머리: x=3864 y=2534.
                //   ⚠ estate 좌표(2304,3498)를 bludeath 에 쓰면 맵 밖이라 아무것도 안 그려진다.
                // ★★ 그래서 **상수로 박지 않고 서두에서 읽는다**(`SetFirstSnapshot`).
                //   2026-09-08: 서두를 estate 로 바꿨는데 스폰이 bludeath 값 그대로라
                //   **사람만 세계에서 멀리 떨어져** 있었다. 위 경고가 반대로 실현된 것이다.
                // ★★★ **사람마다 자리를 흩뜨린다.** 전에는 모두 같은 자리에 세웠다 —
                //   둘이 **겹쳐 서면 화면에서 하나로 보인다.** 「서로 안 보인다」의
                //   후보로 오래 붙들고 있던 것이 실은 이것일 수 있다(2026-09-08).
                //   정품 2인 덤프에서도 둘은 (2627,3831) 과 (3174,2468) 로 멀리 떨어져 있었다.
                //   ★ 간격은 `--spawn-spread` 로 바꾼다 — 기본 40 은 근접 사거리(45) 안이라
                //     바로 싸움이 붙지만, **가려서 안 보이는 것**과 **안 그려지는 것**을
                //     가르려면 크게 벌려 봐야 한다(2026-09-09).
                if (s.x == 0 && s.y == 0) {
                    // ★★ 09-19 — **새 사람은 맵의 PlayerStart 에 선다**(정답지 `newPlayer`,
                    //   `player.go:416~426` — `mapFindPlayerStart_4F7AB0`). 전에는 아래 고정값만 써서
                    //   Estate·CapFlag·FlagWar **세 맵 모두 3864,2534** 에 섰다(09-19 실측).
                    //   ⚠ 깃발 맵이 어두운 것은 이 탓이 아니었다 — 고친 뒤에도 CapFlag 평균 밝기가 같다(R 19.7 → 19.0). 맵 조명이다.
                    //   ⚠ `--spawn-at`·`--item-at-spawn` 으로 못 박았으면 그 값이 먼저다 — 진단·시험이 거기에 기댄다
                    float fx = 0.0f, fy = 0.0f;
                    if (!spawnForced_ && FindPlayerStart(s, fx, fy)) {
                        s.x = static_cast<int>(fx);
                        s.y = static_cast<int>(fy);
                        ++spawnFromMap_;
                        std::printf("[첫자리] 맵 시작점 %d,%d (시작점 %zu곳 중)\n", s.x, s.y, playerStarts_.size());
                    } else {
                        const int n = static_cast<int>(inGameCount());
                        s.x = spawnX_ + (n % 4) * spawnSpread_;
                        s.y = spawnY_ + (n / 4) * spawnSpread_;
                        ++spawnFromDefault_;
                        std::printf("[첫자리] 고정값 %d,%d (%s)\n", s.x, s.y,
                                    spawnForced_ ? "못 박음 — --spawn-at 또는 --item-at-spawn" : "맵에 시작점 없음");
                    }
                }
                // ★ 오브젝트 번호는 **접속마다 달라야 한다**. 전에는 모두 713 이라
                //   둘째 클라가 첫째와 같은 번호를 받아 화면이 검었다(2026-09-08).
                // ★ 다섯 칸씩 띄운다 — 본인 + 장비 넷. Server.h 의 kNetCodeStride 참조.
                if (s.netCode == 0) {
                    s.netCode = nextPlayerNetCode_;
                    nextPlayerNetCode_ += kNetCodeStride;
                }
                // ★★ **직업과 레벨을 읽어 스탯을 정한다** (2026-09-10 기획데이터 이식).
                //   전에는 누구에게나 75/150 — 녹음본 주인공(위저드) 값이 굳어 있었다.
                //   그래서 워리어가 정본 150 대신 75 로 놀았다. 자리는 `GameBalance.h`.
                if (it.bodyLen() >= kJoinPlayerInfoLen)
                    s.info97.assign(it.body(), it.body() + kJoinPlayerInfoLen);
                if (it.bodyLen() > kInfoClassAt) {
                    const uint8_t c = it.body()[kInfoClassAt];
                    s.charClass = (c <= 2) ? static_cast<CharClass>(c) : CharClass::Warrior;
                    // ⚠ 레벨은 이 구조에 없다 — 온라인은 직업 최대 스탯을 그대로 준다.
                    s.charLevel = kMaxLevel;
                    // ★ **읽은 값을 찍는다** (2026-09-11). 화면에는 `Jack the Conjurer` 가
                    //   골라져 있는데 서버는 `마나0/0`(워리어 값)으로 봤다 — 어디서 어긋나는지
                    //   찍지 않으면 못 가른다. `원래 0x%02x` 가 51번 자리의 날바이트다.
                    std::printf("[직업] 원래 0x%02x → %s · 레벨 %u · 본문 %zu바이트\n",
                                c,
                                (c == 0 ? "워리어" : c == 1 ? "위저드" : c == 2 ? "컨저러" : "모름→워리어"),
                                (unsigned)s.charLevel, it.bodyLen());
                    std::fflush(stdout);
                } else {
                    // ⚠ 여기로 오면 직업을 **아예 못 읽는다** — 기본값(워리어)이 그대로 남는다.
                    std::printf("[직업] ✗ 본문이 %zu바이트뿐이라 직업을 못 읽었다 (%zu 넘어야 한다)\n",
                                it.bodyLen(), kInfoClassAt);
                    std::fflush(stdout);
                }
                if (s.maxHealth == 0) {
                    // ★ **온라인은 레벨을 안 본다** — 그 직업의 최대 스탯을 그대로 준다
                    //   (`GameBalance.h` HealthOnline 주석 · 정품이 접속자 레벨도 10 으로 민다).
                    s.maxHealth = static_cast<uint16_t>(HealthOnline(s.charClass));
                    s.maxMana   = static_cast<uint16_t>(ManaOnline(s.charClass));
                    // 온라인 = EngineAdmin → serverSetAllSpells(p, true, 0): 주문 1~136 전부 세기 3(spells.go:117) · 직업 거르기는 시전 때
                    for (int k = 1; k < 137; ++k) s.spellLvl[k] = 3;
                    s.strength  = StrengthOnline(s.charClass);
                    s.carryCap  = static_cast<uint16_t>(CarryCapacityOnline(s.charClass));   // +490(묶음 5)
                    GiveDefaultGear(s);                    // ★ 09-27 B1 — 기본 장비를 서버 가방에도(전에는 스냅샷으로 클라에만 → 입기 처리가 번호를 못 찾음)
                    s.health = s.maxHealth;
                    s.mana   = s.maxMana;
                    s.manaPrev = s.mana;                   // sub_4EFF10 +4 = +6 = 최대(`GAME3_3.c:8103`)
                    s.statsDirty = true;                   // plrReadVals_4EEDC0 끝 +2184 = 1(`GAME3_3.c:7723`) — 스냅샷 꼬리가 싣고 끈다
                    // ★ 진단 — 들어올 때 체력을 낮춰 둔다(`--start-health N`).
                    //   ⚠ **최대 체력은 안 건드린다** — 그래야 부활 뒤 「최대로 돌아왔나」를 잰다.
                    //   왜 필요한가: 실클라 졸업시험이 90초 동안 안 죽어 「죽음·부활」을 못 쟀다
                    //   (거머리 2마리가 38 밖에 못 깎았다, 2026-09-11). 낮춰 두면 금방 죽는다.
                    if (startHealth_ > 0 && startHealth_ < s.maxHealth)
                        s.health = static_cast<uint16_t>(startHealth_);
                    // ★ 진단 — 들어올 때 마나를 채운다(`--start-mana N`).
                    //   주먹(29) 같은 비싼 주문을 무인으로 쏘려면 필요하다
                    //   (09-18 실측 — 마나 5/125 로 시작해 여섯 번 막혔다).
                    //   ⚠ 최대 마나를 넘겨 채우지는 않는다.
                    if (startMana_ > 0)
                        s.mana = static_cast<uint16_t>(startMana_ > s.maxMana ? s.maxMana : startMana_);
                }
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
                } else if (standalone_) {
                    // ★ 독립 모드 — 녹음 없이 조립해 보낸다 (P5-c-2).
                    //   플레이어 정보 97B 는 방금 받은 CLIENT_ACCEPT 에서 그대로.
                    // ★ 09-19 — **캐릭터가 검은 실루엣**인 것의 첫 의심 자리.
                    //   `BuildNewPlayer`(JoinBundle.cpp:111~112)는 이 포인터가 nullptr 이면
                    //   **97바이트를 전부 0 으로** 채운다. 색도 0 = 검정이 된다.
                    //   클라는 `CLIENT_ACCEPT` 에 피부·머리·바지·윗옷 색을 담아 보낸다
                    //   (`gameloop.go:424~430` `info.Colors.*`)
                    acceptBodyLenLast_ = it.bodyLen();
                    const uint8_t* info97 =
                        (it.bodyLen() >= 97) ? it.body() : nullptr;
                    if (!info97) ++joinInfo97Null_; else ++joinInfo97Ok_;
                    // ★ 09-19 — 97B **내용**을 한 번 찍는다. 「실었다」와 「값이 들어 있다」는 다르다.
                    //   클라가 색을 0 으로 보내는 것이라면 우리 서버는 죄가 없다
                    if (info97 && joinInfo97Ok_ == 1) {
                        // ⚠ 09-19 — 앞 97B 가 **전부 0** 이었다. 자리가 틀린 것이니 본문을 통째로 본다
                        std::printf("[접속정보97] len=%zu | ", it.bodyLen());
                        for (size_t q = 0; q < it.bodyLen() && q < 200; ++q)
                            std::printf("%02x ", it.body()[q]);
                        std::printf("\n");
                        std::fflush(stdout);
                    }
                    std::vector<uint8_t> jb = BuildJoinBundle(s.netCode, s.frame, info97, gameSettings_);
                    Emit(s, /*reliable=*/true, /*stream=*/0, jb.data(), jb.size(), out);
                    ++handled_;
                }
                // 여기서부터 게임에 들어온 것으로 본다 — 매 틱 상태를 받는다.
                if (!s.inGame) {
                    s.joinOrder = ++nextJoinOrder_;
                    // 9-a — 비밀벽 「아는 상태」를 비운다. 다음 알림 때 깃발 8 대로 다시 채워진다
                    //   (`sub_4DF2A0` 이 접속 때 그 사람 비트를 미리 켜는 자리와 같은 몫).
                    //   ⚠ 안 비우면 **주소를 다시 쓰는 접속**이 옛 사람의 기억을 물려받는다
                    s.secretSeen.clear();
                }
                s.inGame = true;

                // ★ 깃발 판이 이미 서 있으면 **새로 온 사람에게도 팀을 준다.**
                //   전에는 두 명이 찬 그 순간 한 번만 나눠서, 셋째부터 팀 0 으로 남아
                //   깃발 판정에서 통째로 빠졌다(2026-09-11 감사).
                if (!flags_.empty()) { AssignTeams(); SendTeamState(out); }   // 8-b — 나눈 뒤 알린다

                if (standalone_ && useMapBody_.empty()) {
                    std::vector<uint8_t> um = BuildUseMap(saMapName_, saMapCrc_, s.frame);
                    Emit(s, /*reliable=*/true, /*stream=*/0, um.data(), um.size(), out);
                    ++useMapSent_;                         // 1-a — 갈래 셋 중 하나(조립 쪽)
                    ++handled_;
                    // 스냅샷도 예약 — 발송 자리에서 조립본을 보낸다
                    s.snapshotIn = kSnapshotDelayTicks;
                }

                if (!useMapBody_.empty()) {
                    std::vector<uint8_t> msg = useMapBody_;
                    if (msg.size() > kUseMapFrame + 3)
                        PutU32(msg.data() + kUseMapFrame, s.frame);
                    Emit(s, /*reliable=*/true, /*stream=*/0, msg.data(), msg.size(), out);
                    ++useMapSent_;                         // 1-a — 녹음 쪽도 같이 센다(자리가 둘이다)
                    ++handled_;
                }
                // ★ 맵 지정 뒤에 정품이 더 보내는 것들을 그대로 이어 보낸다.
                for (const auto& p : postMap_) {
                    if (p.empty()) continue;
                    Emit(s, /*reliable=*/true, /*stream=*/0, p.data(), p.size(), out);
                    ++handled_;
                }
                // ★★★ **첫 세계 스냅샷** — 이 한 조각에 사람을 그리는 데 필요한 것이 다 있다.
                //   정품 덤프를 나란히 놓고 재어 확정했다(2026-09-08, 487바이트 조각 하나):
                //
                //     27 a4 b2 a7 aa e4 cc(e4) cc(dd) de cc(e9) cc(4c) cc(60) cc(4c) cc(60)
                //     cc(4c) cc(60) cc(4c) cc(44) cc(60) cc(6a) ef cc(b3) 67 cc(5a) 49
                //     cc(dd) de 48 cc(43)
                //
                //   `0xe9` 살려내기 · `0x60` 장비 넷 · `0x4c` 집을 것 넷 ·
                //   `0xdd`/`0xde` 최대 체력·마나 · `0x43` 현재 체력 · `0x48` 능력치.
                //   이걸 안 보내면 **이름표만 뜨고 몸이 없고 막대가 텅 빈다** — 실측 증상 그대로다.
                //
                // ⚠ 뽑아만 두고 **아무도 안 보내고 있었다**. `SetFirstSnapshot` 은 정의만
                //   있었고 부르는 곳이 없었다 — 이 파일 머리 주석이 경고하던 그 빠진 배선이다.
                // ⚠⚠ **여기서 바로 보내면 안 된다.** 클라가 아직 맵을 안 읽었다 —
                //   같은 묶음에 붙여 보냈더니 화면이 검은 채로 남았다(2026-09-08 실측).
                //   정품처럼 몇 틱 뒤로 미룬다. 실제 발송은 `Tick` 에서.
                if (!firstSnapshot_.empty()) s.snapshotIn = kSnapshotDelayTicks;

                // ★★★ **사람 소개를 서로 주고받는다** — `2d NEW_PLAYER`.
                //   내가 보내던 것은 **접속한 본인 것 하나뿐**이었다. 정품은 다르다
                //   (2026-09-09 대조: 정품 클라 A 가 자기 `cb 02` 와 상대 `d0 02` 를
                //    둘 다 받았고, 내 서버에서 A 는 자기 `c9 02` 하나만 받았다).
                //   이 소개가 없으면 클라에 **그 사람의 playerInfo 가 아예 없다** —
                //   오픈녹스 원본을 보면 `2d` 가 이름·배운 주문(100번 자리)·장비·
                //   상태 플래그(112번 자리)를 통째로 실어 나른다
                //   (`nox_xxx_netNewPlayerMakePacket_4DDA90`). 상태 알림(`6a`)도
                //   `playerInfoGetByID` 가 빈손이면 그 자리에서 되돌아간다.
                //
                //   묶음(216바이트) 안 `2d` 는 87번 자리에서 시작해 129바이트다 —
                //   6+5+7+20+49 로 딱 떨어진다(Server.h 의 kJoin* 주석).
                //   ⚠ **여기서 바로 보내지 않는다.** 정품은 접속 답장에 붙이지 않고
                //     조금 뒤 다른 것들과 묶어 보낸다(2026-09-09 정품 덤프: 208바이트
                //     조각의 79번 자리). 접속 답장에 붙였더니 재생 대조(`replay_connect`)가
                //     「정품은 여기서 다른 것을 보냈다」로 두 자리를 잡아냈다.
                //     실제 발송은 `Tick` 에서 — 첫 스냅샷과 같은 자리다.
                s.pendingIntro = true;

                // ★★★ **먼저 와 있던 사람들에게 이 사람의 장비만 보낸다.**
                //   미니맵에는 둘이 찍히는데 몸이 안 그려진다 = 오브젝트는 있고
                //   **입힐 것이 없다**. 아침에 자기 몸이 안 그려지던 것과 같은 증상이다.
                //   ⚠ 스냅샷을 통째로/껍데기 전부 보내면 **받는 클라가 죽는다**(실측).
                //     정품 순서에서 장비에 해당하는 `cc(4c)`(집을 것) + `cc(60)`(장착)
                //     **네 쌍만** 골라 보낸다.
                //   ★ `--no-newgear` 로 끌 수 있다 — Server.h 의 SetNotifyNewGear 참조.
                if (notifyNewGear_ && !firstSnapshot_.empty()) {
                    std::vector<uint8_t> patched;
                    const std::vector<uint8_t>& src =
                        PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, patched, forceHudOn_)
                            ? patched : firstSnapshot_;
                    std::vector<uint8_t> gear;
                    size_t q = 0;
                    while (q + 4 <= src.size()) {
                        if (src[q] != static_cast<uint8_t>(Op::SEQ_IMPORTANT)) { ++q; continue; }
                        const size_t inner = src[q + 3];
                        if (q + 4 + inner > src.size()) break;
                        const uint8_t iop = (inner >= 1) ? src[q + 4] : 0;
                        // ★ `4c`(집을 것)+`60`(장착)만 보내니 **몸은 그려지는데 누워 있었다.**
                        //   정품 순서에서 그 앞에 오는 `e9`(살려내기)와
                        //   `dd`/`de`(최대 체력·마나)를 같이 보내 세운다.
                        //   ⚠ 더 넣으면 나빠진다 — 하나씩 재서 확인했다(2026-09-08):
                        //     `e9`+`dd`+`de` 셋 다  → 받는 클라가 **죽는다**
                        //     `e9`(살려내기)만      → 상대가 **아예 안 보인다**(오브젝트가 사라짐)
                        //   **`4c`+`60` 둘만**이 맞다. 다만 상대가 **누워 있다** —
                        //   세우는 것은 다른 자리에서 풀 것(자세/애니 쪽으로 보인다).
                        // ★ 어떤 오프코드를 넘길지는 `--peer-ops` 로 바꿀 수 있다.
                        //   「무엇을 더 보내야 상대가 그려지나」를 다시 컴파일하지 않고
                        //   한 번에 쓸어보려고 손잡이로 뺐다(2026-09-09).
                        if (peerOps_.count(iop) != 0)
                            gear.insert(gear.end(), src.begin() + q, src.begin() + q + 4 + inner);
                        q += 4 + inner;
                    }
                    if (!gear.empty()) {
                        for (auto& kv2 : sessions_) {
                            Session& o = kv2.second;
                            if (!o.inGame || o.addr == s.addr) continue;
                            // ★ 녹음본에서 떼어 온 껍데기라 번호가 그 녹음 때 것이다.
                            //   받는 사람마다 **그 사람의 번호로 다시 매겨** 보낸다.
                            std::vector<uint8_t> g = gear;
                            RenumberSeq(o, g);
                            Emit(o, /*reliable=*/true, /*stream=*/0, g.data(), g.size(), out);
                            ++handled_;
                        }
                    }
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
// 서두(녹음본) 조각 하나에서 **사람 항목만** 이 사람 것으로 갈아 끼운다.
//
// 왜 필요한가 — 서두는 남이 접속했을 때 녹음한 것이라 그 사람의 번호(715)와
// 자리(2600,2578)가 박혀 있다. 그대로 흘리면 클라는 「내가 715, 저기에 있다」로
// 알았다가 내 갱신이 다른 번호·다른 자리를 말해 **딴 오브젝트로 본다**.
//
// ⚠ **무늬 찾기로 하지 않는다.** 뜯어서(DecodeUpdate) 고치고 다시 짠다(EncodeUpdate).
//   바이트열을 눈대중으로 찾으면 우연히 같은 자리를 건드린다.
// 돌려주는 값 = 갈아 끼운 것이 있으면 참(그때만 out 이 채워진다).
// ★★★ 서두에 실린 **남의 번호**를 이 사람 번호로 바꾼다.
//   `0xa4` 안의 사람 항목만 고치면 모자란다 — 장비·체력·부활은 `0xcc` 껍데기 안에
//   **대상 번호를 앞 2바이트로** 달고 온다. 그 번호가 남의 것이면 클라는
//   「713번 사람은 만들었는데 입힐 장비와 체력은 715번 것」이 되어 **몸을 못 그린다**.
//   실측(2026-09-08, 내 번호 713):
//       dd cb 02 4b 00 4b 00        최대 체력 → 0x02cb = 715
//       e9 cb 02 dd 07 00 00 ff 01  살려내기   → 715
//   길이를 안 바꾸는 **자리 치환**이라 뒤 스트림이 밀리지 않는다.
// 사람 하나가 차지하는 번호 칸 수 — 본인 + 장비 넷. Server.h 의 kNetCodeStride 와 같은 값이다.
static constexpr uint16_t kSlotSpan = 5;

static void PatchSeqTargets(std::vector<uint8_t>& buf, uint16_t from, uint16_t to,
                            bool forceHudOn) {
    if (from == to) return;
    size_t q = 0;
    while (q + 4 <= buf.size()) {
        if (buf[q] != static_cast<uint8_t>(Op::SEQ_IMPORTANT)) { ++q; continue; }
        const size_t inner = buf[q + 3];
        if (q + 4 + inner > buf.size()) break;
        // 껍데기 머리 4바이트 뒤가 속 메시지다. `[오프코드][대상번호 2]…` 꼴이면 바꾼다.
        //
        // ★★★ **사람 하나가 아니라 다섯 칸을 통째로 옮긴다** — 본인 + 장비 넷.
        //   정품은 사람마다 장비 번호가 다르다(715→716~719 · 720→721~724).
        //   내 서버는 사람만 옮기고 장비는 녹음본 번호(716~719)를 **둘 다 그대로** 써서
        //   두 사람이 같은 오브젝트를 가리켰다 — 미니맵엔 둘인데 몸은 하나였다(2026-09-09).
        //   ⚠ 한 칸씩 여러 번 훑으면 **먼저 바꾼 값을 다시 바꾼다**(715→713 뒤에 717→715 가
        //     또 걸린다). 그래서 한 번에 범위로 판정하고 한 번만 쓴다.
        if (inner >= 3) {
            const uint16_t id = static_cast<uint16_t>(buf[q + 5] | (buf[q + 6] << 8));
            if (id >= from && id < from + kSlotSpan) {
                const uint16_t nid = static_cast<uint16_t>(to + (id - from));
                buf[q + 5] = static_cast<uint8_t>(nid);
                buf[q + 6] = static_cast<uint8_t>(nid >> 8);
            }
        }
        // ★★★ **서두 안에 「HUD 를 숨겨라」가 들어 있다.**
        //   녹음본에 `cc 00 00 07 | 6a <번호2> 01 00 00 00` 이 그대로 있다(182번 자리).
        //   `01` 은 상태 bit0 = **GUI 안 그림**(오픈녹스 `nox_client_onClientStatusA`:
        //   `v&1` 이면 renderGUI=false). 접속하자마자 이걸 받으니 HUD 가 없는 것이 당연했다.
        //   `--hud-on` 을 주면 그 자리를 0 으로 뒤집는다.
        //   ⚠ 정품은 처음에 `01` 을 주고 놀기 시작할 때 `00` 을 준다 — 그러니 이건
        //     「정품 흉내」가 아니라 **가르는 시험**이다. 켜서 HUD 가 뜨면 이 자리가 맞는 것.
        if (forceHudOn && inner >= 7 &&
            buf[q + 4] == static_cast<uint8_t>(Op::REPORT_CLIENT_STATUS)) {
            buf[q + 7] = 0; buf[q + 8] = 0; buf[q + 9] = 0; buf[q + 10] = 0;
        }
        q += 4 + inner;
    }
}

// 껍데기(`0xcc`) **밖에** 맨몸으로 실린 메시지의 대상 번호를 옮긴다.
//
// ⚠ **아무 자리나 바꾸면 안 된다.** 두 바이트가 우연히 남의 번호와 같은 좌표일 수 있다.
//   그래서 **머리 바로 뒤 두 바이트가 오브젝트 번호인 것이 와이어로 확인된 오프코드**만
//   손댄다. 지금 목록의 근거(2026-09-09 정품·내 서버 덤프에서 눈으로 본 원바이트):
//     de  `de cb 02 96 00 96 00`      최대 마나  = <번호2> <지금2> <최대2>
//     48  `48 cb 02 4b 00 96 00 72`   능력치     = <번호2> …
//   ★ 늘리려면 **그 오프코드의 원바이트를 먼저 볼 것.** 짐작으로 넣지 말 것.
static bool BareTargetOpcode(uint8_t op) {
    return op == static_cast<uint8_t>(Op::REPORT_TOTAL_MANA) ||
           op == static_cast<uint8_t>(Op::REPORT_STATS);
}

// ★ 몇 자리를 고쳤고 **어디서 걷기가 멈췄나**를 같이 돌려준다.
//   「0 자리 고침」만 보면 「고칠 게 없었다」와 「거기까지 못 갔다」가 안 갈린다.
//   실제로 두 번을 그렇게 헛짚었다(2026-09-09).
struct BarePatchStat { int patched = 0; size_t stoppedAt = 0; size_t total = 0; };

static BarePatchStat PatchBareTargets(std::vector<uint8_t>& buf, uint16_t from, uint16_t to) {
    BarePatchStat st;
    st.total = buf.size();
    if (from == to) return st;
    // ★★ **걷지 않고 훑는다.** 처음에는 메시지를 차례로 걸어가며 고치려 했는데
    //   `0xaa MSG_IMPORTANT` 에서 길이가 어긋나 **187/543 바이트에서 섰다** —
    //   고칠 `0xde` 는 302번 자리라 거기까지 가 보지도 못했다(2026-09-09 계측).
    //   `PatchSeqTargets` 도 같은 이유로 훑기를 쓴다.
    //   ⚠ 훑기는 우연히 같은 세 바이트를 만날 수 있다. 그래서 **오프코드 + 남의 번호**가
    //     나란히 있는 자리만 손대고, 몇 자리를 고쳤는지 세어 로그로 남긴다.
    for (size_t q = 0; q + 3 <= buf.size(); ++q) {
        if (!BareTargetOpcode(buf[q])) continue;
        const uint16_t id = static_cast<uint16_t>(buf[q + 1] | (buf[q + 2] << 8));
        if (id < from || id >= from + kSlotSpan) continue;
        const uint16_t nid = static_cast<uint16_t>(to + (id - from));
        buf[q + 1] = static_cast<uint8_t>(nid);
        buf[q + 2] = static_cast<uint8_t>(nid >> 8);
        ++st.patched;
    }
    st.stoppedAt = buf.size();
    return st;
}

static bool PatchPreludePlayer(const std::vector<uint8_t>& frag, uint16_t netCode,
                               int x, int y, std::vector<uint8_t>& out,
                               bool forceHudOn) {
    out.clear();
    out.reserve(frag.size() + 16);
    bool any = false;
    uint16_t origPlayer = 0;      // 서두에 박힌 남의 번호
    size_t pos = 0;
    while (pos < frag.size()) {
        const uint8_t op = frag[pos];
        const size_t msz = MessageSize(frag.data() + pos, frag.size() - pos,
                                       /*fromClient=*/false);
        if (msz > 0) {                       // 길이를 아는 메시지 — 그대로 옮긴다
            if (pos + msz > frag.size()) break;
            out.insert(out.end(), frag.begin() + pos, frag.begin() + pos + msz);
            pos += msz;
            continue;
        }
        if (op != static_cast<uint8_t>(Op::UPDATE_STREAM)) break;   // 모르는 것 — 접는다

        UpdateMsg um = DecodeUpdate(frag.data() + pos + 1, frag.size() - pos - 1, nullptr);
        if (!um.ok || um.consumed == 0) break;
        bool hit = false;
        for (auto& e : um.objects) {
            if (e.type != kPlayerType) continue;
            if (origPlayer == 0) origPlayer = e.netCode;   // 껍데기 치환에 쓸 남의 번호
            e.alias = 0xff;                  // 전체 번호로 다시 소개한다
            e.netCode = netCode;
            e.x = x;
            e.y = y;
            // ⚠ **덧붙이는 건드리지 않는다.** 녹음본의 사람 항목은 `[c0 00]` 인데
            //   정품 지금 덤프는 `[40 00]` 이라, 정품에 맞춰 `[40 00]` 으로 강제해 봤더니
            //   **클라가 접속을 끊었다**(2026-09-08 실측: 화면이 서버 목록으로 되돌아갔다).
            //   `c0` 의 bit 7 이 「한 바이트 더」 신호인 것은 맞지만, 그 다음 `00` 이
            //   그 한 바이트를 채우고 있어 **녹음본 안에서는 짝이 맞는다.**
            //   즉 뜯어 본 「별칭=0f·번호 0」 쓰레기 항목은 원인이 아니라 **내 디코더가
            //   갈래를 넓게 연 결과**일 수 있다 — 관측대기로 남긴다.
            hit = true;
        }
        if (hit) {
            // 머리도 이 사람이 보는 자리로. 안 맞추면 화면이 남의 자리에 선다.
            um.x = x;
            um.y = y;
        }
        uint8_t buf[1024];
        buf[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        // ⚠ 09-19 — 여기도 **잘릴 수 있다**(`EncodeUpdate` 가 부분 인코딩으로 바뀌었다).
        //   다만 입력이 **정품 녹음본**이라 480B 안쪽이고, 통이 1024 라 여유가 두 배다.
        //   이 함수는 Server 멤버가 아니라 계수기를 바로 못 붙인다 — 세야 할 근거가
        //   생기면 `droppedOut` 을 인자로 빼서 호출부(스냅샷 만드는 자리)에서 더할 것.
        const size_t n = EncodeUpdate(um, buf + 1, sizeof(buf) - 1);
        if (n == 0) break;                   // 다시 못 짜면 손대지 않는다
        out.insert(out.end(), buf, buf + 1 + n);
        pos += 1 + um.consumed;
        any = any || hit;
    }
    if (!any) { out.clear(); return false; }
    // ⚠ 끝까지 못 읽었으면 **남은 것은 원본 그대로** 붙인다. 통째로 포기하면
    //   갈아 끼우기가 조용히 안 먹는다(2026-09-08: 그래서 첫 시도가 헛돌았다).
    if (pos < frag.size()) out.insert(out.end(), frag.begin() + pos, frag.end());
    // ★★★ 껍데기 속 대상 번호도 함께 옮긴다 — 여기까지 해야 장비·체력이 이 사람에게 붙는다.
    PatchSeqTargets(out, origPlayer, netCode, forceHudOn);
    // ★★★ **껍데기 밖에 맨몸으로 실린 것도 옮긴다.** `PatchSeqTargets` 는 `0xcc` 속만 본다.
    //   2026-09-09 실측 — 같은 조각 안에서 `dd`(최대 체력)는 내 번호로 나가는데
    //   바로 다음 `de`(최대 마나)는 **남의 번호 715 그대로** 나가고 있었다:
    //     ★속 op=0xdd(껍데기 안) | dd c9 02 4b 00 4b 00   ← 갈아끼움
    //     ★   op=0xde(맨몸)      | cb 02 96 00 96 00      ← 남의 번호
    //   마나 막대가 안 뜨는 것과 자리가 같다.
    const BarePatchStat bst = PatchBareTargets(out, origPlayer, netCode);
    // ★ 접속마다 한 줄. 「0 자리 고침」이 「고칠 게 없었다」인지 「거기까지 못 갔다」인지
    //   멈춘 자리를 같이 봐야 갈린다. [[feedback_zero_needs_stall_count]]
    std::printf("서두 맨몸 대상 %d자리 고침 · 걷기 %zu/%zu 바이트 · 멈춘 오프코드 0x%02x\n",
                bst.patched, bst.stoppedAt, bst.total,
                bst.stoppedAt < out.size() ? out[bst.stoppedAt] : 0);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// 벽 충돌 — 정답지 이식 (2026-09-13 묶음 2-b)
//
//   ★ 정답지 벽은 **칸이 아니라 칸 안의 대각선 선분**이다. 칸 원점 (23x, 23y) 에서
//     왼쪽 선분 = (0,0)→(23,23) 방향(\) · 오른쪽 선분 = (23,0)→(0,23) 방향(/).
//     방향 0~10 이 둘 중 무엇을, 어느 구간(0~23 · 0~11.5 · 11.5~23)만큼 쓰는지 표가 정한다.
//   전에는 벽 칸을 통째 네모로 막았다(CanStand) — 화면의 벽 선과 멈추는 자리가 달랐다.
// ════════════════════════════════════════════════════════════════════════════
namespace {
// `GAME5.c:53` table_313272 — [왼쪽 켬, 최소, 최대, 오른쪽 켬, 최소, 최대]
struct WallSeg { bool l; float lmin, lmax; bool r; float rmin, rmax; };
constexpr WallSeg kWallSegs[11] = {
    { false, 0, 0,     true,  0, 23 },
    { true,  0, 23,    false, 0, 0 },
    { true,  0, 23,    true,  0, 23 },
    { true,  0, 11.5f, true,  0, 23 },
    { true,  0, 23,    true,  11.5f, 23 },
    { true,  11.5f, 23, true, 0, 23 },
    { true,  0, 23,    true,  0, 11.5f },
    { true,  0, 11.5f, true,  11.5f, 23 },
    { true,  11.5f, 23, true, 11.5f, 23 },
    { true,  11.5f, 23, true, 0, 11.5f },
    { true,  0, 11.5f, true,  0, 11.5f },
};
// `0x587000+292496` 방향별 이웃 확인 비트 — 정답지 데이터 덩어리(`blob_587000.dat`)에서 읽은 값
constexpr uint8_t kWallNeighborBits[11] = { 9, 6, 15, 11, 7, 13, 14, 3, 5, 12, 10 };
constexpr float kGrid = 23.0f;
constexpr float kInvGrid = 0.043478262f;      // `GAME5.c:6610`
constexpr float kDiag = 0.70709997f;          // `GAME5.c:6735`
constexpr float kWallPush = 100.0f;           // `dword_587000_292492` = 100.0 (데이터 덩어리)

// sub_57CD30 · sub_57CD70 — 선분 p1→p2 와 칸 대각선의 교점
inline void CrossLeft(float x1, float y1, float x2, float y2, float cx, float cy, float& ox, float& oy) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float v = ((cy - y1) * dx - (cx - x1) * dy) / (dy - dx);
    ox = cx + v; oy = cy + v;
}
inline void CrossRight(float x1, float y1, float x2, float y2, float cx, float cy, float& ox, float& oy) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float v = ((cy - y1) * dx - (cx - x1) * dy) / (-dy - dx);
    ox = cx - v; oy = cy + v;
}
// sub_550280 — 왼쪽 대각선 위 가장 가까운 점(구간 끝은 a4·a5 가 허락할 때만 붙든다)
inline bool NearestOnLeft(float ox, float oy, float lo, float hi, bool a4, bool a5,
                          float px, float py, float& cx, float& cy) {
    float t = ((px - ox) + (py - oy)) * kDiag * kDiag;
    if (t < lo) { if (!a4) return false; t = lo; }
    if (t > hi) { if (!a5) return false; t = hi; }
    cx = t + ox; cy = t + oy;
    return true;
}
// sub_5502F0 — 오른쪽 대각선
inline bool NearestOnRight(float ox, float oy, float lo, float hi, bool a4, bool a5,
                           float px, float py, float& cx, float& cy) {
    float t = 23.0f - ((py - oy) * kDiag - (px - (ox + 23.0f)) * kDiag) * kDiag;
    if (t < lo) { if (!a4) return false; t = lo; }
    if (t > hi) { if (!a5) return false; t = hi; }
    cx = t + ox; cy = 23.0f - t + oy;
    return true;
}
// sub_550380 — 가까운 점까지가 반지름보다 가까우면 밀어낸다: 벽 쪽 속도 성분을 없애고
//   (반지름 − 거리) × 100 ÷ 질량 을 가속(+96 Pos24 = Sub548600)에 더한다.
//   ⚠ 공(GameBall) 예외·충돌 기록(collSysAddCollision)은 사람에게 안 쓰여 뺐다.
// 09-21 계측용 — 재는 동안만 가리킨다(계측이 물리 식을 안 건드리게 밖에 둔다)
double* gWallNearestLen = nullptr;
unsigned long long* gWallPushCount = nullptr;
inline bool PushOut(double r, double mass, double px, double py, float cx, float cy,
                    double& vx, double& vy, double& ax, double& ay) {
    const float dx = static_cast<float>(px) - cx, dy = static_cast<float>(py) - cy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0.0f) len = 0.0099999998f;
    if (len >= static_cast<float>(r)) return false;
    if (gWallNearestLen && len < *gWallNearestLen) *gWallNearestLen = len;   // 09-21 계측
    if (gWallPushCount) ++*gWallPushCount;
    const float nx = dx / len, ny = dy / len;
    const double into = -ny * vy + -nx * vx;
    if (into > 0.0) { vx += into * nx; vy += into * ny; }
    const double push = (r - len) * kWallPush;
    ax += push * nx / mass;
    ay += push * ny / mass;
    return true;
}
}  // namespace

// sub_57B500(`server/wall.go:839`) — 벽이 있으면 방향, 없으면 −1.
// ★ 9-a — **반 넘게 열린 비밀벽은 −1**(막지 않는다). 정답지 `server/wall.go:848` 의
//   `wl.Flags4.Has(FlagSecret) && *(uint8)(wl.Data+22) > 11` 갈래다. 칸은 0~23 이니 **11 초과**가 절반이다.
// ★★ 09-22 묶음 9 — 문을 **제 칸의 벽으로** 만들어 붙인다. 정답지 `nox_xxx_doorAttachWall_410360`
//   (`GAME1.c:692`)은 맵을 읽을 때 `nox_xxx_wallCreateAt_410250(gx, gy)` 로 **없던 벽을 만들고**
//   `Flags4 |= 0x10` 을 켠 뒤 벽의 `Data` 에 문을 건다. 그래서 파일의 WallMap 에는 문 칸이 없다.
void Server::AttachDoorWalls() {
    // ⚠ 09-22 — **둘 다** 크기를 맞춰야 한다. `wallTiles_` 만 맞추고 `walls_` 를 뒀다가
    //   벽 없는 맵에서 `walls_[...]` 가 범위를 넘어 **죽었다**(시험 ⑱-5 가 SEGFAULT 로 잡았다)
    if (wallTiles_.empty()) wallTiles_.assign(256 * 256, WallTile{});
    if (walls_.size() != 256 * 256) walls_.assign(256 * 256, 0);
    doorCellDiff_ = 0;
    doorCellOldHadWall_ = 0; doorCellNewHadWall_ = 0;
    for (size_t i = 0; i < props_.size(); ++i) {
        if (props_[i].kind != PropKind::Door) continue;
        // ★★ 09-29 Z1-A — **칸은 문짝 가운데다**(`DoorWallCell` — (자리 + 표값/2) ÷ 23 · `GAME3_3.c:11520~11524`).
        //   ✖ 전에는 문 자리(경첩) ÷ 23 이었다. 58맵 부팅으로 견주니 대부분의 문이 칸이 달랐고(Estate 6/7 · G_Castle 40/48),
        //     정답지 칸은 **맵 벽과 한 번도 안 겹치고**(0 — 파일 WallMap 에 문 칸이 없다는 것과 맞다) 옛 칸은 자주 겹쳤다
        //     (G_Lava 19 · G_Castle 14 · Estate 2). 옛 칸이 맵 벽이면 ① 닫힌 문이 제 칸을 안 막고 ② 문이 열리면 **그 맵 벽이
        //     걸음 추적에서 사라졌다**(`WallShapeAt` 문 갈래 — 열렸으면 −1)
        int gx = 0, gy = 0;
        DoorWallCell(props_[i], gx, gy);
        {
            const int ox = props_[i].x / 23, oy = props_[i].y / 23;   // 옛 칸 — 진단으로만 센다
            if (ox != gx || oy != gy) {
                ++doorCellDiff_;
                // 판정용 — 두 칸에 **맵 벽이 원래 있었나**(문을 붙이기 전 · 앞 문이 만든 벽은 door 로 가린다)
                const WallTile* wo = (ox >= 0 && ox < 256 && oy >= 0 && oy < 256) ? &wallTiles_[oy * 256 + ox] : nullptr;
                const WallTile* wn = (gx >= 0 && gx < 256 && gy >= 0 && gy < 256) ? &wallTiles_[gy * 256 + gx] : nullptr;
                if (wo && wo->present && wo->door < 0) ++doorCellOldHadWall_;
                if (wn && wn->present && wn->door < 0) ++doorCellNewHadWall_;
            }
        }
        if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256) continue;
        WallTile& w = wallTiles_[gy * 256 + gx];
        w.door = static_cast<int16_t>(i);
        // 이미 벽이 있으면 방향은 그대로 두고 문만 건다(정답지도 `wallCreateAt` 이 있는 벽을 돌려준다)
        if (!w.present) {
            const int d = DoorWallDir(props_[i].angle);
            if (d < 0) continue;                         // 그 각도는 아예 안 막는다
            w.present = true;
            w.dir = static_cast<uint8_t>(d);
        }
        walls_[gy * 256 + gx] = 1;
    }
    AIPathBakeWalls();   // 길찾기 격자에도 같은 벽을 굽는다
}

// ★ 09-29 Z1-A — 문의 벽 칸(Server.h 선언 주석). 표값의 반은 C 정수 나눗셈(0 쪽 버림 — −23/2 = −11) ·
//   곱은 `(long long)` 버림(`GAME3_3.c:11520~11523`). 우리 문 자리는 정수(맵의 float 를 버린 값)지만 표값이 정수라 같은 칸이 나온다
void Server::DoorWallCell(const WorldProp& p, int& gx, int& gy) {
    const int a = p.angle & 31;
    const int hx = kDoorWallTable[a][0] / 2, hy = kDoorWallTable[a][1] / 2;
    gx = static_cast<int>(static_cast<long long>((static_cast<double>(hx) + static_cast<double>(p.x)) * 0.043478262));
    gy = static_cast<int>(static_cast<long long>((static_cast<double>(hy) + static_cast<double>(p.y)) * 0.043478262));
}

// ★ 09-29 Z1-A — 퀘스트에서 문이 풀릴 때(`sub_4E8390` `GAME3_3.c:3127~3130`): 갱신자료 +48 = 1 · 모두에게 `f0 0f <extent u16>`
//   (`sub_4D6A20(255, 문)` `GAME3_2.c:3313~3322` — MSG_GAUNTLET 15 · netSendPacket0 중요). +48 은 우리에게 읽는 곳이 없어 안 둔다
void Server::DoorUnlockQuest(size_t di, std::vector<OutPacket>& out) {
    if (di >= props_.size()) return;
    const uint16_t ext = props_[di].netCode;   // 번호 = 맵 extent(15비트) — 클라가 이 번호로 문을 찾는다
    const uint8_t msg[4] = { static_cast<uint8_t>(Op::GAUNTLET), 15, static_cast<uint8_t>(ext), static_cast<uint8_t>(ext >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
}

// ★ 09-29 Z1-A — `nox_xxx_collideDoor_4E8AC0(문, 닿은 것)`(`GAME3_3.c:3383~3516` · 열쇠 찾기 `DoorCheckKey` `server/object_door.go:57~92`).
//   ① 닫힘 각도일 때만(`:3407` +12 == +4) ② 마법 잠금(+508 · SPELL_LOCK)은 우리에게 없다 — 늘 비어 있다
//   ③ 잠김 0 → 끝 ④ 잠김 5(장치) → 「…LockedMechanism」 ⑤ 열쇠 없음 → 소리 + `f0 21` 「…LockedKey」(잠김 종류를 끝 바이트에)
//   ⑥ 열쇠 있음 → 잠김 0 · 짝 문도 0(`fnFindCloseDoors_4E8340`) · 퀘스트면 `f0 0f` · 소리 234(Unlock) · 열쇠 지움(`4d` — `sub_4ED0C0`)
//   ④⑤ 의 말은 1.5초에 한 번(정답지는 실시간 ms 한 칸을 모든 문·사람이 같이 쓴다 — 우리는 프레임으로 센다)
//   소리 — 부분류 GATE(`+12 & 4`)면 244 GateLocked · 아니면 240 WoodenDoorLocked · 자리는 문
//   ⚠ 안 옮긴 것 — 퀘스트에서 은 열쇠를 나눠 쓰기(`sub_4D72C0` 깃발 — 켜는 `sub_4D71F0` 이 없다) · 「KeyShared1」(그 깃발일 때만) · 몬스터가 닿는 문(우리 문은 가장 가까운 사람만 본다)
void Server::DoorCollide(size_t di, Session& s, std::vector<OutPacket>& out) {
    if (di >= props_.size()) return;
    WorldProp& d = props_[di];
    const int coarse = (32 * static_cast<int>(d.fine) / 256) & 31;
    if (coarse != static_cast<int>(d.angle)) { ++doorTouch_[static_cast<size_t>(DoorTouch::NotClosed)]; return; }
    if (d.lock == 0) { ++doorTouch_[static_cast<size_t>(DoorTouch::Unlocked)]; return; }
    // 1.5초 — `ticks − 지난 ms <= 0x5DC` 면 말 없이 끝(`:3413~3417`). 30fps 에서 45프레임 이하 = 조용
    auto quiet = [&]() {
        if (frame_ < doorLockMsgFrame_) { ++doorTouch_[static_cast<size_t>(DoorTouch::Quiet)]; return true; }
        doorLockMsgFrame_ = frame_ + kServerTickRate * 3 / 2 + 1;
        return false;
    };
    const uint16_t lockedSnd = d.gate ? 244 : 240;   // GateLocked · WoodenDoorLocked
    if (d.lock == 5) {
        if (quiet()) return;
        PlaySoundAt(lockedSnd, static_cast<float>(d.x), static_cast<float>(d.y));
        SendPriMsg(s, d.gate ? "objcoll.c:GateLockedMechanism" : "objcoll.c:DoorLockedMechanism", out);
        ++doorTouch_[static_cast<size_t>(DoorTouch::Mechanism)];
        return;
    }
    // 열쇠 찾기 — 가방 첫 물건부터(정답지 가방은 새 것이 머리 · 우리 벡터는 뒤가 새 것)
    size_t keyAt = s.inventory.size();
    if (d.lock >= 1 && d.lock <= 4 && keyWire_[d.lock] != 0) {
        for (size_t i = s.inventory.size(); i-- > 0;)
            if (s.inventory[i].type == keyWire_[d.lock]) { keyAt = i; break; }
    }
    if (keyAt == s.inventory.size()) {
        if (quiet()) return;
        PlaySoundAt(lockedSnd, static_cast<float>(d.x), static_cast<float>(d.y));
        // sub_4FADD0(`GAME4.c:2238~2257`) — 52바이트 `f0 21 <글>\0 … <[51] = 잠김 종류>` · netSendPacket0 중요 · 사람에게만
        const char* key = d.gate ? "objcoll.c:GateLockedKey" : "objcoll.c:DoorLockedKey";
        uint8_t msg[52] = {};
        msg[0] = static_cast<uint8_t>(Op::GAUNTLET);
        msg[1] = 33;
        std::memcpy(msg + 2, key, std::strlen(key) + 1);
        msg[51] = d.lock;
        EmitImportantRaw(s, msg, sizeof(msg), out);
        ++doorTouch_[static_cast<size_t>(DoorTouch::KeyMissing)];
        return;
    }
    // ── 풀림 ──
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    d.lock = 0;
    int gx = 0, gy = 0;
    DoorWallCell(d, gx, gy);
    int nx = 0, ny = 0;
    bool hasPartner = true;
    switch (d.angle) {                                // `:3477~3502` — 닫힘 각도별 이웃 칸(그 밖 각도는 정답지도 값을 안 채운다)
        case 0:  nx = gx - 1; ny = gy - 1; break;
        case 8:  nx = gx + 1; ny = gy - 1; break;
        case 16: nx = gx + 1; ny = gy + 1; break;
        case 24: nx = gx - 1; ny = gy + 1; break;
        default: hasPartner = false; break;
    }
    if (quest) DoorUnlockQuest(di, out);
    if (hasPartner) {
        // `getUnitsInRect_517C10` — 사각형 = 칸 모서리(23 × 칸) ±34 · 거르기는 충돌 상자 겹침(`server/map.go:305~306`)
        const double rx0 = 23.0 * gx - 34.0, rx1 = 23.0 * gx + 34.0, ry0 = 23.0 * gy - 34.0, ry1 = 23.0 * gy + 34.0;
        for (size_t j = 0; j < props_.size(); ++j) {
            WorldProp& o = props_[j];
            if (o.kind != PropKind::Door) continue;
            double r = 0.0;
            auto tb = typeBodies_.find(o.type);
            if (tb != typeBodies_.end()) r = static_cast<double>(tb->second.r);
            if (!(o.x - r < rx1 && o.x + r > rx0 && o.y - r < ry1 && o.y + r > ry0)) continue;
            int ox = 0, oy = 0;
            DoorWallCell(o, ox, oy);
            if (ox != nx || oy != ny) continue;
            o.lock = 0;
            if (quest) DoorUnlockQuest(j, out);
            ++doorTouch_[static_cast<size_t>(DoorTouch::PartnerUnlocked)];
        }
    }
    PlaySoundAt(234, static_cast<float>(d.x), static_cast<float>(d.y));   // Unlock
    // 열쇠 지움 — `delayedDeleteObject_4E5CC0` → `sub_4ED0C0` → 본인에게 `4d <번호2> <종류2>`(`GAME3_3.c:6335`)
    {
        const Session::Carried k = s.inventory[keyAt];
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_DROP), static_cast<uint8_t>(k.netCode), static_cast<uint8_t>(k.netCode >> 8),
                                 static_cast<uint8_t>(k.type), static_cast<uint8_t>(k.type >> 8) };
        EmitSeq(s, msg, sizeof(msg), out);
        s.inventory.erase(s.inventory.begin() + static_cast<long>(keyAt));
    }
    ++doorTouch_[static_cast<size_t>(DoorTouch::KeyUsed)];
}

// ★★ 09-22 묶음 9 — **문 갈래를 넣었다.** 정답지 `Sub_57B500(pos, flags)` 그대로:
//     · 문 칸인데 `flags & 0x10` 이 없으면 **−1** — 즉 **밀어내기(충돌)는 문을 아예 안 본다**
//       (`sub_54FFC0` 는 64 로만 부른다). 그래서 문에 밀려 튕기는 일이 없다
//     · 있으면(=걸음 추적) **열려 있으면 −1**(지나간다) · 닫혀 있으면 문 각도가 정하는 대각선
//   ⚠ 아직 **문마다의 각도를 안 읽는다** — 맵 짐에서 그 자리를 못 찾아 `kDoorClosed`(0) 로 모두 같다.
//     각도 0 은 표에서 (−23,−23) → 부호가 같으니 **방향 1**(왼쪽 대각 `\`)이다.
//     문 방향이 반대인 문이 있으면 막는 대각선이 틀린다 — 각도를 읽게 되면 그때 바로잡는다
// ⛔ 창(`FlagWindow`) 갈래는 아직 없다
int Server::WallShapeAt(int gx, int gy, uint8_t sflags) const {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256 || wallTiles_.empty()) return -1;
    const WallTile& w = wallTiles_[gy * 256 + gx];
    if (!w.present) return -1;
    if (w.door >= 0 && static_cast<size_t>(w.door) < props_.size()) {
        if (!(sflags & 0x10)) return -1;                 // 충돌은 문을 무시한다
        if (props_[static_cast<size_t>(w.door)].open) return -1;   // 열렸으면 지나간다
        return w.dir;
    }
    // ★ 09-29 Z1-A — 창문은 **0x40 을 안 준 추적**에서만 없는 벽이다(`server/wall.go:848` — 떠 있는 것의 걸음·가까운 칸 추적이 창을 지나간다)
    if (!(sflags & 0x40) && w.window) return -1;
    if (w.secret >= 0 && static_cast<size_t>(w.secret) < secretWalls_.size() &&
        secretWalls_[static_cast<size_t>(w.secret)].anim > 11) return -1;
    return w.dir;
}

// ★ 09-29 Z1-A — 맵의 부서지는 벽·창문 칸을 벽 모양에 얹는다(Server.h 선언 주석). 체력은 모든 벽에 정의 값
void Server::SetWallSpecials(const std::vector<std::pair<uint32_t, uint32_t>>& destructible,
                             const std::vector<std::pair<uint32_t, uint32_t>>& windows) {
    if (wallTiles_.empty()) return;
    for (auto& w : wallTiles_) {
        if (!w.present) continue;
        w.hp = (w.tile < wallDefExtra_.size()) ? wallDefExtra_[w.tile].health : 0;
    }
    uint16_t id = 0;
    for (const auto& g : destructible) {
        if (g.first >= 256 || g.second >= 256) continue;
        WallTile& w = wallTiles_[g.second * 256 + g.first];
        if (!w.present) continue;                            // 벽이 없는 칸은 번호도 안 센다(`:1008~1011`)
        w.breakable = true;
        w.breakId = id++;
    }
    for (const auto& g : windows) {
        if (g.first >= 256 || g.second >= 256) continue;
        WallTile& w = wallTiles_[g.second * 256 + g.first];
        if (w.present) w.window = true;
    }
}

// ★ 09-29 Z1-A — `Nox_xxx_damageToMap_534BC0`(`wall.go:307~353`) — 한 칸 벽에 피해.
//   · 벽 찾기는 `GetWallAtGrid`(`server/wall.go:237~251`) — 홀수 칸·문·부서진 벽은 없는 것
//   · 부서지는 벽이 아니면 맞는 소리만(`Sub_532FE0` = 표 `Sub_532930`(정의 Field36 × 때린 것 재질) · who 자리) → 0
//   · 퀘스트에서 사람이 친 것이면 한 방에(99999999) · 아니면 받은 피해
//   · 체력 − 피해 ≤ 0 → `WallPreDestroy` · 아니면 체력을 깎고 부서지는 재질 소리(`Sub_533010` = `Sub_532EC0`(Field36) · who 자리)
//   ⚠ 안 옮긴 것 — 깎일 때 튀는 벽돌 오브젝트(정의 BrickType42 · 힘 10~20) · 마법 벽(sysuse)
int Server::DamageToMap(int gx, int gy, int dmg, uint8_t dtype, const WallHitter& who, std::vector<OutPacket>& out) {
    (void)dtype;
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256 || wallTiles_.empty() || ((gx + gy) & 1)) { ++wallGate_[0]; return 0; }
    WallTile& w = wallTiles_[gy * 256 + gx];
    if (!w.present || w.door >= 0 || w.broken) { ++wallGate_[0]; return 0; }
    const uint16_t hitMat = (w.tile < wallDefExtra_.size()) ? wallDefExtra_[w.tile].hitMat : 0;
    if (!w.breakable) {
        if (who.present) {
            const uint16_t snd = HitSoundFor(hitMat, who.material);
            if (snd != 0) PlaySoundAt(snd, who.x, who.y, 0, 0, std::string(), Target{ who.src, -1 });
        }
        ++wallGate_[1];
        return 0;
    }
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    const int d = (quest && who.present && who.player) ? 99999999 : dmg;
    if (static_cast<int>(w.hp) - d <= 0) return WallPreDestroy(gx, gy, out);
    w.hp = static_cast<uint8_t>(static_cast<int>(w.hp) - d);
    if (who.present) {
        const uint16_t snd = BreakableHitSound(hitMat);
        if (snd != 0) PlaySoundAt(snd, who.x, who.y, 0, 0, std::string(), Target{ who.src, -1 });
    }
    ++wallGate_[2];
    return 0;
}

// ★ 09-29 Z1-A — `nox_xxx_wallPreDestroy_534DA0`(`wall.go:388~437`) — 부서지는 벽을 부순다.
//   체력 0 · 부서지는 소리(정의 Sound683 · 비면 WallDestroyed)를 벽 자리에 · 연기 `8a <x2> <y2>`(화면 안) ·
//   부서짐 깃발(그 뒤로는 벽이 없다 — 모양·충돌·길 찾기에서 빠진다) · 모두에게 `3a <번호2>`(`wallSendDestroyed_4DF0A0` — 맨 메시지 · 중요)
//   ⚠ 안 옮긴 것 — 벽돌 오브젝트 3~6개(퀘스트 아닐 때 · 힘 4~10)
int Server::WallPreDestroy(int gx, int gy, std::vector<OutPacket>& out) {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256 || wallTiles_.empty()) return 0;
    WallTile& w = wallTiles_[gy * 256 + gx];
    if (!w.present || !w.breakable || w.broken) return 0;
    w.hp = 0;
    const float px = static_cast<float>(gx) * 23.0f + 11.5f, py = static_cast<float>(gy) * 23.0f + 11.5f;
    uint16_t snd = (w.tile < wallDefExtra_.size()) ? wallDefExtra_[w.tile].breakSound : 0;
    if (snd == 0) snd = SoundByName("WallDestroyed");
    if (snd != 0) PlaySoundAt(snd, px, py);
    {
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t fx[5] = { static_cast<uint8_t>(Op::FX_SMOKE_BLAST), static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, fx, sizeof(fx), out);
    }
    w.broken = true;
    w.present = false;                                       // 모양·추적·충돌 모두 이 칸을 빈 칸으로 본다(`GetWallAtGrid2` 가 부서진 것을 뺀다)
    walls_[gy * 256 + gx] = 0;
    AIPathBakeWalls();                                       // 길 찾기 격자도 다시 굽는다
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_WALL), static_cast<uint8_t>(w.breakId), static_cast<uint8_t>(w.breakId >> 8) };
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        EmitImportantRaw(kv.second, msg, sizeof(msg), out);
        ++wallGate_[4];
    }
    ++wallGate_[3];
    return 1;
}

// ★ 09-29 Z1-A — 사람을 벽 피해의 who 로(유닛 — 자리 +56 · 방향 +124(Direction1) · 재질 = NewPlayer 재질)
Server::WallHitter Server::WallHitterOfAddr(const std::string& addr) const {
    WallHitter h;
    auto it = sessions_.find(addr);
    if (it == sessions_.end() || !it->second.inGame) return h;
    h.present = true; h.unit = true; h.player = true;
    h.src = addr;                                            // ★ 09-29 B6 — 맞는 소리의 출처(몬스터가 들을 거리)
    h.x = static_cast<float>(it->second.posX); h.y = static_cast<float>(it->second.posY);
    h.dir = it->second.facing;
    h.material = MaterialOfWire(newPlayerWire_);
    return h;
}

// ★ 09-29 Z1-A — `Nox_xxx_wall_4DF1E0` 의 벽 몫(`wall.go:354~360`) — 들어온 사람에게 이미 부서진 벽마다 `3a <번호2>`
void Server::SendBrokenWalls(Session& s, std::vector<OutPacket>& out) {
    for (const auto& w : wallTiles_) {
        if (!w.broken) continue;
        const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_WALL), static_cast<uint8_t>(w.breakId), static_cast<uint8_t>(w.breakId >> 8) };
        EmitImportantRaw(s, msg, sizeof(msg), out);
        ++wallGate_[4];
    }
}

// ★ 09-29 Z1-A — `Nox_xxx_mapDamageToWalls_534FC0`(`wall.go:210~305`) — 칸 네모 안 벽들에 둘레 피해.
//   · who 가 유닛이면 **그 자리에서** 거리를 재고 그 앞쪽(방향 내적 > 0.5)만 · 아니면 폭발 자리에서
//   · 반지름² 안이고 · 폭발 자리에서 벽 가운데까지 선 추적(깃발 1)이 트였거나 막은 칸이 바로 그 벽이면 — 32칸까지 모아
//   · 모은 칸마다 `DamageToMap`(같은 피해)
bool Server::MapDamageToWalls(int gx0, int gy0, int gx1, int gy1, float px, float py, float rad, int dmg, uint8_t dtype,
                              const WallHitter& who, std::vector<OutPacket>& out) {
    if (wallTiles_.empty()) return false;
    float cs = 0, sn = 0;
    const bool dirSet = who.present && who.unit;
    if (dirSet) { cs = DirVecX(who.dir); sn = DirVecY(who.dir); }
    const float r2 = rad * rad;
    std::vector<std::pair<int, int>> pts;
    for (int yi = gy0; yi <= gy1; ++yi) {
        for (int xi = gx0; xi <= gx1; ++xi) {
            if (xi < 0 || xi >= 256 || yi < 0 || yi >= 256 || ((xi + yi) & 1)) continue;
            const WallTile& w = wallTiles_[yi * 256 + xi];
            if (!w.present || w.door >= 0 || w.broken) continue;   // GetWallAtGrid
            const float bx = dirSet ? who.x : px, by = dirSet ? who.y : py;
            const float dx = static_cast<float>(xi) * 23.0f + 11.5f - bx;
            const float dy = static_cast<float>(yi) * 23.0f + 11.5f - by;
            const float d2 = dy * dy + dx * dx;
            if (!(d2 < r2)) continue;
            if (dirSet && d2 > 0.0f) {
                const float len = std::sqrt(d2) + 0.5f;
                if (!((dy / len) * sn + (dx / len) * cs > 0.5f)) continue;
            }
            const float cx = static_cast<float>(xi) * 23.0f + 11.5f, cy = static_cast<float>(yi) * 23.0f + 11.5f;
            TraceHit th;
            const bool clear = MapTraceRay(px, py, cx, cy, 0x1, &th);
            if ((clear || (th.gx == xi && th.gy == yi)) && pts.size() < 32) pts.emplace_back(xi, yi);
        }
    }
    for (const auto& p : pts) DamageToMap(p.first, p.second, dmg, dtype, who, out);
    return !pts.empty();
}

// mapTraceRayImpl(`server/wall.go:1055`) — 이 칸의 벽 선분을 p1→p2 가 가로지르나.
bool Server::MapTraceRayTile(int gx, int gy, float x1, float y1, float x2, float y2, uint8_t flags, float* hx, float* hy) const {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256) return false;
    if (flags & 0x8) {   // Flag4 — 가까운 칸(60 안)은 Flag3 로
        const float v7 = gx * kGrid + 11.5f - x1, v8 = gy * kGrid + 11.5f - y1;
        if (v8 * v8 + v7 * v7 < 3600.0f) flags |= 0x4;
    }
    // ★ 09-22 — 정답지 `mapTraceRayImpl`(`server/wall.go:1066~1070`)은 `Sub_57B500` 에
    //   `0x40 | (Flag1 이면 0x10)` 을 준다. **0x10 이 켜져야 문을 본다**(닫힌 문이 걸음을 막는다).
    //   우리 걸음 추적은 깃발 0x1(=Flag1)로 부르므로 여기서 0x10 이 붙는다
    // ✖ 09-23 3-b 정정 — **0x40 은 늘 붙지 않는다.** `v30 := byte(^flags & Flag3) << 4` 라 **Flag3(0x4)가 없을 때만** 0x40 이다.
    //   0x40 이 없으면 `Sub_57B500` 이 **창문을 벽으로 안 본다**(`:848`) — 떠 있는 것의 걸음 추적(0x1|0x4)·가까운 칸(Flag4→Flag3)이 창을 지나가는 길.
    //   ⚠ 우리 `WallShapeAt` 에는 창문이 없어(묶음 9) 지금은 0x40 이 있든 없든 결과가 같다 — 창을 넣을 때 틀리지 않게 정답지대로 둔다
    const int t = WallShapeAt(gx, gy, static_cast<uint8_t>(((flags & 0x4) ? 0 : 0x40) | ((flags & 0x1) ? 0x10 : 0)));
    if (t < 0 || t > 10) return false;
    // GetWallAtGrid(Flag1 없음)은 칸 짝홀((x+y)&1)이 홀이면 벽이 없다고 본다(`server/wall.go:238`)
    if (!(flags & 0x1) && ((gx + gy) & 1)) return false;
    const WallTile& w = wallTiles_[gy * 256 + gx];
    // ★ 9-a — **깃발 8(0x80)로 쏜 추적은 「통과」 비밀벽을 안 본다**(`server/wall.go:1080`) —
    //   마디 깃발 **2** 가 그 표다. 우리 추적 깃발은 `MapTraceFlag<N>` = 비트 `1 << (N−1)` 이다
    if ((flags & 0x80) && w.secret >= 0 && static_cast<size_t>(w.secret) < secretWalls_.size() &&
        (secretWalls_[static_cast<size_t>(w.secret)].flags & 0x2)) return false;
    const uint32_t f2 = (w.tile < wallDefFlags_.size()) ? wallDefFlags_[w.tile] : 0;
    if ((f2 & 0x2) || ((flags & 0x40) && !(f2 & 0x1))) return false;
    const float xl = std::min(x1, x2), xh = std::max(x1, x2);
    const float yl = std::min(y1, y2), yh = std::max(y1, y2);
    const float ox = gx * kGrid, oy = gy * kGrid;
    const WallSeg& s = kWallSegs[t];
    // 교차점 — 왼쪽·오른쪽 대각선(둘 다면 p1 에 가까운 것이 첫째 · `server/wall.go:1110~1133`)
    bool hitL = false, hitR = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (t != 0 && s.l) {
        CrossLeft(x1, y1, x2, y2, ox, oy, lx, ly);
        hitL = (ox + s.lmin <= lx && ox + s.lmax >= lx && lx >= xl && lx <= xh && ly >= yl && ly <= yh);
    }
    if (t != 1 && s.r) {
        CrossRight(x1, y1, x2, y2, ox + kGrid, oy, rx, ry);
        hitR = (ox + s.rmin <= rx && ox + s.rmax >= rx && rx >= xl && rx <= xh && ry >= yl && ry <= yh);
    }
    if (!hitL && !hitR) return false;
    if (hx && hy) {
        bool useRight = !hitL;
        if (hitL && hitR) {
            const float dxl = x1 - lx, dxr = x1 - rx, dyl = y1 - ly, dyr = y1 - ry;
            useRight = (dyr * dyr + dxr * dxr < dyl * dyl + dxl * dxl);
        }
        *hx = useRight ? rx : lx;
        *hy = useRight ? ry : ly;
    }
    return true;
}

// MapTraceRayAt(`server/wall.go:898`) — p1→p2 가 벽에 안 막히면 참. 걸음·겨눔 모두 이 길.
//   ⚠ 정답지 그대로의 걸음(곧은 가로·세로 먼저, 그 밖은 기울기 따라 칸 줄 훑기).
bool Server::MapTraceRay(float x1, float y1, float x2, float y2, uint8_t flags, TraceHit* hit) const {
    const int wx1 = static_cast<int>(x1 / kGrid), wy1 = static_cast<int>(y1 / kGrid);
    if (wx1 < 0 || wx1 >= 256 || wy1 < 0 || wy1 >= 256) return false;
    const int wx2 = static_cast<int>(x2 / kGrid), wy2 = static_cast<int>(y2 / kGrid);
    if (wx1 == wx2) {
        const int step = (y2 < y1) ? -1 : 1;
        if (wy1 == wy2 + step) return true;
        for (int yi = wy1; yi != wy2 + step; yi += step)
            if (MapTraceRayTile(wx1, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = wx1; hit->gy = yi; } return false; }
        return true;
    }
    if (wy1 == wy2) {
        const int step = (x2 < x1) ? -1 : 1;
        if (wx1 == wx2 + step) return true;
        for (int xi = wx1; xi != wx2 + step; xi += step)
            if (MapTraceRayTile(xi, wy1, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = wy1; } return false; }
        return true;
    }
    const float dx = x2 - x1, dy = y2 - y1;
    if (std::fabs(dy) >= std::fabs(dx)) {
        const float mulX = (x1 >= x2) ? -1.0f : 1.0f;
        const double stepY = (y1 >= y2) ? -1.0 : 1.0;
        const float slope = dy / dx;
        float xnext = 0;
        for (float x = x1; mulX * x <= mulX * x2; x = xnext + 0.1f * mulX) {
            const int xi = static_cast<int>(std::trunc(x / kGrid));
            float xmin = static_cast<float>(xi) * kGrid, xmax = static_cast<float>(xi + 1) * kGrid;
            if (mulX <= 0) std::swap(xmin, xmax);
            xnext = xmax;
            float ya = y1 + (xmin - x1) * slope;
            if ((stepY > 0 && ya < y1) || (stepY < 0 && ya > y1)) ya = y1;
            float yb = y1 + (xmax - x1) * slope;
            if ((stepY > 0 && yb > y2) || (stepY < 0 && yb < y2)) yb = y2;
            const int yi1 = static_cast<int>(std::trunc(static_cast<double>(ya / kGrid)) - stepY);
            const int yi2 = static_cast<int>(std::trunc(static_cast<double>(yb / kGrid)) + stepY);
            const int dd = (yi1 >= yi2) ? -1 : 1;
            for (int yi = yi1; yi != yi2 + dd; yi += dd)
                if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = yi; } return false; }
        }
        return true;
    }
    const float mulY = (y1 >= y2) ? -1.0f : 1.0f;
    const double stepX = (x1 >= x2) ? -1.0 : 1.0;
    const float slope = dx / dy;
    float ynext = 0;
    for (float y = y1; mulY * y <= mulY * y2; y = ynext + 0.1f * mulY) {
        const int yi = static_cast<int>(std::trunc(y / kGrid));
        float ymin = static_cast<float>(yi) * kGrid, ymax = static_cast<float>(yi + 1) * kGrid;
        if (mulY <= 0) std::swap(ymin, ymax);
        ynext = ymax;
        float xa = x1 + (ymin - y1) * slope;
        if ((stepX > 0 && xa < x1) || (stepX < 0 && xa > x1)) xa = x1;
        float xb = x1 + (ymax - y1) * slope;
        if ((stepX > 0 && xb > x2) || (stepX < 0 && xb < x2)) xb = x2;
        const int xi1 = static_cast<int>(std::trunc(static_cast<double>(xa / kGrid)) - stepX);
        const int xi2 = static_cast<int>(std::trunc(static_cast<double>(xb / kGrid)) + stepX);
        const int dd = (xi1 >= xi2) ? -1 : 1;
        for (int xi = xi1; xi != xi2 + dd; xi += dd)
            if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = yi; } return false; }
    }
    return true;
}

// sub_54FEF0 + sub_54FFC0 — 원 대 벽. 사각형(프레임 시작 자리 ± 반지름)이 덮는 칸마다
//   왼쪽·오른쪽 대각선에서 가장 가까운 점을 찾아 밀어낸다.
//   ⚠ a4·a5(선분 끝을 붙들까)는 **두 대각선 사이에서 이어진다** — 정답지가 두 번째 블록에서 되돌리지 않는다.
//   ⚠ 벽 소리·비밀 벽 열기(sub_548100)는 뺐다 — 묶음 9·10. 「닿은 벽」 기록(+296)은 ★ 2-b2 에서 넣었다(lastGx·lastGy).
bool Server::CircleVsWalls(double bx, double by, double r, double mass,
                           double nx, double ny, double& vx, double& vy, double& ax, double& ay,
                           int* lastGx, int* lastGy) const {
    if (!wallsOn_ || wallTiles_.empty()) return false;
    gWallNearestLen = &wallNearestLen_;          // ★ 09-21 계측 시작(아래 끝에서 되돌린다)
    gWallPushCount = &wallPushPerStepCur_;
    wallPushPerStepCur_ = 0;
    const int x0 = static_cast<int>(static_cast<float>(bx - r) * kInvGrid);
    const int y0 = static_cast<int>(static_cast<float>(by - r) * kInvGrid);
    const int x1 = static_cast<int>(static_cast<float>(bx + r) * kInvGrid);
    const int y1 = static_cast<int>(static_cast<float>(by + r) * kInvGrid);
    const float px = static_cast<float>(nx), py = static_cast<float>(ny);
    bool any = false;
    for (int gy = y0; gy <= y1; ++gy) {
        for (int gx = x0; gx <= x1; ++gx) {
            const int d = WallShapeAt(gx, gy);
            if (d < 0 || d > 10) continue;
            const WallSeg& s = kWallSegs[d];
            const uint8_t nb = kWallNeighborBits[d];
            const float ox = gx * kGrid, oy = gy * kGrid;
            bool a4 = false, a5 = false;
            float cx = 0, cy = 0;
            if (s.l) {
                if ((nb & 2) && WallShapeAt(gx - 1, gy - 1) == -1) a4 = true;
                if ((nb & 4) && WallShapeAt(gx + 1, gy + 1) == -1) a5 = true;
                bool got;
                if (d == 8 || d == 9) {
                    a4 = true;
                    got = NearestOnLeft(ox, oy, s.lmin, s.lmax, true, a5, px, py, cx, cy);
                } else {
                    if (d == 7 || d == 10) a5 = true;
                    got = NearestOnLeft(ox, oy, s.lmin, s.lmax, a4, a5, px, py, cx, cy);
                }
                const double wa0 = ax, wb0 = ay;
                if (got && lastGx) { *lastGx = gx; *lastGy = gy; }   // ★ 2-b2 — +296(밀기 판단 전에 적는다)
                if (got && PushOut(r, mass, nx, ny, cx, cy, vx, vy, ax, ay)) any = true;
                NoteWallPushWorst(ax - wa0, ay - wb0, gx, gy, d, nx, ny, cx, cy);
            }
            if (s.r) {
                if ((nb & 8) && WallShapeAt(gx - 1, gy + 1) == -1) a4 = true;
                if ((nb & 1) && WallShapeAt(gx + 1, gy - 1) == -1) a5 = true;
                bool got;
                if (d == 9 || d == 10) {
                    got = NearestOnRight(ox, oy, s.rmin, s.rmax, a4, true, px, py, cx, cy);
                } else {
                    if (d == 7 || d == 8) a4 = true;
                    got = NearestOnRight(ox, oy, s.rmin, s.rmax, a4, a5, px, py, cx, cy);
                }
                const double wa0 = ax, wb0 = ay;
                if (got && lastGx) { *lastGx = gx; *lastGy = gy; }   // ★ 2-b2 — +296(밀기 판단 전에 적는다)
                if (got && PushOut(r, mass, nx, ny, cx, cy, vx, vy, ax, ay)) any = true;
                NoteWallPushWorst(ax - wa0, ay - wb0, gx, gy, d, nx, ny, cx, cy);
            }
        }
    }
    // ★ 09-21 계측 — 이번 걸음에 **동시에** 민 조각 수의 최대(모서리에서 겹쳐 미는지 가른다)
    if (wallPushPerStepCur_ > wallPushPerStepMax_) wallPushPerStepMax_ = wallPushPerStepCur_;
    gWallNearestLen = nullptr;
    gWallPushCount = nullptr;
    return any;
}

// ════════════════════════════════════════════════════════════════════════════
// 오브젝트끼리 충돌 — 정답지 이식 (2026-09-13 묶음 2-c)
// ════════════════════════════════════════════════════════════════════════════
namespace {
constexpr uint32_t kClsMonster = 0x2, kClsPlayer = 0x4, kClsDoor = 0x80, kClsElevator = 0x4000, kClsShaft = 0x8000;
constexpr uint32_t kFlBelow = 0x1, kFlAllowOverlap = 0x8, kFlShort = 0x10, kFlDestroyed = 0x20, kFlNoCollide = 0x40,
                   kFlNoPushChars = 0x2000, kFlAirborne = 0x4000, kFlFalling = 0x20000;
constexpr float kBodyPush = 40.0f;   // `dword_587000_292488` = 40.0 (데이터 덩어리)
// 딱 겹쳤을 때 고르는 방향 넷 — `0x587000+292784` · `+289928` (둘 다 ±0.1 축 벡터)
constexpr float kNudge[4][2] = { { 0.1f, 0.0f }, { -0.1f, 0.0f }, { 0.0f, 0.1f }, { 0.0f, -0.1f } };

// 몸의 충돌 사각형(+232~+244, `GAME3_3.c:2508` objectUnkUpdateCoords) — 프레임 시작 자리 기준
inline void BodyRect(const Server::PhysBody& b, double x, double y,
                     double& x0, double& y0, double& x1, double& y1) {
    if (b.shape == 3) {
        x0 = x + b.boxLB; y0 = y + b.boxLB; x1 = x + b.boxRT; y1 = y + b.boxRT;
    } else {
        x0 = x - b.r; y0 = y - b.r; x1 = x + b.r; y1 = y + b.r;
    }
}

// sub_54A990 — 원(중심 p, 반지름 r)과 상자(자리 bx,by)의 겹침. 반환 ≥ 0 이면 닿음, n 은 상자→원 방향.
//   상자 꼭짓점 넷: A (LT,LB) · B (LB2,LT2) · C (RT,RB) · D (RB2,RT2) — LT2=LT, LB2=LB, RT2=RT, RB2=RB.
double BoxOverlap(float px, float py, float r, const Server::PhysBody& b, float bx, float by, float& nx, float& ny) {
    const float ax = b.boxLT + bx, ay = b.boxLB + by;
    const float cbx = b.boxLB + bx, cby = b.boxLT + by;
    const float ccx = b.boxRT + bx, ccy = b.boxRB + by;
    const float cdx = b.boxRB + bx, cdy = b.boxRT + by;
    const float v31 = (ay - ax + px - py) * kDiag;
    const float v29 = (cby - cbx + px - py) * kDiag;
    const float v17 = (ccy + ccx - px - py) * kDiag;
    const float v18 = (ay + ax - px - py) * kDiag;
    int k = 0;
    if (v31 <= 0.0f) { if (v29 < 0.0f) k = 2; } else { k = 1; }
    if (v17 >= 0.0f) { if (v18 > 0.0f) k |= 4; } else { k |= 8; }
    auto corner = [&](float qx, float qy) -> double {
        const float dx = px - qx, dy = py - qy;
        float len = std::sqrt(dy * dy + dx * dx);
        if (len == 0.0f) len = 0.1f;
        const double res = r - len;
        if (res >= 0.0) { nx = dx / len; ny = dy / len; }
        return res;
    };
    switch (k) {
        case 0: {
            float dx = px - bx, dy = py - by;
            if (dx == 0.0f && dy == 0.0f) { const int i = std::rand() % 4; dx = kNudge[i][0]; dy = kNudge[i][1]; }
            double len = std::sqrt(dy * dy + dx * dx);
            if (len == 0.0) len = 0.1;
            nx = static_cast<float>(dx / len); ny = static_cast<float>(dy / len);
            return len;
        }
        case 1: nx = kDiag;  ny = -kDiag; return r - v31;
        case 2: nx = -kDiag; ny = kDiag;  return r + v29;
        case 4: nx = -kDiag; ny = -kDiag; return r - v18;
        case 5: return corner(ax, ay);
        case 6: return corner(cbx, cby);
        case 8: nx = kDiag;  ny = kDiag;  return r + v17;
        case 9: return corner(ccx, ccy);
        case 10: return corner(cdx, cdy);
        default: return -1.0;
    }
}
}  // namespace

// sub_548220(거르기 sub_548360 + 모양별 나누기) → 원 대 원(550D00) · 원 대 상자(54AD50, a3=0).
//   ⚠ 뺀 것: 오각형·염력손·검은가루 예외 · Trigger 종류 예외 · NO_COLLIDE_OWNER 같은 편 · 문(5488B0/551250)·
//     승강기(551AE0/551C40)·상자 대 상자(550F80) · 충돌 기록은 4-b1 부터(콜백은 4FDF90 버프 몫만 — 사람·몬스터 충돌 함수는 7)
void Server::BodyVsBody(const PhysBody& a1, double nx, double ny, double bx, double by, double vx, double vy,
                        const PhysBody& a2, double ox, double oy, double obx, double oby, double ovx, double ovy,
                        double& ax, double& ay, const ContactRef* ra, const ContactRef* rb) const {
    // 거르기 (sub_548360 에서 옮긴 것만)
    if ((a1.flags & (kFlDestroyed | kFlNoCollide)) || (a2.flags & (kFlDestroyed | kFlNoCollide))) return;
    if ((a1.cls & kClsDoor) && (a2.flags & (kFlBelow | kFlAllowOverlap))) return;
    if ((a2.cls & kClsDoor) && (a1.flags & (kFlBelow | kFlAllowOverlap))) return;
    if ((a1.flags & kFlAllowOverlap) && (a2.flags & kFlAllowOverlap)) return;
    if (((a1.flags & (kFlBelow | kFlShort)) && (a2.flags & (kFlAirborne | kFlFalling))) ||
        ((a2.flags & (kFlBelow | kFlShort)) && (a1.flags & (kFlAirborne | kFlFalling)))) return;
    if ((a1.cls & kClsDoor) && (a2.cls & kClsDoor)) return;
    if ((a1.cls & kClsElevator) && (a2.cls & kClsElevator)) return;
    if ((a1.cls & kClsShaft) && (a2.cls & kClsShaft)) return;
    if ((a1.cls | a2.cls) & (kClsDoor | kClsElevator | kClsShaft)) return;   // ⚠ 문·승강기 충돌은 묶음 9
    if (a1.shape != 2) return;   // 사람·소환수는 원이다

    const bool losPair = (a1.cls & 0x2204) && (a2.cls & 0x2204);
    if (a2.shape == 2) {
        // ── 원 대 원 (`GAME5.c:7183`) ──
        float dx = static_cast<float>(ox - nx), dy = static_cast<float>(oy - ny);
        if (dx == 0.0f && dy == 0.0f) { const int i = std::rand() % 4; dx = kNudge[i][0]; dy = kNudge[i][1]; }
        float len = std::sqrt(dy * dy + dx * dx);
        if (len == 0.0f) len = 0.0099999998f;
        const float overlap = a1.r + a2.r - len;
        if (overlap <= 0.0f) return;
        if (losPair && !MapTraceRay(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(obx), static_cast<float>(oby), 0)) return;
        if (ra && rb) AddContact(*rb, *ra);                // collSysAddCollision_548630(a2, a1) — 4-b1
        bool push = !((a1.flags & kFlAllowOverlap) || (a2.flags & kFlAllowOverlap));
        if ((!(a1.cls & 6) || (a2.flags & kFlNoPushChars) != kFlNoPushChars) && push) {
            const double unx = dx / len, uny = dy / len;
            const double dvx = vx - ovx, dvy = vy - ovy;
            const double mmin = std::min(a1.mass, a2.mass);
            const double k = kBodyPush * overlap;
            double px = -(k * unx), py = -(k * uny);
            if (!(a1.cls & 6) || !(a2.cls & 6)) {
                const double t = -uny * dvx + dvy * unx;
                px -= t * -uny * mmin * 0.69999999;
                py -= t * mmin * unx * 0.69999999;
            }
            ax += px / a1.mass;
            ay += py / a1.mass;
        }
        return;
    }
    if (a2.shape == 3) {
        // ── 원 대 상자 (`GAME5.c:3465`, a3 = 0 — 원 쪽을 민다) ──
        float unx = 0, uny = 0;
        const double over = BoxOverlap(static_cast<float>(nx), static_cast<float>(ny), a1.r, a2,
                                       static_cast<float>(ox), static_cast<float>(oy), unx, uny);
        if (over < 0.0) return;
        if (losPair && !MapTraceRay(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(obx), static_cast<float>(oby), 0)) return;
        if (ra && rb) AddContact(*ra, *rb);                // collSysAddCollision_548630(a1, a2) — 4-b1
        const bool push = !((a1.flags & kFlAllowOverlap) || (a2.flags & kFlAllowOverlap));
        if ((!(a1.cls & 6) || !(a2.flags & kFlNoPushChars)) && push) {
            const double k = kBodyPush * over;
            const double dvx = vx - ovx, dvy = vy - ovy;
            const double tx = -uny, ty = unx;
            const double t = tx * dvx + dvy * unx;
            const double mmin = std::min(a1.mass, a2.mass);
            const double v16 = mmin * t;
            const double px = unx * k - v16 * tx * 0.69999999;
            const double py = uny * k - v16 * ty * 0.69999999;
            ax += px / a1.mass;
            ay += py / a1.mass;
        }
    }
}

// 진단 — (x,y) 에서 가장 가까운 벽 선분까지 거리(±3칸 안). 벽 없으면 −1.
double Server::DebugWallDistance(double x, double y, int* outGx, int* outGy, int* outDir) const {
    double best = -1.0;
    const int cx = static_cast<int>(x / kGrid), cy = static_cast<int>(y / kGrid);
    for (int gy = cy - 3; gy <= cy + 3; ++gy) {
        for (int gx = cx - 3; gx <= cx + 3; ++gx) {
            const int d = WallShapeAt(gx, gy);
            if (d < 0 || d > 10) continue;
            const WallSeg& s = kWallSegs[d];
            const float ox = gx * kGrid, oy = gy * kGrid;
            float px, py;
            auto take = [&](float qx, float qy) {
                const double dd = std::sqrt((x - qx) * (x - qx) + (y - qy) * (y - qy));
                if (best < 0 || dd < best) {
                    best = dd;
                    if (outGx) *outGx = gx;
                    if (outGy) *outGy = gy;
                    if (outDir) *outDir = d;
                }
            };
            if (s.l && NearestOnLeft(ox, oy, s.lmin, s.lmax, true, true, static_cast<float>(x), static_cast<float>(y), px, py)) take(px, py);
            if (s.r && NearestOnRight(ox, oy, s.rmin, s.rmax, true, true, static_cast<float>(x), static_cast<float>(y), px, py)) take(px, py);
        }
    }
    return best;
}

// 사람 자세 번호 — 정답지는 **상태**에서 뽑는다(`GAME4.c:1709` nox_common_mapPlrActionToStateId_4FA2B0:
//   0 걷기 → 4 · 5 달리기 → 6 · 13 가만 → 0 · 1 공격 → 애니 번호(맨손이면 +8 Field8)).
//   전에는 지난 틱과 자리를 견줘 움직였으면 걷기(4)로 봤다(`animMoving`) — 달리기(6)가 없었고
//   멈춘 뒤 미끄러지는 동안에도 걷는 자세였다.
//   ⚠ 무기 든 공격(0x47F0000 → 29/31 · 그 밖 sub_4FA280)·가만(13)의 대검 0x26·죽음 등은 그 조각(3-b·3-c)에서 채운다.
uint8_t Server::PlayerAnimByte(const Session& s) {
    const uint32_t equip = WeaponEquipOf(s);
    switch (s.moveState) {
        case kPlayerStateWalk: return kAnimWalk;
        case kPlayerStateRun: return kAnimRun;
        case kPlayerStateAttack: case 14: case 22: {
            // ★ 09-23 2-a1 — `:1723~1724` 함성이 켜져 있고 값이 서 있으면 **46 WARCRY** — 무기보다 먼저.
            //   (정적 함수라 `AbilityIsActiveVal` 대신 칸을 바로 읽는다)
            if (s.abilActive[2] && s.abilActiveVal[2]) return 46;
            if (s.abilActive[1]) return 45;                  // ★ 2-b1 — `:1725~1726` 광전사 켜짐이면 BERSERKER_CHARGE
            // `:1720~1737` — 마법 지팡이(0x47F0000)는 29/31 · 무기(0·1 아님)거나 Field8 이 0 이면 sub_4FA280 표 · 맨손은 Field8
            if (equip & 0x47F0000) return 31;   // ⚠ +96&2(충전 휘두르기) 면 29 — 마법 지팡이는 묶음 4
            if ((equip && equip != 1) || s.attackAnim == 0) {
                // sub_4FA280 — 비트 2 부터 가장 낮은 켜진 비트 → blob_587000 215824 표
                static const uint8_t kWeaponAnim[27] = { 0, 0, 33, 34, 0, 0, 44, 44, 27, 28, 37, 26, 35, 39, 39,
                                                         29, 31, 31, 31, 31, 31, 31, 31, 32, 32, 32, 31 };
                for (int b = 2; b < 27; ++b) if (equip & (1u << b)) return kWeaponAnim[b];
                return 0;
            }
            return s.attackAnim;
        }
        case 2: case 10: return 21;                                          // 시전 CAST_SPELL(`:1740~1743`, 4-a)
        case kPlayerStateIdle: return (equip & 0x400) ? 0x26 : kAnimIdle;   // 대검이면 GREAT_SWORD_IDLE(`:1756~1758`)
        case 12: return 3;                                                   // ★ 3-a 점프 JUMP(`:1753~1754`)
        case 30: return 52;                                                  // 움찔 RECOIL(`:1793`)
        case 3: case 4:                                                      // 죽음 DIE 1 · 죽어 있음 DEAD 2
            return (s.lastDamageType == 16) ? 51 : (s.moveState == 3 ? 1 : 2);   // 종류 16 이면 DUST(`GAME4_1.c:4457~4461`)
        case 15: case 16: case 17: return 40;                                // RAISE_SHIELD(`:1759~1763`)
        case 18: return 48;                                                  // GREAT_SWORD_BLOCK_LEFT
        case 19: return 49;                                                  // GREAT_SWORD_BLOCK_RIGHT
        case 20: return 47;                                                  // GREAT_SWORD_BLOCK_DOWN
        case 21: return 30;                                                  // STAFF_BLOCK
        default: return kAnimIdle;
    }
}
// 칸 번호(+236)를 싣는 상태 — `GAME4_1.c:4447~4453` 목록(1·2·10·14~22·24~30·32). 지금 들어오는 것은 1 뿐이다.
bool Server::PlayerAnimFrameSent(const Session& s) {
    switch (s.moveState) {
        case 1: case 2: case 10: case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        case 21: case 22: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 32:
            return true;
        default:
            return false;
    }
}

// 사람 걷기 한 틱 — 정답지 순서 그대로 (묶음 2-a, 2026-09-13).
//
//   ① 힘(ImplA · `object_update.go:133~192`)  상태가 걷기(0)·달리기(5)이고 움직일 수 있으면
//      달리기 = 이미 달리는 중이거나 커서가 몸에서 100 밖 → 속도 ×2 · 힘 = 바라보는 쪽 × 속도
//   ② 상태(ImplB · `:480~534`)  이번 틱 조작이 있으면 — 걷던 중이면 일단 가만(13)으로,
//      이동 코드가 있었으면 ①의 달리기 여부로 걷기/달리기. 조작이 하나도 없으면 상태를 그대로 둔다.
//      ⚠ 입력은 **다음 틱 힘**으로 나타난다(정답지도 ImplA 가 ImplB 보다 먼저 돈다).
//   ③ 적분(`server.go:146` · `GAME4_1.c:3218~3238`)  0.2 씩 5번 — 속도 += (힘 − 속도×0.5)×0.2, 자리 += 속도×0.2
//
//   ★ 벽 — 묶음 2-b(2026-09-13)에서 정답지로 바꿨다: 걸음마다 원 대 벽 밀어내기 + 선 추적(아래 ③).
//     반지름 kBodyRadius(thing.bin NewPlayer EXTENT 10) · 무게 `PlayerMassOf(s)`(힘에서 셈 · 10~30).
//   ⚠ 죽었거나(정답지 상태 3·4 는 ImplB 를 건너뜀) 관전(Field3680&3)이면 힘도 상태 변경도 없다 — 속도만 잦아든다.
//   ★ 2026-09-13 묶음 3-a — ① 에 상태 1(공격: PlayerAttack), ② 에 CCAction(PlayerInputAttack)이 더해졌다.
//     바라보는 쪽(CCOrientation)도 ② 에서 갈아 끼운다 — 전에는 받자마자 바꿔 ① 힘이 한 틱 일찍 새 방향을 썼다.
void Server::UpdatePlayerMovement(uint32_t frame, std::vector<OutPacket>& out) {
    moveFrame_ = frame;   // ★ 2-b2 — 부딪힘 처리(짝 루프)가 쓰는 이 틱 프레임(따라잡기 틱이면 frame_ 와 다르다)
    // ★ 2026-09-13 묶음 2-c — 두 단계로 나눴다. 정답지는 한 걸음(0.2)마다 **움직이는 모두의 충돌을 먼저** 돌리고
    //   (`GAME4_1.c:3195` sub_5481C0 루프) **그다음 모두를 적분**한다(`:3200` 루프). 사람 하나씩 5걸음을 다 돌리면
    //   뒤에 도는 사람이 앞사람의 **끝난 자리**와 부딪혀 순서가 결과를 바꾼다.
    // wallTouchedLastStep — 09-22 계측용. 「닿는 첫 걸음」만 고르려면 직전 걸음의 상태가 있어야 한다
    struct Mover { Session* s; double fx, fy, bx, by, ax, ay; bool collide; bool wallTouchedLastStep = false; };
    std::vector<Mover> movers;
    // 밖에서 x·y 를 바꿨으면(접속·부활·진단 옮기기) 실수 자리를 거기에 맞추고 멈춘다.
    //   ★ 3-a — **모두 먼저** 맞춘다. 앞사람의 공격 판정이 뒷사람의 옛 실수 자리를 보면 안 된다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        if (std::lround(s.posX) != s.x || std::lround(s.posY) != s.y) {
            s.posX = s.x; s.posY = s.y;
            s.velX = s.velY = 0.0;
        }
    }
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        // ⓪' 죽음(묶음 3-c) — `object_update.go:242~275` 상태 3: 30틱 넘으면 4(속도·힘 0) · 상태 4: 15틱 넘으면
        //   관전 아니고 CCAction(클릭) 이 왔거나 자동 부활 설정이면 부활. 그 틱의 조작 목록은 비운다(`cb.Reset()`).
        //   ⚠ 제거전 한도·관전 넘기기는 모드 규칙(8)
        bool respawnedNow = false;                         // 이 틱의 ImplA 는 상태 4 갈래였다 — 13 갈래를 또 돌리지 않는다
        if (s.respawnAtFrame != 0) {
            if (s.moveState == 3 && (frame - s.stateFrame) > kServerTickRate) {
                PlayerSetState(s, 4, frame);
                s.stateFrame = frame;
                s.velX = s.velY = 0.0;
                s.forceX = s.forceY = 0.0;
            } else if (s.moveState == 4 && (frame - s.stateFrame) > kServerTickRate / 2 && !s.observer &&
                       (s.actionThisTick || autoRespawn_)) {
                s.inputThisTick = s.moveInputThisTick = s.actionThisTick = s.hasFacingInput = s.jumpThisTick = false;
                PlayerRespawn(s, frame, out);
                respawnedNow = true;
            }
        }
        const bool dead = (s.respawnAtFrame != 0);
        const bool canMove = !dead && !s.observer && !respawnedNow;

        // ★ 09-24 사람 판(「맞으면 윽 소리가 나야 하지 않나」) — 맞은 사람의 신음(`object_update.go:51~62·80~107`).
        //   쉬는 틱(Field19_1)이 남았으면 하나 줄이고, 아니면 지난 체력 대비 준 비율(천분율)을 재고 지난 체력을 지금 값으로 · 줄었으면 7틱 쉰다.
        //   살아 있고 줄었으면 — 마지막 피해 종류(+524)가 독(5)이면 HurtPoison · 70 이하 Light · 450 이하 Medium · 넘으면 Heavy(여자면 Female 것) · 그 사람 자리
        //   ⚠ 정답지는 이 자리가 기력 회복(`:109~111`) 바로 앞이다 — 우리도 같은 자리
        {
            int v2 = 0;
            if (s.hurtCooldown != 0) {
                --s.hurtCooldown;
            } else {
                if (s.hurtPrevHp != 0) v2 = 1000 * (static_cast<int>(s.hurtPrevHp) - static_cast<int>(s.health)) / static_cast<int>(s.hurtPrevHp);
                s.hurtPrevHp = s.health;
                if (v2 > 0) s.hurtCooldown = 7;
            }
            if (!dead && v2 > 0) {
                const bool female = s.info97.size() > 67 && s.info97[67] != 0;
                HurtVoice k;
                if (s.lastDamageType == 5) k = HurtVoice::Poison;
                else if (v2 <= 450) k = (v2 <= 70) ? HurtVoice::Light : HurtVoice::Medium;
                else k = HurtVoice::Heavy;
                static const char* const kMale[] = { "HumanMaleHurtLight", "HumanMaleHurtMedium", "HumanMaleHurtHeavy", "HumanMaleHurtPoison" };
                static const char* const kFemale[] = { "HumanFemaleHurtLight", "HumanFemaleHurtMedium", "HumanFemaleHurtHeavy", "HumanFemaleHurtPoison" };
                PlaySoundAt(SoundByName((female ? kFemale : kMale)[static_cast<size_t>(k)]),
                            static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
                ++hurtVoice_[static_cast<size_t>(k)];
            }
        }

        // ⓪ 기력 회복(`object_update.go:108` — 죽지 않았으면 100 아래일 때 100/30 씩, ImplA 보다 먼저)
        if (!dead && s.stamina < 100) s.stamina = static_cast<uint8_t>(s.stamina + 100 / kServerTickRate);
        // ① 힘 · 공격(ImplA)
        double fx = 0.0, fy = 0.0;
        bool run = false;
        // ★ 09-23 1-a — 정답지 ImplA 의 `v68`: 이번 틱에 **걷기·달리기 갈래를 끝까지 탔나**(`object_update.go:147`).
        //   아래 살금살금 끄기(LABEL_247)가 이 값을 본다. 정답지는 짐이 넘치면(+3656) 그 앞에서 돌아가 이 값이 거짓이다
        //   — 우리는 짐 넘침의 이동 막기를 아직 안 옮겼지만(⚠ 아래) 이 값만은 정답지대로 거짓으로 둔다
        bool walkBranch = false;
        bool jumpedImplA = false;   // ★ 3-a — 이번 틱 ImplA 가 점프(12) 갈래였다 → ImplB 를 건너뛴다(`object_update.go:118` ok=false)
        if (canMove && (s.moveState == kPlayerStateWalk || s.moveState == kPlayerStateRun) && !s.buffs.Has(25) && !s.buffs.Has(5)) {
            // `object_update.go:132~191` — playerCanMove(얼림 25·붙잡힘 5) · 달리기면 SpeedCur × 2(그 틱만) ·
            //   혼란(3)이면 Direction2 를 흔든다(playerConfusedGetDirection_4F7A40) · 힘 = Direction2 방향 표 × SpeedCur
            //   ⚠ 무게 넘침(+3656 → inform 13 3)·발소리는 아이템 5·소리 10 · 작살 끌림 sub_4F9AB0 은 7
            walkBranch = !s.overweight;
            const double cdx = s.aimX - s.posX, cdy = s.aimY - s.posY;
            // ★ 09-23 1-a — **살금살금 중에는 안 달린다**(`:153` 의 `|| s.Abils.IsActive(u, AbilityTreadLightly)`).
            //   전에는 이 조건이 빠져 살금살금을 켜도 그대로 달렸다 — 사람 판 「F 발동 안 함」의 한 몫
            const bool wantRun = !(s.moveState != kPlayerStateRun && cdx * cdx + cdy * cdy <= kRunCursorDist * kRunCursorDist);
            run = wantRun && !AbilityIsActive(s, 4);
            if (wantRun && !run) ++treadBlockedRun_;
            const double speed = static_cast<double>(s.speedCur) * (run ? 2.0 : 1.0);
            // ★ 09-23 nine-defects 4-d4 — 발소리(`object_update.go:157~187·195~223`). 칸 = (넷코드 + 프레임) ÷ (칸당틱+1) % 칸수 — 한 틱 전 값과 비교
            //   ① 달리면 RUN(6) 칸이 **올라가며** 2 또는 8 이 될 때 RunOn* · 같은 달리기 안에서 IntClamp(0,100) ≤ 1 이면 힘쓰는 소리(남자 것)
            //   ② 걷기 상태(0)면 WALK(4) 칸이 **바뀌며** 3 또는 9 가 될 때 WalkOn* — 살금살금이면 달릴 때만(`!살금살금 || a1`)
            //   ⚠ 정답지는 이 자리를 힘 계산 **앞**(①)·**뒤**(②)에서 본다 — 같은 틱 안이라 결과는 같다
            {
                const int a = static_cast<int>(s.netCode) + static_cast<int>(frame);
                if (run) {
                    const int frames = playerAnimFrames_[6][0], hold = playerAnimFrames_[6][1];
                    if (frames > 0) {
                        const int cur = a / (hold + 1) % frames, prev = (a - 1) / (hold + 1) % frames;
                        if (cur > prev && (cur == 2 || cur == 8)) Footstep(s, true);
                    }
                    if (NextRandom(101) <= 1) {
                        PlaySoundAt(SoundByName("HumanMaleExertionLight"), static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
                        ++exertionLight_;
                    }
                }
                if (s.moveState == kPlayerStateWalk) {
                    const int frames = playerAnimFrames_[4][0], hold = playerAnimFrames_[4][1];
                    if (frames > 0) {
                        const int cur = a / (hold + 1) % frames, prev = (a - 1) / (hold + 1) % frames;
                        if (cur != prev && (cur == 3 || cur == 9)) {
                            if (!AbilityIsActive(s, 4) || run) Footstep(s, false);
                            else ++footTreadSilent_;
                        }
                    }
                }
            }
            if (s.buffs.Has(3)) s.facing = ConfusedDirection(s, frame);
            fx += DirVecX(s.facing) * speed;
            fy += DirVecY(s.facing) * speed;
        } else if (canMove && s.moveState == kPlayerStateAttack) {
            // `object_update.go:224~234` — 0 이 오면 활(WeaponEquip&4)이면 14 · Field34 = 프레임, 아니면 13 · Field8 지움
            if (!PlayerAttack(s, frame, out)) {
                if (WeaponEquipOf(s) & 0x4) {
                    PlayerSetState(s, 14, frame);
                    s.stateFrame = frame;
                } else {
                    PlayerSetState(s, kPlayerStateIdle, frame);
                    s.attackAnim = 0;
                }
            }
        } else if (canMove && s.moveState == 2) {
            // 시전(4-a) — `object_update.go:235~241` CAST_SPELL(21) 칸 = (프레임 − Field34) ÷ (붙듦+1) 을 u8 로 · 칸 수 넘으면 마지막 칸
            const int frames = playerAnimFrames_[21][0], hold = playerAnimFrames_[21][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (static_cast<int>(s.animFrame) >= frames) s.animFrame = static_cast<uint8_t>(frames - 1);
        } else if (canMove && s.moveState == 30) {
            // 움찔(3-e) — `object_update.go:454~461` RECOIL(52) 칸이 끝나면 13
            const int frames = playerAnimFrames_[52][0], hold = playerAnimFrames_[52][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) PlayerSetState(s, kPlayerStateIdle, frame);
        } else if (canMove && s.moveState == 12) {
            // ★ 09-23 nine-defects 3-a — 점프(12) `object_update.go:305~327`.
            //   칸 = (프레임 − Field34) ÷ (붙듦+1) · 애니 3(JUMP) · **작살에 안 걸렸으면** 힘 = Direction1 × 2·SpeedCur(`=` — 덮어씀)
            //   칸 수에 닿으면 상태 0 · 떠 있음 끔 · Field34 = 프레임. **이 갈래는 ImplB 를 안 부른다**(`return …, false`)
            //   ⚠ 칸 번호(+236)는 안 적는다 — 상태 12 는 칸을 싣는 목록 밖이다(`PlayerAnimFrameSent`)
            const int frames = playerAnimFrames_[3][0], hold = playerAnimFrames_[3][1];
            const int idx = static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            bool harpooned = false;
            for (const auto& kv2 : sessions_)
                if (kv2.second.inGame && kv2.second.harpoonTarget.pet < 0 && kv2.second.harpoonTarget.addr == s.addr) { harpooned = true; break; }
            if (!harpooned) {
                const double speed = 2.0 * static_cast<double>(s.speedCur);
                fx = DirVecX(s.facingPrev) * speed;
                fy = DirVecY(s.facingPrev) * speed;
                ++jumpForceTicks_;
            } else {
                ++jumpHeldTicks_;
            }
            if (idx >= frames) {
                PlayerSetState(s, kPlayerStateWalk, frame);
                s.airborne = false;
                s.stateFrame = frame;
                ++jumpLanded_;
            }
            jumpedImplA = true;
        } else if (canMove && s.moveState == kPlayerStateIdle) {
            // 가만(13) — `object_update.go:327~338` 방패를 들었으면 들어오는 투사체가 있거나 가만히 30/4 틱을 넘기면 올리기(15)
            //   ⚠ 웨이포인트(sub_4F9A80)는 묶음 6 · 채팅 모드는 없음
            // ★ 3-a — 머리에서 깃발 &= 0xFFFFBFFE(`:329`) — 떠 있음(0x4000)을 끈다(0x1 BELOW 는 사람에게 안 켜진다)
            if (s.airborne) { s.airborne = false; ++airborneOffIdle_; }
            if ((ArmorEquipOf(s) & 0x3000000) &&
                !(!IncomingMissile(s) && (frame - s.stateFrame) <= kServerTickRate / 4)) {
                PlayerSetState(s, 15, frame);
                s.animFrame = 0;
            }
        } else if (canMove && s.moveState == 14) {
            // ★ 09-29 — 활을 쏜 뒤(`object_update.go:340~346`) — 칸은 활 애니 33 의 마지막 · 상태 시작(Field34)에서 1초(틱 수)가 넘으면 13.
            //   전에는 이 갈래가 없어 활로 한 번 치면 상태 14 에 그대로 머물렀다
            s.animFrame = static_cast<uint8_t>(playerAnimFrames_[33][0] - 1);
            if (static_cast<int>(frame) - static_cast<int>(s.stateFrame) > static_cast<int>(kServerTickRate)) {
                PlayerSetState(s, kPlayerStateIdle, frame);
                ++bowPoseEnded_;
            }
        } else if (canMove && s.moveState == 15) {
            // 방패 올리는 중 — RAISE_SHIELD(40) 칸이 끝나면 16(`:346~353`)
            const int frames = playerAnimFrames_[40][0], hold = playerAnimFrames_[40][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) {
                PlayerSetState(s, 16, frame);
                s.animFrame = static_cast<uint8_t>(frames - 1);
            }
        } else if (canMove && s.moveState == 16) {
            s.animFrame = static_cast<uint8_t>(playerAnimFrames_[40][0] - 1);   // 올린 채(`:354~357`)
        } else if (canMove && s.moveState == 17) {
            // 방패 내리는 중 — 칸을 거꾸로, 0 이면 13(`:358~370`)
            const int frames = playerAnimFrames_[40][0], hold = playerAnimFrames_[40][1];
            int v11 = frames - static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (v11 >= frames) {
                s.animFrame = static_cast<uint8_t>(frames - 1);
            } else {
                if (v11 <= 0) {
                    v11 = 0;
                    PlayerSetState(s, kPlayerStateIdle, frame);
                }
                s.animFrame = static_cast<uint8_t>(v11);
            }
        } else if (canMove && (s.moveState >= 18 && s.moveState <= 21)) {
            // 대검 막기 18·19·20(GREAT_SWORD_BLOCK_LEFT/RIGHT/DOWN 48·49·47) · 지팡이 막기 21(STAFF_BLOCK 30) — 칸 끝나면 13(`:371~398`)
            static const int kBlockAnim[4] = { 48, 49, 47, 30 };
            const int anim = kBlockAnim[s.moveState - 18];
            const int frames = playerAnimFrames_[anim][0], hold = playerAnimFrames_[anim][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) PlayerSetState(s, kPlayerStateIdle, frame);
        }
        // ② 조작(ImplB) — 목록이 비었으면 아무것도 안 한다(`object_update.go:485`)
        //   ★ 3-a — ImplA 가 점프 갈래였으면 ImplB 를 **통째로** 안 부른다(`:118` · 아래 살금살금 끄기 LABEL_247 도 안 돈다)
        bool jumpedImplB = false;   // 이번 틱 ImplB 에서 뛰었다 → 목록 나머지(공격)와 LABEL_247 을 건너뛴다(`:570` return)
        if (canMove && s.inputThisTick && !jumpedImplA) {
            if (s.moveState == kPlayerStateWalk || s.moveState == kPlayerStateRun) {
                PlayerSetState(s, kPlayerStateIdle, frame);
                s.stateFrame = frame;
            }
            // CCOrientation(`:503~508`) — 얼림(25)이면 안 돔 · ★ 2-b1 광전사 켜져 있어도 안 돔(돌진 방향이 고정된다) · ⚠ 퀘스트 거래는 5
            if (s.hasFacingInput && !s.buffs.Has(25) && !AbilityIsActive(s, 1)) s.facing = s.facingInput;
            // 자연의 힘(31) 지속 중이면 방향 입력만 받는다(`:496` orientationOnly = Sub4FEE50 · 4-a3)
            const bool orientationOnly = DurActive(31, s.addr);
            // 이동 — playerCanMove 면 붙들고 있는 주문 취소(cancelAllSpells_4FEE90, `:509~510`) ·
            //   맨손 공격 중(상태 1)에는 안 먹는다(`:506~507`) · 방패 올린 채(16)면 먼저 내린다(17, `:513~514`)
            //   playerCanMove_4F9BC0 — 얼림(25)·붙잡힘(5) 버프면 못 움직임 ⚠ 퀘스트 거래 · 상태 1 + 특정 무기 갈래는 아이템(5)
            const bool playerCanMove = !s.buffs.Has(25) && !s.buffs.Has(5);
            if (s.moveInputThisTick && !orientationOnly && playerCanMove) {
                CancelChannelSpells(s.addr, out);
                // ★ 2-b1 — 광전사가 켜져 있으면 걷기 입력은 **상태를 안 바꾼다**(`:512` `!IsActive(Berserk) && …`)
                if (s.moveState != kPlayerStateAttack && !AbilityIsActive(s, 1)) {
                    if (s.moveState == 16) {
                        PlayerSetState(s, 17, frame);
                    } else {
                        PlayerSetState(s, run ? kPlayerStateRun : kPlayerStateWalk, frame);
                        s.stateFrame = frame;
                    }
                }
            }
            // ★ 09-23 nine-defects 3-a — CCJump(`:550~571`).
            //   움직일 수 없음·광전사 켜짐·함성 켜짐 값이면 무시 → 붙든 주문 취소 → 짐 넘침이면 힘쓰는 소리 + 알림(13,3) →
            //   아니면 기력 90 을 깎을 수 있을 때만(`playerSubStamina_4F7D30` — 모자라면 아무것도 안 함) 혼란이면 방향 흔들기 ·
            //   떠 있음 켬 · 상태 12 · Field34 = 프레임 · **return**(목록 나머지와 LABEL_247 을 안 돈다)
            //   ⚠ 정답지는 목록 **차례대로** 돈다. 우리는 코드별로 적어 두었다가 「보는 쪽 → 걷기 → 점프 → 공격」 차례로 돈다 —
            //     같은 틱에 공격이 점프보다 **먼저** 온 목록이면 정답지는 공격 뒤 점프, 우리는 점프만 한다(한 틱 안의 차례 · 짜임 한계)
            if (s.jumpThisTick && !orientationOnly) {
                ++jumpIn_;
                auto jg = [&](JumpGate g) { ++jumpGate_[static_cast<size_t>(g)]; };
                if (!playerCanMove) {
                    jg(JumpGate::CantMove);
                } else if (AbilityIsActive(s, 1)) {
                    jg(JumpGate::Berserk);
                } else if (AbilityIsActiveVal(s, 2)) {
                    jg(JumpGate::Warcry);
                } else {
                    CancelChannelSpells(s.addr, out);                // cancelAllSpells_4FEE90
                    if (s.overweight) {
                        // 성별 = 사람 정보 97바이트의 67번 칸 IsFemale(`GameBalance.h:43` 이 옮겨 적은 짜임 · 원본은 이 리포 밖) — 미실측
                        const bool female = s.info97.size() > 67 && s.info97[67] != 0;
                        PlaySoundAt(SoundByName(female ? "HumanFemaleExertionHeavy" : "HumanMaleExertionHeavy"),
                                    static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
                        InformSpell(s, 13, 3, out);                   // NetInformTextMsg(13, 3)
                        jg(JumpGate::Overweight);
                    } else if (s.stamina >= 90) {
                        s.stamina = static_cast<uint8_t>(s.stamina - 90);
                        ReportStamina(s, out);                        // netReportStamina_4D8800
                        if (s.buffs.Has(3)) s.facing = ConfusedDirection(s, frame);   // Direction2
                        s.airborne = true;                            // ObjFlags |= 0x4000
                        PlayerSetState(s, 12, frame, &out);
                        s.stateFrame = frame;
                        jg(JumpGate::Ok);
                        jumpedImplB = true;
                    } else {
                        jg(JumpGate::NoStamina);
                    }
                }
            }
            // CCAction(`:536~544`) — playerCanAttack(버프 25 없음 · 상태 23 아님) → 공 차기 아니면 playerInputAttack
            if (!jumpedImplB && s.actionThisTick && !orientationOnly && !s.buffs.Has(25) && s.moveState != 23) PlayerInputAttack(s, frame, out);
        }
        // ★ 09-23 nine-defects 1-a — **살금살금 끄기**(`object_update.go:616~621` LABEL_247).
        //   이번 틱 ImplA 가 걷기·달리기 갈래를 탔는데(`v68`) 조작을 거친 뒤 상태가 걷기·달리기가 아니면 끈다 —
        //   걷다가 멈추거나 · 치거나 · 주문을 걸면 풀린다. 조작 목록이 비어도 이 자리는 돈다(정답지 `goto LABEL_247`).
        //   전에는 이 길이 없어 지속 99999 가 사실상 안 꺼졌고, 다시 누르면 「이미 켜짐」 거절(231)만 났다
        if (!jumpedImplB && walkBranch && s.moveState != kPlayerStateWalk && s.moveState != kPlayerStateRun && AbilityIsActive(s, 4))
            AbilityDisable(s, 4, out);
        // 달리기(8) 버프 — 공격 중(1)이 아니면 상태 5(달림)로 묶는다(`object_update.go:120~122`)
        if (canMove && s.buffs.Has(8) && s.moveState != kPlayerStateAttack) PlayerSetState(s, kPlayerStateRun, frame);
        s.inputThisTick = s.moveInputThisTick = s.actionThisTick = s.hasFacingInput = s.jumpThisTick = false;

        // ⚠ 죽으면 정답지는 ALLOW_OVERLAP 을 켜고(상태 3→4, `object_update.go:247`) 관전은 NO_COLLIDE 를 켠다
        //   (`player.go:156`) — 둘 다 벽·오브젝트 충돌을 안 받는다(sub_5481C0 의 0x60 · 0x8 검사).
        movers.push_back({ &s, fx, fy, s.posX, s.posY, 0.0, 0.0, !dead && !s.observer });
        s.tickStartX = s.posX; s.tickStartY = s.posY;   // ★ 2-b2 — +56(적분 중엔 안 바뀐다)
        s.setPosThisTick = false;
    }
    // 남이 이번 틱에 준 힘(훅 20 — objectApplyForce_52DF80 → ForceVec)은 **모두의 갱신이 끝난 뒤** 적분에 들어간다
    //   (`server.go:135` updateUnits: BBB 갱신 → updateCollide 적분 → CCC 에서 ForceVec 지움). 순서가 결과를 안 바꾸게 여기서 더한다.
    for (auto& m : movers) {
        m.fx += m.s->forceX; m.fy += m.s->forceY;
        m.s->forceX = m.s->forceY = 0.0;
    }

    // ②' 날아가는 것들(4-a2) — 정답지 순서 BBB(갱신) → CallUpdate(투사체) → updateCollide(적분). 맞힘은 사람의 지난 틱 끝 자리 기준
    UpdateMissiles(frame, out);

    // ③ 적분 — 걸음(0.2)마다 [모두 충돌] → [모두 적분]. 사각형 칸 범위는 **프레임 시작 자리**(정답지 +232 는 +56 PosVec).
    PhysBody playerBody;
    playerBody.shape = 2; playerBody.r = static_cast<float>(kBodyRadius);
    // ⚠ 무게는 여기서 못 정한다 — **사람마다 다르다**(직업별 힘에서 나온다). 걸음마다 그 사람 것으로 채운다
    playerBody.cls = kClsPlayer; playerBody.flags = 0; playerBody.mass = 1.0f;   // NewPlayer FLAGS NULL
    contacts_.clear();                                     // nox_xxx_allocHitArray_5486D0
    for (int i = 0; i < kMoveSubSteps; ++i) {
        for (auto& m : movers) {
            Session& s = *m.s;
            m.ax = m.ay = 0.0;
            if (!m.collide) continue;
            // ★ 09-22 — **이 사람의 무게**로 채운다(직업별 힘에서 · 정답지 `GAME3_3.c:7701`)
            playerBody.mass = static_cast<float>(PlayerMassOf(s));
            playerBody.flags = PlayerObjFlags(s);             // ★ 3-a — 떠 있으면 0x4000(밑에 깔린·낮은 것과 안 부딪힌다 — `BodyVsBody` 거르기)
            const ContactRef me{ 1, s.addr, -1 };
            double qx0, qy0, qx1, qy1;
            BodyRect(playerBody, m.bx, m.by, qx0, qy0, qx1, qy1);
            // 오브젝트 먼저(getUnitsInRect → sub_548220) — 상대 사각형과 겹칠 때만(`server/map.go:302`)
            for (const auto& o : obstacles_) {
                double ox0, oy0, ox1, oy1;
                BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 3, std::string(), static_cast<int>(&o - obstacles_.data()) };
                // ★ 09-21 — 갈래마다 따로 센다(아래 둘도 같다). 벽만 세다가 「0 = 튕김 없음」으로
                //   읽을 뻔했다 — 사람 속도 54.8 을 만든 것은 벽이 아니라 이쪽이었다
                const double bx0 = m.ax, by0 = m.ay;
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           o, o.x, o.y, o.x, o.y, 0.0, 0.0, m.ax, m.ay, &me, &other);
                CountBodyPush(0, m.ax - bx0, m.ay - by0);
                NotePush(std::hypot(m.ax - bx0, m.ay - by0) * PlayerMassOf(s));
                {   // 가장 센 밀림의 자세한 값 — 반지름·거리를 같이 봐야 원인이 갈린다
                    const double ddx = m.ax - bx0, ddy = m.ay - by0;
                    if (ddx != 0.0 || ddy != 0.0) {
                        const double ex = o.x - s.posX, ey = o.y - s.posY;
                        const double mag = std::sqrt(ddx * ddx + ddy * ddy);
                        NoteBodyPushWorst(0, mag, kBodyRadius, o.r,
                                          std::sqrt(ex * ex + ey * ey), s.posX, s.posY, o.x, o.y);
                        // 40 = 오브젝트 밀기 상수 → 가속 ÷ 40 이 겹친 깊이
                        NoteFirstKick(0, mag, 40.0 / PlayerMassOf(s), s.velX, s.velY, s.posX, s.posY, -1, -1, -1, frame);
                    }
                }
            }
            for (const auto& m2 : movers) {
                if (m2.s == m.s || !m2.collide) continue;
                double ox0, oy0, ox1, oy1;
                BodyRect(playerBody, m2.bx, m2.by, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 1, m2.s->addr, -1 };
                const double bx0 = m.ax, by0 = m.ay;
                // ★ 09-22 — **상대의 무게**도 그 사람 것이어야 한다(옆으로 미끄러지는 항이 min(둘) 을 쓴다)
                PhysBody otherBody = playerBody;
                otherBody.mass = static_cast<float>(PlayerMassOf(*m2.s));
                otherBody.flags = PlayerObjFlags(*m2.s);      // ★ 3-a
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           otherBody, m2.s->posX, m2.s->posY, m2.bx, m2.by, m2.s->velX, m2.s->velY, m.ax, m.ay, &me, &other);
                CountBodyPush(1, m.ax - bx0, m.ay - by0);
                NotePush(std::hypot(m.ax - bx0, m.ay - by0) * PlayerMassOf(s));
                NoteFirstKick(1, std::hypot(m.ax - bx0, m.ay - by0), 40.0 / PlayerMassOf(s), s.velX, s.velY,
                              s.posX, s.posY, -1, -1, -1, frame);
            }
            for (const auto& k : pets_) {
                auto tb = typeBodies_.find(k.type);
                if (tb == typeBodies_.end()) continue;   // 몸을 모르는 종류는 안 부딪힌다(지어내지 않는다)
                if (!tb->second.hasCollide || (tb->second.shape != 2 && tb->second.shape != 3)) continue;   // COLLIDE 없음 · 몸 없음
                double ox0, oy0, ox1, oy1;
                BodyRect(tb->second, k.x, k.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 2, std::string(), static_cast<int>(&k - pets_.data()) };
                const double bx0 = m.ax, by0 = m.ay;
                // ★ 10-01 시체 — 몸 깃발에 죽음 덧칠(31 의 0x18 ALLOW_OVERLAP → 안 밀림 · 원본 거르기 `sub_548360` 은 DEAD 를 안 봄 `GAME5.c:1925~1930`)
                PhysBody petBody = tb->second;
                petBody.flags = PetObjFlags(k);
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           petBody, k.x, k.y, k.x, k.y, 0.0, 0.0, m.ax, m.ay, &me, &other);
                CountBodyPush(2, m.ax - bx0, m.ay - by0);
                NotePush(std::hypot(m.ax - bx0, m.ay - by0) * PlayerMassOf(s));
                NoteFirstKick(2, std::hypot(m.ax - bx0, m.ay - by0), 40.0 / PlayerMassOf(s), s.velX, s.velY,
                              s.posX, s.posY, -1, -1, -1, frame);
            }
            // 벽 — 오브젝트 다음(sub_5481C0 순서)
            {
                const double ax0 = m.ax, ay0 = m.ay;
                int lgx = -1, lgy = -1;
                const bool wallPushed = CircleVsWalls(m.bx, m.by, kBodyRadius, PlayerMassOf(s), s.posX, s.posY, s.velX, s.velY,
                                                      m.ax, m.ay, &lgx, &lgy);
                // ★ 2-b2 — +296(사람만 · `GAME5.c:6689`) · 민 벽은 짝 (나, 0) 으로 적는다(`GAME5.c:6817` 류 — 사람마다 틱에 한 번)
                if (lgx >= 0) { s.lastWallGx = static_cast<int16_t>(lgx); s.lastWallGy = static_cast<int16_t>(lgy); }
                if (wallPushed) {
                    const size_t n0 = contacts_.size();
                    AddContact(me, ContactRef{ 0, std::string(), -1 });
                    if (contacts_.size() != n0) ++wallContacts_;
                }
                // ★ 09-21 — 벽이 민 걸음과 그 세기를 센다(위 `wallPushSteps()` 주석)
                const double dax = m.ax - ax0, day = m.ay - ay0;
                if (dax != 0.0 || day != 0.0) {
                    ++wallPushSteps_;
                    const double mag = std::sqrt(dax * dax + day * day);
                    if (mag > wallPushMax_) wallPushMax_ = mag;
                    NotePush(mag * PlayerMassOf(s));
                    // ★ 09-22 — **닿는 첫 걸음**만 골라 깊이를 본다(직전 걸음엔 안 밀렸던 경우).
                    //   규칙대로면 0.03 근처여야 한다 — 크면 벽을 늦게 발견한 것이다
                    if (!m.wallTouchedLastStep) NoteWallFirstTouch(mag * PlayerMassOf(s) / kWallPush);
                    m.wallTouchedLastStep = true;
                    // 100 = 벽 밀기 상수 → 가속 ÷ 100 이 파고든 깊이. 칸은 방금 적힌 최악벽에서 가져온다
                    NoteFirstKick(3, mag, 100.0 / PlayerMassOf(s), s.velX, s.velY, s.posX, s.posY,
                                  wallPushWorst_.gx, wallPushWorst_.gy, wallPushWorst_.dir, frame);
                } else {
                    m.wallTouchedLastStep = false;
                }
            }
        }
        for (auto& m : movers) {
            Session& s = *m.s;
            // updateObjectsVelocity_5118A0(`GAME4_1.c:3205~3213`) — 붙잡힘(5)·얼림(25)·매혹(28)이면 ForceVec(+88: 걸음·밀림)은 빼고 부딪힘 힘(+96)만
            const bool pinned = s.buffs.Has(5) || s.buffs.Has(25) || s.buffs.Has(28);
            const double pfx = pinned ? 0.0 : m.fx, pfy = pinned ? 0.0 : m.fy;
            s.velX += ((m.ax + pfx) - s.velX * kPlayerDamping) * kMoveSubStep;
            s.velY += ((m.ay + pfy) - s.velY * kPlayerDamping) * kMoveSubStep;
            {   // ★ 09-21 — 사람의 **틱당 속도** 최대(걷기 6 · 달리기 12 가 정상. 튕기면 훌쩍 넘는다)
                const double sp = std::sqrt(s.velX * s.velX + s.velY * s.velY);
                if (sp > playerSpeedMax_) playerSpeedMax_ = sp;
                ++playerSteps_;
                if (WallAt(s.posX, s.posY)) ++playerInWallSteps_;   // 벽 안에 들어가 있나
            }
            double nx = s.posX + s.velX * kMoveSubStep;
            double ny = s.posY + s.velY * kMoveSubStep;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > kMaxCoord) nx = kMaxCoord;
            if (ny > kMaxCoord) ny = kMaxCoord;
            // ★ 09-21 — **추적이 걸음을 막은 횟수**를 센다. 벽 칸 안에서 5.5% 를 보내는데
            //   막은 적이 없다면 「벽을 뚫고 들어갔다 사출되는 것」이고, 잘 막고 있다면
            //   「벽에 바짝 붙어 선 것」이다 — 고칠 자리가 전혀 다르다
            // ★ 3-b — 정답지 걸음 추적 깃발 = `(오브젝트 깃발 >> 12) & 4 | 1`(`GAME4_1.c:3226`) — 떠 있으면 5(창문을 지나간다 · 위 MapTraceRayTile 주석)
            if (MapTraceRay(static_cast<float>(s.posX), static_cast<float>(s.posY),
                            static_cast<float>(nx), static_cast<float>(ny),
                            static_cast<uint8_t>(0x1 | ((PlayerObjFlags(s) >> 12) & 0x4)))) {
                s.posX = nx; s.posY = ny;
            } else {
                ++traceBlocked_;
            }
            // ★ 09-26 M3 — 바닥 칸 피해(`GAME4_1.c:3240~3245`) — 떠 있지 않고(+16 & 0x4000 없음) 체력이 있으면(+556)
            //   새 자리(+64)의 바닥 번호(`tileNFromPoint_411160`)가 **6**(thing.bin FLOR 순번 6 = `Lava` · 실측 `TestFloorDefs`)이면 짝 (나, 6)을 적는다.
            //   짝은 같은 틱에 한 번만(`collSysAddCollision_548630` 겹 거름 `GAME5.c:1983~2011`) — 걸음 다섯 중 한 번이라도 밟으면 틱에 한 번.
            //   피해는 아래 닿음 처리에서(`:2062~2065`). 소환수는 이 적분을 안 탄다(AI 가 자리를 옮긴다) — 소환수 몫은 틱의 소환수 고리 뒤(09-27 S2)
            if (!(PlayerObjFlags(s) & 0x4000) && !s.observer && s.respawnAtFrame == 0 &&
                floorGrid_.TileAt(static_cast<float>(s.posX), static_cast<float>(s.posY), floorEdges_).tile == 6) {
                AddContact(ContactRef{ 1, s.addr, -1 }, ContactRef{ 6, std::string(), -1 });
            }
        }
    }
    // nox_xxx_collide_548740 — 부딪힌 짝마다 a.collide(b)·4FDF90(a, b) → b.collide(a)·4FDF90(b, a)
    //   ⚠ 충돌 함수(+696 — PlayerCollide 광전사 · MonsterCollide 등)는 7 · 여기서는 버프 몫 4FDF90 만(4-b1)
    {
        const auto pairs = contacts_;
        const size_t petCount = pets_.size();
        for (const auto& pr : pairs) {
            // 되갚기로 소환수가 죽으면 자리 번호가 밀린다 — 그 뒤 소환수 짝은 건너뛴다(정답지는 지우기가 틱 끝이라 안 밀림)
            if (pets_.size() != petCount && (pr.first.kind == 2 || pr.second.kind == 2)) continue;
            // ★ 09-23 1-d — 정답지 순서 그대로: a.collide(b) → 4FDF90(a,b) → b.collide(a) → 4FDF90(b,a).
            //   닿음 피해나 버프 몫(감전 되갚기)으로 소환수가 죽으면 자리 번호가 밀리므로 그 뒤 이 짝은 그만둔다(위 줄과 같은 까닭).
            //   ⚠ 둘째 검사는 전에 없던 것이다 — 전에는 첫 버프 몫이 소환수를 죽여도 밀린 번호로 둘째 버프 몫을 불렀다
            auto shifted = [&] { return pets_.size() != petCount && (pr.first.kind == 2 || pr.second.kind == 2); };
            // ★ 2-b2 — 벽 짝(둘째 0)은 a.collide(0) 하나뿐(`GAME5.c:2055~2066` — 버프 몫은 둘째가 있을 때만 · 되부르기 없음)
            //   ⚠ 정답지 목록은 **머리에 끼운다**(`:2020~2024` — 나중 것이 먼저 돈다). 우리는 넣는 순서부터 정답지와 달라
            //     돌리는 순서만 뒤집어도 같아지지 않는다 — 맞추지 않았다
            // ★ 09-26 M3 — 짝 (나, 6) = 바닥 칸 6(용암). 정답지는 둘째가 1~6 이면 a.collide 도 버프 몫도 안 부르고(`:2055` `v1 > 6 || !v1`)
            //   **피해 함수만** (a, 0, 0, 2, 12) — 때린 이·때린 것 없음 · 피해 2 · 종류 12(`:2062~2065`)
            if (pr.second.kind == 6) {
                if (pr.first.kind == 1) {
                    ++lavaHurts_;
                    HurtTarget(Target{ pr.first.addr, -1 }, 2, out, std::string(), 12, nullptr, HitterNone());
                }
                continue;
            }
            if (pr.second.kind == 0) { ContactCollide(pr.first, pr.second, out); continue; }
            ContactCollide(pr.first, pr.second, out);
            if (shifted()) continue;
            CollideBuffs(pr.first, pr.second, out);
            ContactCollide(pr.second, pr.first, out);
            if (shifted()) continue;
            CollideBuffs(pr.second, pr.first, out);
        }
    }
    for (auto& m : movers) {
        m.s->x = static_cast<int>(std::lround(m.s->posX));
        m.s->y = static_cast<int>(std::lround(m.s->posY));
        m.s->facingPrev = m.s->facing;   // CCC: Direction1 = Direction2(`server.go:167`)
        m.s->prevX = m.bx; m.s->prevY = m.by;   // CCC: PrevPos(+72) = 이번 틱 시작 자리(movers 는 게임 안 모두 — 관전·죽음 포함)
        // ★ 2-b2 — 부딪힘 처리 중 SetPos 로 옮겼으면 PosVec 이 그 자리이므로 PrevPos 도 그 자리다(`server.go:163`)
        if (m.s->setPosThisTick) { m.s->prevX = m.s->posX; m.s->prevY = m.s->posY; m.s->setPosThisTick = false; }
    }
    // CCC: SpeedCur = (SpeedBonus + SpeedBase) × (1 − 0.2 × +541) · 느림(4)이면 × 0.5(`server.go:171~174`) — 빠르게(9) × 1.25 는 updateUnitBuffs 끝
    //   ⚠ SpeedBonus(아이템 무게·마법부여 5) · +541 느려짐 겹수(독·5) 는 0
    for (auto& m : movers) {
        Session& s = *m.s;
        s.speedCur = static_cast<float>((0.0 + kPlayerWalkPerFrame) * (1.0 - 0.2 * 0));
        if (s.buffs.Has(4)) s.speedCur *= 0.5f;
    }
    // CCC: updateUnitBuffs_4FF620(`server.go:189`) — 버프 남은 틱 −1 · 0 이면 끔(4-a4)
    std::vector<std::string> buffUnits;
    for (auto& m : movers) buffUnits.push_back(m.s->addr);
    for (const auto& a : buffUnits) UpdateUnitBuffs(Target{ a, -1 }, out);
    for (size_t i = pets_.size(); i-- > 0;) UpdateUnitBuffs(Target{ std::string(), static_cast<int>(i) }, out);   // 몬스터도 CCC 목록에(죽으면 지워지니 뒤에서부터)
    // 묶음 7-d — `serverAbilities::Update` · 쉬는 시간 −1 · 지속이 다한 것 끄기
    for (auto& kv : sessions_) if (kv.second.inGame) AbilityUpdate(kv.second, out);
    // 묶음 7-g — 작살 줄(`updateHarpoon_54F380` + `UpdatePlayer`)
    for (auto& kv : sessions_) if (kv.second.inGame) HarpoonUpdate(kv.second, out);
}

// ════════════════════════════════════════════════════════════════════════
// 공격 (묶음 3-a, 2026-09-13) — 정답지 이식
//
//   흐름  클릭(CCAction) → ImplB playerInputAttack(`GAME4.c:1588`) → 상태 1
//         → 틱마다 ImplA playerAttack(`GAME4_3.c:3855`) — 애니 칸이 끝나는 틱에 playerTraceAttack(`:3624`)
//         → 0 을 돌려주면 상태 13(`object_update.go:224~234`)
//   판정  sub_518040 사각형 후보(`GAME4_1.c:4091` · 거리 조건 sub_5180B0) → sub_5386A0 앞쪽 가장 가까운 것(몬스터 우선)
//         → sub_538510 방향 표 · 깃발 · 선 추적(벽 0x5) → 피해 함수(+716) · 훅이면 힘 20
//   ⚠ 빠진 것(다른 조각)  무기 갈래 전부(3-b) · 피해 함수 정답지(3-e: 지금은 HurtTarget) · 벽 부수기 mapDamageToWalls(9)
//     · 소환수 밀기(몬스터 물리 6) · 버프(4) · 공 차기 checkWinkFlags(깃발공 모드 없음) · (빗나감 소리 879~884 는 ★ 4-a 에서 넣음)
//     · 커서 아래 물건(아이템 몸 표 없음)
// ════════════════════════════════════════════════════════════════════════
namespace {


// `server/math.go:49` PointOnTheLine — p 를 선분에 내린 발, 선분 상자 안이면 참
bool PointOnTheLine(float x1, float y1, float x2, float y2, float px, float py, float& ox, float& oy) {
    const double dpx = x2 - x1, dpy = y2 - y1;
    const double d2 = dpy * dpy + dpx * dpx;
    const double dd = static_cast<double>(py - y1) * dpy + static_cast<double>(px - x1) * dpx;
    ox = static_cast<float>(dpx * dd / d2 + x1);
    oy = static_cast<float>(dpy * dd / d2 + y1);
    return std::min(x1, x2) <= ox && ox <= std::max(x1, x2) && std::min(y1, y2) <= oy && oy <= std::max(y1, y2);
}
// `server/map.go:606` LineTraceXxx — 두 선분(각자 사각형 두 점)이 걸치나
bool LineTraceXxx(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1) {
    const float r1x0 = std::min(ax0, ax1), r1x1 = std::max(ax0, ax1), r1y0 = std::min(ay0, ay1), r1y1 = std::max(ay0, ay1);
    const float r2x0 = std::min(bx0, bx1), r2x1 = std::max(bx0, bx1), r2y0 = std::min(by0, by1), r2y1 = std::max(by0, by1);
    if (r1x1 < r2x0 || r1x0 > r2x1 || r1y1 < r2y0 || r1y0 > r2y1) return false;
    if (r1y0 == r1y1 && r2y0 == r2y1) return true;
    const float a1w = r1x1 - r1x0, a1h = r1y1 - r1y0, a2w = r2x1 - r2x0, a2h = r2y1 - r2y0;
    const float dx = r2x0 - r1x0, dy = r2y0 - r1y0;
    const float dd1 = dy * a1w - dx * a1h;
    const float dd2 = a2w * a1h - a2h * a1w;
    const float dd3 = dy * a2w - dx * a2h;
    if (dd1 == 0.0f || dd2 == 0.0f || (dd1 < 0.0f && dd2 > 0.0f)) return false;
    if ((dd1 > 0.0f && dd2 < 0.0f) || std::fabs(dd2) < std::fabs(dd1) ||
        (dd3 < 0.0f && dd2 > 0.0f) || (dd3 > 0.0f && dd2 < 0.0f) || std::fabs(dd2) < std::fabs(dd3)) return false;
    return true;
}
}  // namespace

// `GAME4_3.c:2586` monsterTestBlockShield_533E70 + sub_533EB0 — 100 안의 투사체 중 **앞쪽에서 나를 향해** 오는 것이 있나.
//   남(같은 편·자기 것 아님 — sub_54E6F0) · 방향 표 &1(지난 자리) · 속도가 바라보는 쪽과 반대 · 옆으로 20 안 ·
//   나를 겨누는 각(내적 > 0.7) · 시야선. ⚠ 투사체 SUBCLASS·충돌 함수 검사(sub_54E730 나머지)는 투사체 묶음(4)
//   ★ 4-a2 — getMissilesInCircle 은 투사체 목록 전부를 돈다 → 날리는 Magic(missiles_)도 넣는다 · 바라보는 쪽은 방향 표(194136)
bool Server::IncomingMissile(const Session& s) const {
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float dirX = DirVecX(s.facingPrev);
    const float dirY = DirVecY(s.facingPrev);
    auto test = [&](float mx, float my, float prevX, float prevY, double vx, double vy, const std::string& owner, const Buffs* mb) {
        if ((mx - px) * (mx - px) + (my - py) * (my - py) > 100.0f * 100.0f) return false;   // getMissilesInCircle 100
        if (owner == s.addr) return false;                                                   // 같은 사슬
        auto ow = sessions_.find(owner);
        if (ow != sessions_.end() && ow->second.team != 0 && ow->second.team == s.team) return false;   // 같은 팀
        if (!(TestTwoPointsAndDirection(px, py, s.facingPrev, prevX, prevY) & 1)) return false;   // +72
        const double v5 = vx * dirX, v6 = vy * dirY;
        if (!(v5 + v6 < 0.0)) return false;
        const float v10 = mx - px, v11 = my - py;
        double v8 = static_cast<double>(dirX) * v11 + -static_cast<double>(dirY) * v10;
        if (v8 < 0.0) v8 = -v8;
        if (!(v8 < 20.0)) return false;
        const double v9 = std::sqrt(vx * vx + vy * vy);
        const double v14 = std::sqrt(static_cast<double>(v11) * v11 + static_cast<double>(v10) * v10);
        if (v9 == 0.0 || v14 == 0.0) return false;
        if (!(vy / v9 * -(v11 / v14) + vx / v9 * -(v10 / v14) > 0.69999999)) return false;
        if (!CanSeeBuffs(&s.buffs, mb, vx, vy, 0) || !MapTraceVision(px, py, mx, my)) return false;   // CanInteract(나, 투사체, 0)
        return v14 < 100.0;                                                                  // 가장 가까운 것 < 100
    };
    for (const auto& m : missiles_) {
        if (test(m.posX, m.posY, m.prevX, m.prevY, m.velX, m.velY, m.owner, &m.buffs)) return true;
    }
    return false;
}

// `server/map.go:522` MapTraceVision — 그늘 깃발(0x10000) 장애물이 선을 가리나, 그다음 벽(깃발 9).
//   ⚠ 문(클래스 0x80)은 묶음 9. 사람·소환수 몸에는 그늘 깃발이 없다(NewPlayer FLAGS NULL).
//   skipObstacle = 판정 대상 자신(자기를 가린다고 보지 않는다 — `it == obj2`).
bool Server::MapTraceVision(float x1, float y1, float x2, float y2) const {
    return MapTraceVisionSkip(x1, y1, x2, y2, -1);
}
bool Server::MapTraceVisionSkip(float x1, float y1, float x2, float y2, int skipObstacle) const {
    const float qx0 = std::min(x1, x2), qx1 = std::max(x1, x2), qy0 = std::min(y1, y2), qy1 = std::max(y1, y2);
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        if (static_cast<int>(i) == skipObstacle) continue;
        const PhysBody& o = obstacles_[i];
        if (!(o.flags & 0x10000)) continue;
        double ox0, oy0, ox1, oy1;
        BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
        if (ox1 < qx0 || ox0 > qx1 || oy1 < qy0 || oy0 > qy1) continue;   // EachObjInRect
        if (o.shape == 2) {
            float fx, fy;
            if (PointOnTheLine(x1, y1, x2, y2, o.x, o.y, fx, fy)) {
                const double dx = fx - o.x, dy = fy - o.y;
                if (dy * dy + dx * dx <= static_cast<double>(o.r * o.r)) return false;
            }
        } else if (o.shape == 3) {
            // 네 변 — LT·LB / LB2·LT2 / RT·RB / RB2·RT2 (LT2=LT … 는 CalcBox 가 같게 둔다)
            const float px = o.x, py = o.y;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxLB, py + o.boxLT)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxLB, py + o.boxLT)) return false;
        }
    }
    return MapTraceRay(x1, y1, x2, y2, 0x9);
}

// `object.go:1394` playerSetState 중 지금 들어오는 갈래 — 죽음·공중·도발·광전사·함성은 그 조각에서.
bool Server::PlayerSetState(Session& s, uint8_t st, uint32_t frame, std::vector<OutPacket>* out) {
    bool res = true;
    if (s.respawnAtFrame != 0 && st != 3 && st != 4) return false;   // DEAD 면 3·4 만(`:1398`)
    // ★ 3-a — 협동(2048)이 아니면 **떠 있는 동안 움찔(30)을 안 받는다**(`:1401~1407`)
    if (!(gameSettings_.gameFlags & 0x800u) && s.airborne && st == 30) { ++recoilBlockedAirborne_; return false; }
    // ★ 09-23 2-a1 — 공격(1) 중 **함성 애니가 도는 동안**(켜짐 값)에는 죽음(3·4) 말고는 다른 상태로 못 바꾼다(`:1417~1424`).
    //   (1 → 1 은 정답지도 LABEL_26 으로 곧장 간다 — 아래 갈래와 같다)
    if (s.moveState == kPlayerStateAttack && st != kPlayerStateAttack && AbilityIsActiveVal(s, 2) && st != 4 && st != 3)
        return false;
    if (st != kPlayerStateAttack) {
        s.attackAnim = 0;                                   // Field8 = 0(`:1426`)
    } else {
        // LABEL_26(`:1446~1464`) — 쉬는 시간이 끝났으면 애니를 새로 뽑는다. 아니면 상태를 안 바꾼다.
        if (s.attackReadyFrame <= frame) {
            s.attackReadyFrame = 0;
            if (WeaponEquipOf(s) != 0) {
                s.attackAnim = 0;                           // 무기 — Field8 = 0(자세는 무기 비트에서 · PlayerAnimByte)
            } else {
                // 맨손 — 23·24 반반, 워리어면 IntClamp(0,100) > 75 로 25
                s.attackAnim = static_cast<uint8_t>(kAnimPunchLeft + NextRandom(2));
                if (s.charClass == CharClass::Warrior && NextRandom(101) > 75) s.attackAnim = kAnimPunchHook;
                // 투명 해제 · 무적 해제 · 타원 방패 취소 — 버프 중 지금 있는 것은 부활 무적뿐
                BuffOff(Target{ s.addr, -1 }, 0);
                BuffOff(Target{ s.addr, -1 }, 23);
                if (out) DurCancelFor(67, s.addr, *out);
            }
        } else {
            res = false;
            st = s.moveState;
        }
    }
    if (s.moveState != st) {                                // LABEL_42
        s.moveState = st;
        s.stateFrame = frame;
        s.animFrame = 0;
    }
    return res;
}

// `GAME4.c:1211` mapFindPlayerStart_4F7AB0 — PlayerStart 중 **적에게서 가장 가까운 거리가 가장 먼** 곳.
//   적(나 말고 · IsEnemyTo)이 하나도 없거나 고른 곳이 없으면 무작위(randomInt(0, 수−1)). PlayerStart 가 없으면 (2000,2000).
//   ⚠ PlayerStart 의 ENABLED 깃발·팀(sub_4F7CE0)은 맵 xfer 를 아직 안 읽어 모두 켜짐·팀 없음으로 본다(맵 묶음 9).
bool Server::FindPlayerStart(const Session& s, float& x, float& y) {
    if (playerStarts_.empty()) { x = 2000.0f; y = 2000.0f; return false; }
    bool noEnemy = true;                                   // v8
    int best = -1;                                         // j
    float bestD = 0.0f;                                    // v17
    for (size_t i = 0; i < playerStarts_.size(); ++i) {
        float nearest = 10000000.0f;                        // v15
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (&o == &s || !o.inGame || !IsEnemyTo(s, o)) continue;
            const double dx = playerStarts_[i].first - o.posX, dy = playerStarts_[i].second - o.posY;
            const double d2 = dy * dy + dx * dx;
            if (d2 < nearest) nearest = static_cast<float>(d2);
            noEnemy = false;
        }
        if (nearest > bestD) { best = static_cast<int>(i); bestD = nearest; }
    }
    if (noEnemy || best < 0) best = static_cast<int>(NextRandom(static_cast<uint32_t>(playerStarts_.size())));
    x = playerStarts_[best].first;
    y = playerStarts_[best].second;
    return true;
}

void Server::PlayersCenter(int& x, int& y) const {
    long long sx = 0, sy = 0;
    int n = 0;
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        sx += kv.second.x; sy += kv.second.y; ++n;
    }
    if (n == 0) { x = spawnX_; y = spawnY_; return; }
    x = static_cast<int>(sx / n);
    y = static_cast<int>(sy / n);
}

// `object.go:722~731` unitMove 의 플레이어 몫 — 온라인이면 `ea <프레임4>` 를 그 사람에게(오래된 정적 그림을 지우라는 뜻,
//   클라 sub_45A670) · Sub4DE4D0: 정적 오브젝트의 「보이는 중」 비트를 꺼서 다시 소개되게.
void Server::ForgetDrawables(Session& s, std::vector<OutPacket>& out) {
    const uint32_t f = s.tickFrame + 1;                    // 이 틱 끝에 실리는 시각(27) 과 같은 값
    const uint8_t msg[5] = { static_cast<uint8_t>(Op::FORGET_DRAWABLES), static_cast<uint8_t>(f),
                             static_cast<uint8_t>(f >> 8), static_cast<uint8_t>(f >> 16), static_cast<uint8_t>(f >> 24) };
    EmitSeq(s, msg, sizeof(msg), out);
    s.staticIntroAt.clear();
}

// `GAME4.c:1420` playerRespawn_4F7EF0 + `GAME3_3.c:7901` playerMakeDefItems(a1, 1, 0) — 퀘스트 아닌 갈래.
//   ⚠ 옮기지 않은 것 — 부활 소리 148(10) · 부활 이펙트 respawnPlayerImpl_53FBC0(4) · 능력·주문 끄기(7·4) · 퀘스트 목숨·되살림 자리(8)
//   (09-28 B4b — 기본 장비 다시 만들기는 `RespawnGear` 로 옮김)
void Server::PlayerRespawn(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    s.health = s.maxHealth;                                // unitHPsetOnMax
    for (auto& kv : sessions_) if (kv.second.inGame) s.hpReportedTo[kv.first] = s.health;   // +12+2×i 서른둘 = 지금 체력(`GAME3_3.c:7937`) — 그 뒤 피해만 `42`
    PlayerManaRefresh(s);                                  // playerManaRefresh_4EECF0 — +6 = 이전 · +4 = 최대 → 다음 틱 `45`
    s.dmgRemainder = 0.0f;                                 // +84
    s.respawnAtFrame = 0;                                  // 깃발 &= 0xFFEB3FE7 — DEAD 끔
    s.airborne = false;                                    // ★ 3-a — 같은 마스크가 0x4000(AIRBORNE)도 끈다
    PlayerSetState(s, kPlayerStateIdle, frame);
    ClearBuffs(Target{ s.addr, -1 });                      // unitClearBuffs_4FF580(아래에서 23 을 다시 켠다)
    PlayerCancelSpells(s.addr, out);                       // playerCancelSpells_4FEAE0(`GAME3_3.c:8119`, 4-a3)
    s.lastHit = Session::LastHit{};                        // +520 = 0(`GAME3_3.c:7958`, 3-d)
    MarkDirty(s.netCode, PendingReport::What::UnitHealth);
    // 나에게 — 최대·지금 체력(`dd`) · 마나는 워리어가 아니면(`de`)
    {
        const uint8_t dd[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.health),
                                static_cast<uint8_t>(s.health >> 8), static_cast<uint8_t>(s.maxHealth),
                                static_cast<uint8_t>(s.maxHealth >> 8) };
        EmitSeq(s, dd, sizeof(dd), out);
        if (s.charClass != CharClass::Warrior) {
            const uint8_t de[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_MANA), static_cast<uint8_t>(s.netCode),
                                    static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.mana),
                                    static_cast<uint8_t>(s.mana >> 8), static_cast<uint8_t>(s.maxMana),
                                    static_cast<uint8_t>(s.maxMana >> 8) };
            EmitImportantRawSupersede(s, de, sizeof(de), out);   // netReportTotalMana_4D88C0 → sub_4E5450(Packet0)
        }
    }
    // 모두에게 — `e9 <번호2> <프레임4> <무기 깃발> 01`(netSendPlayerRespawn_4EFC30 · 끝 1 = 클라가 기본 장비 겉모습을 다시 세움)
    //   무기 깃발 = getRespawnWeaponFlags_4EF580 — 서버에 쓸 수 있는 기본 장비 종류 비트. 모두 허용 = 0xff(녹음 값과 같음)
    //   ⚠ 프레임은 받는 사람마다 그 사람의 틱 번호로 싣는다(우리 서버는 접속마다 시계가 따로다 — Session::tickFrame)
    // ★ 09-28 사람 판 — 「서로 맨몸인데 상대 화면엔 로브」. 끝 1 을 받은 클라는 그 사람을 **직업 기본 차림**으로 다시 그린다
    //   (`cdecode.c:2875~2879` → `cliPlayerRespawn_417680` `GAME1.c:2590~2696` — 위저드면 로브 0x4000). 정답지 서버는 같은 때
    //   기본 장비를 실제로 다시 준다(B4b — 우리는 미결) → 우리는 가방이 그대로인데 남 화면만 기본 차림이 된다.
    //   ⇒ **B4b 전까지 끝 바이트를 0** 으로 — 다시 그리지 말라. B4b(떨어뜨리기·기본 장비 새로)를 넣으면 **녹음 값 01 로 되돌릴 것**
    // ★ 09-28 B4b 넣음 — 녹음 값 01 로 되돌림(정품 녹음 18:56:07.9 `e9 61 00 57 0e 00 00 ff 01`). 기본 장비는 아래 `RespawnGear` 가 실제로 준다
    constexpr uint8_t kRespawnRedrawGear = 0x01;
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const uint32_t f = o.tickFrame + 1;               // 이 틱 끝에 실리는 시각(27) 과 같은 값 — 대기열이 시각 뒤에 나간다
        const uint8_t e9[9] = { static_cast<uint8_t>(Op::PLAYER_RESPAWN), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(f), static_cast<uint8_t>(f >> 8),
                                static_cast<uint8_t>(f >> 16), static_cast<uint8_t>(f >> 24), 0xff, kRespawnRedrawGear };
        EmitSeq(o, e9, sizeof(e9), out);
    }
    // ★ 09-28 B4b — 가방 정리 + 기본 장비 새로(`GAME3_3.c:7969~8043` — `e9` 바로 앞이 지우기, 바로 뒤가 주기 ·
    //   녹음 차례 `e9` → `4c` → `44` → `60` → `ea`). 지우기는 알림이 없어 `e9` 뒤에 몰아 해도 와이어는 같다
    if (s.gearInBag) RespawnGear(s, out);
    // 자리 — 시작 지점 고르기 → 옮기기(FORGET_DRAWABLES)
    float sx = 0.0f, sy = 0.0f;
    FindPlayerStart(s, sx, sy);
    s.x = static_cast<int>(std::lround(sx));
    s.y = static_cast<int>(std::lround(sy));
    s.posX = sx; s.posY = sy;
    s.prevX = sx; s.prevY = sy;
    s.velX = s.velY = 0.0;
    ForgetDrawables(s, out);
    // 온라인이면 무적 버프 23 을 5초(5 × 30 틱)
    ApplyEnchant(Target{ s.addr, -1 }, 23, 5 * static_cast<int>(kServerTickRate), 5);   // buffApplyTo(23, 5×FPS, 5) · GAME4.c:2201
    std::printf("[부활] 프레임 %u %s → (%.0f,%.0f) · 시작 지점 %zu곳 · 무적 %u틱\n", frame, s.addr.c_str(), sx, sy,
                playerStarts_.size(), 5 * kServerTickRate);
    std::fflush(stdout);
}

// ── 알림 (묶음 3-d) ───────────────────────────────────────────────
//
// diePlayer_54D2B0(`GAME5.c:4971~5057`) — 온라인(게임 깃발 0x2000 · 이 서버는 늘 온라인)이면 모두에게 `a9 0e` 11바이트.
//   <죽인 사람 번호2><도운 사람 번호2><죽은 사람 번호2><종류2><갈래1> · Kind1(틱 목록, `GAME3_2.c:5190~5198`) = 비신뢰
//   클라(sub_495210 · sub_495500)는 화면 위 가운데에 「죽인이 +도운이 [그림] 죽은이」를 90틱 띄운다 —
//   갈래 1 이면 그 종류의 그림(pretty_image) · 2 면 피해 종류별 주문 그림 · 그 밖은 기본 그림(주문 15)
void Server::SendKillInform(Session& s, std::vector<OutPacket>& out) {
    // 죽인 사람 v24 = findParentChainPlayer(+520) — 사람이면 그 번호
    uint16_t killer = 0;
    if (!s.lastHit.player.empty()) {
        const auto k = sessions_.find(s.lastHit.player);
        if (k != sessions_.end() && k->second.inGame) killer = k->second.netCode;
    }
    // 도운 사람 v2 — +3600 이 켜져 있고 10초(10×FPS) 안이면 +3604 사람(살아 있는 유닛) · 죽인 이·죽은 이와 같으면 0(sub_4E7540 · 4-a4)
    uint16_t assist = 0;
    if (s.assistSet && frame_ - s.assistFrame < 10u * kServerTickRate) {
        const auto h = sessions_.find(s.assistBy);
        if (h != sessions_.end() && h->second.inGame && h->second.addr != s.lastHit.player && h->second.addr != s.addr)
            assist = h->second.netCode;
    }
    // 갈래·종류 — +520 이 몬스터면(또는 주인이 몬스터인 무기·투사체면) 갈래 1·그 몬스터 종류, 그 밖은 기록(+304/+300)
    uint8_t kind = s.killKind;
    uint16_t type = s.killType;
    if (s.lastHit.cls == 2 || (s.lastHit.cls == 3 && s.lastHit.ownerCls == 2)) {
        kind = 1;
        type = s.lastHit.monsterType;
    }
    const uint8_t msg[11] = {
        static_cast<uint8_t>(Op::INFORM), static_cast<uint8_t>(InformCode::Kill),
        static_cast<uint8_t>(killer), static_cast<uint8_t>(killer >> 8),
        static_cast<uint8_t>(assist), static_cast<uint8_t>(assist >> 8),
        static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
        static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8), kind,
    };
    for (auto& kv : sessions_) {
        if (kv.second.inGame) Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
    std::printf("[죽음 알림] 프레임 %u 죽은 %u · 죽인 %u · 종류 %u · 갈래 %u\n", frame_, s.netCode, killer, type, kind);
    std::fflush(stdout);
    s.killKind = 0;                                        // +304 = 0
    // ⚠ 갈래 2·종류 2 면 죽인 사람의 능력 1(광전사 돌진) 쉬는 시간을 지운다(sub_4FC0B0 · `GAME5.c:5052~5053`) → 묶음 7
}

// playerReportAnything_4D9900(`GAME3_2.c:4855`) — 자기 몸 갱신(`GAME4_1.c:4428`)마다 「알린 값과 다른 것」만 본인에게 보낸다.
//   옮긴 갈래 — 방어값: +228(지금) 과 +232(알린 값)를 float 로 견줘 다르면 `49 <float 4>` 를 Packet0(중요 · 봉투 없음) · 알린 값을 맞춘다
//   ⚠ 나머지 갈래 — 금 `4a`(+2164, 아이템 5) · 마법부여 표시 `5b`(+440, 아이템 5) · 퀘스트 목숨·은/금 열쇠 `f0 04~06`(8) ·
//     총량 다시 알림 깃발 2184(dd·de·스탯) · 팀 체력(netSendPlrHealthToTeam) · 마나 +4≠+6(netReportMana — 지금은 쓰는 자리마다 우리 식)
// playerReportAnything_4D9900(`GAME3_2.c:4855~4970`) — 자기 갱신(a1 == a2 · netUpdate_518EE0 → netPlayerObjSend) 때 매 틱 「바뀐 것만」
void Server::PlayerReportAnything(Session& s, std::vector<OutPacket>& out) {
    // ① 방어 +228 ≠ +232 → `49 <f32>`(Packet0)
    const float armor = ArmorValueOf(s);   // ★ 09-29 — 입은 갑옷의 합(`53E300`) · 전에는 직업 표
    if (s.armorReported != armor) {
        uint32_t bits = 0;
        std::memcpy(&bits, &armor, sizeof(bits));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_ARMOR_VALUE), static_cast<uint8_t>(bits),
                                 static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits >> 16),
                                 static_cast<uint8_t>(bits >> 24) };
        EmitImportantRaw(s, msg, sizeof(msg), out);        // netReportArmorVal_4D8A30 — netSendPacket0
        s.armorReported = armor;
    }
    // ② 금 +2164 ≠ +2168 → `4a` · ③ 마법부여 표시 +440 ≠ +2172 → `5b` → 아이템(5) · 퀘스트(4096) 목숨·열쇠 → 모드(8)
    // ④ +2184(plrReadVals) → `dd` 총 체력(Packet1) · `de` 총 마나(sub_4E5450 · +2251 직업 0 = 워리어면 안 보냄) · `48` 능력치 14B(Packet0)
    if (s.statsDirty) {
        const uint8_t dd[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.health),
                                static_cast<uint8_t>(s.health >> 8), static_cast<uint8_t>(s.maxHealth),
                                static_cast<uint8_t>(s.maxHealth >> 8) };
        EmitSeq(s, dd, sizeof(dd), out);                   // netReportTotalHealth_4D85C0
        if (s.charClass != CharClass::Warrior) {
            const uint8_t de[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_MANA), static_cast<uint8_t>(s.netCode),
                                    static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.mana),
                                    static_cast<uint8_t>(s.mana >> 8), static_cast<uint8_t>(s.maxMana),
                                    static_cast<uint8_t>(s.maxMana >> 8) };
            EmitImportantRawSupersede(s, de, sizeof(de), out);   // netReportTotalMana_4D88C0
        }
        // servSendStats_4D8990(`GAME3_2.c:4201`) — 번호 · 최대 체력(+556+4) · 최대 마나(+8) · 짐 한계(+490) · 속도(+2235) · 힘(+2239) · 레벨(+3684)
        const uint16_t carry = static_cast<uint16_t>(CarryCapacityOnline(s.charClass));
        const uint16_t spd = static_cast<uint16_t>(SpeedOnline(s.charClass));
        const uint16_t str = static_cast<uint16_t>(s.strength);
        const uint8_t st[14] = { static_cast<uint8_t>(Op::REPORT_STATS), static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                                 static_cast<uint8_t>(s.maxHealth), static_cast<uint8_t>(s.maxHealth >> 8),
                                 static_cast<uint8_t>(s.maxMana), static_cast<uint8_t>(s.maxMana >> 8),
                                 static_cast<uint8_t>(carry), static_cast<uint8_t>(carry >> 8),
                                 static_cast<uint8_t>(spd), static_cast<uint8_t>(spd >> 8),
                                 static_cast<uint8_t>(str), static_cast<uint8_t>(str >> 8), s.charLevel };
        EmitImportantRaw(s, st, sizeof(st), out);
        s.statsDirty = false;
    }
    // ⑤ 체력 +10 ≠ 지금 → `43 <체력2>`(Packet1 · 본인만 — 번호가 안 실린다) · 퀘스트면 팀에 `c4 0c`(8) · +10 = 지금
    if (s.health != s.healthReported) {
        const uint8_t hp[3] = { static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH), static_cast<uint8_t>(s.health), static_cast<uint8_t>(s.health >> 8) };
        EmitSeq(s, hp, sizeof(hp), out);                   // netSendPlrHealthToTeam_4D86E0
        ++healthAbsSent_;
        s.healthReported = s.health;
    }
    // ⑥ 마나 +4 ≠ +6 → `45 <번호2> <마나2>`(sub_4E5450 · 워리어는 안 보냄) · +6 = +4
    if (s.mana != s.manaPrev) {
        if (s.charClass != CharClass::Warrior) {
            const uint8_t mp[5] = { static_cast<uint8_t>(Op::REPORT_MANA), static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                                    static_cast<uint8_t>(s.mana), static_cast<uint8_t>(s.mana >> 8) };
            EmitImportantRawSupersede(s, mp, sizeof(mp), out);   // netReportMana_4D8930
        }
        s.manaPrev = s.mana;
    }
}

// sdecode `0x7B` MSG_TRY_COLLIDE(`server__network__sdecode.c:256`) — 클라는 「쓰기」 커서(IMMOBILE 상자·미믹, `input_c.go:372~385`)에서
//   누르면 `7b <번호2>` 를 보낸다(`GAME1_2.c:3162`). 서버는 관전(3680 & 3)·가게(+280)·대화(+284) 중이 아니면
//   그 오브젝트의 충돌 함수(+696)를 (그것, 사람, 0) 으로 부른다. 윗비트(0x8000)가 선 번호는 extent 다(packetDynamicUnitCode_578B40)
//   ⚠ 충돌 함수 자체(ChestCollide 등)는 그 오브젝트 묶음(상자·보물 5 · 맵 오브젝트 9)에서 옮긴다. 지금은 찾고 · 세고 · 적는다
void Server::TryCollide(Session& s, uint16_t code, std::vector<OutPacket>& out) {
    (void)out;
    // 3680 & 1 관전 · & 2 몬스터 지켜보기(관전 기능 — 8) · 가게·대화 모임은 아직 없다(5·10)
    if (s.observer) return;
    uint16_t netCode = code;
    if (code & 0x8000) {                                   // extent → 그 오브젝트의 번호(없으면 0)
        netCode = 0;
        for (const auto& m : mapObjects_) {
            if (m.extent == static_cast<uint32_t>(code & 0x7FFF)) { netCode = m.netCode; break; }
        }
    }
    if (netCode == 0) return;
    // nox_server_getObjectFromNetCode_4ECCB0 — 사람 · 소환수·몬스터 · 깃발 · 맵 오브젝트
    bool found = false;
    uint16_t type = 0;
    for (const auto& kv : sessions_) {
        if (kv.second.inGame && kv.second.netCode == netCode) { type = kPlayerType; found = true; break; }
    }
    for (size_t i = 0; !found && i < pets_.size(); ++i) {
        if (pets_[i].netCode == netCode) { type = pets_[i].type; found = true; }
    }
    for (size_t i = 0; !found && i < flags_.size(); ++i) {
        if (flags_[i].netCode == netCode) { type = flags_[i].type; found = true; }
    }
    for (size_t i = 0; !found && i < mapObjects_.size(); ++i) {
        if (mapObjects_[i].netCode == netCode) { type = mapObjects_[i].type; found = true; }
    }
    if (!found) return;
    ++tryCollides_;
    const auto fn = collideFn_.find(type);
    if (fn == collideFn_.end() || fn->second.empty()) return;   // +696 이 없으면 아무 일 없음
    ++tryCollideDeferred_;
    std::printf("[충돌 요청] 프레임 %u %s → 번호 %u 종류 %u 충돌 함수 %s — 아직 안 옮김\n",
                frame_, s.addr.c_str(), netCode, type, fn->second.c_str());
    std::fflush(stdout);
}

// ── 시전 틀 (묶음 4-a) ───────────────────────────────────────────────
//
// 정답지 흐름 — 받기 sdecode:211 → spellByBookInsert_4FE340(목록에 넣고 상태 2) → 매 틱 spellCastByBook_4FCB80
//   (음소를 3틱마다 하나씩 나무를 따라 걷고 · 첫 틱에 시전자에게 `70 <주문>`) → 끝에 PlayerSpell(player.go:462 — 검사·마나·inform)
//   → castSpellByUser·castSpell(spells.go:422 — 공격 주문이면 투명·무적·타원 방패 끔 · 겨눈 주문은 날림)
//   → spellAccept(spells.go:193 — 효과 함수 갈래)

// server/spells.go createSpellFrom — 정의를 넣고 음소 나무를 자라게 한다(끝 자리 Ind = 주문 번호 · 음소 끝에 End 4)
void Server::SetSpellDef(uint8_t spell, uint8_t mana, uint32_t flags, const std::vector<uint8_t>& phonemes,
                         uint16_t castSound, uint16_t onSound, uint16_t offSound) {
    if (spell == 0 || spell >= 137) return;
    SpellDef& d = spellDefs_[spell];
    d.valid = true;
    d.enabled = true;
    d.mana = mana;
    d.flags = flags;
    d.castSound = castSound;   // 10-f — thing.bin SPEL 칸의 소리 셋
    d.onSound = onSound;
    d.offSound = offSound;
    d.phonemes.clear();
    spellCost_[spell] = mana;
    if (phonemes.empty()) return;
    int leaf = 0;
    for (uint8_t ph : phonemes) {
        if (ph >= 9) return;
        int next = phonemeTree_[static_cast<size_t>(leaf)].next[ph];
        if (next < 0) {
            phonemeTree_.push_back(PhonemeNode{});
            next = static_cast<int>(phonemeTree_.size() - 1);
            phonemeTree_[static_cast<size_t>(leaf)].next[ph] = next;
        }
        leaf = next;
    }
    phonemeTree_[static_cast<size_t>(leaf)].ind = spell;
    d.phonemes = phonemes;
    d.phonemes.push_back(4);                               // spell.PhonEnd
}

// spellGetPower_4FE7B0(`GAME4.c:3321`) — 멀티 모드(게임 깃발 1392 = KOTR|CTF|깃발볼|아레나|제거전)면 3 · 사람은 SpellLvl
//   ⚠ 이 서버의 모드(아레나·CTF·퀘스트)는 퀘스트만 1392 밖 — 퀘스트는 SpellLvl(온라인이면 전부 3)
int Server::SpellPower(const Session& s, uint8_t spell) const {
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest) return 3;
    return spell < 137 ? s.spellLvl[spell] : 0;
}

// sub_4FD0E0(`GAME4.c:2686`) — 꺼진 주문 10 · 사람이면 playerCheckSpellClass_57AEA0(`GAME5_2.c:1435`):
//   위저드 0x2000000 · 컨저러 0x4000000 · 누구나 0x1000000 가 없으면 9 (워리어는 늘 9)
int Server::SpellCheckClass(const Session& s, uint8_t spell) const {
    if (spell >= 137 || !spellDefs_[spell].valid || !spellDefs_[spell].enabled) return 10;
    const uint32_t fl = spellDefs_[spell].flags;
    uint32_t cls;
    if (s.charClass == CharClass::Wizard) cls = 0x2000000;
    else if (s.charClass == CharClass::Conjurer) cls = 0x4000000;
    else return 9;
    return ((fl & 0x1000000) || (fl & cls)) ? 0 : 9;
}

// checkPlrCantCastSpell_4FD150(`GAME4.c:2706`)
//   ⚠ 옮기지 않은 갈래 — KOTR 왕관·깃발볼 공(17·16)은 모드 8 ·
//     주먹(29)·자연의 힘(31)·유성(52) 이미 있음 3 · 매직미사일(50)·픽시(58) 개수 표는 그 효과(4-c) 와 함께
int Server::CheckPlrCantCastSpell(const Session& s, uint8_t spell, bool glyph) const {
    if (!glyph && flags_.size() > 0 && (SpellFlags(spell) & 0x80000)) {
        // CTF(게임 깃발 32) — 가진 것 가운데 FLAG 클래스(0x10000000)가 있으면 13
        for (const auto& f : flags_) if (f.carrier == s.addr) return 13;
    }
    if (s.buffs.Has(29)) return 14;                        // LABEL_9 — 반마법(ANTI_MAGIC 29) 버프면 14(4-a4)
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && (spell == 111 || spell == 112 || spell == 113 || spell == 114)) return 10;   // sub_4D7100 — 퀘스트 전용 소환
    return 0;
}

// sub_4FCF90(`GAME4.c:2638`) — 소환(75~114)은 blob_587000 217668 표 · 그 밖 ManaCost(,a3) · 모자라면 −1
//   ⚠ 모자랄 때 +160 = ManaCost(,1) · +164 = FPS 는 HUD 깜빡임 자리(아직 읽는 곳을 못 찾음 — 옮기지 않음)
int Server::SpellManaTake(Session& s, uint8_t spell, int costKind, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137) return -1;
    int cost;
    if (spell >= 75 && spell <= 114) {
        static const uint8_t kSummonCost[40] = {   // blob_587000 217668 + 4×(75~114)
            15, 60, 60, 60, 30, 30, 30, 15, 60, 60, 15, 30, 30, 30, 85, 85, 30, 60, 85, 60,
            30, 30, 60, 30, 15, 30, 85, 30, 30, 15, 60, 30, 30, 30, 30, 60, 60, 60, 60, 60 };
        cost = kSummonCost[spell - 75];
    } else {
        cost = spellDefs_[spell].mana;
        if (costKind == 2) {
            // ManaCost(id, 2) — 함정에 넣을 때 에너지볼트·연쇄번개·마나폭탄은 gamedata 값(4-e 글리프에서)
        }
    }
    (void)costKind;
    if (s.mana < cost) return -1;
    PlayerManaSub(s, cost);                                // playerManaSub_4EEBF0(`GAME4.c:2663`) — `45` 는 다음 틱 PlayerReportAnything(+4 ≠ +6)
    (void)out;
    return cost;
}

void Server::SpellManaGive(Session& s, int mana, std::vector<OutPacket>& out) {
    if (mana <= 0) return;
    PlayerManaAdd(s, mana);                                // sub_4FD030 → playerManaAdd_4EEB80
    (void)out;
}

// ── 갱신 알림 정답지화(09-14) — 마나는 「이전 값(+6)」을 남기고, 알림은 PlayerReportAnything 이 한다 ──
// playerManaAdd_4EEB80(`GAME3_3.c:7466`) — +6 = 지금 · +4 += v · 최대로 자름(보호 CRC 는 안 옮김)
void Server::PlayerManaAdd(Session& s, int v) {
    s.manaPrev = s.mana;
    int after = static_cast<int>(s.mana) + v;
    if (after > s.maxMana) after = s.maxMana;
    if (after < 0) after = 0;
    s.mana = static_cast<uint16_t>(after);
}
// playerManaSub_4EEBF0(`GAME3_3.c:7502`) — +6 = 지금 · 모자라면 0(GODMODE 는 안 옮김)
void Server::PlayerManaSub(Session& s, int v) {
    s.manaPrev = s.mana;
    s.mana = static_cast<uint16_t>(s.mana > v ? s.mana - v : 0);
}
// playerManaRefresh_4EECF0(`GAME3_3.c:7573`) — +6 = 지금 · +4 = 최대
void Server::PlayerManaRefresh(Session& s) {
    s.manaPrev = s.mana;
    s.mana = s.maxMana;
}

// sub_4E5450(`GAME3_3.c:1767`) — 이 사람 대기열에서 **첫 바이트(옵)가 같은 것**을 지우고 Packet0 으로 싣는다.
//   정답지 저장본은 맨 메시지라 Packet1 것도 지워지지만, 우리가 이 길로 보내는 것(`45`·`de`)은 다 Packet0 이라 맨 항목만 본다.
void Server::EmitImportantRawSupersede(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    if (n == 0) return;
    const uint8_t op = msg[0];
    s.importantQ.erase(std::remove_if(s.importantQ.begin(), s.importantQ.end(),
                                      [op](const Session::PendingImportant& q) { return !q.wire.empty() && q.wire[0] == op; }),
                       s.importantQ.end());
    EmitImportantRaw(s, msg, n, out);
}

// netPlayerObjSend_518C30(`GAME4_1.c:4405~4425`) · netSendComplexObject_518960(`:4270~4280`) 의 a4/a3 갈래 —
//   오브젝트가 **보는 사람마다** 「마지막으로 알린 체력」을 든다(사람 +12+2×i · 몬스터 +412+2×i). 마지막 피해(+536) 뒤 2틱 넘으면 견줘
//   다르면 netReportHealthDelta_4D8760(보는 이, 번호, 지금 − 알린 것) → **음수만** `42 <번호2> <증감2>` Packet0. 그러고 알린 것 = 지금.
//   ⚠ 처음 보는 오브젝트는 정답지도 0 과 견준다(양수 → 안 보냄 · 기록) — 우리는 「없으면 지금 값을 적고 안 보냄」으로 같다
void Server::ReportHealthDeltaTo(Session& viewer, uint16_t netCode, uint16_t hp, uint32_t lastDamageFrame,
                                 std::map<std::string, uint16_t>& seen, std::vector<OutPacket>& out) {
    // ★ 조각 I — **막힌 것도 센다.** 아래 관문 셋 중 어디서 걸려도 42 는 안 나가는데,
    //   나간 것만 세면 그 0 이 「일이 없었다」인지 「관문에 걸렸다」인지 안 갈린다.
    if (frame_ - lastDamageFrame <= 2) { ++healthDeltaHeld_; return; }
    auto it = seen.find(viewer.addr);
    if (it == seen.end()) { seen.emplace(viewer.addr, hp); ++healthDeltaHeld_; return; }
    if (it->second == hp) return;
    const int delta = static_cast<int>(hp) - static_cast<int>(it->second);
    if (delta < 0) {
        const uint16_t raw = static_cast<uint16_t>(static_cast<int16_t>(delta));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA), static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8),
                                 static_cast<uint8_t>(raw), static_cast<uint8_t>(raw >> 8) };
        EmitImportantRaw(viewer, msg, sizeof(msg), out);
        ++healthDeltaSent_;
    } else {
        ++healthDeltaHeld_;                            // 늘어난 것은 42 로 안 보낸다(음수만)
    }
    it->second = hp;
}

// netInformTextMsg(server/network.go:206) — `a9 <갈래> <u32>` Kind1(틱 목록) · 갈래 0 = 주문 실패 코드 · 1 = 시전 성공 주문 번호
void Server::InformSpell(Session& s, uint8_t code, uint32_t val, std::vector<OutPacket>& out) {
    // ★ 2026-09-18 — **무엇을 알렸는지 찍는다.** 클라는 이 값을 글씨로 바꿔 화면에 띄우는데
    //   (`You cannot see that spot.` 따위), 서버 로그만 보면 왜 그 글씨가 떴는지 못 가른다.
    //   같은 (code,val) 쌍은 처음 한 번만 찍는다.
    if (informShown_.insert((static_cast<uint32_t>(code) << 24) | (val & 0xFFFFFF)).second) {
        std::printf("[알림] %s code=%u val=%u\n", s.addr.c_str(), code, val);
        std::fflush(stdout);
    }
    const uint8_t msg[6] = { static_cast<uint8_t>(Op::INFORM), code, static_cast<uint8_t>(val),
                             static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val >> 16), static_cast<uint8_t>(val >> 24) };
    Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
}

// netReportSpellStat(server/network.go:362) — `df <u32 주문> <u8>` Packet0(중요 · 봉투 없음)
void Server::ReportSpellStat(Session& s, uint32_t spell, uint8_t stat, std::vector<OutPacket>& out) {
    const uint8_t msg[6] = { static_cast<uint8_t>(Op::REPORT_SPELL_STAT), static_cast<uint8_t>(spell),
                             static_cast<uint8_t>(spell >> 8), static_cast<uint8_t>(spell >> 16),
                             static_cast<uint8_t>(spell >> 24), stat };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// netPriMsgToPlayer_4DA2C0(`GAME3_2.c:5209`) — `a9 0f <a3> <글 열쇠>\0` Kind1 · 글은 클라가 CSF 에서 찾는다 · 48 자 넘으면 안 보냄
void Server::SendPriMsg(Session& s, const char* key, std::vector<OutPacket>& out) {
    const size_t n = std::strlen(key);
    if (n == 0 || n > 0x30) return;
    // ★ 2026-09-18 — **화면에 뜨는 글씨를 로그에도 남긴다.**
    //   클라 화면 글씨(`You cannot see that spot.` 따위)는 **로그보다 직접적인 단서**인데
    //   사진에만 있어 놓치기 쉽다. 같은 키가 쏟아지면 로그가 묻히므로 **키마다 처음 한 번만** 찍는다.
    if (priMsgShown_.insert(key).second) {
        std::printf("[화면글씨] %s → %s\n", s.addr.c_str(), key);
        std::fflush(stdout);
    }
    ++priMsgSent_;
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(Op::INFORM));
    msg.push_back(15);
    msg.push_back(0);
    msg.insert(msg.end(), key, key + n + 1);
    Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
}

// spellByBookInsert_4FE340(`GAME4.c:3090~3265`)
bool Server::SpellByBookInsert(Session& s, const uint32_t spells[5], int count, uint32_t gap, bool targetSelf,
                               std::vector<OutPacket>& out) {
    // ★ 2026-09-18 ⑰ — **거절 갈래를 따로 센다.** 「주문 0」만으로는 어느 문에서
    //   막히는지 못 가른다(무인 시전이 여기서 세 번 튕겼다: 직업 → ? → ?).
    if (s.respawnAtFrame != 0) { ++castRejDead_; return false; }   // +16 & 0x8022 — 죽음(0x8000)
    for (int i = 0; i < 5; ++i) if (spells[i] >= 137) { ++castRejRange_; return false; }
    for (int i = 0; i < 5; ++i) if (spells[i] && s.spellLvl[spells[i]] == 0) { ++castRejUnknown_; return false; }   // 안 배움
    if (s.castFrame != 0) { ++castRejBusy_; return false; }         // 시전 중
    bool glyph = false;
    for (int i = 0; i < count; ++i) if (spells[i] == 34) glyph = true;
    if (glyph) {
        // ⚠ 글리프 갈래(spellCheckSmth 마나 합 · 함정 수 한도 MaxTrapCount · 컨저러 폭탄병 한도)는 4-e 에서
        return false;
    }
    int err = SpellCheckClass(s, static_cast<uint8_t>(spells[0]));
    if (err) ++castRejClass_;
    if (!err) {
        err = CheckPlrCantCastSpell(s, static_cast<uint8_t>(spells[0]), false);
        if (err) ++castRejCant_;
    }
    if (err) {
        InformSpell(s, 0, static_cast<uint32_t>(err), out);   // + 소리 231(10)
        return false;
    }
    ++castAccepted_;
    // 받은 조작은 다음 틱(frame_ + 1)의 것이다 — 정답지는 같은 틱 안에서 받기 → 유닛 → 시전 순으로 돈다
    const uint32_t now = frame_ + 1;
    PlayerSetState(s, 2, now);
    s.castFlag = 1;
    s.castFrame = now;
    CastEntry e;
    e.caster = s.addr;
    for (int i = 0; i < 5; ++i) e.spells[i] = (i < count) ? spells[i] : 0;
    e.glyph = glyph;
    e.nextFrame = now;
    e.gap = gap;
    e.targetSelf = targetSelf;
    castList_.insert(castList_.begin(), e);                // 목록 머리
    return true;
}

// spellCastByBook_4FCB80(`GAME4.c:2401~2573`) — 매 틱. 머리부터 돈다
void Server::SpellCastByBook(uint32_t frame, std::vector<OutPacket>& out) {
    for (size_t i = 0; i < castList_.size();) {
        CastEntry& e = castList_[i];
        auto it = sessions_.find(e.caster);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) {   // +16 & 0x8020
            castList_.erase(castList_.begin() + static_cast<long>(i));
            continue;
        }
        Session& s = it->second;
        if (frame < e.nextFrame) { ++i; continue; }
        const uint32_t spell = e.spells[e.slot];
        if (e.phoneme == 0 && !noSpellStart_) {
            const uint8_t msg[2] = { static_cast<uint8_t>(Op::REPORT_SPELL_START), static_cast<uint8_t>(spell) };
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);   // Kind1 — 시전자에게만
        }
        if (static_cast<uint32_t>(phonemeTree_[static_cast<size_t>(e.leaf)].ind) != spell) {
            // 음소 하나 — 남에게 음소 소리(sub_4FC960 → 10) · 나무를 한 칸 · 사람이면 +184
            uint8_t ph = 0;
            if (spell < 137 && e.phoneme < spellDefs_[spell].phonemes.size()) ph = spellDefs_[spell].phonemes[e.phoneme];
            int next = -1;
            if (e.leaf >= 0 && ph < 9) next = phonemeTree_[static_cast<size_t>(e.leaf)].next[ph];
            // PhonemeLeaf.Next 가 nil 이면 정답지는 nil 자리를 가리킨다 — 다음 비교에서 v7(nil 의 첫 u32) 을 읽는다.
            //   정의가 있는 주문은 음소를 끝까지 따라가면 반드시 그 주문 자리에 닿는다. 없으면 뿌리로 두어 「모름」으로 끝낸다.
            e.leaf = (next >= 0) ? next : 0;
            s.castLeaf = e.leaf;
            ++e.phoneme;
            e.nextFrame = frame + e.gap;
            if (next < 0) {
                // 나무에서 떨어졌다 — 다음 틱에 끝 갈래로(음소 수를 넘기면 정답지도 PhonEnd 뒤 nil)
                e.phoneme = 0xFF;
            }
            if (e.phoneme != 0xFF) { ++i; continue; }
        }
        // ⚠ 글리프 갈래(+29 — 마나 선불·+192 목록·다음 칸)는 4-e
        s.castCursorX = static_cast<float>(s.aimX);        // +220/+224 = playerinfo 571/572
        s.castCursorY = static_cast<float>(s.aimY);
        const bool targetSelf = e.targetSelf;
        castList_.erase(castList_.begin() + static_cast<long>(i));
        // playerinfo[910] = [12] ? 시전자 : +288(커서 오브젝트) — PlayerSpell 이 쓴다
        Target tgt;
        if (targetSelf) tgt.addr = s.addr;
        else tgt = FindObjectAtCursor(s);
        spellTarget_ = tgt;
        PlayerSpell(s, out);
        s.castFrame = 0;
        s.castFlag = 0;
        s.glyphCount = 0;
    }
}

// PlayerSpell(player.go:462~533)
void Server::PlayerSpell(Session& s, std::vector<OutPacket>& out) {
    bool unknown = true;                                   // ok2
    int err = 1;                                           // a1
    const int leaf = s.castLeaf;
    const uint8_t spell = (leaf > 0) ? static_cast<uint8_t>(phonemeTree_[static_cast<size_t>(leaf)].ind) : 0;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    // ★ 2026-09-18 — **여기 갈래가 계수기 없이 새고 있었다.** 클라가 주문 29 를 5번 보내고
    //   서버도 5번 받았는데(`castAccepted_` = 5) 발사가 **0** 이고 거절 갈래도 **전부 0** 이었다.
    //   조용히 빠지는 자리가 셋 — ⑴ 잎 0(음소를 못 따라감) ⑵ 잎의 주문 번호가 0 ⑶ **안 배움**.
    //   ⑶ 은 `if (spellLvl != 0 || spell == 34)` 가 거짓일 때 `unknown` 인 채로 그냥 나간다.
    ++spellPlayerCalls_;
    if (leaf == 0) {
        unknown = true;
        ++spellLeafZero_;
    } else if (spell == 0) {
        ++spellIndZero_;
    } else {
        if (s.spellLvl[spell] == 0 && spell != 34) ++spellNotLearned_;
        if (!quest && (SpellFlags(spell) & 0x20)) {
            const Target cur = FindObjectAtCursor(s);      // ud.CursorObj(+288)
            if (!cur.none() && !TargetIsEnemy(s, cur)) {
                ++spellFriendAtCursor_;                    // ★ 조용히 나가던 자리 — 계수기를 달았다
                return;                                    // ⚠ 정답지 그대로 — 상태 2·STAT 를 안 건드리고 나간다
            }
        }
        if (s.spellLvl[spell] != 0 || spell == 34) {
            unknown = false;
            err = SpellCheckClass(s, spell);
            if (err == 0) err = CheckPlrCantCastSpell(s, spell, false);
            if (err != 0) {
                ++spellErrInform_;                         // ★ 직업·못씀 따위로 막힌 수(갈래)
                InformSpell(s, 0, static_cast<uint32_t>(err), out);   // + 소리 PermanentFizzle(10)
            } else {
                const int mana = SpellManaTake(s, spell, 1, out);
                if (mana < 0) {
                    err = 11;
                    ++spellsNoMana_;
                    InformSpell(s, 0, 11, out);            // + 소리 ManaEmpty(10)
                } else {
                    SpellArg arg;
                    arg.obj = spellTarget_;                // Obj3640
                    if (quest && (SpellFlags(spell) & 0x20) && !arg.obj.none() && !TargetIsEnemy(s, arg.obj))
                        arg.obj = Target{};
                    arg.x = s.castCursorX;                 // CursorPos
                    arg.y = s.castCursorY;
                    if (CastSpellByUser(spell, -1, s, arg, out)) {
                        InformSpell(s, 1, spell, out);
                        ++spells_[spell];
                    } else {
                        ++spellCastFailed_;                // ★ 시전 함수가 거짓을 돌려준 수(갈래)
                        SpellManaGive(s, mana, out);
                        err = 8;
                    }
                }
            }
        }
    }
    if (s.moveState == 2) PlayerSetState(s, kPlayerStateIdle, frame_);
    if (unknown) {
        // 줄 글 "plyrspel.c:SpellUnknown"(netSendLineMessage_4D9EB0 · `a8` 글 — 서버가 문자열표를 찾아 싣는다) → 10
    } else if (err != 0) {
        ReportSpellStat(s, spell, 0, out);
    } else if (!(SpellFlags(spell) & 0x100000)) {
        ReportSpellStat(s, spell, 15, out);
    }
}

// castSpellByUser4FDD20 · castSpell(spells.go:422~447)
bool Server::CastSpellByUser(uint8_t spell, int lvl, Session& s, const SpellArg& arg, std::vector<OutPacket>& out) {
    if (lvl < 0) lvl = SpellPower(s, spell);
    if (SpellFlags(spell) & 0x20) {
        // 공격 주문 — 투명(0) 끄기 · 무적(23) 끄기 · 타원 방패(67) 지속 주문 취소
        BuffOff(Target{ s.addr, -1 }, 0);
        BuffOff(Target{ s.addr, -1 }, 23);
        DurCancelFor(67, s.addr, out);
    }
    if (!(SpellFlags(spell) & 0x4) || arg.obj.addr == s.addr) return SpellAccept(spell, s, s.addr, arg, lvl, out);
    // 겨눈 주문 — createSpellFly_4FDDA0 로 "Magic" 오브젝트를 날린다(4-a2) · 닿으면 SpellProjectileCollide 가 받아들인다
    (void)lvl;
    return CreateSpellFly(s, arg.obj, spell, out);
}

// spellAccept(spells.go:193~420)
bool Server::SpellAccept(uint8_t spell, Session& s, const std::string& obj3, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137 || obj3.empty()) return false;   // spellID 0 · obj3 == nil(날아가는 것의 주인이 사라짐)
    if (!spellDefs_[spell].valid) return false;
    const uint32_t fl = spellDefs_[spell].flags;
    // Unk8(0x80) — 대상이 있는데 유닛이 아니면 거절 · 지금 대상은 사람·소환수(유닛)뿐
    // gameCaptureMagic_4FDC10 — CTF 에서 깃발 든 사람에게 CANT_HOLD_CROWN 주문 → 0 · 소리 PermanentFizzle(10)
    if (!arg.obj.none() && !arg.obj.addr.empty() && (fl & 0x80000)) {
        for (const auto& f : flags_) if (f.carrier == arg.obj.addr) return false;
    }
    lastAcceptTarget_ = arg.obj;                             // 시험용 기록
    ++acceptCount_;
    lastAcceptCaster_ = s.addr;
    // ── 효과 갈래(spells.go:219~412 · 효과 = 주문 번호) ── 함수(fnc) 갈래는 lvl > 5 를 5 로 자르고 0 이면 PermanentFizzle(10) · false
    int fnc = -1;                                            // -1 = 이 갈래가 아직 없음
    {
        const int fl5 = lvl > 5 ? 5 : lvl;
        const Target targ = arg.obj;
        BuffConf c;
        switch (spell) {
            case 1:   c.durOpt = "AnchorEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 14, fl5, targ, c, out); break;   // castAnchor
            case 3:   c.dur = 4; c.durInSec = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 2, fl5, targ, c, out); break;         // castBlind
            case 15:  c.dur = 450; c.once = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 16, fl5, targ, c, out); break;      // castDeath
            case 17:  c.dur = 60; c.durInSec = true; fnc = CastBuffSpell(spell, 6, fl5, targ, c, out); break;                                    // castDetectMagic
            case 26:  c.durOpt = "FearEnchantDuration"; fnc = CastBuffSpell(spell, 11, fl5, targ, c, out); break;                                // castFear
            case 32:  c.dur = 4; c.durInSec = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 25, fl5, targ, c, out); break;    // castFreeze
            case 36:  c.durOpt = "HasteEnchantDuration"; fnc = CastBuffSpell(spell, 9, fl5, targ, c, out); break;                               // castHaste
            case 37:  c.durOpt = "InfravisionEnchantDuration"; fnc = CastBuffSpell(spell, 21, fl5, targ, c, out); break;                        // castInfravision
            case 39:  c.durOpt = "InvisibilityEnchantDuration"; c.defensive = true; c.pointFx = static_cast<uint8_t>(Op::FX_SMOKE_BLAST);
                      fnc = CastBuffSpell(spell, 0, fl5, targ, c, out); break;                                                                  // castInvisibility
            case 40:  c.durOpt = "InvulnerabilityEnchantDuration"; c.defensive = true; fnc = CastBuffSpell(spell, 23, fl5, targ, c, out); break;  // castInvulnerability
            case 42:  c.durOpt = "LightEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 15, fl5, targ, c, out); break;   // castLight
            case 55:  c.dur = static_cast<int>(BalanceFloat("WarCryDuration")); c.orig = obj3; c.offensive = true;                              // castNullify — abilities.defs[Warcry].duration
                      fnc = CastBuffSpell(spell, 29, fl5, targ, c, out); break;
            case 61:  c.durOpt = "ProtectElectricityEnchantDuration"; fnc = CastBuffSpell(spell, 20, fl5, targ, c, out); break;                 // castProtectElectricity
            case 62:  c.durOpt = "ProtectFireEnchantDuration"; fnc = CastBuffSpell(spell, 17, fl5, targ, c, out); break;                        // castProtectFire
            case 64:  c.durOpt = "ProtectPoisonEnchantDuration"; fnc = CastBuffSpell(spell, 18, fl5, targ, c, out); break;                      // castProtectPoison
            case 70:  c.durOpt = "RunEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 8, fl5, targ, c, out); break;      // castRun
            case 72:  c.durOpt = "SlowEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 4, fl5, targ, c, out); break;     // castSlow
            case 130: c.durOpt = "VampirismEnchantDuration"; c.durOptMulQuest = "QuestVampDurMultiplier";
                      fnc = CastBuffSpell(spell, 13, fl5, targ, c, out); break;                                                                 // castVampirism
            case 131: c.dur = 12; c.durInSec = true; c.durLevelMul = true; c.orig = obj3; c.offensive = true;
                      fnc = CastBuffSpell(spell, 10, fl5, targ, c, out); break;                                                                 // castVillain
            // useShock_52C5A0(`GAME4_2.c:9653`) — 대상이 없으면 0.
            //   ⚠ **글리프에 걸면** 피해 갈래다(`ShockTrapDamage[세기-1]` 종류 9 전기) — 글리프가 아직 없다(묶음 4-e).
            //   그 밖에는 **충격 마법부여(22)** 를 `ShockEnchantDuration` 만큼. 도운 사람 기록은 **안 한다**.
            case 20:  c.durOpt = "ShockEnchantDuration"; fnc = CastBuffSpell(spell, 22, fl5, targ, c, out); break;
            case 12: fnc = CastConfuse(obj3, targ, fl5); break;                                   // nox_xxx_castConfuse_52C1E0
            case 74: fnc = CastStun(obj3, targ, fl5); break;                                      // nox_xxx_castStun_52C2C0
            case 33: fnc = CastFumble(targ); break;                                               // nox_xxx_castFumble_52C060
            case 38: fnc = CastInversion(s, obj3); break;                                         // sub_52BEB0
            case 14: fnc = CastCurePoison(spell, s, targ, fl5, out); break;                       // nox_xxx_castCurePoison_52CDB0
            case 8: {                                                                          // ChannelLife — 지속(빔 없음)
                // ⚠ 지속 갈래는 **레벨을 안 자른다**(정답지는 New 를 lvl>5 자르기 **앞**에서 돌려준다) — fl5 가 아니라 lvl
                DurFns f;
                f.update = &Server::DurChannelLife;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 35: {                                                                         // GreaterHeal — 지속(치료 광선)
                DurFns f;
                f.create = &Server::DurGreaterHealCreate;
                f.update = &Server::DurGreaterHealTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 22: {                                                                         // DrainMana — 지속(마나 빨기 광선)
                DurFns f;
                f.update = &Server::DurDrainMana;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 43: {                                                                         // ChainLightning — 지속(사슬 번개) · dt 30
                DurFns f;
                f.create = &Server::DurChainLightningCreate;
                f.update = &Server::DurChainLightningTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 30, out) ? 1 : 0;
                break;
            }
            case 46: case 47: case 48: case 49:                                                // Mark 1~4 — 표식 걸기
                fnc = CastMark(spell, s, arg, out);
                break;
            case 121: {                                                                        // TeleportPop — 무작위 표식으로
                DurFns f;
                f.create = &Server::DurTelePopCreate;
                f.update = &Server::DurTelePopTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 122: case 123: case 124: case 125: {                                          // TeleportToMark 1~4
                DurFns f;
                f.create = &Server::DurTeleMarkCreate;
                f.update = &Server::DurTeleMarkTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83:
            case 84: case 85: case 86: case 87: {                                              // Summon — 지속(크기별 시전 시간)
                DurFns f;
                f.create = &Server::DurSummonCreate;
                f.update = &Server::DurSummonTick;
                f.destroy = &Server::DurSummonEnd;                                             // ★ 10-01 summonCancel_5011C0
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 4: {                                                                          // Blink — 지속(한 박자 뒤 자리 옮김)
                DurFns f;
                f.create = &Server::DurBlinkCreate;
                f.update = &Server::DurBlinkTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 51: {                                                                         // Shield — 지속(마법 방패 26)
                DurFns f;
                f.create = &Server::DurShieldCreate;
                f.update = &Server::DurShieldTick;
                f.destroy = &Server::DurShieldDestroy;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 24: {                                                                         // EnergyBolt — 지속(전기 광선) · dt 30
                DurFns f;
                f.create = &Server::DurEnergyBoltCreate;
                f.update = &Server::DurEnergyBoltTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 30, out) ? 1 : 0;
                break;
            }
            case 59: {                                                                        // Plasma — 지속(완드가 있어야 돈다 · 5-f)
                DurFns f;
                f.create = &Server::DurPlasmaCreate;
                f.update = &Server::DurPlasmaTick;
                f.destroy = &Server::DurPlasmaDestroy;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f,
                             static_cast<uint32_t>(BalanceFloat("PlasmaSearchTime")), out) ? 1 : 0;
                break;
            }
            case 23: fnc = CastEarthquake(s, fl5, out); break;                                 // castEquake_52DE40
            case 60: fnc = CastPoison(obj3, targ, fl5, out); break;                            // castPoison_52C720
            case 128: fnc = CastToxicCloud(s, arg, out); break;                                // castToxicCloud_52DB60
            case 5: fnc = CastBurn(s, arg, out); break;                                       // castBurn_52C3E0
            case 27: fnc = CastFireball(s, lvl, out); break;                                  // castFireball_52C790
            case 52: fnc = CastMeteor(s, arg, lvl, out); break;                               // castMeteor_52D9D0
            case 53: fnc = CastMeteorShower(s, arg, lvl, out); break;                         // castMeteorShower_52D8A0
            case 29: fnc = CastFist(s, arg, lvl, out); break;                                 // castFist_52D3C0
            case 16: fnc = CastDeathRay(s, arg, out); break;                                  // castDeathRay(15 는 SPELL_DEATH 다)
            case 10: case 11: fnc = CastCleansingFlame(spell, s, lvl, out); break;            // spellCastCleansingFlame_52D5C0
            case 2: fnc = CastArachnaphobia(s, arg, out); break;                              // spellArachna_52DC80
            case 58: fnc = CastPixieSwarm(spell, s, lvl, out); break;                        // castPixies_540440
            case 65: fnc = CastPushPull(s, fl5, /*pull=*/true); break;                          // castPull_52BFA0
            case 66: fnc = CastPushPull(s, fl5, /*pull=*/false); break;                         // castPush_52C000
            case 41: fnc = CastLesserHeal(spell, s, targ, out); break;                            // sub_52DD50 LesserHeal
            case 68: case 133: fnc = CastRestoreHealth(targ, out); break;                         // castSpellWinkORrestoreHealth_52BF20 RestoreHealth·Wink
            case 69: fnc = CastRestoreMana(targ); break;                                          // sub_52BF50 RestoreMana
            default: break;
        }
    }
    if (fnc >= 0) return fnc != 0;                           // 0 이면 소리 PermanentFizzle(obj4) → 10
    if (SpellEffectPlaceholder(spell, s, arg, out)) return true;
    // 효과 함수를 아직 안 옮긴 주문 — 받아들인 것으로 친다(정답지 default: return true 와 같은 쪽) · 세고 적는다
    ++spellEffectDeferred_;
    std::printf("[시전] 프레임 %u %s 주문 %u(%s) — 효과 함수 아직 안 옮김(묶음 4-b~4-f)\n", frame_, s.addr.c_str(),
                spell, SpellName(spell) ? SpellName(spell) : "?");
    std::fflush(stdout);
    return true;
}

// nox_xxx_gameServerReadyMB_4DD180(`GAME3_2.c:5592`) — CLIENT_READY 마다(정품 클라는 두 번 보낸다)
//   ① +3680 |= 16(준비 — importantReady) ② 온라인·채팅 모드 아니면 무적 23 끄고 다시 5×FPS·세기 5
//   ③ ⚠ 깃발 오브젝트(클래스 0x10000000) 미니맵 표시(미니맵) ④ ⚠ 관전이면 +908/+909 = 몸 자리 · 협동 팀(512)이면 관전 풀기(8)
//   ⑤ nox_xxx_wall_4DF1E0(`wall.go:354`) — 갈래가 넷이다(09-21 에 따로 셌다):
//        ⭕ 비밀 벽(`sub_4DF2A0`) — `SecretWallSync` 가 사람마다 `secretSeen` 으로 이미 한다
//        ⭕ 문 — 09-21 `UpdateProps` 끝의 **문 되맞춤**(`doorTold`)이 같은 일을 한다
//        ⭕ 부서진 벽(`wallSendDestroyed`) — 09-29 Z1-A `SendBrokenWalls` · ⚠ 퀘스트 문(`Sub_4D6A20`) — 아직 안 옮겼다
//      ⑥ ⚠ 퀘스트 건틀릿(8)
//   ⑦ netStatsMultiplier_4D9C20 — `ef <f32 체력> <f32 마나> <f32 힘> <f32 속도>` Packet0 · 직업 배수(아레나는 초기값 1 · 퀘스트 DefaultStatMult 는 8)
void Server::GameServerReadyMB(Session& s, std::vector<OutPacket>& out) {
    if (!s.inGame) return;
    BuffOff(Target{ s.addr, -1 }, 23);
    // ★ 2026-09-18 — `--no-spawn-invuln` 이면 **부활 무적을 다시 걸지 않는다.**
    //
    //   왜 필요한가 — 클라가 CLIENT_READY 를 2초마다 되풀이 보내는데(실측 20~23회)
    //   그때마다 5초 무적을 다시 걸어 **사실상 영영 무적**이 된다. 그러면 못 움직이고
    //   맞아도 반응이 없어 **화면 확인 17건이 통째로 막힌다**(09-15 에 실제로 막혔다).
    //   되풀이의 까닭은 아직 못 닫았으므로(B-1b 미결), 확인을 진행하려고 **무적만 끈다.**
    //   ⚠ 이건 우회로다 — 정품 동작이 아니므로 ④ 판정에서 「무적」 항목은 이 스위치를
    //     **끄고** 따로 봐야 한다.
    if (!noSpawnInvuln_) {
        ApplyEnchant(Target{ s.addr, -1 }, 23, 5 * static_cast<int>(kServerTickRate), 5);
        ++spawnInvulnApplied_;     // 스위치가 실제로 먹는지 가르는 계수기
    }
    SendBrokenWalls(s, out);                               // ⑤ ★ 09-29 Z1-A — 이미 부서진 벽마다 `3a <번호2>`(정답지 차례 — 무적 뒤 · 배수 앞)
    const float mult[4] = { 1.0f, 1.0f, 1.0f, 1.0f };     // Health · Mana · Strength · Speed(server/player.go:167 init)
    uint8_t msg[17] = { static_cast<uint8_t>(Op::STAT_MULTIPLIERS) };
    for (int i = 0; i < 4; ++i) std::memcpy(msg + 1 + 4 * i, &mult[i], 4);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// ── 묶음 4-b4 디버프 C 함수 ─────────────────────────────────────────────
// nox_xxx_castConfuse_52C1E0(`GAME4_2.c:9421`) — 대상 없으면 0 · 혼란(3) ConfuseEnchantDuration · 세기 · 도운 사람(sub_4E7540 a3)
int Server::CastConfuse(const std::string& obj3, const Target& targ, int lvl) {
    if (!BuffsOf(targ)) return 0;
    ApplyEnchant(targ, 3, static_cast<int>(static_cast<float>(BalanceFloat("ConfuseEnchantDuration"))), lvl);
    MarkAssist(obj3, targ);
    return 1;
}

// nox_xxx_castStun_52C2C0(`GAME4_2.c:9436`) — StunEnchantDuration · 사람: 주문 쓰는 직업(+2251)이면 붙잡힘(5) · 워리어면 느림(4)
//   · 몬스터: 질량(+120) > 15 면 느림(4) 아니면 붙잡힘(5) · 도운 사람
int Server::CastStun(const std::string& obj3, const Target& targ, int lvl) {
    if (!BuffsOf(targ)) return 0;
    const int dur = static_cast<int>(static_cast<float>(BalanceFloat("StunEnchantDuration")));
    int buff = 5;
    if (!targ.addr.empty()) {
        buff = (sessions_.at(targ.addr).charClass != CharClass::Warrior) ? 5 : 4;
    } else {
        const auto tb = typeBodies_.find(pets_[static_cast<size_t>(targ.pet)].type);
        const float mass = (tb != typeBodies_.end()) ? tb->second.mass : 1.0f;
        buff = (mass > 15.0f) ? 4 : 5;
    }
    ApplyEnchant(targ, buff, dur, lvl);
    MarkAssist(obj3, targ);
    return 1;
}

// nox_xxx_castFumble_52C060(`GAME4_2.c:9359`) — 대상 없으면 0 · 소리(주문 On → 10) · 1
//   ⚠ 사람·NPC: 든 무기·지팡이·방패(EQUIPPED 0x100 · 클래스 0x1001000 / 0x2000000+SUBCLASS 2) 떨구기는 아이템(5) · 공 들고 있으면 밀어내기(8)
//   ⚠ 그 밖 몬스터(SUBCLASS 0x2000 아님): 가진 것 전부 떨구기(5) + 시전자 자리에서 힘 50(소환수 물리 6)
int Server::CastFumble(const Target& targ) {
    if (!BuffsOf(targ)) return 0;
    return 1;
}

// sub_52BEB0(`GAME4_2.c:9295`) — 시전자(a4) 둘레 InversionRange 안 날아가는 것마다 changeOwner(그것, a3) · 소리(10) · 1
int Server::CastInversion(const Session& a4, const std::string& obj3) {
    const float r = static_cast<float>(BalanceFloat("InversionRange"));
    const float r2 = r * r;
    for (auto& m : missiles_) {                             // getMissilesInCircle — 거리² ≤ r²
        const float dx = static_cast<float>(a4.posX) - m.posX, dy = static_cast<float>(a4.posY) - m.posY;
        if (dx * dx + dy * dy <= r2) MissileChangeOwner(m, obj3, frame_);
    }
    // ⚠ 옛 자리표시 투사체(shots_)는 Magic 이 아니라 거른다(4-c)
    return 1;
}

// nox_xxx_castCurePoison_52CDB0(`GAME4_2.c:9813`) — 대상 없으면 0 · 독(+540) 있으면 풀기(4-c 독과 함께) ·
//   독 없고 대상이 시전자가 아니면 소리 · 1 / 독 없고 자기에게면 마나를 ManaCost(,1) 만큼 되돌려 줌 · 1
int Server::CastCurePoison(uint8_t spell, Session& a2, const Target& targ, int lvl, std::vector<OutPacket>& out) {
    (void)lvl;
    if (!BuffsOf(targ)) return 0;
    const uint8_t* pp = PoisonOf(targ);                     // 4-c2a — 이제 진짜 독을 본다
    if (pp && *pp) {                                        // 독이 있으면 풀고 끝(글 PoisonCure/PoisonClean 은 10)
        RemovePoison(targ, out);
        return 1;
    }
    if (targ.addr != a2.addr) return 1;                     // 소리 → 10
    SpellManaGive(a2, spellDefs_[spell].mana, out);          // sub_4FD030(대상, ManaCost(주문, 1))
    return 1;
}

// ── 묶음 4-b5a 즉발 회복 (spells.go:288~337 · fnc(spellID, obj12, obj3, obj4, &sa, lvl)) ──
//
// sub_52DD50(`GAME4_2.c:10369`) LesserHeal — 대상 없으면 0 · 대상 최대HP && 자기(obj12==대상)면 마나 되돌림(공짜) ·
//   아니면 LesserHealAmount(gamedata) × 시전자 직업 최대체력 만큼 회복 · 소리 aud44(spell,1)
//   ⚠ 직접 시전은 obj12==obj3==시전자라 「자기」 검사 = 대상 == 시전자. 남에게 거는 것은 겨눔(0x4)이라 날아가는 것이 되어 맞을 때 SpellAccept(4-a2)
//   ⚠ nox_float2int 을 **버림**((int))으로 둔다 — 이 리포 관례(Server.cpp 피해 계산). 정품 값과 1 차이면 반올림으로 바꿀 것(화면 ④ 때 대조)
int Server::CastLesserHeal(uint8_t spell, Session& caster, const Target& targ, std::vector<OutPacket>& out) {
    if (!BuffsOf(targ)) return 0;                          // *a5 == 0 — 대상 없음
    bool atMax = false;
    if (!targ.addr.empty()) {
        auto it = sessions_.find(targ.addr);
        atMax = (it != sessions_.end() && it->second.health >= it->second.maxHealth);
    } else if (targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        const Creature& k = pets_[static_cast<size_t>(targ.pet)];
        atMax = static_cast<int>(k.health) >= TuningFor(k.type).health;
    }
    if (atMax && targ.addr == caster.addr) {               // 최대 체력 && obj12(자기) == 대상 → 마나 되돌림
        SpellManaGive(caster, spellDefs_[spell].mana, out);   // sub_4FD030(a3, ManaCost(spell, 1))
        return 1;
    }
    double amount = BalanceFloat("LesserHealAmount");       // ARENA LesserHealAmount = 15(절대 HP)
    // ⚠⚠ 여기 곱하는 것은 **직업 배수**(온라인 1.0)다 — 최대 체력이 아니다.
    //   정답지 전역 이름이 `warriorMaxHealth_587000_*` 라 최대 체력으로 읽었다가 회복량을 100배 부풀렸다(ClassHealthMult 주석).
    amount *= static_cast<double>(ClassHealthMult(caster.charClass));
    UnitAdjustHP(targ, static_cast<int>(amount), out);     // unitAdjustHP(최대 아래일 때만·최대에서 자름) · 알림은 갱신 틱(PlayerReportAnything)
    return 1;
}

// castSpellWinkORrestoreHealth_52BF20 RestoreHealth·Wink — 대상 없으면 0 · 있으면 최대 체력으로(unitHPsetOnMax) · 소리 754
//   ⚠ Wink 는 정답지에서 이 함수(회복)뿐 — 순간이동 안 딸림
int Server::CastRestoreHealth(const Target& targ, std::vector<OutPacket>& out) {
    if (!BuffsOf(targ)) return 0;
    UnitAdjustHP(targ, 999999, out);                       // 최대에서 잘려 가득 채움 · 이미 최대면 아무 일 없음(알림 없음)
    return 1;
}

// sub_52BF50 RestoreMana — 대상 없으면 0 · 대상이 사람이면 최대 마나만큼 더함(playerManaAdd·최대에서 자름) · 소리 755
int Server::CastRestoreMana(const Target& targ) {
    if (!BuffsOf(targ)) return 0;
    if (!targ.addr.empty()) {                              // 소환수는 마나 없음 — 사람만
        auto it = sessions_.find(targ.addr);
        if (it != sessions_.end() && it->second.inGame) PlayerManaAdd(it->second, it->second.maxMana);
    }
    return 1;                                              // 대상이 있으면(사람 아니어도) 받아들인 것
}

// ── 묶음 4-b5b 지속 회복·흡수 — 대상 값 읽기 ──
// unitGetOldMana_4EEC80(`GAME3_3.c:7532`) — 이름과 달리 **지금** 마나(+748+4)다. 사람 아니면: 주인 있는 소환수 1000 · 그 밖 0
int Server::TargetManaCur(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.mana) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size() && !pets_[static_cast<size_t>(t.pet)].owner.empty()) ? 1000 : 0;
}
// playerGetMaxMana_4EECB0(`:7550`) — 사람만, 아니면 0
int Server::TargetManaMax(const Target& t) const {
    if (t.addr.empty()) return 0;
    auto it = sessions_.find(t.addr);
    return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.maxMana) : 0;
}
// unitGetHP_4EE780
int Server::TargetHealth(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.health) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? static_cast<int>(pets_[static_cast<size_t>(t.pet)].health) : 0;
}
// sub_4FEA70(`GAME4.c:3352`) — x·y 어느 한쪽이라도 5 이상 떨어졌나
bool Server::TargetMovedFrom(const Target& t, float x, float y) const {
    float tx = 0, ty = 0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end()) return true;
        tx = static_cast<float>(it->second.posX); ty = static_cast<float>(it->second.posY);
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        tx = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y);
    } else {
        return true;
    }
    return std::fabs(x - tx) >= 5.0f || std::fabs(y - ty) >= 5.0f;
}

// sub_52F460(`GAME4_2.c:11444`) ChannelLife — **시전자의 생명을 대상의 마나로 옮긴다**(빔 없음 — 이 주문은 netStartDurationRaySpell 갈래에 없다)
//   대상(+48) 없음·죽음/지워짐 → 취소 · 글리프(+20)면 대상 마나 +20 · 대상 피해 20 하고 한 방에 끝
//   시전자 달리기(8) → 취소 · 대상이 소환수고 건 자리에서 5 넘게 움직임 → 취소 · **대상 마나가 최대면 취소**
//   시전자 체력 > 1 이면: ChannelLifeCoeff[세기−1] 을 소수 이월(+72)로 쌓아 대상 마나에 붓고 **시전자는 1 깎인다** · 아니면 취소
int Server::DurChannelLife(DurSpell& d, std::vector<OutPacket>& out) {
    if (!BuffsOf(d.target)) return 1;                       // +48 없음
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) return 1;   // DEAD|DESTROYED(0x8020)
    }
    if (d.flag20) {                                         // 글리프가 건 것 — 한 방(대상이 제 생명을 제 마나로)
        if (!d.target.addr.empty()) {
            auto it = sessions_.find(d.target.addr);
            if (it != sessions_.end()) PlayerManaAdd(it->second, 20);
        }
        Damage(d.target.addr, 20, out, std::string(), nullptr, /*ignoreInvuln=*/true);   // unitDamageClear
        return 1;
    }
    const Buffs* cb = BuffsOf(Target{ d.caster, -1 });
    if (cb && cb->Has(8)) return 1;                         // 시전자 달리기 — 채널 끊김
    if (d.target.addr.empty() && TargetMovedFrom(d.target, d.posX, d.posY)) return 1;   // 소환수 대상이 자리를 떴다
    if (TargetManaMax(d.target) == TargetManaCur(d.target)) return 1;                   // 대상 마나가 가득 — 더 줄 것이 없다
    if (TargetHealth(Target{ d.caster, -1 }) <= 1) return 1;                            // 시전자가 더 못 버틴다
    const float coeff = static_cast<float>(BalanceFloatInd("ChannelLifeCoeff", d.level - 1));
    const float v = d.field72 + coeff;
    const int give = static_cast<int>(v);                   // nox_float2int
    d.field72 = v - static_cast<float>(give);               // 소수 이월
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it != sessions_.end()) PlayerManaAdd(it->second, give);
    }
    Damage(d.caster, 1, out, std::string(), nullptr, /*ignoreInvuln=*/true);            // unitDamageClear(시전자, 1)
    return 0;                                               // 이어 간다
}

// unitGetMaxHP_4EE7A0
int Server::TargetMaxHealth(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.maxHealth) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? TuningFor(pets_[static_cast<size_t>(t.pet)].type).health : 0;
}

// unitCanInteractWith_5370E0(`legacy/game.go:296` → `server/object.go:105`) = **CanSee + 벽 시야추적**
bool Server::CanInteract(const Session& obj, const Target& t) const {
    const Buffs* tb = BuffsOf(t);
    if (!tb) return false;
    float tx = 0, ty = 0;
    double tvx = 0, tvy = 0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end() || !it->second.inGame) return false;
        tx = static_cast<float>(it->second.posX); ty = static_cast<float>(it->second.posY);
        tvx = it->second.velX; tvy = it->second.velY;
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        tx = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y);
    } else {
        return false;
    }
    if (!CanSeeBuffs(&obj.buffs, tb, tvx, tvy, 0)) return false;
    return MapTraceVision(tx, ty, static_cast<float>(obj.posX), static_cast<float>(obj.posY));
}

// sub_4E6BD0(`GAME3_3.c:2372`) — 체력이 있고 **지난 1틱 안에** 맞았나(+536). 치료 광선은 맞는 순간 끊긴다
bool Server::TargetJustDamaged(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return it != sessions_.end() && it->second.inGame && (frame_ - it->second.lastDamageFrame) <= 1;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return false;
    return (frame_ - pets_[static_cast<size_t>(t.pet)].lastDamageFrame) <= 1;
}

// netStartDurationRaySpell_4FF130(`GAME4.c:3429`) — `9e <갈래> <값> <번호2> <번호2>` 7바이트 Packet1 **모두에게**
//   갈래는 멈춤(NetStopRaySpell)과 **다른 표**다: 7→3 · 9→2 · 22→5 · 24→4 · 35→6 · 59→1(값 자리에 시전자 방향) · 43→하위 조각마다
//   그 밖(8 ChannelLife 등)은 **빔이 없다** — switch 의 default 가 아무것도 안 보낸다
//   ⚠ 미니맵 표시(netMarkMinimapForAll 2)는 미니맵 묶음에서
void Server::NetStartRaySpell(const DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool petCaster = d.casterPet != 0;                  // ★ 09-29 B7 — 몬스터 시전자(번호 그대로)
    if (!petCaster && (d.caster.empty() || c == sessions_.end() || !c->second.inGame)) return;
    uint8_t typ = 0, val = static_cast<uint8_t>(d.level);
    // 하위 조각(연쇄 번개)은 **시작 쪽이 사람이 아닐 수도** 있다 — fromUnit 이 있으면 그쪽 번호를 쓴다
    uint16_t n1 = !d.fromUnit.none() ? UnitNetCode(d.fromUnit) : (petCaster ? d.casterPet : c->second.netCode);
    if (petCaster && d.spell != 24) return;                  // 몬스터 빔은 전기 광선뿐(옮긴 것)
    uint16_t n2 = UnitNetCode(d.target);
    switch (d.spell) {
        case 43:
            for (const auto& sub : d.sub108) NetStartRaySpell(sub, out);
            return;
        case 7:  typ = 3; break;
        case 9:  typ = 2; break;
        case 22: typ = 5; break;
        case 24: typ = 4; break;
        case 59: typ = 1; val = c->second.facingPrev; break;
        case 35:                                             // 큰 치유 — 받는 쪽이 앞 · 자기에게면 안 보냄
            if (!d.target.addr.empty() && d.target.addr == d.caster) return;
            typ = 6;
            std::swap(n1, n2);
            break;
        default: return;                                     // 빔 없는 주문
    }
    if (d.target.none() || n2 == 0) return;                  // 정답지도 `result = +48` 이 있어야 보낸다
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), typ, val, static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++fxSent_;                                             // 조각 D — FX 는 건수로 센다
}

// sub_52F220(`GAME4_2.c:11329`) GreaterHeal 만들기 — 시전자 둘레 400 에서 받을 이를 찾는다
//   글리프(+20)거나 시전자가 없으면: 찾아서 20 회복하고 **한 방으로 끝**(1) · 아니면 대상을 잡아 두고
//   **적이 아니면** 빔을 켜고 이어 간다(0) · 적이면 취소 · 못 찾으면 글 "GreaterHealNoTarget" 뒤 취소
int Server::DurGreaterHealCreate(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool noCaster = d.caster.empty() || c == sessions_.end() || !c->second.inGame;
    if (noCaster || d.flag20) {
        if (noCaster && !d.flag20) return 1;
        const Target found = SpellFlySearchTarget(true, d.pos2X, d.pos2Y, d.posX, d.posY, std::string(), std::string(),
                                                  SpellFlags(d.spell), 400.0f, nullptr, nullptr);
        if (!found.none()) UnitAdjustHP(found, 20, out);
        return 1;                                            // 글리프는 한 방
    }
    Session& caster = c->second;
    const Target found = SpellFlySearchTarget(true, d.pos2X, d.pos2Y, static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                              d.caster, d.caster, SpellFlags(d.spell), 400.0f, &caster, &caster.buffs);
    d.target = found;
    if (found.none()) {
        SendPriMsg(caster, "ExecDur.c:GreaterHealNoTarget", out);
        return 1;
    }
    if (TargetIsEnemy(caster, found)) return 1;              // 적은 못 고친다
    NetStartRaySpell(d, out);                                // 치료 광선 켜기
    return 0;
}

// sub_52F2E0(`GAME4_2.c:11376`) GreaterHeal 틱 — 대상 체력을 GreaterHealRate(레벨) × 직업배수 만큼(소수 이월) 채우고 **시전자 마나 1**
//   끊김: 대상 없음·죽음 · 시전자 달리기(8) · **못 닿음**(CanInteract) · 시전자 마나 0 · 시전자가 **지난 1틱 안에 맞음** · 대상 체력 가득
int Server::DurGreaterHealTick(DurSpell& d, std::vector<OutPacket>& out) {
    if (!BuffsOf(d.target)) return 1;
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) return 1;   // DEAD|DESTROYED
    }
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                       // 달리면 채널이 끊긴다
    if (!CanInteract(caster, d.target)) return 1;            // 안 보이거나 벽 뒤
    if (caster.mana == 0) return 1;                          // 시전자 마나 바닥
    if (TargetJustDamaged(Target{ d.caster, -1 })) return 1; // 시전자가 방금 맞았다
    if (TargetMaxHealth(d.target) == TargetHealth(d.target)) return 1;   // 다 나았다
    const float v = d.field72 + GreaterHealRate(d.level) * ClassHealthMult(caster.charClass);
    const int heal = static_cast<int>(v);
    d.field72 = v - static_cast<float>(heal);                // 소수 이월
    UnitAdjustHP(d.target, heal, out);
    PlayerManaSub(caster, 1);
    return 0;
}

// sub_52E610 + sub_52E660(`GAME4_2.c:10776`·10789) — 시전자 둘레 ManaDrainRange 에서 **빨 수 있는 것**을 고른다
//   점수 = 무게 × 거리² 이고 **작은 쪽이 이긴다**. 사람은 무게 0.5 라 같은 거리면 마나 기둥(1.0)보다 **먼저 뽑힌다**
//   거를 것: 자기 자신 · 죽음/지워짐 · 마나 0 · **적이 아님** · 같은 팀 · 관전 중 · 벽 뒤(선 추적 5)
//   ⚠ 아직 안 옮긴 갈래 — 마나 기둥(클래스 0x400000·SUBCLASS 0x18, 무게 1.0)은 그 마나 웅덩이를 우리가 안 들고 있다(맵 오브젝트 묶음) ·
//     소환수(+1440 & 0x20 마나 가진 몬스터)도 우리 소환수엔 마나가 없다(크리처 묶음)
Target Server::DrainSearch(const Session& caster) const {
    const float range = static_cast<float>(BalanceFloat("ManaDrainRange"));
    const float range2 = range * range;
    float best = 1e+30f;
    Target found;
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || &o == &caster) continue;
        if (o.respawnAtFrame != 0) continue;                 // DEAD|DESTROYED(0x8020)
        if (o.observer) continue;                            // +3680 & 1
        if (o.mana == 0) continue;                           // sub_52E7C0 — 빨 마나가 있어야
        if (!IsEnemyTo(caster, o)) continue;                 // 적만 · 같은 팀 제외
        const float dx = static_cast<float>(caster.posX - o.posX);
        const float dy = static_cast<float>(caster.posY - o.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 > range2) continue;
        const float score = 0.5f * d2;                       // 사람 무게 0.5
        if (score >= best) continue;
        if (!MapTraceRay(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                         static_cast<float>(o.posX), static_cast<float>(o.posY), 0x5)) continue;   // 벽 뒤는 못 빤다
        best = score;
        found = Target{ o.addr, -1 };
    }
    return found;
}

// sub_52E450(`GAME4_2.c:10676`) — 실제로 빨아 옮긴다. 빨았으면 1
//   시전자가 사람이고 **마나가 이미 가득이면 0** · 사람 대상은 min(대상 마나, 양) 만큼 · 시전자에게 그만큼 더한다
//   ⚠ 퀘스트(4096)면 직업 마나 배수를 곱해 더한다 — 아레나는 그대로
int Server::DrainApply(Session& caster, const Target& t, int amount) {
    if (caster.mana >= caster.maxMana) return 0;
    if (t.addr.empty()) return 0;                            // 마나 기둥·소환수 갈래는 아직(위 주석)
    auto it = sessions_.find(t.addr);
    if (it == sessions_.end() || !it->second.inGame) return 0;
    Session& tgt = it->second;
    const int took = (static_cast<int>(tgt.mana) <= amount) ? static_cast<int>(tgt.mana) : amount;
    if (took <= 0) return 0;
    PlayerManaSub(tgt, took);
    PlayerManaAdd(caster, took);
    return 1;
}

// nox_xxx_spellDrainMana_52E210(`GAME4_2.c:10568`) — **틱마다 대상을 새로 찾아** 마나를 빨고 빔을 옮겨 단다
//   끊김: 시전자 달리기(8) · 시전자 마나 가득 · 시전자가 건 자리에서 5 넘게 움직임 · 시전자가 지난 1틱 안에 맞음 · 빨 대상이 없음
//   글리프(+20)면 찾은 이의 마나 50 을 깎고 한 방으로 끝
int Server::DurDrainMana(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool haveCaster = !d.caster.empty() && c != sessions_.end() && c->second.inGame;
    if (haveCaster && c->second.buffs.Has(8)) return 1;      // 달리면 끊긴다
    if (!haveCaster && !d.flag20) return 1;
    if (!haveCaster) return 1;                               // 글리프 갈래는 시전자 없이도 도는데, 우리 DurNew 는 시전자를 요구한다(4-e 에서)
    Session& caster = c->second;
    if (d.flag20) {                                          // 글리프 — 한 방
        const Target g = DrainSearch(caster);
        if (!g.addr.empty()) {
            auto gi = sessions_.find(g.addr);
            if (gi != sessions_.end()) PlayerManaSub(gi->second, 50);
        }
        return 1;
    }
    if (caster.mana >= caster.maxMana) return 1;             // 더 담을 데가 없다
    if (TargetMovedFrom(Target{ d.caster, -1 }, d.posX, d.posY)) return 1;   // 시전자가 자리를 떴다
    if (TargetJustDamaged(Target{ d.caster, -1 })) return 1; // 맞으면 끊긴다
    const Target found = DrainSearch(caster);
    d.target = found;
    if (found.none()) {                                      // 빨 것이 없다 — 켜 둔 빔이 있으면 끈다
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        return 1;
    }
    const float v = d.field72 + static_cast<float>(BalanceFloatInd("ManaDrainCoeff", d.level - 1));
    const int take = static_cast<int>(v);
    d.field72 = v - static_cast<float>(take);                // 소수 이월
    if (!(d.field36.addr == found.addr && d.field36.pet == found.pet)) {     // 대상이 바뀌었다 — 빔을 옮겨 단다
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    DrainApply(caster, found, take);                         // ⚠ 빨린 소리 230/229 는 소리 묶음(반 초마다)
    d.field36 = found;
    return 0;
}

// ── 묶음 4-d 지속 공격 — 에너지볼트(24) ────────────────────────────────
//
// spellEnergyBoltSetTarget_52EC60(`GAME4_2.c:10981`) — 둘레 안에서 **가장 가까운** 하나를 고른다.
//   거른다: 죽음·부서짐(0x8020) · 자기 · **적이 아닌 것** · **앞쪽이 아닌 것**(TestTwoPointsAndDirection) ·
//   상호작용 못 하는 것(CanInteract = 볼 수 있고 벽에 안 가림).
Target Server::EnergyBoltSearch(const Session& caster, float range) const {
    const float range2 = range * range;
    float best = range2;
    Target found;
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || &o == &caster) continue;
        if (o.respawnAtFrame != 0) continue;                 // DEAD|DESTROYED
        if (!IsEnemyTo(caster, o)) continue;
        const float ox = static_cast<float>(o.posX), oy = static_cast<float>(o.posY);
        if (!(TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                        caster.facingPrev, ox, oy) & 1)) continue;   // 앞쪽만
        if (!CanInteract(caster, Target{ o.addr, -1 })) continue;
        const float dx = ox - static_cast<float>(caster.posX);
        const float dy = oy - static_cast<float>(caster.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        best = d2;
        found = Target{ o.addr, -1 };
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& c = pets_[k];
        if (c.health == 0) continue;
        if (!OwnersEnemy(caster.addr, false, c.owner, c.owner.empty())) continue;
        const float ox = static_cast<float>(c.x), oy = static_cast<float>(c.y);
        if (!(TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                        caster.facingPrev, ox, oy) & 1)) continue;
        const Target t{ std::string(), static_cast<int>(k) };
        if (!CanInteract(caster, t)) continue;
        const float dx = ox - static_cast<float>(caster.posX);
        const float dy = oy - static_cast<float>(caster.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        best = d2;
        found = t;
    }
    return found;
}

// spellEnergyBoltStop_52E820(`GAME4_2.c:10630`) — 켤 때 **연쇄 번개(43)를 끄고** 점 효과 130 을 낸다. 0 = 살려 둔다
int Server::DurEnergyBoltCreate(DurSpell& d, std::vector<OutPacket>& out) {
    if (!d.caster.empty()) DurCancelFor(43, d.caster, out);
    else if (d.casterPet != 0) DurCancelForPet(43, d.casterPet, out);   // ★ 09-29 B7 — 몬스터 시전자
    return 0;
}

// spellEnergyBoltTick_52E850(`GAME4_2.c:10639`) — 매 틱
//   끊기는 자리: 시전자가 **달리기(8)** · 시전자가 없음 · 만든 뒤 2틱 넘겨 **맞음** · **마나 0**
//   고르기: 지금 대상이 아직 앞쪽·사거리 안·볼 수 있으면 그대로, 아니면 둘레에서 다시 고른다
//   피해: `EnergyBoltDamage[세기-1]` + 소수 이월 · 종류 **17(공중 전기)** · 대상이 바뀌면 옛 빔을 끄고 새 빔을 켠다
//   시전자는 자세 10 · **틱마다 마나 1**
int Server::DurEnergyBoltTick(DurSpell& d, std::vector<OutPacket>& out) {
    // ★ 09-29 B7 — 몬스터 시전자(`52E850` 의 +16 이 몬스터인 갈래 · `GAME4_2.c:10911~10997`)
    //   달리기(8)면 끊김 · **시전 자리(+28)에서 5 넘게 움직였으면 끊김**(`sub_4FEA70` — 몬스터만) · 만든 뒤 2틱 넘겨 맞았으면 끊김 ·
    //   지금 대상이 살았고·앞쪽·사거리 안·보이면 그대로 · 아니면 둘레에서 다시 · 없으면 빔만 끄고 이어 감 ·
    //   피해 = `EnergyBoltDamage[세기-1]` + 이월 · 종류 17 · 때린 것 = 그 몬스터 · 마나·자세는 사람만(몬스터는 없음)
    //   ⚠ 1/3초마다 소리 32(시전자·대상)·`LightningSearchTime`·죽은 대상 점 효과 130 은 사람 쪽에도 아직 없음(미결)
    if (d.casterPet != 0) {
        const int ki = PetIndexByNet(d.casterPet);
        if (ki < 0) return 1;
        const Creature& k = pets_[static_cast<size_t>(ki)];
        if (k.buffs.Has(8)) return 1;
        if (std::fabs(d.posX - static_cast<float>(k.x)) >= 5.0f || std::fabs(d.posY - static_cast<float>(k.y)) >= 5.0f) {
            ++castStat_[static_cast<size_t>(CastStat::BeamMoved)];
            return 1;
        }
        const Target self{ std::string(), ki };
        if (frame_ - d.frame60 > 2 && TargetJustDamaged(self)) return 1;
        const float range = static_cast<float>(BalanceFloat("LightningRange"));
        const float kx = static_cast<float>(k.x), ky = static_cast<float>(k.y);
        bool keep = false;
        if (!d.target.none()) {
            float tx = 0, ty = 0;
            if (TargetXY(d.target, tx, ty)) {
                const float dx = tx - kx, dy = ty - ky;
                keep = TargetHealth(d.target) > 0 && (TestTwoPointsAndDirection(kx, ky, k.facing, tx, ty) & 1) != 0 &&
                       std::sqrt(dx * dx + dy * dy) <= range && CanInteractFrom(k, d.target);
            }
        }
        if (!keep) d.target = EnergyBoltSearchPet(k, range);
        if (d.target.none()) {
            if (!d.field36.none()) { NetStopRaySpell(d, d.field36, out); d.field36 = Target{}; }
            return 0;
        }
        const float v = d.field72 + static_cast<float>(BalanceFloatInd("EnergyBoltDamage", d.level - 1));
        const int dmg = static_cast<int>(v);
        d.field72 = v - static_cast<float>(dmg);
        if (!(d.field36.addr == d.target.addr && d.field36.pet == d.target.pet)) {
            if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
            NetStartRaySpell(d, out);
        }
        AttackSrc src;
        src.x = kx; src.y = ky;
        src.byMonsterType = k.type;
        src.byPet = ki;
        src.rootIsMonster = k.owner.empty();
        const std::string kOwner = k.owner;
        const HitterRef hitter = HitterMonster(k);
        const Target tgt = d.target;
        HurtTarget(tgt, dmg, out, kOwner, 17, &src, hitter);  // `k` 는 여기서부터 안 쓴다(목록이 당겨질 수 있다)
        d.field36 = tgt;
        ++castStat_[static_cast<size_t>(CastStat::BeamHit)];
        return 0;
    }
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                       // 달리면 끊긴다
    const float range = static_cast<float>(BalanceFloat("LightningRange"));
    if (frame_ - d.frame60 > 2 && TargetJustDamaged(Target{ d.caster, -1 })) return 1;
    // 지금 대상을 지킬 수 있나
    bool keep = false;
    if (!d.target.none()) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty)) {
            const float dx = tx - static_cast<float>(caster.posX), dy = ty - static_cast<float>(caster.posY);
            keep = (TargetHealth(d.target) > 0)
                && ((TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                               caster.facingPrev, tx, ty) & 1) != 0)
                && (std::sqrt(dx * dx + dy * dy) <= range)
                && CanInteract(caster, d.target);
        }
    }
    if (!keep) d.target = EnergyBoltSearch(caster, range);
    if (d.target.none()) {                                   // 맞힐 것이 없다 — 빔만 끄고 살려 둔다
        if (!d.field36.none()) { NetStopRaySpell(d, d.field36, out); d.field36 = Target{}; }
        return 0;
    }
    const float v = d.field72 + static_cast<float>(BalanceFloatInd("EnergyBoltDamage", d.level - 1));
    const int dmg = static_cast<int>(v);
    d.field72 = v - static_cast<float>(dmg);                 // 소수 이월
    if (!(d.field36.addr == d.target.addr && d.field36.pet == d.target.pet)) {
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    // ★ 4-c3b1 — 때린 것 = 시전자(a2 · a3 = 0 · `GAME4_2.c:10990`)
    HurtTarget(d.target, dmg, out, d.caster, 17, nullptr, HitterByAddr(d.caster));   // 종류 17 = 공중 전기(방어를 안 탄다)
    d.field36 = d.target;
    PlayerSetState(caster, 10, frame_);                      // 시전 자세
    PlayerManaSub(caster, 1);
    // ★ 정답지는 깎은 **뒤에 「이전 마나」**(`unitGetOldMana_4EEC80` = +6)를 본다 —
    //   즉 **이미 0 이던 틱**에 끊긴다(그 틱의 피해는 이미 들어간 뒤다).
    if (caster.manaPrev == 0) return 1;
    return 0;
}

// ── 묶음 4-d(2) 연쇄 번개(43) ──────────────────────────────────────────
//
// lightningCanAttackCheck_52FF10(`GAME4_3.c:389`) — `from` 둘레에서 **가장 가까운** 하나.
//   거른다: 주인의 적이 아닌 것 · 죽음·부서짐 · `from` 자신 · 주인 자신 · **이미 고른 것** ·
//   상호작용 못 하는 것(CanInteract). ⚠ 에너지볼트와 달리 **앞쪽 검사가 없다**.
Target Server::LightningSearch(const Session& owner, const Target& from, float range,
                               const std::vector<Target>& taken) const {
    float fx = 0, fy = 0;
    if (!TargetXY(from, fx, fy)) return Target{};
    const float range2 = range * range;
    float best = range2;
    Target found;
    auto already = [&](const Target& t) {
        for (const auto& k : taken) if (k.addr == t.addr && k.pet == t.pet) return true;
        return false;
    };
    auto consider = [&](const Target& t, float tx, float ty) {
        if (t.addr == from.addr && t.pet == from.pet) return;
        if (!t.addr.empty() && t.addr == owner.addr) return;
        if (already(t)) return;
        if (!CanInteract(owner, t)) return;
        const float dx = tx - fx, dy = ty - fy;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) return;
        best = d2;
        found = t;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;
        if (!IsEnemyTo(owner, o)) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& cr = pets_[k];
        if (cr.health == 0) continue;
        if (!OwnersEnemy(owner.addr, false, cr.owner, cr.owner.empty())) continue;
        consider(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(cr.x), static_cast<float>(cr.y));
    }
    return found;
}

// onStartLightning_52F820(`GAME4_2.c:11660`) — 켤 때 **에너지볼트(24)를 끄고** 소수 이월을 0 으로. 0 = 살려 둔다
//   ★ 묶음 5-g — **든 물건이 충전 지팡이면 그것을 적어 둔다.** 틱마다 마나 대신 그 충전을 쓴다.
//     · **글리프가 건 것(`+20`)이면 안 본다** — 글리프에는 손이 없다
//     · 정답지는 오브젝트 클래스 비트 `0x40000` 을 본다(플라스마의 `0x4000000` 과 **다른 갈래**다).
//       우리는 그 비트를 아직 안 담아 **쓸 때 부르는 함수**(`WandCastUse`)로 갈음한다
//     · ⚠ 플라스마와 달리 **완드가 없어도 취소되지 않는다** — 그냥 마나를 쓴다
int Server::DurChainLightningCreate(DurSpell& d, std::vector<OutPacket>& out) {
    d.field72 = 0.0f;
    d.field80 = 0;
    if (!d.caster.empty()) DurCancelFor(24, d.caster, out);
    if (!d.flag20) {
        auto c = sessions_.find(d.caster);
        if (c != sessions_.end() && c->second.inGame) {
            const int wi = HeldWandIndex(c->second);
            if (wi >= 0) d.field80 = static_cast<uint32_t>(wi + 1);   // 0 은 「없음」이라 +1 해 담는다
        }
    }
    return 0;
}

// onFrameLightning_52F8A0(`GAME4_3.c:71`) — 매 틱
//   끊기는 자리: **달리기(8)** · **이전 마나 0** · 만든 뒤 2틱 넘겨 **맞음**
//   사슬: 세기가 곧 **가지 수**(표 `0x587000+260380` 이 1~5 를 그대로 돌려준다).
//     ① 첫 대상 = 시전자 둘레 `LightningRange`(240) 안 가장 가까운 적
//     ② 세기>1 : 첫 대상 둘레 **×0.95** 에서 하나 더 · ③ 세기>2 : 첫 대상 둘레 **×0.90** 에서 하나 더
//     ④ 세기>3 : **둘째**(②) 둘레 ×0.85 · ⑤ 세기>4 : **셋째**(③) 둘레 ×0.80
//   빔 조각: (시전자→①) · (①→②) · (①→③) · (②나 ③→④) · (③이나 ②→⑤). 조각마다 `9e` 갈래 3
//   피해: `LightningDamage`(아레나 1.5) + 소수 이월 · 종류 **17** · 조각마다 **같은 값**
int Server::DurChainLightningTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                        // 달리면 끊긴다
    if (caster.manaPrev == 0) return 1;                       // unitGetOldMana — 이미 0 이면 시작도 못 한다
    if (frame_ - d.frame60 > 2 && TargetJustDamaged(Target{ d.caster, -1 })) return 1;
    const float range = static_cast<float>(BalanceFloat("LightningRange"));

    // 지난 틱의 조각을 옆으로 밀어 둔다(+104) — 새 조각(+108)과 대조해 빔을 갈아 단다
    std::vector<DurSpell> prev;
    prev.swap(d.sub108);

    const int lvl = (d.level < 1) ? 1 : (d.level > 5 ? 5 : d.level);
    std::vector<Target> t;                                    // 고른 차례대로
    t.push_back(LightningSearch(caster, Target{ d.caster, -1 }, range, t));
    if (t[0].none()) {                                        // 맞힐 것이 없다 — 옛 빔만 끄고 살려 둔다
        for (const auto& sub : prev) if (!sub.target.none()) NetStopRaySpell(sub, sub.target, out);
        return 0;
    }
    Target t1, t2, t3, t4;
    if (lvl > 1) { t1 = LightningSearch(caster, t[0], range * 0.94999999f, t); if (!t1.none()) t.push_back(t1); }
    if (lvl > 2) { t2 = LightningSearch(caster, t[0], range * 0.89999998f, t); if (!t2.none()) t.push_back(t2); }
    if (lvl > 3 && !t1.none()) { t3 = LightningSearch(caster, t1, range * 0.85000002f, t); if (!t3.none()) t.push_back(t3); }
    if (lvl > 4 && !t2.none()) { t4 = LightningSearch(caster, t2, range * 0.80000001f, t); if (!t4.none()) t.push_back(t4); }

    // 새 조각 목록 — 정답지 `NewLightningSub(원본, 시작, 끝)` 순서 그대로
    auto addLink = [&](const Target& from, const Target& to) {
        DurSpell sub;
        sub.spell = 7;                                        // SPELL_CHAIN_LIGHTNING_BOLT — 빔 갈래 3
        sub.level = d.level;
        sub.caster = d.caster;                                // 알림을 보낼 때 쓸 사람
        sub.fromUnit = from;
        sub.target = to;
        d.sub108.push_back(sub);
    };
    addLink(Target{ d.caster, -1 }, t[0]);
    if (lvl > 1 && !t1.none()) addLink(t[0], t1);
    if (lvl > 2 && !t2.none()) addLink(t[0], t2);
    if (lvl > 3 && !t3.none()) addLink(t1.none() ? t2 : t1, t3);
    if (lvl > 4 && !t4.none()) addLink(t2.none() ? t1 : t2, t4);

    const float v = d.field72 + static_cast<float>(BalanceFloat("LightningDamage"));
    const int dmg = static_cast<int>(v);
    d.field72 = v - static_cast<float>(dmg);                  // 소수 이월

    // 자리마다 옛 조각과 견줘 **끝이나 시작이 달라졌으면** 옛 빔을 끄고 새 빔을 켠다
    for (size_t i = 0; i < d.sub108.size(); ++i) {
        DurSpell& sub = d.sub108[i];
        if (i < prev.size()) {
            const DurSpell& old = prev[i];
            const bool same = (old.target.addr == sub.target.addr && old.target.pet == sub.target.pet)
                           && (old.fromUnit.addr == sub.fromUnit.addr && old.fromUnit.pet == sub.fromUnit.pet);
            if (!same) {
                if (!old.target.none()) NetStopRaySpell(old, old.target, out);
                NetStartRaySpell(sub, out);
            }
        } else {
            NetStartRaySpell(sub, out);
        }
        if (dmg > 0) HurtTarget(sub.target, dmg, out, d.caster, 17, nullptr, HitterByAddr(d.caster));   // ★ 4-c3b1 시전자(`GAME4_3.c:333` a3 = 0)
    }
    for (size_t i = d.sub108.size(); i < prev.size(); ++i)     // 남은 옛 빔은 끈다
        if (!prev[i].target.none()) NetStopRaySpell(prev[i], prev[i].target, out);

    // ★ 묶음 5-g — **완드를 들었으면 마나 대신 충전을 쓴다**(`GAME4_3.c:346~372`).
    //   자세도 다르다 — 완드면 **22(집중)**, 마나면 **10(시전)**.
    if (d.field80 != 0) {
        const size_t wi = static_cast<size_t>(d.field80 - 1);
        if (wi >= caster.inventory.size()) return 1;
        Session::Carried& w = caster.inventory[wi];
        if (w.chargesMax != 0) {
            if (w.charges == 0) return 1;
            --w.charges;
            PlayerSetState(caster, 22, frame_);
            SendCharges(caster, w, out);
            if (w.charges == 0) return 1;
        } else {
            PlayerSetState(caster, 22, frame_);
        }
        return 0;                                             // 마나는 안 쓴다
    }
    PlayerSetState(caster, 10, frame_);
    PlayerManaSub(caster, 1);
    if (caster.manaPrev == 0) return 1;
    return 0;
}

// castDeathRay(`spell_death_ray.go:19`) — 커서 자리에 **한 방**으로 터진다(지속 주문이 아니다).
//   · 시전자→커서 **선 추적(9)** 이 막히면 글(0·2)을 내고 0
//   · 둘레 피해 : 바깥 `DeathRayOutRadius`(15) · 안쪽 `DeathRayInRadius`(10) ·
//     피해 `DeathRayDamage`(아레나 100) · 종류 **16(죽음광선)**
//   · 그림 : `94 <시전자 x u16> <y u16> <커서 x u16> <y u16>` **9바이트 선**(netSendRayFx_5232F0)
//   ⚠ 아직 안 옮긴 것 — 그을음(sMakeScorch) · 소리
int Server::CastDeathRay(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 9)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    const int dmg = static_cast<int>(BalanceFloat("DeathRayDamage"));
    const float rout = static_cast<float>(BalanceFloat("DeathRayOutRadius"));
    const float rin = static_cast<float>(BalanceFloat("DeathRayInRadius"));
    AttackSrc ws;                                            // ★ 09-27 S1 — v9 = who = 시전자 사람(+72 지난 자리) · 발사체 아님
    ws.x = static_cast<float>(caster.prevX); ws.y = static_cast<float>(caster.prevY);
    DamageUnitsAround(arg.x, arg.y, rout, rin, dmg, 16, caster.addr, Target{}, out, HitterByAddr(caster.addr), &ws,
                      WallHitterOfAddr(caster.addr));   // ★ 4-c3b1 who = 시전자(`spell_death_ray.go:35`) · ★ 09-29 Z1-A 벽 피해 who 도 시전자
    const uint16_t x1 = static_cast<uint16_t>(static_cast<int>(cx)), y1 = static_cast<uint16_t>(static_cast<int>(cy));
    const uint16_t x2 = static_cast<uint16_t>(static_cast<int>(arg.x)), y2 = static_cast<uint16_t>(static_cast<int>(arg.y));
    const uint8_t fx[9] = { static_cast<uint8_t>(Op::FX_DEATH_RAY),
                            static_cast<uint8_t>(x1), static_cast<uint8_t>(x1 >> 8),
                            static_cast<uint8_t>(y1), static_cast<uint8_t>(y1 >> 8),
                            static_cast<uint8_t>(x2), static_cast<uint8_t>(x2 >> 8),
                            static_cast<uint8_t>(y2), static_cast<uint8_t>(y2 >> 8) };
    SendFxAllCli(arg.x, arg.y, fx, sizeof(fx), out);
    return 1;
}

// ── 묶음 4-d(4) 방패(Shield 51) ────────────────────────────────────────
//
// castShield1_52F5A0(`GAME4_2.c:11510`) — 대상에게 **마법 방패(버프 26)** 를 걸고 **막아 낼 양**을 담아 둔다.
//   · 대상이 없거나 죽음·부서짐이면, 또는 사람도 몬스터도 아니면 **취소**
//   · 길이 = `ShieldDuration[세기-1]` — ⚠ 실측이 **0, 0, 0, 0, 0** 이다(주석 처리된 옛 줄만 600~1800).
//     즉 **시간으로는 안 끝나고**, 막아 낼 양(`ShieldHealth` 20·35·50·75·100)을 다 쓰면 끝난다
int Server::DurShieldCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (d.target.none()) return 1;
    if (TargetHealth(d.target) <= 0) return 1;               // 죽음·부서짐
    if (!BuffsOf(d.target)) return 1;                        // 사람도 몬스터도 아니다
    const int dur = static_cast<int>(BalanceFloatInd("ShieldDuration", d.level - 1));
    ApplyEnchant(d.target, 26, dur, d.level);
    d.pool = static_cast<int>(BalanceFloatInd("ShieldHealth", d.level - 1));
    return 0;
}

// sub_52F650(`:11540`) — 대상이 없거나 죽었으면 끝낸다. 그 밖에는 이어 간다
int Server::DurShieldTick(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (d.target.none()) return 1;
    return (TargetHealth(d.target) <= 0) ? 1 : 0;
}

// sub_52F670(`:11553`) — 끝날 때 버프 26 을 끈다
void Server::DurShieldDestroy(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (!d.target.none()) BuffOff(d.target, 26);
}

// unitShield_52F690(`:11557`) — 그 대상에게 걸린 **방패 주문(51)** 을 찾아 주머니에서 뺀다.
//   찾는 주문이 없으면 버프만 끄고, 다 쓰면(또는 대상이 죽었으면) 주문을 취소한다.
void Server::ShieldDrain(const Target& t, int amount, std::vector<OutPacket>& out) {
    DurSpell* found = nullptr;
    for (auto& d : durSpells_) {
        if (d.spell != 51) continue;
        if (d.target.addr != t.addr || d.target.pet != t.pet) continue;
        found = &d;
        break;
    }
    if (!found) { BuffOff(t, 26); return; }
    if (TargetHealth(t) <= 0) { DurCancel(*found, out); return; }
    if (found->pool - amount > 0) found->pool -= amount;
    else DurCancel(*found, out);
}

// unitShieldReduceDamage_52F710(`:11588`) — 맞는 순간 끼어든다.
//   · 남은 체력이 **피해보다 크면** : 피해를 **반**으로(0 이면 1) 줄이고 그만큼 주머니에서 뺀다
//   · 죽을 만큼이면 : 체력을 **2** 로 두고 피해를 **0** 으로(= 아예 안 맞음) · 주머니를 통째로 비운다
//     ⚠ 단, **워리어가 방패를 올린 채 앞에서** 맞으면 피해를 **1** 로 남긴다(체력은 그대로 깎인다)
//   ⚠ 아직 안 옮긴 것 — 소리 131 · 방패 그림(netSendShieldFx_523670)
void Server::ShieldReduceDamage(const Target& t, int& dmg, uint8_t dtype, const AttackSrc* src, std::vector<OutPacket>& out) {
    (void)dtype;
    // 10-b PlaySound 131 ShieldRepelled — GAME4_2.c:11593 rings it first thing
    if (!t.addr.empty()) {
        auto si = sessions_.find(t.addr);
        if (si != sessions_.end())
            PlaySoundAt(131, static_cast<float>(si->second.posX), static_cast<float>(si->second.posY), 0, 0, t.addr);
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        const Creature& k = pets_[static_cast<size_t>(t.pet)];
        PlaySoundAt(131, static_cast<float>(k.x), static_cast<float>(k.y), 0, 0, std::string(), t);
    }
    const int hp = TargetHealth(t);
    if (hp > dmg) {
        int half = dmg / 2;
        if (half == 0) half = 1;
        dmg = half;
        ShieldDrain(t, dmg, out);
        return;
    }
    bool guarded = false;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it != sessions_.end() && it->second.charClass == CharClass::Warrior && it->second.moveState == 16 && src) {
            guarded = (TestTwoPointsAndDirection(static_cast<float>(it->second.posX), static_cast<float>(it->second.posY),
                                                 it->second.facingPrev, src->x, src->y) & 1) != 0;
        }
    }
    if (guarded) {
        dmg = 1;
    } else {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            if (it != sessions_.end()) it->second.health = 2;         // unitSetHP(2)
        } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
            pets_[static_cast<size_t>(t.pet)].health = 2;
        }
        dmg = 0;
    }
    ShieldDrain(t, 999999, out);
}

// ── 묶음 4-e(1) 점멸(Blink 4) ──────────────────────────────────────────
//
// teleportToMB_4E7190 — 그 자리로 옮긴다. 우리 서버는 물리 자리(posX)와 옛 자리칸(x)을 **둘 다** 맞춰야 한다.
void Server::TeleportTarget(const Target& t, float x, float y, std::vector<OutPacket>* out) {
    sightForceFrame_ = frame_ + 1;   // ★ 09-29 B1 — 유닛 `SetPos` 는 다음 프레임에 모두 다시 훑게 한다(`object.go:719~721`)
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end()) return;
        Session& o = it->second;
        o.posX = x; o.posY = y;
        o.x = static_cast<int>(x); o.y = static_cast<int>(y);
        o.prevX = x; o.prevY = y;
        o.velX = o.velY = 0.0;                               // 옮기면 속도는 죽는다
        // ★★ 09-20 — **사람을 옮기면 `ea FORGET_DRAWABLES` 를 보내야 한다.**
        //   정답지 `SetPos` 의 플레이어 갈래(`object.go:722~731`) — 온라인이면 `ea <프레임>` 을 보내고
        //   `Sub4DE4D0` 으로 **「이 사람이 본 것」 표를 비운다**(그래야 새 자리 것들이 다시 소개된다).
        //   ⚠ 안 보내면 **화면에 제 캐릭터가 안 그려진다**(09-20 `--debug-warp` 실측 — 방은 그려지는데
        //     사람만 없었다). 부활은 이미 부르고 있었는데 순간이동 갈래가 빠져 있었다.
        //   ⇒ 블링크·표식 이동도 같은 결함이었다(주문 121~125 · 아래 호출자 셋)
        if (out) ForgetDrawables(o, *out);
        return;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return;
    pets_[static_cast<size_t>(t.pet)].x = x;
    pets_[static_cast<size_t>(t.pet)].y = y;
    pets_[static_cast<size_t>(t.pet)].prevX = x;   // ★ 09-29 B1 — `SetPos` 는 PrevPos 도 새 자리(`object.go:707`)
    pets_[static_cast<size_t>(t.pet)].prevY = y;
}

// spellBlink2_530310(`GAME4_3.c:600`) — 켤 때 **언제 터질지**만 정한다.
//   협동(2048)이면 `TeleportDelay[세기-1]`(25·20·15·10·5) 틱 뒤, 아니면 **다음 틱**.
//   ⚠ 팀 모드(4096)에서 **글리프가 건 것**이면 아예 안 걸린다.
int Server::DurBlinkCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && d.flag20) return 1;                        // 팀 모드 + 글리프 → 취소
    if (quest) d.frame68 = frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1));
    else       d.frame68 = frame_ + 1;
    return 0;
}

// spellBlink1_530380(`GAME4_3.c:620`) — 정해 둔 프레임이 되면 **한 번** 옮기고 끝난다.
//   · 대상이 없거나 죽었으면 취소 · 아직이면 그냥 이어 간다
//   · **닻(버프 14)** 이 걸려 있으면 소리 231 을 내고 못 옮긴 채 끝난다
//   · 자리 = 맵 **길목** 가운데 무작위 하나(`waypoint_579F00`) · 없으면 **시작 지점**
//   · 옮기기 전후로 점 효과 137
int Server::DurBlinkTick(DurSpell& d, std::vector<OutPacket>& out) {
    if (d.target.none()) return 1;
    if (TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;                   // 아직 때가 아니다
    if (const Buffs* b = BuffsOf(d.target)) {
        if (b->Has(14)) return 1;                            // 닻 — 소리 231(10)
    }
    float tx = 0, ty = 0;
    if (!waypoints_.empty()) {
        const auto& w = waypoints_[NextRandom(static_cast<uint32_t>(waypoints_.size()))];
        tx = w.x; ty = w.y;
    } else if (!playerStarts_.empty()) {                     // mapFindPlayerStart_4F7AB0
        const auto& ps = playerStarts_[NextRandom(static_cast<uint32_t>(playerStarts_.size()))];
        tx = ps.first; ty = ps.second;
    } else {
        return 1;                                            // 갈 데가 없다
    }
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    auto pointFx = [&](float px, float py) {              // netSendPointFx_522FF0
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_TELEPORT),
                                 static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                 static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    };
    pointFx(ox, oy);
    TeleportTarget(d.target, tx, ty, &out);
    pointFx(tx, ty);
    return 1;                                                // 한 번 옮기면 끝
}

// ── 묶음 4-e(2) 표식·순간이동 ─────────────────────────────────────────
//
// 옮길 때 점 효과 137 을 **떠난 자리와 닿은 자리** 두 번 보낸다(투명이면 안 보낸다 — 그건 묶음 4-a4 쪽).
void Server::SendTeleportFx(float ox, float oy, float nx, float ny, std::vector<OutPacket>& out) {
    auto one = [&](float px, float py) {
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_TELEPORT),
                                 static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                 static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    };
    one(ox, oy);
    one(nx, ny);
}

// sub_52CBD0(`GAME4_2.c:9800`) — 표식 걸기(46~49). 칸 = **주문 − 46**.
//   자리 = 글리프에 건 것이면 그 글리프 자리, 아니면 **시전자 자리**.
//   ⚠ 정답지는 `TeleportGlyph1~4` 오브젝트를 세워 두고 이미 있으면 **옮긴다** — 우리는 자리만 담는다(그림은 글리프 조각).
int Server::CastMark(uint8_t spell, Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    (void)arg;
    (void)out;
    if (spell < 46 || spell > 49) return 0;
    const int slot = spell - 46;
    caster.markSet[slot] = true;
    caster.markX[slot] = static_cast<float>(caster.posX);
    caster.markY[slot] = static_cast<float>(caster.posY);
    return 1;
}

// sub_5305D0(`GAME4_3.c:...`) — 표식으로 가기(122~125) 켤 때.
//   그 칸에 표식이 **없으면 아예 안 걸린다**. 있으면 언제 터질지만 잡는다(협동은 TeleportDelay, 아니면 다음 틱).
int Server::DurTeleMarkCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end()) return 1;
    const int slot = static_cast<int>(d.spell) - 122;
    if (slot < 0 || slot > 3) return 1;
    if (!c->second.markSet[slot]) return 1;                  // 표식이 없다
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    d.frame68 = quest ? (frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1)))
                      : (frame_ + 1);
    return 0;
}

// sub_530650 — 그 프레임이 되면 **한 번** 옮기고 끝난다. 닻(14)이면 못 옮긴 채 끝난다
int Server::DurTeleMarkTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (d.target.none() || TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;
    if (const Buffs* b = BuffsOf(d.target)) { if (b->Has(14)) return 1; }   // 닻
    const int slot = static_cast<int>(d.spell) - 122;
    if (slot < 0 || slot > 3 || !c->second.markSet[slot]) return 1;
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    TeleportTarget(d.target, c->second.markX[slot], c->second.markY[slot], &out);
    SendTeleportFx(ox, oy, c->second.markX[slot], c->second.markY[slot], out);
    return 1;
}

// castTele_530820 — 무작위 표식으로 가기(121) 켤 때. 때만 잡는다(표식 검사는 터질 때 한다)
int Server::DurTelePopCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    d.frame68 = quest ? (frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1)))
                      : (frame_ + 1);
    return 0;
}

// sub_530880 — 그 프레임이 되면 시전자의 표식 **네 칸 가운데 걸린 것 하나를 무작위로** 골라 옮긴다.
//   하나도 없으면 그냥 끝난다. 닻(14)이면 못 옮긴 채 끝난다
int Server::DurTelePopTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (d.target.none() || TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;
    if (const Buffs* b = BuffsOf(d.target)) { if (b->Has(14)) return 1; }
    int have = 0;
    for (int i = 0; i < 4; ++i) if (c->second.markSet[i]) ++have;
    if (have == 0) return 1;                                 // 걸린 표식이 없다
    int slot = 0;
    do { slot = static_cast<int>(NextRandom(4)); } while (!c->second.markSet[slot]);
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    TeleportTarget(d.target, c->second.markX[slot], c->second.markY[slot], &out);
    SendTeleportFx(ox, oy, c->second.markX[slot], c->second.markY[slot], out);
    return 1;
}

// ── 묶음 5-f 플라스마(59) ─────────────────────────────────────────────
//
// 지속 주문 셋 — `plasmaSmth_531580`(켤 때) · `plasmaShot_531600`(틱) · `sub_5319E0`(끌 때).
// dt = `PlasmaSearchTime`(기획자료).
//
// ★ **이 주문이 「아이템 묶음 대기」였던 까닭** — 켤 때 **완드를 들고 있나**를 본다.
//   사람이 완드 갈래 물건을 안 들고 있으면 **곧바로 취소**된다(`return 1`).
//   그리고 틱마다 그 완드의 **충전을 하나씩 먹는다** — 완드가 250 충전인 까닭이 이것이다.
//
// ⚠ 안 옮긴 갈래 — 헤큐바(`PlasmaDamageHecubah`)는 그 몬스터가 없어 못 옮겼다 ·
//   빔 그림(`netStartDurationRaySpell`)은 4-a3 틀의 것을 쓴다 · 소리 98 은 묶음 10.

// plasmaSmth_531580 — 켤 때
int Server::DurPlasmaCreate(DurSpell& d, std::vector<OutPacket>& out) {
    d.field80 = 0;                                           // +72 든 완드(우리는 아래 wandIdx 로 쓴다)
    d.field84 = 0;                                           // +76 뒤로 돌았을 때의 3초 유예
    d.target = Target{};                                     // +48
    {   // netSendPointFx_522FF0(131) — 화면 안 사람에게 점 효과
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(d.posX)), iy = static_cast<uint16_t>(static_cast<int>(d.posY));
        const uint8_t fx[5] = { 131, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(d.posX, d.posY, fx, sizeof(fx), out);
    }
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 0;  // 사람이 아니면 그냥 둔다(정답지도 0)
    // 든 것이 **완드 갈래**인가 — 아니면 취소한다. 이게 「완드가 있어야 도는 주문」의 정체다.
    const int wi = HeldWandIndex(c->second);
    if (wi < 0) return 1;
    d.field80 = static_cast<uint32_t>(wi + 1);               // 0 은 「없음」이라 +1 해 담는다
    return 0;
}

// sub_5319E0 — 끌 때. 정답지는 완드 자료의 「지금 쓰는 중」 비트를 내린다. 우리는 그 비트를 안 두어 할 일이 없다.
void Server::DurPlasmaDestroy(DurSpell& d, std::vector<OutPacket>& out) {
    (void)d; (void)out;
}

// plasmaShot_531600 — 매 틱
//   ① 대상이 아직 쓸 만한가 — 죽었거나 **400 밖**이거나 안 보이면 버린다
//      · 대상이 **뒤로 돌아가면** 곧바로 버리지 않고 **3초를 센다**(그 안에 다시 앞에 오면 유지)
//   ② 없으면 둘레 **400** 안에서 **앞쪽에 있는 가장 가까운 적**을 찾는다
//   ③ 있으면 **종류 14(플라스마)** 로 `PlasmaDamage` 만큼 때리고, 빔을 켜고, 자세 22(집중)
//      · 대상이 있는 동안 **끝 프레임을 계속 미룬다**(`+68 = 지금 + PlasmaSearchTime`)
//      · **완드 충전을 하나 먹는다.** 0 이 되면 끝난다
int Server::DurPlasmaTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    Session& s = c->second;
    if (s.respawnAtFrame != 0) return 1;

    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float kRange = 400.0f;

    // ① 들고 있던 대상을 다시 본다
    bool keep = false;
    if (!d.target.none()) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty) && TargetHealth(d.target) > 0) {
            // 뒤로 돌았나 — `& 2` 면 뒤다. 그러면 3초를 센다(앞이면 셈을 지운다)
            const uint8_t dir = TestTwoPointsAndDirection(px, py, s.facingPrev, tx, ty);
            if (dir & 2) {
                if (d.field84 == 0) d.field84 = 3 * kServerTickRate;
            } else {
                d.field84 = 0;
            }
            const float dx = tx - px, dy = ty - py;
            if (dx * dx + dy * dy <= kRange * kRange && CanInteract(s, d.target)) keep = true;
        }
        if (!keep) d.target = Target{};
    }
    // ② 없으면 찾는다 — 앞쪽 · 400 안 · 가장 가까운 적
    if (!keep) {
        d.target = PlasmaSearch(s, kRange);
        d.field84 = 0;
    }
    // 유예 셈 — 0 이 되면 대상을 버린다
    if (d.field84 != 0) {
        --d.field84;
        if (d.field84 == 0) d.target = Target{};
    }

    // ③ 대상이 있으면 쏜다
    if (d.target.none()) {
        if (!d.field36.none()) {
            NetStopRaySpell(d, d.field36, out);
            d.field36 = Target{};
        }
        return 0;
    }
    if (!(d.field36.addr == d.target.addr && d.field36.pet == d.target.pet)) {
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    const int dmg = static_cast<int>(BalanceFloat("PlasmaDamage"));
    HurtTarget(d.target, dmg, out, d.caster, /*dtype=*/14, nullptr, HitterByAddr(d.caster));   // 14 = PLASMA · ★ 4-c3b1 시전자(`GAME4_3.c:1472` a3 = 0)
    if (TargetHealth(d.target) <= 0) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty)) {
            const uint16_t ix = static_cast<uint16_t>(static_cast<int>(tx)), iy = static_cast<uint16_t>(static_cast<int>(ty));
            const uint8_t fx[5] = { 131, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                    static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
            SendFxAllCli(tx, ty, fx, sizeof(fx), out);
        }
    }
    d.field36 = d.target;
    PlayerSetState(s, 22, frame_);                           // CONCENTRATE
    // 대상이 있는 동안 끝을 미룬다 — 유예를 세는 중이면 안 미룬다(정답지 그대로)
    if (d.field84 == 0)
        d.frame68 = frame_ + static_cast<uint32_t>(BalanceFloat("PlasmaSearchTime"));

    // ④ 완드 충전 — 틱마다 하나
    if (d.field80 != 0) {
        const size_t wi = static_cast<size_t>(d.field80 - 1);
        if (wi >= s.inventory.size()) return 1;
        Session::Carried& w = s.inventory[wi];
        if (w.chargesMax != 0) {
            if (w.charges == 0) return 1;
            --w.charges;
            SendCharges(s, w, out);
            if (w.charges == 0) return 1;
        }
    }
    return 0;
}

// 든 것 가운데 **완드 갈래**의 자리(없으면 −1). 완드 = `USE = WandCastUse` 로 읽힌 것.
//   ⚠ 정답지는 오브젝트 클래스 비트 `0x4000000`(WAND)을 본다. 우리는 아직 그 비트를 안 담아
//     **쓸 때 부르는 함수**로 갈음한다 — 같은 집합이다(완드만 `WandCastUse` 를 쓴다).
int Server::HeldWandIndex(const Session& s) {
    for (size_t i = 0; i < s.inventory.size(); ++i)
        if (s.inventory[i].useFn == UseFn::WandCast) return static_cast<int>(i);
    return -1;
}

// sub_531920 — 둘레에서 고르는 잣대: **앞쪽**(`& 1`) · 볼 수 있고 · 가장 가까운 **적**.
Target Server::PlasmaSearch(const Session& s, float range) const {
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    float best = range * range;
    Target found;
    auto consider = [&](const Target& t, float tx, float ty) {
        if (!(TestTwoPointsAndDirection(px, py, s.facingPrev, tx, ty) & 1)) return;   // 앞쪽만
        if (!CanInteract(s, t)) return;
        const float dx = tx - px, dy = ty - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) return;
        best = d2;
        found = t;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0 || o.addr == s.addr) continue;
        if (!IsEnemyTo(s, o)) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& cr = pets_[k];
        if (cr.health == 0) continue;
        if (!OwnersEnemy(s.addr, false, cr.owner, cr.owner.empty())) continue;
        consider(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(cr.x), static_cast<float>(cr.y));
    }
    return found;
}

// ── 묶음 6-e(1) 길 찾기 격자 색인 ─────────────────────────────────────
//
// 정답지 `server/object_ai_path.go:16~224`. 길 찾기는 **벽 격자(256×256) 위에서** 돈다.
// 칸마다 깃발을 세워 두고 「여기를 지날 수 있나」를 묻는다.
//
// ⭕ 깃발(같은 파일 31~36행 + `sub50B8E0`) —
//     `0x1`   벽 — **땅으로 걷는 것**을 막는다
//     `0x4`   승강기 · `0x8` 승강기 통로 · `0x20` 나르는 것
//     `0x10`  구멍 — ★ **날아가는 것**을 막는다(땅으로 걷는 것은 지나간다)
//     `0x100` 오브젝트가 차지함
//     `0x200` 날아가는 것(오브젝트 깃발 `0x4000`)에만 걸리는 자리
//     `0x400` 갈래 비트 10 이 없는 것만 막는 자리
//
// ★ **세대(generation)** 로 묵은 자국을 가른다 — 색인을 다시 뜰 때 세대를 하나 올리고,
//   칸의 세대가 지금 세대와 다르면 **오브젝트 자국이 없는 것으로** 본다.
//   그래서 칸을 일일이 지우지 않고도 매번 새로 뜰 수 있다.
// ★ 색인은 **15틱에 한 번**만 다시 뜬다(`IndexObjects`).
//
// ⚠ 정답지 `CheckIndexFlags` 는 격자 밖을 **안 막는다**(널 포인터를 그냥 읽는다).
//   우리는 밖이면 **막힌 것으로** 본다 — 길 찾기가 맵 밖으로 새면 안 된다.

void Server::AIPathResetIndex() {
    aiIndexGen_ = 0;
    aiIndexFrame_ = 0;                                       // `Init`: lastFrame = 0
    aiIndexCalc_ = false;
    for (auto& n : aiIndex_) { n.gen = 0; n.flags = 0; }
}

Server::AIIndexNode* Server::AIPathIndexAt(int x, int y) {
    if (x < 0 || x >= kAIIndexSize || y < 0 || y >= kAIIndexSize) return nullptr;
    return &aiIndex_[static_cast<size_t>(y) * kAIIndexSize + static_cast<size_t>(x)];
}

uint16_t Server::AIPathFlagsAt(int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    return n ? n->flags : 0;
}

// CheckIndexFlags — **막혔나**. 날아가는 것은 **구멍(0x10)** 에, 땅을 걷는 것은 **벽(0x1)** 에 걸린다.
bool Server::AIPathBlockedFlags(bool airborne, int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    if (!n) return true;                                     // 격자 밖은 막힌 것으로 본다
    return airborne ? (n->flags & 0x10) != 0 : (n->flags & 0x1) != 0;
}

// sub50B8E0 — **오브젝트가 막고 있나**. ★ 칸의 **세대가 지금 세대와 다르면 묵은 자국**이라 0.
bool Server::AIPathBlockedByObj(bool airborne, uint32_t subclass, int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    if (!n) return true;
    if (n->gen != aiIndexGen_) return false;                 // 묵은 자국 — 없는 셈
    if (airborne) return (n->flags & 0x200) != 0;            // 날아가는 것은 이 자리만 본다
    if (n->flags & 0x100) return true;                       // 무엇이 차지하고 있다
    if ((subclass >> 10) & 1) return false;                  // 갈래 비트 10 은 이 자리를 지나간다
    return (n->flags & 0x400) != 0;
}

// Nox_xxx_pathfind_preCheckWalls2_50B8A0 — 둘 다 아니어야 지날 수 있다.
bool Server::AIPathCanPass(bool airborne, uint32_t subclass, int x, int y) {
    if (AIPathBlockedFlags(airborne, x, y)) return false;
    return !AIPathBlockedByObj(airborne, subclass, x, y);
}

// IndexObjects — **15틱에 한 번**만 다시 뜬다. 뜰 때 **세대를 하나 올린다**.
//   ⚠ 오브젝트를 칸에 찍는 `IndexObject`(정답지 100줄)는 아직 안 옮겼다 —
//     지금은 **장애물(움직이지 않는 것)만** 찍는다. 소환수·사람이 서로 길을 막는 갈래는 다음 조각.
void Server::AIPathIndexObjects() {
    // ⚠ 정답지 그대로 — **처음(둘 다 0)에도 안 돈다**(`0 - 0 = 0 < 15`).
    //   길 찾기를 처음 쓰는 자리에서 15틱이 지나야 처음 뜬다.
    if (frame_ - aiIndexFrame_ < 15) return;
    aiIndexFrame_ = frame_;
    ++aiIndexGen_;
    for (const auto& o : obstacles_) {
        const int gx = static_cast<int>(o.x / kWallCell);
        const int gy = static_cast<int>(o.y / kWallCell);
        AIIndexNode* n = AIPathIndexAt(gx, gy);
        if (!n) continue;
        n->gen = aiIndexGen_;
        n->flags |= 0x100;                                   // 무엇이 차지하고 있다
    }
    aiIndexCalc_ = true;
}

// MaybeIndexObjects — 이번 프레임에 이미 떴으면 건너뛴다.
void Server::AIPathMaybeIndexObjects() {
    if (aiIndexCalc_) return;
    AIPathIndexObjects();
}

// Sub_50B500 / Sub_50B510 — 「다시 떠야 한다」고 표시한다. 뒤엣것은 틱 관문까지 푼다.
void Server::AIPathInvalidate(bool alsoFrame) {
    aiIndexCalc_ = false;
    if (alsoFrame) aiIndexFrame_ = 0;
}

// 격자 깃발을 다시 굽는다.
//
// ★★ **2026-09-15 정정(6-e(2b·2))** — 여기서 **벽을 굽지 않는다.**
//   정답지 `Sub50AFA0`(`server/object_ai_path.go:512~596`)이 깃발 **0x1** 을 찍는 자리는
//   **오브젝트 모양**(클래스 0x400000 을 칸마다 눌러 보는 자리)뿐이고, **벽은 안 찍는다**.
//   벽은 **`sub_50B870`(칸의 벽 방향 번호 != −1)과 이음매 규칙**으로만 막는다.
//   전에는 벽을 0x1 에 구워 **먼저 통째로** 막아, 이음매 규칙이 아예 효과가 없었다.
//
// ⚠ 그래서 지금 **0x1 을 찍는 것은 아무것도 없다** — 오브젝트 모양 찍기(`IndexObject`, 정답지 100줄)를
//   아직 안 옮겼기 때문이다. 옮기면 여기가 아니라 그쪽이 찍는다.
void Server::AIPathBakeWalls() {
    for (int y = 0; y < kAIIndexSize; ++y)
        for (int x = 0; x < kAIIndexSize; ++x)
            if (AIIndexNode* n = AIPathIndexAt(x, y)) n->flags &= static_cast<uint16_t>(~0x1);
    AIPathBakeObstacles();   // 묶음 6-e(2b·3) — 오브젝트 모양이 0x1(·0x2)을 찍는다
    AIPathBakeWaypoints();   // 묶음 6-j(2) — 길목 칸(0x40)은 여기서 찍는다
}

// ── 묶음 6-e(2) 길 훑기 ───────────────────────────────────────────────
//
// 정답지 `src/unit_ai_path.go:19~330` `nox_xxx_pathFind_50BA00`.
// ★ 이것은 **물결 훑기(너비 우선)** 다 — 한 물결을 다 보고 다음 물결로 간다.
//
// ⭕ 짜임새
//   · `MapIndexLast` 를 **하나 올려** 「이번 훑기에 가 봤다」 자국으로 쓴다.
//     칸의 `visit` 이 그 수와 같으면 이미 가 본 칸이다(6-e(1) 의 세대와 **다른 수다**).
//   · 물결은 **외줄 사슬** — 칸마다 `next`(정답지 `Field8`)로 잇고, `parent`(`Field4`)로 온 길을 남긴다
//   · 이웃 여덟은 blob `0x587000+234216` 표 — **0~3 이 직교, 4~7 이 대각**.
//     시작 자리를 **난수로 돌려서**(`ioff`) 본다. 같은 거리면 늘 같은 쪽으로 쏠리지 않게 하는 장치다
//   · `awayFrom=false` 면 **목표에 가장 가까운** 칸을, `true` 면 **가장 먼** 칸을 best 로 들고 간다
//     (정답지 이름은 `far` 인데, `windows.h` 의 옛 매크로 `far` 와 부딪혀 이름을 바꿨다)
//     (도망갈 때 쓰는 갈래다 — `Nox_xxx_generateRetreatPath_50CA00`)
//   · 끝나는 갈래 셋 — **0** 목표에 닿음 · **1** 자국 자리를 다 씀 · **2** 깊이 한도·훑을 곳 없음.
//     어느 갈래든 **best 에서 길을 만든다**(`Sub_50C320`)
//
// ★★ **시작 자리가 막혔을 때**(`v63`) — 훑기를 **느슨하게** 연다.
//   막힌 칸에서 출발하면 아무 데도 못 가니, 「지날 수 있는 칸」 조건을 잠시 풀고
//   한 칸이라도 빠져나오면(`v64`) **그 물결에서 못 빠져나온 칸을 쳐내고** 다시 조인다.
//   거르개(`fnc`)에도 같은 장치가 따로 있다(`v61`/`v65`).
//
// ⚠ 아직 안 옮긴 곁가지 — **6-e(2b)** 로 미룬다
//   · `preCheckWalls_50C8D0` 시작 칸 미리 열기
//   · 대각선 **이음매** 검사(`Sub_57B500` 벽 모서리 번호 + 문 열쇠) — 지금은 **두 직교 칸이 뚫려야** 대각을 준다
//   · `sub_50B870`(벽 모서리) · `sub_50C830`(물칸 6번 타일)
//   · `Sub_50AC20` 나르는 칸(구멍·전송·승강기)을 자식으로 얹기
//   · 사람·소환수·문은 아직 `obstacles_` 에 없다(묶음 9)

// 이웃 여덟 (blob_587000.dat +234216, Point32 여덟 개를 그대로 읽음)
static const int kAINb[8][2] = {
    { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 },       // 0~3 직교
    { 1, 1 }, { -1, -1 }, { -1, 1 }, { 1, -1 },     // 4~7 대각
};

// MapTraceObstacles(`server/map.go:453`) — 두 점 사이를 **장애물**이 막는가. 안 막으면 참.
//   ⚠ 정답지는 사람·소환수(적이면 지나침)·문도 본다. 우리 `obstacles_` 에는 아직 안 움직이는 것만 있다
bool Server::MapTraceObstacles(int skipObstacle, float x1, float y1, float x2, float y2) const {
    const float qx0 = std::min(x1, x2), qx1 = std::max(x1, x2);
    const float qy0 = std::min(y1, y2), qy1 = std::max(y1, y2);
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        if (static_cast<int>(i) == skipObstacle) continue;
        const PhysBody& o = obstacles_[i];
        if (!o.hasCollide) continue;                         // FlagNoCollide · AllowOverlap 자리
        double ox0, oy0, ox1, oy1;
        BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
        if (ox1 < qx0 || ox0 > qx1 || oy1 < qy0 || oy0 > qy1) continue;   // EachObjInRect
        if (o.shape == 2) {
            float fx, fy;
            if (PointOnTheLine(x1, y1, x2, y2, o.x, o.y, fx, fy)) {
                const double dx = fx - o.x, dy = fy - o.y;
                if (dy * dy + dx * dx <= static_cast<double>(o.r * o.r)) return false;
            }
        } else if (o.shape == 3) {
            const float px = o.x, py = o.y;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxLB, py + o.boxLT)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxLB, py + o.boxLT)) return false;
        }
    }
    return true;
}

void Server::AIPathResetVisit() { aiVisit_.clear(); }

// NewVisitNode — 자국 자리는 **1024개**(`alloc.NewClassT("VisitNodes", …, 1024)`). 넘으면 −1.
int Server::AIPathNewVisit(int x, int y, int parent, int next) {
    if (aiVisit_.size() >= kAIVisitMax) return -1;
    AIVisitNode n;
    n.x = static_cast<int16_t>(x);
    n.y = static_cast<int16_t>(y);
    n.parent = parent;
    n.next = next;
    aiVisit_.push_back(n);
    return static_cast<int>(aiVisit_.size()) - 1;
}

// ── 묶음 6-e(3) 길에서 점 뽑기 ────────────────────────────────────────
//
// 정답지 `server/object_ai_path.go:353` `Sub_50C320`.
// ★ 이름과 달리 **길을 다듬는 게 아니라, 칸마다 점을 하나씩 찍는다.**
//   찍는 자리가 **왔던 쪽으로 붙어** 있어서, 걸어가면 저절로 **모서리를 질러간다**.
//
// ⭕ 짜임새
//   · 찾은 칸에서 **온 길을 거꾸로** 되짚으며 점을 쌓고, 끝에 **통째로 뒤집는다**(`swapPoints`)
//   · 첫 점 — 목표에 닿았으면 **정확한 목표 자리**, 아니면 그 칸의 **한가운데(+11.5)**
//   · 그다음부터 칸마다 — 「그 칸에서 **앞 칸 쪽으로** 붙은 점」.
//     `dx`·`dy` 는 **앞 칸에서 이 칸을 뺀 값**(되짚는 방향)이고,
//     음수면 칸의 **오른쪽·아래 끝(+23−2.3)**, 양수면 **왼쪽·위 끝(+2.3)**, 0 이면 **가운데(+11.5)**
//   · ⚠ `dx` 와 `dy` 가 **둘 다 0 이면 점을 안 넣는다** — 정답지 `dx == 0` 갈래에 `else` 가 없다
//   · 점은 **1024개**까지(`alloc.Make([]types.Pointf{}, 1024)`)
//
// ⚠ 나르는 칸(`Flags12 & 2` · `sub50AEA0` · 넷코드를 몬스터 자료에 적기)은 **6-e(2b)**.
//   훑기가 아직 나르는 칸을 자식으로 안 얹으므로 이 갈래가 설 자리가 없다.
// ⚠ `MaybeAppendWorkPath`(점이 담을 곳보다 많으면 상태를 1 로)는 **6-f 가기**에서 붙인다.
void Server::AIPathCollect(int node, bool hasExact, float ex, float ey) {
    aiPathCells_.clear();
    aiPathPts_.clear();
    if (node < 0) return;
    // 칸 목록 — 눈으로 보려고 따로 둔다(정답지에는 없다)
    std::vector<int> rev;
    for (int i = node; i >= 0; i = aiVisit_[static_cast<size_t>(i)].parent) rev.push_back(i);
    for (size_t i = rev.size(); i-- > 0; ) {
        const AIVisitNode& n = aiVisit_[static_cast<size_t>(rev[i])];
        aiPathCells_.push_back({ n.x, n.y });
    }

    auto put = [&](double x, double y) {
        if (aiPathPts_.size() >= kAIPointMax) return;
        aiPathPts_.push_back({ static_cast<float>(x), static_cast<float>(y) });
    };
    if (hasExact) {
        put(ex, ey);
    } else {
        put(aiVisit_[static_cast<size_t>(node)].x * 23.0 + 11.5,
            aiVisit_[static_cast<size_t>(node)].y * 23.0 + 11.5);
    }
    int prev = node;
    for (int it = aiVisit_[static_cast<size_t>(node)].parent; it >= 0;
         prev = it, it = aiVisit_[static_cast<size_t>(it)].parent) {
        const int dx = aiVisit_[static_cast<size_t>(it)].x - aiVisit_[static_cast<size_t>(prev)].x;
        const int dy = aiVisit_[static_cast<size_t>(it)].y - aiVisit_[static_cast<size_t>(prev)].y;
        const double bx = aiVisit_[static_cast<size_t>(it)].x * 23.0;
        const double by = aiVisit_[static_cast<size_t>(it)].y * 23.0;
        if (dx < 0) {
            if (dy > 0)       put(bx + 23.0 - 2.3, by + 2.3);
            else if (dy < 0)  put(bx + 23.0 - 2.3, by + 23.0 - 2.3);
            else              put(bx + 23.0 - 2.3, by + 11.5);
        } else if (dx == 0) {
            if (dy > 0)       put(bx + 11.5, by + 2.3);
            else if (dy < 0)  put(bx + 11.5, by + 23.0 - 2.3);
            // ★ 둘 다 0 이면 **점이 없다** — 정답지에 else 가 없다
        } else {
            if (dy < 0)       put(bx + 2.3, by + 23.0 - 2.3);
            else if (dy == 0) put(bx + 2.3, by + 11.5);
            else              put(bx + 2.3, by + 2.3);
        }
    }
    std::reverse(aiPathPts_.begin(), aiPathPts_.end());      // swapPoints
}

int Server::AIPathFind(bool awayFrom, const PathActor& act, float fromX, float fromY,
                       float toX, float toY, const PathFilter& fnc, int maxDepth) {
    aiPathStatus_ = 0;
    ++aiIndexLast_;
    AIPathMaybeIndexObjects();

    const int sx = static_cast<int>(static_cast<float>(fromX / kWallCell));
    const int sy = static_cast<int>(static_cast<float>(fromY / kWallCell));
    const int gx = static_cast<int>(toX / kWallCell);
    const int gy = static_cast<int>(toY / kWallCell);

    // ⚠ `preCheckWalls_50C8D0`(시작 칸 미리 열기)은 6-e(2b)
    bool relaxWall = !AIPathCanPass(act.airborne, act.subclass, sx, sy);   // v63
    bool relaxFn = (fnc && !fnc(sx, sy));                                  // v61

    AIPathResetVisit();
    const int root = AIPathNewVisit(sx, sy, -1, -1);
    if (root < 0) { aiPathStatus_ = 2; AIPathCollect(-1, false, 0, 0); return 2; }
    if (AIIndexNode* rp = AIPathIndexAt(sx, sy)) rp->visit = aiIndexLast_;

    int cur = awayFrom ? 0 : 999999;
    int best = -1;                                           // v67
    int wave = root;                                         // vn2
    int depth = 0;                                           // v66
    bool escWall = false, escFn = false;                     // v64 · v65

    for (;;) {
        int scan = wave;                                     // vn3
        wave = -1;
        int nextWave = -1;                                   // v77
        while (scan >= 0) {
            const int x1 = aiVisit_[static_cast<size_t>(scan)].x;
            const int y1 = aiVisit_[static_cast<size_t>(scan)].y;
            const int dd = (x1 - gx) * (x1 - gx) + (y1 - gy) * (y1 - gy);
            if (awayFrom) {
                if (dd > cur) { best = scan; cur = dd; }
            } else {
                if (dd < cur) { best = scan; cur = dd; }
                if (!relaxWall && !relaxFn && x1 == gx && y1 == gy) {
                    aiPathStatus_ = 0;
                    AIPathCollect(scan, true, toX, toY);
                    return 0;
                }
            }
            const int ioff = static_cast<int>(NextRandom(8));
            for (int i = 0; i < 8; ++i) {
                const int ii = (i + ioff) % 8;
                const int x2 = x1 + kAINb[ii][0];
                const int y2 = y1 + kAINb[ii][1];
                AIIndexNode* ip = AIPathIndexAt(x2, y2);
                if (!ip || ip->visit == aiIndexLast_) continue;
                ip->visit = aiIndexLast_;                    // 가 봤다고 찍는다

                bool accept = false;
                // ★ 이웃이 **바로 목표 칸**이면, 칸 한가운데에서 **정확한 점까지 줄이 뚫리는지** 본다.
                //   뚫리면 벽 검사를 **건너뛰고** 받아들인다(정답지 LABEL_32).
                if (x2 == gx && y2 == gy) {
                    const float ax = x1 * 23.0f + 11.0f, ay = y1 * 23.0f + 11.0f;
                    if (MapTraceObstacles(act.obstacleSkip, ax, ay, toX, toY) &&
                        MapTraceRay(ax, ay, toX, toY, act.airborne ? 5 : 1))
                        accept = true;
                }
                if (!accept) {
                    // 대각선 **이음매** — 두 옆칸의 **벽 방향 번호**가 그 대각을 막는가(6-e(2b)) ·
                    //   그리고 두 옆칸이 아예 막혔나(정답지도 둘 다 본다)
                    if (ii >= 4) {
                        if (!AIPathDiagOk(ii, x1, y1)) continue;
                        if (AIPathBlockedFlags(act.airborne, x1, y2)) continue;
                        if (AIPathBlockedFlags(act.airborne, x2, y1)) continue;
                    }
                    // ★ `sub_50B870` — **벽이 있는 칸은 못 지난다**. 정답지는 벽을 격자 깃발에
                    //   안 찍고 **오로지 여기서** 막는다(6-e(2b·2)).
                    //   ⚠ 이 관문은 **느슨한 갈래보다 먼저**다 — 벽에 파묻히면 못 빠져나온다
                    if (WallDirAt(x2, y2) != -1) continue;
                    if (relaxWall) {
                        if (AIPathCanPass(act.airborne, act.subclass, x2, y2)) {
                            escWall = true;                  // 빠져나왔다
                        } else if (!AIPathBlockedFlags(act.airborne, x1, y1) &&
                                   AIPathBlockedFlags(act.airborne, x2, y2)) {
                            continue;                        // 정답지 주석: 왜 한쪽만 보는지 모름
                        }
                    } else {
                        if (!AIPathCanPass(act.airborne, act.subclass, x2, y2)) continue;
                        if (fnc) {
                            const bool ok = fnc(x2, y2);
                            if (relaxFn) { if (ok) escFn = true; }
                            else if (!ok) continue;
                        }
                    }
                }
                const int nn = AIPathNewVisit(x2, y2, scan, nextWave);
                if (nn < 0) {                                // 자국 자리를 다 썼다
                    aiPathStatus_ = 1;
                    AIPathCollect(best, false, 0, 0);
                    return 1;
                }
                nextWave = nn;
            }
            // ⚠ `Sub_50AC20` 나르는 칸(구멍·전송·승강기) 얹기는 6-e(2b)
            scan = aiVisit_[static_cast<size_t>(scan)].next;
        }
        wave = nextWave;

        // ★ 느슨하게 열었다가 **한 칸이라도 빠져나왔으면**, 못 빠져나온 칸을 물결에서 쳐낸다
        if (escWall) {
            int prev = -1;
            for (int it = wave; it >= 0; ) {
                const int nx = aiVisit_[static_cast<size_t>(it)].next;
                if (AIPathCanPass(act.airborne, act.subclass,
                                  aiVisit_[static_cast<size_t>(it)].x,
                                  aiVisit_[static_cast<size_t>(it)].y)) {
                    prev = it;
                } else if (prev >= 0) {
                    aiVisit_[static_cast<size_t>(prev)].next = nx;
                } else {
                    wave = nx;
                }
                it = nx;
            }
            relaxWall = false; escWall = false;
        }
        if (escFn) {
            int prev = -1;
            for (int it = wave; it >= 0; ) {
                const int nx = aiVisit_[static_cast<size_t>(it)].next;
                if (fnc(aiVisit_[static_cast<size_t>(it)].x, aiVisit_[static_cast<size_t>(it)].y)) {
                    prev = it;
                } else if (prev >= 0) {
                    aiVisit_[static_cast<size_t>(prev)].next = nx;
                } else {
                    wave = nx;
                }
                it = nx;
            }
            relaxFn = false; escFn = false;
        }
        if (wave < 0) break;                                 // 훑을 곳이 없다
        if (maxDepth != 0 && depth >= maxDepth) {            // 깊이 한도
            aiPathStatus_ = 2;
            AIPathCollect(best, false, 0, 0);
            return 2;
        }
        ++depth;
    }
    aiPathStatus_ = 2;
    AIPathCollect(best, false, 0, 0);
    return 2;
}

// ── 묶음 6-f(1) 길 따라 걷기 ──────────────────────────────────────────
//
// 정답지 `legacy/GAME4_1.c:774` `nox_xxx_creatureActuallyMove_50D3B0`.
// 6-e(3) 이 뽑아 둔 점들 가운데 **하나를 골라 그쪽으로 속도를 준다.**
//
// ⭕ 고르는 법 — 지금 자리에서 **줄이 뚫리는**(`mapTraceRay` 깃발 **132**) 점만 본다.
//   · **8 보다 먼** 점 중에서는 **가장 가까운 것**을 고른다 — 너무 멀리 질러가지 않게
//   · **8 안**으로 들어온 점이 **마지막 점**이면 **다 온 것**이다(참을 돌려준다)
//   · 8 안인데 마지막이 아니면 「지나친 점」으로 적어 둔다 —
//     멀리 보이는 점이 하나도 없을 때만 그리로 간다
//   · 그것마저 없으면 길을 버린다(막혔다)
//
// ★ **보는 쪽은 8씩 안 돌린다** — `facing` 과 `facingWant` 를 **같이** 놓는다(정답지 `+62`·`+63`).
//   바라보기 행동(6-d)과 다른 자리다. 걸을 때는 곧바로 그쪽을 본다.
// ★ 보는 쪽은 **가는 점이 아니라 그 점으로 들어오는 토막**의 방향이다
//   (`pt[i] − pt[i−1]`, 첫 점이면 `pt[1] − pt[0]`).
//
// ⚠ 속도에 곱하는 **깃발 0x4000 갈래**(`*(v1+484)+96` 배)는 아직 안 옮겼다 — 6-g 에서 본다.
bool Server::CreatureActuallyMove(Creature& k) {
    if (k.path.empty()) return false;
    const int n = static_cast<int>(k.path.size());
    if (k.pathInd >= n) { k.path.clear(); return false; }

    int farIdx = -1;                 // v16 — 8 보다 먼 것 중 **가장 가까운** 점
    double farD2 = 10000000.0;       // v17
    int nearIdx = -1;                // v18 — 8 안으로 들어온 마지막 점
    for (int i = k.pathInd; i < n; ++i) {
        const float px = k.path[static_cast<size_t>(i)].first;
        const float py = k.path[static_cast<size_t>(i)].second;
        if (!MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), px, py, 132)) continue;
        const double dx = px - k.x, dy = py - k.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > 64.0) {
            if (farIdx < 0 || farD2 > d2) { farD2 = d2; farIdx = i; }
        } else {
            if (i == n - 1) { k.path.clear(); return true; }   // 다 왔다
            nearIdx = i;
        }
    }
    int pick = farIdx;
    if (farIdx < 0) {
        if (nearIdx < 0) { k.path.clear(); return false; }     // 아무 점도 안 보인다
        pick = nearIdx;
    }
    k.pathInd = pick;

    const double dx = k.path[static_cast<size_t>(pick)].first - k.x;
    const double dy = k.path[static_cast<size_t>(pick)].second - k.y;
    const double len = std::sqrt(dx * dx + dy * dy) + 0.01;    // blob 0x581450+10288 = 0.01
    // ★ 묶음 6-h — **뛰는 중이면 빠르기에 뛰기 배율을 곱한다**(깃발 0x4000 · 몬스터표 +96)
    double spd = k.speedCur;
    if (k.running) {
        const MonsterStats* ms = FindMonster(k.type);
        if (ms && ms->runMul > 0.0) spd *= ms->runMul;
    }

    // 보는 쪽 — 그 점으로 **들어오는 토막**의 방향
    double fx, fy;
    if (pick <= 0) {
        // ⚠ 정답지는 여기서 `pt[1]` 을 읽는다 — 점이 하나뿐이면 없는 자리를 읽는다.
        //   우리는 그때 **그 점 쪽**을 본다(길이 하나면 방향이 그것밖에 없다).
        if (n >= 2) {
            fx = k.path[1].first - k.path[0].first;
            fy = k.path[1].second - k.path[0].second;
        } else {
            fx = dx; fy = dy;
        }
    } else {
        fx = k.path[static_cast<size_t>(pick)].first - k.path[static_cast<size_t>(pick - 1)].first;
        fy = k.path[static_cast<size_t>(pick)].second - k.path[static_cast<size_t>(pick - 1)].second;
    }
    const uint8_t dir = DirFromVec(fx, fy);
    k.facing = dir;
    k.facingWant = dir;                                        // ★ 둘을 같이 놓는다

    k.velX = spd * dx / len;
    k.velY = spd * dy / len;
    return false;
}

// 조각 D — 길을 들고 있는 크리처 수. 「N마리인데 길 가진 것 0」이면 길찾기가 안 도는 것이다.
size_t Server::petsWithPath() const {
    size_t n = 0;
    for (const Creature& k : pets_) if (!k.path.empty()) ++n;
    return n;
}

// ── 묶음 6-f(2) 길 내기·다시 내기 ─────────────────────────────────────
//
// 정답지 `src/unit_ai_path.go:520~544` + `legacy/GAME4_1.c:877`.
//
// ⭕ **길은 아무 때나 다시 내지 않는다** — 매 틱 새로 내면 256×256 을 매 틱 훑는다.
//   · `creatureSetDetailedPath` 는 **10프레임 관문**을 둔다. 관문에 걸리면 길을 **비우고**
//     결과를 **1**(자리 다 씀)로 적는다 — 그 틱에는 아예 안 움직인다
//   · `creatureSetMovePath` 는 그 위에 **「목표가 얼마나 움직였나」** 를 더 본다 —
//     줄이 뚫리는 갈래에서는 **50 유닛(제곱 2500)** 을 넘게 움직여야 다시 낸다
//
// ⭕ 점은 **31개까지**만 옮겨 담는다(`ud.Path` 가 32칸인데 `appendWorkPath` 가 마지막 칸을 안 쓴다).
//   넘치면 길 찾기 결과가 **0(닿음) → 1** 로 내려앉는다(`MaybeAppendWorkPath`).
//
// ⚠ **길목 길**(`sub_50D2A0` · `sub_50D2E0` · `sub_547F20`)은 미뤘다 —
//   길목(Waypoint)이 우리 맵 자료에 아직 없다. 정답지에서 길목 길이 **실패했을 때와 같은 길**을 탄다.

// nox_xxx_genPathToPoint_50B9A0 + MaybeAppendWorkPath — 낸 점을 크리처의 길로 옮겨 담는다
int Server::CreatureGenPath(Creature& k, float tx, float ty) {
    uint32_t oflags = 0;
    auto tb = typeBodies_.find(k.type);
    if (tb != typeBodies_.end()) oflags = tb->second.flags;
    const bool airborne = (oflags & 0x4000) != 0;
    AIPathFind(false, PathActor{ airborne, 0, -1 },
               static_cast<float>(k.x), static_cast<float>(k.y), tx, ty, PathFilter(), 0);
    // MaybeAppendWorkPath — 담을 곳보다 점이 많으면 **닿았다(0)를 1 로 내린다**
    if (aiPathPts_.size() > kCreaturePathMax && aiPathStatus_ == 0) aiPathStatus_ = 1;
    k.path.clear();
    for (size_t i = 0; i < aiPathPts_.size(); ++i) {
        if (k.path.size() >= kCreaturePathMax - 1) break;   // 마지막 칸은 안 쓴다
        k.path.push_back(aiPathPts_[i]);
    }
    return static_cast<int>(k.path.size());
}

// Nox_xxx_creatureSetDetailedPath_50D220 — **10프레임 관문**을 넘어야 새로 낸다
void Server::CreatureSetDetailedPath(Creature& k, float tx, float ty) {
    k.pathInd = 0;                                   // ud.Field67 = 0
    if (frame_ - k.pathFrame >= 10) {
        k.pathGoalX = tx; k.pathGoalY = ty;          // ud.Field68 — 이때 잡은 목표를 적어 둔다
        CreatureGenPath(k, tx, ty);
        k.pathStatus = static_cast<uint8_t>(aiPathStatus_);
        k.pathFrame = frame_;
    } else {
        k.path.clear();                              // ud.Field2 = 0 — 길이 없는 셈
        k.pathStatus = 1;
    }
}

// nox_xxx_creatureSetMovePath_50D5A0 — **1 이면 다 왔다**
int Server::CreatureSetMovePath(Creature& k, float tx, float ty) {
    const double ddx = tx - k.x, ddy = ty - k.y;
    if (std::sqrt(ddx * ddx + ddy * ddy) + 0.01 <= 8.0) return 1;   // 목표까지 8 안이면 끝

    uint32_t oflags = 0;
    auto tb = typeBodies_.find(k.type);
    if (tb != typeBodies_.end()) oflags = tb->second.flags;
    const uint8_t rayFlags = static_cast<uint8_t>((oflags >> 12) & 4);   // 깃발 0x4000 이면 4
    const bool clear = MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), tx, ty, rayFlags);

    if (!clear) {
        // ⚠ 여기서 정답지는 **길목 길**을 먼저 본다. 길목이 없으므로 곧바로 자세한 길로 간다
        if (k.path.empty()) CreatureSetDetailedPath(k, tx, ty);
    } else if (k.path.empty() ||
               ((frame_ - k.pathFrame) > 10 &&
                (static_cast<double>(k.pathGoalX - tx) * (k.pathGoalX - tx) +
                 static_cast<double>(k.pathGoalY - ty) * (k.pathGoalY - ty)) > 2500.0)) {
        CreatureSetDetailedPath(k, tx, ty);
    }
    if (!k.path.empty() && CreatureActuallyMove(k)) { k.path.clear(); return 1; }
    return 0;
}

// ── 묶음 6-f(3) 틱에 잇기 ─────────────────────────────────────────────
//
// ★★ 6-e·6-f(1)(2) 는 **함수만 있고 아무도 안 불렀다.** 이 조각이 **크리처 틱에 건다.**
//
// ⚠ 이것이 **임시 구현을 갈아 끼운다** — 그 전에는 목표를 향해 **곧장 직선으로** 갔다.
//   곧장 가면 **벽에 붙어 버틴다.** 이제 길을 내어 **돌아간다**.
//
// ⭕ 한 틱에 하는 일
//   1. `CreatureSetMovePath` — 다 왔으면 참 · 아니면 길을 내고 **속도를 세운다**
//   2. 그 속도로 **자리를 옮긴다**. 벽은 **x·y 를 따로** 보아 한 축이 막혀도 다른 축으로 미끄러진다
//   · 10프레임 관문에 걸려 길이 비면 **그 틱은 안 움직인다**(정답지도 그렇다)
bool Server::CreatureTickMove(Creature& k, double tx, double ty) {
    if (CreatureSetMovePath(k, static_cast<float>(tx), static_cast<float>(ty)) == 1) {
        k.velX = 0; k.velY = 0;
        return true;                                         // 다 왔다
    }
    if (k.path.empty()) { k.velX = 0; k.velY = 0; return false; }
    CreatureApplyVelocity(k);
    return false;
}

// ── 묶음 6-f(5) 아무 데나 걷기(29) ────────────────────────────────────
//
// 정답지 `legacy/GAME5.c:68` `nox_xxx_mobActionRandomWalk_545020` + `sub_545090`.
// 가기(7)가 **막혔을 때** 올리는 행동이다 — 길을 못 내니 **아무 쪽으로나 조금씩 틀며 걷는다**.
//
// ⭕ 갈 쪽 고르기(`sub_545090`)
//   · **지금 보는 쪽에 ±20 을 더한다** — 한 번에 확 꺾지 않고 **조금씩** 튼다.
//     `randomInt(-20, 20)` 은 **양끝을 다 넣으므로** 41가지다
//   · 0~255 로 감싼다(정답지의 두 줄짜리 감싸기는 `& 0xFF` 과 같다)
//
// ⭕ 걷기(`545020`)
//   · 고른 쪽을 **보는 쪽·돌아가려는 쪽에 같이** 놓는다 — ★ 8씩 도는 게 아니라 **곧바로** 튼다
//   · 속도 = 빠르기 × 그 쪽의 단위 벡터. **길 찾기를 안 쓴다**
//
// ⚠ 안 옮긴 갈래 둘
//   · **물칸(타일 6)을 만나면 90° 꺾기** — 타일 자료가 없다(6-e(2b) 의 `sub_50C830` 과 같은 자리)
//   · 깃발 0x400 이 있으면 그 물칸 검사를 통째로 건너뛴다 · 깃발 0x4000 의 속도 배율은 6-g
//   · `monsterMoveAudio` 는 소리라 안 옮긴다

uint8_t Server::AIRandomWalkDir(Creature& k) {
    const int jitter = static_cast<int>(NextRandom(41)) - 20;   // randomInt(-20, 20)
    return static_cast<uint8_t>((static_cast<int>(k.facing) + jitter) & 0xFF);
}

void Server::AIRandomWalk(Creature& k) {
    const uint8_t d = AIRandomWalkDir(k);
    k.facing = d;
    k.facingWant = d;                                          // ★ 둘을 같이 놓는다
    k.velX = k.speedCur * DirVecX(d);
    k.velY = k.speedCur * DirVecY(d);
    MonsterMoveAudio(k, AnimStat::StepWalk);                  // ★ 09-29 B5 — `GAME5.c:88`
}

// ── 묶음 6-g(2b) 때릴 거리를 정하고 사슬을 쌓는다 ─────────────────────
//
// ★★ 6-g(1) 에서 **싸움(15)이 근접(16)을 바로 올린다**고 적었는데 **틀렸다.**
//   싸움은 `nox_xxx_TODOsomeCallingMeleeAttack_531B40`(`legacy/GAME4_3.c:1623`)을 부르고,
//   그것이 **때릴 수 있나**를 보아 갈래를 고른다. 그리고 고른 갈래가
//   `nox_xxx_monsterAction_531C60`(1679행) 으로 **조건과 행동을 한 줄로 쌓는다**.
//
// ⭕ 쌓이는 차례 (아래 → 위) — `monsterAction_531C60`
//     60 새 적 없음 · 42 살아 있나 · [50 쏠 수 있으면 사거리×0.6 안인가] ·
//     45 보이나 · **16 근접 공격** · **26 그쪽 바라보기** · 49 **근접 사거리보다 머나** ·
//     [사람이면 65·40] · **7 그 자리로 가기**
//   ★ 읽는 법 — 맨 위가 **가기(7)** 이고 그 아래 **49(더 머나)** 가 조건이다.
//     즉 **근접 사거리보다 멀면 다가가고**, 닿으면 49 가 깨져 7 과 49 가 빠지며
//     **26(바라보기) → 16(때리기)** 이 드러난다. **이 한 줄이 「쫓아가 때린다」다**
//
// ⭕ 갈래 고르기(`531B40`) — 몬스터 기준
//     `monsterCanMelee_534220` = **근접 사거리(+112) > 0** · `monsterCanShoot_534280` = 몬스터표 **+148**
//   ⚠ **쏠 수 있나(+148)는 아직 못 읽는다** — thing.bin 그 칸을 안 파싱했다. **거짓으로 둔다**.
//     그래서 지금은 「근접이 되면 근접 사슬 · 아니면 `sub_531C10`」 두 갈래만 돈다
//   ⚠ 원거리 갈래(`sub_531D50`)와 주문 갈래(`monsterCanCast_534300`)는 **6-g(3)**
void Server::MonsterPushAttack(Creature& k, const Target& foe, float fx, float fy) {
    const MonsterStats* m = FindMonster(k.type);
    const double reach = (m && m->meleeRange > 0.0) ? m->meleeRange : 0.0;
    const bool canMelee = reach > 0.0;
    // ★ 2026-09-15 — **monster.bin 을 직접 읽어 채웠다**. `monsterCanShoot_534280` 은
    //   몬스터표 **+148(MISSILE_NAME)** 이 비었나만 본다
    const bool canShoot = (m && m->missileName != nullptr);
    const double shootRange = m ? m->missileRange : 0.0;

    auto push = [&](uint8_t act) {
        if (!AIPush(k, act)) return -1;
        k.aiStack[k.aiStackInd].targetAddr = foe.addr;
        k.aiStack[k.aiStackInd].targetPet = foe.pet;
        return k.aiStackInd;
    };

    // ── 갈래 고르기(`531B40`) ──
    //   ★ 09-29 B7 — 맨 먼저 반주문(29)이 아니면 **공격 주문**(`mobCastRelated2_540D90`) — 골랐으면 끝
    if (!k.buffs.Has(29) && MonsterCastPick(k, 0x40000000u, foe, k.castNextOffense, 0, static_cast<int>(3 * kServerTickRate), CastStat::PickOffense)) return;
    //   쏠 수 있으면 — 근접도 되고 **쏘기 사거리의 절반보다 가까우면** 근접 사슬, 아니면 **원거리 사슬**
    if (canShoot) {
        const double ex = fx - k.x, ey = fy - k.y;
        const double dist = std::sqrt(ex * ex + ey * ey);
        if (!(canMelee && dist < shootRange * 0.5)) {
            // sub_531D50 — **원거리 사슬**(아래 → 위)
            //   60 새 적 없음 · 45 보이나 · **17 쏘기** · 26 바라보기 ·
            //   47 쏠 줄이 막혔나 · 49 **쏘기 사거리보다 머나** · 40 또는 · **7 가기**
            push(60);
            push(45);
            const int i17 = push(17);
            if (i17 >= 0) { k.aiStack[i17].argX = fx; k.aiStack[i17].argY = fy; }
            push(26);
            // ⚠ `sub_534710`(깃발 0x40 · 아래 행동 4)은 아직 안 옮겼다 — 늘 거짓으로 본다
            push(47);                                        // 쏠 줄이 막혔나
            const int i49r = push(49);
            if (i49r >= 0) k.aiStack[i49r].argF = static_cast<float>(shootRange);
            AIPush(k, 40);                                   // 또는 — 47 과 49 를 묶는다
            if (push(7) >= 0) {
                k.aiStack[k.aiStackInd].argX = fx;
                k.aiStack[k.aiStackInd].argY = fy;
            }
            return;
        }
    }
    if (!canMelee) {
        // ★ 09-29 B7 — 주문을 쓸 수 있으면(STATUS 0x20) **아무것도 안 쌓는다**(`531B40:1646~1649` — 주문은 위 공격 주문 몫).
        //   ✖ 전에는 비홀더도 아래로 떨어져 적에게 걸어갔다
        if (MonsterBrainOf(k.type).status & 0x20) return;
        // sub_531C10 — 때릴 수단이 없으면 **일단 그 자리로 간다**
        push(60);                                            // 새 적 없음
        push(42);                                            // 살아 있나
        if (push(7) >= 0) {
            k.aiStack[k.aiStackInd].argX = fx;
            k.aiStack[k.aiStackInd].argY = fy;
        }
        return;
    }
    // monsterAction_531C60
    push(60);
    push(42);
    if (canShoot) {
        const int i = push(50);                              // 쏘기 사거리 × 0.6 안인가
        if (i >= 0) k.aiStack[i].argF = static_cast<float>(shootRange * 0.60000002);
    }
    push(45);                                                // 보이나
    AIPush(k, 16);                                           // 근접 공격 — 대상을 안 받는다(정답지도 그렇다)
    push(26);                                                // 그쪽 바라보기
    const int i49 = push(49);                                // **근접 사거리보다 머나**
    if (i49 >= 0) k.aiStack[i49].argF = static_cast<float>(reach);
    // ⚠ 사람이면 65(기력 기다림)·40(또는) 을 더 쌓는다 — 크리처에는 안 쌓는다
    if (push(7) >= 0) {
        k.aiStack[k.aiStackInd].argX = fx;
        k.aiStack[k.aiStackInd].argY = fy;
    }
}

// ── 묶음 6-g(3b) 크리처가 쏘는 것 ────────────────────────────────────
//
// 정답지 `mobActionMissileAtt_532610` 가 `newObjectByTypeID(monsterDef+148)` 로
// **`MISSILE_NAME` 오브젝트를 만들어** 그 쪽으로 속도를 준다.
//   · 피해는 날아가서 **맞을 때** 들어간다(`collideProjectileGeneric_4E87B0`, 종류 **11**)
//   · 그 함수는 **적·아군을 안 가린다** — 맞는 것에 그냥 준다
//   · 양은 `ImpShot` 이면 gamedata **`ImpShotDamage`**, 아니면 `COLLIDE` 의 인자다(둘 다 5)
// ⚠ **앞질러 겨눔**(`projAddVelocitySmth_533080`)은 아직 안 옮겼다 — 곧장 겨눈다
bool Server::SpawnCreatureMissile(Creature& k, const char* name, float tx, float ty,
                                  std::vector<OutPacket>& out) {
    (void)out;
    const ProjTypeInfo* pt = ProjType(name);
    if (!pt || pt->type == 0) return false;                  // 종류를 모르면 안 만든다(지어내지 않는다)
    const double dx = tx - k.x, dy = ty - k.y;
    const uint8_t dir = DirFromVec(dx, dy);
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    m.type = pt->type;
    m.flags = pt->flags;
    m.posX = m.newX = m.prevX = static_cast<float>(k.x);
    m.posY = m.newY = m.prevY = static_cast<float>(k.y);
    m.dir1 = m.dir2 = dir;
    m.createdFrame = m.frame136 = frame_;
    m.speed = pt->speed;
    m.owner = k.owner;                                       // 점수는 부른 사람 것이다
    m.ownerPet = static_cast<int>(&k - pets_.data());         // 09-20 — 쏜 것이 크리처라는 표시(뿌리 가리기)
    m.ud0 = m.ud2 = k.owner;
    m.collideKind = 3;                                       // ProjectileCollide
    m.fromPet = static_cast<int>(&k - pets_.data());         // 제 몸에 안 맞게
    m.collideAmount = pt->collideAmount;
    m.velX = DirVecX(dir) * m.speed;
    m.velY = DirVecY(dir) * m.speed;
    missiles_.push_back(m); ++missilesMade_;
    return true;
}

// ★ 09-26 M4a — 몬스터 소리 모음의 칸 하나(`nox_xxx_monsterGetSoundSet_424300` + `aud_501960(모음+4+4×칸, 몬스터, 0, 0)` — 모두에게 · 몬스터 자리).
//   번호가 0(모음 없음·NULL)이면 안 내고 「모음에없음」으로 센다 · 조각 0 인 번호는 `PlaySoundAt` 이 버린다(정답지도 조각 0 이면 안 만든다)
bool Server::MonsterSound(const Creature& k, int field) {
    if (field < 0 || field >= 18) return false;
    const auto& tbl = monsterSnd_[field];
    const uint16_t snd = (k.type < tbl.size()) ? tbl[k.type] : 0;
    ++monsterSndCount_[field][snd != 0 ? 1 : 0];
    if (snd == 0) return false;
    PlaySoundAt(snd, static_cast<float>(k.x), static_cast<float>(k.y), 0, 0, std::string(), PetTarget(k));
    return true;
}

// ── 묶음 6-i 소환수 명령 ──────────────────────────────────────────────
//
// 정답지 `nox_xxx_orderUnit_533900`(`legacy/GAME4_3.c:2367`) +
//        `nox_xxx_enactUnitOrder_5339A0`(같은 파일 2394).
//
// ⭕ 명령 여섯 — **0 추방 · 1 관찰 · 2 가만히 · 3 지켜 · 4 따라와 · 5 사냥**
//   · 번호가 **0** 이면 **내 것 전부**에게 준다(정답지는 「고른 것」 깃발 0x80 을 본 뒤 준다)
//   · 2·3·4·5 는 **쌓임을 통째로 비우고** 그 행동 하나를 얹는다 —
//     그래서 하던 싸움이 끊긴다. 이것이 명령이 「선다」는 뜻이다
//   · 3·4·5 는 **못 움직이는 것에는 안 먹는다**(`monsterIsMoveing` 빠르기 >= 0.01)
//   · **사나움**(`Aggression` +1304) — 가만히·지켜 **0.5** · 따라와·사냥 **0.83**
//   · 지켜는 **보는 거리**(`SightRange` +1312)를 **250** 으로 놓고, 쏘는 것이면 뛰기 깃발을 켠다
//
// ⚠ 안 옮긴 것 — 추방(`banishUnit_5017F0`)·관찰(`playerObserveMonster_4DDE80`)은 묶음 6-j·10 ·
//   말풍선(`monsterCmdSend_528BD0`) · 「고른 것」 깃발(0x80)
// ★ 09-26 M4b — 소리는 옮겼다: 2·3·4·5 가 **먹힐 때** 소리 모음 칸 16(RECOGNIZE · +68 · `GAME4_3.c:2430~2433·2445~2448·2471~2474·2490~2493`)
//   — 3·4·5 는 움직일 수 있을 때만(그 갈래 안) · 몬스터 자리에서 모두에게
void Server::EnactUnitOrder(Session& src, Creature& k, uint8_t order) {
    if (k.health == 0) return;                               // 죽은 것에는 안 먹는다(깃발 0x8000)
    // `monsterIsMoveing_534320` 그대로 — **지금 빠르기**만 본다(종류표로 눅이지 않는다)
    const bool canMove = k.speedCur >= 0.0099999998f;
    switch (order) {
        case 2:                                              // 가만히
            MonsterSound(k, 16);                             // ★ 09-26 M4b
            k.aggression = 0.5f;
            k.running = false;
            AIClearStack(k);
            AIPush(k, 0);
            k.ordered = true;
            break;
        case 3: {                                            // 지켜 — **선 자리를 지킨다**
            if (!canMove) break;
            MonsterSound(k, 16);                             // ★ 09-26 M4b
            k.aggression = 0.5f;
            const MonsterStats* m = FindMonster(k.type);
            if (m && m->missileName != nullptr) k.running = true;   // 쏘는 것이면 뛰기 깃발
            k.sightRange = 250.0f;
            AIClearStack(k);
            if (AIPush(k, 4)) {
                k.aiStack[k.aiStackInd].argX = static_cast<float>(k.x);
                k.aiStack[k.aiStackInd].argY = static_cast<float>(k.y);
                k.aiStack[k.aiStackInd].argU32 = k.facing;
            }
            k.ordered = true;
            break;
        }
        case 4:                                              // 따라와
            if (!canMove) break;
            MonsterSound(k, 16);                             // ★ 09-26 M4b
            k.aggression = 0.83f;
            k.running = false;
            AIClearStack(k);
            if (AIPush(k, 3)) {
                k.aiStack[k.aiStackInd].argX = static_cast<float>(src.posX);
                k.aiStack[k.aiStackInd].argY = static_cast<float>(src.posY);
                k.aiStack[k.aiStackInd].targetAddr = src.addr;
            }
            k.ordered = true;
            break;
        case 5:                                              // 사냥
            if (!canMove) break;
            MonsterSound(k, 16);                             // ★ 09-26 M4b
            k.aggression = 0.83f;
            k.running = false;
            AIClearStack(k);
            AIPush(k, 5);
            k.ordered = true;
            break;
        default: break;                                      // 0 추방 · 1 관찰 — 아직 안 옮겼다
    }
}

void Server::AIClearStack(Creature& k) {
    // ★ 09-29 B2 — 정답지 `ClearActionStack`(`server/object_ai.go:64~70`)은 「비고 놀기만 남음」이 될 때까지 **빼기**를 되풀이한다 —
    //   시작한 행동마다 끝(End)이 불린다(싸움 끝이 ALERT·달리기를 끈다). 전에는 통째로 지워서 명령이 싸움을 끊어도 ALERT 가 남을 뻔했다
    for (int guard = 0; guard <= Creature::kAIStackMax && !(k.aiStackInd == 0 && k.aiStack[0].action == 0); ++guard) AIPop(k);
    for (int i = 0; i < Creature::kAIStackMax; ++i) k.aiStack[i] = Creature::AIStackItem{};
    k.aiStackInd = 0;
    k.path.clear();
    k.velX = 0; k.velY = 0;
    aiStackChanged_ = true;
}

// ★ 10-01 시체 — 원본은 죽을 때 주인을 지워(`GAME4_1.c:504`) 주인 목록(+516)을 도는 명령에서 시체가 빠진다(`GAME4_3.c:2382~2388`).
//   우리는 주인을 남기지만(시체 플랜 결정 ①) **`EnactUnitOrder` 머리가 이미 체력 0 을 거른다**(원본 `enactUnitOrder_5339A0` 0x8000 `GAME4_3.c:2413`)
//   → 여기서 또 거르지 않음(돌연변이로 같은 일 두 번임을 확인 · 시험 `TestCorpseFilters` ①)
void Server::OrderUnit(Session& src, uint16_t netCode, uint8_t order) {
    if (netCode != 0) {
        for (auto& k : pets_)
            if (k.netCode == netCode && k.owner == src.addr) { EnactUnitOrder(src, k, order); return; }
        return;                                              // 남의 것·없는 것에는 안 먹는다
    }
    for (auto& k : pets_)                                    // 번호 0 = 내 것 전부
        if (k.owner == src.addr) EnactUnitOrder(src, k, order);
}

// ── 묶음 6-j(2) 길목 찾기 ─────────────────────────────────────────────
//
// ★★ **앞 기록 정정** — 6-i(3) 에 「우리 맵 자료에 길목이 없다」고 적었는데 **틀렸다**.
//   길목은 `main.cpp:462` 에서 이미 읽어 `SetWaypoints` 로 서버에 실린다.
//   없었던 것은 **고르는 함수**였다.
//
// ⭕ 정답지 `Sub50AFA0`(`server/object_ai_path.go:596`)이 격자에 길목 칸을 **깃발 0x40** 으로 찍고,
//   `Sub_50CB20`(`src/unit_ai_path.go:336`)이 **거기서부터 물결로 훑어** 가장 먼저 닿는
//   길목 칸을 돌려준다. 이웃은 blob `0x587000+234284` 의 **직교 넷**이다(대각 없음).
//
// ⚠ `sub_50B870`(벽 모서리)·`sub_50C830`(물칸)은 아직 안 옮겨 **지날 수 있는 것으로** 본다
// ⚠ 우리 `waypoints_` 는 점멸(4-e)이 쓰려고 `flags2 & 0x80` 에 더해 `flags & 1`(켜짐)까지 걸러 담았다.
//   정답지의 격자 찍기는 **0x80 만** 본다 — 꺼진 길목이 있으면 우리가 덜 찍는다
void Server::AIPathBakeWaypoints() {
    for (const auto& w : waypoints_) {
        const int gx = static_cast<int>(w.x / kWallCell);
        const int gy = static_cast<int>(w.y / kWallCell);
        if (AIIndexNode* n = AIPathIndexAt(gx, gy)) n->flags |= 0x40;
    }
}

// ── 묶음 6-j(2) 길목 이음 따라가기 ───────────────────────────────────
//
// 정답지 셋을 그대로 옮긴다:
//   `sub_547EE0`  — 쓸 수 있는 길목인가 (있나 + `flags&1` + `가면 & flags2`)
//   `sub_545B00`  — 들른 길목을 **16칸 고리**에 적는다
//   `sub_545C60`  — 지금 길목의 **이음** 중 최근에 안 들른 것을 **무작위로** 하나
//
// ⚠ 가면(mask)은 우리가 쓰는 자리에서 늘 0x80 이라 `waypoints_` 에 **있다 = 쓸 수 있다**로 둔다
//   (넣기 전에 main.cpp 가 `flags&1`·`flags2&0x80` 으로 거른다). 걸러진 길목을 가리키는 이음은
//   여기서 못 찾게 되는데, 그게 곧 정답지의 `sub_547EE0` 거짓이다
const Waypoint* Server::WaypointById(uint32_t id) const {
    if (id == 0) return nullptr;
    for (const auto& w : waypoints_) if (w.id == id) return &w;
    return nullptr;
}

void Server::RoamRemember(Creature& k, uint32_t id) {
    if (++k.roamRecentHead >= Creature::kRoamRecent) k.roamRecentHead = 0;
    k.roamRecent[k.roamRecentHead] = id;
    // ★ 같은 번호가 고리 안에 또 있으면 **옛 자리를 지운다**(정답지 `545B00` 의 뒷루프).
    //   안 지우면 한 길목이 고리를 두 칸 잡아먹어 「최근 16곳」이 실제로는 15곳이 된다
    for (int i = 0; i < Creature::kRoamRecent; ++i)
        if (k.roamRecent[i] == id && i != k.roamRecentHead) k.roamRecent[i] = 0;
}

uint32_t Server::RoamNextWaypoint(const Creature& k, uint32_t cur) {
    const Waypoint* w = WaypointById(cur);
    if (!w) return 0;
    // ① 이음 중 **최근에 안 들른 것**들을 모아 무작위로 하나
    uint32_t cand[32];
    size_t n = 0;
    for (const auto& l : w->links) {
        if (n >= 32) break;
        if (!WaypointById(l.first)) continue;                // sub_547EE0 거짓
        bool seen = false;
        for (int i = 0; i < Creature::kRoamRecent; ++i)
            if (k.roamRecent[i] == l.first) { seen = true; break; }
        if (!seen) cand[n++] = l.first;
    }
    if (n > 0) return cand[NextRandom(static_cast<uint32_t>(n))];
    // ② 다 들렀으면 **고리에서 가장 오래된 것부터** 되짚는다 — 단, 지금 길목의 이음인 것만
    //   (정답지 LABEL_27 — 머리 다음 칸부터 한 바퀴)
    for (int i = 0; i < Creature::kRoamRecent; ++i) {
        int at = k.roamRecentHead + i + 1;
        while (at >= Creature::kRoamRecent) at -= Creature::kRoamRecent;
        const uint32_t id = k.roamRecent[at];
        if (id == 0 || !WaypointById(id)) continue;
        for (const auto& l : w->links) if (l.first == id) return id;
    }
    return 0;                                                 // 막다른 길
}

uint32_t Server::AIPathFindWaypoint(const Creature& k, float& outX, float& outY) {
    if (waypoints_.empty()) return 0;
    ++aiIndexLast_;
    AIPathResetVisit();
    const int sx = static_cast<int>(k.x / kWallCell);
    const int sy = static_cast<int>(k.y / kWallCell);
    int wave = AIPathNewVisit(sx, sy, -1, -1);
    if (wave < 0) return 0;
    if (AIIndexNode* rp = AIPathIndexAt(sx, sy)) rp->visit = aiIndexLast_;
    static const int kNb4[4][2] = { { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 } };
    while (wave >= 0) {
        int next = -1;
        for (int it = wave; it >= 0; it = aiVisit_[static_cast<size_t>(it)].next) {
            const int x1 = aiVisit_[static_cast<size_t>(it)].x;
            const int y1 = aiVisit_[static_cast<size_t>(it)].y;
            if (AIPathFlagsAt(x1, y1) & 0x40) {              // 길목 칸이다
                // ★ 09-20 — **칸 한가운데가 아니라 길목의 진짜 자리**를 준다.
                //   한 칸은 23 이라 한가운데와 길목이 최대 16 어긋나는데, 도착 판정이 **8**(거리² 64)이다.
                //   한가운데로 걸어가면 영영 도착이 안 서서 다음 길목으로 못 넘어간다
                for (const auto& w : waypoints_) {
                    if (static_cast<int>(w.x / kWallCell) != x1) continue;
                    if (static_cast<int>(w.y / kWallCell) != y1) continue;
                    outX = w.x;
                    outY = w.y;
                    return w.id;
                }
                // 칸은 찍혔는데 길목이 없다 — 있을 수 없지만, 있으면 물결을 계속 돌린다
            }
            for (int i = 0; i < 4; ++i) {
                const int x2 = x1 + kNb4[i][0], y2 = y1 + kNb4[i][1];
                AIIndexNode* p = AIPathIndexAt(x2, y2);
                if (!p || p->visit == aiIndexLast_) continue;
                p->visit = aiIndexLast_;
                // ★ 정답지의 막힘 관문은 `sub_50B870`(벽 모서리 번호)·`sub_50C830`(물칸)인데
                //   둘 다 아직 안 옮겼다. **우리가 가진 벽 깃발**로 대신 막는다 —
                //   안 막으면 물결이 벽을 넘어가 **담 밖 길목까지 찾아낸다**(시험이 잡았다)
                if (WallDirAt(x2, y2) != -1) continue;       // sub_50B870 — 벽
                const int nn = AIPathNewVisit(x2, y2, it, next);
                if (nn < 0) return 0;                        // 자국 자리를 다 썼다
                next = nn;
            }
        }
        wave = next;
    }
    return 0;
}

// ── 묶음 6-e(2b) 대각 이음매 ──────────────────────────────────────────
//
// 정답지 `Sub_57B500`(`server/wall.go:839`) — 그 칸 벽의 **방향 번호**(0~10)를 준다.
//   벽이 없거나 격자 밖이면 **−1**. 창·비밀벽·못 여는 문도 −1 이다
//   ★ 9-a — **반 넘게 열린 비밀벽(칸 > 11)은 −1** 로 들어갔다. 창·문 깃발은 아직 없다
int Server::WallDirAt(int x, int y) const {
    if (x < 0 || x >= 256 || y < 0 || y >= 256) return -1;
    if (wallTiles_.empty()) return -1;
    const WallTile& w = wallTiles_[static_cast<size_t>(y) * 256 + static_cast<size_t>(x)];
    if (!w.present) return -1;
    if (w.secret >= 0 && static_cast<size_t>(w.secret) < secretWalls_.size() &&
        secretWalls_[static_cast<size_t>(w.secret)].anim > 11) return -1;
    return static_cast<int>(w.dir);
}

// 길 훑기 cases 4~7 — **대각으로 갈 때 두 옆칸의 벽 방향이 그 대각을 막는가**.
//   정답지 `nox_xxx_pathFind_50BA00`(`src/unit_ai_path.go:135~200`) 그대로.
//   ★ 번호 집합이 방향마다 다르다 — 벽은 **칸 대각선 선분**이라 어느 대각을 막는지가 방향에 달렸다
bool Server::AIPathDiagOk(int ii, int x1, int y1) const {
    auto in = [](int v, int a, int b, int c, int d) {
        return v == -1 || v == a || v == b || v == c || v == d;
    };
    switch (ii) {
        case 4: {   // (+1,+1)
            const int a = WallDirAt(x1, y1 + 1);
            if (!in(a, 1, 6, 10, 9)) return false;
            const int b = WallDirAt(x1 + 1, y1);
            if (!in(b, 1, 4, 7, 8)) return false;
            return true;
        }
        case 5: {   // (−1,−1)
            const int a = WallDirAt(x1 - 1, y1);
            if (!in(a, 1, 6, 10, 9)) return false;
            const int b = WallDirAt(x1, y1 - 1);
            if (!in(b, 1, 4, 7, 8)) return false;
            return true;
        }
        case 6: {   // (−1,+1)
            const int a = WallDirAt(x1, y1 + 1);
            if (!in(a, 0, 5, 9, 8)) return false;
            const int b = WallDirAt(x1 - 1, y1);
            if (!in(b, 0, 3, 10, 7)) return false;
            return true;
        }
        case 7: {   // (+1,−1)
            const int a = WallDirAt(x1 + 1, y1);
            if (!in(a, 0, 5, 9, 8)) return false;
            const int b = WallDirAt(x1, y1 - 1);
            if (!in(b, 0, 3, 10, 7)) return false;
            return true;
        }
        default: return true;
    }
}

// sub_547DB0 — **점이 그 몸 안에 드는가**. 원은 거리제곱, 상자는 **마름모 네 변**이다.
//   (묶음 4-a2 에서 날아가는 것 닿음 판정에 인라인으로 있던 것을 함수로 뽑았다)
bool Server::BodyContains(const PhysBody& o, float qx, float qy) const {
    if (o.shape == 2) {
        const float dx = o.x - qx, dy = o.y - qy;
        return dy * dy + dx * dx <= o.r * o.r;
    }
    if (o.shape == 3) {
        const float v5 = o.boxLT + o.x, v6 = o.boxLB + o.y;
        if ((v6 - v5 + qx - qy) * 0.70709997f < 0.0f &&
            (o.boxRB + o.y - (o.boxRT + o.x) + qx - qy) * 0.70709997f > 0.0f) {
            const float v7 = o.boxRT + o.x, v8 = o.boxRB + o.y;
            return (v8 + v7 - qx - qy) * 0.70709997f > 0.0f && (v6 + v5 - qx - qy) * 0.70709997f < 0.0f;
        }
    }
    return false;
}

// ── 묶음 6-e(2b·3) 오브젝트 모양 찍기 ────────────────────────────────
//
// 정답지 `Sub50AFA0`(`server/object_ai_path.go:545~580`) — 클래스 **0x400000** 인 것의 몸이
// **걸치는 칸마다** 깃발 **0x1** 을, 오브젝트 깃발 **0x10 이 없으면 0x2** 도 찍는다.
//
// ★ 어떻게 「걸친다」를 보나 — 칸 안의 **아홉 점**(blob `0x587000+234108`,
//   {2.3 · 11.5 · 20.7} 의 3×3)을 차례로 눌러 보아 **하나라도 몸 안에 들면** 그 칸을 찍는다.
//   ⚠ 그래서 **칸보다 작은 것은 칸 한가운데를 비켜 서면 안 찍힐 수 있다** — 정답지 그대로다.
void Server::AIPathBakeObstacles() {
    static const float kS[3] = { 2.3f, 11.5f, 20.7f };
    for (const auto& o : obstacles_) {
        if (!o.hasCollide) continue;
        double ox0, oy0, ox1, oy1;
        BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
        const int gx0 = static_cast<int>(ox0 / kWallCell), gx1 = static_cast<int>(ox1 / kWallCell);
        const int gy0 = static_cast<int>(oy0 / kWallCell), gy1 = static_cast<int>(oy1 / kWallCell);
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
                AIIndexNode* n = AIPathIndexAt(gx, gy);
                if (!n) continue;
                bool hit = false;
                for (int a = 0; a < 3 && !hit; ++a)
                    for (int b = 0; b < 3 && !hit; ++b)
                        if (BodyContains(o, gx * 23.0f + kS[b], gy * 23.0f + kS[a])) hit = true;
                if (!hit) continue;
                n->flags |= 0x1;
                if (!(o.flags & 0x10)) n->flags |= 0x2;
            }
    }
}

// ── 묶음 7-a 경험치와 레벨 ────────────────────────────────────────────
//
// 정답지 `NetReportExperience`(`server/network.go:724` = `sub_4D81A0`) ·
//        `nox_xxx_plyrGiveExp_4EF3A0`·`sub_4EF2E0_exp_level`(`legacy/server__gamemech__explevel.c`).
//
// ⭕ 와이어 — `6e <경험치 u32>` **5바이트** · `NetSendPacketXxx0(…, 0, 1)` 이라 **중요**.
//   ★ 경험치는 오브젝트 안에서 **float** 인데 와이어에는 **u32 로 잘라** 싣는다
//   ★ **순번(`cc`)은 안 붙는다.** `NetSendPacketXxx0` 은 여섯째 인자를 **0** 으로 넘기고
//     (`server/network.go:101`), `netImportant_4E5770` 이 그 자리(+184)를 보고 갈라
//     0 이면 `27 <시각> aa` 봉투에 **본문 그대로**, 1 이면 `cc <순번> <길이>` 를 씌운다(`GAME3_3.c:2024`).
//     그래서 우리도 `EmitSeq` 가 아니라 `EmitImportantRaw` 다 — 확인(`ab`)을 안 기다리고,
//     다시 실려도 경험치는 **더하기가 아니라 절댓값**이라 같은 값이 한 번 더 들어올 뿐이다
//
// ⭕ 레벨 — `XPTable` 의 **다음 칸 값 이하**로 경험치가 차면 한 단계 오른다.
//   ⚠ **한 번에 한 단계만** 오른다(정답지가 `while` 이 아니라 `if` 다) — 크게 한 번 받아도 한 칸이다
//   ⚠ 천장은 **10**(`legacy/defs.h:26`)
//   ⚠ 칸 번호는 **0부터**고 gamedata.bin 이 스스로 `First entry is for level "0"` 이라 적어 두었다.
//     그런데 코드가 보는 것은 `XPTable[레벨 + 1]` 이라, **레벨 1 이 다음으로 가려면 셋째 칸(18200)** 이다.
//     ⭕ 표의 뜻과 코드가 한 칸 어긋나 보이지만 **코드 쪽을 그대로 옮긴다**(`sub_4EF2E0` 원문)
//
// ⛔ 정답지가 더 하는데 **여기서는 안 옮기는 것** 넷 —
//   ① `sub_56FA40`·`sub_56F980` — 경험치·레벨의 **XOR 그림자 사본**(`GAME5_2.c:616`). 메모리 조작 잡는 장치라 서버 규칙이 아니다
//   ② `gameGet_4DB1B0() && sub_4DB1C0()` 관문 — **혼자 하기 맵 갈아타는 중**이면 레벨을 안 올린다.
//      두 값은 글자 고르는 화면이 세운다(`gui_selchar.go:1000`) — 붙어 있는 서버엔 없는 상태라 관문이 늘 열려 있다
//   ③ 글줄 "health.c:gainpoints"·"LevelUP" — 문자열표를 타는 글이라 묶음 10 으로
//   ④ 소리 902 — 묶음 10 으로
//
// ⚠ **죽여서 받는 경험치는 없다.** `plyrGiveExp` 를 부르는 곳은 **스크립트**(`builtin.c:355`)뿐이다 —
//   퀘스트가 주는 것이지 몬스터를 잡아서 저절로 오르지 않는다. 그래서 여기서는 **주는 길만** 옮긴다
void Server::SendExperience(Session& s, std::vector<OutPacket>& out) {
    const uint32_t xp = static_cast<uint32_t>(s.experience);
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_EXPERIENCE);
    msg[1] = static_cast<uint8_t>(xp);
    msg[2] = static_cast<uint8_t>(xp >> 8);
    msg[3] = static_cast<uint8_t>(xp >> 16);
    msg[4] = static_cast<uint8_t>(xp >> 24);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// sub_4EF2E0_exp_level — 한 단계 올릴 수 있으면 올린다(**한 단계만**)
bool Server::PlayerCheckLevel(Session& s) {
    if (s.level >= kPlayerMaxLevel) return false;
    const size_t next = static_cast<size_t>(s.level) + 1;
    if (next >= sizeof(kXPTable) / sizeof(kXPTable[0])) return false;
    if (kXPTable[next] > s.experience) return false;
    ++s.level;
    // 10-b PlaySound 902 LevelUp — pausefx.c:59 uses kind 2 (only that player hears it)
    //   ★ `aud_501960` 은 **오브젝트**를 받는다 — 그 오브젝트가 받는 사람이면 와이어가 **0xA7** 로 바뀜다
    PlaySoundAt(902, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
    // `plrReadVals_4EEDC0(a1, 1)` — 딸린 값 다시 셈.
    //   ⚠ 우리 온라인 값은 **직업으로 고정**이라(접속 때 `HealthOnline` 따위) 레벨로 바뀌는 것이 없다.
    //     그래서 다시 셀 것이 없고 **「바뀌었다」 표시만** 세운다(스냅샷 꼬리가 싣고 끈다)
    s.statsDirty = true;
    return true;
}

void Server::PlayerGiveExp(Session& s, float amount, std::vector<OutPacket>& out) {
    s.experience += amount;
    SendExperience(s, out);
    PlayerCheckLevel(s);
}

// ── 묶음 7-b 주문 받기 ────────────────────────────────────────────────
//
// 정답지 `nox_xxx_spellGrantToPlayer_4FB550`(`legacy/server__magic__plyrspel.c:46`) ·
//        `nox_xxx_netSendSpellAward_4D7F90`(`GAME3_2.c`).
//
// ⭕ 와이어 — `6f <주문 번호> <그 주문의 지금 레벨> <알림 | 0x80>` **4바이트**.
//   ★ `netSendPacket1_4E5390(…, 0, 1)` — **Packet1** 이라 여섯째 인자가 **1**,
//     즉 `cc <순번> <길이>` **순번 봉투**에 들어간다. 경험치(`6e`)와 반대다 —
//     주문 레벨은 **놓치면 안 되는 상태**라 확인(`ab`)을 받아야 한다
//   ★ 셋째 칸은 **보낼 때 읽은 레벨**이다(올린 **뒤**). 클라가 그 값을 그대로 제 표에 넣는다
//     (`netSpellRewardCli_45CFE0` — `SpellLvl[번호] = a2`)
//   ★ 넷째 칸 — 아래 7비트가 `알림`(0 이면 조용히), **0x80** 이 넷째 인자다.
//     클라는 `& 0x80` 을 따로 떼어 넘긴다(`cdecode.c:1551~1559`)
//
// ⭕ 올리는 규칙
//   · 주문 번호는 **1~136**. 벗어나면 "AwardSpellError" 글줄만 보내고 실패
//   · **천장** — 협동(2048)·퀘스트(4096)면 **3**, 그 밖은 **5**. 이미 천장이면 "MaxSpellLevel" 글줄만
//   · 한 칸 올리고(`++`) 천장으로 자른다 · `forceLevel` 이 있으면 **그 값으로 덮는다**
//   · **무리 주문** — 깃발 `0x1000`/`0x4000`/`0x10000` 을 가진 주문을 받으면
//     짝이 되는 `0x2000`/`0x8000`/`0x20000` 을 가진 **성한 주문 전부**가 같이 오른다
//     (『보호 무리』처럼 한 장에 여러 개가 묶인 것)
//   · 알림이면 소리 226 과 보상 알림을 **나와 남들 모두**에게 → 둘 다 묶음 10(소리)·미이식
//
// ⚠ 우리 서버는 **온라인 아레나**라 협동·퀘스트 깃발이 꺼져 있다 ⇒ 천장은 늘 **5**.
//   퀘스트 전용 「한 번만 받는 주문 17종」 관문도 같은 이유로 안 탄다(그래도 옮겨 둔다)
void Server::SendSpellAward(Session& s, uint8_t spell, uint8_t announce, bool extra,
                            std::vector<OutPacket>& out) {
    uint8_t msg[4];
    msg[0] = static_cast<uint8_t>(Op::REPORT_SPELL_AWARD);
    msg[1] = spell;
    msg[2] = s.spellLvl[spell];                          // 올린 **뒤**의 레벨
    msg[3] = static_cast<uint8_t>(extra ? (announce | 0x80) : announce);
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

// 퀘스트에서 **한 번만** 받는 주문들(`plyrspel.c:80~82`) — 이미 1 이상이면 더 안 준다
static bool SpellQuestOnce(uint8_t sp) {
    switch (sp) {
        case 34: case 134: case 45: case 46: case 47: case 48: case 49:
        case 117: case 118: case 119: case 120: case 121: case 122:
        case 123: case 124: case 125: case 19:
            return true;
        default: return false;
    }
}

bool Server::SpellGrantToPlayer(Session& s, uint8_t spell, uint8_t announce, bool extra,
                                uint8_t forceLevel, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137) return false;        // "AwardSpellError"(글줄은 묶음 10)
    const bool coopOrQuest = questTotal_ > 0 || questStage_ > 0;   // 2048·4096 자리
    const uint8_t cap = coopOrQuest ? 3 : 5;
    if (s.spellLvl[spell] == cap) return false;          // "MaxSpellLevel"
    // 퀘스트면 목록의 17종은 한 번만
    if (coopOrQuest && SpellQuestOnce(spell) && s.spellLvl[spell] != 0) return false;

    // 10-b PlaySound 226 AwardSpell — plyrspel.c:142 rings it only when announce is set
    // ★ 2026-09-16 실측 — **이 소리는 실제로는 한 번도 안 나간다.** 그래도 맞는 코드다.
    //   `thing.bin` 의 `AUD ` 칸 **927개에 `AwardSpell` 이라는 이름이 아예 없다**
    //   (도구 `scratchpad/p31/aud.cpp`. 이름표는 1,023개인데 AUD 는 927개이고,
    //    `AwardSpell`·`AwardGuide`·`PlasmaSustain` 따위 96개는 **클라가 제 손으로 내는 소리**다).
    //   정답지도 표를 같은 파일에서 만드니 그쪽에서도 안 울린다 ⇒ **어긋남 아님.**
    //   ⚠ 진행 기록의 「묶음 7-b ⛔ 안 옮긴 것 — 소리 226」은 **두 겹으로 틀린 메모**였다:
    //     우리는 부르고 있고, 부르는 것이 맞고, 다만 표에 없어 버려진다.
    //   ⇒ 로그의 `[소리] … 표없어버림 N — 버린 번호 226(AwardSpell)` 은 **정상 지문**이다
    if (announce) PlaySoundAt(226, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
    ++s.spellLvl[spell];
    if (s.spellLvl[spell] > 5) s.spellLvl[spell] = 5;
    if (coopOrQuest && s.spellLvl[spell] > 3) s.spellLvl[spell] = 3;
    if (forceLevel) s.spellLvl[spell] = forceLevel;

    // 무리 주문 — 이끄는 깃발이 있으면 딸린 깃발을 가진 성한 주문을 모두 올린다
    uint32_t member = 0;
    const uint32_t fl = SpellFlags(spell);
    if (fl & 0x1000)       member = 0x2000;
    else if (fl & 0x4000)  member = 0x8000;
    else if (fl & 0x10000) member = 0x20000;
    if (member) {
        for (int i = 1; i < 137; ++i) {
            if (!(SpellFlags(static_cast<uint8_t>(i)) & member)) continue;
            if (!spellDefs_[i].valid) continue;
            if (forceLevel) s.spellLvl[i] = forceLevel;
            else ++s.spellLvl[i];
            if (s.spellLvl[i] > 5) s.spellLvl[i] = 5;
            if (coopOrQuest && s.spellLvl[i] > 3) s.spellLvl[i] = 3;
        }
    }
    SendSpellAward(s, spell, announce, extra, out);
    return true;
}

// ── 묶음 7-c 점수와 죽은 횟수 ─────────────────────────────────────────
//
// 정답지 `nox_xxx_netReportLesson_4D8EF0`(`GAME3_2.c:4471`) = `NetReportLesson`(`server/network.go:369`) ·
//        `nox_xxx_playerUpdateScore_54D980`(`GAME5.c:5221`) · `changeScore_4D8E90` ·
//        `playerSubLessons_4D8EC0` · `playerIncrementElimDeath_4D8D40`.
//
// ⭕ 와이어 — `4e <넷코드 u16> <점수 u32> <죽은 횟수 u32>` **11바이트**.
//   ★ `netSendPacket1_4E5390(**255**, …, 0, 1)` — 첫 인자 255 라 **모두에게** 가고,
//     Packet**1** 이라 `cc` **순번 봉투**에 든다. 남의 점수도 내 화면에 떠야 하므로 그렇다
//   ★ 「Lessons」는 이름만 수업이고 실은 **점수**다(`ns.Player GetScore/ChangeScore` 가 이 자리를 읽고 쓴다).
//     둘째 값 +2140 은 **죽은 횟수**(`playerIncrementElimDeath_4D8D40`)
//
// ⭕ 아레나 셈법 — 판 갈래로 셈이 갈린다(`GAME5.c:5078~5085`):
//     **256 아레나 → `playerUpdateScore_54D980`**(우리 것) · 16 왕 → Kotr · 1024 탈락전 → ElimDeath
//   `playerUpdateScore(죽은이, 죽인이, 도운이, a4)` 의 갈래 —
//     · 죽인이가 **자기 자신**이거나 **아무도 없으면** → 죽은이 점수 **−1**. ⚠ **죽은 횟수는 안 올린다**
//     · 죽인이가 **사람이 아니면**(몬스터·함정) → 죽은 횟수 **+1** 만. 점수는 그대로
//     · 죽인이가 **사람**이고 **같은 편**이면 → 죽인이 점수 **−1** · 죽은 횟수 +1
//     · 그 밖(남) → 죽인이 점수 **+1** · 죽은 횟수 +1
//     · **도운이**가 죽인이와도 죽은이와도 편이 다르면 → 도운이 점수 **+1**(`LABEL_36`)
//   바뀐 사람마다 `4e` 를 **따로** 보낸다(한 번 죽으면 최대 세 통 — 죽인이·죽은이·도운이)
void Server::SendLesson(Session& s, std::vector<OutPacket>& out) {
    uint8_t msg[11];
    msg[0] = static_cast<uint8_t>(Op::REPORT_LESSON);
    msg[1] = static_cast<uint8_t>(s.netCode);
    msg[2] = static_cast<uint8_t>(s.netCode >> 8);
    const uint32_t sc = static_cast<uint32_t>(s.score);
    msg[3] = static_cast<uint8_t>(sc);
    msg[4] = static_cast<uint8_t>(sc >> 8);
    msg[5] = static_cast<uint8_t>(sc >> 16);
    msg[6] = static_cast<uint8_t>(sc >> 24);
    msg[7] = static_cast<uint8_t>(s.deaths);
    msg[8] = static_cast<uint8_t>(s.deaths >> 8);
    msg[9] = static_cast<uint8_t>(s.deaths >> 16);
    msg[10] = static_cast<uint8_t>(s.deaths >> 24);
    // 첫 인자 255 = 모두에게 · Packet1 = 순번 봉투
    for (auto& kv : sessions_) {
        if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    }
}

void Server::ChangeScore(Session& s, int32_t delta, std::vector<OutPacket>& out) {
    s.score += delta;                                      // +2136 (`changeScore_4D8E90` / `playerSubLessons_4D8EC0`)
    SendLesson(s, out);
}

void Server::IncrementDeaths(Session& s, std::vector<OutPacket>& out) {
    ++s.deaths;                                            // +2140 (`playerIncrementElimDeath_4D8D40`)
    SendLesson(s, out);
}

// `playerUpdateScore_54D980` — 아레나(깃발 256)의 죽음 셈
// `GAME5.c:5078~5085` — 게임 깃발로 셈을 고른다
void Server::DeathScoring(Session& victim, std::vector<OutPacket>& out) {
    Session* killer = nullptr;
    if (!victim.lastHit.player.empty()) {
        auto k = sessions_.find(victim.lastHit.player);
        if (k != sessions_.end() && k->second.inGame) killer = &k->second;
    }
    const uint32_t f = gameSettings_.gameFlags;
    if (f & 0x100u)      PlayerUpdateScore(victim, out);   // 256 아레나(7-c)
    else if (f & 0x10u)  KotrDeath(victim, killer, out);   // 16 왕 죽이기
    else if (f & 0x400u) ElimDeath(victim, killer, out);   // 1024 탈락전
    // 그 밖(깃발 판·퀘스트)은 아무 셈도 안 한다
}

void Server::PlayerUpdateScore(Session& victim, std::vector<OutPacket>& out) {
    Session* killer = nullptr;
    if (!victim.lastHit.player.empty()) {
        auto k = sessions_.find(victim.lastHit.player);
        if (k != sessions_.end() && k->second.inGame) killer = &k->second;
    }
    Session* assist = nullptr;
    if (victim.assistSet && frame_ - victim.assistFrame < 10u * kServerTickRate) {
        auto h = sessions_.find(victim.assistBy);
        if (h != sessions_.end() && h->second.inGame &&
            h->second.addr != victim.lastHit.player && h->second.addr != victim.addr) assist = &h->second;
    }
    // 죽인 것이 사람이 아니었나 — +520 사슬이 몬스터면 `!(flags & 4)` 자리다
    const bool byNonPlayer = killer == nullptr &&
                             (victim.lastHit.cls == 2 || (victim.lastHit.cls == 3 && victim.lastHit.ownerCls == 2));

    if (killer == &victim || (killer == nullptr && !byNonPlayer && assist == nullptr)) {
        ChangeScore(victim, -1, out);                      // LABEL_31 — 죽은 횟수는 **안** 올린다
        if (victim.team != 0) {                            // 8-b — 죽은 이의 팀도 한 칸 내려간다
            if (const Team* t = TeamById(victim.team))
                TeamChangeScore(victim.team, t->score - 1, out);
        }
        return;
    }
    if (killer != nullptr) {
        const bool sameTeam = victim.team != 0 && killer->team == victim.team;
        ChangeScore(*killer, sameTeam ? -1 : +1, out);
        // 8-b — 사람 점수와 **같이** 팀 점수도 움직인다(`netChangeTeamID(팀, 팀점수 ± 1)`)
        if (killer->team != 0) {
            if (const Team* t = TeamById(killer->team))
                TeamChangeScore(killer->team, t->score + (sameTeam ? -1 : +1), out);
        }
    }
    IncrementDeaths(victim, out);
    // LABEL_36 — 도운 사람은 죽인이·죽은이와 **편이 다를 때만** 받는다
    if (assist != nullptr) {
        const bool sameAsKiller = killer != nullptr && killer->team != 0 && assist->team == killer->team;
        const bool sameAsVictim = victim.team != 0 && assist->team == victim.team;
        if (!sameAsKiller && !sameAsVictim) ChangeScore(*assist, +1, out);
    }
}

// ── 묶음 7-d 능력 ─────────────────────────────────────────────────────
//
// 정답지 `serverAbilities`(`src/spells_abil.go:135~394`) ·
//        `nox_xxx_abilityRewardServ_4FB9C0`·`nox_xxx_netAbilityReport_4D8060`(`legacy`).
//
// ⭕ 와이어 넷 — 앞 셋은 **본인에게만**.
//   · `cd <능력 번호> <레벨 | 0x80(알림)>` **3바이트** — 받았다. `netSendPacket1` 이라 **순번 봉투**
//   · `ce <능력 번호> <상태>` **3바이트** — 쉬는 시간 상태. `NetSendPacketXxx0` 이라 **순번 없는 중요**
//   · `cf <능력 번호> <켜짐 0/1>` **3바이트** — 지속이 끝났다. 같이 순번 없는 중요
//   · `eb <능력 번호>` **2바이트** — 되돌림. **6 이면 전부**. 같이 순번 없는 중요
//   ★ 받았다(`cd`)만 순번이 붙는다 — **놓치면 능력이 사라지는** 값이라서다.
//     쉬는 시간·켜짐은 다음 틱이면 또 오므로 맨몸으로 간다
//
// ⭕ 능력 레벨은 **주문 레벨 표의 1~5 칸**이다(`SpellLvl[번호]`) — 따로 두지 않는다.
//   받으면 **한 번에 5** 로 올라간다(`*v6 = 5`) · 이미 있으면 "use.c:HadAbility" 만 보내고 실패
//
// ⭕ 쓰기(`Do`)의 관문 차례 — 하나라도 걸리면 그 자리에서 끝난다.
//   ① 번호 1~5 · 죽었거나 사라졌으면 · 사람이 아니면 · **워리어가 아니면** 조용히 끝
//   ② **같이 못 켜는 짝** — 광전사↔함성·작살 / 함성↔광전사·작살 / 작살↔함성·광전사 → 알림(2,2)
//   ③ 그 능력이 **이미 켜져 있으면** → 알림(2,2)
//   ④ 상태 12 이거나 (협동이 아닌데) **떠 있으면** → 알림(2,6)
//   ⑤ (온라인이 아니거나 퀘스트면) 레벨 0 이면 → 알림(2,3). ⚠ 우리는 늘 온라인이라 안 탄다
//   ⑥ 광전사인데 **짐이 넘치면**(+3656) → 알림(2,7)
//   ⑦ **쉬는 시간이 남았으면** → 알림(2,2)
//   그 뒤 쉬는 시간을 걸고(`ce` 상태 0), 지속이 0보다 크면 지속 목록에 얹고, 효과를 낸다
//
// ⛔ 효과 다섯(`do()` — 광전사·함성·작살·살금살금·늑대의 눈)은 **다음 조각(7-e)**.
//   소리(226 따위)와 문자열 글줄은 묶음 10.
void Server::SendAbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out) {
    uint8_t msg[3];
    msg[0] = static_cast<uint8_t>(Op::REPORT_ABILITY_AWARD);
    msg[1] = abil;
    msg[2] = static_cast<uint8_t>(announce ? (s.spellLvl[abil] | 0x80) : s.spellLvl[abil]);
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

void Server::SendAbilityState(Session& s, uint8_t abil, uint8_t state, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_ABILITY_STATE), abil, state };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

void Server::SendAbilityActive(Session& s, uint8_t abil, bool active, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_ACTIVE_ABILITIES), abil,
                             static_cast<uint8_t>(active ? 1 : 0) };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

void Server::SendAbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    const uint8_t msg[2] = { static_cast<uint8_t>(Op::RESET_ABILITIES), abil };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// `nox_xxx_abilityRewardServ_4FB9C0` — 능력 하나를 준다
bool Server::AbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out) {
    if (abil == 0 || abil >= 6) return false;            // "AwardAbilityError"(글줄은 묶음 10)
    if (s.spellLvl[abil] != 0) return false;             // "use.c:HadAbility"
    s.spellLvl[abil] = 5;                                // `*v6 = 5` — 한 번에 천장까지
    SendAbilityAward(s, abil, announce, out);
    return true;
}

bool Server::AbilityIsActive(const Session& s, uint8_t abil) const {
    return abil < kAbilityCount && s.abilActive[abil];
}

// `serverAbilities::Do` — 능력을 쓴다
bool Server::AbilityDo(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    // 10-g PlaySound 231 PermanentFizzle — spells_abil.go rings it on EVERY rejection path
    //   (never on success). InformSpell(2, …) marks exactly those paths, so ring it there.
    auto refuse = [&](uint8_t why) { InformSpell(s, 2, why, out);
                                     PlaySoundAt(231, static_cast<float>(s.posX),
                                                 static_cast<float>(s.posY), 0, 0, s.addr); };
    // ★ 09-19 — **갈래를 갈라 센다.** 능력이 안 먹을 때 「받긴 했나 / 어디서 막혔나」가
    //   한 줄도 안 남아 있었다(로그에 능력 흔적 0). 반쪽 계수기는 없는 것보다 나쁘다
    ++abilCalls_;
    if (abil == 0 || abil >= 6) { ++abilBadNum_; return false; }
    // ★ 09-23 — 여기부터는 **능력별 칸**에 센다(`AbilGate`). 번호가 1~5 로 보장된 뒤라 칸 번호로 쓸 수 있다
    ++abilIn_[abil];
    auto gate = [&](AbilGate g) { ++abilGate_[abil][static_cast<size_t>(g)]; };
    if (s.respawnAtFrame != 0) { gate(AbilGate::Dead); return false; }             // 죽었거나 사라진 것
    if (s.charClass != CharClass::Warrior) { gate(AbilGate::NotWarrior); return false; } // 워리어만
    // ② 같이 못 켜는 짝 — 광전사·함성·작살 셋은 서로를 막는다(`spells_abil.go:154~181`)
    //   ✖ 09-23 2-a1 정정 — 전에는 「정답지가 `IsActiveVal`·`IsActive` 를 섞어 쓰지만 얹을 때 늘 `Active: 1` 이라 둘이 같다」고
    //     보고 하나로 합쳤다. 그런데 함성 애니가 끝나면 `Sub4FC440` 이 값만 0 으로 내린다(목록엔 남는다) — 둘은 **다르다**.
    //     그래서 정답지대로 가른다: 광전사는 함성 **값**·작살 켜짐 · 함성은 광전사·작살 켜짐 · 작살은 함성 **값**·광전사 켜짐.
    //     ⇒ 함성 애니가 끝난 뒤(값 0)에는 광전사·작살을 **안 막는다**(함성 지속 120 전부를 막던 전과 다르다)
    //   ⚠ 광전사의 CTF 깃발 검사(`:155~163`)는 아직 없다
    // ★ 09-23 2-b1 — **깃발을 든 채로는 광전사를 못 쓴다**(`spells_abil.go:155~163` — 깃발 판 · 가진 것 중 FLAG 클래스) → 알림(2,5) · 231.
    //   우리는 깃발을 가방이 아니라 `flags_[].carrier` 로 든다 — 같은 뜻이다(`Carrying`). 겹침보다 **먼저** 본다(정답지 순서)
    if (abil == 1 && (gameSettings_.gameFlags & 0x20u) && Carrying(s.addr)) { gate(AbilGate::CtfFlag); refuse(5); return false; }
    uint8_t blocker = 0;
    switch (abil) {
        case 1: blocker = AbilityIsActiveVal(s, 2) ? 2 : (AbilityIsActive(s, 3) ? 3 : 0); break;
        case 2: blocker = AbilityIsActive(s, 1) ? 1 : (AbilityIsActive(s, 3) ? 3 : 0); break;
        case 3: blocker = AbilityIsActiveVal(s, 2) ? 2 : (AbilityIsActive(s, 1) ? 1 : 0); break;
        default: break;                                      // 살금살금·늑대의 눈 — 짝이 없다
    }
    if (blocker != 0) {
        gate(AbilGate::Exclusive);
        ++abilBlockedBy_[abil][blocker];                     // ★ 0-b — **누가** 막았나
        refuse(2); return false;
    }
    if (AbilityIsActive(s, abil)) { gate(AbilGate::Already); refuse(2); return false; }   // ③
    // ④ 상태 12 이거나 (협동이 아닌데) **떠 있으면**(`spells_abil.go:187`)
    //   ★ 3-a — 떠 있음은 점프가 켠다. 상태가 12 가 아닌데 떠 있는 때 = 점프 중 달리기(8) 버프가 상태를 5 로 바꾼 틱 따위
    if (s.moveState == 12 || (!(gameSettings_.gameFlags & 0x800u) && s.airborne)) { gate(AbilGate::MoveState); refuse(6); return false; }
    // ⑤ (온라인이 아니거나 퀘스트면) 레벨 0 은 못 쓴다 — ⚠ 우리는 늘 온라인이라 이 관문을 안 탄다
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    if (quest && s.spellLvl[abil] == 0) { gate(AbilGate::NotLearned); refuse(3); return false; }
    if (abil == 1 && s.overweight) { gate(AbilGate::Overweight); InformSpell(s, 2, 7, out); return false; }   // ⑥ 짐 넘침(+3656)
    if (s.abilCooldown[abil] != 0) { gate(AbilGate::Cooldown); refuse(2); return false; }   // ⑦

    gate(AbilGate::Ok);
    s.abilCooldown[abil] = kAbilityDelay[abil];
    if (kAbilityDelay[abil] != 0) SendAbilityState(s, abil, 0, out);
    if (kAbilityDuration[abil] > 0) {
        s.abilActive[abil] = true;
        s.abilActiveVal[abil] = true;                     // ★ 2-a1 — 얹을 때 `Active: 1`
        s.abilActiveUntil[abil] = frame_ + kAbilityDuration[abil];
    }
    AbilityEffect(s, abil, out);                          // 7-e — `do()`
    // ★ 09-20 묶음 10 — **쓸 때 소리**(정답지 `spells_abil.go:225~226` — `getSound(능력,0)`).
    //   소리 번호는 thing.bin `ABIL` 칸에서 읽어 넣는다(`SetAbilitySound`) — 지어내지 않는다
    if (abilSoundUse_[abil] != 0) {
        PlaySoundAt(abilSoundUse_[abil], static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, std::string(), Target{ s.addr, -1 });
        ++abilSoundUseCalls_[abil];
    }
    return true;
}

// `serverAbilities::Update` — 틱마다 쉬는 시간을 줄이고, 지속이 다한 것을 끈다
void Server::AbilityUpdate(Session& s, std::vector<OutPacket>& out) {
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (s.abilCooldown[i] == 0) continue;
        if (--s.abilCooldown[i] == 0) SendAbilityState(s, i, 1, out);   // 다 쉬었다
    }
    const bool gone = s.respawnAtFrame != 0;
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (!s.abilActive[i]) continue;
        // ⚠ 죽었거나 사라졌으면 **알리지 않고** 목록에서만 뺀다(정답지 `if (!obj.Flags().HasAny(...))`)
        if (!gone) {
            if (frame_ <= s.abilActiveUntil[i]) continue;               // 아직 남았다
            SendAbilityActive(s, i, false, out);
            // ★ 09-20 묶음 10 — **꺼질 때 소리**(정답지 `spells_abil.go:249~250` — `getSound(능력,2)`)
            if (abilSoundOff_[i] != 0) {
                PlaySoundAt(abilSoundOff_[i], static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, std::string(), Target{ s.addr, -1 });
                ++abilSoundOffCalls_[i];
            }
            if (i == 1) PlayerSetState(s, 13, frame_);                  // 광전사는 끝나면 상태 13
        }
        ++abilEnded_[i][static_cast<size_t>(gone ? AbilEnd::Gone : AbilEnd::Expired)];   // ★ 0-b
        s.abilActive[i] = false;
        s.abilActiveVal[i] = false;
        s.abilActiveUntil[i] = 0;
    }
}

// `serverAbilities::ResetAbility` — 하나를 되돌린다
void Server::AbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    if (abil >= kAbilityCount) return;
    s.abilCooldown[abil] = 0;
    SendAbilityReset(s, abil, out);
    if (s.abilActive[abil]) {
        ++abilEnded_[abil][static_cast<size_t>(AbilEnd::Reset)];   // ★ 0-b
        SendAbilityActive(s, abil, false, out);
        s.abilActive[abil] = false;
        s.abilActiveVal[abil] = false;
        s.abilActiveUntil[abil] = 0;
    }
}

// `serverAbilities::CancelAbilities` — 전부 되돌린다. ★ `eb` 에 **6**(전부)을 싣는다
void Server::AbilityCancelAll(Session& s, std::vector<OutPacket>& out) {
    for (uint8_t i = 0; i < kAbilityCount; ++i) s.abilCooldown[i] = 0;
    SendAbilityReset(s, 6, out);
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (!s.abilActive[i]) continue;
        ++abilEnded_[i][static_cast<size_t>(AbilEnd::CancelAll)];  // ★ 0-b
        SendAbilityActive(s, i, false, out);
        s.abilActive[i] = false;
        s.abilActiveVal[i] = false;
        s.abilActiveUntil[i] = 0;
    }
}

// ★ 09-23 nine-defects 1-a — `serverAbilities::DisableAbility`(`spells_abil.go:325~344`) — **동작이** 능력을 끈다.
//   정답지 순서 — ① 능력별 뒷정리(작살: 끊김 와이어 · 살금살금: 발소리 죽이기(31) 끄기 · 늑대의 눈: **아무것도 안 하고 돌아감**)
//   ② `cf <번호> 0`(`netAbilReportActive`) ③ 켜짐 목록에서 뺀다(`DisableAbilityAaa`).
//   ⚠ 꺼질 때 소리(`getSound(능력,2)`)는 **안 낸다** — 그건 지속이 다할 때(`Update`)만 낸다. 쉬는 시간도 안 건드린다
//   ⚠ 작살은 `HarpoonBreak` 가 이 일(끊김 와이어 + 켜짐 끄기)을 이미 한다 — 작살 줄을 끊을 때는 그쪽을 부른다
//     (정답지도 `breakForOwner` → `DisableAbility(작살)` 순이다). 지금 이 함수를 부르는 자리는 살금살금 하나다
void Server::AbilityDisable(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    if (abil == 0 || abil >= kAbilityCount) return;          // `abil.Valid()`
    switch (abil) {
        case 4:                                              // 살금살금 — `DisableEnchant(ENCHANT_SNEAK)`
            BuffOff(Target{ s.addr, -1 }, 31);
            break;
        case 5:                                              // 늑대의 눈 — 정답지가 여기서 **돌아간다**(알림도 목록 빼기도 없음)
            return;
        default:                                             // 광전사·함성·작살 — 뒷정리 없음(작살은 위 ⚠)
            break;
    }
    SendAbilityActive(s, abil, false, out);
    if (s.abilActive[abil]) ++abilEnded_[abil][static_cast<size_t>(AbilEnd::Disabled)];
    s.abilActive[abil] = false;
    s.abilActiveVal[abil] = false;
    s.abilActiveUntil[abil] = 0;
}

// ★ 09-23 nine-defects 2-b2 — `nox_xxx_unitMove_4E7010` 의 사람 몫(`object.go:693~731` SetPos).
//   NewPos·PosVec·PrevPos 를 모두 그 자리로 · 온라인이면 `ea FORGET_DRAWABLES`(`:722~731`).
//   ⚠ `TeleportTarget`(teleportToMB)과 달리 **속도를 안 지운다** — SetPos 에 속도 줄이 없다
void Server::PlayerSetPos(Session& s, double x, double y, std::vector<OutPacket>& out) {
    s.posX = x; s.posY = y;
    s.x = static_cast<int>(std::lround(x)); s.y = static_cast<int>(std::lround(y));
    s.prevX = x; s.prevY = y;
    s.setPosThisTick = true;                                 // 틱 끝 PrevPos 도 이 자리(부딪힘 틱 끝 줄)
    ForgetDrawables(s, out);
}

// ★ 09-23 nine-defects 2-b2 — `PlayerCollide`(`GAME3_3.c:3165`)의 **광전사 갈래**(`:3188~3253`).
//   부딪힌 짝마다(`nox_xxx_collide_548740`) 사람 쪽에서 부른다. other 가 null 이면 벽이다.
//   ① 켜짐 목록에 광전사(1)가 없으면 아무것도 안 한다(`:3188`).
//      ✖ 09-26 정정 — 「그 뒤 `LABEL_26`(죽음 넘기기)은 이 조각 밖」이었다. M2 에서 옮겼다 — 이 함수를 감싸는 `PlayerCollide`(아래) 끝
//   ② 상대가 있으면 지나갈지 부딪힐지 가른다(`:3194~3211`) —
//        유닛 아님·죽은 유닛 + IMMOBILE 아님 + 깃발 0x80 아님 + 무게 ≤ 나  → 지나감(가벼운 것)
//        문 아님 → 밑에 깔림·겹쳐도 됨(0x9)·투사체(클래스 1)면 지나감, 아니면 부딪힘
//        문 → 잠겼으면 부딪힘, 아니면 지나감
//   ③ 부딪힘(`LABEL_14` `:3212~3253`) — 상태 13 · 땅울림 10(+56 자리) · 광전사 끄기(`sub_4FC300` = DisableAbility)
//        상대: IMMOBILE 아니면 **탄성 충돌**(`sub_4E86E0`) · 상대 피해 함수(때린 이 = 나 · 피해 BerserkerDamage · 종류 2)
//              상대가 유닛·생성기(0x20006)면 **되돌아가고 끝**(기절·자기 피해 없음) · 아니면 나를 기절(5) BerserkerStunDuration · 세기 5
//        벽:   마지막 닿은 벽(+296)이 있고 그 재질 깃발 &5 가 0 이면 **되돌아가고 끝**
//              아니면 소리 171 · 기절 · 벽 피해(⚠ `damageToMap` 은 묶음 9 — 안 한다)
//        그다음 자기 피해 = BerserkerPainRatio × 지금 체력(최소 1 · `unitDamageClear` — 무적 안 봄) → 되돌아감(+72 PrevPos)
//   ⚠ 옮기지 않은 것 — 장애물 피해(우리 장애물엔 체력이 없다) · 장애물·소환수 밀려남(장애물은 전부 IMMOBILE 이고,
//     소환수 속도는 AI 가 매 틱 새로 세운다 — 나만 튕긴다) · 벽 피해(9)
void Server::PlayerBerserkCollide(Session& s, const ContactRef* other, std::vector<OutPacket>& out) {
    if (!AbilityIsActive(s, 1)) return;
    // 상대의 클래스·깃발·무게·죽음 — 사라진 상대(밀린 소환수 자리)는 부르지 않는다(세기 전에 거른다)
    uint32_t cls = 0, flags = 0;
    double mass = 0.0;
    bool deadUnit = false;
    Session* os = nullptr;
    int petIdx = -1;
    if (other) {
        if (other->kind == 1) {
            auto it = sessions_.find(other->addr);
            if (it == sessions_.end() || !it->second.inGame) return;
            os = &it->second;
            cls = kClsPlayer; flags = PlayerObjFlags(*os);   // NewPlayer FLAGS NULL · ★ 3-a 떠 있음(아래 갈래는 0x80·0x9 만 본다)
            mass = PlayerMassOf(*os);
            deadUnit = os->health == 0;                      // `!*v3 && v3[2]` — 이번 틱 앞 짝에서 죽었을 수 있다
        } else if (other->kind == 2) {
            if (other->idx < 0 || static_cast<size_t>(other->idx) >= pets_.size()) return;
            petIdx = other->idx;
            const Creature& k = pets_[static_cast<size_t>(petIdx)];
            auto tb = typeBodies_.find(k.type);
            if (tb == typeBodies_.end()) return;             // 몸 모르는 종류는 짝이 안 생긴다(위 적분)
            cls = tb->second.cls; flags = tb->second.flags; mass = tb->second.mass;
            deadUnit = k.health == 0;
        } else if (other->kind == 3) {
            if (other->idx < 0 || static_cast<size_t>(other->idx) >= obstacles_.size()) return;
            const PhysBody& o = obstacles_[static_cast<size_t>(other->idx)];
            cls = o.cls; flags = o.flags; mass = o.mass;
        } else if (other->kind == 4) {
            // ★ 09-29 Z1-A — 문(UpdateProps 의 문짝 닿음). 몸을 모르면 DOOR+IMMOBILE · 무게 1(thing.bin 에 MASS 없음 → 정답지 기본 1)
            if (other->idx < 0 || static_cast<size_t>(other->idx) >= props_.size()) return;
            const WorldProp& d = props_[static_cast<size_t>(other->idx)];
            if (d.kind != PropKind::Door) return;
            auto tb = typeBodies_.find(d.type);
            if (tb != typeBodies_.end()) { cls = tb->second.cls; flags = tb->second.flags; mass = tb->second.mass; }
            else { cls = kClsDoor | 0x400000u; flags = 0; mass = 1.0; }
        } else {
            return;
        }
    }
    ++berserkCollideCalls_;
    auto hit = [&](BerserkHit h) { ++berserkHit_[static_cast<size_t>(h)]; };
    const double myMass = PlayerMassOf(s);
    if (other) {
        const bool unit = (cls & 0x6) != 0;
        if ((!unit || deadUnit) && !(cls & 0x400000) && !(flags & 0x80) && mass <= myMass) { hit(BerserkHit::Light); return; }
        // 문 — 잠겼으면 부딪힘이다(`GAME3_3.c:3207~3210` — `*(ud+1)` 이 0 이 아니면 LABEL_14).
        //   ★ 09-29 Z1-A — 닿음은 `UpdateProps` 의 문짝 닿음에서 온다(짝 종류 4) · 잠김을 읽게 되어 두 갈래를 가른다.
        //   잠긴 문은 아래 LABEL_14 에서 `Object` 로 세고(IMMOBILE 이라 튕김 없음 · 기절 · 자기 피해) 여기서는 따로만 센다
        if (cls & kClsDoor) {
            const bool locked = other->kind == 4 && props_[static_cast<size_t>(other->idx)].lock != 0;
            if (!locked) { hit(BerserkHit::Door); return; }
            ++berserkDoorLocked_;
        }
        if ((flags & (kFlBelow | kFlAllowOverlap)) || (cls & 0x1)) { hit(BerserkHit::Overlap); return; }
    }
    // ── LABEL_14 부딪힘 ──
    PlayerSetState(s, kPlayerStateIdle, moveFrame_, &out);
    EarthquakeSend(static_cast<float>(s.tickStartX), static_cast<float>(s.tickStartY), 10, out);
    AbilityDisable(s, 1, out);
    const int stunDur = static_cast<int>(static_cast<float>(BalanceFloat("BerserkerStunDuration")));
    if (other) {
        const int dmg = static_cast<int>(static_cast<float>(BalanceFloat("BerserkerDamage")));
        if (!(cls & 0x400000)) {
            // sub_4E86E0 — 무게를 실은 탄성 충돌(`ElasticExchange`)
            double ovx = 0.0, ovy = 0.0;
            if (os) { ovx = os->velX; ovy = os->velY; }
            else if (petIdx >= 0) { ovx = pets_[static_cast<size_t>(petIdx)].velX; ovy = pets_[static_cast<size_t>(petIdx)].velY; }
            ElasticExchange(static_cast<float>(myMass), s.velX, s.velY, static_cast<float>(mass), ovx, ovy);
            if (os) { os->velX = ovx; os->velY = ovy; }      // 소환수는 적지 않는다 — 위 ⚠
        }
        AttackSrc src;
        src.x = static_cast<float>(s.prevX); src.y = static_cast<float>(s.prevY);   // a3 = 나 → +72
        // ★ 4-c3b1 — 때린 것 = 광전사 자신(a3 = a1 · `GAME3_3.c:3223`) — 사람 몸
        if (os) HurtTarget(Target{ other->addr, -1 }, dmg, out, s.addr, 2, &src, HitterPlayerBody());
        else if (petIdx >= 0) HurtTarget(Target{ std::string(), petIdx }, dmg, out, s.addr, 2, &src, HitterPlayerBody());
        if (cls & 0x20006) {
            hit(BerserkHit::Unit);
            PlayerSetPos(s, s.prevX, s.prevY, out);
            return;
        }
        hit(BerserkHit::Object);
        ApplyEnchant(Target{ s.addr, -1 }, 5, stunDur, 5);
    } else {
        const WallTile* w = nullptr;
        if (s.lastWallGx >= 0 && s.lastWallGy >= 0 && !wallTiles_.empty()) {
            const WallTile& t = wallTiles_[static_cast<size_t>(s.lastWallGy) * 256 + static_cast<size_t>(s.lastWallGx)];
            if (t.present) w = &t;
        }
        const uint32_t wf = (w && w->tile < wallDefFlags_.size()) ? wallDefFlags_[w->tile] : 0;
        if (w && !(wf & 0x5)) {
            hit(BerserkHit::WallSoft);
            PlayerSetPos(s, s.prevX, s.prevY, out);
            return;
        }
        hit(BerserkHit::WallHard);
        PlaySoundAt(171, static_cast<float>(s.tickStartX), static_cast<float>(s.tickStartY), 0, 0, s.addr);   // BerserkerCrash
        ApplyEnchant(Target{ s.addr, -1 }, 5, stunDur, 5);
        // ★ 09-29 Z1-A — `nox_xxx_damageToMap_534BC0(새 자리 ÷ 23, 100, 2, 나)`(`GAME3_3.c:3241~3245`) — 칸은 **닿은 벽이 아니라 내 새 자리(+64)** 의 칸이다.
        //   적분 중의 `posX` 가 +64 다(`tickStartX` 가 +56) · `nox_float2int` 는 이 리포 관례대로 버림(`:6215`)
        DamageToMap(static_cast<int>(static_cast<float>(s.posX) * 0.043478262f), static_cast<int>(static_cast<float>(s.posY) * 0.043478262f),
                    100, 2, WallHitterOfAddr(s.addr), out);
    }
    int pain = static_cast<int>(static_cast<float>(BalanceFloat("BerserkerPainRatio") * static_cast<double>(s.health)));
    if (pain < 1) pain = 1;
    Damage(s.addr, pain, out, std::string(), nullptr, /*ignoreInvuln=*/true);   // unitDamageClear_4EE5E0
    PlayerSetPos(s, s.prevX, s.prevY, out);
}

// ★ 09-26 M2 — `PlayerCollide`(`GAME3_3.c:3168~3269`) 전체 = 광전사 갈래(`:3188~3253` → `PlayerBerserkCollide`) + 끝의 `LABEL_26`(`:3254~3268`).
//   정답지는 광전사 갈래의 **어느 길로 나가든**(광전사가 꺼져 있어도 `:3188` 에서 곧장) 끝에 LABEL_26 을 지난다 —
//   닿은 것이 사람(클래스 4)이고 · 죽지 않았고(+16 & 0x8000 없음) · 내게 죽음(16)이 걸려 있고 · 그 남은 시간(`unitGetBuffTimer`)이 14 × FPS 미만이면
//   상대에게 죽음을 **15 × FPS · 내 세기로** 걸고(`buffApplyTo`) 내 것은 끈다(`spellBuffOff`).
//   ⇒ 넘겨받은 쪽은 15초라 14 미만 조건에 안 걸려 **곧바로 되넘기지 않는다**(짝마다 양쪽이 부른다)
//   전에는 이 몫이 없어 죽음이 걸린 사람이 몸을 비벼도 그대로 죽었다(미결 「닿으면 죽음 넘기기」)
void Server::PlayerCollide(Session& s, const ContactRef* other, std::vector<OutPacket>& out) {
    PlayerBerserkCollide(s, other, out);
    // ── LABEL_26 ──
    if (!other || other->kind != 1) return;                  // a2 & 클래스 4(사람) — 소환수·장애물·벽은 아니다
    auto it = sessions_.find(other->addr);
    if (it == sessions_.end() || !it->second.inGame) return;
    Session& o = it->second;
    if (o.respawnAtFrame != 0 || o.health == 0) return;       // +16 & 0x8000(DEAD) — 광전사 갈래가 방금 죽였을 수 있다
    if (!s.buffs.Has(16)) return;
    if (s.buffs.dur[16] >= 14 * kServerTickRate) { ++deathPass_[0]; return; }   // `< 14 * gameFPS()` 가 아니면 안 넘긴다
    const int power = s.buffs.power[16];
    ApplyEnchant(Target{ o.addr, -1 }, 16, static_cast<int>(15 * kServerTickRate), power);
    BuffOff(Target{ s.addr, -1 }, 16);
    ++deathPass_[1];
}

// ── 묶음 7-e 능력 효과 넷 ─────────────────────────────────────────────
//
// 정답지 `serverAbilities::do`(`spells_abil.go:346`) 가 부르는 다섯 중 **넷**.
//   · 광전사 돌진 `nox_xxx_warriorBerserker_53FEB0`(`spell_berserk.go:11`)
//   · 함성       `nox_xxx_warriorWarcry_53FF40`(`spell_warcry.go:13`)
//   · 살금살금   `nox_xxx_warriorTreadLightly_5400B0`(`spell_tread.go:10`)
//   · 늑대의 눈  `nox_xxx_warriorInfravis_540110`(`spell_infra.go:16`)
//   ⛔ 작살(`harpoon.Do`)은 화살 오브젝트·걸림·끌기·끊김이 딸린 한 벌이라 **다음 조각**이다.
//
// ★★ **`cf 1`(켜졌다)은 여기서 나간다.** 7-d 의 `Do` 는 목록에 얹기만 하고 안 보냈다 —
//   정답지도 넷 다 **끝에서** `netAbilReportActive(…, true)` 를 부른다.
//
// ⭕ 광전사 — 걸던 주문 전부 끊기 · (혼란이면 보는 쪽을 흔들기) · 상태 **1** ·
//   투명(0)·무적(23) 끄기 · 타원 방패(주문 51) 끊기
// ⭕ 함성 — 상태 **1** · 둘레 **300** 안의 **적**에게 무효화(주문 55)를 레벨 5 로 건다.
//   거르기 — 워리어는 건너뜀 · 관전자는 건너뜀 · 몬스터는 주문을 쓸 줄 알아야 함 ·
//   **편이 같으면 건너뜀** · 눈이 닿아야(`MapTraceVision`) 함. 끝에 투명·무적·타원 방패도 끈다
// ⭕ 살금살금 — 달리는 중(상태 5)이면 상태 **13** 으로 내리고, 발소리 죽이기(31)를 **능력 레벨**로 건다
// ⭕ 늑대의 눈 — 밤눈(21)을 **능력 레벨**로 건다
void Server::AbilityEffect(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    const Target self{ s.addr, -1 };
    switch (abil) {
        case 1: {   // 광전사 돌진
            // ✖ 09-23 2-b1 정정 — `PlayerCancelSpells`(= `playerCancelSpells_4FEAE0`, 그 사람의 지속 주문 **전부**)를 부르고 있었다.
            //   정답지는 `cancelAllSpells_4FEE90` — **붙들고 있는 주문 일곱**(24·43·35·8·22·59·67)만 끊는다(`GAME4.c:3405~3426`).
            //   우리 쪽 짝은 `CancelChannelSpells` 다(시험 `TestBerserkCharge` ⑥ 이 보호막(51)까지 끊기는 것으로 드러냄)
            CancelChannelSpells(s.addr, out);              // cancelAllSpells_4FEE90
            if (s.buffs.Has(3)) {                          // ENCHANT_CONFUSED — 보는 쪽을 흔든다
                //   `Direction2 = Dir16(Direction1 + 4 × IntClamp(-8, 8))` — 양끝 포함이라 **17가지**
                //   ⚠ 정답지는 `RoundDir` 을 안 거쳐 u16 에 260 같은 값이 남을 수 있지만,
                //     읽는 자리가 모두 한 바이트로 자르므로 우리 uint8 돌아감과 같다
                const int k = static_cast<int>(NextRandom(17)) - 8;
                s.facing = static_cast<uint8_t>(static_cast<int>(s.facingPrev) + 4 * k);
            }
            s.animFrame = 0;                               // ★ 2-b1 — `ud.Field59_0 = 0`(`spell_berserk.go:20~24`)
            PlayerSetState(s, 1, frame_);
            BuffOff(self, 0);                              // 투명
            BuffOff(self, 23);                             // 무적
            // ✖ 09-23 2-b1 정정 — 51(SHIELD) 이었다. 타원 방패(`SPELL_OVAL_SHIELD`)는 **67**(`spell_berserk.go:28`) — 함성 쪽은 2-a4 에서 고침
            DurCancelFor(67, s.addr, out);                 // 타원 방패
            break;
        }
        case 2: {   // 함성
            s.animFrame = 0;                               // ★ 2-a1 — `ud.Field59_0 = 0`(`spell_warcry.go:20~22`) · 애니 칸을 0 부터
            PlayerSetState(s, 1, frame_);
            const double dist = 300.0;
            WarcryBreakCasts(s, dist, out);                // ★ 2-a4 — `Nox_xxx_spell_4FE680(u, dist)`(`spell_warcry.go:25`) 둘레 적의 시전을 끊는다
            // ★ 09-24 H3 — 사람마다 어느 경우로 갔나를 센다(시전자 자신은 안 셈 · 합 = 본 사람 수)
            auto wp = [&](WarcryPeer k) { ++warcryPeer_[static_cast<size_t>(k)]; };
            for (auto& kv : sessions_) {
                Session& o = kv.second;
                if (&o == &s) continue;
                if (!o.inGame || o.respawnAtFrame != 0) { wp(WarcryPeer::NotInGame); continue; }
                if (o.charClass == CharClass::Warrior) { wp(WarcryPeer::Warrior); continue; }   // 워리어는 안 걸린다
                if (o.observer) { wp(WarcryPeer::Observer); continue; }                          // +3680 & 1
                if (s.team != 0 && o.team == s.team) { wp(WarcryPeer::SameTeam); continue; }      // 같은 편
                const double dx = o.posX - s.posX, dy = o.posY - s.posY;
                if (dx * dx + dy * dy > dist * dist) { wp(WarcryPeer::OutOfRange); continue; }
                if (!MapTraceVision(static_cast<float>(o.posX), static_cast<float>(o.posY),
                                    static_cast<float>(s.posX), static_cast<float>(s.posY))) { wp(WarcryPeer::NoSight); continue; }
                SpellArg arg;
                arg.obj = Target{ o.addr, -1 };
                arg.x = static_cast<float>(o.posX);
                arg.y = static_cast<float>(o.posY);
                const bool ok = SpellAccept(55, s, s.addr, arg, 5, out);   // 무효화 · 레벨 5
                wp(ok && o.buffs.Has(29) ? WarcryPeer::Applied : WarcryPeer::Rejected);
            }
            // ★ 09-23 nine-defects 2-a4 — **몬스터도** 같은 둘레에서 본다(`spell_warcry.go:45~50`) — 단 **주문을 쓸 줄 알아야**
            //   (`MonStatusCanCastSpells` = monster.bin `CAN_CAST_SPELLS`, 우리 `castingMonsterTypes_`). 전에는 사람만 봤다.
            //   편 — 소환수의 편은 주인의 편이다(주인 없는 들짐승은 편 0). 정답지 `SameAs` 도 둘 다 0 이 아니고 같을 때만 같은 편이다
            for (size_t k = 0; k < pets_.size(); ++k) {
                const Creature& c = pets_[k];
                if (c.health == 0) continue;                       // 0x8000
                if (castingMonsterTypes_.find(c.type) == castingMonsterTypes_.end()) continue;
                uint8_t team = 0;
                if (!c.owner.empty()) {
                    auto oi = sessions_.find(c.owner);
                    if (oi != sessions_.end()) team = oi->second.team;
                }
                if (s.team != 0 && team == s.team) continue;
                const double dx = c.x - s.posX, dy = c.y - s.posY;
                if (dx * dx + dy * dy > dist * dist) continue;
                if (!MapTraceVision(static_cast<float>(c.x), static_cast<float>(c.y),
                                    static_cast<float>(s.posX), static_cast<float>(s.posY))) continue;
                SpellArg arg;
                arg.obj = Target{ std::string(), static_cast<int>(k) };
                arg.x = static_cast<float>(c.x);
                arg.y = static_cast<float>(c.y);
                SpellAccept(55, s, s.addr, arg, 5, out);
                ++warcryNullMon_;
            }
            BuffOff(self, 0);
            BuffOff(self, 23);
            // ✖ 09-23 2-a4 정정 — 전에는 `DurCancelFor(51, …)` 였다. **51 은 SHIELD** 고 타원 방패(`SPELL_OVAL_SHIELD`)는 **67** 이다
            //   (`SpellNames.cpp` · 정답지 `spell_warcry.go:67`). 공격 처리(`PlayerSetState`)는 67 을 쓰고 있었다
            DurCancelFor(67, s.addr, out);
            break;
        }
        case 4: {   // 살금살금
            if (s.moveState == 5) PlayerSetState(s, 13, frame_);   // 달리는 중이면 내린다
            ApplyEnchant(self, 31, static_cast<int>(kAbilityDuration[4]), s.spellLvl[4]);
            sightForceFrame_ = frame_ + 1;                           // ★ 09-29 B1 — `frameCounterSetCopyToNextFrame_5281D0`(`spell_tread.go:24`) — 다음 프레임에 모두 다시 훑음
            break;
        }
        case 5: {   // 늑대의 눈
            ApplyEnchant(self, 21, static_cast<int>(kAbilityDuration[5]), s.spellLvl[5]);
            break;
        }
        case 3:     // 작살 — 7-f
            // ★ 작살은 **`cf 1` 을 안 보낸다.** 정답지 `harpoon.Do` 는 그것을 안 부르고,
            //   대신 틀에서 `netHarpoonAttach`(`9e 07`)로 **줄이 걸렸다**를 알린다.
            //   지속도 0 이라 지속 목록에도 안 올라간다 — 그래서 여기서 나간다
            HarpoonDo(s, out);
            return;
        default: return;
    }
    SendAbilityActive(s, abil, true, out);                 // ★ 켜졌다는 여기서 나간다
}

// ── 묶음 7-f 작살 — 쏘기와 걸림 ───────────────────────────────────────
//
// 정답지 `abilityHarpoon`(`src/spell_harpoon.go`) · `NetHarpoonAttach/Break`(`server/network.go`).
//
// ⭕ 와이어 둘 — 둘 다 `9e`(지속 주문 그림)에 **갈래 번호만 다르다**.
//   · 걸림 `9e 07 00 <주인 넷코드 u16> <화살 넷코드 u16>` **7바이트**
//   · 끊김 `9e 0e 00 <주인 넷코드 u16> <화살 넷코드 u16>` **7바이트**
//   `NetSendPacketXxx1(255, …)` 라 **모두에게** 가고 **순번 봉투**에 든다 — 줄이 남에게도 보여야 해서다
//
// ⭕ 쏘기(`Do`) — 상태 **32** 로 바꾸고 `HarpoonBolt` 을 **보는 쪽**으로 쏜다.
//   화살은 몸 반지름 **+1** 앞에서 태어나고, 빠르기는 thing.bin `SPEED` 다
//
// ⭕ 닿음(`Collide`) — 갈래가 넷 중 하나다.
//   · **벽**(대상 없음) → 그 칸에 피해 1(종류 11)을 주고 **소리 없이 끊긴다**
//   · 대상이 **죽었거나 자기 자신** → 아무 일도 안 한다(화살은 그대로 난다)
//   · 대상이 **적이 아니면** → 피해만 주고 **끊긴다**
//   · **적이면** → 피해 1 을 주고 **꽂힌다**. 화살은 `NO_COLLIDE` 가 되고 도운 사람으로 기록된다
//   ⚠ 정답지는 `CallDamage` 의 **돌려준 값**도 본다(막혔으면 끊김) — 우리 `HurtTarget` 은 값을 안 돌려준다.
//     ⇒ 지금은 **편 가르기만** 본다. 막기 갈래는 그 함수가 값을 돌려주게 될 때 같이 옮긴다
//
// ⛔ 끌기와 끊김 관문(거리·수명·안 움직임·시야)은 **다음 조각(7-g)**.
void Server::SendHarpoonFx(uint8_t code, uint16_t n1, uint16_t n2, std::vector<OutPacket>& out) {
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), code, 0,
                             static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++fxSent_;                                             // 조각 D — FX 는 건수로 센다
}

// 지금 그 사람의 작살 화살을 찾는다 — 없으면 nullptr
Server::MissileObj* Server::HarpoonBoltOf(const Session& s) {
    if (s.harpoonBoltNc == 0) return nullptr;
    for (auto& m : missiles_) if (m.netCode == s.harpoonBoltNc && !m.destroyed) return &m;
    return nullptr;
}

// `abilityHarpoon::Do` — 화살을 만들어 쏜다
void Server::HarpoonDo(Session& s, std::vector<OutPacket>& out) {
    PlayerSetState(s, 32, frame_);                           // PlayerState32
    ++harpoonDoCalls_;
    const ProjTypeInfo* pt = ProjType("HarpoonBolt");
    if (!pt || pt->type == 0) { ++harpoonNoType_; return; }  // 종류를 모르면 안 만든다
    HarpoonBreak(s, /*emitSound=*/false, HarpoonEnd::Replaced, out);   // 남은 것이 있으면 먼저 치운다
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    m.type = pt->type;
    m.flags = pt->flags;
    const double r = static_cast<double>(kPlayerRadius) + 1.0;   // `u.Shape.Circle.R + 1.0`
    m.posX = m.newX = m.prevX = static_cast<float>(s.posX + DirVecX(s.facingPrev) * r);
    m.posY = m.newY = m.prevY = static_cast<float>(s.posY + DirVecY(s.facingPrev) * r);
    m.dir1 = m.dir2 = s.facingPrev;                          // `u.Direction1`
    m.createdFrame = m.frame136 = frame_;
    m.speed = pt->speed;
    m.owner = m.ud0 = m.ud2 = s.addr;
    m.collideKind = 4;                                       // HarpoonCollide
    m.collideAmount = static_cast<uint8_t>(kHarpoonDamage);
    m.velX = DirVecX(m.dir1) * m.speed;
    m.velY = DirVecY(m.dir1) * m.speed;
    missiles_.push_back(m); ++missilesMade_;
    ++harpoonShot_;                                          // ★ 09-19 — 갈래를 갈라 센다
    s.harpoonBoltNc = m.netCode;
    s.harpoonTarget = Target{};
    s.harpoonFrame35 = 0;                                    // 아직 걸림을 안 알렸다
    s.harpoonFrame38 = 0;
    // ★ 09-19 진단 `--harpoon-force` — **맞히지 않고 바로 건다.**
    //   ⑦ 의 물음은 「끌려오는 **모양**」이지 「잘 맞나」가 아니다. 그런데 무인은 조준을
    //   못 해서(작살은 바라보는 방향으로 나간다) 75초에 16발을 쏴도 **걸림 0** 이었다.
    //   그래서 다른 사람 하나를 골라 걸린 것으로 친다. `--arrow-freeze` 와 같은 부류다
    if (forceHarpoon_) {
        for (auto& kv : sessions_) {
            Session& o = kv.second;
            if (!o.inGame || o.addr == s.addr) continue;
            Target t{};
            t.addr = o.addr;
            s.harpoonTarget = t;
            ++harpoonAttached_;
            TargetXY(t, s.harpoonTargX, s.harpoonTargY);
            s.harpoonFrame38 = frame_;
            break;
        }
    }
}

// `abilityHarpoon::breakForOwner` — 줄을 끊는다
void Server::HarpoonBreak(Session& s, bool emitSound, HarpoonEnd why, std::vector<OutPacket>& out) {
    (void)emitSound;                                         // 소리 `SoundHarpoonBroken` 은 묶음 10
    if (s.harpoonBoltNc == 0) return;
    ++harpoonEnd_[static_cast<size_t>(why)];                 // ★ 0-b — 끊긴 것만 센다(화살이 없으면 위에서 돌아감)
    // `DisableAbility` — 끊김 와이어 + 켜짐 끄기
    SendHarpoonFx(14, s.netCode, s.harpoonBoltNc, out);
    if (s.abilActive[3]) {
        ++abilEnded_[3][static_cast<size_t>(AbilEnd::HarpoonBreak)];   // ★ 0-b
        SendAbilityActive(s, 3, false, out);
        s.abilActive[3] = false;
        s.abilActiveVal[3] = false;
        s.abilActiveUntil[3] = 0;
    }
    for (size_t i = missiles_.size(); i-- > 0;) {
        if (missiles_[i].netCode != s.harpoonBoltNc) continue;
        DestroyMissile(i, out);
        break;
    }
    s.harpoonBoltNc = 0;
    s.harpoonTarget = Target{};
    s.harpoonFrame35 = 0;
    s.harpoonFrame38 = 0;
}

// `abilityHarpoon::Collide` — 화살이 무엇엔가 닿았다. 꽂히면 true(화살을 살려 둔다)
bool Server::HarpoonCollide(MissileObj& m, const Target& targ, std::vector<OutPacket>& out) {
    // ★ 09-23 0-b — 나가는 길 다섯을 다 센다(`HarpoonTouch`). 사람 판에서 「쏨 3 · 걸림 0」만 남아
    //   빗맞혔는지(아무것도 안 닿음) · 벽에 닿았는지 · 편이 같았는지를 못 갈랐다
    auto touch = [&](HarpoonTouch t) { ++harpoonTouch_[static_cast<size_t>(t)]; };
    auto oit = sessions_.find(m.owner);
    if (oit == sessions_.end()) { touch(HarpoonTouch::NoOwner); return false; }
    Session& own = oit->second;
    if (targ.none()) {                                       // 벽 — 그 칸에 피해를 주고 조용히 끊는다
        touch(HarpoonTouch::Wall);
        HarpoonBreak(own, /*emitSound=*/false, HarpoonEnd::Wall, out);
        return false;
    }
    if (!targ.addr.empty() && targ.addr == m.owner) { touch(HarpoonTouch::Self); return true; }   // 자기에게는 안 꽂힌다
    AttackSrc src;
    src.x = m.posX; src.y = m.posY;
    src.byMonsterType = m.type;
    src.missile = true;   // ★ 09-26 M1 — a3 = 작살 화살(클래스 MISSILE) — 지팡이는 발사체를 못 막는다(`GAME3_3.c:301`)
    // ★ 4-c3b2 — 때린 것 = 작살 화살(Go `spell_harpoon.go:172` CallDamage(u5, bolt, …))
    const bool took = HurtTarget(targ, kHarpoonDamage, out, m.owner, 11, &src, HitterMissile(m));      // DamageImpact = 11
    // ★ 4-c3b2b — 정답지(`spell_harpoon.go:172~176`): 피해가 안 들어갔거나(피해 함수 거짓) 적이 아니면 **대상에 기본 타격 소리
    //   (`532E20`, 때린 것 = 작살 화살)를 한 번** 내고 끊는다. 뒤쪽 「아군 피해 켜기(GameplayFlag1 — `guiserv.c:716`)면 유닛에는 꽂힘」은
    //   우리 서버에 그 설정이 없어(늘 꺼짐) 뺀다
    if (!took || !TargetIsEnemy(own, targ)) {
        touch(took ? HarpoonTouch::NotEnemy : HarpoonTouch::Refused);
        HarpoonDefaultHitSound(targ, m.type);
        HarpoonBreak(own, /*emitSound=*/false, took ? HarpoonEnd::NotEnemy : HarpoonEnd::Refused, out);
        return false;
    }
    touch(HarpoonTouch::Attached);
    own.harpoonTarget = targ;
    ++harpoonAttached_;                                      // ★ 상대에 걸렸다
    TargetXY(targ, own.harpoonTargX, own.harpoonTargY);
    own.harpoonFrame38 = frame_;
    m.flags |= 0x40;                                         // FlagNoCollide(비트 6 · ThingDb kFlagNames) — 더는 안 부딪힌다
    MarkAssist(m.owner, targ);                               // sub_4E7540
    return true;
}

// ── 묶음 7-g 작살 끌기와 끊김 관문 ────────────────────────────────────
//
// 정답지 `abilityHarpoon::Update`(`spell_harpoon.go:193`) · `UpdatePlayer`(`:186`).
//
// ⭕ **끌기** — 꽂혀 있으면 매 틱 상대에게 `applyForce(주인 자리, **−10**)` 을 준다.
//   `objectApplyForce_52DF80` 의 셈 그대로 —
//     `dp = 상대자리 − 주인자리` · `r = |dp| + 0.1` · `f = 10 × 힘 / 무게` · `ForceVec += dp × f / r`
//   힘이 **음수**라 상대는 주인 **쪽으로** 끌려온다. 그리고 매 틱 도운 사람으로 기록된다
//
// ⭕ **끊김 관문** — 꽂힌 뒤에는 다섯 가지로 끊긴다(하나라도 걸리면 그 자리에서 끝).
//   ① 주인과 상대가 **375 보다 멀어짐** ② **20 보다 가까워짐**(다 끌어왔다)
//   ③ 줄이 선 지 **150프레임**(`MaxHarpoonExistence`)
//   ④ **30틱마다** 보아 상대가 거리제곱 **1 도 안 움직였으면**(빠져나갈 수 없는 자리에 낀 것)
//   ⑤ 주인↔상대 **시야가 막히면**(벽 추적 9)  · 그리고 상대가 죽으면
//   꽂히기 **전**에는 관문이 하나다 — 주인에게서 **300 보다 멀리 날면** 끊긴다
//
// ⭕ 꽂힌 화살은 **상대 자리에 붙어 다닌다**(자리·이전자리·새자리를 모두 상대에게 맞추고 속도·힘을 0 으로).
//
// ★ `9e 07`(걸림)은 **첫 갱신 틱**에 나간다(`frame35 == 0`) — 꽂혔는지와 상관없다.
//   그래서 수명 150 도 **꽂힌 때가 아니라 그 첫 틱부터** 센다
//
// ✖ 09-23 정정 — 「⛔ 안 옮긴 것 — 겨눈 쪽으로 휘기(시전 순간 커서만 있어서)」라고 적혀 있었다.
//   커서는 그 뒤 `Op::MOUSE` 로 매 틱 받게 됐고(`aimX/aimY`), 휘기는 **nine-defects 1-b 에서 옮겼다**(아래 ⓐ~ⓒ). 소리 셋은 묶음 10
void Server::HarpoonUpdate(Session& s, std::vector<OutPacket>& out) {
    if (s.harpoonBoltNc == 0) return;
    MissileObj* bolt = HarpoonBoltOf(s);
    if (!bolt) { HarpoonBreak(s, /*emitSound=*/true, HarpoonEnd::BoltGone, out); return; }
    if (s.respawnAtFrame != 0) { HarpoonBreak(s, /*emitSound=*/true, HarpoonEnd::OwnerDead, out); return; }   // 주인이 죽었다

    // ★ 09-23 nine-defects 1-b — **겨눈 쪽으로 휘기**(`spell_harpoon.go:211~235`).
    //   정답지 화살의 `UpdateData[0]` = 휘어 가는 상대 → 우리 `bolt->ud1`(작살은 이 칸을 달리 안 쓴다).
    //   ⓐ 휘어 가던 상대가 죽었거나 사라졌으면 끊는다(`:212~216` — 꽂힌 뒤에도 본다)
    //   ⓑ 아직 안 꽂혔고 상대가 없으면 — **살아 있는 커서**(`pl.CursorPos()` = 우리 `aimX/aimY`, `Op::MOUSE`) 둘레
    //      375(`MaxHarpoonDistance`)에서 공격 주문 깃발(0x20)로 찾는다 · 화살의 **처음 방향**(Direction1) 앞쪽이어야 잡는다
    //   ⓒ 잡은 뒤로는 틱마다 속도를 그쪽으로 튼다(크기는 빠르기 그대로) — **잡은 틱에는 안 튼다**(정답지 `else` 갈래)
    //   전에는 「시전 순간 커서만 있다」는 까닭으로 안 옮겼는데(아래 7-g 머리 주석), 커서를 매 틱 받게 된 뒤로 그 전제가 없어졌다.
    //   그동안 작살은 **바라보는 쪽으로 곧게만** 날아 사람 판에서 「쏨 3 · 걸림 0」이었다
    auto aimAlive = [&](const Target& t) {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            return it != sessions_.end() && it->second.inGame && it->second.respawnAtFrame == 0;
        }
        // ★ 10-01 시체 — 겨눈 소환수가 죽었으면 끊는다(`FlagDestroyed|FlagDead` · `spell_harpoon.go:213`) · 전에는 죽으면 바로 지워져 칸 밖이 맡았다
        return t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size() && !pets_[static_cast<size_t>(t.pet)].dead;
    };
    if (!bolt->ud1.none() && !aimAlive(bolt->ud1)) { HarpoonBreak(s, /*emitSound=*/true, HarpoonEnd::AimDead, out); return; }
    if (s.harpoonTarget.none()) {
        if (bolt->ud1.none()) {
            const Target found = SpellFlySearchTarget(true, static_cast<float>(s.aimX), static_cast<float>(s.aimY),
                                                      bolt->posX, bolt->posY, s.addr, std::string(), 0x20,
                                                      static_cast<float>(kHarpoonMaxDist), &s, &bolt->buffs);
            float fx = 0, fy = 0;
            if (!found.none() && TargetXY(found, fx, fy) &&
                (TestTwoPointsAndDirection(bolt->posX, bolt->posY, bolt->dir1, fx, fy) & 1)) {
                bolt->ud1 = found;
                ++harpoonAimFound_;
            }
        } else {
            float tx = 0, ty = 0;
            if (TargetXY(bolt->ud1, tx, ty)) {
                const double vx = static_cast<double>(tx) - bolt->posX, vy = static_cast<double>(ty) - bolt->posY;
                const double len = std::sqrt(vx * vx + vy * vy);
                if (len > 0.0) {                             // 겹친 자리(0)면 방향이 없다 — 그대로 둔다
                    bolt->velX = static_cast<float>(vx / len * bolt->speed);
                    bolt->velY = static_cast<float>(vy / len * bolt->speed);
                    ++harpoonSteerTicks_;
                }
            }
        }
    }

    const double dx0 = static_cast<double>(bolt->posX) - s.posX;
    const double dy0 = static_cast<double>(bolt->posY) - s.posY;
    // ★ 09-24 H4(사람 판 hj2·hj3 「끌어온 상대가 주인 뒤로 계속 간다」) — 정답지 거리 `nox_xxx_calcDistance_4E6C00`
    //   (`GAME3_3.c:2377~2420`)은 **중심 거리에서 두 몸의 크기를 뺀다**(원 = 반지름 · 상자 = 가로세로 중 큰 쪽 절반 · 0.01 아래면 0.01).
    //   화살(HarpoonBolt)은 몸 모양 1 이라 안 빼고, 주인(사람 NewPlayer)은 원 반지름 10 을 뺀다(실측 `--dump-thing`).
    //   전에는 중심 거리를 써서 「20 안 = 다 끌어옴」이 중심 30 이 아니라 20 에서야 걸렸다 — 두 몸(10+10)이 이미 맞닿은 자리라
    //   서로 밀어내는 사이 화살 자리(한 틱 전 상대 자리)와 물러난 주인의 거리가 늘 20 을 넘어 끊기지 않고 주인을 계속 밀었다
    //   (재현 `TestHarpoonDrift` — 거리 200 에서 50틱 동안 주인이 227 밀림)
    double dist = std::sqrt(dx0 * dx0 + dy0 * dy0) - kPlayerRadius;
    if (dist < 0.0099999998) dist = 0.0099999998;

    if (!s.harpoonTarget.none()) {
        ++harpoonPullTicks_;
        // ★ 진단 — 걸린 동안 **매 틱** 두 사람 자리를 찍는다. 확인표의 판정 기준이
        //   「끌려오는 쪽이 주인을 **지나쳐 넘나드는가**」라서 좌표가 곧 판정이다
        if (dumpHarpoon_) {
            float dx0 = 0, dy0 = 0;
            if (TargetXY(s.harpoonTarget, dx0, dy0)) {
                std::printf("[작살추적] 프레임 %u 주인 %.0f,%.0f 상대 %.0f,%.0f 거리 %.0f\n",
                            frame_, s.posX, s.posY, dx0, dy0,
                            std::sqrt((dx0 - s.posX) * (dx0 - s.posX) +
                                      (dy0 - s.posY) * (dy0 - s.posY)));
            }
        }
        float tx = 0, ty = 0;
        if (!TargetXY(s.harpoonTarget, tx, ty)) { HarpoonBreak(s, true, HarpoonEnd::TargetGone, out); return; }   // 상대가 사라졌다
        if (!s.harpoonTarget.addr.empty()) {
            auto it = sessions_.find(s.harpoonTarget.addr);
            if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) {
                HarpoonBreak(s, true, HarpoonEnd::TargetDead, out); return;
            }
        } else if (s.harpoonTarget.pet >= 0 && static_cast<size_t>(s.harpoonTarget.pet) < pets_.size() &&
                   pets_[static_cast<size_t>(s.harpoonTarget.pet)].dead) {
            // ★ 10-01 시체 — 소환수 상대도 죽으면 끊는다(`FlagDestroyed|FlagDead` · `spell_harpoon.go:213·266`) · 전에는 죽으면 바로 지워져 위 「사라짐」이 맡았다
            HarpoonBreak(s, true, HarpoonEnd::TargetDead, out); return;
        }
        if (dist > kHarpoonMaxDist) { HarpoonBreak(s, true, HarpoonEnd::TooFar, out); return; }     // ①
        if (dist < kHarpoonMinDist) { HarpoonBreak(s, false, HarpoonEnd::Arrived, out); return; }   // ②
        if (s.harpoonFrame35 != 0 && frame_ - s.harpoonFrame35 > kHarpoonLifetime) {      // ③
            HarpoonBreak(s, true, HarpoonEnd::Lifetime, out); return;
        }
        if (frame_ - s.harpoonFrame38 > 30u) {                                           // ④ 30틱마다 본다
            s.harpoonFrame38 = frame_;
            const double mx = static_cast<double>(s.harpoonTargX) - tx;
            const double my = static_cast<double>(s.harpoonTargY) - ty;
            if (mx * mx + my * my < 1.0) { HarpoonBreak(s, true, HarpoonEnd::Stuck, out); return; }
            s.harpoonTargX = tx;
            s.harpoonTargY = ty;
        }
        if (!MapTraceRay(static_cast<float>(s.posX), static_cast<float>(s.posY), tx, ty, 0x9)) {   // ⑤
            HarpoonBreak(s, true, HarpoonEnd::NoSight, out); return;
        }
        // 화살을 상대 자리에 붙인다
        bolt->newX = bolt->posX = bolt->prevX = tx;
        bolt->newY = bolt->posY = bolt->prevY = ty;
        bolt->velX = bolt->velY = 0.0f;
        bolt->forceX = bolt->forceY = 0.0f;
        // 끌어당기기 — `applyForce(주인 자리, −힘)`(`UpdatePlayer`)
        MarkAssist(s.addr, s.harpoonTarget);                 // sub_4E7540 — 매 틱 도운 사람으로
        HarpoonPull(s, s.harpoonTarget, tx, ty);
    } else if (dist > kHarpoonMaxFlight) {
        HarpoonBreak(s, true, HarpoonEnd::OverFlight, out); return;   // 안 걸린 채 너무 멀리 날았다
    }
    if (s.harpoonFrame35 == 0) {                             // 첫 틱에 「걸림」을 알린다
        SendHarpoonFx(7, s.netCode, s.harpoonBoltNc, out);
        s.harpoonFrame35 = frame_;
    }
}

// `objectApplyForce_52DF80` — 힘이 음수면 `vec` 쪽으로 끌려온다
void Server::HarpoonPull(const Session& owner, const Target& targ, float tx, float ty) {
    const double dpx = static_cast<double>(tx) - owner.posX;
    const double dpy = static_cast<double>(ty) - owner.posY;
    const double r = std::sqrt(dpx * dpx + dpy * dpy) + 0.1;
    // ★ 09-22 — 끌리는 쪽이 **사람이면 그 사람의 무게**다(직업별 10~30). 전에는 1 로 두어
    //   끌기 세기가 15배였고, 그래서 상대가 주인을 지나쳐 왕복했다(⑦ 의 「튕긴다」)
    double mass = 10.0;   // 힘을 모르는 사람의 바닥값(정답지 식의 최소)
    if (!targ.addr.empty()) {
        const auto ts = sessions_.find(targ.addr);
        if (ts != sessions_.end()) mass = PlayerMassOf(ts->second);
    } else if (targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        const auto tb = typeBodies_.find(pets_[static_cast<size_t>(targ.pet)].type);
        mass = (tb != typeBodies_.end() && tb->second.mass > 0.0f) ? tb->second.mass : 1.0;
    }
    if (mass <= 0.0) mass = 1.0;
    const double f = 10.0 * (-kHarpoonForce) / mass;
    const double fx = dpx * f / r;
    const double fy = dpy * f / r;
    if (!targ.addr.empty()) {
        auto it = sessions_.find(targ.addr);
        if (it == sessions_.end()) return;
        if (it->second.respawnAtFrame != 0) return;          // 죽은 것은 안 밀린다(isObjectMovable)
        it->second.forceX += fx;
        it->second.forceY += fy;
    } else if (targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        // ⚠ 소환수는 아직 ForceVec 적분이 없다(걸음을 AI 가 직접 옮긴다) — 자리로 바로 옮긴다
        pets_[static_cast<size_t>(targ.pet)].x += fx;
        pets_[static_cast<size_t>(targ.pet)].y += fy;
    }
}

// ── 묶음 7-h 안내서(짐승 두루마리) ────────────────────────────────────
//
// 정답지 `nox_xxx_awardBeastGuide_4FAE80`·`nox_xxx_netReportGuideAward_4D8000`
//        (`legacy/server__magic__plyrgide.c`).
//
// ⭕ 와이어 — `d1 <안내서 번호> <알림>` **3바이트**. `netSendPacket1` 이라 **순번 봉투**.
//   ★ 셋째 칸의 **0x80 은 이 길에서는 안 선다** — 부르는 자리가 넷째 인자를 늘 **0** 으로 넘긴다.
//     클라도 `& 0x7F` 로 벗겨 읽는다(`cdecode.c:2576`)
//
// ⭕ 주기 — 번호는 **1~40**(벗어나면 "AwardGuideError" 글줄). 이미 있으면 **조용히 실패**한다
//   (주문·능력과 달리 글줄도 안 보낸다). 없으면 **1** 로 세운다. 안내서는 레벨이 없고 **있다/없다**뿐이다
//
// ⭕⭕ **무리 안내서** — 정답지가 `0x587000 + 216292` 의 표를 훑는다.
//   그 자리는 **포인터 배열**이고, 실측으로 **칸이 하나**뿐이다(둘째 칸이 0 이라 거기서 끝난다).
//   그 하나가 가리키는 목록은 `24, 7, 8, 25, 26, 0` —
//   ⇒ **24 번을 받으면 7·8·25·26 도 같이** 1 이 된다. `d1` 은 **받은 번호 하나만** 간다
//   (⭕ `blob_587000.dat` 오프셋 216268 을 바이트로 읽어 확인했다)
static const uint8_t kGuideGroupLead = 24;
static const uint8_t kGuideGroupRest[] = { 7, 8, 25, 26 };

void Server::SendGuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_GUIDE_AWARD), guide,
                             static_cast<uint8_t>(announce ? 1 : 0) };
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

bool Server::GuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out) {
    if (guide == 0 || guide >= 41) return false;         // "AwardGuideError"(글줄은 묶음 10)
    if (s.guideLvl[guide] != 0) return false;            // 이미 있으면 조용히
    s.guideLvl[guide] = 1;
    // 10-b PlaySound 227 AwardGuide — plyrgide.c:65, kind 0 (everyone nearby)
    PlaySoundAt(227, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
    // ⛔ 보상 알림(나·남들)은 글줄이라 아직이다
    if (guide == kGuideGroupLead) {
        for (uint8_t g : kGuideGroupRest) s.guideLvl[g] = 1;
    }
    SendGuideAward(s, guide, announce, out);
    return true;
}

// ── 묶음 8-b 팀 메시지(0xC4) ──────────────────────────────────────────
//
// 정답지 `sub_4197C0`·`sub_4198A0`·`sub_418D80`(`legacy/GAME1_1.c`) ·
//        `NetTeamRemove`·`NetTeamChangeLessons`(`server/network.go:266~281`) ·
//        받는 쪽 `network_server.go:251`.
//
// ⚠ **`TEAM_MSG` 는 팀 채팅이 아니다.** 하위 코드로 갈리는 **팀 명부·점수** 다중 메시지다.
//   ⭕ 코드와 길이는 클라 해독(`cdecode.c:2305~2450`)의 `return` 값으로 확인했다 —
//     `0` 만들기·고치기 **2×[15]+18** · `1` 사람 넣기 **10** · `2` 사람 빼기 **6** ·
//     `3` 딴 팀으로 **10** · `4` 이름 바꾸기 **46** · `5` **6** · `6` 팀 지우기 **6** ·
//     `7` **2** · `8` 점수 **10** · `9` **2**
//
// ★★ **우리 서버는 그동안 팀 번호만 정하고 클라에 한 번도 안 알렸다**
//   (`AssignTeams` 는 `Session::team` 만 만졌고 `0xC4` 를 보내는 곳이 **0곳**이었다).
//   ⇒ 클라의 팀 목록이 비어 있어 이름표 색도 점수판도 안 섰다. 이 조각이 그 구멍이다.
//
// ⭕ 코드 0 짜임(`sub_4197C0` `GAME1_1.c:558`) —
//   `[2..5]` 팀 번호 u32(+57) · `[6..9]` u32(+60) · `[10..13]` **점수** u32(+52) ·
//   `[14]` 협동팀(깃발 512)이면 1 — 받는 사람이 **그 팀에 저절로 든다** ·
//   `[15]` 이름 길이(**와이드 글자 수**) · `[16]` 칭호 번호(+56) ·
//   `[17]` **1 이면 클라가 제 기본 이름을 쓴다**(+68) · `[18..]` 이름(2바이트씩)
//   ★ 우리는 `[17] = 1` 로 보내고 이름을 **안 싣는다** — 정품 팀 이름표를 **지어내지 않고**
//     클라가 제 문자열표에서 고르게 둔다(`teamTitle(data[16])`)
//
// ⭕ 코드 1(`sub_4198A0` `:588`) — `[2..5]` 팀 번호 · `[6..7]` 넷코드 · `[8..9]` **그림 종류**
// ⭕ 코드 6(`NetTeamRemove`) · 코드 5(`sub_418D80` `:310`) — `[2..5]` 팀 번호. **6바이트**
// ⭕ 코드 8(`NetTeamChangeLessons`) — `[2..5]` 팀 번호 · `[6..9]` 점수. **10바이트**
//   ← 7-c 와 이어진다. `playerUpdateScore_54D980` 이 사람 점수와 **같이** 팀 점수를 올린다
//
// ⚠ 보내는 대상 번호가 **159(0x9F)** 다 — 255(전체)와 다르다. `netSendPacket_4E5030` 이
//   높은 비트(0x80)를 보고 `31` 번 자리를 빼는 꼴인데, **31 은 host 자신**이다
//   (`netImportant_4E5770` 의 `a1 != 31`, `netClientSend2_4E53C0(31, …)`).
//   ⇒ 붙어 있는 전용 서버에는 그 자리가 없으므로 **모두에게**와 같다. (자리 뜻은 ⚠ 미검증)
void Server::SendTeamMsg(const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, n, out);
}

// 코드 0 — 팀 만들기·고치기
void Server::SendTeamCreate(const Team& t, std::vector<OutPacket>& out) {
    uint8_t msg[18];
    msg[0] = static_cast<uint8_t>(Op::TEAM_MSG);
    msg[1] = 0;
    msg[2] = t.id; msg[3] = msg[4] = msg[5] = 0;
    msg[6] = static_cast<uint8_t>(t.field60); msg[7] = static_cast<uint8_t>(t.field60 >> 8);
    msg[8] = static_cast<uint8_t>(t.field60 >> 16); msg[9] = static_cast<uint8_t>(t.field60 >> 24);
    const uint32_t sc = static_cast<uint32_t>(t.score);
    msg[10] = static_cast<uint8_t>(sc); msg[11] = static_cast<uint8_t>(sc >> 8);
    msg[12] = static_cast<uint8_t>(sc >> 16); msg[13] = static_cast<uint8_t>(sc >> 24);
    msg[14] = 0;                  // 협동팀 깃발(512)일 때만 1 — 우리는 아레나다
    msg[15] = 0;                  // 이름 길이 0 — 아래 [17] 이 켜져 클라가 제 이름을 쓴다
    msg[16] = t.titleIdx;
    msg[17] = 1;                  // 기본 이름 쓰기
    SendTeamMsg(msg, sizeof(msg), out);
}

// 코드 1 — 사람을 팀에 넣는다
void Server::SendTeamAddMember(uint8_t teamId, uint16_t netCode, uint16_t spriteType,
                               std::vector<OutPacket>& out) {
    uint8_t msg[10];
    msg[0] = static_cast<uint8_t>(Op::TEAM_MSG);
    msg[1] = 1;
    msg[2] = teamId; msg[3] = msg[4] = msg[5] = 0;
    msg[6] = static_cast<uint8_t>(netCode); msg[7] = static_cast<uint8_t>(netCode >> 8);
    msg[8] = static_cast<uint8_t>(spriteType); msg[9] = static_cast<uint8_t>(spriteType >> 8);
    SendTeamMsg(msg, sizeof(msg), out);
}

// 코드 6 — 팀 지우기
void Server::SendTeamRemove(uint8_t teamId, std::vector<OutPacket>& out) {
    uint8_t msg[6];
    msg[0] = static_cast<uint8_t>(Op::TEAM_MSG);
    msg[1] = 6;
    msg[2] = teamId; msg[3] = msg[4] = msg[5] = 0;
    SendTeamMsg(msg, sizeof(msg), out);
}

// 코드 8 — 팀 점수 (`netChangeTeamID_419090` 이 이것을 부른다)
void Server::SendTeamScore(uint8_t teamId, int32_t score, std::vector<OutPacket>& out) {
    uint8_t msg[10];
    msg[0] = static_cast<uint8_t>(Op::TEAM_MSG);
    msg[1] = 8;
    msg[2] = teamId; msg[3] = msg[4] = msg[5] = 0;
    const uint32_t v = static_cast<uint32_t>(score);
    msg[6] = static_cast<uint8_t>(v); msg[7] = static_cast<uint8_t>(v >> 8);
    msg[8] = static_cast<uint8_t>(v >> 16); msg[9] = static_cast<uint8_t>(v >> 24);
    SendTeamMsg(msg, sizeof(msg), out);
}

Server::Team* Server::TeamById(uint8_t id) {
    for (auto& t : teams_) if (t.id == id) return &t;
    return nullptr;
}

// `netChangeTeamID_419090` — 팀 점수를 놓고 알린다
void Server::TeamChangeScore(uint8_t teamId, int32_t score, std::vector<OutPacket>& out) {
    Team* t = TeamById(teamId);
    if (!t) return;
    t->score = score;
    SendTeamScore(teamId, score, out);
}

// 팀 상태를 통째로 알린다 — 팀을 나눈 뒤에 부른다
void Server::SendTeamState(std::vector<OutPacket>& out) {
    if (teams_.empty()) {
        // 우리 판은 빨강(1)·파랑(2) 둘이다(`StartCtf` 와 `AssignTeams` 가 그 둘만 쓴다)
        Team a; a.id = 1; a.titleIdx = 1;
        Team b; b.id = 2; b.titleIdx = 2;
        teams_.push_back(a);
        teams_.push_back(b);
    }
    for (const Team& t : teams_) SendTeamCreate(t, out);
    for (auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.team == 0) continue;
        SendTeamAddMember(o.team, o.netCode, static_cast<uint16_t>(o.charClass), out);
    }
}

// ── 묶음 8-d 플래그볼 ─────────────────────────────────────────────────
//
// 정답지 득점 `GAME3_3.c:4700~4770` · 공 상태 `sub_4E8290`(`src/flagball.go:5`) ·
//        와이어 `netSendBallStatus_4D95F0`·`netFlagballWinner_4D8C40`(`GAME3_2.c:4785·4328`).
//
// ⭕ **자료가 실제로 있다.** `maps\FlagBall\FlagBall.map` 을 우리 파서로 열어 세어 보니
//   `GameBallStart` **1** · `Flag`(골대) **2** · `PlayerStart` **18** 이다(전체 168개 25종).
//   ⭕ thing.bin — `GameBall` 종류 **241**(COLLIDE `BallCollide`) · `GameBallStart` **243** · `Flag` **179**
//   (우리 CTF 깃발은 176·177 로 **다른 것**이다 — 저건 깃발, 이건 골대다)
//
// ⭕ 공 상태 `d9 <상태 u8> <든 사람 넷코드 u16>` **4바이트** · **모두에게** · 순번 봉투.
//   상태는 넷이다(`sub_4E8290` 을 부르는 자리로 확인) —
//     **0** 제자리(판 시작·득점 뒤) · **1** 바닥에 굴러다님(떨어뜨림) ·
//     **2** 팀 칭호가 2 가 **아닌** 쪽이 듦 · **4** 팀 칭호가 **2** 인 쪽이 듦
//   ⚠ 그래서 「빨강이 들었나 파랑이 들었나」를 상태 번호로 가른다 — 넷코드만으로는 팀을 모른다
//
// ⭕ 득점(`GAME3_3.c:4700~4760`) — 공을 든 사람이 **골대의 다음 팀**과 같은 팀일 때 들어간다.
//   팀이 둘이면 「**남의 골대에 넣는다**」와 같은 말이다.
//   그때 — 사람 점수 **+1**(`4e`) · **팀 점수 +1**(`c4 08`) ·
//   한도(`servGamedataGet(64)`)에 닿은 팀이 있으면 `0x56`(플래그볼 승자) ·
//   그리고 공을 **`GameBallStart` 중 아무 데나** 다시 놓는다
//   ⚠ 한도 자리는 8-a·8-c 와 **같은 값**이다 — `servGamedataGet(모드 번호)` 이고 플래그볼 모드 번호가 64다.
//     ⇒ `GameSettings::limitValue`(⭕ 실측 15)를 그대로 쓴다
//
// ⛔ 안 옮긴 것 — 소리 927·929 · 점 효과 154 · 알림 9(글) → 묶음 10 ·
//   공이 벽에 튕기는 물리(`BallCollide`) — 우리 공은 **든 사람만 따라다닌다**
void Server::StartFlagBall(double ballX, double ballY,
                           double goal1X, double goal1Y, double goal2X, double goal2Y) {
    ball_.clear();
    goals_.clear();
    ballStarts_.clear();
    GameBall b;
    b.netCode = nextShotNetCode_++;
    b.type = kGameBallType;
    b.x = b.homeX = ballX;
    b.y = b.homeY = ballY;
    ball_.push_back(b);
    ballStarts_.push_back({ ballX, ballY });
    goals_.push_back(BallGoal{ 1, goal1X, goal1Y });
    goals_.push_back(BallGoal{ 2, goal2X, goal2Y });
    roundOver_ = false;
    AssignTeams();
}

// `sub_4E8290` — 공 상태를 놓고 알린다. **바뀔 때만**(깃발과 같은 규칙)
void Server::SendBallStatus(std::vector<OutPacket>& out) {
    if (ball_.empty()) return;
    GameBall& b = ball_[0];
    uint16_t carrier = 0;
    uint8_t state = b.atHome ? 0 : 1;
    if (!b.carrier.empty()) {
        auto it = sessions_.find(b.carrier);
        if (it != sessions_.end()) {
            carrier = it->second.netCode;
            // 팀 칭호가 2 면 4, 아니면 2
            const Team* t = TeamById(it->second.team);
            state = (t && t->titleIdx == 2) ? 4 : 2;
        }
    }
    const uint32_t mark = (static_cast<uint32_t>(state) << 16) | carrier;
    if (b.sentMark == mark) return;
    b.sentMark = mark;
    const uint8_t msg[4] = { static_cast<uint8_t>(Op::REPORT_BALL_STATUS), state,
                             static_cast<uint8_t>(carrier), static_cast<uint8_t>(carrier >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
}

// `netFlagballWinner_4D8C40` — 까닭 칸이 **늘 0** 이다(팀 데스매치 `0x59` 와 다른 점)
void Server::SendFlagBallWinner(uint8_t teamId, std::vector<OutPacket>& out) {
    SendRoundWinner(static_cast<uint8_t>(Op::REPORT_FLAG_BALL_WINNER), teamId, 0, out);
}

void Server::DropBall(const std::string& who, std::vector<OutPacket>& out) {
    if (ball_.empty()) return;
    GameBall& b = ball_[0];
    if (b.carrier != who) return;
    auto it = sessions_.find(who);
    if (it != sessions_.end()) { b.x = it->second.x; b.y = it->second.y; }
    b.carrier.clear();
    b.atHome = false;
    SendBallStatus(out);
}

// 골대의 **다음 팀** — 정답지 `teamNext(getTeamByID(골대팀))`, 끝이면 첫 팀으로 돈다
uint8_t Server::TeamAfter(uint8_t teamId) const {
    if (teams_.empty()) return 0;
    for (size_t i = 0; i < teams_.size(); ++i)
        if (teams_[i].id == teamId) return teams_[(i + 1) % teams_.size()].id;
    return teams_[0].id;
}

void Server::UpdateBall(std::vector<OutPacket>& out) {
    if (ball_.empty()) return;
    SendBallStatus(out);
    if (roundOver_) return;                              // 끝난 판은 더 안 센다(깃발과 같은 규칙)
    GameBall& b = ball_[0];

    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame || s.respawnAtFrame != 0 || s.team == 0) continue;
        const double dx = b.x - s.x, dy = b.y - s.y;
        if (b.carrier.empty() && dx * dx + dy * dy <= kFlagTouchRange * kFlagTouchRange) {
            b.carrier = kv.first;
            // 10-b PlaySound 927 BallGrab — GAME3_3.c:5446, kind 0
            PlaySoundAt(927, static_cast<float>(b.x), static_cast<float>(b.y));                        // 줍는다
            b.atHome = false;
            SendBallStatus(out);
        }
        if (b.carrier != kv.first) continue;
        b.x = s.x; b.y = s.y;                            // 든 사람을 따라다닌다

        for (const BallGoal& g : goals_) {               // 골대에 닿았나
            if (TeamAfter(g.team) != s.team) continue;   // **다음 팀**만 넣을 수 있다
            const double gx = g.x - s.x, gy = g.y - s.y;
            if (gx * gx + gy * gy > kFlagTouchRange * kFlagTouchRange) continue;
            // 10-b PlaySound 929 BallHitGoal — GAME3_3.c:4727, kind 0
            PlaySoundAt(929, static_cast<float>(g.x), static_cast<float>(g.y));
            ChangeScore(s, +1, out);                     // 사람 점수
            if (const Team* t = TeamById(s.team))        // 팀 점수
                TeamChangeScore(s.team, t->score + 1, out);
            // 공을 `GameBallStart` 중 아무 데나 다시 놓는다
            b.carrier.clear();
            b.atHome = true;
            if (!ballStarts_.empty()) {
                const size_t k = ballStarts_.size() == 1
                               ? 0 : static_cast<size_t>(NextRandom(static_cast<uint32_t>(ballStarts_.size())));
                b.x = ballStarts_[k].first;
                b.y = ballStarts_[k].second;
            } else {
                b.x = b.homeX; b.y = b.homeY;
            }
            SendBallStatus(out);
            // 한도에 닿은 팀이 있으면 판이 끝난다
            const int32_t limit = static_cast<int32_t>(gameSettings_.limitValue);
            if (limit > 0) {
                for (const Team& t : teams_) {
                    if (t.score < limit) continue;
                    roundOver_ = true;
                    restartAtFrame_ = frame_ + kRoundRestartTicks;
                    SendFlagBallWinner(t.id, out);
                    return;
                }
            }
            break;
        }
    }

    // 든 사람이 사라졌으면 공을 바닥에 둔다
    if (!b.carrier.empty()) {
        auto it = sessions_.find(b.carrier);
        if (it == sessions_.end() || !it->second.inGame) {
            b.carrier.clear();
            b.atHome = false;
            SendBallStatus(out);
        }
    }
}

// `nox_xxx_playerCallDisconnect_4DEAB0(대상, 4)` — 사람을 내보낸다.
//   조용해서 내보내는 길(틱 안)과 **같은 절차**를 한 자리로 모은 것이다 —
//   들고 있던 깃발·공을 떨구고 · 걸던 주문을 끊고 · 표를 지우고 · 자리를 비우고 ·
//   모두에게 `PLAYER_QUIT` 와 `DESTROY_OBJECT` 를 알린다.
void Server::KickPlayer(const std::string& addr, std::vector<OutPacket>& out) {
    uint16_t netCode = 0;
    bool wasInGame = false;
    {
        auto g = sessions_.find(addr);
        if (g == sessions_.end()) return;
        netCode = g->second.netCode;
        wasInGame = g->second.inGame;
    }
    if (wasInGame) {
        DropFlag(addr, out);
        DropBall(addr, out);
        PlayerCancelSpells(addr, out);
    }
    VoteDropPlayer(addr, out);                          // 나간 사람의 표를 지운다(`sub_506740`)
    // 10-d — 지우기 **전에** 알린다. 정답지 `player.go:630~643` 순서 —
    //   먼저 `0xAE` 를 **나머지 사람들에게**, 그다음 `0xC5` 를 **나가는 그 사람에게**
    SendOutgoingClient(netCode, addr, out);
    {
        auto g = sessions_.find(addr);
        if (g != sessions_.end()) {
            const uint8_t kick[1] = { static_cast<uint8_t>(Op::KICK_NOTIFICATION) };
            Emit(g->second, /*reliable=*/true, /*stream=*/0, kick, sizeof(kick), out);
        }
    }
    sessions_.erase(addr);
    if (!wasInGame || netCode == 0) return;
    uint8_t quit[3] = { static_cast<uint8_t>(Op::PLAYER_QUIT),
                        static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8) };
    Broadcast(/*viaSeq=*/true, quit, sizeof(quit), out);
    uint8_t bye[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT),
                       static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8) };
    Broadcast(/*viaSeq=*/false, bye, sizeof(bye), out);
}

// ── 묶음 8-e 투표(0xEE) ───────────────────────────────────────────────
//
// 정답지 `GAME4.c:5986~6640`(18함수) · 매 틱 `nox_xxx_voteUptate_506F30`
//        (`server__system__server.c:721`) · 와이어 `sub_507190`·`netSendVote_506840`.
//
// ⭕ 와이어 셋 —
//   · 받기 `ee <갈래 0~5> [겨눈 사람 이름 와이드 25자]` — **52바이트**(갈래 0~3) 또는 **2바이트**(4·5)
//   · 보내기 `ee 06 <값>` **3바이트** — 목록이 **비었다가 찼을 때 1**, **비었을 때 0**
//   · 보내기 `ee 07` **2바이트** — 갈래 2 투표가 사라질 때 **표를 던진 사람에게만**
//
// ⭕ 받는 갈래 여섯(`sdecode.c:315`) — 앞 넷은 이름이 붙고 뒤 둘은 안 붙는다.
//   `0` 찬성(퀘스트면 **3**, 아니면 **0**) · `1` 찬성(늘 1) ·
//   `2` 물리기(퀘스트면 3, 아니면 0) · `3` 물리기(늘 1) ·
//   `4` 찬성 갈래 **2** · `5` 물리기 갈래 **2**
//
// ⭕ 투표 갈래 넷과 매 틱 하는 일(`voteUptate_506F30`) —
//   `0`·`1` **강퇴** → `sub_506F80` · `2` **퀘스트 다시 시작** → `sub_507090` ·
//   `3` **퀘스트에서 내보내기** → `sub_507100`
//
// ⭕ 필요한 표 수(⭕ `blob_587000.dat` 를 바이트로 읽음) —
//   갈래 0·1 = **4**(오프셋 229980) · 갈래 2·3 = **6**(`sub_506A20` 이 **숫자 6 을 그대로** 넣는다).
//   곁들여 229984 기본 4 · 229988 `resetQuestMinVotes` 6 · 229992 `kickQuestPlayerMinVotes` 5
//
// ⭕ **통과 규칙**(`sub_507000`) — 둘 중 하나면 통과다.
//   ① 표가 **필요 수 이상**  ② 표가 **(사람 수 − 1) 이상**이고 **2표 이상**
//   ②의 사람 수는 `팀만` 표가 서 있으면(`[20] == 1`) **같은 팀**만 센다
//
// ⛔ 안 옮긴 것 — 갈래 2·3(**퀘스트 전용**). 다시 시작은 `맵을 다시 읽는` 것이고
//   내보내기는 `sub_4DCFB0` 인데 우리 서버엔 그 길이 없다. 갈래 0·1 **강퇴**만 옮긴다.
//   그리고 글줄(`sub_416770`)은 묶음 10
void Server::SendVoteGui(uint8_t val, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::VOTE), 6, val };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
}

void Server::SendVoteClose(Session& s, std::vector<OutPacket>& out) {
    const uint8_t msg[2] = { static_cast<uint8_t>(Op::VOTE), 7 };
    EmitSeq(s, msg, sizeof(msg), out);
}

Server::Vote* Server::FindVote(uint8_t kind, const std::string& target) {
    for (auto& v : votes_) if (v.kind == kind && v.target == target) return &v;
    return nullptr;
}

// `sub_5067B0` — 투표를 지운다. 갈래 2 면 **표를 던진 사람에게** `ee 07` 을 보낸다
void Server::RemoveVote(size_t i, std::vector<OutPacket>& out) {
    if (i >= votes_.size()) return;
    if (votes_[i].kind == 2) {
        for (const std::string& who : votes_[i].voters) {
            auto it = sessions_.find(who);
            if (it != sessions_.end() && it->second.inGame) SendVoteClose(it->second, out);
        }
    }
    votes_.erase(votes_.begin() + static_cast<long>(i));
    if (votes_.empty()) SendVoteGui(0, out);          // 목록이 비면 화면을 닫는다
}

// `sub_507000` — 통과했나
bool Server::VotePasses(const Vote& v) const {
    const uint8_t need = (v.kind == 2 || v.kind == 3) ? 6 : kVoteKickMinVotes;
    if (v.voters.size() >= need) return true;
    size_t n = 0;
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        if (v.teamOnly && kv.second.team != v.team) continue;
        ++n;
    }
    return v.voters.size() + 1 >= n && v.voters.size() >= 2;
}

// `sub_5068E0`·`sub_506B00`·`sub_506B80` — 찬성 한 표
void Server::VoteCast(Session& voter, uint8_t kind, const std::string& targetName,
                      std::vector<OutPacket>& out) {
    std::string target;                               // 겨눈 사람의 주소
    if (kind != 2) {
        if (targetName.empty()) return;
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame || kv.second.name != targetName) continue;
            target = kv.first;
            break;
        }
        if (target.empty() || target == voter.addr) return;   // 없는 사람·자기 자신은 안 된다
    }
    Vote* v = FindVote(kind, target);
    const bool wasEmpty = votes_.empty();
    if (!v) {
        Vote nv;
        nv.kind = kind;
        nv.target = target;
        nv.team = voter.team;
        nv.startFrame = frame_;
        votes_.push_back(nv);
        v = &votes_.back();
        if (wasEmpty) SendVoteGui(1, out);            // 첫 투표가 서면 화면을 연다
    }
    for (const std::string& w : v->voters) if (w == voter.addr) return;   // 한 사람 한 표
    v->voters.push_back(voter.addr);
}

// `sub_506D00`·`sub_506DE0`·`sub_506E50` — 표를 물린다. 표가 0 이 되면 투표가 사라진다
void Server::VoteWithdraw(Session& voter, uint8_t kind, const std::string& targetName,
                          std::vector<OutPacket>& out) {
    std::string target;
    if (kind != 2) {
        if (targetName.empty()) return;
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame || kv.second.name != targetName) continue;
            target = kv.first;
            break;
        }
        if (target.empty()) return;
    }
    for (size_t i = 0; i < votes_.size(); ++i) {
        if (votes_[i].kind != kind || votes_[i].target != target) continue;
        auto& vs = votes_[i].voters;
        for (size_t k = 0; k < vs.size(); ++k) {
            if (vs[k] != voter.addr) continue;
            vs.erase(vs.begin() + static_cast<long>(k));
            if (vs.empty()) RemoveVote(i, out);
            return;
        }
        return;
    }
}

// `sub_506740` — 사람이 나가면 그 사람의 표를 모두 지운다
void Server::VoteDropPlayer(const std::string& who, std::vector<OutPacket>& out) {
    for (size_t i = votes_.size(); i-- > 0;) {
        auto& vs = votes_[i].voters;
        for (size_t k = 0; k < vs.size(); ++k) {
            if (vs[k] != who) continue;
            vs.erase(vs.begin() + static_cast<long>(k));
            break;
        }
        if (vs.empty()) RemoveVote(i, out);
    }
}

// `nox_xxx_voteUptate_506F30` — 매 틱. 갈래 0·1 만 옮겼다
void Server::VoteUpdate(std::vector<OutPacket>& out) {
    for (size_t i = votes_.size(); i-- > 0;) {
        if (i >= votes_.size()) continue;
        const Vote& v = votes_[i];
        if (v.kind != 0 && v.kind != 1) continue;     // 갈래 2·3 은 퀘스트 전용 — 안 옮겼다
        auto t = sessions_.find(v.target);
        if (t == sessions_.end() || !t->second.inGame) { RemoveVote(i, out); continue; }
        if (!VotePasses(v)) continue;
        const std::string target = v.target;
        RemoveVote(i, out);
        KickPlayer(target, out);                      // `playerCallDisconnect(대상, 4)`
    }
}

// ── 묶음 8-f 모드별 죽음 셈 ───────────────────────────────────────────
//
// 정답지 `GAME5.c:5078~5085` 가 **게임 깃발로 갈래를 고른다** —
//   **256 아레나** → `playerUpdateScore_54D980`(7-c 에서 옮김)
//   **16 왕 죽이기(KOTR)** → `playerHandleKotrDeath_54DC40`(`GAME5.c:5444`)
//   **1024 탈락전** → `playerHandleElimDeath_54D7A0`(`GAME5.c:5139`)
//   ⚠ 셋 중 아무것도 아니면 **아무 셈도 안 한다**(깃발 판·퀘스트가 그 경우다)
//
// ⭕ 왕은 **왕관을 가진 사람**이다(`unitIsCrown_4E7BE0` 이 가방 사슬에서 `Crown` 을 찾는다).
//   ⭕ `Crown` 은 thing.bin 종류 **242** · 무게 1 · `PICKUP = CrownPickup` ·
//     ⭕ **우리 estate 맵에도 3개 있다**(맵 파서로 셈 — 157개 맵 거의 전부에 3개씩이다).
//     ⚠ `CrownPickup` 갈래는 아직 안 옮겨 **보통 줄기**로 가방에 들어간다 — 왕 판정에는 그것으로 족하다
//
// ⭕ KOTR 점수 ⭕ gamedata.bin 「# KotR definitions」 —
//   `KotRPawnKillsKingPoints` **1** · `KotRKingKillsPawnPoints` **1** · `KotRKingKillsKingPoints` **3**
//
// ⭕ KOTR 규칙(`playerHandleKotrDeath_54DC40`) —
//   · **자살이거나 같은 편** — 죽인 이가 왕이 **아니면** 죽은 횟수만 +1 ·
//     왕이**면** 죽인 이 점수 **−1**, 그 팀 **−1**, 죽은 횟수 +1
//   · **편이 다르거나 편이 없음** —
//     둘 중 하나라도 왕이면 점수를 준다. 죽인 이가 왕이면 `왕이 졸을` 점수(1),
//     죽은 이가 왕이면 `졸이 왕을` 점수(1), **둘 다 왕이면 3**.
//     편이 있으면 **팀 점수도 같은 만큼** 오른다. 그리고 죽은 횟수 +1
//   · 아무도 왕이 아니면 **죽은 횟수만** 오른다
//
// ⭕ 탈락전 규칙(`playerHandleElimDeath_54D7A0`) — 점수는 **적을수록 좋다**(죽은 수를 센다).
//   · **자살** — 점수 **−1** · 죽은 횟수 +1 · **제 팀 +1**
//   · **같은 편** — 죽인 이 점수 **−1**
//   · 그 밖(편이 없거나 다른 편) — 죽인 이 점수 **+1**
//   · 그리고 **늘** 죽은 횟수 +1 과 **죽은 이 팀 +1**
//   ⚠ 아레나와 달리 **자살도 죽은 횟수가 오른다**
bool Server::IsCrown(const Session& s) const {
    for (const auto& c : s.inventory) if (c.type == kCrownType) return true;
    return false;
}

// `playerHandleKotrDeath_54DC40`
void Server::KotrDeath(Session& victim, Session* killer, std::vector<OutPacket>& out) {
    if (killer == nullptr) { IncrementDeaths(victim, out); return; }
    const bool sameSide = (killer == &victim) || (victim.team != 0 && killer->team == victim.team);
    if (sameSide) {
        if (IsCrown(*killer)) {
            ChangeScore(*killer, -1, out);
            if (killer->team != 0)
                if (const Team* t = TeamById(killer->team))
                    TeamChangeScore(killer->team, t->score - 1, out);
        }
        IncrementDeaths(victim, out);
        return;
    }
    const bool kKing = IsCrown(*killer), vKing = IsCrown(victim);
    if (kKing || vKing) {
        const int32_t pts = (kKing && vKing) ? kKotrKingKillsKing
                          : (kKing ? kKotrKingKillsPawn : kKotrPawnKillsKing);
        ChangeScore(*killer, pts, out);
        if (killer->team != 0)                         // 편이 있으면 팀 점수도
            if (const Team* t = TeamById(killer->team))
                TeamChangeScore(killer->team, t->score + pts, out);
    }
    IncrementDeaths(victim, out);
}

// `playerHandleElimDeath_54D7A0`
void Server::ElimDeath(Session& victim, Session* killer, std::vector<OutPacket>& out) {
    if (killer == &victim || killer == nullptr) {
        ChangeScore(victim, -1, out);                  // 자살
    } else {
        const bool sameTeam = victim.team != 0 && killer->team == victim.team;
        ChangeScore(*killer, sameTeam ? -1 : +1, out);
    }
    IncrementDeaths(victim, out);                      // ⚠ 자살도 오른다(아레나와 다른 점)
    if (victim.team != 0)                              // 죽은 이의 팀이 **오른다**(적을수록 좋으니 벌점이다)
        if (const Team* t = TeamById(victim.team))
            TeamChangeScore(victim.team, t->score + 1, out);
}

// ── 묶음 9-a 비밀벽 여닫기 ───────────────────────────────────────────
//
// 정답지 — 마디 읽기 `mapRWSecretWalls_4297C0`(`GAME1_2.c:1064`) ·
//   상태 기계 `nox_xxx_updateUnits_51B100_D`(`server__system__server.c:913`) ·
//   사람별 알림 `sub_519660`(`server.go:598`) ·
//   보내기 `sub_4DF120`(열림 59) · `sub_4DF180`(닫힘 60)(`GAME3_2.c:6108·6127`).
//
// ⭕ **와이어는 3바이트다** — `3b/3c | 번호 u16`. `netSendPacket0` 이라 **봉투(cc) 없이**
//   중요 대기열에 맨 메시지로 실린다(확인이 올 때까지 다시 실린다).
// ⭕ **번호는 맵을 읽은 차례**다(0,1,2…). 벽 구조 `+10` 에 매겨지고,
//   벽이 **실제로 있는 칸**에만 매겨진다(`GAME1_2.c:1125~1131`).
// ⭕ **받는 쪽은 애니를 안 돌린다** — 열림이면 칸을 23, 닫힘이면 0 으로 **툭 놓는다**
//   (`cdecode.c:651·665`). 그래서 서버도 애니가 **끝난 뒤에야** 반대쪽을 보낸다.

// ── 묶음 9-b 쏘는 덫 ─────────────────────────────────────────────────
//
// ⭕ 맵 실측 — `ArrowTrap1` 477(26맵) · `ArrowTrap2` 326(29맵) ·
//   `Skull1` 9 · `Skull2` 8 · `Skull3` 30 · `Skull4` 36 → **합계 886**.
// ⭕ gamedata `ArrowTrapDamage` = **25** · `MercArcherArrow` 속도 800 · 닿음 `MonsterArrowCollide 10 10`.
//
// 주기 `nox_xxx_updateShootingTrap_54F9A0` 를 그대로 옮긴다 —
//   · 깃발에 **ENABLED**(0x1000000)가 없으면 아무것도 안 하고 「켜져 있었나」를 끈다
//   · 켜진 첫 프레임이면 세는 값 둘을 0 으로
//   · 살피는 값이 0 이면 **다시 살피고** 30프레임을 채운다
//   · **겨눈 것이 있고 재는 값이 0**이면 쏘고 30프레임을 잰다
//   · 두 값을 각각 1 씩 내린다
void Server::SetShootingTraps(const std::vector<ShootingTrapIn>& list) {
    traps_.clear();
    traps_.reserve(list.size());
    for (const ShootingTrapIn& in : list) {
        ShootingTrap t;
        t.x = in.x; t.y = in.y; t.facing = in.facing;
        t.proj = in.proj; t.fxKind = in.fxKind; t.damage = in.damage;
        traps_.push_back(t);
    }
}

// `sub_54FBF0` — 덫 둘레 ±350 네모 안에 **겨눌 것**이 있나.
//   ① 사람·소환수 ② 죽지 않았음 ③ 적 ④ 시야가 통함 ⑤ **덫이 보는 쪽**
bool Server::TrapSeesEnemy(const ShootingTrap& t) const {
    // ★ 2026-09-18 ⑫ — **어느 문에서 걸리는지 갈래를 센다.**
    //   「겨눈 덫 0」만으로는 사정거리·방향·벽 셋 중 어디서 막히는지 못 가른다.
    //   실측(G_Temple, 사람 5206,1239)에서 덫 100개 중 **44개가 350 안**인데 겨눈 덫이 0이었다.
    auto ok = [&](double ox, double oy) {
        if (std::fabs(ox - t.x) > 350.0 || std::fabs(oy - t.y) > 350.0) { ++trapFailRange_; return false; }
        // ⑤ `testTwoPointsAndDirection_4E6E50(덫자리, 덫방향, 상대자리) & 1`
        if (!(TestTwoPointsAndDirection(t.x, t.y, t.facing, static_cast<float>(ox), static_cast<float>(oy)) & 1)) {
            ++trapFailFacing_;
            return false;
        }
        // ④ `mapCheck_537110` — 벽 시야선
        if (!MapTraceRay(t.x, t.y, static_cast<float>(ox), static_cast<float>(oy), 0x9)) {
            ++trapFailWall_;
            return false;
        }
        ++trapSeeOk_;
        return true;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.observer) continue;
        if (o.moveState == 3 || o.moveState == 4) continue;   // 죽었으면 안 본다(+16 & 0x8020)
        if (ok(o.posX, o.posY)) return true;
    }
    for (const auto& k : pets_) {
        if (k.health == 0) continue;                      // 죽었으면 안 본다
        if (ok(k.x, k.y)) return true;
    }
    return false;
}

// `nox_xxx_sendArrowTrapFX_5238A0` — **6바이트** `a1 | x i16 | y i16 | 갈래 u8`
void Server::SendArrowTrapFx(float x, float y, uint8_t kind, std::vector<OutPacket>& out) {
    const int16_t ix = static_cast<int16_t>(x), iy = static_cast<int16_t>(y);
    const uint8_t msg[6] = { static_cast<uint8_t>(Op::FX_ARROW_TRAP),
                             static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                             static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8), kind };
    SendFxAllCli(x, y, msg, sizeof(msg), out);
}

// `nox_xxx_createArrowTrapProjectile_54FA80` — 덫 앞에 쏘는 것을 놓는다
void Server::TrapShoot(ShootingTrap& t, std::vector<OutPacket>& out) {
    const ProjTypeInfo* pt = ProjType(t.proj);
    if (!pt || pt->type == 0) return;                        // 종류를 모르면 안 만든다(지어내지 않는다)
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    m.type = pt->type;
    m.flags = pt->flags;
    // 자리 = 덫 자리 + (반지름 + 4) × 방향단위벡터. 덫 반지름은 thing.bin EXTENT CIRCLE 8
    const double r = 8.0 + 4.0;
    m.posX = m.newX = m.prevX = static_cast<float>(t.x + DirVecX(t.facing) * r);
    m.posY = m.newY = m.prevY = static_cast<float>(t.y + DirVecY(t.facing) * r);
    m.dir1 = m.dir2 = t.facing;
    m.createdFrame = m.frame136 = frame_;
    m.speed = pt->speed;
    m.velX = DirVecX(m.dir1) * m.speed;
    m.velY = DirVecY(m.dir1) * m.speed;
    // 닿음 갈래 — 화살은 `MonsterArrowCollide`(5), 불덩이는 `SparkExplosionCollide`(1)
    m.collideKind = (t.damage != 0) ? 5 : 1;
    m.collideAmount = (t.damage != 0) ? static_cast<uint8_t>(t.damage) : pt->collideAmount;
    missiles_.push_back(m); ++missilesMade_;
    // ★ 2026-09-18 ⑫ — **동시에 떠 있는 최대 수**를 센다.
    //   틱 로그의 「나는것」은 `frame % 30 == 0` 일 때만 찍는데 **덫 발사 주기도 30틱**이라
    //   늘 같은 위상만 본다(화살이 이미 사라진 순간). 그래서 31틱 전부 0 으로 보였다.
    //   이 값은 주기와 무관하므로 「정말 안 나는가」를 가른다.
    if (missiles_.size() > missilesPeak_) missilesPeak_ = missiles_.size();
    if (t.fxKind != 0) SendArrowTrapFx(t.x, t.y, t.fxKind, out);
    // 10-a — 쏘는 소리. 정답지 `createArrowTrapProjectile_54FA80` 이 `aud_501960(889, …)` 를 부른다.
    //   ⭕ 889 = **`ArrowTrapShoot`**(이름표로 확인). 쏘는 것이 화살일 때만 울린다
    if (t.proj == "MercArcherArrow") PlaySoundAt(889, t.x, t.y);
}

void Server::ShootingTrapUpdate(std::vector<OutPacket>& out) {
    // ★ 조각 D — **겨눈 덫 수를 매 틱 다시 센다.** 맵에 덫이 32개인데 초당 화살이 4발이면
    //   「28개가 고장났나」와 「28개는 봇을 못 보는 게 맞나」가 안 갈린다. 이 값이 그 자리다.
    trapsArmed_ = 0;
    for (ShootingTrap& t : traps_) {
        if (!t.enabled) { t.wasEnabled = false; continue; }   // 깃발 0x1000000 이 없으면 아무것도 안 한다
        if (!t.wasEnabled) { t.scanCountdown = 0; t.fireCountdown = 0; }
        t.wasEnabled = true;
        if (t.scanCountdown == 0) {                           // `sub_54FBB0` — 다시 살핀다
            const bool sees = TrapSeesEnemy(t);
            if (sees) { if (!t.armed) { t.armed = true; t.fireCountdown = 0; } }
            else t.armed = false;
            t.scanCountdown = kServerTickRate;                // `*v2 = gameFPS()`
        }
        if (t.armed && t.fireCountdown == 0) {
            TrapShoot(t, out);
            t.fireCountdown = kServerTickRate;                // `v2[1] = 30`
        }
        if (t.armed) ++trapsArmed_;
        if (t.scanCountdown) --t.scanCountdown;
        if (t.fireCountdown) --t.fireCountdown;
    }
}

// ── 묶음 10-a 소리 ───────────────────────────────────────────────────
//
// 정답지 — 담기 `newAudioEventObj`·`newAudioEventPos`(`server/audio_event.go:137·162`) ·
//   고르기·보내기 `NetUpdateRemotePlrAudioEvents`(`src/audio_event.go:13`) ·
//   와이어 `netSendAudioEvent`(`src/network.go:557`) · 거리 `soundFadePerc`(`src/unit_ai.go:386`).
//
// ⭕ **와이어는 4바이트다** —
//   `[0]` 이 소리 임자가 **받는 사람 자신이면 `0xA7`**, 아니면 **`0xA6`**
//   `[1]` = 좌우 치우침 `int8(50 × (소리x − 사람x) / (창너비/2))`
//   `[2..3]` = u16 **`소리번호 | (세기 << 10)`**
// ⭕ **소리 이름이 정확히 1,023개**라 아래 10비트에 딱 들어맞는다(`SoundNames.h`).
// ⚠ **창너비는 우리 값이다**(➖). 정답지는 **host 자신의 창**을 보는데 전용 서버엔 창이 없다.
//   녹스 기본 640 의 절반인 320 을 쓴다 — 이 값은 좌우 치우침에만 쓰이고 들림 여부는 안 가른다
// ── 묶음 10-c 음악 ───────────────────────────────────────────────────
//
// 정답지 `server/network.go:290~313` — 넷 다 **3바이트**·**모두에게**·**순번 봉투**다.
// ⚠ `Push`·`Pop`·`Event` 는 **뒤 두 바이트를 안 채운다**(`var buf [3]byte` 뒤 `buf[0]` 만 쓴다) —
//   즉 **0 0** 이 그대로 나간다. 지어내지 말고 그대로 0 을 보낸다.
// ── 묶음 10-d 떠남·강퇴 알림 ─────────────────────────────────────────
//
// `ae <번호 u16>` **3바이트** — 받는 이가 `pli | 0x80` 이라 **그 사람만 빼고 모두**다.
// ⚠ 봉투가 없다(`NetSendPacketXxx0(…, 0, 0)`) — 중요 대기열에도 안 들어간다.
//   떠나는 사람 본인에게는 안 간다(자기 자리는 자기가 안다)
void Server::SendOutgoingClient(uint16_t netCode, const std::string& except,
                                std::vector<OutPacket>& out) {
    if (netCode == 0) return;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::OUTGOING_CLIENT),
                             static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8) };
    for (auto& kv : sessions_) {
        if (kv.first == except) continue;
        if (!kv.second.inGame) continue;
        Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
}

// ── 묶음 10-e 일지·장 끝 ─────────────────────────────────────────────
//
// `d5 <갈래> <이름 64바이트> <u16>` **68바이트**.
// ⚠ 정답지는 `char v3[68]` 을 **안 비우고** `strcpy` 만 한다 — 이름 뒤에 쓰레기가 남는다.
//   받는 쪽은 NUL 까지만 읽으므로 뜻은 같다. 우리는 **0 으로 채운다**(같은 값을 두 번 보내면 같은 바이트가 나오게).
void Server::SendJournal(Session& to, JournalKind kind, const std::string& name, uint16_t value,
                         std::vector<OutPacket>& out) {
    uint8_t msg[68] = {};
    msg[0] = static_cast<uint8_t>(Op::JOURNAL_MSG);
    msg[1] = static_cast<uint8_t>(kind);
    const size_t n = name.size() < 63 ? name.size() : 63;   // [2..65] 예순넷 · 끝은 NUL
    std::memcpy(msg + 2, name.data(), n);
    // ⚠ 빼기(2)는 뒤 u16 을 **안 싣는다** — 정답지가 그 줄기에만 안 쓴다
    if (kind != JournalKind::Remove) {
        msg[66] = static_cast<uint8_t>(value);
        msg[67] = static_cast<uint8_t>(value >> 8);
    }
    EmitImportantRaw(to, msg, sizeof(msg), out);
}

void Server::SendJournalAll(JournalKind kind, const std::string& name, uint16_t value,
                            std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) if (kv.second.inGame) SendJournal(kv.second, kind, name, value, out);
}

// `netSendChapterEnd_4D9560` — `d6 <장> <끝인가>` **3바이트**
void Server::SendChapterEnd(Session& to, uint8_t chapter, bool ended, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::CHAPTER_END), chapter,
                             static_cast<uint8_t>(ended ? 1 : 0) };
    EmitImportantRaw(to, msg, sizeof(msg), out);
}

void Server::MusicPlay(uint8_t music, uint8_t volume, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::MUSIC_EVENT), music, volume };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++musicSent_;
}

void Server::MusicPush(std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::MUSIC_PUSH_EVENT), 0, 0 };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++musicSent_;
}

void Server::MusicPop(std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::MUSIC_POP_EVENT), 0, 0 };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++musicSent_;
}

// `NetMusicEvent` — `e5` 인데 **곡도 소리크기도 0** 이다(`Music(0, 0)` 과 같은 줄기)
void Server::MusicEvent(std::vector<OutPacket>& out) {
    MusicPlay(0, 0, out);
}

void Server::SetSoundDefs(const std::vector<SoundDefIn>& list) {
    soundDefs_.assign(kSoundCount, SoundDef{});
    for (const SoundDefIn& in : list) {
        if (in.id == 0 || in.id >= kSoundCount) continue;
        SoundDef& d = soundDefs_[in.id];
        d.maxDist = in.maxDist;
        d.merged = in.merged;
        d.samples = in.samples;
        d.flags = in.flags;
    }
}

// `soundFadePerc` — 0~100. 최대거리 밖이면 0(= 안 보낸다)
int Server::SoundFadePerc(uint16_t id, float sx, float sy, double px, double py) const {
    if (id == 0 || id >= soundDefs_.size()) return 0;
    const int max = soundDefs_[id].maxDist;
    if (max <= 0) return 0;
    const double dx = static_cast<double>(sx) - px;
    const double dy = static_cast<double>(sy) - py;
    if (std::fabs(dx) >= max || std::fabs(dy) >= max) return 0;
    const int dist = static_cast<int>(std::sqrt(dy * dy + dx * dx + 0.1));
    if (dist >= max) return 0;
    int v = 100 * (max - dist) / max;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

void Server::PlaySoundAt(uint16_t id, float x, float y, uint8_t kind, uint32_t code,
                         const std::string& owner, const Target& src) {
    // ⚠ **소리조각이 0 이면 아예 안 만든다** — `newAudioEventPos` 의 `Field12 <= 0` 문.
    //   thing.bin 을 안 읽었으면 표가 비어 있어 **한 개도 안 나간다**(정답지와 같다)
    if (id == 0 || id >= soundDefs_.size() || soundDefs_[id].samples <= 0) { ++soundsDropped_; ++soundsDroppedIds_[id]; return; }
    AudioEvent e;
    e.sound = id;
    e.x = x;
    e.y = y;
    e.kind = kind;
    e.code = code;
    e.owner = owner;
    audioEvents_.push_back(std::move(e));
    ++soundsQueued_;
    // ★ 09-29 B6 — 몬스터가 들을 거리(`EventObj` → `onSound`(kind ≠ 2) → `NewSound` `unit_ai.go:289~312` · `server.go:652~656`)
    //   깃발 0 이면 버림 · 낸 것이 유닛이거나 유닛이 임자인 것만(`GetOwnerUnit() == nil` 이면 버림 — 자리 소리·문·판·공은 안 남는다)
    //   출처 = `src`(넘긴 유닛) · 없으면 `owner`(그 사람) · 자리 = 낸 것의 자리
    if (kind != 2 && soundDefs_[id].flags != 0) {
        ListenRec r;
        if (!src.none()) {
            if (!src.addr.empty()) r.srcAddr = src.addr;
            else if (static_cast<size_t>(src.pet) < pets_.size()) { r.srcIsPet = true; r.srcNet = pets_[static_cast<size_t>(src.pet)].netCode; }
        } else if (!owner.empty()) {
            r.srcAddr = owner;
        }
        if (!r.srcAddr.empty() || r.srcIsPet) {
            r.snd = id; r.x = x; r.y = y; r.frame = frame_;
            listens_.push_back(std::move(r));
            ++hearStat_[static_cast<size_t>(HearStat::Recorded)];
        }
    }
}

void Server::FlushAudioEvents(std::vector<OutPacket>& out) {
    if (audioEvents_.empty()) return;
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        // 이 사람이 들을 것만 고른다
        struct Cand { const AudioEvent* ev; int perc; };
        std::vector<Cand> cand;
        for (const AudioEvent& e : audioEvents_) {
            if (e.kind == 1 && s.team != static_cast<uint8_t>(e.code)) continue;   // 같은 팀만
            if (e.kind == 2 && s.netCode != static_cast<uint16_t>(e.code)) continue;  // 그 한 사람만
            // ★ 세기는 **반으로 나눠** 싣는다(`fade / 2`) — 그래서 실리는 값은 0~50 이고 6비트에 든다
            const int perc = SoundFadePerc(e.sound, e.x, e.y, s.posX, s.posY) / 2;
            if (perc <= 0) continue;
            cand.push_back(Cand{ &e, perc });
        }
        if (cand.empty()) continue;
        // ★ `merged`(정답지 `Field20`)가 0 이 아닌 소리는 **가장 큰 것 N개까지만** 간다
        //   (`addToList` 가 세기 내림차순 줄을 세우고 `EachEventBitmap` 이 N개에서 끊는다)
        std::vector<size_t> order(cand.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return cand[a].perc > cand[b].perc; });
        std::map<uint16_t, int> sent;
        for (size_t i : order) {
            const Cand& c = cand[i];
            const uint8_t cap = soundDefs_[c.ev->sound].merged;
            if (cap != 0 && ++sent[c.ev->sound] > cap) continue;
            const double dx = static_cast<double>(c.ev->x) - s.posX;
            const int8_t mv = static_cast<int8_t>(50 * static_cast<int>(dx) / kSoundPanHalfWidth);
            const uint16_t packed = static_cast<uint16_t>(c.ev->sound | (c.perc << 10));
            const uint8_t msg[4] = {
                static_cast<uint8_t>((!c.ev->owner.empty() && c.ev->owner == s.addr)
                                         ? Op::AUDIO_PLAYER_EVENT : Op::AUDIO_EVENT),
                static_cast<uint8_t>(mv),
                static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8) };
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
            ++soundsSent_;
        }
    }
    audioEvents_.clear();
}

void Server::SetSecretWalls(const std::vector<SecretWallIn>& list) {
    secretWalls_.clear();
    for (auto& kv : sessions_) kv.second.secretSeen.clear();
    if (wallTiles_.empty()) return;
    for (const SecretWallIn& in : list) {
        WallTile& t = wallTiles_[static_cast<size_t>(in.gy) * 256 + in.gx];
        if (!t.present) continue;                 // 벽이 없는 칸엔 안 단다(정답지도 그렇다)
        SecretWall w;
        w.gx = in.gx;
        w.gy = in.gy;
        w.id = static_cast<uint16_t>(secretWalls_.size());
        w.waitSecs = in.waitSecs;
        w.flags = in.flags;
        w.state = in.state;
        w.anim = in.anim;
        // 맵이 상태를 안 적어 두었으면 깃발 8 로 정한다(`GAME1_2.c:1132~1141`)
        if (w.state == 0) {
            if (w.flags & 8) { w.state = 3; w.anim = 23; }
            else             { w.state = 1; w.anim = 0; }
        }
        t.secret = static_cast<int16_t>(secretWalls_.size());
        secretWalls_.push_back(w);
    }
}

Server::SecretWall* Server::SecretWallAt(int gx, int gy) {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256 || wallTiles_.empty()) return nullptr;
    const int16_t i = wallTiles_[static_cast<size_t>(gy) * 256 + static_cast<size_t>(gx)].secret;
    if (i < 0 || static_cast<size_t>(i) >= secretWalls_.size()) return nullptr;
    return &secretWalls_[static_cast<size_t>(i)];
}

// `Wall.open`·`Wall.close`(`src/wall.go:152·172`) — **이미 그쪽으로 가고 있으면 아무것도 안 한다**
void Server::SecretWallSetState(SecretWall& w, uint8_t st) {
    if (st == 4) { if (w.state == 3 || w.state == 4) return; }
    else if (st == 2) { if (w.state == 1 || w.state == 2) return; }
    else return;
    w.state = st;
    // 10-a — 여닫는 소리. 정답지 `Wall.open`·`close`(`src/wall.go:152·172`)가 그 자리에서 울린다.
    //   ⚠ 정답지는 **벽 재질마다 소리가 다르다**(`wallFindOpenSound_410EE0`) —
    //     재질별 이름표를 아직 안 읽었으므로 **맨밑 이름**(`SecretWallOpen`·`Close`)을 쓴다.
    //     둘 다 thing.bin 에 있다(실측 — 들리는 거리 1500 · 한 번에 하나)
    const float wx = static_cast<float>(w.gx * kWallCell + 11.5);
    const float wy = static_cast<float>(w.gy * kWallCell + 11.5);
    PlaySoundAt(SoundByName(st == 4 ? "SecretWallOpen" : "SecretWallClose"), wx, wy);
}

// ⚠ 09-20 — 계수기를 뒤늦게 달았다. **0 을 「안 열렸다」와 「안 셌다」로 못 가르고 있었다**
void Server::SendOpenWall(Session& to, uint16_t id, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::OPEN_WALL),
                             static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8) };
    EmitImportantRaw(to, msg, sizeof(msg), out);
    ++wallOpenSent_;
}

void Server::SendCloseWall(Session& to, uint16_t id, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::CLOSE_WALL),
                             static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8) };
    EmitImportantRaw(to, msg, sizeof(msg), out);
    ++wallCloseSent_;
}

// `nox_xxx_updateUnits_51B100_D` 의 비밀벽 고리 — 매 프레임 상태를 한 칸씩 민다
void Server::SecretWallUpdate(std::vector<OutPacket>& out) {
    (void)out;
    for (SecretWall& w : secretWalls_) {
        switch (w.state) {
            case 1:                                   // 닫힘 — 깃발 4 와 8 이 **둘 다** 있어야 스스로 연다
                if (!((w.flags & 4) && (w.flags & 8))) break;
                // ⚠ 정답지엔 **0 검사가 없다**(`v48 = +24 − 1; +24 = v48; if (v48) break;`).
                //   0 에서 빼면 0xFFFFFFFF 로 돌아 사실상 안 열린다 — 그 손버릇까지 그대로 둔다
                if (--w.countdown == 0) w.state = 4;
                break;
            case 2:                                   // 닫는 중 — 칸을 내린다
                if (w.anim > 0) --w.anim;
                if (w.anim == 0) {
                    w.state = 1;
                    w.countdown = w.waitSecs * kServerTickRate;
                }
                break;
            case 3:                                   // 열림 — 깃발 4 는 있고 **8 은 없어야** 스스로 닫는다
                if (!(w.flags & 4) || (w.flags & 8)) break;
                if (--w.countdown == 0) w.state = 2;   // ⚠ 여기도 0 검사가 없다(위와 같다)
                break;
            case 4:                                   // 여는 중 — 칸을 올린다
                if (w.anim < 23) ++w.anim;
                if (w.anim == 23) {
                    w.state = 3;
                    w.countdown = w.waitSecs * kServerTickRate;
                }
                break;
            default: break;
        }
    }
    // ⛔ 움직이는 칸 둘레 ±42.5 의 사람 밀어내기(`getUnitsInRect_517C10` + `sub_51B860`)는 안 옮겼다 —
    //   무엇을 하는 함수인지 정답지에서 아직 못 짚었다
}

// `sub_519660` — 이 사람이 **아는 상태**와 **지금 상태**가 다르면 알린다.
//   ⚠ 상태 **1·4 는 닫힘 · 2·3 은 열림**이다(여는 중은 아직 닫힌 것으로 친다).
//   ⚠ 정답지는 사람 화면(반쪽 + 128) 네모 안 비밀벽만 본다 — 우리는 시야 반경(`kAoiRadius`)으로 대신한다
void Server::SecretWallSync(Session& s, std::vector<OutPacket>& out) {
    if (!s.inGame || secretWalls_.empty()) return;
    if (s.secretSeen.size() != secretWalls_.size()) {
        // 처음 보는 사람 — 깃발 8 대로 미리 켜 둔다(`sub_4DF2A0` · `wall.go:378`).
        //   ⇒ **처음부터 열린 벽은 접속 때 알림이 안 간다**(클라가 맵에서 이미 안다)
        s.secretSeen.assign(secretWalls_.size(), 0);
        for (size_t i = 0; i < secretWalls_.size(); ++i)
            s.secretSeen[i] = (secretWalls_[i].flags & 8) ? 1 : 0;
    }
    for (size_t i = 0; i < secretWalls_.size(); ++i) {
        const SecretWall& w = secretWalls_[i];
        const double wx = w.gx * kWallCell, wy = w.gy * kWallCell;
        const double dx = wx - s.posX, dy = wy - s.posY;
        if (dx * dx + dy * dy > kAoiRadius * kAoiRadius) continue;
        const bool open = (w.state == 2 || w.state == 3);
        if ((s.secretSeen[i] != 0) == open) continue;
        if (open) { SendOpenWall(s, w.id, out); s.secretSeen[i] = 1; }
        else      { SendCloseWall(s, w.id, out); s.secretSeen[i] = 0; }
    }
}

// ── 묶음 8-g 판이 끝나는 나머지 길 ───────────────────────────────────
//
// 정답지 — 시간 초과 갈래잡이 `GAME4.c:6919~6930` ·
//   `sub_5099B0`(`GAME4_1.c:120`) · `sub_5098A0`(`GAME4_1.c:57`) · `sub_5095E0`(`GAME4.c:6827`) ·
//   그리고 매 프레임 도는 `nox_server_checkVictory_509A60`(`GAME4_1.c:154` · `server.go:143`).
//
// ⭕ **시간이 다하면 모드로 갈린다**(`GAME4.c:6919~6930`) —
//   · 깃발 96(= 32 CTF | 64 플래그볼) → `sub_5099B0` —
//     **점수가 가장 높은 팀**. 동점이거나 팀이 없으면 **무승부(0)**.
//     플래그볼이면 `0x56`(까닭 늘 0) · 아니면 `0x57`(까닭 **1**)
//   · 깃발 272(= 16 왕 죽이기 | 256 아레나) → `sub_5098A0` —
//     **팀을 먼저** 훑어 최고 점수를 잡고, **이어서 사람**을 훑되 그 값을 **그대로 이어** 비교한다.
//     ⇒ 사람은 **최고 팀 점수 이상**이어야 이긴다. 동점이면 무승부.
//     사람이 이기면 `0x58`, 팀이 이기면 `0x59`. **까닭 1**(시간 초과)
//   · 깃발 1024(탈락전) → `sub_5095E0` — 같은 꼴인데 **죽은 횟수가 가장 적은** 쪽이다
//
// ⭕ **탈락전은 매 프레임 「살아남은 쪽이 하나인가」도 본다**(`checkVictory_509A60` 앞머리) —
//   목숨 한도(`servGamedataGet(모드)`)보다 **죽은 횟수가 적은** 사람만 셈에 넣고,
//   그들이 **한 팀뿐이면 그 팀**, **한 사람뿐이면 그 사람**, **아무도 없으면 무승부**.
//   둘 이상 남았으면 판이 이어진다. **까닭 0**
//
// ⚠ **시간 한도 값은 우리 캡처에 없다** — 한도 없이 돌았고 남은 시간이 음수로 내려갔다.
//   그래서 `GameSettings::timeLimitFrames` 를 두되 **기본 0(=없음)** 으로 하고 규칙만 옮긴다
void Server::RoundTimeUp(std::vector<OutPacket>& out) {
    if (roundOver_) return;
    const uint32_t f = gameSettings_.gameFlags;
    // ⚠ 「판 끝」 깃발(`setGameFlags_40A4D0(8)`)은 **갈래 안에서** 선다 —
    //   세 갈래 어디에도 안 걸리는 모드면 깃발도 안 서고 알림도 없다(`GAME4.c:6919~6930`)
    auto markOver = [&]() { roundOver_ = true; restartAtFrame_ = frame_ + kRoundRestartTicks; };

    if (f & 0x60u) {                                      // 96 — 깃발 판·플래그볼
        int32_t best = -1;                                // ⚠ `sub_5099B0` 은 **−1** 에서 시작한다
        uint8_t who = 0;
        bool tie = false;
        for (const Team& t : teams_) {
            if (t.score < best) continue;
            tie = (t.score == best) && who != 0;
            best = t.score;
            who = t.id;
        }
        if (tie) who = 0;                                 // 동점이면 무승부
        markOver();
        if (f & 0x40u) SendFlagBallWinner(who, out);      // 플래그볼 — 까닭 늘 0
        else           SendFlagWinner(who, out);          // 깃발 판 — 까닭 1(그 함수가 넣는다)
        return;
    }

    const bool elim = (f & 0x400u) != 0;
    if (!(f & 0x110u) && !elim) return;                   // 272 도 1024 도 아니면 알릴 것이 없다

    // 팀을 먼저, 그다음 사람 — **같은 값을 이어** 비교한다(정답지 그대로).
    // ⚠ `sub_5098A0` 의 `v2 = 2147483648` 은 **부호 있는 int** 라 실제로는 최솟값이다
    int32_t best = elim ? INT32_MAX : INT32_MIN;
    uint8_t team = 0;
    uint16_t player = 0;
    bool tie = false;
    auto better = [&](int32_t v) { return elim ? (v <= best) : (v >= best); };
    // ⚠ **빈 팀도 셈에 든다.** 팀 목록은 「만들어진 팀」이지 「사람이 든 팀」이 아니다
    //   (`serverTeams.First` 는 `active` 만 본다 · `wndGuiTeamCreate_4185B0` 은 빈 팀 둘을 만든다).
    //   ⇒ 빈 팀이 둘이면 둘 다 점수 0 이라 **동점**이 되고, 탈락전에서는 그 0 을
    //     죽은 횟수와 견주므로 사람은 0번 죽지 않는 한 못 이긴다 — 정답지가 그렇다
    for (const Team& t : teams_) {
        if (!better(t.score)) continue;
        tie = (t.score == best) && team != 0;
        best = t.score;
        team = t.id;
    }
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.observer || o.team != 0) continue;   // 팀에 든 사람은 팀으로 센다
        const int32_t v = elim ? static_cast<int32_t>(o.deaths) : o.score;
        if (!better(v)) continue;
        tie = (v == best) && player != 0;
        best = v;
        player = o.netCode;
    }
    markOver();
    if (tie)             SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_TEAM_WINNER), 0, 1, out);
    else if (player != 0) SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER), player, 1, out);
    else if (team != 0)   SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_TEAM_WINNER), team, 1, out);
}

// `nox_server_checkVictory_509A60` 앞머리 — 탈락전에서 **살아남은 쪽이 하나인가**
void Server::CheckVictorySurvivors(std::vector<OutPacket>& out) {
    if (roundOver_) return;
    if (!(gameSettings_.gameFlags & 0x400u)) return;      // 탈락전만
    const int32_t lives = static_cast<int32_t>(gameSettings_.limitValue);
    if (lives < 1) return;
    uint8_t team = 0;
    uint16_t lone = 0;
    bool any = false;
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.observer) continue;
        if (static_cast<int32_t>(o.deaths) >= lives) continue;   // 이미 탈락
        any = true;
        if (o.team != 0) {
            if (team != 0 && team != o.team) return;      // 두 팀이 남았다 — 판이 이어진다
            // ⚠ 정답지는 **여기서 무소속(v9)을 안 본다** — 무소속을 먼저 찾아 놓았어도
            //   팀 쪽이 나중에 들어오면 그냥 덮어써 **팀 승리**가 된다. 그대로 옮긴다
            team = o.team;
        } else {
            if (lone != 0 || team != 0) return;           // 둘 이상 남았다
            lone = o.netCode;
        }
    }
    (void)any;
    // `nox_xxx_gamePlayIsAnyPlayers_40A8A0`(`GAME1.c:498`) — **둘보다 많아야** 판이 끝난다.
    //   · 팀을 만들어 두었으면(플레이 깃발 4 · `wndGuiTeamCreate_4185B0`) **사람이 든 팀**의 수를 세고
    //   · 아니면 사람 수를 센다. 어느 쪽이든 **탈락 여부는 안 본다**(관전자만 뺀다)
    //   ⚠ 우리 서버엔 플레이 깃발이 없어 「팀을 만들어 두었나」를 `teams_` 가 비었나로 대신한다
    int head = 0;
    if (!teams_.empty()) {
        for (const Team& t : teams_)
            for (const auto& kv : sessions_)
                if (kv.second.inGame && !kv.second.observer && kv.second.team == t.id) { ++head; break; }
    } else {
        for (const auto& kv : sessions_) if (kv.second.inGame && !kv.second.observer) ++head;
    }
    if (head <= 1) return;
    roundOver_ = true;
    restartAtFrame_ = frame_ + kRoundRestartTicks;
    if (team != 0)      SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_TEAM_WINNER), team, 0, out);
    else if (lone != 0) SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER), lone, 0, out);
    else                SendRoundWinner(static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER), 0, 0, out);
}

// ── 묶음 6-a AI 행동 쌓기 ─────────────────────────────────────────────
//
// 정답지 `server/object_ai.go`(밀기·빼기) + `unit_ai.go:575~610`(매 틱 굴리기).
// 몬스터는 **할 일을 쌓아 두고 맨 위를 한다.** 여기까지가 틀이고, 행동 하나하나는 다음 조각이다.
//
// ⭕ 행동 갈래는 **0~38**(`common/unit/ai` ActionType).
//   0 놀기 · 4 지키기 · 5 사냥 · 7 가기 · 14 주인에게 · 15 싸움 · 16 근접 · 24 달아나기 ·
//   26 바라보기 · 29 아무 데나 걷기 · 30 죽어가는 중 · 31 죽음 · 38 없음
//
// ★ **조건(Condition)** 갈래가 따로 있다 — 빼기가 맨 위를 내린 뒤 **그 아래 조건들도 같이** 내린다.
//   ⭕ `ActionType.IsCondition()`(`common/unit/ai/ai.go:22` · `monsterActionIsCondition_50A010`) —
//     **40 `DEPENDENCY_OR` ~ 71 `DEPENDENCY_NOT_MOVED`** 가 조건이다.
//     즉 **0~38 은 할 일**이고 **40~71 은 그 할 일에 붙는 조건**이다(39 는 비어 있다).
//     보기 — 42 살아 있나 · 45 보이나 · 49 더 머나 · 53 보이는 적 · 64 성치 않나 · 70 안 답답한가
bool Server::AIIsCondition(uint8_t act) {
    return act >= 40 && act <= 71;
}

// monsterActionReset_50A0C0 — 행동을 바꿀 때 딸린 셈들을 0 으로 하고 **바꾼 프레임을 적는다**.
//   ⭕ 정답지는 `+124` 와 `+137` 에 **같은 프레임**을 적는다(`object_ai.go:60~61`).
//   ⚠ 함께 0 이 되는 셈들(Field2·67·74·91·120…)은 우리 `Creature` 에 아직 없다 — 생기면 여기에 붙인다.
void Server::AIActionReset(Creature& k) {
    k.aiActionFrame = frame_;
    k.stuckFrame = frame_;          // ★ 09-29 B4b — `+124`(+496) 는 답답함 시계와 같은 칸이다 — 행동이 바뀌면 0.5초를 새로 잰다
    k.animFrame = 0;                // ★ 09-29 B5 — `Field120_1~3`(+481~+483) — 애니를 첫 칸부터(칸 수 +480 은 그대로)
    k.animTick = 0;
    k.animDone = false;
    k.aiMorphReady = false;
}

// monsterPushAction_50A260 — 할 일을 하나 올린다. 못 올리면 거짓.
//   · 쌓임이 꽉 차면 안 올린다
//   · 맨 위가 **죽음(31)** 이면 **죽어가는 중(30)** 말고는 안 올린다
//   · 맨 위가 **놀기(0)** 이고 바닥이면 **덮어쓴다**(쌓지 않는다)
//   · 그 밖에는 맨 위를 **취소(Cancel)** 하고 그 위에 올린다
bool Server::AIPush(Creature& k, uint8_t act, const Target& targ, float x, float y) {
    if (k.aiStackInd >= Creature::kAIStackMax - 1) return false;
    int ind = k.aiStackInd;
    const uint8_t cur = k.aiStack[ind].action;
    if (cur == 31 && act != 30) return false;                // 죽은 것에는 안 올린다
    if (cur == 0 && ind == 0) {
        ind = -1;                                            // 바닥의 놀기는 덮어쓴다
    } else if (!AIIsCondition(cur) && k.aiStack[ind].started) {
        AICancel(k, cur);
        // ★ 09-30 — 취소가 빼기면 자리가 내려간다 — 정답지는 취소 **뒤** 자리에서 올린다(`object_ai.go:105~111` `ud.AIStackInd++`).
        //   ✖ 전에는 취소 전 자리를 써서, 아래에 다른 할 일이 깔려 있으면 **뺀 것이 그 자리에 그대로 남았다**(바닥일 때만 우연히 같았다)
        ind = k.aiStackInd;
    }
    ++ind;
    k.aiStack[ind] = Creature::AIStackItem{};
    k.aiStack[ind].action = act;
    k.aiStack[ind].targetAddr = targ.addr;
    k.aiStack[ind].targetPet = targ.pet;
    k.aiStack[ind].argX = x;
    k.aiStack[ind].argY = y;
    k.aiStackInd = ind;
    AIActionReset(k);
    aiStackChanged_ = true;
    return true;
}

// monsterPopAction_50A3A0 — 맨 위를 끝내고 내린다.
//   내린 뒤 **그 아래 조건 항목들도 같이** 내린다. 다 내려가면 바닥에 **놀기(0)** 가 남는다.
int Server::AIPop(Creature& k) {
    const uint8_t cur = k.aiStack[k.aiStackInd].action;
    if (!AIIsCondition(cur) && k.aiStack[k.aiStackInd].started) AIEnd(k, cur);
    aiStackChanged_ = true;
    --k.aiStackInd;
    for (; k.aiStackInd >= 0; --k.aiStackInd)
        if (!AIIsCondition(k.aiStack[k.aiStackInd].action)) break;
    AIActionReset(k);
    const int si = k.aiStackInd;
    if (si < 0) {
        k.aiStackInd = 0;
        k.aiStack[0] = Creature::AIStackItem{};              // 바닥은 놀기(0)
    }
    return si;
}

// `unit_ai.go:581~603` — 매 틱. 맨 위가 아직 **안 시작했으면 시작**하고,
//   시작이 쌓임을 바꿨으면 **다시 맨 위를 본다**. 그러고 **마지막 맨 위를 갱신**한다.
//   ⚠ 앞에 붙는 것(체력 회복·시야 갱신·`monsterMainAIFn_547210` 두뇌·애니)은 다음 조각이다.
void Server::AITick(Creature& k, std::vector<OutPacket>& out) {
    const bool changedPrev = aiStackChanged_;
    uint8_t curAct = 0;
    for (;;) {
        Creature::AIStackItem& cur = k.aiStack[k.aiStackInd];
        curAct = cur.action;
        if (cur.started) {
            if (!aiStackChanged_) aiStackChanged_ = changedPrev;
            break;
        }
        cur.started = true;
        aiStackChanged_ = false;
        AIStart(k, curAct, out);
        if (!aiStackChanged_) {
            aiStackChanged_ = changedPrev;
            break;
        }
    }
    AIUpdate(k, curAct, out);
}

// ── 행동 하나하나(`server/object_ai_actions.go`) ──────────────────────
//
// ⭕ 정답지에서 **Go 로 옮겨진 행동은 여섯뿐**이다 — 기다리기(1) · 상대 기다리기(2) ·
//   상자로 변신(33) · 제 모습으로(34) · 떨구기(12, 아무것도 안 함) · 찾기(13, 아무것도 안 함).
//   나머지(가기 7 · 싸움 15 · 바라보기 26 …)는 아직 **C 안에** 있다 — 다음 조각이다.
//
// 시작은 넷 다 **아무 일도 안 한다**(정답지 `Start(_ *Object) {}`).
// sub_50A040(`legacy/GAME4_1.c:391`) — 맨 위 **아래**의 행동. ★ **조건은 건너뛴다**.
//   없으면 38(없음). 가기(7)가 「내 아래가 호위(3)인가」를 이것으로 묻는다
uint8_t Server::AIActionBelow(const Creature& k) {
    for (int i = k.aiStackInd - 1; i >= 0; --i)
        if (!AIIsCondition(k.aiStack[i].action)) return k.aiStack[i].action;
    return 38;
}

// nox_xxx_monsterIsActionScheduled_50A090 — 그 행동이 **쌓임 어딘가에** 있나
bool Server::AIHasAction(const Creature& k, uint8_t act) {
    for (int i = 0; i <= k.aiStackInd && i < Creature::kAIStackMax; ++i)
        if (k.aiStack[i].action == act) return true;
    return false;
}

// 세워 둔 속도로 자리를 옮긴다. 벽은 **x·y 를 따로** 보아 한 축이 막혀도 미끄러진다
void Server::CreatureApplyVelocity(Creature& k) {
    const double nx = k.x + k.velX, ny = k.y + k.velY;
    if (!WallAt(nx, k.y)) k.x = nx;
    if (!WallAt(k.x, ny)) k.y = ny;
}

void Server::AIStart(Creature& k, uint8_t act, std::vector<OutPacket>& out) {
    (void)out;
    switch (act) {
        case 15: {   // ★ 09-29 B2 — 싸움 시작 `nox_xxx_mobActionFightStart_531E20`(`GAME4_3.c:1791~1808`)
            //   교전 소리(+20 = 칸 4) · ALERT(0x100 — 전방위 시야) · 이 프레임 모두 다시 훑기(`frameCounterSetCopy_5281E0`) · 곧바로 알아보기 ·
            //   달리기(`sub_534750` — NEVER_RUN 이 아니면 0x4000 · 우리 몬스터 중 NEVER_RUN 없음 — monster.bin 에 Troll 뿐)
            //   ⚠ 스크립트 사건 13(교전) 안 옮김
            MonsterSound(k, 4);
            k.alert = true;
            sightForceFrame_ = frame_;
            UpdateSight(k);
            k.running = true;
            ++b2Stat_[static_cast<size_t>(B2Stat::FightStart)];
            break;
        }
        case 24:     // ★ 09-29 B4 — 도망 시작 `sub_544740` = 달리기(`534750`)
            k.running = true;
            break;
        case 17: {   // ── 묶음 6-g(3) 쏘기 시작 — `sub_532540`(`legacy/GAME4_3.c:2113~2140`)
            // 근접과 **같은 꼴**이다 — 쉬는 시간(+512)이 끝났으면 휘두르기를 시작하고
            // 다음 때를 **쏘기 간격**(`monsterDef+220`·`+224`)으로 잡는다.
            // ★ 09-30 — 쉬는 중이면 근접처럼 **50(쏘기 사거리 +212 × 1.2 · 지금 상대 +1196)·1(+512 까지)** 을 올린다(`:2128~2138`).
            //   ✖ 전에는 「정답지에 그 갈래가 없다」로 적고 아무것도 안 올렸다 — 틀림. 올리면 취소가 쏘기를 뺀다
            const MonsterStats* m = FindMonster(k.type);
            if (frame_ >= k.nextHitFrame) {
                k.attackStartFrame = frame_;
                const int lo = (m && m->missileMin > 0) ? m->missileMin : 30;
                const int hi = (m && m->missileMax > 0) ? m->missileMax : lo;
                const int span = (hi >= lo) ? (hi - lo + 1) : 1;
                k.nextHitFrame = frame_ + static_cast<uint32_t>(lo) + NextRandom(static_cast<uint32_t>(span));
                MonsterSound(k, 9);                          // ★ 09-26 M4a — MISSILE_ATTACK_INIT(+40 · `GAME4_3.c:2124~2127` result[10])
            } else {
                const double shootRange = m ? m->missileRange : 0.0;
                if (AIPush(k, 50)) {
                    k.aiStack[k.aiStackInd].argF = static_cast<float>(shootRange * 1.2);
                    k.aiStack[k.aiStackInd].targetAddr = k.enemyAddr;
                    k.aiStack[k.aiStackInd].targetPet = k.enemyPet;
                }
                if (AIPush(k, 1)) k.aiStack[k.aiStackInd].argU32 = k.nextHitFrame;
            }
            break;
        }
        case 31: {   // ── 묶음 6-j 죽음 시작 — `mobActionDead1_544D80`
            // **속도·힘을 0 으로** 두고 오브젝트 깃발에 `0x18` 을 얹는다(시체는 안 움직인다)
            //   ⚠ 풀려난 넋(`createReleasedSoul_544E60`)·좀비 시간표는 안 옮겼다
            k.velX = 0; k.velY = 0;
            k.path.clear();
            k.flagsSet |= 0x18;                              // ★ 10-01 — ALLOW_OVERLAP·SHORT(`GAME4_3.c:9893~9894` · 좀비 아님) — 길을 안 막고 낮다
            break;
        }
        case 16: {   // ── 묶음 6-g(2) 근접 공격 시작 — `mobActionMelee1_532130`(`legacy/GAME4_3.c:1943`)
            //
            // ⭕ **쉬는 시간이 끝났나**(`+512`)로 갈린다.
            //   · 끝났으면 — **휘두르기를 시작**하고 다음 쉬는 시간을 `randomInt(최소, 최대)` 로 잡는다
            //   · 아직이면 — **「대상이 가까이 있으면」 조건(50)** 을 깔고 **그때까지 기다리기(1)** 를 올린다.
            //     조건의 자는 **근접 사거리 × 1.2** 다(`monsterDef+112 * 1.2`)
            //
            // ⚠ **앞에 아군이 있나** 검사(`sub_532390` — 앞 60° 안에서 가장 가까운 것)와
            //   그때 미는 **각도 바라보기(27)·때(41)·도망(24)** 갈래는 아직 안 옮겼다 — 6-g(2b)
            // ⚠ 사람의 기력 깎기(깃발 0x10)와 소리는 안 옮긴다
            const MonsterStats* m = FindMonster(k.type);
            if (frame_ >= k.nextHitFrame) {
                k.attackStartFrame = frame_;                 // +136 휘두르기 시작 프레임
                const int lo = (m && m->minDelay > 0) ? m->minDelay : 30;
                const int hi = (m && m->maxDelay > 0) ? m->maxDelay : lo;
                const int span = (hi >= lo) ? (hi - lo + 1) : 1;   // randomInt 는 양끝 포함
                k.nextHitFrame = frame_ + static_cast<uint32_t>(lo) + NextRandom(static_cast<uint32_t>(span));
                MonsterSound(k, 5);                          // ★ 09-26 M4a — MELEE_ATTACK_INIT(+24 · `GAME4_3.c:2007~2010` result[6])
            } else {
                const double reach = (m && m->meleeRange > 0.0) ? m->meleeRange : 0.0;
                if (AIPush(k, 50)) {
                    k.aiStack[k.aiStackInd].argF = static_cast<float>(reach * 1.2);
                    k.aiStack[k.aiStackInd].targetAddr = k.enemyAddr;
                    k.aiStack[k.aiStackInd].targetPet = k.enemyPet;
                }
                if (AIPush(k, 1)) k.aiStack[k.aiStackInd].argU32 = k.nextHitFrame;
            }
            break;
        }
        default: break;
    }
}

// sub_545240(`GAME5.c:154`) — **한 점을 바라보게 돌린다**. 다 돌았으면 참(그러면 부르는 쪽이 뺀다).
//   · 그 점까지의 방향을 단위로 만든다(길이에 **0.001** 을 더해 0 나눗셈을 막는다 — 정답지 그대로)
//   · 지금 쪽과 **외적**을 재 어느 쪽으로 돌지 정하고 `돌아가려는 쪽`을 **8** 만큼 돌린다
//   · 지금 쪽과 **내적이 0.9 를 넘으면** 다 본 것이다(`sub_534120`)
bool Server::AIFacePoint(Creature& k, float tx, float ty) {
    const float dx = tx - static_cast<float>(k.x);
    const float dy = ty - static_cast<float>(k.y);
    const float len = std::sqrt(dx * dx + dy * dy) + 0.001f;
    const float nx = dx / len, ny = dy / len;
    const float cx = DirVecX(k.facing), cy = DirVecY(k.facing);
    if (ny * cx - nx * cy >= 0.0f) k.facingWant = static_cast<uint8_t>(k.facingWant + 8);
    else                            k.facingWant = static_cast<uint8_t>(k.facingWant - 8);
    return (cy * ny + cx * nx) > 0.89999998f;
}

// 갱신 — 여기가 알맹이다.
void Server::AIUpdate(Creature& k, uint8_t act, std::vector<OutPacket>& out) {
    Creature::AIStackItem& st = k.aiStack[k.aiStackInd];
    switch (act) {
        case 0:      // ★ 09-29 B2 — 놀기(`AIActionIdle.Update` `unit_ai.go:637~670`)
            MonsterIdleUpdate(k);
            break;
        case 24: {   // ★ 09-29 B4 — 도망 `nox_xxx_mobActionFlee_544760`(`GAME4_3.c:9619~9677`) — 스스로는 안 빠진다(아래 조건이 뺀다)
            //   · 움직일 수 없으면 뺀다 · 지금 상대가 있으면 도망칠 자리(인자)를 그 자리로 고치고, 도망 거리 안이면 0.5초마다 길을 다시 낸다
            //   · 길이 2초 넘게 묵었으면 버린다 · 길이 있거나 낸 지 10프레임 안이면 따라 걷는다 · 아니면 멀어지는 길을 내고(`50CA00`) 점 둘 이상이면 걷는다 ·
            //     못 내면 도망칠 자리를 제 자리로
            //   ⚠ 주문 몬스터의 도망 중 주문(`mobCastRelated`·`BuffSelf`·`CastOffensive`)은 B7 · 걸음 소리는 걸을 때(B5)
            if (k.speedCur < 0.0099999998f) { AIPop(k); break; }
            const Target e{ k.enemyAddr, k.enemyPet };
            float ex = 0, ey = 0;
            if (!e.none() && TargetXY(e, ex, ey)) {
                st.argX = ex; st.argY = ey;
                const float fr = MonsterBrainOf(k.type).fleeRange;
                const double dx = ex - k.x, dy = ey - k.y;
                if (static_cast<double>(fr) * fr > dx * dx + dy * dy && frame_ - k.pathFrame > static_cast<uint32_t>(kServerTickRate / 2)) k.path.clear();
                // ★ 09-29 B7 — 도망 중 주문(`GAME4_3.c:9646~9653`) — 주문 가능 · 반주문(29) 아님 · 성향 ≥ 0.33(`534400`·`534440` 아님)이면
                //   후퇴(6) 중이면 달아날 때 주문(`541050` · 자기) · 아니면 제 몸 주문(`540B90`) · 둘 다 못 했고 상대가 보이면 되받기(`540F20`)
                if ((MonsterBrainOf(k.type).status & 0x20) && !k.buffs.Has(29) && !(k.aggression < 0.33000001f)) {
                    const Target self = PetTarget(k);
                    const bool did = AIHasAction(k, 6)
                        ? MonsterCastPick(k, 0x80000000u, self, k.castNextEscape, 0, static_cast<int>(6 * kServerTickRate), CastStat::PickEscape)
                        : MonsterCastPick(k, 0x10000000u, self, k.castNextBuff, static_cast<int>(3 * kServerTickRate), static_cast<int>(10 * kServerTickRate), CastStat::PickBuff);
                    if (!did && CanInteractFrom(k, e))
                        MonsterCastPick(k, 0x20000000u, e, k.castNextReact, static_cast<int>(3 * kServerTickRate), static_cast<int>(6 * kServerTickRate), CastStat::PickReact);
                }
            }
            if (!k.path.empty() && frame_ - k.pathFrame > static_cast<uint32_t>(2 * kServerTickRate)) k.path.clear();
            bool go = !k.path.empty() || frame_ - k.pathFrame <= 10;
            if (!go) {
                const int n = CreatureGenRetreatPath(k, st.argX, st.argY);
                k.pathFrame = frame_;
                k.pathInd = 0;
                go = n > 1;
            }
            if (go) {
                if (!k.path.empty() && CreatureActuallyMove(k)) k.path.clear();
                MonsterMoveAudio(k, AnimStat::StepFlee);              // ★ 09-29 B5 — `GAME4_3.c:9667`
            } else {
                st.argX = static_cast<float>(k.x); st.argY = static_cast<float>(k.y);
                ++brainStat_[static_cast<size_t>(BrainStat::FleeNoPath)];
            }
            break;
        }
        case 6: {    // ★ 09-29 B4 — 후퇴 `nox_xxx_mobActionRetreat_545440`(`GAME5.c:250~284`)
            //   · 다시 싸울 수 있고(체력 비율 ≥ 회복 비율) 반주문에 안 걸렸으면(`5455B0`) 뺀다
            //   · 지금 상대가 있으면 — 반주문이거나 후퇴 주문(`mobCastRelated_541050` — B7)이 없으면 때(41 · 4~6초)·도망(24 그 자리)
            //   · 없으면 — 아직 다시 못 싸우면 먹을 것 찾기(`mobRetreatCheckEdibles_5455E0` — 먹기는 B4b · 지금은 없는 갈래만: 64·56·58·떠돌기)
            const MonsterBrainInfo bi = MonsterBrainOf(k.type);
            const bool anti = (bi.status & 0x20) && k.buffs.Has(29);
            if (MonsterCanResumeAttack(k) && !anti) { AIPop(k); break; }
            const Target e{ k.enemyAddr, k.enemyPet };
            float ex = 0, ey = 0;
            if (!e.none() && TargetXY(e, ex, ey)) {
                if (AIPush(k, 41)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(4 * kServerTickRate) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate + 1));
                if (AIPush(k, 24)) { k.aiStack[k.aiStackInd].argX = ex; k.aiStack[k.aiStackInd].argY = ey; k.aiStack[k.aiStackInd].argU32 = 0; }
            } else if (!MonsterCanResumeAttack(k)) {
                AIPush(k, 64);
                AIPush(k, 56);
                AIPush(k, 58);
                AIPush(k, 10);
            }
            break;
        }
        case 9: {    // ★ 09-29 B4b — 피하기 `nox_xxx_mobActionDodge_544640`(`GAME4_3.c:9577~9609`) — 움직일 수 없으면 뺀다 ·
            //   혼란(3)·붙들림(5)·홀림(28)이 아니면 인자 자리로 **뛰기 배율 × 빠르기**로 곧장(속도를 직접 세움) · 8 안이면 뺀다
            if (k.speedCur < 0.0099999998f) { AIPop(k); break; }
            if (k.buffs.Has(3) || k.buffs.Has(5) || k.buffs.Has(28)) break;
            const double dx = st.argX - k.x, dy = st.argY - k.y;
            const double d = std::sqrt(dx * dx + dy * dy) + 0.000099999997;
            if (d < 8.0) { AIPop(k); break; }
            const MonsterStats* dm = FindMonster(k.type);
            k.speedCur = static_cast<float>((dm ? dm->runMul : 1.0) * k.speedCur);
            k.velX = k.speedCur * dx / d;
            k.velY = k.speedCur * dy / d;
            break;
        }
        case 28: {   // ★ 09-29 B4b — 쪽 세우기 `sub_5453E0`(`GAME5.c:235~247`) — 보는 쪽을 인자(0~255)로 두고 뺀다
            k.facing = k.facingWant = static_cast<uint8_t>(st.argU32 & 0xFF);
            AIPop(k);
            break;
        }
        case 36: {   // ★ 09-29 B4 — 혼란 `nox_xxx_mobActionConfuse_545140`(`GAME5.c:119~146`) — 85% 아무 데나 걷기 ·
            //   15% 는 근접이 되면(주문 몬스터 아님 · 근접 사거리 > 0) 쏠 수 없거나 50% 로 근접(16) · 아니면 쏠 수 있으면 앞 10 지점 쏘기(17)
            if (NextRandom(101) >= 15) { AIRandomWalk(k); break; }
            const MonsterStats* mm = FindMonster(k.type);
            const bool canCast = (MonsterBrainOf(k.type).status & 0x20) != 0;
            const bool canMelee = !canCast && mm && mm->meleeRange > 0.0;          // `534220`
            const bool canShoot = mm && mm->missileName != nullptr;               // `534280`
            if (canMelee) {
                if (!canShoot || NextRandom(101) < 50) { AIPush(k, 16); break; }
            } else if (!canShoot) {
                break;
            }
            if (AIPush(k, 17)) {
                Creature::AIStackItem& it = k.aiStack[k.aiStackInd];
                it.targetAddr.clear(); it.targetPet = -1;
                it.argX = DirVecX(k.facing) * 10.0f + static_cast<float>(k.x);
                it.argY = DirVecY(k.facing) * 10.0f + static_cast<float>(k.y);
                it.argU32 = 0;
            }
            break;
        }
        case 25:     // 자리를 바라보기(`sub_545210`) — 인자로 받은 점
            if (AIFacePoint(k, st.argX, st.argY)) AIPop(k);
            break;
        case 26: {   // 무엇을 바라보기(`sub_545300`) — **대상이 없으면 그냥 뺀다**
            float tx = 0, ty = 0;
            const Target targ{ st.targetAddr, st.targetPet };
            if (!TargetXY(targ, tx, ty)) { AIPop(k); break; }
            if (AIFacePoint(k, tx, ty)) AIPop(k);
            break;
        }
        case 27:     // 각도를 바라보기(`sub_545340`) — 인자가 **방향 번호**다.
            //   그 번호의 단위 벡터를 「내 앞의 한 점」으로 삼으면 같은 셈이 된다
            if (AIFacePoint(k, static_cast<float>(k.x) + DirVecX(static_cast<int>(st.argU32)) * 100.0f,
                            static_cast<float>(k.y) + DirVecY(static_cast<int>(st.argU32)) * 100.0f))
                AIPop(k);
            break;
        case 1:      // 기다리기 — **그 프레임이 되면** 스스로 빠진다(`지금 >= 인자`)
            if (frame_ >= st.argU32) AIPop(k);
            break;
        case 2:      // 상대 기다리기 — **행동을 바꾼 프레임 + 인자**를 **넘기면** 빠진다
            //   ⚠ 기다리기(1)는 `>=`, 이쪽은 `>` 다. 정답지가 그렇게 갈라 놓았다
            if (frame_ > k.aiActionFrame + st.argU32) AIPop(k);
            break;
        case 33:     // 상자로 변신 — 「됐다」가 서면 빠지고 **변신 표시를 켠다**
            if (k.aiMorphReady) { AIPop(k); k.morphed = true; }
            break;
        case 34:     // 제 모습으로 — 빠지고 변신 표시를 **끈다**
            if (k.aiMorphReady) { AIPop(k); k.morphed = false; }
            break;
        case 7: {    // ── 묶음 6-f(4) 가기 — `nox_xxx_mobActionMoveTo_5443F0`(`legacy/GAME4_3.c:9487`)
            // ① **못 움직이면 그냥 뺀다**(`monsterIsMoveing_534320` = 빠르기 >= 0.01)
            if (!(k.speedCur >= 0.0099999998f)) { AIPop(k); break; }
            // ★ 묶음 6-h — **바로 아래가 호위(3)면 걷기·뛰기를 갈아탄다**(`sub_50A040 == 3`).
            //   잣대는 **호위 거리 × 3**(그보다 가까우면 걷기)과 **거기에 +30**(그보다 멀면 뛰기).
            //   ⚠ 그 사이는 **그대로 둔다** — 경계에서 달렸다 걸었다 하지 않게 하는 틈이다
            if (AIActionBelow(k) == 3) {
                const double near2 = k.escortDist * 3.0;
                const double far2 = near2 + 30.0;
                const double gx2 = st.argX - k.x, gy2 = st.argY - k.y;
                const double d2 = gx2 * gx2 + gy2 * gy2;
                if (d2 >= near2 * near2) { if (d2 > far2 * far2) k.running = true; }
                else k.running = false;
            }
            // ★ 09-29 B5 — 걸음 소리는 이 함수 **끝에서 늘** 부른다(`GAME4_3.c:9548`) — 아직 가는 중이어도 · 닿아 뺀 뒤에도(아래 `break` 셋 앞마다)
            if (CreatureSetMovePath(k, st.argX, st.argY) != 1) { MonsterMoveAudio(k, AnimStat::StepMoveTo); break; }   // 아직 가는 중

            // ② 길 찾기 **결과(+284)** 로 갈래가 갈린다
            const uint8_t ps = k.pathStatus;
            // **막힌 것으로 본다** — 결과 2 이거나, 결과 1 이 **5초 안에 두 번** 났을 때
            const bool stuck = (ps == 2) ||
                               (ps == 1 && (frame_ - k.pathStuckFrame) < static_cast<uint32_t>(5 * 30));
            if (ps == 1) k.pathStuckFrame = frame_;
            // ③ **제대로 닿았으면**(결과 0) 그쪽을 보고 **행동을 뺀다**
            //   ★ 09-28 — 단 **대상 오브젝트(+12)가 있으면 안 뺀다**(`GAME4_3.c:9529` `!*(v4 + 12)`). 그 자리는 매 틱
            //     대상 자리로 고쳐지므로(`AIRefreshStackArgs`) 닿아도 계속 따라가고, 빠지는 것은 아래 조건(49 더 머나)이 깨질 때다
            //   ⚠ `sub_547F10`(길목 모드)은 아직 안 옮겼다
            const bool hasObj = !Target{ st.targetAddr, st.targetPet }.none();
            if (ps == 0 && !hasObj) {
                // ★ `mobCalcDir_533CC0` 은 **돌아가려는 쪽(+126)만** 세운다 —
                //   보는 쪽(+124)은 건드리지 않는다(그쪽은 딴 데서 8씩 쫓아간다)
                k.facingWant = DirFromVec(st.argX - k.x, st.argY - k.y);
                AIPop(k);
                MonsterMoveAudio(k, AnimStat::StepMoveTo);
                break;
            }
            // ④ **막혔으면** — 2~4초짜리 **때(41) 조건**을 깔고 **아무 데나 걷기(29)** 를 올린다
            if (stuck) {
                const int idx = AIPush(k, 41) ? k.aiStackInd : -1;
                if (idx >= 0)
                    // `randomInt(2*FPS, 4*FPS)` — ★ **양끝을 다 넣는다**(`IntClamp`).
                    //   60~120 이니 값이 **61가지**다
                    k.aiStack[idx].argU32 = frame_ + 60 + NextRandom(61);
                AIPush(k, 29);
                k.frustrated = true;                         // ★ 09-29 B4b — FRUSTRATED 도 켠다(`GAME4_3.c:9539`)
            }
            // ⑤ 결과가 0 이 아니면 **잠깐 기다린다**(0.5~1초) — `GAME4_3.c:9541` `if (+284)`
            if (ps != 0 && AIPush(k, 1))
                k.aiStack[k.aiStackInd].argU32 = frame_ + 15 + NextRandom(16);   // randomInt(FPS/2, FPS) = 15~30
            MonsterMoveAudio(k, AnimStat::StepMoveTo);
            break;
        }
        case 3: {    // ── 묶음 6-h 호위 — `nox_xxx_mobActionEscort_546430`(`legacy/GAME5.c:924`)
            //
            // ⭕ 「주인 곁에 있어라」다. 셋으로 갈린다 —
            //   · 호위할 것이 **없으면 뺀다**(`monsterGetObjEscortName_546600`)
            //   · **지금 상대가 있으면 싸움(15)** 을 올린다(그쪽이 알아서 쫓고 때린다)
            //   · **너무 멀면**(호위 거리 + 30) 조건을 깔고 **가기(7)** 를 민다
            //     — 44 공격받고 있지 않나 · 56 보이는 적이 없나 · 49 **호위 거리보다 머나**
            //   · 가까우면 아무 일도 안 한다(치유 갈래 `mobHealSomeone_5411A0` 은 아직 안 옮겼다)
            //
            // ⚠ 우리 크리처의 호위 대상은 **부른 사람**이다 — 정답지의 이름표가 `**PLAYER**` 인 갈래
            // ⚠ `monsterCanAttackAtWill_534390`·`sub_5343C0` 는 안 옮겼다 — 소환수는 **늘 싸울 수 있다**로 본다
            const Target master{ k.owner, -1 };
            float mx = 0, my = 0;
            if (k.owner.empty() || !TargetXY(master, mx, my)) { AIPop(k); break; }
            st.argX = mx; st.argY = my;                       // +4·+8 — 호위 대상 자리
            // ★ 09-29 B6 — 성향 갈래를 정답지대로(`GAME5.c:948~1002`) — 마음대로면 지금 상대로 싸움 · 아니면 때린 쪽 돌아보기 /
            //   가운데면 되갚기 · 때린 쪽 돌아보기 / 곁이면 [마음대로면 들은 소리(`Sub_5466F0`)] · 못 했으면 치유(B7) /
            //   멀면 [가운데·마음대로면 44] · [마음대로면 56] · 49 · 7. 소환수는 0.83(마음대로)이라 싸움·멀 때는 겉이 같다
            const bool midE = k.aggression < 0.66000003f && k.aggression > 0.33000001f;
            const bool atWillE = k.aggression > 0.66000003f;
            if (atWillE) {
                const Target foe{ k.enemyAddr, k.enemyPet };
                float ex = 0, ey = 0;
                if (!foe.none() && TargetXY(foe, ex, ey)) {
                    if (AIPush(k, 15)) {
                        k.aiStack[k.aiStackInd].argX = ex;
                        k.aiStack[k.aiStackInd].argY = ey;
                        k.aiStack[k.aiStackInd].argU32 = frame_;
                    }
                    break;
                }
                if (MonsterLookAtDamager(k)) break;
            } else if (midE) {
                if (MonsterRetaliate(k)) break;
                if (MonsterLookAtDamager(k)) break;
            }
            const double ddx = k.x - mx, ddy = k.y - my;
            const double lim = k.escortDist + 30.0;
            if (ddx * ddx + ddy * ddy <= lim * lim) {         // 곁에 있다
                if (!atWillE || !MonsterHearReact(k)) {
                    // `mobHealSomeone_5411A0` — 치유 주문(B7 — 반주문 29 아니면)
                }
                break;
            }
            if (midE || atWillE) AIPush(k, 44);               // 공격받고 있지 않나
            if (atWillE) AIPush(k, 56);                       // 보이는 적이 없나
            if (AIPush(k, 49)) {                              // 호위 거리보다 머나
                k.aiStack[k.aiStackInd].argF = k.escortDist;
                k.aiStack[k.aiStackInd].targetAddr = k.owner;
                k.aiStack[k.aiStackInd].targetPet = -1;
            }
            if (AIPush(k, 7)) {
                k.aiStack[k.aiStackInd].argX = mx;
                k.aiStack[k.aiStackInd].argY = my;
            }
            break;
        }
        case 30:     // ── 묶음 6-j 죽어가는 중 — `sub_544D60`(`legacy/GAME4_3.c:9846~9854`)
            // 하는 일이 하나뿐이다 — **죽는 애니가 끝나면 빠진다**(`+483`).
            //   빠지면 아래에 깔아 둔 **죽음(31)** 이 드러난다
            // ★ 09-30 — 애니 칸(B5)으로 원본대로 **죽어가기 애니(9)가 끝나면** 뺀다. ✖ 전에는 애니 칸이 없던 때의 대신으로 첫 갱신에 바로 뺐다
            // ❌ 정정(10-01) — 「우리는 체력이 0 이면 곧바로 지운다 · 30 은 실제 판에서 안 쌓인다」는 이제 틀림:
            //   `KillPet` 이 죽음 함수가 되어 31·30 을 쌓는다(시체 · `ee7be4c`)
            if (k.animDone) AIPop(k);
            break;
        case 31:     // ── 묶음 6-j 죽음 — `mobActionDead2_544EC0`
            // 갱신 목록에서 빼고, 종류표가 그러라고 하면 **치운다**.
            //   ⚠ 좀비의 되살아나기(`mobRaiseZombie_534AB0`)와 불타 사라지기는 안 옮겼다
            // ★ 10-01 — `deadDone` 을 AI 루프가 읽는다(다음 틱부터 AI·애니 안 돎 = `unitRemoveFromUpdatable_4DA920` `GAME4_3.c:9942`) ·
            //   보이는 목록·지금 상대 비움(`sub_544F70` `:9952~9987`) · STATUS 0x1 DESTROY_WHEN_DEAD(Ghost·Bomber)면 지울 표시(`:9945~9947`)
            k.deadDone = true;
            k.seen.clear();
            k.enemyAddr.clear(); k.enemyNet = 0; k.enemyPet = -1;
            if (MonsterBrainOf(k.type).status & 0x1) k.removeMe = true;
            break;
        case 4: {    // ── 묶음 6-i(2) 지키기 — `nox_xxx_mobActionGuard_546010`(`legacy/GAME5.c:738`)
            //
            // ⭕ 「이 자리를 지켜라」다. 쌓임 항목의 `+4·+8` 이 **지킬 자리**, `+12` 가 **볼 쪽**이다.
            //   · **지금 상대가 있으면 싸움(15)** 을 올린다(쫓고 때리는 것은 그쪽 몫) — ✖ 09-29 정정: 마음대로일 때만 곧장(아래 ★★ ②)
            //   · 아니면 **16프레임에 한 번**만 자리를 살핀다(`(프레임 + 번호) & 0xF`) —
            //     지킬 자리에서 **8 넘게** 벗어났으면 **56·59·44** 를 깔고 **가기(7)** 로 되돌아간다
            //   ⚠ 매 틱 살피지 않는 것이 핵심이다 — 번호를 더해 **개체마다 살피는 틱이 어긋난다**
            //
            // ★★ 09-29 B6 — **정답지 전체로 다시 옮김**(`GAME5.c:738~915`). ✖ 전에는 「지금 상대가 있으면 늘 싸움」 + 자리 살피기뿐이었다 —
            //   지키기 명령(성향 0.5 = 가운데)이 **자리에서 먼 적까지 쫓아갔다**. 정답지 차례:
            //   ① 성향 < 0.08 이거나 되갚기가 0 일 때만 아래(되갚기가 쌓았으면 끝)
            //   ② 지금 상대가 있으면 — 마음대로(> 0.66)면 싸움(15) → 끝 · 가운데면 **상대가 지킬 자리에서 시야(+1312) 안일 때만**
            //      43(0)·52(시야 × 1.5 · 지킬 자리)·40(또는)·15 — 「맞고 있거나 자리 둘레에 있는 동안만 싸운다」(식충식물은 63(시야 × 1.05)·15) · 끝 아님
            //   ③ 때린 쪽 돌아보기(`5466B0`) → 끝 · 마음대로면 들은 소리(`Sub_5466F0`) · 아니면 들은 자리 바라보기(`545DA0`) → 했으면 끝
            //   ④ 16프레임에 한 번(`(프레임 + 번호) & 0xF`) — 자리에서 8 넘게 벗어났으면 [마음대로면 56·59] · 44 · 7(자리) → 끝
            //      제자리면 — 보이는 것이 있으면 지금 상대 · 없으면 보이는 목록의 첫 사람(없으면 첫째)을 바라봄(26 — 이미 보면 안 함) ·
            //      보이는 것이 없으면 볼 쪽(+12)을 봄(56·27 — 이미 보면 안 함)
            //   ⑤ 행동 바꾼 지 0.5초 안이거나 안 움직였으면 치유(B7 — 반주문 29 아니면) · 아니면 지난 자리 바라보기(25)
            //   ⚠ 흉내쟁이 갈래는 우리 몬스터에 없음
            {
                const bool midG = k.aggression < 0.66000003f && k.aggression > 0.33000001f;
                const bool atWillG = k.aggression > 0.66000003f;
                if (!(k.aggression < 0.079999998f || MonsterRetaliate(k) == 0)) break;           // ①
                const float postX = st.argX, postY = st.argY;
                const uint8_t postDir = static_cast<uint8_t>(st.argU32 & 0xFF);
                const Target foe{ k.enemyAddr, k.enemyPet };
                float ex = 0, ey = 0;
                if (!foe.none() && TargetXY(foe, ex, ey)) {                                     // ②
                    if (atWillG) {
                        if (AIPush(k, 15)) { k.aiStack[k.aiStackInd].argX = ex; k.aiStack[k.aiStackInd].argY = ey; k.aiStack[k.aiStackInd].argU32 = frame_; }
                        break;
                    }
                    if (midG) {
                        const double fx = postX - ex, fy = postY - ey;
                        if (static_cast<double>(k.sightRange) * k.sightRange > fx * fx + fy * fy) {
                            if (k.type == kCarnivorousPlantType) {
                                if (AIPush(k, 63)) k.aiStack[k.aiStackInd].argF = static_cast<float>(k.sightRange * 1.05);
                            } else {
                                if (AIPush(k, 43)) k.aiStack[k.aiStackInd].argU32 = 0;
                                if (AIPush(k, 52)) { k.aiStack[k.aiStackInd].argF = static_cast<float>(k.sightRange * 1.5); k.aiStack[k.aiStackInd].argX = postX; k.aiStack[k.aiStackInd].argY = postY; }
                                AIPush(k, 40);
                            }
                            if (AIPush(k, 15)) { k.aiStack[k.aiStackInd].argX = ex; k.aiStack[k.aiStackInd].argY = ey; k.aiStack[k.aiStackInd].argU32 = frame_; }
                        }
                    }
                }
                if (MonsterLookAtDamager(k)) break;                                             // ③
                if (atWillG) { if (MonsterHearReact(k)) break; }
                else if (MonsterLookAtHeard(k)) break;
                if (((frame_ + k.netCode) & 0xF) == 0) {                                         // ④
                    const double px = postX - k.x, py = postY - k.y;
                    if (px * px + py * py > 64.0) {
                        if (atWillG) { AIPush(k, 56); AIPush(k, 59); }
                        AIPush(k, 44);
                        if (AIPush(k, 7)) { k.aiStack[k.aiStackInd].argX = postX; k.aiStack[k.aiStackInd].argY = postY; k.aiStack[k.aiStackInd].argU32 = 0; }
                        break;
                    }
                    auto facingDot = [&](double vx, double vy) { return DirVecX(k.facing) * vx + DirVecY(k.facing) * vy > 0.89999998; };   // `sub_534120`
                    if (!k.seen.empty()) {
                        Target look = foe;
                        if (look.none()) {
                            SightResolve(k.seen[0], look);
                            for (const auto& r : k.seen) { if (r.addr.empty()) continue; Target t; if (SightResolve(r, t)) look = t; break; }
                        }
                        float lx = 0, ly = 0;
                        if (!look.none() && TargetXY(look, lx, ly)) {
                            const double vx = lx - k.x, vy = ly - k.y, vl = std::sqrt(vx * vx + vy * vy) + 0.001;
                            if (!facingDot(vx / vl, vy / vl) && AIPush(k, 26)) { k.aiStack[k.aiStackInd].targetAddr = look.addr; k.aiStack[k.aiStackInd].targetPet = look.pet; }
                        }
                    } else if (!facingDot(DirVecX(postDir), DirVecY(postDir))) {
                        AIPush(k, 56);
                        if (AIPush(k, 27)) k.aiStack[k.aiStackInd].argU32 = postDir;
                    }
                }
                if (frame_ - k.aiActionFrame <= static_cast<uint32_t>(kServerTickRate / 2) || (k.x == k.prevX && k.y == k.prevY)) {   // ⑤
                    // `mobHealSomeone_5411A0` — 치유 주문(B7)
                } else if (AIPush(k, 25)) {
                    k.aiStack[k.aiStackInd].argX = static_cast<float>(k.prevX); k.aiStack[k.aiStackInd].argY = static_cast<float>(k.prevY);
                }
            }
            break;
        }
        case 5: {    // ── 묶음 6-i(3) 사냥 — `nox_xxx_mobActionHunt_5449D0`(`legacy/GAME4_3.c:9692`)
            // 하는 일이 거의 없다 — **사나움을 0.83 으로 놓고 떠돌기(10)를 민다**.
            //   `+4` 에 0(아직 고른 길목 없음) · `+12` 의 낮은 바이트에 **0x80**
            //   (길목을 못 찾으면 `sub_50CB20` 으로 한 번 더 찾아보라는 표시)
            k.aggression = 0.83f;
            if (AIPush(k, 10)) {
                k.aiStack[k.aiStackInd].argX = 0;
                k.aiStack[k.aiStackInd].argU32 = 0x80;
            }
            break;
        }
        case 10: {   // ── 묶음 6-i(3) 떠돌기 — `nox_xxx_mobActionRoam_5457E0`(`legacy/GAME5.c:428`)
            //
            // ⭕ 정답지의 떠돌기는 **길목(Waypoint)을 하나 골라 그리로 걸어간다**.
            //   · 그 전에 **지금 상대가 있으면 싸움(15)** 을 올린다
            //   · 길목을 **하나도 못 찾으면** — 「Cannot find any waypoints」를 찍고
            //     **스스로 빠진 뒤 1~2초 기다리기(1)** 를 올린다
            //
            // ✖ **정정 09-20** — 위에 「우리 맵 자료에는 길목이 없다」고 적어 뒀는데 **틀렸다.**
            //   길목은 `main.cpp` 가 읽어 `SetWaypoints` 로 싣는다. 없던 것은 **고르는 함수**였고,
            //   그건 6-j(2) 에서 채웠다.
            //
            // ★★ 09-20 묶음 6-j(2) — **고른 길목을 기억한다.** 전에는 매 틱 「가장 가까운 길목」을
            //   다시 골랐는데, 물결이 **제 칸부터** 훑으니 길목 위에 선 순간 **제자리**를 답으로 준다.
            //   그래서 소환수가 첫 길목까지만 가고 거기 붙박였다(⑥ 벽 우회가 막혀 있던 뿌리 ·
            //   실측 — estate 에서 71 움직이고 길목 (3463,2358) 에서 **5 떨어진 자리**에 섬).
            //   정답지는 고른 길목을 쌓임 항목 `+4` 에 적어 두고, **거리² ≤ 64** 면 이음을 타고
            //   다음 길목으로 넘어간다(`GAME5.c:519~528` · `monsterRoamDeadEnd_545BB0`)
            // ★ 09-29 B6 — 앞머리를 정답지대로(`GAME5.c:453~492`) — 성향 가운데·마음대로면 되갚기가 먼저 · 싸움은 **마음대로(> 0.66)일 때만** ·
            //   그다음 들은 소리(`Sub_5466F0`). ✖ 전에는 성향과 상관없이 지금 상대로 싸웠다(사냥 명령은 0.83 이라 겉 차이 없음)
            //   ⚠ 봇(0x20000)의 가만히 서기 갈래(`:459~476`)는 우리 몬스터에 없음
            {
                const bool midR = k.aggression < 0.66000003f && k.aggression > 0.33000001f;
                const bool atWillR = k.aggression > 0.66000003f;
                if ((midR || atWillR) && MonsterRetaliate(k)) break;
                if (atWillR) {
                    const Target foe{ k.enemyAddr, k.enemyPet };
                    float ex = 0, ey = 0;
                    if (!foe.none() && TargetXY(foe, ex, ey)) {
                        if (AIPush(k, 15)) {
                            k.aiStack[k.aiStackInd].argX = ex;
                            k.aiStack[k.aiStackInd].argY = ey;
                            k.aiStack[k.aiStackInd].argU32 = frame_;
                        }
                        break;
                    }
                    if (MonsterHearReact(k)) break;
                }
            }
            // ★ 묶음 6-j(2) — 고른 길목이 아직 쓸 만한가(`sub_547EE0`). 아니면 잊는다
            if (!WaypointById(st.argWp)) st.argWp = 0;
            float wx = 0, wy = 0;
            if (st.argWp == 0) {                             // 아직 없다 — 물결로 찾는다(`Sub_50CB20`)
                st.argWp = AIPathFindWaypoint(k, wx, wy);
                if (st.argWp != 0) RoamRemember(k, st.argWp);
            }
            if (st.argWp != 0) {
                const Waypoint* w = WaypointById(st.argWp);
                // 도착했나 — 거리² ≤ 64(=8). 정답지가 쓰는 바로 그 눈금이다
                const float dx = w->x - static_cast<float>(k.x);
                const float dy = w->y - static_cast<float>(k.y);
                if (dx * dx + dy * dy <= 64.0f) {
                    const uint32_t nx = RoamNextWaypoint(k, st.argWp);
                    if (nx == 0) {                           // "Reached dead end, giving up"
                        ++roamDeadEnd_;
                        AIPop(k);
                        break;
                    }
                    RoamRemember(k, nx);
                    st.argWp = nx;
                    w = WaypointById(nx);
                    ++roamAdvance_;                          // 이음을 타고 넘어간 횟수
                }
                if (AIPush(k, 7)) {
                    k.aiStack[k.aiStackInd].argX = w->x;
                    k.aiStack[k.aiStackInd].argY = w->y;
                    k.aiStack[k.aiStackInd].argU32 = 0;
                }
                break;
            }
            AIPop(k);                                        // 길목이 없다 — 스스로 빠지고
            if (AIPush(k, 1))                                // 1~2초 기다린다(randomInt(FPS, 2*FPS))
                k.aiStack[k.aiStackInd].argU32 = frame_ + 30 + NextRandom(31);
            break;
        }
        case 15: {   // ── 묶음 6-g(1) 싸움 — `nox_xxx_mobActionFight_531EC0`(`legacy/GAME4_3.c:1823`)
            //
            // ⭕ 싸움은 **대상을 쫓는 행동이 아니라, 대상이 없으면 마지막 자리로 가 보는 행동**이다.
            //   · **10초 넘게 대상 소식이 없으면** 스스로 빠진다(쌓임 항목 `+12` 가 마지막 소식 때다)
            //   · 대상이 있으면 그때를 적고 **근접 공격(16)** 을 올린다
            //   · 대상이 없으면 **마지막으로 본 자리**(`+4`·`+8`)로 **가기(7)** 를 민다.
            //     이미 그 자리(8 안)면 그냥 빠진다
            //
            // ⚠ 아직 안 옮긴 갈래 — 스스로 버프 걸기(`monsterBuffSelf_540B90`)·
            //   공격 주문(`monsterCastOffensive_540F20`)은 **6-g(3)** · `sub_534710`(깃발 0x40·아래 행동 4) ·
            //   마지막 자리 둘레 30 안에 그 적이 보이나(`unitsGetInCircle` + `sub_532040`)
            if (frame_ - st.argU32 > static_cast<uint32_t>(10 * 30)) { AIPop(k); break; }
            const Target foe{ k.enemyAddr, k.enemyPet };
            float fx = 0, fy = 0;
            if (!foe.none()) {
                st.argU32 = frame_;                          // 소식이 있었다고 적는다
                // ⚠ `checkIsKillable_528190` 은 **있나**로만 대신한다 — 온전한 조건은 뒤 조각
                if (!TargetXY(foe, fx, fy)) { AIPop(k); break; }
                MonsterPushAttack(k, foe, fx, fy);           // 6-g(2b) — 조건 사슬 + 가기(7)
                break;
            }
            // 대상이 없다 — **마지막으로 본 자리**로 가 본다
            const float lastX = st.argX, lastY = st.argY;
            const double dx = lastX - k.x, dy = lastY - k.y;
            if (dx * dx + dy * dy < 64.0) { AIPop(k); break; }   // 이미 그 자리다
            AIPush(k, 56);                                   // 「보이는 적이 없으면」 조건을 깐다
            if (AIPush(k, 7)) {
                k.aiStack[k.aiStackInd].argX = lastX;
                k.aiStack[k.aiStackInd].argY = lastY;
                k.aiStack[k.aiStackInd].argU32 = 0;          // 정답지 `v3[3] = 0`
            }
            break;
        }
        case 17: {   // 쏘기 — `mobActionMissileAtt_532610`(`GAME4_3.c:2143~2226`) (묶음 6-g(3b))
            // ⭕ 정답지는 애니의 **쏘는 칸**(`monsterDef+216`)에서 `MISSILE_NAME` 오브젝트를
            //   **만들어 날린다**. 이제 우리도 **진짜로 만든다** — 피해는 그것이 맞을 때 들어간다
            // ★ 09-30 — **애니 칸으로 굴린다**(원본 그대로) — 쏘기 애니(3)가 쏘는 칸(`MonsterMissileFrameOf`)·칸 안 틱 0 이면 쏘고(`:2178`),
            //   **애니가 끝나면**(+483) 뺀다(`:2220~2223`). 겨눌 곳도 그 칸에서만 읽는다(`:2181~2188`)
            //   ✖ 전에는 애니 칸이 없던 때(B5 전)의 대신으로 「휘두르기를 시작한 틱에 쏘고 곧 뺌」 · 대상이 없으면 곧 뺐다
            //   · 쏘는 칸에서 겨눌 곳이 없으면(대상이 사라짐 · 자리 0) 안 쏘고 애니 끝을 기다린다 — 정답지는 그 자리를 그대로 읽는다(사라진 대상은 조건 42·45 가 먼저 뺀다)
            // ⚠ **앞질러 겨눔**(`projAddVelocitySmth_533080`)은 아직 안 옮겼다 — 지금은 곧장 겨눈다
            // ★ 09-29 B4 — 겨누는 곳은 **쌓임 항목**이다(`532610` `:2181~2188` — 대상 오브젝트 v4[3] 이 있으면 그것 · 없으면 자리 v4[1·2]).
            //   ✖ 전에는 지금 상대(+1196)를 겨눴다 — 정답지 쏘기는 +1196 을 안 읽는다. 공격 사슬은 대상·자리를 둘 다 싣고,
            //     혼란(36)은 자리만(앞 10) 싣는다
            const MonsterStats* m = FindMonster(k.type);
            if (!m || m->missileName == nullptr) { AIPop(k); break; }
            const int shotFrame = MonsterMissileFrameOf(k.type);
            if (k.animFrame == static_cast<uint8_t>(shotFrame) && k.animTick == 0) {
                CountAttackTiming(k, 1, shotFrame);
                const Target foe{ st.targetAddr, st.targetPet };
                float tx = 0, ty = 0;
                bool aim = true;
                if (!foe.none()) aim = TargetXY(foe, tx, ty);
                else if (st.argX != 0.0f || st.argY != 0.0f) { tx = st.argX; ty = st.argY; }
                else aim = false;
                // ★ 09-26 M4a — MISSILE_ATTACK(+44 · `GAME4_3.c:2215~2218`) — 정답지는 발사체 오브젝트를 **만들었을 때만**(`if (v6)` 안 ·
                //   선 추적이 막혀 곧 지워져도 소리는 난다)
                if (!aim) ++shotNoAim_;
                else if (SpawnCreatureMissile(k, m->missileName, tx, ty, out)) MonsterSound(k, 10);
            }
            if (k.animDone) AIPop(k);
            break;
        }
        case 16: {   // 근접 공격 — `mobActionMeleeAtt_532440`(`GAME4_3.c:2058~2110`)
            // ★ 09-30 — **애니 칸으로 굴린다**(원본 그대로) — 근접 애니(1)가 **때리는 칸**(monster.bin +108 · `MonsterMeleeFrameOf`)에 닿고
            //   칸 안 틱이 0 이면 한 번 때리고(`:2086`), **애니가 끝나면**(+483) 뺀다(`:2097~2100`).
            //   ✖ 전에는 애니 칸이 없던 때(B5 전)의 대신으로 「휘두르기를 시작한 틱에 때리고 곧 뺌」 — 체력이 휘두르는 그림보다 먼저 깎였다
            //   · 때리기 함수가 위로 무엇을 올리면(유령 도망 사슬 25·41·24) 취소가 근접을 뺀다(09-30 취소 = 빼기) — 올리기가 애니 셈을 0 으로 하므로 이 틱엔 한 번 더 안 빠진다
            //   · 때리는 칸이 끝 칸이면 원본도 두 번 때린다(끝 칸에 닿을 때 · 끝을 넘으려다 설 때 — 우리 열셋 중 SmallAlbinoSpider 6칸·칸 5)
            // ★★ 09-29 B3 — **정답지 근접은 지금 상대를 안 본다**(`532440` `:2083~2107`). 때리는 칸에서 몬스터의 근접 함수
            //   (monster.bin `MELEE_STRIKE_FUNCTION`)를 부르고, 함수가 **스스로 대상을 고른다**(`MonsterPickMeleeTarget`) · 함수가 NULL 이면 빠진다.
            //   ✖ 전에는 「지금 상대가 없으면 빠짐 · 있으면 거리·시야 상관없이 그 상대를 늘 맞힘」 — 식충식물이 200 밖을 물어 죽였다(B1 시험에서 잡힘)
            const MonsterStats* m = FindMonster(k.type);
            const int dmg = (m && m->meleeDamage > 0) ? m->meleeDamage : 0;
            const MeleeStrikeInfo si = MeleeStrikeOf(k.type);
            if (dmg == 0 || si.fn == StrikeFn::None) { AIPop(k); break; }   // 「Tried to MELEE_ATTACK but cannot」(`:2101~2106`)
            const int hitFrame = MonsterMeleeFrameOf(k.type);
            if (k.animFrame != static_cast<uint8_t>(hitFrame) || k.animTick != 0) {
                if (k.animDone) AIPop(k);
                break;
            }
            CountAttackTiming(k, 0, hitFrame);
            // 때리다 상대(앞 자리 크리처)가 죽으면 목록이 당겨져 `k` 가 다른 것을 가리킬 수 있다 — 소리에 쓸 값은 먼저 떠 두고, 뒤에는 번호로 다시 찾는다
            const Creature kSnap = k;
            const int res = MonsterStrike(k, si, dmg, out);
            // ★ 09-24 H1(사람 판 「박쥐가 물 때 원작은 박쥐 소리」) — 때리기 함수 뒤 그 종류 소리 모음의 **맞히는 소리**(+32)를
            //   몬스터 자리에서 모두에게(`GAME4_3.c:2087~2094` · `aud_501960(…, a1, 0, 0)`). 4-c3b3b 가 맞는 쪽 소리를 끈 것의 짝이다.
            //   ★ 09-29 B3 — 함수가 0 이면 **빗나가는 소리**(+36 = 칸 8) — 기본은 대상을 찾았는데 시야가 막힌 때만 0 · 거미는 대상이 없어도 0.
            //   방패에 막혀도 정답지 함수는 1 이다(피해 함수만 부르고 결과를 안 본다)
            if (res) {
                const uint16_t snd = (kSnap.type < monsterSnd_[7].size()) ? monsterSnd_[7][kSnap.type] : 0;   // ★ 09-26 M4a — 칸 7 표로 합침
                if (snd != 0) {
                    PlaySoundAt(snd, static_cast<float>(kSnap.x), static_cast<float>(kSnap.y), 0, 0, std::string(), PetTarget(kSnap));
                    ++monsterMeleeHitPlayed_;
                } else {
                    ++monsterMeleeHitNone_;
                }
            } else {
                MonsterSound(kSnap, 8);                                  // +36 MELEE_ATTACK_MISSING
                ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::MissSound)];
            }
            // 애니가 끝났으면 뺀다(`:2097~2100` — 때린 뒤에 본다) · 목록이 당겨졌을 수 있어 번호로 다시 찾는다
            for (Creature& self : pets_) {
                if (self.netCode != kSnap.netCode) continue;
                if (self.animDone) AIPop(self);
                break;
            }
            break;
        }
        case 29:     // 아무 데나 걷기 — 6-f(5). **스스로 안 빠진다**(아래 깔린 때(41) 조건이 뺀다)
            AIRandomWalk(k);
            break;
        // ★ 09-29 B7 — 시전 행동(`unit_ai.go:672~743`) — 인자 = 주문 번호(argU32) · 대상(target)
        //   18 대상에게 — 대상이 없으면 뺀다 · 시전(`5413B0`, 0) · 애니가 끝나면(+483) 뺀다
        //   19 자리에 — 시전(1) · 애니가 끝나면 뺀다
        //   20 지속 — 대상이 있으면 시전(0) · 없으면 뺀다 · 스스로는 안 빠진다(아래 41 때가 뺀다 · 끝·취소가 그 주문을 끈다)
        case 18:
            if (Target{ st.targetAddr, st.targetPet }.none()) { AIPop(k); break; }
            MonsterActionCast(k, 0, out);
            if (k.animDone) AIPop(k);
            break;
        case 19:
            MonsterActionCast(k, 1, out);
            if (k.animDone) AIPop(k);
            break;
        case 20:
            if (Target{ st.targetAddr, st.targetPet }.none()) { AIPop(k); break; }
            MonsterActionCast(k, 0, out);
            break;
        default: break;   // 12·13 은 아무 일도 안 한다 · 나머지는 아직 안 옮겼다
    }
}

// 끝내기 — 넷 다 아무 일도 안 한다(정답지 `End(_ *Object) {}`).
void Server::AIEnd(Creature& k, uint8_t act) {
    // ★ 09-29 B2 — 싸움 끝 `sub_531E90`(`GAME4_3.c:1811~1820`) — ALERT 끄기 · 달리기 끄기(`sub_534780` — ALWAYS_RUN 이 아니면 · 우리 몬스터에 없음).
    //   싸움에는 취소 함수가 없다(`legacy/unit_ai.go:50`) — 위에 무엇을 쌓아도 ALERT 는 남는다
    if (act == 15) {
        k.alert = false;
        k.running = false;
        ++b2Stat_[static_cast<size_t>(B2Stat::FightEnd)];
    }
    if (act == 24) k.running = false;   // ★ 09-29 B4 — 도망 끝 `sub_544750` = 달리기 끄기(`534780`)
    // ★ 09-29 B7 — 지속 시전 끝 = 그 주문을 끈다(`AIActionCastDuration.End` → `Dur.CancelFor(주문, 나)`)
    if (act == 20) { std::vector<OutPacket> sink; DurCancelForPet(static_cast<uint8_t>(k.aiStack[k.aiStackInd].argU32), k.netCode, aiOut_ ? *aiOut_ : sink); }
}

// 취소 — ★ **기다리기 둘**이 스스로 빠진다(`Cancel` 이 `MonsterPopAction`) — 아래 바라보기 넷·근접·쏘기도(09-30 「둘만」 정정).
//   변신·떨구기·찾기는 취소돼도 가만히 있는다.
//   ★ 바라보기 넷(25·26·27·28)도 스스로 빠진다 — 등록표가 `Cancel: nox_ai_action_pop_532100`
//     을 달아 두었다(`legacy/unit_ai.go:57~60`).
//   ★ 09-30 — 근접(16)·쏘기(17)도 같다(`legacy/unit_ai.go:51~52`) — 휘두르는 중에 위로 무엇이 올라오면 휘두르기가 빠진다
//     (쉬는 중 갈래의 50·1 · 유령 도망 사슬 · 두뇌의 혼란·두려움). 막기 21~23 도 같은 등록이지만 우리가 안 쌓는다
void Server::AICancel(Creature& k, uint8_t act) {
    if (act == 1 || act == 2 || act == 16 || act == 17 || (act >= 25 && act <= 28)) AIPop(k);
    // ★ 09-29 B7 — 지속 시전 취소 = 그 주문을 끄고 빠진다(`AIActionCastDuration.Cancel`) — 빼기가 끝내기(End)를 한 번 더 불러도 이미 꺼진 것이라 겉 차이 없음
    if (act == 20) { std::vector<OutPacket> sink; DurCancelForPet(static_cast<uint8_t>(k.aiStack[k.aiStackInd].argU32), k.netCode, aiOut_ ? *aiOut_ : sink); AIPop(k); }
}

// nox_xxx_mobActionDependency(`unit_ai.go:44~262`) — 쌓임에 붙은 **조건**을 **위에서부터** 본다.
//
// 조건은 할 일에 **붙어 다니는 꼬리표**다(갈래 40~71). 틀리면 그 조건과 **그것이 붙은 할 일까지**
// 한꺼번에 빼 버린다 — 「보이는 동안만 쫓아라」 같은 것을 이 한 줄로 만든다.
//
//   · 조건이 아니면 지나간다(그때 `또는` 묶음이 끊긴다)
//   · **40 `또는`** — 그 아래로 이어지는 조건들을 **하나만 맞아도** 되게 묶는다.
//     묶음의 끝(아래가 조건이 아닌 자리)에서 **하나도 안 맞았으면** 뺀다
//   · 틀리면 — `자리 < i` 가 되거나 쌓임이 **바닥 놀기**가 될 때까지 뺀다. 그러고 **맨 위부터 다시** 본다
//
// ⭕ 옮긴 조건(우리 상태로 충실히 잴 수 있는 것):
//   41 때 · 42 살아 있나 · 45 보이나 · 46 안 보이나 · 49 더 머나 · 50 더 가까운가 ·
//   51 자리가 더 머나 · 52 자리가 더 가까운가 · 53 보이는 적이 있나 · 56 보이는 적이 없나 · 60 새 적이 없나(09-28) ·
//   61 안 끊기나(늘 참) · 62 마법이 걸렸나 · 71 안 움직였나
// ⚠ 안 옮긴 조건 — 43·44 맞고 있나(피격 기록) · 47 사선이 막혔나 · 48 보이는 자리인가 ·
//   54·55·57·58 보이는 친구·먹이(몬스터 시야 목록이 아직 없다) · 59 소리 ·
//   63·66 적 거리 · 64 성치 않나 · 65 기력 · 67 커서 아래 · 68 몰렸나 · 69 안전한 자리 · 70 답답한가.
//   **이들은 늘 참으로 둔다** — 없는 것을 거짓으로 만들면 멀쩡한 할 일이 사라진다.
void Server::AIProcDeps(Creature& k, std::vector<OutPacket>& out) {
    bool isOR = false;
    bool okPrev = false;
    for (int i = k.aiStackInd; i >= 0; --i) {
        bool ok = true;
        Creature::AIStackItem& st = k.aiStack[i];
        const uint8_t typ = st.action;
        if (!AIIsCondition(typ)) { isOR = false; continue; }

        const Target targ{ st.targetAddr, st.targetPet };
        float tx = 0, ty = 0;
        const bool hasT = TargetXY(targ, tx, ty);
        const float dx = static_cast<float>(k.x) - st.argX;
        const float dy = static_cast<float>(k.y) - st.argY;

        switch (typ) {
            case 40:                                         // 또는
                isOR = true;
                okPrev = false;
                continue;
            case 41: ok = st.argU32 > frame_; break;         // 때 — 그 프레임 전까지만
            case 42:                                                            // 살아 있나
                ok = hasT && TargetHealth(targ) > 0;
                // ★ 09-29 B6 — 깨지면 들은 소리를 지우고 1초 귀를 막는다(`unit_ai.go:71~73` Field97 = 0 · Field101 = 지금 + 1초)
                if (!ok) { k.heardSnd = 0; k.heardFrame = frame_ + static_cast<uint32_t>(kServerTickRate); }
                break;
            case 43: {                                                          // ★ 09-29 B2 — 맞는 중(`unit_ai.go:75~84`)
                // 다쳤고 때린 쪽이 유닛이면 인자를 지금으로 고친다 · 참 = 인자 뒤 10초 안
                Target att;
                if (k.injured && SightResolve(k.lastAttacker, att)) st.argU32 = frame_;
                ok = frame_ - st.argU32 <= static_cast<uint32_t>(10 * kServerTickRate);
                break;
            }
            case 44: {                                                          // ★ 09-29 B2 — 안 맞는 중(`unit_ai.go:85~98`) — 다쳤고 때린 쪽이 유닛이면 거짓
                Target att;
                ok = !(k.injured && SightResolve(k.lastAttacker, att));
                break;
            }
            case 63: {                                                          // ★ 09-29 B4 — 적이 이보다 가깝나(`unit_ai.go:188~190`) — 지금 상대까지 ≤ 자
                const Target e{ k.enemyAddr, k.enemyPet };
                float ex = 0, ey = 0;
                ok = !e.none() && TargetXY(e, ex, ey) &&
                     std::sqrt((ex - k.x) * (ex - k.x) + (ey - k.y) * (ey - k.y)) <= static_cast<double>(st.argF);
                break;
            }
            case 64: ok = !MonsterCanResumeAttack(k); break;                   // ★ 09-29 B4 — 건강하지 않나(`:191~199`) — 체력 비율 < 회복 비율
            case 68: ok = !(frame_ - k.moveAttemptFrame < static_cast<uint32_t>(3 * kServerTickRate)); break;   // ★ 09-29 B4 — 구석 아님(`:213~214`)
            case 59:                                                            // ★ 09-29 B6 — 들은 소리가 없나(`:157~160`) — 3초 안 들은 것(+388)이 있으면 거짓
                if (k.heardSnd != 0 && frame_ - k.heardFrame < static_cast<uint32_t>(3 * kServerTickRate)) { ok = false; ++hearStat_[static_cast<size_t>(HearStat::Cond59)]; }
                break;
            case 69:                                                            // ★ 09-29 B6 — 그 자리가 안전한가(`:215~223`) — 둘레 50 에 위험한 것
                if (!LocationIsSafe(k, st.argX, st.argY)) { ok = false; ++hearStat_[static_cast<size_t>(HearStat::Cond69)]; }
                break;
            case 70:                                                            // ★ 09-29 B4b — 답답하지 않나(`:224~227`) — 답답하면 거짓 · 그 깃발을 지운다
                if (k.frustrated) { ok = false; k.frustrated = false; }
                break;
            case 45: ok = hasT && CanInteractFrom(k, targ); break;             // 보이나
            case 46: ok = !(hasT && CanInteractFrom(k, targ)); break;          // 안 보이나
            case 47: {                                                          // 쏠 줄이 막혔나
                // DEPENDENCY_BLOCKED_LINE_OF_FIRE — **막혀 있는 동안 참**이다.
                //   원거리 사슬에서 49(더 머나)와 **또는(40)** 으로 묶여 「멀거나 막혔으면 다가간다」가 된다.
                //   ⚠ 안 옮겼을 때는 `default: 늘 참`이라 **그 묶음이 절대 안 깨져** 임프가
                //     사거리를 지나쳐 몸에 붙을 때까지 걸어갔다(2026-09-15 6-g(3b) 에서 잡힘)
                if (!hasT) { ok = false; break; }
                ok = !MapTraceVision(static_cast<float>(k.x), static_cast<float>(k.y), tx, ty);
                break;
            }
            case 49: {                                                          // 더 머나
                if (!hasT) { ok = false; break; }
                const float ex = static_cast<float>(k.x) - tx, ey = static_cast<float>(k.y) - ty;
                ok = std::sqrt(ex * ex + ey * ey) > st.argF;
                break;
            }
            case 50: {                                                          // 더 가까운가
                if (!hasT) { ok = false; break; }
                const float ex = static_cast<float>(k.x) - tx, ey = static_cast<float>(k.y) - ty;
                ok = std::sqrt(ex * ex + ey * ey) <= st.argF;
                break;
            }
            case 51: ok = std::sqrt(dx * dx + dy * dy) >= st.argF; break;      // 자리가 더 머나
            case 52: ok = std::sqrt(dx * dx + dy * dy) <= st.argF; break;      // 자리가 더 가까운가
            // ★ 09-28 박쥐(사람 판 「멀리 도망갔다 오면 안 때리고 옛 자리로 간다」) — 53·56 은 시야 목록이 아니라
            //   **지금 상대(+1196)가 있나**만 본다(`unit_ai.go:147~150`). 우리 `enemyAddr` 가 그 자리다(보이는 적만 넣는다).
            //   늘 참으로 두었을 때는 「마지막으로 본 자리로 가기」(15 → 56·7)가 적이 다시 보여도 안 깨져
            //   **옆에서 때려도 옛 자리로 계속 갔다**
            case 53: ok = !Target{ k.enemyAddr, k.enemyPet }.none(); break;   // 보이는 적이 있나
            case 56: ok = Target{ k.enemyAddr, k.enemyPet }.none(); break;    // 보이는 적이 없나
            case 60: {                                                          // 새 적이 없나(`unit_ai.go:161~183`)
                // 옛 대상이 없으면 거짓 · 지금 상대가 없으면 참 · 있으면 **지금 상대가 옛 대상보다 가깝지 않은가**
                if (!hasT) { ok = false; break; }
                const Target foe{ k.enemyAddr, k.enemyPet };
                float ex = 0, ey = 0;
                if (foe.none() || !TargetXY(foe, ex, ey)) break;
                const float ox = tx - static_cast<float>(k.x), oy = ty - static_cast<float>(k.y);
                const float nx = ex - static_cast<float>(k.x), ny = ey - static_cast<float>(k.y);
                ok = (nx * nx + ny * ny) >= (ox * ox + oy * oy);
                break;
            }
            case 62: ok = k.buffs.Has(static_cast<int>(st.argU32)); break;     // 마법이 걸렸나
            case 71: ok = (k.x == k.prevX && k.y == k.prevY); break;           // 안 움직였나
            default: ok = true; break;   // ⚠ 안 옮긴 조건 — 늘 참(없는 것을 거짓으로 만들지 않는다)
        }

        if (!isOR) {
            if (ok) continue;
            // 틀렸다 — 아래에서 뺀다
        } else {
            if (ok) okPrev = true;
            if (i > 0 && AIIsCondition(k.aiStack[i - 1].action)) continue;   // 묶음이 아직 안 끝났다
            if (okPrev) continue;                                            // 하나라도 맞았다
        }
        for (;;) {
            AIPop(k);
            if (k.aiStackInd < i || (k.aiStackInd == 0 && k.aiStack[0].action == 0)) break;
        }
        i = k.aiStackInd + 1;   // 맨 위부터 다시 본다(다음 돎에서 −1 되어 맨 위가 된다)
        AIActionReset(k);
    }
    (void)out;
}

// nox_xxx_mobAction_50A910(`GAME4_1.c:582~673`) — 매 틱 조건을 보기 **전에** 쌓임을 위에서 아래로 훑어
//   **대상 오브젝트(+12)가 있는 항목의 자리(+4·+8)를 그 오브젝트의 지금 자리로** 고쳐 적는다.
//   · 3 호위 — 대상이 있으면 늘
//   · 7·8 가기 — 대상이 **보이거나** 쌓임에 호위(3)가 있으면 고쳐 적고, 아니면 **대상을 잊는다**(+12 = 0 — 그 뒤로는 그 자리까지만 간다)
//   · 17 쏘기 — 대상이 보이면
//   · 15 싸움 — 지금 상대(+1196)의 자리. ⚠ 이것은 이미 틱이 한다(`Tick` 의 「마지막으로 본 자리」 줄) — 여기선 안 한다
// ★ 09-28 박쥐 — 이것이 없어 공격 사슬의 가기(7)가 **사슬을 쌓던 때의 자리**로만 갔다. 달아나는 사람을 쫓다가
//   그 사람이 곁으로 돌아와도 박쥐는 옛 자리로 계속 갔다(사람 판 「도망간 길을 안 따라오고 옆에서 때려도 안 문다」)
// ⚠ 「보이나」(`nox_xxx_unitCanInteractWith_5370E0`)는 틱의 상대 고르기와 같은 잣대 — `CanInteractFrom`(크리처 눈)
// ⚠ 안 옮긴 것 — 각 항목의 오브젝트 인자가 죽었으면 비우는 `sub_509FF0`(우리는 `TargetXY` 가 못 찾으면 그 자리를 안 고친다)
void Server::AIRefreshStackArgs(Creature& k) {
    const bool escorting = AIHasAction(k, 3);
    for (int i = k.aiStackInd; i >= 0; --i) {
        Creature::AIStackItem& st = k.aiStack[i];
        const Target targ{ st.targetAddr, st.targetPet };
        if (targ.none()) continue;
        float tx = 0, ty = 0;
        switch (st.action) {
            case 3:
                if (TargetXY(targ, tx, ty)) { st.argX = tx; st.argY = ty; }
                break;
            case 7:
            case 8: {
                const bool known = TargetXY(targ, tx, ty);
                if (known && (escorting || CanInteractFrom(k, targ))) {
                    st.argX = tx; st.argY = ty;
                    ++aiMoveTargetFollow_;
                } else {
                    st.targetAddr.clear();
                    st.targetPet = -1;
                    ++aiMoveTargetForgot_;
                }
                break;
            }
            case 17:
                if (TargetXY(targ, tx, ty) && CanInteractFrom(k, targ)) {
                    st.argX = tx; st.argY = ty;
                }
                break;
            default: break;
        }
    }
}

// 크리처가 대상을 볼 수 있나 — **크리처 자신의 눈**(`CanInteract(u, obj, 0)` · `unit_ai.go:99~103` · `nox_xxx_unitCanInteractWith_5370E0`).
//   버프(눈멂·투명)는 `CanSee` 그대로, 시야선은 크리처 자리에서 본다(사람판 `CanInteract` 와 같은 두 겹).
// ✖ 09-28 정정 — 전에는 **주인의 눈**으로 갈음했고 주인이 없으면 늘 참이었다. 틱은 상대를 **크리처 자리의 시야**로 고르므로
//   「크리처는 보는데 주인은 벽 너머라 못 보는」 자리에서 조건 45 가 매 틱 깨져 공격 사슬을 쌓았다 무너뜨리기만 하며
//   **제자리에 섰다**(사람 판 박쥐 「때려도 맞기만 하고 안 따라온다」 · 시험 TestMonsterChaseReturn ②).
//   상대 고르기·조건 45/46·가기(7) 자리 고치기가 이제 이 한 잣대를 쓴다
bool Server::CanInteractFrom(const Creature& k, const Target& t) const {
    if (!CanSeeFrom(k, t)) return false;
    float tx = 0, ty = 0;
    if (!TargetXY(t, tx, ty)) return false;
    return MapTraceVision(static_cast<float>(k.x), static_cast<float>(k.y), tx, ty);
}

// ★ 09-29 B1 — `CanSee`(`server/object.go:77~104`) — 버프만 본다(눈멂 · 투명 — 느리면 안 보임 · 적외선). 시야선은 안 본다.
//   대상 빠르기는 사람·크리처 모두 그 속도(09-28 까지는 크리처를 0 으로 봤다 — 움직이는 투명 크리처를 못 봤다)
bool Server::CanSeeFrom(const Creature& k, const Target& t) const {
    const Buffs* tb = BuffsOf(t);
    if (!tb) return false;
    double tvx = 0, tvy = 0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it != sessions_.end()) { tvx = it->second.velX; tvy = it->second.velY; }
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        tvx = pets_[static_cast<size_t>(t.pet)].velX; tvy = pets_[static_cast<size_t>(t.pet)].velY;
    }
    return CanSeeBuffs(&k.buffs, tb, tvx, tvy, 0);
}

// ── ★ 09-29 B1 알아보기 ────────────────────────────────────────────────
//
// 정답지 `nox_xxx_unitUpdateSightMB_5281F0`(`GAME4_2.c:7278~7388`) · 둘레 하나씩 `monsterUpdateSeenEnemies_5286D0`(`:7468~7499`) ·
//   목록에 넣기 `monsterVisionSeeEnemy_5287B0`(`:7502~7565`) · 빼기 `aiLostSight_528560`(`:7391~7428`) · 상대 고르기 `sub_528610`(`:7431~7465`)
//   · 거리 = 250(퀘스트 640) 과 몬스터 자체 시야(+1312) 중 큰 것
//   · 목록에서 뺀다 — 죽음·사라짐(깃발 0x8020) · 버프로 안 보임 · (거리 + 30) 밖 · 한 프레임에 √1000 넘게 옮겨짐 · 2초마다 시야선까지 다시 봐서 안 보임
//   · 훑는다 — 상대가 없거나 마지막 훑기가 2초 지났고, 다음 훑기 때가 됐거나(온라인은 5~10프레임마다) 누가 다시 훑으라 했을 때(순간이동·살금살금 켜기)
//   · 넣는다 — 유닛(사람·크리처) · 적 · 아직 목록에 없음 · **앞쪽 부채꼴**(보는 쪽과의 내적 ≥ 0.5 — STATUS ALERT 면 안 봄 · monster.bin 에 ALERT 0) · 시야선
//              목록이 차면(16) 가장 먼 것보다 가까울 때만 그것을 밀어내고 넣는다 · 넣을 때 적이면 발견 소리(소리 모음 칸 16 · 2~4초에 한 번)
//   · 지금 상대 = 목록 중 **가장 가까운** 죽일 수 있는 적(+1216 우선 상대는 스크립트만 세운다 — 우리에게 없음)
//   ⚠ 안 옮긴 것 — 스크립트 사건(14 보았다 · 15 놓쳤다) · 좀비 예외 · STATUS CAN_SEE_FRIENDS(0x400 — monster.bin 에 0) · 오프라인 훑기 주기(가까운 적 거리로 늦춤)
bool Server::SightResolve(const Creature::SeenRef& r, Target& t) const {
    t = Target{};
    if (!r.addr.empty()) {
        auto it = sessions_.find(r.addr);
        if (it == sessions_.end() || !it->second.inGame) return false;
        t.addr = r.addr;
        return true;
    }
    for (size_t i = 0; i < pets_.size(); ++i)
        if (pets_[i].netCode == r.petNet) { t.pet = static_cast<int>(i); return true; }
    return false;
}

// 깃발 0x8020(죽음·사라짐) — 사람은 부활 기다림·체력 0 · 크리처는 체력 0(죽음 행동 중)
bool Server::TargetDeadOrGone(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0 || it->second.health == 0;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return true;
    return pets_[static_cast<size_t>(t.pet)].health == 0;
}

// `unitIsEnemyTo_5330C0`(크리처, 대상) — 뿌리 주인으로 가른다(`OwnersEnemy` 와 같은 규칙 · 그쪽은 투사체 계수를 올리므로 따로 둔다)
//   같은 주인 → 적 아님 · 둘 다 주인 없는 몬스터 → 적 아님 · 한쪽이 주인 없음 → 적 · 둘 다 사람 쪽 → `IsEnemyTo`(팀)
bool Server::CreatureEnemyTo(const Creature& k, const Target& t) const {
    std::string tOwner;
    if (!t.addr.empty()) tOwner = t.addr;
    else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        if (&pets_[static_cast<size_t>(t.pet)] == &k) return false;
        tOwner = pets_[static_cast<size_t>(t.pet)].owner;
        if (k.owner.empty() && tOwner.empty()) return false;
    } else return false;
    if (k.owner.empty() || tOwner.empty()) return true;
    if (k.owner == tOwner) return false;
    return IsEnemyTo(k.owner, tOwner);
}

void Server::SightLose(Creature& k, size_t i, SightStat why) {
    if (i >= k.seen.size()) return;
    ++sightStat_[static_cast<size_t>(why)];
    const Creature::SeenRef r = k.seen[i];
    const bool wasEnemy = (!r.addr.empty() && r.addr == k.enemyAddr) || (r.addr.empty() && r.petNet != 0 && r.petNet == k.enemyNet);
    if (wasEnemy) { k.enemyAddr.clear(); k.enemyNet = 0; k.enemyPet = -1; }   // `:7409~7414` — 지금 상대였으면 비운다
    k.seen.erase(k.seen.begin() + static_cast<long>(i));
}

// `monsterVisionSeeEnemy_5287B0` — 목록에 넣고 발견 소리
void Server::SightSeeEnemy(Creature& k, const Target& t) {
    float tx = 0, ty = 0;
    TargetXY(t, tx, ty);
    if (k.seen.size() >= 16) {
        double farD2 = 0.0; size_t far = 0;
        for (size_t i = 0; i < k.seen.size(); ++i) {
            Target o; float ox = 0, oy = 0;
            if (!SightResolve(k.seen[i], o) || !TargetXY(o, ox, oy)) continue;
            const double d2 = (ox - k.x) * (ox - k.x) + (oy - k.y) * (oy - k.y);
            if (d2 > farD2) { farD2 = d2; far = i; }
        }
        const double nd2 = (tx - k.x) * (tx - k.x) + (ty - k.y) * (ty - k.y);
        if (farD2 <= nd2) { ++sightStat_[static_cast<size_t>(SightStat::FullFar)]; return; }
        SightLose(k, far, SightStat::LostReplaced);                                  // `sub_528910`
    }
    Creature::SeenRef r;
    if (!t.addr.empty()) r.addr = t.addr; else r.petNet = pets_[static_cast<size_t>(t.pet)].netCode;
    k.seen.push_back(r);
    ++sightStat_[static_cast<size_t>(SightStat::Added)];
    if (frame_ > k.spotSoundFrame && CreatureEnemyTo(k, t)) {                       // `:7552~7563`
        MonsterSound(k, 16);                                                          // +68 RECOGNIZE
        k.spotSoundFrame = frame_ + static_cast<uint32_t>(2 * kServerTickRate) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate + 1));
        ++sightStat_[static_cast<size_t>(SightStat::Spot)];
    }
}

// `sub_528610` — 목록 중 가장 가까운 「적 · 죽일 수 있음」(`checkIsKillable_528190` — 체력 자료가 있고 살았거나 최대 0)
void Server::SightPickEnemy(Creature& k) {
    k.enemyAddr.clear(); k.enemyNet = 0; k.enemyPet = -1;
    double best = 100000000.0;
    for (const auto& r : k.seen) {
        Target t; float tx = 0, ty = 0;
        if (!SightResolve(r, t) || !TargetXY(t, tx, ty)) continue;
        if (!CreatureEnemyTo(k, t) || TargetDeadOrGone(t)) continue;
        const double d2 = (tx - k.x) * (tx - k.x) + (ty - k.y) * (ty - k.y);
        if (d2 < best) { best = d2; k.enemyAddr = t.addr; k.enemyNet = t.addr.empty() ? r.petNet : 0; k.enemyPet = t.pet; }
    }
}

void Server::UpdateSight(Creature& k) {
    if (k.health == 0) return;                                        // `:7306` 죽은 것(0x8000)
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    const float base = quest ? 640.0f : 250.0f;                       // `:7309~7313` 게임 깃발 4096
    const float range = (base >= k.sightRange) ? base : k.sightRange;
    bool recheck = false;
    if (frame_ - k.sightRecheckFrame > static_cast<uint32_t>(2 * kServerTickRate)) { recheck = true; k.sightRecheckFrame = frame_; }
    bool changed = false;
    for (size_t i = 0; i < k.seen.size();) {
        Target t; float tx = 0, ty = 0;
        SightStat why = SightStat::Count;
        if (!SightResolve(k.seen[i], t) || !TargetXY(t, tx, ty) || TargetDeadOrGone(t)) why = SightStat::LostGone;
        else if (!CanSeeFrom(k, t)) why = SightStat::LostCantSee;
        else if ((k.x - tx) * (k.x - tx) + (k.y - ty) * (k.y - ty) > (range + 30.0) * (range + 30.0)) why = SightStat::LostFar;
        else if ((k.x - k.prevX) * (k.x - k.prevX) + (k.y - k.prevY) * (k.y - k.prevY) > 1000.0) why = SightStat::LostMoved;
        else if (recheck && !CanInteractFrom(k, t)) why = SightStat::LostRecheck;
        if (why != SightStat::Count) { SightLose(k, i, why); changed = true; continue; }
        ++i;
    }
    {   // `:7344~7347` — 지금 상대가 홀려(버프 28) 있으면 다시 고른다
        Target e; e.addr = k.enemyAddr; e.pet = -1;
        if (e.addr.empty() && k.enemyNet != 0) for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == k.enemyNet) e.pet = static_cast<int>(i);
        const Buffs* eb = e.none() ? nullptr : BuffsOf(e);
        if (eb && eb->Has(28)) changed = true;
    }
    const bool noEnemy = k.enemyAddr.empty() && k.enemyNet == 0;
    if ((noEnemy || frame_ - k.sightScanFrame > static_cast<uint32_t>(2 * kServerTickRate)) &&
        (k.sightNextScan <= frame_ || frame_ == sightForceFrame_)) {
        ++sightStat_[static_cast<size_t>(SightStat::Scan)];
        auto consider = [&](const Target& t, float tx, float ty) {    // `monsterUpdateSeenEnemies_5286D0`
            ++sightStat_[static_cast<size_t>(SightStat::Cand)];
            if (TargetDeadOrGone(t)) { ++sightStat_[static_cast<size_t>(SightStat::NotEnemy)]; return; }
            if (!CreatureEnemyTo(k, t)) { ++sightStat_[static_cast<size_t>(SightStat::NotEnemy)]; return; }
            for (const auto& r : k.seen)
                if ((!t.addr.empty() && r.addr == t.addr) || (t.addr.empty() && r.petNet == pets_[static_cast<size_t>(t.pet)].netCode)) {
                    ++sightStat_[static_cast<size_t>(SightStat::Already)]; return;
                }
            const double dx = tx - k.x, dy = ty - k.y;
            const double len = std::sqrt(dx * dx + dy * dy) + 0.001;
            // ★ 09-29 B2 — ALERT(싸움 중)면 부채꼴을 안 본다(`:7487` `v5 & 0x100 ||`)
            if (!k.alert && dy / len * DirVecY(k.facing) + dx / len * DirVecX(k.facing) < 0.5) { ++sightStat_[static_cast<size_t>(SightStat::Cone)]; return; }
            if (!CanInteractFrom(k, t)) { ++sightStat_[static_cast<size_t>(SightStat::Los)]; return; }
            SightSeeEnemy(k, t);
        };
        // `unitsGetInCircle_517F90(자리, 거리)` — 유닛 중심이 원 안인 것(⚠ 정답지는 충돌 칸 겹침 — 몸 반지름만큼 넓다)
        const double r2 = static_cast<double>(range) * range;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (!o.inGame) continue;
            const double dx = o.posX - k.x, dy = o.posY - k.y;
            if (dx * dx + dy * dy > r2) continue;
            consider(Target{ kv.first, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
        }
        for (size_t i = 0; i < pets_.size(); ++i) {
            if (&pets_[i] == &k) continue;
            const double dx = pets_[i].x - k.x, dy = pets_[i].y - k.y;
            if (dx * dx + dy * dy > r2) continue;
            consider(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y));
        }
        k.sightScanFrame = frame_;
        k.sightRecheckFrame = frame_;
        changed = true;
    }
    if (changed) SightPickEnemy(k);
    if (k.sightScanFrame == frame_)                                   // `:7370~7373` — 온라인(게임 깃발 0x2000)은 늘 5~10프레임 뒤
        k.sightNextScan = frame_ + 5 + NextRandom(6);
    // 지금 상대의 자리 번호는 틱마다 번호로 다시 푼다(앞 크리처가 지워지면 밀린다)
    k.enemyPet = -1;
    if (k.enemyAddr.empty() && k.enemyNet != 0)
        for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == k.enemyNet) { k.enemyPet = static_cast<int>(i); break; }
}

// ── ★ 09-29 B2 놀기·되갚기·소리 넷 ─────────────────────────────────────
//
// 몬스터 한 틱 앞머리(`nox_xxx_unitUpdateMonster_50A5C0` `unit_ai.go:557~573`) — 살았으면:
//   ① INJURED(0x200)면 다침 소리(소리 모음 +64 = 칸 15) ② 적을 때린 지(+520) 1초가 지났으면 도발(`532800` — +8 = 칸 1 · 2~4초에 한 번) 뒤 +520 비움
//   ③ 놀 때 소리(`mobAction_5469B0` — 마음대로 공격 아님(성향 ≤ 0.66) · 지금 상대 없음 · 가까운 적 거리(+524) ≤ 300 · 맨 위가 놀기 0 이나 지키기 4 ·
//      +528 이 지남 → 다음 20~60초 · +16 = 칸 3). +524 는 온라인에서 알아보기가 안 적어 0 이다(`GAME4_2.c:7370~7386` — 오프라인 갈래만 적음)
//   ⚠ 스크립트 사건(다침 · 놀기 중 적 찾기) · 체력 차기(`monsterRegenerateHP` — 우리 몬스터는 차는 값이 없다)는 안 옮겼다
void Server::NoteMonsterStruck(const AttackSrc* src) {
    if (!src || src->byPet < 0 || static_cast<size_t>(src->byPet) >= pets_.size()) return;
    Creature& m = pets_[static_cast<size_t>(src->byPet)];
    if (m.tauntFrame == 0) m.tauntFrame = frame_;       // `sub_532880` — 비었을 때만
}

void Server::MonsterHeadUpdate(Creature& k) {
    if (k.health == 0) return;                                            // `:557` 죽음·사라짐이 아니면
    if (k.injured) { MonsterSound(k, 15); ++b2Stat_[static_cast<size_t>(B2Stat::InjuredSound)]; }   // `:558~567` +64
    if (k.tauntFrame != 0 && frame_ - k.tauntFrame >= static_cast<uint32_t>(kServerTickRate)) {    // `:568~571` 1초
        if (frame_ >= k.tauntNext) {                                                               // `532800` +532
            k.tauntNext = frame_ + static_cast<uint32_t>(2 * kServerTickRate) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate + 1));
            MonsterSound(k, 1);                                                                    // +8
            ++b2Stat_[static_cast<size_t>(B2Stat::Taunt)];
        } else {
            ++b2Stat_[static_cast<size_t>(B2Stat::TauntQuiet)];
        }
        k.tauntFrame = 0;
    }
    // `mobAction_5469B0`
    const bool atWill = k.aggression > 0.66000003f;
    const bool noEnemy = k.enemyAddr.empty() && k.enemyNet == 0;
    const uint8_t top = k.aiStack[k.aiStackInd].action;                   // `mobActionGet_50A020` — 맨 위(조건도 그대로)
    if (!atWill && noEnemy && (top == 0 || top == 4) && frame_ >= k.idleSoundNext) {
        k.idleSoundNext = frame_ + static_cast<uint32_t>(20 * kServerTickRate) + NextRandom(static_cast<uint32_t>(40 * kServerTickRate + 1));
        MonsterSound(k, 3);                                                                        // +16
        ++b2Stat_[static_cast<size_t>(B2Stat::IdleSound)];
    }
}

// `MonsterLookAtDamager`(`server/object_ai.go:160~166`) — 다쳤으면 때린 것의 자리를 바라본다(25)
bool Server::MonsterLookAtDamager(Creature& k) {
    if (!k.injured) return false;
    if (AIPush(k, 25)) { k.aiStack[k.aiStackInd].argX = k.hitFromX; k.aiStack[k.aiStackInd].argY = k.hitFromY; }
    return true;
}

// 되갚기 `Sub_545E60`(`server/object_ai.go:176~219`) — 10초 안에 맞았고 아직 안 본 피해면:
//   때린 쪽(주인 유닛)이 있으면 — 적이 아니면 0 · 적이면 조건 43(맞는 중) · 싸움(그 자리) · 안 보이면 조건 56·(마음대로 공격이면 59)·가기(7 그 자리) → 1
//     (식충식물은 안 보이면 0 · 보이면 조건 63(적이 SightRange × 1.05 안) — 43 대신)
//   때린 쪽이 없으면 — 떠돌기(10)가 없을 때만 조건 41(5초)·56·(59)·떠돌기 → 0
int Server::MonsterRetaliate(Creature& k) {
    const uint32_t ts = k.lastDamageFrame;
    if (k.hitHandledFrame >= ts || frame_ - ts >= static_cast<uint32_t>(10 * kServerTickRate)) return 0;
    k.hitHandledFrame = ts;
    const bool atWill = k.aggression > 0.66000003f;
    Target att;
    if (SightResolve(k.lastAttacker, att) && !TargetDeadOrGone(att)) {
        if (!CreatureEnemyTo(k, att)) { ++b2Stat_[static_cast<size_t>(B2Stat::RetNotEnemy)]; return 0; }
        float ax = 0, ay = 0;
        TargetXY(att, ax, ay);
        const bool can = CanInteractFrom(k, att);
        if (k.type == kCarnivorousPlantType) {
            if (!can) { ++b2Stat_[static_cast<size_t>(B2Stat::RetNotEnemy)]; return 0; }
            if (AIPush(k, 63)) k.aiStack[k.aiStackInd].argF = k.sightRange * 1.05f;
        } else if (AIPush(k, 43)) {
            k.aiStack[k.aiStackInd].argU32 = frame_;
        }
        if (AIPush(k, 15)) { k.aiStack[k.aiStackInd].argX = ax; k.aiStack[k.aiStackInd].argY = ay; k.aiStack[k.aiStackInd].argU32 = frame_; }
        if (!can) {
            AIPush(k, 56);
            if (atWill) AIPush(k, 59);
            if (AIPush(k, 7)) { k.aiStack[k.aiStackInd].argX = ax; k.aiStack[k.aiStackInd].argY = ay; k.aiStack[k.aiStackInd].argU32 = 0; }
        }
        ++b2Stat_[static_cast<size_t>(B2Stat::RetFight)];
        return 1;
    }
    if (!AIHasAction(k, 10)) {
        if (AIPush(k, 41)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(5 * kServerTickRate);
        AIPush(k, 56);
        if (atWill) AIPush(k, 59);
        AIPush(k, 10);                                        // 떠돌기(길목 0 = 새로 고름 · 정답지 인자 0,0,−128)
        ++b2Stat_[static_cast<size_t>(B2Stat::RetRoam)];
    }
    return 0;
}

// ── ★ 09-29 B3 근접 다시 고르기 ─────────────────────────────────────────
//
// `nox_xxx_monsterPickMeleeTarget_549440` + 거르기 `sub_5494C0`(`GAME5.c:2541~2591`)
//   · 사각형 = 몬스터 자리 ± (근접 사거리 + 제 몸 반지름 + 30(`0x587000+287328` — blob 실측 30.0)) · 거르기는 충돌 상자 겹침
//   · 후보 = 자기 아님 · 유닛(사람·크리처) · 적(`unitIsEnemyTo` — 죽음은 안 본다) · **앞쪽**(내적 > 0.5) ·
//     틈(거리 − 두 몸 반지름) 이 가장 작은 것 — 처음 값 = 근접 사거리(그보다 멀면 못 고른다)
//   ⚠ 사각형은 틈 검사가 이미 더 좁혀서 따로 안 둔다 · 관전자는 뺀다(세계에 몸이 없다)
Target Server::MonsterPickMeleeTarget(const Creature& k, double meleeRange) const {
    Target best;
    double bestGap = meleeRange;
    double myR = 0.0;
    {
        const auto tb = typeBodies_.find(k.type);
        if (tb != typeBodies_.end()) myR = static_cast<double>(tb->second.r);
    }
    const double fx = DirVecX(k.facing), fy = DirVecY(k.facing);
    auto consider = [&](const Target& t, double tx, double ty, double tr) {
        if (!CreatureEnemyTo(k, t)) return;
        const double dx = tx - k.x, dy = ty - k.y;
        const double len = std::sqrt(dx * dx + dy * dy) + 0.001;
        if (dy / len * fy + dx / len * fx <= 0.5) return;
        const double gap = len - (tr + myR);
        if (gap < bestGap) { bestGap = gap; best = t; }
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.observer) continue;
        consider(Target{ kv.first, -1 }, o.posX, o.posY, kBodyRadius);
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (&pets_[i] == &k) continue;
        double r = 0.0;
        const auto tb = typeBodies_.find(pets_[i].type);
        if (tb != typeBodies_.end()) r = static_cast<double>(tb->second.r);
        consider(Target{ std::string(), static_cast<int>(i) }, pets_[i].x, pets_[i].y, r);
    }
    return best;
}

// `objectApplyForce_52DF80` — 대상을 (from → 대상) 쪽으로 민다(힘 < 0 이면 끌어온다) · `f = 10 × 힘 / 무게` · `ForceVec += dp × f / (|dp| + 0.1)`
//   ⚠ 소환수는 ForceVec 적분이 없어 자리로 바로 옮긴다(작살 끌기와 같은 단순화)
void Server::ObjectApplyForce(float fromX, float fromY, const Target& t, double force) {
    float tx = 0, ty = 0;
    if (!TargetXY(t, tx, ty)) return;
    const double dpx = static_cast<double>(tx) - fromX, dpy = static_cast<double>(ty) - fromY;
    const double r = std::sqrt(dpx * dpx + dpy * dpy) + 0.1;
    double mass = 1.0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end() || it->second.respawnAtFrame != 0) return;   // 죽은 것은 안 밀린다
        mass = PlayerMassOf(it->second);
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        const auto tb = typeBodies_.find(pets_[static_cast<size_t>(t.pet)].type);
        mass = (tb != typeBodies_.end() && tb->second.mass > 0.0f) ? tb->second.mass : 1.0;
    }
    if (mass <= 0.0) mass = 1.0;
    const double f = 10.0 * force / mass;
    const double fxv = dpx * f / r, fyv = dpy * f / r;
    if (!t.addr.empty()) {
        Session& s = sessions_.at(t.addr);
        s.forceX += fxv; s.forceY += fyv;
    } else {
        pets_[static_cast<size_t>(t.pet)].x += fxv;
        pets_[static_cast<size_t>(t.pet)].y += fyv;
    }
}

// 근접 함수(monster.bin `MELEE_STRIKE_FUNCTION`) — 1 이면 맞힘 소리 · 0 이면 빗나감 소리(부르는 쪽)
//   · 기본(`strikeMonsterDefault_549380` `GAME5.c:2503~2538`) — 대상을 고르고 없으면 1(허공) · 시야선(추적 5)이 막히면 0 ·
//     피해 함수(대상, 나, 나, 근접 피해, 피해 종류) · 밀어내기(IMPACT > 0) · 1
//   · 거미(`strikeSpider_549BC0` `:2866~2903`) — 대상 없으면 0 · 기본과 같이 때린 뒤 독(확률 %) 걸리면 「aifunc.c:Poisoned」
//   · 유령(`strikeGhost_549A60` `:2806~2860`) — 기본과 같이 때린 뒤 대상에 기절(5) 2초 세기 3 · 대상 자리 바라보기(25) · 때(41 — 2~4초) · 도망(24)
//     ⚠ 도망(24)은 B4a 가 옮긴다 — 그때까지는 때(41)가 다할 때 빠진다
//   · 폭탄병(`strikeBomber_549BB0`) — 늘 1
int Server::MonsterStrike(Creature& k, const MeleeStrikeInfo& si, int dmg, std::vector<OutPacket>& out) {
    if (si.fn == StrikeFn::Bomber) { ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::Bomber)]; return 1; }
    const MonsterStats* m = FindMonster(k.type);
    const double range = (m && m->meleeRange > 0.0) ? m->meleeRange : 0.0;
    const Target v = MonsterPickMeleeTarget(k, range);
    if (v.none()) {
        ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::NoTarget)];
        return si.fn == StrikeFn::Spider ? 0 : 1;
    }
    float vx = 0, vy = 0;
    TargetXY(v, vx, vy);
    if (!MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), vx, vy, 0x5)) {
        ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::Blocked)];
        return 0;
    }
    const float kx = static_cast<float>(k.x), ky = static_cast<float>(k.y);
    const std::string kOwner = k.owner;
    AttackSrc src;
    src.x = kx; src.y = ky;
    src.byMonsterType = k.type;
    src.byPet = static_cast<int>(&k - pets_.data());
    src.rootIsMonster = k.owner.empty();                     // 09-20 — 맵 몬스터가 때리면 뿌리가 몬스터
    const size_t kIdx = static_cast<size_t>(&k - pets_.data());
    const size_t petsBefore = pets_.size();
    const HitterRef hitter = HitterMonster(k);
    HurtTarget(v, dmg, out, kOwner, si.dtype, &src, hitter); // ★ 4-c3b3b a2 = a3 = 그 몬스터(`GAME5.c:2529~2531`)
    ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::Hit)];
    // 때린 뒤 상대(앞 자리 크리처)가 죽으면 번호가 당겨진다 — 그 뒤 몫(밀기·독·유령)은 사람 대상이거나 목록이 그대로일 때만
    const bool listSame = pets_.size() == petsBefore;
    if (si.impact > 0.0f && (listSame || !v.addr.empty())) ObjectApplyForce(kx, ky, v, si.impact);
    if (si.fn == StrikeFn::Spider && si.poisonChance > 0 && (listSame || !v.addr.empty())) {   // `sub_549690` — randomInt(1,100) <= 확률
        if (static_cast<int>(1 + NextRandom(100)) <= si.poisonChance && ActivatePoison(v, si.poisonStrength, si.poisonMax, out)) {
            ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::Poisoned)];
            if (!v.addr.empty()) { auto it = sessions_.find(v.addr); if (it != sessions_.end()) SendPriMsg(it->second, "aifunc.c:Poisoned", out); }
        }
    }
    if (si.fn == StrikeFn::Ghost && listSame && kIdx < pets_.size()) {
        ApplyEnchant(v, 5, static_cast<int>(2 * kServerTickRate), 3);
        Creature& g = pets_[kIdx];
        if (AIPush(g, 25)) { g.aiStack[g.aiStackInd].argX = vx; g.aiStack[g.aiStackInd].argY = vy; }
        if (AIPush(g, 41)) g.aiStack[g.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(2 * kServerTickRate) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate + 1));
        if (AIPush(g, 24)) { g.aiStack[g.aiStackInd].argX = vx; g.aiStack[g.aiStackInd].argY = vy; g.aiStack[g.aiStackInd].argU32 = 0; }
        ++meleeStrike_[static_cast<size_t>(MeleeStrikeWay::GhostFlee)];
    }
    return 1;
}

// ── ★ 09-29 B4 두뇌 반응 층 ─────────────────────────────────────────────
//
// `nox_xxx_monsterMainAIFn_547210`(`GAME5.c:1193~1548`) — 알아보기 뒤 · 조건 앞(`unit_ai.go:576~578`)
//   · 놀기(0)·지키기(4)가 맨 위면 16프레임에 한 번(몬스터마다 엇갈림 — 정답지 `(프레임 + 오브젝트+36 − 갱신자료+548) & 0xF` ·
//     +548 은 행동을 바꾼 프레임(`Field137` — 우리 `aiActionFrame`) · +36 은 번호)
//   · 맨 위 아래에 61(방해 금지)이 있으면 끝 · 죽었으면 끝 · 협동 커서 갈래는 안 옮김(멀티 아님)
//   · 혼란(버프 3)이고 36 이 없으면 62(3)·36 · 거꾸로 주문(`5408D0` — B7)은 늘 0 으로 본다
//   · 두려움(버프 11)·움직일 수 있음·24 없음 → 62(11)·24(제 자리) · 소리 칸 11 → 끝
//   · 후퇴 — 체력 자료가 있고 움직일 수 있고 막힌 지 3초 넘고 24·6·14 가 없을 때, 체력 비율 ≤ 후퇴 비율(ARENA 전부 0)이거나
//     주문 몬스터(STATUS 0x20)가 반주문(29)에 걸렸으면 → 공격 행동 빼기 · 68 · 6(협동의 소환수면 14) · 소리 칸 12 → 끝
//   ⚠ B4b 몫 — 공격 성향(지키기 중 100 안 적) · 거리 두기(도망 거리) · 방패 막기 · 피하기(협동만) · 답답함 · 먹기
void Server::MonsterBrain(Creature& k, std::vector<OutPacket>& out) {
    const uint8_t top = k.aiStack[k.aiStackInd].action;
    if ((top == 0 || top == 4) && ((frame_ + k.netCode - k.aiActionFrame) & 0xF)) return;
    for (int i = k.aiStackInd - 1; i >= 0; --i) if (k.aiStack[i].action == 61) return;
    if (k.health == 0) return;
    ++brainStat_[static_cast<size_t>(BrainStat::Runs)];
    const MonsterBrainInfo bi = MonsterBrainOf(k.type);
    const bool moving = k.speedCur >= 0.0099999998f;                              // `534320`
    const bool cornered = frame_ - k.moveAttemptFrame < static_cast<uint32_t>(3 * kServerTickRate);   // `534810`
    if (k.buffs.Has(3) && !AIHasAction(k, 36)) {                                // `:1315~1322`
        if (AIPush(k, 62)) k.aiStack[k.aiStackInd].argU32 = 3;
        AIPush(k, 36);
        ++brainStat_[static_cast<size_t>(BrainStat::Confused)];
    }
    if (k.buffs.Has(11) && moving && !AIHasAction(k, 24)) {                     // `:1324~1341`
        if (AIPush(k, 62)) k.aiStack[k.aiStackInd].argU32 = 11;
        if (AIPush(k, 24)) { k.aiStack[k.aiStackInd].argX = static_cast<float>(k.x); k.aiStack[k.aiStackInd].argY = static_cast<float>(k.y); k.aiStack[k.aiStackInd].argU32 = 0; }
        MonsterSound(k, 11);                                                     // +48
        ++brainStat_[static_cast<size_t>(BrainStat::Fear)];
        return;
    }
    // ★ B4b — 공격 성향(`:1342~1360`) — 16프레임마다, 성향이 가운데·마음대로이고 지키기(4)가 쌓임에 있는데 맨 위가 아니고 싸움이 없으면
    //   100 안 적(`EnemyAggroXxx`)이 유닛일 때 **맞은 것처럼** 적는다 — INJURED · 마지막 때린 쪽 = **지금 상대**(+1196 — 찾은 것이 아니다) · 피해 종류 11 · 피해 프레임
    //   → 놀기가 되갚기로 그쪽과 싸운다
    const bool mid = k.aggression < 0.66000003f && k.aggression > 0.33000001f;
    const bool atWill = k.aggression > 0.66000003f;
    if (!(frame_ & 0xF) && (mid || atWill) && AIHasAction(k, 4) && k.aiStack[k.aiStackInd].action != 4 && !AIHasAction(k, 15)) {
        const Target f = MonsterEnemyAggro(k, 100.0f);
        if (!f.none()) {
            k.injured = true;
            k.lastAttacker = Creature::SeenRef{};
            if (!k.enemyAddr.empty()) k.lastAttacker.addr = k.enemyAddr; else k.lastAttacker.petNet = k.enemyNet;
            k.lastDamageType = 11;
            k.lastDamageFrame = frame_;
            ++brainStat_[static_cast<size_t>(BrainStat::AggroAtWill)];
        }
    }
    // ★ B4b — 거리 두기(`:1361~1419`) — 성향 ≥ 0.08 · 움직일 수 있음 · 맨 위가 주문(18~20) 아님 · 혼란 아님 · 구석 아님 · 지금 상대가 도망 거리 안이면
    //   (주문 몬스터의 점멸 — B7) 도망(24)이 없고 도망 거리 ≠ 0 이면 28(뒤로 돌기)·68·63(도망 거리 + 30)·24(상대 자리) · 50% 로 소리 칸 11
    if (k.aggression >= 0.079999998f && moving && !(top >= 18 && top <= 20) && !k.buffs.Has(3) && !cornered) {
        const Target e{ k.enemyAddr, k.enemyPet };
        float ex = 0, ey = 0;
        const double d = (!e.none() && TargetXY(e, ex, ey)) ? std::sqrt((ex - k.x) * (ex - k.x) + (ey - k.y) * (ey - k.y)) : 1e9;
        // ★ 09-29 B7 — 주문 몬스터(STATUS 0x20)가 점멸(4)을 쥐고 · 반주문(29) 아니고 · 달아날 때 쉬는 때(+1484) 지났고 · 상대가 **도망 거리 절반** 안이면
        //   곧장 제 자리에 점멸(`mobCast_541300(4, 나, {나, 내 자리})` — 시전 행동 없이) · 쉬는 때 = 지금 + randomInt(0, 6초) → 끝(`:1368~1385`)
        if (d < static_cast<double>(bi.fleeRange) && (bi.status & 0x20) && MonsterHasSpell(k, 4) && !k.buffs.Has(29) && frame_ >= k.castNextEscape &&
            static_cast<double>(bi.fleeRange) * 0.5 > d) {
            MonsterCastSpellNow(k, 4, PetTarget(k), static_cast<float>(k.x), static_cast<float>(k.y), out);
            k.castNextEscape = frame_ + NextRandom(static_cast<uint32_t>(6 * kServerTickRate + 1));
            ++castStat_[static_cast<size_t>(CastStat::BlinkNear)];
            return;
        }
        if (d < 1e9 && bi.fleeRange != 0.0f && !AIHasAction(k, 24)) {
            if (d < static_cast<double>(bi.fleeRange)) {
                if (AIPush(k, 28)) k.aiStack[k.aiStackInd].argU32 = static_cast<uint32_t>((k.facing + 128) & 0xFF);
                AIPush(k, 68);
                if (AIPush(k, 63)) k.aiStack[k.aiStackInd].argF = bi.fleeRange + 30.0f;
                if (AIPush(k, 24)) { k.aiStack[k.aiStackInd].argX = ex; k.aiStack[k.aiStackInd].argY = ey; k.aiStack[k.aiStackInd].argU32 = 0; }
                if (NextRandom(2)) MonsterSound(k, 11);
                ++brainStat_[static_cast<size_t>(BrainStat::KeepDistance)];
                return;
            }
        }
    }
    const int maxHp = TuningFor(k.type).health;
    if (maxHp != 0 && moving && !cornered && !AIHasAction(k, 24) && !AIHasAction(k, 6) && !AIHasAction(k, 14)) {   // `:1421~1423`
        const double ratio = static_cast<double>(k.health) / static_cast<double>(maxHp);
        const bool anti = (bi.status & 0x20) && k.buffs.Has(29);
        if (ratio <= static_cast<double>(bi.retreat) || anti) {
            MonsterPopAttackActions(k);
            AIPush(k, 68);
            AIPush(k, 6);                                                        // 협동(2048)의 소환수면 14 — 멀티 아님
            MonsterSound(k, 12);                                                 // +52
            ++brainStat_[static_cast<size_t>(BrainStat::Retreat)];
            if (anti) ++brainStat_[static_cast<size_t>(BrainStat::RetreatAnti)];
            return;
        }
    }
    // ⚠ 방패 막기(`:1442~1469` — NPC·방패 든 몬스터 — 우리 몬스터에 없음) · 피하기(`:1470~1483` — 협동 전용) 는 안 옮김
    // ★ B4b — 답답함(`:1484~1517`) — 맨 위가 가기(7)·집으로(37)·먼 가기(8)·떠돌기(10)·도망(24)이면: 15 넘게 움직였으면 자리·때를 새로 적고,
    //   0.5초 넘게 15 안에 머물렀으면 FRUSTRATED · 도망·후퇴 중이면 +508(구석) · 싸움 중이면 옆걸음 · 아니면 67% 기다리기(0.5~2초)(33% 는 옆걸음 먼저) → 끝
    {
        const uint8_t cur = k.aiStack[k.aiStackInd].action;
        if (cur == 7 || cur == 37 || cur == 8 || cur == 10 || cur == 24) {
            const double dx = k.stuckX - k.x, dy = k.stuckY - k.y;
            if (dx * dx + dy * dy > 225.0) {
                k.stuckFrame = frame_;
                k.stuckX = static_cast<float>(k.x); k.stuckY = static_cast<float>(k.y);
            } else if (frame_ - k.stuckFrame > static_cast<uint32_t>(kServerTickRate / 2)) {
                k.frustrated = true;
                ++brainStat_[static_cast<size_t>(BrainStat::Frustrated)];
                if (AIHasAction(k, 6) || AIHasAction(k, 14) || AIHasAction(k, 24)) k.moveAttemptFrame = frame_;
                if (AIHasAction(k, 15)) {
                    if (MonsterCheckDodgeables(k)) ++brainStat_[static_cast<size_t>(BrainStat::Sidestep)];
                } else if (NextRandom(101) >= 33 || !MonsterCheckDodgeables(k)) {
                    if (AIPush(k, 1)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(kServerTickRate / 2) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate - kServerTickRate / 2 + 1));
                    ++brainStat_[static_cast<size_t>(BrainStat::WaitStuck)];
                } else {
                    ++brainStat_[static_cast<size_t>(BrainStat::Sidestep)];
                }
                k.stuckFrame = frame_;
                k.stuckX = static_cast<float>(k.x); k.stuckY = static_cast<float>(k.y);
                return;
            }
        }
    }
    // ★ B4b — 먹기(`:1518~1534`) — 성향 ≥ 0.08 · 체력 < 최대 · 16프레임마다 — 75 안 먹을 것(`mobSearchEdible_544A00`) 을 집는다
    //   거르기(`544A40`) — 먹을 것 클래스 · 항아리(4) 아님 · 버섯(0x80)은 **독이 있을 때만** · 물약(8)은 NPC 의 체력 물약만(우리 몬스터 없음) · 보임 · 가장 가까운 것
    //   ⚠ 집은 것이 체력 물약·버섯(0x90)이면 곧바로 쓴다(`useByNetCode`) — 몬스터 쓰기는 안 옮김(버섯만 닿는다) · 사과 등은 집기만
    if (k.aggression >= 0.079999998f && maxHp != 0 && k.health < maxHp && !(frame_ & 0xF)) {
        size_t best = statics_.size();
        double bestD2 = 75.0 * 75.0;
        for (size_t i = 0; i < statics_.size(); ++i) {
            const WorldStatic& w = statics_[i];
            if (!w.isFood) continue;
            if (w.foodBits & 0x4) continue;
            if ((w.foodBits & 0x80) && k.poison == 0) continue;
            if (w.foodBits & 0x8) continue;
            const double dx = w.x - k.x, dy = w.y - k.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 >= bestD2) continue;
            if (!MapTraceVision(static_cast<float>(k.x), static_cast<float>(k.y), static_cast<float>(w.x), static_cast<float>(w.y))) continue;
            bestD2 = d2; best = i;
        }
        if (best < statics_.size()) {
            const uint16_t nc = statics_[best].netCode;
            const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
            Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);             // 사람이 주울 때와 같은 길(바닥에서 사라진다)
            TakeStatic(nc);
            ++brainStat_[static_cast<size_t>(BrainStat::Eat)];
        }
    }
}

// `nox_xxx_monsterCheckDodgeables_547C50`(`GAME5.c:1551~1609`) — 옆걸음 — 보는 쪽의 옆으로 (2~3 × 몸 반지름, 최대 15) 지점을 다섯 번까지 골라
//   선 추적(1)·장애물·용암(바닥 6) 이 아니면 공격 행동을 빼고 때(41 · 1초)·피하기(9 그 지점) · 못 고르면 0
bool Server::MonsterCheckDodgeables(Creature& k) {
    double r = 0.0;
    {
        const auto tb = typeBodies_.find(k.type);
        if (tb != typeBodies_.end()) r = static_cast<double>(tb->second.r);
    }
    const double dirX = DirVecX(k.facing), dirY = DirVecY(k.facing);
    for (int tries = 0; tries < 5; ++tries) {
        double v = (2.0 + NextRandomFloat()) * r;
        if (v > 15.0) v = 15.0;
        if (NextRandom(101) < 50) v = -v;
        const float px = static_cast<float>(v * -dirY + k.x), py = static_cast<float>(v * dirX + k.y);
        if (!MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), px, py, 0x1)) continue;
        if (!MapTraceObstacles(-1, static_cast<float>(k.x), static_cast<float>(k.y), px, py)) continue;
        if (floorGrid_.TileAt(px, py, floorEdges_).tile == 6) continue;
        MonsterPopAttackActions(k);
        if (AIPush(k, 41)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(kServerTickRate);
        if (AIPush(k, 9)) { k.aiStack[k.aiStackInd].argX = px; k.aiStack[k.aiStackInd].argY = py; k.aiStack[k.aiStackInd].argU32 = 0; }
        return true;
    }
    return false;
}

// `EnemyAggroXxx`(`server/object_ai.go:225~263`, r = max = 100) — 둘레 안 유닛·생성기 중 적 · 안 죽음 · 보임 · 가장 가까운 것(악당 버프면 거리 ÷ 3)
Target Server::MonsterEnemyAggro(const Creature& k, float r) const {
    Target best;
    double bestD = r;
    auto consider = [&](const Target& t, double tx, double ty, const Buffs* b) {
        if (!CreatureEnemyTo(k, t) || TargetDeadOrGone(t) || !CanInteractFrom(k, t)) return;
        const double dx = tx - k.x, dy = ty - k.y;
        const double dist = std::sqrt(dx * dx + dy * dy) + 0.001;
        if (dist > r) return;                                  // `EachObjInCircle` 반지름
        double d2 = dist;
        if (b && b->Has(10)) d2 /= 3.0;                        // ENCHANT_VILLAIN
        if (d2 < bestD) { bestD = d2; best = t; }
    };
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        consider(Target{ kv.first, -1 }, kv.second.posX, kv.second.posY, &kv.second.buffs);
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (&pets_[i] == &k) continue;
        consider(Target{ std::string(), static_cast<int>(i) }, pets_[i].x, pets_[i].y, &pets_[i].buffs);
    }
    return best;
}

// ── ★ 09-29 B5 애니 칸 · 걸음 소리 ─────────────────────────────────────
//
// `nox_xxx_mobActionToAnimation_533790`(`GAME4_3.c:2278~2364`) — 맨 위 행동 → 애니 순번(libs `MonsterAnimationType`)
//   걷는 행동(7·8·10·13·29·36·37)은 달리기(0x4000)면 13 MOVE_2 · 아니면 12 MOVE · 피하기 9 는 12 · 근접 16 → 1 · 쏘기 17 → 3 ·
//   주문 18~20 → 7 · 막기 21·23 → 5 · 22 → 6 · 도망 24 → 13 · 죽어가는 중 30 → 9 · 죽음 31 → 10 · 변신 33·35 → 14 · 34 → 15 · 그 밖 8 IDLE
//   · 식충식물은 놀기(8)인데 지금 상대가 없으면 14 · 흉내쟁이·좀비 갈래는 우리 몬스터에 없음
int Server::MonsterAnimIndex(const Creature& k) const {
    int v = 8;
    switch (k.aiStack[k.aiStackInd].action) {
        case 7: case 8: case 10: case 13: case 29: case 36: case 37: v = k.running ? 13 : 12; break;
        case 9:  v = 12; break;
        case 16: v = 1;  break;
        case 17: v = 3;  break;
        case 18: case 19: case 20: v = 7; break;
        case 21: case 23: v = 5; break;
        case 22: v = 6;  break;
        case 24: v = 13; break;
        case 30: v = 9;  break;
        case 31: v = 10; break;
        case 33: case 35: v = 14; break;
        case 34: v = 15; break;
        default: v = 8;  break;
    }
    if (k.type == kCarnivorousPlantType && v == 8 && k.enemyAddr.empty() && k.enemyPet < 0) return 14;   // `534A10` · +1196 없음
    return v;
}

// ★ 10-01 — 몸 깃발(+16) — thing.bin FLAGS 에 죽음 덧칠(`Creature::flagsSet`·`flagsClear`). 몸을 모르는 종류는 덧칠만
uint32_t Server::PetObjFlags(const Creature& k) const {
    const auto tb = typeBodies_.find(k.type);
    const uint32_t base = (tb != typeBodies_.end()) ? tb->second.flags : 0u;
    return (base | k.flagsSet) & ~k.flagsClear;
}

// `nox_xxx_updateNPCAnimData_50A850`(`GAME4_1.c:532~579`) — 조건 뒤 · 행동 앞에 한 번(`unit_ai.go:579`) · 끝났으면(+483) 안 넘긴다
//   칸 수(+480)를 적고 0 이면 끝 · 칸 안 틱(+482)을 올려 붙드는 틱 + 1 에 닿으면 다음 칸(+481) · 끝 칸을 넘으면 되풀이면 0, 아니면 끝 칸에 서고 끝
//   ⚠ NPC(아종 0x10)의 사람 애니 갈래는 우리 몬스터에 없다
void Server::MonsterAnimUpdate(Creature& k) {
    if (k.animDone) return;
    const auto it = monsterAnims_.find(k.type);
    if (it == monsterAnims_.end()) { ++animStat_[static_cast<size_t>(AnimStat::NoTable)]; return; }   // +476 표 없음
    const MonsterAnimRow& a = it->second[static_cast<size_t>(MonsterAnimIndex(k))];
    k.animFrames = a.frames;
    if (a.frames == 0) { k.animDone = true; ++animStat_[static_cast<size_t>(AnimStat::NoFrames)]; return; }
    k.animTick = static_cast<uint8_t>(k.animTick + 1);
    if (k.animTick < a.hold + 1) return;
    k.animTick = 0;
    k.animFrame = static_cast<uint8_t>(k.animFrame + 1);
    ++animStat_[static_cast<size_t>(AnimStat::Advance)];
    if (k.animFrame < a.frames) return;
    if (a.loop) { k.animFrame = 0; ++animStat_[static_cast<size_t>(AnimStat::Wrap)]; return; }
    k.animFrame = static_cast<uint8_t>(a.frames - 1);
    k.animDone = true;
    ++animStat_[static_cast<size_t>(AnimStat::Done)];
}

// ★ 09-30 — 근접·쏘기가 칸에서 때린 **시점**을 센다(계수 뜻은 `AttackTiming` 머리 주석) — 부르는 자리 = 칸 · 틱 0 에 닿은 그 갱신
void Server::CountAttackTiming(const Creature& k, int kind, int frame) {
    const size_t base = kind == 0 ? static_cast<size_t>(AttackTiming::MeleeOnTime) : static_cast<size_t>(AttackTiming::ShotOnTime);
    if (k.animDone) { ++attackTiming_[base + 2]; return; }                 // 끝 칸 두 번째
    const uint32_t lag = frame_ - k.attackStartFrame;
    if (!attackLagSeen_[kind] || lag < attackLagMin_[kind]) attackLagMin_[kind] = lag;
    if (!attackLagSeen_[kind] || lag > attackLagMax_[kind]) attackLagMax_[kind] = lag;
    attackLagSeen_[kind] = true;
    const auto it = monsterAnims_.find(k.type);
    const uint32_t hold = it == monsterAnims_.end() ? 0u : it->second[kind == 0 ? 1u : 3u].hold;
    const uint32_t want = static_cast<uint32_t>(frame) * (hold + 1u);
    const bool onTime = lag == want || (want > 0 && lag == want - 1);
    ++attackTiming_[base + (onTime ? 0 : 1)];
}

// `nox_xxx_monsterMoveAudio_534030`(`GAME4_3.c:2648~2691`) — 지금 칸(+481)이 걸음 칸 A·B(monster.bin +100·+104) 중 하나이고 칸 안 틱(+482)이 0 이면
//   소리 모음 칸 17(MOVE_SOUND · +72)을 몬스터 자리에서 · 부르는 자리 = 가기(7) 끝 · 도망(24) 걸을 때 · 아무 데나 걷기(`545020`) 끝 · 떠돌기(10 — 우리는 가기로 걷는다)
//   ⚠ NPC(아종 0x30)의 사람 걷기 애니 갈래는 우리 몬스터에 없다
void Server::MonsterMoveAudio(Creature& k, AnimStat site) {
    ++animStat_[static_cast<size_t>(site)];
    const MonsterMoveSound ms = MonsterMoveSoundOf(k.type);
    if ((k.animFrame == ms.a || k.animFrame == ms.b) && k.animTick == 0) {
        ++animStat_[static_cast<size_t>(AnimStat::StepHit)];
        MonsterSound(k, 17);
    }
}

// ── ★ 09-29 B6 소리 듣기 ────────────────────────────────────────────────
//
// `aiListenToSounds`(`unit_ai.go:314~360`) — 몬스터 갱신 앞(`unitUpdateMonster_50A5C0` `:549` — 알아보기보다 먼저)
//   · 죽일 수 없는 것(체력 자료 없음·죽음 — `checkIsKillable_528190`)은 안 듣는다 · 물고기·개구리·쥐 갈래는 우리 몬스터에 없음
//   · 들을 거리는 2프레임까지만 산다(지금 < 그 프레임 이거나 2 넘게 지났으면 지움)
//   · 귀가 안 막혔고(+404 ≤ 그 기록) 들어야 하는 것(`shouldUnitListen`) 중 **세기가 가장 큰 것**(같으면 새것 — 정답지 목록은 새것이 앞)
//   · 그것이 마지막 들은 것보다 새롭거나(프레임) 더 크면(+408) 들은 것으로 적는다(`unitEmitHearEvent_50D110` — 스크립트 사건은 안 옮김)
void Server::MonsterListen(Creature& k) {
    for (size_t i = 0; i < listens_.size();) {
        const ListenRec& r = listens_[i];
        if (frame_ < r.frame || frame_ - r.frame > 2) { listens_.erase(listens_.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        ++i;
    }
    if (k.health == 0 || TuningFor(k.type).health == 0) return;
    const ListenRec* best = nullptr;
    int maxDist = 0;
    for (auto it = listens_.rbegin(); it != listens_.rend(); ++it) {
        const ListenRec& r = *it;
        // `ud.Field101 <= it.frame` — 막힌 귀(+404 가 앞날)는 여기서 걸린다(그래서 `shouldUnitListen` 의 같은 검사는 이 흐름에선 안 닿는다 · 셈은 여기)
        if (k.heardFrame > r.frame) { if (k.heardFrame > frame_) ++hearStat_[static_cast<size_t>(HearStat::Deaf)]; continue; }
        if (!MonsterShouldListen(k, r)) continue;
        const int d = MonsterTraceSound(k, r);
        if (d > 0 && d > maxDist) { maxDist = d; best = &r; }
    }
    if (!best || !(best->frame > k.heardFrame || maxDist > k.heardDist)) return;
    k.heardSnd = best->snd;
    k.heardFrame = frame_;
    k.heardDist = maxDist;
    k.heardSrcNet = 0;
    if (best->srcIsPet) k.heardSrcNet = best->srcNet;
    else { auto si = sessions_.find(best->srcAddr); if (si != sessions_.end()) k.heardSrcNet = si->second.netCode; }
    k.heardX = best->x;
    k.heardY = best->y;
    ++hearStat_[static_cast<size_t>(HearStat::Heard)];
}

// `shouldUnitListen`(`unit_ai.go:418~450`) — 귀가 막혔으면(+404 > 지금) 거짓 · 낸 것의 임자 사슬 끝(`FindOwnerChainPlayer` — 사람이면 그 사람 ·
//   크리처면 임자 사람, 없으면 그 크리처)이 없으면(사라짐) 깃발만 보고 · 적이면 깃발만 보고 · 아군·자기 것이면 깃발 0x10 이 있어야 한다
//   ⚠ 방(폴리곤)이 다르면 안 듣는 갈래(`Sub_501C00` · `polygonIsPlayerInPolygon`)는 우리 맵에 방 자료가 없어 안 옮김
bool Server::MonsterShouldListen(const Creature& k, const ListenRec& r) {
    if (k.heardFrame > frame_) { ++hearStat_[static_cast<size_t>(HearStat::Deaf)]; return false; }
    const uint16_t flags = soundDefs_[r.snd].flags;
    Creature::SeenRef ref; ref.addr = r.srcAddr; ref.petNet = r.srcIsPet ? r.srcNet : 0;
    Target pu;
    if (!SightResolve(ref, pu)) return flags != 0;
    if (pu.addr.empty() && !pets_[static_cast<size_t>(pu.pet)].owner.empty()) pu = Target{ pets_[static_cast<size_t>(pu.pet)].owner, -1 };
    if (CreatureEnemyTo(k, pu)) return flags != 0;
    if (!(flags & 0x10)) { ++hearStat_[static_cast<size_t>(HearStat::NotListen)]; return false; }
    return true;
}

// `traceSound`(`unit_ai.go:362~375`) — 세기(`soundFadePerc` — 멀티는 끄는 문턱 0 · `game.go:349`)가 문턱(깃발 0x20 → 89 · 0x40 → 20 · 그 밖 50) 아래면 −1 ·
//   몬스터→소리 자리 선 추적(5)이 막히면 반으로 깎아 다시 본다
int Server::MonsterTraceSound(const Creature& k, const ListenRec& r) {
    const uint16_t flags = soundDefs_[r.snd].flags;
    auto enough = [&](int p) { const int th = (flags & 0x20) ? 89 : ((flags & 0x40) ? 20 : 50); return p >= th; };
    int perc = SoundFadePerc(r.snd, r.x, r.y, k.x, k.y);
    if (!enough(perc)) { ++hearStat_[static_cast<size_t>(HearStat::Quiet)]; return -1; }
    if (!MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), r.x, r.y, 0x5)) {
        perc = static_cast<int>(static_cast<double>(perc) * 0.5);
        if (!enough(perc)) { ++hearStat_[static_cast<size_t>(HearStat::WallQuiet)]; return -1; }
        ++hearStat_[static_cast<size_t>(HearStat::WallHalved)];
    }
    return perc;
}

// `sub_5466F0`(`GAME5.c:1065~1112`) — 3초 안에 들은 것(+388)이 있으면: 불에 안 타거나(IMMUNE_FIRE 0x400) 그 자리가 용암(바닥 6)이 아니고 ·
//   닿을 수 있는 칸이면(`sub_50B810` — 벽 칸 아님 · 막힘 아님) — 시야선(추적 9)이 트였으면 바라보기(`545DA0`) · 아니면 59·56·70·69(자리)·7(자리)로 가 본다 ·
//   어느 쪽이든 들은 것을 지우고 1 · 들은 것이 없으면 0
bool Server::MonsterHearReact(Creature& k) {
    if (k.heardSnd == 0 || frame_ - k.heardFrame >= static_cast<uint32_t>(3 * kServerTickRate)) return false;
    const uint32_t sub = MonsterSubclass(k.type);
    bool acted = false;
    if ((sub & 0x400) || floorGrid_.TileAt(k.heardX, k.heardY, floorEdges_).tile != 6) {
        const int gx = static_cast<int>(static_cast<double>(k.heardX) / 23.0), gy = static_cast<int>(static_cast<double>(k.heardY) / 23.0);
        bool airborne = false;
        { const auto tb = typeBodies_.find(k.type); if (tb != typeBodies_.end()) airborne = (tb->second.flags & 0x4000) != 0; }
        if (WallDirAt(gx, gy) == -1 && AIPathCanPass(airborne, sub, gx, gy)) {
            acted = true;
            if (MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), k.heardX, k.heardY, 0x9)) {
                MonsterLookAtHeard(k);
            } else {
                const float hx = k.heardX, hy = k.heardY;
                AIPush(k, 59);
                AIPush(k, 56);
                AIPush(k, 70);
                if (AIPush(k, 69)) { k.aiStack[k.aiStackInd].argX = hx; k.aiStack[k.aiStackInd].argY = hy; }
                if (AIPush(k, 7)) { k.aiStack[k.aiStackInd].argX = hx; k.aiStack[k.aiStackInd].argY = hy; k.aiStack[k.aiStackInd].argU32 = 0; }
                ++hearStat_[static_cast<size_t>(HearStat::ReactGo)];
            }
        }
    }
    if (!acted) ++hearStat_[static_cast<size_t>(HearStat::ReactSkip)];
    k.heardSnd = 0;
    return true;
}

// `sub_545DA0`(`GAME5.c:713~735`) — 3초 안에 들은 것이 있으면 59·56·1(1~2초)·25(들은 자리) 를 쌓고 지운다
bool Server::MonsterLookAtHeard(Creature& k) {
    if (k.heardSnd == 0 || frame_ - k.heardFrame >= static_cast<uint32_t>(3 * kServerTickRate)) return false;
    const float hx = k.heardX, hy = k.heardY;
    AIPush(k, 59);
    AIPush(k, 56);
    if (AIPush(k, 1)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(kServerTickRate) + NextRandom(static_cast<uint32_t>(kServerTickRate + 1));
    if (AIPush(k, 25)) { k.aiStack[k.aiStackInd].argX = hx; k.aiStack[k.aiStackInd].argY = hy; }
    k.heardSnd = 0;
    ++hearStat_[static_cast<size_t>(HearStat::ReactLook)];
    return true;
}

// 조건 69 `DEPENDENCY_LOCATION_IS_SAFE`(`unit_ai.go:215~223`) — 그 자리 둘레 50 에 위험한 것이 없나(`unitIsDangerous_547120`):
//   불(클래스 FIRE) — IMMUNE_FIRE(0x400)가 아니면 · 독구름(ToxicCloud·SmallToxicCloud) — IMMUNE_POISON(0x200)이 아니면 · 클래스 DANGEROUS 는 늘
//   ⚠ 둘레는 오브젝트 가운데 자리로만 잰다(정답지 `EachObjInCircle` 은 몸 크기도 본다) · 보는 오브젝트 = 주문이 놓은 것·세계 물건·판/문 등
bool Server::LocationIsSafe(const Creature& k, float x, float y) const {
    const uint32_t sub = MonsterSubclass(k.type);
    auto danger = [&](uint16_t type, float ox, float oy) {
        const auto it = dangerTypes_.find(type);
        if (it == dangerTypes_.end()) return false;
        const float dx = ox - x, dy = oy - y;
        if (dx * dx + dy * dy > 50.0f * 50.0f) return false;
        switch (it->second) {
            case 1:  return (sub & 0x400) == 0;
            case 2:  return (sub & 0x200) == 0;
            default: return true;
        }
    };
    for (const auto& p : placed_) if (danger(p.type, p.x, p.y)) return false;
    for (const auto& w : statics_) if (danger(w.type, static_cast<float>(w.x), static_cast<float>(w.y))) return false;
    for (const auto& p : props_) if (danger(p.type, static_cast<float>(p.x), static_cast<float>(p.y))) return false;
    return true;
}

// ── ★ 09-29 B7 몬스터 주문 ──────────────────────────────────────────────
//
// 고르기 넷의 공통 꼴 — `mobCastRelated2_540D90`(공격 0x40000000 · 대상 = 상대) · `mobCastRelated_541050`(달아날 때 0x80000000 · 대상 = 자기) ·
//   `monsterBuffSelf_540B90`(제 몸 0x10000000 · 자기) · `monsterCastOffensive_540F20`(되받기 0x20000000 · 상대)
//   관문 — 주문 가능(STATUS 0x20 · `534300`) · 쉬는 때 지남 · 시전 중(맨 위 18~20 · `sub_5408A0`) 아님
//   그 깃발이고 **몬스터가 쓸 수 있는**(주문 깃발 0x10 MobsCanCast) 주문 중 무작위 하나 → `monsterCast_540A30` · 쉬는 때 = 지금 + randomInt(lo, hi)
//   ⚠ 제 몸·달아날 때의 「이미 걸린 버프 주문이 있으면 안 함」(`sub_540CE0` — 버프→주문 표 `getEnchantSpell_424920`)은 우리 표가 없어 안 옮김 —
//     우리 몬스터의 해당 주문은 점멸(4)뿐이고 점멸은 버프가 아니라 겉 차이 없음 · 공격의 소환 주문 갈래(75~114 · 소환 수 한도)도 우리 몬스터에 없음
bool Server::MonsterCastPick(Creature& k, uint32_t mask, const Target& target, uint32_t& nextFrame, int lo, int hi, CastStat stat) {
    if (!(MonsterBrainOf(k.type).status & 0x20)) return false;
    if (frame_ < nextFrame) return false;
    const uint8_t top = k.aiStack[k.aiStackInd].action;
    if (top >= 18 && top <= 20) return false;
    std::vector<uint8_t> list;
    for (const auto& sf : k.spells) if ((sf.second & mask) && (SpellFlags(sf.first) & 0x10)) list.push_back(sf.first);
    if (list.empty()) return false;
    const uint8_t sp = list[NextRandom(static_cast<uint32_t>(list.size()))];
    MonsterCast(k, sp, target);
    nextFrame = frame_ + static_cast<uint32_t>(lo) + NextRandom(static_cast<uint32_t>(hi - lo + 1));
    ++castStat_[static_cast<size_t>(stat)];
    return true;
}

// `MonsterCast`(`server/object_monster.go:569~590`) — 61(안 끊김) · 지속 주문(깃발 0x2)이면 41(0.5~2초)·20 · 아니면 18 ·
//   대상이 자기가 아니고 도망(24) 중이 아니면 [겨눔(0x4)이 아니면 26 바라보기 · 47 사선 막힘] · 49(쏘기 사거리 +212) · 40(또는) · 7(대상 자리)
//   → 「멀거나 사선이 막혔으면 다가가고, 닿으면 바라보고, 바라보면 시전」
void Server::MonsterCast(Creature& k, uint8_t spell, const Target& target) {
    AIPush(k, 61);
    const uint32_t fl = SpellFlags(spell);
    if (fl & 0x2) {
        if (AIPush(k, 41)) k.aiStack[k.aiStackInd].argU32 = frame_ + static_cast<uint32_t>(kServerTickRate / 2) + NextRandom(static_cast<uint32_t>(2 * kServerTickRate - kServerTickRate / 2 + 1));
        if (AIPush(k, 20, target)) k.aiStack[k.aiStackInd].argU32 = spell;
    } else {
        if (AIPush(k, 18, target)) k.aiStack[k.aiStackInd].argU32 = spell;
    }
    const Target self = PetTarget(k);
    const bool toSelf = target.addr.empty() && target.pet >= 0 && target.pet == self.pet;
    if (toSelf || AIHasAction(k, 24)) return;
    if (!(fl & 0x4)) { AIPush(k, 26, target); AIPush(k, 47, target); }
    const MonsterStats* m = FindMonster(k.type);
    if (AIPush(k, 49, target)) k.aiStack[k.aiStackInd].argF = static_cast<float>(m ? m->missileRange : 0.0);
    AIPush(k, 40);
    float tx = 0, ty = 0;
    TargetXY(target, tx, ty);
    AIPush(k, 7, target, tx, ty);
}

// `mobActionCast_5413B0`(`GAME4_3.c:8799~8840`) — 칸 안 틱(+482) 0 일 때만: 지금 칸(+481)이 쏘기 칸(monster.bin +216)이면
//   대상(0 = 쌓임의 대상 · 1 = 쌓임의 자리)으로 · 반주문(29)이 아니면 `mobCast_541300` · 1칸이면 소리 칸 13(SPELL_INIT +56)
//   ⚠ 0 갈래의 「자리 흔들기」(`mobCastRandomRecoil_541490`)는 대상 자리로 둔다 — 우리 몬스터 주문(전기 광선·점멸)은 자리를 안 쓴다
void Server::MonsterActionCast(Creature& k, int a2, std::vector<OutPacket>& out) {
    if (k.animTick != 0) return;
    const Creature::AIStackItem& st = k.aiStack[k.aiStackInd];
    if (k.animFrame == static_cast<uint8_t>(MonsterMissileFrameOf(k.type))) {
        Target target;
        float x = 0, y = 0;
        if (a2 == 0) { target = Target{ st.targetAddr, st.targetPet }; TargetXY(target, x, y); }
        else if (a2 == 1) { x = st.argX; y = st.argY; }
        else return;
        if (k.buffs.Has(29)) { ++castStat_[static_cast<size_t>(CastStat::CastAnti)]; return; }
        MonsterCastSpellNow(k, static_cast<uint8_t>(st.argU32), target, x, y, out);
    } else if (k.animFrame == 1) {
        MonsterSound(k, 13);
        ++castStat_[static_cast<size_t>(CastStat::InitSound)];
    }
}

// `mobCast_541300` + `castSpellByUser_4FDD20` → `castSpell`(`spells.go:432~443`) — 그 자리를 보게 돌리고(`mobCalcDir_533CC0`) ·
//   공격 주문(0x20)이면 투명(0)·무적(23)을 끄고 타원 방패(67)를 끈다 · 겨눔(0x4)이 아니거나 대상이 자기면 곧장 받아들임(`spellAccept`) ·
//   겨눔이고 남이면 날아가는 주문(`createSpellFly_4FDDA0`) — 우리 몬스터에 없음
//   받아들임 — 전기 광선(24) = 지속(30) · 점멸(4) = 지속(0, 대상 자기) · 그 밖은 안 옮김
bool Server::MonsterCastSpellNow(Creature& k, uint8_t spell, const Target& target, float x, float y, std::vector<OutPacket>& out) {
    ++castStat_[static_cast<size_t>(CastStat::Cast)];
    if ((x != 0.0f || y != 0.0f) && (x != static_cast<float>(k.x) || y != static_cast<float>(k.y))) k.facingWant = DirFromVec(x - k.x, y - k.y);   // 제 자리면 안 돌림
    const uint32_t fl = SpellFlags(spell);
    const Target self = PetTarget(k);
    if (fl & 0x20) {
        BuffOff(self, 0);
        BuffOff(self, 23);
        DurCancelForPet(67, k.netCode, out);
    }
    const bool toSelf = target.addr.empty() && target.pet >= 0 && target.pet == self.pet;
    if ((fl & 0x4) && !toSelf) { ++castStat_[static_cast<size_t>(CastStat::NotPorted)]; return false; }
    DurFns f;
    uint32_t dt = 0;
    if (spell == 24) { f.create = &Server::DurEnergyBoltCreate; f.update = &Server::DurEnergyBoltTick; dt = 30; }
    else if (spell == 4) { f.create = &Server::DurBlinkCreate; f.update = &Server::DurBlinkTick; dt = 0; }
    else { ++castStat_[static_cast<size_t>(CastStat::NotPorted)]; return false; }
    ++castStat_[static_cast<size_t>(CastStat::Accepted)];
    return MonsterDurNew(k, spell, spell == 4 ? self : target, f, dt, out);
}

// `spellsDuration.New`(`spells_dur.go:88`)의 몬스터 시전자 판 — 시전자 = 그 몬스터(번호) · 자리(+28) = 몬스터 자리 · 같은 주문의 옛것은 끄고 ·
//   취소 깃발 붙은 것을 치우고 · 머리에 넣고 · 켜는 소리(몬스터 자리) · 만들기 함수가 0 이 아니면 취소
bool Server::MonsterDurNew(Creature& k, uint8_t spell, const Target& target, const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out) {
    if (k.health == 0) return false;
    const uint16_t net = k.netCode;
    const float kx = static_cast<float>(k.x), ky = static_cast<float>(k.y);
    const Target self = PetTarget(k);
    DurCancelForPet(spell, net, out);
    DurOnNewSpell(out);
    DurSpell d;
    d.id = ++durLastId_;
    d.spell = spell;
    d.level = k.spellLevel;
    d.casterPet = net;
    d.posX = kx; d.posY = ky;
    d.target = target;
    float tx = 0, ty = 0;
    if (TargetXY(target, tx, ty)) { d.pos2X = tx; d.pos2Y = ty; }
    d.frame60 = frame_;
    d.frame64 = frame_;
    d.frame68 = dt + frame_;
    d.fns = fns;
    durSpells_.push_front(d);
    PlaySoundAt(SpellSoundOn(spell), kx, ky, 0, 0, std::string(), self);
    DurSpell& p = durSpells_.front();
    if (fns.create == nullptr || (this->*fns.create)(p, out) == 0) return true;
    DurCancel(p, out);
    return false;
}

// `CancelFor`(`spells_dur.go:169`)의 몬스터 시전자 판
void Server::DurCancelForPet(uint8_t spell, uint16_t net, std::vector<OutPacket>& out) {
    if (net == 0) return;
    for (auto& d : durSpells_) if (d.spell == spell && d.casterPet == net && !(d.flags88 & 1)) DurCancel(d, out);
}

// `spellEnergyBoltSetTarget_52EC60` 의 몬스터 시전자 판 — 둘레(번개 사거리) 안 · 자기 아님 · 살아 있음 · 적(`CreatureEnemyTo`) ·
//   앞쪽(`TestTwoPointsAndDirection` — 몬스터 보는 쪽) · 볼 수 있음(`CanInteractFrom`) · 가장 가까운 것
Target Server::EnergyBoltSearchPet(const Creature& k, float range) const {
    float best = range * range;
    Target found;
    const float kx = static_cast<float>(k.x), ky = static_cast<float>(k.y);
    auto consider = [&](const Target& t, float ox, float oy) {
        if (!CreatureEnemyTo(k, t) || TargetHealth(t) <= 0) return;
        if (!(TestTwoPointsAndDirection(kx, ky, k.facing, ox, oy) & 1)) return;
        if (!CanInteractFrom(k, t)) return;
        const float dx = ox - kx, dy = oy - ky, d2 = dx * dx + dy * dy;
        if (d2 >= best) return;
        best = d2; found = t;
    };
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame || kv.second.respawnAtFrame != 0) continue;
        consider(Target{ kv.first, -1 }, static_cast<float>(kv.second.posX), static_cast<float>(kv.second.posY));
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (&pets_[i] == &k || pets_[i].health == 0) continue;
        consider(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y));
    }
    return found;
}

// `nox_xxx_monsterPopAttackActions_5471B0` — 맨 위가 공격 행동(16~20 · 25~28)인 동안 뺀다
void Server::MonsterPopAttackActions(Creature& k) {
    for (int guard = 0; guard <= Creature::kAIStackMax; ++guard) {
        const uint8_t a = k.aiStack[k.aiStackInd].action;
        if (!((a >= 16 && a <= 20) || (a >= 25 && a <= 28))) return;
        AIPop(k);
    }
}

// `nox_xxx_monsterCanResumeAttack_545520` — 체력 비율(최대 0 이면 1) ≥ 회복 비율(+1344)
bool Server::MonsterCanResumeAttack(const Creature& k) const {
    const int maxHp = TuningFor(k.type).health;
    const double ratio = maxHp != 0 ? static_cast<double>(k.health) / static_cast<double>(maxHp) : 1.0;
    return ratio >= static_cast<double>(MonsterBrainOf(k.type).resume);
}

// `Nox_xxx_generateRetreatPath_50CA00`(`unit_ai_path.go:526~530`) — 길찾기 멀어지기 모드 · 거르기 = 그 칸 가운데 100 안에 적이 없음(`HasNoEnemiesAround`) · 깊이 6
int Server::CreatureGenRetreatPath(Creature& k, float fromX, float fromY) {
    uint32_t oflags = 0;
    auto tb = typeBodies_.find(k.type);
    if (tb != typeBodies_.end()) oflags = tb->second.flags;
    const bool airborne = (oflags & 0x4000) != 0;
    auto noEnemies = [&](int gx, int gy) {
        const double cx = gx * 23.0 + 11.5, cy = gy * 23.0 + 11.5;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (!o.inGame) continue;
            const double dx = o.posX - cx, dy = o.posY - cy;
            if (dx * dx + dy * dy <= 100.0 * 100.0 && CreatureEnemyTo(k, Target{ kv.first, -1 })) return false;
        }
        for (size_t i = 0; i < pets_.size(); ++i) {
            if (&pets_[i] == &k) continue;
            const double dx = pets_[i].x - cx, dy = pets_[i].y - cy;
            if (dx * dx + dy * dy <= 100.0 * 100.0 && CreatureEnemyTo(k, Target{ std::string(), static_cast<int>(i) })) return false;
        }
        return true;
    };
    AIPathFind(true, PathActor{ airborne, 0, -1 }, static_cast<float>(k.x), static_cast<float>(k.y), fromX, fromY, PathFilter(noEnemies), 6);
    if (aiPathPts_.size() > kCreaturePathMax && aiPathStatus_ == 0) aiPathStatus_ = 1;
    k.path.clear();
    for (size_t i = 0; i < aiPathPts_.size(); ++i) {
        if (k.path.size() >= kCreaturePathMax - 1) break;
        k.path.push_back(aiPathPts_[i]);
    }
    return static_cast<int>(k.path.size());
}

// 놀기 `AIActionIdle.Update`(`unit_ai.go:637~670`)
//   성향이 가운데(0.33~0.66)이거나 마음대로 공격(> 0.66)이면 — 지금 상대가 있으면 싸움(그 자리) · 들은 소리가 있으면(`Sub_5466F0` — B6) 그쪽 → 끝
//   성향 < 0.08 이거나 되갚기가 0 이면 — 다쳤으면 때린 자리 바라보기 · 아니면 행동 바꾼 지 0.5초 안이거나 안 움직였으면 치유(B7) · 그 밖에는 지난 자리 바라보기(25)
void Server::MonsterIdleUpdate(Creature& k) {
    const bool mid = k.aggression < 0.66000003f && k.aggression > 0.33000001f;   // `Sub_5343C0`
    const bool atWill = k.aggression > 0.66000003f;                              // `534390`
    if (mid || atWill) {
        const Target e{ k.enemyAddr, k.enemyPet };
        float ex = 0, ey = 0;
        if (!e.none() && TargetXY(e, ex, ey)) {
            if (AIPush(k, 15)) { k.aiStack[k.aiStackInd].argX = ex; k.aiStack[k.aiStackInd].argY = ey; k.aiStack[k.aiStackInd].argU32 = frame_; }
            ++b2Stat_[static_cast<size_t>(B2Stat::IdleFight)];
            return;
        }
        if (MonsterHearReact(k)) return;                                          // ★ 09-29 B6 — `Sub_5466F0` 들은 소리(3초 안) 쪽으로
    }
    if (k.aggression < 0.079999998f || MonsterRetaliate(k) == 0) {              // `Sub_534440` || `Sub_545E60() == 0`
        if (MonsterLookAtDamager(k)) { ++b2Stat_[static_cast<size_t>(B2Stat::IdleLookDamager)]; return; }
        if (frame_ - k.aiActionFrame <= static_cast<uint32_t>(kServerTickRate / 2) || (k.x == k.prevX && k.y == k.prevY)) {
            ++b2Stat_[static_cast<size_t>(B2Stat::IdleStay)];                    // `mobHealSomeone_5411A0` — 치유 주문(B7)
        } else {
            if (AIPush(k, 25)) { k.aiStack[k.aiStackInd].argX = static_cast<float>(k.prevX); k.aiStack[k.aiStackInd].argY = static_cast<float>(k.prevY); }
            ++b2Stat_[static_cast<size_t>(B2Stat::IdleFaceBack)];
        }
    }
}

// ── 묶음 5-h 금 ───────────────────────────────────────────────────────
//
// sub_4D8870(`GAME3_2.c:4150`) — `4a <금 u32>` **5바이트**. `netSendPacket0(…, 1)` 이라 **중요(cc)** 다.
//   ⚠ 충전 알림(`64`)과 다르다 — 그쪽은 마지막 인자가 0 이라 중요가 아니었다.
void Server::SendGold(Session& s, std::vector<OutPacket>& out) {
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_GOLD);
    msg[1] = static_cast<uint8_t>(s.gold);
    msg[2] = static_cast<uint8_t>(s.gold >> 8);
    msg[3] = static_cast<uint8_t>(s.gold >> 16);
    msg[4] = static_cast<uint8_t>(s.gold >> 24);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// playerAddGold_4FA590(`GAME4.c:1862`) — 갱신자료 `+2164` 에 더하고 알린다.
void Server::PlayerAddGold(Session& s, uint32_t v, std::vector<OutPacket>& out) {
    s.gold += v;
    SendGold(s, out);
}

// playerSubGold_4FA5D0(`GAME4.c:1870`) — 뺀다. ★ **모자라면 0 으로 내린다**(음수로 안 간다).
void Server::PlayerSubGold(Session& s, uint32_t v, std::vector<OutPacket>& out) {
    s.gold = (s.gold >= v) ? (s.gold - v) : 0;
    SendGold(s, out);
}

// ── 묶음 5-e 충전 완드 ────────────────────────────────────────────────
//
// netReportCharges_4D82B0(`GAME3_2.c:3830`) — `64 <번호 2> <지금> <최대>` **5바이트**.
//   ⭕ 짜임은 디컴파일 그대로다. `netSendPacket1(…, 0, 0)` 이라 **중요(cc)가 아니다**.
void Server::SendCharges(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_CHARGES);
    msg[1] = static_cast<uint8_t>(c.netCode);
    msg[2] = static_cast<uint8_t>(c.netCode >> 8);
    msg[3] = c.charges;
    msg[4] = c.chargesMax;
    Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
}

// useWandCastSpell_53F4F0(`GAME4_3.c:7934`) — 완드로 주문을 건다.
//   ★ 이것이 묶음 4 에서 「충전 지팡이로 걸면 마나 대신 충전을 쓴다」로 미뤄 둔 바로 그 길이다.
//     **마나를 아예 안 본다** — 주문을 곧장 `spellAccept` 로 넣는다(세기 4 고정).
//   · **최대 != 0 이고 지금 == 0** 이면 못 쓴다(소리 222 — 묶음 10)
//   · 재사용 대기 — `지금 프레임 − 마지막 − (대기 − 2×준비효과)`. ⚠ 준비효과(아이템 수식)는 아직 없어 0
//   · 주문이 받아들여져야 **마지막 프레임을 갱신하고 충전을 깎는다**(실패하면 안 깎는다)
//   · 최대 != 0 일 때만 깎고 `REPORT_CHARGES` 를 보낸다(= 0 은 무한 완드)
// ⚠ 안 옮긴 갈래 — 정답지는 `a2[2] & 0x1000 && a2[3] & 0x4040000` 이면 **안 깎는다**.
//   그 두 비트가 무엇인지 못 짚어(오브젝트 깃발·클래스로 보이나 확증 없음) 늘 깎는 쪽으로 두었다.
bool Server::UseWandCast(Session& s, Session::Carried& c, std::vector<OutPacket>& out) {
    if (c.wandSpell == 0) return false;
    if (c.chargesMax != 0 && c.charges == 0) {                // 다 썼다
        // 10-f PlaySound 222 DepletedWand (GAME4_3.c:7907, kind 0 at the player)
        PlaySoundAt(222, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
        return false;
    }
    // ⚠ 「아직 안 썼다」를 따로 안 둔다 — 정답지도 `+104` 가 0 인 채로 `지금 − 0 >= 대기` 를 본다.
    //   즉 **판이 시작하고 대기만큼 지나야 첫 발이 나간다.** 그게 원본 버릇이다.
    if (frame_ - c.wandLastFrame < c.wandCooldown) return false;

    SpellArg arg;
    // ★ 겨눈 자리는 커서, **대상은 커서가 집은 것**. 집은 것이 없으면 **시전자 자신**이다
    //   (`GAME4_3.c:7967~7972` — `+288` 이 0 이면 `v11[0] = a1`). 이걸 빼면 방패처럼
    //   대상이 있어야 도는 주문이 완드로는 영영 안 걸린다.
    arg.obj = FindObjectAtCursor(s);
    if (arg.obj.none()) arg.obj = Target{ s.addr, -1 };
    arg.x = static_cast<float>(s.aimX);
    arg.y = static_cast<float>(s.aimY);
    if (!SpellAccept(c.wandSpell, s, s.addr, arg, /*lvl=*/4, out)) return false;

    c.wandLastFrame = frame_;
    if (c.chargesMax != 0) {
        --c.charges;
        SendCharges(s, c, out);
    }
    return false;   // ★ 완드는 **안 없어진다** — 충전만 준다
}

// ── 묶음 5-d 먹을거리·버섯·사이다 ─────────────────────────────────────
//
// useConsume_53EE10(`object_use.go:13~55`) — 먹는 것.
//   · 먹을거리(CLASS 에 FOOD)가 아니면 아무 일도 안 난다
//   · **물약 갈래(`POTION`)는 여기서 빠진다** — 그쪽은 `usePotion` 이 맡는다
//   · **체력이 가득이면 안 먹는다**(그대로 남는다)
//   · `USE` 값만큼 체력을 움직이고 없어진다
//     ★ 값이 **음수인 것이 있다** — thing.bin 실측 `USE = ConsumeUse -15`.
//       먹으면 깎이는 음식이다. 「회복량」이라고 부르면 이 갈래를 놓친다
// ⚠ 안 옮긴 것 — 남녀·먹을거리 갈래별 소리(`SoundHumanFemaleEatApple` 따위)는 묶음 10.
bool Server::UseConsume(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    if (!c.isFood) return false;                             // CLASS 에 FOOD 가 아님
    if (c.foodBits & kFoodPotion) return false;              // 물약은 usePotion 담당
    if (c.useAmount == 0) return false;                      // UseData 없음
    if (s.health >= s.maxHealth) return false;               // 가득이면 안 먹는다
    UnitAdjustHP(Target{ s.addr, -1 }, c.useAmount, out);
    return true;
}

// useMushroom_53ECE0(`object_use.go:57~70`) — 버섯.
//   · 독이 있으면 **풀고** 글 `Use.c:MushroomClean` · 없으면 글 `Use.c:MushroomConfuse`
//   · ★ **어느 쪽이든 10초 혼란(세기 5)** 이 걸린다. 독을 풀어도 어지럽다
//   · 늘 없어진다(체력이 가득이어도)
bool Server::UseMushroom(Session& s, std::vector<OutPacket>& out) {
    if (s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        SendPriMsg(s, "Use.c:MushroomClean", out);
    } else {
        SendPriMsg(s, "Use.c:MushroomConfuse", out);
    }
    ApplyEnchant(Target{ s.addr, -1 }, 3, 10 * static_cast<int>(kServerTickRate), 5);   // ENCHANT_CONFUSED
    return true;
}

// useCiderConfuse_53EF00(`object_use.go:72~85`) — 사이다.
//   · **5초 혼란(세기 4)** + 글 `Use.c:CiderConfuse`
//   · 그러고 나서 먹기(`useConsume`)를 부른다 — 체력이 가득이면 안 채워지지만
//     ★ **사이다는 그래도 없어진다**(정답지가 `useConsume` 의 답과 무관하게 지운다)
bool Server::UseCiderConfuse(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    ApplyEnchant(Target{ s.addr, -1 }, 3, 5 * static_cast<int>(kServerTickRate), 4);    // ENCHANT_CONFUSED
    SendPriMsg(s, "Use.c:CiderConfuse", out);
    UseConsume(s, c, out);
    return true;
}

// ── 묶음 5-c 줍는 순간 마시기 ─────────────────────────────────────────
//
// playerClassCanUseItem_57B3D0 + sub_57B370(`object_pickup.go:107~131`) —
//   **그 직업이 이 물건을 쓸 수 있나.** 먹을거리 갈래는 표가 아니라 **셈**으로 나온다:
//     `허용 = ~(SUBCLASS >> 5) | 0xFE`  →  `(1 << 직업) & 허용` 이 0 이 아니면 쓸 수 있다
//   `SUBCLASS >> 5` 의 맨 아랫비트가 **MANA_POTION**(갈래표 다섯째)이다. 그래서
//     · 마나 물약이면 허용 = 0xFE → 워리어(1 << 0 = 1)만 걸러진다
//     · 그 밖이면  허용 = 0xFF → 누구나 쓴다
//   ★ 즉 이 셈 한 줄이 뜻하는 것은 **「워리어는 마나 물약을 못 든다」** 하나다.
// ⚠ 무기·완드·방어구 갈래는 **수식표**(`Modif`)를 봐야 한다 — 그 표가 아직 없어 안 옮겼다.
//   여기(`PotionPickup`)로 오는 것은 먹을거리뿐이라 이 조각에서는 걸리지 않는다.
bool Server::ClassCanUseFood(CharClass cls, uint32_t foodBits) {
    const uint32_t allow = (~(foodBits >> 5) | 0xFEu) & 0xFFu;
    return ((1u << static_cast<int>(cls)) & allow) != 0;
}

// pickupPotion_4F37D0(`object_pickup.go:133~181`) — 물약을 주울 때.
//   돌려주는 값 — **−1 못 줍는다 · 0 그냥 가방으로 · 1 그 자리에서 마셨다**
//
// ★ 가방에서 쓸 때(`UsePotion`)와 **문턱이 다르다**:
//     가방  — `지금 < 최대`         (모자라기만 하면 마신다)
//     줍기  — `회복량 + 지금 < 최대` (**넘칠 것 같으면 안 마시고 가방으로 간다**)
//   아까운 물약을 버리지 않게 하는 규칙이다. 둘을 같게 만들면 가득 차기 직전에
//   50 짜리를 주워 1 만 채우고 날리게 된다.
//
// ⚠ 안 옮긴 것 — 버프 물약 갈래는 **줍기에는 없다**(그냥 가방으로) · 소리는 묶음 10 ·
//   `CheckXxx`(사람마다 켜지는 비트 `playersXxx`)는 무엇을 뜻하는지 못 짚어 안 옮겼다.
//   그 비트가 켜지면 줍는 순간 **아무것도 안 마시고** 바로 가방으로 간다.
int Server::PickupPotion(Session& s, const WorldStatic& w, std::vector<OutPacket>& out) {
    // ① 직업 검사 — **온라인이고 퀘스트가 아닐 때만**(gameFlags 0x2000 && !0x1000).
    //   우리 서버는 늘 온라인 아레나라 퀘스트 갈래만 빼면 된다.
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && !ClassCanUseFood(s.charClass, w.foodBits)) {
        SendPriMsg(s, "pickup.c:ObjectEquipClassFail", out);
        return -1;
    }

    bool consumed = false;
    // ② 체력 — **회복량까지 더해도 최대 아래**일 때만 마신다
    if ((w.foodBits & kFoodHealthPotion) && w.useAmount > 0) {
        const int dhp = static_cast<int>(w.useAmount * ClassHealthMult(s.charClass));
        if (dhp + static_cast<int>(s.health) < static_cast<int>(s.maxHealth)) {
            UnitAdjustHP(Target{ s.addr, -1 }, dhp, out);
            // 10-g PlaySound 754 RestoreHealth (object_pickup.go:160)
            PlaySoundAt(754, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
            consumed = true;
        }
    }
    // ③ 마나 — 같은 꼴
    if ((w.foodBits & kFoodManaPotion) && w.useAmount > 0) {
        const int dmp = static_cast<int>(w.useAmount * ClassManaMult(s.charClass));
        if (dmp + static_cast<int>(s.mana) < static_cast<int>(s.maxMana)) {
            PlayerManaAdd(s, dmp);
            // 10-g PlaySound 755 RestoreMana (object_pickup.go:172)
            PlaySoundAt(755, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
            consumed = true;
        }
    }
    // ④ 독 해제 — 독이 있으면 **바로 풀고 끝난다**(정답지가 여기서 곧장 돌아간다)
    if ((w.foodBits & kFoodCurePoisonPotion) && s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        // 10-g PlaySound: CurePoison spell OnSound (object_pickup.go:178)
        PlaySoundAt(SpellSoundOn(14)   /* SPELL_CURE_POISON */, static_cast<float>(s.posX),
                    static_cast<float>(s.posY), 0, 0, s.addr);
        return 1;
    }
    return consumed ? 1 : 0;
}

// ── 묶음 5-b 물약 쓰기 ────────────────────────────────────────────────
//
// usePotion_53EF70(`object_use.go:115~180`) — 가방에서 물약을 쓸 때.
//   ★ 갈래는 **`SUBCLASS` 비트**가 정한다(`FoodClassBits`). 한 물약이 여러 갈래를 겸할 수 있고
//     정답지도 `if` 를 줄줄이 늘어놓아 **겹치면 다 적용한다**.
//   · 죽었으면 아무 일도 안 난다
//   · 체력 물약 — **지금 체력 < 최대**일 때만. 양 = `USE` 값 × **직업 배수**
//   · 마나 물약 — **지금 마나 < 최대**일 때만. 양 = `USE` 값 × **직업 배수**
//   · 독 해제 물약 — 독이 있을 때만 푼다
//   · 버프 물약 **여덟** — 빠르게(9) · 투명(0) · 불 보호(17) · 전기 보호(20) · 독 보호(18) ·
//     무적(23) · 어둠보기(21) · 흡혈(13). 전부 **120초 · 세기 3**
//   · 방패 물약 — **주문 51**(SPELL_SHIELD)을 `USE` 값을 세기로 걸음
//     ⚠ 26 은 **버프 번호**(ENCHANT_SHIELD)지 주문 번호가 아니다 — 둘을 헷갈리면 엉뚱한 주문이 나간다
//   · 하나라도 걸렸으면 물약이 없어진다. 아무것도 안 걸렸으면 **그대로 남는다**
//
// ⚠ 안 옮긴 것 — 소리(RestoreHealth·RestoreMana·CurePoison) 는 묶음 10.
bool Server::UsePotion(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    if (s.respawnAtFrame != 0) return false;                 // 죽은 사람은 못 쓴다
    const uint32_t fb = c.foodBits;
    bool consumed = false;

    // 체력 — 가득 차 있으면 **안 마신다**(정답지가 여기서 `consumed` 를 안 켠다)
    if ((fb & kFoodHealthPotion) && c.useAmount > 0 && s.health < s.maxHealth) {
        const int dhp = static_cast<int>(c.useAmount * ClassHealthMult(s.charClass));
        UnitAdjustHP(Target{ s.addr, -1 }, dhp, out);
        consumed = true;
    }
    // 마나 — 같은 꼴
    if ((fb & kFoodManaPotion) && c.useAmount > 0 && s.mana < s.maxMana) {
        const int dmp = static_cast<int>(c.useAmount * ClassManaMult(s.charClass));
        PlayerManaAdd(s, dmp);
        consumed = true;
    }
    // 독 해제 — **독이 있을 때만**
    if ((fb & kFoodCurePoisonPotion) && s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        consumed = true;
    }
    // 버프 여덟 — 전부 120초 · 세기 3(`object_use.go:147~163`)
    {
        static const struct { uint32_t bit; int buff; } kBuffs[] = {
            { kFoodHastePotion,           9 },               // ENCHANT_HASTED
            { kFoodInvisibilityPotion,    0 },               // ENCHANT_INVISIBLE
            { kFoodFireProtectPotion,    17 },               // ENCHANT_PROTECT_FROM_FIRE
            { kFoodShockProtectPotion,   20 },               // ENCHANT_PROTECT_FROM_ELECTRICITY
            { kFoodPoisonProtectPotion,  18 },               // ENCHANT_PROTECT_FROM_POISON
            { kFoodInvulnerabilityPotion,23 },               // ENCHANT_INVULNERABLE
            { kFoodInfravisionPotion,    21 },               // ENCHANT_INFRAVISION
            { kFoodVampirismPotion,      13 },               // ENCHANT_VAMPIRISM
        };
        for (const auto& b : kBuffs) {
            if (!(fb & b.bit)) continue;
            ApplyEnchant(Target{ s.addr, -1 }, b.buff, 120 * static_cast<int>(kServerTickRate), 3);
            consumed = true;
        }
    }
    // 방패 물약 — `USE` 값이 **세기**다
    if ((fb & kFoodShieldPotion) && c.useAmount > 0) {
        SpellArg arg;
        arg.obj = Target{ s.addr, -1 };
        arg.x = static_cast<float>(s.posX);
        arg.y = static_cast<float>(s.posY);
        SpellAccept(/*SPELL_SHIELD=*/51, s, s.addr, arg, c.useAmount, out);
        consumed = true;
    }
    return consumed;
}

// ── 묶음 4-f 소환 ──────────────────────────────────────────────────────
//
// SummonSize(`object_monster.go:652`) — thing.bin 몬스터 갈래로 정한다.
//   `SMALL_MONSTER` 1 · `MEDIUM_MONSTER` 2 · `LARGE_MONSTER` 4. 모르면 1.
int Server::SummonSizeOf(uint16_t type) const {
    const auto tb = typeBodies_.find(type);
    if (tb == typeBodies_.end()) return 1;
    const uint32_t sub = tb->second.subclass;
    if (sub & 0x4) return 4;                                 // LARGE_MONSTER
    if (sub & 0x2) return 2;                                 // MEDIUM_MONSTER
    return 1;                                                // SMALL_MONSTER(또는 모름)
}

// countControlledCreatures_500D10 — 그 사람이 거느린 것들의 **크기 합**.
int Server::CountControlledCreatures(const std::string& owner) const {
    int n = 0;
    for (const auto& k : pets_) {
        if (k.owner != owner) continue;
        if (k.health == 0) continue;
        n += SummonSizeOf(k.type);
    }
    return n;
}

// summonStart_500DA0(`GAME4.c:4060`) — 켤 때
//   · 시전자가 없거나 죽었으면, 또는 **글리프가 건 것**이면 취소
//   · **거느린 크기 합 + 이번 크기 > 4** 면 글(`Summon.c:CreatureControlFailed`)을 내고 취소
//   · 걸리면 **크기별 시전 시간**을 잡는다 — `SummonDuration`(아레나 60·210·375) 을
//     작은 것 [0] · 중간 [1] · 큰 것 [2] 로 쓴다
//   ⚠ 안 옮긴 것 — 퀘스트의 「길잡이(Guide)가 있어야 부른다」 검사 · 부르는 자리 고르기(sub_500F40)는 앞 자리로 갈음
int Server::DurSummonCreate(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (c->second.respawnAtFrame != 0) return 1;
    if (d.flag20) return 1;                                  // 글리프가 건 것은 안 된다
    const uint16_t type = SummonType(d.spell);
    if (type == 0) return 1;
    const int size = SummonSizeOf(type);
    if (CountControlledCreatures(d.caster) + size > kSummonLimit) {
        SendPriMsg(c->second, "Summon.c:CreatureControlFailed", out);
        return 1;
    }
    const int idx = (size >= 4) ? 2 : (size >= 2 ? 1 : 0);
    const uint32_t dur = static_cast<uint32_t>(BalanceFloatInd("SummonDuration", idx));
    d.frame68 = frame_ + dur;
    // ★ 10-01 — 자리·종류·방향·효과 번호를 **시작 때** 적고 모두에게 `7e`(`GAME4.c:4086~4126` → `sendSummonStartFX_5236F0` `GAME4_2.c:3911~3928`).
    //   ✖ 안 보내 소환 소용돌이가 없었다(10-01 사람 판 3) · 자리는 전처럼 「시전자 앞 kPetReach」(sub_500F40 갈음)인데 **끝이 아니라 시작 때** 정한다
    //   `7e | x i16 | y i16 | 효과번호 u16 | 종류 u16 | 방향 u8 | 시간 u16`(12B · Packet0 을 255 = 모두에게)
    Session& s = c->second;
    const double rad = s.facing * 3.14159265358979 / 128.0;  // 시전자 앞
    d.summonType = type;
    d.summonX = static_cast<float>(s.posX + kPetReach * std::cos(rad));
    d.summonY = static_cast<float>(s.posY + kPetReach * std::sin(rad));
    d.summonDir = s.facing;
    d.summonFxId = summonFxNext_++;
    if (summonFxNext_ >= 0xFDE8) summonFxNext_ = 0;
    const uint16_t fx = static_cast<uint16_t>(static_cast<long long>(d.summonX));   // (long long) 로 버린 뒤 아래 16비트(원본 `:3922~3925`)
    const uint16_t fy = static_cast<uint16_t>(static_cast<long long>(d.summonY));
    const uint8_t msg[12] = { static_cast<uint8_t>(Op::FX_SUMMON),
                              static_cast<uint8_t>(fx), static_cast<uint8_t>(fx >> 8),
                              static_cast<uint8_t>(fy), static_cast<uint8_t>(fy >> 8),
                              static_cast<uint8_t>(d.summonFxId), static_cast<uint8_t>(d.summonFxId >> 8),
                              static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8),
                              d.summonDir,
                              static_cast<uint8_t>(dur), static_cast<uint8_t>(dur >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    d.summonFxSent = true;
    ++summonFxSent_[0];
    return 0;
}

// ★ 10-01 `summonCancel_5011C0`(`GAME4.c:4257~4262`) — 파괴 때(끝났든 끊겼든 불린다) **아직 안 세웠으면** 모두에게 `7f`(효과 번호 · 3B · Packet0)
//   + 그 자리에 소리 900 `SummonAbort`(`audCreate_501A30` — 자리 소리). 클라는 그 번호의 소용돌이를 지운다(`cdecode.c:1601~1605`)
//   ⚠ 원본은 만들기가 실패해도(시작 검사에서 끝남) 이 길을 타며 그때 +83·+74 는 안 채워진 값이다 → 짓지 않음: `7e` 를 보낸 것만
//     (첫 효과 번호가 0 이라 `7f 00 00` 을 막 보내면 남의 소용돌이를 지울 수 있다)
void Server::DurSummonEnd(DurSpell& d, std::vector<OutPacket>& out) {
    if (d.summonDone || !d.summonFxSent) return;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::FX_SUMMON_CANCEL),
                             static_cast<uint8_t>(d.summonFxId), static_cast<uint8_t>(d.summonFxId >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    ++summonFxSent_[1];
    PlaySoundAt(900, d.summonX, d.summonY);                  // SummonAbort(`sound.go:937` − 37)
}

// summonFinish_5010D0(`GAME4.c:4230`) — 그 프레임이 되면 **한 마리** 세우고 끝난다.
//   ★ 한도를 **여기서 다시 본다** — 부르는 동안 다른 것을 부르면 넘칠 수 있다.
int Server::DurSummonTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (c->second.respawnAtFrame != 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;                   // 아직 부르는 중
    const uint16_t type = SummonType(d.spell);
    if (type == 0) return 1;
    const int size = SummonSizeOf(type);
    if (CountControlledCreatures(d.caster) + size > kSummonLimit) {
        SendPriMsg(c->second, "Summon.c:CreatureControlFailed", out);
        return 1;
    }
    Session& s = c->second;
    // ★ 10-01 — **시작 때 적은 자리·방향**에 세운다(`GAME4.c:4246~4247` `unitDoSummonAt_5016C0(+72, +74, 시전자, +82)`).
    //   전에는 이 틱의 시전자 앞에 세워, 부르는 동안 움직이면 소용돌이(`7e`) 자리와 어긋났다
    SpawnCreature(type, d.summonX, d.summonY, d.caster);
    if (!pets_.empty()) pets_.back().facing = d.summonDir;
    if (!pets_.empty()) {
        ReportAcquireCreature(s, pets_.back(), out);         // ★ 10-01 — unitDoSummonAt_5016C0(`spell_summon.go:54`)
        PlaySoundAt(899, static_cast<float>(pets_.back().x), static_cast<float>(pets_.back().y), 0, 0, std::string(),
                    Target{ std::string(), static_cast<int>(pets_.size() - 1) });   // SummonComplete(`sound.go:936` − 37) · `aud_501960(899, 그것)`
    }
    d.summonDone = true;                                     // +85 = 1 → 파괴(DurSummonEnd)가 `7f` 를 안 낸다
    return 1;
}

// ★ 10-01 `nox_xxx_netReportAcquireCreature_4D91A0`(`GAME3_2.c:4602~4614`) — 주인에게 `6c`(번호2·종류2) + `netReportTotalHealth_4D85C0`
//   `dd`(번호2·체력2·최대2) · 둘 다 Packet1. 클라는 `6c` 로 **제 소환수 목록·체력 막대**를 세운다(`cdecode.c:1507~1527`).
//   ✖ 안 보내서 소환수 머리 위 체력·정보가 없었다(10-01 사람 판 3 — 비홀더·곰·임프).
//   ⚠ 안 옮김 — 종류 윗비트(GameFlag28 0x8000000 이면 0x80 · 뜻 미상 → 0) · 미니맵 장부 `netMarkMinimapObject_417190` ·
//     `SendSimpleObject2_4DF360` · 팀 알림(`createAtImpl_4191D0`)
void Server::ReportAcquireCreature(Session& owner, const Creature& k, std::vector<OutPacket>& out) {
    const uint8_t acq[5] = { static_cast<uint8_t>(Op::REPORT_ACQUIRE_CREATURE),
                             static_cast<uint8_t>(k.netCode), static_cast<uint8_t>(k.netCode >> 8),
                             static_cast<uint8_t>(k.type), static_cast<uint8_t>(k.type >> 8) };
    EmitSeq(owner, acq, sizeof(acq), out);
    ++summonOwnSent_[0];
    const int maxHp = TuningFor(k.type).health;
    const uint8_t dd[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH),
                            static_cast<uint8_t>(k.netCode), static_cast<uint8_t>(k.netCode >> 8),
                            static_cast<uint8_t>(k.health), static_cast<uint8_t>(k.health >> 8),
                            static_cast<uint8_t>(maxHp), static_cast<uint8_t>(maxHp >> 8) };
    EmitSeq(owner, dd, sizeof(dd), out);
    ++summonOwnSent_[1];
}

// ── 묶음 4-c1 밀기·당기기·땅흔들기 ─────────────────────────────────────
//
// mapPushUnitsAround_52E040(`GAME4_2.c:10477`) + unitPushAroundFn_52E0E0(`:10507`)
//   가운데에서 `outer` 안에 있는 **움직일 수 있는** 것마다 힘을 준다. 힘은 **×10** 해서 넣는다(정답지 `a3a[3] = a4 * 10.0`).
//   · 가운데→그것 **선 추적(0)** 이 막히면 안 민다(벽 너머로는 안 밀린다)
//   · 거리 d = √(dx²+dy²) **+ 0.1**(0 나눗셈 막기) · d > outer 면 건너뜀
//   · d ≤ inner 면 힘 그대로 · 그 밖은 `(1 − (d−inner)/(outer−inner))` 로 줄어든다
//   · 실제로 더하는 것은 **힘 ÷ 질량** 을 방향으로 나눈 것 — ForceVec 에 **더한다**(덮어쓰지 않는다)
//   ⚠ outer 가 inner 보다 작으면 inner 를 outer 로 쓴다(정답지 a3a[1] 고르기)
void Server::PushUnitsAround(float cx, float cy, float outer, float inner, float power) {
    if (outer < inner) outer = inner;
    const float force = power * 10.0f;
    auto apply = [&](float ox, float oy, float mass, double& fx, double& fy) {
        if (!MapTraceRay(cx, cy, ox, oy, 0x0)) return;       // 벽 너머로는 안 민다
        const float dx = ox - cx, dy = oy - cy;
        const float d = std::sqrt(dx * dx + dy * dy) + 0.1f;
        if (d > outer) return;
        const float f = (d > inner && outer > inner)
                      ? (1.0f - (d - inner) / (outer - inner)) * force
                      : force;
        const float per = f / (mass > 0.0f ? mass : 1.0f);
        fx += static_cast<double>(per * dx / d);             // ForceVec 에 **더한다**
        fy += static_cast<double>(per * dy / d);
    };
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;    // 죽은 것은 안 밀린다(isObjectMovable 깃발 갈래)
        apply(static_cast<float>(o.posX), static_cast<float>(o.posY), static_cast<float>(PlayerMassOf(o)), o.forceX, o.forceY);
    }
    for (auto& k : pets_) {
        if (k.dead) continue;                                // ★ 10-01 시체 — `isObjectMovable_52E020` +16 & 0x8068(DEAD 포함 · `GAME4_2.c:10468`)
        const auto tb = typeBodies_.find(k.type);
        const float mass = (tb != typeBodies_.end()) ? tb->second.mass : 1.0f;
        double fx = 0, fy = 0;
        apply(static_cast<float>(k.x), static_cast<float>(k.y), mass, fx, fy);
        // ⚠ 소환수는 아직 ForceVec 적분이 없다(걸음을 AI 가 직접 옮긴다) — 힘을 자리로 바로 옮겨 둔다
        k.x += fx;
        k.y += fy;
    }
}

// castPull_52BFA0(`GAME4_2.c:9337`) · castPush_52C000(`:9350`) — 시전자 자리에서 둘레 600·안쪽 10 로 민다(당기기는 **음수**)
int Server::CastPushPull(const Session& caster, int lvl, bool pull) {
    const double coeff = BalanceFloat(pull ? "PullPowerCoeff" : "PushPowerCoeff");
    const float power = static_cast<float>(pull ? -(coeff * lvl) : (coeff * lvl));
    PushUnitsAround(static_cast<float>(caster.posX), static_cast<float>(caster.posY), 600.0f, 10.0f, power);
    return 1;                                                // 소리는 10
}

// earthquakeSend_4D9110(`GAME3_2.c:4579`) — 가운데에서 **300 안**(거리² < 90000)에 있는 사람마다
//   `97 <세기>` 2바이트를 Kind1(틱 목록)로. 세기는 `(1 − 거리²/90000) × jiggle` 로 멀수록 약해진다
// ★ 09-20 묶음 10 — 높이 알림(`5e REPORT_Z_PLUS` · `5f REPORT_Z_MINUS`).
//   ⚠ 이걸 안 보내서 **주먹이 바닥에 붙어 나타났다**(⑰ · 09-20 캡처로 확인).
//   정답지는 더티 비트 `0x400000` 으로 «바뀌었을 때» 보낸다(`GAME4_2.c:7166`). 우리는 떨어지는 동안
//   **값이 바뀐 틱에만** 보낸다 — 효과는 같고 메시지 수도 같다(정수로 바뀔 때만).
// ★ 09-20 묶음 10 — **낙하 시작 알림**(`9f FX_DELTAZ_SPELL_START`, 6바이트).
//   짜임은 정답지 `netReportUnitHeight_4D9020` 의 **6바이트 갈래**(`GAME3_2.c:4536~4548`):
//     `9f | 번호 u16 | 높이(+104) u8 | z속도(+108) i8 | 되튐(+116) u8`
//   ⭕ 세 칸이 다 **우리가 아는 값**이다 — 높이 255 · z속도 −FistSpeed · 되튐 9.0(`Server.cpp` 낙하 논리).
//   클라는 이 값을 그림의 +432 묶음에 넣고 **스스로 떨어뜨려 그린다**(`cdecode.c:1957~1977`).
//   ⚠ 높이만 보내는 `5e/5f` 는 사람·몬스터용 칸(ZVal)이라 **주문 물체에는 안 먹는다**(09-20 실측:
//     90건을 보내고 클라가 그림도 찾았는데 화면은 그대로였다)
void Server::SendDeltaZStart(uint16_t netCode, float z, float zVel, float bounce,
                             std::vector<OutPacket>& out) {
    if (netCode == 0) return;
    auto clampU8 = [](float v) -> uint8_t {
        if (v < 0.0f) v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        return static_cast<uint8_t>(v);
    };
    auto clampI8 = [](float v) -> uint8_t {
        if (v < -128.0f) v = -128.0f;
        if (v > 127.0f) v = 127.0f;
        return static_cast<uint8_t>(static_cast<int8_t>(v));
    };
    const uint8_t msg[6] = {
        static_cast<uint8_t>(Op::FX_DELTAZ_SPELL_START),
        static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8),
        clampU8(z), clampI8(zVel), clampU8(bounce),
    };
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        EmitSeq(kv.second, msg, sizeof(msg), out);
    }
    ++unitHeightSent_;
}
// ★ 09-20 ⑰ — 만든 틱에 보내면 **클라가 버린다**(아직 그림이 없다). 다음 틱 머리에서 낸다.
void Server::QueueDeltaZStart(uint16_t netCode, float z, float zVel, float bounce) {
    if (netCode == 0) return;
    pendingDeltaZ_.push_back(PendingZ{ netCode, z, zVel, bounce });
}
void Server::FlushDeltaZStart(std::vector<OutPacket>& out) {
    if (pendingDeltaZ_.empty()) return;
    std::vector<PendingZ> q;
    q.swap(pendingDeltaZ_);
    for (const auto& p : q) SendDeltaZStart(p.netCode, p.z, p.zVel, p.bounce, out);
}
void Server::SendUnitHeight(uint16_t netCode, float z, std::vector<OutPacket>& out) {
    if (netCode == 0) return;
    float az = (z < 0.0f) ? -z : z;
    if (az > 255.0f) az = 255.0f;
    const uint8_t msg[4] = {
        static_cast<uint8_t>(z < 0.0f ? Op::REPORT_Z_MINUS : Op::REPORT_Z_PLUS),
        static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8),
        static_cast<uint8_t>(az),
    };
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        EmitSeq(kv.second, msg, sizeof(msg), out);
    }
    ++unitHeightSent_;
}
void Server::SendJiggle(float cx, float cy, int jiggle, std::vector<OutPacket>& out) {
    ++fxSent_;                                             // 조각 D — FX 는 건수로 센다
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const float dx = cx - static_cast<float>(o.posX), dy = cy - static_cast<float>(o.posY);
        const double d2 = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
        if (d2 >= 90000.0) continue;
        const int v = static_cast<int>((1.0 - d2 / 90000.0) * static_cast<double>(jiggle));
        const uint8_t msg[2] = { static_cast<uint8_t>(Op::FX_JIGGLE), static_cast<uint8_t>(v) };
        Emit(o, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
}

// castEquake_52DE40(`GAME4_2.c:10418`) + equakeDamage_52DEC0(`:10437`) — 둘레 안에 **거리에 반비례하는** 피해(종류 11) + 화면 흔들기
//   피해 = EarthquakeDamage[세기−1] × (1 − 거리/EarthquakeRange) · 자기 자신·공중(0x4000)·체력 0 은 뺀다
int Server::CastEarthquake(Session& caster, int lvl, std::vector<OutPacket>& out) {
    const float range = static_cast<float>(BalanceFloat("EarthquakeRange"));
    const double base = BalanceFloatInd("EarthquakeDamage", lvl - 1);
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    auto hurtAt = [&](const Target& t, float ox, float oy) {
        const float dx = ox - cx, dy = oy - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > range) return;
        const double dmg = base * (1.0 - static_cast<double>(dist) / static_cast<double>(range));
        if (dmg <= 0.0) return;
        AttackSrc src;
        src.x = cx; src.y = cy;
        HurtTarget(t, static_cast<int>(dmg), out, caster.addr, 11, &src, HitterByAddr(caster.addr));   // 피해 종류 11 · ★ 4-c3b1 a3 = 시전자(`GAME4_2.c:10457`)
    };
    // ★ 09-23 nine-defects 3-b — 정답지 차례 `:10448~10452`: 자기 아님 → **떠 있음(0x4000) 아님** → 체력 있음.
    //   전에는 주석에만 「공중은 뺀다」가 있고 코드가 없었다(사람은 떠 있음 칸이 없었고, 소환수는 종류 깃발을 안 봤다)
    //   (계수기는 둘레 안의 것만 센다 — 정답지는 `unitsGetInCircle` 로 추린 것에만 이 검사를 한다)
    auto inRange = [&](double ox, double oy) {
        const float dx = static_cast<float>(ox) - cx, dy = static_cast<float>(oy) - cy;
        return std::sqrt(dx * dx + dy * dy) <= range;
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame || kv.first == caster.addr) continue;         // 자기 자신은 뺀다
        if (PlayerObjFlags(kv.second) & 0x4000) {                           // 뛰는 중
            if (inRange(kv.second.posX, kv.second.posY)) ++equakeSkipAirborne_;
            continue;
        }
        if (kv.second.respawnAtFrame != 0 || kv.second.health == 0) continue;
        who.push_back(kv.first);
    }
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        hurtAt(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        // 날아다니는 종류(thing.bin FLAGS AIRBORNE — 박쥐·임프 따위). 몸 모르는 종류는 깃발도 모른다 → 안 뺀다
        const auto tb = typeBodies_.find(pets_[i].type);
        if (tb != typeBodies_.end() && (tb->second.flags & 0x4000)) {
            if (inRange(pets_[i].x, pets_[i].y)) ++equakeSkipAirborne_;
            continue;
        }
        if (pets_[i].health == 0) continue;
        hurtAt(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y));
    }
    const int jiggle = static_cast<int>(BalanceFloatInd("EarthquakeJiggle", lvl - 1));
    SendJiggle(cx, cy, jiggle, out);                         // 화면 흔들기 — 소리는 10
    return 1;
}

// ── 묶음 4-c2a 독 ────────────────────────────────────────────────────
//
// 자료 — +540 남은 양 · 체력자료 +16 시작 프레임 · +542 감쇠 계수기
uint8_t* Server::PoisonOf(const Target& t) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? &it->second.poison : nullptr;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return nullptr;
    return &pets_[static_cast<size_t>(t.pet)].poison;
}

// netReportObjectPoison_4D7F40(`GAME3_2.c:3771`) — `da <번호2> <켜짐>` 4바이트 Packet0.
//   사람은 이 메시지가 아니라 **상태 비트 1024**(`6a`)로 알린다 — 소환수는 주인에게 이 메시지로.
void Server::SetPoisonAmount(const Target& t, int amount, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return;
    const bool wasZero = (*pp == 0);
    if (wasZero && amount > 0) {                            // 0 에서 켜질 때 시작 프레임을 적는다
        if (!t.addr.empty()) sessions_.at(t.addr).poisonStart = frame_;
        else pets_[static_cast<size_t>(t.pet)].poisonStart = frame_;
    }
    *pp = static_cast<uint8_t>(amount);
    if (!t.addr.empty()) {
        Session& s = sessions_.at(t.addr);
        // 상태 비트 1024 — 켜고 끔(관전 1 과 함께 싣는다)
        const uint32_t bits = (s.observer ? 1u : 0u) | (amount != 0 ? 1024u : 0u);
        BroadcastClientStatus(s.netCode, bits, out);
        s.poisonDecay = (amount != 0) ? 1000 : 0;
    } else {
        Creature& k = pets_[static_cast<size_t>(t.pet)];
        auto oi = sessions_.find(k.owner);                  // 주인에게만 알린다(+508)
        if (oi != sessions_.end() && oi->second.inGame) {
            const uint8_t msg[4] = { static_cast<uint8_t>(Op::REPORT_OBJECT_POISON),
                                     static_cast<uint8_t>(k.netCode), static_cast<uint8_t>(k.netCode >> 8),
                                     static_cast<uint8_t>(amount != 0 ? 1 : 0) };
            EmitImportantRaw(oi->second, msg, sizeof(msg), out);
        }
        k.poisonDecay = (amount != 0) ? 1000 : 0;
    }
}

// activatePoison_4EE7E0(`GAME3_3.c:7270`) — 독을 얹는다. 얹었으면 1
//   거절: 대상 없음 · **무적(23)** · 관전 중인 사람 · 몬스터 특정 갈래(+12 & 0x200)
//   ⚠ 저항 굴림(getPoisonDmg_4E0040 × 100)은 **입은 것에서 나온다** — 아이템 묶음. 지금은 저항 0 으로 둔다(지어내지 않는다)
//   새 양 = 지금 + add 이고 cap 을 넘지 않는다 · 지금이 이미 cap 을 넘으면 그대로 둔다
bool Server::ActivatePoison(const Target& t, int add, int cap, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return false;
    const Buffs* b = BuffsOf(t);
    if (b && b->Has(23)) return false;                      // 무적이면 안 걸린다
    if (!t.addr.empty()) {
        const Session& s = sessions_.at(t.addr);
        if (s.observer || s.respawnAtFrame != 0) return false;
    }
    const int cur = static_cast<int>(*pp);
    int next = cur;
    if (cur + add <= cap)      next = cur + add;
    else if (cur <= cap)       next = cap;
    if (next != cur) SetPoisonAmount(t, next, out);          // 소리 100 은 10
    return true;
}

// updatePoison_4EE8F0(`GAME3_3.c:7333`) — 양을 깎고, 0 이 되면 끈다(사람은 글 "PoisonFade")
void Server::UpdatePoison(const Target& t, int amount, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return;
    if (static_cast<int>(*pp) > amount) { *pp = static_cast<uint8_t>(*pp - amount); return; }
    SetPoisonAmount(t, 0, out);
    if (!t.addr.empty()) {
        sessions_.at(t.addr).poisonStart = 0;
        SendPriMsg(sessions_.at(t.addr), "Health.c:PoisonFade", out);
    } else {
        pets_[static_cast<size_t>(t.pet)].poisonStart = 0;
    }
}

// removePoison_4EE9D0(`GAME3_3.c:7375`) — 통째로 없앤다(해독 주문·물약)
void Server::RemovePoison(const Target& t, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp || *pp == 0) return;
    SetPoisonAmount(t, 0, out);
    if (!t.addr.empty()) sessions_.at(t.addr).poisonStart = 0;
    else pets_[static_cast<size_t>(t.pet)].poisonStart = 0;
}

// 독 틱(`server.go:175~201`) — 감쇠와 피해가 한자리에 있다
//   ① 감쇠 — +542 를 매 틱 줄이고 0 이 되면 **독 1 감소** 뒤 다시 1000(≈33초에 1)
//   ② 피해 — 시작 뒤 **60틱 유예**를 지나면, 독이 8 을 넘으면 매 틱 · 아니면 `128 >> (양−1)` 틱마다 1(종류 5)
void Server::PoisonTick(std::vector<OutPacket>& out) {
    std::vector<Target> list;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.poison) list.push_back(Target{ kv.first, -1 });
    for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].poison) list.push_back(Target{ std::string(), static_cast<int>(i) });
    for (const Target& t : list) {
        uint8_t* pp = PoisonOf(t);
        if (!pp || *pp == 0) continue;
        uint16_t* decay = nullptr;
        uint32_t start = 0;
        bool dead = false;
        if (!t.addr.empty()) {
            Session& s = sessions_.at(t.addr);
            decay = &s.poisonDecay; start = s.poisonStart; dead = (s.respawnAtFrame != 0);
        } else {
            Creature& k = pets_[static_cast<size_t>(t.pet)];
            decay = &k.poisonDecay; start = k.poisonStart;
            dead = k.dead;                                   // ★ 10-01 시체 — 감쇠의 독 갱신은 `!FlagDead`(`server.go:182`) · 피해는 아래 체력 0 이 거른다(`:191`)
        }
        if (decay && *decay > 0) {                          // ① 감쇠
            if (--*decay == 0) {
                if (!dead) UpdatePoison(t, 1, out);
                if (PoisonOf(t) && *PoisonOf(t)) *decay = 1000;
            }
        }
        uint8_t* pp2 = PoisonOf(t);
        if (!pp2 || *pp2 == 0) continue;
        const int amt = static_cast<int>(*pp2);
        if (TargetHealth(t) <= 0) continue;
        if (start != 0 && (frame_ - start) <= 60) continue;  // ② 시작 뒤 60틱은 안 아프다
        const bool hit = (amt > 8) || (frame_ % static_cast<uint32_t>(128u >> (amt - 1)) == 0);
        if (!hit) continue;
        HurtTarget(t, 1, out, std::string(), 5, nullptr, HitterNone());    // 피해 1 · 종류 5(독) · ⚠ 퀘스트는 2 · ★ 4-c3b1 nil·nil(Go `server.go:198`) → 소리 없음
    }
}

// castPoison_52C720(`GAME4_2.c:9567`) — 대상 없으면 0 · 독을 세기만큼(상한도 세기) 얹고 도운 사람 기록
int Server::CastPoison(const std::string& obj3, const Target& targ, int lvl, std::vector<OutPacket>& out) {
    if (!PoisonOf(targ)) return 0;
    ActivatePoison(targ, lvl, lvl, out);
    MarkAssist(obj3, targ);                                  // sub_4E7540
    return 1;
}

// ── 묶음 4-c3 — 주문이 놓는 오브젝트 ─────────────────────────────────
//
// 정답지는 `newObjectWithTypeInd(이름번호)` 로 만들고 `createAt_4DAA50` 로 자리에 놓은 뒤
// 수명을 건다(불꽃은 unitSetDecayTime, 독구름은 갱신자료 첫 칸). 우리는 한 목록(placed_)에 담는다.
Server::PlacedObj* Server::PlaceObject(uint16_t type, float x, float y, const std::string& owner, uint32_t life, uint8_t kind,
                                       uint8_t dmgAmount, uint8_t dmgType, float radius) {
    if (type == 0 || life == 0) return nullptr;
    PlacedObj o;
    o.netCode = nextShotNetCode_++;
    o.type = type;
    o.x = x; o.y = y;
    o.owner = owner;
    o.life = life;
    o.nextHitFrame = frame_;                                 // 놓자마자 한 번 판정한다(+136 초깃값 0)
    o.kind = kind;
    o.dmgAmount = dmgAmount;
    o.dmgType = dmgType;
    o.radius = radius;
    o.createdFrame = frame_;
    placed_.push_back(o);
    return &placed_.back();
}

void Server::DestroyPlaced(size_t i, std::vector<OutPacket>& out) {
    if (i >= placed_.size()) return;
    const uint16_t nc = placed_[i].netCode;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    placed_.erase(placed_.begin() + static_cast<long>(i));
}

// updateToxicCloud_53D850(`GAME4_3.c:6855`) + sub_53D8C0(`:6872`)
//   · 판정 프레임(+136)이 지났으면 둘레 **75** 안을 훑어 사람·몬스터에게 **3~10 피해(종류 5)** ·
//     놓은 이의 적이면 **독 1(상한 1)** 도 얹는다 · 벽 뒤(선 추적 5)는 건너뛴다
//   · 다음 판정은 **5~10틱 뒤** 무작위 · 수명은 매 틱 하나씩 줄고 0 이면 지운다
void Server::UpdatePlaced(std::vector<OutPacket>& out) {
    for (size_t i = 0; i < placed_.size();) {
        PlacedObj& o = placed_[i];
        if (o.kind == 1 && frame_ >= o.nextHitFrame) {
            const float r2 = 75.0f * 75.0f;
            auto hit = [&](const Target& t, float tx, float ty) {
                const float dx = tx - o.x, dy = ty - o.y;
                if (dx * dx + dy * dy > r2) return;
                if (!MapTraceRay(tx, ty, o.x, o.y, 0x5)) return;     // 벽 뒤는 안 닿는다
                const int dmg = 3 + static_cast<int>(NextRandom(8));  // randomInt(3,10)
                HurtTarget(t, dmg, out, o.owner, 5, nullptr, HitterObj(o.type));   // 피해 종류 5(독) · ★ 4-c3b2 a3 = 구름(`GAME4_3.c:6892`)
                bool enemy = false;                                   // 놓은 이의 적이면 독도
                auto oi = sessions_.find(o.owner);
                if (oi != sessions_.end()) enemy = TargetIsEnemy(oi->second, t);
                if (enemy) ActivatePoison(t, 1, 1, out);
            };
            std::vector<std::string> who;
            for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
            for (const auto& addr : who) {
                auto it = sessions_.find(addr);
                if (it == sessions_.end()) continue;
                hit(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
            }
            for (size_t k = 0; k < pets_.size(); ++k)
                hit(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
            o.nextHitFrame = frame_ + 5 + NextRandom(6);              // randomInt(5,10)
        }
        // ── kind 2 — 닿음 피해(collideDamage_4E9430) ──────────────────
        //   몸이 겹친 틱마다 부른다. 피해 = **양 >> 1** · 단 양이 1 이면 홀수 프레임에만 1(짝수 틱은 0).
        //   ⚠ 갈래·팀을 안 가린다 — 불꽃 FLAGS 에 NO_COLLIDE_OWNER 가 없어 **시전자·같은 편도 탄다**
        //     (sub_548360 `v11 & 0x400 && unitsHaveSameTeam` 가지를 안 탄다).
        if (o.kind == 2) PlacedCollide(o, out);
        // ── kind 6 — 바깥으로 나가는 불꽃(updateFlameCleanse_53D510) ──────────
        //   ① 수명(+136)이 지났거나 ② **시작점에서 지금 자리까지 선 추적(65)이 막혔거나**
        //   ③ 만든 지 3틱이 넘었는데 **한 틱 동안 안 움직였으면** 사라진다.
        //   움직임은 감쇠 없이 속도를 그대로 더한다(+112 = 0).
        if (o.kind == 6) {
            const float px = o.x, py = o.y;
            o.x += o.velX;
            o.y += o.velY;
            const bool stuck = (frame_ - o.createdFrame > 3u) && (px == o.x) && (py == o.y);
            if (frame_ >= o.expireFrame || !MapTraceRay(o.originX, o.originY, o.x, o.y, 65) || stuck) {
                DestroyPlaced(i, out);
                continue;
            }
            PlacedCollide(o, out);
        }
        // ── kind 3 — 떨어지는 운석(MeteorUpdate = meteorExplode_53D6E0) ──────
        //   하는 일이 하나다: **높이(+104)가 0 이하면** 터뜨리고 지운다.
        //   높이는 시전 때 255 로 띄우고(unitRaise) z 속도를 −MeteorSpeed(8)로 준다.
        if (o.kind == 3) {
            o.z += o.zVel;
            if (o.z <= 0.0f) { MeteorExplode(o, out); DestroyPlaced(i, out); continue; }
        }
        // ── kind 5 — 떨어지는 주먹(FistUpdate 53D400 + FistCollide 4EADF0) ───
        //   높이 물리는 `updateFallLogic_51B870` 의 **0x800000 갈래**를 그대로 쓴다:
        //     높이 += z속도 · 높이 ≥ 0 이면 z속도 −= 0.5(중력) ·
        //     아니면 높이 = 0 · z속도 = −z속도 × (되튐 9.0) × 0.1 · 그래도 2 미만이면 멈춘다
        //   처음 땅에 닿을 때 **화면 흔들기 30** 을 보내고 「닿았음」을 적는다(+16 비트 31).
        //   닿음 피해는 **닿은 뒤에만** — 몸이 겹친 틱마다 `FistOfVengeanceDamage`(50·100·200·300·400) 종류 **2(짓눌림)**.
        //   수명은 3초(90틱).
        if (o.kind == 5) {
            o.z += o.zVel;
            if (o.z >= 0.0f) {
                o.zVel -= 0.5f;
            } else {
                o.z = 0.0f;
                o.zVel = -o.zVel * 9.0f * 0.1f;              // +116 = 9.0(되튐)
                if (o.zVel < 2.0f) { o.z = 0.0f; o.zVel = 0.0f; }
            }
            if (o.z <= 0.0f && !o.landed) {
                o.landed = true;
                SendJiggle(o.x, o.y, 30, out);               // earthquakeSend_4D9110(자리, 30)
            }
            if (o.z <= 0.0f && o.damage > 0) {
                const float reach = o.radius + static_cast<float>(kBodyRadius);
                const float rr = reach * reach;
                auto touch = [&](const Target& t, float tx, float ty) {
                    const float dx = tx - o.x, dy = ty - o.y;
                    if (dx * dx + dy * dy > rr) return;
                    HurtTarget(t, o.damage, out, o.owner, 2, nullptr, HitterObj(o.type));   // 종류 2 = 짓눌림 · ★ 4-c3b2 a3 = 주먹(`GAME3_3.c:4922`)
                };
                std::vector<std::string> who;
                for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
                for (const auto& addr : who) {
                    auto it = sessions_.find(addr);
                    if (it == sessions_.end()) continue;
                    touch(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
                }
                for (size_t k = 0; k < pets_.size(); ++k)
                    touch(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
            }
            if (frame_ - o.createdFrame > 3u * static_cast<uint32_t>(kServerTickRate)) { DestroyPlaced(i, out); continue; }
        }
        // ── kind 7 — 거미 낳는 자리(updateArachnaphobia_53DA60) ──────────────
        //   판정 프레임(+136)이 지나면 **SmallSpider 한 마리**를 제자리에 낳고 다음은 **1~5틱 뒤**.
        //   만든 지 **3초(90틱)** 가 넘으면 사라진다. ⚠ 낳은 뒤에도 검사하므로 마지막 틱에도 한 마리 더 나온다.
        if (o.kind == 7) {
            if (frame_ > o.nextHitFrame) {
                const float sx = o.x, sy = o.y;
                const std::string owner = o.owner;
                SpawnCreature(NamedType("SmallSpider"), sx, sy, owner);
                placed_[i].nextHitFrame = frame_ + 1 + NextRandom(5);   // randomInt(1,5)
            }
            if (frame_ - placed_[i].createdFrame > 3u * static_cast<uint32_t>(kServerTickRate)) {
                DestroyPlaced(i, out);
                continue;
            }
        }
        // ── kind 4 — 유성우 뿌리개(updateMeteorShower_53D5A0) ────────────────
        //   만든 뒤 **5초(150틱)** 동안 **4~8틱마다** 운석 하나를 무작위 자리에 떨군다.
        //   자리 = 방향 무작위(0~255) · 거리 = `무작위실수(4,12)`의 **제곱**(16~144).
        //   그 자리까지 선 추적(traceRay 5)이 막히면 그 번만 거른다. 5초가 지나면 스스로 지워진다.
        if (o.kind == 4) {
            if (frame_ - o.createdFrame >= 5u * static_cast<uint32_t>(kServerTickRate)) { DestroyPlaced(i, out); continue; }
            if (frame_ >= o.nextHitFrame) {
                // ⚠ 아래에서 `placed_` 에 새로 밀어 넣는다 — 벡터가 옮겨 앉으면 `o` 가 죽는다.
                //   그래서 쓸 값을 **먼저 복사**하고, 되쓸 때는 자리(i)로 다시 잡는다.
                const float ox = o.x, oy = o.y;
                const std::string owner = o.owner;
                const int dmg = o.damage;
                const int dir = static_cast<int>(NextRandom(256));
                const float r = 4.0f + NextRandomFloat() * 8.0f;         // randomFloat(4,12)
                const float d = r * r;                                   // ★ **제곱**이 거리다(16~144)
                const float mx = ox + d * DirVecX(dir);
                const float my = oy + d * DirVecY(dir);
                if (MapTraceRay(ox, oy, mx, my, 0x5)) {
                    if (PlacedObj* mo = PlaceObject(NamedType("Meteor"), mx, my, owner, kPlacedNoLife, /*kind=*/3)) {
                        mo->z = 255.0f;                                  // unitRaise_4E46F0(255)
                        mo->zVel = -static_cast<float>(BalanceFloat("MeteorSpeed"));
                        QueueDeltaZStart(mo->netCode, mo->z, mo->zVel, 0.0f);   // 09-20 ⑰ — 운석도 같은 알림(되튐 없음)
                        mo->damage = dmg;                                // 뿌리개가 든 값을 그대로
                    }
                }
                placed_[i].nextHitFrame = frame_ + 4 + NextRandom(5);    // randomInt(4,8)
            }
        }
        // ⚠ `o` 는 위 갈래에서 `placed_` 가 커졌으면 이미 죽은 참조다 — 자리(i)로 다시 잡는다
        if (placed_[i].life > 0 && placed_[i].life != kPlacedNoLife) --placed_[i].life;
        if (placed_[i].life == 0) { DestroyPlaced(i, out); continue; }
        ++i;
    }
}

// castToxicCloud_52DB60(`GAME4_2.c:10279`) — 시전자→커서 **선 추적**이 통하면 그 자리에 독구름을 놓는다
//   막히면 글(`a9` 코드 0 · 값 2 「거기엔 못 놓는다」)을 내고 **0**(받아들이지 않음)
//   수명 = ToxicCloudLifetime × 초당 틱
int Server::CastToxicCloud(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);                      // netInformTextMsg(0, 2)
        return 0;
    }
    const uint16_t type = NamedType("ToxicCloud");
    const uint32_t life = static_cast<uint32_t>(BalanceFloat("ToxicCloudLifetime") * static_cast<double>(kServerTickRate));
    PlaceObject(type, arg.x, arg.y, caster.addr, life, /*kind=*/1);
    return 1;                                                // 소리는 10
}

// castBurn_52C3E0(`GAME4_2.c:9471`) — 시전자→커서 **선 추적**이 통하면 그 자리에 불꽃(MediumFlame)을 놓는다
//   막히면 글(코드 0 · 값 2)을 내고 **0** · 통하면 불꽃 + 불똥 터짐(반지름 64) + 소리
//   ★ 수명 = `BurnDuration`(= 300) **그대로가 틱이다** — 독구름과 달리 초당 틱을 **안 곱한다**
//     (`unitSetDecayTime_511660` 은 +136 에 `지금 프레임 + a2` 를 넣는다. 다른 부르는 자리는 전부 `gameFPS() × N` 인데
//      불꽃만 `float2int(BurnDuration)` 을 날로 넘긴다).
//   ⚠ 아직 안 옮긴 갈래 — 대상이 **글리프**면 선 추적 없이 글리프 자리에 놓는다. 글리프가 아직 없다(묶음 5).
int Server::CastBurn(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);                      // netInformTextMsg(0, 2)
        return 0;
    }
    const uint16_t type = NamedType("MediumFlame");
    const uint32_t life = static_cast<uint32_t>(BalanceFloat("BurnDuration"));
    // thing.bin 의 MediumFlame — `COLLIDE = DamageCollide 2 DAMAGE_FLAME` · `EXTENT = CIRCLE 6`
    PlaceObject(type, arg.x, arg.y, caster.addr, life, /*kind=*/2, /*dmgAmount=*/2, /*dmgType=*/1, /*radius=*/6.0f);
    return 1;
}

// castFireball_52C790(`GAME4_2.c:9580`) — 앞으로 날리는 불덩이. **겨눈 주문이 아니다**(Magic 을 안 쓴다).
//   ★ 종류가 세기마다 다르다 — 정답지 표 `0x587000+258864+4×세기`:
//     1 Fireball · 2 StrongFireball · **3·4·5 모두 TitanFireball**
//   나는 자리 = 시전자 자리 + (몸 반지름 × 2) × 방향 + 시전자 속도.
//   그 자리까지 선 추적(5)이 막히면 **시전자 제자리에** 만든다(실패가 아니다 — 벽에 붙어 쏘면 제 발밑에서 터진다).
//   빠르기 = `FireballSpeedCoeff[세기-1]`(1.6·2.0·2.4·2.5·2.6) × 그 종류의 SPEED 기본값.
int Server::CastFireball(Session& caster, int lvl, std::vector<OutPacket>& out) {
    (void)out;
    if (lvl < 1) lvl = 1;
    if (lvl > 5) lvl = 5;
    static const char* const kName[6] = { "", "Fireball", "StrongFireball", "TitanFireball", "TitanFireball", "TitanFireball" };
    const ProjTypeInfo* pt = ProjType(kName[lvl]);
    if (!pt || pt->type == 0) return 1;                      // newObjectByTypeID 가 0 이면 정답지도 1 을 돌려준다
    const int dir = caster.facingPrev;                       // +124 Direction1
    const float px = static_cast<float>(caster.posX), py = static_cast<float>(caster.posY);
    const float reach = static_cast<float>(kBodyRadius) * 2.0f;   // +176 반지름 두 배
    float sx = static_cast<float>(static_cast<double>(reach) * DirVecX(dir) + px) + static_cast<float>(caster.velX);
    float sy = static_cast<float>(static_cast<double>(reach) * DirVecY(dir) + py) + static_cast<float>(caster.velY);
    if (!MapTraceRay(px, py, sx, sy, 0x5)) { sx = px; sy = py; }  // 막히면 제자리
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    m.type = pt->type;
    m.flags = pt->flags;
    m.posX = m.newX = m.prevX = sx;
    m.posY = m.newY = m.prevY = sy;
    m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
    m.createdFrame = m.frame136 = frame_;
    m.speed = static_cast<float>(BalanceFloatInd("FireballSpeedCoeff", lvl - 1)) * pt->speed;
    m.owner = caster.addr;
    m.ud0 = m.ud2 = caster.addr;
    m.spell = 27;
    m.level = lvl;
    m.collideKind = 1;                                       // SparkExplosionCollide
    m.collideAmount = pt->collideAmount;
    m.velX = DirVecX(dir) * m.speed + static_cast<float>(caster.velX);
    m.velY = DirVecY(dir) * m.speed + static_cast<float>(caster.velY);
    missiles_.push_back(m); ++missilesMade_;
    return 1;
}

// castMeteor_52D9D0(`GAME4_2.c:10192`) — 커서 자리 **하늘(높이 255)** 에 운석을 띄우고 떨어뜨린다.
//   ① 그 사람이 이미 띄운 운석이 있으면 **거절**하고 글(`ExecSpel.c:TooManyMeteors`)을 보낸다
//   ② 시전자→커서 선 추적(깃발 **73**, 협동 모드면 9)이 막히면 글(0·2)을 내고 0
//   ③ 갱신자료 첫 칸 = `MeteorDamage[세기-1]`(45·65·85·105·125) · z 속도 = **−MeteorSpeed(8)**
int Server::CastMeteor(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    for (const auto& p : placed_) {
        if (p.kind == 3 && p.owner == caster.addr) {
            SendPriMsg(caster, "ExecSpel.c:TooManyMeteors", out);   // netPriMsgToPlayer_4DA2C0
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* mo = PlaceObject(NamedType("Meteor"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/3)) {
        mo->z = 255.0f;                                      // unitRaise_4E46F0(255)
        mo->zVel = -static_cast<float>(BalanceFloat("MeteorSpeed"));
        QueueDeltaZStart(mo->netCode, mo->z, mo->zVel, 0.0f);   // 09-20 ⑰
        mo->damage = static_cast<int>(BalanceFloatInd("MeteorDamage", lvl - 1));
    }
    return 1;
}

// castMeteorShower_52D8A0(`GAME4_2.c:10112`) — 커서 자리에 **뿌리개**(보이지 않는 MeteorShower)를 놓는다.
//   선 추적은 운석과 같다. 갱신자료 첫 칸에 같은 `MeteorDamage[세기-1]` 을 담아 떨어뜨릴 운석에 물려준다.
//   ⚠ 운석과 달리 **「하나만」 제한이 없다**(정답지에 그 검사가 없다).
int Server::CastMeteorShower(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* so = PlaceObject(NamedType("MeteorShower"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/4))
        so->damage = static_cast<int>(BalanceFloatInd("MeteorDamage", lvl - 1));
    return 1;
}

// newObjectWithTypeInd_4E3450 + createAt_4DAA50 — 몬스터 하나를 그 자리에 만든다.
//   ⚠ 움직임·AI 는 묶음 6·7 이다. 여기서는 **나오는 것까지**가 산출물이다.
void Server::SpawnCreature(uint16_t type, float x, float y, const std::string& owner) {
    if (type == 0) return;
    Creature k;
    k.owner = owner;
    if (owner.empty()) k.aggression = 0.5f;   // ★ 09-29 B2 — 맵 몬스터 공격 성향 기본(unit_monster.go:124)
    if (type == kCarnivorousPlantType) {      // ★ 09-29 B4 — 식충식물 자체 시야 = 근접 사거리 + 몸 반지름 + 10(`unit_monster.go:21~24` — 되갚기 조건 63 의 자)
        const MonsterStats* pm = FindMonster(type);
        const auto tb = typeBodies_.find(type);
        k.sightRange = static_cast<float>((pm ? pm->meleeRange : 0.0) + (tb != typeBodies_.end() ? tb->second.r : 0.0f) + 10.0);
    }
    k.type = type;
    k.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    k.x = x;
    k.y = y;
    // ★ 09-29 B5 — 놓을 때 지난 자리도 그 자리(`CreateObjectAt` `object.go:251` PrevPos = pos).
    //   ✖ 전에는 (0,0) 이라 첫 틱에 놀기(0)가 「움직였다」로 보고 (0,0) 쪽 바라보기(25)를 쌓았다(B5 시험에서 잡힘)
    k.prevX = x;
    k.prevY = y;
    k.health = static_cast<uint16_t>(TuningFor(type).health);
    // ★ 09-29 B7 — 자동 주문(`nox_xxx_monsterAutoSpells_54C0C0` `GAME5.c:4211~4252`) — 종류 이름으로 주문 칸(+1488 + 4×주문)에 깃발 · 세기(+2040) 3.
    //   우리 소환 열셋 중엔 **Beholder 하나** — 전기 광선 24 = 공격(0x40000000) · 퀘스트가 아니면 점멸 4 = 달아날 때(0x80000000)
    //   (나머지 여덟 — UrchinShaman·Wizard·WizardWhite·Lich·LichLord·Demon·WizardGreen·WillOWisp — 은 우리 표에 없음)
    if (type == 1382) {
        k.spells.push_back({ 24, 0x40000000u });
        if (!(questTotal_ > 0 || questStage_ > 0)) k.spells.push_back({ 4, 0x80000000u });
        k.spellLevel = 3;
    }
    pets_.push_back(k);
}

// spellArachna_52DC80(`GAME4_2.c:10327`) — 시전자→커서 선 추적(5)이 통하면 그 자리에
//   `ArachnaphobiaFocus`(보이지 않는 자리표)를 놓는다. 막히면 글(0·2)을 내고 0.
int Server::CastArachnaphobia(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* ao = PlaceObject(NamedType("ArachnaphobiaFocus"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/7))
        ao->nextHitFrame = frame_;                           // +136 초깃값 — 놓자마자 한 마리
    return 1;
}

// castPixies_540440(`GAME4_3.c:8299`) — 시전자 둘레에 **픽시**를 `PixieCount[세기-1]`(2·3·4·5·6) 마리까지 채운다.
//   · 이미 제 픽시가 그만큼 있으면 **아무것도 안 만든다**(모자란 만큼만 채운다)
//   · 자리 = 시전자 + (시전자 반지름 + 4) × 무작위 방향 · 선 추적(5)이 막히면 그 마리는 거른다
//   · 속도 0 으로 나오고, 수명 = 지금 + 초당틱 × 무작위(30, 90)
//   ⚠ **날아다니는 AI 는 아직 없다**(`updatePixie_53CD20` — 적 찾기·떼 짓기·주인 따라가기·되돌아오기).
//     그건 유닛 움직임 묶음(6·7)이다. 여기서는 **나오는 것·수명·닿음 피해**까지가 산출물이다.
int Server::CastPixieSwarm(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out) {
    (void)out;
    const uint16_t type = NamedType("Pixie");
    if (type == 0) return 1;
    int have = 0;
    for (const auto& m : missiles_) if (m.type == type && m.owner == caster.addr) ++have;
    const int want = static_cast<int>(BalanceFloatInd("PixieCount", lvl - 1));
    if (have >= want) return 1;                              // 이미 다 차 있으면 소리만
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    const float dist = static_cast<float>(kBodyRadius) + 4.0f;   // +176 반지름 + 4
    for (int n = have; n < want; ++n) {
        const int dir = static_cast<int>(NextRandom(256));
        const float px = cx + dist * DirVecX(dir);
        const float py = cy + dist * DirVecY(dir);
        if (!MapTraceRay(cx, cy, px, py, 0x5)) continue;
        MissileObj m;
        m.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        m.type = type;
        m.flags = 0x4000;                                    // thing FLAGS = AIRBORNE
        m.posX = m.newX = m.prevX = px;
        m.posY = m.newY = m.prevY = py;
        m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
        m.createdFrame = m.frame136 = frame_;
        m.speed = 0.0f;                                      // 속도 0 으로 나온다(움직임은 AI 몫 — 묶음 6·7)
        m.owner = caster.addr;
        m.ud0 = m.ud2 = caster.addr;
        m.spell = spell;
        m.level = lvl;
        m.collideKind = 2;                                   // PixieCollide
        m.collideAmount = 8;                                 // thing.bin `COLLIDE = PixieCollide 8`
        m.expireFrame = frame_ + static_cast<uint32_t>(kServerTickRate) * (30u + NextRandom(61u));   // randomInt(30,90) 초
        missiles_.push_back(m); ++missilesMade_;
    }
    return 1;
}

// 놓인 것의 **닿음 함수** — 몸이 겹친 것에 한 틱마다 한 번.
//   · `collideDamage_4E9430` — 피해 = **양 >> 1**, 단 양이 1 이면 홀수 프레임만 1. 갈래·팀을 안 가린다
//   · `collideManadrain_4E9490` — **사람에게만**, 마나를 **양만큼** 깎는다(피해가 아니다)
void Server::PlacedCollide(PlacedObj& o, std::vector<OutPacket>& out) {
    if (o.dmgAmount == 0) return;
    const int dmg = (o.dmgAmount == 1 && (frame_ & 1u)) ? 1 : (o.dmgAmount >> 1);
    const float reach = o.radius + static_cast<float>(kBodyRadius);
    const float rr = reach * reach;
    auto touch = [&](const Target& t, float tx, float ty) {
        const float dx = tx - o.x, dy = ty - o.y;
        if (dx * dx + dy * dy > rr) return;
        if (o.collideFn == 1) {
            if (t.addr.empty()) return;                      // 사람만(+8 & 4)
            auto it = sessions_.find(t.addr);
            if (it == sessions_.end() || ManaOnline(it->second.charClass) == 0) return;   // 마나 없는 직업은 거른다
            PlayerManaSub(it->second, static_cast<int>(o.dmgAmount));
            return;
        }
        HurtTarget(t, dmg, out, o.owner, o.dmgType, nullptr, HitterObj(o.type));   // ★ 4-c3b2 a3 = 놓인 것(`GAME3_3.c:3820`)
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        touch(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k)
        touch(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
}

// spellCastCleansingFlame_52D5C0(`GAME4_2.c:10031`) — 시전자 둘레에 **불꽃 48개**를 뿌린다.
//   · 온라인(협동 아님)이면 **세기를 4로 못 박는다**
//   · 불꽃마다 `세기 − 무작위(0,1)` 로 종류를 고르고, 0 이하면 그 번은 안 만든다
//   · 종류표 열 칸 — 붉은 다섯(주문 10 CleansingFlame) · 파란 다섯(주문 11 CleansingManaFlame).
//     ★ 붉은 것은 `DamageCollide`(피해), **파란 것은 `ManaDrainCollide`(마나 빨기)** 다
//   · 자리 = 시전자 + (불꽃 반지름 + 시전자 반지름 + 4) × 방향(무작위 0~255).
//     시작점→그 자리 선 추적(65)이 막히면 그 불꽃은 **안 만든다**
//   · 속도 = 방향 × 4 · 수명 = 지금 + 무작위(3초, 6초)
int Server::CastCleansingFlame(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out) {
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest) lvl = 4;                                     // gameFlags(2048) 밖 = 온라인
    // 표 열 칸 — 0~4 붉은 · 5~9 파란. 정답지 셈은 `2487756 + 4×((주문 != 10 ? 5 : 0) + v8)` = 칸 (off + v8 − 1)
    static const char* const kName[10] = {
        "SmallFlameCleanse", "SmallFlameCleanse", "MediumFlameCleanse", "FlameCleanse", "LargeFlameCleanse",
        "SmallBlueFlameCleanse", "SmallBlueFlameCleanse", "MediumBlueFlameCleanse", "BlueFlameCleanse", "LargeBlueFlameCleanse",
    };
    static const float kRadius[10] = { 4, 4, 6, 8, 12, 4, 4, 6, 8, 12 };
    static const uint8_t kAmount[10] = { 1, 1, 2, 3, 4, 3, 3, 6, 9, 12 };
    const int off = (spell != 10) ? 5 : 0;
    const bool manaFlame = (off == 5);
    // ★ 이 사람의 불꽃이 하나라도 남아 있으면 **거절**한다(글 `plyrspel.c:TooManySpells`)
    for (const auto& p : placed_) {
        if (p.kind == 6 && p.owner == caster.addr) {
            SendPriMsg(caster, "plyrspel.c:TooManySpells", out);
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    for (int n = 0; n < 48; ++n) {
        const int v8 = lvl - static_cast<int>(NextRandom(2));   // randomInt(0,1)
        if (v8 < 1) continue;
        const int idx = off + v8 - 1;
        if (idx < 0 || idx >= 10) continue;
        const int dir = static_cast<int>(NextRandom(256));
        const float dist = kRadius[idx] + static_cast<float>(kBodyRadius) + 4.0f;
        const float fx = cx + dist * DirVecX(dir);
        const float fy = cy + dist * DirVecY(dir);
        if (!MapTraceRay(cx, cy, fx, fy, 65)) continue;
        PlacedObj* fo = PlaceObject(NamedType(kName[idx]), fx, fy, caster.addr, kPlacedNoLife, /*kind=*/6,
                                    kAmount[idx], /*dmgType=*/1, kRadius[idx]);
        if (!fo) continue;
        fo->collideFn = manaFlame ? 1 : 0;
        fo->velX = DirVecX(dir) * 4.0f;
        fo->velY = DirVecY(dir) * 4.0f;
        fo->originX = cx;
        fo->originY = cy;
        fo->expireFrame = frame_ + 3u * static_cast<uint32_t>(kServerTickRate)
                        + NextRandom(3u * static_cast<uint32_t>(kServerTickRate) + 1u);   // randomInt(3×FPS, 6×FPS)
    }
    return 1;
}

// castFist_52D3C0(`GAME4_2.c:9956`) — 커서 자리 하늘에 **주먹**을 띄워 떨어뜨린다. 운석과 같은 꼴이되,
//   ① 그 사람이 이미 띄운 주먹이 있으면 글(`ExecSpel.c:TooManyFists`)과 함께 거절
//   ② 세기별 종류 — 1 SmallFist(둘레 25) · 2 MediumFist(30) · **3·4·5 LargeFist(35)**
//   ③ 갱신자료 첫 칸 = `FistOfVengeanceDamage[세기-1]`(50·100·200·300·400) · z 속도 = −FistSpeed(10)
//   ④ 깃발에 0x800000 을 얹는다 — 떨어짐 논리에서 **되튀는 갈래**가 이 비트다
int Server::CastFist(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    if (lvl < 1) lvl = 1;
    if (lvl > 5) lvl = 5;
    for (const auto& p : placed_) {
        if (p.kind == 5 && p.owner == caster.addr) {
            ++fistTooMany_;          // ★ 갈래 — 이미 자기 주먹이 떠 있다
            SendPriMsg(caster, "ExecSpel.c:TooManyFists", out);
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        ++fistNoLos_;                // ★ 갈래 — 커서 자리가 안 보인다(「You cannot see that spot」)
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    static const char* const kName[6] = { "", "SmallFist", "MediumFist", "LargeFist", "LargeFist", "LargeFist" };
    static const float kRadius[6]     = { 0.0f, 25.0f, 30.0f, 35.0f, 35.0f, 35.0f };   // EXTENT CIRCLE
    if (PlacedObj* fo = PlaceObject(NamedType(kName[lvl]), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/5,
                                    /*dmgAmount=*/0, /*dmgType=*/0, kRadius[lvl])) {
        fo->z = 255.0f;                                      // unitRaise_4E46F0(255)
        fo->zVel = -static_cast<float>(BalanceFloat("FistSpeed"));
        fo->damage = static_cast<int>(BalanceFloatInd("FistOfVengeanceDamage", lvl - 1));
        // ★ 09-20 ⑰ — **떨어지는 모습**은 이 알림으로 간다(안 보내면 바닥에 붙어 나타난다).
        //   ⚠ 이 틱에 보내면 클라가 버린다 — 다음 틱 머리로 미룬다(`QueueDeltaZStart` 주석)
        QueueDeltaZStart(fo->netCode, fo->z, fo->zVel, 9.0f);
    }
    return 1;
}

// meteorExplode_53D6E0(`GAME4_3.c:6540`) — 높이가 0 이하가 된 운석이 하는 일.
//   · 그을음 · **화면 흔들기 10**(earthquakeSend) · `MeteorExplode` 그림 오브젝트 · 소리 87
//   · 둘레 피해 : 바깥 **80** · 안쪽 **30** · 피해 = 갱신자료 첫 칸 · 종류 **7**(폭발)
//   ⚠ 아직 안 옮긴 것 — 그을음 · 소리 · (09-29 Z1-A — 벽 피해는 `DamageUnitsAround` 끝이 한다 · who = 주인)
void Server::MeteorExplode(PlacedObj& o, std::vector<OutPacket>& out) {
    const float x = o.x, y = o.y;
    const int dmg = o.damage;
    const std::string owner = o.owner;
    SendJiggle(x, y, 10, out);                               // earthquakeSend_4D9110(자리, 10)
    // ★ 09-27 S1 — v9 = who = 주인 사람(+72 지난 자리) · 주인이 사람이 아니면(없으면) 옛 방식(막기 없음)
    AttackSrc ws;
    const AttackSrc* wsp = nullptr;
    if (auto oi = sessions_.find(owner); oi != sessions_.end()) {
        ws.x = static_cast<float>(oi->second.prevX); ws.y = static_cast<float>(oi->second.prevY);
        wsp = &ws;
    }
    DamageUnitsAround(x, y, 80.0f, 30.0f, dmg, 7, owner, Target{}, out, HitterByAddr(owner), wsp,
                      WallHitterOfAddr(owner));   // ★ 4-c3b1 who = 주인 사람(`GAME4_3.c:6839~6840`)
    // 터짐 그림은 **1초 뒤 스스로 사라지는**(OneSecondDieUpdate) 장식 오브젝트다
    PlaceObject(NamedType("MeteorExplode"), x, y, owner, static_cast<uint32_t>(kServerTickRate), /*kind=*/0);
}

// mapDamageUnitsAround_4E25B0(Go `game.go:1407`) — 둘레 피해. rr = max(r1, r2)
//   · 자기(날아간 것)와 **이미 직접 맞은 것**은 건너뛴다 · 벽 너머(선 추적 1)는 안 닿는다
//   · 안쪽 r2 까지는 온전, 그 밖은 `1 − (거리 − r2) / (rr − r2)` 로 줄어든다
//   ★ 09-29 Z1-A — 벽 피해(mapDamageToWalls_534FC0)를 끝에 붙였다(`wallWho` — 부르는 쪽이 who 를 넘긴다)
//   ★ 09-27 S1 — 정답지는 `CallDamage(who, nil, …)`(`game.go:1438`) — a3 가 없으니 피해 함수의 v9 = who 다(방패·대검·지팡이 막기·되받아치기가 이것을 본다).
//     전에는 여기서 때린 것 없이(nullptr) 넘겨 **둘레 피해는 막기 판정을 아예 안 탔다**. 부르는 쪽이 who 로 채운 `whoSrc` 를 넘긴다
//     (불덩이 = 불덩이 자신 · 죽음 광선·운석 = 시전자 사람). 못 넘기면 옛 방식(막기 없음)
//   ⚠ who 자신 건너뛰기(`:1418` `u == who && !damageWalls`)는 옮기지 않는다 — damageWalls 가 늘 참으로 돌아와(`:1401·1448`) 실제로 안 걸린다
void Server::DamageUnitsAround(float cx, float cy, float r1, float r2, int dmg, uint8_t dtype,
                               const std::string& by, const Target& skip, std::vector<OutPacket>& out, HitterRef hitter,
                               const AttackSrc* whoSrc, const WallHitter& wallWho) {
    const float rr = (r1 < r2) ? r2 : r1;
    if (rr <= 0.0f || dmg <= 0) return;
    auto hit = [&](const Target& t, float tx, float ty) {
        if (!skip.none() && t.addr == skip.addr && t.pet == skip.pet) return;
        const float dx = tx - cx, dy = ty - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > rr) return;
        if (!MapTraceRay(cx, cy, tx, ty, 0x1)) return;
        float rdmg = static_cast<float>(dmg);
        if (dist >= r2 && rr > r2) rdmg *= 1.0f - (dist - r2) / (rr - r2);
        HurtTarget(t, static_cast<int>(rdmg), out, by, dtype, whoSrc, hitter);   // Go `game.go:1438` CallDamage(who, nil, ...)
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        hit(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k)
        hit(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
    // ★ 09-29 Z1-A — 끝에 늘 벽 피해(`game.go:1441~1447`) — 칸 네모 = 둘레 네모(rr) ÷ 23 · 반지름은 **r1** · 피해는 줄이지 않은 값
    const int gx0 = static_cast<int>(cx - rr) / 23, gy0 = static_cast<int>(cy - rr) / 23;
    const int gx1 = static_cast<int>(cx + rr) / 23, gy1 = static_cast<int>(cy + rr) / 23;
    MapDamageToWalls(gx0, gy0, gx1, gy1, cx, cy, r1, dmg, dtype, wallWho, out);
}

// fireballCollide_4E9AC0(`GAME3_3.c:4160`) — 닿은 자리에서 터진다. 인자 한 바이트가 크기를 다 정한다.
//   · 밀기 : 둘레 **양 × 0.66667** · 안쪽 0 · 힘 50
//   · 직접 맞은 것 : **양 >> 1** 피해(종류 1 불)
//   · 둘레 피해 : 바깥 **양 × 0.33333** · 안쪽 **15** · 같은 **양 >> 1** · 직접 맞은 것은 건너뜀
//   · 불똥 그림 : `netSparkExplosionFx_5231B0` = **`93 <x u16> <y u16> <양 i8>` 6바이트**를 화면 안 모두에게
//     ⚠ 옛 자리표시는 여기에 `85`(FireBoom)를 보냈다 — **짐작이었고, 정답지는 `93` 이다.**
//   ⚠ 아직 안 옮긴 것 — 그을음(sMakeScorch) · 소리 42 · (09-29 Z1-A — 벽 피해는 `DamageUnitsAround` 끝 · who = 불덩이 — 유닛이 아니라 폭발 자리에서 잰다)
void Server::FireballExplode(MissileObj& m, const Target& hit, std::vector<OutPacket>& out) {
    const int amount = static_cast<int>(m.collideAmount);
    if (amount <= 0) return;
    PushUnitsAround(m.posX, m.posY, static_cast<float>(static_cast<double>(amount) * 0.66666669), 0.0f, 50.0f);
    const int dmg = amount >> 1;
    // ★ 4-c3b2 — 직격의 때린 것 = 불덩이(a3 = a1 · `GAME3_3.c:4181`) · 둘레 피해는 who = 불덩이 자신(`:4185·4191` v12 = a1)이라
    //   때린 이도 불덩이다 — 몬스터가 쏜 불덩이여도 둘레 쪽은 몬스터 소리 모음을 안 본다
    // ★ 09-26 M1 — 직격의 a3 = 불덩이(클래스 MISSILE · `:4181`) · 전에는 때린 것 없이(nullptr) 넘겨 막기 판정을 안 탔다.
    //   ★ 09-27 S1 — 둘레 피해(아래)도 정답지는 who = 불덩이 · a3 = 없음이라 v9 = 불덩이다 → 같은 `src` 를 넘긴다(대검이 둘레 불꽃도 막는다)
    AttackSrc src;
    src.x = m.posX; src.y = m.posY;
    src.byMonsterType = m.type;
    src.rootIsMonster = MissileRootIsMonster(m);
    src.missile = true;
    if (!hit.none()) HurtTarget(hit, dmg, out, m.owner, 1, &src, HitterMissile(m));
    WallHitter fw;                                           // ★ 09-29 Z1-A — 벽 피해 who = 불덩이(MISSILE — 방향 없음 · 재질은 그 종류)
    fw.present = true; fw.x = m.posX; fw.y = m.posY; fw.material = MaterialOfWire(m.type);
    fw.src = m.owner;                                        // ★ 09-29 B6 — 불덩이의 임자(사람) · 몬스터가 쏜 것은 비어 기록 안 됨(⚠ 쏜 몬스터 추적 없음)
    DamageUnitsAround(m.posX, m.posY, static_cast<float>(static_cast<double>(amount) * 0.33333334), 15.0f,
                      dmg, 1, m.owner, hit, out, HitterObj(m.type), &src, fw);
    const int16_t ex = static_cast<int16_t>(m.posX), ey = static_cast<int16_t>(m.posY);
    const uint8_t fx[6] = {
        static_cast<uint8_t>(Op::FX_SPARK_EXPLOSION),
        static_cast<uint8_t>(ex), static_cast<uint8_t>(ex >> 8),
        static_cast<uint8_t>(ey), static_cast<uint8_t>(ey >> 8),
        static_cast<uint8_t>(amount),
    };
    SendFxAllCli(m.posX, m.posY, fx, sizeof(fx), out);
}

// changeOwner_52BE40(`spell_counter.go:82`) — MISSILE+MissileMagic 만(날리는 Magic 은 모두) · 대상이 새 주인이면 대상 = 옛 주인 · 주인 = 새 주인 · 만든 프레임 = 지금(소리 → 10)
//   ⚠ 자리바꾸기(SWAP 115) 주문이면 cspellRemoveSpell_52BC90(4-e)
void Server::MissileChangeOwner(MissileObj& m, const std::string& own, uint32_t frame) {
    if (m.ud1.addr == own && !own.empty()) {
        m.ud1 = Target{ m.ud0, -1 };
        m.ud0 = own;
        m.owner = own;
        m.createdFrame = frame;
    }
}

// ★ 자리표시 효과 — 옛 시전 자리에 있던 투사체(27·24·43)·소환(75~87)을 그대로 옮겨 둔다. 4-c·4-d·4-f 에서 걷어낸다.
bool Server::SpellEffectPlaceholder(uint8_t spell, Session& s, const SpellArg& arg, std::vector<OutPacket>& out) {
    (void)out;
    if (const uint16_t stype = SummonType(spell)) {
        Creature k;
        k.owner = s.addr;
        k.type = stype;
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.facing = s.facing;
        const double rad = s.facing * 3.14159265358979 / 128.0;
        k.x = s.x + kPetReach * std::cos(rad);
        k.y = s.y + kPetReach * std::sin(rad);
        k.health = static_cast<uint16_t>(TuningFor(k.type).health);
        pets_.push_back(k);
        return true;
    }
    // ★ 자리표시 투사체(shots_)는 **걷어냈다**(주문 27·43·24 가 모두 정답지로 옮겨졌다).
    //   효과 함수를 아직 안 옮긴 주문은 위 소환 갈래에 걸리지 않으면 **아무 일도 안 한다** —
    //   부르는 쪽이 「안 옮김」으로 세고 적는다.
    (void)arg;
    return false;
}

// ── 날아가는 오브젝트 (묶음 4-a2) ─────────────────────────────────────────
//
// 정답지: 투사체(MISSILE)는 5×0.2 물리에 안 든다 — 따로 목록(Objs.MissileList)을 틱마다
//   갱신 함수 → 움직임(sub_51B810) → 선 추적 맞힘(projectileTraceHit_537850 → 충돌 함수 둘) → PrevPos·PosVec 넘김(server_update.go:72~88).
//   모양은 전부 EXTENT = CENTER(점)라 모양 충돌(sub_548220)에 안 걸리고, 선 추적으로만 맞는다.

namespace {
// sub_57F2A0(`GAME5_2.c:2061`) — 칸 안에서 점이 어느 삼각형 쪽인가. ⚠ LOBYTE 대입이 윗바이트를 남기는 것까지 그대로(칸 밖이면 0~3 밖 값)
int32_t WallTriSide(float px, float py, int gx, int gy) {
    const int32_t v3 = static_cast<int32_t>(px - static_cast<float>(23 * gx));
    int32_t v4 = static_cast<int32_t>(py - static_cast<float>(23 * gy));
    const uint32_t cond = (22 - v3 <= v4) ? 1u : 0u;
    if (v3 <= v4) {
        uint32_t t = (static_cast<uint32_t>(v4) & ~0xFFu) | cond;
        uint32_t v6 = t - 1u;
        v6 = (v6 & ~0xFFu) | ((v6 & 0xFFu) & 0xFDu);
        return static_cast<int32_t>(v6 + 3u);
    }
    const uint32_t t = (static_cast<uint32_t>(v4) & ~0xFFu) | cond;
    return static_cast<int32_t>(t + 1u);
}
// sub_57F1D0(`GAME5_2.c:2027`) — 칸 안(23 나머지) 사분면 비트
uint8_t WallQuadBits(float hx, float hy) {
    uint8_t v1 = 0;
    const int v2 = static_cast<int>(hx);
    const uint8_t v7 = static_cast<uint8_t>(static_cast<int>(hy) % 23);
    const double v3 = static_cast<double>(static_cast<uint8_t>(v2 % 23));
    if (v3 >= 11.5) {
        if (static_cast<double>(v7) >= 11.5) v1 = 4;
        if (static_cast<double>(v7) <= 11.5) v1 |= 1;
    }
    if (v3 <= 11.5) {
        if (static_cast<double>(v7) >= 11.5) v1 |= 8;
        if (static_cast<double>(v7) <= 11.5) v1 |= 2;
    }
    return v1;
}
const float kWallDiag = 0.70709997f;
}  // namespace

// sub_57CDB0(`GAME5_2.c:1757~2020`) — 벽 칸 모양(0~10) · 출발점 삼각형 · 맞은 점 사분면으로 벽 법선(대각선 넷 중 하나). 모르면 false(법선 안 씀)
static bool WallNormal(int shape, int32_t v9, uint8_t v4, float& nx, float& ny) {
    auto set = [&](float x, float y) { nx = x; ny = y; return true; };
    switch (shape) {
        case 0: if (v9 != 1 && v9) return set(kWallDiag, kWallDiag); return set(-kWallDiag, -kWallDiag);
        case 1: if (v9 == 1 || v9 == 2) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, kWallDiag);
        case 2:
            switch (v9) {
                case 0: return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
                case 1: if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag);
                case 2: return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
                case 3: if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag);
                default: return true;           // 정답지 default: return 1 — 법선을 안 적는다(앞 값 그대로)
            }
        case 3:
            if (!v9) return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
            if (v9 != 1) return set(kWallDiag, kWallDiag);
            if (!(v4 & 1)) return set(kWallDiag, -kWallDiag);
            return set(-kWallDiag, -kWallDiag);
        case 4:
            if (v9 == 1) { if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag); }
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            return set(-kWallDiag, kWallDiag);
        case 5:
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            return set(-kWallDiag, -kWallDiag);
        case 6:
            if (!v9) return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            return set(kWallDiag, -kWallDiag);
        case 7:
            if (v9 == 1) { if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag); }
            if (v4 & 1) return set(kWallDiag, kWallDiag);
            return set(-kWallDiag, kWallDiag);
        case 8:
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            return set(-kWallDiag, (v4 & 1) ? -kWallDiag : kWallDiag);
        case 9:
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            if (v4 & 4) return set(kWallDiag, -kWallDiag);
            return set(-kWallDiag, -kWallDiag);
        case 10:
            if (v9) { if (v4 & 2) return set(kWallDiag, -kWallDiag); return set(kWallDiag, kWallDiag); }
            return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
        default: return false;
    }
}

// server/spells.go:360 Nox_xxx_spellFlySearchTarget — 사람 시전자·공격 주문이면 커서 오브젝트가 적이면 그것 ·
//   아니면 중심(pos 가 있고 선 추적 1|3 통과면 pos, 아니면 날아가는 것 자리) 둘레 dist 에서 가장 가까운 대상
//   CanInteract = CanSee(버프 — 눈멂·투명·적외선, 4-a4) + 시야선 · 몬스터 생성기(8)는 없음
Target Server::SpellFlySearchTarget(bool havePos, float px, float py, float mslX, float mslY, const std::string& mslOwner,
                                    const std::string& mslSelf, uint32_t sflags, float dist, const Session* self, const Buffs* mslBuffs) const {
    // a5 = 0 — 날아가는 것 자신(mslSelf: 만들 때는 시전자)은 뺀다
    if (self && (sflags & 0x20)) {
        const Target cur = FindObjectAtCursor(*self);
        if (!cur.none() && TargetIsEnemy(*self, cur) && (mslSelf.empty() || cur.addr != mslSelf)) return cur;
    }
    float cx = mslX, cy = mslY;
    if (havePos) {
        if (MapTraceRay(mslX, mslY, px, py, 0x5)) { cx = px; cy = py; }
    }
    auto ownerIt = sessions_.find(mslOwner);
    const Session* owner = (ownerIt != sessions_.end()) ? &ownerIt->second : nullptr;
    const float dist2 = dist * dist;
    float minDist = 1e+08f;
    Target found;
    auto consider = [&](const Target& t, float ox, float oy, const Buffs* tb, double tvx, double tvy) {
        if ((sflags & 0x20) && owner && !TargetIsEnemy(*owner, t)) return;
        const float dx = cx - ox, dy = cy - oy;
        const float odist = dy * dy + dx * dx;
        if (odist > dist2) return;
        if (!CanSeeBuffs(mslBuffs, tb, tvx, tvy, 0) || !MapTraceVision(ox, oy, mslX, mslY)) return;   // CanInteract(msl, it)
        if (owner && (!CanSeeBuffs(&owner->buffs, tb, tvx, tvy, 0) ||
                      !MapTraceVision(ox, oy, static_cast<float>(owner->posX), static_cast<float>(owner->posY)))) return;
        if (odist < minDist) { found = t; minDist = odist; }
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.observer || o.respawnAtFrame != 0) continue;         // 관전 · DEAD
        if (!mslSelf.empty() && o.addr == mslSelf) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY), &o.buffs, o.velX, o.velY);
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (pets_[i].dead) continue;                         // ★ 10-01 시체 — `FlagDestroyed|FlagDead`(`server/spells.go:426`)
        const auto tb = typeBodies_.find(pets_[i].type);
        if (tb != typeBodies_.end() && (tb->second.subclass & 0xC000)) continue;   // 몬스터 SUBCLASS 0x8000·0x4000
        consider(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y), &pets_[i].buffs, 0.0, 0.0);   // ⚠ 소환수 속도는 6
    }
    return found;
}

// createSpellFly_4FDDA0(`GAME4.c:2880`)
bool Server::CreateSpellFly(Session& s, Target target, uint8_t spell, std::vector<OutPacket>& out) {
    (void)out;
    if (magicType_.type == 0) return false;
    if (target.none()) {
        // 사람 — 커서(playerinfo +2284/+2288) 둘레 600 에서 고른다(날아가는 것 = 시전자)
        target = SpellFlySearchTarget(true, static_cast<float>(s.aimX), static_cast<float>(s.aimY),
                                      static_cast<float>(s.posX), static_cast<float>(s.posY), s.addr, s.addr, SpellFlags(spell), 600.0f, &s, &s.buffs);
    }
    const int dir = s.facingPrev;                            // +124 Direction1
    const float r = static_cast<float>(kBodyRadius) + 4.0f;  // +176 반지름 + 4
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float sx = static_cast<float>(static_cast<double>(r) * DirVecX(dir) + px) + static_cast<float>(s.velX);
    const float sy = static_cast<float>(static_cast<double>(r) * DirVecY(dir) + py) + static_cast<float>(s.velY);
    if (!MapTraceRay(px, py, sx, sy, 0x5)) return false;     // 몸 앞이 벽이면 안 만든다(정답지는 이때 0 을 돌려 fizzle)
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    m.type = magicType_.type;
    m.flags = magicType_.flags;
    m.posX = m.newX = m.prevX = sx;
    m.posY = m.newY = m.prevY = sy;
    m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
    m.createdFrame = m.frame136 = frame_;
    m.speed = magicType_.speed;
    m.owner = s.addr;
    m.ud0 = s.addr;
    m.ud1 = target;
    m.ud2 = s.addr;
    m.spell = spell;
    m.level = SpellPower(s, spell);
    m.velX = DirVecX(dir) * m.speed + static_cast<float>(s.velX);
    m.velY = DirVecY(dir) * m.speed + static_cast<float>(s.velY);
    // 시전자가 적외선(21)이면 날아가는 것에도(buffApplyTo — 유닛이 아니라 알림 없음) · 시전 소리는 10
    if (s.buffs.Has(21)) {
        m.buffs.bits |= 1u << 21;
        m.buffs.dur[21] = s.buffs.dur[21];
        m.buffs.power[21] = s.buffs.power[21];
    }
    std::printf("[날리기] 프레임 %u %s 주문 %u(%s) Magic 번호 %u 자리 (%.1f,%.1f) 대상 %s\n", frame_, s.addr.c_str(), spell,
                SpellName(spell) ? SpellName(spell) : "?", m.netCode, sx, sy,
                target.none() ? "없음" : (!target.addr.empty() ? target.addr.c_str() : "소환수"));
    std::fflush(stdout);
    missiles_.push_back(m); ++missilesMade_;
    return true;
}

// netReportDestroyObject_5289D0(`GAME4_2.c:7630`) — 보던 사람에게 `31 <번호>` Packet0 · 날아가는 것은 갱신에 늘 실리므로 게임 안 모두
void Server::DestroyMissile(size_t i, std::vector<OutPacket>& out) {
    if (i >= missiles_.size()) return;
    const uint16_t nc = missiles_[i].netCode;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    missiles_.erase(missiles_.begin() + static_cast<long>(i));
    ++missilesGone_;
}

// ── 묶음 4-a4 버프 틀 ─────────────────────────────────────────────────
//   정답지 GAME4.c:3538~3650(buffApplyTo · GetBuffTimer · GetPower · ClearBuffs · spellBuffOff · updateUnitBuffs) · GAME3_3.c:1269(setUnitBuffFlags)
//   · spells_buffs.go(castBuffSpell) · server/object.go:76(CanSee) · GAME3_3.c:2593(sub_4E7540)
//   ⚠ 소리(On/Off · 26 · 779)는 10 · Hecubah/Necromancer 갈래는 몬스터(7) · 보호 CRC 없음

Buffs* Server::BuffsOf(const Target& t) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? &it->second.buffs : nullptr;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? &pets_[static_cast<size_t>(t.pet)].buffs : nullptr;
}
const Buffs* Server::BuffsOf(const Target& t) const {
    return const_cast<Server*>(this)->BuffsOf(t);
}
uint16_t Server::UnitNetCode(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? it->second.netCode : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? pets_[static_cast<size_t>(t.pet)].netCode : 0;
}

// setUnitBuffFlags_4E48F0 — 비트를 넣고 보는 사람 모두에게 `5a` 를 올린다(더티 0x800000)
void Server::SetBuffFlags(const Target& t, uint32_t bits) {
    Buffs* b = BuffsOf(t);
    if (!b) return;
    b->bits = bits;
    MarkDirty(UnitNetCode(t), PendingReport::What::Enchant);
}

// buffApplyTo_4FF380
void Server::ApplyEnchant(const Target& t, int buff, int dur, int power) {
    if (buff < 0 || buff >= 32) return;
    Buffs* b = BuffsOf(t);
    if (!b) return;
    // ⚠ Hecubah 반마법(29) 무시 · 퀘스트 Hecubah/Necromancer 혼란(3) · 몬스터 공포(11) 소리 갈래는 몬스터(7)
    if (!t.addr.empty() && sessions_.at(t.addr).respawnAtFrame != 0) return;   // +16 & 0x8022(DEAD)
    if (!b->Has(buff) || b->dur[buff] != 0) {
        if (buff != 0) BuffOff(t, 0);                    // 새 버프는 투명을 끈다
        b->dur[buff] = static_cast<uint16_t>(dur);
        b->power[buff] = static_cast<uint8_t>(power);
        SetBuffFlags(t, b->bits | (1u << buff));
        // 소리 On(주문 정의) → 10
    }
}

// spellBuffOff_4FF5B0
void Server::BuffOff(const Target& t, int buff) {
    if (buff < 0 || buff >= 32) return;
    Buffs* b = BuffsOf(t);
    if (!b || !b->Has(buff)) return;
    SetBuffFlags(t, b->bits & ~(1u << buff));
    b->dur[buff] = 0;
    b->power[buff] = 0;
    // 16·30 아니면 소리 Off → 10
}

// unitClearBuffs_4FF580
void Server::ClearBuffs(const Target& t) {
    Buffs* b = BuffsOf(t);
    if (!b) return;
    SetBuffFlags(t, 0);
    for (int i = 0; i < 32; ++i) { b->dur[i] = 0; b->power[i] = 0; }
}

// updateUnitBuffs_4FF620 — updateUnitsCCC(적분 뒤) 에서 유닛마다
void Server::UpdateUnitBuffs(const Target& t, std::vector<OutPacket>& out) {
    Buffs* b = BuffsOf(t);
    if (!b || b->bits == 0) return;
    for (int v = 0; v < 32; ++v) {
        b = BuffsOf(t);
        if (!b) return;
        if (!b->Has(v)) continue;
        // 16(DEATH) 이면 남은 틱 % 30 == 29 마다 소리 26 → 10
        if (b->dur[v] == 0) continue;
        --b->dur[v];
        if (b->dur[v] != 0) continue;
        if (v == 7) {
            // ETHEREAL — 깃발 &~0x40(NO_COLLIDE) ⚠ 유닛 깃발 칸이 없어 효과 묶음(4-b)에서
        } else if (v == 16) {
            // DEATH — +520 = 0 · +524 = 13 · unitDamageClear_4EE5E0(9999999: 방어·무적 안 봄) · 소리 779(10) · 사람이면 제거전 죽음·교훈(8)
            if (!t.addr.empty()) {
                Session& s = sessions_.at(t.addr);
                s.lastHit = Session::LastHit{};
                s.lastDamageType = 13;
                const Session::LastHit none;
                Damage(t.addr, 9999999, out, std::string(), &none, /*ignoreInvuln=*/true);
            } else {
                KillPet(static_cast<size_t>(t.pet), out);   // 몬스터 죽음 함수(monsterCallDieFn) — 순번이 바뀌므로 여기서 끝
                return;
            }
        }
        BuffOff(t, v);
        b = BuffsOf(t);
        if (b) b->power[v] = 0;
    }
    // 9(HASTED) 면 SpeedCur × 1.25(4-b3) — 사람만(소환수 걸음은 AI 6)
    b = BuffsOf(t);
    if (b && b->Has(9) && !t.addr.empty()) sessions_.at(t.addr).speedCur *= 1.25f;
}

// nox_xxx_playerConfusedGetDirection_4F7A40(`GAME4.c:1187`) — (프레임 + 번호) % 40 를 0~20 삼각파로 · (세기 + 3) × (파 − 10) 만큼 흔들어 0~255
uint8_t Server::ConfusedDirection(const Session& s, uint32_t frame) const {
    const int v1 = s.facing;
    const int v2 = s.buffs.power[3];
    int v3 = static_cast<int>((frame + static_cast<uint32_t>(s.netCode)) % 0x28u);
    if (v3 > 20) v3 = 40 - v3;
    int v4 = (v2 + 3) * (v3 - 10) + v1;
    if (v4 < 0) v4 += static_cast<int>((static_cast<unsigned>(255 - v4) >> 8) << 8);
    if (v4 >= 256) v4 += -256 * static_cast<int>(static_cast<unsigned>(v4) >> 8);
    return static_cast<uint8_t>(v4);
}

// CanSee(server/object.go:76) — obj·targ 버프만 본다(시야선은 부르는 쪽 MapTraceVision)
bool Server::CanSeeBuffs(const Buffs* obj, const Buffs* targ, double targVelX, double targVelY, int flags) {
    if (obj && obj->Has(2)) return false;                                    // BLINDED
    if (!(flags & 0x1) && targ && targ->Has(0)) {                            // INVISIBLE
        const bool infra = obj && obj->Has(21);                              // 퀘스트 Hecubah/Necromancer 갈래는 7
        // ⚠ 픽시(주인 적외선) 갈래는 4-c
        if (!infra && std::fabs(targVelX) <= 6.0 && std::fabs(targVelY) <= 6.0) return false;
    }
    // ⚠ 폭탄병(Bomber) ↔ 살금살금(TreadLightly) 능력은 7
    return true;
}

// netSendFxAllCli_523030 — 자리가 화면(반폭 640·반높이 480 + 50) 안인 사람에게 Kind1
// ★ 조각 D 정정(09-16) — **여기가 FX 를 세는 유일한 자리다.**
//   전에는 `++fxSent_` 가 부르는 쪽 **열 곳 중 둘**에만 붙어 있어, 덫 연기 56건이
//   나갔는데도 로그가 「FX 0」으로 읽혔다. 반쪽 계수기는 **없는 것보다 나쁘다** —
//   0 을 「안 보냈다」로 읽게 만든다. 세는 것은 **받는 사람 수가 아니라 보낸 건수**다.
void Server::SendFxAllCli(float x, float y, const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    ++fxSent_;
    for (auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame) continue;
        if (std::fabs(x - o.posX) < 640.0 + 50.0 && std::fabs(y - o.posY) < 480.0 + 50.0)
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, n, out);
    }
}

// sub_4E7540 — 사람이 사람에게(서로 다름) 걸면 대상 +3600=1 · +3604=건 이 번호 · +3608=지금
void Server::MarkAssist(const std::string& orig, const Target& targ) {
    if (orig.empty() || targ.addr.empty() || orig == targ.addr) return;
    auto o = sessions_.find(orig);
    auto t = sessions_.find(targ.addr);
    if (o == sessions_.end() || t == sessions_.end()) return;
    t->second.assistSet = true;
    t->second.assistBy = orig;
    t->second.assistFrame = frame_;
}

// castBuffSpell(spells_buffs.go:25)
int Server::CastBuffSpell(uint8_t spell, int enc, int lvl, const Target& targ, const BuffConf& opts, std::vector<OutPacket>& out) {
    (void)spell;
    Buffs* b = BuffsOf(targ);
    if (!b) return 0;
    if (opts.once && b->Has(enc)) return 0;
    int dur = opts.dur;
    if (!opts.durOpt.empty()) dur = static_cast<int>(BalanceFloat(opts.durOpt));
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!opts.durOptMulQuest.empty() && quest) dur = static_cast<int>(static_cast<double>(dur) * BalanceFloat(opts.durOptMulQuest));
    if (opts.durInSec) dur *= static_cast<int>(kServerTickRate);
    if (opts.durLevelMul) dur *= lvl;
    if (opts.defensive && !targ.addr.empty()) DurCancelOffensiveFor(targ.addr, out);
    ApplyEnchant(targ, enc, dur, lvl);
    if (!opts.orig.empty() && opts.offensive) MarkAssist(opts.orig, targ);
    if (opts.pointFx != 0) {                                  // netSendPointFx_522FF0 — `<fx> <x2> <y2>` 화면 안 사람
        float px = 0, py = 0;
        if (!targ.addr.empty()) { px = static_cast<float>(sessions_.at(targ.addr).posX); py = static_cast<float>(sessions_.at(targ.addr).posY); }
        else { px = static_cast<float>(pets_[static_cast<size_t>(targ.pet)].x); py = static_cast<float>(pets_[static_cast<size_t>(targ.pet)].y); }
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { opts.pointFx, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8), static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    }
    return 1;
}

// ── 묶음 4-a3 지속 주문 틀 ─────────────────────────────────────────────
//   정답지 server/spells_dur.go(DurSpell 칸 · 목록 · 취소) + spells_dur.go(New · 틱 · 지우기) + GAME4.c:3369~3525
//   ⚠ 소리(On/Cast/Off)는 10 · 미니맵 표시 풀기(417470)는 미니맵 묶음 · 몬스터 주인(MonsterCancelDurSpell)은 7 · 글리프 주인은 4-e

static bool SpellIsSummon(uint8_t sp) { return sp >= 75 && sp <= 114; }   // SPELL_SUMMON_BAT ~ SPELL_SUMMON_URCHIN_SHAMAN

// spellsDuration.New(spells_dur.go:88)
bool Server::DurNew(uint8_t spell, const std::string& a2, const std::string& obj3, bool u3Glyph, const SpellArg& arg, int lvl,
                    const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out) {
    // u2 == nil || u2 죽음·지워짐 && u3 != nil && u3 글리프 아님 → false (u3 는 늘 있다 — 시전자 또는 날아가는 것)
    auto u2It = sessions_.find(obj3);
    if (obj3.empty() || u2It == sessions_.end() || !u2It->second.inGame) return false;
    const Session& u2 = u2It->second;
    if (u2.respawnAtFrame != 0 && !u3Glyph) return false;
    if ((spell == 59 || spell == 43) && DurActive(spell, obj3)) return true;   // 플라스마·연쇄 번개는 이미 있으면 새로 안 만듦
    DurCancelFor(spell, obj3, out);
    DurOnNewSpell(out);
    DurSpell d;
    d.id = ++durLastId_;
    d.spell = spell;
    d.level = lvl;
    d.obj12 = a2;
    d.caster = obj3;
    d.flag20 = u3Glyph;                                      // 글리프면 Obj24·Pos = 글리프 자리(4-e)
    d.posX = static_cast<float>(u2.posX);
    d.posY = static_cast<float>(u2.posY);
    d.target = arg.obj;
    d.pos2X = arg.x;
    d.pos2Y = arg.y;
    d.frame60 = frame_;
    d.frame64 = frame_;
    d.frame68 = dt + frame_;
    d.fns = fns;
    durSpells_.push_front(d);                                // Add — 머리에
    // 소리 — 겨눔(0x4)이면 On · 아니면 Cast(10)
    // 10-f PlaySound: on apply, TARGETED(0x4) -> OnSound else CastSound, at the target (spells_dur.go:131~138)
    PlaySoundAt(SpellSoundOn(spell), static_cast<float>(u2.posX), static_cast<float>(u2.posY), 0, 0, std::string(), Target{ u2.addr, -1 });
    DurSpell& p = durSpells_.front();
    if (fns.create == nullptr || (this->*fns.create)(p, out) == 0) return true;
    DurCancel(p, out);
    return false;
}

// onNewSpell — 취소 깃발 붙은 것을 전부 지운다
void Server::DurOnNewSpell(std::vector<OutPacket>& out) {
    for (auto it = durSpells_.begin(); it != durSpells_.end();) {
        auto next = std::next(it);
        if (it->flags88 & 1) DurDestroy(it, out);
        it = next;
    }
}

// destroyDurSpell(spells_dur.go:39)
void Server::DurDestroy(std::list<DurSpell>::iterator it, std::vector<OutPacket>& out) {
    // 10-f PlaySound: on expiry, OffSound at the caster (spells_dur.go:39~43)
    {
        auto ci = sessions_.find(it->caster);
        if (!it->caster.empty() && ci != sessions_.end())
            PlaySoundAt(SpellSoundOff(it->spell), static_cast<float>(ci->second.posX),
                        static_cast<float>(ci->second.posY), 0, 0, std::string(), Target{ it->caster, -1 });   // ★ 09-29 B6 — 출처 = 시전자
        // ★ 09-29 B7 — 몬스터 시전자면 그 자리
        const int pi = it->casterPet ? PetIndexByNet(it->casterPet) : -1;
        if (pi >= 0)
            PlaySoundAt(SpellSoundOff(it->spell), static_cast<float>(pets_[static_cast<size_t>(pi)].x),
                        static_cast<float>(pets_[static_cast<size_t>(pi)].y), 0, 0, std::string(), Target{ std::string(), pi });
    }
    if (it->fns.destroy) (this->*(it->fns.destroy))(*it, out);
    auto c = sessions_.find(it->caster);
    if (!it->caster.empty() && c != sessions_.end() && c->second.inGame) {
        // 워리어가 광전사 돌진 중이 아니면 상태 13 ⚠ 능력(광전사)은 묶음 7 — 지금은 늘 13
        PlayerSetState(c->second, kPlayerStateIdle, frame_);
    }
    durSpells_.erase(it);                                    // Unlink · FreeRecursive(하위 포함)
}

// CancelSpell(server/spells_dur.go:181) = nox_xxx_spellCancelSpellDo_4FE9D0
void Server::DurCancel(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (!d.caster.empty() && c != sessions_.end() && c->second.inGame)
        ReportSpellStat(c->second, d.spell, (d.spell == 43) ? 0 : 15, out);
    if (d.spell == 43) {
        for (const auto& sub : d.sub108) if (!sub.target.none()) NetStopRaySpell(sub, sub.target, out);
    } else if (!d.target.none()) {
        NetStopRaySpell(d, d.target, out);
    }
    d.flags88 |= 1;
}

// CancelFor(server/spells_dur.go:169)
void Server::DurCancelFor(uint8_t spell, const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) {
        if ((d.spell == spell && d.caster == obj) || (SpellIsSummon(spell) && SpellIsSummon(d.spell) && d.caster == obj))
            DurCancel(d, out);
    }
}

// CancelOffensiveFor(server/spells_dur.go:159)
void Server::DurCancelOffensiveFor(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_)
        if (d.caster == obj && (SpellFlags(d.spell) & 0x20)) DurCancel(d, out);
}

// Sub4FEE50 — 안 취소된 · 글리프 아닌 · 그 주문 · 그 주인
bool Server::DurActive(uint8_t spell, const std::string& obj) const {
    for (const auto& d : durSpells_)
        if (!d.flag20 && d.spell == spell && d.caster == obj && !(d.flags88 & 1)) return true;
    return false;
}

// nox_xxx_playerCancelSpells_4FEAE0 — 그 주인의 것 전부
void Server::PlayerCancelSpells(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) if (d.caster == obj) DurCancel(d, out);
}

// nox_xxx_cancelAllSpells_4FEE90 — 걷기·뛰기 입력이면 붙들고 있는 주문(번개 24 · 연쇄 번개 43 · 큰 치유 35 · 생명 흘리기 8 · 마나 흡수 22 · 플라스마 59 · 타원 방패 67)
void Server::CancelChannelSpells(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) {
        if (d.caster != obj) continue;
        const uint8_t v = d.spell;
        if (v == 24 || v == 43 || v == 35 || v == 8 || v == 22 || v == 59 || v == 67) DurCancel(d, out);
    }
}

// sub_4FF2D0 — 안 취소된 · 그 주문 · 대상이 그것
const Server::DurSpell* Server::DurFindByTarget(uint8_t spell, const Target& target) const {
    for (const auto& d : durSpells_) {
        if ((d.flags88 & 1) || d.spell != spell || d.target.none()) continue;
        if (d.target.addr == target.addr && d.target.pet == target.pet) return &d;
    }
    return nullptr;
}

// NetStopRaySpell(server/spells.go:458) — `9e <갈래> <값> <u1 번호2> <u2 번호2>` Packet1 모두
void Server::NetStopRaySpell(const DurSpell& d, const Target& who, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool petCaster = d.casterPet != 0;                  // ★ 09-29 B7 — 몬스터 시전자(번호 그대로) — 빔은 전기 광선뿐
    if (petCaster && d.spell != 24) return;
    if (!petCaster && (d.caster.empty() || c == sessions_.end() || !c->second.inGame)) return;
    auto netOf = [&](const Target& t) -> uint16_t {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            return (it != sessions_.end() && it->second.inGame) ? it->second.netCode : 0;
        }
        return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? pets_[static_cast<size_t>(t.pet)].netCode : 0;
    };
    if (who.none() || netOf(who) == 0) return;
    uint8_t typ = 0, val = 0;
    uint16_t n1 = !d.fromUnit.none() ? netOf(d.fromUnit) : (petCaster ? d.casterPet : c->second.netCode);
    uint16_t n2 = netOf(who);
    switch (d.spell) {
        case 43:                                             // 연쇄 번개 — 하위마다
            for (const auto& sub : d.sub108) NetStopRaySpell(sub, sub.target, out);
            return;
        case 59: typ = 8; val = c->second.facingPrev; break;             // 플라스마 — 주인 Direction1
        case 9: typ = 9; val = static_cast<uint8_t>(d.level); break;     // 매혹
        case 7: typ = 10; val = static_cast<uint8_t>(d.level); break;    // 연쇄 번개 조각
        case 22: typ = 12; val = static_cast<uint8_t>(d.level); break;   // 마나 흡수
        case 24: typ = 11; val = static_cast<uint8_t>(d.level); break;   // 번개
        case 35:                                             // 큰 치유 — 자기에게면 없음 · 받는 쪽이 앞
            if (!d.target.addr.empty() && d.target.addr == d.caster) return;
            typ = 13; val = static_cast<uint8_t>(d.level);
            std::swap(n1, n2);
            break;
        default: return;
    }
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), typ, val, static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    ++fxSent_;                                             // 조각 D — FX 는 건수로 센다
    // 미니맵 표시 풀기(Nox_xxx_netUnmarkMinimapSpec_417470 · 2) — 미니맵 표시 묶음
}

// spellCastByPlayer(spells_dur.go:62) — 틱마다 castByBook 다음
void Server::SpellCastByPlayer(uint32_t frame, std::vector<OutPacket>& out) {
    auto alive = [&](const std::string& a) {
        auto s = sessions_.find(a);
        return s != sessions_.end() && s->second.inGame;
    };
    for (auto it = durSpells_.begin(); it != durSpells_.end();) {
        auto next = std::next(it);
        DurSpell& d = *it;
        if (d.flags88 & 1) { DurDestroy(it, out); it = next; continue; }
        if (!d.caster.empty() && (!alive(d.caster) || sessions_.at(d.caster).respawnAtFrame != 0)) d.caster.clear();   // DEAD|DESTROYED
        if (!d.obj12.empty() && !alive(d.obj12)) d.obj12.clear();                                                      // DESTROYED
        // ★ 09-29 B7 — 몬스터 시전자 — 없어졌거나 죽었으면 시전자 없음
        if (d.casterPet != 0) { const int pi = PetIndexByNet(d.casterPet); if (pi < 0 || pets_[static_cast<size_t>(pi)].health == 0) d.casterPet = 0; }
        if (d.caster.empty() && d.casterPet == 0 && !d.flag20) { DurCancel(d, out); it = next; continue; }
        if ((d.frame68 != d.frame60 && d.frame68 <= frame) || (d.fns.update && (this->*d.fns.update)(d, out) != 0))
            DurCancel(d, out);
        it = next;
    }
}

// 시험용 create · update · destroy
int Server::DebugDurCreate(DurSpell& d, std::vector<OutPacket>& out) { (void)d; (void)out; return debugDurCreateRet_; }
int Server::DebugDurUpdate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)d; (void)out;
    ++debugDurUpdates_;
    return (debugDurUpdateStopAt_ > 0 && debugDurUpdates_ >= debugDurUpdateStopAt_) ? 1 : 0;
}
void Server::DebugDurDestroyFn(DurSpell& d, std::vector<OutPacket>& out) { (void)d; (void)out; ++debugDurDestroys_; }
bool Server::DebugDurNew(const std::string& caster, uint8_t spell, uint32_t dt, int createRet, int updateStopAt, bool withDestroy,
                         const Target& target, std::vector<OutPacket>& out) {
    if (sessions_.find(caster) == sessions_.end()) return false;
    DurFns f;
    if (createRet >= 0) { debugDurCreateRet_ = createRet; f.create = &Server::DebugDurCreate; }
    if (updateStopAt >= 0) { debugDurUpdateStopAt_ = updateStopAt; debugDurUpdates_ = 0; f.update = &Server::DebugDurUpdate; }
    if (withDestroy) f.destroy = &Server::DebugDurDestroyFn;
    SpellArg arg;
    arg.obj = target;
    return DurNew(spell, caster, caster, false, arg, 3, f, dt, out);
}

// 날아가는 것들 한 틱 — updateUnitsCallUpdate(server_update.go:72) 의 투사체 몫. 사람 적분 **앞**에서 부른다
void Server::UpdateMissiles(uint32_t frame, std::vector<OutPacket>& out) {
    for (size_t i = 0; i < missiles_.size();) {
        MissileObj& m = missiles_[i];
        // 만든 틱에는 안 돈다(ObjectsAddPending 뒤) — 우리 틀은 시전 목록(SpellCastByBook)이 이 뒤에 돌아 저절로 그렇다
        // ①-a ProjectileUpdate 53AC10(`object_update.go:938`) — 불덩이 같은 **보통 투사체**의 주기 함수.
        //   하는 일이 하나뿐이다: 만든 뒤 **40틱**이 지나면 제 닿음 함수를 대상 없이(0,0) 부르고 지운다.
        //   ⚠ 아래 ①-b(SpellProjectileUpdate)의 수명 15/45·대상 찾기·따라가는 힘은 **Magic 전용**이다 —
        //     불덩이에 씌우면 15틱 만에 사라져 멀리 못 간다(실측: 세기 1 불덩이가 280 을 못 갔다).
        if (m.collideKind == 1) {
            if (frame - m.createdFrame > 40u) {
                FireballExplode(m, Target{}, out);
                DestroyMissile(i, out);
                continue;
            }
        } else if (m.collideKind == 5) {
            // ①-f 묶음 9-b 화살 — `MercArcherArrow` 도 `UPDATE = ProjectileUpdate` 다(40틱).
            //   정답지는 때가 되면 대상 없이 닿음 함수를 부르고, 그쪽은 맵에 피해를 주고 사라진다.
            //   ⚠ 맵 피해(`damageToMap_534BC0`)는 아직 안 옷겼다 — 지금은 그냥 지운다
            if (frame - m.createdFrame > 40u) { ++arrowGoneAge_; DestroyMissile(i, out); continue; }
        } else if (m.collideKind == 6) {
            // ★ 09-29 사람이 쏜 화살 — `UPDATE = ProjectileUpdate`(40틱) 뒤 닿음 함수(ArrowCollide)를 대상 없이 부른다.
            //   그 갈래는 벽 기록(`sub_537760`)이 서 있으면 그 칸에 맵 피해를 주고 지우는데, 기록은 닿음 함수를 부를 때마다
            //   지워지므로(`sub_537770` `GAME4_3.c:3377·3384`) 수명으로 부를 때는 비어 있다 — 그냥 지운다
            if (frame - m.createdFrame > 40u) { ++arrowHit_[static_cast<size_t>(ArrowHit::Age)]; DestroyMissile(i, out); continue; }
        } else if (m.collideKind == 3) {
            // ①-d 묶음 6-g(3b) — `ImpShot` 도 `UPDATE = ProjectileUpdate` 다(불덩이와 같은 40틱).
            //   때가 되면 **대상 없이** 제 닿음 함수를 부르는데, 그쪽은 맵에 피해를 주고 사라진다.
            //   ⚠ 맵 피해(`damageToMap_534BC0`)는 아직 안 옮겼다 — 지금은 그냥 지운다
            if (frame - m.createdFrame > 40u) { DestroyMissile(i, out); continue; }
        } else if (m.collideKind == 4) {
            // ①-e 묶음 7-f 작살 — `UPDATE = HarpoonUpdate`(`updateHarpoon_54F380`).
            //   ★ 아래 ①-b(주문 투사체)로 떨어지면 **15틱 만에 사라지고** 엉뚱한 상대를 찾아 휜다.
            //     여기서는 **그냥 날게** 둔다 — 거리·수명·시야 관문과 끌기는 다음 조각(7-g)이다
        } else if (m.collideKind == 2) {
            // ①-c updatePixie_53CD20 — 여기서는 **수명만** 본다.
            //   ⚠ 적 찾기·떼 짓기·주인 따라가기·되돌아오기는 유닛 움직임 묶음(6·7)이다.
            if (m.expireFrame != 0 && frame > m.expireFrame) { DestroyMissile(i, out); continue; }
        } else
        // ①-b SpellProjectileUpdate 53B940(`GAME4_3.c:5764`)
        {
            const uint32_t life = m.ud1.none() ? 15u : 45u;   // ⭕ gamedata ARENA UnTargeted/TargetedSpellLifetime
            if (frame - m.createdFrame > life) {
                const int16_t fx = static_cast<int16_t>(m.posX), fy = static_cast<int16_t>(m.posY);
                const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_RICOCHET), static_cast<uint8_t>(fx), static_cast<uint8_t>(fx >> 8),
                                         static_cast<uint8_t>(fy), static_cast<uint8_t>(fy >> 8) };
                for (auto& kv : sessions_) {                  // netSendFxAllCli_523030 — 화면(반폭+50) 안인 사람만 · Kind1
                    const Session& o = kv.second;
                    if (!o.inGame) continue;
                    if (std::fabs(m.posX - o.posX) < 640.0 + 50.0 && std::fabs(m.posY - o.posY) < 480.0 + 50.0)
                        Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
                }
                DestroyMissile(i, out);
                continue;
            }
            auto alive = [&](const std::string& a) {
                auto it = sessions_.find(a);
                return it != sessions_.end() && it->second.inGame;
            };
            if (!m.ud0.empty() && !alive(m.ud0)) m.ud0.clear();
            if (!m.ud2.empty() && !alive(m.ud2)) m.ud2.clear();
            if (!m.ud1.addr.empty()) {
                auto it = sessions_.find(m.ud1.addr);
                if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) m.ud1 = Target{};   // 0x20 · 0x8000
            } else if (m.ud1.pet >= 0 && static_cast<size_t>(m.ud1.pet) >= pets_.size()) {
                m.ud1 = Target{};
            }
            if (m.ud1.none()) {
                if (frame - m.frame136 > static_cast<uint32_t>(kServerTickRate >> 2) || m.frame136 == m.createdFrame) {
                    auto ownIt = sessions_.find(m.ud0);
                    m.ud1 = SpellFlySearchTarget(false, 0, 0, m.posX, m.posY, m.owner, std::string(), SpellFlags(m.spell), 600.0f,
                                                 ownIt != sessions_.end() ? &ownIt->second : nullptr, &m.buffs);
                    m.frame136 = frame;
                }
            }
            if (!m.ud1.none()) {
                float tx = 0, ty = 0;
                if (!m.ud1.addr.empty()) { tx = static_cast<float>(sessions_.at(m.ud1.addr).posX); ty = static_cast<float>(sessions_.at(m.ud1.addr).posY); }
                else { tx = static_cast<float>(pets_[static_cast<size_t>(m.ud1.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(m.ud1.pet)].y); }
                const float v18 = tx - m.posX;
                const float v17 = ty - m.posY;
                const float v11 = static_cast<float>(std::sqrt(static_cast<double>(v17) * v17 + static_cast<double>(v18) * v18) + 0.10000000149011612);
                m.damping = 0.89999998f;                     // 1063675494
                m.forceX = v18 * m.speed / v11;
                m.forceY = v17 * m.speed / v11;
            } else {
                const float v13 = static_cast<float>(std::sqrt(static_cast<double>(m.velX) * m.velX + static_cast<double>(m.velY) * m.velY) + 0.10000000149011612);
                m.damping = 0.89999998f;
                m.forceX = m.speed * m.velX / v13;
                m.forceY = m.speed * m.velY / v13;
            }
        }
        // ② 떨어짐(updateFallLogic) — 높이는 묶음 5·9
        // ③ 움직임 sub_51B810(`GAME4_1.c:4799`)
        m.posX = m.newX; m.posY = m.newY;
        m.velX = (m.forceX + m.velX) * m.damping;
        m.velY = (m.forceY + m.velY) * m.damping;
        // ★ 진단 `--arrow-freeze` — 화살을 **난 자리에 세워 둔다**(안 움직이고 안 부딪힌다).
        //   까닭 — 「프레임이 그림을 바꾸나」를 화면으로 닫으려면 **같은 자리에서 프레임만**
        //   달라진 두 장이 있어야 한다. 날아가면 배경이 달라져 픽셀 대조가 안 된다.
        //   ⚠ 진단 전용이다.
        //   ⚠⚠ **한계 — 이걸로 「프레임이 화면을 바꾸나」를 못 닫는다**(09-18 실측).
        //     멈추는 것은 **속도뿐**이고 수명은 그대로 흘러서, 덫이 30틱마다 새로 쏘고
        //     40틱이면 사라지는 순환이 계속된다 ⇒ 화면이 프레임과 **무관하게** 계속 바뀐다.
        //     연속 캡처의 인접 차이를 재 보면 프레임을 번갈아 보낸 판(중앙 656)과
        //     0 으로 못 박은 대조군(중앙 605)이 **구별되지 않는다.**
        //     닫으려면 수명도 멈추거나, 덫을 한 번만 쏘게 해야 한다.
        if (arrowFreeze_ && IsArrowType(m.type)) { m.velX = m.velY = 0.0f; }
        m.newX += m.velX; m.newY += m.velY;
        // ④ 선 추적 맞힘 projectileTraceHit_537850(`GAME4_3.c:3386`) → 충돌 함수 둘(sub_537770)
        bool erased = false;
        // ★ 09-24 사람 판 — 「더는 안 부딪힘」(FlagNoCollide 0x40)이 선 것은 맞힘을 **통째로 건너뛴다**(움직임·넘김은 한다).
        //   꽂힌 작살 화살이 이 깃발을 켜는데(`HarpoonCollide`) 거르는 자리가 대상 쪽(`passFlags`)뿐이라 **매 틱 다시 맞았다** —
        //   사람 판 hj1: 5발에 「걸림」 91 · 그때마다 피해 1 과 맞는 소리. 정답지는 NO_COLLIDE 인 것을 충돌 짝에서 아예 뺀다(`sub_5481C0`)
        if (!(m.flags & 0x40)) {
            enum { HitNone, HitPlayer, HitPet, HitObstacle } hk = HitNone;
            std::string hAddr; int hIdx = -1;
            // ★★ 2026-09-16 — **닿은 것을 셋으로 가른다.** 전에는 `HitPlayer` 인가 아닌가로만
            //   갈라서, `HitObstacle` 의 **장애물 색인이 소환수 색인 자리로** 들어갔다
            //   (`hIdx` 는 `obstacles_` 의 자리인데 `Target::pet` 은 `pets_` 의 자리다).
            //   기둥에 맞은 화살이 **소환수 N번을 때렸다.** 실측 — 신전 판에서
            //   피해 14건 중 **13건이 크리처 갈래로 갔는데 크리처는 0마리**였다.
            //   소환수가 있는 판(퀘스트·코옵)이면 **엉뚱한 놈이 맞는다.**
            //   ⚠ 장애물은 유닛이 아니므로 **빈 대상**을 준다 — `HurtTarget` 이 그 자리에서 되돌아가고,
            //     `HarpoonCollide`·`FireballExplode` 는 빈 대상을 이미 「벽에 맞음」으로 다룬다.
            auto HitTargetOf = [](int kind, const std::string& a, int idx) -> Target {
                if (kind == HitPlayer) return Target{ a, -1 };
                if (kind == HitPet)    return Target{ std::string(), idx };
                return Target{};                     // 장애물·없음 — 유닛이 아니다
            };
            float hx = 0, hy = 0;                            // 맞은 것 자리
            float onx = 0, ony = 0;                          // 오브젝트 법선(점 − 그 자리)
            // sub_54E730(날아가는 것, 대상) — 깃발 거르기. 대상 NO_COLLIDE(0x40) 는 안 맞음 · MISSILE_HIT(0x80) 는 늘 맞음 ·
            //   BELOW|SHORT(0x11) 와 AIRBORNE(0x4000) 은 서로 안 맞음 · 어느 쪽이든 NO_COLLIDE_OWNER(0x400) 면 같은 편(주인 사슬)은 안 맞음
            //   ⚠ 몬스터가 쏜 투사체 ↔ 몬스터 갈래(+508 주인 클래스 2)는 몬스터 시전(묶음 7) · 충돌 함수 없음은 hasCollide 로
            auto passFlags = [&](uint32_t of, bool sameTeam) {
                if (of & 0x40) return false;
                if (of & 0x80) return true;
                if ((m.flags & 0x11) && (of & 0x4000)) return false;
                if ((of & 0x11) && (m.flags & 0x4000)) return false;
                if (((m.flags | of) & 0x400) && sameTeam) return false;
                return true;
            };
            auto ownIt = sessions_.find(m.owner);
            const uint8_t ownTeam = (ownIt != sessions_.end()) ? ownIt->second.team : 0;
            // sub_54E810 — 점이 모양 안에 드는 것(원 거리² ≤ R² · 상자 · 문 선분은 9) · 마지막 것
            auto pointHit = [&](float qx, float qy) {
                bool any = false;
                for (const auto& kv : sessions_) {
                    const Session& o = kv.second;
                    if (!o.inGame || o.observer) continue;   // 관전 NO_COLLIDE(0x40)
                    // NewPlayer FLAGS NULL · ★ 3-a — 떠 있으면 0x4000(밑에 깔린·낮은 날아가는 것에 안 맞는다)
                    if (!passFlags(PlayerObjFlags(o), o.addr == m.owner || (ownTeam != 0 && o.team == ownTeam))) continue;
                    const float dx = static_cast<float>(o.posX) - qx, dy = static_cast<float>(o.posY) - qy;
                    if (dy * dy + dx * dx > static_cast<float>(kBodyRadius * kBodyRadius)) continue;
                    hk = HitPlayer; hAddr = o.addr; hx = static_cast<float>(o.posX); hy = static_cast<float>(o.posY); any = true;
                }
                for (size_t k = 0; k < pets_.size(); ++k) {
                    // ★ **쏜 것 자신은 건너뛴다** — 발사체는 그 자리에서 생기므로 안 그러면
                    //   나가자마자 제 몸에 맞는다. 정답지는 부모 사슬을 타고 올라가 가른다
                    //   (`findParentChainPlayer_4EC580`)
                    if (m.fromPet >= 0 && static_cast<int>(k) == m.fromPet) continue;
                    const auto tb = typeBodies_.find(pets_[k].type);
                    if (tb == typeBodies_.end() || !tb->second.hasCollide || tb->second.shape != 2) continue;
                    // ★ 10-01 시체 — 죽음 덧칠(0x80 끔 · 31 의 SHORT 0x10 ↔ 떠 있는 발사체 0x4000 은 안 맞음 · `sub_54E730` `GAME5.c:5701~5729` 는 DEAD 를 안 봄)
                    if (!passFlags(PetObjFlags(pets_[k]), !pets_[k].owner.empty() && pets_[k].owner == m.owner)) continue;
                    const float dx = static_cast<float>(pets_[k].x) - qx, dy = static_cast<float>(pets_[k].y) - qy;
                    if (dy * dy + dx * dx > tb->second.r * tb->second.r) continue;
                    hk = HitPet; hIdx = static_cast<int>(k); hx = static_cast<float>(pets_[k].x); hy = static_cast<float>(pets_[k].y); any = true;
                }
                for (size_t k = 0; k < obstacles_.size(); ++k) {
                    const PhysBody& o = obstacles_[k];
                    if (!passFlags(o.flags, false)) continue;
                    if (!BodyContains(o, qx, qy)) continue;
                    hk = HitObstacle; hIdx = static_cast<int>(k); hx = o.x; hy = o.y; any = true;
                }
                return any;
            };
            const float ddx = m.newX - m.posX, ddy = m.newY - m.posY;
            const double d2 = static_cast<double>(ddy) * ddy + static_cast<double>(ddx) * ddx;
            if (d2 <= 36.0) {
                if (pointHit(m.newX, m.newY)) { onx = m.posX - hx; ony = m.posY - hy; }
            } else {
                const int steps = static_cast<int>(std::sqrt(d2 * 0.027777778)) + 1;
                const float stx = ddx / static_cast<float>(steps), sty = ddy / static_cast<float>(steps);
                float lx = m.posX, ly = m.posY, qx = m.posX, qy = m.posY;
                for (int k = 0; k < steps; ++k) {
                    qx += stx; qy += sty;
                    if (pointHit(qx, qy)) { onx = lx - hx; ony = ly - hy; break; }
                    lx = qx; ly = qy;
                }
            }
            TraceHit th;
            bool wall = false;
            float wnx = 0, wny = 0;
            if (!MapTraceRay(m.posX, m.posY, m.newX, m.newY, 0x5, &th)) {
                wall = WallNormal(WallShapeAt(th.gx, th.gy), WallTriSide(m.posX, m.posY, th.gx, th.gy),
                                  WallQuadBits(th.x, th.y), wnx, wny);
                m.newX = m.posX; m.newY = m.posY;
            }
            bool objHit = (hk != HitNone);
            bool wallHit = wall;
            if (objHit && wall) {
                const double od = (m.posX - hx) * (m.posX - hx) + (m.posY - hy) * (m.posY - hy);
                const double wd = (m.posX - th.x) * (m.posX - th.x) + (m.posY - th.y) * (m.posY - th.y);
                if (od < wd) wallHit = false; else objHit = false;
            }
            // SpellProjectileCollide 4E9500(`GAME3_3.c:3925`) — 날아가는 것 쪽. 맞은 것 쪽 충돌 함수(PlayerCollide 등)는 광전사·버프 갈래뿐이라 4-a4·7
            auto reflectFrom = [&](float ox, float oy) {     // projectileReflect_4E0A70
                const double v3 = static_cast<double>(m.posX) - ox, v4 = static_cast<double>(m.posY) - oy;
                m.dir1 = static_cast<uint8_t>(m.dir1 + 128);
                const float v13 = static_cast<float>(std::sqrt(v4 * v4 + v3 * v3) + 0.1);
                const double v6 = (v4 * m.velY + v3 * m.velX) / v13;
                const float v9 = static_cast<float>(v6 * v3 / v13);
                const float v10 = static_cast<float>(v6 * v4 / v13);
                const float v11 = static_cast<float>(-v4);
                const double v7 = (static_cast<double>(v11) * m.velX + v3 * m.velY) / v13;
                const float v12 = static_cast<float>(v7 * v11 / v13);
                m.velX = v12 - v9;
                m.velY = static_cast<float>(v3 * v7 / v13) - v10;
                m.newX = m.prevX; m.newY = m.prevY;
            };
            if (wallHit) {
                // 7-f — 작살은 벽에 닿으면 **대상 없이** 제 닿음 함수를 타 조용히 끊긴다.
                // ⚠ `HarpoonCollide` 가 `HarpoonBreak` 을 통해 이미 지웠으면 목록이 줄어 색인 i 가 다음 것을 가리킨다.
                //   그러나 주인을 못 찾는 따위로 **안 지워졌을 수도** 있으므로 여기서 한 번 더 확인한다 —
                //   그러지 않으면 `continue` 가 같은 색인을 다시 보아 **무한 고리**가 된다(돌연변이 HP9 가 드러냈다)
                if (m.collideKind == 4) {
                    const uint16_t nc = m.netCode;
                    HarpoonCollide(m, Target{}, out);
                    if (i < missiles_.size() && missiles_[i].netCode == nc) DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                if (m.collideKind == 5) {                    // 9-b 화살 — 벽에 닿으면 사라진다
                    // ★ 2026-09-18 ⑫ — **왜 사라지는지 갈래를 센다.**
                    //   덫 화살이 「만든 704 · 사라짐 704 · 현재 늘 0」이라 화면에 한 번도 안 그려진다.
                    //   수명은 40틱인데 그 전에 사라지니, 벽인지 다른 것인지 갈라야 한다.
                    ++arrowGoneWall_;
                    if (frame - m.createdFrame <= 1u) ++arrowGoneWallAtBirth_;   // 태어난 자리가 벽 속인가
                    // ★ 수명을 모아 **평균 몇 틱 사나**를 낸다. 「벽이 가까운 게 정상」인지
                    //   「방향이 틀려 바로 처박히는지」를 사람이 가르려면 이 숫자가 있어야 한다.
                    arrowLifeSum_ += (frame - m.createdFrame);
                    ++arrowLifeCnt_;
                    DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                if (m.collideKind == 1) {                    // 불덩이는 벽에서도 터진다(fireballCollide 의 a2 == 0 갈래)
                    FireballExplode(m, Target{}, out);
                    DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                if (m.collideKind == 6) {
                    // ★ 09-29 `collideArrow_4EB490` 의 a2 == 0 갈래(`GAME3_3.c:5257~5266`) — 막은 벽 칸(`sub_537760` = 이 추적이 막힌 칸)에
                    //   화살 피해(종류 11)를 주고 지운다. 때린 것(who)은 화살 — 유닛이 아니다(`damageToMap_534BC0(x, y, 피해, 11, 화살)`)
                    WallHitter who;
                    who.present = true;
                    who.x = m.posX; who.y = m.posY; who.dir = m.dir1;
                    who.material = MaterialOfWire(m.type);
                    who.src = m.owner;
                    DamageToMap(th.gx, th.gy, ArrowDamage(m.owner, m.type), 11, who, out);
                    ++arrowHit_[static_cast<size_t>(ArrowHit::Wall)];
                    DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                // collideReflect_57B810 — 법선 부호 곱으로 속도 성분 맞바꿈
                if (wny * wnx <= 0.0f) std::swap(m.velX, m.velY);
                else { const float v3 = m.velX; m.velX = -m.velY; m.velY = -v3; }
            } else if (objHit) {
                bool dead = false;
                if (hk == HitPlayer) dead = (sessions_.at(hAddr).respawnAtFrame != 0);
                if (!dead) {
                    bool handled = false;
                    if (hk == HitPlayer) {
                        Session& t = sessions_.at(hAddr);
                        auto changeOwner = [&]() { MissileChangeOwner(m, t.addr, frame); };
                        const bool front = (TestTwoPointsAndDirection(static_cast<float>(t.posX), static_cast<float>(t.posY),
                                                                      t.facingPrev, m.prevX, m.prevY) & 1) != 0;
                        if (t.moveState == 16 && front) {            // 방패 올림 앞쪽 — 되받아침(소리 878 → 10)
                            reflectFrom(hx, hy);
                            changeOwner();
                            handled = true;
                        } else if (t.moveState == kPlayerStateIdle && (WeaponEquipOf(t) & 0x400) && front) {
                            reflectFrom(hx, hy);                     // 대검 가만 앞쪽 — 되받아침 + 상태 18~20(소리 890 → 10)
                            changeOwner();
                            PlayerSetState(t, static_cast<uint8_t>(18 + NextRandom(3)), frame);
                            t.animFrame = static_cast<uint8_t>(playerAnimFrames_[PlayerAnimByte(t)][0] - 1);
                        }
                        // LABEL_13 — ⚠ 반전(Inversion)은 든 물건의 마법부여 함수(아이템 5)
                        //   되받아치기 방패(27) 버프 · 앞쪽(날아가는 것 지금 자리 +56) → 주인·대상 맞바꿈(소리 122 → 10) · 되받아침 없이 그대로
                        if (!handled && t.buffs.Has(27) &&
                            (TestTwoPointsAndDirection(static_cast<float>(t.posX), static_cast<float>(t.posY), t.facingPrev, m.posX, m.posY) & 1)) {
                            if (m.collideKind == 1) {
                                // 불덩이는 제 닿음 함수 안에서 **되받아치고 주인을 넘긴다** — 터지지 않는다(소리 122)
                                //   `projectileReflect_4E0A70` → `unitClearOwner_4EC300` → `unitSetOwner_4EC290(맞은 이, 불덩이)`
                                reflectFrom(hx, hy);
                                m.owner = t.addr;
                                m.ud0 = m.ud2 = t.addr;
                            } else {
                                changeOwner();
                            }
                            handled = true;
                        }
                    }
                    // collidePixie_4EA080 — **적일 때만** 인자만큼 피해(종류 11 충격)를 주고 제가 사라진다
                    if (!handled && m.collideKind == 2) {
                        const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                        auto ownIt = sessions_.find(m.owner);
                        const bool enemy = (ownIt != sessions_.end()) && TargetIsEnemy(ownIt->second, tgt);
                        if (enemy) {
                            // ★ 09-26 M1 — a3 = 픽시(클래스 MISSILE) · 전에는 때린 것 없이(nullptr) 넘겨 막기 판정을 아예 안 탔다
                            AttackSrc src;
                            src.x = m.posX; src.y = m.posY;
                            src.byMonsterType = m.type;
                            src.rootIsMonster = MissileRootIsMonster(m);
                            src.missile = true;
                            HurtTarget(tgt, static_cast<int>(m.collideAmount), out, m.owner, 11, &src, HitterMissile(m));   // ★ 4-c3b2 a3 = 픽시(`GAME3_3.c:4349`)
                            DestroyMissile(i, out);
                            erased = true;
                        }
                        handled = true;
                    }
                    // collideProjectileGeneric_4E87B0 — ★ **적·아군을 안 가린다**(픽시와 다르다).
                    //   맞는 것에 **종류 11** 피해. 양은 `ImpShot` 이면 gamedata `ImpShotDamage`,
                    //   아니면 `COLLIDE` 의 인자다(둘 다 5 라 값은 같다)
                    if (!handled && m.collideKind == 3) {
                        const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                        // ⭕ 정답지는 피해를 줄 때 **a2 로 발사체 자신**을 넘긴다
                        //   (`(*(a2+716))(a2, 부모사람, **발사체**, 피해, 11)`) — 죽음 알림 그림이 그것이다
                        AttackSrc src;
                        src.x = m.posX; src.y = m.posY;
                        src.byMonsterType = m.type;
                        // ★ 09-20 — 뿌리가 몬스터인가: **크리처가 쏜 화살**만 그렇다.
                        //   덫 화살은 `ownerPet` 이 −1 이라 뿌리가 몬스터가 아니다
                        src.rootIsMonster = MissileRootIsMonster(m);
                        src.missile = true;                    // ★ 09-26 M1 — a3 = 발사체(클래스 MISSILE) → 막기·되받아치기가 `*(v9+8) & 1` 로 가른다
                        HurtTarget(tgt, static_cast<int>(m.collideAmount), out, m.owner, 11, &src, HitterMissile(m));   // ★ 4-c3b2 a3 = 발사체(`GAME3_3.c:3340`)
                        DestroyMissile(i, out);
                        erased = true;
                        handled = true;
                    }
                    // 7-f — 작살. 꽂히면 화살을 **살려 둔다**(줄이 된다)
                    // `collideMonsterArrow_4EB800` — 맞는 것에 **종류 3**(꺿림) 피해를 주고 사라진다.
                    //   ★ **적·아군을 안 가린다**(갈래 3 과 같다). 양은 `COLLIDE` 의 둘째 인자인데
                    //   화살 덧은 그걸 `ArrowTrapDamage`(25)로 덮어 버린다(`GAME5.c:6432~6435`)
                    if (!handled && m.collideKind == 5) {
                        const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                        AttackSrc src;
                        src.x = m.posX; src.y = m.posY;
                        src.byMonsterType = m.type;
                        // ★ 09-20 — **크리처가 쏜 화살**만 뿌리가 몬스터다. 덫 화살은 `ownerPet` 이 −1 이라
                        //   몬스터에게 적이 된다(⑮). 위 갈래 11 과 같은 셈
                        src.rootIsMonster = MissileRootIsMonster(m);
                        src.missile = true;                    // ★ 09-26 M1 — a3 = 화살(클래스 MISSILE) — 대검이 종류 3 화살도 막는다(`GAME3_3.c:260`)
                        HurtTarget(tgt, static_cast<int>(m.collideAmount), out, m.owner, 3, &src, HitterMissile(m));   // ★ 4-c3b2 a3 = 화살(`GAME3_3.c:5329`) · 몬스터가 쏜 것은 b3
                        DestroyMissile(i, out);
                        erased = true;
                        handled = true;
                    }
                    // ★ 09-29 `nox_xxx_collideArrow_4EB490`(`GAME3_3.c:5197~5309`) — 사람이 쏜 화살(활·석궁)
                    //   · 쏜 이에게 닿으면 아무 일 없음(그대로 날아감 `:5227~5232`) · 죽은 것(+16 & 0x8000)도 그대로(`:5272~5275`)
                    //   · 피해 = 쏜 이 힘으로 `calcBoltDamage`(+0.5 버림) · 종류 11 · 때린 것 = 화살(MISSILE — 막기·되받아치기가 본다)
                    //   · 쇠뇌살이면 — 맞은 것에 체력이 있고 지금 0 이면(죽였으면) 최대가 0 이 아닌 한 **안 지우고 뚫고 간다**(`:5297~5304`)
                    //   · 그 밖은 피해가 안 들어갔으면(피해 함수가 0) 그대로 두고, 들어갔으면 지운다(`:5305~5308`)
                    //   · 유닛 아닌 장애물 — 피해 함수가 DefaultDamage 계열(`nox_xxx_damageDefaultProc_4E0B30` — 버프 26 이 없으면 1)이면 지운다.
                    //     화약(`damageBlackPowder_4E2560` `GAME3_3.c:677~686`)은 종류 1·12·2·0 만 받고 11 이면 0 → 화살이 그대로 지나간다
                    //     ⚠ 장애물 자신의 체력 깎기는 안 옮겼다(다른 날아가는 것 갈래와 같다 — 장애물 체력이 우리에게 없다)
                    //   ⚠ 안 옮긴 것 — 화살 수식 공격 효과(`itemApplyAttackEffect_538840`) · 맞는 쪽 미리 효과(`playerPreAttackEffects_538290`) — 우리 물건에 수식이 없다 ·
                    //     퀘스트 갈래(`:5238~5256` 같은 편 사람 안 때림) · 막을 때의 되받아치기는 앞의 방패(16)·대검(가만) 갈래만 — 막기 상태(18~20) 대검은 되받아치지 않아
                    //     화살이 그대로 남는다(정답지는 피해 함수 안에서 되받아친다 `GAME3_3.c:269~276`)
                    if (!handled && m.collideKind == 6) {
                        auto hit = [&](ArrowHit k) { ++arrowHit_[static_cast<size_t>(k)]; };
                        const bool petDead = (hk == HitPet && pets_[static_cast<size_t>(hIdx)].health == 0);
                        if (hk == HitPlayer && hAddr == m.owner) { hit(ArrowHit::Owner); }
                        else if (petDead) { hit(ArrowHit::Dead); }
                        else if (hk == HitObstacle) {
                            if (obstacles_[static_cast<size_t>(hIdx)].damageFn == kDmgBlackPowder) hit(ArrowHit::NoHurt);
                            else { hit(ArrowHit::Obstacle); DestroyMissile(i, out); erased = true; }
                        }
                        else {
                            const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                            AttackSrc src;
                            src.x = m.posX; src.y = m.posY;
                            src.byMonsterType = m.type;
                            src.rootIsMonster = MissileRootIsMonster(m);
                            src.missile = true;
                            const bool hurt = HurtTarget(tgt, ArrowDamage(m.owner, m.type), out, m.owner, 11, &src, HitterMissile(m));
                            bool keep = false;
                            // ⚠ 종류 번호는 쏠 때 쓴 표(`ProjType`)에서 — 이름표(`NamedType`)엔 ArcherBolt 가 등록돼 있지 않다(시험이 잡음)
                            const ProjTypeInfo* boltPt = ProjType("ArcherBolt");
                            if (boltPt && boltPt->type != 0 && m.type == boltPt->type) {
                                int hp = -1, hpMax = 0;
                                if (hk == HitPlayer) { const Session& t2 = sessions_.at(hAddr); hp = t2.health; hpMax = t2.maxHealth; }
                                else { hp = pets_[static_cast<size_t>(hIdx)].health; hpMax = static_cast<int>(TuningFor(pets_[static_cast<size_t>(hIdx)].type).health); }
                                if (hp == 0) { keep = (hpMax != 0); if (keep) hit(ArrowHit::BoltPass); }
                                else hit(hurt ? ArrowHit::Hurt : ArrowHit::NoHurt);   // 쇠뇌살은 피해 결과와 상관없이 지운다
                            } else if (!hurt) {
                                keep = true;
                                hit(ArrowHit::NoHurt);
                            } else {
                                hit(ArrowHit::Hurt);
                            }
                            if (!keep) { DestroyMissile(i, out); erased = true; }
                        }
                        handled = true;
                    }
                    if (!handled && m.collideKind == 4) {
                        const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                        if (!HarpoonCollide(m, tgt, out)) { erased = true; }
                        handled = true;
                    }
                    if (!handled && m.collideKind == 1) {
                        // 불덩이 — 대상을 안 가린다(맞는 것마다 터진다). 되받아치기 방패(27)는 위에서 이미 handled
                        const Target tgt = HitTargetOf(hk, hAddr, hIdx);
                        FireballExplode(m, tgt, out);
                        DestroyMissile(i, out);
                        erased = true;
                        handled = true;
                    }
                    if (!handled) {
                        const bool isTarget = (hk == HitPlayer && m.ud1.addr == hAddr) || (hk == HitPet && m.ud1.pet == hIdx);
                        if (isTarget) {
                            auto casterIt = sessions_.find(m.ud2);
                            if (casterIt != sessions_.end()) {
                                SpellArg arg;
                                arg.obj = HitTargetOf(hk, hAddr, hIdx);
                                SpellAccept(m.spell, casterIt->second, m.ud0, arg, m.level, out);   // (ud[3], ud[2], ud[0], 날아가는 것, 대상)
                            }
                            DestroyMissile(i, out);
                            erased = true;
                        } else {
                            reflectFrom(hx, hy);
                        }
                    }
                }
                (void)onx; (void)ony;
            }
        }
        if (erased) continue;
        // ⑤ 넘김(server_update.go:80~85)
        missiles_[i].dir1 = missiles_[i].dir2;
        missiles_[i].prevX = missiles_[i].posX; missiles_[i].prevY = missiles_[i].posY;
        missiles_[i].posX = missiles_[i].newX; missiles_[i].posY = missiles_[i].newY;
        missiles_[i].forceX = missiles_[i].forceY = 0;
        ++i;
    }
}

// ★ 09-27 B1 — 직업 기본 장비를 서버 가방에 싣는다.
//   정답지는 기본 장비를 진짜 물건으로 만들어 가방에 넣는다(`playerMakeDefItems_4EF7D0` → `playerRespawnItem_4EF750`
//   → `inventoryServPlace_4F36F0`, `GAME3_3.c:7872~7897·7990~8041`). 우리는 스냅샷 꼬리로 클라에만 보내고
//   (`SnapshotTail.cpp:128~183`) 가방엔 없어서, 입기·벗기·버리기가 그 번호를 못 찾았다.
//   번호·순서는 스냅샷 꼬리와 같다 — 사람 번호 +1 부터 `LoadoutFor` 순서(두 곳이 같은 순서인지는 시험이 본다).
//   ⚠ 종류 틀(무게·수식 갈래·장비 비트)이 없으면 싣지 않는다 — 무게를 짐작으로 채우면 줍기 한도가 틀어진다.
//   ⚠ 안 옮긴 것 — 가방 순서(정답지는 새로 든 것을 맨 앞에 끼운다 · 우리 줍기처럼 뒤에 붙임) · 부활 때 다시 주기
void Server::GiveDefaultGear(Session& s) {
    const Loadout l = LoadoutFor(s.charClass);
    for (int g = 0; g < l.count && g < 4; ++g) {
        const auto it = itemTemplate_.find(l.items[g]);
        if (it == itemTemplate_.end()) { ++gearGiven_[0]; continue; }
        Session::Carried c = it->second;
        c.netCode = static_cast<uint16_t>(s.netCode + 1 + g);
        c.type = l.items[g];
        c.equipped = true;                     // 스냅샷 꼬리가 `60 REPORT_EQUIP` 을 같이 보낸다
        DefaultGearMods(c.type, s.info97, c.mods);   // ★ 09-29 — 평상복 색(수식) — 남에게 겉모습을 알릴 때 싣는다
        s.inventory.push_back(c);
        ++gearGiven_[1];
        s.gearInBag = true;
        // ★ 09-27 B2 — 무기면 무기 칸에. 정답지는 기본 장비를 넣는 길(`inventoryServPlace_4F36F0` → 무기 줍기 `53A720`)에서
        //   손이 비었으면 무기 들기 `53A420` 을 불러 +104 에 둔다(`GAME4_3.c:5102~5104`)
        if (c.isWeapon && s.weaponNet == 0) s.weaponNet = c.netCode;
    }
}

// ★ 09-28 B4b — 평상복인가(`sub_53E2D0` 의 반대 — 갑옷이고 입음 비트 & 0xC0D · `GAME4_3.c:7315~7324`). 셔츠 0x400 · 바지 4 · 신발 1
static bool IsStreetClothes(const Session::Carried& c) {
    return c.equipBit != 0 && !c.isWeapon && !c.weaponClass && (c.equipBit & 0xC0Du) != 0;
}

// ★ 09-28 B4b — 가방 한 칸을 바닥에 떨군다(`dropDefault_4ED290` 의 몸통 — 손 버리기 `72 TRY_DROP` 과 죽을 때 떨어뜨리기가 함께 쓴다).
//   ① 벗기 `sub_4ED0C0`(`GAME3_3.c:6304~6365`) — 본인 알림은 늘 없다(a3 = 0) · 남에게는 산 사람만(죽었으면 a4 = 0 · `:6321~6323`).
//      무기는 든 것일 때만 내려놓는다(53A140 이 가린다)
//   ② 본인에게 `4d REPORT_DROP <번호2> <종류2>`(`netReportDrop_4D8B50`) → 가방에서 뺀다
//   ③ 그 자리에 세계로(`createAt_4DAA50`) — 소개는 시야 갱신이 한다 · 사라짐 시계(`DropDecayFrame`)
void Server::DropCarriedToWorld(Session& s, size_t at, double tx, double ty, bool dead, std::vector<OutPacket>& out) {
    if (at >= s.inventory.size()) return;
    const uint16_t nc = s.inventory[at].netCode;
    if (s.inventory[at].equipped) {
        if (s.inventory[at].weaponClass) {
            PlayerDequipWeapon(s, nc, out, /*toSelf=*/false, /*toOthers=*/!dead);
        } else {
            s.inventory[at].equipped = false;
            ReportEquipChange(s, s.inventory[at], false, out, /*toSelf=*/false, /*toOthers=*/!dead);
        }
    }
    const Session::Carried c = s.inventory[at];
    {
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_DROP), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8),
                                 static_cast<uint8_t>(c.type), static_cast<uint8_t>(c.type >> 8) };
        EmitSeq(s, msg, sizeof(msg), out);
    }
    s.inventory.erase(s.inventory.begin() + static_cast<long>(at));
    if (s.weaponNet == nc) s.weaponNet = 0;   // 들고 있지 않은 칸이어도 번호가 남지 않게(HeldWeapon 은 어차피 못 찾는다)
    // 세계로 — 칸은 `INVENTORY_FAIL` 되돌리기와 같다
    WorldStatic w;
    w.netCode = nc; w.type = c.type; w.modifiable = c.modifiable; w.useAmount = c.useAmount;
    w.equipBit = c.equipBit; w.isWeapon = c.isWeapon; w.weaponClass = c.weaponClass;
    w.wandClass = c.wandClass; w.subclass = c.subclass; w.weight = c.weight;
    w.isFood = c.isFood; w.foodBits = c.foodBits; w.useFn = c.useFn;
    w.charges = c.charges; w.chargesMax = c.chargesMax; w.wandSpell = c.wandSpell; w.wandCooldown = c.wandCooldown;
    w.x = static_cast<int>(std::lround(tx)); w.y = static_cast<int>(std::lround(ty));
    // ★ 09-28 사람 판 「버린 물건이 10초가 지나도 안 사라진다」 — 사라짐 시계(`DropDecayFrame` 주석).
    //   ✖ 09-28 정정 — 처음엔 `dropDefault_4ED290` 의 10초 규칙만 옮겨 「기본 장비만 10초 · 맵 물건은 남음」이라 했다.
    //     정품 녹음은 로브·맵 갑옷 모두 **25초**(750프레임) — 갑옷·무기는 물건별 버리기 함수가 25초로 다시 건다
    w.decayExempt = c.decayExempt;
    w.dropFn = c.dropFn;
    for (int m = 0; m < 4; ++m) w.mods[m] = c.mods[m];   // ★ 09-29 — 수식도 바닥으로
    // ★ 09-29 — 줍기 함수 갈래·줍는 소리·재장전 칸도 바닥으로. 물약 줍기 깃발은 갈래에서 다시 세운다
    //   (✖ 전에는 버린 물약이 `potionPickup` 을 잃어 다시 주우면 「그 자리에서 마시기」·직업 거절을 안 탔다)
    w.pickupFn = c.pickupFn; w.pickupSound = c.pickupSound; w.bowReload = c.bowReload;
    w.potionPickup = (c.pickupFn == PickupFn::Potion);
    w.decayFrame = DropDecayFrame(c.dropFn, c.decayExempt);
    statics_.push_back(w);
    freshStatics_.insert(nc);   // 시야 반경이 꺼진 판에서도 소개되게(틱 갱신의 freshStatics_ 자리)
}

// ★ 09-28 B4b — 죽을 때 가방 떨어뜨리기 `nox_xxx_dropAllItems_4EDA40`(`GAME3_3.c:6701~6861`). 퀘스트가 아니면 `diePlayer_54D2B0` 이 부른다(`GAME5.c:5093~5095`).
//   · 거름 `unitDropCheckSome_4EDCD0`(`:6864~6869`) — 사람이면 NO_AUTO_DROP(0x10000000)은 안 떨군다. 그 깃발은 평상복을 **입을 때** 서고
//     **벗을 때** 지워진다(`GAME4_3.c:7479~7481·7558~7560` · 벗기 `&= 0xEFFFFEFF`) ⇒ **입은 평상복만 남는다**
//   · 차례 — 가방 머리부터(정답지는 새로 든 것을 머리에 끼운다 · 우리 가방은 뒤에 붙이므로 **뒤에서부터**).
//     정품 녹음 18:55:55.9 — `4d` 넷이 b0·bc·b8·68 = 가장 최근에 주운 것부터
//   · 자리 — 몸 둘레를 네모 나선으로 돌며(간격 = 떨굴 것 중 가장 큰 몸 반지름 × 2 + 6 · 칸마다 ±3 흔듦) 몸에서 그 자리까지
//     선 추적(깃발 1)이 트인 첫 자리에 하나씩. 한 바퀴를 다 돌아도 하나도 못 놓으면 그 뒤 것은 전부 **몸 자리에**
//   · 벗을 때 남에게 안 알린다(죽음 — `DropCarriedToWorld` dead) · 본인에게 `4d` 만(녹음과 같다)
void Server::DropAllItems(Session& s, std::vector<OutPacket>& out) {
    std::vector<uint16_t> order;
    float maxR = 0.0f;
    for (size_t k = s.inventory.size(); k-- > 0;) {
        const Session::Carried& c = s.inventory[k];
        if (c.equipped && IsStreetClothes(c)) { ++deathDropKept_; continue; }
        order.push_back(c.netCode);
        const auto r = itemRadius_.find(c.type);
        if (r != itemRadius_.end() && r->second > maxR) maxR = r->second;
    }
    const double step = static_cast<double>(maxR) + maxR + 6.0;   // v13
    const double px = s.posX, py = s.posY;
    auto dropNc = [&](uint16_t nc, double x, double y) {
        for (size_t i = 0; i < s.inventory.size(); ++i)
            if (s.inventory[i].netCode == nc) { DropCarriedToWorld(s, i, x, y, /*dead=*/true, out); return; }
    };
    int v2 = 0, v6 = -1, v7 = 3, v14 = 0, v15 = 0, v20 = 0, v21 = -1;
    bool v16 = false;
    auto stepDir = [&](int d) {
        switch (d) {
            case 0: v20 = ++v2; break;
            case 1: v21 = ++v6; break;
            case 2: v20 = --v2; break;
            case 3: v21 = --v6; break;
            default: break;
        }
    };
    size_t oi = 0;
    for (; oi < order.size(); ++oi) {
        bool v8 = false, v17 = false;
        int v18 = v7 - 1;
        for (;;) {
            double tx = v20 * step + px, ty = py - v21 * step;
            tx += -3.0 + 6.0 * NextRandomFloat();
            ty += -3.0 + 6.0 * NextRandomFloat();
            if (MapTraceRay(static_cast<float>(px), static_cast<float>(py), static_cast<float>(tx), static_cast<float>(ty), 1)) {
                dropNc(order[oi], tx, ty);
                ++deathDropSpiral_;
                v8 = v16 = v17 = true;
            } else {
                v8 = v17;
            }
            if (v14 != v18) { ++v14; stepDir(v15); }
            else if (v15 != 3 && v18) { ++v15; v14 = 1; stepDir(v15); }
            else {
                if (!v16) break;                                  // 한 바퀴 내내 못 놓았다
                v7 += 2; v18 += 2;
                v2 = 1 - v7 / 2; v20 = v2; v14 = 1;
                v6 = v7 / -2; v21 = v6; v15 = 0; v16 = false;
            }
            if (v8) break;
        }
        if (!v8) break;
    }
    for (; oi < order.size(); ++oi) { dropNc(order[oi], px, py); ++deathDropAtBody_; }   // 남은 것은 몸 자리에(`:6849~6859`)
}

// ★ 09-28 B4b — 부활 때 가방 정리 + 기본 장비 새로 — `playerMakeDefItems_4EF7D0(a1, 1, 0)` 의 a3 = 0 갈래(`GAME3_3.c:7969~8043`).
//   ① 가방에서 지운다 — **입은 평상복 중 0x808 비트가 아닌 것**(셔츠·바지·신발)만 남긴다(`:7969~7979`). 알림 없음(`delayedDeleteObject`)
//   ② (부르는 쪽이 `e9 … 01` 을 보낸 뒤) 새로 준다 — `playerRespawnItem_4EF750`(`:7872~7897`): 새 오브젝트 · RESPAWN 지움 · 가방에 넣고 입힘
//      셔츠: (협동 2048 | 협동팀 512) 이거나 워리어가 아니면 · 입은 비트에 0x400 없을 때 / 바지: 4 없을 때 / 신발: 1 없을 때
//      직업(협동·퀘스트 아님): 위저드 로브 · 워리어 롱소드+나무방패 · 컨저러 없음
//   ③ 본인에게 물건마다 `4c <번호2> <종류2> <색4>` · (내구도 있으면) `44 <번호2> <내구도2> <내구도2>` · `60 <번호2>` — 정품 녹음 18:56:07.9
//      위저드 부활 = `4c 0c 02 79 04 ff ff ff ff` · `44 0c 02 45 01 45 01` · `60 0c 02`. 남에게는 알림 없음(`e9 … 01` 로 클라가 기본 차림을 다시 그린다)
//   ⚠ 안 옮긴 것 — 협동(2048)·퀘스트(4096) 갈래의 직업 장비 표 · 평상복 색(사람이 고른 색 — 접속 스냅샷과 같은 녹음 색을 쓴다)
void Server::RespawnGear(Session& s, std::vector<OutPacket>& out) {
    for (size_t i = s.inventory.size(); i-- > 0;) {
        const Session::Carried& c = s.inventory[i];
        const bool keep = c.equipped && IsStreetClothes(c) && (c.equipBit & 0x808u) == 0;
        if (keep) continue;
        if (s.weaponNet == c.netCode) s.weaponNet = 0;
        s.inventory.erase(s.inventory.begin() + static_cast<long>(i));
        ++respawnGearDeleted_;
    }
    uint32_t worn = 0;
    for (const auto& c : s.inventory) if (c.equipped && !c.isWeapon) worn |= c.equipBit;
    const bool coop = (gameSettings_.gameFlags & 0x800u) != 0;
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    std::vector<uint16_t> give;
    if (((gameSettings_.gameFlags & 0xA00u) != 0 || s.charClass != CharClass::Warrior) && !(worn & 0x400u)) give.push_back(kItemStreetShirt);
    if (!(worn & 0x4u)) give.push_back(kItemStreetPants);
    if (!(worn & 0x1u)) give.push_back(kItemStreetSneakers);
    if (coop || quest) {
        ++respawnGearNotPorted_;                              // ⚠ 협동·퀘스트의 직업 장비 표는 안 옮김
    } else if (s.charClass == CharClass::Wizard) {
        give.push_back(kItemWizardRobe);
    } else if (s.charClass == CharClass::Warrior) {
        give.push_back(kItemLongsword);
        give.push_back(kItemWoodenShield);
    }
    for (uint16_t type : give) {
        const auto it = itemTemplate_.find(type);
        if (it == itemTemplate_.end()) { ++respawnGearNoTemplate_; continue; }   // 틀이 없으면 안 준다(무게를 짐작하지 않는다)
        Session::Carried c = it->second;
        c.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        c.type = type;
        c.equipped = true;
        c.decayExempt = DecayExempt::None;                    // RESPAWN 지움(`:7891`)
        uint8_t look[4] = { 0xff, 0xff, 0xff, 0xff };
        uint16_t dur = 0;
        DefaultGearLook(type, look, dur);                     // 내구도만 이 표에서
        DefaultGearMods(type, s.info97, c.mods);              // ★ 09-29 — 색(수식)은 캐릭터 색으로(정보 97B 가 없으면 같은 표)
        const uint8_t pk[9] = { static_cast<uint8_t>(Op::REPORT_MODIFIABLE_PICKUP), static_cast<uint8_t>(c.netCode), static_cast<uint8_t>(c.netCode >> 8),
                                static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8), c.mods[0], c.mods[1], c.mods[2], c.mods[3] };
        EmitSeq(s, pk, sizeof(pk), out);
        if (dur > 0) {
            const uint8_t d[7] = { 0x44, static_cast<uint8_t>(c.netCode), static_cast<uint8_t>(c.netCode >> 8),
                                   static_cast<uint8_t>(dur), static_cast<uint8_t>(dur >> 8), static_cast<uint8_t>(dur), static_cast<uint8_t>(dur >> 8) };
            EmitSeq(s, d, sizeof(d), out);
        }
        const uint8_t eq[3] = { static_cast<uint8_t>(Op::REPORT_EQUIP), static_cast<uint8_t>(c.netCode), static_cast<uint8_t>(c.netCode >> 8) };
        EmitSeq(s, eq, sizeof(eq), out);
        s.inventory.push_back(c);
        if (c.isWeapon && s.weaponNet == 0) s.weaponNet = c.netCode;
        ++respawnGearGiven_;
    }
}

// ★ 09-27 B3 — 입기·벗기 알림(09-27 전에는 입기 처리 안에 있던 것을 그대로 옮김) — 본인에게 `0x60/0x61` · 남에게 겉모습
void Server::ReportEquipChange(Session& s, const Session::Carried& c, bool wear, std::vector<OutPacket>& out, bool toSelf, bool toOthers) {
    const uint16_t nc = c.netCode;
    uint8_t msg[3] = {
        static_cast<uint8_t>(wear ? Op::REPORT_EQUIP : Op::REPORT_DEQUIP),
        static_cast<uint8_t>(nc),
        static_cast<uint8_t>(nc >> 8),
    };
    if (toSelf) EmitSeq(s, msg, sizeof(msg), out);   // 본인에게(버리기는 빼고 `4d REPORT_DROP` 이 대신한다)
    if (wear) ++equipped_; else ++dequipped_;
    // ★ 09-28 B4b — 죽은 사람이 떨어뜨리는 갑옷·무기는 **남에게도 안 알린다**(`sub_4ED0C0` `GAME3_3.c:6321~6323` 이 a4 = 0 ·
    //   정품 녹음 `orig_arena_1851` 18:55:55.9 — 죽은 쪽에 `4d` 넷뿐, 상대에게 `53`·`54` 없음). 클라는 부활 `e9 … 01` 로 다시 그린다
    if (!toOthers) return;

    // ★★ **남들에게 겉모습을 알린다** (P7). 그동안 본인 표시(0x60/0x61)만
    //   있어서 옆 사람 화면의 몸이 안 바뀌었다.
    //   짜임은 클라 원본 확정(2026-09-11) —
    //     입기: op | 번호2(**bit15=사람**) | 장비비트4 | (수식4, 빈 칸 ff)
    //     벗기: op | 번호2(**bit15 안 봄** — 클라가 사람 전용으로 처리) | 장비비트4
    //   장비비트가 0 이면(표에 없는 이름) **안 보낸다** — 짐작으로 안 채운다.
    // ★ 09-29 — 입기는 `BuildWornLook`(`sub_4D82F0`)로 — 수식이 하나라도 있으면 51·52(수식 넷), 없으면 50·4f.
    //   ✖ 전에는 CLASS 가 수식 갈래면 늘 52 에 ff 넷을 실었다 — 평상복 색(수식)이 빠져 남 화면에 셔츠가 살색으로 보였다(09-28 사람 판)
    if (wear && c.equipBit != 0) {
        uint8_t am[11];
        const size_t an = BuildWornLook(s, c, am);
        if (an > 0) {
            Broadcast(/*viaSeq=*/true, am, an, out);
            ++appearanceSent_;
        } else {
            ++appearanceUnknown_;
        }
        return;
    }
    if (c.equipBit != 0) {                                 // 벗기 — `53`·`54` 7바이트
        uint8_t am[7];
        am[0] = static_cast<uint8_t>(c.isWeapon ? Op::REPORT_WEAPON_DEQUIP : Op::REPORT_ARMOR_DEQUIP);
        am[1] = static_cast<uint8_t>(s.netCode);
        am[2] = static_cast<uint8_t>(s.netCode >> 8);
        PutU32(am + 3, c.equipBit);
        Broadcast(/*viaSeq=*/true, am, sizeof(am), out);
        ++appearanceSent_;
    } else {
        ++appearanceUnknown_;
    }
}

// ★ 09-29 — `sub_4D82F0`(`GAME3_2.c:3841~3947`) — 입은 물건 하나의 겉모습.
//   무기 갈래(CLASS & 0x11001000 — 무기·지팡이·깃발): 수식이 하나라도 있으면 `51`(11바이트 · 수식 넷), 없으면 `50`(7바이트)
//   갑옷(CLASS & 0x2000000): 같은 규칙으로 `52`·`4f` · 번호는 주인이 사람이면 bit15 · 비트는 무기/갑옷 입음 비트
//   ⭕ 정품 녹음 `orig_arena_1851` 18:53:27.410 — 새로 온 위저드에게 워리어의 나무방패 `4f 5c 80 00 00 00 01` · 롱소드 `50 5c 80 00 02 00 00` ·
//     신발 `52 5c 80 01 00 00 00 b9 ca ff ff` · 바지 `52 5c 80 04 00 00 00 ff bf ff ff`
size_t Server::BuildWornLook(const Session& who, const Session::Carried& c, uint8_t buf[11]) {
    if (c.equipBit == 0) return 0;                         // 비트를 모르면 안 보낸다(짐작으로 안 채운다)
    bool hasMod = false;
    for (uint8_t m : c.mods) if (m != 0xff) hasMod = true;
    const bool weapon = c.weaponClass || c.isWeapon;
    buf[0] = static_cast<uint8_t>(weapon ? (hasMod ? Op::REPORT_MODIFIABLE_WEAPON_EQUIP : Op::REPORT_MUNDANE_WEAPON_EQUIP)
                                         : (hasMod ? Op::REPORT_MODIFIABLE_ARMOR_EQUIP : Op::REPORT_MUNDANE_ARMOR_EQUIP));
    const uint16_t nc = static_cast<uint16_t>(who.netCode | 0x8000u);
    buf[1] = static_cast<uint8_t>(nc);
    buf[2] = static_cast<uint8_t>(nc >> 8);
    PutU32(buf + 3, c.equipBit);
    if (!hasMod) return 7;
    for (int i = 0; i < 4; ++i) buf[7 + i] = c.mods[i];
    return 11;
}

// ★ 09-29 — `sub_4DDE10`(`GAME3_2.c:5670~5693`) — 받는 사람에게 그 사람의 입은 것(+ 깃발) 전부를 `sub_4D82F0` 로.
//   새로 온 사람에게 먼저 온 사람을 소개할 때 `2d` 뒤에 부른다(녹음 18:53:27.410 — `2d` → `4f`·`50`·`52`·`52`).
//   ✖ 전에는 이 자리가 없어 나중에 들어온 사람이 먼저 온 사람의 **평상복 색**(수식)을 못 받았다 — 09-28 사람 판 「컨저러 셔츠가 살색」
//   ⚠ 깃발 갈래(입지 않아도 `Flag` 면)는 우리 깃발이 가방에 없어 뺀다
void Server::SendWornLooks(Session& to, const Session& who, std::vector<OutPacket>& out) {
    // 차례 — 가방 머리부터(정답지는 새로 든 것을 머리에 끼운다 · 우리는 뒤에 붙이므로 뒤에서부터). 녹음 차례 = 방패 → 검 → 신발 → 바지
    for (size_t k = who.inventory.size(); k-- > 0;) {
        const Session::Carried& c = who.inventory[k];
        if (!c.equipped) continue;
        uint8_t buf[11];
        const size_t n = BuildWornLook(who, c, buf);
        if (n == 0) { ++wornLookSkipped_; continue; }
        EmitSeq(to, buf, n, out);
        ++wornLookSent_;
    }
}

// ★ 09-29 — 기본 장비의 수식 넷. 평상복은 캐릭터 색이 수식이다 — `playerMakeDefItems_4EF7D0`(`GAME3_3.c:7983~8006`):
//   셔츠 { 0, UserColor1+색[84], UserColor1+색[85], 0 } · 바지 { 0, UserColor1+색[83], 0, 0 } · 신발 { UserColor1+색[87], UserColor1+색[86], 0, 0 }
//   UserColor1 번호 = **0xb3**(정품 녹음 — 캐릭터 색 07·13·0c·17·06 → 수식 ba·c6·bf·ca·b9 다섯이 모두 0xb3 + 색)
//   정보 97B 가 없으면(시험 접속) 접속 스냅샷과 같은 표(`DefaultGearLook`) · 로브·방패·검은 수식 없음
void Server::DefaultGearMods(uint16_t type, const std::vector<uint8_t>& info97, uint8_t mods[4]) {
    mods[0] = mods[1] = mods[2] = mods[3] = 0xff;
    if (info97.size() >= 88) {
        constexpr uint8_t kUserColor1 = 0xb3;
        if (type == kItemStreetShirt) { mods[1] = static_cast<uint8_t>(kUserColor1 + info97[84]); mods[2] = static_cast<uint8_t>(kUserColor1 + info97[85]); }
        else if (type == kItemStreetPants) { mods[1] = static_cast<uint8_t>(kUserColor1 + info97[83]); }
        else if (type == kItemStreetSneakers) { mods[0] = static_cast<uint8_t>(kUserColor1 + info97[87]); mods[1] = static_cast<uint8_t>(kUserColor1 + info97[86]); }
        return;
    }
    uint16_t dur = 0;
    uint8_t look[4];
    if (DefaultGearLook(type, look, dur)) for (int i = 0; i < 4; ++i) mods[i] = look[i];
}

// ★ 10-01 — 클라 「대체 무기」 알림 `e0 <번호2>`(sdecode `0xE0` · `server__network__sdecode.c:281~293`).
//   클라는 무기 바꾸기 단축키 대상을 정할 때 보낸다(`nox_xxx_clientSetAltWeapon_461550` → `clientReportSecondaryWeapon_4BF010` · `GAME3_1.c:1622~1627`).
//   서버 `sub_53AB90`(`GAME4_3.c:5192~5205`) —
//     번호 0 → 칸(+108)을 비움 · 아니면 그 오브젝트를 찾아(`getObjectFromNetCode_4ECCB0` — 우리는 가방 속에서) 있으면
//     직업(`57B3D0` = `sub_57B370` · `object_pickup.go:111~138`) **또는** 힘(`4F3180` · `GAME3_3.c:10664~10682`)이 안 맞을 때
//     `e0 <0 0> 01`(`netSendSecondaryWeapon_4D9670` · `GAME3_2.c:4805~4812` · Packet1)을 그 사람에게 보내 클라 쪽을 비우게 한다 ·
//     그리고 **맞든 안 맞든** 칸에 적는다(`:5204`) · 못 찾으면 칸 0
//   직업 표 — 무기·지팡이는 무기 정의 · 갑옷은 갑옷 정의(정의가 없으면 0 = 못 씀) · 음식은 `~(SUBCLASS >> 5) | 0xFE` · 그 밖은 0xFF(누구나)
//   힘 — 갑옷(0x2000000)이면 갑옷 정의 · 아니면 무기 정의의 필요 힘 · **정의가 없으면 거짓**(그래서 무기·갑옷이 아닌 물건은 늘 되돌림)
//   ⚠ 번호 윗비트(0x8000 = extent · `packetDynamicUnitCode_578B40`)는 가방 물건이 아니라 못 찾음으로 센다
//   ⚠ +108 을 읽는 자리는 저장 파일(`GAME1_1.c:1261~1267`) 말고는 못 찾았다 — 칸은 적어만 둔다
void Server::ReportSecondaryWeapon(Session& s, uint16_t code, std::vector<OutPacket>& out) {
    if (code == 0) { s.secondaryWeapon = 0; ++secondaryWeaponStat_[0]; return; }
    const Session::Carried* item = nullptr;
    if (!(code & 0x8000))
        for (const auto& c : s.inventory) if (c.netCode == code) { item = &c; break; }
    if (!item) { s.secondaryWeapon = 0; ++secondaryWeaponStat_[1]; return; }
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(s.charClass));
    const bool armor = IsArmorItem(*item);
    uint8_t classes = 0xFF;
    if (item->weaponClass) {
        const auto d = weaponDef_.find(item->type);
        classes = (d == weaponDef_.end()) ? 0 : d->second.classes;
    } else if (armor) {
        const auto d = armorDef_.find(item->type);
        classes = (d == armorDef_.end()) ? 0 : d->second.classes;
    } else if (item->isFood) {
        classes = ClassCanUseFood(s.charClass, item->foodBits) ? bit : 0;   // 같은 셈(`~(SUBCLASS >> 5) | 0xFE`)
    }
    bool strengthOk = false;
    if (armor) {
        const auto d = armorDef_.find(item->type);
        strengthOk = d != armorDef_.end() && s.strength >= static_cast<int>(d->second.reqStrength);
    } else {
        const auto d = weaponDef_.find(item->type);
        strengthOk = d != weaponDef_.end() && s.strength >= static_cast<int>(d->second.reqStrength);
    }
    if (!(classes & bit) || !strengthOk) {
        const uint8_t msg[4] = { static_cast<uint8_t>(Op::REPORT_SECONDARY_WEAPON), 0, 0, 1 };
        EmitSeq(s, msg, sizeof(msg), out);
        ++secondaryWeaponStat_[3];
    } else {
        ++secondaryWeaponStat_[2];
    }
    s.secondaryWeapon = code;
}

// ★ 09-27 B3 — 무기 들기 `nox_xxx_playerEquipWeapon_53A420`(`GAME4_3.c:4906~4993`) — 관문 순서 그대로.
//   옮긴 것: 무기 클래스 `:4908` · 이미 듦 `:4911~4914` · 함성·광전사 켜짐 값 `:4923` · 직업 `:4926~4932`(modifier.bin CLASSUSE) ·
//     힘 `:4933~4940`(REQUIRED_STRENGTH) · 공격 중이면 상태 13 `:4951~4953` · 먼저 든 무기 내려놓기 `:4964~4967` · 듦 깃발·알림·무기 칸 `:4968~4975`
//   ⚠ 안 옮긴 것 — 마법부여 켜기 `:4988`(붙일 것 없음) — 끝의 B5 주석
//     (09-27 B5 에서 지팡이 충전 알림 `:4976~4979` 와 `53A3D0` `:4989~4991` 은 옮김)
//     (★ 09-29 화살통 갈래 `:4954~4963`(활 없으면 가방에서 들기 `53A680` · 없으면 BowNotFound · 든 화살통 내려놓기 `53A0F0`) ·
//      화살통은 무기 칸을 안 바꿈 `:4964·4973` · WEAPON 쪽 충전 알림(0x82)·활 재장전 칸 0(0xC) `:4980~4986` 옮김)
//     NPC 갈래 `:4916~4918` · 사람 아님 `:4919~4921` 은 우리 부름이 늘 사람이라 없음 · 가방에 있음 `:4941~4950` 은 부르는 쪽이 가방에서 찾아 넘김
//   거절 글·소리 — `netPriMsgToPlayer` + `aud_501960(925, 사람, 2, 사람 번호)`(a4 가 참일 때만 소리 · 입기 요청은 참)
bool Server::PlayerEquipWeapon(Session& s, uint16_t nc, bool failSound, std::vector<OutPacket>& out) {
    auto gate = [&](EquipGate g) { ++equipGate_[static_cast<size_t>(g)]; return g == EquipGate::Equipped; };
    Session::Carried* item = nullptr;
    for (auto& c : s.inventory) if (c.netCode == nc) { item = &c; break; }
    if (!item || !item->weaponClass) return gate(EquipGate::NotWeapon);
    if (item->equipped) return gate(EquipGate::Already);
    if (AbilityIsActiveVal(s, 2) || AbilityIsActiveVal(s, 1)) return gate(EquipGate::Ability);   // probablyWarcryCheck_4FC3E0 = IsActiveVal
    auto refuse = [&](const char* key, EquipGate g) {
        SendPriMsg(s, key, out);
        if (failSound)
            PlaySoundAt(925, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
        return gate(g);
    };
    // 직업 — `57B3D0` = (1 << 직업) & Classes62 · 무기 정의가 없으면 0(`object_pickup.go:113~118`)
    const auto d = weaponDef_.find(item->type);
    const uint8_t classes = (d == weaponDef_.end()) ? 0 : d->second.classes;
    if (!(classes & (1u << static_cast<uint8_t>(s.charClass))))
        return refuse("weapon.c:WeaponEquipClassFail", EquipGate::ClassFail);
    // 힘 — `4F3180` = 힘 ≥ ReqStrength60(정의가 없으면 거짓 — 위에서 이미 걸러짐)
    if (s.strength < static_cast<int>(d->second.reqStrength))
        return refuse("weapon.c:WeaponEquipStrengthFail", EquipGate::StrengthFail);
    if (s.moveState == kPlayerStateAttack) PlayerSetState(s, kPlayerStateIdle, frame_, &out);
    const uint16_t nc0 = item->netCode;
    const bool isQuiver = (item->equipBit == 2);
    if (isQuiver) {
        // ★ 09-29 — `:4954~4963` 활·석궁(비트 0xC)을 안 들었으면 가방에서 든다 — 못 들면 BowNotFound(+ 소리). 그다음 든 화살통을 내려놓는다(1, 1)
        //   ⚠ 가방에서 활을 들면 그 들기가 다시 이 함수를 타(`53A680` → `53A420(1,1)`) 가방이 바뀔 수 있다 — 아래에서 번호로 다시 찾는다
        if (!(WeaponEquipOf(s) & 0xC) && !EquipBowFromBag(s, out)) return refuse("weapon.c:BowNotFound", EquipGate::BowNotFound);
        DequipQuiver(s, true, true, out);
        item = nullptr;
        for (auto& c : s.inventory) if (c.netCode == nc0) { item = &c; break; }
        if (!item) return gate(EquipGate::NotWeapon);
    }
    const Session::Carried* prev = HeldWeapon(s);
    if (prev && !isQuiver && !PlayerDequipWeapon(s, prev->netCode, out)) return gate(EquipGate::DequipFail);
    item->equipped = true;
    ReportEquipChange(s, *item, true, out);                                     // netReportEquip_4D8540(+ 겉모습 — 우리 옛 입기 그대로)
    if (!isQuiver) s.weaponNet = item->netCode;                                 // +104 — 화살통은 무기 칸을 안 바꾼다(`:4973`)
    // ★ 09-27 B5 — 충전 알림(`:4976~4979`) — 지팡이 클래스이고 SUBCLASS 가 마법 지팡이(0x47F0000)면 `64 <번호> <지금> <최대>`
    //   ★ 09-29 — 그 밖의 WEAPON 클래스(`:4980~4986`) — 화살통·수리검(비트 0x82)이면 충전 알림 (화살 수, 최대) · 활·석궁(0xC)이면 재장전 칸 0
    //   ⚠ 마법부여 켜기 `4F2FF0`(`:4988`)는 우리 물건에 마법부여가 없어(맵 물건 수식을 안 읽어 늘 −1) 붙일 것이 없다
    if (item->wandClass && (item->subclass & 0x47F0000)) { SendCharges(s, *item, out); ++wandChargesOnEquip_; }
    else if (!item->wandClass) {
        if (item->equipBit & 0x82) SendCharges(s, *item, out);
        else if (item->equipBit & 0xC) item->bowReload = 0;
    }
    // ★ 09-27 B5 — `sub_53A3D0`(`:4989~4991` · `GAME4_3.c:4887~4903`) — 두 손 무기(무기 비트 0x7FFE40C)를 들면 입은 방패(갑옷 비트 0x3000000)를 벗는다(`53E430(1, 1)`).
    //   ★ 09-29 — 이제 갑옷 벗기(`PlayerDequipArmor` = `53E430(1,1)`)로. 방패 막기는 `ArmorEquipOf` 가 가방을 읽어 따라 꺼진다
    if (item->equipBit & 0x7FFE40C) {
        std::vector<uint16_t> shields;                                           // 먼저 모은다 — 벗기가 가방 깃발을 만진다
        for (const auto& c : s.inventory)
            if (c.equipped && !c.isWeapon && (c.equipBit & 0x3000000)) shields.push_back(c.netCode);
        for (uint16_t snc : shields)
            if (PlayerDequipArmor(s, snc, out)) ++twoHandShieldOff_;
    }
    return gate(EquipGate::Equipped);
}

// ★ 09-27 B4 — 무기 내려놓기 `nox_xxx_playerDequipWeapon_53A140`(`GAME4_3.c:4755~4816`) — 사람 갈래.
//   무기 클래스 아님 → 0(`:4760`) · 공격 중이면 상태 13(`:4764~4766`) · **든 무기가 아니면 0**(`:4767~4770` — 화살통만 예외) ·
//   든 것의 듦 깃발 끔 · 본인 알림(a3 `4D8590`)·모두 알림(a4 `4D84C0`) · +104 = 0(`:4784~4795`)
//   ⚠ 안 옮긴 것 — 마법부여 끄기 `4F3030`(`:4796`) — 우리 물건에 마법부여가 없다. `:4797~4814` 는 원본 exe 에 없는 GameEx 확장이라 안 옮긴다(소스 주석)
//     (09-27 B5 에서 지속 주문 끊기 `4FEB60`(`:4774`)은 옮김)
//     (★ 09-29 — 활·석궁이면 화살통 내려놓기 `53A0F0(a3, a4)`(`:4771~4773`) · 화살통 갈래(`:4775~4783` — 깃발·비트 2 끄고 알림, 무기 칸은 그대로) 옮김.
//      화살통도 **든 무기가 있어야** 내려놓는다(`:4768` 의 `!v11 ||`))
//   부르는 자리는 모두 (a3, a4) = (1, 1) 이다 — 무기 들기(`:4965`) · 벗기 요청(`playerTryDequip_4F2FB0` `GAME3_3.c:10571`)
bool Server::PlayerDequipWeapon(Session& s, uint16_t nc, std::vector<OutPacket>& out, bool toSelf, bool toOthers) {
    auto gate = [&](DequipGate g) { ++dequipGate_[static_cast<size_t>(g)]; return g == DequipGate::Dequipped; };
    Session::Carried* item = nullptr;
    for (auto& c : s.inventory) if (c.netCode == nc) { item = &c; break; }
    if (!item || !item->weaponClass) return gate(DequipGate::NotWeapon);
    if (s.moveState == kPlayerStateAttack) PlayerSetState(s, kPlayerStateIdle, frame_, &out);
    Session::Carried* held = nullptr;
    for (auto& c : s.inventory) if (s.weaponNet != 0 && c.netCode == s.weaponNet) { held = &c; break; }
    const bool isQuiver = (item->equipBit == 2);
    if (!held || (held != item && !isQuiver)) return gate(DequipGate::NotHeld);   // `:4768` — 화살통만 「든 것과 같아야」를 비켜 간다
    if (item->equipBit & 0xC) DequipQuiver(s, toSelf, toOthers, out);         // ★ 09-29 `:4771~4773` — 활·석궁을 내리면 화살통도
    if (isQuiver) {
        // ★ 09-29 `:4775~4783` — 화살통: 깃발(+16 & 0x100)·비트 2 끄고 본인(a3)·모두(a4)에게 알림. 무기 칸(+104)은 그대로
        item->equipped = false;
        ReportEquipChange(s, *item, false, out, toSelf, toOthers);
        gate(DequipGate::QuiverOff);
        return true;
    }
    // ★ 09-27 B5 — `sub_4FEB60`(`:4774` · `GAME4.c:3389~3402`) — 지팡이 클래스면 SUBCLASS 0x40000 은 지속 주문 43(연쇄 번개) · 0x4000000 은 59(플라스마)를 끊는다(`4FEB10`)
    if (item->wandClass) {
        if (item->subclass & 0x40000) { DurCancelFor(43, s.addr, out); ++wandSpellCut_; }
        if (item->subclass & 0x4000000) { DurCancelFor(59, s.addr, out); ++wandSpellCut_; }
    }
    held->equipped = false;
    ReportEquipChange(s, *held, false, out, toSelf, toOthers);   // ★ 09-28 — 버리기는 a3 = 0(본인 알림 없음, `GAME3_3.c:6334`) · 죽음은 a4 도 0
    s.weaponNet = 0;
    return gate(DequipGate::Dequipped);
}

// ★ 09-27 B2 — 무기 칸이 가리키는 가방 항목. 칸은 번호로 들고 여기서 가방을 뒤진다 —
//   버리기·되돌리기로 가방에서 빠지면 저절로 맨손이 된다(칸을 지우는 자리를 빠뜨려도 없는 물건을 안 든다)
const Session::Carried* Server::HeldWeapon(const Session& s) {
    if (s.weaponNet == 0) return nullptr;
    for (const auto& c : s.inventory)
        if (c.netCode == s.weaponNet) return &c;
    return nullptr;
}

// ★ 09-30 — 바닥 물건 → 가방 항목. 줍기(TRY_GET)가 하던 칸 옮기기 그대로(번호도 들고 온다)
Session::Carried Server::CarriedFromStatic(const WorldStatic& w) {
    Session::Carried c;
    c.netCode = w.netCode;
    c.type = w.type;
    c.modifiable = w.modifiable;
    c.useAmount = w.useAmount;
    c.equipBit = w.equipBit;
    c.isWeapon = w.isWeapon;
    c.weaponClass = w.weaponClass;         // ★ 09-27 B3
    c.wandClass = w.wandClass; c.subclass = w.subclass;   // ★ 09-27 B5
    c.weight = w.weight;
    c.isFood = w.isFood;
    c.foodBits = w.foodBits;
    c.useFn = w.useFn;
    c.charges = w.charges; c.chargesMax = w.chargesMax;
    c.wandSpell = w.wandSpell; c.wandCooldown = w.wandCooldown;
    c.decayExempt = w.decayExempt;         // ★ 09-28 — 주우면 사라짐 시계는 풀린다(`decay_511750` 이 들린 것은 목록에서만 뺀다) · 예외 까닭은 들고 간다
    c.dropFn = w.dropFn;
    for (int m = 0; m < 4; ++m) c.mods[m] = w.mods[m];   // ★ 09-29 — 수식(버린 평상복을 다시 주우면 색이 따라온다)
    c.pickupFn = w.pickupFn; c.pickupSound = w.pickupSound; c.bowReload = w.bowReload;   // ★ 09-29
    return c;
}

// ★ 09-30 사람 판 시험용 — `--give-items` 로 고른 물건을 **세계를 받은 지 2초 뒤** 가방에 넣는다(한 사람에 한 번 · 관전 중이면 기다림).
//   정답지에 없는 **시험 도구**다. 맵 물건을 서버에서 옮기면 클라가 맵으로 만든 그림과 어긋나서(`main.cpp` `--item-at-spawn` 주석),
//   새 번호로 가방에만 넣는다 — 본인에게 `4c <번호2> <종류2> <수식4>`(부활 새 장비 `RespawnGear` 와 같은 알림) · 충전이 있으면 `64`.
//   틀은 main 이 맵 바닥 물건에서 떠 둔다(화살통은 맵 짐의 20/20) — 맵에 없는 종류는 틀이 없어 안 준다
void Server::GiveTestItems(std::vector<OutPacket>& out) {
    if (giveItems_.empty()) return;
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.observer || o.snapshotIn >= 0 || o.testItemsGiven) continue;
        if (o.testItemsFrom == 0) o.testItemsFrom = frame_ + 2 * kServerTickRate;
        if (frame_ < o.testItemsFrom) continue;
        o.testItemsGiven = true;
        for (uint16_t type : giveItems_) {
            const auto it = itemTemplate_.find(type);
            if (it == itemTemplate_.end()) { ++testItemsNoTemplate_; continue; }
            Session::Carried c = it->second;
            c.netCode = nextShotNetCode_++;
            if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
            c.type = type;
            c.equipped = false;
            c.decayExempt = DecayExempt::None;
            const uint8_t pk[9] = { static_cast<uint8_t>(Op::REPORT_MODIFIABLE_PICKUP), static_cast<uint8_t>(c.netCode), static_cast<uint8_t>(c.netCode >> 8),
                                    static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8), c.mods[0], c.mods[1], c.mods[2], c.mods[3] };
            EmitSeq(o, pk, sizeof(pk), out);
            if (c.chargesMax != 0) SendCharges(o, c, out);
            o.inventory.push_back(c);
            ++testItemsGiven_;
        }
        std::printf("[시험물건] %s 가방에 %zu개 넣음(틀 없음 %llu)\n", o.addr.c_str(), giveItems_.size(), testItemsNoTemplate_);
        std::fflush(stdout);
    }
}

// ── ★ 09-29 활·석궁·화살통 ─────────────────────────────────────────────
//
// 정답지 가방 고리(+504)는 새로 든 것이 머리다 — 우리 가방은 뒤에 붙이므로 「첫 것」은 **뒤에서부터** 찾는다.
// 세 도우미 모두 고리에서 **처음 맞는 것 하나**에 부르고 끝난다(그것이 실패해도 다음 것을 안 본다).

// `sub_53A680`(`GAME4_3.c:4997~5011`) — 활·석궁(비트 0xC)을 가방에서 찾아 든다(a3 = a4 = 1)
bool Server::EquipBowFromBag(Session& s, std::vector<OutPacket>& out) {
    for (size_t i = s.inventory.size(); i-- > 0;)
        if (s.inventory[i].weaponClass && (s.inventory[i].equipBit & 0xC))
            return PlayerEquipWeapon(s, s.inventory[i].netCode, /*failSound=*/true, out);
    return false;
}

// `sub_53A0F0`(`GAME4_3.c:4732~4752`) — 든(+16 & 0x100) 화살통(비트 2)을 내려놓는다(`53A140(a1, 그것, a2, a3)`)
void Server::DequipQuiver(Session& s, bool toSelf, bool toOthers, std::vector<OutPacket>& out) {
    for (size_t i = s.inventory.size(); i-- > 0;)
        if (s.inventory[i].equipped && s.inventory[i].weaponClass && s.inventory[i].equipBit == 2) {
            PlayerDequipWeapon(s, s.inventory[i].netCode, out, toSelf, toOthers);
            return;
        }
}

// `playerTryReloadQuiver_539FF0`(`GAME4_3.c:4672~4686`) — 화살통(비트 2)을 가방에서 찾아 든다(a3 = a4 = 1).
//   ⚠ 든 화살통이 첫 것이면 「이미 듦」으로 실패한다 — 정답지 그대로(다 쓴 화살통은 이미 지워져 보통은 안 걸린다)
bool Server::TryReloadQuiver(Session& s, std::vector<OutPacket>& out) {
    for (size_t i = s.inventory.size(); i-- > 0;)
        if (s.inventory[i].weaponClass && s.inventory[i].equipBit == 2)
            return PlayerEquipWeapon(s, s.inventory[i].netCode, /*failSound=*/true, out);
    return false;
}

// `sub_53A6C0`(`GAME4_3.c:5014~5031`) — 무기 줍는 소리(사람 자리에서 모두에게). 번호는 main 이 클래스·재질로 정해 둔다
void Server::PlayWeaponPickupSound(const Session& s, uint16_t snd) {
    if (snd == 0) return;
    PlaySoundAt(snd, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
    ++weaponPick_[static_cast<size_t>(WeaponPick::Sound)];
}

// `nox_xxx_calcBoltDamage_4EF1E0`(`GAME3_3.c:7752~7765`) — (힘 − 요구 힘) × 계수 + 최소 · 화살의 modifier.bin 정의로.
//   힘은 쏜 이(+508)의 것 · 쏜 이가 없으면 30(`GAME3_3.c:5233~5237`). 솔로(깃발 2048)의 쇠뇌살 갈래는 우리 판에 없다.
//   부르는 쪽이 `(long long)(값 + 0.5)`(`:5294` — qword_581450_9544 = 0.5)로 자른다 — 여기서 함께
int Server::ArrowDamage(const std::string& owner, uint16_t arrowType) const {
    const auto d = weaponDef_.find(arrowType);
    if (d == weaponDef_.end()) return 0;                                  // 정의가 없으면 정답지는 닿음 함수에서 바로 돌아간다(`:5223~5226`)
    int str = 30;
    auto it = sessions_.find(owner);
    if (it != sessions_.end()) str = it->second.strength;
    const double v = static_cast<double>(str - static_cast<int>(d->second.reqStrength)) * static_cast<double>(d->second.coeff) +
                     static_cast<double>(d->second.damageMin);
    return static_cast<int>(static_cast<long long>(static_cast<double>(static_cast<float>(v)) + 0.5));
}

// `nox_xxx_shootBowCrossbow1_539BD0`(`GAME4_3.c:4460~4534`) — 활·석궁의 한 발. 참 = 쐈다(화살을 만들었든 앞이 막혔든)
//   ① 재장전 칸(쓰기 짐 [0])이 0 이면 — 든 화살통 중 화살이 남은 첫 것으로 쏜다(`539D80`)
//      없으면 — (퀘스트 · 활이면 화살통 없이 약한 화살) · 가방의 화살통을 들어 보고(ReloadQuiver · 칸 0) 못 들면 NoQuiver(퀘스트 · 활이면 글 없음) ·
//      소리 활 887(퀘스트면 없음) · 석궁 888 · 상태 13
//   ② 0 이 아니면 — ReloadingQuiver · 소리(활 887 퀘스트면 없음 · 석궁 888) · 칸 하나 줄임
bool Server::ShootBowCrossbow(Session& s, Session::Carried& bow, std::vector<OutPacket>& out) {
    const uint32_t kind = bow.equipBit & 0xC;                              // 4 활 · 8 석궁(`weaponInventoryEquipFlags_415820`)
    const bool quest = (questTotal_ > 0 || questStage_ > 0);               // gameFlags 4096
    const uint16_t bowNet = bow.netCode;
    auto bowRef = [&]() -> Session::Carried* { for (auto& c : s.inventory) if (c.netCode == bowNet) return &c; return nullptr; };
    auto sound = [&](uint16_t id) { PlaySoundAt(id, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr); };
    if (bow.bowReload == 0) {
        for (size_t i = s.inventory.size(); i-- > 0;) {
            Session::Carried& q = s.inventory[i];
            if (q.equipped && q.weaponClass && q.equipBit == 2 && q.charges != 0) {   // [1] 화살 수(무한 [2] 은 우리 판에 없음)
                ShootArrow(s, &q, kind, out);
                return true;
            }
        }
        if (quest && kind == 4) ShootArrow(s, nullptr, 4, out);            // LABEL_14 — 퀘스트 활은 화살통 없이도 약한 화살
        bool skipMsg = false;
        if (TryReloadQuiver(s, out)) {
            SendPriMsg(s, "pattack.c:ReloadQuiver", out);
            if (Session::Carried* b = bowRef()) b->bowReload = 0;
            ++bowShot_[static_cast<size_t>(BowShot::Reloaded)];
        } else {
            if (quest && kind == 4) skipMsg = true;                        // goto LABEL_25
            else SendPriMsg(s, "pattack.c:NoQuiver", out);
            ++bowShot_[static_cast<size_t>(BowShot::NoQuiver)];
        }
        if (skipMsg || kind == 4) { if (!quest) sound(887); }              // LABEL_25
        else if (kind == 8) sound(888);
        PlayerSetState(s, kPlayerStateIdle, frame_, &out);                 // LABEL_29
        return false;
    }
    SendPriMsg(s, "pattack.c:ReloadingQuiver", out);
    if (kind == 4) { if (!quest) sound(887); }
    else if (kind == 8) sound(888);
    --bow.bowReload;
    ++bowShot_[static_cast<size_t>(BowShot::Reloading)];
    return false;
}

// `nox_xxx_shootBowCrossbow2_539D80`(`GAME4_3.c:4537~4618`) — 화살 하나를 만든다.
//   · 자리 = 사람 자리 + (몸 반지름 + 4) × 바라보는 쪽(Direction1 +124) — 사람 자리에서 거기까지 선 추적(5)이 막히면 아무것도 안 한다
//   · 종류 = 화살통이 있으면 활 ArcherArrow · 석궁 ArcherBolt, 없으면 WeakArcherArrow
//   · 주인(+508)·갱신 짐 [1] = 쏜 이 · 속도 = 바라보는 쪽 × 제 빠르기(+544) · 방향 둘 = 사람 방향
//   · 화살통이 있고 무한이 아니면 화살 수 하나 줄이고 충전 알림 — 0 이 되면 화살통을 지운다(`delayedDeleteObject` → `sub_4ED0C0`)
//   · 소리 활 885 · 석궁 886
//   ⚠ 안 옮긴 것 — 화살통 수식을 화살에(`modifSetItemAttrs_4E4990` — 우리 물건에 수식 효과가 없다) · 활의 마법부여 반동·빠르기(`shootApplyEffects_539F40`)
void Server::ShootArrow(Session& s, Session::Carried* quiver, uint32_t kind, std::vector<OutPacket>& out) {
    const double r = static_cast<double>(kBodyRadius) + 4.0;
    const uint8_t dir = s.facingPrev;                                       // +124 Direction1
    const float sx = static_cast<float>(r * DirVecX(dir) + s.posX);
    const float sy = static_cast<float>(r * DirVecY(dir) + s.posY);
    TraceHit th;
    if (!MapTraceRay(static_cast<float>(s.posX), static_cast<float>(s.posY), sx, sy, 0x5, &th)) {
        ++bowShot_[static_cast<size_t>(BowShot::Blocked)];
        return;
    }
    const char* name = quiver ? (kind == 4 ? "ArcherArrow" : "ArcherBolt") : "WeakArcherArrow";
    const ProjTypeInfo* pt = ProjType(name);
    if (pt && pt->type != 0) {                                              // 종류를 모르면 안 만든다(지어내지 않는다)
        MissileObj m;
        m.netCode = nextShotNetCode_++;
        m.type = pt->type;
        m.flags = pt->flags;
        m.posX = m.newX = m.prevX = sx;
        m.posY = m.newY = m.prevY = sy;
        m.dir1 = m.dir2 = dir;
        m.createdFrame = m.frame136 = frame_;
        m.speed = pt->speed;
        m.velX = DirVecX(dir) * m.speed;
        m.velY = DirVecY(dir) * m.speed;
        m.collideKind = 6;                                                  // ArrowCollide
        m.owner = s.addr;
        missiles_.push_back(m); ++missilesMade_;
        if (missiles_.size() > missilesPeak_) missilesPeak_ = missiles_.size();
    }
    if (quiver) {
        --quiver->charges;
        SendCharges(s, *quiver, out);
        if (quiver->charges == 0) {
            // `delayedDeleteObject_4E5CC0` → 가방에 있으면 `sub_4ED0C0`(`GAME3_3.c:6304~6366` · Go `object.go:67~69`) —
            //   벗기(a3 = 0 · 살아 있으니 a4 = 1 — 본인 알림 없이 남에게만) → 본인에게 `4d <번호2> <종류2>` → 가방에서 뺌
            const uint16_t qnc = quiver->netCode, qtype = quiver->type;
            PlayerDequipWeapon(s, qnc, out, /*toSelf=*/false, /*toOthers=*/true);
            const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_DROP), static_cast<uint8_t>(qnc), static_cast<uint8_t>(qnc >> 8),
                                     static_cast<uint8_t>(qtype), static_cast<uint8_t>(qtype >> 8) };
            EmitSeq(s, msg, sizeof(msg), out);
            for (size_t i = 0; i < s.inventory.size(); ++i)
                if (s.inventory[i].netCode == qnc) { s.inventory.erase(s.inventory.begin() + static_cast<long>(i)); break; }
        }
    }
    PlaySoundAt(kind == 4 ? 885 : 886, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
    ++bowShot_[static_cast<size_t>(BowShot::Shot)];
}

// ── ★ 09-29 갑옷 입기·벗기 · 방어값 ─────────────────────────────────────
//
// `nox_xxx_playerEquipArmor_53E650`(`GAME4_3.c:7508~7567`) — 사람 갈래.
//   갑옷 클래스(0x2000000) 아님 → 0 · 이미 입음 → 0 · 같은 부위(SUBCLASS 가 같은 입은 갑옷 — `armorHaveSameSubclass_53E7B0`)를 기억 ·
//   직업(modifier.bin CLASSUSE — 정의가 없으면 0 · `57B3D0`) → 글 `armor.c:ArmorEquipClassFail` + [a4 면 소리 925(kind 2)] · 0 ·
//   힘(REQUIRED_STRENGTH · `4F3180`) → 글 `armor.c:ArmorEquipStrengthFail` + 소리 · 0 · 같은 부위를 벗김(`53E430(1,1)`) ·
//   입음 깃발 · 갑옷 비트 · 알림(`4D8540`) · 방어값 다시(`53E300`) · 방패(SUBCLASS & 2)면 든 두 손 무기 내려놓기(`sub_53E600`)
//   ⚠ 안 옮긴 것 — 마법부여 켜기(`4F2FF0` — 우리 물건에 마법부여 없음) · `sub_53E2D0`(0x10000000 깃발 — 쓰는 곳 못 찾음)
bool Server::PlayerEquipArmor(Session& s, uint16_t nc, bool failSound, std::vector<OutPacket>& out) {
    auto gate = [&](ArmorGate g) { ++armorGate_[static_cast<size_t>(g)]; return g == ArmorGate::Equipped; };
    Session::Carried* item = nullptr;
    for (auto& c : s.inventory) if (c.netCode == nc) { item = &c; break; }
    if (!item || !IsArmorItem(*item)) return gate(ArmorGate::NotArmor);
    if (item->equipped) return gate(ArmorGate::Already);
    uint16_t sameNet = 0;
    for (const auto& c : s.inventory)
        if (c.netCode != nc && c.equipped && IsArmorItem(c) && c.subclass == item->subclass) { sameNet = c.netCode; break; }
    auto refuse = [&](const char* key, ArmorGate g) {
        SendPriMsg(s, key, out);
        if (failSound) PlaySoundAt(925, static_cast<float>(s.posX), static_cast<float>(s.posY), /*kind=*/2, s.netCode, s.addr);
        return gate(g);
    };
    const auto d = armorDef_.find(item->type);
    const uint8_t classes = (d == armorDef_.end()) ? 0 : d->second.classes;
    if (!(classes & (1u << static_cast<uint8_t>(s.charClass)))) return refuse("armor.c:ArmorEquipClassFail", ArmorGate::ClassFail);
    if (s.strength < static_cast<int>(d->second.reqStrength)) return refuse("armor.c:ArmorEquipStrengthFail", ArmorGate::StrengthFail);
    if (sameNet != 0) { PlayerDequipArmor(s, sameNet, out); ++armorGate_[static_cast<size_t>(ArmorGate::Swapped)]; }
    for (auto& c : s.inventory) if (c.netCode == nc) { item = &c; break; }   // 벗기가 가방을 안 바꾸지만 다시 잡아 둔다
    item->equipped = true;
    ReportEquipChange(s, *item, true, out);
    if (item->subclass & 2) {                                                // `sub_53E600` — 방패면 든 두 손 무기(0x7FFE40C)를 내려놓는다
        std::vector<uint16_t> twoHand;
        for (const auto& x : s.inventory)
            if (x.equipped && x.weaponClass && (x.equipBit & 0x7FFE40C)) twoHand.push_back(x.netCode);
        for (uint16_t wnc : twoHand)
            if (PlayerDequipWeapon(s, wnc, out)) ++shieldDropsTwoHand_;
    }
    return gate(ArmorGate::Equipped);
}

// `sub_53E430`(`GAME4_3.c:7390~7441`) — 사람 갈래. 갑옷 아님·안 입음·가방에 없음 → 0 · 입음 깃발·갑옷 비트 끔 ·
//   a3 면 본인 알림(`4D8590`) · a4 면 모두 알림(`4D84C0`) · 방어값 다시 · 마법부여 끄기(없음) ·
//   방패(SUBCLASS & 2)이고 자세가 막기(15·16·17)면 자세 13
bool Server::PlayerDequipArmor(Session& s, uint16_t nc, std::vector<OutPacket>& out, bool toSelf, bool toOthers) {
    auto gate = [&](ArmorGate g) { ++armorGate_[static_cast<size_t>(g)]; return g == ArmorGate::Dequipped; };
    Session::Carried* item = nullptr;
    for (auto& c : s.inventory) if (c.netCode == nc) { item = &c; break; }
    if (!item || !IsArmorItem(*item) || !item->equipped) return gate(ArmorGate::NotWorn);
    item->equipped = false;
    ReportEquipChange(s, *item, false, out, toSelf, toOthers);
    if ((item->subclass & 2) && (s.moveState == 15 || s.moveState == 16 || s.moveState == 17)) {
        PlayerSetState(s, kPlayerStateIdle, frame_, &out);
        ++armorGate_[static_cast<size_t>(ArmorGate::ShieldBlockIdle)];
    }
    return gate(ArmorGate::Dequipped);
}

// `nox_xxx_recalculateArmorVal_53E300`(`GAME4_3.c:7327~7354`) — 가방을 **앞(새것)부터** 돌며 입은 갑옷의 방어값(`itemApplyDefendEffect_415C00` =
//   modifier.bin ARMOR_VALUE · 마법부여 콜백 +76 은 없음)을 float 로 더하고 1.0 을 넘으면 1.0.
//   ⚠ 우리 가방은 뒤에 붙이므로(push_back) 거꾸로 돈다 — 순서가 float 값을 바꾼다(3-d · 위저드 0x3e6b8520)
//   정의가 없는 종류는 옛 표(`ItemArmorValue` — 기본 장비 다섯)로 · 기본 장비를 가방에 안 실은 시험 서버는 직업 표
float Server::ArmorValueOf(const Session& s) const {
    if (!s.gearInBag) return ArmorValueFloat(s.charClass);
    float v = 0.0f;
    for (auto it = s.inventory.rbegin(); it != s.inventory.rend(); ++it) {
        if (!it->equipped || !IsArmorItem(*it)) continue;
        const auto d = armorDef_.find(it->type);
        v = (d != armorDef_.end() ? d->second.value : ItemArmorValue(it->type)) + v;
    }
    if (v > 1.0) v = 1.0f;
    return v;
}

// 든 무기의 종류 번호 — 죽음 알림의 무기 그림(3-d). 시험용 바꿔 들기는 main 이 채운 이름 표
//   ★ 09-27 B2 — 그 밖엔 무기 칸의 물건 종류(기본 장비를 못 실은 시험 서버만 직업 표)
uint16_t Server::WeaponWireOf(const Session& s) const {
    if (s.debugWeapon >= 0) {
        const auto it = weaponWire_.find(static_cast<uint32_t>(s.debugWeapon));
        return it == weaponWire_.end() ? 0 : it->second;
    }
    if (!s.gearInBag) return LoadoutFor(s.charClass).weapon;
    const Session::Carried* h = HeldWeapon(s);
    return h ? h->type : 0;
}

// 플레이어 +4 WeaponEquip — 든 무기의 클래스 비트.
//   ★ 09-27 B2 — 무기 칸 물건의 비트(= 정답지 `weaponInventoryEquipFlags_415820` — 같은 표 `object_weapon.go:108~135`).
//   수치 표(kWeapons)에 없는 무기(활·지팡이류·차크람)도 비트는 선다 — 전에는 표에 없으면 0(맨손)이었다
uint32_t Server::WeaponEquipOf(const Session& s) {
    if (s.debugWeapon >= 0 || !s.gearInBag) {
        const WeaponStats* w = WeaponStatsOf(s);
        return w ? w->classBits : 0u;
    }
    const Session::Carried* h = HeldWeapon(s);
    // ★ 09-29 — 든 화살통은 무기 칸(+104)에 안 들어가고 비트(2)만 선다(`playerEquipWeapon_53A420` `:4971·4973`) — 원본 +4 는 그 합이다
    uint32_t quiver = 0;
    for (const auto& c : s.inventory) if (c.equipped && c.weaponClass && c.equipBit == 2) { quiver = 2; break; }
    return (h ? h->equipBit : 0u) | quiver;
}
// 든 무기의 modifier 값 — 시험용 바꿔 들기(DebugSetWeapon)가 있으면 그 이름, 없으면 무기 칸(09-27 B2 · 시험 서버는 직업 기본 장비)
//   ⚠ 수치 표에 없는 무기면 nullptr — 부르는 자리가 비트(WeaponEquipOf)로 먼저 가른다
const WeaponStats* Server::WeaponStatsOf(const Session& s) {
    if (s.debugWeapon >= 0) {
        if (s.debugWeapon == 0) return nullptr;             // 맨손
        for (const auto& w : kWeapons)
            if (static_cast<uint32_t>(s.debugWeapon) == w.classBits) return &w;
        return nullptr;
    }
    if (!s.gearInBag) return WeaponOf(s.charClass);
    const Session::Carried* h = HeldWeapon(s);
    if (!h) return nullptr;
    for (const auto& w : kWeapons)
        if (h->equipBit == w.classBits) return &w;
    return nullptr;
}

// `GAME4.c:1395` weaponGetStaminaByType_4F7E80 — 무기 비트 → 깎는 기력
static int WeaponStaminaCost(uint32_t equip) {
    if (equip & 0x200) return 70;
    if (equip & 0x4000) return 100;
    if (equip & 0x800) return 50;
    if (equip & 0x100) return 45;
    if (equip & 0x1000) return 75;
    if (equip & 0x2000) return 100;
    if (equip & 0x7FF8000) return 45;
    return (equip & 0x400) ? 75 : 10;
}

// `GAME3_2.c:4124` netReportStamina_4D8800 — `47 <기력>` 2바이트 · 중요(netSendPacket1 → 4E5030 · cc)
void Server::ReportStamina(Session& s, std::vector<OutPacket>& out) {
    const uint8_t msg[2] = { static_cast<uint8_t>(Op::REPORT_STAMINA), s.stamina };
    EmitSeq(s, msg, sizeof(msg), out);
}

// `server/network.go:629~645` earthquakeSend — 자리에서 300 안의 사람마다 `97 <흔들림>`(비신뢰 · 목록 Kind1)
void Server::EarthquakeSend(float x, float y, int jiggle, std::vector<OutPacket>& out) {
    ++fxSent_;                                             // 조각 D — FX 는 건수로 센다
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const double dx = static_cast<double>(x - static_cast<float>(o.x));
        const double dy = static_cast<double>(y - static_cast<float>(o.y));
        const double dd2 = dy * dy + dx * dx;
        if (dd2 >= 90000.0) continue;
        const uint8_t msg[2] = { static_cast<uint8_t>(Op::FX_JIGGLE),
                                 static_cast<uint8_t>(static_cast<int8_t>(static_cast<long long>((1.0 - dd2 * 1.1111111e-05) * jiggle))) };
        Emit(o, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
}

// `GAME4.c:1588` playerInputAttack — 겨눈 것이 적(또는 빈 곳)일 때만.
void Server::PlayerInputAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    if (CursorOnFriend(s)) {                                // playerAimsAtEnemy_4F9DC0 가 0
        std::printf("[공격] 프레임 %u %s 커서 아래가 편 — 안 때림\n", frame, s.addr.c_str());
        return;
    }
    const uint32_t equip = WeaponEquipOf(s);
    if (equip) {
        // ⚠ 마법 지팡이(0x47F0000 · 충전 쓰기 useByNetCode)는 주문 묶음(4)에서. 기본 장비에 없다.
        if (s.moveState != kPlayerStateAttack) {
            const int cost = WeaponStaminaCost(equip);
            if (s.stamina >= cost) {
                s.stamina = static_cast<uint8_t>(s.stamina - cost);
                ReportStamina(s, out);
                s.stateFrame = frame;                       // a1[34] = 프레임 — setState 보다 먼저
                s.animFrame = 0;
                const bool entered = PlayerSetState(s, kPlayerStateAttack, frame, &out);
                if (!entered) s.stamina = static_cast<uint8_t>(s.stamina + cost);   // sub_4F7DB0 되돌림
                std::printf("[공격] 프레임 %u %s 무기 0x%X %s · 기력 %d 깎음 → %u\n", frame, s.addr.c_str(), equip,
                            entered ? "상태 1 들어감" : "쉬는 시간이라 안 들어감(기력 되돌림)", cost, s.stamina);
            } else {
                std::printf("[공격] 프레임 %u %s 무기 0x%X 기력 모자람 %u < %d\n", frame, s.addr.c_str(), equip, s.stamina, cost);
            }
            std::fflush(stdout);
        }
        // 투명(0)·무적(23) 끄기 · 타원 방패(67) 취소 — 무기 갈래는 **클릭마다** 끈다(`:1629~1631`)
        BuffOff(Target{ s.addr, -1 }, 0);
        BuffOff(Target{ s.addr, -1 }, 23);
        DurCancelFor(67, s.addr, out);                     // spellCancelDurSpell_4FEB10(67)
        return;
    }
    if (s.moveState != kPlayerStateAttack) {
        const bool entered = PlayerSetState(s, kPlayerStateAttack, frame, &out);
        std::printf("[공격] 프레임 %u %s %s 애니 %u\n", frame, s.addr.c_str(),
                    entered ? "상태 1 들어감" : "쉬는 시간이라 안 들어감", s.attackAnim);
        std::fflush(stdout);
    }
}

// `GAME4.c:1637` playerAimsAtEnemy 의 거꾸로 — 커서 아래 것(findObjectAtCursor_54AF40)이 **적이 아니면** 참.
//   퀘스트(게임 깃발 4096)면 늘 적으로 본다. 커서 오브젝트 고르기(`GAME5.c:3547~3640`) —
//   커서 100 안 · 자기 아님 · 죽음/지워짐 아님 · 클래스 0x80000206 · 시야선 · 관전자 아님 ·
//   원 몸이면 (x, y−z) 중심 반지름 안(int 로 자른 r²) · 여럿이면 y+z 가 가장 큰 것(화면에서 앞)
bool Server::CursorOnFriend(const Session& s) const {
    if (questTotal_ > 0 || questStage_ > 0) return false;   // gameFlags 4096
    const Target t = FindObjectAtCursor(s);
    if (t.none()) return false;
    return !TargetIsEnemy(s, t);
}

// findObjectAtCursor_54AF40 — 커서 둘레 100 · 시야선 · 몸 안에 커서 · 가장 아래(y 큰) 것. (3-a 에서 CursorOnFriend 안에 옮긴 것을 떼어 냄 · 4-a)
Target Server::FindObjectAtCursor(const Session& s) const {
    Target found;
    if (!s.hasAim) return found;
    const float cx = static_cast<float>(s.aimX), cy = static_cast<float>(s.aimY);
    float best = 0.0f;
    auto consider = [&](float ox, float oy, float r) -> bool {
        const float dx0 = ox - cx, dy0 = oy - cy;
        if (dx0 * dx0 + dy0 * dy0 > 100.0f * 100.0f) return false;         // EachObjInCircle 100
        if (!MapTraceVision(ox, oy, static_cast<float>(s.posX), static_cast<float>(s.posY))) return false;   // mapCheck(후보, 사람)
        const int r2 = static_cast<int>(r * r);
        const float ddx = cx - ox, ddy = cy - oy;                            // z = 0
        if (static_cast<float>(r2) <= ddy * ddy + ddx * ddx && cy != oy) return false;
        if (cy == oy && !(cx > ox - r && cx < ox + r)) return false;
        const float score = oy;                                              // +104 z + +60 y
        if (score <= best) return false;
        best = score;
        return true;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (&o == &s || !o.inGame || o.respawnAtFrame != 0 || o.observer) continue;   // 0x8020 · 관전(3680&1)
        if (o.buffs.Has(0) && !s.buffs.Has(21)) continue;  // 투명이면 적외선일 때만(`GAME5.c:3588`, 4-b1)
        if (consider(static_cast<float>(o.posX), static_cast<float>(o.posY), static_cast<float>(kBodyRadius))) {
            found = Target{ o.addr, -1 };
        }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        const Creature& k = pets_[i];
        if (k.dead) continue;                                // ★ 10-01 시체 — `playerCursorScanFn_54AFB0` +16 & 0x8020(`GAME5.c:3587`)
        auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end() || tb->second.shape != 2) continue;   // 상자 몸 몬스터는 map_57B850(아직 없음)
        if (k.buffs.Has(0) && !s.buffs.Has(21)) continue;  // 투명이면 적외선일 때만(4-b1)
        if (consider(static_cast<float>(k.x), static_cast<float>(k.y), tb->second.r)) found = Target{ std::string(), static_cast<int>(i) };
    }
    return found;
}

// IsEnemyTo(사람, 대상) — 사람이면 팀 규칙 · 소환수면 주인 기준(맵 몬스터는 늘 적 · 내 것은 친구)
bool Server::TargetIsEnemy(const Session& s, const Target& t) const {
    if (!t.addr.empty()) {
        if (t.addr == s.addr) return false;
        auto it = sessions_.find(t.addr);
        return it != sessions_.end() && IsEnemyTo(s, it->second);
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return false;
    const Creature& k = pets_[static_cast<size_t>(t.pet)];
    if (k.owner.empty()) return true;
    if (k.owner == s.addr) return false;
    return IsEnemyTo(s.addr, k.owner);
}

// ★ 09-23 nine-defects 2-a2 — **함성 외침 효과**(`GAME4_3.c:3991~4000` — 함성 애니가 칸 2 → 3 을 넘는 틱).
//   ① 땅울림 15(`earthquakeSend_4D9110`)
//   ② 둘레 300 안(중심 거리 · `EachObjInCircle`)의 **몬스터** 중 서브클래스 `WARCRY_STUN`(0x20000)이고
//      죽거나 사라지지 않은 것(+16 & 0x8020)에 붙잡힘(버프 5)을 90틱 · 세기 3(`warcryStunMonsters_539B90` `GAME4_3.c:4444~4457`).
//      편은 안 가린다 — 정답지 콜백에 편 검사가 없다(내 소환수도 걸린다). 사람은 몬스터 클래스가 아니라 빠진다
//   ⛔ ③ 반주문 13(`castCounterSpell_52BBB0`)은 2-a3
void Server::WarcryShout(Session& s, std::vector<OutPacket>& out) {
    EarthquakeSend(static_cast<float>(s.posX), static_cast<float>(s.posY), 15, out);
    constexpr uint32_t kWarcryStunBit = 1u << 17;          // MonsterClassBitsOf 표의 17번째 = WARCRY_STUN
    const double r2 = 300.0 * 300.0;
    for (size_t i = 0; i < pets_.size(); ++i) {
        const Creature& k = pets_[i];
        if (k.health == 0) continue;                         // 죽은 것(0x8000)
        const double dx = k.x - s.posX, dy = k.y - s.posY;
        if (dx * dx + dy * dy > r2) continue;
        const auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end() || !(tb->second.subclass & kWarcryStunBit)) continue;
        ApplyEnchant(Target{ std::string(), static_cast<int>(i) }, 5, 90, 3);
        ++warcryStunned_;
    }
    CastCounterSpell(s, out);                                // ★ 2-a3 — ③ 반주문 13(`castCounterSpell(13, a1, a1, a1)`)
}

// ★ 09-23 nine-defects 2-a4 — `nox_xxx_spell_4FE680`(`GAME4.c:3267~3318`) — 함성이 **둘레 적의 시전을 끊는다**.
//   책 시전 목록(`castList_` = magicEnt)을 돌며 — 시전자가 사람이 아니거나 **같은 편이 아니면**(`servCompareTeams` — 둘 다 0 아니고 같아야 같은 편)
//   거리+0.1 < dist 이고 워리어→시전자 시야(`mapCheck` = `MapTraceVision`)가 트이면 그 칸을 빼고,
//   시전자가 사람이면 +216(`castFrame`)·+188(`castFlag`) 을 0 · 알림(0, 15) · 소리 231 · 상태 13
void Server::WarcryBreakCasts(Session& s, double dist, std::vector<OutPacket>& out) {
    for (size_t i = castList_.size(); i-- > 0;) {
        auto ci = sessions_.find(castList_[i].caster);
        if (ci == sessions_.end()) continue;
        Session& c = ci->second;
        const bool sameTeam = (s.team != 0 && c.team != 0 && s.team == c.team);
        if (sameTeam) continue;                              // 목록의 시전자는 늘 사람이다(몬스터 시전은 묶음 6)
        const double dx = c.posX - s.posX, dy = c.posY - s.posY;
        if (!(std::sqrt(dx * dx + dy * dy) + 0.1 < dist)) continue;
        if (!MapTraceVision(static_cast<float>(s.posX), static_cast<float>(s.posY),
                            static_cast<float>(c.posX), static_cast<float>(c.posY))) continue;
        c.castFrame = 0;
        c.castFlag = 0;
        InformSpell(c, 0, 15, out);
        PlaySoundAt(231, static_cast<float>(c.posX), static_cast<float>(c.posY), 0, 0, std::string(), Target{ c.addr, -1 });
        PlayerSetState(c, kPlayerStateIdle, frame_);
        castList_.erase(castList_.begin() + static_cast<std::ptrdiff_t>(i));
        ++warcryCastsBroken_;
    }
}

// ★ 09-23 nine-defects 2-a3 — `nox_xxx_castCounterSpell_52BBB0`(`spell_counter.go:37~80`) — **반주문**.
//   함성 외침이 `(13, a1, a1, a1)` 로 부른다 — 둘레의 가운데이자 빼는 것(a4)이 시전자다. 둘레 = gamedata `CounterspellRange`.
//   ① 둘레 안(중심 거리 · `EachMissileInCircle`)의 날아가는 것 중 서브클래스 `MISSILE_COUNTERSPELL` 인 것(`cspellRemoveSpell_52BC90` `:13~35`) —
//      반주문 폭발 FX(0x87)를 그 자리에 내고, `Magic` 은 그냥 지우고, 나머지는 **대상 없이 닿음 함수를 부른 뒤** 지운다
//      (불덩이 = 그 자리에서 터짐 · 픽시 = 대상도 벽도 없으면 지워짐 `GAME3_3.c:4390~4392`).
//      ⚠ 죽음의 공(DeathBall → 조각 뿌리기) 갈래는 우리에게 그 오브젝트가 없어 없다
//   ② 지속 주문 중 `CAN_COUNTER`(0x40000)이고 (+88 & 2 가 없거나 연쇄 번개(43)면) 자리(+28)가 둘레 안인 것을 끊는다
//   ③ 둘레 안 오브젝트(시전자 빼고 · 안 죽은 것)의 버프 0~31 중 **지속이 0 이 아니고**(0 = 영구) 그 버프의 주문이 `CAN_COUNTER` 면 끈다.
//      한 오브젝트에서 하나라도 끄면 그 자리에 FX(0x87) 한 번
//   ④ 반주문(13) 시전 소리를 시전자 자리에서
int Server::CastCounterSpell(Session& caster, std::vector<OutPacket>& out) {
    ++counterCasts_;
    const double r = BalanceFloat("CounterspellRange");
    const double r2 = r * r;
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    auto fx = [&](float px, float py) {                      // netSendPointFx_522FF0(MSG_FX_COUNTERSPELL_EXPLOSION)
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_COUNTERSPELL_EXPLOSION),
                                 static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                 static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    };
    auto inRange = [&](double x, double y) { const double dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r2; };
    constexpr uint32_t kCanCounter = 0x40000;               // things.SpellCanCounter

    // ① 날아가는 것 — 지우면 색인이 밀리므로 뒤에서부터
    for (size_t i = missiles_.size(); i-- > 0;) {
        if (i >= missiles_.size()) continue;                 // 불덩이가 터지며 목록이 줄었을 수 있다
        MissileObj& m = missiles_[i];
        if (counterMissileTypes_.find(m.type) == counterMissileTypes_.end()) continue;
        if (!inRange(m.posX, m.posY)) continue;
        fx(m.posX, m.posY);
        ++counterMissiles_;
        const uint16_t nc = m.netCode;
        if (m.type != magicType_.type && m.collideKind == 1) FireballExplode(m, Target{}, out);   // SparkExplosionCollide(0,0)
        // 픽시(2)·Magic·그 밖 — 대상 없는 닿음이 아무 일도 안 하거나 지우기뿐이다. 아직 남아 있으면 지운다
        if (i < missiles_.size() && missiles_[i].netCode == nc) DestroyMissile(i, out);
    }
    // ② 지속 주문 — `DurCancel` 은 표시만 하고 지우기는 뒤에 한다(돌면서 불러도 된다)
    //   ⚠ 이미 끊긴 것(+88 & 1)도 거르지 않는다 — 정답지 반주문·`CancelSpell`(`server/spells_dur.go:177~198`) 둘 다 그 검사가 없다
    for (auto& d : durSpells_) {
        if (!(SpellFlags(d.spell) & kCanCounter)) continue;
        if ((d.flags88 & 2) && d.spell != 43) continue;
        if (!inRange(d.posX, d.posY)) continue;
        DurCancel(d, out);
        ++counterDurs_;
    }
    // ③ 버프 — 버프 번호 → 그 버프를 거는 주문(`spells_enchant.go:71~104`, 0 = 주문 없음)
    static const uint8_t kEnchantSpell[32] = {
        39, 54, 3, 12, 72, 74, 0, 0, 70, 36, 131, 26, 0, 130, 1, 42,
        15, 62, 64, 63, 61, 37, 71, 40, 127, 32, 51, 67, 9, 55, 0, 0,
    };
    auto strip = [&](const Target& t, float tx, float ty) {
        Buffs* b = BuffsOf(t);
        if (!b) return;
        bool shown = false;
        for (int e = 0; e < 32; ++e) {
            if (!b->Has(e) || b->dur[e] == 0) continue;
            const uint8_t sp = kEnchantSpell[e];
            if (sp == 0 || !(SpellFlags(sp) & kCanCounter)) continue;
            if (!shown) { fx(tx, ty); shown = true; }
            BuffOff(t, e);
            ++counterEnchants_;
        }
    };
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (&o == &caster || !o.inGame || o.respawnAtFrame != 0) continue;   // a4 빼고 · 0x8020
        if (!inRange(o.posX, o.posY)) continue;
        strip(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        if (pets_[k].health == 0 || !inRange(pets_[k].x, pets_[k].y)) continue;
        strip(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
    }
    // ④ 시전 소리
    if (spellDefs_[13].valid && spellDefs_[13].castSound != 0) PlaySoundAt(spellDefs_[13].castSound, cx, cy, 0, 0, std::string(), Target{ caster.addr, -1 });
    return 1;
}

// `GAME4_3.c:3855` playerAttack — 맨손(`:4020~4066`) · 무기(`:4067~4419`) · 끝(LABEL_159 `:4420~4439`). 참 = 아직 공격 중.
//   무기 갈래는 **애니 한가운데 칸(frames/2)** 에 한 번 판정한다(지난 틱 칸보다 클 때만 — 같은 칸 두 번 방지).
//   ⚠ 옮기지 않은 갈래(다른 묶음): 활·석궁(0x4·0x8 화살 오브젝트 5) · 차크람·수리검(0x40·0x80 날아가는 오브젝트 5)
//     · 마법 지팡이 0x47F0000(주문 4) · 무기 마법 효과 itemApplyAttackEffect(5) · 광전사 돌진(nine-defects 2-b)
//   ✖ 09-23 정정 — 「함성(능력 4)」도 안 옮긴 갈래로 적혀 있었다. 함성은 nine-defects 2-a1·2-a2 에서 옮겼다(아래 함성 갈래)
bool Server::PlayerAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    const uint32_t equip = WeaponEquipOf(s);
    const WeaponStats* w = WeaponStatsOf(s);
    const uint8_t prevFrame = s.animFrame;                  // v76 아래 바이트(+236 지난 틱 값)
    int frames = 0, hold = 0, idx = 0;
    auto range = [&](int anim) {
        frames = (anim >= 0 && anim < kPlayerAnimSlots) ? playerAnimFrames_[anim][0] : 0;
        hold = (anim >= 0 && anim < kPlayerAnimSlots) ? playerAnimFrames_[anim][1] : 0;
    };
    auto calcIdx = [&]() { idx = static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1)); };
    auto logJudge = [&](const AttackInfo& ai, bool hit, int anim) {
        ++melees_;
        std::printf("[공격] 프레임 %u %s 판정(시작 %u · 칸 %d/%d) 애니 %d 피해 %d 종류 %u 사거리 %.0f → %s\n", frame,
                    s.addr.c_str(), s.stateFrame, idx, frames, anim, ai.dmg, ai.type, ai.range, hit ? "닿음" : "빗나감");
        std::fflush(stdout);
    };
    // calcBoltDamage_4EF1E0 — (힘 − 요구힘) × 계수 + 최소 · float 로 담고 sub_538510 이 +0.5 버림
    auto weaponDmg = [&]() {
        const float f = static_cast<float>(static_cast<double>(s.strength - w->reqStrength) *
                                           static_cast<double>(static_cast<float>(w->coeff)) + w->damageMin);
        return static_cast<int>(static_cast<long long>(static_cast<double>(f) + 0.5));
    };
    auto meleeInfo = [&](uint8_t type, bool overlap) {
        AttackInfo ai;
        ai.dmg = weaponDmg();
        ai.type = type;
        ai.range = static_cast<float>(kBodyRadius) + static_cast<float>(w->range);
        ai.px = static_cast<float>(s.posX); ai.py = static_cast<float>(s.posY);
        ai.hitsOverlap = overlap;
        ai.weapon = true;
        return ai;
    };
    auto setReady = [&]() {
        if (s.attackReadyFrame == 0) s.attackReadyFrame = frame + static_cast<uint32_t>(frames * (hold + 1));
    };
    auto midHit = [&]() { return idx == frames / 2 && idx > prevFrame; };
    // ★ 09-23 nine-defects 4-a — **휘두르기 소리**(`GAME4_3.c:4061~4285` 아홉 자리). 판정(playerTraceAttack)이 **아무것에도 안 닿으면**
    //   그 갈래의 빗나감 소리를 낸다 — 맨손·지팡이·창 879 PunchMissing · 0x200 880 Longsword · 0x100·0x400 881 Sword ·
    //   0x800 884 Mace · 0x3000 883 Axe. 망치(0x4000)만 **닿든 말든** 882 Hammer(땅울림 뒤 · `:4236~4240`).
    //   `aud_501960(번호, 사람, 0, 0)` — 사람 자리에서 모두에게. 전에는 판정만 하고 소리 자리가 「묶음 10」 으로 비어 있었다
    auto swingSound = [&](uint16_t id) {
        PlaySoundAt(id, static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
        if (id >= 879 && id <= 884) ++swingSound_[id - 879];
    };
    auto judge = [&](const AttackInfo& ai, int anim, uint16_t missSound) {
        const bool hit = PlayerTraceAttack(s, ai, out);
        logJudge(ai, hit, anim);
        if (!hit) swingSound(missSound);
    };

    // ★ 09-23 nine-defects 2-a1 — **함성 갈래**(`GAME4_3.c:3988~4005`) — 맨손·무기 갈래보다 먼저 본다.
    //   함성이 켜져 있고 켜짐 값이 서 있으면(외치는 애니가 도는 중) 애니 46(WARCRY) 칸을 센다.
    //   · 칸이 2 → 3 으로 넘어가는 틱(정답지 `v76 == 770` — 아래 바이트 = 지난 칸 2 · 위 바이트 = 이 칸 3)에
    //     외침 효과(땅울림 15 · 둘레 300 몬스터 기절 · 반주문 13)가 난다 — ⛔ 효과는 2-a2·2-a3 에서 옮긴다. 지금은 센다
    //   · 칸이 다 차면 켜짐 **값**만 내린다(`sub_4FC440`) → 이 틱에 거짓을 돌려주므로 상태 13(ImplA)
    //   전에는 이 갈래가 없어 함성을 쓰면 상태 1 로 **보통 주먹질**이 나갔다(사람 판 「S 발동 안 함」)
    //   ⚠ 광전사 갈래(`:4006~4023`)는 2-b 에서 — 순서상 이 아래에 들어간다
    if (AbilityIsActiveVal(s, 2)) {                          // `IsActive(2) && IsActiveVal(2)` — 값은 켜짐을 품는다
        range(46);
        calcIdx();
        if (prevFrame == 2 && idx == 3) {
            ++warcryShouts_;
            WarcryShout(s, out);                             // ★ 2-a2 — 땅울림 · 몬스터 기절(반주문은 2-a3)
        }
        if (idx >= frames) AbilityValOff(s, 2);
        s.animFrame = static_cast<uint8_t>(idx >= frames ? frames - 1 : idx);   // LABEL_159
        return idx < frames;
    }
    // ★ 09-23 nine-defects 2-b1 — **광전사 갈래**(`GAME4_3.c:4006~4023`) — 함성 갈래 다음, 맨손·무기 갈래보다 먼저.
    //   켜져 있으면(목록에 있기만 하면 — `IsActive`) 얼림(25)·붙잡힘(5)이 아닌 한 **돌진한다** —
    //   · SpeedCur = SpeedBase × 6 · 힘(ForceVec) += 바라보는 쪽(Direction1) × 그 빠르기 — 매 틱
    //   · 애니 45(BERSERKER_CHARGE) 칸 = (프레임 − 상태 시작) ÷ (붙듦+1) · **칸 수−1 이상이면 0**(애니를 되돌린다 — 그래서 늘 「아직 공격 중」)
    //   · 얼림·붙잡힘이면 거짓(공격 끝 → 상태 13)
    //   끝은 지속(90)이 다할 때 `AbilityUpdate` 가 상태 13 으로 내리거나(`spells_abil.go:252~254`), 부딪혀 멈출 때(2-b2)다.
    //   전에는 이 갈래가 없어 광전사를 쓰면 상태 1 로 **보통 주먹질**이 나갔다(사람 판 「A 발동 안 함」)
    if (AbilityIsActive(s, 1)) {
        if (s.buffs.Has(25) || s.buffs.Has(5)) return false;
        range(45);
        idx = static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
        const double speed = static_cast<double>(kPlayerWalkPerFrame) * 6.0;   // SpeedBase(+548) × 6 — 우리 SpeedBase 는 걷기 기본값
        s.speedCur = static_cast<float>(speed);
        s.forceX += DirVecX(s.facingPrev) * speed;            // Direction1(+124) — 이 틱 시작 때의 바라보는 쪽
        s.forceY += DirVecY(s.facingPrev) * speed;
        ++berserkChargeTicks_;
        if (idx >= frames - 1) idx = 0;
        s.animFrame = static_cast<uint8_t>(idx >= frames ? frames - 1 : idx);   // LABEL_159
        return idx < frames;
    }

    if (equip == 0) {
        if (s.attackAnim == 0) {
            // `:3903~3916` — 애니가 비었으면 여기서 뽑는다(`!class && rand(0,100) >= 75` → 25)
            s.attackAnim = static_cast<uint8_t>(kAnimPunchLeft + NextRandom(2));
            if (s.charClass == CharClass::Warrior && NextRandom(101) >= 75) s.attackAnim = kAnimPunchHook;
        }
        const int anim = s.attackAnim;
        range(anim);
        setReady();
        calcIdx();
        if (idx >= frames) {
            AttackInfo ai;
            const int add = (anim == kAnimPunchLeft || anim == kAnimPunchRight) ? kPunchDamageMin
                          : (anim == kAnimPunchHook) ? kKickDamageMin : 0;
            const float coeff = (anim >= kAnimPunchLeft && anim <= kAnimPunchHook) ? kUnarmedCoeffF : 0.0f;
            const float dmg = static_cast<float>(static_cast<double>(s.strength) * coeff + add);
            ai.dmg = static_cast<int>(static_cast<long long>(static_cast<double>(dmg) + 0.5));
            ai.type = 10;
            ai.range = static_cast<float>(kBodyRadius) + kUnarmedReachExtra;
            ai.px = static_cast<float>(s.posX); ai.py = static_cast<float>(s.posY);
            ai.anim = static_cast<uint8_t>(anim);
            judge(ai, anim, 879);                            // `:4061~4064` PunchMissing
        }
    } else if (equip & 0x47F8000) {
        // 지팡이 — 막대(0x8000) 면 29 휘두르기. 마법 지팡이는 +96&2 가 아니면 31(주문 4)
        const int anim = 29;
        range(anim);
        setReady();                                         // 29 면 길이만큼 · 31 이면 지금(`:4075~4079`)
        calcIdx();
        if (idx < frames && midHit()) {
            // ★ 09-27 B2 — 수치 표에 없는 지팡이(마법 지팡이 — 정답지는 31 로 주문을 건다 · 안 옮김)는 판정 없이 센다.
            //   무기 칸이 생기기 전엔 표에 있는 무기만 들 수 있어 이 막이가 필요 없었다(없으면 w 가 비어 죽는다)
            if (!w) { ++weaponNoStats_; }
            else {
            AttackInfo ai = meleeInfo(0, false);
            judge(ai, anim, 879);                            // `:4114~4117`
            }
        }
    } else if (equip & 0x7800000) {
        const int anim = (equip & 0x3800000) ? 32 : 31;    // 32 찌르기만 판정
        range(anim);
        setReady();
        calcIdx();
        if (anim == 32 && midHit()) {
            if (!w) { ++weaponNoStats_; }                   // ★ 09-27 B2 — 오블리비언 넷은 수치 표에 없다(위와 같은 막이)
            else {
            AttackInfo ai = meleeInfo(0, false);
            judge(ai, anim, 879);                            // `:4142~4145`
            }
        }
    } else if (!(equip & 0xC0)) {
        struct Branch { uint32_t bits; int anim; uint8_t type; bool overlap; uint16_t sound; };
        // `:4149~4288` 순서 그대로 — 롱소드 · 칼 · 대검 · 망치 · 철퇴 · 도끼 · ★ 4-a 끝 칸 = 그 갈래의 휘두르기 소리
        static const Branch kBranches[] = {
            { 0x200, 28, 0, false, 880 }, { 0x100, 27, 0, false, 881 }, { 0x400, 37, 0, false, 881 },
            { 0x4000, 39, 2, true, 882 }, { 0x800, 26, 2, true, 884 }, { 0x3000, 35, 0, true, 883 },
        };
        const Branch* br = nullptr;
        for (const auto& b : kBranches) if (equip & b.bits) { br = &b; break; }
        if (br) {
            range(br->anim);
            setReady();
            calcIdx();
            if (midHit()) {
                AttackInfo ai = meleeInfo(br->type, br->overlap);
                if (br->bits == 0x4000) {
                    // 망치 — 앞 35 를 중심으로 반지름 = 무기 사거리(몸 반지름 안 더함) · 둘레 모두 · 땅울림(`:4220~4240`)
                    const float rad = s.facingPrev * 2.0f * 3.14159265f / 256.0f;
                    ai.px = std::cos(rad) * 35.0f + static_cast<float>(s.posX);
                    ai.py = std::sin(rad) * 35.0f + static_cast<float>(s.posY);
                    ai.range = static_cast<float>(w->range);
                    logJudge(ai, PlayerTraceAttack(s, ai, out), br->anim);
                    EarthquakeSend(static_cast<float>(s.posX), static_cast<float>(s.posY),
                                   static_cast<int>(std::lround(static_cast<double>(s.strength) * 0.1)), out);   // nox_float2int
                    swingSound(br->sound);                           // ★ 4-a — 망치는 닿든 말든 882(`:4240`)
                } else {
                    judge(ai, br->anim, br->sound);
                }
            }
        }
        // ★ 09-29 — 활(0x4)·석궁(0x8)(`GAME4_3.c:4288~4324`). 준비 효과(`itemCheckReadinessEffect_4E0960` — 무기 수식)는 우리 물건에 없어 0
        //   활 — 애니 33 · 칸이 (칸 수 − 1) 이상이고 지난 칸보다 크면 한 발(`539BD0`) 뒤 칸 = 칸 수(→ 공격 끝 · 상태 14)
        //   석궁 — 애니 34 · 칸이 1 이 되는 틱(지난 칸 0)에 한 발 · 애니는 끝까지 돈다
        else if (equip & 0x4) {
            range(33);
            setReady();
            calcIdx();
            if (idx >= frames - 1 && idx > prevFrame) {
                Session::Carried* bow = nullptr;
                for (auto& c : s.inventory) if (c.netCode == s.weaponNet) { bow = &c; break; }
                if (bow) ShootBowCrossbow(s, *bow, out);
                idx = frames;
            }
        } else if (equip & 0x8) {
            range(34);
            setReady();
            calcIdx();
            if (idx == 1 && prevFrame == 0) {
                Session::Carried* bow = nullptr;
                for (auto& c : s.inventory) if (c.netCode == s.weaponNet) { bow = &c; break; }
                if (bow) ShootBowCrossbow(s, *bow, out);
            }
        }
    }
    // 차크람(0x40)·수리검(0x80) — 날아가는 오브젝트라 묶음 5 로
    s.animFrame = static_cast<uint8_t>(idx >= frames ? frames - 1 : idx);   // LABEL_159
    return idx < frames;
}

// ★ 09-23 nine-defects 4-c2 — `Sub_532930`(`server/sound.go:31~161`) 그대로. 소리 이름이 `<때린 것>Hit<맞는 쪽>` 이라
//   (862 MetalHitFlesh = 맞는 쪽 FLESH × 때린 것 METAL) 표를 이름으로 다시 셀 수 있다 — 시험이 그렇게 본다.
//   ⚠ 맞는 쪽 DIAMOND(0x2000) 줄만 이름과 어긋난다(돌에 맞으면 868 DiamondHitStone) — 정답지 그대로 둔다
uint16_t Server::HitSoundFor(uint32_t a2, uint32_t a3) {
    // 때린 것 한 칸 → 번호 — 맞는 쪽 줄마다 (돌 32 · 다이아 0x2000 · 쇠 16 · 살 1/2/4 · 나무 8) 다섯 값
    auto pick = [&](uint16_t stone, uint16_t diamond, uint16_t metal, uint16_t flesh, uint16_t wood) -> uint16_t {
        switch (a3) {
            case 32: return stone;
            case 0x2000: return diamond;
            case 16: return metal;
            case 1: case 2: case 4: return flesh;
            case 8: return wood;
            default: return 0;
        }
    };
    switch (a2) {
        case 32:              return pick(848, 868, 858, 863, 853);   // 맞는 쪽 돌
        case 1: case 2: case 4: return pick(852, 872, 862, 867, 857); // 살·천·가죽
        case 8:               return pick(850, 870, 860, 865, 855);   // 나무
        case 16:              return pick(851, 871, 861, 866, 856);   // 쇠
        case 64: case 1024:   return pick(849, 869, 859, 864, 854);   // 흙·눈
        case 0x2000:          return pick(868, 871, 871, 872, 870);   // 다이아(`:117~141` — 0x2000·16 은 둘 다 871)
        default:              return 0;
    }
}

// ★ 09-23 nine-defects 4-c3a — `Sub_532EC0`(`server/sound.go:9~24`) 부서지는 재질 소리
uint16_t Server::BreakableHitSound(uint32_t mat) {
    switch (mat) {
        case 0x8:    return 875;                           // HitWoodBreakable
        case 0x10:   return 876;                           // HitMetalBreakable
        case 0x20:   return 873;                           // HitStoneBreakable
        case 0x40:   return 874;                           // HitEarthBreakable
        case 0x400:  return 874;                           // (눈도 흙 소리)
        case 0x1000: return 877;                           // HitMagicBreakable
        default:     return 0;
    }
}

// ★ 09-23 nine-defects 4-c3a — `nox_xxx_soundDefaultDamageSound_532E20`(`server/sound.go:169~202`)를 **소환수**에.
//   ① 맞는 쪽이 들린 무기·지팡이면 안 냄(소환수는 해당 없음)
//   ② 때린 것(a3, 없으면 a2)이 없거나 그 재질이 NONE(0x4000)이거나 피해 종류(+524)가 0·2·3·10·11 이 아니면 안 냄(`sub_532F70`)
//   ③ 맞는 쪽에 체력이 있고(최대 ≠ 0) 재질이 나무·돌·흙(8·32·64 — `sub_532FB0`)이면 **부서지는 소리**(`Sub_532EC0`)
//   ④ 아니면 표(`Sub_532930`)를 맞는 쪽 재질 × 때린 것 재질로
//   ★ 4-c3b1 — 「때린 것」은 부르는 자리가 `HitterRef` 로 넘긴다(안 옮긴 자리는 세기만)
void Server::PetDamageSound(const Creature& k, uint8_t dtype, HitterRef hitter) {
    ++defHitSoundCalls_;
    auto way = [&](DefHitSoundWay w) { ++defHitSoundWay_[static_cast<size_t>(w)]; };
    if (MonsterHitSoundOwns(hitter)) { way(DefHitSoundWay::MonsterSound); return; }   // ★ 4-c3b3b `GAME3_2.c:7252~7254`
    if (hitter.kind == HitterRef::NotPorted) { way(DefHitSoundWay::NotPorted); return; }
    const bool hitterPresent = hitter.kind == HitterRef::Type;
    const uint32_t hitterMat = hitterPresent ? MaterialOfWire(hitter.type) : 0u;
    DefHitSoundWay w = DefHitSoundWay::Count;
    const uint16_t id = DefaultHitSound(MaterialOfWire(k.type), TuningFor(k.type).health != 0, dtype, hitterPresent, hitterMat, w);
    way(w);
    if (id == 0) return;
    PlaySoundAt(id, static_cast<float>(k.x), static_cast<float>(k.y), 0, 0, std::string(), PetTarget(k));
    lastHitSound_ = id;
}

// ★ 09-23 nine-defects 4-c3b2b — 작살이 끊길 때의 `532E20(대상, 작살 화살)`(`spell_harpoon.go:173`).
//   피해 함수 밖에서 부르므로 맞는 쪽은 **대상 오브젝트 그대로**다 — 사람이면 NewPlayer 재질(가슴갑옷 안 봄 · `PlayerDamageSound` 와 다르다) ·
//   피해 종류는 대상의 +524(`obj1.Field131`) — 막혀서 이번 피해가 안 들어갔으면 **그 전 값**이 남아 있다
void Server::HarpoonDefaultHitSound(const Target& t, uint16_t boltType) {
    ++harpoonHitSoundCalls_;
    uint32_t mat = 0;
    bool hasHp = false;
    uint8_t dtype = 0;
    float x = 0, y = 0;
    std::string addr;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end()) { ++harpoonHitSoundWay_[static_cast<size_t>(DefHitSoundWay::NotPorted)]; return; }
        mat = MaterialOfWire(newPlayerWire_);
        hasHp = true;
        dtype = it->second.lastDamageType;
        x = static_cast<float>(it->second.posX); y = static_cast<float>(it->second.posY);
        addr = t.addr;
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        const Creature& k = pets_[static_cast<size_t>(t.pet)];
        mat = MaterialOfWire(k.type);
        hasHp = TuningFor(k.type).health != 0;
        dtype = k.lastDamageType;
        x = static_cast<float>(k.x); y = static_cast<float>(k.y);
    } else {
        ++harpoonHitSoundWay_[static_cast<size_t>(DefHitSoundWay::NotPorted)];
        return;
    }
    DefHitSoundWay w = DefHitSoundWay::Count;
    const uint16_t id = DefaultHitSound(mat, hasHp, dtype, boltType != 0, MaterialOfWire(boltType), w);
    ++harpoonHitSoundWay_[static_cast<size_t>(w)];
    if (id == 0) return;
    if (!addr.empty()) PlaySoundAt(id, x, y, 0, 0, addr);
    else PlaySoundAt(id, x, y, 0, 0, std::string(), t);
    lastHitSound_ = id;
}

// ★ 09-23 nine-defects 4-d4 — 발밑 바닥(`tileNFromPoint_411160`) → 재질(FLOR field_36) → 소리. 그 사람 자리에서(`EventObj(snd, u)`)
//   칸 찾기가 −1 이거나 번호가 정의 밖이면 소리 없음(`object_update.go:162·202` 의 `ti >= 0 && ti < len(tiles)`)
void Server::Footstep(const Session& s, bool run) {
    const FloorGrid::Hit hit = floorGrid_.TileAt(static_cast<float>(s.posX), static_cast<float>(s.posY), floorEdges_);
    FootKind k = FootKind::NoTile;
    if (hit.tile >= 0 && static_cast<size_t>(hit.tile) < floorMaterial_.size()) k = FootKindOf(floorMaterial_[static_cast<size_t>(hit.tile)]);
    ++(run ? footRun_ : footWalk_)[static_cast<size_t>(k)];
    const char* nm = FootSoundName(k, run);
    if (!nm) return;
    PlaySoundAt(SoundByName(nm), static_cast<float>(s.posX), static_cast<float>(s.posY), 0, 0, s.addr);
}

// `532E20` 판단부 — 위 주석 ②~④
uint16_t Server::DefaultHitSound(uint32_t mat, bool hasHp, uint8_t dtype, bool hitterPresent, uint32_t hitter, DefHitSoundWay& way) {
    const bool typeOk = dtype == 0 || dtype == 2 || dtype == 3 || dtype == 10 || dtype == 11;   // sub_532F70
    if (!hitterPresent || hitter == 0x4000u || !typeOk) { way = DefHitSoundWay::Filtered; return 0; }
    if (hasHp && (mat == 8 || mat == 32 || mat == 64)) {                                          // sub_532FB0
        way = DefHitSoundWay::Breakable;
        return BreakableHitSound(mat);
    }
    const uint16_t id = HitSoundFor(mat, hitter);
    way = id ? DefHitSoundWay::Played : DefHitSoundWay::TableEmpty;
    return id;
}

// ★ 09-23 nine-defects 4-c2 — `nox_xxx_soundPlayerDamageSound_5328B0`(`sound.go:15~32`).
//   맞는 쪽 재질 = FLESH(1)에서 시작해 **소지품 중 가슴갑옷**(ARMOR + BREASTPLATE — 입었든 말든, 정답지가 입음을 안 본다)의
//   재질이 더 크면 그 값(수 비교) · 재질 수식이 「Material7」 인 것이면 0x2000(`Sub_4133D0` — ⚠ 우리 물건엔 수식이 없어 늘 아님)
//   때린 것 = 때린 물건(a3 — 든 무기)이 있으면 그 재질, 없으면 때린 이(a2 — 사람이면 NewPlayer) 재질. 둘 다 없으면 소리 없음
//   소리 자리 = 맞는 사람(`aud_501960(번호, 맞는 쪽, 0, 0)`)
void Server::PlayerDamageSound(const Session& victim, HitterRef h) {
    ++hitSoundCalls_;
    auto way = [&](HitSoundWay w) { ++hitSoundWay_[static_cast<size_t>(w)]; };
    if (MonsterHitSoundOwns(h)) { way(HitSoundWay::MonsterSound); return; }        // ★ 4-c3b3b — 몬스터 맞히는 소리가 대신(`GAME3_2.c:7252~7254`)
    if (h.kind == HitterRef::NotPorted) { way(HitSoundWay::NotPorted); return; }   // 아직 안 옮긴 자리(4-c3b 표)
    if (h.kind == HitterRef::None) { way(HitSoundWay::NoHitter); return; }         // obj2 == nil
    const uint32_t hitter = MaterialOfWire(h.type);
    uint32_t mat = 1;                                                     // FLESH
    auto consider = [&](uint16_t wire) {
        if (wire == 0 || wire >= breastByWire_.size() || !breastByWire_[wire]) return;
        const uint32_t m = MaterialOfWire(wire);
        if (static_cast<int32_t>(m) > static_cast<int32_t>(mat)) mat = m;
    };
    const Loadout l = LoadoutFor(victim.charClass);
    for (int k = 0; k < l.count && k < 4; ++k) consider(l.items[k]);
    for (const auto& c : victim.inventory) consider(c.type);
    const uint16_t id = HitSoundFor(mat, hitter);
    if (id == 0) { way(HitSoundWay::TableEmpty); return; }
    PlaySoundAt(id, static_cast<float>(victim.posX), static_cast<float>(victim.posY), 0, 0, victim.addr);
    lastHitSound_ = id;
    way(HitSoundWay::Played);
}

// `GAME4_3.c:3624` playerTraceAttack — 참 = 무엇에든 닿음(빗나감 소리 879~884 를 가른다 — ★ 4-a 부터 `PlayerAttack` 의 judge 가 낸다).
//   망치(무기 SUBCLASS 0x4000)는 판정 중심 둘레 **모두**에 sub_538510 을 부르고, 그 밖은 하나를 고른다(sub_5386A0).
bool Server::PlayerTraceAttack(Session& s, const AttackInfo& ai, std::vector<OutPacket>& out) {
    const float ax = static_cast<float>(s.posX), ay = static_cast<float>(s.posY);
    const float dirX = std::cos(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    const float dirY = std::sin(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    const bool hammer = ai.weapon && (WeaponEquipOf(s) & 0x4000);
    const float gx = ai.px, gy = ai.py;                     // sub_518040 중심(a2+16 이거나 사람 자리)
    bool hitFlag = false;                                   // dword_5d4594_2488656

    enum Kind { None, Player, Pet, Obstacle };
    // sub_5180B0 거리 조건 — 사거리 − (중심에서 가장자리까지) > 0
    auto gate = [&](const PhysBody& b, float ox, float oy) {
        double g;
        if (b.shape == 2) {
            g = ai.range - (std::sqrt((gx - ox) * (gx - ox) + (gy - oy) * (gy - oy)) - b.r);
        } else if (b.shape == 3) {
            float nx, ny;
            g = BoxOverlap(gx, gy, ai.range, b, ox, oy, nx, ny);
        } else {
            g = ai.range - std::sqrt((gx - ox) * (gx - ox) + (gy - oy) * (gy - oy));
        }
        return g > 0.0;
    };
    // sub_538510 — 방향 표 &1 · 깃발 · 선 추적(벽 0x5) → 피해(+716) · 맨손 훅이면 힘 20
    auto strike = [&](Kind kind, const std::string& addr, int idx) {
        float tx = 0, ty = 0;
        uint32_t tflags = 0;
        if (kind == Player) {
            const Session& o = sessions_.at(addr);
            tx = static_cast<float>(o.posX); ty = static_cast<float>(o.posY);
            if (o.respawnAtFrame != 0) tflags |= 0x8000 | 0x8;
            if (o.observer) tflags |= 0x40;
        } else if (kind == Pet) {
            tx = static_cast<float>(pets_[idx].x); ty = static_cast<float>(pets_[idx].y);
            tflags = PetObjFlags(pets_[idx]);                // ★ 10-01 시체 — 죽음 덧칠(0x8000 → 아래 0x8040 에 걸림 · `sub_538510` `GAME4_3.c:3716`)
        } else {
            tx = obstacles_[idx].x; ty = obstacles_[idx].y;
            tflags = obstacles_[idx].flags;
        }
        if (!(TestTwoPointsAndDirection(ax, ay, s.facingPrev, tx, ty) & 1)) return;
        if (tflags & 0x8040) return;
        if (!ai.hitsOverlap && (tflags & 0x8)) return;
        hitFlag = true;                                     // 재질 NONE(0x4000) 이 아니면 — 재질은 아직 안 읽는다
        if (!MapTraceRay(ax, ay, tx, ty, 0x5)) return;
        AttackSrc src;                                      // 막기 판정(3-e2) — 때린 사람의 지난 자리(+72)
        src.x = static_cast<float>(s.prevX);
        src.y = static_cast<float>(s.prevY);
        src.obj = ai.weapon;                                // +28 무기 오브젝트 — 죽음 알림의 무기 그림(3-d)
        src.objType = ai.weapon ? WeaponWireOf(s) : 0;
        // ★ 4-c2 — 맞는 소리의 때린 것 = 든 무기(a3 · `GAME4_3.c:3986`), 맨손이면 때린 사람 몸(a2 · a3 = 0 `:4060`)
        HitterRef hr = HitterPlayerBody();
        if (ai.weapon) hr.type = WeaponWireOf(s);
        if (kind == Player) {
            HurtTarget(Target{ addr, -1 }, ai.dmg, out, s.addr, ai.type, &src, hr);
            auto it = sessions_.find(addr);
            if (!ai.weapon && ai.anim == kAnimPunchHook && it != sessions_.end()) {
                // 훅 — 맞은 쪽 ForceVec += (맞은 자리 − 때린 자리) × (10 × 20 ÷ 질량) ÷ (거리 + 0.1)
                Session& o = it->second;
                const float dpx = static_cast<float>(o.posX) - ax, dpy = static_cast<float>(o.posY) - ay;
                const float r = static_cast<float>(std::sqrt(static_cast<double>(dpx * dpx + dpy * dpy)) + 0.1);
                const float f = 10.0f * kHookForce / static_cast<float>(PlayerMassOf(o));
                o.forceX += static_cast<float>(static_cast<double>(dpx) * f / r);
                o.forceY += static_cast<float>(static_cast<double>(dpy) * f / r);
            }
        } else if (kind == Pet) {
            HurtTarget(Target{ std::string(), idx }, ai.dmg, out, s.addr, ai.type, &src, hr);
            // ⚠ 훅 힘 — 소환수는 아직 물리가 없다(묶음 6)
        }
        // 장애물 — 피해 함수가 없는 것(나무·바위). 부서지는 것은 묶음 5·9.
    };

    PhysBody playerBody;
    playerBody.shape = 2; playerBody.r = static_cast<float>(kBodyRadius); playerBody.cls = kClsPlayer;

    if (hammer) {
        // 둘레 모두 — 목록이 바뀔 수 있어(소환수가 죽으면 지워짐) 먼저 모은 뒤 친다
        std::vector<std::pair<Kind, std::pair<std::string, uint16_t>>> hits;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (&o == &s || !o.inGame) continue;
            if (gate(playerBody, static_cast<float>(o.posX), static_cast<float>(o.posY))) hits.push_back({ Player, { kv.first, uint16_t{0} } });
        }
        for (const auto& k : pets_) {
            auto tb = typeBodies_.find(k.type);
            if (tb == typeBodies_.end()) continue;
            if (gate(tb->second, static_cast<float>(k.x), static_cast<float>(k.y))) hits.push_back({ Pet, { std::string(), k.netCode } });
        }
        for (size_t i = 0; i < obstacles_.size(); ++i) {
            if (gate(obstacles_[i], obstacles_[i].x, obstacles_[i].y)) hits.push_back({ Obstacle, { std::string(), static_cast<uint16_t>(i) } });
        }
        for (const auto& h : hits) {
            if (h.first == Pet) {
                int idx = -1;
                for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == h.second.second) { idx = static_cast<int>(i); break; }
                if (idx >= 0) strike(Pet, std::string(), idx);
            } else if (h.first == Player) {
                if (sessions_.count(h.second.first)) strike(Player, h.second.first, -1);
            } else {
                strike(Obstacle, std::string(), h.second.second);
            }
        }
        return hitFlag;
    }

    // 하나 고르기 — sub_5386A0 앞쪽 가장 가까운 것(몬스터 우선)
    float best = ai.range;
    Kind kind = None;
    std::string bestAddr;
    int bestIdx = -1;
    bool bestMonster = false;
    auto pick = [&](const PhysBody& b, float ox, float oy, bool isEnemy, int obstacleIdx) -> bool {
        if (!gate(b, ox, oy)) return false;
        if (b.flags & 0x8049) return false;
        if (!((b.cls & 6) || !(b.flags & 0x10) || (b.flags & 0x80))) return false;
        if (!(isEnemy || !(b.cls & 6))) return false;
        if (!CanSeeBuffs(&s.buffs, nullptr, 0.0, 0.0, 1) || !MapTraceVisionSkip(ax, ay, ox, oy, obstacleIdx)) return false;   // unitCanInteractWith(…, 1) — 눈멂만(4-a4)
        if (!(best > 0.0f)) return false;
        const float vx = ox - ax;
        const double vy = oy - ay;
        double len = std::sqrt(vy * vy + vx * vx);
        if (len == 0.0) return false;
        if (!(vy / len * dirY + vx / len * dirX > 0.5)) return false;
        if (b.shape == 2) {
            len = len - b.r;
        } else if (b.shape == 3) {
            float nx, ny;
            const double v7 = BoxOverlap(ax, ay, best, b, ox, oy, nx, ny);
            if (v7 < 0.0) return false;
            len = best - v7;
        }
        if (len < 0.0) len = 0.0;
        const bool isMonster = (b.cls & kClsMonster) != 0;
        if ((len < best || (kind != None && !bestMonster && isMonster)) && (kind == None || !bestMonster)) {
            best = static_cast<float>(len);
            bestMonster = isMonster;
            return true;
        }
        return false;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (&o == &s || !o.inGame) continue;
        PhysBody b = playerBody;
        if (o.respawnAtFrame != 0) b.flags |= 0x8000 | 0x8;    // 죽음 DEAD · ALLOW_OVERLAP
        if (o.observer) b.flags |= 0x40;                       // 관전 NO_COLLIDE
        if (pick(b, static_cast<float>(o.posX), static_cast<float>(o.posY), IsEnemyTo(s, o), -1)) {
            kind = Player; bestAddr = kv.first;
        }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        const Creature& k = pets_[i];
        auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end()) continue;                 // 몸을 모르는 종류(지어내지 않는다)
        const bool enemy = k.owner.empty() || (k.owner != s.addr && IsEnemyTo(s.addr, k.owner));
        PhysBody pb = tb->second;
        pb.flags = PetObjFlags(k);                             // ★ 10-01 시체 — 0x8049 에 걸림(`sub_5386A0` `GAME4_3.c:3783`)
        if (pick(pb, static_cast<float>(k.x), static_cast<float>(k.y), enemy, -1)) {
            kind = Pet; bestIdx = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        const PhysBody& o = obstacles_[i];
        if (pick(o, o.x, o.y, false, static_cast<int>(i))) { kind = Obstacle; bestIdx = static_cast<int>(i); }
    }
    if (kind == None) return false;
    strike(kind, bestAddr, bestIdx);
    // ⚠ 무기 닳기 playerDamageWeapon_4E1560(ItemDamagePercentage) — 아이템 체력(묶음 5) · 벽 부수기 mapDamageToWalls(9)
    return hitFlag;
}

int Server::Tick(uint32_t nowMs, std::vector<OutPacket>& out) {
    // ★ 09-29 B7 — 이 틱 동안 끝내기·취소가 낼 패킷도 `out` 으로(나갈 때 풀어 둔다)
    struct AiOutGuard { std::vector<OutPacket>*& p; ~AiOutGuard() { p = nullptr; } } aiOutGuard{ aiOut_ };
    aiOut_ = &out;
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

    // ★ 09-20 ⑰ — 지난 틱에 만든 물체의 **낙하 시작 알림**을 여기서 낸다.
    //   지난 틱 끝의 `a4 UPDATE_STREAM` 이 그 물체를 이미 소개했으므로 클라가 그림을 찾는다.
    FlushDeltaZStart(out);

    // ── 판 다시 열기 ────────────────────────────────────────────────
    //
    // ★ 전에는 **판이 한 번 끝나면 그 프로세스에서 다시 못 열었다.**
    //   `roundOver_` 를 되돌리는 곳이 `StartQuest` 하나뿐이었고 아레나·깃발은
    //   되돌리는 데가 아예 없었다(2026-09-11 감사). 사람 둘이 앉아 계속 놀 수가 없었다.
    //
    // ⚠ 다시 여는 시점 `kRoundRestartTicks` 는 ➖ **우리 값이다.** 정품이 몇 초 뒤에
    //   새 판을 여는지는 표본이 없다. 승자 화면을 볼 시간은 줘야 하므로 몇 초로 둔다.
    if (roundOver_ && restartAtFrame_ != 0 && frame_ >= restartAtFrame_) {
        restartAtFrame_ = 0;
        roundOver_ = false;
        // 점수를 지운다 — 셋 다.
        for (auto& kv : sessions_) {
            kv.second.score = 0;
            kv.second.deaths = 0;
        }
        caps_[0] = caps_[1] = caps_[2] = 0;
        // 퀘스트는 스테이지 1 부터 다시. 새 웨이브가 다음 틱에 밀려온다.
        if (questTotal_ > 0) {
            questStage_ = 0;
            questAnnounced_ = false;
            SpawnQuestStage(nullptr);
        }
        // 깃발은 집으로 되돌린다(들고 있던 사람 손에서도 뺀다).
        for (auto& f : flags_) {
            f.carrier.clear();
            f.x = f.homeX; f.y = f.homeY; f.atHome = true;
        }
        ++roundsPlayed_;
        std::printf("[판] 다시 엶 — %llu번째 판 (점수·깃발 초기화)\n", roundsPlayed_ + 1);
        std::fflush(stdout);
    }

    // ★ 10-01 R1 — **판끝 동안 세계를 멈춘다**(원본은 판끝 깃발 `GameFlag4` 가 서 있으면 `server_B` 를 안 부른다 —
    //   유닛·능력·각본·`updateRemotePlayers`(사람별 시각·갱신 짜기) 전부 · 보내기 `server_A2` 만 · `game.go:790~801` · `server.go:325~362·435~470`).
    //   막는 것 일곱 묶음 — 사람 걸음(이동·공격·나는 것·버프·시전 목록) · 크리처 생각·시체 정리 · 소환수 용암 피해 ·
    //     깃발·공·투표·덫·비밀벽·탈락전·문 · 놓인 물체·버린 물건·독 · 사람별 `27`·`a4`·정적 소개·비밀벽 맞춤
    //   남기는 것 — 재시작 · 세션 정리 · 진단 스위치 · 소리 보내기 · 중요 메시지(Kind1 은 원본도 나감 · `network_lists.go:206~224`) ·
    //     판 시계 · 지연 보고 · 새로 온 사람 스냅샷
    //   판이 끝난 **그 틱**은 끝까지 돈다(여기서 한 번 읽는다) · 받은 패킷이 바로 하는 일(줍기·입기 등 `OnPacket`)은 안 막는다(원본도 `server_B` 밖)
    const bool frozen = roundOver_;
    if (frozen) ++roundFrozenTicks_;

    // ── 자리 비우기 — **조용해진 세션을 걷어낸다** ──
    //
    // 지우는 자리가 아예 없었다. 클라를 죽이면 세션이 그대로 남아 사람 수에 계속
    // 세어졌고, 시험할 때마다 유령이 끼어 「누가 누구를 보는가」 판정이 흐려졌다
    // (2026-09-09 — 두 클라를 붙였는데 서버가 사람 3 이라고 했다).
    // ⚠ 접속 절차 중인 세션도 지운다 — 서버 찾기만 하고 사라진 상대가 쌓인다.
    {
        std::vector<std::string> gone;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            // ★ 정상 종료를 알린 사람은 **아래 문지기보다 먼저** 걷어낸다.
            //   「한 번도 못 받은 세션」 검사에 걸리면 `0x0a` 를 보내고도 안 나간다 —
            //   틱이 아직 0 이면 `lastSeenFrame` 도 0 이라 늘 걸린다(시험이 잡아냈다).
            if (o.closing) { gone.push_back(kv.first); continue; }
            // 아직 한 번도 못 받은 세션(이제 막 만든 것)은 건드리지 않는다.
            if (o.lastSeenFrame == 0) continue;
            // ★ 10-d — 정답지(`server.go:440`)는 **입력을 받는 중**이면 90초, 아니면 30초이다
            const uint32_t limit = o.inGame ? kSessionTimeoutTicksInGame : kSessionTimeoutTicks;
            if (frame_ - o.lastSeenFrame > limit) gone.push_back(kv.first);
        }
        for (const std::string& addr : gone) {
            // ★ 나갔다고 **알린다.** 전에는 조용히 지우기만 해서 남은 사람들 화면에
            //   그 몸이 영영 서 있었다. 둘을 같이 보낸다 —
            //     `2e PLAYER_QUIT`(껍데기 안)  = 사람 목록·점수판에서 뺀다
            //     `31 DESTROY_OBJECT`(생)      = 세계에서 몸을 지운다
            //   ⚠ 지우기 **전에** 번호를 떠 둔다. 지운 뒤에는 읽을 자리가 없다.
            uint16_t netCode = 0;
            bool wasInGame = false;
            {
                auto g = sessions_.find(addr);
                if (g != sessions_.end()) {
                    netCode   = g->second.netCode;
                    wasInGame = g->second.inGame;
                    // 깃발을 들고 있었으면 그 자리에 떨군다 — 안 그러면 깃발이
                    // 사라진 사람 손에 남아 판이 영영 안 끝난다.
                    if (wasInGame) DropFlag(addr, out);
                }
            }
            std::printf("조용해서 내보냄: %s\n", addr.c_str());
            if (wasInGame) PlayerCancelSpells(addr, out);   // 오브젝트 지움 → playerCancelSpells(object.go:70, 4-a3)
                        // 10-d PlayerDisconnect: notify BEFORE erasing.
            //   timeout (reason 3): 0xC6 to that player, then 0xAE to the rest.
            //   a player that announced a clean quit (closing) is not a timeout.
            {
                auto g = sessions_.find(addr);
                if (g != sessions_.end() && !g->second.closing) {
                    const uint8_t to[1] = { static_cast<uint8_t>(Op::TIMEOUT_NOTIFICATION) };
                    Emit(g->second, /*reliable=*/true, /*stream=*/0, to, sizeof(to), out);
                }
            }
            SendOutgoingClient(netCode, addr, out);
            sessions_.erase(addr);

            if (wasInGame && netCode != 0) {
                uint8_t quit[3] = {
                    static_cast<uint8_t>(Op::PLAYER_QUIT),
                    static_cast<uint8_t>(netCode),
                    static_cast<uint8_t>(netCode >> 8),
                };
                Broadcast(/*viaSeq=*/true, quit, sizeof(quit), out);

                uint8_t bye[3] = {
                    static_cast<uint8_t>(Op::DESTROY_OBJECT),
                    static_cast<uint8_t>(netCode),
                    static_cast<uint8_t>(netCode >> 8),
                };
                Broadcast(/*viaSeq=*/false, bye, sizeof(bye), out);
            }
        }
    }

    // ★ 진단 — 첫 사람이 들어오고 N 틱 뒤, 크리처를 사람 좌우 ±300 으로 **한 번** 옮긴다
    //   (`--creature-away-after N`, 2026-09-13).
    //   왜 — 「사라짐(OBJECT_OUT_OF_SIGHT)」을 화면으로 보려면 크리처가 시야 사각형 밖으로 나가야 하는데,
    //   그러려면 사람이 걸어야 한다. **재는 대상은 사라짐이지 걷기가 아니니 걷기를 빼 버린다**
    //   (`--item-at-spawn` 과 같은 논리).
    //   ±300 은 화면 안(±640)이지만 판정용 사각형(--creature-aoi-half 120 → 나감 ±144) 밖이다 —
    //   대조군(시야 판정 끔)은 계속 보이고, 실험군만 사라져야 한다.
    // 진단 둘이 함께 쓰는 「첫 사람이 들어온 프레임」.
    {
        for (const auto& kv : sessions_) if (kv.second.inGame) { if (firstInGameFrame_ == 0) firstInGameFrame_ = frame_; break; }
    }
    // ★ 진단(묶음 3-e) — N 틱마다 관전 아닌 첫 사람에게 주인 없는 것의 칼 피해 25. 움찔(20 이상)·체력 막대를 화면으로 본다.
    if (debugHurtEvery_ > 0 && firstInGameFrame_ != 0 && frame_ > firstInGameFrame_ &&
        (frame_ - firstInGameFrame_) % debugHurtEvery_ < static_cast<uint32_t>(ticks)) {
        for (auto& kv : sessions_) {
            Session& s = kv.second;
            if (!s.inGame || s.observer || s.respawnAtFrame != 0) continue;
            const uint16_t before = s.health;
            AttackSrc front;                                   // --debug-hurt-front — 바라보는 쪽 30 앞에서 때린 것으로(막기 판정용)
            const float rad = s.facingPrev * 2.0f * 3.14159265f / 256.0f;
            front.x = static_cast<float>(s.posX) + std::cos(rad) * 30.0f;
            front.y = static_cast<float>(s.posY) + std::sin(rad) * 30.0f;
            HurtTarget(Target{ kv.first, -1 }, 25, out, std::string(), 0, debugHurtFront_ ? &front : nullptr);
            std::printf("[진단] 프레임 %u 에 %s 피해 25(종류 0) → 체력 %u→%u · 이월 %.2f · 상태 %u\n", frame_, kv.first.c_str(),
                        before, s.health, s.dmgRemainder, s.moveState);
            std::fflush(stdout);
            break;
        }
    }
    // ★ 진단 — 첫 사람이 들어오고 N 틱 뒤 **그 사람 소유 임프**를 사람 자리에 만들어 고정한다
    //   (`--debug-own-pet N`, 2026-09-13).
    //   왜 — 친구 알림의 화면 효과는 **커서 색**이다(친구면 파랑 · 적이면 빨강, `video_cgo.go:616`).
    //   퀘스트엔 소환수가 없고, 움직이는 소환수에 커서를 맞추기 어렵다. 그래서 사람 자리에 붙여 두고
    //   커서를 화면 한가운데(=사람 자리)에 둔다. 소환 조작(A키)을 빼 버린다(`--item-at-spawn` 과 같은 논리).
    if (debugOwnPetAfter_ > 0 && debugOwnPetNetCode_ == 0 && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugOwnPetAfter_) {
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            Creature k;
            k.owner = debugPetWild_ ? std::string() : kv.second.addr;   // --debug-dummy 면 주인 없음(적)
            if (k.owner.empty()) k.aggression = 0.5f;                  // ★ 09-29 B2 — 맵 몬스터 공격 성향 기본
            k.type = SummonType(87);   // SUMMON_IMP → Imp
            k.netCode = nextShotNetCode_++;
            if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
            // ⚠ 사람 자리에 겹치면 화면에서 구분이 안 되고 커서도 안 걸렸다(09-13) — 오른쪽 +80 에 떨어뜨린다.
            k.x = kv.second.x + (debugPetWild_ ? debugDummyDx_ : 80);   // 허수아비는 주먹 사거리(30) 안(기본 25)
            k.y = kv.second.y;
            k.health = static_cast<uint16_t>(TuningFor(k.type).health);
            pets_.push_back(k);
            debugOwnPetNetCode_ = k.netCode;
            // ★ 09-20 ⑥ — 명령을 안 주면 **한 칸도 안 움직인다**(주인 있는 크리처는 명령 대기).
            //   `--debug-pet-order 5`(사냥)를 주면 스스로 적을 찾아간다
            if (debugPetOrder_ != 0 && !debugPetWild_) {
                auto si = sessions_.find(kv.first);
                if (si != sessions_.end()) {
                    EnactUnitOrder(si->second, pets_.back(), debugPetOrder_);
                    std::printf("[진단] 소환수에게 명령 %u(2가만히·3지켜·4따라와·5사냥)\n",
                                static_cast<unsigned>(debugPetOrder_));
                }
            }
            std::printf("[진단] 프레임 %u 에 %s %s 임프(번호 %u · 체력 %u)를 사람(%d,%d) 오른쪽 +%d 에 고정\n",
                        frame_, kv.second.addr.c_str(), debugPetWild_ ? "옆에 주인 없는" : "소유",
                        static_cast<unsigned>(k.netCode), static_cast<unsigned>(k.health),
                        kv.second.x, kv.second.y, debugPetWild_ ? debugDummyDx_ : 80);
            std::fflush(stdout);
            break;
        }
    }
    if (debugEnchantBuff_ >= 0 && !debugEnchantDone_ && firstInGameFrame_ != 0 && frame_ >= firstInGameFrame_ + debugEnchantAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame || kv.second.observer) continue;
            ApplyEnchant(Target{ kv.first, -1 }, debugEnchantBuff_, 0, debugEnchantPower_);
            std::printf("[진단] 프레임 %u 에 %s 버프 %d(세기 %d) 걸음 → 비트 0x%08X\n", frame_, kv.first.c_str(), debugEnchantBuff_,
                        debugEnchantPower_, kv.second.buffs.bits);
            std::fflush(stdout);
            debugEnchantDone_ = true;
            break;
        }
    }
    if (debugAwayAfter_ > 0 && !debugAwayDone_) {
        const Session* who = nullptr;
        for (const auto& kv : sessions_) if (kv.second.inGame) { who = &kv.second; break; }
        if (who && frame_ >= firstInGameFrame_ + debugAwayAfter_) {
            int side = 1;
            for (auto& k : pets_) { k.x = who->x + 300.0 * side; k.y = who->y; side = -side; }
            debugAwayDone_ = true;
            std::printf("[진단] 프레임 %u 에 크리처 %zu 마리를 사람(%d,%d) 좌우 ±300 으로 옮김\n",
                        frame_, pets_.size(), who->x, who->y);
            std::fflush(stdout);
        }
    }
    // ★ 진단 — 첫 사람 N틱 뒤 **그 사람 팀을 한 번 바꾼다**(`--debug-team-after N`, 2026-09-13).
    //   왜 — 팀이 바뀌면 정답지는 그 사람에게 `36 RESET_FRIENDS` 를 보내고 친구를 다시 알린다.
    //   친구 임프를 둔 채 팀을 바꿔 **다시 알리기가 빠지면 임프가 적(빨강)으로 바뀌는지**를 화면으로 가른다.
    if (debugTeamAfter_ > 0 && !debugTeamDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugTeamAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            const uint8_t before = kv.second.team;
            kv.second.team = (before == 1) ? 2 : 1;
            std::printf("[진단] 프레임 %u 에 %s 팀 %u → %u\n", frame_, kv.second.addr.c_str(),
                        static_cast<unsigned>(before), static_cast<unsigned>(kv.second.team));
            std::fflush(stdout);
            break;
        }
        debugTeamDone_ = true;
    }
    // ★ 09-19 `--debug-team-every N` — **N틱마다 첫 사람의 팀을 1↔2 로 오간다.**
    //   `--debug-team-after` 는 한 번만 바꿔서 「다른 팀 → 같은 팀」(친구 ADD)만 보였다.
    //   확인표 ④ 가 보는 것은 **REMOVE**(같은 팀 → 다른 팀)라 한 번 더 옮길 수단이 필요하다
    if (debugTeamEvery_ > 0 && firstInGameFrame_ != 0 && frame_ > firstInGameFrame_ &&
        ((frame_ - firstInGameFrame_) % debugTeamEvery_) == 0) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            const uint8_t before = kv.second.team;
            kv.second.team = (before == 1) ? 2 : 1;
            std::printf("[진단] 프레임 %u 에 %s 팀 %u → %u (되풀이)\n", frame_, kv.second.addr.c_str(),
                        static_cast<unsigned>(before), static_cast<unsigned>(kv.second.team));
            std::fflush(stdout);
            break;
        }
    }

    // ★★★ 09-19 진단 `--debug-kill N` — **N틱 뒤 첫 사람이 둘째를 죽인 것으로 친다.**
    //   왜 — 「킬 → 점수 → 판 끝」 사슬의 앞머리를 무인으로 못 일으킨다.
    //   같은 자리에 세우고 `NOX_AUTO_ATTACK` 으로 때려도 **근접 146 에 피해 5**,
    //   체력을 5 로 낮춰도 **피해 1** 이었다(09-19). 무인은 **상대를 안 바라본다**.
    //   그래서 `Damage` 를 직접 불러 죽인다 — 그 함수가 킬 처리·점수·`RoundCheckLimit` 까지 한다
    if (debugKillAfter_ > 0 && !debugKillDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugKillAfter_) {
        std::string killer, victim;
        for (auto& kv : sessions_) {
            if (!kv.second.inGame || kv.second.observer) continue;
            if (killer.empty()) killer = kv.first;
            else if (victim.empty()) { victim = kv.first; break; }
        }
        if (!killer.empty() && !victim.empty()) {
            std::printf("[진단] 프레임 %u — %s 가 %s 를 죽인다(진단)\n",
                        frame_, killer.c_str(), victim.c_str());
            std::fflush(stdout);
            Damage(victim, 9999, out, killer, nullptr, /*ignoreInvuln=*/true);
            debugKillDone_ = true;
        }
    }

    // ★★★ 09-20 진단 `--debug-fist N dx dy` — **서버가 직접 주먹을 떨어뜨린다**(⑰).
    //   클라로 시키면 시전 시각(접속 뒤 25초+주기)·벽·자기 피해 때문에 여덟 번 헛돌았다.
    //   여기서 쏘면 **틱이 정해져 있어** 녹화를 그 시각에 맞출 수 있다
    if (debugFistAfter_ > 0 && !debugFistDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugFistAfter_) {
        for (auto& kv : sessions_) {
            Session& o = kv.second;
            if (!o.inGame || o.observer || o.respawnAtFrame != 0) continue;
            SpellArg arg{};
            arg.x = static_cast<float>(o.posX) + static_cast<float>(debugFistDx_);
            arg.y = static_cast<float>(o.posY) + static_cast<float>(debugFistDy_);
            const int r = CastFist(o, arg, /*lvl=*/3, out);
            std::printf("[진단] 프레임 %u 주먹 — 사람 %.0f,%.0f → 자리 %.0f,%.0f · 결과 %d(1이면 떴다)\n",
                        frame_, o.posX, o.posY, arg.x, arg.y, r);
            std::fflush(stdout);
            debugFistDone_ = true;
            break;
        }
    }
    // ★★★ 09-20 진단 `--debug-warp N x y` — **첫 사람을 그 자리로 한 번 옮긴다.**
    //   문·밟는 판은 「다가가야」 알림이 나는데, 시작 자리에 세워 두면 알림이 접속 직후에 나가
    //   클라가 준비 전이라 버린다. 서버가 시각을 정하면 **다 들어온 뒤 다가가기**를 만들 수 있다
    if (debugWarpAfter_ > 0 && !debugWarpDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugWarpAfter_) {
        for (auto& kv : sessions_) {
            Session& o = kv.second;
            if (!o.inGame || o.observer || o.respawnAtFrame != 0) continue;
            std::printf("[진단] 프레임 %u 순간이동 — %.0f,%.0f → %d,%d\n",
                        frame_, o.posX, o.posY, debugWarpX_, debugWarpY_);
            std::fflush(stdout);
            o.forceX = o.forceY = 0.0;
            // 진짜 순간이동 경로를 그대로 쓴다 — `ea FORGET_DRAWABLES` 까지 나가야 화면이 맞는다
            TeleportTarget(Target{ o.addr, -1 }, static_cast<float>(debugWarpX_),
                           static_cast<float>(debugWarpY_), &out);
            debugWarpDone_ = true;
            break;
        }
    }
    // ★★★ 09-20 진단 `--debug-mob N <종류> <x> <y>` — **그 자리에 맵 몬스터 하나를 세운다.**
    //   퀘스트 스폰은 사람 자리 기준이라 ⑮ 를 화면으로 못 봤다(세 번 실패). 자리를 못 박는다
    if (debugMobAfter_ > 0 && !debugMobDone_ && debugMobType_ != 0 && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugMobAfter_) {
        const size_t before = pets_.size();
        SpawnCreature(debugMobType_, static_cast<float>(debugMobX_), static_cast<float>(debugMobY_), std::string());
        std::printf("[진단] 프레임 %u 몬스터 %u 를 %d,%d 에 세움 — 크리처 %zu → %zu\n",
                    frame_, static_cast<unsigned>(debugMobType_), debugMobX_, debugMobY_, before, pets_.size());
        std::fflush(stdout);
        debugMobDone_ = true;
    }
    // ★★★ 09-20 진단 `--debug-wall N gx gy` — **그 칸의 비밀벽을 연다.**
    //   비밀벽은 다가가서 열리는 것이 아니다(깃발 4·8 이 0 이면 각본만 연다). 「클라가 그려 주나」만 본다
    if (debugWallAfter_ > 0 && !debugWallDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugWallAfter_) {
        const int before = SecretWallState(debugWallGx_, debugWallGy_);
        WallOpen(debugWallGx_, debugWallGy_);
        const int after = SecretWallState(debugWallGx_, debugWallGy_);
        std::printf("[진단] 프레임 %u 비밀벽 %d,%d — 상태 %d → %d(4면 여는 중)\n",
                    frame_, debugWallGx_, debugWallGy_, before, after);
        std::fflush(stdout);
        debugWallDone_ = true;
    }
    // ★★★ 09-19 진단 `--debug-score N 점수` — **첫 사람의 점수를 한 번 올린다.**
    //   왜 — 판 끝은 점수가 `limitValue`(기본 **15** 킬)에 닿아야 서는데(`Server.cpp:1405`),
    //   무인으로는 15킬을 못 낸다(조준이 안 되고, 덫에 죽는 것은 점수가 아니다).
    //   그래서 점수를 직접 주어 **「판 끝 → 승자 알림 → 재시작」 사슬이 도는지**를 본다.
    //   플랜 v6 의 목표가 「한 판이 **처음부터 끝까지** 돈다」라서 이 사슬이 마지막 관문이다
    if (debugScoreAfter_ > 0 && !debugScoreDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugScoreAfter_) {
        for (auto& kv : sessions_) {
            Session& o = kv.second;
            if (!o.inGame || o.observer) continue;
            o.score = static_cast<int>(debugScoreVal_);
            std::printf("[진단] 프레임 %u 에 %s 점수를 %d 로 (한도 %d)\n", frame_, o.addr.c_str(),
                        o.score, static_cast<int>(gameSettings_.limitValue));
            std::fflush(stdout);
            break;
        }
        debugScoreDone_ = true;
    }

    // ★ 진단 — 첫 사람 N틱 뒤 **경험치를 한 번 준다**(`--debug-xp N 양`, 2026-09-15).
    //   왜 — 경험치는 스크립트·퀘스트가 주는 것이라 아레나에서는 저절로 안 생긴다.
    //   ④대기 ⑨ 「HUD 경험치 막대가 차오르나」를 정품 화면으로 가르려면 줄 길이 필요하다.
    if (debugXpAfter_ > 0 && !debugXpDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugXpAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            const int before = kv.second.level;
            PlayerGiveExp(kv.second, debugXpAmount_, out);
            std::printf("[진단] 프레임 %u 에 %s 경험치 +%.0f (레벨 %d → %d)\n", frame_,
                        kv.second.addr.c_str(), debugXpAmount_, before, kv.second.level);
            std::fflush(stdout);
            break;
        }
        debugXpDone_ = true;
    }

    // ★ 진단 — 첫 사람 N틱 뒤 **주문 하나를 준다**(`--debug-give-spell N 주문`, 2026-09-15).
    //   ④대기 ⑨ 「주문책에 주문이 느나」용. `announce=1` 이라 받는 소리(226)도 같이 난다.
    if (debugSpellAfter_ > 0 && debugSpellId_ != 0 && !debugSpellDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugSpellAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            const bool ok = SpellGrantToPlayer(kv.second, debugSpellId_, 1, false, 0, out);
            std::printf("[진단] 프레임 %u 에 %s 주문 %u 주기 %s (칸 %u)\n", frame_,
                        kv.second.addr.c_str(), static_cast<unsigned>(debugSpellId_),
                        ok ? "됨" : "거절", static_cast<unsigned>(kv.second.spellLvl[debugSpellId_]));
            std::fflush(stdout);
            break;
        }
        debugSpellDone_ = true;
    }

    // ★ 진단 — 컨저러에게 박쥐·흰거미 둘·거미 안내서(`--debug-conjurer-guides`, 09-24 사람 판).
    //   정답지에서 안내서는 맵 두루마리 줍기(`GAME4_3.c:8104~8120`)나 저장 캐릭터 읽기(`GAME1_1.c:1543~1568`)로만 온다 —
    //   우리는 둘 다 없어 컨저러가 부를 짐승이 없다. 들어온 것을 본 뒤 60틱 뒤에 1 Bat · 7 AlbinoSpider · 8 SmallAlbinoSpider · 24 Spider
    //   (번호 = `blob_587000.dat` 70500 의 이름 표 · 24 는 무리라 7·8·25·26 도 같이 서지만 7·8 은 d1 을 따로 보내려고 먼저 준다)
    if (debugConjurerGuides_) {
        static const uint8_t kDebugGuides[] = { 1, 7, 8, 24 };
        for (auto& kv : sessions_) {
            Session& s = kv.second;
            if (!s.inGame || s.charClass != CharClass::Conjurer) continue;
            if (s.debugGuideAt == 0) { s.debugGuideAt = frame_ + 60; continue; }
            if (frame_ < s.debugGuideAt || s.guideLvl[1] != 0) continue;
            for (uint8_t g : kDebugGuides) GuideAward(s, g, false, out);
            std::printf("[진단] 프레임 %u 에 %s 컨저러 안내서 넷(박쥐·흰거미 둘·거미) 줌\n", frame_, s.addr.c_str());
            std::fflush(stdout);
        }
    }

    // ── 사람을 걷게 하고 때리게 한다 (묶음 2-a · 3-a) — 걸음마다 제 프레임으로 ──
    //    그 틱의 시전 목록도 함께(spellBookReact_4FCB70 — 정답지는 유닛 갱신 뒤 · server.go:341)
    for (int step = 0; step < ticks && !frozen; ++step) {                                    // R1 — 판끝 동안 멈춤
        UpdatePlayerMovement(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);
        SpellCastByBook(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);
        SpellCastByPlayer(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);   // 4-a3 지속 주문(spellBookReact_4FCB70)
    }

    // ── 묶음 6-f(6) 불려 나온 것들을 **쌓임으로** 움직인다 ────────────────
    //
    // ★★ 2026-09-15 이전에는 이 자리가 **임시 구현**이었다 — 가장 가까운 적에게
    //   곧장 다가가 사거리에서 때렸다. 이제 **AI 행동 쌓기**(6-a~6-g)가 그 일을 한다.
    //
    // ⭕ 한 틱에 하는 일
    //   1. 그 몬스터의 **빠르기**를 세운다(thing/monster.bin 값)
    //   2. **가장 가까운 적**을 골라 `enemyAddr` 에 적는다 — 싸움(15)이 이것을 본다
    //      ⚠ 정답지의 「본 것」 갱신(`unitUpdateSightMB_5281F0` 따위)은 아직 안 옮겼다.
    //        지금은 **아주 멀리까지 보는** 임시 규칙이다(전과 같다)
    //   3. **싸움(15)이 쌓임에 없으면 쌓는다** — 그 안에서 쫓기·때리기·쏘기가 갈린다
    //   4. **조건을 훑고**(`AIProcDeps`) **쌓임을 굴린다**(`AITick`)
    //   5. 행동이 세워 둔 **속도로 자리를 옮긴다**
    //
    // ⚠ 속도는 **틱마다 0 으로 되돌린다** — 정답지는 물리가 깎지만 우리는 그 자리가 없다.
    //   안 되돌리면 때리는 동안에도 지난 속도로 밀려간다
    // ⚠ 때리다 상대가 죽으면 **목록이 바뀐다** — 크기가 바뀌면 그 걸음을 접는다
    for (int step = 0; step < ticks && !pets_.empty() && !frozen; ++step) {                  // R1 — 판끝 동안 멈춤
        const size_t before = pets_.size();
        for (size_t pi = 0; pi < pets_.size(); ++pi) {
            Creature& k = pets_[pi];
            // 진단 임프는 **틱에서 뺀다**(고정) — 조준·친구 판정용이라 움직이면 곤란하다.
            // ★ 09-20 ⑥ — 다만 **명령을 준 경우엔 풀어 준다**(`--debug-pet-order`).
            //   안 풀면 「사냥」을 줘도 한 칸도 안 움직인다 — **계측이 대상을 안 돌리고 있던 것**이다
            if (debugOwnPetNetCode_ != 0 && k.netCode == debugOwnPetNetCode_ && debugPetOrder_ == 0) continue;
            const PetTuning tune = TuningFor(k.type);
            k.speedCur = static_cast<float>(tune.speed);
            // ★★ **보는 쪽은 매 프레임 「돌아가려는 쪽」을 그대로 옮겨 쓴다**
            //   (`server_update.go:81` · `server.go:167` — `obj.Direction1 = obj.Direction2`).
            //   점진 회전은 **want 를 8씩 돌려서** 생기고, 보는 쪽은 한 프레임 뒤따라온다.
            //   ⚠ 이 한 줄이 없으면 바라보기(26)의 「다 봤나」가 **영영 안 참이 되어** 제자리에 선다
            //     (2026-09-15 틱 교체에서 잡혔다 — 6-d 는 want 쪽만 옮겼었다)
            k.facing = k.facingWant;

            // ★ **보이는 적만 「지금 상대」로 삼는다** — 조건 45(보이나)가 같은 것을 보기 때문이다.
            //   안 보이는 것을 상대로 잡아 두면, 45 가 깨져 사슬이 빠지고 싸움이 다시 쌓기를
            //   **매 틱 되풀이**해 제자리에 선다(2026-09-15 벽 시험에서 잡혔다).
            //   정답지도 **알아본 적**만 `+1196` 에 넣는다
            //   ⚠ 잣대는 **크리처 자리에서 보는 선 추적**이다. (09-28 전) `CanInteractFrom` 은 **주인 시점**이라
            //     크리처 시야로는 틀렸다(주인이 보면 소환수도 본다는 셈이 된다)
            // ★ 09-28 — `CanInteractFrom` 을 크리처 눈(버프 + 시야선)으로 고치고 잣대를 그 하나로 — 조건 45 와 어긋나면 사슬이 매 틱 무너진다
            // ★★ 09-29 B1 — **지금 상대는 알아보기(`UpdateSight`)가 정한다** — 거리 250(퀘스트 640) · 앞쪽 부채꼴 · 시야선으로 목록에 넣고,
            //   목록 중 가장 가까운 적. 한번 본 것은 (거리+30) 안에서 보이는 동안 남는다(시야선은 2초마다 다시 봄).
            //   ✖ 전에는 거리 제한 없이 가장 가까운 적(`NearestTarget(…, 1e9)`)을 매 틱 골랐다 — 멀리서도 · 등 뒤도 알아봤다(09-28 사람 판 박쥐)
            //   ★ 09-29 B2 — 주인 없는 것은 놀기(0) 갱신이 지금 상대(안 보여도)로 싸움을 쌓는다 · 아래는 주인 있는 것의 호위 대체(소환 기본 명령)만 남았다
            // ★ 10-01 시체 — 죽은 것은 듣기(`checkIsKillable_528190` 0 → `unit_ai.go:317`)·앞머리(`:557`)·알아보기(`GAME4_2.c:7306`)·
            //   주 AI(`GAME5.c:1262~1264` 0x8000 이면 돌아감)를 건너뛰고 **조건·애니·행동만** 돈다(`unit_ai.go:578~603`) —
            //   죽어감(30) 애니가 끝나야 죽음(31)이 드러난다
            //   ⚠ 안쪽 넷(`UpdateSight`·`MonsterListen`·`MonsterHeadUpdate`·`MonsterBrain`)도 이미 체력 0 이면 돌아간다 — 이 갈래를 빼도
            //     겉 동작이 같다(10-01 돌연변이 「등가」) · 원본 갱신 차례를 그대로 적어 두는 자리
            if (k.dead) {
                if (k.deadDone) continue;                    // ★ 10-01 — 31 첫 갱신 뒤 갱신 목록 밖(`GAME4_3.c:9942`) — 시체는 그대로 그려지기만 한다
                k.velX = 0; k.velY = 0;
                AIRefreshStackArgs(k);
                AIProcDeps(k, out);
                MonsterAnimUpdate(k);
                AITick(k, out);
                k.prevX = k.x; k.prevY = k.y;
                continue;
            }
            // ★ 09-29 B6 — 소리 듣기(`unitUpdateMonster_50A5C0` `:549` — 앞머리·알아보기보다 먼저)
            if (!debugNoAutoFight_) MonsterListen(k);
            // ★ 09-29 B2 — 몬스터 한 틱 앞머리(unit_ai.go:557~573) — 다침 소리 · 도발 · 놀 때 소리(알아보기 앞)
            MonsterHeadUpdate(k);
            if (debugNoAutoFight_) { k.enemyAddr.clear(); k.enemyNet = 0; k.enemyPet = -1; }
            else UpdateSight(k);
            Target t; t.addr = k.enemyAddr; t.pet = k.enemyPet;
            float sx = 0, sy = 0;
            const bool sees = !t.none() && TargetXY(t, sx, sy) && CanInteractFrom(k, t);
            if (sees) {
                // ★ **할 일이 아예 없을 때만** 쌓는다(바닥이 놀기 하나).
                //   ⚠ 정답지는 **적을 알아본 순간**(`unitUpdateSightMB_5281F0` 둘레)에 한 번 쌓는다.
                //     그 「알아보기」를 아직 안 옮겨서, **놀고 있으면 쌓는다**로 대신한다.
                //     이렇게 두면 이미 굴러가는 사슬을 매 틱 덮어쓰지 않는다
                //   ★★ 묶음 6-h(2) — **주인이 있으면 싸움이 아니라 호위(3)** 를 쌓는다.
                //     정답지에서 **싸움을 올리는 쪽이 호위**다(`mobActionEscort_546430`) —
                //     호위가 바닥에 남아 **서 있는 명령**이 되고, 싸움이 끝나면 다시 주인에게 붙는다.
                //     주인이 없는 것(퀘스트 몬스터)은 호위가 스스로 빠지므로 **싸움을 바로 쌓는다**
                //   ⚠ **사람이 명령을 준 것은 안 덮는다**(6-i) — 「가만히」가 바닥에 놀기를 남기므로
                //     이 관문이 없으면 다음 틱에 호위로 되돌아가 명령이 안 선다
                // ★ 09-29 B2 — 주인 없는 것의 싸움 쌓기는 놀기(0) 갱신이 한다(AIActionIdle.Update → MonsterIdleUpdate) — 여기서는 호위만
                if (!k.ordered && k.aiStackInd == 0 && k.aiStack[0].action == 0) {
                    if (!k.owner.empty()) AIPush(k, 3);
                } else {
                    // ★ **보이는 동안에는 「마지막으로 본 자리」를 계속 고쳐 적는다.**
                    //   정답지는 알아보기 쪽이 이 자리를 갱신한다. 안 고치면 적이 옮겨가도
                    //   **처음 본 자리로만 달려간다**(2026-09-15 소환수 죽음 시험에서 잡혔다)
                    for (int i = 0; i <= k.aiStackInd && i < Creature::kAIStackMax; ++i)
                        if (k.aiStack[i].action == 15) { k.aiStack[i].argX = sx; k.aiStack[i].argY = sy; }
                }
            } else if (!debugNoAutoFight_) {
                // ★ 09-29 B1 — 지금 상대를 여기서 비우지 않는다(알아보기가 목록에서 뺄 때 비운다 · `aiLostSight_528560`)
                // ★ 적이 안 보여도 **주인이 있으면 호위**는 선다 — 그래야 따라다닌다
                if (!k.ordered && !k.owner.empty() && k.aiStackInd == 0 && k.aiStack[0].action == 0) AIPush(k, 3);
            }

            // ★ 09-29 B4 — 두뇌(`monsterMainAIFn_547210`) — 알아보기 뒤 · 조건 앞(`unit_ai.go:576~578`)
            if (!debugNoAutoFight_) MonsterBrain(k, out);
            k.velX = 0; k.velY = 0;
            AIRefreshStackArgs(k);                             // ★ 09-28 — 정답지는 조건 보기 전(`unit_ai.go:548`)
            AIProcDeps(k, out);
            if (pets_.size() != before) break;
            MonsterAnimUpdate(k);                              // ★ 09-29 B5 — 애니 칸 넘기기 — 조건 뒤 · 행동 앞(`unit_ai.go:579`)
            AITick(k, out);
            if (pets_.size() != before) break;
            k.injured = false;                                // ★ 09-29 B2 — INJURED 는 제 갱신 끝에 끈다(unit_ai.go:608)
            // ★ 09-29 B1 — 지난 자리(+72 PrevPos)는 **옮기기 직전 자리**다(`server.go:163` — 정답지는 이동 적분마다 적는다).
            //   ✖ 전에는 크리처에서 한 번도 안 적었다(시험만 세움) — 조건 71(안 움직였나)이 (0,0) 과 견줬고, 알아보기의 「순간이동」 검사가 늘 참이 될 뻔했다
            k.prevX = k.x; k.prevY = k.y;
            if (k.velX != 0.0 || k.velY != 0.0) CreatureApplyVelocity(k);
        }
        if (pets_.size() != before) break;
    }
    if (!frozen) FlushDeadPets(out);                         // ★ 10-01 — 썩은 시체·지울 표시(AI 를 다 돈 뒤 한꺼번에) · R1 판끝 동안 멈춤

    // ★ 09-27 S2 — 소환수·맵 몬스터 바닥 칸 피해(`GAME4_1.c:3240~3245` — 사람 몫은 09-26 M3). 정답지는 이동 적분 목록의 오브젝트마다
    //   **서 있어도** 새 자리(+64)의 바닥 번호가 6(Lava)이고 떠 있지 않고(+16 & 0x4000 — thing.bin FLAGS AIRBORNE) 체력 자료(+556)가 있으면 짝 (나, 6) →
    //   닿음 처리가 피해 함수 (a, 0, 0, 2, 12)(`GAME5.c:2062~2065`). 짝은 틱에 한 번(겹 거름) — 우리 소환수는 적분 대신 AI 가 자리를 옮기므로 옮긴 뒤 한 번 본다.
    //   몸 모르는 종류는 깃발도 모른다 → 안 떠 있는 것으로 본다(지진과 같은 규칙).
    //   ⚠ 안 옮긴 것 — 적분을 건너뛰는 두 경우(`:3201` 오브젝트 깃발 0x2 · 몬스터 행동 67) · 짝 목록 대신 바로 피해 함수(순서만 다르다)
    //   뒤에서부터 돈다 — 피해로 죽으면 자리 번호가 당겨지기 때문
    for (size_t i = frozen ? 0 : pets_.size(); i-- > 0;) {                                   // R1 — 판끝 동안 멈춤
        const Creature& k = pets_[i];
        if (k.health == 0) continue;
        const auto tb = typeBodies_.find(k.type);
        if (tb != typeBodies_.end() && (tb->second.flags & 0x4000)) continue;
        if (floorGrid_.TileAt(static_cast<float>(k.x), static_cast<float>(k.y), floorEdges_).tile != 6) continue;
        ++lavaHurtsPet_;
        HurtTarget(Target{ std::string(), static_cast<int>(i) }, 2, out, std::string(), 12, nullptr, HitterNone());
    }

    // ★★ **걷고 있나를 정한다** — 갱신 항목의 애니 번호(IDLE/WALK)를 가르는 데 썼다.
    //   ⚠ 2026-09-13 부터 자세는 상태에서 뽑는다(PlayerAnimByte). 이 값은 더 안 쓰인다 — 진단용으로만 남는다.
    //   지난 틱 자리와 견줘 움직였으면 걷는 것으로 본다.
    //   ⚠ 문턱 1.0 은 ➖ 우리 값이다. 정품 덤프의 「정지」 표본이 틱당 0.3~1.3 유닛이라
    //     그보다 위를 걷기로 잡았다. 너무 낮으면 서 있어도 걷는 자세가 된다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        const double dx = s.x - s.animPrevX;
        const double dy = s.y - s.animPrevY;
        s.animMoving = (dx * dx + dy * dy) > 1.0;
        s.animPrevX = s.x;
        s.animPrevY = s.y;

    }

    // ★★★ **서버는 걷는 그림 칸을 안 굴린다** (2026-09-11 화면으로 확정).
    //
    //   한동안 `0x6B REPORT_ANIMATION_FRAME` 으로 칸을 6틱마다 한 칸씩 밀어 보냈다.
    //   **아무 효과가 없었다.** 서 있는 채로 칸만 0↔2 로 바꿔 찍어 견주니 —
    //     대조군(같은 칸 두 장) 8,200 픽셀 · 실험(칸 0 vs 2) 7,484 픽셀 — **실험이 더 작다.**
    //
    //   까닭은 클라 코드에 있었다. `0x6B` 은 `spriteSetFrameMB` 로 **스프라이트 칸만** 세울 뿐
    //   (`cdecode.c:1482-1498`) **자세 시퀀스를 안 고른다.** 그리고 걷기 같은 되풀이 애니는
    //   `AnimLoop` 라 클라가 **`Frame()` 으로 스스로 굴린다**(`drawable_vector.go:119-124`) —
    //   `AnimStart` 조차 안 쓴다. 즉 **서버가 굴릴 자리가 아니다.**
    //
    //   ⭕ 반대 증거도 잡았다 — 자세만 4(WALK)로 못 박고 **안 걸은 채** 두 장을 찍으니
    //     788 픽셀이 달랐다(잡음 바닥 396). 그 런의 계수는 `걸음 0 · 걷그림 2`,
    //     즉 **서버가 칸을 한 번도 안 굴렸는데 화면은 돌았다.**
    //
    //   ⚠ `0x6B` 자체는 정품에도 있다(`netReportAnimFrame_4D81F0`). 다만 **「칸이 바뀐
    //     오브젝트」 더티 비트 `0x10000`** 이 켜졌을 때만 나간다(`GAME4_2.c:7148`).
    //     우리가 그 비트를 세울 일이 생기면 그때 되살린다. 지금은 세울 일이 없다.

    // ★ 09-14 갱신 알림 정답지화 — 접속 직후 HUD(`45`·`43`·`dd`·`de`)를 다섯 번 되풀이하던 자리표시를 걷어냈다.
    //   정답지는 스냅샷 꼬리(첫 playerReportAnything 몫 dd·de·48·43 — SnapshotTail.cpp #21·#22)뿐이고, 그 뒤는 바뀔 때만(PlayerReportAnything).
    //   되풀이가 필요했던 까닭(cc 가 접속 직후 막힘)은 7821fc7 대기열로 해소됐다.

    if (!frozen) {                                            // R1 — 판끝 동안 멈춤
    UpdateFlags(out);
    UpdateBall(out);          // 8-d 플래그볼
    VoteUpdate(out);          // 8-e 투표
    ShootingTrapUpdate(out);      // 9-b 쏘는 덫(화살 덫·해골)
    SecretWallUpdate(out);        // 9-a 비밀벽 상태 기계
    CheckVictorySurvivors(out);   // 8-g — 탈락전에서 하나만 남았나(`server.go:143`)
    if (gameSettings_.timeLimitFrames != 0 && frame_ >= gameSettings_.timeLimitFrames) RoundTimeUp(out);
    UpdateProps(out);   // 문·오벨리스크 (P5)
    }
    GiveTestItems(out); // ★ 09-30 사람 판 시험용(`--give-items` 가 없으면 아무것도 안 함)
    // 10-a — 이 틱에 울린 소리를 보낸다.
    //   ★ **모든 갱신이 끝난 뒤에야** 한다 — 정답지도 분배자에서 모아 보낸다
    //   (`NetUpdateRemotePlrAudioEvents`). 앞에 두면 그 틱에 울린 소리가 **한 틱 늦는다**
    FlushAudioEvents(out);

    // (4-a4) 무적 켜짐·꺼짐 비교 루프는 걷어냄 — 버프가 바뀌는 자리(setUnitBuffFlags)가 곧바로 `5a` 를 올린다

    if (!frozen) {                                         // R1 — 판끝 동안 멈춤
    UpdatePlaced(out);                                     // 4-c3 놓인 오브젝트(독구름 …)의 주기 함수·수명
    DecayStatics(out);                                     // ★ 09-28 버린 물건 10초 사라짐(`decay_511750` · `server.go:142`)
    PoisonTick(out);                                       // 4-c2a 독 — 감쇠와 피해(server.go 의 유닛 갱신 자리)
    }

    // ★ R4 — 이 틱에 바뀐 것들을 각자에게 보내고 목록을 비운다.
    //   정품의 「비트가 켜진 것만 보내고 곧바로 끈다」와 같은 자리다.
    FlushPending(out);
    // 퀘스트 첫 스테이지 알림 — StartQuest 때는 out 이 없어 못 보냈다. 첫 틱에 한 번.
    if (questTotal_ > 0 && !questAnnounced_) {
        questAnnounced_ = true;
        if (questStage_ < kQuestStageCount) SendNotice(kQuestStages[questStage_].label, out);
    }

    // ── 판 시계 `d3` — **주기가 아니라 「바뀔 때」 보낸다** (09-21 정정) ──────────
    //
    // ✖ 전에는 66틱(2.2초)마다 보냈고 주석에 「실측 간격이 2초 남짓」이라고 적어 뒀는데 **근거가 없었다.**
    //   `nox_opcount` 로 09-06 정품 덤프(31분)를 시간 창으로 갈라 세니 —
    //       14:36~14:41  d3 2 · 14:46~14:51  d3 1(클라 요청 d4 는 254) · 중간 14분 내내 0 ·
    //       15:05~15:07(끝)  d3 **44**
    //   합계 46건인데 **그중 44가 마지막 2분**에 몰려 있다. 주기로 보내는 게 아니다.
    //   우리 방식이면 같은 31분에 850건이 나간다.
    // ⭕ 정답지도 같다 — `sub_40A1F0`(`GAME1.c:257`)이 **시계 상태가 바뀔 때** 모두에게 한 번 보내고,
    //   그 밖에는 `d4` 물음에 답하는 자리뿐이다(`network_server.go:115`). **주기 호출자가 없다.**
    // ⇒ 여기서는 **판이 끝났나**가 바뀐 틱에만 보낸다. 나머지는 물음(`d4`)에 답하는 것으로 족하다 —
    //   클라는 제 시계와 어긋나면 스스로 묻는다(그 도장은 09-21 에 받는 사람 시계로 고쳤다).
    if (roundOver_ != timerLastRoundOver_) {
        timerLastRoundOver_ = roundOver_;
        SendTimer(out);
    }

    // ★★ 09-22 `0xd7` 지연 보고 — **주기와 뽑는 방식을 둘 다 고쳤다.**
    //   ✖ 옛 우리 규칙 = 6틱마다 **접속자 전원**. 주석의 「6틱에 한 번」은 정품에서
    //     「갱신 1,287건 대 d7 217건」을 나눈 값이라 분모가 **틱이 아니라 갱신**이었다.
    //   ⭕ 정답지 둘 —
    //     ① 주기(`server.go:259`) : `지금 − 마지막 > 0x1F4` = **500밀리초마다 한 번**(30틱이면 15틱)
    //     ② 뽑기(`GAME3_2.c:1444~1470`) : 커서를 하나 들고 **돌아가며 한 명만** 골라
    //        그 한 건을 **모두에게** 보낸다(전원을 매번 보내지 않는다)
    //   ⇒ 실측 비율(우리 106 : 대조군 40)이 이 둘로 설명된다
    uint16_t latencyPick = 0;
    if (frame_ >= lastLatencyFrame_ + kServerTickRate / 2) {
        lastLatencyFrame_ = frame_;
        std::vector<uint16_t> alive;
        for (const auto& kv2 : sessions_) if (kv2.second.inGame) alive.push_back(kv2.second.netCode);
        if (!alive.empty()) {
            latencyCursor_ = (latencyCursor_ + 1) % alive.size();
            latencyPick = alive[latencyCursor_];
        }
    }

    // 게임에 들어와 있는 사람에게만 보낸다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;

        // ★★★ 첫 세계 스냅샷 — 접속 처리에서 예약해 둔 것을 **몇 틱 뒤** 여기서 보낸다.
        //   이 한 조각에 `0xe9` 살려내기 · `0x60` 장비 넷 · `0x4c` 집을 것 넷 ·
        //   `0xdd`/`0xde` 최대 체력·마나가 다 실려 있다(정품 대조, 2026-09-08).
        //   접속과 같은 묶음으로 보내면 클라가 아직 맵을 안 읽어 그냥 버린다.
        // ★ 2026-09-13 — **준비(CLIENT_READY) 전이면 0 에서 멈춰 기다린다.** 스냅샷 속 cc 는 대기열을 안 거치고
        //   한 번만 나가므로, 준비 전에 닿아 클라가 버리면 순번 구멍이 생겨 뒤 cc 가 전부 막힌다(Session::importantQ 주석).
        if (s.snapshotIn >= 0 && (s.snapshotIn > 0 || s.importantReady) && --s.snapshotIn < 0 &&
            (!firstSnapshot_.empty() || standalone_)) {
            // 서두는 남의 세션 녹음이라 사람 항목의 번호·자리가 남의 것이다.
            std::vector<uint8_t> patched;
            if (firstSnapshot_.empty()) {
                // ★ 독립 모드 (P5-c-2) — 녹음 없이 조립한다:
                //   27 시각 + a4(사람 + 시야 안 정적) + cc 꼬리부(BuildSnapshotTail).
                patched.push_back(static_cast<uint8_t>(Op::TIMESTAMP));
                patched.push_back(static_cast<uint8_t>(s.tickFrame));
                patched.push_back(static_cast<uint8_t>(s.tickFrame >> 8));
                UpdateMsg m;
                m.headAlias = 0xff;
                m.headNetCode = 0;
                m.headType = 0;
                m.x = s.x;
                m.y = s.y;
                m.flags = 0;
                m.headAnim = 0;
                ObjEntry me;
                // ★ 자세 — 상태에서 뽑는다(PlayerAnimByte: 걷기 4 · 달리기 6 · 가만 0 · 공격 = 애니 번호). 묶음 2-a·3-a
                FillEntry(s.aliases, s.netCode, kPlayerType, s.tickFrame, s.x, s.y,
                          s.facing, me, /*complex=*/true,
                          /*playerAnim=*/ forceAnimPose_ >= 0
                              ? static_cast<uint8_t>(forceAnimPose_)
                              : PlayerAnimByte(s),
                          forceAnimDir_,
                          PlayerAnimFrameSent(s) ? s.animFrame : -1);
                m.objects.push_back(me);
                // 시야 안 정적 — **상한 필수** (AppendAoiStatics 주석: 상한 없이는
                // 스폰 주변 40개↑ 가 480B 를 넘겨 a4 가 통째로 비어 나갔다).
                int snBaseX = s.x, snBaseY = s.y;   // 직전 항목(me)의 자리
                AppendAoiStatics(statics_, s.aliases, s.staticIntroAt,
                                 s.tickFrame, s.x, s.y, snBaseX, snBaseY, m);
                uint8_t ub[480];
                ub[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
                size_t droppedSnap = 0;
                const size_t un = EncodeUpdate(m, ub + 1, sizeof(ub) - 1, &droppedSnap);
                updateDroppedSnap_ += droppedSnap;   // 자리를 갈라 센다(위 주석)
                patched.insert(patched.end(), ub, ub + 1 + un);
                // 3-c: 받는 사람의 지금 프레임 · ★ 09-20 — 버프 비트는 **지금 값**을 싣는다
                //   (박아 둔 `0x00800000` 때문에 클라가 접속 내내 무적으로 봤다 — `JoinBundle.h` 주석)
                // ★ 09-20 — 맵 이름도 **지금 맵**으로. `b1` 에 `"estate"` 가 박혀 있었다
                std::string mn = saMapName_;
                if (mn.size() > 4 && mn.compare(mn.size() - 4, 4, ".map") == 0) mn.resize(mn.size() - 4);
                const std::vector<uint8_t> tail = BuildSnapshotTail(s.netCode, s.charClass, s.tickFrame, s.tickFrame,
                                                                    s.buffs.bits, mn);
                patched.insert(patched.end(), tail.begin(), tail.end());
            } else if (!(patchSnapshot_ &&
                  PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, patched, forceHudOn_))) {
                patched = firstSnapshot_;
            }
            // ★★★ **번호를 다시 매긴 뒤에 보낸다** — 안 그러면 이 뒤에 보내는
            //   신뢰 메시지가 녹음본의 같은 번호에 먹혀 조용히 사라진다(Server.h RenumberSeq 참조).
            RenumberSeq(s, patched);
            const std::vector<uint8_t>& snap = patched;
            Emit(s, /*reliable=*/true, /*stream=*/0, snap.data(), snap.size(), out);
            // 스냅샷 안에 이미 전체번호(ff) 머리의 갱신이 들어 있다 — 그것이 **첫 갱신**이다.
            // 그러니 이 틱에 내 갱신을 또 보태지 않고, 다음 틱부터 별칭 `01` 로 잇는다.
            s.sentFirstUpdate = true;
            // ★★★ **혼자가 아니면 스냅샷 바로 뒤에 전체 갱신을 한 번 더 준다.**
            //   스냅샷은 **이 사람 하나**만 담은 녹음본이라, 이미 와 있던 사람이 거기 없다.
            //   그 뒤로는 별칭 `01` 짜리 작은 갱신만 나가서 **나중에 들어온 쪽은 상대를
            //   소개받을 자리가 없었다**(2026-09-09 사람 눈 판정).
            //   여기서 머리를 되돌리면 다음 틱 갱신이 전체번호로 둘을 다 싣는다.
            if (inGameCount() >= 2) s.sentFirstUpdate = false;
            // ★★★ **소개를 다시 한다 — 스냅샷이 앞서 보낸 소개를 지우기 때문이다.**
            //   이 녹음본 안에 `ea FORGET_DRAWABLES` 와 `b1`(맵 `estate` 로 새 판)이 들어 있다
            //   (2026-09-09 와이어 실측: 545바이트 조각의 210·216번 자리).
            //   접속 순서상 소개(`2d`·`6a`)는 **스냅샷보다 170ms 앞서** 나가므로 통째로 날아간다.
            //   그래서 **나중에 들어온 쪽만** 상대를 못 봤다 — 먼저 온 쪽은 스냅샷을 안 받으니 멀쩡했다.
            //   여기서 다시 세워 두면 다음 틱에 아래 소개 자리가 한 번 더 돈다.
            if (inGameCount() >= 2) s.pendingIntro = true;
            // 스냅샷 꼬리에 방어값 `49` 가 들어 있다 — 그것이 첫 playerReportAnything 몫이다(+232 = +228).
            //   녹음 서두는 위저드 녹음이라 그 값(0x3e6b8520)이다
            // +2184·+10·+6 도 같은 자리 — 꼬리의 dd·de·48(#21) 과 43(#22) 이 첫 playerReportAnything 몫이다.
            // ★★ **「내가 무엇을 알렸나」를 적어야지 「지금 값」을 적으면 안 된다.**
            //   꼬리는 `dd`·`43` 에 **직업 최대치**(HealthOnline)를 싣는다 — 정품은 접속 때 늘 가득이라 둘이 같지만,
            //   `--start-health` 처럼 서버 값이 다르면 어긋난다. 「지금 값」을 적어 두면 PlayerReportAnything 이
            //   「안 바뀌었다」고 보고 **영영 `43` 을 안 보낸다** → 클라 체력바가 가득인 채로 굳는다
            //   (2026-09-14 b5a_heal 화면에서 잡음 — 전에는 HUD 5회 되풀이가 이걸 덮어 가리고 있었다).
            const CharClass tailCls = firstSnapshot_.empty() ? s.charClass : CharClass::Wizard;
            s.armorReported = ArmorValueFloat(tailCls);
            s.statsDirty = false;
            s.healthReported = static_cast<uint16_t>(HealthOnline(tailCls));   // 꼬리가 실은 값
            s.manaPrev      = static_cast<uint16_t>(ManaOnline(tailCls));      // 꼬리 `de`/`45` 가 실은 값
            continue;
        }

        // 자기 몸 갱신 몫 — 바뀐 것만 알린다(playerReportAnything_4D9900 · 묶음 3-d). 스냅샷을 기다리는 동안은 아니다
        if (s.snapshotIn < 0) PlayerReportAnything(s, out);

        // ★★★ **사람 소개를 서로 주고받는다** — `2d NEW_PLAYER`.
        //   내가 보내던 것은 **접속한 본인 것 하나뿐**이었다. 정품은 다르다
        //   (2026-09-09 대조: 정품 클라 A 가 자기 `cb 02` 와 상대 `d0 02` 를 둘 다 받았고,
        //    내 서버에서 A 는 자기 `c9 02` 하나만 받았다).
        //   이 소개가 없으면 클라에 **그 사람의 playerInfo 가 아예 없다** — 오픈녹스
        //   원본을 보면 `2d` 가 이름·배운 주문(100번 자리)·장비·상태 플래그(112번 자리)를
        //   통째로 실어 나르고(`nox_xxx_netNewPlayerMakePacket_4DDA90`),
        //   상태 알림(`6a`)도 `playerInfoGetByID` 가 빈손이면 그 자리에서 되돌아간다.
        //
        //   묶음(216바이트) 안 `2d` 는 87번 자리에서 시작해 129바이트다 — 6+5+7+20+49.
        // ⚠ **여기에 `&& s.snapshotIn < 0` 을 붙였다가 되돌렸다**(2026-09-09).
        //   「나중에 들어온 쪽도 전체 갱신을 받게」 하려던 것인데, 사람 눈으로 재 보니
        //   **먼저 있던 쪽까지 상대를 못 보게 됐다.** 새로 들어온 쪽 몫은 아래
        //   스냅샷 발송 자리에서 따로 챙긴다.
        if (s.pendingIntro) {
            s.pendingIntro = false;
            // ★★★ **사람이 늘면 다음 갱신의 머리를 다시 `ff`(전체)로 되돌린다.**
            //   2026-09-09 사람 눈 판정 — 정품 서버에서는 먼저 들어온 쪽에 상대가
            //   **그려지는데** 내 서버는 양쪽 다 안 그려졌다. 정품 덤프에서 상대 사람이
            //   실려 오는 갱신은 늘 **머리 별칭 `ff`**(번호 0·종류 0)에 세계 오브젝트까지
            //   함께 담긴 큰 갱신이었다(항목 12~15개). 내 것은 접속 첫 틱에 한 번만 `ff` 를
            //   쓰고 그 뒤로는 `01` 짜리 작은 갱신만 보냈다 —
            //   즉 **새로 들어온 사람을 「전체 갱신」에 실어 소개한 적이 없다.**
            for (auto& kv3 : sessions_) {
                if (kv3.second.inGame) kv3.second.sentFirstUpdate = false;
            }
            for (auto& kv2 : sessions_) {
                Session& o = kv2.second;
                if (!o.inGame || o.addr == s.addr) continue;
                // 상태는 **그 사람이 지금 어떤지** 그대로 — 관전 중이면 1, 놀고 있으면 0.
                EmitNewPlayer(o, s, s.observer ? 1 : 0, out);   // 먼저 온 사람에게 새 사람을
                // ★ 09-29 — 먼저 온 사람에게 새 사람의 `e9 <번호2> <프레임4> ff 01`(기본 차림 다시 그리기). 정답지는 새 사람이 기본 장비를 받을 때
                //   `netSendPlayerRespawn_4EFC30(…, 1)` 을 **모두에게** 보낸다 — 정품 녹음 18:53:27.374 먼저 온 워리어가 `e9 61 00 7c 02 00 00 ff 01` 받음.
                //   ✖ 전에는 새 사람 본인(스냅샷 꼬리)에게만 갔다 — 먼저 온 사람 화면에서 새 사람의 로브가 갈색으로 보였다(09-29 사람 판)
                {
                    const uint32_t f = o.tickFrame + 1;
                    const uint8_t e9[9] = { static_cast<uint8_t>(Op::PLAYER_RESPAWN), static_cast<uint8_t>(s.netCode),
                                            static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(f), static_cast<uint8_t>(f >> 8),
                                            static_cast<uint8_t>(f >> 16), static_cast<uint8_t>(f >> 24), 0xff, 0x01 };
                    EmitSeq(o, e9, sizeof(e9), out);
                    ++introRedrawSent_;
                }
                EmitNewPlayer(s, o, o.observer ? 1 : 0, out);   // 새 사람에게 먼저 온 사람을
                SendWornLooks(s, o, out);                       // ★ 09-29 — 그 사람이 입은 것 전부(색 수식 포함 · `sub_4DDE10`)

                // ★★★ **소개 바로 뒤에 상태를 한 번 더 갈아 끼운다.**
                //   `2d` 의 상태 자리는 클라에서 `|=` 로 들어가 **한 번 선 비트가 안 꺼진다.**
                //   그래서 「이 사람은 관전이 아니다」를 `2d` 로는 말할 수 없다 — `6a` 뿐이다.
                //   ⚠ 서로 **상대 것만** 보낸다. 자기 번호로 오면 클라가 HUD 를 다시 켜고 끈다.
                EmitClientStatus(o, s.netCode, s.observer ? 1u : 0u, out);
                EmitClientStatus(s, o.netCode, o.observer ? 1u : 0u, out);

                // ★★★ **상대 몫의 첫 스냅샷을 통째로 보낸다** (`--peer-snapshot`).
                //   2026-09-09 — 자리·좌표 모드·덧붙이·간격·장비·번호·별칭·이름을 다 기각하고
                //   남은 가설 = **스프라이트는 첫 스냅샷에서만 만들어지고 갱신만으로는 안 만들어진다.**
                //   내 몸은 스냅샷에 있어 그려지고, 상대는 갱신에만 있어 미니맵에만 찍힌다.
                //   ⚠ 09-08 에 「스냅샷을 통째로 보내면 받는 클라가 죽는다」고 적었지만
                //     그건 **오픈녹스 클라** 이야기다 — 정품 클라는 `--peer-ops` 를 넓게 줘도 멀쩡했다.
                if (peerSnapshot_ && !firstSnapshot_.empty()) {
                    std::vector<uint8_t> pa, pb;
                    if (!PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, pa, forceHudOn_)) pa = firstSnapshot_;
                    RenumberSeq(o, pa);   // 받는 쪽은 o 다 — 번호도 o 의 것이라야 한다
                    Emit(o, /*reliable=*/true, /*stream=*/0, pa.data(), pa.size(), out);
                    if (!PatchPreludePlayer(firstSnapshot_, o.netCode, o.x, o.y, pb, forceHudOn_)) pb = firstSnapshot_;
                    RenumberSeq(s, pb);
                    Emit(s, /*reliable=*/true, /*stream=*/0, pb.data(), pb.size(), out);
                    ++handled_;
                }
            }
        }

        // ★ 2026-09-13 묶음 3-c — 여기서 45틱 뒤 저절로 되살리던 것을 걷어냈다. 부활은 걷기 틱의 상태 4 가 한다
        //   (UpdatePlayerMovement · PlayerRespawn — 클릭하거나 자동 부활 설정일 때).

        // ★ 재생 모드 — 내가 만든 것 대신 정품이 보냈던 조각을 순서대로 내보낸다.
        //   클라가 그려지면 「내용이 틀렸다」, 안 그려지면 「구조가 틀렸다」로 갈린다.
        // ★ 재생분이 남아 있으면 그것부터. **다 쓰면 내 생성으로 이어받는다** —
        //   「첫 스냅샷만 문제인가, 내 스트림 자체가 문제인가」를 가르는 자리다.
        if (s.preludeAt < mirror_.size()) {
            const std::vector<uint8_t>& m = mirror_[s.preludeAt++];
            if (!m.empty()) {
                // ★ 서두에 실린 프레임을 따라간다. 서두가 끝나면 그 다음 수부터 이어 센다.
                //   안 이으면 시계가 튀고, 앞으로 튄 쪽 클라는 화면이 검다(Server.h 의 대조표).
                if (m.size() >= 3 && m[0] == static_cast<uint8_t>(Op::TIMESTAMP)) {
                    s.tickFrame = static_cast<uint32_t>(m[1]) |
                                  (static_cast<uint32_t>(m[2]) << 8);
                    s.tickFrameSet = true;
                }
                // ★★ 서두는 **녹음본이라 남의 번호·남의 자리**를 담고 있다.
                //   그 안의 사람 항목을 이 사람 것으로 갈아 끼운다 — 안 그러면 클라가
                //   서두에서 「나는 715, 저기 (2600,2578)」로 알았다가 내 갱신이
                //   다른 번호·다른 자리를 말해 **딴 오브젝트로 본다**(2026-09-08 대조).
                std::vector<uint8_t> patched;
                const std::vector<uint8_t>& send =
                    PatchPreludePlayer(m, s.netCode, s.x, s.y, patched, forceHudOn_) ? patched : m;
                // ★ 09-30 — 녹음 조각은 이미 한 데이터그램이다 — 묶지 않고 그대로
                EmitNow(s, /*reliable=*/false, /*stream=*/0, send.data(), send.size(), out);
                ++bundleStat_[static_cast<size_t>(BundleStat::DirectMirror)];
            }
            continue;
        }

        // 서두가 없거나 거기에 프레임이 안 실렸으면 접속한 그 시점부터 센다.
        if (!s.tickFrameSet) {
            s.tickFrame = s.frame;
            s.tickFrameSet = true;
        }
        ++s.tickFrame;
        // R1 — 판끝 동안은 시각·갱신을 안 짠다(원본 `updateRemotePlayers` 가 안 돎) · 시계는 계속 센다(원본 프레임도 `server_E` 에서 오름) ·
        //   중요 메시지는 보낸다(원본 `sendPoll` 은 시각이 없어도 Kind1·중요를 싣는다 — `network_lists.go:185·206~224`)
        if (frozen) { FlushImportant(s, out); continue; }

        // ① 시각 — 본문 2바이트(LE16). 클라는 이 값으로 프레임을 맞춘다.
        {
            uint8_t msg[3];
            msg[0] = static_cast<uint8_t>(Op::TIMESTAMP);
            msg[1] = static_cast<uint8_t>(s.tickFrame);
            msg[2] = static_cast<uint8_t>(s.tickFrame >> 8);
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }
        // ①' 중요 메시지 — 준비 뒤에만, 확인 올 때까지 다시 싣는다(Session::importantQ 주석).
        FlushImportant(s, out);

        // ★★ **첫 세계 스냅샷이 아직 안 나갔으면 갱신을 참는다.** 정품은 USE_MAP 뒤로
        //   시각(`27`)만 몇 번 보내다가 스냅샷을 첫 `a4` 로 내보낸다. 내가 먼저
        //   내 갱신을 흘리면 클라는 **세계를 모르는 채 사람 하나만 받는다**(2026-09-08 대조:
        //   정품 첫 갱신은 항목 14개, 내 첫 갱신은 1개였다).
        if (s.snapshotIn >= 0) continue;

        // ② 상태 갱신 — 머리는 「보는 사람」, 항목에는 **게임에 있는 모두**를 싣는다.
        //    이것이 M 의 「서로를 본다」다. 관심영역(멀리 있는 것은 빼기)은 나중에.
        UpdateMsg m;
        // ★★ 정품은 **첫 갱신만** 머리를 전체번호(ff·번호 0·종류 0)로 보내고,
        //   그 뒤로는 **별칭 `01`** 한 바이트로 보낸다(2026-09-08 대조):
        //     정품  [갱신1] 머리 별칭=ff  [갱신2~] 머리 별칭=01
        //     내 것 계속 ff + 번호 0 + 종류 0
        //   ⚠ **`01` 이 무엇의 별칭인지는 모른다.** 플레이어 별칭은 `cb` 였으니
        //     자기 자신은 아니다. 고정값으로 보고 그대로 맞춘다.
        if (s.sentFirstUpdate) {
            m.headAlias = 0x01;
        } else {
            m.headAlias = 0xff;              // 첫 갱신은 전체 번호로
            m.headNetCode = 0;
            m.headType = 0;
            s.sentFirstUpdate = true;
        }
        m.x = s.x;
        m.y = s.y;
        // ★★★ **머리는 「카메라」다 — 사람이 아니다.** (2026-09-11 짜는 쪽 소스로 확정)
        //
        //   갱신 스트림 머리는 `MSG_PLAYER_OBJ` 에서 만들어지는데(`network_lists.go:230`),
        //   그 메시지를 만드는 함수 이름이 답이다 —
        //   `opennox/src/network.go:573` **`nox_xxx_netPlayerObjSendCamera_519330`** :
        //
        //       buf[0] = MSG_PLAYER_OBJ
        //       PutUint16(buf[1:], 0)      // 번호 **0**
        //       PutUint16(buf[3:], 0)      // 종류 **0**
        //       PutUint16(buf[5:], pos.X)  // 카메라 자리
        //       PutUint16(buf[7:], pos.Y)
        //       buf[9]  = 0                // 방향 **0**
        //       buf[10] = 0xff             // 그림 칸 **없음**
        //       buf[11] = 0                // 자세 **0**
        //
        //   ⭕ 우리 실측(2026-09-08)의 「머리 `ff` · 번호 0 · 종류 0」과 **정확히 맞는다.**
        //   ★ 잠깐 「머리가 곧 보는 사람이니 자세를 여기 실어야 한다」고 고쳤다가 되돌렸다 —
        //     `sub_4DF810` 만 보면 그렇게 읽히지만, **그 입력을 만드는 함수**를 열어 보니
        //     전부 0 인 카메라였다. **정답지도 한 겹만 읽으면 틀린 결론이 난다.**
        m.flags = 0;
        m.headAnim = 0;

        lastLoopSeen_ = 0;
        lastSessions_ = sessions_.size();
        // ★★ **보는 사람을 첫 항목으로 놓는다.** 두 가지가 여기에 달려 있다:
        //   ⑴ 정품 2인 갱신이 「자기 먼저(상대좌표) · 남 나중(절대좌표)」 순서다
        //   ⑵ 아래 상대좌표 최적화가 **첫 항목에만** 걸린다(기준이 머리 좌표라서)
        //   `sessions_` 는 주소로 묶인 map 이라 순서가 회차마다 뒤바뀐다 —
        //   맡겨 두면 남이 첫째가 되어 자기 항목이 절대좌표로 나갔다(2026-09-09 실측).
        //   ★ `--peer-first` 로 순서를 뒤집는다 — **가르는 시험용**이다.
        //     자기는 그려지고 남은 안 그려지는데, 둘의 차이가 이제 「목록에서의 자리」와
        //     그에 딸린 좌표 모드(첫째=상대·나머지=절대)뿐이다. 뒤집었을 때
        //     **남이 그려지고 자기가 안 그려지면** 원인은 자리·좌표 모드이고,
        //     그대로면 원인은 「누구냐」쪽이다(2026-09-09).
        std::vector<Session*> order;
        order.reserve(sessions_.size());
        if (peerFirst_) {
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second != &s) order.push_back(&kv2.second);
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second == &s) order.push_back(&kv2.second);
        } else {
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second == &s) order.push_back(&kv2.second);
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second != &s) order.push_back(&kv2.second);
        }
        // 상대좌표의 기준. 첫 항목은 머리 좌표에서 출발하고, 그 뒤로는 앞 항목 자리를 잇는다.
        int baseX = m.x, baseY = m.y;
        for (Session* op : order) {
            Session& o = *op;
            ++lastLoopSeen_;
            // netPlayerObjSend_518C30 a4 갈래 — 보는 사람 s 에게 o 의 `42` 증감(마지막 피해 뒤 2틱 · 음수만 · Packet0)
            ReportHealthDeltaTo(s, o.netCode, o.health, o.lastDamageFrame, o.hpReportedTo, out);

            ObjEntry e;
            // 별칭은 **클라와 똑같은 규칙**으로 잡는다. 한 칸만 어긋나면
            // 그때부터 클라가 엉뚱한 것을 그린다(Alias.h 참조).
            // ★ `--peer-type N` — **남을 사람 아닌 종류로 보내 본다**(가르는 시험).
            //   자기는 그려지고 남은 안 그려지는데, 자리·좌표·덧붙이·이름·스냅샷을 다 기각했다.
            //   남을 박쥐 같은 다른 종류로 보내서 **뭐라도 그려지면** 클라는 남의 오브젝트를
            //   그릴 수 있는 것이고 문제는 「사람 종류」에 있다. 아무것도 안 그려지면
            //   애초에 남 항목으로는 그릴 것을 안 만드는 것이다(2026-09-09).
            const uint16_t useType =
                (&o != &s && peerType_ != 0) ? peerType_ : kPlayerType;
            // ★ 남도 걷는 자세가 보여야 한다. 사람이 아니면 이 값은 안 쓰인다.
            // ★★ `--anim-pose` 진단은 **여기에도** 걸어야 한다(2026-09-11 자가검증에서 잡음).
            //   앞서 스냅샷 쪽에만 걸어 놓고 「자세가 먹는다」고 판정했는데, **실제로 걸을 때
            //   쓰이는 길은 이 주기 갱신**이다. 한쪽만 재고 다른 쪽을 결론지을 뻔했다.
            FillEntry(s.aliases, o.netCode, useType, s.tickFrame, o.x, o.y, o.facing, e,
                      /*complex=*/true,
                      /*playerAnim=*/ forceAnimPose_ >= 0
                          ? static_cast<uint8_t>(forceAnimPose_)
                          : PlayerAnimByte(o),
                      forceAnimDir_,
                      PlayerAnimFrameSent(o) ? o.animFrame : -1);
            // ★★ **별칭으로 보낼 때는 좌표도 상대로.** 정품 원바이트가 그렇다(2026-09-08):
            //     정품  … 01 <머리좌표4> 00 00 │ cb 00 00 │ 40 │ 끝     (14바이트)
            //     내 것 … 01 <머리좌표4> 00 00 │ 00 c9 <절대좌표4> │ 40 │ 끝  (17바이트)
            //   절대 모드는 앞에 `00` 표시가 더 붙는다. 정품은 그 경로를 안 쓴다.
            //   ⚠ 기준은 **머리 좌표**다 — 이 항목이 목록의 첫째일 때만 맞는다.
            //     사람이 첫 항목이라 지금은 맞지만, 앞에 뭘 더 넣으면 같이 손봐야 한다.
            //   ★★★ **정정(2026-09-09) — 기준은 머리가 아니라 「바로 앞 항목」이다.**
            //     뜯는 쪽(`UpdateStream.cpp`)이 `x += e.dx; y += e.dy;` 로 **이어서 더한다.**
            //     첫 항목만 머리 좌표에서 출발할 뿐, 둘째부터는 앞 항목 자리가 기준이다.
            //     그래서 **둘째 항목도 상대좌표를 쓸 수 있다** — 이걸 몰라서 남 항목을
            //     늘 절대좌표로 보냈고, 그 탓에 남을 별칭으로 보낼 길이 막혀 있었다.
            //   ❌ **`ff` 에도 상대좌표를 쓰는 것은 기각됐다**(2026-09-09 실측).
            //     정품 자기 항목이 `ff <번호> <종류> 01 01 40 00` 이라 되는 줄 알았는데,
            //     그렇게 보내니 **화면이 검어지고 두 클라가 다 이탈했다**
            //     (보낸 것 30·100건 만에 조용해짐). `ff` 는 절대좌표라야 한다.
            if (e.alias != 0xff) {
                const int dx = o.x - baseX, dy = o.y - baseY;
                if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127) {
                    e.absolute = false;
                    e.dx = static_cast<int8_t>(dx);
                    e.dy = static_cast<int8_t>(dy);
                }
            }
            // 다음 항목의 기준은 이 항목의 자리다(절대로 보냈든 상대로 보냈든 같다).
            baseX = o.x;
            baseY = o.y;
            // ★★★ **사람이 둘 이상이면 정품은 짜임을 통째로 바꾼다** (2026-09-08 실측):
            //
            //   a4 01 <머리4> 00 00 │ ff d0 02 c9 02 │ 01 01 │ 40 00 │
            //                         00 ff cb 02 c9 02 │ 66 0c a4 09 │ 00 │ 00 00 00
            //     보는 사람 : `ff`+번호+종류 + **상대**좌표 + `40 00`(2)
            //     남        : `00`(절대) + `ff`+번호+종류 + **절대**좌표 + `00`(1)
            //
            //   ⚠ **하나씩 떼어 재 봤더니 셋 다 효과가 없었다** — 별칭만·덧붙이만·자리만.
            //     셋이 한 벌인 것으로 보고 통째로 맞춘다.
            //   ⚠ 좌표 모드는 위에서 이미 정해졌다. 별칭만 `ff` 로 올리고 **모드는 건드리지 않는다** —
            //     정품의 보는 사람 항목이 `ff` 인데도 좌표가 상대(`01 01`)이기 때문이다.
            if (inGameCount() >= 2) {
                // ★★★ **소개 형태를 자기와 남이 다르게 간다.** 2026-09-09 에 둘 다 재 봤다:
                //
                //   자기 — 별칭으로. 매 틱 `ff` 로 다시 소개하니 클라가 보낸 `a5 NEW_ALIAS` 가
                //          **갱신 하나당 0.94개**(4,585 / 4,875)였다. 정품은 0.03개다.
                //          별칭으로 돌리니 36개로 떨어졌다. 첫 항목이라 좌표도 상대로 가고,
                //          그 꼴(`cb <dx> <dy> 40`)이 정품 1인 갱신과 같다.
                //   남   — **전체번호(`ff`)로.** 별칭으로 보냈더니 **둘째 클라 화면이 검어졌다**
                //          (색 4종, 34건 보내고 조용해짐). 남 항목은 절대좌표라
                //          `00 <별칭> <좌표4>` 꼴이 되는데 클라가 그걸 못 읽는 것으로 보인다.
                //          정품 표본도 남은 `00 ff <번호> <종류> <좌표4> 30` 이다.
                //   `--intro-every-tick` 은 옛 동작(자기도 매 틱 `ff`)을 되살린다(대조용).
                //   ❌ **자기를 별칭으로 보내는 것은 기각됐다**(2026-09-09). 별칭 churn 은
                //      4,585 → 2,302 으로 줄었지만 **스트림이 어긋났다** — 2인 갱신이
                //      「항목 3개」로 뜯혔다: `ce 00 00` 에서 항목이 끝나고 덧붙이 `40` 이
                //      다음 항목의 별칭으로 읽혔다(정품 2인 표본은 자기도 `ff` 다).
                //      churn 이 큰 진짜 까닭은 따로 있다 — 정품은 2인 갱신을 거의 안 보낸다
                //      (3,488 중 **2건**). 둘이 서로 시야 밖이라 각자 자기만 실린다.
                //      즉 이건 **관심영역(AOI)을 안 만든 탓**이지 소개 형태 탓이 아니다.
                //   ★★★ **정정(2026-09-09 저녁) — 매 틱 `ff` 는 클라를 무너뜨린다.**
                //     사람 눈 판정: 둘이 움직이다 보면 **1분 안에 둘 다 끊긴다.**
                //     끊기기 직전 와이어는 멀쩡했다(갱신 30바이트·좌표 정상). 대신 지표가 튄다 —
                //     `a5 NEW_ALIAS` 3,073 대 갱신 2,129 로 **갱신보다 별칭이 많다**(1.44개/갱신).
                //     정품은 0.03이다. 초당 서른 번 오브젝트를 새로 등록시키고 있었던 것.
                //     그래서 **이미 소개한 사람은 별칭으로 보낸다** — 남 항목도 마찬가지다.
                //     남을 별칭으로 못 보내던 까닭은 좌표를 늘 절대로 보냈기 때문인데,
                //     기준이 「앞 항목」이라는 것을 알고 나니 상대좌표로 보낼 수 있다(위 참조).
                if (introEveryTick_ || e.alias == 0xff) {
                    e.alias = 0xff;
                    e.netCode = o.netCode;
                }
                if (&o != &s) {
                    // ⚠ 남 항목을 **절대좌표로 강제하던 것을 뺐다**(2026-09-09).
                    //   그건 정품 표본을 베낀 것인데 그 표본의 상대는 화면 밖이었다.
                    //   기준이 「앞 항목」인 것을 알고 나서 상대좌표가 가능해졌으므로
                    //   위에서 정해진 모드를 그대로 둔다 — 자기 항목과 같은 길이 된다.
                    // ★★★ **정정(2026-09-11) — 남 항목도 별칭 여부와 무관하게 두 바이트다.**
                    //   자기 항목과 같은 결함이었다(아래 `else` 가지 주석 참조).
                    //   `extra = 1` 이면 **자세 바이트가 안 나가** 클라가 다음 항목의
                    //   첫 바이트를 자세로 먹고 그 뒤가 통째로 밀린다.
                    if (e.extra < 2) e.extra = 2;   // 3 = 애니 칸이 붙은 것(묶음 3-a) — 줄이지 않는다
                    // ★★★ **`00` 이 아니라 `0x30` 이다.** 2026-09-09 에 정품 2인 갱신과
                    //   원바이트를 나란히 놓으니 30바이트 중 **이 한 자리만** 달랐다:
                    //     내 것 … 00 ff ce 02 c9 02 <좌표4> **00** │ 00 00
                    //     정품  … 00 ff cb 02 c9 02 <좌표4> **30** │ 00 00
                    //   자기 항목이 늘 `40` 인 것과 같은 자리다(자세·프레임 쪽으로 보인다).
                    //   ⚠ 앞서 「덧붙이만 바꿔 봤다」는 기록이 있지만, 그때는 번호가 겹치고
                    //     HUD 도 꺼져 있어 다른 것이 가리고 있었다.
                    //   ⚠⚠ **`30` 은 「안 그려지는 상태」의 바이트일 수 있다.** 그 값을 뜬
                    //     정품 표본에서 두 사람은 1,400 떨어져 **서로 화면 밖**이었다.
                    //     자기 항목은 `40`(+`00`)인데 그건 그려진다 — `--peer-pose 40` 으로
                    //     남에게도 같은 값을 줘 가른다(2026-09-09).
                    // ⭕ 2026-09-13 묶음 3-a 정정 — `0x30` 은 **그때 상대가 바라보던 쪽**이었다.
                    //   정답지 짜는 쪽(`GAME4_1.c:4471~4478`)이 이 바이트 위 4비트에 방향만 싣는다(0x80 = 애니 칸 붙음).
                    //   고정값으로 덮으면 남의 방향이 늘 3 이 되고 0x80 도 지워진다. 진단 스위치를 줄 때만 덮는다.
                    if (peerPose_ >= 0) e.extraBytes[0] = static_cast<uint8_t>(peerPose_);
                    // ★ 둘째 바이트(자세)는 `FillEntry` 가 넣은 것을 그대로 둔다.
                    //   전에는 여기서 `0x00` 으로 지웠다 — 남이 걸어도 늘 선 자세였다.
                } else {
                    // ★★★ **별칭을 `ff` 로 올렸으면 덧붙이도 첫 소개 규약(2바이트)으로
                    //   같이 올려야 한다.** `FillEntry` 는 「`ff` 면 `40 00`, 별칭이면 `40`」
                    //   으로 정해 놓는데, 여기서 별칭만 `ff` 로 덮고 덧붙이는 1 로 둔 채
                    //   내보내고 있었다. 클라는 `40` 다음 바이트를 **다음 항목의 시작**으로
                    //   읽어 스트림이 통째로 어긋난다.
                    //   지문 = 둘째가 들어온 지 100ms 남짓에 **먼저 있던 클라가 그냥 사라진다**
                    //   (`0x0a CLIENT_CLOSE` 도 `0xae` 도 없이 프로세스가 죽는다).
                    //   2026-09-09 실측 — `--no-update` 로 갱신만 끄면 둘 다 끝까지 살았다.
                    //   ★★★ **정정(2026-09-11 와이어 실측) — 별칭일 때도 두 바이트다.**
                    //
                    //     여기서 `FillEntry` 가 정해 놓은 값을 **하드코딩으로 덮어쓰고 있었다.**
                    //     `extraBytes[1] = 0x00` 이 **자세를 지웠고**, `extra = 1` 이
                    //     **자세 바이트 자체를 안 나가게** 했다. 오늘 하루 자세 실험이
                    //     전부 0 픽셀이던 진짜 원인이다 — `FillEntry` 를 아무리 고쳐도
                    //     이 세 줄이 그 뒤에서 되돌리고 있었다.
                    //
                    //   ⭕ 근거 둘 —
                    //     ⑴ **짜는 쪽 정품 코드** `opennox/src/network_lists.go:297-299`:
                    //        `if b2[0] == MSG_PLAYER_OBJ { off++; b1[off] = b2[11] }`
                    //        — 사람이면 별칭 여부와 무관하게 **자세 한 바이트를 더** 붙인다
                    //     ⑵ **프록시 와이어 실측**(`CAP_c9fix.txt`) — 별칭 `0xc9` 항목이
                    //        2바이트로 읽힌다. ⚠ 다만 그 둘째 바이트 `00` 은 우리가 1 만
                    //        보내서 **다음 항목의 첫 바이트가 끌려 들어온 것**이다.
                    //        클라도 같은 규칙으로 읽으니 그 뒤가 통째로 밀린다
                    //
                    //   ⚠ 「별칭이면 한 바이트」라던 2026-09-08 실측은 **판독 오독**이었다
                    //     (그 덤프엔 별칭 사람 항목이 0건이었다 — `nox_verify` 로 확인).
                    if (e.extra < 2) e.extra = 2;   // 3 = 애니 칸이 붙은 것(묶음 3-a)
                    // `extraBytes` 는 `FillEntry` 가 이미 채웠다 — **덮어쓰지 않는다.**
                }
            }
            m.objects.push_back(e);
        }

        // ★ 4-a2 날아가는 오브젝트.
        // ★★★ **정정(2026-09-16 전수조사 ⓐ) — 「SIMPLE 이라 붙는 바이트가 없다」는 틀렸다.**
        //   `Magic`(955) 하나만 보고 내린 판단을 **날아가는 것 전부에** 못 박아 두었다.
        //   thing.bin 실측 — `Magic`·`MagicMissile`·`Pixie` 는 SIMPLE 이지만
        //   **`MercArcherArrow`(528)·`Fireball`(695)·`StrongFireball`(694) 은 COMPLEX** 다.
        //   복합인데 덧붙이를 안 붙이면 클라가 **다음 항목의 첫 바이트를 덧붙이로 먹어**
        //   그 뒤 스트림이 통째로 밀린다 — 09-15 정품 플레이에서 화살이 기울고 72초에 클라가 죽었다.
        //   ⇒ 종류표로 가른다. 하위 4비트는 **0**(셋째 갈래 `netSendPhantomPlrMb`).
        for (const auto& mo : missiles_) {
            if (!CoordInRange(static_cast<int>(mo.posX), static_cast<int>(mo.posY))) continue;   // ⓒ 관문 — 한 항목이 갱신 한 통을 죽인다
            ObjEntry e;
            // ★ `playerFrame` 은 이름만 사람 것이고 뜻은 **스프라이트 프레임**이다.
            //   화살·쇠뇌살만 정답지대로 `Direction1 >> 3` 을 싣는다(위 FillEntry 주석 ⑬).
            //   그 밖(불덩이 등)은 -1 이라 덧붙이가 한 바이트로 남는다.
            FillEntry(s.aliases, mo.netCode, mo.type, s.tickFrame,
                      static_cast<int>(mo.posX), static_cast<int>(mo.posY), mo.dir1, e,
                      /*complex=*/ThingIsComplex(mo.type),
                      /*playerAnim=*/0, /*forceDir=*/-1,
                      /*playerFrame=*/!IsArrowType(mo.type) ? -1
                                    : (arrowFrameAlt_ > 0
                                           ? (((s.tickFrame / arrowFrameAlt_) % 2) ? 16 : 0)
                                       : forceArrowFrame_ >= 0 ? forceArrowFrame_
                                                               : static_cast<int>(mo.dir1 >> 3)),
                      /*complexAnim=*/0);
            Relativize(e, mo.netCode, static_cast<int>(mo.posX), static_cast<int>(mo.posY),
                       baseX, baseY);
            m.objects.push_back(e);
            ++missileEntriesSent_;   // ★ 갱신에 실린 누계 — 「지금 몇 개」로는 못 답한다(표본이 어긋난다)
        }

        // 4-c3 놓인 오브젝트(독구름 …) — ⓐ 와 같은 부류라 종류표로 가른다(2026-09-16)
        for (const auto& po : placed_) {
            if (!CoordInRange(static_cast<int>(po.x), static_cast<int>(po.y))) continue;   // ⓒ 관문 — 한 항목이 갱신 한 통을 죽인다
            ObjEntry e;
            FillEntry(s.aliases, po.netCode, po.type, s.tickFrame,
                      static_cast<int>(po.x), static_cast<int>(po.y), 0, e,
                      /*complex=*/ThingIsComplex(po.type),
                      /*playerAnim=*/0, /*forceDir=*/-1, /*playerFrame=*/-1,
                      /*complexAnim=*/0);
            Relativize(e, po.netCode, static_cast<int>(po.x), static_cast<int>(po.y), baseX, baseY);
            m.objects.push_back(e);
        }

        // 깃발도 싣는다. 들고 있는 것은 안 싣는다 — 든 사람에 겹쳐 보이면 어지럽다.
        //   ⓐ 와 같은 부류라 **못 박지 않고 종류표로 가른다**(2026-09-16).
        for (const auto& f : flags_) {
            if (!f.carrier.empty()) continue;
            if (!CoordInRange(static_cast<int>(f.x), static_cast<int>(f.y))) continue;   // ⓒ 관문 — 한 항목이 갱신 한 통을 죽인다
            ObjEntry e;
            FillEntry(s.aliases, f.netCode, f.type, s.tickFrame,
                      static_cast<int>(f.x), static_cast<int>(f.y), 0, e,
                      /*complex=*/ThingIsComplex(f.type),
                      /*playerAnim=*/0, /*forceDir=*/-1, /*playerFrame=*/-1,
                      /*complexAnim=*/0);
            Relativize(e, f.netCode, static_cast<int>(f.x), static_cast<int>(f.y),
                       baseX, baseY);
            m.objects.push_back(e);
        }

        // 8-d — 공도 싣는다(깃발과 같은 꼴). 들고 있는 것은 안 싣는다
        for (const auto& bb : ball_) {
            if (!bb.carrier.empty()) continue;
            if (!CoordInRange(static_cast<int>(bb.x), static_cast<int>(bb.y))) continue;   // ⓒ 관문 — 한 항목이 갱신 한 통을 죽인다
            ObjEntry e;
            FillEntry(s.aliases, bb.netCode, bb.type, s.tickFrame,
                      static_cast<int>(bb.x), static_cast<int>(bb.y), 0, e,
                      /*complex=*/ThingIsComplex(bb.type),
                      /*playerAnim=*/0, /*forceDir=*/-1, /*playerFrame=*/-1,
                      /*complexAnim=*/0);
            Relativize(e, bb.netCode, static_cast<int>(bb.x), static_cast<int>(bb.y), baseX, baseY);
            m.objects.push_back(e);
        }

        // 불려 나온 것도 싣는다 — 안 그러면 사람을 때리는 게 화면에 안 보인다.
        //
        // ★ **시야 판정** (2026-09-13 · 기본 꺼짐 — `--creature-aoi`).
        //   정답지 `legacy/GAME4_1.c:4488` `nox_xxx_netSendObjects2Plr_519410` 는 플레이어마다
        //   오브젝트의 「보이는 중」 비트(+148)를 들고, 보이다가 안 보이면 사라짐을 보낸다.
        //   순회 범위는 플레이어 위치 ± (화면절반 + 100) 사각형이다(`server.go:533` · `player.go:325`).
        //   ⚠ 시야선(벽 뒤)은 아직 없다 — 거리만 본다(묶음 1 ⓓ3 에서 넣는다).
        //   ⚠ 들어옴 ±half · 나감 ±half×1.2 — 경계를 드나드는 것이 매 틱 재소개되면 클라가
        //     멎는다(Session::staticIntroAt 주석의 사고). 그 사이는 이전 상태를 유지한다.
        //   ⚠ **별칭은 건드리지 않는다** — Alias.h 의 「잊지 말 것」 주석.
        std::vector<uint16_t> creatureExits;
        for (auto& k : pets_) {
            if (creatureAoiHalfX_ > 0.0) {
                const double dx = std::fabs(k.x - s.x);
                const double dy = std::fabs(k.y - s.y);
                const bool inside  = dx <= creatureAoiHalfX_ && dy <= creatureAoiHalfY_;
                const bool outside = dx > creatureAoiHalfX_ * 1.2 || dy > creatureAoiHalfY_ * 1.2;
                const bool was = s.visibleCreatures.count(k.netCode) != 0;
                if (inside && !was) s.visibleCreatures.insert(k.netCode);
                if (outside && was) {
                    s.visibleCreatures.erase(k.netCode);
                    creatureExits.push_back(k.netCode);
                }
                if (s.visibleCreatures.count(k.netCode) == 0) continue;   // 안 보이는 것은 안 싣는다
            }
            if (!CoordInRange(static_cast<int>(k.x), static_cast<int>(k.y))) continue;   // ⓒ 관문
            ReportHealthDeltaTo(s, k.netCode, k.health, k.lastDamageFrame, k.hpReportedTo, out);   // netSendComplexObject_518960 a3 갈래(+412+2×i)
            ObjEntry e;
            // ★ 10-01 — **애니 번호와 칸을 싣는다.** 전에는 `FillEntry` 기본값 8(가만)만 실어
            //   곰이 **멈춘 그림으로 미끄러졌다**(10-01 사람 판 3 · 임프는 가만 애니가 날갯짓이라 티가 안 났다).
            //   정답지 `netSendComplexObject_518960`(`GAME4_1.c:4294~4296`) — 애니 = `mobActionToAnimation_533790`(= MonsterAnimIndex) ·
            //   칸 = +481 · 줄이는 쪽(`network_lists.go:292~297`)이 칸이 255 가 아니면 0x80 을 켜고 칸 한 바이트를 붙인다.
            //   ⚠ 안 옮김 — `:4297~4301` 아종 0x10(NPC) 의 20% 애니 14 갈래(우리 소환 13종에 NPC 없음)
            const int animIdx = MonsterAnimIndex(k);
            FillEntry(s.aliases, k.netCode, k.type, s.tickFrame,
                      static_cast<int>(k.x), static_cast<int>(k.y), k.facing, e,
                      /*complex=*/true, /*playerAnim=*/0, /*forceDir=*/-1,
                      /*playerFrame=*/k.animFrame == 255 ? -1 : static_cast<int>(k.animFrame),
                      static_cast<uint8_t>(animIdx));
            ++monsterAnimSent_[animIdx & 0x0F];
            Relativize(e, k.netCode, static_cast<int>(k.x), static_cast<int>(k.y),
                       baseX, baseY);
            m.objects.push_back(e);
        }
        // 죽어서 목록에서 빠진 것은 「보이는 중」에서도 지운다 — 번호가 다시 쓰일 때 섞이지 않게.
        for (auto it = s.visibleCreatures.begin(); it != s.visibleCreatures.end();) {
            bool alive = false;
            for (const auto& k : pets_) if (k.netCode == *it) { alive = true; break; }
            it = alive ? std::next(it) : s.visibleCreatures.erase(it);
        }

        // ★ **친구 알림** (2026-09-13 · 기본 꺼짐 — `--friends`).
        //   정답지 분배자(`GAME4_1.c:4488`)는 오브젝트의 +140(대기)·+144(친구) 비트를 보고
        //   `OBJECT_FRIEND_ADD(52)` / `REMOVE(53)` 를 **Packet1** 로 보낸다(`GAME3_2.c:4831` →
        //   `GAME3_3.c:1742` → `netImportant_4E5770` 이 [0xCC][순번][길이][본문] 로 싣는다 = 내 EmitSeq).
        //   친구 = **적이 아닌 것**(`server/object.go:1575` — 함수 이름 「HostileMimic」은 동작과 반대다).
        //   클라는 이 목록으로 **커서·선택 원 색**을 가른다(`video_cgo.go:616` 친구·물건 파랑 / 그 외 빨강).
        //   ⚠⚠ **부분 구현이다** — 적대 판정(`server/object.go:1667` IsEnemyTo, 규칙 20개) 중
        //     ⑬ 주인 같음 · ⑭ 같은 팀 · ⑱ 팀 있으면 적 · ⑳ 그 외 적 만 넣었다.
        //     매혹(⑪)·관전(⑫)·몬스터 종류와 SubClass(⑥~⑩)·생성기(④⑤⑮⑯)·도깨비불(⑰)은 묶음 4·6·8 에서 채운다.
        //
        // ★ **팀이 바뀌면 친구 목록을 비우라고 알린다** — `36 RESET_FRIENDS`(본문 없음, Packet1).
        //   정답지: 팀에 넣을 때(`common__system__team.c:154`)·뺄 때(`GAME1_1.c:377`) 그 사람에게
        //   `sub_4D97E0`(GAME3_2.c:4838) → 곧이어 친구 비트를 다시 세운다(`sub_4E8110`·monsterMarkUpdate).
        //   클라는 목록을 통째로 비운다(`client__network__cdecode.c:595` → `sub_4959B0`).
        //   그래서 여기서도 **보냈다고 적어 둔 친구를 잊어** 아래 블록이 다시 알리게 한다.
        //   ⚠ 친구 알림이 꺼져 있어도 보낸다 — 정답지는 기능 스위치 없이 보낸다(클라 목록이 비어 있으면 해가 없다).
        if (s.team != s.friendResetTeam) {
            s.friendResetTeam = s.team;
            const uint8_t reset[1] = { static_cast<uint8_t>(Op::RESET_FRIENDS) };
            EmitSeq(s, reset, sizeof(reset), out);
            s.friendsSent.clear();
        }
        if (friendsOn_) {
            auto sendFriend = [&](uint16_t nc, bool friendly) {
                const bool sent = s.friendsSent.count(nc) != 0;
                if (friendly == sent) return;   // 바뀔 때만 보낸다
                const uint8_t msg[3] = {
                    static_cast<uint8_t>(friendly ? Op::OBJECT_FRIEND_ADD : Op::OBJECT_FRIEND_REMOVE),
                    static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
                EmitSeq(s, msg, sizeof(msg), out);
                std::printf("[친구] 프레임 %u %s 에게 번호 %u %s\n", frame_, s.addr.c_str(),
                            static_cast<unsigned>(nc), friendly ? "ADD" : "REMOVE");
                if (friendly) s.friendsSent.insert(nc); else s.friendsSent.erase(nc);
            };
            for (const auto& k : pets_) {
                if (creatureAoiHalfX_ > 0.0 && s.visibleCreatures.count(k.netCode) == 0) continue;
                bool friendly = false;
                if (!k.owner.empty() && k.owner == s.addr) {
                    friendly = true;                                        // ⑬ 자기 소환수
                } else if (!k.owner.empty() && s.team != 0) {
                    auto oi = sessions_.find(k.owner);
                    if (oi != sessions_.end() && oi->second.team == s.team) friendly = true;   // ⑭ 같은 팀
                }
                sendFriend(k.netCode, friendly);                            // 그 외 ⑱⑳ 적
            }
            for (const auto& kv2 : sessions_) {
                const Session& o = kv2.second;
                if (!o.inGame || o.addr == s.addr) continue;
                sendFriend(o.netCode, s.team != 0 && o.team == s.team);    // ⑭ 같은 팀 · 그 외 ⑱⑳ 적
            }
            // 사라진 것(죽은 크리처·나간 사람)은 목록에서 지운다 — 번호가 다시 쓰일 때 섞이지 않게.
            for (auto it = s.friendsSent.begin(); it != s.friendsSent.end();) {
                bool alive = false;
                for (const auto& k : pets_) if (k.netCode == *it) { alive = true; break; }
                if (!alive) for (const auto& kv2 : sessions_) if (kv2.second.inGame && kv2.second.netCode == *it) { alive = true; break; }
                it = alive ? std::next(it) : s.friendsSent.erase(it);
            }
        }

        // ── 맵 정적 오브젝트 — **시야 반경 안만, 가까운 순 상한** (독립 서버 P4-b) ──
        //
        // 담는 규칙과 상한의 이유는 AppendAoiStatics 주석에. 드나듦은 별칭 임대(2초)가
        // 알아서 정리하고, 같은 자리에 서 있으면 부분집합이 안 바뀌어 별칭도 안정된다.
        // ⚠ 기본 꺼짐(aoiOn_) — 실클라 검증(P4-c) 전에는 켜지 않는다.
        // ★ 크리처 사라짐(위 루프의 creatureExits)을 먼저 담는다 — 보내는 길은 정적 오브젝트와 같다.
        // ⚠ **효과는 증명되지 않았다** (2026-09-13 돌연변이 시험). 이 줄을 빼 OUT_OF_SIGHT 를 안 보내도
        //   크리처가 화면에서 **똑같이 사라졌다** — 클라가 갱신에서 빠진 동적 오브젝트를 스스로 지운다.
        //   (실험군·돌연변이 옮김 순간 사람 옆 자리 픽셀 차 457 대 445, 두 사진 모두 원뿔 없음)
        //   정답지(`GAME4_1.c:4488`)가 보내므로 남긴다. 효과가 드러나는 조건(정적 오브젝트? 즉시 대 지연?)은 관측대기.
        // 9-a — 비밀벽 여닫힘을 이 사람에게 맞춘다(`sub_519660` 이 분배자에서 도는 자리)
        SecretWallSync(s, out);

        std::vector<uint16_t> outOfSight(creatureExits);
        if (aoiOn_) {
            AppendAoiStatics(statics_, s.aliases, s.staticIntroAt,
                             s.tickFrame, s.x, s.y, baseX, baseY, m);
            // 멀어진 것은 갱신 **항목이 아니라** 별도 메시지로 알린다 —
            // 갱신에 실을 수 있는 것은 「움직인 것」뿐이라는 규약(P4 8런의 결론) 때문이다.
            CollectAoiExits(s.staticIntroAt, statics_, s.x, s.y, outOfSight);
        } else if (!freshStatics_.empty()) {
            // ★ 09-28 사람 판 — 「버리면 가방에서는 빠지는데 바닥에 안 생긴다」. 시야 반경이 꺼진 판은 바닥 물건을
            //   **접속 스냅샷에서만** 소개했다(위 14707 자리). 게임 중 새로 놓인 것은 소개할 길이 없었다 →
            //   그것들만 반경 안에서 한 번 소개한다(소개 규약은 같다 — ff·절대·한 번). 지우기는 줍기의 DESTROY 가 한다
            AppendAoiStatics(statics_, s.aliases, s.staticIntroAt,
                             s.tickFrame, s.x, s.y, baseX, baseY, m, &freshStatics_);
        }

        // ★★★ 2026-09-19 — **512 → 2048.** 정답지가 쓰는 크기다(`gameloop.go:487`
        //   `BufferSize: 2048` · netstr 기본값은 1024).
        //   512 로는 덫밭에서 항목 89개 중 **한 통 최대 47개가 잘렸다**(654틱 중 250틱).
        //   부분 인코딩으로 갱신이 죽지는 않게 됐지만, 잘린 오브젝트는 그 틱에 안 가서
        //   화면에서 깜빡인다. 통을 정답지 크기로 맞춰 잘림 자체를 없앤다.
        uint8_t body[2048];
        body[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        size_t droppedObjs = 0;
        // ★ 10-01 — 통은 **같은 묶음의 `27` 을 뺀 자리**까지(정답지 `sendPoll` 은 `27` 을 쓴 뒤 남은 자리에 갱신을 짠다 · `network_lists.go:185~195`)
        //   ✖ 전에는 2048 통을 따로 채워 `[27 3B]` 뒤에 붙여 조각이 최대 2051(`OverSize`) — 받는 쪽 16384(`netstr.go:332`)라 깨지진 않았다
        //   `27` 은 이 틱 앞에서 이미 `bundleTs` 에 담겼다(위 ① · `Emit`) · 묶지 않는 갈래(시험 모드 등)면 비어 있어 2048 그대로
        const size_t tsLen = s.bundleTs.size();
        const size_t room = tsLen < sizeof(body) ? sizeof(body) - tsLen : 1;
        size_t n = EncodeUpdate(m, body + 1, room - 1, &droppedObjs);
        if (droppedObjs) {
            updateDroppedObjs_ += droppedObjs;
            ++updateDroppedTicks_;
            if (droppedObjs > updateDroppedMax_) updateDroppedMax_ = droppedObjs;
        }
        // ⚠ **이 대입이 없어서 「갱신에 0」이 나왔다**(2026-09-08). `git checkout` 으로
        //   이분법을 하다가 잃었고, 초기값 0 을 관측으로 읽어 한참을 헤맸다.
        //   ★ 계측을 고칠 때는 **그 계측이 실제로 갱신되는지부터** 볼 것.
        lastUpdateObjects_ = m.objects.size();
        // ★ 09-19 — 여기서 갈린다. `n == 0` 이면 아래 `if (n && sendUpdates_)` 가
        //   통째로 건너뛰어 **그 틱 갱신이 한 바이트도 안 나간다.**
        //   「갱신에 실었다」(항목을 만든 수)와 「와이어에 나갔다」는 다른 말이다.
        if (n) {
            ++updateEncOk_;
            if (m.objects.size() > updateEncOkObjsMax_) updateEncOkObjsMax_ = m.objects.size();
        } else {
            ++updateEncFail_;
            if (m.objects.size() > updateEncFailObjsMax_) updateEncFailObjsMax_ = m.objects.size();
        }
        // ★★ 갱신 **바로 뒤에** 사람마다 높이(Z)를 붙인다 — `5e | 번호 2 | z 1`.
        //   정품은 한 조각 안에서 `27 TIMESTAMP` → `a4 UPDATE_STREAM` → `5e` 순으로 보낸다
        //   (2026-09-08 실측: `… a4 cb 42 0a f6 0e 30 00 00 00 00 | 5e cb 02 00`).
        //   녹스는 아이소메트릭이라 그릴 자리를 정하려면 높이가 있어야 하는 것으로 보인다.
        //   ⚠ **z 값은 실측이 아니다** — 정품 표본이 전부 0 이라 0 으로 둔다.
        //   ⚠⚠ **효과는 확인되지 않았다.** 이걸 넣어도 클라 화면에 캐릭터가 안 뜬다
        //      (2026-09-08 AH14 캡처). 깨지지도 않고 정품이 보내는 것이라 남겨 둘 뿐이다.
        //      「이것 때문에 그려진다」고 읽지 말 것.
        // ★★★ **매 틱 보내지 않는다.** 정품은 접속당 **4번**뿐인데 나는 1,081번 보냈다.
        //   그 사이 클라가 죽었다 — 정품 서버 3/3 생존, 내 서버 3/3 사망(2026-09-08 실측).
        //   ⚠ 「깨지지도 않으니 남겨 둔다」고 적어 둔 것이 **270배 과잉**이었다.
        //     정품과 다른 것을 볼 때 **덜 보내는 것만 보고 더 보내는 것을 안 봤다.**
        //   지금은 꺼 둔다. 필요하면 접속 직후 몇 번만 보내는 쪽으로 되살릴 것.
        if (false && n) {
            for (const auto& kv2 : sessions_) {
                const Session& o = kv2.second;
                if (!o.inGame) continue;
                if (1 + n + 4 > sizeof(body)) break;
                uint8_t* p = body + 1 + n;
                p[0] = static_cast<uint8_t>(Op::REPORT_Z_PLUS);
                p[1] = static_cast<uint8_t>(o.netCode);
                p[2] = static_cast<uint8_t>(o.netCode >> 8);
                p[3] = 0;
                n += 4;
            }
        }
        // ★ **시야에서 빠진 것** — `32 <번호2>`. **중요 대기열에 맨 메시지로** 싣는다(2026-09-13).
        //   정답지 `nox_xxx_netObjectOutOfSight_528A60`(GAME4_2.c:7651) = `netSendPacket0_4E5420(pl, 32 번호2, 3, 0, 1)`
        //   — Packet0 은 cc 봉투 없이 본문 그대로, 확인이 올 때까지 다시 싣는다.
        //   전에는 갱신 조각 끝에 한 번만 붙였다(갱신이 안 나간 틱엔 아예 빠졌다).
        //   ⚠ 싣는 것은 **다음 틱** FlushImportant — 정답지는 같은 프레임 갱신 뒤에 싣는다(network_lists.go:217). 1틱 늦다.
        for (uint16_t nc : outOfSight) {
            const uint8_t msg[3] = { static_cast<uint8_t>(Op::OBJECT_OUT_OF_SIGHT),
                                     static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
            EmitImportantRaw(s, msg, sizeof(msg), out);
        }
        // ★★★ **지연 보고 `0xd7`** — 갱신 조각 끝에 `d7 <번호2> <지연2>` 로 붙는다.
        // ✖ **09-22 정정 — 옛 주석의 「약 6틱에 한 번」은 틀린 셈이었다.**
        //   그건 정품에서 「갱신 1,287건 대 d7 217건」을 나눈 값이라 분모가 **틱이 아니라 갱신**인데,
        //   그걸 `frame_ % 6` 으로 옮겨 놓고 **접속자 전원**을 매번 보냈다. 대조군의 2.65배였다.
        //   ⭕ 고친 뒤 같은 60초·같은 도구로 재니 **우리 1:15.1 ↔ 대조군 1:14.7**(갱신 대 d7) 로 붙었다.
        //   ⚠ `0x5e` 처럼 **매 틱 보내면 클라가 죽는다.** 대조군 비율을 지킬 것.
        // ★ 09-22 — 위에서 **한 명만** 뽑아 뒀다(500ms마다 돌아가며). 그 한 건을 모두에게 싣는다
        if (n && latencyPick != 0 && 1 + n + 5 <= sizeof(body)) {
            uint8_t* p = body + 1 + n;
            p[0] = static_cast<uint8_t>(Op::REPORT_ALL_LATENCY);
            p[1] = static_cast<uint8_t>(latencyPick);
            p[2] = static_cast<uint8_t>(latencyPick >> 8);
            p[3] = 0;      // 지연 — 정품 표본이 전부 0 이라 0 으로 둔다
            p[4] = 0;
            n += 5;
        }
        if (n && sendUpdates_) Emit(s, /*reliable=*/false, /*stream=*/0, body, 1 + n, out);
    }
    // ★ 09-30 한 틱 한 데이터그램 — 이 틱(과 지난 틱 뒤 받는 자리)에서 담은 것을 사람마다 한 조각으로(정답지 `server.go:474~475`)
    FlushBundles(out);
    return ticks;
}

}  // namespace nox
