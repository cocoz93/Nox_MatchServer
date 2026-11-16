// tcp_lines_test.cpp — TCP 줄 부품(`nox::CTcpLines` — 작업 8a · ★ 10-20 v2 작업 2 바깥으로 붙기) 시험. 루프백 TCP 로 실제로 주고받는다.
//   `common_test.cpp` 와 따로 둔 까닭 — winsock(windows.h)의 `far` 매크로가 그쪽 토큰 시험 본문과 부딪힌다(`net_thread_test.cpp` 와 같은 꼴).
//   결과는 (통과, 한 줄) 목록으로 돌려주고, `common_test.cpp` 의 `TestTcpLines` 가 `Check` 로 센다.
//   ★ 11-17 D-1 — 본문은 `tcp_lines_cases.h`(형 인자 판 — ServerCore 판 `CTcpLinesCore` 도 같은 본문을 `tcp_lines_core_test` 에서 돈다)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "tcp_lines_cases.h"

std::vector<std::pair<bool, std::string>> RunTcpLinesChecks() {
    std::vector<std::pair<bool, std::string>> r;
    g_out = &r;
    TestTcpLines<nox::CTcpLines>();
    TestConnect<nox::CTcpLines, nox::CTcpLines>();   // ★ 10-20 v2 작업 2
    g_out = nullptr;
    return r;
}
