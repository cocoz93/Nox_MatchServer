// JoinToken.h — 입장 토큰(로비가 서명 · 게임 서버가 혼자 검증). 설계 Docs/TOKEN_DESIGN.md.
//
// 로비는 방 참가(`JOINGAME`)를 받을 때 토큰을 만들어 키로 서명하고, 클라의 DLL이 그것을 접속 시도·CONNECT 꼬리에 싣는다.
// 게임 서버는 같은 키로 서명·만료·보낸 IP를 보고, CONNECT 때 일회용 번호를 써 버린다(`CJoinTokenLedger`).
// 로비(`nox_lobby_core`)와 게임 서버(`nox_proto`)가 같이 쓰므로 둘 다에 기대지 않는 `nox_common` 에 둔다.
//
// 바이너리 51바이트(작은 끝수)
//   [0] 판 1 · [1..2] 방 2 · [3..6] 클라 IPv4(a.b.c.d 차례) 4 · [7..10] 만료(유닉스 초) 4 · [11..18] 일회용 번호 8
//   [19..34] 로비 이름 16(0 채움) · [35..50] 서명 16 = HMAC-SHA256(키, [0..34]) 앞 16바이트
// UDP 꼬리 = 토큰 51 + `NXT1` 4 = 55바이트(패킷 맨 끝 — 녹스 패킷엔 길이 칸이 없어 쪼개기 전에 뗀다)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace nox {

constexpr uint8_t kJoinTokenVersion = 1;
constexpr size_t kJoinTokenBodySize = 35;
constexpr size_t kJoinTokenMacSize = 16;
constexpr size_t kJoinTokenSize = kJoinTokenBodySize + kJoinTokenMacSize;   // 51
constexpr size_t kJoinAccountSize = 16;
constexpr char kJoinTrailerMagic[4] = { 'N', 'X', 'T', '1' };
constexpr size_t kJoinTrailerSize = kJoinTokenSize + sizeof(kJoinTrailerMagic);   // 55
constexpr uint32_t kJoinTokenLifetimeSec = 30;     // 로비가 만들 때 만료 = 지금 + 이것
constexpr uint32_t kJoinTokenMaxAheadSec = 120;    // 만료가 이보다 멀면 거절(시계가 크게 어긋났거나 위조)

using JoinKey = std::array<uint8_t, 32>;

struct JoinToken {
    uint8_t version = kJoinTokenVersion;
    uint16_t roomId = 0;
    uint32_t clientIp = 0;     // a.b.c.d → (a << 24) | (b << 16) | (c << 8) | d
    uint32_t expiresAt = 0;    // 유닉스 초
    uint64_t nonce = 0;
    std::string account;       // 로비 이름 — 16바이트에서 자른다
};

// 검증 결과 — 갈래마다 따로 센다(시험은 개수로 본다)
enum class JoinTokenCheck : uint8_t {
    Ok,
    BadSize,        // 51바이트가 아니다
    BadVersion,
    BadMac,         // 서명이 틀렸다(키가 다르거나 내용을 바꿨다)
    Expired,        // 만료가 지났다
    TooFarAhead,    // 만료가 너무 멀다
    IpMismatch,     // 토큰의 IP ≠ 패킷을 보낸 IP
    kCount
};
const char* JoinTokenCheckName(JoinTokenCheck c);

// HMAC-SHA256 — 윈도 기본 암호 API(BCrypt). 실패하면 false
bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t out[32]);
// 암호 수준 난수(BCryptGenRandom)
bool RandomBytes(uint8_t* out, size_t len);

bool ParseHex(const std::string& hex, uint8_t* out, size_t outLen);   // 정확히 outLen*2 글자만 받는다
std::string ToHex(const uint8_t* data, size_t len);
bool ParseJoinKey(const std::string& hex, JoinKey& out);              // 16진 64자
bool ParseIpv4(const std::string& text, uint32_t& out);              // "a.b.c.d"
std::string Ipv4ToString(uint32_t ip);

// 로비 쪽 — 서명해 51바이트를 채운다
bool SignJoinToken(const JoinKey& key, const JoinToken& t, uint8_t out[kJoinTokenSize]);
// 게임 서버 쪽 — 서명·판·만료·IP 를 본다(일회용은 `CJoinTokenLedger`)
JoinTokenCheck VerifyJoinToken(const JoinKey& key, const uint8_t* data, size_t len,
                               uint32_t nowUnix, uint32_t fromIp, JoinToken& out);

// 패킷 끝의 꼬리(`… 토큰 51 · NXT1`)를 찾는다. 있으면 토큰 자리와 뗀 뒤의 길이를 돌려준다
bool FindJoinTrailer(const uint8_t* packet, size_t len, size_t minKeep, const uint8_t*& token, size_t& keptLen);

// 일회용 번호 장부 — 쓴 번호를 만료까지 기억한다. 같은 번호를 두 번 쓰면 false
class CJoinTokenLedger {
public:
    bool Consume(uint64_t nonce, uint32_t expiresAt, uint32_t nowUnix);
    void Prune(uint32_t nowUnix);   // 만료 지난 번호를 잊는다
    size_t Size() const { return _used.size(); }

private:
    std::unordered_map<uint64_t, uint32_t> _used;   // 번호 → 만료
};

uint32_t UnixNow();

}  // namespace nox
