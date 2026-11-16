// LobbyRoomsQueue.cpp — 조정 스레드(로비 연결)와 게임 스레드(방)를 큐로 잇는다(★ 11-17 C-3d-3 · 머리 파일 주석 참고)
#include <windows.h>

#include "nox/LobbyRoomsQueue.h"

#include "nox/NetworkThread.h"

namespace nox {

bool LobbyRoomReport::Empty() const {
    return changes.rooms.empty() && changes.roomsGone.empty() && changes.ui.empty() && changes.uiGone.empty() && creates.empty() && noShows.empty() &&
           replies.empty() && matches.empty();
}

// ── 큐 둘 ─────────────────────────────────────────
CLobbyRoomsHub::CLobbyRoomsHub(int gameThreads) {
    if (gameThreads < 1) gameThreads = 1;
    for (int k = 0; k < gameThreads; ++k) {
        auto q = std::make_unique<CommandQueue>();
        q->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 자동으로 되돌아가는 이벤트 — 깬 쪽이 통째로 비운다
        _commands.push_back(std::move(q));
    }
    _reportsReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

CLobbyRoomsHub::~CLobbyRoomsHub() {
    for (auto& q : _commands)
        if (q->ready) CloseHandle(q->ready);
    if (_reportsReady) CloseHandle(_reportsReady);
}

void CLobbyRoomsHub::PushCommand(int thread, LobbyRoomCommand c) {
    if (thread < 0 || thread >= GameThreads()) {
        ++_badThread;
        return;
    }
    CommandQueue& q = *_commands[thread];
    {
        std::lock_guard<std::mutex> g(q.lock);
        q.items.push_back(std::move(c));
    }
    SetEvent(q.ready);
}

void CLobbyRoomsHub::SwapOutCommands(int thread, std::vector<LobbyRoomCommand>& into) {
    into.clear();
    if (thread < 0 || thread >= GameThreads()) return;
    CommandQueue& q = *_commands[thread];
    std::lock_guard<std::mutex> g(q.lock);
    into.swap(q.items);
}

void* CLobbyRoomsHub::CommandsReadyHandle(int thread) const {
    return thread >= 0 && thread < GameThreads() ? _commands[thread]->ready : nullptr;
}

void CLobbyRoomsHub::PushReport(LobbyRoomReport r) {
    {
        std::lock_guard<std::mutex> g(_reportLock);
        _reports.push_back(std::move(r));
    }
    SetEvent(_reportsReady);
}

void CLobbyRoomsHub::SwapOutReports(std::vector<LobbyRoomReport>& into) {
    into.clear();
    std::lock_guard<std::mutex> g(_reportLock);
    into.swap(_reports);
}

// ── 게임 스레드 몫 ─────────────────────────────────
size_t CLobbyRoomsAgent::RunCommands() {
    _hub.SwapOutCommands(_thread, _cmds);
    for (LobbyRoomCommand& c : _cmds) {
        switch (c.type) {
            case LobbyRoomCommand::Type::Create: {
                LobbyCreateResult r;
                r.req = c.seq;
                r.roomId = _rooms.CreateRoom(c.settings);   // 번호는 이 스레드 몫(% N == k — `SetRoomIdSlice`)
                _out.creates.push_back(r);
                break;
            }
            case LobbyRoomCommand::Type::ClientGone: {
                LobbyRoomReport::Reply r;
                r.seq = c.seq;
                r.sessions = _rooms.OnClientLobbyGone(c.account);
                _out.replies.push_back(std::move(r));
                break;
            }
            case LobbyRoomCommand::Type::Kick: {
                LobbyRoomReport::Reply r;
                r.seq = c.seq;
                r.sessions = _rooms.KickAccount(c.account);
                _out.replies.push_back(std::move(r));
                break;
            }
            case LobbyRoomCommand::Type::NoticeRatings: {
                LobbyRoomReport::Reply r;
                r.seq = c.seq;
                r.found = NoticeRatingsInRoom(_rooms, c.roomId, c.notes, r.sent);
                _out.replies.push_back(std::move(r));
                break;
            }
        }
    }
    const size_t n = _cmds.size();
    _cmds.clear();
    return n;
}

void CLobbyRoomsAgent::AddMatch(std::string match, std::string json, int roomId) {
    LobbyRoomReport::Match m;
    m.match = std::move(match);
    m.json = std::move(json);
    m.roomId = roomId;
    _out.matches.push_back(std::move(m));
}

bool CLobbyRoomsAgent::Report() {
    // 새 방을 만든 바퀴면 그 방의 ROOM 바뀜이 같은 묶음에 실린다(결과보다 앞 — 조정 스레드가 묶음을 통째로 얹은 뒤 보낸다)
    _diff.Diff(_rooms.ListRooms(), _rooms.ListMemberUi(), _out.changes);
    _rooms.TakeRankNoShows(_out.noShows);
    const size_t rc = _rooms.RoomCount();
    if (_out.Empty() && _sentOnce && rc == _lastRoomCount) return false;
    _out.thread = _thread;
    _out.roomCount = rc;
    _hub.PushReport(std::move(_out));
    _out = LobbyRoomReport{};
    _sentOnce = true;
    _lastRoomCount = rc;
    return true;
}

// ── 조정 스레드 몫 ─────────────────────────────────
CQueuedLobbyRooms::CQueuedLobbyRooms(CLobbyRoomsHub& hub, bool rankOneMap)
    : _hub(hub), _rankOneMap(rankOneMap), _roomCount(static_cast<size_t>(hub.GameThreads()), 0), _inFlight(static_cast<size_t>(hub.GameThreads()), 0) {}

int CQueuedLobbyRooms::PickThread() const {
    // 구체화 C-나 — 방 수 + 답 없는 새 방 수가 가장 적은 스레드 · 같으면 작은 번호
    int best = 0;
    for (int k = 1; k < static_cast<int>(_roomCount.size()); ++k)
        if (_roomCount[k] + _inFlight[k] < _roomCount[best] + _inFlight[best]) best = k;
    return best;
}

void CQueuedLobbyRooms::CreateRoom(uint32_t req, const RoomSettings& s) {
    const int k = PickThread();
    ++_inFlight[k];
    LobbyRoomCommand c;
    c.type = LobbyRoomCommand::Type::Create;
    c.seq = req;
    c.settings = s;
    _hub.PushCommand(k, std::move(c));
}

void CQueuedLobbyRooms::Broadcast(LobbyCommandResult::Kind kind, const std::string& account) {
    // 구체화 C-④ — 그 이름이 어느 방인지 모르니 모든 게임 스레드에. 처리 수는 모두 답한 뒤 합으로 한 번
    const uint32_t seq = _nextSeq++;
    Waiting& w = _waiting[seq];
    w.result.kind = kind;
    w.result.account = account;
    w.left = _hub.GameThreads();
    for (int k = 0; k < _hub.GameThreads(); ++k) {
        LobbyRoomCommand c;
        c.type = kind == LobbyCommandResult::Kind::Kick ? LobbyRoomCommand::Type::Kick : LobbyRoomCommand::Type::ClientGone;
        c.seq = seq;
        c.account = account;
        _hub.PushCommand(k, std::move(c));
    }
}

void CQueuedLobbyRooms::NoticeRatings(int roomId, std::vector<std::pair<std::string, std::string>> notes) {
    if (roomId < 0) {   // 방 번호가 없는 판(옛 꼴도 곧바로 「방 없음」)
        LobbyCommandResult r;
        r.kind = LobbyCommandResult::Kind::NoticeRatings;
        r.roomId = roomId;
        r.notes = std::move(notes);
        _cmdDone.push_back(std::move(r));
        return;
    }
    const uint32_t seq = _nextSeq++;
    LobbyRoomCommand c;
    c.type = LobbyRoomCommand::Type::NoticeRatings;
    c.seq = seq;
    c.roomId = roomId;
    c.notes = notes;
    Waiting& w = _waiting[seq];
    w.result.kind = LobbyCommandResult::Kind::NoticeRatings;
    w.result.roomId = roomId;
    w.result.notes = std::move(notes);
    w.left = 1;
    _hub.PushCommand(roomId % _hub.GameThreads(), std::move(c));   // 방 주인 = 방 번호 % N(`CRoomManager::SetRoomIdSlice`)
}

void CQueuedLobbyRooms::Absorb(const LobbyRoomReport& r) {
    const int t = r.thread;
    if (t < 0 || t >= static_cast<int>(_roomCount.size())) {
        ++_unknownReplies;
        return;
    }
    _roomCount[t] = r.roomCount;
    for (const LobbyCreateResult& c : r.creates) {
        if (_inFlight[t] > 0) --_inFlight[t];
        _done.push_back(c);
    }
    _noShows.insert(_noShows.end(), r.noShows.begin(), r.noShows.end());
    // 방 — 번호는 한 스레드 몫이라 온 차례대로
    for (const auto& kv : r.changes.rooms) {
        RoomNow& n = _roomNow[kv.first];
        n.present = true;
        n.line = kv.second;
    }
    for (uint32_t id : r.changes.roomsGone) {
        RoomNow& n = _roomNow[id];
        n.present = false;
        n.line.clear();
    }
    // 사람 — 이름마다 스레드별 몫(나감은 그 스레드 몫만 지운다)
    for (const auto& kv : r.changes.ui) {
        _uiBy[kv.first][t] = std::make_pair(++_uiStamp, kv.second);
        _uiDirty.insert(kv.first);
    }
    for (const std::string& name : r.changes.uiGone) {
        const auto it = _uiBy.find(name);
        if (it != _uiBy.end()) {
            it->second.erase(t);
            if (it->second.empty()) _uiBy.erase(it);
        }
        _uiDirty.insert(name);
    }
    // 계정 명령 · 점수 알림의 답
    for (const LobbyRoomReport::Reply& a : r.replies) {
        const auto it = _waiting.find(a.seq);
        if (it == _waiting.end()) {
            ++_unknownReplies;
            continue;
        }
        Waiting& w = it->second;
        if (w.result.kind == LobbyCommandResult::Kind::NoticeRatings) {
            w.result.roomFound = a.found;
            w.result.sent = a.sent;
        } else {
            w.result.sessions += a.sessions;
        }
        if (--w.left > 0) continue;
        _cmdDone.push_back(std::move(w.result));
        _waiting.erase(it);
    }
}

void CQueuedLobbyRooms::TakeCreateResults(std::vector<LobbyCreateResult>& out) {
    out.insert(out.end(), _done.begin(), _done.end());
    _done.clear();
}

void CQueuedLobbyRooms::CollectChanges(LobbyFactChanges& out) {
    // 넘긴 것과 견줘 달라진 것만 — 방 번호 · 이름 차례(`CRoomFactDiff` 와 같은 꼴)
    for (const auto& kv : _roomNow) {
        const auto g = _roomGiven.find(kv.first);
        if (kv.second.present) {
            if (g != _roomGiven.end() && g->second == kv.second.line) continue;
            out.rooms.emplace_back(kv.first, kv.second.line);
            _roomGiven[kv.first] = kv.second.line;
        } else if (g != _roomGiven.end()) {
            out.roomsGone.push_back(kv.first);
            _roomGiven.erase(g);
        }
    }
    _roomNow.clear();
    for (const std::string& name : _uiDirty) {
        const auto g = _uiGiven.find(name);
        const auto it = _uiBy.find(name);
        if (it != _uiBy.end()) {
            const std::string* line = nullptr;
            unsigned long long newest = 0;
            for (const auto& byThread : it->second) {
                if (line && byThread.second.first < newest) continue;
                newest = byThread.second.first;
                line = &byThread.second.second;
            }
            if (g != _uiGiven.end() && g->second == *line) continue;
            out.ui.emplace_back(name, *line);
            _uiGiven[name] = *line;
        } else if (g != _uiGiven.end()) {
            out.uiGone.push_back(name);
            _uiGiven.erase(g);
        }
    }
    _uiDirty.clear();
}

void CQueuedLobbyRooms::TakeRankNoShows(std::vector<CRoomManager::RankNoShow>& out) {
    out.insert(out.end(), _noShows.begin(), _noShows.end());
    _noShows.clear();
}

void CQueuedLobbyRooms::TakeCommandResults(std::vector<LobbyCommandResult>& out) {
    for (LobbyCommandResult& r : _cmdDone) out.push_back(std::move(r));
    _cmdDone.clear();
}

// ── 조정 스레드 고리 ───────────────────────────────
void CLobbyCoordinator::Turn(const std::vector<NetworkEvent>& lobbyEvents, std::vector<NetworkRequest>& out) {
    ++_turns;
    for (const NetworkEvent& ev : lobbyEvents) _lobby.OnNetworkEvent(ev, out);
    _hub.SwapOutReports(_reports);
    for (const LobbyRoomReport& r : _reports) {
        ++_reportsTaken;
        _rooms.Absorb(r);
        // 옛 게임 고리와 같은 차례 — 판 결과(RESULT)가 그 바퀴의 방 바뀜(`AfterTick`)보다 먼저
        for (const LobbyRoomReport::Match& m : r.matches) {
            ++_matchesPassed;
            _lobby.OnMatchResult(m.match, m.json, out, m.roomId);
        }
    }
    _reports.clear();
    _lobby.AfterTick(out);
}

void CLobbyCoordinator::Run(CNetworkThread& net, int queue, const std::function<bool()>& stopRequested) {
    std::vector<NetworkEvent> evs;
    std::vector<NetworkRequest> out;
    HANDLE waits[2] = {net.EventsReadyHandle(queue), _hub.ReportsReadyHandle()};
    while (!stopRequested()) {
        WaitForMultipleObjects(2, waits, FALSE, kIdleWaitMs);
        net.SwapOutNetworkEvents(queue, evs);
        Turn(evs, out);
        evs.clear();
        if (!out.empty()) net.PushRequests(queue, out);
        out.clear();
    }
}

}  // namespace nox
