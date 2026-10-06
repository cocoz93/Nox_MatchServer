// Lobby.h — 로비 서버 로직 (MO 2단계, Docs/LOBBY_DESIGN.md).
//
// 소켓도 시간도 모른다 — 받은 줄 하나를 넣으면 보낼 줄 목록을 돌려준다(게임 서버의 Server::OnPacket 과 같은 짜임).
// 그래서 1단계 녹음의 정품 클라 줄을 그대로 넣어 XWIS 원래 답과 대조할 수 있다(`lobby_replay` 시험).
//
// 2단계 범위 = 한 사람 한 바퀴: 로그인 → 방 목록 → 채팅 로비 입장 → 게임 버튼 → 고정 방 하나 참가.
//   빠진 것 = 방 만들기 · 채팅 글 중계 · 같은 채널 다른 사람에게 입·퇴장 알리기(녹음이 없다 — 4단계).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nox/WolGameInfo.h"

namespace nox::wol {

constexpr uint16_t kServServPort = 4005;  // 정품 모듈에 박힌 첫 주소의 포트(WOLAPI.dll "TCP;servserv.westwood.com;4005")
constexpr uint16_t kLobbyPort    = 4000;  // 605 답으로 알려 주는 로비 포트(XWIS 와 같게)

struct LobbyConfig {
    std::string serverName = "Nox_Origin";          // 605 답의 '0:<이름>'
    uint16_t servServPort = kServServPort;          // 이 포트로 들어온 접속은 「로비 찾기」로 다룬다
    uint16_t lobbyPort = kLobbyPort;                // 605 답으로 알려 주는 로비 포트
    std::vector<std::string> motd{"Welcome to Nox_Origin!", ""};  // 첫 줄은 375, 나머지는 372
    std::string roomHost = "NoxOrigin";             // 고정 방의 방장(가짜 사용자) — 채널 #<방장>'s_game
    uint32_t gameServerIp = 0x7f000001;             // 게임 서버 IPv4 정수(a<<24|b<<16|c<<8|d) — 326·353 두 곳에 같은 값
    GameInfo room;                                  // 고정 방 정보(맵·모드·인원) — nox_server 실행값과 맞출 것
};

struct Out {
    int conn = 0;
    std::string line;          // CRLF 없이
    bool closeAfter = false;   // 이 줄을 보낸 뒤 끊는다(QUIT 답)
};

class Lobby {
public:
    explicit Lobby(LobbyConfig cfg);

    // port = 받은 포트(4005/4000) · peerIp = 상대 IPv4 · localIp = 클라가 붙은 내 쪽 IPv4(605 답에 싣는다)
    void OnConnect(int conn, uint16_t port, uint32_t peerIp, uint32_t localIp);
    void OnLine(int conn, std::string_view line, std::vector<Out>& out);
    void OnClose(int conn);

    // 소켓 루프가 찍을 기록(처음 본 명령 등). 꺼내면 비운다.
    std::vector<std::string> TakeLog();

    std::string RoomChannel() const { return "#" + cfg_.roomHost + "'s_game"; }

private:
    struct Conn {
        uint16_t port = 0;
        uint32_t peerIp = 0;
        uint32_t localIp = 0;
        std::string nick;
        int codepage = 0;
    };

    void OnServServ(int conn, Conn& c, const std::string& cmd, std::string_view line, std::vector<Out>& out);
    void OnChat(int conn, Conn& c, const std::string& cmd, const std::vector<std::string>& t,
                std::string_view line, std::vector<Out>& out);
    void SendList(int conn, bool withChat, std::vector<Out>& out);
    std::string Members353(const std::string& channel, bool gameRoom) const;
    void Unknown(const std::string& cmd, std::string_view line);

    LobbyConfig cfg_;
    std::string roomInfo_;                          // Pack(cfg_.room) — 86바이트, ':' 로 시작
    std::map<int, Conn> conns_;
    std::map<std::string, std::set<int>> members_;  // 채널 → 들어와 있는 접속
    std::set<std::string> unknownSeen_;
    std::vector<std::string> log_;
};

std::string IpDotted(uint32_t ip);

}  // namespace nox::wol
