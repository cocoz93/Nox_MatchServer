// RoomManager.h — 방 여럿을 게임 스레드 하나에서 돌린다(`Docs/ROOMS_DESIGN.md` — MO 3단계 플랜 B).
//
// ★ 10-09 플랜 B 작업 6a — 틀. 방 관리자는 통신 층 이벤트와 게임(`nox::Server`) 사이에 선다.
//     · 이벤트 → 세션표의 방. 접속 시도(`JOIN_REQUESTED`)는 방으로 넘기지 않고 여기서 답한다
//     · 방마다 쌓인 통신 요청을 처리한 차례대로 모아 한 묶음으로 내준다(`SwapOutNetworkRequests`)
//     · 틱 — 방마다 시계를 따로 돌린다(`BeginTick` → `RunTick`). 게임 루프가 기다릴 다음 틱 시각은 방들 가운데 가장 이른 것
// ★ 10-09 플랜 B 작업 6b — 명단 · 배정 · 거절 · 방이 아직 없는 세션 · 나감 · 방 이벤트 · 방 만들기/닫기.
//     · 열린 방식(명단이 비었음) — 모두 방 0. `CONNECTED` 때 바로 넘긴다(플랜 A 끝과 와이어가 같다)
//     · 닫힌 방식 — 접속 시도는 이름이 명단에 있어야 받는다(없으면 `SERVER_ERROR` 사유 4 — 원본의 닫힌 판 명단 거절).
//       `CONNECTED` 는 담아 두었다가 입장(`CLIENT_ACCEPT`) 본문의 이름으로 방을 정해 그 방에 차례대로 넘긴다.
//       클라는 접속 시도와 접속을 다른 포트에서 보내므로 접속 시도의 주소로는 방을 못 정한다(`ROOMS_DESIGN.md`)
//     · 나감 — 방이 내는 끊기 요청(`DISCONNECT_SESSION`)을 넘길 때 세션표에서 지운다(나가는 길이 모두 그 요청으로 모인다)
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
    explicit CRoom(int id) : _id(id) {}
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

// 방 관리자의 셈 — 갈래마다 하나씩(받음 · 거절 · 끊음의 까닭 셋 · 방이 없는 세션의 끝 셋).
struct CRoomStats {
    unsigned long long joinAccepted = 0;       // 접속 시도를 받음
    unsigned long long joinRejected = 0;       // 접속 시도를 거절(닫힌 방식 · 명단 밖)
    unsigned long long assigned = 0;           // 방에 넣음(열린 방식 `CONNECTED` · 닫힌 방식 입장)
    unsigned long long assignedDefault = 0;    // 그 가운데 명단 밖이라 기본 방(`SetDefaultRoom`)으로
    unsigned long long acceptNoRoster = 0;     // 입장 때 끊음 — 명단 밖
    unsigned long long acceptIpMismatch = 0;   // 입장 때 끊음 — 명단의 IP 와 다름
    unsigned long long acceptNoRoom = 0;       // 입장 때 끊음 — 명단의 방이 없음(닫힘)
    unsigned long long unassignedTimedOut = 0; // 방이 없는 세션 — 시간 초과(`0xC6` 보내고 끊음)
    unsigned long long unassignedClosed = 0;   // 방이 없는 세션 — 클라가 닫음
    unsigned long long unassignedDropped = 0;  // 방이 없는 세션의 다른 이벤트를 버림(입장 전 게임 메시지 · 닫힌 방식에서 모르는 세션)
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
    // 그 방의 사람마다 `SERVER_CLOSE` 를 보내고 끊은 뒤 방을 지운다. 방 0 · 기본 방은 못 닫는다(false).
    bool CloseRoom(int id);

    // ── 명단 ── 이름(대소문자 무시) → 방[@IP]. 하나라도 있으면 닫힌 방식
    void SetRosterEntry(const std::string& name, int roomId, const std::string& ip = {});
    // 닫힌 방식에서 명단 밖을 이 방으로 받는다(시험용 `--room-default`). `kNoRoom` 이면 끔.
    void SetDefaultRoom(int roomId) { _defaultRoom = roomId; }
    bool OpenMode() const { return _roster.empty(); }

    // ── 세션표 ──
    int RoomOfSession(int64_t sessionId) const;   // 방이 없거나 모르는 세션이면 `kNoRoom`
    size_t SeatCount() const { return _seats.size(); }   // 방이 없는 세션 포함
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
    struct Seat {
        int roomId = kNoRoom;      // `kNoRoom` = 방이 아직 없음(닫힌 방식 · 입장 전)
        NetworkEvent connected;     // 방이 없는 동안 담아 둔 `CONNECTED`
    };
    struct RosterEntry {
        int roomId = kFirstRoom;
        std::string ip;             // 빈 글자면 대조 안 함
    };

    void OnJoinRequested(const NetworkEvent& ev);
    size_t OnUnassigned(int64_t sessionId, const NetworkEvent& ev);
    size_t Forward(int roomId, const NetworkEvent& ev);
    // 입장 본문의 이름 · 주소로 방을 고른다. 끊어야 하면 `kNoRoom`(셈은 안에서)
    int PickRoom(const std::string& name, const std::string& addr);
    bool RosterAccepts(const std::string& name) const;
    void RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg);
    void RequestSendMsg(int64_t sessionId, bool reliable, const uint8_t* msg, size_t n);
    void RequestDisconnectSession(int64_t sessionId);
    void PullRequests(CRoom& room);   // 방의 요청 목록을 방 관리자 목록 뒤로 옮긴다(끊기 요청이면 세션표에서 지운다)

    std::map<int, std::unique_ptr<CRoom>> _rooms;
    int _nextRoomId = kFirstRoom + 1;
    std::map<std::string, RosterEntry> _roster;   // 소문자 이름 → 방[@IP]
    int _defaultRoom = kNoRoom;
    std::map<int64_t, Seat> _seats;               // 세션표
    std::vector<NetworkRequest> _requests;
    std::vector<NetworkRequest> _pull;            // `PullRequests` 가 쓰는 그릇(다시 씀)
    std::vector<RoomEvent> _roomEvents;
    CRoomStats _stats;
};

}  // namespace nox
