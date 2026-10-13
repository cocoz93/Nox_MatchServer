// tcp_lines_test.cpp — TCP 줄 부품(`nox::CTcpLines` — 작업 8a) 시험. 루프백 TCP 로 실제로 주고받는다.
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

}  // namespace

std::vector<std::pair<bool, std::string>> RunTcpLinesChecks() {
    std::vector<std::pair<bool, std::string>> r;
    g_out = &r;
    TestTcpLines();
    g_out = nullptr;
    return r;
}