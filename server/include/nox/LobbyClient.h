// LobbyClient.h — 게임 서버의 로비 연결(MO 6단계 v2 작업 4b — `Docs/LOBBY_SPLIT_DESIGN.md` 「누가 무엇을 맡나」 게임 서버 게임 스레드).
//
// 왜 — v2 에서 로비는 따로 된 프로세스(`nox_lobby`)이고 이 게임 서버는 판만 돈다. 방의 사실은 이 게임 서버에만 있으므로
//   로비에 알린다 — 방이 생김 · 바뀜(인원 · 방장 · 판 상태 · 맵) · 닫힘(`ROOM` · `ROOM_GONE`). 새 방은 로비가 만든 사람만 넘겨 묻고(`CREATE`)
//   맵의 주인인 이 게임 서버가 대기 맵 · 대전 맵을 골라 만든 뒤 답한다(옛 `CChannelLobby::CreateRoomFor` 몸통 그대로 — 10-14 작업 10a ·
//   10-16 작업 13a). 판 결과도 연결로 로비에(`RESULT` — 파일 기록기와 같은 JSON 줄 · 7단계에서 로비가 DB 에 쓴다).
//
// 연결 차례(줄 꼴은 `LobbyLink.h`)
//   네트워크 스레드가 로비 연결 포트에 붙으면(`LOBBY_OPENED`) 로비가 일회용 값을 보낸다(`HELLO`) → 같은 설정 키에서 뽑은 연결 키로 서명(`AUTH`) ·
//   등록(`REGISTER <실행 번호> <클라가 붙을 주소> <UDP 포트>`) · 돌고 있는 로비 방 전부(`ROOM`). 그 뒤 틱마다 방 사실을 지난번 보낸 것과
//   견줘 다른 방만 알린다. 끊기면 네트워크 스레드가 다시 붙고 이 차례를 처음부터 한다(로비 거울은 끊김에 이 게임 서버 방을 내렸다)
//   · 로비가 없어도 판은 끝까지 — 진행 중 판 · 입장(토큰은 키로 혼자 확인)은 그대로. 새 방만 못 생긴다. 판 결과는 파일 기록기가 받친다
//
// 입장 토큰 키 — 로비는 이 게임 서버 방의 토큰을 「이 게임 서버의 이 실행」 키로 서명한다(`TokenKeyFor` = `GameServerTokenKey` —
//   등록 줄 값 그대로). 방 관리자에 이 키를 건다(`main`). 그래서 다른 게임 서버 · 다시 켠 이 게임 서버엔 옛 토큰이 안 맞는다
//
// ★ 7단계 — 살아 있음(`PING` · `LobbyLink.h`). 로비 PC 가 전원째 꺼지면 끊김 신호가 안 와 이 게임 서버는 붙은 줄로 안다 —
//   받은 줄이 `kLinkDeadMs` 동안 없으면 네트워크 스레드에 끊으라고 하고(`LOBBY_CLOSE`) 끊김 길(다시 붙기)로 간다
// ★ 7단계 조각 3 — 판 결과는 로비의 `SAVED` 까지 쥔다(아래 `kResultResendMs`). 로비가 꺼진 동안 끝난 판도 다시 붙으면 간다
//   (10-21 결정 — 7단계 설계에서 넣기 · 옛 꼴은 연결이 없으면 셈만 하고 버렸다 · 사람 판 49)
//
// 소켓은 모른다 — 게임 스레드 한 곳에서만 부른다(로비 사건 · 틱 뒤 · 판 결과). 시간은 살아 있음 몫만(`SetClock` · 기본 `NowMs`).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "nox/Clock.h"
#include "nox/LobbyLink.h"
#include "nox/RoomManager.h"
#include "nox/UdpServer.h"
#include "nox/WorldBuild.h"

namespace nox {

// 연결의 셈 — 갈래마다
struct CLobbyClientStats {
    unsigned long long linksUp = 0;          // 로비에 붙음
    unsigned long long linksDown = 0;        // 끊김
    unsigned long long registers = 0;        // HELLO 에 AUTH · REGISTER 를 보냄
    unsigned long long roomLines = 0;        // ROOM 보냄
    unsigned long long roomGoneLines = 0;    // ROOM_GONE 보냄
    unsigned long long creates = 0;          // CREATE 받음
    unsigned long long created = 0;          // 방을 만들어 CREATED
    unsigned long long createFailed = 0;     // 못 만들어 CREATE_FAILED
    unsigned long long results = 0;          // RESULT 처음 보냄(판 하나에 한 번)
    // ★ 7단계 조각 3 — 옛 뜻은 「연결 없음 · 등록 전 · 줄 한도」. 이제 연결 없음 · 등록 전은 쥐었다가 보낸다(`resultsHeldDown`)
    unsigned long long resultsNotSent = 0;   // 줄로 못 지음(줄 한도 · 줄바꿈) — 쥐지 않는다(파일 기록기엔 있다)
    unsigned long long resultsHeldDown = 0;  // ★ 7단계 조각 3 — 연결 없음 · 등록 전에 받아 쥠(붙어 등록하면 보냄)
    unsigned long long resultsResent = 0;    // 다시 보냄(다시 붙어 등록한 직후 · 답 없이 `kResultResendMs`)
    unsigned long long savedOk = 0;          // SAVED ok — 로비가 새로 넣음 · 쥔 것을 지움
    unsigned long long savedDup = 0;         // SAVED dup — 로비에 이미 있음(다시 보낸 것) · 지움
    unsigned long long savedBad = 0;         // SAVED bad — 로비가 못 읽어 버림 · 지움(다시 보내도 같다 — 파일엔 있다)
    unsigned long long savedUnknown = 0;     // 쥐고 있지 않은 판의 SAVED(다시 보낸 것의 늦은 둘째 답 등 — 버림)
    unsigned long long resultsDropped = 0;   // 쥔 것이 `kMaxHeldResults` 를 넘어 가장 옛것을 버림(파일엔 있다)
    unsigned long long byes = 0;             // 로비가 BYE(끊기 직전 까닭)
    unsigned long long badLines = 0;         // 못 읽는 줄 · 차례에 안 맞는 줄(버림)
    // ★ 7단계 — 살아 있음
    unsigned long long pingsIn = 0;          // PING 받음
    unsigned long long pingsOut = 0;         // PING 보냄(등록 뒤 `kLinkPingEveryMs` 마다)
    unsigned long long deadLinks = 0;        // 받은 줄이 `kLinkDeadMs` 동안 없어 끊으라고 함(`LOBBY_CLOSE`)
};

class CLobbyClient {
public:
    // ★ 10-14 작업 10a — 방 이름 「<닉>'s game」에서 닉은 이만큼(바이트)까지 — 이름 칸 15바이트 = 닉 8 + 「's game」 7
    //   (10-16 영문으로 바꿈 — 옛 「의 게임」도 CP949 7바이트라 값은 그대로) · ★ 10-20 옛 `CChannelLobby` 에서 옮김
    static constexpr size_t kRoomNameNickBytes = 8;
    // ★ 10-16 작업 12a — 로비가 만든 방의 최대 인원(`Docs/ROOMS_DESIGN.md` 「10-16 정한 것」) · ★ 10-20 옛 `CChannelLobby` 에서 옮김
    static constexpr int kRoomMaxPlayers = 8;
    // ★ 7단계 조각 3 — 판 결과는 로비가 「DB 에 넣음」(`SAVED`)이라 답할 때까지 쥔다(`Docs/ACCOUNT_DB_DESIGN.md` 원칙 —
    //   소켓에 쓴 것은 받은 것이 아니다: 반쯤 열린 연결에 쓴 줄은 사라진다). 둘 다 근거 없는 값
    static constexpr uint32_t kResultResendMs = 30000;   // 붙어 있어도 이만큼 답이 없으면 다시(로비 쪽 DB 가 잠깐 죽음 — 로비는 답을 안 한다)
    static constexpr size_t kMaxHeldResults = 1000;      // 넘으면 가장 옛것을 버리고 센다(쥐는 곳은 메모리 — 게임 서버가 꺼지면 파일에만 남는다)

    struct Config {
        JoinKey tokenKey{};       // 설정 파일의 토큰 키 — 연결 확인 키 · 이 게임 서버 토큰 키를 이것에서 뽑는다
        uint32_t epoch = 0;       // 실행 번호(= 방 관리자 `Epoch` — 판 번호 앞자리와 같은 값)
        uint32_t registerIp = 0;  // 등록 주소 — 클라가 붙을 이 게임 서버 주소(0 = 클라가 로비에 붙은 로비 쪽 주소 — 같은 PC)
        uint16_t port = 0;        // 이 게임 서버 UDP 포트
    };
    // 로비가 이 게임 서버 방의 토큰에 쓰는 키(등록 줄 값 그대로 — `LobbyLink.h` 「키」)
    static JoinKey TokenKeyFor(const Config& c) { return GameServerTokenKey(c.tokenKey, c.epoch, c.registerIp, c.port); }

    // `rooms` 는 이 객체보다 오래 산다. `roomTemplate` = 새 방의 틀(실행 인자의 방 설정 — 진단 · 시야 손잡이) ·
    //   `waitMaps` · `battleMaps` = 대기 맵 · 대전 맵 후보(맵 파일 경로 — 방 관리자의 맵 목록에 있어야 한다)
    CLobbyClient(CRoomManager& rooms, Config cfg, RoomSettings roomTemplate = {}, std::vector<std::string> waitMaps = {},
                 std::vector<std::string> battleMaps = {});
    void SetSeed(uint32_t seed) { _rng.seed(seed); }   // 맵 고르기 씨앗(시험이 정해 보려고)
    void SetVerbose(bool v) { _verbose = v; }           // 연결 줄을 한 줄씩 찍는가(`--quiet` 반대). 기록(`[로비 연결]`)은 늘 찍는다
    void SetClock(std::function<uint32_t()> nowMs) { _nowMs = std::move(nowMs); }   // ★ 7단계 — 살아 있음의 시계(시험이 정해 보려고)
    // 「<닉>'s game」 · 닉이 길면 닉 쪽을 자른다(옛 `CChannelLobby::RoomNameFor`)
    static std::string RoomNameFor(const std::string& nick);

    // 로비 사건 하나(`IsLobbyEvent`) → 보낼 줄을 `out` 뒤에 요청(`LOBBY_SEND`)으로. 로비 사건이 아니면 아무것도 안 한다
    void OnNetworkEvent(const NetworkEvent& ev, std::vector<NetworkRequest>& out);
    // 틱 뒤 — 방 사실이 지난번 보낸 것과 다른 방만 ROOM · 사라진 방은 ROOM_GONE(등록 뒤에만)
    //   ★ 7단계 — 살아 있음도 여기서(바퀴마다 불린다 — 방이 없어도 한 틱 길이마다 깬다 `main.cpp`)
    void AfterTick(std::vector<NetworkRequest>& out);
    // 판 결과 JSON 한 줄(`FormatMatchResultJson`) → RESULT(등록 뒤에만 · 줄 한도 안에서만 — 아니면 셈만)
    //   ★ 7단계 조각 3 — `match` = 판 번호(`MatchIdOf` — `SAVED` 와 짝). 쥐었다가 등록 뒤에 보내고 `SAVED` 에 지운다 · 줄로 못 지으면 셈만
    void OnMatchResult(const std::string& match, const std::string& json, std::vector<NetworkRequest>& out);
    size_t HeldResults() const { return _held.size(); }   // 아직 `SAVED` 를 못 받은 판 결과

    bool Registered() const { return _stage == Stage::Registered; }
    const CLobbyClientStats& Stats() const { return _stats; }

private:
    enum class Stage : uint8_t { Down, WaitHello, Registered };

    void Send(const std::string& line, std::vector<NetworkRequest>& out);
    void OnLine(const std::string& line, std::vector<NetworkRequest>& out);
    void OnCreate(uint32_t req, const std::string& creator, std::vector<NetworkRequest>& out);
    // 새 방 — 틀 + 만든 사람 · 이름 · 대기 맵(무작위) · 채팅 모드 · 비면 닫음 → 방 관리자의 만들기 입구(옛 `CChannelLobby::CreateRoomFor`)
    int CreateRoomFor(const std::string& creator, std::string& why);
    // 로비 방 = 만든 사람이 있는 방만(실행 인자의 방은 로비 목록에 안 낸다). 채널 · 목록 이름은 지금 방장
    static bool ToLinkRoom(const RoomFact& f, LinkRoom& out);
    // 지난번 보낸 것(`_sent`)과 다른 방만 ROOM · 사라진 방은 ROOM_GONE — 붙을 때 · 끊길 때 `_sent` 를 비워 다시 붙으면 전부 간다
    void SendChanged(std::vector<NetworkRequest>& out);
    // ★ 7단계 — 등록 뒤 PING · 받은 줄이 한도 동안 없으면 `LOBBY_CLOSE`(한 접속에 한 번)
    void LinkTick(std::vector<NetworkRequest>& out);
    // ★ 7단계 조각 3 — 쥔 판 결과 하나를 보낸다(처음이면 `results` · 다시면 `resultsResent`)
    struct HeldResult {
        std::string match;      // 판 번호(`SAVED` 짝)
        std::string line;       // 지어 둔 RESULT 줄
        uint32_t sentMs = 0;    // 마지막으로 보낸 때
        unsigned sends = 0;     // 보낸 수(0 = 아직 — 연결 없음 · 등록 전에 받음)
    };
    void SendHeld(HeldResult& h, uint32_t now, std::vector<NetworkRequest>& out);

    CRoomManager& _rooms;
    Config _cfg;
    JoinKey _authKey;
    RoomSettings _template;
    std::vector<std::string> _waitMaps;
    std::vector<std::string> _battleMaps;
    std::mt19937 _rng;
    bool _verbose = false;
    Stage _stage = Stage::Down;
    int _conn = 0;                            // 로비 연결 접속 번호(네트워크 스레드 `CTcpLines` 것)
    std::map<uint32_t, std::string> _sent;    // 방 번호 → 지난번 보낸 ROOM 줄(바뀜 견주기)
    std::function<uint32_t()> _nowMs = NowMs;
    uint32_t _lastRecvMs = 0;                 // ★ 7단계 — 이 접속에서 마지막으로 줄을 받은 때(붙은 때부터)
    uint32_t _nextPingMs = 0;
    bool _closeAsked = false;                 // 이 접속에 `LOBBY_CLOSE` 를 이미 냄(끊김 사건까지 다시 안 냄)
    std::deque<HeldResult> _held;             // ★ 7단계 조각 3 — `SAVED` 를 못 받은 판 결과(받은 차례 — 한도를 넘으면 앞에서 버림)
    CLobbyClientStats _stats;
};

}  // namespace nox
