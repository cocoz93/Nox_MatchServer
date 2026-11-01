// launcher_main.cpp — 녹스 온라인 런처(NoxLauncher.exe · 별도 프로세스 · 11-01 사용자 결정 1). 판단은 LauncherCore.h.
//
// 화면 — WebView2(윈도에 들어 있는 엣지 화면)에 런처 옆 `launcher_ui\index.html` 을 `https://launcher.nox/` 로 띄운다.
//   화면 ↔ 본체는 폼 꼴 글 한 줄(`cmd=login&name=…` · `ev=login&ok=1` — URLSearchParams 와 같은 꼴 · JSON 처리기 없음)
//   화면이 보내는 것: init · login(name · password · remember) · start · logout · signup · quit
//   본체가 보내는 것: init(lastName · auto · hasKey) · login(ok · reason) · start(ok · reason) · logout · gameExit · busy
//   로그인 서버 일은 일꾼 스레드에서(창이 안 멈추게) — 끝나면 창 메시지로 화면 스레드에 돌아와 화면에 보낸다
// 게임 — 띄우면 런처 창을 숨기고, 게임이 끝나면 다시 보인다. 자동 로그인이 아니면 그때 로그아웃(열쇠 지움 — 결정 2)
// 그림 — 시작할 때 게임 폴더에서 정품 그림 · 글꼴을 뽑아(NoxArt.h · 처음 한 번) `https://art.nox/` 로 화면에 준다. 창은 640x480 의 정수배
//
// 화면 모드 인자 — [--exe <게임 exe>](mo_local 이 클라2 런처 창에 Game2.exe) · [--dev](개발 도구 · 오른쪽 단추 메뉴)
// 명령 줄 모드(창 없음 — tools\mo_local.ps1 · 시험)
//   NoxLauncher.exe --cli --name <이름> --password <암호> [--remember] [--exe <게임 exe>] [--no-wait]
//     로그인 → 게임 띄움 → (기본) 게임이 끝날 때까지 기다리고 자동 로그인이 아니면 로그아웃. 종료 코드 0 됨 · 2 로그인 실패 · 3 게임 못 띄움
//   NoxLauncher.exe --logout --name <이름>
//   결과 줄은 --log <파일>(UTF-8)에도 남긴다(창 프로그램이라 콘솔 출력은 부른 쪽 콘솔이 있을 때만)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>

#include <map>
#include <string>
#include <thread>

#include "WebView2.h"
#include "LauncherCore.h"
#include "NoxArt.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "WebView2LoaderStatic.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const UINT WM_APP_POST = WM_APP + 1;        // lParam = new std::string(화면에 보낼 글)
const UINT WM_APP_GAMEEXIT = WM_APP + 2;    // lParam = new std::string(화면에 보낼 글)
const wchar_t kClass[] = L"NoxOnlineLauncher";
const wchar_t kHost[] = L"launcher.nox";
const wchar_t kArtHost[] = L"art.nox";

std::wstring g_artDir;       // 정품 그림을 뽑아 둔 폴더(못 뽑았으면 빈 글 — 화면은 기본 모양)
HWND g_hwnd = NULL;
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2> g_web;
launcher::Config g_cfg;
std::string g_sessionName;   // 지금 로그인한 이름(이번 실행)
bool g_sessionRemember = false;
volatile LONG g_busy = 0;    // 로그인 서버 일 · 게임 중(겹쳐 누름 막음)

// ── 폼 꼴 글
std::map<std::string, std::string> ParseQuery(const std::string& q)
{
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        const std::string pair = q.substr(i, amp - i);
        const size_t eq = pair.find('=');
        auto hx = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            c = static_cast<char>(c | 0x20);
            return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        auto dec = [&hx](const std::string& s) {
            std::string o;
            for (size_t k = 0; k < s.size(); ++k) {
                if (s[k] == '+') {
                    o.push_back(' ');
                } else if (s[k] == '%' && k + 2 < s.size() && hx(s[k + 1]) >= 0 && hx(s[k + 2]) >= 0) {
                    o.push_back(static_cast<char>(hx(s[k + 1]) * 16 + hx(s[k + 2])));
                    k += 2;
                } else {
                    o.push_back(s[k]);   // 틀린 % 는 글자 그대로
                }
            }
            return o;
        };
        if (!pair.empty()) out.emplace(dec(pair.substr(0, eq)), eq == std::string::npos ? std::string() : dec(pair.substr(eq + 1)));
        i = amp + 1;
    }
    return out;
}

std::string Q(std::initializer_list<std::pair<const char*, std::string>> kv)
{
    std::string s;
    for (const auto& p : kv) {
        if (!s.empty()) s.push_back('&');
        s += p.first;
        s.push_back('=');
        s += authclient::detail::UrlEncode(p.second);
    }
    return s;
}

void PostToUi(UINT msg, const std::string& text) { PostMessageW(g_hwnd, msg, 0, reinterpret_cast<LPARAM>(new std::string(text))); }

void SendToPage(const std::string& text)
{
    if (g_web) g_web->PostWebMessageAsString(launcher::Wide(text).c_str());
}

// ── 게임 띄우고 끝날 때까지(일꾼 스레드)
void RunGame(const std::string& name, bool remember)
{
    PROCESS_INFORMATION pi;
    std::wstring err;
    if (!launcher::LaunchGame(g_cfg, name, pi, err)) {
        InterlockedExchange(&g_busy, 0);
        PostToUi(WM_APP_POST, Q({ { "ev", "start" }, { "ok", "0" }, { "reason", "launch_failed" }, { "detail", launcher::Utf8(err) } }));
        return;
    }
    PostToUi(WM_APP_POST, Q({ { "ev", "start" }, { "ok", "1" } }));
    ShowWindow(g_hwnd, SW_HIDE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    bool loggedOut = false;
    if (!remember) {   // 이번 실행만 — 게임이 끝나면 열쇠 폐기(결정 2)
        launcher::Logout(g_cfg, name);
        loggedOut = true;
    }
    PostToUi(WM_APP_GAMEEXIT, Q({ { "ev", "gameExit" }, { "loggedOut", loggedOut ? "1" : "0" } }));
}

void OnPageMessage(const std::string& text)
{
    auto m = ParseQuery(text);
    const std::string cmd = m["cmd"];
    if (cmd == "init") {
        const launcher::State s = launcher::LoadState();
        std::string key;
        const bool hasKey = !s.lastName.empty() && launchkey::Load(s.lastName, key);
        if (!key.empty()) SecureZeroMemory(&key[0], key.size());
        SendToPage(Q({ { "ev", "init" }, { "lastName", s.lastName }, { "auto", s.autoLogin && hasKey ? "1" : "0" } }));
        return;
    }
    if (cmd == "signup") {
        const std::wstring url = authclient::WithPath(g_cfg.authBase, L"/signup");
        ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (cmd == "quit") {
        DestroyWindow(g_hwnd);
        return;
    }
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        SendToPage(Q({ { "ev", "busy" } }));
        return;
    }
    if (cmd == "login") {
        std::string name = m["name"], pw = m["password"];
        const bool remember = m["remember"] == "1", saveId = m["saveId"] != "0";
        if (!launchkey::NameOk(name) || pw.empty()) {
            InterlockedExchange(&g_busy, 0);
            SendToPage(Q({ { "ev", "login" }, { "ok", "0" }, { "reason", "bad_input" } }));
            return;
        }
        std::thread([name, pw, remember, saveId]() mutable {   // 이번 실행 이름(g_sessionName)은 화면 스레드가 이 답을 받을 때 고친다(WndProc)
            const launcher::LoginResult r = launcher::Login(g_cfg, name, pw, remember, saveId);
            InterlockedExchange(&g_busy, 0);
            PostToUi(WM_APP_POST, Q({ { "ev", "login" }, { "ok", r.outcome == launcher::Outcome::Ok ? "1" : "0" }, { "reason", r.reason }, { "name", name },
                                      { "remember", remember ? "1" : "0" } }));
        }).detach();
        SecureZeroMemory(&m["password"][0], m["password"].size());
        return;
    }
    if (cmd == "start") {
        // 이번 실행에 로그인했으면 그 열쇠로 바로 · 아니면(자동 로그인으로 시작) 열쇠를 한 번 확인
        const bool fresh = !g_sessionName.empty();
        const launcher::State s = launcher::LoadState();
        const std::string name = fresh ? g_sessionName : s.lastName;
        const bool remember = fresh ? g_sessionRemember : s.autoLogin;
        std::thread([name, remember, fresh]() {
            if (!fresh) {
                const launcher::LoginResult r = launcher::CheckKey(g_cfg, name);
                if (r.outcome != launcher::Outcome::Ok) {
                    InterlockedExchange(&g_busy, 0);
                    PostToUi(WM_APP_POST, Q({ { "ev", "start" }, { "ok", "0" }, { "reason", r.reason } }));
                    return;
                }
            }
            RunGame(name, remember);
        }).detach();
        return;
    }
    if (cmd == "logout") {
        const std::string name = !g_sessionName.empty() ? g_sessionName : launcher::LoadState().lastName;
        std::thread([name]() {
            launcher::Logout(g_cfg, name);
            InterlockedExchange(&g_busy, 0);
            PostToUi(WM_APP_POST, Q({ { "ev", "logout" } }));
        }).detach();
        return;
    }
    InterlockedExchange(&g_busy, 0);
}

void ShowNoRuntime()
{
    MessageBoxW(g_hwnd,
                L"이 PC 에 Microsoft Edge WebView2 런타임이 없습니다.\n"
                L"https://developer.microsoft.com/microsoft-edge/webview2/ 에서 「Evergreen Bootstrapper」를 받아 설치한 뒤 다시 실행해 주세요.",
                L"녹스 온라인", MB_ICONERROR);
    DestroyWindow(g_hwnd);
}

void CreateWebView()
{
    const std::wstring keys = launchkey::KeyDir();
    const std::wstring dataDir = keys.empty() ? std::wstring() : keys.substr(0, keys.rfind(L'\\')) + L"\\webview";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.empty() ? nullptr : dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(res) || !env) {
                ShowNoRuntime();
                return S_OK;
            }
            env->CreateCoreWebView2Controller(
                g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                            if (FAILED(r2) || !c) {
                                ShowNoRuntime();
                                return S_OK;
                            }
                            g_ctrl = c;
                            g_ctrl->get_CoreWebView2(&g_web);
                            ComPtr<ICoreWebView2Settings> st;
                            if (SUCCEEDED(g_web->get_Settings(&st))) {
                                const bool dev = wcsstr(GetCommandLineW(), L"--dev") != nullptr;
                                st->put_AreDevToolsEnabled(dev ? TRUE : FALSE);
                                st->put_AreDefaultContextMenusEnabled(dev ? TRUE : FALSE);
                                st->put_IsStatusBarEnabled(FALSE);
                                st->put_IsZoomControlEnabled(FALSE);
                            }
                            RECT rc;
                            GetClientRect(g_hwnd, &rc);
                            g_ctrl->put_Bounds(rc);
                            ComPtr<ICoreWebView2_3> w3;
                            if (SUCCEEDED(g_web.As(&w3))) {
                                const std::wstring ui = g_cfg.exeDir + L"\\launcher_ui";
                                w3->SetVirtualHostNameToFolderMapping(kHost, ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                // 화면이 그림 픽셀을 읽어야 하므로(글자 오리기 · 그늘) fetch 까지 허용
                                if (!g_artDir.empty()) w3->SetVirtualHostNameToFolderMapping(kArtHost, g_artDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }
                            EventRegistrationToken tok;
                            g_web->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                    LPWSTR s = nullptr;
                                    if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) {
                                        OnPageMessage(launcher::Utf8(s));
                                        CoTaskMemFree(s);
                                    }
                                    return S_OK;
                                }).Get(),
                                &tok);
                            // 화면 안 링크가 새 창을 열면 기본 브라우저로(런처 안에서 남의 쪽을 띄우지 않음)
                            g_web->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                    LPWSTR uri = nullptr;
                                    if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                                        ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
                                        CoTaskMemFree(uri);
                                    }
                                    a->put_Handled(TRUE);
                                    return S_OK;
                                }).Get(),
                                &tok);
                            g_web->Navigate(L"https://launcher.nox/index.html");
                            return S_OK;
                        }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) ShowNoRuntime();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
        case WM_SIZE:
            if (g_ctrl) {
                RECT rc;
                GetClientRect(h, &rc);
                g_ctrl->put_Bounds(rc);
            }
            return 0;
        case WM_APP_POST:
        case WM_APP_GAMEEXIT: {
            std::string* s = reinterpret_cast<std::string*>(l);
            auto ev = ParseQuery(*s);   // 이번 실행 이름은 화면 스레드에서만 고친다
            if (ev["ev"] == "login" && ev["ok"] == "1") {
                g_sessionName = ev["name"];
                g_sessionRemember = ev["remember"] == "1";
            } else if (ev["ev"] == "logout" || (ev["ev"] == "gameExit" && ev["loggedOut"] == "1")) {
                g_sessionName.clear();
            }
            if (msg == WM_APP_GAMEEXIT) {
                InterlockedExchange(&g_busy, 0);
                ShowWindow(h, SW_SHOW);
                SetForegroundWindow(h);
            }
            SendToPage(*s);
            delete s;
            return 0;
        }
        case WM_DESTROY:
            g_ctrl.Reset();
            g_web.Reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ── 명령 줄 모드
std::wstring ArgAfter(int argc, wchar_t** argv, const wchar_t* key)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], key) == 0) return argv[i + 1];
    return std::wstring();
}
bool HasArg(int argc, wchar_t** argv, const wchar_t* key)
{
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], key) == 0) return true;
    return false;
}

int RunCli(int argc, wchar_t** argv)
{
    const bool console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    const std::wstring logPath = ArgAfter(argc, argv, L"--log");
    std::string logText;
    auto say = [&](const std::string& line) {
        logText += line + "\r\n";
        if (console) {
            DWORD w = 0;
            const std::string l = line + "\r\n";
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), l.data(), static_cast<DWORD>(l.size()), &w, NULL);
        }
        if (!logPath.empty()) launcher::WriteFileUtf8(logPath, logText);
    };
    const std::string name = launcher::Utf8(ArgAfter(argc, argv, L"--name"));
    if (HasArg(argc, argv, L"--logout")) {
        const launcher::LoginResult r = launcher::Logout(g_cfg, name);
        say("logout " + name + " " + (r.outcome == launcher::Outcome::Ok ? "ok" : "server:" + r.reason));
        return 0;
    }
    std::string pw = launcher::Utf8(ArgAfter(argc, argv, L"--password"));
    const bool remember = HasArg(argc, argv, L"--remember");
    const std::wstring exe = ArgAfter(argc, argv, L"--exe");
    if (!exe.empty()) g_cfg.gameExe = exe;
    const launcher::LoginResult r = launcher::Login(g_cfg, name, pw, remember);
    if (r.outcome != launcher::Outcome::Ok) {
        say("login " + name + " fail " + r.reason);
        return 2;
    }
    say("login " + name + " ok (remember " + (remember ? "1" : "0") + ")");
    PROCESS_INFORMATION pi;
    std::wstring err;
    if (!launcher::LaunchGame(g_cfg, name, pi, err)) {
        say("launch fail " + launcher::Utf8(err));
        if (!remember) launcher::Logout(g_cfg, name);
        return 3;
    }
    say("launch ok pid " + std::to_string(pi.dwProcessId));
    if (HasArg(argc, argv, L"--no-wait")) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!remember) {
        launcher::Logout(g_cfg, name);
        say("game exit — logout (session key)");
    } else {
        say("game exit — key kept (auto login)");
    }
    return 0;
}

// 게임 폴더에서 정품 그림을 뽑는다(처음 한 번 — 이후는 art.stamp 로 건너뜀). 못 뽑아도 런처는 뜬다
void PrepareArt()
{
    const std::wstring keys = launchkey::KeyDir();   // …\NoxOnline\keys
    const size_t cut = keys.rfind(L'\\');
    const std::wstring game = launcher::GamePath(g_cfg);
    const size_t gameCut = game.find_last_of(L"\\/");
    if (cut == std::wstring::npos || gameCut == std::wstring::npos) return;
    const std::wstring art = keys.substr(0, cut) + L"\\art";
    if (noxart::Extract(game.substr(0, gameCut), art)) g_artDir = art;
}

// 창 안쪽 = 640x480 의 정수배. 화면에 들어가는 가장 큰 배수 — 다만 100% 배율 기준 2배(게임 창과 같은 크기)까지만 · 고해상도 화면은 그 배율만큼 더
int StageScale(DWORD style, int dpi, RECT& work)
{
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRect(&frame, style, FALSE);
    const int availW = (work.right - work.left) - (frame.right - frame.left);
    const int availH = (work.bottom - work.top) - (frame.bottom - frame.top);
    const int fits = (std::min)(availW / 640, availH / 480);
    const int cap = (std::max)(2, MulDiv(2, dpi, 96));
    return (std::max)(1, (std::min)(fits, cap));
}

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.rfind(L'\\'));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_cfg = launcher::LoadConfig(ExeDir());
    if (HasArg(argc, argv, L"--cli") || HasArg(argc, argv, L"--logout")) {
        const int rc = RunCli(argc, argv);
        LocalFree(argv);
        return rc;
    }
    // ★ 런처 P11 — 화면 모드도 --exe 를 받는다(mo_local 이 클라마다 런처 창 하나 · 클라2 = Game2.exe). 창 제목에 그 이름을 붙임
    const std::wstring exeArg = ArgAfter(argc, argv, L"--exe");
    if (!exeArg.empty()) g_cfg.gameExe = exeArg;
    LocalFree(argv);
    std::wstring title = L"녹스 온라인";
    if (!exeArg.empty()) title += L" — " + exeArg.substr(exeArg.find_last_of(L"\\/") + 1);

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(12, 10, 8));
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
    HDC dc = GetDC(NULL);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT work;
    const int scale = StageScale(style, dpi, work);
    RECT r = { 0, 0, 640 * scale, 480 * scale };
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int x = work.left + ((work.right - work.left) - ww) / 2, y = work.top + ((work.bottom - work.top) - wh) / 2;
    g_hwnd = CreateWindowExW(0, kClass, title.c_str(), style, x, y, ww, wh, NULL, NULL, inst, NULL);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    PrepareArt();
    CreateWebView();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
