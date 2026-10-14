// ChannelLobby.h — 채널 안 로비(목표 구조 v3 — `Docs/ROOMS_DESIGN.md` 「그림」의 게임 스레드 `CLobby`).
//
// ★ 10-14 플랜 B 작업 8d — 옛 `nox_lobby`(따로 뜬 로비 프로세스)를 채널(`nox_server`)의 게임 스레드로 옮겼다.
//   · 로비 TCP 4000 소켓은 네트워크 스레드가 든다(`CNetworkThread::OpenLobby`) — 사건(`LOBBY_*`)이 같은 큐로 온다
//   · 이 객체는 그 사건을 로비 규약(`wol::Lobby` — 녹음과 대조된 그 로직 그대로)에 넘기고, 돌려받은 줄을 요청(`LOBBY_SEND`)으로 낸다
//   · 로비가 그리는 방은 방 관리자의 돌고 있는 방(`CRoomManager::ListRooms`) — 방을 만든 사람이 있는 방만(로비가 만든 방 · 작업 10).
//     실행 인자의 방(단독 모드)은 로비 목록에 안 나온다. 방 목록의 정본은 방 관리자 하나다(설정 파일 `[rooms]` 없앰)
// 소켓도 시간도 모른다 — 게임 스레드 한 곳에서만 부른다.
#pragma once

#include <string>
#include <vector>

#include "nox/Lobby.h"
#include "nox/RoomManager.h"
#include "nox/UdpServer.h"

namespace nox {

// 채널 로비의 셈 — 줄 단위(로비 규약의 셈은 `wol::LobbyStats`)
struct CChannelLobbyStats {
    unsigned long long opened = 0;    // 새 접속
    unsigned long long linesIn = 0;   // 받은 줄
    unsigned long long linesOut = 0;  // 낸 줄(`LOBBY_SEND`)
    unsigned long long closed = 0;    // 끊김
};

class CChannelLobby final : private wol::ILobbyRooms {
public:
    // `rooms` 는 이 객체보다 오래 산다
    CChannelLobby(CRoomManager& rooms, wol::LobbyConfig cfg);
    CChannelLobby(const CChannelLobby&) = delete;
    CChannelLobby& operator=(const CChannelLobby&) = delete;

    // 로비 사건 하나(`IsLobbyEvent`) → 보낼 줄을 `out` 뒤에 요청으로. 로비 사건이 아니면 아무것도 안 한다
    void OnNetworkEvent(const NetworkEvent& ev, std::vector<NetworkRequest>& out);
    // 접속 · 받은 줄 · 보낸 줄 · 끊김을 한 줄씩 찍는가(`--quiet` 반대 — 옛 `nox_lobby` 의 `[cN:4000]` 줄). 로비 기록(`[로비]`)은 늘 찍는다
    void SetVerbose(bool v) { _verbose = v; }
    const CChannelLobbyStats& Stats() const { return _stats; }
    const wol::LobbyStats& LobbyStats() const { return _lobby.Stats(); }

private:
    std::vector<wol::LobbyRoom> LobbyRooms() const override;
    void Flush(std::vector<NetworkRequest>& out);   // 로비가 돌려준 줄 → 요청 · 로비 기록 찍기

    CRoomManager& _rooms;
    wol::Lobby _lobby;          // 방은 이 객체가 준다(`ILobbyRooms`)
    bool _verbose = false;
    CChannelLobbyStats _stats;
    std::vector<wol::Out> _outs;   // 다시 쓰는 그릇
};

}  // namespace nox
