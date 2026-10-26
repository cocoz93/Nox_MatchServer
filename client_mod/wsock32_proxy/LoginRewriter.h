// LoginRewriter.h — 로비 · 라우터로 나가는 로그인 줄을 줄 단위로 모아 정해진 칸만 바꿔 내보내는 보내기 감싸기(소켓 하나의 상태).
//   DLL(wsock32_proxy.cpp)과 반복 시험(login_rewrite_test.cpp)이 같이 쓴다 — 시험이 본 코드가 곧 DLL 코드다(`LobbyLineFilter.h` 와 같은 꼴).
//   설계 Docs/LOGIN_AUTH_DESIGN.md 「DLL — 고침」 · 줄 꼴은 서버와 같은 `nox/LoginLine.h`.
//
// 왜 줄 단위인가 — 정품은 로그인 묶음을 64바이트씩 쪼개 보내서 `apgar` 줄이 두 조각에 걸칠 수 있다
//   (`_evidence/_rooms_check_20261015/wsock_Game.exe_23936.log:14`). 그래서 줄 끝(\n)이 올 때까지 쥐고 있다가 줄째 판단한다.
//   다만 바꿀 줄의 머리와 겹치지 않는 줄은 덜 와도 바로 내보낸다(로비 모듈 기다림을 늘리지 않으려고).
//
// 모드 둘
//   Lobby  — `NICK <이름>` 을 기억해 두고, `apgar <암호> 0` 의 암호 자리를 onLogin(이름, 암호) 이 돌려준 글로 바꾼다.
//            손님(`None`)은 그대로 · `USER` 줄이 지나면 더 안 본다(로그인 묶음은 거기까지)
//   Router — `whereto <이름> <암호> …` 의 암호 칸을 `x` 로 바꾼다(라우터는 암호를 안 쓴다). 손님(빈 칸)은 그대로 · 그 줄이 지나면 더 안 본다
//
// 길이 — 바꾼 줄은 길이가 달라진다. 게임(부르는 쪽)에는 늘 원래 길이를 돌려주고, 실제로 나가는 몫은 여기 쌓는다(Pending).
//   Flush 가 진짜 send 로 내보내고, 막지 않는 소켓이 일부만 받거나 WOULDBLOCK 이면 남은 몫을 쥐고 있다가 다음 Flush 에 이어 보낸다
#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <functional>
#include <string>

#include "nox/LoginLine.h"

class CLoginRewriter {
public:
    enum class Mode { Lobby, Router };

    explicit CLoginRewriter(Mode mode) : mode_(mode) {}

    // Lobby 모드에서 `apgar` 자리에 넣을 글을 돌려준다(이름 · 암호). 돌려준 글이 비었거나 빈칸 · 줄바꿈이 있으면 `NXL!server_error` 로 대신한다
    std::function<std::string(const std::string& name, const std::string& password)> onLogin;

    // 게임이 보낸 바이트를 넣는다. 나갈 바이트는 Pending() 에 쌓인다
    void Feed(const char* p, int n) {
        if (n <= 0) return;
        std::string data = hold_ + std::string(p, static_cast<size_t>(n));
        hold_.clear();
        size_t i = 0;
        while (i < data.size()) {
            if (done_) {   // 로그인 줄이 지났다 — 나머지는 통째 통과
                out_.append(data, i, std::string::npos);
                return;
            }
            const size_t nl = data.find('\n', i);
            if (midPass_) {   // 앞 조각에서 이미 내보낸 줄의 나머지 — 줄 끝까지 통과
                if (nl == std::string::npos) {
                    out_.append(data, i, std::string::npos);
                    return;
                }
                out_.append(data, i, nl + 1 - i);
                i = nl + 1;
                midPass_ = false;
                continue;
            }
            if (nl == std::string::npos) {   // 줄 끝이 덜 왔다
                const std::string rest = data.substr(i);
                if (rest.size() <= kMaxHold && Watched(rest)) {
                    hold_ = rest;
                } else {
                    out_ += rest;
                    midPass_ = true;
                }
                return;
            }
            Line(data.substr(i, nl + 1 - i));
            i = nl + 1;
        }
    }

    // 지금 내보낼 바이트(앞에서부터)
    const std::string& Pending() const { return out_; }
    // 줄이 덜 와서 쥐고 있는 바이트
    size_t HeldBytes() const { return hold_.size(); }
    int Logins() const { return logins_; }                  // 암호 자리를 바꾼 수(Lobby)
    int Guests() const { return guests_; }                  // 손님(`None`)이라 그대로 보낸 수(Lobby)
    int RouterRewrites() const { return routerRewrites_; }  // 암호 칸을 `x` 로 바꾼 수(Router)

    // Pending 을 real(buf, len) 로 내보낸다. real 은 send 와 같은 뜻(보낸 바이트 수 · 음수 오류).
    //   막지 않는 소켓이 일부만 받으면 남은 몫을 쥔다(0 을 돌려줌 — 다음 Flush 에 이어서). 진짜 오류면 -1(쥔 것을 버림)
    template <class RealSend, class WouldBlock>
    int Flush(RealSend&& real, WouldBlock&& wouldBlock) {
        while (!out_.empty()) {
            const int len = static_cast<int>((std::min)(out_.size(), static_cast<size_t>(INT_MAX)));
            const int r = real(out_.data(), len);
            if (r > 0) {
                out_.erase(0, static_cast<size_t>(r));
                continue;
            }
            if (r == 0 || wouldBlock()) return 0;
            out_.clear();
            return -1;
        }
        return 0;
    }

private:
    static constexpr size_t kMaxHold = 256;   // 줄 끝 없이 이만큼 넘게 쌓이면 바꿀 줄이 아니다 — 쥐지 않고 통과

    // 줄 머리 — 줄 하나가 이것으로 시작하면 바꿀 후보(소문자)
    bool Watched(const std::string& partial) const {
        static const char* const lobby[] = { "nick ", "apgar ", "user " };
        static const char* const router[] = { "whereto " };
        const char* const* marks = mode_ == Mode::Lobby ? lobby : router;
        const size_t count = mode_ == Mode::Lobby ? 3 : 1;
        for (size_t m = 0; m < count; ++m) {
            const std::string mark = marks[m];
            const size_t n = (std::min)(partial.size(), mark.size());
            bool same = true;
            for (size_t k = 0; k < n && same; ++k) {
                char c = partial[k];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                same = (c == mark[k]);
            }
            if (same) return true;   // 머리와 겹친다 — 부분이 머리의 앞부분이거나 머리로 시작
        }
        return false;
    }

    static bool ValidToken(const std::string& t) {
        if (t.empty() || t.size() > 512) return false;
        for (const char c : t)
            if (c == ' ' || c == '\r' || c == '\n') return false;
        return true;
    }

    void Line(const std::string& line) {
        size_t bodyEnd = line.size();
        while (bodyEnd > 0 && (line[bodyEnd - 1] == '\n' || line[bodyEnd - 1] == '\r')) --bodyEnd;
        const std::string body = line.substr(0, bodyEnd);
        const std::string eol = line.substr(bodyEnd);
        using nox::login_line_detail::StartsWithNoCase;
        if (mode_ == Mode::Lobby) {
            if (StartsWithNoCase(body, "nick ")) {
                const size_t e = body.find(' ', 5);
                name_ = body.substr(5, (e == std::string::npos ? body.size() : e) - 5);
                out_ += line;
                return;
            }
            if (StartsWithNoCase(body, "user ")) {
                done_ = true;
                out_ += line;
                return;
            }
            if (StartsWithNoCase(body, "apgar ")) {
                LobbyApgar(body, eol);
                return;
            }
        } else if (StartsWithNoCase(body, "whereto ")) {
            done_ = true;
            RouterWhereto(body, eol);
            return;
        }
        out_ += line;
    }

    void LobbyApgar(const std::string& body, const std::string& eol) {
        size_t e = body.find(' ', 6);
        if (e == std::string::npos) e = body.size();
        const std::string arg = body.substr(6, e - 6);
        if (arg.empty()) {   // 인자 없는 줄 — 로비가 가려서 보게 그대로
            out_ += body + eol;
            return;
        }
        if (arg == nox::kGuestApgar) {
            ++guests_;
            out_ += body + eol;
            return;
        }
        std::string token = onLogin ? onLogin(name_, arg) : std::string();
        if (!ValidToken(token)) token = std::string(nox::kLoginFailMark) + nox::LoginFailName(nox::LoginFail::ServerError);
        ++logins_;
        out_ += body.substr(0, 6) + token + body.substr(e) + eol;
    }

    void RouterWhereto(const std::string& body, const std::string& eol) {
        const size_t s1 = body.find(' ', 8);   // 이름 끝
        if (s1 == std::string::npos) {
            out_ += body + eol;
            return;
        }
        const size_t pwBegin = s1 + 1;
        const size_t s2 = body.find(' ', pwBegin);   // 암호 끝
        const size_t pwEnd = s2 == std::string::npos ? body.size() : s2;
        if (pwEnd == pwBegin) {   // 손님 — 이름 · 암호 칸이 빔
            out_ += body + eol;
            return;
        }
        ++routerRewrites_;
        out_ += body.substr(0, pwBegin) + "x" + body.substr(pwEnd) + eol;
    }

    Mode mode_;
    std::string out_;        // 내보낼 바이트
    std::string hold_;       // 줄 끝이 덜 와서 쥔 바이트(바꿀 줄의 머리와 겹칠 때만)
    std::string name_;       // Lobby — 마지막 `NICK`
    bool midPass_ = false;   // 앞 조각에서 줄 가운데까지 이미 내보냈다
    bool done_ = false;      // 로그인 줄이 지났다
    int logins_ = 0;
    int guests_ = 0;
    int routerRewrites_ = 0;
};
