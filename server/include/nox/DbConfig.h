// DbConfig.h — MySQL 에 붙을 곳 · 표 버전. 로비 DB 워커(`DbWorker.h`)와 로그인 서버(`nox_auth`)가 같이 쓴다.
//   ★ 로그인 암호 조각 2 — `DbWorker.h` 에서 떼어 냄(쓰는 실행 파일이 둘이 됨 · 로그인 서버는 로비 저장소 경계를 모른다)
#pragma once

#include <cstdint>
#include <string>

namespace nox::db {

constexpr uint32_t kSchemaVersion = 2;   // `tools/db/` SQL 파일을 차례로 돌린 schema_version — 표를 바꾸면 SQL 파일과 같이 올린다
                                         //   ★ 로그인 암호 조각 1 — 2 = `002_auth.sql`(pass_iter · 로비는 그 칸을 안 씀)

struct DbConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user;
    std::string password;            // 기록에 안 찍는다
    std::string database;
    unsigned connectTimeoutSec = 2;  // 근거 없는 값 둘 — 로그인 답 한도(`kLoginCheckTimeoutMs` 5초)보다 짧게(10-19 검사 지적 7)
    unsigned rwTimeoutSec = 3;
};

}  // namespace nox::db
