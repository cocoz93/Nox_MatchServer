// tcp_lines_core_test.cpp — TCP 줄 부품 두 판(`nox::CTcpLines` · ServerCore 판 `nox::CTcpLinesCore`)이 같은 시험 본문을 같은 수로 통과하나.
//   ★ 11-17 D-1(`Docs/SERVERCORE_LOBBY_MOVE_PLAN_1116.md` 4-4 · 원칙 2 「옮기는 동안 두 구현이 같은 시험을 통과한다」)
//   본문 = `tcp_lines_cases.h`. ServerCore 판은 프로세스에 하나라(`TcpLinesCore.h` 머리) 판마다 자기 자신을 따로 띄운다.
//     old-lines · old-connect        = 옛 판(common_test 와 같은 본문)
//     core-lines                     = ServerCore 판 혼자
//     core-connect-cli · core-srv    = 붙기 시험의 붙는 쪽 · 듣는 쪽을 ServerCore 판으로(상대는 옛 판)
//   인자 없이 부르면 다섯 판을 띄우고 통과 수를 견준다 — 본문이 같으니 판끼리 확인 수가 같아야 한다(빠진 확인이 없다)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "nox/TcpLinesCore.h"
#include "tcp_lines_cases.h"

namespace {

int RunCase(const std::string& which) {
    std::vector<std::pair<bool, std::string>> r;
    g_out = &r;
    if (which == "old-lines")
        TestTcpLines<nox::CTcpLines>();
    else if (which == "old-connect")
        TestConnect<nox::CTcpLines, nox::CTcpLines>();
    else if (which == "core-lines")
        TestTcpLines<nox::CTcpLinesCore>();
    else if (which == "core-connect-cli")
        TestConnect<nox::CTcpLines, nox::CTcpLinesCore>();
    else if (which == "core-connect-srv")
        TestConnect<nox::CTcpLinesCore, nox::CTcpLines>();
    else {
        std::printf("모르는 판 '%s'\n", which.c_str());
        return 2;
    }
    g_out = nullptr;
    size_t pass = 0;
    for (const auto& c : r) {
        std::printf("%s %s\n", c.first ? "[ok]  " : "[FAIL]", c.second.c_str());
        pass += c.first ? 1 : 0;
    }
    std::printf("== tcp_lines_core_test %s %zu/%zu\n", which.c_str(), pass, r.size());
    return pass == r.size() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // 시험 쪽 막는 소켓용(부품은 제 것을 따로 연다)
    if (argc > 1) {
        const int rc = RunCase(argv[1]);
        std::fflush(stdout);
        WSACleanup();
        return rc;
    }

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    const char* cases[] = {"old-lines", "old-connect", "core-lines", "core-connect-cli", "core-connect-srv"};
    std::map<std::string, std::pair<size_t, size_t>> got;   // 판 → (통과, 전체)
    bool allOk = true;
    for (const char* c : cases) {
        const std::string cmd = std::string("\"\"") + self + "\" " + c + "\"";   // cmd /c 가 바깥 따옴표 한 쌍을 벗긴다
        std::fflush(stdout);
        FILE* p = _popen(cmd.c_str(), "r");
        if (!p) {
            std::printf("[FAIL] 판 %s 를 못 띄움\n", c);
            allOk = false;
            continue;
        }
        char line[1024];
        size_t pass = 0, total = 0;
        bool sawSummary = false;
        const std::string head = std::string("== tcp_lines_core_test ") + c + " ";
        while (std::fgets(line, sizeof(line), p)) {
            std::fputs(line, stdout);
            if (std::strncmp(line, head.c_str(), head.size()) == 0) {   // 「통과/전체」
                char* slash = nullptr;
                const unsigned long long p0 = std::strtoull(line + head.size(), &slash, 10);
                if (slash && *slash == '/') {
                    pass = static_cast<size_t>(p0);
                    total = static_cast<size_t>(std::strtoull(slash + 1, nullptr, 10));
                    sawSummary = true;
                }
            }
        }
        const int rc = _pclose(p);
        got[c] = {pass, total};
        const bool ok = rc == 0 && sawSummary && pass == total && total > 0;
        std::printf("%s 판 %s (종료 값 %d · %zu/%zu)\n", ok ? "[ok]  " : "[FAIL]", c, rc, pass, total);
        allOk = allOk && ok;
    }
    // 본문이 같으니 확인 수가 같아야 한다 — ServerCore 판에서 확인이 빠지거나(판이 일찍 죽음) 더 생기면 여기서 걸린다
    const bool linesSame = got["old-lines"].second == got["core-lines"].second;
    const bool connectSame =
        got["old-connect"].second == got["core-connect-cli"].second && got["old-connect"].second == got["core-connect-srv"].second;
    std::printf("%s 줄 시험 확인 수 같음 — 옛 %zu · ServerCore %zu\n", linesSame ? "[ok]  " : "[FAIL]", got["old-lines"].second,
                got["core-lines"].second);
    std::printf("%s 붙기 시험 확인 수 같음 — 옛 %zu · 붙는 쪽 ServerCore %zu · 듣는 쪽 ServerCore %zu\n", connectSame ? "[ok]  " : "[FAIL]",
                got["old-connect"].second, got["core-connect-cli"].second, got["core-connect-srv"].second);
    allOk = allOk && linesSame && connectSame;
    std::printf("== tcp_lines_core_test %s\n", allOk ? "전부 통과" : "실패 있음");
    WSACleanup();
    return allOk ? 0 : 1;
}
