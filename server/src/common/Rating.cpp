// Rating.cpp — 판 하나 → 자리별 점수 증감(규칙은 `Rating.h` 머리 주석).
#include "nox/Rating.h"

#include <algorithm>
#include <cmath>

namespace nox {

const char* RatingCaseName(RatingCase c) {
    switch (c) {
        case RatingCase::NotRanked: return "not-ranked";
        case RatingCase::Aborted:   return "aborted";
        case RatingCase::NoWinner:  return "no-winner";
        case RatingCase::Team:      return "team";
        case RatingCase::Player:    return "player";
        case RatingCase::Duel:      return "duel";
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

// ★ 11-07 — 1:1 Elo 판이면 두 자리(명단 차례)를 채우고 참. 랭크 · 관전자 빼고 정확히 둘 · 둘 다 계정(플랜 2절)
bool DuelSeats(const MatchRecord& rec, size_t& a, size_t& b) {
    if (!rec.ranked) return false;
    size_t n = 0;
    for (size_t i = 0; i < rec.players.size(); ++i) {
        if (rec.players[i].observer) continue;
        if (!rec.players[i].hasAccount) return false;
        (n == 0 ? a : b) = i;
        if (++n > 2) return false;
    }
    return n == 2;
}

// 내가 이길 확률(Elo)
double Expected(int32_t mine, int32_t other) { return 1.0 / (1.0 + std::pow(10.0, (static_cast<double>(other) - mine) / 400.0)); }

// 1:1 — 이긴 쪽 몫 · 비김 · 둘 다 짐. 갈래는 나감이 먼저(하나만 나감 = 남은 쪽 이김 · 둘 다 = 둘 다 짐) → 나간 사람 없는 중단 0 → 승자 칸 → 비김
void DuelDeltas(const MatchRecord& rec, size_t a, size_t b, int32_t ra, int32_t rb, RatingResult& r) {
    const MatchRecordPlayer& pa = rec.players[a];
    const MatchRecordPlayer& pb = rec.players[b];
    const double ea = Expected(ra, rb);
    const double eb = 1.0 - ea;
    auto win = [&](bool aWins) {
        const double eWinner = aWins ? ea : eb;
        const int32_t w = std::max<int32_t>(1, static_cast<int32_t>(std::lround(kRatingDuelK * (1.0 - eWinner))));
        r.seats[a] = {true, aWins ? w : -w};
        r.seats[b] = {true, aWins ? -w : w};
    };
    if (pa.left && pb.left) {
        r.seats[a] = {true, -static_cast<int32_t>(std::lround(kRatingDuelK * ea))};
        r.seats[b] = {true, -static_cast<int32_t>(std::lround(kRatingDuelK * eb))};
        return;
    }
    if (pa.left != pb.left) return win(pb.left);
    if (rec.endReason == "aborted") {
        r.seats[a] = {true, 0};
        r.seats[b] = {true, 0};
        return;
    }
    // 승자 칸 — 둘 중 정확히 하나만 맞으면 그 사람. 둘 다 · 아무도 아니면 비김으로 셈. 전적(`MatchOutcomeOf`)은 그런 판에서 둘을 같은 결과
    //   (둘 다 승 · 둘 다 짐 · 둘 다 비김)로 보인다 — 1:1 랭크에선 안 생길 꼴로 봄(승자는 둘 중 하나 · 팀 판이면 서로 다른 팀 — 확인 안 함)
    if (rec.hasWinnerId && (rec.winnerKind == "player" || rec.winnerKind == "team")) {
        const bool team = rec.winnerKind == "team";
        const bool aWon = team ? pa.team != 0 && pa.team == rec.winnerId : pa.netCode == rec.winnerId;
        const bool bWon = team ? pb.team != 0 && pb.team == rec.winnerId : pb.netCode == rec.winnerId;
        if (aWon != bWon) return win(aWon);
    }
    // 비김 · 승자 없음 — 낮은 쪽이 d 를 받고 둘 다 −5
    const bool aLow = ra <= rb;
    const double eLow = aLow ? ea : eb;
    const int32_t d = static_cast<int32_t>(std::lround(kRatingDuelK * (0.5 - eLow)));
    r.seats[a] = {true, (aLow ? d : -d) - kRatingDrawPenalty};
    r.seats[b] = {true, (aLow ? -d : d) - kRatingDrawPenalty};
}

RatingResult GroupDeltas(const MatchRecord& rec);   // 1:1 Elo 밖의 판(옛 규칙 — 아래)

}  // namespace

RatingResult RatingDeltas(const MatchRecord& rec, const std::vector<int32_t>& before) {
    size_t a = 0, b = 0;
    if (!DuelSeats(rec, a, b)) return GroupDeltas(rec);
    RatingResult r;
    r.kind = RatingCase::Duel;
    r.seats.assign(rec.players.size(), RatingSeat{});   // 관전자는 반영 안 함
    const auto at = [&before](size_t i) { return i < before.size() ? before[i] : kRatingStart; };
    DuelDeltas(rec, a, b, at(a), at(b), r);
    return r;
}

namespace {

// ★ 11-07 조각 4 — 옛 인자 하나 `RatingDeltas`(판 전 점수 없이 셈하는 길)를 바깥에서 못 부르게 이 파일 안으로
RatingResult GroupDeltas(const MatchRecord& rec) {
    RatingResult r;
    r.kind = CaseOf(rec);
    r.seats.assign(rec.players.size(), RatingSeat{});
    switch (r.kind) {
        case RatingCase::NotRanked:
        case RatingCase::Duel:   // `CaseOf` 는 안 돌려줌 — 1:1 은 `RatingDeltas` 가 먼저 가른다
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

}  // namespace

}  // namespace nox
