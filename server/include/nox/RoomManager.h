// RoomManager.h — 방 여럿을 게임 스레드 하나에서 돌린다(`Docs/ROOMS_DESIGN.md` — MO 3단계 플랜 B).
//
// ★ 10-09 플랜 B 작업 6a — 틀. 방 관리자는 통신 층 이벤트와 게임(`nox::Server`) 사이에 선다.
//     · 이벤트 → 세션표의 방. 접속 시도(`JOIN_REQUESTED`)는 방으로 넘기지 않고 여기서 답한다
//     · 방마다 쌓인 통신 요청을 처리한 차례대로 모아 한 묶음으로 내준다(`SwapOutNetworkRequests`)
//     · 틱 — 방마다 시계를 따로 돌린다(`BeginTick` → `RunTick`). 게임 루프가 기다릴 다음 틱 시각은 방들 가운데 가장 이른 것
// ★ 10-09 플랜 B 작업 6b — 배정 · 거절 · 나감 · 방 이벤트 · 방 만들기/닫기.
//     · 나감 — 방이 내는 끊기 요청(`DISCONNECT_SESSION`)을 넘길 때 세션표에서 지운다(나가는 길이 모두 그 요청으로 모인다)
// ★ 10-10 토큰 작업 2 — 6b 의 「명단(이름 → 방) · 입장(`CLIENT_ACCEPT`)까지 `CONNECTED` 담아 두기」를 입장 토큰으로 바꿨다
//   (`Docs/TOKEN_DESIGN.md`). 로비는 클라의 캐릭터 이름을 모르므로 이름 열쇠는 로비와 이어지지 않는다.
//     · 토큰 방식(`SetJoinKey`) — 접속 시도는 꼬리의 토큰을 확인만 하고(서명·만료·보낸 IP·방) 받거나 `SERVER_ERROR` 사유 4,
//       `CONNECTED` 는 같은 토큰을 써 버리고(일회용) 그 방에 바로 넘긴다. 틀리면 끊는다 — 통신 층은 CONNECT 에 이미
//       접속 수락을 보냈다(`UdpServer.cpp` SERVER_CONNECT). 클라는 접속 시도와 CONNECT 를 다른 포트에서 보내므로 둘 다에 꼬리가 있다
//     · 열린 방식(`SetOpenEntry` — 토큰 검사 끄기) — 모두 방 0. 녹음 재생·대조와 시험 도구만 켠다(`CSyncHost` 가 켠다)
//     · 둘 다 안 정했으면 모두 거절한다(설정 하나 빠뜨린 서버가 아무나 받지 않게)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nox/JoinToken.h"
#include "nox/Server.h"
#include "nox/UdpServer.h"

namespace nox {

// 방 하나가 틱을 도는 데 쓴 시간(부하 — `CRoom::Load`). 방 워커를 둘지 가르는 근거(`ROOMS_DESIGN.md` 「대규모 확장 경로」)
struct CRoomLoad {
    unsigned long long tickRuns = 0;   // `RunTick` 부른 수
    double tickMsSum = 0.0;
    double tickMsMax = 0.0;
};

// 방 하나 — 원본 재현 게임(`nox::Server`) 하나를 든다. MO 규칙(대기/진행/끝 · 판 시작 · 결과)은 여기에 쌓인다(작업 7 · 5단계).
class CRoom {
public:
    explicit CRoom(int id) : _id(id) { _game.SetLogRoom(id); }   // ★ 10-12 작업 7b — 방에 든 게임의 줄 머리에 `[방 N]`
    CRoom(const CRoom&) = delete;
    CRoom& operator=(const CRoom&) = delete;

    int Id() const { return _id; }
    Server& Game() { return _game; }
    const Server& Game() const { return _game; }
    const CRoomLoad& Load() const { return _load; }
    void ResetLoad() { _load = CRoomLoad{}; }

private:
    friend class CRoomManager;
    int _id;
    Server _game;
    int _ticks = 0;            // 이번 `BeginTick` 이 정한 돌 틱 수(`RunTick` 이 쓴다)
    bool _wasRoundOver = false;   // 판 끝 이벤트 — 거짓 → 참으로 바뀔 때 한 번
    CRoomLoad _load;
};

// 방이 바깥에 알리는 것(4단계 인원 갱신 · 5단계 결과 기록이 쓴다). 나감은 세션표 변화로 본다(위 머리 주석).
struct RoomEvent {
    enum class Type { ROUND_OVER };
    Type type = Type::ROUND_OVER;
    int roomId = 0;
};

// 방 관리자의 셈 — 갈래마다 하나씩. 토큰 확인 갈래는 접속 시도와 CONNECT 를 합쳐 센다(어느 쪽인지는 받음·거절·끊음 셈으로).
struct CRoomStats {
    unsigned long long joinAccepted = 0;       // 접속 시도를 받음
    unsigned long long joinRejected = 0;       // 접속 시도를 거절(까닭은 아래 토큰 셈)
    unsigned long long assigned = 0;           // 방에 넣음(열린 방식 `CONNECTED` · 토큰 `CONNECTED`)
    unsigned long long connectRejected = 0;    // `CONNECTED` 를 끊음(까닭은 아래 토큰 셈)
    unsigned long long tokenNoKey = 0;         // 키도 열린 방식도 안 정함 — 설정 빠짐
    unsigned long long tokenMissing = 0;       // 꼬리(토큰)가 없음
    unsigned long long tokenBadAddr = 0;       // 보낸 주소를 IPv4 로 못 읽음
    unsigned long long tokenCheck[static_cast<int>(JoinTokenCheck::kCount)] = {};   // 검증 결과별(통과 포함 — `VerifyJoinToken`)
    unsigned long long tokenNoRoom = 0;        // 토큰의 방이 없음(닫힘)
    unsigned long long tokenReused = 0;        // 일회용 번호를 이미 씀(`CONNECTED`)
    unsigned long long droppedUnknown = 0;     // 토큰 방식에서 세션표에 없는 세션의 다른 이벤트를 버림
    unsigned long long left = 0;               // 방이 끊은 세션(나감 · 내보냄 · 시간 초과 — 끊기 요청)
    unsigned long long roomsClosed = 0;
    unsigned long long closedRoomSessions = 0; // 방을 닫으며 내보낸 세션
};

class CRoomManager {
public:
    static constexpr int kFirstRoom = 0;
    static constexpr int kNoRoom = -1;

    CRoomManager();   // 방 0 을 만든다
    CRoomManager(const CRoomManager&) = delete;
    CRoomManager& operator=(const CRoomManager&) = delete;

    CRoom& Room(int id) { return *_rooms.at(id); }
    const CRoom& Room(int id) const { return *_rooms.at(id); }
    bool HasRoom(int id) const { return _rooms.count(id) != 0; }
    Server& Game(int roomId = kFirstRoom) { return Room(roomId).Game(); }
    const Server& Game(int roomId = kFirstRoom) const { return Room(roomId).Game(); }
    size_t RoomCount() const { return _rooms.size(); }

    // ── 방 만들기 · 닫기(실행 중에도) ──
    int CreateRoom();   // 돌려주는 값 = 새 방 id
    // 그 방의 사람마다 `SERVER_CLOSE` 를 보내고 끊은 뒤 방을 지운다. 방 0 은 못 닫는다(false).
    bool CloseRoom(int id);

    // ── 입장 방식 ── (머리 주석 「10-10 토큰 작업 2」)
    void SetJoinKey(const JoinKey& key);   // 토큰 방식 — 실제 서버
    void SetOpenEntry();                   // 토큰 검사 끄기 — 모두 방 0. 녹음 재생·대조와 시험 도구만
    bool OpenEntry() const { return _openEntry; }
    void SetUnixClock(std::function<uint32_t()> now) { _unixNow = std::move(now); }   // 시험이 만료를 정해 보려고
    void SetLogJoins(bool on) { _logJoins = on; }   // 입장 판정을 한 줄씩 찍는다(`main` 이 켠다 — 시험은 조용히)

    // ── 세션표 ──
    int RoomOfSession(int64_t sessionId) const;   // 모르는 세션이면 `kNoRoom`
    size_t SeatCount() const { return _seats.size(); }
    // 소켓 없이 방에 바로 세운 사람(봇 · 시험 — `CSyncHost::DebugEnterGame`)을 세션표에 적는다
    void AssignDirect(int64_t sessionId, int roomId);

    // 통신 층 이벤트 하나. 돌려주는 값 = 방의 게임이 읽은 게임 메시지 수(`Server::OnNetworkEvent`).
    size_t OnNetworkEvent(const NetworkEvent& ev);
    // 방마다 게임 시계를 돌려 돌 틱 수를 정해 둔다. 돌려주는 값 = 방들 가운데 가장 많은 틱 수(0 이면 이번에 도는 방이 없다).
    int BeginTick(uint32_t nowMs);
    // `BeginTick` 이 정한 틱 수대로 방마다 게임 일을 한다(부하를 재고 판 끝을 알린다).
    void RunTick();
    // 다음 틱 시각 — 방들 가운데 가장 이른 것(`Server::NextTickDue`). 시계를 돌린 방이 없으면 false.
    bool NextTickDue(uint32_t& due) const;
    // 쌓인 통신 요청을 통째로 꺼낸다 — 방 관리자가 낸 것(접속 답 · 끊기)과 방마다 낸 것이 처리한 차례대로 들어 있다.
    //   이벤트 · 틱 밖에서 방에 바로 쌓인 것(시험 · 진단이 게임을 바로 부른 것)은 여기서 방 차례로 모은다.
    void SwapOutNetworkRequests(std::vector<NetworkRequest>& into);
    void SwapOutRoomEvents(std::vector<RoomEvent>& into) { into.swap(_roomEvents); _roomEvents.clear(); }
    const CRoomStats& Stats() const { return _stats; }

private:
    void OnJoinRequested(const NetworkEvent& ev);
    size_t Forward(int roomId, const NetworkEvent& ev);
    // 토큰으로 방을 정한다 — 접속 시도(확인만)와 `CONNECTED`(`consume` — 일회용 번호를 씀)가 같이 쓴다.
    //   못 받으면 `kNoRoom` · `why` = 까닭(셈은 안에서)
    int AdmitByToken(const NetworkEvent& ev, bool consume, JoinToken& tok, const char*& why);
    void LogJoin(const char* what, const NetworkEvent& ev, int roomId, const JoinToken& tok, const char* why) const;
    void RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg);
    void RequestSendMsg(int64_t sessionId, bool reliable, const uint8_t* msg, size_t n);
    void RequestDisconnectSession(int64_t sessionId);
    void PullRequests(CRoom& room);   // 방의 요청 목록을 방 관리자 목록 뒤로 옮긴다(끊기 요청이면 세션표에서 지운다)

    std::map<int, std::unique_ptr<CRoom>> _rooms;
    int _nextRoomId = kFirstRoom + 1;
    bool _openEntry = false;
    bool _hasKey = false;
    JoinKey _key{};
    CJoinTokenLedger _ledger;
    std::function<uint32_t()> _unixNow = UnixNow;
    bool _logJoins = false;
    std::map<int64_t, int> _seats;                // 세션표 — 세션 → 방
    std::vector<NetworkRequest> _requests;
    std::vector<NetworkRequest> _pull;            // `PullRequests` 가 쓰는 그릇(다시 씀)
    std::vector<RoomEvent> _roomEvents;
    CRoomStats _stats;
};

}  // namespace nox
