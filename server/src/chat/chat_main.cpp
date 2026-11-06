// chat_main.cpp — 귓속말 채팅 서버 실행 파일(`Docs/MO_WHISPER_PLAN_1105.md` 3 · 4절).
//
//   nox_chat.exe --config <설정 파일> [--bind 0.0.0.0] [--port 4020] [--seconds N] [--quiet] [--no-db] [--test-db] [--no-auth]
//
// 클라 DLL 이 로비가 알려 준 주소(`NOXCHAT 주소:포트`)로 둘째 연결을 맺는다. 이 서버는 이름 → 연결 표 하나만 쥐고 귓속말을 나른다 —
//   로비 · 게임 서버와 연결하지 않는다(정지는 같은 `account_bans` 를 5초마다 읽어 맞춘다).
//
//   설정 파일
//     [auth] key = <16진 64자>   로그인 서버 · 로비와 같은 값(로그인 토큰 검증). `--no-auth` 면 안 읽음(시험 — `AUTH 이름` 만으로 받음)
//     [chat] port = <포트>        기본 4020(`--port` 가 앞섬)
//     [db] host · port · user · password · database(test_database)   정지 읽기 — `--no-db` 면 안 읽음(정지 목록 없음)
//   DB 는 읽기만 한다(활성 정지). 기존 `nox_mo` 계정의 SELECT 권한으로 충분하다(플랜 4절 「운영」)
//
// 소켓만 맡는다 — 받은 줄은 `nox::chat::CChatHub` 에 넘기고 돌려받은 줄을 보낸다. 소켓은 `nox::CTcpLines`(라우터 · 로비와 같음)
//
// ⚠ --bind 를 0.0.0.0 으로 두면 처음 실행할 때 Windows 방화벽 허용 창이 뜰 수 있다(`tools/mo_firewall.ps1` 이 4020 규칙을 넣는다)
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "nox/ChatHub.h"
#include "nox/IniFile.h"
#include "nox/JoinToken.h"   // `ParseIpv4` · `ParseJoinKey` · `UnixNow`
#include "nox/LobbyStore.h"
#include "nox/TcpLines.h"
#if NOX_HAVE_MYSQL
#include "nox/DbLobbyStore.h"
#endif

namespace {

bool g_quiet = false;

void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stdout, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

// 클라 줄은 글자가 아닌 바이트만 '.' 로(UTF-8 한글은 그대로 — 콘솔이 UTF-8) · 귓속말 줄은 내용 대신 꼴만 찍는다
std::string Shown(const std::string& s) {
    if (s.rfind("W ", 0) == 0 || s.rfind("F ", 0) == 0 || s.rfind("TO ", 0) == 0) {
        const size_t sp = s.find(' ', s.find(' ') + 1);
        return s.substr(0, sp) + " …(" + std::to_string(nox::chat::Utf8Chars(sp == std::string::npos ? std::string() : s.substr(sp + 1))) + "자)";
    }
    if (s.rfind("AUTH ", 0) == 0) {   // 토큰은 찍지 않는다
        const size_t sp = s.find(' ', 5);
        return sp == std::string::npos ? s : s.substr(0, sp) + " ***";
    }
    std::string r = s;
    for (char& c : r) {
        const uint8_t b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7f) c = '.';
    }
    return r;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr uint64_t kBanPollMs = 5000;   // 로비 「즉시 차단」과 같은 주기(`Lobby.cpp` 정지 읽기)

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    nox::chat::ChatConfig cfg;
    uint32_t bindIp = INADDR_ANY;
    uint16_t port = 0;   // 0 = 설정 → 기본 4020
    int seconds = 0;     // 0 이면 끝없이. 자체검사가 정해진 시간 뒤 스스로 끝나게 쓴다
    std::string configPath;
    bool noDb = false, testDb = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--quiet") g_quiet = true;
        else if (a == "--bind" && hasNext) {
            if (!nox::ParseIpv4(argv[++i], bindIp)) { Log("[실패] --bind 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--port" && hasNext) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--config" && hasNext) configPath = argv[++i];
        else if (a == "--seconds" && hasNext) seconds = std::atoi(argv[++i]);
        else if (a == "--no-db") noDb = true;
        else if (a == "--test-db") testDb = true;
        else if (a == "--no-auth") cfg.noAuth = true;
        else { Log("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (configPath.empty()) { Log("[멈춤] --config <설정 파일> 이 없다 — [auth] key 가 있어야 뜬다(시험은 --no-auth)\n"); return 2; }
    nox::CIniFile ini;
    if (!ini.LoadFile(configPath)) { Log("[멈춤] 설정 파일을 못 읽었다 — %s\n", ini.Error().c_str()); return 2; }
    if (!cfg.noAuth && !nox::ParseJoinKey(ini.Get("auth", "key"), cfg.loginKey)) {
        Log("[멈춤] 설정 파일의 [auth] key 가 16진 64자가 아니다 — %s(시험은 --no-auth)\n", configPath.c_str());
        return 2;
    }
    if (port == 0) port = ini.Get("chat", "port").empty() ? nox::chat::kChatPort : static_cast<uint16_t>(std::atoi(ini.Get("chat", "port").c_str()));
    if (port == 0) { Log("[멈춤] 포트 0 — [chat] port 또는 --port\n"); return 2; }

    // 정지 읽기 — 로비와 같은 저장소(읽기 워커만 씀). 포트를 열기 전에 — 못 뜨면 포트도 안 잡는다
    std::unique_ptr<nox::wol::ILobbyStore> store;
#if NOX_HAVE_MYSQL
    nox::db::CDbLobbyStore* dbStore = nullptr;
#endif
    if (noDb) {
        Log("[채팅] 정지 읽기 없음(--no-db)\n");
    } else {
#if NOX_HAVE_MYSQL
        nox::db::DbConfig dc;
        if (!ini.Get("db", "host").empty()) dc.host = ini.Get("db", "host");
        if (!ini.Get("db", "port").empty()) dc.port = static_cast<uint16_t>(std::atoi(ini.Get("db", "port").c_str()));
        dc.user = ini.Get("db", "user");
        dc.password = ini.Get("db", "password");   // 찍지 않는다
        const std::string dbKey = testDb ? "test_database" : "database";
        dc.database = ini.Get("db", dbKey).empty() ? std::string(testDb ? "nox_mo_test" : "nox_mo") : ini.Get("db", dbKey);
        if (dc.user.empty()) { Log("[멈춤] 설정 파일에 [db] user 가 없다(%s) — 정지 읽기 없이 띄우려면 --no-db\n", configPath.c_str()); return 2; }
        auto s = std::make_unique<nox::db::CDbLobbyStore>(dc);
        std::string err;
        if (!s->Start(err)) { Log("[멈춤] DB %s:%u/%s — %s(정지 읽기 없이 띄우려면 --no-db)\n", dc.host.c_str(), dc.port, dc.database.c_str(), err.c_str()); return 2; }
        dbStore = s.get();
        store = std::move(s);
        Log("[채팅] 정지 읽기 = DB %s:%u/%s(%llu초마다)\n", dc.host.c_str(), dc.port, dc.database.c_str(), kBanPollMs / 1000);
#else
        Log("[멈춤] 이 채팅 서버는 MySQL 없이 빌드됐다 — --no-db 로만 뜬다\n");
        return 2;
#endif
    }
    HANDLE storeWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!storeWake) { Log("[멈춤] 저장소 깨우기 이벤트를 못 만들었다\n"); return 2; }
    if (store) store->SetWake([storeWake] { SetEvent(storeWake); });

    nox::CTcpLines tcp;
    {
        std::string err;
        if (!tcp.Listen(bindIp, port, err)) { Log("[실패] %s\n", err.c_str()); return 1; }
    }
    nox::chat::CChatHub hub(cfg);
    // 닫기 신호 — 창 없이 띄운 도구가 켜는 이름 있는 이벤트(`nox_lobby_stop_<pid>` 와 같은 꼴 · `tools/mo_local.ps1` Stop). 켜지면 끝 줄 셈을 찍고 나간다
    wchar_t stopName[64];
    swprintf(stopName, 64, L"nox_chat_stop_%lu", GetCurrentProcessId());
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName);
    Log("[채팅] 듣기 %s:%u — 로그인 확인 = %s · 귓속말 %zu자 · 5초 %zu줄\n", nox::Ipv4ToString(bindIp).c_str(), tcp.ListenPort(0),
        cfg.noAuth ? "이름만(--no-auth 시험)" : "로그인 토큰", nox::chat::kWhisperMaxChars, nox::chat::kWhisperRateMax);
    std::fflush(stdout);

    unsigned long long nIn = 0, nOut = 0, nConn = 0, banReads = 0, banFails = 0;
    uint32_t banReq = 0;
    bool banAsking = false;
    uint64_t banDue = 0;   // 0 = 곧바로 한 번
    const auto start = std::chrono::steady_clock::now();
    std::vector<nox::TcpLineEvent> evs;
    std::vector<nox::chat::ChatOut> outs;
    std::vector<nox::wol::StoreAnswer> answers;
    auto send = [&](const std::vector<nox::chat::ChatOut>& list) {
        for (const auto& o : list) {
            if (o.line.empty()) {   // 보낼 줄 없이 끊기 — 사건은 다음 바퀴 `Close` 의 CLOSED 로 hub 에
                std::vector<nox::TcpLineEvent> closed;
                tcp.Close(o.conn, closed);
                for (const auto& e : closed) hub.OnClose(e.conn);
                if (!g_quiet) Log("[c%d] 끊음 — 채팅 서버가 정함\n", o.conn);
                continue;
            }
            if (!tcp.Send(o.conn, o.line, o.close)) continue;
            ++nOut;
            if (!g_quiet) Log("[c%d] << %s\n", o.conn, Shown(o.line).c_str());
        }
    };
    auto flushLog = [&] {
        for (const auto& l : hub.TakeLog()) Log("[채팅] %s\n", l.c_str());
    };
    auto onEvent = [&](const nox::TcpLineEvent& e) {
        switch (e.type) {
        case nox::TcpLineEvent::Type::OPENED:
            ++nConn;
            hub.OnConnect(e.conn, e.peerIp, NowMs());
            if (!g_quiet) Log("[c%d] 접속 %s\n", e.conn, nox::Ipv4ToString(e.peerIp).c_str());
            break;
        case nox::TcpLineEvent::Type::LINE:
            ++nIn;
            if (!g_quiet) Log("[c%d] >> %s\n", e.conn, Shown(e.line).c_str());
            outs.clear();
            hub.OnLine(e.conn, e.line, NowMs(), nox::UnixNow(), outs);
            send(outs);
            break;
        case nox::TcpLineEvent::Type::CLOSED:
            hub.OnClose(e.conn);
            if (!g_quiet) Log("[c%d] 끊김 — %s\n", e.conn, e.why);
            break;
        case nox::TcpLineEvent::Type::REFUSED:
            Log("[채팅] 접속이 너무 많아 끊음 (한도 %zu)\n", nox::CTcpLines::kMaxConns);
            break;
        }
    };
    const HANDLE waitFor[] = {static_cast<HANDLE>(tcp.WaitHandle()), storeWake};
    uint64_t lastTick = 0;
    for (;;) {
        if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        WaitForMultipleObjects(2, waitFor, FALSE, 100);
        evs.clear();
        tcp.Poll(evs);
        for (const auto& e : evs) onEvent(e);
        const uint64_t now = NowMs();
        if (now - lastTick >= 1000) {   // PING · 조용한 연결 · 정리 — 1초마다
            lastTick = now;
            outs.clear();
            hub.Tick(now, nox::UnixNow(), outs);
            send(outs);
        }
        if (store) {
            if (!banAsking && now >= banDue) {
                banAsking = true;
                store->GetActiveBans(++banReq, nox::UnixNow());
            }
            answers.clear();
            store->TakeAnswers(answers);
            for (const auto& a : answers) {
                if (a.kind != nox::wol::StoreAnswerKind::ActiveBans || a.req != banReq) continue;
                banAsking = false;
                banDue = now + kBanPollMs;
                if (!a.bansOk) {   // 못 읽음 — 앞 목록 유지(0건과 다르다)
                    ++banFails;
                    Log("[채팅] 정지 목록을 못 읽음 — 앞 목록 유지(%llu번째)\n", banFails);
                    continue;
                }
                ++banReads;
                outs.clear();
                hub.ApplyBans(a.bans, nox::UnixNow(), outs);
                send(outs);
            }
        }
        flushLog();
        evs.clear();
        tcp.Flush(evs);
        for (const auto& e : evs) onEvent(e);
        flushLog();
        std::fflush(stdout);
    }
    tcp.CloseAll();
#if NOX_HAVE_MYSQL
    if (dbStore) dbStore->Stop();
#endif
    CloseHandle(storeWake);
    if (stopEvent) CloseHandle(stopEvent);
    const nox::chat::ChatStats& s = hub.Stats();
    Log("[채팅] 끝 — 접속 %llu · 받은 줄 %llu · 보낸 줄 %llu · 최대 동시 이름 %zu · 정지 읽기 %llu(못 읽음 %llu) · PING %llu · 잘림 %llu · 글자 %llu\n", nConn, nIn, nOut,
        s.maxOnline, banReads, banFails, s.pingsSent, s.truncated, s.sentChars);
    std::string tally;
    for (size_t i = 0; i < static_cast<size_t>(nox::chat::ChatOutcome::kCount); ++i) {
        if (s.outcome[i] == 0) continue;
        tally += std::string(tally.empty() ? "" : " · ") + nox::chat::ChatOutcomeName(static_cast<nox::chat::ChatOutcome>(i)) + " " + std::to_string(s.outcome[i]);
    }
    Log("[채팅] 결과 — %s\n", tally.empty() ? "없음" : tally.c_str());
    // 시험이 읽는 끝 줄(ASCII — 셈 이름은 갈래 번호)
    std::string counts;
    for (size_t i = 0; i < static_cast<size_t>(nox::chat::ChatOutcome::kCount); ++i) counts += (i ? "," : "") + std::to_string(s.outcome[i]);
    Log("CHATSTATS conns=%llu in=%llu out=%llu bans=%llu banfail=%llu outcome=%s\n", nConn, nIn, nOut, banReads, banFails, counts.c_str());
    std::fflush(stdout);
    return 0;
}
