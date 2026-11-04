// RecordDesk.cpp — 전적 창구. 설명은 `RecordDesk.h` 머리.
#include "nox/RecordDesk.h"

#include <utility>

#include "nox/AccountRules.h"
#include "nox/MatchHistory.h"

namespace nox::wol {

void RecordDesk::AskMine(int conn, const std::string& nick, std::vector<Out>& out) { Ask(conn, nick, true, out); }

void RecordDesk::AskName(int conn, const std::string& name, std::vector<Out>& out) { Ask(conn, name, false, out); }

void RecordDesk::Ask(int conn, const std::string& name, bool mine, std::vector<Out>& out) {
    if (reqOfConn_.count(conn) != 0) {
        ++stats_.recordBusy;
        return;
    }
    ++stats_.recordAsked;
    if (!mine && CheckAccountName(name) != NameCheck::Ok) {   // 계정 이름이 될 수 없는 이름 — DB 까지 갈 까닭이 없다
        AnswerNoUser(conn, name, out);
        return;
    }
    if (!store_) {
        ++stats_.recordFailed;
        out.push_back({conn, "NOXRANK RECFAIL"});
        return;
    }
    const uint32_t req = nextReq_++;
    pending_[req] = Asking{conn, name, mine};
    reqOfConn_[conn] = req;
    store_->GetMatchHistory(req, name);
}

void RecordDesk::OnClose(int conn) {
    const auto r = reqOfConn_.find(conn);
    if (r == reqOfConn_.end()) return;
    pending_.erase(r->second);
    reqOfConn_.erase(r);
    ++stats_.recordClosed;
}

void RecordDesk::OnAnswer(const StoreAnswer& a, std::vector<Out>& out) {
    const auto p = pending_.find(a.req);
    if (a.kind != StoreAnswerKind::History || p == pending_.end()) {
        ++stats_.recordLate;   // 끊긴 물음 · 안 물은 답
        return;
    }
    const Asking w = std::move(p->second);
    pending_.erase(p);
    reqOfConn_.erase(w.conn);
    if (!a.historyOk) {
        ++stats_.recordFailed;
        log_.push_back("[대전 기록 — 못 읽음] " + w.name + " — " + (a.why.empty() ? std::string("저장소 실패") : a.why));
        out.push_back({w.conn, "NOXRANK RECFAIL"});
        return;
    }
    if (!w.mine && !a.accountFound) {
        AnswerNoUser(w.conn, w.name, out);
        return;
    }
    ++stats_.recordSent;
    log_.push_back("[대전 기록] " + w.name + " — 판 " + std::to_string(a.history.totals.games) + " · 줄 " + std::to_string(a.history.rows.size()));
    for (std::string& l : MatchHistoryLines(w.name, a.ratingFound, a.rating, a.rank, a.history)) out.push_back({w.conn, std::move(l)});
}

void RecordDesk::AnswerNoUser(int conn, const std::string& name, std::vector<Out>& out) {
    ++stats_.recordNoUser;
    const std::string echo = name.substr(0, kRecordEchoMaxBytes);
    log_.push_back("[전적 검색 — 없는 사용자] " + echo);
    out.push_back({conn, "NOXRANK RECNONE " + echo});
}

std::vector<std::string> RecordDesk::TakeLog() {
    std::vector<std::string> l;
    l.swap(log_);
    return l;
}

}  // namespace nox::wol
