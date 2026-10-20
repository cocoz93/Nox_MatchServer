// TcpLines.cpp — TCP 줄 서버 부품. 설명은 `TcpLines.h` 머리.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "nox/TcpLines.h"

#pragma comment(lib, "ws2_32.lib")

namespace nox {

namespace {
constexpr long kConnEvents = FD_READ | FD_WRITE | FD_CLOSE;
}  // namespace

CTcpLines::CTcpLines() {
    WSADATA wsa;
    _wsaUp = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    _event = WSACreateEvent();   // 수동으로 꺼지는 이벤트 — `Poll` 이 소켓을 훑기 전에 끈다
}

CTcpLines::~CTcpLines() {
    CloseAll();
    if (_event) WSACloseEvent(static_cast<WSAEVENT>(_event));
    if (_wsaUp) WSACleanup();
}

bool CTcpLines::Listen(uint32_t bindIp, uint16_t port, std::string& err) {
    if (!_wsaUp || !_event) { err = "WSAStartup 실패"; return false; }
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "소켓 실패"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(bindIp);
    a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR || listen(s, SOMAXCONN) == SOCKET_ERROR) {
        err = std::to_string(port) + " 포트를 열 수 없다 (WSA " + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return false;
    }
    // 이벤트에 걸면 소켓이 막지 않는 모드가 된다 — 받을 접속을 `WSAEWOULDBLOCK` 까지 비운다
    if (WSAEventSelect(s, static_cast<WSAEVENT>(_event), FD_ACCEPT) == SOCKET_ERROR) {
        err = "WSAEventSelect 실패 (WSA " + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return false;
    }
    sockaddr_in got{};
    int gl = sizeof(got);
    getsockname(s, reinterpret_cast<sockaddr*>(&got), &gl);
    Listener l;
    l.s = static_cast<uintptr_t>(s);
    l.port = ntohs(got.sin_port);
    _listeners.push_back(l);
    return true;
}

void CTcpLines::Poll(std::vector<TcpLineEvent>& out) {
    // 훑기 전에 끈다 — 훑는 동안 새로 생긴 일은 이벤트를 다시 켜 다음 기다림이 바로 돌아온다.
    //   소켓마다 `WSAEnumNetworkEvents` 로 끄면, 이벤트 하나를 같이 쓰는 다른 소켓에 막 생긴 일이 꺼진 채 남을 수 있다
    WSAResetEvent(static_cast<WSAEVENT>(_event));
    for (const Listener& l : _listeners) Accept(l, out);
    std::vector<int> ids;
    ids.reserve(_conns.size());
    for (const auto& kv : _conns) ids.push_back(kv.first);
    for (const int id : ids) {
        CheckConnect(id, out);   // ★ 10-20 — 붙는 중이면 붙었나부터(붙었으면 이 바퀴에 바로 받는다)
        Drain(id, out);
    }
}

int CTcpLines::Connect(uint32_t ip, uint16_t port, std::string& err) {
    if (!_wsaUp || !_event) { err = "WSAStartup 실패"; return 0; }
    if (_conns.size() >= kMaxConns) { err = "접속 수 한도"; return 0; }
    // 0.0.0.0 · 포트 0 은 갈 곳이 아니다 — 막지 않는 `connect` 는 이것도 곧바로 거절하지 않아(10-20 시험) 여기서 거른다
    if (ip == 0 || port == 0) { err = "갈 곳이 아닌 주소(0.0.0.0 · 포트 0)"; return 0; }
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "소켓 실패"; return 0; }
    // 이벤트에 걸면 막지 않는 모드가 된다 — `connect` 는 곧바로 `WSAEWOULDBLOCK` 으로 돌아오고, 끝나면 `FD_CONNECT` 가 이벤트를 켠다
    if (WSAEventSelect(s, static_cast<WSAEVENT>(_event), kConnEvents | FD_CONNECT) == SOCKET_ERROR) {
        err = "WSAEventSelect 실패 (WSA " + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        err = "붙기 실패 (WSA " + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return 0;
    }
    const int id = _nextConn++;
    Conn c;
    c.s = static_cast<uintptr_t>(s);
    c.port = port;
    c.outbound = true;
    c.connecting = true;
    c.peerIp = ip;
    _conns[id] = c;
    return id;
}

void CTcpLines::CheckConnect(int conn, std::vector<TcpLineEvent>& out) {
    auto it = _conns.find(conn);
    if (it == _conns.end() || !it->second.connecting) return;
    Conn& c = it->second;
    const SOCKET s = static_cast<SOCKET>(c.s);
    // 막지 않는 붙기의 끝 — 윈도는 붙으면 쓰기 쪽, 못 붙으면 예외 쪽에 낸다(`select` 문서 「connect 실패」)
    fd_set w, x;
    FD_ZERO(&w);
    FD_ZERO(&x);
    FD_SET(s, &w);
    FD_SET(s, &x);
    timeval tv{0, 0};
    if (select(0, nullptr, &w, &x, &tv) <= 0) return;   // 아직 붙는 중
    if (FD_ISSET(s, &x)) { Drop(conn, kWhyConnectFailed, out); return; }
    if (!FD_ISSET(s, &w)) return;
    c.connecting = false;
    sockaddr_in local{};
    int llen = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
    TcpLineEvent e;
    e.type = TcpLineEvent::Type::OPENED;
    e.conn = conn;
    e.port = c.port;
    e.outbound = true;
    e.peerIp = c.peerIp;
    e.localIp = ntohl(local.sin_addr.s_addr);
    out.push_back(e);
}

void CTcpLines::Accept(const Listener& l, std::vector<TcpLineEvent>& out) {
    for (;;) {
        sockaddr_in peer{};
        int plen = sizeof(peer);
        const SOCKET s = accept(static_cast<SOCKET>(l.s), reinterpret_cast<sockaddr*>(&peer), &plen);
        if (s == INVALID_SOCKET) return;   // 받을 것이 없음(WSAEWOULDBLOCK) · 오류 — 다음 기다림에
        if (_conns.size() >= kMaxConns) {
            closesocket(s);
            TcpLineEvent e;
            e.type = TcpLineEvent::Type::REFUSED;
            e.port = l.port;
            out.push_back(e);
            continue;
        }
        if (WSAEventSelect(s, static_cast<WSAEVENT>(_event), kConnEvents) == SOCKET_ERROR) {
            closesocket(s);
            continue;
        }
        sockaddr_in local{};
        int llen = sizeof(local);
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
        const int id = _nextConn++;
        Conn c;
        c.s = static_cast<uintptr_t>(s);
        c.port = l.port;
        _conns[id] = c;
        TcpLineEvent e;
        e.type = TcpLineEvent::Type::OPENED;
        e.conn = id;
        e.port = l.port;
        e.peerIp = ntohl(peer.sin_addr.s_addr);
        e.localIp = ntohl(local.sin_addr.s_addr);
        out.push_back(e);
    }
}

void CTcpLines::Drain(int conn, std::vector<TcpLineEvent>& out) {
    auto it = _conns.find(conn);
    if (it == _conns.end() || it->second.connecting) return;   // ★ 10-20 — 붙기 전 `recv` 는 오류(WSAENOTCONN)라 건너뛴다
    Conn& c = it->second;
    char buf[4096];
    for (;;) {
        const int n = recv(static_cast<SOCKET>(c.s), buf, sizeof(buf), 0);
        if (n == 0) { Drop(conn, kWhyPeerClosed, out); return; }
        if (n < 0) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
            Drop(conn, kWhyRecvError, out);
            return;
        }
        c.in.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = c.in.find('\n')) != std::string::npos) {
            TcpLineEvent e;
            e.type = TcpLineEvent::Type::LINE;
            e.conn = conn;
            e.port = c.port;
            e.outbound = c.outbound;
            e.line = c.in.substr(0, nl);
            if (!e.line.empty() && e.line.back() == '\r') e.line.pop_back();
            c.in.erase(0, nl + 1);
            out.push_back(std::move(e));
        }
        if (c.in.size() > kMaxLine) { Drop(conn, kWhyLineTooLong, out); return; }
    }
}

bool CTcpLines::Send(int conn, const std::string& line, bool closeAfter) {
    auto it = _conns.find(conn);
    if (it == _conns.end()) return false;
    it->second.out += line;
    it->second.out += "\r\n";
    if (closeAfter) it->second.closeAfter = true;
    return true;
}

void CTcpLines::Flush(std::vector<TcpLineEvent>& out) {
    std::vector<int> ids;
    ids.reserve(_conns.size());
    for (const auto& kv : _conns) ids.push_back(kv.first);
    for (const int id : ids) {
        auto it = _conns.find(id);
        if (it == _conns.end() || it->second.connecting) continue;   // ★ 10-20 — 붙는 동안 쌓인 줄은 붙은 뒤 `Flush` 에서
        Conn& c = it->second;
        // 쓰기 가능 여부와 관계없이 시도 — 막히면 `WSAEWOULDBLOCK` 으로 남기고, 자리가 나면 `FD_WRITE` 가 이벤트를 켠다
        bool failed = false;
        while (!c.out.empty()) {
            const int n = send(static_cast<SOCKET>(c.s), c.out.data(), static_cast<int>(c.out.size()), 0);
            if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
            failed = true;
            break;
        }
        if (failed) { Drop(id, kWhySendError, out); continue; }
        if (c.out.empty() && c.closeAfter) Drop(id, kWhyCloseAfter, out);
    }
}

bool CTcpLines::Close(int conn, std::vector<TcpLineEvent>& out) {
    if (!Has(conn)) return false;
    Drop(conn, kWhyLocal, out);
    return true;
}

void CTcpLines::Drop(int conn, const char* why, std::vector<TcpLineEvent>& out) {
    auto it = _conns.find(conn);
    if (it == _conns.end()) return;
    const uint16_t port = it->second.port;
    const bool outbound = it->second.outbound;
    closesocket(static_cast<SOCKET>(it->second.s));
    _conns.erase(it);
    TcpLineEvent e;
    e.type = TcpLineEvent::Type::CLOSED;
    e.conn = conn;
    e.port = port;
    e.outbound = outbound;
    e.why = why;
    out.push_back(e);
}

void CTcpLines::CloseAll() {
    for (auto& kv : _conns) closesocket(static_cast<SOCKET>(kv.second.s));
    _conns.clear();
    for (const Listener& l : _listeners) closesocket(static_cast<SOCKET>(l.s));
    _listeners.clear();
}

}  // namespace nox
