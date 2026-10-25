// LoginToken.h — 로그인 토큰(로그인 서버가 서명 · 로비가 혼자 검증). 설계 Docs/LOGIN_AUTH_DESIGN.md 「플랜을 짜며 고른 것」.
//
// 왜 — 정품 클라는 로비 로그인 묶음의 `apgar <암호> 0` 줄에 암호를 평문으로 싣는다. 로비가 암호를 받지 않게,
//   클라 DLL 이 로그인 서버(nox_auth · HTTPS)에 암호를 대고 받은 이 토큰을 `apgar` 자리에 바꿔 싣는다.
//   로비는 같은 키로 서명 · 만료 · 이름 · IP 를 보고 일회용 번호를 써 버린다(`CJoinTokenLedger` 다시 씀) — DB 를 묻지 않는다.
//   입장 토큰(`JoinToken.h`)과 짓는 법이 같고 키는 따로다(로그인 서버가 입장 토큰을 못 짓게 — 원칙 둘째).
//
// 바이너리 57바이트(작은 끝수)
//   [0] 판 1 · [1..8] 계정 번호 8 · [9..12] 클라 IPv4(a.b.c.d 차례) 4 · [13..16] 만료(유닉스 초) 4 · [17..24] 일회용 번호 8
//   [25..40] 계정 이름 16(0 채움) · [41..56] 서명 16 = HMAC-SHA256(키, [0..40]) 앞 16바이트
// 줄에 실을 때는 16진 114자(`LoginLine.h` — `apgar NXL1<16진> 0`)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "nox/JoinToken.h"

namespace nox {

constexpr uint8_t kLoginTokenVersion = 1;
constexpr size_t kLoginTokenBodySize = 41;
constexpr size_t kLoginTokenMacSize = 16;
constexpr size_t kLoginTokenSize = kLoginTokenBodySize + kLoginTokenMacSize;   // 57
constexpr size_t kLoginNameSize = 16;
constexpr uint32_t kLoginTokenLifetimeSec = 60;   // 로그인 서버가 만들 때 만료 = 지금 + 이것(DLL 이 받자마자 로비에 보냄)
constexpr uint32_t kLoginTokenMaxAheadSec = 120;  // 만료가 이보다 멀면 거절(시계가 크게 어긋났거나 위조)

struct LoginToken {
    uint8_t version = kLoginTokenVersion;
    uint64_t accountId = 0;
    uint32_t clientIp = 0;     // a.b.c.d → (a << 24) | (b << 16) | (c << 8) | d — 로그인 서버가 본 상대 주소
    uint32_t expiresAt = 0;    // 유닉스 초
    uint64_t nonce = 0;
    std::string name;          // 계정 이름 — 16바이트에서 자른다(이름 규칙이 9자라 늘 들어감 — `AccountRules.h`)
};

// 검증 결과 — 갈래마다 따로 센다(시험은 개수로 본다)
enum class LoginTokenCheck : uint8_t {
    Ok,
    BadSize,        // 57바이트가 아니다
    BadVersion,
    BadMac,         // 서명이 틀렸다(키가 다르거나 내용을 바꿨다)
    Expired,
    TooFarAhead,
    IpMismatch,     // 토큰의 IP ≠ 로비 연결 상대 IP
    NameMismatch,   // 토큰의 이름 ≠ 로그인 묶음의 `NICK`(남의 토큰을 제 이름으로 쓰기)
    kCount
};
const char* LoginTokenCheckName(LoginTokenCheck c);

// 로그인 서버 쪽 — 서명해 57바이트를 채운다
bool SignLoginToken(const JoinKey& key, const LoginToken& t, uint8_t out[kLoginTokenSize]);
// 로비 쪽 — 서명 · 판 · 만료 · IP · 이름을 본다(일회용은 `CJoinTokenLedger`)
LoginTokenCheck VerifyLoginToken(const JoinKey& key, const uint8_t* data, size_t len, uint32_t nowUnix,
                                 uint32_t fromIp, const std::string& nick, LoginToken& out);

}  // namespace nox
