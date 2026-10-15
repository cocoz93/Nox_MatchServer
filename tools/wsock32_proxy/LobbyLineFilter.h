// LobbyLineFilter.h — 로비 TCP 받기에서 「NOXTOKEN …\r\n」 줄만 떼어 내는 recv 감싸기(소켓 하나의 상태).
//   DLL(wsock32_proxy.cpp)과 반복 시험(linefilter_test.cpp)이 같이 쓴다 — 시험이 본 코드가 곧 DLL 코드다.
//
// TCP 는 줄 경계를 지키지 않는다 — 로비가 한 줄을 한 번에 보내도 클라의 recv 는 `NOXTO` 까지만 받을 수 있다.
// 그래서 줄 머리에서 표시와 겹치는 바이트는 줄 끝(\r\n)이 올 때까지 쥐고 있다가, 표시 줄이면 버리고(토큰은 onToken 으로)
// 아니면 그대로 넘긴다. 로비 모듈(WOLAPI.dll)은 막힘 없는 소켓을 초당 약 30번 부르므로(10-10 로그), 토큰 줄만 와서 넘길 것이
// 없으면 진짜 recv 를 한 번 더 불러 그 답(대개 WSAEWOULDBLOCK)을 그대로 돌려준다 — 끊김(0)으로 잘못 알리지 않는다.
// 10-10 반복 시험: 모든 조각 크기 × recv 크기 1~4096 · 무작위 2만 판, 25,412판 실패 0(`Docs/TOKEN_DESIGN.md` 근거).
//
// ★ 10-16 작업 11 시험 — 방 목록 끝(`: 323 `) 바로 앞에 줄 하나를 끼운다(onListEnd 가 있을 때만 — 없으면 예전과 바이트까지 같다).
//   10-15 대조: 정품 클라는 받은 목록에 있는 채널에만 방 참가(JOINGAME)를 보낸다 → Host Game 을 「내 방 들어가기」로 돌리려면
//   내 방 채널이 목록에 있어야 한다. 목록 줄(`: 326 `)과 끝 줄도 토큰 줄처럼 줄 끝까지 쥐었다가 판단한다.
#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

class LobbyLineFilter {
public:
    static constexpr const char* kMark = "NOXTOKEN ";
    static constexpr size_t kMarkLen = 9;
    static constexpr const char* kListItem = ": 326 ";   // 방 목록 한 줄(우리 로비의 꼴 — `Lobby::SendList`)
    static constexpr const char* kListEnd = ": 323 ";    // 방 목록 끝

    // 목록 끝 줄 바로 앞에 끼울 줄(\r\n 포함 · 빈 글자면 안 끼움). 인자 = 그 목록의 `326` 줄들(\r\n 뗀 글자)
    std::function<std::string(const std::vector<std::string>&)> onListEnd;

    // 들어온 바이트를 넘길 것(ready) · 쥘 것(hold) · 토큰 줄로 나눈다. 토큰 줄은 \r\n 을 뗀 글자로 onToken(line)
    template <class OnToken>
    void Feed(const char* p, int n, OnToken&& onToken) {
        std::string data = hold_ + std::string(p, static_cast<size_t>(n));
        hold_.clear();
        size_t i = 0;
        while (i < data.size()) {
            if (lineStart_) {
                const Kind k = KindAt(data, i);
                if (k != Kind::Plain) {   // 표시와 겹친다
                    const size_t eol = data.find("\r\n", i);
                    if (k == Kind::More || eol == std::string::npos) { hold_ = data.substr(i); return; }   // 줄 끝이 덜 왔다 — 쥔다
                    const std::string line = data.substr(i, eol - i);
                    i = eol + 2;
                    switch (k) {
                    case Kind::Token:
                        onToken(line);   // 줄째로 버린다
                        break;
                    case Kind::ListItem:
                        listItems_.push_back(line);
                        ready_ += line + "\r\n";
                        break;
                    case Kind::ListEnd:
                        ready_ += onListEnd(listItems_);
                        listItems_.clear();
                        ready_ += line + "\r\n";
                        break;
                    case Kind::Plain:
                    case Kind::More:
                        break;
                    }
                    continue;   // 다음 줄 머리
                }
            }
            const char c = data[i++];
            ready_.push_back(c);
            lineStart_ = (c == '\n');
        }
    }

    // real(buf, len) = 진짜 recv. 돌려주는 값은 recv 와 같은 뜻(바이트 수 · 0 끊김 · 음수 오류 — 오류 값은 real 이 남긴 그대로)
    template <class RealRecv, class OnToken>
    int Recv(RealRecv&& real, char* buf, int len, OnToken&& onToken) {
        for (;;) {
            if (!ready_.empty()) {
                const int n = static_cast<int>((std::min)(static_cast<size_t>(len), ready_.size()));
                std::memcpy(buf, ready_.data(), static_cast<size_t>(n));
                ready_.erase(0, static_cast<size_t>(n));
                return n;
            }
            char tmp[4096];
            const int want = hold_.empty() ? len : static_cast<int>(sizeof(tmp));   // 토큰 줄을 쥐고 있을 때만 크게 읽는다
            const int r = real(tmp, (std::min)(want, static_cast<int>(sizeof(tmp))));
            if (r == 0) { hold_.clear(); return 0; }   // 진짜로 끊겼다
            if (r < 0) return r;                       // WSAEWOULDBLOCK 등 그대로
            Feed(tmp, r, onToken);
            // ready 가 비면(토큰 줄만 왔거나 쥐고만 있으면) 한 번 더 읽어 본다 → 없으면 real 의 답(WOULDBLOCK)
        }
    }

private:
    enum class Kind { Plain, More, Token, ListItem, ListEnd };

    // 줄 머리 i 가 어느 표시인가 — 표시 전체가 맞으면 그 종류 · 남은 바이트가 표시보다 짧은데 앞이 맞으면 More(더 와야 안다)
    Kind KindAt(const std::string& d, size_t i) const {
        static const char* const marks[] = { kMark, kListItem, kListEnd };
        static const Kind kinds[] = { Kind::Token, Kind::ListItem, Kind::ListEnd };
        const size_t count = onListEnd ? 3 : 1;   // 끼우기가 꺼져 있으면 토큰 줄만 본다(예전 그대로)
        bool more = false;
        for (size_t m = 0; m < count; ++m) {
            const size_t len = std::strlen(marks[m]);
            const size_t cmp = (std::min)(d.size() - i, len);
            if (std::memcmp(d.data() + i, marks[m], cmp) != 0) continue;
            if (cmp == len) return kinds[m];
            more = true;
        }
        return more ? Kind::More : Kind::Plain;
    }

    std::string ready_;
    std::string hold_;
    bool lineStart_ = true;
    std::vector<std::string> listItems_;   // 지금 받는 목록의 `326` 줄들 — 끝 줄에서 비운다
};
