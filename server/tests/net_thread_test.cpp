// net_thread_test.cpp — 네트워크 스레드(`CNetworkThread`) 시험. 로컬 UDP 소켓으로 실제로 주고받는다.
//   `proto_test.cpp` 와 따로 둔 까닭 — winsock(windows.h)의 min/max 매크로가 그쪽 시험 본문과 부딪힌다.
//   결과는 (통과, 한 줄) 목록으로 돌려주고, `proto_test.cpp` 의 `TestNetworkThread` 가 `Check` 로 센다.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nox/NetworkThread.h"
#include "nox/Opcode.h"

namespace {

uint32_t Now() { return static_cast<uint32_t>(GetTickCount64()); }

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

// ① 접속 → 접속 수락이 소켓으로 돌아오고 CONNECTED 이벤트가 큐로 온다(넣은 시각이 찍혀)
// ② 게임 요청(보내기)을 넣으면 깨어나 곧 보낸다 — 기다림 최대를 5초로 두었으니 깨우기가 없으면 다음 타이머(2초)까지 늦는다
// ③ 확인이 없으면 접속 수락을 보낸 지 2초에 다시 받는다 — 타이머 마감에 깨어난다(깨지 않으면 5초)
// ④ `Stop` — 스레드가 끝나고 두 번 불러도 된다
std::vector<std::pair<bool, std::string>> RunNetworkThreadChecks() {
    std::vector<std::pair<bool, std::string>> r;
    char m[240];
    nox::CNetworkThread net;
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
    std::snprintf(m, sizeof(m), "① 스레드 — 접속 수락 받음 %d · CONNECTED 이벤트 세션 %lld · 넣은 시각 찍힘 %d",
                  (int)IsAccept(acc), (long long)sid, (int)(queuedMs != 0));
    r.push_back({ IsAccept(acc) && sid != 0 && queuedMs != 0, m });

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
    return r;
}
