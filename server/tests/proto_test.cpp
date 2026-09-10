// proto_test.cpp — 이식한 메시지 정의가 스스로 앞뒤가 맞는지만 본다.
//
// 여기서 통과한다고 클라가 받아준다는 뜻은 아니다.
// 진짜 검증은 캡처와 대조하는 것이고, 이건 그 전에 오타를 거르는 용도다.
//
// 빌드:
//   cl /std:c++17 /EHsc /I server\include server\tests\proto_test.cpp server\src\Opcode.cpp
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nox/Messages.h"
#include "nox/Server.h"
#include "nox/UpdateStream.h"
#include "nox/SpellNames.h"
#include "nox/Opcode.h"
#include "nox/ThingDb.h"
#include "nox/ThingFlags.h"
#include "nox/NoxCrypt.h"
#include "nox/BlowfishTables.h"
#include "nox/MapFile.h"
#include "nox/JoinBundle.h"

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

    // 70 1b  — 주문 27. **주문 이름표와 맞물려** 본다. 표가 어긋나면 여기서 걸린다
    {
        auto m = Parse<nox::MsgReportSpellStart>("REPORT_SPELL_START", {0x1b},
                                                 nox::MsgReportSpellStart::kSize);
        Check(m.spell == 27, "REPORT_SPELL_START 주문이 27 이 아님");
        Check(std::strcmp(nox::SpellName(m.spell), "SPELL_FIREBALL") == 0,
              "주문 27 이 SPELL_FIREBALL 이 아님");
    }

    // a9 0c 01 00 00 00  — 갈래 12 Observer, 본문 4바이트
    {
        auto m = Parse<nox::MsgInform>("INFORM(Observer)", {0x0c, 0x01, 0x00, 0x00, 0x00}, 5);
        Check(m.code == nox::InformCode::Observer, "INFORM 갈래가 Observer 가 아님");
        Check(m.body.size() == 4, "Observer 본문이 4바이트가 아님");
    }

    // a9 0f 00 "use.c:HadAbility" 00  — 갈래 15 StringID. **글자가 아니라 CSF 열쇠**다
    {
        std::vector<uint8_t> body = {0x0f, 0x00};
        const char* key = "use.c:HadAbility";
        for (const char* p = key; *p; ++p) body.push_back(static_cast<uint8_t>(*p));
        body.push_back(0);

        auto m = Parse<nox::MsgInform>("INFORM(StringID)", body, body.size());
        Check(m.code == nox::InformCode::StringID, "INFORM 갈래가 StringID 가 아님");
        Check(m.StringId() == "use.c:HadAbility", "CSF 열쇠가 표본과 다름");
    }

    // 갈래마다 본문 길이가 달라야 한다 — 하나로 뭉뚱그리면 파서가 어긋난다
    Check(nox::MsgInform::BodySize(nox::InformCode::ClassChanged) == 0, "ClassChanged 본문이 0 이 아님");
    Check(nox::MsgInform::BodySize(nox::InformCode::SpellOK) == 4, "SpellOK 본문이 4 가 아님");
    Check(nox::MsgInform::BodySize(nox::InformCode::FlagCapture) == 8, "FlagCapture 본문이 8 이 아님");
    Check(nox::MsgInform::BodySize(nox::InformCode::Kill) == 9, "Kill 본문이 9 가 아님");
    Check(nox::MsgInform::BodySize(nox::InformCode::StringID) == -1, "StringID 가 가변으로 안 잡힘");

    std::printf("  게임 중 메시지 13종 — 캡처한 실제 바이트로 뜯음\n");
}

// ── 서버 로직 — 죽음과 부활이 실제로 도는가 ────────────────────
//
// 위 시험들은 「바이트를 제대로 뜯나」를 본다. 여기는 **흐름이 도나**를 본다.
// 둘은 다른 것이다 — 짜임이 맞아도 순서나 껍데기가 틀리면 게임은 안 돈다.

// 나간 조각에서 메시지 부분만 꺼낸다(머리 2바이트를 뗀다). 키를 안 준 상대라 평문이다.
std::vector<uint8_t> Body(const nox::OutPacket& p) {
    return std::vector<uint8_t>(p.data.begin() + nox::kWireHeaderSize, p.data.end());
}

// 시험 기본 캐릭터는 **위저드**다(DebugEnterGame 기본값) — 체력 75·힘 35.
//   맨손 한 대 = 힘 35 × 0.04 + 5 = 6.4 → 6
//   맞는 쪽도 위저드라 방어합 0.23(로브 0.20 + 평상복 셋 각 0.01) →
//   실제로 깎이는 값 = 6 × 0.77 = 4.62 → **5**
// 전에는 누구나 12 로 고정이었고 방어구도 없었다.
const int kWizardPunch = nox::ApplyArmor(
    nox::MeleeDamageFor(nox::CharClass::Wizard,
                        nox::StrengthOnline(nox::CharClass::Wizard), /*kick=*/false),
    nox::LoadoutFor(nox::CharClass::Wizard).armorSum,
    nox::DamageKind::Physical);

// out 안에서 이 오프코드로 시작하는 메시지를 찾는다. 껍데기(0xcc) 속도 들여다본다.
const std::vector<uint8_t>* FindMsg(const std::vector<std::vector<uint8_t>>& msgs, uint8_t op) {
    for (const auto& m : msgs) {
        if (!m.empty() && m[0] == op) return &m;
    }
    return nullptr;
}

// 나간 조각들을 메시지 목록으로 편다. 0xcc 껍데기는 벗겨 속을 넣는다.
std::vector<std::vector<uint8_t>> Flatten(const std::vector<nox::OutPacket>& out) {
    std::vector<std::vector<uint8_t>> msgs;
    for (const auto& p : out) {
        std::vector<uint8_t> b = Body(p);
        if (b.size() >= 4 && b[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) {
            const size_t inner = b[3];
            if (4 + inner <= b.size()) {
                msgs.emplace_back(b.begin() + 4, b.begin() + 4 + inner);
                continue;
            }
        }
        msgs.push_back(std::move(b));
    }
    return msgs;
}

// 받는 쪽을 가려서 푼다 — **누구에게 갔는지**를 봐야 하는 시험용.
std::vector<std::vector<uint8_t>> FlattenTo(const std::vector<nox::OutPacket>& out,
                                            const std::string& to) {
    std::vector<nox::OutPacket> mine;
    for (const auto& p : out) if (p.to == to) mine.push_back(p);
    return Flatten(mine);
}

void TestDeathFlow() {
    nox::Server srv;
    const std::string who = "10.0.0.1:1000";
    srv.DebugEnterGame(who, 728);
    Check(srv.IsAlive(who), "들어오자마자 살아 있지 않음");
    Check(srv.HealthOf(who) == nox::kDefaultMaxHealth, "처음 체력이 최대가 아님");

    // ① 안 죽을 만큼 때린다 — 증감과 절대값이 둘 다 나가야 한다
    {
        std::vector<nox::OutPacket> out;
        const bool died = srv.Damage(who, 30, out);
        Check(!died, "30 맞고 죽으면 안 됨");
        Check(srv.HealthOf(who) == 45, "75-30 이 45 가 아님");

        auto msgs = Flatten(out);
        const auto* d = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
        Check(d != nullptr, "0x42 가 안 나감");
        if (d) {
            Check(d->size() == 5, "0x42 가 5바이트가 아님");
            Check((*d)[1] == 0xd8 && (*d)[2] == 0x02, "0x42 의 번호가 728 이 아님");
            const int16_t delta = static_cast<int16_t>((*d)[3] | ((*d)[4] << 8));
            Check(delta == -30, "0x42 의 증감이 -30 이 아님");
        }
        const auto* h = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH));
        Check(h != nullptr, "0x43 이 안 나감 (껍데기 안에 있어야 한다)");
        if (h) Check(((*h)[1] | ((*h)[2] << 8)) == 45, "0x43 값이 45 가 아님");

        // 0x43 은 **껍데기 안**에 있어야 한다 — 겉으로 나가면 틀린 것이다
        bool wrapped = false;
        for (const auto& p : out) {
            auto b = Body(p);
            if (!b.empty() && b[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) wrapped = true;
        }
        Check(wrapped, "0xcc 껍데기가 안 쓰임");
    }

    // ② 죽인다
    {
        std::vector<nox::OutPacket> out;
        const bool died = srv.Damage(who, 100, out);
        Check(died, "100 맞고 안 죽음");
        Check(srv.HealthOf(who) == 0, "죽었는데 체력이 0 이 아님");
        Check(!srv.IsAlive(who), "죽었는데 살아 있다고 나옴");

        auto msgs = Flatten(out);
        const auto* e = FindMsg(msgs, static_cast<uint8_t>(nox::Op::PLAYER_DIED));
        Check(e != nullptr, "0xe8 이 안 나감");
        if (e) {
            Check(e->size() == 3, "0xe8 이 3바이트가 아님");
            Check((*e)[1] == 0xd8 && (*e)[2] == 0x02, "0xe8 의 번호가 728 이 아님");
        }
        // 체력이 0 밑으로 안 내려가야 한다 — 증감은 남은 만큼만
        const auto* d = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
        if (d) {
            const int16_t delta = static_cast<int16_t>((*d)[3] | ((*d)[4] << 8));
            Check(delta == -45, "남은 45 만 깎여야 하는데 아님");
        }
    }

    // ③ 죽어 있는 동안 또 때려도 아무 일 없어야 한다
    {
        std::vector<nox::OutPacket> out;
        Check(!srv.Damage(who, 10, out), "죽은 사람이 또 죽음");
        Check(out.empty(), "죽은 사람을 때렸는데 뭔가 나감");
    }

    // ④ 시간이 지나면 되살아난다
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);                    // 첫 호출은 기준점만 잡는다
        out.clear();
        // 부활까지 필요한 시간 + 여유. 기본 틱이 33ms 다
        srv.Tick(33 * (nox::kRespawnDelayTicks + 2), out);
        Check(srv.IsAlive(who), "때가 지났는데 안 살아남");
        Check(srv.HealthOf(who) == nox::kDefaultMaxHealth, "되살아났는데 체력이 최대가 아님");

        auto msgs = Flatten(out);
        const auto* r = FindMsg(msgs, static_cast<uint8_t>(nox::Op::PLAYER_RESPAWN));
        Check(r != nullptr, "0xe9 가 안 나감");
        if (r) {
            Check(r->size() == 9, "0xe9 가 9바이트가 아님");
            Check((*r)[1] == 0xd8 && (*r)[2] == 0x02, "0xe9 의 번호가 728 이 아님");
            const uint32_t tick = (*r)[3] | ((*r)[4] << 8) | ((*r)[5] << 16) |
                                  (static_cast<uint32_t>((*r)[6]) << 24);
            Check(tick == srv.frame(), "0xe9 의 4바이트가 지금 틱이 아님");
            Check((*r)[7] == 0xff && (*r)[8] == 0x01, "0xe9 끝 두 바이트가 표본과 다름");
        }
        const auto* tot = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH));
        Check(tot != nullptr, "0xdd 가 안 나감");
        if (tot) {
            Check(((*tot)[3] | ((*tot)[4] << 8)) == nox::kDefaultMaxHealth, "0xdd 의 지금 체력이 다름");
            Check(((*tot)[5] | ((*tot)[6] << 8)) == nox::kDefaultMaxHealth, "0xdd 의 최대 체력이 다름");
        }
    }

    std::printf("  죽음·부활 흐름 — 맞음→죽음→기다림→부활\n");
}

// ── 두루 보내기 — 남이 맞고·죽고·되살아나는 것이 **내 화면에도** 오는가 ─────
//
// 전에는 피해·죽음·부활을 **맞은 본인에게만** 보냈다. 옆 사람 화면에서는 아무 일도
// 안 일어났고, 나간 사람 몸은 영영 서 있었다.
//
// ★ 이 시험이 앞선 것들과 다른 점 = **받는 쪽을 가려서 본다**(`FlattenTo`).
//   `Flatten` 은 나간 것을 다 합치므로 「본인에게만 갔다」와 「모두에게 갔다」를 **못 가른다.**
//   실제로 이 프로젝트에서 시험이 통과했는데 한 줄도 안 고쳐진 적이 있다.
void TestBroadcastToOthers() {
    nox::Server srv;
    const std::string a = "10.9.0.1:41001";
    const std::string b = "10.9.0.2:41002";
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
    srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);

    // ① 피해 — a 가 맞으면 **b 에게도** a 의 번호로 `0x42` 가 가야 한다
    {
        std::vector<nox::OutPacket> out;
        srv.Damage(a, 30, out);
        const auto toB = FlattenTo(out, b);
        const auto* d = FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
        Check(d != nullptr, "남이 맞은 0x42 가 옆 사람에게 안 감");
        if (d) {
            Check(d->size() == 5, "방송된 0x42 가 5바이트가 아님");
            Check((*d)[1] == 0xd8 && (*d)[2] == 0x02,
                  "방송된 0x42 의 번호가 맞은 사람(728)이 아님");
            const int16_t delta = static_cast<int16_t>((*d)[3] | ((*d)[4] << 8));
            Check(delta == -30, "방송된 0x42 의 증감이 -30 이 아님");
        }
        // ⚠⚠ **번호가 없는 `0x43` 은 남에게 새면 안 된다.** 받는 쪽이 제 것으로 읽어
        //     남의 체력바가 망가진다. 이게 이 조각에서 제일 쉽게 저지를 실수다.
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH)) == nullptr,
              "번호 없는 0x43 이 남에게 새어 나감");
        // 본인은 당연히 둘 다 받는다
        const auto toA = FlattenTo(out, a);
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH)) != nullptr,
              "본인이 0x43 을 못 받음");
    }

    // ② 죽음 — `0xE8` 이 옆 사람에게
    {
        std::vector<nox::OutPacket> out;
        srv.Damage(a, srv.HealthOf(a) + 100, out);
        Check(!srv.IsAlive(a), "치명타를 맞고도 안 죽음");
        const auto toB = FlattenTo(out, b);
        const auto* d = FindMsg(toB, static_cast<uint8_t>(nox::Op::PLAYER_DIED));
        Check(d != nullptr, "남이 죽은 0xE8 이 옆 사람에게 안 감");
        if (d) {
            Check(d->size() == 3, "방송된 0xE8 이 3바이트가 아님");
            Check((*d)[1] == 0xd8 && (*d)[2] == 0x02, "방송된 0xE8 의 번호가 죽은 사람이 아님");
        }
    }

    // ③ 부활 — `0xE9`·`0xDD` 가 옆 사람에게
    {
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);                                        // 첫 호출은 기준점만
        std::vector<nox::OutPacket> out;
        srv.Tick(33 * (nox::kRespawnDelayTicks + 2), out);
        Check(srv.IsAlive(a), "기다렸는데 안 되살아남");
        const auto toB = FlattenTo(out, b);
        const auto* r = FindMsg(toB, static_cast<uint8_t>(nox::Op::PLAYER_RESPAWN));
        Check(r != nullptr, "남이 되살아난 0xE9 가 옆 사람에게 안 감");
        if (r) {
            Check(r->size() == 9, "방송된 0xE9 가 9바이트가 아님");
            Check((*r)[1] == 0xd8 && (*r)[2] == 0x02, "방송된 0xE9 의 번호가 다름");
        }
        const auto* tot = FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH));
        Check(tot != nullptr, "남의 되살아난 체력 0xDD 가 옆 사람에게 안 감");
        if (tot) Check(tot->size() == 7, "방송된 0xDD 가 7바이트가 아님");
    }

    std::printf("  두루 보내기 — 피해·죽음·부활이 옆 사람에게 · 0x43 은 안 새어 나감\n");
}

// 받는 쪽으로 간 **생바이트** 안에서 이 토막을 몇 번 보는가.
//
// ⚠ `FlattenTo` 로는 이걸 못 센다 — 갱신 뒤에 이어 붙인 메시지는 `0xa4` 한 덩어리
//   안에 들어가 있어 「메시지 목록」으로 갈라지지 않는다. 그래서 바이트로 센다.
size_t CountBytes(const std::vector<nox::OutPacket>& out, const std::string& to,
                  const std::vector<uint8_t>& pat) {
    size_t hits = 0;
    for (const auto& p : out) {
        if (p.to != to) continue;
        const std::vector<uint8_t> b = Body(p);
        if (b.size() < pat.size()) continue;
        for (size_t i = 0; i + pat.size() <= b.size(); ++i)
            if (std::equal(pat.begin(), pat.end(), b.begin() + i)) ++hits;
    }
    return hits;
}

// ── 시야 이탈 — 멀어지면 `32` 로 지우는가 ──────────────────────────────
//
// 전에는 「반경에 들 때 한 번 소개」만 있고 나갈 때가 없었다. 클라는 한 번 본 것을
// 영영 들고 있었다.
// ★ 정품도 이 메시지를 쓴다 — **서버→클라 156건**
//   (`_evidence/CAP_server_0906.txt`, 2026-09-10 `nox_opcount` 대조). 지어낸 것이 아니다.
// ★ 판정은 **가까울 때 0 · 멀 때 1** 의 차이로 본다. 한쪽만 보면 우연히 같은
//   세 바이트가 갱신 속에 있었을 때 속는다.
void TestAoiSightExit() {
    nox::Server srv;
    srv.SetAoi(true);
    // 시험용 번호 0x4321 — 갱신 속 우연한 세 바이트와 겹칠 확률을 낮추려고 고른 값.
    const uint16_t kCode = 0x4321;
    std::vector<nox::Server::WorldStatic> st;
    st.push_back({ kCode, 1234, 1000, 1000 });
    srv.SetWorldStatics(st);

    const std::string a = "10.9.1.1:41011";
    srv.DebugEnterGame(a, 728);
    srv.DebugMoveTo(a, 1000, 1000);

    const std::vector<uint8_t> pat = {
        static_cast<uint8_t>(nox::Op::OBJECT_OUT_OF_SIGHT), 0x21, 0x43,
    };

    std::vector<nox::OutPacket> warm;
    srv.Tick(0, warm);                     // 첫 호출은 기준점만 잡는다

    // ① 바로 옆에 있을 때 — 소개는 되고 `32` 는 나가면 안 된다
    size_t nearHits = 0;
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(33, out);
        nearHits = CountBytes(out, a, pat);
        Check(nearHits == 0, "옆에 있는데 벌써 32 가 나감");
    }

    // ② 시야 밖으로 걸어가면 — `32 <번호2>` 가 나가야 한다
    {
        srv.DebugMoveTo(a, 1000 + static_cast<int>(nox::kAoiDropRadius) + 300, 1000);
        std::vector<nox::OutPacket> out;
        srv.Tick(66, out);
        const size_t farHits = CountBytes(out, a, pat);
        Check(farHits > nearHits, "시야 밖으로 갔는데 32 가 안 나감");
    }

    // ③ 되돌아오면 **다시 소개**돼야 한다 — `introAt` 에서 지웠어야 성립한다
    {
        srv.DebugMoveTo(a, 1000, 1000);
        std::vector<nox::OutPacket> out;
        srv.Tick(99, out);
        // 소개는 `ff` + 전체번호라 갱신 안에 종류·좌표까지 실린다. 번호 두 바이트만 본다.
        const std::vector<uint8_t> code = { 0x21, 0x43 };
        Check(CountBytes(out, a, code) > 0, "되돌아왔는데 다시 소개가 안 됨");
    }

    std::printf("  시야 이탈 — 멀어지면 32 · 되돌아오면 재소개 (정품 156건 대조)\n");
}

// ── 주문 마나 비용 — thing.bin 에서 읽은 값이 **정품 콘솔과 같은가** ──────────
//
// SPEL 섹션의 이름 뒤 첫 바이트를 마나 비용으로 읽는다. 그게 맞다는 근거는
// 정품 콘솔 `list spells` 가 뱉은 115줄(`_evidence/CONSOLE_list_spells.txt`,
// `이름 <탭> Cost:NNN`)이다.
//
// ⚠ **줄 순서로 맞추면 안 된다.** 콘솔은 보이는 이름(`Ring of Fire`) 차례고
//   파일은 속이름(`SPELL_CLEANSING_FLAME`) 차례라 자리가 어긋난다.
//   그래서 **정렬해서 전수 대조**한다 — 115개 값이 하나도 안 틀려야 통과.
void TestSpellMana() {
    nox::ThingDb tdb;
    if (!tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
        std::printf("  주문 마나 — 건너뜀(thing.bin 없음)\n");
        return;
    }
    Check(!tdb.Spells().empty(), "SPEL 섹션이 비었다 — 파서가 그 섹션을 안 읽는다");

    FILE* f = std::fopen(
        "C:\\Users\\USER\\Desktop\\MyGit\\Nox_Server\\_evidence\\CONSOLE_list_spells.txt", "rb");
    if (!f) {
        // 증거 파일은 `_evidence/`(git 밖)라 없을 수 있다. 그때는 값 대조를 건너뛴다.
        // ⚠ 걸음 자체는 `TestThingDb` 가 지킨다 — SPEL 을 잘못 걸으면 뒤의 THNG 2,874종이 무너진다.
        std::printf("  주문 마나 — %zu개 읽음. 콘솔 정답지가 없어 값 대조는 건너뜀\n",
                    tdb.Spells().size());
        return;
    }
    std::vector<int> console;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        const char* at = std::strstr(line, "Cost:");
        if (at) console.push_back(std::atoi(at + 5));
    }
    std::fclose(f);

    Check(console.size() == tdb.Spells().size(),
          "콘솔 줄 수와 SPEL 칸 수가 다르다 — 걸음이 밀렸다");

    std::vector<int> parsed;
    parsed.reserve(tdb.Spells().size());
    for (const auto& s : tdb.Spells()) parsed.push_back(s.mana);
    std::sort(parsed.begin(), parsed.end());
    std::sort(console.begin(), console.end());
    Check(parsed == console, "마나 비용이 콘솔과 다르다 — 읽는 바이트 자리가 틀렸다");

    // 이름을 짝지어 아는 것 몇 개는 자리까지 확인한다(정렬 대조는 자리를 안 본다).
    struct KV { const char* name; int mana; };
    const KV known[] = {
        { "SPELL_ANCHOR", 10 }, { "SPELL_CHANNEL_LIFE", 0 }, { "SPELL_COUNTERSPELL", 20 },
        { "SPELL_CURE_POISON", 30 }, { "SPELL_DEATH", 85 }, { "SPELL_CLEANSING_FLAME", 60 },
    };
    for (const KV& k : known) {
        bool have = false;
        const int got = tdb.ManaByName(k.name, &have);
        const std::string miss = std::string("SPEL 에 ") + k.name + " 이 없다";
        const std::string diff = std::string(k.name) + " 의 마나가 콘솔과 다르다";
        Check(have, miss.c_str());
        Check(got == k.mana, diff.c_str());
    }

    // 우리가 실제로 쏘는 주문들이 표에 있는가 — 없으면 그 주문은 공짜가 된다.
    const int fired[] = { 27, 43, 24, 41, 4, 51, 75, 86, 87 };
    int missing = 0;
    for (int id : fired) {
        bool have = false;
        tdb.ManaByName(nox::SpellName(id), &have);
        if (!have) ++missing;
    }
    std::printf("  주문 마나 — %zu개 전수 일치(정품 콘솔 대조) · 우리가 쏘는 %zu개 중 표에 없는 것 %d\n",
                tdb.Spells().size(), sizeof(fired) / sizeof(fired[0]), missing);
}

// 아래에서 정의한다 — 시전 조각 하나를 만든다.
std::vector<uint8_t> SpellPacket(uint8_t spell, bool onSelf);

// ── 마나가 실제로 깎이는가 · 모자라면 못 쓰는가 ─────────────────────────
//
// 전에는 `maxMana` 를 쓰기만 하고 읽는 데가 없어 **주문이 무한이었다.**
// ★ 표를 시험 안에서 직접 넣는다 — thing.bin 이 없는 자리에서도 이 논리는 돌아야 한다.
void TestManaSpend() {
    nox::Server srv;
    const std::string a = "10.9.2.1:41021";
    const std::string b = "10.9.2.2:41022";
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
    srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
    const uint16_t full = srv.ManaOf(a);
    Check(full == nox::ManaOnline(nox::CharClass::Wizard), "들어오자마자 마나가 최대가 아님");

    const uint8_t kSpell = 27;      // 파이어볼
    const uint8_t kCost = 30;
    srv.SetSpellCost(kSpell, kCost);
    Check(srv.SpellCost(kSpell) == kCost, "넣은 비용이 안 들어감");

    // ① 한 번 쏘면 그만큼 깎이고 `0x45` 가 **본인에게만** 간다
    {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = SpellPacket(kSpell, /*onSelf=*/false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.ManaOf(a) == full - kCost, "시전했는데 마나가 안 깎임");

        const auto toA = FlattenTo(out, a);
        const auto* m = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_MANA));
        Check(m != nullptr, "0x45 가 안 나감");
        if (m) {
            Check(m->size() == 5, "0x45 가 5바이트가 아님");
            Check((*m)[1] == 0xd8 && (*m)[2] == 0x02, "0x45 의 번호가 시전자가 아님");
            const int mana = (*m)[3] | ((*m)[4] << 8);
            Check(mana == full - kCost, "0x45 가 실은 값이 지금 마나가 아님");
        }
        // ⚠ 홀로 보내는 메시지다 — 남에게 가면 조각만 헛되이 태운다
        const auto toB = FlattenTo(out, b);
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_MANA)) == nullptr,
              "0x45 가 남에게도 감");
    }

    // ② 마나가 바닥나면 **시전 자체가 안 일어난다**
    {
        const int room = srv.ManaOf(a) / kCost;
        for (int i = 0; i < room; ++i) {
            std::vector<nox::OutPacket> out;
            const std::vector<uint8_t> pkt = SpellPacket(kSpell, false);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        Check(srv.ManaOf(a) < kCost, "다 쓰고도 비용만큼 남아 있음");

        const unsigned long long before = srv.spellsNoMana();
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = SpellPacket(kSpell, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.spellsNoMana() == before + 1, "마나가 없는데 튕긴 것으로 안 셈");
        // 시전 알림(0x70)이 나가면 안 된다 — 못 쓴 주문이다
        const auto toA = FlattenTo(out, a);
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) == nullptr,
              "마나가 없는데 시전 알림이 나감");
    }

    // ③ 비용을 모르는 주문(표에 없음)은 공짜로 나간다 — **그건 「0」이 아니라 「모름」이다**
    {
        const uint16_t was = srv.ManaOf(a);
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = SpellPacket(/*spell=*/43, false);   // 표에 안 넣었다
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.ManaOf(a) == was, "표에 없는 주문이 마나를 깎았다");
    }

    std::printf("  마나 — 시전에 깎임 · 바닥나면 못 씀 · 0x45 는 본인에게만\n");
}

// 오프코드 하나 + 번호 두 바이트짜리 조각. `0x73`·`0xF1` 이 같은 꼴이다.
std::vector<uint8_t> CodePacket(nox::Op op, uint16_t netCode) {
    std::vector<uint8_t> p(nox::kWireHeaderSize + 3, 0);
    p[0] = 0x00;   // 비신뢰 · 스트림 0
    p[1] = 0x00;
    p[nox::kWireHeaderSize + 0] = static_cast<uint8_t>(op);
    p[nox::kWireHeaderSize + 1] = static_cast<uint8_t>(netCode);
    p[nox::kWireHeaderSize + 2] = static_cast<uint8_t>(netCode >> 8);
    return p;
}

// ── 판 흐름 — 팀 배정과 판 다시 열기 ────────────────────────────────────
//
// ★ 전에 두 군데가 틀렸다 (2026-09-11 감사) —
//   ① 팀을 `sessions_` 순회로 나눴는데 그 열쇠가 **주소 문자열**이라 **사전순**이었다.
//      「들어온 차례로」라는 주석과 실제가 달랐고, 포트가 어떻게 뽑히느냐에 따라 팀이 뒤집혔다.
//   ② **두 명이 찬 그 순간 한 번만** 나눠서, 셋째부터 팀 0 으로 남아 깃발 판정에서 빠졌다.
// ★ 그리고 **판이 한 번 끝나면 다시 못 열었다** — `roundOver_` 를 되돌리는 데가
//   `StartQuest` 하나뿐이었다.
void TestRoundFlow() {
    nox::Server srv;
    // 주소를 **사전순과 접속순이 어긋나게** 고른다 — 이게 이 시험의 핵심이다.
    //   사전순: "10.0.0.1" < "10.0.0.2" < "10.0.0.3"
    //   접속순: c(10.0.0.3) → a(10.0.0.1) → b(10.0.0.2)
    const std::string a = "10.0.0.1:1", b = "10.0.0.2:2", c = "10.0.0.3:3";
    srv.DebugEnterGame(c, 728);
    srv.DebugEnterGame(a, 729);
    srv.DebugEnterGame(b, 730);
    srv.StartCtf(1000, 1000, 2000, 2000);

    // ① 팀은 **접속한 차례**로 갈린다 — 사전순이 아니라
    Check(srv.TeamOf(c) == 1, "첫째로 들어온 사람이 1팀이 아님 — 사전순으로 나눴다");
    Check(srv.TeamOf(a) == 2, "둘째로 들어온 사람이 2팀이 아님");
    Check(srv.TeamOf(b) == 1, "셋째로 들어온 사람이 1팀이 아님");

    // ② 판이 선 뒤에 들어온 사람도 팀을 받는다 (전에는 0 으로 남았다)
    {
        const std::string d = "10.0.0.4:4";
        srv.DebugEnterGame(d, 731);
        std::vector<uint8_t> pkt(nox::kWireHeaderSize + 1, 0);
        pkt[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::CLIENT_ACCEPT);
        // DebugEnterGame 은 CLIENT_ACCEPT 를 안 거치므로 직접 부른다
        srv.AssignTeams();
        Check(srv.TeamOf(d) != 0, "판이 선 뒤 들어온 사람이 팀 0 으로 남았다");
    }

    // ③ 깃발 3점이면 판끝, 그리고 **때가 되면 다시 열린다**
    {
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);
        Check(!srv.RoundOver(), "시작하자마자 판끝이면 안 된다");
        srv.DebugCapture(c, 3);            // 1팀이 3점
        Check(srv.RoundOver(), "3점인데 판끝이 아님");
        Check(srv.roundsPlayed() == 0, "아직 다시 안 열렸는데 판 수가 올랐다");

        std::vector<nox::OutPacket> out;
        srv.Tick(33 * (nox::kRoundRestartTicks + 2), out);
        Check(!srv.RoundOver(), "때가 됐는데 판이 안 다시 열렸다");
        Check(srv.roundsPlayed() == 1, "판 수가 안 올랐다");
        Check(srv.CapturesOf(1) == 0, "다시 열렸는데 점수가 안 지워졌다");
    }

    std::printf("  판 흐름 — 팀은 접속 차례로(사전순 아님) · 늦게 와도 배정 · 판끝 뒤 다시 열림\n");
}

// ── 마우스 조준 — 투사체가 겨눈 쪽으로 나는가 ───────────────────────────
//
// ⭕ `0xac MOUSE` 짜임은 실측 — 본문 4바이트 = 월드 좌표 x2·y2, 정품 표본 18,700+.
// ★ 전에는 투사체가 **늘 몸이 바라보는 쪽**으로만 났다(`s.facing`). 녹스는 마우스로
//   겨누는 게임인데 `0xac` 를 안 읽고 버리고 있었다.
void TestAim() {
    nox::Server srv;
    const std::string a = "10.9.7.1:41071";
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
    srv.DebugMoveTo(a, 1000, 1000);
    // 몸은 +x 를 본다(facing 기본 0). 겨눔은 **+y 쪽**으로 준다 — 갈리는 자리다.

    // ① `0xac` 를 받아야 한다 — 전에는 세지도 않았다
    {
        std::vector<uint8_t> pkt(nox::kWireHeaderSize + 5, 0);
        size_t i = nox::kWireHeaderSize;
        pkt[i++] = static_cast<uint8_t>(nox::Op::MOUSE);
        pkt[i++] = static_cast<uint8_t>(1000);        // x = 1000 (제자리)
        pkt[i++] = static_cast<uint8_t>(1000 >> 8);
        pkt[i++] = static_cast<uint8_t>(1600);        // y = 1600 (아래쪽)
        pkt[i++] = static_cast<uint8_t>(1600 >> 8);
        std::vector<nox::OutPacket> out;
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.aims() == 1, "0xac 를 받은 것으로 안 셌다 — 아직 버리고 있다");
    }

    // ② 쏘면 **겨눈 쪽(+y)** 으로 난다. 몸 방향(+x)이 아니라
    {
        srv.SetSpellCost(27, 0);
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> sp = SpellPacket(27, /*onSelf=*/false);
        srv.OnPacket(a, sp.data(), sp.size(), out);
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);
        std::vector<nox::OutPacket> t1;
        srv.Tick(33, t1);
        Check(srv.flying() == 1, "투사체가 안 생겼다");
        const auto sh = srv.DebugShotAt(0);
        Check(sh.second > 1000, "투사체가 겨눈 쪽(+y)으로 안 갔다");
        Check(std::abs(sh.first - 1000.0) < 3.0,
              "투사체가 몸 방향(+x)으로 갔다 — 겨눔을 안 쓴다");
    }

    // ③ 겨눔이 낡으면 몸 방향으로 되돌아간다 (➖ 우리 값 kAimStaleTicks)
    {
        nox::Server s2;
        s2.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        s2.DebugMoveTo(a, 1000, 1000);
        s2.SetSpellCost(27, 0);
        s2.DebugAimAt(a, 1000, 1600);
        std::vector<nox::OutPacket> warm;
        s2.Tick(0, warm);
        std::vector<nox::OutPacket> aged;
        s2.Tick(33 * (nox::kAimStaleTicks + 5), aged);   // 겨눔을 늙힌다
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> sp = SpellPacket(27, false);
        s2.OnPacket(a, sp.data(), sp.size(), out);
        std::vector<nox::OutPacket> t1;
        s2.Tick(33 * (nox::kAimStaleTicks + 6), t1);
        const auto sh = s2.DebugShotAt(0);
        Check(sh.first > 1000, "겨눔이 낡았는데 몸 방향(+x)으로 안 갔다");
    }

    std::printf("  겨눔 — 0xac 를 읽고 투사체가 겨눈 쪽으로 · 낡으면 몸 방향으로\n");
}

// ── 채팅 — 친 말이 모두에게 가는가, 남의 번호로 말할 수 있는가 ──────────
//
// ⭕ **보내는 짜임은 정품 표본 26건으로 확정**이다:
//    `cb 02 02 20 0c cc 09 fa 00 00` + 글자
//     번호2 · 깃발1(0x02=UTF-8) · x2 · y2 · 크기1 · 미상2
// ⚠ **받는 짜임은 표본이 0건**이다(2026-09-10 전수). 「클라→서버 277건」으로 보이던 것은
//   조각을 잘못 쪼갠 잡음이었다 — 전부 같은 바이트, 전부 조각 한가운데.
//   그래서 **클라가 준 머리를 안 쓴다** — 글자만 꺼내고 번호·좌표는 서버가 새로 짓는다.
void TestChat() {
    nox::Server srv;
    const std::string a = "10.9.6.1:41061";
    const std::string b = "10.9.6.2:41062";
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);
    srv.DebugMoveTo(a, 0x0c20, 0x09cc);        // 정품 표본과 같은 좌표로 맞춰 본다

    // 클라가 보낸다고 보는 꼴 — 머리 10 + 글자. ★ 머리의 번호를 **남의 것(729)** 으로
    //   넣어 본다. 서버가 그것을 그대로 쓰면 「남의 번호로 말하기」가 된다.
    auto ChatPacket = [](uint16_t claimNet, const char* text) {
        const size_t n = std::strlen(text);
        std::vector<uint8_t> p(nox::kWireHeaderSize + 1 + 10 + n, 0);
        size_t i = nox::kWireHeaderSize;
        p[i++] = static_cast<uint8_t>(nox::Op::TEXT_MESSAGE);
        p[i++] = static_cast<uint8_t>(claimNet);
        p[i++] = static_cast<uint8_t>(claimNet >> 8);
        p[i++] = 0x02;                          // UTF-8
        p[i++] = 0x11; p[i++] = 0x11;           // 거짓 x
        p[i++] = 0x22; p[i++] = 0x22;           // 거짓 y
        p[i++] = static_cast<uint8_t>(n);
        p[i++] = 0; p[i++] = 0;
        std::memcpy(p.data() + i, text, n);
        return p;
    };

    {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = ChatPacket(/*claimNet=*/729, "hello");
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.chats() == 1, "채팅을 받은 것으로 안 셌다");

        // ① 말한 사람에게도, 남에게도 간다
        for (const std::string& to : { a, b }) {
            const auto msgs = FlattenTo(out, to);
            const auto* m = FindMsg(msgs, static_cast<uint8_t>(nox::Op::TEXT_MESSAGE));
            const std::string why = std::string("채팅이 ") + to + " 에게 안 감";
            Check(m != nullptr, why.c_str());
            if (!m) continue;
            Check(m->size() == 1 + 10 + 5, "채팅 길이가 머리10+글자5 가 아님");
            // ② ★ 번호는 **말한 사람(728)** 이라야 한다 — 클라가 우긴 729 가 아니라
            Check((*m)[1] == 0xd8 && (*m)[2] == 0x02,
                  "채팅 번호가 말한 사람이 아니다 — 남의 번호로 말하기가 뚫렸다");
            // ③ 좌표도 서버가 아는 값 — 클라가 준 거짓 좌표가 아니라
            Check((*m)[4] == 0x20 && (*m)[5] == 0x0c, "채팅 x 가 서버가 아는 값이 아님");
            Check((*m)[6] == 0xcc && (*m)[7] == 0x09, "채팅 y 가 서버가 아는 값이 아님");
            Check((*m)[3] == 0x02, "깃발이 UTF-8(0x02) 이 아님");
            Check((*m)[8] == 5, "크기 칸이 글자 수가 아님");
            Check(std::memcmp(m->data() + 11, "hello", 5) == 0, "글자가 다름");
        }
    }

    // ④ 빈 말은 안 보낸다
    {
        const unsigned long long before = srv.chats();
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = ChatPacket(728, "");
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.chats() == before, "빈 말을 방송했다");
    }

    std::printf("  채팅 — 모두에게 감 · 번호·좌표는 서버가 새로 지음(남의 번호로 말하기 막힘)\n");
}

// ── 판 시계 물음(0xd4) · 정상 종료(0x0a) ────────────────────────────────
//
// ★ `0xd4` 는 **조각을 죽이던 자리**다. 길이표가 `-1` 이라 그 조각을 통째로 버렸고,
//   같이 실린 정상 메시지까지 죽었다(09-10 실클라 런의 `[버림]` 228건이 전수 이것).
//   본문 0 은 실측 — 정품 클라 표본 336건이 전수 「남은 1」(홑 `d4`)이었다.
void TestTimerAskAndClose() {
    // ① 길이표 — 클라→서버만 본문 0, 서버→클라는 표본이 없으니 미상 그대로
    Check(nox::MsgBodyLenByDir(0xD4, /*fromClient=*/true) == 0,
          "0xd4 클라→서버 본문이 0 이 아님");
    Check(nox::MsgBodyLenByDir(0xD4, /*fromClient=*/false) < 0,
          "0xd4 서버→클라는 아직 미상이라야 한다 — 표본이 없다");

    nox::Server srv;
    const std::string a = "10.9.5.1:41051";
    const std::string b = "10.9.5.2:41052";
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);

    // ② `0xd4` 홑 한 바이트 조각 — 안 버려지고 `0xd3` 로 답해야 한다
    {
        std::vector<uint8_t> pkt(nox::kWireHeaderSize + 1, 0);
        pkt[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::REQUEST_TIMER_STATUS);
        std::vector<nox::OutPacket> out;
        const unsigned long long stopped = srv.stopped();
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.stopped() == stopped, "0xd4 조각이 버려졌다 — 길이표가 아직 미상이다");
        Check(srv.timerAsks() == 1, "0xd4 를 받은 것으로 안 셌다");
        const auto toA = FlattenTo(out, a);
        const auto* t = FindMsg(toA, static_cast<uint8_t>(nox::Op::TIMER_STATUS));
        Check(t != nullptr, "0xd4 에 0xd3 로 안 답함");
        if (t) Check(t->size() == 13, "0xd3 가 13바이트가 아님");
        // ⚠ 물어본 사람에게만 — 주기 발송은 따로 있다
        Check(FindMsg(FlattenTo(out, b), static_cast<uint8_t>(nox::Op::TIMER_STATUS)) == nullptr,
              "0xd4 즉답이 남에게도 감");
    }

    // ③ 정상 종료 — 30초를 안 기다리고 다음 틱에 나간다
    {
        std::vector<uint8_t> pkt(nox::kWireHeaderSize + 1, 0);
        pkt[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::CLIENT_CLOSE);
        std::vector<nox::OutPacket> out;
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.inGameCount() == 2, "받자마자 지우면 안 된다 — 잡고 있는 참조가 죽는다");

        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);
        std::vector<nox::OutPacket> t2;
        srv.Tick(33, t2);
        Check(srv.inGameCount() == 1, "정상 종료를 알렸는데 안 나감");
        const auto toB = FlattenTo(t2, b);
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::PLAYER_QUIT)) != nullptr,
              "나감 0x2E 가 남은 사람에게 안 감");
    }

    std::printf("  판 시계 물음·정상 종료 — 0xd4 안 버려지고 0xd3 즉답 · 0x0a 면 바로 나감\n");
}

// ── 줍기 — 집으면 내 것이 되고 바닥에서 사라지는가 ──────────────────────
//
// ★ 이 갈래는 **`switch` 까지 오지도 못했다.** `0x73` 의 길이가 `-1` 이라 조각을
//   훑는 걸음이 거기서 멈추고 조각을 통째로 버렸다. 길이표를 채워 풀었다.
// ★ `4c`(수식 붙는 것)의 짜임은 정품 표본 10건으로 확인했다 —
//   `4c cc 02 6d 04 ff bf ff ff` = 번호2 · 종류2 · 수식4(빈 칸은 `ff`).
void TestPickup() {
    nox::Server srv;
    const std::string a = "10.9.3.1:41031";
    const std::string b = "10.9.3.2:41032";
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);
    srv.DebugMoveTo(a, 1000, 1000);
    srv.DebugMoveTo(b, 1000, 1000);

    const uint16_t kPotion = 176;    // 물약 — 수식 안 붙는다 → `4b`
    const uint16_t kSword  = 180;    // 무기 — 수식 붙는다   → `4c`
    const uint16_t kFar    = 200;    // 멀리 있는 것
    std::vector<nox::Server::WorldStatic> st;
    st.push_back({ kPotion, 638, 1000, 1000, false });
    st.push_back({ kSword, 1133, 1005, 1000, true });
    st.push_back({ kFar,    638, 9000, 9000, false });
    srv.SetWorldStatics(st);
    Check(srv.WorldStaticCount() == 3, "정적 셋을 안 넣었다");

    // ① 발밑의 물약 — `4b`(5B) 는 나에게만, `31` 은 모두에게, 세계에서 사라진다
    {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = CodePacket(nox::Op::TRY_GET, kPotion);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);

        Check(srv.Carries(a, kPotion), "주웠는데 인벤토리에 없다");
        Check(srv.WorldStaticCount() == 2, "주웠는데 세계에서 안 없어졌다");

        const auto toA = FlattenTo(out, a);
        const auto* pk = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_PICKUP));
        Check(pk != nullptr, "0x4b 가 안 나감");
        if (pk) {
            Check(pk->size() == 5, "0x4b 가 5바이트가 아님");
            // ★ 첫 필드는 **물건 번호**다 — 집는 이(728)가 아니다
            Check((*pk)[1] == 0xb0 && (*pk)[2] == 0x00, "0x4b 의 첫 필드가 물건 번호가 아님");
            Check((*pk)[3] == 0x7e && (*pk)[4] == 0x02, "0x4b 의 종류가 638 이 아님");
        }
        // 소멸은 옆 사람에게도 가야 한다
        const auto toB = FlattenTo(out, b);
        const auto* de = FindMsg(toB, static_cast<uint8_t>(nox::Op::DESTROY_OBJECT));
        Check(de != nullptr, "0x31 이 옆 사람에게 안 감");
        // ⚠ 주운 사실 자체는 남에게 갈 것이 아니다
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_PICKUP)) == nullptr,
              "0x4b 가 남에게도 감");
    }

    // ② 수식 붙는 물건은 `4c`(9B) — 빈 수식 칸은 `ff`
    {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = CodePacket(nox::Op::TRY_GET, kSword);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        const auto toA = FlattenTo(out, a);
        const auto* pk = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_MODIFIABLE_PICKUP));
        Check(pk != nullptr, "0x4c 가 안 나감");
        if (pk) {
            Check(pk->size() == 9, "0x4c 가 9바이트가 아님");
            Check((*pk)[5] == 0xff && (*pk)[6] == 0xff && (*pk)[7] == 0xff && (*pk)[8] == 0xff,
                  "0x4c 의 빈 수식 칸이 ff 가 아님");
        }
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_PICKUP)) == nullptr,
              "수식 붙는 물건인데 4b 로 보냄");
    }

    // ③ 손이 안 닿으면 못 줍는다
    {
        const unsigned long long before = srv.pickTooFar();
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = CodePacket(nox::Op::TRY_GET, kFar);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.pickTooFar() == before + 1, "멀리 있는 것을 못 집은 것으로 안 셈");
        Check(!srv.Carries(a, kFar), "손도 안 닿는데 주워졌다");
        Check(srv.WorldStaticCount() == 1, "못 주웠는데 세계에서 없어졌다");
    }

    // ④ 클라가 `0xF1` 로 되돌리면 **도로 떨군다** — 안 그러면 물건이 세상에서 사라진다
    {
        const size_t worldBefore = srv.WorldStaticCount();
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> pkt = CodePacket(nox::Op::INVENTORY_FAIL, kPotion);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(!srv.Carries(a, kPotion), "되돌렸는데 아직 들고 있다");
        Check(srv.WorldStaticCount() == worldBefore + 1, "되돌렸는데 바닥에 안 놓였다");
    }

    std::printf("  줍기 — 4b/4c 갈라짐 · 31 은 모두에게 · 손 닿는 데까지 · F1 이면 도로 떨굼\n");
}

// ── 쓰기·입기 — 물약이 실제로 채우는가, 입은 것이 기록되는가 ──────────────
//
// ★ 회복량은 **파일에서 온다** — `thing.bin` 의 `USE = PotionUse 50`.
//   여기서는 그 값을 시험이 직접 넣어 논리만 본다(파일 대조는 `TestUseFromFile`).
// ⭕ `60 REPORT_EQUIP`(3B)은 정품 표본 22건으로 확인된 메시지다.
void TestUseAndEquip() {
    nox::Server srv;
    const std::string a = "10.9.4.1:41041";
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
    srv.DebugMoveTo(a, 1000, 1000);

    const uint16_t kRed = 176, kBlue = 177, kSword = 180, kBook = 181;
    std::vector<nox::Server::WorldStatic> st;
    st.push_back({ kRed,   638, 1000, 1000, false, nox::UseKind::Health, 50 });
    st.push_back({ kBlue,  639, 1000, 1000, false, nox::UseKind::Mana,   50 });
    st.push_back({ kSword, 1133, 1000, 1000, true,  nox::UseKind::None,   0 });
    st.push_back({ kBook,  700, 1000, 1000, false, nox::UseKind::None,   0 });
    srv.SetWorldStatics(st);
    for (uint16_t nc : { kRed, kBlue, kSword, kBook }) {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_GET, nc);
        srv.OnPacket(a, p.data(), p.size(), out);
    }
    Check(srv.CarriedCount(a) == 4, "넷을 다 못 주웠다");

    // ① 빨강 물약 — 다친 뒤에 쓰면 그만큼 찬다
    {
        std::vector<nox::OutPacket> hurt;
        srv.Damage(a, 40, hurt);
        const uint16_t low = srv.HealthOf(a);
        Check(low < srv.HealthOf(a) + 40, "안 다쳤다");

        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_USE, kRed);
        srv.OnPacket(a, p.data(), p.size(), out);
        // 위저드 최대 체력은 75 라 40 맞고 50 을 마시면 **최대치에서 멈춘다**
        Check(srv.HealthOf(a) == nox::HealthOnline(nox::CharClass::Wizard),
              "물약을 마셨는데 최대치까지 안 참");
        Check(!srv.Carries(a, kRed), "마신 물약이 아직 인벤토리에 있다");

        const auto toA = FlattenTo(out, a);
        const auto* d = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
        Check(d != nullptr, "회복 0x42 가 안 나감");
        if (d) {
            const int16_t delta = static_cast<int16_t>((*d)[3] | ((*d)[4] << 8));
            Check(delta > 0, "회복인데 증감이 양수가 아님");
            Check(delta == nox::HealthOnline(nox::CharClass::Wizard) - low,
                  "회복량이 실제로 찬 만큼이 아님");
        }
    }

    // ② 파란 물약 — 마나를 채우고 `0x45` 로 알린다
    {
        std::vector<nox::OutPacket> burn;
        srv.SetSpellCost(27, 30);
        for (int i = 0; i < 2; ++i) {
            const std::vector<uint8_t> sp = SpellPacket(27, false);
            srv.OnPacket(a, sp.data(), sp.size(), burn);
        }
        const uint16_t low = srv.ManaOf(a);
        Check(low < nox::ManaOnline(nox::CharClass::Wizard), "마나를 안 썼다");

        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_USE, kBlue);
        srv.OnPacket(a, p.data(), p.size(), out);
        Check(srv.ManaOf(a) == low + 50, "파란 물약이 50 을 안 채웠다");
        const auto toA = FlattenTo(out, a);
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_MANA)) != nullptr,
              "마나 0x45 가 안 나감");
    }

    // ③ 갈래를 모르는 것(책) — **아무 일도 안 일어나고 없어지지도 않는다.**
    //    「안 된다」를 「0 만큼 된다」로 꾸미지 않는 자리다.
    {
        const unsigned long long before = srv.useUnknown();
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_USE, kBook);
        srv.OnPacket(a, p.data(), p.size(), out);
        Check(srv.useUnknown() == before + 1, "모르는 갈래를 안 셌다");
        Check(srv.Carries(a, kBook), "아무 일도 안 났는데 없어졌다");
    }

    // ④ 입기·벗기 — `60`/`61` 이 본인에게, 상태가 기록된다
    {
        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> on = CodePacket(nox::Op::TRY_EQUIP, kSword);
        srv.OnPacket(a, on.data(), on.size(), out);
        Check(srv.IsEquipped(a, kSword), "입었는데 기록이 안 됨");
        const auto toA = FlattenTo(out, a);
        const auto* e = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_EQUIP));
        Check(e != nullptr, "0x60 이 안 나감");
        if (e) {
            Check(e->size() == 3, "0x60 이 3바이트가 아님");
            // ★ 실린 것은 **물건 번호**다 — 정품 `60 cc 02` 가 물건 716 을 가리키듯이
            Check((*e)[1] == 0xb4 && (*e)[2] == 0x00, "0x60 이 물건 번호가 아님");
        }

        std::vector<nox::OutPacket> out2;
        const std::vector<uint8_t> off = CodePacket(nox::Op::TRY_DEQUIP, kSword);
        srv.OnPacket(a, off.data(), off.size(), out2);
        Check(!srv.IsEquipped(a, kSword), "벗었는데 기록이 안 됨");
        Check(FindMsg(FlattenTo(out2, a), static_cast<uint8_t>(nox::Op::REPORT_DEQUIP)) != nullptr,
              "0x61 이 안 나감");
    }

    std::printf("  쓰기·입기 — 물약이 채움(최대에서 멈춤) · 모르는 갈래는 아무 일 없음 · 60/61\n");
}

// ── 물약 회복량이 **파일에서** 오는가 ──────────────────────────────────
//
// 위 시험은 논리만 본다. 이건 `thing.bin` 을 직접 열어 값을 확인한다 —
// 「우리가 넣은 50」이 아니라 「파일에 적힌 50」이라야 정본이다.
void TestUseFromFile() {
    nox::ThingDb tdb;
    if (!tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
        std::printf("  쓰기 정본 — 건너뜀(thing.bin 없음)\n");
        return;
    }
    struct KV { const char* name; const char* handler; int arg; const char* sub; };
    const KV known[] = {
        { "RedPotion",  "PotionUse", 50, "HEALTH_POTION" },
        { "BluePotion", "PotionUse", 50, "MANA_POTION" },
    };
    for (const KV& k : known) {
        const uint16_t wire = tdb.WireByName(k.name);
        const std::string miss = std::string(k.name) + " 을 표에서 못 찾음";
        Check(wire != 0, miss.c_str());
        if (!wire) continue;
        const nox::ThingInfo* ti = tdb.ByWire(wire);
        const std::string hb = std::string(k.name) + " 의 USE 다루는 이름이 다름";
        const std::string ab = std::string(k.name) + " 의 USE 숫자가 다름";
        const std::string sb = std::string(k.name) + " 의 SUBCLASS 가 다름";
        Check(ti && ti->useHandler == k.handler, hb.c_str());
        Check(ti && ti->useArg == k.arg, ab.c_str());
        Check(ti && ti->subclassRaw.find(k.sub) != std::string::npos, sb.c_str());
    }
    // 회복량을 담은 갈래가 실제로 얼마나 있나 — 0 이면 파서가 그 키를 안 읽는 것이다
    size_t withUse = 0;
    for (size_t i = 1; i <= tdb.Count(); ++i) {
        const nox::ThingInfo* ti = tdb.ByWire(static_cast<uint16_t>(i));
        if (ti && (ti->useHandler == "PotionUse" || ti->useHandler == "ConsumeUse")) ++withUse;
    }
    Check(withUse > 0, "USE 를 읽은 것이 하나도 없다 — 파서가 그 키를 안 본다");
    std::printf("  쓰기 정본 — 물약 USE = PotionUse 50 (파일) · 회복 갈래 %zu종\n", withUse);
}

// ── 주문이 실제로 사람을 깎는가 ────────────────────────────────
//
// 클라가 보내는 조각을 그대로 만들어 `OnPacket` 에 넣는다.
// 머리 2바이트 + `79 | 주문번호1 | 0 채움19 | 대상플래그1` (본문 21 = kTrySpellBodyLen).
std::vector<uint8_t> SpellPacket(uint8_t spell, bool onSelf) {
    std::vector<uint8_t> p(nox::kWireHeaderSize + 1 + nox::kTrySpellBodyLen, 0);
    p[0] = 0x00;   // 비신뢰 · 스트림 0
    p[1] = 0x00;   // 순번
    p[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::TRY_SPELL);
    p[nox::kWireHeaderSize + 1] = spell;
    p.back() = onSelf ? 1 : 0;
    return p;
}

// 시전한 뒤 투사체가 날아가 닿을 때까지 틱을 돌린다.
// 돌려주는 값 = 그 사이에 나간 조각들.
void FlyUntilDone(nox::Server& srv, std::vector<nox::OutPacket>& out, int maxTicks = 40) {
    uint32_t ms = 1;
    srv.Tick(0, out);
    for (int i = 0; i < maxTicks && srv.flying() > 0; ++i) {
        ms += 33;
        srv.Tick(ms, out);
    }
}

void TestSpellDamage() {
    nox::Server srv;
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    // ★ **둘 다 워리어로 둔다** — 정본 파이어볼은 최대 세기에서 127 이라
    //   위저드(75)는 한 방에 죽는다. 여기서 보려는 것은 「날아가서 맞나」이므로
    //   맞고도 살아남는 워리어(150)로 재야 판정이 흐려지지 않는다.
    //   ⚠ 한 방에 죽는 것 자체는 결함이 아니다 — 정본이 그렇다.
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
    srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
    const int kHP = nox::HealthOnline(nox::CharClass::Warrior);
    // ★ 이제 파이어볼은 **날아간다.** 바라보는 쪽이 기본 0(월드 +x)이므로
    //   맞으려면 b 가 a 의 앞쪽에 있어야 한다. 같은 자리에 두면 안 맞는 게 옳다.
    srv.DebugMoveTo(b, 1100, 1000);

    // ① 파이어볼을 남에게 — 맞는 쪽이 깎여야 한다
    {
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, /*onSelf=*/false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        // ★ 쏘자마자는 아무도 안 깎여야 한다 — 날아가는 중이다
        Check(srv.HealthOf(b) == kHP, "쏘자마자 맞음 (투사체가 안 날아감)");
        Check(srv.flying() == 1, "날아가는 것이 하나가 아님");

        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == kHP - nox::Server::SpellDamage(27),
              "날아간 파이어볼이 안 맞음");
        Check(srv.HealthOf(a) == kHP, "쏜 쪽이 깎임");
        Check(srv.hits() == 1, "맞은 횟수가 1 이 아님");
        Check(srv.flying() == 0, "맞았는데 안 사라짐");

        // 시전 알림이 **보는 사람 모두**에게 나갔나
        int spellStarts = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            if (!body.empty() && body[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) {
                Check(body.size() == 2, "0x70 이 2바이트가 아님");
                Check(body[1] == 27, "0x70 의 주문이 27 이 아님");
                ++spellStarts;
            }
        }
        Check(spellStarts == 2, "0x70 이 둘에게 안 나감");
    }

    // ② 자기에게 걸면 자기가 깎인다 (대상 플래그 1)
    {
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, /*onSelf=*/true);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(a) == kHP - nox::Server::SpellDamage(27),
              "자기에게 건 파이어볼이 자기를 안 깎음");
        Check(srv.flying() == 0, "자기에게 건 것이 날아감");
    }

    // ③ 공격이 아닌 주문은 아무도 안 깎는다
    {
        const uint16_t before = srv.HealthOf(b);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(4, /*onSelf=*/false);   // 4 = 점멸
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == before, "점멸이 남을 깎음");
        Check(nox::Server::SpellDamage(4) == 0, "점멸이 공격으로 잡힘");
    }

    // ④ 계속 때리면 죽는다 — 흐름이 끝까지 이어지는지
    {
        int guard = 0;
        while (srv.IsAlive(b) && guard++ < 20) {
            std::vector<nox::OutPacket> out;
            auto pkt = SpellPacket(27, /*onSelf=*/false);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            FlyUntilDone(srv, out);
        }
        Check(!srv.IsAlive(b), "파이어볼을 스무 번 맞고도 안 죽음");
        Check(srv.HealthOf(b) == 0, "죽었는데 체력이 0 이 아님");
    }

    // ⑤ 죽은 사람은 다음 대상에서 빠진다 — 이제 a 만 남으니 아무도 안 맞는다
    {
        const uint16_t before = srv.HealthOf(a);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, /*onSelf=*/false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(a) == before, "남이 없는데 자기가 맞음");
        Check(srv.HealthOf(b) == 0, "죽은 사람이 또 맞음");
    }

    std::printf("  주문 피해 — 시전 알림·대상 고르기·죽을 때까지\n");
}

// ── 근접 공격 ─────────────────────────────────────────────────
//
// 클라의 `0x3f PLAYER_INPUT` 을 흉내낸다.
// 본문 = `[항목 길이] [코드1 0 0 0] …` — 코드마다 4바이트를 쓰고 뒤에 데이터가 붙는다.
// `CCAction`(6) 은 데이터가 없다.
std::vector<uint8_t> InputPacket(uint8_t code) {
    std::vector<uint8_t> p(nox::kWireHeaderSize + 1 + 1 + 4, 0);
    p[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::PLAYER_INPUT);
    p[nox::kWireHeaderSize + 1] = 4;      // 항목 길이
    p[nox::kWireHeaderSize + 2] = code;   // 코드
    return p;
}

void TestMelee() {
    nox::Server srv;
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);   // 둘 다 (1000,1000) — 근접 사거리 안이다

    // ① 한 번 치면 한 번 깎인다
    {
        std::vector<nox::OutPacket> out;
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth - kWizardPunch,
              "왼버튼 한 번에 안 깎임");
        Check(srv.melees() == 1, "근접 집계가 1 이 아님");
    }

    // ★★ ② 누르고 있으면 매 틱 오는데, **쉬는 시간이 먹혀야 한다**
    //    이게 없으면 초당 서른 대가 들어가 게임이 아니게 된다
    {
        const uint16_t before = srv.HealthOf(b);
        for (int i = 0; i < 10; ++i) {
            std::vector<nox::OutPacket> out;
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        Check(srv.HealthOf(b) == before, "쉬는 시간 안에 또 맞음");
        Check(srv.melees() == 1, "쉬는 시간 중에도 근접이 나감");
    }

    // ③ 시간이 지나면 다시 칠 수 있다
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        srv.Tick(33 * (nox::kMeleeCooldownTicks + 1), out);
        const uint16_t before = srv.HealthOf(b);
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == before - kWizardPunch, "쉬는 시간이 지났는데 안 깎임");
        Check(srv.melees() == 2, "근접 집계가 2 가 아님");
    }

    // ④ 걸음 코드는 때리지 않는다 — 코드를 뭉뚱그리면 걷기만 해도 사람이 죽는다
    {
        const uint16_t before = srv.HealthOf(b);
        std::vector<nox::OutPacket> out;
        auto pkt = InputPacket(2);   // CCMoveForward
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == before, "걸음 코드가 사람을 깎음");
    }

    // ⑤ 멀리 있으면 안 닿는다 — 주문 사거리(210)보다 근접이 짧아야 한다
    {
        nox::Server far;
        far.DebugEnterGame(a, 728);
        far.DebugEnterGame(b, 729);
        far.DebugMoveTo(b, 1000 + static_cast<int>(nox::kMeleeRange) + 20, 1000);
        std::vector<nox::OutPacket> out;
        auto pkt = InputPacket(6);
        far.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(far.HealthOf(b) == nox::kDefaultMaxHealth, "근접 사거리 밖인데 맞음");
        Check(nox::kMeleeRange < nox::kSpellRange, "근접이 주문보다 멀다");
    }

    std::printf("  근접 공격 — 한 대·쉬는 시간·걸음과 구분·사거리\n");
}

// ── 관전 풀기 ─────────────────────────────────────────────────
//
// 접속 직후 클라는 **관전(observer)** 이다 — HUD(주문 5개·체력/마나 막대)가 없고
// 자기 몸이 반투명하게만 보인다. 정품 서버는 **첫 왼클릭**에
// `6a REPORT_CLIENT_STATUS <내번호2> 00 00 00 00` 을 `cc` 껍데기에 실어 관전을 푼다.
// 2026-09-09 정품 대조로 확정 — 그 알림 앞뒤로 화면 색이 826 → 1220 으로 뛰었고,
// 안 보내던 내 서버는 978 → 978 로 한 톨도 안 변했다.
// ── 녹음본 스냅샷의 이어달기 번호 ──────────────────────
//
// ★★★ **클라는 `cc` 번호로 순서를 맞추고 지나간 번호를 조용히 버린다.**
//   녹음본은 그 녹음 때의 번호(0 · 3 · 4 …)를 담고 있고 서버도 0 부터 매기니
//   그대로 흘리면 겹친다 — 스냅샷 뒤에 보낸 `2d`·`6a` 가 통째로 사라진다.
//   (2026-09-09 실측: 나중에 들어온 쪽 화면에서만 상대가 안 보이던 까닭)
//
// ⚠ 이 시험이 진짜로 필요한 까닭 — 처음 넣은 재번호 코드는 **한 줄도 안 고쳤다.**
//   조각을 훑는 걸음이 길이표에 없는 `a4`(갱신)에서 멈춰 뒤의 `cc` 를 못 봤는데,
//   빌드도 시험도 다 통과해서 **와이어를 뜯어보고서야** 알았다.
void TestSnapshotSeqRenumber() {
    nox::Server srv;

    // 진짜 녹음본과 같은 모양 — `27`(시각) + `a4`(갱신) 뒤에 `cc` 셋, 번호는 0·3·4.
    const std::vector<uint8_t> snap = {
        0x27, 0x11, 0x22,
        0xa4, 0xff, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x0a, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x00,
        0xcc, 0x00, 0x00, 0x03, 0xe4, 0x00, 0x01,
        0xcc, 0x03, 0x00, 0x03, 0xdd, 0x00, 0x01,
        0xcc, 0x04, 0x00, 0x03, 0xde, 0x00, 0x01,
    };
    // 위 자리표 — `cc` 는 18 · 25 · 32 번 바이트에서 시작한다
    const size_t at[3] = { 18, 25, 32 };
    Check(snap[at[0]] == 0xcc && snap[at[1]] == 0xcc && snap[at[2]] == 0xcc,
          "시험이 잡은 `cc` 자리가 틀렸다");

    srv.SetFirstSnapshot(snap);
    const std::string a = "10.0.0.1:1000";
    srv.DebugEnterGame(a, 728);

    std::vector<nox::OutPacket> warm;
    srv.Tick(0, warm);              // 첫 호출은 기준점만 잡는다
    srv.DebugArmSnapshot(a);
    std::vector<nox::OutPacket> out;
    srv.Tick(33, out);

    // ⚠ 여기서 `&bodies.back()` 을 들고 있으면 안 된다 — 다음 push_back 이 옮겨 담으면
    //   그 손잡이가 죽는다. 값으로 복사해 둔다(2026-09-09 이 시험이 그 탓에 헛돌았다).
    std::vector<uint8_t> got;
    for (const auto& p : out) {
        if (p.to != a) continue;
        std::vector<uint8_t> b = Body(p);
        if (b.size() == snap.size()) got = b;
    }
    Check(!got.empty(), "스냅샷 조각이 안 나갔다");
    if (got.empty()) { std::printf("  스냅샷 번호 다시 매기기 — 못 쟀다\n"); return; }

    // 번호가 **끊김 없이 하나씩** 이어져야 한다.
    //   ⚠ 시작값을 못 박지 않는다 — 이 세션은 스냅샷 전에 판 시계(`d3`)를 이미 한 통 받았고,
    //     그 통도 같은 계수기를 쓴다. 봐야 할 것은 **이어지는가**다.
    //     녹음본 그대로면 0·3·4 라 여기서 걸린다.
    unsigned seq[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        Check(got[at[k]] == 0xcc, "그 자리가 `cc` 가 아니다");
        seq[k] = got[at[k] + 1] | (got[at[k] + 2] << 8);
    }
    const bool chained = (seq[1] == seq[0] + 1) && (seq[2] == seq[1] + 1);
    Check(chained, "녹음본 번호가 그대로다 — 이러면 뒤에 보내는 신뢰 메시지가 사라진다");
    if (!chained) {
        std::printf("  나온 번호: %u %u %u · 조각: ", seq[0], seq[1], seq[2]);
        for (size_t i = 0; i < got.size(); ++i) std::printf("%02x ", got[i]);
        std::printf("\n");
    }

    std::printf("  스냅샷 번호 다시 매기기 — 0·1·2 로 이어짐\n");
}

// ── thing.bin 직접 읽기 (독립 서버 P1) ──────────────────
//
// ★ 검증 셋 — ① 복호 왕복(내가 만든 Blowfish 가 스스로 맞는가)
//   ② 기존 표(ThingFlags, Go 로 구운 것)와 **복합 여부 전수 대조** — 걸음이 한 바이트만
//     밀려도 이름이 깨져 여기서 걸린다  ③ 실측 다섯 종의 이름·복합 못박기.
void TestThingDb() {
    const char* kThingBin = "C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin";
    FILE* probe = std::fopen(kThingBin, "rb");
    if (!probe) {
        std::printf("  thing.bin 직접 읽기 — 건너뜀 (게임 데이터 없음)\n");
        return;
    }
    std::fclose(probe);

    // ① 복호 왕복 — 앞 64바이트를 풀었다 다시 잠가 원문과 같은가
    {
        FILE* f = std::fopen(kThingBin, "rb");
        uint8_t raw[64] = {0};
        const size_t got = std::fread(raw, 1, sizeof(raw), f);
        std::fclose(f);
        Check(got == 64, "thing.bin 이 64바이트보다 작다");
        uint8_t work[64];
        std::memcpy(work, raw, 64);
        nox::Blowfish bf;
        bf.Init(nox::bf::kNoxKeys + 28 * nox::kKeyThingBin, 56);
        for (int i = 0; i < 64; i += 8) bf.DecryptBlock(work + i);
        // 복호 결과는 섹션 이름(거꾸로)으로 시작해야 한다
        bool sectOk = false;
        const char* names[] = {"ROLF", "EGDE", "LLAW", " DUA", "TNVA", "LEPS", "LIBA", "GAMI", "GNHT"};
        for (const char* nm : names)
            if (std::memcmp(work, nm, 4) == 0) sectOk = true;
        Check(sectOk, "복호 결과가 섹션 이름으로 시작하지 않는다 — 키나 알고리즘이 틀렸다");
        for (int i = 0; i < 64; i += 8) bf.EncryptBlock(work + i);
        Check(std::memcmp(work, raw, 64) == 0, "복호→암호 왕복이 원문과 다르다");

        // ★ 돌연변이 — **틀린 키**(gamedata 용 8)로 풀면 섹션 이름이 안 나와야 한다.
        //   이게 통과하면 위 sectOk 검사는 아무거나 통과시키는 헛검사다.
        nox::Blowfish bad;
        bad.Init(nox::bf::kNoxKeys + 28 * nox::kKeyGameData, 56);
        uint8_t wrong[8];
        std::memcpy(wrong, raw, 8);
        bad.DecryptBlock(wrong);
        bool wrongLooksValid = false;
        for (const char* nm : names)
            if (std::memcmp(wrong, nm, 4) == 0) wrongLooksValid = true;
        Check(!wrongLooksValid, "틀린 키로 푼 것도 섹션 이름이다 — 검사가 헛돈다");
    }

    // ② 파싱 + 전수 대조
    nox::ThingDb db;
    const bool ok = db.Load(kThingBin);
    Check(ok, "ThingDb::Load 실패");
    if (!ok) { std::printf("    까닭: %s\n", db.Error().c_str()); return; }

    Check(db.Count() + 1 >= nox::kThingCount, "종류 수가 기존 표보다 적다");
    size_t mismatch = 0;
    uint16_t firstBad = 0;
    for (uint16_t w = 1; w < nox::kThingCount && w <= db.Count(); ++w) {
        const nox::ThingInfo* ti = db.ByWire(w);
        if (!ti) { ++mismatch; if (!firstBad) firstBad = w; continue; }
        if (ti->complex != nox::ThingIsComplex(w)) { ++mismatch; if (!firstBad) firstBad = w; }
    }
    Check(mismatch == 0, "복합 여부가 기존 표와 어긋난다");
    if (mismatch)
        std::printf("    어긋남 %zu건, 첫 번호 %u (이름 '%s')\n", mismatch, firstBad,
                    db.ByWire(firstBad) ? db.ByWire(firstBad)->name.c_str() : "?");

    // ③ 실측 다섯 — 이름과 복합을 못박는다
    struct { uint16_t wire; const char* name; bool complex; } fixed[] = {
        { 713,  "NewPlayer",  true  },
        { 1328, "Imp",        true  },
        { 1357, "Bat",        true  },
        { 1348, "Bomber",     true  },
        { 176,  "RedFlag",    false },
    };
    for (const auto& fx : fixed) {
        const nox::ThingInfo* ti = db.ByWire(fx.wire);
        Check(ti != nullptr, "실측 번호가 표 밖이다");
        if (!ti) continue;
        Check(ti->name == fx.name, "실측 번호의 이름이 다르다");
        Check(ti->complex == fx.complex, "실측 번호의 복합 여부가 다르다");
        if (ti->name != fx.name)
            std::printf("    %u → '%s' (기대 '%s')\n", fx.wire, ti->name.c_str(), fx.name);
    }
    // 사람 반지름 — EXTENT CIRCLE 이 읽혔는가 (값 자체는 못박지 않는다)
    if (const nox::ThingInfo* pl = db.ByWire(713))
        Check(pl->radius > 0.0f, "NewPlayer 의 충돌 반지름이 0 이다 — EXTENT 가 안 읽혔다");

    std::printf("  thing.bin 직접 읽기 — %zu종, 기존 표와 전수 일치\n", db.Count());
}

// ── .map 섹션 워커 (독립 서버 P2-a) ──────────────────
//
// 시험이 보는 것 — ① 복호(키 19)와 머리가 맞는가(magic) ② 섹션 이름·크기가
//   끝까지 읽혀 오버런 없이 나오는가 ③ 서버가 쓸 세 섹션이 있는가.
void TestMapSections() {
    const char* kMap = "C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\estate\\Estate.map";
    FILE* probe = std::fopen(kMap, "rb");
    if (!probe) { std::printf("  .map 섹션 워커 — 건너뜀 (맵 없음)\n"); return; }
    std::fclose(probe);

    nox::MapFile mf;
    const bool ok = mf.Load(kMap);
    Check(ok, ".map 읽기 실패");
    if (!ok) { std::printf("    까닭: %s\n", mf.Error().c_str()); return; }

    // 서버가 쓸 섹션 셋이 있어야 한다
    Check(mf.Find("ObjectTOC") != nullptr, "ObjectTOC 섹션이 없다");
    Check(mf.Find("ObjectData") != nullptr, "ObjectData 섹션이 없다");
    Check(mf.Find("WallMap") != nullptr, "WallMap 섹션이 없다");
    Check(mf.Find("MapInfo") != nullptr, "MapInfo 섹션이 없다");

    std::printf("  .map 섹션 워커 — %zu섹션: ", mf.Sections().size());
    for (const auto& s : mf.Sections()) std::printf("%s(%zu) ", s.name.c_str(), s.size);
    std::printf("\n");

    // ── P2-b: 오브젝트 목록 ──
    //
    // ★ 검증은 **녹음본과의 대조**다 — 서버 스폰 자리(3864,2534)는 estate 녹음에서
    //   읽어 낸 것이니, 맵에도 그 근처가 걸을 수 있는 땅이라야 하고,
    //   좌표는 전부 맵 범위(0~6000) 안이라야 한다.
    const std::vector<nox::MapObject> objs = mf.ReadObjects();
    Check(!objs.empty(), "오브젝트가 하나도 안 읽혔다");
    if (!objs.empty()) {
        size_t badPos = 0;
        for (const auto& o : objs)
            if (o.x < 0 || o.x > 6000 || o.y < 0 || o.y > 6000) ++badPos;
        Check(badPos == 0, "맵 범위(0~6000) 밖 좌표가 있다 — 걸음이 밀렸다");

        // 녹음본 첫 스냅샷에 실려 오던 오브젝트 종류가 맵에도 있어야 한다.
        // (09-09 스냅샷 실측 — 종류 649 셋 · 1152 둘 · 1154 둘 등. 649 는 이름 짐작이
        //  아니라 **좌표 대조**로 본다: 스냅샷 머리 자리(3864,2534) 반경 600 에
        //  오브젝트가 여럿 있어야 한다.)
        size_t nearSpawn = 0;
        for (const auto& o : objs) {
            const double dx = o.x - 3864, dy = o.y - 2534;
            if (dx * dx + dy * dy < 600.0 * 600.0) ++nearSpawn;
        }
        Check(nearSpawn > 0, "스폰(3864,2534) 근처에 오브젝트가 하나도 없다 — 좌표가 밀렸다");
        if (badPos || nearSpawn == 0) mf.DumpFirstObjects(3);
        std::printf("  .map 오브젝트 — %zu개, 스폰 반경 600 안 %zu개, 첫 셋: ", objs.size(), nearSpawn);
        for (size_t i = 0; i < objs.size() && i < 3; ++i)
            std::printf("%s(%.0f,%.0f) ", objs[i].type.c_str(), objs[i].x, objs[i].y);
        std::printf("\n");
    } else if (!mf.Error().empty()) {
        std::printf("    까닭: %s\n", mf.Error().c_str());
    }

    // ── P2-c: 벽 격자 ──
    //
    // 포맷 자체가 검증이다 — 종결 0xff 에 **정확히** 닿아야 하고 뒤에 남으면 실패.
    // (7바이트 걸음이 한 칸이라도 밀리면 종결을 못 만나거나 trailing 이 남는다)
    const std::vector<nox::MapFile::WallCell> walls = mf.ReadWalls();
    Check(!walls.empty(), "벽이 하나도 안 읽혔다");
    if (walls.empty() && !mf.Error().empty()) std::printf("    까닭: %s\n", mf.Error().c_str());
    if (!walls.empty()) {
        // 크기 검산 — (3211 - 18 - 1) / 7 = 456 이 딱 떨어져야 한다
        Check((mf.Find("WallMap")->size - 18 - 1) % 7 == 0, "WallMap 크기가 7 로 안 나뉜다");
        std::printf("  .map 벽 — %zu칸 (격자 x %u~%u · y %u~%u)\n", walls.size(),
                    walls.front().x, walls.back().x, walls.front().y, walls.back().y);
    }
}

// ── 벽 충돌 (독립 서버 P3-a) ──────────────────────────────
//
// 시험 셋 — ① 벽이 없으면 걸음이 그대로 간다 ② 벽 앞에서 막힌다
//   ③ 대각으로 스치면 갈 수 있는 축으로 **미끄러진다**.
// ── 접속 묶음 조립 (독립 서버 P5-a) ──────────────────
//
// 녹음본과 같은 값(번호 713 · 프레임 2484 · 정보 97B)으로 조립하면
// **216바이트가 전부 같아야** 한다. 한 자리라도 다르면 조립이 틀린 것.
// USE_MAP 은 맵 CRC 까지 교차 검증 — .map 머리의 CRC 가 와이어의 값과 같아야 한다.
// ── 녹음 스냅샷 ↔ 맵 파서 교차 대조 (P2-d 정밀) ──────────
//
// 같은 estate 맵이니 — 스냅샷의 오브젝트 좌표가 맵 파일의 오브젝트와
// 맞아떨어져야 한다. 파서는 왕복 검증된 DecodeUpdate 를 쓴다(눈대중 파싱 금지).
// 녹음 첫 스냅샷 543B — TestSnapshotVsMap 과 TestMapNetCodeRule 이 같이 쓴다.
static const uint8_t kSnap543[543] = {
        0x27, 0x21, 0x4b, 0xa4, 0xff, 0x00, 0x00, 0x00, 0x00, 0xde, 0x0a, 0xb0, 0x0d, 0x00, 0x00, 0xff,

        0xc9, 0x02, 0xc9, 0x02, 0x00, 0x01, 0xc0, 0x00, 0x0f, 0x00, 0xff, 0x28, 0x02, 0x8a, 0x04, 0xc3,

        0x0b, 0x94, 0x0d, 0xff, 0x27, 0x02, 0x94, 0x04, 0x19, 0x1e, 0x00, 0xff, 0xb0, 0x00, 0x7e, 0x02,

        0xec, 0x0c, 0xdb, 0x0d, 0x00, 0xff, 0xc7, 0x01, 0x89, 0x02, 0x06, 0x0c, 0xbb, 0x0e, 0xff, 0xb4,

        0x00, 0x9a, 0x04, 0xe5, 0x02, 0xff, 0xc9, 0x01, 0x89, 0x02, 0x46, 0x0d, 0xff, 0xc8, 0x01, 0x89,

        0x02, 0xd7, 0x0c, 0xff, 0xd9, 0x01, 0x89, 0x02, 0x30, 0x08, 0x00, 0xff, 0xf8, 0x01, 0x92, 0x04,

        0x0f, 0x0a, 0xaf, 0x0c, 0xff, 0x2b, 0x02, 0x80, 0x04, 0xd9, 0xf9, 0xff, 0x8b, 0x02, 0xd9, 0x00,

        0xae, 0x75, 0x00, 0xff, 0x87, 0x02, 0x7e, 0x02, 0x0d, 0x0a, 0x8a, 0x0e, 0x00, 0xff, 0x15, 0x02,

        0x82, 0x04, 0x47, 0x0d, 0x1c, 0x0c, 0xff, 0x17, 0x02, 0x75, 0x04, 0x03, 0xe9, 0x00, 0x00, 0x00,

        0xab, 0x20, 0x4b, 0x00, 0x00, 0xb2, 0x3d, 0x80, 0x18, 0xa7, 0x00, 0x45, 0xc8, 0xa7, 0x00, 0x2c,

        0xc9, 0xaa, 0xcc, 0x00, 0x00, 0x03, 0xe4, 0x00, 0x01, 0xcc, 0x00, 0x00, 0x03, 0xe4, 0x00, 0x01,

        0xcc, 0x00, 0x00, 0x02, 0xec, 0x01, 0xcc, 0x00, 0x00, 0x07, 0x6a, 0xc9, 0x02, 0x00, 0x00, 0x00,

        0x00, 0xcc, 0x00, 0x00, 0x0b, 0x4e, 0xc9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

        0xcc, 0x01, 0x00, 0x05, 0xea, 0x17, 0x4b, 0x00, 0x00, 0xcc, 0x02, 0x00, 0x3c, 0xb1, 0x01, 0x65,

        0x73, 0x74, 0x61, 0x74, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x20, 0x47, 0x61, 0x6d,

        0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

        0xff, 0xff, 0xff, 0x07, 0x21, 0x0f, 0x00, 0x00, 0x00, 0xe4, 0x01, 0x01, 0xcc, 0x00, 0x00, 0x03,

        0xe4, 0x01, 0x01, 0xcc, 0x03, 0x00, 0x07, 0xdd, 0xc9, 0x02, 0x4b, 0x00, 0x4b, 0x00, 0xde, 0xc9,

        0x02, 0x96, 0x00, 0x96, 0x00, 0xcc, 0x04, 0x00, 0x09, 0xe9, 0xc9, 0x02, 0x1f, 0x4b, 0x00, 0x00,

        0xff, 0x01, 0xcc, 0x05, 0x00, 0x09, 0x4c, 0xca, 0x02, 0x6c, 0x04, 0xff, 0xba, 0xc6, 0xff, 0xcc,

        0x06, 0x00, 0x03, 0x60, 0xca, 0x02, 0xcc, 0x07, 0x00, 0x09, 0x4c, 0xcb, 0x02, 0x6d, 0x04, 0xff,

        0xbf, 0xff, 0xff, 0xcc, 0x08, 0x00, 0x03, 0x60, 0xcb, 0x02, 0xcc, 0x09, 0x00, 0x09, 0x4c, 0xcc,

        0x02, 0x6e, 0x04, 0xb9, 0xca, 0xff, 0xff, 0xcc, 0x0a, 0x00, 0x03, 0x60, 0xcc, 0x02, 0xcc, 0x0b,

        0x00, 0x09, 0x4c, 0xcd, 0x02, 0x79, 0x04, 0xff, 0xff, 0xff, 0xff, 0xcc, 0x0c, 0x00, 0x07, 0x44,

        0xcd, 0x02, 0x45, 0x01, 0x45, 0x01, 0xcc, 0x0d, 0x00, 0x03, 0x60, 0xcd, 0x02, 0xcc, 0x0e, 0x00,

        0x07, 0x6a, 0xc9, 0x02, 0x00, 0x00, 0x00, 0x00, 0xef, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80,

        0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0xcc, 0x0f, 0x00, 0x04, 0xb3, 0x53, 0x89,

        0x32, 0xcc, 0x10, 0x00, 0x04, 0xb3, 0x54, 0x89, 0x32, 0xcc, 0x11, 0x00, 0x04, 0xb3, 0x51, 0x89,

        0x32, 0xcc, 0x12, 0x00, 0x04, 0xb3, 0x52, 0x89, 0x32, 0x67, 0x8b, 0x02, 0xff, 0xff, 0x9c, 0xff,

        0xcc, 0x13, 0x00, 0x04, 0xb3, 0x4b, 0x80, 0x32, 0xcc, 0x14, 0x00, 0x07, 0x5a, 0xc9, 0x02, 0x00,

        0x00, 0x80, 0x00, 0x49, 0x20, 0x85, 0x6b, 0x3e, 0xcc, 0x15, 0x00, 0x07, 0xdd, 0xc9, 0x02, 0x4b,

        0x00, 0x4b, 0x00, 0xde, 0xc9, 0x02, 0x96, 0x00, 0x96, 0x00, 0x48, 0xc9, 0x02, 0x4b, 0x00, 0x96,

        0x00, 0x72, 0x06, 0x5c, 0x44, 0x23, 0x00, 0x0a, 0xcc, 0x16, 0x00, 0x03, 0x43, 0x4b, 0x00,
};

void TestSnapshotVsMap() {
    const uint8_t* kSnap = kSnap543;
    // 27 xx xx 뒤가 a4 상태 갱신이다
    Check(kSnap[0] == 0x27 && kSnap[3] == 0xa4, "스냅샷 앞머리가 다르다");
    const nox::UpdateMsg um = nox::DecodeUpdate(kSnap + 4, sizeof(kSnap543) - 4, nullptr);
    Check(um.ok, "스냅샷 갱신을 못 뜿었다");
    if (!um.ok) return;

    nox::MapFile mf;
    if (!mf.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\estate\\Estate.map")) {
        std::printf("  스냅샷↔맵 대조 — 건너뜀(맵 없음)\n");
        return;
    }
    const std::vector<nox::MapObject> objs = mf.ReadObjects();

    // 전체번호(ff)로 온 항목만 본다 — 별칭 항목은 종류를 모른다.
    size_t checked = 0, matched = 0;
    for (const auto& e : um.objects) {
        if (e.alias != 0xff) continue;
        if (e.type == nox::kPlayerType) continue;   // 사람은 맵 파일에 없다
        ++checked;
        for (const auto& o : objs) {
            const double dx = o.x - e.x, dy = o.y - e.y;
            if (dx * dx + dy * dy <= 4.0) { ++matched; break; }   // 2 유닛 안
        }
    }
    Check(checked > 0, "전체번호 항목이 없다 — 뜯기가 이상하다");
    // ★ 기대값 정정(2026-09-10 진단) — 「전수 일치」가 아니다. 스냅샷에는
    //   **맵 파일의 정적 오브젝트**(BluePotion·FanChakram — 거리 0.0 일치)와
    //   **게임이 시작 때 만든 동적 오브젝트**(649 불꽃 무리 등 — 맵에 없음)가 섞인다.
    //   좌표계 검증으로 맞는 물음은 「정적인 것이 정확히 겹치는가」다.
    if (matched < 2) {
        // 진단 — 항목별 최근접 맵 오브젝트와 거리를 찍는다.
        //   전부 같은 벡터 차면 오프셋, 제각각이면 동적 생성물이다.
        for (const auto& e : um.objects) {
            if (e.alias != 0xff || e.type == nox::kPlayerType) continue;
            double bd = 1e18; const nox::MapObject* bo = nullptr;
            for (const auto& o : objs) {
                const double dx = o.x - e.x, dy = o.y - e.y;
                const double d2 = dx * dx + dy * dy;
                if (d2 < bd) { bd = d2; bo = &o; }
            }
            if (bo)
                std::printf("    type=%u (%d,%d) → 최근접 %s (%.0f,%.0f) 거리 %.1f\n",
                            e.type, e.x, e.y, bo->type.c_str(), bo->x, bo->y, std::sqrt(bd));
        }
    }
    Check(matched >= 2, "맵 정적 오브젝트와 정확히 겹치는 항목이 둘도 안 된다 — 좌표계가 어긋났다");
    std::printf("  스냅샷↔맵 대조 — 전체번호 %zu개 중 %zu개가 맵 오브젝트와 2유닛 안 일치\n",
                checked, matched);
}

// ── 기획데이터 정본 — 옮겨 적은 값이 맞나 (P6) ──────────
//
// 게임 파일을 런타임에 안 읽고 `GameBalance.h` 에 옮겨 적었다. 그러면 **옮기다 틀릴 수
// 있다.** 여기서 그 값들을 숫자로 못박는다 — 표를 잘못 만지면 여기서 걸린다.
// 근거는 전부 `Docs/GAMEDATA_AUDIT.md`.
void TestGameBalance() {
    using nox::CharClass;
    // ⭕ gamedata.bin — 아레나·레벨10 직업 스탯
    Check(nox::HealthOnline(CharClass::Warrior)  == 150, "워리어 체력이 150 이 아님");
    Check(nox::HealthOnline(CharClass::Wizard)   ==  75, "위저드 체력이 75 가 아님");
    Check(nox::HealthOnline(CharClass::Conjurer) == 100, "컨저러 체력이 100 이 아님");
    Check(nox::ManaOnline(CharClass::Warrior)    ==   0, "워리어 마나가 0 이 아님");
    Check(nox::ManaOnline(CharClass::Wizard)     == 150, "위저드 마나가 150 이 아님");
    Check(nox::ManaOnline(CharClass::Conjurer)   == 125, "컨저러 마나가 125 가 아님");
    Check(nox::StrengthOnline(CharClass::Warrior) == 125, "워리어 힘이 125 가 아님");

    // ◑ 캠페인 보간은 **레벨 1 이 기본값, 10 이 최대값** — 0 이 아니다
    Check(nox::HealthAtLevel(CharClass::Warrior, 1)  ==  20, "레벨1 체력이 기본값(20)이 아님");
    Check(nox::HealthAtLevel(CharClass::Warrior, 10) == 150, "레벨10 체력이 최대값이 아님");
    Check(nox::ManaAtLevel(CharClass::Warrior, 1)    ==   0, "워리어는 어느 레벨에서도 마나 0");

    // ⭕ modifier.bin — 무기 표. 순서가 밀리면 엉뚱한 무기를 든다
    Check(std::string(nox::kWeapons[2].name) == "Longsword", "무기표 2번이 롱소드가 아님");
    Check(nox::kWeapons[2].damageMin == 9 && nox::kWeapons[2].reqStrength == 30,
          "롱소드 최소값·요구힘이 다름");
    // 워리어(힘 125)가 롱소드로 = (125−30)×0.30 + 9 = 37.5 → 38
    Check(nox::MeleeDamageFor(CharClass::Warrior, 125, false) == 38,
          "워리어 롱소드 피해가 38 이 아님");
    // 위저드(힘 35)는 맨손 = 35×0.04 + 5 = 6.4 → 6
    Check(nox::MeleeDamageFor(CharClass::Wizard, 35, false) == 6,
          "위저드 맨손 피해가 6 이 아님");
    Check(nox::MeleeRangeFor(CharClass::Warrior) == 45.0, "롱소드 사거리가 45 가 아님");

    // ⭕ thing.bin — 파이어볼 COLLIDE 인자 ÷2
    Check(nox::FireballDirectDamage(1) ==  64, "레벨1 파이어볼이 64 가 아님");
    Check(nox::FireballDirectDamage(5) == 127, "레벨5 파이어볼이 127 이 아님");
    Check(nox::kFireballPerFrame == 12.0, "파이어볼 속도가 12.0 이 아님");
    Check(nox::kPlayerWalkPerFrame == 3.0, "걷기가 3.0 이 아님");

    // ⭕ monster.bin — 소환수
    const nox::MonsterStats* imp = nox::FindMonster(1328);
    Check(imp && imp->health == 25, "임프 체력이 25 가 아님");
    const nox::MonsterStats* bat = nox::FindMonster(1357);
    Check(bat && bat->health == 35 && bat->meleeDamage == 4, "박쥐 값이 다름");

    // ◑ 방어구 — 곱해서 깎고, 전기는 안 탄다
    Check(nox::ApplyArmor(100, 0.23, nox::DamageKind::Physical) == 77, "방어 0.23 에 77 이 아님");
    Check(nox::ApplyArmor(100, 0.23, nox::DamageKind::Ignores) == 100, "주문이 방어를 탐");
    Check(nox::ApplyArmor(100, 5.0,  nox::DamageKind::Physical) == 0,  "방어합이 1.0 에서 안 잘림");

    // ◑ 기본 장비 — 워리어는 셔츠를 안 받는다
    const nox::Loadout w = nox::LoadoutFor(CharClass::Warrior);
    Check(w.weapon == nox::kItemLongsword, "워리어가 롱소드를 안 듦");
    for (int i = 0; i < w.count; ++i)
        Check(w.items[i] != nox::kItemStreetShirt, "워리어가 셔츠를 받음");
    const nox::Loadout z = nox::LoadoutFor(CharClass::Wizard);
    Check(z.count == 4 && z.weapon == 0, "위저드 장비가 넷·무기없음이 아님");
    Check(nox::LoadoutFor(CharClass::Conjurer).count == 3, "컨저러 장비가 셋이 아님");

    std::printf("  기획데이터 — 직업 스탯·무기·주문·몬스터·방어구·장비 전수 대조 통과\n");
}

// ── 스냅샷이 직업마다 다른가 (P6) ──────────
//
// 정본화 전에는 **누구에게나 위저드 한 벌**이 나갔다. 직업을 바꾸면 체력·장비가
// 따라 바뀌어야 하고, **위저드 것은 예전 그대로**여야 한다(TestSnapshotTail 이 그 짝).
void TestSnapshotPerClass() {
    using nox::CharClass;
    const auto wiz = nox::BuildSnapshotTail(713, CharClass::Wizard);
    const auto war = nox::BuildSnapshotTail(713, CharClass::Warrior);
    const auto con = nox::BuildSnapshotTail(713, CharClass::Conjurer);
    Check(wiz != war, "워리어 스냅샷이 위저드와 같다 — 직업이 안 갈림");
    Check(con != wiz && con != war, "컨저러 스냅샷이 남과 같다");

    // 체력 자리(`0xdd <번호2> <지금2> <최대2>`)가 직업 값인가
    auto hasTotalHealth = [](const std::vector<uint8_t>& m, uint16_t want) {
        for (size_t i = 0; i + 6 < m.size(); ++i)
            if (m[i] == 0xdd && m[i + 1] == 0xc9 && m[i + 2] == 0x02 &&
                (m[i + 3] | (m[i + 4] << 8)) == want) return true;
        return false;
    };
    Check(hasTotalHealth(wiz,  75), "위저드 스냅샷에 체력 75 가 없다");
    Check(hasTotalHealth(war, 150), "워리어 스냅샷에 체력 150 이 없다");
    Check(hasTotalHealth(con, 100), "컨저러 스냅샷에 체력 100 이 없다");

    // 장비 자리(`0x4c <번호2> <종류2> …`)에 로브가 위저드에게만 있나
    auto hasItem = [](const std::vector<uint8_t>& m, uint16_t type) {
        for (size_t i = 0; i + 4 < m.size(); ++i)
            if (m[i] == 0x4c && (m[i + 3] | (m[i + 4] << 8)) == type) return true;
        return false;
    };
    Check(hasItem(wiz, nox::kItemWizardRobe),  "위저드에게 로브가 없다");
    Check(!hasItem(war, nox::kItemWizardRobe), "워리어가 마법사 로브를 입었다");
    Check(!hasItem(con, nox::kItemWizardRobe), "컨저러가 마법사 로브를 입었다");
    Check(hasItem(war, nox::kItemLongsword),   "워리어가 롱소드를 안 들었다");
    Check(!hasItem(war, nox::kItemStreetShirt), "워리어가 셔츠를 입었다");
    std::printf("  스냅샷 — 직업마다 체력·장비가 갈린다 (위저드 %zuB · 워리어 %zuB · 컨저러 %zuB)\n",
                wiz.size(), war.size(), con.size());
}

// ── 정품 오브젝트 번호 ↔ 맵 색인 — 연번 규칙 규명 (P4-d ①) ──────────
//
// 정품 스냅샷의 아이템 번호(176~651)는 맵 로드 때 매기는 연번으로 보인다
// (사과 455·456·457 이 연속). 클라도 같은 순회로 같은 번호를 만들어 두므로,
// 서버가 **그 번호로 불러야** 실체가 합쳐진다 — 20000번대 새 번호는 이중
// 실체를 만들어 클라를 죽였다(2026-09-10 ao1~ao5). 여기서 좌표로 짝을 지어
// nc 와 맵 색인의 관계를 찍는다 — 규칙이 서면 P4-d ③ 의 번호 재구성 근거다.
void TestMapNetCodeRule() {
    const nox::UpdateMsg um =
        nox::DecodeUpdate(kSnap543 + 4, sizeof(kSnap543) - 4, nullptr);
    Check(um.ok, "번호 규칙 — 스냅샷을 못 뜯었다");
    if (!um.ok) return;
    nox::MapFile mf;
    if (!mf.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\estate\\Estate.map")) {
        std::printf("  번호 규칙 — 건너뜀(맵 없음)\n");
        return;
    }
    const auto objs = mf.ReadObjects();
    size_t matched = 0;
    std::printf("  번호 규칙 — 정품 nc ↔ 맵 색인 (좌표 2유닛 짝):\n");
    for (const auto& e : um.objects) {
        if (e.alias != 0xff || e.type == nox::kPlayerType) continue;
        int best = -1; double bd = 1e18;
        for (size_t i = 0; i < objs.size(); ++i) {
            const double dx = objs[i].x - e.x, dy = objs[i].y - e.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 < bd) { bd = d2; best = static_cast<int>(i); }
        }
        if (best >= 0 && bd <= 4.0) {
            ++matched;
            std::printf("    nc=%3u ↔ 색인 %3d (차 %+d) %s\n", e.netCode, best,
                        static_cast<int>(e.netCode) - best, objs[best].type.c_str());
        }
    }
    // 스냅샷은 정적(맵)과 동적(서버 생성)이 섞이므로 전수 일치는 기대하지 않는다 —
    // 좌표가 정확히 겹치는 것(=맵 아이템)만이 규칙의 표본이다.
    Check(matched >= 2, "번호 규칙 — 맵과 겹치는 표본이 둘도 안 된다");
}

// ── 아이템 판별(isPickup) 경계 확정 (P4-d ②) ──────────
//
// 정품 갱신에 실리는 실측 집합(물약·무기·방어구·사과·화살통 …)은 전부 isPickup 이고,
// 장식(나무·불꽃·꽃·화덕)은 전부 아니어야 한다. CLASS 원문도 같이 찍는다 —
// 경계가 틀리면 여기서 원문을 보고 ParseClasses 의 후보를 고친다.
void TestPickupClass() {
    nox::ThingDb tdb;
    if (!tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
        std::printf("  아이템 판별 — 건너뜀(thing.bin 없음)\n");
        return;
    }
    // 정품 갱신·스냅샷 실측 집합 (2026-09-10 오픈녹스 ref + 녹음 543B)
    static const char* kMust[] = {
        "RedPotion", "BluePotion", "RedApple", "Cider", "WoodenShield", "BattleAxe",
        "GreatSword", "Breastplate", "LeatherHelm", "LeatherArmoredBoots",
        "FanChakram", "Quiver", "CrossBow", "Bow", "Sword", "LeatherArmor",
    };
    // ao1~ao5 붕괴 갱신의 장식 (실었다가 클라가 죽은 것들)
    static const char* kMustNot[] = {
        "TreePine14", "FlameImmobile", "SmallFlameImmobile", "MediumFlameImmobile",
        "FlowersYellowSparse", "OgreFirePitUnlit",
    };
    size_t bad = 0;
    for (const char* nm : kMust) {
        const uint16_t w = tdb.WireByName(nm);
        const nox::ThingInfo* t = w ? tdb.ByWire(w) : nullptr;
        if (!t) { std::printf("    [?] %s — 종류표에 없음\n", nm); ++bad; continue; }
        if (!t->isPickup) {
            std::printf("    [✗] %-20s isPickup=0  CLASS=%s\n", nm, t->classRaw.c_str());
            ++bad;
        }
    }
    for (const char* nm : kMustNot) {
        const uint16_t w = tdb.WireByName(nm);
        const nox::ThingInfo* t = w ? tdb.ByWire(w) : nullptr;
        if (!t) { std::printf("    [?] %s — 종류표에 없음\n", nm); ++bad; continue; }
        if (t->isPickup) {
            std::printf("    [✗] %-20s isPickup=1  CLASS=%s\n", nm, t->classRaw.c_str());
            ++bad;
        }
    }
    if (bad == 0) std::printf("  아이템 판별 — 실측 집합 %zu+%zu 전부 경계 안\n",
                              std::size(kMust), std::size(kMustNot));
    Check(bad == 0, "아이템 판별 경계가 실측 집합과 어긋난다 — 위 CLASS 원문으로 후보 수정");
}

// ── 시야 예산 — 상한 없이는 a4 가 통째로 비어 나간다 (P4-b 방어) ──────────
//
// 2026-09-10 pa1 실측 — 조립 스냅샷이 387B(`27 | a4 | cc 꼬리`)로 나갔다.
// estate 스폰 주변은 반경 700 안 정적이 40개를 넘어, 전부 절대 소개(항목 ~11B)로
// 실으면 EncodeUpdate 예산 479B 를 넘겨 **0 을 돌려준다** — 사람 몸까지 안 실려
// 화면이 검었다. 여기서 그 환경을 재현하고, kAoiMaxPerUpdate 상한이 막는지 본다.
void TestAoiBudget() {
    nox::MapFile mf;
    if (!mf.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\estate\\Estate.map")) {
        std::printf("  시야 예산 — 건너뜀(맵 없음)\n");
        return;
    }
    nox::ThingDb tdb;
    if (!tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
        std::printf("  시야 예산 — 건너뜀(thing.bin 없음)\n");
        return;
    }
    // main.cpp 와 같은 절차로 정적 목록 재구성
    struct P { uint16_t nc, tp; double x, y; };
    std::vector<P> st;
    uint16_t nextCode = 20000;
    for (const auto& o : mf.ReadObjects()) {
        const uint16_t wire = tdb.WireByName(o.type);
        if (wire == 0) continue;
        st.push_back({ nextCode++, wire, o.x, o.y });
    }
    Check(st.size() > 40, "estate 정적이 40개도 안 된다 — 파서가 이상하다");

    // 실측 스폰 두 자리(sa2 · pa1)에서 반경 안 수를 센다
    const double spawns[2][2] = { { 2782, 3504 }, { 3864, 2534 } };
    size_t worst = 0;
    for (const auto& sp : spawns) {
        size_t n = 0;
        for (const auto& w : st) {
            const double dx = w.x - sp[0], dy = w.y - sp[1];
            if (dx * dx + dy * dy <= nox::kAoiRadius * nox::kAoiRadius) ++n;
        }
        if (n > worst) worst = n;
    }
    std::printf("  시야 예산 — 스폰 주변 반경 %d 안 정적 최대 %zu개 (상한 %zu)\n",
                static_cast<int>(nox::kAoiRadius), worst, nox::kAoiMaxPerUpdate);
    Check(worst > nox::kAoiMaxPerUpdate,
          "반경 안 정적이 상한보다 적다 — 이 시험의 전제(넘침 환경)가 깨졌다");

    // 갱신 조립 — 사람 하나 + 정적 N 개를 절대 소개로. 상한 없이(전부) vs 상한.
    // ★ 자리는 **가장 몰린 스폰**으로 — 널널한 자리로 조립하면 넘침이 재현 안 된다.
    auto build = [&](const double* sp, size_t cap) {
        nox::UpdateMsg m;
        m.headAlias = 0xff; m.headNetCode = 0; m.headType = 0;
        m.x = static_cast<int>(sp[0]); m.y = static_cast<int>(sp[1]);
        m.flags = 0; m.headAnim = 0;
        nox::ObjEntry me;
        me.alias = 0xff; me.netCode = 713; me.type = nox::kPlayerType;
        me.absolute = true; me.x = m.x; me.y = m.y;
        me.extra = 2; me.extraBytes[0] = 0x40; me.extraBytes[1] = 0;
        m.objects.push_back(me);
        size_t n = 0;
        for (const auto& w : st) {
            const double dx = w.x - sp[0], dy = w.y - sp[1];
            if (dx * dx + dy * dy > nox::kAoiRadius * nox::kAoiRadius) continue;
            if (n++ >= cap) break;
            nox::ObjEntry e;
            e.alias = 0xff; e.netCode = w.nc; e.type = w.tp;
            e.absolute = true; e.x = static_cast<int>(w.x); e.y = static_cast<int>(w.y);
            e.extra = 1; e.extraBytes[0] = 0x08;
            m.objects.push_back(e);
        }
        uint8_t buf[480];
        buf[0] = 0xa4;
        return nox::EncodeUpdate(m, buf + 1, sizeof(buf) - 1);
    };
    const double* dense = spawns[0];
    {
        size_t best = 0;
        for (const auto& sp : spawns) {
            size_t n = 0;
            for (const auto& w : st) {
                const double dx = w.x - sp[0], dy = w.y - sp[1];
                if (dx * dx + dy * dy <= nox::kAoiRadius * nox::kAoiRadius) ++n;
            }
            if (n > best) { best = n; dense = sp; }
        }
    }
    const size_t unAll = build(dense, st.size());              // 상한 없음 — 결함 재현
    const size_t unCap = build(dense, nox::kAoiMaxPerUpdate);  // 상한 — 방어
    std::printf("  시야 예산 — 전부 실으면 %zuB · 상한이면 %zuB\n", unAll, unCap);
    Check(unAll == 0, "전부 실었는데 넘치지 않았다 — 예산 계산이 달라졌다(상한 상향 검토)");
    Check(unCap > 0, "상한을 걸어도 a4 가 비어 나온다 — 상한이 예산보다 크다");
}

void TestJoinBundle() {
    // 녹음본 접속 묶음 216B — SNAP_M_warrior_ctf 복호본에서 실측(2026-09-10)
    static const uint8_t kRec[216] = {
        0xa9, 0x0c, 0x01, 0x00, 0x00, 0x00, 0x28, 0xb4, 0x09, 0x00, 0x00, 0x2c, 0xc9, 0x02, 0x00, 0x00,
        0x00, 0x00, 0xaf, 0xb4, 0x09, 0x00, 0x00, 0x9a, 0x03, 0x01, 0x00, 0x00, 0x21, 0x03, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x20, 0x0f, 0x00, 0xb0, 0x55, 0x73, 0x65, 0x72, 0x20, 0x47, 0x61, 0x6d, 0x65,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x2d, 0xc9, 0x02, 0x4a, 0x00, 0x61, 0x00, 0x63, 0x00,
        0x6b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x73, 0x4d,
        0x22, 0xda, 0x9a, 0x6e, 0xda, 0x9a, 0x6e, 0xda, 0x9a, 0x6e, 0xda, 0x9a, 0x6e, 0x0c, 0x07, 0x13,
        0x17, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::vector<uint8_t> built =
        nox::BuildJoinBundle(713, 2484, kRec + 90);
    Check(built.size() == 216, "조립 크기가 216 이 아니다");
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < 216 && i < built.size(); ++i)
        if (built[i] != kRec[i]) { ++diff; if (!first) first = i; }
    Check(diff == 0, "조립이 녹음본과 다르다");
    if (diff) std::printf("    다른 자리 %zu개, 첫 자리 %zu (%02x != %02x)\n",
                          diff, first, built[first], kRec[first]);

    // USE_MAP — 이름·CRC·프레임으로 조립. CRC 는 실측 0x27de9016.
    static const uint8_t kRecMap[41] = {
        0x2b, 0x65, 0x73, 0x74, 0x61, 0x74, 0x65, 0x2e, 0x6d, 0x61, 0x70, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x90, 0xde,
        0x27, 0xb4, 0x09, 0x00, 0x00 };
    const std::vector<uint8_t> um = nox::BuildUseMap("estate.map", 0x27de9016u, 2484);
    Check(um.size() == 41 && std::memcmp(um.data(), kRecMap, 41) == 0,
          "USE_MAP 조립이 녹음본과 다르다");

    // ★ 교차 검증 — .map 머리의 CRC 가 와이어 값과 같은가
    nox::MapFile mf;
    if (mf.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\estate\\Estate.map"))
        Check(mf.Crc() == 0x27de9016u, "맵 파일 CRC 가 와이어의 CRC 와 다르다");

    std::printf("  접속 묶음 조립 — 216B 전수 일치 · USE_MAP 일치 · CRC 교차 확인\n");
}

// ── 스냅샷 꼬리부 조립 (P5-c-1) ──────────────────
void TestSnapshotTail() {
    static const uint8_t kTail[381] = {
        0xcc, 0x00, 0x00, 0x03, 0xe4, 0x00, 0x01, 0xcc, 0x00, 0x00, 0x03, 0xe4, 0x00, 0x01, 0xcc, 0x00,

        0x00, 0x02, 0xec, 0x01, 0xcc, 0x00, 0x00, 0x07, 0x6a, 0xc9, 0x02, 0x00, 0x00, 0x00, 0x00, 0xcc,

        0x00, 0x00, 0x0b, 0x4e, 0xc9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcc, 0x01,

        0x00, 0x05, 0xea, 0x17, 0x4b, 0x00, 0x00, 0xcc, 0x02, 0x00, 0x3c, 0xb1, 0x01, 0x65, 0x73, 0x74,

        0x61, 0x74, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x20, 0x47, 0x61, 0x6d, 0x65, 0x00,

        0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

        0xff, 0x07, 0x21, 0x0f, 0x00, 0x00, 0x00, 0xe4, 0x01, 0x01, 0xcc, 0x00, 0x00, 0x03, 0xe4, 0x01,

        0x01, 0xcc, 0x03, 0x00, 0x07, 0xdd, 0xc9, 0x02, 0x4b, 0x00, 0x4b, 0x00, 0xde, 0xc9, 0x02, 0x96,

        0x00, 0x96, 0x00, 0xcc, 0x04, 0x00, 0x09, 0xe9, 0xc9, 0x02, 0x1f, 0x4b, 0x00, 0x00, 0xff, 0x01,

        0xcc, 0x05, 0x00, 0x09, 0x4c, 0xca, 0x02, 0x6c, 0x04, 0xff, 0xba, 0xc6, 0xff, 0xcc, 0x06, 0x00,

        0x03, 0x60, 0xca, 0x02, 0xcc, 0x07, 0x00, 0x09, 0x4c, 0xcb, 0x02, 0x6d, 0x04, 0xff, 0xbf, 0xff,

        0xff, 0xcc, 0x08, 0x00, 0x03, 0x60, 0xcb, 0x02, 0xcc, 0x09, 0x00, 0x09, 0x4c, 0xcc, 0x02, 0x6e,

        0x04, 0xb9, 0xca, 0xff, 0xff, 0xcc, 0x0a, 0x00, 0x03, 0x60, 0xcc, 0x02, 0xcc, 0x0b, 0x00, 0x09,

        0x4c, 0xcd, 0x02, 0x79, 0x04, 0xff, 0xff, 0xff, 0xff, 0xcc, 0x0c, 0x00, 0x07, 0x44, 0xcd, 0x02,

        0x45, 0x01, 0x45, 0x01, 0xcc, 0x0d, 0x00, 0x03, 0x60, 0xcd, 0x02, 0xcc, 0x0e, 0x00, 0x07, 0x6a,

        0xc9, 0x02, 0x00, 0x00, 0x00, 0x00, 0xef, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00,

        0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0xcc, 0x0f, 0x00, 0x04, 0xb3, 0x53, 0x89, 0x32, 0xcc,

        0x10, 0x00, 0x04, 0xb3, 0x54, 0x89, 0x32, 0xcc, 0x11, 0x00, 0x04, 0xb3, 0x51, 0x89, 0x32, 0xcc,

        0x12, 0x00, 0x04, 0xb3, 0x52, 0x89, 0x32, 0x67, 0x8b, 0x02, 0xff, 0xff, 0x9c, 0xff, 0xcc, 0x13,

        0x00, 0x04, 0xb3, 0x4b, 0x80, 0x32, 0xcc, 0x14, 0x00, 0x07, 0x5a, 0xc9, 0x02, 0x00, 0x00, 0x80,

        0x00, 0x49, 0x20, 0x85, 0x6b, 0x3e, 0xcc, 0x15, 0x00, 0x07, 0xdd, 0xc9, 0x02, 0x4b, 0x00, 0x4b,

        0x00, 0xde, 0xc9, 0x02, 0x96, 0x00, 0x96, 0x00, 0x48, 0xc9, 0x02, 0x4b, 0x00, 0x96, 0x00, 0x72,

        0x06, 0x5c, 0x44, 0x23, 0x00, 0x0a, 0xcc, 0x16, 0x00, 0x03, 0x43, 0x4b, 0x00,
    };
    const std::vector<uint8_t> built = nox::BuildSnapshotTail(713, nox::CharClass::Wizard);
    Check(built.size() == 381, "꼬리 크기가 381 이 아니다");
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < 381 && i < built.size(); ++i)
        if (built[i] != kTail[i]) { ++diff; if (!first) first = i; }
    Check(diff == 0, "꼬리 조립이 실측과 다르다");
    if (diff) std::printf("    다른 자리 %zu개, 첫 자리 %zu (%02x != %02x)\n",
                          diff, first, built[first], kTail[first]);
    else std::printf("  스냅샷 꼬리 조립 — 381B 전수 일치\n");
}

void TestWallCollision() {
    const std::string a = "10.0.0.1:1000";

    // 걸음 입력 한 조각 — 방향(코드 1)로 바라보게 하고 앞으로(코드 2) 한 걸음.
    // ⚠ **와이어 머리 2바이트를 잊지 말 것** — 빼먹으면 서버가 op 를 머리로 떼어
    //   전부 어긋난다(이 시험이 처음에 그 탓에 「걸음이 안 갔다」로 실패했다).
    auto walkPkt = [](uint8_t facing) {
        std::vector<uint8_t> p(nox::kWireHeaderSize, 0);
        p.push_back(static_cast<uint8_t>(nox::Op::PLAYER_INPUT));
        p.push_back(10);                        // 항목 길이 합
        const uint8_t items[10] = { 1, 0, 0, 0, facing,
                                    2, 0, 0, 0, facing };
        p.insert(p.end(), items, items + 10);
        return p;
    };

    // ① 벽 없음 — 동쪽(0)으로 걸으면 x 가 는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);   // (1000,1000)
        std::vector<nox::OutPacket> out;
        auto pkt = walkPkt(0);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.XOf(a) > 1000, "벽이 없는데 걸음이 안 갔다");
    }

    // ② 동쪽 바로 앞이 벽 — 막힌다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        // (1000,1000) 동쪽 칸 = x 44 (1012/23=44) — 몸 반지름까지 치면 한 걸음에 닿는다
        std::vector<std::pair<uint8_t, uint8_t>> cells = { { 44, 42 }, { 44, 43 }, { 44, 44 } };
        srv.SetWalls(cells);
        std::vector<nox::OutPacket> out;
        for (int i = 0; i < 6; ++i) {   // 여섯 걸음 — 벽을 넘으면 x 가 1012 를 훌쩍 넘는다
            auto pkt = walkPkt(0);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        Check(srv.XOf(a) < 1012, "동쪽 벽을 뚫었다");
    }

    // ③ 대각(북동 = 224)으로 스치면 y 축으로 미끄러진다
    //   ⚠ 시작점 주의 — (1000,1000)은 몸 가장자리(+12)가 1012 = 칸 44 경계에 **정확히**
    //     걸려 시작부터 낀 상태다(처음에 이 배치로 「미끄러짐 없음」 오판). 990 에서 출발.
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugMoveTo(a, 990, 1000);
        std::vector<std::pair<uint8_t, uint8_t>> cells = { { 44, 42 }, { 44, 43 }, { 44, 44 } };
        srv.SetWalls(cells);
        const int y0 = srv.YOf(a);
        std::vector<nox::OutPacket> out;
        for (int i = 0; i < 6; ++i) {
            auto pkt = walkPkt(224);   // 북동 — x+ 는 막히고 y- 는 열려 있다
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        Check(srv.XOf(a) < 1012, "미끄러지다 벽을 뚫었다");
        Check(srv.YOf(a) < y0, "미끄러짐이 없다 — 대각에서 통째로 멈췄다");
    }

    std::printf("  벽 충돌 — 통과·막힘·미끄러짐\n");
}

void TestObserverExit() {
    nox::Server srv;
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    srv.DebugEnterGame(a, 728, /*observer=*/true);
    srv.DebugEnterGame(b, 729);   // 둘 다 (1000,1000) — 근접 사거리 안이다

    const uint16_t before = srv.HealthOf(b);

    // ① 첫 클릭은 관전 풀기다. **때리지 않는다**
    {
        std::vector<nox::OutPacket> out;
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == before, "관전 중 첫 클릭이 때렸다");
        Check(srv.melees() == 0, "관전 중 첫 클릭이 근접으로 세어졌다");

        const auto msgs = Flatten(out);
        const std::vector<uint8_t>* st =
            FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_CLIENT_STATUS));
        Check(st != nullptr, "관전 풀기 알림(0x6a)이 안 나갔다");
        if (st) {
            Check(st->size() == 7, "0x6a 길이가 7 이 아님");
            // 알리는 대상은 **자기 번호**다. 남의 번호를 실으면 클라가 엉뚱한 것을 깨운다
            Check((*st)[1] == 0xd8 && (*st)[2] == 0x02, "0x6a 가 자기 번호(728)를 안 실었다");
        }

        // ★★★ **남에게도 가야 한다.** 이게 안 가면 남의 화면에서 이 사람이 안 그려진다 —
        //   `2d NEW_PLAYER` 가 세운 관전 비트는 클라에서 `|=` 라 안 꺼지고,
        //   `DrawPlayer` 는 자기가 아닌 사람의 그 비트가 서 있으면 그리지 않고 되돌아간다.
        //   (2026-09-09 오픈녹스 원본: `drawable_player.go` · `network_client.go` · `cdecode.c` case 106)
        {
            const auto other = FlattenTo(out, b);
            const std::vector<uint8_t>* st2 =
                FindMsg(other, static_cast<uint8_t>(nox::Op::REPORT_CLIENT_STATUS));
            Check(st2 != nullptr, "관전 풀기 알림(0x6a)이 상대에게 안 갔다");
            if (st2) {
                Check(st2->size() == 7, "상대에게 간 0x6a 길이가 7 이 아님");
                Check((*st2)[1] == 0xd8 && (*st2)[2] == 0x02,
                      "상대에게 간 0x6a 가 관전을 푸는 사람 번호(728)를 안 실었다");
                Check((*st2)[3] == 0 && (*st2)[4] == 0 && (*st2)[5] == 0 && (*st2)[6] == 0,
                      "상대에게 간 0x6a 상태가 0 이 아니다 — 관전 비트가 안 꺼진다");
            }
        }
    }

    // ② 두 번째 클릭부터는 여느 근접 공격이다
    {
        std::vector<nox::OutPacket> out;
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.HealthOf(b) == before - kWizardPunch, "관전을 푼 뒤 클릭이 안 때렸다");
        Check(srv.melees() == 1, "관전을 푼 뒤 근접 집계가 1 이 아님");
    }

    std::printf("  관전 풀기 — 첫 클릭은 알림·둘째부터 공격\n");
}

// ── 투사체만의 성질 ───────────────────────────────────────────
//
// 시전하는 순간 맞으면 **피할 수가 없다.** 여기서 보는 것은 「비키면 안 맞나」다.
// 그게 안 되면 투사체를 만든 뜻이 없다.
void TestProjectile() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";

    // ① 옆으로 비켜 있으면 안 맞는다 — 앞으로만 날아가니까
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000, 1100);   // 바라보는 쪽(+x)이 아니라 옆(+y)이다
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "옆에 비켜 있는데 맞음");
        Check(srv.hits() == 0, "안 맞았는데 맞은 것으로 셈");
    }

    // ② 사거리를 넘으면 사라진다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000 + static_cast<int>(nox::kSpellRange) + 100, 1000);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "사거리 밖인데 맞음");
        Check(srv.flying() == 0, "사거리를 넘었는데 안 사라짐");
    }

    // ③ 쏜 사람은 자기 것에 안 맞는다 — 자기 자리에서 나가니 이게 없으면 늘 자폭한다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth, "쏜 사람이 자기 것에 맞음");
    }

    // ④ 가는 도중에는 아직 안 맞는다 — 한 틱만 돌려 본다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1180, 1000);   // 한 틱(26)으로는 못 닿는 거리
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        srv.Tick(0, out);
        srv.Tick(33, out);                // 딱 한 틱
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "한 틱 만에 180 을 날아가 맞음");
        Check(srv.flying() == 1, "가는 도중인데 사라짐");
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) < nox::kDefaultMaxHealth, "끝까지 날아가도 안 맞음");
    }

    // ⑤ 바라보는 쪽을 바꾸면 그쪽으로 날아간다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000, 1100);   // +y 쪽
        // 바라보는 쪽을 +y(한 바퀴 256 중 64)로 돌린다 — 코드 1 이 방향이다
        std::vector<uint8_t> face(nox::kWireHeaderSize + 1 + 1 + 4 + 1, 0);
        face[nox::kWireHeaderSize] = static_cast<uint8_t>(nox::Op::PLAYER_INPUT);
        face[nox::kWireHeaderSize + 1] = 5;   // 항목 길이 = 코드4 + 데이터1
        face[nox::kWireHeaderSize + 2] = 1;   // CCTurn 같은 방향 코드
        face.back() = 64;                     // 한 바퀴 256 중 64 = +y
        std::vector<nox::OutPacket> out;
        srv.OnPacket(a, face.data(), face.size(), out);
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) < nox::kDefaultMaxHealth, "바라보는 쪽으로 안 날아감");
    }

    std::printf("  투사체 — 비키면 안 맞음·사거리·자폭 안 함·가는 도중·방향\n");
}

// ── 날아가는 것이 **화면에 실려 나가나** ────────────────────────
//
// 서버가 안다고 클라가 보는 것이 아니다. `0xa4` 항목에 실려야 그려진다.
// 그래서 나간 조각을 **다시 뜯어** 확인한다 — 「보냈다」가 아니라 「담겼다」를 본다.
void TestProjectileVisible() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    nox::Server srv;
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);
    srv.DebugMoveTo(b, 1600, 1000);   // 멀리 둬서 날아가는 동안 안 맞게

    std::vector<nox::OutPacket> out;
    auto pkt = SpellPacket(27, false);
    srv.OnPacket(a, pkt.data(), pkt.size(), out);
    out.clear();
    srv.Tick(0, out);
    srv.Tick(33, out);   // 한 틱만 — 아직 날아가는 중이다
    Check(srv.flying() == 1, "아직 날아가는 중이어야 함");

    // 나간 0xa4 를 뜯어 파이어볼(695)이 항목에 있는지 본다
    bool seen = false;
    int players = 0;
    for (const auto& p : out) {
        auto body = Body(p);
        if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
        nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
        Check(m.ok, "내가 만든 0xa4 를 내가 못 뜯음");
        for (const auto& e : m.objects) {
            if (e.type == 695) seen = true;
            if (e.type == nox::kPlayerType) ++players;
        }
    }
    Check(seen, "파이어볼(695)이 상태 갱신에 안 실림");
    Check(players > 0, "사람이 상태 갱신에서 사라짐");

    // 맞고 나면 더는 안 실려야 한다 — 안 지우면 화면에 영원히 떠 있는다
    FlyUntilDone(srv, out);
    out.clear();
    srv.Tick(33 * 200, out);
    bool still = false;
    for (const auto& p : out) {
        auto body = Body(p);
        if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
        nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
        for (const auto& e : m.objects) if (e.type == 695) still = true;
    }
    Check(!still, "사라진 파이어볼이 아직도 실려 나감");

    // 종류 번호가 주문마다 갈리나 — 뭉뚱그리면 번개가 파이어볼로 보인다
    Check(nox::Server::ProjectileType(27) == 695, "파이어볼 종류가 695 가 아님");
    Check(nox::Server::ProjectileType(43) == 710, "번개 종류가 710 이 아님");
    Check(nox::Server::ProjectileType(24) == 703, "에너지볼트 종류가 703 이 아님");
    Check(nox::Server::ProjectileType(4) == 0, "점멸이 날아가는 것으로 잡힘");

    std::printf("  투사체가 화면에 실린다 — 상태 갱신을 다시 뜯어 확인\n");
}

// ── 소환 (컨저러) ─────────────────────────────────────────────
void TestSummon() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";

    // ① 부르면 하나 생기고 **화면에도 실린다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(79, false);   // 79 = SPELL_SUMMON_BOMBER
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        Check(srv.pets() == 1, "불렀는데 안 생김");
        Check(srv.flying() == 0, "소환이 투사체로 날아감");

        out.clear();
        srv.Tick(0, out);
        srv.Tick(33, out);
        bool seen = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
            for (const auto& e : m.objects) if (e.type == 1348) seen = true;   // Bomber
        }
        Check(seen, "불려 나온 것(1348 Bomber)이 상태 갱신에 안 실림");
    }

    // ★★ ② **부른 사람은 안 때린다** — 이게 없으면 부르자마자 자기가 맞는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(79, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        srv.Tick(0, out);
        srv.Tick(33 * 200, out);   // 넉넉히 돌린다
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth, "부른 사람이 자기 것에게 맞음");
    }

    // ③ 남에게는 다가가 때린다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1400, 1000);   // 멀리 둔다 — 다가가야 닿는다
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(79, false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        srv.Tick(0, out);
        srv.Tick(33, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "부르자마자 멀리 있는 사람이 맞음");
        srv.Tick(33 * 300, out);          // 걸어가 때릴 시간을 준다
        Check(srv.HealthOf(b) < nox::kDefaultMaxHealth, "다가가서 안 때림");
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth, "부른 사람이 맞음");
    }

    // ④ 주문마다 다른 것이 나온다 — 뭉뚱그리면 유령이 곰으로 보인다
    Check(nox::Server::SummonType(79) == 1348, "BOMBER 가 1348 이 아님");
    Check(nox::Server::SummonType(85) == 1325, "GHOST 가 1325 가 아님");
    Check(nox::Server::SummonType(84) == 1337, "EMBER_DEMON 이 1337 이 아님");
    Check(nox::Server::SummonType(27) == 0, "파이어볼이 소환으로 잡힘");
    Check(nox::Server::ProjectileType(79) == 0, "소환이 투사체로도 잡힘");

    std::printf("  소환 — 생김·화면에 실림·부른 사람은 안 맞음·다가가 때림\n");
}

// ── 불려 나온 것도 맞고 죽는다 ─────────────────────────────────
void TestPetDies() {
    const std::string a = "10.0.0.1:1000";   // 부르는 쪽
    const std::string b = "10.0.0.2:1000";   // 때리는 쪽

    // ① 남의 소환수를 근접으로 때려 죽인다 — 그리고 **0x31 이 나가야 한다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        auto sum = SpellPacket(79, false);
        srv.OnPacket(a, sum.data(), sum.size(), out);
        Check(srv.pets() == 1, "안 불려 나옴");

        // b 를 소환수 옆에 세운다 (a 는 (1000,1000), 소환수는 그 앞 40)
        srv.DebugMoveTo(b, 1040, 1000);

        uint32_t ms = 0;
        srv.Tick(0, out);
        int guard = 0;
        while (srv.pets() > 0 && guard++ < 60) {
            out.clear();
            auto pkt = InputPacket(6);              // 왼버튼
            srv.OnPacket(b, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.pets() == 0, "때려도 안 죽음");
        Check(srv.petsKilled() == 1, "죽은 횟수가 1 이 아님");

        // ★ 마지막 묶음에 0x31 DESTROY_OBJECT 가 담겼나 — 안 보내면 시체가 남는다
        bool destroyed = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.size() == 3 &&
                body[0] == static_cast<uint8_t>(nox::Op::DESTROY_OBJECT)) destroyed = true;
        }
        Check(destroyed, "0x31 DESTROY_OBJECT 가 안 나감");

        // 죽은 뒤에는 상태 갱신에도 안 실려야 한다
        out.clear();
        ms += 33;
        srv.Tick(ms, out);
        bool still = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
            for (const auto& e : m.objects) if (e.type == 1348) still = true;
        }
        Check(!still, "죽은 소환수가 아직도 상태 갱신에 실림");
    }

    // ★★ ② 부른 사람의 공격에는 안 맞는다 (근거 없이 정한 규칙이지만 그대로 도는지)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        auto sum = SpellPacket(79, false);
        srv.OnPacket(a, sum.data(), sum.size(), out);
        uint32_t ms = 0;
        srv.Tick(0, out);
        for (int i = 0; i < 30; ++i) {
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.pets() == 1, "부른 사람이 자기 소환수를 죽임");
        Check(srv.petsKilled() == 0, "부른 사람의 공격에 맞음");
    }

    // ③ 투사체도 남의 소환수를 맞힌다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        auto sum = SpellPacket(79, false);
        srv.OnPacket(a, sum.data(), sum.size(), out);
        // b 를 소환수 뒤쪽에 세우고 소환수 쪽(+x)으로 쏘게 한다
        srv.DebugMoveTo(b, 900, 1000);
        // b 의 바라보는 쪽은 0(+x) 이라 소환수(1040,1000) 를 지나간다
        auto pkt = SpellPacket(27, false);
        srv.OnPacket(b, pkt.data(), pkt.size(), out);
        FlyUntilDone(srv, out);
        Check(srv.hits() >= 1, "투사체가 소환수를 못 맞힘");
    }

    std::printf("  소환수도 맞고 죽는다 — 0x31 로 사라짐·부른 사람 공격엔 안 맞음\n");
}

// ── 아레나 — 점수와 시계 ───────────────────────────────────────
void TestArena() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";

    // ① 근접으로 죽이면 **때린 쪽** 점수가 오른다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        uint32_t ms = 0;
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        int guard = 0;
        while (srv.IsAlive(b) && guard++ < 40) {
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(!srv.IsAlive(b), "안 죽음");
        Check(srv.KillsOf(a) == 1, "때린 쪽 점수가 1 이 아님");
        Check(srv.DeathsOf(b) == 1, "죽은 쪽의 죽은 수가 1 이 아님");
        Check(srv.KillsOf(b) == 0, "맞은 쪽에 점수가 붙음");
    }

    // ★★ ② 자기에게 건 주문으로 죽어도 **점수는 안 준다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        int guard = 0;
        while (srv.IsAlive(a) && guard++ < 20) {
            auto pkt = SpellPacket(27, /*onSelf=*/true);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        Check(!srv.IsAlive(a), "자기 주문으로 안 죽음");
        Check(srv.KillsOf(a) == 0, "자기를 죽이고 점수를 얻음");
        Check(srv.DeathsOf(a) == 1, "죽은 수가 안 오름");
    }

    // ③ 한도에 닿으면 판이 끝나고 **0x58 이 나간다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        uint32_t ms = 0;
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        int guard = 0;
        while (!srv.RoundOver() && guard++ < 2000) {
            out.clear();
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);   // 부활도 여기서 돈다
        }
        Check(srv.RoundOver(), "한도에 닿아도 안 끝남");
        Check(srv.KillsOf(a) >= nox::kArenaKillLimit, "점수가 한도에 안 닿음");

        bool won = false;
        for (const auto& p : out) {
            auto body = Body(p);
            // 껍데기(0xcc) 속에 들어간다
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT) &&
                body[4] == static_cast<uint8_t>(nox::Op::REPORT_DEATHMATCH_WINNER)) {
                won = true;
                // ★★★ 속 짜임 — 오픈녹스 `cdecode.c` case 88 에서 확정
                //   58 | 승자 번호 2 | 끝난 까닭 1 | 프레임 4
                Check(body.size() >= 12, "0x58 이 8바이트가 아니다");
                if (body.size() >= 12) {
                    Check((body[5] | (body[6] << 8)) == 728, "0x58 의 승자 번호가 728 이 아니다");
                    Check(body[7] == 0,
                          "0x58 의 끝난 까닭이 0(보통 승리)이 아니다 — 여기에 점수를 실으면 안 된다");
                    const uint32_t f = static_cast<uint32_t>(body[8]) |
                                       (static_cast<uint32_t>(body[9]) << 8) |
                                       (static_cast<uint32_t>(body[10]) << 16) |
                                       (static_cast<uint32_t>(body[11]) << 24);
                    // 0 이면 클라가 `프레임 <= 저장값(0)` 으로 **통째로 버린다**
                    Check(f > 0, "0x58 의 프레임이 0 이다 — 클라가 그냥 버린다");
                }
            }
        }
        Check(won, "0x58 REPORT_DEATHMATCH_WINNER 가 안 나감");
    }

    // ④ 판 시계 — 짜임이 실측 표본과 같은가
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        out.clear();
        srv.Tick(33 * 100, out);

        const std::vector<uint8_t>* timer = nullptr;
        std::vector<uint8_t> inner;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT) &&
                body[4] == static_cast<uint8_t>(nox::Op::TIMER_STATUS)) {
                inner.assign(body.begin() + 4, body.end());
                timer = &inner;
            }
        }
        Check(timer != nullptr, "0xd3 TIMER_STATUS 가 안 나감");
        if (timer) {
            Check(timer->size() == 13, "0xd3 이 13바이트가 아님");
            // 첫 4바이트는 실측 표본 전수가 0 이었다
            Check((*timer)[1] == 0 && (*timer)[2] == 0 && (*timer)[3] == 0 && (*timer)[4] == 0,
                  "0xd3 첫 칸이 0 이 아님");
            const int32_t remain = static_cast<int32_t>(
                (*timer)[5] | ((*timer)[6] << 8) | ((*timer)[7] << 16) |
                (static_cast<uint32_t>((*timer)[8]) << 24));
            const uint32_t tick = (*timer)[9] | ((*timer)[10] << 8) | ((*timer)[11] << 16) |
                                  (static_cast<uint32_t>((*timer)[12]) << 24);
            Check(tick == srv.frame(), "0xd3 의 틱이 지금 틱이 아님");
            Check(remain < 0, "한도가 없는데 남은 시간이 음수가 아님");
            // 실측에서 (남은시간 / 틱) 이 −35.6 근처였다. 우리 것도 그 언저리여야 한다
            const double ratio = static_cast<double>(remain) / static_cast<double>(tick ? tick : 1);
            Check(ratio < -30.0 && ratio > -40.0, "남은시간/틱 비가 실측(−35.6) 언저리가 아님");
        }
    }

    std::printf("  아레나 — 점수·자살은 점수 없음·승자 알림·판 시계\n");
}

// ── 깃발 (CTF) ───────────────────────────────────────────────
void TestCtf() {
    const std::string a = "10.0.0.1:1000";   // 빨강
    const std::string b = "10.0.0.2:1000";   // 파랑

    // 판을 차린다 — 빨강 진영 (1000,1000) · 파랑 진영 (2000,1000)
    auto setup = [&](nox::Server& srv) {
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.StartCtf(1000, 1000, 2000, 1000);
        srv.DebugSetTeam(a, 1);
        srv.DebugSetTeam(b, 2);
        // ⚠ 둘 다 기본 자리가 (1000,1000) 인데 그게 **빨강 깃발의 제자리**다.
        //   그대로 두면 파랑인 b 가 시작하자마자 빨강 깃발을 주워 간다 —
        //   처음에 이걸 놓쳐 시험 다섯 줄이 한꺼번에 떨어졌다. **코드는 맞게 굴었다.**
        srv.DebugMoveTo(b, 1500, 1500);
    };

    // ① 깃발이 **화면에 실린다** (복합이 아니라 붙는 바이트가 없어야 한다)
    {
        nox::Server srv;
        setup(srv);
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        out.clear();
        srv.Tick(33, out);
        int redSeen = 0, blueSeen = 0, checked = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
            Check(m.ok, "깃발이 든 0xa4 를 못 뜯음");
            ++checked;

            // ★ 항목 수를 센다 — 사람 둘 + 깃발 둘 = 넷.
            //
            //   처음에는 `e.extra == 0` 을 봤는데 **그건 시험이 아니었다.**
            //   `extra` 는 뜯는 쪽이 **종류를 보고 정하는 값**이라, 내가 한 바이트를
            //   더 실어도 그대로 0 으로 돌아온다. 일부러 붙여 봤더니 안 걸렸다.
            //   대신 **개수**를 보면 걸린다 — 군더더기 바이트는 다음 항목의 시작으로
            //   읽혀 목록이 어긋나기 때문이다.
            Check(m.objects.size() == 4, "상태 갱신 항목이 넷이 아님 (군더더기 바이트?)");
            for (const auto& e : m.objects) {
                if (e.type == nox::kFlagTypeRed) ++redSeen;
                if (e.type == nox::kFlagTypeBlue) ++blueSeen;
            }
        }
        Check(checked > 0, "0xa4 가 아예 안 나감");
        Check(redSeen > 0 && blueSeen > 0, "깃발이 상태 갱신에 안 실림");
    }

    // ★★ ② 적 깃발에 닿으면 들고, **내 깃발은 안 든다**
    {
        nox::Server srv;
        setup(srv);
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        srv.DebugMoveTo(a, 1000, 1000);   // 자기 깃발 위
        srv.Tick(33, out);
        Check(!srv.Carrying(a), "자기 깃발을 들었다");

        srv.DebugMoveTo(a, 2000, 1000);   // 적 깃발 위
        out.clear();
        srv.Tick(66, out);
        Check(srv.Carrying(a), "적 깃발을 안 들었다");

        // 줍기 알림이 실제로 나갔나 (0xa9 갈래 6)
        bool picked = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.size() >= 2 && body[0] == static_cast<uint8_t>(nox::Op::INFORM) &&
                body[1] == static_cast<uint8_t>(nox::InformCode::FlagPickup)) picked = true;
        }
        Check(picked, "FlagPickup 알림이 안 나감");
    }

    // ③ 들고 내 진영에 닿으면 득점
    {
        nox::Server srv;
        setup(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        srv.DebugMoveTo(a, 2000, 1000);
        ms += 33; srv.Tick(ms, out);
        Check(srv.Carrying(a), "안 들었다");
        srv.DebugMoveTo(a, 1000, 1000);
        out.clear();
        ms += 33; srv.Tick(ms, out);
        Check(srv.CapturesOf(1) == 1, "득점이 안 됐다");
        Check(!srv.Carrying(a), "득점했는데 아직 들고 있다");
        bool cap = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.size() >= 2 && body[0] == static_cast<uint8_t>(nox::Op::INFORM) &&
                body[1] == static_cast<uint8_t>(nox::InformCode::FlagCapture)) cap = true;
        }
        Check(cap, "FlagCapture 알림이 안 나감");
    }

    // ★★ ④ 들고 있다 죽으면 **그 자리에 떨어뜨린다**
    {
        nox::Server srv;
        setup(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        srv.DebugMoveTo(a, 2000, 1000);
        ms += 33; srv.Tick(ms, out);
        Check(srv.Carrying(a), "안 들었다");

        out.clear();
        srv.Damage(a, 999, out, b);          // b 가 죽인다
        Check(!srv.IsAlive(a), "안 죽음");
        Check(!srv.Carrying(a), "죽었는데 아직 들고 있다");
        bool dropped = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.size() >= 2 && body[0] == static_cast<uint8_t>(nox::Op::INFORM) &&
                body[1] == static_cast<uint8_t>(nox::InformCode::FlagDrop)) dropped = true;
        }
        Check(dropped, "FlagDrop 알림이 안 나감");
    }

    // ⑤ 한도에 닿으면 판이 끝난다
    {
        nox::Server srv;
        setup(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        for (int i = 0; i < nox::kCaptureLimit; ++i) {
            srv.DebugMoveTo(a, 2000, 1000);
            ms += 33; srv.Tick(ms, out);
            srv.DebugMoveTo(a, 1000, 1000);
            ms += 33; srv.Tick(ms, out);
        }
        Check(srv.CapturesOf(1) == nox::kCaptureLimit, "득점이 한도에 안 닿음");
        Check(srv.RoundOver(), "한도에 닿아도 판이 안 끝남");
    }

    std::printf("  깃발 — 화면에 실림·적 것만 듦·득점·죽으면 떨굼·한도\n");
}

// 상태 갱신 본문에서 「전체 번호로 온 항목」을 종류별로 센다.
//
// ★★ 왜 뜯지 않고 바이트를 직접 보나 — 우리 `DecodeUpdate` 는 **종류표가 없어서**
//    복합 오브젝트 뒤에 몇 바이트가 붙는지 **짐작으로 갈래를 연다.** 항목이 서넛만 돼도
//    엉뚱한 갈래로 자리를 잡아 개수가 부풀 수 있다(퀘스트 시험에서 넷을 일곱으로 읽었다).
//    진짜 클라는 `thing.bin` 이 있어 안 헷갈리니 **와이어는 멀쩡했다** —
//    본문 58바이트가 손으로 센 값(머리 11 + 항목 11×4 + 끝 3)과 정확히 같았다.
//    그러니 시험이 그 짐작에 기대면 안 된다. 첫 등장 꼴을 바이트로 직접 센다.
//
//    첫 등장 항목의 꼴:  00 ff <번호 2> <종류 2> <x 2> <y 2> [붙는 1]
int CountFirstSight(const std::vector<uint8_t>& body, uint16_t type) {
    int n = 0;
    for (size_t i = 0; i + 6 < body.size(); ++i) {
        if (body[i] != 0x00 || body[i + 1] != 0xff) continue;
        const uint16_t t = static_cast<uint16_t>(body[i + 4] | (body[i + 5] << 8));
        if (t == type) ++n;
    }
    return n;
}

// ── 퀘스트 ────────────────────────────────────────────────────
//
// 「불려 나온 것」과 「맵의 몬스터」를 같은 것으로 다루는 설계라,
// 먼저 볼 것은 **사람이 주인 없는 몬스터를 때릴 수 있나**다. 이게 안 되면 판이 안 끝난다.
void TestQuest() {
    const std::string a = "10.0.0.1:1000";

    // ① 판을 차리면 몬스터가 서고 **화면에도 실린다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.StartQuest(3, 1000, 1000);
        Check(srv.pets() == 3, "몬스터가 셋이 아님");
        Check(srv.QuestLeft() == 3, "남은 수가 셋이 아님");

        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        out.clear();
        srv.Tick(33, out);
        int seen = 0, players = 0, streams = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            ++streams;
            seen += CountFirstSight(body, 1358);              // GiantLeech (퀘스트 몬스터)
            players += CountFirstSight(body, nox::kPlayerType);
        }
        Check(streams > 0, "0xa4 가 아예 안 나감");
        Check(seen == 3, "몬스터 셋이 상태 갱신에 다 안 실림");
        Check(players == 1, "사람이 상태 갱신에서 사라짐");
    }

    // ★★ ② 사람이 **주인 없는 몬스터를 때릴 수 있다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.StartQuest(1, 1000, 1000);
        srv.DebugMoveTo(a, 1300, 1000);   // 몬스터가 (1300,1000) 에 선다
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        const unsigned long long before = srv.petsKilled();
        int guard = 0;
        while (srv.pets() > 0 && guard++ < 40) {
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.pets() == 0, "사람이 몬스터를 못 때린다");
        Check(srv.petsKilled() == before + 1, "죽은 수가 안 오름");
    }

    // ③ 다 잡으면 판이 끝나고 **알림이 나간다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.StartQuest(2, 1000, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        int guard = 0;
        while (!srv.RoundOver() && guard++ < 400) {
            out.clear();
            // 몬스터가 다가와 붙으면 근접이 닿는다
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.RoundOver(), "다 잡아도 판이 안 끝남");
        Check(srv.QuestLeft() == 0, "남은 수가 0 이 아님");

        bool noticed = false;
        for (const auto& p : out) {
            auto body = Body(p);
            if (!body.empty() && body[0] == static_cast<uint8_t>(nox::Op::TEXT_MESSAGE)) {
                // 짜임이 맞나 — 머리 10 + 글자 길이
                Check(body.size() == 11 + body[8], "0xa8 길이가 머리10+글자와 안 맞음");
                Check((body[3] & 0x02) != 0, "UTF-8 깃발이 아님");
                noticed = true;
            }
        }
        Check(noticed, "퀘스트 완료 알림이 안 나감");
    }

    // ★★ ④ **소환수는 퀘스트 셈에 안 든다** — 주인이 있으니 맵 몬스터가 아니다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        const std::string b = "10.0.0.2:1000";
        srv.DebugEnterGame(b, 729);
        srv.StartQuest(2, 5000, 5000);          // 몬스터는 멀리 둔다
        std::vector<nox::OutPacket> out;
        auto sum = SpellPacket(79, false);
        srv.OnPacket(b, sum.data(), sum.size(), out);   // b 가 하나 부른다
        Check(srv.pets() == 3, "소환수까지 셋이 아님");

        uint32_t ms = 0;
        srv.Tick(0, out);
        int guard = 0;
        while (srv.pets() > 2 && guard++ < 40) {         // a 가 b 의 소환수를 잡는다
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.QuestKilled() == 0, "소환수를 잡았는데 퀘스트 셈이 올랐다");
        Check(!srv.RoundOver(), "소환수를 잡고 판이 끝났다");
    }

    std::printf("  퀘스트 — 몬스터가 서고·사람이 때리고·다 잡으면 끝·소환수는 셈 밖\n");
}

// ── 실측값 못 박기 ────────────────────────────────────────────
//
// ★★ 왜 이 시험이 따로 있나 — 2026-09-07 에 **돌연변이로 시험을 시험해 봤다.**
//    상수 열둘을 하나씩 틀어 보니 **넷만 걸리고 여덟은 안 걸렸다.**
//
//    안 걸린 여덟은 대개 **결함이 아니다.** 시험이 상수를 기호로 쓰기 때문이다 —
//      Check(HealthOf(b) == kDefaultMaxHealth - kMeleeDamage)
//    처럼 쓰면 `kMeleeDamage` 가 바뀌어도 양쪽이 같이 바뀌어 통과한다.
//    그리고 **그게 자리표시 값에는 옳은 설계다.** 아직 못 잰 숫자를 시험에 박아 두면
//    진짜 실측이 들어올 때마다 시험을 고쳐야 한다.
//
//    ★ 하지만 그 사이에 **실측값도 같이 안 묶여 있었다.** 덤프에서 바이트로 얻은 값은
//      함부로 바뀌면 안 되는데, 바꿔도 아무도 안 짖었다. 그래서 여기서 못 박는다.
//
//    ⚠ 이 시험이 깨지면 「시험을 고칠 것」이 아니라 **「왜 실측값을 바꿨나」를 물을 것.**
void TestMeasuredConstants() {
    // 접속할 때 주는 최대 체력 — 실측 `dd d0 02 4b 00 4b 00` (번호 720 · 75/75)
    Check(nox::kDefaultMaxHealth == 75, "최대 체력 실측값(75)이 바뀌었다");

    // 한 걸음 — 2026-09-06 캡처 표본 866개에서 가장 잦은 값 7
    Check(nox::kStepPerTick == 7.0, "걸음 실측값(7.0)이 바뀌었다");

    // 플레이어의 와이어 종류 번호 — 상태 갱신 30,169건이 이 번호였다
    Check(nox::kPlayerType == 713, "플레이어 종류 실측값(713)이 바뀌었다");

    // 주문 시전 본문 길이 — 표본 83개 전수로 확정
    Check(nox::kTrySpellBodyLen == 21, "0x79 본문 길이 실측값(21)이 바뀌었다");
    Check(nox::kSpellFireball == 27, "파이어볼 주문 번호(27)가 바뀌었다");

    // SERVER_INFO 의 토큰 자리
    Check(nox::kInfoTokenOffset == 41, "토큰 자리 실측값(41)이 바뀌었다");

    // 주문 번호 ↔ 이름 — 표가 어긋나면 여기서 걸린다
    Check(std::strcmp(nox::SpellName(4), "SPELL_BLINK") == 0, "주문 4 가 점멸이 아님");
    Check(std::strcmp(nox::SpellName(27), "SPELL_FIREBALL") == 0, "주문 27 이 파이어볼이 아님");
    Check(std::strcmp(nox::SpellName(43), "SPELL_CHAIN_LIGHTNING") == 0, "주문 43 이 연쇄번개가 아님");

    // 메시지 길이 — 전부 덤프의 진짜 바이트에서 얻은 것
    Check(nox::MsgPlayerDied::kSize == 2, "0xE8 길이(2)가 바뀌었다");
    Check(nox::MsgPlayerRespawn::kSize == 8, "0xE9 길이(8)가 바뀌었다");
    Check(nox::MsgReportPlayerHealth::kSize == 2, "0x43 길이(2)가 바뀌었다");
    Check(nox::MsgReportHealthDelta::kSize == 4, "0x42 길이(4)가 바뀌었다");
    Check(nox::MsgReportTotalHealth::kSize == 6, "0xDD 길이(6)가 바뀌었다");
    Check(nox::MsgReportPickup::kSize == 4, "0x4B 길이(4)가 바뀌었다");
    Check(nox::MsgReportModifiablePickup::kSize == 8, "0x4C 길이(8)가 바뀌었다");
    Check(nox::MsgReportDrop::kSize == 4, "0x4D 길이(4)가 바뀌었다");
    Check(nox::MsgReportEquip::kSize == 2, "0x60 길이(2)가 바뀌었다");
    Check(nox::MsgReportSpellStart::kSize == 1, "0x70 길이(1)가 바뀌었다");
    Check(nox::MsgSeqImportant::kHeaderSize == 3, "0xCC 머리 길이(3)가 바뀌었다");
    Check(nox::MsgTextMessage::kHeaderSize == 10, "0xA8 머리 길이(10)가 바뀌었다");

    // `0xa9` 갈래별 본문 길이 — opennox 원본에서 옮긴 것
    Check(nox::MsgInform::BodySize(nox::InformCode::Kill) == 9, "INFORM Kill 본문(9)이 바뀌었다");
    Check(nox::MsgInform::BodySize(nox::InformCode::FlagCapture) == 8, "INFORM 깃발 본문(8)이 바뀌었다");

    std::printf("  실측값 못 박기 — 상수·길이 23자리\n");
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
    TestMeasuredConstants();
    TestGameplayMessages();
    TestDeathFlow();
    TestBroadcastToOthers();
    TestAoiSightExit();
    TestSpellMana();
    TestManaSpend();
    TestRoundFlow();
    TestAim();
    TestChat();
    TestTimerAskAndClose();
    TestPickup();
    TestUseAndEquip();
    TestUseFromFile();
    TestSpellDamage();
    TestMelee();
    TestThingDb();
    TestMapSections();
    TestWallCollision();
    TestJoinBundle();
    TestSnapshotTail();
    TestSnapshotVsMap();
    TestGameBalance();
    TestSnapshotPerClass();
    TestMapNetCodeRule();
    TestPickupClass();
    TestAoiBudget();
    TestObserverExit();
    TestSnapshotSeqRenumber();
    TestProjectile();
    TestProjectileVisible();
    TestSummon();
    TestPetDies();
    TestArena();
    TestCtf();
    TestQuest();
    TestOpcodeTable();
    TestSpellNames();
    TestBounds();

    std::printf("\n%s\n", g_failed == 0 ? "전부 통과" : "실패 있음");
    return g_failed == 0 ? 0 : 1;
}
