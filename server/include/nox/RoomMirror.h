// RoomMirror.h — 로비의 방 거울(MO 6단계 v2 작업 3a-2 — `Docs/LOBBY_SPLIT_DESIGN.md` 「원칙」).
//
// 왜 — v2 에서 방은 게임 서버(`nox_server`)에 산다. 로비(`nox_lobby`)는 게임 서버들이 연결(`LobbyLink.h`)로 알려 준 방으로 목록을 그린다.
//   · 방 목록의 정본은 게임 서버의 돌고 있는 방 — 거울은 알림(`ROOM` · `ROOM_GONE`)으로만 바뀐다. 어긋나면 입장 최종 판단은 게임 서버(v3 원칙 그대로)
//   · 맵 목록의 주인은 게임 서버 — 새 방은 만든 사람만 넘기고(`CREATE`) 게임 서버가 맵을 골라 만든 뒤 답한다(`CREATED`)
//   · 연결은 게임 서버가 붙는다. 붙자마자 일회용 값(`HELLO`) → 같은 설정 키를 쥐었는지 서명(`AUTH`) → 등록(`REGISTER`) 차례만 받는다
//   · 게임 서버마다 토큰 키가 따로(`GameServerTokenKey` — 등록 줄 값 그대로) — 그 게임 서버 방의 입장 토큰은 그 키로 서명한다
//   · 연결이 끊기면 그 게임 서버의 방을 목록에서 내리고 기다리던 새 방 요청은 곧바로 실패(로비 한도까지 안 기다림). 다시 붙어 등록하면 돌려놓는다
//
//   · ★ 7단계 — 살아 있음(`PING` · `LobbyLink.h`). 전원째 꺼진 게임 서버는 끊김 신호가 안 와 등록된 채로 남는다 — 그 방이 목록에 남고
//     다시 켠 그 게임 서버는 「같은 주소」로 거절됐다(6단계 미결). 받은 줄이 `kLinkDeadMs` 동안 없으면 BYE 뒤 끊고 위 끊김 길로 간다
//
//   · ★ 7단계 조각 5 — 판 결과(`RESULT`)를 읽어(`MatchRecord.h`) 저장소(`ILobbyStore` — `AttachStore`)에 넣고 답이 오면 그 연결에
//     `SAVED <판 번호> ok|dup`. 못 읽으면 곧바로 `SAVED … bad`(판 번호도 못 읽으면 답할 수 없어 셈만) · 저장소가 못 넣으면 답을 안 한다
//     (게임 서버가 30초 뒤 다시 보낸다 — `LobbyClient.h` `kResultResendMs`) · 겹침은 저장소가 (등록 주소 · 포트 · 판 번호)로 가른다
//
// 로비 규약(`Lobby`)의 방 공급자(`ILobbyRooms`)다. 소켓은 모른다 — 연결 사건을 넣으면 보낼 줄을 `out` 에 덧붙인다.
//   시간은 살아 있음 몫만 — 시계(`SetClock` · 기본 `NowMs`)를 받고 소켓 루프가 자주(줄이 없어도) `Tick` 을 부른다.
//   새 방 요청(`RequestRoom`)은 로비가 클라 줄을 처리하는 도중에 부르므로 그 `CREATE` 줄은 쌓아 두고 `TakeOut` 으로 꺼낸다.
//   게임 서버의 답(`CREATED` · `CREATE_FAILED`)은 붙인 로비(`Attach`)에 곧바로 넘긴다 — 미룬 참가 답이 같은 `out` 에 실린다.
//   클라 접속과 연결 접속은 같은 TCP 줄 부품의 접속 번호를 쓴다(번호가 겹치지 않는다 — 부르는 쪽이 듣기 포트로 가른다).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nox/Clock.h"
#include "nox/Lobby.h"
#include "nox/LobbyLink.h"
#include "nox/LobbyStore.h"
#include "nox/MatchRecord.h"

namespace nox::wol {

// 거울의 셈 — 갈래마다
struct MirrorStats {
    unsigned long long linksOpened = 0;       // 연결 접속
    unsigned long long linksClosed = 0;       // 연결 끊김(어느 단계든)
    unsigned long long authOk = 0;            // 확인 서명 맞음
    unsigned long long authFailed = 0;        // 확인 서명 틀림 · 첫 줄이 AUTH 아님(끊음)
    unsigned long long registered = 0;        // 등록 받음
    unsigned long long registerRejected = 0;  // 같은 주소 · 포트로 이미 등록됨(끊음)
    unsigned long long outOfOrder = 0;        // 등록 전 다른 줄(끊음)
    unsigned long long badLines = 0;          // 등록 뒤 못 읽는 줄 · 게임 서버가 보낼 리 없는 줄(버림 — 연결은 둠)
    unsigned long long roomLines = 0;         // ROOM
    unsigned long long roomGoneLines = 0;     // ROOM_GONE
    unsigned long long roomGoneUnknown = 0;   // 거울에 없는 방의 ROOM_GONE
    unsigned long long noServer = 0;          // 새 방 — 등록된 게임 서버 없음(곧바로 실패)
    unsigned long long createsSent = 0;       // CREATE 보냄
    unsigned long long createdReplies = 0;    // CREATED → 로비에
    unsigned long long createFailedReplies = 0; // CREATE_FAILED → 로비에
    unsigned long long createdNoRoom = 0;     // CREATED 인데 그 방의 ROOM 이 먼저 안 옴(실패로 넘김)
    unsigned long long unknownReplies = 0;    // 이 게임 서버에 안 물은 요청의 답(버림)
    unsigned long long failedOnClose = 0;     // 연결이 끊겨 실패로 넘긴 기다리던 요청
    unsigned long long cancelled = 0;         // ★ 작업 3c — 로비가 접은 요청(시간 초과 · 클라 끊김)을 기다림 장부에서 지움
    unsigned long long results = 0;           // RESULT(판 결과 — 7단계 전엔 한 줄 찍고 셈)
    // ★ 7단계 — 살아 있음
    unsigned long long pingsIn = 0;           // PING 받음(등록 뒤)
    unsigned long long pingsOut = 0;          // PING 보냄(등록된 게임 서버에 `kLinkPingEveryMs` 마다)
    unsigned long long deadLinks = 0;         // 받은 줄이 `kLinkDeadMs` 동안 없어 끊음(어느 단계든 — 등록 전 말없는 접속도)
    // ★ 7단계 조각 5 — 판 저장(RESULT 하나는 저장 요청 · 못 읽음 · 저장소 없음 중 하나)
    unsigned long long saveRequests = 0;      // 저장소에 판 저장 요청
    unsigned long long savedOk = 0;           // 답 Saved → SAVED ok
    unsigned long long savedDup = 0;          // 답 Duplicate → SAVED dup
    unsigned long long savedBad = 0;          // 못 읽음 · 판 번호는 읽음 → 곧바로 SAVED bad
    unsigned long long saveFailed = 0;        // 답 Failed — SAVED 안 보냄(게임 서버가 다시)
    unsigned long long saveConnGone = 0;      // 답이 왔는데 그 연결이 이미 끊김(버림 — 게임 서버가 다시 붙으면 다시 보내 dup)
    unsigned long long saveAnswerUnknown = 0; // 묻지 않은 요청 번호의 답(버림)
    unsigned long long noStore = 0;           // 저장소를 안 붙인 거울(시험 · 옛 꼴) — 기록만
    unsigned long long unreadable[static_cast<size_t>(MatchJsonParse::kCount)] = {};   // 못 읽은 판 결과 — 갈래마다(Ok 칸은 안 씀)
    unsigned long long unreadableNoMatch = 0; // 그중 판 번호도 못 읽음(답 없음)
    unsigned long long UnreadableTotal() const {
        unsigned long long n = 0;
        for (const unsigned long long v : unreadable) n += v;
        return n;
    }
};

class RoomMirror final : public ILobbyRooms {
public:
    // `tokenKey` = 설정 파일의 토큰 키(`[token] key`) — 연결 확인 키 · 게임 서버 토큰 키를 이것에서 뽑는다
    explicit RoomMirror(const JoinKey& tokenKey);
    // 게임 서버의 새 방 답을 넘길 로비(로비가 이 거울을 방 공급자로 쥐므로 만든 뒤에 붙인다)
    void Attach(Lobby& lobby) { lobby_ = &lobby; }
    // ★ 7단계 — 살아 있음의 시계(ms · 32비트로 돈다). 시험이 정해 보려고 — 붙기 전에 건다
    void SetClock(std::function<uint32_t()> nowMs) { nowMs_ = std::move(nowMs); }

    void OnLinkOpen(int conn, std::vector<Out>& out);                         // HELLO 를 보낸다
    void OnLinkLine(int conn, std::string_view line, std::vector<Out>& out);
    void OnLinkClose(int conn, std::vector<Out>& out);                        // 그 게임 서버의 방 내림 · 기다리던 요청 실패(★ 10-20 — 클라에게 거절 답)
    // ★ 7단계 — 등록된 게임 서버에 PING · 받은 줄이 한도 동안 없는 접속은 BYE 뒤 끊기(끊김 사건은 소켓이 `OnLinkClose` 로)
    void Tick(std::vector<Out>& out);
    // ★ 7단계 조각 5 — 판 결과를 넣을 저장소(거울보다 오래 산다 · 안 붙이면 판 결과는 기록만) · 저장소 답 하나(갈래 SaveMatch 만)
    void AttachStore(ILobbyStore& store) { store_ = &store; }
    void OnStoreAnswer(const StoreAnswer& a, std::vector<Out>& out);
    size_t PendingSaves() const { return saves_.size(); }
    void TakeOut(std::vector<Out>& out);                                      // `RequestRoom` 이 쌓은 CREATE 줄
    std::vector<std::string> TakeLog();
    std::vector<std::string> TakeResults();                                   // RESULT 의 JSON(받은 차례)

    // ILobbyRooms
    std::vector<LobbyRoom> LobbyRooms() const override;
    RoomRequest RequestRoom(uint32_t req, const std::string& creator, LobbyRoom& out, std::string& why) override;
    void CancelRoom(uint32_t req) override;   // ★ 작업 3c — 로비가 접은 요청을 기다림 장부에서 지운다

    const MirrorStats& Stats() const { return stats_; }
    size_t ServerCount() const;   // 등록된 게임 서버 수
    size_t RoomCount() const;     // 거울의 방 수(모든 게임 서버)

private:
    enum class Stage : uint8_t { WaitAuth, WaitRegister, Registered, Closing };   // Closing = BYE 를 보내고 끊김을 기다림(그 뒤 줄은 버림)
    struct Server {
        Stage stage = Stage::WaitAuth;
        std::string nonce;               // 보낸 일회용 값(16진)
        uint32_t epoch = 0;              // 등록 — 실행 번호
        uint32_t ip = 0;                 // 등록 — 클라가 붙을 주소(0 = 로비 쪽 주소)
        uint16_t port = 0;               // 등록 — UDP 포트
        JoinKey tokenKey{};              // 등록 값으로 뽑은 그 게임 서버의 토큰 키
        std::map<uint32_t, LinkRoom> rooms;
        std::set<uint32_t> asked;        // 이 게임 서버에 물은 새 방 요청
        bool registered = false;         // ★ 7단계 — 등록한 적 있음(무소식 끊기는 등록된 게임 서버에도 BYE 를 보낸다 — 끊김 기록 · 이름에 주소를 남긴다)
        uint32_t lastRecvMs = 0;         // ★ 7단계 — 마지막으로 줄을 받은 때(붙은 때부터)
        uint32_t nextPingMs = 0;         // ★ 7단계 — 다음 PING 시각
    };

    void Bye(int conn, const std::string& why, std::vector<Out>& out);   // BYE 줄 뒤 끊기
    std::string Name(int conn, const Server& s) const;                    // 기록용 「c5 10.0.0.5:18590」
    LobbyRoom ToLobbyRoom(const Server& s, const LinkRoom& r) const;

    JoinKey tokenKey_;
    JoinKey authKey_;
    Lobby* lobby_ = nullptr;
    std::function<uint32_t()> nowMs_ = NowMs;
    ILobbyStore* store_ = nullptr;
    struct PendingSave {
        int conn = 0;          // 판 결과를 보낸 연결(접속 번호는 다시 안 쓴다 — 끊겼으면 그 게임 서버의 그 연결이 아니다)
        std::string match;     // SAVED 에 실을 판 번호
    };
    std::map<uint32_t, PendingSave> saves_;   // 저장 요청 번호 → 답할 곳
    uint32_t nextSaveReq_ = 1;
    std::map<int, Server> servers_;   // 연결 접속 번호 → 게임 서버(차례 = 붙은 차례)
    std::vector<Out> pendingOut_;
    std::vector<std::string> log_;
    std::vector<std::string> results_;
    MirrorStats stats_;
};

}  // namespace nox::wol
