// AccountStore.h — 로그인 서버의 계정 저장소 경계(Docs/LOGIN_AUTH_DESIGN.md 「클래스 설계」 nox_auth).
//
// 왜 — 로그인 서버의 판단(`CAuthService`)은 DB 를 모른다. 이 경계만 안다 → 시험은 메모리 가짜로 갈래를 전수로 돈다.
//   · 막는 꼴 — 부르는 일꾼 스레드가 DB 를 기다린다(로비와 다름: 로비는 한 스레드라 못 기다림 `DbWorker.h:3`).
//     일꾼 하나에 저장소 하나(MySQL 연결은 스레드마다 — `MySqlAccountStore.h`)
//   · 이름은 바이트 그대로 같음으로 찾는다(로비 · DB 와 같은 잣대 — `LobbyStore.h:9`)
//   · 암호 없는 옛 행(로비가 아이디만으로 만든 계정 — 7단계 결정 2)은 「암호 없음」으로 돌려주고, 그 이름으로 가입하면
//     새 행을 안 만들고 그 행에 암호를 넣는다(`Claimed` — 판 기록의 계정 번호가 그대로 이어짐)
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "nox/AccountRules.h"

namespace nox::auth {

struct AccountRow {
    uint64_t id = 0;
    bool hasPassword = false;   // false = 암호 칸이 NULL(아이디만 시절의 행)
    PasswordRecord password;
};

enum class FindResult : uint8_t { Found, NotFound, Failed, kCount };
enum class InsertResult : uint8_t {
    Created,     // 새 행
    Claimed,     // 암호 없는 옛 행에 암호를 넣음
    Duplicate,   // 암호 있는 같은 이름이 이미 있음
    Failed,      // DB 가 안 됨
    kCount
};

class IAccountStore {
public:
    virtual ~IAccountStore() = default;
    virtual FindResult Find(const std::string& name, AccountRow& out) = 0;
    virtual InsertResult Insert(const std::string& name, const PasswordRecord& pw, uint32_t nowUnix, uint64_t& id) = 0;
    virtual bool TouchLogin(uint64_t id, uint32_t nowUnix) = 0;   // 마지막 로그인
};

// 메모리 가짜(시험) — 실패 손잡이 · 옛 행 넣기
class CMemoryAccountStore final : public IAccountStore {
public:
    FindResult Find(const std::string& name, AccountRow& out) override;
    InsertResult Insert(const std::string& name, const PasswordRecord& pw, uint32_t nowUnix, uint64_t& id) override;
    bool TouchLogin(uint64_t id, uint32_t nowUnix) override;

    void SetFail(bool on) { fail_ = on; }
    uint64_t AddLegacy(const std::string& name);   // 암호 없는 옛 행
    size_t Count() const { return rows_.size(); }
    uint32_t LastLogin(uint64_t id) const;         // 0 = 없음 · 안 들어옴

private:
    struct Row {
        AccountRow row;
        uint32_t lastLogin = 0;
    };
    std::map<std::string, Row> rows_;
    uint64_t next_ = 1;
    bool fail_ = false;
};

}  // namespace nox::auth
