// auth_test.cpp — 로그인 서버 판단(`CAuthService`) 시험 — 메모리 저장소로(로그인 암호 조각 2 · Docs/LOGIN_AUTH_DESIGN.md).
//
//   ① 폼 풀기 — `+` · `%xx` · 틀린 `%` · 같은 이름 둘 · 빈 짝 · `=` 없음
//   ② 가입 갈래 — 만듦 · 옛 행에 암호 · 이미 있음 · 이름 · 암호 · 확인 다름 · 실패 · 폼 틀림
//   ③ 로그인 갈래 — 됨(토큰이 로비 쪽 확인을 통과 · 계정 번호 · IP · 만료 · 마지막 로그인) · 암호 틀림 · 계정 없음(없음 · 옛 행) · 규칙 밖 · 실패
//   ④ 길 — 방식 다름 405 · 없는 길 404
//   ⑤ 셈 — 갈래 열여섯이 모두 나옴 · 합 = 요청 수(세는 자리 하나)
//   ⑥ ★ 조각 3 화면 — 양식(GET / · /signup · 칸 한도 · 스크립트 없음) · 결과 화면 · 되돌린 이름 걸러 냄 · 암호 안 실림 ·
//     규칙 갈래마다 안내 글 · 화면 답 머리(HTML · 콘텐츠 보안 정책 · nosniff)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "../src/auth/AuthService.h"
#include "nox/LoginLine.h"
#include "nox/LoginToken.h"

namespace {

int g_checks = 0;
int g_fails = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  [실패] %s\n", what.c_str());
    }
}

using namespace nox::auth;

constexpr uint32_t kNow = 1761400000;
constexpr uint32_t kIp = 0x7f000001u;

nox::JoinKey Key() {
    nox::JoinKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(0xa0 ^ i);
    return k;
}

HttpRequest Post(const std::string& path, const std::string& body) {
    HttpRequest r;
    r.method = "POST";
    r.path = path;
    r.body = body;
    r.peerIp = kIp;
    return r;
}

struct Rig {
    CMemoryAccountStore db;
    CAuthService svc;
    unsigned requests = 0;
    Rig() : svc([] {
                AuthConfig c;
                c.loginKey = Key();
                c.iterations = 1000;   // 시험만 짧게(실제는 600,000 — 일꾼이 뜰 때 시간을 찍음)
                c.unixNow = [] { return kNow; };
                c.nonce = [] { return 0x0123456789abcdefull; };
                return c;
            }()) {}
    HttpResponse Do(const HttpRequest& r, AuthOutcome expect, const std::string& what) {
        AuthOutcome got = AuthOutcome::kCount;
        HttpResponse resp = svc.Handle(r, db, &got);
        ++requests;
        Check(got == expect, what + " — 갈래 " + AuthOutcomeName(got) + "(바란 것 " + AuthOutcomeName(expect) + ")");
        return resp;
    }
};

bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string Header(const HttpResponse& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name) return h.second;
    return "";
}

// ★ 조각 3 — 화면 답 꼴(HTML · 스크립트 금지 정책 · nosniff)
bool IsPage(const HttpResponse& r) {
    return r.contentType == "text/html; charset=utf-8" && Has(Header(r, "Content-Security-Policy"), "default-src 'none'") &&
           Has(Header(r, "Content-Security-Policy"), "form-action 'self'") && Header(r, "X-Content-Type-Options") == "nosniff" &&
           Has(r.body, "<!doctype html>") && Has(r.body, "</html>");
}

bool FailBody(const HttpResponse& r, int status, nox::LoginFail why) {
    std::string payload;
    nox::LoginFail f = nox::LoginFail::kCount;
    return r.status == status && nox::ClassifyApgar(r.body, payload) == nox::ApgarKind::Fail && nox::ParseLoginFail(payload, f) && f == why;
}

void TestForm() {
    std::printf("① 폼 풀기\n");
    std::map<std::string, std::string> f;
    Check(ParseForm("name=alice9&password=ab%2112+c", f) && f["name"] == "alice9" && f["password"] == "ab!12 c", "%21 · + 풀기");
    Check(ParseForm("a=%41%62", f) && f["a"] == "Ab", "대소문자 16진");
    Check(!ParseForm("a=%4", f) && !ParseForm("a=%", f) && !ParseForm("a=%zz", f) && !ParseForm("%4=b", f), "틀린 % → 실패(값 · 이름)");
    Check(ParseForm("a=1&a=2", f) && f["a"] == "1", "같은 이름 둘 → 첫 것");
    Check(ParseForm("&&a=1&", f) && f.size() == 1 && f["a"] == "1", "빈 짝 건너뜀");
    Check(ParseForm("flag&b=", f) && f.count("flag") && f["flag"].empty() && f.count("b") && f["b"].empty(), "= 없음 · 빈 값");
    Check(ParseForm("", f) && f.empty(), "빈 본문");
}

void TestSignupLogin() {
    std::printf("② 가입 · ③ 로그인 · ④ 길 · ⑤ 셈\n");
    Rig g;
    using O = AuthOutcome;
    // ② 가입
    HttpResponse r = g.Do(Post("/signup", "name=alice9&password=ab12cd&password2=ab12cd"), O::SignupCreated, "가입 만듦");
    Check(r.status == 201 && g.db.Count() == 1, "201 · 행 하나");
    Check(IsPage(r) && Has(r.body, "가입됨") && Has(r.body, ">alice9<") && Has(r.body, "새 계정을") && !Has(r.body, "ab12cd"),
          "결과 화면 — 이름 · 새 계정 글 · 암호는 안 실림");
    g.Do(Post("/signup", "name=alice9&password=zz99zz&password2=zz99zz"), O::SignupDuplicate, "같은 이름");
    Check(g.db.Count() == 1, "이미 있음 — 행 그대로");
    const uint64_t legacy = g.db.AddLegacy("bob");
    r = g.Do(Post("/signup", "name=bob&password=pw1234&password2=pw1234"), O::SignupClaimed, "옛 행에 암호");
    AccountRow bobRow;
    Check(r.status == 201 && g.db.Find("bob", bobRow) == FindResult::Found && bobRow.id == legacy && bobRow.hasPassword && g.db.Count() == 2,
          "옛 행 번호 그대로 · 암호 들어감 · 행 안 늘어남");
    Check(Has(r.body, "예전 이 이름의 기록") && !Has(r.body, "새 계정을"), "옛 행 결과 화면 글");
    g.Do(Post("/signup", "name=bob&password=pw9999&password2=pw9999"), O::SignupDuplicate, "옛 행에 이미 암호 → 이미 있음");
    Check(g.Do(Post("/signup", "name=ab_c&password=ab12&password2=ab12"), O::SignupBadName, "이름 밑줄").status == 400, "이름 규칙 400");
    g.Do(Post("/signup", "name=abcdefghij&password=ab12&password2=ab12"), O::SignupBadName, "이름 10자");
    g.Do(Post("/signup", "name=carol&password=abcdef&password2=abcdef"), O::SignupBadPassword, "숫자 없는 암호");
    g.Do(Post("/signup", "name=carol&password=ab+12&password2=ab+12"), O::SignupBadPassword, "빈칸 든 암호(+)");
    g.Do(Post("/signup", "name=carol&password=ab12cd&password2=ab12ce"), O::SignupMismatch, "확인 칸 다름");
    g.Do(Post("/signup", "name=carol&password=ab12cd"), O::BadForm, "확인 칸 없음");
    g.Do(Post("/signup", "name=car%ol&password=ab12cd&password2=ab12cd"), O::BadForm, "틀린 %");
    g.db.SetFail(true);
    Check(g.Do(Post("/signup", "name=carol&password=ab12cd&password2=ab12cd"), O::SignupFailed, "DB 안 됨").status == 503, "가입 실패 503");
    g.db.SetFail(false);
    Check(g.db.Count() == 2, "실패한 가입은 행을 안 남김");

    // ③ 로그인
    r = g.Do(Post("/login", "name=alice9&password=ab12cd"), O::LoginOk, "로그인 됨");
    std::string payload;
    Check(r.status == 200 && nox::ClassifyApgar(r.body, payload) == nox::ApgarKind::Token && payload.size() == nox::kLoginTokenSize * 2,
          "본문 = NXL1 + 16진 114자");
    uint8_t raw[nox::kLoginTokenSize] = {};
    nox::LoginToken t;
    Check(nox::ParseHex(payload, raw, sizeof(raw)) &&
              nox::VerifyLoginToken(Key(), raw, sizeof(raw), kNow, kIp, "alice9", t) == nox::LoginTokenCheck::Ok,
          "토큰이 로비 쪽 확인을 통과(같은 키 · IP · 이름)");
    Check(t.accountId == 1 && t.expiresAt == kNow + nox::kLoginTokenLifetimeSec && t.nonce == 0x0123456789abcdefull && t.clientIp == kIp,
          "계정 번호 · 만료 60초 · 일회용 · 상대 IP");
    Check(g.db.LastLogin(1) == kNow, "마지막 로그인 적힘");
    Check(g.Do(Post("/login", "name=bob&password=pw1234"), O::LoginOk, "옛 행에 암호 넣은 계정").status == 200, "옛 행 로그인");
    Check(FailBody(g.Do(Post("/login", "name=alice9&password=ab12ce"), O::LoginWrongPassword, "암호 틀림"), 401, nox::LoginFail::WrongPassword),
          "401 NXL!wrong_password");
    g.Do(Post("/login", "name=alice9&password=AB12cd"), O::LoginWrongPassword, "대소문자 다른 암호");
    Check(FailBody(g.Do(Post("/login", "name=dave&password=ab12cd"), O::LoginNoAccount, "없는 계정"), 401, nox::LoginFail::NoAccount),
          "401 NXL!no_account");
    g.db.AddLegacy("erin");
    g.Do(Post("/login", "name=erin&password=ab12cd"), O::LoginNoAccount, "암호 없는 옛 행 → 계정 없음(가입 안내)");
    Check(FailBody(g.Do(Post("/login", "name=al ice&password=ab12cd"), O::LoginBadInput, "이름 빈칸(+ 아닌 날 빈칸)"), 400, nox::LoginFail::BadInput),
          "400 NXL!bad_input");
    g.Do(Post("/login", "name=alice9&password=abc"), O::LoginBadInput, "규칙 밖 암호(3자) — 해시 없이");
    g.Do(Post("/login", "name=alice9&password=ab%zz"), O::LoginBadInput, "틀린 %");
    g.Do(Post("/login", "password=ab12cd"), O::LoginBadInput, "이름 칸 없음");
    g.db.SetFail(true);
    Check(FailBody(g.Do(Post("/login", "name=alice9&password=ab12cd"), O::LoginFailed, "DB 안 됨"), 503, nox::LoginFail::ServerError),
          "503 NXL!server_error");
    g.db.SetFail(false);

    // ④ 길
    HttpRequest get = Post("/login", "");
    get.method = "GET";
    Check(g.Do(get, O::BadMethod, "GET /login").status == 405, "405");
    Check(g.Do(Post("/admin", ""), O::NotFound, "없는 길").status == 404, "404");
    Check(g.Do(Post("/login/", "name=alice9&password=ab12cd"), O::NotFound, "끝에 / 붙은 길").status == 404, "길은 글자 그대로");

    // ⑥ 화면(조각 3)
    HttpRequest getPage = Post("/signup", "");
    getPage.method = "GET";
    r = g.Do(getPage, O::SignupPage, "GET /signup");
    auto count = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
        return n;
    };
    Check(count(r.body, "maxlength=\"16\"") == 2 && count(r.body, "minlength=\"4\"") == 2 && count(r.body, "maxlength=\"9\"") == 1,
          "칸 한도 — 암호 둘 4~16 · 이름 9(칸마다 셈 — 한 칸만 바뀌어도 걸리게)");
    Check(r.status == 200 && IsPage(r) && Has(r.body, "maxlength=\"9\"") && Has(r.body, "maxlength=\"16\"") &&
              Has(r.body, "action=\"/signup\"") && Has(r.body, "name=\"password2\"") && !Has(r.body, "<script"),
          "양식 — 칸 한도 9 · 16 · 보내는 곳 · 확인 칸 · 스크립트 없음");
    getPage.path = "/";
    Check(g.Do(getPage, O::SignupPage, "GET /").status == 200, "/ 도 양식");
    Check(g.Do(Post("/", ""), O::BadMethod, "POST /").status == 405, "POST / 405");
    HttpRequest other = Post("/signup", "");
    other.method = "OTHER";
    Check(g.Do(other, O::BadMethod, "다른 방식 /signup").status == 405, "다른 방식 405");
    r = g.Do(Post("/signup", "name=%3Cscript%3Ex%22&password=ab12cd&password2=ab12cd"), O::SignupBadName, "이름에 꺾쇠 · 따옴표");
    Check(IsPage(r) && !Has(r.body, "<script>x") && Has(r.body, "value=\"&lt;script&gt;x&quot;\""), "되돌린 이름은 걸러 실림");
    r = g.Do(Post("/signup", "name=frank&password=abcdef&password2=abcdef"), O::SignupBadPassword, "숫자 없음 화면");
    Check(r.status == 400 && Has(r.body, "숫자를 하나 이상") && Has(r.body, "value=\"frank\"") && !Has(r.body, "abcdef"),
          "안내 · 이름 다시 채움 · 암호 안 채움");
    r = g.Do(Post("/signup", "name=alice9&password=zz99zz&password2=zz99zz"), O::SignupDuplicate, "같은 이름 화면");
    Check(r.status == 409 && IsPage(r) && Has(r.body, "이미 있는 이름"), "409 화면 글");
    {   // 규칙 갈래마다 안내 글이 따로 나옴(기본 글로 안 떨어짐)
        struct Case {
            std::string form;
            O expect;
            std::string text;
        };
        const Case cases[] = {
            {"name=&password=ab12&password2=ab12", O::SignupBadName, "이름을 넣어"},
            {"name=abcdefghij&password=ab12&password2=ab12", O::SignupBadName, "9자까지"},
            {"name=a.b&password=ab12&password2=ab12", O::SignupBadName, "영문 · 숫자만"},
            {"name=gina&password=ab1&password2=ab1", O::SignupBadPassword, "4자 이상"},
            {"name=gina&password=abcdefghijklmno12&password2=abcdefghijklmno12", O::SignupBadPassword, "16자까지"},
            {"name=gina&password=ab+12&password2=ab+12", O::SignupBadPassword, "빈칸은"},
            {"name=gina&password=ab12%B0%A1&password2=ab12%B0%A1", O::SignupBadPassword, "특수문자만 됩니다"},   // 「한글 안 됨」은 양식 도움말에도 있어 못 가름
            {"name=gina&password=123456&password2=123456", O::SignupBadPassword, "영문을 하나"},
            {"name=gina&password=abcdef&password2=abcdef", O::SignupBadPassword, "숫자를 하나"},
            {"name=gina&password=ab12cd&password2=ab12ce", O::SignupMismatch, "암호 확인이 다릅니다"},
            {"name=gina&password=ab12cd", O::BadForm, "양식이 틀렸습니다"},
        };
        unsigned shown = 0;
        for (const Case& c : cases) {
            const HttpResponse cr = g.Do(Post("/signup", c.form), c.expect, "안내 글 " + c.text);
            shown += (cr.status == 400 && IsPage(cr) && Has(cr.body, c.text) && !Has(cr.body, "확인해 주세요")) ? 1 : 0;
        }
        Check(shown == sizeof(cases) / sizeof(cases[0]), "안내 글 " + std::to_string(shown) + "/" + std::to_string(sizeof(cases) / sizeof(cases[0])));
    }

    // ⑤ 셈
    const auto s = g.svc.Stats();
    unsigned long long total = 0;
    unsigned kinds = 0;
    for (const auto v : s) {
        total += v;
        kinds += v ? 1 : 0;
    }
    Check(kinds == kAuthOutcomeCount, "갈래 " + std::to_string(kinds) + "/" + std::to_string(kAuthOutcomeCount) + " 이 나옴");
    Check(total == g.requests, "셈 합 " + std::to_string(total) + " = 요청 " + std::to_string(g.requests));
    for (size_t i = 0; i < kAuthOutcomeCount; ++i) Check(AuthOutcomeName(static_cast<AuthOutcome>(i))[0] != '?', "갈래마다 이름 있음");
}

}  // namespace

// ★ 조각 3 — `auth_test --dump-pages <폴더>` = 화면 다섯을 파일로(헤드리스 크롬 캡처로 눈으로 보려고 · 시험과 같은 판단 길)
int DumpPages(const std::string& dir) {
    Rig g;
    g.db.AddLegacy("oldname");
    HttpRequest get = Post("/signup", "");
    get.method = "GET";
    const struct {
        const char* file;
        HttpResponse r;
    } pages[] = {
        {"form.html", g.svc.Handle(get, g.db)},
        {"done.html", g.svc.Handle(Post("/signup", "name=alice9&password=ab12cd&password2=ab12cd"), g.db)},
        {"done_claimed.html", g.svc.Handle(Post("/signup", "name=oldname&password=ab12cd&password2=ab12cd"), g.db)},
        {"error_password.html", g.svc.Handle(Post("/signup", "name=bob&password=abcdef&password2=abcdef"), g.db)},
        {"error_duplicate.html", g.svc.Handle(Post("/signup", "name=alice9&password=zz99zz&password2=zz99zz"), g.db)},
    };
    for (const auto& p : pages) {
        const std::string path = dir + "/" + p.file;
        std::ofstream f(path, std::ios::binary);
        f.write(p.r.body.data(), static_cast<std::streamsize>(p.r.body.size()));
        if (!f) {
            std::printf("[실패] 못 씀 %s\n", path.c_str());
            return 1;
        }
        std::printf("%s %d %zu바이트\n", p.file, p.r.status, p.r.body.size());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--dump-pages") == 0) return DumpPages(argv[2]);
    TestForm();
    TestSignupLogin();
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
