// lobby_test.cpp — 로비 서버(MO 2단계) 시험. Docs/LOBBY_DESIGN.md 「시험」 표.
//
//   lobby_test                       늘 도는 단위 시험 (녹음 없이)
//   lobby_test --gameinfo <녹음>     녹음 속 실제 방 정보 덩어리를 풀었다 다시 포장해 바이트 대조
//   lobby_test --replay <녹음>       녹음의 정품 클라 줄을 로비에 넣고, 나온 답을 XWIS 원래 답과 접속 단위로 대조
//
// 녹음 = 1단계 중계기 기록(`_evidence/_wol_record_20261006/relay_log_*.txt`).
//   중계기가 XWIS 줄을 Latin-1 로 읽어 UTF-8 로 적었으므로, 읽을 때 UTF-8 → 바이트로 되돌린다.
//   `<<` 줄이 XWIS 원래 줄이고 `<~` 줄은 중계기가 바꿔 끼운 줄이라 정답지에서 뺀다.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "nox/Lobby.h"
#include "nox/LobbyLink.h"
#include "nox/LobbyStore.h"
#include "nox/RoomMirror.h"
#include "nox/Router.h"
#include "nox/WolGameInfo.h"

namespace {

int g_checks = 0;
int g_fails = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  [실패] %s\n", what);
    }
}
void Check(bool ok, const std::string& what) { Check(ok, what.c_str()); }   // ★ 로그인 암호 조각 4 — 개수를 글에 싣는 확인

// ★ 10-20 거절 답 — `out` 이 그 접속에 거절 한 줄뿐인가(숫자 471 · 채널 칸 · 까닭 글까지). 471 은 상수 대신 글자로 — 정품이 받아 주는 숫자가 정답지다
bool OnlyReject(const std::vector<nox::wol::Out>& out, int conn, const std::string& channel, nox::wol::JoinReject why) {
    return out.size() == 1 && out[0].conn == conn && !out[0].closeAfter &&
           out[0].line == ": 471 u " + channel + " :" + nox::wol::JoinRejectText(why);
}
unsigned long long Rejected(const nox::wol::LobbyStats& s, nox::wol::JoinReject why) { return s.joinRejected[static_cast<size_t>(why)]; }

std::string FromHex(const char* hex) {
    std::string out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        auto nib = [](char c) { return (c <= '9') ? c - '0' : (c | 0x20) - 'a' + 10; };
        out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

bool SameInfo(const nox::wol::GameInfo& a, const nox::wol::GameInfo& b) {
    return a.access == b.access && a.disallow == b.disallow && a.unk1 == b.unk1 &&
           a.resolution == b.resolution && a.limitRes == b.limitRes && a.players == b.players &&
           a.maxPlayers == b.maxPlayers && a.minPing == b.minPing && a.maxPing == b.maxPing &&
           a.unk2 == b.unk2 && a.map == b.map && a.name == b.name && a.unk3 == b.unk3 &&
           a.flags == b.flags && a.fragLimit == b.fragLimit && a.timeLimit == b.timeLimit;
}

// ── 늘 도는 단위 시험 ───────────────────────────────────────────────
void UnitTests() {
    using namespace nox::wol;

    // 1) 정답값 — opennox 알고리즘(crypt.go·gameinfo.go)을 그대로 옮긴 참조 구현으로 만든 값.
    //    그 참조 구현은 녹음 덩어리 여섯을 다시 포장해 바이트까지 같음을 먼저 확인했다(10-07).
    GameInfo g;
    g.resolution = 2;
    g.maxPlayers = 31;
    g.map = "estate";
    g.name = "Nox_Origin";
    g.fragLimit = 15;
    const auto raw = Marshal(g);
    const std::string wantRaw = FromHex(
        "00ff02001fffffffff9e486573746174650000004e6f785f4f726967696e0000000000"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffff07210f000000");
    Check(std::string(raw.begin(), raw.end()) == wantRaw, "날것 69바이트가 정답값과 같다");
    const std::string wantPack = std::string(kHeader) + FromHex(
        "80fe8b80f0e3fffffffffbc4d4ec9cbae1e89583808080a7eff0fdfa"
        "a4aedab3e9dc818080808080ffffffffffffffffffffffffffffffff"
        "ffffffffffffffffffffffffffffffff87c2bc808080");
    Check(Pack(g) == wantPack, "포장 86바이트가 정답값과 같다");

    // 2) 기본값이 아닌 칸을 다 채워 왕복
    GameInfo h;
    h.access = 2;
    h.disallow = 5;
    h.unk1 = 0x12;
    h.resolution = 3;
    h.limitRes = true;
    h.players = 3;
    h.maxPlayers = 16;
    h.minPing = 100;
    h.maxPing = 300;
    h.unk2 = 0x1234;
    h.map = "abcdefghi";            // 9바이트 꽉
    h.name = "ABCDEFGHIJKLMNO";     // 15바이트 꽉
    for (size_t i = 0; i < h.unk3.size(); ++i) h.unk3[i] = static_cast<uint8_t>(i * 37 + 1);
    h.flags = 0x2007 | kMapCtf;
    h.fragLimit = 20;
    h.timeLimit = 600;
    const std::string hp = Pack(h);
    GameInfo back;
    Check(Unpack(hp, back), "꽉 채운 방 정보를 풀 수 있다");
    Check(SameInfo(h, back), "꽉 채운 방 정보가 왕복 후 같다");

    // 3) 포장 조각에 줄을 깨뜨리는 바이트(0x00·0x0A·0x0D)가 없다 — 머리 뒤는 전부 0x80 이상
    bool allHigh = hp.size() == kHeader.size() + kPackedSize;
    for (size_t i = kHeader.size(); i < hp.size(); ++i)
        if ((static_cast<uint8_t>(hp[i]) & 0x80) == 0) allHigh = false;
    Check(allHigh, "포장 조각 78개가 모두 최상위 비트 1");

    // 4) 잘못된 덩어리는 거절
    std::string bad = wantPack;
    bad[1] = 'X';
    Check(!Unpack(bad, back), "머리가 다르면 거절");
    Check(!Unpack(std::string_view(wantPack).substr(0, wantPack.size() - 1), back), "한 바이트 모자라면 거절");
    bad = wantPack;
    bad[20] = static_cast<char>(static_cast<uint8_t>(bad[20]) & 0x7f);
    Check(!Unpack(bad, back), "최상위 비트가 빠진 조각이 있으면 거절");

    // 5) 알려진 한계 — 넘치는 글자는 잘리고, 시간 제한 칸 상위 6비트는 실리지 않는다
    GameInfo t;
    t.map = "123456789AB";
    t.timeLimit = 0xffff;
    Check(Unpack(Pack(t), back) && back.map == "123456789", "맵 이름은 9바이트에서 잘린다");
    Check(back.timeLimit == 0x03ff, "시간 제한은 아래 10비트만 실린다");
}

std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> t;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(' ', p);
        if (q == std::string::npos) {
            t.push_back(s.substr(p));
            return t;
        }
        t.push_back(s.substr(p, q - p));
        p = q + 1;
    }
}

std::vector<std::string> Lines(const std::vector<nox::wol::Out>& out) {
    std::vector<std::string> r;
    for (const auto& o : out) r.push_back(o.line);
    return r;
}

// ── 로비 단위 시험 — 값이 설정대로 실리는지 (꼴은 --replay 가 녹음과 대조한다) ──
//   ★ 10-10 토큰 작업 3 — 방 둘 · 클라 DLL 알림(`NOXCLIENT`) · 방 참가 앞의 `NOXTOKEN`(게임 서버 키로 풀리나) · 거절 갈래
nox::JoinKey LobbyTestKey() {
    nox::JoinKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(0x30 + i);
    return k;
}
constexpr uint32_t kLobbyTestNow = 1760064016;   // 시험 시계(유닉스 초)

// ★ 10-14 작업 8d — 로비는 방 목록을 갖지 않고 채널이 준다(`ILobbyRooms`). 시험은 가짜 — 옛 설정 `[rooms]` 의 「방장 이름 = 게임 방 번호」 자리
nox::wol::LobbyRoom FakeRoom(uint16_t id, const char* host) {
    nox::wol::LobbyRoom r;
    r.gameRoomId = id;
    r.host = host;   // ★ 10-16 결함 ② — 옛 `creator`(채널 · 353 방장 자리는 지금 방장)
    return r;
}
struct FakeRooms final : nox::wol::ILobbyRooms {
    std::vector<nox::wol::LobbyRoom> rooms;
    bool canCreate = true;   // ★ 10-14 작업 10a — 새 방 만들기가 되나(채널이 대기 맵 없음 · 세계 차리기 실패면 false)
    bool ask = false;        // ★ 10-20 v2 작업 3a — 게임 서버에 묻는 꼴(`Pending`)로 답하나(거울 흉내 — 답은 시험이 `OnRoomCreated` 로)
    int created = 0;
    std::vector<uint32_t> asked;   // 물은 요청 번호
    std::vector<nox::wol::RoomAsk> asks;   // ★ 랭크 매칭 조각 4 — 받은 요청 꼴(갈래와 무관하게 전부 · 명단 · ranked)
    std::vector<uint32_t> cancelled;       // ★ 랭크 매칭 조각 4 — 로비가 접은 요청
    std::vector<nox::wol::LobbyRoom> LobbyRooms() const override { return rooms; }
    nox::wol::RoomRequest RequestRoom(uint32_t req, const nox::wol::RoomAsk& a, nox::wol::LobbyRoom& out, std::string& why) override {
        asks.push_back(a);
        if (ask) {
            asked.push_back(req);
            return nox::wol::RoomRequest::Pending;
        }
        if (!canCreate) {
            why = "가짜 — 못 만듦";
            return nox::wol::RoomRequest::Failed;
        }
        out = Make(a.creator);
        rooms.push_back(out);
        return nox::wol::RoomRequest::Created;
    }
    void CancelRoom(uint32_t req) override { cancelled.push_back(req); }
    nox::wol::LobbyRoom Make(const std::string& creator) {
        ++created;
        nox::wol::LobbyRoom r = FakeRoom(static_cast<uint16_t>(100 + created), creator.c_str());
        r.name = creator + "의 게임";   // UTF-8(이 파일은 /utf-8) — 한글 이름의 코드페이지 바꾸기를 보려고(채널의 `RoomNameFor` 는 10-16 부터 영문)
        r.map = "so_lod";
        r.mode = "chat";
        return r;
    }
};

void LobbyUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.gameServerIp = (192u << 24) | (0u << 16) | (2u << 8) | 10u;   // 192.0.2.10 (문서용 주소)
    cfg.room.map = "estate";
    cfg.room.maxPlayers = 31;
    FakeRooms fake;
    fake.rooms = {FakeRoom(0, "NoxOrigin"), FakeRoom(1, "NoxOrigin2")};
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    Lobby lb(cfg, fake);
    const uint32_t loop = 0x7f000001;
    const uint32_t peer = (198u << 24) | (51u << 16) | (100u << 8) | 7u;   // 198.51.100.7 (문서용 주소)
    const std::string gameIp = std::to_string(cfg.gameServerIp);
    std::vector<Out> out;

    // 4000 — DLL 알림 + 로그인 묶음 → 376, verchk → 379
    //   (★ 10-14 작업 8b — 4005 로비 찾기는 라우터로 — `RouterUnitTests`)
    lb.OnConnect(2, peer, loop);
    for (const char* l : {"NOXCLIENT 1", "CVERS 11020 9472", "PASS supersecret", "NICK wn", "apgar 1234 0", "SERIAL 7726",
                          "USER UserName HostName irc.westwood.com :RealName", "verchk 32512 720916", "SETOPT 17,33"})
        lb.OnLine(2, l, out);
    Check(out.size() >= 2 && out[out.size() - 2].line == ": 376 u" &&
              out.back().line == ": 379 u :none none none 1 32512 NONREQ",
          "DLL 알림 + 로그인 → 로그인 끝(376) 뒤 패치 없음(379)");

    // 첫 목록(-1) = 방 2 + 채팅 로비 3, 방 줄에 게임 서버 IP · 방 정보는 풀리고 이름이 방장 이름
    out.clear();
    lb.OnLine(2, "LIST -1 37", out);
    int n326 = 0, n327 = 0;
    std::vector<std::string> names;
    for (const auto& o : out) {
        const auto t = Tokens(o.line);
        if (t.size() > 1 && t[1] == "326") {
            ++n326;
            Check(t.size() == 11 && t[9] == gameIp, "326 의 방장 주소 칸 = 게임 서버 IP");
            GameInfo g;
            Check(t.size() == 11 && t[10].rfind("128:", 0) == 0 && Unpack(t[10].substr(4), g) && g.map == "estate",
                  "326 의 방 정보가 풀리고 맵이 설정대로");
            if (t.size() == 11 && Unpack(t[10].substr(4), g)) names.push_back(t[3] + "=" + g.name);   // `: 326 u #채널 …` — 채널은 넷째 칸
        }
        if (t.size() > 1 && t[1] == "327") ++n327;
    }
    Check(n326 == 2 && n327 == 5, "LIST -1 37 = 방 2줄 + 채팅 로비 5줄(★ 10-20 v2 작업 3b — XWIS 셋 + 갈라바 · 그록 토르)");
    std::string seen;
    for (const auto& n : names) seen += " [" + n + "]";
    const std::string namesMsg = "방 차례 = 설정 차례 · 방 정보 이름 = 방장 이름 —" + seen;
    Check(names.size() == 2 && names[0] == "#NoxOrigin's_game=NoxOrigin" && names[1] == "#NoxOrigin2's_game=NoxOrigin2",
          namesMsg.c_str());
    out.clear();
    lb.OnLine(2, "LIST 37 37", out);
    n327 = 0;
    for (const auto& o : out)
        if (Tokens(o.line).size() > 1 && Tokens(o.line)[1] == "327") ++n327;
    Check(n327 == 0 && out.size() == 4, "LIST 37 37 = 방 2줄만");

    // 방 참가(둘째 방) — 답 맨 앞에 NOXTOKEN(게임 서버 주소 · 키로 풀리는 토큰), 353 에 그 방의 방장과 나
    out.clear();
    lb.OnLine(2, "JOINGAME #NoxOrigin2's_game 1 ", out);
    const auto first = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    nox::JoinToken tok;
    nox::JoinTokenCheck chk = nox::JoinTokenCheck::BadSize;
    uint8_t raw[nox::kJoinTokenSize] = {};
    if (first.size() == 3 && nox::ParseHex(first[2], raw, sizeof(raw)))
        chk = nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok);
    Check(first.size() == 3 && first[0] == "NOXTOKEN" && first[1] == "192.0.2.10:18590", "참가 답 맨 앞 = NOXTOKEN 게임 서버 주소");
    Check(chk == nox::JoinTokenCheck::Ok && tok.roomId == 1 && tok.clientIp == peer && tok.account == "wn" &&
              tok.expiresAt == kLobbyTestNow + nox::kJoinTokenLifetimeSec && tok.nonce == 0x0102030405060708ull,
          "토큰이 게임 서버 키로 풀리고 방 1 · 내 IP · 로비 이름 · 만료 30초");
    const std::string want353 = ": 353 u = #NoxOrigin2's_game :@NoxOrigin2,0," + gameIp + " wn,0," + std::to_string(peer);
    bool has353 = false;
    for (const auto& o : out) has353 = has353 || o.line == want353;
    Check(out.size() == 5 && Tokens(out[1].line).size() > 1 && Tokens(out[1].line)[1] == "JOINGAME" && has353,
          "NOXTOKEN 뒤 원래 답 넷(JOINGAME · 332 · 353 · 366), 353 = 둘째 방 방장 + 나");
    out.clear();
    lb.OnLine(2, "JOINGAME #nobody's_game 1 ", out);
    Check(OnlyReject(out, 2, "#nobody's_game", JoinReject::NoSuchRoom) && Rejected(lb.Stats(), JoinReject::NoSuchRoom) == 1,
          "모르는 방 참가 → 거절 471 한 줄(토큰 없음 · ★ 10-20 — 그 전엔 답 없음)");
    {   // 까닭 글은 까닭마다 다르다(시험이 글을 같은 함수로 견주므로 글끼리 뒤바뀌면 위 확인으론 안 보인다) · 줄을 깨는 글자 없음
        std::set<std::string> texts;
        bool clean = true;
        for (size_t i = 0; i < kJoinRejectCount; ++i) {
            const std::string s = JoinRejectText(static_cast<JoinReject>(i));
            texts.insert(s);
            clean = clean && !s.empty() && s != "?" && s.find_first_of("\r\n") == std::string::npos;
        }
        Check(texts.size() == kJoinRejectCount && clean, "거절 까닭 글 넷이 서로 다르고 빈 글 · 줄바꿈 없음");
    }
    Check(lb.Stats().tokensIssued == 1, "토큰 낸 수 1");

    // 모르는 명령 — 답 없음, 처음 한 번만 기록
    out.clear();
    lb.TakeLog();
    lb.OnLine(2, "PING abc", out);
    lb.OnLine(2, "PING def", out);
    Check(out.empty() && lb.TakeLog().size() == 1, "모르는 명령은 답 없이 처음 한 번만 기록");

    // DLL 없는 로그인 · 판 낮음 · 로그인 전 참가 — 갈래마다 센다
    //   ★ 10-11 토큰 작업 5 — 끊지 않는다. 로그인은 받고 인사말(372)에 패치 안내 · 방 참가엔 토큰 없이 원래 답(게임 서버가 거절)
    //   (10-10 처음엔 `ERROR` 로 끊었는데 정품 화면이 아무 말 없이 「Connecting…」에서 멈췄다)
    auto hasLine = [](const std::vector<Out>& o, const std::string& prefix) {
        for (const auto& x : o) if (x.line.rfind(prefix, 0) == 0) return true;
        return false;
    };
    auto anyClose = [](const std::vector<Out>& o) {
        for (const auto& x : o) if (x.closeAfter) return true;
        return false;
    };
    out.clear();
    lb.OnConnect(3, peer, loop);
    for (const char* l : {"CVERS 11020 9472", "NICK plain", "USER UserName HostName irc.westwood.com :RealName"}) lb.OnLine(3, l, out);
    Check(!out.empty() && out.back().line == ": 376 u" && hasLine(out, ": 372 u :- *** Nox Online client patch") && !anyClose(out) &&
              !hasLine(out, "ERROR") && lb.Stats().loginNoClient == 1,
          "NOXCLIENT 없는 로그인 → 끊지 않고 376 까지 · 인사말에 패치 안내");
    out.clear();
    lb.OnLine(3, "JOINGAME #NoxOrigin's_game 1 ", out);
    Check(out.size() == 4 && !hasLine(out, "NOXTOKEN") && Tokens(out[0].line).size() > 1 && Tokens(out[0].line)[1] == "JOINGAME" &&
              lb.Stats().joinNoClient == 1 && lb.Stats().tokensIssued == 1,
          "DLL 없는 클라의 방 참가 → 원래 답 넷 · 토큰 없음");
    out.clear();
    lb.OnConnect(4, peer, loop);
    for (const char* l : {"NOXCLIENT 0", "NICK old", "USER UserName HostName irc.westwood.com :RealName"}) lb.OnLine(4, l, out);
    Check(!out.empty() && out.back().line == ": 376 u" && hasLine(out, ": 372 u :- *** Nox Online client patch") && !anyClose(out) &&
              lb.Stats().loginOldClient == 1 && lb.Stats().loginNoClient == 1,
          "DLL 판 0 → 「판 낮음」으로 따로 세고 인사말 안내");
    out.clear();
    lb.OnConnect(5, peer, loop);
    for (const char* l : {"NICK early", "JOINGAME #NoxOrigin's_game 1 "}) lb.OnLine(5, l, out);
    // ★ 7단계 조각 6 — 옛 꼴은 원래 답 넷(토큰 없음)이었다. 이제 로그인 확인 전 참가는 거절 471 한 줄(토큰 · 새 방은 확인됨만 · 검사 지적 2)
    Check(OnlyReject(out, 5, "#NoxOrigin's_game", JoinReject::NotLoggedIn) && lb.Stats().joinNoClient == 1 && lb.Stats().tokensIssued == 1,
          "로그인 전 방 참가 → 거절 471 한 줄(토큰 없음 · 원래 답 없음)");
    out.clear();
    lb.OnLine(2, "JOINGAME #NoxOrigin's_game 1 ", out);   // DLL 있는 접속은 그대로 토큰
    Check(out.size() == 5 && hasLine(out, "NOXTOKEN ") && lb.Stats().tokensIssued == 2, "DLL 있는 접속은 계속 토큰 + 원래 답 넷");

    // ★ 10-16 작업 11 — 우리 로비 표시: DLL 판 2 이상이면 NOXCLIENT 에 곧바로 NOXSERVER 1 한 줄 · 판 1 은 답 없음(그 DLL 은 이 줄을 못 뗀다)
    std::vector<Out> o2, o1;
    lb.OnConnect(6, peer, loop);
    lb.OnLine(6, "NOXCLIENT 2", o2);
    lb.OnConnect(7, peer, loop);
    lb.OnLine(7, "NOXCLIENT 1", o1);
    Check(o2.size() == 1 && o2[0].conn == 6 && o2[0].line == "NOXSERVER 1" && !o2[0].closeAfter && o1.empty(),
          "NOXCLIENT 2 → NOXSERVER 1 한 줄 · NOXCLIENT 1 → 답 없음");
}

// ── ★ 10-14 작업 8d — 채널 안 로비: 방은 채널이 그 순간 주는 것 · 방 정보 = 방의 이름 · 맵 · 모드 · 인원 · 게임 서버 주소 = 붙은 내 쪽 주소 ──
void ChannelLobbyUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;   // 게임 서버 주소 0 = 이 접속이 붙은 내 쪽 주소(채널이 곧 게임 서버)
    cfg.room.maxPlayers = 31;
    cfg.gameServerPort = 18590;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    FakeRooms fake;
    LobbyRoom r = FakeRoom(7, "wn");
    r.name = "wn game";
    r.map = "so_lod";
    r.mode = "chat";
    r.players = 2;
    fake.rooms = {r};
    Lobby lb(cfg, fake);
    const uint32_t local = (203u << 24) | (0u << 16) | (113u << 8) | 5u;   // 203.0.113.5 — 채널이 붙은 주소(문서용)
    const uint32_t peer = (198u << 24) | (51u << 16) | (100u << 8) | 7u;
    std::vector<Out> out;
    lb.OnConnect(1, peer, local);
    // 접속 이름은 방을 만든 사람(wn)과 다르게 — 같으면 사라진 뒤 참가가 「새 방」(작업 10a)이 된다
    for (const char* l : {"NOXCLIENT 1", "NICK bob", "USER UserName HostName irc.westwood.com :RealName"}) lb.OnLine(1, l, out);
    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    GameInfo g;
    std::string chan, ipCol;
    int n326 = 0;
    for (const auto& o : out) {
        const auto t = Tokens(o.line);
        if (t.size() == 11 && t[1] == "326") {
            ++n326;
            chan = t[3];
            ipCol = t[9];
            Unpack(t[10].substr(4), g);
        }
    }
    Check(n326 == 1 && chan == "#wn's_game" && ipCol == std::to_string(local), "326 = 만든 사람 채널 · 방장 주소 칸 = 붙은 내 쪽 주소");
    Check(g.name == "wn game" && g.map == "so_lod" && (g.flags & kMapTypeMask) == kMapChat && g.players == 2 && g.maxPlayers == 31 &&
          g.access == 0,
          "방 정보 = 방의 이름 · 맵 · 모드(chat) · 인원 + 틀의 최대 인원 · 열림");
    {   // ★ 10-16 작업 12c — 방의 최대 인원이 있으면 그 값 · 판 중이면 닫힘(access 1)
        LobbyRoom r2 = r;
        r2.maxPlayers = 8;
        r2.playing = true;
        FakeRooms fake2;
        fake2.rooms = {r2};
        Lobby lb2(cfg, fake2);
        std::vector<Out> o2;
        lb2.OnConnect(1, peer, local);
        for (const char* l : {"NOXCLIENT 1", "NICK bob", "USER UserName HostName irc.westwood.com :RealName", "LIST 37 37"}) lb2.OnLine(1, l, o2);
        GameInfo g2;
        int n2 = 0;
        for (const auto& o : o2) {
            const auto t = Tokens(o.line);
            if (t.size() == 11 && t[1] == "326") { ++n2; Unpack(t[10].substr(4), g2); }
        }
        Check(n2 == 1 && g2.maxPlayers == 8 && g2.access == 1 && g2.players == 2, "방 정보 = 방의 최대 인원(8) · 판 중 닫힘(access 1)");
    }
    out.clear();
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    const auto first = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    nox::JoinToken tok;
    uint8_t raw[nox::kJoinTokenSize] = {};
    nox::JoinTokenCheck chk = nox::JoinTokenCheck::BadSize;
    if (first.size() == 3 && nox::ParseHex(first[2], raw, sizeof(raw)))
        chk = nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok);
    Check(first.size() == 3 && first[1] == "203.0.113.5:18590" && chk == nox::JoinTokenCheck::Ok && tok.roomId == 7,
          "NOXTOKEN = 붙은 내 쪽 주소 · 그 방 번호");
    // 방이 사라지면(방 관리자가 닫음) 다음 목록에서 빠지고 참가엔 거절(★ 10-20 — 그 전엔 답하지 않았다)
    fake.rooms.clear();
    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    const bool emptyList = out.size() == 2 && Tokens(out[0].line)[1] == "321" && Tokens(out[1].line)[1] == "323";
    out.clear();
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    Check(emptyList && OnlyReject(out, 1, "#wn's_game", JoinReject::NoSuchRoom) && lb.Stats().tokensIssued == 1,
          "방이 사라지면 목록 0줄 · 참가엔 거절 471(토큰 없음)");
}

// ── ★ 10-14 작업 10a — 「새 방」: 제 이름 채널로 온 없는 방 참가 → 채널에 만들게 하고 그 방으로 참가 · 방 이름은 접속의 코드페이지로 ──
void NewRoomUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    FakeRooms fake;
    Lobby lb(cfg, fake);
    const uint32_t local = 0x7f000001, peer = 0x7f000001;
    std::vector<Out> out;
    auto login = [&](int conn, const char* nick, bool dll, const char* codepage) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        if (dll) lb.OnLine(conn, "NOXCLIENT 1", o);
        lb.OnLine(conn, std::string("NICK ") + nick, o);
        lb.OnLine(conn, "USER UserName HostName irc.westwood.com :RealName", o);
        if (codepage) lb.OnLine(conn, std::string("SETCODEPAGE ") + codepage, o);
    };
    auto kinds = [](const std::vector<Out>& o) {
        std::string k;
        for (const auto& x : o) {
            const auto t = Tokens(x.line);
            k += (k.empty() ? "" : ",") + (t.size() > 1 && t[0] == ":" ? t[1] : t.size() > 1 && !t[0].empty() && t[0][0] == ':' ? "user:" + t[1] : t[0]);
        }
        return k;
    };
    auto nameIn326 = [](const std::vector<Out>& o) {
        for (const auto& x : o) {
            const auto t = Tokens(x.line);
            GameInfo g;
            if (t.size() == 11 && t[1] == "326" && Unpack(t[10].substr(4), g)) return g.name;
        }
        return std::string("(없음)");
    };

    login(1, "wn", true, "949");
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    const auto tk = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    nox::JoinToken tok;
    uint8_t raw[nox::kJoinTokenSize] = {};
    nox::JoinTokenCheck chk = nox::JoinTokenCheck::BadSize;
    if (tk.size() == 3 && nox::ParseHex(tk[2], raw, sizeof(raw))) chk = nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok);
    bool host353 = false;
    for (const auto& o : out) host353 = host353 || o.line.find(":@wn,0,") != std::string::npos;
    Check(fake.created == 1 && kinds(out) == "NOXTOKEN,user:JOINGAME,332,353,366" && chk == nox::JoinTokenCheck::Ok && tok.roomId == 101 &&
              host353 && lb.Stats().roomsCreated == 1,
          "제 이름 채널의 없는 방 → 새 방(101) · NOXTOKEN · 원래 답 넷 · 353 방장 = 만든 사람");

    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    const std::string ko = nameIn326(out);
    Check(ko == "wn\xC0\xC7 \xB0\xD4\xC0\xD3", "코드페이지 949 접속 — 방 이름 「wn의 게임」을 CP949 로(7바이트 한글 + 공백)");
    login(2, "bob", true, "1252");
    out.clear();
    lb.OnLine(2, "LIST 37 37", out);
    Check(nameIn326(out) == "wn? ??", "코드페이지 1252 접속 — 한글은 '?'(그 코드페이지에 없는 글자)");

    out.clear();
    lb.OnLine(2, "JOINGAME #wn's_game 1 ", out);
    const auto tk2 = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    nox::JoinToken tok2;
    uint8_t raw2[nox::kJoinTokenSize] = {};
    if (tk2.size() == 3 && nox::ParseHex(tk2[2], raw2, sizeof(raw2))) nox::VerifyJoinToken(LobbyTestKey(), raw2, sizeof(raw2), kLobbyTestNow, peer, tok2);
    Check(fake.created == 1 && out.size() == 5 && tok2.roomId == 101, "남의 방(있음) 참가 — 새로 안 만들고 그 방 토큰");
    out.clear();
    lb.OnLine(2, "JOINGAME #carol's_game 1 ", out);
    Check(fake.created == 1 && OnlyReject(out, 2, "#carol's_game", JoinReject::NoSuchRoom), "남의 이름 채널의 없는 방 — 안 만들고 거절 471");

    login(3, "plain", false, "949");
    out.clear();
    lb.OnLine(3, "JOINGAME #plain's_game 1 ", out);
    Check(fake.created == 1 && out.empty() && lb.Stats().newRoomNoClient == 1 && lb.Stats().JoinRejectedTotal() == 1,
          "DLL 없는 클라의 새 방 — 안 만들고 답 없음(들어올 수 없는 방 · 그 클라의 원래 방 만들기엔 정품이 받아 줄 거절 답이 없다)");

    fake.canCreate = false;
    login(4, "dave", true, "949");
    out.clear();
    lb.OnLine(4, "JOINGAME #dave's_game 1 ", out);
    Check(OnlyReject(out, 4, "#dave's_game", JoinReject::RequestFailed) && lb.Stats().roomCreateFailed == 1 && lb.Stats().roomsCreated == 1,
          "채널이 못 만들면 거절 471(곧바로 실패) · 실패 셈");

    // 같은 이름으로 다시 만든 방 — 옛 방(닫힘)의 채널 구성원(wn · bob)을 비우고 새로 든 wn 만(326 사람 수 = 가짜 방장 1 + 1)
    fake.canCreate = true;
    fake.rooms.clear();   // 방 관리자가 101 을 닫았다
    out.clear();
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    out.clear();
    lb.OnLine(2, "LIST 37 37", out);
    std::string users;
    for (const auto& o : out) {
        const auto t = Tokens(o.line);
        if (t.size() == 11 && t[1] == "326") users = t[4];
    }
    Check(fake.created == 2 && users == "2", "같은 이름으로 다시 만든 방 — 옛 채널 구성원 비움(326 사람 수 2)");

    // 이름 칸 15바이트 — 넘치면 글자 단위로 뒤에서 자른다(두 바이트 글자를 반으로 안 자름)
    fake.rooms.back().name = "abcdefghijk의 게임";   // 11 + 7 = 18바이트(CP949)
    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    Check(nameIn326(out) == "abcdefghijk\xC0\xC7 ", "이름 칸 15바이트 — 「게」(2바이트)가 넘쳐 「abcdefghijk의 」 14바이트");
}

// ── ★ 10-20 MO 6단계 v2 작업 3a — 미룬 새 방 답: 게임 서버에 묻고(`Pending`) 답이 오면 참가 답 · 갈래(만듦 · 실패 · 시간 초과 · 끊김 · 늦은 답 ·
//   기다리는 중 또 참가) · 방마다 게임 서버 주소 · 포트 · 토큰 키 ──
void PendingRoomUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    uint32_t clock = 0xfffff000u;   // 32비트 시계가 도는 자리(한도 계산이 돌아도 맞나)
    cfg.nowMs = [&clock] { return clock; };
    FakeRooms fake;
    fake.ask = true;
    Lobby lb(cfg, fake);
    const uint32_t local = 0x7f000001, peer = 0x7f000001;
    const uint32_t gsIp = (10u << 24) | 5u;   // 10.0.0.5 — 게임 서버가 등록한 주소
    const nox::JoinKey gsKey = nox::GameServerTokenKey(LobbyTestKey(), 1760000000u, gsIp, 18600);
    auto login = [&](int conn, const char* nick) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        for (const std::string l : {std::string("NOXCLIENT 2"), std::string("NICK ") + nick,
                                    std::string("USER UserName HostName irc.westwood.com :RealName"), std::string("SETCODEPAGE 949")})
            lb.OnLine(conn, l, o);
    };
    auto made = [&](const std::string& creator) {   // 게임 서버가 만든 방(그 게임 서버 주소 · 포트 · 키)
        LobbyRoom r = fake.Make(creator);
        r.gameIp = gsIp;
        r.gamePort = 18600;
        r.ownTokenKey = true;
        r.tokenKey = gsKey;
        return r;
    };
    auto kindsOf = [](const std::vector<Out>& o) {
        std::string k;
        for (const auto& x : o) {
            const auto t = Tokens(x.line);
            k += (k.empty() ? "" : ",") + (t.size() > 1 && t[0] == ":" ? t[1] : t.size() > 1 && !t[0].empty() && t[0][0] == ':' ? "user:" + t[1] : t[0]);
        }
        return k;
    };
    std::vector<Out> out;

    // ① 물음 → 답 전엔 아무 줄도 안 감 → 답이 오면 참가 답(줄 차례는 곧바로 만든 때와 같음) · 토큰은 그 게임 서버 키로만 풀림
    login(1, "wn");
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    Check(out.empty() && fake.asked.size() == 1 && lb.PendingRooms() == 1 && lb.Stats().roomsRequested == 1 && lb.Stats().roomsCreated == 0,
          "① 새 방 물음 → 답 전엔 줄 없음 · 기다림 1");
    LobbyRoom r1 = made("wn");
    fake.rooms.push_back(r1);   // 거울은 `CREATED` 앞의 `ROOM` 으로 먼저 채워진다
    lb.OnRoomCreated(fake.asked[0], r1, out);
    const auto tk = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    uint8_t raw[nox::kJoinTokenSize] = {};
    nox::JoinToken tok;
    const bool hexOk = tk.size() == 3 && nox::ParseHex(tk[2], raw, sizeof(raw));
    Check(kindsOf(out) == "NOXTOKEN,user:JOINGAME,332,353,366" && lb.PendingRooms() == 0 && lb.Stats().roomsCreated == 1,
          "① 답 → NOXTOKEN · 원래 답 넷(곧바로 만든 때와 같은 차례) · 기다림 0");
    Check(hexOk && tk[1] == "10.0.0.5:18600" &&
              nox::VerifyJoinToken(gsKey, raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::Ok && tok.roomId == r1.gameRoomId &&
              nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::BadMac,
          "① NOXTOKEN = 그 게임 서버 주소:포트 · 토큰은 그 게임 서버 키로만 풀림(설정 토큰 키로는 서명 실패)");
    bool host353 = false;
    for (const auto& o : out) host353 = host353 || o.line.find(":@wn,0," + std::to_string(gsIp) + " ") != std::string::npos;
    Check(host353, "① 353 방장 자리 주소 = 그 게임 서버 주소");

    // ② 목록 — 방마다 그 방의 게임 서버 주소(옛 꼴은 접속마다 하나) · 남의 방 참가도 그 게임 서버 키
    LobbyRoom other = FakeRoom(5, "eve");   // 채널 안 로비 꼴(주소 · 키 없음) — 붙은 내 쪽 주소 · 설정 키
    fake.rooms.push_back(other);
    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    std::vector<std::string> ipCols;
    for (const auto& o : out) {
        const auto t = Tokens(o.line);
        if (t.size() == 11 && t[1] == "326") ipCols.push_back(t[3] + "@" + t[9]);
    }
    Check(ipCols.size() == 2 && ipCols[0] == "#wn's_game@" + std::to_string(gsIp) && ipCols[1] == "#eve's_game@" + std::to_string(local),
          "② 326 방장 주소 칸 = 방마다 그 방의 게임 서버(등록 주소 · 없으면 붙은 내 쪽 주소)");
    login(2, "bob");
    out.clear();
    lb.OnLine(2, "JOINGAME #eve's_game 1 ", out);
    const auto tk2 = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    Check(tk2.size() == 3 && tk2[1] == "127.0.0.1:18590" && nox::ParseHex(tk2[2], raw, sizeof(raw)) &&
              nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::Ok,
          "② 키 없는 방 → 옛 동작(붙은 내 쪽 주소 · 설정 포트 · 설정 토큰 키)");

    // ③ 실패 답 → 거절 471(★ 10-20 — 그 전엔 답 없음) · 실패 셈 · 기다림 0
    login(3, "carol");
    out.clear();
    lb.OnLine(3, "JOINGAME #carol's_game 1 ", out);
    const bool quietBeforeFail = out.empty();
    lb.OnRoomCreateFailed(fake.asked.back(), "대기 맵 없음", out);
    Check(quietBeforeFail && OnlyReject(out, 3, "#carol's_game", JoinReject::CreateFailed) && lb.PendingRooms() == 0 && lb.Stats().roomCreateFailed == 1,
          "③ 실패 답 → 기다리던 클라에 거절 471 한 줄(참가 채널 · 게임 서버 실패) · 실패 셈 1 · 기다림 0");

    // ④ 시간 초과 — 한도 1ms 전엔 남고 한도에 접힘(32비트 시계가 도는 자리) · 그 뒤 온 답은 늦은 답(버림)
    login(4, "dave");
    out.clear();
    lb.OnLine(4, "JOINGAME #dave's_game 1 ", out);
    const uint32_t reqDave = fake.asked.back();
    lb.Tick(out);   // 한도 시각은 32비트를 넘어 0 근처로 돌았고 지금은 아직 안 돎 — 그냥 크기로 견주면 여기서 바로 접힌다
    const bool waitingAtStart = lb.PendingRooms() == 1 && out.empty();
    clock += kRoomRequestTimeoutMs - 1;
    lb.Tick(out);
    const bool stillWaiting = lb.PendingRooms() == 1 && lb.Stats().roomRequestTimeout == 0 && out.empty();
    clock += 1;
    lb.Tick(out);
    Check(waitingAtStart && stillWaiting && lb.PendingRooms() == 0 && lb.Stats().roomRequestTimeout == 1 &&
              OnlyReject(out, 4, "#dave's_game", JoinReject::Timeout),
          "④ 한도 1ms 전엔 기다림(줄 없음) · 한도에 시간 초과 → 거절 471 한 줄 — 시계가 32비트를 넘어 돌아도");
    out.clear();
    lb.OnRoomCreated(reqDave, made("dave"), out);
    Check(out.empty() && lb.Stats().lateRoomReply == 1 && lb.Stats().roomsCreated == 1, "④ 시간 초과 뒤 온 답 → 늦은 답(버림 · 참가 답 없음)");

    // ⑤ 기다리는 중 또 참가 → 무시(답이 둘이 되지 않게) · 기다리는 중 클라가 끊음 → 버림 · 그 뒤 답은 늦은 답
    login(5, "erin");
    out.clear();
    lb.OnLine(5, "JOINGAME #erin's_game 1 ", out);
    const uint32_t reqErin = fake.asked.back();
    lb.OnLine(5, "JOINGAME #erin's_game 1 ", out);
    Check(out.empty() && fake.asked.size() == 4 && lb.Stats().joinWhilePending == 1 && lb.PendingRooms() == 1,
          "⑤ 기다리는 중 또 참가 → 새로 안 묻고 무시");
    lb.OnClose(5, out);
    Check(lb.PendingRooms() == 0 && lb.Stats().roomRequestDropped == 1, "⑤ 기다리는 중 클라가 끊음 → 버림 셈 1");
    lb.OnRoomCreated(reqErin, made("erin"), out);
    lb.OnRoomCreateFailed(9999, "모르는 요청", out);
    Check(out.empty() && lb.Stats().lateRoomReply == 3, "⑤ 끊긴 뒤 온 답 · 모르는 요청의 실패 → 늦은 답(버림 · 거절 줄도 없음)");

    // ⑥ 요청 번호는 겹치지 않는다(답이 엉뚱한 접속에 가지 않게)
    std::set<uint32_t> uniq(fake.asked.begin(), fake.asked.end());
    Check(uniq.size() == fake.asked.size() && fake.asked.size() == 4, "⑥ 요청 번호 넷이 모두 다름");
    // 셈 맞추기 — 물음 4 = 만듦 1 + 실패 1 + 시간 초과 1 + 버림 1
    const LobbyStats& s = lb.Stats();
    Check(s.roomsRequested == 4 && s.roomsRequested == s.roomsCreated + s.roomCreateFailed + s.roomRequestTimeout + s.roomRequestDropped,
          "⑥ 물음 4 = 만듦 + 실패 + 시간 초과 + 버림(요청마다 한 갈래로 끝남)");
    // ★ 10-20 거절 답 — 거절은 실패 · 시간 초과마다 하나(만듦 · 버림 · 늦은 답 · 기다리는 중 또 참가엔 없음)
    Check(s.JoinRejectedTotal() == 2 && Rejected(s, JoinReject::CreateFailed) == 1 && Rejected(s, JoinReject::Timeout) == 1 &&
              s.JoinRejectedTotal() == s.roomCreateFailed + s.roomRequestTimeout,
          "⑥ 거절 2 = 게임 서버 실패 1 + 시간 초과 1(실패 + 시간 초과와 같은 수)");
}

// ── ★ 랭크 매칭 조각 4 — 줄: 줄 섬 · 모음(최소 + 모으기 시간) · 최대 즉시 · 빠짐 셋(창 닫힘 · LIST · 끊김) · 요청 중 빠짐 ·
//    방 실패 · 시간 초과(줄 앞에 되돌림 · 쉬고 다시) · 곧바로 실패 · 곧바로 섬 · 표시 버림 · 낮은 판 · 참가 무시 · 셈의 합 ──
// ★ 매칭 손보기 6 — 수락 단계(판 5 DLL 끼리 · `Lobby.h` `kNoxClientAcceptVersion`)
//   ① 방이 서면 둘 다 `NOXRANK FOUND 10`(참가 답 없음) · 하나 수락 → 아직 · 둘 다 수락 → 각자 참가 답
//   ② 하나 수락 뒤 다른 쪽 LEAVE(창의 「아니오」 · 취소) → 수락한 쪽 `NOXRANK WAIT` · 줄 맨 앞 · 다음 사람과 다시 찾음
//   ③ 한도 — 1ms 앞 그대로 · 한도에 수락 안 한 쪽 `NOXRANK EXPIRED`(줄에서 빠짐) · 수락한 쪽 WAIT
//   ④ 판 4 가 섞이면 옛 길(곧바로 참가 답) ⑤ 찾음 중이 아닌 ACCEPT → 버림 ⑥ 찾음 중 끊김 → 남은 쪽 WAIT ⑦ 셈의 합
void RankAcceptUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    uint32_t clock = 0xfffff000u;   // 32비트 시계가 도는 자리
    cfg.nowMs = [&clock] { return clock; };
    cfg.rankMin = cfg.rankMax = 2;
    FakeRooms fake;   // 곧바로 섬(`Created`)
    Lobby lb(cfg, fake);
    const uint32_t local = 0x7f000001, peer = 0x7f000001;
    auto login = [&](int conn, const std::string& nick, int ver) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        for (const std::string l : {"NOXCLIENT " + std::to_string(ver), "NICK " + nick, std::string("USER UserName HostName irc.westwood.com :RealName")})
            lb.OnLine(conn, l, o);
    };
    auto rankJoin = [&](int conn, const std::string& nick, std::vector<Out>& o) {
        lb.OnLine(conn, "NOXRANK JOIN", o);
        lb.OnLine(conn, "JOINGAME #" + nick + "'s_game 1 ", o);
    };
    auto has = [](const std::vector<Out>& o, int conn, const std::string& line) {
        int n = 0;
        for (const auto& x : o) n += x.conn == conn && x.line == line ? 1 : 0;
        return n;
    };
    auto tokens = [](const std::vector<Out>& o, int conn) {
        int n = 0;
        for (const auto& x : o) n += x.conn == conn && x.line.rfind("NOXTOKEN ", 0) == 0 ? 1 : 0;
        return n;
    };
    const std::string found = "NOXRANK FOUND " + std::to_string(kRankAcceptMs / 1000);
    const LobbyStats& s = lb.Stats();
    // ★ 10-31 — 빠진 사람의 묶인 참가 풀기 줄(474)
    auto rel = [](const std::string& nick) { return ": " + std::to_string(kRankReleaseNumeric) + " u #" + nick + "'s_game :Ranked match left"; };
    auto rels = [](const std::vector<Out>& o, int conn) {
        int n = 0;
        for (const auto& x : o) n += x.conn == conn && x.line.rfind(": " + std::to_string(kRankReleaseNumeric) + " ", 0) == 0 ? 1 : 0;
        return n;
    };

    // ①
    login(1, "amy", 5);
    login(2, "bob", 5);
    std::vector<Out> o1;
    rankJoin(1, "amy", o1);
    rankJoin(2, "bob", o1);
    const bool bothFound = has(o1, 1, found) == 1 && has(o1, 2, found) == 1 && tokens(o1, 1) == 0 && tokens(o1, 2) == 0 && lb.RankFoundPending() == 1 &&
                           fake.asks.empty();   // ★ 10-31 — 찾음 때 방 요청 없음
    std::vector<Out> oa;
    lb.OnLine(1, "NOXRANK ACCEPT", oa);
    const bool waitOther = oa.empty() && lb.RankFoundPending() == 1 && fake.asks.empty();
    lb.OnLine(2, "NOXRANK ACCEPT", oa);
    Check(bothFound && waitOther && has(oa, 1, "NOXRANK READY") == 1 && has(oa, 2, "NOXRANK READY") == 1 && fake.asks.size() == 1 &&
              tokens(oa, 1) == 1 && tokens(oa, 2) == 1 && lb.RankFoundPending() == 0 && s.rankAcceptMatched == 2 && s.rankMatched == 2,
          "★ ① 판 5 둘 → 둘 다 FOUND(참가 답 · 방 요청 없음) · 하나 수락은 그대로 · ★ 10-31 둘 다 수락 → READY · 그때 방 요청 1 → 각자 참가 답(토큰)");

    // ②
    login(3, "cat", 5);
    login(4, "dan", 5);
    login(5, "eve", 5);
    std::vector<Out> o2;
    rankJoin(3, "cat", o2);
    rankJoin(4, "dan", o2);
    lb.OnLine(3, "NOXRANK ACCEPT", o2);
    o2.clear();
    lb.OnLine(4, "NOXRANK LEAVE", o2);
    const bool catBack = has(o2, 3, "NOXRANK WAIT") == 1 && lb.RankQueueSize() == 1 && lb.RankFoundPending() == 0 &&
                         s.rankDeclined[static_cast<size_t>(RankLeave::Dll)] == 1 && s.rankFoundRequeued == 1 &&
                         has(o2, 4, rel("dan")) == 1 && rels(o2, 3) == 0;   // ★ 10-31 — 거절한 dan 만 묶인 참가 풂 · 다시 기다리는 cat 은 묶인 그대로
    o2.clear();
    const size_t asks2 = fake.asks.size();
    rankJoin(5, "eve", o2);
    const bool again = catBack && has(o2, 3, found) == 1 && has(o2, 5, found) == 1 && has(o2, 4, found) == 0 && lb.RankFoundPending() == 1 &&
                       fake.asks.size() == asks2;   // ★ 10-31 — 깨진 묶음 · 다시 찾음 어느 쪽도 게임 서버에 안 닿음
    // 마무리 — 둘 다 수락
    std::vector<Out> o2b;
    lb.OnLine(3, "NOXRANK ACCEPT", o2b);
    lb.OnLine(5, "NOXRANK ACCEPT", o2b);
    Check(again && fake.asks.size() == asks2 + 1 && fake.asks.back().roster == std::vector<std::string>{"cat", "eve"},
          "★ ② 수락 뒤 상대가 LEAVE(거절) → 거절한 dan 에 474(묶인 참가 풂) · 수락한 cat 에 WAIT · 줄 맨 앞 → 다음 eve 와 다시 찾음 · "
          "★ 10-31 방 요청은 둘 다 수락한 뒤 한 번(명단 cat · eve)");

    // ③
    login(6, "fay", 5);
    login(7, "gus", 5);
    std::vector<Out> o3;
    rankJoin(6, "fay", o3);
    rankJoin(7, "gus", o3);
    lb.OnLine(6, "NOXRANK ACCEPT", o3);
    clock += kRankAcceptMs - 1;
    o3.clear();
    lb.Tick(o3);
    const bool heldBefore = o3.empty() && lb.RankFoundPending() == 1;
    clock += 1;
    lb.Tick(o3);
    Check(heldBefore && has(o3, 7, "NOXRANK EXPIRED") == 1 && has(o3, 6, "NOXRANK WAIT") == 1 && lb.RankQueueSize() == 1 && s.rankExpired == 1 &&
              lb.RankFoundPending() == 0 && has(o3, 7, rel("gus")) == 1 && rels(o3, 6) == 0,
          "★ ③ 수락 한도 1ms 앞 그대로 · 한도에 수락 안 한 gus EXPIRED + 474(줄에서 빠짐 · 묶인 참가 풂) · 수락한 fay WAIT(줄 맨 앞 · 묶인 그대로)");

    // ④ — fay(5, 줄에 남음) + hal(4) → 옛 길(곧바로 참가 답)
    login(8, "hal", 4);
    std::vector<Out> o4;
    rankJoin(8, "hal", o4);
    const unsigned long long groups0 = s.rankFoundGroups;
    Check(tokens(o4, 6) == 1 && tokens(o4, 8) == 1 && has(o4, 6, found) == 0 && s.rankFoundGroups == groups0,
          "★ ④ 판 4 가 섞인 묶음 → 옛 길(찾음 없이 곧바로 참가 답)");

    // ⑤
    std::vector<Out> o5;
    lb.OnLine(8, "NOXRANK ACCEPT", o5);   // 판 4(참가 답 받음 — 찾음 아님)
    login(9, "ivy", 5);
    lb.OnLine(9, "NOXRANK ACCEPT", o5);   // 판 5 · 줄에도 없음
    Check(o5.empty() && s.rankAcceptStray == 2, "★ ⑤ 찾음 중이 아닌 접속의 ACCEPT(판 4 참가 뒤 · 판 5 줄 밖) → 둘 다 수락 버림 셈 · 줄 없음");

    // ⑥
    login(10, "jon", 5);
    std::vector<Out> o6;
    rankJoin(9, "ivy", o6);
    rankJoin(10, "jon", o6);
    o6.clear();
    lb.OnClose(9, o6);
    Check(has(o6, 10, "NOXRANK WAIT") == 1 && lb.RankQueueSize() == 1 && s.rankDeclined[static_cast<size_t>(RankLeave::Closed)] == 1 &&
              rels(o6, 9) == 0 && rels(o6, 10) == 0,
          "★ ⑥ 찾음 중 ivy 끊김 → 거절(끊김 — 474 없음) · 남은 jon WAIT · 줄 맨 앞");

    // ⑦ 셈의 합 — 찾음 한 사람 = 다 수락 참가 + 되돌림 + 거절 + 한도 + 아직 찾음
    char m[300];
    std::snprintf(m, sizeof(m), "★ ⑦ 찾음 %llu = 다 수락 %llu + 되돌림 %llu + 거절 %llu + 한도 %llu (+ 끝날 때 찾음 %zu묶음)", s.rankFound, s.rankAcceptMatched,
                  s.rankFoundRequeued, s.RankDeclinedTotal(), s.rankExpired, lb.RankFoundPending());
    Check(s.rankFound == s.rankAcceptMatched + s.rankFoundRequeued + s.RankDeclinedTotal() + s.rankExpired && lb.RankFoundPending() == 0 &&
              s.rankFound == 10 && s.rankFoundGroups == 5,
          m);
    // ★ 10-31 — 474 = 거절(창 닫힘 · LIST) + 한도 — 이 묶음엔 dan(창 닫힘) · gus(한도) 둘
    Check(s.rankJoinReleased == s.rankDeclined[static_cast<size_t>(RankLeave::Dll)] + s.rankDeclined[static_cast<size_t>(RankLeave::List)] + s.rankExpired &&
              s.rankJoinReleased == 2,
          "★ ⑦ 묶인 참가 풂(474) 2 = 거절(창 닫힘 · LIST) + 한도 — 끊김엔 없음");
    // ★ 10-31 — 버려진 방 0: 찾음 5묶음 가운데 다 수락한 셋(①② 마무리 · ④ 는 옛 길)만 방 요청 — 깨진 묶음(② 첫 · ③ · ⑥)은 요청 없음
    Check(fake.asks.size() == 3 && s.rankAcceptMatched == 4, "★ ⑦ 방 요청 3 = 다 수락 2묶음 + 옛 길 1묶음 · 깨진 찾음 3묶음은 게임 서버에 안 닿음");
}

// ── ★ 10-31 — 다 수락한 뒤 방 요청(사용자 결정 ②). 묶음마다 새 로비(줄 · 쉬는 시간이 섞이지 않게)
//   ⑧ 게임 서버에 물음(Pending) → 다 수락 READY · 그때 물음 · CREATED → 참가 답
//   ⑨ 다 수락 뒤 방 시간 초과 → 둘 다 WAIT · 줄 맨 앞(차례 그대로) · 요청 접음 · 쉬고 나서 다시 찾음
//   ⑩ 다 수락 뒤 곧바로 실패 → 둘 다 WAIT · 줄 앞
//   ⑪ 다 수락 뒤 방 기다리는 중 한 사람 LEAVE → 그 사람 474 · 남은 사람 WAIT · 줄 앞 · 요청 접음 → 늦은 CREATED 는 버림(참가 답 없음)
//   ⑫ 옛 길(판 4)은 방 시간 초과에 WAIT 없음
void RankReadyUnitTests() {
    using namespace nox::wol;
    uint32_t clock = 0x10000u;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    cfg.nowMs = [&clock] { return clock; };
    cfg.rankMin = cfg.rankMax = 2;
    auto login = [](Lobby& lb, int conn, const std::string& nick, int ver) {
        lb.OnConnect(conn, 0x7f000001, 0x7f000001);
        std::vector<Out> o;
        for (const std::string l : {"NOXCLIENT " + std::to_string(ver), "NICK " + nick, std::string("USER UserName HostName irc.westwood.com :RealName")})
            lb.OnLine(conn, l, o);
    };
    auto rankJoin = [](Lobby& lb, int conn, const std::string& nick, std::vector<Out>& o) {
        lb.OnLine(conn, "NOXRANK JOIN", o);
        lb.OnLine(conn, "JOINGAME #" + nick + "'s_game 1 ", o);
    };
    auto has = [](const std::vector<Out>& o, int conn, const std::string& line) {
        int n = 0;
        for (const auto& x : o) n += x.conn == conn && x.line == line ? 1 : 0;
        return n;
    };
    auto tokens = [](const std::vector<Out>& o, int conn) {
        int n = 0;
        for (const auto& x : o) n += x.conn == conn && x.line.rfind("NOXTOKEN ", 0) == 0 ? 1 : 0;
        return n;
    };
    const std::string found = "NOXRANK FOUND " + std::to_string(kRankAcceptMs / 1000);
    // 둘(판 ver)을 줄에 세워 둘 다 수락까지 — 나온 줄은 o 에
    auto bothAccept = [&](Lobby& lb, int ver, std::vector<Out>& o) {
        login(lb, 1, "amy", ver);
        login(lb, 2, "bob", ver);
        rankJoin(lb, 1, "amy", o);
        rankJoin(lb, 2, "bob", o);
        if (ver >= kNoxClientAcceptVersion) {
            lb.OnLine(1, "NOXRANK ACCEPT", o);
            lb.OnLine(2, "NOXRANK ACCEPT", o);
        }
    };

    {   // ⑧
        FakeRooms fake;
        fake.ask = true;
        Lobby lb(cfg, fake);
        std::vector<Out> o;
        bothAccept(lb, 5, o);
        const bool asked = has(o, 1, "NOXRANK READY") == 1 && has(o, 2, "NOXRANK READY") == 1 && fake.asked.size() == 1 && tokens(o, 1) + tokens(o, 2) == 0 &&
                           lb.RankRequestsPending() == 1 && lb.RankFoundPending() == 0;
        std::vector<Out> oc;
        lb.OnRoomCreated(fake.asked.back(), fake.Make("amy"), oc);
        Check(asked && tokens(oc, 1) == 1 && tokens(oc, 2) == 1 && lb.RankRequestsPending() == 0 && lb.Stats().rankMatched == 2,
              "★ ⑧ 게임 서버에 묻는 꼴 — 다 수락 → READY 둘 · 그때 물음 1 · 토큰 없음 → CREATED → 각자 참가 답");
    }
    {   // ⑨
        FakeRooms fake;
        fake.ask = true;
        Lobby lb(cfg, fake);
        std::vector<Out> o;
        bothAccept(lb, 5, o);
        const uint32_t req = fake.asked.back();
        clock += kRoomRequestTimeoutMs - 1;
        std::vector<Out> ot;
        lb.Tick(ot);
        const bool held = ot.empty() && lb.RankRequestsPending() == 1;
        clock += 1;
        lb.Tick(ot);
        const bool back = has(ot, 1, "NOXRANK WAIT") == 1 && has(ot, 2, "NOXRANK WAIT") == 1 && lb.RankQueueSize() == 2 && lb.RankRequestsPending() == 0 &&
                          lb.Stats().rankRoomTimeout == 1 && lb.Stats().rankRequeued == 2 && fake.cancelled == std::vector<uint32_t>{req} &&
                          tokens(ot, 1) + tokens(ot, 2) == 0 && has(ot, 1, found) == 0;   // 쉬는 중 — 곧바로 다시 안 묶음
        clock += kRankRetryMs;
        std::vector<Out> or2;
        lb.Tick(or2);
        Check(held && back && has(or2, 1, found) == 1 && has(or2, 2, found) == 1 && fake.asked.size() == 1,
              "★ ⑨ 다 수락 뒤 방 시간 초과 — 1ms 앞 그대로 · 한도에 둘 다 WAIT · 줄 앞 2 · 요청 접음 · 쉬는 동안 안 묶음 → 쉬고 나서 다시 찾음(방 요청은 아직 1)");
    }
    {   // ⑩
        FakeRooms fake;
        fake.canCreate = false;
        Lobby lb(cfg, fake);
        std::vector<Out> o;
        bothAccept(lb, 5, o);
        Check(has(o, 1, "NOXRANK READY") == 1 && has(o, 1, "NOXRANK WAIT") == 1 && has(o, 2, "NOXRANK WAIT") == 1 && lb.RankQueueSize() == 2 &&
                  lb.Stats().rankRoomFailed == 1 && tokens(o, 1) + tokens(o, 2) == 0,
              "★ ⑩ 다 수락 뒤 곧바로 실패 → READY 뒤 둘 다 WAIT · 줄 앞 2 · 참가 답 없음");
    }
    {   // ⑪
        FakeRooms fake;
        fake.ask = true;
        Lobby lb(cfg, fake);
        std::vector<Out> o;
        bothAccept(lb, 5, o);
        const uint32_t req = fake.asked.back();
        std::vector<Out> ol;
        lb.OnLine(2, "NOXRANK LEAVE", ol);
        const bool broke = has(ol, 1, "NOXRANK WAIT") == 1 && has(ol, 2, "NOXRANK WAIT") == 0 &&
                           has(ol, 2, ": " + std::to_string(kRankReleaseNumeric) + " u #bob's_game :Ranked match left") == 1 && lb.RankQueueSize() == 1 &&
                           lb.RankRequestsPending() == 0 && fake.cancelled == std::vector<uint32_t>{req} && lb.Stats().rankReadyRequeued == 1 &&
                           lb.Stats().rankLeftRequested[static_cast<size_t>(RankLeave::Dll)] == 1;
        const unsigned long long late0 = lb.Stats().lateRoomReply;
        std::vector<Out> oc;
        lb.OnRoomCreated(req, fake.Make("amy"), oc);
        Check(broke && tokens(oc, 1) == 0 && tokens(oc, 2) == 0 && lb.Stats().lateRoomReply == late0 + 1 && lb.RankQueueSize() == 1,
              "★ ⑪ 다 수락 뒤 방 기다리는 중 bob LEAVE → bob 474 · amy WAIT · 줄 앞 · 요청 접음 → 늦은 CREATED 버림(참가 답 없음 · amy 는 줄에)");
    }
    {   // ⑫
        FakeRooms fake;
        fake.ask = true;
        Lobby lb(cfg, fake);
        std::vector<Out> o;
        bothAccept(lb, 4, o);
        const bool asked = fake.asked.size() == 1 && has(o, 1, found) == 0 && has(o, 1, "NOXRANK READY") == 0;
        clock += kRoomRequestTimeoutMs;
        std::vector<Out> ot;
        lb.Tick(ot);
        Check(asked && has(ot, 1, "NOXRANK WAIT") == 0 && has(ot, 2, "NOXRANK WAIT") == 0 && lb.RankQueueSize() == 2,
              "★ ⑫ 옛 길(판 4) — 찾음 · READY 없이 곧바로 물음 · 시간 초과에 WAIT 없음(정품 창 그대로) · 줄 앞 2");
    }
}

void RankQueueUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    uint32_t clock = 0xfffff000u;   // 32비트 시계가 도는 자리
    cfg.nowMs = [&clock] { return clock; };
    cfg.rankMin = 2;
    cfg.rankMax = 3;
    cfg.rankGatherMs = 30000;
    FakeRooms fake;
    fake.ask = true;
    Lobby lb(cfg, fake);
    const uint32_t local = 0x7f000001, peer = 0x7f000001;
    auto login = [&](int conn, const std::string& nick, int ver = 4) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        for (const std::string l : {"NOXCLIENT " + std::to_string(ver), "NICK " + nick, std::string("USER UserName HostName irc.westwood.com :RealName"),
                                    std::string("SETCODEPAGE 949")})
            lb.OnLine(conn, l, o);
    };
    auto rankJoin = [&](int conn, const std::string& nick, std::vector<Out>& o) {
        lb.OnLine(conn, "NOXRANK JOIN", o);
        lb.OnLine(conn, "JOINGAME #" + nick + "'s_game 1 ", o);
    };
    // 접속 하나가 받은 줄의 종류(`NOXTOKEN,user:JOINGAME,332,353,366`)
    auto kindsFor = [](const std::vector<Out>& o, int conn) {
        std::string k;
        for (const auto& x : o) {
            if (x.conn != conn) continue;
            const auto t = Tokens(x.line);
            k += (k.empty() ? "" : ",") + (t.size() > 1 && t[0] == ":" ? t[1] : t.size() > 1 && !t[0].empty() && t[0][0] == ':' ? "user:" + t[1] : t[0]);
        }
        return k;
    };
    // 그 접속의 참가 답이 제 채널 · 353 방장 = 그 사람 · 토큰이 게임 방 `room` 으로 풀리나
    auto joinedOwn = [&](const std::vector<Out>& o, int conn, const std::string& nick, uint16_t room) {
        bool chan = false, host = false, tokOk = false;
        for (const auto& x : o) {
            if (x.conn != conn) continue;
            const auto t = Tokens(x.line);
            if (t.size() == 3 && t[0] == "NOXTOKEN") {
                uint8_t raw[nox::kJoinTokenSize] = {};
                nox::JoinToken tok;
                tokOk = nox::ParseHex(t[2], raw, sizeof(raw)) &&
                        nox::VerifyJoinToken(LobbyTestKey(), raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::Ok && tok.roomId == room &&
                        tok.account == nick;
            }
            chan = chan || x.line == ":" + nick + "!u@h JOINGAME 1 32 37 1 0 " + std::to_string(peer) + " 1 :#" + nick + "'s_game";
            host = host || x.line.find(": 353 u = #" + nick + "'s_game :@" + nick + ",0,") == 0;
        }
        return chan && host && tokOk;
    };
    auto made = [&](const std::string& creator) {
        LobbyRoom r = fake.Make(creator);
        fake.rooms.push_back(r);
        return r;
    };
    std::vector<Out> out;

    // ① 줄 섬 — 답 없음 · 줄 1 · 최소(2) 밑이라 요청 없음
    login(1, "amy");
    rankJoin(1, "amy", out);
    Check(out.empty() && lb.RankQueueSize() == 1 && fake.asks.empty() && lb.Stats().rankQueued == 1 && lb.Stats().roomsRequested == 0,
          "① 랭크 표시 + 내 방 참가 → 답 없음 · 줄 1 · 새 방 요청 없음");

    // ② 모음 — 둘째가 서면 모으기 시작 · 한도 1ms 전엔 안 묶음 · 한도에 묶어 요청(명단 = 줄 차례 · ranked · 방장 = 첫 사람)
    login(2, "bob");
    rankJoin(2, "bob", out);
    const bool noAskAtMin = out.empty() && fake.asks.empty() && lb.RankQueueSize() == 2;
    clock += cfg.rankGatherMs - 1;
    lb.Tick(out);
    const bool noAskBeforeDue = out.empty() && fake.asks.empty();
    clock += 1;
    lb.Tick(out);
    Check(noAskAtMin && noAskBeforeDue && fake.asks.size() == 1 && fake.asks[0].ranked && fake.asks[0].creator == "amy" &&
              fake.asks[0].roster == std::vector<std::string>{"amy", "bob"} && lb.RankQueueSize() == 0 && lb.RankRequestsPending() == 1 &&
              out.empty() && lb.Stats().rankGroupGather == 1,
          "② 최소가 차도 곧바로 안 묶음 · 모으기 한도 1ms 전엔 그대로 · 한도에 묶어 랭크 요청(명단 amy,bob · 방장 amy) — 시계가 32비트를 넘어 돌아도");
    const uint32_t req2 = fake.asked.back();
    const LobbyRoom r2 = made("amy");
    lb.OnRoomCreated(req2, r2, out);
    Check(kindsFor(out, 1) == "NOXTOKEN,user:JOINGAME,332,353,366" && kindsFor(out, 2) == "NOXTOKEN,user:JOINGAME,332,353,366" &&
              joinedOwn(out, 1, "amy", r2.gameRoomId) && joinedOwn(out, 2, "bob", r2.gameRoomId) && lb.RankRequestsPending() == 0 &&
              lb.Stats().rankMatched == 2 && lb.Stats().roomsCreated == 0,
          "② 방이 서면 둘 다 참가 답 — 각자 제 채널 #<닉>'s_game · 353 방장 = 그 사람 · 토큰은 같은 게임 방(일반 새 방 셈은 그대로 0)");

    // ③ 최대(3) 즉시 — 셋째가 서는 순간 묶음(모으기 시간 안 기다림)
    out.clear();
    login(3, "cat");
    login(4, "dan");
    login(5, "eve");
    rankJoin(3, "cat", out);
    rankJoin(4, "dan", out);
    const size_t asksBefore = fake.asks.size();
    rankJoin(5, "eve", out);
    Check(asksBefore == 1 && fake.asks.size() == 2 && fake.asks[1].roster == std::vector<std::string>{"cat", "dan", "eve"} &&
              lb.RankQueueSize() == 0 && lb.Stats().rankGroupFull == 1 && out.empty(),
          "③ 최대가 차는 순간 곧바로 묶음(cat,dan,eve) — 모으기 시간 안 기다림");
    lb.OnRoomCreated(fake.asked.back(), made("cat"), out);
    out.clear();

    // ④ 빠짐 셋 — 창 닫힘(DLL) · LIST · 끊김. 빠지면 남은 사람으로 모으기를 다시 본다(최소 밑이면 멈춤 · 나중에 다시 차면 시계를 새로)
    login(6, "fay");
    login(7, "gus");
    rankJoin(6, "fay", out);
    rankJoin(7, "gus", out);   // 모으기 시작
    clock += 10000;
    lb.OnLine(6, "NOXRANK LEAVE", out);   // 줄 1 — 모으기 멈춤
    lb.OnLine(7, "LIST 37 37", out);      // 줄 0 · 목록 답은 그대로
    bool listReplied = false;
    for (const auto& o : out) listReplied = listReplied || (o.conn == 7 && o.line == ": 323 u:");
    // ★ 10-31 — 빠진 둘 다 묶인 참가 풂(474) — 제 채널로 한 줄씩
    const auto relCount = [&out](int conn, const std::string& nick) {
        int n = 0;
        for (const auto& o : out) n += o.conn == conn && o.line == ": " + std::to_string(kRankReleaseNumeric) + " u #" + nick + "'s_game :Ranked match left" ? 1 : 0;
        return n;
    };
    const bool released4 = relCount(6, "fay") == 1 && relCount(7, "gus") == 1;
    login(8, "hal");
    rankJoin(8, "hal", out);
    rankJoin(6, "fay", out);   // 다시 줄 — 모으기 시계는 새로(옛 시계가 남았으면 20초 뒤 묶인다)
    clock += 20000;
    lb.Tick(out);
    const bool freshClock = fake.asks.size() == 2 && lb.RankQueueSize() == 2;
    lb.OnClose(8, out);   // 줄 1
    clock += 20000;
    lb.Tick(out);
    lb.OnLine(7, "NOXRANK LEAVE", out);   // 줄에도 요청에도 없음 — 버림
    const LobbyStats& s4 = lb.Stats();
    Check(listReplied && released4 && freshClock && fake.asks.size() == 2 && lb.RankQueueSize() == 1 &&
              s4.rankLeft[static_cast<size_t>(RankLeave::Dll)] == 1 && s4.rankLeft[static_cast<size_t>(RankLeave::List)] == 1 &&
              s4.rankLeft[static_cast<size_t>(RankLeave::Closed)] == 1 && s4.rankLeaveStray == 1 &&
              relCount(8, "hal") == 0 && relCount(7, "gus") == 1 && s4.rankJoinReleased == 2,
          "④ 빠짐 셋(창 닫힘 · LIST · 끊김) 하나씩 · 창 닫힘 · LIST 엔 474(끊김 · 줄 밖 창 닫힘엔 없음) · LIST 엔 목록 답 · 다시 차면 모으기 시계를 새로 · "
          "빠진 뒤 최소 밑이면 안 묶음 · 줄 밖의 창 닫힘은 버림");
    lb.OnClose(6, out);   // 줄 0 으로 정리
    out.clear();

    // ⑤ 요청 중 빠짐 — 방이 서도 빠진 사람엔 답 없음 · 남은 사람만
    login(9, "ivy");
    login(10, "jon");
    rankJoin(9, "ivy", out);
    rankJoin(10, "jon", out);
    clock += cfg.rankGatherMs;
    lb.Tick(out);
    const uint32_t req5 = fake.asked.back();
    lb.OnLine(9, "NOXRANK LEAVE", out);
    const LobbyRoom r5 = made("ivy");
    lb.OnRoomCreated(req5, r5, out);
    Check(fake.asks.back().roster == std::vector<std::string>{"ivy", "jon"} && kindsFor(out, 9) == std::to_string(kRankReleaseNumeric) &&
              joinedOwn(out, 10, "jon", r5.gameRoomId) && lb.Stats().rankLeftRequested[static_cast<size_t>(RankLeave::Dll)] == 1,
          "⑤ 요청 중 창 닫힘 → 그 사람엔 474(묶인 참가 풂) 하나뿐 · 방이 서도 참가 답 없음 · 남은 jon 만 참가 답");
    out.clear();

    // ⑥ 방 실패 → 줄 앞에 되돌림(차례 그대로 · 거절 줄 없음) → 쉬는 동안 안 물음 → 쉬고 나면 곧바로 다시 묶음(모으기 시간 또 안 기다림)
    login(11, "kim");
    login(12, "lee");
    rankJoin(11, "kim", out);
    rankJoin(12, "lee", out);
    clock += cfg.rankGatherMs;
    lb.Tick(out);
    lb.OnRoomCreateFailed(fake.asked.back(), "대기 맵 없음", out);
    const bool requeued = out.empty() && lb.RankQueueSize() == 2 && lb.RankRequestsPending() == 0 && lb.Stats().rankRoomFailed == 1;
    const size_t asksAtFail = fake.asks.size();
    clock += kRankRetryMs - 1;
    lb.Tick(out);
    const bool restHeld = fake.asks.size() == asksAtFail;
    clock += 1;
    lb.Tick(out);
    Check(requeued && restHeld && fake.asks.size() == asksAtFail + 1 && fake.asks.back().roster == std::vector<std::string>{"kim", "lee"} &&
              lb.Stats().rankRequeued == 2,
          "⑥ 방 실패 → 거절 줄 없이 줄 앞에 되돌림 · 쉬는 시간 1ms 전엔 안 물음 · 끝나면 곧바로 같은 차례로 다시 묶음");
    // ⑦ 시간 초과 → 거울에 접음 알림 · 되돌림 · 그 뒤 온 답은 늦은 답(참가 답 없음)
    const uint32_t req7 = fake.asked.back();
    clock += kRoomRequestTimeoutMs - 1;
    lb.Tick(out);
    const bool waiting7 = lb.RankRequestsPending() == 1 && lb.Stats().rankRoomTimeout == 0;
    clock += 1;
    lb.Tick(out);
    const bool timedOut = lb.RankRequestsPending() == 0 && lb.Stats().rankRoomTimeout == 1 && lb.RankQueueSize() == 2 &&
                          !fake.cancelled.empty() && fake.cancelled.back() == req7;
    lb.OnRoomCreated(req7, made("kim"), out);
    Check(waiting7 && timedOut && out.empty() && lb.Stats().lateRoomReply == 1,
          "⑦ 한도 1ms 전엔 기다림 · 한도에 시간 초과 → 거울에 접음 알림 · 줄 앞에 되돌림 · 늦게 온 답은 버림(참가 답 없음)");
    // ⑧ 곧바로 실패(게임 서버 없음) → 되돌리고 쉼(한 바퀴 안에서 다시 묻지 않음) · 곧바로 섬 → 그 자리에서 참가 답
    fake.ask = false;
    fake.canCreate = false;
    clock += kRankRetryMs;
    const size_t asks8 = fake.asks.size();
    lb.Tick(out);
    const bool failedOnce = fake.asks.size() == asks8 + 1 && lb.RankQueueSize() == 2 && lb.Stats().rankRoomFailed == 2;
    fake.canCreate = true;
    clock += kRankRetryMs;
    lb.Tick(out);
    Check(failedOnce && fake.asks.size() == asks8 + 2 && joinedOwn(out, 11, "kim", fake.rooms.back().gameRoomId) &&
              joinedOwn(out, 12, "lee", fake.rooms.back().gameRoomId) && lb.RankQueueSize() == 0,
          "⑧ 곧바로 실패 → 한 번만 묻고 되돌려 쉼 · 곧바로 섬 → 그 자리에서 둘 다 참가 답");
    fake.ask = true;
    out.clear();

    // ⑨ 표시 버림 · 낮은 판 · 참가 무시
    login(13, "max");
    lb.OnLine(13, "NOXRANK JOIN", out);
    lb.OnLine(13, "LIST 37 37", out);                 // 표시 다음 줄이 참가가 아님 → 버림
    lb.OnLine(13, "JOINGAME #max's_game 1 ", out);   // 표시 없음 → 일반 새 방
    const bool markDropped = lb.Stats().rankMarkDropped == 1 && lb.Stats().roomsRequested == 1 && lb.RankQueueSize() == 0;
    login(14, "ned", 3);
    lb.OnLine(14, "NOXRANK JOIN", out);              // 판 3 — 안 믿음
    lb.OnLine(14, "JOINGAME #ned's_game 1 ", out);   // 일반 새 방
    const bool oldIgnored = lb.Stats().rankOldClient == 1 && lb.Stats().roomsRequested == 2 && lb.RankQueueSize() == 0;
    login(15, "oli");
    rankJoin(15, "oli", out);
    rankJoin(15, "oli", out);                         // 줄에 있는 접속이 또 → 무시
    lb.OnLine(15, "JOINGAME #oli's_game 1 ", out);   // 표시 없는 참가도 무시
    // 줄 섬 14 = amy bob · cat dan eve · fay gus hal fay(다시) · ivy jon · kim lee · oli (max · ned 는 일반 새 방)
    Check(markDropped && oldIgnored && lb.Stats().rankDupJoin == 2 && lb.RankQueueSize() == 1 && lb.Stats().rankQueued == 14,
          "⑨ 표시 다음 줄이 참가가 아니면 표시 버림(그 뒤 참가는 일반 새 방) · 판 3 의 표시는 안 믿음 · 줄에 있는 접속의 참가는 무시 · 줄 섬 14");
    lb.OnClose(15, out);

    // ⑩ 셈의 합 — 줄 들어옴(섬 + 되돌림) = 나감(빠짐 · 줄) + 묶임 + 남음 · 묶임 = 참가 답 + 되돌림 + 빠짐(요청 중) · 요청 = 섬 + 실패 + 시간 초과 + 기다림
    const LobbyStats& s = lb.Stats();
    unsigned long long leftQ = 0, leftR = 0;
    for (size_t i = 0; i < kRankLeaveCount; ++i) {
        leftQ += s.rankLeft[i];
        leftR += s.rankLeftRequested[i];
    }
    Check(s.rankQueued + s.rankRequeued == leftQ + s.rankGrouped + lb.RankQueueSize() && s.rankGrouped == s.rankMatched + s.rankRequeued + leftR &&
              s.rankRequests == s.rankRoomsCreated + s.rankRoomFailed + s.rankRoomTimeout + lb.RankRequestsPending() &&
              s.rankRequests == s.rankGroupFull + s.rankGroupGather && s.RankLeftTotal() == leftQ + leftR,
          "⑩ 셈의 합 — 줄 들어옴 = 나감 + 묶임 + 남음 · 묶임 = 참가 답 + 되돌림 + 요청 중 빠짐 · 요청 = 섬 + 실패 + 시간 초과 + 기다림 = 최대 + 모으기 끝");
    std::printf("  랭크 셈 — 줄 섬 %llu · 되돌림 %llu · 빠짐(줄) %llu · 빠짐(요청 중) %llu · 묶임 %llu · 참가 답 %llu · 요청 %llu(섬 %llu · 실패 %llu · 시간 초과 %llu)\n",
                s.rankQueued, s.rankRequeued, leftQ, leftR, s.rankGrouped, s.rankMatched, s.rankRequests, s.rankRoomsCreated, s.rankRoomFailed,
                s.rankRoomTimeout);
}

// ── ★ 10-20 MO 6단계 v2 작업 3b — 채팅 채널: 같은 채널 사람에게 드나듦 · 글 · 다른 채널엔 안 감 · 안 들어간 채널 글은 거름 ──
void ChatUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.joinKey = LobbyTestKey();
    FakeRooms fake;
    Lobby lb(cfg, fake);
    const uint32_t ip = 0x7f000001;
    for (int conn : {1, 2, 3}) {
        lb.OnConnect(conn, ip, ip);
        std::vector<Out> o;
        lb.OnLine(conn, std::string("NICK ") + (conn == 1 ? "amy" : conn == 2 ? "ben" : "cy"), o);
    }
    auto to = [](const std::vector<Out>& o, int conn) {
        std::vector<std::string> v;
        for (const auto& x : o)
            if (x.conn == conn) v.push_back(x.line);
        return v;
    };
    std::vector<Out> out;
    lb.OnLine(1, "JOIN #Lob_37_3 zot0blat", out);
    Check(out.size() == 3 && to(out, 1).size() == 3, "혼자 들어감 → 본인에게만 JOIN · 353 · 366");
    out.clear();
    lb.OnLine(2, "JOIN #Lob_37_3 zot0blat", out);
    const auto a1 = to(out, 1), b1 = to(out, 2);
    Check(a1.size() == 1 && a1[0] == ":ben!u@h JOIN :0,0 #Lob_37_3" && b1.size() == 3 && b1[1].find("amy,0,0") != std::string::npos &&
              b1[1].find("ben,0,0") != std::string::npos,
          "둘째가 들어감 → 먼저 있던 사람에게 JOIN 한 줄 · 본인 353 에 둘");
    out.clear();
    lb.OnLine(3, "JOIN #Lob_37_0 zot0blat", out);
    Check(out.size() == 3 && to(out, 3).size() == 3, "다른 채널 입장 → 그 채널 사람에게만(다른 채널엔 안 감)");

    out.clear();
    lb.OnLine(1, "PRIVMSG #Lob_37_3 :hello there", out);
    Check(out.size() == 1 && out[0].conn == 2 && out[0].line == ":amy!u@h PRIVMSG #Lob_37_3 :hello there" && lb.Stats().chatMessages == 1 &&
              lb.Stats().chatDelivered == 1,
          "글 → 같은 채널의 다른 사람에게만(보낸 사람 · 다른 채널엔 안 감 · 빈칸 든 글 그대로)");
    out.clear();
    lb.OnLine(3, "PRIVMSG #Lob_37_3 :peek", out);
    Check(out.empty() && lb.Stats().chatNotMember == 1, "안 들어간 채널로 글 → 거름(셈)");
    lb.OnLine(1, "PRIVMSG #Lob_37_3", out);
    lb.OnLine(1, "PRIVMSG", out);
    Check(out.empty() && lb.Stats().chatMalformed == 2, "글 칸 없는 PRIVMSG → 답 없음(셈)");

    out.clear();
    lb.OnLine(2, "PART #Lob_37_3", out);
    const auto a2 = to(out, 1), b2 = to(out, 2);
    Check(a2.size() == 1 && a2[0] == ":ben!u@h PART #Lob_37_3" && b2.size() == 1, "나감 → 남은 사람에게 PART 한 줄 · 본인에게도");
    out.clear();
    lb.OnLine(1, "PRIVMSG #Lob_37_3 :alone", out);
    Check(out.empty() && lb.Stats().chatMessages == 2 && lb.Stats().chatDelivered == 1, "혼자 남은 채널 글 → 받는 사람 0");

    out.clear();
    lb.OnLine(1, "LIST -1 37", out);
    std::vector<std::string> chans;
    for (const auto& o : out) {
        const auto tk = Tokens(o.line);
        if (tk.size() > 4 && tk[1] == "327") chans.push_back(tk[3] + "=" + tk[4]);
    }
    Check(chans.size() == 5 && chans[0] == "#Lob_37_0=2" && chans[3] == "#Lob_37_3=2" && chans[4] == "#Lob_37_4=1",
          "목록의 대화방 다섯 · 사람 수 칸 = 그 채널 사람 수 + 1(빈 채널도 1 — 정품이 0 되면 지움)");
    out.clear();
    lb.OnLine(1, "JOIN #Lob_37_5 x", out);
    Check(out.empty(), "다섯 밖 대화방 → 모르는 채널(답 없음)");

    // 끊김 — 대화방 남은 사람에게 PART(QUIT 꼴은 정품 미확인) · 게임 방 채널은 안 알림
    out.clear();
    lb.OnLine(2, "JOIN #Lob_37_3 zot0blat", out);
    out.clear();
    lb.OnClose(2, out);
    Check(out.size() == 1 && out[0].conn == 1 && out[0].line == ":ben!u@h PART #Lob_37_3", "끊김 → 같은 대화방 남은 사람에게 PART 한 줄");
    fake.rooms = {FakeRoom(9, "zed")};
    lb.OnLine(1, "NOXCLIENT 1", out);
    lb.OnLine(3, "NOXCLIENT 1", out);
    lb.OnLine(1, "JOINGAME #zed's_game 1 ", out);
    lb.OnLine(3, "JOINGAME #zed's_game 1 ", out);
    out.clear();
    lb.OnClose(3, out);
    Check(out.empty(), "끊김 — 게임 방 채널 · 혼자 있던 대화방엔 알림 없음");
}

// ── ★ 10-20 MO 6단계 v2 작업 3a-2 — 로비의 방 거울(`RoomMirror`) + 가짜 게임 서버(연결 줄을 `LobbyLink` 로 지어 넣는다) ──
//   확인(HELLO · AUTH) · 등록 차례 · 같은 주소 거절 · 방 알림 → 목록 · 새 방(골라 묻기 → ROOM + CREATED → 미룬 참가 답 · 그 게임 서버 키) ·
//   실패 답 · 안 물은 답 · ROOM 없는 CREATED · 기다리는 중 연결 끊김 · 게임 서버 없음 · 판 결과 · 못 읽는 줄 · 다시 붙어 등록
void MirrorUnitTests() {
    using namespace nox::wol;
    LobbyConfig cfg;
    cfg.room.maxPlayers = 31;
    cfg.joinKey = LobbyTestKey();
    cfg.unixNow = [] { return kLobbyTestNow; };
    cfg.nonce = [] { return 0x0102030405060708ull; };
    uint32_t clock = 1000;   // 미룬 답 한도(⑪)
    cfg.nowMs = [&clock] { return clock; };
    cfg.rankMin = cfg.rankMax = 2;   // ★ 매칭 손보기 7 — ⑫ 둘이 서는 순간 묶음(앞 갈래엔 랭크 줄 없음)
    RoomMirror mirror(LobbyTestKey());
    Lobby lb(cfg, mirror);
    mirror.Attach(lb);
    std::vector<Out> out;
    const uint32_t local = 0x7f000001, peer = 0x7f000001;

    // 가짜 게임 서버 — 연결 접속 번호(클라 접속과 안 겹치게 100 대) · 등록 값
    struct Gs {
        int conn;
        uint32_t epoch, ip;
        uint16_t port;
        std::string nonce;
    };
    auto open = [&](Gs& g) {
        out.clear();
        mirror.OnLinkOpen(g.conn, out);
        nox::LinkLine l;
        if (out.size() == 1 && nox::ParseLinkLine(out[0].line, l) == nox::LinkParse::Ok && l.kind == nox::LinkKind::Hello) g.nonce = l.text;
        return out.size() == 1 && out[0].conn == g.conn && !out[0].closeAfter && g.nonce.size() == 32;
    };
    auto send = [&](const Gs& g, const std::string& line) {
        out.clear();
        mirror.OnLinkLine(g.conn, line, out);
    };
    auto authReg = [&](Gs& g) {
        send(g, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), g.nonce)));
        const bool authQuiet = out.empty();
        send(g, nox::LinkRegister(g.epoch, g.ip, g.port));
        return authQuiet && out.empty();
    };
    auto byeTo = [&](int conn) {
        nox::LinkLine l;
        return out.size() == 1 && out[0].conn == conn && out[0].closeAfter && nox::ParseLinkLine(out[0].line, l) == nox::LinkParse::Ok &&
               l.kind == nox::LinkKind::Bye;
    };
    auto room = [](uint32_t id, const char* host, uint32_t players) {
        nox::LinkRoom r;
        r.id = id;
        r.players = players;
        r.maxPlayers = 8;
        r.name = std::string(host) + "'s game";
        r.creator = host;
        r.host = host;
        r.map = "so_lod";
        r.mode = "chat";
        return r;
    };

    // ① 확인 — 붙자마자 HELLO · 서명 틀림 → BYE 뒤 끊기 · 그 뒤 줄은 버림 · 첫 줄이 AUTH 아님 → 끊기
    Gs bad{101, 1, 0, 18590, {}};
    Check(open(bad), "① 붙자마자 HELLO 한 줄(일회용 값 16진 32자)");
    send(bad, nox::LinkAuth(std::string(64, '0')));
    const bool byeBad = byeTo(101);
    send(bad, nox::LinkRegister(1, 0, 18590));
    Check(byeBad && out.empty() && mirror.Stats().authFailed == 1 && mirror.ServerCount() == 0,
          "① 서명 틀림 → BYE 뒤 끊기 · 끊김 전 줄은 버림(BYE 하나)");
    mirror.OnLinkClose(101, out);
    Gs bad2{102, 1, 0, 18590, {}};
    open(bad2);
    send(bad2, nox::LinkRegister(1, 0, 18590));
    Check(byeTo(102) && mirror.Stats().authFailed == 2, "① 첫 줄이 AUTH 아님 → 확인 실패로 끊기");
    mirror.OnLinkClose(102, out);
    {   // 다른 키(설정이 다른 게임 서버)의 서명
        nox::JoinKey other = LobbyTestKey();
        other[0] ^= 1;
        Gs bad3{103, 1, 0, 18590, {}};
        open(bad3);
        send(bad3, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(other), bad3.nonce)));
        Check(byeTo(103) && mirror.Stats().authFailed == 3, "① 다른 토큰 키에서 뽑은 서명 → 끊기");
        mirror.OnLinkClose(103, out);
    }

    // ② 등록 차례 — 확인 뒤 REGISTER 전 다른 줄 → 끊기 · 바른 차례 → 등록
    Gs early{104, 1, 0, 18590, {}};
    open(early);
    send(early, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), early.nonce)));
    send(early, nox::LinkRoomLine(room(0, "x", 0)));
    Check(byeTo(104) && mirror.Stats().outOfOrder == 1 && mirror.Stats().authOk == 1, "② 확인 뒤 등록 전 ROOM → 끊기");
    mirror.OnLinkClose(104, out);
    const uint32_t ipA = (10u << 24) | 1u, ipB = (10u << 24) | 2u;   // 10.0.0.1 · 10.0.0.2
    Gs a{110, 1760000000u, ipA, 18590, {}}, b{111, 1760000005u, ipB, 18590, {}};
    open(a);
    open(b);
    Check(authReg(a) && authReg(b) && mirror.ServerCount() == 2 && mirror.Stats().registered == 2, "② 게임 서버 둘 확인 · 등록(답 없음)");

    // ③ 같은 주소 · 포트 둘째 등록 → 거절(키가 같아지고 IP 하나에 게임 서버 하나) · 포트가 다르면 받음
    Gs dup{112, 1760000009u, ipA, 18590, {}};
    open(dup);
    send(dup, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), dup.nonce)));
    send(dup, nox::LinkRegister(dup.epoch, dup.ip, dup.port));
    Check(byeTo(112) && mirror.Stats().registerRejected == 1 && mirror.ServerCount() == 2, "③ 같은 주소 · 포트 둘째 등록 → BYE 뒤 끊기");
    mirror.OnLinkClose(112, out);

    // ④ 방 알림 → 목록 · 바뀜 · 사라짐 — 방마다 그 게임 서버 주소 · 포트 · 키
    send(a, nox::LinkRoomLine(room(0, "alice", 1)));
    send(a, nox::LinkRoomLine(room(1, "amy", 2)));
    send(b, nox::LinkRoomLine(room(0, "bea", 3)));
    std::vector<LobbyRoom> lr = mirror.LobbyRooms();
    const nox::JoinKey keyA = nox::GameServerTokenKey(LobbyTestKey(), a.epoch, a.ip, a.port);
    const nox::JoinKey keyB = nox::GameServerTokenKey(LobbyTestKey(), b.epoch, b.ip, b.port);
    Check(lr.size() == 3 && lr[0].host == "alice" && lr[0].gameIp == ipA && lr[0].gamePort == 18590 && lr[0].ownTokenKey && lr[0].tokenKey == keyA &&
              lr[2].host == "bea" && lr[2].gameIp == ipB && lr[2].tokenKey == keyB && lr[2].players == 3 && lr[2].maxPlayers == 8 &&
              lr[2].name == "bea's game" && lr[2].map == "so_lod",
          "④ 방 알림 셋 → 목록 셋 · 방마다 그 게임 서버 주소 · 포트 · 등록 값으로 뽑은 키");
    nox::LinkRoom up = room(1, "amy", 4);
    up.playing = true;
    send(a, nox::LinkRoomLine(up));
    send(a, nox::LinkRoomGone(0));
    send(a, nox::LinkRoomGone(9));
    lr = mirror.LobbyRooms();
    Check(lr.size() == 2 && lr[0].host == "amy" && lr[0].players == 4 && lr[0].playing && mirror.Stats().roomGoneUnknown == 1,
          "④ ROOM 다시 → 바뀜(인원 · 판 중) · ROOM_GONE → 빠짐 · 없는 방의 ROOM_GONE 셈");
    {   // 방장 칸이 비어 오면 만든 사람(옛 `CChannelLobby::ToLobbyRoom` 의 「방장 없으면 만든 사람」)
        nox::LinkRoom noHost = room(5, "hal", 0);
        noHost.host.clear();
        send(b, nox::LinkRoomLine(noHost));
        const auto l2 = mirror.LobbyRooms();
        Check(l2.size() == 3 && l2[2].host == "hal", "④ 방장 칸 빈 방 → 채널 · 353 방장 = 만든 사람");
        send(b, nox::LinkRoomGone(5));
    }
    lb.OnConnect(1, peer, local);
    {
        std::vector<Out> o;
        for (const char* l : {"NOXCLIENT 2", "NICK carl", "USER UserName HostName irc.westwood.com :RealName", "LIST 37 37"}) lb.OnLine(1, l, o);
        std::vector<std::string> cols;
        for (const auto& x : o) {
            const auto t = Tokens(x.line);
            if (t.size() == 11 && t[1] == "326") cols.push_back(t[3] + "@" + t[9]);
        }
        Check(cols.size() == 2 && cols[0] == "#amy's_game@" + std::to_string(ipA) && cols[1] == "#bea's_game@" + std::to_string(ipB),
              "④ 클라 목록 = 거울 · 방장 주소 칸 = 그 방의 게임 서버");
    }

    // ⑤ 새 방 — 방이 적은 게임 서버에 CREATE(a · b 방 하나씩 — 같으면 먼저 붙은 a) · ROOM + CREATED → 미룬 참가 답 · 그 게임 서버 키
    out.clear();
    lb.OnLine(1, "JOINGAME #carl's_game 1 ", out);
    std::vector<Out> sent;
    mirror.TakeOut(sent);
    nox::LinkLine cl;
    const bool asked = sent.size() == 1 && nox::ParseLinkLine(sent[0].line, cl) == nox::LinkParse::Ok && cl.kind == nox::LinkKind::Create &&
                       cl.text == "carl";
    Check(out.empty() && asked && sent[0].conn == a.conn && lb.PendingRooms() == 1, "⑤ 새 방 → 클라엔 아직 줄 없음 · 방 수 같으면 먼저 붙은 게임 서버에 CREATE(만든 사람)");
    send(a, nox::LinkRoomLine(room(7, "carl", 0)));
    send(a, nox::LinkCreated(cl.req, 7));
    const auto tk = out.empty() ? std::vector<std::string>{} : Tokens(out[0].line);
    uint8_t raw[nox::kJoinTokenSize] = {};
    nox::JoinToken tok;
    Check(out.size() == 5 && out[0].conn == 1 && tk.size() == 3 && tk[0] == "NOXTOKEN" && tk[1] == "10.0.0.1:18590" &&
              nox::ParseHex(tk[2], raw, sizeof(raw)) &&
              nox::VerifyJoinToken(keyA, raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::Ok && tok.roomId == 7 &&
              nox::VerifyJoinToken(keyB, raw, sizeof(raw), kLobbyTestNow, peer, tok) == nox::JoinTokenCheck::BadMac &&
              lb.PendingRooms() == 0 && mirror.Stats().createdReplies == 1,
          "⑤ ROOM + CREATED → 클라에 참가 답(NOXTOKEN 그 게임 서버 주소 · 그 키로만 풀림 · 다른 게임 서버 키는 서명 실패)");
    lb.OnConnect(2, peer, local);
    {
        std::vector<Out> o;
        for (const char* l : {"NOXCLIENT 2", "NICK dan", "USER UserName HostName irc.westwood.com :RealName", "JOINGAME #dan's_game 1 "})
            lb.OnLine(2, l, o);
    }
    sent.clear();
    mirror.TakeOut(sent);
    Check(sent.size() == 1 && sent[0].conn == b.conn, "⑤ 다음 새 방 → 방이 적은 게임 서버(b)");

    // ⑥ 실패 답 → 로비 실패(★ 10-20 — 클라에 거절 471) · 안 물은 요청의 답 → 버림
    nox::LinkLine dl;
    nox::ParseLinkLine(sent.empty() ? std::string() : sent[0].line, dl);
    send(b, nox::LinkCreateFailed(dl.req, "no wait map"));
    Check(OnlyReject(out, 2, "#dan's_game", JoinReject::CreateFailed) && lb.PendingRooms() == 0 && lb.Stats().roomCreateFailed == 1 &&
              mirror.Stats().createFailedReplies == 1,
          "⑥ CREATE_FAILED → 로비 실패 셈 · 기다리던 클라(c2)에 거절 471");
    send(b, nox::LinkCreated(dl.req, 3));
    send(a, nox::LinkCreated(4242, 3));
    Check(out.empty() && mirror.Stats().unknownReplies == 2, "⑥ 끝난 요청 · 안 물은 요청의 답 → 버림 셈 2");

    // ⑦ ROOM 없이 CREATED → 들일 수 없는 방 — 실패로
    lb.OnConnect(3, peer, local);
    {
        std::vector<Out> o;
        for (const char* l : {"NOXCLIENT 2", "NICK eve", "USER UserName HostName irc.westwood.com :RealName", "JOINGAME #eve's_game 1 "})
            lb.OnLine(3, l, o);
    }
    sent.clear();
    mirror.TakeOut(sent);
    nox::LinkLine el;
    nox::ParseLinkLine(sent.empty() ? std::string() : sent[0].line, el);
    const Gs& eg = sent.empty() || sent[0].conn == a.conn ? a : b;
    send(eg, nox::LinkCreated(el.req, 55));
    Check(OnlyReject(out, 3, "#eve's_game", JoinReject::CreateFailed) && mirror.Stats().createdNoRoom == 1 && lb.Stats().roomCreateFailed == 2 &&
              lb.PendingRooms() == 0,
          "⑦ ROOM 없이 CREATED → 실패로(기다리던 클라 c3 에 거절 471)");

    // ⑧ 기다리는 중 연결 끊김 → 로비 한도를 안 기다리고 곧바로 실패 · 그 게임 서버의 방이 목록에서 빠짐
    lb.OnConnect(4, peer, local);
    {
        std::vector<Out> o;
        for (const char* l : {"NOXCLIENT 2", "NICK fay", "USER UserName HostName irc.westwood.com :RealName", "JOINGAME #fay's_game 1 "})
            lb.OnLine(4, l, o);
    }
    sent.clear();
    mirror.TakeOut(sent);
    const int target = sent.empty() ? 0 : sent[0].conn;
    const size_t before = mirror.LobbyRooms().size();
    const size_t targetRooms = target == a.conn ? 2 : 1;   // a = alice 빠짐 뒤 amy · carl / b = bea
    out.clear();
    mirror.OnLinkClose(target, out);
    Check(OnlyReject(out, 4, "#fay's_game", JoinReject::CreateFailed) && lb.PendingRooms() == 0 && lb.Stats().roomCreateFailed == 3 &&
              mirror.Stats().failedOnClose == 1 && mirror.LobbyRooms().size() == before - targetRooms && mirror.ServerCount() == 1,
          "⑧ 기다리는 중 연결 끊김 → 곧바로 실패(클라 c4 에 거절 471) · 그 게임 서버 방이 목록에서 빠짐");

    // ⑨ 게임 서버 없음 → 곧바로 실패(묻지 않음)
    const int other = target == a.conn ? b.conn : a.conn;
    out.clear();
    mirror.OnLinkClose(other, out);
    Check(out.empty(), "⑨ 기다리는 요청이 없는 게임 서버 끊김 → 거절 줄 없음");
    lb.OnConnect(5, peer, local);
    {
        std::vector<Out> o;
        for (const char* l : {"NOXCLIENT 2", "NICK gus", "USER UserName HostName irc.westwood.com :RealName"}) lb.OnLine(5, l, o);
        o.clear();   // 로그인 답은 빼고 참가의 답만 본다
        lb.OnLine(5, "JOINGAME #gus's_game 1 ", o);
        sent.clear();
        mirror.TakeOut(sent);
        Check(OnlyReject(o, 5, "#gus's_game", JoinReject::RequestFailed) && sent.empty() && mirror.Stats().noServer == 1 &&
                  lb.Stats().roomCreateFailed == 4 && mirror.LobbyRooms().empty(),
              "⑨ 등록된 게임 서버 없음 → 곧바로 실패(거절 471 · 사람 확인 50 의 갈래) · CREATE 없음 · 목록 0");
    }

    // ⑩ 다시 붙어 등록(같은 주소 — 옛 연결은 끊김) · 판 결과 · 못 읽는 줄은 버리고 연결은 둠 · 게임 서버가 보낼 리 없는 줄도
    Gs a2{120, 1760000100u, ipA, 18590, {}};
    open(a2);
    Check(authReg(a2) && mirror.ServerCount() == 1, "⑩ 끊긴 뒤 같은 주소로 다시 등록 → 받음");
    const std::string json = "{\"match\":\"1760000100-0-1\",\"roomName\":\"a b\"}";
    std::string resLine;
    nox::LinkResult(json, resLine);
    send(a2, resLine);
    const auto res = mirror.TakeResults();
    // ★ 7단계 조각 5 — 이 줄은 칸이 모자라(room 없음) 못 읽음 → 곧바로 SAVED bad(옛 꼴은 줄 없음). 판 저장 갈래는 MirrorSaveUnitTests
    Check(out.size() == 1 && out[0].conn == a2.conn && out[0].line == "SAVED 1760000100-0-1 bad" && res.size() == 1 && res[0] == json &&
              mirror.Stats().results == 1,
          "⑩ RESULT → 판 결과 JSON 그대로 · 셈 · ★ 7단계 칸 모자람 → SAVED bad");
    send(a2, "ROOM 1 2");
    const bool badKept = out.empty() && mirror.ServerCount() == 1;
    send(a2, nox::LinkCreate(1, "x"));
    Check(badKept && out.empty() && mirror.Stats().badLines == 2 && mirror.ServerCount() == 1,
          "⑩ 못 읽는 줄 · 게임 서버가 보낼 리 없는 줄(CREATE) → 버림 셈 · 연결 그대로");

    // 셈 맞추기 — 붙음 = 끊김 + 살아 있음 · 확인 = 맞음 + 틀림 + (확인 전 끊김 없음)
    const MirrorStats& ms = mirror.Stats();
    Check(ms.linksOpened == 8 && ms.linksClosed == 7 && ms.authOk == 5 && ms.authFailed == 3 && ms.registered + ms.registerRejected + ms.outOfOrder == 5,
          "셈 — 붙음 8 = 끊김 7 + 살아 있음 1 · 확인 8 = 맞음 5 + 틀림 3 · 확인된 5 = 등록 3 + 거절 1 + 차례 틀림 1");

    // ⑪ ★ 작업 3c — 로비가 접은 요청(시간 초과 · 클라 끊김) → 거울 기다림 장부에서 지움(`CancelRoom`) · 늦은 CREATED 는 안 물은 답 ·
    //   게임 서버가 끊길 때 이미 접은 요청을 다시 실패로 넘기지 않음(nox_lobby_smoke 에서 나온 어긋남)
    auto host = [&](int conn, const char* nick) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        for (const std::string l : {std::string("NOXCLIENT 2"), std::string("NICK ") + nick, std::string("USER UserName HostName irc.westwood.com :RealName"),
                                    "JOINGAME #" + std::string(nick) + "'s_game 1 "})
            lb.OnLine(conn, l, o);
        std::vector<Out> s;
        mirror.TakeOut(s);
        nox::LinkLine ll;
        return s.size() == 1 && nox::ParseLinkLine(s[0].line, ll) == nox::LinkParse::Ok ? ll.req : 0u;
    };
    const uint32_t reqHal = host(6, "hal");
    clock += kRoomRequestTimeoutMs;
    out.clear();
    lb.Tick(out);
    Check(OnlyReject(out, 6, "#hal's_game", JoinReject::Timeout), "⑪ 시간 초과 → 클라 c6 에 거절 471");
    const uint32_t reqIvy = host(7, "ivy");
    out.clear();
    lb.OnClose(7, out);
    Check(reqHal && reqIvy && lb.Stats().roomRequestTimeout == 1 && lb.Stats().roomRequestDropped == 1 && mirror.Stats().cancelled == 2,
          "⑪ 시간 초과 · 클라 끊김 → 거울 장부에서도 지움(2)");
    const auto late0 = lb.Stats().lateRoomReply;
    const auto unknown0 = mirror.Stats().unknownReplies;
    send(a2, nox::LinkRoomLine(room(3, "hal", 0)));
    send(a2, nox::LinkCreated(reqHal, 3));
    Check(out.empty() && mirror.Stats().unknownReplies == unknown0 + 1 && lb.Stats().lateRoomReply == late0,
          "⑪ 접은 요청의 늦은 CREATED → 거울이 안 물은 답으로 버림(로비까지 안 감)");
    const auto failedOnClose0 = mirror.Stats().failedOnClose;
    const auto failed0 = lb.Stats().roomCreateFailed;
    out.clear();
    mirror.OnLinkClose(a2.conn, out);
    Check(out.empty() && mirror.Stats().failedOnClose == failedOnClose0 && lb.Stats().roomCreateFailed == failed0 && lb.Stats().lateRoomReply == late0,
          "⑪ 게임 서버가 끊겨도 접은 요청을 다시 실패로 안 넘김(거절 줄도 없음)");
    // ★ 10-20 거절 답 셈 — 요청마다 거절은 하나: 게임 서버 실패 3(⑥ ⑦ ⑧) + 곧바로 실패 1(⑨) + 시간 초과 1(⑪) · 끊긴 클라(ivy)엔 없음
    const LobbyStats& ls = lb.Stats();
    Check(ls.JoinRejectedTotal() == 5 && Rejected(ls, JoinReject::CreateFailed) == 3 && Rejected(ls, JoinReject::RequestFailed) == 1 &&
              Rejected(ls, JoinReject::Timeout) == 1 && Rejected(ls, JoinReject::NoSuchRoom) == 0 &&
              ls.JoinRejectedTotal() == ls.roomCreateFailed + ls.roomRequestTimeout,
          "셈 — 거절 5 = 게임 서버 실패 3 + 곧바로 실패 1 + 시간 초과 1(실패 4 + 시간 초과 1 과 같은 수)");

    // ⑫ ★ 매칭 손보기 7 — 랭크 줄로 선 방은 남의 목록(326)에 안 실림 · ROOM 이 다시 와도(인원 바뀜) 그대로 ·
    //   ROOM_GONE 뒤 같은 번호로 온 일반 방은 보임 · 랭크 요청이 실패한 뒤 그 요청의 늦은 답 방도 보임(표시는 CREATED 를 받은 랭크 요청에만)
    Gs a3{121, 1760000200u, ipA, 18590, {}};
    open(a3);
    Check(authReg(a3) && mirror.ServerCount() == 1, "⑫ 새 게임 서버 등록");
    auto login4 = [&](int conn, const char* nick) {
        lb.OnConnect(conn, peer, local);
        std::vector<Out> o;
        for (const std::string l : {std::string("NOXCLIENT 4"), std::string("NICK ") + nick, std::string("USER UserName HostName irc.westwood.com :RealName")})
            lb.OnLine(conn, l, o);
    };
    auto listed = [&](int conn) {   // 그 접속이 받은 목록의 방 채널
        std::vector<Out> o;
        lb.OnLine(conn, "LIST 37 37", o);
        std::string chans;
        for (const auto& x : o) {
            const auto t = Tokens(x.line);
            if (t.size() == 11 && t[1] == "326") chans += (chans.empty() ? "" : ",") + t[3];
        }
        return chans;
    };
    std::vector<int32_t> lastRatings;
    auto rankAsk = [&](int c1, const char* n1, int c2, const char* n2) {   // 둘이 랭크 줄에 서면 묶여 CREATE(랭크 꼬리) 한 줄
        login4(c1, n1);
        login4(c2, n2);
        std::vector<Out> o;
        lb.OnLine(c1, "NOXRANK JOIN", o);
        lb.OnLine(c1, "JOINGAME #" + std::string(n1) + "'s_game 1 ", o);
        lb.OnLine(c2, "NOXRANK JOIN", o);
        lb.OnLine(c2, "JOINGAME #" + std::string(n2) + "'s_game 1 ", o);
        std::vector<Out> s;
        mirror.TakeOut(s);
        nox::LinkLine ll;
        const bool okLine = s.size() == 1 && s[0].conn == a3.conn && nox::ParseLinkLine(s[0].line, ll) == nox::LinkParse::Ok &&
                            ll.kind == nox::LinkKind::Create && ll.ranked && ll.roster.size() == 2;
        lastRatings = ll.rosterRatings;   // ★ 매칭 손보기 5 — CREATE 랭크 칸 2 의 명단 점수
        return okLine ? ll.req : 0u;
    };
    const uint32_t reqJay = rankAsk(8, "jay", 9, "kim");
    const std::vector<int32_t> jayRatings = lastRatings;
    login4(10, "lee");
    send(a3, nox::LinkRoomLine(room(20, "jay", 0)));
    send(a3, nox::LinkRoomLine(room(21, "mo", 1)));   // 견줄 일반 방
    send(a3, nox::LinkCreated(reqJay, 20));
    size_t tokens = 0;
    for (const auto& x : out) tokens += (x.conn == 8 || x.conn == 9) && x.line.rfind("NOXTOKEN ", 0) == 0 ? 1 : 0;
    const std::string l1 = listed(10);
    Check(reqJay && tokens == 2 && l1 == "#mo's_game" && lb.Stats().rankRoomsHidden == 1,
          "⑫ 랭크 방(20) → 명단 둘엔 참가 답 · 셋째 사람 목록엔 일반 방(21)만 = " + l1 + " · 뺀 셈 1");
    send(a3, nox::LinkRoomLine(room(20, "jay", 2)));
    const std::string l2 = listed(10);
    Check(l2 == "#mo's_game" && lb.Stats().rankRoomsHidden == 2, "⑫ 랭크 방 ROOM 다시(인원 2) → 그대로 빠짐 = " + l2 + " · 뺀 셈 2");
    send(a3, nox::LinkRoomGone(20));
    send(a3, nox::LinkRoomLine(room(20, "ned", 0)));
    const std::string l3 = listed(10);
    Check(l3 == "#ned's_game,#mo's_game" && lb.Stats().rankRoomsHidden == 2, "⑫ ROOM_GONE 뒤 같은 번호 일반 방 → 보임 = " + l3);
    lb.OnRatingsChanged({{"oli", 1234, 10}});   // ★ 매칭 손보기 5 — 판 저장 답이 고친 점수(거울이 SAVED ok 때 넘김)
    const uint32_t reqOli = rankAsk(11, "oli", 12, "pat");
    Check(jayRatings == std::vector<int32_t>{1000, 1000} && lastRatings == std::vector<int32_t>{1234, 1000} && lb.RatingOf("oli") == 1234 &&
              lb.RatingOf("nobody") == nox::kRatingStart,
          "★ ⑫ 랭크 방 요청 CREATE 에 명단 점수 — 점수 모름 = 시작 점수(1000 · 1000) · 판 저장 답으로 고친 oli 1234 · 없는 이름 1000");
    send(a3, nox::LinkCreateFailed(reqOli, "no wait map"));
    clock += kRankRetryMs;   // 쉬는 시간 뒤 다시 묻는 CREATE 는 버림(이 갈래는 실패한 요청의 표시만 본다)
    out.clear();
    lb.Tick(out);
    std::vector<Out> again;
    mirror.TakeOut(again);
    send(a3, nox::LinkRoomLine(room(22, "oli", 0)));
    send(a3, nox::LinkCreated(reqOli, 22));   // 끝난 요청 — 안 물은 답(랭크 표시 안 붙음)
    send(a3, nox::LinkRoomLine(room(23, "quinn", 0)));
    const std::string l4 = listed(10);
    Check(reqOli && again.size() == 1 && l4.find("#quinn's_game") != std::string::npos && l4.find("#oli's_game") != std::string::npos &&
              lb.Stats().rankRoomsHidden == 2,
          "⑫ 랭크 요청 실패 → 그 요청 번호의 늦은 답 방도 · 새 일반 방도 보임 = " + l4);
}

// ── ★ 7단계 — 게임 서버 연결 살아 있음(`RoomMirror::Tick` — `Docs/ACCOUNT_DB_DESIGN.md` 조각 2) ──
//   ① 등록된 게임 서버에만 `kLinkPingEveryMs` 마다 PING 한 줄(그 전엔 없음 · 등록 전 접속엔 없음)
//   ② 게임 서버의 PING · 어떤 줄이든 받으면 한도를 다시 잰다(PING 은 맞지 않는 줄로 안 셈)
//   ③ 받은 줄이 `kLinkDeadMs` 동안 없으면 BYE 뒤 끊기(한 번 — 끊김 사건까지 다시 안 함) · 말없는 등록 전 접속도
//   ④ 끊김에 그 방이 목록에서 빠짐 · 끊기 전엔 거절되던 같은 주소 다시 등록이 받힘(6단계 미결 — 전원째 꺼진 게임 서버)
//   시계는 32비트가 넘어가는 자리 근처에서 시작한다(한도를 뺄셈으로 재는지)
void LinkAliveUnitTests() {
    using namespace nox::wol;
    uint32_t clock = 0xfffff000u;
    const uint32_t t0 = clock;
    RoomMirror mirror(LobbyTestKey());
    mirror.SetClock([&clock] { return clock; });
    std::vector<Out> out;
    const uint32_t ipA = (10u << 24) | 1u;
    auto open = [&](int conn) {
        out.clear();
        mirror.OnLinkOpen(conn, out);
        nox::LinkLine l;
        return out.size() == 1 && nox::ParseLinkLine(out[0].line, l) == nox::LinkParse::Ok ? l.text : std::string();
    };
    auto send = [&](int conn, const std::string& line) {
        out.clear();
        mirror.OnLinkLine(conn, line, out);
    };
    auto reg = [&](int conn, uint32_t epoch) {
        const std::string nonce = open(conn);
        send(conn, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), nonce)));
        send(conn, nox::LinkRegister(epoch, ipA, 18590));
    };
    auto tick = [&](uint32_t at) {
        clock = t0 + at;
        out.clear();
        mirror.Tick(out);
    };
    auto onlyPing = [&](int conn) { return out.size() == 1 && out[0].conn == conn && !out[0].closeAfter && out[0].line == nox::LinkPing(); };
    nox::LinkRoom room;
    room.name = "a's game";
    room.creator = room.host = "a";
    room.map = "so_lod";
    room.mode = "chat";

    // ① PING — 등록 직후엔 없음 · 한도 1ms 앞 없음 · 한도에 한 줄 · 같은 때 다시 불러도 하나
    reg(201, 1760000000u);
    send(201, nox::LinkRoomLine(room));
    tick(nox::kLinkPingEveryMs - 1);
    const bool early = out.empty();
    tick(nox::kLinkPingEveryMs);
    const bool first = onlyPing(201);
    tick(nox::kLinkPingEveryMs);
    Check(early && first && out.empty() && mirror.Stats().pingsOut == 1, "① 등록된 게임 서버에 PING — 한도 전 없음 · 한도에 한 줄 · 같은 때 다시 없음");

    // ② 게임 서버의 PING → 셈만(답 없음 · 맞지 않는 줄 아님) · 등록 전 접속(202)엔 PING 이 안 감
    clock = t0 + 6000;
    send(201, nox::LinkPing());
    const bool pingQuiet = out.empty() && mirror.Stats().pingsIn == 1 && mirror.Stats().badLines == 0;
    open(202);   // 붙기만 하고 말없음(6000)
    tick(2 * nox::kLinkPingEveryMs);
    Check(pingQuiet && onlyPing(201), "② PING 받음 → 셈만 · 다음 PING 은 등록된 201 에만(말없는 등록 전 202 엔 없음)");

    // 같은 주소 둘째 등록 — 201 이 살아 있다고 보는 동안은 거절(6단계의 거절 그대로)
    clock = t0 + 2 * nox::kLinkPingEveryMs;
    reg(203, 1760000009u);
    const bool rejected = out.size() == 1 && out[0].closeAfter && mirror.Stats().registerRejected == 1;
    mirror.OnLinkClose(203, out);
    Check(rejected && mirror.ServerCount() == 1, "② 201 이 살아 있는 동안 같은 주소 다시 등록 → 거절");

    // ③ 한도 — 마지막 받은 줄(201 = 6000 · 202 = 붙은 6000)에서 1ms 앞은 그대로 · 한도에 둘 다 BYE 뒤 끊기 · 다시 불러도 안 늘어남
    //   BYE 를 보낸 게임 서버는 끊김 사건 전이라도 목록 · 등록 수에서 곧바로 빠진다(닫는 중)
    //   이 때는 202 가 붙은 지 PING 간격을 넘겼다 — 그래도 PING 은 등록된 201 에만(등록 전 접속엔 없음)
    tick(6000 + nox::kLinkDeadMs - 1);
    const bool alive = onlyPing(201) && mirror.Stats().deadLinks == 0 && mirror.LobbyRooms().size() == 1;
    tick(6000 + nox::kLinkDeadMs);
    const bool listedGone = mirror.LobbyRooms().empty() && mirror.ServerCount() == 0;
    std::set<int> byeTo;
    for (const Out& o : out) {
        nox::LinkLine l;
        if (o.closeAfter && nox::ParseLinkLine(o.line, l) == nox::LinkParse::Ok && l.kind == nox::LinkKind::Bye) byeTo.insert(o.conn);
    }
    const bool dead = out.size() == 2 && byeTo == std::set<int>{201, 202} && mirror.Stats().deadLinks == 2;
    tick(6000 + nox::kLinkDeadMs + 5000);
    bool logged = false;
    for (const std::string& l : mirror.TakeLog()) logged = logged || l.find("초 동안 받은 줄 없음 — 끊음") != std::string::npos;
    Check(alive && dead && listedGone && out.empty() && mirror.Stats().deadLinks == 2 && logged,
          "③ 한도 1ms 앞 그대로(방 1) · 한도에 201 · 202(등록 전 말없음) BYE 뒤 끊기 · 그 자리에서 목록 0 · 다시 안 함 · 기록");

    // ④ 끊김 사건 → 거울에서 지움 · 같은 주소 다시 등록이 받힘
    mirror.OnLinkClose(201, out);
    mirror.OnLinkClose(202, out);
    const bool closed = out.empty() && mirror.Stats().linksClosed == 3;   // 203(거절) · 201 · 202
    // 닫는 중(BYE 보냄)이던 201 도 등록했던 게임 서버로 기록(주소 · 방 수) — 202 는 등록 전
    int closeLog201 = 0, closeLog202 = 0;
    for (const std::string& l : mirror.TakeLog()) {
        closeLog201 += l.find("c201 10.0.0.1:18590 끊김 — 방 1개 목록에서 내림") != std::string::npos ? 1 : 0;
        closeLog202 += l.find("c202 끊김(등록 전)") != std::string::npos ? 1 : 0;
    }
    Check(closeLog201 == 1 && closeLog202 == 1, "④ 끊김 기록 — 무소식으로 끊은 201 은 주소 · 방 수 · 등록 전 202 는 「등록 전」");
    reg(204, 1760000100u);
    Check(closed && out.empty() && mirror.ServerCount() == 1 && mirror.Stats().registerRejected == 1,
          "④ 끊김 사건 셋째까지 거절 줄 없음 · 같은 주소 다시 등록 → 받음(거절 셈 그대로 1)");
}

// ── ★ 7단계 조각 4 — 저장소 경계의 메모리 가짜(`CMemoryLobbyStore` — `--no-db` · 시험 · 정할 것 b) ──
//   ①② ★ 로그인 암호 조각 4 — 로그인 갈래 걷음(답은 다음 `TakeAnswers` 에 — 아래 ③ 이 봄)
//   ③ 판 저장 — 새 판 Saved · 같은 (게임 서버 · 판 번호) Duplicate · 다른 게임 서버의 같은 판 번호는 Saved(따로) · 넣은 칸 그대로
//   ④ 실패 스위치 — 판 저장 Failed(까닭 글) · 실패한 판은 안 넣음 · 끄면 다시 됨
void LobbyStoreUnitTests() {
    using namespace nox::wol;
    CMemoryLobbyStore st;
    std::vector<StoreAnswer> ans;

    nox::MatchRecord rec;
    rec.match = "1760000000-0-1";
    rec.roomName = "wn's game";
    rec.players.resize(2);
    rec.players[0].account = "wn";
    const MatchSource a{0x0a000001u, 18590}, b{0x0a000002u, 18590};
    ans.clear();
    st.SaveMatch(1, a, rec, "{}");
    st.SaveMatch(2, a, rec, "{}");
    st.SaveMatch(3, b, rec, "{}");
    const bool notYet = st.MatchCount() == 2;   // 넣기는 곧바로지만 답은 다음 꺼내기에
    std::vector<StoreAnswer> none;
    st.TakeAnswers(ans);
    st.TakeAnswers(none);
    const nox::MatchRecord* got = st.FindMatch(a, "1760000000-0-1");
    Check(notYet && none.empty() && ans.size() == 3 && ans[0].kind == StoreAnswerKind::SaveMatch && ans[0].req == 1 && ans[0].save == SaveResult::Saved &&
              ans[1].save == SaveResult::Duplicate && ans[2].save == SaveResult::Saved && st.MatchCount() == 2 && got &&
              got->roomName == "wn's game" && got->players.size() == 2 && got->players[0].account == "wn" && !st.FindMatch(b, "x"),
          "③ 새 판 Saved · 같은 게임 서버 같은 판 Duplicate · 다른 게임 서버 같은 판 번호 Saved · 넣은 칸 그대로");

    ans.clear();
    st.SetFailSaves(true);
    rec.match = "1760000000-0-2";
    st.SaveMatch(21, a, rec, "{}");
    st.TakeAnswers(ans);
    const bool failed = ans.size() == 1 && ans[0].req == 21 && ans[0].save == SaveResult::Failed && !ans[0].why.empty() && st.MatchCount() == 2;
    st.SetFailSaves(false);
    ans.clear();
    st.SaveMatch(22, a, rec, "{}");
    st.TakeAnswers(ans);
    Check(failed && ans.size() == 1 && ans[0].save == SaveResult::Saved && st.MatchCount() == 3,
          "④ 실패 스위치 — 판 저장 Failed(까닭 글 · 안 넣음) · 끄면 같은 판이 Saved");

    // ⑤ ★ 7단계 조각 7 — 저장소 칸에 안 맞는 판 → Invalid(DB 워커와 같은 잣대 `MatchFitsStore` · 안 넣음) · 한도 꼭 맞으면 Saved
    ans.clear();
    nox::MatchRecord fit = rec;
    fit.match = "1760000000-0-9";
    fit.roomName = std::string(kRoomNameBytes, 'r');
    fit.players[0].account = std::string(kAccountNameBytes, 'a');
    st.SaveMatch(30, a, fit, "{}");
    nox::MatchRecord longRoom = fit;
    longRoom.match = "1760000000-0-10";
    longRoom.roomName += "r";
    st.SaveMatch(31, a, longRoom, "{}");
    nox::MatchRecord longName = fit;
    longName.match = "1760000000-0-11";
    longName.players[1].account = std::string(kAccountNameBytes + 1, 'b');
    st.SaveMatch(32, a, longName, "{}");
    nox::MatchRecord many = fit;
    many.match = "1760000000-0-12";
    many.players.resize(kMaxMatchPlayers + 1);
    st.SaveMatch(33, a, many, "{}");
    nox::MatchRecord badRaw = fit;
    badRaw.match = "1760000000-0-13";
    st.SaveMatch(34, a, badRaw, std::string("{\"x\":\"\xb0\"}"));
    st.TakeAnswers(ans);
    bool whys = ans.size() == 5;
    for (size_t i = 1; whys && i < ans.size(); ++i) whys = !ans[i].why.empty();
    Check(ans.size() == 5 && ans[0].save == SaveResult::Saved && ans[1].save == SaveResult::Invalid && ans[2].save == SaveResult::Invalid &&
              ans[3].save == SaveResult::Invalid && ans[4].save == SaveResult::Invalid && whys && st.MatchCount() == 4,
          "⑤ 칸 한도 꼭 맞음 Saved · 방 이름 65 · 이름 17 · 사람 256 · UTF-8 아닌 원문 → Invalid(까닭 글 · 안 넣음)");

    // ⑥ ★ 레이팅 조각 3 — 랭크 판만 점수(DB 워커와 같은 `RatingDeltas`) · 겹친 판 안 셈 · 점수 · 순위 읽기 · 읽기 실패 스위치
    CMemoryLobbyStore rs;
    auto duel = [](const char* match, bool ranked, const char* win, const char* lose) {
        nox::MatchRecord m;
        m.match = match;
        m.ranked = ranked;
        m.endReason = "score_player";
        m.winnerKind = "player";
        m.hasWinnerId = true;
        m.players.resize(2);
        m.players[0].hasAccount = m.players[1].hasAccount = true;
        m.players[0].account = win;
        m.players[0].score = 1;
        m.players[1].account = lose;
        return m;
    };
    rs.SaveMatch(40, a, duel("r-1", false, "wn", "bob"), "{}");   // 일반 판
    rs.SaveMatch(41, a, duel("r-2", true, "wn", "bob"), "{}");
    rs.SaveMatch(42, a, duel("r-2", true, "wn", "bob"), "{}");    // 겹친 판
    rs.SaveMatch(43, a, duel("r-3", true, "cat", "wn"), "{}");
    int32_t vWn = 0, vBob = 0, vCat = 0;
    const bool rated = rs.FindRating("wn", vWn) && rs.FindRating("bob", vBob) && rs.FindRating("cat", vCat);
    Check(rated && vWn == 1000 && vBob == 990 && vCat == 1010, "⑥ 랭크 판만 점수 · 겹친 판 안 셈 — wn 1000(+10 −10) · bob 990 · cat 1010");
    ans.clear();
    rs.TakeAnswers(ans);
    ans.clear();
    rs.GetRating(50, "cat");
    rs.GetRating(51, "bob");
    rs.GetRating(52, "nobody");
    rs.SetFailRatings(true);
    rs.GetRating(53, "cat");
    rs.SetFailRatings(false);
    rs.SetHoldRatings(true);
    rs.GetRating(54, "cat");   // 답 없음(로그인 한도 시험용)
    rs.SetHoldRatings(false);
    rs.TakeAnswers(ans);
    Check(ans.size() == 4 && ans[0].kind == StoreAnswerKind::Rating && ans[0].req == 50 && ans[0].ratingOk && ans[0].ratingFound && ans[0].rating == 1010 &&
              ans[0].rank == 1 && ans[1].rating == 990 && ans[1].rank == 3 && ans[2].ratingOk && !ans[2].ratingFound && ans[2].rating == nox::kRatingStart &&
              ans[2].rank == 0 && ans[3].req == 53 && !ans[3].ratingOk && !ans[3].why.empty(),
          "⑥ 점수 읽기 — cat 1010 · 1위 · bob 990 · 3위 · 없는 이름 1000 · 순위 0 · 실패 스위치 ratingOk false · 붙잡기 스위치는 답 없음");
}

// ── ★ 7단계 조각 5 — 로비 판 저장(`RoomMirror` RESULT → 저장소 → SAVED) ──
//   ① 온전한 판 → 그 자리엔 줄 없음(저장 요청) → 답 → 그 연결에 SAVED ok · 저장소에 (등록 주소 · 포트 · 판 번호)로 들어감
//   ② 같은 판 다시 → dup · 다른 게임 서버의 같은 판 번호 → ok(따로)
//   ③ 못 읽음 — 판 번호 있음 → 곧바로 SAVED bad(저장 요청 없음) · 판 번호도 없음 → 줄 없음 · 갈래마다 셈
//   ④ 저장소 실패 → 줄 없음(게임 서버가 다시) · 답 전에 연결 끊김 → 버림 · 묻지 않은 답 → 버림 · 저장소 없는 거울 → 기록만
void MirrorSaveUnitTests() {
    using namespace nox::wol;
    RoomMirror mirror(LobbyTestKey());
    CMemoryLobbyStore store;
    mirror.AttachStore(store);
    std::vector<Out> out;
    auto reg = [&](int conn, uint32_t ip) {
        out.clear();
        mirror.OnLinkOpen(conn, out);
        nox::LinkLine l;
        nox::ParseLinkLine(out.empty() ? std::string() : out[0].line, l);
        out.clear();
        mirror.OnLinkLine(conn, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), l.text)), out);
        mirror.OnLinkLine(conn, nox::LinkRegister(1760000000u, ip, 18590), out);
    };
    auto result = [&](int conn, const std::string& json) {
        out.clear();
        std::string line;
        nox::LinkResult(json, line);
        mirror.OnLinkLine(conn, line, out);
    };
    // 저장소 답을 꺼내 거울에 넘김(로비 루프 한 자리) — 나온 줄
    auto pump = [&]() {
        std::vector<StoreAnswer> ans;
        store.TakeAnswers(ans);
        out.clear();
        for (const StoreAnswer& a : ans) mirror.OnStoreAnswer(a, out);
        return ans.size();
    };
    auto only = [&](int conn, const std::string& line) { return out.size() == 1 && out[0].conn == conn && !out[0].closeAfter && out[0].line == line; };
    const std::string m1 =
        "{\"match\":\"1760000000-0-1\",\"room\":0,\"roomName\":\"wn's game\",\"map\":\"so_lod\",\"mode\":\"chat\",\"start\":1,\"end\":2,\"frames\":30,"
        "\"sec\":1,\"endReason\":\"score_player\",\"winner\":{\"kind\":\"player\",\"id\":2,\"account\":\"bob\"},\"limit\":1,\"timeLimitSec\":0,"
        "\"players\":[{\"account\":\"wn\",\"netCode\":1,\"state\":\"finished\",\"team\":0,\"score\":0,\"deaths\":1}]}";
    const uint32_t ipA = (10u << 24) | 1u, ipB = (10u << 24) | 2u;
    reg(301, ipA);
    reg(302, ipB);

    // ①
    result(301, m1);
    const bool asked = out.empty() && mirror.PendingSaves() == 1 && mirror.Stats().saveRequests == 1;
    const size_t n1 = pump();
    const nox::MatchRecord* stored = store.FindMatch(MatchSource{ipA, 18590}, "1760000000-0-1");
    Check(asked && n1 == 1 && only(301, "SAVED 1760000000-0-1 ok") && stored && stored->roomName == "wn's game" && stored->players.size() == 1 &&
              mirror.PendingSaves() == 0 && mirror.Stats().savedOk == 1 && mirror.TakeResults().size() == 1,
          "① 온전한 판 → 그 자리 줄 없음 · 답 뒤 그 연결에 SAVED ok · 저장소에 (등록 주소 · 포트 · 판 번호) · 기록용 JSON 그대로");

    // ②
    result(301, m1);
    pump();
    const bool dup = only(301, "SAVED 1760000000-0-1 dup") && mirror.Stats().savedDup == 1;
    result(302, m1);
    pump();
    Check(dup && only(302, "SAVED 1760000000-0-1 ok") && store.MatchCount() == 2 && mirror.Stats().savedOk == 2,
          "② 같은 판 다시 → dup · 다른 게임 서버의 같은 판 번호 → ok(따로 · 저장소 판 둘)");

    // ③
    result(301, "{\"match\":\"1760000000-0-9\",\"room\":\"x\"}");
    const bool bad = only(301, "SAVED 1760000000-0-9 bad") && mirror.PendingSaves() == 0 && mirror.Stats().savedBad == 1 &&
                     mirror.Stats().unreadable[static_cast<size_t>(nox::MatchJsonParse::Type)] == 1;
    result(301, "{\"match\":");
    Check(bad && out.empty() && mirror.Stats().unreadable[static_cast<size_t>(nox::MatchJsonParse::Syntax)] == 1 && mirror.Stats().unreadableNoMatch == 1 &&
              mirror.Stats().UnreadableTotal() == 2 && mirror.Stats().saveRequests == 3 && pump() == 0,
          "③ 못 읽음 — 판 번호 있음(타입) → 곧바로 SAVED bad · 판 번호도 없음(문법) → 줄 없음 · 둘 다 저장 요청 없음");

    // ④
    store.SetFailSaves(true);
    const std::string m2 = std::string(m1).replace(m1.find("0-1\""), 4, "0-2\"");
    result(301, m2);
    pump();
    const bool failQuiet = out.empty() && mirror.Stats().saveFailed == 1;
    store.SetFailSaves(false);
    result(302, m2);
    mirror.OnLinkClose(302, out);
    pump();
    const bool goneQuiet = out.empty() && mirror.Stats().saveConnGone == 1;
    StoreAnswer stray;
    stray.kind = StoreAnswerKind::SaveMatch;
    stray.req = 9999;
    out.clear();
    mirror.OnStoreAnswer(stray, out);
    const bool strayQuiet = out.empty() && mirror.Stats().saveAnswerUnknown == 1;
    RoomMirror bare(LobbyTestKey());   // 저장소를 안 붙임 — 옛 꼴(기록만)
    out.clear();
    bare.OnLinkOpen(401, out);
    nox::LinkLine hl;
    nox::ParseLinkLine(out.empty() ? std::string() : out[0].line, hl);
    out.clear();
    bare.OnLinkLine(401, nox::LinkAuth(nox::LinkAuthSig(nox::LinkAuthKey(LobbyTestKey()), hl.text)), out);
    bare.OnLinkLine(401, nox::LinkRegister(1, ipA, 18590), out);
    std::string line;
    nox::LinkResult(m1, line);
    bare.OnLinkLine(401, line, out);
    Check(failQuiet && goneQuiet && strayQuiet && out.empty() && bare.Stats().noStore == 1 && bare.TakeResults().size() == 1 && mirror.PendingSaves() == 0,
          "④ 저장소 실패 → 줄 없음 · 답 전에 끊김 → 버림 · 묻지 않은 답 → 버림 · 저장소 없는 거울 → 기록만");

    // ⑤ ★ 7단계 조각 7 — 저장소가 못 받는 판(Invalid — 방 이름 65바이트) → SAVED bad(다시 보내도 같다)
    const std::string longRoom = std::string(m1).replace(m1.find("wn's game"), 9, std::string(65, 'r'));
    const auto bad0 = mirror.Stats().savedBad;
    result(301, std::string(longRoom).replace(longRoom.find("0-1\""), 4, "0-7\""));
    pump();
    Check(only(301, "SAVED 1760000000-0-7 bad") && mirror.Stats().saveInvalid == 1 && mirror.Stats().savedBad == bad0 + 1,
          "⑤ 저장소가 못 받는 판(방 이름 65바이트) → SAVED bad · 셈");

    // ⑥ ★ 매칭 손보기 1 — 랭크 판 ok → SAVED 꼬리(점수를 고친 사람마다 이름 · 새 점수 · 증감 — 판 사람 차례) · 같은 판 다시(dup) → 꼬리 없음 ·
    //   일반 판 ok → 꼬리 없음(위 ①) · 두 판째 → 새 점수가 쌓인 값
    const std::string duel =
        "{\"match\":\"1760000000-0-20\",\"room\":0,\"roomName\":\"r\",\"map\":\"estate\",\"mode\":\"arena\",\"start\":1,\"end\":2,\"frames\":30,"
        "\"sec\":1,\"endReason\":\"score_player\",\"winner\":{\"kind\":\"player\",\"id\":2,\"account\":\"bob\"},\"limit\":1,\"timeLimitSec\":0,\"ranked\":true,"
        "\"players\":[{\"account\":\"wn\",\"netCode\":1,\"state\":\"finished\",\"team\":0,\"score\":0,\"deaths\":1},"
        "{\"account\":\"bob\",\"netCode\":2,\"state\":\"finished\",\"team\":0,\"score\":1,\"deaths\":0}]}";
    LobbyConfig watchCfg;
    Lobby watcher(watchCfg, mirror);   // ★ 매칭 손보기 5 — 거울이 SAVED ok 때 로비가 쥔 점수도 고치나(이 시험의 거울엔 여기서부터 붙임)
    mirror.Attach(watcher);
    result(301, duel);
    pump();
    const bool tail1 = only(301, "SAVED 1760000000-0-20 ok wn 990 -10 bob 1010 10");
    const std::string seen1 = out.empty() ? std::string("-") : out[0].line;
    result(301, duel);
    pump();
    const bool dupBare = only(301, "SAVED 1760000000-0-20 dup");
    result(301, std::string(duel).replace(duel.find("0-20\""), 5, "0-21\""));
    pump();
    Check(tail1 && dupBare && only(301, "SAVED 1760000000-0-21 ok wn 980 -10 bob 1020 10"),
          ("⑥ 랭크 판 ok → 꼬리(wn 990 −10 · bob 1010 +10) = " + seen1 + " · 같은 판 dup → 꼬리 없음 · 두 판째 → 쌓인 점수(980 · 1020)").c_str());
    Check(watcher.RatingOf("wn") == 980 && watcher.RatingOf("bob") == 1020, "★ ⑥ 거울이 SAVED ok 때 로비에도 넘김 — 로비가 쥔 점수 wn 980 · bob 1020");
}

// ── ★ 로그인 암호 조각 4 — 로그인 토큰(`Lobby` — `Docs/LOGIN_AUTH_DESIGN.md` 차이 5 · 6 · 정할 것 a · b · c) ──
//   (7단계 조각 6 의 「저장소에 묻고 답 기다리기」 시험을 갈음 — 그 길은 걷어냈다)
//   ① 아이디만(키 없음) — 정품 묶음 → NOXSERVER · 인사 · 376 → 379 → 329 차례 · 평문 apgar 를 안 봄 · 방 참가 토큰
//   ② 암호 판 맞는 토큰 — 같은 차례 · 안내 없음 · 계정 번호 · 방 참가 토큰
//   ③ 토큰 갈래 일곱(크기 · 판 · 서명 · 만료 · 너무 멂 · IP · 이름) — 거절 378(★ 10-28) · 뒷줄 무시 · 다시 쓴 토큰
//   ④ 실패 표시 까닭 다섯(계정 없음은 가입 주소) · 모르는 까닭 · 손님 · 평문(DLL 판 2 · 3) · apgar 없음
//   ⑤ 같은 계정 새 접속 → 옛 접속에 끊는 줄 하나(정할 것 b) · 장부 · 옛 접속이 끊겨도 새 접속은 장부에
//   ⑥ 로그인 실패 접속은 거절 378 · 뒷줄 무시(★ 10-28) · USER 전 접속의 대화방 = 보기만(남에게 들어옴 안 알림 · 구성원 아님 · 글 거름)
//   ⑥b 거절 뒤 클라가 안 끊으면 한도에 끊음
//   ⑦ 이름 17바이트 · 빈 이름 · 로그인 마감 · 두 번째 USER ⑧ 셈 — USER 수 = 갈래 합 · 갈래가 다 나옴
void LoginUnitTests() {
    using namespace nox::wol;
    uint32_t clock = 0xfffff000u;   // 32비트가 넘어가는 자리 근처(마감을 뺄셈으로 재는지)
    nox::JoinKey loginKey = LobbyTestKey();
    loginKey[0] ^= 0x77;            // 입장 토큰 키와 다른 값
    uint64_t nextNonce = 100;
    auto makeCfg = [&](bool auth) {
        LobbyConfig cfg;
        cfg.gameServerIp = 0x7f000001u;
        cfg.joinKey = LobbyTestKey();
        cfg.unixNow = [] { return kLobbyTestNow; };
        cfg.nonce = [] { return 0x0102030405060708ull; };
        cfg.nowMs = [&clock] { return clock; };
        cfg.authOn = auth;
        cfg.loginKey = loginKey;
        return cfg;
    };
    // 로그인 서버가 짓는 것과 같은 토큰(`AuthService.cpp`) — 바꿀 칸만 고쳐 쓴다
    struct Tok {
        uint64_t account = 7;
        uint32_t ip = 0x7f000001u;
        uint32_t expires = kLobbyTestNow + nox::kLoginTokenLifetimeSec;
        uint64_t nonce = 0;
        std::string name = "wn";
        uint8_t version = nox::kLoginTokenVersion;
        bool otherKey = false;
    };
    auto apgar = [&](Tok t) {
        nox::LoginToken lt;
        lt.version = t.version;
        lt.accountId = t.account;
        lt.clientIp = t.ip;
        lt.expiresAt = t.expires;
        lt.nonce = t.nonce ? t.nonce : nextNonce++;
        lt.name = t.name;
        uint8_t raw[nox::kLoginTokenSize];
        nox::SignLoginToken(t.otherKey ? LobbyTestKey() : loginKey, lt, raw);
        return "apgar NXL1" + nox::ToHex(raw, sizeof(raw)) + " 0";
    };
    FakeRooms fake;
    fake.rooms = {FakeRoom(0, "host1")};
    std::vector<Out> out;
    const uint32_t peer = 0x7f000001u;
    auto feed = [&](Lobby& l, int conn, const std::vector<std::string>& ls) {
        out.clear();
        for (const std::string& s : ls) l.OnLine(conn, s, out);
    };
    auto has = [&](const std::string& prefix) {
        for (const Out& x : out)
            if (x.line.rfind(prefix, 0) == 0) return true;
        return false;
    };
    auto lines = [&]() {
        std::vector<std::string> v;
        for (const Out& x : out) v.push_back(x.line);
        return v;
    };
    const std::string user = "USER UserName HostName irc.westwood.com :RealName";
    auto bundle = [&](const std::string& ver, const std::string& nick, const std::string& apgarLine) {
        std::vector<std::string> b = {"NOXCLIENT " + ver, "CVERS 11020 9472", "PASS supersecret", "NICK " + nick};
        if (!apgarLine.empty()) b.push_back(apgarLine);
        b.insert(b.end(), {"SERIAL 7726", user, "verchk 32512 720916", "SETOPT 17,33", "SETCODEPAGE 949"});
        return b;
    };
    // 정품 묶음 답의 차례 — NOXSERVER · 375 · (372 …) · 376 → 379 → 329 · 실패 안내 없음
    auto welcomeOk = [&]() {
        const auto v = lines();
        size_t i376 = v.size();
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == ": 376 u") i376 = i;
        return v.size() >= 5 && v[0] == "NOXSERVER 1" && v[1].rfind(": 375 ", 0) == 0 && i376 + 3 == v.size() &&
               v[i376 + 1].rfind(": 379 ", 0) == 0 && v[i376 + 2] == ": 329 u 949" && !has(": 372 u :- *** Login failed");
    };
    // ★ 10-28 — 실패는 인사(375 · 376) 없이 `378` 한 줄로 끝난다(뒷줄 verchk · SETCODEPAGE 엔 답 없음 · 곧바로 안 끊음)
    auto failWith = [&](const std::string& text) {
        const auto v = lines();
        unsigned closes = 0;
        for (const Out& x : out) closes += x.closeAfter ? 1 : 0;
        return !v.empty() && v.back().rfind(": 378 u :", 0) == 0 && v.back().find(text) != std::string::npos && !has(": 375 ") &&
               !has(": 376 ") && !has(": 372 ") && !has(": 379 ") && !has(": 329 ") && closes == 0;
    };
    unsigned users = 0;   // 첫 USER 수(셈 맞추기)

    // ①
    Lobby plain(makeCfg(false), fake);
    plain.OnConnect(1, peer, peer);
    feed(plain, 1, bundle("2", "wn", "apgar 1234 0"));
    const bool plainOk = welcomeOk() && plain.Stats().loginNoAuth == 1 && plain.Stats().LoginTotal() == 1;
    feed(plain, 1, {"JOINGAME #host1's_game 1 "});
    Check(plainOk && has("NOXTOKEN ") && plain.AccountsOnline() == 0,
          "① 아이디만 — 묶음 차례 376 → 379 → 329 · 평문 apgar 안 봄 · 방 참가 토큰 · 계정 장부 없음");

    Lobby lb(makeCfg(true), fake);
    // ②
    lb.OnConnect(1, peer, peer);
    feed(lb, 1, bundle("3", "wn", apgar(Tok{})));
    ++users;
    const bool tokOk = welcomeOk() && lb.Stats().loginToken[0] == 1 && lb.AccountsOnline() == 1;
    feed(lb, 1, {"JOINGAME #host1's_game 1 "});
    Check(tokOk && has("NOXTOKEN "), "② 맞는 토큰 — 묶음 차례 그대로 · 안내 없음 · 장부 하나 · 방 참가 토큰");

    // ③ 토큰 갈래 — 접속마다 하나
    int conn = 10;
    auto tryLogin = [&](const std::string& ver, const std::string& nick, const std::string& apgarLine, uint32_t from = 0x7f000001u) {
        ++conn;
        lb.OnConnect(conn, from, peer);
        feed(lb, conn, bundle(ver, nick, apgarLine));
        ++users;
        return conn;
    };
    struct Case {
        const char* what;
        Tok t;
        std::string nick;
        uint32_t from;
        std::string raw;   // 비어 있지 않으면 apgar 줄 그대로
    };
    Tok far;
    far.expires = kLobbyTestNow + nox::kLoginTokenMaxAheadSec + 1;
    Tok gone;
    gone.expires = kLobbyTestNow - 1;
    Tok v2;
    v2.version = 2;
    Tok other;
    other.otherKey = true;
    const Case cases[] = {
        {"크기(16진 셋)", Tok{}, "wn", peer, "apgar NXL1abc 0"},
        {"판 2", v2, "wn", peer, ""},
        {"서명(입장 토큰 키)", other, "wn", peer, ""},
        {"만료", gone, "wn", peer, ""},
        {"만료가 너무 멂", far, "wn", peer, ""},
        {"IP 다름", Tok{}, "wn", 0x7f000002u, ""},
        {"이름 다름(남의 토큰)", Tok{}, "eve", peer, ""},
    };
    unsigned rejected = 0;
    for (const Case& c : cases) {
        const int k = tryLogin("3", c.nick, c.raw.empty() ? apgar(c.t) : c.raw, c.from);
        const bool notice = failWith("Login token rejected");
        feed(lb, k, {"JOINGAME #host1's_game 1 "});
        const bool ignored = out.empty();   // ★ 10-28 — 거절한 접속의 뒷줄엔 답 없음(전엔 471)
        rejected += (notice && ignored) ? 1 : 0;
        if (!(notice && ignored)) std::printf("    (③ %s — 거절 378 %d · 뒷줄 무시 %d)\n", c.what, notice, ignored);
    }
    Check(rejected == 7, "③ 토큰 갈래 일곱 — 거절 378 · 방 참가 무시 (" + std::to_string(rejected) + "/7)");
    Tok once;
    once.nonce = 4242;
    once.name = "fay";
    once.account = 8;
    tryLogin("3", "fay", apgar(once));
    const bool firstOk = !has(": 372 u :- *** Login failed");
    tryLogin("3", "fay", apgar(once));
    Check(firstOk && failWith("already used") && lb.Stats().loginReplay == 1, "③ 같은 토큰 두 번 — 처음 들어감 · 둘째 다시 쓴 토큰");

    // ④
    const struct {
        const char* fail;
        const char* text;
    } fails[] = {{"wrong_password", "Wrong password."},
                 {"no_account", "sign up at https://127.0.0.1:8443/signup"},
                 {"server_error", "Login server error"},
                 {"unreachable", "Cannot reach the login server"},
                 {"bad_input", "letters/digits 1-9. Password: 4-8,"},
                 {"key_invalid", "start Nox Online from the launcher again"},   // ★ 런처 P1
                 {"launcher_required", "Start Nox Online from the launcher."},  // ★ 런처 P1
                 {"what", "get the new client patch"}};
    unsigned failShown = 0;
    for (const auto& f : fails) {
        tryLogin("3", "gus", std::string("apgar NXL!") + f.fail + " 0");
        failShown += failWith(f.text) ? 1 : 0;
    }
    tryLogin("3", "gus", "apgar None 0");
    const bool guest = failWith("Guest login is off - sign up at https://127.0.0.1:8443/signup");
    tryLogin("2", "gus", "apgar 1234 0");
    const bool old2 = failWith("too old for password login");
    tryLogin("3", "gus", "apgar 1234 0");
    const bool plain3 = failWith("Login token missing");
    tryLogin("3", "gus", "");
    const bool missing = failWith("Login token missing");
    Check(failShown == 8 && guest && old2 && plain3 && missing,
          "④ 실패 표시 일곱(계정 없음 = 가입 주소) · 모르는 까닭 · 손님 · 평문(DLL 판 2 = 판 낮음 · 판 3 = 토큰 없음) · apgar 없음 — 안내 (" +
              std::to_string(failShown) + "/8)");

    // ⑤ 정할 것 b — 같은 계정(7) 새 접속
    const int k5 = tryLogin("3", "wn", apgar(Tok{}));
    int kicks = 0;
    for (const Out& o : out) kicks += (o.conn == 1 && o.closeAfter && o.line == "ERROR :Closing Link (logged in elsewhere)") ? 1 : 0;
    const bool newIn = !has(": 372 u :- *** Login failed") && lb.Stats().loginKickedOld == 1;
    std::vector<Out> closeOut;
    lb.OnClose(1, closeOut);   // 소켓 루프가 끊는 줄을 보낸 뒤 끊김 사건
    feed(lb, k5, {"JOINGAME #host1's_game 1 "});
    const bool stillIn = has("NOXTOKEN ");
    const size_t online = lb.AccountsOnline();
    lb.OnClose(k5, closeOut);
    Check(kicks == 1 && newIn && stillIn && online == 2 && lb.AccountsOnline() == 1,
          "⑤ 같은 계정 새 접속 → 옛 접속에 끊는 줄 하나 · 새 접속 들어감 · 옛 접속 끊김이 새 접속 장부를 안 지움(계정 7 · 8 → 둘) · 새 접속 끊기면 지움");

    // ⑥ 보기만 — 로그인된 hal 이 대화방에 있고, 실패한 접속(★ 10-28 끊겨 뒷줄 무시) · USER 전 접속(보기만)이 들어옴
    Tok halTok;
    halTok.name = "hal";
    halTok.account = 9;
    const int kHal = tryLogin("3", "hal", apgar(halTok));
    feed(lb, kHal, {"JOIN #Lob_37_0 zot0blat"});
    const int kFailed = tryLogin("3", "ivy", "apgar NXL!wrong_password 0");
    const bool failClosed = failWith("Wrong password.");
    feed(lb, kFailed, {"JOIN #Lob_37_0 zot0blat", "PRIVMSG #Lob_37_0 :hello"});
    const bool failIgnored = out.empty();
    const int kBad = ++conn;   // USER 전(규약 밖 클라)
    lb.OnConnect(kBad, peer, peer);
    feed(lb, kBad, {"NICK ivy", "JOIN #Lob_37_0 zot0blat"});
    bool toHal = false, toSelf = false;
    for (const Out& o : out) {
        toHal = toHal || o.conn == kHal;
        toSelf = toSelf || (o.conn == kBad && o.line == ":ivy!u@h JOIN :0,0 #Lob_37_0");
    }
    const bool selfDone = has(": 366 u #Lob_37_0");
    feed(lb, kBad, {"PRIVMSG #Lob_37_0 :hello"});
    bool spoke = false;
    for (const Out& o : out) spoke = spoke || o.conn == kHal;
    feed(lb, kHal, {"PART #Lob_37_0", "JOIN #Lob_37_0 zot0blat"});
    bool listed = false;
    for (const Out& o : out) listed = listed || (o.line.rfind(": 353 ", 0) == 0 && o.line.find("ivy") != std::string::npos);
    std::vector<Out> preClose;
    lb.OnClose(kBad, preClose);   // ⑦ 로그인 마감 셈에 안 끼게
    Check(failClosed && failIgnored && !toHal && toSelf && selfDone && !spoke && !listed && lb.Stats().chatViewOnly == 1,
          "⑥ 로그인 실패 → 거절 378 · 그 뒷줄(대화방 · 글) 무시 / USER 전 접속의 대화방 = 보기만 — 본인에게만 들어옴 답 · 남에게 안 알림 · 글 거름 · 사람 목록에 없음");

    // ⑥b 거절 뒤 한도 — 클라가 안 끊으면 `kLoginRejectCloseMs` 에 끊는 줄 하나(1ms 앞엔 그대로). 지금까지 거절한 접속은 다 같은 시각이라 같이 끊긴다
    const int kSlow = tryLogin("3", "jay", "apgar NXL!wrong_password 0");
    clock += kLoginRejectCloseMs - 1;
    out.clear();
    lb.Tick(out);
    const bool rejectEarly = !out.empty();
    clock += 1;
    out.clear();
    lb.Tick(out);
    unsigned rejectCloses = 0, slowCloses = 0;
    for (const Out& o : out) {
        const bool closeLine = o.closeAfter && o.line == "ERROR :Closing Link (login failed)";
        rejectCloses += closeLine ? 1 : 0;
        slowCloses += (closeLine && o.conn == kSlow) ? 1 : 0;
    }
    const bool allClosed = rejectCloses == out.size() && lb.Stats().loginRejectClosed == lb.Stats().loginRejected && slowCloses == 1;
    out.clear();
    lb.Tick(out);
    Check(!rejectEarly && allClosed && out.empty(),
          "⑥b 거절 뒤 한도 1ms 앞 그대로 · 한도에 거절한 접속마다 끊는 줄 하나 (" + std::to_string(rejectCloses) + "/" +
              std::to_string(lb.Stats().loginRejected) + ") · 다음 틱엔 다시 안 함");

    // ⑦
    tryLogin("3", "abcdefghijklmnopq", apgar(Tok{}));   // 17바이트
    const bool longName = failWith("Name must be 1-16 bytes");
    const int kDead = ++conn;
    lb.OnConnect(kDead, peer, peer);   // 말없음
    clock += kLoginDeadlineMs - 1;
    out.clear();
    lb.Tick(out);
    bool early = false;
    for (const Out& o : out) early = early || o.conn == kDead;
    clock += 1;
    out.clear();
    lb.Tick(out);
    const bool deadline = out.size() == 1 && out[0].conn == kDead && out[0].closeAfter && lb.Stats().loginDeadlineClosed == 1;
    feed(lb, kHal, {user});
    Check(longName && !early && deadline && out.empty(),
          "⑦ 17바이트 이름 → 안내 · 로그인 마감 1ms 앞 그대로 · 마감에 끊는 줄 하나 · 두 번째 USER → 아무것도 안 함");

    // ⑧ 셈 — 첫 USER 하나 = 갈래 하나 · 토큰 갈래(통과 포함 여덟) · 실패 까닭 일곱이 다 나옴(★ 런처 P1 — 둘 더함)
    const LobbyStats& s = lb.Stats();
    unsigned tokenKinds = 0, failKinds = 0;
    for (const auto v : s.loginToken) tokenKinds += v ? 1 : 0;
    for (const auto v : s.loginFail) failKinds += v ? 1 : 0;
    Check(s.LoginTotal() == users && tokenKinds == static_cast<unsigned>(nox::LoginTokenCheck::kCount) &&
              s.loginToken[static_cast<size_t>(nox::LoginTokenCheck::Ok)] >= 4 && failKinds == static_cast<unsigned>(nox::LoginFail::kCount) &&
              s.loginFailUnknown == 1 && s.loginGuest == 1 && s.loginPlain == 2 && s.loginMissing == 1 && s.loginBadName == 1 && s.loginNoAuth == 0 &&
              s.loginRejected == users - s.loginToken[static_cast<size_t>(nox::LoginTokenCheck::Ok)],
          "⑧ 셈 — 로그인 " + std::to_string(s.LoginTotal()) + " = 첫 USER " + std::to_string(users) +
              " · 토큰 거절 갈래 일곱 · 실패 까닭 일곱 · 모르는 1 · 손님 1 · 평문 2 · 없음 1 · 이름 1 · 거절 378 " + std::to_string(s.loginRejected) +
              " = USER − 통과");

    // ⑨ ★ 레이팅 조각 4 — 저장소를 붙인 로비. 토큰 통과 → 점수 물음 → 답까지 인사를 미루고 뒷줄은 쌓음 →
    //   375 · (372 …) · 372 점수 줄(ASCII — ★ 10-30 한글은 정품에서 깨짐) · 376 → 379 → 329. 못 읽음 · 시간 초과 = 점수 줄 없이 · 끊김 · 늦은 답 · 쌓기 한도
    {
        CMemoryLobbyStore st;
        nox::MatchRecord m;   // 랭크 판 하나 — wn 이김(1010) · bob 짐(990)
        m.match = "r-1";
        m.ranked = true;
        m.endReason = "score_player";
        m.winnerKind = "player";
        m.hasWinnerId = true;
        m.players.resize(2);
        m.players[0].hasAccount = m.players[1].hasAccount = true;
        m.players[0].account = "wn";
        m.players[0].score = 1;
        m.players[1].account = "bob";
        st.SaveMatch(1, MatchSource{1, 1}, m, "{}");
        std::vector<StoreAnswer> ans;
        st.TakeAnswers(ans);
        Lobby rl(makeCfg(true), fake);
        rl.AttachStore(&st);
        auto pump = [&]() {   // 저장소 답을 로비로(소켓 루프의 한 바퀴)
            ans.clear();
            st.TakeAnswers(ans);
            out.clear();
            for (const StoreAnswer& a : ans) rl.OnRatingAnswer(a, out);
        };
        // 인사 차례 — 375 · … · 점수 줄(있으면 376 바로 앞) · 376 → 379 → 329
        auto greeted = [&](const std::string& ratingBytes) {
            const auto v = lines();
            size_t i376 = v.size();
            for (size_t i = 0; i < v.size(); ++i)
                if (v[i] == ": 376 u") i376 = i;
            if (v.empty() || v[0].rfind(": 375 ", 0) != 0 || i376 == v.size() || i376 + 3 != v.size() || v[i376 + 1].rfind(": 379 ", 0) != 0 ||
                v[i376 + 2] != ": 329 u 949")
                return false;
            const bool hasRating = i376 >= 1 && v[i376 - 1] == ": 372 u :- " + ratingBytes;
            bool anyRating = false;
            for (const auto& l : v) anyRating = anyRating || l.find(":- Rating ") != std::string::npos;
            return ratingBytes.empty() ? !anyRating : hasRating;
        };
        // 영문(ASCII) — ★ 10-30 사람 확인 59 한글 공지 줄이 정품에서 깨짐(`Lobby.cpp` `OnRatingAnswer`)
        auto ratingLine = [&](const char* score, const char* rank) { return std::string("Rating ") + score + " - Rank #" + rank; };

        // a) wn — 묶음을 다 보내도 NOXSERVER 만(뒷줄 셋은 쌓임) → 답 → 점수 줄 「내 점수 1010 · 랭크 1위」
        rl.OnConnect(1, peer, peer);
        feed(rl, 1, bundle("3", "wn", apgar(Tok{})));
        const bool waitedA = lines() == std::vector<std::string>{"NOXSERVER 1"} && rl.PendingRatings() == 1 && rl.Stats().ratingAsked == 1;
        pump();
        Check(waitedA && greeted(ratingLine("1010", "1")) && rl.PendingRatings() == 0 && rl.Stats().heldLines == 3,
              "⑨ 점수 기다림 — 답 전엔 NOXSERVER 만 · 답 뒤 375 … 「Rating 1010 - Rank #1」 · 376 → 379 → 329 · 쌓은 뒷줄 3");
        // b) bob — 990 · 2위
        rl.OnConnect(2, peer, peer);
        feed(rl, 2, bundle("3", "bob", apgar(Tok{8, peer, kLobbyTestNow + nox::kLoginTokenLifetimeSec, 0, "bob"})));
        pump();
        Check(greeted(ratingLine("990", "2")), "⑨ 점수 줄 「Rating 990 - Rank #2」");
        // c) cat — 랭크 판 없음
        rl.OnConnect(3, peer, peer);
        feed(rl, 3, bundle("3", "cat", apgar(Tok{9, peer, kLobbyTestNow + nox::kLoginTokenLifetimeSec, 0, "cat"})));
        pump();
        Check(greeted("Rating 1000 - No ranked games yet"), "⑨ 랭크 판 없음 줄 「Rating 1000 - No ranked games yet」");
        // d) 못 읽음 — 점수 줄 없이 376(로그인은 됨)
        st.SetFailRatings(true);
        rl.OnConnect(4, peer, peer);
        feed(rl, 4, bundle("3", "dan", apgar(Tok{10, peer, kLobbyTestNow + nox::kLoginTokenLifetimeSec, 0, "dan"})));
        pump();
        st.SetFailRatings(false);
        Check(greeted(std::string()) && rl.Stats().ratingFailed == 1, "⑨ 저장소 못 읽음 — 점수 줄 없이 376 → 379 → 329");
        // e) 시간 초과 — 한도 1ms 앞 그대로 · 한도에 점수 줄 없이 376 · 그 뒤 온 답은 늦은 답
        st.SetHoldRatings(true);
        rl.OnConnect(5, peer, peer);
        feed(rl, 5, bundle("3", "eve", apgar(Tok{11, peer, kLobbyTestNow + nox::kLoginTokenLifetimeSec, 0, "eve"})));
        st.SetHoldRatings(false);
        clock += kRatingTimeoutMs - 1;
        out.clear();
        rl.Tick(out);
        const bool notYet = out.empty() && rl.PendingRatings() == 1;
        clock += 1;
        out.clear();
        rl.Tick(out);
        const bool timedOut = greeted(std::string()) && rl.Stats().ratingTimeout == 1 && rl.PendingRatings() == 0;
        out.clear();
        rl.OnRatingAnswer(StoreAnswer{StoreAnswerKind::Rating, 5}, out);   // 끝난 물음(요청 5 는 eve 의 번호)
        Check(notYet && timedOut && out.empty() && rl.Stats().ratingLate == 1,
              "⑨ 시간 초과 — 1ms 앞 그대로 · 한도에 점수 줄 없이 376 → 379 → 329 · 늦은 답 버림");
        // f) 기다리는 동안 끊김 → 답은 늦은 답 · 쌓기 한도
        rl.OnConnect(6, peer, peer);
        feed(rl, 6, bundle("3", "fay", apgar(Tok{12, peer, kLobbyTestNow + nox::kLoginTokenLifetimeSec, 0, "fay"})));
        std::vector<std::string> flood(kMaxHeldLines, "VERCHK 32512 720916");
        feed(rl, 6, flood);
        const bool dropped = rl.Stats().heldDropped == 3;   // 묶음 뒷줄 3 + 64 → 한도 64 를 넘은 3
        rl.OnClose(6, out);
        pump();
        const LobbyStats& rs = rl.Stats();
        Check(dropped && out.empty() && rs.ratingClosed == 1 && rs.ratingLate == 2 && rl.PendingRatings() == 0,
              "⑨ 기다리는 동안 끊김 — 답 버림(늦은 답) · 쌓기 한도 64 넘은 줄 버림");
        Check(rs.ratingAsked == 6 && rs.RatingTotal() == rs.ratingAsked && rs.ratingShown == 2 && rs.ratingNoGames == 1 && rs.ratingFailed == 1 &&
                  rs.ratingTimeout == 1 && rs.ratingClosed == 1,
              "⑨ 셈 — 물음 6 = 점수 줄 2 · 판 없음 1 · 못 읽음 1 · 시간 초과 1 · 끊김 1");
    }
}

// ── 라우터 단위 시험(작업 8b) — 605 의 채널 주소 갈래 둘 · QUIT 뒤 끊기 · 모르는 명령 ──
void RouterUnitTests() {
    using namespace nox::wol;
    const uint32_t loop = 0x7f000001;
    const uint32_t peer = (198u << 24) | (51u << 16) | (100u << 8) | 7u;   // 198.51.100.7 (문서용 주소)
    const char* const five[] = {"verchk 32512 65556", "verchk 9472 65538", "lobcount 9472", "whereto wn 1234 9472 65538 7726", "QUIT"};
    std::vector<Out> out;

    // 채널 주소 설정 없음 — 「클라가 붙은 내 쪽 주소」:4000, QUIT 뒤엔 끊는다(옛 로비 4005 답과 같다)
    Router a{RouterConfig{}};
    a.OnConnect(1, peer, loop);
    for (const char* l : five) a.OnLine(1, l, out);
    const std::vector<std::string> wantA = {": 610 u 1", ": 605 u :127.0.0.1 4000 '0:Nox_Online' -8 36.1083 -115.0582", ": 607"};
    Check(Lines(out) == wantA, "라우터 4005 답 = 610 · 605(내 쪽 주소 127.0.0.1:4000) · 607");
    Check(!out.empty() && out.back().closeAfter && !out.front().closeAfter, "QUIT 답 뒤에만 끊는다");
    a.OnClose(1);

    // 채널 주소 설정 있음 — 그 주소 · 포트 · 이름
    RouterConfig c;
    c.channelIp = (192u << 24) | (0u << 16) | (2u << 8) | 10u;   // 192.0.2.10 (문서용 주소)
    c.channelPort = 4100;
    c.serverName = "Chan";
    Router b{c};
    out.clear();
    b.OnConnect(7, peer, loop);
    b.OnLine(7, "whereto wn 1234 9472 65538 7726", out);
    Check(Lines(out) == std::vector<std::string>{": 605 u :192.0.2.10 4100 '0:Chan' -8 36.1083 -115.0582"},
          "채널 주소를 설정하면 605 = 그 주소 · 포트 · 이름");

    // 모르는 명령 — 답 없음, 처음 한 번만 기록 · 모르는 접속 번호는 무시
    out.clear();
    b.OnLine(7, "PING abc", out);
    b.OnLine(7, "ping def", out);
    b.OnLine(99, "lobcount 9472", out);
    Check(out.empty() && b.TakeLog().size() == 1, "모르는 명령은 답 없이 처음 한 번만 기록 · 모르는 접속은 무시");
}

// ── 녹음 대조 ───────────────────────────────────────────────────────

// 중계기가 적은 UTF-8 줄 → 원래 바이트. U+00FF 를 넘는 글자(한글 로그 문구 등)는 '?' 로.
std::string Utf8ToLatin1(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            const uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            out.push_back(cp <= 0xFF ? static_cast<char>(cp) : '?');
            i += 2;
        } else {
            out.push_back('?');
            ++i;
            while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) ++i;
        }
    }
    return out;
}

// 빈칸으로 앞 n 칸을 자르고 나머지는 통째로 마지막 칸 — 방 정보 조각엔 빈칸(0x20)이 없다.
std::vector<std::string> SplitN(const std::string& s, size_t n) {
    std::vector<std::string> t;
    size_t p = 0;
    while (t.size() + 1 < n) {
        const size_t q = s.find(' ', p);
        if (q == std::string::npos) break;
        t.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    t.push_back(s.substr(p));
    return t;
}

// 칸으로 풀었다 다시 채운 날것이 원래와 다른 자리가 「글자 칸(맵 11-19 · 이름 20-34)의 첫 0 뒤」뿐인가.
//   글자 칸은 첫 0 앞까지만 읽으므로 그 뒤 찌꺼기는 사라지는 게 맞다(녹음 quiteplaz 방 맵 칸 "gather\0h\0").
bool OnlyTextTailDiffers(const nox::wol::Raw& orig, const nox::wol::Raw& re, bool& anyDiff) {
    anyDiff = false;
    auto tailStart = [&](size_t from, size_t cap) {
        size_t i = from;
        while (i < from + cap && orig[i] != 0) ++i;
        return i;
    };
    const size_t mapTail = tailStart(11, 9), nameTail = tailStart(20, 15);
    for (size_t i = 0; i < orig.size(); ++i) {
        if (orig[i] == re[i]) continue;
        anyDiff = true;
        const bool inMapTail = i >= mapTail && i < 20;
        const bool inNameTail = i >= nameTail && i < 35;
        if (!inMapTail && !inNameTail) return false;
    }
    return true;
}

bool Printable(const std::string& s) {
    for (char c : s)
        if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) > 0x7e) return false;
    return true;
}

int GameInfoFromRecording(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("[실패] 녹음을 열 수 없다: %s\n", path);
        return 1;
    }
    int lines = 0, xwisLines = 0, blobs = 0, same = 0, tailJunk = 0;
    std::string line;
    while (std::getline(f, line)) {
        ++lines;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t at = line.find("] << ");
        if (at == std::string::npos) continue;
        ++xwisLines;
        const std::string body = Utf8ToLatin1(line.substr(at + 5));
        const auto head = SplitN(body, 3);
        if (head.size() < 3) continue;
        std::string payload, room;
        if (head[1] == "326") {
            const auto t = SplitN(body, 11);   // ": 326 u #방 인원 0 37 1 ? IP 128::G1P3…"
            if (t.size() < 11) continue;
            room = t[3];
            if (t[10].compare(0, nox::wol::kListPrefix.size(), nox::wol::kListPrefix) != 0) {
                Check(false, "326 방 정보 앞에 128: 이 있다");
                continue;
            }
            payload = t[10].substr(nox::wol::kListPrefix.size());
        } else if (head[1] == "332") {
            const auto t = SplitN(body, 5);    // ": 332 u #방 :G1P3…"
            if (t.size() < 5) continue;
            room = t[3];
            payload = t[4];
        } else {
            continue;
        }
        ++blobs;
        nox::wol::Raw raw;
        const bool ok = nox::wol::UnpackRaw(payload, raw);
        if (!ok) {
            std::printf("  %-4s %-22s 풀지 못함\n", head[1].c_str(), room.c_str());
            Check(false, "녹음 덩어리를 풀 수 있다");
            continue;
        }
        const nox::wol::GameInfo g = nox::wol::Unmarshal(raw);
        std::printf("  %-4s %-22s 맵 %-9s 이름 %-15s 인원 %u/%u 깃발 0x%04x\n", head[1].c_str(), room.c_str(),
                    g.map.c_str(), g.name.c_str(), g.players, g.maxPlayers, g.flags);
        // ① 포장만 — 날것을 다시 포장하면 녹음과 바이트까지 같아야 한다
        const bool eq = nox::wol::PackRaw(raw) == payload;
        Check(eq, "날것을 다시 포장하면 녹음과 바이트가 같다");
        if (eq) ++same;
        // ② 칸을 거친 왕복 — 다른 자리는 글자 칸의 0 뒤 찌꺼기뿐이어야 한다
        bool anyDiff = false;
        Check(OnlyTextTailDiffers(raw, nox::wol::Marshal(g), anyDiff), "칸 왕복에서 달라지는 건 글자 칸 0 뒤뿐");
        if (anyDiff) {
            ++tailJunk;
            std::printf("       ↳ 글자 칸 0 뒤 찌꺼기 — 칸으로 풀면 사라진다\n");
        }
        // 왕복만으로는 「서로 역인 틀린 변환」을 못 거른다 — 풀린 값이 말이 되는지도 본다
        Check(Printable(g.map) && !g.map.empty(), "풀린 맵 이름이 글자다");
        Check(Printable(g.name) && !g.name.empty(), "풀린 방 이름이 글자다");
        Check(g.maxPlayers >= 1 && g.maxPlayers <= 32 && g.players <= g.maxPlayers, "인원 칸이 0~32 범위");
        Check(g.unk2 == 0x489e, "unk2 가 녹음 공통값 0x489e");
    }
    // 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 지나간 양을 같이 찍는다
    std::printf("녹음 %d줄 · XWIS 원래 줄 %d · 방 정보 덩어리 %d · 포장 바이트 같음 %d · 글자 칸 찌꺼기 %d\n", lines,
                xwisLines, blobs, same, tailJunk);
    Check(blobs > 0, "녹음에서 방 정보 덩어리를 하나 이상 찾았다");
    return 0;
}

// ── 녹음 되돌리기 — 정품 클라 줄을 로비에 넣고 XWIS 원래 답과 접속 단위로 대조 ──────────

struct RecConn {
    int id = 0;
    int port = 0;
    std::vector<std::string> client;   // `>>` 정품 클라가 보낸 줄
    std::vector<std::string> xwis;     // `<<` XWIS 원래 줄 (`<~` 바꿔 끼운 줄은 뺀다)
};

// 답 한 줄의 종류 — `: 376 u` → "376", `:wn!u@h JOIN …` → "user:JOIN"
std::string Kind(const std::string& l) {
    const auto t = Tokens(l);
    if (t.size() < 2) return "?" + l;
    if (t[0] == ":") return t[1];
    if (!t[0].empty() && t[0][0] == ':') return "user:" + t[1];
    return "?" + t[0];
}

struct Item {
    std::string kind;
    std::vector<std::string> lines;   // 목록 묶음이면 그 안 줄들, 아니면 그 줄 하나
};

// 대조 단위로 묶는다 — 인사(372)는 줄 수가 달라도 되니 하나로, 목록(321…323)은 「안에 든 종류」로.
// XWIS 의 `PAGE`(참가 직후 한 줄)는 로비가 일부러 안 보내므로 뺀다(LOBBY_DESIGN.md).
std::vector<Item> Normalize(const std::vector<std::string>& lines, int& droppedPage) {
    std::vector<Item> r;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string k = Kind(lines[i]);
        if (k == "user:PAGE") {
            ++droppedPage;
            continue;
        }
        if (k == "372") {
            if (r.empty() || r.back().kind != "372*") r.push_back({"372*", {lines[i]}});
            continue;
        }
        if (k == "321") {
            Item it;
            std::set<std::string> kinds;
            size_t j = i + 1;
            for (; j < lines.size() && Kind(lines[j]) != "323"; ++j) {
                kinds.insert(Kind(lines[j]));
                it.lines.push_back(lines[j]);
            }
            it.kind = "LIST{";
            for (const auto& s : kinds) it.kind += s + (s == *kinds.rbegin() ? "" : ",");
            it.kind += "}";
            r.push_back(it);
            i = j;   // 323 까지 건너뛴다
            continue;
        }
        r.push_back({k, {lines[i]}});
    }
    return r;
}

// 353 이름 목록 칸 `@?이름,0,숫자` 꼴인가
bool MembersShape(const std::string& l) {
    const size_t at = l.find(" :");
    if (at == std::string::npos) return false;
    for (const auto& e : Tokens(l.substr(at + 2))) {
        const size_t c1 = e.find(",0,");
        if (c1 == std::string::npos || c1 == 0 || c1 + 3 >= e.size()) return false;
        for (size_t k = c1 + 3; k < e.size(); ++k)
            if (e[k] < '0' || e[k] > '9') return false;
    }
    return true;
}

// 같은 종류끼리 꼴 대조 — 칸 수가 정해진 답은 칸 수, 353 은 구분 칸과 이름 목록 꼴, 목록은 안의 줄 칸 수
int ShapeDiffs(const Item& ours, const Item& xw) {
    static const std::set<std::string> kFixed = {"610", "605", "607", "376", "379", "329", "328", "366",
                                                 "user:JOIN", "user:PART", "user:JOINGAME", "332", "375"};
    // ★ 10-30 — 375 의 끝 칸(인사말)은 자유 글 — 낱말 수가 아니라 `:- ` 뒤에 글이 있는지만 본다(인사말 「Nox Online」 으로 낱말이 늘어 걸림)
    const auto fixedTokens = [](const Item& it) {
        std::string l = it.lines[0];
        if (it.kind == "375") {
            const size_t at = l.find(" :- ");
            if (at != std::string::npos && at + 4 < l.size()) l = l.substr(0, at + 3) + " _";
        }
        return Tokens(l).size();
    };
    int d = 0;
    if (kFixed.count(ours.kind) && fixedTokens(ours) != fixedTokens(xw)) {
        std::printf("    칸 수 다름 [%s]\n      우리 %s\n      XWIS %s\n", ours.kind.c_str(), ours.lines[0].c_str(),
                    xw.lines[0].c_str());
        ++d;
    }
    if (ours.kind == "353") {
        const auto a = Tokens(ours.lines[0]), b = Tokens(xw.lines[0]);
        if (a.size() < 4 || b.size() < 4 || a[3] != b[3] || !MembersShape(ours.lines[0])) {
            std::printf("    353 꼴 다름\n      우리 %s\n      XWIS %s\n", ours.lines[0].c_str(), xw.lines[0].c_str());
            ++d;
        }
    }
    if (ours.kind.rfind("LIST{", 0) == 0) {
        for (const auto& ol : ours.lines)
            for (const auto& xl : xw.lines)
                if (Kind(ol) == Kind(xl) && Tokens(ol).size() != Tokens(xl).size()) {
                    std::printf("    목록 줄 칸 수 다름\n      우리 %s\n      XWIS %s\n", ol.c_str(), xl.c_str());
                    ++d;
                    break;
                }
    }
    return d;
}

int ReplayRecording(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("[실패] 녹음을 열 수 없다: %s\n", path);
        return 1;
    }
    std::vector<RecConn> conns;
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) {
        ++lines;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t b = line.find("[c");
        if (b == std::string::npos) continue;
        const size_t colon = line.find(':', b), close = line.find(']', b);
        if (colon == std::string::npos || close == std::string::npos || colon > close) continue;
        const int id = std::atoi(line.substr(b + 2, colon - b - 2).c_str());
        const int port = std::atoi(line.substr(colon + 1, close - colon - 1).c_str());
        const std::string rest = line.substr(close + 1);
        RecConn* rc = nullptr;
        for (auto& c : conns)
            if (c.id == id) rc = &c;
        if (!rc) {
            conns.push_back({id, port, {}, {}});
            rc = &conns.back();
        }
        if (rest.rfind(" >> ", 0) == 0 && rest.find("(partial)") == std::string::npos)
            rc->client.push_back(Utf8ToLatin1(rest.substr(4)));
        else if (rest.rfind(" << ", 0) == 0 && rest.find("(partial)") == std::string::npos)
            rc->xwis.push_back(Utf8ToLatin1(rest.substr(4)));
    }

    // 녹음에서 정품 클라가 고른 방은 `#tak7's_game` — 방 하나의 방장 이름을 그것으로 맞춰 둔다
    // ★ 10-10 토큰 작업 3 — 로비는 토큰 방식 하나뿐이다. 녹음 속 클라에는 DLL 이 없었으므로, 실제 클라처럼 DLL 몫을 흉내 낸다:
    //   4000 로그인 앞에 `NOXCLIENT 1` 을 끼우고, 나온 답에서 `NOXTOKEN` 줄을 떼어 센 뒤(녹음 속 방 참가 수와 같아야 한다)
    //   나머지만 XWIS 원래 답과 견준다. 그래서 이 대조는 「DLL 이 줄을 뗀 뒤 정품 로비 모듈이 받는 것」을 본다.
    nox::wol::LobbyConfig cfg;
    cfg.gameServerIp = 0x7f000001;   // 옛 로비 기본값(따로 뜬 로비 — 127.0.0.1) 그대로 · ★ 10-14 작업 8d 기본값은 0(붙은 주소)
    FakeRooms fake;
    fake.rooms = {FakeRoom(0, "tak7")};
    cfg.joinKey = LobbyTestKey();
    nox::wol::Lobby lb(cfg, fake);
    // ★ 로그인 암호 조각 4 — 저장소에 묻는 로그인을 걷어냄. 녹음 속 클라는 평문 apgar 라 아이디만(`authOn` false — 키 없는 로비)으로 대조한다
    //   (암호 판 로비의 줄 차례는 `LoginUnitTests` ② 가 이 녹음과 같은 묶음 꼴로 본다)
    // ★ 10-14 작업 8b — 4005 접속(로비 찾기)은 라우터가 받는다. 채널 주소 설정이 없으니 605 는 「붙은 주소」:4000(옛 로비와 같은 답)
    nox::wol::Router rt{nox::wol::RouterConfig{}};
    int tokenLines = 0, joinGames = 0, routerConns = 0;
    const uint32_t loop = 0x7f000001;   // 녹음의 정품 클라는 127.0.0.1 에서 붙었다

    int compared = 0, skipped = 0, items = 0, diffs = 0, droppedPage = 0;
    for (const auto& rc : conns) {
        bool login = false, invalid = false;
        for (const auto& x : rc.xwis) {
            login = login || Kind(x) == "376";
            invalid = invalid || x.find("Invalid Password") != std::string::npos;
        }
        if (rc.port == nox::wol::kLobbyPort && !login) {
            std::printf("  c%d:%d 건너뜀 — XWIS 가 로그인에 답하지 않음(클라 줄 %zu)\n", rc.id, rc.port, rc.client.size());
            ++skipped;
            continue;
        }
        if (invalid) {
            std::printf("  c%d:%d 건너뜀 — 손님 로그인, XWIS 가 제한 모드로 답함(Invalid Password)\n", rc.id, rc.port);
            ++skipped;
            continue;
        }
        std::vector<nox::wol::Out> out;
        if (rc.port == nox::wol::kServServPort) {
            ++routerConns;
            rt.OnConnect(rc.id, loop, loop);
            for (const auto& cl : rc.client) rt.OnLine(rc.id, cl, out);
            rt.OnClose(rc.id);
        } else {
            lb.OnConnect(rc.id, loop, loop);
            if (rc.port == nox::wol::kLobbyPort) lb.OnLine(rc.id, "NOXCLIENT 1", out);   // DLL 이 로그인 앞에 끼우는 줄
            for (const auto& cl : rc.client) {
                lb.OnLine(rc.id, cl, out);
                if (cl.rfind("JOINGAME ", 0) == 0) ++joinGames;
            }
            lb.OnClose(rc.id, out);   // ★ 10-20 v2 작업 3b — 대화방 남은 사람에게 PART(녹음은 접속을 하나씩 돌려 남은 사람 없음)
        }
        std::vector<std::string> kept;   // DLL 이 `NOXTOKEN` 줄을 뗀 뒤 정품 로비 모듈이 받는 줄
        for (const auto& l : Lines(out)) {
            if (l.rfind("NOXTOKEN ", 0) == 0) ++tokenLines;
            else kept.push_back(l);
        }
        int oursPage = 0;
        const auto ours = Normalize(kept, oursPage);
        const auto xw = Normalize(rc.xwis, droppedPage);
        int d = oursPage;   // 로비는 PAGE 를 보내지 않는다 — 보냈다면 다름
        if (ours.size() != xw.size()) ++d;
        for (size_t i = 0; i < ours.size() && i < xw.size(); ++i) {
            if (ours[i].kind != xw[i].kind) {
                ++d;
                continue;
            }
            d += ShapeDiffs(ours[i], xw[i]);
        }
        ++compared;
        items += static_cast<int>(xw.size());
        diffs += d;
        std::printf("  c%d:%d 클라 줄 %zu · 답 묶음 우리 %zu / XWIS %zu — %s\n", rc.id, rc.port, rc.client.size(),
                    ours.size(), xw.size(), d ? "다름" : "같음");
        if (d) {
            std::printf("    우리:");
            for (const auto& it : ours) std::printf(" %s", it.kind.c_str());
            std::printf("\n    XWIS:");
            for (const auto& it : xw) std::printf(" %s", it.kind.c_str());
            std::printf("\n");
        }
    }
    for (const auto& l : lb.TakeLog()) std::printf("  로비 기록: %s\n", l.c_str());
    for (const auto& l : rt.TakeLog()) std::printf("  라우터 기록: %s\n", l.c_str());
    // 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 지나간 양을 같이 찍는다
    std::printf("녹음 %d줄 · 접속 %zu (대조 %d · 건너뜀 %d · 그 가운데 라우터 %d) · 대조한 답 묶음 %d · 일부러 뺀 PAGE %d · 다름 %d\n",
                lines, conns.size(), compared, skipped, routerConns, items, droppedPage, diffs);
    Check(compared > 0 && items > 0, "녹음에서 대조할 접속과 답을 찾았다");
    Check(routerConns == 5, "녹음의 4005 접속 다섯을 라우터가 받았다");
    Check(diffs == 0, "로비 답이 XWIS 원래 답과 종류·순서·꼴이 같다(DLL 이 NOXTOKEN 을 뗀 뒤)");
    std::printf("녹음 속 방 참가 %d · 낸 NOXTOKEN %d\n", joinGames, tokenLines);
    Check(joinGames > 0 && tokenLines == joinGames, "녹음 속 방 참가마다 NOXTOKEN 하나");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--gameinfo") == 0) {
        if (GameInfoFromRecording(argv[2]) != 0) return 1;
    } else if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) {
        if (ReplayRecording(argv[2]) != 0) return 1;
    } else {
        UnitTests();
        LobbyUnitTests();
        ChannelLobbyUnitTests();
        NewRoomUnitTests();
        PendingRoomUnitTests();   // ★ 10-20 v2 작업 3a
        RankQueueUnitTests();     // ★ 랭크 매칭 조각 4
        RankAcceptUnitTests();    // ★ 매칭 손보기 6
        RankReadyUnitTests();     // ★ 10-31 다 수락 뒤 방 요청
        MirrorUnitTests();        // ★ 10-20 v2 작업 3a-2
        LinkAliveUnitTests();     // ★ 7단계 조각 2
        LobbyStoreUnitTests();    // ★ 7단계 조각 4
        MirrorSaveUnitTests();    // ★ 7단계 조각 5
        LoginUnitTests();         // ★ 7단계 조각 6
        ChatUnitTests();          // ★ 10-20 v2 작업 3b
        RouterUnitTests();
    }
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
