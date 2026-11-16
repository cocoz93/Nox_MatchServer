// net_thread_test.cpp — 네트워크 스레드(`CNetworkThread`) 시험. 로컬 UDP 소켓으로 실제로 주고받는다.
//   `proto_test.cpp` 와 따로 둔 까닭 — winsock(windows.h)의 min/max 매크로가 그쪽 시험 본문과 부딪힌다.
//   결과는 (통과, 한 줄) 목록으로 돌려주고, `proto_test.cpp` 의 `TestNetworkThread` 가 `Check` 로 센다.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nox/Clock.h"
#include "nox/JoinToken.h"   // ★ 11-17 C-3b — 토큰 꼬리(⑩)
#include "nox/NetworkThread.h"
#include "nox/Opcode.h"

namespace {

uint32_t Now() { return nox::NowMs(); }   // 서버와 같은 시계(`Clock.h` — 10-09 자가검증 ② 전엔 `GetTickCount64`)

// 기다리는 시간 안에 데이터그램 하나를 받는다. 못 받으면 빈 것.
std::vector<uint8_t> RecvOne(SOCKET c, DWORD timeoutMs) {
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    uint8_t buf[2048];
    sockaddr_in from = {};
    int fl = sizeof(from);
    const int n = recvfrom(c, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) return {};
    return std::vector<uint8_t>(buf, buf + n);
}

bool IsAccept(const std::vector<uint8_t>& d) {   // 접속 수락 — 머리 2 + `1f 00 01 번호4 키` (키 0 이라 평문)
    return d.size() == 10 && d[0] == 0x80 && d[2] == static_cast<uint8_t>(nox::Op::ACCEPTED);
}

}  // namespace

// ① 접속 → 접속 수락이 소켓으로 돌아오고 CONNECTED 이벤트가 큐로 온다(넣은 시각이 찍혀 · 게임 루프를 깨우는 신호가 켜져)
// ② 게임 요청(보내기)을 넣으면 깨어나 곧 보낸다 — 기다림 최대를 5초로 두었으니 깨우기가 없으면 다음 타이머(2초)까지 늦는다
// ③ 확인이 없으면 접속 수락을 보낸 지 2초에 다시 받는다 — 타이머 마감에 깨어난다(깨지 않으면 5초)
// ④ `Stop` — 스레드가 끝나고 두 번 불러도 된다
// ⑤ 닫기 — 넣자마자 멈춘 요청도 나가고 `SERVER_CLOSE` 가 간다 ⑥ 큐 한도 — 넘친 요청·이벤트는 버리고 센다
// ★ 11-17 C-2 — `rio` = UDP 부품의 RIO 팔(거짓이면 보통 팔). 같은 시험을 팔마다 한 번씩(`proto_test` `TestNetworkThread`) ·
//   ⑨ 몰아 보내기 · 칸보다 큰 조각(두 팔 다 — 받기 고리가 받을 것을 끝까지 비우나 · 부품 셈이 `recvErrors` 로 오나 · 팔이 실제로 바뀌었나)
std::vector<std::pair<bool, std::string>> RunNetworkThreadChecks(bool rio) {
    std::vector<std::pair<bool, std::string>> r;
    char m[240];
    nox::CNetworkThread net;
    net.SetUdpOptions(rio, 0, 0);
    net.Network().SetXorKey(0);
    net.SetMaxWaitMs(5000);
    std::string err;
    if (!net.Open("127.0.0.1", 0, err)) { r.push_back({ false, "열기 실패 — " + err }); return r; }
    net.Start();

    const SOCKET c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
    bind(c, reinterpret_cast<sockaddr*>(&me), sizeof(me));
    sockaddr_in to = me;
    to.sin_port = htons(net.Port());
    const uint8_t conn[3] = { 0xff, 0x00, 0x00 };
    sendto(c, reinterpret_cast<const char*>(conn), sizeof(conn), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    const std::vector<uint8_t> acc = RecvOne(c, 1000);
    const uint32_t acceptAt = Now();
    // ★ 10-09 플랜 A 5d 자가검증 ② — 이벤트를 넣으면 게임 루프를 깨우는 신호가 켜진다(게임 루프는 이것과 틱 마감을 기다린다)
    const bool readySignaled = WaitForSingleObject(static_cast<HANDLE>(net.EventsReadyHandle()), 1000) == WAIT_OBJECT_0;
    int64_t sid = 0;
    uint32_t queuedMs = 0;
    std::vector<nox::NetworkEvent> evs;
    for (int i = 0; i < 100 && sid == 0; ++i) {
        net.SwapOutNetworkEvents(evs);
        for (const auto& e : evs)
            if (e.type == nox::NetworkEvent::Type::CONNECTED) { sid = e.sessionId; queuedMs = e.queuedMs; }
        evs.clear();
        if (sid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::snprintf(m, sizeof(m), "① 스레드 — 접속 수락 받음 %d · CONNECTED 이벤트 세션 %lld · 넣은 시각 찍힘 %d · 깨우는 신호 %d",
                  (int)IsAccept(acc), (long long)sid, (int)(queuedMs != 0), (int)readySignaled);
    r.push_back({ IsAccept(acc) && sid != 0 && queuedMs != 0 && readySignaled, m });

    // ② 접속 수락 뒤 0.3초에 보내기 요청 — 다음 타이머(다시 보내기)는 1.7초 뒤라 깨우기가 없으면 그때까지 안 나간다
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::vector<nox::NetworkRequest> reqs(1);
    reqs[0].type = nox::NetworkRequest::Type::SEND_MSG;
    reqs[0].sessionId = sid;
    reqs[0].body = { 0x27, 1, 2 };
    const uint32_t pushAt = Now();
    net.PushRequests(reqs);
    std::vector<uint8_t> got;
    for (int i = 0; i < 5 && !(got.size() == 5 && got[2] == 0x27); ++i) got = RecvOne(c, 3000);
    const uint32_t sendLat = Now() - pushAt;
    std::snprintf(m, sizeof(m), "② 요청 넣고 받기까지 %ums(100ms 안이어야 — 깨우기) · 요청 그릇 비움 %d", sendLat, (int)reqs.empty());
    r.push_back({ got.size() == 5 && got[2] == 0x27 && sendLat < 100 && reqs.empty(), m });

    // ③ 다시 보낸 접속 수락 — 보낸 지 2초(± 이 시험의 시계 · 틱 오차)
    std::vector<uint8_t> again;
    for (int i = 0; i < 5 && !IsAccept(again); ++i) again = RecvOne(c, 6000);
    const uint32_t gap = Now() - acceptAt;
    std::snprintf(m, sizeof(m), "③ 다시 보낸 접속 수락 %d · 첫 수락에서 %ums(1900~2200 이어야 — 타이머 마감에 깸)", (int)IsAccept(again), gap);
    r.push_back({ IsAccept(again) && gap >= 1900 && gap <= 2200, m });

    net.Stop();
    const bool stopped = !net.Running();
    net.Stop();
    std::snprintf(m, sizeof(m), "④ 멈춤 — 스레드 끝남 %d · 두 번 불러도 됨", (int)stopped);
    r.push_back({ stopped && !net.Running(), m });
    closesocket(c);

    // ⑤ ★ 10-09 조각 5d-2c — 닫기 — 요청을 넣자마자 `Stop(알림)` 해도 그 요청이 나가고, 그 뒤 `SERVER_CLOSE`(0x0b) 가 간다
    {
        nox::CNetworkThread n2;
        n2.SetUdpOptions(rio, 0, 0);
        n2.Network().SetXorKey(0);
        std::string e2;
        n2.Open("127.0.0.1", 0, e2);
        n2.Start();
        const SOCKET c2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(c2, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        sockaddr_in to2 = me;
        to2.sin_port = htons(n2.Port());
        sendto(c2, reinterpret_cast<const char*>(conn), sizeof(conn), 0, reinterpret_cast<sockaddr*>(&to2), sizeof(to2));
        const bool acc2 = IsAccept(RecvOne(c2, 1000));
        int64_t sid2 = 0;
        for (int i = 0; i < 100 && sid2 == 0; ++i) {
            n2.SwapOutNetworkEvents(evs);
            for (const auto& e : evs) if (e.type == nox::NetworkEvent::Type::CONNECTED) sid2 = e.sessionId;
            evs.clear();
            if (sid2 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // 접속 시도만 한 상대 하나 — 키를 안 받았으니 닫힘 알림이 안 가야 한다
        const SOCKET c2b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(c2b, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        std::vector<uint8_t> tj(100, 0);
        tj[2] = static_cast<uint8_t>(nox::Op::SERVER_TRY_JOIN);
        sendto(c2b, reinterpret_cast<const char*>(tj.data()), static_cast<int>(tj.size()), 0, reinterpret_cast<sockaddr*>(&to2), sizeof(to2));
        for (int i = 0; i < 100 && n2.LatestThreadStats().datagramsIn < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<nox::NetworkRequest> last(1);
        last[0].type = nox::NetworkRequest::Type::SEND_MSG;
        last[0].sessionId = sid2;
        last[0].body = { 0x27, 9, 9 };
        n2.PushRequests(last);
        n2.Stop(/*closeSessions=*/true);
        const bool unkeyedClosed = !RecvOne(c2b, 300).empty();
        closesocket(c2b);
        int got27 = 0, gotClose = 0;
        for (int i = 0; i < 6; ++i) {
            const std::vector<uint8_t> d = RecvOne(c2, 500);
            if (d.empty()) break;
            if (d.size() == 5 && d[2] == 0x27) ++got27;
            if (d.size() == 3 && d[2] == static_cast<uint8_t>(nox::Op::SERVER_CLOSE)) ++gotClose;
        }
        const unsigned long long closeSent = n2.LatestNetworkStats().serverCloseSent;
        std::snprintf(m, sizeof(m), "⑤ 닫기 — 접속 %d · 넣자마자 멈춘 요청 받음 %d · SERVER_CLOSE 받음 %d(셈 %llu) · 접속 시도만 한 상대가 받음 %d · 스레드 끝남 %d",
                      (int)acc2, got27, gotClose, closeSent, (int)unkeyedClosed, (int)!n2.Running());
        r.push_back({ acc2 && got27 == 1 && gotClose == 1 && closeSent == 1 && !unkeyedClosed && !n2.Running(), m });
        closesocket(c2);
    }

    // ⑥ ★ 10-09 조각 5d-2c — 큐 한도 — 한도 3 에 요청 다섯(시작 전에 넣음) · 이벤트 다섯(접속 시도 다섯 · 게임이 안 꺼냄) → 둘씩 버리고 센다
    {
        nox::CNetworkThread n3;
        n3.SetUdpOptions(rio, 0, 0);
        n3.SetQueueMaxForTest(3);
        std::string e3;
        n3.Open("127.0.0.1", 0, e3);
        std::vector<nox::NetworkRequest> many(5);
        for (auto& q : many) { q.type = nox::NetworkRequest::Type::SEND_MSG; q.sessionId = 999; q.body = { 0x27 }; }
        n3.PushRequests(many);
        n3.Start();
        const SOCKET c3 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(c3, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        sockaddr_in to3 = me;
        to3.sin_port = htons(n3.Port());
        std::vector<uint8_t> tryJoin(100, 0);
        tryJoin[2] = static_cast<uint8_t>(nox::Op::SERVER_TRY_JOIN);
        for (int i = 0; i < 5; ++i)
            sendto(c3, reinterpret_cast<const char*>(tryJoin.data()), static_cast<int>(tryJoin.size()), 0,
                   reinterpret_cast<sockaddr*>(&to3), sizeof(to3));
        nox::CNetworkThreadStats ts;
        for (int i = 0; i < 100; ++i) {
            ts = n3.LatestThreadStats();
            if (ts.datagramsIn >= 5) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::snprintf(m, sizeof(m), "⑥ 큐 한도 3 — 받음 %llu · 버림 이벤트 %llu · 요청 %llu · 큐에 남은 이벤트 %zu",
                      ts.datagramsIn, ts.eventsDropped, ts.requestsDropped, ts.eventsQueued);
        r.push_back({ ts.datagramsIn == 5 && ts.eventsDropped == 2 && ts.requestsDropped == 2 && ts.eventsQueued == 3, m });
        n3.Stop();
        closesocket(c3);
    }

    // ⑦ ★ 10-14 작업 8c — 로비 TCP 듣기(채널의 4000 — `OpenLobby`) 시험은 ★ 10-20 v2 작업 4b-2 에서 듣기와 같이 걷었다(⑧ 이 로비에 붙기)

    // ⑧ ★ 10-20 MO 6단계 v2 작업 4a — 로비에 붙기(`ConnectLobby`) — 게임 서버가 로비 연결 포트에 붙는다.
    //   붙음 → LOBBY_OPENED · 줄 왕복 · 로비가 끊음 → LOBBY_CLOSED → 다시 붙기 간격 뒤 새 접속 · 듣는 이 없음 → 게임엔 사건 없이 셈만 ·
    //   듣기와 붙기는 같이 못 씀 · 주소 0 거절
    {
        nox::CNetworkThread bad1, bad2;
        std::string e5;
        const bool refuse = !bad1.ConnectLobby(0, 4010, e5) && !bad1.ConnectLobby(0x7f000001u, 0, e5) && bad2.ConnectLobby(0x7f000001u, 4010, e5);
        std::snprintf(m, sizeof(m), "⑧-1 주소 0 · 포트 0 → 거절 · 바른 주소 → 받음 %d", (int)refuse);
        r.push_back({ refuse, m });

        nox::CTcpLines srv;
        std::vector<nox::TcpLineEvent> se;
        const bool listening = srv.Listen(0x7f000001u, 0, e5);
        auto pollSrv = [&](size_t want, int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (se.size() < want && std::chrono::steady_clock::now() < until) {
                WaitForSingleObject(static_cast<HANDLE>(srv.WaitHandle()), 20);
                srv.Poll(se);
                srv.Flush(se);
            }
        };
        nox::CNetworkThread n5;
        n5.SetUdpOptions(rio, 0, 0);
        n5.SetLobbyRetryMsForTest(300);
        n5.SetMaxWaitMs(5000);   // 다시 붙기 시각에 스스로 깨야 아래 3초 안에 붙는다(기본 0.25초 깨기에 기대지 않게)
        const bool started = n5.Open("127.0.0.1", 0, e5) && n5.ConnectLobby(0x7f000001u, srv.ListenPort(0), e5);
        n5.Start();
        std::vector<nox::NetworkEvent> ev5;
        auto drain5 = [&](size_t want, int ms) {
            for (int i = 0; i < ms / 10 && ev5.size() < want; ++i) {
                std::vector<nox::NetworkEvent> got5;
                n5.SwapOutNetworkEvents(got5);
                for (auto& e : got5) ev5.push_back(std::move(e));
                if (ev5.size() < want) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        pollSrv(1, 2000);
        drain5(1, 2000);
        const int sconn = se.empty() ? 0 : se[0].conn;
        const int c1 = ev5.empty() ? 0 : ev5[0].lobbyConn;
        const bool up = listening && started && se.size() == 1 && se[0].type == nox::TcpLineEvent::Type::OPENED && ev5.size() == 1 &&
                        ev5[0].type == nox::NetworkEvent::Type::LOBBY_OPENED && c1 > 0 && ev5[0].lobbyPeerIp == 0x7f000001u &&
                        ev5[0].sessionId == 0;
        std::snprintf(m, sizeof(m), "⑧-2 시작하자마자 붙음 — 로비 쪽 접속 %zu · 게임 쪽 LOBBY_OPENED(상대 127.0.0.1 · 세션 번호 없음) %d", se.size(), (int)up);
        r.push_back({ up, m });

        se.clear();
        ev5.clear();
        srv.Send(sconn, "HELLO abc");
        srv.Flush(se);
        drain5(1, 2000);
        std::vector<nox::NetworkRequest> rq5(1);
        rq5[0].type = nox::NetworkRequest::Type::LOBBY_SEND;
        rq5[0].lobbyConn = c1;
        rq5[0].lobbyLine = "AUTH x";
        n5.PushRequests(rq5);
        pollSrv(1, 2000);
        const bool lines = ev5.size() == 1 && ev5[0].type == nox::NetworkEvent::Type::LOBBY_LINE && ev5[0].lobbyLine == "HELLO abc" &&
                           ev5[0].lobbyConn == c1 && se.size() == 1 && se[0].type == nox::TcpLineEvent::Type::LINE && se[0].line == "AUTH x";
        std::snprintf(m, sizeof(m), "⑧-3 줄 왕복 — 로비 → LOBBY_LINE · LOBBY_SEND → 로비 %d", (int)lines);
        r.push_back({ lines, m });

        se.clear();
        ev5.clear();
        srv.Close(sconn, se);
        se.clear();
        drain5(2, 3000);   // 끊김 + 다시 붙음
        pollSrv(1, 3000);
        const bool reconnected = ev5.size() == 2 && ev5[0].type == nox::NetworkEvent::Type::LOBBY_CLOSED && ev5[0].lobbyConn == c1 &&
                           ev5[1].type == nox::NetworkEvent::Type::LOBBY_OPENED && ev5[1].lobbyConn != c1 && se.size() == 1 &&
                           se[0].type == nox::TcpLineEvent::Type::OPENED;
        std::snprintf(m, sizeof(m), "⑧-4 로비가 끊음 → LOBBY_CLOSED → 다시 붙기(새 접속 번호 · 로비 쪽 새 접속) %d · 사건 %zu", (int)reconnected, ev5.size());
        r.push_back({ reconnected, m });
        // ★ 7단계 조각 2 — 게임 스레드가 끊으라고 함(`LOBBY_CLOSE` — 살아 있음 한도) → LOBBY_CLOSED → 끊김과 같은 길로 다시 붙기 · 로비 쪽은 상대가 닫음
        const int c2 = ev5.size() == 2 ? ev5[1].lobbyConn : 0;
        se.clear();
        ev5.clear();
        std::vector<nox::NetworkRequest> close5(1);
        close5[0].type = nox::NetworkRequest::Type::LOBBY_CLOSE;
        close5[0].lobbyConn = c2;
        n5.PushRequests(close5);
        drain5(2, 3000);
        pollSrv(2, 3000);
        // 로비 쪽은 게임 쪽 사건 둘을 다 기다린 뒤에 읽어 옛 접속의 끊김과 새 접속이 같이 쌓여 있다 — 차례는 안 본다(받기가 먼저 나온다)
        size_t srvClosed = 0, srvOpened = 0;
        for (const auto& e : se) {
            srvClosed += e.type == nox::TcpLineEvent::Type::CLOSED ? 1 : 0;
            srvOpened += e.type == nox::TcpLineEvent::Type::OPENED ? 1 : 0;
        }
        const bool closedAgain = c2 > 0 && ev5.size() == 2 && ev5[0].type == nox::NetworkEvent::Type::LOBBY_CLOSED && ev5[0].lobbyConn == c2 &&
                                 ev5[1].type == nox::NetworkEvent::Type::LOBBY_OPENED && ev5[1].lobbyConn != c2 && se.size() == 2 &&
                                 srvClosed == 1 && srvOpened == 1;
        std::snprintf(m, sizeof(m), "⑧-4b LOBBY_CLOSE → LOBBY_CLOSED(접속 %d) → 다시 붙기 · 로비 쪽 끊김 + 새 접속 %d · 사건 %zu[%d %d · 접속 %d %d] · 로비 쪽 %zu[%d %d]",
                      c2, (int)closedAgain, ev5.size(), ev5.size() > 0 ? (int)ev5[0].type : -1, ev5.size() > 1 ? (int)ev5[1].type : -1,
                      ev5.size() > 0 ? ev5[0].lobbyConn : -1, ev5.size() > 1 ? ev5[1].lobbyConn : -1, se.size(),
                      se.size() > 0 ? (int)se[0].type : -1, se.size() > 1 ? (int)se[1].type : -1);
        r.push_back({ closedAgain, m });
        // 끊긴 옛 접속으로 보낸 줄 — 버리고 센다(끊김과 게임 스레드의 줄이 엇갈리면 생긴다 — 옛 ⑦ 의 「없는 접속으로」 셈)
        std::vector<nox::NetworkRequest> stale(1);
        stale[0].type = nox::NetworkRequest::Type::LOBBY_SEND;
        stale[0].lobbyConn = c1;
        stale[0].lobbyLine = "late";
        n5.PushRequests(stale);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        n5.Stop();
        const nox::CNetworkThreadStats s5 = n5.LatestThreadStats();
        std::snprintf(m, sizeof(m), "⑧-5 셈 — 시도 %llu · 붙음 %llu · 못 붙음 %llu · 끊김 %llu · 받은 줄 %llu · 보낸 줄 %llu · 옛 접속으로 %llu · 끊기 요청 %llu",
                      s5.lobbyConnects, s5.lobbyOpened, s5.lobbyConnectFailed, s5.lobbyClosed, s5.lobbyLinesIn, s5.lobbyLinesOut,
                      s5.lobbySendNoConn, s5.lobbyCloseRequests);
        r.push_back({ s5.lobbyConnects == 3 && s5.lobbyOpened == 3 && s5.lobbyConnectFailed == 0 && s5.lobbyClosed == 2 &&
                          s5.lobbyLinesIn == 1 && s5.lobbyLinesOut == 1 && s5.lobbySendNoConn == 1 && s5.lobbyCloseRequests == 1, m });

        // 듣는 이 없는 포트 — 게임 스레드에 사건이 안 가고(붙기 전 끊김은 못 붙음으로 셈) 다시 시도한다
        uint16_t dead = 0;
        {
            nox::CTcpLines tmp;
            std::string e6;
            if (tmp.Listen(0x7f000001u, 0, e6)) dead = tmp.ListenPort(0);
        }
        nox::CNetworkThread n6;
        n6.SetUdpOptions(rio, 0, 0);
        n6.SetLobbyRetryMsForTest(300);
        const bool s6 = n6.Open("127.0.0.1", 0, e5) && n6.ConnectLobby(0x7f000001u, dead, e5);
        n6.Start();
        nox::CNetworkThreadStats t6;
        for (int i = 0; i < 80; ++i) {   // 윈도 루프백 거절은 한 번에 1~2초 — 둘째 시도까지
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            t6 = n6.LatestThreadStats();
            if (t6.lobbyConnects >= 2) break;
        }
        std::vector<nox::NetworkEvent> ev6;
        n6.SwapOutNetworkEvents(ev6);
        n6.Stop();
        std::snprintf(m, sizeof(m), "⑧-6 듣는 이 없음 — 시도 %llu · 못 붙음 %llu · 붙음 %llu · 게임에 간 사건 %zu",
                      t6.lobbyConnects, t6.lobbyConnectFailed, t6.lobbyOpened, ev6.size());
        r.push_back({ s6 && dead != 0 && t6.lobbyConnects >= 2 && t6.lobbyConnectFailed >= 1 && t6.lobbyOpened == 0 && ev6.empty(), m });
    }

    // ⑨ ★ 11-17 C-2 — 몰아 보내기 · 칸보다 큰 조각. 모르는 주소에서 쓰레기 300 조각(통신 층이 세고 버린다 — `droppedNoSession`) +
    //   한 칸(2,048)보다 큰 3,000바이트 하나 → 받은 조각 = 접속 1 + 300 · 칸보다 커서 버림 1 · 받기 오류(`recvErrors`) 1 · 그 밖의 받기 오류 0.
    //   팔이 실제로 바뀌었나 — 접속 수락을 보냈으니 RIO 는 묶어 낸 수(`udpFlushes`)가 1 이상 · 보통은 0. 받기 칸 수는 팔마다 기본
    {
        nox::CNetworkThread n9;
        n9.SetUdpOptions(rio, 0, 0);
        n9.Network().SetXorKey(0);
        std::string e9;
        const bool open9 = n9.Open("127.0.0.1", 0, e9);
        n9.Start();
        sockaddr_in to9 = me;
        to9.sin_port = htons(n9.Port());
        const SOCKET c9a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(c9a, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        sendto(c9a, reinterpret_cast<const char*>(conn), sizeof(conn), 0, reinterpret_cast<sockaddr*>(&to9), sizeof(to9));
        const bool acc9 = IsAccept(RecvOne(c9a, 1000));
        const SOCKET c9 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(c9, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        const uint8_t junk[2] = { 0x00, 0x00 };
        for (int i = 0; i < 300; ++i)
            sendto(c9, reinterpret_cast<const char*>(junk), sizeof(junk), 0, reinterpret_cast<sockaddr*>(&to9), sizeof(to9));
        const std::vector<char> big(3000, 0);
        sendto(c9, big.data(), static_cast<int>(big.size()), 0, reinterpret_cast<sockaddr*>(&to9), sizeof(to9));
        nox::CNetworkThreadStats t9;
        for (int i = 0; i < 150; ++i) {   // 지표 사본은 0.25초마다 — 3초까지
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            t9 = n9.LatestThreadStats();
            if (t9.datagramsIn >= 301 && t9.udpOversizeDropped >= 1) break;
        }
        n9.Stop();
        closesocket(c9);
        closesocket(c9a);
        const uint32_t wantSlots = rio ? nox::CNetworkThread::kUdpRecvSlotsRio : nox::CNetworkThread::kUdpRecvSlotsWinsock;
        const bool armOk = rio ? t9.udpFlushes >= 1 : t9.udpFlushes == 0;
        std::snprintf(m, sizeof(m), "⑨ 몰아 보내기 — 열기 %d · 수락 %d · 받음 %llu(301) · 칸보다 큼 %llu(1) · 받기 오류 %llu(1) · 그 밖 %llu(0) · 묶어 냄 %llu · 받기 칸 %u(%u)",
                      (int)open9, (int)acc9, t9.datagramsIn, t9.udpOversizeDropped, t9.recvErrors, t9.udpRecvFail, t9.udpFlushes,
                      n9.UdpRecvSlots(), wantSlots);
        r.push_back({ open9 && acc9 && t9.datagramsIn == 301 && t9.udpOversizeDropped == 1 && t9.recvErrors == 1 && t9.udpRecvFail == 0 &&
                          armOk && n9.UdpRecvSlots() == wantSlots && t9.maxDatagramOutBytes > 0 && t9.maxDatagramsOutPerWake >= 1, m });
    }
    if (rio)
        for (auto& kv : r) kv.second = "[RIO 팔] " + kv.second;
    return r;
}

// ⑩ ★ 11-17 C-3b — 게임 스레드 여럿 — 세션 주인 정하기(`CSessionOwners` · 계획 5-2) · 큐 번호마다 사건 · 요청.
//   앞은 소켓 없이 주인 지도만(갈래마다 개수로) · 뒤는 실제 소켓(토큰 꼬리를 단 접속 → 그 방 주인 큐 · 넣는 스레드별 요청 큐 · 로비 큐).
//   팔과 상관없는 일이라 보통 팔로 한 번만 돈다(`proto_test` `TestNetworkThread`)
namespace {

// 접속 시도 · CONNECT 꼬리에 실을 토큰 — 판 2 · 방 번호만 채움(통신 스레드는 서명을 안 본다)
std::vector<uint8_t> TokenForRoom(uint32_t room) {
    std::vector<uint8_t> t(nox::kJoinTokenSize, 0);
    t[0] = nox::kJoinTokenVersion;
    t[1] = static_cast<uint8_t>(room);
    t[2] = static_cast<uint8_t>(room >> 8);
    t[3] = static_cast<uint8_t>(room >> 16);
    t[4] = static_cast<uint8_t>(room >> 24);
    return t;
}

nox::NetworkEvent Ev(nox::NetworkEvent::Type t, int64_t sid, const std::vector<uint8_t>& token = {}) {
    nox::NetworkEvent e;
    e.type = t;
    e.sessionId = sid;
    e.token = token;
    return e;
}

// 갈래 이름 — 갈래를 늘리면 여기서 컴파일러가 잡는다(값을 돌려주는 switch)
const char* WhyName(nox::CSessionOwners::Why w) {
    switch (w) {
        case nox::CSessionOwners::Why::Single: return "하나";
        case nox::CSessionOwners::Why::Lobby: return "로비";
        case nox::CSessionOwners::Why::Known: return "앎";
        case nox::CSessionOwners::Why::Token: return "토큰";
        case nox::CSessionOwners::Why::NoToken: return "토큰 없음";
        case nox::CSessionOwners::Why::LoadArrival: return "도착 차례";
        case nox::CSessionOwners::Why::ThreadZero: return "0 고정";
    }
    return "?";
}

}  // namespace

std::vector<std::pair<bool, std::string>> RunGameThreadRouteChecks() {
    using T = nox::NetworkEvent::Type;
    using W = nox::CSessionOwners::Why;
    std::vector<std::pair<bool, std::string>> r;
    char m[300];

    // ⑩-1 토큰의 방 번호 % N — [1..4] 작은 끝수 · 부호 없는 수.
    //   0x01020304 % 2 = 0(큰 끝수로 읽으면 1) · 0x80000001 % 3 = 0(부호 있는 수로 읽으면 -1) · 5 % 2 = 1 · 7 % 3 = 1 · 길이 52 · 빈 것 → 토큰 없음 0
    {
        bool h1 = false, h2 = false, h3 = false, h4 = false, h5 = false, h6 = false;
        const int a = nox::CSessionOwners::OwnerByToken(TokenForRoom(0x01020304u), 2, h1);
        const int b = nox::CSessionOwners::OwnerByToken(TokenForRoom(0x80000001u), 3, h2);
        const int c = nox::CSessionOwners::OwnerByToken(TokenForRoom(5), 2, h3);
        const int d = nox::CSessionOwners::OwnerByToken(TokenForRoom(7), 3, h4);
        std::vector<uint8_t> shortTok = TokenForRoom(5);
        shortTok.pop_back();
        const int e = nox::CSessionOwners::OwnerByToken(shortTok, 2, h5);
        const int f = nox::CSessionOwners::OwnerByToken({}, 2, h6);
        std::snprintf(m, sizeof(m), "⑩-1 토큰 방 %% N — 0x01020304%%2=%d(0) · 0x80000001%%3=%d(0) · 5%%2=%d(1) · 7%%3=%d(1) · 52바이트 %d/%d(0/0) · 빈 것 %d/%d(0/0)",
                      a, b, c, d, e, (int)h5, f, (int)h6);
        r.push_back({ a == 0 && b == 0 && c == 1 && d == 1 && e == 0 && !h5 && f == 0 && !h6 && h1 && h2 && h3 && h4, m });
    }

    // ⑩-2 토큰 규칙 N = 2 — 갈래마다 개수. 사건 여덟:
    //   로비 둘(→ 큐 2) · 세션 1 CONNECTED 방 3(→ 1 토큰) · 세션 1 RECEIVED(→ 1 앎) · 세션 2 JOIN 토큰 없음(→ 0 토큰 없음) ·
    //   세션 2 CONNECTED 방 1(→ 0 앎 — 첫 것이 주인) · 세션 3 JOIN 방 4(→ 0 토큰) · 세션 3 DISCONNECTED(→ 0 앎)
    {
        nox::CSessionOwners o;
        o.Configure(2, nox::OwnerRule::Token, 0);
        struct Case { nox::NetworkEvent ev; int want; W why; };
        const Case cases[] = {
            { Ev(T::LOBBY_OPENED, 0), 2, W::Lobby },
            { Ev(T::CONNECTED, 1, TokenForRoom(3)), 1, W::Token },
            { Ev(T::RECEIVED, 1), 1, W::Known },
            { Ev(T::JOIN_REQUESTED, 2), 0, W::NoToken },
            { Ev(T::CONNECTED, 2, TokenForRoom(1)), 0, W::Known },
            { Ev(T::JOIN_REQUESTED, 3, TokenForRoom(4)), 0, W::Token },
            { Ev(T::LOBBY_LINE, 0), 2, W::Lobby },
            { Ev(T::DISCONNECTED, 3), 0, W::Known },
        };
        int ok = 0;
        int tally[7] = {};
        std::string bad;
        for (const Case& cs : cases) {
            W w = W::Single;
            const int k = o.Route(cs.ev, &w);
            ++tally[static_cast<int>(w)];
            if (k == cs.want && w == cs.why) ++ok;
            else bad += std::string(" [") + WhyName(w) + "→" + std::to_string(k) + "]";
        }
        const bool counts = o.byToken == 2 && o.noToken == 1 && o.byLoad == 0 && o.threadZero == 0 && o.Size() == 3 &&
                            tally[static_cast<int>(W::Lobby)] == 2 && tally[static_cast<int>(W::Known)] == 3 &&
                            tally[static_cast<int>(W::Token)] == 2 && tally[static_cast<int>(W::NoToken)] == 1;
        std::snprintf(m, sizeof(m), "⑩-2 토큰 규칙 N=2 — 맞음 %d/8 · 셈 토큰 %llu(2) · 토큰 없음 %llu(1) · 지도 %zu(3) · 큐 수 %d(3) · 로비 큐 %d(2)%s",
                      ok, o.byToken, o.noToken, o.Size(), o.QueueCount(), o.LobbyQueue(), bad.c_str());
        r.push_back({ ok == 8 && counts && o.QueueCount() == 3 && o.LobbyQueue() == 2, m });

        // ⑩-3 지우기 — 끊기 요청(`Forget`) 하나 · 없는 번호는 안 셈 · 걷힌 세션(`Prune` — 2 만 없어짐) 하나 · 지운 번호가 다시 오면 새로 정한다
        o.Forget(1);
        o.Forget(99);
        const size_t pr = o.Prune([](int64_t sid) { return sid != 2; });
        W w4 = W::Single;
        const int k4 = o.Route(Ev(T::CONNECTED, 1, TokenForRoom(4)), &w4);
        std::snprintf(m, sizeof(m), "⑩-3 지우기 — 끊기 %llu(1) · 걷음 %zu/%llu(1/1) · 남은 지도 %zu(2 — 3 과 다시 정한 1) · 다시 온 1 → %d %s(0 토큰)",
                      o.forgot, pr, o.pruned, o.Size(), k4, WhyName(w4));
        r.push_back({ o.forgot == 1 && pr == 1 && o.pruned == 1 && o.Size() == 2 && k4 == 0 && w4 == W::Token && o.Knows(3) && !o.Knows(2), m });
    }

    // ⑩-4 게임 스레드 하나 — 지도를 안 쓰고 모두 큐 0(로비 사건까지 — 옛 꼴) · 셈 0
    {
        nox::CSessionOwners o;
        o.Configure(1, nox::OwnerRule::Token, 0);
        W w1 = W::Lobby, w2 = W::Lobby;
        const int k1 = o.Route(Ev(T::CONNECTED, 1, TokenForRoom(3)), &w1);
        const int k2 = o.Route(Ev(T::LOBBY_LINE, 0), &w2);
        std::snprintf(m, sizeof(m), "⑩-4 하나 — 토큰 방 3 → %d %s · 로비 → %d %s · 지도 %zu · 셈 %llu · 큐 수 %d · 로비 큐 %d",
                      k1, WhyName(w1), k2, WhyName(w2), o.Size(), o.byToken + o.noToken, o.QueueCount(), o.LobbyQueue());
        r.push_back({ k1 == 0 && k2 == 0 && w1 == W::Single && w2 == W::Single && o.Size() == 0 && o.byToken + o.noToken == 0 &&
                          o.QueueCount() == 1 && o.LobbyQueue() == 0, m });
    }

    // ⑩-4b ★ 11-17 C-3d-4 — 게임 스레드 하나 + 로비 큐 따로(조정 스레드 · C 플랜 원칙 3) — 로비 사건만 큐 1 · 나머지는 큐 0 · 지도 · 셈 안 씀.
    //   같은 사건을 따로 아닌 판(⑩-4)과 견준다 — 로비 사건 하나만 갈린다
    {
        nox::CSessionOwners o;
        o.Configure(1, nox::OwnerRule::Token, 0, /*lobbyApart=*/true);
        W w1 = W::Lobby, w2 = W::Single, w3 = W::Lobby, w4 = W::Single;
        const int k1 = o.Route(Ev(T::CONNECTED, 1, TokenForRoom(3)), &w1);
        const int k2 = o.Route(Ev(T::LOBBY_LINE, 0), &w2);
        const int k3 = o.Route(Ev(T::RECEIVED, 1), &w3);
        const int k4 = o.Route(Ev(T::LOBBY_OPENED, 0), &w4);
        std::snprintf(m, sizeof(m), "⑩-4b 하나 + 로비 따로 — 토큰 방 3 → %d %s · 로비 줄 → %d %s · 받음 → %d %s · 로비 붙음 → %d %s · 지도 %zu · 셈 %llu · 큐 수 %d · 로비 큐 %d · 따로 %d",
                      k1, WhyName(w1), k2, WhyName(w2), k3, WhyName(w3), k4, WhyName(w4), o.Size(), o.byToken + o.noToken + o.byLoad + o.threadZero,
                      o.QueueCount(), o.LobbyQueue(), (int)o.LobbyApart());
        r.push_back({ k1 == 0 && w1 == W::Single && k2 == 1 && w2 == W::Lobby && k3 == 0 && w3 == W::Single && k4 == 1 && w4 == W::Lobby &&
                          o.Size() == 0 && o.byToken + o.noToken + o.byLoad + o.threadZero == 0 && o.QueueCount() == 2 && o.LobbyQueue() == 1 &&
                          o.LobbyApart(), m });
    }

    // ⑩-5 부하 판 — 방 인원 10 · N = 2 · 새 세션 25 → 0 열 · 1 열 · 0 다섯(15 / 10) · 이미 아는 세션은 차례를 안 먹음 → 26번째 새 세션 = 차례 25 → 0.
    //   N = 3 · 30 → 10 / 10 / 10(셋째 열이 2 로 가나 — 나머지가 하나 어긋나면 갈린다)
    {
        nox::CSessionOwners o;
        o.Configure(2, nox::OwnerRule::LoadArrival, 10);
        int per[2] = {};
        bool order = true;
        for (int i = 0; i < 25; ++i) {
            const int k = o.Route(Ev(T::CONNECTED, 100 + i));
            if (k >= 0 && k < 2) ++per[k];
            if (k != (i / 10) % 2) order = false;
        }
        W wk = W::Single;
        const int again = o.Route(Ev(T::RECEIVED, 100 + 12), &wk);
        const int next = o.Route(Ev(T::CONNECTED, 200));
        nox::CSessionOwners o3;
        o3.Configure(3, nox::OwnerRule::LoadArrival, 10);
        int per3[3] = {};
        for (int i = 0; i < 30; ++i) {
            const int k = o3.Route(Ev(T::JOIN_REQUESTED, 1 + i, TokenForRoom(1)));
            if (k >= 0 && k < 3) ++per3[k];
        }   // 부하 판은 토큰을 안 본다
        std::snprintf(m, sizeof(m), "⑩-5 부하 판 — N=2 · 25 → %d/%d(15/10) 차례 %d · 아는 세션 12 → %d %s(1 앎) · 다음 새 → %d(0) · 셈 %llu(26) · N=3 · 30 → %d/%d/%d(10/10/10) · 토큰 셈 %llu(0)",
                      per[0], per[1], (int)order, again, WhyName(wk), next, o.byLoad, per3[0], per3[1], per3[2], o3.byToken);
        r.push_back({ per[0] == 15 && per[1] == 10 && order && again == 1 && wk == W::Known && next == 0 && o.byLoad == 26 &&
                          per3[0] == 10 && per3[1] == 10 && per3[2] == 10 && o3.byToken == 0 && o3.byLoad == 30, m });
    }

    // ⑩-6 열린 방 하나(토큰 끔 · 부하 판 아님) — 토큰이 있어도 0 · 셈
    {
        nox::CSessionOwners o;
        o.Configure(3, nox::OwnerRule::ThreadZero, 0);
        W w = W::Single;
        const int k = o.Route(Ev(T::CONNECTED, 1, TokenForRoom(2)), &w);
        std::snprintf(m, sizeof(m), "⑩-6 열린 방 — 토큰 방 2 · N=3 → %d %s(0) · 셈 %llu(1)", k, WhyName(w), o.threadZero);
        r.push_back({ k == 0 && w == W::ThreadZero && o.threadZero == 1 && o.byToken == 0, m });
    }

    // ⑩-7 `SetGameThreads` 거절 셋(0 · 상한 넘음 · 부하 판에 방 인원 없음) · 받음 → 큐 셋 · 손잡이 0~2 있음 · 3 · -1 없음
    {
        nox::CNetworkThread n;
        std::string e;
        const bool r0 = !n.SetGameThreads(0, nox::OwnerRule::Token, 0, e);
        const bool r1 = !n.SetGameThreads(nox::CNetworkThread::kMaxGameThreads + 1, nox::OwnerRule::Token, 0, e);
        const bool r2 = !n.SetGameThreads(2, nox::OwnerRule::LoadArrival, 0, e);
        const bool before = n.QueueCount() == 1 && n.EventsReadyHandle() != nullptr;
        const bool ok = n.SetGameThreads(2, nox::OwnerRule::Token, 0, e);
        const bool handles = n.EventsReadyHandle(0) && n.EventsReadyHandle(1) && n.EventsReadyHandle(2) && !n.EventsReadyHandle(3) &&
                             !n.EventsReadyHandle(-1) && n.EventsReadyHandle(0) != n.EventsReadyHandle(1);
        std::snprintf(m, sizeof(m), "⑩-7 게임 스레드 수 — 거절 0 %d · 상한 넘음 %d · 방 인원 없음 %d · 처음 큐 하나 %d · 받음 %d · 큐 %d(3) · 로비 큐 %d(2) · 손잡이 %d",
                      (int)r0, (int)r1, (int)r2, (int)before, (int)ok, n.QueueCount(), n.LobbyQueue(), (int)handles);
        r.push_back({ r0 && r1 && r2 && before && ok && n.QueueCount() == 3 && n.LobbyQueue() == 2 && handles, m });
    }

    // ⑩-7b ★ 11-17 C-3d-4 — 하나 + 로비 따로 → 큐 둘 · 로비 큐 1 · 손잡이 0 · 1 있음(서로 다름) · 2 없음. 따로 없이 다시 정하면 옛 꼴(큐 하나)
    {
        nox::CNetworkThread n;
        std::string e;
        const bool ok = n.SetGameThreads(1, nox::OwnerRule::Token, 0, e, /*lobbyApart=*/true);
        const int qc = n.QueueCount(), lq = n.LobbyQueue();
        const bool handles = n.EventsReadyHandle(0) && n.EventsReadyHandle(1) && !n.EventsReadyHandle(2) && n.EventsReadyHandle(0) != n.EventsReadyHandle(1);
        const bool back = n.SetGameThreads(1, nox::OwnerRule::Token, 0, e);
        std::snprintf(m, sizeof(m), "⑩-7b 하나 + 로비 따로 — 받음 %d · 큐 %d(2) · 로비 큐 %d(1) · 손잡이 %d · 따로 없이 다시 → 큐 %d(1) · 로비 큐 %d(0)",
                      (int)ok, qc, lq, (int)handles, n.QueueCount(), n.LobbyQueue());
        r.push_back({ ok && qc == 2 && lq == 1 && handles && back && n.QueueCount() == 1 && n.LobbyQueue() == 0, m });
    }

    // ⑩-8 실물 — N = 2 · 토큰 규칙. 소켓 A 가 꼬리(방 1)를 단 CONNECT → CONNECTED 는 큐 1 로만(큐 1 손잡이가 켜지고 큐 0 은 꺼진 채) ·
    //   소켓 B 가 꼬리 없는 CONNECT → 큐 0 · 게임 스레드 1 이 넣은 보내기 → A · 0 이 넣은 보내기 → B · 없는 번호 7 로 넣은 보내기도 A 에 감(셈 1) ·
    //   A 끊기 요청 → 주인 지움 1 · 지도 1
    {
        nox::CNetworkThread n;
        n.Network().SetXorKey(0);
        std::string e;
        const bool set = n.SetGameThreads(2, nox::OwnerRule::Token, 0, e);
        const bool open = n.Open("127.0.0.1", 0, e);
        n.Start();
        sockaddr_in me = {};
        me.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
        sockaddr_in to = me;
        to.sin_port = htons(n.Port());
        const SOCKET a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const SOCKET b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bind(a, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        bind(b, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        std::vector<uint8_t> connA = { 0xff, 0x00, 0x00 };
        const std::vector<uint8_t> tokA = TokenForRoom(1);
        connA.insert(connA.end(), tokA.begin(), tokA.end());
        connA.insert(connA.end(), nox::kJoinTrailerMagic, nox::kJoinTrailerMagic + sizeof(nox::kJoinTrailerMagic));
        sendto(a, reinterpret_cast<const char*>(connA.data()), static_cast<int>(connA.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        const bool accA = IsAccept(RecvOne(a, 1000));
        const bool sig1 = WaitForSingleObject(static_cast<HANDLE>(n.EventsReadyHandle(1)), 1000) == WAIT_OBJECT_0;
        const bool sig0 = WaitForSingleObject(static_cast<HANDLE>(n.EventsReadyHandle(0)), 0) == WAIT_OBJECT_0;
        std::vector<nox::NetworkEvent> q0, q1, q2;
        n.SwapOutNetworkEvents(0, q0);
        n.SwapOutNetworkEvents(1, q1);
        const int64_t sidA = q1.size() == 1 && q1[0].type == T::CONNECTED ? q1[0].sessionId : 0;
        const bool aRouted = q0.empty() && sidA != 0 && sig1 && !sig0;
        std::snprintf(m, sizeof(m), "⑩-8a 꼬리 방 1 접속 — 설정 %d · 열기 %d · 수락 %d · 큐 1 사건 %zu(세션 %lld) · 큐 0 사건 %zu · 손잡이 1 켜짐 %d · 0 켜짐 %d",
                      (int)set, (int)open, (int)accA, q1.size(), (long long)sidA, q0.size(), (int)sig1, (int)sig0);
        r.push_back({ set && open && accA && aRouted, m });

        const uint8_t connB[3] = { 0xff, 0x00, 0x00 };
        sendto(b, reinterpret_cast<const char*>(connB), sizeof(connB), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        const bool accB = IsAccept(RecvOne(b, 1000));
        int64_t sidB = 0;
        for (int i = 0; i < 100 && sidB == 0; ++i) {
            q0.clear();
            n.SwapOutNetworkEvents(0, q0);
            for (const auto& ev : q0) if (ev.type == T::CONNECTED) sidB = ev.sessionId;
            if (sidB == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        q1.clear();
        n.SwapOutNetworkEvents(1, q1);
        std::snprintf(m, sizeof(m), "⑩-8b 꼬리 없는 접속 — 수락 %d · 큐 0 세션 %lld · 큐 1 사건 %zu(0)", (int)accB, (long long)sidB, q1.size());
        r.push_back({ accB && sidB != 0 && sidB != sidA && q1.empty(), m });

        auto sendTo = [&](int producer, int64_t sid, uint8_t tag) {
            std::vector<nox::NetworkRequest> rq(1);
            rq[0].type = nox::NetworkRequest::Type::SEND_MSG;
            rq[0].sessionId = sid;
            rq[0].body = { 0x27, tag, tag };
            n.PushRequests(producer, rq);
        };
        auto recvTag = [&](SOCKET s, uint8_t tag) {
            for (int i = 0; i < 5; ++i) {
                const std::vector<uint8_t> d = RecvOne(s, 1000);
                if (d.empty()) return false;
                if (d.size() == 5 && d[2] == 0x27 && d[3] == tag) return true;
            }
            return false;
        };
        sendTo(1, sidA, 0x11);
        const bool gotA = recvTag(a, 0x11);
        sendTo(0, sidB, 0x22);
        const bool gotB = recvTag(b, 0x22);
        sendTo(7, sidA, 0x33);
        const bool gotBad = recvTag(a, 0x33);
        std::vector<nox::NetworkRequest> dc(1);
        dc[0].type = nox::NetworkRequest::Type::DISCONNECT_SESSION;
        dc[0].sessionId = sidA;
        n.PushRequests(1, dc);
        nox::CNetworkThreadStats s;
        for (int i = 0; i < 100; ++i) {   // 지표 사본은 0.25초마다
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            s = n.LatestThreadStats();
            if (s.ownerForgot >= 1 && s.queueIndexBad >= 1) break;
        }
        n.Stop();
        closesocket(a);
        closesocket(b);
        std::snprintf(m, sizeof(m), "⑩-8c 요청 큐 — 스레드 1 → A %d · 0 → B %d · 없는 번호 7 → A %d(셈 %llu) · 끊기 → 주인 지움 %llu(1) · 지도 %zu(1) · 셈 토큰 %llu(1) · 토큰 없음 %llu(1)",
                      (int)gotA, (int)gotB, (int)gotBad, s.queueIndexBad, s.ownerForgot, s.owners, s.ownerByToken, s.ownerNoToken);
        r.push_back({ gotA && gotB && gotBad && s.queueIndexBad == 1 && s.ownerForgot == 1 && s.owners == 1 && s.ownerByToken == 1 &&
                          s.ownerNoToken == 1, m });
    }

    // ⑩-9 실물 — N = 2 로비 — 로비 사건은 로비 큐(2)로만 · 조정 스레드 몫 큐(2)로 넣은 줄이 로비에 간다
    //   ★ 11-17 C-3d-4 — 같은 시험을 N = 1 + 로비 따로(로비 큐 1)로 한 번 더(⑩-9b) — 게임 큐는 모두 비어야 한다
    auto lobbyQueueCase = [&](int gameThreads, bool apart, const char* tag) {
        nox::CTcpLines srv;
        std::vector<nox::TcpLineEvent> se;
        std::string e;
        const bool listening = srv.Listen(0x7f000001u, 0, e);
        nox::CNetworkThread n;
        const bool set = n.SetGameThreads(gameThreads, nox::OwnerRule::Token, 0, e, apart);
        const int lqi = n.LobbyQueue();
        const bool started = n.Open("127.0.0.1", 0, e) && n.ConnectLobby(0x7f000001u, srv.ListenPort(0), e);
        n.Start();
        auto pollSrv = [&](size_t want, int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (se.size() < want && std::chrono::steady_clock::now() < until) {
                WaitForSingleObject(static_cast<HANDLE>(srv.WaitHandle()), 20);
                srv.Poll(se);
                srv.Flush(se);
            }
        };
        std::vector<nox::NetworkEvent> lq, g0;
        size_t gameQueued = 0;   // 게임 큐(0 ~ N-1)에 온 사건 — 0 이어야 한다
        auto drainLobby = [&](size_t want, int ms) {
            for (int i = 0; i < ms / 10 && lq.size() < want; ++i) {
                std::vector<nox::NetworkEvent> got;
                n.SwapOutNetworkEvents(lqi, got);
                for (auto& ev : got) lq.push_back(std::move(ev));
                if (lq.size() < want) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        pollSrv(1, 2000);
        drainLobby(1, 2000);
        const int sconn = se.empty() ? 0 : se[0].conn;
        const int lconn = lq.empty() ? 0 : lq[0].lobbyConn;
        se.clear();
        srv.Send(sconn, "HELLO c3b");
        srv.Flush(se);
        drainLobby(2, 2000);
        std::vector<nox::NetworkRequest> rq(1);
        rq[0].type = nox::NetworkRequest::Type::LOBBY_SEND;
        rq[0].lobbyConn = lconn;
        rq[0].lobbyLine = "AUTH c3b";
        n.PushRequests(n.LobbyQueue(), rq);
        pollSrv(1, 2000);
        for (int q = 0; q < gameThreads; ++q) {
            g0.clear();
            n.SwapOutNetworkEvents(q, g0);
            gameQueued += g0.size();
        }
        n.Stop();
        const bool ok = listening && set && started && lqi == gameThreads && lq.size() == 2 && lq[0].type == T::LOBBY_OPENED &&
                        lq[1].type == T::LOBBY_LINE && lq[1].lobbyLine == "HELLO c3b" && gameQueued == 0 && se.size() == 1 && se[0].line == "AUTH c3b";
        std::snprintf(m, sizeof(m), "%s 로비 큐 %d — 로비 큐 사건 %zu(2 — 붙음 · 줄) · 게임 큐 %d개에 %zu(0) · 로비 큐로 넣은 줄이 로비에 %d",
                      tag, lqi, lq.size(), gameThreads, gameQueued, (int)(se.size() == 1 && se[0].line == "AUTH c3b"));
        r.push_back({ ok, m });
    };
    lobbyQueueCase(2, false, "⑩-9");
    lobbyQueueCase(1, true, "⑩-9b N=1 따로");
    return r;
}
