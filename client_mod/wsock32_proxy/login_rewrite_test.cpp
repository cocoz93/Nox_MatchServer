// login_rewrite_test.cpp — LoginRewriter.h 반복 시험(DLL 과 같은 코드). test.bat 이 돌린다.
//
// 확인할 것: 게임이 로그인 묶음을 어디서 자르든(1바이트 조각까지) 그리고 진짜 send 가 얼마씩 받든(일부만 · 막힘 섞임)
//   ① 선에 나간 바이트 = 기대값(바꿀 줄만 바뀌고 나머지는 순서까지 그대로) — 원래 암호는 선에 한 글자도 없다
//   ② onLogin 은 (이름, 암호) 로 정확히 한 번 · 손님(`None`)과 로그인 뒤 같은 꼴 줄에는 안 불린다
//   ③ 끝에 쥔 것도 쌓인 것도 없다(줄 끝이 온 묶음)
//   ④ 라우터 `whereto` 암호 칸 → `x` · 손님 줄과 그 뒤 줄은 그대로
//   ⑤ 바꿀 줄과 겹치지 않는 덜 온 줄은 쥐지 않고 바로 내보낸다 · 바꿀 줄의 덜 온 몫은 쥔다 · 너무 길면 안 쥔다
//   ⑥ onLogin 이 못 쓸 글(빈 글 · 빈칸)을 돌려주면 `NXL!server_error`
//   ⑦ 진짜 send 가 진짜 오류면 Flush 가 -1 을 돌리고 쥔 것을 버린다
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "LoginRewriter.h"

static int g_fail = 0;

static void Check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

static void Feed(CLoginRewriter& rw, const std::string& s) { rw.Feed(s.data(), static_cast<int>(s.size())); }   // 글자 수를 손으로 넘기지 않는다

static const std::string kTok = "NXL1" + std::string(114, 'c');   // 로그인 서버가 돌려주는 토큰 글(꼴만 같으면 됨)

// 가짜 소켓 — sockMode: 0 한 번에 다 받음 · k>0 한 번에 최대 k바이트 · k<0 홀수 번째 부름은 최대 -k바이트, 짝수 번째 부름은 막힘
struct FakeSock {
    std::string wire;
    int sockMode = 0;
    int calls = 0;
    bool blocked = false;
    int Send(const char* b, int n)
    {
        ++calls;
        if (sockMode < 0 && (calls % 2) == 0) { blocked = true; return -1; }
        blocked = false;
        const int cap = sockMode == 0 ? n : (sockMode > 0 ? sockMode : -sockMode);
        const int take = std::min(n, cap);
        wire.append(b, static_cast<size_t>(take));
        return take;
    }
};

struct Case {
    const char* label;
    CLoginRewriter::Mode mode;
    std::string in, want;
    int logins, guests, router;
    std::string gotName, gotPw;   // logins == 1 일 때 onLogin 이 받아야 할 값
};

static bool RunOne(const Case& c, const std::vector<size_t>& cuts, int sockMode)
{
    CLoginRewriter rw(c.mode);
    std::string seenName, seenPw;
    int calls = 0;
    rw.onLogin = [&](const std::string& n, const std::string& p) {
        seenName = n;
        seenPw = p;
        ++calls;
        return kTok;
    };
    FakeSock s;
    s.sockMode = sockMode;
    auto flush = [&] { return rw.Flush([&](const char* b, int n) { return s.Send(b, n); }, [&] { return s.blocked; }); };
    size_t prev = 0;
    std::vector<size_t> ends = cuts;
    ends.push_back(c.in.size());
    for (const size_t e : ends) {
        if (e <= prev || e > c.in.size()) continue;
        rw.Feed(c.in.data() + prev, static_cast<int>(e - prev));
        if (flush() != 0) { std::printf("[%s] Flush 가 오류를 돌림 sock=%d\n", c.label, sockMode); return false; }
        prev = e;
    }
    for (int guard = 0; guard < 100000 && !rw.Pending().empty(); ++guard) flush();   // recv 훅이 부르는 몫 — 막힘이 풀릴 때까지
    bool ok = true;
    if (s.wire != c.want) { std::printf("[%s] ① 선에 나간 바이트가 다름 sock=%d (%zu / %zu)\n", c.label, sockMode, s.wire.size(), c.want.size()); ok = false; }
    if (calls != c.logins) { std::printf("[%s] ② onLogin %d번 (기대 %d)\n", c.label, calls, c.logins); ok = false; }
    if (c.logins == 1 && (seenName != c.gotName || seenPw != c.gotPw)) {
        std::printf("[%s] ② onLogin 인자 [%s] [%s]\n", c.label, seenName.c_str(), seenPw.c_str());
        ok = false;
    }
    if (rw.Logins() != c.logins || rw.Guests() != c.guests || rw.RouterRewrites() != c.router) {
        std::printf("[%s] 셈 로그인 %d 손님 %d 라우터 %d (기대 %d %d %d)\n", c.label, rw.Logins(), rw.Guests(), rw.RouterRewrites(), c.logins, c.guests, c.router);
        ok = false;
    }
    if (!rw.Pending().empty() || rw.HeldBytes() != 0) { std::printf("[%s] ③ 쌓인 것 %zu · 쥔 것 %zu\n", c.label, rw.Pending().size(), rw.HeldBytes()); ok = false; }
    return ok;
}

static std::vector<Case> Cases()
{
    using M = CLoginRewriter::Mode;
    std::vector<Case> v;
    // NOOP 줄은 가운데에 바꿀 줄 머리와 같은 글이 낀 줄 — 조각이 거기서 잘려 나머지가 줄 머리처럼 보여도 건드리면 안 된다
    const std::string head = "CVERS 11020 9472\r\nPASS supersecret\r\nNOOP x apgar pw99 0\r\nNOOP x nick bob\r\nNOOP x user q\r\nNICK wn9457\r\n";
    const std::string tail =
        "SERIAL 00000000000000000000\r\n"
        "USER UserName HostName irc.westwood.com :RealName\r\n"
        "verchk 32512 720916\r\n"
        "SETOPT 17,33\r\n"
        "NICK later\r\napgar zz99 0\r\nJOIN #Lob_37_0 zot0blat\r\n";   // 로그인 뒤 — 같은 꼴 줄도 안 건드림
    v.push_back({ "로비", M::Lobby, head + "apgar ab12cd 0\r\n" + tail, head + "apgar " + kTok + " 0\r\n" + tail, 1, 0, 0, "wn9457", "ab12cd" });
    v.push_back({ "로비 손님", M::Lobby, head + "apgar None 0\r\n" + tail, head + "apgar None 0\r\n" + tail, 0, 1, 0, "", "" });
    v.push_back({ "로비 대소문자", M::Lobby, "nick Wn1\r\nAPGAR pw12 0\r\nuser x\r\nAPGAR later 0\r\n",
                  "nick Wn1\r\nAPGAR " + kTok + " 0\r\nuser x\r\nAPGAR later 0\r\n", 1, 0, 0, "Wn1", "pw12" });
    v.push_back({ "로비 NICK 없음", M::Lobby, "apgar pw12 0\r\nUSER x\r\n", "apgar " + kTok + " 0\r\nUSER x\r\n", 1, 0, 0, "", "pw12" });
    v.push_back({ "로비 LF 만", M::Lobby, "NICK a1\napgar pw12 0\nUSER x\n", "NICK a1\napgar " + kTok + " 0\nUSER x\n", 1, 0, 0, "a1", "pw12" });
    const std::string rIn = "verchk 32512 65556\r\nverchk 9472 65538\r\nlobcount 9472\r\nNOOP x whereto a b c\r\n";
    v.push_back({ "라우터", M::Router, rIn + "whereto wn9457 ab12cd 9472 65538 00000000000000000000\r\nQUIT\r\nwhereto zz pw99 1\r\n",
                  rIn + "whereto wn9457 x 9472 65538 00000000000000000000\r\nQUIT\r\nwhereto zz pw99 1\r\n", 0, 0, 1, "", "" });
    v.push_back({ "라우터 손님", M::Router, rIn + "whereto   9472 65538 00000000000000000000\r\nQUIT\r\n",
                  rIn + "whereto   9472 65538 00000000000000000000\r\nQUIT\r\n", 0, 0, 0, "", "" });
    return v;
}

int main()
{
    const std::vector<Case> cases = Cases();
    const int sockModes[] = { 0, 1, 3, 7, 64, -1, -3, -9 };
    long long runs = 0;
    for (const Case& c : cases) {
        // 기대값이 정말 바뀐 줄을 담는가(시험이 아무것도 안 보는 것을 막는다)
        if (c.logins == 1) Check(c.want.find(c.gotPw) == std::string::npos || c.gotPw.empty(), "기대값에 원래 암호가 없음");
        if (c.router == 1) Check(c.want.find("ab12cd") == std::string::npos, "라우터 기대값에 원래 암호가 없음");
        for (const int sm : sockModes) {
            for (size_t k = 1; k <= c.in.size(); ++k) {   // 모든 고정 조각 크기
                std::vector<size_t> cuts;
                for (size_t p = k; p < c.in.size(); p += k) cuts.push_back(p);
                ++runs;
                if (!RunOne(c, cuts, sm)) ++g_fail;
            }
            for (size_t p = 1; p < c.in.size(); ++p) {   // 모든 한 자리 자르기(두 조각)
                ++runs;
                if (!RunOne(c, { p }, sm)) ++g_fail;
            }
        }
        std::mt19937 rng(20261026);   // 무작위 자르기
        for (int k = 0; k < 3000; ++k) {
            std::vector<size_t> cuts;
            for (size_t p = 1; p < c.in.size(); ++p) if (rng() % 6 == 0) cuts.push_back(p);
            ++runs;
            if (!RunOne(c, cuts, sockModes[rng() % (sizeof(sockModes) / sizeof(sockModes[0]))])) ++g_fail;
        }
    }

    // ⑤ 덜 온 줄 — 바꿀 줄과 겹치지 않으면 바로 나가고, 겹치면 쥔다
    {
        CLoginRewriter rw(CLoginRewriter::Mode::Lobby);
        rw.onLogin = [](const std::string&, const std::string&) { return kTok; };
        Feed(rw, "CVERS 11020 94");
        Check(rw.Pending() == "CVERS 11020 94" && rw.HeldBytes() == 0, "⑤ 겹치지 않는 덜 온 줄은 바로 나감");
        Feed(rw, "\r\nPASS sup");
        Check(rw.Pending() == "CVERS 11020 94\r\nPASS sup" && rw.HeldBytes() == 0, "⑤ 줄 끝이 오면 다음 줄 덜 온 몫도 바로");
        Feed(rw, "ersecret\r\napg");
        Check(rw.Pending() == "CVERS 11020 94\r\nPASS supersecret\r\n" && rw.HeldBytes() == 3, "⑤ 앞 줄의 나머지는 통과 · apgar 머리의 앞부분은 쥠");
        Feed(rw, "ar ab12cd 0\r");
        Check(rw.Pending() == "CVERS 11020 94\r\nPASS supersecret\r\n" && rw.HeldBytes() == 15, "⑤ 바꿀 줄은 줄 끝까지 쥠(암호가 나가지 않음)");
        Feed(rw, "\n");
        Check(rw.Pending() == "CVERS 11020 94\r\nPASS supersecret\r\napgar " + kTok + " 0\r\n" && rw.HeldBytes() == 0, "⑤ 줄 끝이 오면 바꿔서 내보냄");
    }
    {   // 줄 끝 없이 너무 긴 글 — 바꿀 줄이 아니다(쥐지 않고 통과 · 불리지 않음)
        CLoginRewriter rw(CLoginRewriter::Mode::Lobby);
        int calls = 0;
        rw.onLogin = [&](const std::string&, const std::string&) { ++calls; return kTok; };
        const std::string longLine = "apgar " + std::string(300, 'x');
        rw.Feed(longLine.data(), static_cast<int>(longLine.size()));
        Check(rw.Pending() == longLine && rw.HeldBytes() == 0, "⑤ 너무 긴 덜 온 줄은 안 쥐고 통과");
        Feed(rw, " 0\r\nUSER x\r\n");
        Check(calls == 0 && rw.Pending() == longLine + " 0\r\nUSER x\r\n", "⑤ 그 줄의 나머지도 통과 · onLogin 안 불림");
    }
    {   // 줄 머리만 같고 다른 줄(`nickname …`) · USER 앞 줄 사이 — 그대로
        CLoginRewriter rw(CLoginRewriter::Mode::Lobby);
        rw.onLogin = [](const std::string&, const std::string&) { return kTok; };
        const std::string s = "nickname x\r\napgarx pw12 0\r\nusers x\r\n";
        rw.Feed(s.data(), static_cast<int>(s.size()));
        Check(rw.Pending() == s && rw.Logins() == 0, "⑤ 머리 글자만 같은 다른 줄은 건드리지 않음");
    }
    // ⑥ onLogin 이 못 쓸 글을 돌려줌 · onLogin 이 없음
    for (const std::string bad : { std::string(), std::string("a b"), std::string("a\r\nb") }) {
        CLoginRewriter rw(CLoginRewriter::Mode::Lobby);
        rw.onLogin = [&](const std::string&, const std::string&) { return bad; };
        const std::string s = "NICK a1\r\napgar pw12 0\r\n";
        rw.Feed(s.data(), static_cast<int>(s.size()));
        Check(rw.Pending() == "NICK a1\r\napgar NXL!server_error 0\r\n", "⑥ 못 쓸 글 → NXL!server_error");
    }
    {
        CLoginRewriter rw(CLoginRewriter::Mode::Lobby);
        const std::string s = "apgar pw12 0\r\n";
        rw.Feed(s.data(), static_cast<int>(s.size()));
        Check(rw.Pending() == "apgar NXL!server_error 0\r\n", "⑥ onLogin 없음 → NXL!server_error(암호가 선에 안 나감)");
    }
    {   // ⑦ 진짜 오류 — 쥔 것을 버리고 -1
        CLoginRewriter rw(CLoginRewriter::Mode::Router);
        const std::string s = "whereto a1 pw12 1\r\n";
        rw.Feed(s.data(), static_cast<int>(s.size()));
        const int rc = rw.Flush([](const char*, int) { return -1; }, [] { return false; });
        Check(rc == -1 && rw.Pending().empty(), "⑦ 진짜 오류면 -1 · 쥔 것을 버림");
        rw.Feed(s.data(), static_cast<int>(s.size()));
        const int rc2 = rw.Flush([](const char*, int) { return -1; }, [] { return true; });
        Check(rc2 == 0 && !rw.Pending().empty(), "⑦ 막힘이면 0 · 쥠");
    }

    std::printf("login_rewrite runs=%lld fail=%d\n", runs, g_fail);
    std::printf("%s\n", g_fail ? "LOGINREWRITE FAIL" : "LOGINREWRITE PASS");
    return g_fail ? 1 : 0;
}
