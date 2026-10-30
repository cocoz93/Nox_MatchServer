// Lobby.cpp — 명령 → 답. 답의 꼴은 모두 1단계 녹음(XWIS 원래 줄)을 따른다.
//   표 = Docs/LOBBY_DESIGN.md 「명령 → 답」 · 녹음 = _evidence/_wol_record_20261006/relay_log_20261006_233255.txt
#include "nox/Lobby.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // ★ 10-14 작업 10a — 방 이름 UTF-8 → 클라 코드페이지(MultiByteToWideChar · WideCharToMultiByte)

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace nox::wol {

namespace {

// XWIS 의 채팅 로비 셋 — 화면 이름 Brin · Ix · Dun Mir (opennox xwis.go:22-26). 녹음 LIST 답 그대로.
// ★ 10-20 MO 6단계 v2 작업 3b — 대화방 다섯(배틀넷 꼴 채팅 채널 · 10-20 결정: 스타처럼 채팅 채널 → 로비에서 방 고르기).
//   정품 대화방 목록은 이름이 `Lob_37_` 로 시작하는 것만 그리고 화면 이름은 정품 고정 표(csf `Nox_Lobby_<n+1>` 53개)에서 온다
//   (Game.exe 0x41E8F0~0x41E917 · v2 시범 정적 분석) → 방마다 이름을 지을 수 없어 넷째 · 다섯째(정품 화면 갈라바 · 그록 토르)를 더함.
//   v2 시범(사람 확인 43)에서 정품 목록에 다섯이 뜨고 들어가 채팅이 됨
const char* const kChatLobbies[] = {"#Lob_37_0", "#Lob_37_1", "#Lob_37_2", "#Lob_37_3", "#Lob_37_4"};

// ★ 10-11 토큰 작업 5 — DLL 없는 클라의 인사말에 덧붙이는 줄(영문 — WOL 글자는 라틴 1)
const char* const kNoClientMotd[] = {
    "*** Nox Online client patch (wsock32.dll) is missing ***",
    "You can chat, but joining a game needs the patch. Ask the server host.",
};

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

// ★ 10-14 작업 10a — UTF-8 → 코드페이지 바이트. `maxBytes` 를 넘으면 글자 단위로 뒤에서 자른다(두 바이트 글자를 반으로 안 자른다).
//   영문만이면 바꾸지 않는다(옛 동작 그대로). 그 코드페이지에 없는 글자는 '?'(윈도 기본)
std::string ToCodepage(const std::string& utf8, int codepage, size_t maxBytes) {
    bool ascii = true;
    for (char ch : utf8)
        if (static_cast<uint8_t>(ch) >= 0x80) ascii = false;
    if (ascii) return utf8.substr(0, maxBytes);
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) return utf8.substr(0, maxBytes);
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], wn);
    const UINT cp = static_cast<UINT>(codepage > 0 ? codepage : kDefaultCodepage);
    for (size_t n = w.size(); n > 0; --n) {
        const int bn = WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (bn <= 0) break;
        if (static_cast<size_t>(bn) > maxBytes) continue;
        std::string out(static_cast<size_t>(bn), '\0');
        WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(n), &out[0], bn, nullptr, nullptr);
        return out;
    }
    return {};
}

constexpr size_t kNameBytes = 15;   // 방 정보 이름 칸(`WolGameInfo.h` — 앞 15바이트만 실린다)

}  // namespace

std::string IpDotted(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xff) + "." +
           std::to_string((ip >> 8) & 0xff) + "." + std::to_string(ip & 0xff);
}

Lobby::Lobby(LobbyConfig cfg, ILobbyRooms& rooms) : cfg_(std::move(cfg)), rooms_(rooms) {}

const LobbyRoom* Lobby::FindRoom(const std::vector<LobbyRoom>& rooms, const std::string& channel) {
    for (const LobbyRoom& r : rooms)
        if (Channel(r) == channel) return &r;
    return nullptr;
}

bool Lobby::IsRoomHost(const std::string& name) const {
    for (const LobbyRoom& r : rooms_.LobbyRooms())
        if (r.host == name) return true;
    return false;
}

// ★ 10-14 작업 8d — 방 정보 = 틀 + 그 방의 이름 · 맵 · 모드 · 인원(옛 「방마다 이름만 방장 이름으로」 — 10-10)
std::string Lobby::RoomInfo(const LobbyRoom& r, int codepage) const {
    GameInfo g = cfg_.room;
    g.name = ToCodepage(r.name.empty() ? r.host : r.name, codepage, kNameBytes);   // ★ 10-14 작업 10a
    if (!r.map.empty()) g.map = r.map;
    uint16_t bits = 0;
    if (ModeFlagsOf(r.mode, bits)) g.flags = static_cast<uint16_t>((g.flags & ~kMapTypeMask) | bits);
    g.players = r.players;
    if (r.maxPlayers != 0) g.maxPlayers = r.maxPlayers;   // ★ 10-16 작업 12c
    if (r.playing) g.access = 1;                          // ★ 10-16 작업 12c — 닫힘(정품 목록이 어떻게 그리는지는 정품 확인)
    return Pack(g);
}

void Lobby::OnConnect(int conn, uint32_t peerIp, uint32_t localIp) {
    Conn c;
    c.peerIp = peerIp;
    c.localIp = localIp;
    c.loginDueMs = cfg_.nowMs() + kLoginDeadlineMs;   // ★ 7단계 조각 6 — 로그인 마감
    conns_[conn] = c;
}

void Lobby::OnClose(int conn, std::vector<Out>& out) {
    RankRemove(conn, RankLeave::Closed, out);   // ★ 랭크 매칭 조각 4 — 줄 · 요청에서 빼고(남은 사람으로 모으기를 다시 본다) 접속을 지운다
    // ★ 10-20 v2 작업 3b — 끊기면 대화방 남은 사람들에게 PART(드나듦 알림의 나머지 반 — 안 알리면 그 이름이 사용자 목록에 남는다).
    //   QUIT 줄 꼴은 정품이 받는지 미확인이라 이미 쓰는 PART 꼴로 · 게임 방 채널은 정품이 그 화면에 없어 안 알림
    const auto ci = conns_.find(conn);
    for (auto& kv : members_) {
        if (kv.second.erase(conn) == 0 || !IsChatLobby(kv.first) || ci == conns_.end()) continue;
        for (int other : kv.second) out.push_back({other, ":" + ci->second.nick + "!u@h PART " + kv.first});
    }
    // ★ 10-20 v2 작업 3a — 기다리던 새 방 답은 버린다(방은 게임 서버가 만들면 아무도 안 들어와 빈 방 30초로 닫힌다)
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.conn != conn) {
            ++it;
            continue;
        }
        ++stats_.roomRequestDropped;
        log_.push_back("[새 방 — 버림] 요청 " + std::to_string(it->first) + " — 기다리는 동안 클라가 끊음");
        rooms_.CancelRoom(it->first);   // ★ 작업 3c — 공급자(거울)의 기다림 장부에서도
        it = pending_.erase(it);
    }
    // ★ 로그인 암호 조각 4 — 계정 장부에서 이 접속을 지운다(새 접속이 이미 차지했으면 그대로 — 끊긴 쪽이 옛 접속)
    if (ci != conns_.end() && ci->second.accountId != 0) {
        const auto a = accountConn_.find(ci->second.accountId);
        if (a != accountConn_.end() && a->second == conn) accountConn_.erase(a);
    }
    // ★ 레이팅 조각 4 — 점수를 기다리던 접속 — 답은 늦은 답으로 버린다(저장소는 읽기만 해서 남는 일 없음)
    if (ci != conns_.end() && ci->second.login == LoginState::Rating) {
        ratingPending_.erase(ci->second.ratingReq);
        ++stats_.ratingClosed;
    }
    conns_.erase(conn);
}

void Lobby::OnRoomCreated(uint32_t req, const LobbyRoom& room, std::vector<Out>& out) {
    if (const auto g = rankPending_.find(req); g != rankPending_.end()) {   // ★ 랭크 매칭 조각 4 — 랭크 방
        const std::vector<int> group = std::move(g->second.conns);
        rankPending_.erase(g);
        ++stats_.rankRoomsCreated;
        RankReply(req, group, room, out);
        return;
    }
    const auto p = pending_.find(req);
    if (p == pending_.end()) {
        ++stats_.lateRoomReply;
        log_.push_back("[새 방 — 늦은 답] 요청 " + std::to_string(req) + " → " + Channel(room) + " (끝난 요청 · 버림)");
        return;
    }
    const int conn = p->second.conn;
    pending_.erase(p);
    const auto c = conns_.find(conn);
    if (c == conns_.end()) return;   // 끊긴 접속의 요청은 `OnClose` 가 이미 지웠다 — 여기 오지 않는다
    JoinNewRoom(conn, c->second, room, out);
}

void Lobby::OnRoomCreateFailed(uint32_t req, const std::string& why, std::vector<Out>& out) {
    if (const auto g = rankPending_.find(req); g != rankPending_.end()) {   // ★ 랭크 매칭 조각 4 — 줄 앞에 되돌림(정품은 아직 답을 못 받아 기다리는 중)
        const std::vector<int> group = std::move(g->second.conns);
        rankPending_.erase(g);
        ++stats_.rankRoomFailed;
        log_.push_back("[랭크 — 방 실패] 요청 " + std::to_string(req) + " — " + why + " · 줄 앞에 되돌림");
        RankRequeue(req, group);
        RankPump(out);   // 쉬는 중이라 곧바로 다시 묻지는 않는다
        return;
    }
    const auto p = pending_.find(req);
    if (p == pending_.end()) {
        ++stats_.lateRoomReply;
        log_.push_back("[새 방 — 늦은 답] 요청 " + std::to_string(req) + " 실패(" + why + ") — 끝난 요청 · 버림");
        return;
    }
    const int conn = p->second.conn;
    const auto c = conns_.find(conn);
    ++stats_.roomCreateFailed;
    log_.push_back("[새 방 — 실패] " + (c == conns_.end() ? std::string("?") : c->second.nick) + " — " + why + " (요청 " +
                   std::to_string(req) + " · 거절 471)");
    // ★ 10-20 거절 답 — 기다리던 클라에게 471(끊긴 접속의 요청은 `OnClose` 가 이미 지웠다 — 찾지 못하면 보낼 곳이 없다)
    if (c != conns_.end()) RejectJoin(conn, p->second.channel, JoinReject::CreateFailed, out);
    pending_.erase(p);
}

void Lobby::Tick(std::vector<Out>& out) {
    const uint32_t now = cfg_.nowMs();
    // ★ 7단계 조각 6 — 로그인 마감(끊기). 32비트 시계가 돌아도 맞게 · ★ 로그인 암호 조각 4 — 계정 확인 한도는 걷음
    for (auto& kv : conns_) {
        Conn& c = kv.second;
        if (c.closing || static_cast<int32_t>(now - c.loginDueMs) < 0) continue;
        if (c.login == LoginState::Rating) {   // ★ 레이팅 조각 4 — 점수 답이 한도 안에 안 옴 → 점수 줄 없이 로그인 끝
            ++stats_.ratingTimeout;
            ratingPending_.erase(c.ratingReq);
            log_.push_back("[로그인 — 점수 시간 초과] " + c.nick + " — " + std::to_string(kRatingTimeoutMs) + "ms 안에 저장소 답 없음(점수 줄 없이 376)");
            FinishLogin(kv.first, c, LoginState::Ok, std::string(), out);
        } else if (c.login == LoginState::Waiting) {
            ++stats_.loginDeadlineClosed;
            c.closing = true;
            log_.push_back("[로그인 — 마감] c" + std::to_string(kv.first) + " — " + std::to_string(kLoginDeadlineMs / 1000) + "초 안에 USER 없음 · 끊음");
            out.push_back({kv.first, "ERROR :Closing Link (login timeout)", true});
        } else if (c.login == LoginState::Rejected) {   // ★ 10-28 — 378 뒤 클라가 스스로 안 끊음
            ++stats_.loginRejectClosed;
            c.closing = true;
            log_.push_back("[로그인 — 거절 뒤 끊음] c" + std::to_string(kv.first) + " — 378 뒤 " + std::to_string(kLoginRejectCloseMs / 1000) +
                           "초 안에 클라가 안 끊음");
            out.push_back({kv.first, "ERROR :Closing Link (login failed)", true});
        }
    }
    // ★ 랭크 매칭 조각 4 — 랭크 방 요청의 시간 초과(줄 앞에 되돌림) · 모으기 시간 · 쉬는 시간
    for (auto it = rankPending_.begin(); it != rankPending_.end();) {
        if (static_cast<int32_t>(now - it->second.deadlineMs) < 0) {
            ++it;
            continue;
        }
        ++stats_.rankRoomTimeout;
        log_.push_back("[랭크 — 방 시간 초과] 요청 " + std::to_string(it->first) + " 에 " + std::to_string(kRoomRequestTimeoutMs) +
                       "ms 안에 답 없음 · 줄 앞에 되돌림");
        rooms_.CancelRoom(it->first);
        const uint32_t req = it->first;
        const std::vector<int> group = std::move(it->second.conns);
        it = rankPending_.erase(it);
        RankRequeue(req, group);
    }
    // ★ 매칭 손보기 6 — 수락 한도. 수락 안 한 사람은 줄에서 빼고 `NOXRANK EXPIRED`(DLL 이 창을 정품 취소 길로 닫음) · 수락한 사람은 줄 앞에
    for (auto it = rankFound_.begin(); it != rankFound_.end();) {
        if (static_cast<int32_t>(now - it->second.deadlineMs) < 0) {
            ++it;
            continue;
        }
        const uint32_t req = it->first;
        const std::vector<int> conns = it->second.conns;
        ++it;   // 아래 `RankFoundBreak` 가 이 칸을 지운다
        for (int id : conns) {
            const auto ci = conns_.find(id);
            if (ci == conns_.end() || ci->second.rankReq != req || ci->second.rank != Conn::Rank::Found) continue;
            ci->second.rank = Conn::Rank::None;
            ++stats_.rankExpired;
            out.push_back({id, "NOXRANK EXPIRED"});
            log_.push_back("[랭크 — 수락 한도] " + ci->second.nick + " — " + std::to_string(kRankAcceptMs / 1000) + "초 안에 수락 안 함 · 줄에서 뺌");
        }
        RankFoundBreak(req, -1, out);
    }
    RankPump(out);
    if (pending_.empty()) return;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (static_cast<int32_t>(now - it->second.deadlineMs) < 0) {   // 32비트 시계가 돌아도 맞게
            ++it;
            continue;
        }
        ++stats_.roomRequestTimeout;
        const auto c = conns_.find(it->second.conn);
        log_.push_back("[새 방 — 시간 초과] " + (c == conns_.end() ? std::string("?") : c->second.nick) + " — 요청 " +
                       std::to_string(it->first) + " 에 " + std::to_string(kRoomRequestTimeoutMs) + "ms 안에 답 없음(거절 471)");
        if (c != conns_.end()) RejectJoin(it->second.conn, it->second.channel, JoinReject::Timeout, out);   // ★ 10-20 거절 답
        rooms_.CancelRoom(it->first);   // ★ 작업 3c — 공급자(거울)의 기다림 장부에서도
        it = pending_.erase(it);
    }
}

// 새 방에 참가 — 같은 이름 채널의 옛 방(닫힘)에 들어 있던 접속은 새 방 구성원이 아니다(남겨 두면 326 의 사람 수 칸이 부푼다)
void Lobby::JoinNewRoom(int conn, const Conn& c, const LobbyRoom& created, std::vector<Out>& out) {
    ++stats_.roomsCreated;
    log_.push_back("[새 방] " + c.nick + " → " + Channel(created) + " (게임 방 " + std::to_string(created.gameRoomId) + " · 맵 " +
                   created.map + ")");
    members_.erase(Channel(created));
    ReplyJoin(conn, c, created, out);
}

// ★ 랭크 매칭 조각 4 — 줄 ───────────────────────────────────────────────────────────────

const char* RankLeaveName(RankLeave why) {
    switch (why) {
        case RankLeave::Dll:    return "창 닫힘(DLL)";
        case RankLeave::List:   return "LIST";
        case RankLeave::Closed: return "끊김";
        case RankLeave::kCount: break;
    }
    return "?";
}

std::string Lobby::RankNames(const std::vector<int>& conns) const {
    std::string r;
    for (int id : conns) {
        const auto c = conns_.find(id);
        r += (r.empty() ? "" : ",") + (c == conns_.end() ? "c" + std::to_string(id) : c->second.nick);
    }
    return r;
}

void Lobby::RankEnqueue(int conn, Conn& c, std::vector<Out>& out) {
    c.rank = Conn::Rank::Queued;
    rankQueue_.push_back(conn);
    ++stats_.rankQueued;
    log_.push_back("[랭크 — 줄 섬] " + c.nick + " (줄 " + std::to_string(rankQueue_.size()) + "명 · 최소 " + std::to_string(cfg_.rankMin) +
                   " · 최대 " + std::to_string(cfg_.rankMax) + ")");
    RankPump(out);
}

// 줄에 있으면 빼고 남은 사람으로 모으기를 다시 본다 · 요청 중이면 그 사람만 표시를 지운다(방이 서도 답을 안 보낸다) · 둘 다 아니면 DLL 알림만 센다
void Lobby::RankRemove(int conn, RankLeave why, std::vector<Out>& out) {
    const auto ci = conns_.find(conn);
    if (ci == conns_.end()) return;
    Conn& c = ci->second;
    const size_t w = static_cast<size_t>(why);
    switch (c.rank) {
        case Conn::Rank::Queued:
            rankQueue_.erase(std::find(rankQueue_.begin(), rankQueue_.end(), conn));
            c.rank = Conn::Rank::None;
            ++stats_.rankLeft[w];
            log_.push_back("[랭크 — 빠짐] " + c.nick + " — " + RankLeaveName(why) + " (줄 " + std::to_string(rankQueue_.size()) + "명)");
            RankPump(out);
            return;
        case Conn::Rank::Requested:
            c.rank = Conn::Rank::None;
            ++stats_.rankLeftRequested[w];
            log_.push_back("[랭크 — 빠짐] " + c.nick + " — " + RankLeaveName(why) + " · 방 요청 " + std::to_string(c.rankReq) + " 중(방이 서도 답 안 함)");
            return;
        case Conn::Rank::Found:      // ★ 매칭 손보기 6 — 찾음 · 수락 뒤 빠짐 = 거절(창의 「아니오」 · 취소 → LEAVE · LIST · 끊김) — 남은 사람을 줄 앞에
        case Conn::Rank::Accepted: {
            const uint32_t req = c.rankReq;
            c.rank = Conn::Rank::None;
            ++stats_.rankDeclined[w];
            log_.push_back("[랭크 — 거절] " + c.nick + " — " + RankLeaveName(why) + " · 찾음 " + std::to_string(req) + " 풀어 남은 사람 줄 앞에");
            RankFoundBreak(req, conn, out);
            RankPump(out);
            return;
        }
        case Conn::Rank::None:
            if (why == RankLeave::Dll) {
                ++stats_.rankLeaveStray;
                log_.push_back("[랭크 — 빠짐 알림 버림] " + c.nick + " — 줄에도 요청에도 없음(잡힌 뒤 · 거절 뒤 창이 닫힘)");
            }
            return;
    }
}

// 모으기 — 최대가 차면 곧바로(여러 묶음이면 여러 번) · 최소가 차면 모으기 시간을 재고 끝나면 그때 줄 전부 · 최소 밑이면 시계를 끔.
//   방 요청이 실패 · 시간 초과로 되돌린 뒤엔 쉬는 시간 동안 안 묶는다(게임 서버가 없을 때 한 바퀴 안에서 도는 것을 막음)
void Lobby::RankPump(std::vector<Out>& out) {
    const uint32_t now = cfg_.nowMs();
    if (rankResting_) {
        if (static_cast<int32_t>(now - rankRestDueMs_) < 0) return;
        rankResting_ = false;
    }
    while (!rankResting_ && rankQueue_.size() >= cfg_.rankMax) RankRequest(cfg_.rankMax, true, out);
    if (rankResting_) return;
    if (rankQueue_.size() < cfg_.rankMin) {
        if (rankGathering_) log_.push_back("[랭크 — 모으기 멈춤] 줄 " + std::to_string(rankQueue_.size()) + "명 — 최소 밑");
        rankGathering_ = false;
        return;
    }
    if (!rankGathering_) {
        rankGathering_ = true;
        rankGatherDueMs_ = now + cfg_.rankGatherMs;
        log_.push_back("[랭크 — 모으기 시작] 줄 " + std::to_string(rankQueue_.size()) + "명 · " + std::to_string(cfg_.rankGatherMs) + "ms 더 모음");
    }
    if (static_cast<int32_t>(now - rankGatherDueMs_) < 0) return;
    rankGathering_ = false;
    RankRequest(rankQueue_.size(), false, out);
}

// 줄 앞 `count` 명을 묶어 랭크 방을 요청한다
void Lobby::RankRequest(size_t count, bool full, std::vector<Out>& out) {
    const std::vector<int> group(rankQueue_.begin(), rankQueue_.begin() + static_cast<std::ptrdiff_t>(count));
    rankQueue_.erase(rankQueue_.begin(), rankQueue_.begin() + static_cast<std::ptrdiff_t>(count));
    const uint32_t req = nextReq_++;
    RoomAsk ask;
    ask.ranked = true;
    for (int id : group) {
        Conn& c = conns_[id];   // 줄엔 살아 있는 접속만 있다(끊김은 `OnClose` 가 먼저 뺀다)
        c.rank = Conn::Rank::Requested;
        c.rankReq = req;
        ask.roster.push_back(c.nick);
        ask.ratings.push_back(RatingOf(c.nick));   // ★ 매칭 손보기 5 — 게임 서버가 판 시작 전 「Ranked: … vs …」에
    }
    ask.creator = ask.roster.front();
    ++stats_.rankRequests;
    stats_.rankGrouped += group.size();
    ++(full ? stats_.rankGroupFull : stats_.rankGroupGather);
    LobbyRoom created;
    std::string why;
    switch (rooms_.RequestRoom(req, ask, created, why)) {
        case RoomRequest::Created:
            log_.push_back("[랭크 — 묶음] 요청 " + std::to_string(req) + " " + (full ? "최대" : "모으기 끝") + " — " + RankNames(group) + " · 곧바로 섬");
            ++stats_.rankRoomsCreated;
            RankReply(req, group, created, out);
            return;
        case RoomRequest::Failed:
            ++stats_.rankRoomFailed;
            log_.push_back("[랭크 — 묶음] 요청 " + std::to_string(req) + " — " + RankNames(group) + " · 실패(" + why + ") · 줄 앞에 되돌림");
            RankRequeue(req, group);
            return;
        case RoomRequest::Pending:
            rankPending_[req] = RankGroup{group, cfg_.nowMs() + kRoomRequestTimeoutMs};
            log_.push_back("[랭크 — 묶음] 요청 " + std::to_string(req) + " " + (full ? "최대" : "모으기 끝") + " — " + RankNames(group) +
                           " · 게임 서버에 물음");
            return;
    }
}

// 방이 섰다 — 묶인 사람 가운데 아직 요청 중인 사람마다 제 채널로 참가 답(토큰은 그 방). 채널 = 그 사람이 보낸 `#<닉>'s_game` ·
//   353 방장 = 그 사람 — 사람 확인을 거친 Host Game 답 꼴 그대로(정품이 다른 채널 이름의 답을 받는지는 미확인이라 안 씀)
void Lobby::RankReply(uint32_t req, const std::vector<int>& group, const LobbyRoom& room, std::vector<Out>& out) {
    // ★ 매칭 손보기 6 — 아직 요청 중인 사람이 둘 이상이고 다 판 5 DLL 이면 참가 답 대신 수락 단계(`NOXRANK FOUND <초>`)
    {
        std::vector<int> live;
        bool allNew = true;
        for (int id : group) {
            const auto ci = conns_.find(id);
            if (ci == conns_.end() || ci->second.rank != Conn::Rank::Requested || ci->second.rankReq != req) continue;
            live.push_back(id);
            allNew = allNew && ci->second.clientVer >= kNoxClientAcceptVersion;
        }
        if (allNew && live.size() >= 2) {
            const std::string line = "NOXRANK FOUND " + std::to_string(kRankAcceptMs / 1000);
            for (int id : live) {
                conns_[id].rank = Conn::Rank::Found;
                out.push_back({id, line});
            }
            rankFound_[req] = RankFoundGroup{live, room, cfg_.nowMs() + kRankAcceptMs};
            ++stats_.rankFoundGroups;
            stats_.rankFound += live.size();
            log_.push_back("[랭크 — 찾음] 요청 " + std::to_string(req) + " → 게임 방 " + std::to_string(room.gameRoomId) + " · " + RankNames(live) +
                           " · 수락 기다림 " + std::to_string(kRankAcceptMs / 1000) + "초");
            return;
        }
    }
    size_t sent = 0;
    for (int id : group) {
        const auto ci = conns_.find(id);
        if (ci == conns_.end() || ci->second.rank != Conn::Rank::Requested || ci->second.rankReq != req) continue;   // 요청 중 빠짐
        RankJoinReply(id, ci->second, room, out);
        ++sent;
    }
    log_.push_back("[랭크 — 방] 요청 " + std::to_string(req) + " → 게임 방 " + std::to_string(room.gameRoomId) + " · 명단 " + RankNames(group) +
                   " · 참가 답 " + std::to_string(sent) + "명");
}

// 실패 · 시간 초과 — 아직 요청 중인 사람을 줄 맨 앞에(원래 차례 그대로) · 쉬는 시간 시작. 이미 모은 사람들이라 모으기 시계는
//   쉬는 시간 끝에 맞춰 둔다(쉬고 나면 최소 이상이면 곧바로 다시 묶음 — 모으기 시간을 또 기다리지 않음)
void Lobby::RankRequeue(uint32_t req, const std::vector<int>& group) {
    std::vector<int> back;
    for (int id : group) {
        const auto ci = conns_.find(id);
        if (ci == conns_.end() || ci->second.rank != Conn::Rank::Requested || ci->second.rankReq != req) continue;
        ci->second.rank = Conn::Rank::Queued;
        back.push_back(id);
    }
    rankQueue_.insert(rankQueue_.begin(), back.begin(), back.end());
    stats_.rankRequeued += back.size();
    rankResting_ = true;
    rankRestDueMs_ = cfg_.nowMs() + kRankRetryMs;
    rankGathering_ = true;
    rankGatherDueMs_ = rankRestDueMs_;
}

// ★ 매칭 손보기 6 — 참가 답 하나(제 채널 · 353 방장 = 그 사람 · 토큰은 그 방 — 옛 `RankReply` 안 몫 그대로)
void Lobby::RankJoinReply(int id, Conn& c, const LobbyRoom& room, std::vector<Out>& out) {
    c.rank = Conn::Rank::None;
    LobbyRoom mine = room;
    mine.host = c.nick;
    members_.erase(Channel(mine));   // 같은 이름 채널의 옛 방 구성원(`JoinNewRoom` 과 같은 까닭)
    ReplyJoin(id, c, mine, out);
    ++stats_.rankMatched;
}

// ★ 매칭 손보기 6 — 찾음 창의 「예」. 묶음이 다 수락이면 각자 참가 답
void Lobby::RankAccept(int /*conn*/, Conn& c, std::vector<Out>& out) {
    const auto g = c.rank == Conn::Rank::Found ? rankFound_.find(c.rankReq) : rankFound_.end();
    if (g == rankFound_.end()) {
        ++stats_.rankAcceptStray;
        log_.push_back("[랭크 — 수락 버림] " + c.nick + " — 찾음 중이 아님");
        return;
    }
    c.rank = Conn::Rank::Accepted;
    ++stats_.rankAccepted;
    for (int id : g->second.conns) {
        const auto ci = conns_.find(id);
        if (ci == conns_.end() || ci->second.rank != Conn::Rank::Accepted) {
            log_.push_back("[랭크 — 수락] " + c.nick + " — 찾음 " + std::to_string(g->first) + " 남 기다림");
            return;
        }
    }
    const RankFoundGroup fg = std::move(g->second);
    const uint32_t req = g->first;
    rankFound_.erase(g);
    for (int id : fg.conns) {
        RankJoinReply(id, conns_[id], fg.room, out);
        ++stats_.rankAcceptMatched;
    }
    log_.push_back("[랭크 — 다 수락] 찾음 " + std::to_string(req) + " → 게임 방 " + std::to_string(fg.room.gameRoomId) + " · 참가 답 " + RankNames(fg.conns));
}

// ★ 매칭 손보기 6 — 묶음 풀기(누가 거절 · 한도). `except` 말고 찾음 · 수락 중인 사람을 줄 맨 앞에(원래 차례 · 쉬지 않음) + `NOXRANK WAIT`.
//   한도면 부르는 쪽이 수락 안 한 사람을 먼저 빼고 부른다. 선 게임 서버 방은 그대로 둔다 — 아무도 안 들어오면 도착 한도 뒤 랭크를 풀고 빈 방 닫기가 닫는다
void Lobby::RankFoundBreak(uint32_t req, int except, std::vector<Out>& out) {
    const auto g = rankFound_.find(req);
    if (g == rankFound_.end()) return;
    const std::vector<int> conns = std::move(g->second.conns);
    rankFound_.erase(g);
    std::vector<int> back;
    for (int id : conns) {
        if (id == except) continue;
        const auto ci = conns_.find(id);
        if (ci == conns_.end() || ci->second.rankReq != req ||
            (ci->second.rank != Conn::Rank::Found && ci->second.rank != Conn::Rank::Accepted)) continue;
        ci->second.rank = Conn::Rank::Queued;
        back.push_back(id);
        out.push_back({id, "NOXRANK WAIT"});
    }
    rankQueue_.insert(rankQueue_.begin(), back.begin(), back.end());
    stats_.rankFoundRequeued += back.size();
    if (!back.empty()) log_.push_back("[랭크 — 찾음 풂] 요청 " + std::to_string(req) + " — " + RankNames(back) + " 줄 앞에(WAIT)");
}

// ★ 10-20 거절 답 — 까닭이 무엇이든 숫자는 471(정품이 받아 주는 유일한 것 — `Lobby.h` `JoinReject` 주석). 꼴은 다른 숫자 답과 같은 `: <숫자> u <채널> :<글>`
//   (정품 로비 모듈은 명령 칸 숫자와 제 기다림 상태로만 가른다 — 나머지 칸은 일반 오류 글로 엮기만 하고 471 갈래에선 안 쓴다 · `WOLAPI.dll` `0x210096d3`)
void Lobby::RejectJoin(int conn, const std::string& channel, JoinReject why, std::vector<Out>& out) {
    ++stats_.joinRejected[static_cast<size_t>(why)];
    out.push_back({conn, ": " + std::to_string(kJoinRejectNumeric) + " u " + channel + " :" + JoinRejectText(why)});
}

const char* JoinRejectText(JoinReject why) {
    switch (why) {
        case JoinReject::RequestFailed: return "Cannot create game (request failed)";
        case JoinReject::CreateFailed:  return "Cannot create game (game server refused)";
        case JoinReject::Timeout:       return "Cannot create game (game server timed out)";
        case JoinReject::NoSuchRoom:    return "No such game";
        case JoinReject::NotLoggedIn:   return "Not logged in";
        case JoinReject::kCount:        break;
    }
    return "?";
}

// ★ 7단계 조각 6 — 인사. 실패도 XWIS 꼴로 로그인은 끝낸다(376) — 답을 안 하면 정품이 「Connecting…」에서 멈춘다(10-10)
//   ★ 로그인 암호 조각 4 — 뒷줄 쌓기 걷음(곧바로 답하므로 정품 묶음의 뒷줄은 받은 차례대로 376 뒤에 온다)
//   ★ 레이팅 조각 4 — 점수를 기다리는 토큰 로그인은 다시 쌓는다(`OnLine` · `BeginOkLogin`)
void Lobby::FinishLogin(int conn, Conn& c, LoginState result, const std::string& failNotice, std::vector<Out>& out,
                        const std::string& ratingLine) {
    c.ratingReq = 0;
    // ★ 10-28 사람 확인 54 — 암호 판의 실패는 로비에 들이지 않는다. 인사(375 · 376) 대신 `378` 한 줄 — 정품 로비 모듈이 378 을
    //   「암호 틀림」(`0x80040065` — `WOLAPI.dll` `0x210096d3` → `0x21009784`)으로 게임에 넘기고, 게임이 「틀린 아이디 또는 패스워드.」
    //   (`nox.csf` `Wol.c:InvalidPass` · `Game.exe` `0x41cf50` 분기표 `0x41d1a4`) 창을 띄운다. 433(이름 사용 중 — 같은 함수 표 자리 +0x38)이
    //   창을 띄우고 클라가 스스로 끊은 것(10-11 사람 확인)과 같은 길 — 정적 분석. 376 뒤에 끊으면 클라가 로비에 들어가 채널 목록을 기다리며 멈췄다(10-28).
    //   글 칸은 정품이 안 쓴다(창 글은 클라 것) — 까닭은 기록 · 셈에. 클라가 안 끊으면 `kLoginRejectCloseMs` 뒤 `Tick` 이 끊는다
    if (result == LoginState::Failed && cfg_.authOn) {
        c.login = LoginState::Rejected;
        c.loginDueMs = cfg_.nowMs() + kLoginRejectCloseMs;
        ++stats_.loginRejected;   // 기록 줄은 부른 쪽(까닭과 함께)
        out.push_back({conn, ": 378 u :" + failNotice});
        return;
    }
    c.login = result;
    out.push_back({conn, ": 375 u :- " + (cfg_.motd.empty() ? std::string() : cfg_.motd[0])});
    for (size_t i = 1; i < cfg_.motd.size(); ++i) out.push_back({conn, ": 372 u :- " + cfg_.motd[i]});
    if (c.clientVer < kNoxClientMinVersion)
        for (const char* l : kNoClientMotd) out.push_back({conn, std::string(": 372 u :- ") + l});
    if (!failNotice.empty()) {
        out.push_back({conn, ": 372 u :- *** Login failed: " + failNotice + " ***"});
        if (!cfg_.authOn) out.push_back({conn, ": 372 u :- You can chat, but joining a game needs a successful login."});
    }
    if (!ratingLine.empty()) out.push_back({conn, ": 372 u :- " + ratingLine});   // ★ 레이팅 조각 4 — 정품은 공지를 로비 채팅창에 줄로(`Game.exe` 0x40c180 → 0x446e90)
    out.push_back({conn, ": 376 u"});
    // ★ 레이팅 조각 4 — 쌓아 둔 뒷줄을 받은 차례대로(이 함수는 한 바퀴 안 — 그 사이 끊김 없음). 넘기는 동안 다시 쌓이지 않는다(로그인이 끝났다)
    std::vector<std::string> held;
    held.swap(c.held);
    stats_.heldLines += held.size();
    for (const std::string& l : held) OnLine(conn, l, out);
}

// ★ 레이팅 조각 4 — 토큰 로그인 통과 뒤. 저장소가 있으면 점수를 묻고 인사를 미룬다(`kRatingTimeoutMs`) · 없으면 곧바로
void Lobby::BeginOkLogin(int conn, Conn& c, std::vector<Out>& out) {
    if (!store_) {
        FinishLogin(conn, c, LoginState::Ok, std::string(), out);
        return;
    }
    c.login = LoginState::Rating;
    c.ratingReq = nextRatingReq_++;
    c.loginDueMs = cfg_.nowMs() + kRatingTimeoutMs;
    ratingPending_[c.ratingReq] = conn;
    ++stats_.ratingAsked;
    store_->GetRating(c.ratingReq, c.nick);
}

void Lobby::OnRatingAnswer(const StoreAnswer& a, std::vector<Out>& out) {
    const auto p = ratingPending_.find(a.req);
    if (a.kind != StoreAnswerKind::Rating || p == ratingPending_.end()) {
        ++stats_.ratingLate;   // 끝난 물음(한도 · 끊김) · 안 물은 답
        return;
    }
    const int conn = p->second;
    ratingPending_.erase(p);
    const auto ci = conns_.find(conn);
    if (ci == conns_.end() || ci->second.login != LoginState::Rating || ci->second.ratingReq != a.req) {
        ++stats_.ratingLate;
        return;
    }
    Conn& c = ci->second;
    // 영문(ASCII)만 — ★ 10-30 사람 확인 59 — 한글(949 바이트) 공지 줄이 정품 채팅창에서 깨짐. 정품은 공지 줄을 `%S` 로 넓은 글자로 바꿔 그리는데
    //   (`Game.exe` 0x446950 · 형식 0x59fb04) 이 변환이 949 를 따르지 않는 것으로 봄(채팅 줄은 한글이 나옴 — 다른 길). 가운뎃점도 ASCII 아님 → `-`
    std::string line;
    if (a.ratingOk) ratingOf_[c.nick] = a.rating;   // ★ 매칭 손보기 5 — 랭크 방 요청의 명단 점수(판 없음이면 시작 점수 그대로)
    if (!a.ratingOk) {
        ++stats_.ratingFailed;
        log_.push_back("[로그인 — 점수 못 읽음] " + c.nick + " — " + (a.why.empty() ? std::string("저장소 실패") : a.why) + "(점수 줄 없이 376)");
    } else if (a.ratingFound) {
        ++stats_.ratingShown;
        line = "Rating " + std::to_string(a.rating) + " - Rank #" + std::to_string(a.rank);
        log_.push_back("[로그인 — 점수] " + c.nick + " — " + std::to_string(a.rating) + " · " + std::to_string(a.rank) + "위");
    } else {
        ++stats_.ratingNoGames;
        line = "Rating " + std::to_string(a.rating) + " - No ranked games yet";
        log_.push_back("[로그인 — 점수] " + c.nick + " — 랭크 판 없음");
    }
    FinishLogin(conn, c, LoginState::Ok, std::string(), out, line);
}

// ★ 매칭 손보기 5 — 머리 주석은 `Lobby.h`
void Lobby::OnRatingsChanged(const std::vector<LinkRatingChange>& changes) {
    for (const LinkRatingChange& c : changes) ratingOf_[c.account] = c.after;
}

int32_t Lobby::RatingOf(const std::string& account) const {
    const auto it = ratingOf_.find(account);
    return it == ratingOf_.end() ? kRatingStart : it->second;
}

std::string Lobby::SignupUrl(const Conn& c) const {
    return "https://" + IpDotted(c.localIp) + ":" + std::to_string(cfg_.authPort) + "/signup";
}

// ★ 로그인 암호 조각 4 — 같은 계정의 옛 접속을 끊는다(정할 것 b). 끊는 줄은 마감 끊기와 같은 꼴(`Tick`) · 대화방 PART 는 끊김 사건의 `OnClose` 가
void Lobby::TakeAccount(int conn, Conn& c, uint64_t accountId, std::vector<Out>& out) {
    c.accountId = accountId;
    int& owner = accountConn_[accountId];
    if (owner != 0 && owner != conn) {
        const auto old = conns_.find(owner);
        if (old != conns_.end() && !old->second.closing) {
            ++stats_.loginKickedOld;
            old->second.closing = true;
            log_.push_back("[로그인 — 옛 접속 끊음] " + c.nick + " — 계정 " + std::to_string(accountId) + " · c" + std::to_string(owner) + " → c" +
                           std::to_string(conn));
            out.push_back({owner, "ERROR :Closing Link (logged in elsewhere)", true});
        }
    }
    owner = conn;
}

// ★ 로그인 암호 조각 4 — `apgar` 자리를 가른다(`LoginLine.h`) → 갈래 하나를 세고 인사. 세는 자리는 이 함수와 USER 갈래(이름 · 키 없음)뿐
void Lobby::CheckLoginToken(int conn, Conn& c, std::vector<Out>& out) {
    std::string payload;
    const ApgarKind kind = ClassifyApgar(c.apgar, payload);
    c.apgar.clear();   // 다 봤으면 쥐지 않는다
    const char* tooOld = "Client patch is too old for password login - get the new wsock32.dll.";
    switch (kind) {
        case ApgarKind::Token: {
            uint8_t raw[kLoginTokenSize];
            LoginToken t;
            const uint32_t now = cfg_.unixNow();
            const LoginTokenCheck r = ParseHex(payload, raw, sizeof(raw))
                                          ? VerifyLoginToken(cfg_.loginKey, raw, sizeof(raw), now, c.peerIp, c.nick, t)
                                          : LoginTokenCheck::BadSize;
            if (r == LoginTokenCheck::Ok && !loginNonces_.Consume(t.nonce, t.expiresAt, now)) {
                ++stats_.loginReplay;
                log_.push_back("[로그인 — 다시 쓴 토큰] " + c.nick + " — 일회용 번호를 이미 씀(거절 378)");
                FinishLogin(conn, c, LoginState::Failed, "Login token was already used - log in again.", out);
                return;
            }
            ++stats_.loginToken[static_cast<size_t>(r)];
            if (r != LoginTokenCheck::Ok) {
                log_.push_back("[로그인 — 토큰 거절] " + c.nick + " — " + LoginTokenCheckName(r) + "(거절 378)");
                FinishLogin(conn, c, LoginState::Failed, "Login token rejected - log in again.", out);
                return;
            }
            TakeAccount(conn, c, t.accountId, out);
            log_.push_back("[로그인] " + c.nick + " — 계정 " + std::to_string(t.accountId) + "(토큰)");
            BeginOkLogin(conn, c, out);   // ★ 레이팅 조각 4 — 점수를 물은 뒤 인사(저장소 없으면 곧바로)
            return;
        }
        case ApgarKind::Fail: {
            LoginFail f = LoginFail::kCount;
            if (!ParseLoginFail(payload, f)) {
                ++stats_.loginFailUnknown;
                log_.push_back("[로그인 — 실패 표시] " + c.nick + " — 모르는 까닭(거절 378)");
                FinishLogin(conn, c, LoginState::Failed, "Login failed - get the new client patch.", out);
                return;
            }
            ++stats_.loginFail[static_cast<size_t>(f)];
            std::string notice;
            switch (f) {
                case LoginFail::WrongPassword: notice = "Wrong password."; break;
                case LoginFail::NoAccount: notice = "No such account - sign up at " + SignupUrl(c); break;
                case LoginFail::ServerError: notice = "Login server error - try again later."; break;
                case LoginFail::Unreachable: notice = "Cannot reach the login server - try again later."; break;
                case LoginFail::BadInput: notice = "Name: letters/digits 1-9. Password: 4-8, a letter and a digit, no spaces."; break;
                case LoginFail::kCount: notice = "Login failed."; break;
            }
            log_.push_back("[로그인 — 실패 표시] " + c.nick + " — " + LoginFailName(f) + "(거절 378)");
            FinishLogin(conn, c, LoginState::Failed, notice, out);
            return;
        }
        case ApgarKind::Guest:
            ++stats_.loginGuest;
            log_.push_back("[로그인 — 손님] c" + std::to_string(conn) + " — 암호 판엔 손님 없음(거절 378)");
            FinishLogin(conn, c, LoginState::Failed, "Guest login is off - sign up at " + SignupUrl(c), out);
            return;
        case ApgarKind::Plain:
            ++stats_.loginPlain;
            log_.push_back("[로그인 — 평문] " + c.nick + " — 클라 DLL 판 " + std::to_string(c.clientVer) + "(토큰 없음 · 거절 378)");
            FinishLogin(conn, c, LoginState::Failed, c.clientVer < kNoxClientAuthVersion ? tooOld : "Login token missing - log in again.", out);
            return;
        case ApgarKind::Missing:
        case ApgarKind::kCount:
            break;
    }
    ++stats_.loginMissing;
    log_.push_back("[로그인 — apgar 없음] " + c.nick + "(거절 378)");
    FinishLogin(conn, c, LoginState::Failed, c.clientVer < kNoxClientAuthVersion ? tooOld : "Login token missing - log in again.", out);
}

// 방 참가 답 — 방장 주소가 353 에 실린다(326 과 같은 값). ★ 10-20 v2 작업 3a — 옛 JOINGAME 갈래의 몸통(미룬 새 방 답도 이것)
void Lobby::ReplyJoin(int conn, const Conn& c, const LobbyRoom& lr, std::vector<Out>& out) {
    const std::string room = Channel(lr);
    const uint32_t gameIp = GameServerIp(c, lr);
    // ★ 10-11 토큰 작업 5 — 토큰은 DLL 이 있는 클라에게만. 없으면(로그인 전 참가 포함) 또는 서명이 실패하면 토큰 없이 원래 답만 —
    //   클라는 게임 서버에서 `0x13` 사유 4 로 거절된다. 답을 안 하면 클라가 참가 화면에서 멈춘다(10-10 로그인 거절 때 멈춘 것과 같은 꼴)
    if (c.clientVer < kNoxClientMinVersion) {
        ++stats_.joinNoClient;
        log_.push_back("[참가 — 토큰 없음] " + c.nick + " → " + room + " (클라 DLL 없음 — 게임 서버가 거절할 것)");
    } else {
        // ★ 10-10 토큰 작업 3 — 입장 토큰. 답 바로 앞 줄이어야 한다 — 클라는 답을 받고 1ms 뒤 게임 서버에 접속 시도를 보낸다
        //   (10-10 로그). DLL 이 이 줄을 떼어 쥐고 접속 시도 · CONNECT 꼬리에 붙인다.
        JoinToken tok;
        tok.roomId = lr.gameRoomId;
        tok.clientIp = c.peerIp;   // 게임 서버가 UDP 를 보낸 주소와 견준다 — 같은 공유기 뒤면 같은 바깥 주소
        tok.expiresAt = cfg_.unixNow() + kJoinTokenLifetimeSec;
        if (cfg_.nonce) tok.nonce = cfg_.nonce();
        else RandomBytes(reinterpret_cast<uint8_t*>(&tok.nonce), sizeof(tok.nonce));
        tok.account = c.nick;
        uint8_t raw[kJoinTokenSize];
        // ★ 10-20 v2 작업 3a — 그 방이 사는 게임 서버의 키(그 게임 서버의 그 실행에만 맞는다 — `LobbyLink.h` 「키」)
        if (!SignJoinToken(lr.ownTokenKey ? lr.tokenKey : cfg_.joinKey, tok, raw)) {
            ++stats_.tokenSignFailed;
            log_.push_back("[토큰 실패] 서명 — " + c.nick + " → " + room + " (토큰 없이 답함)");
        } else {
            ++stats_.tokensIssued;
            const uint16_t port = lr.gamePort ? lr.gamePort : cfg_.gameServerPort;
            out.push_back({conn, "NOXTOKEN " + IpDotted(gameIp) + ":" + std::to_string(port) + " " + ToHex(raw, sizeof(raw))});
            log_.push_back("[토큰] " + c.nick + " → " + room + " (게임 방 " + std::to_string(lr.gameRoomId) + " · " +
                           IpDotted(c.peerIp) + ")");
        }
    }
    members_[room].insert(conn);
    // `1 32 37 1 0 <내 IP> 1` — 녹음 그대로, 내 IP 만 바꾼다(앞 칸들 뜻 확인 안 함)
    out.push_back({conn, ":" + c.nick + "!u@h JOINGAME 1 32 37 1 0 " + std::to_string(c.peerIp) + " 1 :" + room});
    out.push_back({conn, ": 332 u " + room + " " + RoomInfo(lr, c.codepage)});   // 방 정보가 ':' 로 시작한다
    out.push_back({conn, ": 353 u = " + room + " :" + Members353(room, lr.host, gameIp)});
    out.push_back({conn, ": 366 u " + room + " :"});
}

std::vector<std::string> Lobby::TakeLog() {
    std::vector<std::string> r;
    r.swap(log_);
    return r;
}

void Lobby::OnLine(int conn, std::string_view line, std::vector<Out>& out) {
    auto it = conns_.find(conn);
    if (it == conns_.end() || line.empty()) return;
    // ★ 10-28 — 끊는 줄을 보낸 접속 · 로그인을 거절한 접속의 뒷줄(같은 묶음의 verchk · SETCODEPAGE …)엔 답하지 않는다
    if (it->second.closing || it->second.login == LoginState::Rejected) return;
    // ★ 레이팅 조각 4 — 점수를 기다리는 중이면 뒷줄은 쌓아 둔다(답 뒤 `FinishLogin` 이 차례대로 — 녹음 차례 376 → 379 → 329)
    if (it->second.login == LoginState::Rating) {
        if (it->second.held.size() >= kMaxHeldLines) {
            ++stats_.heldDropped;
            return;
        }
        it->second.held.emplace_back(line);
        return;
    }
    const auto t = Split(line);
    const std::string cmd = Upper(t[0]);
    OnChat(conn, it->second, cmd, t, line, out);   // ★ 10-14 작업 8b — 4005(로비 찾기)는 라우터(`Router.cpp`)로 옮겼다
}

// 4000 — 로그인 · 목록 · 채팅 로비 · 방 참가
void Lobby::OnChat(int conn, Conn& c, const std::string& cmd, const std::vector<std::string>& t,
                   std::string_view line, std::vector<Out>& out) {
    // ★ 랭크 매칭 조각 4 — 랭크 표시는 바로 다음 줄(내 방 참가)에만 걸린다. 다른 줄이 끼면 표시를 버린다(그 줄은 원래대로)
    if (c.rankNext && cmd != "JOINGAME" && cmd != "NOXRANK") {
        c.rankNext = false;
        ++stats_.rankMarkDropped;
        log_.push_back("[랭크 — 표시 버림] " + c.nick + " — NOXRANK JOIN 다음 줄이 " + cmd);
    }
    // 답이 없는 줄(녹음 그대로). 이름·암호는 검사하지 않는다 — 2단계는 아무 이름이나 받는다
    if (cmd == "CVERS" || cmd == "PASS" || cmd == "SERIAL" || cmd == "SETOPT") return;
    if (cmd == "NOXRANK") {   // ★ 랭크 매칭 조각 4 — 클라 DLL 판 4 의 표시(정품 클라는 모른다 — DLL 이 끼운다 · `wsock32_proxy.cpp` ⑪)
        if (c.clientVer < kNoxClientRankVersion) {
            ++stats_.rankOldClient;
            Unknown("NOXRANK(판 " + std::to_string(c.clientVer) + ")", line);
            return;
        }
        const std::string sub = t.size() > 1 ? Upper(t[1]) : std::string();
        if (sub == "JOIN") c.rankNext = true;
        else if (sub == "LEAVE") RankRemove(conn, RankLeave::Dll, out);
        else if (sub == "ACCEPT") RankAccept(conn, c, out);   // ★ 매칭 손보기 6 — 판 5 DLL · 찾음 창의 「예」
        else Unknown("NOXRANK " + sub, line);
        return;
    }
    if (cmd == "APGAR") {   // ★ 로그인 암호 조각 4 — 인자를 USER 까지 쥔다(DLL 이 로그인 토큰 · 실패 표시로 바꿔 실음 · 기록에 안 찍음)
        c.apgar = t.size() > 1 ? t[1] : std::string();
        return;
    }
    if (cmd == "NICK") {
        if (t.size() > 1) c.nick = t[1];
        return;
    }
    if (cmd == "NOXCLIENT") {   // ★ 10-10 토큰 작업 3 — 클라 DLL 의 알림(정품 클라는 이 줄을 모른다 — DLL 이 로그인 앞에 끼운다)
        c.clientVer = t.size() > 1 ? std::atoi(t[1].c_str()) : 0;
        if (c.clientVer >= kNoxClientHostGameVersion) out.push_back({conn, "NOXSERVER 1"});   // ★ 10-16 작업 11 — 우리 로비 표시
        return;
    }
    if (cmd == "USER") {   // 로그인 묶음의 끝 — 인사(375 · 372…) 뒤 376 이 「로그인 끝」
        // ★ 10-11 토큰 작업 5 — DLL 이 없거나 판이 낮아도 로그인은 받고, 인사말에 패치가 필요하다고 싣는다. 방 참가엔 토큰을 안 준다.
        //   10-10 처음엔 `ERROR` 를 보내고 끊었는데, 정품 화면은 아무 말 없이 「Connecting to Westwood Online...」에서 멈췄다
        //   (사람 확인 — 고장처럼 보인다). XWIS 도 손님 로그인을 끊지 않고 인사말에 경고를 실어 들여보낸다(10-06 녹음 c4:4000)
        if (c.login != LoginState::Waiting) {   // ★ 7단계 조각 6 — 두 번째 USER(로그인은 한 번) — 처음 본 명령처럼 한 번만 기록
            Unknown("USER(두 번째)", line);
            return;
        }
        const bool noClient = c.clientVer < kNoxClientMinVersion;
        if (noClient) {
            if (c.clientVer < 0) ++stats_.loginNoClient;
            else ++stats_.loginOldClient;
            log_.push_back("[로그인 안내] " + c.nick + " — 클라 DLL " +
                           (c.clientVer < 0 ? std::string("없음(NOXCLIENT 줄 없음)") : "판 " + std::to_string(c.clientVer) + " 낮음") +
                           " · 인사말로 알림 · 방 참가엔 토큰 없음");
        }
        if (c.nick.empty() || c.nick.size() > kMaxNickBytes) {   // 토큰 이름 칸 16바이트 — 자르면 남의 계정과 겹칠 수 있다
            ++stats_.loginBadName;
            log_.push_back("[로그인 — 이름] c" + std::to_string(conn) + " 이름 " + std::to_string(c.nick.size()) + "바이트(1~" +
                           std::to_string(kMaxNickBytes) + ") — " + (cfg_.authOn ? "거절 378" : "안내 + 376"));
            FinishLogin(conn, c, LoginState::Failed, "Name must be 1-16 bytes - log in with a shorter name.", out);
            return;
        }
        // ★ 로그인 암호 조각 4 — 키가 없으면 아이디만(조각 6 이전 꼴 · 시험용 — 정할 것 c) · 있으면 토큰(정할 것 a — DB 안 물음)
        if (!cfg_.authOn) {
            ++stats_.loginNoAuth;
            FinishLogin(conn, c, LoginState::Ok, std::string(), out);
            return;
        }
        CheckLoginToken(conn, c, out);
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
            if (IsRoomHost(t[i])) cp = 1252;   // 가짜 방장 — 녹음의 방장(tak7)이 1252
            for (const auto& kv : conns_)
                if (kv.second.nick == t[i]) cp = kv.second.codepage;
            r += " " + t[i] + "`" + std::to_string(cp);
        }
        out.push_back({conn, r});
        return;
    }
    if (cmd == "LIST") {   // `LIST -1 37` = 채팅 로비 + 게임 방, `LIST 37 37` = 게임 방만(녹음: 게임 버튼 뒤 목록엔 327 이 없다)
        // ★ 랭크 매칭 조각 4 — 줄 선 접속의 목록 다시 받기 = 기다림을 그만둠(정품 취소 뒤 목록 화면이 LIST 를 보낸다 · 사람 확인 50)
        if (c.rank != Conn::Rank::None) RankRemove(conn, RankLeave::List, out);
        SendList(conn, t.size() > 1 && t[1] == "-1", out);
        return;
    }
    if (cmd == "JOIN") {   // 채팅 로비 입장 — `JOIN #Lob_37_0 zot0blat`(뒤 칸은 채널 열쇠로 보임, 안 본다)
        if (t.size() < 2 || !IsChatLobby(t[1])) {
            log_.push_back("[모르는 채널] " + std::string(line));
            return;
        }
        // ★ 로그인 암호 조각 4 — 암호 판에서 로그인 안 된 접속은 보기만(본인에게만 들어옴 답 — 남에게 이름이 안 보이고 구성원이 아니라 글이 걸러짐).
        //   답을 안 하면 정품이 들어가기 화면에서 멈출 수 있어 거절 대신 이 꼴(정품이 대화방 거절을 어떻게 그리는지 미확인)
        //   ★ 10-28 — 실패한 로그인은 거절하고 뒷줄을 안 받으므로(`FinishLogin` · `OnLine`) 여기 오는 것은 USER 전 접속뿐(규약 밖 클라)
        const bool viewOnly = cfg_.authOn && c.login != LoginState::Ok;
        if (viewOnly) {
            ++stats_.chatViewOnly;
            log_.push_back("[대화방 — 보기만] " + (c.nick.empty() ? "c" + std::to_string(conn) : c.nick) + " → " + t[1] + " (로그인 안 됨)");
        } else {
            // ★ 10-20 v2 작업 3b — 같은 대화방 사람들에게도 들어옴을 알린다(전에는 본인에게만 — v2 시범 줄 그대로)
            for (int other : members_[t[1]])
                if (other != conn) out.push_back({other, ":" + c.nick + "!u@h JOIN :0,0 " + t[1]});
            members_[t[1]].insert(conn);
        }
        out.push_back({conn, ":" + c.nick + "!u@h JOIN :0,0 " + t[1]});
        out.push_back({conn, ": 353 u * " + t[1] + " :" + Members353(t[1], std::string(), 0)});
        out.push_back({conn, ": 366 u " + t[1] + " :"});
        return;
    }
    if (cmd == "PART") {
        if (t.size() < 2) return;
        if (auto m = members_.find(t[1]); m != members_.end()) {
            m->second.erase(conn);
            for (int other : m->second) out.push_back({other, ":" + c.nick + "!u@h PART " + t[1]});   // ★ 10-20 v2 작업 3b — 남은 사람들에게도
        }
        out.push_back({conn, ":" + c.nick + "!u@h PART " + t[1]});
        return;
    }
    if (cmd == "PRIVMSG") {   // ★ 10-20 v2 작업 3b — 채팅을 같은 채널 사람들에게(보낸 사람에겐 안 돌려줌 — 정품이 제 글을 스스로 그린다 · v2 시범)
        const size_t colon = line.find(" :");
        if (t.size() < 3 || colon == std::string_view::npos) {
            ++stats_.chatMalformed;
            Unknown(cmd, line);
            return;
        }
        const std::string& ch = t[1];
        const auto m = members_.find(ch);
        if (m == members_.end() || !m->second.count(conn)) {   // 들어가 있지 않은 채널엔 못 보낸다(남의 채널 엿보내기 막기)
            ++stats_.chatNotMember;
            log_.push_back("[채팅 — 거름] " + c.nick + " → " + ch + " (들어가 있지 않은 채널)");
            return;
        }
        const std::string text(line.substr(colon + 2));
        ++stats_.chatMessages;
        for (int other : m->second) {
            if (other == conn) continue;
            out.push_back({other, ":" + c.nick + "!u@h PRIVMSG " + ch + " :" + text});
            ++stats_.chatDelivered;
        }
        return;
    }
    if (cmd == "JOINGAME") {   // 방 참가 — 방장 주소가 353 에 실린다(326 과 같은 값)
        const bool rankMark = c.rankNext;   // ★ 랭크 매칭 조각 4 — 바로 앞 줄이 `NOXRANK JOIN`
        c.rankNext = false;
        // ★ 7단계 조각 6 — 토큰 · 새 방은 로그인 확인됨만(USER 없이 NICK + JOINGAME 만 보내도 토큰이 나가던 것 · 검사 지적 2).
        //   정품은 376 을 받은 뒤에야 참가하므로 이 갈래는 실패한 로그인 · 규약 밖 클라 몫 — 471(정품이 받아 주는 거절)
        if (c.login != LoginState::Ok) {
            log_.push_back("[참가 — 로그인 안 됨] " + (c.nick.empty() ? "c" + std::to_string(conn) : c.nick) + " → " +
                           (t.size() >= 2 ? t[1] : std::string("*")) + " (거절 471)");
            RejectJoin(conn, t.size() >= 2 ? t[1] : std::string("*"), JoinReject::NotLoggedIn, out);
            return;
        }
        // ★ 랭크 매칭 조각 4 — 줄에 있거나 랭크 방을 기다리는 접속의 참가는 무시(답이 둘이 되지 않게 — 정품은 기다리는 동안 다시 안 보냄)
        if (c.rank != Conn::Rank::None) {
            ++stats_.rankDupJoin;
            log_.push_back("[랭크 — 참가 무시] " + c.nick + " — 줄에 있음 · 랭크 방을 기다리는 중");
            return;
        }
        const bool ownChannel = t.size() >= 2 && !c.nick.empty() && t[1] == "#" + c.nick + "'s_game";
        if (rankMark) {
            if (ownChannel) {   // 줄 서기 — 답은 묶여 방이 선 뒤(한도 없음 · 빠짐은 DLL 알림 · LIST · 끊김)
                RankEnqueue(conn, c, out);
                return;
            }
            ++stats_.rankMarkDropped;
            log_.push_back("[랭크 — 표시 버림] " + c.nick + " — 내 방 참가가 아님(" + (t.size() >= 2 ? t[1] : std::string("*")) + ")");
        }
        const std::vector<LobbyRoom> rooms = rooms_.LobbyRooms();   // ★ 10-14 작업 8d — 그 순간의 돌고 있는 방
        const LobbyRoom* found = t.size() < 2 ? nullptr : FindRoom(rooms, t[1]);
        // ★ 10-14 플랜 B 작업 10a — 「새 방」: 없는 방인데 채널이 제 이름 채널(`#<내 닉>'s_game` — 원작 `Host Game` 과 같은 꼴)이면
        //   그 사람이 방장인 방을 만들고 그 방으로 참가시킨다(`Docs/ROOMS_DESIGN.md` 흐름 3 · 결정 4 — 정품 `Host Game` 을 DLL 이 이 참가로 돌린다).
        //   남의 이름 채널 · 없는 방은 ★ 10-20 거절 답 `471`(그 전엔 답을 안 해 정품이 참가 화면에서 멈췄다 — 사람 확인 50).
        //   DLL 없는 클라에겐 안 만든다(토큰이 없어 들어올 수 없는 방이 30초 남는다) — 답도 안 한다(그 클라의 원래 방 만들기엔 정품이 받아 줄 거절 답이 없다 · `Lobby.h` `JoinReject`)
        if (!found && ownChannel) {
            if (c.clientVer < kNoxClientMinVersion) {
                ++stats_.newRoomNoClient;
                log_.push_back("[새 방 — 안 만듦] " + c.nick + " — 클라 DLL 없음(토큰이 없어 들어올 수 없다)");
                return;
            }
            // ★ 10-20 v2 작업 3a — 이미 새 방을 기다리는 접속이면 무시(답이 둘이 되지 않게 — 정품은 답을 기다리며 다시 안 보낸다고 봄)
            for (const auto& kv : pending_) {
                if (kv.second.conn != conn) continue;
                ++stats_.joinWhilePending;
                log_.push_back("[새 방 — 무시] " + c.nick + " — 요청 " + std::to_string(kv.first) + " 의 답을 기다리는 중");
                return;
            }
            const uint32_t req = nextReq_++;
            LobbyRoom created;
            std::string why;
            RoomAsk ask;   // ★ 랭크 매칭 조각 4 — 일반 새 방 = 만든 사람 하나
            ask.creator = c.nick;
            switch (rooms_.RequestRoom(req, ask, created, why)) {
                case RoomRequest::Created:
                    JoinNewRoom(conn, c, created, out);
                    return;
                case RoomRequest::Failed:
                    ++stats_.roomCreateFailed;
                    log_.push_back("[새 방 — 실패] " + c.nick + " — " + why + " (거절 471)");
                    RejectJoin(conn, t[1], JoinReject::RequestFailed, out);   // ★ 10-20 거절 답(사람 확인 50 — 게임 서버 없음)
                    return;
                case RoomRequest::Pending:
                    ++stats_.roomsRequested;
                    pending_[req] = PendingRoom{conn, cfg_.nowMs() + kRoomRequestTimeoutMs, t[1]};
                    log_.push_back("[새 방 — 물음] " + c.nick + " — 요청 " + std::to_string(req) + "(답이 오면 참가 답)");
                    return;
            }
            return;
        }
        if (!found) {
            log_.push_back("[모르는 방] " + std::string(line) + " (거절 471)");
            RejectJoin(conn, t.size() >= 2 ? t[1] : std::string("*"), JoinReject::NoSuchRoom, out);   // ★ 10-20 거절 답
            return;
        }
        ReplyJoin(conn, c, *found, out);
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
    // 셋째 칸 = 채널 안 사람 수로 보인다(녹음: 전용 서버 방 Anwserver 는 덩어리 인원 0 인데 이 칸 1) → 가짜 방장 1 + 참가자
    // `41681088` 은 뜻 모름 — 녹음 다섯 방 중 셋의 값(나머지 13893824 · 13894208)
    // ★ 10-14 작업 8d — 방마다 한 줄(돌고 있는 방 · 방 번호 차례 — 옛 설정 차례). 방장 주소 칸 = 이 접속이 붙은 채널 주소
    const auto ci = conns_.find(conn);
    const int codepage = ci == conns_.end() ? 0 : ci->second.codepage;   // ★ 10-14 작업 10a — 방 이름을 그 접속의 코드페이지로
    for (const LobbyRoom& lr : rooms_.LobbyRooms()) {
        if (lr.ranked) {   // ★ 매칭 손보기 7 — 랭크 방은 명단 사람만(참가 답은 줄이 보냄) — 남의 목록엔 안 실음
            ++stats_.rankRoomsHidden;
            continue;
        }
        // ★ 10-20 v2 작업 3a — 방마다 그 방의 게임 서버 주소(옛 꼴은 접속마다 주소 하나를 모든 방에)
        const uint32_t gameIp = ci == conns_.end() ? (lr.gameIp ? lr.gameIp : cfg_.gameServerIp) : GameServerIp(ci->second, lr);
        const std::string room = Channel(lr);
        const size_t users = 1 + members_[room].size();
        out.push_back({conn, ": 326 u " + room + " " + std::to_string(users) + " 0 37 1 41681088 " +
                                 std::to_string(gameIp) + " " + std::string(kListPrefix) + RoomInfo(lr, codepage)});
    }
    // ★ 10-29 — 대화방 사람 수도 +1(위 방 줄과 같은 꼴). 정품은 채널마다 이 칸에서 시작해 들어옴 +1 · 나감 −1 로 세고
    //   0 이 되면 그 채널을 목록에서 지운다(Game.exe 0x41ee40 · 0x41eed4 → 0x41ea70 · 되살리는 길은 새 LIST 뿐).
    //   빈 채널을 0 으로 주면 한 번 들어갔다 나오는 것만으로 채널이 사라졌다
    if (withChat)
        for (const char* l : kChatLobbies)
            out.push_back({conn, ": 327 u " + std::string(l) + " " + std::to_string(1 + members_[l].size()) + " 0 388"});
    out.push_back({conn, ": 323 u:"});
}

// 353 의 이름 목록. 채팅 로비 = `<이름>,0,0` · 게임 방 = `@<방장>,0,<게임 서버 IP> <이름>,0,<그 사람 IP>`
//   ★ 10-14 작업 8d — 게임 방이면 `host`(방을 만든 사람)가 비어 있지 않다 · `gameIp` = 이 접속이 붙은 채널 주소
std::string Lobby::Members353(const std::string& channel, const std::string& host, uint32_t gameIp) const {
    const bool gameRoom = !host.empty();
    std::string r;
    if (gameRoom) r = "@" + host + ",0," + std::to_string(gameIp);
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
