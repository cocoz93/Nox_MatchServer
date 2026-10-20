// 녹스 통신 길목 — wsock32.dll 프록시 (MO 입장 토큰의 첫 시험: 통과만)
//
// ── 왜 이 DLL 이 있나 — 정품 클라의 한계와 우회 ─────────────────────────────────────────────
// 전제 = 정품 실행 파일(Game.exe · WOL\WOLAPI.dll)은 고치지 않는다. 게임 폴더에 이 DLL 하나만 둔다(cnc-ddraw 의 ddraw.dll 과 같은 방식).
//   프로토콜의 정답지는 계속 정품이다. 이 DLL 은 통신 길목에서 바이트를 더하고 떼거나, 정품 클라 메모리 몇 칸을 바꿀 뿐이다.
//
//   A. 정품을 고칠 수 없다
//      → 통신 길목 WSOCK32.dll 을 대신한다 — KnownDLLs 가 아니라 게임 폴더 것이 먼저 실리고, 정품은 함수를 번호로 들여오므로
//        내보내기 75개를 같은 번호로 낸다(아래 「진짜 wsock32.dll 의 내보내기」)
//   B. 게임 서버 접속 시도(0x0e)에 방 칸이 없다 — 서버는 들어온 사람이 몇 번 방으로 가려는지 모른다.
//      실린 신원도 고정값뿐이다(캐릭터 · 로비 이름 · PC 일련번호 — 한 번 새면 계속 사칭에 쓰인다)
//      → 로비가 방 참가마다 서명한 일회용 토큰을 주고(`NOXTOKEN` 줄), 이 DLL 이 접속 시도 · CONNECT 끝에 꼬리로 붙인다(아래 ① ~ ③).
//        게임 서버가 같은 키로 혼자 검증해 그 방에 바로 넣는다. 로비가 게임 서버에 미리 알리는 방식은 안 쓴다 —
//        클라가 방 참가 답 1ms 뒤에 접속 시도를 보내서(10-10 로그) 그 사이 경쟁이 생긴다
//   C. 정품 로비 모듈은 Westwood Online 원래 줄만 안다
//      → 우리가 더한 줄(`NOXTOKEN` · `NOXSERVER`)은 받기에서 떼어 내 로비 모듈엔 원래 답만 간다(`LobbyLineFilter.h`)
//   D. 로비는 이 DLL 이 있는 클라인지 알 수 없다
//      → 로그인 묶음 앞에 `NOXCLIENT <판>` 한 줄(① — 없으면 로비가 로그인은 받고 인사말에 패치 안내 · 토큰은 안 줌)
//   E. 정품 방 만들기 버튼 `Host Game` 은 방장 PC 에 서버를 켠다. 방 채널 만들기 요청은 그 서버가 뜬 뒤에 나가서
//      로비 답으로 막을 수 없다(10-13 녹음 · 정적 분석)
//      → 캐릭터 창이 뜨면 클라 메모리 다섯 칸을 바꿔 「만들기」를 「목록의 방에 들어가기」로 돌리고, 그 방 항목 · WOL 채널을 지어 넣는다(⑥).
//        로비는 제 이름 채널(`#<닉>'s_game`)로 온 참가에 우리 서버 새 방을 만든다
//   F. 정품은 받은 방 목록에 있는 채널에만 참가를 보내고, 제 이름 방은 화면 목록에 안 그린다(10-15 · 10-16)
//      → 목록 끝에 「내 방」 줄을 끼우고(⑤), 목록 항목은 베끼지 않고 짓는다(⑥ · `HostGameEntry.h`)
//   G. 메모리 칸 주소는 Game.exe 판마다 다르다
//      → 판 확인(PE 시각 · 이미지 크기 · 실린 자리)이 맞을 때만 E · F 를 켠다
//   H. 이 DLL 은 다른 로비(XWIS 등)에 붙을 때도 실린다
//      → 로비가 `NOXSERVER 1` 로 답한 「우리 로비」에서만 E · F 를 켠다 — 아니면 원작 그대로
//
//   그래서 — 정품의 화면 · 버튼 · 프로토콜은 그대로 쓰고, 정품이 못 하는 「누가 몇 번 방인지 알리기」와 「우리 서버에 방 만들기」만
//   이 DLL 이 통신 길목과 메모리 다섯 칸으로 메운다.
//   서버 쪽 짝 — 토큰 짓기 · 검증 `server/include/nox/JoinToken.h` · 로비 줄과 새 방 `server/src/lobby/Lobby.cpp` · 꼬리 떼기 `server/src/UdpServer.cpp`
// ────────────────────────────────────────────────────────────────────────────────────────
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
//   ① 로비 로그인 묶음의 첫 줄(`CVERS `)이 나가기 전에 `NOXCLIENT <판>`(10-16 부터 2) 한 줄을 먼저 보낸다 — 로비는 이 줄이 없으면 로그인을 거절한다.
//      그 소켓을 「로비 연결」로 적어 둔다
//   ② 로비 연결의 받기에서 `NOXTOKEN <게임 서버 IP:포트> <토큰 16진>` 줄을 떼어 쥔다(`LobbyLineFilter.h` — 줄이 나뉘어 와도)
//      로비 모듈(WOLAPI.dll)은 원래 답만 본다
//   ③ 쥔 지 60초 안에 그 게임 서버 주소로 가는 접속 시도(100바이트 · 셋째 `0x0e`)와 CONNECT(`ff 00 00`)에
//      꼬리(토큰 51 · `NXT1`)를 붙여 보낸다. 다른 주소(LAN 판 등)로 가는 것엔 안 붙인다 — 그 서버는 꼬리를 모른다
//   ④ closesocket — 로비 연결 상태를 지운다(소켓 번호는 다시 쓰인다)
//
// ★ 10-16 플랜 B 작업 11 — 정품 `Host Game` → 우리 채널에 내 방을 만들고 들어가기(`Docs/ROOMS_DESIGN.md` 결정 4 · 작업 11)
//   켜는 조건 = 판 확인(Game.exe PE 시각 · 이미지 크기 · 실린 자리 — 아래 메모리 칸이 그 판의 것) 그리고 **우리 로비**
//   (`NOXCLIENT 2` 에 로비가 `NOXSERVER 1` 로 답함 — ② 처럼 떼어 낸다). 그 줄이 없는 로비(XWIS 등)에선 아무것도 안 돌린다 —
//   원작 Host Game 이 그대로 산다. 끄기: 환경변수 NOX_NO_HOSTGAME=1
//   ⑤ 로비가 보내는 방 목록의 끝 줄 바로 앞에 「내 방」 줄(`326` · 채널 `#<내 닉>'s_game`)을 끼운다. 닉은 로그인 묶음의 `NICK` 줄에서 본다.
//      정품 클라는 받은 목록에 있는 채널에만 방 참가를 보낸다(10-15 대조) — 이 줄이 있어야 그 채널로 참가가 나가고,
//      채널은 없는 방이면 그 자리에서 만든다(`Lobby.cpp` JOINGAME 「새 방」). 그 채널이 이미 목록에 있으면 안 끼운다.
//      제 이름 방은 정품이 제 화면 목록에 안 그린다(10-16) — 그래서 ⑥ 은 항목을 베끼지 않고 짓는다
//   ⑥ Host Game 을 누르고 캐릭터 창이 뜨면 「내 방」 목록 항목 · WOL 채널을 지어(`HostGameEntry.h`) 고른 항목으로 넣고
//      「만들기」를 「들어가기」로 바꾼다 — 다섯 칸(10-13~16 외부 도구 memhost3~5 가 정품에서 쓴 칸). 50ms 마다 보는 스레드 하나
//   ⑦ 시험용 — 환경변수 NOX_MYROOM_LINE=1 이면 판 확인 없이 ⑤ 만 켠다(selftest.exe 가 줄 끼우기를 보려고 · ⑥ 은 판 확인 없이 안 돈다)
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
#include <vector>

#include "HostGameEntry.h"     // ⑤ · ⑥ 내 방 정보 · 항목 · 채널 짓기(방 정보 포장은 서버와 같은 WolGameInfo — build.bat 이 같이 빌드)
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

// DLL 판 — 로비 `kNoxClientMinVersion` 이상이어야 로그인된다 · ★ 10-16 판 2 = `NOXSERVER` 떼기 · ⑤ · ⑥(`kNoxClientHostGameVersion`)
static const char kNoxClientHello[] = "NOXCLIENT 2\r\n";
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
static bool g_listOn = false;           // ⑤ 줄 끼우기를 걸어 두나(판 확인 통과 또는 ⑦ 시험 스위치 · 끄기 스위치 없음)
static bool g_hostGameOn = false;       // ⑥ Host Game 돌리기 스레드(판 확인 통과 · 끄기 스위치 없음)
static volatile LONG g_ourLobby = 0;    // ⑤ · ⑥ 지금 로비 연결이 `NOXSERVER` 를 보냈나 — 새 로비 연결마다 0
static volatile LONG g_lobbyIp = 0;     // 그 로비 연결의 상대 IPv4(a<<24|b<<16|c<<8|d) — 채널이 곧 게임 서버
static std::string g_nick;              // ⑤ · ⑥ 로비 로그인의 `NICK <닉>` — g_tokCs 로 지킨다

static std::string MyRoomLine(const std::vector<std::string>& items);

static bool MarkLobby(SOCKET s)   // 새로 적었으면 true
{
    EnterCriticalSection(&g_tokCs);
    const bool added = g_lobby.find(s) == g_lobby.end();
    if (added) {
        LobbyLineFilter* f = new LobbyLineFilter();
        if (g_listOn) f->onListEnd = [](const std::vector<std::string>& items) { return MyRoomLine(items); };
        g_lobby[s] = f;
        InterlockedExchange(&g_ourLobby, 0);   // 새 로비 연결 — 표시 줄을 다시 받아야 켠다
    }
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

// ② 떼어 낸 줄 — 토큰이면 쥐고, 우리 로비 표시(`NOXSERVER`)면 그 연결의 상대 주소를 적고 ⑤ · ⑥ 을 켠다
static void OnControlLine(SOCKET s, const std::string& line, const char* who)
{
    if (line.compare(0, strlen(LobbyLineFilter::kServerMark), LobbyLineFilter::kServerMark) != 0) { OnTokenLine(line, who); return; }
    typedef int (WSAAPI* PFN_getpeername)(SOCKET, struct sockaddr*, int*);
    const DWORD err = GetLastError();
    sockaddr_in peer = {};
    int plen = (int)sizeof(peer);
    if (((PFN_getpeername)Resolve(5))(s, (struct sockaddr*)&peer, &plen) == 0 && peer.sin_family == AF_INET) {
        const unsigned char* b = (const unsigned char*)&peer.sin_addr;
        InterlockedExchange(&g_lobbyIp, (LONG)(((DWORD)b[0] << 24) | ((DWORD)b[1] << 16) | ((DWORD)b[2] << 8) | b[3]));
        InterlockedExchange(&g_ourLobby, 1);
        Log("우리 로비 표시 받음(%s) — 로비 %u.%u.%u.%u · 내 방 줄 %s · Host Game 돌리기 %s", line.c_str(), b[0], b[1], b[2], b[3],
            g_listOn ? "켬" : "끔", g_hostGameOn ? "켬" : "끔");
    } else {
        Log("우리 로비 표시 받음(%s) — 로비 연결의 상대 주소를 못 읽어 켜지 않음 err=%lu", line.c_str(), GetLastError());
    }
    SetLastError(err);
}

// ⑤ 보내는 로비 글에서 `NICK <닉>` 줄을 찾아 둔다 — 로그인 묶음은 여러 줄이 한 번에 나간다
static void NoteNick(const char* buf, int len)
{
    for (int i = 0; i + 5 <= len; ++i) {
        if ((i > 0 && buf[i - 1] != '\n') || memcmp(buf + i, "NICK ", 5) != 0) continue;
        int e = i + 5;
        while (e < len && buf[e] != '\r' && buf[e] != '\n' && buf[e] != ' ') ++e;
        if (e == i + 5) continue;
        const std::string nick(buf + i + 5, (size_t)(e - (i + 5)));
        EnterCriticalSection(&g_tokCs);
        g_nick = nick;
        LeaveCriticalSection(&g_tokCs);
        Log("로비 닉 = %s — 내 방 줄의 채널 이름", nick.c_str());
    }
}

static std::string NickNow()
{
    EnterCriticalSection(&g_tokCs);
    const std::string nick = g_nick;
    LeaveCriticalSection(&g_tokCs);
    return nick;
}

static std::string Dotted(DWORD ip)
{
    char a[16];
    sprintf(a, "%lu.%lu.%lu.%lu", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return a;
}

// ⑤ 방 목록 끝 바로 앞에 끼울 `326` 줄. 칸 꼴은 `Lobby::SendList` 와 같다 — 방장 주소 칸 = 이 로비 연결의 상대(채널이 곧 게임 서버)
//   방 정보 = `hostgame::MyRoomInfo`(⑥ 의 항목과 같은 값). 우리 로비가 아니거나 · 닉을 아직 못 봤거나 · 그 채널이 이미 목록에 있으면 빈 글자(안 끼움)
//   목록은 창이 열려 있는 동안 몇 초마다 다시 오므로 기록은 상세 줄(상한 있음)로만
static std::string MyRoomLine(const std::vector<std::string>& items)
{
    if (!g_ourLobby) return std::string();
    const std::string nick = NickNow();
    const std::string channel = "#" + nick + "'s_game";
    bool listed = false;
    for (const std::string& l : items) listed = listed || l.find(" " + channel + " ") != std::string::npos;
    if (nick.empty()) { Detail("내 방 줄 — 안 끼움(로비 닉을 아직 못 봄)"); return std::string(); }
    if (listed) { Detail("내 방 줄 — 안 끼움(%s 가 이미 목록에 있음 · 목록 %u줄)", channel.c_str(), (unsigned)items.size()); return std::string(); }
    const DWORD ip = (DWORD)g_lobbyIp;
    Detail("내 방 줄 끼움 — %s · 방장 주소 %s · 그 목록의 방 %u줄", channel.c_str(), Dotted(ip).c_str(), (unsigned)items.size());
    return ": 326 u " + channel + " 1 0 " + std::to_string(hostgame::kChannelType) + " 1 " + std::to_string(hostgame::kLocation) + " " +
           std::to_string((unsigned long)ip) + " " + std::string(nox::wol::kListPrefix) + nox::wol::Pack(hostgame::MyRoomInfo(nick)) + "\r\n";
}

// ─────────────────────────────────────────────── ⑥ Host Game → 내 방 들어가기 (10-16 작업 11)

// 이 판 Game.exe(GOG — 10-16 Game.exe · Game2.exe 둘 다 이 값)의 메모리 칸. 10-13 디스어셈블로 찾고 memhost3~5 가 정품에서 써서 확인했다
static const DWORD kGameTimeStamp = 0x3978A3FE;
static const DWORD kGameImageSize = 0x57E000;
static const DWORD kGameBase = 0x400000;
static const uintptr_t kCreateOrJoin = 0x69905C;   // 1 = Host Game(만들기) · 0 = 들어가기
static const uintptr_t kNetMode = 0x59A13C;        // 1 = 인터넷(WOL)
static const uintptr_t kSelEntry = 0x698EB4;       // 고른 목록 항목
static const uintptr_t kSelValid = 0x699064;       // 고름 유효
static const uintptr_t kJoinGate = 0x699058;       // 들어가기 문
static const uintptr_t kSelPort = 0x698EA0;        // 고른 방 포트(16비트)
static const uintptr_t kCharWin = 0x711500;        // 캐릭터 창(+4 = 창 깃발 — 0x810 이면 닫히는 중 · 0xACACACAC 는 풀린 메모리)
static const DWORD kAliveMs = 400;                 // 캐릭터 창이 뜨고 이만큼 지나서 쓴다(memhost4 와 같음)

static bool GameBuildOk()
{
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kGameTimeStamp &&
           nt->OptionalHeader.SizeOfImage == kGameImageSize && (DWORD)(uintptr_t)base == kGameBase;
}

static bool ReadSelf(uintptr_t a, DWORD* v)   // 풀렸을지 모르는 곳(창 포인터 너머)은 이걸로 — 실패하면 false
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, v, sizeof(*v), &n) && n == sizeof(*v);
}

static unsigned char g_hgEntry[hostgame::kEntrySize];       // 지은 항목 · 채널 — 들어가는 동안 정품이 가리킨다(프로세스 끝까지 둔다)
static unsigned char g_hgChannel[hostgame::kChannelSize];

static DWORD WINAPI HostGameThread(LPVOID)
{
    bool done = false;
    DWORD prevCoj = 0, aliveSince = 0;
    for (;;) {
        Sleep(50);
        const DWORD coj = *(volatile DWORD*)kCreateOrJoin;
        const DWORD netMode = *(volatile DWORD*)kNetMode;
        if (done && coj == 1 && prevCoj != 1) done = false;   // Host Game 을 다시 눌렀다
        prevCoj = coj;
        const DWORD win = *(volatile DWORD*)kCharWin;
        DWORD flags = 0;
        const bool alive = win && ReadSelf(win + 4, &flags) && flags != 0xACACACAC && (flags & 0x810) == 0;
        if (!alive) aliveSince = 0;
        else if (!aliveSince) aliveSince = GetTickCount() | 1;
        if (done || coj != 1 || netMode != 1 || !alive || GetTickCount() - aliveSince < kAliveMs) continue;
        done = true;
        if (!g_ourLobby) { Log("Host Game — 우리 로비가 아님(표시 줄 없음) · 원작 만들기 그대로"); continue; }
        const std::string nick = NickNow();
        const std::string chan = nick + "'s_game";
        if (nick.empty() || chan.size() >= hostgame::kChannelNameSize) {
            Log("Host Game — 돌리지 않음(닉 「%s」 — 비었거나 채널 이름 칸 16자를 넘음)", nick.c_str());
            continue;
        }
        const DWORD ip = (DWORD)g_lobbyIp;
        const nox::wol::Raw raw = nox::wol::Marshal(hostgame::MyRoomInfo(nick));
        hostgame::BuildChannel(g_hgChannel, chan, 1, ip, raw);
        hostgame::BuildEntry(g_hgEntry, (uint32_t)(uintptr_t)g_hgEntry, (uint32_t)(uintptr_t)g_hgChannel, Dotted(ip), raw);
        *(volatile DWORD*)kSelEntry = (DWORD)(uintptr_t)g_hgEntry;
        *(volatile DWORD*)kSelValid = 1;
        *(volatile DWORD*)kJoinGate = 1;
        *(volatile WORD*)kSelPort = hostgame::kGamePort;
        *(volatile DWORD*)kCreateOrJoin = 0;
        Log("Host Game → 내 방 들어가기로 돌림 — 채널 #%s · 게임 서버 %s:%u · 항목 %p", chan.c_str(), Dotted(ip).c_str(),
            (unsigned)hostgame::kGamePort, (void*)g_hgEntry);
    }
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
    if ((g_listOn || g_hostGameOn) && buf && len > 0 && LobbyFilterOf(s)) NoteNick(buf, len);   // ⑤ · ⑥
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
                     [&](const std::string& line) { OnControlLine(s, line, who); });
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
        char sw[8] = { 0 };
        const bool off = GetEnvironmentVariableA("NOX_NO_HOSTGAME", sw, sizeof(sw)) && sw[0] == '1';   // ⑤ · ⑥ 끄기
        sw[0] = 0;
        const bool forceList = GetEnvironmentVariableA("NOX_MYROOM_LINE", sw, sizeof(sw)) && sw[0] == '1';   // ⑦ 시험
        const bool gameOk = GameBuildOk();
        g_hostGameOn = gameOk && !off;
        g_listOn = (gameOk || forceList) && !off;
        if (g_hostGameOn) CloseHandle(CreateThread(NULL, 0, HostGameThread, NULL, 0, NULL));

        char dir[MAX_PATH];
        if (GetEnvironmentVariableA("NOX_HOOK_LOG_DIR", dir, MAX_PATH)) {
            char exe[MAX_PATH]; exe[0] = 0;
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
            _snprintf(g_log, MAX_PATH - 1, "%s\\wsock_%s_%lu.log", dir, base, GetCurrentProcessId());
            char self[MAX_PATH]; self[0] = 0;
            GetModuleFileNameA(h, self, MAX_PATH);
            Log("=== wsock32 프록시(통과만) 붙음 ===  exe=%s  나=%s", exe, self);
            Log("Host Game 돌리기 = %s · 내 방 줄 = %s (판 확인 %s · 끄기 NOX_NO_HOSTGAME %s · 시험 NOX_MYROOM_LINE %s) — 둘 다 우리 로비 표시를 받아야 움직임",
                g_hostGameOn ? "켬" : "끔", g_listOn ? "켬" : "끔", gameOk ? "통과" : "다른 판", off ? "켬" : "끔", forceList ? "켬" : "끔");
            CloseHandle(CreateThread(NULL, 0, SummaryThread, NULL, 0, NULL));
        }
    }
    return TRUE;
}
