// RankPenalty.cpp — 머리 주석은 `RankPenalty.h`.
#include "nox/RankPenalty.h"
#include <algorithm>
namespace nox::wol {
const char* RankStrikeName(RankStrike why) {
    switch (why) {
        case RankStrike::AcceptTimeout: return "수락 한도";
        case RankStrike::Declined:      return "거절";
        case RankStrike::ListLeft:      return "목록으로 나감";
        case RankStrike::Closed:        return "끊김";
        case RankStrike::ReadyLeft:     return "방 기다리다 빠짐";
        case RankStrike::NoShow:        return "안 들어옴";
        case RankStrike::MatchLeft:     return "판 나감";
        case RankStrike::kCount:        break;
    }
    return "?";
}

PenaltyResult PenaltyStep(uint32_t count, uint32_t blockOverrideSec) {
    PenaltyResult r;
    r.count = count;
    if (count >= kRankPenaltyBanCount) {
        r.kind = PenaltyResult::Kind::Ban;
        return r;
    }
    if (count <= 1) return r;   // 경고
    r.kind = PenaltyResult::Kind::Block;
    r.blockSec = blockOverrideSec > 0 ? blockOverrideSec : count == 2 ? 60 : count == 3 ? 5 * 60 : 15 * 60;
    return r;
}

PenaltyResult CRankPenalty::Strike(const std::string& name, RankStrike why, uint32_t now) {
    ++struck_[static_cast<size_t>(why)];
    Entry& e = names_[name];
    if (e.count > 0 && now >= e.resetAt) e = Entry{};   // 30분이 지났는데 아직 안 지운 칸
    ++e.count;
    const PenaltyResult r = PenaltyStep(e.count, blockOverrideSec_);
    switch (r.kind) {
        case PenaltyResult::Kind::Warn:
            e.warnPending = true;
            e.resetAt = now + kRankPenaltyResetSec;
            break;
        case PenaltyResult::Kind::Block:
            e.warnPending = false;   // 막힘 안내가 대신한다
            e.blockUntil = std::max(e.blockUntil, now + r.blockSec);   // 막힌 동안 들어온 위반(판 결과는 늦게 온다)이 남은 시간을 줄이지 않게
            e.resetAt = e.blockUntil + kRankPenaltyResetSec;
            break;
        case PenaltyResult::Kind::Ban:
            names_.erase(name);   // 정지를 넣는 순간 0 — 풀린 사람은 1회 경고부터(사용자 11-05)
            break;
    }
    return r;
}

uint32_t CRankPenalty::BlockedForSec(const std::string& name, uint32_t now) const {
    const auto it = names_.find(name);
    if (it == names_.end() || it->second.blockUntil <= now) return 0;
    return it->second.blockUntil - now;
}

void CRankPenalty::Clear(const std::string& name) { names_.erase(name); }

bool CRankPenalty::TakeWarnPending(const std::string& name, uint32_t now) {
    const auto it = names_.find(name);
    if (it == names_.end() || !it->second.warnPending || now >= it->second.resetAt) return false;
    it->second.warnPending = false;
    return true;
}

uint32_t CRankPenalty::CountOf(const std::string& name, uint32_t now) const {
    const auto it = names_.find(name);
    return it == names_.end() || now >= it->second.resetAt ? 0 : it->second.count;
}

void CRankPenalty::Sweep(uint32_t now) {
    if (now < nextSweep_) return;
    nextSweep_ = now + kRankPenaltySweepSec;
    for (auto it = names_.begin(); it != names_.end();) {
        if (now >= it->second.resetAt) it = names_.erase(it);
        else ++it;
    }
}
}  // namespace nox::wol
