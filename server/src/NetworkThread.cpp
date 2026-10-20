// NetworkThread.cpp — 네트워크 스레드. 설명은 `NetworkThread.h` 머리.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "nox/NetworkThread.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "nox/Clock.h"
#include "nox/JoinToken.h"   // `Ipv4ToString`
#include "nox/Opcode.h"

#pragma comment(lib, "ws2_32.lib")

namespace nox {

namespace {
// ★ 10-09 플랜 A 5d 자가검증 ② — 시계는 게임 루프와 같은 `nox::NowMs`(`Clock.h` — 옛 이 자리 `GetTickCount64` 는 15.6ms 단위)

std::string KeyOf(const sockaddr_in& a) {
    char ip[64] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(a.sin_port));
    return std::string(buf);
}
}  // namespace

CNetworkThread::CNetworkThread() = default;

CNetworkThread::~CNetworkThread() {
    Stop();
    if (_sock != static_cast<uintptr_t>(INVALID_SOCKET)) closesocket(static_cast<SOCKET>(_sock));
    if (_sockEvent) WSACloseEvent(static_cast<WSAEVENT>(_sockEvent));
    if (_wakeEvent) CloseHandle(static_cast<HANDLE>(_wakeEvent));
    if (_eventsReady) CloseHandle(static_cast<HANDLE>(_eventsReady));
    if (_wsaUp) WSACleanup();
}

bool CNetworkThread::Open(const std::string& ip, uint16_t port, std::string& err) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup 실패"; return false; }
    _wsaUp = true;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { err = "소켓 실패"; return false; }
    _sock = static_cast<uintptr_t>(s);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip.empty()) addr.sin_addr.s_addr = INADDR_ANY;
    else inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        char m[160];
        std::snprintf(m, sizeof(m), "bind 실패 (%d). 포트 %d 를 이미 누가 쓰고 있는가?", WSAGetLastError(), port);
        err = m;
        return false;
    }
    sockaddr_in got = {};
    int gl = sizeof(got);
    getsockname(s, reinterpret_cast<sockaddr*>(&got), &gl);
    _port = ntohs(got.sin_port);
    // 소켓 읽기 이벤트 — `WSAEventSelect` 가 소켓을 막지 않는 모드로 바꾼다(받을 것을 `WSAEWOULDBLOCK` 까지 비운다)
    _sockEvent = WSACreateEvent();
    if (WSAEventSelect(s, static_cast<WSAEVENT>(_sockEvent), FD_READ) == SOCKET_ERROR) { err = "WSAEventSelect 실패"; return false; }
    _wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 자동으로 되돌아가는 이벤트
    _eventsReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // 같은 꼴 — 게임 루프가 기다린다
    return true;
}

bool CNetworkThread::OpenLobby(uint32_t bindIp, uint16_t port, std::string& err) {
    if (_thread.joinable()) { err = "시작한 뒤에는 로비를 열 수 없다"; return false; }
    if (_lobbyPort != 0) { err = "로비에 붙기와 로비 듣기는 같이 못 쓴다"; return false; }
    if (!_lobby.Listen(bindIp, port, err)) return false;
    _lobbyOpen = true;
    return true;
}

bool CNetworkThread::ConnectLobby(uint32_t ip, uint16_t port, std::string& err) {
    if (_thread.joinable()) { err = "시작한 뒤에는 로비에 붙기를 정할 수 없다"; return false; }
    if (_lobbyOpen) { err = "로비 듣기와 로비에 붙기는 같이 못 쓴다"; return false; }
    if (ip == 0 || port == 0) { err = "로비 주소가 0"; return false; }
    _lobbyIp = ip;
    _lobbyPort = port;
    _lobbyTryAt = NowMs();   // 시작하자마자 붙는다
    return true;
}

void CNetworkThread::LobbyConnectTick(uint32_t now, std::vector<NetworkEvent>& events) {
    if (_lobbyPort == 0) return;
    if (_lobbyConn == 0) {
        if (static_cast<int32_t>(now - _lobbyTryAt) < 0) return;   // 다음 시도 전(32비트 시계가 돌아도 맞게)
        ++_threadStats.lobbyConnects;
        std::string err;
        _lobbyConn = _lobby.Connect(_lobbyIp, _lobbyPort, err);
        _lobbyConnStartMs = now;
        if (_lobbyConn == 0) {   // 곧바로 실패(소켓 · 한도) — 사건이 안 오므로 여기서 센다
            ++_threadStats.lobbyConnectFailed;
            _lobbyTryAt = now + _lobbyRetryMs;
        }
        return;
    }
    if (!_lobbyUp && now - _lobbyConnStartMs >= kLobbyConnectTimeoutMs) {   // 붙는 중 한도 — 접고(끊김 사건 → 못 붙음 셈) 다시
        std::vector<TcpLineEvent> tcpEvs;
        _lobby.Close(_lobbyConn, tcpEvs);
        LobbyToEvents(tcpEvs, events);
    }
}

void CNetworkThread::LobbyToEvents(const std::vector<TcpLineEvent>& in, std::vector<NetworkEvent>& out) {
    for (const TcpLineEvent& e : in) {
        NetworkEvent ev;
        ev.lobbyConn = e.conn;
        // ★ 10-20 v2 작업 4a — 로비에 붙은 접속 — 붙기 전 끊김(못 붙음)은 게임에 안 넘기고 세고 다시 시도를 잡는다
        if (e.outbound && e.conn == _lobbyConn) {
            if (e.type == TcpLineEvent::Type::OPENED) _lobbyUp = true;
            if (e.type == TcpLineEvent::Type::CLOSED) {
                const bool wasUp = _lobbyUp;
                _lobbyConn = 0;
                _lobbyUp = false;
                _lobbyTryAt = NowMs() + _lobbyRetryMs;
                if (!wasUp) {
                    ++_threadStats.lobbyConnectFailed;
                    continue;
                }
            }
        }
        switch (e.type) {
            case TcpLineEvent::Type::OPENED:
                ++_threadStats.lobbyOpened;
                ev.type = NetworkEvent::Type::LOBBY_OPENED;
                ev.lobbyPeerIp = e.peerIp;
                ev.lobbyLocalIp = e.localIp;
                ev.addr = Ipv4ToString(e.peerIp) + ":tcp" + std::to_string(e.port);   // 기록용(세션 주소와 겹치지 않게)
                break;
            case TcpLineEvent::Type::LINE:
                ++_threadStats.lobbyLinesIn;
                ev.type = NetworkEvent::Type::LOBBY_LINE;
                ev.lobbyLine = e.line;
                break;
            case TcpLineEvent::Type::CLOSED:
                ++_threadStats.lobbyClosed;
                ev.type = NetworkEvent::Type::LOBBY_CLOSED;
                ev.lobbyWhy = e.why;
                break;
            case TcpLineEvent::Type::REFUSED:
                ++_threadStats.lobbyRefused;   // 접속 번호가 없어 게임에 넘길 것이 없다
                continue;
        }
        out.push_back(std::move(ev));
    }
}

void CNetworkThread::Start() {
    _stop.store(false);
    _thread = std::thread([this] { Run(); });
}

void CNetworkThread::Stop(bool closeSessions) {
    if (!_thread.joinable()) return;
    _closeOnStop.store(closeSessions);
    _stop.store(true);
    SetEvent(static_cast<HANDLE>(_wakeEvent));
    _thread.join();
}

void CNetworkThread::PushRequests(std::vector<NetworkRequest>& reqs) {
    if (reqs.empty()) return;
    {
        std::lock_guard<std::mutex> lk(_queueMu);
        for (NetworkRequest& r : reqs) {
            if (_requests.size() >= _queueMax) { ++_requestsDropped; continue; }
            _requests.push_back(std::move(r));
        }
    }
    reqs.clear();
    SetEvent(static_cast<HANDLE>(_wakeEvent));
}

void CNetworkThread::SwapOutNetworkEvents(std::vector<NetworkEvent>& into) {
    std::lock_guard<std::mutex> lk(_queueMu);
    into.swap(_events);
    _events.clear();
}

CUdpStats CNetworkThread::LatestNetworkStats() const {
    std::lock_guard<std::mutex> lk(_statsMu);
    return _pubNetwork;
}

CNetworkThreadStats CNetworkThread::LatestThreadStats() const {
    std::lock_guard<std::mutex> lk(_statsMu);
    return _pubThread;
}

void CNetworkThread::Publish() {
    CNetworkThreadStats t = _threadStats;
    {
        std::lock_guard<std::mutex> lk(_queueMu);
        t.requestsDropped = _requestsDropped;
        t.eventsDropped = _eventsDropped;
        t.requestsQueued = _requests.size();
        t.eventsQueued = _events.size();
    }
    CUdpStats n = _network.Stats();
    std::lock_guard<std::mutex> lk(_statsMu);
    _pubNetwork = std::move(n);
    _pubThread = t;
}

void CNetworkThread::Run() {
    const SOCKET s = static_cast<SOCKET>(_sock);
    // ★ 10-14 작업 8c — 로비를 열었으면 TCP 일 이벤트도 같이 기다린다 · ★ 10-20 v2 작업 4a — 로비에 붙을 때도
    WSAEVENT evs[3] = { static_cast<WSAEVENT>(_sockEvent), static_cast<WSAEVENT>(_wakeEvent), static_cast<WSAEVENT>(_lobby.WaitHandle()) };
    const bool lobbyTcp = _lobbyOpen || _lobbyPort != 0;
    const DWORD nEvs = lobbyTcp ? 3 : 2;
    std::map<std::string, sockaddr_in> addrOf;   // 주소 글자 → 진짜 주소(이 스레드 것)
    std::vector<OutPacket> out;
    std::vector<NetworkEvent> events;
    std::vector<NetworkRequest> reqs;
    std::vector<TcpLineEvent> tcpEvs;
    std::set<uint8_t> told;                      // `[처음]` — 접속 제어 오프코드(게임 오프코드는 게임 스레드가 찍는다)
    uint8_t in[2048];
    Publish();
    for (;;) {
        // 기다리는 시간 — 타이머 맨 앞 마감까지(없으면 `_maxWaitMs` — 기본 `kMaxWaitMs`)
        DWORD wait = _maxWaitMs;
        uint32_t due = 0;
        if (_network.NextTimerDue(due)) {
            const int32_t d = static_cast<int32_t>(due - NowMs());
            wait = d <= 0 ? 0 : std::min<DWORD>(static_cast<DWORD>(d), _maxWaitMs);
        }
        // ★ 10-20 v2 작업 4a — 로비 다시 붙기 · 붙는 시간 한도에 맞춰 깬다(붙어 있으면 기다릴 시각 없음)
        if (_lobbyPort != 0 && !_lobbyUp) {
            const uint32_t at = _lobbyConn == 0 ? _lobbyTryAt : _lobbyConnStartMs + kLobbyConnectTimeoutMs;
            const int32_t d = static_cast<int32_t>(at - NowMs());
            wait = d <= 0 ? 0 : std::min<DWORD>(static_cast<DWORD>(d), wait);
        }
        WSAWaitForMultipleEvents(nEvs, evs, FALSE, wait, FALSE);
        // ★ 10-09 조각 5d-2c — 멈추라면 마지막으로 한 번 — 쌓인 요청을 보내고(닫기면 `SERVER_CLOSE` 까지) 끝낸다. 받기 · 타이머는 안 한다
        const bool stopping = _stop.load();
        ++_threadStats.wakes;
        WSANETWORKEVENTS ne;
        WSAEnumNetworkEvents(s, evs[0], &ne);    // 소켓 이벤트를 되돌린다(받을 것은 아래에서 다 비운다)
        const uint32_t now = NowMs();

        // ① 시계 — 마감 지난 타이머(다시 보내기 · 시간 초과 알림 · 접속 전 걷기)
        if (!stopping) _network.Tick(now, out, events);
        // ② 쌓인 요청 — 게임이 지난 틱에 만든 것이라 이번에 받은 것보다 먼저(`CSyncHost` 의 「받기 앞 요청」과 같은 차례)
        {
            std::lock_guard<std::mutex> lk(_queueMu);
            reqs.swap(_requests);
        }
        for (const NetworkRequest& r : reqs) {
            if (r.type == NetworkRequest::Type::LOBBY_SEND) {   // ★ 10-14 작업 8c — 로비 줄은 TCP 로(통신 층 몫이 아니다)
                if (_lobby.Send(r.lobbyConn, r.lobbyLine, r.lobbyCloseAfter)) ++_threadStats.lobbyLinesOut;
                else ++_threadStats.lobbySendNoConn;
                continue;
            }
            _network.OnRequest(r, out);
        }
        reqs.clear();
        // ★ 10-14 작업 8c — 로비 줄을 보낸다(막히면 남겨 두고 TCP 일 이벤트에 다시) · 보낸 뒤 끊기(QUIT 답)는 끊김 이벤트로
        if (lobbyTcp) {
            tcpEvs.clear();
            _lobby.Flush(tcpEvs);
            LobbyToEvents(tcpEvs, events);
        }
        if (!stopping) LobbyConnectTick(now, events);   // ★ 10-20 v2 작업 4a
        // ③ 받을 것 전부(멈출 때는 안 받는다)
        unsigned long long got = 0;
        while (!stopping) {
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            const int n = recvfrom(s, reinterpret_cast<char*>(in), sizeof(in), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n == SOCKET_ERROR) {
                const int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                ++_threadStats.recvErrors;           // 닫힌 포트 알림(WSAECONNRESET) 등 — 다음 것을 계속 받는다
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
                break;
            }
            if (n <= 0) continue;
            ++got;
            ++_threadStats.datagramsIn;
            const std::string who = KeyOf(from);
            addrOf[who] = from;
            const size_t evBefore = events.size(), outBefore = out.size();
            const size_t ctl = _network.OnDatagram(who, in, static_cast<size_t>(n), out, events);
            // ★★ **클라가 처음 보낸 오프코드를 그때그때 알린다**(옛 `main` 의 같은 줄 — 접속 제어 쪽. ⚠ `--quiet` 여도 찍는다)
            for (const auto& kv : _network.seen()) {
                if (!told.insert(kv.first).second) continue;
                std::printf("[처음] 클라가 보낸 새 오프코드 0x%02x (%s)\n", kv.first, OpName(static_cast<Op>(kv.first)));
                std::fflush(stdout);
            }
            if (_verbose) {
                const size_t passed = events.size() - evBefore, made = out.size() - outBefore;
                // ★ 10-09 — 여러 번에 나눠 만드는 줄은 다 만든 뒤 한 번에 찍는다(나눠 찍으면 게임 스레드의 줄이 그 사이에 낀다)
                char b[64];
                // 접속 제어도 못 읽고 게임에도 안 넘겼으면 그 바이트를 찍는다(옛 `[못 읽음]`)
                if (ctl == 0 && passed == 0 && n > 3) {
                    std::string line = "  [못 읽음]";
                    for (int i = 0; i < n && i < 32; ++i) {
                        std::snprintf(b, sizeof(b), " %02x", in[i]);
                        line += b;
                    }
                    std::printf("%s\n", line.c_str());
                }
                // ★ 10-09 조각 5d-2b — 게임 메시지 수는 게임 스레드가 다음 틱에 읽으므로 여기서는 「게임으로 넘김」만 센다
                std::printf("[받음 %llu] %s  %d바이트 → 접속 제어 %zu개 · 게임으로 넘김 %zu · 보낸 조각 %zu개\n",
                            _threadStats.datagramsIn, who.c_str(), n, ctl, passed, made);
                // 뜯었는데 답이 없으면 무엇을 봤는지 · 열쇠 상태(옛 `main` 의 같은 줄 — 복호가 어긋나면 쓰레기가 오프코드로 보인다)
                if (ctl > 0 && made == 0 && passed == 0 && n > 3) {
                    std::string line = "     ↳ 본 것:";
                    for (uint8_t o : _network.LastFragOps()) {
                        std::snprintf(b, sizeof(b), " %02x(%s)", o, OpName(static_cast<Op>(o)));
                        line += b;
                    }
                    std::snprintf(b, sizeof(b), "  · 열쇠 %s 0x%02x", _network.IsKeyed(who) ? "있음" : "없음", _network.KeyOf(who));
                    line += b;
                    std::printf("%s\n", line.c_str());
                }
                std::fflush(stdout);
            }
        }
        if (got > _threadStats.maxDatagramsPerWake) _threadStats.maxDatagramsPerWake = got;
        // ③.5 ★ 10-14 작업 8c — 로비 TCP — 새 접속 · 받은 줄 · 끊김(멈출 때는 안 받는다)
        if (lobbyTcp && !stopping) {
            tcpEvs.clear();
            _lobby.Poll(tcpEvs);
            LobbyToEvents(tcpEvs, events);
        }
        // ④ 이벤트를 큐로 — 넣은 시각을 찍는다(게임 스레드가 넘김 지연을 잰다)
        if (!events.empty()) {
            bool queued = false;
            {
                std::lock_guard<std::mutex> lk(_queueMu);
                for (NetworkEvent& ev : events) {
                    if (_events.size() >= _queueMax) { ++_eventsDropped; continue; }
                    ev.queuedMs = now;
                    _events.push_back(std::move(ev));
                    queued = true;
                }
            }
            events.clear();
            // ★ 10-09 플랜 A 5d 자가검증 ② — 게임 루프를 깨운다(틱 마감 전이라도 받은 것을 바로 처리)
            if (queued) SetEvent(static_cast<HANDLE>(_eventsReady));
        }
        // ★ 10-09 조각 5d-2c — 닫기면 키를 준 상대마다 `SERVER_CLOSE`(쌓인 요청 뒤에)
        if (stopping && _closeOnStop.load()) _network.CloseAll(out);
        // ⑤ 나갈 것 전부(주소를 모르는 상대 — 소켓 없는 봇 — 는 건너뛴다)
        for (const OutPacket& p : out) {
            auto it = addrOf.find(p.to);
            if (it == addrOf.end()) continue;
            sendto(s, reinterpret_cast<const char*>(p.data.data()), static_cast<int>(p.data.size()), 0,
                   reinterpret_cast<sockaddr*>(&it->second), sizeof(it->second));
            ++_threadStats.datagramsOut;
        }
        out.clear();
        if (stopping || static_cast<uint32_t>(now - _lastPublishMs) >= kMaxWaitMs) {   // 지표 사본은 기다림 설정과 따로 0.25초마다 · 멈출 때 한 번 더
            _lastPublishMs = now;
            Publish();
        }
        if (stopping) {
            _lobby.CloseAll();   // ★ 10-14 작업 8c — 남은 줄은 위 `Flush` 에서 보냈다. 로비 접속을 닫는다(게임 스레드는 이미 멈춤)
            break;
        }
    }
}

}  // namespace nox
