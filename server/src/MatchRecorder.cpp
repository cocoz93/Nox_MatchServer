// ★ 10-19 MO 5단계 작업 3 — 판 결과 기록기. 머리 주석은 `MatchRecorder.h`
#include "nox/MatchRecorder.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace nox {

namespace {

// UTF-8 로 맞는 바이트열인가(겹친 꼴 · 대리 자리 · 4바이트 넘는 값은 틀린 것으로)
bool IsUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;   // 뒤 바이트가 모자람
        for (size_t k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;   // 겹친 꼴
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

// JSON 문자열(따옴표 포함)
void PutJsonString(std::string& out, const std::string& s) {
    const bool utf8 = IsUtf8(s);
    out += '"';
    char buf[8];
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            default: break;
        }
        if (c < 0x20 || (c >= 0x80 && !utf8)) {
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += ch;
        }
    }
    out += '"';
}

void PutKeyString(std::string& out, const char* key, const std::string& v) {
    out += '"'; out += key; out += "\":";
    PutJsonString(out, v);
}

template <class T> void PutKeyNumber(std::string& out, const char* key, T v) {
    out += '"'; out += key; out += "\":";
    out += std::to_string(v);
}

const char* StateName(const MatchPlayer& p) { return p.left ? "left" : "finished"; }

// 사람을 부르는 이름 — 로비 이름, 없으면 `#<netCode>`(10-19 결정 「빈 이름 = 사람 번호」)
std::string DisplayName(const MatchPlayer& p) {
    return p.account.empty() ? "#" + std::to_string(p.netCode) : p.account;
}

}  // namespace

std::string MatchIdOf(const MatchResult& r) {
    return std::to_string(r.epoch) + "-" + std::to_string(r.roomId) + "-" + std::to_string(r.matchNo);
}

std::string FormatMatchResultJson(const MatchResult& r) {
    std::string o;
    o.reserve(256 + 96 * r.players.size());
    o += '{';
    PutKeyString(o, "match", MatchIdOf(r)); o += ',';
    PutKeyNumber(o, "room", r.roomId); o += ',';
    PutKeyString(o, "roomName", r.roomName); o += ',';
    PutKeyString(o, "map", r.mapKey); o += ',';
    PutKeyString(o, "mode", r.mode); o += ',';
    PutKeyNumber(o, "start", r.startUnix); o += ',';
    PutKeyNumber(o, "end", r.endUnix); o += ',';
    PutKeyNumber(o, "frames", r.frames); o += ',';
    PutKeyNumber(o, "sec", r.frames / 30u); o += ',';
    PutKeyString(o, "endReason", MatchEndName(r)); o += ',';
    o += "\"winner\":{";
    PutKeyString(o, "kind", RoundWinnerKindName(r.winner));
    switch (r.winner) {
        case RoundWinnerKind::Player:
            o += ','; PutKeyNumber(o, "id", r.winnerId);
            o += ",\"account\":";
            if (r.winnerAccount.empty()) o += "null"; else PutJsonString(o, r.winnerAccount);
            break;
        case RoundWinnerKind::Team:
            o += ','; PutKeyNumber(o, "id", r.winnerId);
            break;
        case RoundWinnerKind::None:
        case RoundWinnerKind::Draw:
        case RoundWinnerKind::kCount:
            break;
    }
    o += "},";
    PutKeyNumber(o, "limit", r.limit); o += ',';
    PutKeyNumber(o, "timeLimitSec", r.timeLimitFrames / 30u); o += ',';
    o += "\"players\":[";
    for (size_t i = 0; i < r.players.size(); ++i) {
        const MatchPlayer& p = r.players[i];
        if (i) o += ',';
        o += "{\"account\":";
        if (p.account.empty()) o += "null"; else PutJsonString(o, p.account);
        o += ','; PutKeyNumber(o, "netCode", p.netCode);
        o += ','; PutKeyString(o, "state", StateName(p));
        if (!p.left) {
            o += ','; PutKeyNumber(o, "team", static_cast<unsigned>(p.team));
            o += ','; PutKeyNumber(o, "score", p.score);
            o += ','; PutKeyNumber(o, "deaths", p.deaths);
            if (p.observer) o += ",\"observer\":true";
        }
        o += '}';
    }
    o += ']';
    if (!r.teams.empty()) {
        o += ",\"teams\":[";
        for (size_t i = 0; i < r.teams.size(); ++i) {
            const RoundTeamScore& t = r.teams[i];
            if (i) o += ',';
            o += '{';
            PutKeyNumber(o, "id", static_cast<unsigned>(t.id)); o += ',';
            PutKeyNumber(o, "score", t.score); o += ',';
            PutKeyNumber(o, "caps", static_cast<unsigned>(t.caps));
            o += '}';
        }
        o += ']';
    }
    o += '}';
    return o;
}

std::string FormatMatchResultLine(const MatchResult& r) {
    std::string o = "[판 결과] " + MatchIdOf(r) + " · 방 " + std::to_string(r.roomId);
    if (!r.roomName.empty()) o += " 「" + r.roomName + "」";
    o += " · " + r.mapKey + "(" + r.mode + ") · " + MatchEndName(r) + " · ";
    switch (r.winner) {
        case RoundWinnerKind::Player:
            o += "승자 " + (r.winnerAccount.empty() ? "#" + std::to_string(r.winnerId) : r.winnerAccount);
            break;
        case RoundWinnerKind::Team:   o += "승자 팀 " + std::to_string(r.winnerId); break;
        case RoundWinnerKind::Draw:   o += "비김"; break;
        case RoundWinnerKind::None:   o += "승자 없음"; break;
        case RoundWinnerKind::kCount: o += "?"; break;
    }
    o += " · " + std::to_string(r.frames / 30u) + "초";
    for (const MatchPlayer& p : r.players) {
        o += " · " + DisplayName(p);
        if (p.left) o += " 나감";
        else o += " " + std::to_string(p.score) + "/" + std::to_string(p.deaths);
    }
    return o;
}

bool CMatchRecorder::Start() {
    {
        std::lock_guard<std::mutex> g(_mu);
        if (_started) return true;
        _started = true;
        _stop = false;
    }
    bool ok = true;
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        ok = static_cast<bool>(f);
    }
    _thread = std::thread([this] { Run(); });
    return ok;
}

void CMatchRecorder::Push(const MatchResult& r) {
    std::string line = FormatMatchResultJson(r);
    ++_pushed;
    {
        std::lock_guard<std::mutex> g(_mu);
        _queue.push_back(std::move(line));
    }
    _cv.notify_one();
}

void CMatchRecorder::Stop() {
    {
        std::lock_guard<std::mutex> g(_mu);
        if (!_started) return;
        _stop = true;
    }
    _cv.notify_one();
    if (_thread.joinable()) _thread.join();
    std::lock_guard<std::mutex> g(_mu);
    _started = false;
}

CMatchRecorder::Stats CMatchRecorder::GetStats() const {
    Stats s;
    s.pushed = _pushed.load();
    s.written = _written.load();
    s.writeFailed = _writeFailed.load();
    return s;
}

void CMatchRecorder::Run() {
    std::unique_lock<std::mutex> lk(_mu);
    for (;;) {
        _cv.wait(lk, [this] { return _stop || !_queue.empty(); });
        while (!_queue.empty()) {
            std::string line = std::move(_queue.front());
            _queue.pop_front();
            lk.unlock();
            if (WriteLine(line)) ++_written;
            else ++_writeFailed;
            lk.lock();
        }
        if (_stop) return;   // 큐를 다 비운 뒤에만 끝난다
    }
}

// 줄마다 열고 닫는다 — 판 하나에 한 줄이라 드물고, 열어 둔 채 죽어도 앞 줄이 남는다
bool CMatchRecorder::WriteLine(const std::string& line) {
    std::ofstream f(std::filesystem::u8path(_path), std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write(line.data(), static_cast<std::streamsize>(line.size()));
    f.put('\n');
    f.flush();
    return static_cast<bool>(f);
}

}  // namespace nox
