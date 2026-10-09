// RoomManager.h — 방 여럿을 게임 스레드 하나에서 돌린다(`Docs/ROOMS_DESIGN.md` — MO 3단계 플랜 B).
//
// ★ 10-09 플랜 B 작업 6a — 틀만 넣었다. 방은 하나(방 0)이고 모든 사람이 방 0에 든다(열린 방식) — 동작은 플랜 A 끝과 같다.
//   방 관리자는 통신 층 이벤트와 게임(`nox::Server`) 사이에 선다.
//     · 이벤트 → 방. 접속 시도(`JOIN_REQUESTED`)는 방으로 넘기지 않고 여기서 답한다(지금은 늘 허락 — 6b 부터 명단)
//     · 방마다 쌓인 통신 요청을 처리한 차례대로 모아 한 묶음으로 내준다(`SwapOutNetworkRequests`)
//     · 틱 — 방마다 시계를 따로 돌린다(`BeginTick` → `RunTick`). 게임 루프가 기다릴 다음 틱 시각은 방들 가운데 가장 이른 것
//   명단 · 방 배정 · 거절 · 방이 아직 없는 세션 · 나감 · 방 만들기/닫기는 작업 6b.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "nox/Server.h"
#include "nox/UdpServer.h"

namespace nox {

// 방 하나 — 원본 재현 게임(`nox::Server`) 하나를 든다. MO 규칙(대기/진행/끝 · 판 시작 · 결과)은 여기에 쌓인다(작업 7 · 5단계).
class CRoom {
public:
    explicit CRoom(int id) : _id(id) {}
    CRoom(const CRoom&) = delete;
    CRoom& operator=(const CRoom&) = delete;

    int Id() const { return _id; }
    Server& Game() { return _game; }
    const Server& Game() const { return _game; }

private:
    friend class CRoomManager;
    int _id;
    Server _game;
    int _ticks = 0;   // 이번 `BeginTick` 이 정한 돌 틱 수(`RunTick` 이 쓴다)
};

class CRoomManager {
public:
    static constexpr int kFirstRoom = 0;

    CRoomManager();   // 방 0 을 만든다
    CRoomManager(const CRoomManager&) = delete;
    CRoomManager& operator=(const CRoomManager&) = delete;

    CRoom& Room(int id) { return *_rooms.at(id); }
    const CRoom& Room(int id) const { return *_rooms.at(id); }
    Server& Game(int roomId = kFirstRoom) { return Room(roomId).Game(); }
    const Server& Game(int roomId = kFirstRoom) const { return Room(roomId).Game(); }
    size_t RoomCount() const { return _rooms.size(); }

    // 통신 층 이벤트 하나. 돌려주는 값 = 방의 게임이 읽은 게임 메시지 수(`Server::OnNetworkEvent`).
    size_t OnNetworkEvent(const NetworkEvent& ev);
    // 방마다 게임 시계를 돌려 돌 틱 수를 정해 둔다. 돌려주는 값 = 방들 가운데 가장 많은 틱 수(0 이면 이번에 도는 방이 없다).
    int BeginTick(uint32_t nowMs);
    // `BeginTick` 이 정한 틱 수대로 방마다 게임 일을 한다.
    void RunTick();
    // 다음 틱 시각 — 방들 가운데 가장 이른 것(`Server::NextTickDue`). 시계를 돌린 방이 없으면 false.
    bool NextTickDue(uint32_t& due) const;
    // 쌓인 통신 요청을 통째로 꺼낸다 — 방 관리자가 낸 것(접속 답)과 방마다 낸 것이 처리한 차례대로 들어 있다.
    //   이벤트 · 틱 밖에서 방에 바로 쌓인 것(시험 · 진단이 게임을 바로 부른 것)은 여기서 방 차례로 모은다.
    void SwapOutNetworkRequests(std::vector<NetworkRequest>& into);

private:
    void OnJoinRequested(const NetworkEvent& ev);
    void RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg);
    CRoom& RoomOf(int64_t sessionId);
    void PullRequests(CRoom& room);   // 방의 요청 목록을 방 관리자 목록 뒤로 옮긴다

    std::map<int, std::unique_ptr<CRoom>> _rooms;
    std::vector<NetworkRequest> _requests;
    std::vector<NetworkRequest> _pull;   // `PullRequests` 가 쓰는 그릇(다시 씀)
};

}  // namespace nox
