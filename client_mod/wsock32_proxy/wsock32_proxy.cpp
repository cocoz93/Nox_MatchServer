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
#include <shellapi.h>   // ★ 런처 P5 — ⑮ 「등록」 → 가입 페이지(ShellExecuteW)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")   // ★ 11-02 — ⑱ IsWindow
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <deque>    // ★ 11-06 귓속말 조각 4 — 둘째 연결이 받은 줄 큐
#include <map>
#include <string>
#include <vector>

#include "HostGameEntry.h"     // ⑤ · ⑥ 내 방 정보 · 항목 · 채널 짓기(방 정보 포장은 서버와 같은 WolGameInfo — build.bat 이 같이 빌드)
#include "LobbyLineFilter.h"
#include "LoginRewriter.h"     // ★ 로그인 암호 조각 5 — 기록에 찍는 보낸 글에서 암호 칸 가리기(Log 모드) · 로비 연결의 apgar 자리 바꿔쓰기(Lobby 모드)
#include "RankNotice.h"        // ★ 11-05 플랜1 — 줄 서기 제한 줄 · 문구
#include "GameListRow.h"       // ★ 11-06 플랜1 조각 6 ㉓ — 게임 목록 줄
#include "LoginNotice.h"       // ★ 11-05 플랜1 — 로그인 화면에서 돌아왔을 때의 안내 고르기
#include "WhisperCore.h"       // ★ 11-06 귓속말 조각 3 — 명령 해석 · 표시 자리 · 다시 붙기 값 · NOXCHAT 줄 읽기(소켓 없는 판단 — whisper_test 가 같은 코드를 봄)
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
static void LauncherSummary(); // ★ 런처 P5 — ⑮
static void WhisperSummary();  // ★ 11-06 귓속말 조각 5 — 입력 자리 셋 × 갈래

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
        LauncherSummary(); // ★ 런처 P5 — ⑮ 도
        WhisperSummary();  // ★ 11-06 귓속말 조각 5 도
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
//   ★ 판 6 = 줄 서기 제한 안내(`kNoxClientPenaltyVersion` · 플랜 11-05 7절) — `NOXRANK WARN` · `BLOCKED` · `BANNED`
//   ★ 판 7 = 귓속말(`kNoxClientWhisperVersion` · `Docs/MO_WHISPER_PLAN_1105.md`) — 로비가 로그인 끝에 `NOXCHAT <채팅 서버 주소>` 를 보냄 · ② 처럼 떼어 낸다
static const char kNoxClientHello[] = "NOXCLIENT 7\r\n";
static wchar_t g_rankWaitNote[100];   // ★ 11-05 플랜1 — 기다림 글 앞에 얹는 경고(머리 주석 `g_rankWaitBuf`)
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
// ★ 11-06 귓속말 조각 3 — 로비가 알린 채팅 서버(`NOXCHAT a.b.c.d:포트`). ★ 조각 4 — 주소는 둘째 연결의 대상(`g_chatTarget` · 아래 「둘째 연결」)에 적는다
static volatile LONG g_chatNotices = 0; // 받은 NOXCHAT 줄 수(꼴 틀림 포함 — 다시 로그인할 때마다 하나)
static std::string g_loginName;         // ★ 귓속말 조각 4 — `AuthLogin` 이 받은 로그인 이름(NICK) · 둘째 연결의 AUTH 이름 · g_tokCs 로 지킨다

static std::string MyRoomLine(const std::vector<std::string>& items);
static void RankListArrived();   // ★ 11-05 플랜1 — `RankListBusy`
static std::string AuthLogin(SOCKET s, const std::string& name, const std::string& password);
static void RankOnToken();   // ★ 랭크 매칭 조각 3 — ⑪
static void RankOnLobbyLine(SOCKET s, const std::string& line);   // ★ 매칭 손보기 6 — ⑬
static SOCKET g_recSock = INVALID_SOCKET;   // ★ 11-04 ㉔ 대전 기록 — 우리 로비 표시(`NOXSERVER`)가 온 소켓 · 요청 줄을 보낼 곳

static bool MarkLobby(SOCKET s)   // 새로 적었으면 true
{
    EnterCriticalSection(&g_tokCs);
    const bool added = g_lobby.find(s) == g_lobby.end();
    if (added) {
        LobbyLineFilter* f = new LobbyLineFilter();
        if (g_listOn) f->onListEnd = [](const std::vector<std::string>& items) { RankListArrived(); return MyRoomLine(items); };   // ★ 11-05 플랜1 — `RankListBusy`
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
static volatile LONG g_loginKeyInvalid = 0;   // ★ 11-04 플랜1 — 마지막 로그인 답이 열쇠 거절이었나(`NXL!key_invalid`) · 실패 안내를 고른다(받기 스레드 → 주 스레드)
static volatile LONG g_loginBannedUntil = 0;   // ★ 11-05 플랜1 — 마지막 로그인 답의 정지 끝 시각(0 = 정지 아님 · 받기 스레드 → 주 스레드 · `LoginNotice.h`)

// ★ 11-06 귓속말 조각 4 — 로그인 서버 기준 주소(길 없음): NOX_AUTH_URL 이 있으면 그것, 없으면 이 로비 연결의 상대 IP : 8443. false = 상대 주소를 못 읽음
//   `AuthLogin` 과 둘째 연결(`ChatNote` — 로비 소켓이 살아 있는 NOXCHAT 처리 때 한 번)이 같이 쓴다
static bool AuthUrlOf(SOCKET s, std::wstring& url)
{
    wchar_t over[512];
    const DWORD en = GetEnvironmentVariableW(L"NOX_AUTH_URL", over, 512);
    if (en > 0 && en < 512) {
        url = over;
        return true;
    }
    typedef int (WSAAPI* PFN_getpeername)(SOCKET, struct sockaddr*, int*);
    sockaddr_in peer = {};
    int plen = (int)sizeof(peer);
    if (((PFN_getpeername)Resolve(5))(s, (struct sockaddr*)&peer, &plen) != 0 || peer.sin_family != AF_INET) return false;
    const unsigned char* b = (const unsigned char*)&peer.sin_addr;
    wchar_t buf[64];
    swprintf(buf, 64, L"https://%u.%u.%u.%u:8443", b[0], b[1], b[2], b[3]);
    url = buf;
    return true;
}

// ★ 조각 4 — 환경변수 NOX_AUTH_PIN(10-28 다른 PC · 서버 인증서 지문). `AuthLogin` 에서 뗐다
static std::string AuthPinHex()
{
    char pin[80] = {};
    const DWORD pn = GetEnvironmentVariableA("NOX_AUTH_PIN", pin, sizeof(pin));
    return pn > 0 && pn < sizeof(pin) ? std::string(pin, pn) : std::string(pn ? "?" : "");   // 너무 길면 틀린 꼴로 넘겨 붙지 않게
}

static std::string AuthLogin(SOCKET s, const std::string& name, const std::string& password)
{
    InterlockedExchange(&g_loginBannedUntil, 0);
    (void)password;   // ★ 런처 P4 — 정품 창의 암호 칸은 쓰지 않는다(선에도 안 나감 — apgar 자리를 통째로 바꿈)
    EnterCriticalSection(&g_tokCs);
    g_loginName = name;   // ★ 귓속말 조각 4 — 둘째 연결이 이 이름으로 AUTH(런처 · 열쇠 유무와 상관없이 — 아이디만 판 로비도 이 이름으로 받음)
    LeaveCriticalSection(&g_tokCs);
    if (LaunchName().empty()) {
        if (g_log[0]) Detail("로그인 서버 안 부름 s=%u — 런처로 안 켬(NOX_LAUNCH 없음) → NXL!launcher_required", (unsigned)s);
        return authclient::detail::FailLine(nox::LoginFail::LauncherRequired);
    }
    std::string key;
    if (!launchkey::Load(name, key)) {
        if (g_log[0]) Detail("로그인 서버 안 부름 s=%u — 「%s」 의 열쇠 파일이 없거나 못 풂 → NXL!key_invalid", (unsigned)s, name.c_str());
        InterlockedExchange(&g_loginKeyInvalid, 1);
        return authclient::detail::FailLine(nox::LoginFail::KeyInvalid);
    }
    const std::string pinHex = AuthPinHex();
    std::wstring url;
    if (!AuthUrlOf(s, url)) {
        if (g_log[0]) Detail("로그인 서버 호출 못 함 — 로비 연결의 상대 주소를 못 읽음 s=%u", (unsigned)s);
        return authclient::detail::FailLine(nox::LoginFail::Unreachable);
    }
    url = authclient::WithPath(url, L"/token");
    const authclient::Result r = authclient::Token(url, name, key, authclient::kTimeoutMs, pinHex);
    SecureZeroMemory(&key[0], key.size());
    if (g_log[0]) {
        const bool token = std::string_view(r.line).substr(0, nox::kLoginTokenMark.size()) == nox::kLoginTokenMark;
        Detail("로그인 서버 호출 s=%u 상태=%d 오류=%lu %lums 지문=%s → %s", (unsigned)s, r.status, r.error, r.ms, pinHex.empty() ? "없음" : "고정",
               token ? "로그인 토큰(NXL1…)" : r.line.c_str());   // 토큰 글은 기록에 안 남김
    }
    InterlockedExchange(&g_loginKeyInvalid, r.line == authclient::detail::FailLine(nox::LoginFail::KeyInvalid) ? 1 : 0);
    if (r.line == authclient::detail::FailLine(nox::LoginFail::Banned)) {
        uint32_t until = 0;
        if (nox::ParseBanUntil(r.detail, until)) InterlockedExchange(&g_loginBannedUntil, (LONG)until);
    }
    return r.line;
}

// ─────────────── ★ 11-06 귓속말 조각 4 — 둘째 연결(채팅 서버 nox_chat · 플랜 `Docs/MO_WHISPER_PLAN_1105.md` 5절 「둘째 스레드」)
//   로비 받기 스레드의 NOXCHAT 처리(`ChatNote`)가 대상(주소 · 로그인 이름 · 로그인 서버 주소)을 적고 깨운다 → 둘째 스레드(`ChatThread`)가 연결 → /token → AUTH → 받기.
//   받은 F · TO · NOUSER · LIMIT 은 잠금 걸린 큐(`g_chatIn`)로 → 주 스레드 `FrameTick` 의 `WhisperTick` 이 꺼내 판단 부품(`WhisperCore.h`)에 넣는다(게임 함수는 주 스레드에서만).
//   소켓 함수는 진짜 wsock32(`Resolve`) — 우리 훅(로비 줄 거름 · 기록 · 로그인 줄 바꿔쓰기)을 안 탄다.
//   `AuthLogin` 을 그대로 안 쓰는 까닭: 소켓 번호는 닫힌 뒤 다른 소켓에 다시 쓰여 엉뚱한 곳에 열쇠를 보낼 수 있고, `g_loginKeyInvalid` · `g_loginBannedUntil` 은
//   로비 실패 안내가 읽는다(둘째 연결은 둘 다 안 건드림). 그래서 로비 소켓이 살아 있는 NOXCHAT 처리 때 로그인 서버 주소를 한 번 구해 둔다.
//   토큰은 채팅 서버에 붙은 뒤에 받는다 — 채팅 서버가 죽었을 때 모두가 로그인 서버를 치지 않게(플랜 5절)
struct ChatTarget {
    uint32_t ip = 0;        // a<<24|b<<16|c<<8|d(g_lobbyIp 와 같은 차례)
    uint16_t port = 0;
    std::string name;       // AuthLogin 이 받은 로그인 이름
    std::wstring authUrl;   // 로그인 서버 기준 주소(빈 글자 = 못 구함 → 토큰 못 받음)
    std::string pin;        // NOX_AUTH_PIN
    LONG gen = 0;           // NOXCHAT 을 받을 때마다 하나씩(0 = 아직 없음)
};
static CRITICAL_SECTION g_chatCs;
static ChatTarget g_chatTarget;             // g_chatCs
static std::deque<std::string> g_chatIn;    // 받은 줄(둘째 → 주) · 빈 글자 = AUTHOK 받은 연결이 끊김 · g_chatCs
static HANDLE g_chatWake = NULL;            // 새 NOXCHAT → 다시 붙기 기다림 · 멈춤에서 깸(자동 리셋) · g_chatCs 아래 한 번 만듦
static volatile LONG g_chatStarted = 0;     // 둘째 스레드를 띄웠나(주 스레드가 잠금 없이 봄)
static volatile LONG g_chatInDropped = 0;   // 큐가 넘쳐 버린 줄
static std::deque<std::string> g_chatOut;   // ★ 조각 5 — 보낼 줄(주 → 둘째 · 「W 이름 글\r\n」) · g_chatCs
static bool g_chatUp = false;               // ★ 조각 5 — AUTHOK 받은 연결이 있음(주 스레드가 보낼지 8번으로 안내할지) · g_chatCs
static const size_t kChatInMax = 200;       // 주 스레드가 못 비우는 동안(그리기 고리가 안 도는 화면) 쌓이는 한도 — 넘치면 오래된 것부터 버리고 셈
static const DWORD kChatConnectMs = 3000;   // 붙기 한도 — 죽은 주소에 윈도 기본(약 21초)만큼 안 막히게
static const DWORD kChatSilentMs = 90000;   // 서버는 30초 조용하면 PING(`ChatHub.h` kChatPingMs) — 90초 아무것도 안 오면 끊긴 것으로(서버 kChatIdleMs 와 같은 값)
static const size_t kChatLineMax = 4096;    // 줄바꿈 없이 이만큼 오면 끊음(서버 `TcpLines.h` kMaxLine 과 같은 값)

typedef SOCKET(WSAAPI* PFN_wscSocket)(int, int, int);
typedef int(WSAAPI* PFN_wscConnect)(SOCKET, const struct sockaddr*, int);
typedef int(WSAAPI* PFN_wscIoctl)(SOCKET, long, u_long*);
typedef int(WSAAPI* PFN_wscSelect)(int, fd_set*, fd_set*, fd_set*, const struct timeval*);
typedef int(WSAAPI* PFN_wscSend)(SOCKET, const char*, int, int);
typedef int(WSAAPI* PFN_wscRecv)(SOCKET, char*, int, int);
typedef int(WSAAPI* PFN_wscClose)(SOCKET);
typedef int(WSAAPI* PFN_wscErr)(void);
typedef int(WSAAPI* PFN_wscStartup)(WORD, LPWSADATA);

static bool ChatSendAll(SOCKET s, const std::string& d)
{
    for (size_t i = 0; i < d.size();) {
        const int r = ((PFN_wscSend)Resolve(19))(s, d.data() + i, (int)(d.size() - i), 0);
        if (r <= 0) return false;
        i += (size_t)r;
    }
    return true;
}

// 받은 줄 하나를 주 스레드 큐에 — F · TO · NOUSER · LIMIT 만(그 밖은 이 스레드가 끝냄). 빈 글자 = 끊김 표시
static void ChatQueue(const std::string& line)
{
    EnterCriticalSection(&g_chatCs);
    if (g_chatIn.size() >= kChatInMax) {
        g_chatIn.pop_front();
        const LONG n = InterlockedIncrement(&g_chatInDropped);
        if (n == 1 || n % 50 == 0) Log("귓속말 받은 줄 큐가 넘쳐 오래된 것을 버림(누적 %ld) — 주 스레드가 못 비움", n);
    }
    g_chatIn.push_back(line);
    const size_t q = g_chatIn.size();
    LeaveCriticalSection(&g_chatCs);
    if (!line.empty()) Log("귓속말 받음 %s(큐 %zu)", line.substr(0, line.find(' ')).c_str(), q);   // 낱말만 — 이름 · 글은 기록에 안 남김(서버와 같음)
}

struct ChatEnd {
    whisper::Drop why = whisper::Drop::ConnectFail;
    bool authed = false;   // AUTHOK 를 받았나 — 받았으면 다시 붙기 간격을 3초로 되돌린다
    bool moved = false;    // 새 NOXCHAT 의 주소 · 이름이 달라 끊음 — 곧바로 다시
    LONG gen = 0;          // 이 연결이 받아들인 마지막 NOXCHAT 차례 번호 — 같은 대상의 NOXCHAT(같은 계정으로 다시 로그인)은 연결을 그대로 두고 받아들인다
};

// 연결 하나 — 붙기 → 토큰 → AUTH → 받기 고리. 끝난 까닭을 돌려준다
static ChatEnd ChatSession(const ChatTarget& t)
{
    ChatEnd e;
    e.gen = t.gen;
    const unsigned a0 = t.ip >> 24, a1 = (t.ip >> 16) & 255, a2 = (t.ip >> 8) & 255, a3 = t.ip & 255;
    if (t.name.empty()) {   // 로그인 줄이 이 DLL 을 안 지났다 — 이름 없이는 서버가 꼴 틀림으로 거절하므로 붙지 않는다
        e.why = whisper::Drop::AuthForm;
        Log("귓속말 — 로그인 이름을 못 봄(로그인 줄이 이 DLL 을 안 지남) · 안 붙음");
        return e;
    }
    const SOCKET s = ((PFN_wscSocket)Resolve(23))(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        Log("귓속말 소켓 못 만듦 err=%d", ((PFN_wscErr)Resolve(111))());
        return e;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = (u_short)(((t.port & 0xff) << 8) | (t.port >> 8));   // 망 차례(htons — 이 DLL 은 바이트 차례 함수를 들여오지 않는다)
    addr.sin_addr.s_addr = (t.ip >> 24) | ((t.ip >> 8) & 0xff00) | ((t.ip & 0xff00) << 8) | ((t.ip & 0xff) << 24);
    u_long nb = 1;
    ((PFN_wscIoctl)Resolve(12))(s, FIONBIO, &nb);
    const DWORD t0 = GetTickCount();
    bool up = ((PFN_wscConnect)Resolve(4))(s, (const struct sockaddr*)&addr, sizeof(addr)) == 0;
    if (!up && ((PFN_wscErr)Resolve(111))() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(s, &wf); FD_SET(s, &ef);
        timeval tv = { (long)(kChatConnectMs / 1000), (long)(kChatConnectMs % 1000) * 1000 };
        up = ((PFN_wscSelect)Resolve(18))(0, NULL, &wf, &ef, &tv) > 0 && wf.fd_count == 1;   // 못 붙으면 ef 쪽에 선다(FD_ISSET 은 들여오는 함수라 안 씀)
    }
    if (!up) {
        ((PFN_wscClose)Resolve(3))(s);
        Log("귓속말 서버에 못 붙음 — %u.%u.%u.%u:%u(%lums)", a0, a1, a2, a3, t.port, GetTickCount() - t0);
        return e;
    }
    nb = 0;
    ((PFN_wscIoctl)Resolve(12))(s, FIONBIO, &nb);
    Log("귓속말 서버에 붙음 — %u.%u.%u.%u:%u(%lums)", a0, a1, a2, a3, t.port, GetTickCount() - t0);

    // 토큰 — 런처 열쇠가 있으면 /token(최대 3초 · 이 스레드라 게임은 안 막힘). 열쇠가 없으면 토큰 없이(아이디만 판 채팅 서버만 받고 토큰 판은 AUTHNO form → 멈춤)
    std::string auth = "AUTH " + t.name;
    std::string key;
    if (!LaunchName().empty() && launchkey::Load(t.name, key)) {
        authclient::Result r;
        if (t.authUrl.empty()) r.line = authclient::detail::FailLine(nox::LoginFail::Unreachable);
        else r = authclient::Token(authclient::WithPath(t.authUrl, L"/token"), t.name, key, authclient::kTimeoutMs, t.pin);
        SecureZeroMemory(&key[0], key.size());
        std::string hex;
        whisper::Drop why = whisper::Drop::TokenUnreachable;
        const bool ok = whisper::TokenOfLine(r.line, hex, why);
        Log("귓속말 토큰 — 상태=%d 오류=%lu %lums → %s", r.status, r.error, r.ms, ok ? "받음" : r.line.c_str());   // 토큰 글은 기록에 안 남김
        if (!ok) {
            ((PFN_wscClose)Resolve(3))(s);
            e.why = why;
            return e;
        }
        auth += " " + hex;
        SecureZeroMemory(&hex[0], hex.size());
    } else {
        Log("귓속말 — 런처 열쇠가 없어 토큰 없이 AUTH(아이디만 판 채팅 서버만 받음)");
    }
    auth += "\r\n";
    const bool sent = ChatSendAll(s, auth);
    SecureZeroMemory(&auth[0], auth.size());
    if (!sent) {
        ((PFN_wscClose)Resolve(3))(s);
        e.why = whisper::Drop::Lost;
        Log("귓속말 AUTH 못 보냄 err=%d", ((PFN_wscErr)Resolve(111))());
        return e;
    }

    // 받기 고리 — 50ms 마다 새 NOXCHAT 을 본다(주소 · 이름이 바뀌었으면 끊고 새로)
    std::string buf;
    DWORD lastRecv = GetTickCount();
    bool end = false;
    char tmp[1024];
    while (!end) {
        std::deque<std::string> out;   // ★ 조각 5 — 주 스레드가 쌓은 보낼 줄(AUTHOK 뒤에만 쌓임 — g_chatUp)
        EnterCriticalSection(&g_chatCs);
        if (g_chatTarget.gen != e.gen) {
            e.moved = g_chatTarget.ip != t.ip || g_chatTarget.port != t.port || g_chatTarget.name != t.name;
            e.gen = g_chatTarget.gen;   // 같은 대상이면(같은 계정으로 다시 로그인) 이 연결 그대로 — 그 NOXCHAT 은 이 연결이 받아들임
        }
        out.swap(g_chatOut);
        LeaveCriticalSection(&g_chatCs);
        if (e.moved) {
            e.why = whisper::Drop::Lost;
            Log("귓속말 — 새 NOXCHAT 의 주소 · 이름이 달라 끊고 새로 붙음");
            break;
        }
        for (const std::string& l : out) {
            if (!ChatSendAll(s, l)) {
                e.why = whisper::Drop::Lost;
                Log("귓속말 보내기 실패 err=%d — 끊김으로", ((PFN_wscErr)Resolve(111))());
                end = true;
                break;
            }
            Log("귓속말 보냄 W(%zu바이트)", l.size());   // 길이만 — 이름 · 글은 기록에 안 남김
        }
        if (end) break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv = { 0, 50 * 1000 };
        const int sr = ((PFN_wscSelect)Resolve(18))(0, &rf, NULL, NULL, &tv);
        if (sr == 0) {
            if (GetTickCount() - lastRecv < kChatSilentMs) continue;
            e.why = whisper::Drop::Lost;
            Log("귓속말 서버가 %lu초 동안 아무것도 안 보냄 — 끊긴 것으로", kChatSilentMs / 1000);
            break;
        }
        const int r = sr < 0 ? -1 : ((PFN_wscRecv)Resolve(16))(s, tmp, (int)sizeof(tmp), 0);
        if (r <= 0) {
            e.why = whisper::Drop::Lost;
            Log("귓속말 연결 끊김 r=%d err=%d", r, r < 0 ? ((PFN_wscErr)Resolve(111))() : 0);
            break;
        }
        lastRecv = GetTickCount();
        buf.append(tmp, (size_t)r);
        for (size_t p; !end && (p = buf.find('\n')) != std::string::npos;) {
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();   // 서버는 \r\n(`TcpLines.cpp` Send)
            const whisper::ServerLine sl = whisper::ParseServerLine(line);
            switch (sl.kind) {
            case whisper::Line::AuthOk:
                e.authed = true;
                EnterCriticalSection(&g_chatCs);
                g_chatUp = true;   // ★ 조각 5 — 이제부터 /w 를 보낸다
                LeaveCriticalSection(&g_chatCs);
                Log("귓속말 로그인 됨 — %s", t.name.c_str());
                break;
            case whisper::Line::AuthNo:
                e.why = whisper::DropOfAuthNo(sl.text);
                Log("귓속말 로그인 거절 — AUTHNO %s", sl.text.c_str());
                end = true;
                break;
            case whisper::Line::Kicked:
                e.why = whisper::Drop::Kicked;
                Log("귓속말 — KICKED(같은 이름이 다른 게임에서 새로 붙음)");
                end = true;
                break;
            case whisper::Line::Ping:
                if (!ChatSendAll(s, "PONG\r\n")) { e.why = whisper::Drop::Lost; end = true; }
                break;
            case whisper::Line::Pong:
                break;
            case whisper::Line::From:
            case whisper::Line::To:
            case whisper::Line::NoUser:
            case whisper::Line::Limit:
                ChatQueue(line);
                break;
            case whisper::Line::Unknown:
            case whisper::Line::kCount:
                Log("귓속말 모르는 줄 %zu바이트 — 버림", line.size());
                break;
            }
        }
        if (!end && buf.size() > kChatLineMax) {
            e.why = whisper::Drop::Lost;
            Log("귓속말 줄바꿈 없이 %zu바이트 — 끊음", buf.size());
            break;
        }
    }
    ((PFN_wscClose)Resolve(3))(s);
    // ★ 조각 5 — 연결 없음으로 돌리고 못 보낸 줄을 버린 뒤에 끊김 표시를 넣는다. 주 스레드는 받은 줄을 비운 다음에 입력을 받으므로(`WhisperInput`)
    //   그사이 친 /w 는 8번 안내로 · 그 전에 친(답 기다림에 든) 것은 끊김 표시에서 8번으로 — 답 차례가 안 어긋난다
    EnterCriticalSection(&g_chatCs);
    g_chatUp = false;
    const size_t unsent = g_chatOut.size();
    g_chatOut.clear();
    LeaveCriticalSection(&g_chatCs);
    if (unsent) Log("귓속말 연결이 끝나 못 보낸 줄 %zu 버림(8번 안내로)", unsent);
    if (e.authed) ChatQueue(std::string());   // 끊김 표시 — 답을 못 받은 보낸 글을 주 스레드가 8번으로
    return e;
}

static DWORD WINAPI ChatThread(LPVOID)
{
    WSADATA wd = {};
    const int st = ((PFN_wscStartup)Resolve(115))(MAKEWORD(2, 2), &wd);   // 자기 참조 수 — Game.exe 가 WSACleanup 을 불러도(10곳) 이 연결은 산다(플랜 5절)
    Log("귓속말 둘째 스레드 %lu 시작 — WSAStartup %d", GetCurrentThreadId(), st);
    uint32_t failures = 0;   // 이어진 실패(AUTHOK 받으면 0 · 새 NOXCHAT 이면 0)
    LONG seenGen = 0, stoppedGen = 0;
    for (;;) {
        ResetEvent(g_chatWake);   // 이미 읽을 NOXCHAT 의 깨움을 지운다 — 차례 번호를 이 뒤에 읽으므로 그다음 깨움은 안 놓친다
        EnterCriticalSection(&g_chatCs);
        const ChatTarget t = g_chatTarget;
        LeaveCriticalSection(&g_chatCs);
        if (t.gen == stoppedGen) {   // 멈춤 — 다음 NOXCHAT(로비에 다시 로그인)까지
            WaitForSingleObject(g_chatWake, INFINITE);
            continue;
        }
        if (t.gen != seenGen) {
            seenGen = t.gen;
            failures = 0;
        }
        const ChatEnd e = ChatSession(t);
        seenGen = e.gen;   // 연결 중에 받아들인 같은 대상의 NOXCHAT 으로는 실패 수를 안 지운다
        if (e.moved) continue;
        if (e.authed) failures = 0;
        if (!whisper::ShouldRetry(e.why)) {
            stoppedGen = e.gen;   // 연결이 받아들인 NOXCHAT 까지는 멈춤 앞의 것 — 멈출 까닭이 온 뒤의 NOXCHAT(새 로그인)만 다시 붙게
            Log("귓속말 멈춤 — %s · 다음 NOXCHAT(로비에 다시 로그인)까지 안 붙음", whisper::DropName(e.why));
            continue;
        }
        ResetEvent(g_chatWake);
        EnterCriticalSection(&g_chatCs);
        const LONG now = g_chatTarget.gen;
        LeaveCriticalSection(&g_chatCs);
        if (now != e.gen) {   // 붙기 · 토큰 동안 새 NOXCHAT — 새 로그인이니 기다리지 않는다(위에서 실패 수도 0)
            Log("귓속말 — 연결 중에 새 NOXCHAT(%ld번째) · 기다리지 않고 다시(%s)", now, whisper::DropName(e.why));
            continue;
        }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const uint32_t wait = whisper::NextRetryMs(failures, (uint32_t)q.LowPart);
        ++failures;
        Log("귓속말 다시 붙기 %ums 뒤 — %s(이어진 실패 %u)", wait, whisper::DropName(e.why), failures);
        WaitForSingleObject(g_chatWake, wait);   // 새 NOXCHAT 이면 일찍 깬다
    }
}

// NOXCHAT 처리(로비 받기 스레드) — 대상을 적고 둘째 스레드를 깨운다(처음이면 띄운다). 로그인 서버 주소는 로비 소켓이 살아 있는 지금 구한다
static void ChatNote(SOCKET s, uint32_t ip, uint16_t port)
{
    std::wstring url;
    const bool urlOk = AuthUrlOf(s, url);
    const std::string pin = AuthPinHex();
    EnterCriticalSection(&g_tokCs);
    const std::string name = g_loginName;
    LeaveCriticalSection(&g_tokCs);
    EnterCriticalSection(&g_chatCs);
    g_chatTarget.ip = ip;
    g_chatTarget.port = port;
    g_chatTarget.name = name;
    g_chatTarget.authUrl = urlOk ? url : std::wstring();
    g_chatTarget.pin = pin;
    const LONG gen = ++g_chatTarget.gen;
    bool started = false;
    if (!g_chatStarted) {
        g_chatWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        const HANDLE th = CreateThread(NULL, 0, ChatThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
        started = th != NULL;
        InterlockedExchange(&g_chatStarted, 1);   // 못 띄웠어도 다시 안 해 본다(기록에 남음)
    }
    LeaveCriticalSection(&g_chatCs);
    SetEvent(g_chatWake);
    Log("귓속말 연결 대상 — %u.%u.%u.%u:%u · 이름 %s · 로그인 서버 주소 %s(%ld번째)%s", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port,
        name.empty() ? "(못 봄)" : name.c_str(), urlOk ? "구함" : "못 구함", gen, started ? " · 둘째 스레드 띄움" : "");
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
    if (line.compare(0, strlen(LobbyLineFilter::kChatMark), LobbyLineFilter::kChatMark) == 0) {   // ★ 11-06 귓속말 조각 3 — 주소 · ★ 조각 4 — 둘째 연결을 깨움
        uint32_t ip = 0;
        uint16_t port = 0;
        const LONG n = InterlockedIncrement(&g_chatNotices);
        if (!whisper::ParseChatLine(line, ip, port)) { Log("귓속말 서버 알림 꼴이 틀려 버림 — %.60s(%ld번째)", line.c_str(), n); return; }
        Log("귓속말 서버 알림 받음 — %u.%u.%u.%u:%u(%ld번째)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port, n);
        ChatNote(s, ip, port);
        return;
    }
    if (line.compare(0, strlen(LobbyLineFilter::kServerMark), LobbyLineFilter::kServerMark) != 0) { OnTokenLine(line, who); return; }
    typedef int (WSAAPI* PFN_getpeername)(SOCKET, struct sockaddr*, int*);
    const DWORD err = GetLastError();
    sockaddr_in peer = {};
    int plen = (int)sizeof(peer);
    if (((PFN_getpeername)Resolve(5))(s, (struct sockaddr*)&peer, &plen) == 0 && peer.sin_family == AF_INET) {
        const unsigned char* b = (const unsigned char*)&peer.sin_addr;
        InterlockedExchange(&g_lobbyIp, (LONG)(((DWORD)b[0] << 24) | ((DWORD)b[1] << 16) | ((DWORD)b[2] << 8) | b[3]));
        InterlockedExchange(&g_ourLobby, 1);
        g_recSock = s;   // ★ 11-04 ㉔
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

// ★ 11-05 플랜1 — ⑲ 「리스트」 누름부터 그 목록 답이 끝나고 조금 뒤까지 공용 알림창을 안 건드린다(`RankListBusy` · `RankFoundTick`)
//   0 = 안 기다림 · 1 = 답 기다림 · 2 = 답 받음(정품이 창을 닫을 때까지 잠깐 더)
static volatile LONG g_rankListState = 0;
static DWORD g_rankListSinceMs = 0;
static void RankListArrived()
{
    if (g_rankListState != 1) return;
    g_rankListSinceMs = GetTickCount();
    InterlockedExchange(&g_rankListState, 2);
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
static const uintptr_t kClassWin = 0x7114E4;       // 직업 고르기 창(「SelClass.wnd」 만든 뒤 넣는 칸 — 0x4a3b7a) — ★ 11-05 캐릭터가 하나도 없으면 캐릭터 창 없이 곧바로 이 창
static const uintptr_t kColorWin = 0x711640;       // 캐릭터 꾸미기 창(「SelColor.wnd」 — 0x4a5029) — 직업 다음 창
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

static bool CharStepWinAlive(uintptr_t slot)
{
    const DWORD win = *(volatile DWORD*)slot;
    DWORD flags = 0;
    return win && ReadSelf(win + 4, &flags) && flags != 0xACACACAC && (flags & 0x810) == 0;
}

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
        // ★ 11-05 — 캐릭터 파일이 없는 사람은 캐릭터 창이 안 떠 여기를 못 지나 자기 PC 에 로컬 게임을 열었다(꾸러미 파일럿 — 빈 Save 로 「게임 호스트」 → 정품 게임 설정 창).
        //   캐릭터를 고르거나 만드는 세 창 가운데 어느 것이든 뜨면 돌린다
        const bool alive = CharStepWinAlive(kCharWin) || CharStepWinAlive(kClassWin) || CharStepWinAlive(kColorWin);
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
static const DWORD kBtnHost = 10002, kBtnRank = 10007, kBtnQuest = 10003;
static volatile LONG g_questBlocked = 0;           // ★ 11-04 퀘스트 호스트(10003) 누름을 막은 수
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
        case kBtnQuest:   // ★ 11-04 — 퀘스트 호스트 막음. 우리 서버엔 퀘스트가 없어 원래 길은 퀘스트 캐릭터를 고르게 한 뒤 일반 방과 같은 대기 맵으로 들어간다(`Docs/MO_UX_AUDIT_1103.md` q1 · q2)
            if (!g_ourLobby) return orig(win, msg, a3, a4);
            InterlockedIncrement(&g_questBlocked);
            if (InterlockedExchange(&g_rankArmed, 0)) Log("⑩ 퀘스트 호스트 누름 — 남아 있던 랭크 표시 끔");
            Log("⑩ 퀘스트 호스트 누름 막음(누적 %ld) — 글자는 tools/ui_apply.ps1 「퀘스트 호스트 (미구현)」", (long)g_questBlocked);
            return 0;
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
// ★ 11-02 — 랭크 기다림이 취소 · 거절 · 수락 한도로 끝나면 방 목록으로(RankBackToListTick). 랭크 누름이 Host Game 길을 타서
//   지도 화면이 남는데, 돌아갈 「리스트」 칸은 ⑫ 가 숨겼고 「리스트 갱신」은 지도를 안 닫는다(`noxworld.c` 10006 · 10004)
static volatile LONG g_rankToList = 0;
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

// ★ 11-02 — 알림창 포인터를 따라 들어가기 전 확인. 정품은 화면을 통째로 치울 때(`0x489ee0`) 창만 풀고
//   `[0x69cba4]` 는 그대로 둔다(0 으로 만드는 곳은 알림창 닫기 `0x449fe3` 뿐) — 풀린 창은 0xAC 로 채워져
//   자식 찾기(`0x4889d0`)가 `0x4889e8` 에서 죽는다(11-02 판 끝 알림창이 뜬 채 창 닫기 · 6판 전부)
static bool LiveMsgBox(DWORD* box)
{
    static DWORD s_logged = 0;
    DWORD flags = 0;
    if (!ReadSelf(kMsgBox, box) || !*box) return false;
    if (ReadSelf(*box + 4, &flags) && flags != 0xACACACAC) return true;
    if (*box != s_logged) { s_logged = *box; Log("⑱ 알림창 %#lx 는 이미 치워짐(포인터만 남음) — 건드리지 않음", *box); }
    return false;
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
        g_rankWaitNote[0] = L'\0';   // ★ 11-05 플랜1 — 앞 기다림의 경고를 지움
        const LONG n = InterlockedIncrement(&g_rankMarked);
        Log("⑪ 랭크 참가(누적 %ld) — 내 방 참가 줄 앞에 NOXRANK JOIN · 기다림 켬(알림창 %s)", n, MsgBoxOpen() ? "떠 있음" : "없음");
        return kRankJoinLine;
    }
    if (g_rankWaiting && !MsgBoxOpen() && InterlockedExchange(&g_rankWaiting, 0)) {
        InterlockedExchange(&g_rankToList, 1);
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
    InterlockedExchange(&g_rankToList, 0);       // ★ 11-02 — 판으로 들어간다
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
// ★ 11-05 플랜1 — 판 나가기 · 끊김으로 받은 1회 경고(다음 랭크 단추의 `NOXRANK WARN match`). 줄은 그대로 서므로 확인 창으로 바꾸면 안 된다 —
//   확인을 누르면 창이 닫혀 ⑪ 이 줄에서 뺀다. 기다림 글 앞에 얹어 기다리는 동안 보여 준다
static wchar_t g_rankWaitBuf[160];         // 정품 글 칸이 쥐는 글(게임 주 스레드만 씀)
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
    if (!LiveMsgBox(&box)) return;
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
        swprintf(g_rankWaitBuf, sizeof(g_rankWaitBuf) / sizeof(g_rankWaitBuf[0]), L"%ls상대 찾는 중... 0:00", g_rankWaitNote);
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
    swprintf(g_rankWaitBuf, sizeof(g_rankWaitBuf) / sizeof(g_rankWaitBuf[0]), L"%ls상대 찾는 중... %lu:%02lu", g_rankWaitNote, sec / 60, sec % 60);
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
static const DWORD kBtnRefresh = 10006;            // ★ 11-03 ⑳ — 「리스트 갱신」
static volatile LONG g_roomsChanged = 0;           // ★ 11-03 ⑳ — `NOXRANK ROOMS` 받음(받기 훅에서 누름)
static volatile LONG g_roomsRefresh = 0;           // ★ 11-03 ⑳ — 누른 횟수
static const DWORD kRadioOn = 4;                   // 창 +0x24 — 라디오 눌림
static volatile LONG g_menuFilterWrapped = 0;      // 감싼 필터 창
static volatile LONG g_menuFilterSkip = 0;         // 처리 함수 칸이 원래 함수가 아니라 안 감싼 필터 창
static volatile LONG g_menuDone = 0;               // 「완료」 → 「리스트」 길
static volatile LONG g_menuDoneMiss = 0;           // 「완료」인데 게임 목록 창 · 「리스트」 버튼을 못 찾음(아무것도 안 함)
static volatile LONG g_menuListHidden = 0;         // 「리스트」 칸 숨김(보여서 숨긴 횟수)
static volatile LONG g_menuListMiss = 0;           // 게임 목록 창에서 「리스트」 버튼을 못 찾음(창마다 한 번)

// 「리스트」(10004) 누름을 정품 목록 함수에 + 라디오 맞춤 — 만들기 표시를 끄고 목록을 보여 주고 새로 받는다(`noxworld.c` 10004)
static void PressListMode(DWORD world, DWORD list, DWORD a4)
{
    ((PFN_wndproc4)kListProc)(world, kMsgClick, list, a4);
    const DWORD filter = ((PFN_findchild)kFindChild)(world, kBtnFilter);
    *(volatile DWORD*)(list + 0x24) |= kRadioOn;
    if (filter) *(volatile DWORD*)(filter + 0x24) &= ~kRadioOn;
}

// ★ 11-02 — 받기 훅(주 스레드 · 어느 창 함수도 호출 중이 아님) — 알림창이 닫힌 뒤 방 목록으로(`g_rankToList` 머리 주석)
static void RankBackToListTick(DWORD world)
{
    if (!g_rankToList || MsgBoxOpen()) return;
    InterlockedExchange(&g_rankToList, 0);
    const DWORD list = ((PFN_findchild)kFindChild)(world, kBtnList);
    if (!list) { Log("⑲ 랭크 끝남 — 「리스트」(10004)를 못 찾아 지도 그대로"); return; }
    PressListMode(world, list, 0);
    g_rankListSinceMs = GetTickCount();
    InterlockedExchange(&g_rankListState, 1);   // ★ 11-05 플랜1
    Log("⑲ 랭크 끝남 — 지도 대신 방 목록으로(「리스트」 누름을 넣음)");
}

static bool PressChild(DWORD win, DWORD id);
// ★ 11-03 ⑳ — 로비의 `NOXRANK ROOMS`(방 목록이 바뀜) → 받기 훅(주 스레드 · 어느 창 함수도 호출 중이 아님)에서 「리스트 갱신」(10006)을 누른다.
//   왜 — 정품은 LIST 를 그 화면에 들어갈 때 · 그 단추를 누를 때만 보내, 먼저 나온 사람 화면엔 닫힌 방이 그대로 남았다(11-03 다른 PC 판 ③).
//   알림창이 떠 있거나(랭크 기다림 · 안내) 필터 화면이면(정품이 그 단추를 꺼 둠) 다음 틱으로 미룬다 — 누름은 사람이 누른 것과 같은 꼴(⑯ PressChild)
static void RoomsChangedTick(DWORD world)
{
    if (!g_roomsChanged || MsgBoxOpen()) return;
    DWORD filter = 0, flags = 0;
    if (ReadSelf(kFilterWin, &filter) && filter && ReadSelf(filter + 4, &flags) && !(flags & kWinHidden)) return;
    // 10006 은 만들기 깃발까지 끈다(`noxworld.c` 10006) — 「게임 호스트하기」 를 누르고 캐릭터 고르는 중이면 그 길이 끊기므로 안 누른다 · 창이 숨었을 때도
    if (*(volatile DWORD*)kCreateOrJoin || !ReadSelf(world + 4, &flags) || (flags & kWinHidden)) return;
    InterlockedExchange(&g_roomsChanged, 0);
    const bool ok = PressChild(world, kBtnRefresh);
    const LONG n = InterlockedIncrement(&g_roomsRefresh);
    Log("⑳ 방 목록 바뀜 — 「리스트 갱신」(10006) %s(누적 %ld)", ok ? "누름" : "못 찾음", n);
}

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
    PressListMode(world, list, a4);
    const DWORD filter = ((PFN_findchild)kFindChild)(world, kBtnFilter);
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
// ★ 11-03 — 수락 한도가 지나면 창이 말없이 닫혀 「갑자기 창이 꺼짐」으로 보였다(다른 PC 판 · `_out/qa_1103/a1`) — 까닭을 말하는 확인 창을 정품 취소 길 뒤에 띄운다
static const wchar_t kRankExpiredText[] = L"수락 시간이 지나 매칭이 취소되었습니다";
static const DWORD kDlgNoticeFlags = 0x21;    // 확인 단추 + 누르면 닫힘(정품 「Kicked」 · 「Timeout」 알림과 같은 값 — `noxworld.c` 237 · 243)
static volatile LONG g_rankEvFound = 0, g_rankEvWait = 0, g_rankEvExpired = 0, g_rankEvReady = 0;   // 받은 줄 → ⑩ 자리에서 처리
// ★ 11-05 플랜1 — 줄 서기 제한 줄. 정품 글 칸은 글을 복사하지 않고 주소만 쥐어 전역 버퍼에 둔다
static volatile LONG g_rankEvNotice = 0, g_rankNoticeValue = 0, g_rankNoticeShown = 0;
static wchar_t g_rankNoticeBuf[96];
static volatile LONG g_lobbyBannedUntil = 0;   // `NOXRANK BANNED` — 뒤이은 끊김 안내(㉑)가 정지 문구를 쓰게
// ★ 11-04 — 정품 소리 함수 `0x4528d0`(cdecl 번호 · 음량 — 호출 꼴 `push 0x64 · push 번호 · call · add esp,8` · 실제 호출을 훅으로 재 확인).
//   번호 = OpenNox `common/sound/sound.go` 상수 차례(ShellClick 921 등 호출 기록과 일치). 775 = MetallicBong(bong1.wav · 3.0초 · 사용자가 소리 모음에서 고름)
typedef void(__cdecl* PFN_sound)(int id, int volume);
static const uintptr_t kSoundPlay = 0x4528d0;
static const int kSoundMatchFound = 775;
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

// ★ 11-04 플랜1 ㉒ — 로비 줄 `NOXRANK ROOMUI <상태> <방장> <시작 가능> <점수> <분> <맵>`(서버 `CLobbyClient::SendChanged` · 사람마다 · 달라질 때만)
//   받기 훅에서 값만 적고 쓰는 건 프레임 자리(`RoomUiTick`). 맵 칸은 `~` = 없음 · 그 밖은 영문 맵 열쇠라 `%XX` 를 안 푼다(`MapKeyOf` — 소문자 파일 이름)
enum { kRuGone = 0, kRuWaiting = 1, kRuPlaying = 2, kRuRankArriving = 3 };
struct RoomUi { bool valid; int state; bool host; bool canStart; int score; int timeMin; char map[32]; };
static CRITICAL_SECTION g_ruCs;
static RoomUi g_ru = {};
static void RoomUiOnLine(const char* tail)
{
    int state = 0, host = 0, canStart = 0, score = 0, timeMin = 0;
    char map[32] = {};
    const int got = sscanf(tail, "%d %d %d %d %d %31s", &state, &host, &canStart, &score, &timeMin, map);
    if (got != 6) { Log("㉒ 방 안 화면 줄을 못 읽음 — 칸 %d 개", got); return; }
    EnterCriticalSection(&g_ruCs);
    g_ru.valid = state != kRuGone;
    g_ru.state = state;
    g_ru.host = host != 0;
    g_ru.canStart = canStart != 0;
    g_ru.score = score;
    g_ru.timeMin = timeMin;
    strcpy_s(g_ru.map, map[0] == '~' ? "" : map);
    LeaveCriticalSection(&g_ruCs);
}

// ★ 11-04 플랜2 ㉔ 대전 기록 — 로비 줄 `NOXRANK REC...`(서버 `RecordDesk`) · 받기 훅에서 칸만 채우고 그리는 건 프레임 자리(`RecordTick`)
//   ★ 11-04 전적 검색 — `RECNONE <물은 이름>` = 없는 사용자. 서버는 접속당 한 물음이라 겹치면 뒤 물음을 말없이 버린다 → 앞 창에서 물은 답이 늦게 오면 이름을 보고 버린다(내 기록의 이름 = 로그인 `NICK` 글자 · 서버 `c.nick` 도 그 줄 그대로 · 못 읽어 빈 글자면 대조 안 함)
//   RECSUM <랭크 판 있음> <점수> <순위> <랭크 승> <랭크 패> <일반 승> <일반 패> <이름> · RECROW <결과> <랭크> <점수|-> <증감|-> <맵> <킬|-> <데스|-> <초> <끝 시각> · RECEND <줄 수> · RECFAIL
//   결과 = W 승 · L 패 · D 비김 · A 중단 · Q 이탈 · 숫자 = 그 등수. 값 판단은 서버(`MatchHistory.h`) · DLL 은 글자 · 색만
enum { kRecMaxRows = 20 };
struct RecRow { char res[8]; int ranked; char after[12]; char delta[12]; char map[40]; char kills[12]; char deaths[12]; unsigned sec; unsigned endUnix; };
struct RecData { int hasRanked, rating, rank, rw, rl, nw, nl; char name[33]; int n; RecRow rows[kRecMaxRows]; };
enum RecState { kRecWait = 0, kRecDone = 1, kRecFail = 2, kRecNoUser = 3 };
static RecData g_recIn = {};              // g_ruCs 안에서만
static char g_recAskName[33] = "";        // g_ruCs 안에서만 — 답에 실릴 이름(빈 글자 = 대조 안 함)
static bool g_recSkip = false;            // g_ruCs 안에서만 — 이름이 다른 RECSUM 뒤의 RECROW · RECEND 를 버리는 중
static volatile LONG g_recState = kRecWait;
static void RecOnLine(const char* sub)
{
    EnterCriticalSection(&g_ruCs);
    if (strcmp(sub, "RECFAIL") == 0) {
        InterlockedExchange(&g_recState, kRecFail);
    } else if (strncmp(sub, "RECNONE ", 8) == 0) {
        if (strcmp(sub + 8, g_recAskName) == 0) InterlockedExchange(&g_recState, kRecNoUser);
        else Log("㉔ 물은 이름과 다른 답 버림(없는 사용자) — 물음 「%s」 · 답 「%s」", g_recAskName, sub + 8);
    } else if (strncmp(sub, "RECSUM ", 7) == 0) {
        RecData in = {};
        const int got = sscanf(sub + 7, "%d %d %d %d %d %d %d %32s", &in.hasRanked, &in.rating, &in.rank, &in.rw, &in.rl, &in.nw, &in.nl, in.name);
        g_recSkip = got == 8 && g_recAskName[0] && strcmp(in.name, g_recAskName) != 0;
        if (g_recSkip) {
            Log("㉔ 물은 이름과 다른 답 버림 — 물음 「%s」 · 답 「%s」", g_recAskName, in.name);
        } else {
            g_recIn = in;
            InterlockedExchange(&g_recState, kRecWait);
            if (got != 8) {
                Log("㉔ 대전 기록 요약 줄을 못 읽음");
                InterlockedExchange(&g_recState, kRecFail);
            }
        }
    } else if (g_recSkip) {
        if (strncmp(sub, "RECEND ", 7) == 0) g_recSkip = false;
    } else if (strncmp(sub, "RECROW ", 7) == 0) {
        RecRow r = {};
        const int got = sscanf(sub + 7, "%7s %d %11s %11s %39s %11s %11s %u %u", r.res, &r.ranked, r.after, r.delta, r.map, r.kills, r.deaths, &r.sec, &r.endUnix);
        if (got == 9 && g_recIn.n < kRecMaxRows) g_recIn.rows[g_recIn.n++] = r;
        else { Log("㉔ 대전 기록 줄을 못 읽음 — 칸 %d 개 · 줄 %d", got, g_recIn.n); InterlockedExchange(&g_recState, kRecFail); }
    } else if (strncmp(sub, "RECEND ", 7) == 0) {
        const int want = atoi(sub + 7);
        if (g_recState == kRecWait) InterlockedExchange(&g_recState, want == g_recIn.n ? kRecDone : kRecFail);
        if (want != g_recIn.n) Log("㉔ 대전 기록 줄 수가 다름 — 알린 %d · 받은 %d", want, g_recIn.n);
    }
    LeaveCriticalSection(&g_ruCs);
}

static void RankOnLobbyLine(SOCKET s, const std::string& line)
{
    const std::string sub = line.substr(strlen(LobbyLineFilter::kRankMark));
    if (sub.compare(0, 3, "REC") == 0) {   // ★ 11-04 ㉔ — 한 번에 스무 줄 넘게 와 아래 줄 기록을 안 남긴다
        RecOnLine(sub.c_str());
        Detail("㉔ 로비 줄 「%s」", line.c_str());
        return;
    }
    if (sub.compare(0, 5, "FOUND") == 0) {
        const long sec = sub.size() > 6 ? strtol(sub.c_str() + 6, NULL, 10) : 0;   // `FOUND <초>` — 이상하면 10 그대로
        if (sec > 0 && sec <= 120) InterlockedExchange(&g_rankFoundSec, sec);
        g_rankSock = s;
        InterlockedExchange(&g_rankEvFound, 1);
    }
    else if (sub == "WAIT") InterlockedExchange(&g_rankEvWait, 1);
    else if (sub == "READY") InterlockedExchange(&g_rankEvReady, 1);
    else if (sub == "EXPIRED") InterlockedExchange(&g_rankEvExpired, 1);
    else if (sub == "ROOMS") InterlockedExchange(&g_roomsChanged, 1);   // ★ 11-03 ⑳ — 방 목록이 바뀜(`RoomsChangedTick`)
    else if (sub.compare(0, 7, "ROOMUI ") == 0) RoomUiOnLine(sub.c_str() + 7);   // ★ 11-04 ㉒
    else {
        const ranknotice::Line n = ranknotice::Parse(sub.c_str());   // ★ 11-05 플랜1 — 줄 서기 제한. 창은 ⑩ 자리에서
        if (n.kind == ranknotice::Kind::Banned) {
            InterlockedExchange(&g_lobbyBannedUntil, (LONG)n.value);
        } else if (n.kind != ranknotice::Kind::None) {
            InterlockedExchange(&g_rankNoticeValue, (LONG)n.value);
            InterlockedExchange(&g_rankEvNotice, (LONG)n.kind);
        }
    }
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
    InterlockedExchange(&g_rankToList, 1);   // ★ 11-02 — 취소 · 아니오 · 수락 한도 공통
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
    const DWORD cancel = LiveMsgBox(&box) ? ((PFN_findchild)kFindChild)(box, kDlgCancelId) : 0;
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

// ★ 11-05 플랜1 — ⑲ 의 목록 새로 받기가 도는 중인가. 공용 알림창은 하나뿐이라(`0x449590` — 있으면 그 창의 글만 바꿈) 이때 띄운 안내는
//   목록 답과 함께 닫힌다(실물 — 거절 뒤 안내가 안 보임 · 창이 없다고 읽히는 프레임에 띄워도 다음 프레임 LIST 뒤 사라짐). 값 = 답 3초(로비가 안 답해도 안내는 나가게 · 제 판단) ·
//   답 뒤 200ms(약 6프레임 · 정품이 받은 목록을 넣고 창을 닫을 시간 · 제 판단 — 실물 판으로 확인)
static const DWORD kRankListWaitMs = 3000, kRankListSettleMs = 200;
static bool RankListBusy()
{
    const LONG st = g_rankListState;
    if (st == 0) return false;
    if (GetTickCount() - g_rankListSinceMs < (st == 1 ? kRankListWaitMs : kRankListSettleMs)) return true;
    InterlockedExchange(&g_rankListState, 0);
    return false;
}

// ★ 11-05 플랜1 — 받기 훅(주 스레드 · 어느 창 함수도 호출 중이 아님 — `RankFoundClick` 주석). 갈래 넷:
//   줄 서기 전에 온 경고(기다림 중 · 줄은 섬) = 기다림 글 앞에 얹음 / 제한(기다림 중 · 줄 안 섬) = EXPIRED 처럼 정품 취소 길 뒤 확인 창 /
//   거절 · 수락 한도 뒤 = 확인 창(창이 닫혔으면 새로 · 수락 한도 안내가 떠 있으면 그 창의 글이 바뀜 — 창 함수가 같은 창을 다시 쓴다)
static void RankNoticeShow(ranknotice::Kind k, DWORD value)
{
    const std::wstring text = ranknotice::Text(k, value);
    wcsncpy_s(g_rankNoticeBuf, sizeof(g_rankNoticeBuf) / sizeof(g_rankNoticeBuf[0]), text.c_str(), _TRUNCATE);
    if (k == ranknotice::Kind::WarnMatch && g_rankWaiting) {
        swprintf(g_rankWaitNote, sizeof(g_rankWaitNote) / sizeof(g_rankWaitNote[0]), L"%ls\n", g_rankNoticeBuf);
        Log("⑬ 줄 서기 제한 — %s · 기다림 글 앞에 얹음(줄은 그대로)", ranknotice::KindName(k));
        return;
    }
    if (g_rankWaiting) RankInjectCancel("줄 서기 제한");
    RankDlg(g_rankNoticeBuf, kDlgNoticeFlags);
    const LONG n = InterlockedIncrement(&g_rankNoticeShown);
    Log("⑬ 줄 서기 제한 — %s 값 %lu · 안내 창(누적 %ld)", ranknotice::KindName(k), value, n);
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
            // ★ 11-04 — 잡혔다는 소리. 기다리는 동안 다른 창을 보고 있어도 알 수 있게(깜빡임은 사용자 결정으로 안 함)
            ((PFN_sound)kSoundPlay)(kSoundMatchFound, 100);
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
    bool expiredBox = false;   // ★ 11-05 플랜1
    if (InterlockedExchange(&g_rankEvExpired, 0)) {
        InterlockedExchange(&g_rankFoundState, 0);
        InterlockedIncrement(&g_rankExpiries);
        if (open && RankInjectCancel("EXPIRED(수락 한도)")) {
            RankDlg(kRankExpiredText, kDlgNoticeFlags);   // 받기 훅 자리라 누름 처리 한가운데가 아니다(`RankFoundClick` 주석) · 방 목록으로는 이 창이 닫힌 뒤(`RankBackToListTick`)
            Log("⑬ EXPIRED — 안내 창 「수락 시간이 지나...」(누적 %ld)", g_rankExpiries);
            expiredBox = true;
        }
    }
    // ★ 11-05 플랜1 — EXPIRED 뒤 — 같은 바퀴면 수락 한도 안내 위에 경고 · 제한 글이 남는다
    //   공용 알림창은 하나뿐이다 — 남의 창(⑲ 목록 새로 받기)이 떠 있거나 곧 뜰 때 글만 바꾸면 목록이 오는 순간 같이 닫혀 안 보인다(`RankListBusy`).
    //   그 일이 끝날 때까지 미룬다 · 내 창(기다림 · 찾음 · 방금 띄운 안내)은 그 자리에서
    if (g_rankEvNotice && (g_rankWaiting || expiredBox || (!MsgBoxOpen() && !RankListBusy()))) {
        const LONG notice = InterlockedExchange(&g_rankEvNotice, 0);
        if (notice) RankNoticeShow((ranknotice::Kind)notice, (DWORD)g_rankNoticeValue);
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

// ★ 11-03 ㉑ — 로비 연결이 끊기면 알린다. 정품은 기다림 창을 없애고 「불러오는 중...」만 남겨(`_out/qa_1103/q1/cut_end.png`) 사람은 까닭을 모른다
//   끊김은 받기 훅에서 표시만(`g_lobbyLost` — 목록 창이 보이거나 랭크를 기다리는 중일 때만 · 판 안에선 게임 서버가 살아 있어 안 건드림.
//   기다리는 동안은 목록 창이 숨어 있다 — `_out/qa_1103/e1` 「안 보임 · 안내 안 함」),
//   창은 1초 뒤 프레임 자리(`FrameTick`)에서 — 정품이 끊김을 받아 기다림 창을 닫는 일이 끝난 뒤. 소켓이 닫히면 로비 받기 훅은 더 안 불린다
static volatile LONG g_lobbyLost = 0;
static DWORD g_lobbyLostAtMs = 0;
static volatile LONG g_lobbyLostShown = 0;
static const wchar_t kLobbyLostText[] = L"서버와 연결이 끊겼습니다. 뒤로를 눌러 나간 뒤 다시 접속해 주세요";
static wchar_t g_lobbyLostBuf[64];   // ★ 11-05 플랜1 — 정지 문구(글 칸은 주소만 쥔다)
static void LobbyLostMark(int rc, DWORD err)
{
    if (!g_rankOn || !g_ourLobby || GetCurrentThreadId() != g_mainTid) return;
    if (rc > 0 || (rc < 0 && err == WSAEWOULDBLOCK)) return;
    DWORD world = 0, flags = 0;
    const bool shown = ReadSelf(kListWin, &world) && world && ReadSelf(world + 4, &flags) && !(flags & kWinHidden);
    const bool waiting = g_rankWaiting || g_rankFoundState;
    Log("㉑ 로비 연결 끊김 rc=%d err=%lu — 목록 창 %#lx %s · 랭크 기다림 %d — %s", rc, err, world, shown ? "보임" : "안 보임", waiting ? 1 : 0,
        shown || waiting ? "안내 창 예약" : "안내 안 함");
    if (shown || waiting) {
        g_lobbyLostAtMs = GetTickCount();
        InterlockedExchange(&g_lobbyLost, 1);
    } else {
        InterlockedExchange(&g_lobbyBannedUntil, 0);   // ★ 11-05 플랜1 — 판 안 등 안내 안 띄움(다음 온라인 누름의 정지 안내가 받음) — 묵은 값이 나중 끊김 안내로 새지 않게
    }
}
static bool BackToConnBusy();   // ★ 11-05 플랜1 — ⑯ 절
static void LobbyLostTick(const char* where)
{
    if (!g_lobbyLost || GetCurrentThreadId() != g_mainTid || GetTickCount() - g_lobbyLostAtMs < 1000) return;
    if (BackToConnBusy()) return;   // ★ 11-05 플랜1 — 메뉴를 돌려보내는 중에 띄우면 화면이 바뀌며 창이 풀린다(실물 — 정지 안내가 1초 만에 사라짐) · 접속 방식에 도착한 뒤에
    InterlockedExchange(&g_lobbyLost, 0);
    InterlockedExchange(&g_rankWaiting, 0);      // 줄은 로비가 끊김으로 이미 뺐다(`Lobby::OnClose`)
    InterlockedExchange(&g_rankFoundState, 0);
    // ★ 11-05 플랜1 — 정지로 끊겼으면(`NOXRANK BANNED`) 「다시 접속해 주세요」 대신 정지 문구 — 다시 눌러도 안 되는 일을 다시 하라고 하지 않게
    const uint32_t bannedUntil = (uint32_t)InterlockedExchange(&g_lobbyBannedUntil, 0);
    const wchar_t* text = kLobbyLostText;
    if (bannedUntil) {
        const std::wstring b = loginnotice::Text(loginnotice::Pick(false, bannedUntil), bannedUntil);
        wcsncpy_s(g_lobbyLostBuf, sizeof(g_lobbyLostBuf) / sizeof(g_lobbyLostBuf[0]), b.c_str(), _TRUNCATE);
        text = g_lobbyLostBuf;
    }
    ((PFN_dlgopen)kDlgOpen)(0, NULL, text, kDlgNoticeFlags, 0, 0);   // 부모 0 = 정품 「Timeout」 알림과 같은 꼴(`noxworld.c` 243)
    const LONG n = InterlockedIncrement(&g_lobbyLostShown);
    Log("㉑ 안내 창 띄움(누적 %ld · %s)", n, where);
}

static void NoticeBoxTick(const char* where)
{
    if (!g_rankOn || GetCurrentThreadId() != g_mainTid) return;
    if (!InterlockedExchange(&g_nbRan, 1)) Log("⑭ 점수 알림창 꾸밈 — 주 스레드 %s 훅에서 처음 돎", where);
    DWORD box = 0;
    if (!LiveMsgBox(&box)) return;
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
// ★ 11-06 플랜1 조각 6 ㉓ 게임 목록 — 정품 줄 함수(`0x43b340` · 부르는 자리 둘)가 칸마다 줄을 더한 **뒤에** 핑 칸 글을 맵 이름으로, 상태 칸 글을
//   「열림 · 닫힘」으로, 상태 칸 색을 초록 · 빨강으로 고친다. 줄 더하기 메시지의 색 인자는 색 표 번호라 초록 · 빨강을 못 고른다(대전 기록 창과 같은 까닭).
//   원본 소스의 `0x43B7C0` 은 실제 `Game.exe` 에서 함수 시작이 아니라 쓰면 안 된다. 다섯 칸 갈래(`[0x59a140]` ≠ 0)일 때만 · 우리 로비일 때만 고친다.
//   글자 칸 = 항목 배열 + 524×줄 + 4 · 색 = +516 · 다음 줄 번호 = 칸 자료 +46(`GAME3.c` AddLine · 원본 기준). 줄이 정확히 하나 는 걸 보고서만 쓴다 —
//   줄을 못 넣었는데 쓰면 앞 줄을 덮는다.
static const uintptr_t kListRowFn = 0x43b340, kListRowCallA = 0x43ad34, kListRowCallB = 0x49f685;
static const uintptr_t kListFiveCols = 0x59a140, kListColMap = 0x699048, kListColStatus = 0x69904c;   // 전역에 든 칸 창 — 맵(원래 핑) · 상태
static const uintptr_t kFilterIdx = 0x59a13c, kFilterModes = 0x710be8, kFilterVals = 0x710bf8;        // 필터 함수 `0x49c100` 의 칸 번호 · 모드 표 · 값 묶음(44바이트씩)
static volatile LONG g_listRowFixed = 0, g_listRowSkipped = 0;
typedef int(__cdecl* PFN_listrow)(DWORD srv);

static int ListNextRow(DWORD colSlot, DWORD* items)
{
    const DWORD col = *(volatile DWORD*)colSlot;
    DWORD data = 0, pos = 0;
    if (!col || !ReadSelf(col + 32, &data) || !data || !ReadSelf(data + 24, items) || !*items || !ReadSelf(data + 44, &pos)) return -1;
    return (short)(pos >> 16);   // +44 개수 · +46 다음 줄 번호
}

static int __cdecl ListRowHook(DWORD srv)
{
    const bool on = g_ourLobby && srv && *(volatile DWORD*)kListFiveCols != 0;
    DWORD mapItems = 0, stItems = 0;
    const int mapRow = on ? ListNextRow(kListColMap, &mapItems) : -1;
    const int stRow = on ? ListNextRow(kListColStatus, &stItems) : -1;
    const int r = ((PFN_listrow)kListRowFn)(srv);
    if (!on) return r;
    const bool added = mapRow >= 0 && stRow >= 0 && ListNextRow(kListColMap, &mapItems) == mapRow + 1 && ListNextRow(kListColStatus, &stItems) == stRow + 1;
    if (!added) {
        if (InterlockedIncrement(&g_listRowSkipped) == 1) Log("㉓ 목록 줄 — 줄이 하나 늘지 않아 안 고침(맵 칸 %d · 상태 칸 %d)", mapRow, stRow);
        return r;
    }
    const unsigned char flags = *(const unsigned char*)(srv + 0x64), players = *(const unsigned char*)(srv + 0x67), maxPlayers = *(const unsigned char*)(srv + 0x68);
    wchar_t map[16];
    const bool haveMap = gamelist::MapText((const char*)(srv + 111), map, _countof(map));
    if (haveMap) wcscpy_s((wchar_t*)(mapItems + 524 * (DWORD)mapRow + 4), 256, map);
    const gamelist::Status st = gamelist::StatusOf(flags, players, maxPlayers);
    wcscpy_s((wchar_t*)(stItems + 524 * (DWORD)stRow + 4), 256, gamelist::StatusText(st));
    const PFN_rgb rgb = (PFN_rgb)kColorRgb;
    *(volatile DWORD*)(stItems + 524 * (DWORD)stRow + 516) = st == gamelist::Status::Open ? rgb(60, 220, 60) : rgb(230, 70, 60);
    const LONG n = InterlockedIncrement(&g_listRowFixed);
    if (n <= 8) Log("㉓ 목록 줄 %ld — 깃발 %#x · %u/%u · 맵 %s · %s", n, flags, players, maxPlayers, haveMap ? (const char*)(srv + 111) : "(없음)", st == gamelist::Status::Open ? "열림" : "닫힘");
    return r;
}

// 판 중인 방(`access 1`)이 목록에서 사라지면 빨간 「닫힘」을 볼 수 없다. 프로세스에서 목록 창을 처음 볼 때 한 번만 정품 필터 값을 고친다 —
//   매 프레임 쓰지 않으므로 사람이 필터 창으로 다시 켤 수 있다. 줄에서 닫힘 표시를 지우는 길은 안 쓴다(정품 글 · 들어가기 거절이 그 표시를 본다).
static void ListFilterOnce()
{
    static bool done = false;
    if (done) return;
    done = true;
    const DWORD idx = *(volatile DWORD*)kFilterIdx;
    if (idx > 1) { Log("㉓ 필터 — 칸 번호 %lu 가 표 밖이라 안 고침", idx); return; }
    volatile DWORD* mode = (volatile DWORD*)(kFilterModes + 4 * idx);
    volatile DWORD* vals = (volatile DWORD*)(kFilterVals + 44 * idx);
    const DWORD before = *mode, closedBefore = vals[1];
    const gamelist::FilterFix fix = gamelist::FilterFixOf(before, closedBefore != 0);
    switch (fix) {
        case gamelist::FilterFix::ModeOff: *mode = 0; break;
        case gamelist::FilterFix::ClosedOff: vals[1] = 0; break;
        case gamelist::FilterFix::None: break;
    }
    Log("㉓ 필터 — 칸 %lu · 모드 %lu · 닫힌 방 거름 %lu → 모드 %lu · 닫힌 방 거름 %lu", idx, before, closedBefore, *mode, vals[1]);
}

static void RankUiTick()
{
    if (GetCurrentThreadId() != g_mainTid || !g_ourLobby || *(volatile DWORD*)kNetMode != 1) return;
    RankWaitText();   // ★ ⑪ ④ 기다림 창 글
    RankFoundTick();  // ★ 매칭 손보기 6 — ⑬ 로비 수락 줄 → 창
    const DWORD win = *(volatile DWORD*)kListWin;
    if (win) ListFilterOnce();   // ★ 11-06 플랜1 조각 6 ㉓
    if (!win) { InterlockedExchange(&g_roomsChanged, 0); return; }   // ★ 11-03 ⑳ — 이 화면 밖의 바뀜은 들어올 때 LIST 가 다 받는다
    RankMenuTick(win);   // ★ 매칭 손보기 8 — ⑫ 필터 창 감싸기 · 「리스트」 칸 숨김
    RankBackToListTick(win);   // ★ 11-02 — ⑲ 랭크 취소 · 거절 · 한도 뒤 방 목록으로
    RoomsChangedTick(win);     // ★ 11-03 — ⑳ 방 목록 바뀜 → 리스트 갱신
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
        if (rc <= 0) {   // ★ 11-03 ㉑
            const DWORD err = GetLastError();
            LobbyLostMark(rc, err);
            SetLastError(err);
        }
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

// ─────────────────────────────────────────────── ⑮ 런처 — 정품 로그인 창(wolapi.wnd) 감싸기 (런처 P5 · Docs/LAUNCHER_RESEARCH.md ②)
// 로그인 창을 여는 `0x44a0e0` 이 `call 0x49fde0("wolapi.wnd", 0x44a690)` 으로 창을 만든다(`0x44a144`). 그 call 을 DLL 로 돌려
//   처리 함수 인자만 DLL 함수로 바꿔 넘긴다 — 창이 처음부터 DLL 함수를 쥔다(만든 뒤 칸을 바꾸는 ⑩ 꼴과 달리 소켓 훅을 기다릴 필요 없음 ·
//   창 처리 함수는 게임 주 스레드의 GUI 가 부른다). 누름(0x4007 · 셋째 인자 = 눌린 단추 창 · 번호는 창 +0)만 보고 나머지는 원래 함수로
//   · 「로그인!」(0x6aa) — 런처로 안 켰으면(NOX_LAUNCH 없음) 원래 함수로 안 넘기고 공용 알림창에 안내(11-01 사용자 결정 4 · 문구 그대로).
//     서버도 암호 → 토큰 길을 닫았으므로(410) 이 막음은 친절의 몫 — 막는 것은 서버 · DLL 열쇠 길(④)
//   · 「등록」(0x6a9) — 정품은 WOL 등록 화면(상태 6)으로 감 → 우리 가입 페이지를 기본 브라우저로 열고 원래 함수로 안 넘김.
//     주소 = NOX_AUTH_URL(기준 주소 · 꾸러미) 또는 라우터 이름(servserv.westwood.com — hosts 가 우리 서버로) :8443/signup. 못 정하면 정품 그대로
//   알림창 = `0x449590`(부모, 제목, 글, 깃발, 확인 콜백, 취소 콜백 — OpenNox `NewDialogWindow` · 정품 「서버 접속 시도중...」 `0x43b1e7` 과 같은 함수).
//     깃발 0x21 = 확인 단추 + 누르면 닫힘(OpenNox gui.DialogOKButton | DialogFlag6). 확인 누름은 부모(이 창)로 4001 번이 다시 오는데
//     원래 함수는 0x6a7 · 0x6a9 · 0x6aa · 0x7c1 · 0xfaa 밖의 번호를 안 본다(`0x44a89d`~`0x44aa0f`) — 그대로 넘겨도 해가 없다
static const uintptr_t kWndLoad = 0x49fde0;          // (창 파일 이름, 처리 함수) → 창(정품 창 파일 읽어 만들기)
static const uintptr_t kLoginWndLoadCall = 0x44a144; // 로그인 창 만들기 call
static const uintptr_t kLoginProc = 0x44a690;        // 로그인 창 원래 처리 함수
static const uintptr_t kNewDialog = 0x449590;        // 공용 알림창 띄우기
static const DWORD kBtnLogin = 0x6aa, kBtnRegister = 0x6a9;
static const DWORD kDlgOkCloseFlags = 0x21;
static const wchar_t kLauncherTitle[] = L"녹스 온라인";
static const wchar_t kLauncherText[] = L"녹스 온라인에 접속하려면 런처로 실행하세요";   // 11-01 사용자 문구 그대로
static volatile LONG g_loginWndMade = 0;     // 감싼 채로 만든 로그인 창 수
static volatile LONG g_loginBlocked = 0;     // 「로그인!」 막고 안내
static volatile LONG g_loginPassed = 0;      // 「로그인!」 넘김(런처로 켬)
static volatile LONG g_loginSignup = 0;      // 「등록」 → 가입 페이지
static volatile LONG g_loginSignupMiss = 0;  // 「등록」인데 주소를 못 정해 정품 그대로

typedef DWORD(__cdecl* PFN_wndload)(DWORD file, DWORD proc);
typedef int(__cdecl* PFN_newdialog)(DWORD parent, const wchar_t* title, const wchar_t* text, DWORD flags, DWORD okCb, DWORD cancelCb);

// 가입 페이지 주소 — 못 정하면 빈 글
static std::wstring SignupUrl()
{
    wchar_t over[512];
    const DWORD en = GetEnvironmentVariableW(L"NOX_AUTH_URL", over, 512);
    if (en > 0 && en < 512) return authclient::WithPath(over, L"/signup");
    typedef int(WSAAPI * PFN_startup)(WORD, LPWSADATA);
    typedef int(WSAAPI * PFN_cleanup)(void);
    WSADATA wd;
    if (((PFN_startup)Resolve(115))(MAKEWORD(1, 1), &wd) != 0) return std::wstring();   // 정품이 이미 했어도 셈만 하나 늘었다 줄어든다
    struct hostent* h = ((PFN_gethostbyname)Resolve(52))("servserv.westwood.com");
    std::wstring url;
    if (h && h->h_addrtype == AF_INET && h->h_addr_list && h->h_addr_list[0]) {
        const unsigned char* b = (const unsigned char*)h->h_addr_list[0];
        wchar_t buf[64];
        swprintf(buf, 64, L"https://%u.%u.%u.%u:8443/signup", b[0], b[1], b[2], b[3]);
        url = buf;
    }
    ((PFN_cleanup)Resolve(116))();
    return url;
}

static int __cdecl LoginProc(DWORD win, DWORD msg, DWORD a3, DWORD a4)
{
    const PFN_wndproc4 orig = (PFN_wndproc4)kLoginProc;
    DWORD id = 0;
    if (msg != kMsgClick || !a3 || !ReadSelf(a3, &id)) return orig(win, msg, a3, a4);
    if (id == kBtnLogin) {
        if (LaunchName().empty()) {
            const LONG n = InterlockedIncrement(&g_loginBlocked);
            Log("⑮ 「로그인!」 막음(누적 %ld) — 런처로 안 켬(NOX_LAUNCH 없음) · 안내창", n);
            ((PFN_newdialog)kNewDialog)(win, kLauncherTitle, kLauncherText, kDlgOkCloseFlags, 0, 0);
            return 0;
        }
        InterlockedIncrement(&g_loginPassed);
    } else if (id == kBtnRegister) {
        const std::wstring url = SignupUrl();
        if (!url.empty()) {
            const HINSTANCE r = ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
            const LONG n = InterlockedIncrement(&g_loginSignup);
            Log("⑮ 「등록」 → 가입 페이지(누적 %ld) %ls · ShellExecute %ld", n, url.c_str(), (LONG)(INT_PTR)r);
            return 0;
        }
        const LONG n = InterlockedIncrement(&g_loginSignupMiss);
        Log("⑮ 「등록」 — 가입 주소를 못 정함(누적 %ld · NOX_AUTH_URL 없음 · servserv.westwood.com 못 풂) → 정품 등록 화면 그대로", n);
    }
    return orig(win, msg, a3, a4);
}

static DWORD __cdecl LoginWndLoad(DWORD file, DWORD proc)
{
    const bool ours = proc == (DWORD)kLoginProc;
    const DWORD win = ((PFN_wndload)kWndLoad)(file, ours ? (DWORD)(uintptr_t)LoginProc : proc);
    const LONG n = ours && win ? InterlockedIncrement(&g_loginWndMade) : g_loginWndMade;
    Log("⑮ 로그인 창 만듦(누적 %ld) 창=%#lx 처리 함수=%s · 런처 %s", n, win, ours ? "DLL" : "원래(인자가 0x44a690 아님)",
        LaunchName().empty() ? "아님" : "켬");
    return win;
}

// call 하나의 대상만 바꾼다(국가 칸 훅과 같은 꼴) — 그 자리가 정품 `call <want>` 일 때만
static bool HookCallSite(uintptr_t siteAddr, uintptr_t want, void* ours, const char* what)
{
    unsigned char* site = (unsigned char*)siteAddr;
    DWORD rel = 0;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || (DWORD)(uintptr_t)(site + 5) + rel != (DWORD)want) {
        Log("%s — %#lx 가 정품 `call %#lx` 이 아니라 안 붙임", what, (DWORD)siteAddr, (DWORD)want);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) { Log("%s — 보호 바꾸기 실패 err=%lu", what, GetLastError()); return false; }
    Put32(site + 1, (DWORD)(uintptr_t)ours - (DWORD)(uintptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("%s — %#lx 의 call 을 DLL 로 돌림", what, (DWORD)siteAddr);
    return true;
}

// ─────────────────────────────────────────────── ⑯ 런처 — 정품 화면 넷 자동 진행 (런처 P6 · P7 · Docs/LAUNCHER_RESEARCH.md ③ ④)
// 런처로 켰으면(NOX_LAUNCH — DLL 이 붙을 때 한 번 읽음) 메인 메뉴 → 접속 방식 → 로그인 → 서버 목록을 사람 손 없이 지나간다.
//   프레임 자리 — 「모든 창 그리기」(`0x489bf0` · 인자 없음 · 창 목록 `[0x6f73e8]` 을 돎)를 프레임 루프에서 부르는 call 둘(`0x43e29a` · `0x4aa99d`)을
//   DLL 로 돌려 그리기 직전에 LauncherTick. 이때는 어느 창 함수도 호출 중이 아니다(누른 단추가 창을 닫아도 그 창을 다시 읽는 정품 줄이 없음).
//   ✕ 11-01 파일럿 — PeekMessageA 가져오기 칸은 메뉴 동안 0번(정품 펌프 `0x444f30` · `0x556ff0` 가 안 돎) · timeGetTime 첫 자리 `0x416b2d` 는
//   아무 데서나 불리는 시계 · 그리기 칸(+0x17c)은 `0x489c80` 이 부른 뒤 같은 창을 계속 읽고 숨긴 창은 안 불러 — 셋 다 버림
//   화면마다 정품 전역 창 칸이 새 값이 되면 기다렸다가(kSettleTicks 프레임 + kSettleMs — 화면 바꾸기가 끝나기 전에 누르지 않게 · 미확인 3) 한 번 누른다.
//   누름 = `0x488da0(창, 0x4007, 단추 창, 0)`(칸 +0x178 처리 함수 부르기 — ⑪ ④ · ⑭ 와 같은 길). 화면마다 한 프로세스에 한 번만
//   (로그인이 실패해 로그인 창이 다시 떠도 또 누르지 않는다 — 되풀이 막음. 그다음은 사람이 누른다)
//   · 메인 메뉴 `[0x71132c]` 112 「멀티플레이」 · 접속 방식 `[0x711ca0]` 411 「인터넷」
//   · 로그인 `[0x69cbbc]` — 이름 칸 `[0x69cbc4]` 에 계정 이름 · 암호 칸 `[0x69cbc8]` 에 아무 글(0x401e · 넓은 글자 — 정품 `0x44a7fb` 꼴 ·
//     DLL 이 apgar 자리를 열쇠 토큰으로 바꾸므로 이 글은 어디에도 안 나감) → 1706 「로그인!」
//   · 서버 목록 `[0x69cb30]` — 목록 1961 이 딱 한 줄이면 그 줄을 고르고(0x4013 · 정품 `0x44a275` 꼴) 1962 「확인」. 둘 이상이면 손 안 댐
// 숨기지 않는다 — 숨긴 창은 그려지지 않고, 로그인이 실패하면 숨긴 창이 남아 사람이 못 쓴다. 화면이 잠깐 보였다 넘어간다(기능 먼저)
//   ★ 11-04 위 줄을 뒤집음 — 런처로 켠 판은 화면 넷을 첫 그리기 전에 숨긴 채 누른다(숨긴 창에도 누름이 먹음 · 11-04 파일럿 8판).
//     왜: 사용자가 런처에 로그인했는데 게임이 멀티플레이 → 온라인 → 로그인을 혼자 눌러 가는 모습이 어색함 · 대화방까지 약 4초(`Docs/MO_UX_FIX_PLAN_1104.md`).
//     갇힘 걱정은 실패 길에서 푼다(AutoRunWatch · BackToConnTick · AutoGiveUp — 숨김을 풀고 사람에게 넘김)
static const uintptr_t kMainMenuWin = 0x71132c, kConnWin = 0x711ca0, kLoginWin = 0x69cbbc, kServListWin = 0x69cb30;
static const uintptr_t kLoginNameEdit = 0x69cbc4, kLoginPassEdit = 0x69cbc8;
static const DWORD kBtnMulti = 112, kBtnInternet = 411, kServList = 0x7a9, kServOk = 0x7aa;
static const DWORD kMsgSetEditText = 0x401e, kMsgListSelect = 0x4013;
static const LONG kSettleTicks = 3;
static const DWORD kSettleMs = 300;   // 근거 없는 값 — 파일럿에서 줄이거나 늘린다
static std::string g_launchAtStart;   // DllMain 에서 한 번(NOX_LAUNCH) — 프레임마다 환경을 안 읽게
// 시험 스위치 NOX_TEST_LOGINBLOCK=1(런처 없이 켠 판에서만) — 메인 메뉴 · 접속 방식은 자동으로 넘기고 로그인 창에선 칸을 안 채우고 「로그인!」만 한 번
//   누른다 → ⑮ 가 막고 안내창을 띄우는지 사람 손 없이 본다(⑨ · ⑪ 시험 스위치와 같은 관례)
static bool g_testLoginBlock = false;
static volatile LONG g_ticks = 0;            // 프레임 자리가 돈 수(주 스레드)
static volatile LONG g_autoPress[4] = {};    // 화면마다 누름(메인 메뉴 · 접속 방식 · 로그인 · 서버 목록)
static volatile LONG g_autoSkip = 0;         // 서버 목록이 한 줄이 아니라 손 안 댐 · 단추 못 찾음

// ✕ 11-01 파일럿 — 메인 메뉴가 뜬 지 344ms 에 넣은 112 가 무시됨: 메뉴 처리 함수는 화면 기록 `[0x711338]+0x40` 이 켜져 있으면
//   누름을 버린다(`0x4a10f5`~`0x4a1110` — 화면이 움직이는 동안으로 짐작). 그래서 「한 번 누르고 끝」 대신 화면마다 성공 표시를 보고
//   아직이면 kRetryMs 마다 다시 누른다(최대 kMaxTries). 성공 표시 = 메인 메뉴 → 접속 방식 창이 뜸 · 접속 방식 → 로그인 창이 뜸 ·
//   로그인 → WOL 화면 상태가 1(로그인)이 아님 · 서버 목록 → 그 창이 사라지거나 바뀜
static const DWORD kRetryMs = 1500;
static const int kMaxTries = 5;
// ★ 11-04 런처로 켠 판 — 숨긴 채 넘어가므로 기다릴 까닭이 줄었다. 150ms · 60ms = 파일럿 값(이 값으로 8판 대화방까지 3.8~4.1초 · 메인 메뉴 · 접속 방식은 움직임이 끝날 때까지 몇 번 더 누른다).
//   40번 = 150ms × 40 = 6초 — 움직임이 길어도 넉넉하되 사람이 갇혔다고 느끼기 전에 포기
static const DWORD kFastRetryMs = 150;
static const int kFastMaxTries = 40;
static const DWORD kFastSettleMs = 60;
static const uintptr_t kWolStateSlot = 0x652f18;   // ⑨ kWolState 와 같은 칸(1 = 로그인 화면)

static bool PressChild(DWORD win, DWORD id);
// ★ 11-04 자동 넘김 동안 소리 막기 — 누름 하나마다 클릭 · 미끄러짐 소리가 나 부팅 약 3초 동안 20번 넘게 울렸다(11-04 실측 22~23번).
//   정품 소리 함수 `0x4528d0`(⑬ kSoundPlay) 입구 5바이트 `mov eax,[esp+4] · push esi`를 DLL 로 돌린다. 막는 동안은 그대로 돌아가고, 아니면 그 두 명령을 대신 하고 `0x4528d5` 로 이어 간다
static volatile LONG g_autoMute = 0;       // 주 스레드만 씀
static volatile LONG g_autoMuted = 0;      // 막은 수
static DWORD g_muteOffAt = 0;              // 이 시각이 지나면 푼다(켤 때 안전장치 시각 · 대화방에 들어오면 1초 뒤로 앞당김)
static const DWORD kMuteAfterChatMs = 1000;   // 대화방에 들어온 뒤 — 대화방이 내려오는 소리도 부팅 소리의 끝이라 같이 막는다(파일럿 값)
static const DWORD kMuteMaxMs = 20000;        // 안전장치 — 대화방에 못 가도 언제까지나 무음이지 않게(정상은 약 4초)
static const uintptr_t kSoundResume = 0x4528d5;
__declspec(naked) static void SoundStub()
{
    __asm {
        cmp g_autoMute, 0
        je pass
        lock inc g_autoMuted
        ret
    pass:
        mov eax, dword ptr [esp + 4]
        push esi
        mov ecx, kSoundResume
        jmp ecx
    }
}
static bool InstallSoundMute()
{
    static const unsigned char want[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
    unsigned char* site = (unsigned char*)kSoundPlay;
    if (memcmp(site, want, 5) != 0) {
        Log("⑯ 소리 막기 — %#lx 입구가 정품과 달라 안 붙임", (DWORD)kSoundPlay);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) { Log("⑯ 소리 막기 — 보호 바꾸기 실패 err=%lu", GetLastError()); return false; }
    site[0] = 0xE9;
    Put32(site + 1, (DWORD)(uintptr_t)&SoundStub - (DWORD)(uintptr_t)(site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("⑯ 소리 막기 — %#lx 입구를 DLL 로 돌림", (DWORD)kSoundPlay);
    return true;
}
// ★ 11-09 ㉕ — 자리 비움 관전 요청(bb) 막기. 정품 입력 처리(0x432210)는 입력 셈 [0x697b88] − 마지막 입력 [0x697b8c] 가
//   2700(0xa8c)을 넘으면 입력이 올 때까지 틱마다 서버 명령 0번(표식 F00D F0AD · 와이어 0xbb)을 보낸다(방장이면 0x443a20 으로 자기 안에서)
//   0x432262 jbe(76) → jmp(EB) 한 바이트로 그 검사를 늘 건너뜀 — 함수 입구를 막으면 앞의 입력 읽기(0x432010 · 0x432080)까지 죽는다
//   cmp ecx,0xa8c 부터 8바이트가 정품과 같을 때만 고침(다른 판에 엉뚱한 바이트를 쓰지 않게)
static const uintptr_t kIdleCmp = 0x43225c;
static bool InstallIdleObserverBlock()
{
    static const unsigned char want[8] = { 0x81, 0xF9, 0x8C, 0x0A, 0x00, 0x00, 0x76, 0x3B };
    unsigned char* site = (unsigned char*)kIdleCmp;
    if (memcmp(site, want, 8) != 0) {
        Log("㉕ 자리 비움 관전 요청 막기 — %#lx 가 정품과 달라 안 붙임", (DWORD)kIdleCmp);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(site + 6, 1, PAGE_EXECUTE_READWRITE, &old)) { Log("㉕ 자리 비움 관전 요청 막기 — 보호 바꾸기 실패 err=%lu", GetLastError()); return false; }
    site[6] = 0xEB;
    VirtualProtect(site + 6, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site + 6, 1);
    Log("㉕ 자리 비움 관전 요청 막기 — %#lx jbe → jmp (입력 없음 2700 검사 건너뜀)", (DWORD)(kIdleCmp + 6));
    return true;
}
static void AutoMuteSet(bool on, const char* why)
{
    if ((g_autoMute != 0) == on) return;
    InterlockedExchange(&g_autoMute, on ? 1 : 0);
    g_muteOffAt = on ? GetTickCount() + kMuteMaxMs : 0;
    Log("⑯ 소리 막기 %s — %s(막은 수 누적 %ld)", on ? "켬" : "끔", why, g_autoMuted);
}

// ★ 11-04 고지 화면(저작권) 건너뜀 — 창 칸 `[0x745d54]` · 타이머 word `[0x745d78]`. 정품은 타이머를 300(`0x4cb906`)에서 내리며 270 초과면 누름을 버린다(`0x4cb9b0` — 약 8초의 정체).
//   타이머를 270 으로 놓고 9901 을 누른다. 글은 런처 창 아래로 옮김(11-04 사용자 결정)
static const uintptr_t kLegalWin = 0x745d54, kLegalTimer = 0x745d78;
static const DWORD kBtnLegalOk = 9901;
static const unsigned short kLegalReady = 270;
static bool LegalSkipTick()
{
    static bool done = false;
    if (done) return false;
    DWORD legal = 0;
    if (!ReadSelf(kLegalWin, &legal) || !legal) return false;
    done = true;
    ((PFN_sethidden)kSetHidden)(legal, 1);
    *(volatile unsigned short*)kLegalTimer = kLegalReady;
    const bool ok = PressChild(legal, kBtnLegalOk);
    if (!ok) ((PFN_sethidden)kSetHidden)(legal, 0);   // 못 누르면 정품 고지 화면 그대로(사람이 누름)
    Log("⑯ 고지 화면 %#lx — %s", legal, ok ? "숨기고 건너뜀" : "단추를 못 찾아 그대로");
    return true;
}

struct AutoStep {
    const char* name;
    uintptr_t winSlot;
    DWORD seenWin;    // 지켜본 창(새 창이면 다시 셈)
    LONG sinceTick;
    DWORD sinceMs;
    int tries;        // 누른 수
    DWORD lastPressMs;
    bool done;
};
static AutoStep g_steps[4] = {
    { "메인 메뉴", kMainMenuWin, 0, 0, 0, 0, 0, false },
    { "접속 방식", kConnWin, 0, 0, 0, 0, 0, false },
    { "로그인", kLoginWin, 0, 0, 0, 0, 0, false },
    { "서버 목록", kServListWin, 0, 0, 0, 0, 0, false },
};

// 화면 i 의 누름이 먹었나(다음 화면으로 넘어갔나)
static bool AutoSucceeded(int i, DWORD win)
{
    DWORD v = 0;
    switch (i) {
        case 0: return ReadSelf(kConnWin, &v) && v;
        case 1: return ReadSelf(kLoginWin, &v) && v;
        case 2: return ReadSelf(kWolStateSlot, &v) && v != 1;
        case 3: return !ReadSelf(kServListWin, &v) || v != win;
    }
    return true;
}

static bool PressChild(DWORD win, DWORD id)
{
    const DWORD btn = ((PFN_findchild)kFindChild)(win, id);
    if (!btn) return false;
    ((PFN_wndproc4)kWinCall94)(win, kMsgClick, btn, 0);
    return true;
}

static bool SetEdit(uintptr_t slot, const wchar_t* text)
{
    DWORD edit = 0;
    if (!ReadSelf(slot, &edit) || !edit) return false;
    ((PFN_wndproc4)kWinCall94)(edit, kMsgSetEditText, (DWORD)(uintptr_t)text, 0);
    return true;
}

// 화면 i 의 누름 — 갈래 셋(눌렀음 · 단추 · 칸을 못 찾음 · 사람 몫이라 손 안 댐)
enum class AutoResult { Pressed, Missing, LeftAlone, NotYet };
static AutoResult Pressed(bool ok) { return ok ? AutoResult::Pressed : AutoResult::Missing; }
static AutoResult AutoAct(int i, DWORD win)
{
    switch (i) {
        case 0: return Pressed(PressChild(win, kBtnMulti));
        case 1: return Pressed(PressChild(win, kBtnInternet));
        case 2: {
            if (g_testLoginBlock) return PressChild(win, kBtnLogin) ? AutoResult::LeftAlone : AutoResult::Missing;   // 시험 — 한 번 누르고 끝(다시 안 누름)
            static wchar_t name[32];
            swprintf(name, 32, L"%S", g_launchAtStart.c_str());
            if (!SetEdit(kLoginNameEdit, name) || !SetEdit(kLoginPassEdit, L"launcher")) return AutoResult::Missing;
            return Pressed(PressChild(win, kBtnLogin));
        }
        case 3: {
            const DWORD list = ((PFN_findchild)kFindChild)(win, kServList);
            DWORD data = 0, count = 0;
            if (!list || !ReadSelf(list + 0x20, &data) || !ReadSelf(data + 0x2c, &count)) return AutoResult::Missing;   // 목록 줄 수 = 자료 +0x2c(⑨ 대화방 목록과 같은 목록 칸)
            const int rows = (int)(short)(count & 0xFFFF);
            if (rows == 0 && !g_launchAtStart.empty()) return AutoResult::NotYet;   // ★ 11-04 — 빠른 간격이면 줄이 오기 전에 볼 수 있다. 다음 차례에 다시
            if (rows != 1) {
                Log("⑯ 서버 목록 — 줄 %d 개라 손 안 댐(한 줄일 때만 고름)", rows);
                return AutoResult::LeftAlone;
            }
            ((PFN_wndproc4)kWinCall94)(list, kMsgListSelect, 0, 0);
            return Pressed(PressChild(win, kServOk));
        }
    }
    return AutoResult::LeftAlone;
}

// ★ 11-04 대화방 「뒤로」 · 실패 길 — 로그인 창을 사람에게 보이지 않고 접속 방식(온라인 고르는 화면)으로 돌려보낸다.
//   왜: 대화방 「뒤로」는 정품이 로그인 창을 다시 띄우는데(칸 `[0x69cbbc]` 가 0 → 새 창 · 11-04 칸 기록), 런처로 로그인한 판에서 이 창은 쓸 일이 없다.
//       그대로 자동 통과시키면 대화방에 갇힌다(「뒤로」를 눌러도 다시 대화방). 사용자 결정 = 온라인 고르는 화면으로.
//   누름 = 화면 공용 아래 줄 창 `[0x711328]`(OptsBack.wnd — `0x4a0d75` 에서 칸에 넣음)의 「뒤로」 152. 움직임 중엔 정품이 버리므로(`gui_main_menu.go` guiOptsBackProc · 원본 기준) 사라질 때까지 되풀이.
//   실패 길 = 로그인을 누른 뒤 ① 로그인 화면으로 돌아옴(취소 · 서버 거절) ② kAutoRunLimitMs 안에 대화방에 못 감 → 진행 창 `[0x6d5d94]`(wolProg.wnd — `0x467a85`)의 취소 1806 을 대신 눌러 ① 로 만듦.
//     ✕ 숨긴 로그인 창에 사람이 갇힌다(`:1961` 쪽 옛 걱정 · 11-04 라우터를 끈 판: 「소프트웨어 업데이트 확인...」 취소만 남고 WOLAPI 가 연결을 되풀이)
static const uintptr_t kChatWin = 0x69c8bc, kOptsBackWin = 0x711328, kWolProgWin = 0x6d5d94;
static const DWORD kBtnOptsBack = 152, kBtnProgCancel = 1806, kBtnProgOk = 1807;
static const DWORD kAutoRunLimitMs = 10000;   // 정상은 약 1초(11-04 로컬 · 다른 PC 는 미측정). 사람이 「안 되나 보다」 할 때쯤으로 어림 — 다른 PC 판에서 다시 볼 값
enum class BackWhy { None, Chat, Fail };
static BackWhy g_backWhy = BackWhy::None;     // 지금 로그인 창을 돌려보내는 중인가(주 스레드만)
static DWORD g_backLastMs = 0;
static int g_backTries = 0;
static bool g_chatReached = false;            // 대화방에 들어왔고 아직 로그인 창이 안 생김
static bool g_autoRunning = false;            // 로그인을 넘겼고 아직 대화방 전
static DWORD g_autoRunSince = 0;
static bool g_autoCancelPressed = false;
static volatile LONG g_chatBacks = 0, g_autoFails = 0, g_backGiveUps = 0;

static void AutoGiveUp(int i, DWORD win, const char* why)
{
    ((PFN_sethidden)kSetHidden)(win, 0);   // 숨긴 채 두면 사람이 못 쓴다
    AutoMuteSet(false, why);
    InterlockedIncrement(&g_autoSkip);
    Log("⑯ %s 창 %#lx — 숨김 풂 · 사람 몫(%s)", g_steps[i].name, win, why);
}

static bool ChatVisible()
{
    DWORD win = 0, flags = 0;
    return ReadSelf(kChatWin, &win) && win && ReadSelf(win + 4, &flags) && !(flags & kWinHidden);
}

static bool BackToConnBusy() { return g_backWhy != BackWhy::None; }   // ★ 11-05 플랜1 — `LobbyLostTick`

// 돌려보내는 중이면 true(이 프레임은 다른 자동 누름 안 함)
static bool BackToConnTick(DWORD now)
{
    DWORD login = 0, flags = 0;
    ReadSelf(kLoginWin, &login);
    if (g_chatReached && login) {
        g_chatReached = false;
        g_backWhy = BackWhy::Chat;
        g_backTries = 0;
        g_backLastMs = now - kFastRetryMs;
        InterlockedIncrement(&g_chatBacks);
        AutoMuteSet(true, "대화방 뒤로 · 접속 방식으로");
        Log("⑯ 대화방 뒤로 — 로그인 창 %#lx 를 숨기고 접속 방식으로", login);
    }
    if (g_backWhy == BackWhy::None) return false;
    if (!login) {   // 로그인 창이 사라짐 = 접속 방식으로 돌아감. 다음 온라인 누름에 다시 통과하게
        const BackWhy why = g_backWhy;
        g_backWhy = BackWhy::None;
        for (int i = 2; i < 4; ++i) { g_steps[i].done = false; g_steps[i].seenWin = 0; g_steps[i].tries = 0; }
        AutoMuteSet(false, "접속 방식 도착");
        Log("⑯ 접속 방식으로 돌아감(누름 %d 번 · 까닭 %s)", g_backTries, why == BackWhy::Fail ? "실패" : "대화방 뒤로");
        if (why == BackWhy::Fail) {
            const uint32_t bannedUntil = (uint32_t)g_loginBannedUntil;
            const loginnotice::Kind kind = loginnotice::Pick(g_loginKeyInvalid != 0, bannedUntil);
            Log("⑯ 실패 안내 — %s", loginnotice::KindName(kind));
            static std::wstring noticeText;   // 알림창이 닫힐 때까지 글을 살려 둔다
            noticeText = loginnotice::Text(kind, bannedUntil);
            ((PFN_dlgopen)kDlgOpen)(0, NULL, noticeText.c_str(), kDlgNoticeFlags, 0, 0);   // 부모 0 = ㉑ 안내 창과 같은 꼴
        }
        return false;
    }
    if (ReadSelf(login + 4, &flags) && !(flags & kWinHidden)) ((PFN_sethidden)kSetHidden)(login, 1);   // 첫 그리기 전에
    if (now - g_backLastMs < kFastRetryMs) return true;
    g_backLastMs = now;
    DWORD bar = 0;
    if (g_backTries >= kFastMaxTries || !ReadSelf(kOptsBackWin, &bar) || !bar || !PressChild(bar, kBtnOptsBack)) {
        g_backWhy = BackWhy::None;
        InterlockedIncrement(&g_backGiveUps);
        ((PFN_sethidden)kSetHidden)(login, 0);
        AutoMuteSet(false, "접속 방식으로 못 돌아감");
        Log("⑯ 접속 방식으로 못 돌아감(누름 %d 번) — 로그인 창 숨김 풂", g_backTries);
        return false;
    }
    ++g_backTries;
    return true;
}

// 로그인을 넘긴 뒤 대화방까지 지켜봄
static void AutoRunWatch(DWORD now)
{
    if (!g_chatReached && ChatVisible()) {
        g_chatReached = true;
        if (g_autoRunning) Log("⑯ 대화방 도착 — 로그인 넘긴 뒤 %lums", now - g_autoRunSince);
        g_autoRunning = false;
        if (g_autoMute) g_muteOffAt = now + kMuteAfterChatMs;
        return;
    }
    if (!g_autoRunning) return;
    DWORD login = 0, state = 0;
    if (ReadSelf(kLoginWin, &login) && login && ReadSelf(kWolStateSlot, &state) && state == 1) {   // 로그인 화면으로 돌아옴(취소 · 서버 거절 · 진행 창 확인)
        g_autoRunning = false;
        g_backWhy = BackWhy::Fail;
        g_backTries = 0;
        g_backLastMs = now - kFastRetryMs;
        InterlockedIncrement(&g_autoFails);
        Log("⑯ 대화방 못 감 — 로그인 화면으로 돌아옴(%lums 뒤) · 접속 방식 + 안내", now - g_autoRunSince);
        return;
    }
    if (!g_autoCancelPressed && now - g_autoRunSince > kAutoRunLimitMs) {
        g_autoCancelPressed = true;
        DWORD prog = 0, cancel = 0, cflags = 0;
        ReadSelf(kWolProgWin, &prog);
        if (prog && (cancel = ((PFN_findchild)kFindChild)(prog, kBtnProgCancel)) != 0 && ReadSelf(cancel + 4, &cflags)) {
            const DWORD id = (cflags & kWinHidden) ? kBtnProgOk : kBtnProgCancel;   // 진행 창은 끝나면 취소를 숨기고 확인을 보인다
            const bool ok = PressChild(prog, id);
            Log("⑯ 대화방 못 감 — %lums · 진행 창 %#lx 의 %lu 대신 누름(%s)", now - g_autoRunSince, prog, id, ok ? "함" : "못 찾음");
        } else {
            Log("⑯ 대화방 못 감 — %lums · 진행 창 없음(%#lx)", now - g_autoRunSince, prog);
        }
        AutoMuteSet(false, "대화방 못 감");
    }
}

static void LauncherTick()
{
    if (g_launchAtStart.empty() && !g_testLoginBlock) return;
    const LONG tick = g_ticks;
    const DWORD now = GetTickCount();
    const bool fast = !g_launchAtStart.empty();   // ★ 11-04 시험 스위치 NOX_TEST_LOGINBLOCK 판은 옛 꼴(보이는 채 · 옛 간격)
    if (fast) {
        if (g_autoMute && now - g_muteOffAt < 0x80000000u) AutoMuteSet(false, g_chatReached ? "대화방 1초 뒤" : "안전장치 20초");
        if (LegalSkipTick()) return;
        AutoRunWatch(now);
        if (BackToConnTick(now)) return;
    }
    const DWORD retryMs = fast ? kFastRetryMs : kRetryMs;
    const int maxTries = fast ? kFastMaxTries : kMaxTries;
    const DWORD settleMs = fast ? kFastSettleMs : kSettleMs;
    for (int i = 0; i < 4; ++i) {
        AutoStep& s = g_steps[i];
        if (s.done) continue;
        if (s.tries > 0 && AutoSucceeded(i, s.seenWin)) {
            s.done = true;
            Log("⑯ %s — 넘어감(누름 %d 번 · 마지막 누름 %lums 뒤)", s.name, s.tries, now - s.lastPressMs);
            if (fast && i == 2) {   // ★ 11-04 여기부터 대화방까지 지켜봄(AutoRunWatch)
                g_autoRunning = true;
                g_autoRunSince = now;
                g_autoCancelPressed = false;
            }
            continue;
        }
        DWORD win = 0, flags = 0;
        if (!ReadSelf(s.winSlot, &win) || !win || !ReadSelf(win + 4, &flags)) { s.seenWin = 0; continue; }
        if (fast) {   // ★ 11-04 첫 그리기 전에 숨김(이 틱은 그리기 바로 앞에서 돈다) · 숨긴 채 누른다
            if (!(flags & kWinHidden)) ((PFN_sethidden)kSetHidden)(win, 1);
        } else if (flags & kWinHidden) { s.seenWin = 0; continue; }
        if (win != s.seenWin) {
            s.seenWin = win;
            s.sinceTick = tick;
            s.sinceMs = now;
            s.tries = 0;
            if (fast && i == 2) AutoMuteSet(true, "온라인 · 대화방까지");   // ★ 11-04 부팅은 이미 켜져 있음 · 사람이 온라인을 다시 눌렀을 때
            continue;
        }
        if (s.tries == 0) {
            if (tick - s.sinceTick < kSettleTicks || now - s.sinceMs < settleMs) continue;
        } else {
            if (now - s.lastPressMs < retryMs) continue;
            if (s.tries >= maxTries) {
                s.done = true;
                if (fast) AutoGiveUp(i, win, "눌러도 안 넘어감"); else InterlockedIncrement(&g_autoSkip);
                Log("⑯ %s 창 %#lx — %d 번 눌러도 안 넘어감 · 그만(사람이 누름)", s.name, win, s.tries);
                continue;
            }
        }
        const AutoResult r = AutoAct(i, win);
        s.lastPressMs = now;
        switch (r) {
            case AutoResult::NotYet:
                return;   // 누른 수에 안 넣는다(다음 차례에 다시 봄)
            case AutoResult::Pressed:
                ++s.tries;
                if (s.tries == 1) InterlockedIncrement(&g_autoPress[i]);
                break;
            case AutoResult::Missing:
            case AutoResult::LeftAlone:
                ++s.tries;
                s.done = true;   // 단추 · 칸을 못 찾음 · 서버 목록이 한 줄이 아님 — 사람 몫
                if (fast) AutoGiveUp(i, win, r == AutoResult::Missing ? "단추 · 칸 못 찾음" : "서버 목록이 한 줄이 아님"); else InterlockedIncrement(&g_autoSkip);
                break;
        }
        Log("⑯ %s 창 %#lx — %s %d 번째(창 뜬 뒤 프레임 %ld · %lums)", s.name, win,
            r == AutoResult::Pressed ? "자동 누름" : r == AutoResult::LeftAlone ? "손 안 댐(사람 몫)" : "단추 · 칸을 못 찾아 손 안 댐", s.tries, tick - s.sinceTick,
            now - s.sinceMs);
        return;   // 한 프레임에 한 화면만
    }
}

// ─────────────────────────────────────────────── ⑰ 시험 — 파일로 받은 단추 누름 (사람 확인 대신 돌리는 판 · 11-02)
// 시험 스위치 NOX_TEST_PRESS=<파일>(판 확인 통과일 때만 · ⑨ · ⑪ 시험 스위치와 같은 관례). 없으면 아무 일도 안 한다.
//   바깥 시험 도구가 파일 끝에 한 줄씩 더하면 프레임 자리(주 스레드 · 어느 창 함수도 호출 중이 아님 — ⑯ 과 같은 자리)에서 차례로 실행한다
//   `p <창 칸 16진> <단추 번호>`         그 창의 단추 누름(0x4007) — 정품 단추가 부모에게 보내는 것과 같은 꼴(⑯ PressChild)
//   `s <창 칸 16진> <목록 번호> <줄>`    목록 줄 고름(0x4013 — ⑯ 서버 목록과 같은 꼴)
//   왜 — 마우스 주입은 커서 배율 같은 게임과 무관한 문제를 만든다(`CLAUDE.md`). 대신 마우스 좌표 → 단추 판정은 이 길로 못 본다
static std::string g_testPressFile;
static long g_testPressDone = 0;       // 파일에서 처리한 바이트
static DWORD g_testPressLastMs = 0;
static volatile LONG g_testPressOk = 0, g_testPressMiss = 0;

// ★ 11-04 플랜1 ㉒ 시작 단추 — 일반 방의 방장 · 대기 중에만 보이는 정품 창(`window\\MoStart.wnd` — `tools/ui_apply.ps1` 이 넣음). 누르면 채팅 `!start`
//   왜 창 — 서버 명령을 사람이 외우지 않게. 조건 판정은 서버(`ROOMUI` 의 방장 · 시작 가능) · 서버가 마지막에 또 거른다(`RoomTick.cpp` HandleChatCommands)
//   정품 한계 우회: 창 파일 머리 좌표가 판(640) 밖이면 안 그려져 파일은 0 0 + 런타임 `kWinSetPos`(11-04 시험 t3 · t4) · 판 나갈 때 정품이 모든 창을 지워 이벤트 2 로 칸을 비움 ·
//     맵이 바뀌어도 창은 그대로(시험 t4 so_lod → estate → so_lod) · 칸 값은 Home 창이 열리면 그 아래로 옮겨 겹치지 않음
static const uintptr_t kChatSendFn = 0x486fd0;   // 정품 채팅 보내기(`MSG_TEXT_MESSAGE` 를 채우는 자리 0x487016 으로 찾음 · 11-04 실물)
static const uintptr_t kHomeWin = 0x6d1870;      // Home(방 설정) 창 — 열리면 주소 · 닫히면 0
static const DWORD kStartBtnId = 9501, kWinEnabled = 0x8;
static const int kStartBtnW = 140, kStartBtnH = 40;
typedef DWORD(__cdecl* PFN_moWndLoad)(const char*, void*);
typedef void(__cdecl* PFN_chatSend)(const wchar_t*, int);
static DWORD g_startWin = 0;
static bool g_startLoadFailed = false;
static int __cdecl StartWinProc(DWORD win, DWORD ev, DWORD a1, DWORD)
{
    if (ev == kMsgClick) {
        Log("㉒ 시작 단추 누름 → !start");
        ((PFN_chatSend)kChatSendFn)(L"!start", 0);   // 서버는 `!` 줄을 퍼뜨리지 않는다(`Server.cpp`) — 화면에 안 뜸(시험 t3)
        return 1;
    }
    if (ev == 2 && win == g_startWin) { Log("㉒ 시작 단추 창 지워짐"); g_startWin = 0; }
    (void)a1;
    return 0;
}
// 자리 — Home 창이 닫혔으면 오른쪽 위 맨 위 · 열렸으면 그 바로 아래(사용자 고른 시안 `_out/qa_1104/start_pos/button_follow_compare.png`)
static void StartBtnPlace()
{
    static int lastX = -1, lastY = -1;
    const int scrW = *(volatile int*)kScreenW;
    int x = scrW - 255, y = 10;   // Home 창(폭 350 · 오른쪽 끝에서 10 — `guiserv.c` setPos) 가운데와 같은 줄 — 열고 닫을 때 가로로 안 움직인다
    DWORD home = 0;
    if (ReadSelf(kHomeWin, &home) && home) {
        int hx = 0, hy = 0, hw = 0, hh = 0;
        ReadSelf(home + 8, (DWORD*)&hw);
        ReadSelf(home + 0xc, (DWORD*)&hh);
        ReadSelf(home + 0x10, (DWORD*)&hx);
        ReadSelf(home + 0x14, (DWORD*)&hy);
        x = hx + (hw - kStartBtnW) / 2;
        y = hy + hh + 6;
    }
    if (x == lastX && y == lastY) return;
    lastX = x; lastY = y;
    ((PFN_setpos)kWinSetPos)(g_startWin, x, y);
    Log("㉒ 시작 단추 자리 %d,%d(Home 창 %s · 화면 폭 %d)", x, y, home ? "열림" : "닫힘", scrW);
    if (home) { DWORD r[4] = {}; for (int i = 0; i < 4; i++) ReadSelf(home + 8 + 4 * i, &r[i]); Log("㉒ Home 창 +8.. = %lu %lu %lu %lu", r[0], r[1], r[2], r[3]); }
}
static void StartBtnTick(bool inRoom, const RoomUi& s)
{
    const bool want = inRoom && s.valid && s.host && s.state == kRuWaiting;
    if (want && !g_startWin && !g_startLoadFailed) {
        g_startWin = ((PFN_moWndLoad)kWndLoad)("MoStart.wnd", (void*)StartWinProc);
        if (!g_startWin) { g_startLoadFailed = true; Log("㉒ 시작 단추 창을 못 만듦 — 게임 폴더에 window\\MoStart.wnd 가 없음(ui_apply)"); return; }
        Log("㉒ 시작 단추 창 만듦 %#lx", g_startWin);
    }
    if (!g_startWin) return;
    DWORD f = 0;
    if (!ReadSelf(g_startWin + 4, &f)) return;
    const bool hidden = (f & kWinHidden) != 0;
    if (hidden == want) {
        ((PFN_sethidden)kSetHidden)(g_startWin, want ? 0 : 1);
        Log("㉒ 시작 단추 %s(상태 %d · 방장 %d)", want ? "보임" : "숨김", s.state, s.host ? 1 : 0);
    }
    if (!want) return;
    StartBtnPlace();
    const DWORD btn = ((PFN_findchild)kFindChild)(g_startWin, kStartBtnId);
    DWORD bf = 0;
    if (btn && ReadSelf(btn + 4, &bf) && ((bf & kWinEnabled) != 0) != s.canStart) {
        *(volatile DWORD*)(btn + 4) = s.canStart ? (bf | kWinEnabled) : (bf & ~kWinEnabled);   // 정품은 이 비트로 그림을 고르고 누름을 안 받는다(`DISABLEDIMAGE`)
        Log("㉒ 시작 단추 %s", s.canStart ? "켬" : "회색");
    }
}
// ★ 11-04 플랜1 ㉓ Home(방 설정) 창 — 창은 남기되(사용자 결정 · 확장 여지) 못 바꾸게 회색으로 · 칸에는 실제 판 값
//   왜 — 정품 창의 판수 · 시간 · 맵은 클라에만 남고 서버로 안 간다(`hm2` 측정) · 그대로 두면 바꾼 줄 알게 된다
//   값은 칸에 글자만 넣는다 — 정품이 칸을 떠날 때 받는 0x4003 을 안 보내 클라 쪽 값도 그대로. 맵 줄 고르기(0x4013)는 알림 없이 표시만 바꾼다(`GAME3.c:700` · 원본 기준)
//   정품 한계 우회: 창을 닫으면 지워지고 다시 만들어져 한 번만 하면 안 된다 → 주소가 바뀔 때마다 · 끈 칸은 그림만 바뀌고 맵 목록 · 탭은 모양이 그대로라 눌림만 막힘(`client/gui/window.go:286` · 원본 기준)
//   확인의 윗칸까지 끄면 누름이 거기 안 닿아(같은 곳 — 켜진 칸만 따라 내려감) 확인이 든 칸은 남긴다
static const DWORD kHomeOkId = 10149, kHomeScoreId = 10134, kHomeTimeId = 10135, kHomeMapListId = 10114;
static const DWORD kMsgListTop = 0x401c, kMsgListText = 0x4016;
static const uintptr_t kHomeToggleFn = 0x457050;   // 정품 Home 키가 부르는 자리(열림 ↔ 닫힘 · 시험 누름 `H` 전용 — `guiserv.c` guiServerOptsLoad · 0x6d1870 에 저장해 찾음)
// ★ 11-07 판 시간 제한 T3 — 점수판 단계(F9 자리) · 시험 누름 `Y`(넘김) · `y`(읽기만) 전용. 파일럿 11-07 DLL 기록에서 0 → 넘김 → 2 → 3 → 4 → 5 → 0 을 봄
//   (원본 기준 넘김 = `sub_4703F0` · 단계 = `dword_5d4594_1090120` · 퀘스트가 아니면 1 을 건너뜀 `gameloop.go:740-753` — 정품 주소는 이 판의 것)
static const uintptr_t kRankStageFn = 0x483bd0, kRankStage = 0x6f6b94;
static bool HomeGrayWalk(DWORD win, int depth, int& off)
{
    bool hasOk = false;
    if (depth > 8) return false;
    DWORD c = 0;
    ReadSelf(win + 400, &c);
    for (int guard = 0; c && guard < 400; ++guard) {
        DWORD id = 0, f = 0, next = 0;
        ReadSelf(c, &id);
        ReadSelf(c + 4, &f);
        const bool sub = HomeGrayWalk(c, depth + 1, off);
        if (id == kHomeOkId || sub) hasOk = true;
        else if (f & kWinEnabled) { *(volatile DWORD*)(c + 4) = f & ~kWinEnabled; ++off; }
        if (!ReadSelf(c + 388, &next)) break;
        c = next;
    }
    return hasOk;
}
static void HomeFieldNumber(DWORD home, DWORD id, int value)
{
    const DWORD field = ((PFN_findchild)kFindChild)(home, id);
    if (!field) { Log("㉓ Home 칸 %lu 못 찾음", id); return; }
    wchar_t text[16];
    swprintf_s(text, L"%d", value);
    ((PFN_wndproc4)kWinCall94)(field, kMsgSetEditText, (DWORD)(uintptr_t)text, 0);
}
// 줄 글자 = 맵 이름 + 탭 + 사람 수 → 이름이 같은 줄을 찾는다(`guiserv.c` updateMapList)
static int HomeMapSelect(DWORD home, const char* map)
{
    const DWORD list = ((PFN_findchild)kFindChild)(home, kHomeMapListId);
    if (!list) return -2;
    const size_t n = strlen(map);
    for (int row = 0; row < 512; ++row) {
        const wchar_t* line = (const wchar_t*)(uintptr_t)((PFN_wndproc4)kWinCall94)(list, kMsgListText, (DWORD)row, 0);
        if (!line || !line[0]) break;
        size_t i = 0;
        while (i < n && line[i] && towlower(line[i]) == (wchar_t)tolower((unsigned char)map[i])) ++i;
        if (i == n && (line[i] == 0 || line[i] == L'\t' || line[i] == L' ')) {
            ((PFN_wndproc4)kWinCall94)(list, kMsgListSelect, (DWORD)row, 0);
            ((PFN_wndproc4)kWinCall94)(list, kMsgListTop, (DWORD)row, 0);
            return row;
        }
    }
    return -1;
}
static void HomeWinTick(bool inRoom, const RoomUi& s)
{
    static DWORD grayed = 0, filled = 0;
    DWORD home = 0;
    ReadSelf(kHomeWin, &home);
    if (!home) { grayed = filled = 0; return; }
    if (!inRoom) return;   // 방 밖에서 정품이 쓰는 경우(LAN 호스트 흐름)는 손대지 않는다
    if (home != filled && s.valid) {   // 줄이 창보다 늦어도 그때 채운다
        filled = home;
        HomeFieldNumber(home, kHomeScoreId, s.score);
        HomeFieldNumber(home, kHomeTimeId, s.timeMin);
        const int row = s.map[0] ? HomeMapSelect(home, s.map) : -3;
        Log("㉓ Home 창 값 — 판수 %d · 시간 %d · 맵 %s 줄 %d", s.score, s.timeMin, s.map, row);
    }
    if (home != grayed) {   // 값을 넣은 뒤에 끈다(칸은 켜져 있어야 글자를 받는 경로를 그대로 — `hm2`)
        grayed = home;
        int off = 0;
        HomeGrayWalk(home, 0, off);
        Log("㉓ Home 창 %#lx 회색 칸 %d", home, off);
    }
}

// 방 안 = 게임 목록 · 대화방 칸이 다 비었을 때. 나가면 목록이나 대화방이 다시 선다 — 그때 값을 지운다(다음 방의 줄이 올 때까지 옛 값을 안 쓰게)
//   들어갈 때는 줄이 목록이 닫히기 전에 올 수 있어(`_out/qa_p12` 로그) 방 안 → 밖으로 바뀔 때만 지운다
// ★ 11-04 플랜2 ㉔ 대전 기록 창(`window\MoRecord.wnd` — `tools/ui_apply.ps1` 이 넣음). 입구 = 대화방 1927(옛 「랭킹 확인」 자리)
//   왜 게임 안 창 — 런처는 게임 중 숨겨진다(`launcher_main.cpp:125`). 값 판단은 서버 · 여기는 받은 줄을 글자 · 색으로만
//   왜 열마다 목록 칸 — 정품 목록은 줄 하나에 색 하나라 칸이 줄을 나눠 가지면 한 줄 안에서 색이 갈린다(정품 게임 목록 `noxworld.wnd` 10038~10042 와 같은 짜임 — 11-04 시험 캡처)
//   정품 한계 우회: 대화방 처리 함수에 1927 칸을 다루는 길이 없어 ⑩ 처럼 처리 함수 칸을 감싼다 · 열려 있는 동안 대화방을 숨겨 그 아래 칸이 눌리지 않게 · 대화방이 없어지면(끊김) 같이 닫음
// ★ 11-04 전적 검색 — 입구 = 대화방 1902(옛 「찾기」 · 정품 찾기 창은 더 안 연다) · 창 = `window\MoSearch.wnd`(대전 기록 창과 자식 번호가 같고 왼쪽 칸에 이름 칸 · 검색 단추만 더함 · `tools/ui_apply.ps1`).
//   표 · 줄 · 판정은 대전 기록 것 그대로 — 두 창은 동시에 안 열리므로(둘 다 대화방을 숨김) 그리기는 「지금 열린 창」 하나만 본다
//   정품 한계 우회: 입력 칸은 Enter(0x401f)를 칸 +0x34 가 가리키는 곳으로만 보낸다(`GAME2_2.c` 키 처리 · 원본 기준). 칸이 보이려면 왼쪽 칸 9602 의 자식이어야 해서(맨 위 창의 자식은 9602 에 가려짐 · 11-04 r17 캡처)
//   창 파일 처리 함수(맨 위 창)에 안 온다 → 대화방과 같은 방식으로 9602 처리 함수 칸을 감싼다
static const DWORD kRecBtnId = 1927, kRecBackId = 9690, kRecMsgId = 9601, kRecColFirst = 9610, kRecValFirst = 9630;
static const DWORD kSearchBtnId = 1902, kSearchPanelId = 9602, kSearchEntryId = 9650, kSearchGoId = 9651;
static const DWORD kMsgEntrySet = 0x401e, kMsgEntryEnter = 0x401f;   // 입력 칸 — 글자 넣기 · Enter 알림(`GAME2_2.c` 16414 · 16415 · 원본 기준 · 알림은 11-04 r17 실측)
static const DWORD kEntryNotifySlot = 0x34, kEntryDataSlot = 0x20;   // 알림 받을 곳 · 글자 칸(11-04 r17 실측 — 글자는 칸 맨 앞 UTF-16)
// ★ 11-04 전적 검색 조각 4 — 키는 포커스 칸으로 간다 · 대화방을 숨겨도 포커스는 안 옮겨 간다(r18 — 숨은 칸이 계속 키를 받음) → 창을 열 때 저장해 옮기고 닫을 때 되돌린다
static const uintptr_t kFocusSlot = 0x6f73f8, kFocusFn = 0x488e10;   // 지금 포커스 칸 · 포커스 함수 — 인자 0 = 빈 포커스(원본 `GUI.Focus` · 이 exe 는 조각 4 파일럿 로그)
static const DWORD kMsgListAdd = 16397, kMsgListClear = 16399;
static const DWORD kRecTimeoutMs = 5000;
enum { kRecCols = 8, kRecVals = 5 };
enum RecKind { kRecRecord = 0, kRecSearch = 1, kRecKinds };
static const char* const kRecFile[kRecKinds] = { "MoRecord.wnd", "MoSearch.wnd" };
static const char* const kRecName[kRecKinds] = { "대전 기록", "전적 검색" };
static struct {
    DWORD win[kRecKinds];         // 만든 창(지워지면 0)
    bool loadFailed[kRecKinds];
    RecKind kind;                 // 지금 열린 창
    bool open;
    bool drawn;                   // 답을 기다리지 않음(그림 · 안내를 넣음 · 전적 검색의 처음 화면)
    DWORD askedAt;
    DWORD chatOrigProc, panelOrigProc;
    DWORD savedFocus;             // 창을 열기 전 포커스 칸
    wchar_t val[kRecVals][48];    // 정품 글 칸은 글을 복사하지 않고 쥔다(㉑ 안내 글과 같음) — 전역
    wchar_t msg[64];
} g_rec = {};
static DWORD RecWin() { return g_rec.win[g_rec.kind]; }

static void RecSetMessage(const wchar_t* text)
{
    const DWORD w = ((PFN_findchild)kFindChild)(RecWin(), kRecMsgId);
    if (!w) return;
    wcsncpy(g_rec.msg, text ? text : L"", _countof(g_rec.msg) - 1);
    ((PFN_wndproc4)kWinCall94)(w, kMsgSetText, (DWORD)(uintptr_t)g_rec.msg, (DWORD)-1);
    ((PFN_sethidden)kSetHidden)(w, text ? 0 : 1);
}
static void RecSetVal(int i, const wchar_t* text)
{
    const DWORD w = ((PFN_findchild)kFindChild)(RecWin(), kRecValFirst + (DWORD)i);
    if (!w) return;
    wcsncpy(g_rec.val[i], text, _countof(g_rec.val[i]) - 1);
    ((PFN_wndproc4)kWinCall94)(w, kMsgSetText, (DWORD)(uintptr_t)g_rec.val[i], (DWORD)-1);
}
static void RecClearAll()
{
    for (int c = 0; c < kRecCols; ++c) {
        const DWORD w = ((PFN_findchild)kFindChild)(RecWin(), kRecColFirst + (DWORD)c);
        if (w) ((PFN_wndproc4)kWinCall94)(w, kMsgListClear, 0, 0);
    }
    for (int i = 0; i < kRecVals; ++i) RecSetVal(i, L"");
}
static void RecDraw(const RecData& d)
{
    const PFN_rgb rgb = (PFN_rgb)kColorRgb;
    const DWORD gold = rgb(240, 180, 42), green = rgb(60, 220, 60), red = rgb(230, 70, 60), gray = rgb(150, 150, 150);
    wchar_t t[64];
    MultiByteToWideChar(949, 0, d.name, -1, t, _countof(t));
    RecSetVal(0, t);
    swprintf_s(t, L"%d", d.rating);
    RecSetVal(1, t);
    if (d.hasRanked && d.rank > 0) swprintf_s(t, L"%d위", d.rank); else wcscpy_s(t, L"-");
    RecSetVal(2, t);
    const auto wl = [&t](int w, int l) {
        if (w + l > 0) swprintf_s(t, L"%d승 %d패 %d%%", w, l, (w * 100 + (w + l) / 2) / (w + l));
        else wcscpy_s(t, L"-");
    };
    wl(d.rw, d.rl); RecSetVal(3, t);
    wl(d.nw, d.nl); RecSetVal(4, t);
    DWORD col[kRecCols];
    for (int c = 0; c < kRecCols; ++c) col[c] = ((PFN_findchild)kFindChild)(RecWin(), kRecColFirst + (DWORD)c);
    int rowNow = 0;
    // 정품 줄 더하기의 색 인자는 색이 아니라 색 표 번호(0~16 · 그 밖 = 칸 글자색 — `GAME3.c` AddLine · 원본 기준)라 초록 · 빨강을 고를 수 없다 → 더한 줄의 색 칸(+516)에 직접 넣는다
    const auto add = [&col, &rowNow](int c, const wchar_t* s, DWORD color) {
        if (!col[c]) return;
        ((PFN_wndproc4)kWinCall94)(col[c], kMsgListAdd, (DWORD)(uintptr_t)s, (DWORD)-1);
        DWORD data = 0, items = 0;
        if (ReadSelf(col[c] + 32, &data) && data && ReadSelf(data + 24, &items) && items) *(volatile DWORD*)(items + 524 * (DWORD)rowNow + 516) = color;
    };
    for (int i = 0; i < d.n; ++i) {
        rowNow = i;
        const RecRow& r = d.rows[i];
        const wchar_t* res = L"";
        DWORD resColor = gray;
        switch (r.res[0]) {
            case 'W': res = L"승"; resColor = green; break;
            case 'L': res = L"패"; resColor = red; break;
            case 'D': res = L"비김"; break;
            case 'A': res = L"중단"; break;
            case 'Q': res = L"이탈"; break;
            default: swprintf_s(t, L"%S등", r.res); res = t; resColor = gold; break;
        }
        add(0, res, resColor);
        add(1, r.ranked ? L"랭크" : L"일반", r.ranked ? gold : gray);
        MultiByteToWideChar(CP_ACP, 0, r.after, -1, t, _countof(t));
        add(2, t, r.after[0] == '-' ? gray : gold);
        MultiByteToWideChar(CP_ACP, 0, r.delta, -1, t, _countof(t));
        add(3, t, r.delta[0] == '+' ? green : (r.delta[0] == '-' && r.delta[1]) ? red : gray);
        MultiByteToWideChar(CP_ACP, 0, r.map, -1, t, _countof(t));
        t[0] = towupper(t[0]);
        add(4, t, gold);
        if (r.kills[0] == '-' && r.deaths[0] == '-') wcscpy_s(t, L"-");
        else swprintf_s(t, L"%S/%S", r.kills, r.deaths);
        add(5, t, t[1] ? gold : gray);
        swprintf_s(t, L"%u:%02u", r.sec / 60, r.sec % 60);
        add(6, t, gold);
        const time_t when = (time_t)r.endUnix;
        struct tm lt = {};
        if (localtime_s(&lt, &when) == 0) swprintf_s(t, L"%02d-%02d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        else wcscpy_s(t, L"-");
        add(7, t, gold);
    }
    RecSetMessage(d.n ? NULL : L"아직 대전 기록이 없습니다.");
    Log("㉔ 대전 기록 그림 — 줄 %d · 점수 %d · 순위 %d · 랭크 %d승 %d패 · 일반 %d승 %d패", d.n, d.rating, d.rank, d.rw, d.rl, d.nw, d.nl);
}
static void RecSetFocus(DWORD w, const char* why)
{
    ((int(__cdecl*)(DWORD))kFocusFn)(w);
    DWORD now = 0, id = 0;
    if (ReadSelf(kFocusSlot, &now) && now) ReadSelf(now, &id);
    Log("㉔ 포커스 %s — 넣은 칸 %#lx · 지금 %#lx(번호 %lu)", why, w, now, id);
}
static void RecClose(const char* why)
{
    if (!g_rec.open) return;
    g_rec.open = false;
    if (RecWin()) ((PFN_sethidden)kSetHidden)(RecWin(), 1);
    DWORD chat = 0;
    if (ReadSelf(kChatWin, &chat) && chat) {
        ((PFN_sethidden)kSetHidden)(chat, 0);
        RecSetFocus(g_rec.savedFocus, "되돌림");
    } else {
        RecSetFocus(0, "대화방이 없어져 비움");   // 저장한 칸도 대화방과 함께 없어졌을 수 있다 · 숨은 이름 칸이 판 중 키를 받으면 안 된다
    }
    Log("㉔ %s 닫음 — %s", kRecName[g_rec.kind], why);
}
static int __cdecl RecWinProc(DWORD win, DWORD ev, DWORD a1, DWORD)
{
    DWORD id = 0;
    if (ev == kMsgClick && a1 && ReadSelf(a1, &id) && id == kRecBackId) { RecClose("뒤로"); return 1; }
    if (ev == 2) {
        for (int k = 0; k < kRecKinds; ++k) {
            if (win != g_rec.win[k]) continue;
            Log("㉔ %s 창 지워짐", kRecName[k]);
            g_rec.win[k] = 0;
            if (g_rec.kind == k) g_rec.open = false;
        }
    }
    return 0;
}
// 로비에 물음 한 줄 · 화면은 「묻는 중」 — 빈 글자 = 내 기록(`NOXRANK RECORD`)
static void RecAsk(const char* name)
{
    const std::string expect = name[0] ? std::string(name) : NickNow();
    EnterCriticalSection(&g_ruCs);
    strncpy_s(g_recAskName, expect.c_str(), _TRUNCATE);
    LeaveCriticalSection(&g_ruCs);
    InterlockedExchange(&g_recState, kRecWait);
    RecClearAll();
    RecSetMessage(L"기록을 불러오는 중입니다.");
    g_rec.drawn = false;
    g_rec.askedAt = GetTickCount();
    char line[64];
    sprintf_s(line, name[0] ? "NOXRANK RECORD %s\r\n" : "NOXRANK RECORD\r\n", name);
    typedef int(WSAAPI* PFN_send_t)(SOCKET, const char*, int, int);
    const int rc = g_recSock == INVALID_SOCKET ? -1 : ((PFN_send_t)Resolve(19))(g_recSock, line, (int)strlen(line), 0);
    Log("㉔ %s 요청 「%s」 rc=%d(로비 소켓 %u)", kRecName[g_rec.kind], name, rc, (unsigned)g_recSock);
    if (rc < 0) InterlockedExchange(&g_recState, kRecFail);
}
// ★ 11-04 전적 검색 — Enter · 검색 단추. 묻는 중엔 무시(서버가 겹친 물음을 말없이 버린다) · 빈 칸은 안 보낸다 · 칸의 글자는 그대로 두고 표만 바꾼다
static void RecSearchSubmit(const char* how)
{
    if (!g_rec.open || g_rec.kind != kRecSearch) return;
    if (!g_rec.drawn) { Log("㉔ 전적 검색 묻는 중 — %s 무시", how); return; }
    const DWORD entry = ((PFN_findchild)kFindChild)(RecWin(), kSearchEntryId);
    DWORD data = 0;
    if (!entry || !ReadSelf(entry + kEntryDataSlot, &data) || !data) { Log("㉔ 전적 검색 이름 칸을 못 읽음 — %s", how); return; }
    char name[33];
    const int n = WideCharToMultiByte(949, 0, (const wchar_t*)(uintptr_t)data, -1, name, sizeof(name), NULL, NULL);
    if (n <= 1) { Log("㉔ 전적 검색 빈 칸 또는 긴 이름 — %s 안 보냄", how); return; }
    Log("㉔ 전적 검색 %s", how);
    RecAsk(name);
}
static int __cdecl RecPanelProc(DWORD win, DWORD msg, DWORD a3, DWORD a4)
{
    DWORD id = 0;
    if ((msg == kMsgEntryEnter || msg == kMsgClick) && a3 && ReadSelf(a3, &id)) {
        if (msg == kMsgEntryEnter && id == kSearchEntryId) { RecSearchSubmit("Enter"); return 1; }
        if (msg == kMsgClick && id == kSearchGoId) { RecSearchSubmit("검색 단추"); return 1; }
    }
    return g_rec.panelOrigProc ? ((PFN_wndproc4)g_rec.panelOrigProc)(win, msg, a3, a4) : 0;
}
static bool RecEnsureWin(RecKind kind)
{
    DWORD& w = g_rec.win[kind];
    if (w || g_rec.loadFailed[kind]) return w != 0;
    w = ((PFN_moWndLoad)kWndLoad)(kRecFile[kind], (void*)RecWinProc);
    if (!w) {
        g_rec.loadFailed[kind] = true;
        Log("㉔ %s 창을 못 만듦 — 게임 폴더에 window\\%s 가 없음(ui_apply)", kRecName[kind], kRecFile[kind]);
        return false;
    }
    Log("㉔ %s 창 만듦 %#lx", kRecName[kind], w);
    if (kind == kRecSearch) {
        const DWORD panel = ((PFN_findchild)kFindChild)(w, kSearchPanelId), entry = ((PFN_findchild)kFindChild)(w, kSearchEntryId), go = ((PFN_findchild)kFindChild)(w, kSearchGoId);
        DWORD proc = 0, notify = 0, goNotify = 0;
        if (panel && entry && go && ReadSelf(panel + kProcSlot, &proc) && ReadSelf(entry + kEntryNotifySlot, &notify) && ReadSelf(go + kEntryNotifySlot, &goNotify)) {
            g_rec.panelOrigProc = proc;
            *(volatile DWORD*)(panel + kProcSlot) = (DWORD)(uintptr_t)RecPanelProc;
            Log("㉔ 전적 검색 왼쪽 칸 %#lx 처리 함수 감쌈(원래 %#lx) · 이름 칸 %#lx 의 알림 곳 %#lx · 검색 단추 %#lx 의 알림 곳 %#lx", panel, proc, entry, notify, go, goNotify);
        } else {
            Log("㉔ 전적 검색 왼쪽 칸 · 이름 칸을 못 찾음 — 칸 %#lx · %#lx", panel, entry);
        }
    }
    return true;
}
static void RecOpen(DWORD chat, RecKind kind)
{
    if (g_rec.open) return;
    if (!RecEnsureWin(kind)) return;
    g_rec.kind = kind;
    RecClearAll();
    ((PFN_sethidden)kSetHidden)(RecWin(), 0);
    ((PFN_sethidden)kSetHidden)(chat, 1);
    g_rec.open = true;
    g_rec.savedFocus = 0;
    ReadSelf(kFocusSlot, &g_rec.savedFocus);
    if (kind == kRecRecord) {
        RecSetFocus(0, "대전 기록 — 비움");   // 키 받을 칸이 없는 창 · 숨은 대화방 칸이 받지 않게(사람 확인 97)
        RecAsk("");
        return;
    }
    // ★ 11-04 전적 검색 — 처음 화면(뒤로 후 다시 열어도): 칸 · 표를 비움 · 물음 없음
    const DWORD entry = ((PFN_findchild)kFindChild)(RecWin(), kSearchEntryId);
    if (entry) ((PFN_wndproc4)kWinCall94)(entry, kMsgEntrySet, (DWORD)(uintptr_t)L"", 0);
    RecSetFocus(entry, "전적 검색 — 이름 칸");
    RecSetMessage(L"이름을 입력하세요");
    g_rec.drawn = true;
    Log("㉔ 전적 검색 열림");
}
static int __cdecl RecChatProc(DWORD win, DWORD msg, DWORD a3, DWORD a4)
{
    DWORD id = 0;
    if (msg == kMsgClick && a3 && ReadSelf(a3, &id)) {
        if (id == kRecBtnId) { RecOpen(win, kRecRecord); return 0; }
        if (id == kSearchBtnId) { RecOpen(win, kRecSearch); return 0; }   // ★ 11-04 전적 검색 — 정품 찾기 창 대신
    }
    return ((PFN_wndproc4)g_rec.chatOrigProc)(win, msg, a3, a4);
}
static void RecordTick()
{
    DWORD chat = 0;
    ReadSelf(kChatWin, &chat);
    if (chat && g_ourLobby) {
        DWORD proc = 0;
        if (ReadSelf(chat + kProcSlot, &proc) && proc && proc != (DWORD)(uintptr_t)RecChatProc) {
            g_rec.chatOrigProc = proc;
            *(volatile DWORD*)(chat + kProcSlot) = (DWORD)(uintptr_t)RecChatProc;
            Log("㉔ 대화방 창 %#lx 처리 함수 감쌈(원래 %#lx)", chat, proc);
        }
    }
    if (!g_rec.open) return;
    if (!chat) { RecClose("대화방이 없어짐"); return; }
    if (g_rec.drawn) return;
    const LONG st = g_recState;
    switch (st) {
        case kRecDone: {
            RecData d;
            EnterCriticalSection(&g_ruCs);
            d = g_recIn;
            LeaveCriticalSection(&g_ruCs);
            RecDraw(d);
            g_rec.drawn = true;
            break;
        }
        case kRecNoUser:   // ★ 11-04 전적 검색
            RecSetMessage(L"없는 사용자입니다.");
            Log("㉔ 전적 검색 없는 사용자");
            g_rec.drawn = true;
            break;
        case kRecFail:
            RecSetMessage(L"기록을 읽을 수 없습니다.");
            Log("㉔ %s 못 읽음 — 로비가 실패로 답함", kRecName[g_rec.kind]);
            g_rec.drawn = true;
            break;
        default:
            if (GetTickCount() - g_rec.askedAt > kRecTimeoutMs) {
                RecSetMessage(L"기록을 읽을 수 없습니다.");
                Log("㉔ %s 못 읽음 — 5초 안에 답 없음", kRecName[g_rec.kind]);
                g_rec.drawn = true;
            }
            break;
    }
}

static void RoomUiTick()
{
    DWORD list = 0, chat = 0;
    ReadSelf(kListWin, &list);
    ReadSelf(kChatWin, &chat);
    const bool inRoom = !list && !chat;
    static bool wasIn = false;
    RoomUi s;
    EnterCriticalSection(&g_ruCs);
    if (wasIn && !inRoom) g_ru.valid = false;
    s = g_ru;
    LeaveCriticalSection(&g_ruCs);
    wasIn = inRoom;
    StartBtnTick(inRoom, s);
    HomeWinTick(inRoom, s);
    RecordTick();   // ★ 11-04 ㉔
}

static const DWORD kBtnJoinGame = 10001;   // 게임 목록 정보 창의 참가(`noxworld.wnd:71`)
// ★ 11-04 전적 검색 시험 — 포커스 · 칸 글자를 찍고 키를 포커스 칸에 넣는다(`K` = 키 처리 함수 자리 +0x174 를 포커스부터 부모로 — 정품 키 전달의 뒤쪽 절반 · 진짜 키보드 경로는 사람 확인)
static const DWORD kKeySlot = 0x174, kParentSlot = 0x18c, kEvKey = 0x15;
static DWORD TestFindChild(DWORD id)   // 열린 창 먼저 · 다음 대화방
{
    const DWORD w = g_rec.open && RecWin() ? ((PFN_findchild)kFindChild)(RecWin(), id) : 0;
    if (w) return w;
    DWORD chat = 0;
    return ReadSelf(kChatWin, &chat) && chat ? ((PFN_findchild)kFindChild)(chat, id) : 0;
}
static void TestLogFocus(const char* tag)
{
    DWORD f = 0, id = 0, fl = 0;
    ReadSelf(kFocusSlot, &f);
    if (f) { ReadSelf(f, &id); ReadSelf(f + 4, &fl); }
    Log("⑰ 시험 %s 포커스 %#lx 번호 %lu 칸값 %#lx", tag, f, id, fl);
}
static void TestLogText(DWORD win)
{
    DWORD data = 0, id = 0;
    if (!win || !ReadSelf(win + kEntryDataSlot, &data) || !data) { Log("⑰ 시험 글자 못 읽음 %#lx", win); return; }
    ReadSelf(win, &id);
    char u[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)(uintptr_t)data, -1, u, sizeof(u) - 1, NULL, NULL);
    Log("⑰ 시험 칸 %lu 글자 「%s」", id, u);
}
static void TestKeys(const char* s)
{
    char* e = nullptr;
    for (unsigned long sc = strtoul(s, &e, 16); e != s; s = e, sc = strtoul(s, &e, 16)) {
        for (int st = 2; st >= 1; --st) {   // 눌림 2 · 뗌 1
            DWORD f = 0, who = 0;
            ReadSelf(kFocusSlot, &f);
            for (DWORD w = f; w; ) {
                DWORD proc = 0, up = 0;
                ReadSelf(w + kKeySlot, &proc);
                if (proc && ((PFN_wndproc4)proc)(w, kEvKey, sc, (DWORD)st)) { who = w; break; }
                if (!ReadSelf(w + kParentSlot, &up)) break;
                w = up;
            }
            DWORD id = 0;
            if (who) ReadSelf(who, &id);
            if (st == 2) Log("⑰ 시험 키 %#lx 받은 칸 %lu(%#lx)", sc, id, who);
        }
    }
}
static bool TestPressLine(const char* line)
{
    if (line[0] == 'B') {   // ★ 11-04 ㉔ 대전 기록 · 전적 검색 뒤로
        const DWORD back = RecWin() ? ((PFN_findchild)kFindChild)(RecWin(), kRecBackId) : 0;
        if (!back) return false;
        ((PFN_wndproc4)kWinCall94)(RecWin(), kMsgClick, back, 0);
        return true;
    }
    if (line[0] == 'Q') {   // ★ 11-04 전적 검색 단추 — 왼쪽 칸에 누름 알림(정품 단추가 부모에게 보내는 꼴)
        const DWORD panel = g_rec.win[kRecSearch] ? ((PFN_findchild)kFindChild)(g_rec.win[kRecSearch], kSearchPanelId) : 0;
        const DWORD go = panel ? ((PFN_findchild)kFindChild)(panel, kSearchGoId) : 0;
        if (!go) return false;
        ((PFN_wndproc4)kWinCall94)(panel, kMsgClick, go, 0);
        return true;
    }
    if (line[0] == 'N') {   // ★ 11-04 전적 검색 이름 칸에 글자 넣기 — 키로 못 치는 이름(`N 한글` — 시험 파일은 UTF-8 · 칸은 영문 · 숫자만 받는다)
        const DWORD entry = TestFindChild(kSearchEntryId);
        if (!entry) return false;
        wchar_t w[64] = {};
        if (line[1]) MultiByteToWideChar(CP_UTF8, 0, line + 2, -1, w, _countof(w) - 1);
        ((PFN_wndproc4)kWinCall94)(entry, kMsgEntrySet, (DWORD)(uintptr_t)w, 0);
        return true;
    }
    if (line[0] == 'R' || line[0] == 'G') {   // ★ 11-06 귓속말 조각 5 — `R 글` 대화방 입력칸 1910 · `G 글` 판 채팅 입력칸에 글을 넣고 Enter 알림(0x401f).
        //   사람이 치고 Enter 를 누른 것과 같은 정품 처리 함수로 가서 입력 자리(0x447ab8 · 0x487361)를 그대로 지난다(조사 파일럿 W6 · W5 꼴 · 11-05 실물)
        static const uintptr_t kGameChatWin = 0x6f70d8, kGameChatEntry = 0x6f70dc, kGameChatProc = 0x487340;
        static const DWORD kRoomEntryId = 1910;
        wchar_t w[200] = {};
        if (line[1]) MultiByteToWideChar(CP_UTF8, 0, line + 2, -1, w, _countof(w) - 1);
        DWORD win = 0, entry = 0;
        if (line[0] == 'R') {
            ReadSelf(kChatWin, &win);
            entry = win ? ((PFN_findchild)kFindChild)(win, kRoomEntryId) : 0;
        } else {
            ReadSelf(kGameChatWin, &win);
            ReadSelf(kGameChatEntry, &entry);
        }
        if (!win || !entry) return false;
        ((PFN_wndproc4)kWinCall94)(entry, kMsgEntrySet, (DWORD)(uintptr_t)w, 0);
        if (line[0] == 'R') ((PFN_wndproc4)kWinCall94)(win, kMsgEntryEnter, entry, 0);
        else ((PFN_wndproc4)kGameChatProc)(win, kMsgEntryEnter, entry, 0);
        return true;
    }
    if (line[0] == 'C') {   // `C 안녕` — 정품 채팅 보내기 함수로 한 줄(UTF-8 파일 글자 → UTF-16). `K` 는 입력칸 1910 에 글자를 못 넣어(11-05) 진짜 와이어 꼴을 보려면 이 길
        wchar_t w[160] = {};
        MultiByteToWideChar(CP_UTF8, 0, line + 2, -1, w, 159);
        ((PFN_chatSend)kChatSendFn)(w, 0);
        return true;
    }
    if (line[0] == 'K') { TestKeys(line + 1); return true; }   // `K 1e 30` — 스캔 코드(16진)
    if (line[0] == 'U') {   // `U 9651` — 그 칸의 키 처리 함수에 Enter(눌림 · 뗌). 정품 단추의 누름 길(`ButtonProc2` → +0x34 로 0x4007 · 원본 기준)을 그대로 지난다 — NOFOCUS 칸은 포커스로 못 가므로 바로
        const DWORD w = TestFindChild(strtoul(line + 1, nullptr, 10));
        DWORD proc = 0;
        if (!w || !ReadSelf(w + kKeySlot, &proc) || !proc) return false;
        ((PFN_wndproc4)proc)(w, kEvKey, 0x1c, 2);
        ((PFN_wndproc4)proc)(w, kEvKey, 0x1c, 1);
        return true;
    }
    if (line[0] == 'f') { TestLogFocus("f"); return true; }
    if (line[0] == 'F') {   // `F 1910` — 그 칸으로 포커스(정품 포커스 함수)
        const DWORD w = TestFindChild(strtoul(line + 1, nullptr, 10));
        if (!w) return false;
        ((int(__cdecl*)(DWORD))kFocusFn)(w);   // 포커스를 안 받는 칸(NOFOCUS)이면 그대로(원본 `GUI.Focus`)
        TestLogFocus("F");
        return true;
    }
    if (line[0] == 'T') { TestLogText(TestFindChild(strtoul(line + 1, nullptr, 10))); return true; }   // `T 9650` — 칸 글자
    if (line[0] == 'H') { ((int(__cdecl*)())kHomeToggleFn)(); return true; }   // ★ 11-04 ㉓ — Home 키 자리(앞세우기 없이)
    if (line[0] == 'Y' || line[0] == 'y') {   // ★ 11-07 T3 — `Y` = 점수판 단계 넘김(F9 자리) · `y` = 단계 읽기만 — 실물에서 「사람이 끈 판엔 서버가 안 다시 켬」을 보려고
        if (line[0] == 'Y') ((void(__cdecl*)())kRankStageFn)();
        DWORD st = 0;
        ReadSelf(kRankStage, &st);
        Log("⑰ 점수판 단계 = %lu(%c)", (unsigned long)st, line[0]);
        return true;
    }
    if (line[0] == 'S') {   // ★ 11-04 ㉒ 시작 단추 — 회색이면 사람처럼 안 눌린 것으로 친다(정품은 꺼진 칸에 누름을 안 보냄 · 마우스 확인은 사람 판)
        const DWORD btn = g_startWin ? ((PFN_findchild)kFindChild)(g_startWin, kStartBtnId) : 0;
        DWORD bf = 0;
        if (!btn || !ReadSelf(btn + 4, &bf) || !(bf & kWinEnabled)) return false;
        ((PFN_wndproc4)kWinCall94)(g_startWin, kMsgClick, btn, 0);
        return true;
    }
    char op = 0;
    unsigned long slot = 0, id = 0;
    int row = 0;
    const int got = sscanf(line, " %c %lx %lu %d", &op, &slot, &id, &row);
    DWORD win = 0;
    if (got < 3 || !ReadSelf((uintptr_t)slot, &win) || !win) return false;
    switch (op) {
        case 'p': return PressChild(win, id);
        case 'd': {   // `d 7114e4 601` — 눌림 알림(0x4005). 직업 고르기 그림(601~603)은 뗌(0x4007)이 아니라 이 알림으로 고른다(`client__shell__selclass.c` sub_4A4A20)
            const DWORD btn = ((PFN_findchild)kFindChild)(win, id);
            if (!btn) return false;
            ((PFN_wndproc4)kWinCall94)(win, 0x4005, btn, 0);
            return true;
        }
        case 'l': {   // 게임 목록 줄 참가 — 이름 칸(id + 1)이 목록(id)에 알리는 꼴 그대로(0x4010 · 줄 번호 — `GAME1_2.c:3862` sub_439050) 뒤 참가 단추
            if (got < 4) return false;
            const DWORD list = ((PFN_findchild)kFindChild)(win, id);
            const DWORD names = ((PFN_findchild)kFindChild)(win, id + 1);
            const DWORD join = ((PFN_findchild)kFindChild)(win, kBtnJoinGame);   // 줄을 고르면 정보 창이 목록에서 떨어져 나가 그 뒤엔 못 찾는다(`noxworld.c:415` 부모 0) — 먼저 찾아 둠
            if (!list || !names || !join) return false;
            ((PFN_wndproc4)kWinCall94)(list, 0x4010, names, (DWORD)row);
            ((PFN_wndproc4)kWinCall94)(win, kMsgClick, join, 0);
            return true;
        }
        case 's': {
            if (got < 4) return false;
            const DWORD list = ((PFN_findchild)kFindChild)(win, id);
            if (!list) return false;
            ((PFN_wndproc4)kWinCall94)(list, kMsgListSelect, (DWORD)row, 0);
            return true;
        }
    }
    return false;
}

// ★ 11-04 시험 판에서만 — 화면 칸 값 · 숨김 변화를 기록에 찍는다(어느 칸이 언제 비고 다시 차는지가 「뒤로」 왕복의 근거)
static void TestSlotLogTick()
{
    struct Watch { const char* name; uintptr_t slot; DWORD last; DWORD lastFlags; };
    static Watch w[] = { { "메인 메뉴", kMainMenuWin, 0, 0 }, { "접속 방식", kConnWin, 0, 0 }, { "로그인", kLoginWin, 0, 0 }, { "서버 목록", kServListWin, 0, 0 },
                         { "대화방", kChatWin, 0, 0 }, { "게임 목록", kListWin, 0, 0 }, { "옵션 뒤로", kOptsBackWin, 0, 0 }, { "진행", kWolProgWin, 0, 0 } };
    for (Watch& x : w) {
        DWORD v = 0, f = 0;
        if (!ReadSelf(x.slot, &v)) continue;
        if (v) ReadSelf(v + 4, &f);
        if (v != x.last || (f & kWinHidden) != (x.lastFlags & kWinHidden)) {
            Log("⑰ 칸 %s %#lx → %#lx 숨김 %d", x.name, x.last, v, (f & kWinHidden) ? 1 : 0);
            x.last = v; x.lastFlags = f;
        }
    }
    static DWORD lastState = 0xFFFFFFFF;
    DWORD st = 0;
    if (ReadSelf(kWolStateSlot, &st) && st != lastState) { Log("⑰ 칸 WOL 상태 %ld → %ld", (long)lastState, (long)st); lastState = st; }
}

static void TestPressTick()
{
    if (g_testPressFile.empty()) return;
    TestSlotLogTick();
    const DWORD now = GetTickCount();
    if (now - g_testPressLastMs < 100) return;
    g_testPressLastMs = now;
    FILE* f = fopen(g_testPressFile.c_str(), "rb");
    if (!f) return;
    char line[128];
    const bool have = fseek(f, g_testPressDone, SEEK_SET) == 0 && fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!have || !strchr(line, '\n')) return;   // 아직 안 씀 · 쓰는 중인 줄은 다음 차례에(한 프레임에 한 줄)
    g_testPressDone += (long)strlen(line);
    line[strcspn(line, "\r\n")] = 0;
    const bool ok = TestPressLine(line);
    const LONG n = InterlockedIncrement(ok ? &g_testPressOk : &g_testPressMiss);
    Log("⑰ 시험 누름 「%s」 — %s(누적 %ld)", line, ok ? "함" : "창 · 단추를 못 찾음", n);
}

// ─────────────── ★ 11-06 귓속말 조각 5 — 입력 잡기 셋 · 표시(플랜 `Docs/MO_WHISPER_PLAN_1105.md` 5절 「입력 잡기」 · 「표시」 · 7절 문구)
//   입력 — 정품이 입력 글을 넘기는 call 셋을 DLL 로 돌린다(HookCallSite). /w · /r 이면 귓속말(원래 함수를 안 부름) · 아니면 원래 함수 그대로
//     0x487361 판 채팅 → 0x486fd0(글, 팀) · 0x447ab8 대화방 입력 → 0x446b00(글) · 0x446b7a 대화방 명단에서 고른 줄 → 정품 비공개 0x41f430(이름, 글)
//     고른 줄은 언제나 귓속말로 — 정품은 WOL 비공개 메시지를 보내 우리 로비에선 말없이 사라졌다(사람 확인 103). 0 을 돌려 정품의 「내 줄」(0x446b8c → 0x446d20(이름, 글, 1, 1))을
//     건너뛴다 — 보낸 줄은 서버 답(TO) 뒤 한 번만(/w 와 같게 · 안 그러면 두 줄 · 없는 사람이면 글 줄 + 안내 줄)
//   보내기 — 주 스레드가 g_chatCs 아래 받은 줄을 먼저 비우고(끊김 표시를 먼저 반영해야 답 차례가 안 어긋남) OnInput → 보낼 줄을 g_chatOut 에(둘째 스레드가 50ms 안에 보냄)
//   표시 — 매 틀(FrameTick) ChooseShow · 대화방 줄 0x446d20(이름, 글, 1, 받음 0 · 보냄 1 — 머리 꼴은 `ui_apply.ps1` 의 정품 문구 두 줄) ·
//     머리 없는 안내(7 · 9 · 10 · 11) = 대화방 출력 칸 [0x69c8c8] 에 줄 더하기(0x400d · 색 7 — 0x446d20 안쪽 0x446d89 와 같은 호출) · 판 가운데 0x445020(글) ·
//     그 밖 화면(게임 목록 · 메뉴 · 숨은 대화방)은 쥐었다가 다음에(CShowBox 20건)
//   받은 글은 0x446d20 · 0x445020 모두 형식 문자열이 아닌 인자로만 넘어간다(플랜 9절 · 역어셈블) — %s %n 이 든 글도 그대로
typedef int(__cdecl* PFN_whRoomSend)(const wchar_t*);
typedef int(__cdecl* PFN_whPriv)(const char*, const wchar_t*);
typedef void(__cdecl* PFN_whRoomLine)(const char*, const wchar_t*, int, int);
typedef void(__cdecl* PFN_whCenter)(const wchar_t*);
typedef int(__cdecl* PFN_whPlay)(void);
static const uintptr_t kWhGameChatCall = 0x487361, kWhRoomSendCall = 0x447ab8, kWhRoomSend = 0x446b00, kWhListCall = 0x446b7a, kWhPriv = 0x41f430;
static const uintptr_t kWhRoomLine = 0x446d20, kWhRoomOut = 0x69c8c8, kWhCenter = 0x445020, kWhPlay = 0x42ca50;
static const DWORD kWhPlainColor = 7;   // 0x446d20 이 귓속말 줄에 쓰는 색(채널 줄은 6) — 머리 없는 안내도 귓속말 줄과 같은 색

enum class WhSite : uint8_t { Game, Room, List, kCount };
static const char* WhSiteName(WhSite s)
{
    switch (s) {
    case WhSite::Game: return "판 채팅";
    case WhSite::Room: return "대화방";
    case WhSite::List: return "고른 줄";
    case WhSite::kCount: break;
    }
    return "?";
}
static whisper::CWhisperState g_whState;   // 주 스레드만 · 바꿀 때는 g_chatCs 아래(받은 줄 비우기와 입력이 한 잠금 안에서)
static whisper::CShowBox g_whBox;          // 주 스레드만
static volatile LONG g_whInput[(size_t)WhSite::kCount][(size_t)whisper::Cmd::kCount] = {};   // 세는 자리 — 입력 자리 셋 × 갈래 여섯(일반 채팅 포함)
static volatile LONG g_whRoomNoOut = 0;    // 머리 없는 안내를 대화방 줄로 골랐는데 출력 칸이 없던 수

// 요약 스레드(1초마다) — 셈이 바뀌었으면 자리마다 한 줄(갈래 여섯 전부 · 0 도 찍음 — 「안 셌다」와 「0번」을 가르게)
static void WhisperSummary()
{
    static LONG seen[(size_t)WhSite::kCount][(size_t)whisper::Cmd::kCount] = {};
    static LONG noOutSeen = 0;
    bool changed = g_whRoomNoOut != noOutSeen;
    LONG now[(size_t)WhSite::kCount][(size_t)whisper::Cmd::kCount];
    for (size_t s = 0; s < (size_t)WhSite::kCount; ++s)
        for (size_t c = 0; c < (size_t)whisper::Cmd::kCount; ++c) {
            now[s][c] = g_whInput[s][c];
            changed = changed || now[s][c] != seen[s][c];
        }
    if (!changed) return;
    for (size_t s = 0; s < (size_t)WhSite::kCount; ++s) {
        char line[400];
        int o = _snprintf(line, sizeof(line) - 1, "귓속말 셈 — %s |", WhSiteName((WhSite)s));
        for (size_t c = 0; c < (size_t)whisper::Cmd::kCount && o > 0 && o < (int)sizeof(line) - 60; ++c)
            o += _snprintf(line + o, sizeof(line) - 1 - o, " %s %ld", whisper::CmdName((whisper::Cmd)c), now[s][c]);
        line[sizeof(line) - 1] = 0;
        Log("%s", line);
    }
    noOutSeen = g_whRoomNoOut;
    Log("귓속말 셈 — 대화방 출력 칸 없어 못 낸 안내 %ld", noOutSeen);
    memcpy(seen, now, sizeof(seen));
}

static std::string WhMyName()
{
    EnterCriticalSection(&g_tokCs);
    const std::string n = g_loginName;
    LeaveCriticalSection(&g_tokCs);
    return n;
}

// g_chatCs 를 쥔 채로 — 받은 줄을 판단 부품에(빈 글자 = AUTHOK 받은 연결이 끊김 → 답 못 받은 보낸 글은 8번)
static size_t WhisperDrainLocked(std::vector<whisper::Notice>& show)
{
    std::deque<std::string> in;
    in.swap(g_chatIn);
    for (const std::string& l : in) {
        if (l.empty()) g_whState.OnDisconnect(show);
        else g_whState.OnServerLine(whisper::ParseServerLine(l), show);
    }
    return in.size();
}

// 입력 하나 — 귓속말 명령이면 true(원래 호출을 안 함)
static bool WhisperInput(WhSite site, const whisper::Parsed& p)
{
    InterlockedIncrement(&g_whInput[(size_t)site][(size_t)p.cmd]);
    if (p.cmd == whisper::Cmd::Plain) return false;
    std::vector<whisper::Notice> show;
    EnterCriticalSection(&g_chatCs);
    const size_t got = WhisperDrainLocked(show);
    const std::string line = g_whState.OnInput(p, g_chatUp, show);
    if (!line.empty()) g_chatOut.push_back(line + "\r\n");
    const size_t pend = g_whState.Pending();
    LeaveCriticalSection(&g_chatCs);
    for (const whisper::Notice& n : show) g_whBox.Push(n);
    Log("귓속말 입력 — %s · %s · %s(받은 줄 %zu · 낼 것 %zu · 답 기다림 %zu)", WhSiteName(site), whisper::CmdName(p.cmd), line.empty() ? "안 보냄" : "보냄", got, show.size(), pend);
    return true;
}

static void __cdecl WhGameChat(const wchar_t* text, int team)
{
    if (WhisperInput(WhSite::Game, whisper::ParseCommand(text, WhMyName()))) return;
    ((PFN_chatSend)kChatSendFn)(text, team);
}
static int __cdecl WhRoomSend(const wchar_t* text)
{
    if (WhisperInput(WhSite::Room, whisper::ParseCommand(text, WhMyName()))) return 0;   // 부른 쪽(0x447abd 뒤)은 반환을 안 쓴다
    return ((PFN_whRoomSend)kWhRoomSend)(text);
}
static int __cdecl WhListSend(const char* name, const wchar_t* text)
{
    WhisperInput(WhSite::List, whisper::ParseListWhisper(name, text, WhMyName()));   // 꼴 틀림이어도 정품 길로 안 보낸다(말없이 사라지는 대신 7번)
    return 0;   // 0x446b84 — 0 이면 정품 「내 줄」을 건너뜀
}

static void WhRoomSink(const whisper::Notice& n)
{
    const whisper::RoomArgs a = whisper::RoomOf(n);
    bool noOut = false;
    if (a.plain) {
        DWORD out = 0;
        if (ReadSelf(kWhRoomOut, &out) && out) ((PFN_wndproc4)kWinCall94)(out, kMsgListAdd, (DWORD)(uintptr_t)a.text.c_str(), kWhPlainColor);
        else { noOut = true; InterlockedIncrement(&g_whRoomNoOut); }
    } else {
        ((PFN_whRoomLine)kWhRoomLine)(a.name.c_str(), a.text.c_str(), 1, a.sent);
    }
    Log("귓속말 표시 — 대화방 줄 · %s%s", whisper::KindName(n.kind), noOut ? "(출력 칸 없음 — 안 냄)" : "");
}
static void WhCenterSink(const std::wstring& line)
{
    ((PFN_whCenter)kWhCenter)(line.c_str());
    Log("귓속말 표시 — 판 가운데(%zu자)", whisper::CountChars(line));
}

// 매 틀(주 스레드) — 받은 줄을 판단 부품에 · 쥔 것을 지금 화면에
static void WhisperTick()
{
    std::vector<whisper::Notice> show;
    size_t got = 0;
    if (g_chatStarted) {
        EnterCriticalSection(&g_chatCs);
        got = WhisperDrainLocked(show);
        LeaveCriticalSection(&g_chatCs);
    }
    for (const whisper::Notice& n : show) g_whBox.Push(n);
    if (got) Log("귓속말 주 스레드 — 받은 줄 %zu · 낼 것 %zu · 쥔 것 %zu · 답 기다림 %zu", got, show.size(), g_whBox.Held(), g_whState.Pending());
    static size_t s_heldLogged = 0;
    if (g_whBox.Held() == 0) { s_heldLogged = 0; return; }
    DWORD chat = 0;
    ReadSelf(kChatWin, &chat);
    const whisper::Where w = whisper::ChooseShow(ChatVisible(), chat != 0, ((PFN_whPlay)kWhPlay)());
    g_whBox.Pump(w, (uint64_t)GetTickCount(), WhRoomSink, WhCenterSink);
    if (w == whisper::Where::Hold && g_whBox.Held() != s_heldLogged) {
        s_heldLogged = g_whBox.Held();
        Log("귓속말 쥔 것 %zu — 대화방 · 판이 아닌 화면(버림 누적 %llu)", s_heldLogged, (unsigned long long)g_whBox.GetStats().dropped);
    }
}

static const uintptr_t kDrawAll = 0x489bf0;
static const uintptr_t kDrawAllCallMain = 0x43e29a, kDrawAllCallB = 0x4aa99d;   // 주 루프 그리기 · 다른 프레임 루프(어느 쪽이 메뉴에서 도는지는 셈으로)
static volatile LONG g_drawMain = 0, g_drawB = 0;
typedef void(__cdecl* PFN_void)(void);
static void FrameTick(volatile LONG* count, const char* what)
{
    if (GetCurrentThreadId() != g_mainTid) return;
    if (InterlockedIncrement(count) == 1) Log("⑯ 프레임 자리 — 첫 %s", what);
    InterlockedIncrement(&g_ticks);
    LauncherTick();
    LobbyLostTick(what);   // ★ 11-03 ㉑
    RoomUiTick();          // ★ 11-04 ㉒
    WhisperTick();         // ★ 11-06 귓속말 조각 4 — 둘째 연결이 받은 줄
    TestPressTick();
}
static void __cdecl DrawAllMain()
{
    FrameTick(&g_drawMain, "주 루프 그리기(0x43e29a)");
    ((PFN_void)kDrawAll)();
}
static void __cdecl DrawAllB()
{
    FrameTick(&g_drawB, "다른 루프 그리기(0x4aa99d)");
    ((PFN_void)kDrawAll)();
}

static void InstallTickHook()
{
    const bool a = HookCallSite(kDrawAllCallMain, kDrawAll, (void*)DrawAllMain, "⑯ 프레임 자리 — 주 루프 그리기");
    const bool b = HookCallSite(kDrawAllCallB, kDrawAll, (void*)DrawAllB, "⑯ 프레임 자리 — 다른 루프 그리기");
    Log("⑯ 프레임 자리 — %s · %s · 런처 %s", a ? "붙임" : "못 붙임", b ? "붙임" : "못 붙임",
        g_launchAtStart.empty() ? "아님(자동 진행 안 함 · 셈만)" : g_launchAtStart.c_str());
}

static void LauncherSummary()
{
    static LONG seen[10] = {};
    const LONG now[10] = { g_loginWndMade, g_loginBlocked, g_loginPassed, g_loginSignup, g_loginSignupMiss,
                           g_autoPress[0], g_autoPress[1], g_autoPress[2], g_autoPress[3], g_autoSkip };
    if (memcmp(seen, now, sizeof(now)) == 0) return;
    memcpy(seen, now, sizeof(now));
    Log("⑮ 로그인 창 셈 — 만듦 %ld · 「로그인!」 막음 %ld · 넘김 %ld · 「등록」 → 가입 페이지 %ld · 주소 못 정함 %ld", now[0], now[1], now[2], now[3], now[4]);
    Log("⑯ 자동 진행 셈 — 메인 메뉴 %ld · 접속 방식 %ld · 로그인 %ld · 서버 목록 %ld · 손 안 댐 %ld · 프레임 %ld(주 루프 %ld · 다른 루프 %ld)", now[5], now[6],
        now[7], now[8], now[9], (LONG)g_ticks, (LONG)g_drawMain, (LONG)g_drawB);
}

static void InstallLoginHook()
{
    HookCallSite(kLoginWndLoadCall, kWndLoad, (void*)LoginWndLoad, "⑮ 로그인 창");
    InstallTickHook();   // ★ 런처 P6 · P7 — ⑯
}

// ★ 11-02 ⑱ 판 안에서 창 닫기 — 프로세스가 창 없이 남던 것.
//   정품 `WM_DESTROY`(`0x444dc4`)는 끝내기 깃발 둘(`0x59b7bc` · `0x59b7c0`)을 0 으로 내려 주 고리를 끝낸다.
//   그런데 WinMain 은 주 고리(`0x43de10`)를 두 자리에서 부른다 — 메뉴 `0x4016a8`(0 을 돌려주면 종료 길 `0x401a49`) ·
//   판 안 `0x401964`(반환값을 안 봄). 판 안에서 닫으면 맨 위로 돌아 메뉴 자리가 주 고리를 다시 열며 깃발을 1 로 되살린다
//   → 창 없는 게임 목록 화면을 계속 돈다(정품 원래 흐름 · 전체 화면 전제라 판 안 X 를 안 본 것으로 짐작).
//   그래서 메뉴 자리에서 게임 창이 이미 없으면 주 고리를 열지 않고 0 을 돌려준다 = 대화방에서 닫을 때와 같은 정품 종료 길.
//   깃발로 가르지 말 것 — 메뉴로 판을 나갈 때(`0x43dd90`)도 0 이다. 창이 없는 경우만 끝낸다
static const uintptr_t kMainLoop = 0x43de10, kMainLoopCallMenu = 0x4016a8, kGameWnd = 0x83a31c;
typedef int(__cdecl* PFN_mainloop)(void);
static int __cdecl MenuLoopEntry()
{
    const HWND wnd = (HWND)(uintptr_t)*(volatile DWORD*)kGameWnd;
    if (wnd && !IsWindow(wnd)) {
        Log("⑱ 게임 창 %p 이 이미 닫힘 — 메뉴로 돌아가지 않고 정품 종료 길로", wnd);
        return 0;
    }
    return ((PFN_mainloop)kMainLoop)();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_statCs);
        InitializeCriticalSection(&g_resolveCs);
        InitializeCriticalSection(&g_tokCs);
        InitializeCriticalSection(&g_ruCs);   // ★ 11-04 ㉒
        InitializeCriticalSection(&g_chatCs); // ★ 11-06 귓속말 조각 4 — 둘째 연결
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
        g_launchAtStart = LaunchName();   // ★ 런처 P7 — ⑯ 자동 진행은 처음 환경으로만 가른다
        sw[0] = 0;
        g_testLoginBlock = gameOk && g_launchAtStart.empty() && GetEnvironmentVariableA("NOX_TEST_LOGINBLOCK", sw, sizeof(sw)) && sw[0] == '1';   // ★ 런처 P5 시험
        char pressFile[MAX_PATH] = { 0 };
        if (gameOk && GetEnvironmentVariableA("NOX_TEST_PRESS", pressFile, MAX_PATH)) g_testPressFile = pressFile;   // ★ ⑰ 시험

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
        if (gameOk && !g_launchAtStart.empty() && InstallSoundMute()) AutoMuteSet(true, "런처로 켠 판 · 대화방까지");   // ★ 11-04
        if (gameOk) InstallLoginHook();               // ★ 런처 P5 — ⑮ 정품 로그인 창(「로그인!」 막음 · 「등록」 → 가입 페이지)
        if (gameOk) InstallIdleObserverBlock();       // ★ 11-09 ㉕ — 가만히 둔 클라가 관전 요청(bb)을 안 보냄
        if (gameOk) {   // ★ 11-06 플랜1 조각 6 ㉓ — 게임 목록 줄
            HookCallSite(kListRowCallA, kListRowFn, (void*)ListRowHook, "㉓ 목록 줄 — 새 항목");
            HookCallSite(kListRowCallB, kListRowFn, (void*)ListRowHook, "㉓ 목록 줄 — 다시 그리기");
        }
        if (gameOk) HookCallSite(kMainLoopCallMenu, kMainLoop, (void*)MenuLoopEntry, "⑱ 메뉴 주 고리");   // ★ 11-02 — 판 안에서 창 닫기
        if (gameOk) {   // ★ 11-06 귓속말 조각 5 — 입력 잡기 셋(묶음 설명은 WhisperInput 위)
            HookCallSite(kWhGameChatCall, kChatSendFn, (void*)WhGameChat, "귓속말 — 판 채팅 입력");
            HookCallSite(kWhRoomSendCall, kWhRoomSend, (void*)WhRoomSend, "귓속말 — 대화방 입력");
            HookCallSite(kWhListCall, kWhPriv, (void*)WhListSend, "귓속말 — 대화방 고른 줄(정품 비공개)");
        }
    }
    return TRUE;
}
