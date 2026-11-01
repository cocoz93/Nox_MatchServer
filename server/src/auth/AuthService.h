// AuthService.h — 로그인 서버의 판단(가입 · 로그인). 설계 Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」 nox_auth.
//
// 소켓도 스레드도 모른다 — 요청 하나와 그 일꾼의 저장소를 넣으면 답 하나를 돌려준다(로비 `Lobby` 와 같은 짜임 `Lobby.h:9`).
//   일꾼 여럿이 이 하나를 같이 부른다 — 바뀌는 것은 셈(원자 값)뿐
// 길
//   · POST /login  — ★ 런처 P1 부터 닫힘: 410 `NXL!launcher_required`(암호 → 토큰 길을 막음 · 11-01 사용자 결정 4)
//     옛 뜻 — 폼 `name` · `password` → 본문 한 줄 `NXL1<토큰 16진>`(200) 또는 `NXL!<까닭>`(`LoginLine.h`).
//     DLL 은 받은 글을 그대로 `apgar` 자리에 넣는다(JSON 없음 — 클래스 설계 「설계하며 고른 것」)
//   · ★ 런처 P1 — 자동 로그인 열쇠(Docs/LAUNCHER_PLAN.md)
//     POST /launcher/login — 폼 `name` · `password` · `device`(기기 이름 · 없어도 됨) · `remember`(`1` = 30일 · 그 밖 = 12시간)
//                            → 200 `NXK1<열쇠 16진 64자>` · 실패는 옛 `/login` 과 같은 `NXL!<까닭>`(400 · 401 · 503)
//     POST /token          — 폼 `name` · `key`(`NXK1…`) → 200 `NXL1<토큰>`(옛 `/login` 답과 같은 꼴 — DLL 이 `apgar` 에 넣음) ·
//                            401 `NXL!key_invalid`(없음 · 만료 · 남의 이름) · 400 `NXL!bad_input` · 503 `NXL!server_error`.
//                            통과하면 열쇠 만료를 「지금 + 수명」으로 밀고 마지막 로그인을 적는다
//     POST /logout         — 폼 `key` → 200 `NXOK`(없는 열쇠여도 — 몇 번 해도 같다) · 400 · 503
//     열쇠 글은 저장하지 않는다 — SHA-256 만(`AccountStore.h`)
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

// ★ 런처 P1 — 열쇠 수명(쓸 때마다 다시 이만큼 · 11-01 사용자 결정 2). 이번 실행만 = 런처가 게임이 끝나면 `/logout` 으로 지운다 —
//   런처가 그걸 못 했을 때(강제 종료 등)의 뒷받침이 12시간(근거 없는 값 — 한 번 앉아 하는 시간보다 넉넉히)
constexpr uint32_t kKeyTtlRememberSec = 30u * 24u * 3600u;
constexpr uint32_t kKeyTtlSessionSec = 12u * 3600u;
constexpr size_t kDeviceMax = 64;   // 기기 이름 바이트(넘으면 자름 · 조절 글자는 `?` — 로그인을 막을 까닭은 아님)
constexpr std::string_view kLogoutOk = "NXOK";

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
    LoginOk,             // ★ 런처 P1 — 이제 `/launcher/login` 의 열쇠 발급(암호 확인 갈래 다섯은 옛 `/login` 과 같음)
    LoginWrongPassword,
    LoginNoAccount,      // 없거나 암호 없는 옛 행
    LoginBadInput,       // 이름 · 암호 규칙 밖 · 폼이 틀림
    LoginFailed,         // DB · 서명 · 열쇠 넣기 실패
    LoginClosed,         // ★ 런처 P1 — 옛 `/login`(410)
    TokenOk,             // ★ 런처 P1 — `/token`
    TokenBadKey,         //   열쇠 없음 · 남의 이름
    TokenExpired,        //   만료
    TokenBadInput,       //   이름 규칙 밖 · 열쇠 꼴 틀림 · 폼 틀림
    TokenFailed,         //   DB · 서명 실패
    LogoutOk,            // ★ 런처 P1 — `/logout`(없는 열쇠 포함)
    LogoutFailed,        //   DB 실패
    BadForm,             // 가입 폼이 틀림(`%` 꼴 · 칸 없음) · ★ 로그아웃 폼이 틀림
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
    HttpResponse Login(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);   // ★ 런처 P1 — `/launcher/login`(열쇠 발급)
    HttpResponse Token(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);   // ★ 런처 P1
    HttpResponse Logout(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);  // ★ 런처 P1
    HttpResponse Signup(const HttpRequest& req, IAccountStore& db, AuthOutcome& o);
    bool IssueLoginToken(const HttpRequest& req, IAccountStore& db, uint64_t accountId, const std::string& name, uint32_t now, std::string& line);

    AuthConfig cfg_;
    std::array<std::atomic<uint64_t>, kAuthOutcomeCount> stats_{};
};

}  // namespace nox::auth
