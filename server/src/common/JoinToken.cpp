// JoinToken.cpp — 입장 토큰 서명·검증. 설계 Docs/TOKEN_DESIGN.md 「토큰 모양」.
#include "nox/JoinToken.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace nox {

const char* JoinTokenCheckName(JoinTokenCheck c) {
    switch (c) {
    case JoinTokenCheck::Ok: return "통과";
    case JoinTokenCheck::BadSize: return "크기";
    case JoinTokenCheck::BadVersion: return "판";
    case JoinTokenCheck::BadMac: return "서명";
    case JoinTokenCheck::Expired: return "만료";
    case JoinTokenCheck::TooFarAhead: return "만료가 너무 멂";
    case JoinTokenCheck::IpMismatch: return "IP 다름";
    case JoinTokenCheck::kCount: break;
    }
    return "?";
}

bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 32, 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool RandomBytes(uint8_t* out, size_t len) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

static int Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseHex(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; ++i) {
        const int hi = Nibble(hex[2 * i]), lo = Nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        s[2 * i] = kDigits[data[i] >> 4];
        s[2 * i + 1] = kDigits[data[i] & 0x0f];
    }
    return s;
}

bool ParseJoinKey(const std::string& hex, JoinKey& out) {
    return ParseHex(hex, out.data(), out.size());
}

bool ParseIpv4(const std::string& text, uint32_t& out) {
    uint32_t v = 0;
    size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
        unsigned n = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 3) {
            n = n * 10 + static_cast<unsigned>(text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || n > 255) return false;
        v = (v << 8) | n;
    }
    if (i != text.size()) return false;   // 꼬리 글자
    out = v;
    return true;
}

std::string Ipv4ToString(uint32_t ip) {
    char b[16];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return b;
}

static void PutLE(uint8_t* p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

static uint64_t GetLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// 몸통 35바이트 — 서명은 이 바이트 위에 한다
static void PackBody(const JoinToken& t, uint8_t body[kJoinTokenBodySize]) {
    std::memset(body, 0, kJoinTokenBodySize);
    body[0] = t.version;
    PutLE(body + 1, t.roomId, 2);
    body[3] = static_cast<uint8_t>(t.clientIp >> 24);
    body[4] = static_cast<uint8_t>(t.clientIp >> 16);
    body[5] = static_cast<uint8_t>(t.clientIp >> 8);
    body[6] = static_cast<uint8_t>(t.clientIp);
    PutLE(body + 7, t.expiresAt, 4);
    PutLE(body + 11, t.nonce, 8);
    std::memcpy(body + 19, t.account.data(), t.account.size() < kJoinAccountSize ? t.account.size() : kJoinAccountSize);
}

static bool Mac(const JoinKey& key, const uint8_t body[kJoinTokenBodySize], uint8_t mac[kJoinTokenMacSize]) {
    uint8_t full[32];
    if (!HmacSha256(key.data(), key.size(), body, kJoinTokenBodySize, full)) return false;
    std::memcpy(mac, full, kJoinTokenMacSize);
    return true;
}

bool SignJoinToken(const JoinKey& key, const JoinToken& t, uint8_t out[kJoinTokenSize]) {
    PackBody(t, out);
    return Mac(key, out, out + kJoinTokenBodySize);
}

JoinTokenCheck VerifyJoinToken(const JoinKey& key, const uint8_t* data, size_t len,
                               uint32_t nowUnix, uint32_t fromIp, JoinToken& out) {
    if (!data || len != kJoinTokenSize) return JoinTokenCheck::BadSize;
    uint8_t mac[kJoinTokenMacSize];
    if (!Mac(key, data, mac)) return JoinTokenCheck::BadMac;
    uint8_t diff = 0;   // 같은 시간에 끝나는 비교 — 어디서 틀렸는지 시간으로 새지 않게
    for (size_t i = 0; i < kJoinTokenMacSize; ++i) diff |= static_cast<uint8_t>(mac[i] ^ data[kJoinTokenBodySize + i]);
    if (diff != 0) return JoinTokenCheck::BadMac;

    out.version = data[0];
    out.roomId = static_cast<uint16_t>(GetLE(data + 1, 2));
    out.clientIp = (static_cast<uint32_t>(data[3]) << 24) | (static_cast<uint32_t>(data[4]) << 16) |
                   (static_cast<uint32_t>(data[5]) << 8) | data[6];
    out.expiresAt = static_cast<uint32_t>(GetLE(data + 7, 4));
    out.nonce = GetLE(data + 11, 8);
    const char* acc = reinterpret_cast<const char*>(data + 19);
    out.account.assign(acc, strnlen(acc, kJoinAccountSize));

    if (out.version != kJoinTokenVersion) return JoinTokenCheck::BadVersion;
    if (nowUnix > out.expiresAt) return JoinTokenCheck::Expired;
    if (out.expiresAt - nowUnix > kJoinTokenMaxAheadSec) return JoinTokenCheck::TooFarAhead;
    if (out.clientIp != fromIp) return JoinTokenCheck::IpMismatch;
    return JoinTokenCheck::Ok;
}

bool FindJoinTrailer(const uint8_t* packet, size_t len, size_t minKeep, const uint8_t*& token, size_t& keptLen) {
    if (!packet || len < minKeep + kJoinTrailerSize) return false;
    if (std::memcmp(packet + len - sizeof(kJoinTrailerMagic), kJoinTrailerMagic, sizeof(kJoinTrailerMagic)) != 0) return false;
    keptLen = len - kJoinTrailerSize;
    token = packet + keptLen;
    return true;
}

bool CJoinTokenLedger::Consume(uint64_t nonce, uint32_t expiresAt, uint32_t nowUnix) {
    if (_used.size() >= 4096) Prune(nowUnix);   // 만료 30초라 보통은 몇 개뿐 — 많아졌을 때만 쓸어 낸다
    return _used.emplace(nonce, expiresAt).second;
}

void CJoinTokenLedger::Prune(uint32_t nowUnix) {
    for (auto it = _used.begin(); it != _used.end();) {
        if (it->second < nowUnix) it = _used.erase(it);
        else ++it;
    }
}

uint32_t UnixNow() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace nox
