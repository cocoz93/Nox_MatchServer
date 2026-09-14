// JoinBundle.cpp — 접속 묶음 조립. 불변 바이트의 출처는 전부 실측이다
// (2026-09-10 SNAP_M_warrior_ctf 복호본 · 두 접속 대조는 PROTOCOL.md).
//
// ★ 검증 = proto_test TestJoinBundle — 녹음본과 같은 값(번호·프레임·정보97)을 넣어
//   조립하면 **216바이트가 전부 같아야** 한다. 한 자리라도 다르면 조립이 틀린 것.
#include "nox/JoinBundle.h"

#include <cstring>

namespace nox {
namespace {

inline void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// 2d NEW_PLAYER 에서 정보 97B **뒤에 붙는 29B** — 실측 그대로 (join216.bin 187~215).
//   12번 자리의 `01` 은 클라 쪽 2d 파싱의 116번(Field2152)에 닿는다 — 뜻은 미상.
//   ⚠ 처음에 `01` 을 꼬리 맨 앞에 뒀다가 시험이 「187 에서 2자리 다름」으로 잡았다 —
//     이 상수는 **손으로 옮겨 적지 말고 녹음본에서 뽑을 것.**
const uint8_t kNewPlayerTail[29] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };

}  // namespace

// ── 묶음 8-a — `af` 20바이트(`network.go:468~479`) ────────────────
std::vector<uint8_t> BuildGameSettings(const GameSettings& g, uint32_t frame) {
    std::vector<uint8_t> m(20, 0);
    m[0] = 0xaf;
    PutU32(m.data() + 1, frame);
    PutU32(m.data() + 5, g.clientVersion);
    PutU32(m.data() + 9, g.gameFlags & 0x7FFF0u);          // ★ 정답지가 여기서 가린다
    PutU32(m.data() + 13, g.subFlags);
    m[17] = g.maxPlayers;
    m[18] = g.limitValue;
    m[19] = g.limitKind;
    return m;
}

// ── 묶음 8-a — `b0` 49바이트(`network.go:480~500`) ────────────────
std::vector<uint8_t> BuildGameSettings2(const GameSettings& g) {
    std::vector<uint8_t> m(49, 0);
    m[0] = 0xb0;
    // 방 이름 — 정답지 `copy(buf[1:17], name)` 그대로 **16바이트까지** 보내고,
    //   그 뒤에 **16번 자리를 0 으로** 못 박는다. ⇒ 쓸 수 있는 글자는 15자이고 끝은 항상 끝표다
    const size_t n = g.name.size() < 16 ? g.name.size() : 16;
    std::memcpy(m.data() + 1, g.name.data(), n);
    m[16] = 0;
    for (int i = 0; i < 5; ++i) PutU32(m.data() + 17 + 4 * i, g.vals[i]);
    for (int i = 0; i < 4; ++i) m[37 + i] = g.bytes4[i];
    PutU32(m.data() + 41, g.field48);
    PutU32(m.data() + 45, g.timeLeft);
    return m;
}

std::vector<uint8_t> BuildJoinBundle(uint16_t netCode, uint32_t frame,
                                     const uint8_t* info97, const GameSettings& g) {
    std::vector<uint8_t> m;
    m.reserve(kJoinBundleSize);

    // a9 INFORM — 갈래 0x0c(관전 시작), 값 1
    const uint8_t a9[6] = { 0xa9, 0x0c, 0x01, 0x00, 0x00, 0x00 };
    m.insert(m.end(), a9, a9 + 6);

    // 28 FULL_TIMESTAMP
    uint8_t t28[5] = { 0x28 };
    PutU32(t28 + 1, frame);
    m.insert(m.end(), t28, t28 + 5);

    // 2c JOIN_DATA — 번호 + 미상 u32(실측 0)
    uint8_t j2c[7] = { 0x2c };
    PutU16(j2c + 1, netCode);
    PutU32(j2c + 3, 0);
    m.insert(m.end(), j2c, j2c + 7);

    // af GAME_SETTINGS · b0 GAME_SETTINGS_2 — 묶음 8-a 부터 **설정에서 짓는다**
    {
        const std::vector<uint8_t> af = BuildGameSettings(g, frame);
        m.insert(m.end(), af.begin(), af.end());
        const std::vector<uint8_t> b0 = BuildGameSettings2(g);
        m.insert(m.end(), b0.begin(), b0.end());
    }

    // 2d NEW_PLAYER — 번호 + 정보 97 + 꼬리 29
    const std::vector<uint8_t> np = BuildNewPlayer(netCode, info97);
    m.insert(m.end(), np.begin(), np.end());

    return m;
}

// 2d NEW_PLAYER 하나만. **남을 소개할 때** 쓴다.
//
// ★ 왜 따로 필요한가 — 독립 모드에는 녹음(`joinBody_`)이 없어서 `EmitNewPlayer` 가
//   조기 return 하고 있었다(2026-09-10 감사). 그래서 접속 묶음 안 자기 소개 하나만
//   나가고 **남을 소개하는 2d 가 한 번도 안 나갔다.**
std::vector<uint8_t> BuildNewPlayer(uint16_t netCode, const uint8_t* info97) {
    std::vector<uint8_t> m;
    m.reserve(kNewPlayerSize);
    uint8_t head2d[3] = { 0x2d };
    PutU16(head2d + 1, netCode);
    m.insert(m.end(), head2d, head2d + 3);
    if (info97) m.insert(m.end(), info97, info97 + 97);
    else m.insert(m.end(), 97, 0);
    m.insert(m.end(), kNewPlayerTail, kNewPlayerTail + sizeof(kNewPlayerTail));
    return m;
}

std::vector<uint8_t> BuildUseMap(const std::string& mapName, uint32_t mapCrc,
                                 uint32_t frame) {
    std::vector<uint8_t> m(kUseMapSize, 0);
    m[0] = 0x2b;
    const size_t n = mapName.size() < 32 ? mapName.size() : 31;
    std::memcpy(m.data() + 1, mapName.data(), n);
    PutU32(m.data() + 33, mapCrc);
    PutU32(m.data() + 37, frame);
    return m;
}

}  // namespace nox
