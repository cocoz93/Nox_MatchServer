// chat_test.cpp — 귓속말 채팅 서버 판단(`CChatHub`) 시험. 플랜 `Docs/MO_WHISPER_PLAN_1105.md` 8절 `chat_test` 줄.
//
// 소켓 · DB 없이 줄과 시각을 넣고 나온 줄과 `ChatOutcome` 개수를 본다. 갈래마다 개수가 정확히 맞는지(합이 넣은 사건 수와 같은지)까지 본다.
#include <cstdio>
#include <string>
#include <vector>

#include "nox/ChatHub.h"

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

using nox::chat::CChatHub;
using nox::chat::ChatConfig;
using nox::chat::ChatOut;
using nox::chat::ChatOutcome;

constexpr uint32_t kPeer = 0x7f000001u;   // 127.0.0.1
constexpr uint32_t kNow = 1800000000u;    // 유닉스 초(토큰 만료 기준)

nox::JoinKey TestKey() {
    nox::JoinKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(0x30 + i);
    return k;
}

struct TokenSpec {
    std::string name = "qa1";
    uint32_t ip = kPeer;
    uint32_t expires = kNow + 60;
    uint64_t nonce = 1;
    bool otherKey = false;
};

// C++17 — 지정 초기화가 없어 이름 · 번호 · 고칠 칸을 함수로
TokenSpec Spec(const std::string& name, uint64_t nonce) {
    TokenSpec s;
    s.name = name;
    s.nonce = nonce;
    return s;
}

std::string Token(const TokenSpec& s) {
    nox::LoginToken t;
    t.accountId = 7;
    t.clientIp = s.ip;
    t.expiresAt = s.expires;
    t.nonce = s.nonce;
    t.name = s.name;
    nox::JoinKey k = TestKey();
    if (s.otherKey) k[0] ^= 0x55;
    uint8_t raw[nox::kLoginTokenSize];
    nox::SignLoginToken(k, t, raw);
    return nox::ToHex(raw, sizeof(raw));
}

unsigned long long N(const CChatHub& h, ChatOutcome o) { return h.Stats().outcome[static_cast<size_t>(o)]; }
unsigned long long Sum(const CChatHub& h) {
    unsigned long long s = 0;
    for (const auto v : h.Stats().outcome) s += v;
    return s;
}

// 보낼 줄이 정확히 이것들인가(차례 · 접속 · 끊기까지)
bool Same(const std::vector<ChatOut>& out, const std::vector<ChatOut>& want) {
    if (out.size() != want.size()) return false;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].conn != want[i].conn || out[i].line != want[i].line || out[i].close != want[i].close) return false;
    return true;
}

std::string Repeat(const std::string& s, size_t n) {
    std::string r;
    for (size_t i = 0; i < n; ++i) r += s;
    return r;
}

const std::string kHan = "\xEA\xB0\x80";   // 「가」 UTF-8 3바이트

// ① 글자 수 · 자르기 — 바이트가 아니라 글자 · 글자 중간에서 안 자름
void Utf8Tests() {
    using nox::chat::Utf8Chars;
    using nox::chat::Utf8Head;
    Check(Utf8Chars("") == 0 && Utf8Chars("abc") == 3, "① 영문 글자 수");
    Check(Utf8Chars(kHan + kHan) == 2, "① 한글 두 글자 = 2(바이트 6)");
    Check(Utf8Chars(Repeat(kHan, 80)) == 80, "① 한글 80자");
    Check(Utf8Head(Repeat("a", 80), 80) == Repeat("a", 80), "① 영문 80자 그대로");
    Check(Utf8Head(Repeat("a", 81), 80) == Repeat("a", 80), "① 영문 81자 → 80");
    Check(Utf8Head(Repeat(kHan, 80), 80) == Repeat(kHan, 80), "① 한글 80자 그대로(240바이트)");
    Check(Utf8Head(Repeat(kHan, 81), 80) == Repeat(kHan, 80), "① 한글 81자 → 80(240바이트 — 81째 첫 바이트 앞에서 끊음)");
    Check(Utf8Head("ab" + kHan + "c", 3) == "ab" + kHan, "① 섞인 글 — 한글 뒤에서 끊음");
    Check(Utf8Head("ab" + kHan + "c", 2) == "ab", "① 섞인 글 — 한글 앞에서 끊음(바이트 하나도 안 남김)");
    Check(Utf8Head("\x80\x80" "a", 0) == "\x80\x80", "① 첫 바이트가 이어지는 바이트면 글자로 안 셈(0자에도 남김)");
}

// ② 로그인 — 토큰 갈래 전수 · 일회용 · 정지 · 꼴
void AuthTests() {
    ChatConfig cfg;
    cfg.loginKey = TestKey();
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    int conn = 0;
    auto auth = [&](const std::string& line, uint32_t peer = kPeer) {
        ++conn;
        h.OnConnect(conn, peer, 0);
        out.clear();
        h.OnLine(conn, line, 0, kNow, out);
        return conn;
    };
    const int ok = auth("AUTH qa1 " + Token({}));
    Check(Same(out, {{ok, "AUTHOK", false}}) && h.IsOnline("qa1") && N(h, ChatOutcome::AuthOk) == 1, "② 맞는 토큰 → AUTHOK · 표에 이름");
    const int again = ok;
    out.clear();
    h.OnLine(again, "AUTH qa1 " + Token(Spec("qa1", 2)), 0, kNow, out);
    Check(Same(out, {{again, "AUTHNO again", false}}) && N(h, ChatOutcome::AuthAgain) == 1 && h.IsOnline("qa1"), "② 로그인한 연결이 또 AUTH → again · 안 끊음");

    const int replay = auth("AUTH qa1 " + Token({}));
    Check(Same(out, {{replay, "AUTHNO replay", true}}) && N(h, ChatOutcome::AuthReplay) == 1, "② 같은 토큰 다시 → replay · 끊음");
    Check(h.IsOnline("qa1"), "② 다시 쓴 토큰은 옛 연결을 안 끊음");

    struct Bad {
        const char* what;
        TokenSpec spec;
        std::string authName;
        nox::LoginTokenCheck want;
    };
    TokenSpec otherKey = Spec("qa2", 10), expired = Spec("qa2", 11), farAhead = Spec("qa2", 12), otherIp = Spec("qa2", 13);
    otherKey.otherKey = true;
    expired.expires = kNow - 1;          // 지금 > 만료 — 지금 = 만료는 아직 맞음(`LoginToken.cpp`)
    farAhead.expires = kNow + nox::kLoginTokenMaxAheadSec + 1;
    otherIp.ip = 0x0a000001u;
    const Bad bads[] = {
        {"다른 키", otherKey, "qa2", nox::LoginTokenCheck::BadMac},
        {"만료", expired, "qa2", nox::LoginTokenCheck::Expired},
        {"너무 먼 만료", farAhead, "qa2", nox::LoginTokenCheck::TooFarAhead},
        {"IP 다름", otherIp, "qa2", nox::LoginTokenCheck::IpMismatch},
        {"이름 다름(남의 토큰)", Spec("qa3", 14), "qa2", nox::LoginTokenCheck::NameMismatch},
    };
    for (const Bad& b : bads) {
        const int c = auth("AUTH " + b.authName + " " + Token(b.spec));
        Check(Same(out, {{c, "AUTHNO token", true}}) && h.Stats().tokenCheck[static_cast<size_t>(b.want)] == 1,
              std::string("② 토큰 거절 — ") + b.what + " → token · 갈래 1");
    }
    const int shortTok = auth("AUTH qa2 abcd");
    Check(Same(out, {{shortTok, "AUTHNO token", true}}) && h.Stats().tokenCheck[static_cast<size_t>(nox::LoginTokenCheck::BadSize)] == 1, "② 짧은 토큰 → token(BadSize)");
    Check(N(h, ChatOutcome::AuthToken) == 6 && !h.IsOnline("qa2"), "② 토큰 거절 6건 · 아무도 안 들어옴");
    Check(h.Stats().tokenCheck[static_cast<size_t>(nox::LoginTokenCheck::Ok)] == 2, "② 맞는 토큰 갈래 2(첫 로그인 + 다시 쓴 것 — 서명은 맞음)");

    const char* forms[] = {"AUTH", "AUTH qa2", "AUTH qa_2 " "00", "AUTH abcdefghij 00", "AUTH qa2 00 11"};
    unsigned long long formsSeen = 0;
    for (const char* f : forms) {
        const int c = auth(f);
        formsSeen += Same(out, {{c, "AUTHNO form", true}}) ? 1 : 0;
    }
    Check(formsSeen == 5 && N(h, ChatOutcome::AuthBadForm) == 5, "② 꼴 틀림 5갈래(이름 없음 · 토큰 없음 · 이름 규칙 밖 둘 · 토큰에 빈칸) → form · 끊음");

    // 정지 — 읽은 목록에 있으면 맞는 토큰이어도 거절
    std::vector<ChatOut> bo;
    h.ApplyBans({{"qa4", kNow + 3600}}, kNow, bo);
    const int banned = auth("AUTH qa4 " + Token(Spec("qa4", 20)));
    Check(Same(out, {{banned, "AUTHNO banned", true}}) && N(h, ChatOutcome::AuthBanned) == 1 && !h.IsOnline("qa4"), "② 정지 중 → banned · 끊음");
    h.ApplyBans({}, kNow, bo);
    const int unbanned = auth("AUTH qa4 " + Token(Spec("qa4", 21)));
    Check(Same(out, {{unbanned, "AUTHOK", false}}) && h.IsOnline("qa4"), "② 정지가 풀린 뒤 새 토큰 → AUTHOK");

    // 거절한 연결에서 더 온 줄은 버린다
    out.clear();
    h.OnLine(replay, "W qa1 hi", 0, kNow, out);
    Check(out.empty() && N(h, ChatOutcome::AfterClose) == 1, "② 끊기로 정한 연결의 줄 → 버림 · AfterClose 1");

    const unsigned long long events = 1 + 1 + 1 + 5 + 1 + 5 + 1 + 1 + 1;
    Check(Sum(h) == events, "② 결과 합 = 넣은 줄 수 " + std::to_string(events) + " (지금 " + std::to_string(Sum(h)) + ")");
}

// ③ 토큰 없이(`--no-auth`) — 이름 규칙 · 정지는 그대로
void NoAuthTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    h.OnConnect(1, kPeer, 0);
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    Check(Same(out, {{1, "AUTHOK", false}}) && N(h, ChatOutcome::AuthNoAuth) == 1 && N(h, ChatOutcome::AuthOk) == 0, "③ 토큰 없이 → AUTHOK · AuthNoAuth 갈래");
    out.clear();
    h.OnConnect(2, kPeer, 0);
    h.OnLine(2, "AUTH qa_1", 0, kNow, out);
    Check(Same(out, {{2, "AUTHNO form", true}}), "③ 토큰 없이도 이름 규칙은 봄");
    h.ApplyBans({{"qa3", kNow + 10}}, kNow, out);
    out.clear();
    h.OnConnect(3, kPeer, 0);
    h.OnLine(3, "AUTH qa3", 0, kNow, out);
    Check(Same(out, {{3, "AUTHNO banned", true}}), "③ 토큰 없이도 정지는 봄");
    out.clear();
    h.OnConnect(4, kPeer, 0);
    h.OnLine(4, "AUTH qa3", 0, kNow + 10, out);
    Check(Same(out, {{4, "AUTHOK", false}}), "③ 정지 끝 시각이 지나면(다음 읽기 전에도) 들어옴");
}

// ④ 같은 이름이 새로 붙음 — 옛 연결에 KICKED · 표는 새 연결 · 옛 연결이 닫혀도 새 이름은 그대로
void ReplaceTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    for (int c = 1; c <= 3; ++c) h.OnConnect(c, kPeer, 0);
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    h.OnLine(3, "AUTH qa2", 0, kNow, out);
    out.clear();
    h.OnLine(2, "AUTH qa1", 0, kNow, out);
    Check(Same(out, {{1, "KICKED", true}, {2, "AUTHOK", false}}) && N(h, ChatOutcome::Replaced) == 1, "④ 같은 이름 → 옛 연결 KICKED(끊음) 뒤 새 연결 AUTHOK");
    out.clear();
    h.OnLine(3, "W qa1 hi", 100, kNow, out);
    Check(Same(out, {{2, "F qa2 hi", false}, {3, "TO qa1 hi", false}}), "④ 귓속말은 새 연결로");
    h.OnClose(1);
    Check(h.IsOnline("qa1") && h.Online() == 2, "④ 옛 연결이 닫혀도 새 연결의 이름은 표에 남음");
    h.OnClose(2);
    Check(!h.IsOnline("qa1") && h.Online() == 1 && h.ConnCount() == 1, "④ 새 연결이 닫히면 이름이 빠짐");
    out.clear();
    h.OnLine(3, "W qa1 hi", 200, kNow, out);
    Check(Same(out, {{3, "NOUSER qa1", false}}), "④ 나간 뒤엔 NOUSER");
    Check(h.Stats().maxOnline == 2, "④ 최대 접속 2(같은 이름은 하나로)");
}

// ⑤ 귓속말 — 전달 · 없는 사람 · 대소문자 · 자기에게 · 로그인 전 · 틀린 줄 · 80자 · 제어 바이트
void WhisperTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    for (int c = 1; c <= 3; ++c) h.OnConnect(c, kPeer, 0);
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    h.OnLine(2, "AUTH qa2", 0, kNow, out);
    const unsigned long long base = Sum(h);
    uint64_t t = 0;
    auto w = [&](int conn, const std::string& line) {
        out.clear();
        t += 10000;   // 속도 제한에 안 걸리게 10초씩
        h.OnLine(conn, line, t, kNow, out);
    };
    w(1, "W qa2 hello there");
    Check(Same(out, {{2, "F qa1 hello there", false}, {1, "TO qa2 hello there", false}}) && N(h, ChatOutcome::Sent) == 1, "⑤ 전달 — 받는 쪽 F(보낸이는 서버가 찍음) · 보낸 쪽 TO · 빈칸 든 글 그대로");
    w(1, "W nobody hi");
    Check(Same(out, {{1, "NOUSER nobody", false}}) && N(h, ChatOutcome::NoUser) == 1, "⑤ 없는 사람 → NOUSER");
    w(1, "W QA2 hi");
    Check(Same(out, {{1, "NOUSER QA2", false}}) && N(h, ChatOutcome::NoUser) == 2, "⑤ 이름은 바이트 그대로 — QA2 ≠ qa2");
    w(1, "W qa1 hi");
    Check(out.empty() && N(h, ChatOutcome::ToSelf) == 1, "⑤ 자기에게 → 답 없음(DLL 이 먼저 막음)");
    w(3, "W qa1 hi");
    Check(out.empty() && N(h, ChatOutcome::NotAuthed) == 1, "⑤ 로그인 전 → 답 없음");
    const char* bad[] = {"W", "W qa2", "W qa2 ", "W qa2    ", "W  hi", "HELLO", "PING x", ""};
    for (const char* b : bad) w(1, b);
    Check(N(h, ChatOutcome::BadLine) == 8, "⑤ 틀린 줄 8갈래(낱말만 · 글 없음 · 빈칸만 · 대상 빈 값 · 모르는 낱말 · 인자 붙은 PING · 빈 줄) → 답 없음 · 8");
    w(1, "W qa2 " + Repeat(kHan, 81));
    Check(Same(out, {{2, "F qa1 " + Repeat(kHan, 80), false}, {1, "TO qa2 " + Repeat(kHan, 80), false}}) && h.Stats().truncated == 1,
          "⑤ 한글 81자 → 양쪽 다 80자 · 잘림 1");
    w(1, "W qa2 " + Repeat("b", 80));
    Check(out.size() == 2 && out[0].line == "F qa1 " + Repeat("b", 80) && h.Stats().truncated == 1, "⑤ 80자는 안 자름(잘림 그대로 1)");
    w(1, std::string("W qa2 a\x01" "b\x7f" "c"));
    Check(out.size() == 2 && out[0].line == "F qa1 a b c" && out[1].line == "TO qa2 a b c", "⑤ 제어 바이트 → 빈칸");
    w(1, "W qa2 %s %n %d");
    Check(out.size() == 2 && out[0].line == "F qa1 %s %n %d", "⑤ % 든 글 그대로(형식 문자로 안 씀)");
    Check(h.Stats().sentChars == 11 + 80 + 80 + 5 + 8, "⑤ 전달 글자 수 합(내용 대신 길이)");
    const unsigned long long events = 4 + 1 + 8 + 4;   // 전달 · 없는 사람 둘 · 자기에게 / 로그인 전 / 틀린 줄 / 전달 넷
    Check(Sum(h) - base == events, "⑤ 결과 합 = 넣은 줄 수 " + std::to_string(events) + " (지금 " + std::to_string(Sum(h) - base) + ")");
    Check(N(h, ChatOutcome::Sent) == 5, "⑤ 전달 5");
}

// ⑥ 속도 — 보낸 사람당 5초에 3줄 · 경계 · 없는 사람도 셈 · 사람마다 따로 · 접속을 바꿔도 이어 셈
void RateTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    const uint64_t t0 = 1000000;
    for (int c = 1; c <= 4; ++c) h.OnConnect(c, kPeer, t0);
    h.OnLine(1, "AUTH qa1", t0, kNow, out);   // 로그인 시각을 t0 에 — 0 에 두면 끝의 Tick 에서 받는 qa2 가 조용해서 끊긴다
    h.OnLine(2, "AUTH qa2", t0, kNow, out);
    h.OnLine(3, "AUTH qa3", t0, kNow, out);
    auto w = [&](int conn, const std::string& to, uint64_t at) {
        out.clear();
        h.OnLine(conn, "W " + to + " x", at, kNow, out);
        return out.empty() ? std::string() : out.back().line;
    };
    Check(w(1, "qa2", t0) == "TO qa2 x" && w(1, "nobody", t0 + 1) == "NOUSER nobody" && w(1, "qa2", t0 + 2) == "TO qa2 x", "⑥ 3줄까지 받음(없는 사람 포함)");
    Check(w(1, "qa2", t0 + 4999) == "LIMIT" && N(h, ChatOutcome::Limited) == 1, "⑥ 넷째 4999ms → LIMIT");
    Check(w(3, "qa2", t0 + 4999) == "TO qa2 x", "⑥ 다른 사람은 따로 셈");
    Check(w(1, "qa2", t0 + 5000) == "TO qa2 x", "⑥ 첫 줄에서 5000ms → 다시 받음(경계 · 4999 의 LIMIT 이 칸을 썼다면 여기서 막힘)");
    Check(w(1, "qa2", t0 + 5001) == "TO qa2 x" && w(1, "qa2", t0 + 5002) == "TO qa2 x", "⑥ 둘째 · 셋째 줄도 5000ms 지나 하나씩 빠짐");
    Check(w(1, "qa2", t0 + 5003) == "LIMIT" && N(h, ChatOutcome::Limited) == 2, "⑥ 5003 — 5000 · 5001 · 5002 셋이 참 → LIMIT");
    // 접속을 바꿔도 이어 셈 — 같은 이름으로 다시 붙어 한도를 피하지 못하게
    h.OnLine(4, "AUTH qa1", t0 + 5004, kNow, out);
    Check(w(4, "qa2", t0 + 5005) == "LIMIT" && N(h, ChatOutcome::Limited) == 3, "⑥ 같은 이름 새 연결도 앞 기록으로 셈");
    std::vector<ChatOut> tick;
    h.Tick(t0 + 20000, kNow, tick);
    Check(w(4, "qa2", t0 + 20001) == "TO qa2 x", "⑥ 정리 뒤 다시 받음");
}

// ⑦ 살아 있는지 — 30초 조용하면 PING · 답이 오면 다시 셈 · 90초 아무것도 없으면 끊음 · 로그인 전 연결도 90초
void IdleTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    h.OnConnect(1, kPeer, 0);
    h.OnConnect(2, kPeer, 0);
    h.OnConnect(3, kPeer, 0);   // 로그인 안 함
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    h.OnLine(2, "AUTH qa2", 0, kNow, out);
    out.clear();
    h.OnLine(1, "PING", 10, kNow, out);
    Check(Same(out, {{1, "PONG", false}}) && N(h, ChatOutcome::PingAnswered) == 1, "⑦ 클라 PING → PONG");
    out.clear();
    h.Tick(29999, kNow, out);
    Check(out.empty(), "⑦ 29999ms — 아직 PING 없음");
    out.clear();
    h.Tick(30000, kNow, out);
    Check(Same(out, {{2, "PING", false}}), "⑦ 30000ms — 조용한 qa2 에만 PING(qa1 은 10ms 에 말함 · 로그인 전 연결엔 안 보냄)");
    out.clear();
    h.Tick(30005, kNow, out);
    Check(out.empty() && h.Stats().pingsSent == 1, "⑦ PING 을 보낸 뒤 30초 안엔 또 안 보냄");
    out.clear();
    h.OnLine(2, "PONG", 30020, kNow, out);
    Check(out.empty() && N(h, ChatOutcome::PongSeen) == 1, "⑦ PONG 받음 — 답 없음");
    out.clear();
    h.Tick(89999, kNow, out);
    Check(Same(out, {{1, "PING", false}, {2, "PING", false}}) && h.ConnCount() == 3, "⑦ 89999ms — 30초 넘게 조용한 둘에 PING · 아무도 안 끊음");
    out.clear();
    h.Tick(90000, kNow, out);
    Check(Same(out, {{3, "", true}}) && N(h, ChatOutcome::IdleClosed) == 1, "⑦ 90000ms — 로그인 전 연결(0ms 뒤 조용) 끊음");
    out.clear();
    h.Tick(90010, kNow, out);
    Check(Same(out, {{1, "", true}}) && !h.IsOnline("qa1") && h.IsOnline("qa2"), "⑦ 90010ms — 10ms 에 말한 qa1 끊음 · 이름 빠짐 · PONG 한 qa2 는 남음");
    out.clear();
    h.OnLine(2, "PONG", 100000, kNow, out);
    h.Tick(129999, kNow, out);
    Check(out.empty() && N(h, ChatOutcome::PongSeen) == 2, "⑦ PONG(100000) 뒤 30초 전 — PING 없음 · 끊긴 연결엔 안 보냄");
    h.Tick(130000, kNow, out);
    Check(Same(out, {{2, "PING", false}}), "⑦ PONG 뒤 30초 — 다시 PING");
    out.clear();
    h.Tick(189999, kNow, out);
    Check(Same(out, {{2, "PING", false}}) && h.IsOnline("qa2"), "⑦ 답 없이 조용하면 30초마다 PING · 아직 안 끊음");
    out.clear();
    h.Tick(190000, kNow, out);
    Check(Same(out, {{2, "", true}}) && N(h, ChatOutcome::IdleClosed) == 3 && h.Online() == 0, "⑦ 마지막 줄(100000) 뒤 90초 — 끊음");
}

// ⑧ 정지 — 새로 정지된 이름의 연결을 끊음 · 접속 안 한 이름은 그냥 · 이미 정지된 이름은 다시 안 끊음 · 못 읽음은 부르지 않음
void BanTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    for (int c = 1; c <= 3; ++c) h.OnConnect(c, kPeer, 0);
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    h.OnLine(2, "AUTH qa2", 0, kNow, out);
    out.clear();
    const size_t closed = h.ApplyBans({{"qa2", kNow + 100}, {"ghost", kNow + 100}}, kNow, out);
    Check(closed == 1 && Same(out, {{2, "", true}}) && N(h, ChatOutcome::BanClosed) == 1 && !h.IsOnline("qa2") && h.IsOnline("qa1"),
          "⑧ 새 정지 qa2 → 끊음 · 이름 빠짐 · 접속 안 한 ghost 는 셈 안 함");
    out.clear();
    h.OnLine(1, "W qa2 hi", 10, kNow, out);
    Check(Same(out, {{1, "NOUSER qa2", false}}), "⑧ 끊긴 정지 이름에게 → NOUSER");
    h.OnLine(3, "AUTH qa2", 20, kNow, out);
    out.clear();
    const size_t again = h.ApplyBans({{"qa2", kNow + 100}}, kNow, out);
    Check(again == 0 && out.empty() && N(h, ChatOutcome::BanClosed) == 1, "⑧ 같은 목록을 다시 읽으면 새로 안 끊음(로그인도 막혀 있음)");
    Check(N(h, ChatOutcome::AuthBanned) == 1, "⑧ 정지 중 다시 붙기 → banned");
}

// ⑨ 갈래 이름 전수 — 빈 값 · 「?」 · 겹침 없음
void NameTests() {
    std::vector<std::string> seen;
    bool ok = true;
    for (size_t i = 0; i < static_cast<size_t>(ChatOutcome::kCount); ++i) {
        const std::string n = nox::chat::ChatOutcomeName(static_cast<ChatOutcome>(i));
        if (n.empty() || n == "?") ok = false;
        for (const std::string& s : seen)
            if (s == n) ok = false;
        seen.push_back(n);
    }
    Check(ok && seen.size() == 19, "⑨ ChatOutcome 19갈래 이름 — 빈 값 · 「?」 · 겹침 없음");
}

// ⑩ 기록에 귓속말 내용이 안 남는다
void LogTests() {
    ChatConfig cfg;
    cfg.noAuth = true;
    CChatHub h(cfg);
    std::vector<ChatOut> out;
    h.OnConnect(1, kPeer, 0);
    h.OnConnect(2, kPeer, 0);
    h.OnLine(1, "AUTH qa1", 0, kNow, out);
    h.OnLine(2, "AUTH qa2", 0, kNow, out);
    h.TakeLog();
    h.OnLine(1, "W qa2 secretword", 10, kNow, out);
    const std::vector<std::string> log = h.TakeLog();
    bool leak = false;
    for (const std::string& l : log) leak = leak || l.find("secretword") != std::string::npos;
    Check(log.size() == 1 && !leak && log[0].find("10자") != std::string::npos, "⑩ 전달 기록 한 줄 · 내용 없음 · 길이만");
    Check(h.TakeLog().empty(), "⑩ 꺼내면 비움");
}

}  // namespace

int main() {
    Utf8Tests();
    AuthTests();
    NoAuthTests();
    ReplaceTests();
    WhisperTests();
    RateTests();
    IdleTests();
    BanTests();
    NameTests();
    LogTests();
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
