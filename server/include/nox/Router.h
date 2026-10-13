// Router.h — 라우터 로직(로비 찾기 · TCP 4005). 목표 구조 v3 의 첫 층(`Docs/ROOMS_DESIGN.md` 「그림」 ①).
//
// ★ 10-14 플랜 B 작업 8b — 옛 `Lobby::OnServServ`(4005 몫)를 떼어 냈다. 정품 클라는 로비 주소를 두 번 거쳐 얻는다 —
//   `servserv.westwood.com:4005` 에 묻고(이 라우터), `605` 답의 주소로 로그인한다(채널 · 4000). 라우터는 방도 로그인도 모른다.
//
// 소켓도 시간도 모른다 — 받은 줄 하나를 넣으면 보낼 줄 목록을 돌려준다(`Lobby` 와 같은 짜임 · 소켓은 `nox_router` 의 `CTcpLines`).
// 답의 꼴은 1단계 녹음(XWIS 원래 줄) 그대로 — verchk ×2 · lobcount · whereto · QUIT → 610 · 605 · 607
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nox/Lobby.h"   // `Out` · `IpDotted` · `kLobbyPort`

namespace nox::wol {

struct RouterConfig {
    std::string serverName = "Nox_Origin";   // 605 답의 '0:<이름>'
    // 605 로 알려 줄 채널 주소. 0 이면 「클라가 라우터에 붙은 주소」 — 라우터와 채널이 같은 PC 일 때(설정 `[router] channel` 이 없을 때)
    uint32_t channelIp = 0;
    uint16_t channelPort = kLobbyPort;
};

class Router {
public:
    explicit Router(RouterConfig cfg) : cfg_(std::move(cfg)) {}

    // peerIp = 상대 IPv4 · localIp = 클라가 붙은 내 쪽 IPv4(채널 주소를 설정하지 않았을 때 605 답에 싣는다)
    void OnConnect(int conn, uint32_t peerIp, uint32_t localIp);
    void OnLine(int conn, std::string_view line, std::vector<Out>& out);
    void OnClose(int conn);

    // 소켓 루프가 찍을 기록(처음 본 명령). 꺼내면 비운다.
    std::vector<std::string> TakeLog();
    const RouterConfig& Config() const { return cfg_; }

private:
    struct Conn {
        uint32_t peerIp = 0;
        uint32_t localIp = 0;
    };

    RouterConfig cfg_;
    std::map<int, Conn> conns_;
    std::set<std::string> unknownSeen_;
    std::vector<std::string> log_;
};

}  // namespace nox::wol
