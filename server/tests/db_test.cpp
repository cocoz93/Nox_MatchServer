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
//   ⑩ ★ 11-05 플랜1(⑨ 뒤) — 계정 정지 저장소(`account_bans` — 가장 늦은 끝 시각 · 푼 행 · 이력 · 영구 · 한글 사유)
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nox/DbLobbyStore.h"   // ★ 11-04 전적 검색 — 쓰기 · 읽기 워커를 나누는 저장소(`CDbWorker` 는 그 안에)
#include "MatchHistoryScenario.h"
#include "nox/IniFile.h"
#include "nox/LoginLine.h"
#include "nox/MatchTotalsRebuild.h"   // ★ 11-04 전적 검색 — ⑫ 다시 계산
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
std::vector<nox::wol::StoreAnswer> WaitAnswers(nox::wol::ILobbyStore& w, size_t n) {
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
    a.hasKills = true;   // ★ 11-04 플랜2
    a.kills = 4294967295u;
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
    bool laid = ad.Run("DROP TABLE IF EXISTS account_bans, match_totals, login_keys, ratings, match_players, matches, accounts, schema_version");   // ★ 레이팅 조각 3 — ratings 가 accounts 를 가리켜 먼저 · ★ 런처 P1 — login_keys 도 · ★ 11-04 전적 검색 — match_totals 도
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string schema = ss.str();
        laid = laid && !schema.empty() && ad.Run(schema);
    }
    Check(laid, "표 새로 깔기(시험 DB · SQL 파일 " + std::to_string(argc - 2) + "개 그대로 차례로)");

    nox::db::CDbLobbyStore w(cfg);
    Check(w.Start(err), "워커 둘 뜸(붙기 · 표 버전 · 준비된 문장) " + err);

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
                            "IFNULL(deaths,'NULL'), observer, IFNULL(kills,'NULL') FROM match_players WHERE match_id = " + mid + " ORDER BY seat");

    const bool players = pr.size() == 4 && pr[0][0] == "0" && pr[0][1] == idWn && pr[0][2] == Hex("wn") && pr[0][3] == "1" && pr[0][4] == "finished" &&
                         pr[0][5] == "0" && pr[0][6] == "-1" && pr[0][7] == "4294967295" && pr[0][8] == "0" && pr[1][1] == id949 && pr[1][2] == "B0C7" &&
                         pr[1][8] == "1" && pr[2][1] == "NULL" && pr[2][2] == Hex("ghost") && pr[2][3] == "65535" && pr[2][5] == "2" &&
                         pr[2][6] == "2147483647" && pr[3][1] == "NULL" && pr[3][2] == "NULL" && pr[3][4] == "left" && pr[3][5] == "NULL" &&
                         pr[3][6] == "NULL" && pr[3][7] == "NULL" && pr[0][9] == "4294967295" && pr[1][9] == "NULL" && pr[3][9] == "NULL";
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
    const unsigned long cid = w.WriterConnectionId();
    Check(cid != 0 && ad.Run("KILL " + std::to_string(cid)), "④ 워커 연결을 KILL(같은 계정 — 제 연결은 권한 없이 끊음)");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    w.SaveMatch(20, srcA, Sample("1760000000-3-20"), "{}");   // ★ 로그인 암호 조각 4 — 로그인 대신 판으로(끊김 뒤 다시 붙기 · 계정 찾기도 지나감)
    auto a5 = WaitAnswers(w, 1);
    const nox::db::DbStats s4 = w.Stats();
    Check(a5.size() == 1 && a5[0].save == SaveResult::Saved && s4.retries == 1 && s4.connects == 3 && w.WriterConnectionId() != cid,
          "④ 끊긴 뒤 다음 요청 → 다시 붙어 성공(다시 1 · 붙음 3 = 처음 둘 + 다시 하나 · 새 연결 번호) " + s4.lastError);

    // ⑤
    Check(ad.Run("UPDATE schema_version SET version = 99 WHERE id = 1"), "⑤ 표 버전을 99 로");
    {
        nox::db::CDbLobbyStore w2(cfg);
        std::string e2;
        Check(!w2.Start(e2) && e2.find("표 버전 99") != std::string::npos, "⑤ 표 버전이 다르면 Start 실패(" + e2 + ")");
        nox::db::CDbWorker r2(cfg, nox::db::DbWorkerRole::Reader);   // ★ 11-04 전적 검색 — 읽기 연결도 따로 본다
        std::string e3;
        Check(!r2.Start(e3) && e3.find("표 버전 99") != std::string::npos, "⑤ 읽기 워커도 표 버전이 다르면 Start 실패(" + e3 + ")");
    }
    Check(ad.Run("UPDATE schema_version SET version = " + std::to_string(nox::db::kSchemaVersion) + " WHERE id = 1"), "⑤ 표 버전 되돌림");

    // ⑨ ★ 런처 P1 — 로그인 서버의 MySQL 열쇠 저장소(`004_launcher_keys.sql`). 시험 DB 를 로비 사용자로 붙는다
    //   (진짜 로그인 서버는 nox_auth 사용자 — 그 권한은 사람 1회 `config/launcher_setup.sql` · auth_smoke 가 본다)
    {
        using nox::auth::FindResult;
        nox::auth::CMySqlAccountStore st(cfg);
        std::string e9;
        const bool opened = st.Open(e9);
        Check(opened, "⑨ 열쇠 저장소 Open(준비된 문장 열다섯) " + e9);
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
        // ★ 11-05 플랜1 — 암호 바꾸기: 실제 트랜잭션. h3(PC3) 하나가 남아 있는 상태에서 시작
        {
            const uint64_t otherId = std::strtoull(ad.One("SELECT id FROM accounts WHERE name = 'Wn'").c_str(), nullptr, 10);
            nox::auth::LoginKeyHash other{}, h4{}, h5{};
            other.fill(9);
            h4.fill(4);
            h5.fill(5);
            nox::PasswordRecord rec1, rec2;
            rec1.salt.fill(0x11);
            rec1.hash.fill(0x22);
            rec1.iterations = 1000;
            rec2.salt.fill(0x33);
            rec2.hash.fill(0x44);
            rec2.iterations = 2000;
            const bool otherKey = st.InsertKey(otherId, other, "OTHER", 100, 2000) && st.InsertKey(wnId, h1, "PC1", 100, 2000) && st.InsertKey(wnId, h3, "PC3", 100, 2000) &&
                                  ad.One("SELECT COUNT(*) FROM login_keys WHERE account_id = " + std::to_string(wnId)) == "2";   // 앞 만료 지움에 h3 이 사라져 다시 넣는다
            // 바뀜 — wn 열쇠 둘(h3 · h1) 지워지고 h4 하나 · 남의 열쇠는 그대로 · 암호 칸 셋 · 새 열쇠의 값
            const bool changed = st.ChangePassword(wnId, rec1, h4, "PC4", 100, 3000);
            // 커밋됐나 — 다른 연결(ad)에서 곧바로. 같은 연결로 읽으면 커밋을 빼도 통과한다(다음 START TRANSACTION 이 앞 일을 커밋)
            const bool committed = ad.One("SELECT pass_iter FROM accounts WHERE id = " + std::to_string(wnId)) == "1000" &&
                                   ad.One("SELECT COUNT(*) FROM login_keys WHERE account_id = " + std::to_string(wnId)) == "1";
            nox::auth::AccountRow row;
            const bool saved = changed && st.Find("wn", row) == FindResult::Found && row.hasPassword && row.password.iterations == 1000 && row.password.salt == rec1.salt &&
                               row.password.hash == rec1.hash;
            const bool keys = ad.One("SELECT COUNT(*) FROM login_keys WHERE account_id = " + std::to_string(wnId)) == "1" && st.FindKey(h4, k) == FindResult::Found && k.accountId == wnId &&
                              k.ttlSec == 100 && k.expiresUnix == 3100 && st.FindKey(h1, k) == FindResult::NotFound && st.FindKey(h3, k) == FindResult::NotFound &&
                              st.FindKey(other, k) == FindResult::Found;
            // 되돌림 — 새 열쇠가 남의 지문과 겹쳐 넣기(셋째 문장)가 실패 → 암호 · 열쇠 그대로
            const bool rolledBack = !st.ChangePassword(wnId, rec2, other, "PC5", 100, 4000);
            nox::auth::AccountRow after;
            const bool kept = st.Find("wn", after) == FindResult::Found && after.password.iterations == 1000 && after.password.hash == rec1.hash &&
                              ad.One("SELECT pass_iter FROM accounts WHERE id = " + std::to_string(wnId)) == "1000" && st.FindKey(h4, k) == FindResult::Found &&
                              ad.One("SELECT COUNT(*) FROM login_keys WHERE account_id = " + std::to_string(wnId)) == "1" &&
                              ad.One("SELECT account_id FROM login_keys WHERE key_hash = UNHEX(REPEAT('09', 32))") == std::to_string(otherId);
            // 없는 계정 — 바뀌는 행 0 → 실패 · 아무것도 안 남음
            const bool noRow = !st.ChangePassword(99999999ull, rec2, h5, "PC5", 100, 4000) && st.LastError().find("계정 행 없음") != std::string::npos && st.FindKey(h5, k) == FindResult::NotFound;
            // 되돌린 뒤 연결이 그대로 자동 커밋으로 돎 — 다른 연결(ad)에서 바로 보임
            const bool afterTx = st.InsertKey(wnId, h5, "PC5", 100, 4000) && ad.One("SELECT device FROM login_keys WHERE key_hash = UNHEX(REPEAT('05', 32))") == "PC5";
            Check(otherKey && changed && committed && saved && keys && rolledBack && kept && noRow && afterTx,
                  "⑨ 암호 바꾸기 — 암호 저장 + 열쇠 전부 삭제 + 새 열쇠 · 남의 열쇠 그대로 · 셋째 문장 실패면 되돌림 · 없는 계정 · 되돌린 뒤 연결 멀쩡 (" + std::to_string(otherKey) +
                      std::to_string(changed) + std::to_string(committed) + std::to_string(saved) + std::to_string(keys) + std::to_string(rolledBack) + std::to_string(kept) + std::to_string(noRow) +
                      std::to_string(afterTx) + ") " + st.LastError());
            Check(ad.Run("UPDATE accounts SET pass_salt = NULL, pass_hash = NULL, pass_iter = NULL WHERE id = " + std::to_string(wnId)), "⑨ 암호 칸 치움");
        }
        Check(ad.Run("DELETE FROM login_keys"), "⑨ 열쇠 행 치움");

        // ⑩ ★ 11-05 플랜1 — 정지(가장 늦은 끝 시각 · 경계 · 푼 행 · 지난 행은 이력 그대로 · 영구 · 한글 사유 · 넣은 쪽 · 목록 · 없는 계정)
        {
            const std::string why = "\xEC\x9A\x95\xEC\x84\xA4 \xEB\xB0\x98\xEB\xB3\xB5";   // 「욕설 반복」 UTF-8 다섯 글자
            const uint64_t otherId = std::strtoull(ad.One("SELECT id FROM accounts WHERE name = 'Wn'").c_str(), nullptr, 10);
            nox::auth::BanRow b;
            const bool none = st.FindActiveBan(wnId, 1000, b) == FindResult::NotFound;
            const bool ins3 = st.InsertBan(wnId, 2000, "first", std::string(nox::kBanSourceAdmin), 1000) && st.InsertBan(wnId, 3000, why, std::string(nox::kBanSourceAuto), 1001) &&
                              st.InsertBan(wnId, 1500, "third", std::string(nox::kBanSourceAdmin), 1002);
            const bool latest = st.FindActiveBan(wnId, 1000, b) == FindResult::Found && b.untilUnix == 3000 && b.reason == why;
            const bool stored = ad.One("SELECT CHAR_LENGTH(reason) FROM account_bans WHERE until_unix = 3000") == "5" &&
                                ad.One("SELECT LENGTH(reason) FROM account_bans WHERE until_unix = 3000") == std::to_string(why.size()) &&
                                ad.One("SELECT source FROM account_bans WHERE until_unix = 3000") == "auto" &&
                                ad.One("SELECT created_unix FROM account_bans WHERE until_unix = 3000") == "1001" &&
                                ad.One("SELECT lifted_unix FROM account_bans WHERE until_unix = 3000") == "0";
            const bool edge = st.FindActiveBan(wnId, 2999, b) == FindResult::Found && st.FindActiveBan(wnId, 3000, b) == FindResult::NotFound;
            const bool others = st.FindActiveBan(otherId, 1000, b) == FindResult::NotFound;
            std::vector<nox::auth::BanListRow> list;
            const bool listAll = st.ListActiveBans(1000, list) && list.size() == 3 && list[0].name == "wn" && list[0].untilUnix == 2000 && list[0].source == "admin" &&
                                 list[0].createdUnix == 1000 && list[1].reason == why && list[1].source == "auto";
            const bool listCut = st.ListActiveBans(1600, list) && list.size() == 2 && st.ListActiveBans(3000, list) && list.empty();
            // 1700 에 풀기 — 1500 행은 이미 지나 이력 그대로(lifted 0) · 나머지 둘만 풀림
            unsigned lifted = 99;
            const bool lift = st.LiftBans(wnId, 1700, lifted) && lifted == 2 && st.FindActiveBan(wnId, 1800, b) == FindResult::NotFound &&
                              ad.One("SELECT COUNT(*) FROM account_bans WHERE lifted_unix = 1700") == "2" && ad.One("SELECT lifted_unix FROM account_bans WHERE until_unix = 1500") == "0" &&
                              ad.One("SELECT COUNT(*) FROM account_bans") == "3";
            const bool liftAgain = st.LiftBans(wnId, 1700, lifted) && lifted == 0 && st.ListActiveBans(1000, list) && list.size() == 1 && list[0].untilUnix == 1500;
            const bool forever = st.InsertBan(otherId, nox::kBanForever, "", std::string(nox::kBanSourceAdmin), 1000) && st.FindActiveBan(otherId, 4294967294u, b) == FindResult::Found &&
                                 b.untilUnix == nox::kBanForever && b.reason.empty();
            const bool noRow = !st.InsertBan(987654321ull, 2000, "x", std::string(nox::kBanSourceAdmin), 1000);
            const bool afterFail = st.LiftBans(otherId, 1000, lifted) && lifted == 1 && st.FindActiveBan(otherId, 1000, b) == FindResult::NotFound;
            Check(none && ins3 && latest && stored && edge && others && listAll && listCut && lift && liftAgain && forever && noRow && afterFail,
                  "⑩ 정지 — 가장 늦은 끝 시각 · 경계 · 다른 계정 · 한글 사유 · 넣은 쪽 · 목록 · 지난 행은 이력 그대로 · 영구 · 없는 계정 · 실패 뒤 연결 (" + std::to_string(none) + std::to_string(ins3) +
                      std::to_string(latest) + std::to_string(stored) + std::to_string(edge) + std::to_string(others) + std::to_string(listAll) + std::to_string(listCut) + std::to_string(lift) +
                      std::to_string(liftAgain) + std::to_string(forever) + std::to_string(noRow) + std::to_string(afterFail) + ") " + st.LastError());
            Check(ad.Run("DELETE FROM account_bans"), "⑩ 정지 행 치움");
        }
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
    rk2.winnerId = 1;   // ★ 11-07 — 1:1 Elo 는 이긴 사람을 승자 칸(netCode)으로 본다(견본의 2 는 관전자 자리)
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
              s.ratingRequests == 5 && s.ratingReads == 4 && s.ratingFailed == 1 && s.ratingRows == 3 && s.totalsRows == 10 && ad.One("SELECT COUNT(*) FROM accounts") == "3",
          "⑦ 셈 — 요청 14 = Saved 9 · Duplicate 2 · Invalid 2 · 실패 1 · 다시 1 · 점수 읽기 5 = 읽음 4 + 실패 1 · 점수 행 3 · 합계 행 10(판 9 의 wn + 판 41 의 Wn) · 로비가 계정을 안 만듦(셋 그대로)");
    // ⑩ ★ 11-04 플랜2 — 대전 기록 읽기. lobby_test 메모리 가짜와 같은 판 묶음 · 같은 기대 줄(`MatchHistoryScenario.h`)
    {
        Check(ad.Run("INSERT INTO accounts (name, created_unix, last_login_unix) VALUES ('h1', 1, 1), ('h2', 1, 1), ('h3', 1, 1), ('h4', 1, 1)"), "⑩ 계정 넷");
        Check(ad.Run("DELETE FROM ratings"), "⑩ 앞 시험의 점수 행 치움 — 순위가 메모리 가짜의 기대 줄과 같은 사람들로만 나오게");
        nox::db::CDbLobbyStore hw(cfg);
        std::string herr;
        Check(hw.Start(herr), "⑩ 워커 — " + herr);
        uint32_t req = 100;
        const auto scenario = nox::test::HistoryScenario();
        for (const nox::MatchRecord& m : scenario) hw.SaveMatch(req++, srcA, m, "{}");
        auto saved = WaitAnswers(hw, scenario.size());
        size_t savedOk = 0;
        for (const StoreAnswer& a : saved) savedOk += a.save == SaveResult::Saved ? 1 : 0;
        Check(savedOk == scenario.size(), "⑩ 판 묶음 저장 " + std::to_string(savedOk));
        {   // ★ 11-07 점수 묶기 조각 4 — 판 전 점수(행 없음 1000 · 있음 그 값) · 같은 둘의 판 연달아(앞 판 결과로 셈) · 칸 = 저장 때 셈한 값
            //   판 1 = 둘 다 행 없음(1000 · 1000) → ±10 · 판 2 = 990 · 1010 → h1 +11 · 판 7 = 1001 · 999 → h2 +10
            const auto dOf = [&](const char* key) {
                std::string s;
                for (const auto& row : ad.Rows(std::string("SELECT IFNULL(p.rating_delta,'NULL') FROM match_players p JOIN matches m ON m.id = p.match_id WHERE m.match_key = '") +
                                               key + "' ORDER BY p.seat"))
                    s += (s.empty() ? "" : " ") + row[0];
                return s;
            };
            const auto hr = ad.Rows("SELECT a.name, r.rating FROM ratings r JOIN accounts a ON a.id = r.account_id WHERE a.name IN ('h1', 'h2') ORDER BY a.name");
            Check(dOf("h-1") == "-10 10" && dOf("h-2") == "11 -11" && dOf("h-late") == "-10 10" && hr.size() == 2 && hr[0][1] == "991" && hr[1][1] == "1009",
                  "★ ⑩ 1:1 판 전 점수 — 판 1 " + dOf("h-1") + " · 판 2 " + dOf("h-2") + " · 판 7 " + dOf("h-late") + " · 점수 " +
                      (hr.size() == 2 ? hr[0][0] + " " + hr[0][1] + " · " + hr[1][0] + " " + hr[1][1] : std::string("?")));
        }
        const std::vector<std::pair<std::string, std::vector<std::string>>> who = {{"h1", nox::test::HistoryExpectH1()},
                                                                                   {"h2", nox::test::HistoryExpectH2()},
                                                                                   {"h3", nox::test::HistoryExpectH3()},
                                                                                   {"nobody", nox::test::HistoryExpectNobody()}};
        for (const auto& w2 : who) {
            hw.GetMatchHistory(req, w2.first);
            auto got = WaitAnswers(hw, 1);
            bool same = got.size() == 1 && got[0].kind == StoreAnswerKind::History && got[0].req == req && got[0].historyOk;
            std::string shown;
            if (same) {
                const auto ls = nox::wol::MatchHistoryLines(w2.first, got[0].ratingFound, got[0].rating, got[0].rank, got[0].history);
                same = ls == w2.second;
                for (const auto& l : ls) shown += l + " | ";
            }
            Check(same, "⑩ " + w2.first + " 줄 — " + shown);
            ++req;
        }
        // 못 읽음 → historyOk false · 칸을 되돌리면 같은 연결로 다시 읽음
        Check(ad.Run("RENAME TABLE match_players TO match_players_off"), "⑩ 시험용 — match_players 를 잠깐 치움");
        hw.GetMatchHistory(req++, "h1");
        auto failed = WaitAnswers(hw, 1);
        Check(failed.size() == 1 && failed[0].kind == StoreAnswerKind::History && !failed[0].historyOk && !failed[0].why.empty() && failed[0].history.rows.empty(),
              "⑩ 읽기 실패 → historyOk false · 까닭 " + (failed.empty() ? std::string() : failed[0].why));
        Check(ad.Run("RENAME TABLE match_players_off TO match_players"), "⑩ match_players 되돌림");
        hw.GetMatchHistory(req++, "h1");
        auto again = WaitAnswers(hw, 1);
        Check(again.size() == 1 && again[0].historyOk && again[0].history.totals.games == 6 && again[0].history.rows.size() == 6, "⑩ 되돌린 뒤 다시 읽음 — 판 6 · 줄 6");

        // ⑪ ★ 11-04 전적 검색 — 합계 표. 저장 때 올린 값 · 같은 판 다시 · 저장 실패 때 되돌림 · 합계를 못 올리면 판도 없음 · 합계 행 없는 계정
        auto totalsOf = [&](const char* name) {
            const auto rows = ad.Rows(std::string("SELECT t.games, t.ranked_wins, t.ranked_losses, t.normal_wins, t.normal_losses FROM match_totals t JOIN accounts a ON a.id = t.account_id WHERE a.name = '") + name + "'");
            std::string out;
            if (!rows.empty())
                for (const std::string& v : rows[0]) out += (out.empty() ? "" : " ") + v;
            return out;
        };
        // 판 / 랭크 승 · 패 / 일반 승 · 패 — h1 판 6(관전 판 6 뺌) · h3 = 22승 + 판 3 의 2등 · h4 = 관전 판 3 뺌 + 판 6 이김
        Check(totalsOf("h1") == "6 1 2 1 1" && totalsOf("h2") == "6 2 1 1 1" && totalsOf("h3") == "23 0 0 22 1" && totalsOf("h4") == "1 0 0 1 0",
              "⑪ 합계 — h1 " + totalsOf("h1") + " · h2 " + totalsOf("h2") + " · h3 " + totalsOf("h3") + " · h4 " + totalsOf("h4"));
        hw.SaveMatch(req++, srcA, scenario.back(), "{}");
        auto dupT = WaitAnswers(hw, 1);
        Check(dupT.size() == 1 && dupT[0].save == SaveResult::Duplicate && totalsOf("h1") == "6 1 2 1 1" && totalsOf("h2") == "6 2 1 1 1", "⑪ 같은 판 다시 → Duplicate · 합계 그대로");
        const std::string matchesBefore = ad.One("SELECT COUNT(*) FROM matches");
        Check(ad.Run("ALTER TABLE match_players ADD CONSTRAINT ck_db_test CHECK (net_code <> 4242)"), "⑪ 시험용 CHECK 걸기");
        nox::MatchRecord halfT = scenario.back();
        halfT.match = "h-half";
        halfT.players[1].netCode = 4242;
        hw.SaveMatch(req++, srcA, halfT, "{}");
        auto failT = WaitAnswers(hw, 1);
        Check(failT.size() == 1 && failT[0].save == SaveResult::Failed && totalsOf("h1") == "6 1 2 1 1" && ad.One("SELECT COUNT(*) FROM matches") == matchesBefore,
              "⑪ 둘째 사람 넣기 실패 → Failed · 첫 사람 합계 되돌아감 · 판 행 없음");
        Check(ad.Run("ALTER TABLE match_players DROP CHECK ck_db_test"), "⑪ 시험용 CHECK 풀기");
        Check(ad.Run("RENAME TABLE match_totals TO match_totals_off"), "⑪ 시험용 — match_totals 를 잠깐 치움");
        nox::MatchRecord noTotals = scenario.back();
        noTotals.match = "h-no-totals";
        hw.SaveMatch(req++, srcA, noTotals, "{}");
        auto failN = WaitAnswers(hw, 1);
        Check(failN.size() == 1 && failN[0].save == SaveResult::Failed && ad.One("SELECT COUNT(*) FROM matches") == matchesBefore, "⑪ 합계를 못 올림 → Failed · 판 행 없음(점수와 같은 트랜잭션)");
        hw.GetMatchHistory(req++, "h1");
        auto failH = WaitAnswers(hw, 1);
        Check(failH.size() == 1 && !failH[0].historyOk && failH[0].history.rows.empty(), "⑪ 합계를 못 읽음 → historyOk false");
        Check(ad.Run("RENAME TABLE match_totals_off TO match_totals"), "⑪ match_totals 되돌림");
        Check(ad.Run("INSERT INTO accounts (name, created_unix, last_login_unix) VALUES ('h5', 1, 1)"), "⑪ 판 없는 계정");
        hw.GetMatchHistory(req++, "h5");
        auto noRow = WaitAnswers(hw, 1);
        for (const nox::test::HistoryAccountCase& c2 : nox::test::HistoryAccountCases()) {   // ★ 전적 검색 — 계정 유무 · lobby_test 메모리 가짜와 같은 표
            hw.GetMatchHistory(req++, c2.name);
            auto acc = WaitAnswers(hw, 1);
            Check(acc.size() == 1 && acc[0].historyOk && acc[0].accountFound == c2.found, std::string("⑪ 계정 유무 ") + c2.name);
        }
        Check(noRow.size() == 1 && noRow[0].historyOk && noRow[0].history.totals == MatchTotals{} && noRow[0].history.rows.empty() && totalsOf("h5").empty(), "⑪ 합계 행 없는 계정 — 전부 0 · 줄 없음");
        hw.Stop();
        const nox::db::DbStats hs = hw.Stats();
        Check(hs.historyRequests == 15 && hs.historyReads == 13 && hs.historyFailed == 2, "⑩ 셈 — 읽기 요청 15 = 읽음 13 + 실패 2");

        // ⑫ ★ 11-04 전적 검색 — 다시 계산(`MatchTotalsRebuild.h`). 판정 입력이 저장(`MatchRowOf`)과 읽기(`MatchRowBinds`) 두 길이라 값이 같아야 한다
        auto totalsTable = [&] {   // 0 행은 행 없음으로 친다 — 다시 계산은 행을 안 지운다
            return ad.Rows("SELECT account_id, games, ranked_wins, ranked_losses, normal_wins, normal_losses FROM match_totals WHERE games > 0 ORDER BY account_id");
        };
        const auto raised = totalsTable();
        nox::db::MatchTotalsRebuildResult rb;
        std::string rbErr;
        Check(nox::db::RebuildMatchTotals(cfg, rb, rbErr), "⑫ 다시 계산 — " + rbErr);
        Check(raised.size() >= 5 && totalsTable() == raised && rb.accounts == raised.size() && rb.existing == raised.size(),
              "⑫ 올린 합계 = 다시 계산한 합계(계정 " + std::to_string(raised.size()) + " · 있던 행 " + std::to_string(rb.existing) + " · 쓴 계정 " + std::to_string(rb.accounts) + " · 판 줄 " + std::to_string(rb.rows) + ")");
        // 어긋난 표를 고친다 — 값 틀림 · 행 없음 · 판 없는 계정의 행
        Check(ad.Run("UPDATE match_totals t JOIN accounts a ON a.id = t.account_id SET t.games = t.games + 5, t.ranked_wins = 99 WHERE a.name = 'h1'") &&
              ad.Run("DELETE t FROM match_totals t JOIN accounts a ON a.id = t.account_id WHERE a.name = 'h2'") &&
              ad.Run("INSERT INTO match_totals SELECT id, 7, 7, 0, 0, 0, 1 FROM accounts WHERE name = 'h5'"), "⑫ 시험용 — 표를 어긋나게");
        Check(totalsTable() != raised, "⑫ 어긋난 표는 다르게 보인다");
        Check(nox::db::RebuildMatchTotals(cfg, rb, rbErr) && totalsTable() == raised && totalsOf("h5") == "0 0 0 0 0", "⑫ 값 틀림 · 행 없음 · 판 없는 계정의 행을 고침(h5 " + totalsOf("h5") + ")");
        Check(nox::db::RebuildMatchTotals(cfg, rb, rbErr) && totalsTable() == raised, "⑫ 두 번 돌려도 같다");
        // 실패하면 되돌아간다 — 0 으로 돌린 뒤 판 행 읽기에서 실패
        Check(ad.Run("RENAME TABLE match_players TO match_players_off"), "⑫ 시험용 — match_players 를 잠깐 치움");
        Check(!nox::db::RebuildMatchTotals(cfg, rb, rbErr) && !rbErr.empty() && totalsTable() == raised, "⑫ 판 행을 못 읽음 → 실패 · 합계 그대로(" + rbErr + ")");
        Check(ad.Run("RENAME TABLE match_players_off TO match_players"), "⑫ match_players 되돌림");

        // ⑬ ★ 11-04 전적 검색 — 쓰기 · 읽기 연결 나누기(`DbLobbyStore.h`). 일부러 잠가 멈춘 동안 무엇이 오나를 본다(우연에 안 기댐).
        //   잠금은 3초(워커 읽기 한도 `rwTimeoutSec`)보다 짧게 쥔다
        nox::db::CDbLobbyStore ss(cfg);
        std::string e13;
        Check(ss.Start(e13) && ss.Stats().connects == 2 && ss.WriterConnectionId() != 0 && ss.ReaderConnectionId() != 0 && ss.WriterConnectionId() != ss.ReaderConnectionId(),
              "⑬ 뜰 때 연결 둘 " + e13);
        auto countOf = [](const std::vector<StoreAnswer>& v, StoreAnswerKind k) {
            size_t n = 0;
            for (const StoreAnswer& x : v) n += x.kind == k ? 1 : 0;
            return n;
        };
        auto gather = [&](std::vector<StoreAnswer>& v, int ms, bool stopOnAny) {   // `ms` 동안 답을 모은다
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until && !(stopOnAny && !v.empty())) {
                ss.TakeAnswers(v);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        // 1 — 기록 읽기가 멈춘 동안 저장 답이 온다(일반 판은 `ratings` 를 안 고쳐 이 잠금에 안 걸린다 · 나누기 전 측정 = 저장 답 0)
        nox::MatchRecord splitNormal = scenario.back();
        splitNormal.ranked = false;
        splitNormal.match = "split-1";
        Check(ad.Run("LOCK TABLES ratings WRITE"), "⑬ ratings 잠금");
        ss.GetMatchHistory(req++, "h1");
        ss.SaveMatch(req++, srcA, splitNormal, "{}");
        std::vector<StoreAnswer> during;
        gather(during, 1500, true);
        Check(countOf(during, StoreAnswerKind::SaveMatch) == 1 && countOf(during, StoreAnswerKind::History) == 0, "⑬ 읽기가 멈춘 동안 저장 답 옴 · 기록 답 없음(받은 답 " + std::to_string(during.size()) + ")");
        Check(ad.Run("UNLOCK TABLES"), "⑬ 잠금 풀기");
        auto afterUnlock = WaitAnswers(ss, 1);
        Check(afterUnlock.size() == 1 && afterUnlock[0].kind == StoreAnswerKind::History && afterUnlock[0].historyOk, "⑬ 풀리면 기록 답 옴");
        // 2 — 읽기 연결을 끊어도 저장은 안 멈추고 읽기는 다시 붙어 답한다
        const unsigned long rid13 = ss.ReaderConnectionId(), wid13 = ss.WriterConnectionId();
        Check(ad.Run("KILL " + std::to_string(rid13)), "⑬ 읽기 연결 KILL");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        nox::MatchRecord splitNormal2 = splitNormal;
        splitNormal2.match = "split-2";
        ss.SaveMatch(req++, srcA, splitNormal2, "{}");
        ss.GetMatchHistory(req++, "h1");
        auto afterKill = WaitAnswers(ss, 2);
        const nox::db::DbStats sk = ss.Stats();
        Check(countOf(afterKill, StoreAnswerKind::SaveMatch) == 1 && countOf(afterKill, StoreAnswerKind::History) == 1 && sk.retries == 1 && sk.connects == 3 &&
                  ss.ReaderConnectionId() != rid13 && ss.WriterConnectionId() == wid13,
              "⑬ 읽기 연결 KILL → 저장 · 읽기 둘 다 답 · 읽기만 다시 붙음(다시 " + std::to_string(sk.retries) + " · 붙음 " + std::to_string(sk.connects) + ")");
        for (const StoreAnswer& x : afterKill)
            if (x.kind == StoreAnswerKind::SaveMatch) Check(x.save == SaveResult::Saved, "⑬ 그동안의 저장 Saved");
            else Check(x.historyOk, "⑬ 다시 붙어 읽음");
        // 3 — 같은 시점. 읽기를 줄 읽기 앞에서 세워 두고 그 사이 새 줄을 넣는다 → 답에 없어야 한다 · 다음 읽기엔 보인다(대조)
        //   `LOCK TABLES` 중엔 잠근 표만 읽을 수 있어 번호를 먼저 읽는다. 부모 표(`matches` · `accounts`)는 읽기로 함께 잠겨 점수 · 합계 읽기는 지나간다
        const std::string splitId = ad.One("SELECT id FROM matches WHERE match_key = 'split-1'"), h1Id = ad.One("SELECT id FROM accounts WHERE name = 'h1'");
        ss.GetMatchHistory(req++, "h1");
        auto snapBefore = WaitAnswers(ss, 1);
        const size_t rowsBefore = snapBefore.empty() ? 0 : snapBefore[0].history.rows.size();
        const std::string readerState = "SELECT IFNULL(STATE, '') FROM information_schema.PROCESSLIST WHERE ID = " + std::to_string(ss.ReaderConnectionId());
        Check(ad.Run("LOCK TABLES match_players WRITE"), "⑬ match_players 잠금");
        ss.GetMatchHistory(req++, "h1");
        bool readerWaits = false;
        for (int i = 0; i < 200 && !readerWaits; ++i) {
            readerWaits = ad.One(readerState).find("metadata lock") != std::string::npos;
            if (!readerWaits) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(readerWaits, "⑬ 읽기가 줄 읽기 앞에서 멈춤(`Waiting for table metadata lock`)");
        Check(ad.Run("INSERT INTO match_players (match_id, seat, account_id, name, net_code, state, team, score, deaths, observer, rating_delta, kills) VALUES (" + splitId + ", 9, " + h1Id + ", 'h1', 9, 'finished', 0, 0, 0, 0, NULL, 0)"),
              "⑬ 그 사이 새 줄 넣기");
        Check(ad.Run("UNLOCK TABLES"), "⑬ 잠금 풀기");
        auto snapShot = WaitAnswers(ss, 1);
        Check(snapShot.size() == 1 && snapShot[0].historyOk && snapShot[0].history.rows.size() == rowsBefore, "⑬ 시작한 뒤 넣은 줄은 안 보임(한 시점 · 줄 " + std::to_string(snapShot.empty() ? 0 : snapShot[0].history.rows.size()) + " · 전 " + std::to_string(rowsBefore) + ")");
        ss.GetMatchHistory(req++, "h1");
        auto snapAfter = WaitAnswers(ss, 1);
        Check(snapAfter.size() == 1 && snapAfter[0].history.rows.size() == rowsBefore + 1, "⑬ 다음 읽기엔 보임(늘 안 보이는 것이 아님)");
        // 4 — 순서. 저장을 세워 두면 뒤에 넣은 점수 읽기도 기다리고, 풀리면 저장 답 뒤에 새 점수가 온다
        const int h2Before = std::atoi(ad.One("SELECT r.rating FROM ratings r JOIN accounts a ON a.id = r.account_id WHERE a.name = 'h2'").c_str());
        // ★ 11-07 점수 묶기 조각 4 — 1:1 Elo 라 증감이 판 전 점수에 달림(±10 고정 아님) — 기대값은 지금 두 점수로 셈한 값
        const int h1Before = std::atoi(ad.One("SELECT r.rating FROM ratings r JOIN accounts a ON a.id = r.account_id WHERE a.name = 'h1'").c_str());
        nox::MatchRecord splitRanked = scenario.back();
        splitRanked.match = "split-ranked";
        Check(splitRanked.ranked && ad.Run("LOCK TABLES matches WRITE"), "⑬ matches 잠금");
        ss.SaveMatch(req++, srcA, splitRanked, "{}");
        ss.GetRating(req++, "h2");
        std::vector<StoreAnswer> heldOrder;
        gather(heldOrder, 500, false);
        Check(heldOrder.empty(), "⑬ 저장이 멈춘 동안 뒤의 점수 읽기도 답 없음(받은 답 " + std::to_string(heldOrder.size()) + ")");
        Check(ad.Run("UNLOCK TABLES"), "⑬ 잠금 풀기");
        auto ordered = WaitAnswers(ss, 2);
        const int h2Delta = nox::RatingDeltas(splitRanked, {h1Before, h2Before}).seats[1].delta;
        Check(ordered.size() == 2 && ordered[0].kind == StoreAnswerKind::SaveMatch && ordered[0].save == SaveResult::Saved && ordered[1].kind == StoreAnswerKind::Rating && ordered[1].ratingOk && ordered[1].rating == h2Before + h2Delta,
              "⑬ 풀리면 저장 답 뒤 새 점수(전 " + std::to_string(h2Before) + " · 증감 " + std::to_string(h2Delta) + " · 뒤 " + std::to_string(ordered.size() == 2 ? ordered[1].rating : 0) + ")");
        // 5 — 끝낼 때 — 읽기는 하던 것만 하고 남은 조회를 버린다 · 쓰기는 남은 판을 다 한다
        const nox::db::DbStats beforeStop = ss.Stats();
        Check(ad.Run("LOCK TABLES ratings WRITE, matches WRITE"), "⑬ 두 표 잠금");
        for (int i = 0; i < 3; ++i) ss.GetMatchHistory(req++, "h1");
        for (int i = 0; i < 3; ++i) {
            nox::MatchRecord left3 = splitNormal;
            left3.match = "split-stop-" + std::to_string(i);
            ss.SaveMatch(req++, srcA, left3, "{}");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::thread stopper([&ss] { ss.Stop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        Check(ad.Run("UNLOCK TABLES"), "⑬ 잠금 풀기");
        stopper.join();
        std::vector<StoreAnswer> leftovers;
        ss.TakeAnswers(leftovers);
        const nox::db::DbStats ds = ss.Stats();
        Check(ds.saved == beforeStop.saved + 3 && countOf(leftovers, StoreAnswerKind::SaveMatch) == 3 && ad.One("SELECT COUNT(*) FROM matches WHERE match_key LIKE 'split-stop-%'") == "3", "⑬ 끝낼 때 남은 판 셋 다 저장");
        Check(ds.historyDropped == 2 && ds.historyReads == beforeStop.historyReads + 1 && countOf(leftovers, StoreAnswerKind::History) == 1 && ds.historyRequests == ds.historyReads + ds.historyFailed + ds.historyDropped,
              "⑬ 끝낼 때 조회 — 하던 것 1 · 버림 2 · 요청 = 읽음 + 실패 + 버림(버림 " + std::to_string(ds.historyDropped) + ")");
    }
    {
        // ⑭ ★ 11-05 — 쓰기 · 읽기 워커 모두 답을 넣은 **뒤** 깨운다(`SetWake`). 깨우기 안에서 곧바로 꺼내 본다 —
        //   넣기 전에 깨우면 읽기 워커의 답이 남아 꺼낸 수가 모자라고, 잠금을 쥔 채 깨우면 여기서 막힌다
        nox::db::CDbLobbyStore ws(cfg);
        std::string e14;
        Check(ws.Start(e14), "⑭ 워커 둘 뜸 " + e14);
        std::atomic<int> woke{0}, seen{0};
        ws.SetWake([&ws, &woke, &seen] {
            std::vector<StoreAnswer> peek;
            ws.TakeAnswers(peek);
            seen += static_cast<int>(peek.size());
            ++woke;
        });
        auto waitWoke = [&woke](int n) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (woke.load() < n && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        ws.GetRating(1, "wn");
        ws.SaveMatch(2, srcA, Sample("wake-1"), "{}");
        waitWoke(2);   // 쓰기 워커 둘을 먼저 — 읽기 워커의 깨우기가 쓰기 쪽 답을 대신 꺼내 주지 못하게
        const int wokeWriter = woke.load();
        ws.GetMatchHistory(3, "wn");
        waitWoke(3);
        ws.Stop();
        Check(wokeWriter == 2 && woke.load() == 3 && seen.load() == 3,
              "⑭ 답 3개 — 쓰기 워커 깨움 " + std::to_string(wokeWriter) + " · 합 " + std::to_string(woke.load()) + " · 깨울 때 꺼낸 답 " + std::to_string(seen.load()));
    }
    {
        // ⑮ ★ 11-05 플랜1 — 활성 정지 읽기(`CDbWorker::ExecBans`). 실제 SELECT — 지난 행 · 푼 행 제외 · 여러 행이면 늦은 끝 · 이름 바이트 그대로 · 「0건」과 「못 읽음」을 가름
        nox::db::CDbLobbyStore bs(cfg);
        std::string e15;
        Check(bs.Start(e15), "⑮ 워커 둘 뜸 " + e15);
        const uint32_t now15 = 2000000000u;
        auto ask = [&](uint32_t req) {
            bs.GetActiveBans(req, now15);
            return WaitAnswers(bs, 1);
        };
        Check(ad.Run("DELETE FROM account_bans"), "⑮ 정지 행 비움");
        const auto a0 = ask(1);
        Check(a0.size() == 1 && a0[0].kind == StoreAnswerKind::ActiveBans && a0[0].req == 1 && a0[0].bansOk && a0[0].bans.empty(), "⑮ 0건 = 읽음 · 행 0");
        const std::string idUpper = ad.One("SELECT id FROM accounts WHERE name = 'Wn'");
        auto row = [](const std::string& id, unsigned long long until, unsigned lifted) {
            return "(" + id + "," + std::to_string(until) + ",'r','admin',1," + std::to_string(lifted) + ")";
        };
        Check(ad.Run("INSERT INTO account_bans (account_id, until_unix, reason, source, created_unix, lifted_unix) VALUES " +
                     row(idWn, now15 + 100ull, 0) + "," + row(idWn, now15 + 500ull, 0) + "," + row(idWn, now15 + 9000ull, 5) + "," +   // 푼 행은 늦어도 안 셈
                     row(idUpper, now15, 0) + "," +                                                                                    // 끝이 지금 = 끝남(경계)
                     row(id949, 4294967295ull, 0)),
              "⑮ 정지 행 다섯 깔기(wn 셋 · Wn 하나 · 949 이름 영구)");
        const auto a1 = ask(2);
        std::map<std::string, uint32_t> got;
        if (a1.size() == 1)
            for (const BanEntry& b : a1[0].bans) got[Hex(b.name)] = b.until;
        Check(a1.size() == 1 && a1[0].bansOk && a1[0].bans.size() == 2 && got[Hex("wn")] == now15 + 500 && got["B0C7"] == 4294967295u && got.count(Hex("Wn")) == 0,
              "⑮ wn = 늦은 끝(푼 행 제외) · Wn = 끝남 제외 · 949 이름 바이트 · 영구 · 행 " + std::to_string(a1.empty() ? 0 : a1[0].bans.size()));
        // 못 읽음 — 테이블을 잠시 치우면 문장이 실패한다(0건이 아님)
        Check(ad.Run("RENAME TABLE account_bans TO account_bans_gone"), "⑮ 테이블 치움");
        const auto a2 = ask(3);
        Check(ad.Run("RENAME TABLE account_bans_gone TO account_bans"), "⑮ 테이블 돌려놓음");
        Check(a2.size() == 1 && a2[0].kind == StoreAnswerKind::ActiveBans && !a2[0].bansOk && a2[0].bans.empty() && !a2[0].why.empty(), "⑮ 못 읽음 = bansOk false · 까닭 있음");
        const auto a3b = ask(4);
        Check(a3b.size() == 1 && a3b[0].bansOk && a3b[0].bans.size() == 2, "⑮ 돌려놓으면 다음 읽기는 됨");
        bs.Stop();
        const nox::db::DbStats ds15 = bs.Stats();
        Check(ds15.bansRequests == 4 && ds15.bansReads == 3 && ds15.bansFailed == 1 && ds15.bansDropped == 0, "⑮ 셈 — 요청 4 = 읽음 3 + 실패 1 + 버림 0");

        // ⑯ ★ 11-05 플랜1 — 줄 서기 10회째 자동 정지(`CDbWorker::ExecBanWrite`). 실제 INSERT 바이트 · 같은 테이블을 `GetActiveBans` 가 읽는가 · 없는 이름 · 못 넣음
        nox::db::CDbLobbyStore bw(cfg);
        std::string e16;
        Check(bw.Start(e16), "⑯ 워커 둘 뜸 " + e16);
        const uint32_t now16 = 2000000000u;
        const uint32_t until16 = now16 + 86400u;
        const std::string reason16 = "랭크 매칭 거절·이탈 반복";   // 플랜 9절 문구 그대로(상수를 안 써서 상수가 틀리면 lobby_test 와 어긋난다)
        Check(ad.Run("DELETE FROM account_bans"), "⑯ 정지 행 비움");
        bw.BanAccount("wn", until16, reason16, now16);
        const auto w1 = WaitAnswers(bw, 1);
        Check(w1.size() == 1 && w1[0].kind == StoreAnswerKind::BanWrite && w1[0].banWritten, "⑯ 넣음 — 답 banWritten");
        const std::string q16 = "SELECT CONCAT_WS('|', account_id, until_unix, HEX(reason), source, created_unix, lifted_unix) FROM account_bans";
        Check(ad.One(q16) == idWn + "|" + std::to_string(until16) + "|" + Hex(reason16) + "|auto|" + std::to_string(now16) + "|0", "⑯ 행 바이트 — 계정 · 끝 · 사유 · 출처 auto · 넣은 때 · 안 풀림");
        bw.GetActiveBans(1, now16);
        const auto r16 = WaitAnswers(bw, 1);
        Check(r16.size() == 1 && r16[0].bansOk && r16[0].bans.size() == 1 && r16[0].bans[0].name == "wn" && r16[0].bans[0].until == until16, "⑯ 활성 정지 읽기에 나옴");
        bw.BanAccount("no_such_account", until16, reason16, now16);
        const auto w2 = WaitAnswers(bw, 1);
        Check(w2.size() == 1 && !w2[0].banWritten && !w2[0].why.empty() && ad.One("SELECT COUNT(*) FROM account_bans") == "1", "⑯ 없는 이름 — 못 넣음 · 행 그대로");
        Check(ad.Run("RENAME TABLE account_bans TO account_bans_gone"), "⑯ 테이블 치움");
        bw.BanAccount("wn", until16, reason16, now16);
        const auto w3 = WaitAnswers(bw, 1);
        Check(ad.Run("RENAME TABLE account_bans_gone TO account_bans"), "⑯ 테이블 돌려놓음");
        Check(w3.size() == 1 && !w3[0].banWritten && !w3[0].why.empty(), "⑯ 쓰기 실패 — 못 넣음 · 까닭");
        bw.BanAccount("wn", until16 + 1, reason16, now16);
        const auto w4 = WaitAnswers(bw, 1);
        Check(w4.size() == 1 && w4[0].banWritten && ad.One("SELECT COUNT(*) FROM account_bans") == "2", "⑯ 돌려놓으면 다음 쓰기는 됨");
        bw.Stop();
        const nox::db::DbStats ds16 = bw.Stats();
        Check(ds16.banWriteRequests == 4 && ds16.banWrites == 2 && ds16.banWriteNoAccount == 1 && ds16.banWriteFailed == 1, "⑯ 셈 — 요청 4 = 넣음 2 + 계정 없음 1 + 실패 1");
    }
    std::printf("  ④ KILL 뒤 처음 걸린 오류: %s\n", s4.lastError.c_str());   // 끊김 번호가 무엇으로 오는지(`IsLost` 갈래 확인용)
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
