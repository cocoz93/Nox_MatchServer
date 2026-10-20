// RoomMirror.cpp — 로비의 방 거울. 설명은 `RoomMirror.h` 머리 · 줄 꼴은 `LobbyLink.h`.
#include "nox/RoomMirror.h"

#include <algorithm>
#include <utility>

namespace nox::wol {

RoomMirror::RoomMirror(const JoinKey& tokenKey) : tokenKey_(tokenKey), authKey_(LinkAuthKey(tokenKey)) {}

std::string RoomMirror::Name(int conn, const Server& s) const {
    std::string n = "c" + std::to_string(conn);
    if (s.stage == Stage::Registered) n += " " + (s.ip ? IpDotted(s.ip) : std::string("(로비 쪽 주소)")) + ":" + std::to_string(s.port);
    return n;
}

void RoomMirror::Bye(int conn, const std::string& why, std::vector<Out>& out) {
    out.push_back({conn, LinkBye(why), true});
    servers_[conn].stage = Stage::Closing;
}

void RoomMirror::OnLinkOpen(int conn, std::vector<Out>& out) {
    ++stats_.linksOpened;
    Server s;
    s.nonce = NewLinkNonceHex();
    out.push_back({conn, LinkHello(s.nonce)});
    servers_[conn] = std::move(s);
    log_.push_back("[게임 서버] c" + std::to_string(conn) + " 붙음 — 확인 기다림");
}

void RoomMirror::OnLinkClose(int conn) {
    const auto it = servers_.find(conn);
    if (it == servers_.end()) return;
    ++stats_.linksClosed;
    Server& s = it->second;
    if (s.stage == Stage::Registered)
        log_.push_back("[게임 서버] " + Name(conn, s) + " 끊김 — 방 " + std::to_string(s.rooms.size()) + "개 목록에서 내림 · 기다리던 새 방 " +
                       std::to_string(s.asked.size()) + "개 실패");
    else
        log_.push_back("[게임 서버] c" + std::to_string(conn) + " 끊김(등록 전)");
    const std::set<uint32_t> asked = std::move(s.asked);
    servers_.erase(it);   // 로비에 넘기기 전에 지운다 — 로비가 곧바로 목록을 다시 그려도 끊긴 방이 안 나온다
    for (const uint32_t req : asked) {
        ++stats_.failedOnClose;
        if (lobby_) lobby_->OnRoomCreateFailed(req, "게임 서버 연결 끊김");
    }
}

void RoomMirror::OnLinkLine(int conn, std::string_view line, std::vector<Out>& out) {
    const auto it = servers_.find(conn);
    if (it == servers_.end()) return;
    Server& s = it->second;
    if (s.stage == Stage::Closing) return;   // BYE 를 보냈다 — 끊김까지 온 줄은 버린다(BYE 가 둘이 되지 않게)
    LinkLine l;
    const LinkParse p = ParseLinkLine(line, l);

    // 등록 전 — 정해진 차례(AUTH → REGISTER)만 받는다. 그 밖은 끊는다(같은 설정을 쥔 게임 서버가 아니거나 규약이 어긋남)
    if (s.stage == Stage::WaitAuth) {
        if (p != LinkParse::Ok || l.kind != LinkKind::Auth || !CheckLinkAuth(authKey_, s.nonce, l.text)) {
            ++stats_.authFailed;
            log_.push_back("[게임 서버] c" + std::to_string(conn) + " 확인 실패 — 끊음(첫 줄 " + std::string(line.substr(0, 16)) + "…)");
            Bye(conn, "auth failed", out);
            return;
        }
        ++stats_.authOk;
        s.stage = Stage::WaitRegister;
        return;
    }
    if (s.stage == Stage::WaitRegister) {
        if (p != LinkParse::Ok || l.kind != LinkKind::Register) {
            ++stats_.outOfOrder;
            log_.push_back("[게임 서버] c" + std::to_string(conn) + " 등록 전 다른 줄 — 끊음: " + std::string(line));
            Bye(conn, "register first", out);
            return;
        }
        // 같은 주소 · 포트는 하나만(IP 하나에 게임 서버 하나 · 키가 같아진다). 주소 0(로비 쪽 주소)도 하나만
        for (const auto& kv : servers_) {
            if (kv.first == conn || kv.second.stage != Stage::Registered) continue;
            if (kv.second.ip == l.ip && kv.second.port == l.port) {
                ++stats_.registerRejected;
                log_.push_back("[게임 서버] c" + std::to_string(conn) + " 등록 거절 — " + Ipv4ToString(l.ip) + ":" + std::to_string(l.port) +
                               " 는 " + Name(kv.first, kv.second) + " 가 이미 씀 · 끊음");
                Bye(conn, "same address registered", out);
                return;
            }
        }
        ++stats_.registered;
        s.stage = Stage::Registered;
        s.epoch = l.epoch;
        s.ip = l.ip;
        s.port = l.port;
        s.tokenKey = GameServerTokenKey(tokenKey_, l.epoch, l.ip, l.port);
        log_.push_back("[게임 서버] " + Name(conn, s) + " 등록 — 실행 " + std::to_string(s.epoch));
        return;
    }

    // 등록 뒤 — 못 읽는 줄 · 게임 서버가 보낼 리 없는 줄은 버리고 센다(한 줄 때문에 그 게임 서버의 방을 다 내리지 않는다)
    if (p != LinkParse::Ok) {
        ++stats_.badLines;
        log_.push_back("[게임 서버] " + Name(conn, s) + " 못 읽는 줄(" + LinkParseName(p) + ") — 버림: " + std::string(line.substr(0, 80)));
        return;
    }
    switch (l.kind) {
        case LinkKind::Room:
            ++stats_.roomLines;
            s.rooms[l.room.id] = l.room;
            return;
        case LinkKind::RoomGone:
            ++stats_.roomGoneLines;
            if (s.rooms.erase(l.room.id) == 0) ++stats_.roomGoneUnknown;
            return;
        case LinkKind::Created:
        case LinkKind::CreateFailed: {
            if (s.asked.erase(l.req) == 0) {
                ++stats_.unknownReplies;
                log_.push_back("[게임 서버] " + Name(conn, s) + " 안 물은 요청 " + std::to_string(l.req) + " 의 답 — 버림");
                return;
            }
            if (l.kind == LinkKind::CreateFailed) {
                ++stats_.createFailedReplies;
                if (lobby_) lobby_->OnRoomCreateFailed(l.req, "게임 서버 " + Name(conn, s) + " — " + l.text);
                return;
            }
            const auto r = s.rooms.find(l.room.id);
            if (r == s.rooms.end()) {   // 규약은 「CREATED 앞에 그 방의 ROOM」 — 없으면 목록에도 없는 방이라 들일 수 없다
                ++stats_.createdNoRoom;
                log_.push_back("[게임 서버] " + Name(conn, s) + " 요청 " + std::to_string(l.req) + " 의 방 " + std::to_string(l.room.id) +
                               " 이 거울에 없음(ROOM 이 먼저 안 옴) — 실패로");
                if (lobby_) lobby_->OnRoomCreateFailed(l.req, "만든 방의 ROOM 이 안 옴");
                return;
            }
            ++stats_.createdReplies;
            if (lobby_) lobby_->OnRoomCreated(l.req, ToLobbyRoom(s, r->second), out);
            return;
        }
        case LinkKind::Result:
            ++stats_.results;
            results_.push_back(l.text);
            return;
        case LinkKind::Hello:
        case LinkKind::Auth:
        case LinkKind::Register:
        case LinkKind::Create:
        case LinkKind::Bye:
        case LinkKind::kCount:
            break;
    }
    ++stats_.badLines;
    log_.push_back("[게임 서버] " + Name(conn, s) + " 등록 뒤 맞지 않는 줄 — 버림: " + std::string(line.substr(0, 80)));
}

LobbyRoom RoomMirror::ToLobbyRoom(const Server& s, const LinkRoom& r) const {
    LobbyRoom lr;
    lr.gameRoomId = static_cast<uint16_t>(r.id);
    lr.host = r.host.empty() ? r.creator : r.host;
    lr.name = r.name;
    lr.map = r.map;
    lr.mode = r.mode;
    lr.players = static_cast<uint8_t>(std::min<uint32_t>(r.players, 255));
    lr.maxPlayers = static_cast<uint8_t>(std::min<uint32_t>(r.maxPlayers, 255));
    lr.playing = r.playing;
    lr.gameIp = s.ip;   // 0 이면 로비가 그 클라 접속의 로비 쪽 주소를 쓴다(같은 PC)
    lr.gamePort = s.port;
    lr.ownTokenKey = true;
    lr.tokenKey = s.tokenKey;
    return lr;
}

std::vector<LobbyRoom> RoomMirror::LobbyRooms() const {
    std::vector<LobbyRoom> v;
    for (const auto& kv : servers_) {
        if (kv.second.stage != Stage::Registered) continue;
        for (const auto& rr : kv.second.rooms) v.push_back(ToLobbyRoom(kv.second, rr.second));
    }
    return v;
}

// 새 방을 놓을 게임 서버 = 방이 가장 적은 곳(같으면 먼저 붙은 곳). 부하 보고는 범위 밖(플랜 「범위 밖」)
RoomRequest RoomMirror::RequestRoom(uint32_t req, const std::string& creator, LobbyRoom& /*out*/, std::string& why) {
    int best = 0;
    size_t bestRooms = 0;
    for (const auto& kv : servers_) {
        if (kv.second.stage != Stage::Registered) continue;
        if (best == 0 || kv.second.rooms.size() < bestRooms) {
            best = kv.first;
            bestRooms = kv.second.rooms.size();
        }
    }
    if (best == 0) {
        ++stats_.noServer;
        why = "등록된 게임 서버 없음";
        return RoomRequest::Failed;
    }
    Server& s = servers_[best];
    s.asked.insert(req);
    ++stats_.createsSent;
    pendingOut_.push_back({best, LinkCreate(req, creator)});
    log_.push_back("[게임 서버] " + Name(best, s) + " 에 새 방 요청 " + std::to_string(req) + " — 만든 사람 " + creator);
    return RoomRequest::Pending;
}

void RoomMirror::CancelRoom(uint32_t req) {
    for (auto& kv : servers_)
        if (kv.second.asked.erase(req)) ++stats_.cancelled;
}

void RoomMirror::TakeOut(std::vector<Out>& out) {
    for (Out& o : pendingOut_) out.push_back(std::move(o));
    pendingOut_.clear();
}

std::vector<std::string> RoomMirror::TakeLog() {
    std::vector<std::string> r;
    r.swap(log_);
    return r;
}

std::vector<std::string> RoomMirror::TakeResults() {
    std::vector<std::string> r;
    r.swap(results_);
    return r;
}

size_t RoomMirror::ServerCount() const {
    size_t n = 0;
    for (const auto& kv : servers_) n += kv.second.stage == Stage::Registered ? 1 : 0;
    return n;
}

size_t RoomMirror::RoomCount() const {
    size_t n = 0;
    for (const auto& kv : servers_) n += kv.second.rooms.size();
    return n;
}

}  // namespace nox::wol
