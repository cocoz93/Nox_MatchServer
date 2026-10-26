// AuthService.cpp — 로그인 서버의 판단. 설명은 `AuthService.h` 머리.
#include "AuthService.h"

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

HttpResponse CAuthService::Handle(const HttpRequest& req, IAccountStore& db, AuthOutcome* outcome) {
    AuthOutcome o = AuthOutcome::NotFound;
    HttpResponse r;
    if (req.path == "/login" || req.path == "/signup") {
        if (req.method != "POST") {
            o = AuthOutcome::BadMethod;
            r.status = 405;
            r.body = "POST only";
        } else {
            r = req.path == "/login" ? Login(req, db, o) : Signup(req, db, o);
        }
    } else {
        r.status = 404;
        r.body = "not found";
    }
    stats_[static_cast<size_t>(o)].fetch_add(1, std::memory_order_relaxed);   // 세는 자리는 여기 하나
    if (outcome) *outcome = o;
    return r;
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

HttpResponse CAuthService::Signup(const HttpRequest& req, IAccountStore& db, AuthOutcome& o) {
    HttpResponse r;
    std::map<std::string, std::string> form;
    if (!ParseForm(req.body, form) || !form.count("name") || !form.count("password") || !form.count("password2")) {
        o = AuthOutcome::BadForm;
        r.status = 400;
        r.body = "bad form";
        return r;
    }
    const std::string& name = form["name"];
    const std::string& pw = form["password"];
    if (const NameCheck nc = CheckAccountName(name); nc != NameCheck::Ok) {
        o = AuthOutcome::SignupBadName;
        r.status = 400;
        r.body = std::string("name: ") + NameCheckName(nc);
        return r;
    }
    if (const PasswordCheck pc = CheckPassword(pw); pc != PasswordCheck::Ok) {
        o = AuthOutcome::SignupBadPassword;
        r.status = 400;
        r.body = std::string("password: ") + PasswordCheckName(pc);
        return r;
    }
    if (pw != form["password2"]) {
        o = AuthOutcome::SignupMismatch;
        r.status = 400;
        r.body = "password2: 다름";
        return r;
    }
    PasswordRecord rec;
    uint64_t id = 0;
    const InsertResult ir = MakePasswordRecord(pw, cfg_.iterations, rec) ? db.Insert(name, rec, cfg_.unixNow(), id) : InsertResult::Failed;
    switch (ir) {
    case InsertResult::Created:
    case InsertResult::Claimed:
        o = ir == InsertResult::Created ? AuthOutcome::SignupCreated : AuthOutcome::SignupClaimed;
        r.status = 201;
        r.body = "created " + std::to_string(id);
        return r;
    case InsertResult::Duplicate:
        o = AuthOutcome::SignupDuplicate;
        r.status = 409;
        r.body = "name: 이미 있음";
        return r;
    case InsertResult::Failed:
    case InsertResult::kCount: break;
    }
    o = AuthOutcome::SignupFailed;
    r.status = 503;
    r.body = "server error";
    return r;
}

}  // namespace nox::auth
