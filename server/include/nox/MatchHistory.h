// MatchHistory.h — 대전 기록(11-04 플랜2 `Docs/MO_UX_FIX_PLAN_1104.md`)의 순수 계산: 저장된 판 줄 → 결과 · 승패 · 로비 줄.
//
// 왜 — 승패 칸을 새로 안 만들고 저장된 판에서 센다(사용자 결정 · `RATING_DESIGN.md` 정한 것 9). DB 워커 · 메모리 가짜가 판 줄을
//   각자 읽어 `Add` 로 넘기고, 결과 판정 · 승패 셈 · 줄 짓기는 이 한 곳이 맡는다(저장소마다 잣대가 갈리지 않게)
//   · 결과 = 나감 → 이탈 / 중단 → 중단 / 비김 · 승자 없음 → 비김 / 팀 판 · 두 명 판 → 승패 / 여러 명 판 → 1등 승 · 그 밖 등수
//     팀 판의 무소속(팀 0)은 점수 반영이 없어(`Rating.h`) 비김으로 친다
//     상대가 나가 점수가 오른 랭크 중단 판은 승(사용자 결정 11-04 — 「중단 +10」 인데 승패에 안 들면 어색함)
//   · 승패 = 승만 승 · 패 · 등수 · 이탈은 패 · 비김 · 중단은 뺌. 표의 20판이 아니라 전체 판으로 센다(랭크 · 일반 따로)
//   · 점수 = 그 판이 끝난 뒤 값 — 저장돼 있지 않아 지금 점수에서 증감을 최신부터 거꾸로 뺀다. 일반 판 · 증감 없는 판은 `-`
//   · 관전한 판은 기록이 아니다(부르는 쪽이 줄을 안 넘긴다 · 사람 수 `seats` 에도 안 든다)
//
// 로비 → DLL 줄(읽는 쪽 `wsock32_proxy.cpp` `RecOnLine` — 칸 수 · 폭이 그쪽과 맞아야 한다)
//   `NOXRANK RECSUM <랭크 판 있음 0/1> <점수> <순위> <랭크 승> <랭크 패> <일반 승> <일반 패> <이름>`
//   `NOXRANK RECROW <결과> <랭크 0/1> <점수|-> <증감|-> <맵> <킬|-> <데스|-> <초> <끝 유닉스 시각>`
//   `NOXRANK RECEND <줄 수>` · 못 읽음 = `NOXRANK RECFAIL`
//   결과 = W 승 · L 패 · D 비김 · A 중단 · Q 이탈 · 숫자 = 등수
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nox::wol {

constexpr size_t kMatchHistoryMaxRows = 20;     // 표에 싣는 최근 판 수(플랜2)
constexpr size_t kMatchHistoryMapChars = 39;    // DLL `RecRow::map` 칸(`%39s`) — 넘기면 뒤 칸이 밀린다
constexpr uint32_t kMatchHistoryFramesPerSec = 30;   // 시간 칸 = 프레임 / 30(판 결과 `sec` 와 같은 값 · DB 엔 프레임만 있다)

// 저장된 판 하나에서 「나」의 줄
struct MatchHistoryRow {
    bool ranked = false;
    std::string map;
    uint32_t end = 0;           // 끝난 유닉스 시각
    uint32_t frames = 0;
    std::string endReason;      // aborted = 중단
    std::string winnerKind;     // none · player · team · draw
    bool hasWinnerId = false;
    uint32_t winnerId = 0;      // player = netCode · team = 팀 번호
    bool left = false;          // 나감(아래 team · deaths 는 없음)
    uint16_t netCode = 0;
    uint8_t team = 0;
    bool hasKills = false;      // 킬 칸이 생기기 전 판 · 나간 사람은 없음
    uint32_t kills = 0;
    bool hasDeaths = false;
    uint32_t deaths = 0;
    bool hasDelta = false;      // 점수 반영 안 함(일반 판 · 팀 0)은 없음
    int32_t delta = 0;
    uint32_t higher = 0;        // 나보다 판 점수가 높은, 끝까지 한 사람 수(관전 빼고)
    uint32_t seats = 0;         // 판의 사람 수(관전 빼고 · 나간 사람 포함)
};

enum class MatchOutcome : uint8_t { Win, Loss, Place, Left, Aborted, Draw, kCount };
MatchOutcome MatchOutcomeOf(const MatchHistoryRow& r);

// 최신 판부터 차례로 `Add` — 승패는 다 세고 줄은 앞 `kMatchHistoryMaxRows` 개만 쥔다
struct MatchHistory {
    uint32_t rankedWins = 0;
    uint32_t rankedLosses = 0;
    uint32_t normalWins = 0;
    uint32_t normalLosses = 0;
    uint32_t total = 0;         // 넘겨받은 판 수(기록 · 시험)
    std::vector<MatchHistoryRow> rows;
    void Add(const MatchHistoryRow& row);
};

// 로비 줄 전부(RECSUM · RECROW … · RECEND — CRLF 없이). `rating` · `rank` · `ratingFound` = 점수 읽기 답과 같은 뜻
std::vector<std::string> MatchHistoryLines(const std::string& name, bool ratingFound, int32_t rating, uint32_t rank, const MatchHistory& h);

}  // namespace nox::wol
