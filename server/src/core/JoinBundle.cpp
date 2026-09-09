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

// af GAME_SETTINGS 의 불변부 (프레임 4바이트 뒤 15바이트).
//   9a 03 01 00 = 버전 0x0001039a (클라 setVersion 이 읽는 값, cdecode 0xAF 절)
//   나머지는 게임 깃발·정원 — 두 접속에서 같았다. 뜻이 갈리는 자리는 P6 에서 잰다.
const uint8_t kAfTail[15] = { 0x9a, 0x03, 0x01, 0x00, 0x00, 0x21, 0x03, 0x00,
                              0x02, 0x00, 0x00, 0x00, 0x20, 0x0f, 0x00 };

// b0 GAME_SETTINGS_2 — 방 이름 "User Game"(16B 칸) + ff×28 + 0×4. 통째 불변.
const uint8_t kB0Body[48] = {
    'U', 's', 'e', 'r', ' ', 'G', 'a', 'm', 'e', 0, 0, 0, 0, 0, 0, 0,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 };

// 2d NEW_PLAYER 에서 정보 97B **뒤에 붙는 29B** — 실측 그대로 (join216.bin 187~215).
//   12번 자리의 `01` 은 클라 쪽 2d 파싱의 116번(Field2152)에 닿는다 — 뜻은 미상.
//   ⚠ 처음에 `01` 을 꼬리 맨 앞에 뒀다가 시험이 「187 에서 2자리 다름」으로 잡았다 —
//     이 상수는 **손으로 옮겨 적지 말고 녹음본에서 뽑을 것.**
const uint8_t kNewPlayerTail[29] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };

}  // namespace

std::vector<uint8_t> BuildJoinBundle(uint16_t netCode, uint32_t frame,
                                     const uint8_t* info97) {
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

    // af GAME_SETTINGS — 프레임 + 불변부
    uint8_t af[20] = { 0xaf };
    PutU32(af + 1, frame);
    std::memcpy(af + 5, kAfTail, sizeof(kAfTail));
    m.insert(m.end(), af, af + 20);

    // b0 GAME_SETTINGS_2
    m.push_back(0xb0);
    m.insert(m.end(), kB0Body, kB0Body + sizeof(kB0Body));

    // 2d NEW_PLAYER — 번호 + 정보 97 + 꼬리 29
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
