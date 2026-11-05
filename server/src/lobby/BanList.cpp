// BanList.cpp — 머리 주석은 `BanList.h`.
#include "nox/BanList.h"

#include <algorithm>

namespace nox::wol {

std::vector<std::string> CBanList::Replace(const std::vector<BanEntry>& rows, uint32_t now) {
    std::map<std::string, uint32_t> next;
    for (const BanEntry& r : rows) {
        uint32_t& v = next[r.name];
        v = std::max(v, r.until);   // 같은 이름이 둘이면 늦은 끝 — 정지 여러 행의 규칙과 같다(플랜 5절)
    }
    std::vector<std::string> fresh;
    for (const auto& kv : next) {
        const bool wasAdded = added_.erase(kv.first) != 0;   // 이미 막은 이름 — 읽은 목록으로 넘어간다
        if (!wasAdded && read_.count(kv.first) == 0) fresh.push_back(kv.first);
    }
    read_.swap(next);
    for (auto it = added_.begin(); it != added_.end();) {
        if (it->second <= now) it = added_.erase(it);
        else ++it;
    }
    return fresh;
}

void CBanList::Add(const std::string& name, uint32_t until) {
    uint32_t& v = added_[name];
    v = std::max(v, until);
}

uint32_t CBanList::UntilOf(const std::string& name, uint32_t now) const {
    uint32_t until = 0;
    if (const auto r = read_.find(name); r != read_.end()) until = r->second;
    if (const auto a = added_.find(name); a != added_.end()) until = std::max(until, a->second);
    return until > now ? until : 0;   // 찾기 문장의 `until_unix > ?` 와 같은 경계
}

}  // namespace nox::wol
