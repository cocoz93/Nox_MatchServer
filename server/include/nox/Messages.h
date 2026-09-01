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

} // namespace nox
