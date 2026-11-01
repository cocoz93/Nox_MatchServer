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
//      → 판 확인(PE 시각 · 이미지 크기 · 실린 자리)이 맞을 때만 E · F · I 를 켠다
//   H. 이 DLL 은 다른 로비(XWIS 등)에 붙을 때도 실린다
//      → 로비가 `NOXSERVER 1` 로 답한 「우리 로비」에서만 E · F 를 켠다 — 아니면 원작 그대로
//   I. ★ 10-20 v2 작업 5 · 10-21 고침 — 대화방 화면 오른쪽 위 채널 목록을 누르면 정품이 가끔 죽는다(오류 기록 0x4793D 넷 — 10-06 둘 · 10-11 · 10-15).
//      대화방 창(0x44790A~0x44793D)이 누른 칸의 글자를 목록에 묻는데(창 메시지 0x4016) 정품 목록은 **칸 번호를 안 따지고**
//      「항목 배열 + 524×칸 + 4」를 돌려준다(0x4a2a12 · 목록 데이터 = 창+0x20 · 배열 +0x18 · 개수 +0x2c 2바이트 — 0x4a23f0 · 0x4a29dc).
//      칸이 −1 · 개수 이상이면 배열 밖을 읽고, 그 주소가 메모리에 없으면 `cmp word ptr [eax],0` 접근 위반 0x44793D.
//      (10-20 시범은 opennox `GAME3.c:773-775` 의 「범위 밖이면 0」을 정품으로 읽었다 — 그건 opennox 가 넣은 검사. 10-21 ⑨ 시험: 칸 −1 에 0 이 안 옴)
//      v2 는 배틀넷 꼴로 채팅 채널을 쓰므로(10-20 결정) 사람이 그 목록을 자주 누른다
//      → 그 자리 9바이트(0x44793A)를 작은 동굴로 돌려 칸이 0 ~ 개수−1 밖이면 글자를 안 읽고 「빈 칸」 길(0x447992 — 목록만 다시 그리고 끝)로 보낸다(⑧).
//        그래도 그 줄에서 접근 위반이 나면 예외 처리기가 그때 값을 적고 같은 길로 넘긴다(안전망). 판 확인만 — 우리 로비와 무관한 정품 화면 결함
//
//   그래서 — 정품의 화면 · 버튼 · 프로토콜은 그대로 쓰고, 정품이 못 하는 「누가 몇 번 방인지 알리기」와 「우리 서버에 방 만들기」만
//   이 DLL 이 통신 길목과 메모리 다섯 칸으로 메운다. 정품 화면 결함 하나(I)는 코드 9바이트를 돌려 막는다.
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
//   ★ 로그인 암호 조각 5 의 3 — `NOXCLIENT 3`. 로비 연결의 보내기는 `CLoginRewriter`(Lobby 모드)를 거친다: `NICK` 을 기억해 두고 `apgar <암호> 0` 의 암호 자리를
//      로그인 서버(nox_auth · 로비 IP:8443 · `AuthClient.h`)가 준 로그인 토큰(또는 실패 표시)으로 바꿔 보낸다. 평문 암호는 TLS 안에서만 나간다(`Docs/LOGIN_AUTH_DESIGN.md`).
//      게임엔 원래 길이를 돌려준다
//   ★ 조각 5 의 4 — 라우터 연결(connect 상대 포트 4005)의 보내기도 `CLoginRewriter`(Router 모드)를 거친다: `whereto <이름> <암호> …` 의 암호 칸을 `x` 로.
//      라우터는 암호를 안 쓰므로(로비 주소만 알려 줌) 평문 암호가 거기로 나갈 까닭이 없다. 손님(빈 칸)은 그대로
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
// ★ 10-20 MO 6단계 v2 작업 5 — ⑧ 대화방 목록 빈 칸 가드(위 I · `Docs/LOBBY_SPLIT_DESIGN.md` 작업 5 · v2 시범의 ⑧ 을 본판으로 —
//   시범의 시작 신호 · 버튼 대신 누르기 · 메시지 훅은 안 가져옴). 판 확인이 맞고 그 자리 바이트가 정품 그대로일 때 DLL 이 실리며 붙인다.
//   정품 클라의 대화방 목록 빈 칸 버그(눌린 칸 번호를 범위 검사 없이 씀)를 메모리에서만 막는다 — 게임 파일은 고치지 않는다.
//   그 자리를 지나간 수 · 막은 수(범위 밖) · 안전망이 받은 수는 바뀌었으면 1초 안에 로그 한 줄
//   (10-21 — 막은 수만 10초마다 찍어 0 이 「안 막음」인지 「안 지나감」인지 몰랐다).
//   끄기: 환경변수 NOX_NO_CHANGUARD=1(정품이 죽는지 다시 보는 회차용 — 동굴도 안전망도 없음)
//   ⑨ 시험용 — 환경변수 NOX_TEST_CHANGUARD=1 이면(판 확인 통과일 때만) 대화방에서 채널에 들어가고 3초 뒤 한 번, 목록이 「누름」을 알리는
//      메시지(16400 = 0x4010)를 대화방 창에 넣는다(목록이 보내는 것과 같은 꼴 — 0x4a1e2d~0x4a1e32). 칸은 차례로
//      ① −1 — 항목 아래 빈 칸을 누른 값(0x4a1dee) ② 「죽는 칸」 — 배열 + 524×칸 + 4 가 맨 앞 64KB(늘 비어 있는 주소)에 떨어지게 고른 번호
//      ③ 가드가 있으면 범위 검사만 잠깐 끄고 ② 한 번 더 — 안전망이 받는지.
//      가드가 있으면 안 죽고 막음 +2 · 안전망 +1 · NOX_NO_CHANGUARD=1 과 같이 켜면 ② 에서 0x44793D 로 죽어야 맞다 — 사람은 로그인만
//
// ★ 랭크 매칭 조각 2(`Docs/RANK_MATCH_PLAN.md`) — ⑩ 게임 목록 여섯째 칸(10007 · 글자는 tools/ui_apply.ps1 이 「랭크 매칭」으로) 누름을
//   랭크 표시 + 「게임 호스트하기」 누름으로 바꾼다(게임 목록 창 처리 함수 칸을 감쌈 — 아래 ⑩ 절). 켜는 조건 = ⑥ 과 같음(판 확인 · 우리 로비 · NOX_NO_HOSTGAME)
// ★ 랭크 매칭 조각 3 — ⑪ 그 뒤 내 방 참가 줄 앞에 `NOXRANK JOIN`(로비가 새 방 대신 줄에 세움) · 「서버 접속 시도중...」 창이 닫히면 다음 보내기 앞에
//   `NOXRANK LEAVE`(로비가 줄에서 뺌) · 참가 토큰이 오면 기다림 끝(아래 ⑪ 절). DLL 판 4. 시험 스위치 NOX_TEST_RANKMARK=1(판 확인 없이 내 방 참가마다)
//
// 로그: 환경변수 NOX_HOOK_LOG_DIR 이 있을 때만 <그 폴더>\wsock_<exe>_<pid>.log 에 쓴다(dinput_proxy 와 같은 스위치).
//       없으면 로그도 집계도 안 하고 진짜로 넘기기만 한다.
//       상세 줄 = 접속 시도(UDP 3번째 바이트 0x0e)의 바이트 전부 · TCP connect/send/recv · 이름 찾기
//       ★ 로그인 암호 조각 5 — 보낸 글의 미리 보기는 `apgar` 인자 · `whereto` 암호 칸을 `***` 로 가린 글(`LoginRewriter.h` Log 모드 · 가린 줄은 줄 끝이 올 때까지 늦게 나옴)
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
#include "LoginRewriter.h"     // ★ 로그인 암호 조각 5 — 기록에 찍는 보낸 글에서 암호 칸 가리기(Log 모드) · 로비 연결의 apgar 자리 바꿔쓰기(Lobby 모드)
#include "AuthClient.h"        // ★ 로그인 암호 조각 5 의 3 — 로그인 서버(nox_auth)에 이름 · 암호를 묻는 부품(WinHTTP) · ★ 런처 P4 — 이제 이름 · 열쇠로 토큰
#include "../common/LaunchKey.h"   // ★ 런처 P4 — 런처가 이 PC 에 둔 자동 로그인 열쇠(DPAPI)

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

static volatile LONG g_chanGuardHits = 0;   // ★ 10-20 v2 작업 5 — ⑧ 가드가 막은 횟수(10-21 부터 칸이 범위 밖일 때 · 동굴 코드가 센다)
static volatile LONG g_chanGuardPass = 0;   // ★ 10-21 — ⑧ 그 자리를 지나간 횟수(막음 포함 · 동굴 코드가 센다) — 막음 0 옆에 같이 읽는다
static volatile LONG g_chanGuardFault = 0;  // ★ 10-21 — ⑧ 그래도 그 줄에서 접근 위반이 나 안전망이 받은 횟수
static volatile LONG g_chanGuardNoRange = 0;   // ★ 10-21 — 0 아니면 동굴이 범위 검사를 건너뜀(⑨ 시험 ③ 이 안전망을 보려고 잠깐만)
static bool g_chanGuardOn = false;          // ★ 10-21 — ⑧ 동굴을 붙였나(⑨ 시험 줄에 적는다)

static void RankSummary();
static void MenuSummary();   // ★ 매칭 손보기 8 — ⑫
static void AcceptSummary(); // ★ 매칭 손보기 6 — ⑬
static void NoticeSummary(); // ★ 10-31 — ⑭

static DWORD WINAPI SummaryThread(LPVOID)
{
    static ModStat last[kMaxMods];
    for (unsigned tick = 1;; ++tick) {
        Sleep(1000);
        static LONG passSeen = 0, guardSeen = 0, faultSeen = 0;   // ★ 10-21 — ⑧ 셋 중 하나라도 바뀌었으면 한 줄(1초마다 봄)
        const LONG pass = g_chanGuardPass, hits = g_chanGuardHits, faults = g_chanGuardFault;
        if (pass != passSeen || hits != guardSeen || faults != faultSeen) {
            Log("⑧ 대화방 목록 가드 — 지나감 누적 %ld번 · 막음(칸 범위 밖) 누적 %ld번 · 안전망 누적 %ld번", pass, hits, faults);
            passSeen = pass; guardSeen = hits; faultSeen = faults;
        }
        RankSummary();   // ★ 랭크 매칭 조각 2 — ⑩ 셈이 바뀌었으면 한 줄
        MenuSummary();   // ★ 매칭 손보기 8 — ⑫ 도
        AcceptSummary(); // ★ 매칭 손보기 6 — ⑬ 도
        NoticeSummary(); // ★ 10-31 — ⑭ 도
        if (tick % 10) continue;   // 모듈 요약은 10초마다
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
//   ★ 판 3 = 로그인 암호(`kNoxClientAuthVersion`) — 로비가 이 판 이상만 암호 판으로 봄
//   ★ 판 4 = 랭크 매칭(`kNoxClientRankVersion`) — ⑪ `NOXRANK JOIN` · `NOXRANK LEAVE` 를 보냄 · 로비는 이 판 이상의 표시만 믿음
//   ★ 판 5 = 수락 단계(`kNoxClientAcceptVersion` · 매칭 손보기 6) — ⑬ 로비 `NOXRANK FOUND` 에 「수락?」 · 예면 `NOXRANK ACCEPT`
static const char kNoxClientHello[] = "NOXCLIENT 5\r\n";
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
static std::map<SOCKET, CLoginRewriter*> g_logRedact;   // ★ 조각 5 — 기록용 줄 상태(소켓마다 · 기록을 켰을 때만 생김) · g_tokCs 로 지킨다
static std::map<SOCKET, CLoginRewriter*> g_loginRw;     // ★ 조각 5 의 3 · 4 — 로비 연결(Lobby 모드) · 라우터 연결(Router 모드)마다 로그인 줄 바꿔쓰기(늘 생김) · g_tokCs 로 지킨다
static bool g_listOn = false;          // ⑤ 줄 끼우기를 걸어 두나(판 확인 통과 또는 ⑦ 시험 스위치 · 끄기 스위치 없음)
static bool g_hostGameOn = false;       // ⑥ Host Game 돌리기 스레드(판 확인 통과 · 끄기 스위치 없음)
static volatile LONG g_ourLobby = 0;    // ⑤ · ⑥ 지금 로비 연결이 `NOXSERVER` 를 보냈나 — 새 로비 연결마다 0
static volatile LONG g_lobbyIp = 0;     // 그 로비 연결의 상대 IPv4(a<<24|b<<16|c<<8|d) — 채널이 곧 게임 서버
static std::string g_nick;              // ⑤ · ⑥ 로비 로그인의 `NICK <닉>` — g_tokCs 로 지킨다

static std::string MyRoomLine(const std::vector<std::string>& items);
static std::string AuthLogin(SOCKET s, const std::string& name, const std::string& password);
static void RankOnToken();   // ★ 랭크 매칭 조각 3 — ⑪
static void RankOnLobbyLine(SOCKET s, const std::string& line);   // ★ 매칭 손보기 6 — ⑬

static bool MarkLobby(SOCKET s)   // 새로 적었으면 true
{
    EnterCriticalSection(&g_tokCs);
    const bool added = g_lobby.find(s) == g_lobby.end();
    if (added) {
        LobbyLineFilter* f = new LobbyLineFilter();
        if (g_listOn) f->onListEnd = [](const std::vector<std::string>& items) { return MyRoomLine(items); };
        g_lobby[s] = f;
        CLoginRewriter* rw = new CLoginRewriter(CLoginRewriter::Mode::Lobby);   // ★ 조각 5 의 3 — 이 연결의 apgar 줄을 로그인 토큰으로
        rw->onLogin = [s](const std::string& name, const std::string& password) { return AuthLogin(s, name, password); };
        CLoginRewriter*& old = g_loginRw[s];
        delete old;   // 닫힘 없이 같은 번호가 다시 CVERS 를 보낸 경우(closesocket 이 지우므로 드묾)
        old = rw;
        InterlockedExchange(&g_ourLobby, 0);   // 새 로비 연결 — 표시 줄을 다시 받아야 켠다
    }
    LeaveCriticalSection(&g_tokCs);
    return added;
}

// ★ 조각 5 — 보낸 바이트를 기록에 찍을 글로. `apgar` 인자 · `whereto` 암호 칸이 `***` 로 가려진다.
//   정품은 로그인 묶음을 64바이트씩 쪼개 보내므로(암호 줄이 두 번에 걸칠 수 있다) 소켓마다 줄 상태를 쥔다 —
//   가린 글은 줄 끝이 올 때까지 늦게 나온다. 소켓이 로비인지 라우터인지 몰라도 되게 둘 다 가린다
static std::string RedactedForLog(SOCKET s, const char* buf, int len)
{
    std::string out;
    EnterCriticalSection(&g_tokCs);
    CLoginRewriter*& rw = g_logRedact[s];
    if (!rw) rw = new CLoginRewriter(CLoginRewriter::Mode::Log);
    rw->Feed(buf, len);
    rw->Flush([&](const char* b, int n) { out.append(b, static_cast<size_t>(n)); return n; }, [] { return false; });
    LeaveCriticalSection(&g_tokCs);
    return out;
}

// ★ 런처 P4 — 런처로 켰나: 런처가 Game.exe 를 띄울 때 환경 변수 NOX_LAUNCH=<계정 이름>을 넣는다(열쇠는 안 넣음 — `LaunchKey.h`).
//   부를 때마다 읽는다 — 시험(selftest)이 프로세스 안에서 바꿔 본다. 이름 꼴이 틀리면 없는 것으로
static std::string LaunchName()
{
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA("NOX_LAUNCH", v, sizeof(v));
    const std::string name = n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
    return launchkey::NameOk(name) ? name : std::string();
}

// ★ 조각 5 의 3 — 로비 연결의 이름으로 로그인 서버(nox_auth)에서 `apgar` 자리에 넣을 글 한 줄(`NXL1…` 또는 `NXL!…`)을 받아 온다.
//   ★ 런처 P4 — 암호(정품 로그인 창에 친 글)는 안 쓴다. 런처로 켰으면 그 이름의 열쇠(`LaunchKey.h`)로 `/token`, 아니면 안 묻고 `NXL!launcher_required`
//     (11-01 사용자 결정 4 — 서버도 암호 → 토큰 길을 닫음). 열쇠 파일이 없거나 못 풀면 `NXL!key_invalid`(런처로 다시 로그인)
//   주소 = 이 로비 연결의 상대 IP : 8443(서버 `kDefaultAuthPort`) · 환경변수 NOX_AUTH_URL(예: https://host:8443 · 옛 꼴 …/login 도 받음)이 있으면 그것.
//   게임 주 스레드가 최대 3초 막힌다(`AuthClient.h`). TLS 검사를 끄지 않으므로 믿을 수 없는 서버(XWIS 등 우리 로비가 아닌 곳)에는 본문(열쇠)이 나가기 전에 멈춘다
//   ★ 10-28 다른 PC — 환경변수 NOX_AUTH_PIN(서버 인증서 SHA-256 16진 64자 · 꾸러미 2_PLAY 가 넣음)이 있으면 윈도 검사 대신 그 지문으로 가른다
static std::string AuthLogin(SOCKET s, const std::string& name, const std::string& password)
{
    (void)password;   // ★ 런처 P4 — 정품 창의 암호 칸은 쓰지 않는다(선에도 안 나감 — apgar 자리를 통째로 바꿈)
    if (LaunchName().empty()) {
        if (g_log[0]) Detail("로그인 서버 안 부름 s=%u — 런처로 안 켬(NOX_LAUNCH 없음) → NXL!launcher_required", (unsigned)s);
        return authclient::detail::FailLine(nox::LoginFail::LauncherRequired);
    }
    std::string key;
    if (!launchkey::Load(name, key)) {
        if (g_log[0]) Detail("로그인 서버 안 부름 s=%u — 「%s」 의 열쇠 파일이 없거나 못 풂 → NXL!key_invalid", (unsigned)s, name.c_str());
        return authclient::detail::FailLine(nox::LoginFail::KeyInvalid);
    }
    char pin[80] = {};
    const DWORD pn = GetEnvironmentVariableA("NOX_AUTH_PIN", pin, sizeof(pin));
    const std::string pinHex = pn > 0 && pn < sizeof(pin) ? std::string(pin, pn) : std::string(pn ? "?" : "");   // 너무 길면 틀린 꼴로 넘겨 붙지 않게
    std::wstring url;
    wchar_t over[512];
    const DWORD en = GetEnvironmentVariableW(L"NOX_AUTH_URL", over, 512);
    if (en > 0 && en < 512) {
        url = over;
    } else {
        typedef int (WSAAPI* PFN_getpeername)(SOCKET, struct sockaddr*, int*);
        sockaddr_in peer = {};
        int plen = (int)sizeof(peer);
        if (((PFN_getpeername)Resolve(5))(s, (struct sockaddr*)&peer, &plen) != 0 || peer.sin_family != AF_INET) {
            if (g_log[0]) Detail("로그인 서버 호출 못 함 — 로비 연결의 상대 주소를 못 읽음 s=%u", (unsigned)s);
            return authclient::detail::FailLine(nox::LoginFail::Unreachable);
        }
        const unsigned char* b = (const unsigned char*)&peer.sin_addr;
        wchar_t buf[64];
        swprintf(buf, 64, L"https://%u.%u.%u.%u:8443", b[0], b[1], b[2], b[3]);
        url = buf;
    }
    url = authclient::WithPath(url, L"/token");
    const authclient::Result r = authclient::Token(url, name, key, authclient::kTimeoutMs, pinHex);
    SecureZeroMemory(&key[0], key.size());
    if (g_log[0]) {
        const bool token = std::string_view(r.line).substr(0, nox::kLoginTokenMark.size()) == nox::kLoginTokenMark;
        Detail("로그인 서버 호출 s=%u 상태=%d 오류=%lu %lums 지문=%s → %s", (unsigned)s, r.status, r.error, r.ms, pinHex.empty() ? "없음" : "고정",
               token ? "로그인 토큰(NXL1…)" : r.line.c_str());   // 토큰 글은 기록에 안 남김
    }
    return r.line;
}

static LobbyLineFilter* LobbyFilterOf(SOCKET s)
{
    EnterCriticalSection(&g_tokCs);
    auto it = g_lobby.find(s);
    LobbyLineFilter* f = it == g_lobby.end() ? NULL : it->second;
    LeaveCriticalSection(&g_tokCs);
    return f;   // 그 소켓을 받는 스레드만 쓴다 — closesocket 전까지 유효
}

// ★ 조각 5 의 4 — 라우터 연결(정품 servserv.westwood.com:4005 → 우리 nox_router)로 적는다. 그 소켓의 whereto 암호 칸이 x 로 나간다
static const unsigned short kRouterPort = 4005;
static void MarkRouter(SOCKET s)
{
    CLoginRewriter* rw = new CLoginRewriter(CLoginRewriter::Mode::Router);
    EnterCriticalSection(&g_tokCs);
    CLoginRewriter*& old = g_loginRw[s];
    delete old;   // 같은 소켓이 다시 connect 한 경우
    old = rw;
    LeaveCriticalSection(&g_tokCs);
}

// ★ 10-30 — 국가 고르기 창 막기(같은 PC 두 클라 판). mo_local 이 띄울 때 `HKCU\Software\Classes\Wchat\Nick0~129\Locale` 을 20 으로 두지만,
//   「아이디 기억」을 끈 로그인은 끝에 Nick129 의 국가를 0 으로 지운다(Game.exe `0x41fa94` → `0x40da80`) → 뒤에 로그인하는 다른 클라가 0 을 읽어 창이 뜸
//   (10-30 사람 판 — 판 뒤 Nick129 만 0). 「로그인」 누름의 국가 읽기 바로 앞(InstallCountryHook)에서 20 이 아닌 칸을 다시 20 으로. 서버는 이 값을 안 쓴다
#pragma comment(lib, "advapi32.lib")
static void KeepCountry()
{
    int fixed = 0, fail = 0;
    for (int n = 0; n <= 129; n++) {
        wchar_t path[64];
        swprintf_s(path, L"Software\\Classes\\Wchat\\Nick%d\\Locale", n);
        HKEY k = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) { fail++; continue; }
        wchar_t v[8] = {};
        DWORD type = 0, sz = sizeof(v) - sizeof(wchar_t);
        const bool is20 = RegQueryValueExW(k, NULL, NULL, &type, (BYTE*)v, &sz) == ERROR_SUCCESS && type == REG_SZ && wcscmp(v, L"20") == 0;
        if (!is20) {
            if (RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE*)L"20", 3 * sizeof(wchar_t)) == ERROR_SUCCESS) fixed++;
            else fail++;
        }
        RegCloseKey(k);
    }
    Log("국가 칸 — 국가 읽기 앞 Nick0~129 Locale 20 확인 · 다시 씀 %d · 실패 %d", fixed, fail);
}

static CLoginRewriter* LoginRewriterOf(SOCKET s)   // 로비 연결이 아니면 NULL · 그 소켓을 보내는 스레드만 쓴다(LobbyFilterOf 와 같은 가정)
{
    EnterCriticalSection(&g_tokCs);
    auto it = g_loginRw.find(s);
    CLoginRewriter* r = it == g_loginRw.end() ? NULL : it->second;
    LeaveCriticalSection(&g_tokCs);
    return r;
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
    RankOnToken();   // ★ 랭크 매칭 조각 3 — ⑪ ③ 줄 서 있었으면 잡힌 것
}

// ② 떼어 낸 줄 — 토큰이면 쥐고, 우리 로비 표시(`NOXSERVER`)면 그 연결의 상대 주소를 적고 ⑤ · ⑥ 을 켠다
static void OnControlLine(SOCKET s, const std::string& line, const char* who)
{
    if (line.compare(0, strlen(LobbyLineFilter::kRankMark), LobbyLineFilter::kRankMark) == 0) { RankOnLobbyLine(s, line); return; }   // ★ ⑬
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

// ★ 10-21 ⑨ — ⑧ 가드 시험(머리 주석 ⑨). 칸 주소는 판 확인한 Game.exe 의 것(정적 분석 — `nox-nine-defects-progress.md` 10-21 01:05 판)
static const uintptr_t kWolState = 0x652F18;       // WOL 화면 상태(7 = 대화방 · `0x41e060`)
static const uintptr_t kChatRoot = 0x69C8BC;       // 대화방 창(창 proc 0x447840 · 닫으면 0 — `0x448025`)
static const uintptr_t kChatList = 0x69C8C0;       // 그 채널 목록(창 번호 0x774 · `0x447273`)
static const uintptr_t kCurChan = 0x65344C;        // 지금 들어간 채널(없으면 0 · `0x41e780`)
static const uintptr_t kChatProc = 0x447840;       // 대화방 창 proc — 16400 은 0x44790A 로(분기표 0x447C54 · 0x447C40)
static const DWORD kChatListId = 0x774;
static const DWORD kTestDelayMs = 3000;            // 채널에 들어가고 이만큼 지나서(목록이 다 찬 뒤)
static bool g_testGuard = false;                   // NOX_TEST_CHANGUARD=1 · 판 확인 통과
static DWORD g_mainTid = 0;                        // DLL 을 실은 스레드 = 게임 주 스레드(정품이 들여오는 DLL — 시범에서 recv · 게임 창과 같은 번호)

// 목록이 누름을 알릴 때와 같은 꼴로 대화방 창 proc 를 부른다 — 부르기 전 줄은 죽어도 남는다
static void SendListClick(DWORD root, DWORD list, int idx, const char* what)
{
    const LONG p0 = g_chanGuardPass, h0 = g_chanGuardHits, f0 = g_chanGuardFault;
    Log("⑨ 가드 시험 %s — 칸 %d 누름(16400) 보냄", what, idx);
    typedef int(__cdecl * PFN_wndproc)(DWORD win, DWORD msg, DWORD a3, int a4);
    ((PFN_wndproc)kChatProc)(root, 0x4010, list, idx);
    Log("⑨ 가드 시험 %s — 돌아옴(안 죽음) · 지나감 +%ld · 막음 +%ld · 안전망 +%ld", what, (LONG)g_chanGuardPass - p0,
        (LONG)g_chanGuardHits - h0, (LONG)g_chanGuardFault - f0);
}

// 게임 주 스레드에서만 — 로비 모듈의 recv 가 거기서 불리고, 로비 이벤트를 받는 정품 함수들도 그 안에서 화면을 고친다(OnChannelJoin 0x40c240 등)
static void ChanGuardTest()
{
    static bool fired = false;
    static DWORD since = 0;
    if (fired || GetCurrentThreadId() != g_mainTid) return;
    const DWORD root = *(volatile DWORD*)kChatRoot, list = *(volatile DWORD*)kChatList;
    if (*(volatile DWORD*)kWolState != 7 || !root || !list || !*(volatile DWORD*)kCurChan) { since = 0; return; }
    if (!since) { since = GetTickCount() | 1; return; }
    if (GetTickCount() - since < kTestDelayMs) return;
    fired = true;
    DWORD id = 0, data = 0, items = 0, count = 0;
    if (!ReadSelf(list, &id) || id != kChatListId) { Log("⑨ 가드 시험 — 안 보냄(목록 창 번호 %#lx · 기대 0x774)", id); return; }
    if (!ReadSelf(list + 0x20, &data) || !ReadSelf(data + 0x18, &items) || !ReadSelf(data + 0x2c, &count) || !items) {
        Log("⑨ 가드 시험 — 안 보냄(목록 데이터 %#lx · 배열 %#lx 를 못 읽음)", data, items);
        return;
    }
    // ② 죽는 칸 — 배열 + 4 + 524×칸 이 2^32 를 막 넘어 [0, 524) 에 떨어지는 가장 작은 칸(맨 앞 64KB 는 Windows 가 늘 비워 둔다)
    const DWORD crashIdx = (DWORD)((0x100000000ULL - (items + 4ULL) + 523) / 524);
    Log("⑨ 가드 시험 — 가드 %s · 스레드 %lu · 목록 개수 %d · 배열 %#lx · 죽는 칸 %ld(읽을 주소 %#lx)",
        g_chanGuardOn ? "붙음" : "없음(② 에서 0x44793D 로 죽어야 맞다)", GetCurrentThreadId(), (int)(short)(count & 0xFFFF), items,
        (LONG)crashIdx, (DWORD)(items + 4 + 524ULL * crashIdx));
    SendListClick(root, list, -1, "① 빈 칸");
    SendListClick(root, list, (int)crashIdx, "② 죽는 칸");
    if (g_chanGuardOn) {
        g_chanGuardNoRange = 1;
        SendListClick(root, list, (int)crashIdx, "③ 죽는 칸 · 범위 검사 끔(안전망이 받아야 함)");
        g_chanGuardNoRange = 0;
    }
}

// ─────────────────────────────────────────────── ⑩ 랭크 매칭 — 게임 목록 여섯째 칸 누름 (랭크 매칭 조각 2 · Docs/RANK_MATCH_PLAN.md)
// 게임 목록 창(`[0x699018]` · 처리 함수 칸 +0x178 = 0x439a30 — 만들 때 `0x437884` 가 `0x488bd0` 으로 넣음)을 DLL 함수로 감싼다.
//   버튼 누름은 창 메시지 0x4007 로 온다(셋째 인자 = 눌린 버튼 창 · 번호는 창 +0 — `0x4889b0` · 갈래 `0x439a59`~`0x439a76`).
//   10007(원래 「찾기」 — 글자는 tools/ui_apply.ps1 이 「랭크 매칭」으로)이면 랭크 표시를 켜고 「게임 호스트하기」(10002) 누름을 원래 함수에 대신 넣는다.
//   그 뒤는 Host Game 과 같은 길(캐릭터 고르기 → ⑥ 내 방 들어가기). 랭크 표시는 ⑪ 이 그 참가 줄 앞에 `NOXRANK JOIN` 을 끼울 때 쓴다(조각 3).
//   나머지 메시지는 원래 함수로. 사람이 진짜 10002 를 누르면 남은 표시를 끈다(캐릭터 고르기에서 뒤로 나와 일반 방을 만들 때 랭크로 새지 않게)
// 감싸는 자리 = 받기(recv) 훅 — 게임 주 스레드에서만(목록 창은 만들자마자 LIST 를 보내 곧 답이 온다). 칸 값이 원래 함수일 때만 바꾼다
//   (창이 같은 주소에 다시 만들어져도 칸 값으로 가른다)
// 10007 숨김 — 정품이 WOL 모드 목록을 만들 때 스스로 보이게 한다(`0x437948` · 숨김 비트 0x10). 감쌀 때 그 비트를 기록에 찍고, 켜져 있으면 끈다
//   (10-30 사용자 「찾기 버튼이 안 보인다」와 정적 분석이 어긋남 — 이 줄로 가린다)
static const uintptr_t kListWin = 0x699018;        // 게임 목록 창(없으면 0)
static const uintptr_t kListProc = 0x439a30;       // 그 창의 원래 처리 함수
static const uintptr_t kFindChild = 0x4889d0;      // (창, 번호) → 자식 창(재귀)
static const uintptr_t kSetHidden = 0x488510;      // (창, 0 이면 보임 · 아니면 숨김)
static const DWORD kProcSlot = 0x178;
static const DWORD kBtnHost = 10002, kBtnRank = 10007;
static const DWORD kMsgClick = 0x4007;
static const DWORD kWinHidden = 0x10;
static bool g_rankOn = false;                      // 판 확인 통과 · ⑥ 켬(⑥ 길을 타므로 같이 켜고 같이 끈다)
static volatile LONG g_rankArmed = 0;              // 랭크 매칭을 눌렀다 — ⑪ 이 다음 내 방 참가 줄에 표시를 끼우고 끈다
static volatile LONG g_rankWrapped = 0;            // 감싼 목록 창 수
static volatile LONG g_rankWrapSkip = 0;           // 처리 함수 칸이 원래 함수가 아니라 안 감싼 창 수
static volatile LONG g_rankClicks = 0;             // 10007 누름 → 랭크(가로챔)
static volatile LONG g_rankNotOurs = 0;            // 10007 누름인데 우리 로비가 아님 → 원래 「찾기」로 넘김
static volatile LONG g_rankNoHost = 0;             // 10007 누름인데 10002 버튼을 못 찾음(아무것도 안 함)
static volatile LONG g_rankHostClicks = 0;         // 사람이 누른 10002(넘김 · 남은 표시 끔)
static volatile LONG g_rankOtherClicks = 0;        // 그 밖의 누름(넘김)
static volatile LONG g_rankInnerClicks = 0;        // 그중 대신 넣은 10002 를 원래 함수가 처리하는 동안 온 것(표시 그대로)
// 대신 넣은 10002 처리 중 — 정품 Host Game 은 지역을 안 골랐으면(`[0x587000+87412]` = -1) 세계 지도 칸(10054~10057) 하나를 무작위로 골라
//   이 창에 누름을 곧바로 다시 넣는다(OpenNox `noxworld.c:850-857` — OpenNox 는 `if (0)` 으로 껐지만 정품은 탐).
//   그 누름이 「다른 누름」 갈래로 표시를 꺼 랭크가 일반 방으로 샜다(10-30 사람 판 — 같은 1/1000초에 10055 · 10057). 게임 주 스레드만 씀
static bool g_rankInjecting = false;

typedef int(__cdecl* PFN_wndproc4)(DWORD win, DWORD msg, DWORD a3, DWORD a4);
typedef DWORD(__cdecl* PFN_findchild)(DWORD win, DWORD id);
typedef int(__cdecl* PFN_sethidden)(DWORD win, DWORD hide);

static bool RankFoundClick(DWORD id);   // ★ 매칭 손보기 6 — ⑬ 찾음 창의 예(4007) · 아니오(4006) — 가로챘으면 true
static void RankLeaveNow(const char* why);   // ★ 10-31 — ⑪ 기다리는 중이면 그 자리에서 NOXRANK LEAVE(⑬ 절)
static volatile LONG g_rankFoundState = 0;   // ★ ⑬ 0 없음 · 1 찾음(예 · 아니오 창) · 2 수락함(남 기다림) — ④ 의 1초 고침을 멈춤
// ★ 10-31 — ⑬ 찾음 창 남은 초(`NOXRANK FOUND <초>` 의 초 · DLL 시각으로 셈). 1 동안 ④ 자리가 1초마다 이 버퍼를 고친다
static volatile LONG g_rankFoundSec = 10;      // 로비가 준 수락 한도(초) — 못 읽으면 10(`Lobby.h` kRankAcceptMs)
static DWORD g_rankFoundSince = 0;             // 찾음 창을 띄운 시각(게임 주 스레드만)
static DWORD g_rankFoundShownLeft = 0;         // 창에 낸 남은 초(창을 띄울 때 한도로 맞춤 — 앞 찾음의 값이 남아 한 칸 건너뛰지 않게)
static wchar_t g_rankFoundBuf[64];             // 정품 글 칸이 쥐는 찾음 글(게임 주 스레드만 씀)
static volatile LONG g_rankFoundTicks = 0;     // 남은 초 고침(초가 바뀔 때마다)
static void RankFoundText(DWORD left)
{
    swprintf(g_rankFoundBuf, sizeof(g_rankFoundBuf) / sizeof(g_rankFoundBuf[0]),
             L"매칭 성공! 수락하시겠습니까? 남은 시간 %lu초 (시간이 끝나면 자동 거절)", left);
}

static int __cdecl RankListProc(DWORD win, DWORD msg, DWORD a3, DWORD a4)
{
    const PFN_wndproc4 orig = (PFN_wndproc4)kListProc;
    DWORD id = 0;
    if (msg != kMsgClick || !a3 || !ReadSelf(a3, &id)) return orig(win, msg, a3, a4);
    if ((id == 4006 || id == 4007) && RankFoundClick(id)) return 0;   // ★ ⑬ — 정품 목록 함수로 안 넘김(그 번호를 정품이 어떻게 하는지 모름)
    // ★ 10-31 — 기다림 창(「상대 찾는 중」 · 「상대 수락 기다리는 중」)의 취소(4002)는 그 자리에서 LEAVE. 정품 취소 길은 로비에 아무 줄도 안 보낸다
    //   (10-31 사람 판 — 취소 뒤 LIST 없음 · ⑪ 「다음 보내기 앞」 LEAVE 0 → 로비가 몰라 줄에 남거나 · 다음 랭크 누름의 LIST 를 빠짐으로 읽음). 누름은 정품으로 넘긴다
    if (id == 4002) RankLeaveNow("기다림 창 취소");   // 기다리는 중이 아니면 아무것도 안 함
    switch (id) {
        case kBtnRank: {
            if (!g_ourLobby) {
                InterlockedIncrement(&g_rankNotOurs);
                Log("⑩ 10007 누름 — 우리 로비가 아님 · 원래 「찾기」로 넘김");
                return orig(win, msg, a3, a4);
            }
            const DWORD host = ((PFN_findchild)kFindChild)(win, kBtnHost);
            if (!host) {
                InterlockedIncrement(&g_rankNoHost);
                Log("⑩ 랭크 매칭 누름 — 게임 호스트하기(10002) 버튼을 못 찾아 아무것도 안 함");
                return 0;
            }
            const LONG n = InterlockedIncrement(&g_rankClicks);
            InterlockedExchange(&g_rankArmed, 1);
            Log("⑩ 랭크 매칭 누름(누적 %ld) — 랭크 표시 켬 · 게임 호스트하기(10002 창 %#lx) 누름을 대신 넣음", n, host);
            g_rankInjecting = true;
            const int r = orig(win, kMsgClick, host, a4);
            g_rankInjecting = false;
            return r;
        }
        case kBtnHost:
            InterlockedIncrement(&g_rankHostClicks);
            if (InterlockedExchange(&g_rankArmed, 0)) Log("⑩ 게임 호스트하기 누름 — 남아 있던 랭크 표시 끔(일반 방)");
            return orig(win, msg, a3, a4);
        default:   // ★ 조각 3 — 다른 누름도 남은 표시를 끈다(퀘스트 호스트 · 목록 줄 고르기 … 랭크를 누른 뒤 캐릭터 고르기에서 뒤로 나온 경우)
            InterlockedIncrement(&g_rankOtherClicks);
            if (g_rankInjecting) {
                InterlockedIncrement(&g_rankInnerClicks);
                Log("⑩ 게임 호스트하기 안의 누름(번호 %lu) — 랭크 표시 그대로", id);
            } else if (InterlockedExchange(&g_rankArmed, 0)) {
                Log("⑩ 다른 누름(번호 %lu) — 남아 있던 랭크 표시 끔", id);
            }
            return orig(win, msg, a3, a4);
    }
}

// ─────────────────────────────────────────────── ⑪ 랭크 참가 표시 · 기다림 창 닫힘 알림 (랭크 매칭 조각 3)
// 랭크도 로비엔 Host Game 과 같은 참가 줄(`JOINGAME #<닉>'s_game 1 `)로 간다 — 사람 확인을 거친 길 그대로(목록에 새 채널 줄을 끼우지 않는다 —
//   정품은 받은 목록에 있는 채널에만 참가를 보내고 · 제 이름 방을 안 그리는 기준을 모른다). 다른 것은 그 줄 바로 앞의 표시 한 줄뿐:
//   ① 랭크 표시(⑩)가 켜져 있고 로비로 내 방 참가 줄이 나가면 그 보내기 앞에 `NOXRANK JOIN` → 로비가 새 방 대신 줄에 세운다. 「기다림」 켬
//      (정품은 참가 줄 하나를 보내기 한 번에 낸다 — 10-20 · 10-29 기록 `send … len=23 | JOINGAME #1's_game 1 \r\n`)
//   ② 기다리는 동안 로비로 나가는 보내기마다 「서버 접속 시도중...」 창을 본다 — 정품 공용 알림창 `[0x69cba4]`(띄움 `0x43b1b0` → `0x449590` ·
//      닫으면 0 `0x449f80`). 닫혀 있으면 그 보내기 앞에 `NOXRANK LEAVE` · 기다림 끔.
//      취소 = 단추 0xfa2 → 목록 창 `0x439c3b` → 창 닫음(`0x43a4e0`) → 목록 다시(`0x4375b0` → LIST) — 그래서 그 LIST 앞에 실린다(정적 분석 · 정품 화면 미확인)
//      ✕ 10-31 사람 판 — 취소 뒤 LIST 가 안 나감(다음 누름 때에야). 이제 취소 · 아니오 · 한도는 ⑬ 의 RankLeaveNow 가 그 자리에서 보내고, 여기는 놓친 경우의 뒷받침
//   ③ 로비 참가 답의 토큰(`NOXTOKEN`)이 오면 기다림 끔(잡혔다 — 창은 판으로 가며 닫힌다)
//   보내는 자리는 게임이 그 소켓에 보내는 순간뿐(따로 스레드에서 쓰지 않는다 — 로비 모듈의 줄과 섞이지 않게)
// 시험 스위치 NOX_TEST_RANKMARK=1 — 판 확인 없이 내 방 참가마다 ① (selftest — 그 exe 엔 알림창 칸이 없어 「닫힘」으로 읽힌다)
static const uintptr_t kMsgBox = 0x69cba4;
static const char kRankJoinLine[] = "NOXRANK JOIN\r\n";
static const char kRankLeaveLine[] = "NOXRANK LEAVE\r\n";
static bool g_rankMarkTest = false;
static volatile LONG g_rankWaiting = 0;    // ① 뒤 · ② · ③ 전
static volatile LONG g_rankMarked = 0;     // ① 참가 줄 앞 표시
static volatile LONG g_rankLeaves = 0;     // ② 창 닫힘 → LEAVE
static volatile LONG g_rankTokenEnds = 0;  // ③ 토큰 → 기다림 끝

static bool MsgBoxOpen()
{
    // ★ 10-30 — 정품 판이 아니면(시험 exe) 늘 닫힘. 그 주소가 시험 exe 의 다른 메모리에 걸려 0 이 아닌 값을 읽으면
    //   「떠 있음」이 되어 selftest 의 LEAVE 확인이 가끔 실패했다(test.bat 두 번에 한 번 — 매칭 손보기 8b 빌드 때)
    if (!g_rankOn) return false;
    DWORD v = 0;
    return ReadSelf(kMsgBox, &v) && v != 0;   // 못 읽으면 닫힘
}

// 로비 소켓으로 나가는 보내기 하나 — 그 앞에 끼울 줄(없으면 빈 글)
static std::string RankPrefix(const char* buf, int len)
{
    if (!g_ourLobby) return std::string();
    const std::string nick = NickNow();
    const std::string own = "JOINGAME #" + nick + "'s_game ";
    if (!nick.empty() && len >= (int)own.size() && memcmp(buf, own.data(), own.size()) == 0) {
        const bool armed = g_rankMarkTest || InterlockedExchange(&g_rankArmed, 0) != 0;
        if (!armed) return std::string();
        InterlockedExchange(&g_rankWaiting, 1);
        const LONG n = InterlockedIncrement(&g_rankMarked);
        Log("⑪ 랭크 참가(누적 %ld) — 내 방 참가 줄 앞에 NOXRANK JOIN · 기다림 켬(알림창 %s)", n, MsgBoxOpen() ? "떠 있음" : "없음");
        return kRankJoinLine;
    }
    if (g_rankWaiting && !MsgBoxOpen() && InterlockedExchange(&g_rankWaiting, 0)) {
        char p[64];
        Preview(buf, len < 40 ? len : 40, p, sizeof(p));
        const LONG n = InterlockedIncrement(&g_rankLeaves);
        Log("⑪ 기다림 창 닫힘(누적 %ld) — 다음 보내기(%s) 앞에 NOXRANK LEAVE · 기다림 끔", n, p);
        return kRankLeaveLine;
    }
    return std::string();
}

// ③ — OnTokenLine 이 부른다
static void RankOnToken()
{
    InterlockedExchange(&g_rankFoundState, 0);   // ★ ⑬ 잡혔다(다 수락 — 참가 답)
    if (!InterlockedExchange(&g_rankWaiting, 0)) return;
    const LONG n = InterlockedIncrement(&g_rankTokenEnds);
    Log("⑪ 랭크 잡힘(누적 %ld) — 참가 토큰 받음 · 기다림 끔", n);
}

// ④ 기다리는 동안 알림창 글을 「상대 찾는 중...」으로(10-30 사용자 「서버 접속 시도중이라기보다는 상대 찾는중이라고 하는게낫지않을까」).
//   정품 글(「서버 접속 시도중...」)은 그대로 두고 랭크 기다림 창만 — 일반 방 참가도 같은 창을 쓰기 때문
//   글 칸 = 알림창 자식 4004(OpenNox `gui_dialog.go` guiDialogTextID) · 바꾸기 = 그 칸 처리 함수(+0x178)에 0x4001(글 · -1)
//   정품 글 칸은 글을 베끼지 않고 포인터만 쥔다(OpenNox `widget_static_text.go:141`) → DLL 안의 고정 글을 넘긴다. 받기 훅(게임 주 스레드)에서만
// ★ 매칭 손보기 4(`Docs/MATCH_UX_REVIEW.md`) — 글 뒤에 지난 시간 「상대 찾는 중... 0:15」 · 1초마다 고친다(시각은 DLL 혼자 — 창이 뜬 때부터).
//   글 칸이 포인터만 쥐므로 DLL 안 버퍼를 제자리에서 고치고 같은 0x4001 을 다시 보낸다(글 칸이 길이 · 줄을 미리 셈해 두는지 몰라 — 다시 보내면 그 셈도 새로).
//   받기 훅은 기다리는 동안 초당 약 30번 돈다(10-30 사람 판 DLL 기록 — WOLAPI recv 10초 297회 · 글 바꿈이 참가 줄과 같은 1/1000초)
static const DWORD kDlgTextId = 4004;
static const DWORD kMsgSetText = 0x4001;
static wchar_t g_rankWaitBuf[48];          // 정품 글 칸이 쥐는 글(게임 주 스레드만 씀)
static volatile LONG g_rankTextSet = 0;    // ④ 글 바꿈(창마다 처음 한 번)
static volatile LONG g_rankTextMiss = 0;   // ④ 글 칸 · 처리 함수를 못 찾음
static volatile LONG g_rankTextTicks = 0;  // ★ ④ 지난 시간 고침(초가 바뀔 때마다)

static void RankWaitText()
{
    static DWORD done = 0;     // 글을 바꾼 알림창(기다림마다 한 번)
    static DWORD text = 0, proc = 0, since = 0, shownSec = 0;
    if (!g_rankWaiting) {
        if (text) Log("⑪ 기다림 끝 — 지난 시간 %lu초 · 글 고침 누적 %ld번", (GetTickCount() - since) / 1000, g_rankTextTicks);
        done = 0;
        text = 0;
        return;
    }
    DWORD box = 0;
    if (!ReadSelf(kMsgBox, &box) || !box) return;
    const DWORD now = GetTickCount();
    if (box != done) {
        done = box;
        text = ((PFN_findchild)kFindChild)(box, kDlgTextId);
        proc = 0;
        if (!text || !ReadSelf(text + kProcSlot, &proc) || !proc) {
            text = 0;
            InterlockedIncrement(&g_rankTextMiss);
            Log("⑪ 기다림 글 — 알림창 %#lx 의 글 칸(4004) · 처리 함수를 못 찾아 그대로", box);
            return;
        }
        since = now;
        shownSec = 0;
        swprintf(g_rankWaitBuf, sizeof(g_rankWaitBuf) / sizeof(g_rankWaitBuf[0]), L"상대 찾는 중... 0:00");
        ((PFN_wndproc4)proc)(text, kMsgSetText, (DWORD)(uintptr_t)g_rankWaitBuf, (DWORD)-1);
        const LONG n = InterlockedIncrement(&g_rankTextSet);
        Log("⑪ 기다림 글(누적 %ld) — 알림창 %#lx 글을 「상대 찾는 중... 0:00」으로(1초마다 지난 시간)", n, box);
        return;
    }
    if (!text) return;   // 이 창은 글 칸을 못 찾았다
    if (g_rankFoundState == 1) {   // ★ 10-31 — 찾음 창(같은 알림창 · 같은 글 칸) 남은 초. 0 이 되면 로비의 EXPIRED 가 창을 닫는다
        const DWORD gone = (now - g_rankFoundSince) / 1000, lim = (DWORD)g_rankFoundSec;
        const DWORD left = gone >= lim ? 0 : lim - gone;
        if (left == g_rankFoundShownLeft) return;
        g_rankFoundShownLeft = left;
        RankFoundText(left);
        ((PFN_wndproc4)proc)(text, kMsgSetText, (DWORD)(uintptr_t)g_rankFoundBuf, (DWORD)-1);
        InterlockedIncrement(&g_rankFoundTicks);
        return;
    }
    if (g_rankFoundState) return;   // ★ ⑬ 찾음 · 수락 기다림 — 창 글이 그 문구(지난 시간은 계속 셈 — WAIT 뒤 이어서)
    const DWORD sec = (now - since) / 1000;
    if (sec == shownSec) return;
    shownSec = sec;
    swprintf(g_rankWaitBuf, sizeof(g_rankWaitBuf) / sizeof(g_rankWaitBuf[0]), L"상대 찾는 중... %lu:%02lu", sec / 60, sec % 60);
    ((PFN_wndproc4)proc)(text, kMsgSetText, (DWORD)(uintptr_t)g_rankWaitBuf, (DWORD)-1);
    InterlockedIncrement(&g_rankTextTicks);
}

// ─────────────────────────────────────────────── ⑫ 메뉴바 — 필터 창 「완료」 · 「리스트」 칸 숨김 (매칭 손보기 8 · Docs/MATCH_UX_REVIEW.md)
// 정품은 「필터」를 누르면 필터 화면으로 가며 「리스트 갱신」 · 「랭크 매칭」(10006~10007)을 끄고, 목록으로 돌아오는 길은 「리스트」(10004)뿐이다.
//   ① 필터 창(`[0x699024]` · 10022 — 게임 목록 창의 자식: `0x43778c` 이 `0x49c2c0([0x699018])` · 그 안에서 `filter.wnd` + 처리 함수 `0x49c5b0`)을 DLL 함수로 감싼다.
//      「완료」(10090 — 창 파일은 tools/ui_apply.ps1)를 누르면 게임 목록 창의 원래 처리 함수에 「리스트」 누름을 넣는다 — 정품 「리스트」 길 그대로
//      (`0x439f69` — `[0x69905c]` 0 · `0x437f60` 목록 보임 · 필터 숨김 · 10006~10007 켬 · `0x437470` 목록 다시 받기 · 누름 소리 `0x439aa7`.
//      OpenNox 이름 기준 `[0x69905c]` = 만들기 · 참가 표시, 필터 숨길 때 부르는 `0x49bfb0` = 필터 칸 값 저장 `sub_489870`).
//      그 뒤 두 라디오(묶음 3 — 리스트 · 필터)의 눌림 비트(창 +0x24 의 4)를 정품 라디오 누름처럼 바꾼다(`0x4a78b6` 눌려 있으면 알림 없음 ·
//      `0x4a78f2` 같은 묶음 지움 · `0x4a7903` 제 것 켬) — 안 바꾸면 「필터」가 눌린 채라 다시 눌러도 필터 화면으로 안 감
//   ② 「리스트」 칸을 숨긴다(「완료」가 생겨 할 일이 없음 — 셋째 칸을 나중 버튼 자리로). Game.exe 엔 10004 를 번호로 찾는 곳이 없어(32비트 값 0건)
//      정품이 다시 보이게 할 길은 창을 새로 만들 때뿐으로 보지만, 받기 때마다 보고 보이면 다시 숨긴다(셈 · 기록은 창마다 처음 한 번)
// 우리 로비 · 인터넷 모드에서만(⑩ 과 같은 자리 — 받기 훅 · 게임 주 스레드). 「완료」는 DLL 없는 클라 · LAN 에선 아무 일도 안 함(정품 함수 기본 갈래)
static const uintptr_t kFilterWin = 0x699024;      // 필터 창(없으면 0)
static const uintptr_t kFilterProc = 0x49c5b0;     // 그 창의 원래 처리 함수(0x17 · 0x4007 — 10015~10028 갈래 표)
static const DWORD kBtnList = 10004, kBtnFilter = 10005, kBtnDone = 10090;
static const DWORD kRadioOn = 4;                   // 창 +0x24 — 라디오 눌림
static volatile LONG g_menuFilterWrapped = 0;      // 감싼 필터 창
static volatile LONG g_menuFilterSkip = 0;         // 처리 함수 칸이 원래 함수가 아니라 안 감싼 필터 창
static volatile LONG g_menuDone = 0;               // 「완료」 → 「리스트」 길
static volatile LONG g_menuDoneMiss = 0;           // 「완료」인데 게임 목록 창 · 「리스트」 버튼을 못 찾음(아무것도 안 함)
static volatile LONG g_menuListHidden = 0;         // 「리스트」 칸 숨김(보여서 숨긴 횟수)
static volatile LONG g_menuListMiss = 0;           // 게임 목록 창에서 「리스트」 버튼을 못 찾음(창마다 한 번)

static int __cdecl RankFilterProc(DWORD win, DWORD msg, DWORD a3, DWORD a4)
{
    const PFN_wndproc4 orig = (PFN_wndproc4)kFilterProc;
    DWORD id = 0;
    if (msg != kMsgClick || !a3 || !ReadSelf(a3, &id) || id != kBtnDone) return orig(win, msg, a3, a4);
    const DWORD world = *(volatile DWORD*)kListWin;
    const DWORD list = world ? ((PFN_findchild)kFindChild)(world, kBtnList) : 0;
    if (!list) {
        InterlockedIncrement(&g_menuDoneMiss);
        Log("⑫ 「완료」 누름 — 게임 목록 창 %#lx 의 「리스트」(10004)를 못 찾아 아무것도 안 함", world);
        return 0;
    }
    ((PFN_wndproc4)kListProc)(world, kMsgClick, list, a4);
    const DWORD filter = ((PFN_findchild)kFindChild)(world, kBtnFilter);
    *(volatile DWORD*)(list + 0x24) |= kRadioOn;
    if (filter) *(volatile DWORD*)(filter + 0x24) &= ~kRadioOn;
    const LONG n = InterlockedIncrement(&g_menuDone);
    Log("⑫ 「완료」 누름(누적 %ld) — 필터 창 %#lx → 「리스트」 누름을 게임 목록 창에 넣음 · 라디오 리스트 켬 · 필터 %s", n, win, filter ? "끔" : "못 찾음");
    return 0;
}

// 받기 훅(⑩ 자리) — 게임 목록 창이 있을 때마다
static void RankMenuTick(DWORD world)
{
    static DWORD loggedWorld = 0;   // 「리스트」 숨김 · 못 찾음 기록은 창마다 처음 한 번
    const DWORD list = ((PFN_findchild)kFindChild)(world, kBtnList);
    DWORD flags = 0;
    if (!list) {
        if (loggedWorld != world) {
            loggedWorld = world;
            InterlockedIncrement(&g_menuListMiss);
            Log("⑫ 게임 목록 창 %#lx — 「리스트」(10004)를 못 찾음 · 그대로", world);
        }
    } else if (ReadSelf(list + 4, &flags) && !(flags & kWinHidden)) {
        ((PFN_sethidden)kSetHidden)(list, 1);
        const LONG n = InterlockedIncrement(&g_menuListHidden);
        if (loggedWorld != world) {
            loggedWorld = world;
            Log("⑫ 게임 목록 창 %#lx — 「리스트」(10004 창 %#lx) 숨김(누적 %ld) · 그때 깃발 %#lx", world, list, n, flags);
        }
    }
    DWORD filter = 0, proc = 0;
    if (!ReadSelf(kFilterWin, &filter) || !filter || !ReadSelf(filter + kProcSlot, &proc) || proc == (DWORD)(uintptr_t)RankFilterProc) return;
    if (proc != kFilterProc) {
        static DWORD skipped = 0;   // 같은 창엔 한 번만 찍는다
        if (skipped != filter) {
            skipped = filter;
            InterlockedIncrement(&g_menuFilterSkip);
            Log("⑫ 필터 창 %#lx — 처리 함수 칸이 %#lx(원래 %#lx 아님) · 안 감쌈", filter, proc, (DWORD)kFilterProc);
        }
        return;
    }
    *(volatile DWORD*)(filter + kProcSlot) = (DWORD)(uintptr_t)RankFilterProc;
    const DWORD done = ((PFN_findchild)kFindChild)(filter, kBtnDone);
    const LONG n = InterlockedIncrement(&g_menuFilterWrapped);
    Log("⑫ 필터 창 %#lx 감쌈(누적 %ld) — 「완료」(10090) %s", filter, n, done ? "있음" : "없음(창 파일 안 고침 — tools/ui_apply.ps1)");
}

// ─────────────────────────────────────────────── ⑬ 수락 단계 (매칭 손보기 6 · Docs/MATCH_UX_REVIEW.md · 판 5)
// 로비가 묶음의 방을 세우면 참가 답 대신 `NOXRANK FOUND <초>` → 기다림 창(같은 창)을 「매칭 성공! 수락하시겠습니까?」 예 · 아니오로 바꾼다.
//   ★ 10-31 — 글 뒤에 「남은 시간 N초 (시간이 끝나면 자동 거절)」 · 1초마다 ④ 자리(RankWaitText)가 고침(시각은 DLL — 창을 띄운 때부터 · 로비 한도와 왕복 시간만큼 차이)
//   - 왜: 고정 글만으론 한도가 있다는 것도, 안 누르면 거절로 처리된다는 것도 안 보였음(10-31 사람 판)
//   창 바꾸기 = 정품 알림창 열기 `0x449590(부모, 제목, 글, 깃발, 0, 0)` — 창이 이미 있으면 그 창의 깃발(`0x449a20`) · 글만 바꾸고 둔다(`0x4495d9`~`0x44960f`).
//   깃발 예 0x8 · 아니오 0x10 · 취소 0x2 · 누르면 닫힘 0x20(OpenNox `client/gui/dialog.go`). 예 · 아니오 함수 칸은 0 — 정품이 다음에 이 함수를 부르면
//   앞 「아니오 함수」를 불러 버린다(`0x4495a9`). 대신 누름은 창이 부모(게임 목록 창 — ⑩ 이 감쌈)에 넘기므로 거기서 4007 · 4006 을 가로챈다
//   · 예 → 로비 소켓에 `NOXRANK ACCEPT` 를 곧바로(게임 주 스레드 · 줄 하나 send 하나 — 정품 로비 모듈은 줄마다 send 한 번(10-30 기록) ·
//     그 send 가 어느 스레드인지는 미확인 — 받기는 주 스레드(④ 근거)) ·
//     창을 「상대 수락 기다리는 중...」 취소(0x22 — 처음 기다림 창 깃발)로(★ 10-31 다음 받기 훅에서). 그 뒤 취소면 ⑩ 이 그 자리 LEAVE → 로비는 거절로
//   · 아니오 · 로비 `NOXRANK EXPIRED` → 그 자리 LEAVE(★ 10-31 RankLeaveNow) + 창의 취소(4002) 누름을 정품 목록 함수에 넣는다 — 정품 취소 길(창 닫음 · 로비엔 줄 없음)
//   · 로비 `NOXRANK WAIT`(남이 거절 · 한도 — 줄 맨 앞에 되돌림) → 창을 다시 기다림 글(④ 버퍼 · 지난 시간 이어서) 취소로
//   · ★ 10-31 로비 `NOXRANK READY`(다 수락 — 로비가 그때 방을 요청) → 창을 「방 만드는 중...」 취소로. 방이 서면 참가 답(③ 토큰) ·
//     방 실패 · 시간 초과 · 남이 빠지면 WAIT. 여기서 취소면 ⑩ 이 그 자리 LEAVE → 로비가 요청을 접고 남은 사람을 줄 앞에
//   받은 줄은 받기 훅 안에서 표시만 세우고, 창은 ⑩ 자리(받기 훅 · 게임 주 스레드)에서 바꾼다. 찾음 · 수락 기다림 동안엔 ④ 의 1초 고침을 멈춘다
static const uintptr_t kDlgOpen = 0x449590;
static const DWORD kDlgYesNo = 0x18, kDlgWaitFlags = 0x22, kDlgCancelId = 4002;
static const wchar_t kRankAcceptedText[] = L"상대 수락 기다리는 중...";
static const wchar_t kRankReadyText[] = L"방 만드는 중...";   // ★ 10-31 — 로비 `NOXRANK READY`(다 수락 → 그때 방 요청)
static volatile LONG g_rankEvFound = 0, g_rankEvWait = 0, g_rankEvExpired = 0, g_rankEvReady = 0;   // 받은 줄 → ⑩ 자리에서 처리
static volatile LONG g_rankReadys = 0;        // ★ 10-31 — READY → 창을 「방 만드는 중...」으로
static volatile LONG g_rankFoundShown = 0;    // 창을 예 · 아니오로 바꿈
static volatile LONG g_rankAccepts = 0;       // 예 → ACCEPT 보냄
static volatile LONG g_rankAcceptFail = 0;    // ACCEPT 보내기 실패(로비 소켓 없음 · send 오류)
static volatile LONG g_rankDeclines = 0;      // 아니오 → 취소 길
static volatile LONG g_rankWaits = 0;         // WAIT → 기다림 창으로
static volatile LONG g_rankExpiries = 0;      // EXPIRED → 취소 길
static volatile LONG g_rankFoundMiss = 0;     // 창 · 취소 단추를 못 찾음(아무것도 안 함)
static volatile LONG g_rankLeaveNowSent = 0;  // ★ 10-31 — 그 자리 LEAVE 보냄(⑪ 「창 닫힘 LEAVE」 셈에도 듦)
static volatile LONG g_rankLeaveNowFail = 0;  // ★ 10-31 — 그 자리 LEAVE 보내기 실패(기다림 그대로 — ⑪ 다음 보내기 앞 LEAVE 로 넘김)
static volatile LONG g_rankEvAccepted = 0;    // ★ 10-31 — 예 → 창을 「상대 수락 기다리는 중...」으로(다음 받기 훅에서)
static SOCKET g_rankSock = INVALID_SOCKET;    // 랭크 참가 줄이 나간 로비 소켓 · FOUND 가 온 소켓(ACCEPT · LEAVE 를 보낼 곳)
typedef int(__cdecl* PFN_dlgopen)(DWORD parent, const wchar_t* title, const wchar_t* text, DWORD flags, DWORD yes, DWORD no);

static void RankOnLobbyLine(SOCKET s, const std::string& line)
{
    const std::string sub = line.substr(strlen(LobbyLineFilter::kRankMark));
    if (sub.compare(0, 5, "FOUND") == 0) {
        const long sec = sub.size() > 6 ? strtol(sub.c_str() + 6, NULL, 10) : 0;   // `FOUND <초>` — 이상하면 10 그대로
        if (sec > 0 && sec <= 120) InterlockedExchange(&g_rankFoundSec, sec);
        g_rankSock = s;
        InterlockedExchange(&g_rankEvFound, 1);
    }
    else if (sub == "WAIT") InterlockedExchange(&g_rankEvWait, 1);
    else if (sub == "READY") InterlockedExchange(&g_rankEvReady, 1);
    else if (sub == "EXPIRED") InterlockedExchange(&g_rankEvExpired, 1);
    Log("⑬ 로비 줄 「%s」 — 받음(창은 다음 받기 훅에서)", line.c_str());
}

// ★ 10-31 — ⑪ 기다리는 중이면 로비 소켓에 `NOXRANK LEAVE` 를 곧바로(ACCEPT 와 같은 자리 · 게임 주 스레드 · 줄 하나 send 하나).
//   왜 — 정품 취소 길(창의 취소 · 아래 RankInjectCancel)은 로비에 아무 줄도 안 보낸다(10-31 사람 판 두 번 — 취소 · 「아니오」 뒤 LIST · PART 없음).
//   ⑪ 의 「다음 보내기 앞 LEAVE」만으론 로비가 다음 랭크 누름(그때의 LIST)까지 모르고, 「아니오」는 거절이 아니라 한도로 끝났다.
//   로비는 LEAVE 를 받으면 묶여 있던 참가에 474 를 보내 정품 로비 모듈의 참가 기다림을 푼다(`Lobby.h` `kRankReleaseNumeric`)
static void RankLeaveNow(const char* why)
{
    if (!InterlockedExchange(&g_rankWaiting, 0)) return;
    InterlockedExchange(&g_rankFoundState, 0);
    typedef int(WSAAPI* PFN_send_t)(SOCKET, const char*, int, int);
    const int len = (int)strlen(kRankLeaveLine);
    const int rc = g_rankSock == INVALID_SOCKET ? -1 : ((PFN_send_t)Resolve(19))(g_rankSock, kRankLeaveLine, len, 0);
    if (rc != len) {
        InterlockedExchange(&g_rankWaiting, 1);   // 창이 닫히면 ⑪ 이 다음 보내기 앞에 싣는다
        InterlockedIncrement(&g_rankLeaveNowFail);
        Log("⑬ %s — LEAVE 곧바로 보내기 실패 rc=%d err=%lu(로비 소켓 %u) · 다음 보내기 앞으로 넘김", why, rc, GetLastError(), (unsigned)g_rankSock);
        return;
    }
    InterlockedIncrement(&g_rankLeaves);
    const LONG n = InterlockedIncrement(&g_rankLeaveNowSent);
    Log("⑬ %s — 로비에 NOXRANK LEAVE 곧바로(누적 %ld) · 기다림 끔", why, n);
}

// 창의 취소(4002) 누름을 정품 목록 함수에 — 정품 취소 길(로비엔 아무 줄도 안 감 — 그래서 먼저 LEAVE)
static bool RankInjectCancel(const char* why)
{
    RankLeaveNow(why);
    DWORD box = 0;
    const DWORD world = *(volatile DWORD*)kListWin;
    const DWORD cancel = ReadSelf(kMsgBox, &box) && box ? ((PFN_findchild)kFindChild)(box, kDlgCancelId) : 0;
    if (!world || !cancel) {
        InterlockedIncrement(&g_rankFoundMiss);
        Log("⑬ %s — 게임 목록 창 %#lx · 알림창 %#lx 의 취소(4002)를 못 찾아 아무것도 안 함", why, world, box);
        return false;
    }
    ((PFN_wndproc4)kListProc)(world, kMsgClick, cancel, 0);
    Log("⑬ %s — 알림창 %#lx 의 취소 누름을 정품 목록 함수에 넣음(창 닫음)", why, box);
    return true;
}

static void RankDlg(const wchar_t* text, DWORD flags)
{
    const DWORD world = *(volatile DWORD*)kListWin;
    ((PFN_dlgopen)kDlgOpen)(world, NULL, text, flags, 0, 0);
}

// ⑩ 자리(받기 훅 · 게임 주 스레드) — 받은 줄을 창에
static void RankFoundTick()
{
    DWORD box = 0;
    const bool open = ReadSelf(kMsgBox, &box) && box;
    if (InterlockedExchange(&g_rankEvFound, 0)) {
        if (!open || !g_rankWaiting) {
            InterlockedIncrement(&g_rankFoundMiss);
            Log("⑬ FOUND — 기다림 창이 없음(알림창 %#lx · 기다림 %ld) · 그대로", box, g_rankWaiting);
        } else {
            g_rankFoundSince = GetTickCount();
            g_rankFoundShownLeft = (DWORD)g_rankFoundSec;
            RankFoundText(g_rankFoundShownLeft);   // ★ 10-31 — 남은 초 · 자동 거절 안내(1초마다 ④ 자리가 고침)
            InterlockedExchange(&g_rankFoundState, 1);
            RankDlg(g_rankFoundBuf, kDlgYesNo);
            const LONG n = InterlockedIncrement(&g_rankFoundShown);
            Log("⑬ 찾음(누적 %ld) — 알림창 %#lx 를 「매칭 성공! 수락하시겠습니까? 남은 시간 %ld초」 예 · 아니오로", n, box, g_rankFoundSec);
        }
    }
    if (InterlockedExchange(&g_rankEvAccepted, 0)) {   // ★ 10-31 — 예 누름 처리가 끝난 뒤(아래 RankFoundClick 주석)
        if (open && g_rankFoundState == 2) {
            RankDlg(kRankAcceptedText, kDlgWaitFlags);
            Log("⑬ 예 뒤 — 알림창 %#lx 를 「상대 수락 기다리는 중...」 취소로", box);
        } else {
            InterlockedIncrement(&g_rankFoundMiss);
            Log("⑬ 예 뒤 — 창을 못 바꿈(알림창 %#lx · 찾음 상태 %ld)", box, g_rankFoundState);
        }
    }
    if (InterlockedExchange(&g_rankEvReady, 0)) {   // ★ 10-31 — 다 수락 → 방 만드는 중(예 뒤 창 바꾸기보다 뒤 — 같은 훅이면 이 글이 남는다)
        if (open && g_rankWaiting && g_rankFoundState == 2) {
            RankDlg(kRankReadyText, kDlgWaitFlags);
            const LONG n = InterlockedIncrement(&g_rankReadys);
            Log("⑬ READY(누적 %ld) — 알림창 %#lx 를 「방 만드는 중...」 취소로", n, box);
        } else {
            Log("⑬ READY — 창을 안 바꿈(알림창 %#lx · 기다림 %ld · 찾음 상태 %ld — 참가 답이 먼저 왔으면 정상)", box, g_rankWaiting, g_rankFoundState);
        }
    }    if (InterlockedExchange(&g_rankEvWait, 0)) {
        InterlockedExchange(&g_rankFoundState, 0);
        InterlockedIncrement(&g_rankWaits);
        if (open) RankDlg(g_rankWaitBuf, kDlgWaitFlags);
        Log("⑬ WAIT — 다시 기다림(알림창 %#lx · 글은 ④ 버퍼 · 취소 단추)", box);
    }
    if (InterlockedExchange(&g_rankEvExpired, 0)) {
        InterlockedExchange(&g_rankFoundState, 0);
        InterlockedIncrement(&g_rankExpiries);
        if (open) RankInjectCancel("EXPIRED(수락 한도)");
    }
}

static bool RankFoundClick(DWORD id)
{
    if (g_rankFoundState != 1) return false;
    if (id == 4007) {   // 예
        static const char kAccept[] = "NOXRANK ACCEPT\r\n";
        typedef int(WSAAPI* PFN_send_t)(SOCKET, const char*, int, int);
        const int rc = g_rankSock == INVALID_SOCKET ? -1 : ((PFN_send_t)Resolve(19))(g_rankSock, kAccept, (int)strlen(kAccept), 0);
        InterlockedExchange(&g_rankFoundState, 2);
        // ★ 10-31 — 창 바꾸기는 다음 받기 훅에서. 이 함수는 정품 알림창의 누름 처리 한가운데서 불린다 — 알림창은 부모에 누름을 넘긴 뒤
        //   깃발을 다시 읽어 「누르면 닫힘」(0x20)이면 창을 닫는데, 그 앞의 쌓기 빼기는 옛 깃발(0x18)로 건너뛴다(OpenNox `gui_dialog.go:248-259`).
        //   여기서 0x22 로 바꾸면 창만 닫히고 쌓기는 남아 화면이 굳었다(10-31 사람 판 — 「불러오는 중...」 · 옵션 · 뒤로가 안 먹음 · WAIT 때 알림창 0)
        InterlockedExchange(&g_rankEvAccepted, 1);
        if (rc != (int)strlen(kAccept)) {
            InterlockedIncrement(&g_rankAcceptFail);
            Log("⑬ 예 — ACCEPT 보내기 실패 rc=%d err=%lu(로비 소켓 %u)", rc, GetLastError(), (unsigned)g_rankSock);   // 이 DLL 은 ws2_32 를 안 링크 — WSAGetLastError 대신
        } else {
            const LONG n = InterlockedIncrement(&g_rankAccepts);
            Log("⑬ 예(누적 %ld) — 로비에 NOXRANK ACCEPT · 창을 「상대 수락 기다리는 중...」 취소로", n);
        }
        return true;
    }
    InterlockedExchange(&g_rankFoundState, 0);   // 아니오
    InterlockedIncrement(&g_rankDeclines);
    RankInjectCancel("아니오(거절)");
    return true;
}

// 1초마다(요약 스레드) — ⑩ · ⑪ 셈 중 하나라도 바뀌었으면 한 줄. 누름 갈래의 합 = 감싼 창에 온 누름 전부 · 표시 = 창 닫힘 + 잡힘 + 기다림
static void RankSummary()
{
    static LONG seen[15] = {};
    const LONG now[15] = { g_rankWrapped, g_rankWrapSkip, g_rankClicks, g_rankNotOurs, g_rankNoHost, g_rankHostClicks, g_rankOtherClicks, g_rankInnerClicks,
                           g_rankArmed, g_rankMarked, g_rankLeaves, g_rankTokenEnds, g_rankWaiting, g_rankTextSet, g_rankTextMiss };
    if (memcmp(seen, now, sizeof(now)) == 0) return;
    memcpy(seen, now, sizeof(now));
    Log("⑩ 랭크 매칭 셈 — 감싼 창 %ld · 안 감쌈 %ld | 누름: 랭크 %ld · 우리 로비 아님 %ld · 10002 못 찾음 %ld · 게임 호스트하기 %ld · 그 밖 %ld(그중 호스트 안 %ld) | 랭크 표시 %ld"
        " || ⑪ 참가 표시 %ld = 창 닫힘 LEAVE %ld + 잡힘 %ld + 기다리는 중 %ld | 기다림 글 바꿈 %ld · 못 찾음 %ld",
        now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12], now[13], now[14]);
}

// ★ ⑫ — 메뉴바 셈(따로 한 줄 — 바뀌었을 때만)
static void MenuSummary()
{
    static LONG seen[6] = {};
    const LONG now[6] = { g_menuFilterWrapped, g_menuFilterSkip, g_menuDone, g_menuDoneMiss, g_menuListHidden, g_menuListMiss };
    if (memcmp(seen, now, sizeof(now)) == 0) return;
    memcpy(seen, now, sizeof(now));
    Log("⑫ 메뉴바 셈 — 감싼 필터 창 %ld · 안 감쌈 %ld | 「완료」 → 리스트 %ld · 못 찾음 %ld | 「리스트」 숨김 %ld · 못 찾음 %ld",
        now[0], now[1], now[2], now[3], now[4], now[5]);
}

// ★ ⑬ — 수락 단계 셈(따로 한 줄 — 바뀌었을 때만). MenuSummary 와 같은 자리에서 부른다
static void AcceptSummary()
{
    static LONG seen[11] = {};
    const LONG now[11] = { g_rankFoundShown, g_rankAccepts, g_rankAcceptFail, g_rankDeclines, g_rankWaits, g_rankExpiries, g_rankFoundMiss,
                          g_rankLeaveNowSent, g_rankLeaveNowFail, g_rankFoundTicks, g_rankReadys };
    if (memcmp(seen, now, sizeof(now)) == 0) return;
    memcpy(seen, now, sizeof(now));
    Log("⑬ 수락 셈 — 찾음 창 %ld · 예(ACCEPT) %ld · 보내기 실패 %ld · 아니오 %ld · WAIT %ld · EXPIRED %ld · 못 찾음 %ld | 그 자리 LEAVE %ld · 실패 %ld | 남은 초 고침 %ld · READY %ld",
        now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10]);
}

// ─────────────────────────────────────────────── ⑭ 랭크 점수 알림창 꾸밈 (10-31 · 사용자가 고른 시안 3)
// 게임 서버가 판 뒤 `0xa8` 깃발 0x10(알림창 길 — 서버 `Server::SendNoticeBoxTo`)으로 「랭크 상승! 1000 -> 1010 (+10)」을 보내면 정품이 공용 알림창을 띄운다
//   (`Game.exe` 0x471e9f → 0x449590(0, 「Notice」, 글, 0x21) · 창 `[0x69cba4]` · 제목 4005 · 글 4004 · OK 4001 — `window\dlg.wnd`, 글꼴이 크면 `ldlg.wnd`(0x449624)).
// 창이 뜬 뒤 다음 받기 · 보내기 훅(게임 주 스레드)에서 그 창을 시안 3 꼴로 바꾼다:
//   · 창 300x150 을 화면 가운데로 — 정품 띄움과 같은 셈(0x44970e — 화면 너비 [0x95410c] · 높이 [0x954764] · 자리 함수 0x4882c0 이 끝 칸도 고침)
//   · 제목 칸 = 첫 줄 「랭크 상승!」(오름 초록 · 내림 빨강 · 같음 금색) · 글 칸 = 둘째 줄 「1000 -> 1010 (+10)」 흰색 · OK 는 가운데 아래
//   · 큰 글꼴 = 정품 0x449ee0(1)(제목 · 글 칸 +0xec 에 「large」 — 정품이 쓰는 큰 알림창 갈래)
//   - 색 칸 +0x68 은 OpenNox `WindowData.TextColorVal` 자리 — GOG 판에서 정적 확인을 못 해, 쓰기 전에 지금 값이 dlg.wnd 의 글 금색(240 180 42) ·
//     제목 초록(0 200 0)을 정품 색 함수 0x435e10 으로 지은 값과 같은지 본다. 다르면 색은 안 건드리고 셈 · 값을 기록에(다음 판에서 자리를 고칠 근거)
//   - 글은 칸 +0x20 → 글 자료 +0 의 넓은 글 포인터(정품은 0x4001 로 포인터만 쥠 — OpenNox `StaticTextSetText`) — 우리 버퍼(프로세스 끝까지)를 0x4001 로 넣는다
//   - 한 번 꾸민 창은 글 포인터가 우리 버퍼라 다시 안 꾸민다 · 「랭크 」로 시작하지 않는 글(수락 창 등)은 손 안 댐
//   - 왜: 정품 알림창은 150x155 · 작은 글꼴 한 줄이라 판 뒤 점수 변화가 잘 안 보임(10-31 사람 판 「글자 크게 · 창 크기」 → 시안 3)
static const DWORD kDlgTitleId = 4005, kDlgOkId = 4001;   // 글 칸 kDlgTextId(4004) · 글 넣기 kMsgSetText(0x4001)는 ⑪ ④ 것
static const uintptr_t kWinSetPos = 0x4882c0, kDlgBigFont = 0x449ee0, kColorRgb = 0x435e10, kWinCall94 = 0x488da0;
static const uintptr_t kScreenW = 0x95410c, kScreenH = 0x954764;
static const int kNbW = 300, kNbH = 150, kNbLineH = 24;
static wchar_t g_nbLine1[32], g_nbLine2[48];          // 꾸민 창이 가리키는 글(프로세스 끝까지)
static DWORD g_nbBadText = 0;                         // 숫자가 없어 못 나눈 글 포인터(같은 창에 한 번만 셈)
static volatile LONG g_nbDone = 0, g_nbMiss = 0, g_nbColorSkip = 0, g_nbBad = 0;
static volatile LONG g_nbRan = 0;                     // 주 스레드에서 한 번이라도 돌았나(0 이면 이 훅이 주 스레드에서 안 불림)
typedef int(__cdecl* PFN_setpos)(DWORD win, int x, int y);
typedef void(__cdecl* PFN_dlgfont)(int mode);
typedef DWORD(__cdecl* PFN_rgb)(int r, int g, int b);

static void NbPlace(DWORD win, int w, int h, int x, int y)
{
    *(volatile int*)(win + 8) = w;
    *(volatile int*)(win + 0xc) = h;
    ((PFN_setpos)kWinSetPos)(win, x, y);
}

static void NoticeBoxTick(const char* where)
{
    if (!g_rankOn || GetCurrentThreadId() != g_mainTid) return;
    if (!InterlockedExchange(&g_nbRan, 1)) Log("⑭ 점수 알림창 꾸밈 — 주 스레드 %s 훅에서 처음 돎", where);
    DWORD box = 0;
    if (!ReadSelf(kMsgBox, &box) || !box) return;
    const DWORD text = ((PFN_findchild)kFindChild)(box, kDlgTextId);
    DWORD wd = 0, tp = 0;
    if (!text || !ReadSelf(text + 0x20, &wd) || !wd || !ReadSelf(wd, &tp) || !tp) return;
    if (tp == (DWORD)(uintptr_t)g_nbLine2 || tp == g_nbBadText) return;   // 이미 꾸밈 · 이미 못 나눔
    wchar_t s[64] = {};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)tp, s, sizeof(s) - sizeof(wchar_t), &got)) return;
    if (wcsncmp(s, L"랭크 ", 3) != 0) return;   // 우리 알림이 아님
    const wchar_t* digit = s;
    while (*digit && !(*digit >= L'0' && *digit <= L'9')) ++digit;
    const DWORD title = ((PFN_findchild)kFindChild)(box, kDlgTitleId);
    const DWORD ok = ((PFN_findchild)kFindChild)(box, kDlgOkId);
    if (!*digit || !title || !ok) {
        g_nbBadText = tp;
        InterlockedIncrement(!*digit ? &g_nbBad : &g_nbMiss);
        Log("⑭ 알림창 %#lx — %s · 손 안 댐", box, !*digit ? "글에 숫자가 없어 못 나눔" : "제목(4005) · OK(4001) 칸을 못 찾음");
        return;
    }
    size_t headLen = (size_t)(digit - s);
    while (headLen && s[headLen - 1] == L' ') --headLen;
    if (headLen >= _countof(g_nbLine1)) headLen = _countof(g_nbLine1) - 1;
    wmemcpy(g_nbLine1, s, headLen);
    g_nbLine1[headLen] = 0;
    wcsncpy(g_nbLine2, digit, _countof(g_nbLine2) - 1);
    g_nbLine2[_countof(g_nbLine2) - 1] = 0;
    const bool up = wcsstr(g_nbLine1, L"상승") != NULL, down = wcsstr(g_nbLine1, L"하락") != NULL;   // 셋째 = 같음(유지)

    const PFN_rgb rgb = (PFN_rgb)kColorRgb;
    const DWORD gold = rgb(240, 180, 42), green = rgb(0, 200, 0);
    DWORD textColor = 0, titleColor = 0;
    const bool colorOk = ReadSelf(text + 0x68, &textColor) && ReadSelf(title + 0x68, &titleColor) && textColor == gold && titleColor == green;

    ((PFN_dlgfont)kDlgBigFont)(1);   // 제목 · 글 칸 큰 글꼴(정품 갈래)
    DWORD okW = 55, okH = 20;
    ReadSelf(ok + 8, &okW);
    ReadSelf(ok + 0xc, &okH);
    const int scrW = *(volatile int*)kScreenW, scrH = *(volatile int*)kScreenH;
    NbPlace(box, kNbW, kNbH, (scrW - kNbW) / 2, (scrH - kNbH) / 2);
    NbPlace(title, kNbW, kNbLineH, 0, 30);
    NbPlace(text, kNbW, kNbLineH, 0, 64);
    ((PFN_setpos)kWinSetPos)(ok, (kNbW - (int)okW) / 2, kNbH - (int)okH - 12);
    ((PFN_wndproc4)kWinCall94)(title, kMsgSetText, (DWORD)(uintptr_t)g_nbLine1, (DWORD)-1);   // 0x488da0 = 칸 +0x178 처리 함수 부르기(정품 · ⑪ ④ 와 같은 길)
    ((PFN_wndproc4)kWinCall94)(text, kMsgSetText, (DWORD)(uintptr_t)g_nbLine2, (DWORD)-1);
    if (colorOk) {
        *(volatile DWORD*)(title + 0x68) = up ? rgb(60, 230, 60) : down ? rgb(235, 60, 50) : gold;
        *(volatile DWORD*)(text + 0x68) = rgb(242, 242, 242);
    } else {
        InterlockedIncrement(&g_nbColorSkip);
    }
    const LONG n = InterlockedIncrement(&g_nbDone);
    char l1[96] = {}, l2[144] = {};   // 기록 파일은 UTF-8(`%ls` 는 C 로캘에서 한글이 깨짐)
    WideCharToMultiByte(CP_UTF8, 0, g_nbLine1, -1, l1, sizeof(l1) - 1, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, g_nbLine2, -1, l2, sizeof(l2) - 1, NULL, NULL);
    Log("⑭ 알림창 %#lx 꾸밈(누적 %ld) — %dx%d 화면 %dx%d 가운데 · 「%s」(%s) / 「%s」 · 색 %s(글 칸 %#lx · 제목 칸 %#lx · 기대 금 %#lx · 초록 %#lx)", box, n, kNbW,
        kNbH, scrW, scrH, l1, up ? "오름" : down ? "내림" : "같음", l2, colorOk ? "바꿈" : "안 바꿈(자리 값이 기대와 다름)", textColor, titleColor, gold, green);
}

static void NoticeSummary()
{
    static LONG seen[5] = {};
    const LONG now[5] = { g_nbRan, g_nbDone, g_nbColorSkip, g_nbBad, g_nbMiss };
    if (memcmp(seen, now, sizeof(now)) == 0) return;
    memcpy(seen, now, sizeof(now));
    Log("⑭ 점수 알림창 셈 — 주 스레드에서 돎 %ld · 꾸밈 %ld(그중 색 안 바꿈 %ld) · 못 나눔 %ld · 칸 못 찾음 %ld", now[0], now[1], now[2], now[3], now[4]);
}

// 받기 훅에서 — 새 게임 목록 창이면 감싸고 10007 을 보이게
static void RankUiTick()
{
    if (GetCurrentThreadId() != g_mainTid || !g_ourLobby || *(volatile DWORD*)kNetMode != 1) return;
    RankWaitText();   // ★ ⑪ ④ 기다림 창 글
    RankFoundTick();  // ★ 매칭 손보기 6 — ⑬ 로비 수락 줄 → 창
    const DWORD win = *(volatile DWORD*)kListWin;
    if (!win) return;
    RankMenuTick(win);   // ★ 매칭 손보기 8 — ⑫ 필터 창 감싸기 · 「리스트」 칸 숨김
    DWORD proc = 0;
    if (!ReadSelf(win + kProcSlot, &proc) || proc == (DWORD)(uintptr_t)RankListProc) return;
    if (proc != kListProc) {
        static DWORD skipped = 0;   // 같은 창엔 한 번만 찍는다
        if (skipped != win) {
            skipped = win;
            InterlockedIncrement(&g_rankWrapSkip);
            Log("⑩ 게임 목록 창 %#lx — 처리 함수 칸이 %#lx(원래 %#lx 아님) · 안 감쌈", win, proc, (DWORD)kListProc);
        }
        return;
    }
    const DWORD rank = ((PFN_findchild)kFindChild)(win, kBtnRank);
    DWORD flags = 0;
    const bool read = rank && ReadSelf(rank + 4, &flags);
    if (read && (flags & kWinHidden)) ((PFN_sethidden)kSetHidden)(rank, 0);
    *(volatile DWORD*)(win + kProcSlot) = (DWORD)(uintptr_t)RankListProc;
    const LONG n = InterlockedIncrement(&g_rankWrapped);
    const char* what = !rank ? "못 찾음" : !read ? "깃발 못 읽음" : (flags & kWinHidden) ? "숨김이었음 → 보이게 함" : "이미 보임(정품 0x437948)";
    Log("⑩ 게임 목록 창 %#lx 감쌈(누적 %ld) — 10007 %s · 그때 깃발 %#lx", win, n, what, flags);
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
    if (name && namelen >= (int)sizeof(sockaddr_in) && name->sa_family == AF_INET) {   // ★ 조각 5 의 4 — 라우터 연결이면 whereto 암호 칸 지우기
        const unsigned char* pp = (const unsigned char*)&((const sockaddr_in*)name)->sin_port;
        if (((pp[0] << 8) | pp[1]) == kRouterPort) MarkRouter(s);
    }
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
    const bool lobbySock = buf && len > 0 && LobbyFilterOf(s);
    if ((g_listOn || g_hostGameOn || g_rankMarkTest) && lobbySock) NoteNick(buf, len);   // ⑤ · ⑥ · ⑪
    // ★ 랭크 매칭 조각 3 — ⑪ 이 보내기 앞에 끼울 표시 줄(랭크 참가 · 기다림 창 닫힘)
    const std::string rankPre = ((g_rankOn || g_rankMarkTest) && lobbySock) ? RankPrefix(buf, len) : std::string();
    if (rankPre == kRankJoinLine) g_rankSock = s;   // ★ 10-31 — ⑬ 의 그 자리 LEAVE 를 보낼 곳(찾음 전 취소에도)
    int rc;
    CLoginRewriter* lw = (buf && len > 0) ? LoginRewriterOf(s) : NULL;
    if (lw) {
        // ★ 로그인 암호 조각 5 의 3 · 4 — 로비 · 라우터 연결은 줄 단위로 거쳐 간다. `apgar` 암호 자리가 로그인 토큰으로(로비) · `whereto` 암호 칸이 x 로(라우터) 바뀌어 나가고(길이가 달라진다),
        //   게임에는 늘 원래 길이를 돌려준다. 막지 않는 소켓이 일부만 받으면 남은 몫을 쥐고 다음 보내기에 이어 보낸다(Flush)
        //   ★ 랭크 매칭 조각 3 — 끼울 줄도 같은 줄 상태에 먼저 넣는다(쥔 몫이 있으면 그 뒤로 — 차례가 안 바뀌게)
        if (!rankPre.empty()) lw->Feed(rankPre.data(), (int)rankPre.size());
        lw->Feed(buf, len);
        const int fr = lw->Flush([&](const char* b, int n) { return ((PFN_send)Resolve(19))(s, b, n, flags); },
                                 [] { return GetLastError() == WSAEWOULDBLOCK; });
        rc = fr < 0 ? SOCKET_ERROR : len;
    } else {
        if (!rankPre.empty()) ((PFN_send)Resolve(19))(s, rankPre.data(), (int)rankPre.size(), flags);   // 로비 소켓엔 늘 줄 상태가 있어 여기 안 옴(안전망)
        rc = ((PFN_send)Resolve(19))(s, buf, len, flags);
    }
    if (g_log[0]) {
        DWORD err = GetLastError();
        ModStat* m = ModOf(ret);
        Count(m, F_SEND, rc);
        const std::string shown = (buf && len > 0) ? RedactedForLog(s, buf, len) : std::string();   // ★ 조각 5 — 암호 칸 가림
        char p[512]; Preview(shown.data(), (int)shown.size(), p, 400);
        Detail("send s=%u len=%d rc=%d 부른 곳=%s | %s", (unsigned)s, len, rc, m->name, p);
        SetLastError(err);
    }
    return rc;
}

extern "C" int WSAAPI Hook_recv(SOCKET s, char* buf, int len, int flags)
{
    void* ret = _ReturnAddress();
    if (g_testGuard) ChanGuardTest();   // ★ 10-21 ⑨ — 받기 전에(넘길 버퍼가 아직 없을 때)
    if (g_rankOn) RankUiTick();         // ★ 랭크 매칭 조각 2 — ⑩ 새 게임 목록 창 감싸기(게임 주 스레드에서만)
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
    NoticeBoxTick("sendto");   // ★ 10-31 — ⑭ 게임 안 보내기(주 스레드일 때만)
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
    CLoginRewriter* lr = NULL;
    CLoginRewriter* lw = NULL;
    EnterCriticalSection(&g_tokCs);
    auto it = g_lobby.find(s);
    if (it != g_lobby.end()) { f = it->second; g_lobby.erase(it); }
    auto wit = g_loginRw.find(s);   // ★ 조각 5 의 3 — 로그인 줄 바꿔쓰기 상태도 소켓 번호와 함께 지운다
    if (wit != g_loginRw.end()) { lw = wit->second; g_loginRw.erase(wit); }
    auto lit = g_logRedact.find(s);   // ★ 조각 5 — 소켓 번호는 다시 쓰이므로 기록용 줄 상태도 지운다
    if (lit != g_logRedact.end()) { lr = lit->second; g_logRedact.erase(lit); }
    LeaveCriticalSection(&g_tokCs);
    delete f;
    delete lr;
    delete lw;
    return ((PFN_closesocket)Resolve(3))(s);
}

extern "C" int WSAAPI Hook_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen)
{
    void* ret = _ReturnAddress();
    NoticeBoxTick("recvfrom");   // ★ 10-31 — ⑭ 게임 안 받기(주 스레드일 때만)
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

// ★ 10-20 MO 6단계 v2 작업 5 — ⑧ 대화방 목록 빈 칸 가드(머리 주석 I). 0x44793A 의 `add esp,10h` · `cmp word ptr [eax],0` · `je 447992`
//   (9바이트)를 동굴로 돌린다. ★ 10-21 고침 — 10-20 판은 「eax(칸 글자)가 0 이면 막음」이었는데 정품 목록은 0 을 안 돌려줘(머리 주석 I) 한 번도 안 걸렸다.
//   그 자리의 레지스터 — esi = 누른 칸(0x447925) · edi = 목록 창(0x44790A · 창 번호 0x774 확인 뒤) · eax = 0x4016 이 돌려준 주소. ecx 는 뒤에서 안 읽는다
//   ① 지나간 수 +1 ② 칸이 0 ~ 개수−1 밖이면 막은 수 +1 · 447992(목록 다시 그리고 끝 — 원래 빈 글자 길) ③ 아니면 원래 비교 그대로 이어 447943
//   ④ 그래도 ③ 의 읽기에서 접근 위반이 나면 예외 처리기(ChanGuardVeh)가 받아 447992 로(스택은 원래 je 와 같은 자리)
static void Put32(unsigned char* p, DWORD v) { memcpy(p, &v, 4); }
static void* g_chanGuardCmp = NULL;   // 동굴 안 `cmp word ptr [eax],0` 자리 — 안전망은 이 자리에서 난 접근 위반만 받는다

static LONG CALLBACK ChanGuardVeh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->ExceptionAddress != g_chanGuardCmp) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    InterlockedIncrement(&g_chanGuardFault);
    DWORD data = 0, items = 0, count = 0;
    const bool ok = ReadSelf(c->Edi + 0x20, &data) && ReadSelf(data + 0x18, &items) && ReadSelf(data + 0x2c, &count);
    Log("⑧ 안전망 — 칸 글자 읽다 접근 위반(읽은 주소 %#lx · 칸 %ld · 개수 %d · 배열 %#lx%s) → 빈 칸 길 0x447992",
        (DWORD)er->ExceptionInformation[1], (LONG)c->Esi, (int)(short)(count & 0xFFFF), items, ok ? "" : " · 목록 데이터 못 읽음");
    c->Eip = 0x447992;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void InstallChanGuard()
{
    unsigned char* site = (unsigned char*)0x44793A;
    static const unsigned char orig[9] = { 0x83, 0xC4, 0x10, 0x66, 0x83, 0x38, 0x00, 0x74, 0x4F };
    if (memcmp(site, orig, sizeof(orig)) != 0) { Log("⑧ 대화방 목록 가드 — 자리 바이트가 정품과 달라 안 붙임"); return; }
    unsigned char* cave = (unsigned char*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   // 61바이트(10-21)
    if (!cave) { Log("⑧ 대화방 목록 가드 — 동굴 메모리 실패 err=%lu", GetLastError()); return; }
    unsigned char* p = cave;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x10;                                   // add esp,10h
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05; Put32(p, (DWORD)(uintptr_t)&g_chanGuardPass); p += 4;   // lock inc [pass]
    *p++ = 0x83; *p++ = 0x3D; Put32(p, (DWORD)(uintptr_t)&g_chanGuardNoRange); p += 4; *p++ = 0x00;   // cmp dword ptr [norange],0
    *p++ = 0x75; unsigned char* jnzText = p++;                               // jne text(⑨ 시험 ③ 만)
    *p++ = 0x85; *p++ = 0xF6;                                                // test esi,esi
    *p++ = 0x7C; unsigned char* jlBlock = p++;                               // jl block
    *p++ = 0x8B; *p++ = 0x4F; *p++ = 0x20;                                   // mov ecx,[edi+20h]   목록 데이터
    *p++ = 0x0F; *p++ = 0xBF; *p++ = 0x49; *p++ = 0x2C;                       // movsx ecx,word ptr [ecx+2Ch]   개수
    *p++ = 0x3B; *p++ = 0xF1;                                                // cmp esi,ecx
    *p++ = 0x7D; unsigned char* jgeBlock = p++;                              // jge block
    *jnzText = (unsigned char)(p - (jnzText + 1));
    g_chanGuardCmp = p;
    *p++ = 0x66; *p++ = 0x83; *p++ = 0x38; *p++ = 0x00;                       // text: cmp word ptr [eax],0
    *p++ = 0x0F; *p++ = 0x84; Put32(p, 0x447992 - (DWORD)(uintptr_t)(p + 4)); p += 4;   // je 447992
    *p++ = 0xE9; Put32(p, 0x447943 - (DWORD)(uintptr_t)(p + 4)); p += 4;      // jmp 447943
    *jlBlock = (unsigned char)(p - (jlBlock + 1));
    *jgeBlock = (unsigned char)(p - (jgeBlock + 1));
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05; Put32(p, (DWORD)(uintptr_t)&g_chanGuardHits); p += 4;   // block: lock inc [hits]
    *p++ = 0xE9; Put32(p, 0x447992 - (DWORD)(uintptr_t)(p + 4)); p += 4;      // jmp 447992
    if (!AddVectoredExceptionHandler(1, ChanGuardVeh)) { Log("⑧ 대화방 목록 가드 — 안전망 붙이기 실패 err=%lu", GetLastError()); return; }
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) { Log("⑧ 대화방 목록 가드 — 보호 바꾸기 실패 err=%lu", GetLastError()); return; }
    site[0] = 0xE9;
    Put32(site + 1, (DWORD)(uintptr_t)cave - (DWORD)(uintptr_t)(site + 5));
    for (int i = 5; i < 9; ++i) site[i] = 0x90;
    VirtualProtect(site, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(orig));
    g_chanGuardOn = true;
    Log("⑧ 대화방 목록 가드 붙임 — 0x44793A → 동굴 %p(%d바이트 · 칸 범위 검사 · 안전망 자리 %p)", (void*)cave, (int)(p - cave), g_chanGuardCmp);
}

// ★ 10-30 — 국가 칸 읽기를 DLL 로 돌림. 정품은 「로그인」 누름 처리 안에서 라우터에 붙기 전에 읽는다 —
//   `0x44a9df call 0x40da50`((칸+1, &국가) · cdecl · WOLAPI 를 부름) → 0 이면 국가 고르기 창(`0x44a9f6 call 0x4b4a80`)
//   라우터 연결에서 되살린 첫 판은 늦었다(10-30 사람 판 2 — 클라2 로그인 실패 뒤 Nick129 0 · 클라1 은 연결 없이 창). 그 call 하나의 대상만 바꾼다
static const uintptr_t kCountryCall = 0x44a9df, kCountryRead = 0x40da50;
typedef int(__cdecl* PFN_countryread)(int nick, int* out);

static int __cdecl CountryRead(int nick, int* out)
{
    KeepCountry();
    return ((PFN_countryread)kCountryRead)(nick, out);
}

static void InstallCountryHook()
{
    unsigned char* site = (unsigned char*)kCountryCall;
    DWORD rel = 0;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || (DWORD)(uintptr_t)(site + 5) + rel != (DWORD)kCountryRead) { Log("국가 칸 — 0x44a9df 가 정품 `call 0x40da50` 이 아니라 안 붙임"); return; }
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) { Log("국가 칸 — 보호 바꾸기 실패 err=%lu", GetLastError()); return; }
    Put32(site + 1, (DWORD)(uintptr_t)CountryRead - (DWORD)(uintptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("국가 칸 — 로그인 누름의 국가 읽기(0x44a9df)를 DLL 로 돌림(읽기 앞에 Nick0~129 를 20 으로)");
}

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
        g_rankOn = g_hostGameOn;   // ★ 랭크 매칭 조각 2 — ⑩ 은 ⑥ 길을 탄다
        sw[0] = 0;
        g_rankMarkTest = !off && GetEnvironmentVariableA("NOX_TEST_RANKMARK", sw, sizeof(sw)) && sw[0] == '1';   // ★ 조각 3 — ⑪ 시험
        g_listOn = (gameOk || forceList) && !off;
        if (g_hostGameOn) CloseHandle(CreateThread(NULL, 0, HostGameThread, NULL, 0, NULL));
        sw[0] = 0;
        const bool noGuard = GetEnvironmentVariableA("NOX_NO_CHANGUARD", sw, sizeof(sw)) && sw[0] == '1';   // ★ 10-20 v2 작업 5 — ⑧ 끄기
        sw[0] = 0;
        g_testGuard = gameOk && GetEnvironmentVariableA("NOX_TEST_CHANGUARD", sw, sizeof(sw)) && sw[0] == '1';   // ★ 10-21 ⑨ 시험
        g_mainTid = GetCurrentThreadId();

        char dir[MAX_PATH];
        if (GetEnvironmentVariableA("NOX_HOOK_LOG_DIR", dir, MAX_PATH)) {
            char exe[MAX_PATH]; exe[0] = 0;
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
            _snprintf(g_log, MAX_PATH - 1, "%s\\wsock_%s_%lu.log", dir, base, GetCurrentProcessId());
            char self[MAX_PATH]; self[0] = 0;
            GetModuleFileNameA(h, self, MAX_PATH);
            Log("=== wsock32 프록시(통과만) 붙음 ===  exe=%s  나=%s", exe, self);
            Log("Host Game 돌리기 = %s · 내 방 줄 = %s · ⑩ 랭크 매칭 = %s (판 확인 %s · 끄기 NOX_NO_HOSTGAME %s · 시험 NOX_MYROOM_LINE %s) — 셋 다 우리 로비 표시를 받아야 움직임",
                g_hostGameOn ? "켬" : "끔", g_listOn ? "켬" : "끔", g_rankOn ? "켬" : "끔", gameOk ? "통과" : "다른 판", off ? "켬" : "끔", forceList ? "켬" : "끔");
            Log("⑧ 대화방 목록 가드 = %s · ⑨ 시험(NOX_TEST_CHANGUARD) = %s · 실은 스레드 %lu", !gameOk ? "안 붙임(다른 판)" : noGuard ? "끔(NOX_NO_CHANGUARD)" : "붙임",
                g_testGuard ? "켬" : "끔", g_mainTid);
            Log("⑪ 랭크 참가 표시 시험(NOX_TEST_RANKMARK) = %s", g_rankMarkTest ? "켬" : "끔");
            CloseHandle(CreateThread(NULL, 0, SummaryThread, NULL, 0, NULL));
        }
        if (gameOk && !noGuard) InstallChanGuard();   // ★ 10-20 v2 작업 5 — 로그 파일을 정한 뒤(붙임 · 안 붙임 줄이 기록에 남게)
        if (gameOk) InstallCountryHook();             // ★ 10-30 — 두 클라 판 국가 고르기 창
    }
    return TRUE;
}
