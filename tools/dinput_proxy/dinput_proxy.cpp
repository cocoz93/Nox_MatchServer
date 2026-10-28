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
// 옛 판(자동 조작 통로 포함)은 git 46f5830 직전.

#define DIRECTINPUT_VERSION 0x0700
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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

    DevProxy(IDirectInputDeviceA* real, REFGUID g)
        : r(real), ref(1), cFg(0), cBgKept(0), cBgDropped(0), cStateZeroed(0), cCallOk(0), cCallFail(0), lastFg(-1)
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
        HRESULT hr = r->GetDeviceData(cb, p, pn, f);
        if (FAILED(hr)) { cCallFail++; return hr; }
        cCallOk++;
        // 개수만 묻거나 비우는 호출(p == NULL)은 그대로 둔다 — 넘기는 이벤트가 없다
        if (!p || !pn || cb < 2 * sizeof(DWORD)) return hr;

        bool fg = InForeground();
        NoteFg(fg);
        DWORD n = *pn;
        if (fg) { cFg += (long)n; return hr; }

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
        long m[6] = { 0 }, k[6] = { 0 };
        EnterCriticalSection(&g_devcs);
        if (g_mouseDev) { m[0] = g_mouseDev->cCallOk; m[1] = g_mouseDev->cCallFail; m[2] = g_mouseDev->cFg; m[3] = g_mouseDev->cBgKept; m[4] = g_mouseDev->cBgDropped; m[5] = 1; }
        if (g_kbDev)    { k[0] = g_kbDev->cCallOk;    k[1] = g_kbDev->cCallFail;    k[2] = g_kbDev->cFg;    k[3] = g_kbDev->cBgKept;    k[4] = g_kbDev->cBgDropped;    k[5] = 1; }
        LeaveCriticalSection(&g_devcs);
        char vks[256 * 3 + 1]; vks[0] = 0;
        for (int i = 0; i < 256; i++) if (g_vkAsked[i]) { char t[8]; sprintf(t, "%02X ", i); strcat(vks, t); }
        Log("[요약] 앞=%s | 마우스 호출 %ld/실패 %ld · 앞 %ld · 뒤 남김 %ld · 뒤 버림 %ld | 키보드 호출 %ld/실패 %ld · 앞 %ld · 뒤 남김 %ld · 뒤 버림 %ld | GetAsyncKeyState 호출 %ld · 앞 눌림 %ld · 뒤 막음 %ld · 물은 키 %s",
            InForeground() ? "나" : "남", m[0], m[1], m[2], m[3], m[4], k[0], k[1], k[2], k[3], k[4],
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
    }
    return TRUE;
}
