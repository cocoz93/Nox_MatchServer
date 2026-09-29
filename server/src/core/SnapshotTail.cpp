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

// 기본 장비의 색 4바이트·내구도 — 아래 스냅샷 꼬리 주석 참고(09-28 B4b 부활 새 장비가 같은 표를 쓰려고 밖으로 뺐다)
struct SnapGearItem { uint16_t type; uint16_t durability; uint8_t c[4]; };
const SnapGearItem kSnapShirt    = { kItemStreetShirt,      0, { 0xff, 0xba, 0xc6, 0xff } };
const SnapGearItem kSnapPants    = { kItemStreetPants,      0, { 0xff, 0xbf, 0xff, 0xff } };
const SnapGearItem kSnapSneakers = { kItemStreetSneakers,   0, { 0xb9, 0xca, 0xff, 0xff } };
const SnapGearItem kSnapRobe     = { kItemWizardRobe,     325, { 0xff, 0xff, 0xff, 0xff } };
const SnapGearItem kSnapShield   = { kItemWoodenShield,   200, { 0xff, 0xff, 0xff, 0xff } };
const SnapGearItem kSnapSword    = { kItemLongsword,      180, { 0xff, 0xff, 0xff, 0xff } };

}  // namespace

bool DefaultGearLook(uint16_t type, uint8_t look[4], uint16_t& durability) {
    for (const SnapGearItem* g : { &kSnapShirt, &kSnapPants, &kSnapSneakers, &kSnapRobe, &kSnapShield, &kSnapSword }) {
        if (g->type != type) continue;
        std::memcpy(look, g->c, 4);
        durability = g->durability;
        return true;
    }
    look[0] = look[1] = look[2] = look[3] = 0xff;
    durability = 0;
    return false;
}

std::vector<uint8_t> BuildSnapshotTail(uint16_t netCode, CharClass cls, uint32_t eaFrame, uint32_t e9Frame,
                                       uint32_t enchantBits, const std::string& mapName) {
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
        // FORGET_DRAWABLES — 프레임 4바이트(녹음 0x4b17 — JoinBundle.h 주석)
        const uint8_t ea[5] = { 0xea, static_cast<uint8_t>(eaFrame), static_cast<uint8_t>(eaFrame >> 8),
                                static_cast<uint8_t>(eaFrame >> 16), static_cast<uint8_t>(eaFrame >> 24) };
        PushSeq(m, seq, ea, 5);
        // b1 — 새 판 알림. 01 + 맵이름(12B 칸) + 방이름 "r Game"? …
        //   클라는 `data+2` 부터 **58바이트를 게임설정 칸에 통째로 복사**한다
        //   (`cdecode.c:2180` `memcpy(v39, data+2, 0x3A)`) — 로비·GUI 가 그걸 읽는다.
        // ★★ 09-20 — **맵 이름이 `"estate"` 로 박혀 있었다.** 녹음이 estate 판이었을 뿐인데
        //   G_Castle·G_Temple 에서도 클라에 「estate」라고 말하고 있었다.
        //   무적 비트(`5a`)와 **같은 꼴의 결함**이다 — 녹음본 값을 사람·판 상태 칸에 박아 둔 것.
        //   ⇒ **지금 맵 이름**을 싣는다. 12바이트 칸이고 남는 자리는 0 이다
        //   ⚠ "r Game" 과 뒤쪽 `ff…`·`07 21 0f` 는 **여전히 뜻 미상**이라 실측 그대로 둔다
        uint8_t b1[60] = {
            0xb1, 0x01, 0x65, 0x73, 0x74, 0x61, 0x74, 0x65, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x72, 0x20, 0x47, 0x61, 0x6d, 0x65, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x07, 0x21, 0x0f, 0x00, 0x00, 0x00 };
        if (!mapName.empty()) {
            std::memset(b1 + 2, 0, 12);
            const size_t n = mapName.size() < 12 ? mapName.size() : 12;
            std::memcpy(b1 + 2, mapName.data(), n);
        }
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
    // 살려내기 — ❌ 정정(2026-09-13 묶음 3-c): 「체력 자리」가 아니라 **프레임 4바이트 · 무기 깃발 · 1**(netSendPlayerRespawn_4EFC30)
    {
        uint8_t e9[9] = { 0xe9, lo, hi, static_cast<uint8_t>(e9Frame), static_cast<uint8_t>(e9Frame >> 8),
                          static_cast<uint8_t>(e9Frame >> 16), static_cast<uint8_t>(e9Frame >> 24), 0xff, 0x01 };
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
        using GearItem = SnapGearItem;
        const GearItem& kShirt = kSnapShirt;
        const GearItem& kPants = kSnapPants;
        const GearItem& kSneakers = kSnapSneakers;
        const GearItem& kRobe = kSnapRobe;
        const GearItem& kShield = kSnapShield;
        const GearItem& kSword = kSnapSword;

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
                // ★ 09-29 — 롱소드 → 방패(정품 녹음 `orig_arena_1851` 18:53:09.486 · `LoadoutFor` 주석)
                gear[0] = &kPants; gear[1] = &kSneakers; gear[2] = &kSword; gear[3] = &kShield;
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
        // ★★ 09-20 — **여기에 녹음본 값 `00 00 80 00`(= 비트 23 무적)을 박아 두고 있었다.**
        //   그 한 줄 때문에 클라가 **접속 내내 사람을 무적으로 보고** 파란 보호막(색 128,128,255)을
        //   계속 그렸다 — 사용자가 「처음 들어가면 파랗게 번쩍거린다」고 짚은 그것이다.
        //   ⚠ 녹음본의 그 값은 **그 판 그 순간의 스폰 보호**였고, 정품은 5초 뒤 `5a` 로 **푼다**.
        //     우리는 바이트만 베끼고 푸는 알림을 안 보내 **영영 무적으로 남았다**
        //     (실측 — `--no-spawn-invuln` 을 켜고 `마법알림 0` 인 판에서도 클라 진단이 `무적 true`).
        //   ⇒ **지금 버프 값을 싣는다.** 무적이 실제로 걸려 있으면 그대로 가고, 아니면 0 이 간다
        uint8_t en[7] = { 0x5a, lo, hi,
                          static_cast<uint8_t>(enchantBits), static_cast<uint8_t>(enchantBits >> 8),
                          static_cast<uint8_t>(enchantBits >> 16), static_cast<uint8_t>(enchantBits >> 24) };
        PushSeq(m, seq, en, 7);       // #20
        // 방어값 `49 <float 4>` — 첫 playerReportAnything 몫(+232 = 0 → +228). ★ 3-d: 직업 값으로 —
        //   녹음 `20 85 6b 3e` 는 위저드 방어값을 **들어온 순서 거꾸로** float 로 더한 값과 한 비트까지 같다(ArmorValueFloat 주석)
        const float armor = ArmorValueFloat(cls);
        uint32_t bits = 0;
        std::memcpy(&bits, &armor, sizeof(bits));
        const uint8_t ar[5] = { 0x49, static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
                                static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24) };
        PushRaw(m, ar, 5);
    }
    // ★★ 09-20 전수 점검 — **「사람 상태」를 담은 칸에 녹음값이 박혀 있으면 안 된다.**
    //   무적 비트가 그렇게 박혀 있어 **접속하는 사람마다 영영 무적**이었다(위 `5a` 주석).
    //   지금 상태:
    //     ⭕ 체력·마나(`dd`·`de`·`48`·`43`) · 방어값(`49`) · 짐·속도·힘·레벨 — **직업 값**으로 간다
    //     ⭕ 버프(`5a`) — **인자로** 간다(09-20 고침)
    //     ➖ 오벨리스크 대상 넷(`b3 …`) — **estate 맵의 번호**다. 다른 맵에서는 없는 번호라
    //        클라가 버린다(해롭지는 않으나 **맞는 값도 아니다**). 맵에서 읽게 되면 그때 고칠 것
    //     ➖ 장비 색 4B · `67` · `72` — **뜻 미상**이라 실측 그대로 둔다
    // 총량 재확인 + 능력치(48) + 72 + 현재 체력(43) — 여기도 직업 값으로 간다
    {
        uint8_t dd[7] = { 0xdd, lo, hi, hpLo, hpHi, hpLo, hpHi };
        PushSeq(m, seq, dd, 7);       // #21
        // `48` 능력치 14B — servSendStats_4D8990(`GAME3_2.c:4201`): 번호 · 최대 체력 · 최대 마나 · 짐 한계(+490) · 속도(+2235) · 힘(+2239) · 레벨(+3684)
        //   ★ 09-14 정정 — 뒤 셋을 녹음 상수(1650·17500·35 = 위저드)로 박아 두었다. 직업 값으로(워리어 3000·21000·125 · 컨저러 1950·18500·55)
        const uint16_t carry = static_cast<uint16_t>(CarryCapacityOnline(cls));
        const uint16_t spd = static_cast<uint16_t>(SpeedOnline(cls));
        const uint16_t str = static_cast<uint16_t>(StrengthOnline(cls));
        uint8_t de48[21] = { 0xde, lo, hi, mpLo, mpHi, mpLo, mpHi,
                             0x48, lo, hi, hpLo, hpHi, mpLo, mpHi,
                             static_cast<uint8_t>(carry), static_cast<uint8_t>(carry >> 8),
                             static_cast<uint8_t>(spd), static_cast<uint8_t>(spd >> 8),
                             static_cast<uint8_t>(str), static_cast<uint8_t>(str >> 8), kMaxLevel };
        PushRaw(m, de48, 21);
        const uint8_t cur[3] = { 0x43, hpLo, hpHi };
        PushSeq(m, seq, cur, 3);      // #22
    }
    return m;
}

}  // namespace nox
