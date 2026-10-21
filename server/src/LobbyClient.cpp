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
    : _rooms(rooms), _cfg(cfg), _authKey(LinkAuthKey(cfg.tokenKey)), _template(std::move(roomTemplate)), _waitMaps(std::move(waitMaps)),
      _battleMaps(std::move(battleMaps)), _rng(std::random_device{}()) {}

std::string CLobbyClient::RoomNameFor(const std::string& nick) {
    // ★ 10-16 — 영문(10-15 정품 확인 23 — 정품 목록에서 「의 게임」 한글이 깨짐 · `Docs/ROOMS_DESIGN.md` 「새 방 이름」의 대안 그대로)
    return nick.substr(0, kRoomNameNickBytes) + "'s game";
}

bool CLobbyClient::ToLinkRoom(const RoomFact& f, LinkRoom& lr) {
    if (f.creator.empty()) return false;   // 로비가 만들지 않은 방(실행 인자의 방) — 목록에 안 낸다
    lr.id = static_cast<uint32_t>(f.roomId);
    lr.creator = f.creator;
    // ★ 10-16 결함 ② — 채널 · 목록 이름은 **지금 방장**(`RoomFact::host`). 만든 사람에 묶어 두면 판 중에 나간 옛 방장의
    //   `Host Game`(제 이름 채널)이 그 방(판 중)으로 가 거절됐다. 방장이 만든 사람이면 방 설정의 이름 그대로
    lr.host = f.host.empty() ? f.creator : f.host;
    lr.name = lr.host == f.creator ? f.name : RoomNameFor(lr.host);
    lr.map = f.mapKey;
    lr.mode = f.mode;
    lr.players = static_cast<uint32_t>(f.players);
    lr.maxPlayers = static_cast<uint32_t>(std::max(f.maxPlayers, 0));
    lr.playing = f.phase == RoomPhase::Playing;
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
            _sent.clear();
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
            SendChanged(out);   // 붙을 때 지난번 보낸 것을 비웠으므로 로비 방 전부
            // ★ 7단계 조각 3 — 쥔 판 결과 전부(끊긴 동안 끝난 판 · 보냈는데 답을 못 받은 판 — 로비는 판 번호로 겹침을 거른다)
            if (!_held.empty()) {
                const uint32_t now = _nowMs();
                for (HeldResult& h : _held) SendHeld(h, now, out);
                std::printf("[로비 연결] 쥔 판 결과 %zu개를 보냄(SAVED 를 기다림)\n", _held.size());
            }
            return;
        case LinkKind::Create:
            if (_stage != Stage::Registered) break;
            OnCreate(l.req, l.text, out);
            return;
        case LinkKind::Bye:
            ++_stats.byes;
            std::printf("[로비 연결] 로비가 끊음 — %s(설정 키 · 등록 주소 확인)\n", Printable(l.text).c_str());
            return;
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
                case SaveOutcome::Ok: ++_stats.savedOk; break;
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
        case LinkKind::RoomGone:
        case LinkKind::Created:
        case LinkKind::CreateFailed:
        case LinkKind::Result:
        case LinkKind::kCount:
            break;
    }
    ++_stats.badLines;
    std::printf("[로비 연결] 차례에 안 맞는 줄 — 버림: %s\n", Printable(line.substr(0, 80)).c_str());
}

void CLobbyClient::OnCreate(uint32_t req, const std::string& creator, std::vector<NetworkRequest>& out) {
    ++_stats.creates;
    std::string why;
    const int id = creator.empty() ? CRoomManager::kNoRoom : CreateRoomFor(creator, why);
    if (creator.empty()) why = "만든 사람 없음";
    if (id == CRoomManager::kNoRoom) {
        ++_stats.createFailed;
        std::printf("[로비 연결] 새 방 실패 — 요청 %u · 만든 사람 %s · %s\n", req, Printable(creator).c_str(), why.c_str());
        Send(LinkCreateFailed(req, why), out);
        return;
    }
    // 규약 — 답 앞에 그 방의 ROOM(로비 거울이 그 방을 먼저 알아야 참가 답 · 토큰을 낸다)
    SendChanged(out);
    ++_stats.created;
    Send(LinkCreated(req, static_cast<uint32_t>(id)), out);
}

// ★ 10-14 작업 10a — 새 방: 틀 + 만든 사람 · 이름 · 대기 맵(무작위) · 채팅 모드 · 비면 닫음 → 방 관리자의 만들기 입구
// ★ 10-20 v2 작업 4b — 옛 `CChannelLobby::CreateRoomFor` 몸통 그대로(로비가 이제 다른 프로세스라 연결 줄 `CREATE` 로 묻는다).
//   못 만든 까닭은 `CREATE_FAILED` 로 로비 기록에 실린다
int CLobbyClient::CreateRoomFor(const std::string& creator, std::string& why) {
    if (_waitMaps.empty()) {
        why = "대기 맵 후보 없음(--wait-maps)";
        return CRoomManager::kNoRoom;
    }
    RoomSettings s = _template;
    s.creator = creator;
    s.roomName = RoomNameFor(creator);
    s.mapPath = _waitMaps[std::uniform_int_distribution<size_t>(0, _waitMaps.size() - 1)(_rng)];
    s.mode = "chat";        // 대기 = 원작 채팅 모드 맵(결정 1 — 싸움 막기는 작업 13)
    // ★ 10-16 작업 13a — 대전 맵(후보에서 무작위 · 없으면 빈 것 — `!start` 가 「대전 맵 없음」) · 아레나(「10-16 정한 것」)
    s.battleMapPath = _battleMaps.empty() ? std::string()
                                          : _battleMaps[std::uniform_int_distribution<size_t>(0, _battleMaps.size() - 1)(_rng)];
    s.battleMode = "arena";
    s.standalone = true;    // 맵을 직접 차린다(녹음 서두 없음)
    s.closeWhenEmpty = true;
    s.maxPlayers = kRoomMaxPlayers;   // ★ 10-16 작업 12a
    s.bots.clear();         // 봇은 실행 인자의 방 몫(네트워크 스레드를 띄운 뒤라 세울 수도 없다)
    const int id = _rooms.CreateRoom(s);
    if (id == CRoomManager::kNoRoom) why = "방 차리기 실패(게임 서버 [방] 줄)";
    return id;
}

void CLobbyClient::SendChanged(std::vector<NetworkRequest>& out) {
    if (_stage != Stage::Registered) return;
    std::map<uint32_t, std::string> now;
    for (const RoomFact& f : _rooms.ListRooms()) {
        LinkRoom lr;
        if (!ToLinkRoom(f, lr)) continue;
        now[lr.id] = LinkRoomLine(lr);
    }
    for (const auto& kv : now) {
        const auto s = _sent.find(kv.first);
        if (s != _sent.end() && s->second == kv.second) continue;
        ++_stats.roomLines;
        Send(kv.second, out);
    }
    for (const auto& kv : _sent) {
        if (now.count(kv.first)) continue;
        ++_stats.roomGoneLines;
        Send(LinkRoomGone(kv.first), out);
    }
    _sent.swap(now);
}

void CLobbyClient::AfterTick(std::vector<NetworkRequest>& out) {
    LinkTick(out);
    SendChanged(out);
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

void CLobbyClient::OnMatchResult(const std::string& match, const std::string& json, std::vector<NetworkRequest>& out) {
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
    if (_stage != Stage::Registered) {
        ++_stats.resultsHeldDown;
        std::printf("[로비 연결] 판 결과를 쥠 — %s(연결 없음 · 등록 전 — 붙어 등록하면 보냄)\n", Printable(match).c_str());
        std::fflush(stdout);
        return;
    }
    SendHeld(_held.back(), _nowMs(), out);
}

}  // namespace nox
