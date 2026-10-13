// Router.cpp — 로비 찾기(4005) 명령 → 답. 설명은 `Router.h` 머리.
//   녹음 = _evidence/_wol_record_20261006/relay_log_20261006_233255.txt (c1 · c3 · c5 · c6 · c8 — 다섯 다 같은 다섯 줄)
#include "nox/Router.h"

#include <cctype>

namespace nox::wol {

void Router::OnConnect(int conn, uint32_t peerIp, uint32_t localIp) {
    Conn c;
    c.peerIp = peerIp;
    c.localIp = localIp;
    conns_[conn] = c;
}

void Router::OnClose(int conn) {
    conns_.erase(conn);
}

std::vector<std::string> Router::TakeLog() {
    std::vector<std::string> r;
    r.swap(log_);
    return r;
}

void Router::OnLine(int conn, std::string_view line, std::vector<Out>& out) {
    auto it = conns_.find(conn);
    if (it == conns_.end() || line.empty()) return;
    // 명령 이름은 대소문자가 섞여 온다(`verchk` · `QUIT` — 녹음 그대로)
    std::string cmd(line.substr(0, line.find(' ')));
    for (char& ch : cmd) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    if (cmd == "VERCHK") return;   // 4005 에선 XWIS 도 답하지 않았다
    if (cmd == "LOBCOUNT") {
        out.push_back({conn, ": 610 u 1"});
        return;
    }
    if (cmd == "WHERETO") {
        // 채널 주소 — 설정이 없으면 클라가 우리에게 붙은 주소(같은 PC). 뒤의 `-8 36.1083 -115.0582` 는 XWIS 그대로(뜻 확인 안 함)
        const uint32_t ip = cfg_.channelIp ? cfg_.channelIp : it->second.localIp;
        out.push_back({conn, ": 605 u :" + IpDotted(ip) + " " + std::to_string(cfg_.channelPort) + " '0:" + cfg_.serverName +
                                 "' -8 36.1083 -115.0582"});
        return;
    }
    if (cmd == "QUIT") {
        out.push_back({conn, ": 607", true});
        return;
    }
    // 모르는 명령은 답하지 않고 처음 한 번만 기록한다(로비의 같은 규칙)
    if (unknownSeen_.insert(cmd).second) log_.push_back("[처음 본 명령] " + std::string(line));
}

}  // namespace nox::wol
