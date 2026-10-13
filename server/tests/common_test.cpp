// common_test.cpp — nox_common(입장 토큰 · 설정 파일) 시험. Docs/TOKEN_DESIGN.md 「시험」 표의 「토큰 단위」.
//
//   ① HMAC-SHA256 — RFC 4231 시험 값 둘(서명이 표준과 같은 값을 내나)
//   ② 16진·IPv4 글자 풀기
//   ③ 토큰 — 서명한 것이 그대로 풀리나 · 검증 갈래 일곱이 각각 나오나(개수로)
//   ④ 꼬리 찾기 — 접속 시도(100바이트)·CONNECT(3바이트) 뒤 55바이트
//   ⑤ 일회용 장부
//   ⑥ 설정 파일 — 구역·주석·BOM·대소문자·차례 · 틀린 꼴은 줄 번호와 함께 실패
//   ⑦ TCP 줄 부품(작업 8a) — 루프백 접속 · 줄 나누기(CRLF · LF · 나뉘어 온 줄) · 보내기 CRLF · 끊김 갈래 넷(본체는 `tcp_lines_test.cpp`)
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nox/IniFile.h"
#include "nox/JoinToken.h"

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
    Check(r.size() == 14, "TCP 줄 부품 확인 수 14(본체가 끝까지 돌았나)");
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
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
