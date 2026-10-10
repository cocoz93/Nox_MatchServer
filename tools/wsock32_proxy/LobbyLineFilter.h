// LobbyLineFilter.h — 로비 TCP 받기에서 「NOXTOKEN …\r\n」 줄만 떼어 내는 recv 감싸기(소켓 하나의 상태).
//   DLL(wsock32_proxy.cpp)과 반복 시험(linefilter_test.cpp)이 같이 쓴다 — 시험이 본 코드가 곧 DLL 코드다.
//
// TCP 는 줄 경계를 지키지 않는다 — 로비가 한 줄을 한 번에 보내도 클라의 recv 는 `NOXTO` 까지만 받을 수 있다.
// 그래서 줄 머리에서 표시와 겹치는 바이트는 줄 끝(\r\n)이 올 때까지 쥐고 있다가, 표시 줄이면 버리고(토큰은 onToken 으로)
// 아니면 그대로 넘긴다. 로비 모듈(WOLAPI.dll)은 막힘 없는 소켓을 초당 약 30번 부르므로(10-10 로그), 토큰 줄만 와서 넘길 것이
// 없으면 진짜 recv 를 한 번 더 불러 그 답(대개 WSAEWOULDBLOCK)을 그대로 돌려준다 — 끊김(0)으로 잘못 알리지 않는다.
// 10-10 반복 시험: 모든 조각 크기 × recv 크기 1~4096 · 무작위 2만 판, 25,412판 실패 0(`Docs/TOKEN_DESIGN.md` 근거).
#pragma once

#include <algorithm>
#include <cstring>
#include <string>

class LobbyLineFilter {
public:
    static constexpr const char* kMark = "NOXTOKEN ";
    static constexpr size_t kMarkLen = 9;

    // 들어온 바이트를 넘길 것(ready) · 쥘 것(hold) · 토큰 줄로 나눈다. 토큰 줄은 \r\n 을 뗀 글자로 onToken(line)
    template <class OnToken>
    void Feed(const char* p, int n, OnToken&& onToken) {
        std::string data = hold_ + std::string(p, static_cast<size_t>(n));
        hold_.clear();
        size_t i = 0;
        while (i < data.size()) {
            if (lineStart_) {
                const size_t rest = data.size() - i;
                const size_t cmp = (std::min)(rest, kMarkLen);
                if (std::memcmp(data.data() + i, kMark, cmp) == 0) {   // 표시와 겹친다
                    const size_t eol = data.find("\r\n", i);
                    if (eol == std::string::npos) { hold_ = data.substr(i); return; }   // 줄 끝이 덜 왔다 — 쥔다
                    onToken(data.substr(i, eol - i));
                    i = eol + 2;   // 줄째로 버린다
                    continue;
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
    std::string ready_;
    std::string hold_;
    bool lineStart_ = true;
};
