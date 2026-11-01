// db_test.cpp — 로비 DB 워커(`CDbWorker`) 시험 — 진짜 MySQL 의 시험 DB 에 붙는다(MO 7단계 조각 7 · `Docs/ACCOUNT_DB_DESIGN.md`).
//
//   db_test <설정 파일> <tools/db/001_init.sql> [<002_auth.sql> …](★ 로그인 암호 조각 1 — SQL 파일을 차례로 깐다)
//   설정 [db] 의 test_database 에 표를 새로 깔고(그 DB 만 표 만들기 권한 — `config/db_setup.sql` · 정할 것 a) 워커를 그 DB 로 띄운다.
//   설정이 없거나 붙지 못하면(사람 1회 전) 종료 코드 77 — ctest 「건너뜀」(통과로 안 보이게 `SKIP_RETURN_CODE`)
//   ① 계정 셋을 시험이 깐다(★ 로그인 암호 조각 4 — 계정은 로그인 서버가 만들고 로비는 계정 확인 길을 걷어냄 · 정할 것 a)
//   ② 판 저장 — Saved · 읽어 칸 전부 대조(판 · 사람 · 계정 번호 · NULL 칸 · 원문) · 같은 판 Duplicate(행 그대로) · 다른 게임 서버의 같은 판 번호 Saved
//      · 사람 행이 실패하면 판 행도 안 남음(한 트랜잭션 — 시험 표에 CHECK 를 잠깐 걸어 둘째 사람을 실패시킴)
//   ③ 칸 한도 → Invalid(DB 에 안 감) ④ 끊김 — 같은 계정으로 워커 연결을 KILL → 다음 요청이 다시 붙어 성공(retries 1)
//   ⑤ 표 버전이 다르면 Start 실패 ⑥ 끝낼 때 남은 일을 다 함(Stop 직전에 넣은 판 셋이 다 들어감) ⑦ 셈
//   ⑧ ★ 레이팅 조각 3(⑤ 와 ⑥ 사이) — 랭크 판 점수 · 겹친 판 안 셈 · 일반 판 NULL · 점수 · 순위 읽기 · 읽기 실패
//   ⑨ ★ 런처 P1(⑤ 와 ⑧ 사이) — 로그인 서버의 MySQL 열쇠 저장소(넣기 · 찾기 · 쓰임 · 만료 지움 · 같은 지문 · 지우기)
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nox/DbWorker.h"
#include "nox/IniFile.h"
#include "../src/auth/MySqlAccountStore.h"   // ★ 런처 P1 — ⑨ 열쇠 저장소

namespace {

int g_checks = 0;
int g_fails = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  [실패] %s\n", what.c_str());
    }
}

std::string Hex(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (const unsigned char c : s) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}

// 시험 쪽 연결(표 깔기 · 대조 · KILL) — 워커와 같은 계정 · 여러 문장 한 번에
struct Admin {
    MYSQL* m = nullptr;
    ~Admin() {
        if (m) mysql_close(m);
    }
    bool Connect(const nox::db::DbConfig& c, std::string& err) {
        m = mysql_init(nullptr);
        bool getKey = true;
        mysql_options(m, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &getKey);
        mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (!mysql_real_connect(m, c.host.c_str(), c.user.c_str(), c.password.c_str(), c.database.c_str(), c.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
            err = std::string(mysql_error(m)) + " (" + std::to_string(mysql_errno(m)) + ")";
            return false;
        }
        // 잠금 기다림 한도 — 기본은 표 잠금 1년 · 행 잠금 50초라, 워커가 트랜잭션을 열어 둔 채면(커밋 빠짐) 아래 ALTER 가 실패 대신 멈춘다(10-21 돌연변이 D10)
        if (mysql_query(m, "SET SESSION lock_wait_timeout = 5, innodb_lock_wait_timeout = 5") != 0) {
            err = std::string("잠금 한도 — ") + mysql_error(m);
            return false;
        }
        return true;
    }
    bool Run(const std::string& sql) {   // 여러 문장 — 결과는 다 비운다
        if (mysql_query(m, sql.c_str()) != 0) {
            std::printf("  SQL 실패: %s — %s\n", sql.substr(0, 80).c_str(), mysql_error(m));
            return false;
        }
        int st = 0;
        do {
            if (MYSQL_RES* r = mysql_store_result(m)) mysql_free_result(r);
            st = mysql_next_result(m);
        } while (st == 0);
        return st == -1;
    }
    std::vector<std::vector<std::string>> Rows(const std::string& sql) {
        std::vector<std::vector<std::string>> out;
        if (mysql_query(m, sql.c_str()) != 0) {
            std::printf("  SQL 실패: %s — %s\n", sql.substr(0, 80).c_str(), mysql_error(m));
            return out;
        }
        MYSQL_RES* r = mysql_store_result(m);
        if (!r) return out;
        const unsigned n = mysql_num_fields(r);
        while (MYSQL_ROW row = mysql_fetch_row(r)) {
            unsigned long* len = mysql_fetch_lengths(r);
            std::vector<std::string> v;
            for (unsigned i = 0; i < n; ++i) v.push_back(row[i] ? std::string(row[i], len[i]) : std::string("NULL"));
            out.push_back(std::move(v));
        }
        mysql_free_result(r);
        return out;
    }
    std::string One(const std::string& sql) {
        const auto r = Rows(sql);
        return r.empty() || r[0].empty() ? std::string("(없음)") : r[0][0];
    }
};

// 답 n 개를 기다린다(5초 한도)
std::vector<nox::wol::StoreAnswer> WaitAnswers(nox::db::CDbWorker& w, size_t n) {
    std::vector<nox::wol::StoreAnswer> got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < n && std::chrono::steady_clock::now() < until) {
        w.TakeAnswers(got);
        if (got.size() < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return got;
}

nox::MatchRecord Sample(const std::string& match) {
    nox::MatchRecord r;
    r.match = match;
    r.room = 3;
    r.roomName = "\xEC\xB2\xA0's game";   // 철's game(UTF-8)
    r.map = "estate";
    r.mode = "arena";
    r.start = 1760000100u;
    r.end = 1760000117u;
    r.frames = 510;
    r.sec = 17;
    r.endReason = "score_player";
    r.winnerKind = "player";
    r.hasWinnerId = true;
    r.winnerId = 2;
    r.hasWinnerAccount = true;
    r.winnerAccount = std::string("\xb0\xc7", 2);   // 949 바이트
    r.limit = 1;
    r.timeLimitSec = 300;
    nox::MatchRecordPlayer a, b, c, d;
    a.hasAccount = true;
    a.account = "wn";
    a.netCode = 1;
    a.team = 0;
    a.score = -1;
    a.deaths = 4294967295u;
    b.hasAccount = true;
    b.account = std::string("\xb0\xc7", 2);
    b.netCode = 2;
    b.score = 1;
    b.observer = true;
    c.hasAccount = true;
    c.account = "ghost";   // 계정 없음 → account_id NULL
    c.netCode = 65535;
    c.team = 2;
    c.score = 2147483647;
    d.netCode = 4;         // 로비를 안 거침(이름 null) · 나감
    d.left = true;
    r.players = {a, b, c, d};
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace nox::wol;
    if (argc < 3) {
        std::printf("쓰는 법: db_test <설정 파일> <001_init.sql> [<002_auth.sql> …]\n");
        return 2;
    }
    nox::CIniFile ini;
    if (!ini.LoadFile(argv[1]) || ini.Get("db", "user").empty()) {
        std::printf("[건너뜀] 설정 [db] 가 없다(%s) — 사람 1회(config/db_setup.sql) 전\n", argv[1]);
        return 77;
    }
    nox::db::DbConfig cfg;
    cfg.host = ini.Get("db", "host").empty() ? "127.0.0.1" : ini.Get("db", "host");
    cfg.port = static_cast<uint16_t>(ini.Get("db", "port").empty() ? 3306 : std::atoi(ini.Get("db", "port").c_str()));
    cfg.user = ini.Get("db", "user");
    cfg.password = ini.Get("db", "password");
    cfg.database = ini.Get("db", "test_database").empty() ? "nox_mo_test" : ini.Get("db", "test_database");
    mysql_library_init(0, nullptr, nullptr);
    Admin ad;
    std::string err;
    if (!ad.Connect(cfg, err)) {
        std::printf("[건너뜀] 시험 DB %s 에 못 붙음 — %s(사람 1회 config/db_setup.sql 전이면 이 꼴)\n", cfg.database.c_str(), err.c_str());
        return 77;
    }
    bool laid = ad.Run("DROP TABLE IF EXISTS login_keys, ratings, match_players, matches, accounts, schema_version");   // ★ 레이팅 조각 3 — ratings 가 accounts 를 가리켜 먼저 · ★ 런처 P1 — login_keys 도
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string schema = ss.str();
        laid = laid && !schema.empty() && ad.Run(schema);
    }
    Check(laid, "표 새로 깔기(시험 DB · SQL 파일 " + std::to_string(argc - 2) + "개 그대로 차례로)");

    nox::db::CDbWorker w(cfg);
    Check(w.Start(err), "워커 뜸(붙기 · 표 버전 · 준비된 문장) " + err);

    // ① ★ 로그인 암호 조각 4 — 계정은 로그인 서버가 만든다(로비는 계정 확인 길을 걷어냄 · 정할 것 a). 시험이 계정 행을 직접 깐다 —
    //   대소문자만 다른 이름 · 949 바이트 이름(판 사람의 계정 번호를 이름 바이트 그대로 찾는지 ② 가 본다)
    Check(ad.Run("INSERT INTO accounts (name, created_unix, last_login_unix) VALUES ('wn', 1, 1), ('Wn', 1, 1), (0xB0C7, 1, 1)"),
          "① 계정 셋 깔기(wn · Wn · 949 바이트)");
    const std::string idWn = ad.One("SELECT id FROM accounts WHERE name = 'wn'"), id949 = ad.One("SELECT id FROM accounts WHERE name = 0xB0C7");
    Check(!idWn.empty() && !id949.empty() && idWn != id949 && ad.One("SELECT COUNT(*) FROM accounts") == "3", "① 계정 번호 셋");

    // ②
    const MatchSource srcA{0x0a000001u, 18590}, srcB{0x0a000002u, 18590};
    const nox::MatchRecord m1 = Sample("1760000000-3-1");
    const std::string raw1 = "{\"match\":\"1760000000-3-1\",\"x\":\"\xEC\xB2\xA0\"}";
    w.SaveMatch(10, srcA, m1, raw1);
    auto a2 = WaitAnswers(w, 1);
    Check(a2.size() == 1 && a2[0].kind == StoreAnswerKind::SaveMatch && a2[0].req == 10 && a2[0].save == SaveResult::Saved, "② 판 저장 → Saved " + (a2.empty() ? "" : a2[0].why));
    const auto mr = ad.Rows(
        "SELECT id, server_ip, server_port, match_key, room_id, HEX(room_name), map_key, mode, start_unix, end_unix, frames, end_reason, winner_kind, "
        "IFNULL(winner_id,'NULL'), IFNULL(HEX(winner_name),'NULL'), score_limit, time_limit_sec, raw_json FROM matches ORDER BY id");
    const bool matchRow = mr.size() == 1 && mr[0][1] == std::to_string(srcA.ip) && mr[0][2] == "18590" && mr[0][3] == m1.match && mr[0][4] == "3" &&
                          mr[0][5] == Hex(m1.roomName) && mr[0][6] == "estate" && mr[0][7] == "arena" && mr[0][8] == "1760000100" &&
                          mr[0][9] == "1760000117" && mr[0][10] == "510" && mr[0][11] == "score_player" && mr[0][12] == "player" && mr[0][13] == "2" &&
                          mr[0][14] == "B0C7" && mr[0][15] == "1" && mr[0][16] == "300" && mr[0][17] == raw1;
    Check(matchRow, "② matches 한 행 — 칸 열여덟 전부(등록 주소 · 포트 · 판 번호 · 방 이름 바이트 · 승자 949 · 원문 JSON UTF-8)");
    const std::string mid = mr.empty() ? "0" : mr[0][0];
    const auto pr = ad.Rows("SELECT seat, IFNULL(account_id,'NULL'), IFNULL(HEX(name),'NULL'), net_code, state, IFNULL(team,'NULL'), IFNULL(score,'NULL'), "
                            "IFNULL(deaths,'NULL'), observer FROM match_players WHERE match_id = " + mid + " ORDER BY seat");

    const bool players = pr.size() == 4 && pr[0][0] == "0" && pr[0][1] == idWn && pr[0][2] == Hex("wn") && pr[0][3] == "1" && pr[0][4] == "finished" &&
                         pr[0][5] == "0" && pr[0][6] == "-1" && pr[0][7] == "4294967295" && pr[0][8] == "0" && pr[1][1] == id949 && pr[1][2] == "B0C7" &&
                         pr[1][8] == "1" && pr[2][1] == "NULL" && pr[2][2] == Hex("ghost") && pr[2][3] == "65535" && pr[2][5] == "2" &&
                         pr[2][6] == "2147483647" && pr[3][1] == "NULL" && pr[3][2] == "NULL" && pr[3][4] == "left" && pr[3][5] == "NULL" &&
                         pr[3][6] == "NULL" && pr[3][7] == "NULL";
    Check(players, "② match_players 넷 — (판, 자리) · 계정 번호(이름으로 찾음 · 없으면 NULL) · 바이트 이름 · 음수 · 최대 값 · 나간 사람 NULL 칸");
    w.SaveMatch(11, srcA, m1, raw1);
    w.SaveMatch(12, srcB, m1, raw1);
    auto a3 = WaitAnswers(w, 2);
    Check(a3.size() == 2 && a3[0].save == SaveResult::Duplicate && a3[1].save == SaveResult::Saved && ad.One("SELECT COUNT(*) FROM matches") == "2" &&
              ad.One("SELECT COUNT(*) FROM match_players") == "8",
          "② 같은 판 다시 → Duplicate(행 그대로) · 다른 게임 서버의 같은 판 번호 → Saved(판 둘 · 사람 여덟)");
    // 한 트랜잭션 — 시험 표에 CHECK 를 걸어 둘째 사람 넣기를 실패시킨다(10-21 돌연변이 D9 · autocommit 이 살아남음).
    //   바로 뒤 판까지 넣는 것은 ROLLBACK 을 빠뜨린 경우를 잡으려고 — 열린 트랜잭션은 다음 START TRANSACTION 이 커밋해 버린다
    Check(ad.Run("ALTER TABLE match_players ADD CONSTRAINT ck_db_test CHECK (net_code <> 4242)"), "② 시험용 CHECK 걸기");
    nox::MatchRecord half = Sample("1760000000-3-4");
    half.players[1].netCode = 4242;
    w.SaveMatch(15, srcA, half, "{}");
    w.SaveMatch(16, srcA, Sample("1760000000-3-5"), "{}");
    auto a6 = WaitAnswers(w, 2);
    Check(a6.size() == 2 && a6[0].save == SaveResult::Failed && !a6[0].why.empty() && a6[1].save == SaveResult::Saved &&
              ad.One("SELECT COUNT(*) FROM matches WHERE match_key = '1760000000-3-4'") == "0" && ad.One("SELECT COUNT(*) FROM matches") == "3" &&
              ad.One("SELECT COUNT(*) FROM match_players") == "12",
          "② 둘째 사람 넣기 실패 → Failed · 판 행 · 첫 사람 행 안 남음(한 트랜잭션) · 다음 판 Saved(판 셋 · 사람 열둘) " +
              (a6.empty() ? std::string() : a6[0].why));
    Check(ad.Run("ALTER TABLE match_players DROP CHECK ck_db_test"), "② 시험용 CHECK 풀기");

    // ③
    nox::MatchRecord big = Sample("1760000000-3-2");
    big.roomName = std::string(65, 'r');
    w.SaveMatch(13, srcA, big, "{}");
    w.SaveMatch(14, srcA, Sample("1760000000-3-3"), std::string("{\"x\":\"\xb0\"}"));   // UTF-8 아닌 원문
    auto a4 = WaitAnswers(w, 2);
    Check(a4.size() == 2 && a4[0].save == SaveResult::Invalid && !a4[0].why.empty() && a4[1].save == SaveResult::Invalid &&
              ad.One("SELECT COUNT(*) FROM matches") == "3",
          "③ 방 이름 65바이트 · UTF-8 아닌 원문 → Invalid(DB 에 안 감)");

    // ④
    const unsigned long cid = w.ConnectionId();
    Check(cid != 0 && ad.Run("KILL " + std::to_string(cid)), "④ 워커 연결을 KILL(같은 계정 — 제 연결은 권한 없이 끊음)");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    w.SaveMatch(20, srcA, Sample("1760000000-3-20"), "{}");   // ★ 로그인 암호 조각 4 — 로그인 대신 판으로(끊김 뒤 다시 붙기 · 계정 찾기도 지나감)
    auto a5 = WaitAnswers(w, 1);
    const nox::db::DbStats s4 = w.Stats();
    Check(a5.size() == 1 && a5[0].save == SaveResult::Saved && s4.retries == 1 && s4.connects == 2 && w.ConnectionId() != cid,
          "④ 끊긴 뒤 다음 요청 → 다시 붙어 성공(다시 1 · 붙음 2 · 새 연결 번호) " + s4.lastError);

    // ⑤
    Check(ad.Run("UPDATE schema_version SET version = 99 WHERE id = 1"), "⑤ 표 버전을 99 로");
    {
        nox::db::CDbWorker w2(cfg);
        std::string e2;
        Check(!w2.Start(e2) && e2.find("표 버전 99") != std::string::npos, "⑤ 표 버전이 다르면 Start 실패(" + e2 + ")");
    }
    Check(ad.Run("UPDATE schema_version SET version = " + std::to_string(nox::db::kSchemaVersion) + " WHERE id = 1"), "⑤ 표 버전 되돌림");

    // ⑨ ★ 런처 P1 — 로그인 서버의 MySQL 열쇠 저장소(`004_launcher_keys.sql`). 시험 DB 를 로비 사용자로 붙는다
    //   (진짜 로그인 서버는 nox_auth 사용자 — 그 권한은 사람 1회 `config/launcher_setup.sql` · auth_smoke 가 본다)
    {
        using nox::auth::FindResult;
        nox::auth::CMySqlAccountStore st(cfg);
        std::string e9;
        const bool opened = st.Open(e9);
        Check(opened, "⑨ 열쇠 저장소 Open(준비된 문장 아홉) " + e9);
        const uint64_t wnId = std::strtoull(ad.One("SELECT id FROM accounts WHERE name = 'wn'").c_str(), nullptr, 10);
        nox::auth::LoginKeyHash h1{}, h2{}, h3{};
        h1.fill(1);
        h2.fill(2);
        h3.fill(3);
        nox::auth::LoginKeyRow k;
        const bool ins = opened && st.InsertKey(wnId, h1, "PC1", 100, 1000) && st.InsertKey(wnId, h2, "PC2", 50, 1000);
        const bool found = st.FindKey(h1, k) == FindResult::Found && k.accountId == wnId && k.accountName == "wn" && k.ttlSec == 100 &&
                           k.expiresUnix == 1100 && ad.One("SELECT device FROM login_keys WHERE key_hash = UNHEX(REPEAT('02', 32))") == "PC2";
        const bool none = st.FindKey(h3, k) == FindResult::NotFound;
        const bool touched = st.TouchKey(h1, 1090, 1190) && st.FindKey(h1, k) == FindResult::Found && k.expiresUnix == 1190 &&
                             ad.One("SELECT last_used_unix FROM login_keys WHERE key_hash = UNHEX(REPEAT('01', 32))") == "1090";
        // 1060 에 새 열쇠 — 만료 1050 인 h2 는 지워지고 1190 인 h1 은 남는다
        const bool pruned = st.InsertKey(wnId, h3, "PC3", 10, 1060) && ad.One("SELECT COUNT(*) FROM login_keys") == "2" &&
                            st.FindKey(h2, k) == FindResult::NotFound;
        const bool dup = !st.InsertKey(wnId, h3, "PC3", 10, 1060);   // 같은 지문 → 실패(유일 색인)
        const bool deleted = st.DeleteKey(h1) && st.DeleteKey(h1) && st.FindKey(h1, k) == FindResult::NotFound &&
                             ad.One("SELECT COUNT(*) FROM login_keys") == "1";
        Check(ins && found && none && touched && pruned && dup && deleted,
              "⑨ 열쇠 — 넣기 · 찾기(계정 이름 이음 · 기기) · 없음 · 쓰임 · 만료 지움 · 같은 지문 실패 · 지우기 두 번 (" +
                  std::to_string(ins) + std::to_string(found) + std::to_string(none) + std::to_string(touched) + std::to_string(pruned) +
                  std::to_string(dup) + std::to_string(deleted) + ") " + st.LastError());
        Check(ad.Run("DELETE FROM login_keys"), "⑨ 열쇠 행 치움");
    }

    // ⑧ ★ 레이팅 조각 3 — 랭크 판이 같은 트랜잭션에서 점수를 고치나 · 겹친 판은 안 고치나 · 일반 판은 NULL · 점수 · 순위 읽기
    Check(ad.One("SELECT COUNT(*) FROM matches WHERE ranked <> 0") == "0" && ad.One("SELECT COUNT(*) FROM match_players WHERE rating_delta IS NOT NULL") == "0" &&
              ad.One("SELECT COUNT(*) FROM ratings") == "0",
          "⑧ 일반 판 넷 — ranked 0 · rating_delta 모두 NULL · ratings 빈 표");
    nox::MatchRecord rk1 = Sample("1760000000-3-40");   // wn(−1) · 949 관전 · ghost(계정 행 없음 · 최대 점수) · 이름 없는 나감
    rk1.ranked = true;
    nox::MatchRecord rk2 = Sample("1760000000-3-41");   // wn 3 · Wn 1 → wn +10 · Wn −10
    rk2.ranked = true;
    rk2.winnerAccount = "wn";
    rk2.players.resize(2);
    rk2.players[0].score = 3;
    rk2.players[1].account = "Wn";
    rk2.players[1].observer = false;
    rk2.players[1].score = 1;
    w.SaveMatch(40, srcA, rk1, "{}");
    w.SaveMatch(41, srcA, rk2, "{}");
    w.SaveMatch(42, srcA, rk1, "{}");   // 겹친 판 — 점수 그대로여야
    auto a8 = WaitAnswers(w, 3);
    Check(a8.size() == 3 && a8[0].save == SaveResult::Saved && a8[1].save == SaveResult::Saved && a8[2].save == SaveResult::Duplicate,
          "⑧ 랭크 판 둘 Saved · 같은 판 다시 Duplicate");
    {   // ★ 매칭 손보기 1 — 답의 점수 바뀐 사람(커밋 뒤 다시 읽은 새 점수 · 판 사람 차례) · 겹친 판은 빔
        auto ch = [&](size_t i) {
            std::string s;
            if (i < a8.size())
                for (const auto& c : a8[i].changes) s += (s.empty() ? "" : ",") + c.account + ":" + std::to_string(c.after) + ":" + std::to_string(c.delta);
            return s;
        };
        Check(ch(0) == "wn:990:-10" && ch(1) == "wn:1000:10,Wn:990:-10" && ch(2).empty(),
              "★ ⑧ 답의 점수 바뀐 사람 — 첫 판 " + ch(0) + " · 둘째 판 " + ch(1) + " · 겹친 판 빔(" + ch(2) + ")");
    }
    const auto rt = ad.Rows("SELECT a.name, r.rating FROM ratings r JOIN accounts a ON a.id = r.account_id ORDER BY r.rating DESC, a.id");
    Check(rt.size() == 2 && rt[0][0] == "wn" && rt[0][1] == "1000" && rt[1][0] == "Wn" && rt[1][1] == "990",
          "⑧ ratings — wn 1000(−10 뒤 +10 · 겹친 판 안 셈) · Wn 990 · 계정 행 없는 ghost · 관전자 949 · 이름 없는 사람은 행 없음");
    const std::string mk1 = ad.One("SELECT id FROM matches WHERE match_key = '1760000000-3-40'");
    const auto d1 = ad.Rows("SELECT seat, IFNULL(rating_delta,'NULL') FROM match_players WHERE match_id = " + mk1 + " ORDER BY seat");
    Check(ad.One("SELECT ranked FROM matches WHERE id = " + mk1) == "1" && d1.size() == 4 && d1[0][1] == "-10" && d1[1][1] == "NULL" &&
              d1[2][1] == "NULL" && d1[3][1] == "NULL",
          "⑧ 랭크 판 행 — ranked 1 · rating_delta wn −10 · 관전 · 계정 없음(+10 이어도) · 이름 없음 NULL");
    w.GetRating(50, "wn");
    w.GetRating(51, "Wn");
    w.GetRating(52, std::string("\xb0\xc7", 2));   // 계정은 있으나 랭크 판 없음(관전만)
    w.GetRating(53, "nobody");
    auto a9 = WaitAnswers(w, 4);
    Check(a9.size() == 4 && a9[0].kind == StoreAnswerKind::Rating && a9[0].req == 50 && a9[0].ratingOk && a9[0].ratingFound && a9[0].rating == 1000 &&
              a9[0].rank == 1 && a9[1].ratingOk && a9[1].ratingFound && a9[1].rating == 990 && a9[1].rank == 2 && a9[2].ratingOk && !a9[2].ratingFound &&
              a9[2].rating == nox::kRatingStart && a9[2].rank == 0 && a9[3].ratingOk && !a9[3].ratingFound,
          "⑧ 점수 읽기 — wn 1000 · 1위 · Wn 990 · 2위 · 랭크 판 없는 계정 · 없는 이름 = 찾음 없음(1000 · 순위 0)");
    Check(ad.Run("RENAME TABLE ratings TO ratings_off"), "⑧ 시험용 — ratings 를 잠깐 치움");
    w.GetRating(54, "wn");
    auto a10 = WaitAnswers(w, 1);
    Check(a10.size() == 1 && a10[0].kind == StoreAnswerKind::Rating && !a10[0].ratingOk && !a10[0].why.empty(),
          "⑧ 점수 읽기 실패 → ratingOk false · 까닭 " + (a10.empty() ? std::string() : a10[0].why));
    Check(ad.Run("RENAME TABLE ratings_off TO ratings"), "⑧ ratings 되돌림");

    // ⑥ 끝낼 때 남은 일
    w.SaveMatch(30, srcA, Sample("1760000000-3-30"), "{}");
    w.SaveMatch(31, srcA, Sample("1760000000-3-31"), "{}");
    w.SaveMatch(32, srcA, Sample("1760000000-3-32"), "{}");
    w.Stop();
    std::vector<StoreAnswer> left;
    w.TakeAnswers(left);
    Check(left.size() == 3 && ad.One("SELECT COUNT(*) FROM matches") == "9", "⑥ Stop 직전에 넣은 판 셋 — 다 들어가고 답 셋(판 아홉)");

    // ⑦ 셈 — 요청 = 판 14(10 · 11 · 12 · 15 · 16 · 13 · 14 · 20 · 40 · 41 · 42 · 30 · 31 · 32) — ★ 로그인 암호 조각 4 로그인 걷음
    //   · 판 = Saved 9 + Duplicate 2 + Invalid 2 + 실패 1(CHECK — 끊김이 아니라 다시 안 함 · 다시는 ④ 의 1 뿐)
    //   · ★ 레이팅 조각 3 — 점수 읽기 5(50~54) = 읽음 4 + 실패 1 · 고친 점수 행 3(판 40 의 wn · 판 41 의 wn · Wn)
    const nox::db::DbStats s = w.Stats();
    Check(s.requests == 14 && s.saved == 9 && s.duplicates == 2 && s.invalid == 2 && s.saveFailed == 1 && s.retries == 1 &&
              s.ratingRequests == 5 && s.ratingReads == 4 && s.ratingFailed == 1 && s.ratingRows == 3 && ad.One("SELECT COUNT(*) FROM accounts") == "3",
          "⑦ 셈 — 요청 14 = Saved 9 · Duplicate 2 · Invalid 2 · 실패 1 · 다시 1 · 점수 읽기 5 = 읽음 4 + 실패 1 · 점수 행 3 · 로비가 계정을 안 만듦(셋 그대로)");
    std::printf("  ④ KILL 뒤 처음 걸린 오류: %s\n", s4.lastError.c_str());   // 끊김 번호가 무엇으로 오는지(`IsLost` 갈래 확인용)
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
