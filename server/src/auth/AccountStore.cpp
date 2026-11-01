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

// ★ 런처 P1 — 열쇠(MySQL 쪽과 같은 뜻 — `MySqlAccountStore.cpp`)
bool CMemoryAccountStore::InsertKey(uint64_t accountId, const LoginKeyHash& h, const std::string& device, uint32_t ttlSec, uint32_t nowUnix) {
    if (fail_) return false;
    for (auto it = keys_.begin(); it != keys_.end();) {   // 그 계정의 만료된 열쇠 지움
        if (it->second.accountId == accountId && it->second.expiresUnix < nowUnix) it = keys_.erase(it);
        else ++it;
    }
    KeyRow k;
    k.accountId = accountId;
    k.device = device;
    k.ttlSec = ttlSec;
    k.expiresUnix = nowUnix + ttlSec;
    k.lastUsed = nowUnix;
    return keys_.emplace(h, k).second;   // 같은 지문이 이미 있으면 실패(32바이트 난수라 실제로는 없음 — 표의 유일 색인과 같은 뜻)
}

FindResult CMemoryAccountStore::FindKey(const LoginKeyHash& h, LoginKeyRow& out) {
    if (fail_) return FindResult::Failed;
    const auto it = keys_.find(h);
    if (it == keys_.end()) return FindResult::NotFound;
    for (const auto& kv : rows_) {
        if (kv.second.row.id == it->second.accountId) {
            out = LoginKeyRow{};
            out.accountId = it->second.accountId;
            out.accountName = kv.first;
            out.ttlSec = it->second.ttlSec;
            out.expiresUnix = it->second.expiresUnix;
            return FindResult::Found;
        }
    }
    return FindResult::NotFound;   // 계정 행이 없음(표의 외래 키가 막는 꼴)
}

bool CMemoryAccountStore::TouchKey(const LoginKeyHash& h, uint32_t nowUnix, uint32_t expiresUnix) {
    if (fail_) return false;
    const auto it = keys_.find(h);
    if (it == keys_.end()) return false;
    it->second.lastUsed = nowUnix;
    it->second.expiresUnix = expiresUnix;
    return true;
}

bool CMemoryAccountStore::DeleteKey(const LoginKeyHash& h) {
    if (fail_) return false;
    keys_.erase(h);
    return true;
}

bool CMemoryAccountStore::HasDevice(const std::string& device) const {
    for (const auto& kv : keys_)
        if (kv.second.device == device) return true;
    return false;
}

}  // namespace nox::auth
