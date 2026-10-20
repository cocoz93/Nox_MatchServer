// tcp_lines_test.cpp — TCP 줄 부품(`nox::CTcpLines` — 작업 8a · ★ 10-20 v2 작업 2 바깥으로 붙기) 시험. 루프백 TCP 로 실제로 주고받는다.
//   `common_test.cpp` 와 따로 둔 까닭 — winsock(windows.h)의 `far` 매크로가 그쪽 토큰 시험 본문과 부딪힌다(`net_thread_test.cpp` 와 같은 꼴).
//   결과는 (통과, 한 줄) 목록으로 돌려주고, `common_test.cpp` 의 `TestTcpLines` 가 `Check` 로 센다.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nox/TcpLines.h"

namespace {

std::vector<std::pair<bool, std::string>>* g_out = nullptr;
void Check(bool ok, const char* what) { g_out->emplace_back(ok, what); }
// ── ⑦ TCP 줄 부품 — 상대 쪽은 막는 소켓(시험이 기다리는 쪽) ──
// `fromIp` 0 이 아니면 그 주소에서 붙는다 — 상대 주소와 내 쪽 주소를 다르게 해 둘이 뒤바뀌면 걸리게(127.0.0.2 → 127.0.0.1)
SOCKET ConnectLoop(uint16_t port, uint32_t fromIp = 0) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fromIp) {
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_addr.s_addr = htonl(fromIp);
        bind(s, reinterpret_cast<sockaddr*>(&b), sizeof(b));
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001u);
    a.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD ms = 2000;   // 받기가 영영 막히지 않게
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    return s;
}

void SendRaw(SOCKET s, const std::string& b) {
    size_t p = 0;
    while (p < b.size()) {
        const int n = send(s, b.data() + p, static_cast<int>(b.size() - p), 0);
        if (n <= 0) return;
        p += static_cast<size_t>(n);
    }
}

// 상대가 끊을 때까지(또는 2초) 받은 것 전부
std::string RecvAll(SOCKET s) {
    std::string r;
    char buf[512];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return r;
        r.append(buf, static_cast<size_t>(n));
    }
}

// 사건이 `want` 개 쌓일 때까지 이벤트를 기다리며 훑는다(2초 한도). 기다린 이벤트가 한 번이라도 켜졌나를 돌려준다
bool PollUntil(nox::CTcpLines& t, std::vector<nox::TcpLineEvent>& ev, size_t want) {
    bool signaled = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ev.size() < want && std::chrono::steady_clock::now() < until) {
        if (WaitForSingleObject(static_cast<HANDLE>(t.WaitHandle()), 50) == WAIT_OBJECT_0) signaled = true;
        t.Poll(ev);
    }
    return signaled;
}

void TestTcpLines() {
    using E = nox::TcpLineEvent;
    nox::CTcpLines t;
    std::string err;
    Check(t.Listen(0x7f000001u, 0, err) && t.ListenPort(0) != 0, "루프백 듣기(포트는 OS 가 고름)");
    const uint16_t port = t.ListenPort(0);
    std::vector<E> ev;

    SOCKET c = ConnectLoop(port, 0x7f000002u);
    Check(PollUntil(t, ev, 1), "새 접속에 이벤트가 켜짐");
    Check(ev.size() == 1 && ev[0].type == E::Type::OPENED && ev[0].port == port && ev[0].peerIp == 0x7f000002u &&
              ev[0].localIp == 0x7f000001u && t.ConnCount() == 1,
          "새 접속 = OPENED 하나 · 듣기 포트 · 상대 127.0.0.2 · 내 쪽 127.0.0.1");
    int conn = ev.empty() ? 0 : ev[0].conn;
    ev.clear();
    t.Poll(ev);
    Check(ev.empty() && WaitForSingleObject(static_cast<HANDLE>(t.WaitHandle()), 0) == WAIT_TIMEOUT,
          "할 일 없이 훑은 뒤엔 이벤트가 꺼짐(안 꺼지면 기다리는 쪽이 헛돈다)");

    SendRaw(c, "A b\r\nC\nD");
    PollUntil(t, ev, 2);
    Check(ev.size() == 2 && ev[0].type == E::Type::LINE && ev[0].line == "A b" && ev[0].conn == conn && ev[1].line == "C",
          "CRLF · LF 둘 다 줄 · 끝 CR 뗌 · 안 끝난 줄은 남김");
    ev.clear();
    SendRaw(c, "E\r\n");
    PollUntil(t, ev, 1);
    Check(ev.size() == 1 && ev[0].line == "DE", "나뉘어 온 줄을 이어 붙임");
    ev.clear();

    Check(t.Send(conn, "x 1") && t.Send(conn, "y", /*closeAfter=*/true) && !t.Send(9999, "z"), "보내기 버퍼 · 없는 접속은 false");
    t.Flush(ev);
    Check(RecvAll(c) == "x 1\r\ny\r\n", "상대가 받은 바이트 = 줄마다 CRLF · 그 뒤 끊김");
    Check(ev.size() == 1 && ev[0].type == E::Type::CLOSED && ev[0].conn == conn &&
              std::strcmp(ev[0].why, nox::CTcpLines::kWhyCloseAfter) == 0 && !t.Has(conn),
          "보낸 뒤 끊기 → CLOSED 한 번(까닭 「보낸 뒤 끊기」) · 접속 사라짐");
    closesocket(c);
    ev.clear();

    c = ConnectLoop(port);
    PollUntil(t, ev, 1);
    conn = ev.empty() ? 0 : ev[0].conn;
    ev.clear();
    closesocket(c);
    PollUntil(t, ev, 1);
    Check(ev.size() == 1 && ev[0].type == E::Type::CLOSED && ev[0].conn == conn &&
              std::strcmp(ev[0].why, nox::CTcpLines::kWhyPeerClosed) == 0,
          "상대가 닫음 → CLOSED(까닭 「상대가 닫음」)");
    ev.clear();

    c = ConnectLoop(port);
    PollUntil(t, ev, 1);
    ev.clear();
    SendRaw(c, std::string(nox::CTcpLines::kMaxLine + 10, 'a'));
    PollUntil(t, ev, 1);
    Check(ev.size() == 1 && ev[0].type == E::Type::CLOSED && std::strcmp(ev[0].why, nox::CTcpLines::kWhyLineTooLong) == 0,
          "줄바꿈 없이 4096 바이트 넘게 → 끊음(줄 사건 없음)");
    closesocket(c);
    ev.clear();

    c = ConnectLoop(port);
    PollUntil(t, ev, 1);
    conn = ev.empty() ? 0 : ev[0].conn;
    ev.clear();
    Check(t.Send(conn, "버려질 줄") && t.Close(conn, ev) && ev.size() == 1 &&
              std::strcmp(ev[0].why, nox::CTcpLines::kWhyLocal) == 0 && !t.Close(conn, ev),
          "이쪽에서 닫음 → CLOSED 한 번 · 두 번째는 false");
    Check(RecvAll(c).empty(), "이쪽에서 닫으면 보내기 버퍼는 버림");
    closesocket(c);
    Check(t.ConnCount() == 0, "남은 접속 0");
}

// ── ★ 10-20 MO 6단계 v2 작업 2 — 바깥으로 붙기(`Connect`). 두 부품(듣는 쪽 · 붙는 쪽)을 같이 훑는다 ──
size_t CountType(const std::vector<nox::TcpLineEvent>& ev, nox::TcpLineEvent::Type t) {
    size_t n = 0;
    for (const auto& e : ev) n += e.type == t ? 1 : 0;
    return n;
}

const nox::TcpLineEvent* FindType(const std::vector<nox::TcpLineEvent>& ev, nox::TcpLineEvent::Type t) {
    for (const auto& e : ev)
        if (e.type == t) return &e;
    return nullptr;
}

// 이벤트 둘 중 하나를 기다리고 둘 다 훑고 보낸다 — `done` 이 참이거나 `ms` 가 지나면 끝
template <class F>
void PollBoth(nox::CTcpLines& a, nox::CTcpLines& b, std::vector<nox::TcpLineEvent>& ea, std::vector<nox::TcpLineEvent>& eb, F done,
              int ms = 2000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    HANDLE h[2] = {static_cast<HANDLE>(a.WaitHandle()), static_cast<HANDLE>(b.WaitHandle())};
    while (!done() && std::chrono::steady_clock::now() < until) {
        WaitForMultipleObjects(2, h, FALSE, 50);
        a.Poll(ea);
        b.Poll(eb);
        a.Flush(ea);
        b.Flush(eb);
    }
}

void TestConnect() {
    using E = nox::TcpLineEvent;
    nox::CTcpLines srv, cli;
    std::string err;
    Check(srv.Listen(0x7f000001u, 0, err), "붙기 — 듣는 쪽 열기");
    const uint16_t port = srv.ListenPort(0);
    std::vector<E> es, ec;

    const int id = cli.Connect(0x7f000001u, port, err);
    Check(id != 0 && cli.Has(id) && cli.Send(id, "early"), "붙기 시작 → 접속 번호 · 붙는 중에도 보내기 버퍼에 쌓음");
    PollBoth(srv, cli, es, ec, [&] { return CountType(ec, E::Type::OPENED) && CountType(es, E::Type::LINE); });
    const E* co = FindType(ec, E::Type::OPENED);
    Check(co && ec.size() == 1 && co->conn == id && co->outbound && co->port == port && co->peerIp == 0x7f000001u &&
              co->localIp == 0x7f000001u,
          "붙음 → OPENED 하나(바깥 · 상대 포트 · 상대 127.0.0.1 · 내 쪽 127.0.0.1)");
    const E* so = FindType(es, E::Type::OPENED);
    const E* sl = FindType(es, E::Type::LINE);
    Check(so && !so->outbound && sl && sl->line == "early" && !sl->outbound, "붙는 중 쌓은 줄이 붙은 뒤 상대에 감(듣는 쪽 사건은 바깥 아님)");
    const int sc = so ? so->conn : 0;
    es.clear();
    ec.clear();

    Check(srv.Send(sc, "from srv") && cli.Send(id, "from cli"), "양쪽 보내기");
    PollBoth(srv, cli, es, ec, [&] { return CountType(ec, E::Type::LINE) && CountType(es, E::Type::LINE); });
    const E* cl = FindType(ec, E::Type::LINE);
    sl = FindType(es, E::Type::LINE);
    Check(cl && cl->line == "from srv" && cl->conn == id && cl->outbound && sl && sl->line == "from cli",
          "줄 왕복 — 붙은 쪽 LINE 도 바깥 표시");
    es.clear();
    ec.clear();

    srv.Close(sc, es);
    PollBoth(srv, cli, es, ec, [&] { return CountType(ec, E::Type::CLOSED) != 0; });
    const E* cc = FindType(ec, E::Type::CLOSED);
    Check(cc && ec.size() == 1 && cc->conn == id && cc->outbound && std::strcmp(cc->why, nox::CTcpLines::kWhyPeerClosed) == 0 &&
              !cli.Has(id),
          "상대가 닫음 → CLOSED(바깥 · 「상대가 닫음」) · 접속 사라짐");
    es.clear();
    ec.clear();

    // 아무도 안 듣는 포트 — 잠깐 열었다 닫은 포트(윈도 루프백은 거절을 몇 번 다시 시도해 1~2초 걸린다)
    uint16_t deadPort = 0;
    {
        nox::CTcpLines tmp;
        std::string e2;
        if (tmp.Listen(0x7f000001u, 0, e2)) deadPort = tmp.ListenPort(0);
    }
    // 붙는 동안 줄을 쌓아 두고 `Flush` 를 여러 번 — 붙기 전 보내기를 건너뛰어야 까닭이 「보내기 오류」가 아니라 「붙지 못함」
    const int id2 = cli.Connect(0x7f000001u, deadPort, err);
    cli.Send(id2, "never");
    PollBoth(srv, cli, es, ec, [&] { return CountType(ec, E::Type::CLOSED) != 0; }, 8000);
    cc = FindType(ec, E::Type::CLOSED);
    Check(deadPort && id2 != 0 && cc && ec.size() == 1 && cc->conn == id2 && cc->outbound &&
              std::strcmp(cc->why, nox::CTcpLines::kWhyConnectFailed) == 0 && !cli.Has(id2),
          "듣는 이 없는 포트(줄 쌓아 둠) → CLOSED 「붙지 못함」 하나(OPENED 없음 · 보내기 오류 아님)");
    ec.clear();

    const int id3 = cli.Connect(0x7f000001u, deadPort, err);
    Check(id3 != 0 && cli.Send(id3, "lost") && cli.Close(id3, ec) && ec.size() == 1 &&
              std::strcmp(ec[0].why, nox::CTcpLines::kWhyLocal) == 0 && ec[0].outbound,
          "붙는 중 이쪽에서 닫음 → CLOSED 「이쪽에서 닫음」 하나");
    PollBoth(srv, cli, es, ec, [] { return false; }, 300);
    Check(ec.size() == 1 && es.empty(), "닫은 뒤 그 접속 사건 없음 · 듣는 쪽엔 아무것도 안 옴");

    std::string err0, errP;
    Check(cli.Connect(0, port, err0) == 0 && !err0.empty() && cli.Connect(0x7f000001u, 0, errP) == 0 && !errP.empty() &&
              cli.ConnCount() == 0,
          "주소 0.0.0.0 · 포트 0 → 곧바로 0 · 까닭 · 접속 안 남음");
}

}  // namespace

std::vector<std::pair<bool, std::string>> RunTcpLinesChecks() {
    std::vector<std::pair<bool, std::string>> r;
    g_out = &r;
    TestTcpLines();
    TestConnect();   // ★ 10-20 v2 작업 2
    g_out = nullptr;
    return r;
}