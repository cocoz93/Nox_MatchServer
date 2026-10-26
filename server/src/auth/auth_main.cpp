// auth_main.cpp — 로그인 서버 `nox_auth`(Docs/LOGIN_AUTH_DESIGN.md · 로그인 암호 조각 2).
//
//   nox_auth.exe --config <설정 파일> [--port 8443] [--workers 4] [--test-db] [--seconds N] [--quiet]
//
// 왜 따로 된 프로세스인가 — 암호를 지키는 일을 TLS(윈도 http.sys)에 맡기고, 로비 · 게임 서버는 평문 암호를 한 번도 만지지 않게(서명 토큰만 봄).
//   가입 · 로그인 같은 계정 일이 웹 쪽에 모인다(플랜 머리말)
// 조립만 맡는다 — http.sys 요청 큐(`CHttpSysServer`) 하나를 일꾼 N 이 같이 받고, 판단(`CAuthService`)은 하나를 같이 쓰고,
//   계정 저장소(`CMySqlAccountStore`)는 일꾼마다 하나(MySQL 연결은 스레드마다)
//   · 뜨기 전에 다 본다 — `[auth] key` · `[auth_db]` 사용자 · 포트의 인증서 묶음 · 일꾼마다 DB 붙기(표 버전 2) · 주소 예약.
//     하나라도 안 되면 까닭 한 줄 + 종료 코드 2(로비와 같은 꼴 — `lobby_main.cpp`)
//   · 뜰 때 PBKDF2 한 번의 시간을 찍는다(정할 것 f — 이 PC 에서 1초가 넘으면 반복 수를 다시 정함)
//   · 기록에 암호 · 토큰을 안 찍는다(요청 줄 = 방식 · 길 · 상대 IP · 갈래 · 걸린 ms)
//   · 닫기 신호 둘 — 콘솔(Ctrl+C · 창 닫기)과 이름 있는 이벤트 `nox_auth_stop_<pid>`(로비 `nox_lobby_stop_<pid>` 와 같은 꼴). 끝 줄(셈)을 찍고 나간다
// 설정 파일 — `[auth] key = <16진 64자>`(로비와 같은 값 · `[token] key` 와 따로) · `port` · `workers`
//   · `[auth_db] user · password`(DB 사용자 `nox_auth` — accounts 만) · host · port · database 는 `[db]` 를 따름(`--test-db` 면 `[db] test_database`)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AuthService.h"
#include "HttpSysServer.h"
#include "MySqlAccountStore.h"
#include "nox/AccountRules.h"
#include "nox/IniFile.h"

namespace {

bool g_quiet = false;
std::atomic<bool> g_stop{false};
std::mutex g_logMu;

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMu);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stdout, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

std::string Ip(uint32_t ip) {
    char b[16];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    using namespace nox::auth;

    std::string configPath;
    int port = 0, workers = 0, seconds = 0;
    bool testDb = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--quiet") g_quiet = true;
        else if (a == "--config" && hasNext) configPath = argv[++i];
        else if (a == "--port" && hasNext) port = std::atoi(argv[++i]);
        else if (a == "--workers" && hasNext) workers = std::atoi(argv[++i]);
        else if (a == "--seconds" && hasNext) seconds = std::atoi(argv[++i]);
        else if (a == "--test-db") testDb = true;
        else { Log("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    nox::CIniFile ini;
    if (configPath.empty() || !ini.LoadFile(configPath)) { Log("[멈춤] --config <설정 파일> 을 못 읽음(%s)\n", configPath.c_str()); return 2; }
    AuthConfig ac;
    if (!nox::ParseJoinKey(ini.Get("auth", "key"), ac.loginKey)) { Log("[멈춤] [auth] key 가 없거나 16진 64자가 아님\n"); return 2; }
    if (port == 0) port = ini.Get("auth", "port").empty() ? 8443 : std::atoi(ini.Get("auth", "port").c_str());
    if (workers == 0) workers = ini.Get("auth", "workers").empty() ? 4 : std::atoi(ini.Get("auth", "workers").c_str());
    if (port <= 0 || port > 65535 || workers <= 0 || workers > 64) { Log("[멈춤] 포트 %d · 일꾼 %d\n", port, workers); return 2; }

    nox::db::DbConfig dc;
    if (!ini.Get("db", "host").empty()) dc.host = ini.Get("db", "host");
    if (!ini.Get("db", "port").empty()) dc.port = static_cast<uint16_t>(std::atoi(ini.Get("db", "port").c_str()));
    const std::string dbKey = testDb ? "test_database" : "database";
    dc.database = ini.Get("db", dbKey).empty() ? std::string(testDb ? "nox_mo_test" : "nox_mo") : ini.Get("db", dbKey);
    dc.user = ini.Get("auth_db", "user");
    dc.password = ini.Get("auth_db", "password");   // 찍지 않는다
    if (dc.user.empty()) { Log("[멈춤] [auth_db] user 가 없음(config/auth_setup.sql — 사람 1회)\n"); return 2; }

    {   // 정할 것 f — 이 PC 에서 PBKDF2 한 번(설정만 맞으면 찍는다 — 인증서 · DB 가 없어도 시간은 보이게)
        nox::PasswordRecord rec;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = nox::MakePasswordRecord("measure1", ac.iterations, rec);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Log("[로그인 서버] 해시 PBKDF2-HMAC-SHA256 %u회 한 번 = %.1f ms%s\n", ac.iterations, ms, ok ? "" : "(실패)");
    }

    std::string err;
    if (!CHttpSysServer::SslBound(static_cast<uint16_t>(port), err)) { Log("[멈춤] %s\n", err.c_str()); return 2; }

    // 일꾼마다 저장소를 그 스레드에서 붙인다 — 다 붙어야 주소를 연다
    CHttpSysServer http;
    CAuthService service(ac);
    std::vector<std::thread> threads;
    std::atomic<int> ready{0}, failed{0};
    std::mutex errMu;
    std::string firstErr;
    std::atomic<bool> go{false};
    std::atomic<unsigned long long> retries{0}, sendFailed{0}, tooLarge{0}, recvErrors{0};
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            struct ThreadEnd {
                ~ThreadEnd() { CMySqlAccountStore::ThreadDone(); }   // 저장소(연결)가 먼저 닫힌 뒤
            } threadEnd;
            CMySqlAccountStore store(dc);
            std::string e;
            if (!store.Open(e)) {
                std::lock_guard<std::mutex> lk(errMu);
                if (firstErr.empty()) firstErr = "일꾼 " + std::to_string(w) + " DB — " + e;
                ++failed;
                return;
            }
            ++ready;
            while (!go && !g_stop && failed == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!go) return;
            for (;;) {
                HttpRequest req;
                std::string re;
                const ReceiveResult rr = http.Receive(req, re);
                if (rr == ReceiveResult::Closed) break;
                if (rr == ReceiveResult::TooLarge) { ++tooLarge; continue; }
                if (rr == ReceiveResult::Error) { ++recvErrors; Log("[받기 오류] %s\n", re.c_str()); continue; }
                const auto t0 = std::chrono::steady_clock::now();
                AuthOutcome o = AuthOutcome::kCount;
                const HttpResponse resp = service.Handle(req, store, &o);
                std::string se;
                if (!http.Send(req.id, resp, se)) { ++sendFailed; Log("[보내기 오류] %s\n", se.c_str()); }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (!g_quiet) Log("[w%d] %s %s %s → %d %s (%.1f ms)\n", w, req.method.c_str(), req.path.c_str(), Ip(req.peerIp).c_str(), resp.status,
                                  AuthOutcomeName(o), ms);
            }
            retries += store.Retries();
        });
    }
    while (ready + failed < workers) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto stopAll = [&] {
        g_stop = true;
        http.Close();
        for (auto& t : threads) t.join();
    };
    if (failed) {
        stopAll();
        Log("[멈춤] %s\n", firstErr.c_str());
        return 2;
    }
    const std::wstring url = L"https://+:" + std::to_wstring(port) + L"/";
    if (!http.Open(url, err)) {
        stopAll();
        Log("[멈춤] 주소 열기 — %s\n", err.c_str());
        return 2;
    }
    go = true;
    Log("[로그인 서버] 듣기 https://+:%d/ · 일꾼 %d · DB %s:%u/%s(사용자 %s · 표 버전 %u)\n", port, workers, dc.host.c_str(), dc.port,
        dc.database.c_str(), dc.user.c_str(), nox::db::kSchemaVersion);

    wchar_t stopName[64];
    swprintf(stopName, 64, L"nox_auth_stop_%lu", GetCurrentProcessId());
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName);
    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        if (stopEvent && WaitForSingleObject(stopEvent, 100) == WAIT_OBJECT_0) break;
        if (!stopEvent) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
    }
    stopAll();
    if (stopEvent) CloseHandle(stopEvent);

    const auto s = service.Stats();
    unsigned long long total = 0;
    std::string parts;
    for (size_t i = 0; i < kAuthOutcomeCount; ++i) {
        total += s[i];
        parts += std::string(i ? " · " : "") + AuthOutcomeName(static_cast<AuthOutcome>(i)) + " " + std::to_string(s[i]);
    }
    Log("[로그인 서버] 끝 — 요청 %llu(%s) · 너무 큼 %llu · 받기 오류 %llu · 보내기 실패 %llu · DB 끊겨 다시 %llu\n", total, parts.c_str(),
        tooLarge.load(), recvErrors.load(), sendFailed.load(), retries.load());
    return 0;
}
