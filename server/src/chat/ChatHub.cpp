// ChatHub.cpp — 머리 주석은 `ChatHub.h`.
#include "nox/ChatHub.h"

#include "nox/AccountRules.h"   // 이름 규칙(영문 · 숫자 9자)

namespace nox::chat {

const char* ChatOutcomeName(ChatOutcome o) {
    switch (o) {
        case ChatOutcome::AuthOk: return "로그인";
        case ChatOutcome::AuthNoAuth: return "로그인(토큰 없이)";
        case ChatOutcome::AuthBadForm: return "로그인 꼴 틀림";
        case ChatOutcome::AuthToken: return "토큰 거절";
        case ChatOutcome::AuthReplay: return "다시 쓴 토큰";
        case ChatOutcome::AuthBanned: return "정지 중 로그인";
        case ChatOutcome::AuthAgain: return "로그인 두 번";
        case ChatOutcome::Replaced: return "같은 이름 — 옛 연결 끊음";
        case ChatOutcome::Sent: return "전달";
        case ChatOutcome::NoUser: return "접속 중 아님";
        case ChatOutcome::Limited: return "너무 빠름";
        case ChatOutcome::ToSelf: return "자기에게";
        case ChatOutcome::NotAuthed: return "로그인 전 귓속말";
        case ChatOutcome::BadLine: return "틀린 줄";
        case ChatOutcome::PingAnswered: return "PING 답";
        case ChatOutcome::PongSeen: return "PONG 받음";
        case ChatOutcome::AfterClose: return "끊는 연결의 줄";
        case ChatOutcome::IdleClosed: return "조용해서 끊음";
        case ChatOutcome::BanClosed: return "정지로 끊음";
        case ChatOutcome::kCount: break;
    }
    return "?";
}

size_t Utf8Chars(std::string_view s) {
    size_t n = 0;
    for (const char ch : s)
        if ((static_cast<uint8_t>(ch) & 0xC0) != 0x80) ++n;
    return n;
}

std::string Utf8Head(std::string_view s, size_t maxChars) {
    size_t chars = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) continue;   // 앞 글자에 붙은 바이트
        if (chars == maxChars) break;                                // 다음 글자의 첫 바이트에서 멈춘다
        ++chars;
    }
    return std::string(s.substr(0, i));
}

namespace {

// 앞 낱말과 나머지(빈칸 하나로 가름). 빈칸이 없으면 나머지는 빈 값
void SplitWord(std::string_view s, std::string_view& word, std::string_view& rest) {
    const size_t sp = s.find(' ');
    word = s.substr(0, sp);
    rest = sp == std::string_view::npos ? std::string_view() : s.substr(sp + 1);
}

}  // namespace

void CChatHub::OnConnect(int conn, uint32_t peerIp, uint64_t nowMs) {
    Conn& c = conns_[conn];
    c = Conn{};
    c.peerIp = peerIp;
    c.lastRecvMs = nowMs;
    c.lastPingMs = nowMs;
}

void CChatHub::OnClose(int conn) {
    const auto it = conns_.find(conn);
    if (it == conns_.end()) return;
    if (!it->second.name.empty()) {
        const auto n = byName_.find(it->second.name);
        if (n != byName_.end() && n->second == conn) byName_.erase(n);   // 새 연결이 이름을 가져갔으면 그대로
    }
    conns_.erase(it);
}

void CChatHub::OnLine(int conn, std::string_view line, uint64_t nowMs, uint32_t unixNow, std::vector<ChatOut>& out) {
    const auto it = conns_.find(conn);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    if (c.closing) {
        Count(ChatOutcome::AfterClose);
        return;
    }
    c.lastRecvMs = nowMs;
    std::string_view cmd, args;
    SplitWord(line, cmd, args);
    if (cmd == "AUTH") {
        OnAuth(conn, c, args, unixNow, out);
    } else if (cmd == "W") {
        OnWhisper(conn, c, args, nowMs, out);
    } else if (cmd == "PING" && args.empty()) {
        Count(ChatOutcome::PingAnswered);
        out.push_back({conn, "PONG"});
    } else if (cmd == "PONG" && args.empty()) {
        Count(ChatOutcome::PongSeen);
    } else {
        Count(ChatOutcome::BadLine);
    }
}

void CChatHub::Refuse(int conn, Conn& c, ChatOutcome why, const char* word, std::vector<ChatOut>& out) {
    Count(why);
    log_.push_back(std::string("[로그인 거절] c") + std::to_string(conn) + " — " + ChatOutcomeName(why));
    c.closing = true;
    out.push_back({conn, std::string("AUTHNO ") + word, true});
}

void CChatHub::OnAuth(int conn, Conn& c, std::string_view args, uint32_t unixNow, std::vector<ChatOut>& out) {
    if (!c.name.empty()) {   // 이미 로그인 — DLL 은 안 보낸다. 연결은 그대로 둔다
        Count(ChatOutcome::AuthAgain);
        out.push_back({conn, "AUTHNO again"});
        return;
    }
    std::string_view nameView, token;
    SplitWord(args, nameView, token);
    const std::string name(nameView);
    if (CheckAccountName(name) != NameCheck::Ok || (!cfg_.noAuth && (token.empty() || token.find(' ') != std::string_view::npos))) {
        Refuse(conn, c, ChatOutcome::AuthBadForm, "form", out);
        return;
    }
    if (!cfg_.noAuth) {
        uint8_t raw[kLoginTokenSize];
        LoginToken t;
        const LoginTokenCheck r = ParseHex(std::string(token), raw, sizeof(raw))
                                      ? VerifyLoginToken(cfg_.loginKey, raw, sizeof(raw), unixNow, c.peerIp, name, t)
                                      : LoginTokenCheck::BadSize;
        ++stats_.tokenCheck[static_cast<size_t>(r)];
        if (r != LoginTokenCheck::Ok) {
            log_.push_back("[로그인 — 토큰] " + name + " — " + LoginTokenCheckName(r));
            Refuse(conn, c, ChatOutcome::AuthToken, "token", out);
            return;
        }
        if (!nonces_.Consume(t.nonce, t.expiresAt, unixNow)) {
            Refuse(conn, c, ChatOutcome::AuthReplay, "replay", out);
            return;
        }
    }
    if (bans_.UntilOf(name, unixNow) != 0) {
        Refuse(conn, c, ChatOutcome::AuthBanned, "banned", out);
        return;
    }
    // 같은 이름의 옛 연결 — 로비 재로그인과 같다(`Lobby.cpp` 재로그인). KICKED 를 받은 DLL 은 다시 붙지 않아 두 게임이 서로를 끊지 않는다
    if (const auto old = byName_.find(name); old != byName_.end()) {
        const auto oc = conns_.find(old->second);
        if (oc != conns_.end()) {
            Count(ChatOutcome::Replaced);
            log_.push_back("[같은 이름] " + name + " — c" + std::to_string(old->second) + " 끊고 c" + std::to_string(conn));
            Drop(old->second, oc->second, "KICKED", out);
        }
    }
    c.name = name;
    byName_[name] = conn;
    if (byName_.size() > stats_.maxOnline) stats_.maxOnline = byName_.size();
    Count(cfg_.noAuth ? ChatOutcome::AuthNoAuth : ChatOutcome::AuthOk);
    log_.push_back("[로그인] " + name + " — c" + std::to_string(conn) + (cfg_.noAuth ? "(토큰 없이)" : "") + " · 접속 " + std::to_string(byName_.size()));
    out.push_back({conn, "AUTHOK"});
}

void CChatHub::OnWhisper(int conn, Conn& c, std::string_view args, uint64_t nowMs, std::vector<ChatOut>& out) {
    if (c.name.empty()) {
        Count(ChatOutcome::NotAuthed);
        return;
    }
    std::string_view targetView, text;
    SplitWord(args, targetView, text);
    if (targetView.empty() || text.find_first_not_of(' ') == std::string_view::npos) {
        Count(ChatOutcome::BadLine);
        return;
    }
    const std::string target(targetView);
    if (target == c.name) {
        Count(ChatOutcome::ToSelf);
        return;
    }
    // 보낸 사람당 5초에 3줄 — 없는 사람에게 보낸 것도 센다(이름 훑기를 막는다)
    std::deque<uint64_t>& times = rate_[c.name];
    while (!times.empty() && nowMs - times.front() >= kWhisperRateWindowMs) times.pop_front();
    if (times.size() >= kWhisperRateMax) {
        Count(ChatOutcome::Limited);
        out.push_back({conn, "LIMIT"});
        return;
    }
    times.push_back(nowMs);
    const auto to = byName_.find(target);
    if (to == byName_.end()) {
        Count(ChatOutcome::NoUser);
        out.push_back({conn, "NOUSER " + target});
        return;
    }
    std::string body = Utf8Head(text, kWhisperMaxChars);
    if (body.size() < text.size()) ++stats_.truncated;
    for (char& ch : body)   // 제어 바이트는 빈칸으로 — 받는 DLL 의 C 글자열 · 화면이 깨지지 않게
        if (static_cast<uint8_t>(ch) < 0x20 || ch == 0x7f) ch = ' ';
    const size_t chars = Utf8Chars(body);
    stats_.sentChars += chars;
    Count(ChatOutcome::Sent);
    log_.push_back("[귓속말] c" + std::to_string(conn) + " → c" + std::to_string(to->second) + " · " + std::to_string(chars) + "자");
    out.push_back({to->second, "F " + c.name + " " + body});
    out.push_back({conn, "TO " + target + " " + body});
}

void CChatHub::Drop(int conn, Conn& c, const char* line, std::vector<ChatOut>& out) {
    if (!c.name.empty()) {
        const auto n = byName_.find(c.name);
        if (n != byName_.end() && n->second == conn) byName_.erase(n);
    }
    c.closing = true;
    out.push_back({conn, line, true});
}

void CChatHub::Tick(uint64_t nowMs, uint32_t unixNow, std::vector<ChatOut>& out) {
    for (auto& kv : conns_) {
        Conn& c = kv.second;
        if (c.closing) continue;
        if (nowMs - c.lastRecvMs >= kChatIdleMs) {
            Count(ChatOutcome::IdleClosed);
            log_.push_back("[조용해서 끊음] c" + std::to_string(kv.first) + (c.name.empty() ? std::string(" · 로그인 전") : " · " + c.name));
            Drop(kv.first, c, "", out);
            continue;
        }
        if (!c.name.empty() && nowMs - c.lastRecvMs >= kChatPingMs && nowMs - c.lastPingMs >= kChatPingMs) {
            c.lastPingMs = nowMs;
            ++stats_.pingsSent;
            out.push_back({kv.first, "PING"});
        }
    }
    nonces_.Prune(unixNow);
    for (auto it = rate_.begin(); it != rate_.end();) {
        if (it->second.empty() || nowMs - it->second.back() >= kWhisperRateWindowMs) it = rate_.erase(it);
        else ++it;
    }
}

size_t CChatHub::ApplyBans(const std::vector<wol::BanEntry>& rows, uint32_t unixNow, std::vector<ChatOut>& out) {
    size_t closed = 0;
    for (const std::string& name : bans_.Replace(rows, unixNow)) {
        const auto n = byName_.find(name);
        if (n == byName_.end()) continue;
        const auto c = conns_.find(n->second);
        if (c == conns_.end()) continue;
        Count(ChatOutcome::BanClosed);
        log_.push_back("[정지 — 끊음] " + name + " — c" + std::to_string(n->second));
        Drop(n->second, c->second, "", out);
        ++closed;
    }
    return closed;
}

std::vector<std::string> CChatHub::TakeLog() {
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

}  // namespace nox::chat
