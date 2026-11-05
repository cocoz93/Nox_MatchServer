// AccountStore.cpp — 계정 저장소 메모리 가짜(시험). 경계 설명은 `AccountStore.h`.
#include "AccountStore.h"

namespace nox::auth {

FindResult CMemoryAccountStore::Find(const std::string& name, AccountRow& out) {
    ++findCalls_;
    if (fail_) return FindResult::Failed;
    const auto it = rows_.find(name);
    if (it == rows_.end()) return FindResult::NotFound;
    out = it->second.row;
    return FindResult::Found;
}

InsertResult CMemoryAccountStore::Insert(const std::string& name, const PasswordRecord& pw, uint32_t /*nowUnix*/, uint64_t& id) {
    ++insertCalls_;
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

// ★ 11-05 플랜1 — 한 트랜잭션과 같은 뜻: 실패할 자리를 먼저 다 본 뒤에 바꾼다
bool CMemoryAccountStore::ChangePassword(uint64_t id, const PasswordRecord& pw, const LoginKeyHash& h, const std::string& device, uint32_t ttlSec, uint32_t nowUnix) {
    if (fail_ || failChange_) return false;
    Row* row = nullptr;
    for (auto& kv : rows_)
        if (kv.second.row.id == id) row = &kv.second;
    if (!row) return false;
    const auto dup = keys_.find(h);
    if (dup != keys_.end() && dup->second.accountId != id) return false;   // 남의 행과 같은 지문(유일 색인 실패 → 되돌림과 같은 뜻)
    row->row.hasPassword = true;
    row->row.password = pw;
    for (auto it = keys_.begin(); it != keys_.end();) {
        if (it->second.accountId == id) it = keys_.erase(it);
        else ++it;
    }
    KeyRow k;
    k.accountId = id;
    k.device = device;
    k.ttlSec = ttlSec;
    k.expiresUnix = nowUnix + ttlSec;
    k.lastUsed = nowUnix;
    keys_.emplace(h, k);
    return true;
}

bool CMemoryAccountStore::HasDevice(const std::string& device) const {
    for (const auto& kv : keys_)
        if (kv.second.device == device) return true;
    return false;
}

FindResult CMemoryAccountStore::FindActiveBan(uint64_t id, uint32_t nowUnix, BanRow& out) {
    if (fail_ || failBan_) return FindResult::Failed;
    const BanEntry* best = nullptr;
    for (const BanEntry& b : bans_) {
        if (b.accountId != id || b.liftedUnix != 0 || b.untilUnix <= nowUnix) continue;
        if (!best || b.untilUnix > best->untilUnix) best = &b;
    }
    if (!best) return FindResult::NotFound;
    out = BanRow{};
    out.untilUnix = best->untilUnix;
    out.reason = best->reason;
    return FindResult::Found;
}

bool CMemoryAccountStore::InsertBan(uint64_t id, uint32_t untilUnix, const std::string& reason, const std::string& source, uint32_t nowUnix) {
    if (fail_ || failBan_) return false;
    bool known = false;
    for (const auto& kv : rows_)
        known = known || kv.second.row.id == id;
    if (!known) return false;   // 표의 외래 키가 막는 꼴
    BanEntry b;
    b.accountId = id;
    b.untilUnix = untilUnix;
    b.reason = reason;
    b.source = source;
    b.createdUnix = nowUnix;
    bans_.push_back(std::move(b));
    return true;
}

bool CMemoryAccountStore::LiftBans(uint64_t id, uint32_t nowUnix, unsigned& lifted) {
    lifted = 0;
    if (fail_ || failBan_) return false;
    for (BanEntry& b : bans_) {
        if (b.accountId != id || b.liftedUnix != 0 || b.untilUnix <= nowUnix) continue;
        b.liftedUnix = nowUnix;
        ++lifted;
    }
    return true;
}

bool CMemoryAccountStore::ListActiveBans(uint32_t nowUnix, std::vector<BanListRow>& out) {
    out.clear();
    if (fail_ || failBan_) return false;
    for (const BanEntry& b : bans_) {
        if (b.liftedUnix != 0 || b.untilUnix <= nowUnix) continue;
        for (const auto& kv : rows_) {
            if (kv.second.row.id != b.accountId) continue;
            BanListRow r;
            r.name = kv.first;
            r.untilUnix = b.untilUnix;
            r.reason = b.reason;
            r.source = b.source;
            r.createdUnix = b.createdUnix;
            out.push_back(std::move(r));
        }
    }
    return true;
}

}  // namespace nox::auth
