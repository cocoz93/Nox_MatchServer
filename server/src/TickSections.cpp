// ★ 11-10 부하 실험 — 구간 시간 출력(머리 주석은 `TickSections.h`)
#include "nox/TickSections.h"

#include <cstdio>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>   // ★ 11-11 M0 조각 3 — 서버 메모리(`GetProcessMemoryInfo` · 윈도 7 이후 kernel32 의 `K32` 판으로 이어진다)

namespace nox {

namespace {

double NsToMs(uint64_t ns) { return static_cast<double>(ns) / 1000000.0; }

// ★ 11-11 부하 측정 M0 조각 3 — 서버 프로세스 메모리(MB). 개인 = 이 프로세스만 쓰는 확정 메모리(오래 돌 때 느는지 볼 값) ·
//   작업 = 지금 실제 메모리에 올라 있는 몫(윈도가 덜어 낼 수 있어 늘어남 판단엔 개인을 쓴다). 못 읽으면 0
void ProcessMemoryMb(double& privateMb, double& workingMb) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        privateMb = workingMb = 0.0;
        return;
    }
    privateMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    workingMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

double ThreadCpuMs(void* thread) {
    if (!thread) return 0.0;
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(static_cast<HANDLE>(thread), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks100ns = [](const FILETIME& f) {
        return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks100ns(kernel) + ticks100ns(user)) / 10000.0;
}

}  // namespace

CTickSectionReport::CTickSectionReport(void* gameThread, void* networkThread, void* recorderThread, void* coordinatorThread, int gameIndex,
                                       uint32_t nowMs)
    : _threads{gameThread, networkThread, recorderThread, coordinatorThread}, _gameIndex(gameIndex), _startMs(nowMs), _lastPrintMs(nowMs),
      _lastPrintAt(SectionClock::now()) {
    if (_gameIndex != 0) _threads[1] = _threads[2] = _threads[3] = nullptr;   // ★ 11-17 C-3a — 통신 · 기록 · 조정 CPU 는 게임 스레드 0 줄에만(없으면 0)
    for (int i = 0; i < kThreads; ++i) _cpuMsLast[i] = ThreadCpuMs(_threads[i]);   // 첫 줄에 시작 비용이 안 들어가게
}

void CTickSectionReport::AddTick(double tickMs) {
    ++_ticks;
    _tickSumMs += tickMs;
    if (tickMs > _tickMaxMs) _tickMaxMs = tickMs;
    const size_t slot = tickMs >= static_cast<double>(kHistogramSlots) ? kHistogramSlots : static_cast<size_t>(tickMs);
    ++_histogram[slot];
    ++_histogramTicks;
}

void CTickSectionReport::Print(uint32_t nowMs, const CSectionNetTotals& net, const CSectionRoomTotals& rooms) {
    _lastPrintMs = nowMs;
    const bool netCols = _gameIndex == 0;   // ★ 11-17 C-3a — 통신 몫 칸은 게임 스레드 0 줄에만(머리 주석은 `TickSections.h`)
    // ★ 11-11 부하 측정 M0 조각 3 — 실제로 지난 ms · 게임 스레드 바쁜 비율 · 고리 나머지(측정 플랜 2-2절)
    const SectionClock::time_point at = SectionClock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(at - _lastPrintAt).count();
    _lastPrintAt = at;
    const CGameSectionTotals& g = g_gameSections;
    const double busyMs = elapsedMs > g.waitMs ? elapsedMs - g.waitMs : 0.0;
    const double busyPct = elapsedMs > 0.0 ? 100.0 * busyMs / elapsedMs : 0.0;
    const double restMs = busyMs - (_tickSumMs + g.eventMs + g.roomCloseMs);   // 틱 · 사건(방 만들기 포함) · 방 닫기 밖 — 방 전체를 도는 고리 자리 · 줄 찍기 등
    const double handoffAvgMs = g.eventCount ? static_cast<double>(g.handoffLatSumMs) / static_cast<double>(g.eventCount) : 0.0;
    double privateMb = 0.0, workingMb = 0.0;
    ProcessMemoryMb(privateMb, workingMb);
    const auto sec = [&g](TickSection s) { return g.sectionMs[static_cast<size_t>(s)]; };

    // ★ 11-17 C-3a — 게임 스레드 0 이 아니면 통신 누계를 안 읽는다(앞 값 0 그대로 · 칸은 0)
    const uint64_t resend = netCols ? g_networkSections.resendNs.load(std::memory_order_relaxed) : 0;
    const uint64_t send = netCols ? g_networkSections.sendNs.load(std::memory_order_relaxed) : 0;
    const uint64_t recv = netCols ? g_networkSections.recvNs.load(std::memory_order_relaxed) : 0;
    const uint64_t recvCount = netCols ? g_networkSections.recvCount.load(std::memory_order_relaxed) : 0;
    const uint64_t handoff = netCols ? g_networkSections.handoffNs.load(std::memory_order_relaxed) : 0;
    // ★ 11-11 부하 측정 M0 조각 4 — 통신 바쁜 비율 · 보내기 실패(머리 주석은 `TickSections.h`)
    const uint64_t netWait = netCols ? g_networkSections.waitNs.load(std::memory_order_relaxed) : 0;
    const double netWaitMs = NsToMs(netWait - _netWaitNsLast);
    const double netBusyPct = netCols && elapsedMs > 0.0 ? 100.0 * (elapsedMs > netWaitMs ? elapsedMs - netWaitMs : 0.0) / elapsedMs : 0.0;
    const uint64_t sendFail = netCols ? g_sendFail.count.load(std::memory_order_relaxed) : 0;
    const int sendFailError = netCols ? g_sendFail.lastError.load(std::memory_order_relaxed) : 0;

    double cpuNow[kThreads];
    for (int i = 0; i < kThreads; ++i) cpuNow[i] = ThreadCpuMs(_threads[i]);

    const unsigned long long overflowTotal = g_importantOverflowTotal + net.reliableOverflow;
    // ★ 11-11 M0 조각 3 — 끝에 덧붙임(앞 칸 차례는 그대로): 끊김 까닭 · 지난 · 기다림 · 바쁨 · 고리 나머지 · 넘김 지연 · 큐 버림 · 메모리
    std::printf("[구간] 초 %u · 틱 %llu · 틱합 %.1f · 틱최대 %.1f · 사건 %.1f(수 %llu)"
                " · 방 만들기 %.1f(수 %llu · 최대 %.1f) · 방 닫기 %.1f(수 %llu · 최대 %.1f) · 뒷일 %.1f"
                " · 앞 %.1f · 걸음 %.1f · 생각 %.1f · 시전 %.1f · 세계 %.1f · 자기 %.1f · 사람 %.1f · 물체 %.1f · 소환수 %.1f · 정적 %.1f · 묶음 %.1f"
                " · 통신 재전송 %.1f · 보내기 %.1f · 받기 %.1f(수 %llu) · 넘기기 %.1f"
                " · CPU 게임 %.0f 통신 %.0f 기록 %.0f · 끊김 %llu(시간 초과 %llu · 0a %llu) · 넘침 %llu"
                " · 지난 %.1f · 기다림 %.1f · 바쁨 %.1f%% · 고리 나머지 %.1f · 넘김 지연 평균 %.2f 최대 %u"
                " · 큐 버림 사건 %llu 요청 %llu · 메모리 개인 %.1fMB 작업 %.1fMB"
                " · 통신 기다림 %.1f 바쁨 %.1f%% · 보내기 실패 %llu(마지막 오류 %d)"
                " · CPU 조정 %.0f · 게임 스레드 %d · 방 %zu(덜 참 %zu)\n",
                static_cast<uint32_t>(nowMs - _startMs) / 1000u, _ticks, _tickSumMs, _tickMaxMs, g.eventMs, g.eventCount,
                g.roomCreateMs, g.roomCreates, g.roomCreateMaxMs, g.roomCloseMs, g.roomCloses, g.roomCloseMaxMs, g.afterMs,
                sec(TickSection::Front), sec(TickSection::Walk), sec(TickSection::Think), sec(TickSection::Cast), sec(TickSection::World),
                sec(TickSection::Self), sec(TickSection::People), sec(TickSection::Objects), sec(TickSection::Creatures),
                sec(TickSection::Statics), sec(TickSection::Bundle),
                NsToMs(resend - _resendNsLast), NsToMs(send - _sendNsLast), NsToMs(recv - _recvNsLast),
                static_cast<unsigned long long>(recvCount - _recvCountLast), NsToMs(handoff - _handoffNsLast),
                cpuNow[0] - _cpuMsLast[0], cpuNow[1] - _cpuMsLast[1], cpuNow[2] - _cpuMsLast[2],
                g_leaveTotal - _leaveLast, g_leaveTimeoutTotal - _leaveTimeoutLast, g_leaveCloseTotal - _leaveCloseLast,
                overflowTotal - _overflowLast,
                elapsedMs, g.waitMs, busyPct, restMs, handoffAvgMs, g.handoffLatMaxMs,
                net.eventsDropped - _eventsDroppedLast, net.requestsDropped - _requestsDroppedLast, privateMb, workingMb,
                netWaitMs, netBusyPct, static_cast<unsigned long long>(sendFail - _sendFailLast), sendFailError,
                cpuNow[3] - _cpuMsLast[3], _gameIndex, rooms.rooms, rooms.underfilled);
    std::fflush(stdout);
    _netWaitNsLast = netWait;
    _sendFailLast = sendFail;

    _resendNsLast = resend; _sendNsLast = send; _recvNsLast = recv; _recvCountLast = recvCount; _handoffNsLast = handoff;
    for (int i = 0; i < kThreads; ++i) _cpuMsLast[i] = cpuNow[i];
    _leaveLast = g_leaveTotal;
    _leaveTimeoutLast = g_leaveTimeoutTotal;
    _leaveCloseLast = g_leaveCloseTotal;
    _eventsDroppedLast = net.eventsDropped;
    _requestsDroppedLast = net.requestsDropped;
    _overflowLast = overflowTotal;
    g_gameSections.Reset();
    _ticks = 0;
    _tickSumMs = _tickMaxMs = 0.0;

    if (++_linesSinceHistogram >= kLinesPerHistogram) PrintHistogram(nowMs);
}

void CTickSectionReport::PrintFinal(uint32_t nowMs, const CSectionNetTotals& net, const CSectionRoomTotals& rooms) {
    Print(nowMs, net, rooms);
    uint64_t sendSamples = 0;   // ★ 11-11 M0 조각 4 — 보내기 표본만 남아도(표본 = 칸의 합)
    if (_gameIndex == 0)        // ★ 11-17 C-3a — `[보내기분포]` 는 게임 스레드 0 만
        for (const std::atomic<uint64_t>& n : g_sendDelay.histogram) sendSamples += n.load(std::memory_order_relaxed);
    if (_histogramTicks > 0 || g_tickCompletion.samples > 0 || sendSamples != _sendSamplesLast)   // ★ 11-11 M0 조각 2 — 완료 표본만 남아도
        PrintHistogram(nowMs);
}

void CTickSectionReport::PrintHistogram(uint32_t nowMs) {
    _linesSinceHistogram = 0;
    char b[64];
    char threadTail[32];   // ★ 11-17 C-3a — 분포 줄 셋 끝에 덧붙이는 칸(`[구간]` 줄과 같은 「끝에 덧붙임」)
    std::snprintf(threadTail, sizeof(threadTail), " · 게임 스레드 %d", _gameIndex);
    std::snprintf(b, sizeof(b), "[틱분포] 초 %u · 칸 1ms ·", static_cast<uint32_t>(nowMs - _startMs) / 1000u);
    std::string line = b;
    for (size_t i = 0; i < kHistogramSlots; ++i) {
        if (!_histogram[i]) continue;
        std::snprintf(b, sizeof(b), " %zu:%llu", i, _histogram[i]);
        line += b;
    }
    std::snprintf(b, sizeof(b), " %zu이상:%llu", kHistogramSlots, _histogram[kHistogramSlots]);
    line += b;
    line += threadTail;   // ★ 11-17 C-3a — 끝에 덧붙임
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    for (unsigned long long& n : _histogram) n = 0;
    _histogramTicks = 0;

    // ★ 11-11 부하 측정 M0 조각 2 — 틱 완료 시간 분포(머리 주석은 `TickSections.h` `CTickCompletion`). `[틱분포]` 와 같은 10초 · 같은 칸.
    //   표본 = 방별 틱 수 합 · 밀림 = 2틱 이상 몰아 돈 횟수(괄호 안은 그때 더 돈 틱 수)
    CTickCompletion& c = g_tickCompletion;
    char head[160];   // 한글 한 자 3바이트 + 숫자 넷(최대 20자리씩) — 위 `b`(64)로는 잘린다
    std::snprintf(head, sizeof(head), "[완료분포] 초 %u · 칸 1ms · 표본 %llu · 밀림 %llu(틱 %llu) ·",
                  static_cast<uint32_t>(nowMs - _startMs) / 1000u, c.samples, c.lateRuns, c.lateExtraTicks);
    line = head;
    for (size_t i = 0; i < CTickCompletion::kSlots; ++i) {
        if (!c.histogram[i]) continue;
        std::snprintf(b, sizeof(b), " %zu:%llu", i, c.histogram[i]);
        line += b;
    }
    std::snprintf(b, sizeof(b), " %zu이상:%llu", CTickCompletion::kSlots, c.histogram[CTickCompletion::kSlots]);
    line += b;
    line += threadTail;   // ★ 11-17 C-3a
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    c.Reset();
    if (_gameIndex != 0) return;   // ★ 11-17 C-3a — `[보내기분포]` 는 게임 스레드 0 만(통신 스레드 누계 — 앞 값과 빼기를 한 곳에서)

    // ★ 11-11 부하 측정 M0 조각 4 — 보내는 쪽 지연 분포(머리 주석은 `TickSections.h` `CSendDelayTotals`). 통신 스레드가 쓰는 누계를 읽어 앞 값과 뺀다
    uint64_t sendNow[CSendDelayTotals::kSlots + 1];
    uint64_t sendSamples = 0;   // 표본 = 칸의 합(머리 주석)
    for (size_t i = 0; i <= CSendDelayTotals::kSlots; ++i) {
        sendNow[i] = g_sendDelay.histogram[i].load(std::memory_order_relaxed);
        sendSamples += sendNow[i];
    }
    std::snprintf(head, sizeof(head), "[보내기분포] 초 %u · 칸 1ms · 표본 %llu ·",
                  static_cast<uint32_t>(nowMs - _startMs) / 1000u, static_cast<unsigned long long>(sendSamples - _sendSamplesLast));
    line = head;
    for (size_t i = 0; i < CSendDelayTotals::kSlots; ++i) {
        const uint64_t n = sendNow[i] - _sendDelayLast[i];
        if (!n) continue;
        std::snprintf(b, sizeof(b), " %zu:%llu", i, static_cast<unsigned long long>(n));
        line += b;
    }
    std::snprintf(b, sizeof(b), " %zu이상:%llu", CSendDelayTotals::kSlots,
                  static_cast<unsigned long long>(sendNow[CSendDelayTotals::kSlots] - _sendDelayLast[CSendDelayTotals::kSlots]));
    line += b;
    line += threadTail;   // ★ 11-17 C-3a
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    for (size_t i = 0; i <= CSendDelayTotals::kSlots; ++i) _sendDelayLast[i] = sendNow[i];
    _sendSamplesLast = sendSamples;
}

}  // namespace nox
