// auth_test.cpp — 로그인 서버 판단(`CAuthService`) 시험 — 메모리 저장소로(로그인 암호 조각 2 · Docs/LOGIN_AUTH_DESIGN.md).
//
//   ① 폼 풀기 — `+` · `%xx` · 틀린 `%` · 같은 이름 둘 · 빈 짝 · `=` 없음
//   ② 가입 갈래 — 만듦 · 옛 행에 암호 · 이미 있음 · 이름 · 암호 · 확인 다름 · 실패 · 폼 틀림
//   ③ 로그인 갈래 — ★ 런처 P1: 옛 /login 닫힘(410) · /launcher/login 열쇠 발급(NXK1 · 수명 30일 · 12시간 · 만료 행 지움 · 기기 이름) ·
//     /token 토큰(로비 쪽 확인 통과 · 계정 번호 · IP · 만료 · 마지막 로그인 · 쓸 때마다 만료 밀림 · 만료 · 남의 이름 · 없는 열쇠 · 꼴 · 실패) ·
//     /logout(지운 열쇠는 토큰 못 받음 · 두 번 · 꼴 · 실패) · 발급의 암호 갈래(암호 틀림 · 계정 없음(없음 · 옛 행) · 규칙 밖 · 실패)
//   ④ 길 — 방식 다름 405 · 없는 길 404
//   ⑤ 셈 — 갈래 열여섯이 모두 나옴 · 합 = 요청 수(세는 자리 하나)
//   ⑥ ★ 조각 3 화면 — 양식(GET / · /signup · 칸 한도 · 스크립트 없음) · 결과 화면 · 되돌린 이름 걸러 냄 · 암호 안 실림 ·
//     규칙 갈래마다 안내 글 · 화면 답 머리(HTML · 콘텐츠 보안 정책 · nosniff)
//   ⑧ ★ 11-05 플랜1 암호 바꾸기 — 바뀜 · 옛 암호 거절 · 다른 기기 열쇠 전부 삭제 · 현재 암호 틀림 · 새 암호 규칙 밖 · 같은 암호 · 저장 실패면 그대로 · 시도 제한
//   ⑦ ★ 11-05 플랜1 시도 제한 — 단계 표 전수 · 횟수별 잠금 시간 · 막힌 동안 DB · 해시 안 감 · 성공 · 1시간 초기화 · 다른 계정 · 주소 안 막힘 ·
//     주소별 합산(로그인 실패 + 가입) · 없는 이름은 계정 칸 안 만듦 · 잠긴 동안 돌아온 요청은 안 셈 · 지난 칸 지움
#include <cctype>
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
    uint32_t clock = kNow;   // ★ 런처 P1 — 열쇠 만료를 보려고 시계를 움직인다
    CAuthService svc;
    unsigned requests = 0;
    Rig() : svc([this] {
                AuthConfig c;
                c.loginKey = Key();
                c.iterations = 1000;   // 시험만 짧게(실제는 600,000 — 일꾼이 뜰 때 시간을 찍음)
                c.unixNow = [this] { return clock; };
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

    // ③ 로그인 — ★ 런처 P1: 옛 `/login` 은 닫힘 · `/launcher/login` 이 열쇠 · `/token` 이 토큰 · `/logout` 이 폐기
    r = g.Do(Post("/login", "name=alice9&password=ab12cd"), O::LoginClosed, "옛 /login 닫힘");
    Check(FailBody(r, 410, nox::LoginFail::LauncherRequired), "410 NXL!launcher_required");
    r = g.Do(Post("/launcher/login", "name=alice9&password=ab12cd&device=PC%01one&remember=1"), O::LoginOk, "열쇠 발급(자동 로그인)");
    const std::string keyMark(nox::kLoginKeyMark);
    const std::string aliceKey = r.body;
    bool keyHex = r.status == 200 && aliceKey.size() == keyMark.size() + 2 * nox::kLoginKeyBytes && aliceKey.compare(0, keyMark.size(), keyMark) == 0;
    for (size_t i = keyMark.size(); keyHex && i < aliceKey.size(); ++i) keyHex = std::isxdigit(static_cast<unsigned char>(aliceKey[i])) != 0;
    Check(keyHex && g.db.KeyCount() == 1 && Header(r, "Cache-Control") == "no-store", "본문 = NXK1 + 16진 64자 · 열쇠 행 하나 · 저장 금지 머리");
    Check(g.db.LastLogin(1) == 0, "열쇠 발급은 마지막 로그인이 아님(게임에 들어갈 때 — /token)");
    auto tokenOf = [&](const std::string& name, const std::string& key) { return Post("/token", "name=" + name + "&key=" + key); };
    r = g.Do(tokenOf("alice9", aliceKey), O::TokenOk, "토큰 됨");
    std::string payload;
    Check(r.status == 200 && nox::ClassifyApgar(r.body, payload) == nox::ApgarKind::Token && payload.size() == nox::kLoginTokenSize * 2 &&
              Header(r, "Cache-Control") == "no-store",
          "본문 = NXL1 + 16진 114자(옛 /login 답과 같은 꼴)");
    uint8_t raw[nox::kLoginTokenSize] = {};
    nox::LoginToken t;
    Check(nox::ParseHex(payload, raw, sizeof(raw)) &&
              nox::VerifyLoginToken(Key(), raw, sizeof(raw), kNow, kIp, "alice9", t) == nox::LoginTokenCheck::Ok,
          "토큰이 로비 쪽 확인을 통과(같은 키 · IP · 이름)");
    Check(t.accountId == 1 && t.expiresAt == kNow + nox::kLoginTokenLifetimeSec && t.nonce == 0x0123456789abcdefull && t.clientIp == kIp,
          "계정 번호 · 만료 60초 · 일회용 · 상대 IP");
    Check(g.db.LastLogin(1) == kNow, "마지막 로그인 적힘(/token)");
    g.clock = kNow + 29u * 24u * 3600u;   // 자동 로그인 열쇠 — 29일 뒤에도 됨 · 쓸 때마다 30일 밀림
    g.Do(tokenOf("alice9", aliceKey), O::TokenOk, "29일 뒤 토큰");
    g.clock += 29u * 24u * 3600u;
    g.Do(tokenOf("alice9", aliceKey), O::TokenOk, "다시 29일 뒤(밀린 만료) 토큰");
    g.clock += kKeyTtlRememberSec + 1;
    Check(FailBody(g.Do(tokenOf("alice9", aliceKey), O::TokenExpired, "30일 넘게 안 씀"), 401, nox::LoginFail::KeyInvalid), "401 NXL!key_invalid(만료)");
    g.clock = kNow;
    r = g.Do(Post("/launcher/login", "name=bob&password=pw1234"), O::LoginOk, "옛 행에 암호 넣은 계정 · 이번 실행만(remember 없음)");
    const std::string bobKey = r.body;
    g.clock = kNow + kKeyTtlSessionSec - 1;
    g.Do(tokenOf("bob", bobKey), O::TokenOk, "이번 실행만 열쇠 — 12시간 안");
    g.clock += kKeyTtlSessionSec + 1;   // 밀린 만료(마지막 씀 + 12시간)를 넘김(같은 초는 아직 됨 — `<`)
    g.Do(tokenOf("bob", bobKey), O::TokenExpired, "이번 실행만 열쇠 — 마지막 씀부터 12시간 넘음");
    const size_t keysBefore = g.db.KeyCount();
    g.Do(Post("/launcher/login", "name=bob&password=pw1234&remember=0"), O::LoginOk, "bob 새 열쇠(만료된 bob 열쇠는 지워짐)");
    Check(keysBefore == 2 && g.db.KeyCount() == 2, "만료된 bob 열쇠 지움 — 행 수 그대로 2(alice9 1 · bob 새 1 — 다른 계정 행은 안 건드림)");
    g.clock = kNow;
    Check(FailBody(g.Do(tokenOf("bob", aliceKey), O::TokenBadKey, "남의 열쇠를 제 이름으로"), 401, nox::LoginFail::KeyInvalid), "401 남의 이름");
    const std::string unknownKey = keyMark + std::string(64, '0');
    Check(FailBody(g.Do(tokenOf("alice9", unknownKey), O::TokenBadKey, "없는 열쇠"), 401, nox::LoginFail::KeyInvalid), "401 없는 열쇠");
    Check(FailBody(g.Do(tokenOf("al_ce", aliceKey), O::TokenBadInput, "이름 규칙 밖"), 400, nox::LoginFail::BadInput), "400 이름");
    g.Do(tokenOf("alice9", aliceKey.substr(0, aliceKey.size() - 1)), O::TokenBadInput, "열쇠 63자");
    g.Do(tokenOf("alice9", "NXK2" + aliceKey.substr(4)), O::TokenBadInput, "열쇠 머리 다름");
    g.Do(tokenOf("alice9", keyMark + std::string(64, 'g')), O::TokenBadInput, "열쇠 16진 아님");
    g.Do(Post("/token", "name=alice9"), O::TokenBadInput, "열쇠 칸 없음");
    g.Do(Post("/token", "name=alice9&key=%zz"), O::TokenBadInput, "틀린 %");
    g.db.SetFail(true);
    Check(FailBody(g.Do(tokenOf("alice9", aliceKey), O::TokenFailed, "토큰 — DB 안 됨"), 503, nox::LoginFail::ServerError), "503 토큰");
    g.db.SetFail(false);

    // 열쇠 발급의 암호 확인 갈래(옛 /login 과 같은 판단)
    Check(FailBody(g.Do(Post("/launcher/login", "name=alice9&password=ab12ce"), O::LoginWrongPassword, "암호 틀림"), 401, nox::LoginFail::WrongPassword),
          "401 NXL!wrong_password");
    g.Do(Post("/launcher/login", "name=alice9&password=AB12cd"), O::LoginWrongPassword, "대소문자 다른 암호");
    Check(FailBody(g.Do(Post("/launcher/login", "name=dave&password=ab12cd"), O::LoginNoAccount, "없는 계정"), 401, nox::LoginFail::NoAccount),
          "401 NXL!no_account");
    g.db.AddLegacy("erin");
    g.Do(Post("/launcher/login", "name=erin&password=ab12cd"), O::LoginNoAccount, "암호 없는 옛 행 → 계정 없음(가입 안내)");
    Check(FailBody(g.Do(Post("/launcher/login", "name=al ice&password=ab12cd"), O::LoginBadInput, "이름 빈칸(+ 아닌 날 빈칸)"), 400, nox::LoginFail::BadInput),
          "400 NXL!bad_input");
    g.Do(Post("/launcher/login", "name=alice9&password=abc"), O::LoginBadInput, "규칙 밖 암호(3자) — 해시 없이");
    g.Do(Post("/launcher/login", "name=alice9&password=ab%zz"), O::LoginBadInput, "틀린 %");
    g.Do(Post("/launcher/login", "password=ab12cd"), O::LoginBadInput, "이름 칸 없음");
    const size_t keysNoFail = g.db.KeyCount();
    g.db.SetFail(true);
    Check(FailBody(g.Do(Post("/launcher/login", "name=alice9&password=ab12cd"), O::LoginFailed, "DB 안 됨"), 503, nox::LoginFail::ServerError),
          "503 NXL!server_error");
    g.db.SetFail(false);
    Check(g.db.KeyCount() == keysNoFail, "실패한 발급은 열쇠를 안 남김");

    // 기기 이름 — 조절 글자 `?` · 64바이트에서 자름(로그인은 막지 않음)
    r = g.Do(Post("/launcher/login", "name=alice9&password=ab12cd&device=" + std::string(70, 'x')), O::LoginOk, "긴 기기 이름");
    Check(r.status == 200, "긴 기기 이름도 발급");
    Check(g.db.HasDevice("PC?one") && g.db.HasDevice(std::string(64, 'x')) && !g.db.HasDevice(std::string(70, 'x')),
          "기기 이름 — %01 → ? · 70바이트 → 64");

    // 로그아웃 — 지운 열쇠는 토큰 못 받음 · 몇 번 해도 같음
    r = g.Do(Post("/logout", "key=" + aliceKey), O::LogoutOk, "로그아웃");
    Check(r.status == 200 && r.body == std::string(kLogoutOk), "200 NXOK");
    g.Do(tokenOf("alice9", aliceKey), O::TokenBadKey, "로그아웃한 열쇠로 토큰");
    Check(g.Do(Post("/logout", "key=" + aliceKey), O::LogoutOk, "또 로그아웃").status == 200, "없는 열쇠 로그아웃도 200");
    Check(g.Do(Post("/logout", "key=abc"), O::BadForm, "로그아웃 열쇠 꼴 틀림").status == 400, "로그아웃 400");
    g.db.SetFail(true);
    Check(g.Do(Post("/logout", "key=" + bobKey), O::LogoutFailed, "로그아웃 — DB 안 됨").status == 503, "로그아웃 503");
    g.db.SetFail(false);
    HttpRequest getTok = tokenOf("alice9", aliceKey);
    getTok.method = "GET";
    Check(g.Do(getTok, O::BadMethod, "GET /token").status == 405, "GET /token 405");
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

    // ⑧ 암호 바꾸기 네 갈래가 이 셈에도 나오게(자세한 값은 TestChangePassword)
    g.Do(Post("/launcher/password", "name=alice9&password=ab12cd&new_password=ef34gh"), O::PasswordChanged, "셈 — 바꿈");
    g.Do(Post("/launcher/password", "name=alice9&password=ab12cd&new_password=ij56kl"), O::PasswordWrong, "셈 — 옛 암호");
    g.Do(Post("/launcher/password", "name=alice9&password=ef34gh&new_password=ef34gh"), O::PasswordBadInput, "셈 — 같은 암호");
    g.db.SetFailChange(true);
    g.Do(Post("/launcher/password", "name=alice9&password=ef34gh&new_password=ij56kl"), O::PasswordFailed, "셈 — 저장 실패");
    g.db.SetFailChange(false);
    g.Do(Post("/launcher/password", "name=alice9&password=ef34gh&new_password=ab12cd"), O::PasswordChanged, "셈 — 되돌림");

    // ⑦ 시도 제한 두 갈래가 이 셈에도 나오게(자세한 값은 TestThrottle)
    constexpr uint32_t kThrottledIp = 0x0a000009u;
    for (int i = 0; i < 20; ++i) {
        HttpRequest bad = Post("/launcher/login", "name=nobody" + std::to_string(i % 10) + "&password=ab12cd");
        bad.peerIp = kThrottledIp;
        g.Do(bad, O::LoginNoAccount, "주소별 20회 채움");
    }
    HttpRequest lockedLogin = Post("/launcher/login", "name=alice9&password=ab12cd");
    lockedLogin.peerIp = kThrottledIp;
    g.Do(lockedLogin, O::LoginThrottled, "주소 잠김 — 로그인");
    HttpRequest lockedSignup = Post("/signup", "name=zed&password=ab12cd&password2=ab12cd");
    lockedSignup.peerIp = kThrottledIp;
    g.Do(lockedSignup, O::SignupThrottled, "주소 잠김 — 가입");

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

// ★ 11-05 플랜1 — 시도 제한(`LoginThrottle.h`)
std::string Detail(const HttpResponse& r) {
    const size_t nl = r.body.find('\n');
    return nl == std::string::npos ? std::string() : r.body.substr(nl + 1);
}

void TestThrottle() {
    std::printf("⑦ 시도 제한\n");
    using O = AuthOutcome;
    // 단계 표 전수 — 경계마다
    const struct {
        uint32_t fails, account, address;
    } stages[] = {
        {0, 0, 0}, {1, 0, 0}, {4, 0, 0}, {5, 60, 0}, {6, 300, 0}, {7, 900, 0}, {8, 900, 0}, {19, 900, 0}, {20, 900, 600}, {21, 900, 600}, {1000, 900, 600},
    };
    for (const auto& st : stages)
        Check(AccountLockSec(st.fails) == st.account && AddressLockSec(st.fails) == st.address, "단계 표 " + std::to_string(st.fails) + "회");

    constexpr uint32_t kOther = 0x0a000002u;   // 계정별만 보려고 주소를 나눈다
    auto from = [](uint32_t ip, HttpRequest r) {
        r.peerIp = ip;
        return r;
    };
    Rig g;
    g.Do(from(kOther, Post("/signup", "name=alice9&password=ab12cd&password2=ab12cd")), O::SignupCreated, "alice9 가입");
    g.Do(from(kOther, Post("/signup", "name=bob&password=pw1234&password2=pw1234")), O::SignupCreated, "bob 가입");
    const HttpRequest wrong = Post("/launcher/login", "name=alice9&password=ab12zz");
    const HttpRequest right = Post("/launcher/login", "name=alice9&password=ab12cd");

    // 횟수별 잠금 시간 — 5회째 1분 → 풀린 뒤 틀리면 5분 → 15분 → 15분
    for (int i = 1; i <= 5; ++i) g.Do(wrong, O::LoginWrongPassword, std::to_string(i) + "회째 — 아직 암호 틀림 답");
    unsigned finds = g.db.FindCalls();
    HttpResponse r = g.Do(right, O::LoginThrottled, "5회 뒤 맞는 암호도 거절");
    Check(FailBody(HttpResponse{r.status, r.contentType, r.body.substr(0, r.body.find('\n')), {}}, 429, nox::LoginFail::TooManyTries), "429 첫 줄 NXL!too_many_tries");
    Check(Detail(r) == "60 account" && Header(r, "Retry-After") == "60", "둘째 줄 60 account · Retry-After");
    Check(g.db.FindCalls() == finds, "막힌 동안 DB · 해시 안 감");
    g.clock = kNow + 59;
    Check(Detail(g.Do(wrong, O::LoginThrottled, "1분 안")) == "1 account", "남은 초가 줄어듦");
    Check(g.db.FindCalls() == finds, "막힌 동안 틀린 암호도 안 셈");
    g.clock = kNow + 60;
    g.Do(wrong, O::LoginWrongPassword, "풀린 뒤 또 틀림");
    Check(Detail(g.Do(right, O::LoginThrottled, "6회째 뒤")) == "300 account", "6회째 5분");
    g.clock += 300;
    g.Do(wrong, O::LoginWrongPassword, "7회째");
    Check(Detail(g.Do(right, O::LoginThrottled, "7회째 뒤")) == "900 account", "7회째 15분");
    g.clock += 900;
    g.Do(wrong, O::LoginWrongPassword, "8회째");
    Check(Detail(g.Do(right, O::LoginThrottled, "8회째 뒤")) == "900 account", "8회째도 15분");

    // 다른 계정 · 다른 주소의 같은 계정
    g.Do(from(kOther, Post("/launcher/login", "name=bob&password=pw1234")), O::LoginOk, "다른 계정은 안 막힘");
    g.Do(Post("/launcher/login", "name=bob&password=pw1234"), O::LoginOk, "같은 주소의 다른 계정도 안 막힘(주소는 아직 20회 전)");
    g.Do(from(kOther, right), O::LoginThrottled, "계정별 잠금은 주소를 바꿔도 막힘");

    // 성공 초기화
    g.clock += 900;
    g.Do(right, O::LoginOk, "풀린 뒤 맞는 암호");
    for (int i = 0; i < 4; ++i) g.Do(wrong, O::LoginWrongPassword, "성공 뒤 다시 1~4회");
    g.Do(right, O::LoginOk, "4회까진 안 막힘 — 셈이 0 부터(성공 초기화)");

    // 1시간 초기화 — 4회 + 1시간 + 4회 = 안 막힘 · 3599 초면 이어짐
    for (int i = 0; i < 4; ++i) g.Do(wrong, O::LoginWrongPassword, "1시간 시험 앞 4회");
    g.clock += 3600;
    for (int i = 0; i < 4; ++i) g.Do(wrong, O::LoginWrongPassword, "1시간 뒤 4회");
    g.Do(right, O::LoginOk, "1시간 뒤 셈이 0 부터");
    for (int i = 0; i < 4; ++i) g.Do(wrong, O::LoginWrongPassword, "3599 시험 앞 4회");
    g.clock += 3599;
    g.Do(wrong, O::LoginWrongPassword, "3599 초 뒤 5회째");
    Check(Detail(g.Do(right, O::LoginThrottled, "3599 초면 이어짐")) == "60 account", "1시간 전이면 셈이 이어짐");

    // 주소별 — 로그인 실패 + 가입 시도 합산 · 20회째 10분 · 다른 주소 안 막힘
    {
        Rig a;
        constexpr uint32_t kBad = 0x0a000003u;
        a.Do(from(kOther, Post("/signup", "name=carol&password=ab12cd&password2=ab12cd")), O::SignupCreated, "carol 가입");
        for (int i = 0; i < 9; ++i) a.Do(from(kBad, Post("/launcher/login", "name=none" + std::to_string(i) + "&password=ab12cd")), O::LoginNoAccount, "없는 이름");
        a.db.AddLegacy("erin");
        a.Do(from(kBad, Post("/launcher/login", "name=erin&password=ab12cd")), O::LoginNoAccount, "옛 행(주소별만)");
        for (int i = 0; i < 9; ++i)
            a.Do(from(kBad, Post("/signup", "name=new" + std::to_string(i) + "&password=ab12cd&password2=ab12cd")), O::SignupCreated, "가입 시도도 합산");
        a.Do(from(kBad, Post("/signup", "name=badname_&password=ab12cd&password2=ab12cd")), O::SignupBadName, "규칙 밖은 해시를 안 돌려 안 셈");
        a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab%zz")), O::LoginBadInput, "폼 틀림도 안 셈");
        a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab12cd")), O::LoginOk, "19회 — 아직 안 막힘 · 성공은 주소 셈을 안 지움");
        a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab12zz")), O::LoginWrongPassword, "20회째");
        const unsigned aFinds = a.db.FindCalls(), aInserts = a.db.InsertCalls();
        HttpResponse ar = a.Do(from(kBad, Post("/launcher/login", "name=new1&password=ab12cd")), O::LoginThrottled, "주소 잠김 — 딴 계정도");
        Check(ar.status == 429 && Detail(ar) == "600 address", "600 address");
        ar = a.Do(from(kBad, Post("/signup", "name=fresh&password=ab12cd&password2=ab12cd")), O::SignupThrottled, "주소 잠김 — 가입");
        Check(ar.status == 429 && IsPage(ar) && Has(ar.body, "시도가 너무 많습니다. 10분 뒤 다시 해 주세요.") && Header(ar, "Retry-After") == "600" &&
                  Has(ar.body, "value=\"fresh\""),
              "가입 화면 문구 · 이름 다시 채움");
        Check(a.db.FindCalls() == aFinds && a.db.InsertCalls() == aInserts, "주소 잠긴 동안 DB · 해시 안 감");
        a.Do(from(kOther, Post("/launcher/login", "name=carol&password=ab12cd")), O::LoginOk, "다른 주소는 안 막힘");
        a.clock += 599;
        Check(Has(a.Do(from(kBad, Post("/signup", "name=fresh&password=ab12cd&password2=ab12cd")), O::SignupThrottled, "1초 남음").body, "1분 뒤"), "초를 분으로 올림");
        a.clock += 1;
        a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab12cd")), O::LoginOk, "10분 뒤 풀림");
        a.Do(from(kBad, Post("/launcher/login", "name=none0&password=ab12cd")), O::LoginNoAccount, "풀린 뒤 또 실패");
        Check(Detail(a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab12cd")), O::LoginThrottled, "또 10분")) == "600 address",
              "1시간 안이면 실패마다 다시 10분");
        a.clock += 3600;
        a.Do(from(kBad, Post("/launcher/login", "name=carol&password=ab12cd")), O::LoginOk, "마지막 실패에서 1시간 — 주소 셈 0");
    }

    // 없는 이름은 계정 칸을 안 만든다 — 없는 이름으로 5번 틀린 뒤 그 이름으로 가입하면 안 막힘
    {
        Rig n;
        constexpr uint32_t kA = 0x0a000004u, kB = 0x0a000005u;
        for (int i = 0; i < 6; ++i) n.Do(from(kA, Post("/launcher/login", "name=dave&password=ab12cd")), O::LoginNoAccount, "없는 이름 6회");
        n.Do(from(kB, Post("/signup", "name=dave&password=ab12cd&password2=ab12cd")), O::SignupCreated, "그 이름으로 가입");
        n.Do(from(kB, Post("/launcher/login", "name=dave&password=ab12cd")), O::LoginOk, "계정 칸이 없어 안 막힘");
        n.Do(from(kB, Post("/token", "name=dave&key=NXK1" + std::string(64, '0'))), O::TokenBadKey, "열쇠 길은 시도 제한을 안 지남");
    }
    // 열쇠 길 — 암호로 잠겨도 있던 열쇠는 그대로
    {
        Rig k;
        k.Do(Post("/signup", "name=gina&password=ab12cd&password2=ab12cd"), O::SignupCreated, "gina 가입");
        const std::string key = k.Do(Post("/launcher/login", "name=gina&password=ab12cd&remember=1"), O::LoginOk, "gina 열쇠").body;
        for (int i = 0; i < 5; ++i) k.Do(Post("/launcher/login", "name=gina&password=ab12zz"), O::LoginWrongPassword, "gina 틀림");
        k.Do(Post("/launcher/login", "name=gina&password=ab12cd"), O::LoginThrottled, "gina 잠김");
        k.Do(Post("/token", "name=gina&key=" + key), O::TokenOk, "잠겨도 열쇠로는 됨(이미 로그인한 사람은 안 막힘)");
    }

    // 부품 — 칸 수 · 잠긴 동안 돌아온 요청 · 지난 칸 지움 · 긴 쪽
    {
        CLoginThrottle t;
        const std::string name = "alice9";
        t.OnFail(nullptr, kIp, kNow);
        Check(t.AccountSlots() == 0 && t.AddressSlots() == 1, "nullptr — 계정 칸 없음 · 주소 칸 하나");
        for (int i = 0; i < 5; ++i) t.OnFail(&name, 1000 + i, kNow);
        Check(t.AccountSlots() == 1 && t.RetryAfterSec(name, 9, kNow) == 60, "다른 주소에서 5회 — 계정 잠김");
        t.OnFail(&name, 9, kNow + 1);   // 일꾼 넷이 같이 돌리던 해시가 잠근 뒤 돌아옴
        t.OnFail(&name, 9, kNow + 1);
        Check(t.RetryAfterSec(name, 9, kNow + 1) == 59, "잠긴 동안 돌아온 요청은 안 셈");
        t.OnFail(&name, 9, kNow + 60);
        Check(t.RetryAfterSec(name, 9, kNow + 60) == 300, "그 뒤 첫 실패 = 6회째(7회째로 뛰지 않음)");
        ThrottleScope sc = ThrottleScope::Address;
        Check(t.RetryAfterSec(name, 9, kNow + 60, &sc) == 300 && sc == ThrottleScope::Account, "계정이 길면 account");
        for (int i = 0; i < 19; ++i) t.OnFail(nullptr, 77, kNow + 60);
        Check(t.RetryAfterSec(name, 77, kNow + 60) == 300, "주소 19회 — 계정 잠금만");
        t.OnFail(nullptr, 77, kNow + 60);
        sc = ThrottleScope::Account;
        Check(t.RetryAfterSec(name, 77, kNow + 60, &sc) == 600 && sc == ThrottleScope::Address, "주소가 길면 address · 긴 쪽");
        Check(t.AddressRetryAfterSec(77, kNow + 60) == 600 && t.AddressRetryAfterSec(78, kNow + 60) == 0, "가입용 주소 검사");
        // 앞선 검사 없이 OnFail 만 와도 1시간이면 셈이 0 부터(부르는 쪽 순서에 안 기댐)
        {
            CLoginThrottle u;
            for (int i = 0; i < 4; ++i) u.OnFail(&name, 5, kNow);
            const std::string third = "carol";
            u.OnFail(&third, 7, kNow + 3599);   // 칸 지우기가 방금 돌아 다음 실패 때는 안 돎 — 셈 초기화 줄만 본다
            u.OnFail(&name, 5, kNow + 3600);
            Check(u.RetryAfterSec(name, 5, kNow + 3600) == 0, "검사 없이 1시간 뒤 실패 — 셈 1(잠금 없음)");
            for (int i = 0; i < 4; ++i) u.OnFail(&name, 6, kNow + 3599 + 3600 + i);
            Check(u.RetryAfterSec(name, 6, kNow + 3599 + 3600 + 3) == 60, "그 뒤 3599 초 안의 실패는 이어 셈 — 5회째 잠금");
        }
        const std::string other = "bob";
        t.OnFail(&other, 200, kNow + 60 + 3600);   // 지난 칸 지움 — 이 칸 둘만 남는다
        Check(t.AccountSlots() == 1 && t.AddressSlots() == 1, "1시간 지난 칸을 지움(메모리가 안 자람) — 계정 " + std::to_string(t.AccountSlots()) + " · 주소 " + std::to_string(t.AddressSlots()));
        t.OnSuccess(other, kNow + 60 + 3601);
        Check(t.AccountSlots() == 0, "성공 — 계정 칸 지움");
    }
}

// ★ 11-05 플랜1 — 암호 바꾸기(플랜 4절)
void TestChangePassword() {
    std::printf("⑧ 암호 바꾸기\n");
    using O = AuthOutcome;
    auto tokenOf = [](const std::string& name, const std::string& key) { return Post("/token", "name=" + name + "&key=" + key); };
    auto change = [](const std::string& cur, const std::string& next, const std::string& rest = "") {
        return Post("/launcher/password", "name=alice9&password=" + cur + "&new_password=" + next + rest);
    };
    Rig g;
    g.Do(Post("/signup", "name=alice9&password=ab12cd&password2=ab12cd"), O::SignupCreated, "alice9 가입");
    g.Do(Post("/signup", "name=bob&password=pw1234&password2=pw1234"), O::SignupCreated, "bob 가입");
    const std::string pc1 = g.Do(Post("/launcher/login", "name=alice9&password=ab12cd&device=PC1&remember=1"), O::LoginOk, "PC1 열쇠").body;
    const std::string pc2 = g.Do(Post("/launcher/login", "name=alice9&password=ab12cd&device=PC2&remember=1"), O::LoginOk, "PC2 열쇠").body;
    const std::string bobKey = g.Do(Post("/launcher/login", "name=bob&password=pw1234&device=BOB&remember=1"), O::LoginOk, "bob 열쇠").body;
    Check(g.db.KeyCount() == 3, "열쇠 셋(alice9 둘 · bob 하나)");

    // 규칙 밖 — 현재 암호가 맞아도 아무것도 안 바뀜 · 까닭 글이 둘째 줄
    const struct {
        const char* next;
        const char* text;
    } bad[] = {
        {"ab1", "암호는 4자 이상입니다."},
        {"abcdefghijklmno12", "암호는 16자까지입니다."},
        {"ab+12", "암호에 빈칸은 안 됩니다."},
        {"123456", "암호에 영문을 하나 이상 넣어 주세요."},
        {"abcdef", "암호에 숫자를 하나 이상 넣어 주세요."},
        {"", "암호는 4자 이상입니다."},
        {"ab12cd", "새 암호가 지금 암호와 같습니다."},
    };
    const unsigned hashesBefore = g.db.FindCalls();
    for (const auto& b : bad) {
        HttpResponse r = g.Do(change("ab12cd", b.next), O::PasswordBadInput, std::string("규칙 밖 ") + b.next);
        Check(r.status == 400 && Detail(r) == b.text && r.body.compare(0, 14, "NXL!bad_input\n") == 0, std::string("400 둘째 줄 — ") + b.text);
    }
    Check(g.db.FindCalls() == hashesBefore + 7, "규칙 밖 일곱 — 찾기는 한 번씩(현재 암호를 먼저 봄)");
    // 폼이 틀림 · 현재 암호 칸이 규칙 밖 · 이름 규칙 밖 — 바꾸기 전에 거절(둘째 줄 없음)
    for (const char* body : {"name=alice9&password=ab%zz&new_password=ef34gh", "name=alice9&password=ab1&new_password=ef34gh", "name=ab_c&password=ab12cd&new_password=ef34gh"}) {
        HttpResponse r = g.Do(Post("/launcher/password", body), O::PasswordBadInput, std::string("폼 — ") + body);
        Check(FailBody(r, 400, nox::LoginFail::BadInput), "400 까닭 한 줄");
    }
    // 없는 이름 · 옛 행
    g.db.AddLegacy("erin");
    Check(FailBody(g.Do(Post("/launcher/password", "name=nobody&password=ab12cd&new_password=ef34gh"), O::PasswordWrong, "없는 이름"), 401, nox::LoginFail::NoAccount), "없는 이름 401");
    Check(FailBody(g.Do(Post("/launcher/password", "name=erin&password=ab12cd&new_password=ef34gh"), O::PasswordWrong, "옛 행"), 401, nox::LoginFail::NoAccount), "옛 행 401");
    // 현재 암호 틀림 — 그대로
    Check(FailBody(g.Do(change("ab12zz", "ef34gh"), O::PasswordWrong, "현재 암호 틀림"), 401, nox::LoginFail::WrongPassword), "현재 암호 틀림 401");
    Check(g.db.KeyCount() == 3, "실패 뒤 열쇠 그대로");
    g.Do(tokenOf("alice9", pc1), O::TokenOk, "실패 뒤 PC1 열쇠 됨");
    // 저장 실패 — 그대로 · 찾기 실패 — 503
    g.db.SetFailChange(true);
    Check(FailBody(g.Do(change("ab12cd", "ef34gh"), O::PasswordFailed, "저장 실패"), 503, nox::LoginFail::ServerError), "저장 실패 503");
    g.db.SetFailChange(false);
    Check(g.db.KeyCount() == 3, "저장 실패 뒤 열쇠 그대로");
    g.Do(tokenOf("alice9", pc2), O::TokenOk, "저장 실패 뒤 PC2 열쇠 됨");
    g.db.SetFail(true);
    Check(FailBody(g.Do(change("ab12cd", "ef34gh"), O::PasswordFailed, "찾기 실패"), 503, nox::LoginFail::ServerError), "찾기 실패 503");
    g.db.SetFail(false);
    g.Do(Post("/launcher/login", "name=alice9&password=ab12cd&device=PC2&remember=1"), O::LoginOk, "저장 실패 뒤 옛 암호가 아직 맞음");

    // 바뀜 — 답은 새 열쇠 · 다른 기기 열쇠 전부 삭제 · 남의 열쇠는 그대로
    HttpResponse ok = g.Do(change("ab12cd", "ef34gh", "&device=PC3&remember=1"), O::PasswordChanged, "바뀜");
    Check(ok.status == 200 && ok.body.size() == 68 && ok.body.compare(0, 4, "NXK1") == 0 && Header(ok, "Cache-Control") == "no-store", "200 NXK1 · no-store");
    const std::string pc3 = ok.body;
    Check(g.db.KeyCount() == 2 && g.db.HasDevice("PC3") && !g.db.HasDevice("PC1") && !g.db.HasDevice("PC2") && g.db.HasDevice("BOB"), "alice9 열쇠는 새것 하나 · bob 그대로");
    Check(FailBody(g.Do(tokenOf("alice9", pc1), O::TokenBadKey, "옛 열쇠 PC1"), 401, nox::LoginFail::KeyInvalid), "옛 열쇠 401");
    g.Do(tokenOf("alice9", pc2), O::TokenBadKey, "옛 열쇠 PC2");
    g.Do(tokenOf("alice9", pc3), O::TokenOk, "새 열쇠로 토큰");
    g.Do(tokenOf("bob", bobKey), O::TokenOk, "bob 열쇠는 안 건드림");
    g.Do(Post("/launcher/login", "name=alice9&password=ab12cd"), O::LoginWrongPassword, "옛 암호 거절");
    g.Do(Post("/launcher/login", "name=alice9&password=ef34gh&device=PC4"), O::LoginOk, "새 암호로 로그인");
    g.Do(Post("/launcher/login", "name=bob&password=ef34gh"), O::LoginWrongPassword, "bob 암호는 안 바뀜");
    g.Do(Post("/launcher/login", "name=bob&password=pw1234"), O::LoginOk, "bob 암호 그대로");

    // 수명 — remember 없으면 12시간 · 16자 새 암호 됨
    HttpResponse ses = g.Do(change("ef34gh", "abcdefghijklmn12"), O::PasswordChanged, "16자 · 이번 실행만");
    g.clock += kKeyTtlSessionSec + 1;
    g.Do(tokenOf("alice9", ses.body), O::TokenExpired, "이번 실행만 열쇠 — 12시간 넘음");
    HttpResponse rem = g.Do(change("abcdefghijklmn12", "ef34gh", "&remember=1"), O::PasswordChanged, "자동 로그인 유지");
    g.clock += kKeyTtlSessionSec + 1;
    g.Do(tokenOf("alice9", rem.body), O::TokenOk, "자동 로그인 열쇠 — 12시간 넘어도 됨");

    // 시도 제한 — 로그인과 같은 셈(이 길로 대입 못 함) · 막힌 동안 DB 안 감 · 맞는 암호면 셈 초기화
    for (int i = 1; i <= 4; ++i) g.Do(change("zz99zz", "ab12cd"), O::PasswordWrong, "틀림");
    g.Do(change("ef34gh", "ef34gh"), O::PasswordBadInput, "맞는 암호 — 셈 0 부터");
    for (int i = 1; i <= 5; ++i) g.Do(change("zz99zz", "ab12cd"), O::PasswordWrong, "다시 틀림");
    const unsigned finds = g.db.FindCalls();
    HttpResponse lock = g.Do(change("ef34gh", "ab12cd"), O::LoginThrottled, "잠김 — 맞는 암호도");
    Check(lock.status == 429 && Detail(lock) == "60 account" && Header(lock, "Retry-After") == "60" && g.db.FindCalls() == finds, "429 · 둘째 줄 · DB 안 감");
    g.Do(Post("/launcher/login", "name=alice9&password=ef34gh"), O::LoginThrottled, "로그인 길도 잠김");
    // 없는 이름 · 옛 행도 주소 칸에 센다(이 길로 이름을 훑어도 막히게) — 갈래마다 주소를 따로 써서 한쪽만 빠져도 걸리게
    const struct {
        const char* name;
        uint32_t ip;
    } scans[] = {{"nobody", 0xC6336447u}, {"erin", 0xC6336448u}};
    for (const auto& s : scans) {
        HttpRequest r = Post("/launcher/password", std::string("name=") + s.name + "&password=ab12cd&new_password=ef34gh");
        r.peerIp = s.ip;
        for (uint32_t i = 1; i <= kAddressLimit; ++i) g.Do(r, O::PasswordWrong, std::string("주소 셈 — ") + s.name);
        HttpResponse locked = g.Do(r, O::LoginThrottled, std::string("주소 잠김 — ") + s.name);
        Check(locked.status == 429 && Detail(locked) == std::to_string(kAddressLockSec) + " address", std::string("주소 잠금 답 — ") + s.name);
    }
    // GET 은 405
    HttpRequest get = Post("/launcher/password", "");
    get.method = "GET";
    Check(g.Do(get, O::BadMethod, "GET").status == 405, "GET 405");
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
    TestThrottle();
    TestChangePassword();
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
