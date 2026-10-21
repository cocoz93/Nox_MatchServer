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
    std::vector<nox::wol::LobbyRoom> LobbyRooms() const override { return rooms; }
    nox::wol::RoomRequest RequestRoom(uint32_t req, const std::string& creator, nox::wol::LobbyRoom& out, std::string& why) override {
        if (ask) {
            asked.push_back(req);
            return nox::wol::RoomRequest::Pending;
        }
        if (!canCreate) {
            why = "가짜 — 못 만듦";
            return nox::wol::RoomRequest::Failed;
        }
        out = Make(creator);
        rooms.push_back(out);
        return nox::wol::RoomRequest::Created;
    }
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
    Check(!out.empty() && out.back().line == ": 376 u" && hasLine(out, ": 372 u :- *** Nox_Origin client patch") && !anyClose(out) &&
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
    Check(!out.empty() && out.back().line == ": 376 u" && hasLine(out, ": 372 u :- *** Nox_Origin client patch") && !anyClose(out) &&
              lb.Stats().loginOldClient == 1 && lb.Stats().loginNoClient == 1,
          "DLL 판 0 → 「판 낮음」으로 따로 세고 인사말 안내");
    out.clear();
    lb.OnConnect(5, peer, loop);
    for (const char* l : {"NICK early", "JOINGAME #NoxOrigin's_game 1 "}) lb.OnLine(5, l, out);
    Check(out.size() == 4 && !hasLine(out, "NOXTOKEN") && lb.Stats().joinNoClient == 2 && lb.Stats().tokensIssued == 1,
          "로그인 전 방 참가 → 원래 답 넷 · 토큰 없음");
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
    Check(chans.size() == 5 && chans[0] == "#Lob_37_0=1" && chans[3] == "#Lob_37_3=1" && chans[4] == "#Lob_37_4=0",
          "목록의 대화방 다섯 · 사람 수 칸 = 그 채널 사람 수");
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
//   ① 답은 다음 `TakeAnswers` 에(부를 때 곧바로 안 남) · 갈래 · 요청 번호 그대로
//   ② 로그인 — 처음 이름 → 만듦(번호 1) · 같은 이름 → 같은 번호 · 대소문자 다르면 다른 계정(바이트 그대로) · 949 바이트 이름
//   ③ 판 저장 — 새 판 Saved · 같은 (게임 서버 · 판 번호) Duplicate · 다른 게임 서버의 같은 판 번호는 Saved(따로) · 넣은 칸 그대로
//   ④ 실패 스위치 — 로그인 · 판 저장 Failed(까닭 글) · 실패한 판은 안 넣음 · 끄면 다시 됨
void LobbyStoreUnitTests() {
    using namespace nox::wol;
    CMemoryLobbyStore st;
    std::vector<StoreAnswer> ans;
    st.CheckLogin(7, "wn");
    const bool notYet = st.AccountCount() == 1;   // 계정은 곧바로 만들지만 답은 다음 꺼내기에
    st.TakeAnswers(ans);
    Check(notYet && ans.size() == 1 && ans[0].kind == StoreAnswerKind::Login && ans[0].req == 7 && ans[0].login == LoginResult::Ok &&
              ans[0].created && ans[0].accountId == 1,
          "① 답은 꺼낼 때 · 갈래 Login · 요청 번호 7 · 처음 이름 → 만듦(번호 1)");
    ans.clear();
    st.TakeAnswers(ans);
    const bool emptied = ans.empty();
    st.CheckLogin(8, "wn");
    st.CheckLogin(9, "Wn");
    st.CheckLogin(10, std::string("\xb0\xc7", 2));
    st.TakeAnswers(ans);
    Check(emptied && ans.size() == 3 && ans[0].req == 8 && !ans[0].created && ans[0].accountId == 1 && ans[1].created && ans[1].accountId == 2 &&
              ans[2].created && ans[2].accountId == 3 && st.AccountId(std::string("\xb0\xc7", 2)) == 3 && st.AccountId("x") == 0,
          "② 꺼낸 뒤 빔 · 같은 이름 같은 번호 · 대소문자 다르면 다른 계정 · 949 바이트 이름 · 없는 이름 0");

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
    st.TakeAnswers(ans);
    const nox::MatchRecord* got = st.FindMatch(a, "1760000000-0-1");
    Check(ans.size() == 3 && ans[0].kind == StoreAnswerKind::SaveMatch && ans[0].req == 1 && ans[0].save == SaveResult::Saved &&
              ans[1].save == SaveResult::Duplicate && ans[2].save == SaveResult::Saved && st.MatchCount() == 2 && got &&
              got->roomName == "wn's game" && got->players.size() == 2 && got->players[0].account == "wn" && !st.FindMatch(b, "x"),
          "③ 새 판 Saved · 같은 게임 서버 같은 판 Duplicate · 다른 게임 서버 같은 판 번호 Saved · 넣은 칸 그대로");

    ans.clear();
    st.SetFailLogins(true);
    st.SetFailSaves(true);
    st.CheckLogin(20, "new");
    rec.match = "1760000000-0-2";
    st.SaveMatch(21, a, rec, "{}");
    st.TakeAnswers(ans);
    const bool failed = ans.size() == 2 && ans[0].login == LoginResult::Failed && !ans[0].why.empty() && ans[0].accountId == 0 &&
                        ans[1].save == SaveResult::Failed && !ans[1].why.empty() && st.AccountId("new") == 0 && st.MatchCount() == 2;
    st.SetFailLogins(false);
    st.SetFailSaves(false);
    ans.clear();
    st.SaveMatch(22, a, rec, "{}");
    st.TakeAnswers(ans);
    Check(failed && ans.size() == 1 && ans[0].save == SaveResult::Saved && st.MatchCount() == 3,
          "④ 실패 스위치 — 로그인 · 판 저장 Failed(까닭 글 · 안 넣음) · 끄면 같은 판이 Saved");
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
    const std::vector<std::string> wantA = {": 610 u 1", ": 605 u :127.0.0.1 4000 '0:Nox_Origin' -8 36.1083 -115.0582", ": 607"};
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
    int d = 0;
    if (kFixed.count(ours.kind) && Tokens(ours.lines[0]).size() != Tokens(xw.lines[0]).size()) {
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
        MirrorUnitTests();        // ★ 10-20 v2 작업 3a-2
        LinkAliveUnitTests();     // ★ 7단계 조각 2
        LobbyStoreUnitTests();    // ★ 7단계 조각 4
        MirrorSaveUnitTests();    // ★ 7단계 조각 5
        ChatUnitTests();          // ★ 10-20 v2 작업 3b
        RouterUnitTests();
    }
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
