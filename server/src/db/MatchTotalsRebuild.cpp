// MatchTotalsRebuild.cpp — 승패 합계 표 다시 계산. 설명은 `MatchTotalsRebuild.h` 머리.
#include "nox/MatchTotalsRebuild.h"

#include "MatchRowBinds.h"
#include "MySqlBind.h"

#include <cstring>
#include <ctime>
#include <map>

namespace nox::db {

namespace {

// `kSelHistory` 와 같은 칸(`NOX_SQL_MATCH_ROW_COLUMNS`) · 관전 판 · 계정 없는 사람은 저장 때도 안 올린다(`DbWorker.cpp`). 맨 뒤 칸 = 계정 번호
constexpr const char* kSelAllRows =
    "SELECT " NOX_SQL_MATCH_ROW_COLUMNS ", p.account_id FROM match_players p JOIN matches m ON m.id = p.match_id "
    "WHERE p.account_id IS NOT NULL AND p.observer = 0";
// 칸 차례 = `BindMatchTotals`(저장의 `kUpsTotals` 와 같고 겹칠 때 더하지 않고 덮어쓴다)
constexpr const char* kSetTotals =
    "INSERT INTO match_totals (account_id, games, ranked_wins, ranked_losses, normal_wins, normal_losses, updated_unix) VALUES (?,?,?,?,?,?,?) "
    "ON DUPLICATE KEY UPDATE games = ?, ranked_wins = ?, ranked_losses = ?, normal_wins = ?, normal_losses = ?, updated_unix = ?";

constexpr unsigned kRebuildTimeoutSec = 300;   // 근거 없는 값 — 로비의 3초(`DbConfig`)는 판 하나 몫이라 전체 판 읽기엔 짧다. 잠금 대기(innodb_lock_wait_timeout 기본 50초)보다 길게

// 연결 · 문장 닫기를 한 곳에(실패 갈래가 많다)
struct Closer {
    MYSQL* m = nullptr;
    MYSQL_STMT* sel = nullptr;
    MYSQL_STMT* set = nullptr;
    ~Closer() {
        if (sel) mysql_stmt_close(sel);
        if (set) mysql_stmt_close(set);
        if (m) mysql_close(m);
    }
};

bool Prepare(MYSQL* m, MYSQL_STMT*& s, const char* sql, std::string& err) {
    s = mysql_stmt_init(m);
    if (!s) {
        err = std::string("문장 만들기 — ") + mysql_error(m);
        return false;
    }
    if (mysql_stmt_prepare(s, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        err = std::string("문장 준비 — ") + mysql_stmt_error(s);
        return false;
    }
    return true;
}

// 트랜잭션 안의 일 — 여기서 실패하면 부르는 쪽이 되돌린다
bool RebuildInTransaction(Closer& c, MatchTotalsRebuildResult& out, std::string& err) {
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    const std::string zero = "UPDATE match_totals SET games = 0, ranked_wins = 0, ranked_losses = 0, normal_wins = 0, normal_losses = 0, updated_unix = " + std::to_string(now);
    if (mysql_query(c.m, zero.c_str()) != 0) {
        err = std::string("0 으로 돌리기 — ") + mysql_error(c.m);
        return false;
    }
    out.existing = mysql_affected_rows(c.m);
    if (!Prepare(c.m, c.sel, kSelAllRows, err) || !Prepare(c.m, c.set, kSetTotals, err)) return false;

    MatchRowBinds row;
    uint64_t accountId = 0;
    MYSQL_BIND r[MatchRowBinds::kColumns + 1] = {};
    row.Bind(r);
    U64(r[MatchRowBinds::kColumns], accountId);
    std::map<uint64_t, wol::MatchTotals> totals;
    if (mysql_stmt_bind_result(c.sel, r) != 0 || mysql_stmt_execute(c.sel) != 0 || mysql_stmt_store_result(c.sel) != 0) {
        err = std::string("판 행 읽기 — ") + mysql_stmt_error(c.sel);
        return false;
    }
    int f = 0;
    while ((f = mysql_stmt_fetch(c.sel)) == 0) {
        totals[accountId].Add(row.Row());
        ++out.rows;
    }
    if (f != MYSQL_NO_DATA) {
        err = std::string("판 행 꺼내기 — ") + mysql_stmt_error(c.sel);
        return false;
    }
    mysql_stmt_free_result(c.sel);

    for (const auto& [id, t] : totals) {
        MYSQL_BIND p[kMatchTotalsParams] = {};
        BindMatchTotals(p, id, t, now);
        if (mysql_stmt_bind_param(c.set, p) != 0 || mysql_stmt_execute(c.set) != 0) {
            err = std::string("합계 쓰기 — ") + mysql_stmt_error(c.set);
            return false;
        }
        ++out.accounts;
    }
    return true;
}

}  // namespace

bool RebuildMatchTotals(const DbConfig& cfg, MatchTotalsRebuildResult& out, std::string& err) {
    out = MatchTotalsRebuildResult{};
    if (!LibInit()) {
        err = "mysql_library_init 실패";
        return false;
    }
    DbConfig dc = cfg;
    if (dc.rwTimeoutSec < kRebuildTimeoutSec) dc.rwTimeoutSec = kRebuildTimeoutSec;
    bool connectFailed = false;
    Closer c;
    c.m = OpenChecked(dc, err, connectFailed);   // 붙기 + 표 버전(`MySqlBind.h`)
    if (!c.m) return false;
    if (mysql_query(c.m, "START TRANSACTION") != 0) {
        err = std::string("트랜잭션 열기 — ") + mysql_error(c.m);
        return false;
    }
    if (!RebuildInTransaction(c, out, err)) {
        mysql_rollback(c.m);
        return false;
    }
    if (mysql_commit(c.m) != 0) {
        err = std::string("끝내기(commit) — ") + mysql_error(c.m);
        mysql_rollback(c.m);
        return false;
    }
    return true;
}

}  // namespace nox::db
