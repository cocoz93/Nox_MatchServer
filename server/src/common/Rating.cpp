// Rating.cpp — 판 하나 → 자리별 점수 증감(규칙은 `Rating.h` 머리 주석).
#include "nox/Rating.h"

#include <cmath>

namespace nox {

const char* RatingCaseName(RatingCase c) {
    switch (c) {
        case RatingCase::NotRanked: return "not-ranked";
        case RatingCase::Aborted:   return "aborted";
        case RatingCase::NoWinner:  return "no-winner";
        case RatingCase::Team:      return "team";
        case RatingCase::Player:    return "player";
        case RatingCase::kCount:    break;
    }
    return "?";
}

namespace {

// 판의 갈래 — 중단이 승자 종류보다 먼저(중단 판은 승자가 없다 · `RoomManager.h:69` `aborted`)
RatingCase CaseOf(const MatchRecord& rec) {
    if (!rec.ranked) return RatingCase::NotRanked;
    if (rec.endReason == "aborted") return RatingCase::Aborted;
    if (rec.winnerKind == "team" && rec.hasWinnerId) return RatingCase::Team;
    if (rec.winnerKind == "player" && rec.hasWinnerId) return RatingCase::Player;
    return RatingCase::NoWinner;   // none · draw(읽개가 낱말을 넷으로 거른다 — `MatchRecord.cpp`)
}

// 점수가 바뀔 수 있는 사람인가 — 관전자 · 계정 없는 사람은 늘 반영 안 함
bool Eligible(const MatchRecordPlayer& p) { return p.hasAccount && !p.observer; }

}  // namespace

RatingResult RatingDeltas(const MatchRecord& rec) {
    RatingResult r;
    r.kind = CaseOf(rec);
    r.seats.assign(rec.players.size(), RatingSeat{});
    switch (r.kind) {
        case RatingCase::NotRanked:
        case RatingCase::kCount:
            return r;
        case RatingCase::Aborted: {
            // ★ 10-31 — 중단은 판 도중 2명 밑으로 떨어질 때뿐(`RoomTick.cpp` `CheckMatchHeadcount`)이라 1대1 에선 나가면 늘 중단 —
            //   옛 「모두 0」이면 지기 직전에 나가도 손해가 없었다(10-31 사람 판). 나간 사람(계정 없어도) 이 있으면 나감 −10 · 남은 사람 +10
            bool anyLeft = false;
            for (const MatchRecordPlayer& p : rec.players) anyLeft = anyLeft || p.left;
            for (size_t i = 0; i < rec.players.size(); ++i) {
                const MatchRecordPlayer& p = rec.players[i];
                if (!Eligible(p)) continue;
                r.seats[i] = {true, p.left ? -kRatingStep : anyLeft ? kRatingStep : 0};
            }
            return r;
        }
        case RatingCase::NoWinner:
            for (size_t i = 0; i < rec.players.size(); ++i)
                if (Eligible(rec.players[i])) r.seats[i] = {true, rec.players[i].left ? -kRatingStep : 0};
            return r;
        case RatingCase::Team:
            for (size_t i = 0; i < rec.players.size(); ++i) {
                const MatchRecordPlayer& p = rec.players[i];
                if (!Eligible(p)) continue;
                if (p.left) r.seats[i] = {true, -kRatingStep};
                else if (p.team == 0) continue;   // 팀 판의 무소속 — 이긴 쪽도 진 쪽도 아님
                else r.seats[i] = {true, p.team == rec.winnerId ? kRatingStep : -kRatingStep};
            }
            return r;
        case RatingCase::Player: {
            // 순위는 끝까지 한 사람(관전자 빼고) 모두로 — 계정 없는 사람도 자리를 차지한다
            std::vector<int32_t> scores;
            for (const MatchRecordPlayer& p : rec.players)
                if (!p.left && !p.observer) scores.push_back(p.score);
            const size_t k = scores.size();
            for (size_t i = 0; i < rec.players.size(); ++i) {
                const MatchRecordPlayer& p = rec.players[i];
                if (!Eligible(p)) continue;
                if (p.left) {
                    r.seats[i] = {true, -kRatingStep};
                    continue;
                }
                if (k < 2) {   // 상대가 없는 판(최소 인원 검사가 막지만 — `RoomManager.h` kMinPlayers)
                    r.seats[i] = {true, 0};
                    continue;
                }
                // 같은 점수는 순위 평균(0 부터) — 나보다 높은 수 + (나와 같은 수 − 1) / 2
                size_t higher = 0, same = 0;
                for (int32_t s : scores) {
                    if (s > p.score) ++higher;
                    else if (s == p.score) ++same;
                }
                const double rank = static_cast<double>(higher) + static_cast<double>(same - 1) / 2.0;
                const double d = kRatingStep - 2.0 * kRatingStep * rank / static_cast<double>(k - 1);
                r.seats[i] = {true, static_cast<int32_t>(std::lround(d))};
            }
            return r;
        }
    }
    return r;
}

}  // namespace nox
