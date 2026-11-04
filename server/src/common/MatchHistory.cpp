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
    // 중단은 판 도중 2명 밑일 때뿐 — 상대가 나가 +10 을 받은 랭크 판(`RATING_DESIGN.md` 4′)은 승. 일반 판은 점수가 없어 중단 그대로
    if (r.endReason == "aborted") return r.ranked && r.hasDelta && r.delta > 0 ? MatchOutcome::Win : MatchOutcome::Aborted;
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

// 메모리 가짜의 줄과 DB 의 줄(`DbWorker.cpp` `kSelHistory`)이 같은 값이어야 한다 — 없는 칸은 0(DB 의 NULL 을 읽을 때와 같게)
MatchHistoryRow MatchRowOf(const MatchRecord& m, const RatingResult& rr, size_t seat) {
    const MatchRecordPlayer& p = m.players[seat];
    MatchHistoryRow r;
    r.ranked = m.ranked;
    r.map = m.map;
    r.end = m.end;
    r.frames = m.frames;
    r.endReason = m.endReason;
    r.winnerKind = m.winnerKind;
    r.hasWinnerId = m.hasWinnerId;
    r.winnerId = m.hasWinnerId ? m.winnerId : 0;
    r.left = p.left;
    r.netCode = p.netCode;
    r.team = p.left ? 0 : p.team;
    r.hasKills = !p.left && p.hasKills;
    r.kills = r.hasKills ? p.kills : 0;
    r.hasDeaths = !p.left;
    r.deaths = r.hasDeaths ? p.deaths : 0;
    r.hasDelta = rr.seats[seat].rated;
    r.delta = r.hasDelta ? rr.seats[seat].delta : 0;
    for (const MatchRecordPlayer& o : m.players) {
        if (o.observer) continue;
        ++r.seats;
        if (!p.left && !o.left && o.score > p.score) ++r.higher;
    }
    return r;
}

void MatchTotals::Add(const MatchHistoryRow& row) {
    ++games;
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
}

std::vector<std::string> MatchHistoryLines(const std::string& name, bool ratingFound, int32_t rating, uint32_t rank, const MatchHistory& h) {
    std::vector<std::string> out;
    out.reserve(h.rows.size() + 2);
    out.push_back("NOXRANK RECSUM " + std::string(ratingFound ? "1 " : "0 ") + std::to_string(rating) + " " + std::to_string(rank) + " " +
                  std::to_string(h.totals.rankedWins) + " " + std::to_string(h.totals.rankedLosses) + " " + std::to_string(h.totals.normalWins) + " " +
                  std::to_string(h.totals.normalLosses) + " " + name);
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
