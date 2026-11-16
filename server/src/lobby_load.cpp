// lobby_load.cpp — 로비 부하 도구 `nox_lobby_load` (★ 11-17 D-2 · `Docs/SERVERCORE_LOBBY_MOVE_PLAN_1116.md` 6절).
//
//   nox_lobby_load.exe --lobby 127.0.0.1:4000 [--router 127.0.0.1:4005] [--people 10] [--active 0.1] [--rate-scale 1]
//                      [--ramp 50] [--seconds 30] [--hold-sec 30] [--prefix ld] [--chat-lobbies 1] [--seed 1] [--workers 1]
//                      [--timeout-ms 10000]
//
// 무엇을 하나 — 사람 M(`--people`) 명을 흉내 낸다. 한 사람 = 정품 클라 + DLL 판 7 이 보내는 차례
//   (`tools/lobby_smoke.ps1:5-7` · 사람 판 로비 기록 `_out/mo_local/*/lobby.log` — 11-15 판의 줄과 시각 묶음).
//   1. 라우터(`--router` 를 주면)에 붙어 녹음 다섯 줄(`tools/router_smoke.ps1` 의 줄) → 607 → 라우터가 끊는다
//   2. 로비에 붙어 로그인 네 묶음 — 정품이 답을 기다렸다가 다음 묶음을 보내는 자리 그대로(사람 판 기록의 같은 ms 묶음):
//      ① NOXCLIENT 7 … SETOPT → 379  ② SETLOCALE · SETCODEPAGE → 329  ③ SETOPT · LIST -1 37 · JOIN #Lob → 323 · 366  ④ GETCODEPAGE → 328
//   3. 로비가 알려 준 채팅 서버(`NOXCHAT ip:port`)에 붙어 `AUTH 이름` → AUTHOK(채팅 서버 `--no-auth` 판 — 첫 판은 암호 확인 비용을 뺀다 · 계획 6-3)
//   4. 쉬기 — 줄 0. 채팅 서버의 PING 에만 PONG(DLL 이 하는 일 — 안 하면 90초에 끊긴다 `ChatHub.h:44`). 이 상태의 사람 수가 M 이다
//   5. 활동 — 활동하는 사람(비율 R = `--active`)만. 다음 일까지의 간격은 지수 분포(빈도 합의 역수) · 일은 빈도 비율로 고른다.
//      빈도 = 사람 판 기록에서 센 값(구체화 D-④ · `MyDocs/claude-plans/impl1116-work/d2_lines.py` — 로그인 뒤 29.3분 · 접속 11) × `--rate-scale`.
//      1분에 — 목록 0.51 · 전적 0.31 · 방(내 방 참가 → `--hold-sec` 머묾 → 나감) 0.17 · 대화방 오가기(PART → JOIN → GETCODEPAGE) 0.14 ·
//      채팅 한 줄 0.07 · 다시 로그인(QUIT → 1 부터) 0.07
//      안 넣은 것 — 랭크 줄(NOXRANK JOIN · ACCEPT · LEAVE · 기록 59줄 중 6)은 묶임 · 수락 차례에 짝이 필요해 이 판에서 뺐다
//   한 사람은 답을 받은 뒤에야 다음 일을 한다(사람처럼). 그래서 답은 보낸 차례대로 오고, 앞에서부터 끝 답 줄과 맞춘다
//
// 단계 — 올림(초마다 `--ramp` 명씩 시작) → 잼(모두 쉬기에 든 뒤 `--seconds` 초) → 끝(모두 QUIT → 607 · 채팅 끊기 · 15초 한도)
//
// 무엇을 재나(계획 6-2 「더미 쪽」) — 답 지연 = 줄을 보낸 때 ~ 끝 답 줄을 받은 때. 받은 때는 이 프로세스의 `Poll` 이 집은 때라 더미 쪽
//   줄 섬 시간이 들어간다(더미가 바쁘면 답이 늦어 보인다 — `[더미 구간]` 의 CPU 칸으로 가른다) · 답 없음(`--timeout-ms`) · 뜻밖 끊김(까닭별) ·
//   붙기 실패 · 보낸/받은 줄 수(로비 끝 줄 「받은 줄 · 보낸 줄」과 같은 단위 — 줄 하나 = CRLF 하나 · 셈은 로비 접속 몫만 따로).
//   초마다 `[더미 구간]` · 끝에 `[더미 끝]` 줄. 답 분포는 잼 구간과 전체를 따로 찍는다
//
// 소켓 — `CTcpLinesCore`(ServerCore 위 · D-1). `CTcpLines` 위가 아닌 까닭 — 더미가 접속 수천 개를 쥐면 깰 때마다 모두에 받기 호출을 해서
//   서버보다 먼저 더미가 찬다(계획 6-3). 한 사람이 로비 · 채팅 · (잠깐) 라우터를 쥐므로 접속 한도 = 사람 × 3 + 16.
//   ⚠ ServerCore 기록은 표준 출력으로 섞여 나온다(`Logger::Init` 을 안 부름 — D-1 기록) · ⚠ 줄 한도 4,096 — 넘는 답 줄(사람이 많은 대화방의 353)은
//   이쪽이 「줄 너무 김」으로 끊는다. 정품 클라가 그 줄을 어떻게 받는지는 모른다 — 끊김 까닭 칸에 그대로 센다
#ifndef NOMINMAX
#define NOMINMAX   // `std::min` · `std::max` 를 windows.h 매크로가 가리지 않게
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "nox/TcpLinesCore.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

using Clock = std::chrono::steady_clock;

double MsBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

bool ParseHostPort(const std::string& s, uint32_t& ip, uint16_t& port) {
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    in_addr a{};
    if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &a) != 1) return false;
    const int p = std::atoi(s.c_str() + colon + 1);
    if (p <= 0 || p > 65535) return false;
    ip = ntohl(a.s_addr);
    port = static_cast<uint16_t>(p);
    return true;
}

double ProcessCpuMs() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}

// 요청 종류 — 답 지연을 이 갈래로 따로 센다. 이름은 `[더미 끝]` 줄에 그대로
enum class Req : int { Router, Login, Codepage, FirstList, ChatJoin, GetCodepage, ChatAuth, List, Record, Room, RoomPart, ChatPart, Quit, kCount };
constexpr int kReqCount = static_cast<int>(Req::kCount);
const char* ReqName(Req r) {
    switch (r) {
        case Req::Router: return "라우터";
        case Req::Login: return "로그인";
        case Req::Codepage: return "코드페이지";
        case Req::FirstList: return "첫 목록";
        case Req::ChatJoin: return "대화방 들어감";
        case Req::GetCodepage: return "코드페이지 묻기";
        case Req::ChatAuth: return "채팅 로그인";
        case Req::List: return "목록";
        case Req::Record: return "전적";
        case Req::Room: return "방 참가";
        case Req::RoomPart: return "방 나감";
        case Req::ChatPart: return "대화방 나감";
        case Req::Quit: return "QUIT";
        case Req::kCount: break;
    }
    return "?";
}

// 접속 몫 — 라우터 · 로비 · 채팅(줄 셈을 따로 한다)
enum Role : int { kRouter = 0, kLobby = 1, kChat = 2, kRoles = 3 };
const char* RoleName(int r) { return r == kRouter ? "라우터" : r == kLobby ? "로비" : "채팅"; }

// 받은 줄의 종류 — `tools/lobby_smoke.ps1` 의 `Kind` 와 같은 갈래(`: 366 …` = 366 · `:이름!u@h PART …` = user:PART) + 우리 줄(NOX… · 채팅 서버 줄)
struct Parsed {
    std::string kind, nick, channel;
};
Parsed ParseLine(const std::string& l) {
    std::vector<std::string> t;
    size_t at = 0;
    for (;;) {
        const size_t sp = l.find(' ', at);
        t.push_back(l.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
        if (sp == std::string::npos) break;
        at = sp + 1;
    }
    Parsed p;
    if (t.empty() || t[0].empty()) return p;
    if (t[0] == ":") {   // 숫자 답 — `: 366 u #Lob_37_0 :`
        if (t.size() > 1) p.kind = t[1];
        if (t.size() > 3) p.channel = t[3];
        return p;
    }
    if (t[0][0] == ':') {   // 사람 줄 — `:이름!u@h PART #Lob_37_0`
        const size_t bang = t[0].find('!');
        p.nick = t[0].substr(1, bang == std::string::npos ? std::string::npos : bang - 1);
        p.kind = "user:" + (t.size() > 1 ? t[1] : std::string());
        if (t.size() > 2) p.channel = t[2];
        return p;
    }
    p.kind = t[0];
    if (t[0] == "NOXRANK" && t.size() > 1) p.kind = "NOXRANK " + t[1];
    if (t[0] == "NOXCHAT" && t.size() > 1) p.channel = t[1];   // 채팅 서버 주소 `ip:port`
    return p;
}

// 답을 기다리는 요청 하나 — 끝 답 줄 종류(셋 중 하나라도) · 채널(비지 않으면 그 채널) · 사람 줄이면 보낸 사람이 나여야
struct Pending {
    Req req = Req::Login;
    Clock::time_point sentAt;
    const char* kinds[3] = {nullptr, nullptr, nullptr};
    std::string channel;
    bool self = false;
};
bool Matches(const Pending& w, const Parsed& p, const std::string& me) {
    bool kindOk = false;
    for (const char* k : w.kinds)
        if (k && p.kind == k) kindOk = true;
    if (!kindOk) return false;
    if (!w.channel.empty() && p.channel != w.channel) return false;
    if (w.self && p.nick != me) return false;
    return true;
}

enum class Step { Start, Router, Login, ChatAuth, Ready, Act, InRoom, Quit, Done, Failed };

struct Person {
    int idx = 0;
    std::string name, chatLobby, room;
    bool active = false;
    Step step = Step::Start;
    int router = 0, lobby = 0, chat = 0;
    std::deque<Pending> pending;
    int loginStage = 0;   // 로그인 묶음 차례(1~4)
    Clock::time_point nextAct, roomLeave;
    uint32_t chatIp = 0;
    uint16_t chatPort = 0;
    bool relogin = false;   // 지금 QUIT 이 다시 로그인인가(끝 단계 QUIT 과 가름)
};

// 활동 — 빈도(1분에)는 사람 판 기록에서 센 값(머리 주석 5)
enum class Act : int { List, Record, Room, ChatHop, Chat, Relogin, kCount };
constexpr int kActCount = static_cast<int>(Act::kCount);
constexpr double kActPerMin[kActCount] = {0.51, 0.31, 0.17, 0.14, 0.07, 0.07};
const char* ActName(Act a) {
    switch (a) {
        case Act::List: return "목록";
        case Act::Record: return "전적";
        case Act::Room: return "방";
        case Act::ChatHop: return "대화방 오가기";
        case Act::Chat: return "채팅";
        case Act::Relogin: return "다시 로그인";
        case Act::kCount: break;
    }
    return "?";
}

struct Options {
    uint32_t lobbyIp = 0, routerIp = 0;
    uint16_t lobbyPort = 0, routerPort = 0;
    int people = 10;
    double active = 0.1;
    double rateScale = 1.0;
    int ramp = 50;
    int seconds = 30;
    int holdSec = 30;
    std::string prefix = "ld";
    int chatLobbies = 1;
    uint64_t seed = 1;
    int workers = 1;
    int timeoutMs = 10000;
};

struct RoleStats {
    unsigned long long opened = 0, sent = 0, recv = 0, connectFailed = 0, closedExpected = 0;
    std::map<std::string, unsigned long long> closedUnexpected;
};

// 백분위 — 정렬한 값에서 올림 자리(느린 1% = 99번째 백분위)
double Percentile(std::vector<float>& v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::ceil(q * static_cast<double>(v.size()))) ;
    if (i > 0) --i;
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

class CLoad {
public:
    explicit CLoad(const Options& o) : opt_(o), rng_(o.seed) {
        nox::TcpLinesCoreOptions to;
        to.workers = o.workers;
        to.maxConns = static_cast<size_t>(o.people) * 3 + 16;
        to.sendRingBytes = 8192;   // 더미가 한 번에 내는 것은 로그인 묶음(300바이트 안팎)뿐 — 표 크기만큼 미리 잡는 링을 줄인다
        tcp_ = std::make_unique<nox::CTcpLinesCore>(to);
    }

    int Run() {
        std::string err;
        if (!tcp_->Start(err)) {
            std::printf("[멈춤] 소켓 부품 Start — %s\n", err.c_str());
            return 2;
        }
        people_.resize(static_cast<size_t>(opt_.people));
        std::vector<int> order(people_.size());
        for (size_t i = 0; i < people_.size(); ++i) order[i] = static_cast<int>(i);
        std::shuffle(order.begin(), order.end(), rng_);
        const int activeCount = static_cast<int>(std::lround(opt_.active * opt_.people));
        for (size_t i = 0; i < people_.size(); ++i) {
            Person& p = people_[i];
            p.idx = static_cast<int>(i);
            char nm[32];
            std::snprintf(nm, sizeof(nm), "%s%05d", opt_.prefix.c_str(), static_cast<int>(i));
            p.name = nm;
            p.chatLobby = "#Lob_37_" + std::to_string(static_cast<int>(i) % opt_.chatLobbies);
        }
        for (int k = 0; k < activeCount && k < opt_.people; ++k) people_[static_cast<size_t>(order[static_cast<size_t>(k)])].active = true;
        activeCount_ = activeCount;
        for (int a = 0; a < kActCount; ++a) actRateSum_ += kActPerMin[a];
        std::printf("[더미] 사람 %d(활동 %d · 빈도 배율 %.2f · 1분에 %.2f번) · 로비 %u · 라우터 %s · 올림 초당 %d · 잼 %d초 · 방 머묾 %d초 · 대화방 %d개 · 답 한도 %dms\n",
                    opt_.people, activeCount, opt_.rateScale, actRateSum_ * opt_.rateScale, opt_.lobbyPort, opt_.routerPort ? "씀" : "안 씀",
                    opt_.ramp, opt_.seconds, opt_.holdSec, opt_.chatLobbies, opt_.timeoutMs);
        std::fflush(stdout);

        start_ = Clock::now();
        secAt_ = start_;
        cpuAt_ = ProcessCpuMs();
        std::vector<nox::TcpLineEvent> evs;
        int started = 0;
        for (;;) {
            WaitForSingleObject(static_cast<HANDLE>(tcp_->WaitHandle()), 10);
            const Clock::time_point now = Clock::now();
            evs.clear();
            tcp_->Poll(evs);
            for (const auto& e : evs) OnEvent(e, Clock::now());

            // 올림 — 지난 시간 × 초당 명 까지 시작
            if (phase_ == 0) {
                const int due = std::min(opt_.people, static_cast<int>(MsBetween(start_, now) / 1000.0 * opt_.ramp) + 1);
                while (started < due) Begin(people_[static_cast<size_t>(started++)], now);
                if (started == opt_.people && Settled()) {
                    phase_ = 1;
                    measureAt_ = now;
                    std::printf("[더미] 잼 시작 — 올림 %.1f초 · 쉬기 %d · 실패 %d\n", MsBetween(start_, now) / 1000.0, CountStep(Step::Ready) + CountStep(Step::InRoom) + CountStep(Step::Act), CountStep(Step::Failed));
                }
            } else if (phase_ == 1 && MsBetween(measureAt_, now) >= opt_.seconds * 1000.0) {
                phase_ = 2;
                endAt_ = now;
                std::printf("[더미] 끝 단계 — 모두 QUIT\n");
            }
            for (Person& p : people_) Tick(p, now);
            evs.clear();
            tcp_->Flush(evs);
            for (const auto& e : evs) OnEvent(e, Clock::now());

            if (MsBetween(secAt_, now) >= 1000.0) PrintSecond(now);
            std::fflush(stdout);
            if (phase_ == 2 && (AllEnded() || MsBetween(endAt_, now) >= 15000.0)) break;
        }
        PrintSecond(Clock::now());
        const size_t left = tcp_->ConnCount();
        tcp_->CloseAll();
        PrintEnd(left);
        return 0;
    }

private:
    bool Settled() const {   // 모두 쉬기(또는 실패)에 들었나 — 로그인 · 채팅 로그인 · 라우터 중이 아님
        for (const Person& p : people_)
            if (p.step == Step::Start || p.step == Step::Router || p.step == Step::Login || p.step == Step::ChatAuth) return false;
        return true;
    }
    int CountStep(Step s) const {
        int n = 0;
        for (const Person& p : people_)
            if (p.step == s) ++n;
        return n;
    }
    bool AllEnded() const {
        for (const Person& p : people_)
            if (p.step != Step::Done && p.step != Step::Failed) return false;
        return tcp_->ConnCount() == 0;
    }

    int ConnectAs(Person& p, int role, uint32_t ip, uint16_t port) {
        std::string err;
        const int c = tcp_->Connect(ip, port, err);
        if (c == 0) {
            ++roles_[role].connectFailed;
            return 0;
        }
        connOf_[c] = {p.idx, role};
        return c;
    }
    bool Send(int role, int conn, const std::string& line) {
        if (!tcp_->Send(conn, line)) return false;
        ++roles_[role].sent;
        ++secSent_[role];
        return true;
    }
    void Expect(Person& p, Req r, Clock::time_point now, const char* k1, const char* k2 = nullptr, const char* k3 = nullptr,
                const std::string& channel = std::string(), bool self = false) {
        Pending w;
        w.req = r;
        w.sentAt = now;
        w.kinds[0] = k1;
        w.kinds[1] = k2;
        w.kinds[2] = k3;
        w.channel = channel;
        w.self = self;
        p.pending.push_back(w);
    }

    // 1 부터 — 라우터(있으면) · 아니면 곧바로 로비
    void Begin(Person& p, Clock::time_point now) {
        p.pending.clear();
        p.loginStage = 0;
        p.chatIp = 0;
        p.chatPort = 0;
        if (opt_.routerPort) {
            p.router = ConnectAs(p, kRouter, opt_.routerIp, opt_.routerPort);
            if (!p.router) return Fail(p, "라우터 붙기 못 함");
            p.step = Step::Router;
            for (const std::string& l : {std::string("verchk 32512 65556"), std::string("verchk 9472 65538"), std::string("lobcount 9472"),
                                         "whereto " + p.name + " 1234 9472 65538 7726", std::string("QUIT")})
                Send(kRouter, p.router, l);
            Expect(p, Req::Router, now, "607");
            return;
        }
        StartLobby(p, now);
    }
    void StartLobby(Person& p, Clock::time_point now) {
        p.lobby = ConnectAs(p, kLobby, opt_.lobbyIp, opt_.lobbyPort);
        if (!p.lobby) return Fail(p, "로비 붙기 못 함");
        p.step = Step::Login;
        p.loginStage = 1;
        for (const std::string& l : {std::string("NOXCLIENT 7"), std::string("CVERS 11020 9472"), std::string("PASS supersecret"), "NICK " + p.name,
                                     std::string("apgar 1234 0"), std::string("SERIAL 7726"),
                                     std::string("USER UserName HostName irc.westwood.com :RealName"), std::string("verchk 32512 720916"),
                                     std::string("SETOPT 17,33")})
            Send(kLobby, p.lobby, l);
        Expect(p, Req::Login, now, "379");
    }
    // 로그인 묶음 하나의 답이 다 왔다 → 다음 묶음
    void NextLogin(Person& p, Clock::time_point now) {
        switch (++p.loginStage) {
            case 2:
                Send(kLobby, p.lobby, "SETLOCALE 0");
                Send(kLobby, p.lobby, "SETCODEPAGE 949");
                Expect(p, Req::Codepage, now, "329");
                return;
            case 3:
                Send(kLobby, p.lobby, "SETOPT 17,33");
                Send(kLobby, p.lobby, "LIST -1 37");
                Send(kLobby, p.lobby, "JOIN " + p.chatLobby + " zot0blat");
                Expect(p, Req::FirstList, now, "323");
                Expect(p, Req::ChatJoin, now, "366", nullptr, nullptr, p.chatLobby);
                return;
            case 4:
                Send(kLobby, p.lobby, "GETCODEPAGE " + p.name);
                Expect(p, Req::GetCodepage, now, "328");
                return;
            default:
                break;
        }
        if (p.chatPort == 0) {   // 로비가 채팅 서버를 안 알렸다 — 채팅 없이 쉬기
            ++noChatAddr_;
            return Ready(p, now);
        }
        p.chat = ConnectAs(p, kChat, p.chatIp, p.chatPort);
        if (!p.chat) return Fail(p, "채팅 붙기 못 함");
        p.step = Step::ChatAuth;
        Send(kChat, p.chat, "AUTH " + p.name);
        Expect(p, Req::ChatAuth, now, "AUTHOK", "AUTHNO");
    }
    void Ready(Person& p, Clock::time_point now) {
        p.step = Step::Ready;
        ScheduleAct(p, now);
    }
    void ScheduleAct(Person& p, Clock::time_point now) {
        if (!p.active) return;
        std::exponential_distribution<double> gap(actRateSum_ * opt_.rateScale / 60.0);   // 1초에 몇 번
        p.nextAct = now + std::chrono::microseconds(static_cast<long long>(gap(rng_) * 1e6));
    }
    void DoAct(Person& p, Clock::time_point now) {
        std::uniform_real_distribution<double> pick(0.0, actRateSum_);
        double x = pick(rng_);
        Act a = Act::List;
        for (int i = 0; i < kActCount; ++i) {
            if (x < kActPerMin[i]) {
                a = static_cast<Act>(i);
                break;
            }
            x -= kActPerMin[i];
            a = static_cast<Act>(i);
        }
        ++acts_[static_cast<int>(a)];
        switch (a) {
            case Act::List:
                p.step = Step::Act;
                Send(kLobby, p.lobby, "LIST 37 37");
                Expect(p, Req::List, now, "323");
                return;
            case Act::Record:
                p.step = Step::Act;
                Send(kLobby, p.lobby, "NOXRANK RECORD");
                Expect(p, Req::Record, now, "NOXRANK RECEND", "NOXRANK RECFAIL", "NOXRANK RECNONE");
                return;
            case Act::Room:
                p.step = Step::Act;
                p.room = "#" + p.name + "'s_game";
                Send(kLobby, p.lobby, "JOINGAME " + p.room + " 1 ");   // 정품 줄 끝 빈칸 그대로(사람 판 기록)
                Expect(p, Req::Room, now, "366", "471");
                return;
            case Act::ChatHop:
                p.step = Step::Act;
                Send(kLobby, p.lobby, "PART " + p.chatLobby);
                Send(kLobby, p.lobby, "JOIN " + p.chatLobby + " zot0blat");
                Send(kLobby, p.lobby, "GETCODEPAGE " + p.name);
                Expect(p, Req::ChatPart, now, "user:PART", nullptr, nullptr, p.chatLobby, true);
                Expect(p, Req::ChatJoin, now, "366", nullptr, nullptr, p.chatLobby);
                Expect(p, Req::GetCodepage, now, "328");
                return;
            case Act::Chat:   // 답 없음(로비는 보낸 사람에게 안 돌려준다) — 셈만
                Send(kLobby, p.lobby, "PRIVMSG " + p.chatLobby + " :hello " + std::to_string(++chatSeq_));
                ScheduleAct(p, now);
                return;
            case Act::Relogin:
                ++relogins_;
                p.relogin = true;
                p.step = Step::Quit;
                Send(kLobby, p.lobby, "QUIT");
                Expect(p, Req::Quit, now, "607");
                return;
            case Act::kCount:
                return;
        }
    }

    void Fail(Person& p, const char* why) {
        ++failed_[why];
        p.step = Step::Failed;
        p.pending.clear();
        std::vector<nox::TcpLineEvent> ignore;
        for (int* c : {&p.router, &p.lobby, &p.chat}) {
            if (*c) {
                connOf_.erase(*c);
                tcp_->Close(*c, ignore);
                *c = 0;
            }
        }
    }

    // 답 하나가 맞았다 — 다음 할 일
    void Answered(Person& p, const Pending& w, const Parsed& got, Clock::time_point now) {
        const float ms = static_cast<float>(MsBetween(w.sentAt, now));
        const int r = static_cast<int>(w.req);
        lat_[r][0].push_back(ms);
        if (phase_ == 1) lat_[r][1].push_back(ms);
        secLat_.push_back(ms);
        if (!p.pending.empty()) return;   // 같은 묶음의 답이 더 남았다
        switch (p.step) {
            case Step::Router:   // 607 — 라우터가 끊는다(끊김은 기다리지 않는다 · 정품도 곧바로 로비로)
                StartLobby(p, now);
                return;
            case Step::Login:
                NextLogin(p, now);
                return;
            case Step::ChatAuth:
                if (got.kind != "AUTHOK") return Fail(p, "채팅 로그인 거절");
                Ready(p, now);
                return;
            case Step::Act:
                if (w.req == Req::Room) {
                    if (got.kind == "471") {
                        ++roomRejected_;
                        Ready(p, now);
                        return;
                    }
                    ++roomJoined_;
                    p.step = Step::InRoom;
                    p.roomLeave = now + std::chrono::seconds(opt_.holdSec);
                    return;
                }
                Ready(p, now);
                return;
            case Step::Quit:   // 607 — 로비가 끊는다 · 채팅은 이쪽이 끊는다
                if (p.chat) {
                    std::vector<nox::TcpLineEvent> ignore;
                    connOf_.erase(p.chat);
                    tcp_->Close(p.chat, ignore);
                    ++roles_[kChat].closedExpected;
                    p.chat = 0;
                }
                if (p.relogin && phase_ != 2) {
                    p.relogin = false;
                    pendingRelogin_.push_back(p.idx);   // 로비 접속이 닫힌 뒤 1 부터(같은 이름이 둘이 되지 않게)
                } else {
                    p.relogin = false;
                    p.step = Step::Done;
                }
                return;
            default:
                return;
        }
    }

    void OnEvent(const nox::TcpLineEvent& e, Clock::time_point now) {
        const auto it = connOf_.find(e.conn);
        if (it == connOf_.end()) {
            if (e.type == nox::TcpLineEvent::Type::LINE) ++strayLines_;
            return;
        }
        Person& p = people_[static_cast<size_t>(it->second.first)];
        const int role = it->second.second;
        switch (e.type) {
            case nox::TcpLineEvent::Type::OPENED:
                ++roles_[role].opened;
                return;
            case nox::TcpLineEvent::Type::LINE: {
                ++roles_[role].recv;
                ++secRecv_[role];
                const Parsed got = ParseLine(e.line);
                if (role == kChat && got.kind == "PING") {   // DLL 이 하는 일 — 채팅 서버 살아 있음
                    Send(kChat, p.chat, "PONG");
                    ++pongs_;
                    return;
                }
                if (role == kLobby && got.kind == "NOXCHAT") {
                    uint32_t ip = 0;
                    uint16_t port = 0;
                    if (ParseHostPort(got.channel, ip, port)) {
                        p.chatIp = ip;
                        p.chatPort = port;
                    }
                }
                if (p.pending.empty()) return;
                const Pending w = p.pending.front();
                const int wantRole = (w.req == Req::Router) ? kRouter : (w.req == Req::ChatAuth) ? kChat : kLobby;
                if (role != wantRole || !Matches(w, got, p.name)) return;
                p.pending.pop_front();
                Answered(p, w, got, now);
                return;
            }
            case nox::TcpLineEvent::Type::CLOSED: {
                connOf_.erase(it);
                int* slot = role == kRouter ? &p.router : role == kLobby ? &p.lobby : &p.chat;
                if (*slot == e.conn) *slot = 0;
                const bool connectFailed = std::strcmp(e.why, nox::CTcpLinesCore::kWhyConnectFailed) == 0;
                if (connectFailed) {
                    ++roles_[role].connectFailed;
                    return Fail(p, role == kRouter ? "라우터 붙기 못 함" : role == kLobby ? "로비 붙기 못 함" : "채팅 붙기 못 함");
                }
                // 기다린 끊김 — 라우터는 607 뒤 · 로비는 QUIT 607 뒤(그 답을 받았거나 받는 중)
                const bool expected = role == kRouter ? (p.step != Step::Router || p.pending.empty())
                                    : role == kLobby  ? (p.step == Step::Done || p.step == Step::Quit || IsQueuedRelogin(p.idx))
                                                      : false;
                if (expected) {
                    ++roles_[role].closedExpected;
                    return;
                }
                ++roles_[role].closedUnexpected[e.why];
                ++secClosed_;
                if (role == kRouter) return Fail(p, "라우터 뜻밖 끊김");
                if (role == kLobby) return Fail(p, "로비 뜻밖 끊김");
                return Fail(p, "채팅 뜻밖 끊김");
            }
            case nox::TcpLineEvent::Type::REFUSED:
                ++refused_;
                return;
        }
    }
    bool IsQueuedRelogin(int idx) const { return std::find(pendingRelogin_.begin(), pendingRelogin_.end(), idx) != pendingRelogin_.end(); }

    void Tick(Person& p, Clock::time_point now) {
        // 답 한도 — 앞 요청 하나만 본다(답은 차례대로)
        if (!p.pending.empty() && MsBetween(p.pending.front().sentAt, now) >= opt_.timeoutMs) {
            ++timeouts_[static_cast<int>(p.pending.front().req)];
            ++secTimeouts_;
            const Step s = p.step;
            if (s == Step::Router || s == Step::Login || s == Step::ChatAuth || s == Step::Quit) return Fail(p, "답 없음");
            p.pending.clear();
            Ready(p, now);
            return;
        }
        if (phase_ == 2) {   // 끝 — 쉬는 사람 · 방에 있는 사람은 QUIT(방은 로비가 끊김으로 치운다) · 일 중인 사람은 답을 받은 뒤
            if ((p.step == Step::Ready || p.step == Step::InRoom) && p.pending.empty()) {
                p.relogin = false;
                p.step = Step::Quit;
                Send(kLobby, p.lobby, "QUIT");
                Expect(p, Req::Quit, now, "607");
            }
            return;
        }
        if (p.step == Step::InRoom && now >= p.roomLeave) {
            p.step = Step::Act;
            Send(kLobby, p.lobby, "PART " + p.room);
            Expect(p, Req::RoomPart, now, "user:PART", nullptr, nullptr, p.room, true);
            return;
        }
        if (p.step == Step::Ready && p.active && now >= p.nextAct) DoAct(p, now);
    }

    void PrintSecond(Clock::time_point now) {
        // 다시 로그인 — 로비 접속이 닫힌 사람부터 1 부터
        for (auto it = pendingRelogin_.begin(); it != pendingRelogin_.end();) {
            Person& p = people_[static_cast<size_t>(*it)];
            if (p.lobby == 0) {
                it = pendingRelogin_.erase(it);
                if (phase_ == 2) p.step = Step::Done;   // 끝 단계에 들었다 — 다시 붙지 않고 끝
                else Begin(p, now);
            } else {
                ++it;
            }
        }
        const double cpu = ProcessCpuMs();
        std::vector<float> sorted = secLat_;
        const double p99 = Percentile(sorted, 0.99);
        const double mx = sorted.empty() ? 0.0 : sorted.back();
        const char* phase = phase_ == 0 ? "올림" : phase_ == 1 ? "잼" : "끝";
        std::printf("[더미 구간] 초 %.0f · 단계 %s · 쉬기 %d · 일 %d · 방 %d · 로그인 중 %d · 실패 %d · 끝 %d · 접속 %zu"
                    " · 로비 보낸 줄 %llu 받은 줄 %llu · 채팅 보낸 줄 %llu 받은 줄 %llu · 라우터 보낸 줄 %llu 받은 줄 %llu"
                    " · 답 %zu · 느린 1%% %.1f · 최대 %.1f · 답 없음 %llu · 뜻밖 끊김 %llu · CPU 더미 %.0f\n",
                    MsBetween(start_, now) / 1000.0, phase, CountStep(Step::Ready), CountStep(Step::Act) + CountStep(Step::Quit), CountStep(Step::InRoom),
                    CountStep(Step::Router) + CountStep(Step::Login) + CountStep(Step::ChatAuth) + CountStep(Step::Start), CountStep(Step::Failed),
                    CountStep(Step::Done), tcp_->ConnCount(), secSent_[kLobby], secRecv_[kLobby], secSent_[kChat], secRecv_[kChat], secSent_[kRouter],
                    secRecv_[kRouter], secLat_.size(), p99, mx, secTimeouts_, secClosed_, cpu - cpuAt_);
        secAt_ = now;
        cpuAt_ = cpu;
        secLat_.clear();
        secTimeouts_ = secClosed_ = 0;
        for (int r = 0; r < kRoles; ++r) secSent_[r] = secRecv_[r] = 0;
    }

    void PrintEnd(size_t leftConns) {
        const double measuredSec = phase_ >= 1 ? MsBetween(measureAt_, phase_ == 2 ? endAt_ : Clock::now()) / 1000.0 : 0.0;
        std::string fails;
        unsigned long long failSum = 0;
        for (const auto& kv : failed_) {
            fails += (fails.empty() ? "" : " · ") + kv.first + " " + std::to_string(kv.second);
            failSum += kv.second;
        }
        std::printf("[더미 끝] 사람 %d(활동 %d) · 끝 %d · 실패 %d(%s) · 다시 로그인 %llu · 잰 시간 %.1f초 · 채팅 줄 %llu · PONG %llu · "
                    "방 참가 %llu(거절 471 %llu) · 채팅 주소 없음 %llu · 거절 알림 %llu · 주인 없는 줄 %llu · 끝날 때 남은 접속 %zu\n",
                    opt_.people, activeCount_, CountStep(Step::Done), CountStep(Step::Failed), fails.empty() ? "없음" : fails.c_str(), relogins_,
                    measuredSec, chatSeq_, pongs_, roomJoined_, roomRejected_, noChatAddr_, refused_, strayLines_, leftConns);
        (void)failSum;
        std::string acts;
        for (int a = 0; a < kActCount; ++a) acts += (a ? " · " : "") + std::string(ActName(static_cast<Act>(a))) + " " + std::to_string(acts_[a]);
        std::printf("[더미 끝] 활동 — %s\n", acts.c_str());
        for (int r = 0; r < kRoles; ++r) {
            std::string closed;
            unsigned long long closedSum = 0;
            for (const auto& kv : roles_[r].closedUnexpected) {
                closed += (closed.empty() ? "" : " · ") + kv.first + " " + std::to_string(kv.second);
                closedSum += kv.second;
            }
            std::printf("[더미 끝] %s — 붙음 %llu · 보낸 줄 %llu · 받은 줄 %llu · 붙기 실패 %llu · 기다린 끊김 %llu · 뜻밖 끊김 %llu(%s)\n", RoleName(r),
                        roles_[r].opened, roles_[r].sent, roles_[r].recv, roles_[r].connectFailed, roles_[r].closedExpected, closedSum,
                        closed.empty() ? "없음" : closed.c_str());
        }
        for (int band = 1; band >= 0; --band) {
            std::vector<float> all;
            std::string per;
            for (int r = 0; r < kReqCount; ++r) {
                std::vector<float>& v = lat_[r][band];
                if (v.empty() && timeouts_[r] == 0) continue;
                all.insert(all.end(), v.begin(), v.end());
                const double p50 = Percentile(v, 0.50), p99 = Percentile(v, 0.99), mx = v.empty() ? 0.0 : v.back();
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s%s %zu(가운데 %.1f · 느린 1%% %.1f · 최대 %.1f%s)", per.empty() ? "" : " · ", ReqName(static_cast<Req>(r)),
                              v.size(), p50, p99, mx, (band == 0 && timeouts_[r]) ? (" · 답 없음 " + std::to_string(timeouts_[r])).c_str() : "");
                per += buf;
            }
            const double p50 = Percentile(all, 0.50), p99 = Percentile(all, 0.99), mx = all.empty() ? 0.0 : all.back();
            std::printf("[더미 끝] 답(%s) — 모두 %zu(가운데 %.1f · 느린 1%% %.1f · 최대 %.1f) · %s\n", band ? "잼 구간" : "전체", all.size(), p50, p99, mx,
                        per.empty() ? "없음" : per.c_str());
        }
        unsigned long long toSum = 0;
        for (int r = 0; r < kReqCount; ++r) toSum += timeouts_[r];
        std::printf("[더미 끝] 답 없음 %llu\n", toSum);
        std::fflush(stdout);
    }

    Options opt_;
    std::mt19937_64 rng_;
    std::unique_ptr<nox::CTcpLinesCore> tcp_;
    std::vector<Person> people_;
    std::unordered_map<int, std::pair<int, int>> connOf_;   // 접속 번호 → (사람, 몫)
    std::vector<int> pendingRelogin_;
    int phase_ = 0;   // 0 올림 · 1 잼 · 2 끝
    int activeCount_ = 0;
    double actRateSum_ = 0.0;
    Clock::time_point start_, measureAt_, endAt_, secAt_;
    double cpuAt_ = 0.0;
    RoleStats roles_[kRoles];
    std::vector<float> lat_[kReqCount][2];   // [요청][0 전체 · 1 잼 구간]
    unsigned long long timeouts_[kReqCount] = {};
    unsigned long long acts_[kActCount] = {};
    std::map<std::string, unsigned long long> failed_;
    unsigned long long relogins_ = 0, roomJoined_ = 0, roomRejected_ = 0, noChatAddr_ = 0, refused_ = 0, strayLines_ = 0, pongs_ = 0, chatSeq_ = 0;
    // 초마다 줄 몫
    unsigned long long secSent_[kRoles] = {}, secRecv_[kRoles] = {}, secTimeouts_ = 0, secClosed_ = 0;
    std::vector<float> secLat_;
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    Options o;
    bool haveLobby = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--lobby" && hasNext) {
            if (!ParseHostPort(argv[++i], o.lobbyIp, o.lobbyPort)) { std::printf("[실패] --lobby 주소:포트 — %s\n", argv[i]); return 2; }
            haveLobby = true;
        } else if (a == "--router" && hasNext) {
            if (!ParseHostPort(argv[++i], o.routerIp, o.routerPort)) { std::printf("[실패] --router 주소:포트 — %s\n", argv[i]); return 2; }
        } else if (a == "--people" && hasNext) o.people = std::atoi(argv[++i]);
        else if (a == "--active" && hasNext) o.active = std::atof(argv[++i]);
        else if (a == "--rate-scale" && hasNext) o.rateScale = std::atof(argv[++i]);
        else if (a == "--ramp" && hasNext) o.ramp = std::atoi(argv[++i]);
        else if (a == "--seconds" && hasNext) o.seconds = std::atoi(argv[++i]);
        else if (a == "--hold-sec" && hasNext) o.holdSec = std::atoi(argv[++i]);
        else if (a == "--prefix" && hasNext) o.prefix = argv[++i];
        else if (a == "--chat-lobbies" && hasNext) o.chatLobbies = std::atoi(argv[++i]);
        else if (a == "--seed" && hasNext) o.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--workers" && hasNext) o.workers = std::atoi(argv[++i]);
        else if (a == "--timeout-ms" && hasNext) o.timeoutMs = std::atoi(argv[++i]);
        else { std::printf("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    // 조용히 기본값으로 떨어지지 않게 — 틀린 값이면 뜨지 않는다
    bool prefixOk = !o.prefix.empty() && o.prefix.size() <= 11;   // 이름 = 앞글자 + 5자리 ≤ 16(로비 · 채팅 이름 한도)
    for (const char c : o.prefix)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) prefixOk = false;
    if (!haveLobby || o.people < 1 || o.people > 99999 || o.active < 0.0 || o.active > 1.0 || o.rateScale <= 0.0 || o.ramp < 1 || o.seconds < 0 ||
        o.holdSec < 0 || o.chatLobbies < 1 || o.chatLobbies > 5 || o.workers < 1 || o.timeoutMs < 100 || !prefixOk) {
        std::printf("[멈춤] 인자 — --lobby 필수 · 사람 1~99999 · 활동 0~1 · 배율 > 0 · 올림 ≥ 1 · 대화방 1~5 · 앞글자 영문 1~11자 · 답 한도 ≥ 100ms\n");
        return 2;
    }
    CLoad load(o);
    const int rc = load.Run();
    WSACleanup();
    return rc;
}
