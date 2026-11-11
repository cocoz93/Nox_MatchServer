#pragma once
// ★ 11-10 부하 실험 — 구간 시간(`--tick-sections` · `Docs/MO_DUMMY_PLAN_1109.md` 5-1절).
//   물음은 「인원을 늘릴 때 어느 구간이 먼저 차는가」다. 틱 전체 시간만으로는 답이 안 나와 틱 안을 나눠 잰다.
//   스위치를 안 주면 시계를 읽지도 찍지도 않는다. 끊김 · 넘침 셈만 늘 돈다 — 드문 길의 정수 더하기이고,
//   사건 기록을 꺼도 읽어야 하는 값이기 때문이다(변수 통제 플랜 5-3).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nox {

using SectionClock = std::chrono::steady_clock;

inline bool g_tickSections = false;   // `--tick-sections`(방을 만들기 전에 한 번 정하고 안 바꾼다)

// ★ 11-10 부하 실험 — 계측 끄는 스위치 셋(더미 플랜 5-2절 · 변수 통제 플랜 V5). 방을 만들기 전에 한 번 정하고 안 바꾼다.
//   기본은 지금과 같다. 켬 · 끔 차이를 따로 재서 넘는 것만 끄려고 하나씩 둔다. 끊김 · 넘침 셈은 `Log` 바깥이라 사건 기록을 꺼도 읽힌다
inline bool g_noEventLog = false;    // `--no-event-log` — `Server::Log` 입구에서 바로 돌아간다
inline bool g_noFirstSeen = false;   // `--no-first-seen` — 「처음 본 종류」 알림 두 곳을 안 돈다
inline bool g_noOpCount = false;     // `--no-op-count` — 메시지 종류별 셈 · 조각마다 읽은 종류 목록을 안 채운다

inline double MsSince(SectionClock::time_point from) {
    return std::chrono::duration<double, std::milli>(SectionClock::now() - from).count();
}

// `Server::RunTick` 을 틱 처리 순서대로 끊은 11구간(플랜 5-1절 표). 경계마다 「지난 경계부터 여기까지」를 그 구간에 더하므로
// 틱의 모든 줄이 어느 한 구간에 들어간다.
enum class TickSection : uint8_t {
    Front,      // 앞 — 틱 시작 · 판 다시 열기 · 자리 비우기 · 진단 스위치
    Walk,       // 걸음 — 사람 걸음 · 미사일
    Think,      // 생각 — 소환수 · 몬스터 생각 · 죽은 것 정리 · 용암
    Cast,       // 시전 — 시전 목록 · 지속 주문
    World,      // 세계 — 걷는 표시 · 깃발 · 공 · 투표 · 함정 · 승패 · 소품 · 소리 · 독 · 미뤄 둔 일 · 시계 · 점수판 · 나간 사람 시각
    Self,       // 자기 — 받는 사람 머리 · 꼬리
    People,     // 사람 — 받는 사람 × 보이는 사람(N² 의 자리)
    Objects,    // 물체 — 미사일 · 놓은 것 · 깃발 · 공
    Creatures,  // 소환수 — 크리처 항목
    Statics,    // 정적 — 맵 정적 물체 · 나감 · 보내기 요청
    Bundle,     // 묶음 — 사람마다 한 조각으로 묶기
    Count
};

// 게임 스레드 몫의 이번 1초 합(ms). 게임 스레드만 쓰고 읽는다. 네트워크 스레드가 쓰는 누계와 캐시 줄을 나눈다(같은 줄이면 두 스레드가 서로 캐시를 밀어내 재는 값에 끼어든다).
struct alignas(64) CGameSectionTotals {
    double sectionMs[static_cast<size_t>(TickSection::Count)] = {};
    double afterMs = 0.0;                 // 뒷일 — `CRoomManager::RunTick` 에서 방마다 게임 틱 뒤
    double eventMs = 0.0;                 // 사건 처리 — 받은 메시지를 게임에 넣는 곳(틱 시간에 안 들어간다)
    unsigned long long eventCount = 0;    // 지나간 양 — 0 이 「없었다」인지 「못 읽었다」인지 가른다
    double roomCreateMs = 0.0, roomCreateMaxMs = 0.0;   // 방 만들기 — 입장 사건 안이라 사건 처리의 일부
    unsigned long long roomCreates = 0;
    double roomCloseMs = 0.0, roomCloseMaxMs = 0.0;     // 방 닫기 — 틱 앞 `BeginTick` 안이라 틱 시간에도 사건 처리에도 안 들어간다
    unsigned long long roomCloses = 0;
    // ★ 11-11 부하 측정 M0 조각 3 — 게임 고리가 기다린 시간(`main.cpp` 의 세 대기 자리 — 틱 마감 타이머 · 타이머 없을 때 · 방 없을 때).
    //   바쁜 비율 = (지난 시간 − 이것) ÷ 지난 시간. 한 자리만 재면 나머지 두 자리에서 쉰 시간이 바쁨으로 잡힌다(측정 플랜 2-2절)
    double waitMs = 0.0;
    // ★ 11-11 M0 조각 3 — 넘김 지연(통신 스레드가 이벤트를 큐에 넣은 시각 → 게임 스레드가 꺼내 처리하기 시작한 시각, ms 정수). 수는 `eventCount`
    unsigned long long handoffLatSumMs = 0;
    uint32_t handoffLatMaxMs = 0;

    void AddRoomCreate(double ms) { roomCreateMs += ms; ++roomCreates; if (ms > roomCreateMaxMs) roomCreateMaxMs = ms; }
    void AddRoomClose(double ms) { roomCloseMs += ms; ++roomCloses; if (ms > roomCloseMaxMs) roomCloseMaxMs = ms; }
    void Reset() { *this = CGameSectionTotals{}; }
};
inline CGameSectionTotals g_gameSections;

// 네트워크 스레드 몫의 누계(ns). 쓰는 쪽은 네트워크 스레드 하나고 게임 스레드가 초당 한 번 읽어 앞 값과 뺀다.
// 줄마다 쌓아 두었다 넘기는 지표 사본(`Publish` · 0.25초마다)을 쓰면 1초 줄 안에 0.75~1.25초 몫이 들어가 합이 흔들린다.
struct alignas(64) CNetworkSectionTotals {
    std::atomic<uint64_t> resendNs{0};     // 재전송
    std::atomic<uint64_t> sendNs{0};       // 보내기 — 요청 처리 + `sendto` 고리
    std::atomic<uint64_t> recvNs{0};       // 받기
    std::atomic<uint64_t> recvCount{0};    // 받은 데이터그램(지나간 양)
    std::atomic<uint64_t> handoffNs{0};    // 넘기기 — 사건을 큐로
    // ★ 11-11 부하 측정 M0 조각 4 — 기다림(`NetworkThread.cpp` 의 `WSAWaitForMultipleEvents`). 통신 바쁜 비율 = (지난 시간 − 이것) ÷ 지난 시간.
    //   네 구간의 합은 깬 직후의 일이 빠져 쓰지 않는다(측정 플랜 2-2절). 기다림이 끝나야 더하므로 `[구간]` 줄을 찍는 순간 기다리던 몫은
    //   다음 줄로 넘어간다 — 줄 하나는 최대 0.25초(`kMaxWaitMs`)만큼 흔들리고, 측정 구간 합으로 읽으면 맞는다
    std::atomic<uint64_t> waitNs{0};
};
inline CNetworkSectionTotals g_networkSections;

// ★ 11-11 부하 측정 M0 조각 4 — 서버 보내기 실패(`sendto` 가 `SOCKET_ERROR`). 통신 스레드만 쓴다. 스위치와 상관없이 센다 —
//   드문 길의 정수 더하기이고 판정 값이라서다(측정 플랜 2-2절 넘음 넷째 — 소켓 버퍼가 넘치면 조각이 소리 없이 사라져 부하가 낮게 잡힌다).
//   `datagramsOut` 은 전처럼 시도한 수다(실패 포함)
struct alignas(64) CSendFailTotals {
    std::atomic<uint64_t> count{0};
    std::atomic<int> lastError{0};   // 마지막 실패의 `WSAGetLastError` — 10035(`WSAEWOULDBLOCK`)면 보내기 버퍼가 꽉 찬 것
};
inline CSendFailTotals g_sendFail;

// ★ 11-11 부하 측정 M0 조각 4 — 보내는 쪽 지연(측정 플랜 2-2절 · 한계 판정 값). 게임 스레드가 보내기 요청을 큐에 넣은 시각
//   (`CNetworkThread::PushRequests` · `NetworkRequest::queuedMs`)부터 통신 스레드가 그 깸의 `sendto` 고리를 다 돈 시각까지(ms 정수 · 1ms 칸).
//   보내기 요청은 그 깸 안에서 곧바로 나갈 조각이 된다(`CUdpServer::SendOn`). 한 깸에 나간 요청은 고리 끝 시각 하나로 재므로
//   고리 앞쪽 조각은 고리 길이만큼 크게 잡힌다 — 넘음 쪽으로 기우는 쪽이라 그대로 둔다. 요청 하나 = 표본 하나(`SEND_MSG` 만 · 다시 보내기는 요청이 아니라 뺀다).
//   통신 스레드만 쓰고 게임 스레드가 10초마다 읽어 앞 값과 뺀다(`[틱분포]` 와 같은 때 `[보내기분포]` 줄). 전역이라 0 으로 시작한다.
//   표본 수는 따로 세지 않고 칸의 합으로 낸다 — 따로 세면 읽는 순간 쓰는 중인 몫 때문에 줄 하나에서 분포 합과 어긋날 수 있다
struct alignas(64) CSendDelayTotals {
    static constexpr size_t kSlots = 100;   // 1ms 칸 0~99 + 「100이상」 — `[틱분포]` 와 같은 칸
    std::atomic<uint64_t> histogram[kSlots + 1];
};
inline CSendDelayTotals g_sendDelay;

// 쓰는 스레드가 하나뿐이라 잠금 더하기가 필요 없다 — 읽어서 더해 쓴다
inline void AddRelaxed(std::atomic<uint64_t>& total, uint64_t v) {
    total.store(total.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline void AddNetworkSection(std::atomic<uint64_t>& total, SectionClock::time_point& last) {
    const SectionClock::time_point t = SectionClock::now();
    AddRelaxed(total, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t - last).count()));
    last = t;
}

// 프로세스 몫 누계 — 게임 스레드만 쓴다. 방이 닫혀도 안 사라지게 방 밖에 둔다.
inline unsigned long long g_leaveTotal = 0;                // 끊김 = `[나감]` 줄을 내는 두 자리(서버 종료 답 0xc8 은 끊김이 아니라 뺀다)
inline unsigned long long g_importantOverflowTotal = 0;    // 중요 메시지 대기열 넘침으로 끊은 수
// ★ 11-11 부하 측정 M0 조각 3 — 끊김을 까닭으로 나눈다(합 = `g_leaveTotal`). `g_leaveTotal` 은 더미가 스스로 나간 `0a` 도 세어
//   「시간 초과로 끊긴 사람」 판정(측정 플랜 2-2절 넘음 셋째)에 그대로 못 쓴다.
//   ⚠ 시간 초과 칸에는 두 넘침으로 끊은 것도 든다 — 둘 다 시간 초과와 같은 길로 끊는다(`Server.cpp` `ImportantOverflowCheck` · `UdpServer.cpp` 신뢰 대기열).
//   전송 층 넘침 셈은 끊은 사람 수가 아니라 넘친 횟수라 빼서 「순수 시간 초과」를 낼 수 없다. 판정에서는 둘 다 넘음이라 가를 필요가 없다
inline unsigned long long g_leaveTimeoutTotal = 0;   // 시간 초과(조용함 · 넘침)
inline unsigned long long g_leaveCloseTotal = 0;     // 접속 닫기 `0a`
// ★ 11-11 부하 측정 M0 조각 5 — 받은 입력(`0x3f PLAYER_INPUT`) 수. 메시지 종류별 셈(`Server::seen_`)과 떼어 `--no-op-count` 여도 센다 —
//   그 셈에서 읽던 때는 끄면 0 이 찍혀(실측 `s9_op` 더미 13,336 · 서버 0) 입력 손실 판정(측정 플랜 2-2절 넘음 넷째)이 늘 넘음이 됐다.
//   종류별 셈과 같은 자리(`0x00` 에서 끊기 뒤)에서 센다. 게임 스레드만 쓴다. 방 밖에 두어 닫힌 방 몫도 남는다
inline unsigned long long g_inputTotal = 0;

// ★ 11-11 M0 조각 3 — `Print` 에 넘기는 통신 층 누계. 지표 사본(`CNetworkThread::LatestNetworkStats` · `LatestThreadStats`, 0.25초마다 갱신)에서 읽는다
struct CSectionNetTotals {
    unsigned long long reliableOverflow = 0;   // 전송 층 신뢰 대기열 넘침(횟수)
    unsigned long long eventsDropped = 0;      // 이벤트 큐 한도를 넘어 버린 수
    unsigned long long requestsDropped = 0;    // 요청 큐 한도를 넘어 버린 수
};

// ★ 11-11 부하 측정 M0 조각 2 — 틱 완료 시간(`Docs/MO_MEASURE_PLAN_1111.md` 2-1절 · 한계 판정 값). 방의 틱이 돌기로 예정된 시각
//   (`Server::BeginTick` 이 `lastTickMs_` 를 `tickMs_` 씩 올린 값)부터 그 방의 틱 일(`CRoomManager::RunTick` 의 방 하나 몫 · 뒷일까지)이
//   끝난 시각까지. 게임 스레드 하나가 방을 차례로 도니 앞선 사건 처리 · 앞 차례 방의 틱 · 방 만들기로 늦어진 몫이 저절로 들어간다 —
//   `[틱분포]`(`rooms.RunTick()` 한 번의 길이)는 방이 여럿이면 이 몫을 못 본다.
//   몰아 돈 틱(`BeginTick` 이 2 이상)은 틱마다 자기 예정 시각으로 하나씩 넣는다 — 묶음에 하나만 넣으면 늦은 틱이 빠져 가장 느린 1%가 낮게 나온다.
//   시계는 `NowMs`(ms 정수). 예정 시각은 정수라 잰 값은 실제보다 1ms 미만 작다 — 칸이 1ms · 문턱이 33ms라 가르기엔 넉넉하다.
//   방의 보내기 요청은 모든 방이 끝난 뒤 한 번에 넘기므로(`main.cpp` 의 `net.PushRequests`) 앞 차례 방은 넘긴 시각보다 일찍 끝난 것으로 잡힌다.
//   가장 느린 1%는 뒤 차례 방에서 나오므로 그 차이는 작다고 본다(어림 · 안 잼). 게임 스레드만 쓰고 읽는다.
struct CTickCompletion {
    static constexpr size_t kSlots = 100;   // 1ms 칸 0~99 + 「100이상」 — `[틱분포]` 와 같은 칸
    unsigned long long histogram[kSlots + 1] = {};
    unsigned long long samples = 0;          // 지나간 양 — 방별 틱 수 합(몰아 돈 틱 포함)과 같아야 한다
    unsigned long long lateRuns = 0;         // 한 번에 2틱 이상 몰아 돈 횟수(`Server::BeginTick` 의 while)
    unsigned long long lateExtraTicks = 0;   // 그때 더 돈 틱 수(틱 수 − 1 의 합)
    void Add(uint32_t ms) { ++histogram[ms >= kSlots ? kSlots : ms]; ++samples; }
    void Reset() { *this = CTickCompletion{}; }
};
inline CTickCompletion g_tickCompletion;

// 경계마다 `End(이 구간)` 을 부른다. 꺼져 있으면 시계를 안 읽는다
class CTickSectionClock {
public:
    CTickSectionClock() : _on(g_tickSections) { if (_on) _last = SectionClock::now(); }
    void End(TickSection s) {
        if (!_on) return;
        const SectionClock::time_point t = SectionClock::now();
        g_gameSections.sectionMs[static_cast<size_t>(s)] += std::chrono::duration<double, std::milli>(t - _last).count();
        _last = t;
    }
private:
    bool _on;
    SectionClock::time_point _last;
};

// `[구간]` 줄(초당) · `[틱분포]` 줄(10초마다) · `[완료분포]` 줄(`[틱분포]` 바로 뒤 · 11-11 M0 조각 2). `--quiet` 와 상관없이 찍는다. 게임 스레드에서만 부른다.
class CTickSectionReport {
public:
    // 핸들 — 게임 · 네트워크 · 판 기록 스레드(판 기록을 꺼서 없으면 nullptr)
    CTickSectionReport(void* gameThread, void* networkThread, void* recorderThread, uint32_t nowMs);
    void AddTick(double tickMs);
    bool Due(uint32_t nowMs) const { return static_cast<uint32_t>(nowMs - _lastPrintMs) >= kPrintEveryMs; }
    void Print(uint32_t nowMs, const CSectionNetTotals& net);
    // 끝날 때 — 10초를 못 채운 나머지 틱까지 찍는다(분포 총합 = 틱 수가 실행 전체에서 맞게). 스레드를 join 하면 핸들이 닫히므로 그 전에 부른다
    void PrintFinal(uint32_t nowMs, const CSectionNetTotals& net);

private:
    static constexpr uint32_t kPrintEveryMs = 1000;
    static constexpr uint32_t kLinesPerHistogram = 10;
    static constexpr size_t kHistogramSlots = 100;   // 1ms 칸 0~99 + 「100이상」 한 칸 — 33ms 예산의 세 배면 넘긴 것을 가르기에 넉넉하다

    void PrintHistogram(uint32_t nowMs);

    void* _threads[3];
    double _cpuMsLast[3] = {};
    uint32_t _startMs;
    uint32_t _lastPrintMs;
    SectionClock::time_point _lastPrintAt;   // ★ 11-11 M0 조각 3 — 줄마다 실제로 지난 ms(서버가 밀리면 줄 간격이 1초보다 벌어진다 · 측정 플랜 2-3절)
    uint32_t _linesSinceHistogram = 0;

    unsigned long long _ticks = 0;
    double _tickSumMs = 0.0, _tickMaxMs = 0.0;
    unsigned long long _histogram[kHistogramSlots + 1] = {};
    unsigned long long _histogramTicks = 0;

    uint64_t _resendNsLast = 0, _sendNsLast = 0, _recvNsLast = 0, _recvCountLast = 0, _handoffNsLast = 0;
    uint64_t _netWaitNsLast = 0, _sendFailLast = 0;                          // ★ 11-11 M0 조각 4
    uint64_t _sendDelayLast[CSendDelayTotals::kSlots + 1] = {}, _sendSamplesLast = 0;   // ★ 11-11 M0 조각 4 — `[보내기분포]` 앞 값
    unsigned long long _leaveLast = 0, _overflowLast = 0;
    unsigned long long _leaveTimeoutLast = 0, _leaveCloseLast = 0, _eventsDroppedLast = 0, _requestsDroppedLast = 0;   // ★ 11-11 M0 조각 3
};

}  // namespace nox
