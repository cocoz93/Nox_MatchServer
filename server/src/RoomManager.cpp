// RoomManager.cpp — 방 여럿. 설명은 `RoomManager.h` 머리.
#include "nox/RoomManager.h"

#include <chrono>
#include <cstdio>

#include "nox/Messages.h"
#include "nox/Opcode.h"

namespace nox {

namespace {
std::string IpOf(const std::string& addr) {   // "1.2.3.4:5678" → "1.2.3.4"
    const size_t colon = addr.rfind(':');
    return colon == std::string::npos ? addr : addr.substr(0, colon);
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
    if (id == kFirstRoom) return false;
    auto it = _rooms.find(id);
    if (it == _rooms.end()) return false;
    PullRequests(*it->second);   // 그 방이 이미 낸 요청이 먼저
    const uint8_t closeMsg[1] = { static_cast<uint8_t>(Op::SERVER_CLOSE) };
    for (auto s = _seats.begin(); s != _seats.end();) {
        if (s->second != id) { ++s; continue; }
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

void CRoomManager::SetJoinKey(const JoinKey& key) {
    _key = key;
    _hasKey = true;
    _openEntry = false;
}

void CRoomManager::SetOpenEntry() {
    _openEntry = true;
}

int CRoomManager::RoomOfSession(int64_t sessionId) const {
    auto it = _seats.find(sessionId);
    return it == _seats.end() ? kNoRoom : it->second;
}

void CRoomManager::AssignDirect(int64_t sessionId, int roomId) {
    _seats[sessionId] = roomId;
}

size_t CRoomManager::OnNetworkEvent(const NetworkEvent& ev) {
    // 접속 시도는 방으로 넘기지 않는다 — 받아 줄지는 방 관리자가 정한다(사람 자리도 만들지 않는다 · 플랜 A 조각 5c)
    if (ev.type == NetworkEvent::Type::JOIN_REQUESTED) {
        OnJoinRequested(ev);
        return 0;
    }
    auto it = _seats.find(ev.sessionId);
    if (it != _seats.end()) return Forward(it->second, ev);
    // 세션표에 없는 세션
    if (_openEntry) {
        // 열린 방식 — 모두 방 0. `CONNECTED` 때 바로 넘겨 세션 시작 프레임이 플랜 A 끝과 같다(`ROOMS_DESIGN.md`)
        //   (`CONNECTED` 없이 오는 것 — 옛 동작대로 방 0 이 받는다)
        _seats[ev.sessionId] = kFirstRoom;
        if (ev.type == NetworkEvent::Type::CONNECTED) ++_stats.assigned;
        return Forward(kFirstRoom, ev);
    }
    if (ev.type == NetworkEvent::Type::CONNECTED) {
        // 토큰 방식 — 같은 토큰을 이번엔 써 버리고 그 방에 바로 넘긴다. 틀리면 끊는다(접속 수락은 통신 층이 이미 보냈다)
        JoinToken tok;
        const char* why = "";
        const int roomId = AdmitByToken(ev, /*consume=*/true, tok, why);
        if (roomId == kNoRoom) {
            ++_stats.connectRejected;
            RequestDisconnectSession(ev.sessionId);
            LogJoin("CONNECT 끊음", ev, roomId, tok, why);
            return 0;
        }
        _seats[ev.sessionId] = roomId;
        ++_stats.assigned;
        LogJoin("방 배정", ev, roomId, tok, why);
        return Forward(roomId, ev);
    }
    ++_stats.droppedUnknown;   // 토큰 방식 — 방에 안 든 세션의 이벤트(끊은 세션에 늦게 온 것 등)
    return 0;
}

size_t CRoomManager::Forward(int roomId, const NetworkEvent& ev) {
    CRoom& room = Room(roomId);
    const size_t n = room._game.OnNetworkEvent(ev);
    PullRequests(room);   // 처리한 차례대로 — 다음 이벤트에서 방 관리자가 낼 요청(접속 답)보다 앞에 둔다
    return n;
}

int CRoomManager::AdmitByToken(const NetworkEvent& ev, bool consume, JoinToken& tok, const char*& why) {
    if (!_hasKey) { ++_stats.tokenNoKey; why = "키 없음(설정 빠짐)"; return kNoRoom; }
    if (ev.token.empty()) { ++_stats.tokenMissing; why = "토큰 없음"; return kNoRoom; }
    uint32_t ip = 0;
    if (!ParseIpv4(IpOf(ev.addr), ip)) { ++_stats.tokenBadAddr; why = "주소"; return kNoRoom; }
    const uint32_t now = _unixNow();
    const JoinTokenCheck c = VerifyJoinToken(_key, ev.token.data(), ev.token.size(), now, ip, tok);
    ++_stats.tokenCheck[static_cast<int>(c)];
    if (c != JoinTokenCheck::Ok) { why = JoinTokenCheckName(c); return kNoRoom; }
    if (!HasRoom(tok.roomId)) { ++_stats.tokenNoRoom; why = "방 없음"; return kNoRoom; }
    if (consume && !_ledger.Consume(tok.nonce, tok.expiresAt, now)) { ++_stats.tokenReused; why = "이미 쓴 토큰"; return kNoRoom; }
    return tok.roomId;
}

// 입장 판정 한 줄(`SetLogJoins`) — 두 스레드가 한 표준 출력에 찍으므로 한 줄은 `printf` 한 번(플랜 A 자가검증 ①)
void CRoomManager::LogJoin(const char* what, const NetworkEvent& ev, int roomId, const JoinToken& tok, const char* why) const {
    if (!_logJoins) return;
    if (roomId == kNoRoom)
        std::printf("[입장] %s — %s · 까닭 %s\n", what, ev.addr.c_str(), why);
    else
        std::printf("[입장] %s — %s · 방 %d · 로비 이름 %s\n", what, ev.addr.c_str(), roomId, tok.account.c_str());
    std::fflush(stdout);
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
// ★ 10-10 토큰 작업 2 — 열린 방식은 늘 받는다(옛 동작). 토큰 방식은 꼬리의 토큰을 확인만 하고(쓰는 것은 CONNECT 때 —
//   클라는 접속 시도를 다시 보낼 수 있다) 받거나 `SERVER_ERROR` 사유 4(`ConnectError::Closed`) — 원본이 닫힌 판에서
//   이름 목록 밖을 돌려보내는 메시지(`network.go:443-453`)를 그대로 쓴다. 원본 거절 규칙의 나머지(버전·인원·밴)는 기능 일이라 따로.
void CRoomManager::OnJoinRequested(const NetworkEvent& ev) {
    if (_openEntry) {
        ++_stats.joinAccepted;
        RespondJoin(ev.sessionId, /*accept=*/true, {});
        return;
    }
    JoinToken tok;
    const char* why = "";
    const int roomId = AdmitByToken(ev, /*consume=*/false, tok, why);
    if (roomId != kNoRoom) {
        ++_stats.joinAccepted;
        RespondJoin(ev.sessionId, /*accept=*/true, {});
        LogJoin("접속 시도 받음", ev, roomId, tok, why);
        return;
    }
    ++_stats.joinRejected;
    RespondJoin(ev.sessionId, /*accept=*/false,
                { static_cast<uint8_t>(Op::SERVER_ERROR), static_cast<uint8_t>(ConnectError::Closed) });
    LogJoin("접속 시도 거절", ev, roomId, tok, why);
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
