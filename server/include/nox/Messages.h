// Messages.h — 1단계에 필요한 메시지들.
//
// 목표는 둘이다.
//   ① 클라의 멀티플레이 목록에 내 서버가 뜬다      (DISCOVER → INFO)
//   ② 클라가 접속해서 대기 상태까지 간다            (CONNECT → ACCEPT → TRY_JOIN → JOIN_OK)
//
// 242개 중 여기 있는 건 열몇 개뿐이다. 나머지는 맵에 서는 단계에서 채운다.
//
// 출처: opennox/libs noxnet (MIT). 자세한 고지는 THIRD_PARTY.md 참조.
// 바이트 배치는 그쪽에서 가져온 사실이고, 아래 구조체와 직렬화는 새로 썼다.
//
// ⚠ 여기 값은 아직 실제 트래픽으로 검증하지 않았다. 캡처로 대조하기 전까지는
//    "이럴 것"이지 "이렇다"가 아니다. 어긋난 자리는 Docs/PROTOCOL.md에 적어둔다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nox/ByteBuffer.h"
#include "nox/Opcode.h"

namespace nox {

// 게임 포트. UDP 하나로 발견·접속·게임을 다 받는다.
constexpr uint16_t kGamePort = 18590;

// 접속을 거절할 때 SERVER_ERROR에 실어 보내는 사유.
enum class ConnectError : uint8_t {
    LowPing = 0,
    HighPing = 1,
    LowLevel = 2,
    HighLevel = 3,
    Closed = 4,
    Banned = 5,
    WrongPassword = 6,
    IllegalClass = 7,
    TimeOut = 8,
    FindFailed = 9,
    NeedRefresh = 10,
    Full = 11,
    DupSerial = 12,
    WrongVer = 13,
};

// ── 서버 발견 ────────────────────────────────────────────
// 클라가 목록 화면에서 뿌리고, 서버가 자기 소개로 답한다.

// 클라 → 서버. 9바이트.
struct MsgServerDiscover {
    static constexpr Op kOp = Op::SERVER_DISCOVER;
    static constexpr size_t kSize = 9;

    uint8_t unk0[5] = {};
    uint32_t token = 0;   // 답에 그대로 돌려줘야 클라가 자기 요청으로 알아본다

    bool Decode(Reader& r) {
        return r.Bytes(unk0, sizeof(unk0)) && r.U32(token);
    }

    bool Encode(Writer& w) const {
        return w.Bytes(unk0, sizeof(unk0)) && w.U32(token);
    }
};

// 서버 → 클라. 69바이트 고정 + 서버 이름(0으로 닫음).
struct MsgServerInfo {
    static constexpr Op kOp = Op::SERVER_INFO;
    static constexpr size_t kFixedSize = 69;

    uint8_t playersCur = 0;
    uint8_t playersMax = 0;
    uint8_t unk2[5] = {};
    std::string mapName;        // 9바이트 칸
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t unk18[7] = {};
    uint16_t flags = 0;
    uint8_t unk27[2] = {};
    uint8_t unk29[8] = {};
    uint8_t unk37[4] = {};
    uint32_t token = 0;         // DISCOVER에서 받은 값을 그대로
    uint8_t unk45[20] = {};
    uint8_t unk65[4] = {};
    std::string serverName;     // 가변, 0으로 닫음

    size_t EncodeSize() const { return kFixedSize + serverName.size() + 1; }

    bool Encode(Writer& w) const {
        return w.U8(playersCur) && w.U8(playersMax) &&
               w.Bytes(unk2, sizeof(unk2)) &&
               w.FixedString(mapName, 9) &&
               w.U8(status1) && w.U8(status2) &&
               w.Bytes(unk18, sizeof(unk18)) &&
               w.U16(flags) &&
               w.Bytes(unk27, sizeof(unk27)) &&
               w.Bytes(unk29, sizeof(unk29)) &&
               w.Bytes(unk37, sizeof(unk37)) &&
               w.U32(token) &&
               w.Bytes(unk45, sizeof(unk45)) &&
               w.Bytes(unk65, sizeof(unk65)) &&
               w.TailString(serverName);
    }

    bool Decode(Reader& r) {
        return r.U8(playersCur) && r.U8(playersMax) &&
               r.Bytes(unk2, sizeof(unk2)) &&
               r.FixedString(mapName, 9) &&
               r.U8(status1) && r.U8(status2) &&
               r.Bytes(unk18, sizeof(unk18)) &&
               r.U16(flags) &&
               r.Bytes(unk27, sizeof(unk27)) &&
               r.Bytes(unk29, sizeof(unk29)) &&
               r.Bytes(unk37, sizeof(unk37)) &&
               r.U32(token) &&
               r.Bytes(unk45, sizeof(unk45)) &&
               r.Bytes(unk65, sizeof(unk65)) &&
               r.TailString(serverName);
    }
};

// ── 접속 ─────────────────────────────────────────────────

// 클라 → 서버. 본문 없음.
struct MsgServerConnect {
    static constexpr Op kOp = Op::SERVER_CONNECT;
    static constexpr size_t kSize = 0;
};

// 서버 → 클라. 5바이트.
// ★ 이 답에 XOR 키가 실린다. 이 시점 이후의 패킷은 전부 이 키로 뒤집힌다.
struct MsgServerAccept {
    static constexpr Op kOp = Op::SERVER_ACCEPT;
    static constexpr size_t kSize = 5;

    uint32_t id = 0;
    uint8_t xorKey = 0;   // 0이면 평문

    bool Encode(Writer& w) const { return w.U32(id) && w.U8(xorKey); }
    bool Decode(Reader& r) { return r.U32(id) && r.U8(xorKey); }
};

// 클라 → 서버. 97바이트. 이름·직업·레벨·클라 버전을 들고 온다.
//
// ⚠ 이름 칸의 크기가 확실하지 않다. noxnet 쪽에서 넣을 때는 50바이트,
//    꺼낼 때는 52바이트로 서로 다르게 다룬다. 여기서는 구조 주석을 따라
//    이름 50 / 직업 1 / 레벨 1로 갈라두었다. 캡처로 확정할 것.
struct MsgServerTryJoin {
    static constexpr Op kOp = Op::SERVER_TRY_JOIN;
    static constexpr size_t kSize = 97;

    uint8_t unk0 = 0;
    std::string playerName;    // UTF-16LE, 50바이트 칸 (25자)
    uint8_t playerClass = 0;
    uint8_t playerLevel = 0;
    std::string serial;        // 24바이트 칸
    uint32_t version = 0;
    uint32_t team = 0;
    uint8_t unk85[10] = {};
    uint8_t unk95 = 0;
    uint8_t unk96 = 0;

    bool Decode(Reader& r) {
        return r.U8(unk0) &&
               r.FixedString16(playerName, 50) &&
               r.U8(playerClass) && r.U8(playerLevel) &&
               r.FixedString(serial, 24) &&
               r.U32(version) && r.U32(team) &&
               r.Bytes(unk85, sizeof(unk85)) &&
               r.U8(unk95) && r.U8(unk96);
    }

    bool Encode(Writer& w) const {
        return w.U8(unk0) &&
               w.FixedString16(playerName, 50) &&
               w.U8(playerClass) && w.U8(playerLevel) &&
               w.FixedString(serial, 24) &&
               w.U32(version) && w.U32(team) &&
               w.Bytes(unk85, sizeof(unk85)) &&
               w.U8(unk95) && w.U8(unk96);
    }
};

// 서버 → 클라. 본문 없음. 셋 다 접속 결과다.
struct MsgJoinOk {
    static constexpr Op kOp = Op::SERVER_JOIN_OK;
    static constexpr size_t kSize = 0;
};

struct MsgJoinFail {
    static constexpr Op kOp = Op::SERVER_JOIN_FAIL;
    static constexpr size_t kSize = 0;
};

struct MsgPasswordRequired {
    static constexpr Op kOp = Op::PASSWORD_REQUIRED;
    static constexpr size_t kSize = 0;
};

// 서버 → 클라. 1바이트. 거절 사유.
struct MsgServerError {
    static constexpr Op kOp = Op::SERVER_ERROR;
    static constexpr size_t kSize = 1;

    ConnectError err = ConnectError::Closed;

    bool Encode(Writer& w) const { return w.U8(static_cast<uint8_t>(err)); }
    bool Decode(Reader& r) {
        uint8_t v = 0;
        if (!r.U8(v)) return false;
        err = static_cast<ConnectError>(v);
        return true;
    }
};

// 스트림을 받아들였다는 표시. 1바이트.
struct MsgAccepted {
    static constexpr Op kOp = Op::ACCEPTED;
    static constexpr size_t kSize = 1;

    uint8_t id = 0;

    bool Encode(Writer& w) const { return w.U8(id); }
    bool Decode(Reader& r) { return r.U8(id); }
};

// ── 연결 유지 ────────────────────────────────────────────
// 클라가 재는 것과 서버가 재는 것이 따로 있다. 서버 쪽에만 표식 한 바이트가 붙는다.

struct MsgClientPing {
    static constexpr Op kOp = Op::CLIENT_PING;
    static constexpr size_t kSize = 4;

    uint32_t timeMs = 0;

    bool Encode(Writer& w) const { return w.U32(timeMs); }
    bool Decode(Reader& r) { return r.U32(timeMs); }
};

struct MsgClientPong {
    static constexpr Op kOp = Op::CLIENT_PONG;
    static constexpr size_t kSize = 4;

    uint32_t timeMs = 0;

    bool Encode(Writer& w) const { return w.U32(timeMs); }
    bool Decode(Reader& r) { return r.U32(timeMs); }
};

struct MsgServerPing {
    static constexpr Op kOp = Op::SERVER_PING;
    static constexpr size_t kSize = 5;

    uint8_t ind = 0;
    uint32_t timeMs = 0;

    bool Encode(Writer& w) const { return w.U8(ind) && w.U32(timeMs); }
    bool Decode(Reader& r) { return r.U8(ind) && r.U32(timeMs); }
};

struct MsgServerPong {
    static constexpr Op kOp = Op::SERVER_PONG;
    static constexpr size_t kSize = 5;

    uint8_t ind = 0;
    uint32_t timeMs = 0;

    bool Encode(Writer& w) const { return w.U8(ind) && w.U32(timeMs); }
    bool Decode(Reader& r) { return r.U8(ind) && r.U32(timeMs); }
};

struct MsgKeepAlive {
    static constexpr Op kOp = Op::KEEP_ALIVE;
    static constexpr size_t kSize = 0;
};

// ── 종료 ─────────────────────────────────────────────────

struct MsgClientClose {
    static constexpr Op kOp = Op::CLIENT_CLOSE;
    static constexpr size_t kSize = 0;
};

struct MsgServerClose {
    static constexpr Op kOp = Op::SERVER_CLOSE;
    static constexpr size_t kSize = 0;
};

// ── 게임 중 ──────────────────────────────────────────────
//
// 여기부터는 **실제 캡처에서 바이트를 확인한 것들**이다. 위쪽 접속 절과 달리
// "이럴 것"이 아니라 "이렇다". 표본과 근거는 Docs/PROTOCOL.md 에 있다.
//
// ⚠ 이것들은 대개 껍데기 안에 들어 온다 —
//   `0xaa IMPORTANT`(서버→클라 쪽은 1바이트 표식) 뒤에 이어 붙거나,
//   `0xcc SEQ_IMPORTANT` 로 한 겹 더 싸여 온다. 껍데기를 안 벗기면 **안 보인다.**

// 서버 → 클라. 2바이트. 누가 죽었는지만 알린다.
// 실측 `e8 d8 02` — 번호 728. (2026-09-06 덤프)
struct MsgPlayerDied {
    static constexpr Op kOp = Op::PLAYER_DIED;
    static constexpr size_t kSize = 2;

    uint16_t netCode = 0;

    bool Encode(Writer& w) const { return w.U16(netCode); }
    bool Decode(Reader& r) { return r.U16(netCode); }
};

// 서버 → 클라. 8바이트.
// 실측 `e9 d0 02 5d b8 00 00 ff 01` — 번호 720 · 틱 47,197.
//
// ★ 4바이트가 **틱 번호**다. 표본 다섯의 (값 차이 ÷ 시각 차이)가 모두 28.0~28.3/초로,
//   걸음으로 따로 잰 틱 35.77ms(28.0Hz)와 맞았다. 서로 다른 길로 얻은 값이 만난 것이다.
// ⚠ 끝의 `ff 01` 은 다섯 표본 전수가 같아서 뜻을 모른다. 그대로 싣고 나른다.
struct MsgPlayerRespawn {
    static constexpr Op kOp = Op::PLAYER_RESPAWN;
    static constexpr size_t kSize = 8;

    uint16_t netCode = 0;
    uint32_t tick = 0;
    uint8_t unk7 = 0xFF;   // 표본 전수 0xff
    uint8_t unk8 = 0x01;   // 표본 전수 0x01

    bool Encode(Writer& w) const {
        return w.U16(netCode) && w.U32(tick) && w.U8(unk7) && w.U8(unk8);
    }
    bool Decode(Reader& r) {
        return r.U16(netCode) && r.U32(tick) && r.U8(unk7) && r.U8(unk8);
    }
};

// 서버 → 클라. 2바이트. **내 체력의 절대값**이다(번호가 없다).
// 실측 `43 4a 00` → 74. 지속 피해가 들면 74 → 72 → 69 → 65 로 **35ms 간격**에 이어 온다.
struct MsgReportPlayerHealth {
    static constexpr Op kOp = Op::REPORT_PLAYER_HEALTH;
    static constexpr size_t kSize = 2;

    uint16_t health = 0;

    bool Encode(Writer& w) const { return w.U16(health); }
    bool Decode(Reader& r) { return r.U16(health); }
};

// 서버 → 클라. 4바이트. 남의 체력이 얼마나 변했는지.
// 실측 `42 d8 02 81 ff` — 번호 728 · **−127**. 표본 열 건이 전수 음수였다(피해만 있었다).
struct MsgReportHealthDelta {
    static constexpr Op kOp = Op::REPORT_HEALTH_DELTA;
    static constexpr size_t kSize = 4;

    uint16_t netCode = 0;
    int16_t delta = 0;    // 음수 = 피해

    bool Encode(Writer& w) const { return w.U16(netCode) && w.U16(static_cast<uint16_t>(delta)); }
    bool Decode(Reader& r) {
        uint16_t v = 0;
        if (!r.U16(netCode) || !r.U16(v)) return false;
        delta = static_cast<int16_t>(v);
        return true;
    }
};

// 서버 → 클라. 6바이트. 지금 체력과 최대 체력.
// 실측 `dd d0 02 4b 00 4b 00` — 번호 720 · 75/75. 최대값이 위 `0x43` 이 오르내리는 범위와 맞는다.
struct MsgReportTotalHealth {
    static constexpr Op kOp = Op::REPORT_TOTAL_HEALTH;
    static constexpr size_t kSize = 6;

    uint16_t netCode = 0;
    uint16_t health = 0;
    uint16_t maxHealth = 0;

    bool Encode(Writer& w) const { return w.U16(netCode) && w.U16(health) && w.U16(maxHealth); }
    bool Decode(Reader& r) { return r.U16(netCode) && r.U16(health) && r.U16(maxHealth); }
};

// 서버 → 클라. 4바이트. 바닥의 물건을 주웠다.
// 실측 `4b b0 00 7e 02` — 번호 176 · 종류 638(BluePotion).
// ★ 같은 순간 같은 번호로 `0x31 DESTROY_OBJECT` 가 함께 나간다 — 바닥의 물건이 사라지는 것.
struct MsgReportPickup {
    static constexpr Op kOp = Op::REPORT_PICKUP;
    static constexpr size_t kSize = 4;

    uint16_t netCode = 0;
    uint16_t type = 0;    // 와이어 종류 번호 = thing.bin 순번 + 1

    bool Encode(Writer& w) const { return w.U16(netCode) && w.U16(type); }
    bool Decode(Reader& r) { return r.U16(netCode) && r.U16(type); }
};

// 서버 → 클라. 8바이트. 수식이 붙는 물건을 주웠다(무기·방어구).
// 실측 `4c cc 02 6d 04 ff bf ff ff` — 번호 716 · 종류 1133(StreetPants).
// ⚠ 수식 4바이트는 `ff` 가 「없음」 표시로 보이나 미확정이다. 그대로 나른다.
struct MsgReportModifiablePickup {
    static constexpr Op kOp = Op::REPORT_MODIFIABLE_PICKUP;
    static constexpr size_t kSize = 8;

    uint16_t netCode = 0;
    uint16_t type = 0;
    uint8_t mods[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    bool Encode(Writer& w) const {
        return w.U16(netCode) && w.U16(type) && w.Bytes(mods, sizeof(mods));
    }
    bool Decode(Reader& r) {
        return r.U16(netCode) && r.U16(type) && r.Bytes(mods, sizeof(mods));
    }
};

// 서버 → 클라. 4바이트. 물건을 떨어뜨렸다.
// 실측 `4d db 02 91 04` — 번호 731 · 종류 1169(Longsword).
// ★ 죽음(`0xE8`) **0.03초 뒤**에 방패·검이 이걸로 떨어졌다. 죽으면 장비를 흘린다.
struct MsgReportDrop {
    static constexpr Op kOp = Op::REPORT_DROP;
    static constexpr size_t kSize = 4;

    uint16_t netCode = 0;
    uint16_t type = 0;

    bool Encode(Writer& w) const { return w.U16(netCode) && w.U16(type); }
    bool Decode(Reader& r) { return r.U16(netCode) && r.U16(type); }
};

// 서버 → 클라. 2바이트. 그 물건을 착용했다.
// 실측 `60 cc 02` — 접속 직후 처음 지급 장비가 줍기(`0x4C`) 뒤에 곧바로 이걸로 이어진다.
struct MsgReportEquip {
    static constexpr Op kOp = Op::REPORT_EQUIP;
    static constexpr size_t kSize = 2;

    uint16_t netCode = 0;

    bool Encode(Writer& w) const { return w.U16(netCode); }
    bool Decode(Reader& r) { return r.U16(netCode); }
};

// ── 껍데기 ───────────────────────────────────────────────
//
// ★★ 이걸 안 벗기면 위의 것들이 **하나도 안 보인다.** 2026-09-07 에 이것 때문에
//    세 덤프 90만 메시지에서 죽음·체력·줍기가 전부 0건으로 보였다.

// 서버 → 클라. 신뢰 전송 껍데기. 머리 3바이트 + 속 메시지.
// 실측 `cc 20 00 07 | 6a cb 02 00 00 00 00` — 번호 0x0020 · 속 7바이트.
struct MsgSeqImportant {
    static constexpr Op kOp = Op::SEQ_IMPORTANT;
    static constexpr size_t kHeaderSize = 3;

    uint16_t id = 0;
    uint8_t innerSize = 0;   // 속 메시지의 길이(오프코드 포함)

    size_t EncodeSize() const { return kHeaderSize + innerSize; }

    // 속은 그대로 두고 머리만 다룬다. 벗긴 뒤 innerSize 바이트를 다시 읽으면 된다.
    bool Encode(Writer& w) const { return w.U16(id) && w.U8(innerSize); }
    bool Decode(Reader& r) { return r.U16(id) && r.U8(innerSize); }
};

// 서버 → 클라. 머리 10바이트 + 글자. 말풍선.
// 실측 `a8 d0 02 02 96 0e 0a 0b 02 00 00 74 00` — 번호 720 · UTF-8 · (3734, 2826) · 두 글자.
//
// ★ 글자 길이는 `size` 인데 깃발에 UTF8(0x2)·Localized(0x8) 가 **없으면 UTF-16 이라 두 배**다.
// ★ 말풍선 좌표를 서버가 정해 준다.
struct MsgTextMessage {
    static constexpr Op kOp = Op::TEXT_MESSAGE;
    static constexpr size_t kHeaderSize = 10;

    enum Flags : uint8_t { kTeam = 0x01, kUtf8 = 0x02, kLocalized = 0x08, kNotice = 0x10 };

    uint16_t netCode = 0;
    uint8_t flags = kUtf8;
    uint16_t posX = 0;
    uint16_t posY = 0;
    uint8_t size = 0;        // 글자 수
    uint16_t durMs = 0;
    std::vector<uint8_t> data;

    // 글자가 차지하는 바이트 수. 깃발이 정한다.
    size_t DataSize() const {
        return (flags & (kUtf8 | kLocalized)) ? size : static_cast<size_t>(size) * 2;
    }
    size_t EncodeSize() const { return kHeaderSize + DataSize(); }

    bool Encode(Writer& w) const {
        return w.U16(netCode) && w.U8(flags) && w.U16(posX) && w.U16(posY) &&
               w.U8(size) && w.U16(durMs) &&
               w.Bytes(data.data(), data.size());
    }
    bool Decode(Reader& r) {
        if (!(r.U16(netCode) && r.U8(flags) && r.U16(posX) && r.U16(posY) &&
              r.U8(size) && r.U16(durMs))) {
            return false;
        }
        data.resize(DataSize());
        return data.empty() || r.Bytes(data.data(), data.size());
    }
};

} // namespace nox
