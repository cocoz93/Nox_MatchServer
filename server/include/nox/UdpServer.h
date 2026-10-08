// UdpServer.h — 통신 층. 원본 `internal/netstr` 자리(`Docs/LAYERS_DESIGN.md`).
//
// 하는 일 — 와이어 머리 읽기·쓰기 · XOR · 신뢰 순번과 되받기 · 접속 제어(찾기·TRY_JOIN·CONNECT·핑·닫기).
// 소켓은 모른다. 받은 바이트를 넣으면 답 조각(`OutPacket`)과 게임에 넘길 일(`NetworkEvent`)이 나오고,
// 게임이 보낼 몸통을 `RequestSendMsg` 로 넘기면 머리를 붙이고 XOR 을 걸어 조각을 만든다.
// 이름은 ServerCore(`CIOCPServer`·`NetworkEvent`·`RequestSendMsg`)에 맞췄다.
//
// ★ 10-08 조각 3 — `nox::Server` 안에 섞여 있던 것을 옮겼다. 몸통·지표·설정 이름은 옛 `Server` 그대로다.
//   스레드를 나누는 조각 5 전까지는 `nox::Server` 가 이것을 멤버로 들고, `Server::OnPacket` 이 둘을 잇는다.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nox {

// 나갈 조각 하나 — 받는 주소와 와이어 바이트(머리 포함 · XOR 뒤).
struct OutPacket {
    std::string to;
    std::vector<uint8_t> data;
};

// ★ 스트림 하나의 순번 — 실측으로 밝힌 규칙이다.
//
//   **신뢰 전송은 순번을 하나 써서 올리고, 비신뢰는 「직전 신뢰가 쓴 값」을 그대로 싣는다.**
//
// 정품 호스트의 한 접속을 풀어 늘어놓으면 그대로 보인다:
//
//   80 00 1f  신뢰   순번 0   ← 쓰고 올린다
//   00 00 04  비신뢰 순번 0   ← 직전 신뢰가 쓴 값
//   80 01 a9  신뢰   순번 1
//   80 02 2b  신뢰   순번 2
//   00 02 a9  비신뢰 순번 2   ← 다시 직전 신뢰 값
//   00 02 27  비신뢰 순번 2
//   80 03 c2  신뢰   순번 3
//   00 03 a9  비신뢰 순번 3
//
// 신뢰를 한 번도 안 보낸 스트림은 0 이다(서버 찾기만 오간 세션이 그렇다).
//   ★ 10-08 — `Server.h` 에서 옮겼다(통신 층 것이라).
struct StreamSeq {
    uint8_t next = 0;   // 다음 신뢰 전송이 쓸 값
    uint8_t last = 0;   // 직전 신뢰 전송이 쓴 값 — 비신뢰가 싣는다
    // ★ 2026-09-18 페이즈 B-1b — **클라가 보낸 마지막 신뢰 순번**.
    //   정답지에서 되받기는 별도 메시지가 아니라 **답 패킷 머리의 seq 필드**다
    //   (`internal/netstr/conn.go:643~650` — 비신뢰 패킷을 받으면 그 seq 로 송신 큐를 지운다).
    //   이 값을 안 실으면 클라는 답을 못 받았다고 보고 **1초마다 영원히 재전송**한다
    //   (`send_reliable.go:135 Resend`). CLIENT_READY 가 23번 온 까닭.
    uint8_t recvSeq = 0;
    bool    recvSeqSeen = false;   // 한 번이라도 신뢰 패킷을 받았나
};

// ★ 접속 대본 — 덤프 재생 전용(옛 `Server::AcceptScript` · `Server.h` 에 같은 이름 별칭이 있다).
//
// **키와 번호는 접속마다 다르다**(실측 0x70 · 0x2a). 서버가 스스로 정하는 값이라
// 바이트를 맞춰보는 대상이 아니다. 그래서 덤프에서 뽑아 순서대로 주입한다.
// 이걸 안 하면 두 번째 접속에서 키 한 자리 때문에 "다름"이 떠, 정작 봐야 할
// **나머지 아홉 자리가 맞는다는 사실**이 묻힌다.
// 접속 한 번에 정품이 쓴 값들. 전부 **서버가 스스로 정하는 값**이라
// 바이트를 맞춰보는 대상이 아니다.
//   접속 번호·키는 통신 층이 쓰고, 프레임·오브젝트 번호는 `CONNECTED` 이벤트에 실어 게임에 넘긴다.
struct AcceptScript {
    uint32_t connectId = 0;
    uint8_t xorKey = 0;
    uint32_t frame = 0;    // 그때의 서버 프레임
    uint16_t netCode = 0;  // 그 사람에게 준 오브젝트 번호
};

// ★ 10-08 조각 4 — 세션 번호 「없음」. 번호는 1부터라 0 이 비어 있다(옛 빈 주소 글자 자리).
//   리터럴 0 대신 이 이름을 쓴다 — 0 은 `std::string` 인자에도 들어가(널 포인터) 컴파일되고 실행 중에 죽는다.
constexpr int64_t kNoSession = 0;

// SERVER_INFO 본문에서 토큰이 앉는 자리. 실측으로 확정했다.
constexpr size_t kInfoTokenOffset = 41;

// 통신 층 → 게임으로 넘기는 일. ServerCore `NetworkEvent` 꼴.
struct NetworkEvent {
    enum class Type {
        CONNECTED,      // 키를 줬다(SERVER_CONNECT 처리 뒤)
        DISCONNECTED,   // 클라가 접속을 닫는다고 알렸다(0x0A CLIENT_CLOSE)
        RECEIVED        // 게임 메시지 — 처음 0x20 이상 오프코드가 나온 자리부터 조각 끝까지(머리를 뗀 평문)
    };
    Type type = Type::RECEIVED;
    int64_t sessionId = 0;          // 게임이 사람을 가르는 번호(10-08 조각 4)
    std::string addr;               // 그 세션의 주소 — 기록용
    std::vector<uint8_t> body;      // RECEIVED
    // CONNECTED — 덤프 재생이면 그때 서버가 정한 프레임·오브젝트 번호(`scripted`)
    bool scripted = false;
    uint32_t frame = 0;
    uint16_t netCode = 0;
};

// 통신 층의 상대 하나 — 원본 netstr `Conn` 자리. 주소:포트로 가른다.
struct CUdpSession {
    std::string addr;
    int64_t sessionId = 0;   // 처음 볼 때 매긴 번호(1부터) — 게임 쪽 이름표(10-08 조각 4)
    // ★ 10-08 조각 4c — 게임에 이벤트를 넘긴 적이 있나(없으면 서버 찾기·접속 시도만 한 상대 — 통신 층이 걷어낸다) · 마지막으로 받은 때
    bool announced = false;
    uint32_t lastSeenMs = 0;
    uint8_t xorKey = 0;      // 0이면 아직 평문
    bool keyed = false;      // 키를 준 뒤인가
    int lastRelStream = -1;  // 클라가 마지막으로 **신뢰**를 보낸 스트림(되받기 기준). -1 이면 아직 없음
    uint32_t connectId = 0;  // SERVER_ACCEPT 에 실어 보낸 번호
    // 스트림마다 따로 센다. 핑·퐁이 스트림 1 을 쓰는 것이 실측으로 확인됐다.
    StreamSeq seq[128];

    // ★ 10-08 조각 3b — 신뢰로 보낸 조각을 확인이 올 때까지 들고 있다가 다시 보낸다(원본 netstr `reliableSender`
    //   `send_reliable.go`). 정품 호스트도 확인 못 받은 신뢰 조각을 약 2.1초마다 다시 보냈다(정품 녹음 `CAP_GEN3`·`CAP_GEN4`).
    struct PendingReliable {
        std::vector<uint8_t> wire;   // 머리 포함 · XOR 전 — 다시 보낼 때 그때 키로 뒤집는다(원본도 보낼 때 뒤집는다)
        uint8_t seq = 0;
        uint32_t retryAt = 0;        // 이 시각(ms)이 지나면 다시 보낼 차례(`retryAt`)
        bool shouldSend = false;     // 다음 다시 보내기 때 보낸다(`shouldSend`)
    };
    std::vector<PendingReliable> reliableQueue;   // 보낸 차례(오래된 것이 앞)
    // 클라가 비신뢰 머리에 실어 온 「다음에 받을 순번」. 바뀔 때만 처리한다(원본 `seqRecv` — `conn.go:659-665`).
    int lastAckSeen = -1;
};

class CUdpServer {
public:
    // ── 설정 ────────────────────────────────────────────────
    // 서버 찾기에 답할 원본. 머리 2바이트와 오프코드를 뺀 **본문**을 넣는다.
    // 토큰 자리만 요청 값으로 바꿔 그대로 돌려준다 — 모르는 자리를 건드리지 않는 것이
    // 「녹음 재생」의 요점이다.
    void SetInfoTemplate(const uint8_t* body, size_t n) { _infoBody.assign(body, body + n); }

    // 접속을 받아줄 때 클라에게 줄 XOR 키. 실제 서버는 스스로 정하지만,
    // 덤프와 바이트를 맞춰보려면 그때 쓰인 키를 넣을 수 있어야 한다.
    void SetXorKey(uint8_t key) { _xorKey = key; }

    // 다음 접속에 매길 번호. 실측은 1부터 올라갔다.
    void SetNextConnectId(uint32_t id) { _nextConnectId = id; }

    void SetAcceptScript(std::vector<AcceptScript> script) { _script = std::move(script); }

    // ★ 들어오는 것이 이미 평문인가 — 덤프 재생 전용 스위치.
    //
    // 진짜 소켓에서는 키를 준 뒤 받는 것이 암호문이라 풀어야 한다.
    // 그런데 **덤프의 RECV 는 이미 평문이다** — 기록을 건 자리가 복호화 직후라서다
    // (Docs/PROTOCOL.md 「XOR 은 방향에 따라 다르다」). 그걸 또 풀면 되레 망가진다.
    // 이 스위치를 안 두고 돌렸을 때 `0x3f`(클라 입력) 32,450건이
    // `0x4f`(방어구 장착)로 읽혔다 — 정확히 `0x3f ^ 0x70` 이다.
    void SetInboundPlain(bool v) { _inboundPlain = v; }

    // B-1b 되받기 끄기(대조군) — `--no-ack-seq`
    void SetAckRecvSeq(bool v) { _ackRecvSeq = v; }
    // 되받기를 **모든 스트림의 비신뢰**에 싣는다 — 클라가 읽는 쪽이 스트림 0 이라서(RequestSendMsg 주석)
    void SetAckAnyStream(bool v) { _ackAnyStream = v; }

    // ── 받기·보내기 ──────────────────────────────────────────
    // 데이터그램 하나. 앞에서부터 0x20 미만 오프코드는 접속 제어라 여기서 답하고(`out`),
    // 처음 0x20 이상이 나온 자리부터는 `RECEIVED` 로 게임에 넘긴다. 돌려주는 값 = 여기서 읽은 접속 제어 메시지 수.
    size_t OnDatagram(const std::string& from, const uint8_t* data, size_t n,
                      std::vector<OutPacket>& out, std::vector<NetworkEvent>& events);

    // 게임이 만든 몸통 하나를 조각으로 — 머리 2바이트(신뢰·스트림·순번)를 붙이고 키를 줬으면 XOR.
    //   없는 세션이면 버리고 센다(`sendNoSession`).
    void RequestSendMsg(int64_t sessionId, bool reliable, uint8_t stream,
                        const uint8_t* msg, size_t n, std::vector<OutPacket>& out);

    // 게임이 그 상대를 지웠다 — 키·순번도 함께 버린다(다시 접속하면 새 번호로 처음부터).
    void RequestDisconnectSession(int64_t sessionId);

    // ★ 10-08 조각 4 — 세션 번호. 처음 보는 주소에 1부터 매긴다.
    //   `OpenSession` 은 그 주소의 번호(없으면 만든다) — 소켓 없이 사람을 세우는 시험·진단 진입(`DebugEnterGame`)이 쓴다.
    int64_t OpenSession(const std::string& addr);
    int64_t SessionIdOf(const std::string& addr) const;   // 없으면 0
    const std::string& AddrOf(int64_t sessionId) const;    // 없으면 빈 문자열 — 기록용

    // ★ 10-08 조각 3b — 확인이 안 온 신뢰 조각 다시 보내기. 게임 틱이 부른다.
    //   1초마다 살펴 보낸 지 2초 지난 것을 다시 보내고(원본 `MaybeSendReliable` · `reliableRetry = 2s`),
    //   클라가 「그 순번부터 다시」라고 알려 온 것은 다음 틱에 바로 보낸다(`retry`).
    void Tick(uint32_t nowMs, std::vector<OutPacket>& out);
    // 끄기 — 다시 보내기를 안 한다(대조용). 기본 켬.
    void SetReliableResend(bool v) { _reliableResend = v; }

    // ── 조회 ────────────────────────────────────────────────
    // ⚠⚠ 열쇠 상태를 **같이** 봐야 한다 — 복호가 어긋나면 쓰레기 바이트가
    //   **그럴듯한 오프코드로 보인다.** 「메시지 N개」만 보면 속는다.
    bool IsKeyed(const std::string& addr) const {
        auto it = _sessions.find(addr);
        return it != _sessions.end() && it->second.keyed;
    }
    uint8_t KeyOf(const std::string& addr) const {
        auto it = _sessions.find(addr);
        return it == _sessions.end() ? 0 : it->second.xorKey;
    }
    // 이 상대에게 준 XOR 키. 아직 안 줬으면 0.
    // 덤프의 SEND 를 사람이 읽으려면 이게 필요하다.
    uint8_t KeyFor(const std::string& addr) const {
        auto it = _sessions.find(addr);
        return it == _sessions.end() ? 0 : (it->second.keyed ? it->second.xorKey : 0);
    }

    unsigned long long reliableHeard() const { return _reliableHeard; }
    unsigned long long ackSeqApplied() const { return _ackSeqApplied; }
    unsigned long long ackSeqDiffer() const { return _ackSeqDiffer; }
    // ★ 2026-09-18 B-1b — **스트림별로 갈라 센다.** 합계만 보면
    //   「클라가 보낸 스트림」과 「서버가 답한 스트림」이 어긋나도 안 드러난다.
    //   되받기는 **같은 스트림에 실려야** 뜻이 있다(StreamSeq 가 스트림마다 따로다).
    unsigned long long recvReliableOn(uint8_t st) const { return st < 8 ? _recvRelStream[st] : 0; }
    unsigned long long sendReliableOn(uint8_t st) const { return st < 8 ? _sendRelStream[st] : 0; }
    unsigned long long sendPlainOn(uint8_t st) const { return st < 8 ? _sendPlainStream[st] : 0; }
    unsigned long long ackValueCount(uint8_t v) const { return v < 32 ? _ackValueHist[v] : 0; }
    // ★ 09-21 — **되받기 값이 올라가나**. 옛 `ackSeqDiffer` 는 서로 다른 둘을 견주던 숫자라 뜻이 없었다
    //   (`UdpServer.cpp` 되받기 가지 주석). 이것이 0 이면 클라의 신뢰 순번이 **멈춘** 것이다.
    unsigned long long ackSeqChanged() const { return _ackSeqChanged; }
    uint8_t ackLastSent() const { return _ackLastSent; }

    // ★ 10-08 조각 3b — 다시 보내기 지표. 「보관」은 지금 확인을 기다리는 조각 수(모든 상대 합).
    unsigned long long reliableResent() const { return _reliableResent; }
    unsigned long long reliableAcked() const { return _reliableAcked; }
    unsigned long long reliableRetryMarked() const { return _reliableRetryMarked; }
    unsigned long long reliableQueueDropped() const { return _reliableQueueDropped; }
    size_t reliablePending() const {
        size_t n = 0;
        for (const auto& kv : _sessions) n += kv.second.reliableQueue.size();
        return n;
    }
    // ★ 10-08 조각 4 — 지운 세션 번호로 보내려 해 버린 수. 0 이 정상이다.
    unsigned long long sendNoSession() const { return _sendNoSession; }
    // ★ 10-08 조각 4c — 게임에 안 넘긴 채 조용해져 걷어낸 상대 수(서버 찾기·접속 시도만 한 상대).
    unsigned long long unannouncedSwept() const { return _unannouncedSwept; }

    // 접속 제어 조각만 센다(게임 조각은 `nox::Server` 의 같은 이름이 센다).
    const std::map<uint8_t, unsigned long long>& seen() const { return _seen; }
    unsigned long long stopped() const { return _stopped; }
    const std::map<uint8_t, unsigned long long>& stoppedByOp() const { return _stoppedByOp; }
    unsigned long long handled() const { return _handled; }
    unsigned long long connectTailStops() const { return _connectTailStops; }
    const std::vector<uint8_t>& LastFragOps() const { return _lastFragOps; }

private:
    CUdpSession& SessionFor(const std::string& addr);
    // 그 상대에게 몸통 하나를 조각으로(`RequestSendMsg` 몸통 · 접속 제어 답도 이걸 쓴다).
    void SendOn(CUdpSession& s, bool reliable, uint8_t stream,
                const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    // 클라가 「다음에 받을 순번은 seq」라고 알렸다 — 그 순번은 다시 보낼 표시, 그보다 앞은 지운다(`SetCurrent`).
    void Ack(CUdpSession& s, uint8_t seq);
    // 표시된 것을 지금 키로 뒤집어 다시 내보낸다(`Resend`).
    void Resend(CUdpSession& s, std::vector<OutPacket>& out);

    // 원본 값(`send_reliable.go:13` `reliableRetry = 2 * time.Second` · `:19` 1초마다 살핌)
    static constexpr uint32_t kReliableRetryMs = 2000;
    static constexpr uint32_t kReliableCheckMs = 1000;
    // 우리 값 — 원본은 한도가 없다. 확인을 안 보내는 상대(진단 봇 등)에게 쌓이기만 하는 것을 막는다. 넘치면 오래된 것부터 버리고 센다
    static constexpr size_t kReliableQueueMax = 128;
    // ★ 10-08 조각 4c — 게임에 안 넘긴 상대를 걷어내는 조용한 시간. 옛 게임 쪽 값(판에 안 든 사람 30초 · `kSessionTimeoutTicks`) 그대로
    static constexpr uint32_t kUnannouncedIdleMs = 30000;

    std::map<std::string, CUdpSession> _sessions;
    std::map<int64_t, std::string> _addrOf;   // 세션 번호 → 주소(`_sessions` 의 열쇠)
    int64_t _nextSessionId = 1;
    unsigned long long _sendNoSession = 0;
    unsigned long long _unannouncedSwept = 0;

    std::vector<uint8_t> _infoBody;
    uint8_t _xorKey = 0x70;
    uint32_t _nextConnectId = 1;
    bool _inboundPlain = false;
    std::vector<AcceptScript> _script;   // 접속 대본
    size_t _scriptAt = 0;

    unsigned long long _reliableHeard = 0; // 신뢰 비트가 선 조각을 받은 수 (B-1b)
    // 스트림 0~7 만 센다 — 그 위는 실제로 안 쓰인다(넘으면 안 센다는 뜻이니 로그에서 합이 안 맞으면 그것도 단서다)
    unsigned long long _ackValueHist[32] = {};   // 되받기로 실어 보낸 **값**의 분포(0~31)
    unsigned long long _recvRelStream[8] = {};   // 클라 → 서버 **신뢰**, 스트림별
    unsigned long long _sendRelStream[8] = {};   // 서버 → 클라 **신뢰**, 스트림별
    unsigned long long _sendPlainStream[8] = {}; // 서버 → 클라 **비신뢰**, 스트림별(되받기가 실리는 자리)
    unsigned long long _ackSeqApplied = 0; // 되받기 가지를 탄 횟수 (0 이면 분기가 안 도는 것)
    unsigned long long _ackSeqDiffer = 0;  // ✖ 09-21 폐기 — 뜻 없는 견줌이었다(되받기 가지 주석)
    unsigned long long _ackSeqChanged = 0; // 되받기 **값이 직전과 달라진** 횟수 — 0 이면 클라 순번이 멈춤
    uint8_t _ackLastSent = 0;              // 마지막으로 실어 보낸 되받기 값
    // B-1b 되받기 — ⚠ **기본 끔.** 09-18 실측에서 **효과가 없었다**(아래).
    //   켜려면 `--ack-seq`. 분기 자체는 도는 것을 계수기로 확인했으므로
    //   (되받기 37/다름 37) 배선이 아니라 **방식**이 틀린 것이다.
    //
    //   실측 표 — 세 회차가 **완전히 같은 숫자**였다(exe 는 소스보다 최신, 빌드 지문 확인함):
    //     끔(--no-ack-seq)   준비 20 · 신뢰받음 25
    //     켬(N 그대로)        준비 20 · 신뢰받음 25 · 되받기 37/다름 37
    //     켬(N+1)            준비 20 · 신뢰받음 25 · 되받기 37/다름 37
    //
    //   ⇒ 다음에 볼 것 = **스트림 번호**. 클라가 CLIENT_READY 를 보내는 스트림과
    //     우리가 답하는 스트림이 다르면 `s.seq[…]` 칸이 어긋나 되받기가 엉뚱한 데 실린다.
    //     계수기를 **스트림별로 갈라서** 세야 갈린다(「갈래를 세라」).
    // ★★★ 2026-09-18 — **증상은 없앨 수 있는데 정품 바이트와 어긋난다. 기본은 끔이다.**
    //
    //   켜면(`--ack-seq --ack-any-stream`) — `CLIENT_READY` 수신이 **18~25 → 2** 로 떨어지고
    //     클라 신뢰 큐가 0 으로 빈다(09-18 실측 · 재현 2회). 클라 재전송이 멈춘 것이다.
    //   ⚠ 그런데 **`replay_connect` 가 깨진다** — 정품 녹음에서 비신뢰 머리의 순번 바이트는
    //     **늘 `00`** 인데(XOR 0x70 을 풀면 `70` 이 `00`) 우리는 되받기 값을 싣게 된다.
    //     정품 녹음은 우리 최고 등급 근거이므로 **기본은 끔**으로 둔다.
    //   ⇒ 아직 모르는 것 — **정품은 무엇으로 클라 재전송을 멈추나.**
    //     녹음에 `0xab MSG_IMPORTANT_ACK` 가 8 건 있는데 그건 `0xcc SEQ_IMPORTANT` 전용이다.
    //     참고 — 그 녹음에서 클라는 `CLIENT_READY` 를 **4 번** 보냈다(2 번이 아니다).
    //     즉 정품도 몇 번은 재전송한다. 18~25 가 과한 것이지 2 가 정답이라는 근거는 없다.
    //   ⚠ 「켬」은 `_ackAnyStream` 과 **짝으로** 켜야 한다. 되받기를 스트림 1 에만 실으면
    //     클라가 그 패킷의 seq 를 안 읽어(09-18 실측: 받은 seq 분포에 4 가 0 건) 효과가 없다.
    bool _ackRecvSeq = true;
    bool _ackAnyStream = true;   // 되받기를 **스트림 0 비신뢰**에 싣는가(기본 켬 · RequestSendMsg 주석의 정품 근거)

    // ★ 10-08 조각 3b — 다시 보내기
    bool _reliableResend = true;
    uint32_t _nowMs = 0;               // 마지막 Tick 의 시각 — 그 사이에 보낸 조각의 `retryAt` 기준
    bool _resendClockSet = false;
    uint32_t _lastResendCheckMs = 0;
    unsigned long long _reliableResent = 0;       // 다시 보낸 조각 수
    unsigned long long _reliableAcked = 0;        // 확인받아 지운 조각 수
    unsigned long long _reliableRetryMarked = 0;  // 클라가 「그 순번부터 다시」라고 해서 표시한 수
    unsigned long long _reliableQueueDropped = 0; // 한도를 넘어 버린 수

    // 진단 — 접속 제어 조각만(게임 조각은 `nox::Server` 가 센다)
    std::map<uint8_t, unsigned long long> _seen;
    unsigned long long _stopped = 0;
    std::map<uint8_t, unsigned long long> _stoppedByOp;
    std::set<uint8_t> _stopSampleShown;
    unsigned long long _handled = 0;
    unsigned long long _connectTailStops = 0;
    std::vector<uint8_t> _lastFragOps;
};

}  // namespace nox
