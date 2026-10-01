// Wire.h — UDP 한 조각을 메시지로 쪼개는 자리.
//
// 녹스는 **패킷 하나에 메시지를 여러 개 이어 붙인다.** 길이를 모르면 어디서
// 끊기는지 알 수 없고, 한 번 어긋나면 그 뒤가 통째로 밀린다.
//
//   ┌────┬────┬──────┬──────┬──────┬──────┬────
//   │ 0  │ 1  │ 오프 │ 본문 │ 오프 │ 본문 │ ...
//   └────┴────┴──────┴──────┴──────┴──────┴────
//     │    └ 순번
//     └ 0x80=신뢰 전송, 하위 7비트=스트림 ID
//
// ⚠ **머리는 2바이트다.** 예전에 3바이트로 적었던 것은 첫 메시지의 오프코드까지
//    합쳐 본 것이었다 (Docs/PROTOCOL.md 「패킷 뼈대」).
#pragma once

#include <cstdint>
#include <cstring>

#include "nox/MsgLen.h"
#include "nox/Opcode.h"

namespace nox {

// ── 패킷 머리 ────────────────────────────────────────────
constexpr size_t kWireHeaderSize = 2;
constexpr uint8_t kReliableBit = 0x80;

struct WireHeader {
    bool reliable = false;
    uint8_t stream = 0;   // 하위 7비트
    uint8_t seq = 0;

    uint8_t Byte0() const {
        return static_cast<uint8_t>((reliable ? kReliableBit : 0) | (stream & 0x7F));
    }
};

inline bool ReadWireHeader(const uint8_t* p, size_t n, WireHeader& out) {
    if (n < kWireHeaderSize) return false;
    out.reliable = (p[0] & kReliableBit) != 0;
    out.stream = static_cast<uint8_t>(p[0] & 0x7F);
    out.seq = p[1];
    return true;
}

// ── XOR ──────────────────────────────────────────────────
// 접속 수락이 키를 평문으로 흘리고, 그 뒤 그 상대와의 트래픽은 전부 뒤집힌다.
// 키는 **상대마다 다르다** (실측 0x70 · 0x2a). 세션이 들고 있어야 한다.
inline void ApplyXor(uint8_t* p, size_t n, uint8_t key) {
    if (key == 0) return;
    for (size_t i = 0; i < n; ++i) p[i] ^= key;
}

// ── 한 메시지가 몇 바이트를 먹는가 ───────────────────────
//
// 돌려주는 값은 **오프코드 1바이트를 포함한** 전체 크기다.
// 0 이면 "여기서 더 못 읽는다"는 뜻이고, 그때는 남은 것을 통째로 미상으로 센다.
//
// 표(MsgLen.cpp)로 정해지지 않는 것 중 **규칙을 아는 것**은 여기서 직접 잰다.
// 근거는 각 줄에 달아둔다.
inline size_t MessageSize(const uint8_t* p, size_t n, bool fromClient) {
    if (n < 1) return 0;
    const uint8_t op = p[0];
    const uint8_t* b = p + 1;          // 본문 첫 바이트
    const size_t bn = n - 1;           // 남은 본문 길이

    switch (op) {
        // SERVER_INFO — 고정 69 + 서버 이름(0으로 닫음)
        case static_cast<uint8_t>(Op::SERVER_INFO): {
            if (bn < 69) return 0;
            size_t i = 69;
            while (i < bn && b[i] != 0) ++i;
            if (i >= bn) return 0;     // 닫는 0을 못 봤다
            return 1 + i + 1;
        }
        // PLAYER_INPUT — 본문 첫 바이트가 나머지 길이 (msg_player_input.go)
        case static_cast<uint8_t>(Op::PLAYER_INPUT): {
            if (bn < 1) return 0;
            const size_t need = 1 + b[0];
            return bn < need ? 0 : 1 + need;
        }
        // TEXT_MESSAGE — 10바이트 칸 + 글자. 글자 수는 7번, UTF8/지역화가 아니면 2배
        // (msg_text.go: Flags 0x02=UTF8, 0x08=Localized)
        case static_cast<uint8_t>(Op::TEXT_MESSAGE): {
            if (bn < 10) return 0;
            size_t sz = b[7];
            if ((b[2] & 0x02) == 0 && (b[2] & 0x08) == 0) sz *= 2;
            // ★ 글자가 남은 바이트보다 길다고 적혀 있으면 **남은 것까지만** 읽는다.
            //    받는 쪽이 그렇게 한다(msg_text.go: `if sz > len(data) { sz = len(data) }`).
            //    이때 이 메시지가 조각의 마지막이 된다.
            if (sz > bn - 10) sz = bn - 10;
            return 1 + 10 + sz;
        }
        // SEQ_IMPORTANT — 다른 메시지를 감싸는 봉투. ID(2) + 길이(1) + 그만큼
        case static_cast<uint8_t>(Op::SEQ_IMPORTANT): {
            if (bn < 3) return 0;
            const size_t need = 3 + b[2];
            return bn < need ? 0 : 1 + need;
        }
        // SERVER_CMD — 3번 자리가 글자 수이고 글자는 UTF-16 이라 두 배,
        // **그 뒤에 닫는 0 이 두 바이트 더 붙는다.**
        //
        // ★ `4 + 2*sz` 로 읽었다가 2바이트씩 모자랐다. 조각 16,193개가 그 때문에
        //   끝까지 안 쪼개졌다(끝에 `00 a8` 이나 `00 00` 이 남았다).
        //
        //   실측 — `3f`+`aa`+`bb` 로 된 32바이트 조각 16,197개에서 `bb` 이후가
        //   **예외 없이 13바이트**(오프코드 1 + 본문 12)였고 글자수 자리는 늘 3이었다.
        //   `4 + 2*3 + 2 = 12` 로 딱 맞는다.
        //
        //   근거는 인코더에도 있다 — noxnet/msg_server_cmd.go 가 글자를 쓴 뒤
        //   `data[4+n] = 0; data[4+n+1] = 0` 으로 두 바이트를 더 쓴다.
        //   (그쪽 Decode 는 그 둘을 안 세어 Encode 와 어긋난다)
        //
        // ⚠ 글자수 자리가 3 이 아닌 표본은 아직 못 봤다. 다른 값에서도 이 식이
        //   맞는지는 확인 못 했다.
        //
        // ✖ 10-01 정정 — **원본 서버 해독기 식(`5 + 2×글자수` · `network_server.go:211~222`)으로 바꾼다.**
        //   ⑴ 위 「13바이트」는 그때 `aa` 를 몸 5 로 읽던 판 위에서 잰 것 · 글자수 3 이면 원본 식 11 + 끝 채움 `00 00` 으로도 맞는다
        //      (인코더가 덧쓴 두 0 은 원본 서버도 안 읽는다 — 조각 끝이라 `0x00` 에서 멈출 뿐)
        //   ⑵ 글자수 7 표본(10-01 자동 클라 `bb 00 c9 02 07 0d f0 ad f0 00…` · 남은 19)은 원본 식 19 와 꼭 맞고 옛 식(21)으로는 멈췄다
        //   ⇒ 옛 식은 `aa` 몸 5 가 `bb` 첫 바이트를 먹어 가려져 있었다(`aa` 를 4 로 고치며 드러남)
        case static_cast<uint8_t>(Op::SERVER_CMD): {
            if (bn < 4) return 0;
            const size_t need = 4 + 2 * static_cast<size_t>(b[3]);
            return bn < need ? 0 : 1 + need;
        }
        // INFORM — 첫 바이트가 알림 종류이고, 종류마다 크기가 정해져 있다
        // (noxnet/msg_inform.go). 가변은 15번(문자열) 하나뿐이다.
        case static_cast<uint8_t>(Op::INFORM): {
            if (bn < 1) return 0;
            size_t body = 0;
            switch (b[0]) {
                case 0: case 1: case 2:              // 주문 오류·성공·능력
                case 3: case 4:                      // 시간초과·깃발 회수
                case 8: case 16:                     // 깃발 부활·다른 편
                case 12: case 13:                    // 관전·플레이어 상태
                case 18: case 19: case 20: case 21:  // 나감·이동·비밀·이동 대기
                    body = 4; break;
                case 5: case 6: case 7:
                case 9: case 10: case 11:            // 깃발·공·왕관 — 편+사람
                    body = 8; break;
                case 14: body = 9; break;            // 처치
                case 17: body = 0; break;            // 직업 바뀜
                case 15: {                           // 글자표 이름 — 1 + 문자열 + 0
                    if (bn < 3) return 0;
                    size_t i = 2;
                    while (i < bn && b[i] != 0) ++i;
                    if (i >= bn) return 0;
                    body = 1 + (i - 2) + 1;
                    break;
                }
                default: return 0;                   // 모르는 종류는 넘겨짚지 않는다
            }
            const size_t need = 1 + body;
            return bn < need ? 0 : 1 + need;
        }
        // ★★★ **`0xC9` 는 방향에 따라 뜻이 다르다** (2026-09-11 — 두 클라 런이 잡아냈다).
        //
        //     서버→클라  `MSG_SERVER_QUIT_ACK`   (`cdecode.c:2452`)
        //     클라→서버  **`MSG_TRADE`**          (`sdecode.c:346`)
        //
        //   그리고 클라→서버 쪽은 **하위 명령(`b[0]`)마다 길이가 다르다.**
        //   ⭕ 표는 정품 서버 디코더 `server__network__sdecode.c:346-445` 의 반환값 그대로다.
        //
        //   ⚠ 이걸 몰라 `-1` 이던 탓에 **클라 조각을 통째로 버렸다** — 두 클라 런에서
        //     `[버림] c9(MSG_TRADE)` 가 213건까지 쌓여 판이 무효가 됐다(종료 5).
        //     `0xd4`·`0xae` 와 **같은 꼴**이다: 같이 실린 정상 메시지까지 죽는다.
        //   ★ 클라가 거래를 안 해도 온다 — **남이 화면에 들어오면** 보내는 것으로 보인다
        //     (한 클라 런에서는 한 건도 안 왔다).
        case 0xC9: {
            if (!fromClient) break;            // 서버→클라는 표를 따른다
            if (bn < 1) return 0;
            size_t total = 0;                  // 오프코드까지 넣은 **총** 길이
            switch (b[0]) {
                case 0x0E: case 0x11: case 0x12: total = 2; break;
                case 0x0F: case 0x10: case 0x15: case 0x16:
                case 0x18: case 0x1A: case 0x1C: case 0x1E: total = 4; break;
                case 0x17: case 0x19: total = 5; break;
                // ★★★ **`0x02` 는 거래가 아니라 `0xd0` 와 같은 계열이다** (2026-09-11 실측).
                //   멈춤 표본이 그대로 말해 줬다 —
                //       [멈춤표본] op=c9 남은 11 | c9 02 03 0d f0 ad f0 00
                //   `MsgLen.cpp` 의 `0xd0 MSG_DIALOG` 주석에 적힌 무늬와 **글자까지 같다** —
                //       d0 02 03 0d f0 ad f0 00 00 00 [b8|00]   (남은 11 = 본문 10)
                //   가운데 `0d f0 ad f0` 은 `0xF0ADF00D` 매직이다. 즉 **머리만 다른 같은 꼴**이고
                //   본문은 10 이다. 정품 서버 디코더(`sdecode.c`)의 TRADE 갈래에는 이 값이
                //   없지만, **클라는 실제로 보낸다** — 표에 없다고 안 오는 것이 아니다.
                //   ⚠ 이걸 몰라 두 클라 런에서 조각 1,560건을 버렸다.
                case 0x02: total = 11; break;
                default: return 0;             // 모르는 하위 명령은 넘겨짚지 않는다
            }
            return bn + 1 < total ? 0 : total;
        }
        // ── 묶음 8-e 투표(0xEE) — **갈래마다 길이가 다르다**
        //   클라→서버(`sdecode.c:315`) — 갈래 `0~3` 은 이름(와이드 25자)이 붙어 **총 52** ·
        //     갈래 `4`·`5` 는 이름이 없어 **총 2**
        //   서버→클라(`cdecode.c:2871`) — `6` 은 **총 3** · `7` 은 **총 2**
        //   ⚠ 이걸 표 하나로 못 적어 `MsgLen` 에는 -1 로 둔다. 안 가르치면 조각을 읽다 **멈춘다**
        case static_cast<uint8_t>(Op::VOTE): {
            if (bn < 1) return 0;
            size_t total = 0;
            if (fromClient) {
                total = (b[0] <= 3) ? 52 : 2;
            } else {
                if (b[0] == 6) total = 3;
                else if (b[0] == 7) total = 2;
                else return 0;
            }
            return bn + 1 < total ? 0 : total;
        }
        // XFER — 하위 코드마다 크기가 다르다 (noxnet/netxfer/message.go)
        case static_cast<uint8_t>(Op::XFER_MSG): {
            if (bn < 1) return 0;
            size_t need = 0;
            switch (b[0]) {
                case 0: need = 1 + 138; break;                 // Start
                case 1: need = 1 + 2;   break;                 // Accept
                case 2: {                                      // Data
                    if (bn < 7) return 0;
                    const size_t dsz = static_cast<size_t>(b[5]) |
                                       (static_cast<size_t>(b[6]) << 8);
                    need = 1 + 6 + dsz;
                    break;
                }
                case 3: need = 1 + 4; break;                   // Ack
                case 4: need = 1 + 1; break;                   // Done
                case 5:
                case 6: need = 1 + 2; break;                   // Cancel · Abort
                default: return 0;
            }
            return bn < need ? 0 : 1 + need;
        }
        default: break;
    }

    const int len = MsgBodyLenByDir(op, fromClient);
    if (len < 0) return 0;
    const size_t need = static_cast<size_t>(len);
    return bn < need ? 0 : 1 + need;
}

// ── 패킷 안의 메시지를 하나씩 ────────────────────────────
//
// 쓰는 법:
//   nox::MessageIter it(body, bodyLen, true);
//   while (it.Next()) { it.op() ... it.size() ... }
//   if (it.stoppedAt() >= 0) { 여기서 못 읽었다 }
class MessageIter {
public:
    MessageIter(const uint8_t* data, size_t size, bool fromClient)
        : data_(data), size_(size), fromClient_(fromClient) {}

    bool Next() {
        if (off_ >= size_ || stopped_) return false;
        const size_t n = MessageSize(data_ + off_, size_ - off_, fromClient_);
        if (n == 0) {
            // 길이를 모른다. 남은 것을 통째로 미상으로 넘긴다.
            // ★ 여기서 "끝까지 읽었다"고 여기면 안 된다 — 뒤에 붙은 것을 못 본 것이다.
            stopOp_ = data_[off_];
            stopRest_ = size_ - off_;
            stopped_ = true;
            return false;
        }
        op_ = data_[off_];
        body_ = data_ + off_ + 1;
        bodyLen_ = n - 1;
        off_ += n;
        ++count_;
        return true;
    }

    uint8_t op() const { return op_; }
    const uint8_t* body() const { return body_; }
    size_t bodyLen() const { return bodyLen_; }
    size_t count() const { return count_; }

    bool stopped() const { return stopped_; }
    int stopOp() const { return stopped_ ? static_cast<int>(stopOp_) : -1; }
    size_t stopRest() const { return stopRest_; }

private:
    const uint8_t* data_;
    size_t size_;
    bool fromClient_;
    size_t off_ = 0;
    size_t count_ = 0;

    uint8_t op_ = 0;
    const uint8_t* body_ = nullptr;
    size_t bodyLen_ = 0;

    bool stopped_ = false;
    uint8_t stopOp_ = 0;
    size_t stopRest_ = 0;
};

}  // namespace nox
