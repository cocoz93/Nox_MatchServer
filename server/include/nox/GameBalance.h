// GameBalance.h — **게임의 숫자는 전부 여기 모은다.** 정본은 게임 데이터 파일이다.
//
// 왜 있나 — 데미지·체력·사거리를 코드 여기저기에 자리표시로 박아 두었는데,
// 2026-09-10 전수검사에서 그 값들이 **게임 파일에 그대로 적혀 있다**는 것이 드러났다
// (`Docs/GAMEDATA_AUDIT.md`). 흩어진 상수를 한자리로 모으고 출처를 붙인다.
//
// ## 등급 — 이 파일의 값은 세 갈래다
//
//     ⭕ 파일    gamedata.bin · modifier.bin · monster.bin · thing.bin 에 적힌 값
//     ◑ 재구현   opennox 소스에서 얻은 **수식**. 원본 exe 디컴파일 파생이라
//                파일 값보다 한 등급 아래다 — 어긋나면 이기는 쪽은 정품 클라
//     ➖ 우리 값  운영 설정(승리 조건)·설계값(시야). 원래 파일에 없는 것
//
// ⚠ 게임 파일을 런타임에 읽지 않고 **여기 옮겨 적는다.** 까닭 둘 —
//   ① 게임이 안 깔린 곳에서도 서버가 서야 한다(대조 장치·CI)
//   ② 옮긴 값이 무엇인지 눈으로 보이는 편이 낫다. 대신 **출처를 줄마다 적는다.**
#pragma once

#include <cstdint>
#include <string>

namespace nox {

// ── 직업 ─────────────────────────────────────────────────────────
//
// 접속 정보 97바이트의 **자리 51**에 온다(`Docs/PROTOCOL.md` 「직업과 레벨」).
// 자리 52 는 레벨(0~10).
enum class CharClass : uint8_t {
    Warrior  = 0,
    Wizard   = 1,
    Conjurer = 2,
};
// ★★★ **직업은 사람 정보 97바이트의 66번 자리다** (2026-09-11 정정).
//
//   ⭕ 근거 — 클라 원본 `libs/noxnet/msg_player_new.go:98-109` 의 `PlayerInfo` —
//
//       PlayerName      string   //  0-49
//       Unk50           uint32   // 50-53
//       Unk54           uint32   // 54-57
//       Unk58           uint32   // 58-61
//       Unk62           uint32   // 62-65
//       **PlayerClass   byte     // 66**
//       IsFemale        byte     // 67
//       Colors                   // 68-87
//
//   ⚠⚠ **전에는 51 을 읽었다.** 51 은 `Unk50` 의 가운데 바이트라 **늘 0** 이었고,
//     0 은 마침 워리어다 — 그래서 **누가 무엇을 골라도 워리어로 접속했다.**
//     워리어는 최대 마나가 0 이므로 마나칸이 비고, 시전이 `cost > s.mana` 에서
//     **카운터도 안 올린 채 조용히 전부 막혔다.**
//     사용자가 겪은 「마나칸 0 · 마나 안 참 · 주문」이 전부 여기서 나왔다.
//     지문 — 화면 캡처에는 `Jack the Conjurer` 가 골라져 있는데 서버는 `마나0/0`.
constexpr size_t kInfoClassAt = 66;
// ⚠ **레벨은 이 구조에 없다.** 전에 52 를 레벨로 읽었는데 그것도 `Unk50` 안이라
//   늘 0 이었다(로그에 `레벨 0` 으로 찍혔다 — 레벨은 최소 1 이라 그 자체가 지문이었다).
//   온라인은 레벨을 안 보고 직업 최대 스탯을 그대로 주므로(`HealthOnline` 주석) 해가 없다.
//   캠페인을 붙일 때 다시 찾는다.
constexpr uint8_t kMaxLevel   = 10;   // gamedata.bin 주석: "class stat maximums (at level 10)"

// ── 직업별 스탯 ⭕ gamedata.bin ───────────────────────────────────
//
// 기본값(레벨 0)은 직업 공통이고, 최대값(레벨 10)이 직업마다 갈린다.
// **아레나 값을 쓴다** — 접두어 없는 줄은 두 모드 공용, `SOLO` 는 캠페인용이다.
//
//     BaseHealth = 20; BaseMana = 78; BaseSpeed = 17250; BaseStrength = 20;
//     ARENA WarriorMaxHealth = 150;  WarriorMaxMana = 0;   WarriorMaxSpeed = 21000;  WarriorMaxStrength = 125;
//     ARENA WizardMaxHealth  = 75;   ARENA WizardMaxMana = 150;   ARENA WizardMaxSpeed = 17500;   WizardMaxStrength = 35;
//     ARENA ConjurerMaxHealth = 100; ARENA ConjurerMaxMana = 125; ARENA ConjurerMaxSpeed = 18500; ConjurerMaxStrength = 55;
constexpr int kBaseHealth   = 20;
constexpr int kBaseMana     = 78;
constexpr int kBaseSpeed    = 17250;
constexpr int kBaseStrength = 20;

struct ClassStats {
    int maxHealth;
    int maxMana;
    int maxSpeed;     // gamedata 눈금(17250~21000). 걸음 계산은 아래 SpeedUnits 참조
    int maxStrength;
};

// 레벨 10·아레나 기준.
constexpr ClassStats kClassMax[3] = {
    /* Warrior  */ { 150,   0, 21000, 125 },
    /* Wizard   */ {  75, 150, 17500,  35 },
    /* Conjurer */ { 100, 125, 18500,  55 },
};

inline const ClassStats& MaxStatsOf(CharClass c) {
    const int i = static_cast<int>(c);
    return kClassMax[(i >= 0 && i < 3) ? i : 0];
}

// ★★★ **온라인은 레벨을 아예 안 본다** ◑ (opennox `nox_xxx_plrReadVals_4EEDC0`,
//   src/legacy/GAME3_3.c:7660). 게임 깃발에 `GameOnline`(0x2000)이 서 있으면
//   **그 직업의 최대 스탯을 그대로** 준다 — 보간이 돌지 않는다.
//   같은 함수가 접속한 사람의 레벨도 10 으로 밀어 둔다(`sub_4EF140`, :7732).
//
//   그래서 우리 서버(멀티)는 **직업만 보면 된다.** 레벨 자리(52)는 기록으로만 읽는다.
//
// ⚠ 검산 — 녹음 속 위저드가 체력 75·마나 150 이었다. 이 규칙대로면 정확히 그 값이다
//   (레벨을 봤다면 그 캐릭터 레벨에 따라 달랐을 것이다).
inline int HealthOnline(CharClass c)   { return MaxStatsOf(c).maxHealth; }
inline int ManaOnline(CharClass c)     { return MaxStatsOf(c).maxMana; }
inline int StrengthOnline(CharClass c) { return MaxStatsOf(c).maxStrength; }
inline int SpeedOnline(CharClass c)    { return MaxStatsOf(c).maxSpeed; }

// ★★ **직업 스탯 배수** ◑ (`server/player.go:167` serverPlayers::init) — 온라인(아레나)은 **전부 1.0**.
//   접속 준비 때 `ef STAT_MULTIPLIERS` 로 클라에 보내는 그 값이다.
//   ⚠⚠ **2026-09-14 정정** — 정답지 전역 `warriorMaxHealth_587000_312784` 따위는 이름과 달리
//     **최대 체력이 아니라 이 배수**다(`legacy/player.go:58` → `Players.Mult.Warrior.Health`).
//     이름만 보고 최대 체력으로 읽어 LesserHeal 회복량을 75~150배 부풀렸다(c4b017b 결함).
//   ⚠ 퀘스트는 `DefaultStatMult` 로 체력 3배(`server/player.go:101`) — 그 모드를 붙이면 여기가 갈린다.
inline float ClassHealthMult(CharClass) { return 1.0f; }
// 마나 쪽 배수 — 같은 표의 다른 칸(`Players.Mult.<직업>.Mana`). 아레나는 마찬가지로 1.0.
//   물약(`usePotion_53EF70`)과 마나 회복 주문이 이것을 곱한다.
inline float ClassManaMult(CharClass) { return 1.0f; }

// GreaterHeal 틱 회복량 ◑ — **gamedata 가 아니라** `blob_587000.dat` 의 `+260360 + 4×레벨` 에 있다
//   (`GreaterHealCoeff` 키는 정품 gamedata 에 **0칸** — 찾다 없으면 여기를 볼 것).
//   실측(2026-09-14 · 그 blob 을 직접 떠서): 레벨 1 0.5 · 2 0.75 · 3 1.0 · 4 1.25 · 5 1.5. 여기에 직업 배수를 곱한다.
//   ⚠ 정답지는 레벨을 **그대로** 색인한다(−1 이 아니다).
inline float GreaterHealRate(int level) {
    static const float kRate[6] = { 0.0f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f };
    return (level >= 1 && level <= 5) ? kRate[level] : 0.0f;
}
// +490 짐 한계 ◑ plrReadVals_4EEDC0(`GAME3_3.c:7708`) — (힘 ÷ 워리어(직업 0) 최대 힘 × 1250 + 750) × blob(0x581450+10216).
//   blob 은 못 읽었다 — 위저드 녹음 `48` 의 1650 = (35/125 × 1250 + 750) × **1.5** 로 역산한 값이다.
//   `48 REPORT_STATS` 의 넷째 칸이 이것이다(servSendStats_4D8990 `GAME3_2.c:4209`).
inline int CarryCapacityOnline(CharClass c) {
    return static_cast<int>((static_cast<double>(StrengthOnline(c)) / static_cast<double>(kClassMax[0].maxStrength) * 1250.0 + 750.0) * 1.5);
}

// ★★★ +120 **사람 무게** ⭕ plrReadVals_4EEDC0(`GAME3_3.c:7701`) — **바로 윗줄 짐 한계와 같은 분모**다:
//       무게 = 힘 ÷ 워리어(직업 0) 최대 힘 × 20 + 10        → 10 ~ 30
//
//   ⚠⚠ **thing.bin `NewPlayer MASS` 는 1 이 맞지만 그게 실제 무게가 아니다.**
//     정답지도 처음엔 1 을 넣고(`server/object_type.go:209`) **사람이 생길 때 이 식으로 덮어쓴다.**
//     우리는 덮어쓰는 자리가 없어 1 로 남아 있었고, 밀어내기 가속은 **힘 ÷ 무게** 라
//     같은 접촉에서 우리만 **15배** 튕겨 나갔다(2026-09-22 벽 튕김의 뿌리).
//
//   ⭕ 검산 — 대조군(정답지 서버)의 `Sub548600` 에서 무게를 직접 읽어 **15.60** 을 봤다(위저드).
//     이 식도 35 ÷ 125 × 20 + 10 = **15.6**. 컨저러 18.8 · 워리어 30.0
inline double PlayerMassFromStrength(int strength) {
    return static_cast<double>(strength) / static_cast<double>(kClassMax[0].maxStrength) * 20.0 + 10.0;
}
inline double PlayerMassOnline(CharClass c) { return PlayerMassFromStrength(StrengthOnline(c)); }

// ★★ **문이 막는 대각선** ⭕ `server/math.go:74` doorWallTable + `server/wall.go:877~887`.
//   문은 제 칸에 벽을 만들어 붙고(`GAME1.c:692`), 그 벽의 **방향**은 문 각도(0~31)로 정해진다:
//       표에서 (x,y) 를 꺼내 — 부호가 같으면 **1**(왼쪽 대각 `\`) · 다르면 **0**(오른쪽 대각 `/`)
//       한쪽이 0 이면 **−1**(그 각도에서는 아예 안 막는다)
constexpr int kDoorWallTable[32][2] = {
    { -23, -23 }, { -18, -27 }, { -12, -30 }, {  -6, -31 },
    {   0, -32 }, {   6, -31 }, {  12, -30 }, {  18, -27 },
    {  23, -23 }, {  27, -18 }, {  30, -12 }, {  31,  -6 },
    {  32,   0 }, {  31,   6 }, {  30,  12 }, {  27,  18 },
    {  23,  23 }, {  18,  27 }, {  12,  30 }, {   6,  31 },
    {   0,  32 }, {  -6,  31 }, { -12,  30 }, { -18,  27 },
    { -23,  23 }, { -27,  18 }, { -30,  12 }, { -31,   6 },
    { -32,   0 }, { -31,  -6 }, { -30, -12 }, { -27, -18 },
};
inline int DoorWallDir(uint8_t angle) {
    const int x = kDoorWallTable[angle & 31][0], y = kDoorWallTable[angle & 31][1];
    if (x > 0 && y > 0) return 1;
    if (x < 0 && y < 0) return 1;
    if (x < 0 && y > 0) return 0;
    if (x > 0 && y < 0) return 0;
    return -1;
}

// 캠페인(솔로·퀘스트)용 보간 ◑ — **레벨 1 이 기본값, 레벨 10 이 최대값**이다.
//   `stat(L) = base + (max − base) × (L−1) / 9`
//   ⚠ 0 이 아니라 **1 부터**다. `NOX_PLAYER_MAX_LEVEL − 1 = 9` 로 나눈다(GAME3_3.c:7678).
//   ⚠ 마나만 위쪽으로 자른다 — 그래서 워리어(최대 0)는 어느 레벨에서도 0 이다.
//   지금 서버는 온라인만 돌므로 **쓰지 않는다.** 캠페인을 붙이면 여기가 정본이다.
inline int LerpStat(int base, int maxAt10, uint8_t level, bool clampUp) {
    int L = (level > kMaxLevel) ? kMaxLevel : level;
    if (L < 1) L = 1;
    double v = base + (maxAt10 - base) * static_cast<double>(L - 1) / (kMaxLevel - 1);
    if (clampUp && v > maxAt10) v = maxAt10;
    return static_cast<int>(v + 0.5);
}
inline int HealthAtLevel(CharClass c, uint8_t level) {
    return LerpStat(kBaseHealth, MaxStatsOf(c).maxHealth, level, /*clampUp=*/false);
}
inline int ManaAtLevel(CharClass c, uint8_t level) {
    return LerpStat(kBaseMana, MaxStatsOf(c).maxMana, level, /*clampUp=*/true);
}
inline int StrengthAtLevel(CharClass c, uint8_t level) {
    return LerpStat(kBaseStrength, MaxStatsOf(c).maxStrength, level, /*clampUp=*/false);
}

// ── 싸움 ─────────────────────────────────────────────────────────

// 무기 데미지 ◑ = (힘 − 요구힘) × 계수 + 최소값
//   `nox_xxx_calcBoltDamage_4EF1E0` (opennox src/legacy/GAME3_3.c:7752)
//   최소값·계수·요구힘은 ⭕ modifier.bin.
struct WeaponStats {
    const char* name;
    int    damageMin;
    double coeff;
    int    reqStrength;
    double range;
    // ★ 2026-09-13 묶음 3-b — 무기 클래스 비트(thing.bin SUBCLASS · libs `object/subclass_weapon.go` 순서).
    //   정답지는 이 비트(플레이어 WeaponEquip)로 공격 갈래·애니·기력을 고른다(`GAME4_3.c:4067~` · `GAME4.c:1395`).
    //   proto_test TestThingDb ⑦ 이 파일 SUBCLASS 와 전수 대조한다.
    uint32_t classBits;
};

// ⭕ modifier.bin `WEAPON_DEFINITIONS` — 워리어가 드는 것만 (지금 필요한 범위).
constexpr WeaponStats kWeapons[] = {
    { "Sword",        5, 0.21, 20, 40.0, 0x100  },   // SWORD
    { "MorningStar",  6, 0.25, 20, 35.0, 0x800  },   // MACE
    { "Longsword",    9, 0.30, 30, 45.0, 0x200  },   // LONG_SWORD
    { "BattleAxe",   21, 0.38, 40, 40.0, 0x1000 },   // AXE
    { "OgreAxe",     30, 0.40, 50, 40.0, 0x2000 },   // OGRE_AXE
    { "GreatSword",  50, 0.54, 70, 50.0, 0x400  },   // GREAT_SWORD
    { "WarHammer",  100, 0.50, 70, 20.0, 0x4000 },   // HAMMER
    { "StaffWooden",  4, 0.40, 20, 45.0, 0x8000 },   // STAFF
};

// SUBCLASS 원문('+' 이음) → 무기 클래스 비트. 이름 순서 = 비트 순서(libs WeaponClassNames).
inline uint32_t WeaponClassBitsOf(const std::string& raw) {
    static const char* const kNames[] = {
        "FLAG", "QUIVER", "BOW", "CROSSBOW", "ARROW", "BOLT", "CHAKRAM", "SHURIKEN", "SWORD", "LONG_SWORD",
        "GREAT_SWORD", "MACE", "AXE", "OGRE_AXE", "HAMMER", "STAFF", "STAFF_SULPHOROUS_FLARE",
        "STAFF_SULPHOROUS_SHOWER", "STAFF_LIGHTNING", "STAFF_FIREBALL", "STAFF_TRIPLE_FIREBALL",
        "STAFF_FORCE_OF_NATURE", "STAFF_DEATH_RAY", "STAFF_OBLIVION_HALBERD", "STAFF_OBLIVION_HEART",
        "STAFF_OBLIVION_WIERDLING", "STAFF_OBLIVION_ORB",
    };
    uint32_t bits = 0;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t e = raw.find('+', start);
        if (e == std::string::npos) e = raw.size();
        std::string tok = raw.substr(start, e - start);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
            if (tok == kNames[i]) bits |= (1u << i);
        start = e + 1;
    }
    return bits;
}

inline int WeaponDamage(const WeaponStats& w, int strength) {
    const double d = (strength - w.reqStrength) * w.coeff + w.damageMin;
    return (d <= 0.0) ? 0 : static_cast<int>(d + 0.5);
}

// 맨손 ◑ — 코드가 가짜 무기를 스택에 만들어 위와 **같은 식**을 태운다.
//   `nox_xxx_playerAttack_538960` (opennox src/legacy/GAME4_3.c:4037~4041)
//     주먹  계수 0.04 · 최소 5      발차기 계수 0.04 · 최소 10
// ★ 맨손은 **무기를 안 든 사람만** 친다(`src/object.go:1446` — WeaponEquip 이 0 일 때).
// ★ 발차기는 **워리어만** 나온다, 25% (같은 자리 — `PlayerClass() == Warrior && rand>75`).
//   그런데 워리어는 멀티에서 롱소드를 받으므로(아래 기본 장비) 실제로는 거의 안 나온다.
constexpr double kUnarmedCoeff   = 0.04;
constexpr int    kPunchDamageMin = 5;
constexpr int    kKickDamageMin  = 10;
constexpr int    kKickPercent    = 25;
constexpr double kUnarmedRange   = 45.0;   // ⚠ 맨손 사거리는 파일에 없다 — 지팡이와 같게 둔다

inline int UnarmedDamage(int strength, bool kick) {
    const double d = strength * kUnarmedCoeff + (kick ? kKickDamageMin : kPunchDamageMin);
    return static_cast<int>(d + 0.5);
}

// ── 접속할 때 주는 기본 장비 ◑ ───────────────────────────────────
//
// `nox_xxx_playerMakeDefItems_4EF7D0` (opennox src/legacy/GAME3_3.c:7901~8051).
// 갈래가 셋인데(캠페인·퀘스트·그 밖) **멀티/아레나는 「그 밖」**이다(:8033) —
//
//     워리어    Longsword + WoodenShield + StreetPants + StreetSneakers
//               ★ **StreetShirt 를 안 받는다** — 셔츠 자리는 「협동 모드거나 워리어가
//                 아닐 때」만 채운다(:7983). 웃통을 벗고 있는 것이 정품이다
//     위저드    WizardRobe + StreetShirt + StreetPants + StreetSneakers
//     컨저러    StreetShirt + StreetPants + StreetSneakers  (**무기 없음**)
//
// ★★ 검산 — 녹음(위저드)이 실어 나른 장비가 정확히 넷이고 그 넷이
//   StreetShirt·StreetPants·StreetSneakers·WizardRobe 였다. 코드가 말한 그대로다.
// ⚠ 와이어 번호는 종류표(`_evidence/types.tsv`) 값이다 — thing.bin 색인이 아니라
//   **이미 와이어 번호**다(정품 스냅샷이 1132 로 StreetShirt 를 보냈다).
constexpr uint16_t kItemStreetShirt    = 1132;
constexpr uint16_t kItemStreetPants    = 1133;
constexpr uint16_t kItemStreetSneakers = 1134;
constexpr uint16_t kItemWizardRobe     = 1145;
constexpr uint16_t kItemWoodenShield   = 1162;
constexpr uint16_t kItemLongsword      = 1169;

struct Loadout {
    uint16_t items[4];    // 0 이면 그 칸은 비었다
    int      count;
    uint16_t weapon;      // 든 무기의 와이어 번호. 0 이면 맨손
    double   armorSum;    // 입은 것들의 ARMOR_VALUE 합 (⭕ modifier.bin)
};

// ⭕ ARMOR_VALUE — StreetShirt/Pants/Sneakers 각 0.01 · WizardRobe 0.20 ·
//    WoodenShield **0.0**(방패는 일반 피해 분배에 안 낀다 — gamedata 주석과 일치)
inline Loadout LoadoutFor(CharClass c) {
    switch (c) {
        case CharClass::Wizard:
            return { { kItemStreetShirt, kItemStreetPants, kItemStreetSneakers,
                       kItemWizardRobe }, 4, 0, 0.23 };
        case CharClass::Conjurer:
            return { { kItemStreetShirt, kItemStreetPants, kItemStreetSneakers, 0 },
                     3, 0, 0.03 };
        case CharClass::Warrior:
        default:
            // ★ 09-29 — 차례를 **롱소드 → 방패**로 고침. 정품 녹음 `orig_arena_1851` 18:53:09.486 워리어 접속 —
            //   `4c 5d 바지 · 5e 신발 · 5f 롱소드(0x491) · 60 나무방패(0x48a)` · 정답지도 롱소드를 먼저 준다(`GAME3_3.c:8040~8041`).
            //   전에는 방패가 먼저였다(녹음에 워리어가 없던 때의 짐작 — 스냅샷 꼬리 주석 「못 본 것(방패·검)」)
            return { { kItemStreetPants, kItemStreetSneakers, kItemLongsword,
                       kItemWoodenShield }, 4, kItemLongsword, 0.02 };
    }
}

// ★ 2026-09-13 묶음 3-e — 입은 것 하나의 ARMOR_VALUE(⭕ modifier.bin, 위 주석 값). 갑옷 아니면 0.
inline float ItemArmorValue(uint16_t wire) {
    switch (wire) {
        case kItemStreetShirt: case kItemStreetPants: case kItemStreetSneakers: return 0.01f;
        case kItemWizardRobe: return 0.2f;
        case kItemWoodenShield: return 0.0f;
        default: return 0.0f;
    }
}
// 플레이어 +228 방어값 — `nox_xxx_recalculateArmorVal_53E300`(GAME4_3.c:7327): 입은 갑옷의 값을 **float 로** 더하고
//   1.0(blob_581450 9512) 을 넘으면 1.0. ⚠ 지금은 직업 기본 장비(주워 입기는 묶음 5). 마법 효과(+76 콜백)는 5.
//   ★ 3-d 정정 — **순서가 값을 바꾼다.** 인벤토리는 새로 든 것을 맨 앞에 끼운다(`GAME3_3.c:10635~10640` · `GAME4_2.c:3246~3251`)
//   그래서 목록 순회는 든 순서의 **거꾸로**다. 위저드(셔츠·바지·신발 → 로브 순으로 받음, `GAME3_3.c:7990~8037`)를
//   로브부터 더하면 0x3e6b8520 — 녹음 스냅샷의 `49 20 85 6b 3e` 와 같고, 든 순서대로 더하면 0x3e6b851f 로 한 비트 어긋난다.
inline float ArmorValueFloat(CharClass c) {
    const Loadout l = LoadoutFor(c);
    float v = 0.0f;
    const int n = l.count < 4 ? l.count : 4;
    for (int i = n - 1; i >= 0; --i) v = ItemArmorValue(l.items[i]) + v;
    if (v > 1.0) v = 1.0f;
    return v;
}

// 든 무기의 스탯. 맨손이면 nullptr.
inline const WeaponStats* WeaponOf(CharClass c) {
    if (LoadoutFor(c).weapon == kItemLongsword) return &kWeapons[2];   // Longsword
    return nullptr;
}

// 이 직업이 한 대 때리는 피해. 무기가 있으면 무기 식, 없으면 맨손 식.
inline int MeleeDamageFor(CharClass c, int strength, bool kick) {
    if (const WeaponStats* w = WeaponOf(c)) return WeaponDamage(*w, strength);
    return UnarmedDamage(strength, kick);
}
inline double MeleeRangeFor(CharClass c) {
    if (const WeaponStats* w = WeaponOf(c)) return w->range;
    return kUnarmedRange;
}

// 방어구 ◑ — 입은 것들의 `ARMOR_VALUE`(⭕ modifier.bin, 0.01~0.24)를 **더하고**,
//   1.0 에서 자른 뒤 곱해서 깎는다.
//   `nox_server_handler_PlayerDamage_4E17B0` (opennox src/legacy/GAME3_3.c:350~424)
//
//   피해 종류(1바이트)마다 방어가 먹는 정도가 다르다 —
//     0·3·7·8·0xA·0xB   물리     받는 피해 = 원 피해 × (1 − 방어합)
//     2                 절반     × (1 − 방어합 × 0.5)
//     9·0x11            전기     **방어를 안 탄다** — 에너지볼트·연쇄번개
//     1·0xC·4·5·6·0xE~0x10       **방어를 안 탄다** — 파이어볼 직격이 종류 1 이다
//   그래서 갈래를 「방어가 얼마나 먹나」로 셋만 둔다.
//
// ★ 번호 ↔ 이름 정본 (2026-09-14 실측, 묶음 4-c3). thing.bin 의 `COLLIDE = DamageCollide 2 DAMAGE_FLAME` 처럼
//   게임 데이터는 **이름**으로 적고, 번호는 정답지 표의 자리다 — `parseDamageTypeByName_4E0A00`
//   (`GAME3_2.c:6962`)가 0x587000+200728 부터 200800 까지(= 18칸) 이름을 훑어 **몇 번째인지**를 돌려준다.
//   못 찾으면 18(= 칸 수)이고, `collideDamageLoad_536E10` 은 18 이면 실패로 본다.
//   표는 `src/common/memmap/nox/blobdata/blob_587000.dat` 의 그 자리를 풀어 읽었다:
//      0 BLADE   · 1 FLAME  · 2 CRUSH   · 3 IMPALE · 4 DRAIN  · 5 POISON
//      6 DISPEL_UNDEAD · 7 EXPLOSION   · 8 BITE    · 9 ELECTRIC
//     10 CLAW    · 11 IMPACT · 12 LAVA  · 13 DEATH_MAGIC · 14 PLASMA
//     15 MANA_BOMB · 16 ZAP_RAY · 17 AIRBORNE_ELECTRIC
//   ⚠ 위 「방어가 먹는 정도」 목록과 대조해 읽을 것 — 불(1)·용암(12)·독(5)이 방어를 안 타는 게 이 표로 설명된다.
// ★ 09-23 nine-defects 1-d — 위 표를 **코드로** 옮긴다(`parseDamageTypeByName_4E0A00`). 전에는 주석뿐이라
//   thing.bin 의 `DamageCollide <양> <이름>` 을 읽을 길이 없어 주문 불꽃은 값을 손으로 박았다(양 2 · 종류 1).
//   이름은 blob 그대로 **`DAMAGE_` 가 붙는다**(`blob_587000.dat` 을 풀어 18칸 순서 확인). 못 찾으면 18
constexpr int kDamageTypeCount = 18;
inline int DamageTypeByName(const std::string& name) {
    static const char* const kNames[kDamageTypeCount] = {
        "DAMAGE_BLADE", "DAMAGE_FLAME", "DAMAGE_CRUSH", "DAMAGE_IMPALE", "DAMAGE_DRAIN", "DAMAGE_POISON",
        "DAMAGE_DISPEL_UNDEAD", "DAMAGE_EXPLOSION", "DAMAGE_BITE", "DAMAGE_ELECTRIC", "DAMAGE_CLAW", "DAMAGE_IMPACT",
        "DAMAGE_LAVA", "DAMAGE_DEATH_MAGIC", "DAMAGE_PLASMA", "DAMAGE_MANA_BOMB", "DAMAGE_ZAP_RAY", "DAMAGE_AIRBORNE_ELECTRIC",
    };
    for (int i = 0; i < kDamageTypeCount; ++i)
        if (name == kNames[i]) return i;
    return kDamageTypeCount;
}

enum class DamageKind {
    Physical,    // 방어합만큼 깎인다 — 근접·타격
    HalfArmor,   // 방어가 절반만
    Ignores,     // 방어를 안 탄다 — 주문(파이어볼·전기)
};

inline int ApplyArmor(int damage, double armorSum, DamageKind kind) {
    if (armorSum > 1.0) armorSum = 1.0;
    if (armorSum < 0.0) armorSum = 0.0;
    double f = 1.0;
    switch (kind) {
        case DamageKind::Physical:  f = 1.0 - armorSum;       break;
        case DamageKind::HalfArmor: f = 1.0 - armorSum * 0.5; break;
        case DamageKind::Ignores:   f = 1.0;                  break;
    }
    const double d = damage * f;
    return (d <= 0.0) ? 0 : static_cast<int>(d + 0.5);
}

// ── 주문 ─────────────────────────────────────────────────────────

// 파이어볼 ⭕ thing.bin — 주문 세기가 부르는 오브젝트가 레벨마다 다르고,
// 그 오브젝트의 `COLLIDE = SparkExplosionCollide <n>` 인자가 곧 세기다.
//
//     레벨 1     Fireball        128
//     레벨 2     StrongFireball  192
//     레벨 3~5   TitanFireball   255
//
// 코드는 이 인자를 이렇게 쓴다 ◑ (`nox_xxx_fireballCollide_4E9AC0`, GAME3_3.c:4176~4197)
//     직접 타격  n >> 1          광역 피해  n >> 1
//     광역 안쪽  15.0 고정        광역 바깥  n / 3
//     밀치기     반경 n×2/3, 힘 50
//
// ★ 검산 — TitanFireball 은 255>>1 = **127**. 09-06 덤프에 정체를 못 가리던
//   체력변화 표본 **−127** 이 있었다(`Docs/UNMEASURED.md`). 값이 딱 맞는다.
constexpr uint8_t kFireballCollideArg[5] = { 128, 192, 255, 255, 255 };

inline int FireballDirectDamage(uint8_t spellLevel) {
    const uint8_t L = (spellLevel < 1) ? 1 : (spellLevel > 5 ? 5 : spellLevel);
    return kFireballCollideArg[L - 1] >> 1;
}
inline double FireballSplashOuter(uint8_t spellLevel) {
    const uint8_t L = (spellLevel < 1) ? 1 : (spellLevel > 5 ? 5 : spellLevel);
    return kFireballCollideArg[L - 1] / 3.0;
}
constexpr double kFireballSplashInner = 15.0;

// 지속 주문 ⭕ gamedata.bin — **계수가 아니라 「프레임당 피해」다.**
// 곱할 밑값이 없다(opennox `nox_xxx_spellEnergyBoltTick_52E850` / `..._onFrameLightning_52F8A0`:
// 소수점을 이월해 두었다가 정수가 될 때 적용). 30fps 면 아래 값의 30배가 초당 피해다.
//
//     ARENA EnergyBoltDamage = 1.3, 1.6, 2.0, 2.3, 2.6   (레벨별)
//     ARENA LightningDamage  = 1.5                        (사슬로 이어진 대상 **모두**에게)
constexpr double kEnergyBoltPerFrame[5] = { 1.3, 1.6, 2.0, 2.3, 2.6 };
constexpr double kLightningPerFrame     = 1.5;
constexpr double kLightningRange        = 240.0;   // ⭕ gamedata `LightningRange`


// ── 속도 ─────────────────────────────────────────────────────────
//
// ◑ `thing.bin` 의 `SPEED` 는 **32로 나눈다.** 투사체(CLASS 에 MISSILE)면 ×2.
//   (opennox src/server/object_type.go:229 · :339)
//
//     Fireball  SPEED 192 → 6.0 → 투사체라 ×2 = 12.0 유닛/프레임
//     NewPlayer SPEED  96 → 3.0 유닛/프레임  ← **걷기**
//
// ★★ 이 규칙이 옛 미해결을 닫는다 — 걸음 실측(정품 866표본)에서 「느린 무리
//   2.8~4.2 가 따로 있는데 걷기와 달리기로 갈리는 듯하나 못 가림」이라 적어 둔 것이
//   바로 이 3.0 이다. 잦은 쪽(7)이 달리기다.
// ★ **우리는 늘 달리기로 간다**(사용자 결정 2026-09-10). 그래서 `kStepPerTick = 7` 유지.
constexpr double kThingSpeedDivisor  = 32.0;
constexpr double kMissileSpeedFactor = 2.0;
constexpr double kPlayerWalkPerFrame = 96.0 / kThingSpeedDivisor;                        // 3.0
constexpr double kFireballPerFrame   = 192.0 / kThingSpeedDivisor * kMissileSpeedFactor; // 12.0

// 날아가는 것의 **수명** ⭕ gamedata.bin — 목표를 물었으면 45, 아니면 15 프레임(아레나).
//   `GAME4_3.c:5787` — `v2[1]`(물린 목표)이 있으면 Targeted, 없으면 UnTargeted 를 읽고
//   `gameFrame() - 만든프레임 <= 그 값` 인 동안만 산다.
// ★ 우리 파이어볼은 목표를 안 문다 → **15 프레임**.
//   한 틱에 12.0 씩 가므로 **180 유닛**을 날면 사라진다.
//   ⚠ 전에 쓰던 210 은 「걸음 서른 걸음쯤」으로 지어낸 값이었다.
constexpr int    kTargetedSpellFrames   = 45;
constexpr int    kUnTargetedSpellFrames = 15;
constexpr double kProjectileMaxTravel   = kFireballPerFrame * kUnTargetedSpellFrames;

// ── 묶음 7-d 능력 ⭕ gamedata.bin ────────────────────────────────
//
// 워리어 능력 **다섯**. 번호는 1부터고(`server/spells_abil.go:186~193`),
// **주문 레벨 표의 1~5 칸을 같이 쓴다**(`serverSetAllWarriorAbilities` 가 `SpellLvl[1..5]` 를 만진다).
//   1 광전사 돌진 · 2 함성 · 3 작살 · 4 살금살금 · 5 늑대의 눈
// ⭕ 값은 gamedata.bin 의 「Ability durations/usage delay in frames ( 30 fps assumed )」 토막 그대로다.
// ⚠ 지속 **99999**(살금살금)는 「시간으로는 안 꺼진다」는 뜻이다 — **걷다가 다른 동작으로 넘어가거나**
//   (`object_update.go:616~621`, 09-23 1-a 에서 옮김) 죽어야 꺼진다.
//   ✖ 09-23 정정 — 전에는 「다시 눌러 끄거나」라고 적혀 있었다. 다시 누르면 **거절**된다(`spells_abil.go:182~186`).
// ⚠ 작살은 지속 **0** 이라 아예 지속 목록에 안 올라간다(`getDuration(abil) > 0` 관문).
// ⚠ 정답지 Go 는 함성 지속을 `Balance.Float("WarCryDuration")`(가운데 C 대문자)로 찾는데
//   파일의 이름은 `WarcryDuration` 이다 — 이름표가 대소문자를 안 가리거나 정답지 쪽 오타다.
//   ⭕ 어느 쪽이든 파일에 적힌 값은 **120** 하나뿐이라 셈에는 차이가 없다.
constexpr int kAbilityCount = 6;                  // 0 = 없음 · 1~5 · (6 = 「전부」 자리)
constexpr uint32_t kAbilityDelay[kAbilityCount]    = { 0, 300, 300, 150, 30, 600 };
constexpr uint32_t kAbilityDuration[kAbilityCount] = { 0,  90, 120,   0, 99999, 300 };
// 이름 — 클라 문자열표 열쇠가 아니라 우리 기록·시험용
constexpr const char* kAbilityNames[kAbilityCount] = {
    "ABILITY_INVALID", "ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY",
    "ABILITY_HARPOON", "ABILITY_TREAD_LIGHTLY", "ABILITY_EYE_OF_THE_WOLF",
};

// ── 묶음 7-f 작살 ⭕ gamedata.bin 「# Harpoon Specific」 토막 ─────
//
// 값은 파일에 적힌 그대로다. 거리는 맵 눈금, 프레임은 30fps 기준.
constexpr int      kHarpoonDamage    = 1;     // HarpoonDamage — 꽂힐 때 한 번
constexpr double   kHarpoonForce     = 10.0;  // HarpoonForce — 매 틱 끌어당기는 힘
constexpr double   kHarpoonMaxDist   = 375.0; // MaxHarpoonDistance — 이보다 멀어지면 끊긴다
constexpr double   kHarpoonMinDist   = 20.0;  // MinHarpoonDistance — 이보다 가까워지면 끊긴다(다 끌어왔다)
constexpr double   kHarpoonMaxFlight = 300.0; // MaxHarpoonFlightDistance — **안 걸린 채** 이보다 멀리 날면 끊긴다
constexpr uint32_t kHarpoonLifetime  = 150;   // MaxHarpoonExistence — 꽂힌 채 버틸 수 있는 프레임

// ── 묶음 7 성장 ⭕ gamedata.bin `XPTable` ─────────────────────────
//
// 「첫 칸이 레벨 **0**, 그다음이 1」이라고 파일에 적혀 있다(주석 그대로).
// 레벨 올림은 `sub_4EF2E0_exp_level` — **다음 레벨의 칸 값 이하**로 경험치가 차면 한 단계 오른다.
// ⚠ 한 번 부를 때 **한 단계만** 오른다(정답지가 `while` 이 아니라 `if` 다).
// ⚠ 레벨 천장은 **10**(`legacy/defs.h:26` `NOX_PLAYER_MAX_LEVEL`) — 표는 30칸이지만 거기까지 안 간다.
constexpr int kPlayerMaxLevel = 10;
constexpr float kXPTable[] = {
    5000, 11000, 18200, 26840, 37208, 49650, 64580, 82495, 103995, 126891,
    151276, 177246, 204903, 234359, 265729, 299139, 334720, 372613, 412970, 455950,
    501723, 550472, 602390, 657682, 716568, 779282, 846072, 917204, 992959, 2000000000.0f,
};

// ── 몬스터 ⭕ monster.bin (아레나 값) ─────────────────────────────
//
// 소환수·퀘스트 몬스터가 쓰는 표. `SPEED` 는 thing.bin 과 같은 눈금이다
// (GruntAxe 가 thing 기본 80 → monster 아레나 90 으로 덮인다).
// ⚠ Imp 은 근접이 없고 원거리(`MISSILE_NAME = ImpShot`)다 —
//   `ARENA ImpShotDamage = 5` (⭕ gamedata.bin).
struct MonsterStats {
    const char* name;
    uint16_t    wireType;      // 와이어 종류 번호 (thing.bin 색인 + 1)
    int         health;        // ARENA HEALTH
    int         meleeDamage;   // ARENA MELEE_ATTACK_DAMAGE — **0 이면 근접이 없다**(원거리형)
    double      meleeRange;    // MELEE_ATTACK_RANGE
    int         speed;         // ARENA SPEED (thing 눈금 — 32로 나눠 쓴다)
    int         minDelay;      // MELEE_ATTACK_MIN_DELAY (프레임) — 0 이면 파일에 없다
    int         maxDelay;
    // ── 묶음 6-g(3) 원거리 ⭕ monster.bin (2026-09-15 `nox_datadump` 로 직접 읽음) ──
    //   `monsterCanShoot_534280` 은 몬스터표 **+148**(MISSILE_NAME)이 비었나만 본다.
    //   ⚠ **Beholder 는 사거리 200 이 적혀 있는데 이름이 NULL** 이라 「못 쏜다」로 갈린다 — 파일 그대로다
    const char* missileName;   // MISSILE_NAME — nullptr 이면 못 쏜다
    double      missileRange;  // MISSILE_ATTACK_RANGE (+212)
    int         missileMin;    // ARENA MISSILE_ATTACK_MIN_DELAY (+220)
    int         missileMax;    // ARENA MISSILE_ATTACK_MAX_DELAY (+224)
    // ── 묶음 6-h — **뛰기 배율** ⭕ monster.bin `ARENA RUN_MULTIPLIER` (몬스터표 **+96**)
    //   `creatureActuallyMove_50D3B0` 이 **깃발 0x4000(뛰는 중)** 이면 빠르기에 이것을 곱한다
    double      runMul;
    // ── 묶음 7 — 잡았을 때 주는 경험치 ⭕ monster.bin `EXPERIENCE`
    int         experience;
};

// `SummonType()` 이 부를 수 있는 열셋 전부. **전부 monster.bin 에서 뽑은 값**이다.
// ⚠ 0 은 「그 키가 파일에 없다」는 뜻이지 「0 이다」가 아니다 —
//   원거리형(Imp·EvilCherub·Beholder·Bomber·EmberDemon)은 근접 칸이 비어 있다.
constexpr MonsterStats kMonsters[] = {
    //  이름                 번호   체력   피해   사거리    속도  최소 최대
    { "EvilCherub",         1317,    40,    0,    0.0,   100,   0,  0,  "CherubArrow", 150.0, 15, 25, 1.2, 110000 },
    { "Ghost",              1325,    40,   10,   10.0,    60,   0,  0,  nullptr,         0.0,  0,  0, 1.1,  20000 },
    { "Imp",                1328,    25,    0,    0.0,   120,   0,  0,  "ImpShot",     150.0, 15, 30, 1.3,  13000 },   // 발사체 피해 5
    { "EmberDemon",         1337,    30,   20,   15.0,    85,   5, 10,  "Fireball",    200.0, 30, 60, 1.3,  60000 },   // ★ 근접 15.0 정정 · ★ 09-29 B3 — 근접 피해 20·간격 5~10 정정(monster.bin ARENA — 「근접 칸이 비었다」는 틀림)
    { "Bomber",             1348,     1,    0,    0.0,   120,   0,  0,  nullptr,         0.0,  0,  0, 1.0,  10000 },   // 자폭형
    { "AlbinoSpider",       1355,    60,   15,   15.0,    80,  15, 25,  nullptr,         0.0,  0,  0, 1.5,  20000 },   // ★ 근접 15.0 정정 · ★ 09-29 B3 — 간격 15~25 정정(monster.bin 모드 구분 없는 값)
    { "SmallAlbinoSpider",  1356,    25,    6,   15.0,   100,   0,  0,  nullptr,         0.0,  0,  0, 1.5,   5000 },
    { "Bat",                1357,    35,    4,   15.0,   105,   0,  0,  nullptr,         0.0,  0,  0, 1.3,   8000 },
    { "GiantLeech",         1358,    60,   20,   15.0,    50,  30, 40,  nullptr,         0.0,  0,  0, 1.3,  12000 },
    { "Bear",               1365,   120,   45,   20.0,    50,  30, 45,  nullptr,         0.0,  0,  0, 1.5,  70000 },
    { "BlackBear",          1366,    90,   18,   20.0,    60,  30, 45,  nullptr,         0.0,  0,  0, 1.5,  36000 },
    { "CarnivorousPlant",   1371,   100,  100,   40.0,     0,   0,  0,  nullptr,         0.0,  0,  0, 1.0,  80000 },   // 못 움직인다
    { "Beholder",           1382,   250,    0,    0.0,    60,   0,  0,  nullptr,       200.0,  0,  0, 1.3,  85000 },   // ⚠ 사거리는 있는데 이름이 NULL
};

// ★ 09-23 nine-defects 2-a4 — monster.bin `STATUS` 에 **`CAN_CAST_SPELLS`** 가 있는 몬스터(정답지 `MonStatusCanCastSpells` 0x20 ·
//   몬스터를 만들 때 `ud.StatusFlags = def.StatusFlags92` 로 실린다 `unit_monster.go:117`). 함성의 무효화는 이런 몬스터에게만 간다
//   (`spell_warcry.go:45~50`). ⭕ `nox_datadump monster.bin` 으로 73종 전수에서 뽑은 11종(09-23). 위 소환 표 중에는 Beholder 하나다
constexpr const char* kMonstersCanCastSpells[] = {
    "UrchinShaman", "Demon", "Wizard", "WizardWhite", "Beholder", "Lich",
    "LichLord", "Hecubah", "WizardGreen", "WillOWisp", "Necromancer",
};

// 원거리 소환수의 발사체 피해 ⭕ gamedata.bin. 아직 Imp 만 확인했다.
//   `ARENA ImpShotDamage = 5`
constexpr int kImpShotDamage = 5;

// ── 퀘스트 웨이브·스테이지 ──────────────────────────────────────
//
// 정품 NoxQuest 는 웨이브가 밀려오고 스테이지가 오른다. 우리도 그 꼴로 —
// **스테이지마다 몬스터 한 무리를 세우고, 다 잡으면 다음 스테이지**로.
//
// ⚠ **스테이지 구성은 ➖ 우리 값이다.** 정품은 전용 맵(`G_*`)의 생성기 배치라
//   파일에 「스테이지 1 은 거머리 둘」 같은 표가 없다. 세기 순으로 우리가 짠다 —
//   약한 것부터 강한 것으로, 수도 조금씩 늘려. 아래 종류·수·체력은 kMonsters(⭕)에서 온다.
//
// ⭕ **한 번에 사는 수의 천장은 파일에 있다** — gamedata `GeneratorMaxActiveCreatures`
//   High/Normal/Low/Singular = 8/5/2/1. Normal(5)을 웨이브 한 무리의 상한으로 쓴다.
struct QuestStage {
    const char* label;        // 화면에 띄울 이름
    uint16_t monster;         // 와이어 종류 번호 (kMonsters)
    int count;                // 이 스테이지에 세우는 마릿수
};
// ★ 09-29 B2 — 식충식물 와이어 번호(위 `kMonsters` 표 · 정답지 `s.IsPlant`) — 되갚기가 다르게 간다(`server/object_ai.go:191~195`)
constexpr uint16_t kCarnivorousPlantType = 1371;

// ★ 09-29 B4 — 두뇌가 읽는 몬스터 값 ⭕ monster.bin(`STATUS` → 갱신자료 +1440 · `ARENA RETREAT_RATIO` +1336 · `RESUME_RATIO` +1344 · `FLEE_RANGE` +1356)
//   위 `kMonsters` 열셋만(스크립트로 13종 전수 대조 — 09-29). ARENA 후퇴 비율은 **전부 0** 이다(SOLO 만 0.1~0.25) — 그래서 멀티에서
//   「체력이 줄면 도망」은 안 일어나고, 후퇴는 주문 쓰는 몬스터가 반주문(버프 29)에 걸렸을 때만 난다(`GAME5.c:1425`)
//   STATUS 비트 — 0x1 DESTROY_WHEN_DEAD · 0x8 CAN_DODGE · 0x20 CAN_CAST_SPELLS(`libs object/subclass_monster.go:73~85`)
struct MonsterBrainInfo { uint32_t status = 0; float retreat = 0.0f, resume = 1.0f, fleeRange = 0.0f; };
inline MonsterBrainInfo MonsterBrainOf(uint16_t wire) {
    switch (wire) {
        case 1317: return { 0x08, 0.0f, 1.0f,  0.0f };   // EvilCherub  CAN_DODGE
        case 1325: return { 0x01, 0.0f, 1.0f,  0.0f };   // Ghost       DESTROY_WHEN_DEAD
        case 1328: return { 0x08, 0.0f, 1.0f,  0.0f };   // Imp         CAN_DODGE
        case 1337: return { 0x08, 0.0f, 0.5f,  0.0f };   // EmberDemon  CAN_DODGE · 회복 0.5
        case 1348: return { 0x01, 0.0f, 1.0f,  0.0f };   // Bomber      DESTROY_WHEN_DEAD
        case 1355: return { 0x00, 0.0f, 1.0f,  0.0f };   // AlbinoSpider
        case 1356: return { 0x00, 0.0f, 1.0f,  0.0f };   // SmallAlbinoSpider
        case 1357: return { 0x00, 0.0f, 0.5f,  0.0f };   // Bat         회복 0.5
        case 1358: return { 0x00, 0.0f, 1.0f,  0.0f };   // GiantLeech
        case 1365: return { 0x00, 0.0f, 0.25f, 0.0f };   // Bear        회복 0.25
        case 1366: return { 0x00, 0.0f, 0.5f,  0.0f };   // BlackBear   회복 0.5
        case 1371: return { 0x00, 0.0f, 1.0f,  0.0f };   // CarnivorousPlant
        case 1382: return { 0x28, 0.0f, 0.25f, 65.0f };  // Beholder    CAN_CAST_SPELLS+CAN_DODGE · 회복 0.25 · 도망 거리 65
        default:   return {};
    }
}

// ★ 09-29 B3 — 근접 때리기 함수·피해 종류·밀어내기·독 ⭕ monster.bin(`MELEE_STRIKE_FUNCTION` +236 · `MELEE_ATTACK_DAMAGE_TYPE` +124 ·
//   `MELEE_ATTACK_IMPACT` +120 · `MELEE_ATTACK_POISON_CHANCE/STRENGTH/MAX` +136·+140·+144) — 위 `kMonsters` 열셋만.
//   피해 종류 번호는 아래 18칸 표(8 BITE · 2 CRUSH · 4 DRAIN · 10 CLAW). NULL(Imp·EvilCherub·Beholder)은 근접을 못 한다(`532440` — 빠짐)
enum class StrikeFn : uint8_t { None = 0, Monster, Spider, Ghost, Bomber };
struct MeleeStrikeInfo { StrikeFn fn = StrikeFn::None; uint8_t dtype = 0; float impact = 0.0f; int poisonChance = 0, poisonStrength = 0, poisonMax = 0; };
inline MeleeStrikeInfo MeleeStrikeOf(uint16_t wire) {
    switch (wire) {
        case 1358: return { StrikeFn::Monster, 8,  0.0f, 0, 0, 0 };    // GiantLeech BITE
        case 1355: return { StrikeFn::Spider,  8,  0.0f, 20, 1, 3 };   // AlbinoSpider BITE · 독 20% · 세기 1 · 최대 3
        case 1356: return { StrikeFn::Monster, 8,  0.0f, 0, 0, 0 };    // SmallAlbinoSpider BITE
        case 1366: return { StrikeFn::Monster, 2, 60.0f, 0, 0, 0 };    // BlackBear CRUSH · 밀어내기 60
        case 1365: return { StrikeFn::Monster, 2, 60.0f, 0, 0, 0 };    // Bear CRUSH · 밀어내기 60
        case 1357: return { StrikeFn::Monster, 8,  0.0f, 0, 0, 0 };    // Bat BITE
        case 1325: return { StrikeFn::Ghost,   4,  0.0f, 0, 0, 0 };    // Ghost DRAIN
        case 1337: return { StrikeFn::Monster, 10, 0.0f, 0, 0, 0 };    // EmberDemon CLAW
        case 1348: return { StrikeFn::Bomber,  0,  0.0f, 0, 0, 0 };    // Bomber(근접 사거리 0 · 함수는 늘 1)
        case 1371: return { StrikeFn::Monster, 8,  0.0f, 0, 0, 0 };    // CarnivorousPlant BITE
        default:   return {};                                          // Imp·EvilCherub·Beholder — NULL
    }
}

constexpr QuestStage kQuestStages[] = {
    { "Stage 1: Leeches",  1358, 2 },   // GiantLeech 체력 60
    { "Stage 2: Spiders",  1355, 3 },   // AlbinoSpider 체력 60·피해 15
    { "Stage 3: Bears",    1366, 3 },   // BlackBear 체력 90
    { "Stage 4: Grizzly",  1365, 3 },   // Bear 체력 120·피해 45
    { "Stage 5: Beholder", 1382, 1 },   // Beholder 체력 250 — 보스
};
constexpr int kQuestStageCount = static_cast<int>(sizeof(kQuestStages) / sizeof(kQuestStages[0]));
// 한 웨이브 상한 ⭕ gamedata `GeneratorMaxActiveCreaturesNormal`.
constexpr int kWaveMaxActive = 5;

// 파일에 공격 간격이 없을 때 쓸 값 ➖ — 몬스터 표의 흔한 값(25~35)에서 가운데를 쓴다.
constexpr int kDefaultMonsterDelay = 30;

// 이름으로 찾는다. 못 찾으면 nullptr.
const MonsterStats* FindMonster(uint16_t wireType);

// ── 몸과 접촉 ⭕ thing.bin ───────────────────────────────────────
//
//     NewPlayer  EXTENT = CIRCLE 10      ← 사람 몸 반지름
//     Flag       EXTENT = CIRCLE 10      ← 깃발도 같다 → 닿는 거리 = 10+10 = 20
constexpr double kPlayerRadius   = 10.0;
constexpr double kFlagRadius     = 10.0;
constexpr double kFlagTouchDist  = kPlayerRadius + kFlagRadius;   // 20

// ── 죽음과 부활 ◑ ────────────────────────────────────────────────
//
// opennox src/object_update.go:242~274 —
//     죽은 뒤 30프레임(틱레이트) → 다음 상태 → 15프레임(틱레이트/2) 더
//     온라인이면 그 뒤 **동작 키를 눌러야** 살아난다(자동 부활은 서버 규칙이 켜졌을 때만)
// ★ 우리가 쓰던 150 은 **부활 지연이 아니라 부활 직후 무적 시간**이었다
//   (`nox_xxx_playerRespawn_4F7EF0`, GAME4.c:1478 — 버프 23 을 5초=150프레임).
//   값은 실재하는데 뜻이 달랐다.
constexpr uint32_t kRespawnMinTicks   = 45;    // 30 + 15
constexpr uint32_t kRespawnInvulnTicks = 150;  // 5초 무적

// ── 장비 겉모습 비트 ◑ ──────────────────────────────────────────
//
// `0x4f`~`0x54`(장비 겉모습)의 u32 는 **장비 한 칸을 고르는 한 비트**다 —
// 클라가 그 비트로 `panim.Weapon[i]`/`panim.Armor[i]` 그림 칸을 고른다.
// 표는 opennox 서버 원본 그대로다(◑ `object_weapon.go:108-134` · `object_armor.go:140-167`,
// 2026-09-11 전수 옮김). 교차검증 — 정품 송신부(`GAME1.c:2664~2730`)가 워리어에게
// Sword(0x100)+WoodenShield(0x1000000), 위저드에게 StaffWooden(0x8000)+WizardRobe(0x4000)
// 를 보내는데 **우리 시작 장비와 일치**한다.
//
// 이름으로 찾는다. 모르면 0 — **0 이면 겉모습을 안 보낸다**(짐작으로 안 채운다).
uint32_t EquipBitFor(const char* thingName, bool* isWeapon);

// 시작 장비 와이어 번호 → 이름(EquipBitFor 로 넘기려고). 모르면 nullptr.
inline const char* LoadoutItemName(uint16_t wire) {
    switch (wire) {
        case kItemStreetShirt: return "StreetShirt";
        case kItemStreetPants: return "StreetPants";
        case kItemStreetSneakers: return "StreetSneakers";
        case kItemWizardRobe: return "WizardRobe";
        case kItemWoodenShield: return "WoodenShield";
        case kItemLongsword: return "Longsword";
        default: return nullptr;
    }
}
// ★ 2026-09-13 묶음 3-e2 — 플레이어 +0 ArmorEquip(입은 갑옷 겉모습 비트 합). 방패 = 0x3000000(나무 0x1000000 · 강철 0x2000000).
//   ⚠ 지금은 직업 기본 장비(주워 입기는 묶음 5).
inline uint32_t ArmorEquipOf(CharClass c) {
    const Loadout l = LoadoutFor(c);
    uint32_t bits = 0;
    for (int i = 0; i < l.count && i < 4; ++i) {
        const char* nm = LoadoutItemName(l.items[i]);
        if (!nm) continue;
        bool isW = false;
        const uint32_t b = EquipBitFor(nm, &isW);
        if (!isW) bits |= b;
    }
    return bits;
}

}  // namespace nox
