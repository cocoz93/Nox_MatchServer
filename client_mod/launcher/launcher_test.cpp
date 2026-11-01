// launcher_test.cpp — LauncherCore.h 의 창 없는 몫 시험(런처 P8). test.bat 이 돌린다.
//   ① 설정 줄 읽기(공백 · 주석 · 없는 키 · CRLF) ② 설정 파일 + 환경 변수가 이김 ③ 기억 파일 왕복(임시 LOCALAPPDATA) · 꼴 틀린 이름은 버림
//   ④ 게임 환경 덩어리 — NOX_LAUNCH 하나만(있던 것은 바꿈) · 다른 값 그대로 · 끝 \0\0 ⑤ 게임 경로(상대 · 절대)
//   로그인 서버 길(Login · CheckKey · Logout)은 실제 nox_auth 파일럿으로 본다(전송 몫은 auth_client_test)
#include <cstdio>
#include <string>

#include "LauncherCore.h"

static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("OK    %s\n", what);
    }
}

int main()
{
    // ①
    const std::string ini = "; 주석\r\n  auth_url =  https://h:8443  \r\nauth_pin=abc\r\n;game_exe = no\r\nx = 1 = 2\r\n";
    Check(launcher::IniValue(ini, "auth_url") == "https://h:8443" && launcher::IniValue(ini, "auth_pin") == "abc", "① 키 = 값(앞뒤 빈칸 · CRLF)");
    Check(launcher::IniValue(ini, "game_exe").empty() && launcher::IniValue(ini, "none").empty(), "① 주석 줄 · 없는 키 → 빈 글");
    Check(launcher::IniValue(ini, "x") == "1 = 2", "① 첫 = 에서 나눔");

    // ② 설정 파일 + 환경 변수
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"nox_launcher_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), NULL);
    SetEnvironmentVariableW(L"NOX_AUTH_URL", NULL);
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", NULL);
    launcher::Config c0 = launcher::LoadConfig(dir);
    Check(c0.authBase == L"https://127.0.0.1:8443" && c0.pin.empty() && c0.gameExe == L"Game.exe", "② 설정 파일 없음 → 기본값");
    launcher::WriteFileUtf8(dir + L"\\NoxLauncher.ini", "\xEF\xBB\xBF" "auth_url = https://10.0.0.5:8443\r\nauth_pin = " + std::string(64, 'a') +
                                                            "\r\ngame_exe = C:\\Games\\Nox\\Game.exe\r\n");
    launcher::Config c1 = launcher::LoadConfig(dir);
    Check(c1.authBase == L"https://10.0.0.5:8443" && c1.pin == std::string(64, 'a') && launcher::GamePath(c1) == L"C:\\Games\\Nox\\Game.exe",
          "② 설정 파일(BOM 있어도) — 주소 · 지문 · 절대 게임 경로");
    SetEnvironmentVariableW(L"NOX_AUTH_URL", L"https://1.2.3.4:8443/login");
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", L"bb");
    launcher::Config c2 = launcher::LoadConfig(dir);
    Check(c2.authBase == L"https://1.2.3.4:8443/login" && c2.pin == "bb", "② 환경 변수가 설정 파일을 이김(꾸러미 값)");
    Check(authclient::WithPath(c2.authBase, L"/launcher/login") == L"https://1.2.3.4:8443/launcher/login", "② 옛 꼴 주소에도 길이 바로 붙음");
    SetEnvironmentVariableW(L"NOX_AUTH_URL", NULL);
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", NULL);

    // ③ 기억 파일
    SetEnvironmentVariableW(L"LOCALAPPDATA", dir.c_str());
    Check(launcher::LoadState().lastName.empty() && !launcher::LoadState().autoLogin, "③ 기억 파일 없음 → 빈 이름 · 자동 로그인 끔");
    launcher::State s;
    s.lastName = "wn9457";
    s.autoLogin = true;
    Check(launcher::SaveState(s), "③ 기억 저장(폴더가 없어도 만듦)");
    const launcher::State r = launcher::LoadState();
    Check(r.lastName == "wn9457" && r.autoLogin, "③ 왕복 — 이름 · 자동 로그인");
    launcher::WriteFileUtf8(launcher::StatePath(), "last_name = bad name\r\nauto_login = 1\r\n");
    Check(launcher::LoadState().lastName.empty(), "③ 꼴 틀린 이름(빈칸)은 버림");
    std::string text;
    launcher::ReadFileUtf8(launcher::StatePath(), text);
    Check(text.find("password") == std::string::npos && text.find("NXK1") == std::string::npos, "③ 기억 파일엔 암호 · 열쇠 칸이 없음");

    // ④ 게임 환경 덩어리
    SetEnvironmentVariableW(L"NOX_LAUNCH", L"old");
    SetEnvironmentVariableW(L"NOX_TEST_KEEP", L"keep me");
    const std::wstring env = launcher::BuildEnvBlock("pilot1");
    int launch = 0;
    bool keep = false, old = false;
    for (const wchar_t* p = env.c_str(); *p; p += wcslen(p) + 1) {
        const std::wstring v(p);
        if (v.rfind(L"NOX_LAUNCH=", 0) == 0) ++launch;
        if (v == L"NOX_LAUNCH=old") old = true;
        if (v == L"NOX_TEST_KEEP=keep me") keep = true;
    }
    Check(launch == 1 && !old && keep, "④ NOX_LAUNCH 하나뿐(있던 값은 바꿈) · 다른 값 그대로");
    Check(env.size() >= 2 && env[env.size() - 1] == L'\0' && env[env.size() - 2] == L'\0', "④ 끝이 \\0\\0");
    SetEnvironmentVariableW(L"NOX_LAUNCH", NULL);

    // ⑤ 게임 경로
    launcher::Config c3;
    c3.exeDir = L"D:\\Nox";
    Check(launcher::GamePath(c3) == L"D:\\Nox\\Game.exe", "⑤ 상대 이름은 런처 폴더 기준");
    c3.gameExe = L"Game2.exe";
    Check(launcher::GamePath(c3) == L"D:\\Nox\\Game2.exe", "⑤ 다른 이름(클라2)");

    DeleteFileW(launcher::StatePath().c_str());
    DeleteFileW((dir + L"\\NoxLauncher.ini").c_str());
    RemoveDirectoryW((dir + L"\\NoxOnline").c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("launcher checks=%d fail=%d\n%s\n", g_checks, g_fail, g_fail ? "LAUNCHER FAIL" : "LAUNCHER PASS");
    return g_fail ? 1 : 0;
}
