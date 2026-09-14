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
#include "nox/Gamedata.h"
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
// ❌ 정정(2026-09-13 묶음 3-e) — 반올림이 아니라 **버림 + 소수 이월**이다(`GAME3_3.c:468` playerDecrementHPMana:
//   (1 − 방어) × 피해 를 float 로 · 앞서 남은 소수를 더해 (int) · 남은 소수는 다음에). 첫 대는 4.62 → **4**(0.62 이월).
const int kWizardPunch = static_cast<int>(static_cast<float>(
    (1.0 - static_cast<double>(nox::ArmorValueFloat(nox::CharClass::Wizard))) *
    nox::MeleeDamageFor(nox::CharClass::Wizard, nox::StrengthOnline(nox::CharClass::Wizard), /*kick=*/false)));

// out 안에서 이 오프코드로 시작하는 메시지를 찾는다. 껍데기(0xcc) 속도 들여다본다.
const std::vector<uint8_t>* FindMsg(const std::vector<std::vector<uint8_t>>& msgs, uint8_t op) {
    for (const auto& m : msgs) {
        if (!m.empty() && m[0] == op) return &m;
    }
    return nullptr;
}

// ★ **마지막** 것을 찾는다 (2026-09-11).
//   `0xdd`(체력 현재+최대)를 **접속 직후에도** 보내게 되면서, 부활을 보는 시험이
//   첫 번째(접속용)를 집어 엉뚱한 값을 봤다. 부활처럼 **나중 일**을 보는 자리는 이쪽을 쓴다.
static const std::vector<uint8_t>* FindLastMsg(const std::vector<std::vector<uint8_t>>& v,
                                               uint8_t op) {
    const std::vector<uint8_t>* found = nullptr;
    for (const auto& m : v) { if (!m.empty() && m[0] == op) found = &m; }
    return found;
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

std::vector<uint8_t> InputPacket(uint8_t code);   // 아래(조작 한 조각) — 3-c 부활 클릭에 앞에서도 쓴다

void TestDeathFlow() {
    nox::Server srv;
    const std::string who = "10.0.0.1:1000";
    srv.DebugEnterGame(who, 728);
    Check(srv.IsAlive(who), "들어오자마자 살아 있지 않음");
    Check(srv.HealthOf(who) == nox::kDefaultMaxHealth, "처음 체력이 최대가 아님");

    // ① 안 죽을 만큼 때린다 — ★ 09-14 정답지화: `42` 증감·`43` 절대값은 피해 자리가 아니라 **갱신 틱**이 보낸다
    //    `43` 은 다음 틱 PlayerReportAnything(+10 ≠ 지금 · Packet1) · `42` 는 마지막 피해(+536) 뒤 2틱 넘은 갱신(보는 사람별 · 음수만 · Packet0)
    {
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);                                   // 첫 호출은 기준점만
        for (int t = 1; t <= 3; ++t) srv.Tick(33 * t, warm); // 봐 둔다 — 처음 보는 오브젝트는 지금 값을 적을 뿐 42 를 안 보낸다(정답지 0 과 견줌 · +536 뒤 2틱)
        std::vector<nox::OutPacket> out;
        const bool died = srv.Damage(who, 30, out);
        Check(!died, "30 맞고 죽으면 안 됨");
        Check(srv.HealthOf(who) == 45, "75-30 이 45 가 아님");
        Check(FindMsg(Flatten(out), static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA)) == nullptr &&
              FindMsg(Flatten(out), static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH)) == nullptr,
              "피해 자리에서 42·43 을 곧바로 보냈다(정답지는 갱신 틱)");
        srv.Tick(33 * 4, out);                               // 1틱 뒤 — 43 은 나가고 42 는 아직(2틱 안)
        auto msgs = Flatten(out);
        const auto* h = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH));
        Check(h != nullptr, "0x43 이 다음 틱에 안 나감 (껍데기 안에 있어야 한다)");
        if (h) Check(((*h)[1] | ((*h)[2] << 8)) == 45, "0x43 값이 45 가 아님");
        Check(FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA)) == nullptr, "마지막 피해 뒤 2틱 안인데 0x42 가 나갔다");

        // 0x43 은 **껍데기 안**에 있어야 한다 — 겉으로 나가면 틀린 것이다
        bool wrapped = false;
        for (const auto& p : out) {
            auto b = Body(p);
            if (!b.empty() && b[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) wrapped = true;
        }
        Check(wrapped, "0xcc 껍데기가 안 쓰임");

        out.clear();
        for (int t = 5; t <= 7; ++t) srv.Tick(33 * t, out);  // 2·3·4틱 — 3틱째 갱신이 42
        msgs = Flatten(out);
        const auto* d = FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
        Check(d != nullptr, "0x42 가 안 나감");
        if (d) {
            Check(d->size() == 5, "0x42 가 5바이트가 아님");
            Check((*d)[1] == 0xd8 && (*d)[2] == 0x02, "0x42 의 번호가 728 이 아님");
            const int16_t delta = static_cast<int16_t>((*d)[3] | ((*d)[4] << 8));
            Check(delta == -30, "0x42 의 증감이 -30 이 아님");
        }
        Check(FindMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH)) == nullptr, "값이 안 바뀌었는데 0x43 을 또 보냈다");
        // 0x42 는 껍데기 **밖**(Packet0) 이다
        bool bare42 = false;
        for (const auto& p : out) { auto b = Body(p); if (!b.empty() && b[0] == static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA)) bare42 = true; }
        Check(bare42, "0x42 가 껍데기 안에 들어갔다(정답지 netSendPacket0)");
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
        // 체력이 0 밑으로 안 내려가야 한다 — 증감(−45)은 ④ 의 갱신 틱에서 잰다(09-14)
    }

    // ③ 죽어 있는 동안 또 때려도 아무 일 없어야 한다
    {
        std::vector<nox::OutPacket> out;
        Check(!srv.Damage(who, 10, out), "죽은 사람이 또 죽음");
        Check(out.empty(), "죽은 사람을 때렸는데 뭔가 나감");
    }

    // ④ ★ 2026-09-13 묶음 3-c — 시간만으로는 안 되살아난다(자동 부활 기본 꺼짐) · 상태 3 → 30틱 → 4 → 15틱 뒤 **클릭하면** 부활
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);                    // 첫 호출은 기준점만 잡는다
        out.clear();
        Check(srv.DebugStateOf(who) == 3, "죽은 순간 상태가 3(DIE) 이 아니다");
        srv.Tick(33 * 60, out);              // 60틱 — 3→4(30) · 15 도 넘김
        {   // 죽음의 `42` 도 갱신 틱이 보낸다 — 남은 45 만 깎여야 한다
            const auto msgs4 = Flatten(out);
            const auto* d = FindMsg(msgs4, static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA));
            Check(d != nullptr, "죽음의 0x42 가 갱신 틱에 안 나감");
            if (d) Check(static_cast<int16_t>((*d)[3] | ((*d)[4] << 8)) == -45, "남은 45 만 깎여야 하는데 아님");
        }
        Check(!srv.IsAlive(who), "클릭 없이 되살아났다 — 자동 부활은 정답지 기본 꺼짐");
        Check(srv.DebugStateOf(who) == 4, "죽고 30틱이 지났는데 상태 4(DEAD) 가 아니다");
        out.clear();
        auto click = InputPacket(6);
        srv.OnPacket(who, click.data(), click.size(), out);
        srv.Tick(33 * 61, out);              // 한 틱
        Check(srv.IsAlive(who), "상태 4 에서 클릭했는데 안 살아남");
        Check(srv.HealthOf(who) == nox::kDefaultMaxHealth, "되살아났는데 체력이 최대가 아님");

        auto msgs = Flatten(out);
        const auto* r = FindMsg(msgs, static_cast<uint8_t>(nox::Op::PLAYER_RESPAWN));
        Check(r != nullptr, "0xe9 가 안 나감");
        if (r) {
            Check(r->size() == 9, "0xe9 가 9바이트가 아님");
            Check((*r)[1] == 0xd8 && (*r)[2] == 0x02, "0xe9 의 번호가 728 이 아님");
            const uint32_t tick = (*r)[3] | ((*r)[4] << 8) | ((*r)[5] << 16) |
                                  (static_cast<uint32_t>((*r)[6]) << 24);
            // ★ 3-c — 프레임 = 받는 사람에게 같은 틱에 실린 시각(27) — 우리 서버는 접속마다 시계가 따로다
            const auto* ts = FindLastMsg(msgs, static_cast<uint8_t>(nox::Op::TIMESTAMP));
            Check(ts != nullptr && (tick & 0xFFFF) == static_cast<uint32_t>((*ts)[1] | ((*ts)[2] << 8)),
                  "0xe9 의 4바이트가 그 틱의 시각(27) 과 다름");
            Check((*r)[7] == 0xff && (*r)[8] == 0x01, "0xe9 끝 두 바이트가 표본과 다름");
        }
        const auto* tot = FindLastMsg(msgs, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH));
        Check(tot != nullptr, "0xdd 가 안 나감");
        if (tot) {
            Check(((*tot)[3] | ((*tot)[4] << 8)) == nox::kDefaultMaxHealth, "0xdd 의 지금 체력이 다름");
            Check(((*tot)[5] | ((*tot)[6] << 8)) == nox::kDefaultMaxHealth, "0xdd 의 최대 체력이 다름");
        }
    }

    std::printf("  죽음·부활 흐름 — 맞음→죽음→기다림→부활\n");
}

// ── 죽음·부활 정답지(묶음 3-c) ─────────────────────────────────────
//   diePlayer_54D2B0(상태 3) · object_update.go:242~275(3→4→클릭) · playerRespawn_4F7EF0 · mapFindPlayerStart_4F7AB0
void TestRespawnFaithful() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    auto click = [](nox::Server& srv, const std::string& who, std::vector<nox::OutPacket>& out) {
        auto p = InputPacket(6);
        srv.OnPacket(who, p.data(), p.size(), out);
    };

    // ① 자세 1(DIE) → 30틱 넘어 2(DEAD) · 상태 3 이나 4 의 15틱 안 클릭은 안 살림 · 그 뒤 클릭은 살림
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 9999, out, "");
        Check(srv.DebugAnimByteOf(a) == 1, "죽은 순간 자세가 1(DIE) 이 아니다");
        int to4 = -1;
        for (int t = 1; t <= 40; ++t) {
            if (t == 20) click(srv, a, out);                // 상태 3 중 클릭 — 무시
            ms += 33; srv.Tick(ms, out);
            if (to4 < 0 && srv.DebugStateOf(a) == 4) to4 = t;
        }
        Check(to4 == 31, "죽고 31틱째(30 넘김) 에 상태 4 가 아니다");
        Check(srv.DebugAnimByteOf(a) == 2, "상태 4 자세가 2(DEAD) 가 아니다");
        Check(!srv.IsAlive(a), "상태 3 중 클릭으로 살아났다");
        // 상태 4 된 지 9틱(40−31) — 15 안이라 클릭해도 안 살림
        click(srv, a, out);
        ms += 33; srv.Tick(ms, out);
        Check(!srv.IsAlive(a), "상태 4 된 지 15틱 안인데 클릭으로 살아났다");
        for (int t = 0; t < 10; ++t) { ms += 33; srv.Tick(ms, out); }
        Check(!srv.IsAlive(a), "클릭 없이 살아났다(자동 부활 꺼짐)");
        click(srv, a, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.IsAlive(a), "상태 4 15틱 뒤 클릭했는데 안 살아났다");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "살아난 뒤 상태가 13 이 아니다");
        Check(srv.IsInvulnerable(a), "살아났는데 무적(버프 23) 이 아니다");
    }

    // ② 시작 지점 — 적(b)에게서 가장 먼 곳 · FORGET_DRAWABLES 는 본인에게만 · E9 는 모두 · DD 는 본인만 · DE 는 워리어가 아니면
    {
        nox::Server srv;
        srv.SetPlayerStarts({ { 1100.0f, 1000.0f }, { 3000.0f, 3000.0f }, { 1500.0f, 1000.0f } });
        srv.SetAutoRespawn(true);
        srv.DebugEnterGame(a, 728);                          // 위저드
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 9999, out, "");
        out.clear();
        int alive = -1;
        for (int t = 1; t <= 60 && alive < 0; ++t) {
            out.clear();                                    // 부활한 그 틱에 나간 것만 본다(HUD 되풀이 등 섞임 방지)
            ms += 33; srv.Tick(ms, out);
            if (srv.IsAlive(a)) alive = t;
        }
        Check(alive == 47, "자동 부활이 3(30)+4(15) 넘긴 47틱째가 아니다");
        Check(srv.XOf(a) == 3000 && srv.YOf(a) == 3000, "적에게서 가장 먼 시작 지점(3000,3000) 으로 안 옮겼다");
        const auto toA = FlattenTo(out, a);
        const auto toB = FlattenTo(out, b);
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::FORGET_DRAWABLES)) != nullptr, "본인에게 FORGET_DRAWABLES(ea) 가 없다");
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::FORGET_DRAWABLES)) == nullptr, "FORGET_DRAWABLES 가 남에게 갔다");
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::PLAYER_RESPAWN)) != nullptr, "E9 가 남에게 안 갔다");
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_MANA)) != nullptr, "위저드인데 DE(최대 마나) 가 없다");
        bool ddLeak = false;
        for (const auto& m : toB)
            if (m.size() >= 3 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH) && m[1] == 0xd8 && m[2] == 0x02) ddLeak = true;
        Check(!ddLeak, "a(728) 의 DD 가 남에게 갔다");
    }
    {
        // 적이 없으면 무작위 — 시작 지점 중 하나 · 워리어는 DE 없음
        nox::Server srv;
        srv.SetPlayerStarts({ { 1100.0f, 1000.0f }, { 3000.0f, 3000.0f } });
        srv.SetAutoRespawn(true);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 9999, out, "");
        out.clear();
        for (int t = 0; t < 60 && !srv.IsAlive(a); ++t) { out.clear(); ms += 33; srv.Tick(ms, out); }
        const bool atStart = (srv.XOf(a) == 1100 && srv.YOf(a) == 1000) || (srv.XOf(a) == 3000 && srv.YOf(a) == 3000);
        Check(atStart, "적이 없는데 시작 지점 중 한 곳으로 안 옮겼다");
        Check(FindMsg(FlattenTo(out, a), static_cast<uint8_t>(nox::Op::REPORT_TOTAL_MANA)) == nullptr, "워리어인데 DE 가 나갔다");
    }
    {
        // 시작 지점이 없으면 (2000,2000)
        nox::Server srv;
        srv.SetAutoRespawn(true);
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 9999, out, "");
        for (int t = 0; t < 60 && !srv.IsAlive(a); ++t) { ms += 33; srv.Tick(ms, out); }
        Check(srv.XOf(a) == 2000 && srv.YOf(a) == 2000, "시작 지점이 없는데 (2000,2000) 이 아니다");
    }

    // ③ 종류 16 으로 죽으면 자세 51(DUST)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(a, -1, 9999, b, 16, out);
        Check(!srv.IsAlive(a), "종류 16 피해로 안 죽었다 — 이 시험이 무의미해진다");
        Check(srv.DebugAnimByteOf(a) == 51, "종류 16 으로 죽었는데 자세가 51(DUST) 이 아니다");
    }
    std::printf("  죽음·부활(정답지) — 자세 1→2·30/15틱·클릭 부활·가장 먼 시작 지점·무작위·(2000,2000)·ea 본인만·e9 모두·dd/de·먼지 51\n");
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
    //   ★ 09-14 정답지화 — 갱신 틱이 **보는 사람마다** 마지막 피해 뒤 2틱 넘어 보낸다(netPlayerObjSend_518C30 a4 갈래)
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);                                        // 기준점
        for (int t = 1; t <= 3; ++t) srv.Tick(33 * t, out);      // 봐 둔다(처음 보는 오브젝트는 42 를 안 보낸다 · +536 뒤 2틱)
        srv.Damage(a, 30, out);
        for (int t = 4; t <= 7; ++t) srv.Tick(33 * t, out);
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
        srv.Tick(33 * 60, warm);                                  // 상태 4 까지(3-c)
        std::vector<nox::OutPacket> out;
        auto click = InputPacket(6);
        srv.OnPacket(a, click.data(), click.size(), out);        // 클릭해야 부활(3-c)
        srv.Tick(33 * 61, out);
        Check(srv.IsAlive(a), "클릭했는데 안 되살아남");
        const auto toB = FlattenTo(out, b);
        const auto* r = FindMsg(toB, static_cast<uint8_t>(nox::Op::PLAYER_RESPAWN));
        Check(r != nullptr, "남이 되살아난 0xE9 가 옆 사람에게 안 감");
        if (r) {
            Check(r->size() == 9, "방송된 0xE9 가 9바이트가 아님");
            Check((*r)[1] == 0xd8 && (*r)[2] == 0x02, "방송된 0xE9 의 번호가 다름");
        }
        // ❌ 정정(3-c) — `0xDD` 는 **본인에게만** 간다(netReportTotalHealth_4D85C0 의 받는 이 = 그 사람 번호 · GAME3_3.c:7962)
        Check(FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH)) == nullptr,
              "되살아난 사람의 0xDD 가 옆 사람에게 새어 나감");
        const auto toA2 = FlattenTo(out, a);
        const auto* tot = FindMsg(toA2, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH));
        Check(tot != nullptr && tot->size() == 7, "되살아난 본인이 0xDD 를 못 받음");
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

// ── 바라보는 쪽이 갱신 항목에 실리는가 ────────────────────────────────────
//
// 왜 새로 쓰나 (2026-09-12) — 방향을 배선하고 **돌연변이를 넣었더니 시험 다섯이
// 하나도 안 잡았다.** `0x40` 고정으로 되돌려도 5/5 통과였다. 이 자리를 보는 시험이
// 아예 없었던 것이다. 그래서 여기를 새로 연다.
//
// 무엇을 보나 — 사람 항목의 **첫 덧붙이 바이트**다. 클라는 그것을 이렇게 쪼갠다:
//     방향 = (값 >> 4) & 7      (`GAME2_3.c:1677-1679`, 4 이상이면 +1)
// ❌ 정정(2026-09-13 묶음 3-a ④) — 서버가 싣는 값은 `((facing >> 5) & 7) << 4` 가 **아니다.**
//   정답지(`GAME4_1.c:4471~4478`)는 방향표 행(x+3y+4, blob 192088 · 문턱 6)을 4 이하 그대로 · 넘으면 −1 로 싣는다.
//     facing 0(동)    → 행 5 → 4 → **0x40**      facing 224(동북 위) → 행 2 → **0x20**
//     facing 64(남)   → 행 7 → 6 → **0x60**      facing 128(서)      → 행 3 → **0x30**      facing 192(북) → 행 1 → **0x10**
//   옛 식은 동쪽을 0(왼쪽 위 그림)으로 보내 **주먹이 엉뚱한 쪽으로** 나갔다(_evidence/b3a_punch_exp2).
// 자세는 **둘째 바이트**로 따로 가고 서 있으면 0 이다.
//
// ⚠ 자세를 같이 보는 까닭 — 방향만 세면 「자세 자리를 방향이 밀어냈다」를 못 잡는다.
//   그 사고를 이미 한 번 냈다(덮어쓰기 세 줄).
// ★ 판정을 **바이트열 통째 비교**로 한다 — 항목 직렬화 꼴을 몰라도 되고,
//   「어느 자리가 방향이냐」를 가정하지 않는다. 같은 상태에서 `facing` 만 바꿨으니
//   달라진 자리가 곧 방향 자리다.
//   ⚠ 패턴 세기(`CountBytes`)로는 못 판정한다 — `00 00` 은 좌표·머리에도 널려 있어
//     「찾았다」가 곧 「그 자리에서 찾았다」가 아니다. 처음에 그렇게 썼다가
//     ②가 우연히 통과했다.
static std::vector<uint8_t> FacingWire(const std::string& a, uint8_t facing) {
    nox::Server srv;
    srv.DebugEnterGame(a, 0x0501);
    srv.DebugMoveTo(a, 1000, 1000);
    srv.DebugSetFacing(a, facing);
    std::vector<nox::OutPacket> warm;
    srv.Tick(0, warm);                      // 첫 호출은 기준점만 잡는다
    std::vector<nox::OutPacket> out;
    srv.Tick(33, out);
    std::vector<uint8_t> all;
    for (const auto& p : out) {
        if (p.to != a) continue;
        const std::vector<uint8_t> b = Body(p);
        all.insert(all.end(), b.begin(), b.end());
    }
    return all;
}

void TestFacingOnWire() {
    const std::string a = "10.9.2.2:41022";
    const std::vector<uint8_t> w0   = FacingWire(a, 0);     // 동 — 행 5 → 0x40
    const std::vector<uint8_t> w224 = FacingWire(a, 224);   // 동북 위 — 행 2 → 0x20
    const std::vector<uint8_t> w0b  = FacingWire(a, 0);     // 대조군 — 같은 값 두 번

    Check(!w0.empty(), "갱신이 아예 안 나갔다 — 시험 전제가 깨졌다");
    Check(w0.size() == w224.size(), "같은 상태인데 길이가 다르다");

    // ★ 대조군이 0 이어야 한다. 아니면 프레임·난수 같은 것이 섞여 판정이 못 선다.
    size_t drift = 0;
    for (size_t i = 0; i < w0.size() && i < w0b.size(); ++i) if (w0[i] != w0b[i]) ++drift;
    Check(drift == 0, "같은 facing 두 번인데 바이트가 흔들린다 — 판정 불가");

    size_t diff = 0, at = 0;
    for (size_t i = 0; i < w0.size(); ++i) if (w0[i] != w224[i]) { ++diff; if (diff == 1) at = i; }

    // ★★ 여기가 **돌연변이를 잡는 자리**다 — `0x40` 고정으로 되돌리면 diff 가 0 이 된다.
    Check(diff > 0, "바라보는 쪽을 바꿨는데 와이어가 한 바이트도 안 달라진다");
    Check(diff == 1, "방향 말고 다른 자리까지 달라진다 — 자세를 밀어냈을 수 있다");
    Check(w0[at] == 0x40 && w224[at] == 0x20,
          "달라진 바이트가 정답지 방향 꼴이 아니다 (동 0x40 ↔ 동북 위 0x20 이어야 한다)");
    // 네 방위 — 같은 자리에서 정답지 값이 나오나
    {
        const std::pair<uint8_t, uint8_t> dirs[] = { { 64, 0x60 }, { 128, 0x30 }, { 192, 0x10 } };
        for (const auto& dv : dirs) {
            const std::vector<uint8_t> w = FacingWire(a, dv.first);
            Check(w.size() == w0.size() && at < w.size() && w[at] == dv.second,
                  "남·서·북 방향 바이트가 정답지 표(0x60·0x30·0x10) 와 다르다");
        }
    }

    std::printf("  바라보는 쪽 — facing 0↔224 로 **한 바이트**만 달라짐 "
                "(자리 %zu · 0x%02X→0x%02X · 대조군 흔들림 %zu)\n",
                at, w0[at], w224[at], drift);
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

// ★ 2026-09-13 묶음 4-a — 시험용 주문 정의. 값은 **thing.bin SPEL 그대로**(TestThingDb ⑨ 가 파일과 한 칸씩 대조한다).
//   정의 없는 주문은 시전이 안 된다(정답지 DefByInd nil → 코드 10). 소환은 컨저러 전용(깃발 0x4000000)이다.
struct TestSpellDef { uint8_t id; uint8_t mana; uint32_t flags; std::vector<uint8_t> phon; };
const std::vector<TestSpellDef>& TestSpellDefs() {
    static const std::vector<TestSpellDef> kDefs = {
        {  4, 10, 0x811c0491, { 5, 3, 1 } },                // SPELL_BLINK
        { 24,  0, 0x42340033, { 7, 5, 3, 1 } },             // SPELL_LIGHTNING
        { 12, 10, 0x222000b5, { 5, 3, 5 } },                // SPELL_CONFUSE(겨눈 주문 · 4-a2)
        { 41, 30, 0x01400214, { 8, 1, 6 } },                // SPELL_LESSER_HEAL(겨눈 · 공격 아님)
        { 27, 30, 0x42200038, { 7, 7, 1 } },                // SPELL_FIREBALL
        { 43,  0, 0x42340033, { 7, 5, 3, 1, 3, 5, 7 } },    // SPELL_CHAIN_LIGHTNING
        { 87,  0, 0x44542438, { 0, 8, 2, 6, 5, 6 } },       // SPELL_SUMMON_IMP
    };
    return kDefs;
}
// 정의를 넣는다 — mana < 0 이면 파일 값, 아니면 시험이 정한 값(비용 논리를 따로 볼 때)
void RegSpell(nox::Server& srv, uint8_t id, int mana = -1) {
    for (const auto& d : TestSpellDefs()) {
        if (d.id != id) continue;
        srv.SetSpellDef(id, static_cast<uint8_t>(mana < 0 ? d.mana : mana), d.flags, d.phon);
        return;
    }
    std::printf("  [실패] 시험 주문 표에 %u 가 없다\n", id);
}
void RegAllSpells(nox::Server& srv) { for (const auto& d : TestSpellDefs()) RegSpell(srv, d.id); }
// 시전 한 번 — 조각을 보내고 음소를 다 말할 때까지((음소 수+1) × 3틱) 틱을 돌린다
void CastAndWait(nox::Server& srv, const std::string& who, uint8_t spell, bool onSelf, uint32_t& ms,
                 std::vector<nox::OutPacket>& out) {
    srv.Tick(ms, out);                                   // 첫 부름이면 기준점만(시험이 이미 잡았으면 0틱)
    const std::vector<uint8_t> pkt = SpellPacket(spell, onSelf);
    srv.OnPacket(who, pkt.data(), pkt.size(), out);
    for (int t = 0; t < 60 && (srv.DebugCasting(who) || srv.CastPending() > 0); ++t) {
        ms += 34;
        srv.Tick(ms, out);
    }
}
// ★ 4-a — 소환 한 번. 소환은 컨저러 주문(깃발 0x4000000)이고 thing.bin 에 SUMMON_BOMBER(79) 정의가 없어 임프(87)를 부른다.
//   시전 틀(음소 7개 × 3틱)을 거치므로 서버 시계를 이어서 틱을 돌린다.
void SummonImp(nox::Server& srv, const std::string& who, std::vector<nox::OutPacket>& out) {
    RegSpell(srv, 87);
    srv.DebugSetClass(who, nox::CharClass::Conjurer);
    uint32_t ms = srv.DebugNowMs();
    CastAndWait(srv, who, 87, false, ms, out);
}

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
    RegSpell(srv, kSpell, kCost);   // 4-a — 정의(깃발·음소)까지
    Check(srv.SpellCost(kSpell) == kCost, "넣은 비용이 안 들어감");

    // ① 한 번 쏘면 그만큼 깎이고 `0x45` 가 **본인에게만** 간다
    //   ★ 4-a — 마나는 **음소를 다 말한 뒤**(PlayerSpell) 깎인다. 받은 틱에는 `70` 만 나가고 마나는 그대로다
    uint32_t ms = 0;
    {
        std::vector<nox::OutPacket> out;
        srv.Tick(ms, out);
        const std::vector<uint8_t> pkt = SpellPacket(kSpell, /*onSelf=*/false);   // 여기만은 틱을 손으로 돌린다
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        ms += 34; srv.Tick(ms, out);
        Check(srv.ManaOf(a) == full, "음소를 말하기도 전에 마나가 깎였다");
        Check(srv.DebugStateOf(a) == 2, "시전을 넣었는데 상태 2(CAST_SPELL) 가 아니다");
        const auto first = FlattenTo(out, a);
        const auto* st = FindMsg(first, static_cast<uint8_t>(nox::Op::REPORT_SPELL_START));
        Check(st && st->size() == 2 && (*st)[1] == kSpell, "첫 음소 틱에 `70 <주문>` 이 시전자에게 안 나감");
        Check(FindMsg(FlattenTo(out, b), static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) == nullptr, "`70` 이 남에게도 감");
        out.clear();
        for (int t = 0; t < 30 && srv.DebugCasting(a); ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.ManaOf(a) == full - kCost, "시전했는데 마나가 안 깎임");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "시전이 끝났는데 상태 13 으로 안 돌아옴");

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
        // ★ 09-14 — `45` 는 맨 메시지(netReportMana_4D8930 → sub_4E5450 → netSendPacket0)다. 껍데기(cc) 안에 든 45 는 옛 길
        for (const auto& p : out) {
            const auto body = Body(p);
            if (p.to == a && body.size() > 4 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT) && body[4] == static_cast<uint8_t>(nox::Op::REPORT_MANA))
                Check(false, "0x45 가 껍데기 안에 들어갔다(정답지 Packet0)");
        }
        // 성공 알림 — inform 1 <주문>(Kind1) · REPORT_SPELL_STAT <주문> 15(Unk21 깃발이 없는 주문)
        bool okInform = false, okStat = false;
        for (const auto& mm : toA) {
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::INFORM) && mm[1] == 1 && mm[2] == kSpell) okInform = true;
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && mm[1] == kSpell && mm[5] == 15) okStat = true;
        }
        Check(okInform, "시전 성공인데 `a9 01 <주문>` 이 없다");
        Check(okStat, "시전 성공인데 `df <주문> 0f` 가 없다");
    }

    // ② 마나가 바닥나면 효과가 안 난다 — 정답지는 첫 음소에 `70` 은 보내고, 끝에 마나를 못 내면 inform 0 코드 11 · STAT 0
    {
        const int room = srv.ManaOf(a) / kCost;
        for (int i = 0; i < room; ++i) {
            std::vector<nox::OutPacket> out;
            CastAndWait(srv, a, kSpell, false, ms, out);
        }
        Check(srv.ManaOf(a) < kCost, "다 쓰고도 비용만큼 남아 있음");

        const unsigned long long before = srv.spellsNoMana();
        std::vector<nox::OutPacket> out;
        CastAndWait(srv, a, kSpell, false, ms, out);
        Check(srv.spellsNoMana() == before + 1, "마나가 없는데 튕긴 것으로 안 셈");
        const auto toA = FlattenTo(out, a);
        Check(FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) != nullptr,
              "마나가 없어도 첫 음소에 `70` 은 나가야 한다(정답지 castByBook 은 마나를 끝에 본다)");
        bool code11 = false, stat0 = false;
        for (const auto& mm : toA) {
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::INFORM) && mm[1] == 0 && mm[2] == 11) code11 = true;
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && mm[1] == kSpell && mm[5] == 0) stat0 = true;
        }
        Check(code11, "마나가 모자란데 `a9 00 0b` 가 없다");
        Check(stat0, "마나가 모자란데 `df <주문> 00` 이 없다");
    }

    // ③ 정의가 없는 주문은 넣지도 않는다 — 코드 10(꺼짐) · STAT 0 · 마나 그대로
    {
        const uint16_t was = srv.ManaOf(a);
        std::vector<nox::OutPacket> out;
        CastAndWait(srv, a, /*spell=*/43, false, ms, out);   // 정의를 안 넣었다
        Check(srv.ManaOf(a) == was, "정의 없는 주문이 마나를 깎았다");
        Check(!srv.DebugCasting(a) && srv.DebugStateOf(a) != 2, "정의 없는 주문이 시전 목록에 들어갔다");
        bool code10 = false;
        for (const auto& mm : FlattenTo(out, a))
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::INFORM) && mm[1] == 0 && mm[2] == 10) code10 = true;
        Check(code10, "정의 없는 주문인데 `a9 00 0a` 가 없다");
    }

    // ④ 직업 거르기 — 위저드 주문(깃발 0x2000000)을 컨저러가 부르면 코드 9
    {
        nox::Server s2;
        RegSpell(s2, kSpell, kCost);
        const std::string c = "10.9.2.3:41023";
        s2.DebugEnterGame(c, 730, false, nox::CharClass::Conjurer);
        std::vector<nox::OutPacket> out;
        uint32_t m2 = 0;
        CastAndWait(s2, c, kSpell, false, m2, out);
        bool code9 = false;
        for (const auto& mm : FlattenTo(out, c))
            if (mm.size() == 6 && mm[0] == static_cast<uint8_t>(nox::Op::INFORM) && mm[1] == 0 && mm[2] == 9) code9 = true;
        Check(code9, "컨저러가 위저드 주문을 넣었는데 코드 9 가 아니다");
        Check(s2.ManaOf(c) == nox::ManaOnline(nox::CharClass::Conjurer), "직업이 안 맞는데 마나를 깎았다");
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

// ── P7 — 겉모습(0x4f~0x54)과 이펙트(0x85·0x8c) ──────────────────────────
//
// ★ 겉모습 — 입기·벗기가 그동안 **본인 표시(0x60/0x61)만** 보냈다. 남 화면의 몸이
//   안 바뀌던 자리다. 짜임은 클라 원본 확정 —
//     입기: op | 번호2(bit15=사람) | 장비비트4 | (수식4, 빈 칸 ff)
//     벗기: op | 번호2(bit15 안 봄) | 장비비트4
//   장비 비트표는 opennox 서버 원문 그대로(무기 27칸·방어구 26칸, GameBalance.cpp).
// ★ 이펙트 — 투사체가 끝나는 자리에서 한 번. 점 5B(0x85) · 선 9B(0x8c/0x8d).
void TestAppearanceAndFx() {
    // 표부터 — 교차검증에 쓴 넷이 원문 값 그대로인가
    bool w = false;
    Check(nox::EquipBitFor("Sword", &w) == 0x100 && w, "Sword 비트가 0x100 이 아님");
    Check(nox::EquipBitFor("WoodenShield", &w) == 0x1000000 && !w, "WoodenShield 비트가 다름");
    Check(nox::EquipBitFor("StaffWooden", &w) == 0x8000 && w, "StaffWooden 비트가 다름");
    Check(nox::EquipBitFor("WizardRobe", &w) == 0x4000 && !w, "WizardRobe 비트가 다름");
    Check(nox::EquipBitFor("NoSuchThing", &w) == 0, "모르는 이름인데 0 이 아님");

    const std::string a = "10.9.9.1:41091";
    const std::string b = "10.9.9.2:41092";

    // ① 입으면 남에게 0x51(무기·수식·11B)이 bit15 와 장비 비트를 싣고 간다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(a, 1000, 1000);
        std::vector<nox::Server::WorldStatic> st;
        nox::Server::WorldStatic sw;
        sw.netCode = 500; sw.type = 1160; sw.x = 1000; sw.y = 1000;
        sw.modifiable = true;                    // 무기라 수식이 붙는다
        sw.equipBit = 0x200;                     // Longsword
        sw.isWeapon = true;
        st.push_back(sw);
        nox::Server::WorldStatic unk;            // 표에 없는 것 — 겉모습을 안 보내야 한다
        unk.netCode = 501; unk.type = 700; unk.x = 1000; unk.y = 1000;
        st.push_back(unk);
        srv.SetWorldStatics(st);

        for (uint16_t nc : { (uint16_t)500, (uint16_t)501 }) {
            std::vector<nox::OutPacket> o1;
            const std::vector<uint8_t> g = CodePacket(nox::Op::TRY_GET, nc);
            srv.OnPacket(a, g.data(), g.size(), o1);
        }
        Check(srv.CarriedCount(a) == 2, "둘을 못 주웠다");

        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> e = CodePacket(nox::Op::TRY_EQUIP, 500);
        srv.OnPacket(a, e.data(), e.size(), out);
        const auto toB = FlattenTo(out, b);
        const auto* m = FindMsg(toB, static_cast<uint8_t>(nox::Op::REPORT_MODIFIABLE_WEAPON_EQUIP));
        Check(m != nullptr, "입었는데 남에게 0x51 이 안 감");
        if (m) {
            Check(m->size() == 11, "0x51 이 11바이트가 아님");
            Check((*m)[1] == 0xd8 && (*m)[2] == 0x82,
                  "번호에 bit15(사람 표식)가 안 실렸다");          // 0x02d8|0x8000 = 0x82d8
            Check((*m)[3] == 0x00 && (*m)[4] == 0x02 && (*m)[5] == 0x00 && (*m)[6] == 0x00,
                  "장비 비트 0x200 이 안 실렸다");
            Check((*m)[7] == 0xFF && (*m)[10] == 0xFF, "빈 수식 칸이 ff 가 아님");
        }
        Check(srv.appearanceSent() == 1, "겉모습 송신을 안 셌다");

        // ② 벗으면 0x54(7B) — bit15 없이
        std::vector<nox::OutPacket> out2;
        const std::vector<uint8_t> d = CodePacket(nox::Op::TRY_DEQUIP, 500);
        srv.OnPacket(a, d.data(), d.size(), out2);
        const auto toB2 = FlattenTo(out2, b);
        const auto* m2 = FindMsg(toB2, static_cast<uint8_t>(nox::Op::REPORT_WEAPON_DEQUIP));
        Check(m2 != nullptr, "벗었는데 남에게 0x54 가 안 감");
        if (m2) {
            Check(m2->size() == 7, "0x54 가 7바이트가 아님");
            Check((*m2)[1] == 0xd8 && (*m2)[2] == 0x02, "벗기에 bit15 가 실렸다 — 클라가 안 본다");
        }

        // ③ 장비 비트를 모르는 것 — 본인 0x60 은 가되 남에게는 아무것도 안 간다
        const unsigned long long unkBefore = srv.appearanceUnknown();
        std::vector<nox::OutPacket> out3;
        const std::vector<uint8_t> e2 = CodePacket(nox::Op::TRY_EQUIP, 501);
        srv.OnPacket(a, e2.data(), e2.size(), out3);
        Check(srv.appearanceUnknown() == unkBefore + 1, "모르는 장비를 안 셌다");
        const auto toB3 = FlattenTo(out3, b);
        for (uint8_t op : { 0x4F, 0x50, 0x51, 0x52 }) {
            const std::string why = "장비 비트를 모르는데 겉모습을 지어 보냈다";
            Check(FindMsg(toB3, op) == nullptr, why.c_str());
        }
    }

    // ④ 파이어볼이 끝난 자리에서 0x85 가 터진다 (5B, 좌표는 맞은 자리)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1000, 1140);   // 사거리(180) 안
        RegSpell(srv, 27, 0);

        std::vector<nox::OutPacket> all;
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);           // ★ 겨눔·시전은 Tick(0) 뒤에 — frame_ 이 맞아야 겨눔이 안 낡는다
        srv.DebugAimAt(a, 1000, 1140);   // 표적을 정통으로 겨눈다
        std::vector<nox::OutPacket> cast1;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, cast1); }   // 4-a 시전 틀
        for (int t = 1; t <= 40 && srv.hits() == 0; ++t) {
            std::vector<nox::OutPacket> o1;
            srv.Tick(33 * t, o1);
            all.insert(all.end(), o1.begin(), o1.end());
        }
        Check(srv.hits() == 1, "파이어볼이 안 맞았다 — 이펙트 시험 전제가 깨짐");
        const auto toBfx = FlattenTo(all, b);
        const auto* fx = FindMsg(toBfx, static_cast<uint8_t>(nox::Op::FX_EXPLOSION));
        Check(fx != nullptr, "맞았는데 0x85 폭발이 안 감");
        if (fx) {
            Check(fx->size() == 5, "0x85 가 5바이트가 아님");
            const int16_t fy = static_cast<int16_t>((*fx)[3] | ((*fx)[4] << 8));
            Check(fy > 1050 && fy < 1160, "폭발 좌표가 맞은 자리가 아님");
        }
        Check(srv.fxSent() == 1, "이펙트 송신을 안 셌다");
    }

    // ⑤ 번개는 선(9B) — 쏜 자리에서 끝난 자리까지
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1000, 1140);   // 사거리 안
        RegSpell(srv, 43, 0);

        std::vector<nox::OutPacket> all;
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);
        srv.DebugAimAt(a, 1000, 1140);
        std::vector<nox::OutPacket> cast1;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 43, false, castMs, cast1); }   // 4-a 시전 틀
        for (int t = 1; t <= 40 && srv.hits() == 0; ++t) {
            std::vector<nox::OutPacket> o1;
            srv.Tick(33 * t, o1);
            all.insert(all.end(), o1.begin(), o1.end());
        }
        const auto toBfx = FlattenTo(all, b);
        const auto* fx = FindMsg(toBfx, static_cast<uint8_t>(nox::Op::FX_LIGHTNING));
        Check(fx != nullptr, "번개가 끝났는데 0x8c 선이 안 감");
        if (fx) {
            Check(fx->size() == 9, "0x8c 가 9바이트가 아님");
            const uint16_t sy = static_cast<uint16_t>((*fx)[3] | ((*fx)[4] << 8));
            Check(sy > 900 && sy < 1100, "선의 시작이 쏜 자리가 아님");
        }
    }

    std::printf("  P7 — 입으면 남에게 0x51(bit15+장비비트) · 벗기 0x54 · 모르면 안 보냄 · 폭발 0x85·번개 0x8c\n");
}

// ── P5 — 부활 무적 · 문 · 오벨리스크 ────────────────────────────────────
//
// ★ 부활 무적 — 상수(`kRespawnInvulnTicks` 150)는 있었는데 **아무도 안 봤다**.
//   그래서 스폰 자리에 서 있던 사람이 되살아나는 즉시 다시 맞았다.
// ★ 문 — **다가가면 열린다.** 클라가 요청을 안 보낸다(정품 덤프에 문 관련
//   클라→서버 0건, 서버→클라 `0xb2` 만 201건). 여는 판단은 서버가 한다.
// ★ 오벨리스크 — 옆에 서면 마나가 찬다. gamedata 에 Regen 키가 0개라
//   이것이 정품 온라인의 마나 회복 수단으로 보인다.
void TestPropsAndInvuln() {
    const std::string a = "10.9.8.1:41081";

    // 부활 무적 — 죽고 되살아난 직후엔 안 맞는다
    {
        nox::Server srv;
        srv.SetAutoRespawn(true);            // 3-c — 서버 설정 자동 부활(무적 시험이라 클릭 절차는 TestDeathFlow 가 본다)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(a, 1000, 1000);
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);
        std::vector<nox::OutPacket> kill;
        srv.Damage(a, srv.HealthOf(a) + 100, kill);
        Check(!srv.IsAlive(a), "안 죽음");

        std::vector<nox::OutPacket> rez;
        srv.Tick(33 * (nox::kRespawnDelayTicks + 2), rez);
        Check(srv.IsAlive(a), "안 되살아남");
        const uint16_t full = srv.HealthOf(a);

        const unsigned long long before = srv.invulnBlocked();
        std::vector<nox::OutPacket> hit;
        srv.Damage(a, 30, hit);
        Check(srv.HealthOf(a) == full, "부활 직후인데 맞았다 — 무적이 안 걸렸다");
        Check(srv.invulnBlocked() == before + 1, "무적으로 막은 것을 안 셌다");

        std::vector<nox::OutPacket> later;
        srv.Tick(33 * (nox::kRespawnDelayTicks + nox::kRespawnInvulnTicks + 5), later);
        std::vector<nox::OutPacket> hit2;
        srv.Damage(a, 30, hit2);
        Check(srv.HealthOf(a) < full, "무적이 끝났는데도 안 맞는다 — 영영 무적이다");
    }

    // 문 — 멀면 닫혀 있고, 다가가면 열린다
    {
        nox::Server s2;
        s2.DebugEnterGame(a, 728);
        s2.DebugMoveTo(a, 5000, 5000);
        std::vector<nox::Server::WorldProp> ps;
        nox::Server::WorldProp door;
        door.netCode = 300; door.type = 306;
        door.x = 1000; door.y = 1000;
        door.kind = nox::PropKind::Door;
        ps.push_back(door);
        s2.SetWorldProps(ps);
        Check(s2.WorldPropCount() == 1, "문을 안 실었다");

        std::vector<nox::OutPacket> warm;
        s2.Tick(0, warm);
        std::vector<nox::OutPacket> far;
        s2.Tick(33, far);
        Check(!s2.PropOpen(300), "멀리 있는데 문이 열렸다");

        s2.DebugMoveTo(a, 1010, 1000);
        std::vector<nox::OutPacket> nearOut;
        s2.Tick(66, nearOut);
        Check(s2.PropOpen(300), "다가갔는데 문이 안 열렸다");
        const auto msgs = FlattenTo(nearOut, a);
        const auto* d = FindMsg(msgs, static_cast<uint8_t>(nox::Op::DOOR_ANGLE));
        Check(d != nullptr, "0xb2 가 안 나감");
        if (d) {
            Check(d->size() == 4, "0xb2 가 4바이트가 아님");
            Check((*d)[3] == nox::kDoorOpen, "열림 각이 안 실렸다");
        }
        // 안 바뀌면 다시 안 보낸다 — 매 틱 보내면 클라가 죽는다
        std::vector<nox::OutPacket> again;
        s2.Tick(99, again);
        Check(FindMsg(FlattenTo(again, a), static_cast<uint8_t>(nox::Op::DOOR_ANGLE)) == nullptr,
              "안 바뀌었는데 0xb2 를 또 보냈다");
    }

    // 오벨리스크 — 옆에 서면 마나가 찬다
    {
        nox::Server s3;
        s3.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        s3.DebugMoveTo(a, 1000, 1000);
        RegSpell(s3, 27, 60);
        std::vector<nox::OutPacket> burn;
        { uint32_t castMs = s3.DebugNowMs(); CastAndWait(s3, a, 27, false, castMs, burn); }   // 4-a 시전 틀
        const uint16_t low = s3.ManaOf(a);
        Check(low < nox::ManaOnline(nox::CharClass::Wizard), "마나를 안 썼다");

        std::vector<nox::Server::WorldProp> ps;
        nox::Server::WorldProp ob;
        ob.netCode = 400; ob.type = 121;
        ob.x = 1010; ob.y = 1000;
        ob.kind = nox::PropKind::Obelisk;
        ps.push_back(ob);
        s3.SetWorldProps(ps);

        std::vector<nox::OutPacket> warm;
        s3.Tick(0, warm);
        std::vector<nox::OutPacket> fill;
        s3.Tick(33 * (nox::kObeliskEveryTicks * 4), fill);
        Check(s3.ManaOf(a) > low, "오벨리스크 옆인데 마나가 안 찼다");
        Check(s3.obeliskMana() > 0, "오벨리스크가 채운 것을 안 셌다");
        Check(FindMsg(FlattenTo(fill, a), static_cast<uint8_t>(nox::Op::REPORT_MANA)) != nullptr,
              "마나 0x45 가 안 나감");
    }

    std::printf("  P5 — 부활 무적 · 문은 다가가면 열림(안 바뀌면 안 보냄) · 오벨리스크가 마나를 채움\n");
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
        RegSpell(srv, 27, 0);
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/false, castMs, out); }   // 4-a 시전 틀
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
        RegSpell(s2, 27, 0);
        s2.DebugAimAt(a, 1000, 1600);
        std::vector<nox::OutPacket> warm;
        s2.Tick(0, warm);
        std::vector<nox::OutPacket> aged;
        s2.Tick(33 * (nox::kAimStaleTicks + 5), aged);   // 겨눔을 늙힌다
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = s2.DebugNowMs(); CastAndWait(s2, a, 27, false, castMs, out); }   // 4-a 시전 틀
        std::vector<nox::OutPacket> t1;
        s2.Tick(s2.DebugNowMs() + 34, t1);   // 4-a — 시전 틀이 시계를 돌렸다 · 한 틱 더 날린다
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
        srv.Tick(srv.DebugNowMs(), hurt);                       // 기준점
        srv.Tick(srv.DebugNowMs() + 33, hurt);                  // 다친 체력 43 이 나간 뒤(09-14 — 알림은 갱신 틱)
        const uint16_t low = srv.HealthOf(a);
        Check(low < srv.HealthOf(a) + 40, "안 다쳤다");

        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_USE, kRed);
        srv.OnPacket(a, p.data(), p.size(), out);
        // 위저드 최대 체력은 75 라 40 맞고 50 을 마시면 **최대치에서 멈춘다**
        Check(srv.HealthOf(a) == nox::HealthOnline(nox::CharClass::Wizard),
              "물약을 마셨는데 최대치까지 안 참");
        Check(!srv.Carries(a, kRed), "마신 물약이 아직 인벤토리에 있다");

        // ★ 09-14 정답지화 — 회복은 `42` 로 안 나간다(netReportHealthDelta_4D8760 은 음수만) · `43` 절대값이 다음 틱에 간다
        Check(FindMsg(FlattenTo(out, a), static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA)) == nullptr, "회복(양수)을 0x42 로 보냈다");
        srv.Tick(srv.DebugNowMs() + 33, out);
        const auto toA1 = FlattenTo(out, a);
        const auto* h = FindMsg(toA1, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH));
        Check(h != nullptr && ((*h)[1] | ((*h)[2] << 8)) == nox::HealthOnline(nox::CharClass::Wizard),
              "회복 뒤 다음 틱 0x43 이 최대치가 아님");
        (void)low;
    }

    // ② 파란 물약 — 마나를 채우고 `0x45` 로 알린다
    {
        std::vector<nox::OutPacket> burn;
        RegSpell(srv, 27, 30);
        for (int i = 0; i < 2; ++i) {
            { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, burn); }   // 4-a 시전 틀
        }
        const uint16_t low = srv.ManaOf(a);
        Check(low < nox::ManaOnline(nox::CharClass::Wizard), "마나를 안 썼다");

        std::vector<nox::OutPacket> out;
        const std::vector<uint8_t> p = CodePacket(nox::Op::TRY_USE, kBlue);
        srv.OnPacket(a, p.data(), p.size(), out);
        Check(srv.ManaOf(a) == low + 50, "파란 물약이 50 을 안 채웠다");
        // ★ 09-14 — `45` 는 물약 자리가 아니라 다음 틱 PlayerReportAnything(+4 ≠ +6 · playerManaAdd 가 +6 을 남긴다)
        Check(FindMsg(FlattenTo(out, a), static_cast<uint8_t>(nox::Op::REPORT_MANA)) == nullptr, "물약 자리에서 0x45 를 곧바로 보냈다");
        srv.Tick(srv.DebugNowMs() + 33, out);
        const auto toA2 = FlattenTo(out, a);
        const auto* m = FindMsg(toA2, static_cast<uint8_t>(nox::Op::REPORT_MANA));
        Check(m != nullptr && m->size() == 5 && ((*m)[3] | ((*m)[4] << 8)) == low + 50,
              "다음 틱 0x45 가 물약 뒤 마나가 아님");
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
    nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    // ★ **둘 다 워리어로 둔다** — 정본 파이어볼은 최대 세기에서 127 이라
    //   위저드(75)는 한 방에 죽는다. 여기서 보려는 것은 「날아가서 맞나」이므로
    //   맞고도 살아남는 워리어(150)로 재야 판정이 흐려지지 않는다.
    //   ⚠ 한 방에 죽는 것 자체는 결함이 아니다 — 정본이 그렇다.
    srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
    srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
    const int kHP = nox::HealthOnline(nox::CharClass::Warrior);
    // ★ 4-a — 워리어는 주문을 못 쓴다(깃발 직업 거르기 코드 9). 체력은 워리어로 두고 직업만 위저드로 · 마나는 0 으로(맞기만 본다)
    srv.DebugSetClass(a, nox::CharClass::Wizard);
    RegSpell(srv, 27, 0);
    // ★ 이제 파이어볼은 **날아간다.** 바라보는 쪽이 기본 0(월드 +x)이므로
    //   맞으려면 b 가 a 의 앞쪽에 있어야 한다. 같은 자리에 두면 안 맞는 게 옳다.
    srv.DebugMoveTo(b, 1100, 1000);

    // ① 파이어볼을 남에게 — 맞는 쪽이 깎여야 한다
    {
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/false, castMs, out); }   // 4-a 시전 틀
        // ★ 쏘자마자는 아무도 안 깎여야 한다 — 날아가는 중이다
        Check(srv.HealthOf(b) == kHP, "쏘자마자 맞음 (투사체가 안 날아감)");
        Check(srv.flying() == 1, "날아가는 것이 하나가 아님");

        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == kHP - nox::Server::SpellDamage(27),
              "날아간 파이어볼이 안 맞음");
        Check(srv.HealthOf(a) == kHP, "쏜 쪽이 깎임");
        Check(srv.hits() == 1, "맞은 횟수가 1 이 아님");
        Check(srv.flying() == 0, "맞았는데 안 사라짐");

        // ★★★ **정정(2026-09-11) — `0x70` 은 시전자에게만, 중요 메시지로 간다.**
        //
        //   전에는 「보는 사람 **모두**에게 나갔나」를 봤는데 **그 기대에 근거가 없었다.**
        //   ⭕ 정품 송신 자리 `opennox/src/legacy/GAME4.c:2455-2463` —
        //       v23[0] = 112;  v23[1] = <주문 번호>;
        //       addToMsgListCli(*(uint8_t*)(v5 + 2064), 1, v23, 2);
        //     앞 인자가 **시전자의 플레이어 인덱스**, 둘째 `1` 이 **중요** 표시다.
        //   ⚠ 중요로 보내면 `cc SEQ_IMPORTANT` 봉투에 들어가므로 **봉투를 열어야** 보인다
        //     (`cc <ID 2> <길이 1> <본문…>` — 본문은 `body[4]` 부터).
        int spellStarts = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            const uint8_t* m = nullptr;
            size_t mn = 0;
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) {
                m = body.data() + 4;            // 봉투 속
                mn = body.size() - 4;
            } else if (!body.empty()) {
                m = body.data();
                mn = body.size();
            }
            if (m && mn >= 1 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) {
                Check(mn == 2, "0x70 이 2바이트가 아님");
                Check(m[1] == 27, "0x70 의 주문이 27 이 아님");
                ++spellStarts;
            }
        }
        Check(spellStarts == 1, "0x70 이 시전자 한 명에게 안 나감");
    }

    // ② 자기에게 걸면 자기가 깎인다 (대상 플래그 1)
    {
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/true, castMs, out); }   // 4-a 시전 틀
        Check(srv.HealthOf(a) == kHP - nox::Server::SpellDamage(27),
              "자기에게 건 파이어볼이 자기를 안 깎음");
        Check(srv.flying() == 0, "자기에게 건 것이 날아감");
    }

    // ③ 공격이 아닌 주문은 아무도 안 깎는다
    {
        const uint16_t before = srv.HealthOf(b);
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 4, /*onSelf=*/false, castMs, out); }   // 4-a 시전 틀   // 4 = 점멸
        Check(srv.HealthOf(b) == before, "점멸이 남을 깎음");
        Check(nox::Server::SpellDamage(4) == 0, "점멸이 공격으로 잡힘");
    }

    // ④ 계속 때리면 죽는다 — 흐름이 끝까지 이어지는지
    {
        int guard = 0;
        while (srv.IsAlive(b) && guard++ < 20) {
            std::vector<nox::OutPacket> out;
            { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/false, castMs, out); }   // 4-a 시전 틀
            FlyUntilDone(srv, out);
        }
        Check(!srv.IsAlive(b), "파이어볼을 스무 번 맞고도 안 죽음");
        Check(srv.HealthOf(b) == 0, "죽었는데 체력이 0 이 아님");
    }

    // ⑤ 죽은 사람은 다음 대상에서 빠진다 — 이제 a 만 남으니 아무도 안 맞는다
    {
        const uint16_t before = srv.HealthOf(a);
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/false, castMs, out); }   // 4-a 시전 틀
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

// ── 누가 누구의 적인가 ────────────────────────────────
//
// ★ 왜 재나 — `0x41`(유닛 현재 체력)은 **적이 아닐 때만** 나간다
//   (정품 `GAME4_2.c:7152-7157`). 즉 **남의 머리 위 체력**이 이 판정에 달려 있다.
//   규칙은 정품 `opennox/src/server/object.go:1667-1741` 에서 옮겼다(`Server.h` 주석 참조).
void TestEnemyTo() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";

    // ① 아레나 — 팀이 없으니 **서로 적**이다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        Check(srv.TeamOf(a) == 0 && srv.TeamOf(b) == 0, "아레나인데 팀이 붙었다");
        Check(srv.IsEnemyTo(a, b), "팀 없는 둘이 적이 아니라고 나온다");
        Check(!srv.IsEnemyTo(a, a), "자기 자신이 적이라고 나온다");
    }

    // ② 깃발 — **같은 팀은 아군, 다른 팀은 적**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.StartCtf(1000, 1000, 2000, 2000);
        const uint8_t ta = srv.TeamOf(a), tb = srv.TeamOf(b);
        Check(ta != 0 && tb != 0, "깃발 판인데 팀이 0 이다");
        if (ta == tb) Check(!srv.IsEnemyTo(a, b), "같은 팀인데 적이라고 나온다");
        else          Check(srv.IsEnemyTo(a, b), "다른 팀인데 아군이라고 나온다");
    }

    // ③ 퀘스트 — 협동이므로 **사람끼리는 아군**(➖ 우리 값)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.StartQuest(2, 1000, 1000, 100.0);
        Check(!srv.IsEnemyTo(a, b), "퀘스트인데 사람끼리 적이라고 나온다");
    }

    // ④ 없는 사람은 적이 아니다 (문지기)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        Check(!srv.IsEnemyTo(a, "9.9.9.9:1"), "없는 상대가 적이라고 나온다");
    }
}

// ── 「바뀌었으니 알려라」 틀과 `0x41` ────────────────────
//
// ★ 정품은 오브젝트에 **플레이어별 더티 비트**를 세우고 바뀐 것만 보낸다
//   (`GAME4_2.c:7136-7181`). 우리는 **세션에 목록**을 쌓아 같은 일을 한다.
// ★ `0x41` 짜임 ⭕ `GAME3_2.c:4022-4038` —
//     [0]=0x41 · [1..2]=넷코드 · **[3]=체력 >> 1** · 총 4 · 중요(`cc` 봉투 안)
void TestDirtyUnitHealth() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";

    // 봉투를 열어 `0x41` 을 센다 — 중요로 보내므로 `cc` 안에 있다.
    auto count41 = [](const std::vector<nox::OutPacket>& out, uint8_t* hpOut) {
        int n = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            const uint8_t* m = nullptr;
            size_t mn = 0;
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) {
                m = body.data() + 4; mn = body.size() - 4;
            } else if (!body.empty()) {
                m = body.data(); mn = body.size();
            }
            if (m && mn >= 4 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_HEALTH)) {
                if (hpOut) *hpOut = m[3];
                ++n;
            }
        }
        return n;
    };

    // ① 퀘스트 — 사람끼리 아군이므로 **간다**
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.StartQuest(2, 5000, 5000, 100.0);   // 사람에게서 멀리 — 몬스터가 안 끼게
        std::vector<nox::OutPacket> out;
        // ⚠ 2026-09-13 묶음 3-a — 주먹은 이제 애니가 끝나야 닿고 퀘스트에선 사람끼리 안 맞는다 → 피해를 직접 준다
        srv.Damage(b, 5, out, a);
        const uint16_t hp = srv.HealthOf(b);
        Check(hp < nox::kDefaultMaxHealth, "때렸는데 안 깎임");

        out.clear();
        // ⚠ `Tick(0)` **한 번으로는 아무것도 안 나온다** — 틱 주기를 안 넘긴다(조각 0 실측).
        //   몇 틱 굴려야 `FlushPending` 이 실제로 돈다.
        for (uint32_t ms = 0; ms <= 120; ms += 33) srv.Tick(ms, out);
        uint8_t got = 0xFF;
        const int n = count41(out, &got);
        Check(n >= 1, "퀘스트인데 0x41 이 한 번도 안 나감");
        Check(got == static_cast<uint8_t>(hp >> 1), "0x41 의 체력이 「체력>>1」이 아님");
        Check(srv.unitHealthSent() >= 1, "0x41 계수가 안 늘었다");
    }

    // ② 아레나 — 서로 적이므로 **안 간다** (대조군)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.Damage(b, 5, out, a);
        Check(srv.HealthOf(b) < nox::kDefaultMaxHealth, "때렸는데 안 깎임(아레나)");

        out.clear();
        srv.Tick(0, out);
        Check(count41(out, nullptr) == 0, "아레나인데 적에게 0x41 이 나갔다");
        Check(srv.unitHealthSent() == 0, "아레나인데 0x41 계수가 늘었다");
    }

    // ③ 목록은 보낸 뒤 비워진다 — 안 바뀐 틱에는 또 안 나간다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.StartQuest(2, 5000, 5000, 100.0);
        std::vector<nox::OutPacket> out;
        srv.Damage(b, 5, out, a);
        out.clear();
        for (uint32_t ms = 0; ms <= 120; ms += 33) srv.Tick(ms, out);   // 실제로 돌게
        const unsigned long long afterFirst = srv.unitHealthSent();
        Check(afterFirst >= 1, "첫 묶음에서 0x41 이 안 나갔다 — 이 시험이 무의미해진다");
        out.clear();
        for (uint32_t ms = 150; ms <= 300; ms += 33) srv.Tick(ms, out);   // 값이 안 바뀐 틱들
        Check(srv.unitHealthSent() == afterFirst, "안 바뀌었는데 또 보냄 — 목록이 안 비워졌다");
    }

    // ④ **불려 나온 것** — 주인에게만 간다 (남에게는 안 간다)
    //
    //   ⭕ 정품 `IsEnemyTo`(`opennox/src/server/object.go:1717-1721`) 는
    //     `own1 := obj.FindOwnerChainPlayer()` 로 **주인을 따라 올라가** 견준다.
    //     주인이 같으면 적이 아니다 → 내 소환수 체력은 나에게 온다.
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀          // 79 = SPELL_SUMMON_BOMBER (A 가 부른다)
        Check(srv.pets() == 1, "불렀는데 안 생김");

        out.clear();
        // 체력을 **정해서** 바꾼다 — 얼마가 남아 있든 시험이 선다(Bomber 는 체력 1 이다).
        const uint16_t petCode = srv.DebugSetOwnedPetHealth(a, 40);
        Check(petCode != 0, "내 소환수를 못 찾았다 — 이 시험이 무의미해진다");
        Check(srv.DebugOwnedPetHealth(a) == 40, "체력을 정했는데 안 바뀌었다");
        for (uint32_t ms = 0; ms <= 120; ms += 33) srv.Tick(ms, out);

        // 누구에게 갔는지 센다 — 받는 사람은 `OutPacket` 의 주소로 가른다.
        int toOwner = 0, toOther = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            const uint8_t* m = nullptr;
            size_t mn = 0;
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) {
                m = body.data() + 4; mn = body.size() - 4;
            } else if (!body.empty()) {
                m = body.data(); mn = body.size();
            }
            if (!m || mn < 4) continue;
            if (m[0] != static_cast<uint8_t>(nox::Op::REPORT_HEALTH)) continue;
            const uint16_t code = static_cast<uint16_t>(m[1] | (m[2] << 8));
            if (code != petCode) continue;
            if (p.to == a) ++toOwner;
            else if (p.to == b) ++toOther;
        }
        Check(toOwner >= 1, "내 소환수 체력이 주인에게 안 왔다");
        Check(toOther == 0, "남의 소환수 체력이 적에게 갔다");
    }
}

// ── 부활 무적을 클라에게 알리나 (`0x5A REPORT_ENCHANT`) ──
//
// ★ 짜임 ⭕ `GAME3_2.c:4496-4503` — `[0]=0x5A · [1..2]=넷코드 · [3..6]=비트 묶음(uint32)` · 중요
// ★ 비트 ⭕ `(Buffs & (1 << v))`(`client/drawable.go:739`) ·
//   `ENCHANT_INVULNERABLE = 23`(`server/spells_enchant.go:60`) → **1 << 23**
void TestEnchantInvuln() {
    const std::string a = "10.0.0.1:1000";
    nox::Server srv;
    srv.SetAutoRespawn(true);   // 3-c — 서버 설정 자동 부활(클릭 절차는 TestDeathFlow)
    srv.DebugEnterGame(a, 728);

    // 봉투를 열어 `0x5A` 의 비트 묶음을 꺼낸다. 없으면 false.
    auto find5a = [](const std::vector<nox::OutPacket>& out, uint32_t* bitsOut) {
        bool got = false;
        for (const auto& p : out) {
            auto body = Body(p);
            const uint8_t* m = nullptr;
            size_t mn = 0;
            if (body.size() >= 5 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT)) {
                m = body.data() + 4; mn = body.size() - 4;
            } else if (!body.empty()) {
                m = body.data(); mn = body.size();
            }
            if (!m || mn < 7) continue;
            if (m[0] != static_cast<uint8_t>(nox::Op::REPORT_ENCHANTMENT)) continue;
            if (bitsOut)
                *bitsOut = static_cast<uint32_t>(m[3]) | (static_cast<uint32_t>(m[4]) << 8) |
                           (static_cast<uint32_t>(m[5]) << 16) | (static_cast<uint32_t>(m[6]) << 24);
            got = true;
        }
        return got;
    };

    std::vector<nox::OutPacket> out;
    // ① 죽인다 → 부활까지 틱을 돌린다. 부활 순간 **무적이 켜지고** `0x5A` 가 나가야 한다.
    srv.Damage(a, 9999, out, "");
    Check(srv.HealthOf(a) == 0, "안 죽었다");
    out.clear();
    uint32_t ms = 0;
    for (int i = 0; i < 120; ++i) { ms += 33; srv.Tick(ms, out); }   // 부활 지연(45틱)을 넘긴다
    Check(srv.HealthOf(a) > 0, "안 되살아났다 — 이 시험이 무의미해진다");
    uint32_t bits = 0;
    Check(find5a(out, &bits), "부활했는데 0x5A 가 안 나갔다");
    Check(bits == nox::kEnchantInvulnerable, "0x5A 의 비트가 1<<23 이 아니다");
    const unsigned long long afterRevive = srv.enchantSent();
    Check(afterRevive >= 1, "0x5A 계수가 안 늘었다");

    // ② 무적이 **끝나면** 0 으로 한 번 더 알려야 한다 (150틱 = 5초)
    out.clear();
    for (int i = 0; i < 200; ++i) { ms += 33; srv.Tick(ms, out); }
    uint32_t bits2 = 0xFFFFFFFFu;
    Check(find5a(out, &bits2), "무적이 끝났는데 0x5A 가 안 나갔다");
    Check(bits2 == 0, "무적이 끝났는데 비트가 0 이 아니다");

    // ③ 그 뒤로는 **안 바뀌니 또 안 보낸다**
    const unsigned long long afterOff = srv.enchantSent();
    out.clear();
    for (int i = 0; i < 60; ++i) { ms += 33; srv.Tick(ms, out); }
    Check(srv.enchantSent() == afterOff, "안 바뀌었는데 0x5A 를 또 보냄");
}

void TestMelee() {
    // ★★ 2026-09-13 묶음 3-a — **정답지 맨손 공격**으로 다시 썼다(옛 시험은 클릭 즉시 깎임 · 15틱 쉬기를 쟀다).
    //   클릭 → ImplB 가 상태 1 · 그 틱이 상태 시작 → 틱마다 칸을 세고 **칸 수(주먹 8 · 붙듦 0)에 닿는 틱**에 판정
    //   → 같은 틱에 상태 13. 다음 공격은 첫 공격 틱 + 8 이후(Field0) — 한 주기 9틱.
    //   사거리 = 몸 반지름 10 + 20 · 앞쪽 60° 안(내적 > 0.5) · 방향 표 &1 · 벽 선 추적.
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    const int kPunchFrames = 8;   // ⭕ thing.bin PUNCH_LEFT/RIGHT/HOOK = 8칸 · 붙듦 0 (TestThingDb ⑥ 이 찍는다)
    auto makeSrv = [&](nox::Server& srv) {
        for (int i = 23; i <= 25; ++i) srv.SetPlayerAnimFrames(i, kPunchFrames, 0);
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1025, 1000);   // 반지름 합(20) 밖 · 사거리 안 · a 가 바라보는 +x 쪽
    };
    auto click = [&](nox::Server& srv, const std::string& who, uint8_t code, std::vector<nox::OutPacket>& out) {
        // 걸음(2~5)은 데이터 1바이트가 붙는다(Server.cpp PLAYER_INPUT 길이 표) — 없으면 서버가 조각을 버린다
        std::vector<uint8_t> pkt = InputPacket(code);
        if (code >= 2 && code <= 5) { pkt[nox::kWireHeaderSize + 1] = 5; pkt.push_back(0); }
        srv.OnPacket(who, pkt.data(), pkt.size(), out);
    };

    // ① 클릭한 순간에는 안 깎인다 · 상태 1 로 들어가 칸이 오른다 · 8틱째 판정 · 같은 틱에 상태 13
    {
        nox::Server srv;
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        click(srv, a, 6, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "클릭한 순간 바로 깎였다 — 애니를 안 기다린다");
        ms += 33; srv.Tick(ms, out);                               // 틱 T — 상태 1 로 들어간다
        Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "클릭한 틱에 상태 1 로 안 들어갔다");
        const uint8_t anim = srv.DebugAttackAnimOf(a);
        Check(anim == 23 || anim == 24, "위저드 맨손 애니가 23·24 가 아니다");
        int hitTick = -1;
        for (int t = 1; t <= 12 && hitTick < 0; ++t) {
            ms += 33; srv.Tick(ms, out);
            if (srv.HealthOf(b) != nox::kDefaultMaxHealth) hitTick = t;
            else Check(srv.DebugAnimFrameOf(a) == t, "공격 중 애니 칸이 (프레임 − 시작) 이 아니다");
        }
        Check(hitTick == kPunchFrames, "주먹이 애니 끝(8틱) 이 아닌 때에 닿았다");
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth - kWizardPunch, "한 대 피해가 맨손 식(힘×0.04+5, 방어) 과 다르다");
        Check(srv.melees() == 1, "근접 집계가 1 이 아님");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "판정한 틱에 상태 13 으로 안 돌아왔다");
        Check(srv.DebugAttackAnimOf(a) == 0, "공격이 끝났는데 애니 번호(Field8) 가 안 지워졌다");
    }

    // ② 공격 중 클릭은 무시 · 끝난 틱의 클릭도 쉬는 시간(Field0) 에 막힌다 · 그다음 틱부터 된다
    {
        nox::Server srv;
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        click(srv, a, 6, out);
        ms += 33; srv.Tick(ms, out);                               // T: 상태 1
        for (int t = 1; t < kPunchFrames; ++t) { click(srv, a, 6, out); ms += 33; srv.Tick(ms, out); }
        Check(srv.melees() == 0, "공격 중 클릭이 판정을 앞당겼다");
        click(srv, a, 6, out);
        ms += 33; srv.Tick(ms, out);                               // T+8: 판정 · 같은 틱 클릭은 Field0(T+9) 에 막힘
        Check(srv.melees() == 1, "8틱째 판정이 안 났다");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "쉬는 시간 중 클릭이 상태 1 로 넣었다");
        click(srv, a, 6, out);
        ms += 33; srv.Tick(ms, out);                               // T+9: 다시 상태 1
        Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "쉬는 시간이 끝났는데 공격이 안 들어간다");
    }

    // ③ 공격 중 걸음 코드는 안 먹는다(맨손 상태 1) · 걸음 코드는 때리지 않는다
    {
        nox::Server srv;
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        click(srv, a, 2, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == nox::kPlayerStateWalk, "걸음 코드가 걷기 상태로 안 바꿨다");
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "걸음 코드가 사람을 깎음");
        click(srv, a, 6, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "걷다가 클릭해도 공격으로 안 바뀐다");
        click(srv, a, 2, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "맨손 공격 중 걸음 코드가 공격을 끊었다");
    }

    // ④ 사거리 — 반지름 10 + 20 = 30 을 가장자리(중심 − 상대 반지름 10)까지 잰다 → 중심 39 는 닿고 41 은 안 닿는다
    {
        for (int dist : { 39, 41 }) {
            nox::Server srv;
            makeSrv(srv);
            srv.DebugMoveTo(b, 1000 + dist, 1000);
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            click(srv, a, 6, out);
            for (int t = 0; t < 12; ++t) { ms += 33; srv.Tick(ms, out); }
            const bool hit = srv.HealthOf(b) != nox::kDefaultMaxHealth;
            Check(dist == 39 ? hit : !hit, dist == 39 ? "사거리 안(39) 인데 안 맞음" : "사거리 밖(41) 인데 맞음");
        }
        Check(nox::kMeleeRange < nox::kSpellRange, "근접이 주문보다 멀다");
    }

    // ⑤ 방향 — 등 뒤(−x) · 옆(+y, 내적 0) 은 안 맞고, 그쪽을 보면 맞는다
    {
        struct Case { int bx, by; uint8_t face; bool expect; const char* why; };
        const Case cases[] = {
            { 975, 1000, 0, false, "등 뒤인데 맞음" },
            { 1000, 1025, 0, false, "옆(90°) 인데 맞음" },
            { 1000, 1025, 64, true, "+y 를 보는데 +y 가 안 맞음" },
            { 975, 1000, 128, true, "−x 를 보는데 −x 가 안 맞음" },
            // 두 문이 따로 막는 자리 — 계산은 blob 방향표·구역표로 뽑았다(방향 0: 표 ±67.5° · 내적 ±60°)
            { 1011, 1022, 0, false, "63° — 내적 문(> 0.5)이 안 막음" },
            { 1008, 1024, 16, false, "방향 16 에서 +49° — 방향 표 문(sub_4E6E50 &1)이 안 막음" },
        };
        for (const auto& c : cases) {
            nox::Server srv;
            makeSrv(srv);
            srv.DebugMoveTo(b, c.bx, c.by);
            srv.DebugSetFacing(a, c.face);
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            click(srv, a, 6, out);
            for (int t = 0; t < 12; ++t) { ms += 33; srv.Tick(ms, out); }
            Check((srv.HealthOf(b) != nox::kDefaultMaxHealth) == c.expect, c.why);
        }
    }

    // ⑥ 갱신 항목 — 공격 중에는 자세가 애니 번호 · 방향 바이트 0x80 · 칸 바이트가 붙는다(3바이트)
    {
        nox::Server srv;
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        click(srv, a, 6, out);
        for (int t = 0; t < 4; ++t) { ms += 33; srv.Tick(ms, out); }
        out.clear();
        ms += 33; srv.Tick(ms, out);
        bool seen = false;
        for (const auto& p : out) {
            if (p.to != a) continue;
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            nox::UpdateMsg m = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
            for (const auto& e : m.objects) {
                if (e.type != nox::kPlayerType || e.extra != 3) continue;
                if (!(e.extraBytes[0] & 0x80)) continue;
                const uint8_t anim = e.extraBytes[2];
                if (anim != 23 && anim != 24) continue;
                Check(e.extraBytes[1] == srv.DebugAnimFrameOf(a) || e.extraBytes[1] + 1 == srv.DebugAnimFrameOf(a) ||
                      e.extraBytes[1] == srv.DebugAnimFrameOf(a) + 1, "갱신의 애니 칸이 서버 칸과 한참 다르다");
                seen = true;
            }
        }
        Check(seen, "공격 중인데 갱신 항목에 [방향|0x80][칸][애니 23·24] 가 없다");
    }

    // ⑦ 벽 너머는 안 맞는다 — 칸 (44,43) 에 「\」 선분 하나(방향 1). y 1000 에서 x 1023 을 지난다.
    //   a(1000)·b(1038) 둘 다 선분에서 10 넘게 떨어져(16.3 · 10.6) 벽에 밀리지 않는다. 벽 없는 대조군은 맞는다.
    {
        for (bool wall : { true, false }) {
            nox::Server srv;
            makeSrv(srv);
            srv.DebugMoveTo(b, 1038, 1000);
            if (wall) srv.SetWallTiles({ nox::Server::WallTileIn{ 44, 43, 1, 0, false } });
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            click(srv, a, 6, out);
            for (int t = 0; t < 12; ++t) { ms += 33; srv.Tick(ms, out); }
            const bool hit = srv.HealthOf(b) != nox::kDefaultMaxHealth;
            Check(wall ? !hit : hit, wall ? "벽 너머인데 맞음" : "벽 없는 대조군(38) 이 안 맞음 — 벽 시험이 무의미해진다");
            Check(srv.XOf(b) == 1038 && srv.XOf(a) == 1000, "벽 시험 중 몸이 밀렸다 — 자리 가정이 깨졌다");
        }
    }

    // ⑧ 그늘 깃발(SHADOW 0x10000) 오브젝트가 선을 가리면 안 맞는다(MapTraceVision `server/map.go:539`).
    //   가리개 = 반지름 3 원 · NO_COLLIDE(0x40) 라 후보(0x8049 거름)도 아니다. 그늘만 뺀 대조군은 맞는다.
    {
        for (bool shadow : { true, false }) {
            nox::Server srv;
            makeSrv(srv);
            srv.DebugMoveTo(b, 1038, 1000);
            nox::Server::PhysBody blocker;
            blocker.x = 1019; blocker.y = 1000; blocker.shape = 2; blocker.r = 3.0f;
            blocker.flags = 0x40 | (shadow ? 0x10000u : 0u);
            srv.SetObstacles({ blocker });
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            click(srv, a, 6, out);
            for (int t = 0; t < 12; ++t) { ms += 33; srv.Tick(ms, out); }
            const bool hit = srv.HealthOf(b) != nox::kDefaultMaxHealth;
            Check(shadow ? !hit : hit, shadow ? "그늘 오브젝트 너머인데 맞음" : "그늘 없는 대조군이 안 맞음 — 그늘 시험이 무의미해진다");
        }
    }

    // ⑨ 방향 입력은 틱의 ImplB 에서 갈아 끼운다 — 그 틱의 힘(ImplA)은 **옛 방향**으로 준다(`object_update.go:117~118`).
    //   동쪽으로 걷던 중 +y(64) 를 받은 틱에는 y 로 힘이 안 들어가 y 가 그대로다. 다음 틱부터 내려간다.
    {
        nox::Server srv;
        makeSrv(srv);
        srv.DebugMoveTo(b, 5000, 5000);
        auto facePkt = [&](uint8_t dir) {
            std::vector<uint8_t> p = InputPacket(1);
            p[nox::kWireHeaderSize + 1] = 5;
            p.push_back(dir);
            return p;
        };
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 4; ++t) { click(srv, a, 2, out); ms += 33; srv.Tick(ms, out); }
        const int y0 = srv.YOf(a);
        Check(srv.XOf(a) > 1000 && y0 == 1000, "동쪽 걷기 준비가 안 됐다 — 이 시험이 무의미해진다");
        auto fp = facePkt(64);
        srv.OnPacket(a, fp.data(), fp.size(), out);
        click(srv, a, 2, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.YOf(a) == y0, "방향을 받은 틱에 벌써 새 방향으로 힘이 들어갔다(ImplA 가 ImplB 보다 먼저)");
        click(srv, a, 2, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.YOf(a) > y0, "방향을 받은 다음 틱에도 새 방향으로 안 간다");
    }

    std::printf("  근접 공격(맨손 정답지) — 애니 끝 판정·쉬는 시간·걸음 막힘·사거리 30·앞쪽·갱신 칸 바이트\n");
}

// ── 무기 공격(묶음 3-b) ─────────────────────────────────────────
//   정답지 `GAME4_3.c:4149~4172`(롱소드) · `GAME4.c:1614~1627`(기력) · `object_update.go:108`(기력 회복).
//   워리어 기본 장비 롱소드(LONG_SWORD 0x200) — 애니 28(8칸 · 붙듦 0) **한가운데 칸 4** 에 한 번 판정,
//   사거리 = 몸 반지름 10 + 무기 45 · 피해 (힘 125 − 30) × 0.3 + 9 → 38 · 기력 70 깎고 `47 <기력>` 알림.
void TestWeaponAttack() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    auto makeSrv = [&](nox::Server& srv, int bx) {
        srv.SetPlayerAnimFrames(28, 8, 0);   // ⭕ thing.bin LONG_SWORD_STRIKE 8/0 (TestThingDb ⑥)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729);           // 위저드 — 방어합 0.23
        srv.DebugMoveTo(b, bx, 1000);
    };
    auto click = [&](nox::Server& srv, std::vector<nox::OutPacket>& out) {
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
    };
    // 위저드 방어 0.23 — (1 − 0.23f) × 38 = 29.26 → 버림 29(3-e 피해 함수)
    const int kSword = static_cast<int>(static_cast<float>((1.0 - static_cast<double>(nox::ArmorValueFloat(nox::CharClass::Wizard))) * 38.0));

    // ① 기력은 0 에서 시작해 틱마다 3 — 모자라면 클릭해도 안 들어간다 · 70 넘으면 들어가고 70 깎고 알린다
    {
        nox::Server srv;
        makeSrv(srv, 1050);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        click(srv, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStaminaOf(a) == 3, "기력이 첫 틱에 3 이 아니다(0 에서 100/30)");
        Check(srv.DebugStateOf(a) != nox::kPlayerStateAttack, "기력 3 인데 롱소드 공격(70)이 들어갔다");
        for (int t = 0; t < 30; ++t) { ms += 33; srv.Tick(ms, out); }
        Check(srv.DebugStaminaOf(a) >= 70, "31틱 뒤 기력이 70 아래 — 회복이 안 돈다");
        const uint8_t before = srv.DebugStaminaOf(a);
        out.clear();
        click(srv, out);
        ms += 33; srv.Tick(ms, out);                             // T: 회복 3 → 깎기 70
        Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "기력이 넉넉한데 공격이 안 들어갔다");
        const int expect = std::min(before + 3, 102) - 70;
        Check(srv.DebugStaminaOf(a) == expect, "롱소드가 기력을 70 깎지 않았다");
        bool report = false;
        for (const auto& m : Flatten(out))
            if (m.size() == 2 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_STAMINA) && m[1] == expect) report = true;
        Check(report, "기력을 깎았는데 `47 <기력>` 알림이 없다");
        Check(srv.DebugAnimByteOf(a) == 28, "롱소드 공격 자세가 28(LONG_SWORD_STRIKE) 이 아니다");

        // ② 한가운데 칸(4) 에서 한 번만 · 칸 8 에서 상태 13
        int hitTick = -1, hits = 0;
        uint16_t hp = srv.HealthOf(b);
        for (int t = 1; t <= 10; ++t) {
            ms += 33; srv.Tick(ms, out);
            if (srv.HealthOf(b) != hp) { ++hits; if (hitTick < 0) hitTick = t; hp = srv.HealthOf(b); }
            if (t == 7) Check(srv.DebugStateOf(a) == nox::kPlayerStateAttack, "칸 7 인데 공격 상태가 끝났다");
            if (t == 8) Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "칸 8 인데 상태 13 으로 안 돌아왔다");
        }
        Check(hitTick == 4, "롱소드가 애니 한가운데(4틱) 가 아닌 때에 닿았다");
        Check(hits == 1, "한 번 휘두르는데 두 번 이상 맞았다");
        Check(srv.HealthOf(b) == nox::HealthOnline(nox::CharClass::Wizard) - kSword, "롱소드 피해가 (힘−30)×0.3+9 · 방어 식과 다르다");
    }

    // ③ 사거리 — 몸 10 + 무기 45 = 55 를 상대 가장자리까지 → 중심 64 는 닿고 66 은 안 닿는다
    for (int dist : { 64, 66 }) {
        nox::Server srv;
        makeSrv(srv, 1000 + dist);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 30; ++t) { ms += 33; srv.Tick(ms, out); }
        click(srv, out);
        for (int t = 0; t < 12; ++t) { ms += 33; srv.Tick(ms, out); }
        const bool hit = srv.HealthOf(b) != nox::HealthOnline(nox::CharClass::Wizard);
        Check(dist == 64 ? hit : !hit, dist == 64 ? "롱소드 사거리 안(64) 인데 안 맞음" : "롱소드 사거리 밖(66) 인데 맞음");
    }

    // ④ 공격하면 부활 무적이 꺼진다(무기 갈래는 클릭마다 버프 23 끄기 · `GAME4.c:1630`)
    {
        nox::Server srv;
        makeSrv(srv, 1050);
        srv.SetAutoRespawn(true);   // 3-c — 서버 설정 자동 부활(클릭 절차는 TestDeathFlow)
        std::vector<nox::OutPacket> out;
        srv.Damage(a, 9999, out, "");
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 120 && srv.HealthOf(a) == 0; ++t) { ms += 33; srv.Tick(ms, out); }
        Check(srv.HealthOf(a) > 0, "워리어가 안 되살아났다 — 이 시험이 무의미해진다");
        Check(srv.IsInvulnerable(a), "부활 직후 무적이 아니다 — 이 시험이 무의미해진다");
        click(srv, out);
        ms += 33; srv.Tick(ms, out);
        Check(!srv.IsInvulnerable(a), "무기 공격 클릭을 했는데 부활 무적이 안 꺼졌다");
    }

    // ⑤ 칸을 붙드는 애니(붙듦 1)면 칸 4 가 두 틱 이어진다 — 「지난 틱 칸보다 클 때만」(v76 아래 바이트)이 두 번 치기를 막는다
    {
        nox::Server srv;
        makeSrv(srv, 1050);
        srv.SetPlayerAnimFrames(28, 8, 1);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 30; ++t) { ms += 33; srv.Tick(ms, out); }
        click(srv, out);
        int hits = 0;
        uint16_t hp = srv.HealthOf(b);
        for (int t = 0; t < 20; ++t) {
            ms += 33; srv.Tick(ms, out);
            if (srv.HealthOf(b) != hp) { ++hits; hp = srv.HealthOf(b); }
        }
        Check(hits == 1, "붙듦 1 애니에서 한가운데 칸이 두 틱 이어져 두 번 맞았다");
    }

    std::printf("  무기 공격(롱소드) — 기력 0→3/틱·70 깎고 알림·자세 28·한가운데 칸 한 번·사거리 55·무적 끄기\n");
}

// ── 피해 함수(묶음 3-e) ─────────────────────────────────────────
//   사람 PlayerDamage(`GAME3_3.c:67`) · 몬스터 DefaultDamage 계열 → damageDefaultProc(`GAME3_2.c:7017`).
// ── 방패·막기(묶음 3-e2) ─────────────────────────────────────────
//   `object_update.go:327~398`(상태 13→15→16→17 · 18~21) · `GAME3_3.c:206~330`(막기 갈래) · `GAME4_3.c:2586`(들어오는 투사체)
void TestShieldBlock();
void TestDamageHandler() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    const int kWizHP = nox::HealthOnline(nox::CharClass::Wizard);

    // ① 방어 버림 + 소수 이월 — 위저드(0.23) 에 칼 6 을 셋: 4.62→4(.62) · 5.24→5(.24) · 4.86→4(.86)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(b, -1, 6, a, 0, out);
        Check(srv.HealthOf(b) == kWizHP - 4, "첫 대가 버림 4 가 아니다");
        srv.DebugHurt(b, -1, 6, a, 0, out);
        Check(srv.HealthOf(b) == kWizHP - 9, "둘째 대가 이월 더해 5 가 아니다");
        srv.DebugHurt(b, -1, 6, a, 0, out);
        Check(srv.HealthOf(b) == kWizHP - 13, "셋째 대가 이월 더해 4 가 아니다");
    }
    // ② 종류별 — 2(망치·철퇴) 는 방어 절반 · 1(불) 은 방어 없음 · 1 짜리가 0 으로 버려지면 1
    {
        struct Case { int dmg; uint8_t type; int expect; const char* why; };
        const Case cases[] = {
            { 6, 2, 5, "종류 2 가 (1 − 0.23×0.5)×6 = 5.31 → 5 가 아니다" },
            { 6, 1, 6, "종류 1(불) 이 방어 없이 6 이 아니다" },
            { 1, 0, 1, "0.77 → 0 이 1 로 올라가지 않았다" },
        };
        for (const auto& c : cases) {
            nox::Server srv;
            srv.DebugEnterGame(a, 728);
            srv.DebugEnterGame(b, 729);
            std::vector<nox::OutPacket> out;
            srv.DebugHurt(b, -1, c.dmg, a, c.type, out);
            Check(srv.HealthOf(b) == kWizHP - c.expect, c.why);
        }
    }
    // ③ 편 거르기 — 같은 팀이면 안 깎인다(친선 피해 기본 꺼짐) · 다른 팀은 깎인다 · 자기가 자기를 치면 깎인다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugSetTeam(a, 1);
        srv.DebugSetTeam(b, 1);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(b, -1, 6, a, 0, out);
        Check(srv.HealthOf(b) == kWizHP, "같은 팀인데 깎였다");
        srv.DebugSetTeam(b, 2);
        srv.DebugHurt(b, -1, 6, a, 0, out);
        Check(srv.HealthOf(b) < kWizHP, "다른 팀인데 안 깎였다");
        srv.DebugHurt(a, -1, 6, a, 0, out);
        Check(srv.HealthOf(a) < kWizHP, "자기가 자기를 쳤는데 안 깎였다(퀘스트 아님)");
    }
    // ④ 20 이상 맞으면 움찔(상태 30 · 자세 52) → RECOIL 칸이 끝나면 13 · 19 는 안 움찔
    {
        nox::Server srv;
        srv.SetPlayerAnimFrames(52, 8, 0);   // ⭕ thing.bin RECOIL 8/0
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);   // 체력 150 · 방어 0.02
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(b, -1, 19, a, 1, out);
        Check(srv.DebugStateOf(b) != 30, "19 에 움찔했다");
        srv.DebugHurt(b, -1, 20, a, 1, out);
        Check(srv.DebugStateOf(b) == 30, "20 을 맞았는데 움찔(상태 30) 이 아니다");
        Check(srv.DebugAnimByteOf(b) == 52, "움찔 자세가 52(RECOIL) 가 아니다");
        uint32_t ms = 0;
        srv.Tick(ms, out);
        int back = -1;
        for (int t = 1; t <= 12 && back < 0; ++t) { ms += 33; srv.Tick(ms, out); if (srv.DebugStateOf(b) == nox::kPlayerStateIdle) back = t; }
        Check(back >= 8 && back <= 9, "움찔이 RECOIL 8칸 뒤에 13 으로 안 돌아왔다");
    }
    // ⑤ 몬스터 — 불 면역은 종류 1 무시 · 종류 0 은 깎임 · 기계 골렘 피해 함수는 전기 두 배 · 내 소환수는 내가 쳐도 안 깎임
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀     // a 가 Bomber 를 부른다
        Check(srv.pets() == 1, "소환이 안 됐다 — 이 시험이 무의미해진다");
        nox::Server::PhysBody body;
        body.shape = 2; body.r = 10.0f; body.cls = 0x2;
        body.subclass = nox::Server::MonsterClassBitsOf("SMALL_MONSTER+IMMUNE_FIRE");
        body.damageFn = nox::Server::DamageFnOf("MechGolemDamage");
        srv.SetTypeBody(1328, body);   // 4-a — 임프
        srv.DebugSetOwnedPetHealth(a, 100);
        srv.DebugHurt("", 0, 10, b, 1, out);
        Check(srv.DebugPetHealth(0) == 100, "불 면역인데 종류 1 에 깎였다");
        srv.DebugHurt("", 0, 10, b, 0, out);
        Check(srv.DebugPetHealth(0) == 90, "종류 0 이 10 깎지 않았다(몬스터는 방어 없음)");
        srv.DebugHurt("", 0, 10, b, 9, out);
        Check(srv.DebugPetHealth(0) == 70, "기계 골렘 피해 함수가 전기를 두 배로 안 했다");
        srv.DebugHurt("", 0, 10, a, 0, out);
        Check(srv.DebugPetHealth(0) == 70, "주인이 자기 소환수를 쳤는데 깎였다(편 거르기)");
    }
    // ⑥ 무적이면 안 깎인다(버프 23)
    {
        nox::Server srv;
        srv.SetAutoRespawn(true);   // 3-c — 서버 설정 자동 부활
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.Damage(b, 9999, out, "");
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 120 && srv.HealthOf(b) == 0; ++t) { ms += 33; srv.Tick(ms, out); }
        Check(srv.IsInvulnerable(b), "부활 무적이 아니다 — 이 시험이 무의미해진다");
        const uint16_t hp = srv.HealthOf(b);
        srv.DebugHurt(b, -1, 30, a, 0, out);
        Check(srv.HealthOf(b) == hp, "무적인데 깎였다");
    }
    std::printf("  피해 함수 — 버림·이월·종류별 방어·최소 1·편 거르기·움찔 20·몬스터 면역·골렘 전기 두 배·무적\n");
}

void TestShieldBlock() {
    const std::string a = "10.0.0.1:1000";   // 워리어(나무 방패 0x1000000)
    const std::string b = "10.0.0.2:1000";
    const int kWarHP = nox::HealthOnline(nox::CharClass::Warrior);
    auto makeSrv = [&](nox::Server& srv) {
        srv.SetPlayerAnimFrames(40, 6, 1);    // ⭕ thing.bin RAISE_SHIELD 6/1
        srv.SetPlayerAnimFrames(48, 3, 1);    // GREAT_SWORD_BLOCK_* 3/1
        srv.SetPlayerAnimFrames(49, 3, 1);
        srv.SetPlayerAnimFrames(47, 3, 1);
        srv.SetPlayerAnimFrames(30, 6, 0);    // STAFF_BLOCK 6/0
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 5000, 5000);
    };
    auto move = [&](nox::Server& srv, std::vector<nox::OutPacket>& out) {
        std::vector<uint8_t> pkt = InputPacket(2);
        pkt[nox::kWireHeaderSize + 1] = 5;
        pkt.push_back(0);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
    };
    nox::Server::AttackSrc front;  front.x = 1030; front.y = 1000;   // a 가 바라보는 +x
    nox::Server::AttackSrc back;   back.x = 970;   back.y = 1000;

    // ① 가만히 30/4=7 틱을 넘기면 방패를 올린다(15) → RAISE_SHIELD 6칸×2틱 = 12틱 뒤 올린 채(16) · 자세 40
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        int up15 = -1, up16 = -1;
        for (int t = 1; t <= 30; ++t) {
            ms += 33; srv.Tick(ms, out);
            if (up15 < 0 && srv.DebugStateOf(a) == 15) up15 = t;
            if (up16 < 0 && srv.DebugStateOf(a) == 16) up16 = t;
        }
        Check(up15 == 8, "가만 8틱째(7 넘김) 에 방패 올리기(15) 가 아니다");
        Check(up16 == up15 + 12, "올리기 12틱 뒤 올린 채(16) 가 아니다");
        Check(srv.DebugAnimByteOf(a) == 40, "방패 자세가 40(RAISE_SHIELD) 이 아니다");
        Check(srv.DebugAnimFrameOf(a) == 5, "올린 채 칸이 마지막(5) 이 아니다");

        // ② 걸음 입력 — 먼저 내리기(17), 다음 걸음 틱에 걷기(0)
        move(srv, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == 17, "방패 올린 채 걸음 입력이 내리기(17) 로 안 갔다");
        move(srv, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == nox::kPlayerStateWalk, "내리는 중 걸음 입력이 걷기로 안 갔다");
    }

    // ③ 방패 올린 채(16) — 앞에서 온 칼은 막고 · 뒤에서 온 칼과 앞에서 온 전기(9)는 들어간다 · 올리기 전(13) 은 안 막음
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        makeSrv(srv);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(a, -1, 30, b, 0, out, &front);
        Check(srv.HealthOf(a) < kWarHP, "방패를 아직 안 올렸는데(13) 막았다");
        const uint16_t hp0 = srv.HealthOf(a);
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 40 && srv.DebugStateOf(a) != 16; ++t) { ms += 33; srv.Tick(ms, out); }
        Check(srv.DebugStateOf(a) == 16, "방패가 안 올라갔다 — 이 시험이 무의미해진다");
        const uint16_t hp1 = srv.HealthOf(a);
        Check(hp1 == hp0, "가만히 있는 사이 체력이 바뀌었다 — 이 시험이 무의미해진다");
        srv.DebugHurt(a, -1, 30, b, 0, out, &front);
        Check(srv.HealthOf(a) == hp1, "방패를 올렸는데 앞에서 온 칼에 깎였다");
        srv.DebugHurt(a, -1, 30, b, 0, out, &back);
        Check(srv.HealthOf(a) < hp1, "뒤에서 온 칼을 방패가 막았다");
        const uint16_t hp2 = srv.HealthOf(a);
        // 뒤에서 맞아 움찔(30) 했을 수 있다 — 다시 올릴 때까지 기다린다
        for (int t = 0; t < 60 && srv.DebugStateOf(a) != 16; ++t) { ms += 33; srv.Tick(ms, out); }
        srv.DebugHurt(a, -1, 30, b, 9, out, &front);
        Check(srv.HealthOf(a) < hp2, "방패가 앞에서 온 전기(9) 까지 막았다");
    }

    // ④ 대검(0x400) — 가만(13) 에 앞에서 온 칼(0) 을 막고 18~20 으로 · 망치류(2) 는 못 막음
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        makeSrv(srv);
        srv.DebugSetWeapon(a, 0x400);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(a, -1, 30, b, 0, out, &front);
        Check(srv.HealthOf(a) == kWarHP, "대검 든 가만 상태가 앞 칼을 못 막았다");
        const uint8_t st = srv.DebugStateOf(a);
        Check(st >= 18 && st <= 20, "대검 막기 뒤 상태가 18~20 이 아니다");
        nox::Server srv2; RegAllSpells(srv2);   // 4-a 주문 정의
        makeSrv(srv2);
        srv2.DebugSetWeapon(a, 0x400);
        srv2.DebugHurt(a, -1, 30, b, 2, out, &front);
        Check(srv2.HealthOf(a) < kWarHP, "대검이 종류 2(망치·철퇴) 를 막았다");
        // 걷는 중(0) 에는 대검도 못 막는다(가만 13 · 막기 18~20 만)
        nox::Server srv3; RegAllSpells(srv3);   // 4-a 주문 정의
        makeSrv(srv3);
        srv3.DebugSetWeapon(a, 0x400);
        uint32_t ms = 0;
        srv3.Tick(ms, out);
        {
            std::vector<uint8_t> pkt = InputPacket(2);
            pkt[nox::kWireHeaderSize + 1] = 5;
            pkt.push_back(0);
            srv3.OnPacket(a, pkt.data(), pkt.size(), out);
        }
        ms += 33; srv3.Tick(ms, out);
        Check(srv3.DebugStateOf(a) == nox::kPlayerStateWalk, "걷기 상태로 못 만들었다 — 이 시험이 무의미해진다");
        nox::Server::AttackSrc front2;
        front2.x = static_cast<float>(srv3.XOf(a) + 30); front2.y = static_cast<float>(srv3.YOf(a));
        srv3.DebugHurt(a, -1, 30, b, 0, out, &front2);
        Check(srv3.HealthOf(a) < kWarHP, "걷는 중인데 대검이 막았다");
    }

    // ⑤ 지팡이(0x8000) — 가만(13) 에 투사체 아닌 칼(0) 을 막고 21 · 투사체는 못 막음
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        makeSrv(srv);
        srv.DebugSetWeapon(a, 0x8000);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(a, -1, 30, b, 0, out, &front);
        Check(srv.HealthOf(a) == kWarHP && srv.DebugStateOf(a) == 21, "지팡이 가만 상태가 앞 칼을 막고 21 로 가지 않았다");
        nox::Server srv2; RegAllSpells(srv2);   // 4-a 주문 정의
        makeSrv(srv2);
        srv2.DebugSetWeapon(a, 0x8000);
        nox::Server::AttackSrc missile = front;
        missile.missile = true;
        srv2.DebugHurt(a, -1, 30, b, 0, out, &missile);
        Check(srv2.HealthOf(a) < kWarHP, "지팡이가 투사체를 막았다");
    }

    // ⑥ 들어오는 투사체가 있으면 7틱 안 기다리고 바로 방패를 올린다(monsterTestBlockShield)
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        makeSrv(srv);
        srv.DebugMoveTo(b, 1090, 1000);
        srv.DebugSetFacing(b, 128);            // b 가 −x(a 쪽) 을 본다
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        ms += 33; srv.Tick(ms, out);           // 한 틱 — 아직 7 안 넘김
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, b, 27, false, castMs, out); }   // 4-a 시전 틀
        Check(srv.flying() == 1, "파이어볼이 안 날았다 — 이 시험이 무의미해진다");
        ms += 33; srv.Tick(ms, out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) == 15 || srv.DebugStateOf(a) == 30, "앞에서 투사체가 오는데 7틱 전에 방패를 안 올렸다");
    }

    std::printf("  방패·막기 — 가만 7틱 뒤 올리기·12틱 올린 채·걸음이면 내리기·앞 막기·뒤·전기 통과·대검·지팡이·투사체 보고 올리기\n");
}

// ── 중요 메시지 대기열 (2026-09-13) ───────────────────────────
//
// 정답지 `nox_xxx_netImportant_4E5770`(GAME3_3.c:1938)·`nox_net_importantACK_4E55A0`(1832)를 옮긴 경로.
//   ① 준비(c0) 전 = 쌓이기만 한다 · ② 준비 뒤 틱 = `[27 시각][aa][cc…]` 한 조각
//   ③ `ab <그 시각>` = 그 조각에 실은 것을 지운다 · ④ 확인이 없으면 60틱 뒤 **같은 순번**으로 다시 싣는다.
// ⚠ 왜 — 한 번만 보내던 때, 준비 전에 나간 순번 0~4 를 클라가 버려 **뒤 cc 가 전부 막혔다**
//   (09-13 실클라: 친구 알림을 cc 로 보내면 빨강 · 맨 메시지로 보내면 파랑).
// `entries` — 실린 항목 수(cc 봉투 + 봉투 없는 것). 3-d 부터 방어값 `49` 가 봉투 없이 같은 조각에 실린다
static bool FindImportantPacket(const std::vector<nox::OutPacket>& out, const std::string& to,
                                uint16_t* frame, std::vector<uint16_t>* ids, size_t* entries = nullptr) {
    for (const auto& p : out) {
        if (p.to != to) continue;
        const std::vector<uint8_t> b = Body(p);
        if (b.size() < 8 || b[0] != static_cast<uint8_t>(nox::Op::TIMESTAMP) ||
            b[3] != static_cast<uint8_t>(nox::Op::IMPORTANT)) continue;
        if (frame) *frame = static_cast<uint16_t>(b[1] | (b[2] << 8));
        size_t q = 4;
        size_t n = 0;
        while (q < b.size()) {
            if (b[q] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT) && q + 4 <= b.size()) {
                if (ids) ids->push_back(static_cast<uint16_t>(b[q + 1] | (b[q + 2] << 8)));
                q += 4 + b[q + 3];
            } else {
                const int len = nox::MsgBodyLenByDir(b[q], /*fromClient=*/false);
                if (len < 0) break;
                q += 1 + static_cast<size_t>(len);
            }
            ++n;
        }
        if (entries) *entries = n;
        return true;
    }
    return false;
}

static std::vector<uint8_t> WireOf(std::initializer_list<uint8_t> msg) {
    std::vector<uint8_t> p(nox::kWireHeaderSize, 0);   // 비신뢰 · 스트림 0 · 순번 0
    p.insert(p.end(), msg);
    return p;
}

void TestImportantQueue() {
    nox::Server srv;
    const std::string a = "10.9.9.1:49001";
    srv.DebugEnterGame(a, 740);
    srv.DebugImportantReal(a, /*ready=*/false);
    uint32_t ms = 0;
    std::vector<nox::OutPacket> warm;
    srv.Tick(ms, warm);   // 첫 부름은 시계만 잡는다

    // ① 준비 전 — 쌓이기만, 틱이 돌아도 안 나간다
    {
        std::vector<nox::OutPacket> out;
        srv.Damage(a, 10, out);
        ms += 34; srv.Tick(ms, out);                          // 09-14 — 43 은 피해 자리가 아니라 다음 틱 PlayerReportAnything 이 싣는다
        const size_t pend = srv.DebugImportantPending(a);
        Check(pend > 0, "준비 전 중요 메시지가 대기열에 안 쌓임");
        for (int i = 0; i < 3; ++i) { ms += 34; srv.Tick(ms, out); }
        Check(!FindImportantPacket(out, a, nullptr, nullptr), "준비 전에 중요 조각(27 aa cc)이 나감");
        Check(srv.DebugImportantPending(a) >= pend, "준비 전에 대기열이 줄었다");
    }

    // ② 준비 — 다음 틱에 [27][aa][cc…], 순번은 0 부터 빈틈 없이
    uint16_t f1 = 0;
    std::vector<uint16_t> ids1;
    size_t carried1 = 0;
    {
        std::vector<nox::OutPacket> out;
        const auto ready = WireOf({ static_cast<uint8_t>(nox::Op::CLIENT_READY) });
        srv.OnPacket(a, ready.data(), ready.size(), out);
        ms += 34; srv.Tick(ms, out);
        Check(FindImportantPacket(out, a, &f1, &ids1, &carried1), "준비 뒤 틱에 중요 조각이 안 나감");
        Check(!ids1.empty() && ids1[0] == 0, "첫 중요 조각이 순번 0 부터가 아님");
        bool contiguous = true;
        for (size_t i = 1; i < ids1.size(); ++i) if (ids1[i] != ids1[i - 1] + 1) contiguous = false;
        Check(contiguous, "첫 중요 조각의 순번에 빈틈이 있음");
    }

    // ③ 확인 — 그 시각으로 `ab` 가 오면 실었던 것이 지워진다
    {
        const size_t before = srv.DebugImportantPending(a);
        std::vector<nox::OutPacket> out;
        const auto ack = WireOf({ static_cast<uint8_t>(nox::Op::IMPORTANT_ACK),
                                  static_cast<uint8_t>(f1), static_cast<uint8_t>(f1 >> 8), 0, 0 });
        srv.OnPacket(a, ack.data(), ack.size(), out);
        Check(srv.DebugImportantPending(a) + carried1 == before, "확인했는데 실은 만큼 안 지워짐");
    }

    // ④ 확인 없음 — 새로 실은 순번이 60틱 안에는 다시 안 나가고, 60틱 무렵 같은 순번으로 다시 나간다
    {
        std::vector<nox::OutPacket> out;
        srv.DebugBuffOff(a, 23);                               // 준비(CLIENT_READY)가 건 무적 23 을 끈다(gameServerReadyMB) — 안 끄면 피해가 막힌다
        srv.Damage(a, 5, out);
        ms += 34; srv.Tick(ms, out);
        std::vector<uint16_t> ids2;
        Check(FindImportantPacket(out, a, nullptr, &ids2), "새 중요 메시지가 안 실림");
        // 다시 싣기는 틱에 하나씩(정답지 카운터 1) — 같은 조각의 뒤 순번은 그만큼 밀리므로 **첫** 순번을 본다
        const uint16_t watch = ids2.empty() ? 0xFFFF : ids2.front();
        int resentAt = -1;
        for (int i = 1; i <= 70 && resentAt < 0; ++i) {
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            std::vector<uint16_t> ids;
            if (FindImportantPacket(o, a, nullptr, &ids))
                for (uint16_t id : ids) if (id == watch) resentAt = i;
        }
        Check(resentAt >= nox::kImportantResendTicks && resentAt <= nox::kImportantResendTicks + 2,
              "확인 없는 중요 메시지가 60틱 무렵에 다시 안 실림");
    }

    // ⑤ 사라짐(32)은 **봉투 없이** 중요 조각에 실린다 — 정답지 `netObjectOutOfSight_528A60` → Packet0
    {
        nox::Server s2;
        s2.SetAoi(true);
        std::vector<nox::Server::WorldStatic> st;
        st.push_back({ 0x4321, 1234, 1000, 1000 });
        s2.SetWorldStatics(st);
        const std::string b = "10.9.9.2:49002";
        s2.DebugEnterGame(b, 741);
        s2.DebugImportantReal(b, /*ready=*/true);
        s2.DebugMoveTo(b, 1000, 1000);
        std::vector<nox::OutPacket> o;
        uint32_t t = 0;
        s2.Tick(t, o);
        t += 34; s2.Tick(t, o);   // 소개
        s2.DebugMoveTo(b, 1000 + static_cast<int>(nox::kAoiDropRadius) + 300, 1000);
        bool rawInImportant = false;
        for (int i = 0; i < 3 && !rawInImportant; ++i) {
            std::vector<nox::OutPacket> oo;
            t += 34; s2.Tick(t, oo);
            for (const auto& p : oo) {
                if (p.to != b) continue;
                const std::vector<uint8_t> body = Body(p);
                if (body.size() < 7 || body[0] != static_cast<uint8_t>(nox::Op::TIMESTAMP) ||
                    body[3] != static_cast<uint8_t>(nox::Op::IMPORTANT)) continue;
                for (size_t q = 4; q + 3 <= body.size(); ++q)
                    if (body[q] == static_cast<uint8_t>(nox::Op::OBJECT_OUT_OF_SIGHT) &&
                        body[q + 1] == 0x21 && body[q + 2] == 0x43) rawInImportant = true;
            }
        }
        Check(rawInImportant, "사라짐(32)이 중요 조각에 맨 메시지로 안 실림");
    }
}

// ── 묶음 3-d — 죽음 알림 `a9 0e` · 방어값 `49` · 충돌 요청 `7b` ─────────────
//
// 죽음 알림(diePlayer_54D2B0 `GAME5.c:4971~5057`): <죽인 사람2><도운 사람2><죽은 사람2><종류2><갈래1> 모두에게.
//   갈래·종류는 피해 함수 기록(+304/+300 `GAME3_3.c:164~246 · 344~437`)과 +520(마지막으로 때린 것)으로 정한다.
// 방어값(playerReportAnything_4D9900): +228 ≠ +232 이면 본인에게 `49 <float>` · 녹음 위저드 값 0x3e6b8520.
// 충돌 요청(sdecode 0x7B): 관전 아니면 번호(윗비트면 extent)로 찾아 충돌 함수(+696) — 함수 자체는 5·9.
void TestKillArmorCollide() {
    const std::string a = "10.3.3.1:3001";
    const std::string b = "10.3.3.2:3002";
    // 죽음 알림 찾기 — 받는 사람별
    auto killMsg = [](const std::vector<nox::OutPacket>& out, const std::string& to) -> std::vector<uint8_t> {
        for (const auto& m : FlattenTo(out, to))
            if (m.size() >= 2 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 14) return m;
        return {};
    };
    auto u16 = [](const std::vector<uint8_t>& m, size_t at) { return static_cast<uint16_t>(m[at] | (m[at + 1] << 8)); };
    // 한 번 죽이고 알림을 돌려준다(b 는 워리어 150 — 한 대에 죽게 체력을 먼저 깎는다)
    auto killWith = [&](const std::string& by, uint8_t dtype, const nox::Server::AttackSrc* src,
                        std::vector<uint8_t>* toA, std::vector<uint8_t>* toB) {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        srv.Damage(b, 149, out, "");
        out.clear();
        srv.DebugHurt(b, -1, 50, by, dtype, out, src);
        Check(!srv.IsAlive(b), "죽음 알림 시험 — 한 대에 안 죽었다(시험이 무의미)");
        if (toA) *toA = killMsg(out, a);
        if (toB) *toB = killMsg(out, b);
    };

    // ① 무기로 죽임 — 갈래 1 · 무기 종류 · 죽인 사람 번호 · 모두에게 11바이트
    {
        nox::Server::AttackSrc src;
        src.x = 1000; src.y = 1000;
        src.obj = true;
        src.objType = nox::kItemLongsword;
        std::vector<uint8_t> ma, mb;
        killWith(a, 0, &src, &ma, &mb);
        Check(ma.size() == 11 && mb.size() == 11, "죽음 알림 `a9 0e` 가 모두에게 11바이트로 안 감");
        if (mb.size() == 11) {
            Check(u16(mb, 2) == 728, "죽음 알림의 죽인 사람 번호가 틀림");
            Check(u16(mb, 4) == 0, "도운 사람 자리가 0 이 아님(도운 이 기록 없음)");
            Check(u16(mb, 6) == 729, "죽음 알림의 죽은 사람 번호가 틀림");
            Check(u16(mb, 8) == nox::kItemLongsword && mb[10] == 1, "무기로 죽였는데 갈래 1·무기 종류가 아님");
        }
    }
    // ② 맨손(종류 10 · 무기 없음) — 갈래 1 · 때린 사람의 종류 713
    {
        nox::Server::AttackSrc src;
        src.x = 1000; src.y = 1000;
        std::vector<uint8_t> mb;
        killWith(a, 10, &src, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 8) == nox::kPlayerType && mb[10] == 1, "맨손으로 죽였는데 갈래 1·종류 713 이 아님");
    }
    // ③ 종류 0 · 무기 없음 — 갈래 2 · 피해 종류 0
    {
        nox::Server::AttackSrc src;
        src.x = 1000; src.y = 1000;
        std::vector<uint8_t> mb;
        killWith(a, 0, &src, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 8) == 0 && mb[10] == 2, "종류 0·무기 없음인데 갈래 2·종류 0 이 아님");
    }
    // ④ 맵 몬스터 — 죽인 사람 0 · 갈래 1 · 몬스터 종류 / 소환수 — 죽인 사람 = 주인
    {
        nox::Server::AttackSrc src;
        src.x = 1000; src.y = 1000;
        src.byMonsterType = 1377;
        std::vector<uint8_t> mb;
        killWith("", 0, &src, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 2) == 0 && u16(mb, 8) == 1377 && mb[10] == 1,
              "몬스터가 죽였는데 죽인 사람 0·갈래 1·몬스터 종류가 아님");
        killWith(a, 0, &src, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 2) == 728 && u16(mb, 8) == 1377 && mb[10] == 1,
              "소환수가 죽였는데 죽인 사람이 주인·갈래 1·몬스터 종류가 아님");
        // 몬스터가 쏜 투사체 — 주인 몬스터 종류
        src.obj = true;
        src.objType = 555;
        killWith("", 0, &src, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 8) == 1377 && mb[10] == 1, "몬스터 투사체로 죽었는데 주인 몬스터 종류가 아님");
    }
    // ⑤ 때린 것이 없음(종류 16) — 죽인 사람 0 · 갈래 2 · 16 / 종류 13 은 기록 안 함(갈래 0)
    {
        std::vector<uint8_t> mb;
        killWith("", 16, nullptr, nullptr, &mb);
        Check(mb.size() == 11 && u16(mb, 2) == 0 && u16(mb, 8) == 16 && mb[10] == 2, "때린 것 없이 종류 16 인데 갈래 2·16 이 아님");
        killWith("", 13, nullptr, nullptr, &mb);
        Check(mb.size() == 11 && mb[10] == 0, "종류 13 은 정답지 switch 밖인데 갈래를 적었다");
        // 기록은 피해마다 0 에서 다시 — 무기로 한 대(안 죽음) 뒤 종류 13 으로 죽으면 갈래 0
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        nox::Server::AttackSrc w;
        w.x = 1000; w.y = 1000; w.obj = true; w.objType = nox::kItemLongsword;
        srv.DebugHurt(b, -1, 10, a, 0, out, &w);
        srv.Damage(b, nox::HealthOnline(nox::CharClass::Warrior) - 20, out, "");
        out.clear();
        srv.DebugHurt(b, -1, 50, "", 13, out, nullptr);
        const auto m13 = killMsg(out, b);
        Check(m13.size() == 11 && m13[10] == 0, "앞 대의 갈래 1 기록이 다음 피해에서 안 비워졌다");
    }
    // ⑥ 실제 흐름 — 워리어가 롱소드로 휘둘러 죽이면 무기 그림 · 부활하면 +520 을 지운다
    {
        nox::Server srv;
        srv.SetPlayerAnimFrames(28, 8, 0);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1050, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, nox::HealthOnline(nox::CharClass::Wizard) - 1, out, "");
        for (int t = 0; t < 30; ++t) { ms += 33; srv.Tick(ms, out); }
        out.clear();
        auto click = InputPacket(6);
        srv.OnPacket(a, click.data(), click.size(), out);
        for (int t = 0; t < 8 && srv.IsAlive(b); ++t) { ms += 33; srv.Tick(ms, out); }
        Check(!srv.IsAlive(b), "롱소드 한 번에 체력 1 짜리가 안 죽었다");
        const auto mb = killMsg(out, b);
        Check(mb.size() == 11 && u16(mb, 2) == 728 && u16(mb, 8) == nox::kItemLongsword && mb[10] == 1,
              "롱소드로 죽였는데 알림에 롱소드 그림(갈래 1·1169)이 아님");
        // 부활 뒤 주인 없는 피해로 죽으면 죽인 사람이 0 이어야 한다(+520 = 0)
        for (int t = 0; t < 60; ++t) { ms += 33; srv.Tick(ms, out); }
        auto clickB = InputPacket(6);
        srv.OnPacket(b, clickB.data(), clickB.size(), out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.IsAlive(b), "클릭했는데 안 되살아남(시험 전제)");
        Check(srv.DebugLastHitCls(b) == 0, "부활했는데 +520(마지막으로 때린 것)을 안 지웠다");
        for (int t = 0; t < 5 * nox::kServerTickRate + 2; ++t) { ms += 33; srv.Tick(ms, out); }   // 부활 무적 5초
        out.clear();
        srv.Damage(b, 999, out);
        const auto mb2 = killMsg(out, b);
        Check(mb2.size() == 11 && u16(mb2, 2) == 0 && mb2[10] == 0, "부활 뒤에도 옛 죽인 사람·갈래가 남아 있다");
    }

    // ⑥-2 실제 흐름 — 소환수가 걸어가 때려 죽이면 죽인 사람 = 주인 · 갈래 1 · 소환수 종류
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1400, 1000);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
        Check(srv.pets() == 1, "소환수가 안 불려 나옴(시험 전제)");
        srv.Damage(b, nox::HealthOnline(nox::CharClass::Wizard) - 1, out, "");
        srv.Tick(0, out);
        uint32_t ms = 0;
        std::vector<uint8_t> mb;
        for (int t = 0; t < 400 && srv.IsAlive(b); ++t) {
            out.clear();
            ms += 33; srv.Tick(ms, out);
            if (!srv.IsAlive(b)) mb = killMsg(out, b);
        }
        Check(!srv.IsAlive(b), "소환수가 체력 1 짜리를 못 죽였다(시험 전제)");
        Check(mb.size() == 11 && u16(mb, 2) == 728 && mb[10] == 1 && u16(mb, 8) != 0 && u16(mb, 8) != nox::kPlayerType,
              "소환수가 죽였는데 알림이 주인·갈래 1·소환수 종류가 아님");
    }

    // ⑦ 방어값 — 녹음 값과 한 비트까지 · 첫 갱신에 본인에게만 한 번 · 다음 틱엔 안 보냄
    {
        const float wiz = nox::ArmorValueFloat(nox::CharClass::Wizard);
        uint32_t bits = 0;
        std::memcpy(&bits, &wiz, 4);
        Check(bits == 0x3e6b8520u, "위저드 방어값이 녹음 `20 85 6b 3e` 와 다르다(더하는 순서)");
        nox::Server srv;
        srv.DebugEnterGame(a, 728);                                   // 위저드
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        ms += 33; srv.Tick(ms, out);
        auto armorMsgs = [](const std::vector<std::vector<uint8_t>>& v) {
            std::vector<std::vector<uint8_t>> r;
            for (const auto& m : v) if (!m.empty() && m[0] == static_cast<uint8_t>(nox::Op::REPORT_ARMOR_VALUE)) r.push_back(m);
            return r;
        };
        const auto ta = armorMsgs(FlattenTo(out, a));
        const auto tb = armorMsgs(FlattenTo(out, b));
        Check(ta.size() == 1 && ta[0].size() == 5 && ta[0][1] == 0x20 && ta[0][2] == 0x85 && ta[0][3] == 0x6b && ta[0][4] == 0x3e,
              "위저드 본인에게 방어값 `49 20 85 6b 3e` 가 한 번 안 감");
        const float war = nox::ArmorValueFloat(nox::CharClass::Warrior);
        uint32_t wb = 0;
        std::memcpy(&wb, &war, 4);
        Check(tb.size() == 1 && tb[0].size() == 5 && (tb[0][1] | (tb[0][2] << 8) | (tb[0][3] << 16) | (static_cast<uint32_t>(tb[0][4]) << 24)) == wb,
              "워리어는 제 방어값을 받아야 하는데 아니다(남의 값이 샌다)");
        out.clear();
        ms += 33; srv.Tick(ms, out);
        Check(armorMsgs(Flatten(out)).empty(), "방어값이 안 바뀌었는데 다음 틱에 또 보냄");
        // 스냅샷 꼬리 — 직업 값이 실린다
        const auto tail = nox::BuildSnapshotTail(713, nox::CharClass::Warrior, 1, 1);
        bool tailHas = false;
        for (size_t i = 0; i + 4 < tail.size(); ++i)
            if (tail[i] == 0x49 && (tail[i + 1] | (tail[i + 2] << 8) | (tail[i + 3] << 16) | (static_cast<uint32_t>(tail[i + 4]) << 24)) == wb) tailHas = true;
        Check(tailHas, "워리어 스냅샷 꼬리의 방어값이 직업 값이 아니다");
    }

    // ⑧ 충돌 요청 — 번호 · extent(윗비트) · 없는 것 · 함수 없는 종류 · 관전
    {
        nox::Server srv;
        std::vector<nox::Server::MapObjRef> refs;
        nox::Server::MapObjRef r1; r1.netCode = 200; r1.extent = 77; r1.type = 900; refs.push_back(r1);
        nox::Server::MapObjRef r2; r2.netCode = 201; r2.extent = 78; r2.type = 901; refs.push_back(r2);
        srv.SetMapObjects(refs);
        srv.SetCollideFn(900, "ChestCollide");
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729, /*observer=*/true);
        std::vector<nox::OutPacket> out;
        auto send = [&](const std::string& who, uint16_t code) {
            const auto p = WireOf({ static_cast<uint8_t>(nox::Op::TRY_COLLIDE), static_cast<uint8_t>(code), static_cast<uint8_t>(code >> 8) });
            srv.OnPacket(who, p.data(), p.size(), out);
        };
        send(a, 200);
        Check(srv.tryCollides() == 1 && srv.tryCollideDeferred() == 1, "번호로 보낸 충돌 요청이 상자를 못 찾음");
        send(a, static_cast<uint16_t>(0x8000 | 77));
        Check(srv.tryCollides() == 2, "윗비트 선 번호를 extent 로 안 찾음");
        send(a, 200 | 0x8000);
        Check(srv.tryCollides() == 2, "윗비트 선 번호를 번호로 찾았다(extent 200 은 없다)");
        send(a, 999);
        Check(srv.tryCollides() == 2, "없는 번호인데 찾았다고 셈");
        send(a, 201);
        Check(srv.tryCollides() == 3 && srv.tryCollideDeferred() == 2, "충돌 함수 없는 종류를 부를 것으로 셈");
        send(b, 200);
        Check(srv.tryCollides() == 3, "관전 중인데 충돌 요청을 받았다(3680 & 3)");
        send(a, 729);
        Check(srv.tryCollides() == 4, "사람 번호로 보낸 충돌 요청을 못 찾음");
    }
    std::printf("  3-d — 죽음 알림 갈래·종류·죽인 사람 · 방어값 녹음 일치·한 번 · 충돌 요청 찾기\n");
}

// ── 묶음 4-a — 시전 틀 ─────────────────────────────────────────────
//
// 정답지: 받기 sdecode:211 → spellByBookInsert_4FE340(상태 2) → 매 틱 spellCastByBook_4FCB80(음소를 3틱마다 하나)
//   → 음소가 끝나면 PlayerSpell(player.go:462 — 직업·마나·inform 1·STAT 15) → castSpell → spellAccept
void TestCastPipeline() {
    const std::string a = "10.4.4.1:4001";
    const std::string b = "10.4.4.2:4002";
    auto count = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t op) {
        size_t n = 0;
        for (const auto& m : FlattenTo(out, to)) if (!m.empty() && m[0] == op) ++n;
        return n;
    };
    auto hasStat = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t spell, uint8_t stat) {
        for (const auto& m : FlattenTo(out, to))
            if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && m[1] == spell && m[5] == stat) return true;
        return false;
    };
    auto send = [](nox::Server& srv, const std::string& who, uint8_t spell, bool self, std::vector<nox::OutPacket>& out) {
        const auto pkt = SpellPacket(spell, self);
        srv.OnPacket(who, pkt.data(), pkt.size(), out);
    };

    // ① 파이어볼(음소 3) — 받은 다음 틱에 `70` · 상태 2 · 자세 21 · 음소 3 × 3틱 + 1 = 10틱째에 끝 · 마나 · inform 1 · STAT 15 · 상태 13
    {
        nox::Server srv;
        RegSpell(srv, 27);
        srv.SetPlayerAnimFrames(21, 8, 2);                    // ⭕ thing.bin CAST_SPELL 8/2
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        const uint16_t full = srv.ManaOf(a);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        send(srv, a, 27, false, out);
        int endTick = -1;
        std::vector<nox::OutPacket> endOut;
        for (int t = 1; t <= 20 && endTick < 0; ++t) {
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            if (t == 1) {
                Check(count(o, a, static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) == 1, "받은 다음 틱에 `70` 이 없다");
                Check(srv.DebugStateOf(a) == 2 && srv.DebugAnimByteOf(a) == 21, "시전 중인데 상태 2 · 자세 21(CAST_SPELL) 이 아니다");
            }
            if (t == 7) Check(srv.DebugAnimFrameOf(a) == 2, "시전 자세 칸이 (프레임−시작)÷3 이 아니다");
            if (t > 1 && srv.DebugCasting(a)) Check(count(o, a, static_cast<uint8_t>(nox::Op::REPORT_SPELL_START)) == 0, "`70` 이 첫 음소 뒤에도 나간다");
            if (srv.DebugCasting(a)) Check(srv.ManaOf(a) == full, "음소를 다 말하기 전에 마나가 깎였다");
            if (!srv.DebugCasting(a)) { endTick = t; endOut = o; }
        }
        Check(endTick == 10, "파이어볼(음소 3) 이 10틱째에 안 끝났다 — 음소 3틱 간격·끝 판정");
        Check(srv.ManaOf(a) == full - 30, "끝났는데 마나 30 이 안 깎였다");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "끝났는데 상태 13 으로 안 돌아왔다");
        Check(hasStat(endOut, a, 27, 15), "끝났는데 `df 1b 00 00 00 0f` 가 없다");
    }

    // ② 시전 중이면 새 시전을 안 받는다 — 주문 하나면 STAT 0 · 목록은 하나 그대로
    {
        nox::Server srv;
        RegSpell(srv, 27);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        send(srv, a, 27, false, out);
        ms += 34; srv.Tick(ms, out);
        out.clear();
        send(srv, a, 27, false, out);
        Check(srv.CastPending() == 1, "시전 중인데 두 번째가 목록에 들어갔다(+216)");
        Check(hasStat(out, a, 27, 0), "시전 중이라 못 넣었는데 STAT 0 이 없다");
    }

    // ③ 관전이면 글 열쇠만 보내고 안 넣는다(STAT 도 없다 — v34 = 0 이면 그 갈래를 안 탄다)
    {
        nox::Server srv;
        RegSpell(srv, 27);
        srv.DebugEnterGame(a, 728, /*observer=*/true, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        send(srv, a, 27, false, out);
        Check(srv.CastPending() == 0, "관전인데 시전 목록에 들어갔다");
        bool key = false;
        const std::string want = "GeneralPrint:NoSpellWarningGeneral";
        for (const auto& m : FlattenTo(out, a))
            if (m.size() == 3 + want.size() + 1 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 15 &&
                std::string(m.begin() + 3, m.end() - 1) == want) key = true;
        Check(key, "관전인데 `a9 0f 00 GeneralPrint:NoSpellWarningGeneral` 이 없다");
        Check(!hasStat(out, a, 27, 0), "관전 갈래에서 STAT 을 보냈다");
    }

    // ④ 공격 주문 하나 · 커서가 친구(내 소환수) 위 → 넣지도 않고 STAT 도 없다(sdecode:239)
    {
        nox::Server srv;
        RegAllSpells(srv);
        nox::Server::PhysBody body;
        body.shape = 2; body.r = 10.0f; body.cls = 0x2;
        srv.SetTypeBody(1328, body);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);
        srv.DebugSetClass(a, nox::CharClass::Wizard);
        double px = 0, py = 0;
        Check(srv.DebugPetXY(0, px, py), "임프가 안 불려 나옴(시험 전제)");
        srv.DebugAimAt(a, static_cast<int>(px), static_cast<int>(py));
        out.clear();
        send(srv, a, 27, false, out);
        Check(srv.CastPending() == 0, "커서가 내 소환수 위인데 공격 주문을 넣었다");
        Check(!hasStat(out, a, 27, 0), "친구 거르기 갈래에서 STAT 을 보냈다");
        // 커서를 치우면 넣는다
        srv.DebugAimAt(a, static_cast<int>(px) + 400, static_cast<int>(py) + 400);
        send(srv, a, 27, false, out);
        Check(srv.CastPending() == 1, "커서가 빈 곳인데 공격 주문을 안 넣었다");
    }

    // ⑤ 음소 나무 — 앞이 겹치는 두 주문. 에너지볼트(7531)는 4음소 = 13틱 · 연쇄번개(7531357)는 7음소 = 22틱에 끝
    for (uint8_t sp : { static_cast<uint8_t>(24), static_cast<uint8_t>(43) }) {
        nox::Server srv;
        RegSpell(srv, 24);
        RegSpell(srv, 43);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        send(srv, a, sp, false, out);
        int endTick = -1;
        std::vector<nox::OutPacket> endOut;
        for (int t = 1; t <= 40 && endTick < 0; ++t) {
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            if (!srv.DebugCasting(a)) { endTick = t; endOut = o; }
        }
        const int want = (sp == 24) ? 13 : 22;
        Check(endTick == want, sp == 24 ? "에너지볼트가 13틱째에 안 끝났다(겹치는 나무)" : "연쇄번개가 22틱째에 안 끝났다(겹치는 나무 — 에너지볼트 자리에서 멈췄나)");
        bool inform = false;
        for (const auto& m : FlattenTo(endOut, a))
            if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 1 && m[2] == sp) inform = true;
        Check(inform, "끝났는데 `a9 01 <주문>` 이 그 주문이 아니다");
    }

    // ⑥ 소환 비용은 thing.bin 이 아니라 blob_587000 217668 표 — 파일에서 읽어 대조(임프 87) · 워리어는 코드 9
    {
        FILE* f = std::fopen("C:\\Users\\USER\\Desktop\\MyGit\\Nox_Server\\third_party\\opennox-src\\src\\common\\memmap\\nox\\blobdata\\blob_587000.dat", "rb");
        int blobCost = -1;
        if (f) {
            if (std::fseek(f, 217668 + 4 * 87, SEEK_SET) == 0) {
                uint8_t v[4] = {};
                if (std::fread(v, 1, 4, f) == 4) blobCost = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
            }
            std::fclose(f);
        }
        if (blobCost < 0) {
            std::printf("  소환 비용 blob 대조 — 건너뜀(blob 없음)\n");
        } else {
            nox::Server srv;
            RegSpell(srv, 87);
            srv.DebugEnterGame(a, 728, false, nox::CharClass::Conjurer);
            srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
            srv.DebugMoveTo(b, 3000, 3000);
            const uint16_t full = srv.ManaOf(a);
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            CastAndWait(srv, a, 87, false, ms, out);
            Check(srv.ManaOf(a) == full - blobCost, "임프 소환 마나가 blob_587000 217668 표 값과 다르다");
            std::vector<nox::OutPacket> o2;
            uint32_t m2 = srv.DebugNowMs();
            srv.SetFriends(false);
            CastAndWait(srv, b, 87, false, m2, o2);
            bool code9 = false;
            for (const auto& m : FlattenTo(o2, b))
                if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 0 && m[2] == 9) code9 = true;
            Check(code9, "워리어가 소환을 넣었는데 코드 9 가 아니다");
            std::printf("  소환 비용 — 임프 %d (blob 대조)\n", blobCost);
        }
    }

    // ⑦ 걸으면 상태 2 는 풀리지만 시전은 계속된다(ImplB 는 상태 2 를 안 막는다 · PlayerSpell 은 상태 2 일 때만 13 으로)
    {
        nox::Server srv;
        RegSpell(srv, 27);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        send(srv, a, 27, false, out);
        ms += 34; srv.Tick(ms, out);
        std::vector<uint8_t> mv = InputPacket(2);
        mv[nox::kWireHeaderSize + 1] = 5;
        mv.push_back(0);
        srv.OnPacket(a, mv.data(), mv.size(), out);
        ms += 34; srv.Tick(ms, out);
        Check(srv.DebugStateOf(a) != 2, "걸었는데 상태 2 가 그대로다");
        Check(srv.DebugCasting(a), "걸었다고 시전이 취소됐다");
        for (int t = 0; t < 20 && srv.DebugCasting(a); ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.ManaOf(a) == nox::ManaOnline(nox::CharClass::Wizard) - 30, "걸으면서 말한 파이어볼이 끝나지 않았다");
    }
    std::printf("  4-a — 음소 3틱·10/13/22틱에 끝 · 시전 중 거절 · 관전 글 · 친구 커서 거르기 · 소환 blob 비용 · 걸어도 계속\n");
}

// ── 묶음 4-a2 — 겨눈 주문 날리기(Magic) ─────────────────────────────
//
// 정답지: castSpell 이 TARGETED 주문을 createSpellFly_4FDDA0 로 "Magic" 을 날리고, 투사체 목록이 틱마다
//   SpellProjectileUpdate(53B940 — 대상 쪽 힘 · 수명) → 움직임(51B810) → 선 추적 맞힘(537850) → SpellProjectileCollide(4E9500)
void TestSpellFly() {
    const std::string a = "10.4.5.1:4501";
    const std::string b = "10.4.5.2:4502";
    const uint16_t kMagic = 1237;
    auto hasOp = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t op) {
        for (const auto& m : FlattenTo(out, to)) if (!m.empty() && m[0] == op) return true;
        return false;
    };
    auto setup = [&](nox::Server& srv) {
        RegSpell(srv, 12);                                    // 혼란(TARGETED)
        srv.SetMagicType(kMagic, 50, 10.0f, 0x4000);          // ⭕ thing.bin Magic SPEED 50 · MASS 10 · FLAGS AIRBORNE
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
    };

    // ① 커서가 b 위 → 대상 b 로 날아가 닿으면 받아들이기(효과는 4-b) · 지움 알림 `31` · 갱신에 Magic 이 실린다
    //   첫 틱 자리 = 1014 + (3.125·14/14.1 + 3.125)·0.9 = 1019.62(감쇠 0.9) · 10틱째에 b(1150) 몸에 닿는다
    {
        nox::Server srv;
        setup(srv);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugAimAt(a, 1150, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "혼란을 겨눠 말했는데 Magic 이 안 생겼다");
        float mx = 0, my = 0;
        srv.DebugMissileXY(0, mx, my);
        Check(std::fabs(mx - 1014.0f) < 0.01f && std::fabs(my - 1000.0f) < 0.01f, "Magic 이 몸 앞(반지름+4)에서 안 생겼다");
        const unsigned long long before = srv.DebugAcceptCount();
        bool seenEntry = false, destroyed = false;
        int hitTick = -1;
        float firstX = 0;
        for (int t = 1; t <= 60 && srv.MissileCount() > 0; ++t) {
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            if (t == 1) { float y1 = 0; srv.DebugMissileXY(0, firstX, y1); }
            for (const auto& p : o) {
                auto body = Body(p);
                if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
                nox::UpdateMsg um = nox::DecodeUpdate(body.data() + 1, body.size() - 1, nullptr);
                for (const auto& e : um.objects) if (e.type == kMagic) seenEntry = true;
            }
            if (srv.MissileCount() == 0) { hitTick = t; destroyed = hasOp(o, a, static_cast<uint8_t>(nox::Op::DESTROY_OBJECT)) && hasOp(o, b, static_cast<uint8_t>(nox::Op::DESTROY_OBJECT)); }
        }
        Check(seenEntry, "날아가는 Magic 이 갱신 항목에 안 실렸다");
        Check(std::fabs(firstX - 1019.623f) < 0.01f, "첫 틱 자리가 (힘+속도)×감쇠 0.9 가 아니다");
        Check(hitTick == 10, "Magic 이 10틱째에 b 에 안 닿았다(힘·감쇠·속도가 틀림)");
        Check(srv.DebugAcceptCount() == before + 1, "b 에 닿았는데 받아들이기(spellAccept)가 안 불렸다");
        Check(srv.DebugLastAcceptTarget().addr == b, "받아들인 대상이 b 가 아니다");
        Check(destroyed, "닿아 지웠는데 `31 <번호>` 가 모두에게 안 갔다");
    }

    // ② 대상 없음 — 둘레 600 에 아무도 없으면 앞으로 날다가 수명 15틱 넘어 FX_RICOCHET(0x96) · 지움 · 받아들이기 없음
    {
        nox::Server srv;
        setup(srv);
        srv.DebugMoveTo(b, 5000, 5000);
        srv.DebugAimAt(a, 1300, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "대상 없이도 Magic 은 생긴다");
        const unsigned long long before = srv.DebugAcceptCount();
        int goneTick = -1;
        bool fx = false;
        float lastX = 0, lastY = 0;
        for (int t = 1; t <= 40 && srv.MissileCount() > 0; ++t) {
            srv.DebugMissileXY(0, lastX, lastY);
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            if (srv.MissileCount() == 0) { goneTick = t; fx = hasOp(o, a, static_cast<uint8_t>(nox::Op::FX_RICOCHET)); }
        }
        Check(goneTick == 16, "대상 없는 Magic 이 수명 15틱을 넘긴 틱(만든 틱 +16)에 안 사라졌다");
        Check(fx, "수명이 다해 지웠는데 FX_RICOCHET(0x96) 가 없다");
        Check(lastX > 1014.0f + 10.0f, "대상 없는 Magic 이 앞으로 안 날았다");
        Check(srv.DebugAcceptCount() == before, "대상이 없는데 받아들이기가 불렸다");
    }

    // ③ 방패 올린(16) 워리어가 앞에서 받으면 되받아쳐 주인과 대상이 바뀐다 → 시전자 a 에게 돌아와 닿는다
    {
        nox::Server srv;
        setup(srv);
        srv.SetPlayerAnimFrames(40, 6, 1);                    // RAISE_SHIELD
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);                           // a 쪽(−x)
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 30; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.DebugStateOf(b) == 16, "워리어가 방패를 안 올렸다(시험 전제)");
        srv.DebugAimAt(a, 1150, 1000);
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        const unsigned long long before = srv.DebugAcceptCount();
        float farX = 0;
        for (int t = 1; t <= 90 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            if (srv.DebugMissileXY(0, x, y) && x > farX) farX = x;
            ms += 34; srv.Tick(ms, out);
        }
        Check(farX < 1150.0f && farX > 1100.0f, "Magic 이 방패 앞에서 안 튕겼다");
        Check(srv.MissileCount() == 0 && srv.DebugAcceptCount() == before + 1, "되받아친 Magic 이 시전자에게 돌아와 받아들여지지 않았다");
        Check(srv.DebugLastAcceptTarget().addr == a, "되받아친 Magic 을 받아들인 대상이 시전자 a 가 아니다(주인·대상 맞바꿈)");
        Check(srv.DebugStateOf(b) == 16, "되받아친 워리어 상태가 바뀌었다");
    }
    // ③b 되받아친 주인(b)이 게임에서 빠지면 ud[0] 이 비고, 시전자에게 닿아도 받아들이지 않는다(spellAccept obj3 == nil → false)
    {
        nox::Server srv;
        setup(srv);
        srv.SetPlayerAnimFrames(40, 6, 1);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 30; ++t) { ms += 34; srv.Tick(ms, out); }
        srv.DebugAimAt(a, 1150, 1000);
        CastAndWait(srv, a, 12, false, ms, out);
        const unsigned long long before = srv.DebugAcceptCount();
        float lastX = 0;
        bool left = false;
        for (int t = 1; t <= 90 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            srv.DebugMissileXY(0, x, y);
            if (!left && lastX > 0 && x < lastX) { srv.DebugLeave(b); left = true; }
            lastX = x;
            ms += 34; srv.Tick(ms, out);
        }
        Check(left, "되받아침이 안 일어났다(시험 전제)");
        Check(srv.MissileCount() == 0 && srv.DebugAcceptCount() == before, "주인이 빠진 Magic 을 받아들였다(obj3 없음)");
    }
    // ④ 벽 — 선 추적이 막히면 새 자리를 되돌리고 벽 법선으로 속도 성분을 맞바꾼다(collideReflect_57B810)
    {
        nox::Server srv;
        setup(srv);
        srv.DebugMoveTo(b, 5000, 5000);
        srv.SetWalls({ { 47, 43 } });                          // 앞 +x 쪽 칸(X 자 벽)
        srv.DebugAimAt(a, 1300, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        float maxX = 0, lastY = 0;
        for (int t = 1; t <= 14 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            srv.DebugMissileXY(0, x, y);
            if (x > maxX) maxX = x;
            lastY = y;
            ms += 34; srv.Tick(ms, out);
        }
        // 6틱째 1076.44 → 다음 자리 1092.43 이 칸(47,43) X 벽에 막혀 새 자리를 되돌린다
        Check(maxX < 1080.0f, "Magic 이 벽에 막혔는데 새 자리를 안 되돌렸다(벽 안쪽까지 들어감)");
        Check(std::fabs(lastY - 1000.0f) > 5.0f, "Magic 이 벽에서 옆으로 안 튕겼다(속도 성분 맞바꿈)");
    }
    const std::string c = "10.4.5.3:4503";

    // ⑤ 대상(b)이 아닌 사람(c)이 길목에 있으면 받아들이지 않고 튕긴다(`*(v5+4) == v3` 아니면 projectileReflect)
    //   + 깃발 거르기(sub_54E730) — 날아가는 것에 NO_COLLIDE_OWNER(0x400)가 있으면 같은 편 c 는 지나간다
    for (int k = 0; k < 2; ++k) {
        nox::Server srv;
        setup(srv);
        if (k == 1) {
            srv.SetMagicType(kMagic, 50, 10.0f, 0x4400);      // 시험용 — AIRBORNE + NO_COLLIDE_OWNER
            srv.DebugSetTeam(a, 1);
        }
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Warrior);
        if (k == 1) srv.DebugSetTeam(c, 1);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugMoveTo(c, 1080, 1000);
        srv.DebugAimAt(a, 1150, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        float maxX = 0;
        for (int t = 1; t <= 60 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            if (srv.DebugMissileXY(0, x, y) && x > maxX) maxX = x;
            ms += 34; srv.Tick(ms, out);
        }
        if (k == 0) {
            Check(maxX < 1080.0f, "길목의 c 를 지나갔다(맞힘 없음)");
            Check(srv.DebugLastAcceptTarget().addr != c, "대상이 아닌 c 에 닿았는데 받아들였다");
        } else {
            Check(maxX > 1100.0f && srv.DebugLastAcceptTarget().addr == b, "NO_COLLIDE_OWNER 인데 같은 편 c 에 막혔다");
        }
    }

    // ⑥ 선 추적 조각 — 한 틱에 19 를 가는 사이(1127.88 → 1147.12) 가운데 반지름 4 장애물은 끝점만 봐선 안 걸린다
    //   조각 수 = floor(√(d²/36))+1 → 조각 ≤ 6 이라 반드시 걸려 튕긴다
    //   + 깃발 — SHORT(0x10) 장애물은 AIRBORNE Magic 이 넘어간다 · MISSILE_HIT(0x80) 이 붙으면 늘 맞는다 · NO_COLLIDE(0x40) 는 안 맞는다
    for (uint32_t obFlags : { 0u, 0x10u, 0x90u, 0x40u }) {
        nox::Server srv;
        setup(srv);
        srv.DebugMoveTo(b, 5000, 5000);
        nox::Server::PhysBody ob;
        ob.shape = 2; ob.r = 4.0f; ob.x = 1137.5f; ob.y = 1000.0f;
        ob.flags = obFlags;
        srv.SetObstacles({ ob });
        srv.DebugAimAt(a, 1300, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        float maxX = 0;
        for (int t = 1; t <= 14 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            if (srv.DebugMissileXY(0, x, y) && x > maxX) maxX = x;
            ms += 34; srv.Tick(ms, out);
        }
        Check(maxX > 1120.0f, "장애물 앞까지 안 날았다(시험 전제)");
        if (obFlags == 0x10u) Check(maxX > 1147.0f, "SHORT 장애물을 AIRBORNE Magic 이 못 넘었다(깃발 거르기 없음)");
        else if (obFlags == 0x40u) Check(maxX > 1147.0f, "NO_COLLIDE 장애물에 Magic 이 맞았다");
        else Check(maxX < 1140.0f, obFlags == 0 ? "틱 사이 가운데 장애물을 뚫고 지나갔다(선 추적 조각 없음)"
                                                : "MISSILE_HIT 장애물을 지나갔다(늘 맞음 없음)");
    }

    // ⑦ 만들 때 찾기는 시전자 자신을 뺀다(a5 = 0 · msl = 시전자) — 커서가 제 발밑이어도 대상은 b
    {
        nox::Server srv;
        setup(srv);
        RegSpell(srv, 41);                                    // 작은 치유(TARGETED · 공격 아님)
        srv.DebugMoveTo(b, 1300, 1000);
        srv.DebugAimAt(a, 1000, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 41, false, ms, out);
        Check(srv.MissileCount() == 1, "치유를 겨눠 말했는데 Magic 이 안 생겼다");
        for (int t = 1; t <= 60 && srv.MissileCount() > 0; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.DebugLastAcceptTarget().addr == b, "커서가 제 발밑일 때 대상이 b 가 아니다(시전자 자신을 안 뺐다)");
    }

    // ⑧ 대검 든 가만(13) 앞에서 받으면 되받아쳐 주인·대상이 바뀌고 18~20 → 시전자 a 에게 돌아와 닿는다
    {
        nox::Server srv;
        setup(srv);
        for (int anim : { 47, 48, 49 }) srv.SetPlayerAnimFrames(anim, 6, 1);   // GREAT_SWORD_BLOCK_*
        srv.DebugSetClass(b, nox::CharClass::Conjurer);       // 방패가 없어야 가만(13)에 머문다
        srv.DebugSetWeapon(b, 0x400);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);
        srv.DebugAimAt(a, 1150, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        Check(srv.DebugStateOf(b) == nox::kPlayerStateIdle, "b 가 가만(13)이 아니다(시험 전제)");
        bool blocked = false;
        for (int t = 1; t <= 90 && srv.MissileCount() > 0; ++t) {
            ms += 34; srv.Tick(ms, out);
            const uint8_t st = srv.DebugStateOf(b);
            if (st >= 18 && st <= 20) blocked = true;
        }
        Check(blocked, "대검 가만 상태가 앞에서 온 Magic 에 18~20 막기를 안 했다");
        Check(srv.DebugLastAcceptTarget().addr == a, "대검이 되받아친 Magic 을 받아들인 대상이 시전자 a 가 아니다");
    }

    // ⑨ 날아오는 Magic 도 「들어오는 투사체」다 — 방패 든 워리어가 가만 7틱을 안 기다리고 올린다(getMissilesInCircle)
    //   b 는 틱 앞머리에 지난 틱 자리를 본다 → 5틱째 1061.8(88 거리)을 6틱째에 보고 15
    {
        nox::Server srv;
        setup(srv);
        srv.SetPlayerAnimFrames(40, 6, 1);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);
        srv.DebugAimAt(a, 1150, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "Magic 이 안 생겼다(시험 전제)");
        srv.DebugSetState(b, nox::kPlayerStateIdle);          // 가만 시계를 0 으로
        int raiseTick = -1;
        for (int t = 1; t <= 10 && raiseTick < 0; ++t) {
            ms += 34; srv.Tick(ms, out);
            if (srv.DebugStateOf(b) == 15) raiseTick = t;
        }
        Check(raiseTick > 0 && raiseTick <= 7, "Magic 이 날아오는데 가만 7틱을 기다려 방패를 올렸다");
    }
    std::printf("  4-a2 — Magic 몸 앞에 생김 · 첫 틱·10틱 닿음 · 지움 31 · 대상 없음 수명 15 · 방패·대검 되받아침 · 벽 튕김 · 길목 튕김 · 조각 추적 · 자신 빼기 · 들어오는 Magic\n");
}

// ── 묶음 4-a3 — 지속 주문 틀 ─────────────────────────────────────────
//
// 정답지: spellsDuration.New(spells_dur.go:88) · spellCastByPlayer(:62) · destroyDurSpell(:39) · server/spells_dur.go CancelFor·CancelSpell
//   · GAME4.c 4FEAE0(주인 것 전부) · 4FEE90(붙들고 있는 주문) · 4FF2D0(대상으로 찾기) · server/spells.go:458 NetStopRaySpell
//   효과 함수(create·update·destroy)는 4-b~4-f — 여기서는 시험용 함수로 틀만 잰다
void TestDurSpells() {
    const std::string a = "10.4.6.1:4601";
    const std::string b = "10.4.6.2:4602";
    auto hasStat = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t spell, uint8_t stat) {
        for (const auto& m : FlattenTo(out, to))
            if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && m[1] == spell && m[5] == stat) return true;
        return false;
    };
    auto rayStop = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t typ, uint8_t val, uint16_t n1, uint16_t n2) {
        for (const auto& m : FlattenTo(out, to))
            if (m.size() == 7 && m[0] == static_cast<uint8_t>(nox::Op::FX_DURATION_SPELL) && m[1] == typ && m[2] == val &&
                (m[3] | (m[4] << 8)) == n1 && (m[5] | (m[6] << 8)) == n2) return true;
        return false;
    };
    auto anyRay = [](const std::vector<nox::OutPacket>& out, const std::string& to) {
        for (const auto& m : FlattenTo(out, to)) if (!m.empty() && m[0] == static_cast<uint8_t>(nox::Op::FX_DURATION_SPELL)) return true;
        return false;
    };
    auto setup = [&](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& out) {
        RegAllSpells(srv);
        for (uint8_t id : { 4, 7, 9, 22, 24, 31, 35, 43, 51, 59, 67, 75 }) srv.SetSpellDef(id, 10, 0x2, {});   // 시험용 정의(깃발만)
        srv.SetSpellDef(24, 10, 0x20 | 0x4, {});              // 번개는 공격 주문으로(CancelOffensiveFor)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.DebugMoveTo(b, 1300, 1000);
        srv.Tick(ms, out);
    };
    const nox::Target none;

    // ① dt 5 — 5틱째에 취소(STAT 15) · 다음 틱에 지움 · 주인 상태 13 · dt 0 이면 끝이 없다
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        Check(srv.DebugDurNew(a, 4, 5, -1, -1, true, none, out), "지속 주문 New 가 false");
        Check(srv.DurCount() == 1, "New 뒤 목록이 1 이 아니다");
        int statTick = -1, goneTick = -1;
        for (int t = 1; t <= 10 && goneTick < 0; ++t) {
            std::vector<nox::OutPacket> o;
            ms += 34; srv.Tick(ms, o);
            if (statTick < 0 && hasStat(o, a, 4, 15)) statTick = t;
            if (srv.DurCount() == 0) goneTick = t;
        }
        Check(statTick == 5, "dt 5 인데 5틱째에 STAT 15 로 취소되지 않았다");
        Check(goneTick == 6, "취소 다음 틱에 지워지지 않았다");
        Check(srv.DebugDurDestroys() == 1, "지울 때 destroy 함수가 한 번 불리지 않았다");
        Check(srv.DebugStateOf(a) == nox::kPlayerStateIdle, "지운 뒤 주인 상태가 13 이 아니다");
        Check(srv.DebugDurNew(a, 4, 0, -1, -1, false, none, out), "New(dt 0) 가 false");
        for (int t = 0; t < 100; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.DurCount() == 1, "dt 0 인데 끝났다(68 == 60 이면 끝 없음)");
    }

    // ② 같은 주문을 또 걸면 앞의 것을 취소(STAT 15)하고 곧바로 지운다(CancelFor → onNewSpell) · 소환끼리도 · 플라스마·연쇄 번개는 안 겹침
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        srv.DebugDurNew(a, 4, 0, -1, -1, false, none, out);
        std::vector<nox::OutPacket> o;
        srv.DebugDurNew(a, 4, 0, -1, -1, false, none, o);
        Check(srv.DurCount() == 1 && hasStat(o, a, 4, 15), "같은 주문을 다시 걸었는데 앞의 것이 취소·지워지지 않았다");
        srv.DebugDurNew(a, 75, 0, -1, -1, false, none, out);
        std::vector<nox::OutPacket> o2;
        srv.DebugDurNew(a, 87, 0, -1, -1, false, none, o2);
        Check(srv.DurCount() == 2 && srv.DebugDurSpellAt(0) == 87 && hasStat(o2, a, 75, 15), "소환 둘 — 앞 소환(75)이 취소되지 않았다");
        srv.DebugDurNew(b, 87, 0, -1, -1, false, none, out);
        Check(srv.DurCount() == 3, "남(b)의 소환이 a 의 소환을 취소했다(주인이 같아야)");
        std::vector<nox::OutPacket> o3;
        srv.DebugDurNew(a, 59, 0, -1, -1, false, none, o3);
        std::vector<nox::OutPacket> o4;
        Check(srv.DebugDurNew(a, 59, 0, -1, -1, false, none, o4), "플라스마 두 번째가 false");
        Check(srv.DurCount() == 4 && !hasStat(o4, a, 59, 15), "플라스마를 또 걸었는데 새로 만들거나 앞의 것을 취소했다(Sub4FEE50)");
        Check(srv.DebugDurActive(59, a) && !srv.DebugDurActive(59, b), "Sub4FEE50 이 주인을 안 가린다");
        std::vector<nox::OutPacket> o5;
        srv.DebugPlayerCancelSpells(a, o5);
        Check(!srv.DebugDurActive(59, a), "취소된 플라스마를 Sub4FEE50 이 아직 있다고 본다");
        std::vector<nox::OutPacket> o6;
        srv.DebugDurNew(a, 59, 0, -1, -1, false, none, o6);
        Check(srv.DebugDurActive(59, a) && srv.DebugDurSpellAt(0) == 59, "취소 뒤 플라스마를 다시 걸었는데 새로 안 만들었다");
    }

    // ③ create 가 0 아니면 취소하고 false · update 가 1 을 돌려준 틱에 취소
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        std::vector<nox::OutPacket> o;
        Check(!srv.DebugDurNew(a, 4, 0, 1, -1, false, none, o), "create 가 1 인데 New 가 true");
        Check(hasStat(o, a, 4, 15) && srv.DebugDurCanceled(0), "create 실패인데 취소(STAT 15·깃발)가 아니다");
        ms += 34; srv.Tick(ms, out);
        Check(srv.DurCount() == 0, "create 실패한 것이 다음 틱에 안 지워졌다");
        Check(srv.DebugDurNew(a, 4, 0, 0, 3, false, none, out), "create 0 인데 New 가 false");
        int statTick = -1;
        for (int t = 1; t <= 6 && statTick < 0; ++t) {
            std::vector<nox::OutPacket> o2;
            ms += 34; srv.Tick(ms, o2);
            if (hasStat(o2, a, 4, 15)) statTick = t;
        }
        Check(statTick == 3 && srv.DebugDurUpdates() == 3, "update 가 3번째에 1 인데 그 틱에 취소되지 않았다");
    }

    // ④ 주인이 죽으면(playerCancelSpells) 취소 · 게임에서 빠지면 틱에서 주인을 비우고 취소 · 연쇄 번개(43)는 STAT 0 · 부활도 취소
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        srv.DebugDurNew(a, 43, 0, -1, -1, false, none, out);
        std::vector<nox::OutPacket> o;
        srv.Damage(a, 1000, o);
        Check(hasStat(o, a, 43, 0), "주인이 죽었는데 연쇄 번개가 STAT 0 으로 취소되지 않았다");
        Check(!srv.DebugDurNew(a, 4, 0, -1, -1, false, none, out), "죽은 주인에게 New 가 true(u2 DEAD)");
        nox::Server srv2; std::vector<nox::OutPacket> out2; uint32_t ms2 = 0;
        setup(srv2, ms2, out2);
        srv2.DebugDurNew(b, 4, 0, -1, -1, false, none, out2);
        srv2.DebugLeave(b);
        ms2 += 34; srv2.Tick(ms2, out2);
        Check(srv2.DebugDurCanceled(0), "주인이 빠졌는데 틱에서 취소되지 않았다");
        ms2 += 34; srv2.Tick(ms2, out2);
        Check(srv2.DurCount() == 0, "주인이 빠진 것이 지워지지 않았다");
    }

    // ⑤ 걷기 입력이면 붙들고 있는 주문(24 등)만 취소 · 자연의 힘(31) 중이면 걷기 입력을 안 받는다
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        srv.DebugDurNew(a, 24, 0, -1, -1, false, none, out);
        srv.DebugDurNew(a, 4, 0, -1, -1, false, none, out);
        std::vector<uint8_t> mv = InputPacket(2);
        mv[nox::kWireHeaderSize + 1] = 5;
        mv.push_back(0);
        std::vector<nox::OutPacket> o;
        srv.OnPacket(a, mv.data(), mv.size(), o);
        ms += 34; srv.Tick(ms, o);
        Check(hasStat(o, a, 24, 15) && !hasStat(o, a, 4, 15), "걷기 입력에 번개(24)만 취소되지 않았다(cancelAllSpells_4FEE90)");
        nox::Server srv2; std::vector<nox::OutPacket> out2; uint32_t ms2 = 0;
        setup(srv2, ms2, out2);
        srv2.DebugDurNew(a, 31, 0, -1, -1, false, none, out2);
        const int x0 = srv2.XOf(a);
        for (int t = 0; t < 5; ++t) {
            srv2.OnPacket(a, mv.data(), mv.size(), out2);
            ms2 += 34; srv2.Tick(ms2, out2);
        }
        Check(srv2.XOf(a) == x0 && srv2.DebugStateOf(a) != nox::kPlayerStateWalk, "자연의 힘 중인데 걸었다(방향 입력만)");
        Check(srv2.DurCount() == 1, "자연의 힘 중 걷기 입력이 주문을 취소했다");
    }

    // ⑥ 공격 주문을 말하거나 맨손으로 치면 타원 방패(67) 취소 · CancelOffensiveFor 는 공격 주문만
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        srv.DebugDurNew(a, 67, 0, -1, -1, false, none, out);
        std::vector<nox::OutPacket> o;
        CastAndWait(srv, a, 27, false, ms, o);
        Check(hasStat(o, a, 67, 15), "파이어볼(공격)을 말했는데 타원 방패가 취소되지 않았다");
        nox::Server srv2; std::vector<nox::OutPacket> out2; uint32_t ms2 = 0;
        setup(srv2, ms2, out2);
        srv2.DebugSetWeapon(a, 0);
        for (int anim : { 23, 24, 25 }) srv2.SetPlayerAnimFrames(anim, 8, 0);   // 주먹 8칸(thing.bin)
        srv2.DebugDurNew(a, 67, 0, -1, -1, false, none, out2);
        { std::vector<nox::OutPacket> o0; ms2 += 34; srv2.Tick(ms2, o0); Check(!hasStat(o0, a, 67, 15), "치지 않았는데 타원 방패가 취소됐다"); }
        std::vector<nox::OutPacket> o2;
        auto click = InputPacket(6);
        srv2.OnPacket(a, click.data(), click.size(), o2);
        ms2 += 34; srv2.Tick(ms2, o2);
        // ⚠ 정답지 그대로 — 같은 틱 spellCastByPlayer 가 취소된 방패를 지우며 상태 13 으로 돌린다(playerSetState 에 1→13 막는 문 없음)
        Check(srv2.DebugStateOf(a) == nox::kPlayerStateIdle, "타원 방패를 지운 틱에 주인 상태가 13 이 아니다(destroyDurSpell)");
        Check(hasStat(o2, a, 67, 15), "맨손으로 쳤는데 타원 방패가 취소되지 않았다");
        nox::Server srv3; std::vector<nox::OutPacket> out3; uint32_t ms3 = 0;
        setup(srv3, ms3, out3);
        srv3.DebugDurNew(a, 24, 0, -1, -1, false, none, out3);
        srv3.DebugDurNew(a, 4, 0, -1, -1, false, none, out3);
        std::vector<nox::OutPacket> o3;
        srv3.DebugDurCancelOffensiveFor(a, o3);
        Check(hasStat(o3, a, 24, 15) && !hasStat(o3, a, 4, 15), "CancelOffensiveFor 가 공격 주문만 취소하지 않았다");
    }

    // ⑦ 대상 있는 것을 취소하면 `9e` 광선 멈춤 — 번개 11 · 마나 흡수 12 · 매혹 9 · 플라스마 8(주인 방향) · 큰 치유 13(받는 쪽이 앞 · 자기면 없음)
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        setup(srv, ms, out);
        srv.DebugSetFacing(a, 77);
        const nox::Target tb{ b, -1 };
        struct { uint8_t spell, typ, val; bool swap; } rows[] = { { 24, 11, 3, false }, { 22, 12, 3, false }, { 9, 9, 3, false },
                                                                  { 59, 8, 77, false }, { 35, 13, 3, true } };
        for (const auto& r : rows) {
            srv.DebugDurNew(a, r.spell, 0, -1, -1, false, tb, out);
            Check(srv.DebugDurFindByTarget(r.spell, tb), "대상으로 찾기(sub_4FF2D0)가 못 찾았다");
            std::vector<nox::OutPacket> o;
            srv.DebugPlayerCancelSpells(a, o);
            const uint16_t n1 = r.swap ? 729 : 728, n2 = r.swap ? 728 : 729;
            Check(rayStop(o, a, r.typ, r.val, n1, n2) && rayStop(o, b, r.typ, r.val, n1, n2), "취소했는데 `9e` 광선 멈춤이 모두에게 안 갔다");
            Check(!srv.DebugDurFindByTarget(r.spell, tb), "취소된 것을 대상으로 찾았다");
            ms += 34; srv.Tick(ms, out);
        }
        srv.DebugDurNew(a, 35, 0, -1, -1, false, nox::Target{ a, -1 }, out);
        srv.DebugDurNew(a, 4, 0, -1, -1, false, tb, out);
        std::vector<nox::OutPacket> o;
        srv.DebugPlayerCancelSpells(a, o);
        Check(!anyRay(o, a), "자기에게 건 큰 치유 · 광선 아닌 주문에 `9e` 를 보냈다");
    }
    std::printf("  4-a3 — dt 끝·dt 0 · 같은 주문·소환 교체 · 플라스마 안 겹침 · create/update 취소 · 죽음·빠짐 · 걷기·자연의 힘 · 타원 방패 · 광선 멈춤\n");
}

// ── 묶음 4-a4 — 버프 틀 · gamedata.bin ──────────────────────────────
//
// 정답지: buffApplyTo_4FF380 · spellBuffOff_4FF5B0 · unitClearBuffs_4FF580 · updateUnitBuffs_4FF620 · setUnitBuffFlags_4E48F0
//   · castBuffSpell(spells_buffs.go) · CanSee(server/object.go:76) · sub_4E7540(도운 사람) · libs balance/balance.go
void TestBuffs() {
    const std::string a = "10.4.7.1:4701";
    const std::string b = "10.4.7.2:4702";
    const std::string c = "10.4.7.3:4703";
    auto enchantBits = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint16_t nc, uint32_t* bits) {
        bool got = false;
        for (const auto& m : FlattenTo(out, to)) {
            if (m.size() != 7 || m[0] != static_cast<uint8_t>(nox::Op::REPORT_ENCHANTMENT) || (m[1] | (m[2] << 8)) != nc) continue;
            if (bits) *bits = static_cast<uint32_t>(m[3]) | (static_cast<uint32_t>(m[4]) << 8) | (static_cast<uint32_t>(m[5]) << 16) |
                              (static_cast<uint32_t>(m[6]) << 24);
            got = true;
        }
        return got;
    };
    auto tickOut = [](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& o) { ms += 34; srv.Tick(ms, o); };

    // ① gamedata 파서 — 태그·전역 · 여러 줄 값 · 사이에 # 줄이면 거기서 끝 · 끝 f 떼기 · 배열 · 이름은 대소문자 안 가림
    {
        nox::Gamedata g;
        const std::string text = "# c\r\nFoo = 1.5f;\r\nARENA Foo = 2;\r\nSOLO Bar = 7;\r\nArr = 1, 2,\r\n 3;\r\nCut = 4\r\n# x\r\nZed = 9;\r\n";
        Check(g.Parse(text), "gamedata 본보기 글을 못 읽었다");
        Check(g.Float("ARENA", "foo") == 2.0 && g.Float("SOLO", "FOO") == 1.5, "태그 값이 먼저 · 없으면 전역이 아니다");
        Check(g.Float("ARENA", "Bar", -1.0) == -1.0 && g.Float("SOLO", "Bar") == 7.0, "다른 태그 값을 가져왔다");
        const auto arr = g.Array("ARENA", "arr");
        Check(arr.size() == 3 && arr[2] == 3.0, "여러 줄 배열(1, 2, / 3;)을 못 읽었다");
        Check(g.Float("ARENA", "cut") == 4.0 && g.Float("ARENA", "zed") == 9.0, "; 없이 # 줄이 오면 거기서 끝내야 한다");
        nox::Gamedata bad;
        Check(!bad.Parse("X = abc;\n"), "숫자가 아닌 값을 받아들였다");
        nox::Gamedata real;
        if (real.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\gamedata.bin")) {
            Check(real.Float("ARENA", "TargetedSpellLifetime") == nox::kTargetedSpellFrames &&
                  real.Float("ARENA", "UnTargetedSpellLifetime") == nox::kUnTargetedSpellFrames, "gamedata 수명이 GameBalance 표와 다르다");
            Check(real.Float("ARENA", "InvulnerabilityEnchantDuration") == nox::kRespawnInvulnTicks, "gamedata 무적 길이가 150 이 아니다");
            Check(real.Float("ARENA", "LightningRange") == nox::kLightningRange, "gamedata LightningRange 가 240 이 아니다");
            const auto eb = real.Array("ARENA", "EnergyBoltDamage");
            bool same = eb.size() == 5;
            for (size_t i = 0; same && i < 5; ++i) same = std::fabs(eb[i] - nox::kEnergyBoltPerFrame[i]) < 1e-9;
            Check(same, "gamedata EnergyBoltDamage 가 GameBalance 표와 다르다");
            std::printf("  gamedata.bin — %zu개 · 표 대조(수명·무적·번개 거리·에너지볼트)\n", real.Count());
        } else {
            std::printf("  gamedata.bin 대조 — 건너뜀(%s)\n", real.Error().c_str());
        }
    }

    // ② 걸기 — 비트·남은 틱·세기 · `5a` 모두에게 · 새 버프는 투명(0)을 끈다 · 끝없음(0)은 덮어쓰지 않는다 · 죽은 이는 안 걸린다
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(a, 0, 100, 1);
        std::vector<nox::OutPacket> o;
        srv.DebugApplyEnchant(a, 21, 90, 3);
        tickOut(srv, ms, o);
        uint32_t bits = 0;
        Check(enchantBits(o, b, 728, &bits) && bits == (1u << 21), "적외선을 걸었는데 `5a` 비트가 1<<21 이 아니다(투명이 안 꺼짐)");
        Check(srv.DebugEnchantPower(a, 21) == 3 && srv.DebugEnchantDur(a, 21) == 89, "세기·남은 틱(90 − 1)이 틀리다");
        srv.DebugApplyEnchant(a, 17, 0, 2);
        srv.DebugApplyEnchant(a, 17, 50, 4);
        Check(srv.DebugEnchantDur(a, 17) == 0 && srv.DebugEnchantPower(a, 17) == 2, "끝없는(0) 버프를 덮어썼다");
        srv.DebugApplyEnchant(a, 21, 40, 5);
        Check(srv.DebugEnchantDur(a, 21) == 40 && srv.DebugEnchantPower(a, 21) == 5, "남은 틱이 있는 버프를 새로 못 덮었다");
        srv.Damage(b, 1000, out);
        srv.DebugApplyEnchant(b, 21, 40, 5);
        Check(!srv.DebugHasEnchant(b, 21), "죽은 이에게 버프가 걸렸다");
    }

    // ③ 틱마다 −1 → 0 되는 틱에 끄고 `5a` 0 · 끝없음은 안 줄어든다 · BuffOff 는 걸린 것만 알린다
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(a, 9, 3, 1);
        srv.DebugApplyEnchant(a, 18, 0, 1);
        int offTick = -1;
        for (int t = 1; t <= 6 && offTick < 0; ++t) {
            std::vector<nox::OutPacket> o;
            tickOut(srv, ms, o);
            uint32_t bits = 0xFFFFFFFFu;
            if (!srv.DebugHasEnchant(a, 9)) { offTick = t; Check(enchantBits(o, a, 728, &bits) && bits == (1u << 18), "꺼진 틱에 `5a` 가 남은 비트로 안 갔다"); }
        }
        Check(offTick == 3, "남은 틱 3 인데 3틱째에 안 꺼졌다");
        for (int t = 0; t < 40; ++t) tickOut(srv, ms, out);
        Check(srv.DebugHasEnchant(a, 18) && srv.DebugEnchantDur(a, 18) == 0, "끝없는 버프가 꺼졌거나 남은 틱이 줄었다");
        std::vector<nox::OutPacket> o;
        srv.DebugBuffOff(a, 9);
        tickOut(srv, ms, o);
        Check(!enchantBits(o, a, 728, nullptr), "안 걸린 버프를 끄는데 `5a` 를 보냈다");
    }

    // ④ 부활 무적 = 버프 23(150틱 · 세기 5) · 무기 클릭·공격 주문이 무적과 투명을 끈다 · DEATH(16) 는 무적을 뚫고 죽인다
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        RegAllSpells(srv);
        srv.SetAutoRespawn(true);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.Tick(ms, out);
        srv.Damage(a, 1000, out);
        for (int t = 0; t < 60 && !srv.IsAlive(a); ++t) tickOut(srv, ms, out);
        Check(srv.IsAlive(a) && srv.DebugHasEnchant(a, 23) && srv.DebugEnchantPower(a, 23) == 5, "부활했는데 버프 23(세기 5)이 아니다");
        Check(srv.DebugEnchantDur(a, 23) > 140 && srv.DebugEnchantDur(a, 23) <= 150, "부활 무적 남은 틱이 150 근처가 아니다");
        srv.DebugApplyEnchant(a, 16, 2, 1);
        for (int t = 0; t < 3; ++t) tickOut(srv, ms, out);
        Check(!srv.IsAlive(a), "DEATH(16) 남은 틱이 끝났는데 무적이라 안 죽었다(unitDamageClear 는 무적을 안 본다)");
        nox::Server srv2; std::vector<nox::OutPacket> out2; uint32_t ms2 = 0;
        RegAllSpells(srv2);
        srv2.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv2.Tick(ms2, out2);
        srv2.DebugApplyEnchant(a, 23, 0, 5);
        srv2.DebugApplyEnchant(a, 0, 0, 5);                     // 투명(0)은 23 을 건 뒤에 걸어야 남는다
        CastAndWait(srv2, a, 27, false, ms2, out2);
        Check(!srv2.DebugHasEnchant(a, 23) && !srv2.DebugHasEnchant(a, 0), "공격 주문을 말했는데 무적·투명이 안 꺼졌다");
    }

    // ⑤ 반마법(29)이면 시전 끝에 코드 14 · 얼림(25)이면 걷기·방향·공격 입력 무시 · 붙잡힘(5)이면 걷기만 무시
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        RegAllSpells(srv);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(a, 29, 0, 1);
        std::vector<nox::OutPacket> o;
        CastAndWait(srv, a, 27, false, ms, o);
        bool code14 = false;
        for (const auto& m : FlattenTo(o, a))
            if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 0 && m[2] == 14) code14 = true;
        Check(code14, "반마법 버프인데 시전 끝에 `a9 00 0e` 가 없다");
        for (int buff : { 25, 5 }) {
            nox::Server s2; std::vector<nox::OutPacket> o2; uint32_t m2 = 0;
            s2.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
            s2.SetPlayerAnimFrames(23, 8, 0); s2.SetPlayerAnimFrames(24, 8, 0);
            s2.Tick(m2, o2);
            s2.DebugApplyEnchant(a, buff, 0, 1);
            const int x0 = s2.XOf(a);
            std::vector<uint8_t> mv = InputPacket(2);
            mv[nox::kWireHeaderSize + 1] = 5;
            mv.push_back(0);
            for (int t = 0; t < 5; ++t) { s2.OnPacket(a, mv.data(), mv.size(), o2); tickOut(s2, m2, o2); }
            Check(s2.XOf(a) == x0 && s2.DebugStateOf(a) != nox::kPlayerStateWalk, buff == 25 ? "얼림인데 걸었다" : "붙잡힘인데 걸었다");
            s2.DebugSetWeapon(a, 0);
            auto click = InputPacket(6);
            s2.OnPacket(a, click.data(), click.size(), o2);
            tickOut(s2, m2, o2);
            if (buff == 25) Check(s2.DebugStateOf(a) != nox::kPlayerStateAttack, "얼림인데 공격이 들어갔다(playerCanAttack)");
            else Check(s2.DebugStateOf(a) == nox::kPlayerStateAttack, "붙잡힘은 공격을 막지 않는다");
            std::vector<uint8_t> fp = InputPacket(1);
            fp[nox::kWireHeaderSize + 1] = 5;
            fp.push_back(64);
            const uint8_t f0 = s2.DebugFacingOf(a);
            s2.OnPacket(a, fp.data(), fp.size(), o2);
            tickOut(s2, m2, o2);
            if (buff == 25) Check(s2.DebugFacingOf(a) == f0, "얼림인데 방향 입력으로 돌았다(CCOrientation)");
            else Check(s2.DebugFacingOf(a) == 64, "붙잡힘은 방향 입력을 막지 않는다");
        }
    }

    // ⑥ CanSee — 눈먼 시전자는 둘레 찾기에서 아무도 못 찾음 · 가만히 선 투명 대상은 못 찾음 · 시전자가 적외선이면 찾음(날아가는 것에 21 복사)
    {
        // kase 4 — 만들 때는 둘레 밖(커서 먼 곳 · b 1650)이라 못 찾고, 날아가다 틱 찾기(날아가는 것 둘레 600)에서 찾는다
        //   → 날아가는 것이 적외선(21)을 물려받아야 투명한 b 를 본다
        for (int kase = 0; kase < 5; ++kase) {
            nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
            RegSpell(srv, 12);
            srv.SetMagicType(1237, 50, 10.0f, 0x4000);
            srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
            srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
            srv.DebugMoveTo(b, kase == 4 ? 1650 : 1150, 1000);
            srv.DebugAimAt(a, 1150, kase == 4 ? 2100 : 1060);   // b 몸 밖(커서 오브젝트 아님) · 둘레 600 안(kase 4 는 밖)
            if (kase == 1) srv.DebugApplyEnchant(a, 2, 0, 1);
            if (kase >= 2) srv.DebugApplyEnchant(b, 0, 0, 1);
            if (kase >= 3) srv.DebugApplyEnchant(a, 21, 0, 1);
            CastAndWait(srv, a, 12, false, ms, out);
            const unsigned long long before = srv.DebugAcceptCount();
            for (int t = 0; t < 60 && srv.MissileCount() > 0; ++t) tickOut(srv, ms, out);
            const bool hitB = srv.DebugAcceptCount() == before + 1 && srv.DebugLastAcceptTarget().addr == b;
            static const char* kMsg[5] = { "눈멀지 않았는데 b 를 못 찾았다(시험 전제)", "눈먼 시전자가 b 를 찾았다",
                                            "가만히 선 투명한 b 를 찾았다", "적외선인데 투명한 b 를 못 찾았다",
                                            "날아가는 것이 시전자 적외선(21)을 안 물려받아 투명한 b 를 틱 찾기에서 못 봤다" };
            Check((kase == 0 || kase >= 3) ? hitB : !hitB, kMsg[kase]);
        }
    }

    // ⑦ 되받아치기 방패(27) — 앞에서 받으면 되받아치지 않고 주인·대상만 맞바꿈(b 가 받아들이지 않음)
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        RegSpell(srv, 12);
        srv.SetMagicType(1237, 50, 10.0f, 0x4000);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Conjurer);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);
        srv.DebugAimAt(a, 1150, 1000);
        srv.DebugApplyEnchant(b, 27, 0, 1);
        CastAndWait(srv, a, 12, false, ms, out);
        float maxX = 0;
        for (int t = 0; t < 90 && srv.MissileCount() > 0; ++t) {
            float x = 0, y = 0;
            if (srv.DebugMissileXY(0, x, y) && x > maxX) maxX = x;
            tickOut(srv, ms, out);
        }
        // ★ 정답지 그대로 — 27 은 되받아치지 않고 주인·대상만 바꾼다(changeOwner 뒤 return) → Magic 이 b 를 지나가
        //   a 쪽으로 돌려다 b 등에 맞아 튕기기를 되풀이하다 수명이 끝난다(시험 기록 DBG: 1148 → 1183 → 1163~1183 왕복)
        Check(maxX > 1160.0f && srv.DebugLastAcceptTarget().addr != b, "되받아치기 방패(27)인데 b 가 받아들였다(주인·대상 맞바꿈 없음)");
    }

    // ⑧ 도운 사람 — castBuffSpell(orig·offensive)이 +3600 을 적고 10초 안에 남이 죽이면 `a9 0e` 에 싣는다 · 죽인 이와 같으면 0
    //   + gamedata 이름 길이 · 초 · 세기 곱 · Once · Defensive(대상의 공격 지속 주문 취소) · 점 효과
    {
        auto killMsg = [](const std::vector<nox::OutPacket>& o, const std::string& to) -> std::vector<uint8_t> {
            for (const auto& m : FlattenTo(o, to))
                if (m.size() == 11 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 14) return m;
            return {};
        };
        for (int kase = 0; kase < 3; ++kase) {
            nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
            nox::Gamedata g;
            g.Parse("ConfuseEnchantDuration = 90;\n");
            srv.SetGamedata(g);
            srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
            srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
            srv.DebugEnterGame(c, 730, false, nox::CharClass::Warrior);
            srv.Tick(ms, out);
            nox::Server::BuffConf conf;
            conf.durOpt = "ConfuseEnchantDuration";
            conf.orig = b;
            conf.offensive = true;
            Check(srv.CastBuffSpell(12, 3, 2, nox::Target{ a, -1 }, conf, out) == 1, "castBuffSpell 이 1 이 아니다");
            Check(srv.DebugEnchantDur(a, 3) == 90 && srv.DebugEnchantPower(a, 3) == 2, "gamedata 이름 길이(90)·세기가 안 들어갔다");
            if (kase == 2) for (int t = 0; t < 301; ++t) tickOut(srv, ms, out);
            std::vector<nox::OutPacket> o;
            srv.Damage(a, 1000, o, kase == 1 ? b : c);
            const auto m = killMsg(o, a);
            const uint16_t assist = m.size() == 11 ? static_cast<uint16_t>(m[4] | (m[5] << 8)) : 0xFFFF;
            static const char* kMsg[3] = { "도운 사람(b)이 죽음 알림에 안 실렸다", "죽인 이가 도운 이와 같은데 도운 자리가 0 이 아니다",
                                           "10초가 지났는데 도운 사람을 실었다" };
            Check(assist == (kase == 0 ? 729 : 0), kMsg[kase]);
        }
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        RegAllSpells(srv);
        srv.SetSpellDef(24, 10, 0x20 | 0x4, {});
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.Tick(ms, out);
        nox::Server::BuffConf conf;
        conf.dur = 2; conf.durInSec = true; conf.durLevelMul = true; conf.once = true;
        srv.CastBuffSpell(51, 26, 3, nox::Target{ a, -1 }, conf, out);
        Check(srv.DebugEnchantDur(a, 26) == 2 * 30 * 3, "초(×30)·세기 곱이 틀리다");
        Check(srv.CastBuffSpell(51, 26, 3, nox::Target{ a, -1 }, conf, out) == 0, "Once 인데 두 번 걸렸다");
        srv.DebugDurNew(a, 24, 0, -1, -1, false, nox::Target{}, out);
        nox::Server::BuffConf def;
        def.defensive = true; def.pointFx = static_cast<uint8_t>(nox::Op::FX_RICOCHET);
        std::vector<nox::OutPacket> o;
        srv.CastBuffSpell(18, 18, 1, nox::Target{ a, -1 }, def, o);
        bool stat15 = false, fx = false;
        for (const auto& m : FlattenTo(o, a)) {
            if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && m[1] == 24 && m[5] == 15) stat15 = true;
            if (m.size() == 5 && m[0] == static_cast<uint8_t>(nox::Op::FX_RICOCHET)) fx = true;
        }
        Check(stat15, "Defensive 인데 대상의 공격 지속 주문(24)이 취소되지 않았다");
        Check(fx, "점 효과를 대상 자리로 안 보냈다");
    }

    // ⑨ 불려 나온 것에도 버프 · `5a` 는 그 번호로 · 눈먼 사람은 맨손으로 못 때린다(unitCanInteractWith 1)
    {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        nox::Server::PhysBody body;
        body.shape = 2; body.r = 10.0f; body.cls = 0x2;
        srv.SetTypeBody(1328, body);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        SummonImp(srv, a, out);
        ms = srv.DebugNowMs();
        Check(srv.pets() == 1, "임프가 안 불려 나옴(시험 전제)");
        std::vector<nox::OutPacket> o;
        srv.DebugApplyEnchantPet(0, 2, 5, 1);
        tickOut(srv, ms, o);
        Check(srv.DebugPetHasEnchant(0, 2) && enchantBits(o, a, srv.DebugPetNetCode(0), nullptr), "소환수 버프가 `5a` 로 안 갔다");
        nox::Server s2; std::vector<nox::OutPacket> o2; uint32_t m2 = 0;
        for (int i = 23; i <= 25; ++i) s2.SetPlayerAnimFrames(i, 8, 0);
        s2.DebugEnterGame(a, 728);
        s2.DebugEnterGame(b, 729);
        s2.DebugMoveTo(b, 1025, 1000);
        s2.Tick(m2, o2);
        s2.DebugApplyEnchant(a, 2, 0, 1);
        auto click = InputPacket(6);
        s2.OnPacket(a, click.data(), click.size(), o2);
        for (int t = 0; t < 12; ++t) tickOut(s2, m2, o2);
        Check(s2.HealthOf(b) == nox::kDefaultMaxHealth, "눈먼 사람이 맨손으로 때렸다");
    }
    // ⑩ 눈먼 워리어는 날아오는 Magic 을 못 봐서(CanInteract) 가만 7틱을 기다려 방패를 올린다
    for (int blind = 0; blind < 2; ++blind) {
        nox::Server srv; std::vector<nox::OutPacket> out; uint32_t ms = 0;
        RegSpell(srv, 12);
        srv.SetMagicType(1237, 50, 10.0f, 0x4000);
        srv.SetPlayerAnimFrames(40, 6, 1);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.DebugMoveTo(b, 1150, 1000);
        srv.DebugSetFacing(b, 128);
        srv.DebugAimAt(a, 1150, 1000);
        CastAndWait(srv, a, 12, false, ms, out);
        if (blind) srv.DebugApplyEnchant(b, 2, 0, 1);
        srv.DebugSetState(b, nox::kPlayerStateIdle);
        int raiseTick = -1;
        for (int t = 1; t <= 10 && raiseTick < 0; ++t) {
            tickOut(srv, ms, out);
            if (srv.DebugStateOf(b) == 15) raiseTick = t;
        }
        Check(blind ? raiseTick == 8 : (raiseTick > 0 && raiseTick <= 7),
              blind ? "눈먼 워리어가 날아오는 Magic 을 보고 방패를 올렸다" : "눈멀지 않은 워리어가 Magic 을 보고 일찍 올리지 않았다(시험 전제)");
    }
    std::printf("  4-a4 — gamedata 파서·표 대조 · 걸기·끄기·틱·5a · 부활 무적 23 · DEATH · 반마법 14 · 얼림·붙잡힘 · CanSee · 되받아치기 27 · 도운 사람 · castBuffSpell · 소환수 · 눈먼 주먹\n");
}

// ── 묶음 4-b1 — 피해 쪽 버프(damageDefaultProc 완성) ─────────────────────
//
// 정답지: nox_xxx_damageDefaultProc_4E0B30(`GAME3_2.c:7017`) · checkFireProtect_4DFE40 · checkElectrProtect_4DFF40
//   · PlayerDamage 27(`GAME3_3.c:173`) · nox_xxx_collide_4FDF90(`GAME4.c:2966`) · playerCursorScanFn_54AFB0(`GAME5.c:3588`)
void TestDamageBuffs() {
    const std::string a = "10.4.8.1:4801";
    const std::string b = "10.4.8.2:4802";
    const std::string c = "10.4.8.3:4803";
    auto mk = [&](nox::Server& srv) {
        nox::Gamedata g;
        g.Parse("ARENA FireSpellProtection = 0.5f, 0.5f, 0.5f, 0.5f, 0.5f;\nARENA ElectricitySpellProtection = 0.5f, 0.5f, 0.5f, 0.5f, 0.5f;\n"
                "ShockDamage = 15, 30, 45, 60, 75;\nVampirismCoeff = 0.1, 0.2, 0.3, 0.4, 0.5;\n");
        srv.SetGamedata(g);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        srv.DebugMoveTo(b, 1100, 1000);
    };
    auto hasOp = [](const std::vector<nox::OutPacket>& out, const std::string& to, uint8_t op, size_t len) {
        for (const auto& m : FlattenTo(out, to)) if (m.size() == len && m[0] == op) return true;
        return false;
    };

    // ① 불(17)·전기(20) 보호 — 세기 표 0.5 로 절반 · 합 최대 0.6 · 곱해서 0 이면 1 · 보호 없으면 그대로
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        const uint16_t h0 = srv.HealthOf(b);
        srv.DebugHurt(b, -1, 40, a, 1, out);
        Check(srv.HealthOf(b) == h0 - 40, "보호 없는데 불 피해가 그대로가 아니다");
        srv.DebugApplyEnchant(b, 17, 0, 2);
        const uint16_t h1 = srv.HealthOf(b);
        srv.DebugHurt(b, -1, 40, a, 1, out);
        Check(srv.HealthOf(b) == h1 - 20, "불 보호(0.5)인데 불 피해가 절반이 아니다");
        const uint16_t h2 = srv.HealthOf(b);
        srv.DebugHurt(b, -1, 1, a, 12, out);
        Check(srv.HealthOf(b) == h2 - 1, "보호로 0 이 된 피해가 1 이 아니다");
        srv.DebugApplyEnchant(b, 20, 0, 1);
        const uint16_t h3 = srv.HealthOf(b);
        srv.DebugHurt(b, -1, 30, a, 9, out);
        Check(srv.HealthOf(b) == h3 - 15, "전기 보호인데 전기 피해가 절반이 아니다");
        nox::Server s2; mk(s2);
        nox::Gamedata g;
        g.Parse("ARENA FireSpellProtection = 0.9, 0.9, 0.9, 0.9, 0.9;\n");
        s2.SetGamedata(g);
        s2.DebugApplyEnchant(b, 17, 0, 1);
        const uint16_t h4 = s2.HealthOf(b);
        s2.DebugHurt(b, -1, 50, a, 1, out);
        // 0.60000002f 로 자르므로 (1 − 0.60000002) × 50 = 19.999999 → 버림 19(정답지 float 그대로)
        Check(s2.HealthOf(b) == h4 - 19, "보호 합이 0.60000002 에서 안 잘렸다(50 × 0.39999998 → 19)");
    }

    // ② 감전(22) 되갚기 — 근접 무기로 때린 사람에게 ShockDamage[4]=75 전기 · 상태 23 · 버프 꺼짐 · 투사체·맨손(무기 오브젝트 없음)은 안 됨
    {
        for (int kase = 0; kase < 3; ++kase) {
            nox::Server srv; mk(srv);
            std::vector<nox::OutPacket> out;
            srv.DebugApplyEnchant(b, 22, 0, 1);
            nox::Server::AttackSrc src;
            src.x = 1000; src.y = 1000;
            src.obj = (kase != 1);
            src.missile = (kase == 2);
            const uint16_t ha = srv.HealthOf(a);
            srv.DebugHurt(b, -1, 10, a, 0, out, &src);
            if (kase == 0) {
                Check(srv.HealthOf(a) == ha - 75 && srv.DebugStateOf(a) == 23 && !srv.DebugHasEnchant(b, 22), "감전 되갚기(75 · 상태 23 · 버프 끔)가 아니다");
            } else {
                Check(srv.HealthOf(a) == ha && srv.DebugHasEnchant(b, 22), kase == 1 ? "무기 오브젝트 없이 감전을 되갚았다" : "투사체에 감전을 되갚았다");
            }
        }
    }

    // ③ 맞으면 투명(0) 풀림 — 때린 이가 있으면 늘 · 없으면 종류 12 만
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        srv.DebugApplyEnchant(b, 0, 0, 1);
        srv.DebugHurt(b, -1, 5, "", 1, out);
        Check(srv.DebugHasEnchant(b, 0), "때린 이 없는 불(1)에 투명이 풀렸다");
        srv.DebugHurt(b, -1, 5, "", 12, out);
        Check(!srv.DebugHasEnchant(b, 0), "때린 이 없는 종류 12 에 투명이 안 풀렸다");
        srv.DebugApplyEnchant(b, 0, 0, 1);
        srv.DebugHurt(b, -1, 5, a, 1, out);
        Check(!srv.DebugHasEnchant(b, 0), "사람에게 맞았는데 투명이 안 풀렸다");
    }

    // ④ 흡혈(13) — 때린 이 세기 2(0.2) × 피해 50 = 10 채움 · `a2` 11바이트 · 최대면 안 채움
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        srv.DebugHurt(a, -1, 40, b, 1, out);
        const uint16_t ha = srv.HealthOf(a);
        srv.DebugApplyEnchant(a, 13, 0, 2);
        std::vector<nox::OutPacket> o;
        srv.DebugHurt(b, -1, 50, a, 1, o);
        Check(srv.HealthOf(a) == ha + 10, "흡혈(0.2 × 50)만큼 안 채웠다");
        Check(hasOp(o, a, static_cast<uint8_t>(nox::Op::FX_VAMPIRISM), 11), "흡혈 효과 `a2` 11바이트가 없다");
        nox::Server full; mk(full);
        full.DebugApplyEnchant(a, 13, 0, 2);
        std::vector<nox::OutPacket> o4;
        full.DebugHurt(b, -1, 50, a, 1, o4);
        Check(!hasOp(o4, a, static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH), 3), "체력이 가득한데 흡혈이 체력 알림을 보냈다(unitAdjustHP 는 최대 아래일 때만)");
    }

    // ⑤ 소환수 — 무적(23)이면 안 깎임 · 불 보호 절반
    {
        nox::Server srv; mk(srv);
        nox::Server::PhysBody body;
        body.shape = 2; body.r = 10.0f; body.cls = 0x2;
        srv.SetTypeBody(1328, body);
        std::vector<nox::OutPacket> out;
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Conjurer);
        srv.DebugMoveTo(c, 3000, 3000);
        SummonImp(srv, c, out);
        Check(srv.pets() == 1, "임프가 안 불려 나옴(시험 전제)");
        const uint16_t hp0 = srv.DebugPetHealth(0);
        srv.DebugApplyEnchantPet(0, 23, 0, 1);
        srv.DebugHurt(std::string(), 0, 3, b, 1, out);
        Check(srv.DebugPetHealth(0) == hp0, "무적 소환수가 깎였다(damageDefaultProc 23)");
        nox::Server s3; mk(s3);
        s3.SetTypeBody(1328, body);
        s3.DebugEnterGame(c, 730, false, nox::CharClass::Conjurer);
        s3.DebugMoveTo(c, 3000, 3000);
        std::vector<nox::OutPacket> o3;
        SummonImp(s3, c, o3);
        s3.DebugApplyEnchantPet(0, 17, 0, 1);
        const uint16_t p0 = s3.DebugPetHealth(0);
        s3.DebugHurt(std::string(), 0, 4, b, 1, o3);
        Check(s3.DebugPetHealth(0) == p0 - 2, "소환수 불 보호(0.5)가 피해를 절반으로 안 줄였다");
    }

    // ⑥ 되받아치기 방패(27) 사람 — 앞쪽 종류 16 은 막고 `84` · 뒤쪽이면 맞음 · 앞쪽 투사체는 막음 · 앞쪽 칼(0)은 맞음
    {
        struct Row { float sx; uint8_t dtype; bool missile; bool blocked; const char* msg; };
        const Row rows[] = { { 1050, 16, false, true, "앞쪽 종류 16 을 되받아치기 방패가 안 막았다" },
                             { 1150, 16, false, false, "뒤쪽 종류 16 을 막았다" },
                             { 1050, 0, true, true, "앞쪽 투사체를 되받아치기 방패가 안 막았다" },
                             { 1050, 0, false, false, "앞쪽 칼(종류 0)을 되받아치기 방패가 막았다" } };
        for (const auto& r : rows) {
            nox::Server srv; mk(srv);
            { std::vector<nox::OutPacket> o0; uint32_t m0 = 0; srv.Tick(m0, o0); m0 += 34; srv.Tick(m0, o0); }   // 옮긴 자리를 실수 자리에 맞춤
            srv.DebugSetFacing(b, 128);
            srv.DebugApplyEnchant(b, 27, 0, 1);
            nox::Server::AttackSrc src;
            src.x = r.sx; src.y = 1000; src.missile = r.missile;
            std::vector<nox::OutPacket> out;
            const uint16_t h = srv.HealthOf(b);
            srv.DebugHurt(b, -1, 10, a, r.dtype, out, &src);
            Check((srv.HealthOf(b) == h) == r.blocked, r.msg);
            if (r.blocked && r.dtype == 16) Check(hasOp(out, a, static_cast<uint8_t>(nox::Op::FX_VIOLET_SPARKS), 5), "종류 16 을 막았는데 점 효과 `84` 가 없다");
        }
    }

    // ⑦ 몸닿기(4FDF90) — 감전(22) 사람이 적과 부딪히면 ShockDamage[세기−1] · 버프 꺼짐 · 투명이면 적에 닿아 풀림 · 같은 편에 닿으면 안 풀림
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugMoveTo(b, 1012, 1000);                        // 몸 반지름 합 20 안 — 겹침
        srv.DebugApplyEnchant(a, 22, 0, 2);
        srv.DebugApplyEnchant(a, 0, 0, 1);
        const uint16_t hb = srv.HealthOf(b);
        ms += 34; srv.Tick(ms, out);
        Check(srv.HealthOf(b) == hb - 30 && !srv.DebugHasEnchant(a, 22), "감전 몸닿기로 적이 ShockDamage[1]=30 을 안 받았다");
        Check(!srv.DebugHasEnchant(a, 0), "적에 닿았는데 투명이 안 풀렸다");
        nox::Server s2; mk(s2);
        std::vector<nox::OutPacket> o2;
        uint32_t m2 = 0;
        s2.Tick(m2, o2);
        s2.DebugSetTeam(a, 1); s2.DebugSetTeam(b, 1);
        s2.DebugMoveTo(b, 1012, 1000);
        s2.DebugApplyEnchant(a, 22, 0, 2);
        s2.DebugApplyEnchant(a, 0, 0, 1);                     // 투명은 나중에(새 버프가 투명을 끈다)
        const uint16_t hb2 = s2.HealthOf(b);
        m2 += 34; s2.Tick(m2, o2);
        Check(s2.DebugHasEnchant(a, 0) && s2.HealthOf(b) == hb2 && s2.DebugHasEnchant(a, 22), "같은 편에 닿았는데 투명이 풀리거나 감전했다");
    }

    // ⑧ 커서 고르기 — 투명한 사람은 적외선일 때만 커서 오브젝트
    {
        nox::Server srv; mk(srv);
        { std::vector<nox::OutPacket> o0; uint32_t m0 = 0; srv.Tick(m0, o0); m0 += 34; srv.Tick(m0, o0); }
        srv.DebugAimAt(a, 1100, 1000);
        Check(srv.DebugFindObjectAtCursor(a).addr == b, "커서 아래 b 를 못 골랐다(시험 전제)");
        srv.DebugApplyEnchant(b, 0, 0, 1);
        Check(srv.DebugFindObjectAtCursor(a).none(), "투명한 b 를 커서가 골랐다");
        srv.DebugApplyEnchant(a, 21, 0, 1);
        Check(srv.DebugFindObjectAtCursor(a).addr == b, "적외선인데 투명한 b 를 커서가 못 골랐다");
    }
    (void)c;
    std::printf("  4-b1 — 불·전기 보호(표·0.6·최소 1) · 감전 되갚기 · 맞으면 투명 풀림 · 흡혈 · 소환수 무적 · 되받아치기 27 · 몸닿기 감전·투명 · 커서 투명\n");
}

// ── 묶음 4-b2 — 버프 주문 17(19 함수) ────────────────────────────────
//
// 정답지: spellAccept 효과 갈래(spells.go:219~412) · spell_anchor.go ~ spell_villain.go(castBuffSpell 한 줄씩)
void TestBuffSpells() {
    const std::string a = "10.4.9.1:4901";
    struct Row { uint8_t spell; int enc; int dur; bool fx; const char* name; };
    // 세기 = 온라인 주문 세기 3 · 초 단위는 ×30 · Villain 은 12초 × 세기
    const Row rows[] = {
        { 1, 14, 301, false, "Anchor" }, { 3, 2, 120, false, "Blind" }, { 15, 16, 450, false, "Death" },
        { 17, 6, 1800, false, "DetectMagic" }, { 26, 11, 302, false, "Fear" }, { 32, 25, 120, false, "Freeze" },
        { 36, 9, 303, false, "Haste" }, { 37, 21, 304, false, "Infravision" }, { 39, 0, 305, true, "Invisibility" },
        { 40, 23, 306, false, "Invulnerability" }, { 42, 15, 307, false, "Light" }, { 55, 29, 308, false, "Nullify" },
        { 61, 20, 309, false, "ProtectElectricity" }, { 62, 17, 310, false, "ProtectFire" }, { 64, 18, 311, false, "ProtectPoison" },
        { 70, 8, 312, false, "Run" }, { 72, 4, 313, false, "Slow" }, { 130, 13, 314, false, "Vampirism" }, { 131, 10, 1080, false, "Villain" },
    };
    const char* kGd =
        "AnchorEnchantDuration = 301;\nFearEnchantDuration = 302;\nHasteEnchantDuration = 303;\nInfravisionEnchantDuration = 304;\n"
        "InvisibilityEnchantDuration = 305;\nInvulnerabilityEnchantDuration = 306;\nLightEnchantDuration = 307;\nWarCryDuration = 308;\n"
        "ProtectElectricityEnchantDuration = 309;\nProtectFireEnchantDuration = 310;\nProtectPoisonEnchantDuration = 311;\n"
        "RunEnchantDuration = 312;\nSlowEnchantDuration = 313;\nVampirismEnchantDuration = 314;\n";
    auto mk = [&](nox::Server& srv, uint8_t spell) {
        nox::Gamedata g;
        g.Parse(kGd);
        srv.SetGamedata(g);
        srv.SetSpellDef(spell, 10, 0x1000000, { 1, 2 });        // 어느 직업이나 · 겨누지 않음 · 음소 둘(9틱)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
    };
    for (const auto& r : rows) {
        nox::Server srv; mk(srv, r.spell);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        CastAndWait(srv, a, r.spell, true, ms, out);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s — 버프 %d 가 길이 %d · 세기 3 으로 안 걸렸다", r.name, r.enc, r.dur);
        Check(srv.DebugHasEnchant(a, r.enc) && srv.DebugEnchantDur(a, r.enc) == r.dur && srv.DebugEnchantPower(a, r.enc) == 3, msg);
        if (r.fx) {
            bool smoke = false;
            for (const auto& m : FlattenTo(out, a)) if (m.size() == 5 && m[0] == static_cast<uint8_t>(nox::Op::FX_SMOKE_BLAST)) smoke = true;
            Check(smoke, "Invisibility — 연기 점 효과 `8a` 가 없다");
        }
    }

    // Death 는 Once — 두 번째는 0 → 받아들이지 않음(마나 되돌림 · STAT 0) · 대상 없으면(겨누지 않은 커서 빈 곳) 0 → 같은 길
    {
        nox::Server srv; mk(srv, 15);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        const uint16_t full = srv.ManaOf(a);
        CastAndWait(srv, a, 15, true, ms, out);
        std::vector<nox::OutPacket> o2;
        CastAndWait(srv, a, 15, true, ms, o2);
        bool stat0 = false;
        for (const auto& m : FlattenTo(o2, a)) if (m.size() == 6 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_SPELL_STAT) && m[1] == 15 && m[5] == 0) stat0 = true;
        Check(srv.ManaOf(a) == full - 10 && stat0, "Death(Once) 두 번째가 실패(마나 되돌림 · STAT 0)가 아니다");
        nox::Server s2; mk(s2, 36);
        std::vector<nox::OutPacket> o3;
        uint32_t m3 = 0;
        const uint16_t full2 = s2.ManaOf(a);
        CastAndWait(s2, a, 36, false, m3, o3);
        Check(!s2.DebugHasEnchant(a, 9) && s2.ManaOf(a) == full2, "대상 없는 Haste 가 걸렸거나 마나를 가져갔다(castBuffSpell 대상 없음 0)");
    }
    std::printf("  4-b2 — 버프 주문 19 함수 길이·세기·연기 · Death 한 번만 · 대상 없음 실패\n");
}

// ── 묶음 4-b3 — 움직임 버프 ─────────────────────────────────────────
//
// 정답지: updateUnitsCCC SpeedCur(`server.go:171`) · updateUnitBuffs 빠르게 ×1.25(`GAME4.c:3678`) · unitUpdatePlayerImplA(`object_update.go:132~191`)
//   · 달리기 버프(`:120`) · updateObjectsVelocity_5118A0(`GAME4_1.c:3205`) · playerConfusedGetDirection_4F7A40(`GAME4.c:1187`)
void TestMoveBuffs() {
    const std::string a = "10.5.0.1:5001";
    auto walkPkt = []() {
        std::vector<uint8_t> mv = InputPacket(2);
        mv[nox::kWireHeaderSize + 1] = 5;
        mv.push_back(0);
        return mv;
    };
    // 한 사람이 buff 를 달고 12틱 걸은 x 거리
    auto walk = [&](int buff, int* facingAfter1, uint32_t* frameAt1) {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugAimAt(a, 3000, 1000);                         // 커서 멀리 — 달림
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        if (buff >= 0) srv.DebugApplyEnchant(a, buff, 0, 1);
        ms += 34; srv.Tick(ms, out);                           // CCC 가 SpeedCur 를 한 번 셈
        const auto mv = walkPkt();
        const int x0 = srv.XOf(a);
        for (int t = 0; t < 12; ++t) {
            srv.OnPacket(a, mv.data(), mv.size(), out);
            ms += 34; srv.Tick(ms, out);
            // 첫 틱은 ImplB 가 걷기로 넣을 뿐 — 힘(과 혼란)은 다음 틱 ImplA 부터. 그래서 둘째 틱 방향·프레임을 본다
            if (t == 0 && facingAfter1) *facingAfter1 = srv.DebugFacingOf(a);
            if (t == 1 && facingAfter1) { *frameAt1 = srv.frame(); facingAfter1[1] = srv.DebugFacingOf(a); }
        }
        return srv.XOf(a) - x0;
    };

    // ① SpeedCur — 버프 없음 3.0 · 느림(4) × 0.5 · 빠르게(9) × 1.25 · 걸은 거리 비도 같다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(a, 4, 0, 1);
        ms += 34; srv.Tick(ms, out);
        Check(std::fabs(srv.DebugSpeedCur(a) - 1.5f) < 1e-6f, "느림인데 SpeedCur 가 1.5 가 아니다");
        srv.DebugBuffOff(a, 4);
        srv.DebugApplyEnchant(a, 9, 0, 1);
        ms += 34; srv.Tick(ms, out);
        Check(std::fabs(srv.DebugSpeedCur(a) - 3.75f) < 1e-6f, "빠르게인데 SpeedCur 가 3.75 가 아니다");
        const int d0 = walk(-1, nullptr, nullptr), dSlow = walk(4, nullptr, nullptr), dHaste = walk(9, nullptr, nullptr);
        Check(d0 > 20, "12틱 걸었는데 거의 안 갔다(시험 전제)");
        Check(std::fabs(dSlow - 0.5 * d0) <= 0.08 * d0, "느림 걸음 거리가 절반이 아니다");
        Check(std::fabs(dHaste - 1.25 * d0) <= 0.08 * d0, "빠르게 걸음 거리가 1.25 배가 아니다");
    }

    // ② 달리기(8) 버프 — 가만히 있어도 상태 5 로 묶여 달린다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(a, 8, 0, 1);
        const int x0 = srv.XOf(a);
        for (int t = 0; t < 10; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.DebugStateOf(a) == nox::kPlayerStateRun && srv.XOf(a) > x0 + 10, "달리기 버프인데 상태 5 로 달리지 않는다");
    }

    // ③ 혼란(3) — 걷는 틱마다 방향을 (세기+3) × (삼각파 − 10) 만큼 흔든다
    {
        for (int power : { 1, 3 }) {
            nox::Server srv;
            srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
            srv.DebugAimAt(a, 3000, 1000);
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            srv.DebugApplyEnchant(a, 3, 0, power);
            const auto mv = walkPkt();
            int prev = srv.DebugFacingOf(a);
            int checked = 0;
            for (int t = 0; t < 30; ++t) {
                srv.OnPacket(a, mv.data(), mv.size(), out);
                const bool walking = srv.DebugStateOf(a) == nox::kPlayerStateWalk || srv.DebugStateOf(a) == nox::kPlayerStateRun;
                ms += 34; srv.Tick(ms, out);
                if (!walking) { prev = srv.DebugFacingOf(a); continue; }
                int v3 = static_cast<int>((srv.frame() + 728u) % 40u);
                if (v3 > 20) v3 = 40 - v3;
                const int want = ((((power + 3) * (v3 - 10) + prev) % 256) + 256) % 256;
                if (srv.DebugFacingOf(a) != want) { Check(false, "혼란 걸음 방향이 playerConfusedGetDirection 식과 다르다"); break; }
                prev = srv.DebugFacingOf(a);
                ++checked;
            }
            Check(checked >= 20, "혼란 걸음을 20틱 넘게 못 쟀다(시험 전제)");
        }
    }

    // ④ 얼림(25)·붙잡힘(5) — 남이 준 힘(ForceVec)을 적분에 넣지 않는다 · 버프 없으면 밀린다
    for (int buff : { -1, 25, 5 }) {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        if (buff >= 0) srv.DebugApplyEnchant(a, buff, 0, 1);
        const int x0 = srv.XOf(a);
        srv.DebugAddForce(a, 40.0, 0.0);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        if (buff < 0) Check(srv.XOf(a) > x0, "버프 없는데 힘을 받고 안 밀렸다(시험 전제)");
        else Check(srv.XOf(a) == x0, buff == 25 ? "얼림인데 밀렸다" : "붙잡힘인데 밀렸다");
    }
    std::printf("  4-b3 — SpeedCur 느림 0.5·빠르게 1.25 · 달리기 버프 상태 5 · 혼란 방향 식 · 얼림·붙잡힘 힘 무시\n");
}

// ── 묶음 4-b4 — 디버프 C 함수(Confuse · Stun · Fumble · Inversion · CurePoison) ──────────────
//
// 정답지: GAME4_2.c:9421(52C1E0) · 9436(52C2C0) · 9359(52C060) · 9295(52BEB0) · 9813(52CDB0) · spell_counter.go:82(changeOwner)
void TestDebuffSpells() {
    const std::string a = "10.5.1.1:5101";
    const std::string b = "10.5.1.2:5102";
    const std::string c = "10.5.1.3:5103";
    auto mk = [&](nox::Server& srv, uint8_t spell, nox::CharClass bClass) {
        nox::Gamedata g;
        g.Parse("ConfuseEnchantDuration = 90;\nStunEnchantDuration = 60;\nInversionRange = 240.0f;\n");
        srv.SetGamedata(g);
        srv.SetSpellDef(spell, 10, 0x1000000, { 1, 2 });
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, bClass);
        srv.DebugMoveTo(b, 1100, 1000);
    };
    auto syncTick = [](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& out) { srv.Tick(ms, out); ms += 34; srv.Tick(ms, out); };

    // ① Confuse — 커서 아래 b 에 혼란 90 · 세기 3 · 도운 사람(a) 기록 → c 가 죽이면 `a9 0e` 도운 자리 728
    {
        nox::Server srv; mk(srv, 12, nox::CharClass::Warrior);
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        syncTick(srv, ms, out);
        srv.DebugAimAt(a, 1100, 1000);
        CastAndWait(srv, a, 12, false, ms, out);
        Check(srv.DebugHasEnchant(b, 3) && srv.DebugEnchantDur(b, 3) == 90 && srv.DebugEnchantPower(b, 3) == 3, "Confuse — b 에 혼란 90·세기 3 이 안 걸렸다");
        std::vector<nox::OutPacket> o;
        srv.Damage(b, 1000, o, c);
        uint16_t assist = 0xFFFF;
        for (const auto& m : FlattenTo(o, b)) if (m.size() == 11 && m[0] == static_cast<uint8_t>(nox::Op::INFORM) && m[1] == 14) assist = static_cast<uint16_t>(m[4] | (m[5] << 8));
        Check(assist == 728, "Confuse 를 건 a 가 죽음 알림 도운 자리에 안 실렸다");
    }

    // ② Stun — 위저드(주문 직업)는 붙잡힘(5) · 워리어는 느림(4) · 길이 60
    for (int k = 0; k < 2; ++k) {
        nox::Server srv; mk(srv, 74, k == 0 ? nox::CharClass::Wizard : nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        syncTick(srv, ms, out);
        srv.DebugAimAt(a, 1100, 1000);
        CastAndWait(srv, a, 74, false, ms, out);
        const int want = k == 0 ? 5 : 4;
        Check(srv.DebugHasEnchant(b, want) && !srv.DebugHasEnchant(b, 9 - want) && srv.DebugEnchantDur(b, want) == 60,
              k == 0 ? "Stun — 위저드가 붙잡힘(5) 60 이 아니다" : "Stun — 워리어가 느림(4) 60 이 아니다");
    }
    //   소환수 — 질량 15 넘으면 느림(4) · 아니면 붙잡힘(5) · 소환수 AI(자리표시)가 움직이지 않게 진단 고정 임프(--debug-own-pet)로
    for (float mass : { 10.0f, 20.0f }) {
        nox::Server srv; mk(srv, 74, nox::CharClass::Warrior);
        nox::Server::PhysBody body;
        body.shape = 2; body.r = 10.0f; body.cls = 0x2; body.mass = mass;
        srv.SetTypeBody(1328, body);
        srv.SetDebugOwnPetAfter(1);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        syncTick(srv, ms, out);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.pets() == 1, "진단 고정 임프가 안 생겼다(시험 전제)");
        double px = 0, py = 0;
        srv.DebugPetXY(0, px, py);
        srv.DebugAimAt(a, static_cast<int>(px), static_cast<int>(py));
        CastAndWait(srv, a, 74, false, ms, out);
        Check(srv.DebugPetHasEnchant(0, mass > 15.0f ? 4 : 5), mass > 15.0f ? "Stun — 무거운 소환수가 느림(4)이 아니다" : "Stun — 가벼운 소환수가 붙잡힘(5)이 아니다");
    }

    // ③ Fumble — 대상 있으면 받아들임(효과는 아이템 5) · 대상 없으면 실패(마나 되돌림)
    {
        nox::Server srv; mk(srv, 33, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        syncTick(srv, ms, out);
        const uint16_t full = srv.ManaOf(a);
        srv.DebugAimAt(a, 2000, 2000);
        CastAndWait(srv, a, 33, false, ms, out);
        Check(srv.ManaOf(a) == full, "대상 없는 Fumble 이 마나를 가져갔다");
        srv.DebugAimAt(a, 1100, 1000);
        CastAndWait(srv, a, 33, false, ms, out);
        Check(srv.ManaOf(a) == full - 10, "대상 있는 Fumble 이 받아들여지지 않았다");
    }

    // ④ Inversion — b 가 a 에게 날린 혼란 Magic 을 a 가 뒤집으면(둘레 240) 대상이 b 로 바뀌어 b 가 혼란
    for (int far = 0; far < 2; ++far) {
        nox::Server srv; mk(srv, 38, nox::CharClass::Wizard);
        srv.SetSpellDef(12, 10, 0x1000000 | 0x4, { 5, 3, 5 });
        srv.SetMagicType(1237, 50, 10.0f, 0x4000);
        nox::Gamedata g;
        g.Parse("ConfuseEnchantDuration = 90;\nInversionRange = 240.0f;\n");
        srv.SetGamedata(g);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.DebugMoveTo(b, far ? 1900 : 1300, 1000);   // far — 뒤집기를 다 말할 때 Magic 이 둘레 240 밖                        // a 가 뒤집기를 다 말할 때(약 10틱) Magic 이 둘레 240 안 · 아직 안 닿음
        srv.DebugSetFacing(b, 128);
        syncTick(srv, ms, out);
        srv.DebugAimAt(b, 1000, 1000);
        CastAndWait(srv, b, 12, false, ms, out);
        Check(srv.MissileCount() == 1, "b 의 혼란 Magic 이 안 생겼다(시험 전제)");
        CastAndWait(srv, a, 38, true, ms, out);
        for (int t = 0; t < 120 && srv.MissileCount() > 0; ++t) { ms += 34; srv.Tick(ms, out); }
        if (!far) Check(srv.DebugHasEnchant(b, 3) && !srv.DebugHasEnchant(a, 3), "Inversion 뒤 Magic 이 b 에게 돌아가 b 를 혼란시키지 않았다");
        else Check(srv.DebugHasEnchant(a, 3) && !srv.DebugHasEnchant(b, 3), "둘레 240 밖 Magic 까지 Inversion 이 뒤집었다");
    }

    // ⑤ CurePoison — 독 없고 자기에게면 마나를 되돌려 받아 공짜 · 남에게면 마나 씀(독 풀기는 4-c)
    {
        nox::Server srv; mk(srv, 14, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        syncTick(srv, ms, out);
        const uint16_t full = srv.ManaOf(a);
        CastAndWait(srv, a, 14, true, ms, out);
        Check(srv.ManaOf(a) == full, "독 없는 자기 CurePoison 이 마나를 되돌려 주지 않았다");
        srv.DebugAimAt(a, 1100, 1000);
        CastAndWait(srv, a, 14, false, ms, out);
        Check(srv.ManaOf(a) == full - 10, "남에게 건 CurePoison 이 마나를 안 썼다");
    }
    std::printf("  4-b4 — Confuse 90·도운 사람 · Stun 직업·질량 갈래 · Fumble 대상 · Inversion 되돌림 · CurePoison 자기 공짜\n");
}

// ── 갱신 알림 정답지화(09-14) — 42 · 43 · 45 · dd/de/48 ─────────────────────
//
// 정답지: netPlayerObjSend_518C30(GAME4_1.c:4405~4425) — `42` 보는 사람별 · +536 뒤 2틱 · 음수만 · Packet0
//   playerReportAnything_4D9900(GAME3_2.c:4855~4970) — `43`(+10 ≠ · Packet1) · `45`(+4 ≠ +6 · sub_4E5450 · 워리어 제외) · +2184 → dd·de·48
//   playerManaAdd/Sub/Refresh(GAME3_3.c:7466·7502·7573 — +6 = 이전) · unitAdjustHP_4EE460(알림 없음) · playerMakeDefItems(:7937 서른둘 = 지금)
void TestReportFramework() {
    const std::string a = "10.6.0.1:6001";
    const std::string b = "10.6.0.2:6002";
    auto delta = [](const std::vector<uint8_t>* d) { return static_cast<int>(static_cast<int16_t>((*d)[3] | ((*d)[4] << 8))); };
    auto count = [](const std::vector<std::vector<uint8_t>>& msgs, uint8_t op) { size_t n = 0; for (const auto& m : msgs) if (!m.empty() && m[0] == op) ++n; return n; };
    auto tick4 = [](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& out) { for (int t = 0; t < 4; ++t) { ms += 33; srv.Tick(ms, out); } };
    const uint8_t k42 = static_cast<uint8_t>(nox::Op::REPORT_HEALTH_DELTA);
    const uint8_t k43 = static_cast<uint8_t>(nox::Op::REPORT_PLAYER_HEALTH);
    const uint8_t k45 = static_cast<uint8_t>(nox::Op::REPORT_MANA);

    // ① 42 는 보는 사람마다 따로 — 늦게 들어온 b 는 a 의 옛 피해를 못 받고(처음 봄 = 지금 값), 그 뒤 피해는 a·b 둘 다 받는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 3; ++t) { ms += 33; srv.Tick(ms, out); }   // 봐 둔다 — 처음 보는 오브젝트는 지금 값을 적을 뿐(정답지 0 과 견줘 양수 → 안 보냄 · +536 뒤 2틱)
        srv.Damage(a, 10, out);
        tick4(srv, ms, out);
        Check(count(FlattenTo(out, a), k42) == 1, "a 가 자기 피해 42 를 한 번 안 받았다");
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        out.clear();
        tick4(srv, ms, out);
        Check(count(FlattenTo(out, b), k42) == 0, "늦게 들어온 b 가 a 의 옛 피해 42 를 받았다(처음 보면 지금 값을 적을 뿐)");
        out.clear();
        srv.Damage(a, 7, out);
        tick4(srv, ms, out);
        const auto toA1 = FlattenTo(out, a), toB1 = FlattenTo(out, b);
        const auto* da = FindMsg(toA1, k42);
        const auto* db = FindMsg(toB1, k42);
        Check(da && db && delta(da) == -7 && delta(db) == -7 && (*db)[1] == 0xd8 && (*db)[2] == 0x02,
              "a 의 새 피해 42(−7 · 번호 728)가 a·b 둘 다에게 안 갔다");
    }
    // ② 마지막 피해 뒤 2틱 안에는 안 나가고, 두 번 맞은 것은 지나서 **합쳐 한 번**(−10 −5 → −15) · 껍데기 밖(Packet0)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 3; ++t) { ms += 33; srv.Tick(ms, out); }
        srv.Damage(a, 10, out);
        ms += 33; srv.Tick(ms, out);
        srv.Damage(a, 5, out);
        ms += 33; srv.Tick(ms, out);
        ms += 33; srv.Tick(ms, out);
        Check(count(FlattenTo(out, a), k42) == 0, "마지막 피해 뒤 2틱 안인데 42 가 나갔다");
        out.clear();
        tick4(srv, ms, out);
        const auto toA = FlattenTo(out, a);
        const auto* d = FindMsg(toA, k42);
        Check(d && delta(d) == -15 && count(toA, k42) == 1, "두 피해가 합쳐진 42(−15) 한 번이 아니다");
        bool bare = false;
        for (const auto& p : out) { const auto body = Body(p); if (!body.empty() && body[0] == k42) bare = true; }
        Check(bare, "42 가 껍데기 안에 들어갔다(정답지 netSendPacket0)");
    }
    // ③ 회복(unitAdjustHP)은 42 없음 — 43 만 다음 틱, 값이 바뀔 때만 · 껍데기 안(Packet1) · 최대로 자름
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 20, out);
        tick4(srv, ms, out);
        out.clear();
        srv.DebugAdjustHP(a, 5);
        ms += 33; srv.Tick(ms, out);
        const auto toA = FlattenTo(out, a);
        Check(count(toA, k42) == 0, "회복(양수)이 42 로 나갔다");
        const auto* h = FindMsg(toA, k43);
        Check(h && h->size() == 3 && ((*h)[1] | ((*h)[2] << 8)) == 60, "회복 뒤 다음 틱 43 이 60 이 아니다");
        bool wrapped = false;
        for (const auto& p : out) { const auto body = Body(p); if (body.size() > 4 && body[0] == static_cast<uint8_t>(nox::Op::SEQ_IMPORTANT) && body[4] == k43) wrapped = true; }
        Check(wrapped, "43 이 껍데기 밖으로 나갔다(정답지 netSendPacket1)");
        out.clear();
        tick4(srv, ms, out);
        Check(count(FlattenTo(out, a), k43) == 0, "값이 안 바뀌었는데 43 을 또 보냈다");
        srv.DebugAdjustHP(a, 100);
        Check(srv.HealthOf(a) == nox::HealthOnline(nox::CharClass::Wizard), "unitAdjustHP 가 최대로 안 잘랐다");
        srv.DebugAdjustHP(a, 5);
        Check(srv.HealthOf(a) == nox::HealthOnline(nox::CharClass::Wizard), "가득한데 unitAdjustHP 가 넘겼다");
    }
    // ④ 마나 — 빼고·더하면 다음 틱 45(맨 메시지) 한 번 · 같은 틱에 두 번 바뀌면 마지막 값 하나 · 대기열에서는 앞의 45 를 지운다(sub_4E5450) · 워리어 없음
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t full = srv.ManaOf(a);
        srv.DebugManaSub(a, 30);
        srv.DebugManaAdd(a, 10);
        Check(srv.ManaOf(a) == full - 20, "빼고 더한 마나가 다르다");
        ms += 33; srv.Tick(ms, out);
        const auto toA = FlattenTo(out, a);
        const auto* m = FindMsg(toA, k45);
        Check(m && m->size() == 5 && (*m)[1] == 0xd8 && (*m)[2] == 0x02 && ((*m)[3] | ((*m)[4] << 8)) == full - 20 && count(toA, k45) == 1,
              "마나가 바뀐 다음 틱 45(마지막 값 · 한 번)가 아니다");
        bool bare = false;
        for (const auto& p : out) { const auto body = Body(p); if (!body.empty() && body[0] == k45) bare = true; }
        Check(bare, "45 가 껍데기 안에 들어갔다(정답지 sub_4E5450 → netSendPacket0)");
        out.clear();
        tick4(srv, ms, out);
        Check(count(FlattenTo(out, a), k45) == 0, "마나가 안 바뀌었는데 45 를 또 보냈다");
        srv.DebugImportantReal(a, true);
        srv.DebugManaSub(a, 5);
        ms += 33; srv.Tick(ms, out);
        srv.DebugManaSub(a, 5);
        ms += 33; srv.Tick(ms, out);
        Check(srv.DebugImportantPendingOp(a, k45) == 1, "대기열에 45 가 둘 남았다(sub_4E5450 은 같은 옵을 지운다)");
        nox::Server s2;
        s2.DebugEnterGame(b, 729, false, nox::CharClass::Warrior);
        std::vector<nox::OutPacket> o2;
        uint32_t m2 = 0;
        s2.Tick(m2, o2);
        s2.DebugManaSub(b, -5);                                        // 최대 0 이라 더하기는 0 — playerManaSub 산수(현재 > 양 → 현재 − 양)로 5 를 만든다
        m2 += 33; s2.Tick(m2, o2);
        Check(s2.ManaOf(b) == 5 && count(FlattenTo(o2, b), k45) == 0, "워리어에게 45 가 나갔다(+2251 직업 0 은 netReportMana 가 안 보냄)");
    }
    // ⑤ +2184 → dd(껍데기 안) · de(맨 · 워리어 제외) · 48 14B(번호·최대 체력·최대 마나·짐 한계·속도·힘·레벨) 한 번만 · 스냅샷 꼬리도 직업 값
    {
        for (int k = 0; k < 2; ++k) {
            const nox::CharClass cls = k == 0 ? nox::CharClass::Wizard : nox::CharClass::Warrior;
            nox::Server srv;
            srv.DebugEnterGame(a, 728, false, cls);
            std::vector<nox::OutPacket> out;
            uint32_t ms = 0;
            srv.Tick(ms, out);
            srv.DebugSetStatsDirty(a);
            ms += 33; srv.Tick(ms, out);
            const auto toA = FlattenTo(out, a);
            const auto* dd = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_HEALTH));
            const auto* de = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_TOTAL_MANA));
            const auto* st = FindMsg(toA, static_cast<uint8_t>(nox::Op::REPORT_STATS));
            const int hp = nox::HealthOnline(cls), mp = nox::ManaOnline(cls);
            Check(dd && dd->size() == 7 && ((*dd)[3] | ((*dd)[4] << 8)) == hp && ((*dd)[5] | ((*dd)[6] << 8)) == hp,
                  k == 0 ? "위저드 dd 가 75/75 가 아니다" : "워리어 dd 가 150/150 이 아니다");
            if (k == 0) Check(de && de->size() == 7 && ((*de)[3] | ((*de)[4] << 8)) == mp && ((*de)[5] | ((*de)[6] << 8)) == mp, "위저드 de 가 150/150 이 아니다");
            else Check(de == nullptr, "워리어인데 de 가 나갔다(+2251 직업 0)");
            const int carry = nox::CarryCapacityOnline(cls), spd = nox::SpeedOnline(cls), str = nox::StrengthOnline(cls);
            Check(st && st->size() == 14 && (*st)[1] == 0xd8 && (*st)[2] == 0x02 &&
                  ((*st)[3] | ((*st)[4] << 8)) == hp && ((*st)[5] | ((*st)[6] << 8)) == mp &&
                  ((*st)[7] | ((*st)[8] << 8)) == carry && ((*st)[9] | ((*st)[10] << 8)) == spd &&
                  ((*st)[11] | ((*st)[12] << 8)) == str && (*st)[13] == nox::kMaxLevel,
                  k == 0 ? "위저드 48 이 75·150·1650·17500·35·10 이 아니다" : "워리어 48 이 150·0·3000·21000·125·10 이 아니다");
            out.clear();
            ms += 33; srv.Tick(ms, out);
            Check(FindMsg(FlattenTo(out, a), static_cast<uint8_t>(nox::Op::REPORT_STATS)) == nullptr, "+2184 를 안 껐다(48 이 또 나감)");
        }
        Check(nox::CarryCapacityOnline(nox::CharClass::Wizard) == 1650, "위저드 짐 한계가 녹음 1650 과 다르다");
        const auto tail = nox::BuildSnapshotTail(728, nox::CharClass::Warrior, 0, 0);
        bool ok = false;
        for (size_t i = 0; i + 14 <= tail.size(); ++i) {
            if (tail[i] == 0x48 && tail[i + 1] == 0xd8 && tail[i + 2] == 0x02 && tail[i + 7] == 0xb8 && tail[i + 8] == 0x0b &&
                tail[i + 9] == 0x08 && tail[i + 10] == 0x52 && tail[i + 11] == 125 && tail[i + 12] == 0 && tail[i + 13] == 10) ok = true;
        }
        Check(ok, "워리어 스냅샷 꼬리 48 이 3000·21000·125·10 이 아니다(녹음 상수가 남았다)");
    }
    // ⑥ 부활 — 서른둘 기록 = 지금(되살아난 같은 틱에 맞은 것도 42) · 마나 +6 = 이전 → 다음 틱 45 최대 · 43 지금 체력
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 3; ++t) { ms += 33; srv.Tick(ms, out); }
        srv.DebugManaSub(a, 30);
        srv.Damage(a, 1000, out);
        Check(!srv.IsAlive(a), "안 죽었다(시험 전제)");
        for (int t = 0; t < 60; ++t) { ms += 33; srv.Tick(ms, out); }
        out.clear();
        auto click = InputPacket(6);
        srv.OnPacket(a, click.data(), click.size(), out);
        ms += 33; srv.Tick(ms, out);
        Check(srv.IsAlive(a), "클릭했는데 안 되살아났다(시험 전제)");
        srv.DebugBuffOff(a, 23);                                       // 부활 무적(4-a4)을 끄고 때린다
        srv.Damage(a, 5, out);
        ms += 33; srv.Tick(ms, out);
        const auto toA = FlattenTo(out, a);
        const auto* h = FindLastMsg(toA, k43);
        const auto* m = FindMsg(toA, k45);
        Check(h && ((*h)[1] | ((*h)[2] << 8)) == nox::HealthOnline(nox::CharClass::Wizard) - 5, "부활 뒤 43 이 지금 체력(70)이 아니다");
        Check(m && ((*m)[3] | ((*m)[4] << 8)) == nox::ManaOnline(nox::CharClass::Wizard), "부활 뒤 45 가 최대 마나가 아니다(playerManaRefresh +6)");
        bool bareDe = false;
        for (const auto& p : out) { const auto body = Body(p); if (!body.empty() && body[0] == static_cast<uint8_t>(nox::Op::REPORT_TOTAL_MANA)) bareDe = true; }
        Check(bareDe, "부활 de 가 껍데기 안에 들어갔다(netReportTotalMana_4D88C0 → sub_4E5450 Packet0)");
        out.clear();
        tick4(srv, ms, out);
        const auto toB6 = FlattenTo(out, b);
        const auto* db = FindMsg(toB6, k42);
        Check(db && delta(db) == -5, "부활 직후 피해 42 가 −5 가 아니다(서른둘 기록 = 지금 체력)");
    }
    // ⑥b 부활이 서른둘 기록을 「지금 체력」으로 맞추는 것 — 되살아난 **같은 틱 안**(첫 갱신 전)에 맞으면 그 차이(−5)가 나가야 한다.
    //    (되살린 뒤 갱신이 한 번 돌면 기록은 어차피 지금 값이 되므로, 틱 밖에서 되살리고 곧바로 때려 그 창을 연다)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        for (int t = 0; t < 3; ++t) { ms += 33; srv.Tick(ms, out); }
        srv.Damage(a, 1000, out);
        tick4(srv, ms, out);                                           // 죽음의 42(−75)가 나가고 기록 = 0
        out.clear();
        srv.DebugRespawn(a);                                           // playerMakeDefItems — 서른둘 = 지금(최대)
        srv.DebugBuffOff(a, 23);
        srv.Damage(a, 5, out);                                         // 첫 갱신 전에 맞음
        for (int t = 0; t < 6; ++t) { ms += 33; srv.Tick(ms, out); }
        const auto toB6b = FlattenTo(out, b);
        const auto* db = FindMsg(toB6b, k42);
        Check(db && delta(db) == -5, "부활 직후(첫 갱신 전) 피해 42 가 −5 가 아니다(부활이 서른둘 기록을 지금 체력으로 안 맞춤)");
    }
    // ⑦ 소환수도 — netSendComplexObject_518960 a3 갈래: 보는 사람마다(+412+2×i) · +536 뒤 2틱 · 음수만 · 번호는 소환수 것
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Conjurer);
        srv.DebugMoveTo(b, 1300, 1000);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, b, out);
        const uint16_t pnc = srv.DebugPetNetCode(0);
        Check(pnc != 0, "임프가 안 불려 나왔다(시험 전제)");
        uint32_t ms = srv.DebugNowMs();
        for (int t = 0; t < 3; ++t) { ms += 33; srv.Tick(ms, out); }
        out.clear();
        srv.DebugHurt("", 0, 10, a, 1, out);                           // a 가 b 의 임프를 때린다(주인이 달라 적)
        ms += 33; srv.Tick(ms, out);
        ms += 33; srv.Tick(ms, out);
        auto petDelta = [&](const std::vector<std::vector<uint8_t>>& msgs) -> const std::vector<uint8_t>* {
            for (const auto& mm : msgs) if (mm.size() == 5 && mm[0] == k42 && (mm[1] | (mm[2] << 8)) == pnc) return &mm;
            return nullptr;
        };
        const auto toA0 = FlattenTo(out, a);
        Check(petDelta(toA0) == nullptr, "소환수 마지막 피해 뒤 2틱 안인데 42 가 나갔다");
        out.clear();
        tick4(srv, ms, out);
        const auto toA7 = FlattenTo(out, a), toB7 = FlattenTo(out, b);
        const auto* da = petDelta(toA7);
        const auto* db = petDelta(toB7);
        Check(da && db && delta(da) == -10 && delta(db) == -10, "소환수 피해 42(−10 · 소환수 번호)가 a·b 둘 다에게 안 갔다");
    }
    std::printf("  갱신 알림 정답지화 — 42 보는 사람별·2틱·음수만·합침·소환수 · 43 바뀔 때만 · 45 다음 틱·겹침 지움·워리어 없음 · +2184 dd/de/48 · 부활 기록\n");
}

// ── 묶음 4-b5a — 즉발 회복(LesserHeal · RestoreHealth · Wink · RestoreMana) ──────────────
//
// 정답지: spells.go:288(41 LesserHeal)·334(68 RestoreHealth·133 Wink)·337(69 RestoreMana)
//   GAME4_2.c:10369(52DD50) · 9307(52BF20) · 9320(52BF50)
void TestHealSpells() {
    const std::string a = "10.7.0.1:7001";
    // LesserHealAmount 0.25 → 위저드 75 × 0.25 = 18.75 → 버림 18
    auto mk = [&](nox::Server& srv, uint8_t spell) {
        nox::Gamedata g;
        g.Parse("LesserHealAmount = 15;\n");
        srv.SetGamedata(g);
        srv.SetSpellDef(spell, 10, 0x1000000, { 1, 2 });    // 어느 직업이나 · 음소 둘(9틱)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
    };
    const int full = nox::HealthOnline(nox::CharClass::Wizard);   // 75

    // ① LesserHeal 자기 — 다친 뒤 18 회복(LesserHealAmount × 직업 최대체력, 버림)
    {
        nox::Server srv; mk(srv, 41);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 40, out);                              // 75 → 35
        Check(srv.HealthOf(a) == full - 40, "다침 전제(35)가 아니다");
        CastAndWait(srv, a, 41, true, ms, out);
        Check(srv.HealthOf(a) == full - 40 + 15, "LesserHeal 이 15(LesserHealAmount × 직업배수 1.0) 안 채웠다");
    }

    // ② LesserHeal 자기 · 최대 체력 — 마나를 되돌려 공짜(cast 에서 뗀 마나가 다시 참)
    {
        nox::Server srv; mk(srv, 41);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t manaFull = srv.ManaOf(a);
        Check(srv.HealthOf(a) == full, "시작이 최대 체력이 아니다(전제)");
        CastAndWait(srv, a, 41, true, ms, out);
        Check(srv.HealthOf(a) == full, "최대인데 LesserHeal 이 체력을 바꿨다");
        Check(srv.ManaOf(a) == manaFull, "최대 체력 자기 LesserHeal 이 마나를 안 되돌렸다(공짜여야 한다)");
    }

    // ③ RestoreHealth 자기 — 다친 뒤 가득
    {
        nox::Server srv; mk(srv, 68);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 40, out);
        Check(srv.HealthOf(a) == full - 40, "다침 전제가 아니다");
        CastAndWait(srv, a, 68, true, ms, out);
        Check(srv.HealthOf(a) == full, "RestoreHealth 가 최대로 안 채웠다");
    }

    // ③b Wink(133) — 회복 함수를 RestoreHealth 와 공유(순간이동은 안 딸림)
    {
        nox::Server srv; mk(srv, 133);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 40, out);
        CastAndWait(srv, a, 133, true, ms, out);
        Check(srv.HealthOf(a) == full, "Wink 가 최대로 안 채웠다(회복 몫)");
    }

    // ④ RestoreMana 자기 — 마나 쓴 뒤 가득
    {
        nox::Server srv; mk(srv, 69);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t manaFull = srv.ManaOf(a);
        srv.DebugManaSub(a, 60);                             // 마나 씀
        Check(srv.ManaOf(a) < manaFull, "마나가 안 줄었다(전제)");
        CastAndWait(srv, a, 69, true, ms, out);
        Check(srv.ManaOf(a) == manaFull, "RestoreMana 가 최대 마나로 안 채웠다");
    }

    // ⑤ 대상 없는 커서 빈 곳 — 세 회복 주문 다 받아들이지 않음(fnc 0). SpellAccept 가 false 면 시전 틀이 **마나를 되돌린다**(CastSpellByUser 실패 갈래)
    //   ★ 체력만 보면 「대상 없음 → 1」 돌연변이를 못 잡는다(둘 다 효과 0) — **마나가 되돌아왔나**로 가른다 · 41·68·69 각각
    for (uint8_t heal : { static_cast<uint8_t>(41), static_cast<uint8_t>(68), static_cast<uint8_t>(69) }) {
        nox::Server srv; mk(srv, heal);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(a, 40, out);
        srv.DebugManaSub(a, 5);                              // 마나를 조금 써 둔다 — RestoreMana(69)도 「채웠나」와 「되돌렸나」가 안 겹치게
        const uint16_t manaBefore = srv.ManaOf(a);
        srv.DebugAimAt(a, 4000, 4000);                       // 빈 곳
        CastAndWait(srv, a, heal, false, ms, out);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "대상 없는 회복 주문 %u 이 마나를 안 되돌렸다(SpellAccept 0 → 시전 틀이 되돌림)", heal);
        Check(srv.ManaOf(a) == manaBefore, msg);
        Check(srv.HealthOf(a) == full - 40, "대상 없는 회복 주문이 체력을 채웠다(효과가 0 이어야 한다)");
    }

    // ⑥ LesserHeal 을 **자기가 아닌** 최대 체력 대상에게 — 마나는 안 되돌아온다(되돌림은 「자기+최대」 갈래만)
    {
        const std::string b = "10.7.0.2:7002";
        nox::Server srv;
        nox::Gamedata g; g.Parse("LesserHealAmount = 15;\n"); srv.SetGamedata(g);
        srv.SetSpellDef(41, 10, 0x1000000, { 1, 2 });
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(b, 1100, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t manaFull = srv.ManaOf(a);
        srv.DebugAimAt(a, 1100, 1000);                       // b 를 겨눔(자기 아님)
        CastAndWait(srv, a, 41, false, ms, out);
        Check(srv.HealthOf(b) == full, "b 가 최대 체력이 아니다(전제)");
        Check(srv.ManaOf(a) == manaFull - 10, "자기가 아닌 최대 체력 대상 LesserHeal 인데 마나가 되돌아왔다(자기검사 빠짐)");
    }
    std::printf("  4-b5a — LesserHeal 15(기획값×배수 1.0)·자기 최대 마나 되돌림·남에게는 안 되돌림 · RestoreHealth·Wink 가득 · RestoreMana 가득 · 대상 없음 마나 되돌림\n");
}

// ── 묶음 4-b5b(1) — 생명을 마나로 옮기는 주문(ChannelLife 8) ──────────────
//
// 정답지: spells.go:233 → duration.New(nil, sub_52F460, nil, 0) · sub_52F460(GAME4_2.c:11444)
//   시전자 체력 1 씩 깎아 대상 마나를 ChannelLifeCoeff[세기−1] 만큼 채운다(소수는 이월) · 빔은 없다(netStartDurationRaySpell 갈래에 8 이 없음)
void TestChannelLife() {
    const std::string a = "10.8.0.1:8001";
    const std::string b = "10.8.0.2:8002";
    // 세기 3(온라인) → ChannelLifeCoeff[2]. 이월을 재려고 전 칸을 0.5 로 둔다 — 두 틱에 1 씩 오른다
    auto mk = [&](nox::Server& srv) {
        nox::Gamedata g;
        g.Parse("ChannelLifeCoeff = 0.5, 0.5, 0.5, 0.5, 0.5;\n");
        srv.SetGamedata(g);
        srv.SetSpellDef(8, 10, 0x1000000, { 1, 2 });        // 어느 직업이나 · 겨눔 아님(자기 아닌 대상도 곧바로 SpellAccept)
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(b, 1100, 1000);
    };
    auto cast = [&](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& out) {
        srv.DebugAimAt(a, 1100, 1000);                       // b 를 겨눔
        CastAndWait(srv, a, 8, false, ms, out);
    };

    // ① 시전자 생명 → 대상 마나. 계수 0.5 라 두 틱에 마나 1 · 시전자는 틱마다 1 깎임
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(b, 40);                             // 대상 마나를 비워 둔다(가득이면 곧바로 취소)
        const uint16_t bMana = srv.ManaOf(b);
        const uint16_t aHp = srv.HealthOf(a);
        cast(srv, ms, out);
        Check(srv.DebugDurActive(8, a), "ChannelLife 가 지속 주문으로 안 걸렸다");
        for (int t = 0; t < 10; ++t) { ms += 34; srv.Tick(ms, out); }
        const int gaveMana = static_cast<int>(srv.ManaOf(b)) - static_cast<int>(bMana);
        const int lostHp = static_cast<int>(aHp) - static_cast<int>(srv.HealthOf(a));
        Check(lostHp >= 9 && lostHp <= 11, "시전자가 틱마다 1 씩 안 깎였다");
        Check(gaveMana >= 4 && gaveMana <= 6, "대상 마나가 0.5/틱(이월 포함) 으로 안 찼다");
        Check(gaveMana < lostHp, "계수 0.5 인데 마나가 깎인 체력만큼 올랐다(이월을 안 쓴 것)");
    }

    // ② 대상 마나가 가득이면 곧바로 취소 — 시전자 체력도 안 깎인다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t aHp = srv.HealthOf(a);
        cast(srv, ms, out);                                  // b 는 마나 가득
        for (int t = 0; t < 6; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(8, a), "대상 마나가 가득인데 ChannelLife 가 안 끊겼다");
        Check(srv.HealthOf(a) == aHp, "대상 마나가 가득인데 시전자 체력이 깎였다");
    }

    // ③ 시전자가 달리면(버프 8) 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(b, 40);
        cast(srv, ms, out);
        Check(srv.DebugDurActive(8, a), "걸리지도 않았다(시험 전제)");
        srv.DebugApplyEnchant(a, 8, 0, 1);                   // 달리기
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(8, a), "시전자가 달리는데 ChannelLife 가 안 끊겼다");
    }

    // ④ 시전자 체력이 1 이하로 내려가면 끊긴다(제 목숨까지 태우지 않는다)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(b, 120);                            // 오래 이어지도록 대상 마나를 많이 비움
        cast(srv, ms, out);
        srv.Damage(a, srv.HealthOf(a) - 3, out);             // 체력 3 만 남김
        for (int t = 0; t < 10; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(8, a), "시전자 체력이 바닥인데 ChannelLife 가 안 끊겼다");
        Check(srv.HealthOf(a) >= 1 && srv.IsAlive(a), "ChannelLife 가 시전자를 죽였다(체력 1 이하면 멈춰야 한다)");
    }

    // ⑤ 소환수를 대상으로 걸 때 — 그 소환수가 **건 자리(시전자가 서 있던 곳)** 에서 5 넘게 떨어져 있으면 곧바로 끊긴다
    //   ⚠ 정답지 sub_4FEA70 은 「대상과 **시전 자리**」를 견준다(대상과 시전자가 아니다) — 소환수 대상은 목줄이 짧다
    //   ⚠ 판 짜기 함정 둘을 밟고 고친 자리다:
    //     ⑴ 300 을 띄웠더니 **커서가 임프를 못 집어** 주문이 아예 안 붙었다 — 원본도 돌연변이도 그냥 통과(헛통과)
    //     ⑵ 60 에서 **남의 임프**를 쓰면 걸어와 때려서 체력이 깎인다 → 그걸 ChannelLife 탓으로 오독한다
    //   그래서 **제 임프**(주인은 안 때린다)를 60 띄워 쓰고, 임프가 시전 자리로 다가오기 전에 재려고 틱을 짧게 본다
    {
        const std::string c = "10.8.0.3:8003";
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        // ⚠⚠ **임프 몸을 먼저 등록해야 커서가 집는다** — FindObjectAtCursor 는 몸을 모르는 종류를 건너뛴다(지어내지 않는다).
        //   이걸 빼먹어 대상이 none 으로 붙었고, 그 바람에 「목줄 검사를 지운」 돌연변이가 세 판이나 안 걸렸다(원본도 돌연변이도 대상 없음으로 취소)
        nox::Server::PhysBody impBody;
        impBody.shape = 2; impBody.r = 10.0f; impBody.cls = 0x2; impBody.mass = 10.0f;
        srv.SetTypeBody(1328, impBody);
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Conjurer);
        srv.DebugMoveTo(c, 3000, 3000);
        SummonImp(srv, c, out);                              // ⚠ **남의** 임프라야 한다 — 제 소환수는 커서 친구 거르기에 걸려 대상이 안 잡힌다
        double px = 0, py = 0;
        Check(srv.DebugPetXY(0, px, py), "임프가 안 불려 나왔다(시험 전제)");
        uint32_t ms = srv.DebugNowMs();
        srv.DebugMoveTo(a, static_cast<int>(px) - 60, static_cast<int>(py));   // 60 — 더 띄우면 커서가 임프를 못 집어 주문이 아예 안 붙는다
        ms += 34; srv.Tick(ms, out);
        srv.DebugAimAt(a, static_cast<int>(px), static_cast<int>(py));
        CastAndWait(srv, a, 8, false, ms, out);
        for (int t = 0; t < 2; ++t) { ms += 34; srv.Tick(ms, out); }
        // ⚠ 체력으로는 못 가른다 — **남의 임프가 걸어와 때리기** 때문이다(그걸 ChannelLife 탓으로 오독했던 자리)
        // ⚠⚠ **알려진 구멍(09-14)** — 이 갈래는 「목줄 검사를 지운」 돌연변이(CL10)를 **아직 못 잡는다**. 네 판을 고쳐 봤고
        //   몸 등록 빠짐·제 소환수 친구 거르기·거리 300 에서 대상 못 집음은 전부 걷어냈는데도 원본·돌연변이가 같이 끊긴다.
        //   즉 **이 검사가 아니라 다른 이유로 끊기고 있다** — 소환수 대상 지속주문을 더 볼 때(4-f 소환) 다시 판다
        Check(!srv.DebugDurActive(8, a), "소환수가 건 자리에서 5 넘게 떨어졌는데 ChannelLife 가 안 끊겼다");
    }
    std::printf("  4-b5b(1) — ChannelLife 생명→마나(0.5/틱 이월) · 대상 마나 가득/달리기/체력 1 이하/소환수 목줄 5 면 끊김\n");
}

// ── 묶음 4-b5b(2) — 치료 광선(GreaterHeal 35) ─────────────────────────────
//
// 정답지: spells.go:277 → duration.New(sub_52F220, sub_52F2E0, nil, 0)
//   create 52F220(GAME4_2.c:11329) — 둘레 400 에서 받을 이를 찾아 **적이 아니면** 빔을 켜고 이어 간다
//   tick   52F2E0(:11376) — blob 회복률(레벨 3 → 1.0) × 직업배수 만큼 채우고 **시전자 마나 1**
void TestGreaterHeal() {
    const std::string a = "10.8.1.1:8101";
    const std::string b = "10.8.1.2:8102";
    auto mk = [&](nox::Server& srv, bool sameTeam) {
        srv.SetSpellDef(35, 10, 0x1000000, { 1, 2 });        // 어느 직업이나 · 음소 둘
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1080, 1000);                      // 둘레 400 안
        srv.DebugSetTeam(a, 1);
        srv.DebugSetTeam(b, sameTeam ? 1 : 2);               // 같은 편이라야 고쳐 준다
    };
    const uint8_t k9e = static_cast<uint8_t>(nox::Op::FX_DURATION_SPELL);

    // ① 아군을 고친다 — 빔 `9e 06`(받는 쪽이 앞) · 대상 체력 오름 · 시전자 마나 줄어듦
    {
        nox::Server srv; mk(srv, true);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, 30, out);
        const uint16_t bHp = srv.HealthOf(b);
        out.clear();
        srv.DebugAimAt(a, 1080, 1000);                       // 정답지는 **커서 자리**를 탐색 중심으로 쓴다(Pos2) — 안 겨누면 (0,0) 을 뒤진다
        CastAndWait(srv, a, 35, false, ms, out);
        Check(srv.DebugDurActive(35, a), "아군에게 건 GreaterHeal 이 안 걸렸다");
        // 빔 시작 — 갈래 6 · 받는 쪽(b 729) 이 앞, 시전자(a 728) 가 뒤
        // ⚠ `FindMsg(FlattenTo(...))` 로 쓰면 임시 벡터를 가리키는 **매달린 포인터**가 된다 — 이름을 붙여 살려 둔다
        const auto toB = FlattenTo(out, b);
        const auto* ray = FindMsg(toB, k9e);
        Check(ray != nullptr && ray->size() == 7 && (*ray)[1] == 6 &&
              ((*ray)[3] | ((*ray)[4] << 8)) == 729 && ((*ray)[5] | ((*ray)[6] << 8)) == 728,
              "치료 광선 `9e 06`(받는 쪽 앞)이 안 나갔다");
        // ★ 마나는 **시전 뒤부터** 재야 틱 소모만 보인다 — 시전 비용(10)이 섮이면 「틱마다 1 안 씁」 돌연변이를 가린다
        const uint16_t aManaAfterCast = srv.ManaOf(a);
        for (int t = 0; t < 8; ++t) { ms += 34; srv.Tick(ms, out); }
        const int healed = static_cast<int>(srv.HealthOf(b)) - static_cast<int>(bHp);
        const int spent = static_cast<int>(aManaAfterCast) - static_cast<int>(srv.ManaOf(a));
        Check(healed >= 6 && healed <= 10, "대상 체력이 틱마다 1(레벨 3 → 1.0) 안 찼다");
        Check(spent >= 6 && spent <= 10, "시전자 마나가 틱마다 1 안 줄었다(시전 비용 제외)");
    }

    // ② 적은 못 고친다 — create 가 취소한다
    {
        nox::Server srv; mk(srv, false);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, 30, out);
        const uint16_t bHp = srv.HealthOf(b);
        srv.DebugAimAt(a, 1080, 1000);                       // 정답지는 **커서 자리**를 탐색 중심으로 쓴다(Pos2) — 안 겨누면 (0,0) 을 뒤진다
        CastAndWait(srv, a, 35, false, ms, out);
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(35, a), "적인데 GreaterHeal 이 걸렸다");
        Check(srv.HealthOf(b) == bHp, "적을 고쳤다");
    }

    // ③ 시전자가 맞으면 끊긴다(sub_4E6BD0 — 지난 1틱 안에 맞음)
    {
        nox::Server srv; mk(srv, true);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, 30, out);
        srv.DebugAimAt(a, 1080, 1000);                       // 정답지는 **커서 자리**를 탐색 중심으로 쓴다(Pos2) — 안 겨누면 (0,0) 을 뒤진다
        CastAndWait(srv, a, 35, false, ms, out);
        Check(srv.DebugDurActive(35, a), "걸리지도 않았다(시험 전제)");
        srv.Damage(a, 5, out);                               // 시전자가 맞는다
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(35, a), "시전자가 맞았는데 치료 광선이 안 끊겼다");
    }

    // ④ 대상 체력이 가득이면 첫 틱에 끊긴다
    {
        nox::Server srv; mk(srv, true);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t aMana = srv.ManaOf(a);
        srv.DebugAimAt(a, 1080, 1000);                       // 정답지는 **커서 자리**를 탐색 중심으로 쓴다(Pos2) — 안 겨누면 (0,0) 을 뒤진다
        CastAndWait(srv, a, 35, false, ms, out);             // b 는 안 다쳤다
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(35, a), "대상이 멀쩡한데 치료 광선이 안 끊겼다");
        Check(srv.ManaOf(a) >= aMana - 10, "안 고쳐도 되는데 마나를 계속 썼다");
    }

    // ⑤ 시전자 마나가 바닥이면 끊긴다
    {
        nox::Server srv; mk(srv, true);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, 60, out);
        srv.DebugAimAt(a, 1080, 1000);                       // 정답지는 **커서 자리**를 탐색 중심으로 쓴다(Pos2) — 안 겨누면 (0,0) 을 뒤진다
        CastAndWait(srv, a, 35, false, ms, out);
        Check(srv.DebugDurActive(35, a), "걸리지도 않았다(시험 전제)");
        srv.DebugManaSub(a, srv.ManaOf(a));                  // 마나 0
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(35, a), "시전자 마나가 0 인데 치료 광선이 안 끊겼다");
    }
    // ⑥ 시전자가 달리면 끕긴다(버프 8)
    {
        nox::Server srv; mk(srv, true);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.Damage(b, 40, out);
        srv.DebugAimAt(a, 1080, 1000);
        CastAndWait(srv, a, 35, false, ms, out);
        Check(srv.DebugDurActive(35, a), "걸리지도 않았다(시험 전제)");
        srv.DebugApplyEnchant(a, 8, 0, 1);                   // 달리기
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(35, a), "시전자가 달리는데 치료 광선이 안 끕겼다");
    }
    std::printf("  4-b5b(2) — GreaterHeal 아군 치료 광선 `9e 06` · 틱마다 체력 1/마나 1 · 적·가득·맞음·마나 0 이면 끊김\n");
}

// ── 묶음 4-b5b(3) — 마나 빨기(DrainMana 22) ───────────────────────────────
//
// 정답지: spells.go:255 → duration.New(nil, nox_xxx_spellDrainMana_52E210, nil, 0)
//   틱마다 **대상을 새로 찾아**(sub_52E610) 마나를 옮기고(sub_52E450) 빔을 옮겨 단다
void TestDrainMana() {
    const std::string a = "10.8.2.1:8201";
    const std::string b = "10.8.2.2:8202";
    // 계수 0.5 로 두어 이월을 잰다(두 틱에 1) · 둘레는 넉넉히
    auto mk = [&](nox::Server& srv) {
        nox::Gamedata g;
        g.Parse("ManaDrainCoeff = 0.5, 0.5, 0.5, 0.5, 0.5;\nManaDrainRange = 300.0f;\n");
        srv.SetGamedata(g);
        srv.SetSpellDef(22, 10, 0x1000000, { 1, 2 });
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1080, 1000);                      // 둘레 안 · 팀이 없으니 아레나 규칙으로 서로 적
    };
    const uint8_t k9e = static_cast<uint8_t>(nox::Op::FX_DURATION_SPELL);

    // ① 적 마나를 빨아 온다 — 빔 `9e 05`(시전자가 앞) · 두 틱에 1(이월)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(a, 60);                             // 시전자 마나가 가득이면 안 돈다
        CastAndWait(srv, a, 22, false, ms, out);
        Check(srv.DebugDurActive(22, a), "DrainMana 가 안 걸렸다");
        const auto toB = FlattenTo(out, b);
        const auto* ray = FindMsg(toB, k9e);
        Check(ray != nullptr && ray->size() == 7 && (*ray)[1] == 5 &&
              ((*ray)[3] | ((*ray)[4] << 8)) == 728 && ((*ray)[5] | ((*ray)[6] << 8)) == 729,
              "마나 빨기 광선 `9e 05`(시전자가 앞)이 안 나갔다");
        const uint16_t aMana = srv.ManaOf(a), bMana = srv.ManaOf(b);
        for (int t = 0; t < 10; ++t) { ms += 34; srv.Tick(ms, out); }
        const int gained = static_cast<int>(srv.ManaOf(a)) - static_cast<int>(aMana);
        const int lost = static_cast<int>(bMana) - static_cast<int>(srv.ManaOf(b));
        Check(gained >= 4 && gained <= 6, "시전자 마나가 0.5/틱(이월 포함)으로 안 찼다");
        Check(lost == gained, "빨린 만큼과 채워진 만큼이 다르다");
    }

    // ② 시전자 마나가 가득이면 곧바로 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t bMana = srv.ManaOf(b);
        CastAndWait(srv, a, 22, false, ms, out);
        // ⚠ 시전 자체가 마나 10 을 쓰므로 **시전 직후는 가득이 아니다** — 도로 채워야 「가득」 갈래를 잰다
        srv.DebugManaAdd(a, 999);
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(22, a), "시전자 마나가 가득인데 안 끊겼다");
        Check(srv.ManaOf(b) == bMana, "가득인데 남의 마나를 빨았다");
    }

    // ③ 시전자가 맞으면 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(a, 60);
        CastAndWait(srv, a, 22, false, ms, out);
        Check(srv.DebugDurActive(22, a), "걸리지도 않았다(시험 전제)");
        srv.Damage(a, 5, out);
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(22, a), "시전자가 맞았는데 안 끊겼다");
    }

    // ④ 시전자가 건 자리에서 5 넘게 움직이면 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(a, 60);
        CastAndWait(srv, a, 22, false, ms, out);
        Check(srv.DebugDurActive(22, a), "걸리지도 않았다(시험 전제)");
        srv.DebugMoveTo(a, 1040, 1000);                      // 40 이동
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(22, a), "시전자가 자리를 떴는데 안 끊겼다");
    }

    // ⑤ 빨 것이 없으면(적 마나 0) 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(a, 60);
        srv.DebugManaSub(b, srv.ManaOf(b));                  // 대상 마나 0
        CastAndWait(srv, a, 22, false, ms, out);
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(22, a), "빨 대상이 없는데 안 끊겼다");
    }

    // ⑥ 시전자가 달리면 끊긴다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugManaSub(a, 60);
        CastAndWait(srv, a, 22, false, ms, out);
        Check(srv.DebugDurActive(22, a), "걸리지도 않았다(시험 전제)");
        srv.DebugApplyEnchant(a, 8, 0, 1);
        ms += 34; srv.Tick(ms, out);
        Check(!srv.DebugDurActive(22, a), "시전자가 달리는데 안 끊겼다");
    }

    // ⑦ 곁에 **같은 편**만 있으면 못 빤다(적만 고른다) — 아군 마나는 그대로
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugSetTeam(a, 1);
        srv.DebugSetTeam(b, 1);                              // 같은 편 → 적이 아니다
        srv.DebugManaSub(a, 60);
        const uint16_t bMana = srv.ManaOf(b);
        CastAndWait(srv, a, 22, false, ms, out);
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(22, a), "같은 편만 곁에 있는데 안 끊겼다");
        Check(srv.ManaOf(b) == bMana, "같은 편 마나를 빨았다");
    }

    // ⑧ 둘레(ManaDrainRange) 밖이면 못 빤다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugMoveTo(b, 1000 + 500, 1000);                // 둘레 300 밖
        srv.DebugManaSub(a, 60);
        const uint16_t bMana = srv.ManaOf(b);
        CastAndWait(srv, a, 22, false, ms, out);
        for (int t = 0; t < 4; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(!srv.DebugDurActive(22, a), "둘레 밖인데 안 끊겼다");
        Check(srv.ManaOf(b) == bMana, "둘레 밖 사람의 마나를 빨았다");
    }
    std::printf("  4-b5b(3) — DrainMana 적 마나 빨기 광선 `9e 05` · 0.5/틱 이월 · 가득/맞음/움직임/대상없음/달리기/같은편/둘레밖\n");
}

// ── 묶음 4-c1 — 밀기·당기기·땅흔들기 ──────────────────────────────────────
//
// 정답지: castPull_52BFA0 · castPush_52C000 · castEquake_52DE40(+equakeDamage_52DEC0)
//   셋 다 둘레 훑기 한 함수를 쓴다 — mapPushUnitsAround_52E040 / unitsGetInCircle
void TestPushPullQuake() {
    const std::string a = "10.9.1.1:9101";
    const std::string b = "10.9.1.2:9102";
    const std::string c = "10.9.1.3:9103";
    auto mk = [&](nox::Server& srv, uint8_t spell) {
        nox::Gamedata g;
        g.Parse("PushPowerCoeff = 1.0;\nPullPowerCoeff = 1.0;\n"
                "EarthquakeRange = 200.0f;\nEarthquakeDamage = 30, 30, 30, 30, 30;\nEarthquakeJiggle = 50, 50, 50, 50, 50;\n");
        srv.SetGamedata(g);
        srv.SetSpellDef(spell, 10, 0x1000000, { 1, 2 });
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1080, 1000);                      // 시전자 오른쪽 80
    };

    // ① 밀기 — 남이 **멀어지는 쪽**으로 밀린다
    {
        nox::Server srv; mk(srv, 66);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const int bx = srv.XOf(b);
        CastAndWait(srv, a, 66, false, ms, out);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.XOf(b) > bx, "밀기인데 상대가 멀어지지 않았다");
        // ★ 크기도 재야 한다 — 힘을 **×10** 하는 걸 빼먹어도 밀리기는 하니 방향만 보면 안 걸린다
        //   실측: ×10 있으면 39 · 없으면 4 남짓이라 20 이면 깨끗이 갈린다
        Check(srv.XOf(b) - bx >= 20, "밀린 거리가 너무 작다(힘 ×10 이 빠졌나)");
    }

    // ② 당기기 — 남이 **가까워지는 쪽**으로 끌린다(힘이 음수)
    {
        nox::Server srv; mk(srv, 65);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const int bx = srv.XOf(b);
        CastAndWait(srv, a, 65, false, ms, out);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.XOf(b) < bx, "당기기인데 상대가 가까워지지 않았다");
    }

    // ③ 멀수록 약하게 — 가까운 b 가 먼 c 보다 많이 밀린다
    {
        nox::Server srv; mk(srv, 66);
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(c, 1400, 1000);                      // 둘레 600 안이지만 훨씬 멀다
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const int bx = srv.XOf(b), cx = srv.XOf(c);
        CastAndWait(srv, a, 66, false, ms, out);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        const int bMoved = srv.XOf(b) - bx, cMoved = srv.XOf(c) - cx;
        Check(bMoved > 0 && cMoved >= 0, "밀기가 아예 안 먹었다(시험 전제)");
        Check(bMoved > cMoved, "가까운 쪽이 먼 쪽보다 더 밀리지 않았다(거리 비례가 빠짐)");
    }

    // ④ 땅흔들기 — 거리에 반비례하는 피해(종류 11) · 자기는 안 맞음 · 화면 흔들기 `97`
    {
        const std::string d = "10.9.1.4:9104";
        nox::Server srv; mk(srv, 23);
        srv.DebugEnterGame(c, 730, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(c, 1190, 1000);                      // 둘레 200 안이지만 b(80)보다 훨씬 멀다
        srv.DebugEnterGame(d, 731, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(d, 1000 + 400, 1000);                // **둘레 200 밖 · 화면 흔들기 300 도 밖**
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const uint16_t aHp = srv.HealthOf(a), bHp = srv.HealthOf(b), cHp = srv.HealthOf(c), dHp = srv.HealthOf(d);
        CastAndWait(srv, a, 23, false, ms, out);
        const int bLost = static_cast<int>(bHp) - static_cast<int>(srv.HealthOf(b));
        const int cLost = static_cast<int>(cHp) - static_cast<int>(srv.HealthOf(c));
        Check(srv.HealthOf(a) == aHp, "땅흔들기가 시전자를 때렸다");
        Check(bLost > 0, "가까운 사람이 땅흔들기에 안 맞았다");
        Check(bLost > cLost, "먼 사람이 가까운 사람만큼 맞았다(거리 반비례가 빠짐)");
        Check(srv.HealthOf(d) == dHp, "둘레 밖 사람이 땅흔들기에 맞았다");
        // 화면 흔들기 — 둘레 300 **안** 사람에게만 `97 <세기>` 2바이트
        const auto toB = FlattenTo(out, b);
        const auto* jig = FindMsg(toB, static_cast<uint8_t>(nox::Op::FX_JIGGLE));
        Check(jig != nullptr && jig->size() == 2 && (*jig)[1] > 0, "화면 흔들기 `97` 이 안 나갔다");
        const auto toD = FlattenTo(out, d);
        Check(FindMsg(toD, static_cast<uint8_t>(nox::Op::FX_JIGGLE)) == nullptr,
              "둘레 300 밖 사람에게도 화면 흔들기가 갔다");
    }

    // ⑤ 둘레 밖은 안 밀린다
    {
        nox::Server srv; mk(srv, 66);
        srv.DebugMoveTo(b, 1000 + 900, 1000);                // 둘레 600 밖
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        const int bx = srv.XOf(b);
        CastAndWait(srv, a, 66, false, ms, out);
        for (int t = 0; t < 3; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.XOf(b) == bx, "둘레 밖인데 밀렸다");
    }
    std::printf("  4-c1 — 밀기/당기기 방향·거리 비례 · 둘레 밖 안 밀림 · 땅흔들기 거리 반비례 피해·자기 제외·화면 흔들기 `97`\n");
}

// ── 묶음 4-c2a — 독 ───────────────────────────────────────────────────────
//
// 정답지: castPoison_52C720 · activatePoison_4EE7E0 · setSomePoisonData_4EEA90 ·
//   updatePoison_4EE8F0 · removePoison_4EE9D0 · 독 틱(server.go:175~201)
void TestPoison() {
    const std::string a = "10.9.2.1:9201";
    const std::string b = "10.9.2.2:9202";
    auto mk = [&](nox::Server& srv) {
        srv.SetSpellDef(60, 10, 0x1000000, { 1, 2 });        // 독
        srv.SetSpellDef(14, 10, 0x1000000, { 1, 3 });        // 해독 — ⚠ 음소를 달리. 같은 음소를 주면 음소 나무가 덮여 앞 주문 시전이 아예 안 성립한다
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Wizard);
        srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
        srv.DebugMoveTo(a, 1000, 1000);
        srv.DebugMoveTo(b, 1080, 1000);
    };
    auto poisonB = [&](nox::Server& srv, uint32_t& ms, std::vector<nox::OutPacket>& out) {
        srv.DebugAimAt(a, 1080, 1000);
        CastAndWait(srv, a, 60, false, ms, out);
    };

    // ① 독이 얹히고 **상태 비트 1024** 가 나간다(사람은 `da` 가 아니라 `6a` 로 알린다)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        poisonB(srv, ms, out);
        Check(srv.DebugPoisonOf(b) == 3, "독이 세기(3)만큼 안 얹혔다");
        bool poisoned = false;
        for (const auto& m : FlattenTo(out, a)) {
            if (m.size() == 7 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_CLIENT_STATUS) &&
                (m[1] | (m[2] << 8)) == 729 && (m[4] & 0x04) != 0) poisoned = true;   // 1024 = 0x400 → 상태 u32 의 **다섯째** 바이트 비트 2
        }
        Check(poisoned, "독 상태 비트 1024 가 `6a` 로 안 나갔다");
    }

    // ② 시작 뒤 60틱은 안 아프다(유예)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        poisonB(srv, ms, out);
        const uint16_t bHp = srv.HealthOf(b);
        for (int t = 0; t < 50; ++t) { ms += 34; srv.Tick(ms, out); }
        Check(srv.HealthOf(b) == bHp, "독 시작 60틱 안인데 벌써 아팠다");
    }

    // ③ 유예가 지나면 주기적으로 아프다(독 3 → 128>>2 = 32틱마다 1)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        poisonB(srv, ms, out);
        const uint16_t bHp = srv.HealthOf(b);
        for (int t = 0; t < 200; ++t) { ms += 34; srv.Tick(ms, out); }
        const int lost = static_cast<int>(bHp) - static_cast<int>(srv.HealthOf(b));
        Check(lost >= 2, "유예가 지났는데 독이 안 아프다");
        Check(lost <= 10, "독이 너무 자주 아프다(주기 128>>(양−1) 이 빠졌나)");
    }

    // ④ 무적(23)이면 안 걸린다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        srv.DebugApplyEnchant(b, 23, 0, 5);                  // 무적
        poisonB(srv, ms, out);
        Check(srv.DebugPoisonOf(b) == 0, "무적인데 독이 걸렸다");
    }

    // ⑤ 해독이 푼다 — 독이 0 이 되고 상태 비트가 꺼진다
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        poisonB(srv, ms, out);
        Check(srv.DebugPoisonOf(b) > 0, "독이 안 걸렸다(시험 전제)");
        std::vector<nox::OutPacket> o2;
        srv.DebugAimAt(a, 1080, 1000);
        CastAndWait(srv, a, 14, false, ms, o2);              // a 가 b 에게 해독
        Check(srv.DebugPoisonOf(b) == 0, "해독했는데 독이 안 풀렸다");
        bool cleared = false;
        for (const auto& m : FlattenTo(o2, a)) {
            if (m.size() == 7 && m[0] == static_cast<uint8_t>(nox::Op::REPORT_CLIENT_STATUS) &&
                (m[1] | (m[2] << 8)) == 729 && (m[4] & 0x04) == 0) cleared = true;
        }
        Check(cleared, "해독 뒤 독 상태 비트가 안 꺼졌다");
    }

    // ⑥ 감쇠 — 계수기가 0 이 되는 틱에 독이 1 줄어든다(한 칸이 1000틱이라 계수기를 줄여 흉내낸다)
    {
        nox::Server srv; mk(srv);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(ms, out);
        poisonB(srv, ms, out);
        const int before = srv.DebugPoisonOf(b);
        srv.DebugSetPoisonDecay(b, 1);                       // 다음 틱에 0 이 된다
        ms += 34; srv.Tick(ms, out);
        Check(srv.DebugPoisonOf(b) == before - 1, "감쇠 계수기가 0 인데 독이 안 줄었다");
    }
    std::printf("  4-c2a — 독 얹기·상태 비트 1024 · 60틱 유예 · 주기 피해 · 무적이면 안 걸림 · 해독이 풀기 · 감쇠\n");
}

// ── 접속 준비(gameServerReadyMB_4DD180) ─────────────────────────────
//
// 정답지: network_server.go:99 MSG_CLIENT_READY → GAME3_2.c:5592 — 무적 23(5×FPS · 세기 5) · `ef` 직업 배수(아레나 1.0 넷)
//   ★ 없으면 버프가 한 번도 안 온 사람을 클라가 무적 그림(파랑·회색 깜빡임)으로 남겨 둔다(b4b1_vamp_ctrl 관측)
void TestGameServerReady() {
    const std::string a = "10.5.2.1:5201";
    nox::Server srv;
    srv.DebugEnterGame(a, 750);
    srv.DebugImportantReal(a, /*ready=*/false);
    uint32_t ms = 0;
    std::vector<nox::OutPacket> out;
    srv.Tick(ms, out);
    Check(!srv.DebugHasEnchant(a, 23), "준비 전인데 무적 23 이 걸려 있다(시험 전제)");
    const auto ready = WireOf({ static_cast<uint8_t>(nox::Op::CLIENT_READY) });
    srv.OnPacket(a, ready.data(), ready.size(), out);
    Check(srv.DebugHasEnchant(a, 23) && srv.DebugEnchantDur(a, 23) == 150 && srv.DebugEnchantPower(a, 23) == 5,
          "CLIENT_READY 인데 무적 23(150 · 세기 5)이 안 걸렸다");
    std::vector<nox::OutPacket> o;
    ms += 34; srv.Tick(ms, o);
    const uint8_t want[17] = { static_cast<uint8_t>(nox::Op::STAT_MULTIPLIERS), 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f };
    bool found = false;
    for (const auto& p : o) {
        if (p.to != a) continue;
        const auto body = Body(p);
        for (size_t i = 0; i + sizeof(want) <= body.size() && !found; ++i)
            if (std::memcmp(body.data() + i, want, sizeof(want)) == 0) found = true;
    }
    Check(found, "준비 뒤 `ef` 직업 배수(1.0 넷)가 안 실렸다");
    // 두 번째 준비(정품 클라는 두 번 보낸다) — 무적을 끄고 다시 150
    for (int t = 0; t < 10; ++t) { ms += 34; srv.Tick(ms, o); }
    srv.OnPacket(a, ready.data(), ready.size(), o);
    Check(srv.DebugEnchantDur(a, 23) == 150, "두 번째 CLIENT_READY 에 무적이 150 으로 다시 안 걸렸다");
    std::printf("  접속 준비 — CLIENT_READY 무적 23(150·5) · `ef` 직업 배수 · 두 번째 준비 다시 150\n");
}

// ── 팀이 바뀌면 친구 목록 비우기 (2026-09-13) ─────────────────
//
// 정답지: 팀에 넣고 뺄 때 그 사람에게 `36 RESET_FRIENDS`(`sub_4D97E0`) → 친구 비트를 다시 세운다.
// 클라는 목록을 통째로 비우므로 **리셋 뒤에 친구를 다시 알려야** 한다 — 빠지면 제 소환수가 적으로 칠해진다.
void TestResetFriends() {
    nox::Server srv;
    srv.SetFriends(true);
    const std::string a = "10.9.9.3:49003";
    srv.DebugEnterGame(a, 742);
    std::vector<nox::OutPacket> out;
    SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀   // 소환 — 자기 소환수는 친구(⑬)
    uint32_t ms = srv.DebugNowMs();                // 4-a — 시전 틀이 시계를 돌렸다 · 이어서
    auto count = [&](const std::vector<nox::OutPacket>& o, nox::Op op) {
        size_t n = 0;
        for (const auto& m : FlattenTo(o, a)) if (!m.empty() && m[0] == static_cast<uint8_t>(op)) ++n;
        return n;
    };
    {
        // 불려 나온 틱(시전 틀의 마지막 틱) 에 한 번 — SummonImp 의 out 에 있다
        Check(count(out, nox::Op::OBJECT_FRIEND_ADD) == 1, "제 소환수 친구 알림이 한 번 안 나감");
        std::vector<nox::OutPacket> o;
        ms += 34; srv.Tick(ms, o);
        Check(count(o, nox::Op::OBJECT_FRIEND_ADD) == 0, "안 바뀌었는데 친구 알림을 또 보냄");
        Check(count(o, nox::Op::RESET_FRIENDS) == 0, "팀이 그대로인데 36 이 나감");
    }
    {
        srv.DebugSetTeam(a, 1);
        std::vector<nox::OutPacket> o;
        ms += 34; srv.Tick(ms, o);
        Check(count(o, nox::Op::RESET_FRIENDS) == 1, "팀이 바뀌었는데 36 RESET_FRIENDS 가 안 나감");
        Check(count(o, nox::Op::OBJECT_FRIEND_ADD) == 1, "리셋 뒤 친구를 다시 안 알림");
    }
    {
        std::vector<nox::OutPacket> o;
        ms += 34; srv.Tick(ms, o);
        Check(count(o, nox::Op::RESET_FRIENDS) == 0 && count(o, nox::Op::OBJECT_FRIEND_ADD) == 0,
              "팀이 그대로인데 리셋·친구 알림이 되풀이됨");
    }
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

    // ④ 벽 재질(WALL 섹션, 2026-09-13 묶음 2-b) — 읽혔고, 섹션 걸음이 안 밀렸는가.
    //   걸음이 밀렸으면 뒤따르는 THNG 이름 대조(②③)가 먼저 깨진다. 여기선 개수와 막지 않는 재질 수를 찍는다.
    {
        const auto& ws = db.Walls();
        Check(!ws.empty(), "WALL 섹션이 0개 — 벽 재질을 못 읽었다");
        size_t passable = 0;
        for (const auto& w : ws) if (w.flags & 0x2) ++passable;
        const nox::ThingInfo* pl = db.ByWire(713);
        std::printf("  벽 재질 %zu종(막지 않음 0x2 %zu종) · NewPlayer 반지름 %.1f · MASS %.1f\n",
                    ws.size(), passable, pl ? pl->radius : 0.0f, pl ? pl->mass : 0.0f);
    }
    // ⑤ 충돌 셋(2026-09-13 묶음 2-c) — estate 에 있는 종류를 찍는다(판정 없음, 목록 확인용)
    {
        const char* names[] = { "NewPlayer", "Imp", "RedPotion", "CaveRockPillarShort", "CaveRockPillarTall",
                                "TreeForest01", "TreePine01", "WoodBed", "Fireplace", "OgreHutWallSkull4",
                                "WoodenDoor", "ObeliskPrimitive", "FlowersBlueSparse", "BearskinRug" };
        for (const char* nm : names) {
            const uint16_t w = db.WireByName(nm);
            const nox::ThingInfo* ti = w ? db.ByWire(w) : nullptr;
            if (!ti) { std::printf("    [충돌] %-22s 없음\n", nm); continue; }
            std::printf("    [충돌] %-22s 모양 %d r%.1f 상자 %.0fx%.0f 질량 %.1f · COLLIDE %s · FLAGS %s · CLASS %s\n",
                        nm, ti->shapeKind, ti->radius, ti->boxW, ti->boxH, ti->mass,
                        ti->hasCollide ? ti->collideRaw.c_str() : "-", ti->flagsRaw.c_str(), ti->classRaw.c_str());
        }
    }

    // ⑧ 피해 함수(2026-09-13 묶음 3-e) — 사람은 PlayerDamage, 몬스터 표본의 DAMAGE·SUBCLASS 를 찍고 판정
    {
        const nox::ThingInfo* pl = db.ByWire(713);
        Check(pl && pl->damageFn == "PlayerDamage", "NewPlayer DAMAGE 가 PlayerDamage 가 아니다 — DAMAGE 파싱이 틀렸다");
        size_t monsters = 0, withFn = 0;
        std::map<std::string, int> fns;
        for (uint16_t w = 1; w <= db.Count(); ++w) {
            const nox::ThingInfo* ti = db.ByWire(w);
            if (!ti || !ti->isMonster) continue;
            ++monsters;
            if (!ti->damageFn.empty()) { ++withFn; ++fns[ti->damageFn]; }
        }
        std::printf("    [피해] 몬스터 %zu종 중 DAMAGE 있음 %zu —", monsters, withFn);
        for (const auto& kv : fns) std::printf(" %s×%d", kv.first.c_str(), kv.second);
        std::printf("\n");
        for (const char* nm : { "Imp", "Bomber", "GiantLeech", "Skeleton", "StoneGolem", "MechanicalGolem", "FireSprite" }) {
            const uint16_t w = db.WireByName(nm);
            const nox::ThingInfo* ti = w ? db.ByWire(w) : nullptr;
            if (ti) std::printf("    [피해] %-16s DAMAGE %-16s SUBCLASS %s\n", nm, ti->damageFn.c_str(), ti->subclassRaw.c_str());
        }
    }
    // ⑦ 무기 SUBCLASS(2026-09-13 묶음 3-b) — GameBalance kWeapons 의 클래스 비트가 파일과 같은가(libs WeaponClassNames 순서)
    {
        for (const auto& w : nox::kWeapons) {
            const uint16_t wire = db.WireByName(w.name);
            const nox::ThingInfo* ti = wire ? db.ByWire(wire) : nullptr;
            std::printf("    [무기] %-12s 와이어 %u SUBCLASS %s\n", w.name, wire, ti ? ti->subclassRaw.c_str() : "(없음)");
            Check(ti != nullptr, "kWeapons 이름이 thing.bin 에 없다");
            if (ti) Check(nox::WeaponClassBitsOf(ti->subclassRaw) == w.classBits,
                          (std::string("무기 클래스 비트가 thing.bin SUBCLASS 와 다르다 — ") + w.name).c_str());
        }
    }
    // ⑥ 사람 애니 칸 수(2026-09-13 묶음 3-a) — PlayerDraw STAT. 공격 시각이 이 값이다.
    //   ⚠ 55종 전부가 파일에 있다는 보장은 없다 — 주먹 셋과 걷기·달리기가 0 이 아닌지만 판정, 나머지는 찍기만.
    {
        std::printf("  사람 애니 STAT %d개 담음 —", db.PlayerAnimCount());
        for (int i = 0; i < nox::kPlayerAnimTypeCount; ++i) {
            const auto& a = db.PlayerAnim(i);
            if (a.frames) std::printf(" %d:%s=%d/%d", i, nox::PlayerAnimTypeName(i), a.frames, a.hold);
        }
        std::printf("\n");
        for (int i : { 4, 6, 23, 24, 25 })
            Check(db.PlayerAnim(i).frames > 0,
                  (std::string("사람 애니 칸 수가 0 — ") + nox::PlayerAnimTypeName(i)).c_str());
    }
    // ⑨ 주문 정의(2026-09-13 묶음 4-a) — SPEL 칸 전체를 읽는다. 시험 주문 표(TestSpellDefs)가 파일과 한 칸씩 같은가 ·
    //   끝 소리 이름까지 읽혀 섹션이 어긋나지 않았는가(115개 · 뒤 섹션도 읽힘) · 음소 수 0 인 주문 셈
    {
        Check(db.Spells().size() == 115, "SPEL 주문 수가 115 가 아니다 — 칸을 잘못 읽었다");
        for (const auto& t : TestSpellDefs()) {
            const char* nm = nox::SpellName(t.id);
            const nox::SpellRaw* found = nullptr;
            for (const auto& sr : db.Spells()) if (sr.name == nm) found = &sr;
            Check(found != nullptr, (std::string("시험 주문이 thing.bin 에 없다 — ") + nm).c_str());
            if (!found) continue;
            Check(found->mana == t.mana && found->flags == t.flags && found->phonemes == t.phon,
                  (std::string("시험 주문 표가 thing.bin 과 다르다 — ") + nm).c_str());
        }
        bool fireSound = false;
        for (const auto& sr : db.Spells()) if (sr.name == "SPELL_FIREBALL" && !sr.castSound.empty()) fireSound = true;
        Check(fireSound, "파이어볼 시전 소리 이름이 비었다 — 칸 끝을 못 읽었다");
    }

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
    const auto wiz = nox::BuildSnapshotTail(713, CharClass::Wizard, 0x4b17, 0x4b1f);
    const auto war = nox::BuildSnapshotTail(713, CharClass::Warrior, 0x4b17, 0x4b1f);
    const auto con = nox::BuildSnapshotTail(713, CharClass::Conjurer, 0x4b17, 0x4b1f);
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
    const std::vector<uint8_t> built = nox::BuildSnapshotTail(713, nox::CharClass::Wizard, 0x4b17, 0x4b1f);   // 녹음 그 판의 프레임(3-c 정정)
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

    // ★ 2026-09-13 묶음 2-a — 걸음은 **입력이 상태를 바꾸고 틱이 옮긴다**(정답지 ImplA/B).
    //   그래서 입력마다 틱을 한 번 돌린다. 첫 `Tick` 은 시계만 잡는다.
    //   커서는 **몸을 따라** 바라보는 쪽 `aimAhead` 앞에 둔다 — 진짜 클라처럼. 100 안이면 걷기, 밖이면 달리기.
    uint32_t ms = 0;
    auto walkTick = [&](nox::Server& srv, uint8_t facing, std::vector<nox::OutPacket>& out,
                        int aimAhead = 50) {
        const double rad = facing * 2.0 * 3.14159265358979 / 256.0;
        srv.DebugAimAt(a, srv.XOf(a) + static_cast<int>(aimAhead * std::cos(rad)),
                       srv.YOf(a) + static_cast<int>(aimAhead * std::sin(rad)));
        auto pkt = walkPkt(facing);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        ms += 34;
        srv.Tick(ms, out);
    };

    // ① 벽 없음 — 동쪽(0)으로 걸으면 x 가 는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);   // (1000,1000)
        std::vector<nox::OutPacket> out;
        srv.Tick(ms, out);
        for (int i = 0; i < 3; ++i) walkTick(srv, 0, out);
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
        srv.Tick(ms, out);
        for (int i = 0; i < 12; ++i) walkTick(srv, 0, out);   // 벽을 넘으면 x 가 1012 를 훌쩍 넘는다
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
        srv.Tick(ms, out);
        for (int i = 0; i < 12; ++i) walkTick(srv, 224, out);   // 북동 — x+ 는 막히고 y- 는 열려 있다
        Check(srv.XOf(a) < 1012, "미끄러지다 벽을 뚫었다");
        Check(srv.YOf(a) < y0, "미끄러짐이 없다 — 대각에서 통째로 멈췄다");
    }

    // ④ 속도 — 정답지 적분의 끝 속도 = 힘 ÷ 감쇠. 걷기 3.0/0.5 = 6 · 달리기 ×2 = 12 유닛/틱
    //   커서가 몸에서 100 안이면 걷기, 밖이면 달리기(`object_update.go:150`). 멈추면 잦아든다.
    {
        auto speedAfter = [&](int aimDx, double* stopDist) {
            nox::Server srv;
            srv.DebugEnterGame(a, 728);
            std::vector<nox::OutPacket> out;
            srv.Tick(ms, out);
            for (int i = 0; i < 40; ++i) walkTick(srv, 0, out, aimDx);   // 끝 속도에 닿을 만큼
            const int x0 = srv.XOf(a);
            walkTick(srv, 0, out, aimDx);
            const int v = srv.XOf(a) - x0;
            // 멈춤 — 방향만 보내면(이동 코드 없음) 상태가 가만으로 바뀌고 속도가 잦아든다
            const int xs = srv.XOf(a);
            for (int i = 0; i < 30; ++i) {
                std::vector<uint8_t> p(nox::kWireHeaderSize, 0);
                p.push_back(static_cast<uint8_t>(nox::Op::PLAYER_INPUT));
                p.push_back(5);
                const uint8_t item[5] = { 1, 0, 0, 0, 0 };
                p.insert(p.end(), item, item + 5);
                srv.OnPacket(a, p.data(), p.size(), out);
                ms += 34;
                srv.Tick(ms, out);
            }
            if (stopDist) *stopDist = srv.XOf(a) - xs;
            return v;
        };
        double walkStop = 0, runStop = 0;
        const int walk = speedAfter(50, &walkStop);    // 커서 50 앞 — 걷기
        const int run  = speedAfter(400, &runStop);    // 커서 400 앞 — 달리기
        Check(walk >= 5 && walk <= 7, "걷기 끝 속도가 6 안팎이 아님");
        Check(run >= 11 && run <= 13, "달리기 끝 속도가 12 안팎이 아님");
        // 멈춘 뒤 미끄러짐 = 속도 × 1/(1−0.9^5) 안팎 — 걷기 6 이면 약 15, 영영 가지는 않는다
        //   ⚠ 멈춤 입력 첫 틱에는 아직 힘이 남는다(ImplA 가 ImplB 보다 먼저) — 한 걸음 더 간다.
        Check(walkStop > 0 && walkStop < 30, "멈춘 뒤 안 잦아든다(또는 바로 선다)");
        std::printf("  걷기 물리 — 끝 속도 걷기 %d · 달리기 %d · 멈춘 뒤 미끄러짐 %.0f/%.0f\n",
                    walk, run, walkStop, runStop);
    }

    std::printf("  벽 충돌 — 통과·막힘·미끄러짐\n");
}

// ── 벽 모양 — 대각선 선분 (2026-09-13 묶음 2-b) ────────────────
//
// 정답지 벽은 칸이 아니라 칸 안 대각선이다. 방향 1 = 왼쪽 대각선(\) 0~23 전체.
// 칸 (44,43) 원점 = (1012, 989) → 선분 (1012,989)→(1035,1012).
// 동쪽으로 y=1000 을 달리면 선과 x≈1023 에서 만난다 — 멈추는 중심은 선에서 반지름 10 떨어진 곳,
// 즉 **칸 네모 경계(1012−10=1002)보다 안쪽**까지 들어간다. 선을 따라 남동으로 미끄러진다.
void TestWallShape() {
    const std::string a = "10.0.0.1:1000";
    auto walkPkt = [](uint8_t facing) {
        std::vector<uint8_t> p(nox::kWireHeaderSize, 0);
        p.push_back(static_cast<uint8_t>(nox::Op::PLAYER_INPUT));
        p.push_back(10);
        const uint8_t items[10] = { 1, 0, 0, 0, facing, 2, 0, 0, 0, facing };
        p.insert(p.end(), items, items + 10);
        return p;
    };
    nox::Server srv;
    srv.DebugEnterGame(a, 728);
    srv.DebugMoveTo(a, 960, 1000);
    //   ⚠ 한 칸짜리면 선 끝까지 미끄러져 지나간다(처음에 그렇게 짜서 「뚫었다」로 오판) — 칸 (x, x−1) 을 스무 칸 잇는다.
    std::vector<nox::Server::WallTileIn> tiles;
    for (int gx = 40; gx < 60; ++gx) {
        nox::Server::WallTileIn w; w.x = static_cast<uint8_t>(gx); w.y = static_cast<uint8_t>(gx - 1); w.dir = 1; w.tile = 0;
        tiles.push_back(w);
    }
    srv.SetWallTiles(tiles);

    // ① 선 추적 — 선을 가로지르는 선분은 막히고, 선과 나란한 선분은 안 막힌다
    Check(!srv.DebugTraceRay(1000.0f, 1000.0f, 1030.0f, 1000.0f), "대각선 벽을 가로지르는 선이 안 막혔다");
    Check(srv.DebugTraceRay(1000.0f, 1000.0f, 1000.0f, 1030.0f), "벽 없는 쪽 선이 막혔다");
    Check(srv.DebugTraceRay(1014.0f, 986.0f, 1037.0f, 1009.0f), "대각선과 나란한 선이 막혔다");

    // ② 동쪽으로 달리기 — 선에서 멈추고 선 따라 남동으로 미끄러진다
    uint32_t ms = 0;
    std::vector<nox::OutPacket> out;
    srv.Tick(ms, out);
    int maxX = 0;
    for (int i = 0; i < 40; ++i) {
        srv.DebugAimAt(a, srv.XOf(a) + 300, srv.YOf(a));
        auto pkt = walkPkt(0);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        ms += 34;
        srv.Tick(ms, out);
        maxX = std::max(maxX, srv.XOf(a));
    }
    const int x = srv.XOf(a), y = srv.YOf(a);
    // 선 방정식 y − 989 = x − 1012 → 중심이 선의 **왼아래(서남) 쪽**에 머물러야 한다: (x−1012) − (y−989) < 0
    Check((x - 1012) - (y - 989) < 0, "대각선 벽을 뚫고 반대편으로 갔다");
    Check(y > 1000, "대각선을 따라 남동으로 미끄러지지 않았다");
    std::printf("  벽 모양 — 대각선 막힘·나란함 통과 · 달린 뒤 (%d,%d) · 가장 멀리 x %d\n", x, y, maxX);
}

// ── 몸끼리 충돌 (2026-09-13 묶음 2-c) ─────────────────────────
//
// 원 대 원(`GAME5.c:7183`): 겹침 × 40 으로 민다. 원 대 상자(`GAME5.c:3465` · 3299): 상자는 45도 돌린 마름모 —
// W=H=20 이면 꼭짓점이 중심에서 20×0.35355×2 ≈ 14.1.
// ★ **멈추지 않고 튕긴다** — 오브젝트 충돌 사각형은 프레임 시작 자리(+56)로 잡혀 한 프레임 늦게 닿고(조금 파고듦),
//   벽(sub_550380)과 달리 벽 쪽 속도를 깎는 단계가 없어 탄성으로 되튄다. 처음에 「반지름 합에서 멈춘다」로 짰다가
//   상자 쪽이 x 998 로 튕겨 나온 것을 보고 바로잡음(디버그 출력으로 겹침 4.09 · 속도 11→−21 확인).
//   그래서 **뚫고 지나가지 않는가**(가장 멀리 간 x)와 **밀리는가**를 본다.
void TestBodyCollision() {
    const std::string a = "10.0.0.1:1000";
    const std::string b = "10.0.0.2:1000";
    auto walkPkt = [](uint8_t facing) {
        std::vector<uint8_t> p(nox::kWireHeaderSize, 0);
        p.push_back(static_cast<uint8_t>(nox::Op::PLAYER_INPUT));
        p.push_back(10);
        const uint8_t items[10] = { 1, 0, 0, 0, facing, 2, 0, 0, 0, facing };
        p.insert(p.end(), items, items + 10);
        return p;
    };
    auto runEast = [&](nox::Server& srv, int ticks) {
        uint32_t ms = 0;
        std::vector<nox::OutPacket> out;
        srv.Tick(ms, out);
        int maxX = 0;
        for (int i = 0; i < ticks; ++i) {
            srv.DebugAimAt(a, srv.XOf(a) + 300, srv.YOf(a));
            auto pkt = walkPkt(0);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 34;
            srv.Tick(ms, out);
            maxX = std::max(maxX, srv.XOf(a));
        }
        return maxX;
    };
    // ① 나무(원 r15, 고정 at 1060) — 파고들어도(한 프레임 늦은 감지 — 가장자리 1045 까지 들어갔다) 중심을 못 넘는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        nox::Server::PhysBody tree;
        tree.x = 1060; tree.y = 1000; tree.shape = 2; tree.r = 15; tree.cls = 0x400000; tree.mass = 1e10f;
        srv.SetObstacles({ tree });
        const int maxX = runEast(srv, 30);
        Check(maxX < 1060, "나무를 뚫고 중심을 넘어갔다");
        Check(srv.XOf(a) < 1060, "나무 너머에 서 있다");
        Check(maxX > 1025, "나무에 닿기도 전에 멈췄다");
        std::printf("  몸 충돌 — 나무(r15 at 1060) 가장 멀리 x %d · 끝 x %d", maxX, srv.XOf(a));
    }
    // ② 오벨리스크(상자 20×20, 고정 at 1060) — 중심이 서쪽 꼭짓점(1045.9)을 넘지 않는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        nox::Server::PhysBody ob;
        ob.x = 1060; ob.y = 1000; ob.shape = 3; ob.cls = 0x400000; ob.mass = 1e10f;
        nox::Server::CalcBox(ob, 20, 20);
        srv.SetObstacles({ ob });
        const int maxX = runEast(srv, 30);
        Check(maxX < 1060, "상자를 뚫고 중심을 넘어갔다");
        Check(srv.XOf(a) < 1060, "상자 너머에 서 있다");
        Check(maxX > 1025, "상자에 닿기도 전에 멈췄다");
        std::printf(" · 상자(20×20) 가장 멀리 x %d · 끝 x %d", maxX, srv.XOf(a));
    }
    // ③ 서 있는 사람을 밀면 그 사람이 밀려난다(둘 다 움직이는 몸)
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1040, 1000);
        runEast(srv, 30);
        Check(srv.XOf(b) > 1040, "서 있는 사람이 안 밀렸다");
        Check(srv.XOf(b) - srv.XOf(a) >= 15, "두 몸이 겹쳐 지나갔다");
        std::printf(" · 밀린 사람 x %d (민 사람 %d)\n", srv.XOf(b), srv.XOf(a));
    }
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
    //   ⚠ 2026-09-13 묶음 3-a — 주먹은 애니가 끝나는 틱에 닿고(시험 서버는 칸 표가 비어 다음 틱) · 같은 자리면 방향이 없어 안 닿는다.
    {
        std::vector<nox::OutPacket> out;
        srv.DebugMoveTo(b, 1025, 1000);
        auto pkt = InputPacket(6);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        for (uint32_t ms = 0; ms <= 33 * 4; ms += 33) srv.Tick(ms, out);
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000, 1100);   // 바라보는 쪽(+x)이 아니라 옆(+y)이다
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "옆에 비켜 있는데 맞음");
        Check(srv.hits() == 0, "안 맞았는데 맞은 것으로 셈");
    }

    // ② 사거리를 넘으면 사라진다
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1000 + static_cast<int>(nox::kSpellRange) + 100, 1000);
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "사거리 밖인데 맞음");
        Check(srv.flying() == 0, "사거리를 넘었는데 안 사라짐");
    }

    // ③ 쏜 사람은 자기 것에 안 맞는다 — 자기 자리에서 나가니 이게 없으면 늘 자폭한다
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth, "쏜 사람이 자기 것에 맞음");
    }

    // ④ 가는 도중에는 아직 안 맞는다 — 한 틱만 돌려 본다
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1180, 1000);   // 한 틱(26)으로는 못 닿는 거리
        std::vector<nox::OutPacket> out;
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
        srv.Tick(0, out);
        srv.Tick(33, out);                // 딱 한 틱
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "한 틱 만에 180 을 날아가 맞음");
        Check(srv.flying() == 1, "가는 도중인데 사라짐");
        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) < nox::kDefaultMaxHealth, "끝까지 날아가도 안 맞음");
    }

    // ⑤ 바라보는 쪽을 바꾸면 그쪽으로 날아간다
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
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
        // ⚠ 2026-09-13 묶음 3-a — 방향은 틱의 ImplB 에서 갈아 끼운다(정답지 조작 목록 순서) → 한 틱 돌린 뒤 쏜다
        srv.Tick(0, out);
        srv.Tick(33, out);
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
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
    nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);
    srv.DebugMoveTo(b, 1600, 1000);   // 멀리 둬서 날아가는 동안 안 맞게

    std::vector<nox::OutPacket> out;
    { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, false, castMs, out); }   // 4-a 시전 틀
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
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀   // 79 = SPELL_SUMMON_BOMBER
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
            for (const auto& e : m.objects) if (e.type == 1328) seen = true;   // Imp(4-a)
        }
        Check(seen, "불려 나온 것(1328 Imp)이 상태 갱신에 안 실림");
    }

    // ★★ ② **부른 사람은 안 때린다** — 이게 없으면 부르자마자 자기가 맞는다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
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
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.DebugMoveTo(b, 1300, 1300);   // 4-a — 시전 틀 동안 틱이 돈다. 같은 자리면 몸끼리 밀려(2-c) 부른 자리가 틀어진다
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
        Check(srv.pets() == 1, "안 불려 나옴");
        double petX = 0, petY = 0;
        srv.DebugPetXY(0, petX, petY);

        // b 를 소환수 옆에 세운다 (a 는 (1000,1000), 소환수는 그 앞 40)
        // ⚠ 2026-09-13 묶음 3-a — 주먹은 몸이 있는 것만 고르고(sub_5386A0) 같은 자리면 방향이 없어 안 닿는다.
        //   시험 서버는 thing.bin 을 안 읽으니 몸을 준다(반지름 10 원 · 몬스터) · b 는 소환수 서쪽 25 에서 +x 를 본다.
        {
            nox::Server::PhysBody body;
            body.shape = 2; body.r = 10.0f; body.cls = 0x2;
            srv.SetTypeBody(1328, body);   // 4-a — 임프
        }
        srv.DebugMoveTo(b, static_cast<int>(petX) - 25, static_cast<int>(petY));
        srv.DebugMoveTo(a, 500, static_cast<int>(petY));   // 부른 사람은 비킨다 — b 와 15 로 겹치면 몸끼리 튕겨 b 가 날아간다(2-c)

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
            for (const auto& e : m.objects) if (e.type == 1328) still = true;
        }
        Check(!still, "죽은 소환수가 아직도 상태 갱신에 실림");
    }

    // ★★ ② 부른 사람의 공격에는 안 맞는다 (근거 없이 정한 규칙이지만 그대로 도는지)
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        std::vector<nox::OutPacket> out;
        srv.DebugMoveTo(b, 1300, 1300);   // 4-a — 시전 틀 동안 겹쳐 밀리지 않게
        SummonImp(srv, a, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀
        double petX = 0, petY = 0;
        srv.DebugPetXY(0, petX, petY);
        // b 를 소환수 뒤쪽에 세우고 소환수 쪽(+x)으로 쏘게 한다
        srv.DebugMoveTo(b, static_cast<int>(petX) - 140, static_cast<int>(petY));
        srv.DebugMoveTo(a, 500, 500);
        // b 의 바라보는 쪽은 0(+x) 이라 소환수(1040,1000) 를 지나간다
        { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, b, 27, false, castMs, out); }   // 4-a 시전 틀
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1025, 1000);   // 겹치면 몸끼리 튕긴다(묶음 2-c) — TestMelee 주석
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.DebugEnterGame(a, 728);
        std::vector<nox::OutPacket> out;
        int guard = 0;
        while (srv.IsAlive(a) && guard++ < 20) {
            { uint32_t castMs = srv.DebugNowMs(); CastAndWait(srv, a, 27, /*onSelf=*/true, castMs, out); }   // 4-a 시전 틀
        }
        Check(!srv.IsAlive(a), "자기 주문으로 안 죽음");
        Check(srv.KillsOf(a) == 0, "자기를 죽이고 점수를 얻음");
        Check(srv.DeathsOf(a) == 1, "죽은 수가 안 오름");
    }

    // ③ 한도에 닿으면 판이 끝나고 **0x58 이 나간다**
    //   ⚠ 3-c — 되살아나면 PlayerStart 로 옮겨진다(시험 서버엔 없어 2000,2000). 자동 부활을 켜고 살아나면 옆으로 되돌려 세운다.
    {
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
        srv.SetAutoRespawn(true);
        srv.DebugEnterGame(a, 728);
        srv.DebugEnterGame(b, 729);
        srv.DebugMoveTo(b, 1025, 1000);   // 겹치면 몸끼리 튕긴다(묶음 2-c) — TestMelee 주석
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
            if (srv.IsAlive(b) && srv.XOf(b) != 1025) srv.DebugMoveTo(b, 1025, 1000);
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
        nox::Server srv; RegAllSpells(srv);   // 4-a 주문 정의
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

    // ① 판을 차리면 **스테이지 1** 몬스터가 서고 화면에도 실린다
    //   ★ 스테이지 1 = 거머리(1358) 2마리 (GameBalance.h kQuestStages).
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.StartQuest(0, 1000, 1000);                // count 는 이제 안 쓴다
        Check(srv.pets() == 2, "스테이지 1 몬스터가 둘이 아님");
        Check(srv.QuestLeft() == 2, "남은 수가 둘이 아님");
        Check(srv.QuestStage() == 0, "첫 스테이지가 0 이 아님");

        std::vector<nox::OutPacket> out;
        srv.Tick(0, out);
        out.clear();
        srv.Tick(33, out);
        int seen = 0, players = 0, streams = 0;
        for (const auto& p : out) {
            auto body = Body(p);
            if (body.empty() || body[0] != static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) continue;
            ++streams;
            seen += CountFirstSight(body, 1358);              // GiantLeech (스테이지 1)
            players += CountFirstSight(body, nox::kPlayerType);
        }
        Check(streams > 0, "0xa4 가 아예 안 나감");
        Check(seen == 2, "몬스터 둘이 상태 갱신에 다 안 실림");
        Check(players == 1, "사람이 상태 갱신에서 사라짐");
    }

    // ★★ ② 사람이 **주인 없는 몬스터를 때릴 수 있다** (근접으로)
    //   ★ 스테이지 1(거머리 2)을 근접으로 잡는다. 한 마리 잡으면 아직 남아 스폰이 안 온다.
    {
        nox::Server srv;
        // ⚠ 3-c — 위저드는 거머리에게 먼저 죽고, 이제 클릭 없이는 안 살아난다. 롱소드 워리어로(방패로 앞을 막고 두 대에 잡음)
        srv.SetPlayerAnimFrames(28, 8, 0);
        srv.SetPlayerAnimFrames(40, 6, 1);
        srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
        srv.StartQuest(0, 1000, 1000);
        // 몬스터가 스폰되는 원 위 첫 자리로 붙는다 (i=0 → center + spread·(cos0,sin0))
        // ⚠ 2026-09-13 묶음 3-a — 몸을 주고(시험 서버는 thing.bin 을 안 읽음) 서쪽 25 에 서서 +x 를 본다(같은 자리면 안 닿는다).
        {
            nox::Server::PhysBody body;
            body.shape = 2; body.r = 10.0f; body.cls = 0x2;
            srv.SetTypeBody(1358, body);
        }
        srv.DebugMoveTo(a, 1000 + static_cast<int>(nox::kQuestSpread) - 25, 1000);
        std::vector<nox::OutPacket> out;
        uint32_t ms = 0;
        srv.Tick(0, out);
        const unsigned long long before = srv.petsKilled();
        int guard = 0;
        while (srv.petsKilled() == before && guard++ < 60) {
            auto pkt = InputPacket(6);
            srv.OnPacket(a, pkt.data(), pkt.size(), out);
            ms += 33 * (nox::kMeleeCooldownTicks + 1);
            srv.Tick(ms, out);
        }
        Check(srv.petsKilled() == before + 1, "사람이 몬스터를 못 때린다(죽은 수 안 오름)");
    }

    // ③ ★★ **스테이지가 오른다** — 한 웨이브를 다 잡으면 다음 웨이브가 밀려오고,
    //   마지막 스테이지를 잡아야 판이 끝난다. 위치·근접에 안 기대고 헬퍼로 잡는다.
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        srv.StartQuest(0, 1000, 1000);
        std::vector<nox::OutPacket> warm;
        srv.Tick(0, warm);

        int lastStage = -1, stageChanges = 0;
        int guard = 0;
        std::vector<nox::OutPacket> all;
        while (!srv.RoundOver() && guard++ < 200) {
            if (srv.QuestStage() != lastStage) { ++stageChanges; lastStage = srv.QuestStage(); }
            std::vector<nox::OutPacket> out;
            srv.DebugKillOneWild(out);          // 이 웨이브의 몬스터를 하나씩
            all.insert(all.end(), out.begin(), out.end());
        }
        Check(srv.RoundOver(), "다섯 스테이지를 다 잡아도 판이 안 끝남");
        // 스테이지가 다섯이니 적어도 다섯 번 바뀌었어야 한다
        Check(stageChanges >= nox::kQuestStageCount,
              "스테이지가 안 올랐다 — 웨이브가 한 번에 다 나왔다");

        // 스테이지 알림(0xa8)이 여러 번 나갔다
        int notices = 0;
        for (const auto& p : all) {
            auto body = Body(p);
            if (!body.empty() && body[0] == static_cast<uint8_t>(nox::Op::TEXT_MESSAGE)) {
                Check(body.size() == 11u + body[8], "0xa8 길이가 머리10+글자와 안 맞음");
                ++notices;
            }
        }
        Check(notices >= 2, "스테이지 알림이 두 번도 안 나감");
    }

    // ★★ ④ **소환수는 퀘스트 셈에 안 든다** — 주인이 있으니 맵 몬스터가 아니다
    {
        nox::Server srv;
        srv.DebugEnterGame(a, 728);
        const std::string b = "10.0.0.2:1000";
        srv.DebugEnterGame(b, 729);
        srv.StartQuest(2, 5000, 5000);          // 몬스터는 멀리 둔다
        std::vector<nox::OutPacket> out;
        SummonImp(srv, b, out);   // 4-a — 79(Bomber)는 thing.bin 에 주문 정의가 없어 임프(87) · 컨저러 · 시전 틀   // b 가 하나 부른다
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
    TestRespawnFaithful();
    TestBroadcastToOthers();
    TestAoiSightExit();
    TestFacingOnWire();
    TestSpellMana();
    TestManaSpend();
    TestAppearanceAndFx();
    TestPropsAndInvuln();
    TestRoundFlow();
    TestAim();
    TestChat();
    TestTimerAskAndClose();
    TestPickup();
    TestUseAndEquip();
    TestUseFromFile();
    TestSpellDamage();
    TestEnemyTo();
    TestDirtyUnitHealth();
    TestEnchantInvuln();
    TestMelee();
    TestWeaponAttack();
    TestDamageHandler();
    TestShieldBlock();
    TestThingDb();
    TestMapSections();
    TestWallCollision();
    TestWallShape();
    TestBodyCollision();
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
    TestImportantQueue();
    TestKillArmorCollide();
    TestCastPipeline();
    TestSpellFly();
    TestDurSpells();
    TestBuffs();
    TestDamageBuffs();
    TestBuffSpells();
    TestMoveBuffs();
    TestDebuffSpells();
    TestGameServerReady();
    TestReportFramework();
    TestHealSpells();
    TestChannelLife();
    TestGreaterHeal();
    TestDrainMana();
    TestPushPullQuake();
    TestPoison();
    TestResetFriends();
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
