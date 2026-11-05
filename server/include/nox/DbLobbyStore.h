// DbLobbyStore.h — 로비 저장소의 DB 쪽(`ILobbyStore`) — 일을 쓰기 · 읽기 워커로 나눈다(`Docs/RECORD_SEARCH_PLAN.md` 조각 1-나 · 11-04 사용자 결정).
//
// 왜 — 워커 하나에 다 줄을 세우면 남의 이름으로 묻는 전적 검색이 판 저장 답을 늦춘다(읽기가 잠금에 멈춘 1.5초 동안 저장 답 0 · `db_test` ⑬ 앞 측정).
//   판 결과가 밀리면 게임 서버가 다시 보내므로 쓰기 줄에는 판 저장만 둔다
//   · `SaveMatch` · `GetRating` = 쓰기 — 로그인 공지 점수는 앞서 넣은 판 저장 뒤에 읽혀야 새 점수가 보인다(순서)
//   · `GetMatchHistory` = 읽기 — 저장과 갈라졌으므로 읽기 하나를 한 시점으로 묶는다(`CDbWorker::ExecHistory`)
//   · 시작 검사(표 버전 · 문장 준비)는 두 연결이 각자 지난다 · 하나라도 실패면 `Start` 실패(로비가 안 뜬다) · 다시 붙기는 워커마다 따로
//   · 끝낼 때 읽기 먼저 — 남은 조회는 버린다(잃어도 다시 물으면 된다) · 쓰기는 남은 판을 다 한다(판 결과를 잃지 않게)
//   · 답을 합치는 차례는 상관없다 — 읽기 답은 로비 상태를 안 고친다
//   · ★ 11-05 — 깨우기는 두 워커가 같이 쓴다(어느 쪽 답에 깨어나든 `TakeAnswers` 가 둘 다 꺼낸다)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nox/DbWorker.h"
#include "nox/LobbyStore.h"

namespace nox::db {

class CDbLobbyStore final : public wol::ILobbyStore {
public:
    explicit CDbLobbyStore(const DbConfig& cfg);
    ~CDbLobbyStore() override;

    bool Start(std::string& err);   // 실패면 `err`(로비가 안 뜬다)
    void Stop();                    // 두 번 불러도 됨

    void SaveMatch(uint32_t req, const wol::MatchSource& src, const MatchRecord& rec, const std::string& rawJson) override;
    void GetRating(uint32_t req, const std::string& account) override;
    void GetMatchHistory(uint32_t req, const std::string& account) override;
    void TakeAnswers(std::vector<wol::StoreAnswer>& out) override;
    void SetWake(std::function<void()> wake) override;

    DbStats Stats() const;   // 두 워커의 합(붙음 · 못 붙음 · 끊겨 다시 = 합 · 한 번에 꺼낸 일 = 큰 쪽 · 마지막 오류 = 쓰기 먼저)
    unsigned long WriterConnectionId() const { return writer_.ConnectionId(); }   // 시험이 KILL 로 끊어 보려고
    unsigned long ReaderConnectionId() const { return reader_.ConnectionId(); }

private:
    CDbWorker writer_;
    CDbWorker reader_;
};

}  // namespace nox::db
