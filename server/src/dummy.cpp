// dummy.cpp — 부하 실험 더미. 정품 클라 대신 서버에 붙어 판을 치른다(정본 Docs/MO_DUMMY_PLAN_1109.md).
//
// 왜 패킷으로 만드나 — 재려는 것이 「서버가 소켓으로 받고 보내는 몫」이라서다. 서버 안 봇은 소켓을 안 지나가 그 몫이 안 잡힌다.
// 구조 — 스레드 하나가 더미 전부를 돈다. 더미마다 UDP 소켓 하나. 자기 차례가 되면 받은 것 다 읽기 → 메시지 나누기 → 행동 → 보내기.
//
// 정품 클라가 사람 조작 없이도 보내는 넷(Docs/MO_CLIENT_SELF_SEND_1109.md) — 입력 3f · 핑 06 · 중요 확인 ab · 별칭 a5.
// 하나라도 빠지면 서버가 실물과 다른 길을 탄다(ab 없으면 다시 싣기 · a5 없으면 전체 번호로 계속 소개).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nox/Alias.h"
#include "nox/Opcode.h"
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace {

using nox::Op;

constexpr int64_t kWheelUs = 33000;      // 한 바퀴 33ms — 서버 틱과 같고 정품 입력(초당 29~32번) 안에 든다
constexpr int kWheelSlots = 33;          // 한 바퀴를 1ms 칸으로 나눈다
constexpr int64_t kPingUs = 2000000;     // 정품 핑 간격 2초(R1 7바이트 81번 · Docs/MO_CLIENT_SELF_SEND_1109.md)
constexpr int64_t kJoinRetryUs = 1000000;   // 접속 시도 · 연결을 다시 보내는 간격(UDP 라 한 번이 사라질 수 있다)
constexpr size_t kMaxOutBody = 1000;     // 한 조각에 싣는 본문 상한 — 서버 받기 버퍼 2048(NetworkThread.cpp:218) 안

#define NOX_DUMMY_COUNTERS(X) \
    X(sentInput) X(sentPing) X(sentAck) X(sentAlias) X(sentMouse) X(sentSpell) X(sentAbility) \
    X(recvDgram) X(recvBytes) X(decodeFail) X(dupImportant) X(lateTicks) \
    X(petsAcquired) X(petsLost) X(spellSkippedNoMana) X(retreats) X(heals)

struct Counters {
#define X(n) uint64_t n = 0;
    NOX_DUMMY_COUNTERS(X)
#undef X
};

Counters g_count;
int g_decodeFailShown = 0;

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Args {
    std::string host = "127.0.0.1";
    uint16_t port = 18590;
    int count = 1;
    int roomSize = 1;
    std::vector<int> classes{0};
    int durationSec = 150;
    int joinIntervalMs = 200;
    uint32_t seed = 1;
};

enum class Phase { WaitTurn, TryJoin, Connect, Game };

struct Dummy {
    int index = 0;
    int cls = 0;
    SOCKET sock = INVALID_SOCKET;
    Phase phase = Phase::WaitTurn;
    int64_t joinAtUs = 0;
    int64_t retryAtUs = 0;
    int64_t pingAtUs = 0;
    uint8_t key = 0;
    // ★ 머리 둘째 바이트는 「서버 신뢰 조각 가운데 다음에 받을 순번」이다 — 받은 순번이 아니라.
    //   받은 순번을 그대로 실으면 서버가 「그것부터 못 받았다」로 읽어 마지막 조각을 다시 보낸다(UdpServer.cpp:216-224 · :580-590).
    //   정품 녹음도 신뢰 0 · 1 · 2 를 받은 뒤 3 을 싣는다.
    uint8_t relNext = 0;
    uint32_t frame = 0;
    nox::AliasTable aliases;
    uint16_t selfNet = 0;
    int x = 0, y = 0;
    uint8_t facing = 0;
    std::vector<bool> seenImportant;     // 번호 65536칸 — 같은 번호를 두 번 받으면 서버의 다시 싣기다
    std::vector<uint8_t> out;            // 이번 바퀴에 실을 메시지(입력 뒤에 붙인다)
};

void SendRaw(const Dummy& d, const uint8_t* p, size_t n) {
    send(d.sock, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
}

// 키를 받은 뒤의 조각 — 머리 [01][다음에 받을 순번] + 메시지, 전체를 뒤집는다.
void SendGame(Dummy& d, const uint8_t* msg, size_t n) {
    std::vector<uint8_t> p(n + nox::kWireHeaderSize);
    p[0] = 0x01;
    p[1] = d.relNext;
    std::memcpy(p.data() + nox::kWireHeaderSize, msg, n);
    nox::ApplyXor(p.data(), p.size(), d.key);
    SendRaw(d, p.data(), p.size());
}

void Put16(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& o, uint32_t v) {
    Put16(o, v);
    Put16(o, v >> 16);
}

// 처음 보는 물체를 표에 넣고 알린다 — 원본 GAME2_3.c:1524-1532(머리 · 만료 없음) · :1624-1632(항목 · 프레임 + 60).
// 원본은 전체 번호를 받을 때마다 보낸다(같은 칸이어도 — 플랜 9절 「별칭 답 조건」).
void RegisterAlias(Dummy& d, uint16_t netCode, uint16_t type, bool head) {
    const int a = d.aliases.Generate(netCode, type, d.frame);
    if (a < 0) return;
    const uint32_t deadline = head ? 0xFFFFFFFFu : d.frame + nox::kAliasTTL;
    d.aliases.Set(a, netCode, type, deadline);
    d.out.push_back(static_cast<uint8_t>(Op::NEW_ALIAS));
    d.out.push_back(static_cast<uint8_t>(a));
    Put16(d.out, netCode);
    Put16(d.out, type);
    Put32(d.out, deadline);
    ++g_count.sentAlias;
}

void OnUpdate(Dummy& d, const nox::UpdateMsg& m) {
    if (m.headAlias == 0xff) {
        RegisterAlias(d, m.headNetCode, m.headType, true);
        if (d.selfNet == 0) d.selfNet = m.headNetCode;   // 첫 갱신의 머리가 자기다(UpdateStream.cpp:157-169)
    }
    d.x = m.x;
    d.y = m.y;
    for (const nox::ObjEntry& o : m.objects)
        if (o.alias == 0xff) RegisterAlias(d, o.netCode, o.type, false);
}

void ReportDecodeFail(const Dummy& d, const uint8_t* b, size_t n, const char* why) {
    ++g_count.decodeFail;
    if (g_decodeFailShown >= 20) return;
    ++g_decodeFailShown;
    std::printf("[더미 %d] 못 읽음(%s) op=%02x 남은 %zu:", d.index, why, n ? b[0] : 0, n);
    for (size_t i = 0; i < n && i < 16; ++i) std::printf(" %02x", b[i]);
    std::printf("\n");
    std::fflush(stdout);
}

// 서버→클라 메시지 나누기 — opcount.cpp:133-197 과 같은 방식.
void WalkMessages(Dummy& d, const uint8_t* b, size_t all) {
    size_t eaten = 0;
    while (eaten < all) {
        bool allZero = true;                 // 조각 끝의 0 채움은 메시지가 아니다
        for (size_t i = eaten; i < all; ++i)
            if (b[i] != 0) { allZero = false; break; }
        if (allZero) return;

        const uint8_t op = b[eaten];
        size_t msz = nox::MessageSize(b + eaten, all - eaten, /*fromClient=*/false);

        if (op == static_cast<uint8_t>(Op::IMPORTANT)) {
            // aa 는 cc 봉투들의 목록이다. 표의 길이는 봉투 머리까지만 센 값이라 직접 훑는다.
            size_t q = eaten + 1;
            bool sane = false;
            while (q + 4 <= all && b[q] == static_cast<uint8_t>(Op::SEQ_IMPORTANT)) {
                const size_t inner = b[q + 3];
                if (q + 4 + inner > all) break;
                const uint16_t id = static_cast<uint16_t>(b[q + 1] | (b[q + 2] << 8));
                if (d.seenImportant[id]) ++g_count.dupImportant;
                d.seenImportant[id] = true;
                d.seenImportant[static_cast<uint16_t>(id + 0x8000)] = false;   // 번호가 한 바퀴 돌 때를 대비해 반대쪽을 비운다
                q += 4 + inner;
                sane = true;
            }
            msz = sane ? q - eaten : 1;
            // 정품은 aa 를 받을 때마다 같은 묶음 27 의 프레임으로 답한다(원본 기준 network_client.go:302-318).
            d.out.push_back(static_cast<uint8_t>(Op::IMPORTANT_ACK));
            Put32(d.out, d.frame);
            ++g_count.sentAck;
        } else if (msz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const nox::UpdateMsg m = nox::DecodeUpdate(b + eaten + 1, all - eaten - 1, &d.aliases);
            if (!m.ok) { ReportDecodeFail(d, b + eaten, all - eaten, "갱신"); return; }
            msz = 1 + m.consumed;
            OnUpdate(d, m);
        } else if (op == static_cast<uint8_t>(Op::TIMESTAMP) && msz >= 3) {
            const uint16_t low = static_cast<uint16_t>(b[eaten + 1] | (b[eaten + 2] << 8));
            uint32_t f = (d.frame & 0xFFFF0000u) | low;
            if (low < 0x4000 && (d.frame & 0xFFFF) > 0xC000) f += 0x10000;   // 하위 16비트가 한 바퀴 돌았다
            d.frame = f;
        } else if (op == static_cast<uint8_t>(Op::FULL_TIMESTAMP) && msz >= 5) {
            d.frame = static_cast<uint32_t>(b[eaten + 1]) | (static_cast<uint32_t>(b[eaten + 2]) << 8) |
                      (static_cast<uint32_t>(b[eaten + 3]) << 16) | (static_cast<uint32_t>(b[eaten + 4]) << 24);
        } else if (op == static_cast<uint8_t>(Op::USE_MAP) && msz >= 2) {
            // 맵이 바뀌면 서버가 준비를 끈다(Server.cpp:1910) — 준비를 안 보내면 중요 메시지가 멈춘다.
            d.aliases.Clear();
            const uint8_t ready = static_cast<uint8_t>(Op::CLIENT_READY);
            d.out.push_back(ready);
        }

        if (msz == 0 || eaten + msz > all) { ReportDecodeFail(d, b + eaten, all - eaten, "길이"); return; }
        eaten += msz;
    }
}

void OnDatagram(Dummy& d, uint8_t* p, size_t n) {
    ++g_count.recvDgram;
    g_count.recvBytes += n;
    if (n < nox::kWireHeaderSize) return;

    if (d.phase == Phase::TryJoin) {             // 키를 받기 전 — 평문
        if (n == 3 && p[2] == static_cast<uint8_t>(Op::SERVER_JOIN_OK)) {
            const uint8_t connect[3] = {0xff, 0x00, 0x00};
            SendRaw(d, connect, sizeof(connect));
            d.phase = Phase::Connect;
            d.retryAtUs = NowUs() + kJoinRetryUs;
        }
        return;
    }
    if (d.phase == Phase::Connect) {
        if (n == 10 && p[2] == static_cast<uint8_t>(Op::ACCEPTED)) {
            d.key = p[9];
            d.relNext = static_cast<uint8_t>(p[1] + 1);
            // 0x20 + 153바이트. 본문 66번이 직업(GameBalance.h:52 · Server.cpp:4268-4270).
            uint8_t accept[154] = {};
            accept[0] = static_cast<uint8_t>(Op::CLIENT_ACCEPT);
            accept[1 + 66] = static_cast<uint8_t>(d.cls);
            SendGame(d, accept, sizeof(accept));
            d.phase = Phase::Game;
            d.pingAtUs = NowUs() + kPingUs;
        }
        return;
    }

    nox::ApplyXor(p, n, d.key);
    if ((p[0] & 0x80) != 0 && (p[0] & 0x7f) == 0) {
        // 신뢰 조각은 차례대로만 받는다(원본 conn.go:695-698). 앞선 것 · 지난 것은 버리면 서버가 다시 보낸다.
        if (p[1] != d.relNext) return;
        ++d.relNext;
    }
    WalkMessages(d, p + nox::kWireHeaderSize, n - nox::kWireHeaderSize);
}

void DrainSocket(Dummy& d, std::vector<uint8_t>& buf) {
    for (;;) {
        const int n = recv(d.sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;   // 서버가 아직 안 떴을 때 오는 ICMP 답 — 한 번씩 소비된다
            return;
        }
        OnDatagram(d, buf.data(), static_cast<size_t>(n));
    }
}

void SendTryJoin(const Dummy& d) {
    uint8_t tryJoin[100] = {};
    tryJoin[2] = static_cast<uint8_t>(Op::SERVER_TRY_JOIN);
    SendRaw(d, tryJoin, sizeof(tryJoin));
}

// 정품 입력 꼴 — 방향 칸 둘이 기본(정품 R1 14바이트 4,921번 · 칸이 왜 둘인지는 까닭 미상이라 바이트 그대로).
void PutInput(std::vector<uint8_t>& o, uint8_t facing) {
    const uint8_t m[12] = {static_cast<uint8_t>(Op::PLAYER_INPUT), 0x0a, 0x01, 0, 0, 0, facing, 0x01, 0, 0, facing, facing};
    o.insert(o.end(), m, m + sizeof(m));
}

void FlushOut(Dummy& d, std::vector<uint8_t>& scratch) {
    scratch.clear();
    PutInput(scratch, d.facing);
    ++g_count.sentInput;
    // 입력을 맨 앞에 두고 쌓인 메시지를 잇는다. 상한을 넘으면 나눠 보낸다(메시지 경계에서).
    size_t pos = 0;
    while (pos < d.out.size()) {
        const size_t msz = nox::MessageSize(d.out.data() + pos, d.out.size() - pos, /*fromClient=*/true);
        if (msz == 0 || pos + msz > d.out.size()) {
            ReportDecodeFail(d, d.out.data() + pos, d.out.size() - pos, "보낼 메시지");
            break;
        }
        if (scratch.size() + msz > kMaxOutBody) {
            SendGame(d, scratch.data(), scratch.size());
            scratch.clear();
        }
        scratch.insert(scratch.end(), d.out.begin() + static_cast<std::ptrdiff_t>(pos),
                       d.out.begin() + static_cast<std::ptrdiff_t>(pos + msz));
        pos += msz;
    }
    d.out.clear();
    if (!scratch.empty()) SendGame(d, scratch.data(), scratch.size());
}

void Step(Dummy& d, int64_t nowUs, std::vector<uint8_t>& recvBuf, std::vector<uint8_t>& scratch) {
    switch (d.phase) {
        case Phase::WaitTurn:
            if (nowUs < d.joinAtUs) return;
            SendTryJoin(d);
            d.phase = Phase::TryJoin;
            d.retryAtUs = nowUs + kJoinRetryUs;
            return;
        case Phase::TryJoin:
            DrainSocket(d, recvBuf);
            if (d.phase == Phase::TryJoin && nowUs >= d.retryAtUs) {
                SendTryJoin(d);
                d.retryAtUs = nowUs + kJoinRetryUs;
            }
            return;
        case Phase::Connect:
            DrainSocket(d, recvBuf);
            if (d.phase == Phase::Connect && nowUs >= d.retryAtUs) {
                const uint8_t connect[3] = {0xff, 0x00, 0x00};
                SendRaw(d, connect, sizeof(connect));
                d.retryAtUs = nowUs + kJoinRetryUs;
            }
            return;
        case Phase::Game:
            break;
    }
    DrainSocket(d, recvBuf);
    if (nowUs >= d.pingAtUs) {                   // 정품 핑은 7바이트 혼자 나간다
        const uint32_t t = static_cast<uint32_t>(nowUs / 1000);
        const uint8_t ping[5] = {static_cast<uint8_t>(Op::CLIENT_PING), static_cast<uint8_t>(t), static_cast<uint8_t>(t >> 8),
                                 static_cast<uint8_t>(t >> 16), static_cast<uint8_t>(t >> 24)};
        SendGame(d, ping, sizeof(ping));
        ++g_count.sentPing;
        d.pingAtUs += kPingUs;
    }
    FlushOut(d, scratch);
}

void WaitUntilUs(HANDLE timer, int64_t targetUs) {
    const int64_t now = NowUs();
    if (targetUs <= now) return;
    LARGE_INTEGER due;
    due.QuadPart = -(targetUs - now) * 10;       // 100ns 단위 · 음수 = 지금부터
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

void PrintCounters(const char* head, int sec, int entered, int total, const Counters& c) {
    std::printf("%s 초 %d · 입장 %d/%d", head, sec, entered, total);
#define X(n) std::printf(" %s=%llu", #n, static_cast<unsigned long long>(c.n));
    NOX_DUMMY_COUNTERS(X)
#undef X
    std::printf("\n");
    std::fflush(stdout);
}

Counters Delta(const Counters& now, const Counters& before) {
    Counters d;
#define X(n) d.n = now.n - before.n;
    NOX_DUMMY_COUNTERS(X)
#undef X
    return d;
}

bool ParseArgs(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        const auto need = [&](int& out) {
            if (!v) { err = k + " 값 없음"; return false; }
            out = std::atoi(v);
            ++i;
            return true;
        };
        if (k == "--server") {
            if (!v) { err = "--server 값 없음"; return false; }
            std::string s = v;
            ++i;
            const size_t c = s.find(':');
            if (c != std::string::npos) {
                a.port = static_cast<uint16_t>(std::atoi(s.c_str() + c + 1));
                s.resize(c);
            }
            a.host = s;
        } else if (k == "--count") { if (!need(a.count)) return false; }
        else if (k == "--room-size") { if (!need(a.roomSize)) return false; }
        else if (k == "--duration-sec") { if (!need(a.durationSec)) return false; }
        else if (k == "--join-interval-ms") { if (!need(a.joinIntervalMs)) return false; }
        else if (k == "--seed") {
            int s = 0;
            if (!need(s)) return false;
            a.seed = static_cast<uint32_t>(s);
        } else if (k == "--classes") {
            if (!v) { err = "--classes 값 없음"; return false; }
            a.classes.clear();
            for (const char* p = v; *p;) {
                a.classes.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
            ++i;
        } else {
            err = "모르는 인자 " + k;
            return false;
        }
    }
    if (a.count < 1) { err = "--count 는 1 이상"; return false; }
    if (a.roomSize < 1) { err = "--room-size 는 1 이상"; return false; }
    if (a.count % a.roomSize != 0) { err = "더미 수가 방당 인원의 배수가 아니다"; return false; }
    if (a.classes.empty()) { err = "--classes 가 비었다"; return false; }
    for (int c : a.classes)
        if (c < 0 || c > 2) { err = "직업은 0 전사 · 1 위저드 · 2 컨저러"; return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::string err;
    if (!ParseArgs(argc, argv, a, err)) {
        std::printf("인자 오류: %s\n", err.c_str());
        std::printf("쓰기: nox_dummy [--server 주소:포트] [--count N] [--room-size K] [--classes 0,1] [--duration-sec S] "
                    "[--join-interval-ms M] [--seed S]\n");
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("WSAStartup 실패\n"); return 1; }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(a.port);
    if (inet_pton(AF_INET, a.host.c_str(), &to.sin_addr) != 1) { std::printf("주소를 못 읽음: %s\n", a.host.c_str()); return 1; }

    std::vector<Dummy> dummies(static_cast<size_t>(a.count));
    std::vector<std::vector<int>> slots(kWheelSlots);
    const int64_t t0 = NowUs();
    for (int i = 0; i < a.count; ++i) {
        Dummy& d = dummies[static_cast<size_t>(i)];
        d.index = i;
        d.cls = a.classes[static_cast<size_t>(i) % a.classes.size()];
        d.joinAtUs = t0 + static_cast<int64_t>(i) * a.joinIntervalMs * 1000;
        d.seenImportant.assign(65536, false);
        d.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (d.sock == INVALID_SOCKET) { std::printf("소켓 실패 %d\n", WSAGetLastError()); return 1; }
        u_long nonBlocking = 1;
        ioctlsocket(d.sock, FIONBIO, &nonBlocking);
        if (connect(d.sock, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) { std::printf("connect 실패 %d\n", WSAGetLastError()); return 1; }
        slots[0].push_back(i);
    }

    // 서버와 같은 고해상도 대기 타이머(main.cpp:850) — Sleep(1) 은 약 15.5ms 를 자서 보내는 간격이 흔들린다.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        std::printf("[더미] 정밀 타이머를 못 만들어 보통 타이머로 기다린다 — 간격이 약 15.6ms 단위로 흔들린다\n");
    }

    std::vector<uint8_t> recvBuf(65536);
    std::vector<uint8_t> scratch;
    scratch.reserve(2048);
    Counters printed;
    int64_t base = t0;
    int64_t nextStat = t0 + 1000000;
    const int64_t endAt = t0 + static_cast<int64_t>(a.durationSec) * 1000000;
    int sec = 0;

    const auto countEntered = [&] {
        int n = 0;
        for (const Dummy& d : dummies)
            if (d.phase == Phase::Game) ++n;
        return n;
    };

    for (;;) {
        for (int s = 0; s < kWheelSlots; ++s) {
            if (slots[static_cast<size_t>(s)].empty()) continue;
            WaitUntilUs(timer, base + static_cast<int64_t>(s) * 1000);
            const int64_t now = NowUs();
            for (int i : slots[static_cast<size_t>(s)]) Step(dummies[static_cast<size_t>(i)], now, recvBuf, scratch);
        }
        const int64_t after = NowUs();
        const int64_t next = base + kWheelUs;
        if (after > next) {                      // 한 바퀴가 33ms 를 넘겼다 — 몰아 보내지 않고 지금부터 다시 센다
            ++g_count.lateTicks;
            base = after;
        } else {
            base = next;
        }
        if (after >= nextStat) {
            ++sec;
            PrintCounters("[더미]", sec, countEntered(), a.count, Delta(g_count, printed));
            printed = g_count;
            nextStat += 1000000;
        }
        if (after >= endAt) break;
    }

    PrintCounters("[더미 요약]", sec, countEntered(), a.count, g_count);
    for (Dummy& d : dummies) closesocket(d.sock);
    WSACleanup();
    return 0;
}
