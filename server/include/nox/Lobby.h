// Lobby.h — 로비 서버 로직 (MO 2단계, Docs/LOBBY_DESIGN.md).
//
// 소켓도 시간도 모른다 — 받은 줄 하나를 넣으면 보낼 줄 목록을 돌려준다(게임 서버의 `CSyncHost::OnPacket` 과 같은 짜임).
// 그래서 1단계 녹음의 정품 클라 줄을 그대로 넣어 XWIS 원래 답과 대조할 수 있다(`lobby_replay` 시험).
//
// 2단계 범위 = 한 사람 한 바퀴: 로그인 → 방 목록 → 채팅 로비 입장 → 게임 버튼 → 고정 방 하나 참가.
//   빠진 것 = 방 만들기 · 채팅 글 중계 · 같은 채널 다른 사람에게 입·퇴장 알리기(녹음이 없다 — 4단계).
// ★ 10-10 토큰 작업 3 — 입장 토큰(`Docs/TOKEN_DESIGN.md`). 로비는 토큰 방식 하나뿐이다.
//   · 클라 DLL(`tools/wsock32_proxy`)이 4000 로그인 앞에 `NOXCLIENT <판>` 한 줄을 끼운다. 그 줄이 없거나 판이 낮으면
//     로그인은 받되 인사말(372)에 패치가 필요하다고 싣고, 방 참가엔 토큰 없이 답한다 — 클라는 게임 서버에서 거절된다.
//     ★ 10-11 — 처음엔 로그인 끝(`USER`)에 `ERROR` 로 끊었는데 정품 화면은 아무 말 없이 「Connecting…」에서 멈췄다(사람 확인)
//   · 방 참가(`JOINGAME`) 답 바로 앞에 `NOXTOKEN <게임 서버 IP:포트> <토큰 16진>` — DLL 이 떼어 쥐고 로비 모듈엔 원래 답만 간다.
//     토큰 = 방(게임 방 번호) · 클라 IP(이 TCP 상대) · 만료 · 일회용 번호 · 로비 이름을 키로 서명(`JoinToken.h`)
//   · 방은 여럿(설정 파일 `[rooms]` — 방장 이름 = 게임 방 번호). 방마다 채널 `#<방장>'s_game` · 방 정보 이름도 방장 이름
// ★ 10-14 플랜 B 작업 8b — 로비 찾기(4005 — 610 · 605 · 607)는 라우터(`Router.h` · `nox_router`)로 떼어 냈다. 로비는 4000 만 받는다
// ★ 10-14 플랜 B 작업 8d — 로비는 채널(`nox_server`) 안 게임 스레드에서 돈다(`CChannelLobby` 가 로비 TCP 사건을 넘긴다).
//   방 목록은 갖지 않는다 — 채널이 주는 돌고 있는 방(`ILobbyRooms`)으로 그린다(위 `[rooms]` 줄은 옛 설명 — 설정 파일 방 목록 없앰).
//   방 정보의 이름 · 맵 · 모드 · 인원도 그 방에서. 게임 서버 주소는 이 접속이 붙은 내 쪽 주소(채널 = 게임 서버 · IP 하나에 채널 하나)
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nox/JoinToken.h"
#include "nox/WolGameInfo.h"

namespace nox::wol {

constexpr uint16_t kServServPort = 4005;  // 정품 모듈에 박힌 첫 주소의 포트(WOLAPI.dll "TCP;servserv.westwood.com;4005")
constexpr uint16_t kLobbyPort    = 4000;  // 605 답으로 알려 주는 로비 포트(XWIS 와 같게)
constexpr int kNoxClientMinVersion = 1;   // ★ 10-10 — DLL 이 알리는 판. 이보다 낮거나 없으면 로그인 거절

// ★ 10-14 플랜 B 작업 8d — 로비가 그리는 방 하나. 채널이 방 관리자의 돌고 있는 방(`CRoomManager::ListRooms`)에서 채운다
//   (옛 설정 파일 `[rooms]` 의 「방장 이름 = 게임 방 번호」 자리)
struct LobbyRoom {
    uint16_t gameRoomId = 0;   // 게임 서버의 방 번호 — 입장 토큰에 싣는다
    std::string creator;       // 방을 만든 사람의 로비 이름 — 채널 `#<만든 사람>'s_game` · 353 의 방장 자리(@)
    std::string name;          // 방 이름(UTF-8) — 방 정보의 이름 칸(비면 만든 사람 이름 — 옛 「방 정보 이름도 방장 이름」) · 실을 때 접속의 코드페이지로
    std::string map;           // 맵 열쇠(소문자 · 확장자 없음) — 방 정보의 맵 칸(비면 틀의 맵)
    std::string mode;          // 방 설정의 모드 — 방 정보 깃발의 맵 종류(`ModeFlagsOf` · 모르는 이름이면 틀의 깃발)
    uint8_t players = 0;       // 게임 안 사람 수 — 방 정보의 인원
};

// 채널이 로비에게 주는 방 — 로비는 목록을 그리거나 방 참가를 볼 때마다 묻는다(그 순간의 돌고 있는 방 · 방 번호 차례)
class ILobbyRooms {
public:
    virtual ~ILobbyRooms() = default;
    virtual std::vector<LobbyRoom> LobbyRooms() const = 0;
    // ★ 10-14 플랜 B 작업 10a — 「새 방」 — 그 사람이 만든 사람(방장)인 방을 하나 만든다(채널이 방 관리자의 만들기 입구로).
    //   만들었으면 true · `out` 에 그 방. 실패(대기 맵 없음 · 세계 차리기 실패)면 false
    virtual bool CreateRoomFor(const std::string& creator, LobbyRoom& out) = 0;
};

// ★ 10-14 작업 10a — 방 이름을 실을 때 쓰는 코드페이지(클라가 `SETCODEPAGE` 를 안 보냈을 때). 정품 클라는 949 를 보낸다(1단계 녹음 셋 모두)
constexpr int kDefaultCodepage = 949;

struct LobbyConfig {
    std::vector<std::string> motd{"Welcome to Nox_Origin!", ""};  // 첫 줄은 375, 나머지는 372
    // 게임 서버 IPv4 정수(a<<24|b<<16|c<<8|d) — 326·353·NOXTOKEN 에 같은 값.
    //   ★ 10-14 작업 8d — 0 이면 이 접속이 붙은 내 쪽 주소(채널이 곧 게임 서버). 옛 기본값 127.0.0.1(따로 뜬 로비 몫)
    uint32_t gameServerIp = 0;
    uint16_t gameServerPort = 18590;                // ★ 10-10 — NOXTOKEN 줄의 게임 서버 포트(DLL 이 이 주소로 가는 패킷에만 꼬리를 붙인다)
    GameInfo room;                                  // 방 정보 틀 — 방마다 이름 · 맵 · 모드 · 인원을 그 방 것으로 바꿔 싣는다(★ 8d)
    JoinKey joinKey{};                              // ★ 10-10 — 입장 토큰 서명 키(설정 파일 `[token] key` — 게임 서버와 같은 값)
    std::function<uint32_t()> unixNow = UnixNow;    // 토큰 만료의 기준 시각 — 시험이 정해 보려고
    std::function<uint64_t()> nonce;                // 일회용 번호 — 비면 BCrypt 난수
};

// 로비의 셈 — 입장 토큰 갈래
struct LobbyStats {
    unsigned long long tokensIssued = 0;     // `NOXTOKEN` 을 보냄
    unsigned long long loginNoClient = 0;    // 로그인 — `NOXCLIENT` 없음(인사말로 알림)
    unsigned long long loginOldClient = 0;   // 로그인 — DLL 판이 낮음(인사말로 알림)
    unsigned long long joinNoClient = 0;     // 방 참가 — DLL 없음(로그인 전 참가 포함) · 토큰 없이 답함
    unsigned long long tokenSignFailed = 0;  // 서명 실패(BCrypt) — 토큰 없이 답함
    unsigned long long roomsCreated = 0;     // ★ 10-14 작업 10a — 「새 방」 참가로 방을 만듦
    unsigned long long roomCreateFailed = 0; // ★ 10-14 작업 10a — 「새 방」인데 채널이 못 만듦(답 없음 · 기록)
    unsigned long long newRoomNoClient = 0;  // ★ 10-14 작업 10a — 「새 방」인데 DLL 이 없어 안 만듦(답 없음 · 기록 — 들어올 수 없는 방이라)
};

struct Out {
    int conn = 0;
    std::string line;          // CRLF 없이
    bool closeAfter = false;   // 이 줄을 보낸 뒤 끊는다(QUIT 답)
};

class Lobby {
public:
    // ★ 10-14 작업 8d — 방은 `rooms` 가 준다(채널 · 시험의 가짜). 로비보다 오래 살아야 한다
    Lobby(LobbyConfig cfg, ILobbyRooms& rooms);

    // peerIp = 상대 IPv4 · localIp = 클라가 붙은 내 쪽 IPv4
    void OnConnect(int conn, uint32_t peerIp, uint32_t localIp);
    void OnLine(int conn, std::string_view line, std::vector<Out>& out);
    void OnClose(int conn);

    // 소켓 루프가 찍을 기록(처음 본 명령 등). 꺼내면 비운다.
    std::vector<std::string> TakeLog();

    static std::string Channel(const LobbyRoom& r) { return "#" + r.creator + "'s_game"; }
    const LobbyStats& Stats() const { return stats_; }

private:
    struct Conn {
        uint32_t peerIp = 0;
        uint32_t localIp = 0;
        std::string nick;
        int codepage = 0;
        int clientVer = -1;  // ★ 10-10 — `NOXCLIENT <판>`(줄이 없으면 -1 — 「판 낮음」과 따로 센다)
    };

    void OnChat(int conn, Conn& c, const std::string& cmd, const std::vector<std::string>& t,
                std::string_view line, std::vector<Out>& out);
    void SendList(int conn, bool withChat, std::vector<Out>& out);
    std::string Members353(const std::string& channel, const std::string& host, uint32_t gameIp) const;
    // ★ 10-14 작업 8d — 채널 → 그 방(`rooms` 에서 · 없으면 nullptr)
    static const LobbyRoom* FindRoom(const std::vector<LobbyRoom>& rooms, const std::string& channel);
    bool IsRoomHost(const std::string& name) const;
    // 방 정보 덩어리(`Pack` — 86바이트, ':' 로 시작). ★ 10-14 작업 10a — 방 이름(UTF-8)은 그 접속의 코드페이지로 바꿔 싣는다
    std::string RoomInfo(const LobbyRoom& r, int codepage) const;
    uint32_t GameServerIp(const Conn& c) const { return cfg_.gameServerIp ? cfg_.gameServerIp : c.localIp; }
    void Unknown(const std::string& cmd, std::string_view line);

    LobbyConfig cfg_;
    ILobbyRooms& rooms_;                      // ★ 10-14 작업 8d — 옛 `roomInfo_`(설정 방마다 미리 싼 방 정보) 자리
    LobbyStats stats_;
    std::map<int, Conn> conns_;
    std::map<std::string, std::set<int>> members_;  // 채널 → 들어와 있는 접속
    std::set<std::string> unknownSeen_;
    std::vector<std::string> log_;
};

std::string IpDotted(uint32_t ip);

}  // namespace nox::wol
