// ModeRules.h — **실행 파일의 판단을 시험이 닿는 자리로 끌어낸 곳.**
//
// ★ 왜 이 파일이 생겼나 (2026-09-16, 조각 G)
//   `main.cpp` 는 `nox_server` 에만 들어가고 `proto_test` 는 `nox_proto` 만 링크한다.
//   그래서 **시험도 돌연변이도 `main.cpp` 를 한 줄도 안 본다.**
//   실측 — `main.cpp` 판단 줄 **234개 중 시험이 닿는 것 4개**(1.7%)뿐이고,
//   그 넷도 `--selftest` 가 부르는 `SelfTest()` 안의 녹음 왕복 검사다.
//
//   그 사각지대에서 **같은 자리를 두 번 밟았다** —
//     ⑴ 묶음 8 규칙이 실행 파일에서 도달 불가였다(`SetGameFlags` 를 아무도 안 불렀다)
//     ⑵ 모드 갈래의 `else` 가 arena·ctf 가 아닌 **여섯 모드를 전부 퀘스트로** 시작해
//        `--mode elimination` 에 퀘스트 몬스터가 나왔다(조각 E 실측 — 크리처 2·피해 4)
//
//   ⇒ 판단만 여기로 옮긴다. **인자를 받아 값을 돌려줄 뿐 아무것도 안 건드린다**
//     (소켓·서버 상태·출력 없음). 그래서 `proto_test` 가 그냥 부를 수 있다.
//     `main.cpp` 에는 **그 값을 실제로 쓰는 배선만** 남긴다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nox {

// ── ① `--mode` 이름 → `gameFlags` ────────────────────────────────────
//
// 모르는 이름이면 **0**(그래서 0 이 「모름」 표시로 쓰인다).
//
// 비트는 정답지 `common/flags/gameflags.go:31~62` 정본 —
//   KOTR 0x10 · CTF 0x20 · FlagBall 0x40 · Chat 0x80 · Arena 0x100 ·
//   CoopTeam 0x200 · Elimination 0x400 · Coop 0x800 · Quest 0x1000 ·
//   Online 0x2000 · NotQuest 0x10000 · Flag18 0x20000
//
// ★ 바탕 둘은 정답지 `game.go:277~283 startServer` 가 늘 세우는 것이다 —
//   `Online`(0x2000)과 `Flag18`(0x20000). 그리고 **퀘스트가 아니면** `NotQuest`(0x10000)를 얹는다.
//
// ⚠ `GameModeCoop`(0x800)은 정답지의 모드 가면(`GameModeMask` 0x17F0)에 **없다** — 딸림 값이고,
//   `startServer` 는 그것을 오히려 끈다. 그래도 `--mode coop` 을 받는 이유는
//   그 비트를 켠 판에서 규칙이 어떻게 갈리는지 화면으로 보기 위해서다.
inline uint32_t ModeGameFlags(const std::string& mode) {
    const uint32_t kOnline = 0x2000u, kNotQuest = 0x10000u, kFlag18 = 0x20000u;
    const uint32_t base = kOnline | kFlag18;
    if (mode == "quest")       return base | 0x1000u;              // 퀘스트만 NotQuest 를 뺀다
    if (mode == "arena")       return base | kNotQuest | 0x100u;
    if (mode == "ctf")         return base | kNotQuest | 0x20u;
    if (mode == "kotr")        return base | kNotQuest | 0x10u;
    if (mode == "flagball")    return base | kNotQuest | 0x40u;
    if (mode == "chat")        return base | kNotQuest | 0x80u;
    if (mode == "coopteam")    return base | kNotQuest | 0x200u;
    if (mode == "elimination") return base | kNotQuest | 0x400u;
    if (mode == "coop")        return base | kNotQuest | 0x800u;
    return 0;
}

// 우리가 받는 모드 이름 전부. 시험이 **전수로** 돌 수 있게 여기 한 벌만 둔다.
//   ⚠ 모드를 늘리면 여기도 늘려야 한다 — 안 그러면 시험이 새 모드를 안 본다.
inline const char* const* ModeNames(size_t& count) {
    static const char* const kNames[] = {
        "arena", "ctf", "quest", "kotr", "flagball", "chat", "coopteam", "elimination", "coop",
    };
    count = sizeof(kNames) / sizeof(kNames[0]);
    return kNames;
}

// ── ② 판이 시작될 때 **무엇을 놓나** ──────────────────────────────────
//
// ★ 이 자리가 조각 E 가 잡은 결함의 자리다. 전에는 `main.cpp` 안에서
//   `if (ctf) … else StartQuest(...)` 였고, 그 `else` 가 여섯 모드를 삼켰다.
//   갈래를 **값으로** 돌려주면 시험이 아홉 모드를 전수로 훑어 그 삼킴을 잡는다.
enum class ModeSetup {
    None,       // 놓을 것이 없다 — 규칙은 gameFlags 로 이미 켜져 있다
    Ctf,        // 깃발 둘
    FlagBall,   // 공 하나 + 골대 둘
    Quest,      // 몬스터 웨이브
};

// ⚠ `arena` 는 여기까지 오지 않는다 — 부르는 쪽이 `mode != "arena"` 로 먼저 거른다.
//   그래도 값은 정의해 둔다(`None`). 그래야 시험이 아홉을 **전수로** 돌 수 있다.
inline ModeSetup ModeStartKind(const std::string& mode) {
    if (mode == "ctf")      return ModeSetup::Ctf;
    if (mode == "flagball") return ModeSetup::FlagBall;
    // 협동 둘은 NoxQuest 와 한 줄기라 웨이브를 놓는다.
    if (mode == "quest" || mode == "coop" || mode == "coopteam") return ModeSetup::Quest;
    return ModeSetup::None;
}

// 판이 시작되려면 몇 사람이 있어야 하나.
//   ⭕ 퀘스트는 **혼자 시작이 정상**이다 — 정품 NoxQuest 가 1~4인 협동이다.
//   ⭕ 깃발은 **두 편**이 있어야 한다.
//   ⚠ 플래그볼도 두 편짜리로 보이지만 지금은 1 이다. 바꾸면 **동작이 바뀌는 것**이라
//     뽑아내는 이 조각에서는 손대지 않는다 — 진행 파일에 미결로 적었다.
inline int ModeNeedPlayers(const std::string& mode) {
    return (mode == "ctf") ? 2 : 1;
}

// ── ③ 봇을 어디에 어떻게 세우나 ──────────────────────────────────────
//
// `--debug-bot X Y [직업]` 은 여러 번 쓸 수 있다. 몇 번째냐에 따라
// 번호·편·시작 자리가 갈리는데, 그 셈이 `main.cpp` 안에 있어 시험이 못 봤다.
struct BotPlan {
    bool     place      = false;   // 세울 것인가 (음수 자리거나 맵에 시작 자리가 없으면 false)
    bool     useStart   = false;   // 맵의 시작 자리를 쓸 것인가 (`0 0` 이면 그렇다)
    size_t   startIndex = 0;       // 쓸 때 몇 번째 시작 자리인가
    int      x          = -1;      // useStart 가 false 일 때 쓸 자리
    int      y          = -1;
    uint16_t netCode    = 0;
    uint8_t  team       = 1;
};

// `index` 는 몇 번째 `--debug-bot` 인가. `startCount` 는 맵의 시작 자리 수.
//   · `0 0` = 맵 시작 자리. 여럿이면 **자리를 하나씩 옮겨 가며** 쓴다(겹치면 서로 밀어낸다)
//   · 번호는 900 부터 하나씩, 편은 1·2 를 번갈아 — 깃발·플래그볼이 두 편을 요구한다
inline BotPlan PlanBot(size_t index, int wantX, int wantY, size_t startCount) {
    BotPlan p;
    p.netCode = static_cast<uint16_t>(900 + index);
    p.team    = static_cast<uint8_t>(1 + (index % 2));
    if (wantX < 0 || wantY < 0) return p;                  // 음수면 끔
    if (wantX == 0 && wantY == 0) {
        if (startCount == 0) return p;                     // 시작 자리가 없는 맵 — 못 세운다
        p.place = true;
        p.useStart = true;
        p.startIndex = index % startCount;
        return p;
    }
    p.place = true;
    p.x = wantX;
    p.y = wantY;
    return p;
}

// 봇 주소 — `DebugEnterGame` 이 주소 문자열로 사람을 가른다.
inline std::string BotAddr(size_t index) { return "bot" + std::to_string(index); }

}  // namespace nox
