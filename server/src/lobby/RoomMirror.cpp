// RoomMirror.cpp — 로비의 방 거울. 설명은 `RoomMirror.h` 머리 · 줄 꼴은 `LobbyLink.h`.
#include "nox/RoomMirror.h"

#include <algorithm>
#include <utility>

namespace nox::wol {

RoomMirror::RoomMirror(const JoinKey& tokenKey) : tokenKey_(tokenKey), authKey_(LinkAuthKey(tokenKey)) {}

std::string RoomMirror::Name(int conn, const Server& s) const {
    std::string n = "c" + std::to_string(conn);
    if (s.registered) n += " " + (s.ip ? IpDotted(s.ip) : std::string("(로비 쪽 주소)")) + ":" + std::to_string(s.port);
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
    s.lastRecvMs = nowMs_();   // 붙은 때부터 잰다 — 확인 줄도 안 보내는 접속도 한도에 끊긴다
    s.nextPingMs = s.lastRecvMs + kLinkPingEveryMs;
    out.push_back({conn, LinkHello(s.nonce)});
    servers_[conn] = std::move(s);
    log_.push_back("[게임 서버] c" + std::to_string(conn) + " 붙음 — 확인 기다림");
}

void RoomMirror::OnLinkClose(int conn, std::vector<Out>& out) {
    const auto it = servers_.find(conn);
    if (it == servers_.end()) return;
    ++stats_.linksClosed;
    Server& s = it->second;
    if (s.registered)   // ★ 7단계 — 무소식으로 BYE 를 보낸(닫는 중) 게임 서버도 등록했던 것(옛 조건 `stage == Registered` 는 닫는 중을 「등록 전」으로 찍었다)
        log_.push_back("[게임 서버] " + Name(conn, s) + " 끊김 — 방 " + std::to_string(s.rooms.size()) + "개 목록에서 내림 · 기다리던 새 방 " +
                       std::to_string(s.asked.size()) + "개 실패");
    else
        log_.push_back("[게임 서버] c" + std::to_string(conn) + " 끊김(등록 전)");
    const std::set<uint32_t> asked = std::move(s.asked);
    servers_.erase(it);   // 로비에 넘기기 전에 지운다 — 로비가 곧바로 목록을 다시 그려도 끊긴 방이 안 나온다
    for (const uint32_t req : asked) {
        ++stats_.failedOnClose;
        if (lobby_) lobby_->OnRoomCreateFailed(req, "게임 서버 연결 끊김", out);
    }
}

void RoomMirror::OnLinkLine(int conn, std::string_view line, std::vector<Out>& out) {
    const auto it = servers_.find(conn);
    if (it == servers_.end()) return;
    Server& s = it->second;
    if (s.stage == Stage::Closing) return;   // BYE 를 보냈다 — 끊김까지 온 줄은 버린다(BYE 가 둘이 되지 않게)
    s.lastRecvMs = nowMs_();   // ★ 7단계 — 어떤 줄이든(못 읽는 줄도) 살아 있다는 뜻
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
        s.registered = true;
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
                if (lobby_) lobby_->OnRoomCreateFailed(l.req, "게임 서버 " + Name(conn, s) + " — " + l.text, out);
                return;
            }
            const auto r = s.rooms.find(l.room.id);
            if (r == s.rooms.end()) {   // 규약은 「CREATED 앞에 그 방의 ROOM」 — 없으면 목록에도 없는 방이라 들일 수 없다
                ++stats_.createdNoRoom;
                log_.push_back("[게임 서버] " + Name(conn, s) + " 요청 " + std::to_string(l.req) + " 의 방 " + std::to_string(l.room.id) +
                               " 이 거울에 없음(ROOM 이 먼저 안 옴) — 실패로");
                if (lobby_) lobby_->OnRoomCreateFailed(l.req, "만든 방의 ROOM 이 안 옴", out);
                return;
            }
            ++stats_.createdReplies;
            if (lobby_) lobby_->OnRoomCreated(l.req, ToLobbyRoom(s, r->second), out);
            return;
        }
        case LinkKind::Result: {
            ++stats_.results;
            results_.push_back(l.text);   // 기록용(`lobby_main` 이 [판 결과] 한 줄)
            // ★ 7단계 조각 5 — 읽어 저장소에 · 못 읽으면 곧바로 bad(같은 줄을 다시 보내도 같다 — 파일 기록기엔 있다)
            MatchRecord rec;
            std::string field;
            const MatchJsonParse jp = ParseMatchRecordJson(l.text, rec, &field);
            if (jp != MatchJsonParse::Ok) {
                ++stats_.unreadable[static_cast<size_t>(jp)];
                log_.push_back("[게임 서버] " + Name(conn, s) + " 판 결과를 못 읽음(" + MatchJsonParseName(jp) + (field.empty() ? "" : " · 칸 " + field) +
                               ") — " + (rec.match.empty() ? "판 번호도 못 읽어 답 없음" : "SAVED bad"));
                if (rec.match.empty()) {
                    ++stats_.unreadableNoMatch;
                } else {
                    ++stats_.savedBad;
                    out.push_back({conn, LinkSavedLine(rec.match, SaveOutcome::Bad)});
                }
                return;
            }
            if (!store_) {
                ++stats_.noStore;
                return;
            }
            const uint32_t req = nextSaveReq_++;
            saves_[req] = PendingSave{conn, rec.match};
            ++stats_.saveRequests;
            store_->SaveMatch(req, MatchSource{s.ip, s.port}, rec, l.text);
            return;
        }
        case LinkKind::Ping:   // ★ 7단계 — 받은 시각은 위에서 적었다
            ++stats_.pingsIn;
            return;
        case LinkKind::Hello:
        case LinkKind::Auth:
        case LinkKind::Register:
        case LinkKind::Create:
        case LinkKind::Bye:
        case LinkKind::Saved:
        case LinkKind::kCount:
            break;
    }
    ++stats_.badLines;
    log_.push_back("[게임 서버] " + Name(conn, s) + " 등록 뒤 맞지 않는 줄 — 버림: " + std::string(line.substr(0, 80)));
}

void RoomMirror::OnStoreAnswer(const StoreAnswer& a, std::vector<Out>& out) {
    const auto it = saves_.find(a.req);
    if (a.kind != StoreAnswerKind::SaveMatch || it == saves_.end()) {
        ++stats_.saveAnswerUnknown;
        return;
    }
    const PendingSave ps = std::move(it->second);
    saves_.erase(it);
    const auto sv = servers_.find(ps.conn);
    if (sv == servers_.end() || sv->second.stage != Stage::Registered) {   // 끊김 · 닫는 중 — 다시 붙으면 게임 서버가 다시 보낸다(dup)
        ++stats_.saveConnGone;
        log_.push_back("[게임 서버] c" + std::to_string(ps.conn) + " 판 " + ps.match + " 저장 답이 끊긴 뒤 옴 — 버림");
        return;
    }
    switch (a.save) {
        case SaveResult::Saved:
            ++stats_.savedOk;
            out.push_back({ps.conn, LinkSavedLine(ps.match, SaveOutcome::Ok)});
            return;
        case SaveResult::Duplicate:
            ++stats_.savedDup;
            out.push_back({ps.conn, LinkSavedLine(ps.match, SaveOutcome::Dup)});
            return;
        case SaveResult::Invalid:   // ★ 7단계 조각 7 — 저장소 칸에 안 맞음(다시 보내도 같다 — 파일 기록기엔 있다)
            ++stats_.saveInvalid;
            ++stats_.savedBad;
            log_.push_back("[게임 서버] " + Name(ps.conn, sv->second) + " 판 " + ps.match + " 저장소가 못 받음 — " + a.why + "(SAVED bad)");
            out.push_back({ps.conn, LinkSavedLine(ps.match, SaveOutcome::Bad)});
            return;
        case SaveResult::Failed:
            ++stats_.saveFailed;
            log_.push_back("[게임 서버] " + Name(ps.conn, sv->second) + " 판 " + ps.match + " 저장 실패 — " + a.why +
                           "(SAVED 안 보냄 — 게임 서버가 다시)");
            return;
        case SaveResult::kCount:
            break;
    }
    ++stats_.saveAnswerUnknown;   // 갈래 없는 답 — 저장소 결함
}

void RoomMirror::Tick(std::vector<Out>& out) {
    const uint32_t now = nowMs_();
    for (auto& kv : servers_) {
        Server& s = kv.second;
        if (s.stage == Stage::Closing) continue;   // BYE 를 보냈다 — 끊김 사건을 기다린다
        if (now - s.lastRecvMs >= kLinkDeadMs) {   // 32비트 시계가 돌아도 맞게(뺄셈)
            ++stats_.deadLinks;
            log_.push_back("[게임 서버] " + Name(kv.first, s) + " " + std::to_string(kLinkDeadMs / 1000) +
                           "초 동안 받은 줄 없음 — 끊음(방 " + std::to_string(s.rooms.size()) + "개를 끊김에 내림)");
            Bye(kv.first, "no line for " + std::to_string(kLinkDeadMs / 1000) + "s", out);
            continue;
        }
        if (s.stage == Stage::Registered && static_cast<int32_t>(now - s.nextPingMs) >= 0) {
            ++stats_.pingsOut;
            out.push_back({kv.first, LinkPing()});
            s.nextPingMs = now + kLinkPingEveryMs;
        }
    }
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
