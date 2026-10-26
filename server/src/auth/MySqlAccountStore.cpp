// MySqlAccountStore.cpp — 로그인 서버의 MySQL 계정 저장소. 설명은 `MySqlAccountStore.h` 머리 · 표는 `tools/db/001_init.sql` + `002_auth.sql`.
#include "MySqlAccountStore.h"

#include "../db/MySqlBind.h"

#include <cstring>
#include <utility>

namespace nox::auth {

namespace {

constexpr const char* kFind = "SELECT id, pass_salt, pass_hash, pass_iter FROM accounts WHERE name = ?";
constexpr const char* kInsert =
    "INSERT INTO accounts (name, pass_salt, pass_hash, pass_iter, created_unix, last_login_unix) VALUES (?, ?, ?, ?, ?, 0)";
constexpr const char* kClaim = "UPDATE accounts SET pass_salt = ?, pass_hash = ?, pass_iter = ? WHERE name = ? AND pass_hash IS NULL";
constexpr const char* kTouch = "UPDATE accounts SET last_login_unix = ? WHERE id = ?";

}  // namespace

struct CMySqlAccountStore::Conn {
    MYSQL* m = nullptr;
    MYSQL_STMT* find = nullptr;
    MYSQL_STMT* insert = nullptr;
    MYSQL_STMT* claim = nullptr;
    MYSQL_STMT* touch = nullptr;
    unsigned lastErrno = 0;
    std::string lastError;

    ~Conn() {
        for (MYSQL_STMT* s : {find, insert, claim, touch})
            if (s) mysql_stmt_close(s);
        if (m) mysql_close(m);
    }
    bool StmtFail(MYSQL_STMT* s, const char* what) {
        lastErrno = mysql_stmt_errno(s);
        lastError = std::string(what) + " — " + mysql_stmt_error(s) + " (" + std::to_string(lastErrno) + ")";
        return false;
    }
    bool Prepare(MYSQL_STMT*& s, const char* sql) {
        s = mysql_stmt_init(m);
        if (!s) {
            lastErrno = mysql_errno(m);
            lastError = "문장 만들기";
            return false;
        }
        if (mysql_stmt_prepare(s, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) return StmtFail(s, sql);
        return true;
    }
};

CMySqlAccountStore::CMySqlAccountStore(db::DbConfig cfg) : cfg_(std::move(cfg)) {}

CMySqlAccountStore::~CMySqlAccountStore() = default;

void CMySqlAccountStore::ThreadDone() { mysql_thread_end(); }

bool CMySqlAccountStore::Open(std::string& err) {
    c_.reset();
    if (!db::LibInit()) {
        err = lastError_ = "mysql_library_init 실패";
        return false;
    }
    auto c = std::make_unique<Conn>();
    bool connectFailed = false;
    c->m = db::OpenChecked(cfg_, err, connectFailed);
    if (!c->m) {
        lastError_ = err;
        return false;
    }
    if (!c->Prepare(c->find, kFind) || !c->Prepare(c->insert, kInsert) || !c->Prepare(c->claim, kClaim) || !c->Prepare(c->touch, kTouch)) {
        err = lastError_ = "준비된 문장 — " + c->lastError + "(표 모양 · nox_auth 권한이 SQL 파일과 다른가)";
        return false;
    }
    c_ = std::move(c);
    return true;
}

template <class F>
bool CMySqlAccountStore::Retry(F&& f) {
    if (!c_) {   // 앞서 다시 붙기에 실패해 연결이 없음 — 이번에 다시
        std::string e;
        if (!Open(e)) return false;
    }
    if (f(*c_)) return true;
    lastError_ = c_->lastError;
    if (!db::IsLost(c_->lastErrno)) return false;
    ++retries_;
    std::string e;
    if (!Open(e)) return false;
    if (f(*c_)) return true;
    lastError_ = c_->lastError;
    return false;
}

FindResult CMySqlAccountStore::Find(const std::string& name, AccountRow& out) {
    bool found = false;
    bool broken = false;
    const bool ok = Retry([&](Conn& c) {
        MYSQL_BIND p[1] = {};
        unsigned long nameLen = 0;
        db::Bin(p[0], name, nameLen);
        uint64_t id = 0;
        char salt[64], hash[64];
        unsigned long saltLen = 0, hashLen = 0;
        uint32_t iter = 0;
        bool nulls[4] = {};
        MYSQL_BIND r[4] = {};
        db::U64(r[0], id);
        r[1].buffer_type = MYSQL_TYPE_BLOB;
        r[1].buffer = salt;
        r[1].buffer_length = sizeof(salt);
        r[1].length = &saltLen;
        r[2].buffer_type = MYSQL_TYPE_BLOB;
        r[2].buffer = hash;
        r[2].buffer_length = sizeof(hash);
        r[2].length = &hashLen;
        db::U32(r[3], iter);
        for (int i = 0; i < 4; ++i) r[i].is_null = &nulls[i];
        if (mysql_stmt_bind_param(c.find, p) != 0 || mysql_stmt_bind_result(c.find, r) != 0) return c.StmtFail(c.find, "계정 찾기 묶기");
        if (mysql_stmt_execute(c.find) != 0) return c.StmtFail(c.find, "계정 찾기");
        if (mysql_stmt_store_result(c.find) != 0) return c.StmtFail(c.find, "계정 찾기 받기");
        const int f = mysql_stmt_fetch(c.find);
        mysql_stmt_free_result(c.find);
        if (f == MYSQL_NO_DATA) return found = false, true;
        if (f != 0 && f != MYSQL_DATA_TRUNCATED) return c.StmtFail(c.find, "계정 찾기 꺼내기");
        found = true;
        out = AccountRow{};
        out.id = id;
        out.hasPassword = !nulls[2];
        if (out.hasPassword) {   // 칸 크기가 규칙과 다르면 행이 깨진 것 — 로그인을 막고 기록(맞는 척 하지 않음)
            if (nulls[1] || nulls[3] || saltLen != kPasswordSaltSize || hashLen != kPasswordHashSize || f == MYSQL_DATA_TRUNCATED) {
                broken = true;
                c.lastErrno = 0;
                c.lastError = "계정 " + std::to_string(id) + " 의 암호 칸 크기가 다름(소금 " + std::to_string(saltLen) + " · 해시 " +
                              std::to_string(hashLen) + ")";
                return false;
            }
            std::memcpy(out.password.salt.data(), salt, kPasswordSaltSize);
            std::memcpy(out.password.hash.data(), hash, kPasswordHashSize);
            out.password.iterations = iter;
        }
        return true;
    });
    if (!ok || broken) return FindResult::Failed;
    return found ? FindResult::Found : FindResult::NotFound;
}

InsertResult CMySqlAccountStore::Insert(const std::string& name, const PasswordRecord& pw, uint32_t nowUnix, uint64_t& id) {
    const std::string salt(reinterpret_cast<const char*>(pw.salt.data()), pw.salt.size());
    const std::string hash(reinterpret_cast<const char*>(pw.hash.data()), pw.hash.size());
    const uint32_t iter = pw.iterations;
    bool dup = false;
    const bool ok = Retry([&](Conn& c) {
        MYSQL_BIND p[5] = {};
        unsigned long l0 = 0, l1 = 0, l2 = 0;
        db::Bin(p[0], name, l0);
        db::Bin(p[1], salt, l1);
        db::Bin(p[2], hash, l2);
        db::U32(p[3], iter);
        db::U32(p[4], nowUnix);
        if (mysql_stmt_bind_param(c.insert, p) != 0) return c.StmtFail(c.insert, "계정 넣기 묶기");
        if (mysql_stmt_execute(c.insert) != 0) {
            if (mysql_stmt_errno(c.insert) == ER_DUP_ENTRY) return dup = true, true;
            return c.StmtFail(c.insert, "계정 넣기");
        }
        dup = false;
        id = mysql_stmt_insert_id(c.insert);
        return true;
    });
    if (!ok) return InsertResult::Failed;
    if (!dup) return InsertResult::Created;
    // 같은 이름이 있음 — 암호 없는 옛 행이면 그 행에 넣는다(`AccountStore.h`)
    my_ulonglong changed = 0;
    const bool claimed = Retry([&](Conn& c) {
        MYSQL_BIND p[4] = {};
        unsigned long l0 = 0, l1 = 0, l3 = 0;
        db::Bin(p[0], salt, l0);
        db::Bin(p[1], hash, l1);
        db::U32(p[2], iter);
        db::Bin(p[3], name, l3);
        if (mysql_stmt_bind_param(c.claim, p) != 0) return c.StmtFail(c.claim, "옛 행에 암호 묶기");
        if (mysql_stmt_execute(c.claim) != 0) return c.StmtFail(c.claim, "옛 행에 암호");
        changed = mysql_stmt_affected_rows(c.claim);
        return true;
    });
    if (!claimed) return InsertResult::Failed;
    if (changed != 1) return InsertResult::Duplicate;
    AccountRow row;
    if (Find(name, row) != FindResult::Found) return InsertResult::Failed;
    id = row.id;
    return InsertResult::Claimed;
}

bool CMySqlAccountStore::TouchLogin(uint64_t id, uint32_t nowUnix) {
    return Retry([&](Conn& c) {
        MYSQL_BIND p[2] = {};
        db::U32(p[0], nowUnix);
        db::U64(p[1], id);
        if (mysql_stmt_bind_param(c.touch, p) != 0) return c.StmtFail(c.touch, "마지막 로그인 묶기");
        if (mysql_stmt_execute(c.touch) != 0) return c.StmtFail(c.touch, "마지막 로그인");
        return true;
    });
}

}  // namespace nox::auth
