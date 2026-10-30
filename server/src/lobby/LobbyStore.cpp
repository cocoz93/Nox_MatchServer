// LobbyStore.cpp — 로비 저장소의 메모리 가짜. 설명은 `LobbyStore.h` 머리.
#include "nox/LobbyStore.h"

#include <utility>

namespace nox::wol {

namespace {

// UTF-8 로 맞는 바이트열인가(겹친 꼴 · 대리 자리 · 4바이트 넘는 값은 틀림 — `MatchRecorder.cpp` `IsUtf8` 과 같은 잣대)
bool ValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

}  // namespace

bool MatchFitsStore(const MatchRecord& rec, const std::string& rawJson, std::string& why) {
    auto over = [&why](const std::string& v, size_t max, const char* col) {
        if (v.size() <= max) return false;
        why = std::string(col) + " " + std::to_string(v.size()) + "바이트(한도 " + std::to_string(max) + ")";
        return true;
    };
    if (over(rec.match, kMatchKeyBytes, "match_key") || over(rec.roomName, kRoomNameBytes, "room_name") || over(rec.map, kMapKeyBytes, "map_key") ||
        over(rec.mode, kModeBytes, "mode") || over(rec.endReason, kEndReasonBytes, "end_reason") ||
        over(rec.winnerKind, kWinnerKindBytes, "winner_kind") || over(rec.winnerAccount, kAccountNameBytes, "winner_name"))
        return false;
    if (rec.players.size() > kMaxMatchPlayers) {
        why = "사람 " + std::to_string(rec.players.size()) + "명(한도 " + std::to_string(kMaxMatchPlayers) + ")";
        return false;
    }
    for (const MatchRecordPlayer& p : rec.players)
        if (over(p.account, kAccountNameBytes, "match_players.name")) return false;
    if (!ValidUtf8(rawJson)) {   // raw_json 칸은 utf8mb4 — 쓰개는 UTF-8 아닌 바이트를 \u00XX 로 쓰므로 정상이면 늘 맞다
        why = "raw_json 이 UTF-8 아님";
        return false;
    }
    return true;
}

void CMemoryLobbyStore::SaveMatch(uint32_t req, const MatchSource& src, const MatchRecord& rec, const std::string& rawJson) {
    StoreAnswer a;
    a.kind = StoreAnswerKind::SaveMatch;
    a.req = req;
    if (!MatchFitsStore(rec, rawJson, a.why)) {   // ★ 7단계 조각 7 — DB 워커와 같은 잣대(칸 한도)
        a.save = SaveResult::Invalid;
    } else if (failSaves_) {
        a.save = SaveResult::Failed;
        a.why = "메모리 저장소 — 판 저장 실패 시험 스위치";
    } else {
        const auto ins = matches_.emplace(MatchKey{src.ip, src.port, rec.match}, rec);
        a.save = ins.second ? SaveResult::Saved : SaveResult::Duplicate;
        if (ins.second) {   // ★ 레이팅 조각 3 — 새 판만(겹친 판은 DB 처럼 안 고침) · 계정 표가 없어 이름 있는 사람 = 계정
            const RatingResult rr = RatingDeltas(rec);
            for (size_t i = 0; i < rec.players.size(); ++i) {
                if (!rr.seats[i].rated) continue;
                const auto it = ratings_.emplace(rec.players[i].account, kRatingStart).first;
                it->second += rr.seats[i].delta;
            }
        }
    }
    pending_.push_back(std::move(a));
}

// ★ 레이팅 조각 3 — 순위 = 나보다 점수 높은 사람 수 + 1(DB 워커와 같은 잣대)
void CMemoryLobbyStore::GetRating(uint32_t req, const std::string& account) {
    if (holdRatings_) return;
    StoreAnswer a;
    a.kind = StoreAnswerKind::Rating;
    a.req = req;
    if (failRatings_) {
        a.why = "메모리 저장소 — 점수 읽기 실패 시험 스위치";
    } else {
        a.ratingOk = true;
        a.rating = kRatingStart;
        const auto it = ratings_.find(account);
        if (it != ratings_.end()) {
            a.ratingFound = true;
            a.rating = it->second;
            a.rank = 1;
            for (const auto& kv : ratings_)
                if (kv.second > it->second) ++a.rank;
        }
    }
    pending_.push_back(std::move(a));
}

bool CMemoryLobbyStore::FindRating(const std::string& account, int32_t& rating) const {
    const auto it = ratings_.find(account);
    if (it == ratings_.end()) return false;
    rating = it->second;
    return true;
}

void CMemoryLobbyStore::TakeAnswers(std::vector<StoreAnswer>& out) {
    for (StoreAnswer& a : pending_) out.push_back(std::move(a));
    pending_.clear();
}

const MatchRecord* CMemoryLobbyStore::FindMatch(const MatchSource& src, const std::string& match) const {
    const auto it = matches_.find(MatchKey{src.ip, src.port, match});
    return it == matches_.end() ? nullptr : &it->second;
}

}  // namespace nox::wol
