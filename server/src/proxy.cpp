// proxy.cpp — 클라와 정품 호스트 사이에 끼어 오가는 것을 전부 기록하는 중간자.
//
//   [정품 클라] ──▶ [이 중간자 18590] ──▶ [정품 호스트 18591]
//
// 정품 호스트는 `Game.exe -serveronly -port 18591` 로 띄운다(동작 확인 완료).
// 클라는 평소대로 18590 을 찾으므로 아무 수정 없이 여기로 들어온다.
//
// 답을 되돌릴 때는 반드시 **듣는 소켓(18590)** 으로 보낸다.
// 클라는 자기가 보낸 곳에서 답이 오기를 기대한다.
//
//   nox_proxy.exe [--listen 18590] [--host 127.0.0.1] [--host-port 18591]
//                 [--dump 파일] [--quiet]
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <share.h>
#include <map>
#include <string>
#include <vector>

#include "nox/Opcode.h"
#include "nox/Recording.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

struct Peer {
    SOCKET up = INVALID_SOCKET;   // 호스트와 이야기하는 소켓
    sockaddr_in addr{};           // 클라 주소
    unsigned long long toHost = 0;
    unsigned long long toClient = 0;
    // U3.1 — 접속 수락을 내가 만들어 보낼 때 쓴다
    uint8_t hostKey = 0;          // 호스트가 정한 XOR 키
    uint8_t myKey = 0;            // 내가 클라에게 준 XOR 키
    bool keyed = false;           // 수락이 오간 뒤인가
};

// 클라를 구분하는 열쇠 — 주소와 포트
using PeerKey = unsigned long long;

PeerKey MakeKey(const sockaddr_in& a) {
    return (static_cast<unsigned long long>(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port);
}

std::string NowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::string HexOf(const uint8_t* p, int n) {
    std::string s;
    s.reserve(static_cast<size_t>(n) * 3);
    char b[4];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x ", p[i]);
        s += b;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

FILE* g_dump = nullptr;
bool g_quiet = false;
bool g_ownAccept = false;     // U3.1 — 접속 수락을 내가 만들어 보낸다
uint8_t g_myKey = 0x42;       // 그때 클라에게 주는 XOR 키 (호스트 것과 일부러 다르게)
std::string g_mapName;        // U3.2 — 맵 지정(0x2b) 의 이름 칸을 내가 쓴다
std::map<uint8_t, unsigned long long> g_opCount;   // U2.7 에서 쓸 오프코드 집계

// U3.2 — 맵 이름 칸을 내 것으로 바꾼다. 길이가 같을 때만(뒤 칸이 밀리면 안 된다).
// 맵 이름은 두 군데에 실린다: 0x2b 의 3번, 0xb8 의 10번.
bool PatchMapName(uint8_t* p, int n, int off, const std::string& name) {
    if (n < off + static_cast<int>(name.size()) + 1) return false;
    const size_t was = std::strlen(reinterpret_cast<char*>(p) + off);
    if (was != name.size()) return false;
    std::memcpy(p + off, name.data(), name.size());
    return true;
}

void Log(const char* dir, const sockaddr_in& who, const uint8_t* p, int n) {
    const uint8_t op = (n >= static_cast<int>(nox::kHeaderSize)) ? p[2] : 0xFF;
    if (n >= static_cast<int>(nox::kHeaderSize)) ++g_opCount[op];

    char ip[64] = {};
    inet_ntop(AF_INET, &who.sin_addr, ip, sizeof(ip));

    char head[160];
    std::snprintf(head, sizeof(head), "%s %s %s:%d len=%d op=0x%02x %s",
                  NowStamp().c_str(), dir, ip, ntohs(who.sin_port), n, op,
                  nox::OpName(static_cast<nox::Op>(op)));
    if (!g_quiet) {
        std::printf("%s\n  %s\n", head, HexOf(p, n).c_str());
        std::fflush(stdout);
    }
    if (g_dump) {
        std::fprintf(g_dump, "%s\n  %s\n", head, HexOf(p, n).c_str());
        std::fflush(g_dump);
    }
}

void PrintSummary() {
    std::printf("\n=== 오프코드 집계 ===\n");
    for (const auto& kv : g_opCount) {
        std::printf("  0x%02x  %-34s %llu\n", kv.first,
                    nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
    }
    std::printf("  종류 %zu 가지\n", g_opCount.size());
    std::fflush(stdout);
    if (g_dump) {
        std::fprintf(g_dump, "\n=== 오프코드 집계 ===\n");
        for (const auto& kv : g_opCount) {
            std::fprintf(g_dump, "  0x%02x  %-34s %llu\n", kv.first,
                         nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
        }
        std::fprintf(g_dump, "  종류 %zu 가지\n", g_opCount.size());
        std::fflush(g_dump);
    }
}

BOOL WINAPI OnConsoleEvent(DWORD) {
    PrintSummary();
    if (g_dump) std::fclose(g_dump);
    return FALSE;   // 기본 처리(종료)로 넘긴다
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    int listenPort = 18590;
    int hostPort = 18591;
    std::string hostIp = "127.0.0.1";
    std::string dumpPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--listen" && i + 1 < argc) listenPort = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) hostIp = argv[++i];
        else if (a == "--host-port" && i + 1 < argc) hostPort = std::atoi(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--quiet") g_quiet = true;
        else if (a == "--own-accept") g_ownAccept = true;
        else if (a == "--map" && i + 1 < argc) g_mapName = argv[++i];
        else if (a == "--my-key" && i + 1 < argc) g_myKey = static_cast<uint8_t>(std::strtoul(argv[++i], nullptr, 0));
        else { std::printf("모르는 인자: %s\n", a.c_str()); return 2; }
    }

    if (!dumpPath.empty()) {
        // 돌고 있는 동안에도 남이 읽을 수 있게 공유해서 연다
        g_dump = _fsopen(dumpPath.c_str(), "wb", _SH_DENYWR);
        if (!g_dump) {
            std::printf("기록 파일을 열지 못했다: %s\n", dumpPath.c_str());
            return 1;
        }
    }
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 실패\n");
        return 1;
    }

    SOCKET down = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (down == INVALID_SOCKET) { std::printf("소켓 실패\n"); return 1; }
    BOOL yes = TRUE;
    setsockopt(down, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(static_cast<uint16_t>(listenPort));
    if (bind(down, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        std::printf("bind 실패 (%d). 포트 %d 를 이미 누가 쓰고 있는가?\n",
                    WSAGetLastError(), listenPort);
        return 1;
    }

    sockaddr_in hostAddr{};
    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons(static_cast<uint16_t>(hostPort));
    inet_pton(AF_INET, hostIp.c_str(), &hostAddr.sin_addr);

    std::printf("중간자 — 클라 UDP %d  ->  호스트 %s:%d%s\n", listenPort,
                hostIp.c_str(), hostPort,
                dumpPath.empty() ? "" : ("  기록: " + dumpPath).c_str());
    std::fflush(stdout);

    std::map<PeerKey, Peer> peers;
    uint8_t buf[4096];

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(down, &rd);
        for (auto& kv : peers) FD_SET(kv.second.up, &rd);

        timeval tv{};
        tv.tv_sec = 1;
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;

        // 클라 → 호스트
        if (FD_ISSET(down, &rd)) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(down, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0) {
                const PeerKey key = MakeKey(from);
                auto it = peers.find(key);
                if (it == peers.end()) {
                    Peer np;
                    np.addr = from;
                    np.up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    if (np.up != INVALID_SOCKET) {
                        sockaddr_in any{};
                        any.sin_family = AF_INET;
                        any.sin_addr.s_addr = INADDR_ANY;
                        bind(np.up, reinterpret_cast<sockaddr*>(&any), sizeof(any));
                        it = peers.emplace(key, np).first;
                        char ip[64] = {};
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                        std::printf("[새 클라] %s:%d\n", ip, ntohs(from.sin_port));
                        std::fflush(stdout);
                    }
                }
                if (it != peers.end()) {
                    // 기록은 클라 쪽 키 공간으로 통일한다. 번역은 기록 뒤에.
                    Log("C->S", from, buf, n);
                    if (g_ownAccept && it->second.keyed) {
                        const uint8_t d = static_cast<uint8_t>(it->second.hostKey ^ it->second.myKey);
                        if (d) for (int i = 0; i < n; ++i) buf[i] ^= d;
                    }
                    sendto(it->second.up, reinterpret_cast<const char*>(buf), n, 0,
                           reinterpret_cast<sockaddr*>(&hostAddr), sizeof(hostAddr));
                    ++it->second.toHost;
                }
            }
        }

        // 호스트 → 클라
        for (auto& kv : peers) {
            if (!FD_ISSET(kv.second.up, &rd)) continue;
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = recvfrom(kv.second.up, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) continue;
            if (g_ownAccept) {
                if (!kv.second.keyed && n == 10 &&
                    buf[2] == static_cast<uint8_t>(nox::Op::ACCEPTED)) {
                    kv.second.hostKey = buf[9];
                    kv.second.myKey = g_myKey;
                    kv.second.keyed = true;
                    // ★ 호스트 것을 고쳐 쓰지 않고 10바이트를 처음부터 내가 조립한다.
                    //   실측 3세션에서 키 자리(9번)만 다르고 나머지는 늘 같았다.
                    const uint8_t mine[10] = {0x80, 0x00, 0x1f, 0x00, 0x01,
                                              0x01, 0x00, 0x00, 0x00, kv.second.myKey};
                    std::memcpy(buf, mine, sizeof(mine));
                    n = static_cast<int>(sizeof(mine));
                    if (g_dump) {
                        std::fprintf(g_dump, "  [수락을 내 것으로 — 호스트키 0x%02x -> 내 키 0x%02x]\n",
                                     kv.second.hostKey, kv.second.myKey);
                        std::fflush(g_dump);
                    }
                    if (!g_quiet)
                        std::printf("[수락을 내 것으로 — 호스트키 0x%02x -> 내 키 0x%02x]\n",
                                    kv.second.hostKey, kv.second.myKey);
                } else if (kv.second.keyed) {
                    // ★ 호스트 키로 풀어 평문으로 본다 → 손댈 것을 손대고 → 내 키로 다시 잠근다.
                    //   (예전 코드는 잠긴 상태의 buf[2] 를 오프코드로 착각했다)
                    if (kv.second.hostKey)
                        for (int i = 0; i < n; ++i) buf[i] ^= kv.second.hostKey;

                    // U3.2 — 맵 지정은 내가 쓴다. 이름이 실리는 자리는 두 군데다.
                    if (!g_mapName.empty() && n > 12) {
                        int off = -1;
                        if (buf[2] == static_cast<uint8_t>(nox::Op::USE_MAP)) off = 3;
                        else if (buf[2] == 0xb8) off = 10;   // 맵 정보(90바이트). 이름 10번
                        if (off >= 0) {
                            const bool ok = PatchMapName(buf, n, off, g_mapName);
                            if (g_dump) {
                                std::fprintf(g_dump, "  [맵 이름 %s — 0x%02x 의 %d번 → %s]\n",
                                             ok ? "바꿈" : "못 바꿈(길이 다름)", buf[2], off,
                                             g_mapName.c_str());
                                std::fflush(g_dump);
                            }
                            if (!g_quiet)
                                std::printf("[맵 이름 %s — 0x%02x 의 %d번 → %s]\n",
                                            ok ? "바꿈" : "못 바꿈(길이 다름)", buf[2], off,
                                            g_mapName.c_str());
                        }
                    }

                    if (kv.second.myKey)
                        for (int i = 0; i < n; ++i) buf[i] ^= kv.second.myKey;
                }
            }
            Log("S->C", kv.second.addr, buf, n);
            // 반드시 듣는 소켓으로 되돌린다. 클라는 18590 에서 답이 오길 기대한다.
            sendto(down, reinterpret_cast<const char*>(buf), n, 0,
                   reinterpret_cast<sockaddr*>(&kv.second.addr), sizeof(kv.second.addr));
            ++kv.second.toClient;
        }
    }
}
