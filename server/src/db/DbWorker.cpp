// DbWorker.cpp — 로비의 DB 워커. 설명은 `DbWorker.h` 머리 · 표는 `tools/db/001_init.sql`.
#include "nox/DbWorker.h"

#ifdef _WIN32
#include <winsock2.h>   // mysql.h 앞에 — 구버전 winsock 자동 include 충돌 차단(MMO_Zone DBWorker.cpp 와 같은 꼴)
#endif
#include <errmsg.h>         // CR_SERVER_GONE_ERROR · CR_SERVER_LOST(끊김)
#include <mysql.h>
#include <mysqld_error.h>   // ER_DUP_ENTRY(열쇠 겹침 — 같은 판 · 같은 이름)

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

namespace nox::db {

namespace {

std::once_flag g_libOnce;   // mysql_library_init 은 스레드를 띄우기 전 한 번(끝낼 때 짝을 안 맞춘다 — 프로세스가 끝나며 거둠)
bool g_libOk = false;

uint32_t NowUnix() { return static_cast<uint32_t>(std::time(nullptr)); }

// 끊김 — 다시 붙으면 될 수 있는 것. KILL 당한 연결의 다음 문장이 어느 번호로 오는지는 db_test ④ 가 기록에 찍는다(미확인 — 셋 다 받음)
bool IsLost(unsigned e) {
    return e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST || e == CR_SERVER_LOST_EXTENDED || e == ER_SESSION_WAS_KILLED ||
           e == ER_CLIENT_INTERACTION_TIMEOUT;
}

// 문장 자리(?) 묶기 — MYSQL_BIND 는 0 으로 비운 뒤 칸만 채운다(라이브러리가 나머지를 쓴다)
void Bin(MYSQL_BIND& b, const std::string& s, unsigned long& len) {   // VARBINARY — 바이트 그대로(글자표 바꾸기 없음)
    len = static_cast<unsigned long>(s.size());
    b.buffer_type = MYSQL_TYPE_BLOB;
    b.buffer = const_cast<char*>(s.data());
    b.buffer_length = len;
    b.length = &len;
}
void Txt(MYSQL_BIND& b, const std::string& s, unsigned long& len) {   // VARCHAR · MEDIUMTEXT(utf8mb4)
    Bin(b, s, len);
    b.buffer_type = MYSQL_TYPE_STRING;
}
void U8(MYSQL_BIND& b, const uint8_t& v) { b.buffer_type = MYSQL_TYPE_TINY; b.buffer = const_cast<uint8_t*>(&v); b.is_unsigned = true; }
void U16(MYSQL_BIND& b, const uint16_t& v) { b.buffer_type = MYSQL_TYPE_SHORT; b.buffer = const_cast<uint16_t*>(&v); b.is_unsigned = true; }
void U32(MYSQL_BIND& b, const uint32_t& v) { b.buffer_type = MYSQL_TYPE_LONG; b.buffer = const_cast<uint32_t*>(&v); b.is_unsigned = true; }
void I32(MYSQL_BIND& b, const int32_t& v) { b.buffer_type = MYSQL_TYPE_LONG; b.buffer = const_cast<int32_t*>(&v); }
void U64(MYSQL_BIND& b, const uint64_t& v) { b.buffer_type = MYSQL_TYPE_LONGLONG; b.buffer = const_cast<uint64_t*>(&v); b.is_unsigned = true; }
void Null(MYSQL_BIND& b, bool& isNull) { b.buffer_type = MYSQL_TYPE_NULL; isNull = true; b.is_null = &isNull; }

constexpr const char* kSelAccount = "SELECT id FROM accounts WHERE name = ?";
constexpr const char* kInsAccount = "INSERT INTO accounts (name, created_unix, last_login_unix) VALUES (?, ?, ?)";
constexpr const char* kUpdLogin = "UPDATE accounts SET last_login_unix = ? WHERE id = ?";
constexpr const char* kInsMatch =
    "INSERT INTO matches (server_ip, server_port, match_key, room_id, room_name, map_key, mode, start_unix, end_unix, frames, end_reason, "
    "winner_kind, winner_id, winner_name, score_limit, time_limit_sec, raw_json, saved_unix) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
constexpr const char* kInsPlayer =
    "INSERT INTO match_players (match_id, seat, account_id, name, net_code, state, team, score, deaths, observer) VALUES (?,?,?,?,?,?,?,?,?,?)";

}  // namespace

struct CDbWorker::Conn {
    MYSQL* m = nullptr;
    MYSQL_STMT* selAccount = nullptr;
    MYSQL_STMT* insAccount = nullptr;
    MYSQL_STMT* updLogin = nullptr;
    MYSQL_STMT* insMatch = nullptr;
    MYSQL_STMT* insPlayer = nullptr;
    unsigned lastErrno = 0;
    std::string lastError;

    ~Conn() {
        for (MYSQL_STMT* s : {selAccount, insAccount, updLogin, insMatch, insPlayer})
            if (s) mysql_stmt_close(s);
        if (m) mysql_close(m);
    }
    bool StmtFail(MYSQL_STMT* s, const char* what) {
        lastErrno = mysql_stmt_errno(s);
        lastError = std::string(what) + " — " + mysql_stmt_error(s) + " (" + std::to_string(lastErrno) + ")";
        return false;
    }
    bool ConnFail(const char* what) {
        lastErrno = mysql_errno(m);
        lastError = std::string(what) + " — " + mysql_error(m) + " (" + std::to_string(lastErrno) + ")";
        return false;
    }
    bool Prepare(MYSQL_STMT*& s, const char* sql) {
        s = mysql_stmt_init(m);
        if (!s) return ConnFail("문장 만들기");
        if (mysql_stmt_prepare(s, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) return StmtFail(s, sql);
        return true;
    }
    // 이름으로 계정 번호(없으면 0). 실패면 false(lastErrno)
    bool FindAccount(const std::string& name, uint64_t& id) {
        MYSQL_BIND p[1] = {};
        unsigned long len = 0;
        Bin(p[0], name, len);
        uint64_t got = 0;
        bool isNull = false;
        MYSQL_BIND r[1] = {};
        U64(r[0], got);
        r[0].is_null = &isNull;
        if (mysql_stmt_bind_param(selAccount, p) != 0 || mysql_stmt_bind_result(selAccount, r) != 0) return StmtFail(selAccount, "계정 찾기 묶기");
        if (mysql_stmt_execute(selAccount) != 0) return StmtFail(selAccount, "계정 찾기");
        if (mysql_stmt_store_result(selAccount) != 0) return StmtFail(selAccount, "계정 찾기 받기");
        const int f = mysql_stmt_fetch(selAccount);
        id = (f == 0 && !isNull) ? got : 0;
        mysql_stmt_free_result(selAccount);
        if (f != 0 && f != MYSQL_NO_DATA) return StmtFail(selAccount, "계정 찾기 꺼내기");
        return true;
    }
};

CDbWorker::CDbWorker(DbConfig cfg) : cfg_(std::move(cfg)) {}

CDbWorker::~CDbWorker() { Stop(); }

void CDbWorker::NoteError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mu_);
    stats_.lastError = e;
}

bool CDbWorker::Connect(std::string& err) {
    Drop();
    auto c = std::make_unique<Conn>();
    c->m = mysql_init(nullptr);
    if (!c->m) {
        err = "mysql_init 실패";
        return false;
    }
    unsigned ct = cfg_.connectTimeoutSec, rw = cfg_.rwTimeoutSec;
    mysql_options(c->m, MYSQL_OPT_CONNECT_TIMEOUT, &ct);
    mysql_options(c->m, MYSQL_OPT_READ_TIMEOUT, &rw);
    mysql_options(c->m, MYSQL_OPT_WRITE_TIMEOUT, &rw);
    mysql_options(c->m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    bool getKey = true;   // caching_sha2_password — TLS 가 없을 때도 첫 접속이 되게(서버 공개키로 암호를 싸 보냄)
    mysql_options(c->m, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &getKey);
    if (!mysql_real_connect(c->m, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(), cfg_.database.c_str(), cfg_.port, nullptr, 0)) {
        c->ConnFail("붙기");
        err = c->lastError;
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.connectFailed;
        stats_.lastError = err;
        return false;
    }
    // 표 버전 — 서버 계정은 표를 못 만든다(정할 것 a). 다르면 사람이 SQL 파일을 돌려야 한다
    if (mysql_query(c->m, "SELECT version FROM schema_version WHERE id = 1") != 0) {
        c->ConnFail("표 버전 읽기(schema_version — tools/db/001_init.sql 을 돌렸나)");
        err = c->lastError;
        return false;
    }
    MYSQL_RES* res = mysql_store_result(c->m);
    MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
    const long ver = row && row[0] ? std::strtol(row[0], nullptr, 10) : -1;
    if (res) mysql_free_result(res);
    if (ver != static_cast<long>(kSchemaVersion)) {
        err = "표 버전 " + std::to_string(ver) + " — 이 로비는 " + std::to_string(kSchemaVersion) + " 을 안다(tools/db/ 의 SQL 파일 차례 확인)";
        return false;
    }
    if (!c->Prepare(c->selAccount, kSelAccount) || !c->Prepare(c->insAccount, kInsAccount) || !c->Prepare(c->updLogin, kUpdLogin) ||
        !c->Prepare(c->insMatch, kInsMatch) || !c->Prepare(c->insPlayer, kInsPlayer)) {
        err = "준비된 문장 — " + c->lastError + "(표 모양이 SQL 파일과 다른가)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.connects;
        connId_ = mysql_thread_id(c->m);
    }
    conn_ = std::move(c);
    return true;
}

void CDbWorker::Drop() {
    conn_.reset();
    std::lock_guard<std::mutex> lk(mu_);
    connId_ = 0;
}

bool CDbWorker::Start(std::string& err) {
    if (started_) return true;
    std::call_once(g_libOnce, [] { g_libOk = mysql_library_init(0, nullptr, nullptr) == 0; });
    if (!g_libOk) {
        err = "mysql_library_init 실패";
        return false;
    }
    if (!Connect(err)) return false;   // 빠른 실패 — 로비가 뜨기 전에
    stop_ = false;
    thread_ = std::thread(&CDbWorker::Run, this);
    started_ = true;
    return true;
}

void CDbWorker::Stop() {
    if (!started_) {
        Drop();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();   // 워커가 남은 일을 다 하고 나간다
    Drop();
    started_ = false;
}

void CDbWorker::CheckLogin(uint32_t req, const std::string& name) {
    Job j;
    j.kind = wol::StoreAnswerKind::Login;
    j.req = req;
    j.name = name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(j));
        ++stats_.requests;
    }
    cv_.notify_one();
}

void CDbWorker::SaveMatch(uint32_t req, const wol::MatchSource& src, const MatchRecord& rec, const std::string& rawJson) {
    Job j;
    j.kind = wol::StoreAnswerKind::SaveMatch;
    j.req = req;
    j.src = src;
    j.rec = rec;
    j.raw = rawJson;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(j));
        ++stats_.requests;
    }
    cv_.notify_one();
}

void CDbWorker::TakeAnswers(std::vector<wol::StoreAnswer>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    for (wol::StoreAnswer& a : answers_) out.push_back(std::move(a));
    answers_.clear();
}

DbStats CDbWorker::Stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

unsigned long CDbWorker::ConnectionId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return connId_;
}

void CDbWorker::Run() {
    mysql_thread_init();
    std::vector<Job> local;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !queue_.empty() || stop_; });
            if (queue_.empty() && stop_) break;   // 닫기 + 남은 일 없음 → 끝(남은 일은 다 한다)
            local.swap(queue_);
            if (local.size() > stats_.queueMax) stats_.queueMax = local.size();
        }
        for (const Job& j : local) {
            wol::StoreAnswer a = Exec(j);
            std::lock_guard<std::mutex> lk(mu_);
            answers_.push_back(std::move(a));
        }
        local.clear();
    }
    mysql_thread_end();
}

// 일 하나 — 끊김(2006 · 2013)을 만나면 다시 붙어 한 번 더. 그래도 안 되면 실패 답
wol::StoreAnswer CDbWorker::Exec(const Job& j) {
    wol::StoreAnswer a;
    a.kind = j.kind;
    a.req = j.req;
    if (j.kind == wol::StoreAnswerKind::SaveMatch && !wol::MatchFitsStore(j.rec, j.raw, a.why)) {   // DB 에 보내 봐야 엄격 모드가 거절한다
        a.save = wol::SaveResult::Invalid;
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.invalid;
        return a;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string err;
        if (!conn_ && !Connect(err)) {
            a.why = "DB 에 못 붙음 — " + err;
            break;
        }
        bool ok = false;
        bool duplicate = false;
        Conn& c = *conn_;
        c.lastErrno = 0;
        const uint32_t now = NowUnix();
        switch (j.kind) {
            case wol::StoreAnswerKind::Login: {
                uint64_t id = 0;
                if (!c.FindAccount(j.name, id)) break;
                if (id != 0) {
                    MYSQL_BIND p[2] = {};
                    U32(p[0], now);
                    U64(p[1], id);
                    if (mysql_stmt_bind_param(c.updLogin, p) != 0 || mysql_stmt_execute(c.updLogin) != 0) {
                        c.StmtFail(c.updLogin, "마지막 로그인 적기");
                        break;
                    }
                    a.accountId = id;
                    ok = true;
                    break;
                }
                MYSQL_BIND p[3] = {};
                unsigned long len = 0;
                Bin(p[0], j.name, len);
                U32(p[1], now);
                U32(p[2], now);
                if (mysql_stmt_bind_param(c.insAccount, p) != 0 || mysql_stmt_execute(c.insAccount) != 0) {
                    c.StmtFail(c.insAccount, "계정 만들기");
                    if (c.lastErrno == ER_DUP_ENTRY && c.FindAccount(j.name, id) && id != 0) {   // 그 사이 다른 로비가 만들었다(로비 여럿 대비)
                        a.accountId = id;
                        ok = true;
                    }
                    break;
                }
                a.accountId = mysql_stmt_insert_id(c.insAccount);
                a.created = true;
                ok = true;
                break;
            }
            case wol::StoreAnswerKind::SaveMatch: {
                const MatchRecord& r = j.rec;
                if (mysql_query(c.m, "START TRANSACTION") != 0) {
                    c.ConnFail("트랜잭션 열기");
                    break;
                }
                MYSQL_BIND p[18] = {};
                unsigned long lMatch = 0, lRoom = 0, lMap = 0, lMode = 0, lEnd = 0, lKind = 0, lWin = 0, lRaw = 0;
                bool nullWinId = false, nullWinName = false;
                U32(p[0], j.src.ip);
                U16(p[1], j.src.port);
                Txt(p[2], r.match, lMatch);
                I32(p[3], r.room);
                Bin(p[4], r.roomName, lRoom);
                Txt(p[5], r.map, lMap);
                Txt(p[6], r.mode, lMode);
                U32(p[7], r.start);
                U32(p[8], r.end);
                U32(p[9], r.frames);
                Txt(p[10], r.endReason, lEnd);
                Txt(p[11], r.winnerKind, lKind);
                if (r.hasWinnerId) U32(p[12], r.winnerId);
                else Null(p[12], nullWinId);
                if (r.hasWinnerAccount) Bin(p[13], r.winnerAccount, lWin);
                else Null(p[13], nullWinName);
                U32(p[14], r.limit);
                U32(p[15], r.timeLimitSec);
                Txt(p[16], j.raw, lRaw);
                U32(p[17], now);
                if (mysql_stmt_bind_param(c.insMatch, p) != 0 || mysql_stmt_execute(c.insMatch) != 0) {
                    c.StmtFail(c.insMatch, "판 넣기");
                    duplicate = c.lastErrno == ER_DUP_ENTRY;   // 같은 (게임 서버 · 판 번호) — 다시 보낸 판
                    mysql_rollback(c.m);
                    break;
                }
                const uint64_t matchId = mysql_stmt_insert_id(c.insMatch);
                bool rowsOk = true;
                for (size_t seat = 0; rowsOk && seat < r.players.size(); ++seat) {
                    const MatchRecordPlayer& pl = r.players[seat];
                    uint64_t accountId = 0;
                    if (pl.hasAccount && !c.FindAccount(pl.account, accountId)) {
                        rowsOk = false;
                        break;
                    }
                    MYSQL_BIND q[10] = {};
                    const uint8_t seat8 = static_cast<uint8_t>(seat);
                    const uint8_t observer = pl.observer ? 1 : 0;
                    const std::string state = pl.left ? "left" : "finished";
                    unsigned long lName = 0, lState = 0;
                    bool nullAcc = false, nullName = false, nullTeam = false, nullScore = false, nullDeaths = false;
                    U64(q[0], matchId);
                    U8(q[1], seat8);
                    if (accountId != 0) U64(q[2], accountId);
                    else Null(q[2], nullAcc);   // 로비를 안 거친 사람 · 그 이름 계정이 없음
                    if (pl.hasAccount) Bin(q[3], pl.account, lName);
                    else Null(q[3], nullName);
                    U16(q[4], pl.netCode);
                    Txt(q[5], state, lState);
                    if (pl.left) {   // 나간 사람은 팀 · 점수 · 죽은 수가 안 실렸다
                        Null(q[6], nullTeam);
                        Null(q[7], nullScore);
                        Null(q[8], nullDeaths);
                    } else {
                        U8(q[6], pl.team);
                        I32(q[7], pl.score);
                        U32(q[8], pl.deaths);
                    }
                    U8(q[9], observer);
                    if (mysql_stmt_bind_param(c.insPlayer, q) != 0 || mysql_stmt_execute(c.insPlayer) != 0) {
                        c.StmtFail(c.insPlayer, "판 사람 넣기");
                        rowsOk = false;
                    }
                }
                if (!rowsOk) {
                    mysql_rollback(c.m);
                    break;
                }
                if (mysql_commit(c.m) != 0) {
                    c.ConnFail("판 넣기 끝내기(commit)");
                    mysql_rollback(c.m);
                    break;
                }
                ok = true;
                break;
            }
            case wol::StoreAnswerKind::kCount:
                break;
        }
        if (ok || duplicate) {
            std::lock_guard<std::mutex> lk(mu_);
            if (j.kind == wol::StoreAnswerKind::Login) {
                a.login = wol::LoginResult::Ok;
                ++stats_.loginOk;
                if (a.created) ++stats_.loginCreated;
            } else {
                a.save = duplicate ? wol::SaveResult::Duplicate : wol::SaveResult::Saved;
                ++(duplicate ? stats_.duplicates : stats_.saved);
            }
            return a;
        }
        a.why = c.lastError;
        NoteError(c.lastError);
        if (!IsLost(c.lastErrno)) break;   // 끊김이 아니면 다시 해도 같다
        Drop();                            // 끊김 — 다시 붙어 한 번 더
        if (attempt == 0) {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.retries;
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (j.kind == wol::StoreAnswerKind::Login) {
        a.login = wol::LoginResult::Failed;
        ++stats_.loginFailed;
    } else {
        a.save = wol::SaveResult::Failed;
        ++stats_.saveFailed;
    }
    return a;
}

}  // namespace nox::db
