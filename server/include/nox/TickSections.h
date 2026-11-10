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
};
inline CNetworkSectionTotals g_networkSections;

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

// `[구간]` 줄(초당) · `[틱분포]` 줄(10초마다). `--quiet` 와 상관없이 찍는다. 게임 스레드에서만 부른다.
class CTickSectionReport {
public:
    // 핸들 — 게임 · 네트워크 · 판 기록 스레드(판 기록을 꺼서 없으면 nullptr)
    CTickSectionReport(void* gameThread, void* networkThread, void* recorderThread, uint32_t nowMs);
    void AddTick(double tickMs);
    bool Due(uint32_t nowMs) const { return static_cast<uint32_t>(nowMs - _lastPrintMs) >= kPrintEveryMs; }
    void Print(uint32_t nowMs, unsigned long long reliableOverflowTotal);
    // 끝날 때 — 10초를 못 채운 나머지 틱까지 찍는다(분포 총합 = 틱 수가 실행 전체에서 맞게). 스레드를 join 하면 핸들이 닫히므로 그 전에 부른다
    void PrintFinal(uint32_t nowMs, unsigned long long reliableOverflowTotal);

private:
    static constexpr uint32_t kPrintEveryMs = 1000;
    static constexpr uint32_t kLinesPerHistogram = 10;
    static constexpr size_t kHistogramSlots = 100;   // 1ms 칸 0~99 + 「100이상」 한 칸 — 33ms 예산의 세 배면 넘긴 것을 가르기에 넉넉하다

    void PrintHistogram(uint32_t nowMs);

    void* _threads[3];
    double _cpuMsLast[3] = {};
    uint32_t _startMs;
    uint32_t _lastPrintMs;
    uint32_t _linesSinceHistogram = 0;

    unsigned long long _ticks = 0;
    double _tickSumMs = 0.0, _tickMaxMs = 0.0;
    unsigned long long _histogram[kHistogramSlots + 1] = {};
    unsigned long long _histogramTicks = 0;

    uint64_t _resendNsLast = 0, _sendNsLast = 0, _recvNsLast = 0, _recvCountLast = 0, _handoffNsLast = 0;
    unsigned long long _leaveLast = 0, _overflowLast = 0;
};

}  // namespace nox
