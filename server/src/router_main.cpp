// router_main.cpp — 라우터 실행 파일(목표 구조 v3 첫 층 · `Docs/ROOMS_DESIGN.md` 「그림」 ①).
//
//   nox_router.exe [--config <설정 파일>] [--bind 0.0.0.0] [--port 4005] [--seconds N] [--quiet]
//
// ★ 10-14 플랜 B 작업 8b — 옛 `nox_lobby` 의 4005 몫(로비 찾기)을 떼어 낸 것. 정품 클라가 `servserv.westwood.com:4005` 에 묻는 곳이다.
//   `605` 답으로 채널(`nox_server` 의 4000) 주소 하나를 알려 줄 뿐 — 방 · 로그인 · 게임을 모른다.
//
//   설정 파일(없어도 뜬다)
//     [router] channel = <IPv4>[:<포트>]   605 로 알려 줄 채널. 없으면 클라가 라우터에 붙은 주소:4000(라우터와 채널이 같은 PC)
//     [router] name = <이름>                605 의 '0:<이름>'(기본 Nox_Origin)
//   채널이 둘 이상일 때 고르는 법은 아직 안 정했다(`ROOMS_DESIGN.md` 「안 정한 것」) — 지금은 하나
//
// 소켓만 맡는다 — 받은 줄은 `nox::wol::Router` 에 넘기고 돌려받은 줄을 보낸다. 소켓은 TCP 줄 부품(`nox::CTcpLines` — 작업 8a)
//
// ⚠ --bind 를 0.0.0.0 으로 두면 처음 실행할 때 Windows 방화벽 허용 창이 뜰 수 있다. 같은 PC 시험은 --bind 127.0.0.1
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nox/IniFile.h"
#include "nox/JoinToken.h"   // `ParseIpv4`
#include "nox/Router.h"
#include "nox/TcpLines.h"

namespace {

bool g_quiet = false;

// 줄마다 시각을 붙인다(로비 실행 파일과 같은 꼴)
void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stdout, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

// 클라 줄은 글자가 아닌 바이트를 '.' 로(콘솔이 깨지지 않게) · 우리 기록(한글)은 제어 문자만 거른다
std::string Printable(const std::string& s, bool ours) {
    std::string r = s;
    for (char& c : r) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7f || (!ours && b > 0x7e)) c = '.';
    }
    return r;
}

// `<IPv4>[:<포트>]`
bool ParseChannel(const std::string& s, uint32_t& ip, uint16_t& port) {
    const size_t colon = s.find(':');
    if (!nox::ParseIpv4(s.substr(0, colon), ip) || ip == 0) return false;
    if (colon == std::string::npos) return true;
    const std::string p = s.substr(colon + 1);
    if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return false;
    const int v = std::atoi(p.c_str());
    if (v <= 0 || v > 65535) return false;
    port = static_cast<uint16_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    nox::wol::RouterConfig cfg;
    uint32_t bindIp = INADDR_ANY;
    uint16_t port = nox::wol::kServServPort;
    int seconds = 0;   // 0 이면 끝없이. 자체검사가 정해진 시간 뒤 스스로 끝나게 쓴다
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--quiet") g_quiet = true;
        else if (a == "--bind" && hasNext) {
            if (!nox::ParseIpv4(argv[++i], bindIp)) { Log("[실패] --bind 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--port" && hasNext) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--config" && hasNext) configPath = argv[++i];
        else if (a == "--seconds" && hasNext) seconds = std::atoi(argv[++i]);
        else { Log("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (!configPath.empty()) {
        nox::CIniFile ini;
        if (!ini.LoadFile(configPath)) { Log("[멈춤] 설정 파일을 못 읽었다 — %s\n", ini.Error().c_str()); return 2; }
        if (ini.Has("router", "channel") && !ParseChannel(ini.Get("router", "channel"), cfg.channelIp, cfg.channelPort)) {
            Log("[멈춤] 설정 파일의 [router] channel 이 <IPv4>[:<포트>] 꼴이 아니다 — %s\n", ini.Get("router", "channel").c_str());
            return 2;
        }
        if (ini.Has("router", "name")) cfg.serverName = ini.Get("router", "name");
    }

    nox::CTcpLines tcp;
    {
        std::string err;
        if (!tcp.Listen(bindIp, port, err)) { Log("[실패] %s\n", err.c_str()); return 1; }
    }
    nox::wol::Router router(cfg);
    Log("[라우터] 듣기 %s:%u — 채널 %s:%u · 이름 %s%s\n", nox::wol::IpDotted(bindIp).c_str(), port,
        cfg.channelIp ? nox::wol::IpDotted(cfg.channelIp).c_str() : "(클라가 붙은 주소)", cfg.channelPort, cfg.serverName.c_str(),
        configPath.empty() ? " · 설정 없음" : (" · 설정 " + configPath).c_str());
    std::fflush(stdout);

    unsigned long long nIn = 0, nOut = 0, nConn = 0;
    const auto start = std::chrono::steady_clock::now();
    std::vector<nox::TcpLineEvent> evs;
    std::vector<nox::wol::Out> outs;
    auto onEvent = [&](const nox::TcpLineEvent& e) {
        switch (e.type) {
        case nox::TcpLineEvent::Type::OPENED:
            ++nConn;
            router.OnConnect(e.conn, e.peerIp, e.localIp);
            if (!g_quiet)
                Log("[c%d:%u] 접속 %s → 내 쪽 %s\n", e.conn, e.port, nox::wol::IpDotted(e.peerIp).c_str(),
                    nox::wol::IpDotted(e.localIp).c_str());
            break;
        case nox::TcpLineEvent::Type::LINE:
            ++nIn;
            if (!g_quiet) Log("[c%d:%u] >> %s\n", e.conn, e.port, Printable(e.line, false).c_str());
            outs.clear();
            router.OnLine(e.conn, e.line, outs);
            for (const auto& o : outs) {
                if (!tcp.Send(o.conn, o.line, o.closeAfter)) continue;
                ++nOut;
                if (!g_quiet) Log("[c%d:%u] << %s\n", o.conn, e.port, Printable(o.line, false).c_str());
            }
            for (const auto& l : router.TakeLog()) Log("[라우터] %s\n", Printable(l, true).c_str());
            break;
        case nox::TcpLineEvent::Type::CLOSED:
            router.OnClose(e.conn);
            if (!g_quiet) Log("[c%d:%u] 끊음 — %s\n", e.conn, e.port, e.why);
            break;
        case nox::TcpLineEvent::Type::REFUSED:
            Log("[라우터] 접속이 너무 많아 끊음 (한도 %zu)\n", nox::CTcpLines::kMaxConns);
            break;
        }
    };
    for (;;) {
        if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
        WaitForSingleObject(static_cast<HANDLE>(tcp.WaitHandle()), 100);   // 100ms — --seconds 확인용으로만 깨어난다
        evs.clear();
        tcp.Poll(evs);
        for (const auto& e : evs) onEvent(e);
        evs.clear();
        tcp.Flush(evs);
        for (const auto& e : evs) onEvent(e);
        std::fflush(stdout);
    }
    tcp.CloseAll();
    Log("[라우터] 끝 — 접속 %llu · 받은 줄 %llu · 보낸 줄 %llu\n", nConn, nIn, nOut);
    return 0;
}
