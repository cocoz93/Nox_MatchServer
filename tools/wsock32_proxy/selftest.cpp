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
#include <string.h>

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

int main(int argc, char** argv)
{
    if (argc < 3 || (strcmp(argv[1], "log") != 0 && strcmp(argv[1], "nolog") != 0)) {
        printf("usage: selftest.exe log|nolog <logdir>\n");
        return 2;
    }
    const bool wantLog = strcmp(argv[1], "log") == 0;

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
        const char line[] = "whereto wn9457 1234 9472 65538 00000000000000000000\r\n";
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

    closesocket(sv); closesocket(c); closesocket(ls); closesocket(ua); closesocket(ub);
    WSACleanup();

    // 로그 — 켰으면 일곱 가운데 이 시험이 부른 다섯이 있어야 하고, 껐으면 파일이 없어야 한다
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\') + 1;
    char logPath[MAX_PATH]; sprintf(logPath, "%s\\wsock_%s_%lu.log", argv[2], base, GetCurrentProcessId());
    if (wantLog) {
        Check(FileHas(logPath, "wsock32"), "log: attach line");
        Check(FileHas(logPath, "sendto 0x0e"), "log: sendto 0x0e detail");
        Check(FileHas(logPath, "00 00 0e 00 4a 00 61 00 63 00 6b"), "log: 0x0e bytes");
        Check(FileHas(logPath, "connect s="), "log: connect");
        Check(FileHas(logPath, "send s="), "log: send");
        Check(FileHas(logPath, "recv s="), "log: recv");
        Check(FileHas(logPath, "whereto wn9457"), "log: send text preview");
        Check(FileHas(logPath, "gethostbyname \"localhost\""), "log: gethostbyname");
        Check(FileHas(logPath, "부른 곳=selftest.exe"), "log: caller module name");
    } else {
        Check(GetFileAttributesA(logPath) == INVALID_FILE_ATTRIBUTES, "no log file when NOX_HOOK_LOG_DIR unset");
    }

    printf("%s  fail=%d\n", g_fail ? "SELFTEST FAIL" : "SELFTEST PASS", g_fail);
    return g_fail ? 1 : 0;
}
