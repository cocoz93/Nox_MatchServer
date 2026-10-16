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
//   12번 자리의 `01` 은 2d 의 112번(= 3 + 97 + 12) — **상태 플래그**(`kNewPlayerStatusAt` · 관전 비트 1)다.
//   ✖ 10-16 정정 — 여기 「116번(Field2152) · 뜻 미상」이라 적혀 있었다. 클라 파싱은 112번을 `Field3680 |=` 로 읽는다
//     (`network_client.go:320`) — 116번은 꼬리 16번 자리다
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
                                     const uint8_t* info97, const GameSettings& g, JoinStart start) {
    std::vector<uint8_t> m;
    m.reserve(kJoinBundleSize);

    // a9 INFORM — 갈래 0x0c(관전 시작), 값 = 「공격 키를 누르세요」 글을 붙이나(`PlayerGoObserver` 의 notify)
    //   ★ 10-16 — 바로 노는 사람에게는 안 보낸다(원작은 관전으로 안 보내니 이 알림도 없다 · `ModeRules.h` ④)
    if (start != JoinStart::Play) {
        const uint8_t a9[6] = { 0xa9, 0x0c, static_cast<uint8_t>(start == JoinStart::Observe ? 1 : 0), 0x00, 0x00, 0x00 };
        m.insert(m.end(), a9, a9 + 6);
    }

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
    std::vector<uint8_t> np = BuildNewPlayer(netCode, info97);
    // ★ 10-16 — 상태 자리(꼬리의 `01` = 관전 비트)는 관전일 때만. 바로 노는 사람은 0(원작 `Field3680 & 0x423` 이 0)
    if (start == JoinStart::Play) np[kNewPlayerStatusAt] = 0;
    m.insert(m.end(), np.begin(), np.end());

    return m;
}

// 2d NEW_PLAYER 하나만. **남을 소개할 때** 쓴다.
//
// ★ 왜 따로 필요한가 — 독립 모드에는 녹음(`joinBody_`)이 없어서 `EmitNewPlayer` 가
//   조기 return 하고 있었다(2026-09-10 감사). 그래서 접속 묶음 안 자기 소개 하나만
//   나가고 **남을 소개하는 2d 가 한 번도 안 나갔다.**
// ★★★ 2026-09-19 — **캐릭터가 검은 실루엣으로 그려지던 것을 여기서 막는다.**
//
// 무인 접속(`NOX_JOIN_ADDR`)은 직업·색 선택 화면을 건너뛰므로, 클라가 보내는
// `CLIENT_ACCEPT` 의 **플레이어 정보 97B 가 통째로 0** 이다(09-19 실측 — 0 이 아닌 것은
// `opennox.yml` 의 시리얼 문자열뿐). 그 0 을 그대로 실으면 **색이 전부 검정**이 된다.
// 같은 클라로 opennox 서버에 붙으면 붉은 바지·방패가 보이니, 정답지는 어떻게든 메운다.
//
// 짜임은 `server/player.go:984~995` `PlayerInfo`(정확히 97B, `unsafe.Sizeof` 단언이 있다) —
//   +0 이름 50 · +66 직업 · +67 여성 · +68 머리RGB · +71 피부RGB · +74 콧수염 · +77 염소수염
//   · +80 턱수염 · +83 바지 · +84 윗옷1 · +85 윗옷2 · +86 신발1 · +87 신발2 · +89 이름꼬리 8
//
// 값의 근거 = **정품 녹음 실측**(`proto_test.cpp` 의 `kRec[216]`, 97B 시작 = 그 배열 90번).
//   머리 `73 4d 22`(갈색) · 피부 `da 9a 6e`(살색) · 바지 `0c` · 윗옷 `07 13` · 신발 `17 06`.
//   ⚠ 「정품 **기본값**」이 아니라 **그 녹음 속 캐릭터(Jack)의 색**이다. 검정보다 낫다는 뜻이지
//     정품이 새 캐릭터에 주는 값이라고 읽지 말 것
//
// **색 자리(+68~+87)가 전부 0 일 때만** 메운다 — 사람이 고른 색이 있으면 건드리지 않는다.
void FillDefaultColorsIfBlank(uint8_t* info97) {
    if (!info97) return;
    for (int q = 68; q < 88; ++q) {
        if (info97[q] != 0) return;      // 이미 색이 있다 — 그대로 둔다
    }
    static const uint8_t kHair[3] = { 0x73, 0x4d, 0x22 };
    static const uint8_t kSkin[3] = { 0xda, 0x9a, 0x6e };
    std::memcpy(info97 + 68, kHair, 3);   // 머리
    std::memcpy(info97 + 71, kSkin, 3);   // 피부
    std::memcpy(info97 + 74, kSkin, 3);   // 콧수염 — 녹음도 피부와 같은 값이다
    std::memcpy(info97 + 77, kSkin, 3);   // 염소수염
    std::memcpy(info97 + 80, kSkin, 3);   // 턱수염
    info97[83] = 0x0c;   // 바지
    info97[84] = 0x07;   // 윗옷 1
    info97[85] = 0x13;   // 윗옷 2
    info97[86] = 0x17;   // 신발 1
    info97[87] = 0x06;   // 신발 2
}

// ★ 09-19 — 빈 색 메우기를 켜고 끄는 깃발(기본 **끔**). 까닭은 아래 호출부 주석
bool fillColorsOn = false;

std::vector<uint8_t> BuildNewPlayer(uint16_t netCode, const uint8_t* info97) {
    std::vector<uint8_t> m;
    m.reserve(kNewPlayerSize);
    uint8_t head2d[3] = { 0x2d };
    PutU16(head2d + 1, netCode);
    m.insert(m.end(), head2d, head2d + 3);
    if (info97) m.insert(m.end(), info97, info97 + 97);
    else m.insert(m.end(), 97, 0);
    // ⚠⚠ 09-19 **기본으로 안 부른다.** 아래 함수 주석의 「검은 실루엣」 진단이 **틀렸다** —
    //   그 검은 것은 색이 없어서가 아니라 **장비가 안 그려져서** 맨몸 실루엣이었다.
    //   뿌리는 **두 번째 `2d NEW_PLAYER`** 하나였고, 그것을 안 보내니
    //   색을 안 메워도 **검은 상의·붉은 바지·방패**가 나온다(정답지와 같다).
    //   메우면 오히려 **살색 피부가 드러나 정답지와 달라진다.**
    //   함수와 시험은 남겨 둔다 — `--fill-colors` 로 켜서 그때 화면을 다시 볼 수 있게
    if (fillColorsOn) FillDefaultColorsIfBlank(m.data() + (m.size() - 97));
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
