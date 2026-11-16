// TcpLinesCore.h — TCP 줄 서버 부품의 ServerCore 판. 겉 · 사건 꼴은 `CTcpLines`(`TcpLines.h`)와 같다.
//
// ★ 11-17 D-1(`Docs/SERVERCORE_LOBBY_MOVE_PLAN_1116.md` 4절) — 소켓 층만 ServerCore(`CIOCPServer` · IOCP)로 갈아 끼운 판.
//   왜: `CTcpLines::Poll` 은 깰 때마다 모든 접속에 받기 호출을 한다 — 쉬는 접속도 깰 때마다 커널 호출 한 번이다(계획 1절).
//   여기서는 받기 · 줄 자르기(`CLineFramer`)를 ServerCore 워커가 하고 접속 · 받은 줄 · 끊김만 알림 큐로 넘긴다.
//   `Poll` 은 큐를 바꿔 와 세션 번호를 접속 번호로 바꿀 뿐이다 — 일이 없는 접속은 `Poll` 에 안 나온다.
//
//   겉이 하나 다르다 — `Listen` 을 다 부른 뒤 `Start` 를 한 번 부른다. ServerCore 는 듣기를 `Start` 전에만 더하고
//     bind · listen 실패를 `Start` 가 알린다. `CTcpLines` 에도 늘 성공하는 `Start` 가 있어 두 판이 같은 시험을 돈다.
//   늦게 오는 것 — 보낸 뒤 끊기 · 보내기 넘침의 `CLOSED` 는 `Flush` 가 아니라 뒤의 `Poll` 에서 나온다(ServerCore 가 다 보낸 뒤 알린다).
//   `Close` 는 `CTcpLines` 처럼 정상 끊기다(FIN) — 아직 `Flush` 안 한 줄은 버리고, 이미 넘긴 것은 다 보낸 뒤 닫는다.
//     그래서 줄은 `Flush` 까지 이쪽 버퍼에 두고 `Flush` 때 ServerCore 로 넘긴다(넘긴 뒤에는 ServerCore 링에서 못 뺀다).
//   ⚠ 한 프로세스에 하나 — ServerCore `Start` 가 공용 메시지 풀(`CSerialBuffer::_TlsMsgFreeList`)을 새로 만들어 덮는다.
//     시험은 판마다 프로세스를 따로 띄운다(`tests/tcp_lines_core_test.cpp`).
//
// 스레드: 겉 함수는 모두 한 스레드에서 부른다(`CTcpLines` 와 같다). 안에서 도는 것 = ServerCore 워커 · 보내기 워커 ·
//   듣기마다 받기 스레드 · 붙기 스레드(`Connect` 를 처음 부를 때 띄운다 — 붙기가 막는 호출이라 고리 스레드에서 못 부른다)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nox/TcpLines.h"   // TcpLineEvent · 끊긴 까닭 글자 — 두 판이 같은 것을 쓴다

namespace nox {

struct TcpLinesCoreOptions {
    int workers = 1;              // ServerCore 워커 수 — 처음 값 1(계획 4-3 · 부하 시험이 워커 바쁨을 같이 잰다)
    size_t recvRingBytes = 8192;  // 세션 받기 링 — 한 줄 최대(4,096)보다 커야 한다(작으면 `Start` 가 false)
    size_t sendRingBytes = 65535; // 세션 보내기 링 — 한 접속에 `Flush` 한 번에 내는 최대보다 커야 한다(넘치면 그 접속을 끊는다 —
                                  //   까닭 「보내기 오류」). 값은 구체화 D-① 전까지 ServerCore 기본
};

class CTcpLinesCore {
public:
    static constexpr size_t kMaxLine = CTcpLines::kMaxLine;
    static constexpr size_t kMaxConns = CTcpLines::kMaxConns;   // ServerCore 세션 표 크기 · 붙는 중인 접속도 센다(`CTcpLines` 와 같다)

    static constexpr const char* kWhyPeerClosed = CTcpLines::kWhyPeerClosed;
    static constexpr const char* kWhyRecvError = CTcpLines::kWhyRecvError;
    static constexpr const char* kWhySendError = CTcpLines::kWhySendError;
    static constexpr const char* kWhyLineTooLong = CTcpLines::kWhyLineTooLong;
    static constexpr const char* kWhyCloseAfter = CTcpLines::kWhyCloseAfter;
    static constexpr const char* kWhyLocal = CTcpLines::kWhyLocal;
    static constexpr const char* kWhyConnectFailed = CTcpLines::kWhyConnectFailed;

    static constexpr unsigned long kKeepAliveIdleMs = CTcpLines::kKeepAliveIdleMs, kKeepAliveGapMs = CTcpLines::kKeepAliveGapMs;
    static constexpr uint32_t kConnectTimeoutMs = 5000;   // 붙기 한도 — 게임 서버 → 로비 지금 값(`NetworkThread.h:78`)

    explicit CTcpLinesCore(const TcpLinesCoreOptions& options = TcpLinesCoreOptions());
    ~CTcpLinesCore();
    CTcpLinesCore(const CTcpLinesCore&) = delete;
    CTcpLinesCore& operator=(const CTcpLinesCore&) = delete;

    // 듣기 하나를 더한다(`Start` 전 · 여러 번). 부른 차례가 구분 값이다. `bindIp` 0 = 모든 주소 · `port` 0 = OS 가 고름(시험).
    //   실제로 여는 것은 `Start` — 여기서는 주소만 본다. `Start` 뒤면 false
    bool Listen(uint32_t bindIp, uint16_t port, std::string& err);
    // 듣기를 열고 ServerCore 를 띄운다(한 번). 듣기를 못 열면 false · `err` 에 까닭. 듣기 없이 붙기만 해도 부른다
    bool Start(std::string& err);
    uint16_t ListenPort(size_t i) const;   // `Start` 뒤 i 번째 듣기의 실제 포트
    // 바깥으로 붙기를 시작한다(막지 않음 · `Start` 뒤). 결과는 뒤의 `Poll` 이 `OPENED` · `CLOSED`(「붙지 못함」)로
    int Connect(uint32_t ip, uint16_t port, std::string& err);
    // 알림이 생기면 켜지는 이벤트(수동으로 꺼짐 · `Poll` 이 끈다)
    void* WaitHandle() const;

    void Poll(std::vector<TcpLineEvent>& out);
    bool Send(int conn, const std::string& line, bool closeAfter = false);
    void Flush(std::vector<TcpLineEvent>& out);
    bool Close(int conn, std::vector<TcpLineEvent>& out);
    void CloseAll();

    size_t ConnCount() const;
    unsigned long long KeepAliveOn() const;
    bool Has(int conn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace nox
