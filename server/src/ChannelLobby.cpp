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

CChannelLobby::CChannelLobby(CRoomManager& rooms, wol::LobbyConfig cfg) : _rooms(rooms), _lobby(std::move(cfg), *this) {}

std::vector<wol::LobbyRoom> CChannelLobby::LobbyRooms() const {
    std::vector<wol::LobbyRoom> r;
    for (const RoomFact& f : _rooms.ListRooms()) {
        if (f.creator.empty()) continue;   // 로비가 만들지 않은 방(실행 인자의 방) — 목록에 안 낸다
        wol::LobbyRoom lr;
        lr.gameRoomId = static_cast<uint16_t>(f.roomId);
        lr.creator = f.creator;
        lr.name = f.name;
        lr.map = f.mapKey;
        lr.mode = f.mode;
        lr.players = static_cast<uint8_t>(std::min<size_t>(f.players, 255));
        r.push_back(std::move(lr));
    }
    return r;
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
