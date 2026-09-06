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
#include "nox/Server.h"
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
    TestDeathFlow();
    TestSpellDamage();
    TestMelee();
    TestProjectile();
    TestOpcodeTable();
    TestSpellNames();
    TestBounds();

    std::printf("\n%s\n", g_failed == 0 ? "전부 통과" : "실패 있음");
    return g_failed == 0 ? 0 : 1;
}
