// AccountRules.h — 계정 이름 · 암호 규칙과 암호 해시(PBKDF2). 설계 Docs/LOGIN_AUTH_DESIGN.md 정할 것 d · e · f · g.
//
// 왜 이 규칙인가 — 정품 로그인 창 칸이 정한다(`wolapi.wnd` 원본 기준 · 화면 미확인)
//   · 이름 = 영문 · 숫자 1~9자(d) — 이름 칸에 9자까지 쳐지고 한글은 못 칠 것으로 봄. 그래서 바이트 = ASCII 라 949 바꾸기가 없다
//   · 암호 = 4~16자 · 영문 + 숫자 꼭 · 특수문자는 써도 됨 · 빈칸 금지(e · g) — 로그인 줄을 빈칸으로 나누므로.
//     16자는 DLL 이 암호 칸 한도를 늘려서(e) — 정품 칸 그대로면 8자
// 해시 = PBKDF2-HMAC-SHA256(f) · 소금 16 · 결과 32 · 반복 수는 행마다 적어 둔다(나중에 수를 올려도 옛 행을 그대로 확인)
// 로그인 서버(nox_auth)가 쓴다. 로비는 이름 규칙을 안 본다 — 토큰의 이름이 이미 이 규칙을 지났다
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nox {

constexpr size_t kAccountNameMax = 9;
constexpr size_t kPasswordMin = 4;
constexpr size_t kPasswordMax = 16;
constexpr uint32_t kPbkdf2Iterations = 600000;   // f — OWASP 암호 저장 권고(기억 · 조각 2 에서 이 PC 시간을 잼)
constexpr uint32_t kPbkdf2IterationsMax = 10000000;   // 저장된 행의 반복 수가 이보다 크면 확인을 안 함(틀린 행 하나가 일꾼을 오래 잡지 않게)
constexpr size_t kPasswordSaltSize = 16;
constexpr size_t kPasswordHashSize = 32;

enum class NameCheck : uint8_t {
    Ok,
    Empty,
    TooLong,   // 9자 넘음
    BadChar,   // 영문 · 숫자 밖(한글 · 빈칸 · 특수문자)
    kCount
};
const char* NameCheckName(NameCheck c);
NameCheck CheckAccountName(const std::string& name);

enum class PasswordCheck : uint8_t {
    Ok,
    TooShort,
    TooLong,
    HasSpace,   // 빈칸(로그인 줄을 빈칸으로 나눔)
    BadChar,    // 찍히는 ASCII 밖(한글 · 제어 글자)
    NoLetter,
    NoDigit,
    kCount
};
const char* PasswordCheckName(PasswordCheck c);
PasswordCheck CheckPassword(const std::string& password);

struct PasswordRecord {
    std::array<uint8_t, kPasswordSaltSize> salt{};
    std::array<uint8_t, kPasswordHashSize> hash{};
    uint32_t iterations = 0;
};

// PBKDF2-HMAC-SHA256(BCrypt `BCryptDeriveKeyPBKDF2`). 실패하면 false
bool Pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t* out, size_t outLen);
// 새 소금(암호 수준 난수) + 해시
bool MakePasswordRecord(const std::string& password, uint32_t iterations, PasswordRecord& out);
// 같은 시간에 끝나는 비교 · 반복 수가 0 이거나 한도를 넘으면 false
bool VerifyPassword(const std::string& password, const PasswordRecord& rec);

}  // namespace nox
