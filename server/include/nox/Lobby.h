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
// ★ 7단계 조각 6(`Docs/ACCOUNT_DB_DESIGN.md`) — 로그인은 저장소(`ILobbyStore` — `AttachStore`)의 계정 확인 답을 받고 끝낸다(아이디만 — 결정 2).
//   · `USER`(로그인 묶음의 끝)에 확인 요청 → 답이 오면 375 · 372 · 376. 그 사이 그 접속의 뒷줄(verchk · SETOPT · SETCODEPAGE …)은 쌓았다가
//     답 뒤 차례대로 — 정품은 USER 와 같은 묶음에 뒷줄을 미리 보내므로 녹음 차례(376 → 379 → 329)를 지키려면 쌓아야 한다
//   · 실패(저장소가 답을 못 냄 · 한도 · 이름 길이)는 XWIS 꼴 부드러운 실패 — 안내 372 + 376(로그인은 끝) · 방 참가는 471(결정 4)
//   · 토큰 · 새 방은 「확인됨」만(`USER` 없이 `NICK` + `JOINGAME` 만 보내도 토큰이 나가던 것 — 10-19 검사 지적 2)
//   · 로그인 마감 — 붙고 `kLoginDeadlineMs` 안에 `USER` 가 없으면 끊는다(접속만 하고 말없는 접속이 끝없이 남던 것 — 검사 지적 8)
//   · 저장소를 안 붙이면 옛 꼴(USER 에 곧바로 들여보냄 — 시험 · 녹음 대조용). 로비 실행 파일은 늘 붙인다(DB 또는 메모리 가짜)
// ★ 로그인 암호 조각 4(`Docs/LOGIN_AUTH_DESIGN.md`) — 위 조각 6 의 계정 확인 길(저장소에 묻고 답 기다리기 · 뒷줄 쌓기 · 한도)을 걷어냄(정할 것 a).
//   계정은 로그인 서버(nox_auth)가 쥐고, 클라 DLL 이 `apgar` 자리에 로그인 서버가 서명한 토큰(`NXL1…`)을 싣는다(`LoginLine.h` · `LoginToken.h`).
//   · `USER` 에서 그 토큰의 서명 · 만료 · IP · 이름 · 일회용을 곧바로 본다(DB 안 물음 · 기다림 없음) → 375 · 372 · 376
//   · 안 되면 XWIS 꼴 부드러운 실패(안내 372 + 376) — DLL 이 실은 까닭(`NXL!<까닭>` — 암호 틀림 · 계정 없음 → 가입 페이지 주소 …) · 토큰 갈래 · 손님 · 평문(DLL 낮음)
//     실패한 접속은 대화방을 볼 수만 있다(남에게 이름이 안 보이고 글을 못 보냄 — 확인 안 된 이름이 대화방에 서지 않게 · 정할 것 c) · 게임 참가는 471
//     ★ 10-28 — 실패는 들여보내지 않는다(사람 확인 54): 인사 대신 `378` 한 줄 → 정품 「틀린 아이디 또는 패스워드.」 창(`FinishLogin` 주석) ·
//     뒷줄 안 받음 · 클라가 안 끊으면 `kLoginRejectCloseMs` 뒤 끊음. 보기만은 USER 전 접속에만 남음
//   · 같은 계정이 또 들어오면 옛 접속을 끊는다(정할 것 b — 클라가 죽어 반쯤 열린 옛 연결이 남아도 다시 들어오게)
//   · `[auth] key` 가 없으면(`LobbyConfig::authOn` false) 아이디만 — 조각 6 이전 꼴(시험용 · 정할 것 c)
// ★ 랭크 매칭 조각 4(`Docs/RANK_MATCH_PLAN.md`) — 줄 세우기. DLL 판 4 의 `NOXRANK JOIN` 바로 뒤 내 방 참가(`JOINGAME #<닉>'s_game` — Host Game 과 같은 줄)는
//   새 방 대신 줄에 세우고 답을 미룬다(정품은 「서버 접속 시도중...」 창에서 기다림 — 한도 없음). 최소가 차면 모으기 시간 뒤 · 최대가 차면 곧바로
//   그 사람들을 묶어 랭크 방을 요청하고(`RoomAsk` 명단 · ranked), 방이 서면 묶인 사람마다 **제가 보낸 채널**로 참가 답 + 그 방 토큰
//   (353 방장 = 그 사람 — Host Game 답 꼴 그대로 · 토큰만 공용 방). 줄에서 빼는 것 셋 — `NOXRANK LEAVE`(창 닫힘) · `LIST` · 끊김
//   · 정품의 목록 자동 새로고침(2분)은 알림창이 떠 있으면 `LIST` 를 안 보낸다(`Game.exe` `0x437470` → `0x44a020`) — 기다리는 동안 `LIST` 는 안 옴(정적 분석)
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
#include "nox/LobbyStore.h"   // ★ 레이팅 조각 4 — 로그인 공지의 점수를 묻는다
#include "nox/LoginLine.h"
#include "nox/LoginToken.h"
#include "nox/WolGameInfo.h"

namespace nox::wol {

constexpr uint16_t kServServPort = 4005;  // 정품 모듈에 박힌 첫 주소의 포트(WOLAPI.dll "TCP;servserv.westwood.com;4005")
constexpr uint16_t kLobbyPort    = 4000;  // 605 답으로 알려 주는 로비 포트(XWIS 와 같게)
constexpr int kNoxClientMinVersion = 1;   // ★ 10-10 — DLL 이 알리는 판. 이보다 낮거나 없으면 로그인 거절
// ★ 10-16 작업 11 — 이 판부터 DLL 이 `NOXSERVER` 줄을 떼어 「우리 로비」로 알아보고 Host Game 을 내 방 참가로 돌린다.
//   로비는 `NOXCLIENT` 가 이 판 이상이면 곧바로 `NOXSERVER 1` 한 줄로 답한다(낮은 판 DLL 은 이 줄을 못 떼므로 안 보낸다).
//   DLL 은 이 줄을 못 받은 로비(XWIS 등)에선 아무것도 돌리지 않는다 — 원작 Host Game 이 그대로 산다
constexpr int kNoxClientHostGameVersion = 2;
// ★ 로그인 암호 조각 4 — 이 판부터 DLL 이 `apgar` 자리를 로그인 토큰으로 바꿔 싣는다(낮은 판은 평문 — 암호 판 로비는 실패 안내)
constexpr int kNoxClientAuthVersion = 3;
// ★ 랭크 매칭 조각 4(`Docs/RANK_MATCH_PLAN.md`) — 이 판부터 DLL 이 게임 목록 「랭크 매칭」 참가 줄 앞에 `NOXRANK JOIN`,
//   「서버 접속 시도중...」 창이 닫히면 다음 줄 앞에 `NOXRANK LEAVE` 를 끼운다(`wsock32_proxy.cpp` ⑪). 낮은 판의 표시는 안 믿는다
constexpr int kNoxClientRankVersion = 4;
// ★ 매칭 손보기 6(`Docs/MATCH_UX_REVIEW.md`) — 이 판부터 DLL 이 로비의 `NOXRANK FOUND` 에 기다림 창을 「수락?」 예 · 아니오로 바꾸고 예면 `NOXRANK ACCEPT`.
//   묶음의 방이 서면 아직 요청 중인 사람이 둘 이상이고 다 이 판 이상일 때만 수락 단계(하나라도 낮거나 한 사람만 남았으면 옛 길 — 곧바로 참가 답)
constexpr int kNoxClientAcceptVersion = 5;
// 수락 한도 — 플랜 「10초 무응답」. 넘으면 수락한 사람은 줄 앞에 되돌리고(`NOXRANK WAIT`) 안 한 사람은 줄에서 뺀다(`NOXRANK EXPIRED`)
constexpr uint32_t kRankAcceptMs = 10000;
constexpr uint16_t kDefaultAuthPort = 8443;   // 로그인 서버(nox_auth) — 가입 페이지 주소 안내에 씀(`[auth] port`)

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
    // ★ 매칭 손보기 7(`Docs/MATCH_UX_REVIEW.md`) — 로비가 랭크 줄로 청해 선 방. 목록(`LIST`)에서 뺀다 — 명단 밖 사람은 들어가도 게임 서버가 거절(사유 4)
    bool ranked = false;
};

// ★ 10-20 v2 작업 3a — 새 방 요청의 곧바른 결과
enum class RoomRequest : uint8_t {
    Created,   // 곧바로 만듦(`out`) — 채널 안 로비
    Failed,    // 못 만듦(`why`) — 대기 맵 없음 · 게임 서버 없음 …
    Pending,   // 게임 서버에 물었음 — 답은 뒤에 `Lobby::OnRoomCreated` · `OnRoomCreateFailed` 로(같은 `req`)
};

// ★ 랭크 매칭 조각 4 — 새 방 요청의 꼴. 일반 = 만든 사람 하나(제 이름 채널 참가 — Host Game) · 랭크 = 줄에서 묶은 명단 · ranked
struct RoomAsk {
    std::string creator;               // 방장(랭크면 명단 첫 사람 = 줄에 먼저 선 사람)
    std::vector<std::string> roster;   // 랭크 — 들어올 사람의 로비 이름 전부(차례 = 줄 차례) · 일반은 비움
    bool ranked = false;
    std::vector<int32_t> ratings;      // ★ 매칭 손보기 5 — 명단 사람마다 점수(같은 차례 · 로비가 쥔 값 — 모르면 시작 점수) · 일반은 비움
};

// 채널이 로비에게 주는 방 — 로비는 목록을 그리거나 방 참가를 볼 때마다 묻는다(그 순간의 돌고 있는 방 · 방 번호 차례)
//   ★ 10-20 v2 — 로비 프로세스에선 게임 서버들의 알림으로 채운 거울(`RoomMirror`)이 준다
class ILobbyRooms {
public:
    virtual ~ILobbyRooms() = default;
    virtual std::vector<LobbyRoom> LobbyRooms() const = 0;
    // ★ 10-14 플랜 B 작업 10a — 「새 방」 — 그 사람이 만든 사람(방장)인 방을 하나 만든다.
    //   ★ 10-20 v2 작업 3a — 옛 `CreateRoomFor`(그 자리에서 만들고 bool). 이제 다른 프로세스에 물을 수 있어 결과가 셋(`RoomRequest`)
    //   ★ 랭크 매칭 조각 4 — 만든 사람 하나 → 요청 꼴(`RoomAsk` — 랭크 명단 · ranked)
    virtual RoomRequest RequestRoom(uint32_t req, const RoomAsk& ask, LobbyRoom& out, std::string& why) = 0;
    // ★ 10-20 v2 작업 3c — 로비가 기다림을 접은 요청(시간 초과 · 클라 끊김). 거울은 그 요청을 기다림 장부에서 지운다
    //   (안 지우면 게임 서버가 끊길 때 이미 접은 요청을 「실패」로 다시 넘긴다 — nox_lobby_smoke 에서 나옴). 늦게 온 답은 「안 물은 답」으로 버린다
    virtual void CancelRoom(uint32_t /*req*/) {}
};

// ★ 10-20 v2 작업 3a — 미룬 새 방 답을 기다리는 한도. 근거 없는 값 — 같은 PC 연결 왕복은 ms 단위 · 정품이 얼마나 기다리는지는 미확인
//   (넘으면 실패와 같은 꼴 — ★ 10-20 거절 답 뒤로는 `471` · 그 전엔 답이 없어 정품이 참가 화면에 머물렀다)
constexpr uint32_t kRoomRequestTimeoutMs = 5000;
// ★ 7단계 조각 6 — 로그인. 근거 없는 값(정품이 로그인 묶음을 얼마나 오래 나눠 보내는지 미확인)
//   ★ 로그인 암호 조각 4 — 계정 확인 답 한도 · 뒷줄 쌓기 한도는 걷음(기다릴 답이 없음)
constexpr uint32_t kLoginDeadlineMs = 30000;      // 붙고 USER 까지 한도(정품은 로그인 줄을 한 묶음으로 보낸다)
constexpr size_t kMaxNickBytes = 16;              // 계정 이름 한도 = 입장 토큰 이름 칸(`JoinToken.h`) — 넘으면 실패 길
// ★ 10-28 — 로그인 거절(378) 뒤 클라가 스스로 끊기를 기다리는 한도. 근거 = 433 답엔 클라가 33ms 뒤 끊음(10-11) · 464 엔 11~34초 멈춘 뒤 끊음 —
//   378 은 433 과 같은 길이라 곧바로 끊을 것으로 봄. 이 값은 그 짐작이 틀렸을 때 접속이 남지 않게 하는 안전판
constexpr uint32_t kLoginRejectCloseMs = 10000;
// ★ 레이팅 조각 4(`Docs/RATING_DESIGN.md` 「로그인 때 읽기」) — 토큰 로그인이면 저장소에 점수 · 순위를 묻고 그 답까지 인사(375 … 376)를 미룬다.
//   그 사이 같은 접속의 뒷줄(verchk · SETCODEPAGE …)은 쌓았다가 376 뒤 차례대로(7단계 조각 6 에 있던 것 — 로그인 암호 조각 4 에서 걷었다가 되살림).
//   한도 · 쌓기 한도는 옛 값 그대로 — 근거 없는 값(같은 PC DB 왕복은 ms 단위 · 녹음 접속 하나의 뒷줄은 많아야 18줄). 넘으면 점수 줄 없이 로그인을 끝낸다
constexpr uint32_t kRatingTimeoutMs = 5000;
constexpr size_t kMaxHeldLines = 64;
// ★ 랭크 매칭 조각 4 — 줄 세우기 기본값(`Docs/RANK_MATCH_PLAN.md` 정한 것 1 — 10-30 사용자). 설정 `[rank]` 로 바꾼다 · 부하 실험은 최소 = 최대 = 12
//   최소가 차면 모으기 시간만큼 더 기다렸다가 그때 줄 선 사람 전부(최대까지)로 · 최대가 차면 곧바로. 기다림 한도는 없다(정한 것 2 — 빠짐은 DLL 알림 · LIST · 끊김)
constexpr uint32_t kRankMinDefault = 2;
constexpr uint32_t kRankMaxDefault = 12;
constexpr uint32_t kRankGatherMsDefault = 30000;
constexpr uint32_t kRankMaxLimit = 31;   // 설정 최대의 위 한도 — 방 정보 틀의 최대 인원(로비 틀 31 · `hostgame::MyRoomInfo`)
// 랭크 방 요청이 실패 · 시간 초과로 사람들을 줄 앞에 되돌린 뒤 다시 묻기까지 쉬는 시간. 근거 없는 값 — 게임 서버 다시 붙기 3초 ·
//   붙는 한도 5초(`NetworkThread.h`)와 어울리게. 안 쉬면 게임 서버가 없을 때 곧바로 실패 → 되돌림 → 다시 물음이 한 바퀴 안에서 돈다
constexpr uint32_t kRankRetryMs = 5000;
// 랭크 줄에서 빠진 까닭(`LobbyStats::rankLeft` 차례) — 「서버 접속 시도중...」 창 닫힘(DLL) · 목록 다시 받기(LIST) · 접속 끊김
enum class RankLeave : uint8_t { Dll, List, Closed, kCount };
constexpr size_t kRankLeaveCount = static_cast<size_t>(RankLeave::kCount);
const char* RankLeaveName(RankLeave why);

// ★ 10-20 거절 답(`Docs/LOBBY_SPLIT_DESIGN.md` 「거절 답 플랜」) — 방 참가(`JOINGAME`)를 받아 줄 수 없는 까닭.
//   왜 — 답을 안 하면 정품이 「서버 접속 시도중...」에서 멈춘다(사람 확인 50). 정품이 받아 주는 거절 답은 `471` 하나뿐이다 —
//   정품 로비 모듈이 참가를 기다릴 때(`WOLAPI.dll` 상태 칸 1) 471 을 「채널 가득」(`0x8004006e`)으로 바꿔 게임에 넘기고,
//   게임 목록 화면(WOL 상태 8)은 그 값에만 「억세스 불가! — 서버가 다 찼습니다.」 창을 띄운다(`Game.exe` `0x41de91` → 창 `0x438790` → 하위 상태 1
//   `0x43ab10(1)` — 정적 분석. 창 뒤 화면 · 다시 누르기는 정품 화면 미확인).
//   474 · 475 · 478 은 그 화면에서 버려져 멈춤이 그대로다. 그래서 까닭이 무엇이든 숫자는 471 이고, 진짜 까닭은 로비 기록 · 셈 · 줄 끝 글에 남긴다
//   (정적 분석 근거 = `개인 메모` 「1 조사」)
//   ✕ DLL 없는 클라의 원래 `Host Game`(방 만들기 — 상태 칸 2)은 정품 게임 쪽이 실패 값을 전부 버려(`Game.exe` `0x40c1d0`) 받아 줄 답이 없다 — 그 갈래는 답을 안 한다
enum class JoinReject : uint8_t {
    RequestFailed,   // 새 방 · 곧바로 실패(등록된 게임 서버 없음 · 대기 맵 없음 …)
    CreateFailed,    // 새 방 · 물은 게임 서버가 실패로 답함(연결 끊김 · CREATE_FAILED · 만든 방의 ROOM 이 안 옴)
    Timeout,         // 새 방 · 한도(`kRoomRequestTimeoutMs`) 안에 답 없음
    NoSuchRoom,      // 없는 방(목록이 낡음 · 남의 이름 채널)
    NotLoggedIn,     // ★ 7단계 조각 6 — 로그인이 안 끝났거나 실패(토큰 · 새 방은 확인됨만)
    kCount
};
constexpr size_t kJoinRejectCount = static_cast<size_t>(JoinReject::kCount);
constexpr int kJoinRejectNumeric = 471;   // 정품이 받아 주는 유일한 거절 숫자(위 주석)
// ★ 10-31 — 랭크 줄에서 빠진 사람(취소 · 아니오 · 수락 한도)의 묶인 참가를 푸는 답. 랭크는 참가(`JOINGAME`)의 답을 묶어 둔다 —
//   정품 로비 모듈은 답을 기다리는 동안(`WOLAPI.dll` `[+0x62a0]` = 1) 새 참가를 안 보내고 `0x800401f7` 을 돌려준다(`0x210088da`).
//   그래서 빠진 뒤 다시 「랭크 매칭」을 누르면 로비에 참가가 안 와 줄에 못 섰다(10-31 사람 판). 474 는 그 기다림을 풀고(`0x2100999f` → `0x210099d9`)
//   게임엔 `0x80040072` 로 넘어가 게임 목록 화면에서 버려진다(위 주석 — 471 과 달리 창이 안 뜸). 정적 분석 · 정품 화면은 사람 확인
constexpr int kRankReleaseNumeric = 474;
// 거절 줄 끝 글(ASCII — 정품 화면엔 안 나오고 정품 기록 · DLL 기록에만 남는다)
const char* JoinRejectText(JoinReject why);

// ★ 10-14 작업 10a — 방 이름을 실을 때 쓰는 코드페이지(클라가 `SETCODEPAGE` 를 안 보냈을 때). 정품 클라는 949 를 보낸다(1단계 녹음 셋 모두)
constexpr int kDefaultCodepage = 949;

struct LobbyConfig {
    std::vector<std::string> motd{"Welcome to Nox Online!", ""};  // 첫 줄은 375, 나머지는 372
    // 게임 서버 IPv4 정수(a<<24|b<<16|c<<8|d) — 326·353·NOXTOKEN 에 같은 값.
    //   ★ 10-14 작업 8d — 0 이면 이 접속이 붙은 내 쪽 주소(채널이 곧 게임 서버). 옛 기본값 127.0.0.1(따로 뜬 로비 몫)
    uint32_t gameServerIp = 0;
    uint16_t gameServerPort = 18590;                // ★ 10-10 — NOXTOKEN 줄의 게임 서버 포트(DLL 이 이 주소로 가는 패킷에만 꼬리를 붙인다)
    GameInfo room;                                  // 방 정보 틀 — 방마다 이름 · 맵 · 모드 · 인원을 그 방 것으로 바꿔 싣는다(★ 8d)
    JoinKey joinKey{};                              // ★ 10-10 — 입장 토큰 서명 키(설정 파일 `[token] key` — 게임 서버와 같은 값)
    std::function<uint32_t()> unixNow = UnixNow;    // 토큰 만료의 기준 시각 — 시험이 정해 보려고
    std::function<uint64_t()> nonce;                // 일회용 번호 — 비면 BCrypt 난수
    std::function<uint32_t()> nowMs = NowMs;        // ★ 10-20 v2 작업 3a — 미룬 답 한도의 시계(ms · 32비트로 돈다) — 시험이 정해 보려고
    // ★ 로그인 암호 조각 4 — 로그인 토큰(`[auth] key` 가 있으면 켬 — 없으면 아이디만 · 정할 것 c)
    bool authOn = false;
    JoinKey loginKey{};                             // 로그인 서버와 같은 값 · 입장 토큰 키와 따로
    uint16_t authPort = kDefaultAuthPort;           // 가입 페이지 주소 안내(https://<이 접속이 붙은 내 쪽 주소>:<포트>/signup)
    // ★ 랭크 매칭 조각 4 — 줄 세우기(설정 `[rank] min · max · gather_sec` · 2 ≤ 최소 ≤ 최대 ≤ `kRankMaxLimit` 는 부르는 쪽이 본다)
    uint32_t rankMin = kRankMinDefault;
    uint32_t rankMax = kRankMaxDefault;
    uint32_t rankGatherMs = kRankGatherMsDefault;
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
    // ★ 로그인 암호 조각 4 — 로그인(USER 하나는 아래 갈래 중 하나로 끝난다 · 세는 자리 = USER 갈래(이름 · 키 없음) · `CheckLoginToken`)
    //   이름 틀림 · 아이디만(키 없음) · 토큰(검증 갈래별 — 통과 포함) · 다시 쓴 토큰 · 실패 표시(까닭별 · 모르는 까닭) · 손님 · 평문 · apgar 없음
    unsigned long long loginBadName = 0;     // 이름이 비었거나 `kMaxNickBytes` 넘음 — 실패 길
    unsigned long long loginNoAuth = 0;      // `[auth] key` 없는 로비 — 아이디만으로 곧바로 들여보냄(시험용 · 정할 것 c)
    unsigned long long loginToken[static_cast<size_t>(LoginTokenCheck::kCount)] = {};   // 토큰 검증 갈래(Ok = 들여보냄)
    unsigned long long loginReplay = 0;      // 서명은 맞는데 일회용 번호를 이미 씀 — 실패 길
    unsigned long long loginFail[static_cast<size_t>(LoginFail::kCount)] = {};          // DLL 이 실은 실패 까닭
    unsigned long long loginFailUnknown = 0; // `NXL!` 뒤 모르는 까닭
    unsigned long long loginGuest = 0;       // `apgar None` — 암호 판엔 손님 없음
    unsigned long long loginPlain = 0;       // 평문 암호(DLL 없음 · 판 낮음)
    unsigned long long loginMissing = 0;     // `apgar` 줄 없음
    unsigned long long loginKickedOld = 0;   // 같은 계정 새 접속이 옛 접속을 끊음(정할 것 b)
    unsigned long long chatViewOnly = 0;     // 로그인 안 된 접속의 대화방 입장 — 보기만(이름 안 알림 · ★ 10-28 실패는 거절하므로 USER 전 접속만)
    unsigned long long loginDeadlineClosed = 0;   // `kLoginDeadlineMs` 안에 USER 가 없어 끊음
    unsigned long long loginRejected = 0;    // ★ 10-28 암호 판 로그인 실패 — 378 로 거절(위 실패 갈래들의 합과 같아야 한다)
    unsigned long long loginRejectClosed = 0;   // ★ 10-28 378 뒤 `kLoginRejectCloseMs` 안에 클라가 안 끊어 끊음
    // ★ 레이팅 조각 4 — 점수 물음 하나는 아래 다섯 중 하나로 끝난다(시험은 합을 본다 — `RatingTotal`)
    unsigned long long ratingAsked = 0;      // 토큰 로그인이 저장소에 물음
    unsigned long long ratingShown = 0;      // 답 — 점수 · 순위 줄
    unsigned long long ratingNoGames = 0;    // 답 — 랭크 판 없음 줄
    unsigned long long ratingFailed = 0;     // 답 — 저장소가 못 읽음(줄 없이 로그인 끝)
    unsigned long long ratingTimeout = 0;    // `kRatingTimeoutMs` 안에 답 없음(줄 없이 로그인 끝)
    unsigned long long ratingClosed = 0;     // 기다리는 동안 클라가 끊음
    unsigned long long ratingLate = 0;       // 끝난 물음의 답 · 안 물은 답(버림 — 위 다섯에 안 듦)
    unsigned long long heldLines = 0;        // 기다리는 동안 쌓았다가 376 뒤 넘긴 줄
    unsigned long long heldDropped = 0;      // 쌓기 한도를 넘어 버린 줄
    unsigned long long RatingTotal() const { return ratingShown + ratingNoGames + ratingFailed + ratingTimeout + ratingClosed; }
    // ★ 랭크 매칭 조각 4 — 줄. 줄 선 사람 하나는 「빠짐(줄에서 · 까닭 셋)」 · 「묶임(방 요청에 실림)」 중 하나로 줄을 떠나고(되돌림은 다시 줄),
    //   묶인 사람 하나는 「참가 답」 · 「되돌림(요청 실패 · 시간 초과)」 · 「빠짐(요청 중)」 중 하나로 끝난다. 시험은 이 합을 본다
    unsigned long long rankQueued = 0;          // 줄 섬(NOXRANK JOIN 바로 뒤 내 방 참가)
    unsigned long long rankLeft[kRankLeaveCount] = {};          // 줄에서 빠짐(까닭별)
    unsigned long long rankLeftRequested[kRankLeaveCount] = {}; // 방 요청 중 빠짐(까닭별 — 방이 서도 참가 답을 안 보냄)
    unsigned long long rankLeaveStray = 0;      // NOXRANK LEAVE 인데 줄에도 요청에도 없음(잡힌 뒤 창이 닫힘 · 거절 뒤 — 버림)
    unsigned long long rankGrouped = 0;         // 묶여 방 요청에 실린 사람(되돌린 뒤 다시 묶이면 또 셈)
    unsigned long long rankGroupFull = 0;       // 묶은 까닭 — 최대가 참(곧바로)
    unsigned long long rankGroupGather = 0;     // 묶은 까닭 — 최소가 찬 뒤 모으기 시간 끝
    unsigned long long rankRequests = 0;        // 랭크 방 요청(묶음) = 섬 + 실패 + 시간 초과 + 기다리는 중
    unsigned long long rankRoomsCreated = 0;    // 방이 섬
    unsigned long long rankRoomFailed = 0;      // 곧바로 실패 · 게임 서버 실패
    unsigned long long rankRoomTimeout = 0;     // 한도(`kRoomRequestTimeoutMs`) 안에 답 없음
    unsigned long long rankRequeued = 0;        // 실패 · 시간 초과로 줄 앞에 되돌린 사람
    unsigned long long rankMatched = 0;         // 참가 답(토큰)을 보낸 사람
    unsigned long long rankMarkDropped = 0;     // NOXRANK JOIN 뒤 내 방 참가가 아닌 줄(표시 버림 — 그 줄은 원래대로)
    unsigned long long rankOldClient = 0;       // 판 4 밑 DLL 의 NOXRANK(무시)
    unsigned long long rankDupJoin = 0;         // 줄에 있거나 요청 중인 접속의 방 참가(무시 — 답이 둘이 되지 않게)
    unsigned long long rankRoomsHidden = 0;     // ★ 매칭 손보기 7 — 목록 줄(326)에서 뺀 랭크 방(LIST 한 번에 방마다 하나)
    // ★ 매칭 손보기 6 — 수락 단계. 찾음 한 사람(`rankFound`)은 「참가 답(다 수락)」 · 「되돌림(남이 거절 · 한도 — 나는 수락 또는 아직)」 ·
    //   「거절(나 — 까닭 셋)」 · 「한도 넘김(안 수락)」 중 하나로 끝난다. 참가 답은 `rankMatched` 에도 든다
    unsigned long long rankFoundGroups = 0;     // 수락 단계로 간 묶음
    unsigned long long rankFound = 0;           // 그 사람 수(FOUND 보냄)
    unsigned long long rankAccepted = 0;        // ACCEPT 받음(찾음 중)
    unsigned long long rankAcceptMatched = 0;   // 다 수락 → 참가 답
    unsigned long long rankFoundRequeued = 0;   // 남이 거절 · 한도 → 줄 앞에 되돌림(WAIT)
    unsigned long long rankDeclined[kRankLeaveCount] = {};   // 찾음 · 수락 뒤 빠짐 = 거절(까닭별)
    unsigned long long rankExpired = 0;         // 한도까지 수락 안 함 → 줄에서 뺀(EXPIRED)
    unsigned long long rankAcceptStray = 0;     // 찾음 중이 아닌 접속의 ACCEPT(버림)
    unsigned long long rankJoinReleased = 0;    // ★ 10-31 — 빠진 사람(창 닫힘 · LIST · 한도 — 끊김은 빼고)의 묶인 참가에 474
                                                //   = 빠짐(줄 · 요청 중) 창 닫힘 · LIST + 거절 창 닫힘 · LIST + 한도
    unsigned long long RankDeclinedTotal() const {
        unsigned long long n = 0;
        for (const unsigned long long v : rankDeclined) n += v;
        return n;
    }
    unsigned long long RankLeftTotal() const {
        unsigned long long n = 0;
        for (size_t i = 0; i < kRankLeaveCount; ++i) n += rankLeft[i] + rankLeftRequested[i];
        return n;
    }
    unsigned long long LoginTotal() const {   // USER 로 끝난 로그인 수(위 갈래 합 — 끊음 · 보기만 빼고)
        unsigned long long n = loginBadName + loginNoAuth + loginReplay + loginFailUnknown + loginGuest + loginPlain + loginMissing;
        for (const unsigned long long v : loginToken) n += v;
        for (const unsigned long long v : loginFail) n += v;
        return n;
    }
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
    //   ★ 7단계 조각 6 — 계정 확인 한도 · 로그인 마감도
    void Tick(std::vector<Out>& out);
    size_t PendingRooms() const { return pending_.size(); }
    // ★ 로그인 암호 조각 4 — 로그인된 계정 수(계정 → 접속 장부 · 끊길 때 지움)
    size_t AccountsOnline() const { return accountConn_.size(); }
    // ★ 레이팅 조각 4 — 점수를 물을 저장소(없으면 묻지 않고 곧바로 인사 — 시험 · 녹음 대조). 로비보다 오래 살아야 한다
    void AttachStore(ILobbyStore* store) { store_ = store; }
    // 저장소 답 Rating(소켓 루프가 꺼내 넘긴다) — 끝난 물음이면 버리고 센다
    void OnRatingAnswer(const StoreAnswer& a, std::vector<Out>& out);
    size_t PendingRatings() const { return ratingPending_.size(); }
    // ★ 매칭 손보기 5 — 판 저장이 고친 점수(거울이 SAVED ok 를 지을 때 넘긴다) → 로비가 쥔 점수(랭크 방 요청의 명단 점수)
    void OnRatingsChanged(const std::vector<LinkRatingChange>& changes);
    // 로비가 쥔 그 이름의 점수 — 로그인 점수 답 · 판 저장 답에서. 모르면(점수를 안 물은 로그인 · 읽기 실패) 시작 점수
    int32_t RatingOf(const std::string& account) const;
    // ★ 랭크 매칭 조각 4 — 줄 선 사람 수 · 답을 기다리는 랭크 방 요청 수(그 요청에 묶인 사람은 줄 수에 안 듦)
    size_t RankQueueSize() const { return rankQueue_.size(); }
    size_t RankRequestsPending() const { return rankPending_.size(); }
    size_t RankFoundPending() const { return rankFound_.size(); }   // ★ 매칭 손보기 6 — 수락을 기다리는 묶음

    // 소켓 루프가 찍을 기록(처음 본 명령 등). 꺼내면 비운다.
    std::vector<std::string> TakeLog();

    static std::string Channel(const LobbyRoom& r) { return "#" + r.host + "'s_game"; }
    const LobbyStats& Stats() const { return stats_; }

private:
    // ★ 7단계 조각 6 — 로그인 단계(대기 = USER 전 · 확인됨 · 실패 = 부드러운 실패로 들여보냄)
    //   ★ 로그인 암호 조각 4 — 「확인 중」(저장소 답 기다림) 걷음
    //   ★ 10-28 — 거절 = 암호 판 실패(378 보냄 · 뒷줄 안 받음 · 한도 뒤 끊음). 실패는 이제 아이디만 판의 이름 틀림만
    //   ★ 레이팅 조각 4 — 점수 기다림(토큰은 통과 · 저장소 답까지 인사를 미룸 · 뒷줄 쌓음 · 한도 `kRatingTimeoutMs`)
    enum class LoginState : uint8_t { Waiting, Rating, Ok, Failed, Rejected };
    struct Conn {
        uint32_t peerIp = 0;
        uint32_t localIp = 0;
        std::string nick;
        int codepage = 0;
        int clientVer = -1;  // ★ 10-10 — `NOXCLIENT <판>`(줄이 없으면 -1 — 「판 낮음」과 따로 센다)
        LoginState login = LoginState::Waiting;
        uint32_t loginDueMs = 0;        // 대기 — 로그인 마감(붙은 때 + kLoginDeadlineMs) · ★ 10-28 거절 — 끊는 때(378 + kLoginRejectCloseMs)
                                        //   · ★ 레이팅 조각 4 점수 기다림 — 한도(물은 때 + kRatingTimeoutMs)
        uint32_t ratingReq = 0;         // ★ 레이팅 조각 4 — 점수 기다림의 저장소 요청 번호
        std::vector<std::string> held;  // ★ 레이팅 조각 4 — 점수 기다림 중에 온 뒷줄(376 뒤 차례대로)
        bool closing = false;           // 끊는 줄을 보냄(마감 · 옛 접속 · ★ 10-28 거절 뒤 — 끊김 사건까지 다시 안 함 · 뒷줄 안 받음)
        uint64_t accountId = 0;         // 확인됨 — 계정 번호(토큰 · 아이디만이면 0)
        std::string apgar;              // ★ 조각 4 — 로그인 묶음 `apgar <인자>` 의 인자(USER 까지 쥠 · 기록에 안 찍음)
        // ★ 랭크 매칭 조각 4 — `NOXRANK JOIN` 을 받음(바로 다음 줄이 내 방 참가여야 줄 서기) · 줄 상태 · 묶인 요청 번호
        bool rankNext = false;
        //   ★ 매칭 손보기 6 — 찾음(방이 섰고 수락을 기다림) · 수락함(남을 기다림)
        enum class Rank : uint8_t { None, Queued, Requested, Found, Accepted } rank = Rank::None;
        uint32_t rankReq = 0;
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
    // ★ 7단계 조각 6 — 인사(375 · 372 … · 376). `failNotice` 가 있으면 실패 길(안내 줄 덧붙임)
    //   ★ 레이팅 조각 4 — `ratingLine`(ASCII — 한글은 정품 공지 줄에서 깨짐)이 있으면 376 앞 372 한 줄 · 끝나면 쌓아 둔 뒷줄을 차례대로
    void FinishLogin(int conn, Conn& c, LoginState result, const std::string& failNotice, std::vector<Out>& out,
                     const std::string& ratingLine = std::string());
    // ★ 레이팅 조각 4 — 토큰 로그인 통과 뒤 — 저장소가 있으면 점수를 묻고 기다림 · 없으면 곧바로 인사
    void BeginOkLogin(int conn, Conn& c, std::vector<Out>& out);
    // ★ 로그인 암호 조각 4 — USER 에서 `apgar` 를 가르고 토큰을 본다 → 갈래 하나를 세고 FinishLogin
    void CheckLoginToken(int conn, Conn& c, std::vector<Out>& out);
    // 같은 계정의 옛 접속을 끊는다(정할 것 b) · 새 접속을 장부에
    void TakeAccount(int conn, Conn& c, uint64_t accountId, std::vector<Out>& out);
    std::string SignupUrl(const Conn& c) const;   // https://<붙은 내 쪽 주소>:<authPort>/signup
    void Unknown(const std::string& cmd, std::string_view line);
    // ★ 랭크 매칭 조각 4 — 줄 서기 · 빠지기(줄 · 요청 중 · 둘 다 아니면 버림) · 모으기(최소 · 최대 · 모으기 시간 · 되돌린 뒤 쉼) ·
    //   묶어 방 요청 · 방이 서면 묶인 사람마다 제 채널(`#<닉>'s_game`)로 참가 답 · 실패 · 시간 초과면 줄 앞에 되돌림
    void RankEnqueue(int conn, Conn& c, std::vector<Out>& out);
    void RankRemove(int conn, RankLeave why, std::vector<Out>& out);
    void RankPump(std::vector<Out>& out);
    void RankRequest(size_t count, bool full, std::vector<Out>& out);
    void RankReply(uint32_t req, const std::vector<int>& group, const LobbyRoom& room, std::vector<Out>& out);
    void RankRequeue(uint32_t req, const std::vector<int>& group);
    std::string RankNames(const std::vector<int>& conns) const;
    // ★ 매칭 손보기 6 — 수락 단계: 찾음 보내기 · 수락 받기 · 묶음 풀기(남은 사람을 줄 맨 앞에 — 쉬지 않음 · `except` 는 빼고) · 참가 답
    void RankAccept(int conn, Conn& c, std::vector<Out>& out);
    void RankFoundBreak(uint32_t req, int except, std::vector<Out>& out);
    void RankJoinReply(int id, Conn& c, const LobbyRoom& room, std::vector<Out>& out);
    void RankReleaseJoin(int conn, const Conn& c, std::vector<Out>& out);   // ★ 10-31 — 묶인 참가에 474(`kRankReleaseNumeric`)

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
    CJoinTokenLedger loginNonces_;                // ★ 로그인 암호 조각 4 — 로그인 토큰 일회용 번호(입장 토큰 장부와 따로 — 게임 서버가 씀)
    std::map<uint64_t, int> accountConn_;         // ★ 조각 4 — 계정 → 로그인된 접속(정할 것 b)
    ILobbyStore* store_ = nullptr;                // ★ 레이팅 조각 4 — 점수를 물을 저장소(없으면 안 물음)
    std::map<uint32_t, int> ratingPending_;       // 점수 요청 번호 → 기다리는 접속
    std::map<std::string, int32_t> ratingOf_;     // ★ 매칭 손보기 5 — 계정 이름 → 점수(`RatingOf`)
    uint32_t nextRatingReq_ = 1;
    // ★ 랭크 매칭 조각 4 — 줄(선 차례) · 모으기 시계 · 되돌린 뒤 쉬는 시계 · 답을 기다리는 랭크 방 요청(번호 → 묶인 접속 · 한도)
    std::vector<int> rankQueue_;
    bool rankGathering_ = false;
    uint32_t rankGatherDueMs_ = 0;
    bool rankResting_ = false;
    uint32_t rankRestDueMs_ = 0;
    struct RankGroup {
        std::vector<int> conns;
        uint32_t deadlineMs = 0;
    };
    std::map<uint32_t, RankGroup> rankPending_;
    // ★ 매칭 손보기 6 — 수락을 기다리는 묶음(방 요청 번호 → 사람 · 선 방 · 한도)
    struct RankFoundGroup {
        std::vector<int> conns;
        LobbyRoom room;
        uint32_t deadlineMs = 0;
    };
    std::map<uint32_t, RankFoundGroup> rankFound_;
    std::map<int, Conn> conns_;
    std::map<std::string, std::set<int>> members_;  // 채널 → 들어와 있는 접속
    std::set<std::string> unknownSeen_;
    std::vector<std::string> log_;
};

std::string IpDotted(uint32_t ip);

}  // namespace nox::wol
