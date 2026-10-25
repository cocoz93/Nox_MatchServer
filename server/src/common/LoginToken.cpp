// LoginToken.cpp — 로그인 토큰 서명 · 검증. 설계 Docs/LOGIN_AUTH_DESIGN.md · 짓는 법은 입장 토큰(`JoinToken.cpp`)과 같다.
#include "nox/LoginToken.h"

#include <cstring>

namespace nox {

const char* LoginTokenCheckName(LoginTokenCheck c) {
    switch (c) {
    case LoginTokenCheck::Ok: return "통과";
    case LoginTokenCheck::BadSize: return "크기";
    case LoginTokenCheck::BadVersion: return "판";
    case LoginTokenCheck::BadMac: return "서명";
    case LoginTokenCheck::Expired: return "만료";
    case LoginTokenCheck::TooFarAhead: return "만료가 너무 멂";
    case LoginTokenCheck::IpMismatch: return "IP 다름";
    case LoginTokenCheck::NameMismatch: return "이름 다름";
    case LoginTokenCheck::kCount: break;
    }
    return "?";
}

static void PutLE(uint8_t* p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

static uint64_t GetLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// 몸통 41바이트 — 서명은 이 바이트 위에 한다
static void PackBody(const LoginToken& t, uint8_t body[kLoginTokenBodySize]) {
    std::memset(body, 0, kLoginTokenBodySize);
    body[0] = t.version;
    PutLE(body + 1, t.accountId, 8);
    body[9] = static_cast<uint8_t>(t.clientIp >> 24);
    body[10] = static_cast<uint8_t>(t.clientIp >> 16);
    body[11] = static_cast<uint8_t>(t.clientIp >> 8);
    body[12] = static_cast<uint8_t>(t.clientIp);
    PutLE(body + 13, t.expiresAt, 4);
    PutLE(body + 17, t.nonce, 8);
    std::memcpy(body + 25, t.name.data(), t.name.size() < kLoginNameSize ? t.name.size() : kLoginNameSize);
}

static bool Mac(const JoinKey& key, const uint8_t body[kLoginTokenBodySize], uint8_t mac[kLoginTokenMacSize]) {
    uint8_t full[32];
    if (!HmacSha256(key.data(), key.size(), body, kLoginTokenBodySize, full)) return false;
    std::memcpy(mac, full, kLoginTokenMacSize);
    return true;
}

bool SignLoginToken(const JoinKey& key, const LoginToken& t, uint8_t out[kLoginTokenSize]) {
    PackBody(t, out);
    return Mac(key, out, out + kLoginTokenBodySize);
}

LoginTokenCheck VerifyLoginToken(const JoinKey& key, const uint8_t* data, size_t len, uint32_t nowUnix,
                                 uint32_t fromIp, const std::string& nick, LoginToken& out) {
    if (!data || len != kLoginTokenSize) return LoginTokenCheck::BadSize;
    uint8_t mac[kLoginTokenMacSize];
    if (!Mac(key, data, mac)) return LoginTokenCheck::BadMac;
    uint8_t diff = 0;   // 같은 시간에 끝나는 비교 — 어디서 틀렸는지 시간으로 새지 않게
    for (size_t i = 0; i < kLoginTokenMacSize; ++i) diff |= static_cast<uint8_t>(mac[i] ^ data[kLoginTokenBodySize + i]);
    if (diff != 0) return LoginTokenCheck::BadMac;

    out.version = data[0];
    out.accountId = GetLE(data + 1, 8);
    out.clientIp = (static_cast<uint32_t>(data[9]) << 24) | (static_cast<uint32_t>(data[10]) << 16) |
                   (static_cast<uint32_t>(data[11]) << 8) | data[12];
    out.expiresAt = static_cast<uint32_t>(GetLE(data + 13, 4));
    out.nonce = GetLE(data + 17, 8);
    const char* nm = reinterpret_cast<const char*>(data + 25);
    out.name.assign(nm, strnlen(nm, kLoginNameSize));

    if (out.version != kLoginTokenVersion) return LoginTokenCheck::BadVersion;
    if (nowUnix > out.expiresAt) return LoginTokenCheck::Expired;
    if (out.expiresAt - nowUnix > kLoginTokenMaxAheadSec) return LoginTokenCheck::TooFarAhead;
    if (out.clientIp != fromIp) return LoginTokenCheck::IpMismatch;
    if (out.name != nick) return LoginTokenCheck::NameMismatch;   // 바이트 그대로 같음(로비 · DB 와 같은 잣대 — `LobbyStore.h:9`)
    return LoginTokenCheck::Ok;
}

}  // namespace nox
