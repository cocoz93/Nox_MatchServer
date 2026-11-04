// match_totals_main.cpp — 승패 합계 표를 판 행에서 다시 계산하는 일회성 도구(`nox/MatchTotalsRebuild.h`).
//   nox_match_totals.exe --config <설정 파일> [--test-db]
//   · 계정 = 설정 `[db] user`(`nox_mo` — 표 하나 쓰기 권한은 `config/match_totals_setup.sql`) · DB = `[db] database`(`--test-db` 면 `test_database`)
//   · 어느 DB 를 고치는지 시작할 때 찍는다 · 암호는 안 찍는다 · 로비가 꺼진 때 돌린다
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "nox/IniFile.h"
#include "nox/MatchTotalsRebuild.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string configPath;
    bool testDb = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == "--test-db") testDb = true;
        else {
            std::printf("[실패] 모르는 인자: %s\n", a.c_str());
            return 2;
        }
    }
    nox::CIniFile ini;
    if (configPath.empty() || !ini.LoadFile(configPath)) {
        std::printf("[멈춤] --config <설정 파일> 을 못 읽음(%s)\n", configPath.c_str());
        return 2;
    }
    nox::db::DbConfig dc;
    if (!ini.Get("db", "host").empty()) dc.host = ini.Get("db", "host");
    if (!ini.Get("db", "port").empty()) dc.port = static_cast<uint16_t>(std::atoi(ini.Get("db", "port").c_str()));
    dc.user = ini.Get("db", "user");
    dc.password = ini.Get("db", "password");   // 찍지 않는다
    const std::string dbKey = testDb ? "test_database" : "database";
    dc.database = ini.Get("db", dbKey).empty() ? std::string(testDb ? "nox_mo_test" : "nox_mo") : ini.Get("db", dbKey);
    if (dc.user.empty()) {
        std::printf("[멈춤] 설정 파일에 [db] user 가 없다(%s)\n", configPath.c_str());
        return 2;
    }
    std::printf("[다시 계산] DB %s:%u/%s · 사용자 %s\n", dc.host.c_str(), dc.port, dc.database.c_str(), dc.user.c_str());
    std::fflush(stdout);
    nox::db::MatchTotalsRebuildResult r;
    std::string err;
    if (!nox::db::RebuildMatchTotals(dc, r, err)) {
        std::printf("[실패] %s — 표는 그대로\n", err.c_str());
        return 1;
    }
    std::printf("[끝] 있던 행 %llu 을 0 으로 · 판 줄 %llu · 합계 쓴 계정 %llu\n", r.existing, r.rows, r.accounts);
    return 0;
}
