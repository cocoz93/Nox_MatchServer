// SyncHost.h — 한 스레드 조립. 통신 층(`CUdpServer`)과 게임(`nox::Server`)을 한 스레드에서 잇는다
//   (`Docs/LAYERS_DESIGN.md` 「조각 5d 설계」).
//
// ★ 10-09 조각 5d — 게임은 통신 층을 갖지 않고 와이어 조각도 만들지 않는다. 받은 조각은 통신 층이 일(`NetworkEvent`)로 바꾸고,
//   게임이 쌓은 요청(`NetworkRequest`)은 통신 층이 와이어 조각으로 바꾼다. 이 둘을 한 스레드에서 차례대로 잇는 것이 이 조립이다.
//   시험(`proto_test`) · 덤프 재생(`nox_replay` · `nox_verify`) · `--selftest` 가 쓰고, 조각 5d-2 전까지는 `main` 도 쓴다.
//   차례는 옛 `Server::OnPacket` · `Server::Tick` 그대로다(옛 게임이 그 자리에서 통신 층을 부르던 차례).
// ★ 10-09 플랜 B 작업 6a — 게임 자리에 방 관리자(`CRoomManager`)가 선다. 지금은 방 하나(방 0)라 동작은 그대로이고,
//   `Game()` · `host->X` 는 방 0 의 게임이다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nox/RoomManager.h"
#include "nox/Server.h"
#include "nox/UdpServer.h"

namespace nox {

class CSyncHost {
public:
    // ★ 10-10 토큰 작업 2 — 한 스레드 조립은 시험 · 녹음 재생·대조 · `--selftest` 몫이라 토큰 검사를 끈 채 시작한다
    //   (녹음에는 토큰이 없다 — `Docs/TOKEN_DESIGN.md`). 토큰 길을 보는 시험은 `Rooms().SetJoinKey` 로 켠다.
    CSyncHost() { _rooms.SetOpenEntry(); }

    CUdpServer& Network() { return _network; }
    const CUdpServer& Network() const { return _network; }
    CRoomManager& Rooms() { return _rooms; }
    Server& Game() { return _rooms.Game(); }
    const Server& Game() const { return _rooms.Game(); }
    // 시험 편의 — `host->X` 는 (방 0 의) 게임의 `X`
    Server* operator->() { return &_rooms.Game(); }
    const Server* operator->() const { return &_rooms.Game(); }

    // 받은 데이터그램 하나 — 통신 층 → 일 → 게임 → 요청 → 통신 층. 나간 조각은 `out` 에 붙인다.
    //   돌려주는 값 = 이 조각에서 읽은 메시지 수(통신 층이 읽은 접속 제어 + 게임이 읽은 게임 메시지).
    size_t OnPacket(const std::string& from, const uint8_t* data, size_t n, std::vector<OutPacket>& out);
    // 틱 — 게임 시계(`BeginTick`) → 틱이 돌면 통신 층 Tick(다시 보내기 · 시간 초과 알림 → 게임) → 게임 일(`RunTick`) → 요청.
    //   돌려주는 값 = 이번에 지나간 틱 수.
    int Tick(uint32_t nowMs, std::vector<OutPacket>& out);
    // 게임이 쌓은 요청을 통신 층에 차례대로 넘기고 나온 조각을 `out` 에 붙인다(지우지 않는다).
    void TakeOut(std::vector<OutPacket>& out);

    // 소켓 없이 사람을 세운다(시험·진단) — 통신 층이 번호를 매기고(`OpenSession`) 게임에 세운다. 돌려주는 값 = 세션 번호.
    int64_t DebugEnterGame(const std::string& address, uint16_t netCode, bool observer = false,
                           CharClass cls = CharClass::Wizard);
    // 그 세션에서 지금 무언가를 받은 것으로 친다(통신 층 시간 초과 시계만 — 10-d 시험용).
    void DebugTouch(int64_t sessionId) { _network.DebugTouch(sessionId); }

private:
    CUdpServer _network;
    CRoomManager _rooms;
    std::vector<NetworkRequest> _requests;   // `TakeOut` 이 쓰는 그릇(다시 씀)
};

}  // namespace nox
