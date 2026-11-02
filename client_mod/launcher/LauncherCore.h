// LauncherCore.h — 녹스 온라인 런처의 판단(창 없이 시험되는 몫 · 머리 파일만). 화면(WebView2)과 명령 줄 모드가 같이 쓴다.
//   설계 Docs/LAUNCHER_PLAN.md · 조사 Docs/LAUNCHER_RESEARCH.md · 11-01 사용자 결정 1~4
//
// 하는 일
//   · 설정 — 런처 옆 NoxLauncher.ini(`auth_url` · `auth_pin` · `game_exe`) → 환경 변수 NOX_AUTH_URL · NOX_AUTH_PIN 이 있으면 그것이 이김
//     (다른 PC 꾸러미가 지금 Game.exe 에 넣는 값과 같은 이름 — 게임에도 그대로 물려줌)
//   · 기억 — %LOCALAPPDATA%\NoxOnline\launcher.ini(`last_name` · `auto_login`). 아이디 저장 기본 켬(결정 2) · 암호는 어디에도 안 둠
//   · 로그인 — 암호로 `/launcher/login` → 열쇠(NXK1) → `LaunchKey.h` 로 이 PC 에 암호화해 둠. 자동 로그인이 아니어도 둔다(DLL 이 그 파일로
//     토큰을 받음) — 대신 서버 수명이 12시간이고 게임이 끝나면 `/logout` + 파일 지움(결정 2 「끄면 이번 실행만」)
//   · 열쇠 확인 — 자동 로그인으로 시작할 때 `/token` 을 한 번 불러 본다. key_invalid 면 열쇠를 지우고 로그인 화면으로(게임 안에서 막히기 전에)
//   · 게임 띄우기 — Game.exe 를 그 폴더에서 · 환경 변수 NOX_LAUNCH=<이름>(열쇠는 안 넣음 — `LaunchKey.h`). DLL 이 이 표시로 화면을 넘긴다(⑯)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../common/LaunchKey.h"
#include "../wsock32_proxy/AuthClient.h"

namespace launcher {

constexpr DWORD kAuthTimeoutMs = 10000;   // 런처는 게임 주 스레드가 아니다 — DLL 의 3초보다 넉넉히(가입 해시 0.6초 + 연결)

inline std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), NULL, 0, NULL, NULL);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, NULL, NULL);
    return s;
}

inline std::wstring Wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// `키 = 값` 줄들(UTF-8 · `;` 줄은 주석 · 구역 없음). 없는 키는 빈 글
inline std::string IniValue(const std::string& text, const std::string& key)
{
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        if (b < line.size() && line[b] == ';') continue;
        const size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(b, eq - b);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        size_t v = eq + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        return line.substr(v);
    }
    return std::string();
}

inline bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got) out.append(buf, got);
    CloseHandle(h);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);   // BOM 은 떼고 읽음
    return true;
}

inline bool WriteFileUtf8(const std::wstring& path, const std::string& text)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, NULL) && wrote == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline std::wstring EnvW(const wchar_t* name)
{
    wchar_t v[1024];
    const DWORD n = GetEnvironmentVariableW(name, v, 1024);
    return n > 0 && n < 1024 ? std::wstring(v, n) : std::wstring();
}

struct Config {
    std::wstring exeDir;                                  // 런처가 있는 폴더(끝 \ 없음)
    std::wstring authBase = L"https://127.0.0.1:8443";   // 로그인 서버 기준 주소(옛 꼴 …/login 도 받음 — WithPath)
    bool authBaseFromFile = false;                        // 설정 파일이 준 주소인가(기본값이면 게임에 안 넘김 — DLL 은 로비 상대 IP:8443 으로 스스로 감)
    std::string pin;                                      // 서버 인증서 지문(16진 64자) · 비면 윈도 인증서 검사
    std::wstring gameExe = L"Game.exe";                   // exeDir 기준(절대 경로도 됨)
};

inline Config LoadConfig(const std::wstring& exeDir)
{
    Config c;
    c.exeDir = exeDir;
    std::string ini;
    if (ReadFileUtf8(exeDir + L"\\NoxLauncher.ini", ini)) {
        const std::string url = IniValue(ini, "auth_url"), pin = IniValue(ini, "auth_pin"), game = IniValue(ini, "game_exe");
        if (!url.empty()) {
            c.authBase = Wide(url);
            c.authBaseFromFile = true;
        }
        if (!pin.empty()) c.pin = pin;
        if (!game.empty()) c.gameExe = Wide(game);
    }
    const std::wstring envUrl = EnvW(L"NOX_AUTH_URL"), envPin = EnvW(L"NOX_AUTH_PIN");
    if (!envUrl.empty()) c.authBase = envUrl;
    if (!envPin.empty()) c.pin = Utf8(envPin);
    return c;
}

inline std::wstring GamePath(const Config& c)
{
    if (c.gameExe.size() > 1 && (c.gameExe[1] == L':' || c.gameExe[0] == L'\\')) return c.gameExe;
    return c.exeDir + L"\\" + c.gameExe;
}

// 기억 — 아이디 저장 · 자동 로그인
struct State {
    std::string lastName;
    bool autoLogin = false;
};

inline std::wstring StatePath()
{
    const std::wstring keys = launchkey::KeyDir();   // …\NoxOnline\keys
    if (keys.empty()) return std::wstring();
    return keys.substr(0, keys.rfind(L'\\')) + L"\\launcher.ini";
}

inline State LoadState()
{
    State s;
    std::string text;
    const std::wstring p = StatePath();
    if (p.empty() || !ReadFileUtf8(p, text)) return s;
    s.lastName = IniValue(text, "last_name");
    if (!launchkey::NameOk(s.lastName)) s.lastName.clear();
    s.autoLogin = IniValue(text, "auto_login") == "1";
    return s;
}

inline bool SaveState(const State& s)
{
    const std::wstring p = StatePath();
    if (p.empty()) return false;
    const std::wstring dir = p.substr(0, p.rfind(L'\\'));
    CreateDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    return WriteFileUtf8(p, "; 녹스 온라인 런처 — 아이디 저장 · 자동 로그인(암호 · 열쇠는 여기 없음)\r\nlast_name = " + s.lastName + "\r\nauto_login = " +
                                (s.autoLogin ? "1" : "0") + "\r\n");
}

inline std::string DeviceName()
{
    wchar_t n[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(n, &len)) return "PC";
    return Utf8(std::wstring(n, len)).substr(0, 64);
}

// 이 프로세스 환경 + NOX_LAUNCH=<이름>(이미 있으면 바꿈) — CreateProcessW 의 환경 덩어리(UNICODE · 끝 \0\0)
//   ★ 11-02 꾸러미 — 설정 파일(NoxLauncher.ini)의 auth_url · auth_pin 도 NOX_AUTH_URL · NOX_AUTH_PIN 으로 넣는다(환경에 이미 있으면 그대로).
//     DLL 이 /token 때 같은 이름을 읽어 서버를 가르므로(wsock32_proxy.cpp AuthLogin) 런처만 알고 게임이 모르면 다른 PC 에서 자작 인증서 검사에 막힌다
inline std::wstring BuildEnvBlock(const Config& c, const std::string& launchName)
{
    std::vector<std::wstring> vars;
    bool hasUrl = false, hasPin = false;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring v(p);
            if (_wcsnicmp(v.c_str(), L"NOX_LAUNCH=", 11) == 0) continue;
            if (_wcsnicmp(v.c_str(), L"NOX_AUTH_URL=", 13) == 0) hasUrl = true;
            if (_wcsnicmp(v.c_str(), L"NOX_AUTH_PIN=", 13) == 0) hasPin = true;
            vars.push_back(v);
        }
        FreeEnvironmentStringsW(env);
    }
    if (!hasUrl && c.authBaseFromFile && !c.authBase.empty()) vars.push_back(L"NOX_AUTH_URL=" + c.authBase);
    if (!hasPin && !c.pin.empty()) vars.push_back(L"NOX_AUTH_PIN=" + Wide(c.pin));
    vars.push_back(L"NOX_LAUNCH=" + Wide(launchName));
    std::wstring block;
    for (const std::wstring& v : vars) {
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// 로그인 서버 일 셋 — 돌려주는 글은 서버 답 그대로(성공 꼴 또는 `NXL!<까닭>`)
enum class Outcome { Ok, Fail };

struct LoginResult {
    Outcome outcome = Outcome::Fail;
    std::string reason;   // 실패 까닭(`LoginFailName` — wrong_password 등 · 열쇠 저장 실패는 key_store)
};

inline std::string ReasonOf(const std::string& line)
{
    return line.compare(0, nox::kLoginFailMark.size(), nox::kLoginFailMark) == 0 ? line.substr(nox::kLoginFailMark.size()) : std::string("server_error");
}

// 암호 → 열쇠 → 이 PC 에 둠 · 기억 고침(saveId — 「아이디 저장」 체크 · 자동 로그인이면 늘 저장)
inline LoginResult Login(const Config& c, const std::string& name, std::string& password, bool remember, bool saveId = true)
{
    LoginResult r;
    authclient::Result a =
        authclient::LauncherLogin(authclient::WithPath(c.authBase, L"/launcher/login"), name, password, DeviceName(), remember, kAuthTimeoutMs, c.pin);
    if (!password.empty()) SecureZeroMemory(&password[0], password.size());
    password.clear();
    if (a.line.compare(0, nox::kLoginKeyMark.size(), nox::kLoginKeyMark) != 0) {
        r.reason = ReasonOf(a.line);
        return r;
    }
    const bool saved = launchkey::Save(name, a.line);
    SecureZeroMemory(&a.line[0], a.line.size());
    if (!saved) {
        r.reason = "key_store";
        return r;
    }
    State s = LoadState();
    s.lastName = saveId || remember ? name : std::string();   // 아이디 저장 기본 켬(결정 2) · 자동 로그인은 이름이 있어야 다음에 시작함
    s.autoLogin = remember;
    SaveState(s);
    r.outcome = Outcome::Ok;
    return r;
}

// 자동 로그인으로 시작할 때 — 둔 열쇠가 아직 되나(`/token` 한 번 · 받은 토큰은 버림). 안 되면 열쇠 지움
inline LoginResult CheckKey(const Config& c, const std::string& name)
{
    LoginResult r;
    std::string key;
    if (!launchkey::Load(name, key)) {
        r.reason = "key_invalid";
        return r;
    }
    authclient::Result a = authclient::Token(authclient::WithPath(c.authBase, L"/token"), name, key, kAuthTimeoutMs, c.pin);
    SecureZeroMemory(&key[0], key.size());
    if (a.line.compare(0, nox::kLoginTokenMark.size(), nox::kLoginTokenMark) == 0) {
        r.outcome = Outcome::Ok;
        return r;
    }
    r.reason = ReasonOf(a.line);
    if (r.reason == nox::LoginFailName(nox::LoginFail::KeyInvalid)) {
        launchkey::Remove(name);
        State s = LoadState();
        s.autoLogin = false;
        SaveState(s);
    }
    return r;
}

// 로그아웃 — 서버에서 지우고(못 닿아도) 이 PC 에서도 지움 · 자동 로그인 끔
inline LoginResult Logout(const Config& c, const std::string& name)
{
    LoginResult r;
    std::string key;
    if (launchkey::Load(name, key)) {
        const authclient::Result a = authclient::Logout(authclient::WithPath(c.authBase, L"/logout"), key, kAuthTimeoutMs, c.pin);
        SecureZeroMemory(&key[0], key.size());
        r.reason = a.line == std::string(authclient::kLogoutOk) ? std::string() : ReasonOf(a.line);
    }
    launchkey::Remove(name);
    State s = LoadState();
    s.autoLogin = false;
    SaveState(s);
    r.outcome = r.reason.empty() ? Outcome::Ok : Outcome::Fail;   // 서버에 못 닿았어도 이 PC 의 열쇠는 지웠다(서버 쪽은 수명이 지우거나 다음 로그인 때 만료 행 정리)
    return r;
}

// 게임 띄우기 — 성공이면 pi(부른 쪽이 핸들을 닫음)
inline bool LaunchGame(const Config& c, const std::string& name, PROCESS_INFORMATION& pi, std::wstring& err)
{
    const std::wstring exe = GamePath(c);
    const std::wstring dir = exe.substr(0, exe.rfind(L'\\'));
    std::wstring cmd = L"\"" + exe + L"\"";
    std::wstring env = BuildEnvBlock(c, name);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, &env[0], dir.c_str(), &si, &pi)) {
        wchar_t b[64];
        swprintf(b, 64, L"CreateProcess 오류 %lu", GetLastError());
        err = b;
        return false;
    }
    return true;
}

}  // namespace launcher
