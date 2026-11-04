// RecordDesk.h — 전적 창구: 대전 기록 · 전적 검색 물음을 받아 저장소에 묻고 답을 로비 줄로 돌려준다(`Docs/RECORD_SEARCH_PLAN.md` 조각 2).
//
// 왜 `Lobby` 에서 뗐나 — `Lobby` 는 로그인 · 채팅 · 방 · 랭크 · 수락 · 점수까지 한 클래스라 책임이 섞였다. 판 결과 쪽을 `RoomMirror` 로 뗀 것과 같은 꼴
//   · 맡는 것 = 물음 접수 · 이름 검사 · 겹침 막기 · 답 → 줄 · 셈 · 끊긴 물음 정리
//   · 모르는 것 = 접속 상태 · 소켓 · DB. `Lobby` 를 부르지 않는다 — 로그인 확인과 끊김 알림은 `Lobby` 가 하고, 조립은 `lobby_main`
//
// 물음 둘
//   · `AskMine`(`NOXRANK RECORD`) = 로그인으로 확인된 내 이름. 이름 검사도 「없는 사용자」 답도 없다
//       — `--no-auth` 로그인은 계정 행이 없어도 지금처럼 빈 기록을 봐야 하고, 옛 DLL 은 `RECNONE` 을 모른다
//   · `AskName`(`NOXRANK RECORD <이름>`) = 남의 이름(전적 검색). 계정 이름 규칙(`CheckAccountName`)에 안 맞으면 저장소에 안 묻고 `RECNONE` · 저장소가 계정이 없다고 하면 `RECNONE`
//   · 물음이 겹치면 말없이 버린다(접속당 하나 — 앞 답이 곧 간다) · 줄 꼴은 `MatchHistory.h` 머리
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nox/Lobby.h"   // `Out` 만(`RoomMirror.h` 와 같은 꼴)
#include "nox/LobbyStore.h"

namespace nox::wol {

// `RECNONE` 이 되돌려 주는 이름의 한도 — 정품 칸이 9자까지 받으므로(`Docs/RECORD_SEARCH_PLAN.md` 2절) 한글이어도 CP949 18바이트. DLL 은 물은 이름과 답의 이름을 대조하므로
//   그 안에선 그대로 돌려줘야 하고, 그 밖은 DLL 이 아니므로 잘라도 된다. 18 보다 크고 줄 크기를 묶으려는 값(32 자체에 근거는 없다)
constexpr size_t kRecordEchoMaxBytes = 32;

// 물음 하나 = 줄로 답 · 못 읽음(RECFAIL) · 없는 사용자(RECNONE) · 끊김 중 하나(합 = 물음 · 끝날 때 남은 것 빼고)
struct RecordDeskStats {
    unsigned long long recordAsked = 0;
    unsigned long long recordSent = 0;
    unsigned long long recordFailed = 0;
    unsigned long long recordNoUser = 0;   // 계정이 없음 · 이름 형식 위반(둘 다 `RECNONE`)
    unsigned long long recordClosed = 0;
    unsigned long long recordLate = 0;     // 끝난 물음의 답 · 안 물은 답(버림)
    unsigned long long recordBusy = 0;     // 답을 기다리는 중 또 물음(묻지 않고 버림)
    unsigned long long RecordTotal() const { return recordSent + recordFailed + recordNoUser + recordClosed; }
};

class RecordDesk {
public:
    void AttachStore(ILobbyStore* store) { store_ = store; }   // 없으면 물음마다 RECFAIL · 창구보다 오래 살아야 한다

    void AskMine(int conn, const std::string& nick, std::vector<Out>& out);
    void AskName(int conn, const std::string& name, std::vector<Out>& out);
    void OnClose(int conn);   // 기다리던 답은 늦은 답으로 버린다
    void OnAnswer(const StoreAnswer& a, std::vector<Out>& out);   // 저장소 답 History(소켓 루프가 꺼내 넘긴다)

    size_t Pending() const { return pending_.size(); }
    const RecordDeskStats& Stats() const { return stats_; }
    std::vector<std::string> TakeLog();

private:
    struct Asking {
        int conn = 0;
        std::string name;
        bool mine = false;
    };
    void Ask(int conn, const std::string& name, bool mine, std::vector<Out>& out);
    void AnswerNoUser(int conn, const std::string& name, std::vector<Out>& out);

    ILobbyStore* store_ = nullptr;
    std::map<uint32_t, Asking> pending_;   // 저장소 요청 번호 → 물음
    std::map<int, uint32_t> reqOfConn_;    // 접속 → 기다리는 요청 번호(겹침 · 끊김)
    uint32_t nextReq_ = 1;
    RecordDeskStats stats_;
    std::vector<std::string> log_;
};

}  // namespace nox::wol
