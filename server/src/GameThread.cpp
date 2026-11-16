// GameThread.cpp — 게임 스레드 하나의 고리(머리 주석은 `nox/GameThread.h`). ★ 11-17 C-3d-2 — 옛 `main.cpp` 게임 루프를 줄 · 차례 그대로 옮겼다
#include "nox/GameThread.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

#include "nox/Clock.h"
#include "nox/MatchRecorder.h"
#include "nox/Opcode.h"
#include "nox/TickSections.h"

namespace nox {

CGameThread::CGameThread(CNetworkThread& net, CRoomManager& rooms, Config cfg, Hooks hooks)
    : _net(net), _rooms(rooms), _cfg(std::move(cfg)), _hooks(std::move(hooks)) {}

CGameThread::~CGameThread() {
    if (_tickTimer) CloseHandle(static_cast<HANDLE>(_tickTimer));
}

// 방 이벤트 꺼내기 — 판 결과는 출력 한 줄 + 기록기. 틱 뒤마다 · 끝낼 때 한 번(꺼내지 않으면 판마다 쌓이기만 했다 — 10-19 전)
void CGameThread::DrainRoomEvents() {
    _rooms.SwapOutRoomEvents(_roomEvents);
    for (const RoomEvent& e : _roomEvents) {
        switch (e.type) {
            case RoomEvent::Type::ROUND_OVER:
                break;   // 판 결과와 따로 난다(대기 중 판 끝 · 시험 손잡이 포함) — 지금 받는 쪽 없음
            case RoomEvent::Type::MATCH_RESULT:
                ++_matchResults;
                std::printf("%s\n", FormatMatchResultLine(e.result).c_str());
                std::fflush(stdout);
                if (_cfg.recorder) _cfg.recorder->Push(e.result);
                // ★ 10-20 v2 작업 4b — 로비에도(연결 줄 RESULT · 7단계에서 로비가 DB 에)
                //   ★ 7단계 조각 3 — 로비의 SAVED 까지 쥔다(연결이 없으면 붙어 등록할 때 · 판 번호로 짝)
                //   ★ 매칭 손보기 1 — 방 번호도(SAVED 꼬리의 점수 알림을 그 방 사람에게)
                if (_hooks.matchResult) _hooks.matchResult(e, _lobbyOut);
                break;
        }
    }
    _roomEvents.clear();
    _net.PushRequests(_cfg.index, _lobbyOut);   // 빈 것이면 아무것도 안 한다
}

void CGameThread::HandleEvents() {
    const SectionClock::time_point secEvents = _sectionReport ? SectionClock::now() : SectionClock::time_point{};   // ★ 11-10 부하 실험 — 사건 처리(방 만들기 포함)
    const size_t secEventCount = _netEvents.size();
    const uint32_t nowEv = NowMs();   // 넣은 시각(`queuedMs`)과 같은 시계(`Clock.h`)
    for (const NetworkEvent& ev : _netEvents) {
        const uint32_t lat = nowEv - ev.queuedMs;
        ++_evCount; _evLatSum += lat; if (lat > _evLatMax) _evLatMax = lat;
        if (_sectionReport) {   // ★ 11-11 부하 측정 M0 조각 3 — 넘김 지연을 `[구간]` 에도(위 셈은 `--quiet` 면 안 찍히는 줄 몫)
            g_gameSections.handoffLatSumMs += lat;
            if (lat > g_gameSections.handoffLatMaxMs) g_gameSections.handoffLatMaxMs = lat;
        }
        // ★ 10-14 작업 8d — 로비 TCP 사건은 로비로, 나머지는 방 관리자로(같은 큐로 오는 것을 여기서 나눈다)
        if (IsLobbyEvent(ev.type)) {
            if (_hooks.lobbyEvent) _hooks.lobbyEvent(ev, _lobbyOut);   // ★ 10-20 v2 작업 4b(옛 채널 안 로비 자리)
            continue;
        }
        _rooms.OnNetworkEvent(ev);
    }
    _netEvents.clear();
    _net.PushRequests(_cfg.index, _lobbyOut);   // 로비 답은 틱을 기다리지 않고 바로(빈 것이면 아무것도 안 한다)
    // ★★ **클라가 처음 보낸 오프코드를 그때그때 알린다** — 게임 오프코드 쪽(옛 받은 조각마다 찍던 줄 · ⚠ `--quiet` 여도 찍는다)
    // ★ 10-12 플랜 B 작업 7c — 방마다 본 것을 모아 프로세스 몫 한 줄로(옛 꼴은 방 0 것만 읽었다). `told` 는 루프 밖(옛 `static`)
    if (!g_noFirstSeen) {   // ★ 11-10 부하 실험 — `--no-first-seen`(더미 플랜 5-2절)
        _rooms.ForEachRoom([&](const CRoom& room) {
            for (const auto& kv : room.Game().seen()) {
                if (_told.count(kv.first)) continue;
                _told[kv.first] = true;
                std::printf("[처음] 클라가 보낸 새 오프코드 0x%02x (%s)\n",
                            kv.first, OpName(static_cast<Op>(kv.first)));
                std::fflush(stdout);
            }
        });
    }
    if (_sectionReport) {
        g_gameSections.eventMs += MsSince(secEvents);
        g_gameSections.eventCount += secEventCount;
    }
}

void CGameThread::Run() {
    // ★ 10-09 플랜 A 5d 자가검증 ② — 틱 마감 타이머. 정밀 타이머(윈도 10 1803~)라야 마감에 1ms 안으로 깬다 —
    //   보통 타이머 · `Sleep` 은 윈도 기본 간격(약 15.6ms) 단위로 깬다(10-09 실측 — `Sleep(1)` 이 중앙 15.5ms를 잤다)
    HANDLE tickTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tickTimer) {
        tickTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        std::printf("[루프] 정밀 타이머를 못 만들어 보통 타이머로 기다린다 — 틱 간격이 약 15.6ms 단위로 흔들린다\n");
        std::fflush(stdout);
    }
    _tickTimer = tickTimer;
    _procLastMs = NowMs();
    // ★ 11-10 부하 실험 — 구간 시간 출력(`--tick-sections` 일 때만 만든다). `net.Start()` · 기록기 `Start()` 뒤라 스레드가 있다
    //   ★ 11-17 C-3d-2 — 이 게임 스레드에서 만든다(`GetCurrentThread` 는 부른 스레드 · 첫 CPU 값도 이 스레드 것)
    if (_cfg.tickSections)
        _sectionReport = std::make_unique<CTickSectionReport>(GetCurrentThread(), _net.ThreadHandle(), _cfg.recorderThread,
                                                              _cfg.coordinatorThread, _cfg.index, NowMs());

    // ★ 10-09 조각 5d-2b — 게임 스레드. 쌓인 이벤트를 통째로 받아 처리하고(받은 조각 · 접속 · 닫기 · 접속 시도 · 시간 초과),
    //   틱을 돌리고, 그 사이 쌓인 통신 요청을 통째로 넘긴다. 옛 이 자리는 `select`(5ms) · `recvfrom` 으로 직접 받았다.
    //   받은 조각마다 찍던 `[받음]` · 열쇠 진단과 접속 제어 쪽 `[처음]` 은 네트워크 스레드가 찍는다.
    //   ★ 10-09 플랜 A 5d 자가검증 ② — 한 바퀴 끝에 「이벤트가 들어옴」(`EventsReadyHandle`) · 「다음 틱 마감」(`tickTimer`) 중
    //   먼저 오는 것을 기다린다. 닫기 신호는 늦어도 다음 틱(33ms)에 맨 위에서 본다.
    for (;;) {
        if (_cfg.stopRequested && _cfg.stopRequested()) break;   // ★ 5d-2c — 닫기 신호
        ++_loopTurns;
        _net.SwapOutNetworkEvents(_cfg.index, _netEvents);
        if (!_netEvents.empty()) HandleEvents();
        if (_hooks.turnStart) _hooks.turnStart();   // ★ 11-17 C-3d-4 — 조정 스레드 명령(새 방 · 계정 명령 · 점수 알림)

        // ★ 10-12 플랜 B 작업 7c — 판 차리기는 방마다(`StartModeIfReady` — 옛 이 자리의 블록을 옮김)
        _rooms.ForEachRoom([&](CRoom& room) { room.StartModeIfReady(); });

        // 틱 앞 — 받은 조각에 대한 답 · 판 차리기가 쌓은 요청을 먼저 넘긴다
        _rooms.SwapOutNetworkRequests(_netRequests);
        _net.PushRequests(_cfg.index, _netRequests);
        const int ticks = _rooms.BeginTick(NowMs());   // ★ 10-09 자가검증 ② — 옛 `GetTickCount`(15.6ms 단위) 대신 `Clock.h`
        _rooms.ForEachRoom([&](CRoom& room) {   // ★ 10-12 작업 7c — 방마다(그 방의 설정 · 그 방의 프레임)
            const RoomSettings& rs = room.Settings();
            if (rs.debugCrashAfter > 0 && room.Game().frame() >= static_cast<uint32_t>(rs.debugCrashAfter)) {
                room.Game().Log("[진단스위치] --debug-crash — 프레임 %u 에서 일부러 죽는다\n", room.Game().frame());
                std::fflush(stdout);
                if (_hooks.debugCrash) _hooks.debugCrash();   // ★ 11-17 C-3d-2 — 옛 `CRASH("--debug-crash")`(실행 파일 몫 · 머리 주석)
            }
        });
        if (ticks > 0) {
            _nTicks += static_cast<unsigned long long>(ticks);
            const auto tickStart = std::chrono::steady_clock::now();
            if (_prevTickStart != std::chrono::steady_clock::time_point{}) {
                const double gap = std::chrono::duration<double, std::milli>(tickStart - _prevTickStart).count();
                if (_tickGaps == 0 || gap < _tickGapMin) _tickGapMin = gap;
                if (gap > _tickGapMax) _tickGapMax = gap;
                _tickGapSum += gap; ++_tickGaps;
            }
            _prevTickStart = tickStart;
            // ★ 10-12 작업 7c — 틱 앞의 방마다 보내기 요청 누계(틱 뒤와 견줘 이 틱에 그 방이 낸 수)
            _rooms.ForEachRoom([&](CRoom& room) { room.MarkSendSeen(); });
            _rooms.RunTick();
            const double tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();
            _tickMsSum += tickMs; ++_tickRuns; if (tickMs > _tickMsMax) _tickMsMax = tickMs;
            if (_sectionReport) _sectionReport->AddTick(tickMs);
            // 그 틱의 통신 요청을 한 묶음으로 넘긴다. `made` = 그 틱의 보내기 요청 수(옛 「나간 조각 수」 —
            //   다시 보내기는 이제 네트워크 스레드 몫이라 빠진다). 초당 줄은 전처럼 이것이 0 이 아닐 때만 찍는다
            _rooms.SwapOutNetworkRequests(_netRequests);
            size_t made = 0;
            for (const NetworkRequest& r : _netRequests) if (r.type == NetworkRequest::Type::SEND_MSG) ++made;
            _net.PushRequests(_cfg.index, _netRequests);
            DrainRoomEvents();   // ★ 10-19 MO 5단계 작업 3 — 판 결과(판 끝 · 중단)는 `RunTick` 안에서만 생긴다
            // ★ 10-12 플랜 B 작업 7c — 방마다 진단(`RoomTickDiagnostics` — 옛 이 자리의 블록을 옮김)
            _rooms.ForEachRoom([&](CRoom& room) { room.TickDiagnostics(room.SendsSinceMark()); });
            // ★ 10-12 플랜 B 작업 7c — 프로세스 몫 초당 줄(통신 층 · 스레드 지표). 방 줄과 따로 1초마다 — 방마다 프레임이 달라
            //   방의 문턱(`frame % 30`)에 묶을 수 없다. 그 1초 동안 보낸 것이 있을 때만(옛 문턱 `made > 0` 과 같은 뜻)
            _procMade += made;
            if (_cfg.processLines && !_cfg.quiet && _procMade > 0 && NowMs() - _procLastMs >= 1000) {
                _procLastMs = NowMs();
                _procMade = 0;
                PrintProcessLines();
            }
        }
        if (_sectionReport && _sectionReport->Due(NowMs()))   // ★ 11-10 부하 실험 — 방이 없어도 찍는다(빈 서버 기준선)
            _sectionReport->Print(NowMs(), SectionNet(), SectionRooms());
        // ★ 10-20 v2 작업 4b · 4c — 방 사실이 바뀐 방만 로비에(인원 · 방장 · 판 상태 · 맵 · 빈 방 닫힘). 틱 블록 밖 — 바퀴마다.
        //   틱 블록 안에 두었더니 마지막 방이 닫힌 뒤 틱이 안 돌아(`BeginTick` 은 방이 없으면 0) 닫힘이 로비에 안 갔다(10-20 lobby_smoke 실물)
        if (_hooks.afterTurn) {
            _hooks.afterTurn(_lobbyOut);
            _net.PushRequests(_cfg.index, _lobbyOut);
        }
        Wait();
    }
}

void CGameThread::PrintProcessLines() {
    // ★ 10-09 조각 5d-2a — 통신 층 지표는 사본으로 찍는다(5d-2 부터 네트워크 스레드 값이라 직접 읽지 않는다)
    const CUdpStats ns = _net.LatestNetworkStats();
    // ★ 10-12 작업 7c — 옛 `[틱]` 줄의 통신 층 칸(신뢰받음 · 되받기 · 누적 틱)을 옮겨 왔다. 스트림별 셋은 아래 `[순번·스트림]` 이 전부 찍는다
    // ★ 09-21 — 되받기는 「몇 번 실었나」보다 **값이 올라가나**가 뜻이 있다.
    //   옛 「다름」은 `ack`(클라 순번+1)과 `q.last`(우리 스트림0 순번)를 견주던
    //   숫자라 거의 늘 달랐다 — 결함이 아닌데 결함처럼 읽혔다(09-21 정정).
    //   그리고 **스트림별로 갈라** 찍는다 — 되받기는 같은 스트림에 실려야 뜻이 있다
    std::printf("[통신] 신뢰받음 %llu · 되받기 %llu[값바뀜 %llu·마지막 %u] · 누적 틱 %llu\n",
                ns.reliableHeard, ns.ackSeqApplied, ns.ackSeqChanged, static_cast<unsigned>(ns.ackLastSent), _nTicks);
    // ★ 10-08 조각 3 — 접속 제어 조각의 버림은 통신 층이 센다(게임 몫은 방 줄 `[버림]`)
    if (!ns.stoppedByOp.empty()) {
        char b[128];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다
        std::snprintf(b, sizeof(b), "[버림·통신] 조각 %llu개 —", ns.stopped);
        std::string line = b;
        for (const auto& kv : ns.stoppedByOp) {
            std::snprintf(b, sizeof(b), " %02x(%s)x%llu", kv.first,
                          OpName(static_cast<Op>(kv.first)), kv.second);
            line += b;
        }
        std::printf("%s\n", line.c_str());
    }
    // ★★ B-1b — **신뢰 순번을 스트림별로 갈라 찍는다** (2026-09-18).
    //   합계만 보면 「클라가 보낸 스트림」과 「서버가 답한 스트림」이 어긋나도
    //   드러나지 않는다. 되받기는 **같은 스트림에 실려야** 뜻이 있다
    //   (StreamSeq 가 스트림마다 따로다 — `Server::Emit`).
    //   읽는 법 — 클라가 신뢰를 보낸 스트림에 서버 **비신뢰** 답이 없으면
    //   되받기를 실을 자리가 아예 없다는 뜻이다.
    {
        std::string sl = "[순번·스트림] 받은신뢰";
        char sb[48];
        for (uint8_t st = 0; st < 8; ++st) {
            if (!ns.recvRelStream[st]) continue;
            std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.recvRelStream[st]);
            sl += sb;
        }
        sl += " · 보낸신뢰";
        for (uint8_t st = 0; st < 8; ++st) {
            if (!ns.sendRelStream[st]) continue;
            std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.sendRelStream[st]);
            sl += sb;
        }
        sl += " · 보낸비신뢰";
        for (uint8_t st = 0; st < 8; ++st) {
            if (!ns.sendPlainStream[st]) continue;
            std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.sendPlainStream[st]);
            sl += sb;
        }
        // ★ 되받기로 **어떤 값**을 실었나. 값이 한두 개뿐이면 `recvSeq` 가 안 올라가는 것이다
        sl += " · 되받기값";
        bool any = false;
        for (uint8_t v = 0; v < 32; ++v) {
            if (!ns.ackValueHist[v]) continue;
            std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)v, ns.ackValueHist[v]);
            sl += sb;
            any = true;
        }
        if (!any) sl += " (없음)";
        std::printf("%s\n", sl.c_str());
    }
    // ★ 10-08 조각 3b — 신뢰 패킷 다시 보내기. 「다시 보냄」만 있고 「확인으로 지움」이 0 이면
    //   클라의 확인 순번을 못 읽는 것이다(UdpServer.cpp Ack)
    // ★ 10-09 조각 5b — 「타이머」는 힙에 든 항목 수(꺼낼 때 버릴 항목 포함). 세션 수 + 확인 기다리는 조각 수 근처에서
    //   머물러야 한다 — 버릴 항목은 제 마감(2초 · 한도 시간)에 빠진다. 계속 불어나면 버릴 항목이 안 빠지는 것이다
    std::printf("[다시보냄] 다시 보냄 %llu · 확인으로 지움 %llu · 바로 다시 보낼 표시 %llu · 한도 넘쳐 끊음 %llu · 대기 %zu · 타이머 %zu\n",
                ns.reliableResent, ns.reliableAcked,
                ns.reliableRetryMarked, ns.reliableQueueOverflow,
                ns.reliablePending, ns.timerCount);
    // ★ 10-08 조각 4 — 지운 세션 번호로 보내려다 버린 수. 0 이 아니면 게임이 지운 사람에게 아직 보내고 있다
    std::printf("[세션] 없는 번호로 보내려다 버림 %llu · 접속 전 상대 걷어냄 %llu · 시간 초과로 넘김 %llu · 입장 요청 %llu · 거절 %llu · 답할 때 없음 %llu\n",
                ns.sendNoSession, ns.unannouncedSwept,
                ns.sessionTimedOut,    // ★ 10-09 조각 5a — 통신 층이 시간 초과로 게임에 넘긴 세션 수
                ns.joinRequested, ns.joinRejected,
                ns.joinRespondNoSession);   // ★ 10-09 조각 5c — 접속 시도를 게임에 물은 수 · 거절 · 답할 때 세션이 없던 수
    // ★ 10-09 조각 5d-2b — 스레드 둘의 지표(이 1초 동안). 「큐 버림」은 0 이 정상 · 넘김 지연은 틱 하나(33ms) 안이 정상
    {
        const CNetworkThreadStats ts = _net.LatestThreadStats();
        std::printf("[스레드] 이벤트 넘김 지연 평균 %.1fms · 최대 %ums(%llu건) · 틱 시간 평균 %.2fms · 최대 %.2fms(%llu틱)"
                    " · 틱 간격 최소 %.1fms · 평균 %.1fms · 최대 %.1fms(%llu) · 게임 루프 %llu바퀴"
                    " · 네트워크 깨어남 %llu · 받음 %llu · 보냄 %llu · 한 번에 비운 최대 %llu · 받기 오류 %llu"
                    " · 큐 버림(이벤트 %llu · 요청 %llu) · 큐 남음(이벤트 %zu · 요청 %zu)\n",
                    _evCount ? static_cast<double>(_evLatSum) / static_cast<double>(_evCount) : 0.0, _evLatMax, _evCount,
                    _tickRuns ? _tickMsSum / static_cast<double>(_tickRuns) : 0.0, _tickMsMax, _tickRuns,
                    _tickGapMin, _tickGaps ? _tickGapSum / static_cast<double>(_tickGaps) : 0.0, _tickGapMax, _tickGaps, _loopTurns,
                    ts.wakes, ts.datagramsIn, ts.datagramsOut, ts.maxDatagramsPerWake, ts.recvErrors,
                    ts.eventsDropped, ts.requestsDropped, ts.eventsQueued, ts.requestsQueued);
        _evCount = _evLatSum = _tickRuns = 0; _evLatMax = 0; _tickMsSum = _tickMsMax = 0.0;
        _tickGaps = 0; _tickGapSum = _tickGapMin = _tickGapMax = 0.0; _loopTurns = 0;
    }
    std::fflush(stdout);
}

void CGameThread::Wait() {
    // ★ 10-09 플랜 A 5d 자가검증 ② — 기다림. 옛 `Sleep(1)`(5d-2b — 실제로는 약 15.5ms 잠)은 받은 것을 평균 8ms 늦게 처리했고
    //   틱 간격이 16~48ms로 흔들렸다(10-09 실측). 마감이 이미 지났으면 기다리지 않고 바로 다음 바퀴(밀린 틱을 몰아서 돈다)
    const HANDLE tickTimer = static_cast<HANDLE>(_tickTimer);
    uint32_t tickDue = 0;
    if (_rooms.NextTickDue(tickDue)) {
        const int32_t left = static_cast<int32_t>(tickDue - NowMs());
        if (left > 0) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>(left) * 10000;   // 100ns 단위 · 음수 = 지금부터
            // ★ 11-17 C-3d-4 — 명령 큐 이벤트가 있으면 가운데에 끼운다(사건 · 명령 · 틱 마감 — 틱 마감이 늘 끝)
            const HANDLE cmd = static_cast<HANDLE>(_cfg.commandsReady);
            const HANDLE waits[3] = { static_cast<HANDLE>(_net.EventsReadyHandle(_cfg.index)), cmd ? cmd : tickTimer, tickTimer };
            const DWORD nWaits = cmd ? 3 : 2;
            // ★ 11-11 부하 측정 M0 조각 3 — 기다린 시간(바쁜 비율 · `TickSections.h` `CGameSectionTotals::waitMs`). 이 두 자리와 아래 방 없을 때 한 자리
            const SectionClock::time_point waitStart = _sectionReport ? SectionClock::now() : SectionClock::time_point{};
            if (tickTimer && SetWaitableTimer(tickTimer, &rel, 0, nullptr, nullptr, FALSE))
                WaitForMultipleObjects(nWaits, waits, FALSE, INFINITE);
            else
                WaitForMultipleObjects(cmd ? 2 : 1, waits, FALSE, static_cast<DWORD>(left));   // 타이머가 없으면 이벤트(· 명령)만 시간 한도로(헛돌지 않게)
            if (_sectionReport) g_gameSections.waitMs += MsSince(waitStart);
        }
    } else {
        // ★ 10-20 v2 작업 4c — 방이 하나도 없으면 틱 마감이 없다 — 이벤트(로비 줄 · 접속 시도)나 한 틱 길이까지 기다린다(닫기 신호도 그 안에).
        //   옛 꼴은 기다리지 않고 헛돌았다(10-20 실측 — 방 없는 게임 서버가 5초에 CPU 4.8초). 로비 방식은 방 없이 시작해 늘 이 꼴로 쉰다
        constexpr DWORD kIdleWaitMs = 33;   // 한 틱(30Hz)
        const SectionClock::time_point waitStart = _sectionReport ? SectionClock::now() : SectionClock::time_point{};   // ★ 11-11 M0 조각 3
        if (const HANDLE cmd = static_cast<HANDLE>(_cfg.commandsReady)) {   // ★ 11-17 C-3d-4 — 방이 없을 때도 명령(새 방)에 깬다
            const HANDLE waits[2] = { static_cast<HANDLE>(_net.EventsReadyHandle(_cfg.index)), cmd };
            WaitForMultipleObjects(2, waits, FALSE, kIdleWaitMs);
        } else {
            WaitForSingleObject(static_cast<HANDLE>(_net.EventsReadyHandle(_cfg.index)), kIdleWaitMs);
        }
        if (_sectionReport) g_gameSections.waitMs += MsSince(waitStart);
    }
}

// ★ 11-11 부하 측정 M0 조각 3 — `[구간]` 에 넘길 통신 층 누계(넘침 · 큐 버림)
CSectionNetTotals CGameThread::SectionNet() const {
    CSectionNetTotals t;
    t.reliableOverflow = _net.LatestNetworkStats().reliableQueueOverflow;
    const CNetworkThreadStats ts = _net.LatestThreadStats();
    t.eventsDropped = ts.eventsDropped;
    t.requestsDropped = ts.requestsDropped;
    return t;
}

// ★ 11-17 C-3a — `[구간]` 줄 끝의 방 수 · 덜 참(C 플랜 7절)
CSectionRoomTotals CGameThread::SectionRooms() const {
    CSectionRoomTotals t;
    t.rooms = _rooms.RoomCount();
    t.underfilled = _rooms.LoadRoomsUnderfilled();
    return t;
}

void CGameThread::Finish() {
    if (_sectionReport) _sectionReport->PrintFinal(NowMs(), SectionNet(), SectionRooms());   // `net.Stop` 이 스레드 핸들을 닫기 전에
    _totals.seen = _rooms.SeenTotals();
    _totals.inputs = g_inputTotal;
    _totals.aliasTaken = g_aliasTaken;
    _totals.aliasStaleDropped = g_aliasStaleDropped;
}

void CGameThread::FlushRequests() {
    _rooms.SwapOutNetworkRequests(_netRequests);
    _net.PushRequests(_cfg.index, _netRequests);
}

}  // namespace nox
