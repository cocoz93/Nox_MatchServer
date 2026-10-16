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
//
// ★★ 2026-09-16 조각 L — **이 값은 정답지에 짝이 없다.** 미결을 그렇게 닫았다.
//   정답지 `src/game.go:1199~1231` 은 맵을 켤 때 **맵에 있는 것을 세기만** 한다 —
//     `PlayerStart` 0 개 → 오류 · CTF|플래그볼인데 **깃발 < 2** → 오류 · 플래그볼인데 **공 < 1** → 오류
//   즉 **깃발과 공은 맵이 갖고 있고**, 「사람이 몇 명 모이면 시작한다」는 개념이 **아예 없다**.
//   ⇒ `ModeNeedPlayers` 도, 「N 명 모이면 `StartCtf`/`StartFlagBall` 을 부른다」도
//     **우리 진단 하네스 고유**다. 대조할 원본이 없으므로 **값을 바꾸지 않는다.**
//
//   ⭕ 실측(도구 `scratchpad/p31/flagmaps.cpp`, 맵 **157개** 전수) —
//     깃발 든 맵 **22개** · 공 시작점 든 맵 **2개**(`BankShot`·`FlagBall`) · 둘 다 **2개**.
//     `Estate`·`G_Castle`·`G_Temple` 은 **깃발 0 · 공 0** 이라 정답지라면 CTF 로 **못 켠다**.
//     그래서 우리가 손으로 놓는다 — 아무 맵에서나 판정하려는 하네스이기 때문이다.
//   ⭕ 겹치지 않는다 — `CapFlag` 를 `--mode ctf` 로 켜도 **깃발 2개**(우리 것만).
//     우리는 맵의 깃발 오브젝트를 **안 읽는다**.
//   ⚠ 2026-09-18 정정 — 여기 「맵 깃발 4개」라고 적혀 있었는데 **틀린 수**다.
//     서버 기동 로그의 충돌 함수 줄로 세니 `Flagx2` · `FlagMarkerx2` · `FlagCollidex2` —
//     **맵 깃발도 2개**다. ⇒ 개수는 우리 것과 같고, 어긋나는 것은 **자리**뿐이다.
//     (`Docs/CHECK_SHEET.md` ⑯ 에도 같은 틀린 수가 있어 함께 고쳤다.)
//   ⚠ 그래서 남는 것은 **미이식 하나** — 「맵의 깃발·공 시작점을 읽어 쓰기」.
//     진짜 CTF 맵에서는 **맵에 그려진 깃발 자리와 서버가 아는 자리가 다르다** → ④대기
//
//   ⭕ 퀘스트는 **혼자 시작이 정상**이다 — 정품 NoxQuest 가 1~4인 협동이다.
//   ⭕ 깃발이 2 인 것은 **두 편을 세우려는 하네스 사정**이지 정답지 규칙이 아니다.
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

// ── ④ 막 들어온 사람이 무엇으로 시작하나 — 관전 / 바로 놀기 ─────────────
//
// ★ 10-16 사람 판 — 대기 방(채팅)에 들어오면 캐릭터가 파랗게 깜빡이고 **클릭해야 남에게 보였다.**
//   우리는 모드와 상관없이 모두 관전(`Session::observer = true`)으로 시작했다.
// 원작 `player.go:411~433`(접속 처리 끝) 갈래를 차례대로 옮긴다 —
//   ① 퀘스트(0x1000)   → 관전 · 「공격 키」 글 없음(`PlayerGoObserver(pl, false, true)`)
//   ② 협동팀(0x200)    → 바로(상태 알림만 — `netReportPlayerStatus_417630`)
//   ③ 호스트 사람 + 그림 없음 → 관전 — ⚠ 우리 서버에는 호스트 사람이 없어 안 옮긴다
//   ④ 채팅(0x80)       → 바로. 팀 채팅(`Sub_40A740` — 채팅이고 팀 설정 53번 바이트 < 0)이면서
//                         팀에 못 드는 사람(`Sub_40AA70 == 0`)만 관전 — ⚠ 우리에겐 팀 채팅 설정이 없어 늘 바로
//   ⑤ 협동(0x800)이 아니면 → 관전 · 「공격 키를 누르세요」(`PlayerGoObserver(pl, true, true)`)
//   ⑥ 협동              → 바로
// 관전으로 보내는 것이 와이어에 남기는 것은 둘 — 접속 묶음 맨 앞 `a9 0c <글 여부>`(「관전 중」 글 · `inform.c` 0xC)와
//   `2d` 112번 자리의 상태 비트 1(`Field3680 & 0x423` · 남의 화면은 이 비트가 서면 그 몸을 안 그린다).
//   바로 노는 사람에게는 둘 다 없다
enum class JoinStart {
    Play,           // 관전 없이 바로
    Observe,        // 관전 + 「공격 키를 누르세요」
    ObserveQuiet,   // 관전 · 그 글은 없음(퀘스트)
};

inline JoinStart JoinStartFor(uint32_t gameFlags) {
    if (gameFlags & 0x1000u) return JoinStart::ObserveQuiet;   // ① 퀘스트
    if (gameFlags & 0x200u) return JoinStart::Play;            // ② 협동팀
    if (gameFlags & 0x80u) return JoinStart::Play;             // ④ 채팅
    if (!(gameFlags & 0x800u)) return JoinStart::Observe;      // ⑤ 협동이 아니면
    return JoinStart::Play;                                    // ⑥ 협동
}

inline const char* JoinStartName(JoinStart js) {
    switch (js) {
        case JoinStart::Play:         return "바로";
        case JoinStart::Observe:      return "관전";
        case JoinStart::ObserveQuiet: return "관전(글 없음)";
    }
    return "?";
}

}  // namespace nox
