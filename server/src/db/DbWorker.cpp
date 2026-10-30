// DbWorker.cpp — 로비의 DB 워커. 설명은 `DbWorker.h` 머리 · 표는 `tools/db/001_init.sql`.
#include "nox/DbWorker.h"

#include "MySqlBind.h"   // ★ 로그인 암호 조각 2 — 라이브러리 초기화 · 끊김 · 자리 묶기 · 붙기(로그인 서버와 같이 씀)

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

namespace nox::db {

namespace {

uint32_t NowUnix() { return static_cast<uint32_t>(std::time(nullptr)); }

// ★ 로그인 암호 조각 4 — 계정 만들기 · 마지막 로그인은 로그인 서버 몫(정할 것 a). 로비는 판 사람의 계정 번호를 이름으로 찾기만
constexpr const char* kSelAccount = "SELECT id FROM accounts WHERE name = ?";
// ★ 레이팅 조각 3 — matches.ranked · match_players.rating_delta(`003_rating.sql`)
constexpr const char* kInsMatch =
    "INSERT INTO matches (server_ip, server_port, match_key, room_id, room_name, map_key, mode, start_unix, end_unix, frames, end_reason, "
    "winner_kind, winner_id, winner_name, score_limit, time_limit_sec, ranked, raw_json, saved_unix) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
constexpr const char* kInsPlayer =
    "INSERT INTO match_players (match_id, seat, account_id, name, net_code, state, team, score, deaths, observer, rating_delta) "
    "VALUES (?,?,?,?,?,?,?,?,?,?,?)";
// ★ 레이팅 조각 3 — 점수 올리기(행이 없으면 시작 점수 + 증감으로 만든다) · 이름으로 점수 · 그보다 높은 사람 수
constexpr const char* kUpsRating =
    "INSERT INTO ratings (account_id, rating, updated_unix) VALUES (?,?,?) ON DUPLICATE KEY UPDATE rating = rating + ?, updated_unix = ?";
constexpr const char* kSelRating = "SELECT r.rating FROM accounts a JOIN ratings r ON r.account_id = a.id WHERE a.name = ?";
constexpr const char* kSelRank = "SELECT COUNT(*) FROM ratings WHERE rating > ?";

}  // namespace

struct CDbWorker::Conn {
    MYSQL* m = nullptr;
    MYSQL_STMT* selAccount = nullptr;
    MYSQL_STMT* insMatch = nullptr;
    MYSQL_STMT* insPlayer = nullptr;
    MYSQL_STMT* upsRating = nullptr;   // ★ 레이팅 조각 3
    MYSQL_STMT* selRating = nullptr;
    MYSQL_STMT* selRank = nullptr;
    unsigned lastErrno = 0;
    std::string lastError;

    ~Conn() {
        for (MYSQL_STMT* s : {selAccount, insMatch, insPlayer, upsRating, selRating, selRank})
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
    // ★ 레이팅 조각 3 — 점수 고치기(판 저장 트랜잭션 안에서 부른다)
    bool AddRating(uint64_t accountId, int32_t delta, uint32_t now) {
        MYSQL_BIND p[5] = {};
        int32_t first = kRatingStart + delta;
        U64(p[0], accountId);
        I32(p[1], first);
        U32(p[2], now);
        I32(p[3], delta);
        U32(p[4], now);
        if (mysql_stmt_bind_param(upsRating, p) != 0 || mysql_stmt_execute(upsRating) != 0) return StmtFail(upsRating, "점수 고치기");
        return true;
    }
    // ★ 레이팅 조각 3 — 이름으로 점수 · 순위(행이 없으면 found false). 실패면 false(lastErrno)
    bool ReadRating(const std::string& name, bool& found, int32_t& rating, uint32_t& rank) {
        found = false;
        rating = kRatingStart;
        rank = 0;
        MYSQL_BIND p[1] = {};
        unsigned long len = 0;
        Bin(p[0], name, len);
        int32_t got = 0;
        MYSQL_BIND r[1] = {};
        I32(r[0], got);
        if (mysql_stmt_bind_param(selRating, p) != 0 || mysql_stmt_bind_result(selRating, r) != 0) return StmtFail(selRating, "점수 읽기 묶기");
        if (mysql_stmt_execute(selRating) != 0) return StmtFail(selRating, "점수 읽기");
        if (mysql_stmt_store_result(selRating) != 0) return StmtFail(selRating, "점수 읽기 받기");
        const int f = mysql_stmt_fetch(selRating);
        mysql_stmt_free_result(selRating);
        if (f == MYSQL_NO_DATA) return true;
        if (f != 0) return StmtFail(selRating, "점수 읽기 꺼내기");
        found = true;
        rating = got;
        MYSQL_BIND q[1] = {};
        I32(q[0], rating);
        uint64_t higher = 0;
        MYSQL_BIND s[1] = {};
        U64(s[0], higher);
        if (mysql_stmt_bind_param(selRank, q) != 0 || mysql_stmt_bind_result(selRank, s) != 0) return StmtFail(selRank, "순위 묶기");
        if (mysql_stmt_execute(selRank) != 0) return StmtFail(selRank, "순위");
        if (mysql_stmt_store_result(selRank) != 0) return StmtFail(selRank, "순위 받기");
        const int g = mysql_stmt_fetch(selRank);
        mysql_stmt_free_result(selRank);
        if (g != 0) return StmtFail(selRank, "순위 꺼내기");
        rank = static_cast<uint32_t>(higher + 1);
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
    bool connectFailed = false;
    c->m = OpenChecked(cfg_, err, connectFailed);   // 붙기 + 표 버전(`MySqlBind.h`)
    if (!c->m) {
        if (connectFailed) {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.connectFailed;
            stats_.lastError = err;
        }
        return false;
    }
    if (!c->Prepare(c->selAccount, kSelAccount) || !c->Prepare(c->insMatch, kInsMatch) || !c->Prepare(c->insPlayer, kInsPlayer) ||
        !c->Prepare(c->upsRating, kUpsRating) || !c->Prepare(c->selRating, kSelRating) || !c->Prepare(c->selRank, kSelRank)) {
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
    if (!LibInit()) {
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

void CDbWorker::GetRating(uint32_t req, const std::string& account) {
    Job j;
    j.kind = wol::StoreAnswerKind::Rating;
    j.req = req;
    j.account = account;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(j));
        ++stats_.ratingRequests;
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
            case wol::StoreAnswerKind::SaveMatch: {
                const MatchRecord& r = j.rec;
                if (mysql_query(c.m, "START TRANSACTION") != 0) {
                    c.ConnFail("트랜잭션 열기");
                    break;
                }
                MYSQL_BIND p[19] = {};
                unsigned long lMatch = 0, lRoom = 0, lMap = 0, lMode = 0, lEnd = 0, lKind = 0, lWin = 0, lRaw = 0;
                bool nullWinId = false, nullWinName = false;
                const uint8_t ranked = r.ranked ? 1 : 0;   // ★ 레이팅 조각 3
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
                U8(p[16], ranked);
                Txt(p[17], j.raw, lRaw);
                U32(p[18], now);
                if (mysql_stmt_bind_param(c.insMatch, p) != 0 || mysql_stmt_execute(c.insMatch) != 0) {
                    c.StmtFail(c.insMatch, "판 넣기");
                    duplicate = c.lastErrno == ER_DUP_ENTRY;   // 같은 (게임 서버 · 판 번호) — 다시 보낸 판
                    mysql_rollback(c.m);
                    break;
                }
                const uint64_t matchId = mysql_stmt_insert_id(c.insMatch);
                // ★ 레이팅 조각 3 — 자리별 증감(`Rating.h`). 계정을 못 찾은 사람(이름만 있고 계정 행 없음)은 반영 안 함(NULL)
                const RatingResult rr = RatingDeltas(r);
                bool rowsOk = true;
                unsigned long long ratingRows = 0;
                std::vector<std::pair<std::string, int32_t>> changed;   // ★ 매칭 손보기 1 — 점수를 고친 사람(이름 · 증감 — 커밋 뒤 새 점수를 읽는다)
                for (size_t seat = 0; rowsOk && seat < r.players.size(); ++seat) {
                    const MatchRecordPlayer& pl = r.players[seat];
                    uint64_t accountId = 0;
                    if (pl.hasAccount && !c.FindAccount(pl.account, accountId)) {
                        rowsOk = false;
                        break;
                    }
                    const bool rated = rr.seats[seat].rated && accountId != 0;
                    const int32_t delta = rr.seats[seat].delta;
                    MYSQL_BIND q[11] = {};
                    const uint8_t seat8 = static_cast<uint8_t>(seat);
                    const uint8_t observer = pl.observer ? 1 : 0;
                    const std::string state = pl.left ? "left" : "finished";
                    unsigned long lName = 0, lState = 0;
                    bool nullAcc = false, nullName = false, nullTeam = false, nullScore = false, nullDeaths = false, nullDelta = false;
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
                    if (rated) I32(q[10], delta);
                    else Null(q[10], nullDelta);
                    if (mysql_stmt_bind_param(c.insPlayer, q) != 0 || mysql_stmt_execute(c.insPlayer) != 0) {
                        c.StmtFail(c.insPlayer, "판 사람 넣기");
                        rowsOk = false;
                    } else if (rated) {
                        if (c.AddRating(accountId, delta, now)) {
                            ++ratingRows;
                            changed.emplace_back(pl.account, delta);
                        } else {
                            rowsOk = false;
                        }
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
                // ★ 매칭 손보기 1 — 고친 점수를 다시 읽어 답에(로비가 `SAVED` 꼬리로). 못 읽으면 그 사람만 빼고 저장은 그대로 Saved
                for (const auto& rp : changed) {
                    bool found = false;
                    int32_t after = 0;
                    uint32_t rank = 0;
                    if (c.ReadRating(rp.first, found, after, rank) && found) a.changes.push_back({rp.first, after, rp.second});
                }
                if (ratingRows) {
                    std::lock_guard<std::mutex> lk(mu_);
                    stats_.ratingRows += ratingRows;
                }
                break;
            }
            case wol::StoreAnswerKind::Rating: {   // ★ 레이팅 조각 3 — 읽기만(트랜잭션 없음)
                if (c.ReadRating(j.account, a.ratingFound, a.rating, a.rank)) {
                    a.ratingOk = true;
                    std::lock_guard<std::mutex> lk(mu_);
                    ++stats_.ratingReads;
                    return a;
                }
                break;
            }
            case wol::StoreAnswerKind::kCount:
                break;
        }
        if (ok || duplicate) {
            std::lock_guard<std::mutex> lk(mu_);
            a.save = duplicate ? wol::SaveResult::Duplicate : wol::SaveResult::Saved;
            ++(duplicate ? stats_.duplicates : stats_.saved);
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
    if (j.kind == wol::StoreAnswerKind::Rating) {   // ★ 레이팅 조각 3 — 못 읽음(ratingOk false) · 로비는 점수 줄 없이 로그인을 끝낸다
        ++stats_.ratingFailed;
        return a;
    }
    a.save = wol::SaveResult::Failed;
    ++stats_.saveFailed;
    return a;
}

}  // namespace nox::db
