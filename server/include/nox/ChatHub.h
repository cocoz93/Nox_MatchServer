// ChatHub.h — 귓속말 채팅 서버(`nox_chat` · 포트 4020)의 판단. 플랜 `Docs/MO_WHISPER_PLAN_1105.md` 3 · 4절.
//
// 왜 — 귓속말은 받는 사람이 어느 화면(대화방 · 게임 목록 · 어느 게임 서버의 판)에 있든 닿아야 한다. 로비 · 게임 서버를 거쳐 나르면
//   서버끼리 「누가 어디 있나」를 맞춰야 하므로, 클라 DLL 이 이 서버에 둘째 연결을 직접 맺고 이 서버가 이름 → 연결 표 하나만 쥔다.
//   표는 곧 연결이라 다른 서버와 맞출 일이 없고, 로비 · 게임 서버 대수와 무관하다(플랜 3절 「누가 맡고 누가 시키나」).
//
// 소켓도 DB 도 시계도 모른다 — 받은 줄 · 지금 시각을 넣으면 보낼 줄을 돌려준다(`Router` 와 같은 짜임 · 소켓은 `nox_chat` 의 `CTcpLines`).
//   정지 목록은 부르는 쪽이 `account_bans` 를 읽어 `ApplyBans` 로 넣는다(로비 「즉시 차단」과 같은 테이블 · 같은 `CBanList`).
//
// 규약(UTF-8 줄 · 플랜 4절 표)
//   클라 → 서버  `AUTH 이름 토큰16진`(`--no-auth` 면 토큰 없이) · `W 대상 글` · `PING` · `PONG`
//   서버 → 클라  `AUTHOK` · `AUTHNO 까닭`(form · token · replay · banned — 보낸 뒤 끊음 · again — 안 끊음)
//               `F 보낸이 글`(받는 쪽 — 보낸이는 서버가 표에서 찍어 위조 못 함) · `TO 대상 글`(보낸 쪽 — 전달됨)
//               `NOUSER 대상`(접속 중 아님) · `LIMIT`(너무 빠름) · `KICKED`(같은 이름이 새로 붙어 끊음 — DLL 은 다시 안 붙음) · `PING` · `PONG`
//   ★ `TO` 는 플랜 표에 없던 줄 — 플랜 7절 5 · 6 · 8 이 「보낸 꼴에 까닭」 한 줄만 보이게 적었으므로, DLL 이 보낸 줄을 서버 답을 받은 뒤에
//     한 번만 내려면 전달됨을 알 길이 있어야 한다. 그래서 `W` 하나에 답이 `TO` · `NOUSER` · `LIMIT` 중 꼭 하나다(DLL 이 안 보내는 꼴 —
//     자기에게 · 로그인 전 · 틀린 꼴은 답 없음). 잘린 글도 `TO` 에 잘린 그대로 실어 보낸 쪽 화면이 받는 쪽과 같다
//
// 정한 값(플랜 4절) — 보낸 사람당 5초에 3줄(받는 판 화면이 3칸 · 5초) · 글 80자(글자 수 · 바이트 아님 · 글자 중간에서 안 자름) ·
//   이름은 바이트 그대로(`accounts.name` 이 VARBINARY — `QA2` ≠ `qa2`) · 30초 조용하면 `PING` · 90초 아무것도 안 오면 끊음(값은 제 판단) ·
//   내용은 기록하지 않는다(건수 · 길이만 — 저장 여부는 금칙어 플랜에서)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nox/BanList.h"
#include "nox/JoinToken.h"
#include "nox/LoginToken.h"

namespace nox::chat {

constexpr uint16_t kChatPort = 4020;
constexpr size_t kWhisperMaxChars = 80;           // 대화방 입력 칸 80 · 판 입력 칸 250 중 작은 쪽(플랜 2절)
constexpr uint64_t kWhisperRateWindowMs = 5000;   // 받는 판 화면 3칸이 5초에 사라짐(`r1/f7_c1.png`)
constexpr size_t kWhisperRateMax = 3;
constexpr uint64_t kChatPingMs = 30000;
constexpr uint64_t kChatIdleMs = 90000;           // 망이 끊긴 연결이 표에 남아 「접속 중」으로 잘못 답하지 않게

// 줄 하나 · 사건 하나의 결과 — 갈래마다 하나씩 센다(시험은 개수로 본다)
enum class ChatOutcome : uint8_t {
    AuthOk,        // 토큰 맞음 → AUTHOK
    AuthNoAuth,    // `--no-auth` — 토큰 없이 받음(시험)
    AuthBadForm,   // 꼴 틀림 · 이름 규칙 밖 → AUTHNO form
    AuthToken,     // 토큰 거절(서명 · 만료 · IP · 이름 — 갈래는 `ChatStats::tokenCheck`) → AUTHNO token
    AuthReplay,    // 일회용 번호를 이미 씀 → AUTHNO replay
    AuthBanned,    // 정지 중 → AUTHNO banned
    AuthAgain,     // 이미 로그인한 연결이 또 AUTH → AUTHNO again(안 끊음)
    Replaced,      // 같은 이름이 새로 붙어 옛 연결에 KICKED
    Sent,          // W → F(받는 쪽) + TO(보낸 쪽)
    NoUser,        // 대상이 접속 중 아님 → NOUSER
    Limited,       // 5초 3줄 넘음 → LIMIT
    ToSelf,        // 대상이 자기 — DLL 이 먼저 막으므로 답 없음
    NotAuthed,     // 로그인 전 W — 답 없음
    BadLine,       // 모르는 낱말 · 꼴 틀린 W — 답 없음
    PingAnswered,  // 클라 PING → PONG
    PongSeen,      // 클라 PONG
    AfterClose,    // 끊기로 정한 연결에서 온 줄 — 버림
    IdleClosed,    // 90초 조용 → 끊음
    BanClosed,     // 새로 정지된 이름 → 끊음
    kCount
};
const char* ChatOutcomeName(ChatOutcome o);

// 보낼 것 하나 — `line` 이 비면 보내지 않고 끊기만 · `close` 면 이 줄까지 보낸 뒤 끊는다
struct ChatOut {
    int conn = 0;
    std::string line;
    bool close = false;
};

struct ChatConfig {
    JoinKey loginKey{};    // 로비와 같은 `[auth] key`(로그인 서버가 서명한 토큰을 혼자 검증)
    bool noAuth = false;   // 시험 — `AUTH 이름` 만으로 받음(정지는 그대로 봄)
};

struct ChatStats {
    std::array<unsigned long long, static_cast<size_t>(ChatOutcome::kCount)> outcome{};
    std::array<unsigned long long, static_cast<size_t>(LoginTokenCheck::kCount)> tokenCheck{};
    unsigned long long truncated = 0;   // 80자를 넘어 자른 귓속말
    unsigned long long sentChars = 0;   // 전달한 글자 수 합(내용 대신 길이만 남긴다)
    unsigned long long pingsSent = 0;
    size_t maxOnline = 0;
};

// UTF-8 글자 수 — 이어지는 바이트(10xxxxxx)를 안 센다
size_t Utf8Chars(std::string_view s);
// 앞 `maxChars` 글자 — 글자 중간에서 자르지 않는다(이어지는 바이트는 앞 글자에 붙여 둔다)
std::string Utf8Head(std::string_view s, size_t maxChars);

class CChatHub {
public:
    explicit CChatHub(ChatConfig cfg) : cfg_(cfg) {}

    // peerIp = 상대 IPv4(a<<24|b<<16|c<<8|d — 토큰의 IP 와 대조) · nowMs = 단조 시각
    void OnConnect(int conn, uint32_t peerIp, uint64_t nowMs);
    void OnLine(int conn, std::string_view line, uint64_t nowMs, uint32_t unixNow, std::vector<ChatOut>& out);
    void OnClose(int conn);
    // 조용한 연결에 PING · 90초 조용하면 끊기 · 다 쓴 일회용 번호 · 속도 기록 정리. 부르는 쪽이 1초쯤마다
    void Tick(uint64_t nowMs, uint32_t unixNow, std::vector<ChatOut>& out);
    // 읽은 활성 정지 전체로 바꾼다 — 새로 정지된 이름이 붙어 있으면 끊는다. 돌려주는 값 = 끊은 연결 수.
    //   못 읽었으면 부르지 않는다(앞 목록 유지 — 로비와 같은 약속)
    size_t ApplyBans(const std::vector<wol::BanEntry>& rows, uint32_t unixNow, std::vector<ChatOut>& out);

    // 소켓 루프가 찍을 기록(로그인 · 끊음 · 전달 건수 — 귓속말 내용은 안 싣는다). 꺼내면 비운다
    std::vector<std::string> TakeLog();
    const ChatStats& Stats() const { return stats_; }
    size_t Online() const { return byName_.size(); }
    bool IsOnline(const std::string& name) const { return byName_.count(name) != 0; }
    size_t ConnCount() const { return conns_.size(); }

private:
    struct Conn {
        uint32_t peerIp = 0;
        uint64_t lastRecvMs = 0;
        uint64_t lastPingMs = 0;
        std::string name;      // 로그인한 이름(빈 값 = 로그인 전)
        bool closing = false;  // 끊기로 정함 — 소켓이 닫혀 `OnClose` 가 올 때까지 줄을 안 받는다
    };

    void Count(ChatOutcome o) { ++stats_.outcome[static_cast<size_t>(o)]; }
    void OnAuth(int conn, Conn& c, std::string_view args, uint32_t unixNow, std::vector<ChatOut>& out);
    void OnWhisper(int conn, Conn& c, std::string_view args, uint64_t nowMs, std::vector<ChatOut>& out);
    void Refuse(int conn, Conn& c, ChatOutcome why, const char* word, std::vector<ChatOut>& out);
    // 이름을 표에서 빼고 끊는다(`line` 을 보낸 뒤 · 비면 그냥)
    void Drop(int conn, Conn& c, const char* line, std::vector<ChatOut>& out);

    ChatConfig cfg_;
    std::map<int, Conn> conns_;
    std::map<std::string, int> byName_;                // 이름 → 연결(바이트 그대로)
    std::map<std::string, std::deque<uint64_t>> rate_; // 보낸 사람 → 최근 5초의 W 시각(접속을 바꿔도 이어 셈)
    CJoinTokenLedger nonces_;
    wol::CBanList bans_;
    ChatStats stats_;
    std::vector<std::string> log_;
};

}  // namespace nox::chat
