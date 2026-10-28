// lobby_main.cpp — 로비 프로세스 `nox_lobby` (MO 6단계 v2 작업 3c — `Docs/LOBBY_SPLIT_DESIGN.md`).
//
//   nox_lobby.exe --config <설정 파일> [--bind 0.0.0.0] [--lobby-port 4000]
//                 [--link-bind 127.0.0.1] [--link-port 4010] [--seconds N] [--quiet] [--no-db] [--no-auth]
//
// 왜 다시 따로 된 프로세스인가 — 목표 구조 v2(사용자 10-20 「좋아 v2로가자」): 로비 하나가 전체 상태(로그인 · 채팅 채널 · 게임 목록 ·
//   나중 계정 · 친구 · 매칭)를 쥐고, 게임 서버(`nox_server`)는 판만 돌려 늘리고 줄인다. 10-14 작업 8d 가 이 실행 파일을 없애고
//   로비를 채널 안에 넣었던 것을 되돌린다(그때 없앤 까닭 「로비 ↔ 게임 서버 연결이 없어짐」이 이번 대가 — `LobbyLink.h`).
//
// 소켓만 맡는다 — 한 스레드 루프 · TCP 줄 부품(`CTcpLines`) 하나가 듣기 둘을 든다
//   · 클라(4000) 줄은 로비 규약(`wol::Lobby` — 로그인 · 채팅 채널 · 목록 · 방 참가 · 토큰)에
//   · 게임 서버 연결(4010 · 기본 127.0.0.1 — 바깥에 안 엶) 줄은 방 거울(`wol::RoomMirror` — 확인 · 등록 · 방 알림 · 새 방 답 · 판 결과)에
//   · 둘은 같은 접속 번호 공간이라 돌려받은 줄(`wol::Out`)을 그대로 보낸다. 사건이 어느 듣기에서 왔는지는 듣기 포트로 가른다
//   · 새 방 답을 기다리는 동안에도 다른 줄을 그대로 받는다 — 시간 초과는 루프가 100ms 마다 `Lobby::Tick`
//     (★ 7단계 — 게임 서버 살아 있음 `RoomMirror::Tick` 도 같은 자리)
//   · 닫기 신호 둘 — 콘솔(Ctrl+C · 창 닫기)과 이름 있는 이벤트 `nox_lobby_stop_<pid>`(창 없이 띄운 도구가 켠다 — `nox_server_stop_<pid>` 와 같은 꼴).
//     끝 줄(셈)을 찍고 나간다 — 강제 종료로 셈을 잃지 않게
//
// 설정 파일 — `[token] key = <16진 64자>`(입장 토큰 키 — 게임 서버와 같은 값. 연결 확인 키 · 게임 서버별 토큰 키도 이것에서 뽑는다)
//   ★ 7단계 조각 8 — `[db]` host · port · user · password · database(계정 · 판 기록의 정본 MySQL — `Docs/ACCOUNT_DB_DESIGN.md`).
//     없거나 못 붙거나 표 버전이 다르면 뜨지 않는다(토큰 키와 같은 꼴). `--no-db` = 메모리 저장소(시험 · 끝나면 기록이 사라짐 — 정할 것 b)
//   ★ 로그인 암호 조각 4 — `[auth] key`(로그인 서버와 같은 값)가 있으면 로그인은 로그인 토큰(`Docs/LOGIN_AUTH_DESIGN.md`) · `port`(가입 페이지 안내).
//     없거나 `--no-auth` 면 아이디만(시험용 · 정할 것 c — 다른 PC 판 `mo_host`). 받은 줄 기록은 `apgar` 인자를 가린다(`RedactLoginLine`)
// ⚠ --bind 0.0.0.0 은 처음 실행할 때 Windows 방화벽 허용 창이 뜰 수 있다. 같은 PC 시험은 --bind 127.0.0.1 로 충분하다.
//   연결 포트는 기본 127.0.0.1 — 게임 서버가 다른 PC 면 --link-bind 로 내부 주소를 준다(바깥 주소에 열지 말 것)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nox/IniFile.h"
#include "nox/Lobby.h"
#if NOX_HAVE_MYSQL
#include "nox/DbWorker.h"   // ★ 7단계 조각 8 — MySQL 머리 파일은 안 끌어온다(DbWorker.cpp 에만)
#endif
#include "nox/LobbyLink.h"
#include "nox/LobbyStore.h"
#include "nox/RoomMirror.h"
#include "nox/TcpLines.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

bool g_quiet = false;
std::atomic<bool> g_stop{false};

// 콘솔 창 닫기 · Ctrl+C — 루프가 다음 바퀴에 끝 줄을 찍고 나간다(강제 종료로 셈을 잃지 않게)
BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

// 줄마다 시각을 붙인다 — 10-07 사람 확인에서 마지막 「끊음」이 게임 중인지 게임을 닫을 때인지 기록만으로 못 가렸다(옛 lobby_main 그대로)
void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stdout, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

bool ParseIp(const char* s, uint32_t& out) {
    in_addr a{};
    if (inet_pton(AF_INET, s, &a) != 1) return false;
    out = ntohl(a.s_addr);
    return true;
}

// 방 정보 덩어리처럼 글자가 아닌 바이트는 '.' 로 찍는다(콘솔이 깨지지 않게)
std::string Printable(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) > 0x7e) c = '.';
    return r;
}

// 기록 · 연결 줄은 우리가 쓴 한글 문구라 제어 문자만 거른다(콘솔은 UTF-8 — `SetConsoleOutputCP`)
std::string PrintableLog(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) == 0x7f) c = '.';
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    nox::wol::LobbyConfig cfg;
    // 틀의 최대 인원 · 해상도(채널 안 로비 `main.cpp` 와 같은 값 — 게임 서버가 만든 방은 방의 최대 인원이 실린다)
    cfg.room.maxPlayers = 8;
    cfg.room.resolution = 2;   // 녹음 다섯 방 중 셋이 1024x768
    uint32_t bindIp = INADDR_ANY;
    uint16_t lobbyPort = nox::wol::kLobbyPort;
    uint32_t linkBindIp = 0x7f000001;
    uint16_t linkPort = nox::kLobbyLinkPort;
    int seconds = 0;   // 0 이면 끝없이. 자체검사가 정해진 시간 뒤 스스로 끝나게 쓴다
    std::string configPath;
    bool noDb = false;   // ★ 7단계 조각 8
    bool noAuth = false; // ★ 로그인 암호 조각 4

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--quiet") g_quiet = true;
        else if (a == "--bind" && hasNext) {
            if (!ParseIp(argv[++i], bindIp)) { Log("[실패] --bind 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--lobby-port" && hasNext) lobbyPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--link-bind" && hasNext) {
            if (!ParseIp(argv[++i], linkBindIp)) { Log("[실패] --link-bind 주소: %s\n", argv[i]); return 2; }
        } else if (a == "--link-port" && hasNext) linkPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--config" && hasNext) configPath = argv[++i];
        else if (a == "--seconds" && hasNext) seconds = std::atoi(argv[++i]);
        else if (a == "--no-db") noDb = true;
        else if (a == "--no-auth") noAuth = true;
        else { Log("[실패] 모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (lobbyPort == 0 || linkPort == 0 || lobbyPort == linkPort) {
        Log("[멈춤] 포트 — 클라 %u · 연결 %u (0 이거나 같으면 사건을 못 가른다)\n", lobbyPort, linkPort);
        return 2;
    }
    // 로비는 토큰 방식 하나뿐이라 키가 없으면 뜨지 않는다(`Docs/TOKEN_DESIGN.md`)
    if (configPath.empty()) { Log("[멈춤] --config <설정 파일> 이 없다 — [token] key 가 있어야 뜬다\n"); return 2; }
    nox::CIniFile ini;
    if (!ini.LoadFile(configPath)) { Log("[멈춤] 설정 파일을 못 읽었다 — %s\n", ini.Error().c_str()); return 2; }
    if (!nox::ParseJoinKey(ini.Get("token", "key"), cfg.joinKey)) {
        Log("[멈춤] 설정 파일의 [token] key 가 16진 64자가 아니다 — %s\n", configPath.c_str());
        return 2;
    }
    // ★ 로그인 암호 조각 4 — 로그인 토큰 키(있으면 암호 판). 틀린 꼴이면 뜨지 않는다(조용히 아이디만으로 떨어지지 않게)
    if (!noAuth && !ini.Get("auth", "key").empty()) {
        if (!nox::ParseJoinKey(ini.Get("auth", "key"), cfg.loginKey)) {
            Log("[멈춤] 설정 파일의 [auth] key 가 16진 64자가 아니다 — %s\n", configPath.c_str());
            return 2;
        }
        cfg.authOn = true;
        if (!ini.Get("auth", "port").empty()) cfg.authPort = static_cast<uint16_t>(std::atoi(ini.Get("auth", "port").c_str()));
    }
    Log(cfg.authOn ? "[로비] 로그인 = 로그인 토큰(로그인 서버 :%u · 가입 https://<로비 주소>:%u/signup)\n"
                   : "[로비] 로그인 = 아이디만(시험용 — [auth] key 없음 또는 --no-auth)\n", cfg.authPort, cfg.authPort);

    // ★ 7단계 조각 8 — 저장소(계정 · 판 기록). 포트를 열기 전에 — 못 뜨면 포트도 안 잡는다
    nox::wol::CMemoryLobbyStore memStore;
    nox::wol::ILobbyStore* storePtr = &memStore;
#if NOX_HAVE_MYSQL
    std::unique_ptr<nox::db::CDbWorker> dbWorker;
#endif
    if (noDb) {
        Log("[로비] 저장소 = 메모리(--no-db) — 계정 · 판 기록은 로비가 끝나면 사라진다\n");
    } else {
#if NOX_HAVE_MYSQL
        nox::db::DbConfig dc;
        if (!ini.Get("db", "host").empty()) dc.host = ini.Get("db", "host");
        if (!ini.Get("db", "port").empty()) dc.port = static_cast<uint16_t>(std::atoi(ini.Get("db", "port").c_str()));
        dc.user = ini.Get("db", "user");
        dc.password = ini.Get("db", "password");   // 찍지 않는다
        dc.database = ini.Get("db", "database").empty() ? std::string("nox_mo") : ini.Get("db", "database");
        if (dc.user.empty()) {
            Log("[멈춤] 설정 파일에 [db] user 가 없다(%s) — DB 없이 띄우려면 --no-db(계정 · 판 기록이 끝나면 사라짐)\n", configPath.c_str());
            return 2;
        }
        dbWorker = std::make_unique<nox::db::CDbWorker>(dc);
        std::string err;
        if (!dbWorker->Start(err)) {
            Log("[멈춤] DB %s:%u/%s — %s(사람 1회 config/db_setup.sql 을 돌렸나 · DB 없이 띄우려면 --no-db)\n", dc.host.c_str(), dc.port,
                dc.database.c_str(), err.c_str());
            return 2;
        }
        storePtr = dbWorker.get();
        Log("[로비] 저장소 = DB %s:%u/%s(계정 · 판 기록 · 표 버전 %u)\n", dc.host.c_str(), dc.port, dc.database.c_str(), nox::db::kSchemaVersion);
#else
        Log("[멈춤] 이 로비는 MySQL 없이 빌드됐다(CMake NOX_MYSQL_DIR) — --no-db 로만 뜬다\n");
        return 2;
#endif
    }
    nox::wol::ILobbyStore& store = *storePtr;

    nox::CTcpLines tcp;
    {
        std::string err;
        if (!tcp.Listen(bindIp, lobbyPort, err) || !tcp.Listen(linkBindIp, linkPort, err)) {
            Log("[실패] %s\n", err.c_str());
            return 1;
        }
    }

    nox::wol::RoomMirror mirror(cfg.joinKey);
    nox::wol::Lobby lobby(cfg, mirror);
    mirror.Attach(lobby);
    // ★ 7단계 조각 5 — 저장소(계정 · 판 기록 — 위에서 고름: DB 워커 · --no-db 면 메모리 가짜. 답은 둘 다 다음 바퀴 이후)
    mirror.AttachStore(store);   // ★ 로그인 암호 조각 4 — 로비 로그인은 저장소에 안 묻는다(판 저장만)
    std::vector<nox::wol::StoreAnswer> answers;
    wchar_t stopName[64];
    swprintf(stopName, 64, L"nox_lobby_stop_%lu", GetCurrentProcessId());
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName);
    Log("[로비] 듣기 — 클라 %s:%u · 게임 서버 연결 %s:%u · 방은 게임 서버가 알린 것만 · 입장 토큰(설정 %s)\n",
        nox::wol::IpDotted(bindIp).c_str(), lobbyPort, nox::wol::IpDotted(linkBindIp).c_str(), linkPort, configPath.c_str());
    std::fflush(stdout);

    unsigned long long nIn = 0, nOut = 0, nConn = 0, linkIn = 0, linkOut = 0, nLinks = 0;
    const auto start = std::chrono::steady_clock::now();
    std::vector<nox::TcpLineEvent> evs;
    std::vector<nox::wol::Out> outs;
    std::set<int> linkConns;   // 게임 서버 연결의 접속 번호(보낸 줄을 클라 · 연결로 따로 센다)
    const std::string pingLine = nox::LinkPing();

    // 돌려받은 줄을 보내기 버퍼에 — 클라 줄 · 연결 줄이 섞여 있다(번호 공간이 같다)
    auto sendOuts = [&]() {
        mirror.TakeOut(outs);   // 새 방 요청(`RequestRoom`)이 쌓은 CREATE
        for (const auto& o : outs) {
            if (!tcp.Send(o.conn, o.line, o.closeAfter)) continue;
            const bool toLink = linkConns.count(o.conn) != 0;
            if (toLink) ++linkOut;
            else ++nOut;
            // ★ 7단계 — 살아 있음 줄은 5초마다라 기록엔 안 찍고 셈만(끝 줄 PING)
            if (!g_quiet && !(toLink && o.line == pingLine)) Log("[%s c%d] << %s\n", toLink ? "연결" : "로비", o.conn, Printable(o.line).c_str());
        }
        outs.clear();
        for (const auto& l : lobby.TakeLog()) Log("[로비] %s\n", PrintableLog(l).c_str());
        for (const auto& l : mirror.TakeLog()) Log("%s\n", PrintableLog(l).c_str());
        for (const auto& j : mirror.TakeResults()) Log("[판 결과] %s\n", PrintableLog(j).c_str());   // 7단계 전엔 찍고 셈만(DB 는 7단계)
    };

    auto onEvent = [&](const nox::TcpLineEvent& e) {
        const bool link = e.port == linkPort;
        switch (e.type) {
            case nox::TcpLineEvent::Type::OPENED:
                if (link) {
                    ++nLinks;
                    linkConns.insert(e.conn);
                    mirror.OnLinkOpen(e.conn, outs);
                } else {
                    ++nConn;
                    lobby.OnConnect(e.conn, e.peerIp, e.localIp);
                }
                if (!g_quiet)
                    Log("[%s c%d] 접속 %s → 내 쪽 %s\n", link ? "연결" : "로비", e.conn, nox::wol::IpDotted(e.peerIp).c_str(),
                        nox::wol::IpDotted(e.localIp).c_str());
                break;
            case nox::TcpLineEvent::Type::LINE:
                if (link) {
                    ++linkIn;
                    if (!g_quiet && e.line != pingLine) Log("[연결 c%d] >> %s\n", e.conn, PrintableLog(e.line).c_str());
                    mirror.OnLinkLine(e.conn, e.line, outs);
                } else {
                    ++nIn;
                    if (!g_quiet) Log("[로비 c%d] >> %s\n", e.conn, Printable(nox::RedactLoginLine(e.line)).c_str());   // ★ 조각 4 — 암호 · 토큰 가림
                    lobby.OnLine(e.conn, e.line, outs);
                }
                break;
            case nox::TcpLineEvent::Type::CLOSED:
                if (link) {
                    mirror.OnLinkClose(e.conn, outs);   // ★ 10-20 — 기다리던 새 방은 클라에게 거절 답(471)
                    linkConns.erase(e.conn);
                } else {
                    lobby.OnClose(e.conn, outs);
                }
                if (!g_quiet) Log("[%s c%d] 끊음 — %s\n", link ? "연결" : "로비", e.conn, e.why);
                break;
            case nox::TcpLineEvent::Type::REFUSED:
                Log("[로비] 접속이 너무 많아 끊음 (한도 %zu · 포트 %u)\n", nox::CTcpLines::kMaxConns, e.port);
                break;
        }
        sendOuts();
    };

    while (!g_stop) {
        if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) break;
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        // 100ms — 미룬 새 방 답의 시간 초과(`Tick`) · --seconds · 닫기 신호를 보려고 깨어난다
        WaitForSingleObject(static_cast<HANDLE>(tcp.WaitHandle()), 100);
        // ★ 7단계 조각 5 — 저장소 답을 바퀴 맨 앞에서(지난 바퀴 요청의 답 — 메모리 가짜도 「다음 바퀴에 답」이 된다 · 정할 것 b)
        answers.clear();
        store.TakeAnswers(answers);
        for (const nox::wol::StoreAnswer& a : answers) {
            switch (a.kind) {
                case nox::wol::StoreAnswerKind::SaveMatch: mirror.OnStoreAnswer(a, outs); break;
                case nox::wol::StoreAnswerKind::kCount: break;
            }
        }
        if (!answers.empty()) sendOuts();
        evs.clear();
        tcp.Poll(evs);
        for (const auto& e : evs) onEvent(e);
        lobby.Tick(outs);   // ★ 10-20 — 시간 초과는 클라에게 거절 답(471)
        mirror.Tick(outs);  // ★ 7단계 — 게임 서버 살아 있음(PING · 받은 줄이 한도 동안 없으면 BYE 뒤 끊기 → 끊김 사건에 방 내림)
        sendOuts();
        evs.clear();
        tcp.Flush(evs);   // 보내기 오류 · QUIT 답 · BYE 뒤 끊기가 여기서 나온다
        for (const auto& e : evs) onEvent(e);
        std::fflush(stdout);
    }

    tcp.CloseAll();
    if (stopEvent) CloseHandle(stopEvent);
#if NOX_HAVE_MYSQL
    // ★ 7단계 조각 8 — 남은 일(판 저장)을 다 넣고 닫는다 · 셈(요청 하나 = 아래 갈래 중 하나)
    if (dbWorker) {
        dbWorker->Stop();
        const nox::db::DbStats ds = dbWorker->Stats();
        Log("[로비] 끝 — DB 요청 %llu(판 Saved %llu · Duplicate %llu · Invalid %llu · 판 실패 %llu) · "   // ★ 로그인 암호 조각 4 — 로그인 걷음
            "붙음 %llu · 못 붙음 %llu · 끊겨 다시 %llu · 한 번에 꺼낸 일 최대 %zu%s%s\n",
            ds.requests, ds.saved, ds.duplicates, ds.invalid, ds.saveFailed, ds.connects,
            ds.connectFailed, ds.retries, ds.queueMax, ds.lastError.empty() ? "" : " · 마지막 오류 ", ds.lastError.c_str());
    }
#endif
    const nox::wol::LobbyStats& ls = lobby.Stats();
    const nox::wol::MirrorStats& ms = mirror.Stats();
    // ★ 10-20 거절 답 — 참가 거절(471) 합과 까닭 넷(`JoinReject` 차례 — 곧바로 실패 · 게임 서버 실패 · 시간 초과 · 없는 방)
    using nox::wol::JoinReject;
    const auto jr = [&ls](JoinReject w) { return ls.joinRejected[static_cast<size_t>(w)]; };
    Log("[로비] 끝 — 접속 %llu · 받은 줄 %llu · 보낸 줄 %llu · 토큰 %llu · 새 방 %llu(물음 %llu · 실패 %llu · 시간 초과 %llu · 버림 %llu) · 채팅 %llu"
        " · 참가 거절 %llu(곧바로 실패 %llu · 게임 서버 실패 %llu · 시간 초과 %llu · 없는 방 %llu · 로그인 안 됨 %llu)\n",
        nConn, nIn, nOut, ls.tokensIssued, ls.roomsCreated, ls.roomsRequested, ls.roomCreateFailed, ls.roomRequestTimeout,
        ls.roomRequestDropped, ls.chatMessages, ls.JoinRejectedTotal(), jr(JoinReject::RequestFailed), jr(JoinReject::CreateFailed),
        jr(JoinReject::Timeout), jr(JoinReject::NoSuchRoom), jr(JoinReject::NotLoggedIn));
    // ★ 로그인 암호 조각 4 — 로그인(USER 하나 = 아래 갈래 중 하나 · 합 = 로그인) · 토큰 갈래(`LoginTokenCheck` 차례) · 실패 까닭(`LoginFail` 차례)
    {
        std::string tok, fail;
        for (size_t i = 0; i < static_cast<size_t>(nox::LoginTokenCheck::kCount); ++i)
            tok += std::string(i ? " · " : "") + nox::LoginTokenCheckName(static_cast<nox::LoginTokenCheck>(i)) + " " + std::to_string(ls.loginToken[i]);
        for (size_t i = 0; i < static_cast<size_t>(nox::LoginFail::kCount); ++i)
            fail += std::string(i ? " · " : "") + nox::LoginFailName(static_cast<nox::LoginFail>(i)) + " " + std::to_string(ls.loginFail[i]);
        Log("[로비] 끝 — 로그인 %llu(토큰 [%s] · 다시 쓴 토큰 %llu · 실패 표시 [%s] · 모르는 까닭 %llu · 손님 %llu · 평문 %llu · apgar 없음 %llu · "
            "이름 틀림 %llu · 아이디만 %llu) · 옛 접속 끊음 %llu · 로그인된 계정 %zu · 대화방 보기만 %llu · 마감 끊음 %llu · 실패 끊음 %llu\n",
            ls.LoginTotal(), tok.c_str(), ls.loginReplay, fail.c_str(), ls.loginFailUnknown, ls.loginGuest, ls.loginPlain, ls.loginMissing,
            ls.loginBadName, ls.loginNoAuth, ls.loginKickedOld, lobby.AccountsOnline(), ls.chatViewOnly, ls.loginDeadlineClosed,
            ls.loginFailClosed);
    }
    // ★ 7단계 — 보냄 · 받음은 PING 을 넣은 수(괄호가 그중 PING) · 무소식 끊음 = 받은 줄이 한도 동안 없어 끊은 게임 서버 연결
    Log("[로비] 끝 — 게임 서버 연결 %llu(확인 %llu · 확인 실패 %llu · 등록 %llu · 등록 거절 %llu · 차례 틀림 %llu) · 연결 줄 받음 %llu(PING %llu) · "
        "보냄 %llu(PING %llu) · 방 알림 %llu · 사라짐 %llu · 못 읽는 줄 %llu · 판 결과 %llu · 무소식 끊음 %llu\n",
        nLinks, ms.authOk, ms.authFailed, ms.registered, ms.registerRejected, ms.outOfOrder, linkIn, ms.pingsIn, linkOut, ms.pingsOut,
        ms.roomLines, ms.roomGoneLines, ms.badLines, ms.results, ms.deadLinks);
    // ★ 7단계 조각 5 — 판 결과 한 줄 = 저장 요청 · 못 읽음 · 저장소 없음 중 하나 · 요청 하나 = ok · dup · 실패 · 끊긴 뒤 · 아직(끝날 때 남음) 중 하나
    Log("[로비] 끝 — 판 저장 요청 %llu(SAVED ok %llu · dup %llu · 실패 %llu · 끊긴 뒤 답 %llu · 모르는 답 %llu · 끝날 때 남음 %zu) · "
        "못 읽음 %llu(문법 %llu · 빠진 칸 %llu · 타입 %llu · 값 %llu · SAVED bad %llu · 판 번호도 못 읽음 %llu) · 저장소 없음 %llu · "
        "저장소가 못 받음 %llu(SAVED bad)\n",   // ★ 7단계 조각 7 — SAVED bad 합 = 못 읽음 쪽 + 저장소가 못 받음
        ms.saveRequests, ms.savedOk, ms.savedDup, ms.saveFailed, ms.saveConnGone, ms.saveAnswerUnknown, mirror.PendingSaves(),
        ms.UnreadableTotal(), ms.unreadable[static_cast<size_t>(nox::MatchJsonParse::Syntax)],
        ms.unreadable[static_cast<size_t>(nox::MatchJsonParse::Missing)], ms.unreadable[static_cast<size_t>(nox::MatchJsonParse::Type)],
        ms.unreadable[static_cast<size_t>(nox::MatchJsonParse::Value)], ms.savedBad - ms.saveInvalid, ms.unreadableNoMatch, ms.noStore,
        ms.saveInvalid);
    return 0;
}
