// DbWorker.h — 로비의 DB 워커(MO 7단계 조각 7 — `Docs/ACCOUNT_DB_DESIGN.md` 「누가 무엇을 맡나」 `CDbWorker`).
//
// 왜 — 계정 · 판 기록의 정본은 MySQL(결정 1). 로비 스레드는 DB 를 기다리지 않는다 — 요청은 큐에 넣고 곧바로 돌아오며,
//   전용 스레드 하나가 MySQL 연결 하나로 차례대로 실행해 답 큐에 넣는다. 로비 루프가 바퀴마다 꺼낸다(`ILobbyStore` — 메모리 가짜와 같은 경계).
//   꼴은 MMO_Zone DB 워커(`MMO_Zone/MMOServer/MMOServer/DB/DBWorker.h` — 전용 스레드 · 큐 통째로 꺼내기 · 준비된 문장 · 끊기면 다시 붙기)를 따른다
//   · 연결 하나 · 스레드 하나 — 로비 하나 · 소수 시험이라 더 둘 까닭이 아직 없다(로그인과 판 저장이 같은 차례를 탄다)
//   · 서버 계정은 표를 못 만든다(정할 것 a) — 뜰 때 `schema_version` 이 `kSchemaVersion` 인지와 준비된 문장이 모두 준비되는지로 표 모양을 본다.
//     못 붙거나 다르면 `Start` 가 실패 — 로비가 안 뜬다(토큰 키가 없을 때와 같은 꼴)
//   · 끊기면(2006 · 2013) 그 자리에서 한 번 다시 붙어 같은 일을 다시 한다. 그래도 안 되면 그 일은 실패 답(로그인 → 실패 길 · 판 → 게임 서버가 다시)
//   · 판은 한 트랜잭션(matches 한 행 + match_players) · 같은 (게임 서버 · 판 번호)는 열쇠 겹침(1062)으로 Duplicate
//   · 끝낼 때(`Stop`) 큐에 남은 일을 다 하고 닫는다(판 결과를 잃지 않게)
// MySQL 머리 파일은 이 파일 밖(.cpp)에만 — 부르는 쪽(로비 실행 파일 · 시험)은 MySQL 을 모른다
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nox/LobbyStore.h"
#include "nox/MatchRecord.h"

namespace nox::db {

constexpr uint32_t kSchemaVersion = 1;   // `tools/db/001_init.sql` 의 schema_version — 표를 바꾸면 SQL 파일과 같이 올린다

struct DbConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user;
    std::string password;            // 기록에 안 찍는다
    std::string database;
    unsigned connectTimeoutSec = 2;  // 근거 없는 값 둘 — 로그인 답 한도(`kLoginCheckTimeoutMs` 5초)보다 짧게(10-19 검사 지적 7)
    unsigned rwTimeoutSec = 3;
};

// 셈 — 요청 하나는 아래 갈래 중 하나로 끝난다(시험은 개수로 본다)
struct DbStats {
    unsigned long long requests = 0;        // 받은 요청(로그인 + 판)
    unsigned long long loginOk = 0;         // 찾음 · 만듦
    unsigned long long loginCreated = 0;    // 그중 만듦
    unsigned long long loginFailed = 0;     // 다시 붙어도 안 됨 · 그 밖 오류
    unsigned long long saved = 0;
    unsigned long long duplicates = 0;
    unsigned long long invalid = 0;         // 칸에 안 맞음(`MatchFitsStore`) — DB 에 안 감
    unsigned long long saveFailed = 0;
    unsigned long long connects = 0;        // 붙음(처음 + 다시)
    unsigned long long connectFailed = 0;
    unsigned long long retries = 0;         // 끊김을 만나 다시 붙고 같은 일을 다시 함
    size_t queueMax = 0;                    // 한 번에 꺼낸 일의 최대
    std::string lastError;                  // 마지막 오류 글(기록용 — 비밀번호는 안 실린다)
};

class CDbWorker final : public wol::ILobbyStore {
public:
    explicit CDbWorker(DbConfig cfg);
    ~CDbWorker() override;
    CDbWorker(const CDbWorker&) = delete;
    CDbWorker& operator=(const CDbWorker&) = delete;

    // 부르는 스레드에서 붙기 · 표 버전 · 준비된 문장(빠른 실패) → 워커 스레드. 실패면 `err`(로비가 안 뜬다)
    bool Start(std::string& err);
    // 남은 일을 다 하고 닫는다(두 번 불러도 됨)
    void Stop();

    // ILobbyStore — 요청은 곧바로 돌아온다 · 답은 워커가 넣고 로비가 꺼낸다
    void CheckLogin(uint32_t req, const std::string& name) override;
    void SaveMatch(uint32_t req, const wol::MatchSource& src, const MatchRecord& rec, const std::string& rawJson) override;
    void TakeAnswers(std::vector<wol::StoreAnswer>& out) override;

    DbStats Stats() const;
    unsigned long ConnectionId() const;   // 지금 연결의 MySQL 스레드 번호(시험이 KILL 로 끊어 보려고 · 0 = 없음)

private:
    struct Job {
        wol::StoreAnswerKind kind = wol::StoreAnswerKind::kCount;
        uint32_t req = 0;
        std::string name;
        wol::MatchSource src;
        MatchRecord rec;
        std::string raw;
    };
    struct Conn;   // MYSQL* · 준비된 문장들(.cpp 에만)

    void Run();
    wol::StoreAnswer Exec(const Job& j);
    bool Connect(std::string& err);    // `conn_` 을 새로(실패면 비움)
    void Drop();                       // 연결 · 문장 닫기
    void NoteError(const std::string& e);

    DbConfig cfg_;
    std::unique_ptr<Conn> conn_;       // 워커 스레드만 만진다(Start 전 · Stop 뒤엔 부르는 스레드)
    std::thread thread_;
    mutable std::mutex mu_;            // 큐 · 답 · 셈
    std::condition_variable cv_;
    std::vector<Job> queue_;
    std::vector<wol::StoreAnswer> answers_;
    DbStats stats_;
    unsigned long connId_ = 0;
    bool stop_ = false;
    bool started_ = false;
};

}  // namespace nox::db
