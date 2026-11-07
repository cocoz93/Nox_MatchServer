// Rating.h — 판 하나 → 자리별 점수 증감(레이팅 조각 1 — `Docs/RATING_DESIGN.md` 「정한 것」 1~6).
//
// 왜 — 점수 규칙을 한 함수에 모은다. DB 워커 · 메모리 가짜가 같은 함수로 반영하고(판 저장 트랜잭션 안), 시험은 갈래를 개수로 본다.
//   상대 세기를 보는 계산(Elo 꼴)으로 바꿀 때도 이 함수 하나만 바꾼다
//   · 시작 1000 · 한 판 ±10(고정 — 10-30 사용자)
//   · 랭크 판만(`MatchRecord::ranked`) — 일반 판은 아무도 반영 안 함
//   · 중단(`endReason` = aborted) = 반영하되 모두 0(나간 사람도)
//     ★ 10-31 → 나간 사람이 있으면 나감 −10 · 남은 사람 +10(없으면 모두 0) — 1대1 은 나가면 늘 중단이라 나가기가 손해 없는 길이었음
//   · 비김 · 승자 없음 = 끝까지 한 사람 0 · 나간 사람 −10
//   · 팀 판(승자 = 팀) = 이긴 팀 +10 · 진 팀 −10 · 나간 사람 −10
//   · 각자 판(승자 = 사람) = 끝까지 한 사람(관전자 빼고)의 점수 순위로 1등 +10 ~ 꼴등 −10 고르게(같은 점수는 순위 평균 · 반올림) · 나간 사람 −10
//   · 반영 안 함(증감 없음 — DB 칸 NULL) = 관전자 · 계정 없는 사람(로비 안 거침) · 팀 판의 무소속(팀 0)
//     계정 없는 사람도 각자 판 순위의 자리는 차지한다(상대이므로) — 점수만 안 바뀐다
// ★ 11-07 점수 묶기 조각 3(`Docs/MO_RANK_MATCH_PLAN_1107.md` 5절) — 1:1 랭크 판(관전자 빼고 정확히 둘 · 둘 다 계정)은 Elo — 위 줄들보다 먼저 가른다
//   · 내가 이길 확률 E = 1 / (1 + 10^((상대 판 전 점수 − 내 판 전 점수) / 400))
//   · 이김 · 짐 — 이긴 쪽 몫 w = round(20 × (1 − 이긴 쪽 E)) · 최소 1 · 이긴 쪽 +w · 진 쪽 −w(합 0)
//   · 비김 · 승자 없음 — 낮은 쪽 몫 d = round(20 × (0.5 − 낮은 쪽 E)) · 낮은 쪽 d − 5 · 높은 쪽 −d − 5(합 −10)
//   · 하나만 나감 = 남은 쪽이 이김(1:1 은 나가면 늘 중단 — 위 10-31 줄) · 둘 다 나감 = 둘 다 짐(각자 −round(20 × 제 E)) · 나간 사람 없는 중단 = 둘 다 0
//   · 이긴 사람 = 판 기록의 승자 칸(`winnerId` — 각자 판은 netCode · 팀 판은 팀 번호). 전적 승패(`MatchHistory.cpp` `MatchOutcomeOf`)와 같은 잣대
//   · 둘이 아니거나 계정 없는 사람이 낀 랭크 판은 위 옛 줄 그대로
#pragma once

#include <cstdint>
#include <vector>

#include "nox/MatchRecord.h"

namespace nox {

constexpr int32_t kRatingStart = 1000;   // `ratings` 행이 없는 사람의 점수
constexpr int32_t kRatingStep = 10;      // 한 판 최대 증감(1:1 Elo 밖의 판)
// ★ 11-07 1:1 Elo 의 한 판 폭 — 같은 점수끼리면 20 × 0.5 = 10 이라 옛 ±10 과 같다(이미 쌓인 점수와 이어진다 · 플랜 5-1)
constexpr int32_t kRatingDuelK = 20;
// ★ 11-07 1:1 비김에 둘 다 빼는 값 — 둘이 가만히 있다가 비기는 판을 손해로 만든다(사용자 값 · 플랜 1절). 비긴 판마다 점수 합이 10 줄지만
//   순위는 「나보다 높은 사람 수」라 순위엔 영향 없다
constexpr int32_t kRatingDrawPenalty = 5;

// 판이 어느 갈래로 셈해졌나 — 시험이 갈래 전부를 개수로 본다
enum class RatingCase : uint8_t {
    NotRanked,   // 일반 판 — 아무도 반영 안 함
    Aborted,     // 중단 — 나감 −10 · 남은 사람 +10(★ 10-31 · 나간 사람이 없으면 모두 0)
    NoWinner,    // 승자 없음(none) · 비김(draw) — 끝까지 0 · 나감 −10
    Team,        // 팀 승리
    Player,      // 각자 판 승리 — 점수 순위
    Duel,        // ★ 11-07 1:1(관전자 빼고 둘 · 둘 다 계정) — Elo(판 전 점수)
    kCount
};
const char* RatingCaseName(RatingCase c);

struct RatingSeat {
    bool rated = false;   // false = 반영 안 함(DB `rating_delta` NULL · `ratings` 안 건드림)
    int32_t delta = 0;
};

struct RatingResult {
    RatingCase kind = RatingCase::kCount;
    std::vector<RatingSeat> seats;   // `rec.players` 와 같은 차례 · 같은 수
};

// `before` = 자리별 판 전 점수(`rec.players` 와 같은 차례 · 계정 없는 자리 · 모자란 칸은 `kRatingStart`) — 1:1 Elo 만 읽는다
RatingResult RatingDeltas(const MatchRecord& rec, const std::vector<int32_t>& before);

}  // namespace nox
