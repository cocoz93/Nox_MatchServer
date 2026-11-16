// LobbyRoomsQueue.h — 게임 스레드가 여럿일 때 로비 연결(조정 스레드)과 방(게임 스레드)을 큐로 잇는다
//   (★ 11-17 C-3d-3 · `Docs/SERVERCORE_UDP_PLAN_1116.md` 5-4 ~ 5-6 · 구체화 C-④ · 정할 것 C-나)
//
// 누가 무엇을
//   조정 스레드 — `CLobbyClient` + `CQueuedLobbyRooms`(틀 `ILobbyRooms` 의 큐 판). 명령은 게임 스레드 명령 큐에 넣고, 알림 묶음은 꺼내 얹는다.
//                 고리는 `CLobbyCoordinator`(로비 사건 → 알림 묶음 → `AfterTick`). 틱이 없다 — 로비 사건 · 알림 · 한 틱 길이마다 깬다
//   게임 스레드 k — `CLobbyRoomsAgent` 하나. 바퀴마다 제 명령 큐를 비워 제 방 관리자에 하고(새 방 · 계정 명령 · 점수 알림),
//                   바퀴 끝에 자기 방만 견준 바뀜(`CRoomFactDiff`) + 그동안 모인 결과 · 판 결과를 한 묶음으로 알림 큐에 넣는다
//   ⇒ 한 묶음 = 한 게임 스레드의 한 바퀴. 새 방의 ROOM 바뀜과 그 CREATED 결과가 같은 묶음에 실려 「ROOM 다음 CREATED」(`LobbyLink.h`)가 지켜진다
//
// 명령의 받는 쪽
//   새 방 → 고른 스레드 하나(C-나: 방 수 + 답 없는 새 방 수가 가장 적은 스레드 · 같으면 작은 번호). 번호는 그 스레드 방 관리자가 제 몫(% N == k)에서
//   계정 명령(CLIENT_GONE · KICK) → 모든 게임 스레드(그 이름이 어느 방인지 조정 스레드가 모른다 — C-④). 처리 수는 모두 답한 뒤 합으로 한 번
//   점수 알림 → 방 주인(방 번호 % N — `CRoomManager::SetRoomIdSlice`)
//
// 사람 화면 상태(ROOMUI)는 이름이 열쇠라 두 스레드가 같은 이름을 낼 수 있다(방을 옮긴 사람 — 옛 방의 나감이 새 방의 들어옴보다 늦게 올 수 있다).
//   그래서 이름마다 「어느 스레드가 무엇을 냈나」를 쥐고, 나감은 그 스레드 몫만 지운다. 남은 몫이 있으면 가장 늦게 낸 스레드 줄이 산다.
//   방 번호는 한 스레드 몫이라 온 차례대로 얹는다. 둘 다 「로비 연결에 이미 넘긴 것」과 견줘 달라진 것만 넘긴다(넘긴 적 없는 방의 사라짐은 안 넘긴다)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nox/LobbyClient.h"
#include "nox/RoomManager.h"
#include "nox/UdpServer.h"   // NetworkEvent · NetworkRequest

namespace nox {

class CNetworkThread;

// 조정 스레드 → 게임 스레드
struct LobbyRoomCommand {
    enum class Type : uint8_t { Create, ClientGone, Kick, NoticeRatings };
    Type type = Type::Create;
    uint32_t seq = 0;          // Create = 로비 요청 번호(결과의 짝) · 나머지 = 명령 번호(`CQueuedLobbyRooms` 가 매김)
    RoomSettings settings;     // Create
    std::string account;       // ClientGone · Kick
    int roomId = -1;           // NoticeRatings
    std::vector<std::pair<std::string, std::string>> notes;   // NoticeRatings — (이름 · 글)
};

// 게임 스레드 → 조정 스레드 — 한 게임 스레드의 한 바퀴 몫
struct LobbyRoomReport {
    int thread = 0;
    size_t roomCount = 0;                       // 이 묶음 때 그 스레드의 방 수(C-나 고르기)
    LobbyFactChanges changes;                   // 자기 방만 견준 바뀜
    std::vector<LobbyCreateResult> creates;     // 새 방 결과(이 묶음의 `changes` 에 그 방의 ROOM 이 먼저 실린다)
    std::vector<CRoomManager::RankNoShow> noShows;
    struct Reply {                              // 계정 명령 · 점수 알림의 답(명령을 한 차례 그대로 — 무슨 명령이었나는 조정 스레드가 번호로 안다)
        uint32_t seq = 0;
        size_t sessions = 0;                    // ClientGone · Kick — 이 스레드가 처리한 세션 수
        bool found = false;                     // NoticeRatings — 그 방이 이 스레드에 있었나
        std::vector<size_t> sent;               // NoticeRatings — 이름마다 보낸 사람 수
    };
    std::vector<Reply> replies;
    struct Match {
        std::string match;                      // 판 번호(`MatchIdOf`)
        std::string json;                       // `FormatMatchResultJson`
        int roomId = -1;
    };
    std::vector<Match> matches;                 // 판 결과(조정 고리가 로비 연결 `OnMatchResult` 에 넘긴다)
    bool Empty() const;
};

// 큐 둘 — 명령(게임 스레드마다 하나) · 알림(하나). 넣으면 받는 쪽 이벤트를 켠다(자동으로 꺼지는 이벤트 — 깬 쪽이 통째로 비운다)
class CLobbyRoomsHub {
public:
    explicit CLobbyRoomsHub(int gameThreads);   // 1 미만이면 1
    ~CLobbyRoomsHub();
    CLobbyRoomsHub(const CLobbyRoomsHub&) = delete;
    CLobbyRoomsHub& operator=(const CLobbyRoomsHub&) = delete;
    int GameThreads() const { return static_cast<int>(_commands.size()); }

    void PushCommand(int thread, LobbyRoomCommand c);   // 없는 번호면 버리고 센다(`CommandsBadThread`)
    void SwapOutCommands(int thread, std::vector<LobbyRoomCommand>& into);   // `into` 는 비우고 받는다
    void* CommandsReadyHandle(int thread) const;
    void PushReport(LobbyRoomReport r);
    void SwapOutReports(std::vector<LobbyRoomReport>& into);
    void* ReportsReadyHandle() const { return _reportsReady; }
    unsigned long long CommandsBadThread() const { return _badThread; }

private:
    struct CommandQueue {
        std::mutex lock;
        std::vector<LobbyRoomCommand> items;
        void* ready = nullptr;
    };
    std::vector<std::unique_ptr<CommandQueue>> _commands;
    std::mutex _reportLock;
    std::vector<LobbyRoomReport> _reports;
    void* _reportsReady = nullptr;
    unsigned long long _badThread = 0;   // 조정 스레드만 쓴다
};

// 게임 스레드 몫 — 그 스레드에서만 부른다
class CLobbyRoomsAgent {
public:
    CLobbyRoomsAgent(CLobbyRoomsHub& hub, int thread, CRoomManager& rooms) : _hub(hub), _thread(thread), _rooms(rooms) {}
    // 명령 큐를 비워 제 방 관리자에(새 방 · 계정 명령 · 점수 알림). 결과는 다음 `Report` 묶음에. 돌려주는 값 = 처리한 명령 수
    size_t RunCommands();
    // 판 결과 — 다음 `Report` 묶음에(게임 스레드 판 결과 꺼내기 걸이에서)
    void AddMatch(std::string match, std::string json, int roomId);
    // 바퀴 끝 — 자기 방만 견준 바뀜 + 모인 결과. 빈 묶음이고 방 수도 그대로면 안 보낸다. 보냈으면 true
    bool Report();
    int Thread() const { return _thread; }

private:
    CLobbyRoomsHub& _hub;
    int _thread;
    CRoomManager& _rooms;
    CRoomFactDiff _diff;
    LobbyRoomReport _out;
    std::vector<LobbyRoomCommand> _cmds;
    bool _sentOnce = false;
    size_t _lastRoomCount = 0;
};

// 조정 스레드 몫 — 틀의 큐 판. 조정 스레드에서만 부른다
class CQueuedLobbyRooms final : public ILobbyRooms {
public:
    CQueuedLobbyRooms(CLobbyRoomsHub& hub, bool rankOneMap);
    bool RankOneMap() const override { return _rankOneMap; }
    void CreateRoom(uint32_t req, const RoomSettings& s) override;
    void TakeCreateResults(std::vector<LobbyCreateResult>& out) override;
    void CollectChanges(LobbyFactChanges& out) override;
    void TakeRankNoShows(std::vector<CRoomManager::RankNoShow>& out) override;
    void ClientLobbyGone(const std::string& account) override { Broadcast(LobbyCommandResult::Kind::ClientGone, account); }
    void KickAccount(const std::string& account) override { Broadcast(LobbyCommandResult::Kind::Kick, account); }
    void NoticeRatings(int roomId, std::vector<std::pair<std::string, std::string>> notes) override;
    void TakeCommandResults(std::vector<LobbyCommandResult>& out) override;

    // 알림 묶음 하나를 얹는다(판 결과는 고리가 따로 넘긴다)
    void Absorb(const LobbyRoomReport& r);
    // C-나 — 다음 새 방을 받을 스레드(방 수 + 답 없는 새 방 수가 가장 적은 · 같으면 작은 번호)
    int PickThread() const;
    size_t RoomsOf(int thread) const { return thread >= 0 && thread < static_cast<int>(_roomCount.size()) ? _roomCount[thread] : 0; }
    size_t CreatesInFlight(int thread) const { return thread >= 0 && thread < static_cast<int>(_inFlight.size()) ? _inFlight[thread] : 0; }
    size_t CommandsWaiting() const { return _waiting.size(); }   // 답을 다 못 받은 계정 명령 · 점수 알림
    unsigned long long UnknownReplies() const { return _unknownReplies; }   // 기다리지 않는 번호의 답(셈만)

private:
    void Broadcast(LobbyCommandResult::Kind kind, const std::string& account);

    CLobbyRoomsHub& _hub;
    bool _rankOneMap = false;
    std::vector<size_t> _roomCount;   // 스레드마다 마지막 묶음의 방 수
    std::vector<size_t> _inFlight;    // 스레드마다 보내고 답 없는 새 방 수
    std::vector<LobbyCreateResult> _done;
    std::vector<CRoomManager::RankNoShow> _noShows;
    // 방 — 지금 줄(사라지면 빈 칸 · `present` false) · 로비 연결에 넘긴 줄
    struct RoomNow {
        bool present = false;
        std::string line;
    };
    std::map<uint32_t, RoomNow> _roomNow;      // 넘긴 뒤 바뀐 방만
    std::map<uint32_t, std::string> _roomGiven;
    // 사람 — 이름마다 스레드별 줄(낸 차례 번호와 함께) · 넘긴 줄
    std::map<std::string, std::map<int, std::pair<unsigned long long, std::string>>> _uiBy;
    std::set<std::string> _uiDirty;
    std::map<std::string, std::string> _uiGiven;
    unsigned long long _uiStamp = 0;
    // 계정 명령 · 점수 알림 — 답 기다림(명령 번호 → 모을 결과 · 남은 답 수)
    struct Waiting {
        LobbyCommandResult result;
        int left = 0;
    };
    std::map<uint32_t, Waiting> _waiting;
    uint32_t _nextSeq = 1;
    std::vector<LobbyCommandResult> _cmdDone;
    unsigned long long _unknownReplies = 0;
};

// 조정 스레드 고리
class CLobbyCoordinator {
public:
    static constexpr uint32_t kIdleWaitMs = 33;   // 한 틱(30Hz) — 살아 있음(`CLobbyClient::LinkTick`)을 옛 게임 고리처럼 이만큼마다 본다

    CLobbyCoordinator(CLobbyRoomsHub& hub, CQueuedLobbyRooms& rooms, CLobbyClient& lobby) : _hub(hub), _rooms(rooms), _lobby(lobby) {}
    // 한 바퀴 — 로비 사건마다 로비 연결 → 알림 묶음마다 얹기 · 판 결과 넘기기 → `AfterTick`. 낸 줄은 `out` 뒤에
    void Turn(const std::vector<NetworkEvent>& lobbyEvents, std::vector<NetworkRequest>& out);
    // 닫기 신호까지 — 통신 스레드의 로비 큐(`queue`)에서 사건을 꺼내고 낸 줄을 그 큐로 넣는다.
    //   로비 사건 · 알림 묶음이 오거나 `kIdleWaitMs` 마다 깬다
    void Run(CNetworkThread& net, int queue, const std::function<bool()>& stopRequested);
    unsigned long long Turns() const { return _turns; }
    unsigned long long ReportsTaken() const { return _reportsTaken; }
    unsigned long long MatchesPassed() const { return _matchesPassed; }

private:
    CLobbyRoomsHub& _hub;
    CQueuedLobbyRooms& _rooms;
    CLobbyClient& _lobby;
    std::vector<LobbyRoomReport> _reports;
    unsigned long long _turns = 0;
    unsigned long long _reportsTaken = 0;
    unsigned long long _matchesPassed = 0;
};

}  // namespace nox
