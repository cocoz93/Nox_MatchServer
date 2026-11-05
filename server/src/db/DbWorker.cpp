// DbWorker.cpp — 로비의 DB 워커. 설명은 `DbWorker.h` 머리 · 표는 `tools/db/001_init.sql`.
#include "nox/DbWorker.h"

#include "MatchRowBinds.h"   // ★ 11-04 전적 검색 — 판 줄 읽기 · 합계 쓰기 칸(다시 계산 도구와 같이 씀)
#include "MySqlBind.h"   // ★ 로그인 암호 조각 2 — 라이브러리 초기화 · 끊김 · 자리 묶기 · 붙기(로그인 서버와 같이 씀)

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

namespace nox::db {

namespace {

uint32_t NowUnix() { return static_cast<uint32_t>(std::time(nullptr)); }

// ★ 11-04 전적 검색 — 그 역할이 하는 일(나누는 곳 `DbLobbyStore.cpp`). 다른 역할의 문장은 준비돼 있지 않다
bool RoleDoes(DbWorkerRole role, wol::StoreAnswerKind kind) {
    switch (kind) {
        case wol::StoreAnswerKind::SaveMatch:
        case wol::StoreAnswerKind::Rating: return role == DbWorkerRole::Writer;
        case wol::StoreAnswerKind::History:
        case wol::StoreAnswerKind::ActiveBans: return role == DbWorkerRole::Reader;
        case wol::StoreAnswerKind::kCount: break;
    }
    return false;
}

// ★ 로그인 암호 조각 4 — 계정 만들기 · 마지막 로그인은 로그인 서버 몫(정할 것 a). 로비는 판 사람의 계정 번호를 이름으로 찾기만
constexpr const char* kSelAccount = "SELECT id FROM accounts WHERE name = ?";
// ★ 11-05 플랜1 — 지금 유효한 정지 전체 · 이름마다 가장 늦은 끝(플랜 5절 「여러 행이면 가장 늦은 끝 시각」)
constexpr const char* kSelBans =
    "SELECT a.name, MAX(b.until_unix) FROM account_bans b JOIN accounts a ON a.id = b.account_id "
    "WHERE b.lifted_unix = 0 AND b.until_unix > ? GROUP BY a.name";
// ★ 레이팅 조각 3 — matches.ranked · match_players.rating_delta(`003_rating.sql`)
constexpr const char* kInsMatch =
    "INSERT INTO matches (server_ip, server_port, match_key, room_id, room_name, map_key, mode, start_unix, end_unix, frames, end_reason, "
    "winner_kind, winner_id, winner_name, score_limit, time_limit_sec, ranked, raw_json, saved_unix) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
constexpr const char* kInsPlayer =
    "INSERT INTO match_players (match_id, seat, account_id, name, net_code, state, team, score, deaths, observer, rating_delta, kills) "
    "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";
// ★ 레이팅 조각 3 — 점수 올리기(행이 없으면 시작 점수 + 증감으로 만든다) · 이름으로 점수 · 그보다 높은 사람 수
constexpr const char* kUpsRating =
    "INSERT INTO ratings (account_id, rating, updated_unix) VALUES (?,?,?) ON DUPLICATE KEY UPDATE rating = rating + ?, updated_unix = ?";
constexpr const char* kSelRating = "SELECT r.rating FROM accounts a JOIN ratings r ON r.account_id = a.id WHERE a.name = ?";
constexpr const char* kSelRank = "SELECT COUNT(*) FROM ratings WHERE rating > ?";
// ★ 11-04 플랜2 — 대전 기록. ★ 11-04 전적 검색 — 승패는 합계 표(`kSelTotals`) · 줄은 최근 20개만(`fk_match_players_account` 색인에 기본 열쇠가 붙어
//   색인만 거꾸로 따라가 20줄에서 멈춘다 — 5,000판 계정 실행 계획 5000 → 20행 `_out/qa_p12/bench`). 차례 = 저장 차례(`MatchHistory.cpp` 점수 되짚기와 같은 차례)
constexpr const char* kSelHistory =
    "SELECT " NOX_SQL_MATCH_ROW_COLUMNS " "
    "FROM match_players p JOIN matches m ON m.id = p.match_id JOIN accounts a ON a.id = p.account_id "
    "WHERE a.name = ? AND p.observer = 0 ORDER BY p.match_id DESC LIMIT ?";
constexpr const char* kSelTotals =
    "SELECT t.games, t.ranked_wins, t.ranked_losses, t.normal_wins, t.normal_losses FROM accounts a JOIN match_totals t ON t.account_id = a.id WHERE a.name = ?";
// ★ 11-04 전적 검색 — 판 저장 트랜잭션 안에서 합계 올리기(행이 없으면 만든다 · `ratings` 와 같은 꼴). 칸 차례 = `BindMatchTotals`
constexpr const char* kUpsTotals =
    "INSERT INTO match_totals (account_id, games, ranked_wins, ranked_losses, normal_wins, normal_losses, updated_unix) VALUES (?,?,?,?,?,?,?) "
    "ON DUPLICATE KEY UPDATE games = games + ?, ranked_wins = ranked_wins + ?, ranked_losses = ranked_losses + ?, "
    "normal_wins = normal_wins + ?, normal_losses = normal_losses + ?, updated_unix = ?";

}  // namespace

struct CDbWorker::Conn {
    MYSQL* m = nullptr;
    MYSQL_STMT* selAccount = nullptr;
    MYSQL_STMT* insMatch = nullptr;
    MYSQL_STMT* insPlayer = nullptr;
    MYSQL_STMT* upsRating = nullptr;   // ★ 레이팅 조각 3
    MYSQL_STMT* selRating = nullptr;
    MYSQL_STMT* selRank = nullptr;
    MYSQL_STMT* selHistory = nullptr;   // ★ 11-04 플랜2
    MYSQL_STMT* selTotals = nullptr;    // ★ 11-04 전적 검색
    MYSQL_STMT* upsTotals = nullptr;
    MYSQL_STMT* selBans = nullptr;      // ★ 11-05 플랜1
    unsigned lastErrno = 0;
    std::string lastError;

    ~Conn() {
        for (MYSQL_STMT* s : {selAccount, insMatch, insPlayer, upsRating, selRating, selRank, selHistory, selTotals, upsTotals, selBans})
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
    // ★ 11-04 전적 검색 — 역할의 문장만. 표 모양 검사는 두 연결이 각자 지난다
    bool PrepareFor(DbWorkerRole role) {
        if (!Prepare(selAccount, kSelAccount) || !Prepare(selRating, kSelRating) || !Prepare(selRank, kSelRank)) return false;
        if (role == DbWorkerRole::Writer)
            return Prepare(insMatch, kInsMatch) && Prepare(insPlayer, kInsPlayer) && Prepare(upsRating, kUpsRating) && Prepare(upsTotals, kUpsTotals);
        return Prepare(selTotals, kSelTotals) && Prepare(selHistory, kSelHistory) && Prepare(selBans, kSelBans);
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
    // ★ 레이팅 조각 3 — 이름으로 점수(행이 없으면 found false). 실패면 false(lastErrno)
    //   ★ 11-04 전적 검색 — 순위(`ReadRank`)와 나눔. 판 저장은 고친 점수만 쓰는데 사람마다 순위까지 세고 버렸다
    bool ReadScore(const std::string& name, bool& found, int32_t& rating) {
        found = false;
        rating = kRatingStart;
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
        return true;
    }
    // 그 점수보다 높은 사람 수 + 1
    bool ReadRank(const int32_t& rating, uint32_t& rank) {
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
    // 점수 · 순위(점수 행이 없으면 순위 0)
    bool ReadRating(const std::string& name, bool& found, int32_t& rating, uint32_t& rank) {
        rank = 0;
        if (!ReadScore(name, found, rating)) return false;
        return !found || ReadRank(rating, rank);
    }
    // ★ 11-04 전적 검색 — 합계 올리기(판 저장 트랜잭션 안에서 부른다)
    bool AddTotals(uint64_t accountId, const wol::MatchTotals& t, uint32_t now) {
        MYSQL_BIND p[kMatchTotalsParams] = {};
        BindMatchTotals(p, accountId, t, now);
        if (mysql_stmt_bind_param(upsTotals, p) != 0 || mysql_stmt_execute(upsTotals) != 0) return StmtFail(upsTotals, "합계 올리기");
        return true;
    }
    // ★ 11-04 전적 검색 — 이름의 합계(행이 없으면 전부 0 — 판이 없거나 관전만). 실패면 false(lastErrno)
    bool ReadTotals(const std::string& name, wol::MatchTotals& t) {
        t = wol::MatchTotals{};
        MYSQL_BIND p[1] = {};
        unsigned long len = 0;
        Bin(p[0], name, len);
        MYSQL_BIND r[5] = {};
        U32(r[0], t.games);
        U32(r[1], t.rankedWins);
        U32(r[2], t.rankedLosses);
        U32(r[3], t.normalWins);
        U32(r[4], t.normalLosses);
        if (mysql_stmt_bind_param(selTotals, p) != 0 || mysql_stmt_bind_result(selTotals, r) != 0) return StmtFail(selTotals, "합계 읽기 묶기");
        if (mysql_stmt_execute(selTotals) != 0) return StmtFail(selTotals, "합계 읽기");
        if (mysql_stmt_store_result(selTotals) != 0) return StmtFail(selTotals, "합계 읽기 받기");
        const int f = mysql_stmt_fetch(selTotals);
        mysql_stmt_free_result(selTotals);
        if (f == MYSQL_NO_DATA) {
            t = wol::MatchTotals{};
            return true;
        }
        if (f != 0) return StmtFail(selTotals, "합계 읽기 꺼내기");
        return true;
    }
    // ★ 11-04 플랜2 — 이름의 최근 판을 저장 차례 최신부터 `rows` 에. 실패면 false(lastErrno · `rows` 는 일부만 찬 채)
    // ★ 11-05 플랜1 — 끊겨 다시 할 때 앞 번 줄이 남지 않게 부르는 쪽이 비운다
    bool ReadBans(uint32_t now, std::vector<wol::BanEntry>& rows) {
        MYSQL_BIND p[1] = {};
        U32(p[0], now);
        char name[wol::kAccountNameBytes];
        unsigned long nameLen = 0;
        uint32_t until = 0;
        MYSQL_BIND r[2] = {};
        r[0].buffer_type = MYSQL_TYPE_BLOB;
        r[0].buffer = name;
        r[0].buffer_length = sizeof(name);
        r[0].length = &nameLen;
        U32(r[1], until);
        if (mysql_stmt_bind_param(selBans, p) != 0 || mysql_stmt_bind_result(selBans, r) != 0) return StmtFail(selBans, "정지 읽기 묶기");
        if (mysql_stmt_execute(selBans) != 0) return StmtFail(selBans, "정지 읽기");
        if (mysql_stmt_store_result(selBans) != 0) return StmtFail(selBans, "정지 읽기 받기");
        int f = 0;
        while ((f = mysql_stmt_fetch(selBans)) == 0) {
            wol::BanEntry e;
            e.name.assign(name, nameLen);
            e.until = until;
            rows.push_back(std::move(e));
        }
        if (f != MYSQL_NO_DATA) {
            StmtFail(selBans, "정지 읽기 꺼내기");
            mysql_stmt_free_result(selBans);
            return false;
        }
        mysql_stmt_free_result(selBans);
        return true;
    }
    bool ReadHistory(const std::string& name, std::vector<wol::MatchHistoryRow>& rows) {
        MYSQL_BIND p[2] = {};
        unsigned long len = 0;
        const uint32_t limit = static_cast<uint32_t>(wol::kMatchHistoryMaxRows);
        Bin(p[0], name, len);
        U32(p[1], limit);
        MatchRowBinds row;
        MYSQL_BIND r[MatchRowBinds::kColumns] = {};
        row.Bind(r);
        if (mysql_stmt_bind_param(selHistory, p) != 0 || mysql_stmt_bind_result(selHistory, r) != 0) return StmtFail(selHistory, "대전 기록 묶기");
        if (mysql_stmt_execute(selHistory) != 0) return StmtFail(selHistory, "대전 기록");
        if (mysql_stmt_store_result(selHistory) != 0) return StmtFail(selHistory, "대전 기록 받기");
        int f = 0;
        while ((f = mysql_stmt_fetch(selHistory)) == 0) rows.push_back(row.Row());
        if (f != MYSQL_NO_DATA) {
            StmtFail(selHistory, "대전 기록 꺼내기");
            mysql_stmt_free_result(selHistory);
            return false;
        }
        mysql_stmt_free_result(selHistory);
        return true;
    }
};

CDbWorker::CDbWorker(DbConfig cfg, DbWorkerRole role) : cfg_(std::move(cfg)), role_(role) {}

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
    if (!c->PrepareFor(role_)) {
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
    JoinAndClose();
}

void CDbWorker::StopDiscarding() {
    if (!started_) {
        Drop();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        discard_ = true;
        stats_.historyDropped += queue_.size();
        queue_.clear();
    }
    JoinAndClose();
}

void CDbWorker::JoinAndClose() {
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

void CDbWorker::GetMatchHistory(uint32_t req, const std::string& account) {
    Job j;
    j.kind = wol::StoreAnswerKind::History;
    j.req = req;
    j.account = account;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(j));
        ++stats_.historyRequests;
    }
    cv_.notify_one();
}

void CDbWorker::GetActiveBans(uint32_t req, uint32_t now) {
    Job j;
    j.kind = wol::StoreAnswerKind::ActiveBans;
    j.req = req;
    j.now = now;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(j));
        ++stats_.bansRequests;
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
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (discard_) {   // ★ 11-04 전적 검색 — 꺼내 둔 일도 `StopDiscarding` 뒤엔 버린다
                    if (j.kind == wol::StoreAnswerKind::ActiveBans) ++stats_.bansDropped;   // ★ 11-05 플랜1
                    else ++stats_.historyDropped;
                    continue;
                }
            }
            wol::StoreAnswer a = Exec(j);
            {
                std::lock_guard<std::mutex> lk(mu_);
                answers_.push_back(std::move(a));
            }
            // ★ 11-05 — 넣은 뒤에 깨운다. 꺼내기 뒤에 넣은 답은 이 호출이 다시 깨우므로 놓치지 않는다 · 잠금 밖이라 깨우는 쪽이 저장소를 불러도 된다
            if (wake_) wake_();
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
    if (!RoleDoes(role_, j.kind)) {   // ★ 11-04 전적 검색 — 짜는 실수(`DbLobbyStore.cpp`) · 안 준비된 문장을 부르면 라이브러리가 죽는다
        a.why = "이 워커의 역할이 아닌 일";
        NoteError(a.why);
        return FailAnswer(j, std::move(a));
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string err;
        if (!conn_ && !Connect(err)) {
            a.why = "DB 에 못 붙음 — " + err;
            break;
        }
        Conn& c = *conn_;
        c.lastErrno = 0;
        bool ok = false;
        switch (j.kind) {
            case wol::StoreAnswerKind::SaveMatch: ok = ExecSave(c, j, a); break;
            case wol::StoreAnswerKind::Rating: ok = ExecRating(c, j, a); break;
            case wol::StoreAnswerKind::History: ok = ExecHistory(c, j, a); break;
            case wol::StoreAnswerKind::ActiveBans: ok = ExecBans(c, j, a); break;
            case wol::StoreAnswerKind::kCount: break;
        }
        if (ok) return a;
        a.why = c.lastError;
        NoteError(c.lastError);
        if (!IsLost(c.lastErrno)) break;   // 끊김이 아니면 다시 해도 같다
        Drop();                            // 끊김 — 다시 붙어 한 번 더
        if (attempt == 0) {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.retries;
        }
    }
    return FailAnswer(j, std::move(a));
}

wol::StoreAnswer CDbWorker::FailAnswer(const Job& j, wol::StoreAnswer a) {
    std::lock_guard<std::mutex> lk(mu_);
    if (j.kind == wol::StoreAnswerKind::Rating) {   // ★ 레이팅 조각 3 — 못 읽음(ratingOk false) · 로비는 점수 줄 없이 로그인을 끝낸다
        ++stats_.ratingFailed;
        return a;
    }
    if (j.kind == wol::StoreAnswerKind::History) {   // ★ 11-04 플랜2 — 못 읽음(historyOk false) · 로비는 `RECFAIL`
        a.history = wol::MatchHistory{};
        ++stats_.historyFailed;
        return a;
    }
    if (j.kind == wol::StoreAnswerKind::ActiveBans) {   // ★ 11-05 플랜1 — 못 읽음(bansOk false) · 로비는 앞 목록을 그대로 둔다
        a.bans.clear();
        ++stats_.bansFailed;
        return a;
    }
    a.save = wol::SaveResult::Failed;
    ++stats_.saveFailed;
    return a;
}

// 판 하나 — 성공 · 겹침이면 true(`a.save`)
bool CDbWorker::ExecSave(Conn& c, const Job& j, wol::StoreAnswer& a) {
    const uint32_t now = NowUnix();
    const MatchRecord& r = j.rec;
    if (mysql_query(c.m, "START TRANSACTION") != 0) return c.ConnFail("트랜잭션 열기");
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
        const bool duplicate = c.lastErrno == ER_DUP_ENTRY;   // 같은 (게임 서버 · 판 번호) — 다시 보낸 판
        mysql_rollback(c.m);
        if (!duplicate) return false;
        a.save = wol::SaveResult::Duplicate;
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.duplicates;
        return true;
    }
    const uint64_t matchId = mysql_stmt_insert_id(c.insMatch);
    // ★ 레이팅 조각 3 — 자리별 증감(`Rating.h`). 계정을 못 찾은 사람(이름만 있고 계정 행 없음)은 반영 안 함(NULL)
    const RatingResult rr = RatingDeltas(r);
    bool rowsOk = true;
    unsigned long long ratingRows = 0, totalsRows = 0;
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
        MYSQL_BIND q[12] = {};
        const uint8_t seat8 = static_cast<uint8_t>(seat);
        const uint8_t observer = pl.observer ? 1 : 0;
        const std::string state = pl.left ? "left" : "finished";
        unsigned long lName = 0, lState = 0;
        bool nullAcc = false, nullName = false, nullTeam = false, nullScore = false, nullDeaths = false, nullDelta = false, nullKills = false;
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
        if (!pl.left && pl.hasKills) U32(q[11], pl.kills);   // ★ 11-04 플랜2 — 나간 사람 · 옛 줄은 NULL
        else Null(q[11], nullKills);
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
        if (rowsOk && accountId != 0 && !pl.observer) {   // ★ 11-04 전적 검색 — 승패 합계(같은 트랜잭션 · 겹친 판은 `uq_matches_server_key` 에서 이미 되돌아갔다)
            wol::MatchTotals one;
            one.Add(wol::MatchRowOf(r, rr, seat));
            if (c.AddTotals(accountId, one, now)) ++totalsRows;
            else rowsOk = false;
        }
    }
    if (!rowsOk) {
        mysql_rollback(c.m);
        return false;
    }
    if (mysql_commit(c.m) != 0) {
        c.ConnFail("판 넣기 끝내기(commit)");
        mysql_rollback(c.m);
        return false;
    }
    // ★ 매칭 손보기 1 — 고친 점수를 다시 읽어 답에(로비가 `SAVED` 꼬리로). 못 읽으면 그 사람만 빼고 저장은 그대로 Saved
    for (const auto& rp : changed) {
        bool found = false;
        int32_t after = 0;
        if (c.ReadScore(rp.first, found, after) && found) a.changes.push_back({rp.first, after, rp.second});
    }
    a.save = wol::SaveResult::Saved;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.saved;
    stats_.ratingRows += ratingRows;
    stats_.totalsRows += totalsRows;
    return true;
}

// ★ 레이팅 조각 3 — 읽기만(트랜잭션 없음)
bool CDbWorker::ExecRating(Conn& c, const Job& j, wol::StoreAnswer& a) {
    if (!c.ReadRating(j.account, a.ratingFound, a.rating, a.rank)) return false;
    a.ratingOk = true;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.ratingReads;
    return true;
}

// ★ 11-04 플랜2 — 읽기만 · 다시 할 때 앞서 읽은 줄을 비운다
//   ★ 11-04 전적 검색 — 저장이 다른 스레드로 갈라져 문장 넷 사이에 판이 끼어들 수 있다. 점수 칸은 지금 점수에서 표의 증감을 되짚어 구하므로(`MatchHistory.cpp`)
//     읽기 하나를 한 시점으로 묶는다
bool CDbWorker::ExecBans(Conn& c, const Job& j, wol::StoreAnswer& a) {
    a.bans.clear();
    if (!c.ReadBans(j.now, a.bans)) return false;
    a.bansOk = true;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.bansReads;
    return true;
}

bool CDbWorker::ExecHistory(Conn& c, const Job& j, wol::StoreAnswer& a) {
    a.history = wol::MatchHistory{};
    if (mysql_query(c.m, "START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY") != 0) return c.ConnFail("읽기 시점 열기");
    uint64_t accountId = 0;   // ★ 11-04 전적 검색 — 없는 계정과 판 없는 계정을 가른다(내 기록은 계정이 없어도 줄을 내므로 나머지도 그대로 읽는다)
    const bool ok = c.FindAccount(j.account, accountId) && c.ReadRating(j.account, a.ratingFound, a.rating, a.rank) && c.ReadTotals(j.account, a.history.totals) && c.ReadHistory(j.account, a.history.rows);
    mysql_rollback(c.m);   // 늘 닫는다 — 안 닫으면 다음 조회까지 이 시점이 남는다(실패해 끊긴 연결이면 다음 일이 다시 붙는다)
    if (!ok) return false;
    a.historyOk = true;
    a.accountFound = accountId != 0;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.historyReads;
    return true;
}

}  // namespace nox::db
