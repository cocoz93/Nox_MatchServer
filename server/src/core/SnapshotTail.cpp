// SnapshotTail.cpp — 첫 세계 스냅샷의 **cc 꼬리부(381B)** 를 필드에서 조립한다. P5-c-1.
//
// 스냅샷 543B = `27 시각` + `a4 세계 갱신` + **이 꼬리** 다.
// 앞 둘은 서버의 세계 상태로 만드는 것이고(P4), 꼬리는 사람 하나를 세우는
// 고정 절차라 조립할 수 있다. 절차(2026-09-10 실측 절단, SNAP_M_warrior_ctf):
//
//   cc(e4 00 01)×2 · cc(ec 01) · cc(6a) · cc(4e)      — 페이드·전송률·상태·배움
//   cc(ea) · cc(b1 60B)                                — 그린 것 잊기 · 새 판(estate)
//   낱 e4 01 01 · cc(e4 01 01)                         — 페이드 인
//   cc(dd)·낱 de                                       — 최대 체력 75 · 마나 150
//   cc(e9 …)                                           — 살려내기
//   [cc(4c)·cc(60)]×3 · cc(4c)·cc(44)·cc(60)           — 장비 넷 (1132·1133·1134·1145)
//   cc(6a) · 낱 ef(배수 1.0×4)                          — 상태 · 스탯 배수
//   cc(b3)×4 · 낱 67 · cc(b3) · cc(5a) · 낱 49          — 오벨리스크·수정·마법부여·방어
//   cc(dd)·낱 de+48+72 · cc(43)                        — 총량 재확인 · 능력치 · 현재 체력
//
// ★ 검증 = proto_test TestSnapshotTail — 같은 번호(713)로 조립하면 실측 381B 와
//   **전수 일치**. 번호를 바꾸면 그 자리들만 달라진다.
// ⚠ 낱 값들(장비 색·오벨리스크 대상·67/49/72 의 몸)은 **뜻 미상** — 실측 그대로 싣는다.
#include "nox/JoinBundle.h"

#include <cstring>

namespace nox {
namespace {

inline void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// cc 껍데기 하나를 붙인다 — 번호는 부르는 쪽이 센다.
void PushSeq(std::vector<uint8_t>& m, uint16_t& seq, const uint8_t* body, size_t n) {
    m.push_back(0xcc);
    m.push_back(static_cast<uint8_t>(seq));
    m.push_back(static_cast<uint8_t>(seq >> 8));
    m.push_back(static_cast<uint8_t>(n));
    m.insert(m.end(), body, body + n);
    ++seq;
}
void PushRaw(std::vector<uint8_t>& m, const uint8_t* body, size_t n) {
    m.insert(m.end(), body, body + n);
}

}  // namespace

std::vector<uint8_t> BuildSnapshotTail(uint16_t netCode, CharClass cls) {
    std::vector<uint8_t> m;
    m.reserve(400);
    uint16_t seq = 0;
    const uint8_t lo = static_cast<uint8_t>(netCode);
    const uint8_t hi = static_cast<uint8_t>(netCode >> 8);

    // 페이드 준비(0) 두 번 — 실측이 정확히 두 번이다. 둘 다 순번 0.
    {
        const uint8_t e4[3] = { 0xe4, 0x00, 0x01 };
        uint16_t z = 0;
        PushSeq(m, z, e4, 3);
        z = 0;
        PushSeq(m, z, e4, 3);
        const uint8_t ec[2] = { 0xec, 0x01 };
        z = 0;
        PushSeq(m, z, ec, 2);
        uint8_t st[7] = { 0x6a, lo, hi, 0, 0, 0, 0 };
        z = 0;
        PushSeq(m, z, st, 7);
        uint8_t le[11] = { 0x4e, lo, hi, 0, 0, 0, 0, 0, 0, 0, 0 };
        z = 0;
        PushSeq(m, z, le, 11);
    }
    // 여기부터 순번이 1·2 로 오른다 (실측 — 위 다섯은 전부 #0)
    seq = 1;
    {
        const uint8_t ea[5] = { 0xea, 0x17, 0x4b, 0x00, 0x00 };
        PushSeq(m, seq, ea, 5);
        // b1 — 새 판 알림. 01 + 맵이름 "estate"(13B 칸) + 방이름 "r Game"? …
        //   실측 원바이트 그대로. "estate" 뒤 채움과 "r Game" 의 정체는 미상 —
        //   (b0 의 "User Game" 에서 앞 셋이 잘려 보이는 꼴이지만 확증 없음)
        const uint8_t b1[60] = {
            0xb1, 0x01, 0x65, 0x73, 0x74, 0x61, 0x74, 0x65, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x72, 0x20, 0x47, 0x61, 0x6d, 0x65, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x07, 0x21, 0x0f, 0x00, 0x00, 0x00 };
        PushSeq(m, seq, b1, 60);
    }
    // 페이드 인 — 낱으로 하나, cc(#0)로 하나. 실측 순서 그대로.
    {
        const uint8_t e4in[3] = { 0xe4, 0x01, 0x01 };
        PushRaw(m, e4in, 3);
        uint16_t z = 0;
        PushSeq(m, z, e4in, 3);
    }
    // 최대 체력·마나 — **직업이 정한다** (2026-09-10 정본화).
    //   전에는 75·150 이 박혀 있었다. 그 값은 「기본값」이 아니라 **위저드의 아레나
    //   최대치**였다(⭕ gamedata.bin `ARENA WizardMaxHealth/Mana`) — 녹음의 주인공이
    //   위저드였을 뿐이다. 그래서 워리어가 정본 150 대신 75 로 놀았다.
    const uint16_t hp = static_cast<uint16_t>(HealthOnline(cls));
    const uint16_t mp = static_cast<uint16_t>(ManaOnline(cls));
    const uint8_t hpLo = static_cast<uint8_t>(hp),  hpHi = static_cast<uint8_t>(hp >> 8);
    const uint8_t mpLo = static_cast<uint8_t>(mp),  mpHi = static_cast<uint8_t>(mp >> 8);
    {
        uint8_t dd[7] = { 0xdd, lo, hi, hpLo, hpHi, hpLo, hpHi };
        seq = 3;
        PushSeq(m, seq, dd, 7);
        uint8_t de[7] = { 0xde, lo, hi, mpLo, mpHi, mpLo, mpHi };
        PushRaw(m, de, 7);
    }
    // 살려내기 — 체력 자리가 여기에도 있다
    {
        uint8_t e9[9] = { 0xe9, lo, hi, 0x1f, hpLo, hpHi, 0x00, 0xff, 0x01 };
        PushSeq(m, seq, e9, 9);   // #4
    }
    // 기본 장비 — **직업마다 다르다** ◑ (`GameBalance.h` LoadoutFor 주석).
    //   워리어 롱소드+나무방패+바지+신발(셔츠 없음) · 위저드 로브+평상복 셋 ·
    //   컨저러 평상복 셋(무기 없음).
    // ★ 내구도(`0x44`)는 **`DURABILITY > 0` 인 것만** 보낸다 — 녹음이 로브 하나에만
    //   보낸 까닭이 이것이다(평상복 셋은 modifier.bin 에서 DURABILITY = 0).
    //   로브 값 325 가 녹음의 `0x0145` 와 정확히 같다.
    // ⚠ 색 4바이트는 뜻 미상이다. 녹음에서 본 것은 그대로 쓰고, 못 본 것(방패·검)은
    //   흰색(ff ff ff ff)으로 둔다.
    {
        struct GearItem { uint16_t type; uint16_t durability; uint8_t c[4]; };
        static const GearItem kShirt    = { kItemStreetShirt,      0, { 0xff, 0xba, 0xc6, 0xff } };
        static const GearItem kPants    = { kItemStreetPants,      0, { 0xff, 0xbf, 0xff, 0xff } };
        static const GearItem kSneakers = { kItemStreetSneakers,   0, { 0xb9, 0xca, 0xff, 0xff } };
        static const GearItem kRobe     = { kItemWizardRobe,     325, { 0xff, 0xff, 0xff, 0xff } };
        static const GearItem kShield   = { kItemWoodenShield,   200, { 0xff, 0xff, 0xff, 0xff } };
        static const GearItem kSword    = { kItemLongsword,      180, { 0xff, 0xff, 0xff, 0xff } };

        const GearItem* gear[4] = {};
        int nGear = 0;
        switch (cls) {
            case CharClass::Wizard:
                gear[0] = &kShirt; gear[1] = &kPants; gear[2] = &kSneakers; gear[3] = &kRobe;
                nGear = 4;
                break;
            case CharClass::Conjurer:
                gear[0] = &kShirt; gear[1] = &kPants; gear[2] = &kSneakers;
                nGear = 3;
                break;
            case CharClass::Warrior:
            default:
                gear[0] = &kPants; gear[1] = &kSneakers; gear[2] = &kShield; gear[3] = &kSword;
                nGear = 4;
                break;
        }
        uint16_t itemCode = netCode;   // 장비 번호 = 사람 번호 +1 부터 (다섯 칸 규칙)
        for (int g = 0; g < nGear; ++g) {
            const GearItem& it = *gear[g];
            ++itemCode;
            uint8_t pk[9] = { 0x4c, static_cast<uint8_t>(itemCode),
                              static_cast<uint8_t>(itemCode >> 8),
                              static_cast<uint8_t>(it.type),
                              static_cast<uint8_t>(it.type >> 8),
                              it.c[0], it.c[1], it.c[2], it.c[3] };
            PushSeq(m, seq, pk, 9);
            if (it.durability > 0) {
                const uint8_t dLo = static_cast<uint8_t>(it.durability);
                const uint8_t dHi = static_cast<uint8_t>(it.durability >> 8);
                uint8_t dur[7] = { 0x44, static_cast<uint8_t>(itemCode),
                                   static_cast<uint8_t>(itemCode >> 8), dLo, dHi, dLo, dHi };
                PushSeq(m, seq, dur, 7);
            }
            uint8_t eq[3] = { 0x60, static_cast<uint8_t>(itemCode),
                              static_cast<uint8_t>(itemCode >> 8) };
            PushSeq(m, seq, eq, 3);
        }
    }
    // 상태 재확인 + 스탯 배수(전부 1.0)
    {
        uint8_t st[7] = { 0x6a, lo, hi, 0, 0, 0, 0 };
        PushSeq(m, seq, st, 7);   // #14
        const uint8_t ef[17] = { 0xef, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f,
                                 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f };
        PushRaw(m, ef, 17);
    }
    // 오벨리스크 충전 넷 + 수정(67) + 하나 더 + 마법부여(5a) + 방어값(49)
    //   ⚠ 대상 번호(0x8953…)·값 전부 **estate 맵의 실측** — 다른 맵이면 다르다.
    {
        const uint8_t b3s[5][4] = { { 0xb3, 0x53, 0x89, 0x32 }, { 0xb3, 0x54, 0x89, 0x32 },
                                    { 0xb3, 0x51, 0x89, 0x32 }, { 0xb3, 0x52, 0x89, 0x32 },
                                    { 0xb3, 0x4b, 0x80, 0x32 } };
        for (int i = 0; i < 4; ++i) PushSeq(m, seq, b3s[i], 4);   // #15~18
        const uint8_t m67[7] = { 0x67, 0x8b, 0x02, 0xff, 0xff, 0x9c, 0xff };
        PushRaw(m, m67, 7);
        PushSeq(m, seq, b3s[4], 4);   // #19
        uint8_t en[7] = { 0x5a, lo, hi, 0x00, 0x00, 0x80, 0x00 };
        PushSeq(m, seq, en, 7);       // #20
        const uint8_t ar[5] = { 0x49, 0x20, 0x85, 0x6b, 0x3e };
        PushRaw(m, ar, 5);
    }
    // 총량 재확인 + 능력치(48) + 72 + 현재 체력(43) — 여기도 직업 값으로 간다
    {
        uint8_t dd[7] = { 0xdd, lo, hi, hpLo, hpHi, hpLo, hpHi };
        PushSeq(m, seq, dd, 7);       // #21
        uint8_t de48[21] = { 0xde, lo, hi, mpLo, mpHi, mpLo, mpHi,
                             0x48, lo, hi, hpLo, hpHi, mpLo, mpHi,
                             0x72, 0x06, 0x5c, 0x44, 0x23, 0x00, 0x0a };
        PushRaw(m, de48, 21);
        const uint8_t cur[3] = { 0x43, hpLo, hpHi };
        PushSeq(m, seq, cur, 3);      // #22
    }
    return m;
}

}  // namespace nox
