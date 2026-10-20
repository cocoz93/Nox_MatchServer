// ChannelLobby.h — 채널 안 로비(목표 구조 v3 — `Docs/ROOMS_DESIGN.md` 「그림」의 게임 스레드 `CLobby`).
//
// ★ 10-14 플랜 B 작업 8d — 옛 `nox_lobby`(따로 뜬 로비 프로세스)를 채널(`nox_server`)의 게임 스레드로 옮겼다.
//   · 로비 TCP 4000 소켓은 네트워크 스레드가 든다(`CNetworkThread::OpenLobby`) — 사건(`LOBBY_*`)이 같은 큐로 온다
//   · 이 객체는 그 사건을 로비 규약(`wol::Lobby` — 녹음과 대조된 그 로직 그대로)에 넘기고, 돌려받은 줄을 요청(`LOBBY_SEND`)으로 낸다
//   · 로비가 그리는 방은 방 관리자의 돌고 있는 방(`CRoomManager::ListRooms`) — 방을 만든 사람이 있는 방만(로비가 만든 방 · 작업 10).
//     실행 인자의 방(단독 모드)은 로비 목록에 안 나온다. 방 목록의 정본은 방 관리자 하나다(설정 파일 `[rooms]` 없앰)
// 소켓도 시간도 모른다 — 게임 스레드 한 곳에서만 부른다.
// ★ 10-14 플랜 B 작업 10a — 「새 방」(로비가 `CreateRoomFor` 로 묻는다): 방 틀(실행 인자의 방 설정 — 진단 · 시야 손잡이)에
//   만든 사람 · 이름 「<닉>'s game」(10-16 — 옛 「<닉>의 게임」) · 대기 맵(후보에서 무작위) · 채팅 모드 · 비면 닫음을 넣어 방 관리자의 만들기 입구로(`CreateRoom`).
//   만든 사람 = 방장(`Docs/ROOMS_DESIGN.md` 결정 4 · 「10-13 정한 것」)
#pragma once

#include <random>
#include <string>
#include <vector>

#include "nox/Lobby.h"
#include "nox/RoomManager.h"
#include "nox/UdpServer.h"
#include "nox/WorldBuild.h"

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
    // ★ 10-14 작업 10a — 방 이름 「<닉>'s game」에서 닉은 이만큼(바이트)까지 — 이름 칸 15바이트 = 닉 8 + 「's game」 7
    //   (10-16 영문으로 바꿈 — 옛 「의 게임」도 CP949 7바이트라 값은 그대로)
    static constexpr size_t kRoomNameNickBytes = 8;
    // ★ 10-16 작업 12a — 로비가 만든 방의 최대 인원(`Docs/ROOMS_DESIGN.md` 「10-16 정한 것」 — 옛 목록 표시 31 은 판을 보고 고른 값이 아니었다)
    static constexpr int kRoomMaxPlayers = 8;

    // `rooms` 는 이 객체보다 오래 산다. `roomTemplate` = 새 방의 틀 · `waitMaps` = 대기 맵 후보(맵 파일 경로 — 방 관리자의 맵 목록에 있어야 한다)
    //   ★ 10-16 작업 13a — `battleMaps` = 대전 맵 후보(같은 꼴 · 새 방마다 하나를 방 설정 `battleMapPath` 에 — 비면 그 방은 판을 못 연다)
    CChannelLobby(CRoomManager& rooms, wol::LobbyConfig cfg, RoomSettings roomTemplate = {}, std::vector<std::string> waitMaps = {},
                  std::vector<std::string> battleMaps = {});
    // 대기 맵 고르기의 씨앗(시험이 정해 보려고 — 기본은 `std::random_device`)
    void SetSeed(uint32_t seed) { _rng.seed(seed); }
    // 「<닉>'s game」(로비가 접속의 코드페이지로 바꿔 싣는다 — 닉이 한글이면 그 부분만) · 닉이 길면 닉 쪽을 자른다
    static std::string RoomNameFor(const std::string& nick);
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
    // ★ 10-20 v2 작업 3a — 로비의 새 방 요청(늘 그 자리에서 Created · Failed — 같은 프로세스)
    wol::RoomRequest RequestRoom(uint32_t req, const std::string& creator, wol::LobbyRoom& out, std::string& why) override;
    bool CreateRoomFor(const std::string& creator, wol::LobbyRoom& out);   // ★ 10-14 작업 10a
    void Flush(std::vector<NetworkRequest>& out);   // 로비가 돌려준 줄 → 요청 · 로비 기록 찍기
    static bool ToLobbyRoom(const RoomFact& f, wol::LobbyRoom& out);   // 만든 사람이 있는 방만 true

    CRoomManager& _rooms;
    wol::Lobby _lobby;          // 방은 이 객체가 준다(`ILobbyRooms`)
    RoomSettings _template;     // ★ 10-14 작업 10a — 새 방의 틀
    std::vector<std::string> _waitMaps;
    std::vector<std::string> _battleMaps;   // ★ 10-16 작업 13a
    std::mt19937 _rng;
    bool _verbose = false;
    CChannelLobbyStats _stats;
    std::vector<wol::Out> _outs;   // 다시 쓰는 그릇
};

}  // namespace nox
