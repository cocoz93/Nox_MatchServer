// lobby_main.cpp — 로비 서버 실행 파일 (MO 2단계, Docs/LOBBY_DESIGN.md).
//
//   nox_lobby.exe --config <설정 파일> [--bind 0.0.0.0] [--servserv-port 4005] [--lobby-port 4000]
//                 [--game-ip 127.0.0.1] [--game-port 18590]
//                 [--map estate] [--mode arena] [--max-players 31] [--seconds N] [--quiet]
//
// ★ 10-10 토큰 작업 3 — 설정 파일(`Docs/TOKEN_DESIGN.md`)
//     [token] key = <16진 64자>           입장 토큰 서명 키 — 게임 서버 설정과 같은 값. 없으면 뜨지 않는다
//     [rooms] <방장 이름> = <게임 방 번호>  목록에 싣는 방(차례대로) — 옛 --room-host · --room-name 자리
//
// 소켓만 맡는다 — 받은 줄은 nox::wol::Lobby 에 넘기고, 돌려받은 줄을 보낸다(게임 서버 main.cpp 루프와 같은 짜임).
//   · 한 스레드 루프. 듣기 둘(로비 찾기 · 로비)과 접속들은 TCP 줄 부품(`nox::CTcpLines` — 작업 8a)이 든다
//     (옛 이 자리의 `select` 루프 · 논블로킹 보내기 버퍼 · CRLF 줄 나누기를 그리로 옮겼다)
//
// ⚠ --bind 를 0.0.0.0 으로 두면 처음 실행할 때 Windows 방화벽 허용 창이 뜰 수 있다.
//   같은 PC 시험(hosts → 127.0.0.1)은 --bind 127.0.0.1 로 충분하다.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nox/IniFile.h"
#include "nox/Lobby.h"
#include "nox/TcpLines.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

bool g_quiet = false;

// 줄마다 시각을 붙인다 — 10-07 사람 확인에서 마지막 「끊음」이 게임 중인지 게임을 닫을 때인지 기록만으로 못 가렸다
void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stdout, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

bool ParseIp(const char* s, uint32_t& out) {
    in_addr a{};
    if (inet_pton(AF_INET, s, &a) != 1) return false;
    out = ntohl(a.s_addr);
    return true;
}

// 맵 종류 이름 → 깃발 칸 값 (WolGameInfo.h kMap*)
bool ParseMode(const std::string& m, uint16_t& bits) {
    static const std::map<std::string, uint16_t> kModes = {
        {"kotr", nox::wol::kMapKotr},   {"ctf", nox::wol::kMapCtf},
        {"flagball", nox::wol::kMapFlagBall}, {"chat", nox::wol::kMapChat},
        {"arena", nox::wol::kMapArena}, {"elimination", nox::wol::kMapElimination},
        {"coop", nox::wol::kMapCoop},   {"quest", nox::wol::kMapQuest},
    };
    const auto it = kModes.find(m);
    if (it == kModes.end()) return false;
    bits = it->second;
    return true;
}

// 방 정보 덩어리처럼 글자가 아닌 바이트는 '.' 로 찍는다(콘솔이 깨지지 않게)
std::string Printable(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) > 0x7e) c = '.';
    return r;
}

// ★ 10-10 토큰 작업 3 — 로비 기록(`TakeLog`)은 우리가 쓴 한글 문구다. 위처럼 0x7e 위를 다 거르면 「[토큰]」이 「[......]」로 찍혀
//   사람이 못 읽는다. 제어 문자만 거른다(콘솔은 UTF-8 — `SetConsoleOutputCP`)
std::string PrintableLog(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) == 0x7f) c = '.';
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    nox::wol::LobbyConfig cfg;
    cfg.room.map = "estate";
    cfg.room.maxPlayers = 31;
    cfg.room.resolution = 2;   // 녹음 다섯 방 중 셋이 1024x768
    uint32_t bindIp = INADDR_ANY;
    int seconds = 0;           // 0 이면 끝없이. 자체검사가 정해진 시간 뒤 스스로 끝나게 쓴다
    std::string mode = "arena";
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--quiet") g_quiet = true;
        else if (a == "--bind" && hasNext) {
            if (!ParseIp(argv[++i], bindIp)) { Log("[실패] --bind 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--servserv-port" && hasNext) cfg.servServPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--lobby-port" && hasNext) cfg.lobbyPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--game-ip" && hasNext) {
            if (!ParseIp(argv[++i], cfg.gameServerIp)) { Log("[실패] --game-ip 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--game-port" && hasNext) cfg.gameServerPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--config" && hasNext) configPath = argv[++i];
        else if (a == "--map" && hasNext) cfg.room.map = argv[++i];
        else if (a == "--mode" && hasNext) mode = argv[++i];
        else if (a == "--max-players" && hasNext) cfg.room.maxPlayers = static_cast<uint8_t>(std::atoi(argv[++i]));
        else if (a == "--seconds" && hasNext) seconds = std::atoi(argv[++i]);
        else { Log("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    uint16_t modeBits = 0;
    if (!ParseMode(mode, modeBits)) { Log("[실패] --mode: %s\n", mode.c_str()); return 2; }
    cfg.room.flags = static_cast<uint16_t>((cfg.room.flags & ~nox::wol::kMapTypeMask) | modeBits);

    // ★ 10-10 토큰 작업 3 — 로비는 토큰 방식 하나뿐이라 키가 없으면 뜨지 않는다(`Docs/TOKEN_DESIGN.md`)
    if (configPath.empty()) { Log("[멈춤] --config <설정 파일> 이 없다 — [token] key · [rooms] 가 있어야 뜬다\n"); return 2; }
    nox::CIniFile ini;
    if (!ini.LoadFile(configPath)) { Log("[멈춤] 설정 파일을 못 읽었다 — %s\n", ini.Error().c_str()); return 2; }
    if (!nox::ParseJoinKey(ini.Get("token", "key"), cfg.joinKey)) {
        Log("[멈춤] 설정 파일의 [token] key 가 16진 64자가 아니다 — %s\n", configPath.c_str());
        return 2;
    }
    cfg.rooms.clear();
    for (const auto& kv : ini.Section("rooms")) {
        const int id = std::atoi(kv.second.c_str());
        const bool digits = !kv.second.empty() && kv.second.find_first_not_of("0123456789") == std::string::npos;
        if (!digits || id < 0 || id > 65535 || kv.first.find_first_of(" ',#") != std::string::npos) {
            Log("[멈춤] 설정 파일의 [rooms] 줄이 틀렸다 — %s = %s (방장 이름 = 게임 방 번호, 이름에 빈칸·' , # 없이)\n",
                kv.first.c_str(), kv.second.c_str());
            return 2;
        }
        cfg.rooms.push_back({kv.first, static_cast<uint16_t>(id)});
    }
    if (cfg.rooms.empty()) { Log("[멈춤] 설정 파일에 [rooms] 가 없다 — 방장 이름 = 게임 방 번호 한 줄 이상\n"); return 2; }

    nox::CTcpLines tcp;   // ★ 10-14 작업 8a — 듣기 둘 · 접속들(옛 이 자리의 `WSAStartup` · `Listen` 둘)
    {
        std::string err;
        if (!tcp.Listen(bindIp, cfg.servServPort, err) || !tcp.Listen(bindIp, cfg.lobbyPort, err)) {
            Log("[실패] %s\n", err.c_str());
            return 1;
        }
    }

    nox::wol::Lobby lobby(cfg);
    std::string roomList;
    for (const auto& r : lobby.Rooms())
        roomList += (roomList.empty() ? "" : ", ") + nox::wol::Lobby::Channel(r) + "=게임 방 " + std::to_string(r.gameRoomId);
    Log("[로비] 듣기 %s — 로비 찾기 :%u · 로비 :%u · 방 %s → 게임 서버 %s:%u · 맵 %s · %s · 최대 %u · 입장 토큰(설정 %s)\n",
                nox::wol::IpDotted(bindIp).c_str(), cfg.servServPort, cfg.lobbyPort, roomList.c_str(),
                nox::wol::IpDotted(cfg.gameServerIp).c_str(), cfg.gameServerPort, cfg.room.map.c_str(), mode.c_str(),
                cfg.room.maxPlayers, configPath.c_str());
    std::fflush(stdout);

    unsigned long long nIn = 0, nOut = 0, nConn = 0;
    const auto start = std::chrono::steady_clock::now();
    std::vector<nox::TcpLineEvent> evs;
    std::vector<nox::wol::Out> outs;

    // 사건 하나 — 새 접속 · 받은 줄(로비에 넘기고 돌려받은 줄을 보내기 버퍼에) · 끊김
    auto onEvent = [&](const nox::TcpLineEvent& e) {
        switch (e.type) {
        case nox::TcpLineEvent::Type::OPENED:
            ++nConn;
            lobby.OnConnect(e.conn, e.port, e.peerIp, e.localIp);
            if (!g_quiet)
                Log("[c%d:%u] 접속 %s → 내 쪽 %s\n", e.conn, e.port, nox::wol::IpDotted(e.peerIp).c_str(),
                    nox::wol::IpDotted(e.localIp).c_str());
            break;
        case nox::TcpLineEvent::Type::LINE:
            ++nIn;
            if (!g_quiet) Log("[c%d:%u] >> %s\n", e.conn, e.port, Printable(e.line).c_str());
            outs.clear();
            lobby.OnLine(e.conn, e.line, outs);
            for (const auto& o : outs) {
                if (!tcp.Send(o.conn, o.line, o.closeAfter)) continue;
                ++nOut;
                if (!g_quiet) Log("[c%d:%u] << %s\n", o.conn, e.port, Printable(o.line).c_str());
            }
            for (const auto& l : lobby.TakeLog()) Log("[로비] %s\n", PrintableLog(l).c_str());
            break;
        case nox::TcpLineEvent::Type::CLOSED:
            lobby.OnClose(e.conn);
            if (!g_quiet) Log("[c%d:%u] 끊음 — %s\n", e.conn, e.port, e.why);
            break;
        case nox::TcpLineEvent::Type::REFUSED:
            Log("[로비] 접속이 너무 많아 끊음 (한도 %zu)\n", nox::CTcpLines::kMaxConns);
            break;
        }
    };

    for (;;) {
        if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
        // 100ms — 로비엔 틱이 없다. --seconds 확인용으로만 깨어난다
        WaitForSingleObject(static_cast<HANDLE>(tcp.WaitHandle()), 100);
        evs.clear();
        tcp.Poll(evs);
        for (const auto& e : evs) onEvent(e);
        evs.clear();
        tcp.Flush(evs);   // 보내기 오류 · QUIT 답 뒤 끊기가 여기서 나온다
        for (const auto& e : evs) onEvent(e);
        std::fflush(stdout);
    }

    tcp.CloseAll();
    Log("[로비] 끝 — 접속 %llu · 받은 줄 %llu · 보낸 줄 %llu\n", nConn, nIn, nOut);
    return 0;
}
