// RoomManager.cpp — 방 여럿. 설명은 `RoomManager.h` 머리.
#include "nox/RoomManager.h"

#include <chrono>

#include "nox/ByteBuffer.h"
#include "nox/Messages.h"
#include "nox/Opcode.h"

namespace nox {

namespace {
std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string IpOf(const std::string& addr) {   // "1.2.3.4:5678" → "1.2.3.4"
    const size_t colon = addr.rfind(':');
    return colon == std::string::npos ? addr : addr.substr(0, colon);
}

// 접속 시도 본문 97바이트(`MsgServerTryJoin` — 앞 1바이트 뒤 이름 50)에서 이름을 읽는다. 못 읽으면 false.
bool NameFromTryJoin(const uint8_t* p, size_t n, std::string& name) {
    if (n < MsgServerTryJoin::kSize) return false;
    Reader r(p, MsgServerTryJoin::kSize);
    MsgServerTryJoin m;
    if (!m.Decode(r)) return false;
    name = m.playerName;
    return true;
}

// 입장(`CLIENT_ACCEPT`) 본문 — 사람 정보 `PlayerInfo`(이름 0-49 · 직업 66 · `GameBalance.h` 「직업은 66번」 표)에서 이름을 읽는다.
//   ⚠ 10-09 작업 6b — 접속 시도와 **짜임이 다르다**(이름 자리가 1바이트 어긋남). 처음엔 같은 짜임으로 읽어 시험만 통과했다 —
//     시험이 만든 패킷도 같은 착각으로 만들었기 때문. 정품 녹음 197개(`_evidence/`)의 입장 조각이 `20 4a 00 61 00 …`(이름이 바로 뒤)였다
bool NameFromPlayerInfo(const uint8_t* p, size_t n, std::string& name) {
    constexpr size_t kNameBytes = 50;
    if (n < kNameBytes) return false;
    Reader r(p, kNameBytes);
    return r.FixedString16(name, kNameBytes);
}
}  // namespace

CRoomManager::CRoomManager() {
    _rooms.emplace(kFirstRoom, std::make_unique<CRoom>(kFirstRoom));
}

int CRoomManager::CreateRoom() {
    const int id = _nextRoomId++;
    _rooms.emplace(id, std::make_unique<CRoom>(id));
    return id;
}

bool CRoomManager::CloseRoom(int id) {
    if (id == kFirstRoom || id == _defaultRoom) return false;
    auto it = _rooms.find(id);
    if (it == _rooms.end()) return false;
    PullRequests(*it->second);   // 그 방이 이미 낸 요청이 먼저
    const uint8_t closeMsg[1] = { static_cast<uint8_t>(Op::SERVER_CLOSE) };
    for (auto s = _seats.begin(); s != _seats.end();) {
        if (s->second.roomId != id) { ++s; continue; }
        // 서버를 닫을 때와 같은 알림(플랜 A 조각 5d-2c `CloseAll` — 비신뢰 1바이트) 뒤에 끊는다
        RequestSendMsg(s->first, /*reliable=*/false, closeMsg, sizeof(closeMsg));
        RequestDisconnectSession(s->first);
        ++_stats.closedRoomSessions;
        s = _seats.erase(s);
    }
    _rooms.erase(it);
    ++_stats.roomsClosed;
    return true;
}

void CRoomManager::SetRosterEntry(const std::string& name, int roomId, const std::string& ip) {
    RosterEntry e;
    e.roomId = roomId;
    e.ip = ip;
    _roster[Lower(name)] = e;
}

int CRoomManager::RoomOfSession(int64_t sessionId) const {
    auto it = _seats.find(sessionId);
    return it == _seats.end() ? kNoRoom : it->second.roomId;
}

void CRoomManager::AssignDirect(int64_t sessionId, int roomId) {
    _seats[sessionId].roomId = roomId;
}

size_t CRoomManager::OnNetworkEvent(const NetworkEvent& ev) {
    // 접속 시도는 방으로 넘기지 않는다 — 받아 줄지는 방 관리자가 정한다(사람 자리도 만들지 않는다 · 플랜 A 조각 5c)
    if (ev.type == NetworkEvent::Type::JOIN_REQUESTED) {
        OnJoinRequested(ev);
        return 0;
    }
    auto it = _seats.find(ev.sessionId);
    if (it != _seats.end()) {
        if (it->second.roomId != kNoRoom) return Forward(it->second.roomId, ev);
        return OnUnassigned(ev.sessionId, ev);
    }
    // 세션표에 없는 세션
    if (OpenMode()) {
        // 열린 방식 — 모두 방 0. `CONNECTED` 때 바로 넘겨 세션 시작 프레임이 플랜 A 끝과 같다(`ROOMS_DESIGN.md`)
        //   (`CONNECTED` 없이 오는 것 — 옛 동작대로 방 0 이 받는다)
        _seats[ev.sessionId].roomId = kFirstRoom;
        if (ev.type == NetworkEvent::Type::CONNECTED) ++_stats.assigned;
        return Forward(kFirstRoom, ev);
    }
    if (ev.type == NetworkEvent::Type::CONNECTED) {
        Seat& seat = _seats[ev.sessionId];   // 방은 입장 때 — 그때 이 이벤트부터 방에 넘긴다
        seat.connected = ev;
        return 0;
    }
    ++_stats.unassignedDropped;   // 닫힌 방식 — 접속도 안 한 세션의 이벤트
    return 0;
}

// 방이 아직 없는 세션(닫힌 방식 · `CONNECTED` 뒤 · 입장 전)
size_t CRoomManager::OnUnassigned(int64_t sessionId, const NetworkEvent& ev) {
    switch (ev.type) {
        case NetworkEvent::Type::RECEIVED: {
            const bool isAccept = !ev.body.empty() && ev.body[0] == static_cast<uint8_t>(Op::CLIENT_ACCEPT);
            std::string name;
            if (!isAccept || !NameFromPlayerInfo(ev.body.data() + 1, ev.body.size() - 1, name)) {
                ++_stats.unassignedDropped;   // 입장 전에 온 다른 게임 메시지 — 넘길 방이 없다
                return 0;
            }
            const int roomId = PickRoom(name, ev.addr);
            if (roomId == kNoRoom) {
                RequestDisconnectSession(sessionId);
                _seats.erase(sessionId);
                return 0;
            }
            const NetworkEvent connected = _seats[sessionId].connected;   // 넘기는 동안 세션표가 바뀌어도 되게 떠 둔다
            _seats[sessionId].roomId = roomId;
            ++_stats.assigned;
            Forward(roomId, connected);   // 담아 둔 접속부터 — 방은 이때 세션을 만든다(시작 프레임은 지금 값)
            return Forward(roomId, ev);
        }
        case NetworkEvent::Type::DISCONNECTED:
            if (ev.reason == DisconnectReason::TIMEOUT) {
                // 방이 하는 「자리 비우기」와 같은 차례 — 시간 초과 알림(신뢰) 뒤에 끊는다(`Server.cpp` 자리 비우기)
                const uint8_t to[1] = { static_cast<uint8_t>(Op::TIMEOUT_NOTIFICATION) };
                RequestSendMsg(sessionId, /*reliable=*/true, to, sizeof(to));
                ++_stats.unassignedTimedOut;
            } else {
                ++_stats.unassignedClosed;
            }
            RequestDisconnectSession(sessionId);
            _seats.erase(sessionId);
            return 0;
        case NetworkEvent::Type::CONNECTED:
            _seats[sessionId].connected = ev;
            return 0;
        case NetworkEvent::Type::JOIN_REQUESTED:
            return 0;   // 위에서 돌려보냈다
    }
    return 0;
}

size_t CRoomManager::Forward(int roomId, const NetworkEvent& ev) {
    CRoom& room = Room(roomId);
    const size_t n = room._game.OnNetworkEvent(ev);
    PullRequests(room);   // 처리한 차례대로 — 다음 이벤트에서 방 관리자가 낼 요청(접속 답)보다 앞에 둔다
    return n;
}

int CRoomManager::PickRoom(const std::string& name, const std::string& addr) {
    auto r = _roster.find(Lower(name));
    if (r == _roster.end()) {
        if (_defaultRoom != kNoRoom && HasRoom(_defaultRoom)) { ++_stats.assignedDefault; return _defaultRoom; }
        ++_stats.acceptNoRoster;
        return kNoRoom;
    }
    if (!r->second.ip.empty() && r->second.ip != IpOf(addr)) { ++_stats.acceptIpMismatch; return kNoRoom; }
    if (!HasRoom(r->second.roomId)) { ++_stats.acceptNoRoom; return kNoRoom; }
    return r->second.roomId;
}

bool CRoomManager::RosterAccepts(const std::string& name) const {
    return _roster.count(Lower(name)) != 0 || (_defaultRoom != kNoRoom && HasRoom(_defaultRoom));
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
        const auto start = std::chrono::steady_clock::now();
        room._game.RunTick(room._ticks);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++room._load.tickRuns;
        room._load.tickMsSum += ms;
        if (ms > room._load.tickMsMax) room._load.tickMsMax = ms;
        room._ticks = 0;
        PullRequests(room);
        const bool over = room._game.RoundOver();
        if (over && !room._wasRoundOver) {
            RoomEvent e;
            e.type = RoomEvent::Type::ROUND_OVER;
            e.roomId = room._id;
            _roomEvents.push_back(e);
        }
        room._wasRoundOver = over;
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
//   (원본 `noxnetJoinCheck` `network.go:383` — 버전·인원·중복 일련번호·닫힌 판 명단 …).
// ★ 10-09 작업 6b — 열린 방식은 늘 받는다(옛 동작). 닫힌 방식은 이름이 명단에 있어야(또는 기본 방이 있어야) 받고,
//   아니면 `SERVER_ERROR` 사유 4(`ConnectError::Closed`) — 원본이 닫힌 판에서 이름 목록 밖을 돌려보내는 메시지(`network.go:443-453`).
//   원본 거절 규칙의 나머지(버전·인원·밴)를 옮기는 것은 기능 일이라 따로 정한다.
void CRoomManager::OnJoinRequested(const NetworkEvent& ev) {
    std::string name;
    if (OpenMode() || (NameFromTryJoin(ev.body.data(), ev.body.size(), name) && RosterAccepts(name))) {
        ++_stats.joinAccepted;
        RespondJoin(ev.sessionId, /*accept=*/true, {});
        return;
    }
    ++_stats.joinRejected;
    RespondJoin(ev.sessionId, /*accept=*/false,
                { static_cast<uint8_t>(Op::SERVER_ERROR), static_cast<uint8_t>(ConnectError::Closed) });
}

void CRoomManager::RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg) {
    NetworkRequest r;
    r.type = NetworkRequest::Type::RESPOND_JOIN;
    r.sessionId = sessionId;
    r.accept = accept;
    r.body = rejectMsg;
    _requests.push_back(std::move(r));
}

void CRoomManager::RequestSendMsg(int64_t sessionId, bool reliable, const uint8_t* msg, size_t n) {
    NetworkRequest r;
    r.type = NetworkRequest::Type::SEND_MSG;
    r.sessionId = sessionId;
    r.reliable = reliable;
    r.stream = 0;
    r.body.assign(msg, msg + n);
    _requests.push_back(std::move(r));
}

void CRoomManager::RequestDisconnectSession(int64_t sessionId) {
    NetworkRequest r;
    r.type = NetworkRequest::Type::DISCONNECT_SESSION;
    r.sessionId = sessionId;
    _requests.push_back(std::move(r));
}

void CRoomManager::PullRequests(CRoom& room) {
    room._game.SwapOutNetworkRequests(_pull);
    for (NetworkRequest& q : _pull) {
        // ★ 작업 6b — 나감. 방에서 사람이 빠지는 길(자리 비우기 · 내보내기)은 모두 이 끊기 요청으로 끝난다
        if (q.type == NetworkRequest::Type::DISCONNECT_SESSION && _seats.erase(q.sessionId) != 0) ++_stats.left;
        _requests.push_back(std::move(q));
    }
    _pull.clear();
}

}  // namespace nox
