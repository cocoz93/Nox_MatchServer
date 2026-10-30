// Rating.h — 판 하나 → 자리별 점수 증감(레이팅 조각 1 — `Docs/RATING_DESIGN.md` 「정한 것」 1~6).
//
// 왜 — 점수 규칙을 한 함수에 모은다. DB 워커 · 메모리 가짜가 같은 함수로 반영하고(판 저장 트랜잭션 안), 시험은 갈래를 개수로 본다.
//   상대 세기를 보는 계산(Elo 꼴)으로 바꿀 때도 이 함수 하나만 바꾼다
//   · 시작 1000 · 한 판 ±10(고정 — 10-30 사용자)
//   · 랭크 판만(`MatchRecord::ranked`) — 일반 판은 아무도 반영 안 함
//   · 중단(`endReason` = aborted) = 반영하되 모두 0(나간 사람도)
//   · 비김 · 승자 없음 = 끝까지 한 사람 0 · 나간 사람 −10
//   · 팀 판(승자 = 팀) = 이긴 팀 +10 · 진 팀 −10 · 나간 사람 −10
//   · 각자 판(승자 = 사람) = 끝까지 한 사람(관전자 빼고)의 점수 순위로 1등 +10 ~ 꼴등 −10 고르게(같은 점수는 순위 평균 · 반올림) · 나간 사람 −10
//   · 반영 안 함(증감 없음 — DB 칸 NULL) = 관전자 · 계정 없는 사람(로비 안 거침) · 팀 판의 무소속(팀 0)
//     계정 없는 사람도 각자 판 순위의 자리는 차지한다(상대이므로) — 점수만 안 바뀐다
#pragma once

#include <cstdint>
#include <vector>

#include "nox/MatchRecord.h"

namespace nox {

constexpr int32_t kRatingStart = 1000;   // `ratings` 행이 없는 사람의 점수
constexpr int32_t kRatingStep = 10;      // 한 판 최대 증감

// 판이 어느 갈래로 셈해졌나 — 시험이 갈래 전부를 개수로 본다
enum class RatingCase : uint8_t {
    NotRanked,   // 일반 판 — 아무도 반영 안 함
    Aborted,     // 중단 — 모두 0
    NoWinner,    // 승자 없음(none) · 비김(draw) — 끝까지 0 · 나감 −10
    Team,        // 팀 승리
    Player,      // 각자 판 승리 — 점수 순위
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

RatingResult RatingDeltas(const MatchRecord& rec);

}  // namespace nox
