// auth_client_test.cpp — AuthClient.h 시험(DLL 과 같은 코드). test.bat 이 돌린다. 실제 nox_auth · 인증서 없이 가짜 HTTP 서버로.
//
// 확인할 것
//   ① 요청 — POST /login · 폼 · 이름 · 암호가 바이트 단위로 정확히 인코딩돼 서버가 같은 값으로 푼다(공백 · & · = · % · + · 한글 바이트)
//   ② 답 — 200 은 토큰 꼴(NXL1 + 16진 114자)만 · 400 401 503 은 아는 까닭(NXL!…)만 그대로. 상태와 본문이 안 맞거나 이상하면 NXL!server_error
//   ③ 못 닿음 — 연결 거절 · 응답 없음 · 연결 끊김 · HTTP 아닌 답 · 틀린 주소 · 평문 서버에 https → 늘 NXL!unreachable (한도 안에 돌아옴)
//   ④ 한도는 처음부터 끝까지 — 헤더는 한도 안에 오고 본문이 늦어 합이 한도를 넘으면 unreachable (단계마다 한도를 새로 주면 통과해 버린다)
//   ⑤ 넘겨주기(302)를 따라가지 않는다 — 서버가 요청을 한 번만 받는다
//   ⑥ 토큰 길이 상수가 서버의 kLoginTokenSize 와 같다
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

    srv.Stop();
    WSACleanup();
    std::printf("authclient checks=%d fail=%d\n", g_checks, g_fail);
    std::printf("%s\n", g_fail ? "AUTHCLIENT FAIL" : "AUTHCLIENT PASS");
    return g_fail ? 1 : 0;
}
