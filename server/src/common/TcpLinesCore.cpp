// TcpLinesCore.cpp — TCP 줄 서버 부품의 ServerCore 판. 설명은 `TcpLinesCore.h` 머리.
//   이 파일만 ServerCore 머리를 연다 — ServerCore 의 컴파일 설정(UNICODE · spdlog 매크로 등)으로 컴파일된다(CMake `nox_tcp_core` 가
//   ServerNetworkLib 을 PRIVATE 로 링크). 설정이 갈리면 ServerCore 구조체 모양이 갈려 조용히 깨진다(`ServerCore/CMakeLists.txt` 의 경고)
#include "nox/TcpLinesCore.h"

#include <IOCPServer.h>
#include <MessageFramer.h>
#include <MonitorManager.h>
#include <RecvSink.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

// ServerCore 가 `ShutdownServer` 끝에 부르는 「프로세스를 끝내라」 알림(`IOCPServer.cpp` 의 extern — 쓰는 쪽이 정의한다).
//   녹스 서버는 끝낼 때를 고리 스레드가 정하므로 할 일이 없다. 한 실행 파일에 정의가 하나여야 해서 이 부품이 갖는다
void SignalProcessShutdown() {}

namespace nox {

namespace {

// ServerCore 스레드 → 고리 스레드 알림 하나
struct Note {
    enum class Kind { Opened, Line, Closed, ConnectFailed, Refused };
    Kind kind = Kind::Line;
    int64_t session = 0;
    int tag = 0;              // 듣기 차례(0, 1, …) · 바깥으로 붙은 접속은 −접속 번호(`Connect`)
    bool outbound = false;
    uint32_t peerIp = 0;      // 호스트 바이트 순서(`TcpLineEvent` 와 같다)
    uint32_t localIp = 0;
    DisconnectReason reason = DisconnectReason::IoError;
    int conn = 0;             // ConnectFailed — 붙기를 청한 접속 번호
    std::string line;
};

// 알림 큐 — 넣기 = ServerCore 워커 · 받기 스레드 · 붙기 스레드 / 빼기 = 고리 스레드(`Poll`). 넣을 때마다 깨움 이벤트를 켠다
class CNoteQueue {
public:
    CNoteQueue() : _wake(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~CNoteQueue() {
        if (_wake) CloseHandle(_wake);
    }
    CNoteQueue(const CNoteQueue&) = delete;
    CNoteQueue& operator=(const CNoteQueue&) = delete;

    void Push(Note&& n) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _notes.push_back(std::move(n));
        }
        SetEvent(_wake);
    }
    // 쌓인 것을 통째로 바꿔 온다(`out` 은 비어 있어야 한다)
    void Take(std::vector<Note>& out) {
        std::lock_guard<std::mutex> lock(_mutex);
        out.swap(_notes);
    }
    void Reset() { ResetEvent(_wake); }
    HANDLE Wake() const { return _wake; }

private:
    HANDLE _wake = nullptr;
    std::mutex _mutex;
    std::vector<Note> _notes;
};

// ServerCore 의 받은 것 넘길 곳 — 그 일이 난 스레드에서 불려 알림 하나를 큐에 넣고 돌아간다(오래 붙잡지 않는다 — `RecvSink.h`)
class CLineSink final : public CRecvSink {
public:
    explicit CLineSink(CNoteQueue& notes) : _notes(notes) {}

    void OnConnected(const SinkConnected& info) override {
        Note n;
        n.kind = Note::Kind::Opened;
        n.session = info.sessionId;
        n.tag = info.tag;
        n.outbound = info.outbound;
        n.peerIp = ntohl(info.peerIp);
        n.localIp = ntohl(info.localIp);
        _notes.Push(std::move(n));
    }
    void OnReceived(const SinkReceived& info) override {
        Note n;
        n.kind = Note::Kind::Line;
        n.session = info.sessionId;
        n.line.assign(info.msg->GetReadBufferPtr(), static_cast<size_t>(info.msg->GetDataSize()));   // 줄 끝 CR LF 는 자르기 부품이 뗐다
        info.msg->SubRef();
        _notes.Push(std::move(n));
    }
    void OnDisconnected(const SinkDisconnected& info) override {
        Note n;
        n.kind = Note::Kind::Closed;
        n.session = info.sessionId;
        n.reason = info.reason;
        _notes.Push(std::move(n));
    }
    void OnRefused(const SinkRefused& info) override {
        Note n;
        n.kind = Note::Kind::Refused;
        n.tag = info.tag;
        _notes.Push(std::move(n));
    }

private:
    CNoteQueue& _notes;
};

// ServerCore 끊긴 까닭 → `CTcpLines` 까닭 글자. 갈래마다 하나 — 새 까닭이 늘면 여기서 컴파일 경고(switch 에 default 없음)
const char* WhyOf(DisconnectReason reason) {
    switch (reason) {
        case DisconnectReason::PeerClosed: return CTcpLines::kWhyPeerClosed;
        case DisconnectReason::IoError: return CTcpLines::kWhyRecvError;      // ServerCore 는 입출력 실패를 받기 · 보내기로 안 가른다 — 「받기 오류」 하나로
        case DisconnectReason::BadMessage: return CTcpLines::kWhyLineTooLong; // 줄 단위 자르기가 틀렸다고 하는 것은 줄이 너무 긴 것뿐(`CLineFramer`)
        case DisconnectReason::SendOverflow: return CTcpLines::kWhySendError; // 보내기 링 넘침
        case DisconnectReason::AfterSend: return CTcpLines::kWhyCloseAfter;
        case DisconnectReason::LocalRequest: return CTcpLines::kWhyLocal;     // `Close` 한 접속은 표에서 먼저 지워 여기 안 온다 — 한도 거절 등
        case DisconnectReason::IdleTimeout: return CTcpLines::kWhyLocal;      // 끈 길(`SetIdleTimeoutSec(0)`)
    }
    return CTcpLines::kWhyRecvError;
}

std::string IpText(uint32_t ip) {
    char b[16];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu, ip & 0xFFu);
    return b;
}

}  // namespace

struct CTcpLinesCore::Impl {
    struct Conn {
        int64_t session = 0;      // 0 = 붙는 중(아직 세션 없음)
        uint16_t port = 0;        // 들어온 듣기 포트 · 바깥으로 붙은 접속은 상대 포트
        bool outbound = false;
        bool connecting = false;
        uint32_t peerIp = 0;      // 붙을 상대(OPENED 에 싣는다)
        std::string out;          // 아직 ServerCore 로 안 넘긴 줄(CRLF 붙임) — `Flush` 에서 넘긴다 · 붙는 중이면 붙은 뒤 `Flush` 에서
        bool dirty = false;       // `out` 이 차서 `_dirty` 목록에 들었다
        bool closeAfter = false;  // `Send(…, closeAfter)` — 이 줄까지 넘긴 뒤 보낸 뒤 끊기
        bool closeIssued = false; // 보낸 뒤 끊기를 ServerCore 에 걸었다(한 번만)
    };
    struct ConnectReq {
        int conn = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
    };

    explicit Impl(const TcpLinesCoreOptions& o)
        : options(o),
          server(std::make_unique<CIOCPServer>(0, static_cast<int>(o.maxConns), "nox-tcp-lines", monitor, o.workers, 1, 0, 0, 1)) {}

    TcpLinesCoreOptions options;
    CMonitorManager monitor;   // ServerCore 셈 모음(스레드 없음) — 이 부품이 하나 쥔다(계획 4-3)
    CNoteQueue notes;          // `server` 보다 먼저 — 받은 것 넘길 곳(`server` 안)이 이것을 가리키므로 나중에 사라져야 한다
    std::unique_ptr<CIOCPServer> server;
    std::vector<uint16_t> listenPorts;   // `Start` 뒤 실제 포트 · 차례 = 구분 값
    size_t listenCount = 0;
    bool started = false;

    std::map<int, Conn> conns;
    std::unordered_map<int64_t, int> bySession;
    std::vector<int> dirty;      // `out` 이 찬 접속 — `Flush` 가 이것만 돈다(모든 접속을 훑지 않으려고)
    int nextConn = 1;
    std::vector<Note> taken;     // `Poll` 이 바꿔 오는 그릇(다시 씀)

    // 붙기 스레드 — `ConnectTo` 가 막는 호출이라 따로 돈다
    std::thread connector;
    std::mutex connectMutex;
    std::condition_variable connectCv;
    std::deque<ConnectReq> connectQueue;
    bool connectStop = false;

    void ConnectLoop() {
        for (;;) {
            ConnectReq r;
            {
                std::unique_lock<std::mutex> lock(connectMutex);
                connectCv.wait(lock, [&] { return connectStop || !connectQueue.empty(); });
                if (connectStop) return;
                r = connectQueue.front();
                connectQueue.pop_front();
            }
            // 구분 값 = −접속 번호 — 붙자마자 오는 접속 알림(이 스레드에서 `ConnectTo` 안)이 어느 접속인지 알게. 듣기 구분 값(0 이상)과 안 겹친다
            const int64_t session = server->ConnectTo(IpText(r.ip).c_str(), r.port, -r.conn, kConnectTimeoutMs);
            if (session == 0) {
                Note n;
                n.kind = Note::Kind::ConnectFailed;
                n.conn = r.conn;
                notes.Push(std::move(n));
            }
        }
    }

    void StopConnector() {
        {
            std::lock_guard<std::mutex> lock(connectMutex);
            connectStop = true;
            connectQueue.clear();
        }
        connectCv.notify_all();
        if (connector.joinable()) connector.join();   // `ConnectTo` 하나가 걸려 있으면 붙기 한도(5초)까지 기다린다
    }

    void MarkDirty(int id, Conn& c) {
        if (c.dirty) return;
        c.dirty = true;
        dirty.push_back(id);
    }

    void Emit(std::vector<TcpLineEvent>& out, TcpLineEvent::Type type, int conn, const Conn& c, const char* why = "") {
        TcpLineEvent e;
        e.type = type;
        e.conn = conn;
        e.port = c.port;
        e.outbound = c.outbound;
        e.why = why;
        out.push_back(std::move(e));
    }

    void OnOpened(Note& n, std::vector<TcpLineEvent>& out) {
        if (n.outbound) {
            const int id = -n.tag;
            auto it = conns.find(id);
            if (it == conns.end() || !it->second.connecting) {   // 붙는 동안 이쪽에서 닫았다 — 붙은 세션을 놓는다
                server->RequestDisconnectSession(n.session);
                return;
            }
            Conn& c = it->second;
            c.connecting = false;
            c.session = n.session;
            bySession[n.session] = id;
            TcpLineEvent e;
            e.type = TcpLineEvent::Type::OPENED;
            e.conn = id;
            e.port = c.port;
            e.outbound = true;
            e.peerIp = c.peerIp;
            e.localIp = n.localIp;
            out.push_back(e);
            if (!c.out.empty() || c.closeAfter) MarkDirty(id, c);   // 붙는 동안 쌓인 줄 — 다음 `Flush` 에(`CTcpLines` 와 같다)
            return;
        }
        if (conns.size() >= options.maxConns) {   // 붙는 중인 접속이 자리를 쥐고 있다(ServerCore 표는 세션만 센다) — `CTcpLines` 처럼 받자마자 끊음
            server->RequestDisconnectSession(n.session);
            TcpLineEvent e;
            e.type = TcpLineEvent::Type::REFUSED;
            e.port = (n.tag >= 0 && static_cast<size_t>(n.tag) < listenPorts.size()) ? listenPorts[n.tag] : 0;
            out.push_back(e);
            return;
        }
        const int id = nextConn++;
        Conn c;
        c.session = n.session;
        c.port = (n.tag >= 0 && static_cast<size_t>(n.tag) < listenPorts.size()) ? listenPorts[n.tag] : 0;
        conns[id] = c;
        bySession[n.session] = id;
        TcpLineEvent e;
        e.type = TcpLineEvent::Type::OPENED;
        e.conn = id;
        e.port = c.port;
        e.peerIp = n.peerIp;
        e.localIp = n.localIp;
        out.push_back(e);
    }

    void OnLine(Note& n, std::vector<TcpLineEvent>& out) {
        auto s = bySession.find(n.session);
        if (s == bySession.end()) return;   // 이쪽에서 닫은 접속 — 그 번호는 다시 안 나온다
        auto it = conns.find(s->second);
        if (it == conns.end()) return;
        const Conn& c = it->second;
        TcpLineEvent e;
        e.type = TcpLineEvent::Type::LINE;
        e.conn = s->second;
        e.port = c.port;
        e.outbound = c.outbound;
        e.line = std::move(n.line);
        out.push_back(std::move(e));
    }

    void OnClosed(const Note& n, std::vector<TcpLineEvent>& out) {
        auto s = bySession.find(n.session);
        if (s == bySession.end()) return;   // 이쪽에서 닫은 접속 — `Close` 가 CLOSED 를 이미 냈다
        const int id = s->second;
        bySession.erase(s);
        auto it = conns.find(id);
        if (it == conns.end()) return;
        Emit(out, TcpLineEvent::Type::CLOSED, id, it->second, WhyOf(n.reason));
        conns.erase(it);
    }

    void OnConnectFailed(const Note& n, std::vector<TcpLineEvent>& out) {
        auto it = conns.find(n.conn);
        if (it == conns.end() || !it->second.connecting) return;   // 붙는 동안 이쪽에서 닫았다
        Emit(out, TcpLineEvent::Type::CLOSED, n.conn, it->second, CTcpLines::kWhyConnectFailed);
        conns.erase(it);
    }
};

CTcpLinesCore::CTcpLinesCore(const TcpLinesCoreOptions& options) : _impl(std::make_unique<Impl>(options)) {}

CTcpLinesCore::~CTcpLinesCore() { CloseAll(); }

bool CTcpLinesCore::Listen(uint32_t bindIp, uint16_t port, std::string& err) {
    if (_impl->started) {
        err = "Start 뒤에는 듣기를 더할 수 없다";
        return false;
    }
    const int tag = static_cast<int>(_impl->listenCount);
    if (!_impl->server->AddListener(IpText(bindIp).c_str(), port, tag)) {
        err = std::to_string(port) + " 포트 — 듣기 주소가 틀렸다";
        return false;
    }
    ++_impl->listenCount;
    return true;
}

bool CTcpLinesCore::Start(std::string& err) {
    Impl& m = *_impl;
    if (m.started) return true;
    CIOCPServer& s = *m.server;
    if (!s.SetFramer(std::make_unique<CLineFramer>()) || !s.SetRecvSink(std::make_unique<CLineSink>(m.notes)) ||
        !s.SetIdleTimeoutSec(0) ||   // 쉬는 접속 끊기 끔 — 지금 `CTcpLines` 에 없다(계획 8절 · 원칙 3)
        !s.SetKeepAlive(kKeepAliveIdleMs, kKeepAliveGapMs, false) ||   // 받은 접속에만(`CTcpLines::Accept` 와 같다)
        !s.SetSessionBufferSizes(m.options.recvRingBytes, m.options.sendRingBytes)) {
        err = "ServerCore 설정 실패";
        return false;
    }
    if (!s.Start()) {
        err = "ServerCore Start 실패 — 듣기를 못 열었거나(포트 사용 중 등) 링 크기가 틀림 · 까닭은 ServerCore 기록 줄";
        return false;
    }
    m.listenPorts.clear();
    for (size_t i = 0; i < m.listenCount; ++i) m.listenPorts.push_back(static_cast<uint16_t>(s.GetListenerPort(i)));
    m.started = true;
    return true;
}

uint16_t CTcpLinesCore::ListenPort(size_t i) const { return i < _impl->listenPorts.size() ? _impl->listenPorts[i] : 0; }

int CTcpLinesCore::Connect(uint32_t ip, uint16_t port, std::string& err) {
    Impl& m = *_impl;
    if (!m.started) {
        err = "Start 전";
        return 0;
    }
    if (m.conns.size() >= m.options.maxConns) {
        err = "접속 수 한도";
        return 0;
    }
    if (ip == 0 || port == 0) {
        err = "갈 곳이 아닌 주소(0.0.0.0 · 포트 0)";
        return 0;
    }
    if (!m.connector.joinable()) m.connector = std::thread([&m] { m.ConnectLoop(); });
    const int id = m.nextConn++;
    Impl::Conn c;
    c.port = port;
    c.outbound = true;
    c.connecting = true;
    c.peerIp = ip;
    m.conns[id] = c;
    {
        std::lock_guard<std::mutex> lock(m.connectMutex);
        m.connectQueue.push_back(Impl::ConnectReq{id, ip, port});
    }
    m.connectCv.notify_one();
    return id;
}

void* CTcpLinesCore::WaitHandle() const { return _impl->notes.Wake(); }

void CTcpLinesCore::Poll(std::vector<TcpLineEvent>& out) {
    Impl& m = *_impl;
    // 끄고 나서 뺀다 — 빼는 사이에 온 알림은 이벤트를 다시 켜 다음 기다림이 바로 돌아온다(`CTcpLines::Poll` 과 같은 차례)
    m.notes.Reset();
    m.taken.clear();
    m.notes.Take(m.taken);
    for (Note& n : m.taken) {
        switch (n.kind) {
            case Note::Kind::Opened: m.OnOpened(n, out); break;
            case Note::Kind::Line: m.OnLine(n, out); break;
            case Note::Kind::Closed: m.OnClosed(n, out); break;
            case Note::Kind::ConnectFailed: m.OnConnectFailed(n, out); break;
            case Note::Kind::Refused: {
                TcpLineEvent e;
                e.type = TcpLineEvent::Type::REFUSED;
                e.port = (n.tag >= 0 && static_cast<size_t>(n.tag) < m.listenPorts.size()) ? m.listenPorts[n.tag] : 0;
                out.push_back(e);
                break;
            }
        }
    }
    m.taken.clear();
}

bool CTcpLinesCore::Send(int conn, const std::string& line, bool closeAfter) {
    Impl& m = *_impl;
    auto it = m.conns.find(conn);
    if (it == m.conns.end()) return false;
    Impl::Conn& c = it->second;
    c.out += line;
    c.out += "\r\n";
    if (closeAfter) c.closeAfter = true;
    if (!c.connecting) m.MarkDirty(conn, c);
    return true;
}

void CTcpLinesCore::Flush(std::vector<TcpLineEvent>& out) {
    (void)out;   // 끊김은 ServerCore 가 다 보낸 뒤 알려 뒤의 `Poll` 이 낸다
    Impl& m = *_impl;
    if (m.dirty.empty()) return;
    std::vector<int64_t> closeAfter;
    for (const int id : m.dirty) {
        auto it = m.conns.find(id);
        if (it == m.conns.end()) continue;   // 그새 닫힘
        Impl::Conn& c = it->second;
        c.dirty = false;
        if (c.connecting) continue;
        if (!c.out.empty()) {
            // 넘치면 ServerCore 가 그 접속을 끊는다(까닭 SendOverflow → 「보내기 오류」 · 뒤의 `Poll`)
            m.server->RequestSendRaw(c.session, c.out.data(), static_cast<int>(c.out.size()));
            c.out.clear();
        }
        if (c.closeAfter && !c.closeIssued) {
            c.closeIssued = true;
            closeAfter.push_back(c.session);
        }
    }
    m.dirty.clear();
    m.server->FlushPendingSends();
    // 보낸 뒤 끊기는 넘긴 뒤에 — 「부른 뒤에 넣은 보내기는 안 갈 수 있다」(`IOCPServer.h` RequestDisconnectAfterSend)
    for (const int64_t session : closeAfter) m.server->RequestDisconnectAfterSend(session);
}

bool CTcpLinesCore::Close(int conn, std::vector<TcpLineEvent>& out) {
    Impl& m = *_impl;
    auto it = m.conns.find(conn);
    if (it == m.conns.end()) return false;
    Impl::Conn& c = it->second;
    if (c.connecting) {
        std::lock_guard<std::mutex> lock(m.connectMutex);   // 아직 안 걸었으면 걸지 않는다 · 걸렸으면 붙은 뒤 `OnOpened` 가 놓는다
        for (auto q = m.connectQueue.begin(); q != m.connectQueue.end(); ++q)
            if (q->conn == conn) {
                m.connectQueue.erase(q);
                break;
            }
    } else {
        // `CTcpLines::Close` 와 같게 — 안 넘긴 줄은 버리고, 이미 넘긴 것은 다 보낸 뒤 정상 끊기(FIN).
        //   그냥 끊기(RequestDisconnectSession)는 강제 끊기(RST)라 상대 쪽 까닭이 「상대가 닫음」에서 「받기 오류」로 바뀐다
        m.server->RequestDisconnectAfterSend(c.session);
        m.bySession.erase(c.session);
    }
    m.Emit(out, TcpLineEvent::Type::CLOSED, conn, c, CTcpLines::kWhyLocal);
    m.conns.erase(it);   // `_dirty` 에 남은 번호는 `Flush` 가 건너뛴다
    return true;
}

void CTcpLinesCore::CloseAll() {
    Impl& m = *_impl;
    m.StopConnector();   // `ConnectTo` 를 부르는 스레드를 먼저 멈춘다(`IOCPServer.h` ConnectTo 의 차례)
    if (m.started) {
        m.server->ShutdownServer();
        m.started = false;
    }
    m.conns.clear();
    m.bySession.clear();
    m.dirty.clear();
    m.listenPorts.clear();
}

size_t CTcpLinesCore::ConnCount() const { return _impl->conns.size(); }
unsigned long long CTcpLinesCore::KeepAliveOn() const { return _impl->server->GetKeepAliveOnCount(); }
bool CTcpLinesCore::Has(int conn) const { return _impl->conns.count(conn) != 0; }

}  // namespace nox
