// MatchHistory.cpp — 대전 기록의 순수 계산. 설명은 `MatchHistory.h` 머리.
#include "nox/MatchHistory.h"

#include <algorithm>

namespace nox::wol {

namespace {

// 줄의 칸은 공백으로 나뉜다 — 맵 이름에 공백 · 제어 바이트가 있으면 밑줄로 · 없으면 `-` · DLL 칸 폭까지
std::string MapToken(const std::string& map) {
    std::string s = map.substr(0, kMatchHistoryMapChars);
    for (char& c : s)
        if (static_cast<unsigned char>(c) <= 0x20) c = '_';
    return s.empty() ? std::string("-") : s;
}

std::string ResultToken(const MatchHistoryRow& r) {
    switch (MatchOutcomeOf(r)) {
        case MatchOutcome::Win: return "W";
        case MatchOutcome::Loss: return "L";
        case MatchOutcome::Place: return std::to_string(std::max<uint32_t>(r.higher + 1, 2));   // 1등과 점수가 같아도 승자가 아니면 2등
        case MatchOutcome::Left: return "Q";
        case MatchOutcome::Aborted: return "A";
        case MatchOutcome::Draw:
        case MatchOutcome::kCount: break;
    }
    return "D";
}

}  // namespace

MatchOutcome MatchOutcomeOf(const MatchHistoryRow& r) {
    if (r.left) return MatchOutcome::Left;
    if (r.endReason == "aborted") return MatchOutcome::Aborted;
    if (r.winnerKind == "team") {
        if (!r.hasWinnerId || r.team == 0) return MatchOutcome::Draw;
        return r.team == r.winnerId ? MatchOutcome::Win : MatchOutcome::Loss;
    }
    if (r.winnerKind == "player") {
        if (!r.hasWinnerId) return MatchOutcome::Draw;
        if (r.netCode == r.winnerId) return MatchOutcome::Win;
        return r.seats <= 2 ? MatchOutcome::Loss : MatchOutcome::Place;
    }
    return MatchOutcome::Draw;   // draw · none
}

void MatchHistory::Add(const MatchHistoryRow& row) {
    ++total;
    uint32_t& wins = row.ranked ? rankedWins : normalWins;
    uint32_t& losses = row.ranked ? rankedLosses : normalLosses;
    switch (MatchOutcomeOf(row)) {
        case MatchOutcome::Win: ++wins; break;
        case MatchOutcome::Loss:
        case MatchOutcome::Place:
        case MatchOutcome::Left: ++losses; break;
        case MatchOutcome::Aborted:
        case MatchOutcome::Draw:
        case MatchOutcome::kCount: break;
    }
    if (rows.size() < kMatchHistoryMaxRows) rows.push_back(row);
}

std::vector<std::string> MatchHistoryLines(const std::string& name, bool ratingFound, int32_t rating, uint32_t rank, const MatchHistory& h) {
    std::vector<std::string> out;
    out.reserve(h.rows.size() + 2);
    out.push_back("NOXRANK RECSUM " + std::string(ratingFound ? "1 " : "0 ") + std::to_string(rating) + " " + std::to_string(rank) + " " +
                  std::to_string(h.rankedWins) + " " + std::to_string(h.rankedLosses) + " " + std::to_string(h.normalWins) + " " +
                  std::to_string(h.normalLosses) + " " + name);
    int64_t after = rating;   // 최신 판이 끝난 뒤 값 = 지금 점수 · 한 판 내려갈 때마다 그 판의 증감을 뺀다
    for (const MatchHistoryRow& r : h.rows) {
        std::string afterText = "-", deltaText = "-";
        if (r.ranked && r.hasDelta && ratingFound) {
            afterText = std::to_string(after);
            deltaText = (r.delta > 0 ? "+" : "") + std::to_string(r.delta);
            after -= r.delta;
        }
        out.push_back("NOXRANK RECROW " + ResultToken(r) + (r.ranked ? " 1 " : " 0 ") + afterText + " " + deltaText + " " + MapToken(r.map) + " " +
                      (r.hasKills ? std::to_string(r.kills) : std::string("-")) + " " + (r.hasDeaths ? std::to_string(r.deaths) : std::string("-")) +
                      " " + std::to_string(r.frames / kMatchHistoryFramesPerSec) + " " + std::to_string(r.end));
    }
    out.push_back("NOXRANK RECEND " + std::to_string(h.rows.size()));
    return out;
}

}  // namespace nox::wol
