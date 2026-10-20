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
// 소켓도 시간도 모른다 — 게임 스레드 한 곳에서만 부른다(로비 사건 · 틱 뒤 · 판 결과).
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

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
    unsigned long long results = 0;          // RESULT 보냄
    unsigned long long resultsNotSent = 0;   // 판 결과를 못 보냄(연결 없음 · 등록 전 · 줄 한도) — 파일 기록기엔 있다
    unsigned long long byes = 0;             // 로비가 BYE(끊기 직전 까닭)
    unsigned long long badLines = 0;         // 못 읽는 줄 · 차례에 안 맞는 줄(버림)
};

class CLobbyClient {
public:
    // ★ 10-14 작업 10a — 방 이름 「<닉>'s game」에서 닉은 이만큼(바이트)까지 — 이름 칸 15바이트 = 닉 8 + 「's game」 7
    //   (10-16 영문으로 바꿈 — 옛 「의 게임」도 CP949 7바이트라 값은 그대로) · ★ 10-20 옛 `CChannelLobby` 에서 옮김
    static constexpr size_t kRoomNameNickBytes = 8;
    // ★ 10-16 작업 12a — 로비가 만든 방의 최대 인원(`Docs/ROOMS_DESIGN.md` 「10-16 정한 것」) · ★ 10-20 옛 `CChannelLobby` 에서 옮김
    static constexpr int kRoomMaxPlayers = 8;

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
    // 「<닉>'s game」 · 닉이 길면 닉 쪽을 자른다(옛 `CChannelLobby::RoomNameFor`)
    static std::string RoomNameFor(const std::string& nick);

    // 로비 사건 하나(`IsLobbyEvent`) → 보낼 줄을 `out` 뒤에 요청(`LOBBY_SEND`)으로. 로비 사건이 아니면 아무것도 안 한다
    void OnNetworkEvent(const NetworkEvent& ev, std::vector<NetworkRequest>& out);
    // 틱 뒤 — 방 사실이 지난번 보낸 것과 다른 방만 ROOM · 사라진 방은 ROOM_GONE(등록 뒤에만)
    void AfterTick(std::vector<NetworkRequest>& out);
    // 판 결과 JSON 한 줄(`FormatMatchResultJson`) → RESULT(등록 뒤에만 · 줄 한도 안에서만 — 아니면 셈만)
    void OnMatchResult(const std::string& json, std::vector<NetworkRequest>& out);

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
    CLobbyClientStats _stats;
};

}  // namespace nox
