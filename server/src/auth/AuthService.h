// AuthService.h — 로그인 서버의 판단(가입 · 로그인). 설계 Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」 nox_auth.
//
// 소켓도 스레드도 모른다 — 요청 하나와 그 일꾼의 저장소를 넣으면 답 하나를 돌려준다(로비 `Lobby` 와 같은 짜임 `Lobby.h:9`).
//   일꾼 여럿이 이 하나를 같이 부른다 — 바뀌는 것은 셈(원자 값)뿐
// 길
//   · POST /login  — 폼 `name` · `password` → 본문 한 줄 `NXL1<토큰 16진>`(200) 또는 `NXL!<까닭>`(`LoginLine.h`).
//     DLL 은 받은 글을 그대로 `apgar` 자리에 넣는다(JSON 없음 — 클래스 설계 「설계하며 고른 것」)
//   · POST /signup — 폼 `name` · `password` · `password2` → 201 · 409 · 400 · 503 · 본문은 결과 화면(★ 조각 3 `SignupPage.h`)
//   · GET / · /signup — 가입 양식(★ 조각 3)
//   · 그 밖 — 404 · 길은 맞는데 방식이 다르면 405
// 화면 답에는 콘텐츠 보안 정책(스크립트 금지 · 폼은 이 서버로만)을 · 모든 답에 `nosniff` 를 싣는다
// 이름 · 암호 규칙(`AccountRules.h`)에 안 맞으면 해시도 DB 도 안 간다
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "AccountStore.h"
#include "HttpSysServer.h"
#include "nox/JoinToken.h"

namespace nox::auth {

struct AuthConfig {
    JoinKey loginKey{};                              // 로그인 토큰 서명 키(`[auth] key` — 로비와 같은 값 · 입장 토큰 키와 따로)
    uint32_t iterations = kPbkdf2Iterations;         // 새 가입의 반복 수(확인은 행에 적힌 수로)
    std::function<uint32_t()> unixNow = UnixNow;     // 시험이 정해 보려고
    std::function<uint64_t()> nonce;                 // 일회용 번호 — 비면 BCrypt 난수
};

// 요청 하나는 아래 갈래 중 하나로 끝난다(셈 · 시험은 개수로 본다)
enum class AuthOutcome : uint8_t {
    SignupCreated,
    SignupClaimed,       // 암호 없는 옛 행에 암호를 넣음(`AccountStore.h`)
    SignupDuplicate,
    SignupBadName,
    SignupBadPassword,
    SignupMismatch,      // 암호 확인 칸이 다름
    SignupFailed,        // 해시 · DB 실패
    LoginOk,
    LoginWrongPassword,
    LoginNoAccount,      // 없거나 암호 없는 옛 행
    LoginBadInput,       // 이름 · 암호 규칙 밖 · 폼이 틀림
    LoginFailed,         // DB · 서명 실패
    BadForm,             // 가입 폼이 틀림(`%` 꼴 · 칸 없음)
    SignupPage,          // ★ 조각 3 — GET 가입 양식
    NotFound,
    BadMethod,
    kCount
};
const char* AuthOutcomeName(AuthOutcome o);
constexpr size_t kAuthOutcomeCount = static_cast<size_t>(AuthOutcome::kCount);

// `application/x-www-form-urlencoded` 풀기(`+` = 빈칸 · `%xx`). 틀린 `%` 면 false · 같은 이름이 둘이면 첫 것
bool ParseForm(std::string_view body, std::map<std::string, std::string>& out);

class CAuthService {
public:
    explicit CAuthService(AuthConfig cfg) : cfg_(std::move(cfg)) {}

    HttpResponse Handle(const HttpRequest& req, IAccountStore& db, AuthOutcome* outcome = nullptr);
    std::array<uint64_t, kAuthOutcomeCount> Stats() const;

private:
    HttpResponse Login(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);
    HttpResponse Signup(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);

    AuthConfig cfg_;
    std::array<std::atomic<uint64_t>, kAuthOutcomeCount> stats_{};
};

}  // namespace nox::auth
