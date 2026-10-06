// Lobby.cpp — 명령 → 답. 답의 꼴은 모두 1단계 녹음(XWIS 원래 줄)을 따른다.
//   표 = Docs/LOBBY_DESIGN.md 「명령 → 답」 · 녹음 = _evidence/_wol_record_20261006/relay_log_20261006_233255.txt
#include "nox/Lobby.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace nox::wol {

namespace {

// XWIS 의 채팅 로비 셋 — 화면 이름 Brin · Ix · Dun Mir (opennox xwis.go:22-26). 녹음 LIST 답 그대로.
const char* const kChatLobbies[] = {"#Lob_37_0", "#Lob_37_1", "#Lob_37_2"};

bool IsChatLobby(const std::string& ch) {
    for (const char* l : kChatLobbies)
        if (ch == l) return true;
    return false;
}

// 빈칸 하나마다 자른다 — 빈 칸도 남긴다(손님 접속의 `whereto   9472 …` 처럼 빈 이름이 온다).
std::vector<std::string> Split(std::string_view s) {
    std::vector<std::string> t;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(' ', p);
        if (q == std::string_view::npos) {
            t.emplace_back(s.substr(p));
            break;
        }
        t.emplace_back(s.substr(p, q - p));
        p = q + 1;
    }
    return t;
}

// 명령 이름은 대소문자가 섞여 온다(`verchk` · `CVERS` — 녹음 그대로).
std::string Upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

std::string IpDotted(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xff) + "." +
           std::to_string((ip >> 8) & 0xff) + "." + std::to_string(ip & 0xff);
}

Lobby::Lobby(LobbyConfig cfg) : cfg_(std::move(cfg)), roomInfo_(Pack(cfg_.room)) {}

void Lobby::OnConnect(int conn, uint16_t port, uint32_t peerIp, uint32_t localIp) {
    Conn c;
    c.port = port;
    c.peerIp = peerIp;
    c.localIp = localIp;
    conns_[conn] = c;
}

void Lobby::OnClose(int conn) {
    for (auto& kv : members_) kv.second.erase(conn);
    conns_.erase(conn);
}

std::vector<std::string> Lobby::TakeLog() {
    std::vector<std::string> r;
    r.swap(log_);
    return r;
}

void Lobby::OnLine(int conn, std::string_view line, std::vector<Out>& out) {
    auto it = conns_.find(conn);
    if (it == conns_.end() || line.empty()) return;
    const auto t = Split(line);
    const std::string cmd = Upper(t[0]);
    if (it->second.port == cfg_.servServPort) OnServServ(conn, it->second, cmd, line, out);
    else OnChat(conn, it->second, cmd, t, line, out);
}

// 4005 — 로비 서버 찾기. 녹음: verchk ×2 · lobcount · whereto · QUIT → 610 · 605 · 607
void Lobby::OnServServ(int conn, Conn& c, const std::string& cmd, std::string_view line, std::vector<Out>& out) {
    if (cmd == "VERCHK") return;   // 4005 에선 XWIS 도 답하지 않았다
    if (cmd == "LOBCOUNT") {
        out.push_back({conn, ": 610 u 1"});
        return;
    }
    if (cmd == "WHERETO") {
        // 로비 주소 = 클라가 우리에게 붙은 주소. 뒤의 `-8 36.1083 -115.0582` 는 XWIS 그대로(뜻 확인 안 함)
        out.push_back({conn, ": 605 u :" + IpDotted(c.localIp) + " " + std::to_string(cfg_.lobbyPort) + " '0:" +
                                 cfg_.serverName + "' -8 36.1083 -115.0582"});
        return;
    }
    if (cmd == "QUIT") {
        out.push_back({conn, ": 607", true});
        return;
    }
    Unknown(cmd, line);
}

// 4000 — 로그인 · 목록 · 채팅 로비 · 방 참가
void Lobby::OnChat(int conn, Conn& c, const std::string& cmd, const std::vector<std::string>& t,
                   std::string_view line, std::vector<Out>& out) {
    // 답이 없는 줄(녹음 그대로). 이름·암호는 검사하지 않는다 — 2단계는 아무 이름이나 받는다
    if (cmd == "CVERS" || cmd == "PASS" || cmd == "APGAR" || cmd == "SERIAL" || cmd == "SETOPT") return;
    if (cmd == "NICK") {
        if (t.size() > 1) c.nick = t[1];
        return;
    }
    if (cmd == "USER") {   // 로그인 묶음의 끝 — 인사(375 · 372…) 뒤 376 이 「로그인 끝」
        out.push_back({conn, ": 375 u :- " + (cfg_.motd.empty() ? std::string() : cfg_.motd[0])});
        for (size_t i = 1; i < cfg_.motd.size(); ++i) out.push_back({conn, ": 372 u :- " + cfg_.motd[i]});
        out.push_back({conn, ": 376 u"});
        return;
    }
    if (cmd == "VERCHK") {   // 패치 확인 — 「패치 없음」 글자 그대로
        out.push_back({conn, ": 379 u :none none none 1 32512 NONREQ"});
        return;
    }
    if (cmd == "SETCODEPAGE") {
        c.codepage = t.size() > 1 ? std::atoi(t[1].c_str()) : 0;
        out.push_back({conn, ": 329 u " + std::to_string(c.codepage)});
        return;
    }
    if (cmd == "GETCODEPAGE") {   // `<이름>`<코드페이지>` — 모르는 이름은 0(녹음: 손님은 `Guest`0`)
        std::string r = ": 328 u";
        for (size_t i = 1; i < t.size(); ++i) {
            if (t[i].empty()) continue;
            int cp = 0;
            if (t[i] == cfg_.roomHost) cp = 1252;   // 가짜 방장 — 녹음의 방장(tak7)이 1252
            for (const auto& kv : conns_)
                if (kv.second.nick == t[i]) cp = kv.second.codepage;
            r += " " + t[i] + "`" + std::to_string(cp);
        }
        out.push_back({conn, r});
        return;
    }
    if (cmd == "LIST") {   // `LIST -1 37` = 채팅 로비 + 게임 방, `LIST 37 37` = 게임 방만(녹음: 게임 버튼 뒤 목록엔 327 이 없다)
        SendList(conn, t.size() > 1 && t[1] == "-1", out);
        return;
    }
    if (cmd == "JOIN") {   // 채팅 로비 입장 — `JOIN #Lob_37_0 zot0blat`(뒤 칸은 채널 열쇠로 보임, 안 본다)
        if (t.size() < 2 || !IsChatLobby(t[1])) {
            log_.push_back("[모르는 채널] " + std::string(line));
            return;
        }
        members_[t[1]].insert(conn);
        out.push_back({conn, ":" + c.nick + "!u@h JOIN :0,0 " + t[1]});
        out.push_back({conn, ": 353 u * " + t[1] + " :" + Members353(t[1], false)});
        out.push_back({conn, ": 366 u " + t[1] + " :"});
        return;
    }
    if (cmd == "PART") {
        if (t.size() < 2) return;
        if (auto m = members_.find(t[1]); m != members_.end()) m->second.erase(conn);
        out.push_back({conn, ":" + c.nick + "!u@h PART " + t[1]});
        return;
    }
    if (cmd == "JOINGAME") {   // 방 참가 — 방장 주소가 353 에 실린다(326 과 같은 값)
        const std::string room = RoomChannel();
        if (t.size() < 2 || t[1] != room) {
            log_.push_back("[모르는 방] " + std::string(line));
            return;
        }
        members_[room].insert(conn);
        // `1 32 37 1 0 <내 IP> 1` — 녹음 그대로, 내 IP 만 바꾼다(앞 칸들 뜻 확인 안 함)
        out.push_back({conn, ":" + c.nick + "!u@h JOINGAME 1 32 37 1 0 " + std::to_string(c.peerIp) + " 1 :" + room});
        out.push_back({conn, ": 332 u " + room + " " + roomInfo_});   // roomInfo_ 가 ':' 로 시작한다
        out.push_back({conn, ": 353 u = " + room + " :" + Members353(room, true)});
        out.push_back({conn, ": 366 u " + room + " :"});
        return;
    }
    if (cmd == "QUIT") {
        out.push_back({conn, ": 607", true});
        return;
    }
    Unknown(cmd, line);
}

void Lobby::SendList(int conn, bool withChat, std::vector<Out>& out) {
    out.push_back({conn, ": 321 u:"});
    const std::string room = RoomChannel();
    // 셋째 칸 = 채널 안 사람 수로 보인다(녹음: 전용 서버 방 Anwserver 는 덩어리 인원 0 인데 이 칸 1) → 가짜 방장 1 + 참가자
    // `41681088` 은 뜻 모름 — 녹음 다섯 방 중 셋의 값(나머지 13893824 · 13894208)
    const size_t users = 1 + members_[room].size();
    out.push_back({conn, ": 326 u " + room + " " + std::to_string(users) + " 0 37 1 41681088 " +
                             std::to_string(cfg_.gameServerIp) + " " + std::string(kListPrefix) + roomInfo_});
    if (withChat)
        for (const char* l : kChatLobbies)
            out.push_back({conn, ": 327 u " + std::string(l) + " " + std::to_string(members_[l].size()) + " 0 388"});
    out.push_back({conn, ": 323 u:"});
}

// 353 의 이름 목록. 채팅 로비 = `<이름>,0,0` · 게임 방 = `@<방장>,0,<게임 서버 IP> <이름>,0,<그 사람 IP>`
std::string Lobby::Members353(const std::string& channel, bool gameRoom) const {
    std::string r;
    if (gameRoom) r = "@" + cfg_.roomHost + ",0," + std::to_string(cfg_.gameServerIp);
    const auto m = members_.find(channel);
    if (m == members_.end()) return r;
    for (int id : m->second) {
        const auto c = conns_.find(id);
        if (c == conns_.end()) continue;
        if (!r.empty()) r += " ";
        r += c->second.nick + ",0," + (gameRoom ? std::to_string(c->second.peerIp) : std::string("0"));
    }
    return r;
}

// 모르는 명령은 답하지 않고 처음 한 번만 기록한다(게임 서버의 「처음 본 오프코드」와 같은 방식).
void Lobby::Unknown(const std::string& cmd, std::string_view line) {
    if (unknownSeen_.insert(cmd).second) log_.push_back("[처음 본 명령] " + std::string(line));
}

}  // namespace nox::wol
