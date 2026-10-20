// common_test.cpp — nox_common(입장 토큰 · 설정 파일) 시험. Docs/TOKEN_DESIGN.md 「시험」 표의 「토큰 단위」.
//
//   ① HMAC-SHA256 — RFC 4231 시험 값 둘(서명이 표준과 같은 값을 내나)
//   ② 16진·IPv4 글자 풀기
//   ③ 토큰 — 서명한 것이 그대로 풀리나 · 검증 갈래 일곱이 각각 나오나(개수로)
//   ④ 꼬리 찾기 — 접속 시도(100바이트)·CONNECT(3바이트) 뒤 55바이트
//   ⑤ 일회용 장부
//   ⑥ 설정 파일 — 구역·주석·BOM·대소문자·차례 · 틀린 꼴은 줄 번호와 함께 실패
//   ⑦ TCP 줄 부품(작업 8a) — 루프백 접속 · 줄 나누기(CRLF · LF · 나뉘어 온 줄) · 보내기 CRLF · 끊김 갈래 넷(본체는 `tcp_lines_test.cpp`)
//     ★ 10-20 v2 작업 2 — 바깥으로 붙기: 붙는 중 쌓은 줄 · 왕복 · 상대가 닫음 · 듣는 이 없음(붙지 못함) · 붙는 중 닫기 · 곧바로 실패
//   ⑧ 로비 연결 규약(MO 6단계 v2 작업 1 — `LobbyLink.h`) — 줄 갈래 열 왕복(★ 작업 3a-2 BYE 더함) · 글 칸(한글 · 빈칸 · 빈 글) · 읽기 실패 갈래 여덟 ·
//     판 결과 줄 한도 · 키 파생(고정 값 · 실행 번호 · 주소 · 포트마다 다름 · 다른 게임 서버 키의 토큰은 서명 실패) · 확인 서명
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nox/IniFile.h"
#include "nox/JoinToken.h"
#include "nox/LobbyLink.h"

std::vector<std::pair<bool, std::string>> RunTcpLinesChecks();   // tcp_lines_test.cpp

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

std::string HmacHex(const std::string& key, const std::string& data) {
    uint8_t out[32];
    if (!nox::HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                         reinterpret_cast<const uint8_t*>(data.data()), data.size(), out))
        return "(실패)";
    return nox::ToHex(out, sizeof(out));
}

nox::JoinKey TestKey() {
    nox::JoinKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(i * 7 + 1);
    return k;
}

void TestHmac() {
    std::printf("① HMAC-SHA256\n");
    Check(HmacHex(std::string(20, '\x0b'), "Hi There") ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "RFC 4231 1번");
    Check(HmacHex("Jefe", "what do ya want for nothing?") ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "RFC 4231 2번");
    uint8_t a[32] = {}, b[32] = {};
    Check(nox::RandomBytes(a, sizeof(a)) && nox::RandomBytes(b, sizeof(b)) && std::memcmp(a, b, sizeof(a)) != 0,
          "난수 두 번이 다름");
}

void TestText() {
    std::printf("② 16진 · IPv4\n");
    uint8_t buf[3];
    Check(nox::ParseHex("0aFf10", buf, 3) && buf[0] == 0x0a && buf[1] == 0xff && buf[2] == 0x10, "16진 풀기");
    Check(!nox::ParseHex("0aFf1", buf, 3), "16진 길이 모자람 → 실패");
    Check(!nox::ParseHex("0aFg10", buf, 3), "16진 아닌 글자 → 실패");
    Check(nox::ToHex(buf, 3) == "0aff10", "16진 쓰기");
    nox::JoinKey k;
    Check(nox::ParseJoinKey(std::string(64, 'a'), k) && k[31] == 0xaa, "키 64자");
    Check(!nox::ParseJoinKey(std::string(62, 'a'), k), "키 62자 → 실패");
    uint32_t ip = 0;
    Check(nox::ParseIpv4("127.0.0.1", ip) && ip == 0x7f000001u, "IPv4 127.0.0.1");
    Check(nox::ParseIpv4("203.0.113.9", ip) && nox::Ipv4ToString(ip) == "203.0.113.9", "IPv4 왕복");
    Check(!nox::ParseIpv4("1.2.3.256", ip), "IPv4 256 → 실패");
    Check(!nox::ParseIpv4("1.2.3.4x", ip), "IPv4 꼬리 글자 → 실패");
    Check(!nox::ParseIpv4("1.2.3", ip), "IPv4 셋 → 실패");
    Check(!nox::ParseIpv4("1..2.3", ip), "IPv4 빈 칸 → 실패");
    Check(!nox::ParseIpv4("1.2.3.4567", ip), "IPv4 네 자리 → 실패");
    Check(nox::ParseIpv4("0.0.0.0", ip) && ip == 0 && nox::ParseIpv4("255.255.255.255", ip) && ip == 0xffffffffu,
          "IPv4 양 끝");
}

void TestToken() {
    std::printf("③ 토큰\n");
    const nox::JoinKey key = TestKey();
    const uint32_t now = 1760064016;   // 10-10 시험 무렵
    const uint32_t ip = 0x7f000001u;

    nox::JoinToken t;
    t.roomId = 1;
    t.clientIp = ip;
    t.expiresAt = now + nox::kJoinTokenLifetimeSec;
    t.nonce = 0x0123456789abcdefull;
    t.account = "wn9457";
    uint8_t raw[nox::kJoinTokenSize];
    Check(nox::SignJoinToken(key, t, raw), "서명");

    // 바이트 배치 — 문서(TOKEN_DESIGN.md 「토큰 모양」)와 같아야 로비·게임 서버가 서로 다른 판으로 빌드돼도 맞는다
    Check(raw[0] == 1 && raw[1] == 1 && raw[2] == 0, "배치: 판 1 · 방 1(작은 끝수)");
    Check(raw[3] == 0x7f && raw[4] == 0 && raw[5] == 0 && raw[6] == 1, "배치: IP 127.0.0.1 차례대로");
    Check(raw[7] == static_cast<uint8_t>(t.expiresAt) && raw[10] == static_cast<uint8_t>(t.expiresAt >> 24),
          "배치: 만료 작은 끝수");
    Check(raw[11] == 0xef && raw[18] == 0x01, "배치: 일회용 번호 작은 끝수");
    Check(std::memcmp(raw + 19, "wn9457", 6) == 0 && raw[25] == 0 && raw[34] == 0, "배치: 로비 이름 · 0 채움");

    nox::JoinToken got;
    Check(nox::VerifyJoinToken(key, raw, sizeof(raw), now, ip, got) == nox::JoinTokenCheck::Ok, "맞는 토큰 → 통과");
    Check(got.roomId == 1 && got.clientIp == ip && got.expiresAt == t.expiresAt && got.nonce == t.nonce &&
              got.account == "wn9457" && got.version == nox::kJoinTokenVersion,
          "풀린 값이 넣은 값과 같음");

    // 검증 갈래마다 한 판씩 — 결과를 세어 갈래가 다 나오는지 본다
    unsigned seen[static_cast<int>(nox::JoinTokenCheck::kCount)] = {};
    auto run = [&](const uint8_t* d, size_t n, uint32_t at, uint32_t from, const nox::JoinKey& k) {
        nox::JoinToken o;
        const nox::JoinTokenCheck r = nox::VerifyJoinToken(k, d, n, at, from, o);
        ++seen[static_cast<int>(r)];
        return r;
    };
    using C = nox::JoinTokenCheck;
    Check(run(raw, sizeof(raw), now, ip, key) == C::Ok, "통과");
    Check(run(raw, sizeof(raw) - 1, now, ip, key) == C::BadSize, "50바이트 → 크기");
    {
        nox::JoinToken v2 = t;
        v2.version = 2;
        uint8_t r2[nox::kJoinTokenSize];
        nox::SignJoinToken(key, v2, r2);
        Check(run(r2, sizeof(r2), now, ip, key) == C::BadVersion, "판 2 → 판");
    }
    {
        uint8_t r3[nox::kJoinTokenSize];
        std::memcpy(r3, raw, sizeof(r3));
        r3[1] ^= 0x01;   // 방 1 → 0 으로 바꿔치기
        Check(run(r3, sizeof(r3), now, ip, key) == C::BadMac, "방 바꿔치기 → 서명");
        std::memcpy(r3, raw, sizeof(r3));
        r3[nox::kJoinTokenSize - 1] ^= 0x80;
        Check(run(r3, sizeof(r3), now, ip, key) == C::BadMac, "서명 끝 바이트 → 서명");
        nox::JoinKey other = key;
        other[0] ^= 1;
        Check(run(raw, sizeof(raw), now, ip, other) == C::BadMac, "다른 키 → 서명");
    }
    Check(run(raw, sizeof(raw), t.expiresAt, ip, key) == C::Ok, "만료 바로 그 초 → 통과");
    Check(run(raw, sizeof(raw), t.expiresAt + 1, ip, key) == C::Expired, "만료 1초 뒤 → 만료");
    {
        nox::JoinToken far = t;
        far.expiresAt = now + nox::kJoinTokenMaxAheadSec + 1;
        uint8_t r4[nox::kJoinTokenSize];
        nox::SignJoinToken(key, far, r4);
        Check(run(r4, sizeof(r4), now, ip, key) == C::TooFarAhead, "만료가 121초 뒤 → 너무 멂");
        far.expiresAt = now + nox::kJoinTokenMaxAheadSec;
        nox::SignJoinToken(key, far, r4);
        Check(run(r4, sizeof(r4), now, ip, key) == C::Ok, "만료가 120초 뒤 → 통과");
    }
    Check(run(raw, sizeof(raw), now, 0x7f000002u, key) == C::IpMismatch, "다른 IP → IP 다름");

    unsigned kinds = 0;
    for (unsigned c : seen) kinds += c ? 1 : 0;
    Check(kinds == static_cast<unsigned>(nox::JoinTokenCheck::kCount), "검증 갈래 일곱이 모두 나옴");

    // 로비 이름 — 16바이트에서 자른다 · 꽉 찬 16바이트도 0 없이 읽힌다
    nox::JoinToken longName = t;
    longName.account = "abcdefghijklmnopqrstu";
    nox::SignJoinToken(key, longName, raw);
    Check(nox::VerifyJoinToken(key, raw, sizeof(raw), now, ip, got) == C::Ok && got.account == "abcdefghijklmnop",
          "로비 이름 21자 → 앞 16자");
}

void TestTrailer() {
    std::printf("④ 꼬리\n");
    std::vector<uint8_t> join(100, 0x00);
    join[2] = 0x0e;
    std::vector<uint8_t> tok(nox::kJoinTokenSize, 0x5a);
    std::vector<uint8_t> pkt = join;
    pkt.insert(pkt.end(), tok.begin(), tok.end());
    pkt.insert(pkt.end(), nox::kJoinTrailerMagic, nox::kJoinTrailerMagic + 4);

    const uint8_t* at = nullptr;
    size_t kept = 0;
    Check(nox::FindJoinTrailer(pkt.data(), pkt.size(), 3, at, kept) && kept == 100 && at == pkt.data() + 100,
          "접속 시도 100 + 꼬리 55 → 100 남김");
    Check(!nox::FindJoinTrailer(join.data(), join.size(), 3, at, kept), "꼬리 없음 → 못 찾음");

    std::vector<uint8_t> conn = { 0xff, 0x00, 0x00 };
    conn.insert(conn.end(), tok.begin(), tok.end());
    conn.insert(conn.end(), nox::kJoinTrailerMagic, nox::kJoinTrailerMagic + 4);
    Check(nox::FindJoinTrailer(conn.data(), conn.size(), 3, at, kept) && kept == 3, "CONNECT 3 + 꼬리 55 → 3 남김");
    Check(!nox::FindJoinTrailer(conn.data() + 1, conn.size() - 1, 3, at, kept), "남길 몫이 3바이트 모자람 → 못 찾음");

    std::vector<uint8_t> wrong = pkt;
    wrong.back() = '2';
    Check(!nox::FindJoinTrailer(wrong.data(), wrong.size(), 3, at, kept), "표시 NXT2 → 못 찾음");
}

void TestLedger() {
    std::printf("⑤ 일회용 장부\n");
    nox::CJoinTokenLedger led;
    Check(led.Consume(1, 1000, 990), "처음 쓰는 번호 → 받음");
    Check(!led.Consume(1, 1000, 991), "같은 번호 두 번째 → 거절");
    Check(led.Consume(2, 1000, 991), "다른 번호 → 받음");
    led.Prune(1000);
    Check(led.Size() == 2, "만료 그 초에는 아직 기억");
    led.Prune(1001);
    Check(led.Size() == 0, "만료 지나면 잊음");
}

void TestIni() {
    std::printf("⑥ 설정 파일\n");
    const std::string text =
        "\xEF\xBB\xBF; 메모장이 붙이는 BOM 과 주석\n"
        "[Token]\r\n"
        "  Key = 00112233  \r\n"
        "# 주석\n"
        "[rooms]\n"
        "Estate Arena = 0\n"
        "Lava = 1\n"
        "empty =\n";
    nox::CIniFile ini;
    Check(ini.LoadText(text), "읽기");
    Check(ini.Get("token", "KEY") == "00112233", "구역·키 대소문자 무시 · 앞뒤 공백 걷음");
    Check(ini.Has("rooms", "empty") && ini.Get("rooms", "empty", "x").empty(), "빈 값도 있는 키");
    Check(ini.Get("rooms", "nope", "기본") == "기본", "없는 키 → 기본값");
    const auto rooms = ini.Section("ROOMS");
    Check(rooms.size() == 3 && rooms[0].first == "Estate Arena" && rooms[0].second == "0" && rooms[1].first == "Lava",
          "구역 안 차례 그대로 · 키 원래 글자");

    nox::CIniFile bad;
    Check(!bad.LoadText("[a]\nk=1\nK=2\n") && bad.Error().find("3번 줄") == 0, "같은 키 두 번 → 실패(3번 줄)");
    Check(!bad.LoadText("[a]\nnoequal\n") && bad.Error().find("2번 줄") == 0, "= 없음 → 실패(2번 줄)");
    Check(!bad.LoadText("[a\nk=1\n") && bad.Error().find("1번 줄") == 0, "구역 ] 없음 → 실패(1번 줄)");
    Check(bad.LoadText("[a]\nk=1\n[b]\nk=2\n") && bad.Get("b", "k") == "2", "다른 구역의 같은 키는 따로");
    Check(!bad.LoadFile("C:\\없는\\경로\\nox.ini"), "없는 파일 → 실패");
}

void TestTcpLines() {
    std::printf("⑦ TCP 줄 부품\n");
    const auto r = RunTcpLinesChecks();
    for (const auto& c : r) Check(c.first, c.second.c_str());
    Check(r.size() == 27, "TCP 줄 부품 확인 수 27(본체가 끝까지 돌았나 — 듣기 14 + ★ 10-20 줄 한도 경계 2 · 바깥으로 붙기 11)");
}

void TestLobbyLink() {
    std::printf("⑧ 로비 연결 규약\n");
    using K = nox::LinkKind;
    using P = nox::LinkParse;

    // 글 칸 — 빈칸 · % · ~ · 제어 바이트만 바뀌고 한글(UTF-8)은 그대로
    Check(nox::EncodeLinkText("a b%c~d") == "a%20b%25c%7Ed", "글 칸: 빈칸 · % · ~ → %XX");
    Check(nox::EncodeLinkText("") == "~", "글 칸: 빈 글 → ~");
    Check(nox::EncodeLinkText("\xEC\xB2\xA0\xEC\x88\x98's game") == "\xEC\xB2\xA0\xEC\x88\x98's%20game",
          "글 칸: 한글 바이트는 그대로(기록에서 읽힌다)");
    Check(nox::EncodeLinkText(std::string("\x01\x7f", 2)) == "%01%7F", "글 칸: 제어 바이트");
    std::string dec;
    Check(nox::DecodeLinkText("a%20b%25c%7ed", dec) && dec == "a b%c~d", "글 칸 풀기(소문자 16진도)");
    Check(nox::DecodeLinkText("~", dec) && dec.empty(), "글 칸 풀기: ~ → 빈 글");

    // 줄 갈래 열 — 지은 줄을 읽어 넣은 값이 그대로 나오나 · 갈래마다 센다
    unsigned kinds[static_cast<int>(K::kCount)] = {};
    nox::LinkLine l;
    auto parse = [&](const std::string& line) {
        const P p = nox::ParseLinkLine(line, l);
        if (p == P::Ok) ++kinds[static_cast<int>(l.kind)];
        return p;
    };
    const std::string nonce(32, 'a');
    Check(parse(nox::LinkHello(nonce)) == P::Ok && l.kind == K::Hello && l.text == nonce, "HELLO 왕복");
    const std::string sig(64, 'F');
    Check(parse(nox::LinkAuth(sig)) == P::Ok && l.kind == K::Auth && l.text == sig, "AUTH 왕복");
    Check(nox::LinkRegister(1760000000u, 0, 18590) == "REGISTER 1760000000 0.0.0.0 18590", "REGISTER 꼴(주소 0 = 0.0.0.0)");
    Check(parse(nox::LinkRegister(1760000000u, 0x7f000002u, 18590)) == P::Ok && l.kind == K::Register && l.epoch == 1760000000u &&
              l.ip == 0x7f000002u && l.port == 18590,
          "REGISTER 왕복");
    Check(parse(nox::LinkRegister(1760000000u, 0, 18590)) == P::Ok && l.ip == 0, "REGISTER 주소 0 읽힘(같은 PC — 로비 쪽 주소)");
    nox::LinkRoom room;
    room.id = 65535;
    room.players = 3;
    room.maxPlayers = 8;
    room.playing = true;
    room.name = "\xEC\xB2\xA0\xEC\x88\x98's game";   // 철수's game
    room.creator = "a~b";
    room.host = "c%d";
    room.map = "estate";
    room.mode = "";
    const std::string roomLine = nox::LinkRoomLine(room);
    Check(roomLine == "ROOM 65535 3 8 1 \xEC\xB2\xA0\xEC\x88\x98's%20game a%7Eb c%25d estate ~", "ROOM 꼴(칸 차례 · 글 칸)");
    Check(parse(roomLine) == P::Ok && l.kind == K::Room && l.room.id == 65535 && l.room.players == 3 && l.room.maxPlayers == 8 &&
              l.room.playing && l.room.name == room.name && l.room.creator == "a~b" && l.room.host == "c%d" &&
              l.room.map == "estate" && l.room.mode.empty(),
          "ROOM 왕복(한글 · 빈칸 · % · ~ · 빈 모드)");
    room.playing = false;
    Check(parse(nox::LinkRoomLine(room)) == P::Ok && !l.room.playing, "ROOM 판 중 0");
    Check(parse(nox::LinkRoomGone(7)) == P::Ok && l.kind == K::RoomGone && l.room.id == 7, "ROOM_GONE 왕복");
    Check(parse(nox::LinkCreate(42, "\xEC\xB2\xA0 x")) == P::Ok && l.kind == K::Create && l.req == 42 && l.text == "\xEC\xB2\xA0 x",
          "CREATE 왕복(빈칸 든 이름)");
    Check(parse(nox::LinkCreated(42, 3)) == P::Ok && l.kind == K::Created && l.req == 42 && l.room.id == 3, "CREATED 왕복");
    Check(parse(nox::LinkCreateFailed(43, "no wait map")) == P::Ok && l.kind == K::CreateFailed && l.req == 43 &&
              l.text == "no wait map",
          "CREATE_FAILED 왕복");
    const std::string json = "{\"match\":\"1-0-1\",\"roomName\":\"a b\",\"players\":[]}";
    std::string resLine;
    Check(nox::LinkResult(json, resLine) && parse(resLine) == P::Ok && l.kind == K::Result && l.text == json,
          "RESULT 왕복(빈칸 든 JSON 꼬리 전체)");
    Check(parse(nox::LinkBye("same address")) == P::Ok && l.kind == K::Bye && l.text == "same address" &&
              nox::LinkBye("") == "BYE ~",
          "BYE 왕복(빈칸 든 까닭 · 빈 까닭 ~)");
    unsigned kindsSeen = 0;
    for (unsigned c : kinds) kindsSeen += c ? 1 : 0;
    Check(kindsSeen == static_cast<unsigned>(K::kCount), "줄 갈래 열이 모두 읽힘");
    for (int k = 0; k < static_cast<int>(K::kCount); ++k)
        Check(nox::LinkKindWord(static_cast<K>(k))[0] != '?', "갈래마다 낱말 있음");

    // 읽기 실패 — 갈래마다 한 번 이상(개수로)
    unsigned fails[static_cast<int>(P::kCount)] = {};
    auto bad = [&](const std::string& line, P want) {
        const P p = nox::ParseLinkLine(line, l);
        ++fails[static_cast<int>(p)];
        return p == want;
    };
    Check(bad("", P::Empty), "빈 줄 → 빈 줄");
    Check(bad("PING 1", P::UnknownWord) && bad("room 1", P::UnknownWord), "모르는 낱말 · 소문자 → 모르는 낱말");
    Check(bad("ROOM_GONE 1 2", P::FieldCount) && bad("ROOM_GONE  1", P::FieldCount) && bad("CREATED 1 2 ", P::FieldCount) &&
              bad("RESULT", P::FieldCount) && bad("RESULT ", P::FieldCount) && bad("ROOM 1 2 8 0 a b c d", P::FieldCount),
          "칸 수 — 남음 · 빈칸 둘 · 꼬리 빈칸 · 빈 결과 · ROOM 아홉 칸");
    Check(bad("ROOM_GONE 65536", P::BadNumber) && bad("ROOM_GONE 01", P::BadNumber) && bad("ROOM_GONE -1", P::BadNumber) &&
              bad("ROOM_GONE +1", P::BadNumber) && bad("CREATED 4294967296 1", P::BadNumber) &&
              bad("REGISTER 1 0.0.0.0 0", P::BadNumber) && bad("REGISTER 1 0.0.0.0 65536", P::BadNumber) &&
              bad("ROOM 1 2 8 2 a b c d e", P::BadNumber),
          "수 — 방 65536 · 앞 0 · 부호 · 32비트 넘음 · 포트 0 · 포트 65536 · 판 중 2");
    Check(bad("CREATE 1 a~b", P::BadText) && bad("CREATE 1 %4", P::BadText) && bad("CREATE 1 %zz", P::BadText) &&
              bad("CREATE 1 ", P::BadText) && bad(std::string("CREATE 1 a\x01", 11), P::BadText),
          "글 칸 — 날 ~ · % 뒤 모자람 · 16진 아님 · 빈 칸 · 제어 바이트");
    Check(bad("HELLO abc", P::BadHex) && bad("AUTH " + std::string(63, 'a'), P::BadHex) && bad("HELLO " + std::string(32, 'g'), P::BadHex),
          "16진 — 짧음 · 63자 · 16진 아님");
    Check(bad("REGISTER 1 1.2.3 18590", P::BadAddress), "주소 — 셋");
    unsigned failKinds = 0;
    for (int i = 1; i < static_cast<int>(P::kCount); ++i) failKinds += fails[i] ? 1 : 0;
    Check(failKinds == static_cast<unsigned>(P::kCount) - 1 && fails[static_cast<int>(P::Ok)] == 0,
          "읽기 실패 갈래 여덟이 모두 나옴 · 틀린 줄이 통과한 적 없음");
    for (int i = 0; i < static_cast<int>(P::kCount); ++i)
        Check(nox::LinkParseName(static_cast<P>(i))[0] != '?', "실패 갈래마다 이름 있음");

    // 판 결과 줄 한도 — 받는 쪽은 줄 끝을 못 본 바이트가 4096 을 넘으면 끊는다(`TcpLines.h`)
    Check(nox::kLinkMaxLine == 4095, "줄 한도 4095(CR 몫 하나 뺌)");
    const size_t room0 = nox::kLinkMaxLine - std::strlen("RESULT ");
    Check(nox::LinkResult(std::string(room0, 'x'), resLine) && resLine.size() == nox::kLinkMaxLine, "한도에 꼭 맞는 결과 줄 → 보냄");
    Check(!nox::LinkResult(std::string(room0 + 1, 'x'), resLine), "한도 한 바이트 넘음 → 안 보냄");
    Check(!nox::LinkResult("{\"a\":1}\n", resLine) && !nox::LinkResult("{\r}", resLine) && !nox::LinkResult("", resLine),
          "줄바꿈 든 JSON · 빈 JSON → 안 보냄");

    // 키 — 고정 값(이름 붙이기 꼴이 바뀌면 다른 판으로 빌드된 로비 · 게임 서버가 서로 안 맞는다)
    const nox::JoinKey base = TestKey();
    auto hmacKey = [&](const std::string& label) {
        nox::JoinKey k{};
        nox::HmacSha256(base.data(), base.size(), reinterpret_cast<const uint8_t*>(label.data()), label.size(), k.data());
        return k;
    };
    const nox::JoinKey authKey = nox::LinkAuthKey(base);
    Check(authKey == hmacKey("nox-link-auth-v1") && authKey != base, "연결 키 = HMAC(토큰 키, nox-link-auth-v1)");
    const nox::JoinKey gsA = nox::GameServerTokenKey(base, 1760000000u, 0, 18590);
    Check(gsA == hmacKey("nox-gs-token-v1|1760000000|0.0.0.0|18590"), "게임 서버 키 = HMAC(토큰 키, 등록 줄 값 그대로 — 주소 0 도)");
    Check(gsA == nox::GameServerTokenKey(base, 1760000000u, 0, 18590), "같은 값 → 같은 키(로비 · 게임 서버가 따로 뽑아도 맞음)");
    Check(gsA != nox::GameServerTokenKey(base, 1760000001u, 0, 18590), "실행 번호가 다르면 다른 키(다시 켠 게임 서버)");
    Check(gsA != nox::GameServerTokenKey(base, 1760000000u, 0x7f000002u, 18590), "주소가 다르면 다른 키(둘째 게임 서버)");
    Check(gsA != nox::GameServerTokenKey(base, 1760000000u, 0, 18591), "포트가 다르면 다른 키");
    Check(gsA != authKey && gsA != base, "게임 서버 키 ≠ 연결 키 · 토큰 키");
    {
        nox::JoinToken t;
        t.roomId = 0;
        t.clientIp = 0x7f000001u;
        t.expiresAt = 1760000030u;
        t.nonce = 5;
        t.account = "a";
        uint8_t raw[nox::kJoinTokenSize];
        nox::JoinToken got;
        const nox::JoinKey gsB = nox::GameServerTokenKey(base, 1760000000u, 0x7f000002u, 18590);
        Check(nox::SignJoinToken(gsA, t, raw) &&
                  nox::VerifyJoinToken(gsA, raw, sizeof(raw), 1760000000u, t.clientIp, got) == nox::JoinTokenCheck::Ok &&
                  nox::VerifyJoinToken(gsB, raw, sizeof(raw), 1760000000u, t.clientIp, got) == nox::JoinTokenCheck::BadMac &&
                  nox::VerifyJoinToken(base, raw, sizeof(raw), 1760000000u, t.clientIp, got) == nox::JoinTokenCheck::BadMac,
              "게임 서버 A 키 토큰 → A 통과 · B · 옛 토큰 키는 서명 실패");
    }

    // 확인 서명
    const std::string n1 = nox::NewLinkNonceHex(), n2 = nox::NewLinkNonceHex();
    Check(n1.size() == 32 && n2.size() == 32 && n1 != n2, "일회용 값 16진 32자 · 두 번이 다름");
    const std::string s1 = nox::LinkAuthSig(authKey, n1);
    Check(s1.size() == 64 && parse(nox::LinkAuth(s1)) == P::Ok, "서명 16진 64자 · AUTH 줄로 읽힘");
    Check(nox::CheckLinkAuth(authKey, n1, s1), "맞는 서명 → 통과");
    nox::JoinKey otherKey = authKey;
    otherKey[0] ^= 1;
    std::string s1bad = s1;
    s1bad[63] = s1bad[63] == '0' ? '1' : '0';
    Check(!nox::CheckLinkAuth(otherKey, n1, s1) && !nox::CheckLinkAuth(authKey, n2, s1) && !nox::CheckLinkAuth(authKey, n1, s1bad) &&
              !nox::CheckLinkAuth(authKey, n1, s1.substr(0, 63)),
          "다른 키 · 다른 일회용 값 · 바꾼 서명 · 63자 → 실패");
    Check(nox::CheckLinkAuth(authKey, n1, nox::LinkAuthSig(nox::LinkAuthKey(base), n1)), "따로 뽑은 연결 키로 지은 서명도 통과");
}

}  // namespace

int main() {
    TestHmac();
    TestText();
    TestToken();
    TestTrailer();
    TestLedger();
    TestIni();
    TestTcpLines();
    TestLobbyLink();
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
