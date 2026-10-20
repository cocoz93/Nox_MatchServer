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
    Check(n326 == 2 && n327 == 3, "LIST -1 37 = 방 2줄 + 채팅 로비 3줄");
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
    Check(out.empty(), "모르는 방 참가엔 답하지 않는다(토큰도 없음)");
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
    // 방이 사라지면(방 관리자가 닫음) 다음 목록에서 빠지고 참가엔 답하지 않는다
    fake.rooms.clear();
    out.clear();
    lb.OnLine(1, "LIST 37 37", out);
    const bool emptyList = out.size() == 2 && Tokens(out[0].line)[1] == "321" && Tokens(out[1].line)[1] == "323";
    out.clear();
    lb.OnLine(1, "JOINGAME #wn's_game 1 ", out);
    Check(emptyList && out.empty() && lb.Stats().tokensIssued == 1, "방이 사라지면 목록 0줄 · 참가엔 답 없음(토큰 없음)");
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
    Check(fake.created == 1 && out.empty(), "남의 이름 채널의 없는 방 — 안 만들고 답 없음");

    login(3, "plain", false, "949");
    out.clear();
    lb.OnLine(3, "JOINGAME #plain's_game 1 ", out);
    Check(fake.created == 1 && out.empty() && lb.Stats().newRoomNoClient == 1, "DLL 없는 클라의 새 방 — 안 만들고 답 없음(들어올 수 없는 방)");

    fake.canCreate = false;
    login(4, "dave", true, "949");
    out.clear();
    lb.OnLine(4, "JOINGAME #dave's_game 1 ", out);
    Check(out.empty() && lb.Stats().roomCreateFailed == 1 && lb.Stats().roomsCreated == 1, "채널이 못 만들면 답 없음 · 실패 셈");

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

    // ③ 실패 답 → 답 없음 · 실패 셈 · 기다림 0
    login(3, "carol");
    out.clear();
    lb.OnLine(3, "JOINGAME #carol's_game 1 ", out);
    lb.OnRoomCreateFailed(fake.asked.back(), "대기 맵 없음");
    Check(out.empty() && lb.PendingRooms() == 0 && lb.Stats().roomCreateFailed == 1, "③ 실패 답 → 답 없음 · 실패 셈 1 · 기다림 0");

    // ④ 시간 초과 — 한도 1ms 전엔 남고 한도에 접힘(32비트 시계가 도는 자리) · 그 뒤 온 답은 늦은 답(버림)
    login(4, "dave");
    out.clear();
    lb.OnLine(4, "JOINGAME #dave's_game 1 ", out);
    const uint32_t reqDave = fake.asked.back();
    lb.Tick();   // 한도 시각은 32비트를 넘어 0 근처로 돌았고 지금은 아직 안 돎 — 그냥 크기로 견주면 여기서 바로 접힌다
    const bool waitingAtStart = lb.PendingRooms() == 1;
    clock += kRoomRequestTimeoutMs - 1;
    lb.Tick();
    const bool stillWaiting = lb.PendingRooms() == 1 && lb.Stats().roomRequestTimeout == 0;
    clock += 1;
    lb.Tick();
    Check(waitingAtStart && stillWaiting && lb.PendingRooms() == 0 && lb.Stats().roomRequestTimeout == 1 && out.empty(),
          "④ 한도 1ms 전엔 기다림 · 한도에 시간 초과(답 없음) — 시계가 32비트를 넘어 돌아도");
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
    lb.OnClose(5);
    Check(lb.PendingRooms() == 0 && lb.Stats().roomRequestDropped == 1, "⑤ 기다리는 중 클라가 끊음 → 버림 셈 1");
    lb.OnRoomCreated(reqErin, made("erin"), out);
    lb.OnRoomCreateFailed(9999, "모르는 요청");
    Check(out.empty() && lb.Stats().lateRoomReply == 3, "⑤ 끊긴 뒤 온 답 · 모르는 요청의 실패 → 늦은 답(버림)");

    // ⑥ 요청 번호는 겹치지 않는다(답이 엉뚱한 접속에 가지 않게)
    std::set<uint32_t> uniq(fake.asked.begin(), fake.asked.end());
    Check(uniq.size() == fake.asked.size() && fake.asked.size() == 4, "⑥ 요청 번호 넷이 모두 다름");
    // 셈 맞추기 — 물음 4 = 만듦 1 + 실패 1 + 시간 초과 1 + 버림 1
    const LobbyStats& s = lb.Stats();
    Check(s.roomsRequested == 4 && s.roomsRequested == s.roomsCreated + s.roomCreateFailed + s.roomRequestTimeout + s.roomRequestDropped,
          "⑥ 물음 4 = 만듦 + 실패 + 시간 초과 + 버림(요청마다 한 갈래로 끝남)");
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
            lb.OnClose(rc.id);
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
        RouterUnitTests();
    }
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
