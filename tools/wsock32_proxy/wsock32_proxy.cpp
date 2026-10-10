// 녹스 통신 길목 — wsock32.dll 프록시 (MO 입장 토큰의 첫 시험: 통과만)
//
// 정품 클라(Game.exe)와 로비 모듈(WOL\WOLAPI.dll)은 둘 다 WSOCK32.dll 에서 함수를 **번호로** 들여온다
//   (2026-10-10 가져오기 표 실측 — Game.exe 23개 · WOLAPI.dll 30개, 전부 번호. sendto = 20번)
// WSOCK32 는 KnownDLLs 가 아니어서(그 목록엔 WS2_32 만 있다) 게임 폴더에 두면 이것이 시스템 것보다 먼저 실린다.
//
// 진짜 wsock32.dll 의 내보내기 75개를 같은 번호·같은 이름으로 내놓는다(wsock32.def).
//   여덟 개(connect · recv · recvfrom · send · sendto · closesocket · gethostbyname · WSAAsyncGetHostByName)는 이 파일이 받고,
//   나머지 67개는 진짜 함수로 바로 뛴다(Fwd_<번호>).
//   로그를 쓰는 동안 바뀐 마지막 오류 값은 되돌려 놓는다 — 게임은 connect 뒤 WSAGetLastError 로 WSAEWOULDBLOCK 을 본다.
//
// ★ 10-10 토큰 작업 4 — 입장 토큰을 싣는다(`Docs/TOKEN_DESIGN.md`). 로그와 상관없이 늘 한다.
//   ① 로비 로그인 묶음의 첫 줄(`CVERS `)이 나가기 전에 `NOXCLIENT 1` 한 줄을 먼저 보낸다 — 로비는 이 줄이 없으면 로그인을 거절한다.
//      그 소켓을 「로비 연결」로 적어 둔다
//   ② 로비 연결의 받기에서 `NOXTOKEN <게임 서버 IP:포트> <토큰 16진>` 줄을 떼어 쥔다(`LobbyLineFilter.h` — 줄이 나뉘어 와도)
//      로비 모듈(WOLAPI.dll)은 원래 답만 본다
//   ③ 쥔 지 60초 안에 그 게임 서버 주소로 가는 접속 시도(100바이트 · 셋째 `0x0e`)와 CONNECT(`ff 00 00`)에
//      꼬리(토큰 51 · `NXT1`)를 붙여 보낸다. 다른 주소(LAN 판 등)로 가는 것엔 안 붙인다 — 그 서버는 꼬리를 모른다
//   ④ closesocket — 로비 연결 상태를 지운다(소켓 번호는 다시 쓰인다)
//
// 로그: 환경변수 NOX_HOOK_LOG_DIR 이 있을 때만 <그 폴더>\wsock_<exe>_<pid>.log 에 쓴다(dinput_proxy 와 같은 스위치).
//       없으면 로그도 집계도 안 하고 진짜로 넘기기만 한다.
//       상세 줄 = 접속 시도(UDP 3번째 바이트 0x0e)의 바이트 전부 · TCP connect/send/recv · 이름 찾기
//       10초마다 = 부른 모듈별 호출 수·바이트 요약
// 빌드: build.bat (x86 필수 — Game.exe 가 32비트다) · 시험: test.bat
// 놓는 곳: 게임 폴더 · 되돌리기: 이 파일을 지우면 끝

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <map>
#include <string>

#include "LobbyLineFilter.h"

#pragma intrinsic(_ReturnAddress)

// ─────────────────────────────────────────────── 로그 (켜야 쓴다)

static char g_log[MAX_PATH] = { 0 };
static CRITICAL_SECTION g_logCs;

static void Log(const char* fmt, ...)
{
    if (!g_log[0]) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_logCs);
    FILE* f = fopen(g_log, "a");
    if (f) {
        fprintf(f, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// 상세 줄은 상한까지만 — 긴 판에서 파일이 끝없이 크지 않게. 상한 뒤로는 10초 요약만 남는다
static const LONG kMaxDetail = 5000;
static volatile LONG g_detail = 0;

static void Detail(const char* fmt, ...)
{
    if (!g_log[0]) return;
    LONG n = InterlockedIncrement(&g_detail);
    if (n > kMaxDetail) {
        if (n == kMaxDetail + 1) Log("상세 줄 상한(%ld) — 이후로는 10초 요약만", kMaxDetail);
        return;
    }
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    Log("%s", buf);
}

static void AddrStr(const struct sockaddr* a, int len, char* out)
{
    if (!a || len < (int)sizeof(sockaddr_in) || a->sa_family != AF_INET) { strcpy(out, "?"); return; }
    const sockaddr_in* in = (const sockaddr_in*)a;
    const unsigned char* b = (const unsigned char*)&in->sin_addr;
    const unsigned char* p = (const unsigned char*)&in->sin_port;
    sprintf(out, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)((p[0] << 8) | p[1]));
}

// 로비 대화는 글자 줄이다 — 찍을 수 있는 글자만 남기고 줄바꿈은 \r \n 으로
static void Preview(const char* buf, int len, char* out, int cap)
{
    int o = 0;
    for (int i = 0; i < len && o < cap - 3; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[o] = 0;
}

static void Hex(const char* buf, int len, char* out, int maxBytes)
{
    int n = len < maxBytes ? len : maxBytes;
    char* p = out;
    for (int i = 0; i < n; ++i) p += sprintf(p, i ? " %02x" : "%02x", (unsigned char)buf[i]);
    *p = 0;
}

// ─────────────────────────────────────────────── 부른 모듈별 집계

enum Fn { F_CONNECT, F_SEND, F_RECV, F_SENDTO, F_RECVFROM, F_GETHOSTBYNAME, F_ASYNCHOST, F_COUNT };
static const char* const kFnName[F_COUNT] = {
    "connect", "send", "recv", "sendto", "recvfrom", "gethostbyname", "WSAAsyncGetHostByName",
};

struct ModStat {
    HMODULE h;
    char name[64];
    unsigned long long calls[F_COUNT];
    unsigned long long bytes[F_COUNT];
};
static const int kMaxMods = 16;               // 넘치면 마지막 칸에 모은다
static ModStat g_mods[kMaxMods];
static int g_nMods = 0;
static CRITICAL_SECTION g_statCs;

// 부른 곳(돌아갈 주소)이 속한 모듈. 칸은 지우지 않으므로 돌려준 포인터는 계속 유효하다
static ModStat* ModOf(void* ret)
{
    HMODULE h = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ret, &h);
    EnterCriticalSection(&g_statCs);
    ModStat* m = NULL;
    for (int i = 0; i < g_nMods; ++i) if (g_mods[i].h == h) { m = &g_mods[i]; break; }
    if (!m && g_nMods < kMaxMods) {
        m = &g_mods[g_nMods++];
        m->h = h;
        char path[MAX_PATH] = { 0 };
        if (h && GetModuleFileNameA(h, path, MAX_PATH)) {
            const char* base = strrchr(path, '\\');
            strncpy(m->name, base ? base + 1 : path, sizeof(m->name) - 1);
        } else {
            strcpy(m->name, "(모듈 밖)");
        }
    }
    if (!m) m = &g_mods[kMaxMods - 1];
    LeaveCriticalSection(&g_statCs);
    return m;
}

static void Count(ModStat* m, Fn fn, int bytes)
{
    EnterCriticalSection(&g_statCs);
    ++m->calls[fn];
    if (bytes > 0) m->bytes[fn] += (unsigned long long)bytes;
    LeaveCriticalSection(&g_statCs);
}

static DWORD WINAPI SummaryThread(LPVOID)
{
    static ModStat last[kMaxMods];
    for (;;) {
        Sleep(10000);
        ModStat now[kMaxMods];
        int n;
        EnterCriticalSection(&g_statCs);
        n = g_nMods;
        memcpy(now, g_mods, sizeof(now));
        LeaveCriticalSection(&g_statCs);
        for (int i = 0; i < n; ++i) {
            if (memcmp(now[i].calls, last[i].calls, sizeof(now[i].calls)) == 0) continue;
            char line[1024]; int o = sprintf(line, "요약 %s |", now[i].name);
            for (int f = 0; f < F_COUNT; ++f)
                if (now[i].calls[f])
                    o += sprintf(line + o, " %s %llu회 %lluB", kFnName[f], now[i].calls[f], now[i].bytes[f]);
            Log("%s", line);
        }
        memcpy(last, now, sizeof(last));
    }
}

// ─────────────────────────────────────────────── 진짜 DLL

static const WORD kOrds[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    51, 52, 53, 54, 55, 56, 57,
    101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116,
    151, 500, 1000,
    1100, 1101, 1102, 1103, 1104, 1105, 1106, 1107, 1108, 1109, 1110, 1111, 1112, 1113, 1114, 1115,
    1116, 1117, 1118, 1119, 1120, 1130, 1140, 1141, 1142,
};
static const int kMaxOrd = 1142;

static HMODULE g_real = NULL;
static void* volatile g_fn[kMaxOrd + 1];
static volatile LONG g_ready = 0;
static CRITICAL_SECTION g_resolveCs;

// 처음 불릴 때 한 번에 다 찾는다 — DllMain 에서 LoadLibrary 를 하지 않으려고
static void ResolveAll()
{
    DWORD err = GetLastError();
    EnterCriticalSection(&g_resolveCs);
    if (!g_ready) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   // 32비트 프로세스면 SysWOW64 로 온다
        if (n && n < MAX_PATH - 16) {
            strcat(path, "\\wsock32.dll");
            g_real = LoadLibraryA(path);                 // 전체 경로라 이미 실린 이 프록시가 다시 잡히지 않는다
        }
        Log("진짜 DLL 열기 %s → %s", path, g_real ? "성공" : "실패");
        int missing = 0;
        for (int i = 0; i < (int)(sizeof(kOrds) / sizeof(kOrds[0])); ++i) {
            WORD o = kOrds[i];
            g_fn[o] = g_real ? (void*)GetProcAddress(g_real, MAKEINTRESOURCEA(o)) : NULL;
            if (!g_fn[o]) { ++missing; Log("진짜 DLL 에 %u번이 없음", (unsigned)o); }
        }
        Log("진짜 함수 %d개 찾음 · 못 찾음 %d", (int)(sizeof(kOrds) / sizeof(kOrds[0])) - missing, missing);
        InterlockedExchange(&g_ready, 1);
    }
    LeaveCriticalSection(&g_resolveCs);
    SetLastError(err);
}

extern "C" void* __stdcall Resolve(DWORD ord)
{
    if (!g_ready) ResolveAll();
    void* p = (ord <= (DWORD)kMaxOrd) ? g_fn[ord] : NULL;
    if (!p) FatalAppExitW(0, L"wsock32 프록시: 진짜 wsock32.dll 에서 함수를 못 찾았다 (로그 NOX_HOOK_LOG_DIR)");
    return p;
}

// 넘김 — 인자는 건드리지 않고 진짜 함수로 뛴다. eax·ecx·edx 는 함수 입구에서 원래 버려도 되는 레지스터다
#define FWD(n) extern "C" __declspec(naked) void Fwd_##n(void) { __asm push n __asm call Resolve __asm jmp eax }

FWD(1) FWD(2) FWD(5) FWD(6) FWD(7) FWD(8) FWD(9) FWD(10) FWD(11) FWD(12) FWD(13) FWD(14) FWD(15)
FWD(18) FWD(21) FWD(22) FWD(23) FWD(24) FWD(51) FWD(53) FWD(54) FWD(55) FWD(56) FWD(57)
FWD(101) FWD(102) FWD(104) FWD(105) FWD(106) FWD(107) FWD(108) FWD(109) FWD(110) FWD(111) FWD(112)
FWD(113) FWD(114) FWD(115) FWD(116) FWD(151) FWD(500) FWD(1000)
FWD(1100) FWD(1101) FWD(1102) FWD(1103) FWD(1104) FWD(1105) FWD(1106) FWD(1107) FWD(1108) FWD(1109)
FWD(1110) FWD(1111) FWD(1112) FWD(1113) FWD(1114) FWD(1115) FWD(1116) FWD(1117) FWD(1118) FWD(1119)
FWD(1120) FWD(1130) FWD(1140) FWD(1141) FWD(1142)

// ─────────────────────────────────────────────── 입장 토큰 (10-10 토큰 작업 4 — 머리 주석 ①~④)

static const char kNoxClientHello[] = "NOXCLIENT 1\r\n";   // DLL 판 — 로비 `kNoxClientMinVersion` 이상이어야 로그인된다
static const DWORD kTokenHoldMs = 60000;                   // 토큰을 쥐고 있는 시간(토큰 만료 30초보다 넉넉히 — 판정은 서버가 한다)
static const size_t kTokenRaw = 51;                        // `JoinToken.h` kJoinTokenSize
static CRITICAL_SECTION g_tokCs;
static std::map<SOCKET, LobbyLineFilter*> g_lobby;         // 로비 연결(첫 보내기가 `CVERS `) → 줄 떼기 상태
struct PendingToken {
    bool have = false;
    unsigned char ip[4] = {};    // 게임 서버 a.b.c.d
    unsigned short port = 0;
    unsigned char raw[kTokenRaw] = {};
    DWORD tick = 0;
};
static PendingToken g_tok;

static bool MarkLobby(SOCKET s)   // 새로 적었으면 true
{
    EnterCriticalSection(&g_tokCs);
    const bool added = g_lobby.find(s) == g_lobby.end();
    if (added) g_lobby[s] = new LobbyLineFilter();
    LeaveCriticalSection(&g_tokCs);
    return added;
}

static LobbyLineFilter* LobbyFilterOf(SOCKET s)
{
    EnterCriticalSection(&g_tokCs);
    auto it = g_lobby.find(s);
    LobbyLineFilter* f = it == g_lobby.end() ? NULL : it->second;
    LeaveCriticalSection(&g_tokCs);
    return f;   // 그 소켓을 받는 스레드만 쓴다 — closesocket 전까지 유효
}

static int HexNib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// `NOXTOKEN a.b.c.d:port <16진 102자>` → g_tok
static void OnTokenLine(const std::string& line, const char* who)
{
    unsigned a, b, c, d, port;
    char hex[256] = {};
    PendingToken t;
    bool ok = sscanf(line.c_str(), "NOXTOKEN %u.%u.%u.%u:%u %255s", &a, &b, &c, &d, &port, hex) == 6 &&
              a < 256 && b < 256 && c < 256 && d < 256 && port > 0 && port < 65536 && strlen(hex) == kTokenRaw * 2;
    for (size_t i = 0; ok && i < kTokenRaw; ++i) {
        const int hi = HexNib(hex[2 * i]), lo = HexNib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) ok = false;
        else t.raw[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    if (!ok) { Detail("NOXTOKEN 줄 꼴이 틀려 버림 — %.80s 부른 곳=%s", line.c_str(), who); return; }
    t.have = true;
    t.ip[0] = (unsigned char)a; t.ip[1] = (unsigned char)b; t.ip[2] = (unsigned char)c; t.ip[3] = (unsigned char)d;
    t.port = (unsigned short)port;
    t.tick = GetTickCount();
    EnterCriticalSection(&g_tokCs);
    g_tok = t;
    LeaveCriticalSection(&g_tokCs);
    Detail("NOXTOKEN 받음 — 게임 서버 %u.%u.%u.%u:%u 로 가는 접속 시도·CONNECT 에 붙인다 부른 곳=%s", a, b, c, d, port, who);
}

// 접속 시도(100바이트 · 셋째 0x0e) · CONNECT(ff 00 00) 이고, 쥔 토큰의 게임 서버로 가면 꼬리를 채운다
static bool TailFor(const char* buf, int len, const struct sockaddr* to, int tolen, unsigned char tail[kTokenRaw + 4], const char*& why)
{
    const unsigned char* p = (const unsigned char*)buf;
    const bool join = len == 100 && p[2] == 0x0e;
    const bool conn = len == 3 && p[0] == 0xff && p[1] == 0x00 && p[2] == 0x00;
    if (!join && !conn) { why = NULL; return false; }   // 다른 패킷 — 말할 것 없음
    if (!to || tolen < (int)sizeof(sockaddr_in) || to->sa_family != AF_INET) { why = "주소 꼴"; return false; }
    const sockaddr_in* in = (const sockaddr_in*)to;
    const unsigned char* ip = (const unsigned char*)&in->sin_addr;
    const unsigned short port = (unsigned short)((((const unsigned char*)&in->sin_port)[0] << 8) | ((const unsigned char*)&in->sin_port)[1]);
    bool ok = false;
    EnterCriticalSection(&g_tokCs);
    if (!g_tok.have) why = "쥔 토큰 없음";
    else if (GetTickCount() - g_tok.tick > kTokenHoldMs) why = "쥔 지 60초 넘음";
    else if (memcmp(ip, g_tok.ip, 4) != 0 || port != g_tok.port) why = "토큰의 게임 서버가 아닌 주소";
    else {
        memcpy(tail, g_tok.raw, kTokenRaw);
        memcpy(tail + kTokenRaw, "NXT1", 4);
        ok = true;
        why = join ? "접속 시도에 꼬리" : "CONNECT 에 꼬리";
    }
    LeaveCriticalSection(&g_tokCs);
    return ok;
}

// ─────────────────────────────────────────────── 받는 여덟

typedef int (WSAAPI* PFN_connect)(SOCKET, const struct sockaddr*, int);
typedef int (WSAAPI* PFN_closesocket)(SOCKET);
typedef int (WSAAPI* PFN_send)(SOCKET, const char*, int, int);
typedef int (WSAAPI* PFN_recv)(SOCKET, char*, int, int);
typedef int (WSAAPI* PFN_sendto)(SOCKET, const char*, int, int, const struct sockaddr*, int);
typedef int (WSAAPI* PFN_recvfrom)(SOCKET, char*, int, int, struct sockaddr*, int*);
typedef struct hostent* (WSAAPI* PFN_gethostbyname)(const char*);
typedef HANDLE (WSAAPI* PFN_WSAAsyncGetHostByName)(HWND, u_int, const char*, char*, int);

extern "C" int WSAAPI Hook_connect(SOCKET s, const struct sockaddr* name, int namelen)
{
    void* ret = _ReturnAddress();
    int rc = ((PFN_connect)Resolve(4))(s, name, namelen);
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_CONNECT, 0);
        char a[32]; AddrStr(name, namelen, a);
        Detail("connect s=%u %s rc=%d err=%lu 부른 곳=%s", (unsigned)s, a, rc, rc ? err : 0UL, m->name);
        SetLastError(err);
    }
    return rc;
}

extern "C" int WSAAPI Hook_send(SOCKET s, const char* buf, int len, int flags)
{
    void* ret = _ReturnAddress();
    // ① 로비 로그인 묶음의 첫 줄(CVERS)이 나가기 전에 DLL 판을 알린다. 이 소켓을 로비 연결로 적는다
    if (buf && len >= 6 && memcmp(buf, "CVERS ", 6) == 0 && MarkLobby(s)) {
        const int hr = ((PFN_send)Resolve(19))(s, kNoxClientHello, (int)strlen(kNoxClientHello), flags);
        if (g_log[0]) {
            DWORD err = GetLastError();
            Detail("NOXCLIENT 알림 s=%u rc=%d(%d바이트) 부른 곳=%s", (unsigned)s, hr, (int)strlen(kNoxClientHello), ModOf(ret)->name);
            SetLastError(err);
        }
    }
    int rc = ((PFN_send)Resolve(19))(s, buf, len, flags);
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_SEND, rc);
        char p[512]; Preview(buf, len, p, 400);
        Detail("send s=%u len=%d rc=%d 부른 곳=%s | %s", (unsigned)s, len, rc, m->name, p);
        SetLastError(err);
    }
    return rc;
}

extern "C" int WSAAPI Hook_recv(SOCKET s, char* buf, int len, int flags)
{
    void* ret = _ReturnAddress();
    // ② 로비 연결이면 NOXTOKEN 줄을 떼고 넘긴다(엿보기 `MSG_PEEK` 는 줄 떼기와 맞지 않아 그대로 — 로비 모듈은 안 쓴다)
    LobbyLineFilter* f = (flags & MSG_PEEK) ? NULL : LobbyFilterOf(s);
    int rc;
    if (f) {
        const char* who = g_log[0] ? ModOf(ret)->name : "";
        rc = f->Recv([&](char* b, int n) { return ((PFN_recv)Resolve(16))(s, b, n, flags); }, buf, len,
                     [&](const std::string& line) { OnTokenLine(line, who); });
    } else {
        rc = ((PFN_recv)Resolve(16))(s, buf, len, flags);
    }
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_RECV, rc);
        if (rc > 0) {
            char p[512]; Preview(buf, rc, p, 400);
            Detail("recv s=%u rc=%d 부른 곳=%s | %s", (unsigned)s, rc, m->name, p);
        }
        SetLastError(err);
    }
    return rc;
}

extern "C" int WSAAPI Hook_sendto(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen)
{
    void* ret = _ReturnAddress();
    // ③ 쥔 토큰의 게임 서버로 가는 접속 시도 · CONNECT 에 꼬리. 부른 쪽에는 원래 길이를 돌려준다(UDP 는 통째로 가거나 실패)
    unsigned char tail[kTokenRaw + 4];
    const char* why = NULL;
    int rc;
    bool tailed = false;
    if (buf && len > 0 && TailFor(buf, len, to, tolen, tail, why)) {
        char big[512];
        memcpy(big, buf, (size_t)len);
        memcpy(big + len, tail, sizeof(tail));
        rc = ((PFN_sendto)Resolve(20))(s, big, len + (int)sizeof(tail), flags, to, tolen);
        if (rc == len + (int)sizeof(tail)) rc = len;
        tailed = true;
    } else {
        rc = ((PFN_sendto)Resolve(20))(s, buf, len, flags, to, tolen);
    }
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_SENDTO, rc);
        if (buf && len >= 3 && (unsigned char)buf[2] == 0x0e) {   // 접속 시도 — 머리 3바이트 뒤 본문 97바이트
            char a[32]; AddrStr(to, tolen, a);
            char h[400]; Hex(buf, len, h, 128);
            Detail("sendto 0x0e(접속 시도) %s len=%d rc=%d 부른 곳=%s | %s", a, len, rc, m->name, h);
        }
        if (why) {
            char a[32]; AddrStr(to, tolen, a);
            Detail("토큰 꼬리 %s — %s len=%d rc=%d", tailed ? "붙임" : "안 붙임", a, len, rc);
            if (!tailed) Detail("  까닭: %s", why);
            else Detail("  %s", why);
        }
        SetLastError(err);
    }
    return rc;
}

// ④ 로비 연결 상태를 지운다 — 소켓 번호는 닫힌 뒤 다른 소켓에 다시 쓰인다
extern "C" int WSAAPI Hook_closesocket(SOCKET s)
{
    LobbyLineFilter* f = NULL;
    EnterCriticalSection(&g_tokCs);
    auto it = g_lobby.find(s);
    if (it != g_lobby.end()) { f = it->second; g_lobby.erase(it); }
    LeaveCriticalSection(&g_tokCs);
    delete f;
    return ((PFN_closesocket)Resolve(3))(s);
}

extern "C" int WSAAPI Hook_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen)
{
    void* ret = _ReturnAddress();
    int rc = ((PFN_recvfrom)Resolve(17))(s, buf, len, flags, from, fromlen);
    if (g_log[0]) {
        DWORD err = GetLastError();
        Count(ModOf(ret), F_RECVFROM, rc);
        SetLastError(err);
    }
    return rc;
}

extern "C" struct hostent* WSAAPI Hook_gethostbyname(const char* name)
{
    void* ret = _ReturnAddress();
    struct hostent* h = ((PFN_gethostbyname)Resolve(52))(name);
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_GETHOSTBYNAME, 0);
        char a[32] = "실패";
        if (h && h->h_addrtype == AF_INET && h->h_addr_list && h->h_addr_list[0]) {
            const unsigned char* b = (const unsigned char*)h->h_addr_list[0];
            sprintf(a, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
        Detail("gethostbyname \"%s\" → %s 부른 곳=%s", name ? name : "(null)", a, m->name);
        SetLastError(err);
    }
    return h;
}

extern "C" HANDLE WSAAPI Hook_WSAAsyncGetHostByName(HWND hWnd, u_int wMsg, const char* name, char* buf, int buflen)
{
    void* ret = _ReturnAddress();
    HANDLE r = ((PFN_WSAAsyncGetHostByName)Resolve(103))(hWnd, wMsg, name, buf, buflen);
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_ASYNCHOST, 0);
        Detail("WSAAsyncGetHostByName \"%s\" 요청=%p 부른 곳=%s (답은 창 메시지로 간다 — 여기선 못 봄)",
               name ? name : "(null)", r, m->name);
        SetLastError(err);
    }
    return r;
}

// ─────────────────────────────────────────────── 들고 나기

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_statCs);
        InitializeCriticalSection(&g_resolveCs);
        InitializeCriticalSection(&g_tokCs);

        char dir[MAX_PATH];
        if (GetEnvironmentVariableA("NOX_HOOK_LOG_DIR", dir, MAX_PATH)) {
            char exe[MAX_PATH]; exe[0] = 0;
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
            _snprintf(g_log, MAX_PATH - 1, "%s\\wsock_%s_%lu.log", dir, base, GetCurrentProcessId());
            char self[MAX_PATH]; self[0] = 0;
            GetModuleFileNameA(h, self, MAX_PATH);
            Log("=== wsock32 프록시(통과만) 붙음 ===  exe=%s  나=%s", exe, self);
            CloseHandle(CreateThread(NULL, 0, SummaryThread, NULL, 0, NULL));
        }
    }
    return TRUE;
}
