// AccountStore.cpp — 계정 저장소 메모리 가짜(시험). 경계 설명은 `AccountStore.h`.
#include "AccountStore.h"

namespace nox::auth {

FindResult CMemoryAccountStore::Find(const std::string& name, AccountRow& out) {
    if (fail_) return FindResult::Failed;
    const auto it = rows_.find(name);
    if (it == rows_.end()) return FindResult::NotFound;
    out = it->second.row;
    return FindResult::Found;
}

InsertResult CMemoryAccountStore::Insert(const std::string& name, const PasswordRecord& pw, uint32_t /*nowUnix*/, uint64_t& id) {
    if (fail_) return InsertResult::Failed;
    const auto it = rows_.find(name);
    if (it != rows_.end()) {
        if (it->second.row.hasPassword) return InsertResult::Duplicate;
        it->second.row.hasPassword = true;
        it->second.row.password = pw;
        id = it->second.row.id;
        return InsertResult::Claimed;
    }
    Row r;
    r.row.id = next_++;
    r.row.hasPassword = true;
    r.row.password = pw;
    id = r.row.id;
    rows_.emplace(name, r);
    return InsertResult::Created;
}

bool CMemoryAccountStore::TouchLogin(uint64_t id, uint32_t nowUnix) {
    if (fail_) return false;
    for (auto& kv : rows_) {
        if (kv.second.row.id == id) {
            kv.second.lastLogin = nowUnix;
            return true;
        }
    }
    return false;
}

uint64_t CMemoryAccountStore::AddLegacy(const std::string& name) {
    Row r;
    r.row.id = next_++;
    rows_.emplace(name, r);
    return r.row.id;
}

uint32_t CMemoryAccountStore::LastLogin(uint64_t id) const {
    for (const auto& kv : rows_)
        if (kv.second.row.id == id) return kv.second.lastLogin;
    return 0;
}

}  // namespace nox::auth
