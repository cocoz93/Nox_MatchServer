// launcher_test.cpp — LauncherCore.h 의 창 없는 몫 시험(런처 P8). test.bat 이 돌린다.
//   ① 설정 줄 읽기(공백 · 주석 · 없는 키 · CRLF) ② 설정 파일 + 환경 변수가 이김 ③ 기억 파일 왕복(임시 LOCALAPPDATA) · 꼴 틀린 이름은 버림
//   ④ 게임 환경 덩어리 — NOX_LAUNCH 하나만(있던 것은 바꿈) · 다른 값 그대로 · 끝 \0\0 ⑤ 게임 경로(상대 · 절대)
//   ⑥ 그림 풀기(NoxArt.h) — 줄 묶음 · 잘린 자료 · PNG 꼴 · 게임 폴더 없음 · (NOX_TEST_GAME · NOX_TEST_ART 가 있으면) 정품에서 실제로 뽑기
//   로그인 서버 길(Login · CheckKey · Logout)은 실제 nox_auth 파일럿으로 본다(전송 몫은 auth_client_test)
#include <cstdio>
#include <string>

#include "LauncherCore.h"
#include "NoxArt.h"

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
    const std::wstring env = launcher::BuildEnvBlock(launcher::Config(), "pilot1");
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
    // ④ 꾸러미 — 설정 파일의 auth_url · auth_pin 이 게임 환경에도(환경에 이미 있으면 환경이 이김 · 기본 주소 · 빈 지문은 안 넣음)
    auto envVals = [](const std::wstring& block, const wchar_t* key) {   // <개수, 마지막 값>
        std::pair<int, std::wstring> r(0, L"");
        const std::wstring k = std::wstring(key) + L"=";
        for (const wchar_t* p = block.c_str(); *p; p += wcslen(p) + 1) {
            const std::wstring v(p);
            if (v.rfind(k, 0) == 0) {
                ++r.first;
                r.second = v.substr(k.size());
            }
        }
        return r;
    };
    SetEnvironmentVariableW(L"NOX_AUTH_URL", NULL);
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", NULL);
    launcher::Config cf;
    cf.authBase = L"https://10.0.0.5:8443";
    cf.authBaseFromFile = true;
    cf.pin = std::string(64, 'c');
    const std::wstring envF = launcher::BuildEnvBlock(cf, "pilot1");
    Check(envVals(envF, L"NOX_AUTH_URL") == std::make_pair(1, std::wstring(L"https://10.0.0.5:8443")) &&
              envVals(envF, L"NOX_AUTH_PIN") == std::make_pair(1, std::wstring(64, L'c')),
          "④ 설정 파일 주소 · 지문이 게임 환경에 하나씩");
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", L"bb");
    const std::wstring envE = launcher::BuildEnvBlock(cf, "pilot1");
    Check(envVals(envE, L"NOX_AUTH_PIN") == std::make_pair(1, std::wstring(L"bb")) && envVals(envE, L"NOX_AUTH_URL").first == 1,
          "④ 환경에 있는 지문은 그대로(설정 파일 값으로 안 덮음) · 주소는 설정 파일 것");
    SetEnvironmentVariableW(L"NOX_AUTH_PIN", NULL);
    const std::wstring envD = launcher::BuildEnvBlock(launcher::Config(), "pilot1");
    Check(envVals(envD, L"NOX_AUTH_URL").first == 0 && envVals(envD, L"NOX_AUTH_PIN").first == 0, "④ 설정 파일 없음(기본 주소 · 빈 지문) → 둘 다 안 넣음");

    // ⑤ 게임 경로
    launcher::Config c3;
    c3.exeDir = L"D:\\Nox";
    Check(launcher::GamePath(c3) == L"D:\\Nox\\Game.exe", "⑤ 상대 이름은 런처 폴더 기준");
    c3.gameExe = L"Game2.exe";
    Check(launcher::GamePath(c3) == L"D:\\Nox\\Game2.exe", "⑤ 다른 이름(클라2)");

    // ⑥ 그림 풀기(NoxArt.h) — 줄 묶음 3종 · 잘린 자료는 실패
    {
        std::vector<uint8_t> d(17, 0);
        d[0] = 3;   // 폭 3
        d[4] = 2;   // 높이 2
        // 첫 줄: 빈칸 1 + RGB555 빨강 2 / 둘째 줄: RGBA4444 1 + 재질 2
        const uint8_t body[] = { 1, 1, 2, 2, 0x00, 0x7C, 0x00, 0x7C, 5, 1, 0x3F, 0xF0, 4, 2, 0x80, 0x40 };
        d.insert(d.end(), body, body + sizeof(body));
        noxart::Image im;
        Check(noxart::DecodeImage(d.data(), d.size(), 3, im) && im.w == 3 && im.h == 2, "⑥ 그림 풀림(폭 · 높이)");
        const auto px = [&im](int x, int y) { return &im.rgba[(static_cast<size_t>(y) * im.w + x) * 4]; };
        Check(px(0, 0)[3] == 0 && px(1, 0)[0] == 0xF8 && px(1, 0)[1] == 0 && px(2, 0)[3] == 0xFF, "⑥ 빈칸은 투명 · RGB555 빨강");
        Check(px(0, 1)[0] == 0xF0 && px(0, 1)[3] == 0xF0 && px(1, 1)[0] == 0x80 && px(2, 1)[1] == 0x40, "⑥ RGBA4444 · 재질은 회색");
        noxart::Image cut;
        Check(!noxart::DecodeImage(d.data(), d.size() - 1, 3, cut), "⑥ 잘린 자료는 실패");
        Check(!noxart::DecodeImage(d.data(), d.size(), 9, cut), "⑥ 모르는 종류는 실패");
        const std::vector<uint8_t> png = noxart::EncodePng(im);
        Check(png.size() > 40 && png[1] == 'P' && png[12] == 'I' && std::string(png.end() - 8, png.end() - 4) == "IEND", "⑥ PNG 꼴");
    }
    {
        // 없는 게임 폴더 → 실패(화면은 기본 모양) · 그림 폴더도 안 만듦
        const std::wstring art = dir + L"\\art_none";
        Check(!noxart::Extract(dir + L"\\no_game", art) && GetFileAttributesW(art.c_str()) == INVALID_FILE_ATTRIBUTES, "⑥ 게임 폴더 없음 → 실패");
    }
    // 실물 — NOX_TEST_GAME 이 있으면 정품 video.bag 에서 NOX_TEST_ART 로 뽑아 본다(값 대조는 부른 쪽 · 시간도 찍음)
    {
        const std::wstring game = launcher::EnvW(L"NOX_TEST_GAME"), art = launcher::EnvW(L"NOX_TEST_ART");
        if (!game.empty() && !art.empty()) {
            const DWORD t0 = GetTickCount();
            Check(noxart::Extract(game, art), "⑥ 실물 — 정품 폴더에서 뽑기");
            std::printf("art extract %lu ms\n", GetTickCount() - t0);
            const DWORD t1 = GetTickCount();
            Check(noxart::Extract(game, art), "⑥ 실물 — 두 번째는 건너뜀");
            std::printf("art again %lu ms\n", GetTickCount() - t1);
        }
    }

    DeleteFileW(launcher::StatePath().c_str());
    DeleteFileW((dir + L"\\NoxLauncher.ini").c_str());
    RemoveDirectoryW((dir + L"\\NoxOnline").c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("launcher checks=%d fail=%d\n%s\n", g_checks, g_fail, g_fail ? "LAUNCHER FAIL" : "LAUNCHER PASS");
    return g_fail ? 1 : 0;
}
