// MatchHistory.h — 대전 기록(11-04 플랜2 `Docs/MO_UX_FIX_PLAN_1104.md`)의 순수 계산: 저장된 판 줄 → 결과 · 승패 · 로비 줄.
//
// 왜 — DB 워커 · 메모리 가짜 · 다시 계산 도구가 판 줄을 각자 얻어 넘기고, 결과 판정 · 승패 셈 · 줄 짓기는 이 한 곳이 맡는다(저장소마다 잣대가 갈리지 않게)
//   ★ 11-04 전적 검색 — 승패는 판 저장 때 합계 표(`match_totals`)에 올리고 읽을 땐 그 한 행 + 최근 20줄만(`RATING_DESIGN.md` 정한 것 9 뒤집음).
//     전엔 승패를 세려고 계정의 판을 전부 읽어 조회 하나의 크기가 판 수를 따라갔다. 판 행이 정본 · 합계는 파생 값(틀리면 `nox_match_totals` 로 다시)
//   · 결과 = 나감 → 이탈 / 중단 → 중단 / 비김 · 승자 없음 → 비김 / 팀 판 · 두 명 판 → 승패 / 여러 명 판 → 1등 승 · 그 밖 등수
//     팀 판의 무소속(팀 0)은 점수 반영이 없어(`Rating.h`) 비김으로 친다
//     상대가 나가 점수가 오른 랭크 중단 판은 승(사용자 결정 11-04 — 「중단 +10」 인데 승패에 안 들면 어색함)
//   · 승패 = 승만 승 · 패 · 등수 · 이탈은 패 · 비김 · 중단은 뺌. 표의 20판이 아니라 전체 판으로 센다(랭크 · 일반 따로 · `MatchTotals::Add` 한 곳)
//   · 점수 = 그 판이 끝난 뒤 값 — 저장돼 있지 않아 지금 점수에서 증감을 최신부터 거꾸로 뺀다. 일반 판 · 증감 없는 판은 `-`
//   · 관전한 판은 기록이 아니다(부르는 쪽이 줄을 안 넘긴다 · 사람 수 `seats` 에도 안 든다)
//
// 로비 → DLL 줄(읽는 쪽 `wsock32_proxy.cpp` `RecOnLine` — 칸 수 · 폭이 그쪽과 맞아야 한다)
//   `NOXRANK RECSUM <랭크 판 있음 0/1> <점수> <순위> <랭크 승> <랭크 패> <일반 승> <일반 패> <이름>`
//   `NOXRANK RECROW <결과> <랭크 0/1> <점수|-> <증감|-> <맵> <킬|-> <데스|-> <초> <끝 유닉스 시각>`
//   `NOXRANK RECEND <줄 수>` · 못 읽음 = `NOXRANK RECFAIL` · ★ 11-04 전적 검색 — 없는 사용자 = `NOXRANK RECNONE <물은 이름>`(`RecordDesk.h`)
//   결과 = W 승 · L 패 · D 비김 · A 중단 · Q 이탈 · 숫자 = 등수
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nox/MatchRecord.h"
#include "nox/Rating.h"

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

// 판 `m` 의 자리 `seat` 에서 그 사람의 줄(판 저장 · 메모리 가짜가 같이 부른다 · 관전 여부는 부르는 쪽이 본다). `rr` = `RatingDeltas(m)`
MatchHistoryRow MatchRowOf(const MatchRecord& m, const RatingResult& rr, size_t seat);

// 계정 하나의 승패 합계 — 판마다 `Add`(저장 · 다시 계산 · 메모리 가짜가 같은 함수로)
struct MatchTotals {
    uint32_t games = 0;         // 관전 뺀 판 수
    uint32_t rankedWins = 0;
    uint32_t rankedLosses = 0;
    uint32_t normalWins = 0;
    uint32_t normalLosses = 0;
    void Add(const MatchHistoryRow& row);
    bool operator==(const MatchTotals& o) const {
        return games == o.games && rankedWins == o.rankedWins && rankedLosses == o.rankedLosses && normalWins == o.normalWins && normalLosses == o.normalLosses;
    }
};

// 답 하나 — 합계 + 최근 판(저장 차례 최신부터 · `kMatchHistoryMaxRows` 개까지)
struct MatchHistory {
    MatchTotals totals;
    std::vector<MatchHistoryRow> rows;
};

// 로비 줄 전부(RECSUM · RECROW … · RECEND — CRLF 없이). `rating` · `rank` · `ratingFound` = 점수 읽기 답과 같은 뜻
std::vector<std::string> MatchHistoryLines(const std::string& name, bool ratingFound, int32_t rating, uint32_t rank, const MatchHistory& h);

}  // namespace nox::wol
