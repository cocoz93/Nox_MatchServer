// DbLobbyStore.cpp — 로비 저장소의 DB 쪽. 설명은 `DbLobbyStore.h` 머리.
#include "nox/DbLobbyStore.h"

#include <algorithm>

namespace nox::db {

CDbLobbyStore::CDbLobbyStore(const DbConfig& cfg) : writer_(cfg, DbWorkerRole::Writer), reader_(cfg, DbWorkerRole::Reader) {}

CDbLobbyStore::~CDbLobbyStore() { Stop(); }

bool CDbLobbyStore::Start(std::string& err) {
    if (!writer_.Start(err)) return false;
    if (!reader_.Start(err)) {
        writer_.Stop();
        return false;
    }
    return true;
}

void CDbLobbyStore::Stop() {
    reader_.StopDiscarding();
    writer_.Stop();
}

void CDbLobbyStore::SaveMatch(uint32_t req, const wol::MatchSource& src, const MatchRecord& rec, const std::string& rawJson) {
    writer_.SaveMatch(req, src, rec, rawJson);
}

void CDbLobbyStore::GetRating(uint32_t req, const std::string& account) { writer_.GetRating(req, account); }

void CDbLobbyStore::GetMatchHistory(uint32_t req, const std::string& account) { reader_.GetMatchHistory(req, account); }

void CDbLobbyStore::GetActiveBans(uint32_t req, uint32_t now) { reader_.GetActiveBans(req, now); }

void CDbLobbyStore::TakeAnswers(std::vector<wol::StoreAnswer>& out) {
    writer_.TakeAnswers(out);
    reader_.TakeAnswers(out);
}

void CDbLobbyStore::SetWake(std::function<void()> wake) {
    writer_.SetWake(wake);
    reader_.SetWake(std::move(wake));
}

DbStats CDbLobbyStore::Stats() const {
    DbStats s = writer_.Stats();
    const DbStats r = reader_.Stats();
    s.connects += r.connects;
    s.connectFailed += r.connectFailed;
    s.retries += r.retries;
    s.historyRequests += r.historyRequests;
    s.historyReads += r.historyReads;
    s.historyFailed += r.historyFailed;
    s.historyDropped += r.historyDropped;
    s.bansRequests += r.bansRequests;
    s.bansReads += r.bansReads;
    s.bansFailed += r.bansFailed;
    s.bansDropped += r.bansDropped;
    s.queueMax = std::max(s.queueMax, r.queueMax);
    if (s.lastError.empty()) s.lastError = r.lastError;
    return s;
}

}  // namespace nox::db
