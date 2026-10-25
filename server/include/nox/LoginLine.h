// LoginLine.h — 로그인 줄 꼴(머리 파일만). 설계 Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」 nox_common 셋째 줄.
//
// 왜 머리 파일만인가 — 같은 꼴을 셋이 본다: 클라 DLL(`apgar` 자리에 넣음) · 로비(`apgar` 를 가름) · 로비 · 라우터 기록(암호 칸 가림).
//   DLL 은 서버 라이브러리를 링크하지 않으므로(`client_mod/wsock32_proxy/build.bat`) 함수를 여기 inline 으로 둔다 —
//   시험이 본 코드가 곧 DLL 코드(`LobbyLineFilter.h` 와 같은 꼴)
//
// `apgar` 자리(로그인 묶음 `apgar <인자> 0`)
//   · `NXL1<16진 114자>` — 로그인 토큰(`LoginToken.h` 57바이트) · DLL 이 로그인 서버에서 받은 것
//   · `NXL!<까닭>`       — DLL 이 로그인을 못 받음(까닭 = `LoginFailName`) · 로비가 까닭별 안내
//   · `None`             — 정품 손님 로그인(녹음 그대로)
//   · 그 밖              — 평문 암호(DLL 이 없거나 판이 낮은 클라)
// 로그인 서버(`/login`)의 답 본문도 앞의 두 꼴 그대로 — DLL 은 받은 글을 그대로 `apgar` 자리에 넣는다
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nox {

constexpr std::string_view kLoginTokenMark = "NXL1";
constexpr std::string_view kLoginFailMark = "NXL!";
constexpr std::string_view kGuestApgar = "None";
constexpr std::string_view kRedacted = "***";

// 로그인을 못 받은 까닭 — 로그인 서버가 답으로 · DLL 이 못 닿을 때 스스로 짓는다
enum class LoginFail : uint8_t {
    WrongPassword,   // 암호 틀림
    NoAccount,       // 그 이름의 계정 없음(가입 페이지로)
    ServerError,     // 로그인 서버엔 닿았는데 DB 가 안 됨 · 서명 실패
    Unreachable,     // DLL 이 로그인 서버에 못 닿음(한도 3초)
    BadInput,        // 이름 · 암호 규칙 밖
    kCount
};

// 줄에 싣는 이름(ASCII · 빈칸 없음)
inline const char* LoginFailName(LoginFail f) {
    switch (f) {
    case LoginFail::WrongPassword: return "wrong_password";
    case LoginFail::NoAccount: return "no_account";
    case LoginFail::ServerError: return "server_error";
    case LoginFail::Unreachable: return "unreachable";
    case LoginFail::BadInput: return "bad_input";
    case LoginFail::kCount: break;
    }
    return "?";
}

inline bool ParseLoginFail(std::string_view s, LoginFail& out) {
    for (int i = 0; i < static_cast<int>(LoginFail::kCount); ++i) {
        if (s == LoginFailName(static_cast<LoginFail>(i))) {
            out = static_cast<LoginFail>(i);
            return true;
        }
    }
    return false;
}

enum class ApgarKind : uint8_t {
    Missing,   // `apgar` 줄이 없었음(또는 인자가 빔)
    Token,     // `NXL1…` — payload = 16진 글자(길이는 부르는 쪽이 토큰 풀기로 본다)
    Fail,      // `NXL!…` — payload = 까닭 글
    Guest,     // `None`
    Plain,     // 그 밖 — 평문 암호
    kCount
};

inline ApgarKind ClassifyApgar(std::string_view arg, std::string& payload) {
    payload.clear();
    if (arg.empty()) return ApgarKind::Missing;
    if (arg.substr(0, kLoginTokenMark.size()) == kLoginTokenMark) {
        payload.assign(arg.substr(kLoginTokenMark.size()));
        return ApgarKind::Token;
    }
    if (arg.substr(0, kLoginFailMark.size()) == kLoginFailMark) {
        payload.assign(arg.substr(kLoginFailMark.size()));
        return ApgarKind::Fail;
    }
    if (arg == kGuestApgar) return ApgarKind::Guest;
    return ApgarKind::Plain;
}

namespace login_line_detail {

inline bool StartsWithNoCase(std::string_view s, std::string_view head) {
    if (s.size() < head.size()) return false;
    for (size_t i = 0; i < head.size(); ++i) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != head[i]) return false;
    }
    return true;
}

// 빈칸으로 나눈 n 번째(0 부터) 칸의 [시작, 끝) — 없으면 false. 줄 끝(CR · LF)에서 멈춘다
inline bool Field(std::string_view s, int n, size_t& b, size_t& e) {
    const size_t end = s.find_first_of("\r\n");
    if (end != std::string_view::npos) s = s.substr(0, end);
    size_t i = 0;
    for (int k = 0;; ++k) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) return false;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (k == n) {
            b = i;
            e = j;
            return true;
        }
        i = j;
    }
}

}  // namespace login_line_detail

// 기록에 찍기 전에 암호 칸을 가린다 — `apgar <인자>`(손님 `None` · 실패 표시 `NXL!<까닭>` 은 그대로 · 토큰은 `NXL1***`)
//   · `whereto <이름> <암호> …`(4005 라우터). 그 밖 줄은 그대로. 줄 끝 CRLF 가 붙어 있어도 된다
inline std::string RedactLoginLine(std::string_view line) {
    using namespace login_line_detail;
    size_t b = 0, e = 0;
    std::string out(line);
    if (StartsWithNoCase(line, "apgar ") && Field(line, 1, b, e)) {
        const std::string_view arg = line.substr(b, e - b);
        std::string payload;
        switch (ClassifyApgar(arg, payload)) {
        case ApgarKind::Guest:
        case ApgarKind::Fail: return out;
        case ApgarKind::Token: out.replace(b + kLoginTokenMark.size(), e - b - kLoginTokenMark.size(), kRedacted); return out;
        case ApgarKind::Missing:
        case ApgarKind::Plain:
        case ApgarKind::kCount: out.replace(b, e - b, kRedacted); return out;
        }
        return out;
    }
    if (StartsWithNoCase(line, "whereto ") && Field(line, 2, b, e)) out.replace(b, e - b, kRedacted);
    return out;
}

}  // namespace nox
