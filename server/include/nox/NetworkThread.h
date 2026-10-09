// NetworkThread.h — 네트워크 스레드. 소켓 하나와 통신 층(`CUdpServer`)을 이 스레드 혼자 만진다
//   (`Docs/LAYERS_DESIGN.md` 「네트워크 스레드」 · 「조각 5d 설계」).
//
// ★ 10-09 조각 5d-2b — 게임 스레드와는 큐 둘로만 만난다.
//   통신 층 → 게임 : 이벤트 큐(`SwapOutNetworkEvents` — 게임 스레드가 틱 앞에 통째로 바꿔 간다 · 넣을 때 `EventsReadyHandle` 을 켠다)
//   게임 → 통신 층 : 요청 큐(`PushRequests` — 게임 스레드가 틱 뒤에 그 틱의 요청을 한 묶음으로 넣고 깨운다)
//   깨어나는 때 셋 — 소켓 읽기 · 요청이 들어옴 · 타이머 맨 앞 마감(`CUdpServer::NextTimerDue`).
//   깨면 시계(통신 층 `Tick` — 마감 지난 타이머) → 쌓인 요청 → 받을 것 전부 → 이벤트를 큐로 → 나갈 것 전부.
//   지표는 사본으로만 내준다(`LatestNetworkStats` · `LatestThreadStats`). 통신 층 설정(서버 찾기 원본 · 키 · 되받기 …)과
//   소켓 없는 세션 번호(`OpenSession` — 봇)는 `Start` 전에 `Network()` 로만 한다.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nox/UdpServer.h"

namespace nox {

// 네트워크 스레드 자신의 지표(통신 층 지표 `CUdpStats` 와 따로).
struct CNetworkThreadStats {
    unsigned long long wakes = 0;                 // 깨어난 수
    unsigned long long datagramsIn = 0;           // 받은 데이터그램
    unsigned long long datagramsOut = 0;          // 보낸 데이터그램
    unsigned long long maxDatagramsPerWake = 0;   // 한 번 깨어 비운 받은 데이터그램 최대
    unsigned long long recvErrors = 0;            // `recvfrom` 오류(비었음 빼고 — 받는 쪽이 닫힌 포트 알림 포함)
    unsigned long long eventsDropped = 0;         // 이벤트 큐 한도를 넘어 버린 수 — 0 이 정상
    unsigned long long requestsDropped = 0;       // 요청 큐 한도를 넘어 버린 수 — 0 이 정상
    size_t eventsQueued = 0;                      // 사본을 뜰 때 큐에 남은 이벤트 수
    size_t requestsQueued = 0;                    // 사본을 뜰 때 큐에 남은 요청 수
};

class CNetworkThread {
public:
    // 큐 한도(이벤트 · 요청 각각) — 넘치면 버리고 센다(MMO_Zone 이벤트 큐 accept 차단과 같은 자리)
    static constexpr size_t kQueueMax = 10000;
    // 타이머가 없을 때도 이만큼마다 깨어 지표 사본을 내놓는다
    static constexpr uint32_t kMaxWaitMs = 250;

    CNetworkThread();
    ~CNetworkThread();
    CNetworkThread(const CNetworkThread&) = delete;
    CNetworkThread& operator=(const CNetworkThread&) = delete;

    // 시작 전에만 — 통신 층 설정 · 소켓 없는 세션 번호(봇)
    CUdpServer& Network() { return _network; }

    // 소켓을 연다. `ip` 가 빈 글자면 모든 주소. `port` 0 이면 OS 가 고른다(시험 — `Port`). 실패하면 false · `err` 에 까닭.
    bool Open(const std::string& ip, uint16_t port, std::string& err);
    uint16_t Port() const { return _port; }
    void Start();
    // 깨워서 멈추고 스레드가 끝날 때까지 기다린다. 여러 번 불러도 된다.
    //   ★ 10-09 조각 5d-2c — 멈추기 전 마지막으로 한 번 돈다 — 쌓인 요청을 처리해 보내고, `closeSessions` 면 키를 준 상대마다
    //   `SERVER_CLOSE` 를 보낸다(`CUdpServer::CloseAll`). 그때는 받지 않고 타이머도 돌리지 않는다.
    void Stop(bool closeSessions = false);
    bool Running() const { return _thread.joinable(); }

    // ── 게임 스레드에서 부른다(스레드 안전) ──
    // 그 틱의 요청을 한 묶음으로 넣고 네트워크 스레드를 깨운다. `reqs` 는 비워진다.
    void PushRequests(std::vector<NetworkRequest>& reqs);
    // 쌓인 이벤트를 통째로 꺼낸다(`into` 는 비어 있다고 보고 바꾼다).
    void SwapOutNetworkEvents(std::vector<NetworkEvent>& into);
    // ★ 10-09 플랜 A 5d 자가검증 ② — 이벤트를 큐에 넣을 때 켜지는 이벤트(HANDLE · 자동으로 꺼짐). 게임 루프가 틱 마감 타이머와 함께
    //   기다린다(`WaitForMultipleObjects`) — 옛 `Sleep(1)` 돌려 보기 대신. `Open` 뒤에 있다.
    void* EventsReadyHandle() const { return _eventsReady; }
    CUdpStats LatestNetworkStats() const;
    CNetworkThreadStats LatestThreadStats() const;

    // 받은 조각마다 한 줄(`[받음]`)을 찍는가 — `main` 의 `--quiet` 반대. 시작 전에.
    void SetVerbose(bool v) { _verbose = v; }
    // 시험용 — 타이머가 없을 때 기다리는 최대 시간(기본 `kMaxWaitMs`). 길게 두면 깨어난 까닭(요청 · 타이머)이 시각으로 갈린다. 시작 전에.
    void SetMaxWaitMs(uint32_t ms) { _maxWaitMs = ms; }
    // 시험용 — 큐 한도(기본 `kQueueMax`). 작게 두어 넘칠 때 버리고 세는지 본다. 시작 전에.
    void SetQueueMaxForTest(size_t n) { _queueMax = n; }

private:
    void Run();
    void Publish();

    CUdpServer _network;
    uintptr_t _sock = ~static_cast<uintptr_t>(0);   // SOCKET(INVALID_SOCKET) — 머리에서 winsock 을 안 부르려고
    void* _sockEvent = nullptr;                     // WSAEVENT — 소켓 읽기
    void* _wakeEvent = nullptr;                     // 요청이 들어옴 · 멈춤
    void* _eventsReady = nullptr;                   // 이벤트를 큐에 넣음 — 게임 루프가 기다린다
    uint16_t _port = 0;
    bool _wsaUp = false;
    bool _verbose = false;
    uint32_t _maxWaitMs = kMaxWaitMs;
    size_t _queueMax = kQueueMax;
    std::thread _thread;
    std::atomic<bool> _stop{ false };
    std::atomic<bool> _closeOnStop{ false };

    mutable std::mutex _queueMu;                    // 아래 넷을 지킨다
    std::vector<NetworkRequest> _requests;
    std::vector<NetworkEvent> _events;
    unsigned long long _requestsDropped = 0;
    unsigned long long _eventsDropped = 0;

    mutable std::mutex _statsMu;                    // 지표 사본
    CUdpStats _pubNetwork;
    CNetworkThreadStats _pubThread;

    CNetworkThreadStats _threadStats;               // 네트워크 스레드 혼자 쓴다(큐 버림 셈은 위 큐 쪽)
    uint32_t _lastPublishMs = 0;
};

}  // namespace nox
