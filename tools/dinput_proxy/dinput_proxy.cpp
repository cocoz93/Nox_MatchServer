// 녹스 입력 가르기 — dinput.dll 프록시
//
// 정품 클라 둘을 한 PC 에 띄우면 마우스·키보드가 두 창에 똑같이 들어간다.
// 까닭 (2026-09-28 로그 실측, Game2.exe):
//   키보드 — 게임이 처음부터 SetCooperativeLevel = NONEXCLUSIVE | BACKGROUND 로 잡는다
//   마우스 — 게임은 EXCLUSIVE | FOREGROUND 로 잡으려 한다. 그러면 창이 앞에 없을 때
//            Acquire 가 0x80070005 로 실패해 게임이 꺼진다(2026-09-07 실측).
//            그래서 이 프록시가 NONEXCLUSIVE | BACKGROUND 로 바꿔 넘긴다
//   BACKGROUND = 창이 뒤에 있어도 장치 입력을 받는다. 한 대에 하나만 띄우던 게임이라 원래는 문제가 안 됐다.
//
// 이 프록시가 하는 일 — 게임 창이 앞에 없으면 **진짜 입력을 버린다.**
//   GetDeviceData   누름·이동은 버리고 뗌만 남긴다(창을 옮기는 사이 눌린 채로 굳지 않게)
//   GetDeviceState  전부 0 (= 안 눌림·안 움직임). 녹스는 이 길을 안 쓴다(로그상 0회)
//   GetAsyncKeyState 0 (= 안 눌림). 윈도 전체 키 상태라 창과 무관하게 답한다 —
//                    Game.exe 가 USER32 에서 들여오므로 그 수입표(IAT) 칸을 바꿔 끼운다
//   앞에 있는지 = 「지금 앞 창이 이 프로세스 것인가」
// ★ 11-20 — 앞에 있을 때 왼쪽 버튼 **꾹 누르기 = 계속 공격**(`HoldRepeat`). 끄기 = Scroll Lock 켜기
// ★ 11-21 — 게임 화면 위에서 누른 것만 반복 · 주문 막대·책 같은 창 위에서 누르면 한 번만(`CursorKnown` · 커서 칸 0x5AB824)
//
// ※ cnc-ddraw(ddraw.dll) 도 DirectInput 을 후킹하지만 ddraw.ini 에서 꺼 두었다(no_dinput_hook=true).
//   켜 두면 마우스는 막아 주나 키보드는 안 막는다(cnc-ddraw v7.1.0.0 src/directinput.c:92~93).
// ※ DirectInputCreateW(ver=0x050A) 는 게임 폴더의 GOG 조이스틱 파일(winmmo.dll)이 부른다 — 안 감싼다.
//
// 로그: 환경변수 NOX_HOOK_LOG_DIR 이 있을 때만 <그 폴더>\dihook_<exe>_<pid>.log 에 쓴다.
//       켜 두면 2초마다 경로별 계수기 요약을 한 줄 쓴다(어느 길로 입력이 새는지 가르는 용).
// 빌드: build.bat  (x86 필수 — Game.exe 가 32비트다)
// 시험: test.bat   (bgtest.cpp — 뒤에 있는 창이 마우스 이동을 버리나)
// 놓는 곳: 게임 폴더. 실행파일 폴더가 DLL 검색 1순위라 시스템 것보다 먼저 잡힌다.
// 되돌리기: 이 파일을 지우면 마우스 Acquire 실패로 게임이 꺼질 수 있다(위) —
//           지울 거면 ddraw.ini 의 no_dinput_hook 을 false 로 같이 돌릴 것.
// 옛 판(자동 조작 통로 포함)은 git d610465 직전.

#define DIRECTINPUT_VERSION 0x0700
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>   // _ReturnAddress — 마법 세트 지키기에서 누가 불렀나 기록

// ─────────────────────────────────────────────── 로그 (켜야 쓴다)

static char  g_log[MAX_PATH] = { 0 };
static CRITICAL_SECTION g_cs;

static void Log(const char* fmt, ...)
{
    if (!g_log[0]) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    FILE* f = fopen(g_log, "a");
    if (f) {
        fprintf(f, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

static const char* CoopStr(DWORD f, char* s)
{
    s[0] = 0;
    if (f & DISCL_EXCLUSIVE)    strcat(s, "EXCLUSIVE ");
    if (f & DISCL_NONEXCLUSIVE) strcat(s, "NONEXCLUSIVE ");
    if (f & DISCL_FOREGROUND)   strcat(s, "FOREGROUND ");
    if (f & DISCL_BACKGROUND)   strcat(s, "BACKGROUND ");
    return s;
}

// ─────────────────────────────────────────────── 앞에 있나

static bool InForeground()
{
    // ★ GetForegroundWindow 는 쓰지 않는다 — cnc-ddraw 가 창모드에서 이걸 가로채 늘 게임 창을 돌려준다
    //   (v7.1.0.0 src/winapi_hooks.c:578~586). 2026-09-28 실측: 클라 둘이 같은 때에 둘 다 「앞=나」.
    //   GetProcAddress 도 가로채므로 돌아서 부를 수도 없다.
    //   GetGUIThreadInfo(0) = 지금 앞에 있는 스레드의 활성 창 — cnc-ddraw 가 안 건드린다
    GUITHREADINFO gi; memset(&gi, 0, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(0, &gi) || !gi.hwndActive) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(gi.hwndActive, &pid);
    return pid == GetCurrentProcessId();
}

// DIMOUSESTATE 안 자리값 — 축 X=0 Y=4 Z=8, 버튼 0~3 = 12~15
#define OFS_B0  12

// ★ 11-20 — 꾹 누르기: 진짜 버튼이 눌린 채 이만큼 지나면 뗌·누름을 끼워 넣는다
//   0.1초 = 서버 한 틱(1/30초)의 세 배 · 롱소드 휘두르기 8프레임(0.27초)보다 짧아 휘두르기가 끝나자마자 다음 누름이 기다리고 있다.
//   공격이 실제로 나가는 간격은 서버의 기력(롱소드 70 · 초당 90 참)이 정한다 — 모자라면 서버가 그 누름을 버린다
#define HOLD_REPEAT_MS  100

// ★ 11-21 — 꾹 누르기는 **게임 화면 위에서 누른 것만**. 주문 막대·책 같은 창 위에서 누르면 한 번만
//   (사용자 11-21 「공격은 누르고 있어도 계속 나가되 마법 클릭은 계속 클릭이 아니게 · 책에서 막대로 못 옮긴다」).
//   가르는 값 = 게임이 매 프레임 정하는 커서 모양 — 창 위면 0(고르기), 게임 화면이면 14(걷기 화살표)·5(겨눔) 따위
//   (opennox `input_c.go:405~411` · `client/gui` 커서 표 — 원본 기준).
//   자리 0x5AB824 = 이 Game.exe(PE 시각 0x3978A3FE)에서 실측으로 찾았다 — 11-21 사용자가 커서를 화면↔주문 막대로 오가며
//   마우스로 주문을 쓰는 동안 335번 떠서 0·5·14 만 오간 칸(옆 후보 0x6F7300 은 0·14 만 — 누름 로그에 같이 찍어 견준다).
//   opennox 기록 자리(0x6E0384)는 이 실행 파일에선 다른 것이 들어 있어 못 쓴다(같은 책의 0x59D684 「화면 너비」 자리에 글자 "oshe").
//   실행 파일이 바뀌어 시각이 다르면 가르기를 끄고 옛 동작(늘 반복)으로 간다 — 엉뚱한 칸을 읽지 않게
#define CURSOR_TYPE_VA      0x5AB824u
#define CURSOR_TYPE_VA_ALT  0x6F7300u
#define GAME_EXE_STAMP      0x3978A3FEu

static int g_cursorOk = -1;   // -1 = 아직 안 봄 · 0 = 못 믿음 · 1 = 믿음
static bool CursorKnown()
{
    if (g_cursorOk >= 0) return g_cursorOk == 1;
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    bool ok = false;
    DWORD stamp = 0, sizeImg = 0;
    if (base == (const BYTE*)0x400000) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        stamp = nt->FileHeader.TimeDateStamp;
        sizeImg = nt->OptionalHeader.SizeOfImage;
        ok = stamp == GAME_EXE_STAMP && CURSOR_TYPE_VA_ALT + 4 <= 0x400000u + sizeImg;
    }
    g_cursorOk = ok ? 1 : 0;
    Log("[커서 칸] 실행 파일 바탕 %p · 시각 0x%08lX · 크기 0x%lX → %s", base, stamp, sizeImg,
        ok ? "믿음(게임 화면에서 누른 것만 반복)" : "못 믿음(옛 동작 — 늘 반복)");
    return ok;
}
static DWORD CursorType()    { return *(volatile const DWORD*)CURSOR_TYPE_VA; }
static DWORD CursorTypeAlt() { return *(volatile const DWORD*)CURSOR_TYPE_VA_ALT; }

// 뒤에 있을 때 남길 이벤트 = 뗌만.
//   마우스 축 이벤트의 dwData 는 이동량이라 0x80 비트로 누름을 가를 수 없다 — 자리값으로 먼저 가른다
static bool KeepInBackground(bool isMouse, DWORD ofs, DWORD data)
{
    if (isMouse && (ofs < OFS_B0 || ofs >= OFS_B0 + 4)) return false;   // 축 이동
    return (data & 0x80) == 0;                                          // 키·버튼 뗌
}

// ─────────────────────────────────────────────── 진짜 DLL

static HMODULE g_real = NULL;
typedef HRESULT(WINAPI* PFN_DICreateA)(HINSTANCE, DWORD, LPDIRECTINPUTA*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DICreateW)(HINSTANCE, DWORD, LPDIRECTINPUTW*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DICreateEx)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_Plain)(void);
typedef HRESULT(WINAPI* PFN_GetClassObject)(REFCLSID, REFIID, LPVOID*);

static bool LoadReal()
{
    if (g_real) return true;
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);   // 32비트 프로세스면 SysWOW64 로 온다
    if (!n) return false;
    strcat(path, "\\dinput.dll");
    g_real = LoadLibraryA(path);
    Log("진짜 DLL 열기 %s → %s", path, g_real ? "성공" : "실패");
    return g_real != NULL;
}

// ─────────────────────────────────────────────── 장치 껍데기

class DevProxy;
static CRITICAL_SECTION g_devcs;           // 요약 스레드가 장치를 읽는 동안 지워지지 않게
static DevProxy* g_mouseDev = NULL;
static DevProxy* g_kbDev = NULL;

class DevProxy : public IDirectInputDeviceA
{
public:
    IDirectInputDeviceA* r;
    LONG  ref;
    bool  isMouse;
    const char* name;
    // 계수기 — 앞에서 넘긴 것 / 뒤에서 남긴 것(뗌) / 뒤에서 버린 것 / GetDeviceState 를 0 으로 채운 횟수
    long  cFg, cBgKept, cBgDropped, cStateZeroed;
    // GetDeviceData 호출 — 성공 / 실패(안 잡힘 등). 뒤에 있는 창이 아예 안 부르는지 가르는 분모
    long  cCallOk, cCallFail;
    int   lastFg;   // -1 = 아직 모름
    // ★ 11-20 꾹 누르기 — 진짜 왼쪽 버튼 상태 · 0 = 게임이 본 것도 누름 / 1 = 뗌을 끼워 넣어 다음 호출에 누름을 넣어야 함 ·
    //   마지막으로 누름이 된 때 · 끼워 넣은 순번 · 끼워 넣은 뗌/누름 수
    bool  holdDown;
    int   holdPhase;
    DWORD holdTick, holdSeq;
    long  cInjUp, cInjDown;
    // ★ 11-21 — 이번 누름이 게임 화면에서 시작했나 · 누름 갈래 셋(화면 / 창 / 칸 못 믿음) · 뗀 뒤 창 위라 누름을 안 넣고 멈춘 수
    bool  holdWorld;
    long  cPressWorld, cPressGui, cPressBlind, cStopGui;

    DevProxy(IDirectInputDeviceA* real, REFGUID g)
        : r(real), ref(1), cFg(0), cBgKept(0), cBgDropped(0), cStateZeroed(0), cCallOk(0), cCallFail(0), lastFg(-1),
          holdDown(false), holdPhase(0), holdTick(0), holdSeq(0), cInjUp(0), cInjDown(0),
          holdWorld(false), cPressWorld(0), cPressGui(0), cPressBlind(0), cStopGui(0)
    {
        isMouse = (IsEqualGUID(g, GUID_SysMouse) != 0);
        name = isMouse ? "마우스" : (IsEqualGUID(g, GUID_SysKeyboard) ? "키보드" : "기타");
        Log("[%s] 껍데기 생성 (진짜 %p)", name, real);
    }

    void LogCounts(const char* why)
    {
        Log("[%s] %s — 앞 %ld · 뒤 남김 %ld · 뒤 버림 %ld · 상태 0 채움 %ld",
            name, why, cFg, cBgKept, cBgDropped, cStateZeroed);
    }
    void NoteFg(bool fg)
    {
        if ((int)fg == lastFg) return;
        lastFg = fg ? 1 : 0;
        LogCounts(fg ? "앞으로 옴" : "뒤로 감");
    }

    // ★ 11-20 트레일러 — 왼쪽 버튼 꾹 누르기 = 계속 공격.
    //   정품 클라는 공격(CCAction 6)을 **누르는 순간 한 번만** 서버에 보낸다(2026-09-09 실측 — 180초 눌러 2번 ·
    //   서버 `Server.cpp` 코드 6 주석). 서버는 버튼이 눌려 있는지 모르므로 여기서 누름을 다시 만든다 —
    //   진짜 버튼이 눌린 채 HOLD_REPEAT_MS 가 지나면 「뗌」을, **다음 호출에** 「누름」을 그릇 끝에 붙인다.
    //   한 호출에 둘을 같이 넣지 않는 까닭 = 게임이 한 번 읽은 것의 마지막 상태만 보면 누름 변화가 안 생긴다(짐작 — 나눠 넣으면 어느 쪽이든 성립)
    //   진짜 버튼 이벤트가 오면 그것이 이긴다(게임이 본 상태 = 진짜 상태로 되돌림).
    //   ⚠ 누른 채 쓰는 다른 조작(가방에서 물건 끌기 등)도 끊긴다 — 그때는 Scroll Lock 을 켠다(새 뗌만 막고, 이미 넣은 뗌의 누름은 마저 넣는다)
    //   → 11-21 창(책·가방·주문 막대) 위에서 시작한 끌기는 이제 안 끊긴다(커서 칸으로 가름 · 위 `CursorKnown`). 게임 화면에서 시작한 끌기만 위 그대로
    void HoldRepeat(BYTE* base, DWORD cb, LPDWORD pn, DWORD cap)
    {
        if (cb < 4 * sizeof(DWORD)) return;              // dwOfs · dwData · dwTimeStamp · dwSequence 를 못 채운다
        const DWORD now = GetTickCount();
        const DWORD n = *pn;
        for (DWORD i = 0; i < n; i++) {
            const DWORD* e = (const DWORD*)(base + (size_t)i * cb);
            if (e[3] > holdSeq) holdSeq = e[3];
            if (e[0] != OFS_B0) continue;
            holdDown = (e[1] & 0x80) != 0;
            holdPhase = 0;
            holdTick = now;
            if (!holdDown) continue;
            // ★ 11-21 — 진짜 누름마다 갈래 셋: 칸 못 믿음 → 옛 동작(반복) · 커서 0(창 위) → 이번 누름은 한 번만 · 그 밖(게임 화면) → 반복.
            //   커서 값은 게임이 앞 프레임에 정한 것 — 누른 자리에 커서가 있던 때다
            if (!CursorKnown()) { holdWorld = true; cPressBlind++; continue; }
            const DWORD ct = CursorType();
            holdWorld = (ct != 0);
            if (holdWorld) cPressWorld++; else cPressGui++;
            Log("[마우스] 왼쪽 누름 — 커서 %lu (옆 칸 %lu) → %s", ct, CursorTypeAlt(), holdWorld ? "게임 화면 · 반복" : "창 · 한 번만");
        }
        if (!holdDown || !holdWorld || n >= cap) return; // 안 눌림 · 창에서 시작한 누름 · 그릇이 참(다음 호출에)
        // 누른 채 커서를 창 위로 끌고 갔으면 끼워 넣지 않는다 — 창 위에서 누름을 넣으면 그 창(주문 아이콘 등)을 누른다
        const bool overGui = CursorKnown() && CursorType() == 0;
        DWORD data;
        if (holdPhase == 1) {
            if (overGui) { holdPhase = 0; holdWorld = false; cStopGui++; return; }   // 뗀 채로 멈춘다 — 진짜 뗌이 와도 뗌이라 어긋나지 않는다
            data = 0x80; holdPhase = 0; holdTick = now; cInjDown++;
        }
        else if (now - holdTick >= HOLD_REPEAT_MS && !(GetKeyState(VK_SCROLL) & 1) && !overGui) { data = 0; holdPhase = 1; cInjUp++; }
        else return;
        DWORD* e = (DWORD*)(base + (size_t)n * cb);
        memset(e, 0, cb);
        e[0] = OFS_B0; e[1] = data; e[2] = now; e[3] = ++holdSeq;
        *pn = n + 1;
    }

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* pp)
    {
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputDeviceA)) {
            AddRef(); *pp = (IDirectInputDeviceA*)this; return S_OK;
        }
        // ★ 안 감싼 인터페이스는 진짜를 그대로 준다 — 그 길로 읽으면 거르기를 우회한다
        Log("[%s] ★ 안 감싼 인터페이스를 그대로 넘김", name);
        return r->QueryInterface(riid, pp);
    }
    STDMETHOD_(ULONG, AddRef)() { InterlockedIncrement(&ref); return r->AddRef(); }
    STDMETHOD_(ULONG, Release)()
    {
        ULONG n = r->Release();
        if (InterlockedDecrement(&ref) == 0) {
            LogCounts("Release");
            EnterCriticalSection(&g_devcs);
            if (g_mouseDev == this) g_mouseDev = NULL;
            if (g_kbDev == this)    g_kbDev = NULL;
            LeaveCriticalSection(&g_devcs);
            delete this;
        }
        return n;
    }
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS p) { return r->GetCapabilities(p); }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID pv, DWORD f) { return r->EnumObjects(cb, pv, f); }
    STDMETHOD(GetProperty)(REFGUID g, LPDIPROPHEADER p) { return r->GetProperty(g, p); }
    STDMETHOD(SetProperty)(REFGUID g, LPCDIPROPHEADER p) { return r->SetProperty(g, p); }
    STDMETHOD(Acquire)() { return r->Acquire(); }
    STDMETHOD(Unacquire)() { return r->Unacquire(); }

    STDMETHOD(GetDeviceState)(DWORD cb, LPVOID p)
    {
        HRESULT hr = r->GetDeviceState(cb, p);
        if (SUCCEEDED(hr) && p && cb) {
            bool fg = InForeground();
            NoteFg(fg);
            if (!fg) { memset(p, 0, cb); cStateZeroed++; }
        }
        return hr;
    }
    STDMETHOD(GetDeviceData)(DWORD cb, LPDIDEVICEOBJECTDATA p, LPDWORD pn, DWORD f)
    {
        const DWORD cap = pn ? *pn : 0;                  // ★ 11-20 — 들어올 때 *pn = 그릇 칸 수(꾹 누르기를 붙일 자리)
        HRESULT hr = r->GetDeviceData(cb, p, pn, f);
        if (FAILED(hr)) { cCallFail++; return hr; }
        cCallOk++;
        // 개수만 묻거나 비우는 호출(p == NULL)은 그대로 둔다 — 넘기는 이벤트가 없다
        if (!p || !pn || cb < 2 * sizeof(DWORD)) return hr;

        bool fg = InForeground();
        NoteFg(fg);
        DWORD n = *pn;
        if (fg) {
            cFg += (long)n;
            if (isMouse && !(f & DIGDD_PEEK)) HoldRepeat((BYTE*)p, cb, pn, cap);   // 엿보기는 안 건드린다(같은 것을 다시 읽는다)
            return hr;
        }
        // ★ 11-20 — 뒤로 가면 꾹 누르기를 잊는다(뒤에서 온 뗌을 HoldRepeat 가 못 봐 앞으로 돌아왔을 때 안 눌린 버튼을 되풀이하지 않게)
        if (isMouse) { holdDown = false; holdPhase = 0; }

        // 한 칸 크기는 cb 를 따른다(DirectInput 판마다 구조체 크기가 다르다). 앞 두 칸 dwOfs·dwData 만 본다
        BYTE* base = (BYTE*)p;
        DWORD kept = 0;
        for (DWORD i = 0; i < n; i++) {
            BYTE* e = base + (size_t)i * cb;
            DWORD ofs = ((DWORD*)e)[0], data = ((DWORD*)e)[1];
            if (KeepInBackground(isMouse, ofs, data)) {
                if (kept != i) memmove(base + (size_t)kept * cb, e, cb);
                kept++;
            }
        }
        cBgKept += (long)kept;
        cBgDropped += (long)(n - kept);
        *pn = kept;
        return hr;
    }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT p) { return r->SetDataFormat(p); }
    STDMETHOD(SetEventNotification)(HANDLE h) { return r->SetEventNotification(h); }
    STDMETHOD(SetCooperativeLevel)(HWND h, DWORD f)
    {
        // ★ 녹스는 마우스를 EXCLUSIVE | FOREGROUND 로 잡으려 한다 — 창이 뒤에 있을 때 Acquire 가
        //   0x80070005 로 실패하고 게임이 꺼진다. BACKGROUND 로 바꾸고, 뒤에 있을 때 거르기는 위에서 한다.
        //   EXCLUSIVE 를 떼면 실제 마우스를 안 뺏어 다른 창으로 옮겨 갈 수 있다.
        DWORD want = f;
        if (isMouse) want = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
        char a[64], b[64];
        Log("[%s] SetCooperativeLevel 요청=%s → 넘긴것=%s", name, CoopStr(f, a), CoopStr(want, b));
        return r->SetCooperativeLevel(h, want);
    }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA p, DWORD o, DWORD h) { return r->GetObjectInfo(p, o, h); }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA p) { return r->GetDeviceInfo(p); }
    STDMETHOD(RunControlPanel)(HWND h, DWORD f) { return r->RunControlPanel(h, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v, REFGUID g) { return r->Initialize(h, v, g); }
};

// ─────────────────────────────────────────────── DirectInput 껍데기

class DIProxy : public IDirectInputA
{
public:
    IDirectInputA* r;
    LONG ref;
    DIProxy(IDirectInputA* real) : r(real), ref(1) {}

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* pp)
    {
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputA)) {
            AddRef(); *pp = (IDirectInputA*)this; return S_OK;
        }
        Log("[DI] ★ 안 감싼 인터페이스를 그대로 넘김");
        return r->QueryInterface(riid, pp);
    }
    STDMETHOD_(ULONG, AddRef)() { InterlockedIncrement(&ref); return r->AddRef(); }
    STDMETHOD_(ULONG, Release)()
    {
        ULONG n = r->Release();
        if (InterlockedDecrement(&ref) == 0) delete this;
        return n;
    }
    STDMETHOD(CreateDevice)(REFGUID g, LPDIRECTINPUTDEVICEA* pp, LPUNKNOWN unk)
    {
        LPDIRECTINPUTDEVICEA real = NULL;
        HRESULT hr = r->CreateDevice(g, &real, unk);
        if (SUCCEEDED(hr) && real) {
            DevProxy* dv = new DevProxy(real, g);
            EnterCriticalSection(&g_devcs);
            if (dv->isMouse) g_mouseDev = dv;
            else if (IsEqualGUID(g, GUID_SysKeyboard)) g_kbDev = dv;
            LeaveCriticalSection(&g_devcs);
            *pp = dv;
            return hr;
        }
        *pp = real;
        return hr;
    }
    STDMETHOD(EnumDevices)(DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID pv, DWORD f) { return r->EnumDevices(t, cb, pv, f); }
    STDMETHOD(GetDeviceStatus)(REFGUID g) { return r->GetDeviceStatus(g); }
    STDMETHOD(RunControlPanel)(HWND h, DWORD f) { return r->RunControlPanel(h, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v) { return r->Initialize(h, v); }
};

// ─────────────────────────────────────────────── GetAsyncKeyState (윈도 전체 키 상태)

typedef SHORT(WINAPI* PFN_GAKS)(int);
static PFN_GAKS g_realGAKS = NULL;
static volatile LONG g_gaksCalls = 0, g_gaksFgDown = 0, g_gaksBgBlocked = 0;
static volatile BYTE g_vkAsked[256];       // 게임이 물어본 키 — 무엇에 쓰는지 가르는 용

static SHORT WINAPI HookGAKS(int vk)
{
    InterlockedIncrement(&g_gaksCalls);
    g_vkAsked[vk & 0xff] = 1;
    SHORT r = g_realGAKS(vk);
    if (!InForeground()) {
        if (r) InterlockedIncrement(&g_gaksBgBlocked);
        return 0;
    }
    if (r & 0x8000) InterlockedIncrement(&g_gaksFgDown);
    return r;
}

// 실행파일 수입표(IAT)에서 dll!fn 칸을 hook 으로 바꾼다.
//   이름표(OriginalFirstThunk)가 있으면 이름으로, 없으면 지금 칸에 든 주소로 찾는다
static bool PatchIAT(HMODULE mod, const char* dll, const char* fn, void* hook, void** real)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return false;
    void* want = (void*)GetProcAddress(GetModuleHandleA(dll), fn);
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA* names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : NULL;
        for (int i = 0; iat[i].u1.Function; i++) {
            bool hit;
            if (names) {
                if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                hit = strcmp((const char*)ibn->Name, fn) == 0;
            } else {
                hit = (void*)iat[i].u1.Function == want;
            }
            if (!hit) continue;
            DWORD old;
            if (!VirtualProtect(&iat[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *real = (void*)iat[i].u1.Function;
            iat[i].u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat[i].u1.Function, sizeof(void*), old, &old);
            Log("수입표 바꿈 %s!%s (%s) 원래=%p 진짜=%p", dll, fn, names ? "이름으로" : "주소로", *real, want);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────── 2초 요약 (로그 켰을 때만)

static DWORD WINAPI SummaryThread(LPVOID)
{
    for (;;) {
        Sleep(2000);
        long m[6] = { 0 }, k[6] = { 0 }, inj[2] = { 0 }, pr[4] = { 0 };
        EnterCriticalSection(&g_devcs);
        if (g_mouseDev) { m[0] = g_mouseDev->cCallOk; m[1] = g_mouseDev->cCallFail; m[2] = g_mouseDev->cFg; m[3] = g_mouseDev->cBgKept; m[4] = g_mouseDev->cBgDropped; m[5] = 1; inj[0] = g_mouseDev->cInjUp; inj[1] = g_mouseDev->cInjDown;
                          pr[0] = g_mouseDev->cPressWorld; pr[1] = g_mouseDev->cPressGui; pr[2] = g_mouseDev->cPressBlind; pr[3] = g_mouseDev->cStopGui; }
        if (g_kbDev)    { k[0] = g_kbDev->cCallOk;    k[1] = g_kbDev->cCallFail;    k[2] = g_kbDev->cFg;    k[3] = g_kbDev->cBgKept;    k[4] = g_kbDev->cBgDropped;    k[5] = 1; }
        LeaveCriticalSection(&g_devcs);
        char vks[256 * 3 + 1]; vks[0] = 0;
        for (int i = 0; i < 256; i++) if (g_vkAsked[i]) { char t[8]; sprintf(t, "%02X ", i); strcat(vks, t); }
        Log("[요약] 앞=%s | 마우스 호출 %ld/실패 %ld · 앞 %ld · 뒤 남김 %ld · 뒤 버림 %ld · 꾹 누르기 뗌 %ld/누름 %ld · 왼쪽 누름 화면 %ld/창 %ld/칸 못 믿음 %ld · 창 위라 멈춤 %ld | 키보드 호출 %ld/실패 %ld · 앞 %ld · 뒤 남김 %ld · 뒤 버림 %ld | GetAsyncKeyState 호출 %ld · 앞 눌림 %ld · 뒤 막음 %ld · 물은 키 %s",
            InForeground() ? "나" : "남", m[0], m[1], m[2], m[3], m[4], inj[0], inj[1], pr[0], pr[1], pr[2], pr[3], k[0], k[1], k[2], k[3], k[4],
            g_gaksCalls, g_gaksFgDown, g_gaksBgBlocked, vks[0] ? vks : "-");
    }
}

// ─────────────────────────────────────────────── 내보내는 함수들

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD ver, LPDIRECTINPUTA* pp, LPUNKNOWN unk)
{
    // 수입표는 처음 한 번만 바꾼다 — 모든 dll 의 DllMain 이 끝난 뒤라 다른 후크와 순서가 덜 꼬인다
    static LONG patched = 0;
    if (InterlockedExchange(&patched, 1) == 0 &&
        !PatchIAT(GetModuleHandleA(NULL), "USER32.dll", "GetAsyncKeyState", (void*)HookGAKS, (void**)&g_realGAKS))
        Log("수입표에 USER32.dll!GetAsyncKeyState 없음 — 안 바꿈");
    if (!LoadReal()) return E_FAIL;
    PFN_DICreateA fn = (PFN_DICreateA)GetProcAddress(g_real, "DirectInputCreateA");
    if (!fn) return E_FAIL;
    LPDIRECTINPUTA real = NULL;
    HRESULT hr = fn(hinst, ver, &real, unk);
    Log("DirectInputCreateA(ver=0x%04lX) hr=0x%08lX", ver, hr);
    if (SUCCEEDED(hr) && real) { *pp = new DIProxy(real); return hr; }
    *pp = real;
    return hr;
}

extern "C" HRESULT WINAPI DirectInputCreateW(HINSTANCE hinst, DWORD ver, LPDIRECTINPUTW* pp, LPUNKNOWN unk)
{
    Log("DirectInputCreateW(ver=0x%04lX) — 안 감싸고 그대로 넘김", ver);
    if (!LoadReal()) return E_FAIL;
    PFN_DICreateW fn = (PFN_DICreateW)GetProcAddress(g_real, "DirectInputCreateW");
    return fn ? fn(hinst, ver, pp, unk) : E_FAIL;
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* pp, LPUNKNOWN unk)
{
    Log("DirectInputCreateEx(ver=0x%04lX) — 안 감싸고 그대로 넘김", ver);
    if (!LoadReal()) return E_FAIL;
    PFN_DICreateEx fn = (PFN_DICreateEx)GetProcAddress(g_real, "DirectInputCreateEx");
    return fn ? fn(hinst, ver, riid, pp, unk) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    if (!LoadReal()) return S_FALSE;
    PFN_Plain fn = (PFN_Plain)GetProcAddress(g_real, "DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* pp)
{
    if (!LoadReal()) return E_FAIL;
    PFN_GetClassObject fn = (PFN_GetClassObject)GetProcAddress(g_real, "DllGetClassObject");
    return fn ? fn(rclsid, riid, pp) : E_FAIL;
}
extern "C" HRESULT WINAPI DllRegisterServer(void)
{
    if (!LoadReal()) return E_FAIL;
    PFN_Plain fn = (PFN_Plain)GetProcAddress(g_real, "DllRegisterServer");
    return fn ? fn() : E_FAIL;
}
extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
    if (!LoadReal()) return E_FAIL;
    PFN_Plain fn = (PFN_Plain)GetProcAddress(g_real, "DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

// ─────────────────────────────────────────────── 소환 창 넓히기 (촬영 판만 · NOX_SUMMON_WIDE=1)
// ★ 11-22 트레일러 작업 — 사용자 「몬스터 현황판 같은 거를 여러 개로는 못 하나」(소환 한도 12 촬영 판 · 큰 소환수 셋을 한 화면에).
//   정품 소환 창은 2×2 격자(38px 칸)라 큰 소환수(4칸) 하나가 창 전체 — 둘째부터는 자리를 못 찾아 첫 자리에 겹쳐 그려진다
//   (opennox `GAME3_1.c` 크기 `sub_4C2EF0` · 자리 찾기 `sub_4C2FD0` — 이 실행 파일에선 코드 +0x960 · 메모리 −0x23E0 밀려 있다).
//   → 격자를 2줄 × 6칸(12칸 = 한도 12)으로 넓힌다. 칸 번호 = 줄 + 2 × 열 이라 줄이 2인 한 셈법은 그대로, 바꿀 것은
//     ① 격자 16바이트(0x714A90 · 바로 뒤에 개수 0x714AA0·상태 0x714AA4)를 이 DLL 의 48바이트로 옮김 ② 열 상한 2 → 6
//     ③ 자리 후보 표(0x5B1C68 · 8바이트 × 4 = (열,줄) 0,0 0,1 1,0 1,1)를 12개로 ④ 창 두 개(바깥 87 · 격자 76)를 152 넓히고 왼쪽으로 152 옮김.
//     ⑤ 목록(0x714A10 · 32바이트 × 4 — 번호·종류·씀·열·줄·크기·칸 번호·밝힘)을 이 DLL 의 12칸으로 옮김 — 처음엔 4칸으로 두었다가
//       11-22 사용자 「3배였나 4배까지 늘려 달라고 하지 않았나」 · 「소환한 대로 창이 잘 떠 있는지」 — 클라 메모리를 읽어 보니
//       말벌·트롤·곰·기계 골렘 넷만 있고 다섯째 임프가 빠져 있었다(목록이 차면 `sub_4C2F20` 이 0 을 돌려 안 넣는다).
//       개수(0x714AA0)를 4 와 견주는 곳은 없다(opennox 에서 1321196 을 쓰는 곳 = 늘리기·줄이기·0 검사뿐).
//   기계어는 GOG Game.exe(PE 시각 0x3978A3FE)를 capstone 으로 역어셈블해 골랐다 — 목록 주소가 박힌 dword 를 .text 전체에서 찾은 18곳.
//   바꾸기 전에 31곳 모두 원래 바이트가 맞는지 보고, 하나라도 다르면 하나도 안 바꾼다.
#define SUMMON_COLS     6
#define SUMMON_EXTRA_W  (38 * (SUMMON_COLS - 2))   // 152 — 늘어난 열 넷
#define SUMMON_ENTS     (2 * SUMMON_COLS)          // 12 — 목록 칸(원래 4) = 격자 칸 수 = 한도 12 를 작은 것으로 채운 수
static volatile DWORD g_sumGrid[2 * SUMMON_COLS];
static volatile DWORD g_sumEnt[8 * SUMMON_ENTS];   // 32바이트 × 12
#define SUMMON_ENT_OLD  0x714A10u
struct EntFix { DWORD va; BYTE plen; BYTE pre[2]; DWORD oldAddr; DWORD newOff; const char* what; };
static const EntFix kEntFix[] = {   // 명령 앞머리(plen 바이트) 뒤에 목록 주소 dword 가 박힌 자리 — 앞머리 + 주소만 바꾼다
    { 0x4C2909, 1, {0xB8},      0x714A18, 8,                     "창 만들 때 비우기 — 처음(씀 칸)" },
    { 0x4C2913, 1, {0x3D},      0x714A98, 8 + 32 * SUMMON_ENTS,  "창 만들 때 비우기 — 끝" },
    { 0x4C2A52, 1, {0xBB},      0x714A1C, 12,                    "그리기 — 처음(열 칸)" },
    { 0x4C2C3F, 2, {0x81,0xFB}, 0x714A9C, 12 + 32 * SUMMON_ENTS, "그리기 — 끝" },
    { 0x4C36C2, 1, {0xB9},      0x714A18, 8,                     "첫 칸 — 처음" },
    { 0x4C36D0, 2, {0x81,0xF9}, 0x714A98, 8 + 32 * SUMMON_ENTS,  "첫 칸 — 끝" },
    { 0x4C36DE, 1, {0x05},      0x714A10, 0,                     "첫 칸 — 돌려줄 자리" },
    { 0x4C3702, 2, {0x81,0xC1}, 0x714A18, 8,                     "다음 칸 — 처음" },
    { 0x4C3711, 2, {0x81,0xF9}, 0x714A98, 8 + 32 * SUMMON_ENTS,  "다음 칸 — 끝" },
    { 0x4C371F, 1, {0x05},      0x714A10, 0,                     "다음 칸 — 돌려줄 자리" },
    { 0x4C3883, 1, {0xB8},      0x714A18, 8,                     "빈 칸 찾기 — 처음" },
    { 0x4C3891, 1, {0x3D},      0x714A98, 8 + 32 * SUMMON_ENTS,  "빈 칸 찾기 — 끝" },
    { 0x4C38A8, 2, {0x8D,0x9A}, 0x714A10, 0,                     "빈 칸 찾기 — 지울 자리" },
    { 0x4C38B4, 2, {0xC7,0x82}, 0x714A18, 8,                     "빈 칸 찾기 — 씀 표시" },
    { 0x4C38C1, 2, {0x89,0xB2}, 0x714A28, 24,                    "빈 칸 찾기 — 칸 번호" },
    { 0x4C3B37, 1, {0xB9},      0x714A10, 0,                     "번호로 찾기 — 처음" },
    { 0x4C3B4B, 2, {0x81,0xF9}, 0x714A90, 32 * SUMMON_ENTS,      "번호로 찾기 — 끝" },
    { 0x4C3B5A, 1, {0x05},      0x714A10, 0,                     "번호로 찾기 — 돌려줄 자리" },
};
static const int g_sumPos[2 * SUMMON_COLS][2] = {  // 원래 표와 같은 차례 — 번호 i = (열 i/2, 줄 i%2). 큰 것(4)은 0·4·8 = 열 0·2·4
    {0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}, {4, 0}, {4, 1}, {5, 0}, {5, 1} };
typedef int(__cdecl* PFN_WinNew)(int parent, int flags, int x, int y, int w, int h, int fnc);
#define GAME_WINDOW_NEW  0x489CF0u   // nox_window_new — 호출한 쪽이 스택을 치운다(0x4C2747 add esp, 0x48 = 창 둘 + 함수 걸기 하나)
#define SUMMON_WIN_X     0x7149D0u   // dword_5d4594_1320988 — 창 가로 자리(만들 때 한 번 쓰고 · 그리기 함수가 매 프레임 이 값으로 옮김)
static volatile LONG g_sumWinMade = 0;
static int __cdecl HookSummonOuterNew(int parent, int flags, int x, int y, int w, int h, int fnc)
{
    x -= SUMMON_EXTRA_W; w += SUMMON_EXTRA_W;
    *(volatile int*)SUMMON_WIN_X = x;   // 만들기 바로 앞(0x4C2707)에서 원래 값(화면 너비 − 95)을 써 둔 칸
    Log("[소환 창 넓힘] 바깥 창 만듦 %ld번째 — 가로 자리 %d · 너비 %d", InterlockedIncrement(&g_sumWinMade), x, w);
    return ((PFN_WinNew)GAME_WINDOW_NEW)(parent, flags, x, y, w, h, fnc);
}
static int __cdecl HookSummonGridNew(int parent, int flags, int x, int y, int w, int h, int fnc)
{
    w += SUMMON_EXTRA_W;
    return ((PFN_WinNew)GAME_WINDOW_NEW)(parent, flags, x, y, w, h, fnc);
}
struct CodePatch { DWORD va; BYTE len; BYTE orig[7]; BYTE now[7]; const char* what; };
static void PutDword(BYTE* p, DWORD v) { memcpy(p, &v, 4); }
static void PatchSummonWindow()
{
    char v[8];
    if (!GetEnvironmentVariableA("NOX_SUMMON_WIDE", v, sizeof v) || v[0] != '1') return;
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    DWORD stamp = 0;
    if (base == (const BYTE*)0x400000) {
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        stamp = nt->FileHeader.TimeDateStamp;
    }
    if (stamp != GAME_EXE_STAMP) { Log("[소환 창 넓힘] 실행 파일 시각 0x%08lX — 다른 판이라 안 바꿈", stamp); return; }
    const DWORD grid = (DWORD)(uintptr_t)g_sumGrid, pos = (DWORD)(uintptr_t)g_sumPos;
    CodePatch p[] = {
        { 0x4C3550, 5, {0xBA,0x90,0x4A,0x71,0x00},           {0xBA},             "지우기 — 격자 처음" },
        { 0x4C3557, 5, {0xB9,0x02,0x00,0x00,0x00},           {0xB9,SUMMON_COLS,0,0,0}, "지우기 — 열 수 2" },
        { 0x4C356B, 6, {0x81,0xFA,0x98,0x4A,0x71,0x00},      {0x81,0xFA},        "지우기 — 줄 끝(격자 + 8)" },
        { 0x4C2D80, 3, {0x83,0xFA,0x02},                     {0x83,0xFA,SUMMON_COLS}, "칸 얻기 — 열 < 2" },
        { 0x4C2D94, 7, {0x8B,0x04,0x85,0x90,0x4A,0x71,0x00}, {0x8B,0x04,0x85},   "칸 얻기 — 격자" },
        { 0x4C39ED, 7, {0x8D,0x0C,0x8D,0x90,0x4A,0x71,0x00}, {0x8D,0x0C,0x8D},   "놓기 — 격자" },
        { 0x4C3A74, 7, {0x8D,0x0C,0x8D,0x90,0x4A,0x71,0x00}, {0x8D,0x0C,0x8D},   "비었나 — 격자" },
        { 0x4C393C, 7, {0x8B,0x04,0xF5,0x68,0x1C,0x5B,0x00}, {0x8B,0x04,0xF5},   "자리 찾기 — 표 열" },
        { 0x4C394A, 7, {0x8B,0x0C,0xF5,0x6C,0x1C,0x5B,0x00}, {0x8B,0x0C,0xF5},   "자리 찾기 — 표 줄" },
        { 0x4C3967, 3, {0x83,0xFE,0x04},                     {0x83,0xFE,2 * SUMMON_COLS}, "자리 찾기 — 후보 < 4" },
        { 0x4C3970, 3, {0x83,0xFE,0x04},                     {0x83,0xFE,2 * SUMMON_COLS}, "자리 찾기 — 찾았나 < 4" },
        { 0x4C2712, 5, {0xE8,0xD9,0x75,0xFC,0xFF},           {0xE8},             "바깥 창 만들기" },
        { 0x4C2742, 5, {0xE8,0xA9,0x75,0xFC,0xFF},           {0xE8},             "격자 창 만들기" },
    };
    PutDword(p[0].now + 1, grid);
    PutDword(p[2].now + 2, grid + 8);
    PutDword(p[4].now + 3, grid);
    PutDword(p[5].now + 3, grid);
    PutDword(p[6].now + 3, grid);
    PutDword(p[7].now + 3, pos);
    PutDword(p[8].now + 3, pos + 4);
    PutDword(p[11].now + 1, (DWORD)(uintptr_t)HookSummonOuterNew - (p[11].va + 5));
    PutDword(p[12].now + 1, (DWORD)(uintptr_t)HookSummonGridNew - (p[12].va + 5));
    // 목록 18곳을 같은 꼴로 붙인다 — 원래 = 앞머리 + 옛 주소 · 새로 = 앞머리 + (DLL 목록 + 같은 칸 거리)
    const int np = (int)(sizeof p / sizeof p[0]), ne = (int)(sizeof kEntFix / sizeof kEntFix[0]);
    CodePatch all[sizeof p / sizeof p[0] + sizeof kEntFix / sizeof kEntFix[0]];
    memcpy(all, p, sizeof p);
    const DWORD ent = (DWORD)(uintptr_t)g_sumEnt;
    for (int i = 0; i < ne; i++) {
        const EntFix& f = kEntFix[i];
        CodePatch& c = all[np + i];
        memset(&c, 0, sizeof c);
        c.va = f.va; c.len = (BYTE)(f.plen + 4); c.what = f.what;
        memcpy(c.orig, f.pre, f.plen); PutDword(c.orig + f.plen, f.oldAddr);
        memcpy(c.now, f.pre, f.plen);  PutDword(c.now + f.plen, ent + f.newOff);
    }
    const int n = np + ne;
    for (int i = 0; i < n; i++)
        if (memcmp((const void*)(uintptr_t)all[i].va, all[i].orig, all[i].len) != 0) {
            Log("[소환 창 넓힘] 0x%08lX(%s) 원래 바이트가 다름 — 하나도 안 바꿈", all[i].va, all[i].what);
            return;
        }
    int done = 0;
    for (int i = 0; i < n; i++) {
        DWORD old = 0;
        if (!VirtualProtect((void*)(uintptr_t)all[i].va, all[i].len, PAGE_EXECUTE_READWRITE, &old)) {
            Log("[소환 창 넓힘] 0x%08lX(%s) 쓰기 허락 실패 %lu — 여기서 멈춤(앞 %d곳은 바뀜)", all[i].va, all[i].what, GetLastError(), done);
            return;
        }
        memcpy((void*)(uintptr_t)all[i].va, all[i].now, all[i].len);
        VirtualProtect((void*)(uintptr_t)all[i].va, all[i].len, old, &old);
        ++done;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    Log("[소환 창 넓힘] %d / %d곳 바꿈 — 격자 2×%d(%p) · 자리 표 %d개(%p) · 목록 %d칸(%p) · 창 +%dpx", done, n, SUMMON_COLS, (void*)g_sumGrid,
        2 * SUMMON_COLS, (void*)g_sumPos, SUMMON_ENTS, (void*)g_sumEnt, SUMMON_EXTRA_W);
}

// ─────────────────────────────────────────────── 마법 세트 지키기 (촬영 판만 · NOX_KEEP_SPELLSET=1)
// ★ 11-22 사용자 「내가 스킬 세팅해 놓은 건 왜 자꾸 돌아가는 거야」 → 「DLL로 막아줘」.
//   정품 클라는 게임에 들어갈 때마다 캐릭터 파일을 다시 읽는데(opennox `game.go:600-602` → `nox_xxx_plrLoad_41A480`),
//   읽기 전에 마법 세트 다섯 줄을 통째로 비운다(`GAME1_1.c:844` `sub_4602F0`). 캐릭터 파일 구역은 셋(머리·정보·`sub_41C780`)뿐이라
//   세트는 되살아나지 않고, 서버가 주문을 다시 줄 때 빈 칸부터 기본 차례로 채워진다 — 서버를 고쳐서는 못 막는다.
//   → 비우기 함수(이 실행 파일 0x45FE40 · opennox 0x4602F0 에서 −0x4B0) 첫머리를 우리 함수로 건너뛰게 해 세트를 그대로 둔다.
//     부르는 곳 넷(캐릭터 파일 읽기 0x41A33B · 창 열기 0x4A3C72 · 꼬리로 건너뛰는 0x45FEE5 · 0x4A49FC)이 모두 막힌다.
//     원래 함수가 맨 먼저 하던 펼친 세트 창 닫기(0x460200 `nox_xxx_quickBarClose_4606B0` — 창 쪽 값을 세트에 되돌려 적는다)만 대신 부른다.
//   한계: 클라 메모리에만 있어 클라를 다시 켜면 비어 있다 · 다른 직업으로 들어가면 옛 직업 세트가 남는다(촬영 판은 한 직업).
#define GAME_SPELLSET_CLEAR  0x45FE40u
#define GAME_QUICKBAR_CLOSE  0x460200u
static volatile LONG g_keepSpellHits = 0;
static int __cdecl HookSpellSetClear()
{
    void* back = _ReturnAddress();
    ((int(__cdecl*)())GAME_QUICKBAR_CLOSE)();
    Log("[마법 세트 지킴] 비우기 막음 %ld번째 — 돌아갈 곳 %p", InterlockedIncrement(&g_keepSpellHits), back);
    return 0;   // 돌려준 값 — 0x41A340 은 안 씀 · 0x4A3C77 은 eax 를 1 로 덮음(역어셈블) · 꼬리 둘은 opennox 에서 부르는 쪽이 안 씀
}
static void PatchKeepSpellSet()
{
    char v[8];
    if (!GetEnvironmentVariableA("NOX_KEEP_SPELLSET", v, sizeof v) || v[0] != '1') return;
    const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
    DWORD stamp = 0;
    if (base == (const BYTE*)0x400000) {
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        stamp = nt->FileHeader.TimeDateStamp;
    }
    if (stamp != GAME_EXE_STAMP) { Log("[마법 세트 지킴] 실행 파일 시각 0x%08lX — 다른 판이라 안 바꿈", stamp); return; }
    // 원래 = push ebx · push ebp · push esi · push edi · call 0x460200 의 첫 바이트 — 건너뛰면 뒤 4바이트에는 닿지 않는다
    static const BYTE orig[5] = { 0x53, 0x55, 0x56, 0x57, 0xE8 };
    BYTE now[5] = { 0xE9 };
    PutDword(now + 1, (DWORD)(uintptr_t)HookSpellSetClear - (GAME_SPELLSET_CLEAR + 5));
    if (memcmp((const void*)(uintptr_t)GAME_SPELLSET_CLEAR, orig, sizeof orig) != 0) {
        Log("[마법 세트 지킴] 0x%08lX 원래 바이트가 다름 — 안 바꿈", GAME_SPELLSET_CLEAR);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)(uintptr_t)GAME_SPELLSET_CLEAR, sizeof now, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[마법 세트 지킴] 쓰기 허락 실패 %lu", GetLastError());
        return;
    }
    memcpy((void*)(uintptr_t)GAME_SPELLSET_CLEAR, now, sizeof now);
    VirtualProtect((void*)(uintptr_t)GAME_SPELLSET_CLEAR, sizeof now, old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    Log("[마법 세트 지킴] 0x%08lX 비우기를 건너뛰게 바꿈", GAME_SPELLSET_CLEAR);
}

// ─────────────────────────────────────────────── 들고 나기

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_devcs);

        char dir[MAX_PATH];
        if (GetEnvironmentVariableA("NOX_HOOK_LOG_DIR", dir, MAX_PATH)) {
            char exe[MAX_PATH]; exe[0] = 0;
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
            sprintf(g_log, "%s\\dihook_%s_%lu.log", dir, base, GetCurrentProcessId());
            Log("=== dinput 프록시(입력 가르기) 붙음 ===  exe=%s", exe);
            CloseHandle(CreateThread(NULL, 0, SummaryThread, NULL, 0, NULL));
        }
        PatchSummonWindow();   // 촬영 판만(NOX_SUMMON_WIDE=1) — 창 만들기 전에 바꿔야 해서 붙는 즉시
        PatchKeepSpellSet();   // 촬영 판만(NOX_KEEP_SPELLSET=1) — 게임에 다시 들어가도 마법 세트를 안 비운다
    }
    return TRUE;
}
