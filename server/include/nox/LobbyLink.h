// LobbyLink.h — 로비 ↔ 게임 서버 연결 규약(MO 6단계 v2 · 작업 1 — `Docs/LOBBY_SPLIT_DESIGN.md` 「로비 연결 규약」).
//
// 왜 — 로비(`nox_lobby` — 로그인 · 채팅 · 게임 목록 · 입장 토큰)와 게임 서버(`nox_server` — 방 · 판)가 다른 프로세스다.
//   방 목록의 정본은 게임 서버의 돌고 있는 방이고, 로비는 게임 서버의 알림(`ROOM` · `ROOM_GONE`)으로 거울을 채운다.
//   새 방은 로비가 요청(`CREATE`)하고, 맵을 고르는 게임 서버가 만든 뒤 답한다(`CREATED`). 판 결과도 이 연결로 로비에(`RESULT`).
//   게임 서버가 로비에 붙는다(로비는 내부 주소에만 듣는다). 줄은 사람이 읽는 글 — 로비 규약과 같은 TCP 줄 부품(`CTcpLines`)으로
//   주고받고 기록에 그대로 찍어 대조한다. 로비(`nox_lobby_core`)와 게임 서버(`nox_proto`)가 같이 쓰므로 `nox_common` 에 둔다.
//
// 줄(방향 · 꼴)
//   로비 → 게임 서버  HELLO <일회용 값 16진 32자>                                    붙자마자
//   게임 서버 → 로비  AUTH <서명 16진 64자>                                          서명 = HMAC(연결 키, 일회용 값 글자)
//   게임 서버 → 로비  REGISTER <실행 번호> <IPv4> <UDP 포트>                          IPv4 0.0.0.0 = 클라가 로비에 붙은 로비 쪽 주소(같은 PC)
//   게임 서버 → 로비  ROOM <방> <인원> <최대> <판 중 0|1> <이름> <만든 사람> <방장> <맵> <모드>   등록 직후 돌고 있는 방 전부 · 그 뒤 바뀔 때마다
//   게임 서버 → 로비  ROOM_GONE <방>
//   로비 → 게임 서버  CREATE <요청 번호> <만든 사람>
//   게임 서버 → 로비  CREATED <요청 번호> <방>  ·  CREATE_FAILED <요청 번호> <까닭>       답 앞에 그 방의 `ROOM` 이 먼저 간다
//   게임 서버 → 로비  RESULT <판 결과 JSON 한 줄>                                     파일 기록기와 같은 줄(`FormatMatchResultJson`)
//   로비 → 게임 서버  BYE <까닭>                                                     ★ 작업 3a-2 — 끊기 직전(확인 실패 · 같은 주소 등록 · 차례 틀림)
// 글 칸(이름 · 맵 · 까닭)은 빈칸 · `%` · `~` · 제어 바이트만 `%XX` 로 바꾸고, 빈 글은 `~` 하나 — 한글(UTF-8)은 그대로 읽힌다.
//
// 키 — 설정 파일의 토큰 키(`[token] key`) 하나에서 이름을 붙여 뽑는다(설정 칸을 늘리지 않는다)
//   연결 키          = HMAC(토큰 키, "nox-link-auth-v1") — 붙은 쪽이 같은 설정을 쥔 게임 서버인지(`AUTH`)
//   게임 서버 토큰 키 = HMAC(토큰 키, "nox-gs-token-v1|<실행 번호>|<IPv4>|<포트>") — `REGISTER` 줄에 적힌 값 그대로(0.0.0.0 도).
//     로비가 클라마다 채워 넣는 실제 주소로 뽑으면 클라마다 키가 달라진다. 이 키로 서명한 입장 토큰은 그 게임 서버의 그 실행에만 맞는다 —
//     토큰 칸엔 게임 서버 칸이 없고(`JoinToken.h` — 51바이트를 DLL 이 박아 둠) 일회용 장부가 게임 서버마다 따로라,
//     키가 하나면 한 토큰을 두 게임 서버에서 한 번씩 쓸 수 있었다
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nox/JoinToken.h"
#include "nox/TcpLines.h"

namespace nox {

constexpr size_t kLinkNonceBytes = 16;
// 줄 한도(CRLF 빼고) — 받는 쪽 `CTcpLines` 는 줄 끝을 못 본 바이트가 `kMaxLine` 을 넘으면 끊는다.
//   줄 끝 CR 과 LF 가 따로 도착하면 CR 까지 쌓이므로 한 바이트를 뺀다
constexpr size_t kLinkMaxLine = CTcpLines::kMaxLine - 1;

// 게임 서버가 알리는 방 하나 — 로비가 그리는 칸만(`RoomFact` 에서 게임 서버가 채운다)
struct LinkRoom {
    uint32_t id = 0;            // 게임 서버의 방 번호(입장 토큰의 방 칸 — 16비트)
    uint32_t players = 0;       // 게임 안 사람 수
    uint32_t maxPlayers = 0;    // 0 = 제한 없음
    bool playing = false;       // 판 중
    std::string name;           // 목록에 보일 방 이름(UTF-8)
    std::string creator;        // 방을 만든 사람의 로비 이름
    std::string host;           // 지금 방장의 로비 이름 — 로비 채널 `#<방장>'s_game`
    std::string map;            // 맵 열쇠(소문자 · 확장자 없음)
    std::string mode;           // 방 설정의 모드(`chat` · `arena` …)
};

enum class LinkKind : uint8_t { Hello, Auth, Register, Room, RoomGone, Create, Created, CreateFailed, Result, Bye, kCount };
const char* LinkKindWord(LinkKind k);   // 줄 앞 낱말(`HELLO` …)

// 읽은 줄 하나 — 갈래에 따라 쓰는 칸만 채운다
struct LinkLine {
    LinkKind kind = LinkKind::kCount;
    std::string text;      // Hello = 일회용 값 16진 · Auth = 서명 16진 · Create = 만든 사람 · CreateFailed · Bye = 까닭 · Result = JSON
    uint32_t epoch = 0;    // Register — 실행 번호(게임 서버가 뜬 유닉스 초)
    uint32_t ip = 0;       // Register — 0 = 로비 쪽 주소
    uint16_t port = 0;     // Register
    uint32_t req = 0;      // Create · Created · CreateFailed
    LinkRoom room;         // Room = 전부 · RoomGone · Created = 번호만
};

// 읽기 실패 까닭 — 갈래마다 따로 센다(시험은 개수로 본다)
enum class LinkParse : uint8_t {
    Ok,
    Empty,          // 빈 줄
    UnknownWord,    // 모르는 앞 낱말
    FieldCount,     // 칸 수가 다름(빈칸 두 개 · 꼬리 빈칸 포함)
    BadNumber,      // 십진 수가 아님 · 범위 밖(방 65535 · 포트 1~65535 · 판 중 0/1)
    BadText,        // 글 칸의 `%XX` 가 틀림 · 날 `~` · 제어 바이트
    BadHex,         // 일회용 값 · 서명의 16진 길이 · 글자
    BadAddress,     // IPv4 꼴이 아님
    kCount
};
const char* LinkParseName(LinkParse p);
LinkParse ParseLinkLine(std::string_view line, LinkLine& out);

// 줄 짓기 — 칸 값은 부르는 쪽이 맞게 준다(방 번호 · 포트 범위). 글 칸은 `EncodeLinkText` 로 바꿔 싣는다
std::string LinkHello(const std::string& nonceHex);
std::string LinkAuth(const std::string& sigHex);
std::string LinkRegister(uint32_t epoch, uint32_t ip, uint16_t port);
std::string LinkRoomLine(const LinkRoom& r);
std::string LinkRoomGone(uint32_t id);
std::string LinkCreate(uint32_t req, const std::string& creator);
std::string LinkCreated(uint32_t req, uint32_t id);
std::string LinkCreateFailed(uint32_t req, const std::string& reason);
std::string LinkBye(const std::string& reason);
// 판 결과 줄 — JSON 에 줄바꿈이 있거나(한 줄이 아님) 비었거나 줄이 `kLinkMaxLine` 을 넘으면 false(안 보낸다 — 파일 기록기엔 남는다)
bool LinkResult(const std::string& json, std::string& line);
inline bool LinkLineFits(const std::string& line) { return line.size() <= kLinkMaxLine; }

// 글 칸
std::string EncodeLinkText(const std::string& s);
bool DecodeLinkText(std::string_view s, std::string& out);

// 키 · 확인 서명
JoinKey LinkAuthKey(const JoinKey& tokenKey);
JoinKey GameServerTokenKey(const JoinKey& tokenKey, uint32_t epoch, uint32_t ip, uint16_t port);
std::string NewLinkNonceHex();   // 난수 16바이트 → 16진 32자(난수 실패면 빈 글)
std::string LinkAuthSig(const JoinKey& authKey, const std::string& nonceHex);   // 16진 64자(실패면 빈 글)
bool CheckLinkAuth(const JoinKey& authKey, const std::string& nonceHex, const std::string& sigHex);   // 같은 시간에 끝나는 비교

}  // namespace nox
