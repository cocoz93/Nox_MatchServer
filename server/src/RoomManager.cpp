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

const char* RoomPhaseName(RoomPhase p) {
    switch (p) {
        case RoomPhase::Waiting: return "대기";
        case RoomPhase::Playing: return "판";
    }
    return "?";
}

// ★ 10-14 플랜 B 작업 9d — 방 만들기 입구(옛 빈 방 만들기 + `main` 이 방 0 에만 세계를 차리던 것). 방 번호는 세계 차리기가 된 뒤에 매긴다
int CRoomManager::CreateRoom(const RoomSettings& settings, int* buildRc) {
    const int id = _nextRoomId;
    auto room = std::make_unique<CRoom>(id);
    room->SetSettings(settings);
    room->SetMapCatalog(_hasWorld ? _world.maps : nullptr);   // ★ 10-16 작업 13b — 판을 열 때 붙을 맵 원본
    if (_hasWorld) {
        const int rc = BuildRoom(room->_game, room->_settings, _world, room->_extras);
        if (buildRc) *buildRc = rc;
        if (rc != 0) {
            ++_stats.roomBuildFailed;
            return kNoRoom;
        }
    } else if (buildRc) {
        *buildRc = 0;
    }
    ++_nextRoomId;
    _rooms.emplace(id, std::move(room));
    ++_stats.roomsCreated;
    if (_logRooms) {   // ★ 10-14 작업 9e — 한 줄은 `printf` 한 번(두 스레드가 한 표준 출력에 찍는다)
        std::printf("[방] %d 만듦 — 이름 %s · 맵 %s · 모드 %s%s\n", id, settings.roomName.empty() ? "(없음)" : settings.roomName.c_str(),
                    settings.mapPath.empty() ? "(녹음)" : MapKeyOf(settings.mapPath).c_str(), settings.mode.c_str(),
                    settings.closeWhenEmpty ? " · 비면 닫음" : "");
        std::fflush(stdout);
    }
    return id;
}

std::map<int, size_t> CRoomManager::SeatCounts() const {
    std::map<int, size_t> n;
    for (const auto& kv : _seats) ++n[kv.second];
    return n;
}

std::vector<RoomFact> CRoomManager::ListRooms() const {
    const std::map<int, size_t> seats = SeatCounts();
    std::vector<RoomFact> r;
    r.reserve(_rooms.size());
    for (const auto& kv : _rooms) {
        const CRoom& room = *kv.second;
        RoomFact f;
        f.roomId = room._id;
        f.name = room._settings.roomName;
        f.creator = room._settings.creator;
        f.host = room._hostName;      // ★ 10-16 결함 ②
        f.mapKey = room._curMapKey;   // ★ 10-16 작업 13b — 지금 맵 · 모드(판 중이면 대전 맵 — 옛 값 = 방 설정의 맵 · 모드)
        f.mode = room._curMode;
        f.players = room._game.inGameCount();
        const auto it = seats.find(room._id);
        f.seats = it == seats.end() ? 0 : it->second;
        f.maxPlayers = room._settings.maxPlayers;   // ★ 10-16 작업 12a
        f.phase = room._phase;
        r.push_back(std::move(f));
    }
    return r;
}

// ★ 10-14 작업 9e — 비면 닫는 방(로비가 만든 방)이 빈 채 `kEmptyRoomCloseMs` 지나면 닫는다. 빈 때는 처음 빈 것을 본 `BeginTick` 의 시각
void CRoomManager::CloseEmptyRooms(uint32_t nowMs) {
    std::vector<int> toClose;
    const std::map<int, size_t> seats = SeatCounts();
    for (auto& kv : _rooms) {
        CRoom& room = *kv.second;
        if (!room._settings.closeWhenEmpty) continue;
        const auto it = seats.find(room._id);
        if (it != seats.end() && it->second > 0) {
            room._emptyArmed = false;
            continue;
        }
        if (!room._emptyArmed) {
            room._emptyArmed = true;
            room._emptySinceMs = nowMs;
            continue;
        }
        if (static_cast<uint32_t>(nowMs - room._emptySinceMs) >= kEmptyRoomCloseMs) toClose.push_back(room._id);
    }
    for (const int id : toClose) {
        CloseRoom(id);
        ++_stats.emptyRoomsClosed;
        if (_logRooms) {
            std::printf("[방] %d 닫음 — 빈 채 %u초\n", id, kEmptyRoomCloseMs / 1000);
            std::fflush(stdout);
        }
    }
}

bool CRoomManager::CloseRoom(int id) {
    auto it = _rooms.find(id);
    if (it == _rooms.end()) return false;
    // ★ 10-14 작업 9d — 열린 방식의 방을 닫으면 그 뒤로는 받지 않는다(받는 자리가 `HasRoom` 으로 본다 · 방 번호는 다시 안 쓴다)
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

void CRoomManager::SetOpenEntry(int roomId) {
    _openEntry = true;
    _openRoom = roomId;
}

int CRoomManager::RoomOfSession(int64_t sessionId) const {
    auto it = _seats.find(sessionId);
    return it == _seats.end() ? kNoRoom : it->second;
}

void CRoomManager::AssignDirect(int64_t sessionId, int roomId) {
    _seats[sessionId] = roomId;
    if (HasRoom(roomId)) AddMember(Room(roomId), sessionId, std::string());   // ★ 10-16 작업 12a — 구성원 = 세션표의 이 방 세션
}

size_t CRoomManager::SeatsIn(int roomId) const {
    size_t n = 0;
    for (const auto& kv : _seats) if (kv.second == roomId) ++n;
    return n;
}

// ★ 10-16 작업 12a — 방 규칙. 판 중(결정 3)이면 원작이 닫힌 판에 쓰는 사유(`network.go:443-453`) · 꽉 찼으면 원작 인원 초과 사유(`network.go:409·418·427`)
bool CRoomManager::RoomAdmits(int roomId, const char*& why, ConnectError& err) {
    const CRoom& room = Room(roomId);
    switch (room._phase) {
        case RoomPhase::Waiting: break;
        case RoomPhase::Playing:
            ++_stats.rejectPlaying;
            why = "판 중";
            err = ConnectError::Closed;
            return false;
    }
    const int max = room._settings.maxPlayers;
    if (max > 0 && SeatsIn(roomId) >= static_cast<size_t>(max)) {
        ++_stats.rejectFull;
        why = "꽉 참";
        err = ConnectError::Full;
        return false;
    }
    return true;
}

void CRoomManager::AddMember(CRoom& room, int64_t sessionId, const std::string& account) {
    const bool wasEmpty = room._members.empty();
    room._members.push_back(RoomMember{ sessionId, account });
    if (wasEmpty) {
        ++_stats.hostChanges;
        NoteHostName(room);
        LogHost(room, "첫 사람");
    }
}

// ★ 10-16 결함 ② — 방장 이름(`CRoom::HostName`)을 지금 방장으로. 이름 없는 사람(열린 방식)이 방장이면 그대로 둔다 —
//   로비가 짓는 채널 이름이 빈 이름(`#'s_game`)이 되지 않게
void CRoomManager::NoteHostName(CRoom& room) {
    const RoomMember* h = room.Host();
    if (!h || h->account.empty() || h->account == room._hostName) return;
    room._hostName = h->account;
    ++_stats.hostRenames;
}

void CRoomManager::RemoveMember(CRoom& room, int64_t sessionId) {
    auto& ms = room._members;
    for (size_t i = 0; i < ms.size(); ++i) {
        if (ms[i].sessionId != sessionId) continue;
        ms.erase(ms.begin() + static_cast<long>(i));
        if (i == 0 && !ms.empty()) {   // 방장이 나갔다 — 다음 사람(들어온 차례)
            ++_stats.hostChanges;
            NoteHostName(room);
            LogHost(room, "방장이 나감");
            // ★ 10-16 작업 12b — 남은 사람에게 알림(게임 요청으로 쌓여 다음 옮기기 때 나간다)
            char line[96];
            std::snprintf(line, sizeof(line), "%s is now the host.", ms.front().account.empty() ? "Next player" : ms.front().account.c_str());
            room._game.SendNotice(line);
        }
        return;
    }
}

void CRoomManager::LogHost(const CRoom& room, const char* why) const {
    if (!_logRooms) return;
    const RoomMember* h = room.Host();
    if (!h) return;
    std::printf("[방] %d 방장 → %s(세션 %lld) — %s\n", room._id, h->account.empty() ? "(이름 없음)" : h->account.c_str(),
                static_cast<long long>(h->sessionId), why);
    std::fflush(stdout);
}

size_t CRoomManager::OnNetworkEvent(const NetworkEvent& ev) {
    // 접속 시도는 방으로 넘기지 않는다 — 받아 줄지는 방 관리자가 정한다(사람 자리도 만들지 않는다 · 플랜 A 조각 5c)
    if (ev.type == NetworkEvent::Type::JOIN_REQUESTED) {
        OnJoinRequested(ev);
        return 0;
    }
    // ★ 10-14 작업 8c — 로비 TCP 사건은 로비 몫이다(세션 번호가 없다). 여기로 오면 세지만 세션표에 안 넣는다
    if (IsLobbyEvent(ev.type)) {
        ++_stats.lobbyEventsMisrouted;
        return 0;
    }
    auto it = _seats.find(ev.sessionId);
    if (it != _seats.end()) return Forward(it->second, ev);
    // 세션표에 없는 세션
    if (_openEntry) {
        // 열린 방식 — 모두 방 0. `CONNECTED` 때 바로 넘겨 세션 시작 프레임이 플랜 A 끝과 같다(`ROOMS_DESIGN.md`)
        //   (`CONNECTED` 없이 오는 것 — 옛 동작대로 방 0 이 받는다)
        // ★ 10-14 작업 9d — 「방 0」 대신 정해 준 방. 그 방이 없으면(닫힘) 받지 않는다 — `CONNECTED` 는 끊고 나머지는 버린다
        if (!HasRoom(_openRoom)) {
            ++_stats.openNoRoom;
            if (ev.type == NetworkEvent::Type::CONNECTED) {
                ++_stats.connectRejected;
                RequestDisconnectSession(ev.sessionId);
            }
            return 0;
        }
        if (ev.type == NetworkEvent::Type::CONNECTED) {
            // ★ 10-16 작업 12a — 방 규칙(판 중 · 꽉 참) — 접속 시도 뒤에 바뀌었을 수 있다
            const char* why = "";
            ConnectError err = ConnectError::Closed;
            if (!RoomAdmits(_openRoom, why, err)) {
                ++_stats.connectRejected;
                RequestDisconnectSession(ev.sessionId);
                return 0;
            }
        }
        _seats[ev.sessionId] = _openRoom;
        AddMember(Room(_openRoom), ev.sessionId, std::string());   // ★ 10-16 작업 12a — 세션표에 넣는 곳마다(구성원 = 세션표의 이 방 세션)
        if (ev.type == NetworkEvent::Type::CONNECTED) ++_stats.assigned;
        return Forward(_openRoom, ev);
    }
    if (ev.type == NetworkEvent::Type::CONNECTED) {
        // 토큰 방식 — 같은 토큰을 이번엔 써 버리고 그 방에 바로 넘긴다. 틀리면 끊는다(접속 수락은 통신 층이 이미 보냈다)
        JoinToken tok;
        const char* why = "";
        ConnectError err = ConnectError::Closed;
        const int roomId = AdmitByToken(ev, /*consume=*/true, tok, why, err);
        if (roomId == kNoRoom) {
            ++_stats.connectRejected;
            RequestDisconnectSession(ev.sessionId);
            LogJoin("CONNECT 끊음", ev, roomId, tok, why);
            return 0;
        }
        _seats[ev.sessionId] = roomId;
        ++_stats.assigned;
        AddMember(Room(roomId), ev.sessionId, tok.account);   // ★ 10-16 작업 12a
        LogJoin("방 배정", ev, roomId, tok, why);
        return Forward(roomId, ev);
    }
    ++_stats.droppedUnknown;   // 토큰 방식 — 방에 안 든 세션의 이벤트(끊은 세션에 늦게 온 것 등)
    return 0;
}

size_t CRoomManager::Forward(int roomId, const NetworkEvent& ev) {
    CRoom& room = Room(roomId);
    const size_t n = room._game.OnNetworkEvent(ev);
    room.HandleChatCommands();   // ★ 10-16 작업 12b — 그 이벤트에 든 채팅 명령(알림은 게임 요청으로 쌓여 아래에서 같이 옮겨진다)
    PullRequests(room);   // 처리한 차례대로 — 다음 이벤트에서 방 관리자가 낼 요청(접속 답)보다 앞에 둔다
    return n;
}

int CRoomManager::AdmitByToken(const NetworkEvent& ev, bool consume, JoinToken& tok, const char*& why, ConnectError& err) {
    err = ConnectError::Closed;   // 토큰이 틀린 갈래는 모두 이 사유(10-10 — 원작이 명단 밖을 돌려보내는 메시지)
    if (!_hasKey) { ++_stats.tokenNoKey; why = "키 없음(설정 빠짐)"; return kNoRoom; }
    if (ev.token.empty()) { ++_stats.tokenMissing; why = "토큰 없음"; return kNoRoom; }
    uint32_t ip = 0;
    if (!ParseIpv4(IpOf(ev.addr), ip)) { ++_stats.tokenBadAddr; why = "주소"; return kNoRoom; }
    const uint32_t now = _unixNow();
    const JoinTokenCheck c = VerifyJoinToken(_key, ev.token.data(), ev.token.size(), now, ip, tok);
    ++_stats.tokenCheck[static_cast<int>(c)];
    if (c != JoinTokenCheck::Ok) { why = JoinTokenCheckName(c); return kNoRoom; }
    if (!HasRoom(tok.roomId)) { ++_stats.tokenNoRoom; why = "방 없음"; return kNoRoom; }
    if (!RoomAdmits(tok.roomId, why, err)) return kNoRoom;   // ★ 10-16 작업 12a — 일회용 번호를 쓰기 전에(거절된 토큰은 안 씀)
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
    CloseEmptyRooms(nowMs);   // ★ 10-14 작업 9e
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
        if (!over && room._wasRoundOver) room.OnRoundRestarted();   // ★ 10-16 작업 13b — 판 끝 뒤 다시 열림(판이었으면 대기로)
        room._wasRoundOver = over;
        room.CheckMatchHeadcount();   // ★ 10-16 결함 ④ — 나감은 위 옮기기(`PullRequests`)에서 구성원에서도 빠졌다 · 돌렸으면 그 요청은 다음 옮기기 때
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
        if (!HasRoom(_openRoom)) {   // ★ 10-14 작업 9d — 정해 준 방이 없으면(닫힘) 거절 — 토큰의 방이 닫혔을 때와 같은 답
            ++_stats.openNoRoom;
            ++_stats.joinRejected;
            RespondJoin(ev.sessionId, /*accept=*/false,
                        { static_cast<uint8_t>(Op::SERVER_ERROR), static_cast<uint8_t>(ConnectError::Closed) });
            return;
        }
        // ★ 10-16 작업 12a — 방 규칙(판 중 · 꽉 참)
        const char* why = "";
        ConnectError err = ConnectError::Closed;
        if (!RoomAdmits(_openRoom, why, err)) {
            ++_stats.joinRejected;
            RespondJoin(ev.sessionId, /*accept=*/false, { static_cast<uint8_t>(Op::SERVER_ERROR), static_cast<uint8_t>(err) });
            return;
        }
        ++_stats.joinAccepted;
        RespondJoin(ev.sessionId, /*accept=*/true, {});
        return;
    }
    JoinToken tok;
    const char* why = "";
    ConnectError err = ConnectError::Closed;
    const int roomId = AdmitByToken(ev, /*consume=*/false, tok, why, err);
    if (roomId != kNoRoom) {
        ++_stats.joinAccepted;
        RespondJoin(ev.sessionId, /*accept=*/true, {});
        LogJoin("접속 시도 받음", ev, roomId, tok, why);
        return;
    }
    ++_stats.joinRejected;
    RespondJoin(ev.sessionId, /*accept=*/false, { static_cast<uint8_t>(Op::SERVER_ERROR), static_cast<uint8_t>(err) });
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
        if (q.type == NetworkRequest::Type::DISCONNECT_SESSION && _seats.erase(q.sessionId) != 0) {
            ++_stats.left;
            RemoveMember(room, q.sessionId);   // ★ 10-16 작업 12a — 방장이면 다음 사람
        }
        if (q.type == NetworkRequest::Type::SEND_MSG) ++room._sendRequests;   // ★ 10-12 작업 7c — 방마다 보낸 수
        _requests.push_back(std::move(q));
    }
    _pull.clear();
}

}  // namespace nox
