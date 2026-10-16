// RoomManager.h — 방 여럿을 게임 스레드 하나에서 돌린다(`Docs/ROOMS_DESIGN.md` — MO 3단계 플랜 B).
//
// ★ 10-09 플랜 B 작업 6a — 틀. 방 관리자는 통신 층 이벤트와 게임(`nox::Server`) 사이에 선다.
//     · 이벤트 → 세션표의 방. 접속 시도(`JOIN_REQUESTED`)는 방으로 넘기지 않고 여기서 답한다
//     · 방마다 쌓인 통신 요청을 처리한 차례대로 모아 한 묶음으로 내준다(`SwapOutNetworkRequests`)
//     · 틱 — 방마다 시계를 따로 돌린다(`BeginTick` → `RunTick`). 게임 루프가 기다릴 다음 틱 시각은 방들 가운데 가장 이른 것
// ★ 10-09 플랜 B 작업 6b — 배정 · 거절 · 나감 · 방 이벤트 · 방 만들기/닫기.
//     · 나감 — 방이 내는 끊기 요청(`DISCONNECT_SESSION`)을 넘길 때 세션표에서 지운다(나가는 길이 모두 그 요청으로 모인다)
// ★ 10-10 토큰 작업 2 — 6b 의 「명단(이름 → 방) · 입장(`CLIENT_ACCEPT`)까지 `CONNECTED` 담아 두기」를 입장 토큰으로 바꿨다
//   (`Docs/TOKEN_DESIGN.md`). 로비는 클라의 캐릭터 이름을 모르므로 이름 열쇠는 로비와 이어지지 않는다.
//     · 토큰 방식(`SetJoinKey`) — 접속 시도는 꼬리의 토큰을 확인만 하고(서명·만료·보낸 IP·방) 받거나 `SERVER_ERROR` 사유 4,
//       `CONNECTED` 는 같은 토큰을 써 버리고(일회용) 그 방에 바로 넘긴다. 틀리면 끊는다 — 통신 층은 CONNECT 에 이미
//       접속 수락을 보냈다(`UdpServer.cpp` SERVER_CONNECT). 클라는 접속 시도와 CONNECT 를 다른 포트에서 보내므로 둘 다에 꼬리가 있다
//     · 열린 방식(`SetOpenEntry` — 토큰 검사 끄기) — 모두 방 0. 녹음 재생·대조와 시험 도구만 켠다(`CSyncHost` 가 켠다)
//       (★ 10-14 작업 9d — 「방 0」 대신 정해 준 방 하나. 그 방이 없으면 받지 않는다)
// ★ 10-14 플랜 B 작업 9d — 방 만들기 입구는 하나(`CreateRoom(방 설정)`). 읽기 전용 데이터(`SetWorldData`)를 받았으면 세계까지 차린다 —
//   받지 않은 방 관리자(시험 · 한 스레드 조립)는 빈 방(부르는 쪽이 게임을 직접 차린다). 특별한 방 번호가 없다(방 0 자동 생성 ·
//   「방 0 은 못 닫음」 없앰 — `Docs/ROOMS_DESIGN.md` 원칙)
//     · 둘 다 안 정했으면 모두 거절한다(설정 하나 빠뜨린 서버가 아무나 받지 않게)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nox/JoinToken.h"
#include "nox/Messages.h"   // ★ 10-16 작업 12a — 거절 사유(`ConnectError`)
#include "nox/Server.h"
#include "nox/UdpServer.h"
#include "nox/WorldBuild.h"   // ★ 10-14 작업 9c — 방 설정(`RoomSettings`) · 세계 차리기 결과(`WorldExtras`)

namespace nox {

// ★ 10-16 작업 12a — 방의 판 상태(`Docs/ROOMS_DESIGN.md` 결정 1 — 대기 = 채팅 모드 맵 · 판 = 대전 맵). 늘어나면 `switch` 로 갈래를 셀 것
enum class RoomPhase : uint8_t { Waiting, Playing };
const char* RoomPhaseName(RoomPhase p);

// ★ 10-16 작업 12a — 방 구성원 한 사람(들어온 차례로 든다). 방장 = 맨 앞(결정 2 「먼저 들어온 사람」 · 「10-13 정한 것」 방장 넘김)
struct RoomMember {
    int64_t sessionId = -1;
    std::string account;   // 입장 토큰의 로비 이름(열린 방식은 빈 것)
};

// 방 하나가 틱을 도는 데 쓴 시간(부하 — `CRoom::Load`). 방 워커를 둘지 가르는 근거(`ROOMS_DESIGN.md` 「대규모 확장 경로」)
struct CRoomLoad {
    unsigned long long tickRuns = 0;   // `RunTick` 부른 수
    double tickMsSum = 0.0;
    double tickMsMax = 0.0;
};

// 방 하나 — 원본 재현 게임(`nox::Server`) 하나를 든다. MO 규칙(대기/진행/끝 · 판 시작 · 결과)은 여기에 쌓인다(작업 7 · 5단계).
// ★ 10-14 플랜 B 작업 9c — 방 설정 · 세계 차리기 결과 · 판 차리기 · 진단 상태도 방이 든다(옛 `main.cpp` 의 `RoomRun` — 방 번호를 열쇠로
//   `main` 이 따로 들어 방 하나의 정본이 둘이었다). 판 차리기 · 틱 뒤 진단 몸통은 `RoomTick.cpp`
class CRoom {
public:
    explicit CRoom(int id) : _id(id) { _game.SetLogRoom(id); }   // ★ 10-12 작업 7b — 방에 든 게임의 줄 머리에 `[방 N]`
    CRoom(const CRoom&) = delete;
    CRoom& operator=(const CRoom&) = delete;

    int Id() const { return _id; }
    Server& Game() { return _game; }
    const Server& Game() const { return _game; }
    const CRoomLoad& Load() const { return _load; }
    void ResetLoad() { _load = CRoomLoad{}; }
    // ★ 10-12 플랜 B 작업 7c — 이 방이 낸 보내기 요청(`SEND_MSG`) 누계. 초당 진단이 「이 방이 이번 틱에 보낸 것이 있나」를 본다
    unsigned long long SendRequests() const { return _sendRequests; }

    // ── ★ 10-14 작업 9c — 방 설정 · 세계 차리기 결과 ──
    const RoomSettings& Settings() const { return _settings; }
    void SetSettings(const RoomSettings& s) {
        _settings = s;
        _curMapKey = s.mapPath.empty() ? std::string() : MapKeyOf(s.mapPath);   // ★ 10-16 작업 13b — 지금 맵 · 모드 = 방을 만든 맵 · 모드
        _curMode = s.mode;
    }
    // ★ 10-16 작업 13b — 판을 열 때 붙을 맵 원본(서버가 뜰 때 읽은 맵 목록 — 방 관리자가 만들 때 준다 · 시험은 직접)
    void SetMapCatalog(const MapCatalog* maps) { _maps = maps; }
    const std::string& CurrentMapKey() const { return _curMapKey; }   // 지금 맵 열쇠(판 중이면 대전 맵) — 로비 목록이 그린다
    const std::string& CurrentMode() const { return _curMode; }
    WorldExtras& Extras() { return _extras; }               // 월드 차리기가 채운다
    const WorldExtras& Extras() const { return _extras; }
    void SetBotSession(int64_t sid) { _bot0Sid = sid; }     // `[성장]` 줄의 봇 세션 — 통신 층 값이라 스레드를 띄우기 전에 읽어 둔다

    // ── ★ 10-14 작업 9c — 판 차리기 · 틱 뒤 진단(`RoomTick.cpp` — 옛 `main` 의 `StartModeIfReady` · `RoomTickDiagnostics`) ──
    void StartModeIfReady();
    // 틱 앞에 보낸 수를 적어 두고(`MarkSendSeen`) 틱 뒤 차이(`SendsSinceMark`)를 진단의 `made` 로 넘긴다
    void MarkSendSeen() { _sendSeen = _sendRequests; }
    size_t SendsSinceMark() const { return static_cast<size_t>(_sendRequests - _sendSeen); }
    void TickDiagnostics(size_t made);

    // ── ★ 10-16 작업 12a — 판 상태 · 구성원 · 방장(구성원은 방 관리자가 입장 · 나감 때 고친다) ──
    RoomPhase Phase() const { return _phase; }
    void SetPhase(RoomPhase p) { _phase = p; }
    const std::vector<RoomMember>& Members() const { return _members; }
    const RoomMember* Host() const { return _members.empty() ? nullptr : &_members.front(); }

    // ── ★ 10-16 작업 12b — 채팅 명령(`RoomTick.cpp`). 방 관리자가 게임에 이벤트를 넘긴 뒤 부른다 — 게임이 쌓은 명령(`Server::SwapOutChatCommands`)을 처리 ──
    //   `!start` = 방장 · 대기 · 게임 안 사람 `kMinPlayers` 이상이면 판 — 대전 맵으로 바꾼다(★ 13b · 대전 맵 · 대기 맵 원본이 없으면 `NoMap`).
    //   결과는 갈래마다 센다
    enum class CmdOutcome : uint8_t { Started, NotHost, NotWaiting, TooFew, NoMap, Unknown, kCount };
    static const char* CmdOutcomeName(CmdOutcome o);
    static constexpr size_t kMinPlayers = 2;   // 판을 돌릴 최소 인원(`Docs/ROOMS_DESIGN.md` 「10-13 정한 것」)
    void HandleChatCommands();
    unsigned long long CmdCount(CmdOutcome o) const { return _cmd[static_cast<size_t>(o)]; }

private:
    friend class CRoomManager;
    // ★ 10-16 작업 13b — 판 열기(대전 맵으로 · 대기 맵을 다음 판 맵으로 걸어 둠) · 판 끝 뒤 다시 열림(방 관리자가 판 끝 참 → 거짓을 보고 부른다)
    std::shared_ptr<const MapOriginal> FindMap(const std::string& path) const;
    bool CanStartMatch() const { return FindMap(_settings.battleMapPath) && FindMap(_settings.mapPath); }
    void StartMatch();
    void OnRoundRestarted();
    int _id;
    Server _game;
    int _ticks = 0;            // 이번 `BeginTick` 이 정한 돌 틱 수(`RunTick` 이 쓴다)
    bool _wasRoundOver = false;   // 판 끝 이벤트 — 거짓 → 참으로 바뀔 때 한 번
    CRoomLoad _load;
    unsigned long long _sendRequests = 0;   // ★ 10-12 작업 7c — 방 관리자가 요청을 옮길 때(`PullRequests`) 센다
    // ★ 10-14 작업 9c — 옛 `RoomRun` 의 칸(주석 그대로)
    RoomSettings _settings;                    // 이 방의 설정(실행 인자로 차린 방 = 실행 인자의 방 설정)
    WorldExtras _extras;                       // 월드 차리기가 남긴 것(맵 깃발 자리 · 서버 찾기 답의 맵 이름)
    int64_t _bot0Sid = -1;                     // `[성장]` 줄의 봇 세션 — 통신 층 값이라 스레드를 띄우기 전에 읽어 둔다
    bool _modeStarted = false;
    unsigned long long _tracePetsInWall = 0;   // ③ 크리처가 **벽 칸 안**에 있던 표본 수
    unsigned long long _tracePetsSamples = 0;  // 그 분모 — 0 옆에 늘 지나간 양을 같이 읽는다
    unsigned long long _lastAbilFp = 0;        // 조용한 주행의 능력 갈래 지문(옛 루프 안 `static`)
    unsigned long long _sendSeen = 0;          // 틱 앞에 본 이 방의 보내기 요청 누계 — 이 틱의 `made` = 틱 뒤 − 이것
    // ★ 10-14 작업 9e — 빈 방 닫기(방 관리자가 쓴다): 빈 채가 된 때
    bool _emptyArmed = false;
    uint32_t _emptySinceMs = 0;
    // ★ 10-16 작업 12a
    RoomPhase _phase = RoomPhase::Waiting;
    std::vector<RoomMember> _members;          // 들어온 차례 — 세션표(`CRoomManager::_seats`)의 이 방 세션과 같은 사람들
    // ★ 10-16 작업 12b
    unsigned long long _cmd[static_cast<size_t>(CmdOutcome::kCount)] = {};
    std::vector<Server::ChatCommand> _cmdBuf;   // 다시 쓰는 그릇
    // ★ 10-16 작업 13b
    const MapCatalog* _maps = nullptr;
    std::string _curMapKey;
    std::string _curMode;
};

// 방이 바깥에 알리는 것(4단계 인원 갱신 · 5단계 결과 기록이 쓴다). 나감은 세션표 변화로 본다(위 머리 주석).
struct RoomEvent {
    enum class Type { ROUND_OVER };
    Type type = Type::ROUND_OVER;
    int roomId = 0;
};

// 방 관리자의 셈 — 갈래마다 하나씩. 토큰 확인 갈래는 접속 시도와 CONNECT 를 합쳐 센다(어느 쪽인지는 받음·거절·끊음 셈으로).
struct CRoomStats {
    unsigned long long joinAccepted = 0;       // 접속 시도를 받음
    unsigned long long joinRejected = 0;       // 접속 시도를 거절(까닭은 아래 토큰 셈)
    unsigned long long assigned = 0;           // 방에 넣음(열린 방식 `CONNECTED` · 토큰 `CONNECTED`)
    unsigned long long connectRejected = 0;    // `CONNECTED` 를 끊음(까닭은 아래 토큰 셈)
    unsigned long long tokenNoKey = 0;         // 키도 열린 방식도 안 정함 — 설정 빠짐
    unsigned long long tokenMissing = 0;       // 꼬리(토큰)가 없음
    unsigned long long tokenBadAddr = 0;       // 보낸 주소를 IPv4 로 못 읽음
    unsigned long long tokenCheck[static_cast<int>(JoinTokenCheck::kCount)] = {};   // 검증 결과별(통과 포함 — `VerifyJoinToken`)
    unsigned long long tokenNoRoom = 0;        // 토큰의 방이 없음(닫힘)
    unsigned long long tokenReused = 0;        // 일회용 번호를 이미 씀(`CONNECTED`)
    unsigned long long droppedUnknown = 0;     // 토큰 방식에서 세션표에 없는 세션의 다른 이벤트를 버림
    unsigned long long left = 0;               // 방이 끊은 세션(나감 · 내보냄 · 시간 초과 — 끊기 요청)
    unsigned long long roomsCreated = 0;       // ★ 10-14 작업 9d — 만든 방(세계 차리기까지 된 것)
    unsigned long long roomBuildFailed = 0;    // ★ 10-14 작업 9d — 세계 차리기가 실패해 안 만든 방
    unsigned long long openNoRoom = 0;         // ★ 10-14 작업 9d — 열린 방식인데 정해 준 방이 없어 거절 · 끊음
    unsigned long long roomsClosed = 0;
    unsigned long long emptyRoomsClosed = 0;   // ★ 10-14 작업 9e — 그 가운데 빈 방이라 닫은 것
    unsigned long long closedRoomSessions = 0; // 방을 닫으며 내보낸 세션
    unsigned long long lobbyEventsMisrouted = 0;   // ★ 10-14 작업 8c — 로비 TCP 사건이 여기로 옴(게임 스레드의 나누는 자리가 틀림 — 0 이 정상)
    // ★ 10-16 작업 12a — 방 규칙으로 거절(토큰은 맞음 — 접속 시도와 CONNECT 를 합쳐 셈) · 방장 바뀜
    unsigned long long rejectPlaying = 0;      // 판 중(결정 3) — `ConnectError::Closed`
    unsigned long long rejectFull = 0;         // 방 최대 인원(`RoomSettings::maxPlayers`) — `ConnectError::Full`
    unsigned long long hostChanges = 0;        // 방장이 생기거나 바뀜(첫 사람 포함 · 마지막 사람이 나가 비는 것은 안 셈)
};

// ★ 10-14 플랜 B 작업 9e — 돌고 있는 방 하나의 사실. 로비가 목록을 그린다 — 목록의 정본은 방 관리자의 이것 하나
//   (설정 파일에 방을 적지 않는다 — `Docs/ROOMS_DESIGN.md` 원칙 「방 목록의 정본은 돌고 있는 방」)
struct RoomFact {
    int roomId = -1;
    std::string name;      // 방 설정의 이름(`RoomSettings::roomName`)
    std::string creator;   // ★ 10-14 작업 8d — 방을 만든 사람의 로비 이름(`RoomSettings::creator` — 비면 로비 목록에 안 나옴)
    std::string mapKey;    // 맵 열쇠(`MapKeyOf` — 녹음 모드는 빈 것)
    std::string mode;      // 방 설정의 모드(`arena` · `chat` …)
    size_t players = 0;    // 게임 안 사람 수(`Server::inGameCount`)
    size_t seats = 0;      // 세션표에서 이 방 세션 수(들어오는 중 포함)
    int maxPlayers = 0;    // ★ 10-16 작업 12a — 방 설정의 최대 인원(0 = 제한 없음)
    RoomPhase phase = RoomPhase::Waiting;   // ★ 10-16 작업 12a
};

class CRoomManager {
public:
    static constexpr int kNoRoom = -1;
    // ★ 10-14 작업 9e — 빈 방 닫기까지(`RoomSettings::closeWhenEmpty`). 입장 토큰 수명 — 방을 만든 사람이 토큰을 받고 들어올 동안은 둔다
    static constexpr uint32_t kEmptyRoomCloseMs = kJoinTokenLifetimeSec * 1000;

    CRoomManager() = default;   // ★ 10-14 작업 9d — 방 없이 시작한다(옛 「방 0 을 만든다」)
    CRoomManager(const CRoomManager&) = delete;
    CRoomManager& operator=(const CRoomManager&) = delete;

    CRoom& Room(int id) { return *_rooms.at(id); }
    const CRoom& Room(int id) const { return *_rooms.at(id); }
    bool HasRoom(int id) const { return _rooms.count(id) != 0; }
    Server& Game(int roomId) { return Room(roomId).Game(); }
    const Server& Game(int roomId) const { return Room(roomId).Game(); }
    size_t RoomCount() const { return _rooms.size(); }
    // ★ 10-12 플랜 B 작업 7c — 방을 번호 차례로 돈다(판 차리기 · 진단 · `[처음]` 모으기). 도는 동안 방을 만들거나 닫지 않는다
    template <class F> void ForEachRoom(F&& f) { for (auto& kv : _rooms) f(*kv.second); }
    template <class F> void ForEachRoom(F&& f) const { for (const auto& kv : _rooms) f(static_cast<const CRoom&>(*kv.second)); }

    // ── 방 만들기 · 닫기(실행 중에도) ──
    // ★ 10-14 작업 9d — 읽기 전용 데이터(서버가 뜰 때 한 번 · 부르는 쪽이 든다). 받으면 `CreateRoom` 이 세계까지 차린다
    void SetWorldData(const WorldData& d) { _world = d; _hasWorld = true; }
    // 방 하나를 만든다 — 방 설정을 들고 세계까지(`BuildRoom`). 돌려주는 값 = 새 방 번호 · 실패면 `kNoRoom`(`buildRc` = 세계 차리기의 값)
    int CreateRoom(const RoomSettings& settings, int* buildRc = nullptr);
    // 그 방의 사람마다 `SERVER_CLOSE` 를 보내고 끊은 뒤 방을 지운다. 없는 방이면 false.
    bool CloseRoom(int id);
    // ★ 10-14 작업 9e — 돌고 있는 방의 사실(번호 차례) · 방을 만들고 닫을 때 한 줄씩 찍는다(`main` 이 켠다 — 시험은 조용히)
    std::vector<RoomFact> ListRooms() const;
    void SetLogRooms(bool on) { _logRooms = on; }

    // ── 입장 방식 ── (머리 주석 「10-10 토큰 작업 2」)
    void SetJoinKey(const JoinKey& key);   // 토큰 방식 — 실제 서버
    void SetOpenEntry(int roomId);         // 토큰 검사 끄기 — 모두 그 방. 녹음 재생·대조와 시험 도구 · LAN 실행만
    bool OpenEntry() const { return _openEntry; }
    void SetUnixClock(std::function<uint32_t()> now) { _unixNow = std::move(now); }   // 시험이 만료를 정해 보려고
    void SetLogJoins(bool on) { _logJoins = on; }   // 입장 판정을 한 줄씩 찍는다(`main` 이 켠다 — 시험은 조용히)

    // ── 세션표 ──
    int RoomOfSession(int64_t sessionId) const;   // 모르는 세션이면 `kNoRoom`
    size_t SeatCount() const { return _seats.size(); }
    // 소켓 없이 방에 바로 세운 사람(봇 · 시험 — `CSyncHost::DebugEnterGame`)을 세션표에 적는다
    void AssignDirect(int64_t sessionId, int roomId);

    // 통신 층 이벤트 하나. 돌려주는 값 = 방의 게임이 읽은 게임 메시지 수(`Server::OnNetworkEvent`).
    size_t OnNetworkEvent(const NetworkEvent& ev);
    // 방마다 게임 시계를 돌려 돌 틱 수를 정해 둔다. 돌려주는 값 = 방들 가운데 가장 많은 틱 수(0 이면 이번에 도는 방이 없다).
    //   ★ 10-14 작업 9e — 그 앞에 빈 방 닫기(`closeWhenEmpty` 방이 빈 채 `kEmptyRoomCloseMs` 지남)
    int BeginTick(uint32_t nowMs);
    // `BeginTick` 이 정한 틱 수대로 방마다 게임 일을 한다(부하를 재고 판 끝을 알린다).
    void RunTick();
    // 다음 틱 시각 — 방들 가운데 가장 이른 것(`Server::NextTickDue`). 시계를 돌린 방이 없으면 false.
    bool NextTickDue(uint32_t& due) const;
    // 쌓인 통신 요청을 통째로 꺼낸다 — 방 관리자가 낸 것(접속 답 · 끊기)과 방마다 낸 것이 처리한 차례대로 들어 있다.
    //   이벤트 · 틱 밖에서 방에 바로 쌓인 것(시험 · 진단이 게임을 바로 부른 것)은 여기서 방 차례로 모은다.
    void SwapOutNetworkRequests(std::vector<NetworkRequest>& into);
    void SwapOutRoomEvents(std::vector<RoomEvent>& into) { into.swap(_roomEvents); _roomEvents.clear(); }
    const CRoomStats& Stats() const { return _stats; }

private:
    void OnJoinRequested(const NetworkEvent& ev);
    size_t Forward(int roomId, const NetworkEvent& ev);
    // 토큰으로 방을 정한다 — 접속 시도(확인만)와 `CONNECTED`(`consume` — 일회용 번호를 씀)가 같이 쓴다.
    //   못 받으면 `kNoRoom` · `why` = 까닭(셈은 안에서) · `err` = 접속 시도 거절에 실을 사유(★ 10-16 작업 12a — 방 규칙이면 그 사유)
    int AdmitByToken(const NetworkEvent& ev, bool consume, JoinToken& tok, const char*& why, ConnectError& err);
    // ★ 10-16 작업 12a — 방 규칙(판 중 · 최대 인원)으로 받을 수 있나. 못 받으면 false · `why` · `err`(셈은 안에서).
    //   토큰 방식과 열린 방식이 같이 쓴다 — 방 안 세션은 아직 세션표에 없는 이 사람을 안 셈
    bool RoomAdmits(int roomId, const char*& why, ConnectError& err);
    size_t SeatsIn(int roomId) const;
    // ★ 10-16 작업 12a — 구성원 넣기 · 빼기(방장이 바뀌면 셈 · 한 줄)
    void AddMember(CRoom& room, int64_t sessionId, const std::string& account);
    void RemoveMember(CRoom& room, int64_t sessionId);
    void LogHost(const CRoom& room, const char* why) const;
    void LogJoin(const char* what, const NetworkEvent& ev, int roomId, const JoinToken& tok, const char* why) const;
    void RespondJoin(int64_t sessionId, bool accept, const std::vector<uint8_t>& rejectMsg);
    void RequestSendMsg(int64_t sessionId, bool reliable, const uint8_t* msg, size_t n);
    void RequestDisconnectSession(int64_t sessionId);
    void PullRequests(CRoom& room);   // 방의 요청 목록을 방 관리자 목록 뒤로 옮긴다(끊기 요청이면 세션표에서 지운다)
    void CloseEmptyRooms(uint32_t nowMs);   // ★ 10-14 작업 9e
    std::map<int, size_t> SeatCounts() const;   // 방 → 세션표의 세션 수(한 번 훑기)

    std::map<int, std::unique_ptr<CRoom>> _rooms;
    int _nextRoomId = 0;
    bool _openEntry = false;
    int _openRoom = kNoRoom;                      // ★ 10-14 작업 9d — 열린 방식의 방
    WorldData _world;                             // ★ 10-14 작업 9d — 방을 차릴 때 쓰는 읽기 전용 데이터
    bool _hasWorld = false;
    bool _logRooms = false;                       // ★ 10-14 작업 9e
    bool _hasKey = false;
    JoinKey _key{};
    CJoinTokenLedger _ledger;
    std::function<uint32_t()> _unixNow = UnixNow;
    bool _logJoins = false;
    std::map<int64_t, int> _seats;                // 세션표 — 세션 → 방
    std::vector<NetworkRequest> _requests;
    std::vector<NetworkRequest> _pull;            // `PullRequests` 가 쓰는 그릇(다시 씀)
    std::vector<RoomEvent> _roomEvents;
    CRoomStats _stats;
};

}  // namespace nox
