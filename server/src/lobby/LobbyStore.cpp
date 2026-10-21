// LobbyStore.cpp — 로비 저장소의 메모리 가짜. 설명은 `LobbyStore.h` 머리.
#include "nox/LobbyStore.h"

#include <utility>

namespace nox::wol {

void CMemoryLobbyStore::CheckLogin(uint32_t req, const std::string& name) {
    StoreAnswer a;
    a.kind = StoreAnswerKind::Login;
    a.req = req;
    if (failLogins_) {
        a.login = LoginResult::Failed;
        a.why = "메모리 저장소 — 로그인 실패 시험 스위치";
    } else {
        a.login = LoginResult::Ok;
        const auto it = accounts_.find(name);
        if (it == accounts_.end()) {
            a.accountId = nextAccount_++;
            a.created = true;
            accounts_.emplace(name, a.accountId);
        } else {
            a.accountId = it->second;
        }
    }
    pending_.push_back(std::move(a));
}

void CMemoryLobbyStore::SaveMatch(uint32_t req, const MatchSource& src, const MatchRecord& rec, const std::string& /*rawJson*/) {
    StoreAnswer a;
    a.kind = StoreAnswerKind::SaveMatch;
    a.req = req;
    if (failSaves_) {
        a.save = SaveResult::Failed;
        a.why = "메모리 저장소 — 판 저장 실패 시험 스위치";
    } else {
        const auto ins = matches_.emplace(MatchKey{src.ip, src.port, rec.match}, rec);
        a.save = ins.second ? SaveResult::Saved : SaveResult::Duplicate;
    }
    pending_.push_back(std::move(a));
}

void CMemoryLobbyStore::TakeAnswers(std::vector<StoreAnswer>& out) {
    for (StoreAnswer& a : pending_) out.push_back(std::move(a));
    pending_.clear();
}

uint64_t CMemoryLobbyStore::AccountId(const std::string& name) const {
    const auto it = accounts_.find(name);
    return it == accounts_.end() ? 0 : it->second;
}

const MatchRecord* CMemoryLobbyStore::FindMatch(const MatchSource& src, const std::string& match) const {
    const auto it = matches_.find(MatchKey{src.ip, src.port, match});
    return it == matches_.end() ? nullptr : &it->second;
}

}  // namespace nox::wol
