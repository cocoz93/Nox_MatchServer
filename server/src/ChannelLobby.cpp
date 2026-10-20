// ChannelLobby.cpp — 채널 안 로비. 설명은 `ChannelLobby.h` 머리.
#include "nox/ChannelLobby.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace nox {

namespace {

// 클라 줄은 글자가 아닌 바이트(방 정보 덩어리 등)를 '.' 로 · 우리 기록(한글)은 제어 문자만 거른다 — 옛 `lobby_main.cpp` 의 둘
std::string Printable(const std::string& s, bool ours) {
    std::string r = s;
    for (char& c : r) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7f || (!ours && b > 0x7e)) c = '.';
    }
    return r;
}

}  // namespace

CChannelLobby::CChannelLobby(CRoomManager& rooms, wol::LobbyConfig cfg, RoomSettings roomTemplate, std::vector<std::string> waitMaps,
                             std::vector<std::string> battleMaps)
    : _rooms(rooms), _lobby(std::move(cfg), *this), _template(std::move(roomTemplate)), _waitMaps(std::move(waitMaps)),
      _battleMaps(std::move(battleMaps)), _rng(std::random_device{}()) {}

std::string CChannelLobby::RoomNameFor(const std::string& nick) {
    // ★ 10-16 — 영문(10-15 정품 확인 23 — 정품 목록에서 「의 게임」 한글이 깨짐 · `Docs/ROOMS_DESIGN.md` 「새 방 이름」의 대안 그대로)
    return nick.substr(0, kRoomNameNickBytes) + "'s game";
}

bool CChannelLobby::ToLobbyRoom(const RoomFact& f, wol::LobbyRoom& lr) {
    if (f.creator.empty()) return false;   // 로비가 만들지 않은 방(실행 인자의 방) — 목록에 안 낸다
    lr.gameRoomId = static_cast<uint16_t>(f.roomId);
    // ★ 10-16 결함 ② — 채널 · 목록 이름은 **지금 방장**(`RoomFact::host`). 만든 사람에 묶어 두면 판 중에 나간 옛 방장의
    //   `Host Game`(제 이름 채널)이 그 방(판 중)으로 가 거절됐다. 방장이 만든 사람이면 방 설정의 이름 그대로
    lr.host = f.host.empty() ? f.creator : f.host;
    lr.name = lr.host == f.creator ? f.name : RoomNameFor(lr.host);
    lr.map = f.mapKey;
    lr.mode = f.mode;
    lr.players = static_cast<uint8_t>(std::min<size_t>(f.players, 255));
    lr.maxPlayers = static_cast<uint8_t>(std::min(std::max(f.maxPlayers, 0), 255));   // ★ 10-16 작업 12c
    lr.playing = f.phase == RoomPhase::Playing;
    return true;
}

std::vector<wol::LobbyRoom> CChannelLobby::LobbyRooms() const {
    std::vector<wol::LobbyRoom> r;
    for (const RoomFact& f : _rooms.ListRooms()) {
        wol::LobbyRoom lr;
        if (ToLobbyRoom(f, lr)) r.push_back(std::move(lr));
    }
    return r;
}

// ★ 10-14 작업 10a — 새 방: 틀 + 만든 사람 · 이름 · 대기 맵(무작위) · 채팅 모드 · 비면 닫음 → 방 관리자의 만들기 입구
// ★ 10-20 v2 작업 3a — 로비의 새 틀(`RequestRoom` — 결과 셋)에 맞춤. 같은 프로세스라 늘 그 자리에서 답한다(물음 없음) ·
//   방에 게임 서버 주소 · 키를 안 실어 옛 동작(붙은 내 쪽 주소 · 설정 토큰 키) 그대로. 작업 4 에서 이 객체를 걷는다
wol::RoomRequest CChannelLobby::RequestRoom(uint32_t /*req*/, const std::string& creator, wol::LobbyRoom& out, std::string& why) {
    if (!CreateRoomFor(creator, out)) {
        why = _waitMaps.empty() ? "대기 맵 후보 없음(--wait-maps)" : "채널이 방을 못 만듦(위 [방] 줄)";
        return wol::RoomRequest::Failed;
    }
    return wol::RoomRequest::Created;
}

bool CChannelLobby::CreateRoomFor(const std::string& creator, wol::LobbyRoom& out) {
    if (_waitMaps.empty()) {
        std::printf("[로비] 새 방을 못 만듦 — 대기 맵 후보가 없다(--wait-maps)\n");
        std::fflush(stdout);
        return false;
    }
    RoomSettings s = _template;
    s.creator = creator;
    s.roomName = RoomNameFor(creator);
    s.mapPath = _waitMaps[std::uniform_int_distribution<size_t>(0, _waitMaps.size() - 1)(_rng)];
    s.mode = "chat";        // 대기 = 원작 채팅 모드 맵(결정 1 — 싸움 막기는 작업 13)
    // ★ 10-16 작업 13a — 대전 맵(후보에서 무작위 · 없으면 빈 것 — `!start` 가 「대전 맵 없음」) · 아레나(「10-16 정한 것」)
    s.battleMapPath = _battleMaps.empty() ? std::string()
                                          : _battleMaps[std::uniform_int_distribution<size_t>(0, _battleMaps.size() - 1)(_rng)];
    s.battleMode = "arena";
    s.standalone = true;    // 맵을 직접 차린다(녹음 서두 없음)
    s.closeWhenEmpty = true;
    s.maxPlayers = kRoomMaxPlayers;   // ★ 10-16 작업 12a
    s.bots.clear();         // 봇은 실행 인자의 방 몫(네트워크 스레드를 띄운 뒤라 세울 수도 없다)
    const int id = _rooms.CreateRoom(s);
    if (id == CRoomManager::kNoRoom) return false;
    for (const RoomFact& f : _rooms.ListRooms())
        if (f.roomId == id) return ToLobbyRoom(f, out);
    return false;
}

void CChannelLobby::OnNetworkEvent(const NetworkEvent& ev, std::vector<NetworkRequest>& out) {
    switch (ev.type) {
        case NetworkEvent::Type::LOBBY_OPENED:
            ++_stats.opened;
            _lobby.OnConnect(ev.lobbyConn, ev.lobbyPeerIp, ev.lobbyLocalIp);
            if (_verbose) {
                std::printf("[로비 c%d] 접속 %s → 내 쪽 %s\n", ev.lobbyConn, wol::IpDotted(ev.lobbyPeerIp).c_str(),
                            wol::IpDotted(ev.lobbyLocalIp).c_str());
                std::fflush(stdout);
            }
            return;
        case NetworkEvent::Type::LOBBY_LINE:
            ++_stats.linesIn;
            if (_verbose) std::printf("[로비 c%d] >> %s\n", ev.lobbyConn, Printable(ev.lobbyLine, false).c_str());
            _outs.clear();
            _lobby.OnLine(ev.lobbyConn, ev.lobbyLine, _outs);
            Flush(out);
            return;
        case NetworkEvent::Type::LOBBY_CLOSED:
            ++_stats.closed;
            _lobby.OnClose(ev.lobbyConn);
            if (_verbose) {
                std::printf("[로비 c%d] 끊음 — %s\n", ev.lobbyConn, ev.lobbyWhy);
                std::fflush(stdout);
            }
            return;
        case NetworkEvent::Type::CONNECTED:
        case NetworkEvent::Type::DISCONNECTED:
        case NetworkEvent::Type::RECEIVED:
        case NetworkEvent::Type::JOIN_REQUESTED:
            return;   // 방 · 게임 몫(게임 스레드가 방 관리자로 나눈다)
    }
}

void CChannelLobby::Flush(std::vector<NetworkRequest>& out) {
    for (const wol::Out& o : _outs) {
        NetworkRequest r;
        r.type = NetworkRequest::Type::LOBBY_SEND;
        r.lobbyConn = o.conn;
        r.lobbyLine = o.line;
        r.lobbyCloseAfter = o.closeAfter;
        out.push_back(std::move(r));
        ++_stats.linesOut;
        if (_verbose) std::printf("[로비 c%d] << %s\n", o.conn, Printable(o.line, false).c_str());
    }
    _outs.clear();
    for (const std::string& l : _lobby.TakeLog()) std::printf("[로비] %s\n", Printable(l, true).c_str());
    std::fflush(stdout);
}

}  // namespace nox
