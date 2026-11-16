// GameThread.h — 게임 스레드 하나의 고리(★ 11-17 C-3d-2 — 옛 `main.cpp` 의 게임 루프를 그대로 옮겼다 · C 플랜 `Docs/SERVERCORE_UDP_PLAN_1116.md` 5절)
//
// 게임 스레드 하나 = 방 관리자 하나(`CRoomManager` — 제 몫의 방만) + 통신 스레드의 제 사건 큐 · 요청 큐(번호 `Config::index`).
// 한 바퀴: 닫기 신호 → 사건 꺼내기(로비 사건은 걸이 · 나머지는 방 관리자) → 판 차리기 → 틱 앞 요청 → 틱 → 판 결과 → 방 진단 →
//          초당 통신 · 스레드 줄 → `[구간]` → 바퀴 끝 걸이(로비 연결 `AfterTick`) → 「사건 들어옴 · 다음 틱 마감」 중 먼저 오는 것을 기다림.
// 게임 스레드가 하나(기본)면 옛 `main.cpp` 고리와 줄 · 차례가 같다 — 로비 연결은 이 스레드의 걸이로 돈다(`main.cpp` 가 건다).
// 여럿이면 로비 사건은 조정 스레드가 받고(C-3d-3) 이 스레드는 판 결과만 걸이로 넘긴다(C-3d-4).
//
// 걸이로 받는 까닭 — 로비 연결 · 크래시 덤프 처리기는 실행 파일 몫이다. `CrashDump.h` 는 넣기만 하면 전역 처리기를 걸어서
// 이 파일(`nox_proto` — 시험 · 재생 도구도 링크한다)에 넣으면 그 도구들의 죽는 꼴까지 바뀐다.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "nox/NetworkThread.h"
#include "nox/RoomManager.h"

namespace nox {

class CMatchRecorder;
class CTickSectionReport;
struct CSectionNetTotals;
struct CSectionRoomTotals;

class CGameThread {
public:
    struct Config {
        int index = 0;                         // 게임 스레드 번호 = 사건 큐 · 요청 큐 번호(`CNetworkThread::SetGameThreads`)
        bool quiet = false;                    // `--quiet` — 초당 통신 · 스레드 줄을 안 찍는다
        bool processLines = true;              // 초당 통신 · 스레드 줄(프로세스 몫 — 여럿이면 게임 스레드 0 만)
        bool tickSections = false;             // `--tick-sections` — `[구간]` 줄
        void* recorderThread = nullptr;        // `[구간]` CPU 칸(게임 스레드 0 줄에만 쓴다 — `CTickSectionReport`)
        void* coordinatorThread = nullptr;
        CMatchRecorder* recorder = nullptr;    // 판 결과 기록기(없으면 줄만)
        std::function<bool()> stopRequested;   // 닫기 신호 — 바퀴 맨 위에서 본다
    };
    struct Hooks {
        // 로비 사건(`IsLobbyEvent`) — 비면 버린다(옛 꼴: 로비 연결이 없을 때와 같음). 낸 줄은 `out` 에(이 스레드가 넘긴다)
        std::function<void(const NetworkEvent& ev, std::vector<NetworkRequest>& out)> lobbyEvent;
        // 판 결과(출력 줄 · 기록기 뒤) — 로비 연결에(`CLobbyClient::OnMatchResult`)
        std::function<void(const RoomEvent& e, std::vector<NetworkRequest>& out)> matchResult;
        // 바퀴 끝(틱 블록 밖 · 방이 없어도) — 로비 연결 `AfterTick`
        std::function<void(std::vector<NetworkRequest>& out)> afterTurn;
        // `--debug-crash` — 방 로그를 찍은 뒤 부른다(크래시 덤프는 실행 파일 몫 · 머리 주석)
        std::function<void()> debugCrash;
    };
    // 끝 셈 — 셋은 게임 스레드의 `thread_local` 누계(`TickSections.h`)라 그 스레드에서 담는다(`Finish`)
    struct Totals {
        std::map<uint8_t, unsigned long long> seen;   // 받은 메시지 종류별(닫힌 방 포함 · `CRoomManager::SeenTotals`)
        unsigned long long inputs = 0;                // `g_inputTotal`
        unsigned long long aliasTaken = 0;            // `g_aliasTaken`
        unsigned long long aliasStaleDropped = 0;     // `g_aliasStaleDropped`
    };

    CGameThread(CNetworkThread& net, CRoomManager& rooms, Config cfg, Hooks hooks);
    ~CGameThread();
    CGameThread(const CGameThread&) = delete;
    CGameThread& operator=(const CGameThread&) = delete;

    // 닫기 신호까지 돈다 — 부른 스레드가 이 게임 스레드다(틱 타이머 · `[구간]` 보고기도 여기서 만든다)
    void Run();
    // 고리 뒤 · 같은 스레드 — 마지막 `[구간]`(통신 스레드를 멈추기 전에 · 핸들이 닫히므로) · 끝 셈 담기
    void Finish();
    // 그 사이 쌓인 요청을 통신 스레드에 — 통신 스레드를 멈추기 전에(멈출 때 보낸다)
    void FlushRequests();
    // 판 결과 꺼내기 — 틱 뒤마다(`Run`) · 끝낼 때 한 번(통신 스레드를 멈춘 뒤 — 옛 꼴 그대로)
    void DrainRoomEvents();
    const Totals& FinalTotals() const { return _totals; }
    unsigned long long MatchResults() const { return _matchResults; }

private:
    void HandleEvents();
    void PrintProcessLines();   // 초당 통신 · 스레드 줄(옛 `main.cpp` 블록 그대로)
    void Wait();
    CSectionNetTotals SectionNet() const;     // ★ 11-11 부하 측정 M0 조각 3 — `[구간]` 에 넘길 통신 층 누계(넘침 · 큐 버림)
    CSectionRoomTotals SectionRooms() const;  // ★ 11-17 C-3a — `[구간]` 줄 끝의 방 수 · 덜 참(C 플랜 7절)

    CNetworkThread& _net;
    CRoomManager& _rooms;
    Config _cfg;
    Hooks _hooks;

    void* _tickTimer = nullptr;
    std::unique_ptr<CTickSectionReport> _sectionReport;
    // ★ 10-09 조각 5d-2b — 받기 · 주소 · 보내기는 네트워크 스레드 몫이다(옛 `keyOf` · `addrOf` · `flush` 는 `NetworkThread.cpp`).
    std::vector<NetworkEvent> _netEvents;
    std::vector<NetworkRequest> _netRequests;
    std::vector<NetworkRequest> _lobbyOut;   // 걸이가 낸 줄(옛 `main.cpp` `lobbyRequests` — ★ 10-14 작업 8d — 로비가 낸 줄(`LOBBY_SEND`) · ★ 10-20 판 결과 · 틱 뒤 방 알림도)
    std::vector<RoomEvent> _roomEvents;
    unsigned long long _matchResults = 0;
    Totals _totals;

    unsigned long long _nTicks = 0;
    // 초당 `[스레드]` 줄 — 이벤트 넘김 지연(큐에 넣은 때 → 게임이 처리한 때) · 틱 시간(`RunTick`)
    unsigned long long _evCount = 0, _evLatSum = 0, _tickRuns = 0;
    uint32_t _evLatMax = 0;
    double _tickMsSum = 0.0, _tickMsMax = 0.0;
    // ★ 10-09 플랜 A 5d 자가검증 ② — 틱 간격(앞 `RunTick` 시작 → 이번 시작). 목표 33ms — 루프가 깨는 때가 거칠면 여기서 흔들린다
    std::chrono::steady_clock::time_point _prevTickStart{};
    double _tickGapSum = 0.0, _tickGapMin = 0.0, _tickGapMax = 0.0;
    unsigned long long _tickGaps = 0;
    // 게임 루프가 돈 바퀴 수 — 틱(30) + 이벤트로 깬 수 근처여야 한다. 기다림이 빠지면(헛돎) 수만 단위로 뛴다
    unsigned long long _loopTurns = 0;
    std::map<uint8_t, bool> _told;   // ★ 10-12 작업 7c — `[처음]` 줄을 낸 오프코드(옛 루프 안 `static` · ★ C-3d-2 게임 스레드마다)
    unsigned long long _procMade = 0;   // ★ 10-12 작업 7c — 이 1초 동안 낸 보내기 요청(프로세스 몫 초당 줄의 문턱)
    uint32_t _procLastMs = 0;           //   그 줄을 마지막으로 찍은 때
};

}  // namespace nox
