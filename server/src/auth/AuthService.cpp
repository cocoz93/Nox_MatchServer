// AuthService.cpp — 로그인 서버의 판단. 설명은 `AuthService.h` 머리.
#include "AuthService.h"

#include "SignupPage.h"
#include "nox/LoginLine.h"
#include "nox/LoginToken.h"

#include <utility>

namespace nox::auth {

const char* AuthOutcomeName(AuthOutcome o) {
    switch (o) {
    case AuthOutcome::SignupCreated: return "가입 — 만듦";
    case AuthOutcome::SignupClaimed: return "가입 — 옛 행에 암호";
    case AuthOutcome::SignupDuplicate: return "가입 — 이미 있음";
    case AuthOutcome::SignupBadName: return "가입 — 이름 규칙";
    case AuthOutcome::SignupBadPassword: return "가입 — 암호 규칙";
    case AuthOutcome::SignupMismatch: return "가입 — 암호 확인 다름";
    case AuthOutcome::SignupFailed: return "가입 — 실패";
    case AuthOutcome::LoginOk: return "로그인 — 됨";
    case AuthOutcome::LoginWrongPassword: return "로그인 — 암호 틀림";
    case AuthOutcome::LoginNoAccount: return "로그인 — 계정 없음";
    case AuthOutcome::LoginBadInput: return "로그인 — 규칙 밖";
    case AuthOutcome::LoginFailed: return "로그인 — 실패";
    case AuthOutcome::BadForm: return "폼 틀림";
    case AuthOutcome::SignupPage: return "가입 양식";
    case AuthOutcome::NotFound: return "없는 길";
    case AuthOutcome::BadMethod: return "방식 다름";
    case AuthOutcome::kCount: break;
    }
    return "?";
}

static int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool Decode(std::string_view s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%') {
            if (i + 2 >= s.size()) return false;   // `%` 뒤 두 글자가 없음
            const int hi = Hex(s[i + 1]), lo = Hex(s[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return true;
}

bool ParseForm(std::string_view body, std::map<std::string, std::string>& out) {
    out.clear();
    size_t i = 0;
    while (i <= body.size()) {
        size_t amp = body.find('&', i);
        if (amp == std::string_view::npos) amp = body.size();
        const std::string_view pair = body.substr(i, amp - i);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            std::string k, v;
            if (!Decode(pair.substr(0, eq), k)) return false;
            if (eq != std::string_view::npos && !Decode(pair.substr(eq + 1), v)) return false;
            out.emplace(std::move(k), std::move(v));   // 같은 이름이 둘이면 첫 것
        }
        i = amp + 1;
    }
    return true;
}

std::array<uint64_t, kAuthOutcomeCount> CAuthService::Stats() const {
    std::array<uint64_t, kAuthOutcomeCount> s{};
    for (size_t i = 0; i < kAuthOutcomeCount; ++i) s[i] = stats_[i].load(std::memory_order_relaxed);
    return s;
}

// ★ 조각 3 — 화면 답(가입 양식 · 결과)의 머리 — 스크립트 금지 · 바깥 자원 금지 · 폼은 이 서버로만 · 다른 사이트 안에 못 끼움
static HttpResponse Page(int status, std::string html) {
    HttpResponse r;
    r.status = status;
    r.contentType = "text/html; charset=utf-8";
    r.body = std::move(html);
    r.headers.emplace_back("Content-Security-Policy",
                           "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
    return r;
}

HttpResponse CAuthService::Handle(const HttpRequest& req, IAccountStore& db, AuthOutcome* outcome) {
    AuthOutcome o = AuthOutcome::NotFound;
    HttpResponse r;
    const bool page = req.path == "/" || req.path == "/signup";
    if (req.path == "/login" || page) {
        if (req.method == "POST" && req.path != "/") {
            r = req.path == "/login" ? Login(req, db, o) : Signup(req, db, o);
        } else if (req.method == "GET" && page) {
            o = AuthOutcome::SignupPage;
            r = Page(200, SignupFormPage());
        } else {
            o = AuthOutcome::BadMethod;
            r.status = 405;
            r.body = "method not allowed";
        }
    } else {
        r.status = 404;
        r.body = "not found";
    }
    r.headers.emplace_back("X-Content-Type-Options", "nosniff");
    stats_[static_cast<size_t>(o)].fetch_add(1, std::memory_order_relaxed);   // 세는 자리는 여기 하나
    if (outcome) *outcome = o;
    return r;
}

// ★ 조각 3 — 규칙 갈래마다 가입 화면의 안내 글(Ok · kCount 는 여기 안 오지만 빠짐없이 적고 기본 글로 떨어지게)
static const char* NameMessage(NameCheck c) {
    switch (c) {
    case NameCheck::Empty: return "이름을 넣어 주세요.";
    case NameCheck::TooLong: return "이름은 9자까지입니다.";
    case NameCheck::BadChar: return "이름은 영문 · 숫자만 됩니다.";
    case NameCheck::Ok:
    case NameCheck::kCount: break;
    }
    return "이름을 확인해 주세요.";
}

static const char* PasswordMessage(PasswordCheck c) {
    switch (c) {
    case PasswordCheck::TooShort: return "암호는 4자 이상입니다.";
    case PasswordCheck::TooLong: return "암호는 16자까지입니다.";
    case PasswordCheck::HasSpace: return "암호에 빈칸은 안 됩니다.";
    case PasswordCheck::BadChar: return "암호는 영문 · 숫자 · 특수문자만 됩니다(한글 안 됨).";
    case PasswordCheck::NoLetter: return "암호에 영문을 하나 이상 넣어 주세요.";
    case PasswordCheck::NoDigit: return "암호에 숫자를 하나 이상 넣어 주세요.";
    case PasswordCheck::Ok:
    case PasswordCheck::kCount: break;
    }
    return "암호를 확인해 주세요.";
}

static HttpResponse LoginFailReply(int status, LoginFail why) {
    HttpResponse r;
    r.status = status;
    r.body = std::string(kLoginFailMark) + LoginFailName(why);
    return r;
}

HttpResponse CAuthService::Login(const HttpRequest& req, IAccountStore& db, AuthOutcome& o) {
    std::map<std::string, std::string> form;
    const auto name = [&]() -> const std::string& { return form["name"]; };
    if (!ParseForm(req.body, form) || CheckAccountName(name()) != NameCheck::Ok || CheckPassword(form["password"]) != PasswordCheck::Ok) {
        o = AuthOutcome::LoginBadInput;   // 규칙 밖 암호는 어떤 행과도 안 맞는다 — 해시 없이 거절
        return LoginFailReply(400, LoginFail::BadInput);
    }
    AccountRow row;
    switch (db.Find(name(), row)) {
    case FindResult::Found: break;
    case FindResult::NotFound:
        o = AuthOutcome::LoginNoAccount;
        return LoginFailReply(401, LoginFail::NoAccount);
    case FindResult::Failed:
    case FindResult::kCount:
        o = AuthOutcome::LoginFailed;
        return LoginFailReply(503, LoginFail::ServerError);
    }
    if (!row.hasPassword) {   // 아이디만 시절의 행 — 가입하면 이 행에 암호가 들어간다
        o = AuthOutcome::LoginNoAccount;
        return LoginFailReply(401, LoginFail::NoAccount);
    }
    if (!VerifyPassword(form["password"], row.password)) {
        o = AuthOutcome::LoginWrongPassword;
        return LoginFailReply(401, LoginFail::WrongPassword);
    }
    const uint32_t now = cfg_.unixNow();
    LoginToken t;
    t.accountId = row.id;
    t.clientIp = req.peerIp;
    t.expiresAt = now + kLoginTokenLifetimeSec;
    t.name = name();
    bool nonceOk = true;
    if (cfg_.nonce) t.nonce = cfg_.nonce();
    else nonceOk = RandomBytes(reinterpret_cast<uint8_t*>(&t.nonce), sizeof(t.nonce));
    uint8_t raw[kLoginTokenSize];
    if (!db.TouchLogin(row.id, now) || !nonceOk || !SignLoginToken(cfg_.loginKey, t, raw)) {
        o = AuthOutcome::LoginFailed;
        return LoginFailReply(503, LoginFail::ServerError);
    }
    o = AuthOutcome::LoginOk;
    HttpResponse r;
    r.body = std::string(kLoginTokenMark) + ToHex(raw, sizeof(raw));
    return r;
}

// ★ 조각 3 — 답 본문은 결과 화면(실패면 안내 + 양식 · 이름은 다시 채움 · 암호는 안 채움)
HttpResponse CAuthService::Signup(const HttpRequest& req, IAccountStore& db, AuthOutcome& o) {
    std::map<std::string, std::string> form;
    if (!ParseForm(req.body, form) || !form.count("name") || !form.count("password") || !form.count("password2")) {
        o = AuthOutcome::BadForm;
        return Page(400, SignupFormPage("양식이 틀렸습니다 — 다시 넣어 주세요.", true));
    }
    const std::string& name = form["name"];
    const std::string& pw = form["password"];
    if (const NameCheck nc = CheckAccountName(name); nc != NameCheck::Ok) {
        o = AuthOutcome::SignupBadName;
        return Page(400, SignupFormPage(NameMessage(nc), true, name));
    }
    if (const PasswordCheck pc = CheckPassword(pw); pc != PasswordCheck::Ok) {
        o = AuthOutcome::SignupBadPassword;
        return Page(400, SignupFormPage(PasswordMessage(pc), true, name));
    }
    if (pw != form["password2"]) {
        o = AuthOutcome::SignupMismatch;
        return Page(400, SignupFormPage("암호 확인이 다릅니다.", true, name));
    }
    PasswordRecord rec;
    uint64_t id = 0;
    const InsertResult ir = MakePasswordRecord(pw, cfg_.iterations, rec) ? db.Insert(name, rec, cfg_.unixNow(), id) : InsertResult::Failed;
    switch (ir) {
    case InsertResult::Created:
    case InsertResult::Claimed:
        o = ir == InsertResult::Created ? AuthOutcome::SignupCreated : AuthOutcome::SignupClaimed;
        return Page(201, SignupDonePage(name, ir == InsertResult::Claimed));
    case InsertResult::Duplicate:
        o = AuthOutcome::SignupDuplicate;
        return Page(409, SignupFormPage("이미 있는 이름입니다 — 다른 이름을 골라 주세요.", true, name));
    case InsertResult::Failed:
    case InsertResult::kCount: break;
    }
    o = AuthOutcome::SignupFailed;
    return Page(503, SignupFormPage("서버가 지금 가입을 못 받습니다 — 잠시 뒤 다시 해 주세요.", true, name));
}

}  // namespace nox::auth
