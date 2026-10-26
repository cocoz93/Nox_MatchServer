// MySqlBind.h — MySQL C API 를 쓰는 .cpp 둘이 같이 쓰는 도우미(이 폴더 안에서만 · MySQL 머리 파일을 부르는 쪽에 안 퍼뜨림).
//   쓰는 곳 = 로비 DB 워커(`DbWorker.cpp`) · 로그인 서버 계정 저장소(`server/src/auth/MySqlAccountStore.cpp`)
//   ★ 로그인 암호 조각 2 — `DbWorker.cpp` 의 이름 없는 공간에서 떼어 냄(쓰는 실제 파일이 둘이 됨)
#pragma once

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 앞에 — 구버전 winsock 자동 include 충돌 차단(MMO_Zone DBWorker.cpp 와 같은 꼴)
#endif
#include <errmsg.h>         // CR_SERVER_GONE_ERROR · CR_SERVER_LOST(끊김)
#include <mysql.h>
#include <mysqld_error.h>   // ER_DUP_ENTRY(열쇠 겹침 — 같은 판 · 같은 이름)

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

#include "nox/DbConfig.h"

namespace nox::db {

// mysql_library_init 은 스레드를 띄우기 전 한 번(끝낼 때 짝을 안 맞춘다 — 프로세스가 끝나며 거둠)
inline bool LibInit() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = mysql_library_init(0, nullptr, nullptr) == 0; });
    return ok;
}

// 끊김 — 다시 붙으면 될 수 있는 것. KILL 당한 연결의 다음 문장이 어느 번호로 오는지는 db_test ④ 가 기록에 찍는다(미확인 — 셋 다 받음)
inline bool IsLost(unsigned e) {
    return e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST || e == CR_SERVER_LOST_EXTENDED || e == ER_SESSION_WAS_KILLED ||
           e == ER_CLIENT_INTERACTION_TIMEOUT;
}

// 문장 자리(?) 묶기 — MYSQL_BIND 는 0 으로 비운 뒤 칸만 채운다(라이브러리가 나머지를 쓴다)
inline void Bin(MYSQL_BIND& b, const std::string& s, unsigned long& len) {   // VARBINARY — 바이트 그대로(글자표 바꾸기 없음)
    len = static_cast<unsigned long>(s.size());
    b.buffer_type = MYSQL_TYPE_BLOB;
    b.buffer = const_cast<char*>(s.data());
    b.buffer_length = len;
    b.length = &len;
}
inline void Txt(MYSQL_BIND& b, const std::string& s, unsigned long& len) {   // VARCHAR · MEDIUMTEXT(utf8mb4)
    Bin(b, s, len);
    b.buffer_type = MYSQL_TYPE_STRING;
}
inline void U8(MYSQL_BIND& b, const uint8_t& v) { b.buffer_type = MYSQL_TYPE_TINY; b.buffer = const_cast<uint8_t*>(&v); b.is_unsigned = true; }
inline void U16(MYSQL_BIND& b, const uint16_t& v) { b.buffer_type = MYSQL_TYPE_SHORT; b.buffer = const_cast<uint16_t*>(&v); b.is_unsigned = true; }
inline void U32(MYSQL_BIND& b, const uint32_t& v) { b.buffer_type = MYSQL_TYPE_LONG; b.buffer = const_cast<uint32_t*>(&v); b.is_unsigned = true; }
inline void I32(MYSQL_BIND& b, const int32_t& v) { b.buffer_type = MYSQL_TYPE_LONG; b.buffer = const_cast<int32_t*>(&v); }
inline void U64(MYSQL_BIND& b, const uint64_t& v) { b.buffer_type = MYSQL_TYPE_LONGLONG; b.buffer = const_cast<uint64_t*>(&v); b.is_unsigned = true; }
inline void Null(MYSQL_BIND& b, bool& isNull) { b.buffer_type = MYSQL_TYPE_NULL; isNull = true; b.is_null = &isNull; }

// 붙기 + 표 버전 확인 — 실패면 nullptr · `err` · `connectFailed`(붙기에서 실패 — 버전 다름과 가름)
//   서버 계정은 표를 못 만든다(정할 것 a) — 버전이 다르면 사람이 SQL 파일을 돌려야 한다
inline MYSQL* OpenChecked(const DbConfig& cfg, std::string& err, bool& connectFailed) {
    connectFailed = false;
    MYSQL* m = mysql_init(nullptr);
    if (!m) {
        err = "mysql_init 실패";
        return nullptr;
    }
    unsigned ct = cfg.connectTimeoutSec, rw = cfg.rwTimeoutSec;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &ct);
    mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &rw);
    mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &rw);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    bool getKey = true;   // caching_sha2_password — TLS 가 없을 때도 첫 접속이 되게(서버 공개키로 암호를 싸 보냄)
    mysql_options(m, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &getKey);
    auto fail = [&](const char* what) {
        err = std::string(what) + " — " + mysql_error(m) + " (" + std::to_string(mysql_errno(m)) + ")";
        mysql_close(m);
        return nullptr;
    };
    if (!mysql_real_connect(m, cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0)) {
        connectFailed = true;
        return fail("붙기");
    }
    if (mysql_query(m, "SELECT version FROM schema_version WHERE id = 1") != 0)
        return fail("표 버전 읽기(schema_version — tools/db/001_init.sql 을 돌렸나)");
    MYSQL_RES* res = mysql_store_result(m);
    MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
    const long ver = row && row[0] ? std::strtol(row[0], nullptr, 10) : -1;
    if (res) mysql_free_result(res);
    if (ver != static_cast<long>(kSchemaVersion)) {
        err = "표 버전 " + std::to_string(ver) + " — 이 실행 파일은 " + std::to_string(kSchemaVersion) + " 을 안다(tools/db/ 의 SQL 파일 차례 확인)";
        mysql_close(m);
        return nullptr;
    }
    return m;
}

}  // namespace nox::db
