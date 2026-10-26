// AccountRules.cpp — 계정 이름 · 암호 규칙과 PBKDF2. 설계 Docs/LOGIN_AUTH_DESIGN.md 정할 것 d · e · f · g.
#include "nox/AccountRules.h"

#include "nox/JoinToken.h"   // RandomBytes

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace nox {

const char* NameCheckName(NameCheck c) {
    switch (c) {
    case NameCheck::Ok: return "통과";
    case NameCheck::Empty: return "빈 이름";
    case NameCheck::TooLong: return "너무 김";
    case NameCheck::BadChar: return "영문 · 숫자 밖";
    case NameCheck::kCount: break;
    }
    return "?";
}

static bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

NameCheck CheckAccountName(const std::string& name) {
    if (name.empty()) return NameCheck::Empty;
    if (name.size() > kAccountNameMax) return NameCheck::TooLong;
    for (const char c : name)
        if (!IsLetter(c) && !IsDigit(c)) return NameCheck::BadChar;
    return NameCheck::Ok;
}

const char* PasswordCheckName(PasswordCheck c) {
    switch (c) {
    case PasswordCheck::Ok: return "통과";
    case PasswordCheck::TooShort: return "너무 짧음";
    case PasswordCheck::TooLong: return "너무 김";
    case PasswordCheck::HasSpace: return "빈칸";
    case PasswordCheck::BadChar: return "쓸 수 없는 글자";
    case PasswordCheck::NoLetter: return "영문 없음";
    case PasswordCheck::NoDigit: return "숫자 없음";
    case PasswordCheck::kCount: break;
    }
    return "?";
}

// 길이는 바이트로 센다 — 통과하는 글자는 찍히는 ASCII 뿐이라 통과한 암호는 글자 수 = 바이트 수
//   (길이를 먼저 보므로 8바이트를 넘는 한글 암호는 BadChar 가 아니라 TooLong — 둘 다 거절이라 까닭 글만 다름)
PasswordCheck CheckPassword(const std::string& password) {
    if (password.size() < kPasswordMin) return PasswordCheck::TooShort;
    if (password.size() > kPasswordMax) return PasswordCheck::TooLong;
    bool letter = false, digit = false;
    for (const char c : password) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == ' ') return PasswordCheck::HasSpace;
        if (u < 0x21 || u > 0x7e) return PasswordCheck::BadChar;
        letter = letter || IsLetter(c);
        digit = digit || IsDigit(c);
    }
    if (!letter) return PasswordCheck::NoLetter;
    if (!digit) return PasswordCheck::NoDigit;
    return PasswordCheck::Ok;
}

bool Pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t* out, size_t outLen) {
    if (iterations == 0) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
              BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                                   static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
                                                   static_cast<ULONG>(saltLen), iterations, out, static_cast<ULONG>(outLen), 0));
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool MakePasswordRecord(const std::string& password, uint32_t iterations, PasswordRecord& out) {
    out.iterations = iterations;
    return RandomBytes(out.salt.data(), out.salt.size()) &&
           Pbkdf2Sha256(password, out.salt.data(), out.salt.size(), iterations, out.hash.data(), out.hash.size());
}

bool VerifyPassword(const std::string& password, const PasswordRecord& rec) {
    if (rec.iterations == 0 || rec.iterations > kPbkdf2IterationsMax) return false;
    uint8_t got[kPasswordHashSize];
    if (!Pbkdf2Sha256(password, rec.salt.data(), rec.salt.size(), rec.iterations, got, sizeof(got))) return false;
    uint8_t diff = 0;   // 같은 시간에 끝나는 비교
    for (size_t i = 0; i < kPasswordHashSize; ++i) diff |= static_cast<uint8_t>(got[i] ^ rec.hash[i]);
    return diff == 0;
}

}  // namespace nox
