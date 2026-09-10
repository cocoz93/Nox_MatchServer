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
constexpr size_t kInfoClassAt = 51;
constexpr size_t kInfoLevelAt = 52;
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
};

// ⭕ modifier.bin `WEAPON_DEFINITIONS` — 워리어가 드는 것만 (지금 필요한 범위).
constexpr WeaponStats kWeapons[] = {
    { "Sword",        5, 0.21, 20, 40.0 },
    { "MorningStar",  6, 0.25, 20, 35.0 },
    { "Longsword",    9, 0.30, 30, 45.0 },
    { "BattleAxe",   21, 0.38, 40, 40.0 },
    { "OgreAxe",     30, 0.40, 50, 40.0 },
    { "GreatSword",  50, 0.54, 70, 50.0 },
    { "WarHammer",  100, 0.50, 70, 20.0 },
    { "StaffWooden",  4, 0.40, 20, 45.0 },
};

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
            return { { kItemStreetPants, kItemStreetSneakers, kItemWoodenShield,
                       kItemLongsword }, 4, kItemLongsword, 0.02 };
    }
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
};

// `SummonType()` 이 부를 수 있는 열셋 전부. **전부 monster.bin 에서 뽑은 값**이다.
// ⚠ 0 은 「그 키가 파일에 없다」는 뜻이지 「0 이다」가 아니다 —
//   원거리형(Imp·EvilCherub·Beholder·Bomber·EmberDemon)은 근접 칸이 비어 있다.
constexpr MonsterStats kMonsters[] = {
    //  이름                 번호   체력   피해   사거리    속도  최소 최대
    { "EvilCherub",         1317,    40,    0,    0.0,   100,   0,  0 },   // 원거리 CherubArrow
    { "Ghost",              1325,    40,   10,   10.0,    60,   0,  0 },
    { "Imp",                1328,    25,    0,    0.0,   120,   0,  0 },   // 원거리 ImpShot 5
    { "EmberDemon",         1337,    30,    0,    0.0,    85,   0,  0 },
    { "Bomber",             1348,     1,    0,    0.0,   120,   0,  0 },   // 자폭형
    { "AlbinoSpider",       1355,    60,   15,    0.0,    80,   0,  0 },
    { "SmallAlbinoSpider",  1356,    25,    6,   15.0,   100,   0,  0 },
    { "Bat",                1357,    35,    4,   15.0,   105,   0,  0 },
    { "GiantLeech",         1358,    60,   20,   15.0,    50,  30, 40 },
    { "Bear",               1365,   120,   45,   20.0,    50,  30, 45 },
    { "BlackBear",          1366,    90,   18,   20.0,    60,  30, 45 },
    { "CarnivorousPlant",   1371,   100,  100,   40.0,     0,   0,  0 },   // 못 움직인다
    { "Beholder",           1382,   250,    0,    0.0,    60,   0,  0 },
};

// 원거리 소환수의 발사체 피해 ⭕ gamedata.bin. 아직 Imp 만 확인했다.
//   `ARENA ImpShotDamage = 5`
constexpr int kImpShotDamage = 5;

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

}  // namespace nox
