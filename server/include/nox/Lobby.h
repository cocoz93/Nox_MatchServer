// Lobby.h — 로비 서버 로직 (MO 2단계, Docs/LOBBY_DESIGN.md).
//
// 왜 이 꼴인가 — 정품 클라의 한계(클라 쪽 우회는 `client_mod/wsock32_proxy/wsock32_proxy.cpp` 머리 「왜 이 DLL 이 있나」)
//   · 정품 로비 모듈(WOLAPI.dll)은 Westwood Online 규약만 안다 → 답은 XWIS 녹음의 줄 꼴 그대로. 우리가 더하는 줄은 클라 DLL 이 떼어 정품 모듈에 안 간다
//   · 방 목록 줄(`326`)엔 게임 서버 IP 만 있고 포트 칸이 없다 → IP 하나에 게임 서버(채널) 하나
//   · 접속 시도에 방 칸이 없다 → 「누가 몇 번 방」은 로비가 서명한 입장 토큰이 알린다(`JoinToken.h`)
//   · 정품 `Host Game` 은 방장 PC 에 서버를 켠다 → 클라 DLL 이 제 이름 채널 참가로 돌리고, 로비는 그 참가에 새 방을 만든다(`JOINGAME`)
//
// 소켓도 시간도 모른다 — 받은 줄 하나를 넣으면 보낼 줄 목록을 돌려준다(게임 서버의 `CSyncHost::OnPacket` 과 같은 짜임).
// 그래서 1단계 녹음의 정품 클라 줄을 그대로 넣어 XWIS 원래 답과 대조할 수 있다(`lobby_replay` 시험).
//
// 2단계 범위 = 한 사람 한 바퀴: 로그인 → 방 목록 → 채팅 로비 입장 → 게임 버튼 → 고정 방 하나 참가.
//   빠진 것 = 방 만들기 · 채팅 글 중계 · 같은 채널 다른 사람에게 입·퇴장 알리기(녹음이 없다 — 4단계).
// ★ 10-10 토큰 작업 3 — 입장 토큰(`Docs/TOKEN_DESIGN.md`). 로비는 토큰 방식 하나뿐이다.
//   · 클라 DLL(`client_mod/wsock32_proxy`)이 4000 로그인 앞에 `NOXCLIENT <판>` 한 줄을 끼운다. 그 줄이 없거나 판이 낮으면
//     로그인은 받되 인사말(372)에 패치가 필요하다고 싣고, 방 참가엔 토큰 없이 답한다 — 클라는 게임 서버에서 거절된다.
//     ★ 10-11 — 처음엔 로그인 끝(`USER`)에 `ERROR` 로 끊었는데 정품 화면은 아무 말 없이 「Connecting…」에서 멈췄다(사람 확인)
//   · 방 참가(`JOINGAME`) 답 바로 앞에 `NOXTOKEN <게임 서버 IP:포트> <토큰 16진>` — DLL 이 떼어 쥐고 로비 모듈엔 원래 답만 간다.
//     토큰 = 방(게임 방 번호) · 클라 IP(이 TCP 상대) · 만료 · 일회용 번호 · 로비 이름을 키로 서명(`JoinToken.h`)
//   · 방은 여럿(설정 파일 `[rooms]` — 방장 이름 = 게임 방 번호). 방마다 채널 `#<방장>'s_game` · 방 정보 이름도 방장 이름
// ★ 10-14 플랜 B 작업 8b — 로비 찾기(4005 — 610 · 605 · 607)는 라우터(`Router.h` · `nox_router`)로 떼어 냈다. 로비는 4000 만 받는다
// ★ 10-14 플랜 B 작업 8d — 로비는 채널(`nox_server`) 안 게임 스레드에서 돈다(`CChannelLobby` 가 로비 TCP 사건을 넘긴다).
//   방 목록은 갖지 않는다 — 채널이 주는 돌고 있는 방(`ILobbyRooms`)으로 그린다(위 `[rooms]` 줄은 옛 설명 — 설정 파일 방 목록 없앰).
//   방 정보의 이름 · 맵 · 모드 · 인원도 그 방에서. 게임 서버 주소는 이 접속이 붙은 내 쪽 주소(채널 = 게임 서버 · IP 하나에 채널 하나)
// ★ 10-20 MO 6단계 v2 작업 3a(`Docs/LOBBY_SPLIT_DESIGN.md`) — 로비가 다시 따로 된 프로세스(`nox_lobby`)가 되며 방은 다른 프로세스(게임 서버)에 산다.
//   · 「새 방」은 묻고 기다린다 — `ILobbyRooms::RequestRoom` 이 곧바로 답하거나(채널 안 로비) 「물었음」을 돌려주고(게임 서버 거울),
//     답이 오면 `OnRoomCreated` · `OnRoomCreateFailed` 가 미뤄 둔 참가 답을 마저 보낸다. 답이 안 오면 `Tick` 이 시간 초과로 접는다.
//     로비는 게임 서버를 기다리지 않는다 — 기다리는 동안 다른 접속의 줄을 그대로 받는다(7단계 로그인 DB 답도 같은 틀)
//   · 방마다 게임 서버 주소 · 포트 · 토큰 키(`LobbyRoom::gameIp` · `gamePort` · `tokenKey`) — 토큰은 그 방이 사는 게임 서버의 키로 서명한다
// ★ 10-20 v2 작업 3b — 채팅 채널(배틀넷 꼴): 대화방 다섯 · 같은 채널 사람에게 드나듦 알림 · 글(`PRIVMSG`) 전달 — 위 2단계 「빠진 것」 중 둘을 채움
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nox/Clock.h"
#include "nox/JoinToken.h"
#include "nox/WolGameInfo.h"

namespace nox::wol {

constexpr uint16_t kServServPort = 4005;  // 정품 모듈에 박힌 첫 주소의 포트(WOLAPI.dll "TCP;servserv.westwood.com;4005")
constexpr uint16_t kLobbyPort    = 4000;  // 605 답으로 알려 주는 로비 포트(XWIS 와 같게)
constexpr int kNoxClientMinVersion = 1;   // ★ 10-10 — DLL 이 알리는 판. 이보다 낮거나 없으면 로그인 거절
// ★ 10-16 작업 11 — 이 판부터 DLL 이 `NOXSERVER` 줄을 떼어 「우리 로비」로 알아보고 Host Game 을 내 방 참가로 돌린다.
//   로비는 `NOXCLIENT` 가 이 판 이상이면 곧바로 `NOXSERVER 1` 한 줄로 답한다(낮은 판 DLL 은 이 줄을 못 떼므로 안 보낸다).
//   DLL 은 이 줄을 못 받은 로비(XWIS 등)에선 아무것도 돌리지 않는다 — 원작 Host Game 이 그대로 산다
constexpr int kNoxClientHostGameVersion = 2;

// ★ 10-14 플랜 B 작업 8d — 로비가 그리는 방 하나. 채널이 방 관리자의 돌고 있는 방(`CRoomManager::ListRooms`)에서 채운다
//   (옛 설정 파일 `[rooms]` 의 「방장 이름 = 게임 방 번호」 자리)
struct LobbyRoom {
    uint16_t gameRoomId = 0;   // 게임 서버의 방 번호 — 입장 토큰에 싣는다
    std::string host;          // 방장의 로비 이름 — 채널 `#<방장>'s_game` · 353 의 방장 자리(@)
                               //   ★ 10-16 결함 ② — 옛 이름 `creator`(만든 사람). 방장이 바뀌면 채널도 따라간다(채널이 `RoomFact::host` 로 채움)
    std::string name;          // 방 이름(UTF-8) — 방 정보의 이름 칸(비면 방장 이름 — 옛 「방 정보 이름도 방장 이름」) · 실을 때 접속의 코드페이지로
    std::string map;           // 맵 열쇠(소문자 · 확장자 없음) — 방 정보의 맵 칸(비면 틀의 맵)
    std::string mode;          // 방 설정의 모드 — 방 정보 깃발의 맵 종류(`ModeFlagsOf` · 모르는 이름이면 틀의 깃발)
    uint8_t players = 0;       // 게임 안 사람 수 — 방 정보의 인원
    uint8_t maxPlayers = 0;    // ★ 10-16 작업 12c — 방의 최대 인원(방 설정 · 0 이면 틀의 값)
    bool playing = false;      // ★ 10-16 작업 12c — 판 중이면 방 정보 `access` 1(닫힘 — 원작 칸 · 들어오면 게임 서버가 사유 4 로 거절)
    // ★ 10-20 v2 작업 3a — 그 방이 사는 게임 서버. 비워 두면(채널 안 로비) 옛 동작 — 주소 = 설정 · 붙은 내 쪽 주소 · 포트 = 설정 · 키 = 설정 토큰 키
    uint32_t gameIp = 0;       // 326 · 353 · NOXTOKEN 의 게임 서버 주소(0 = 옛 동작)
    uint16_t gamePort = 0;     // NOXTOKEN 의 포트(0 = `LobbyConfig::gameServerPort`)
    bool ownTokenKey = false;  // `tokenKey` 를 쓰나(false = `LobbyConfig::joinKey`)
    JoinKey tokenKey{};        // 그 게임 서버의 그 실행의 토큰 키(`GameServerTokenKey` — `LobbyLink.h`)
};

// ★ 10-20 v2 작업 3a — 새 방 요청의 곧바른 결과
enum class RoomRequest : uint8_t {
    Created,   // 곧바로 만듦(`out`) — 채널 안 로비
    Failed,    // 못 만듦(`why`) — 대기 맵 없음 · 게임 서버 없음 …
    Pending,   // 게임 서버에 물었음 — 답은 뒤에 `Lobby::OnRoomCreated` · `OnRoomCreateFailed` 로(같은 `req`)
};

// 채널이 로비에게 주는 방 — 로비는 목록을 그리거나 방 참가를 볼 때마다 묻는다(그 순간의 돌고 있는 방 · 방 번호 차례)
//   ★ 10-20 v2 — 로비 프로세스에선 게임 서버들의 알림으로 채운 거울(`RoomMirror`)이 준다
class ILobbyRooms {
public:
    virtual ~ILobbyRooms() = default;
    virtual std::vector<LobbyRoom> LobbyRooms() const = 0;
    // ★ 10-14 플랜 B 작업 10a — 「새 방」 — 그 사람이 만든 사람(방장)인 방을 하나 만든다.
    //   ★ 10-20 v2 작업 3a — 옛 `CreateRoomFor`(그 자리에서 만들고 bool). 이제 다른 프로세스에 물을 수 있어 결과가 셋(`RoomRequest`)
    virtual RoomRequest RequestRoom(uint32_t req, const std::string& creator, LobbyRoom& out, std::string& why) = 0;
    // ★ 10-20 v2 작업 3c — 로비가 기다림을 접은 요청(시간 초과 · 클라 끊김). 거울은 그 요청을 기다림 장부에서 지운다
    //   (안 지우면 게임 서버가 끊길 때 이미 접은 요청을 「실패」로 다시 넘긴다 — nox_lobby_smoke 에서 나옴). 늦게 온 답은 「안 물은 답」으로 버린다
    virtual void CancelRoom(uint32_t /*req*/) {}
};

// ★ 10-20 v2 작업 3a — 미룬 새 방 답을 기다리는 한도. 근거 없는 값 — 같은 PC 연결 왕복은 ms 단위 · 정품이 얼마나 기다리는지는 미확인
//   (넘으면 실패와 같은 꼴 — ★ 10-20 거절 답 뒤로는 `471` · 그 전엔 답이 없어 정품이 참가 화면에 머물렀다)
constexpr uint32_t kRoomRequestTimeoutMs = 5000;

// ★ 10-20 거절 답(`Docs/LOBBY_SPLIT_DESIGN.md` 「거절 답 플랜」) — 방 참가(`JOINGAME`)를 받아 줄 수 없는 까닭.
//   왜 — 답을 안 하면 정품이 「서버 접속 시도중...」에서 멈춘다(사람 확인 50). 정품이 받아 주는 거절 답은 `471` 하나뿐이다 —
//   정품 로비 모듈이 참가를 기다릴 때(`WOLAPI.dll` 상태 칸 1) 471 을 「채널 가득」(`0x8004006e`)으로 바꿔 게임에 넘기고,
//   게임 목록 화면(WOL 상태 8)은 그 값에만 「억세스 불가! — 서버가 다 찼습니다.」 창을 띄운 뒤 목록으로 돌아간다(`Game.exe` `0x41de91`).
//   474 · 475 · 478 은 그 화면에서 버려져 멈춤이 그대로다. 그래서 까닭이 무엇이든 숫자는 471 이고, 진짜 까닭은 로비 기록 · 셈 · 줄 끝 글에 남긴다
//   (정적 분석 근거 = `개인 메모` 「1 조사」)
//   ✕ DLL 없는 클라의 원래 `Host Game`(방 만들기 — 상태 칸 2)은 정품 게임 쪽이 실패 값을 전부 버려(`Game.exe` `0x40c1d0`) 받아 줄 답이 없다 — 그 갈래는 답을 안 한다
enum class JoinReject : uint8_t {
    RequestFailed,   // 새 방 · 곧바로 실패(등록된 게임 서버 없음 · 대기 맵 없음 …)
    CreateFailed,    // 새 방 · 물은 게임 서버가 실패로 답함(연결 끊김 · CREATE_FAILED · 만든 방의 ROOM 이 안 옴)
    Timeout,         // 새 방 · 한도(`kRoomRequestTimeoutMs`) 안에 답 없음
    NoSuchRoom,      // 없는 방(목록이 낡음 · 남의 이름 채널)
    kCount
};
constexpr size_t kJoinRejectCount = static_cast<size_t>(JoinReject::kCount);
constexpr int kJoinRejectNumeric = 471;   // 정품이 받아 주는 유일한 거절 숫자(위 주석)
// 거절 줄 끝 글(ASCII — 정품 화면엔 안 나오고 정품 기록 · DLL 기록에만 남는다)
const char* JoinRejectText(JoinReject why);

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
    std::function<uint32_t()> nowMs = NowMs;        // ★ 10-20 v2 작업 3a — 미룬 답 한도의 시계(ms · 32비트로 돈다) — 시험이 정해 보려고
};

// 로비의 셈 — 입장 토큰 갈래
struct LobbyStats {
    unsigned long long tokensIssued = 0;     // `NOXTOKEN` 을 보냄
    unsigned long long loginNoClient = 0;    // 로그인 — `NOXCLIENT` 없음(인사말로 알림)
    unsigned long long loginOldClient = 0;   // 로그인 — DLL 판이 낮음(인사말로 알림)
    unsigned long long joinNoClient = 0;     // 방 참가 — DLL 없음(로그인 전 참가 포함) · 토큰 없이 답함
    unsigned long long tokenSignFailed = 0;  // 서명 실패(BCrypt) — 토큰 없이 답함
    unsigned long long roomsCreated = 0;     // ★ 10-14 작업 10a — 「새 방」 참가로 방을 만듦
    unsigned long long roomCreateFailed = 0; // ★ 10-14 작업 10a — 「새 방」인데 채널이 못 만듦(기록 · ★ 10-20 거절 답 `471`) · ★ 10-20 — 미룬 요청의 실패 답 포함
    unsigned long long newRoomNoClient = 0;  // ★ 10-14 작업 10a — 「새 방」인데 DLL 이 없어 안 만듦(답 없음 · 기록 — 들어올 수 없는 방이라)
    // ★ 10-20 v2 작업 3a — 미룬 새 방 답의 갈래(요청 하나는 만듦 · 실패 · 시간 초과 · 클라 끊김 중 하나로 끝난다)
    unsigned long long roomsRequested = 0;   // 게임 서버에 물음(`Pending`)
    unsigned long long roomRequestTimeout = 0; // 한도 안에 답이 안 옴(★ 10-20 거절 답 `471`)
    unsigned long long roomRequestDropped = 0; // 기다리는 동안 클라가 끊음(답 버림 — 방은 게임 서버가 빈 방으로 닫는다)
    unsigned long long lateRoomReply = 0;    // 끝난 요청(시간 초과 · 끊김)의 답이 늦게 옴(버림)
    unsigned long long joinWhilePending = 0; // 새 방을 기다리는 접속이 또 참가(무시 — 답이 둘이 되지 않게)
    // ★ 10-20 v2 작업 3b — 채팅(`PRIVMSG`)
    unsigned long long chatMessages = 0;     // 같은 채널에 넘긴 글
    unsigned long long chatDelivered = 0;    // 받은 사람 줄 수(글 하나 × 채널의 다른 사람)
    unsigned long long chatNotMember = 0;    // 들어가 있지 않은 채널로 보냄(거름)
    unsigned long long chatMalformed = 0;    // ` :` 글 칸이 없음(처음 본 명령처럼 한 번 기록)
    // ★ 10-20 거절 답 — 보낸 `471` 을 까닭마다(`JoinReject` 차례). 보내는 자리는 `RejectJoin` 하나 · 부르는 자리 넷이 까닭을 고른다
    unsigned long long joinRejected[kJoinRejectCount] = {};
    unsigned long long JoinRejectedTotal() const {
        unsigned long long n = 0;
        for (const unsigned long long v : joinRejected) n += v;
        return n;
    }
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
    // ★ 10-20 v2 작업 3b — 끊긴 사람을 들어가 있던 대화방의 남은 사람들에게 PART 로 알린다(`out`)
    void OnClose(int conn, std::vector<Out>& out);
    // ★ 10-20 v2 작업 3a — 미룬 새 방의 답(`RequestRoom` 이 `Pending` 을 돌려준 `req`). 끝난 요청이면 버리고 센다
    void OnRoomCreated(uint32_t req, const LobbyRoom& room, std::vector<Out>& out);
    // ★ 10-20 거절 답 — 실패 · 시간 초과도 기다리던 클라에게 `471` 을 보내므로 보낼 줄(`out`)을 받는다
    void OnRoomCreateFailed(uint32_t req, const std::string& why, std::vector<Out>& out);
    // 미룬 답의 시간 초과를 본다 — 소켓 루프가 자주(줄이 없어도) 부른다
    void Tick(std::vector<Out>& out);
    size_t PendingRooms() const { return pending_.size(); }

    // 소켓 루프가 찍을 기록(처음 본 명령 등). 꺼내면 비운다.
    std::vector<std::string> TakeLog();

    static std::string Channel(const LobbyRoom& r) { return "#" + r.host + "'s_game"; }
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
    // ★ 10-20 v2 작업 3a — 그 방의 게임 서버 주소(방에 있으면 그것 · 없으면 옛 동작 — 설정 · 이 접속이 붙은 내 쪽 주소)
    uint32_t GameServerIp(const Conn& c, const LobbyRoom& r) const {
        return r.gameIp ? r.gameIp : cfg_.gameServerIp ? cfg_.gameServerIp : c.localIp;
    }
    // 방 참가 답(토큰 · JOINGAME · 332 · 353 · 366) — ★ 10-20 v2 작업 3a 미룬 새 방 답도 이 하나로
    void ReplyJoin(int conn, const Conn& c, const LobbyRoom& lr, std::vector<Out>& out);
    void JoinNewRoom(int conn, const Conn& c, const LobbyRoom& created, std::vector<Out>& out);
    // ★ 10-20 거절 답 — `: 471 u <채널> :<까닭 글>` 한 줄 · 까닭 셈(`LobbyStats::joinRejected`)
    void RejectJoin(int conn, const std::string& channel, JoinReject why, std::vector<Out>& out);
    void Unknown(const std::string& cmd, std::string_view line);

    LobbyConfig cfg_;
    ILobbyRooms& rooms_;                      // ★ 10-14 작업 8d — 옛 `roomInfo_`(설정 방마다 미리 싼 방 정보) 자리
    LobbyStats stats_;
    // ★ 10-20 v2 작업 3a — 미룬 새 방 답(요청 번호 → 기다리는 접속 · 한도 시각)
    struct PendingRoom {
        int conn = 0;
        uint32_t deadlineMs = 0;
        std::string channel;   // ★ 10-20 거절 답 — 실패 · 시간 초과 때 거절 줄에 싣는 참가 채널
    };
    std::map<uint32_t, PendingRoom> pending_;
    uint32_t nextReq_ = 1;
    std::map<int, Conn> conns_;
    std::map<std::string, std::set<int>> members_;  // 채널 → 들어와 있는 접속
    std::set<std::string> unknownSeen_;
    std::vector<std::string> log_;
};

std::string IpDotted(uint32_t ip);

}  // namespace nox::wol
