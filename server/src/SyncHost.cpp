// SyncHost.cpp — 한 스레드 조립. 설명은 `SyncHost.h` 머리.
#include "nox/SyncHost.h"

namespace nox {

size_t CSyncHost::OnPacket(const std::string& from, const uint8_t* data, size_t n, std::vector<OutPacket>& out) {
    TakeOut(out);   // 앞서 쌓인 요청부터 — 옛 게임은 요청마다 그 자리에서 통신 층에 넘겼다
    std::vector<NetworkEvent> events;
    size_t count = _network.OnDatagram(from, data, n, out, events);
    for (const NetworkEvent& ev : events) count += _rooms.OnNetworkEvent(ev);
    TakeOut(out);
    return count;
}

int CSyncHost::Tick(uint32_t nowMs, std::vector<OutPacket>& out) {
    TakeOut(out);
    const int ticks = _rooms.BeginTick(nowMs);
    if (ticks == 0) return 0;   // 옛 `Tick` 도 여기서 돌아갔다 — 통신 층 Tick 도 안 불렀다
    // 옛 `Tick` 안의 자리 그대로 — 다시 보내기가 이 틱의 게임 조각보다 먼저 나가고, 시간 초과 알림은 「자리 비우기」 전에 든다
    std::vector<NetworkEvent> events;
    _network.Tick(nowMs, out, events);
    for (const NetworkEvent& ev : events) _rooms.OnNetworkEvent(ev);
    _rooms.RunTick();
    TakeOut(out);
    return ticks;
}

void CSyncHost::TakeOut(std::vector<OutPacket>& out) {
    _rooms.SwapOutNetworkRequests(_requests);
    for (const NetworkRequest& r : _requests) _network.OnRequest(r, out);
    _requests.clear();
}

int64_t CSyncHost::DebugEnterGame(const std::string& address, uint16_t netCode, bool observer, CharClass cls) {
    return _rooms.Game().DebugEnterGame(_network.OpenSession(address), address, netCode, observer, cls);
}

}  // namespace nox
