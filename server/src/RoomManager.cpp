// RoomManager.cpp — 방 여럿. 설명은 `RoomManager.h` 머리.
#include "nox/RoomManager.h"

namespace nox {

CRoomManager::CRoomManager() {
    _rooms.emplace(kFirstRoom, std::make_unique<CRoom>(kFirstRoom));
}

size_t CRoomManager::OnNetworkEvent(const NetworkEvent& ev) {
    // 접속 시도는 방으로 넘기지 않는다 — 받아 줄지는 방 관리자가 정한다(사람 자리도 만들지 않는다 · 플랜 A 조각 5c)
    if (ev.type == NetworkEvent::Type::JOIN_REQUESTED) {
        OnJoinRequested(ev);
        return 0;
    }
    CRoom& room = RoomOf(ev.sessionId);
    const size_t n = room._game.OnNetworkEvent(ev);
    PullRequests(room);   // 처리한 차례대로 — 다음 이벤트에서 방 관리자가 낼 요청(접속 답)보다 앞에 둔다
    return n;
}

int CRoomManager::BeginTick(uint32_t nowMs) {
    int most = 0;
    for (auto& kv : _rooms) {
        CRoom& room = *kv.second;
        room._ticks = room._game.BeginTick(nowMs);
        if (room._ticks > most) most = room._ticks;
    }
    return most;
}

void CRoomManager::RunTick() {
    for (auto& kv : _rooms) {
        CRoom& room = *kv.second;
        if (room._ticks <= 0) continue;
        room._game.RunTick(room._ticks);
        room._ticks = 0;
        PullRequests(room);
    }
}

bool CRoomManager::NextTickDue(uint32_t& due) const {
    bool any = false;
    for (const auto& kv : _rooms) {
        uint32_t d = 0;
        if (!kv.second->_game.NextTickDue(d)) continue;
        if (!any || static_cast<int32_t>(d - due) < 0) due = d;   // 32비트 시계가 돌아도 차이로 견준다
        any = true;
    }
    return any;
}

void CRoomManager::SwapOutNetworkRequests(std::vector<NetworkRequest>& into) {
    for (auto& kv : _rooms) PullRequests(*kv.second);
    into.swap(_requests);
    _requests.clear();
}

// ★ 10-09 플랜 B 작업 6a — 옛 `Server::OnJoinRequested`(플랜 A 조각 5c) 자리. 받아 줄지는 게임 규칙이다
//   (원본 `noxnetJoinCheck` `network.go:383` — 버전·인원·중복 일련번호·닫힌 판 명단 …). 지금은 늘 받아 준다 — 옛 통신 층 동작 그대로.
//   6b 부터 명단으로 가르고, 명단 밖은 `SERVER_ERROR` 사유 4(원본의 닫힌 판 명단 거절 — `ROOMS_DESIGN.md`).
//   원본 거절 규칙의 나머지(버전·인원·밴)를 옮기는 것은 기능 일이라 따로 정한다.
void CRoomManager::OnJoinRequested(const NetworkEvent& ev) {
    RespondJoin(ev.sessionId, /*accept=*/true, {});
}

void CRoomManager::RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg) {
    NetworkRequest r;
    r.type = NetworkRequest::Type::RESPOND_JOIN;
    r.sessionId = sessionId;
    r.accept = accept;
    r.body = rejectMsg;
    _requests.push_back(std::move(r));
}

// 6a — 열린 방식 · 방 하나라 모두 방 0. 세션표(세션 → 방)는 6b.
CRoom& CRoomManager::RoomOf(int64_t /*sessionId*/) {
    return Room(kFirstRoom);
}

void CRoomManager::PullRequests(CRoom& room) {
    room._game.SwapOutNetworkRequests(_pull);
    for (NetworkRequest& q : _pull) _requests.push_back(std::move(q));
    _pull.clear();
}

}  // namespace nox
