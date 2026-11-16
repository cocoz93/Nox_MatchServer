// wsock32 프록시 시험 — 이 exe 옆의 wsock32.dll(프록시)이 실려도 결과가 진짜와 같은가, 보는 일곱이 로그에 남는가
//
// 사용: selftest.exe log   <로그 폴더>   — NOX_HOOK_LOG_DIR 를 켠 채로. 동작 + 로그 줄을 본다
//       selftest.exe nolog <로그 폴더>   — 끈 채로. 동작은 같고 로그 파일이 안 생기는지 본다
// 빠지면 안 되는 것:
//   - 실린 wsock32.dll 이 이 폴더의 프록시인가(시스템 것이면 시험이 아무것도 안 본다)
//   - 넘김 함수의 번호 짝(htons·htonl·ntohs) — 엉뚱한 번호로 뛰면 값이 달라진다
//   - 막힘 없는 connect 뒤 WSAGetLastError == WSAEWOULDBLOCK — 프록시가 로그를 쓰며 오류 값을 덮으면 깨진다
//   - UDP 0x0e 100바이트 왕복 · TCP 글자 줄 왕복 · gethostbyname

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "../common/LaunchKey.h"   // ★ 런처 P4 — 열쇠 파일을 시험 폴더에 두고 지움

static int g_fail = 0;

static void Check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static sockaddr_in Loopback(u_short port)
{
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(port);
    return a;
}

// 받기 제한 시간 — 넘김 번호가 틀려 포트가 어긋나면 받기가 끝없이 기다린다. 멈추지 말고 FAIL 로 끝나게
static void RecvTimeout(SOCKET s)
{
    int ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

static u_short PortOf(SOCKET s)
{
    sockaddr_in a; int len = sizeof(a);
    if (getsockname(s, (sockaddr*)&a, &len) != 0) return 0;
    return ntohs(a.sin_port);
}

// ★ 로그인 암호 조각 5 의 3 — 가짜 로그인 서버(평문 HTTP · 한 번만 받는다). 요청 전체를 request 에 담고 reply 를 200 으로 돌려준다
struct FakeAuth {
    SOCKET ls = INVALID_SOCKET;
    std::string request;
    std::string reply;
    int status = 200;   // ★ 귓속말 조각 4 — 401 거절 답도
};

static DWORD WINAPI FakeAuthMain(LPVOID p)
{
    FakeAuth* f = (FakeAuth*)p;
    SOCKET c = accept(f->ls, NULL, NULL);
    if (c == INVALID_SOCKET) return 1;
    RecvTimeout(c);
    std::string req;
    for (;;) {
        const size_t he = req.find("\r\n\r\n");
        if (he != std::string::npos) {
            size_t cl = 0;
            const size_t k = req.find("Content-Length:");
            if (k != std::string::npos) cl = (size_t)atoi(req.c_str() + k + 15);
            if (req.size() >= he + 4 + cl) break;
        }
        char b[512]; int r = recv(c, b, sizeof(b), 0);
        if (r <= 0) break;
        req.append(b, r);
    }
    f->request = req;
    const std::string resp = std::string(f->status == 200 ? "HTTP/1.1 200 OK" : "HTTP/1.1 401 Unauthorized") +
                             "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(f->reply.size()) +
                             "\r\nConnection: close\r\n\r\n" + f->reply;
    send(c, resp.data(), (int)resp.size(), 0);
    closesocket(c);
    return 0;
}

static bool FileHas(const char* path, const char* needle)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

// 로그 파일에서 lineMark 가 든 줄 가운데 needle 도 든 줄이 있나. 이 시험은 한 프로세스에서 서버 쪽까지 맡아 「서버가 받은 글」이 recv 줄로 찍히므로
//   (실제 클라는 서버가 로그인 줄을 되돌려 주지 않는다) 암호 검사는 보낸 쪽 줄(` send s=`)만 본다
static bool FileLineHas(const char* path, const char* lineMark, const char* needle)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (char* line = buf; *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        if (strstr(line, lineMark) && strstr(line, needle)) return true;
        if (!end) break;
        line = end + 1;
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 3 || (strcmp(argv[1], "log") != 0 && strcmp(argv[1], "nolog") != 0)) {
        printf("usage: selftest.exe log|nolog <logdir>\n");
        return 2;
    }
    const bool wantLog = strcmp(argv[1], "log") == 0;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);   // ★ 귓속말 조각 4 — 둘째 연결 시험이 중간에 기록을 읽어 앞으로 옮김
    const char* base = strrchr(exe, '\\') + 1;
    char logPath[MAX_PATH]; sprintf(logPath, "%s\\wsock_%s_%lu.log", argv[2], base, GetCurrentProcessId());

    // 실린 wsock32.dll 이 이 폴더의 것인가
    char self[MAX_PATH], ws[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, self, MAX_PATH);
    *strrchr(self, '\\') = 0;
    HMODULE hw = GetModuleHandleA("wsock32.dll");
    if (hw) GetModuleFileNameA(hw, ws, MAX_PATH);
    printf("loaded wsock32 = %s\n", ws);
    char want[MAX_PATH]; sprintf(want, "%s\\wsock32.dll", self);
    Check(_stricmp(ws, want) == 0, "wsock32.dll is the proxy next to this exe");

    WSADATA wd;
    Check(WSAStartup(MAKEWORD(1, 1), &wd) == 0, "WSAStartup");

    // 넘김 번호 짝 — 바이트 순서 함수 넷은 값으로 갈린다
    Check(htons(0x1234) == 0x3412, "htons (ord 9)");
    Check(ntohs(0x3412) == 0x1234, "ntohs (ord 15)");
    Check(htonl(0x11223344UL) == 0x44332211UL, "htonl (ord 8)");
    Check(ntohl(0x44332211UL) == 0x11223344UL, "ntohl (ord 14)");

    // UDP — 접속 시도 모양(머리 00 00 0e) 100바이트를 보내 그대로 받는가
    SOCKET ua = socket(AF_INET, SOCK_DGRAM, 0), ub = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in any = Loopback(0);
    Check(bind(ua, (sockaddr*)&any, sizeof(any)) == 0 && bind(ub, (sockaddr*)&any, sizeof(any)) == 0, "udp bind");
    RecvTimeout(ua);
    char pkt[100]; memset(pkt, 0, sizeof(pkt));
    pkt[2] = 0x0e; pkt[3] = 0; pkt[4] = 'J'; pkt[6] = 'a'; pkt[8] = 'c'; pkt[10] = 'k';
    sockaddr_in toA = Loopback(PortOf(ua));
    Check(sendto(ub, pkt, sizeof(pkt), 0, (sockaddr*)&toA, sizeof(toA)) == (int)sizeof(pkt), "sendto 100B");
    char got[256]; sockaddr_in from; int fromLen = sizeof(from);
    int n = recvfrom(ua, got, sizeof(got), 0, (sockaddr*)&from, &fromLen);
    Check(n == (int)sizeof(pkt) && memcmp(got, pkt, sizeof(pkt)) == 0, "recvfrom same 100B");
    Check(ntohs(from.sin_port) == PortOf(ub), "recvfrom source port");

    // TCP — 막힘 없는 connect 는 WSAEWOULDBLOCK 이어야 한다(프록시가 로그 쓰다 오류 값을 덮으면 여기서 깨진다)
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    Check(bind(ls, (sockaddr*)&any, sizeof(any)) == 0 && listen(ls, 1) == 0, "tcp listen");
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    u_long nb = 1; ioctlsocket(c, FIONBIO, &nb);
    sockaddr_in toL = Loopback(PortOf(ls));
    int rc = connect(c, (sockaddr*)&toL, sizeof(toL));
    int err = WSAGetLastError();
    printf("nonblocking connect rc=%d err=%d\n", rc, err);
    Check(rc == SOCKET_ERROR && err == WSAEWOULDBLOCK, "nonblocking connect -> WSAEWOULDBLOCK kept");
    fd_set wf; FD_ZERO(&wf); FD_SET(c, &wf);
    timeval tv = { 2, 0 };
    const bool connected = select(0, NULL, &wf, NULL, &tv) == 1;
    Check(connected, "select writable");
    SOCKET sv = INVALID_SOCKET;
    if (connected) {   // 안 붙었으면 accept 가 끝없이 기다리므로 건너뛰고 FAIL 로 센다
        nb = 0; ioctlsocket(c, FIONBIO, &nb);
        sv = accept(ls, NULL, NULL);
        Check(sv != INVALID_SOCKET, "accept");
        RecvTimeout(sv);
        const char line[] = "whereto wn9457 pw7Kx9 9472 65538 00000000000000000000\r\n";   // 암호 칸 pw7Kx9 — 로그에 남으면 안 된다(아래 log 검사)
        Check(send(c, line, (int)strlen(line), 0) == (int)strlen(line), "send line");
        char rbuf[256]; int got2 = 0;
        while (got2 < (int)strlen(line)) {
            int r = recv(sv, rbuf + got2, sizeof(rbuf) - 1 - got2, 0);
            if (r <= 0) break;
            got2 += r;
        }
        Check(got2 == (int)strlen(line) && memcmp(rbuf, line, got2) == 0, "recv same line");
    } else {
        Check(false, "accept/send/recv skipped (not connected)");
    }

    hostent* h = gethostbyname("localhost");
    Check(h != NULL, "gethostbyname localhost");

    // ★ 귓속말 조각 4 — 아무도 안 듣는 포트(받아 두고 바로 닫음). NOXCHAT 이 이제 둘째 연결을 띄우므로 진짜 채팅 서버(4020)가 떠 있는 PC 에서도 시험이 그쪽에 안 붙게
    auto deadPort = [&]() {
        SOCKET d = socket(AF_INET, SOCK_STREAM, 0);
        bind(d, (sockaddr*)&any, sizeof(any));
        const u_short p = PortOf(d);
        closesocket(d);
        return p;
    };
    const u_short deadChat1 = deadPort();

    // ── 10-10 토큰 작업 4 — 로비 연결(첫 보내기 CVERS) · NOXTOKEN 떼기 · 게임 서버로 가는 접속 시도·CONNECT 꼬리 ──
    {
        SOCKET game = socket(AF_INET, SOCK_DGRAM, 0), other = socket(AF_INET, SOCK_DGRAM, 0), cli = socket(AF_INET, SOCK_DGRAM, 0);
        bind(game, (sockaddr*)&any, sizeof(any)); bind(other, (sockaddr*)&any, sizeof(any)); bind(cli, (sockaddr*)&any, sizeof(any));
        RecvTimeout(game); RecvTimeout(other);
        const u_short gamePort = PortOf(game), otherPort = PortOf(other);
        unsigned char raw[53];   // ★ 11-16 판 2 토큰(`JoinToken.h` kJoinTokenSize — 방 칸 4바이트 · 옛 51)
        char hex[107];
        for (int i = 0; i < 53; ++i) { raw[i] = (unsigned char)(i * 5 + 1); sprintf(hex + 2 * i, "%02x", raw[i]); }

        SOCKET lsl = socket(AF_INET, SOCK_STREAM, 0);
        bind(lsl, (sockaddr*)&any, sizeof(any)); listen(lsl, 1);
        SOCKET lc = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in toLobby = Loopback(PortOf(lsl));
        Check(connect(lc, (sockaddr*)&toLobby, sizeof(toLobby)) == 0, "lobby: connect");
        SOCKET lsv = accept(lsl, NULL, NULL);
        RecvTimeout(lsv); RecvTimeout(lc);
        const char cvers[] = "CVERS 11020 9472\r\n";
        Check(send(lc, cvers, (int)strlen(cvers), 0) == (int)strlen(cvers), "lobby: send CVERS returns its own length");
        std::string atLobby;
        while (atLobby.find("CVERS") == std::string::npos) {
            char b[256]; int r = recv(lsv, b, sizeof(b), 0);
            if (r <= 0) break;
            atLobby.append(b, r);
        }
        Check(atLobby == std::string("NOXCLIENT 7\r\n") + cvers, "lobby: NOXCLIENT 7 goes out before CVERS");   // ★ 매칭 손보기 6 — 판 5(수락 단계) · ★ 11-06 판 7(귓속말)

        // ★ 11-16 판 2 토큰 — 옛 꼴(16진 102자 = 토큰 51) 줄은 떼되 쥐지 않는다 → 게임 서버로 가는 접속 시도에 꼬리가 안 붙는다
        {
            const std::string oldLine = std::string("NOXTOKEN 127.0.0.1:") + std::to_string(gamePort) + " " + std::string(hex, 102) + "\r\n: 375 u\r\n";
            send(lsv, oldLine.data(), (int)oldLine.size(), 0);
            std::string og;
            while (og.size() < 9) {
                char b[64]; int r = recv(lc, b, sizeof(b), 0);
                if (r <= 0) break;
                og.append(b, r);
            }
            char j0[100]; memset(j0, 0, sizeof(j0)); j0[2] = 0x0e; j0[4] = 'J';
            sockaddr_in tg = Loopback(gamePort);
            sendto(cli, j0, sizeof(j0), 0, (sockaddr*)&tg, sizeof(tg));
            char gb0[512]; const int n0 = recvfrom(game, gb0, sizeof(gb0), 0, NULL, NULL);
            Check(og == ": 375 u\r\n" && n0 == 100, "token v2: old 102-hex NOXTOKEN line removed, not held (join to game server = 100, no tail)");
        }

        // 토큰 줄을 둘로 나눠 보낸다(TCP 가 줄을 자르는 경우) — 클라는 토큰 줄 없이 앞뒤 줄만 받아야 한다
        const std::string line = std::string("NOXTOKEN 127.0.0.1:") + std::to_string(gamePort) + " " + hex + "\r\n";
        const std::string part1 = ": 376 u\r\n" + line.substr(0, 20);
        const std::string part2 = line.substr(20) + ":wn!u@h JOINGAME 1 32 37 1 0 2130706433 1 :#NoxOrigin's_game\r\n";
        send(lsv, part1.data(), (int)part1.size(), 0);
        Sleep(60);
        send(lsv, part2.data(), (int)part2.size(), 0);
        const std::string want = ": 376 u\r\n:wn!u@h JOINGAME 1 32 37 1 0 2130706433 1 :#NoxOrigin's_game\r\n";
        std::string got;
        while (got.size() < want.size()) {
            char b[64]; int r = recv(lc, b, sizeof(b), 0);   // 작은 버퍼 — 줄이 여러 번에 나뉘어도
            if (r <= 0) break;
            got.append(b, r);
        }
        Check(got == want, "lobby: NOXTOKEN line removed even when split across TCP segments");

        // ★ 10-16 작업 11 — 우리 로비 표시(NOXSERVER)는 떼고, 방 목록 끝 앞에 내 방 줄을 끼우나.
        //   끼우기는 판 확인(Game.exe)을 통과하거나 시험 스위치 NOX_MYROOM_LINE=1 일 때만 — 이 exe 는 Game.exe 가 아니므로 스위치로만 켜진다
        const char nick[] = "NICK selftst\r\n";
        send(lc, nick, (int)strlen(nick), 0);
        std::string atLobby2;
        while (atLobby2.size() < strlen(nick)) {
            char b[64]; int r = recv(lsv, b, sizeof(b), 0);
            if (r <= 0) break;
            atLobby2.append(b, r);
        }
        // ★ 로그인 암호 조각 5 의 3 → ★ 런처 P4 — 암호 줄이 두 번에 걸쳐 나가도(정품은 묶음을 64바이트씩 쪼갠다) 친 암호는 어디에도 안 나간다.
        //   런처 없이(NOX_LAUNCH 없음) → 로그인 서버를 안 부르고 apgar 자리 = NXL!launcher_required ·
        //   런처로(NOX_LAUNCH=이름 + 그 이름의 열쇠 파일) → POST /token(이름 · 열쇠) · 선에는 서버 답(토큰) · 열쇠 파일 없음 → NXL!key_invalid ·
        //   못 닿음 → NXL!unreachable. 게임엔 원래 길이를 돌려준다. 기록에는 암호가 안 남는다(아래 log 검사)
        {
            auto wireLine = [&](const std::string& want) {
                std::string got;
                while (got.size() < want.size()) {
                    char b[64]; int r = recv(lsv, b, sizeof(b), 0);
                    if (r <= 0) break;
                    got.append(b, r);
                }
                return got;
            };
            char tmpDir[MAX_PATH];
            GetTempPathA(MAX_PATH, tmpDir);
            const std::string appData = std::string(tmpDir) + "nox_selftest_appdata_" + std::to_string(GetCurrentProcessId());
            SetEnvironmentVariableA("LOCALAPPDATA", appData.c_str());   // 열쇠 파일을 사용자 프로필 밖에(LaunchKey.h)
            SetEnvironmentVariableA("NOX_LAUNCH", NULL);

            // ① 런처 없이 — 로그인 서버 안 부름
            const char a1[] = "apgar Sec7", a2[] = "ret9 0\r\n";
            Check(send(lc, a1, (int)strlen(a1), 0) == (int)strlen(a1), "login: half apgar line — caller gets its own length");
            const char oth[] = "NOOP interleaved\r\n";
            send(c, oth, (int)strlen(oth), 0);   // 다른 소켓의 보내기가 끼어들어도 로비 소켓의 덜 온 줄 상태와 섞이면 안 된다
            Check(send(lc, a2, (int)strlen(a2), 0) == (int)strlen(a2), "login: rest of the apgar line — caller gets its own length though the wire line is longer");
            const std::string wantL = "apgar NXL!launcher_required 0\r\n";
            Check(wireLine(wantL) == wantL, "login: no launcher (NOX_LAUNCH unset) -> apgar field = NXL!launcher_required, login server not called");

            // ② 런처로 — 열쇠로 POST /token
            const std::string key = "NXK1" + std::string(64, 'c');
            SetEnvironmentVariableA("NOX_LAUNCH", "selftst");
            Check(launchkey::Save("selftst", key), "login: launcher key saved under the temp LOCALAPPDATA (DPAPI)");
            SOCKET als = socket(AF_INET, SOCK_STREAM, 0);
            bind(als, (sockaddr*)&any, sizeof(any)); listen(als, 1);
            FakeAuth fa;
            fa.ls = als;
            fa.reply = "NXL1" + std::string(114, 'a');
            const std::string url = "http://127.0.0.1:" + std::to_string(PortOf(als));   // 기준 주소(길은 DLL 이 붙임)
            SetEnvironmentVariableA("NOX_AUTH_URL", url.c_str());
            HANDLE th = CreateThread(NULL, 0, FakeAuthMain, &fa, 0, NULL);
            const char a3[] = "apgar Sec7ret9 0\r\n";
            Check(send(lc, a3, (int)strlen(a3), 0) == (int)strlen(a3), "login: launcher case — caller gets its own length");
            const std::string wantA = "apgar " + fa.reply + " 0\r\n";
            const std::string wireA = wireLine(wantA);
            WaitForSingleObject(th, 5000); CloseHandle(th);
            closesocket(als);
            Check(wireA == wantA, "login: apgar field replaced by the login server's token on the lobby wire");
            Check(wireA.find("Sec7ret9") == std::string::npos && fa.request.find("Sec7ret9") == std::string::npos,
                  "login: typed password never on the lobby wire nor to the login server");
            Check(fa.request.find("POST /token") == 0 && fa.request.find("name=selftst&key=" + key) != std::string::npos,
                  "login: login server got POST /token with NICK name and the saved launcher key");

            // ③ 못 닿음(옛 꼴 주소 …/login 도 받음 · 안 닫히는 포트 1)
            SetEnvironmentVariableA("NOX_AUTH_URL", "http://127.0.0.1:1/login");
            const char a4[] = "apgar Other999 0\r\n";
            Check(send(lc, a4, (int)strlen(a4), 0) == (int)strlen(a4), "login: unreachable case — caller gets its own length");
            const std::string wantU = "apgar NXL!unreachable 0\r\n";
            Check(wireLine(wantU) == wantU, "login: login server unreachable -> apgar field = NXL!unreachable");

            // ④ 런처로 켰는데 그 이름의 열쇠 파일이 없음
            Check(launchkey::Remove("selftst"), "login: launcher key removed");
            const char a5[] = "apgar x 0\r\n";
            send(lc, a5, (int)strlen(a5), 0);
            const std::string wantK = "apgar NXL!key_invalid 0\r\n";
            Check(wireLine(wantK) == wantK, "login: launcher name but no key file -> apgar field = NXL!key_invalid");
            SetEnvironmentVariableA("NOX_AUTH_URL", NULL);
            SetEnvironmentVariableA("NOX_LAUNCH", NULL);
            const std::string keysDir = appData + "\\NoxOnline\\keys";
            RemoveDirectoryA(keysDir.c_str());
            RemoveDirectoryA((appData + "\\NoxOnline").c_str());
            RemoveDirectoryA(appData.c_str());
        }
        // 표시 줄 전의 목록 — 우리 로비인지 아직 모르니 스위치가 켜져 있어도 끼우지 않는다(XWIS 에선 이 상태가 계속된다)
        const std::string before = ": 321 u:\r\n: 323 u:\r\n";
        send(lsv, before.data(), (int)before.size(), 0);
        std::string bgot;
        while (bgot.find(": 323 u:\r\n") == std::string::npos) {
            char b[64]; int r = recv(lc, b, sizeof(b), 0);
            if (r <= 0) break;
            bgot.append(b, r);
        }
        Check(bgot == before, "myroom: no line before our-lobby mark (NOXSERVER)");
        const std::string list = "NOXSERVER 1\r\nNOXCHAT 127.0.0.1:" + std::to_string(deadChat1) + "\r\n: 321 u:\r\n: 323 u:\r\n";   // ★ 11-06 귓속말 — NOXCHAT 도 떼어야 함
        send(lsv, list.data(), (int)list.size(), 0);
        std::string lgot;
        while (lgot.find(": 323 u:\r\n") == std::string::npos) {
            char b[64]; int r = recv(lc, b, sizeof(b), 0);
            if (r <= 0) break;
            lgot.append(b, r);
        }
        const char* sw = getenv("NOX_MYROOM_LINE");
        const bool myRoomOn = sw && sw[0] == '1';
        const std::string head = ": 326 u #selftst's_game 1 0 37 1 41681088 2130706433 128::G1P3";
        Check(lgot.find("NOXSERVER") == std::string::npos, "lobby: NOXSERVER line removed");
        Check(lgot.find("NOXCHAT") == std::string::npos && lgot.find(": 321 u:\r\n") != std::string::npos, "lobby: NOXCHAT line removed (the next line still arrives)");
        if (myRoomOn)
            // 321 줄 10 · 326 머리 · 방 정보 머리 남은 3 + 포장 78 · \r\n · 323 줄 10
            Check(lgot.compare(0, 10, ": 321 u:\r\n") == 0 && lgot.compare(10, head.size(), head) == 0 &&
                      lgot.size() == 10 + head.size() + 3 + 78 + 2 + 10 && lgot.compare(lgot.size() - 10, 10, ": 323 u:\r\n") == 0,
                  "myroom: own 326 line (nick from NICK · lobby peer ip) inserted before 323");
        else
            Check(lgot == ": 321 u:\r\n: 323 u:\r\n", "myroom off: list passes unchanged");

        // ★ 랭크 매칭 조각 3 — ⑪ 내 방 참가 줄 앞 NOXRANK JOIN · 기다림 창이 닫혀 있으면(이 exe 엔 알림창 칸이 없어 늘 닫힘) 다음 보내기 앞 NOXRANK LEAVE ·
        //   토큰이 오면 기다림 끝(LEAVE 없음). 스위치 NOX_TEST_RANKMARK=1 일 때만 표시 · 꺼져 있으면 셋 다 그대로 나가야 한다
        {
            const char* rsw = getenv("NOX_TEST_RANKMARK");
            const bool rankOn = rsw && rsw[0] == '1';
            auto sendAndRead = [&](const std::string& out, const std::string& tail) {
                send(lc, out.data(), (int)out.size(), 0);
                std::string w;
                while (w.size() < tail.size() || w.compare(w.size() - tail.size(), tail.size(), tail) != 0) {
                    char b[64]; int r = recv(lsv, b, sizeof(b), 0);
                    if (r <= 0) break;
                    w.append(b, r);
                }
                return w;
            };
            const std::string joinOwn = "JOINGAME #selftst's_game 1 \r\n", joinOther = "JOINGAME #other's_game 1 \r\n", listLine = "LIST 37 37\r\n";
            // ① 남의 방 참가 — 표시 없음 · 기다림 안 켬(다음 LIST 에도 LEAVE 없음)
            std::string w = sendAndRead(joinOther, joinOther);
            Check(w == joinOther, "rank: join to another room passes unchanged");
            w = sendAndRead(listLine, listLine);
            Check(w == listLine, "rank: LIST after another room join passes unchanged");
            // ② 내 방 참가 → 토큰 답 → LIST — 표시 하나 · LEAVE 없음
            w = sendAndRead(joinOwn, joinOwn);
            Check(w == (rankOn ? "NOXRANK JOIN\r\n" + joinOwn : joinOwn), rankOn ? "rank: NOXRANK JOIN goes out right before own JOINGAME" : "rank off: own JOINGAME passes unchanged");
            const std::string tokLine = std::string("NOXTOKEN 127.0.0.1:") + std::to_string(gamePort) + " " + hex + "\r\n: 366 u #selftst's_game :\r\n";
            send(lsv, tokLine.data(), (int)tokLine.size(), 0);
            std::string tgot;
            while (tgot.find(": 366 u") == std::string::npos) {
                char b[64]; int r = recv(lc, b, sizeof(b), 0);
                if (r <= 0) break;
                tgot.append(b, r);
            }
            Check(tgot == ": 366 u #selftst's_game :\r\n", "rank: NOXTOKEN for the rank join removed");
            w = sendAndRead(listLine, listLine);
            Check(w == listLine, "rank: LIST after the token passes unchanged (matched — no LEAVE)");
            // ③ 내 방 참가 → (창 닫힘) LIST → LIST — 첫 LIST 앞에만 LEAVE
            w = sendAndRead(joinOwn, joinOwn);
            Check(w == (rankOn ? "NOXRANK JOIN\r\n" + joinOwn : joinOwn), rankOn ? "rank: second own JOINGAME marked again" : "rank off: second own JOINGAME passes unchanged");
            w = sendAndRead(listLine, listLine);
            Check(w == (rankOn ? "NOXRANK LEAVE\r\n" + listLine : listLine), rankOn ? "rank: NOXRANK LEAVE goes out before the first send after the wait window closed" : "rank off: LIST passes unchanged");
            w = sendAndRead(listLine, listLine);
            Check(w == listLine, "rank: only one LEAVE (next LIST passes unchanged)");
        }

        // 꼬리 — 게임 서버 주소로 가는 접속 시도·CONNECT 에만
        char join[100]; memset(join, 0, sizeof(join)); join[2] = 0x0e; join[4] = 'J';
        const unsigned char connPkt[3] = { 0xff, 0x00, 0x00 };
        sockaddr_in toGame = Loopback(gamePort), toOther = Loopback(otherPort);
        Check(sendto(cli, join, sizeof(join), 0, (sockaddr*)&toGame, sizeof(toGame)) == (int)sizeof(join), "tail: sendto returns caller length 100");
        char gb[512]; int n1 = recvfrom(game, gb, sizeof(gb), 0, NULL, NULL);
        Check(n1 == 157 && memcmp(gb, join, 100) == 0 && memcmp(gb + 100, raw, 53) == 0 && memcmp(gb + 153, "NXT1", 4) == 0,
              "tail: join to game server = 100 + token 53 + NXT1");
        sendto(cli, (const char*)connPkt, 3, 0, (sockaddr*)&toGame, sizeof(toGame));
        int n2 = recvfrom(game, gb, sizeof(gb), 0, NULL, NULL);
        Check(n2 == 60 && memcmp(gb, connPkt, 3) == 0 && memcmp(gb + 3, raw, 53) == 0, "tail: CONNECT to game server = 3 + tail 57");
        sendto(cli, join, sizeof(join), 0, (sockaddr*)&toOther, sizeof(toOther));
        int n3 = recvfrom(other, gb, sizeof(gb), 0, NULL, NULL);
        Check(n3 == 100, "tail: join to another server = no tail");
        char ping[12] = { 0, 0, 0x0c };
        sendto(cli, ping, sizeof(ping), 0, (sockaddr*)&toGame, sizeof(toGame));
        int n4 = recvfrom(game, gb, sizeof(gb), 0, NULL, NULL);
        Check(n4 == 12, "tail: other packet to game server = unchanged");

        // 로비 연결이 아닌 TCP — NOXTOKEN 처럼 보이는 줄도 그대로 온다
        SOCKET pc = socket(AF_INET, SOCK_STREAM, 0);
        Check(connect(pc, (sockaddr*)&toLobby, sizeof(toLobby)) == 0, "plain: connect");
        SOCKET psv = accept(lsl, NULL, NULL);
        RecvTimeout(pc);
        const std::string fake = std::string("NOXTOKEN 1.2.3.4:5 ") + hex + "\r\n";
        send(psv, fake.data(), (int)fake.size(), 0);
        std::string pgot;
        while (pgot.size() < fake.size()) {
            char b[256]; int r = recv(pc, b, sizeof(b), 0);
            if (r <= 0) break;
            pgot.append(b, r);
        }
        Check(pgot == fake, "plain: non-lobby socket passes NOXTOKEN-looking line through");

        // ★ 로그인 암호 조각 5 의 3 — 로비 소켓의 진짜 보내기 오류는 게임에 오류로 간다 · 닫힌 로비 소켓 번호를 다시 받은 소켓은 로비가 아니다(바꿔쓰기 상태가 남으면 안 됨)
        {
            SOCKET l2 = socket(AF_INET, SOCK_STREAM, 0);
            Check(connect(l2, (sockaddr*)&toLobby, sizeof(toLobby)) == 0, "login2: connect");
            SOCKET s2 = accept(lsl, NULL, NULL);
            send(l2, cvers, (int)strlen(cvers), 0);   // 로비 연결로 적힘
            shutdown(l2, SD_SEND);
            const char nk[] = "NICK errcase\r\n";
            const int er = send(l2, nk, (int)strlen(nk), 0);
            const int ee = WSAGetLastError();
            Check(er == SOCKET_ERROR && ee == WSAESHUTDOWN, "login: real send error on a lobby socket comes back to the caller as SOCKET_ERROR (WSAESHUTDOWN)");
            const SOCKET old = l2;
            closesocket(l2); closesocket(s2);
            SOCKET reuse = INVALID_SOCKET, spare[64];
            int ns = 0;
            for (int i = 0; i < 64; ++i) {
                SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
                if (t == old) { reuse = t; break; }
                spare[ns++] = t;
            }
            for (int i = 0; i < ns; ++i) closesocket(spare[i]);
            if (reuse == INVALID_SOCKET) {
                printf("SKIP  login: closed lobby socket number not handed out again in 64 tries\n");
            } else {
                connect(reuse, (sockaddr*)&toLobby, sizeof(toLobby));
                SOCKET s3 = accept(lsl, NULL, NULL);
                RecvTimeout(s3);
                const char ap[] = "apgar Plain1 0\r\n";
                send(reuse, ap, (int)strlen(ap), 0);
                std::string w;
                while (w.size() < strlen(ap)) {
                    char b[64]; int r = recv(s3, b, sizeof(b), 0);
                    if (r <= 0) break;
                    w.append(b, r);
                }
                Check(w == ap, "login: a socket that got a closed lobby socket's number is not a lobby socket (apgar passes unchanged)");
                closesocket(reuse); closesocket(s3);
            }
        }

        // ★ 로그인 암호 조각 5 의 4 — 라우터(상대 포트 4005)로 가는 whereto 의 암호 칸은 선에 x 로(두 번에 나눠 보내도). 다른 포트로 가는 같은 줄은 위 「recv same line」이 그대로임을 본다
        {
            SOCKET rl = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in r4 = Loopback(4005);
            if (bind(rl, (sockaddr*)&r4, sizeof(r4)) != 0 || listen(rl, 1) != 0) {
                printf("SKIP  router: 127.0.0.1:4005 busy (nox_router running?)\n");
            } else {
                SOCKET rcl = socket(AF_INET, SOCK_STREAM, 0);
                Check(connect(rcl, (sockaddr*)&r4, sizeof(r4)) == 0, "router: connect to port 4005");
                SOCKET rsv = accept(rl, NULL, NULL);
                RecvTimeout(rsv);
                const char w1[] = "verchk 9472 65538\r\nwhereto rtnm pw", w2[] = "Rt55 9472 65538 7726\r\nQUIT\r\n";
                Check(send(rcl, w1, (int)strlen(w1), 0) == (int)strlen(w1) && send(rcl, w2, (int)strlen(w2), 0) == (int)strlen(w2),
                      "router: caller gets its own lengths");
                const std::string wantR = "verchk 9472 65538\r\nwhereto rtnm x 9472 65538 7726\r\nQUIT\r\n";
                std::string wireR;
                while (wireR.size() < wantR.size()) {
                    char b[64]; int r = recv(rsv, b, sizeof(b), 0);
                    if (r <= 0) break;
                    wireR.append(b, r);
                }
                Check(wireR == wantR, "router: whereto password field goes out as x (line split across two sends)");
                closesocket(rcl); closesocket(rsv);
            }
            closesocket(rl);
        }

        // ★ 11-06 귓속말 조각 4 — 둘째 연결(채팅 서버). 로비 소켓으로 NOXCHAT 이 오면 DLL 이 진짜 wsock32 로 따로 붙는다.
        //   ① 죽은 주소 — 못 붙음 · 그동안 /token 0(토큰은 붙은 뒤에) ② 산 주소 — /token(로그인 이름 · 열쇠) → AUTH 이름 토큰 · PING 에 PONG · 받은 줄은 기록에 낱말만
        //   ③ 서버가 끊으면 3~4초 뒤 새 토큰으로 다시 · AUTHOK 뒤 또 끊으면 다시 3~4초(간격이 되돌아감) ④ KICKED → 멈춤(안 붙음 · /token 0)
        //   ⑤ 새 NOXCHAT → 깨어 붙음 → 토큰 거절(401) → AUTH 없이 끊고 멈춤. 기록을 보며 기다리므로 log 판에서만(nolog 판은 둘째 스레드가 죽은 주소에만 붙어 봄)
        if (wantLog) {
            auto chatNote = [&](u_short port) {   // 로비가 NOXCHAT 을 보냄 — 다음 줄(: 999)이 게임에 오면 받기 훅이 NOXCHAT 을 처리한 뒤다
                const std::string l = "NOXCHAT 127.0.0.1:" + std::to_string(port) + "\r\n: 999 u:\r\n";
                send(lsv, l.data(), (int)l.size(), 0);
                std::string g;
                while (g.find(": 999 u:\r\n") == std::string::npos) {
                    char b[64]; int r = recv(lc, b, sizeof(b), 0);
                    if (r <= 0) break;
                    g.append(b, r);
                }
                return g == ": 999 u:\r\n";
            };
            auto readable = [](SOCKET s, DWORD ms) {   // 듣는 소켓이면 들어온 연결 · 붙은 소켓이면 받을 것(닫힘 포함)
                fd_set f; FD_ZERO(&f); FD_SET(s, &f);
                timeval tv = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
                return select(0, &f, NULL, NULL, &tv) > 0;
            };
            auto readLine = [](SOCKET s, std::string& buf) {   // 한 줄(\r\n 뗌) · 줄 없이 끊기면 "<closed>"
                for (;;) {
                    const size_t p = buf.find('\n');
                    if (p != std::string::npos) {
                        std::string l = buf.substr(0, p);
                        buf.erase(0, p + 1);
                        if (!l.empty() && l.back() == '\r') l.pop_back();
                        return l;
                    }
                    char b[256]; const int r = recv(s, b, sizeof(b), 0);
                    if (r <= 0) return std::string("<closed>");
                    buf.append(b, r);
                }
            };
            auto acceptChat = [&](SOCKET ls) {   // 8초 안에 안 오면 INVALID_SOCKET(앞 Check 가 이미 실패 · 시험이 멈추지 않게 — 11-06 돌연변이 「새 NOXCHAT 이 멈춤을 안 깸」이 accept 에서 멈춤)
                if (!readable(ls, 8000)) return INVALID_SOCKET;
                SOCKET c = accept(ls, NULL, NULL);
                int ms = 6000;   // /token 이 끼어 RecvTimeout(3초)보다 길게
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
                return c;
            };
            auto waitLog = [&](const std::string& needle, DWORD ms) {
                for (const DWORD t0 = GetTickCount(); GetTickCount() - t0 < ms; Sleep(50))
                    if (FileHas(logPath, needle.c_str())) return true;
                return FileHas(logPath, needle.c_str());
            };
            auto tokenOf = [](char c) { return "NXL1" + std::string(114, c); };

            char tmpDir[MAX_PATH];
            GetTempPathA(MAX_PATH, tmpDir);
            const std::string appData = std::string(tmpDir) + "nox_selftest_chat_" + std::to_string(GetCurrentProcessId());
            SetEnvironmentVariableA("LOCALAPPDATA", appData.c_str());
            SetEnvironmentVariableA("NOX_LAUNCH", "selftst");
            const std::string key = "NXK1" + std::string(64, 'e');
            Check(launchkey::Save("selftst", key), "chat: launcher key saved under the temp LOCALAPPDATA");
            SOCKET als = socket(AF_INET, SOCK_STREAM, 0);
            bind(als, (sockaddr*)&any, sizeof(any)); listen(als, 4);
            SetEnvironmentVariableA("NOX_AUTH_URL", ("http://127.0.0.1:" + std::to_string(PortOf(als))).c_str());   // NOXCHAT 처리 때 읽힘
            SOCKET cls = socket(AF_INET, SOCK_STREAM, 0);
            bind(cls, (sockaddr*)&any, sizeof(any)); listen(cls, 4);
            const u_short chatPort = PortOf(cls);
            std::string cb;

            // ① 죽은 주소
            const u_short deadChat2 = deadPort();
            Check(chatNote(deadChat2), "chat ①: NOXCHAT removed from the lobby stream (next line arrives)");
            Check(waitLog("귓속말 서버에 못 붙음 — 127.0.0.1:" + std::to_string(deadChat2), 6000), "chat ①: dead chat server -> connect fails (logged)");
            Check(!readable(als, 0), "chat ①: no /token while the chat server is down (token only after connecting)");

            // ② 산 주소 — 토큰 · AUTH · PING
            FakeAuth fb; fb.ls = als; fb.reply = tokenOf('b');
            HANDLE th = CreateThread(NULL, 0, FakeAuthMain, &fb, 0, NULL);
            Check(chatNote(chatPort), "chat ②: NOXCHAT (live address) removed from the lobby stream");
            Check(readable(cls, 6000), "chat ②: DLL connects to the chat server (woken from the retry wait)");
            SOCKET cs = acceptChat(cls);
            Check(readLine(cs, cb) == "AUTH selftst " + fb.reply.substr(4), "chat ②: AUTH <login name> <token hex> after /token");
            WaitForSingleObject(th, 5000); CloseHandle(th);
            Check(fb.request.find("POST /token") == 0 && fb.request.find("name=selftst&key=" + key) != std::string::npos,
                  "chat ②: /token with the login name (NICK) and the saved launcher key");
            const char ok1[] = "AUTHOK\r\nPING\r\n";
            send(cs, ok1, (int)strlen(ok1), 0);
            Check(readLine(cs, cb) == "PONG", "chat ②: server PING answered with PONG");
            const char lines[] = "F qa2 hello\r\nTO qa2 hello\r\nXYZ\r\n";
            send(cs, lines, (int)strlen(lines), 0);
            Check(waitLog("귓속말 모르는 줄 3바이트", 3000), "chat ②: unknown line logged by size only (\\r stripped -> 3 bytes)");
            Check(FileHas(logPath, "귓속말 로그인 됨 — selftst") && FileHas(logPath, "귓속말 받음 F(큐 1)") && FileHas(logPath, "귓속말 받음 TO(큐 2)"),
                  "chat ②: AUTHOK · F · TO logged (F and TO queued for the main thread)");

            // ③ 서버가 끊음 → 3~4초 뒤 새 토큰으로 · AUTHOK 뒤 또 끊으면 간격이 다시 3~4초
            char gapMsg[160];
            for (int round = 0; round < 2; ++round) {
                FakeAuth fn; fn.ls = als; fn.reply = tokenOf(round == 0 ? 'c' : 'd');
                th = CreateThread(NULL, 0, FakeAuthMain, &fn, 0, NULL);
                const DWORD t0 = GetTickCount();
                closesocket(cs);
                const bool back = readable(cls, 9000);
                const DWORD gap = GetTickCount() - t0;
                sprintf(gapMsg, "chat ③: after the server closes (round %d), DLL reconnects in 2.9~5.5 s (got %lu ms)", round + 1, gap);
                Check(back && gap >= 2900 && gap <= 5500, gapMsg);
                cs = acceptChat(cls);
                cb.clear();
                Check(readLine(cs, cb) == "AUTH selftst " + fn.reply.substr(4), round == 0 ? "chat ③: reconnect with a new token" : "chat ③: second reconnect with a new token");
                WaitForSingleObject(th, 5000); CloseHandle(th);
                if (round == 0) send(cs, "AUTHOK\r\n", 8, 0);   // 붙은 뒤 끊기면 이어진 실패가 0 으로 — 다음 간격도 3~4초
                Sleep(100);
            }

            // ④ KICKED → 멈춤. 붙어 있는 동안 온 같은 대상의 NOXCHAT(같은 계정으로 다시 로그인)은 연결이 받아들인 것 — 그 뒤 KICKED 면 그래도 멈춰야 한다
            Check(chatNote(chatPort), "chat ④: NOXCHAT for the same target while connected removed from the lobby stream");
            Sleep(200);   // 받기 고리(50ms)가 차례 번호를 읽을 틈
            Check(!readable(cs, 0) && !readable(cls, 0), "chat ④: same-target NOXCHAT keeps the connection (not closed, no new connect)");
            const char kick[] = "AUTHOK\r\nKICKED\r\n";
            send(cs, kick, (int)strlen(kick), 0);
            Check(readLine(cs, cb) == "<closed>", "chat ④: KICKED -> DLL closes");
            closesocket(cs);
            Check(!readable(cls, 4500) && !readable(als, 0), "chat ④: after KICKED no reconnect and no /token for 4.5 s (stopped)");
            Check(FileHas(logPath, "귓속말 멈춤 — KICKED"), "chat ④: stop logged");

            // ⑤ 새 NOXCHAT → 깨어 붙음 → 토큰 거절 → 멈춤
            FakeAuth fr; fr.ls = als; fr.status = 401; fr.reply = "NXL!key_invalid";
            th = CreateThread(NULL, 0, FakeAuthMain, &fr, 0, NULL);
            Check(chatNote(chatPort), "chat ⑤: NOXCHAT again (login again) removed from the lobby stream");
            Check(readable(cls, 6000), "chat ⑤: new NOXCHAT wakes the stopped connection");
            cs = acceptChat(cls);
            cb.clear();
            Check(readLine(cs, cb) == "<closed>", "chat ⑤: /token refused (401 key_invalid) -> DLL closes without sending AUTH");
            WaitForSingleObject(th, 5000); CloseHandle(th);
            closesocket(cs);
            Check(fr.request.find("POST /token") == 0, "chat ⑤: /token was asked once after connecting");
            Check(!readable(cls, 4500) && !readable(als, 0), "chat ⑤: after the token refusal no reconnect for 4.5 s (stopped)");
            Check(FileHas(logPath, "귓속말 멈춤 — 토큰 거절"), "chat ⑤: stop logged");

            // 기록 — 귓속말 줄에 글 · 토큰 · 열쇠가 없고, 둘째 연결의 보내기는 우리 훅을 안 탄다(훅을 타면 ` send s=` 줄에 AUTH · 토큰이 찍힌다)
            Check(FileHas(logPath, ("귓속말 연결 대상 — 127.0.0.1:" + std::to_string(chatPort) + " · 이름 selftst").c_str()), "chat: target line (address · login name)");
            Check(FileLineHas(logPath, "귓속말", "selftst") && !FileLineHas(logPath, "귓속말", "hello") && !FileLineHas(logPath, "귓속말", "bbbbbbbb") &&
                      !FileLineHas(logPath, "귓속말", "cccccccc") && !FileLineHas(logPath, "귓속말", "eeeeeeee"),
                  "chat: whisper log lines carry no message text, token or key");
            Check(FileLineHas(logPath, " send s=", "AUTHOK") && !FileLineHas(logPath, " send s=", "AUTH selftst") && !FileLineHas(logPath, " send s=", "PONG"),
                  "chat: second connection sends through the real wsock32 (never on a hooked send line)");

            closesocket(cls); closesocket(als);
            Check(launchkey::Remove("selftst"), "chat: launcher key removed");
            SetEnvironmentVariableA("NOX_AUTH_URL", NULL);
            SetEnvironmentVariableA("NOX_LAUNCH", NULL);
            RemoveDirectoryA((appData + "\\NoxOnline\\keys").c_str());
            RemoveDirectoryA((appData + "\\NoxOnline").c_str());
            RemoveDirectoryA(appData.c_str());
        }

        closesocket(pc); closesocket(psv); closesocket(lc); closesocket(lsv); closesocket(lsl);
        closesocket(game); closesocket(other); closesocket(cli);
    }

    closesocket(sv); closesocket(c); closesocket(ls); closesocket(ua); closesocket(ub);
    WSACleanup();

    // 로그 — 켰으면 일곱 가운데 이 시험이 부른 다섯이 있어야 하고, 껐으면 파일이 없어야 한다
    if (wantLog) {
        Check(FileHas(logPath, "wsock32"), "log: attach line");
        Check(FileHas(logPath, "sendto 0x0e"), "log: sendto 0x0e detail");
        Check(FileHas(logPath, "00 00 0e 00 4a 00 61 00 63 00 6b"), "log: 0x0e bytes");
        Check(FileHas(logPath, "connect s="), "log: connect");
        Check(FileHas(logPath, "send s="), "log: send");
        Check(FileHas(logPath, "recv s="), "log: recv");
        Check(FileHas(logPath, "whereto wn9457"), "log: send text preview");
        // ★ 로그인 암호 조각 5 — 기록에 암호가 없고, 가린 자리가 보인다(apgar 줄은 두 번에 걸쳐 갔다)
        Check(FileLineHas(logPath, " send s=", "whereto wn9457") && FileLineHas(logPath, " send s=", "apgar"), "log: the send lines under test exist (so the next check can fail)");
        Check(!FileLineHas(logPath, " send s=", "pw7Kx9") && !FileLineHas(logPath, " send s=", "Sec7") && !FileLineHas(logPath, " send s=", "ret9"),
              "log: no password text on send lines (whereto field · apgar line split in two sends)");
        Check(FileHas(logPath, "whereto wn9457 *** 9472") && FileHas(logPath, "apgar *** 0"), "log: password fields masked");
        // ★ 조각 5 의 3 — 로그인 서버를 부른 줄은 있고(상태 · 걸린 시간), 로그인 토큰 글은 기록에 없다(보낸 글은 원본이 가려져 찍힌다)
        Check(FileHas(logPath, "로그인 서버 호출") && FileHas(logPath, "NXL!unreachable"), "log: login server call lines exist (token case and unreachable case)");
        //   (이 시험은 가짜 로그인 서버도 같은 프로세스라 그 서버의 send 줄엔 토큰이 찍힌다 — 클라 쪽 줄인 「로그인 서버 호출」 줄만 본다. 보낸 글 쪽은 위 「apgar *** 0」 검사가 맡는다)
        Check(FileLineHas(logPath, "로그인 서버 호출", "NXL1") && !FileLineHas(logPath, "로그인 서버 호출", "aaaaaaaa"), "log: login server call line says a token came back but never prints its text");
        Check(FileLineHas(logPath, " send s=", "NOOP interleaved"), "log: another socket's line is shown as is while the lobby socket holds a half apgar line (per-socket state)");
        Check(FileHas(logPath, "gethostbyname \"localhost\""), "log: gethostbyname");
        Check(FileHas(logPath, "부른 곳=selftest.exe"), "log: caller module name");
        Check(FileHas(logPath, ("귓속말 서버 알림 받음 — 127.0.0.1:" + std::to_string(deadChat1) + "(1번째)").c_str()), "log: NOXCHAT address read (whisper piece 3)");
    } else {
        Check(GetFileAttributesA(logPath) == INVALID_FILE_ATTRIBUTES, "no log file when NOX_HOOK_LOG_DIR unset");
    }

    printf("%s  fail=%d\n", g_fail ? "SELFTEST FAIL" : "SELFTEST PASS", g_fail);
    return g_fail ? 1 : 0;
}
