// proto_test.cpp — 이식한 메시지 정의가 스스로 앞뒤가 맞는지만 본다.
//
// 여기서 통과한다고 클라가 받아준다는 뜻은 아니다.
// 진짜 검증은 캡처와 대조하는 것이고, 이건 그 전에 오타를 거르는 용도다.
//
// 빌드:
//   cl /std:c++17 /EHsc /I server\include server\tests\proto_test.cpp server\src\Opcode.cpp
#include <cstdio>
#include <cstring>
#include <vector>

#include "nox/Messages.h"
#include "nox/SpellNames.h"
#include "nox/Opcode.h"

namespace {

int g_failed = 0;

void Check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [실패] %s\n", what);
        ++g_failed;
    }
}

// 넣었다 꺼냈을 때 같은 값이 나오고, 쓴 바이트 수가 명세와 맞는지 본다.
template <typename T>
void RoundTrip(const char* name, const T& in, size_t expectSize) {
    std::vector<uint8_t> buf(512, 0xCD);

    nox::Writer w(buf.data(), buf.size());
    Check(in.Encode(w), "인코딩 실패");
    Check(w.written() == expectSize, "쓴 바이트 수가 명세와 다름");

    T out{};
    nox::Reader r(buf.data(), w.written());
    Check(out.Decode(r), "디코딩 실패");
    Check(r.offset() == w.written(), "읽은 바이트 수가 쓴 것과 다름");

    std::printf("  %-18s %3zu바이트  op=0x%02X %s\n", name, w.written(),
                static_cast<unsigned>(T::kOp), nox::OpName(T::kOp));
}

void TestServerInfo() {
    nox::MsgServerInfo m;
    m.playersCur = 3;
    m.playersMax = 16;
    m.mapName = "estate";
    m.token = 0xDEADBEEF;
    m.serverName = "Nox_Server test";

    std::vector<uint8_t> buf(512, 0xCD);
    nox::Writer w(buf.data(), buf.size());
    Check(m.Encode(w), "SERVER_INFO 인코딩 실패");
    Check(w.written() == m.EncodeSize(), "SERVER_INFO 크기 계산이 실제와 다름");

    // 토큰이 41번 자리에 리틀엔디안으로 들어갔는지 눈으로 확인한다.
    Check(buf[41] == 0xEF && buf[42] == 0xBE && buf[43] == 0xAD && buf[44] == 0xDE,
          "토큰이 41번 자리에 리틀엔디안으로 안 들어감");
    // 서버 이름은 69번부터.
    Check(std::memcmp(&buf[69], "Nox_Server test", 15) == 0,
          "서버 이름이 69번 자리에서 시작하지 않음");

    nox::MsgServerInfo out;
    nox::Reader r(buf.data(), w.written());
    Check(out.Decode(r), "SERVER_INFO 디코딩 실패");
    Check(out.playersCur == 3 && out.playersMax == 16, "인원 값이 안 맞음");
    Check(out.mapName == "estate", "맵 이름이 안 맞음");
    Check(out.token == 0xDEADBEEF, "토큰이 안 맞음");
    Check(out.serverName == "Nox_Server test", "서버 이름이 안 맞음");

    std::printf("  %-18s %3zu바이트  op=0x%02X %s\n", "SERVER_INFO", w.written(),
                static_cast<unsigned>(nox::MsgServerInfo::kOp),
                nox::OpName(nox::MsgServerInfo::kOp));
}

void TestTryJoin() {
    nox::MsgServerTryJoin m;
    m.playerName = "Jack";
    m.playerClass = 1;
    m.playerLevel = 7;
    m.serial = "ABC123";
    m.version = 0x00010203;
    m.team = 2;

    RoundTrip("SERVER_TRY_JOIN", m, nox::MsgServerTryJoin::kSize);

    std::vector<uint8_t> buf(256, 0);
    nox::Writer w(buf.data(), buf.size());
    m.Encode(w);
    // 이름이 UTF-16LE로 1번 자리부터 들어갔는지.
    Check(buf[1] == 'J' && buf[2] == 0 && buf[3] == 'a' && buf[4] == 0,
          "이름이 UTF-16LE로 안 들어감");
    // 직업·레벨이 51·52번 자리인지. ← 이 배치가 캡처로 확정할 대상이다.
    Check(buf[51] == 1 && buf[52] == 7, "직업·레벨이 51·52번 자리가 아님");

    nox::MsgServerTryJoin out;
    nox::Reader r(buf.data(), nox::MsgServerTryJoin::kSize);
    Check(out.Decode(r), "TRY_JOIN 디코딩 실패");
    Check(out.playerName == "Jack", "이름이 안 맞음");
    Check(out.serial == "ABC123", "시리얼이 안 맞음");
    Check(out.version == 0x00010203, "버전이 안 맞음");
}

void TestOpcodeTable() {
    // 오프코드 값이 표와 어긋나지 않는지 몇 개만 짚는다.
    Check(static_cast<uint8_t>(nox::Op::SERVER_CONNECT) == 0x00, "SERVER_CONNECT != 0x00");
    Check(static_cast<uint8_t>(nox::Op::SERVER_ACCEPT) == 0x01, "SERVER_ACCEPT != 0x01");
    Check(static_cast<uint8_t>(nox::Op::SERVER_DISCOVER) == 0x0C, "SERVER_DISCOVER != 0x0C");
    Check(static_cast<uint8_t>(nox::Op::SERVER_INFO) == 0x0D, "SERVER_INFO != 0x0D");
    Check(static_cast<uint8_t>(nox::Op::SERVER_TRY_JOIN) == 0x0E, "SERVER_TRY_JOIN != 0x0E");
    Check(static_cast<uint8_t>(nox::Op::SERVER_JOIN_OK) == 0x14, "SERVER_JOIN_OK != 0x14");
    Check(static_cast<uint8_t>(nox::Op::INVENTORY_FAIL) == 0xF1, "마지막 오프코드 != 0xF1");

    Check(std::strcmp(nox::OpName(nox::Op::SERVER_INFO), "MSG_SERVER_INFO") == 0,
          "이름표가 안 맞음");
    // 정의되지 않은 자리는 UNKNOWN이어야 한다.
    Check(std::strcmp(nox::OpName(static_cast<nox::Op>(0xFF)), "UNKNOWN") == 0,
          "빈 자리가 UNKNOWN이 아님");
}

// 경계를 넘는 입력에 무너지지 않는지.
void TestBounds() {
    uint8_t small[3] = {};
    nox::Reader r(small, sizeof(small));
    nox::MsgServerAccept m;
    Check(!m.Decode(r), "짧은 입력인데 디코딩이 성공함");
    Check(!r.ok(), "실패했는데 ok()가 참");

    uint8_t tiny[2] = {};
    nox::Writer w(tiny, sizeof(tiny));
    nox::MsgServerAccept out;
    Check(!out.Encode(w), "작은 버퍼인데 인코딩이 성공함");
}

} // namespace


// 주문 이름표 — 09-06 캡처로 실측한 다섯이 계속 맞는지 지킨다.
// 표는 tools/dumpscan -genspells 로 뽑는다. 번호가 밀리면 여기서 걸린다.
void TestSpellNames() {
    struct { int id; const char* name; const char* ko; } kKnown[] = {
        {  4, "SPELL_BLINK",            "점멸"     },
        { 27, "SPELL_FIREBALL",         "파이어볼" },
        { 41, "SPELL_LESSER_HEAL",      "약한치유" },
        { 43, "SPELL_CHAIN_LIGHTNING",  "연쇄번개" },
        { 51, "SPELL_SHIELD",           "보호막"   },
    };
    for (const auto& k : kKnown) {
        const char* got = nox::SpellName(k.id);
        bool ok = std::strcmp(got, k.name) == 0;
        if (!ok) std::printf("      주문 %d: 표에는 \"%s\", 실측은 %s(%s)\n", k.id, got, k.name, k.ko);
        Check(ok, "주문 번호와 이름이 실측과 다름");
    }
    Check(nox::kSpellCount > 100, "주문 표가 너무 작다 — 생성이 덜 됐다");
    Check(std::strcmp(nox::SpellName(0), "") == 0, "0번은 빈 이름이어야 한다");
    Check(std::strcmp(nox::SpellName(-1), "") == 0, "음수는 빈 이름이어야 한다");
    Check(std::strcmp(nox::SpellName(nox::kSpellCount), "") == 0, "범위 밖은 빈 이름이어야 한다");
}

// ── 게임 중 메시지 — **캡처한 진짜 바이트로 뜯는다** ────────────────
//
// ★ 왕복(넣었다 꺼내기)만으로는 부족하다. 내 Encode 가 만든 바이트를 내 Decode 로 읽으면
//   짜임이 틀려도 늘 통과한다 — 이 프로젝트가 이미 세 번 속은 자리다.
//   그래서 **와이어에서 실제로 잡은 바이트**를 넣고 값이 나오는지 본다.
//   표본 출처는 Docs/PROTOCOL.md (2026-09-06 덤프, 09-07 판독).

// 오프코드를 뺀 본문 바이트를 넣어 뜯고, 먹은 길이가 명세와 맞는지 본다.
template <typename T>
T Parse(const char* name, const std::vector<uint8_t>& body, size_t expectSize) {
    T out{};
    nox::Reader r(body.data(), body.size());
    Check(out.Decode(r), name);
    Check(r.offset() == expectSize, "먹은 바이트 수가 명세와 다름");
    Check(r.offset() == body.size(), "본문을 남김없이 먹지 않음");
    return out;
}

void TestGameplayMessages() {
    // e8 d8 02  — 번호 728 이 죽었다 (14:44:51.867)
    {
        auto m = Parse<nox::MsgPlayerDied>("PLAYER_DIED", {0xd8, 0x02},
                                           nox::MsgPlayerDied::kSize);
        Check(m.netCode == 728, "PLAYER_DIED 번호가 728 이 아님");
    }

    // e9 d0 02 5d b8 00 00 ff 01  — 번호 720 · 틱 47,197 (15:00:12)
    //
    // ★ 4바이트가 틱인 근거: 표본 다섯의 (값 차이 ÷ 시각 차이)가 모두 28.0~28.3/초다.
    //   여기 둘로만 봐도  (47,197 − 13,888) / 1,182초 = 28.2  — 걸음으로 잰 28.0Hz 와 맞는다.
    {
        auto m = Parse<nox::MsgPlayerRespawn>("PLAYER_RESPAWN",
                                              {0xd0, 0x02, 0x5d, 0xb8, 0x00, 0x00, 0xff, 0x01},
                                              nox::MsgPlayerRespawn::kSize);
        Check(m.netCode == 720, "PLAYER_RESPAWN 번호가 720 이 아님");
        Check(m.tick == 47197, "PLAYER_RESPAWN 틱이 47,197 이 아님");
        Check(m.unk7 == 0xff && m.unk8 == 0x01, "PLAYER_RESPAWN 끝 두 바이트가 표본과 다름");
    }

    // 43 4a 00  — 체력 74. 이어서 72 → 69 → 65 로 35ms 간격에 내려갔다(지속 피해)
    {
        auto m = Parse<nox::MsgReportPlayerHealth>("REPORT_PLAYER_HEALTH", {0x4a, 0x00},
                                                   nox::MsgReportPlayerHealth::kSize);
        Check(m.health == 74, "REPORT_PLAYER_HEALTH 값이 74 가 아님");
    }

    // 42 d8 02 81 ff  — 번호 728 이 127 을 잃었다. **부호가 살아야 한다**
    {
        auto m = Parse<nox::MsgReportHealthDelta>("REPORT_HEALTH_DELTA",
                                                  {0xd8, 0x02, 0x81, 0xff},
                                                  nox::MsgReportHealthDelta::kSize);
        Check(m.netCode == 728, "REPORT_HEALTH_DELTA 번호가 728 이 아님");
        Check(m.delta == -127, "REPORT_HEALTH_DELTA 가 음수로 안 읽힘");
    }

    // dd d0 02 4b 00 4b 00  — 번호 720 · 75/75
    {
        auto m = Parse<nox::MsgReportTotalHealth>("REPORT_TOTAL_HEALTH",
                                                  {0xd0, 0x02, 0x4b, 0x00, 0x4b, 0x00},
                                                  nox::MsgReportTotalHealth::kSize);
        Check(m.netCode == 720 && m.health == 75 && m.maxHealth == 75,
              "REPORT_TOTAL_HEALTH 가 720 · 75/75 로 안 읽힘");
    }

    // 4b b0 00 7e 02  — 번호 176 · 종류 638 (BluePotion)
    {
        auto m = Parse<nox::MsgReportPickup>("REPORT_PICKUP", {0xb0, 0x00, 0x7e, 0x02},
                                             nox::MsgReportPickup::kSize);
        Check(m.netCode == 176 && m.type == 638, "REPORT_PICKUP 이 176 · 638 로 안 읽힘");
    }

    // 4c cc 02 6d 04 ff bf ff ff  — 번호 716 · 종류 1133 (StreetPants)
    {
        auto m = Parse<nox::MsgReportModifiablePickup>(
            "REPORT_MODIFIABLE_PICKUP", {0xcc, 0x02, 0x6d, 0x04, 0xff, 0xbf, 0xff, 0xff},
            nox::MsgReportModifiablePickup::kSize);
        Check(m.netCode == 716 && m.type == 1133, "0x4C 가 716 · 1133 으로 안 읽힘");
        Check(m.mods[0] == 0xff && m.mods[1] == 0xbf, "0x4C 수식 바이트가 표본과 다름");
    }

    // 4d db 02 91 04  — 번호 731 · 종류 1169 (Longsword). 죽음 0.03초 뒤에 흘린 것
    {
        auto m = Parse<nox::MsgReportDrop>("REPORT_DROP", {0xdb, 0x02, 0x91, 0x04},
                                           nox::MsgReportDrop::kSize);
        Check(m.netCode == 731 && m.type == 1169, "REPORT_DROP 이 731 · 1169 로 안 읽힘");
    }

    // 60 cc 02  — 방금 주운 716 을 착용
    {
        auto m = Parse<nox::MsgReportEquip>("REPORT_EQUIP", {0xcc, 0x02},
                                            nox::MsgReportEquip::kSize);
        Check(m.netCode == 716, "REPORT_EQUIP 번호가 716 이 아님");
    }

    // cc 20 00 07 | ...  — 껍데기. 머리만 먹고 속 7바이트는 부르는 쪽이 다시 읽는다
    {
        nox::MsgSeqImportant m;
        const uint8_t body[] = {0x20, 0x00, 0x07, 0x6a, 0xcb, 0x02, 0x00, 0x00, 0x00, 0x00};
        nox::Reader r(body, sizeof(body));
        Check(m.Decode(r), "SEQ_IMPORTANT");
        Check(m.id == 0x0020, "SEQ_IMPORTANT 번호가 0x0020 이 아님");
        Check(m.innerSize == 7, "SEQ_IMPORTANT 속 길이가 7 이 아님");
        Check(r.offset() == nox::MsgSeqImportant::kHeaderSize, "머리 길이가 3 이 아님");
        Check(m.EncodeSize() == 10, "껍데기 전체 길이가 10 이 아님");
        // 속 첫 바이트가 곧 속 메시지의 오프코드다
        Check(body[r.offset()] == static_cast<uint8_t>(nox::Op::REPORT_CLIENT_STATUS),
              "껍데기 속 오프코드가 0x6A 가 아님");
    }

    // a8 d0 02 02 96 0e 0a 0b 02 00 00 74 00  — 번호 720 · UTF-8 · (3734, 2826) · 두 글자
    {
        auto m = Parse<nox::MsgTextMessage>(
            "TEXT_MESSAGE",
            {0xd0, 0x02, 0x02, 0x96, 0x0e, 0x0a, 0x0b, 0x02, 0x00, 0x00, 0x74, 0x00}, 12);
        Check(m.netCode == 720, "TEXT_MESSAGE 번호가 720 이 아님");
        Check(m.posX == 3734 && m.posY == 2826, "TEXT_MESSAGE 말풍선 좌표가 표본과 다름");
        Check(m.size == 2 && m.data.size() == 2, "TEXT_MESSAGE 글자 길이가 2 가 아님");
        // 깃발에서 UTF-8 을 빼면 같은 size 라도 두 배로 읽어야 한다
        nox::MsgTextMessage u16 = m;
        u16.flags = 0;
        Check(u16.DataSize() == 4, "UTF-16 일 때 글자 길이가 두 배가 안 됨");
    }

    std::printf("  게임 중 메시지 11종 — 캡처한 실제 바이트로 뜯음\n");
}

int main() {
    std::printf("게임 포트 %u/UDP\n\n", nox::kGamePort);

    std::printf("[왕복]\n");
    nox::MsgServerDiscover d;
    d.token = 0x11223344;
    RoundTrip("SERVER_DISCOVER", d, nox::MsgServerDiscover::kSize);

    nox::MsgServerAccept a;
    a.id = 42;
    a.xorKey = 0x5A;
    RoundTrip("SERVER_ACCEPT", a, nox::MsgServerAccept::kSize);

    nox::MsgServerPing p;
    p.ind = 1;
    p.timeMs = 1234;
    RoundTrip("SERVER_PING", p, nox::MsgServerPing::kSize);

    nox::MsgClientPing cp;
    cp.timeMs = 5678;
    RoundTrip("CLIENT_PING", cp, nox::MsgClientPing::kSize);

    nox::MsgServerError e;
    e.err = nox::ConnectError::WrongVer;
    RoundTrip("SERVER_ERROR", e, nox::MsgServerError::kSize);

    TestServerInfo();
    TestTryJoin();

    std::printf("\n[표·경계]\n");
    TestGameplayMessages();
    TestOpcodeTable();
    TestSpellNames();
    TestBounds();

    std::printf("\n%s\n", g_failed == 0 ? "전부 통과" : "실패 있음");
    return g_failed == 0 ? 0 : 1;
}
