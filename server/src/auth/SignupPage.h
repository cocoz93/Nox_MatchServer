// SignupPage.h — 가입 페이지 화면(로그인 암호 조각 3 · Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」 nox_auth).
//
// 왜 서버 안 글인가 — 파일로 두면 실행 위치 · 설정 경로가 하나 더 생긴다. 화면 하나 · 결과 하나라 글 상수로 충분하다.
// 스크립트 없음 — 폼을 보내면 서버가 결과 화면을 통째로 돌려준다(검사는 서버가 한다 · 칸의 maxlength · pattern 은 치는 사람을 돕는 것뿐).
//   그래서 답에 콘텐츠 보안 정책(스크립트 금지 · 폼은 이 서버로만)을 싣는다(`CAuthService` 가 머리를 붙임)
// 사람이 친 이름을 화면에 되돌려 실을 때는 늘 `HtmlEscape` 를 거친다(이름 규칙을 지나지 못한 글도 되돌아오므로)
#pragma once

#include <string>

namespace nox::auth {

inline std::string HtmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c; break;
        }
    }
    return o;
}

namespace signup_page_detail {

inline const char* Head() {
    return R"(<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>녹스 MO 가입</title>
<style>
:root { --bg:#f4f1ea; --card:#fffdf8; --ink:#1f1b16; --sub:#6b6358; --line:#d8d0c2; --accent:#8a3b12; --accent-ink:#fff;
        --bad-bg:#fbe9e4; --bad:#9b2c13; --ok-bg:#e8f2e6; --ok:#2f5d2a; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#16130f; --card:#211d18; --ink:#efe9df; --sub:#a59c8e; --line:#3b342b; --accent:#e08a4f; --accent-ink:#1a1209;
          --bad-bg:#3a1d16; --bad:#f2a48e; --ok-bg:#1d2e1b; --ok:#a8d49f; }
}
* { box-sizing: border-box; }
body { margin:0; min-height:100vh; display:flex; align-items:center; justify-content:center; padding:24px 16px;
       background:var(--bg); color:var(--ink); font:16px/1.55 "Malgun Gothic","Apple SD Gothic Neo",system-ui,sans-serif;
       word-break:keep-all; overflow-wrap:anywhere; }
main { width:100%; max-width:380px; background:var(--card); border:1px solid var(--line); border-radius:10px; padding:28px 24px; }
h1 { margin:0 0 4px; font-size:22px; }
.lead { margin:0 0 20px; color:var(--sub); font-size:14px; }
label { display:block; margin:14px 0 4px; font-weight:600; font-size:14px; }
input { width:100%; padding:10px 12px; font:inherit; color:var(--ink); background:var(--bg); border:1px solid var(--line); border-radius:6px; }
input:focus { outline:2px solid var(--accent); outline-offset:1px; }
.hint { margin:4px 0 0; color:var(--sub); font-size:13px; }
button { width:100%; margin-top:22px; padding:11px; font:inherit; font-weight:700; color:var(--accent-ink); background:var(--accent);
         border:0; border-radius:6px; cursor:pointer; }
.msg { margin:0 0 16px; padding:10px 12px; border-radius:6px; font-size:14px; }
.bad { background:var(--bad-bg); color:var(--bad); }
.ok { background:var(--ok-bg); color:var(--ok); }
.name { font-weight:700; }
a { color:var(--accent); }
</style>
</head>
<body>
<main>
)";
}

inline const char* Tail() { return "</main>\n</body>\n</html>\n"; }

}  // namespace signup_page_detail

// 가입 양식 — `message` 가 있으면 위에 알림(`error` 면 빨강) · `name` 은 다시 채울 이름(되돌려 실으므로 걸러 냄)
inline std::string SignupFormPage(const std::string& message = "", bool error = false, const std::string& name = "") {
    std::string h = signup_page_detail::Head();
    h += "<h1>녹스 MO 가입</h1>\n<p class=\"lead\">여기서 만든 이름 · 암호로 게임 로그인 창에 들어갑니다.</p>\n";
    if (!message.empty()) h += std::string("<p class=\"msg ") + (error ? "bad" : "ok") + "\" role=\"alert\">" + HtmlEscape(message) + "</p>\n";
    h += "<form method=\"post\" action=\"/signup\" accept-charset=\"utf-8\" autocomplete=\"off\">\n"
         "<label for=\"name\">이름</label>\n"
         "<input id=\"name\" name=\"name\" required maxlength=\"9\" pattern=\"[A-Za-z0-9]{1,9}\" autocapitalize=\"off\" spellcheck=\"false\" value=\"" +
         HtmlEscape(name) +
         "\">\n"
         "<p class=\"hint\">영문 · 숫자 1~9자</p>\n"
         "<label for=\"password\">암호</label>\n"
         "<input id=\"password\" name=\"password\" type=\"password\" required minlength=\"4\" maxlength=\"8\">\n"
         "<p class=\"hint\">4~8자 · 영문과 숫자를 꼭 섞기 · 특수문자 써도 됨 · 빈칸 · 한글 안 됨</p>\n"
         "<label for=\"password2\">암호 확인</label>\n"
         "<input id=\"password2\" name=\"password2\" type=\"password\" required minlength=\"4\" maxlength=\"8\">\n"
         "<button type=\"submit\">가입</button>\n"
         "</form>\n";
    h += signup_page_detail::Tail();
    return h;
}

// 가입됨 — `claimed` = 예전 아이디만 계정에 암호를 넣음(판 기록이 그대로 이어짐)
inline std::string SignupDonePage(const std::string& name, bool claimed) {
    std::string h = signup_page_detail::Head();
    h += "<h1>가입됨</h1>\n<p class=\"msg ok\" role=\"status\"><span class=\"name\">" + HtmlEscape(name) + "</span> " +
         (claimed ? "— 예전 이 이름의 기록에 암호를 붙였습니다." : "— 새 계정을 만들었습니다.") +
         "</p>\n<p>게임 로그인 창에 이 이름과 암호를 넣으면 들어갑니다.</p>\n<p><a href=\"/signup\">다른 계정 만들기</a></p>\n";
    h += signup_page_detail::Tail();
    return h;
}

}  // namespace nox::auth
