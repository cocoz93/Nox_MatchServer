// proto_test.cpp — 이식한 메시지 정의가 스스로 앞뒤가 맞는지만 본다.
//
// 여기서 통과한다고 클라가 받아준다는 뜻은 아니다.
// 진짜 검증은 캡처와 대조하는 것이고, 이건 그 전에 오타를 거르는 용도다.
//
// 빌드:
//   cl /std:c++17 /EHsc /I server\include server\tests\proto_test.cpp server\src\Opcode.cpp
#include <cmath>
#include <cstdio>
#include <cstring>
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
    srv.DebugEnterGame(a, 728);
    srv.DebugEnterGame(b, 729);
    // ★ 이제 파이어볼은 **날아간다.** 바라보는 쪽이 기본 0(월드 +x)이므로
    //   맞으려면 b 가 a 의 앞쪽에 있어야 한다. 같은 자리에 두면 안 맞는 게 옳다.
    srv.DebugMoveTo(b, 1100, 1000);

    // ① 파이어볼을 남에게 — 맞는 쪽이 깎여야 한다
    {
        std::vector<nox::OutPacket> out;
        auto pkt = SpellPacket(27, /*onSelf=*/false);
        srv.OnPacket(a, pkt.data(), pkt.size(), out);
        // ★ 쏘자마자는 아무도 안 깎여야 한다 — 날아가는 중이다
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth, "쏘자마자 맞음 (투사체가 안 날아감)");
        Check(srv.flying() == 1, "날아가는 것이 하나가 아님");

        FlyUntilDone(srv, out);
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth - nox::Server::SpellDamage(27),
              "날아간 파이어볼이 안 맞음");
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth, "쏜 쪽이 깎임");
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
        Check(srv.HealthOf(a) == nox::kDefaultMaxHealth - nox::Server::SpellDamage(27),
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
        Check(srv.HealthOf(b) == nox::kDefaultMaxHealth - nox::kMeleeDamage,
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
        Check(srv.HealthOf(b) == before - nox::kMeleeDamage, "쉬는 시간이 지났는데 안 깎임");
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
void TestSnapshotVsMap() {
    static const uint8_t kSnap[543] = {
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
    // 27 xx xx 뒤가 a4 상태 갱신이다
    Check(kSnap[0] == 0x27 && kSnap[3] == 0xa4, "스냅샷 앞머리가 다르다");
    const nox::UpdateMsg um = nox::DecodeUpdate(kSnap + 4, sizeof(kSnap) - 4, nullptr);
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
    const std::vector<uint8_t> built = nox::BuildSnapshotTail(713);
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
        Check(srv.HealthOf(b) == before - nox::kMeleeDamage, "관전을 푼 뒤 클릭이 안 때렸다");
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
            seen += CountFirstSight(body, 1348);              // Bomber
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
    TestSpellDamage();
    TestMelee();
    TestThingDb();
    TestMapSections();
    TestWallCollision();
    TestJoinBundle();
    TestSnapshotTail();
    TestSnapshotVsMap();
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
