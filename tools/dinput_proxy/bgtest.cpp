// dinput.dll 프록시 시험 — 뒤에 있는 창이 누름·이동을 버리고 뗌은 남기나
//
//   bgtest <dinput.dll 경로> filter   프록시 — 마우스 축 0 · 왼버튼 누름 0 뗌 1+ · Ctrl 누름 0 뗌 1+
//   bgtest <dinput.dll 경로> raw      대조군(진짜 SysWOW64\dinput.dll) — 축 1+ · 누름 1+ · 뗌 1+ (마우스·Ctrl 둘 다)
//
// 대조군이 있어야 프록시의 「0」이 「버렸다」인지 「입력이 안 왔다」인지 갈린다.
// 장치에 넘길 창은 안 띄운다 — 앞 창은 늘 다른 프로그램이라 「뒤에 있는」 상태가 된다.
// ★ 뗌만 보내면 이벤트가 안 생긴다(이미 떼어진 상태라 변화가 없다) — 누름·뗌을 한 쌍으로 보낸다.
// ⚠ 진짜 입력을 넣는다 —
//   마우스 좌우로 조금 움직였다 제자리 · 왼클릭 한 번 · 왼Ctrl 한 번
//   클릭은 커서 밑에 잠깐 띄운 「앞으로 오지 않는 창」(WS_EX_NOACTIVATE)이 받는다 — 쓰던 창은 안 눌린다
//   Ctrl 은 지금 앞에 있는 창으로 간다(Ctrl 만 누르고 떼면 대개 아무 일도 없다)

#define DIRECTINPUT_VERSION 0x0700
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <initguid.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT(WINAPI* PFN_DICreateA)(HINSTANCE, DWORD, LPDIRECTINPUTA*, LPUNKNOWN);

#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000   // dinput.h 가 이 판(0x0700)에선 안 내준다
#endif

// 녹스가 쓰는 것과 같은 짜임 — DIMOUSESTATE 16바이트 7항목, 상대 축(flags=2)
static DIOBJECTDATAFORMAT g_objs[] = {
    { &GUID_XAxis, 0,  DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { &GUID_YAxis, 4,  DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { &GUID_ZAxis, 8,  DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0 },
    { NULL,        12, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
    { NULL,        13, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
    { NULL,        14, DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0 },
    { NULL,        15, DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0 },
};
static DIDATAFORMAT g_dfMouse = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
                                  sizeof(DIMOUSESTATE), 7, g_objs };

// 키보드 짜임 — 256 항목, dwOfs = DIK 스캔코드
static DIOBJECTDATAFORMAT g_kobjs[256];
static DIDATAFORMAT g_dfKb = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), 0, 256, 256, g_kobjs };

static void Mouse(int dx, DWORD flags)
{
    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}
static void Key(WORD vk, bool up)
{
    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}
static void Pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(5);
    }
}
static LRESULT CALLBACK ClickProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;   // 눌려도 앞으로 오지 않는다
    return DefWindowProcA(h, m, w, l);
}

struct Counts { DWORD n, axis, down, up; };

static Counts Read(LPDIRECTINPUTDEVICEA dev, bool isMouse, DWORD wantOfs, HRESULT* hr)
{
    Counts c = { 0, 0, 0, 0 };
    DIDEVICEOBJECTDATA buf[256]; DWORD n = 256;
    *hr = dev->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), buf, &n, 0);
    c.n = n;
    for (DWORD i = 0; i < n; i++) {
        if (isMouse && buf[i].dwOfs < 12) { c.axis++; continue; }
        if (buf[i].dwOfs != wantOfs) continue;
        if (buf[i].dwData & 0x80) c.down++; else c.up++;
    }
    return c;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("쓰는 법: bgtest <dinput.dll 경로> filter|raw\n"); return 2; }
    bool expectFilter = strcmp(argv[2], "filter") == 0;

    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = hi; wc.lpszClassName = "bgtest";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("bgtest", "bgtest", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, hi, NULL);

    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("실패 — dll 을 못 엶: %s\n", argv[1]); return 1; }
    PFN_DICreateA fn = (PFN_DICreateA)GetProcAddress(m, "DirectInputCreateA");
    LPDIRECTINPUTA di = NULL;
    HRESULT hr = fn ? fn(hi, 0x0300, &di, NULL) : E_FAIL;           // 녹스와 같은 DirectInput 3
    if (FAILED(hr)) { printf("실패 — DirectInputCreateA hr=0x%08lX\n", hr); return 1; }

    for (int i = 0; i < 256; i++) {
        g_kobjs[i].pguid = &GUID_Key; g_kobjs[i].dwOfs = i;
        g_kobjs[i].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL; g_kobjs[i].dwFlags = 0;
    }
    DIPROPDWORD pd; memset(&pd, 0, sizeof(pd));
    pd.diph.dwSize = sizeof(pd); pd.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    pd.diph.dwHow = DIPH_DEVICE; pd.dwData = 256;

    LPDIRECTINPUTDEVICEA ms = NULL, kb = NULL;
    HRESULT h1 = di->CreateDevice(GUID_SysMouse, &ms, NULL);
    HRESULT h2 = di->CreateDevice(GUID_SysKeyboard, &kb, NULL);
    if (FAILED(h1) || FAILED(h2)) { printf("실패 — CreateDevice 마우스 0x%08lX 키보드 0x%08lX\n", h1, h2); return 1; }
    ms->SetDataFormat(&g_dfMouse);  ms->SetProperty(DIPROP_BUFFERSIZE, &pd.diph);
    kb->SetDataFormat(&g_dfKb);     kb->SetProperty(DIPROP_BUFFERSIZE, &pd.diph);
    ms->SetCooperativeLevel(hw, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    kb->SetCooperativeLevel(hw, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);   // 녹스가 키보드에 요청하는 값
    h1 = ms->Acquire(); h2 = kb->Acquire();
    if (FAILED(h1) || FAILED(h2)) { printf("실패 — Acquire 마우스 0x%08lX 키보드 0x%08lX\n", h1, h2); return 1; }

    // 클릭 받을 창 — 커서 밑에 띄우되 앞으로 오지 않게
    POINT before; GetCursorPos(&before);
    WNDCLASSA wc2; memset(&wc2, 0, sizeof(wc2));
    wc2.lpfnWndProc = ClickProc; wc2.hInstance = hi; wc2.lpszClassName = "bgtestclick";
    wc2.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    RegisterClassA(&wc2);
    HWND pad = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "bgtestclick", "",
                               WS_POPUP, before.x - 20, before.y - 20, 40, 40, NULL, NULL, hi, NULL);
    ShowWindow(pad, SW_SHOWNOACTIVATE);
    Pump(100);

    DWORD flush = INFINITE;
    ms->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), NULL, &flush, 0);   // 앞서 쌓인 것 비우기
    flush = INFINITE;
    kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), NULL, &flush, 0);

    DWORD fgPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    bool inFg = fgPid == GetCurrentProcessId();

    SetCursorPos(before.x, before.y);                                   // 클릭 창 한가운데
    Mouse(0, MOUSEEVENTF_LEFTDOWN); Pump(30); Mouse(0, MOUSEEVENTF_LEFTUP); Pump(30);
    for (int i = 0; i < 10; i++) { Mouse(+4, MOUSEEVENTF_MOVE); Sleep(5); }
    for (int i = 0; i < 10; i++) { Mouse(-4, MOUSEEVENTF_MOVE); Sleep(5); }
    Key(VK_LCONTROL, false); Pump(30); Key(VK_LCONTROL, true);
    Pump(150);
    SetCursorPos(before.x, before.y);                                   // 제자리 (가속으로 어긋난 몫)

    // 클릭 뒤에도 앞 창이 그대로여야 시험이 성립한다
    DWORD fgPid2 = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid2);
    inFg = inFg || fgPid2 == GetCurrentProcessId();

    HRESULT hm, hk;
    Counts cm = Read(ms, true, 12, &hm);            // 왼버튼 = 자리 12
    Counts ck = Read(kb, false, DIK_LCONTROL, &hk);
    DestroyWindow(pad);
    ms->Unacquire(); ms->Release(); kb->Unacquire(); kb->Release(); di->Release();

    bool ok = !inFg && SUCCEEDED(hm) && SUCCEEDED(hk) && cm.up >= 1 && ck.up >= 1;
    if (expectFilter) ok = ok && cm.axis == 0 && cm.down == 0 && ck.down == 0;
    else              ok = ok && cm.axis >= 1 && cm.down >= 1 && ck.down >= 1;
    printf("%s  [%s] 앞창=%s · 마우스 %lu건 = 축 %lu · 누름 %lu · 뗌 %lu · Ctrl 누름 %lu · 뗌 %lu (키보드 %lu건)\n",
        ok ? "통과" : "실패", argv[2], inFg ? "나(시험 무효)" : "남",
        cm.n, cm.axis, cm.down, cm.up, ck.down, ck.up, ck.n);
    return ok ? 0 : 1;
}
