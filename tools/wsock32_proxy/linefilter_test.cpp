// linefilter_test.cpp — LobbyLineFilter.h 반복 시험(DLL 과 같은 코드). test.bat 이 돌린다.
//
// 확인할 것: TCP 가 줄을 어디서 자르든(1바이트 조각까지) 그리고 로비 모듈이 recv 를 몇 바이트로 부르든
//   ① 로비 모듈이 받는 바이트 = 원래 스트림 - 토큰 줄 (순서·내용 그대로)
//   ② 토큰 줄을 정확히 꺼낸다(\r\n 뗀 글자)
//   ③ 스트림이 안 끝났는데 0(=연결 끊김)을 돌려주지 않는다
//   ④ 토큰 줄만 와서 줄 게 없으면 진짜 recv 의 답(WOULDBLOCK)을 그대로
//   ⑤ 줄 가운데의 NOXTOKEN · NOX 로 시작하는 다른 줄은 건드리지 않는다
//   ⑥ ★ 10-16 끼우기를 켜면 — 방 목록 끝(`: 323 `) 바로 앞마다 끼울 줄이 한 번씩 들어가고, 그때 넘긴 목록 줄 수가 맞다.
//      끄면 목록 줄도 그대로 지나간다(①과 같은 기대값). 줄 가운데의 `: 323` · `: 32` 로만 끝나는 줄은 건드리지 않는다
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "LobbyLineFilter.h"

static const int WOULDBLOCK = -2;   // 진짜로는 SOCKET_ERROR + WSAEWOULDBLOCK

// 가짜 네트워크 — 정해진 조각대로 도착한다. 도착분을 다 읽으면 다음 조각이 올 때까지 WOULDBLOCK
struct FakeNet {
    std::string stream;
    std::vector<size_t> cuts;
    size_t arrived = 0, read = 0, nextCut = 0;
    bool closed = false;
    void Tick() { if (nextCut < cuts.size()) arrived = cuts[nextCut++]; else { arrived = stream.size(); closed = true; } }
    int Recv(char* buf, int len) {
        if (read == arrived) return closed ? 0 : WOULDBLOCK;
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(len), arrived - read));
        std::memcpy(buf, stream.data() + read, static_cast<size_t>(n));
        read += static_cast<size_t>(n);
        return n;
    }
};

// 끼우기를 켰을 때 끼울 줄 — 넘겨받은 목록 줄 수를 싣는다(⑥ 줄 수 대조)
static std::string InsertLine(size_t items) { return "INS " + std::to_string(items) + "\r\n"; }

static bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

static std::string Expected(const std::string& s, bool insert) {
    std::string out;
    size_t i = 0, items = 0;
    while (i < s.size()) {
        const size_t eol = s.find("\r\n", i);
        const size_t end = eol == std::string::npos ? s.size() : eol + 2;
        const std::string line = s.substr(i, end - i);
        if (insert && StartsWith(line, LobbyLineFilter::kListItem)) ++items;
        if (insert && StartsWith(line, LobbyLineFilter::kListEnd)) { out += InsertLine(items); items = 0; }
        if (line.compare(0, LobbyLineFilter::kMarkLen, LobbyLineFilter::kMark) != 0) out += line;
        i = end;
    }
    return out;
}

static bool RunOne(const std::string& stream, const std::vector<size_t>& cuts, int len, const std::string& wantLine,
                   bool insert, long long& calls, long long& wouldblocks) {
    FakeNet net;
    net.stream = stream;
    net.cuts = cuts;
    net.Tick();
    LobbyLineFilter f;
    if (insert) f.onListEnd = [](const std::vector<std::string>& items) { return InsertLine(items.size()); };
    std::string got, tokenLine;
    int tokens = 0;
    for (int guard = 0; guard < 1000000; ++guard) {
        char buf[4096];
        const int r = f.Recv([&](char* b, int n) { return net.Recv(b, n); }, buf, len,
                             [&](const std::string& l) { tokenLine = l; ++tokens; });
        ++calls;
        if (r == WOULDBLOCK) { ++wouldblocks; net.Tick(); continue; }   // 로비 모듈은 다음 틀에 다시 부른다
        if (r == 0) {
            if (!net.closed || net.read != stream.size()) { std::printf("③ 끊김 아닌데 0  len=%d\n", len); return false; }
            break;
        }
        if (r > len) { std::printf("받는 크기 초과 r=%d len=%d\n", r, len); return false; }
        got.append(buf, static_cast<size_t>(r));
    }
    if (got != Expected(stream, insert)) { std::printf("① 받은 바이트가 다름 len=%d 끼우기=%d\n", len, insert ? 1 : 0); return false; }
    if (tokens != 1 || tokenLine != wantLine) { std::printf("② 토큰 줄 %d개 [%s]\n", tokens, tokenLine.c_str()); return false; }
    return true;
}

int main() {
    const std::string tokLine = "NOXTOKEN 127.0.0.1:18590 0100007f00000184" + std::string(86, 'a');
    const std::string stream =
        ": 328 u wn9457`949\r\n"
        "NOX\r\n"                                                  // ⑤ 표시의 앞부분만 같은 다른 줄
        ": 999 u NOXTOKEN 가운데 낀 것은 그대로\r\n"                // ⑤ 줄 가운데
        + tokLine + "\r\n"                                         // 떼어 낼 줄
        ":wn9457!u@h JOINGAME 1 32 37 1 0 2130706433 1 :#NoxOrigin's_game\r\n"
        ": 332 u #NoxOrigin's_game :G1P3....\r\n"
        ": 353 u = #NoxOrigin's_game :@NoxOrigin,0,2130706433 wn9457,0,2130706433\r\n"
        ": 366 u #NoxOrigin's_game :\r\n"
        // ⑥ 방 목록 둘 — 줄 둘인 목록 · 빈 목록. 사이에 표시와 앞만 같은 줄(`: 32`) · 줄 가운데 `: 323`
        ": 321 u:\r\n"
        ": 326 u #seed's_game 1 0 37 1 41681088 2130706433 128::G1P3abc\r\n"
        ": 32\r\n"
        ": 999 u : 323 가운데 낀 것은 그대로\r\n"
        ": 326 u #b's_game 2 0 37 1 41681088 2130706433 128::G1P3def\r\n"
        ": 323 u:\r\n"
        ": 321 u:\r\n"
        ": 323 u:\r\n";
    const int lens[] = { 1, 2, 3, 5, 8, 9, 10, 16, 64, 256, 512, 4096 };
    long long runs = 0, calls = 0, wb = 0;
    int fails = 0;
    // 기대값이 정말 끼움을 담는가(시험이 아무것도 안 보는 것을 막는다) — 끼울 줄 둘(줄 2 · 줄 0)
    const std::string withIns = Expected(stream, true);
    if (withIns.find(InsertLine(2) + ": 323 u:") == std::string::npos || withIns.find(InsertLine(0) + ": 323 u:") == std::string::npos ||
        Expected(stream, false).find("INS ") != std::string::npos) {
        std::printf("기대값이 끼움을 안 담음\n");
        ++fails;
    }

    for (const bool insert : { false, true }) {
        for (size_t c = 1; c <= stream.size(); ++c) {   // 모든 고정 조각 크기 × 모든 recv 크기
            std::vector<size_t> cuts;
            for (size_t p = c; p < stream.size(); p += c) cuts.push_back(p);
            for (int len : lens) { ++runs; if (!RunOne(stream, cuts, len, tokLine, insert, calls, wb)) ++fails; }
        }
        const size_t tpos = stream.find(tokLine);   // 토큰 줄 안의 모든 한 자리 자르기(두 조각)
        for (size_t p = tpos; p <= tpos + tokLine.size() + 2; ++p)
            for (int len : lens) { ++runs; if (!RunOne(stream, { p }, len, tokLine, insert, calls, wb)) ++fails; }
        const size_t lpos = stream.find(": 321 u:");   // 목록 둘 안의 모든 한 자리 자르기(두 조각)
        for (size_t p = lpos; p <= stream.size(); ++p)
            for (int len : lens) { ++runs; if (!RunOne(stream, { p }, len, tokLine, insert, calls, wb)) ++fails; }
        std::mt19937 rng(insert ? 20261016 : 20261010);   // 무작위 자르기 2만 판
        for (int k = 0; k < 20000; ++k) {
            std::vector<size_t> cuts;
            for (size_t p = 1; p < stream.size(); ++p) if (rng() % 7 == 0) cuts.push_back(p);
            const int len = lens[rng() % (sizeof(lens) / sizeof(lens[0]))];
            ++runs;
            if (!RunOne(stream, cuts, len, tokLine, insert, calls, wb)) ++fails;
        }
    }
    std::printf("linefilter runs=%lld recv-calls=%lld wouldblock=%lld fail=%d\n", runs, calls, wb, fails);
    std::printf("%s\n", fails ? "LINEFILTER FAIL" : "LINEFILTER PASS");
    return fails ? 1 : 0;
}
