// TcpLines.h — TCP 줄 서버 부품. 로비 규약(Westwood Online — 줄 단위 TCP)을 받는 쪽이 같이 쓴다(`nox_common`).
//
// ★ 10-14 플랜 B 작업 8a(`Docs/ROOMS_DESIGN.md` 목표 구조 v3) — 옛 `lobby_main.cpp` 의 `select` 루프에서 소켓 일을 떼어 냈다.
//   라우터(`nox_router` — 4005)와 채널(`nox_server` 네트워크 스레드 — 4000)이 이것 하나로 TCP 를 받는다.
//
//   · 듣기 소켓 여럿 + 접속들. 모두 이벤트 하나(`WaitHandle`)에 건다 — 부르는 쪽이 그것을 다른 것(UDP 소켓 · 깨우기)과 같이 기다린다
//   · 줄은 LF 로 가르고 끝의 CR 을 뗀다(LF 만 와도 받는다). 보낼 때는 CRLF 를 붙인다
//   · 보내기는 버퍼에 쌓고 `Flush` 에서 보낸다 — 상대가 안 읽어도 부르는 쪽이 막히지 않는다(막히면 다음 `Flush` 에)
//   · 줄을 해석하지 않는다 — 받은 줄을 사건으로 내놓고, 부르는 쪽이 준 줄을 보낼 뿐이다
//
// 한 스레드에서만 부른다(만든 스레드와 같지 않아도 된다).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nox {

// 접속 하나에 일어난 일 — `Poll` · `Flush` 가 일어난 차례대로 덧붙인다
struct TcpLineEvent {
    enum class Type {
        OPENED,    // 새 접속(`port` · `peerIp` · `localIp`)
        LINE,      // 받은 줄 하나(`line` — 줄 끝 CR LF 를 뗀 것)
        CLOSED,    // 끊김(`why`) — 이 뒤로 그 접속 번호는 다시 안 나온다
        REFUSED    // 접속 수 한도라 받자마자 끊음(`port` — 접속 번호 없음)
    };
    Type type = Type::LINE;
    int conn = 0;               // 접속 번호(1부터 · 다시 안 씀)
    uint16_t port = 0;          // 들어온 듣기 포트
    uint32_t peerIp = 0;        // OPENED — 상대 IPv4(a<<24|b<<16|c<<8|d)
    uint32_t localIp = 0;       // OPENED — 상대가 붙은 내 쪽 IPv4(로비 주소를 알려 줄 때 쓴다)
    std::string line;           // LINE
    const char* why = "";       // CLOSED — 까닭(아래 `kWhy*`)
};

class CTcpLines {
public:
    static constexpr size_t kMaxLine = 4096;    // 줄바꿈 없이 이만큼 오면 끊는다 — 규약상 이렇게 긴 줄은 없다(옛 `lobby_main` 값)
    static constexpr size_t kMaxConns = 1024;   // 접속 수 한도 — 넘으면 받자마자 끊는다(`REFUSED`)

    // 끊긴 까닭(`TcpLineEvent::why`) — 갈래마다 하나
    static constexpr const char* kWhyPeerClosed = "상대가 닫음";
    static constexpr const char* kWhyRecvError = "받기 오류";
    static constexpr const char* kWhySendError = "보내기 오류";
    static constexpr const char* kWhyLineTooLong = "줄이 너무 길다";
    static constexpr const char* kWhyCloseAfter = "보낸 뒤 끊기";   // `Send(…, closeAfter)` 의 줄을 다 보냄(로비의 QUIT 답)
    static constexpr const char* kWhyLocal = "이쪽에서 닫음";      // `Close`

    CTcpLines();
    ~CTcpLines();
    CTcpLines(const CTcpLines&) = delete;
    CTcpLines& operator=(const CTcpLines&) = delete;

    // 듣기 하나를 연다(여러 번 부를 수 있다). `bindIp` 0 = 모든 주소 · `port` 0 = OS 가 고름(시험 — `ListenPort`).
    //   실패하면 false · `err` 에 까닭
    bool Listen(uint32_t bindIp, uint16_t port, std::string& err);
    uint16_t ListenPort(size_t i) const { return i < _listeners.size() ? _listeners[i].port : 0; }
    // 소켓 일(새 접속 · 받을 것 · 다시 보낼 수 있음 · 끊김)이 생기면 켜지는 이벤트(WSAEVENT). 수동으로 꺼지는 이벤트라 `Poll` 이 끈다
    void* WaitHandle() const { return _event; }

    // 새 접속 · 받은 줄 · 끊김을 `out` 뒤에 덧붙인다
    void Poll(std::vector<TcpLineEvent>& out);
    // 줄 하나를 보내기 버퍼에(CRLF 를 붙인다). `closeAfter` 면 이 줄까지 다 보낸 뒤 끊는다. 없는 접속이면 false
    bool Send(int conn, const std::string& line, bool closeAfter = false);
    // 쌓인 것을 보낸다(막히면 남겨 둔다). 보내기 오류 · 보낸 뒤 끊기는 `out` 에 `CLOSED` 로
    void Flush(std::vector<TcpLineEvent>& out);
    // 이쪽에서 끊는다(보내기 버퍼는 버린다) — `out` 에 `CLOSED`. 없는 접속이면 false
    bool Close(int conn, std::vector<TcpLineEvent>& out);
    // 듣기 · 접속을 모두 닫는다(사건 없음 — 끝낼 때)
    void CloseAll();

    size_t ConnCount() const { return _conns.size(); }
    bool Has(int conn) const { return _conns.count(conn) != 0; }

private:
    struct Listener {
        uintptr_t s = 0;   // SOCKET — 머리에서 winsock 을 안 부르려고(`NetworkThread.h` 와 같은 꼴)
        uint16_t port = 0;
    };
    struct Conn {
        uintptr_t s = 0;
        uint16_t port = 0;
        std::string in;          // 아직 줄이 안 끝난 받은 바이트
        std::string out;         // 아직 못 보낸 바이트
        bool closeAfter = false; // 보내기 버퍼를 다 비우면 끊는다
    };

    void Accept(const Listener& l, std::vector<TcpLineEvent>& out);
    void Drain(int conn, std::vector<TcpLineEvent>& out);   // 받을 것을 다 받아 줄로 · 끊기면 지운다
    void Drop(int conn, const char* why, std::vector<TcpLineEvent>& out);

    std::vector<Listener> _listeners;
    std::map<int, Conn> _conns;
    int _nextConn = 1;
    void* _event = nullptr;   // WSAEVENT
    bool _wsaUp = false;
};

}  // namespace nox
