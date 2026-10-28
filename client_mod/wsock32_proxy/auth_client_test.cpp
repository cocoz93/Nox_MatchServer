// auth_client_test.cpp — AuthClient.h 시험(DLL 과 같은 코드). test.bat 이 돌린다. 실제 nox_auth · 인증서 없이 가짜 HTTP 서버로.
//
// 확인할 것
//   ① 요청 — POST /login · 폼 · 이름 · 암호가 바이트 단위로 정확히 인코딩돼 서버가 같은 값으로 푼다(공백 · & · = · % · + · 한글 바이트)
//   ② 답 — 200 은 토큰 꼴(NXL1 + 16진 114자)만 · 400 401 503 은 아는 까닭(NXL!…)만 그대로. 상태와 본문이 안 맞거나 이상하면 NXL!server_error
//   ③ 못 닿음 — 연결 거절 · 응답 없음 · 연결 끊김 · HTTP 아닌 답 · 틀린 주소 · 평문 서버에 https → 늘 NXL!unreachable (한도 안에 돌아옴)
//   ④ 한도는 처음부터 끝까지 — 헤더는 한도 안에 오고 본문이 늦어 합이 한도를 넘으면 unreachable (단계마다 한도를 새로 주면 통과해 버린다)
//   ⑤ 넘겨주기(302)를 따라가지 않는다 — 서버가 요청을 한 번만 받는다
//   ⑥ 토큰 길이 상수가 서버의 kLoginTokenSize 와 같다
//   ⑦ ★ 10-28 지문 고정 — 평문 주소 · 꼴이 틀린 지문은 붙지 않음(요청 0) · SHA-256 · 대문자 지문
//   ⑧ ★ 10-28 실제 nox_auth 에 지문 고정(NOX_AUTH_LIVE_PIN 이 있고 8443 이 떠 있을 때만 — 아니면 건너뜀)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AuthClient.h"
#include "nox/LoginToken.h"   // kLoginTokenSize 만 쓴다(머리 파일만 — 링크 안 함)

#pragma comment(lib, "ws2_32.lib")

static_assert(authclient::kTokenHexLen == 2 * nox::kLoginTokenSize, "DLL 의 토큰 길이가 서버와 다름");

static int g_fail = 0;
static int g_checks = 0;

static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

enum class Act { Reply, Hang, CloseNow, Garbage, Slow, Dribble };

struct Script {
    Act act = Act::Reply;
    int status = 200;
    std::string body;
    std::string extraHeaders;   // "Name: v\r\n" 들
    int delay1 = 0, delay2 = 0;
};

struct Seen {
    int count = 0;
    std::string method, path, contentType, body;
};

static std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

static std::string Decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && authclient::detail::IsHex(s[i + 1]) && authclient::detail::IsHex(s[i + 2])) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string FormGet(const std::string& body, const std::string& key)
{
    size_t i = 0;
    while (i <= body.size()) {
        size_t e = body.find('&', i);
        if (e == std::string::npos) e = body.size();
        const std::string kv = body.substr(i, e - i);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && Decode(kv.substr(0, eq)) == key) return Decode(kv.substr(eq + 1));
        i = e + 1;
    }
    return "<없음>";
}

// 가짜 로그인 서버 — 접속마다 스레드 하나. 한 요청을 읽고 정해 둔 대로 답한다
class FakeServer {
public:
    bool Start()
    {
        ls_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls_, 16) != 0) return false;
        int len = sizeof(a);
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        acceptor_ = std::thread([this] {
            for (;;) {
                SOCKET c = accept(ls_, NULL, NULL);
                if (c == INVALID_SOCKET) break;
                Script sc;
                {
                    std::lock_guard<std::mutex> g(mu_);
                    sc = script_;
                }
                workers_.emplace_back([this, c, sc] { Handle(c, sc); });
            }
        });
        return true;
    }

    void Stop()
    {
        stop_ = true;
        closesocket(ls_);
        if (acceptor_.joinable()) acceptor_.join();
        for (std::thread& t : workers_) t.join();
    }

    unsigned short Port() const { return port_; }

    void Set(const Script& s)
    {
        std::lock_guard<std::mutex> g(mu_);
        script_ = s;
        seen_ = Seen();
    }

    Seen Last()
    {
        std::lock_guard<std::mutex> g(mu_);
        return seen_;
    }

private:
    void Handle(SOCKET c, const Script& sc)
    {
        DWORD rt = 1000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rt), sizeof(rt));
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int r = recv(c, buf, sizeof(buf), 0);
            if (r <= 0) { closesocket(c); return; }   // 요청이 안 왔다(평문 서버에 TLS 인사가 온 경우 등)
            req.append(buf, static_cast<size_t>(r));
        }
        const size_t he = req.find("\r\n\r\n");
        const std::string head = req.substr(0, he);
        std::string body = req.substr(he + 4);
        size_t want = 0;
        const std::string lh = Lower(head);
        const size_t cl = lh.find("content-length:");
        if (cl != std::string::npos) want = static_cast<size_t>(std::atoi(head.c_str() + cl + 15));
        while (body.size() < want) {
            const int r = recv(c, buf, sizeof(buf), 0);
            if (r <= 0) break;
            body.append(buf, static_cast<size_t>(r));
        }
        Seen s;
        const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        s.method = head.substr(0, sp1);
        s.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t ct = lh.find("content-type:");
        if (ct != std::string::npos) {
            size_t e = head.find("\r\n", ct);
            if (e == std::string::npos) e = head.size();
            s.contentType = head.substr(ct + 13, e - ct - 13);
            while (!s.contentType.empty() && s.contentType[0] == ' ') s.contentType.erase(0, 1);
        }
        s.body = body;
        {
            std::lock_guard<std::mutex> g(mu_);
            s.count = seen_.count + 1;
            seen_ = s;
        }
        switch (sc.act) {
        case Act::Reply: SendReply(c, sc, sc.body); break;
        case Act::Slow: Sleep(static_cast<DWORD>(sc.delay1)); SendReply(c, sc, sc.body); break;
        case Act::Dribble: {
            Sleep(static_cast<DWORD>(sc.delay1));
            const std::string h = HeaderOf(sc, sc.body.size());
            send(c, h.data(), static_cast<int>(h.size()), 0);
            Sleep(static_cast<DWORD>(sc.delay2));
            send(c, sc.body.data(), static_cast<int>(sc.body.size()), 0);
            break;
        }
        case Act::Garbage: {
            const char g[] = "this is not http\r\n\r\n";
            send(c, g, static_cast<int>(sizeof(g) - 1), 0);
            break;
        }
        case Act::CloseNow: break;
        case Act::Hang:
            while (!stop_) {   // 답 없이 붙들고 있다가 클라가 끊거나 시험이 끝나면 놓는다
                const int r = recv(c, buf, sizeof(buf), 0);
                if (r == 0 || (r < 0 && WSAGetLastError() != WSAETIMEDOUT)) break;   // 끊김(RST 포함)이면 놓는다 — 안 그러면 빈 돌기
            }
            break;
        }
        closesocket(c);
    }

    static std::string HeaderOf(const Script& sc, size_t bodyLen)
    {
        return "HTTP/1.1 " + std::to_string(sc.status) + " X\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(bodyLen) +
               "\r\nConnection: close\r\n" + sc.extraHeaders + "\r\n";
    }

    static void SendReply(SOCKET c, const Script& sc, const std::string& body)
    {
        const std::string all = HeaderOf(sc, body.size()) + body;
        send(c, all.data(), static_cast<int>(all.size()), 0);
    }

    SOCKET ls_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{ false };
    std::mutex mu_;
    Script script_;
    Seen seen_;
};

static std::wstring UrlFor(const char* scheme, unsigned short port, const char* path = "/login")
{
    char u[128];
    std::snprintf(u, sizeof(u), "%s://127.0.0.1:%u%s", scheme, static_cast<unsigned>(port), path);
    return std::wstring(u, u + std::strlen(u));
}

static std::string Tok(char fill = 'a')
{
    std::string t = "NXL1";
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < authclient::kTokenHexLen; ++i) t.push_back(i % 17 == 0 ? fill : hex[i % 16]);
    return t;
}

static const std::string kServerError = "NXL!server_error";
static const std::string kUnreachable = "NXL!unreachable";

int main()
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    FakeServer srv;
    Check(srv.Start(), "가짜 서버 뜸");
    const DWORD kTo = 1500;

    // ① 요청 꼴 · 인코딩
    {
        const std::string tok = Tok();
        Script s;
        s.body = tok;
        srv.Set(s);
        const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo);
        const Seen seen = srv.Last();
        Check(r.line == tok && r.status == 200 && r.error == 0, "① 성공 — 토큰 그대로 · 200");
        Check(seen.count == 1 && seen.method == "POST" && seen.path == "/login", "① POST /login 한 번");
        Check(seen.contentType.compare(0, 33, "application/x-www-form-urlencoded") == 0, "① 폼 꼴");
        Check(seen.body == "name=wn9457&password=ab12cd", "① 본문이 정확히 name=…&password=…");
    }
    {
        Script s;
        s.body = Tok('b');
        srv.Set(s);
        const std::string name = "n m", pw = std::string("a&b=c%1+ x") + "\xb0\xa1" + "~-_.";
        const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), name, pw, kTo);
        const Seen seen = srv.Last();
        Check(r.line == s.body, "① 특수 글자 — 성공");
        Check(FormGet(seen.body, "name") == name && FormGet(seen.body, "password") == pw, "① 공백 · & · = · % · + · 한글 바이트가 그대로 풀림");
        Check(seen.body == "name=n%20m&password=a%26b%3Dc%251%2B%20x%B0%A1~-_.", "① 인코딩 글자 그대로(영숫자와 -_.~ 만 안 바꿈)");
        Check(std::count(seen.body.begin(), seen.body.end(), '&') == 1, "① 칸 경계 & 는 하나뿐");
    }

    // ② 답 가르기
    {
        struct C { int status; const char* body; const char* want; const char* what; };
        const C cases[] = {
            { 401, "NXL!wrong_password", "NXL!wrong_password", "401 암호 틀림" },
            { 401, "NXL!no_account", "NXL!no_account", "401 계정 없음" },
            { 400, "NXL!bad_input", "NXL!bad_input", "400 규칙 밖" },
            { 503, "NXL!server_error", "NXL!server_error", "503 서버 오류" },
            { 401, "NXL!wrong_password\r\n", "NXL!wrong_password", "줄바꿈이 붙어 와도 뗌" },
            { 200, "hello", "NXL!server_error", "200 인데 토큰 아님" },
            { 200, "", "NXL!server_error", "200 빈 본문" },
            { 200, "NXL!wrong_password", "NXL!server_error", "200 인데 실패 표시(상태와 본문 불일치)" },
            { 401, "NXL!bogus_reason", "NXL!server_error", "모르는 까닭" },
            { 401, "", "NXL!server_error", "401 빈 본문" },
            { 500, "NXL!wrong_password", "NXL!server_error", "500 은 본문을 안 믿음" },
            { 404, "", "NXL!server_error", "404" },
        };
        for (const C& c : cases) {
            Script s;
            s.status = c.status;
            s.body = c.body;
            srv.Set(s);
            const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo);
            Check(r.line == c.want && r.status == c.status, c.what);
        }
        // 토큰 꼴이 한 글자라도 틀리면 서버 오류
        const std::string good = Tok();
        const std::string bads[] = { good.substr(0, good.size() - 1), good + "a", good.substr(0, good.size() - 1) + "g", "NXL2" + good.substr(4),
                                     "nxl1" + good.substr(4), std::string(2000, 'a') };
        for (const std::string& b : bads) {
            Script s;
            s.body = b;
            srv.Set(s);
            const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo);
            Check(r.line == kServerError, "② 토큰 꼴이 틀림(113 · 115 · 비16진 · 판 · 대소문자 · 너무 김) → server_error");
        }
        // 대문자 16진은 16진이다
        Script up;
        up.body = "NXL1" + std::string(authclient::kTokenHexLen, 'A');
        srv.Set(up);
        Check(authclient::Login(UrlFor("http", srv.Port()), "a1", "ab12", kTo).line == up.body, "② 대문자 16진도 토큰");
    }

    // ⑤ 넘겨주기 안 따라감
    {
        Script s;
        s.status = 302;
        s.extraHeaders = "Location: http://127.0.0.1:" + std::to_string(srv.Port()) + "/elsewhere\r\n";
        srv.Set(s);
        const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo);
        const Seen seen = srv.Last();
        Check(r.line == kServerError && r.status == 302 && seen.count == 1 && seen.path == "/login", "⑤ 302 를 안 따라감 — 요청 한 번뿐");
    }

    // ③ 못 닿음
    {
        SOCKET tmp = socket(AF_INET, SOCK_STREAM, 0);   // 닫힌 포트 하나 얻기
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(tmp, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        int len = sizeof(a);
        getsockname(tmp, reinterpret_cast<sockaddr*>(&a), &len);
        const unsigned short closedPort = ntohs(a.sin_port);
        closesocket(tmp);
        const authclient::Result r = authclient::Login(UrlFor("http", closedPort), "wn9457", "ab12cd", kTo);
        Check(r.line == kUnreachable && r.status == 0 && r.error != 0 && r.ms <= kTo + 800, "③ 연결 거절 → unreachable · 한도 안");
    }
    {
        Script s;
        s.act = Act::Hang;
        srv.Set(s);
        const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", 1000);
        Check(r.line == kUnreachable && r.status == 0 && r.ms >= 900 && r.ms <= 1000 + 800, "③ 응답 없음 → 한도(1초)에 unreachable");    }
    {
        Script s;
        s.act = Act::CloseNow;
        srv.Set(s);
        Check(authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo).line == kUnreachable, "③ 답 없이 끊김 → unreachable");
        s.act = Act::Garbage;
        srv.Set(s);
        Check(authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo).line == kUnreachable, "③ HTTP 아닌 답 → unreachable");
    }
    {
        Script s;
        s.body = Tok();
        srv.Set(s);
        const authclient::Result r = authclient::Login(UrlFor("https", srv.Port()), "wn9457", "ab12cd", kTo);
        Check(r.line == kUnreachable && r.status == 0 && srv.Last().count == 0, "③ 평문 서버에 https → 인사가 안 끝나 unreachable · 요청은 안 감");
        Check(authclient::Login(L"not a url", "a1", "ab12", kTo).line == kUnreachable, "③ 틀린 주소");
        Check(authclient::Login(UrlFor("ftp", srv.Port()), "a1", "ab12", kTo).line == kUnreachable, "③ http · https 아닌 주소");
    }

    // ④ 한도는 처음부터 끝까지
    {
        Script s;
        s.act = Act::Dribble;
        s.body = Tok();
        s.delay1 = 700;   // 헤더가 한도(1초) 안에 오고
        s.delay2 = 700;   // 본문이 늦어 합 1.4초
        srv.Set(s);
        const authclient::Result r = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", 1000);
        Check(r.line == kUnreachable && r.ms <= 1000 + 500, "④ 헤더 700ms + 본문 700ms → 한도(1초)를 넘으면 unreachable · 1.4초까지 안 기다림");        s.act = Act::Slow;
        s.delay1 = 600;
        srv.Set(s);
        const authclient::Result ok = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo);
        Check(ok.line == s.body && ok.ms >= 550, "④ 600ms 늦은 답은 한도 안이면 받음");
    }

    // ⑦ ★ 10-28 지문 고정 — 붙기 전에 막히는 갈래(가짜 서버가 요청을 하나도 못 받아야 한다) · SHA-256 · 지문 꼴
    {
        const std::string pin(64, 'a');
        Script s;
        s.body = Tok();
        srv.Set(s);
        const authclient::Result plain = authclient::Login(UrlFor("http", srv.Port()), "wn9457", "ab12cd", kTo, pin);
        Check(plain.line == kUnreachable && plain.error == ERROR_WINHTTP_SECURE_FAILURE && srv.Last().count == 0,
              "⑦ 지문을 줬는데 평문(http) 주소 → 붙지 않음 · 요청 0");
        const char* const bad[] = {"", "a", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" /*63*/, "?"};
        int refused = 0;
        for (const char* b : bad) {
            if (!*b) continue;   // 빈 지문 = 옛 길(위 ①~⑤ 가 그 길)
            const authclient::Result r = authclient::Login(UrlFor("https", srv.Port()), "wn9457", "ab12cd", kTo, b);
            refused += (r.line == kUnreachable && r.error == ERROR_INVALID_PARAMETER) ? 1 : 0;
        }
        std::string g = pin;
        g[10] = 'g';
        const authclient::Result r = authclient::Login(UrlFor("https", srv.Port()), "wn9457", "ab12cd", kTo, g);
        refused += (r.line == kUnreachable && r.error == ERROR_INVALID_PARAMETER) ? 1 : 0;
        Check(refused == 4 && srv.Last().count == 0, "⑦ 꼴이 틀린 지문(1자 · 63자 · ? · 16진 아닌 글자) → 옛 길로 안 물러서고 unreachable · 요청 0");
        unsigned char p[authclient::kPinBytes];
        const bool up = authclient::detail::ParsePin(std::string(64, 'F'), p) && p[0] == 0xff && p[31] == 0xff;
        Check(up, "⑦ 대문자 16진 지문도 받음");
        unsigned char h[authclient::kPinBytes];
        const unsigned char abc[] = {'a', 'b', 'c'};
        unsigned char want[authclient::kPinBytes];
        authclient::detail::ParsePin("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
        Check(authclient::detail::Sha256(abc, 3, h) && memcmp(h, want, sizeof(h)) == 0, "⑦ SHA-256(\"abc\") = FIPS 180-2 예시 값");
    }

    // ⑧ ★ 10-28 실제 로그인 서버(nox_auth · 자작 인증서)에 지문 고정 — test.bat 이 tools\auth_pin.ps1 로 NOX_AUTH_LIVE_PIN 을 넣는다.
    //   127.0.0.2 는 인증서 이름(localhost · 127.0.0.1)에 없다 = 다른 PC 가 공인 IP 로 붙는 꼴. 지문 없이는 막히고 지문으로만 통과해야 한다.
    //   계정 없는 이름으로 묻는다 — 401 no_account 가 오면 본문(이름 · 암호)이 서버에 닿은 것
    {
        char live[80] = {};
        const DWORD n = GetEnvironmentVariableA("NOX_AUTH_LIVE_PIN", live, sizeof(live));
        const std::string pin = n > 0 && n < sizeof(live) ? std::string(live, n) : std::string();
        const std::wstring here = L"https://127.0.0.1:8443/login", other = L"https://127.0.0.2:8443/login";
        const std::string kNoAccount = "NXL!no_account";
        const authclient::Result probe = pin.empty() ? authclient::Result() : authclient::Login(here, "zz9pin", "ab12cd", kTo, pin);
        if (pin.empty() || probe.error == ERROR_WINHTTP_CANNOT_CONNECT) {
            std::printf("  ⑧ 실제 로그인 서버 — 건너뜀(%s)\n", pin.empty() ? "NOX_AUTH_LIVE_PIN 없음 · 사람 1회 전" : "8443 안 떠 있음");
        } else {
            Check(probe.line == kNoAccount && probe.status == 401, "⑧ 맞는 지문 · 127.0.0.1 → 본문이 닿음(401 no_account)");
            const authclient::Result o = authclient::Login(other, "zz9pin", "ab12cd", kTo, pin);
            Check(o.line == kNoAccount && o.status == 401, "⑧ 맞는 지문 · 인증서 이름에 없는 주소(127.0.0.2) → 통과(다른 PC 꼴)");
            const authclient::Result noPin = authclient::Login(other, "zz9pin", "ab12cd", kTo);
            Check(noPin.line == kUnreachable && noPin.status == 0, "⑧ 지문 없이 인증서 이름에 없는 주소 → 윈도 검사에 막힘(옛 길 그대로)");
            std::string wrong = pin;
            wrong[0] = wrong[0] == '0' ? '1' : '0';
            const authclient::Result w = authclient::Login(here, "zz9pin", "ab12cd", kTo, wrong);
            Check(w.line == kUnreachable && w.status == 0 && w.error == ERROR_WINHTTP_SECURE_INVALID_CERT,
                  "⑧ 틀린 지문 → 본문 쓰기 전에 멈춤(SECURE_INVALID_CERT · 상태 0)");
        }
    }

    srv.Stop();
    WSACleanup();
    std::printf("authclient checks=%d fail=%d\n", g_checks, g_fail);
    std::printf("%s\n", g_fail ? "AUTHCLIENT FAIL" : "AUTHCLIENT PASS");
    return g_fail ? 1 : 0;
}
