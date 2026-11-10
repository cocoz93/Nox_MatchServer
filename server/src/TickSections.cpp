// ★ 11-10 부하 실험 — 구간 시간 출력(머리 주석은 `TickSections.h`)
#include "nox/TickSections.h"

#include <cstdio>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nox {

namespace {

double NsToMs(uint64_t ns) { return static_cast<double>(ns) / 1000000.0; }

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

CTickSectionReport::CTickSectionReport(void* gameThread, void* networkThread, void* recorderThread, uint32_t nowMs)
    : _threads{gameThread, networkThread, recorderThread}, _startMs(nowMs), _lastPrintMs(nowMs) {
    for (int i = 0; i < 3; ++i) _cpuMsLast[i] = ThreadCpuMs(_threads[i]);   // 첫 줄에 시작 비용이 안 들어가게
}

void CTickSectionReport::AddTick(double tickMs) {
    ++_ticks;
    _tickSumMs += tickMs;
    if (tickMs > _tickMaxMs) _tickMaxMs = tickMs;
    const size_t slot = tickMs >= static_cast<double>(kHistogramSlots) ? kHistogramSlots : static_cast<size_t>(tickMs);
    ++_histogram[slot];
    ++_histogramTicks;
}

void CTickSectionReport::Print(uint32_t nowMs, unsigned long long reliableOverflowTotal) {
    _lastPrintMs = nowMs;
    const CGameSectionTotals& g = g_gameSections;
    const auto sec = [&g](TickSection s) { return g.sectionMs[static_cast<size_t>(s)]; };

    const uint64_t resend = g_networkSections.resendNs.load(std::memory_order_relaxed);
    const uint64_t send = g_networkSections.sendNs.load(std::memory_order_relaxed);
    const uint64_t recv = g_networkSections.recvNs.load(std::memory_order_relaxed);
    const uint64_t recvCount = g_networkSections.recvCount.load(std::memory_order_relaxed);
    const uint64_t handoff = g_networkSections.handoffNs.load(std::memory_order_relaxed);

    double cpuNow[3];
    for (int i = 0; i < 3; ++i) cpuNow[i] = ThreadCpuMs(_threads[i]);

    const unsigned long long overflowTotal = g_importantOverflowTotal + reliableOverflowTotal;
    std::printf("[구간] 초 %u · 틱 %llu · 틱합 %.1f · 틱최대 %.1f · 사건 %.1f(수 %llu)"
                " · 방 만들기 %.1f(수 %llu · 최대 %.1f) · 방 닫기 %.1f(수 %llu · 최대 %.1f) · 뒷일 %.1f"
                " · 앞 %.1f · 걸음 %.1f · 생각 %.1f · 시전 %.1f · 세계 %.1f · 자기 %.1f · 사람 %.1f · 물체 %.1f · 소환수 %.1f · 정적 %.1f · 묶음 %.1f"
                " · 통신 재전송 %.1f · 보내기 %.1f · 받기 %.1f(수 %llu) · 넘기기 %.1f"
                " · CPU 게임 %.0f 통신 %.0f 기록 %.0f · 끊김 %llu · 넘침 %llu\n",
                static_cast<uint32_t>(nowMs - _startMs) / 1000u, _ticks, _tickSumMs, _tickMaxMs, g.eventMs, g.eventCount,
                g.roomCreateMs, g.roomCreates, g.roomCreateMaxMs, g.roomCloseMs, g.roomCloses, g.roomCloseMaxMs, g.afterMs,
                sec(TickSection::Front), sec(TickSection::Walk), sec(TickSection::Think), sec(TickSection::Cast), sec(TickSection::World),
                sec(TickSection::Self), sec(TickSection::People), sec(TickSection::Objects), sec(TickSection::Creatures),
                sec(TickSection::Statics), sec(TickSection::Bundle),
                NsToMs(resend - _resendNsLast), NsToMs(send - _sendNsLast), NsToMs(recv - _recvNsLast),
                static_cast<unsigned long long>(recvCount - _recvCountLast), NsToMs(handoff - _handoffNsLast),
                cpuNow[0] - _cpuMsLast[0], cpuNow[1] - _cpuMsLast[1], cpuNow[2] - _cpuMsLast[2],
                g_leaveTotal - _leaveLast, overflowTotal - _overflowLast);
    std::fflush(stdout);

    _resendNsLast = resend; _sendNsLast = send; _recvNsLast = recv; _recvCountLast = recvCount; _handoffNsLast = handoff;
    for (int i = 0; i < 3; ++i) _cpuMsLast[i] = cpuNow[i];
    _leaveLast = g_leaveTotal;
    _overflowLast = overflowTotal;
    g_gameSections.Reset();
    _ticks = 0;
    _tickSumMs = _tickMaxMs = 0.0;

    if (++_linesSinceHistogram >= kLinesPerHistogram) PrintHistogram(nowMs);
}

void CTickSectionReport::PrintFinal(uint32_t nowMs, unsigned long long reliableOverflowTotal) {
    Print(nowMs, reliableOverflowTotal);
    if (_histogramTicks > 0) PrintHistogram(nowMs);
}

void CTickSectionReport::PrintHistogram(uint32_t nowMs) {
    _linesSinceHistogram = 0;
    char b[64];
    std::snprintf(b, sizeof(b), "[틱분포] 초 %u · 칸 1ms ·", static_cast<uint32_t>(nowMs - _startMs) / 1000u);
    std::string line = b;
    for (size_t i = 0; i < kHistogramSlots; ++i) {
        if (!_histogram[i]) continue;
        std::snprintf(b, sizeof(b), " %zu:%llu", i, _histogram[i]);
        line += b;
    }
    std::snprintf(b, sizeof(b), " %zu이상:%llu", kHistogramSlots, _histogram[kHistogramSlots]);
    line += b;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    for (unsigned long long& n : _histogram) n = 0;
    _histogramTicks = 0;
}

}  // namespace nox
