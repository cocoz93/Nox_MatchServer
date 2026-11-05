// LoginThrottle.cpp — 로그인 시도 제한. 설명은 `LoginThrottle.h`.
#include "LoginThrottle.h"

namespace nox::auth {

uint32_t AccountLockSec(uint32_t failCount) {
    if (failCount < 5) return 0;
    if (failCount == 5) return 60;
    if (failCount == 6) return 300;
    return 900;
}

uint32_t AddressLockSec(uint32_t failCount) { return failCount < kAddressLimit ? 0 : kAddressLockSec; }

// 지난 칸(마지막 실패에서 1시간)은 지우고 0 · 잠겼으면 남은 초
template <class Map, class Key>
uint32_t CLoginThrottle::Left(Map& m, const Key& k, uint32_t now) {
    const auto it = m.find(k);
    if (it == m.end()) return 0;
    if (now - it->second.lastFail >= kThrottleResetSec) {
        m.erase(it);
        return 0;
    }
    return now < it->second.lockedUntil ? it->second.lockedUntil - now : 0;
}

void CLoginThrottle::Count(Slot& s, uint32_t now, uint32_t (*lockSec)(uint32_t)) {
    if (now < s.lockedUntil) return;   // 잠긴 동안 돌아온 요청은 안 센다(`LoginThrottle.h`)
    if (s.fails != 0 && now - s.lastFail >= kThrottleResetSec) s.fails = 0;
    ++s.fails;
    s.lastFail = now;
    const uint32_t lock = lockSec(s.fails);
    if (lock != 0) s.lockedUntil = now + lock;
}

void CLoginThrottle::Prune(uint32_t now) {
    if (now - lastPrune_ < kThrottlePruneEverySec) return;
    lastPrune_ = now;
    for (auto it = addresses_.begin(); it != addresses_.end();) {
        if (now - it->second.lastFail >= kThrottleResetSec) it = addresses_.erase(it);
        else ++it;
    }
    for (auto it = accounts_.begin(); it != accounts_.end();) {
        if (now - it->second.lastFail >= kThrottleResetSec) it = accounts_.erase(it);
        else ++it;
    }
}

uint32_t CLoginThrottle::RetryAfterSec(const std::string& name, uint32_t ip, uint32_t now, ThrottleScope* scope) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t byAccount = Left(accounts_, name, now);
    const uint32_t byAddress = Left(addresses_, ip, now);
    if (byAccount == 0 && byAddress == 0) return 0;
    if (scope) *scope = byAccount >= byAddress ? ThrottleScope::Account : ThrottleScope::Address;
    return byAccount >= byAddress ? byAccount : byAddress;
}

uint32_t CLoginThrottle::AddressRetryAfterSec(uint32_t ip, uint32_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    return Left(addresses_, ip, now);
}

void CLoginThrottle::OnFail(const std::string* name, uint32_t ip, uint32_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    Prune(now);
    Count(addresses_[ip], now, AddressLockSec);
    if (name) Count(accounts_[*name], now, AccountLockSec);
}

void CLoginThrottle::OnSuccess(const std::string& name, uint32_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    Prune(now);
    accounts_.erase(name);
}

size_t CLoginThrottle::AccountSlots() const {
    std::lock_guard<std::mutex> lk(mu_);
    return accounts_.size();
}

size_t CLoginThrottle::AddressSlots() const {
    std::lock_guard<std::mutex> lk(mu_);
    return addresses_.size();
}

}  // namespace nox::auth
