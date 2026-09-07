// 녹스 입력 가로채기 — dinput.dll 프록시
//
// 실측으로 확정된 녹스의 입력 짜임 (1단계 로그):
//   DirectInputCreateA(ver=0x0300)  — DirectInput 3
//   마우스·키보드 둘 다 DirectInput · SetCooperativeLevel = NONEXCLUSIVE | BACKGROUND
//   마우스 자료형식 16바이트 7항목 = DIMOUSESTATE (축 3 + 버튼 4)
//   ★ 읽는 방식은 GetDeviceState 가 아니라 GetDeviceData — 버퍼에서 이벤트를 꺼내 간다
//
// 그래서 2단계는 "상태를 지어내는" 것이 아니라 "이벤트를 큐에 넣어주는" 것이다.
//
// 조종 통로: 이름 붙인 파이프  \\.\pipe\noxdi_<pid>
//   inject 1|0     주입 켜기/끄기 (기본 꺼짐 — 안 켜면 게임은 평소와 완전히 똑같다)
//   m <dx> <dy>    마우스 이동 (진짜 델타. OS 가속이 안 끼므로 그대로 들어간다)
//   mb <n> <0|1>   마우스 버튼 n 떼기/누르기
//   k <scan> <0|1> 키 (DIK 스캔코드)
//   ping           살아 있나
//
// 빌드: build.bat  (x86 필수 — Game.exe 가 32비트다)
// 놓는 곳: 게임 폴더. 실행파일 폴더가 DLL 검색 1순위라 시스템 것보다 먼저 잡힌다.
// 되돌리기: 이 파일 삭제.

#define DIRECTINPUT_VERSION 0x0700
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>

// ─────────────────────────────────────────────── 로그

static char  g_log[MAX_PATH] = { 0 };
static CRITICAL_SECTION g_cs;
static bool  g_csOk = false;

static void Log(const char* fmt, ...)
{
    if (!g_log[0]) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    if (g_csOk) EnterCriticalSection(&g_cs);
    FILE* f = fopen(g_log, "a");
    if (f) {
        fprintf(f, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
    if (g_csOk) LeaveCriticalSection(&g_cs);
}

// 아는 GUID 는 이름으로, 모르는 것은 숫자로 적는다.
static const char* GuidName(REFGUID g)
{
    static char tmp[80];
    // ★ DirectInput 속성은 진짜 GUID 가 아니다 — MAKEDIPROP(n) 은 정수 n 을 포인터로 위장한 것이라
    //   역참조하면 그 자리에서 터진다. 반드시 먼저 걸러야 한다.
    ULONG_PTR v = (ULONG_PTR)&g;
    if (v < 0x10000) {
        static const char* prop[] = { "?", "DIPROP_BUFFERSIZE", "DIPROP_AXISMODE", "DIPROP_GRANULARITY",
            "DIPROP_RANGE", "DIPROP_DEADZONE", "DIPROP_SATURATION", "DIPROP_FFGAIN", "DIPROP_FFLOAD",
            "DIPROP_AUTOCENTER", "DIPROP_CALIBRATIONMODE" };
        if (v >= 1 && v <= 10) { sprintf(tmp, "%s", prop[v]); return tmp; }
        sprintf(tmp, "DIPROP #%lu", (unsigned long)v);
        return tmp;
    }
    if (IsEqualGUID(g, GUID_SysMouse))            return "GUID_SysMouse";
    if (IsEqualGUID(g, GUID_SysKeyboard))         return "GUID_SysKeyboard";
    if (IsEqualGUID(g, IID_IUnknown))             return "IID_IUnknown";
    if (IsEqualGUID(g, IID_IDirectInputA))        return "IID_IDirectInputA";
    if (IsEqualGUID(g, IID_IDirectInputW))        return "IID_IDirectInputW";
    if (IsEqualGUID(g, IID_IDirectInput2A))       return "IID_IDirectInput2A";
    if (IsEqualGUID(g, IID_IDirectInput7A))       return "IID_IDirectInput7A";
    if (IsEqualGUID(g, IID_IDirectInputDeviceA))  return "IID_IDirectInputDeviceA";
    if (IsEqualGUID(g, IID_IDirectInputDevice2A)) return "IID_IDirectInputDevice2A";
    if (IsEqualGUID(g, IID_IDirectInputDevice7A)) return "IID_IDirectInputDevice7A";
    if (IsEqualGUID(g, GUID_XAxis))               return "GUID_XAxis";
    if (IsEqualGUID(g, GUID_YAxis))               return "GUID_YAxis";
    if (IsEqualGUID(g, DIPROP_BUFFERSIZE))        return "DIPROP_BUFFERSIZE";
    if (IsEqualGUID(g, DIPROP_AXISMODE))          return "DIPROP_AXISMODE";
    if (IsEqualGUID(g, DIPROP_GRANULARITY))       return "DIPROP_GRANULARITY";
    sprintf(tmp, "{%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return tmp;
}

// 협조 수준 플래그를 사람 말로.
static const char* CoopStr(DWORD f)
{
    // ★ 한 줄에 두 번 부르면 정적 버퍼 하나로는 둘 다 같은 값이 나온다 — 번갈아 쓴다
    static char sa[128], sb[128];
    static int turn = 0;
    char* s = (turn++ % 2) ? sb : sa;
    s[0] = 0;
    if (f & DISCL_EXCLUSIVE)    strcat(s, "EXCLUSIVE ");
    if (f & DISCL_NONEXCLUSIVE) strcat(s, "NONEXCLUSIVE ");
    if (f & DISCL_FOREGROUND)   strcat(s, "FOREGROUND ");
    if (f & DISCL_BACKGROUND)   strcat(s, "BACKGROUND ");
    if (f & DISCL_NOWINKEY)     strcat(s, "NOWINKEY ");
    if (!s[0]) strcpy(s, "(없음)");
    return s;
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

// ─────────────────────────────────────────────── 주입 통로

static volatile LONG g_inject = 0;     // 0 = 손 안 댐(게임이 평소대로 동작)
static CRITICAL_SECTION g_qcs;
static DWORD g_seq = 0;

// DIMOUSESTATE 안 자리값
#define OFS_X   0
#define OFS_Y   4
#define OFS_Z   8
#define OFS_B0  12

struct EvQueue
{
    DIDEVICEOBJECTDATA buf[512];
    int head, tail;
    EvQueue() : head(0), tail(0) {}
    int Count() const { return (tail - head + 512) % 512; }
    void Push(DWORD ofs, DWORD data)
    {
        int n = (tail + 1) % 512;
        if (n == head) head = (head + 1) % 512;   // 넘치면 오래된 것부터 버린다
        buf[tail].dwOfs = ofs;
        buf[tail].dwData = data;
        buf[tail].dwTimeStamp = GetTickCount();
        buf[tail].dwSequence = ++g_seq;
        tail = n;
    }
    bool Pop(DIDEVICEOBJECTDATA* out)
    {
        if (head == tail) return false;
        *out = buf[head];
        head = (head + 1) % 512;
        return true;
    }
    bool Peek(int i, DIDEVICEOBJECTDATA* out) const
    {
        if (i >= Count()) return false;
        *out = buf[(head + i) % 512];
        return true;
    }
};
// ─────────────────────────────────────────────── 장치 껍데기

class DevProxy : public IDirectInputDeviceA
{
public:
    IDirectInputDeviceA* r;
    LONG  ref;
    GUID  dev;
    const char* devName;
    long  cState, cData, cAcq, cInj;
    bool  isMouse, isKb;
    EvQueue q;
    // ★★ 큐와 **따로** 드는 「지금 눌려 있나」. 큐는 변화를 나르고 이건 현재를 답한다.
    //   하나로 겸하면 GetDeviceData 가 먼저 큐를 비울 때 GetDeviceState 가
    //   아무 일도 없던 것으로 본다 — 홀드가 표현되지 않는다(2026-09-08 실측:
    //   좌버튼을 4초 눌러도 게임이 낸 행동 코드가 1건뿐이었다).
    BYTE  btn[4];
    BYTE  keys[256];

    DevProxy(IDirectInputDeviceA* real, REFGUID g)
        : r(real), ref(1), dev(g), cState(0), cData(0), cAcq(0), cInj(0)
    {
        devName = GuidName(g);
        isMouse = (IsEqualGUID(g, GUID_SysMouse) != 0);
        isKb    = (IsEqualGUID(g, GUID_SysKeyboard) != 0);
        memset(btn, 0, sizeof(btn));
        memset(keys, 0, sizeof(keys));
        Log("[장치] 껍데기 생성 %s  (진짜 %p)", devName, real);
    }

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* pp)
    {
        Log("[장치 %s] QueryInterface(%s)", devName, GuidName(riid));
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputDeviceA)) {
            AddRef(); *pp = (IDirectInputDeviceA*)this; return S_OK;
        }
        // ★ 아직 안 감싸는 인터페이스 — 진짜를 그대로 준다.
        //   이 줄이 찍히면 그 뒤 호출은 내 로그에 안 남는다는 뜻이다.
        HRESULT hr = r->QueryInterface(riid, pp);
        Log("[장치 %s] ★ 안 감싼 인터페이스를 그대로 넘김 hr=0x%08lX", devName, hr);
        return hr;
    }
    STDMETHOD_(ULONG, AddRef)() { InterlockedIncrement(&ref); return r->AddRef(); }
    STDMETHOD_(ULONG, Release)()
    {
        ULONG n = r->Release();
        if (InterlockedDecrement(&ref) == 0) {
            Log("[장치 %s] Release — GetDeviceState %ld회 · GetDeviceData %ld회 · Acquire %ld회",
                devName, cState, cData, cAcq);
            delete this;
        }
        return n;
    }
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS p) { return r->GetCapabilities(p); }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID pv, DWORD f) { return r->EnumObjects(cb, pv, f); }
    STDMETHOD(GetProperty)(REFGUID g, LPDIPROPHEADER p)
    {
        HRESULT hr = r->GetProperty(g, p);
        Log("[장치 %s] GetProperty(%s) hr=0x%08lX", devName, GuidName(g), hr);
        return hr;
    }
    STDMETHOD(SetProperty)(REFGUID g, LPCDIPROPHEADER p)
    {
        DWORD v = 0;
        if (p && p->dwSize >= sizeof(DIPROPDWORD)) v = ((LPCDIPROPDWORD)p)->dwData;
        Log("[장치 %s] SetProperty(%s) 값=%lu", devName, GuidName(g), v);
        return r->SetProperty(g, p);
    }
    STDMETHOD(Acquire)()
    {
        HRESULT hr = r->Acquire();
        if (++cAcq <= 5) Log("[장치 %s] Acquire hr=0x%08lX", devName, hr);
        return hr;
    }
    STDMETHOD(Unacquire)() { Log("[장치 %s] Unacquire", devName); return r->Unacquire(); }

    STDMETHOD(GetDeviceState)(DWORD cb, LPVOID p)
    {
        // ★ 여기서는 **큐를 건드리지 않는다.** 이동량은 GetDeviceData 가 나르고,
        //   여기서는 「지금 눌려 있나」만 답한다. 두 경로가 같은 큐를 두고 다투면
        //   먼저 부른 쪽이 비워 버려 나중 쪽이 눌림을 못 본다.
        if (g_inject && isMouse && cb == sizeof(DIMOUSESTATE) && p) {
            DIMOUSESTATE* m = (DIMOUSESTATE*)p;
            memset(m, 0, sizeof(DIMOUSESTATE));
            EnterCriticalSection(&g_qcs);
            for (int i = 0; i < 4; i++) m->rgbButtons[i] = btn[i];
            LeaveCriticalSection(&g_qcs);
            return DI_OK;
        }
        if (g_inject && isKb && cb == 256 && p) {
            EnterCriticalSection(&g_qcs);
            memcpy(p, keys, 256);
            LeaveCriticalSection(&g_qcs);
            return DI_OK;
        }
        HRESULT hr = r->GetDeviceState(cb, p);
        long n = ++cState;
        if (n <= 3 || (n % 1000) == 0) {
            if (cb == sizeof(DIMOUSESTATE) && SUCCEEDED(hr)) {
                DIMOUSESTATE* m = (DIMOUSESTATE*)p;
                Log("[장치 %s] GetDeviceState #%ld cb=%lu(DIMOUSESTATE) dx=%ld dy=%ld dz=%ld btn=%02X %02X %02X %02X hr=0x%08lX",
                    devName, n, cb, m->lX, m->lY, m->lZ,
                    m->rgbButtons[0], m->rgbButtons[1], m->rgbButtons[2], m->rgbButtons[3], hr);
            } else {
                Log("[장치 %s] GetDeviceState #%ld cb=%lu hr=0x%08lX", devName, n, cb, hr);
            }
        }
        return hr;
    }
    STDMETHOD(GetDeviceData)(DWORD cb, LPDIDEVICEOBJECTDATA p, LPDWORD pn, DWORD f)
    {
        if (g_inject && cb == sizeof(DIDEVICEOBJECTDATA) && pn) {
            // 진짜 하드웨어 이벤트는 버린다 — 사용자가 만지는 마우스가 게임으로 새면 안 된다.
            DWORD flush = INFINITE;
            r->GetDeviceData(cb, NULL, &flush, 0);

            DWORD cap = *pn;
            DWORD got = 0;
            EnterCriticalSection(&g_qcs);
            if (p == NULL) {
                got = (DWORD)q.Count();                    // 개수만 묻는 경우
                if (cap != INFINITE && got > cap) got = cap;
                if (!(f & DIGDD_PEEK)) { DIDEVICEOBJECTDATA e; for (DWORD i = 0; i < got; i++) q.Pop(&e); }
            } else if (f & DIGDD_PEEK) {
                for (DWORD i = 0; i < cap && q.Peek((int)i, &p[i]); i++) got++;
            } else {
                while (got < cap && q.Pop(&p[got])) got++;
            }
            LeaveCriticalSection(&g_qcs);
            *pn = got;
            long n2 = ++cData;
            if (got > 0 && ++cInj <= 20) Log("[장치 %s] ★주입 %lu건 넘김 (게임이 가져감, 누적 %ld · 호출 %ld)", devName, got, cInj, n2);
            return DI_OK;
        }
        HRESULT hr = r->GetDeviceData(cb, p, pn, f);
        long n = ++cData;
        if (n <= 3 || (n % 1000) == 0)
            Log("[장치 %s] GetDeviceData #%ld cb=%lu 개수=%lu flags=%lu hr=0x%08lX",
                devName, n, cb, (pn ? *pn : 0), f, hr);
        return hr;
    }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT p)
    {
        if (p) Log("[장치 %s] SetDataFormat flags=%lu 자료크기=%lu 항목수=%lu",
            devName, p->dwFlags, p->dwDataSize, p->dwNumObjs);
        return r->SetDataFormat(p);
    }
    STDMETHOD(SetEventNotification)(HANDLE h) { Log("[장치 %s] SetEventNotification(%p)", devName, h); return r->SetEventNotification(h); }
    STDMETHOD(SetCooperativeLevel)(HWND h, DWORD f)
    {
        // ★ 녹스는 마우스를 EXCLUSIVE | FOREGROUND 로 잡으려 한다.
        //   그러면 창이 포그라운드가 아닐 때 Acquire 가 0x80070005 로 실패하고 게임이 종료된다.
        //   (원래 cnc-ddraw 의 dinput 후크가 이걸 바꿔 주고 있었다. 그 후크를 껐으므로 내가 한다.)
        //   덤으로 EXCLUSIVE 를 떼면 실제 마우스를 안 뺏어서 사용자가 PC 를 계속 쓸 수 있다.
        DWORD want = f;
        if (isMouse) want = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
        Log("[장치 %s] ★ SetCooperativeLevel(hwnd=%p) 요청=%s → 넘긴것=%s",
            devName, h, CoopStr(f), CoopStr(want));
        return r->SetCooperativeLevel(h, want);
    }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA p, DWORD o, DWORD h) { return r->GetObjectInfo(p, o, h); }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA p) { return r->GetDeviceInfo(p); }
    STDMETHOD(RunControlPanel)(HWND h, DWORD f) { return r->RunControlPanel(h, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v, REFGUID g) { return r->Initialize(h, v, g); }
};

// ─────────────────────────────────────────────── DirectInput 껍데기

// ─────────────────────────────────────────────── 조종 통로 (이름 붙인 파이프)

static DevProxy* g_mouseDev = NULL;
static DevProxy* g_kbDev = NULL;

static void HandleLine(char* s, HANDLE h)
{
    char reply[256]; reply[0] = 0;
    char cmd[32] = { 0 };
    int a = 0, b = 0;
    if (sscanf(s, "%31s", cmd) != 1) return;

    if (!strcmp(cmd, "inject")) {
        sscanf(s, "%*s %i", &a);
        InterlockedExchange(&g_inject, a ? 1 : 0);
        Log("[파이프] inject=%d", a ? 1 : 0);
        sprintf(reply, "inject=%d\n", a ? 1 : 0);
    }
    else if (!strcmp(cmd, "m")) {
        if (sscanf(s, "%*s %i %i", &a, &b) == 2 && g_mouseDev) {
            EnterCriticalSection(&g_qcs);
            if (a) g_mouseDev->q.Push(OFS_X, (DWORD)a);
            if (b) g_mouseDev->q.Push(OFS_Y, (DWORD)b);
            LeaveCriticalSection(&g_qcs);
            sprintf(reply, "ok\n");
        } else sprintf(reply, "err 마우스 장치 없음\n");
    }
    else if (!strcmp(cmd, "mb")) {
        if (sscanf(s, "%*s %i %i", &a, &b) == 2 && g_mouseDev && a >= 0 && a < 4) {
            EnterCriticalSection(&g_qcs);
            g_mouseDev->q.Push(OFS_B0 + a, b ? 0x80 : 0x00);
            g_mouseDev->btn[a] = (BYTE)(b ? 0x80 : 0x00);   // ★ 지금 눌려 있나
            LeaveCriticalSection(&g_qcs);
            sprintf(reply, "ok\n");
        } else sprintf(reply, "err\n");
    }
    else if (!strcmp(cmd, "k")) {
        if (sscanf(s, "%*s %i %i", &a, &b) == 2 && g_kbDev) {
            EnterCriticalSection(&g_qcs);
            g_kbDev->q.Push((DWORD)a, b ? 0x80 : 0x00);
            g_kbDev->keys[a & 0xff] = (BYTE)(b ? 0x80 : 0x00);   // ★ 지금 눌려 있나
            LeaveCriticalSection(&g_qcs);
            sprintf(reply, "ok\n");
        } else sprintf(reply, "err 키보드 장치 없음\n");
    }
    else if (!strcmp(cmd, "stat")) {
        int qm = -1, qk = -1;
        EnterCriticalSection(&g_qcs);
        if (g_mouseDev) qm = g_mouseDev->q.Count();
        if (g_kbDev)    qk = g_kbDev->q.Count();
        LeaveCriticalSection(&g_qcs);
        sprintf(reply, "큐 mouse=%d kb=%d · GetDeviceData 호출 mouse=%ld kb=%ld · 주입넘김 mouse=%ld\n",
            qm, qk,
            g_mouseDev ? g_mouseDev->cData : -1, g_kbDev ? g_kbDev->cData : -1,
            g_mouseDev ? g_mouseDev->cInj : -1);
    }
    else if (!strcmp(cmd, "ping")) {
        sprintf(reply, "pong pid=%lu inject=%ld mouse=%d kb=%d\n",
            GetCurrentProcessId(), g_inject, g_mouseDev ? 1 : 0, g_kbDev ? 1 : 0);
    }
    else sprintf(reply, "?? %s\n", cmd);

    if (reply[0]) { DWORD w = 0; WriteFile(h, reply, (DWORD)strlen(reply), &w, NULL); }
}

static DWORD WINAPI PipeThread(LPVOID)
{
    char name[64];
    // 파이프 이름 = 역슬래시 둘, 점, 역슬래시, pipe, 역슬래시, noxdi_<pid>
    // 역슬래시를 여기 그대로 적으면 도구가 경로로 오해하므로 \x5c 로 적는다
    sprintf(name, "\x5c\x5c.\x5cpipe\x5cnoxdi_%lu", GetCurrentProcessId());
    Log("[파이프] 기다리는 중 %s", name);
    for (;;) {
        HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 4, 4096, 4096, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(h); continue; }
        Log("[파이프] 붙음");
        char buf[4096]; int len = 0;
        for (;;) {
            char c[512]; DWORD rd = 0;
            if (!ReadFile(h, c, sizeof(c), &rd, NULL) || rd == 0) break;
            for (DWORD i = 0; i < rd; i++) {
                if (c[i] == '\n' || c[i] == '\r') {
                    if (len > 0) { buf[len] = 0; HandleLine(buf, h); len = 0; }
                } else if (len < (int)sizeof(buf) - 1) buf[len++] = c[i];
            }
        }
        Log("[파이프] 끊김");
        DisconnectNamedPipe(h); CloseHandle(h);
    }
}

class DIProxy : public IDirectInputA
{
public:
    IDirectInputA* r;
    LONG ref;
    DIProxy(IDirectInputA* real) : r(real), ref(1) { Log("[DI] 껍데기 생성 (진짜 %p)", real); }

    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* pp)
    {
        Log("[DI] QueryInterface(%s)", GuidName(riid));
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputA)) {
            AddRef(); *pp = (IDirectInputA*)this; return S_OK;
        }
        HRESULT hr = r->QueryInterface(riid, pp);
        Log("[DI] ★ 안 감싼 인터페이스를 그대로 넘김 hr=0x%08lX", hr);
        return hr;
    }
    STDMETHOD_(ULONG, AddRef)() { InterlockedIncrement(&ref); return r->AddRef(); }
    STDMETHOD_(ULONG, Release)()
    {
        ULONG n = r->Release();
        if (InterlockedDecrement(&ref) == 0) { Log("[DI] Release"); delete this; }
        return n;
    }
    STDMETHOD(CreateDevice)(REFGUID g, LPDIRECTINPUTDEVICEA* pp, LPUNKNOWN unk)
    {
        LPDIRECTINPUTDEVICEA real = NULL;
        HRESULT hr = r->CreateDevice(g, &real, unk);
        Log("[DI] CreateDevice(%s) hr=0x%08lX", GuidName(g), hr);
        if (SUCCEEDED(hr) && real) {
            DevProxy* dv = new DevProxy(real, g);
            if (dv->isMouse) g_mouseDev = dv;
            if (dv->isKb)    g_kbDev = dv;
            *pp = dv;
            return hr;
        }
        *pp = real;
        return hr;
    }
    STDMETHOD(EnumDevices)(DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID pv, DWORD f) { return r->EnumDevices(t, cb, pv, f); }
    STDMETHOD(GetDeviceStatus)(REFGUID g) { return r->GetDeviceStatus(g); }
    STDMETHOD(RunControlPanel)(HWND h, DWORD f) { return r->RunControlPanel(h, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v) { Log("[DI] Initialize(ver=0x%04lX)", v); return r->Initialize(h, v); }
};

// ─────────────────────────────────────────────── 내보내는 함수들

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE hinst, DWORD ver, LPDIRECTINPUTA* pp, LPUNKNOWN unk)
{
    Log("DirectInputCreateA(ver=0x%04lX) 들어옴", ver);
    if (!LoadReal()) return E_FAIL;
    PFN_DICreateA fn = (PFN_DICreateA)GetProcAddress(g_real, "DirectInputCreateA");
    if (!fn) { Log("  진짜 DirectInputCreateA 를 못 찾음"); return E_FAIL; }
    LPDIRECTINPUTA real = NULL;
    HRESULT hr = fn(hinst, ver, &real, unk);
    Log("  진짜 반환 hr=0x%08lX obj=%p", hr, real);
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
    Log("DirectInputCreateEx(ver=0x%04lX, %s) — 안 감싸고 그대로 넘김", ver, GuidName(riid));
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
    Log("DllGetClassObject(%s)", GuidName(rclsid));
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
        InitializeCriticalSection(&g_cs); g_csOk = true;

        char dir[MAX_PATH];
        if (!GetEnvironmentVariableA("NOX_HOOK_LOG_DIR", dir, MAX_PATH))
            strcpy(dir, "C:\\Users\\USER\\Desktop\\MyGit\\Nox_Server\\_evidence");
        char exe[MAX_PATH]; exe[0] = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
        sprintf(g_log, "%s\\dihook_%s_%lu.log", dir, base, GetCurrentProcessId());

        InitializeCriticalSection(&g_qcs);
        Log("=== dinput 프록시 붙음 ===  exe=%s pid=%lu", exe, GetCurrentProcessId());
        CloseHandle(CreateThread(NULL, 0, PipeThread, NULL, 0, NULL));
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Log("=== dinput 프록시 떨어짐 ===");
    }
    return TRUE;
}