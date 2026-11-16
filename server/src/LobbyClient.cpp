// LobbyClient.cpp — 게임 서버의 로비 연결. 설명은 `LobbyClient.h` 머리 · 줄 꼴은 `LobbyLink.h`.
#include "nox/LobbyClient.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace nox {

namespace {

// 우리 기록(한글)은 제어 문자만 거른다(옛 `ChannelLobby.cpp` 의 `Printable` 중 「ours」 갈래)
std::string Printable(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7f) c = '.';
    }
    return r;
}

}  // namespace

CLobbyClient::CLobbyClient(CRoomManager& rooms, Config cfg, RoomSettings roomTemplate, std::vector<std::string> waitMaps,
                           std::vector<std::string> battleMaps)
    : _ownedRooms(std::make_unique<CDirectLobbyRooms>(rooms)), _rooms(*_ownedRooms), _cfg(cfg), _authKey(LinkAuthKey(cfg.tokenKey)),
      _template(std::move(roomTemplate)), _waitMaps(std::move(waitMaps)), _battleMaps(std::move(battleMaps)), _rng(std::random_device{}()) {}

CLobbyClient::CLobbyClient(ILobbyRooms& rooms, Config cfg, RoomSettings roomTemplate, std::vector<std::string> waitMaps,
                           std::vector<std::string> battleMaps)
    : _rooms(rooms), _cfg(cfg), _authKey(LinkAuthKey(cfg.tokenKey)), _template(std::move(roomTemplate)), _waitMaps(std::move(waitMaps)),
      _battleMaps(std::move(battleMaps)), _rng(std::random_device{}()) {}

std::string CLobbyClient::RoomNameFor(const std::string& nick) {
    // ★ 10-16 — 영문(10-15 정품 확인 23 — 정품 목록에서 「의 게임」 한글이 깨짐 · `Docs/ROOMS_DESIGN.md` 「새 방 이름」의 대안 그대로)
    return nick.substr(0, kRoomNameNickBytes) + "'s game";
}

bool CRoomFactDiff::ToLinkRoom(const RoomFact& f, LinkRoom& lr) {
    if (f.creator.empty()) return false;   // 로비가 만들지 않은 방(실행 인자의 방) — 목록에 안 낸다
    lr.id = static_cast<uint32_t>(f.roomId);
    lr.creator = f.creator;
    // ★ 10-16 결함 ② — 채널 · 목록 이름은 **지금 방장**(`RoomFact::host`). 만든 사람에 묶어 두면 판 중에 나간 옛 방장의
    //   `Host Game`(제 이름 채널)이 그 방(판 중)으로 가 거절됐다. 방장이 만든 사람이면 방 설정의 이름 그대로
    lr.host = f.host.empty() ? f.creator : f.host;
    lr.name = lr.host == f.creator ? f.name : CLobbyClient::RoomNameFor(lr.host);
    lr.map = f.mapKey;
    lr.battleMap = f.battleMapName;   // ★ 11-06 플랜1 조각 6
    lr.mode = f.mode;
    lr.players = static_cast<uint32_t>(f.players);
    lr.maxPlayers = static_cast<uint32_t>(std::max(f.maxPlayers, 0));
    lr.playing = f.phase == RoomPhase::Playing;
    return true;
}

// ★ 11-17 C-3c — 옛 `CLobbyClient::SendChanged` 의 줄 짓기 · 견주기 그대로(보내기만 뺐다 — 보내는 것은 `CLobbyClient`)
std::string CRoomFactDiff::UiLine(const MemberUiFact& f) {
    LinkRoomUi u;
    u.name = f.account;
    u.state = f.rankArriving ? RoomUiState::RankArriving : f.phase == RoomPhase::Playing ? RoomUiState::Playing : RoomUiState::Waiting;
    u.host = f.host;
    u.canStart = f.canStart;
    u.scoreLimit = f.scoreLimit;
    u.timeLimitMin = (f.timeLimitSec + 59) / 60;   // ★ 11-07 T1 — 초 → 분(올림 · 정품 `af` 19번과 같은 셈 `Server::WireSettings`)
    u.map = f.battleMap;
    return LinkRoomUiLine(u);
}

void CRoomFactDiff::Diff(const std::vector<RoomFact>& rooms, const std::vector<MemberUiFact>& ui, LobbyFactChanges& out) {
    std::map<uint32_t, std::string> now;
    for (const RoomFact& f : rooms) {
        LinkRoom lr;
        if (!ToLinkRoom(f, lr)) continue;
        now[lr.id] = LinkRoomLine(lr);
    }
    for (const auto& kv : now) {
        const auto s = _rooms.find(kv.first);
        if (s != _rooms.end() && s->second == kv.second) continue;
        out.rooms.emplace_back(kv.first, kv.second);
    }
    for (const auto& kv : _rooms)
        if (!now.count(kv.first)) out.roomsGone.push_back(kv.first);
    _rooms.swap(now);
    // ★ 11-04 플랜1 — 방 안 화면 상태(방 줄과 같은 꼴 — 달라진 사람만)
    std::map<std::string, std::string> uiNow;
    for (const MemberUiFact& f : ui) uiNow[f.account] = UiLine(f);
    for (const auto& kv : uiNow) {
        const auto s = _ui.find(kv.first);
        if (s != _ui.end() && s->second == kv.second) continue;
        out.ui.emplace_back(kv.first, kv.second);
    }
    for (const auto& kv : _ui)
        if (!uiNow.count(kv.first)) out.uiGone.push_back(kv.first);
    _ui.swap(uiNow);
}

void CDirectLobbyRooms::CreateRoom(uint32_t req, const RoomSettings& s) {
    LobbyCreateResult r;
    r.req = req;
    r.roomId = _rooms.CreateRoom(s);
    _done.push_back(r);
}

void CDirectLobbyRooms::TakeCreateResults(std::vector<LobbyCreateResult>& out) {
    out.insert(out.end(), _done.begin(), _done.end());
    _done.clear();
}

// ★ 11-17 C-3d-3 — 결과는 부른 자리에서 곧바로(셈 · 기록 줄은 `CLobbyClient::FinishCommands` — 부르자마자 꺼내 옛 꼴과 줄 · 차례가 같다)
void CDirectLobbyRooms::ClientLobbyGone(const std::string& account) {
    LobbyCommandResult r;
    r.kind = LobbyCommandResult::Kind::ClientGone;
    r.account = account;
    r.sessions = _rooms.OnClientLobbyGone(account);
    _cmdDone.push_back(std::move(r));
}

void CDirectLobbyRooms::KickAccount(const std::string& account) {
    LobbyCommandResult r;
    r.kind = LobbyCommandResult::Kind::Kick;
    r.account = account;
    r.sessions = _rooms.KickAccount(account);
    _cmdDone.push_back(std::move(r));
}

void CDirectLobbyRooms::NoticeRatings(int roomId, std::vector<std::pair<std::string, std::string>> notes) {
    LobbyCommandResult r;
    r.kind = LobbyCommandResult::Kind::NoticeRatings;
    r.roomId = roomId;
    r.roomFound = NoticeRatingsInRoom(_rooms, roomId, notes, r.sent);
    r.notes = std::move(notes);
    _cmdDone.push_back(std::move(r));
}

void CDirectLobbyRooms::TakeCommandResults(std::vector<LobbyCommandResult>& out) {
    for (LobbyCommandResult& r : _cmdDone) out.push_back(std::move(r));
    _cmdDone.clear();
}

bool NoticeRatingsInRoom(CRoomManager& rooms, int roomId, const std::vector<std::pair<std::string, std::string>>& notes, std::vector<size_t>& sent) {
    if (roomId < 0 || !rooms.HasRoom(roomId)) return false;
    CRoom& room = rooms.Room(roomId);
    for (const auto& note : notes) {
        size_t n = 0;
        for (const RoomMember& m : room.Members()) {
            if (m.account != note.first) continue;
            room.Game().SendNoticeBoxTo(m.sessionId, note.second);   // ★ 10-31 — 말풍선(`SendNoticeTo`) → 정품 알림창
            ++n;
        }
        sent.push_back(n);
    }
    return true;
}

void CLobbyClient::Send(const std::string& line, std::vector<NetworkRequest>& out) {
    NetworkRequest r;
    r.type = NetworkRequest::Type::LOBBY_SEND;
    r.lobbyConn = _conn;
    r.lobbyLine = line;
    out.push_back(std::move(r));
    // ★ 7단계 — 살아 있음 줄은 5초마다라 찍지 않고 셈만(끝 줄 PING)
    if (_verbose && line != LinkPing()) std::printf("[로비 연결] << %s\n", Printable(line).c_str());
}

void CLobbyClient::OnNetworkEvent(const NetworkEvent& ev, std::vector<NetworkRequest>& out) {
    switch (ev.type) {
        case NetworkEvent::Type::LOBBY_OPENED:
            ++_stats.linksUp;
            _conn = ev.lobbyConn;
            _stage = Stage::WaitHello;
            // ★ 11-17 C-3c — 새 로비엔 다시 다 알린다 — 사본은 비우지 않고 등록 때 전부 보낸다(`SendAll` · 옛 꼴은 여기서 지난번 보낸 것을 비움)
            _lastRecvMs = _nowMs();   // ★ 7단계 — 붙은 때부터 잰다(HELLO 도 안 오는 로비 — 멈춘 로비의 대기 줄에 붙음 — 도 한도에 끊는다)
            _nextPingMs = _lastRecvMs + kLinkPingEveryMs;
            _closeAsked = false;
            std::printf("[로비 연결] 붙음 — 로비 %s · 확인 기다림\n", Ipv4ToString(ev.lobbyPeerIp).c_str());
            std::fflush(stdout);
            return;
        case NetworkEvent::Type::LOBBY_LINE:
            if (ev.lobbyConn != _conn) return;   // 끊긴 옛 접속의 남은 줄
            _lastRecvMs = _nowMs();               // ★ 7단계 — 어떤 줄이든 살아 있다는 뜻
            if (_verbose && ev.lobbyLine != LinkPing()) std::printf("[로비 연결] >> %s\n", Printable(ev.lobbyLine).c_str());
            OnLine(ev.lobbyLine, out);
            std::fflush(stdout);
            return;
        case NetworkEvent::Type::LOBBY_CLOSED:
            if (ev.lobbyConn != _conn) return;
            ++_stats.linksDown;
            std::printf("[로비 연결] 끊김 — %s · 네트워크 스레드가 다시 붙는다(진행 중 판 · 입장은 그대로 · 새 방만 못 생김)\n", ev.lobbyWhy);
            std::fflush(stdout);
            _conn = 0;
            _stage = Stage::Down;   // 지난번 보낸 것은 다시 붙을 때 비운다(끊긴 동안은 아무것도 안 보낸다)
            _closeAsked = false;
            return;
        case NetworkEvent::Type::CONNECTED:
        case NetworkEvent::Type::DISCONNECTED:
        case NetworkEvent::Type::RECEIVED:
        case NetworkEvent::Type::JOIN_REQUESTED:
            return;   // 방 · 게임 몫(게임 스레드가 방 관리자로 나눈다)
    }
}

void CLobbyClient::OnLine(const std::string& line, std::vector<NetworkRequest>& out) {
    LinkLine l;
    const LinkParse p = ParseLinkLine(line, l);
    if (p != LinkParse::Ok) {
        ++_stats.badLines;
        std::printf("[로비 연결] 못 읽는 줄(%s) — 버림: %s\n", LinkParseName(p), Printable(line.substr(0, 80)).c_str());
        return;
    }
    switch (l.kind) {
        case LinkKind::Hello:
            if (_stage != Stage::WaitHello) break;
            // 같은 설정 키를 쥐었다는 서명 · 등록 · 돌고 있는 로비 방 전부(로비 거울은 끊김에 이 게임 서버 방을 내렸다 — 처음부터)
            Send(LinkAuth(LinkAuthSig(_authKey, l.text)), out);
            Send(LinkRegister(_cfg.epoch, _cfg.registerIp, _cfg.port), out);
            ++_stats.registers;
            _stage = Stage::Registered;
            std::printf("[로비 연결] 등록 — 실행 %u · 클라가 붙을 주소 %s:%u\n", _cfg.epoch,
                        _cfg.registerIp ? Ipv4ToString(_cfg.registerIp).c_str() : "(로비 쪽 주소)", _cfg.port);
            SendAll(out);   // 로비 방 전부(★ 11-17 C-3c — 옛 꼴은 붙을 때 지난번 보낸 것을 비워 `SendChanged` 로 전부)
            // ★ 7단계 조각 3 — 쥔 판 결과 전부(끊긴 동안 끝난 판 · 보냈는데 답을 못 받은 판 — 로비는 판 번호로 겹침을 거른다)
            if (!_held.empty()) {
                const uint32_t now = _nowMs();
                for (HeldResult& h : _held) SendHeld(h, now, out);
                std::printf("[로비 연결] 쥔 판 결과 %zu개를 보냄(SAVED 를 기다림)\n", _held.size());
            }
            return;
        case LinkKind::Create:
            if (_stage != Stage::Registered) break;
            OnCreate(l, out);
            return;
        case LinkKind::Bye:
            ++_stats.byes;
            std::printf("[로비 연결] 로비가 끊음 — %s(설정 키 · 등록 주소 확인)\n", Printable(l.text).c_str());
            return;
        case LinkKind::ClientGone: {   // ★ 11-03 — 시간 초과 한도만 줄인다(`Server::ClientLobbyGone`)
            ++_stats.clientGoneIn;
            _rooms.ClientLobbyGone(l.text);
            FinishCommands();   // ★ 11-17 C-3d-3 — 셈 · 기록 줄은 결과에서(한 스레드 판은 곧바로)
            return;
        }
        case LinkKind::Kick: {   // ★ 11-05 플랜1 — 계정 정지. 그 이름이 없으면 0(로비가 게임 서버 전부에 보낸다)
            ++_stats.kickIn;
            _rooms.KickAccount(l.text);
            FinishCommands();   // ★ 11-17 C-3d-3
            return;
        }
        case LinkKind::Ping:   // ★ 7단계 — 받은 시각은 사건에서 적었다(어느 단계든)
            ++_stats.pingsIn;
            return;
        case LinkKind::Saved: {   // ★ 7단계 조각 3 — 셋 다 지운다(bad 는 다시 보내도 같다 — 파일엔 있다)
            const auto it = std::find_if(_held.begin(), _held.end(), [&l](const HeldResult& h) { return h.match == l.text; });
            if (it == _held.end()) {
                ++_stats.savedUnknown;   // 다시 보낸 판의 둘째 답 등 — 찍지 않고 셈만
                return;
            }
            switch (l.saved) {
                case SaveOutcome::Ok:
                    ++_stats.savedOk;
                    NoticeRatings(it->roomId, l.ratings);   // ★ 매칭 손보기 1 — 랭크 판이면 꼬리(판 끝 쉬는 동안 온다 — 방은 아직 있음)
                    break;
                case SaveOutcome::Dup: ++_stats.savedDup; break;
                case SaveOutcome::Bad:
                    ++_stats.savedBad;
                    std::printf("[로비 연결] 로비가 판 결과를 못 읽어 버림 — %s(파일 기록기엔 있음)\n", Printable(l.text).c_str());
                    std::fflush(stdout);
                    break;
                case SaveOutcome::kCount: break;
            }
            _held.erase(it);
            return;
        }
        case LinkKind::Auth:
        case LinkKind::Register:
        case LinkKind::Room:
        case LinkKind::RoomUi:
        case LinkKind::RoomGone:
        case LinkKind::Created:
        case LinkKind::CreateFailed:
        case LinkKind::Result:
        case LinkKind::RankNoShow:
        case LinkKind::kCount:
            break;
    }
    ++_stats.badLines;
    std::printf("[로비 연결] 차례에 안 맞는 줄 — 버림: %s\n", Printable(line.substr(0, 80)).c_str());
}

void CLobbyClient::OnCreate(const LinkLine& l, std::vector<NetworkRequest>& out) {
    const uint32_t req = l.req;
    const std::string& creator = l.text;
    ++_stats.creates;
    if (l.ranked) ++_stats.createsRanked;
    std::string why;
    RoomSettings s;
    const bool ok = !creator.empty() && RoomSettingsFor(creator, l.ranked, l.roster, why, s, l.rosterRatings);
    if (creator.empty()) why = "만든 사람 없음";
    if (!ok) {
        CreateFailed(req, creator, why, out);
        return;
    }
    // ★ 11-17 C-3c — 만들어라 → 결과(5-4). 한 스레드 판은 여기서 결과가 곧바로 있다 · 여럿이면 게임 스레드가 답한 뒤 `AfterTick`
    PendingCreate& p = _pending[req];
    p.creator = creator;
    p.ranked = l.ranked;
    p.roster = l.roster;
    _rooms.CreateRoom(req, s);
    FinishCreates(out);
}

void CLobbyClient::CreateFailed(uint32_t req, const std::string& creator, const std::string& why, std::vector<NetworkRequest>& out) {
    ++_stats.createFailed;
    std::printf("[로비 연결] 새 방 실패 — 요청 %u · 만든 사람 %s · %s\n", req, Printable(creator).c_str(), why.c_str());
    Send(LinkCreateFailed(req, why), out);
}

void CLobbyClient::FinishCreates(std::vector<NetworkRequest>& out) {
    std::vector<LobbyCreateResult> done;
    _rooms.TakeCreateResults(done);
    for (const LobbyCreateResult& d : done) {
        PendingCreate p;
        const auto it = _pending.find(d.req);
        if (it != _pending.end()) {
            p = std::move(it->second);
            _pending.erase(it);
        }
        if (d.roomId == CRoomManager::kNoRoom) {
            CreateFailed(d.req, p.creator, "방 차리기 실패(게임 서버 [방] 줄)", out);
            continue;
        }
        if (p.ranked) {   // ★ 랭크 매칭 조각 5 — 명단은 기록에(판 시작 규칙은 방이 — 조각 6)
            std::string names;
            for (const std::string& n : p.roster) names += (names.empty() ? "" : ",") + n;
            std::printf("[로비 연결] 랭크 방 %d — 요청 %u · 명단 %s(%zu명)\n", d.roomId, d.req, Printable(names).c_str(), p.roster.size());
        }
        // 규약 — 답 앞에 그 방의 ROOM(로비 거울이 그 방을 먼저 알아야 참가 답 · 토큰을 낸다)
        SendChanged(out);
        ++_stats.created;
        Send(LinkCreated(d.req, static_cast<uint32_t>(d.roomId)), out);
    }
}

void ApplyLobbyRoomFrame(RoomSettings& s, const std::string& waitMap, const std::vector<std::string>& battleMaps,
                         const std::string& battleMap, int maxPlayers) {
    s.mapPath = waitMap;
    s.mode = "chat";        // 대기 = 원작 채팅 모드 맵(결정 1 — 싸움 막기는 작업 13)
    // ★ 10-16 작업 13a — 대전 맵(후보에서 무작위 · 없으면 빈 것 — `!start` 가 「대전 맵 없음」) · 아레나(「10-16 정한 것」)
    s.battleMapChoices = battleMaps;
    s.battleMapPath = battleMap;
    s.battleMode = "arena";
    s.standalone = true;    // 맵을 직접 차린다(녹음 서두 없음)
    s.closeWhenEmpty = true;
    s.maxPlayers = maxPlayers;   // ★ 10-16 작업 12a
    s.bots.clear();         // 봇은 실행 인자의 방 몫(네트워크 스레드를 띄운 뒤라 세울 수도 없다)
}

// ★ 10-14 작업 10a — 새 방: 틀 + 만든 사람 · 이름 · 대기 맵(무작위) · 채팅 모드 · 비면 닫음 → 방 관리자의 만들기 입구
// ★ 10-20 v2 작업 4b — 옛 `CChannelLobby::CreateRoomFor` 몸통 그대로(로비가 이제 다른 프로세스라 연결 줄 `CREATE` 로 묻는다).
//   못 만든 까닭은 `CREATE_FAILED` 로 로비 기록에 실린다
bool CLobbyClient::RoomSettingsFor(const std::string& creator, bool ranked, const std::vector<std::string>& roster, std::string& why,
                                   RoomSettings& s, const std::vector<int32_t>& ratings) {
    if (_waitMaps.empty()) {
        why = "대기 맵 후보 없음(--wait-maps)";
        return false;
    }
    s = _template;
    s.creator = creator;
    s.ranked = s.ranked || ranked;   // ★ 랭크 매칭 조각 5 — 틀이 랭크(`--ranked-all` 시험)면 그대로
    s.roster = roster;
    s.rosterRatings = ratings.size() == roster.size() ? ratings : std::vector<int32_t>{};   // ★ 매칭 손보기 5
    s.roomName = RoomNameFor(creator);
    const std::string waitMap = _waitMaps[std::uniform_int_distribution<size_t>(0, _waitMaps.size() - 1)(_rng)];
    // ★ 11-06 플랜1 조각 6 — 무작위는 랭크 방만(방장이 없어 고를 사람이 없고 맵 유불리가 갈림). 일반 방 = 맨 앞 맵 · 방장이 `!map` 으로 바꿈
    const std::string battleMap = _battleMaps.empty() ? std::string()
                                  : s.ranked          ? _battleMaps[std::uniform_int_distribution<size_t>(0, _battleMaps.size() - 1)(_rng)]
                                                      : _battleMaps.front();
    ApplyLobbyRoomFrame(s, waitMap, _battleMaps, battleMap, kRoomMaxPlayers);   // ★ 11-10 부하 실험 — 실험 방과 같은 틀
    // ★ 매칭 손보기 3(파일럿 · `--rank-one-map`) — 랭크 방은 처음부터 대전 맵 · 대전 모드(대기 맵 = 대전 맵 → 방 관리자가 제자리 판)
    if (ranked && _rooms.RankOneMap() && !s.battleMapPath.empty()) {
        s.mapPath = s.battleMapPath;
        s.mode = s.battleMode;
    }
    // ★ 랭크 매칭 조각 6 — 랭크 방은 명단 전부를 받는다(일반 방 8 은 10-16 사용자 결정 · 랭크 최대는 로비 설정 — 기본 12 · 10-30 사용자)
    if (ranked && roster.size() > static_cast<size_t>(s.maxPlayers)) s.maxPlayers = static_cast<int>(roster.size());
    return true;   // ★ 11-17 C-3c — 만들기 · 그 실패 까닭(「방 차리기 실패」)은 `FinishCreates`
}

void CLobbyClient::AbsorbChanges(LobbyFactChanges& ch) {
    _rooms.CollectChanges(ch);
    for (const auto& kv : ch.rooms) _roomLines[kv.first] = kv.second;
    for (const uint32_t id : ch.roomsGone) _roomLines.erase(id);
    for (const auto& kv : ch.ui) _uiLines[kv.first] = kv.second;
    for (const std::string& n : ch.uiGone) _uiLines.erase(n);
}

void CLobbyClient::SendChanged(std::vector<NetworkRequest>& out) {
    LobbyFactChanges ch;
    AbsorbChanges(ch);   // 끊긴 동안에도 사본은 고친다(등록 때 `SendAll`)
    if (_stage != Stage::Registered) return;
    for (const auto& kv : ch.rooms) {
        ++_stats.roomLines;
        Send(kv.second, out);
    }
    for (const uint32_t id : ch.roomsGone) {
        ++_stats.roomGoneLines;
        Send(LinkRoomGone(id), out);
    }
    // ★ 11-04 플랜1 — 방 안 화면 상태(방 줄과 같은 꼴 — 달라진 사람만)
    for (const auto& kv : ch.ui) Send(kv.second, out);
    for (const std::string& n : ch.uiGone) {
        LinkRoomUi gone;
        gone.name = n;
        Send(LinkRoomUiLine(gone), out);
    }
}

void CLobbyClient::SendAll(std::vector<NetworkRequest>& out) {
    if (_stage != Stage::Registered) return;
    LobbyFactChanges ch;
    AbsorbChanges(ch);
    for (const auto& kv : _roomLines) {
        ++_stats.roomLines;
        Send(kv.second, out);
    }
    for (const auto& kv : _uiLines) Send(kv.second, out);
}

void CLobbyClient::AfterTick(std::vector<NetworkRequest>& out) {
    LinkTick(out);
    FinishCreates(out);   // ★ 11-17 C-3c — 늦게 온 새 방 결과(한 스레드 판은 없다)
    FinishCommands();     // ★ 11-17 C-3d-3 — 늦게 온 계정 명령 · 점수 알림 결과(한 스레드 판은 없다)
    SendChanged(out);
    SendRankNoShows(out);
}

// ★ 11-05 플랜1 — 끊긴 동안은 쥐고 있다가 붙으면 보낸다(로비는 참가 답을 보낸 지 오래된 방은 안 센다 · `Lobby.h` `kRankJoinedKeepMs`)
void CLobbyClient::SendRankNoShows(std::vector<NetworkRequest>& out) {
    if (_stage != Stage::Registered) return;
    std::vector<CRoomManager::RankNoShow> names;
    _rooms.TakeRankNoShows(names);
    for (const CRoomManager::RankNoShow& n : names) {
        ++_stats.rankNoShowOut;
        Send(LinkRankNoShow(static_cast<uint32_t>(n.room), n.name), out);
        std::printf("[로비 연결] RANK_NOSHOW 방 %d %s\n", n.room, Printable(n.name).c_str());
        std::fflush(stdout);
    }
}

void CLobbyClient::LinkTick(std::vector<NetworkRequest>& out) {
    if (_conn == 0 || _closeAsked) return;
    const uint32_t now = _nowMs();
    if (now - _lastRecvMs >= kLinkDeadMs) {   // 32비트 시계가 돌아도 맞게(뺄셈)
        ++_stats.deadLinks;
        _closeAsked = true;
        std::printf("[로비 연결] %u초 동안 받은 줄 없음 — 끊고 다시 붙음(로비가 멈췄거나 로비 PC 가 꺼짐)\n", kLinkDeadMs / 1000);
        std::fflush(stdout);
        NetworkRequest r;
        r.type = NetworkRequest::Type::LOBBY_CLOSE;
        r.lobbyConn = _conn;
        out.push_back(std::move(r));
        return;
    }
    if (_stage == Stage::Registered && static_cast<int32_t>(now - _nextPingMs) >= 0) {
        ++_stats.pingsOut;
        Send(LinkPing(), out);
        _nextPingMs = now + kLinkPingEveryMs;
    }
    // ★ 7단계 조각 3 — 붙어 있어도 답이 없는 판은 다시(로비는 DB 에 못 넣으면 답을 안 한다 · 겹침은 로비가 거른다)
    if (_stage == Stage::Registered)
        for (HeldResult& h : _held)
            if (now - h.sentMs >= kResultResendMs) SendHeld(h, now, out);
}

void CLobbyClient::SendHeld(HeldResult& h, uint32_t now, std::vector<NetworkRequest>& out) {
    if (h.sends++ == 0) ++_stats.results;
    else ++_stats.resultsResent;
    h.sentMs = now;
    Send(h.line, out);
}

void CLobbyClient::OnMatchResult(const std::string& match, const std::string& json, std::vector<NetworkRequest>& out, int roomId) {
    std::string line;
    if (!LinkResult(json, line)) {   // 줄로 못 지음 — 다시 보내도 같다
        ++_stats.resultsNotSent;
        std::printf("[로비 연결] 판 결과를 로비에 못 보냄 — 줄 한도 · 줄바꿈 %s(파일 기록기엔 있음)\n", Printable(match).c_str());
        std::fflush(stdout);
        return;
    }
    if (_held.size() >= kMaxHeldResults) {
        ++_stats.resultsDropped;
        std::printf("[로비 연결] 쥔 판 결과가 %zu개 — 가장 옛 판 %s 를 버림(파일 기록기엔 있음)\n", _held.size(), Printable(_held.front().match).c_str());
        std::fflush(stdout);
        _held.pop_front();
    }
    _held.push_back(HeldResult{match, std::move(line)});
    _held.back().roomId = roomId;   // ★ 매칭 손보기 1
    if (_stage != Stage::Registered) {
        ++_stats.resultsHeldDown;
        std::printf("[로비 연결] 판 결과를 쥠 — %s(연결 없음 · 등록 전 — 붙어 등록하면 보냄)\n", Printable(match).c_str());
        std::fflush(stdout);
        return;
    }
    SendHeld(_held.back(), _nowMs(), out);
}

// ★ 매칭 손보기 1 — 머리 주석은 `LobbyClient.h`. 증감은 부호를 붙여(+10 · -10 · +0)
std::string CLobbyClient::RatingNoticeText(int32_t after, int32_t delta) {
    // ★ 10-31 — 앞말 = 오름 · 내림 · 같음 세 갈래(시험 ④ 가 셋 다 봄)
    const char* head = delta > 0 ? "랭크 상승!" : delta < 0 ? "랭크 하락!" : "랭크 유지";
    char b[96];
    std::snprintf(b, sizeof(b), "%s %ld -> %ld (%+ld)", head, static_cast<long>(after) - static_cast<long>(delta), static_cast<long>(after),
                  static_cast<long>(delta));
    return b;
}

void CLobbyClient::NoticeRatings(int roomId, const std::vector<LinkRatingChange>& ratings) {
    if (ratings.empty()) return;
    // ★ 11-17 C-3c — 보내기는 틀이(그 방의 게임 스레드) · 수와 기록 줄은 여기서(옛 꼴과 같은 줄 · 같은 차례)
    //   ★ 11-17 C-3d-3 — 수와 기록 줄은 결과가 올 때(`FinishCommands` — 한 스레드 판은 곧바로)
    std::vector<std::pair<std::string, std::string>> notes;
    for (const LinkRatingChange& c : ratings) notes.emplace_back(c.account, RatingNoticeText(c.after, c.delta));
    _rooms.NoticeRatings(roomId, std::move(notes));
    FinishCommands();
}

void CLobbyClient::FinishCommands() {
    std::vector<LobbyCommandResult> done;
    _rooms.TakeCommandResults(done);
    for (const LobbyCommandResult& r : done) {
        switch (r.kind) {
            case LobbyCommandResult::Kind::ClientGone:
                _stats.clientGoneShortened += r.sessions;
                std::printf("[로비 연결] CLIENT_GONE %s — 들어 있던 세션 %zu개 한도 줄임\n", Printable(r.account).c_str(), r.sessions);
                break;
            case LobbyCommandResult::Kind::Kick:
                _stats.kickedSessions += r.sessions;
                std::printf("[로비 연결] KICK %s — 내보낸 세션 %zu개\n", Printable(r.account).c_str(), r.sessions);
                break;
            case LobbyCommandResult::Kind::NoticeRatings:
                if (!r.roomFound || r.sent.size() != r.notes.size()) {
                    _stats.ratingNoRoom += r.notes.size();
                    std::printf("[로비 연결] 점수 알림 %zu명 — 판의 방 %d 이 없음(닫힘) · 안 보냄\n", r.notes.size(), r.roomId);
                    std::fflush(stdout);
                    break;
                }
                for (size_t i = 0; i < r.notes.size(); ++i) {
                    const size_t n = r.sent[i];
                    if (n == 0) ++_stats.ratingNotHere;
                    _stats.ratingNotices += n;
                    std::printf("[로비 연결] 방 %d 점수 알림 — %s 「%s」 %zu명\n", r.roomId, Printable(r.notes[i].first).c_str(), r.notes[i].second.c_str(),
                                n);
                }
                std::fflush(stdout);
                break;
        }
    }
}

}  // namespace nox
