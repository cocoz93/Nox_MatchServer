// MySqlAccountStore.h — 로그인 서버의 MySQL 계정 저장소(`IAccountStore` · Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」).
//
// 일꾼 스레드 하나에 하나 — 그 스레드에서 만들고(`Open`) 쓰고 닫는다(MySQL 연결은 스레드끼리 나눠 쓰지 않는다).
//   · DB 사용자 `nox_auth` = accounts 의 SELECT · INSERT · UPDATE 만(`config/auth_setup.sql` · 표를 못 만듦)
//   · 뜰 때 표 버전(`DbConfig.h` kSchemaVersion)과 준비된 문장 넷을 본다 — 다르면 `Open` 실패 → 로그인 서버가 안 뜬다
//   · 끊기면(`IsLost`) 그 자리에서 한 번 다시 붙어 같은 일을 다시 한다(로비 DB 워커와 같은 꼴 `DbWorker.h:9`)
// MySQL 머리 파일은 .cpp 에만
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "AccountStore.h"
#include "nox/DbConfig.h"

namespace nox::auth {

class CMySqlAccountStore final : public IAccountStore {
public:
    explicit CMySqlAccountStore(db::DbConfig cfg);
    ~CMySqlAccountStore() override;
    CMySqlAccountStore(const CMySqlAccountStore&) = delete;
    CMySqlAccountStore& operator=(const CMySqlAccountStore&) = delete;

    bool Open(std::string& err);   // 붙기 · 표 버전 · 준비된 문장

    FindResult Find(const std::string& name, AccountRow& out) override;
    InsertResult Insert(const std::string& name, const PasswordRecord& pw, uint32_t nowUnix, uint64_t& id) override;
    bool TouchLogin(uint64_t id, uint32_t nowUnix) override;

    // 일꾼 스레드가 끝날 때 한 번(mysql_thread_end — 그 스레드의 저장소를 다 닫은 뒤). 붙기(mysql_init)가 스레드 준비를 스스로 한다
    static void ThreadDone();

    unsigned long long Retries() const { return retries_; }   // 끊김을 만나 다시 붙은 수
    const std::string& LastError() const { return lastError_; }   // 비밀번호는 안 실린다

private:
    struct Conn;   // MYSQL* · 준비된 문장들(.cpp 에만)
    template <class F> bool Retry(F&& f);   // 한 번 하고 끊김이면 다시 붙어 한 번 더

    db::DbConfig cfg_;
    std::unique_ptr<Conn> c_;
    unsigned long long retries_ = 0;
    std::string lastError_;
};

}  // namespace nox::auth
