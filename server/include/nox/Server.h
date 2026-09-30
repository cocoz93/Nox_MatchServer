// Server.h — 접속을 받는 서버 로직. **소켓을 모른다.**
//
// 왜 소켓을 떼어놨나 — 판정을 사람 눈에서 떼기 위해서다.
// 여기가 `recvfrom` 을 직접 부르면 채점하려고 매번 클라를 띄워야 한다.
// 들어온 바이트를 넣으면 나갈 바이트가 나오는 모양이라야
// **덤프를 재생해 기계로 채점**할 수 있다 (server/src/replay.cpp).
//
//   [진짜 소켓]  main.cpp    ─┐
//                             ├─▶  Server::OnPacket  ─▶  나갈 조각들
//   [덤프 재생]  replay.cpp  ─┘
//
// 지금 답하는 것은 접속 절차 세 자리뿐이다. 그 밖의 오프코드는 세기만 한다.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nox/Gamedata.h"

#include "nox/Alias.h"
#include "nox/GameBalance.h"   // 직업·레벨·정본 수치
#include "nox/JoinBundle.h"
#include "nox/SoundNames.h"   // 묶음 8-a GameSettings
#include "nox/Messages.h"   // InformCode
#include "nox/UpdateStream.h"
#include "nox/Wire.h"
#include "nox/MapFile.h"   // ★ 4-d4 — 발밑 바닥(FloorGrid)

namespace nox {

// 나갈 조각 하나.
// 물건을 쓰면 무슨 일이 나는가. `thing.bin` 의 `USE = <다루는이름> <숫자>` 에서 온다.
//
// ⚠ **숫자를 회복량으로 읽어도 되는 갈래는 둘뿐이다** — `PotionUse`(물약)·`ConsumeUse`(음식).
//   `WandUse 20` 의 20 은 회복이 아니라 완드 충전이고, `ReadUse`(책 70종)는 숫자가 없다.
//   그래서 「모르는 갈래」를 None 으로 두고 **아무 일도 안 일어나게** 한다 — 지어내지 않는다.
// ★ 묶음 5-d — **쓸 때 부르는 함수**(`USE = ConsumeUse 10` 의 앞말).
//   정답지 등록표(`legacy/object_use.go:26~56`) · thing.bin 실측 분포 —
//   ReadUse 140 · PotionUse 32 · WandUse 12 · **ConsumeUse 12** · SpellRewardUse 10 ·
//   WandCastUse 8 · WarpReadUse 4 · BowUse 4 · AmmoUse 4 · **MushroomUse 2** ·
//   FireWandUse 2 · FieldGuideUse 2 · **ConsumeConfuseUse 2** · AbilityRewardUse 2.
//   ⚠ 여기 넣은 넷 말고는 아직 안 옮겼다 — 책(Read)·완드(Wand)·활(Bow)은 뒤 조각이다.
enum class UseFn : uint8_t { None = 0, Potion, Consume, ConsumeConfuse, Mushroom, WandCast };
inline UseFn UseFnOf(const std::string& name) {
    if (name == "PotionUse") return UseFn::Potion;
    if (name == "ConsumeUse") return UseFn::Consume;
    if (name == "ConsumeConfuseUse") return UseFn::ConsumeConfuse;
    if (name == "MushroomUse") return UseFn::Mushroom;
    if (name == "WandCastUse") return UseFn::WandCast;
    return UseFn::None;
}

// ★ 묶음 5 — **줄을 때 부르는 함수**(`PICKUP = GoldPickup` 의 이름).
//   갈래마다 「줄는 순간 무슨 일이 나는가」가 다르다 — 물약은 **그 자리에서 마실 수도** 있고,
//   금은 **가방에 안 들어가고 숫자로 쌓인다**. 그 밖은 보통 줄기다.
//   ⚠ 아직 안 옮긴 것 — Armor·Food·SpellBook·Trap·Crown·Treasure·Oblivion·AnkhTradable
//   ★ 09-29 — Weapon(`sub_53A720`)·Ammo(`nox_xxx_pickupAmmo_4F3B00` — 같은 화살통 합치기 뒤 `sub_53A720`)를 가른다
enum class PickupFn : uint8_t { Default = 0, Potion, Gold, Weapon, Ammo };
inline PickupFn PickupFnOf(const std::string& name) {
    if (name == "PotionPickup") return PickupFn::Potion;
    if (name == "GoldPickup") return PickupFn::Gold;
    if (name == "WeaponPickup") return PickupFn::Weapon;
    if (name == "AmmoPickup") return PickupFn::Ammo;
    return PickupFn::Default;
}

// ★ 09-28 — **버릴 때 부르는 함수**(`DROP = ArmorDrop` 의 첫 낱말 · 등록표 `legacy/object_drop.go:22~35`).
//   사라짐 시계가 갈래마다 다르다(모두 `dropDefault_4ED290` 을 먼저 부른다 — 그쪽이 10초 규칙):
//   · Default·AudEvent·Treasure·AnkhTradable — 10초 규칙만(`GAME3_3.c:6440~6444`)
//   · Armor·Weapon — 그 뒤 협동·퀘스트 아니고 「아이템 되살아남」(`sub_409F40(2)`)이면 **25초로 다시**(`GAME4_3.c:7756~7767·5178~5189`) —
//     RESPAWN 깃발과 상관없다
//   · Potion — 그 뒤 협동·퀘스트 아니면 25초(`sub_4EDDE0` `GAME3_3.c:6872~6881`) · Food — 협동 아니면 25초(`:6884~6895`)
//   · Glyph·Trap·Crown — 아직 안 옮김(시계를 안 건다)
//   ⭕ 정품 녹음 09-28 `orig_arena_1851` — 기본 로브·맵에서 주운 갑옷·죽을 때 떨어진 넷이 모두 **750프레임(25×30)** 뒤 사라짐
enum class DropFn : uint8_t { Default = 0, ArmorWeapon, Potion, Food, NotPorted };
inline DropFn DropFnOf(const std::string& name) {
    const std::string first = name.substr(0, name.find(' '));
    if (first == "ArmorDrop" || first == "WeaponDrop") return DropFn::ArmorWeapon;
    if (first == "PotionDrop") return DropFn::Potion;
    if (first == "FoodDrop") return DropFn::Food;
    if (first == "GlyphDrop" || first == "TrapDrop" || first == "CrownDrop") return DropFn::NotPorted;
    return DropFn::Default;
}

// ★★ 묶음 5-b — **먹을거리 갈래 비트**(`SUBCLASS = POTION+HEALTH_POTION` 의 뒷말).
//   ⭕ 이름과 **차례**는 `blob_587000.dat` 0xf968~0xfa78 의 이름표에서 그대로 뽑았다
//     (`SIMPLE` `APPLE` `JUG` `POTION` `HEALTH_POTION` … `VAMPIRISM_POTION`).
//     몬스터 갈래표(`MonsterClassBitsOf`)가 바로 앞 0xf874 에 같은 짜임으로 있다.
//   ⭕ `JUG` 은 길이가 3 이라 문자열 뽑기에 안 걸렸다 — 날바이트로 확인했다:
//     0xf968 `SIMPLE\0\0APPLE\0\0\0JUG\0POTION\0\0HEALTH_POTIO…`
enum FoodClassBits : uint32_t {
    kFoodSimple            = 1u << 0,
    kFoodApple             = 1u << 1,
    kFoodJug               = 1u << 2,
    kFoodPotion            = 1u << 3,
    kFoodHealthPotion      = 1u << 4,
    kFoodManaPotion        = 1u << 5,
    kFoodCurePoisonPotion  = 1u << 6,
    kFoodMushroom          = 1u << 7,
    kFoodHastePotion       = 1u << 8,
    kFoodInvisibilityPotion= 1u << 9,
    kFoodShieldPotion      = 1u << 10,
    kFoodFireProtectPotion = 1u << 11,
    kFoodShockProtectPotion= 1u << 12,
    kFoodPoisonProtectPotion=1u << 13,
    kFoodInvulnerabilityPotion=1u << 14,
    kFoodInfravisionPotion = 1u << 15,
    kFoodVampirismPotion   = 1u << 16,
};
inline uint32_t FoodClassBitsOf(const std::string& raw) {
    static const char* const kNames[] = {
        "SIMPLE", "APPLE", "JUG", "POTION", "HEALTH_POTION", "MANA_POTION",
        "CURE_POISON_POTION", "MUSHROOM", "HASTE_POTION", "INVISIBILITY_POTION",
        "SHIELD_POTION", "FIRE_PROTECT_POTION", "SHOCK_PROTECT_POTION",
        "POISON_PROTECT_POTION", "INVULNERABILITY_POTION", "INFRAVISION_POTION",
        "VAMPIRISM_POTION",
    };
    uint32_t bits = 0;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t e = raw.find('+', start);
        if (e == std::string::npos) e = raw.size();
        std::string tok = raw.substr(start, e - start);
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
            if (tok == kNames[i]) bits |= (1u << i);
        start = e + 1;
    }
    return bits;
}

// 세계에서 **다루는** 것 — 집는 게 아니다.
enum class PropKind : uint8_t { Door = 0, Obelisk, Plate };

// ★ 09-23 nine-defects 0단계 — 능력(`AbilityDo`)이 **어디로 나갔나**. 번호 검사를 지난 뒤의 나가는 길 아홉 개다.
//   합계만 세던 때는 사람 판의 「실행 11 · 겹침 12」가 **어느 능력인지** 몰라서
//   「A 가 겹침에 막혔나 / 실행됐는데 동작이 없나」를 못 갈랐다. 그래서 능력별로 센다
enum class AbilGate : uint8_t {
    Dead = 0,      // 죽었거나 사라짐
    NotWarrior,    // 워리어가 아님
    Exclusive,     // 같이 못 켜는 짝이 켜져 있음
    Already,       // 이미 켜져 있음
    MoveState,     // 움직임 상태 12
    NotLearned,    // 퀘스트인데 레벨 0
    Overweight,    // 짐 넘침(광전사만)
    Cooldown,      // 아직 쉬는 중
    Ok,            // **실행했다**
    CtfFlag,       // ★ 2-b1 — 깃발 판에서 깃발을 든 채 광전사(`spells_abil.go:155~163`)
    Count
};
constexpr size_t kAbilGateCount = static_cast<size_t>(AbilGate::Count);

// ★ 09-23 0-b — 켜진 능력이 **어떻게 꺼졌나**. `abilActive[]` 를 내리는 자리 하나하나다
enum class AbilEnd : uint8_t {
    Expired = 0,   // 지속이 다함(`AbilityUpdate` — 꺼짐 알림·소리가 나가는 길)
    Gone,          // 주인이 죽었거나 사라짐(`AbilityUpdate` — 알리지 않고 뺀다)
    Reset,         // 하나 되돌림(`AbilityReset`)
    CancelAll,     // 전부 되돌림(`AbilityCancelAll`)
    HarpoonBreak,  // 작살 줄 끊김(`HarpoonBreak`)
    Disabled,      // ★ 1-a — 동작이 끔(`AbilityDisable` · 정답지 `DisableAbility`) — 살금살금이 걷다 멈추거나 칠 때
    Count
};
constexpr size_t kAbilEndCount = static_cast<size_t>(AbilEnd::Count);

// ★ 09-23 0-b — 작살 화살이 **무엇에 닿았나**(`HarpoonCollide` 의 나가는 길)
enum class HarpoonTouch : uint8_t {
    NoOwner = 0,   // 주인이 없다(화살만 지운다)
    Wall,          // 벽 — 끊긴다
    Self,          // 자기 몸 — 그냥 지나간다(끊기지 않음)
    NotEnemy,      // 적이 아님 — 피해만 주고 끊긴다
    Refused,       // ★ 4-c3b2b — 피해가 안 들어감(막기·되받아치기·마법 방패 흡수 등 — 피해 함수가 거짓) — 끊긴다
    Attached,      // 적 — 꽂혔다
    Count
};
constexpr size_t kHarpoonTouchCount = static_cast<size_t>(HarpoonTouch::Count);

// ★ 09-23 0-b — 작살 줄이 **왜 끊겼나**(`HarpoonBreak` 를 부르는 자리 하나하나)
enum class HarpoonEnd : uint8_t {
    Replaced = 0,  // 새로 쏘기 전에 남은 것을 치움(`HarpoonDo`)
    Wall,          // 벽에 닿음(`HarpoonCollide`)
    NotEnemy,      // 적 아닌 것에 닿음(`HarpoonCollide`)
    Refused,       // ★ 4-c3b2b — 닿았는데 피해가 안 들어감(`HarpoonCollide` · `spell_harpoon.go:172`)
    BoltGone,      // 화살이 이미 없다(수명·일반 벽 처리로 지워짐)
    OwnerDead,     // 주인이 죽었다
    TargetGone,    // 걸린 상대가 사라졌다
    TargetDead,    // 걸린 상대가 죽었거나 나갔다
    TooFar,        // ① 375 보다 멀어짐
    Arrived,       // ② 20 보다 가까워짐 — 다 끌어왔다
    Lifetime,      // ③ 150프레임
    Stuck,         // ④ 30틱 동안 상대가 안 움직임
    NoSight,       // ⑤ 시야가 막힘
    OverFlight,    // 안 걸린 채 너무 멀리 날았다
    AimDead,       // ★ 1-b — 휘어 가던 상대가 죽었거나 사라짐(`spell_harpoon.go:212~216`)
    Debug,         // 시험용 `DebugHarpoonBreak`
    Count
};
constexpr size_t kHarpoonEndCount = static_cast<size_t>(HarpoonEnd::Count);

// ★ 09-23 2-b2 — 광전사가 **무엇에 어떻게 부딪혔나**(`PlayerCollide` 광전사 갈래 · `GAME3_3.c:3188~3253`).
//   켜진 채 부른 횟수 = 아래 갈래 합 — 이 등식이 「세는 자리가 빠졌나」를 잡는다
enum class BerserkHit : uint8_t {
    Unit = 0,      // 산 유닛·몬스터 생성기(0x20006) — 밀고 150 피해 · **기절·자기 피해 없이** 되돌아감
    Object,        // 유닛 아닌 것 — 150 피해(우리 장애물엔 체력이 없다) · 기절 45 · 자기 피해 · 되돌아감
    WallHard,      // 벽(마지막 닿은 벽의 깃발 &5, 또는 닿은 벽 기록 없음) — 소리 171 · 기절 · 자기 피해 · 되돌아감
    WallSoft,      // 벽(깃발 &5 가 0) — 되돌아가기만
    Light,         // 가볍고 움직이는 것(IMMOBILE·0x80 없음 · 무게 ≤ 나) — 그냥 지나감
    Overlap,       // 밑에 깔림·겹쳐도 됨(0x9)·투사체 — 그냥 지나감
    Door,          // 안 잠긴 문 — 그냥 지나감(`:3207~3210`). ★ 09-29 Z1-A — 잠긴 문은 부딪힘(LABEL_14)이라 `Object` 로 센다(따로 `berserkDoorLocked`).
                   //   닿음은 `UpdateProps` 의 문짝 닿음(`GAME5.c:2166~2169`)에서 온다 — 짝 종류 4(문)
    Count
};
constexpr size_t kBerserkHitCount = static_cast<size_t>(BerserkHit::Count);

// ★ 09-29 B1 — 알아보기가 **무엇을 했나**(`UpdateSight`). 훑을 때 본 후보 = NotEnemy + Already + Cone + Los + FullFar + Added ·
//   목록에서 뺀 수 = Lost* 합. 두 등식이 「세는 자리가 빠졌나」를 잡는다
enum class SightStat : uint8_t {
    Scan = 0,      // 둘레를 훑은 횟수
    Cand,          // 훑을 때 원 안에 든 유닛(자기 빼고)
    NotEnemy,      // 죽음·사라짐이거나 적이 아님
    Already,       // 이미 목록에 있음
    Cone,          // 앞쪽 부채꼴 밖(내적 < 0.5)
    Los,           // 버프·시야선으로 안 보임
    FullFar,       // 목록이 찼는데 가장 먼 것보다 멀다
    Added,         // 목록에 넣음
    LostGone,      // 뺌 — 죽음·사라짐
    LostCantSee,   // 뺌 — 버프로 안 보임
    LostFar,       // 뺌 — (거리 + 30) 밖
    LostMoved,     // 뺌 — 한 프레임에 √1000 넘게 옮겨짐
    LostRecheck,   // 뺌 — 2초마다 시야선까지 다시 봐서 안 보임
    LostReplaced,  // 뺌 — 목록이 차서 더 가까운 것에 밀림
    Spot,          // 발견 소리를 부름(소리 모음 칸 16)
    Count
};
constexpr size_t kSightStatCount = static_cast<size_t>(SightStat::Count);

// ★ 09-29 B2 — 놀기·되갚기·소리 넷·싸움 시작/끝이 **무엇을 했나**
enum class B2Stat : uint8_t {
    Injured = 0,     // 피해 끝에 INJURED 를 켬(몬스터가 맞음)
    InjuredSound,    // 다침 소리(칸 15)를 부름
    Taunt,           // 도발 소리(칸 1)를 부름
    TauntQuiet,      // 도발 때가 됐는데 2~4초 쉬는 중이라 안 부름
    IdleSound,       // 놀 때 소리(칸 3)를 부름
    IdleFight,       // 놀기 — 지금 상대로 싸움을 쌓음
    IdleLookDamager, // 놀기 — 때린 자리를 바라봄
    IdleFaceBack,    // 놀기 — 지난 자리를 바라봄
    IdleStay,        // 놀기 — 그대로(치유는 B7)
    RetFight,        // 되갚기 — 때린 쪽으로 싸움
    RetNotEnemy,     // 되갚기 — 때린 쪽이 적이 아님(식충식물은 안 보임)
    RetRoam,         // 되갚기 — 때린 쪽을 몰라 떠돌기
    FightStart,      // 싸움 시작(칸 4 · ALERT·달리기 켬)
    FightEnd,        // 싸움 끝(ALERT·달리기 끔)
    Count
};
constexpr size_t kB2StatCount = static_cast<size_t>(B2Stat::Count);

// ★ 09-29 B3 — 근접 함수가 **어떻게 끝났나**. 부른 수 = Hit + NoTarget + Blocked + Bomber
enum class MeleeStrikeWay : uint8_t {
    Hit = 0,     // 대상을 골라 피해 함수를 부름
    NoTarget,    // 사거리·앞쪽 안에 적이 없음(기본 1 · 거미 0)
    Blocked,     // 시야선(추적 5)이 막힘 — 0
    Bomber,      // 폭탄병 함수(늘 1)
    Poisoned,    // 거미 독이 걸림(Hit 의 덤)
    GhostFlee,   // 유령 — 기절·도망 사슬(Hit 의 덤)
    MissSound,   // 빗나감 소리(칸 8)를 부름
    Count
};
constexpr size_t kMeleeStrikeWayCount = static_cast<size_t>(MeleeStrikeWay::Count);

// ★ 09-30 — 근접(16)·쏘기(17)가 **언제** 때리고 쐈나 — 시작한 틱(+136)에서 칸(때리는 칸 +108 · 쏘는 칸 +216)까지의 틱 수.
//   기대 = 칸 × (붙듦+1) − 1 ~ 칸 × (붙듦+1)(애니 셈을 0 으로 한 자리가 그 틱의 애니 넘기기 앞이면 −1 · 뒤면 그대로).
//   끝 칸이 그 칸이면 원본도 한 번 더 때린다(끝을 넘으려다 설 때 칸 == 끝 칸 · 틱 0) — 그건 「끝칸 두번째」로 따로 센다.
//   부른 수 = 제때 + 어긋남 + 끝칸 두번째 — 근접은 근접 함수 부른 수(Hit+NoTarget+Blocked+Bomber)와 같아야 한다
enum class AttackTiming : uint8_t {
    MeleeOnTime = 0, MeleeOff, MeleeLastRepeat,
    ShotOnTime, ShotOff, ShotLastRepeat,
    Count
};
constexpr size_t kAttackTimingCount = static_cast<size_t>(AttackTiming::Count);

// ★ 09-30 한 틱 한 데이터그램 — 묶음에 무엇을 담았고, 비신뢰 메시지가 **왜 곧바로** 나갔나(갈래마다 따로)
//   담은 수 = InTs + InUpd + InMisc(+ 중요 조각 InImp) · 곧바로 = Direct* 합 · 낸 묶음 조각 = Datagrams(넘쳐 끊어 더 낸 OverExtra 포함)
enum class BundleStat : uint8_t {
    InTs = 0, InUpd, InMisc, InImp,   // 칸별로 담은 것(중요는 FlushImportant 가 쓴 조각 수)
    Datagrams,                        // 틱 끝에 낸 묶음 조각
    Overflow,                         // 2048 을 넘어 둘 이상으로 끊은 묶음(★ 조각 2 — 끊은 조각도 다 `27` 로 시작)
    OverExtra,                        // 끊어서 더 낸 조각 수(Datagrams 에도 들어간다)
    OverSize,                         // `27` 과 합쳐 한 단위만으로 2048 을 넘은 것 — 메시지는 못 가르므로 그대로 낸다
    DirectStream,                     // 스트림 0 이 아님(퐁 등)
    DirectCtrl,                       // 접속 제어(첫 번호 < 0x20 — 정답지 `netstr/conn.go:704`)
    DirectNotInGame,                  // 아직 게임 밖(접속 중)
    DirectTest,                       // 시험용 즉시 모드(`importantImmediate`)
    DirectChat,                       // 채팅 — 정답지도 곧바로(`network_server.go:149~151`)
    DirectMirror,                     // 서두 재생(녹음 조각 그대로)
    Count
};
constexpr size_t kBundleStatCount = static_cast<size_t>(BundleStat::Count);
constexpr size_t kBundleMaxPayload = 2048;   // 정답지 몸통 한도(`internal/netlist/netlist.go:10` bufSize)

// ★ 09-29 B4 — 두뇌 반응 층이 **무엇을 했나**
enum class BrainStat : uint8_t {
    Runs = 0,        // 두뇌를 끝까지 본 수(16프레임 관문·61·죽음을 지난 것)
    Confused,        // 혼란 — 62(3)·36 을 쌓음
    Fear,            // 두려움 — 62(11)·24 를 쌓고 칸 11 소리
    Retreat,         // 후퇴 — 공격 행동을 빼고 68·6 을 쌓고 칸 12 소리
    RetreatAnti,     //   그중 반주문(버프 29) 걸린 주문 몬스터
    FleeNoPath,      // 도망 — 멀어지는 길을 못 냄(제자리를 도망 자리로)
    AggroAtWill,     // ★ B4b 지키기 중 100 안 적 — 맞은 것처럼(INJURED) 해 되갚기로 싸우게
    KeepDistance,    // ★ B4b 도망 거리 안 적 — 28·68·63·24
    Frustrated,      // ★ B4b 0.5초 넘게 15 안에서 제자리 — FRUSTRATED
    Sidestep,        //   그중 옆걸음(피하기 9)
    WaitStuck,       //   그중 기다리기(1)
    Eat,             // ★ B4b 다친 몬스터가 75 안 먹을 것을 집음
    Count
};
constexpr size_t kBrainStatCount = static_cast<size_t>(BrainStat::Count);

// ★ 09-29 B5 — 몬스터 애니 칸(`updateNPCAnimData_50A850`)과 걸음 소리(`monsterMoveAudio_534030`)가 **무엇을 했나**
//   걸음 소리를 부르는 자리는 셋(가기 7 · 도망 24 · 아무 데나 걷기 29·36) — 자리마다 센다(한 곳만 세면 나머지의 0 이 거짓말이 된다)
enum class AnimStat : uint8_t {
    NoTable = 0,     // 그 종류에 MonsterDraw 표가 없음 — 칸이 안 움직인다
    NoFrames,        // 지금 애니 칸 수 0 → 끝남(+483)
    Advance,         // 칸을 넘김
    Wrap,            //   그중 되풀이 애니가 처음 칸으로
    Done,            //   그중 한 번짜리 애니가 끝 칸에서 끝남
    StepMoveTo,      // 걸음 소리 부름 — 가기(7)
    StepFlee,        //                — 도망(24)
    StepWalk,        //                — 아무 데나 걷기(29 · 혼란 36)
    StepHit,         // 칸이 걸음 칸(A·B)이고 칸 안 틱 0 — 소리 칸 17 을 냄(났나는 monsterSnd(17, …))
    Count
};
constexpr size_t kAnimStatCount = static_cast<size_t>(AnimStat::Count);

// ★ 09-29 B6 — 소리 듣기(`aiListenToSounds` · `Sub_5466F0` · `sub_545DA0`)가 **무엇을 했나**
enum class HearStat : uint8_t {
    Recorded = 0,    // 들을 거리 기록 — 깃발 ≠ 0 · 유닛(또는 유닛이 임자)이 낸 소리 · 조각 있음 · kind ≠ 2
    Deaf,            // 귀 막힘(+404 가 앞날 — 조건 42 가 상대를 잃으면 1초)
    NotListen,       // 아군·자기 소리인데 깃발 0x10 없음
    Quiet,           // 문턱 아래(걷기 0x20 → 89 · 달리기 0x40 → 20 · 그 밖 50)
    WallHalved,      // 벽 너머라 절반 — 그래도 문턱을 넘음
    WallQuiet,       // 벽 너머 절반이라 문턱 아래
    Heard,           // 들었다(+388 번호 · +396 자리 · +404 프레임 · +408 세기)
    ReactGo,         // 들은 자리로 가 봄(59·56·70·69·7)
    ReactLook,       // 들은 자리를 바라봄(59·56·1·25 — `545DA0`)
    ReactSkip,       // 들은 것이 있었지만 용암·못 닿는 칸이라 지움만
    Cond59,          // 조건 59 가 깨짐(3초 안 들은 소리)
    Cond69,          // 조건 69 가 깨짐(자리 둘레 50 에 위험한 것)
    Count
};
constexpr size_t kHearStatCount = static_cast<size_t>(HearStat::Count);

// ★ 09-29 B7 — 몬스터 주문(`540D90` 공격 · `541050` 달아날 때 · `540B90` 제 몸 · `540F20` 되받기 · `5413B0` 시전 행동)이 **무엇을 했나**
enum class CastStat : uint8_t {
    PickOffense = 0, // 싸움 갈래 — 공격(0x40000000) 주문을 골라 시전 사슬을 쌓음
    PickEscape,      // 도망 중 후퇴면 — 달아날 때(0x80000000) 주문
    PickBuff,        // 도망 중 — 제 몸(0x10000000) 주문
    PickReact,       // 도망 중 — 되받기(0x20000000) 주문
    BlinkNear,       // 두뇌 거리 두기 — 도망 거리 절반 안 적이면 곧장 점멸(4)
    InitSound,       // 시전 애니 1칸 — 소리 칸 13(SPELL_INIT)
    Cast,            // 쏘기 칸(+216)에서 시전(`mobCast_541300`)
    CastAnti,        //   반주문(29)이라 안 함
    Accepted,        // 몬스터 시전을 받아들임(전기 광선 24 · 점멸 4)
    NotPorted,       // 몬스터 시전인데 안 옮긴 주문
    BeamHit,         // 몬스터 전기 광선이 한 틱 맞힘
    BeamMoved,       // 몬스터가 시전 자리에서 5 넘게 움직여 끊김(`sub_4FEA70`)
    Count
};
constexpr size_t kCastStatCount = static_cast<size_t>(CastStat::Count);

// ★ 09-29 — 갑옷 입기·벗기(`playerEquipArmor_53E650` · `sub_53E430` · `pickupArmor_53E7F0`)가 **어떻게 끝났나**
enum class ArmorGate : uint8_t {
    NotArmor = 0,     // 갑옷 클래스가 아님(입기)
    Already,          // 이미 입음
    ClassFail,        // 직업(modifier.bin CLASSUSE) — 글 armor.c:ArmorEquipClassFail
    StrengthFail,     // 힘(REQUIRED_STRENGTH) — 글 armor.c:ArmorEquipStrengthFail
    Swapped,          // 같은 부위(SUBCLASS)를 먼저 벗김
    Equipped,         // 입음
    NotWorn,          // 벗기 — 갑옷 아님·안 입음
    Dequipped,        // 벗음
    ShieldBlockIdle,  //   그중 방패를 막기 자세(15~17)에서 벗어 자세 13
    PickupClassFail,  // 줍기 전 직업 거절(`:7618~7624`) — 못 주움
    Count
};
constexpr size_t kArmorGateCount = static_cast<size_t>(ArmorGate::Count);

// ★ 09-23 3-a — 점프(CCJump 7)가 **어떻게 끝났나**(`object_update.go:550~571`). 들어옴 = 갈래 합
enum class JumpGate : uint8_t {
    Ok = 0,        // 뛰었다 — 기력 90 · 떠 있음 · 상태 12
    CantMove,      // 움직일 수 없음(얼림 25 · 붙잡힘 5 — `playerCanMove_4F9BC0`)
    Berserk,       // 광전사 켜짐
    Warcry,        // 함성 켜짐 값
    Overweight,    // 짐 넘침 — 힘쓰는 소리 · 알림(13,3)
    NoStamina,     // 기력 90 모자람 — 아무것도 안 함
    Count
};
constexpr size_t kJumpGateCount = static_cast<size_t>(JumpGate::Count);

// ★ 09-23 4-c2 — 사람이 맞을 때 타격 소리(`damageDefaultProc` `GAME3_2.c:7251~7267` → `PlayerDamageSound` `sound.go:15~32`)가 어떻게 끝났나
enum class HitSoundWay : uint8_t {
    Played = 0,    // 표에 번호가 있어 냈다
    TableEmpty,    // 맞는 쪽 × 때린 것 재질 조합이 표에 없다(`Sub_532930` 이 그냥 돌아감)
    NoHitter,      // 때린 물건도 때린 이도 없다(`obj2 == nil`)
    MonsterSound,  // ★ 4-c3b3b — 때린 이가 몬스터이고 그 소리 모음의 맞히는 소리에 조각이 있다 → 맞는 쪽 소리 안 냄(`GAME3_2.c:7252~7254`)
    NotPorted,     // 「때린 것」을 아직 안 넘기는 자리(시험용 DebugHurt · 진단용 때리기) · 종류를 모르는 오브젝트 — 세기만
    Count
};
constexpr size_t kHitSoundWayCount = static_cast<size_t>(HitSoundWay::Count);

// ★ 09-23 4-c3a — 사람 아닌 것이 맞을 때 기본 타격 소리(`nox_xxx_soundDefaultDamageSound_532E20` · `server/sound.go:169~202`) 갈래
enum class DefHitSoundWay : uint8_t {
    Played = 0,    // 표(`Sub_532930`)로 냄
    Breakable,     // 맞는 쪽이 나무·돌·흙(8·32·64)이고 체력이 있어 「부서지는 소리」(`Sub_532EC0`)
    Filtered,      // 때린 것이 없거나 재질 NONE(0x4000)이거나, 피해 종류가 0·2·3·10·11 이 아님
    TableEmpty,    // 표에 그 조합이 없다
    MonsterSound,  // ★ 4-c3b3b — 때린 이가 몬스터이고 그 소리 모음의 맞히는 소리에 조각이 있다 → 안 냄
    NotPorted,     // 「때린 것」을 아직 안 넘기는 자리 · 종류를 모르는 오브젝트 — 세기만
    Count
};
constexpr size_t kDefHitSoundWayCount = static_cast<size_t>(DefHitSoundWay::Count);

// ★ 09-23 nine-defects 4-d4 — 발소리의 바닥 종류(`object_update.go:164~220` 의 switch). 소리 없음 = 재질 2·0x4000 · 바닥 없음 = 칸 찾기 −1 또는 정의 밖
enum class FootKind : uint8_t { Silent = 0, Wood, Dirt, Water, Snow, Mud, Stone, NoTile, Count };
constexpr size_t kFootKindCount = static_cast<size_t>(FootKind::Count);

// ★ 09-24 사람 판 — 맞은 사람의 「윽」(`object_update.go:80~107`). 독(+524 = 5)이면 Poison · 준 비율(천분율) 70 이하 Light · 450 이하 Medium · 넘으면 Heavy
enum class HurtVoice : uint8_t { Light = 0, Medium, Heavy, Poison, Count };
constexpr size_t kHurtVoiceCount = static_cast<size_t>(HurtVoice::Count);

// ★ 09-24 H3(사람 판 「함성 맞은 표시가 안 나온다」) — 함성이 둘레 사람을 볼 때 어느 경우로 갔나(`spell_warcry.go:26~64`).
//   시전자 자신은 안 센다 · 합 = 본 사람 수 · 「걸림」 = 무효화(55)를 받아들이고 반마법(29)이 실제로 섰다
enum class WarcryPeer : uint8_t { NotInGame = 0, Warrior, Observer, SameTeam, OutOfRange, NoSight, Rejected, Applied, Count };
constexpr size_t kWarcryPeerCount = static_cast<size_t>(WarcryPeer::Count);

// ★ 09-27 B3 — 무기 들기(`nox_xxx_playerEquipWeapon_53A420` `GAME4_3.c:4906~4993`)가 어느 관문에서 나갔나 — 정답지 순서 그대로.
//   무기 아님(0x1001000) · 이미 듦(+16 & 0x100) · 함성·광전사 값(`:4923`) · 직업(`57B3D0`) · 힘(`4F3180`) · 화살통인데 활이 없고 가방에서도 못 듦(`:4954~4961` BowNotFound) ·
//   먼저 든 것 못 내려놓음 · 듦
//   「가방에 없음」(`:4941~4950`)은 뺐다 — 우리 입기 요청은 늘 가방에서 찾은 물건을 넘긴다(구조상 0)
//   ★ 09-29 — 화살통 칸이 「B5 전까지 거절(QuiverLater)」에서 BowNotFound 로 바뀜(화살통 이식)
enum class EquipGate : uint8_t { NotWeapon = 0, Already, Ability, ClassFail, StrengthFail, BowNotFound, DequipFail, Equipped, Count };
constexpr size_t kEquipGateCount = static_cast<size_t>(EquipGate::Count);
// ★ 09-27 B4 — 무기 내려놓기(`nox_xxx_playerDequipWeapon_53A140` `GAME4_3.c:4755~4816`)가 어디로 나갔나 —
//   무기 아님(`:4760`) · 든 무기가 아님(`:4767~4770` — 화살통은 든 무기가 있기만 하면 통과) · 화살통 내려놓음(`:4775~4783` — 무기 칸은 그대로) · 내려놓음 ·
//   공격 중 석궁 벗기 요청 무시(해독 쪽 `server__network__sdecode.c:185~186` — 내려놓기 함수 앞에서 걸림)
//   ★ 09-29 — 화살통 칸이 「B5 전까지 없음(QuiverLater)」에서 QuiverOff 로 바뀜
enum class DequipGate : uint8_t { NotWeapon = 0, NotHeld, QuiverOff, Dequipped, CrossbowAttacking, Count };
// ★ 09-29 — 활·석궁 쏘기(`shootBowCrossbow1_539BD0` · `2_539D80`)가 어떻게 끝났나 —
//   쏨(화살 만듦) · 앞이 벽에 막혀 안 만듦(`:4571` 선 추적 5 — 화살통도 안 깎임) · 재장전 칸이 0 이 아님(ReloadingQuiver) ·
//   든 화살통이 비어 가방의 화살통을 듦(ReloadQuiver) · 화살통이 없음(NoQuiver)
enum class BowShot : uint8_t { Shot = 0, Blocked, Reloading, Reloaded, NoQuiver, Count };
constexpr size_t kBowShotCount = static_cast<size_t>(BowShot::Count);
// ★ 09-29 — 사람이 쏜 화살 닿음(`nox_xxx_collideArrow_4EB490` `GAME3_3.c:5197~5309`)이 어디로 갔나 —
//   쏜 이(그대로 날아감) · 죽은 것(그대로) · 피해가 들어감(지움) · 피해가 안 들어감(화살은 그대로 — `:5305~5306`) ·
//   쇠뇌살이 죽인 것을 뚫고 감(`:5297~5303`) · 유닛 아닌 장애물(피해 함수 DefaultDamage 가 1 → 지움) · 벽(맵 피해 · 지움) · 40틱 수명(지움)
enum class ArrowHit : uint8_t { Owner = 0, Dead, Hurt, NoHurt, BoltPass, Obstacle, Wall, Age, Count };
constexpr size_t kArrowHitCount = static_cast<size_t>(ArrowHit::Count);
// ★ 09-29 — 무기·화살통 줍기(`sub_53A720` `GAME4_3.c:5033~5128` · `pickupAmmo_4F3B00` `GAME3_3.c:10831~10903`)에서 난 일 —
//   직업이 못 쓰는 무기라 못 주움(`:5089~5095`) · 같은 화살통에 합침 · 활·석궁을 들고 있어 화살통을 저절로 듦(`:5105~5111`) ·
//   저절로 안 들어 충전을 따로 알림(`:5113~5123`) · 줍는 소리(`sub_53A6C0` — 830 지팡이 · 842 쇠 · 844 나무)
enum class WeaponPick : uint8_t { ClassFail = 0, AmmoMerged, QuiverAutoEquip, ChargesReport, Sound, Count };
constexpr size_t kWeaponPickCount = static_cast<size_t>(WeaponPick::Count);
constexpr size_t kDequipGateCount = static_cast<size_t>(DequipGate::Count);
// ★ 09-28 — 버리기(`TRY_DROP` → `drop_4ED810`) 나간 길별: 관전 · 가방에 없음 · 벽에 막힘 · 버림
enum class DropGate : uint8_t { Observer = 0, NotInBag, Wall, Dropped, Count };
constexpr size_t kDropGateCount = static_cast<size_t>(DropGate::Count);
// ★ 09-28 — 버린 물건 10초 뒤 사라짐의 예외(`dropDefault_4ED290` `GAME3_3.c:6440~6444`). 물건마다 하나 —
//   없음 · 오브젝트 깃발 RESPAWN(0x80000 — thing.bin FLAGS · 기본 장비는 만들 때 지운다 `:7891`) · 클래스 FLAG(0x10000000) · 종류 Glyph
enum class DecayExempt : uint8_t { None = 0, Respawn, FlagClass, Glyph };
// 버릴 때 사라짐 시계가 어디로 갔나 — 10초 걸림 · 25초 걸림(갑옷·무기·물약·음식) · 협동·퀘스트라 안 걺(`!(2048)·!(4096)`) ·
//   10초 예외 셋(그 물건이 25초 갈래가 아닐 때만 남는다) · 버리기 함수를 안 옮김(글리프·덫·왕관)
enum class DecayGate : uint8_t { Armed = 0, Armed25, CoopOrQuest, Respawn, FlagClass, Glyph, NotPorted, Count };
constexpr size_t kDecayGateCount = static_cast<size_t>(DecayGate::Count);

// ✖ **2026-09-22 정정 — 「70칸 안에 들면 열린다」는 우리가 지어낸 값이었다.**
//   사용자가 화면으로 짚었다: 「문에 어떤 충돌도 없이 그냥 휙 지나가 버린다」.
//   70 이면 **닿기 한참 전에 이미 열려** 부딪힐 일이 없다.
//   ⭕ 정답지는 거리가 아니라 **부딪히면 연다** — `nox_xxx_collideDoor_4E8AC0`(`GAME3_3.c:3384`)는
//     문의 **collide 함수**다. 부딪힘 조건은 두 몸이 겹치는 것이고, 둘 다 thing.bin 의 원이다:
//       사람 `NewPlayer CIRCLE 10` + 문 `WoodenDoor·ArchedHalfDoor CIRCLE 32` = **42**
//   그래서 기본값을 42 로 둔다(문 종류의 반지름을 알면 그것을 쓴다).
constexpr double kDoorOpenDist = 42.0;
// ⭕ 묶음 9-c — 밟는 판의 반쪽 크기. thing.bin `PressurePlate` 의 `EXTENT BOX 45 45` 실측값 45 의 절반이다
constexpr double kPlateHalf = 22.5;
// ➖ 묶음 10-a — 소리 좌우 치우침을 재는 기준 폭(창너비/2).
//   정답지는 **host 자신의 창**을 보는데 전용 서버엔 창이 없다 — 녹스 기본 640 의 절반.
//   이 값은 **치우침에만** 쓰이고 들림 여부는 안 가른다
constexpr int kSoundPanHalfWidth = 320;
constexpr double kObeliskDist = 50.0;
// 오벨리스크가 한 틱에 채우는 마나.
// ✖ **2026-09-22 정정 — 「여섯 틱에 1」은 우리가 지어낸 값이었고 6배 느렸다.**
//   사용자가 화면에서 「마나 차는 속도가 너무 느리다」고 짚었다.
//   ⭕ 정답지 `nox_xxx_updateObelisk_53C580`(`GAME4_3.c:6418`) — 온라인에서는 `v12 = 1` 이고
//     오벨리스크 갱신은 **틱마다** 돈다. 즉 **틱당 1 · 초당 30** 이다
//     (깃발 4096 인 모드에서만 그 직업 최대 마나를 한 번에 채운다 — 우리 모드가 아니다).
//   ⚠ 정답지는 마나석의 **잔량도 틱마다 줄인다**(`:6454` `--*v24`) — 우리는 아직 안 줄인다(별개 묶음)
constexpr uint32_t kObeliskEveryTicks = 1;
constexpr int kObeliskManaPerTick = 1;
// 문 각도 — `0xb2` 의 셋째 바이트. 정품 표본에 `00`·`10`·`18` 이 보였다(`CAP_server_0906`).
// ◑ 값의 뜻(각도 단위)은 모른다. **닫힘 0 · 열림 0x18** 두 값만 쓴다.
// ✖ **2026-09-22 정정 — `kDoorOpen = 0x18` 은 「열림」이 아니었다.**
//   0x18 은 **어느 문의 닫힘 각도**였다. 문 각도는 **문마다 다르고 맵에 적혀 있다.**
//   ⭕ 근거 둘이 맞물린다:
//     ① 맵 짐 꼬리의 **둘째 바이트**가 그 각도다(`MapObject::tail[1]`).
//        Estate 문 여섯을 정품 와이어와 맞대니 **6/6 일치** —
//          맵 8be3=08 8be2=18 8be1=18 8be0=10 8bdf=00 803d=18
//          정품 8be3 08 8be2 18 8be1 18 8be0 10 8bdf 00 803d 18
//        꼬리 `[1]` 과 `[9]` 가 같다 — 정답지의 **닫힘 각도(`ud+4`)와 지금 각도(`ud+12`)**다
//     ② 정답지 `nox_xxx_collideDoor_4E8AC0`(`GAME3_3.c:3477`)의 `switch` 갈래가
//        **0 · 8 · 16 · 24** 넷뿐이다 — 맵에서 읽은 네 값과 같다
constexpr uint8_t kDoorClosedFallback = 0x00;   // 맵에 없을 때만
// ➖ **여는 각도는 아직 근거가 없다.** 가진 캡처에서는 문이 한 번도 안 열려 열림 값이 안 찍혔다.
//   닫힘 각도의 눈금이 8(=네 방향)이니 **한 눈금(90도)** 을 연 것으로 둔다. 근거가 생기면 바로 고친다
constexpr uint8_t kDoorOpenDelta = 8;
inline uint8_t DoorOpenAngle(uint8_t closed) { return static_cast<uint8_t>((closed + kDoorOpenDelta) & 31); }
// ★ 09-21 문 되맞춤(`UpdateProps` 끝) — 몇 틱마다 보나 · 세계를 받은 뒤 몇 틱을 기다리나.
//   기다리는 까닭 — 접속 직후의 알림은 클라가 버린다(09-20 실측). 1초면 넉넉했다
// 마나석 잔량 — 정품 표본 전수가 0x32(=50)였다. 클라는 밝기 16×값/10 · 그림칸 8×값/50 으로 쓴다
//   (`cdecode.c:2236`). 값 0 이면 **꺼진다** — 그래서 한 번도 안 알려 주면 어두운 채로 남는다
constexpr uint8_t kObeliskFullCharge = 0x32;constexpr uint32_t kDoorSyncEveryTicks = 15;
constexpr uint32_t kDoorSyncGraceTicks = 30;
// 주운 아이템이 되살아나기까지. ➖ 우리 값 — `thing.bin` 의 `FLAGS = RESPAWN` 이
// 「되살아난다」는 것만 말하고 **얼마 뒤인지는 안 적혀 있다**. 30Hz 기준 30초.
constexpr uint32_t kItemRespawnTicks = 900;

struct OutPacket {
    std::string to;
    std::vector<uint8_t> data;
};

// 불려 나온 것 하나. 부른 사람 편이라 부른 사람은 안 때린다.
// ★★ **「바뀌었으니 이 사람에게 알려야 한다」 한 건** (R4 — 정품 더티 비트 틀).
//
//   정품의 비트 하나하나가 여기 `What` 하나에 해당한다
//   (`GAME4_2.c:7136-7181` — 비트가 켜진 것만 보내고 곧바로 끈다).
//
//   ★ 2026-09-16 조각 L — **표가 원본보다 짧았다.** 정답지는 비트를 **여덟** 보는데
//     여기 여섯만 적혀 있었고 enum 은 **다섯**이었다. 빠진 둘(`0x80000`·`0x4000000`)은
//     「빠졌다」고 물을 수조차 없었다. 여덟로 맞추고 **왜 안 보내나**를 칸마다 적는다.
//     ⚠ 목록이 원본보다 짧으면 그 차이는 **영영 안 보인다** — 09-16 에 잡은 결함 셋과 같은 모양이다.
//
//     비트         | 보내는 것        | 세우는 곳(정답지)                | 오프코드 | 우리
//     `0x10000`    | 그림 칸          | `servMarkObjAnimFrame_4E4880`   | `0x6B`   | ❌ **안 보내도 된다** — 클라가 굴린다(2026-09-11 실측)
//     `0x20000`    | 유닛 현재 체력   | `unitSetHP_4E4560`              | `0x41`   | ⭕ `UnitHealth`
//     `0x40000`    | 숨김(켜고 끄기)  | `unitSetOnOff_4E4670`           | `0x38`   | ❌ `Hidden` — **안 옮김**. 켜고 끄는 주체가 **맵 스크립트**인데
//                  |                  |                                 |          |    스크립트가 통째로 미이식이라 **지금은 바뀌지 않는다**(우리 쪽 대입 자리 2곳:
//                  |                  |                                 |          |    `DebugTrapEnable`(진단) · 발판 `p.enabled`). 스크립트를 옮길 때 **같이** 옮길 것
//     `0x80000`    | 아이템 속성      | `modifSetItemAttrs_4E4990`      | —        | ❌ **표에 아예 없던 칸.** 아이템 속성이 바뀌면 알린다 — 우리는 안 알린다
//     `0x400000`   | 높이(들어올림)   | `unitRaise_4E46F0`              | `0x9F`   | ❌ `Height` — **안 옮김.** 우리 `z` 는 실제로 바뀐다(복수의 주먹 `kind 5`,
//                  |                  |                                 |          |    `Server.cpp` 떨어짐 논리). 화면에서 떨어지는 것이 보이는지는 **④대기**
//     `0x800000`   | 마법부여         | `setUnitBuffFlags_4E48F0`       | `0x5A`   | ⭕ `Enchant`
//     `0x2000000`  | 팀 기지          | (CTF 기지)                      | `0x67`   | ❌ `TeamBase` — **안 옮김.** 우리에겐 팀 기지 개념 자체가 없다(참조 0건)
//     `0x4000000`  | NPC 알림         | `netSendReportNPC_4D93A0`       | —        | ❌ **표에 아예 없던 칸.** 상인·NPC 를 안 옮겼다
struct PendingReport {
    // ⚠ **여덟이다.** 정답지 비트와 하나씩 짝이 맞아야 「빠진 것」을 셀 수 있다.
    //   지금 거는 것은 `UnitHealth`·`Enchant` 둘뿐이고, 나머지 여섯은 **거는 쪽도 없다**
    //   (그래서 앞뒤가 맞는다 — 안 걸고 안 보낸다). 시험 11-j 가 이 개수를 지킨다.
    enum class What : uint8_t {
        AnimFrame, UnitHealth, Hidden, ItemAttrs, Height, Enchant, TeamBase, ReportNPC,
        kCount,
    };
    uint16_t netCode = 0;   // 무엇이 바뀌었나 (오브젝트 번호)
    What what = What::UnitHealth;

    bool operator==(const PendingReport& o) const {
        return netCode == o.netCode && what == o.what;
    }
};

// ★ 묶음 4-a4 — 유닛 버프(마법부여). 오브젝트 +340 비트 · +344 남은 틱(u16×32) · +408 세기(u8×32)
//   정답지 buffApplyTo_4FF380 · spellBuffOff_4FF5B0 · unitClearBuffs_4FF580 · updateUnitBuffs_4FF620(GAME4.c:3538~3650)
//   번호 = EnchantID(server/spells_enchant.go) — 0 투명 · 2 눈멂 · 21 적외선 · 23 무적 · 25 얼림 · 29 반마법 …
struct Buffs {
    uint32_t bits = 0;
    uint16_t dur[32] = {};
    uint8_t power[32] = {};
    bool Has(int b) const { return b >= 0 && b < 32 && (bits & (1u << b)) != 0; }
};

// ── 묶음 6-j(2) 길목 하나 ────────────────────────────────────────────────
//
// 맵 제작자가 「여기는 걸을 수 있다」고 찍어 둔 자리와, 그 자리에서 **갈 수 있는 이웃**들.
// 점멸(4-e)은 자리만 보고 고르지만, 떠돌기는 **이음을 타고** 다음 길목으로 넘어간다.
//
// ⚠ 이음은 **단방향**이다 — 파일에도 정답지 메모리에도 「나가는 것」만 담긴다
//   (`server/waypoint.go:47 Sub_579A30` 은 깃발만 양쪽으로 OR 한다).
//   그래서 나가는 이음이 없는 길목은 **막다른 길**이고, 정답지도 거기서 떠돌기를 접는다.
struct Waypoint {
    uint32_t id = 0;
    float x = 0, y = 0;
    // {상대 길목 **번호**, 이음깃발} — 자리 번호가 아니다
    std::vector<std::pair<uint32_t, uint8_t>> links;
};

struct Creature {
    std::string owner;     // 부른 사람의 주소
    uint16_t netCode = 0;
    uint16_t type = 0;     // 와이어 종류 번호 (⚠ 종류표에서 이름으로 고른 것. 실측 아님)
    uint8_t facing = 0;
    // ★ 묶음 6-d — **돌아가려는 쪽**(오브젝트 `+126` Direction2). `facing`(+124)이 이쪽으로 쪻아간다.
    //   바라보기 행동이 매 틱 **8씩** 이것을 돌린다(한 바퀴가 256 이니 한 틱에 1/32 바퀴).
    uint8_t facingWant = 0;
    double x = 0, y = 0;
    uint16_t health = 0;
    uint32_t nextHitFrame = 0;   // 이 프레임 전에는 못 때린다
    Buffs buffs;                 // 4-a4
    uint8_t  poison = 0;                            // +540 독 남은 양(셀수록 자주 아프다)
    uint32_t poisonStart = 0;                       // 체력자료 +16 독이 시작한 프레임(0 = 없음) — 60틱 유예에 쓴다
    uint16_t poisonDecay = 0;                       // +542 감쇠 계수기 — 독을 걸 때 1000, 0 이 되면 독 1 줄고 다시 1000
    uint32_t lastDamageFrame = 0;                   // +536 — 마지막 피해 프레임(damageDefaultProc 끝 `GAME3_2.c:7236`)
    uint8_t lastDamageType = 0;                     // ★ 4-c3b2b — +524 마지막 피해 종류(`GAME3_2.c:7234`) — 작살이 끊길 때의 기본 타격 소리가 읽는다
    std::map<std::string, uint16_t> hpReportedTo;   // +412+2×i — 보는 사람마다 마지막으로 알린 체력(`42` 증감 기준 · netSendComplexObject_518960)


    // ── 묶음 6-a — **AI 행동 쌓기(AIStack)** ──────────────────────────
    //
    // 정답지 `server/object_ai.go` — 몬스터는 **할 일을 쌓아 두고 맨 위를 한다.**
    //   · 밀기(`MonsterPushAction`) — 맨 위를 **취소**하고 새 것을 올린다
    //     ⚠ 맨 위가 `죽음(31)` 이면 `죽어가는 중(30)` 말고는 **아예 안 올라간다**
    //     ⚠ 맨 위가 `놀기(0)` 이고 바닥이면 **덮어쓴다**(쌓지 않는다)
    //   · 빼기(`MonsterPopAction`) — 맨 위를 **끝내고**(End) 내린 뒤,
    //     그 아래 **조건 항목들도 같이** 내린다. 다 내려가면 바닥에 `놀기(0)` 가 남는다
    //   · 매 틱 — 맨 위가 아직 **시작 안 했으면 시작(Start)**, 쌓임이 바뀌었으면 다시 맨 위를 본다.
    //     그리고 **마지막 맨 위를 갱신(Update)** 한다(`unit_ai.go:581~603`)
    //
    // ⭕ 행동 갈래는 **0~38** 이다(`common/unit/ai` ActionType — 0 놀기 … 30 죽어가는 중 · 31 죽음 · 38 없음).
    // ★ 2026-09-15 정정 — **24 다**(`server/object_monster.go:182` `AIStack [24]AIStackItem`).
    //   8 로 두었던 것은 틀렸다. 관문도 정답지대로 `ind >= 길이 − 1` 이면 안 쌓는다
    static constexpr int kAIStackMax = 24;
    struct AIStackItem {
        uint8_t action = 0;                    // ActionType
        bool started = false;                  // +5 Field5 — Start 를 했나
        // 행동마다 쓰는 인자. 지금은 대상 하나와 자리 하나만 담는다(늘 때 넓힌다).
        std::string targetAddr;
        int targetPet = -1;
        float argX = 0, argY = 0;
        uint32_t argU32 = 0;                   // 때(41) 같은 셈 인자
        float argF = 0;                        // 거리(49~52) 같은 자 인자
        // ★ 묶음 6-j(2) — 떠돌기가 **지금 고른 길목 번호**(정답지 쌓임 항목의 `+4`). 0 = 아직 없다.
        //   이걸 안 적어 두면 매 틱 「가장 가까운 길목」을 다시 골라, 길목 위에 선 순간
        //   **제자리를 목적지로 찍고 영영 안 움직인다**(⑥ 이 막혀 있던 그 증상)
        uint32_t argWp = 0;
    };
    AIStackItem aiStack[kAIStackMax];
    // ★ 묶음 6-j(2) — **최근에 들른 길목 16곳**(정답지 `sub_545B00` 의 `+300` 고리, 머리는 `+364`).
    //   다음 길목을 고를 때 여기 있는 것은 뺀다 — 안 그러면 두 길목 사이를 오간다.
    //   ⚠ 고리는 **덮어쓰기**다. 같은 번호가 다시 들어오면 정답지는 **옛 자리를 0 으로 지운다**
    static constexpr int kRoamRecent = 16;
    uint32_t roamRecent[kRoamRecent] = {};
    int roamRecentHead = 0;
    double prevX = 0, prevY = 0;               // 묶음 6-b — 지난 틱 자리(조건 71 「안 움직였나」)
    // ★ 묶음 6-c — **행동을 바꾼 프레임**(`monsterActionReset` 의 `+124`·`+137`).
    //   정답지는 둘을 **같은 값**으로 둔다 — `상대 기다리기(2)` 가 이것에 인자를 더해 잰다.
    uint32_t aiActionFrame = 0;
    // ★ 묶음 6-c — 변신(33·34)이 보는 「됐나」 표시(`Field120_3`). 지금은 시험이 직접 세운다.
    bool aiMorphReady = false;
    bool morphed = false;                      // 상자로 변신해 있나(`MonStatusMorphed`)
    int aiStackInd = 0;                        // 맨 위 자리(−1 은 「비었다」로만 잠깐 쓴다)

    // ── 묶음 6-f — **길 따라 걷기** ──────────────────────────────────
    //   `nox_xxx_creatureActuallyMove_50D3B0`(`legacy/GAME4_1.c:774`)가 이 자료를 쓴다.
    //   점 목록은 6-e(3) 이 뽑아 준다(`ud.Path`).
    std::vector<std::pair<float, float>> path;   // +12 부터 8바이트씩 — 갈 점들
    int pathInd = 0;                             // +268 지금 보고 있는 점
    uint8_t pathStatus = 0;                      // +284 길 찾기가 어떻게 끝났나
    uint32_t pathFrame = 0;                      // +70 마지막으로 길을 낸 프레임
    float pathGoalX = 0, pathGoalY = 0;          // +68 그때 잡았던 목표
    float speedCur = 3.0f;                       // +544 SpeedCur
    double velX = 0, velY = 0;                   // 이번 틱에 갈 속도
    uint32_t pathStuckFrame = 0;                 // +540 길 찾기 결과가 1 이었던 마지막 프레임
    // ★ 묶음 6-g — **지금 싸우는 상대**(`+1196`). 싸움(15)이 이것을 본다.
    //   비어 있으면 「마지막으로 본 자리」로 가 보는 갈래로 빠진다.
    std::string enemyAddr;
    int enemyPet = -1;
    uint32_t attackStartFrame = 0;               // +136 휘두르기를 시작한 프레임
    // 묶음 6-h — 호위 거리(+1316 `Field329`, 기본 **30**)와 뛰는 중인가(깃발 0x4000)
    float escortDist = 30.0f;
    bool running = false;
    // 묶음 6-i — 명령이 세우는 값
    float aggression = 0.83f;   // +1304 Aggression (가만히·지켜 0.5 · 따라와·사냥 0.83)
    float sightRange = 0.0f;    // +1312 SightRange (지켜가 250 으로 놓는다)
    bool ordered = false;       // 사람이 명령을 준 적이 있나 — 틱의 기본 호위를 멈춘다
    bool deadDone = false;      // 묶음 6-j — 죽음(31) 갱신이 한 번 돌았나(치울 표시)
    // ★ 09-29 B1 — 알아보기(`nox_xxx_unitUpdateSightMB_5281F0` `GAME4_2.c:7278~7388`).
    //   보이는 목록(+1129 수 · +1132 최대 16) — 사람은 주소 · 크리처는 번호로 적는다(자리 번호는 앞 것이 지워지면 밀린다)
    struct SeenRef { std::string addr; uint16_t petNet = 0; };
    std::vector<SeenRef> seen;
    uint32_t sightScanFrame = 0;    // +1204 마지막으로 둘레를 훑은 프레임
    uint32_t sightNextScan = 0;     // +1208 이 프레임부터 다시 훑는다
    uint32_t sightRecheckFrame = 0; // +1212 보이는 목록을 시야선까지 다시 본 프레임(2초마다)
    uint32_t spotSoundFrame = 0;    // 갱신자료 +536 — 이 프레임이 지나야 발견 소리(RECOGNIZE)를 또 낸다
    uint16_t enemyNet = 0;          // +1196 지금 상대가 크리처면 그 번호(틱마다 `enemyPet` 자리 번호로 푼다)
    // ★ 09-29 B2 — 놀기·되갚기·소리 넷·싸움 시작(`unit_ai.go:537~620` · `AIActionIdle` · `Sub_545E60` · `531E20`·`531E90`)
    bool alert = false;             // StatusFlags ALERT(0x100) — 싸움 시작에 켜고 끝에 끈다(켜져 있으면 알아보기가 앞쪽 부채꼴을 안 본다)
    bool injured = false;           // StatusFlags INJURED(0x200) — 피해 끝에 켜고(`GAME3_2.c:7237~7242`) 제 갱신 끝에 끈다(`unit_ai.go:608`)
    SeenRef lastAttacker;           // 오브젝트 +520 Obj130 — 마지막으로 때린 쪽(주인 유닛으로 푼다)
    float hitFromX = 0, hitFromY = 0;   // 오브젝트 +528 Pos132 — 때린 것의 지난 자리(`GAME3_2.c:7185~7212`)
    uint32_t hitHandledFrame = 0;   // 갱신자료 +516 Field129 — 되갚기가 이미 본 피해 프레임
    uint32_t tauntFrame = 0;        // 갱신자료 +520 Field130 — 적을 때린 프레임(`sub_532880`) · 1초 뒤 도발 소리
    uint32_t tauntNext = 0;         // 갱신자료 +532 — 도발 소리 다음 때(2~4초)
    uint32_t idleSoundNext = 0;     // 갱신자료 +528 — 놀 때 소리 다음 때(20~60초)
    // ★ 09-29 B4 — 두뇌(`monsterMainAIFn_547210`)
    uint32_t moveAttemptFrame = 0;  // 갱신자료 +508 — 막혀서(답답함) 도망·후퇴가 멈춘 프레임 · 3초 안이면 「구석에 몰림」(`534810`)
    uint32_t stuckFrame = 0;        // 갱신자료 +496(`Field124`) — 15 넘게 움직인 마지막 프레임(답답함 재기) · 행동이 바뀌어도 새로 찍힌다(`MonsterActionReset`)
    float stuckX = 0, stuckY = 0;   // 갱신자료 +500·+504 — 그때 자리
    bool frustrated = false;        // StatusFlags FRUSTRATED(0x200000) — 조건 70 이 보고 지운다
    // ★ 09-29 B5 — 애니 칸(갱신자료 +480~+483 · `Field120_0~3`) — 칸 수 · 지금 칸 · 칸 안 틱 · 끝남.
    //   행동이 바뀌면 지금 칸·칸 안 틱·끝남이 0(`MonsterActionReset` — 칸 수는 그대로)
    uint8_t animFrames = 0, animFrame = 0, animTick = 0;
    bool animDone = false;
    // ★ 09-29 B6 — 들은 소리(갱신자료 +388 번호 · +392 낸 것 번호 · +396·+400 자리 · +404 프레임 · +408 세기 — `unitEmitHearEvent_50D110`)
    //   +404 는 조건 42(상대 잃음)가 「지금 + 1초」로 앞당겨 적어 그동안 귀를 막는다
    uint16_t heardSnd = 0, heardSrcNet = 0;
    float heardX = 0, heardY = 0;
    uint32_t heardFrame = 0;
    int heardDist = 0;
    // ★ 09-29 B7 — 몬스터 주문(갱신자료 +1488 + 4×주문 = 깃발 · +2040 세기) · 쉬는 때(+1460 제 몸 · +1468 되받기 · +1476 공격 · +1484 달아날 때)
    std::vector<std::pair<uint8_t, uint32_t>> spells;
    int spellLevel = 3;
    uint32_t castNextBuff = 0, castNextReact = 0, castNextOffense = 0, castNextEscape = 0;
};

// 깃발 하나.
//
// ⚠ **종류 번호는 실측이 아니라 이름 짐작이다** (09-07 자가검증에서 잡음).
//   종류표에 `176 RedFlag`·`173 BlueFlag` 라는 **이름이 있다**는 것뿐이고,
//   **네 덤프 어디에도 깃발 오브젝트가 0건**이다(CTF 로 돌린 적이 없다).
//   투사체 쪽에서 같은 방식으로 고른 `695 Fireball` 이 0건이었던 것과 같은 자리다.
//   ★ 다만 **둘 다 복합이 아니라는 것은 표에서 확인**했다 — 붙는 바이트가 없다.
struct Flag {
    uint8_t team = 0;          // 이 깃발의 임자 팀
    uint16_t netCode = 0;
    uint16_t type = 0;
    double homeX = 0, homeY = 0;   // 제자리
    double x = 0, y = 0;           // 지금 자리
    std::string carrier;           // 들고 있는 사람. 비었으면 바닥에 있다
    bool atHome = true;
    // 마지막으로 클라에 알린 상태 지문(상태<<16 | 든사람번호). **바뀔 때만 보내려고** 둔다.
    // ⚠ 매 틱 보내면 클라가 죽는다 — `0x5e` 를 270배 보냈던 사고(2026-09-08).
    // 0xFFFFFFFF = 아직 한 번도 안 보냄.
    uint32_t sentMark = 0xFFFFFFFFu;
};

// 묶음 8-d — 공과 골대. 깃발(`Flag`)과 짜임이 같다
struct GameBall {
    uint16_t netCode = 0;
    uint16_t type = 0;
    double homeX = 0, homeY = 0;
    double x = 0, y = 0;
    std::string carrier;
    bool atHome = true;
    uint32_t sentMark = 0xFFFFFFFFu;   // 바뀔 때만 보내려고(깃발과 같은 규칙)
};
struct BallGoal { uint8_t team = 0; double x = 0, y = 0; };

// 공격이 닿을 수 있는 것 하나 — **사람이거나 불려 나온 것**이다.
//
// 왜 한 덩이로 묶나 — 근접·투사체·소환수가 저마다 「누구를 때리나」를 따로 고르면
// 셋이 조금씩 다르게 굴어 어긋난다. 고르는 자리를 하나로 모은다.
struct Target {
    std::string addr;   // 사람이면 그 주소
    int pet = -1;       // 불려 나온 것이면 그 자리 번호
    bool none() const { return addr.empty() && pet < 0; }
};


// ★ 스트림 하나의 순번 — 실측으로 밝힌 규칙이다.
//
//   **신뢰 전송은 순번을 하나 써서 올리고, 비신뢰는 「직전 신뢰가 쓴 값」을 그대로 싣는다.**
//
// 정품 호스트의 한 접속을 풀어 늘어놓으면 그대로 보인다:
//
//   80 00 1f  신뢰   순번 0   ← 쓰고 올린다
//   00 00 04  비신뢰 순번 0   ← 직전 신뢰가 쓴 값
//   80 01 a9  신뢰   순번 1
//   80 02 2b  신뢰   순번 2
//   00 02 a9  비신뢰 순번 2   ← 다시 직전 신뢰 값
//   00 02 27  비신뢰 순번 2
//   80 03 c2  신뢰   순번 3
//   00 03 a9  비신뢰 순번 3
//
// 신뢰를 한 번도 안 보낸 스트림은 0 이다(서버 찾기만 오간 세션이 그렇다).
struct StreamSeq {
    uint8_t next = 0;   // 다음 신뢰 전송이 쓸 값
    uint8_t last = 0;   // 직전 신뢰 전송이 쓴 값 — 비신뢰가 싣는다
    // ★ 2026-09-18 페이즈 B-1b — **클라가 보낸 마지막 신뢰 순번**.
    //   정답지에서 되받기는 별도 메시지가 아니라 **답 패킷 머리의 seq 필드**다
    //   (`internal/netstr/conn.go:643~650` — 비신뢰 패킷을 받으면 그 seq 로 송신 큐를 지운다).
    //   이 값을 안 실으면 클라는 답을 못 받았다고 보고 **1초마다 영원히 재전송**한다
    //   (`send_reliable.go:135 Resend`). CLIENT_READY 가 23번 온 까닭.
    uint8_t recvSeq = 0;
    bool    recvSeqSeen = false;   // 한 번이라도 신뢰 패킷을 받았나
};

// 상대 하나. 주소:포트로 가른다.
//
// ⚠ **접속 도중에 클라가 포트를 바꾼다.** 실측에서 TRY_JOIN 은 54721 에서,
//    그 다음 CONNECT 는 63073 에서 왔다. 프로토콜이 그렇게 생겼다
//    (Docs/PROTOCOL.md 「접속 절차」의 "여기서 클라가 새 포트로 옮긴다").
//    그래서 한 사람이 세션 둘로 보인다. 지금은 그대로 둔다.
struct Session {
    std::string addr;
    uint8_t xorKey = 0;      // 0이면 아직 평문
    bool keyed = false;      // 키를 준 뒤인가
    int lastRelStream = -1;  // 클라가 마지막으로 **신뢰**를 보낸 스트림(되받기 기준). -1 이면 아직 없음
    uint32_t connectId = 0;  // SERVER_ACCEPT 에 실어 보낸 번호
    uint32_t frame = 0;      // 이 사람에게 실어 보낼 프레임
    uint16_t netCode = 0;    // 이 사람에게 준 오브젝트 번호
    unsigned long long recvPackets = 0;
    // ★★ 이 사람에게서 마지막으로 무언가를 받은 프레임. **끊긴 세션을 걷어내는 근거다.**
    //   지우는 자리가 아예 없어서 클라를 죽이면 세션이 그대로 남았고, 시험할 때마다
    //   「사람 3」 같은 유령이 끼어 판정이 흐려졌다(2026-09-09).
    uint32_t lastSeenFrame = 0;

    // 게임에 들어와 있는가 — 여기가 참이면 매 틱 상태를 받는다.
    bool inGame = false;
    // ★ 첫 세계 스냅샷을 몇 틱 뒤에 보낼지. 접속 직후 같은 묶음으로 보내면 클라가 못 쓴다
    //   (정품도 다른 조각 여덟 개가 지난 뒤 아홉째로 보낸다 — 2026-09-07 이분법).
    //   ⚠ 이 주석만 있고 **필드가 없었다.** 그래서 접속 처리에서 곧바로 보냈고 화면이 검었다.
    //     정품 조각 순서를 다시 재 보면(2026-09-08): `2b`(USE_MAP) 뒤로 `27 27 27 08 04` 가
    //     지나간 **여섯째**에 스냅샷이 온다. 그 사이가 클라가 맵을 읽는 시간이다.
    //   남은 틱 수. 0 이 되는 틱에 보내고 -1 로 잠근다(-1 = 보낼 것 없음 또는 이미 보냄).
    int snapshotIn = -1;
    // ★ 09-30 사람 판 시험용 — `--give-items` 물건을 넣을 프레임(0 = 아직 안 셈) · 넣었나
    uint32_t testItemsFrom = 0;
    bool testItemsGiven = false;
    // ★★ 이 사람에게 상태 갱신을 한 번이라도 보냈나. **첫 갱신만 머리를 전체번호(ff)로**
    //   보내고 그 뒤로는 별칭 `01` 한 바이트로 보낸다(정품 실측, 2026-09-08).
    //   ⚠ 앞서는 이 판정을 `preludeAt > 0`(서두 재생 진행도)으로 했는데, 재생을 안 쓰면
    //     늘 0 이라 **매 틱 사람을 처음 보는 것처럼 다시 소개**하고 있었다.
    bool sentFirstUpdate = false;
    // ★★ 접속 직후 클라는 **관전(observer)** 이다 — HUD(주문 5개·체력/마나 막대)가 없고
    //   화면에 자기 몸이 반투명하게만 보인다. 정품 서버는 **첫 왼클릭(입력 코드 6)** 에
    //   `6a REPORT_CLIENT_STATUS` 를 신뢰로 보내 관전을 푼다(2026-09-09 정품 대조:
    //   00:31:14 에 `cc(6a cb 02 00 00 00 00)` + `cc(ea 59 05 00 00)` 한 쌍).
    //   내 서버는 그 클릭을 근접 공격으로만 읽어 관전이 영영 안 풀렸다.
    bool observer = true;
    // 아직 서로 소개(`2d NEW_PLAYER`)를 안 주고받았다. 발송은 `Tick` 에서 한다 —
    // 접속 답장에 붙이면 정품이 그 자리에 보내는 것과 어긋난다.
    bool pendingIntro = false;
    // ★ 서두를 어디까지 흘렸나. **접속마다 따로** — 전역으로 두면 둘째 클라가 서두를 못 받는다.
    size_t preludeAt = 0;
    // ★★ 이 사람에게 흘려보낸 프레임. **접속마다 따로 센다.**
    //   전역 하나로 두면 늦게 붙은 사람의 시계가 서두(녹음본이라 늘 3219 로 고정)에서
    //   내 현재 프레임으로 크게 건너뛴다. 2026-09-08 두 클라 대조로 짚었다:
    //     첫째 3221 → 2609 (뒤로 612)   → 세계 정상
    //     둘째 3221 → 5199 (앞으로 1978 ≈ 66초) → 화면 검정
    //   별칭표(aliases)도 접속마다 따로이므로, 그 수명을 재는 프레임도 여기 것을 쓴다.
    uint32_t tickFrame = 0;
    bool tickFrameSet = false;
    // ★ 09-21 문 되맞춤 — 이 사람이 **문마다 어떤 상태로 알고 있나**(0 닫힘 = 클라의 기본 · 1 열림).
    //   `props_` 와 같은 길이. 나중에 들어온 사람이 이미 열린 문을 닫힌 것으로 보던 구멍을 막는다
    std::vector<uint8_t> doorTold;
    uint32_t doorSyncFrom = 0;          // 이 프레임부터 되맞춘다(세계를 받은 뒤 1초) · 0 = 아직 안 정함
    // 09-21 진단 — 시계 요청이 올 때 (그 사람 시계 − 서버 도장). 바뀔 때만 찍으려고 들고 있다
    long long dbgTimerDiff = 0;
    bool dbgTimerDiffSet = false;
    int x = 0, y = 0;        // 맵 위 자리
    uint8_t facing = 0;      // 바라보는 쪽. 한 바퀴를 256으로 나눈 값

    // ── 체력과 죽음 (2026-09-07) ─────────────────────────
    // 짜임과 값은 캡처에서 얻었다(Docs/PROTOCOL.md 「죽음과 부활 ✅」·「체력」 절).
    uint16_t health = 0;
    uint16_t maxHealth = 0;

    // ── 캐릭터 (2026-09-10 기획데이터 이식) ───────────────
    // 접속 정보 97바이트의 자리 51·52 에서 읽는다(`GameBalance.h` 참조).
    // 체력·마나·힘은 **직업과 레벨이 정한다** — 전에는 셋 다 위저드 값으로 굳어 있었다.
    CharClass charClass = CharClass::Warrior;
    uint8_t   charLevel = kMaxLevel;
    // ★ 클라가 보낸 접속 정보 97바이트 그대로. **남에게 이 사람을 소개할 때** 쓴다
    //   (`2d NEW_PLAYER`). 독립 모드에는 녹음이 없어 이것 말고는 만들 재료가 없다.
    // 묶음 8-e — 사람 이름. 투표가 이름으로 겸눈다(정답지 playerinfo +4704)
    //   ⚠ **체우는 길이 아직 없다.** `NEW_PLAYER` 의 **+119** 가 이름 자리로 보이는데
    //     (`netNewPlayerMakePacket_4DDA90` 이 `field_2096` 을 거기 싣는다)
    //     우리 꼬리(`kNewPlayerTail`)는 그 자리가 **0** 이고 녹음 실측도 0 이었다.
    //     ⇒ 지금은 시험이 손으로 넣어 쓴다. 채우는 것은 **따로** 할 일이다
    std::string name;
    std::vector<uint8_t> info97;
    uint16_t  maxMana   = 0;
    // ★ **지금 마나.** 이 칸이 없어서 주문이 무한이었다 — `maxMana` 는 쓰기만 하고
    //   읽는 데가 없었다. 시전할 때 깎고 `0x45` 로 알린다.
    uint16_t  mana      = 0;

    // ★ **가진 것.** 이 칸이 없어서 주울 수도 입을 수도 없었다(계획서 벽②).
    //   세계에서 집어 든 물건이 여기로 옮겨 온다.
    struct Carried {
        uint16_t netCode = 0;   // 세계에 있을 때 쓰던 번호를 그대로 들고 온다
        uint16_t type = 0;
        bool modifiable = false;
        bool equipped = false;
        // 쓰면 무슨 일이 나는가 — 세계에 있을 때 정해진 것을 그대로 들고 온다.
        int useAmount = 0;
        // ★ 겉모습(P7) — 입으면 남들에게 `0x4f`~`0x52` 로 알릴 때 실을 **장비 비트**.
        //   `EquipBitFor(이름)` 가 채운다. **0 이면 겉모습을 안 보낸다**(모르는 것은 안 지어냄).
        uint32_t equipBit = 0;
        bool isWeapon = false;
        bool weaponClass = false;   // ★ 09-27 B3 — CLASS 에 WEAPON 또는 WAND(정답지 무기 들기 첫 관문 `obj_class & 0x1001000`)
        bool wandClass = false;     // ★ 09-27 B5 — CLASS 에 WAND(0x1000 — 충전 알림 `:4977` · 지속 주문 끊기 `4FEB60`)
        uint32_t subclass = 0;      // ★ 09-27 B5 — 무기·지팡이의 SUBCLASS 비트(thing.bin · `WeaponClassBitsOf`) · 09-28 갑옷이면 갑옷 SUBCLASS(`ParseArmorClass` — 방패 2). 그 밖은 0
        uint8_t weight = 255;   // 묶음 5 — 줍기 한도를 재는 무게. 세계에 있을 때 값을 그대로 들고 온다
        bool isFood = false;    // 묶음 5 — 같은 종류 개수 한도(셋)를 재는 갈래
        uint32_t foodBits = 0;  // 묶음 5-b — SUBCLASS 먹을거리 갈래 비트(FoodClassBits)
        UseFn useFn = UseFn::None;   // 묶음 5-d — 쓸 때 부르는 함수 갈래
        // ★ 묶음 5-e — **충전**. `USE = WandCastUse 250 1.0 SPELL_PLASMA` 의 250 이 최대다.
        //   0 이면 **무한**이다(정답지가 `최대 != 0` 일 때만 깎고 알린다).
        uint8_t charges = 0, chargesMax = 0;
        uint8_t wandSpell = 0;         // 걸 주문 번호(0 = 없음)
        uint32_t wandCooldown = 0;     // 재사용 대기 프레임(초 × 30)
        uint32_t wandLastFrame = 0;    // 마지막으로 쓴 프레임(`+104`)
        DecayExempt decayExempt = DecayExempt::None;   // ★ 09-28 — 버려도 안 사라지는 까닭(세계에 있을 때 값을 들고 온다 · 기본 장비는 None)
        DropFn dropFn = DropFn::Default;               // ★ 09-28 — 버릴 때 부르는 함수 갈래(thing.bin `DROP`) — 사라짐 시계가 이것으로 갈린다
        // ★ 09-29 — 수식(모디파이어) 넷의 번호(없으면 0xff) · 오브젝트 `+692` 넷(`sub_4D82F0` 이 +4 바이트를 싣는다).
        //   하나라도 있으면 남에게 겉모습을 `51`·`52`(수식 넷 실음), 없으면 `50`·`4f` 로 알린다.
        //   평상복은 캐릭터 색이 수식이다(`UserColor1` + 색 번호 — 녹음에서 번호 0xb3 + 색) · 맵 물건은 우리가 수식을 안 읽어 없음
        uint8_t mods[4] = { 0xff, 0xff, 0xff, 0xff };
        // ★ 09-29 — 줍기 함수 갈래(thing.bin `PICKUP`) · 무기 줍는 소리(`sub_53A6C0` — main 이 클래스·재질로 정함, 0 = 없음).
        //   바닥에 놓으면 다시 WorldStatic 으로 가므로 들고 다닌다(전에는 버린 물약이 「물약 줍기」를 잃었다)
        PickupFn pickupFn = PickupFn::Default;
        uint16_t pickupSound = 0;
        // ★ 09-29 — 화살통의 쓰기 짐(AmmoUse 3바이트)은 [0] 최대 → `chargesMax` · [1] 화살 수 → `charges` 로 담는다(충전 알림이 (지금, 최대)라 같은 칸).
        //   [2] 무한 깃발은 우리 판에서 늘 0 이라 칸을 안 둔다(짐 읽기가 퀘스트면 0 으로 · 아니면 안 건드림 — 맵 파일에 없는 값)
        //   활·석궁의 쓰기 짐(BowUse 1바이트) [0] = 재장전 칸 — 들 때 0 으로(`:4984~4986`), 쏠 때 0 이 아니면 하나 줄이고 못 쏜다(`:4522~4533`)
        //   ⚠ 이 칸을 올리는 자리는 정답지에서 못 찾았다(BowUse 쓰기 함수는 비어 있다 `object_funcs.go:87`) — 그래서 늘 0
        uint8_t bowReload = 0;
    };
    std::vector<Carried> inventory;
    int       strength  = 0;       // 맨손·무기 피해 계산에 쓴다 · **무게**도 여기서 나온다(아래 PlayerMassOf)
    // ★★ 묶음 5 — **들 무게의 밑천**(정답지 오브젝트 `+490` · `CarryCapacity`).
    //   ⚠⚠ **thing.bin 의 `CARRYCAPACITY = 100` 이 아니다.** 그것은 오브젝트를 만들 때의 기본값이고
    //     (`server/object.go:330`), 사람은 접속·레벨업 때 `plrReadVals_4EEDC0` 가 **힘에서 다시 계산해
    //     `+490` 에 덮어쓴다**(`GAME3_3.c:7704`) — `(힘 ÷ 워리어 최대 힘 × 1250 + 750) × 1.5`.
    //     ⭕ 위저드 1650 은 정품 녹음 `48 REPORT_STATS` 넷째 칸과 맞다.
    //   줍기 한도는 이 값의 **두 배**다(`object_pickup.go:75`). 같은 `+490` 을 **짐 무거움 깃발**은
    //   두 배 없이 그대로 견준다(`GAME3_3.c:7714`) — 두 문턱이 다르다.
    uint16_t  carryCap  = 0;
    // ★ 묶음 5-h — **금**(사람 갱신자료 `+2164`). 가방에 안 들어가고 숫자로 쌓인다.
    //   `playerAddGold_4FA590` 이 더하고 `playerSubGold_4FA5D0` 이 뺀다(0 아래로는 안 내려간다).
    uint32_t  gold      = 0;

    // `0x0a CLIENT_CLOSE` 를 받았다 — 다음 틱에 걷어낸다(30초 타임아웃을 안 기다린다).
    bool closing = false;

    // ★ **겨눈 자리** — `0xac MOUSE` 가 나르는 월드 좌표다(⭕ 정품 표본 18,700+).
    //   투사체가 여기를 향해 난다. 겨눔이 없거나 낡았으면 몸 방향으로 되돌아간다.
    //
    // ⚠ **「없음」을 `aimAtFrame == 0` 으로 나타내지 않는다.** 0 은 진짜 프레임 번호라
    //   첫 틱 전에 받은 겨눔이 영영 「없음」으로 읽힌다 — 시험이 그걸 잡았다.
    //   같은 함정을 `closing` 에서도 한 번 밟았다(2026-09-10, 하루에 둘).
    // ★★ **걷기 물리** (2026-09-13 묶음 2-a — 정답지 `object_update.go` unitUpdatePlayerImplA/B ·
    //   `GAME4_1.c:3166` nox_xxx_updateObjectsVelocity_5118A0 이식).
    //   전에는 이동 입력 **한 통마다** 7 유닛을 바로 옮겼다(`kStepPerTick`, 속도는 실측 못 한 값).
    //   정답지는 입력이 **상태**만 바꾸고, 틱마다 상태에 따라 힘을 주고 속도를 적분한다.
    double posX = 0, posY = 0;     // 실수 자리(+56) — x·y 는 이것을 반올림한 값. 밖에서 x·y 를 바꾸면 다시 맞춘다
    double velX = 0, velY = 0;     // 속도(+80 VelVec)
    uint8_t moveState = 13;        // 정답지 PlayerState — 0 걷기 · 5 달리기 · 13 가만(`GAME4.c:1709` 대응표)
    bool inputThisTick = false;    // 이번 틱에 조작 입력이 하나라도 왔나(정답지 Control 목록이 비었나)
    bool moveInputThisTick = false;// 이번 틱에 이동 코드(2~5)가 왔나
    // ★★ **공격 상태** (2026-09-13 묶음 3-a — 정답지 `GAME4.c:1588` playerInputAttack · `GAME4_3.c:3855` playerAttack ·
    //   `object.go:1394` playerSetState 이식). moveState 에 상태 1(공격)이 더해진다.
    //   전에는 왼클릭 한 통에 **바로** 때리고 15틱 쉬었다(kMeleeCooldownTicks, 우리 값).
    //   정답지는 클릭이 상태만 1로 바꾸고, 틱마다 애니 칸을 세다가 **애니가 끝나는 틱**에 판정한다.
    bool actionThisTick = false;   // 이번 틱에 CCAction(코드 6)이 왔나
    bool jumpThisTick = false;     // ★ 3-a — 이번 틱에 CCJump(코드 7 — 원본 기준 · 와이어 미실측, `Server.cpp` 받는 자리 주석)가 왔나
    // ★ 3-a — 오브젝트 깃발 0x4000 AIRBORNE(떠 있음). 점프가 켜고(`object_update.go:567`) 점프 끝·가만(13)·부활이 끈다
    //   (`:323` · `:329` · 부활 `&= 0xFFEB3FE7`). 사람의 오브젝트 깃발 중 **지금 켜는 것은 이것 하나**다(`PlayerObjFlags`)
    bool airborne = false;
    uint8_t attackAnim = 0;        // 플레이어 +8 (Field8) — 공격 애니 번호(23·24 주먹 · 25 훅). 0 이면 새로 뽑는다
    uint32_t attackReadyFrame = 0; // 갱신 데이터 +0 (Field0) — 이 프레임 전에는 상태 1 로 못 들어간다. 0 이면 바로 가능
    uint32_t stateFrame = 0;       // 오브젝트 +136 (Field34) — 상태가 바뀐 프레임
    uint8_t animFrame = 0;         // 갱신 데이터 +236 (Field59_0) — 스냅샷에 실는 애니 칸
    uint8_t facingPrev = 0;        // 오브젝트 +124 (Direction1) — 지난 틱 끝의 바라보는 쪽. 공격 방향 판정이 이것을 쓴다
    uint8_t facingInput = 0;       // 이번 틱에 온 CCOrientation — ImplB 에서 facing 으로 옮긴다
    bool hasFacingInput = false;
    double forceX = 0, forceY = 0; // 남이 준 힘(+88 ForceVec 몫) — 적분 때 더하고 지운다
    double prevX = 0, prevY = 0;   // +72 PrevPos — 지난 틱 시작 자리(막기 판정이 때린 쪽의 이 자리를 본다, 3-e2)
    // ★ 2-b2 — `unitMove`(SetPos)가 이번 틱에 옮겼나. 옮겼으면 틱 끝 PrevPos 가 **옮긴 자리**다
    //   (SetPos 가 PosVec·PrevPos 를 둘 다 그 자리로 적고 CCC 가 PrevPos = PosVec 을 하므로 · `object.go:705~707` · `server.go:163`)
    bool setPosThisTick = false;
    double tickStartX = 0, tickStartY = 0;   // ★ 2-b2 — 이번 틱 시작 자리(+56 PosVec — 적분 중에는 안 바뀐다). 땅울림 자리가 이것이다
    // ★ 2-b2 — 마지막으로 닿은 벽 칸(+748 → +296 · `GAME5.c:6692` · `:6720`). 벽 선분에 가장 가까운 점을 찾을 때마다 덮어쓴다 —
    //   **지우는 자리는 정답지에 없다.** 광전사가 벽에 부딪히면 이 칸의 재질 깃발로 「세게 · 살짝」을 가른다
    int16_t lastWallGx = -1, lastWallGy = -1;
    int64_t debugWeapon = -1;      // 시험용 바꿔 들기 — −1 기본 장비 · 0 맨손 · 그 밖 kWeapons 클래스 비트(주워 들기는 묶음 5)
    // ★ 09-27 B2 — 든 무기(플레이어 +748 → +104). 가방 항목 번호로 들고, 읽을 때마다 가방에서 찾는다(버리거나 되돌려 없어지면 맨손).
    //   0 = 없음. `gearInBag` 은 기본 장비를 가방에 실었나 — 못 실은 사람(종류 틀이 없는 서버 = 시험)은 옛 방식대로 직업 표에서 읽는다
    uint16_t weaponNet = 0;
    bool     gearInBag = false;
    // ★ 기력(+91 Field22_3, 3-b) — 만들 때 0(`GAME4_1.c:732`), 살아 있으면 틱마다 100 아래일 때 100/30 = 3 씩(`object_update.go:108`).
    //   무기 공격이 무기별로 깎는다(weaponGetStaminaByType_4F7E80 — 롱소드 70). 깎을 때 `47 <기력>` 을 중요로 알린다.
    uint8_t stamina = 0;
    // ★ 피해 소수 이월(+84, 3-e) — 방어로 깎인 피해의 소수부를 다음 피해에 더한다(`GAME3_3.c:468` playerDecrementHPMana)
    float dmgRemainder = 0.0f;
    // ★ 표식 네 자리(playerinfo +116 + 4×칸) — 정답지는 `TeleportGlyph1~4` **오브젝트**를 세워 두고
    //   그 오브젝트 자리를 쓴다. 우리는 자리만 담는다(그림은 묶음 4-e 글리프에서).
    //   칸 = `주문 − 46`(표식 걸기 46~49) = `주문 − 122`(표식으로 가기 122~125).
    bool markSet[4] = { false, false, false, false };
    float markX[4] = { 0, 0, 0, 0 }, markY[4] = { 0, 0, 0, 0 };
    uint8_t lastDamageType = 0;    // +524 — 마지막 피해 종류(damageDefaultProc). 16 에 죽으면 먼지 자세 51(`GAME4_1.c:4459`)
    uint16_t hurtPrevHp = 0;       // ★ 09-24 — 「윽」 재기의 지난 체력(`ud.Field19_0`)
    uint8_t hurtCooldown = 0;      // ★ 09-24 — 「윽」 뒤 쉬는 틱(`ud.Field19_1` — 줄었으면 7)
    uint32_t debugGuideAt = 0;     // 진단 --debug-conjurer-guides — 안내서를 줄 프레임(0 = 아직 못 봄)
    // ★ 죽음 알림(`a9 0e`, 3-d)에 실을 것 — 플레이어 +304 갈래 · +300 종류(`GAME3_3.c:164~246 · 344~437`).
    //   피해 함수가 부를 때마다 0 에서 다시 적는다: 갈래 1 = 종류 번호(무기·투사체 · 없으면 맨손 10·망치 2 때 때린 이) · 2 = 피해 종류
    uint8_t killKind = 0;
    uint16_t killType = 0;
    // +520 마지막으로 때린 것 — damageDefaultProc 가 적는다(`GAME3_2.c:7229~7233`: 무기·투사체가 있으면 그것, 없으면 때린 이). 부활 때 지운다
    struct LastHit {
        uint8_t cls = 0;          // 0 없음 · 1 사람 · 2 몬스터 · 3 무기·투사체
        uint8_t ownerCls = 0;     // cls 3 의 주인 — 0 없음 · 1 사람 · 2 몬스터
        uint16_t monsterType = 0; // 몬스터(또는 주인 몬스터)의 종류 번호
        std::string player;       // findParentChainPlayer — 주인 사슬에서 찾은 사람 주소(없으면 빈)
    } lastHit;
    // +3600/+3604/+3608 도운 사람 — sub_4E7540(혼란·기절 등 공격 버프를 건 사람) · 죽음 알림이 10초 안이면 싣는다(4-a4)
    bool assistSet = false;
    std::string assistBy;
    uint32_t assistFrame = 0;
    // +232 마지막으로 알린 방어값 — playerReportAnything 이 +228 과 다르면 `49` 를 보낸다(3-d)
    float armorReported = 0.0f;
    // ★ 시전(묶음 4-a · spellByBookInsert_4FE340 · spellCastByBook_4FCB80) — 갱신 데이터 자리
    uint8_t castFlag = 0;          // +188 — 시전 중 1
    uint32_t castFrame = 0;        // +216 — 시전을 넣은 프레임. 0 이 아니면 새 시전을 안 받는다
    int castLeaf = 0;              // +184 — 음소 나무에서 지금 자리(0 = 뿌리)
    uint32_t glyphSpells[5] = {};  // +192~+208 — 글리프에 넣을 주문(마나 선불)
    uint8_t glyphCount = 0;        // +212
    float castCursorX = 0, castCursorY = 0;   // +220/+224 — 시전이 끝난 순간의 커서 자리(playerinfo 571/572)
    uint8_t spellLvl[137] = {};    // playerinfo +3696 SpellLvl — 온라인은 전부 3(serverSetAllSpells, EngineAdmin)
    bool hasAim = false;
    int aimX = 0, aimY = 0;
    uint32_t aimAtFrame = 0;
    uint32_t respawnAtFrame = 0;   // 0 이면 살아 있다
    // ★ **부활 직후 무적이 끝나는 프레임.** 살아나자마자 또 죽는 것을 막는다.
    //   ⚠ 「없음」을 0 으로 겸하지 않는다 — 0 은 진짜 프레임 번호라, 첫 틱에 되살아난
    //     사람이 영영 무적이 된다. 오늘 같은 함정을 두 번 밟았다(`closing`·`hasAim`).
    // ★ 마나를 한 번이라도 알렸나. **접속 묶음은 마나바를 안 채운다** —
    //   화면 캡처(`g1_A_join.png`)에서 체력 막대는 가득한데 마나 막대는 **테두리만** 있었다.
    //   클라는 `0x45` 를 받아야 `cliSetMana` 로 그 값을 세운다(`cdecode.c:742-754`).
    //   ⚠ **접속 묶음 바로 뒤에 보내면 안 된다** — 정품 덤프와 바이트가 밀려
    //     `replay_connect` 가 깨진다(2026-09-11 실패로 확인). 그래서 **첫 틱**에 보낸다.
    // ★★ **걷고 있나** — 갱신 항목의 애니 번호를 IDLE(0)/WALK(4) 로 가르는 데 쓴다.
    //   ⭕ 번호표는 소스 확증(`draw_debug.go:294` debugStates) —
    //     0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · **4 WALK** · 6 RUN
    //   지금까지 늘 `0x40`(하위4비트=0=IDLE)을 보내 **걸어도 선 자세**였다.
    //   사용자가 말한 「걷기·뛰기 애니메이션 없음」이 이것이다.
    //   ⚠ 정품이 걷는 표본은 없다(덤프 6개 전수 정지). 그래서 **실험으로** 확인한다.
    double animPrevX = 0, animPrevY = 0;
    bool animMoving = false;
    // ★★ **걷는 그림 프레임** (2026-09-11, 4차 R2①).
    //   ⭕ 정품은 `0x6B REPORT_ANIMATION_FRAME`(7바이트: 번호2 + 프레임u32)로 보낸다
    //     (`GAME3_2.c` netReportAnimFrame_4D81F0 · Go `network.go:734` 가 `Field33` 을 싣는다).
    //     클라는 그 값을 드로어블의 프레임 자리에 넣는다(`spriteSetFrameMB_45AB80` → +308).
    //   ⭕ 보내는 때도 확인했다 — 정품은 오브젝트마다 **「바뀐 것」 비트**를 두고
    //     `0x10000` 이 켜진 것만 보낸다(`GAME4_2.c:7149` netUpdateObjectSpecial).
    //   ⚠ **우리는 이 경로가 통째로 없었다.** 갱신 스트림의 `0x40` 은 bit7=0 이라
    //     프레임 바이트도 안 따라간다 → **프레임이 0 에 고정**돼 자세가 안 바뀌었다.
    //   ⚠ 프레임 값의 범위는 **모른다**(스프라이트마다 다르다). 작게 굴려 보고 화면으로 확인한다.
    // ★ 갱신 알림 정답지화(09-14) — playerReportAnything_4D9900(`GAME3_2.c:4855`)이 매 틱 「바뀐 것만」 알린다.
    //   전에는 `42`·`43`·`45` 를 피해·주문·물약·오벨리스크 자리에서 곧바로, HUD 는 접속 뒤 다섯 번 되풀이해 보냈다.
    uint16_t healthReported = 0;   // +10   마지막으로 알린 체력 — 다르면 `43 <체력2>`(Packet1 · netSendPlrHealthToTeam_4D86E0)
    uint16_t manaPrev = 0;         // +6    이전 마나 — playerManaAdd/Sub/Refresh 가 남긴다 · +4 와 다르면 `45`(sub_4E5450 · 워리어 제외)
    // ── 묶음 7 성장 — 경험치(오브젝트 +28 float)와 레벨(사람자료 +3684 바이트)
    //   레벨 첫값 **1** — 정답지가 글자파일을 열기 **전에** 1 로 되돌린다(`sub_4EFF10` `GAME3_3.c:8098`).
    //   우린 글자파일을 안 읽으므로 그 값이 그대로 남는다
    // 묶음 7-d — 능력 다섯. 레벨은 `spellLvl[1..5]` 를 같이 쓰고, 여기엔 쉬는 시간과 켜짐만 둔다
    uint32_t abilCooldown[kAbilityCount] = {};     // unitAbilities.Cooldowns
    bool     abilActive[kAbilityCount] = {};       // ExecList 에 얹혀 있나
    // ★ 09-23 nine-defects 2-a1 — 목록 칸의 **켜짐 값**(`ExecAbilityClass.Active`). 얹을 때 1 이고,
    //   함성 애니가 끝나면 `Sub4FC440` 이 0 으로만 내린다(목록에는 남는다). `IsActiveVal` 이 이것을 본다
    bool     abilActiveVal[kAbilityCount] = {};
    uint32_t abilActiveUntil[kAbilityCount] = {};  // ExecAbilityClass.Frame — 이 프레임을 **넘으면** 끝
    bool     overweight = false;                   // +3656 짐 넘침 — 광전사 돌진을 막는다
    // 묶음 7-f 작살 — 사람 자료 +132(대상) +136(화살) +140(frame35) +144(대상 자리) +152(frame38)
    // 묶음 7-h 안내서 — 사람 자료 +4244 `BeastScrollLvl[41]`. 레벨이 아니라 **있다/없다**다
    uint8_t  guideLvl[41] = {};
    uint16_t harpoonBoltNc = 0;                    // 0 = 줄이 없다
    Target   harpoonTarget;                        // 꽂힌 상대(없으면 none)
    float    harpoonTargX = 0, harpoonTargY = 0;   // 마지막으로 본 상대 자리
    uint32_t harpoonFrame35 = 0;                   // 0 이면 「걸림」을 아직 안 알렸다
    uint32_t harpoonFrame38 = 0;                   // 30틱마다 상대가 움직였나 보는 시계
    // 묶음 7-c — 점수(+2136 `Lessons`)와 죽은 횟수(+2140). 둘 다 `4e` 한 통에 실린다
    int32_t score = 0;
    uint32_t deaths = 0;
    float experience = 0.0f;
    uint8_t level = 1;
    bool statsDirty = false;       // +2184 plrReadVals_4EEDC0 끝에 1 → `dd`·`de`·`48` 한 벌(접속 때는 스냅샷 꼬리가 그 몫)
    uint32_t lastDamageFrame = 0;  // +536  마지막 피해 프레임 — `42` 증감은 이 뒤 2틱 넘어야(netPlayerObjSend_518C30 `GAME4_1.c:4412`)
    std::map<std::string, uint16_t> hpReportedTo;   // +12+2×i 보는 사람마다 마지막으로 알린 체력 — 부활 때 서른둘 = 지금(`GAME3_3.c:7937`)
    Buffs buffs;                   // ★ 4-a4 버프 — 부활 무적(전 invulnUntil 자리표시)도 버프 23 으로
    uint8_t  poison = 0;           // +540 독 남은 양(셀수록 자주 아프다 · 4-c2)
    uint32_t poisonStart = 0;      // 체력자료 +16 독이 시작한 프레임(0 = 없음) — 60틱 유예에 쓴다
    uint16_t poisonDecay = 0;      // +542 감쇠 계수기 — 독을 걸 때 1000, 0 이 되면 독 1 줄고 다시 1000
    float speedCur = 3.0f;         // +544 SpeedCur — CCC 가 매 틱 셈(느림·빠르게, 4-b3) · NewPlayer SPEED 96/32
    uint16_t seqImpId = 0;         // 0xcc 껍데기의 번호. 실측도 1씩 오른다
    // ★★★ **중요 메시지 대기열** (2026-09-13 — 정답지 `nox_xxx_netSendPacket_4E5030`·`netImportant_4E5770` 이식).
    //   EmitSeq 는 바로 안 보내고 여기 쌓는다. 틱마다 `[27 시각][aa][cc…]` 한 조각으로 싣고,
    //   클라가 `ab <프레임4>` 로 확인하면 그 프레임에 실은 것을 지운다(`nox_net_importantACK_4E55A0`).
    //   확인이 없으면 약 60틱 뒤 다시 싣는다.
    //   왜 — 전에는 한 번만 보냈다. 클라는 **접속 완료 전의 cc 를 버리고**(`client__network__cdecode.c:2550`)
    //   기다리는 번호가 끝내 안 오면 **뒤 번호를 영영 쌓아만 둔다**(`GAME2_3.c:531~564`, 30초 건너뛰기는
    //   꺼내다가 빈틈에서 멈춘 적이 있을 때만 돈다 — 첫 번호부터 없으면 영영 안 돈다). 09-13 로그: 순번 0~4 가 프레임 1849 에 나갔고 첫
    //   CLIENT_READY 는 1851 — 그 뒤 **모든 cc 가 적용 안 됐다**(친구 알림 빨강 · 맨 메시지로 보내면 파랑).
    struct PendingImportant {
        std::vector<uint8_t> wire;     // cc 머리까지 붙은 그대로
        uint32_t sentFrame = 0;        // 마지막으로 실은 조각의 시각(클라 확인과 맞춰 본다)
        uint32_t wait = 0;             // 다시 싣기까지 남은 틱
        bool sent = false;
    };
    std::vector<PendingImportant> importantQ;
    // 정답지는 CLIENT_READY 를 받아 상태 비트 0x10 을 세운 뒤에만 중요 메시지를 싣는다
    //   (`GAME3_2.c:5602` gameServerReadyMB → `GAME3_3.c:1966` 의 `v3[3680] & 0x10`).
    bool importantReady = false;
    // 시험용 — 켜면 예전처럼 **바로** 한 통씩 보낸다(대기열·확인 없음). DebugEnterGame 만 켠다.
    //   메시지 **내용**을 보는 시험 수십 곳이 처리 직후 out 을 들여다본다. 전송 경로는 TestImportantQueue 가 따로 잰다.
    bool importantImmediate = false;
    //   ★ 09-30 — 이 시험용 즉시 모드이면 **묶지도 않는다**(아래 묶음 칸 — 같은 까닭: 처리 직후 out 을 보는 시험들)
    // ★ 09-30 **한 틱 한 데이터그램** — 정답지 `sendPoll`(`network_lists.go:177~227`)의 차례 그대로 칸 넷에 모았다가
    //   틱 끝 `FlushBundles` 가 한 조각으로 낸다: 시각 `27`(Kind2 맨 앞) → 갱신 `a4` → 잡다(Kind1 — `ab`·소리 등 넣은 차례) → `aa`+중요
    std::vector<uint8_t> bundleTs, bundleUpd, bundleMisc, bundleImp;
    std::vector<uint32_t> bundleMiscEnds;   // ★ 09-30 조각 2 — 잡다 칸 메시지마다 끝 자리(넘칠 때 메시지 경계에서 끊는다)
    uint32_t importantAcked = 0;   // 진단 — 확인받아 지운 수
    uint32_t importantResent = 0;  // 진단 — 다시 실은 수
    uint8_t team = 0;              // 0 = 무소속 · 1 = 빨강 · 2 = 파랑
    // ★ **접속한 차례.** 팀을 번갈아 나눌 때 쓴다.
    //   ⚠ `sessions_` 를 그냥 돌면 **주소 문자열 사전순**이라 들어온 차례가 아니다 —
    //     포트가 어떻게 뽑히느냐에 따라 A·B 팀이 뒤집혔다(2026-09-11 감사).
    uint32_t joinOrder = 0;
    // ★ 9-a — 「이 비밀벽을 열린 것으로 알고 있나」. 정답지는 마디 `+28` 에 **사람 번호 비트맵**을
    //   두지만(사람 32칸), 우리 서버는 사람을 주소로 잡아 번호가 없다 — 그래서 **사람 쪽에** 둔다.
    //   담는 값도 하는 일도 같다(비밀벽 자리표 → 0/1)
    std::vector<uint8_t> secretSeen;
    // ★ 10-e — 관리자 비밀번호를 맞혔나(`Sub_4D1210` 자리)
    bool sysop = false;
    // ★ 7-c — 옛 임시 카운터(`kills`·`deaths`)를 **정답지 자리로 합쳤다**.
    //   점수는 위의 `score`(+2136 `Lessons`), 죽은 횟수는 `deaths`(+2140).
    //   옛것은 자살에도 죽은 횟수를 올렸는데 `playerUpdateScore_54D980` 은 **안 올린다**

    // ★★★ **이 사람에게 아직 못 보낸 「바뀐 것」 목록** (R4 — 정품 더티 비트 틀을 옮긴 것).
    //
    //   ⭕ 정품은 오브젝트마다 **플레이어별 비트 한 칸**을 두고(`a2[플레이어번호+140]`),
    //     값이 바뀐 쪽이 비트를 세운 뒤 **켜진 것만 보내고 비트를 끈다**
    //     (`GAME4_2.c:7136-7181` `netUpdateObjectSpecial_527E50`).
    //
    //   ⚠ 우리는 오브젝트가 `Creature`·`WorldProp`·`Flag`·`Projectile` 로 **갈라져 있고**
    //     세션은 주소로 찾는 `map` 이라, 오브젝트마다 칸을 두기가 나쁘다.
    //     그래서 **반대로 세션에 「보낼 것 목록」을 둔다.** 얻는 것 셋 —
    //       ⑴ 오브젝트가 갈라져 있어도 **붙일 자리가 한 군데**다
    //       ⑵ 시야(AOI)가 이미 세션 기준이라 **「볼 수 있는 사람에게만」이 공짜**다
    //       ⑶ 정품의 「보낸 뒤 비트 끄기」가 **목록 비우기**로 자연스럽다
    std::vector<PendingReport> pending;

    // 이 상대가 **자기 쪽에서 굴리고 있을** 별칭 표. 서버가 똑같이 굴려야
    // 어긋나지 않는다(Alias.h 참조).
    AliasTable aliases;

    // 정적 오브젝트를 이 사람에게 **마지막으로 소개(ff)한 프레임** (P4-b).
    //
    // ★★ 소개 스로틀의 근거다 — 스냅샷이 소개한 물체를 갱신이 매 틱 다시 소개하면
    //   (30/s × 20개) 클라 게임 루프가 **첫 프레임에 멎는다**(2026-09-10 ao2~ao4 실측:
    //   입력이 1,499B 로 한 번에 토해지고 침묵·화면 검정). 클라는 스냅샷 소개만으로
    //   별칭을 각 1번 알려 주므로(pa2 실측: 정적 20개 a5 각 ×1) 재소개가 필요 없다 —
    //   별칭이 아직/다시 없을 때만, 물체당 2초에 1번으로 눌러 소개한다.
    std::map<uint16_t, uint32_t> staticIntroAt;

    // 이 사람에게 **지금 보이는 중인** 크리처 번호 (2026-09-13, `--creature-aoi` 일 때만 쓴다).
    // 정답지는 오브젝트 쪽에 플레이어별 비트(+148)로 든다(`GAME4_1.c:4488`). 여기선 사람 수가 적어
    // 사람 쪽에 든다. 보이다가 빠지면 `OBJECT_OUT_OF_SIGHT` 를 보낸다.
    std::set<uint16_t> visibleCreatures;

    // 이 사람에게 **친구로 알린**(OBJECT_FRIEND_ADD 를 보낸) 번호 (2026-09-13, `--friends` 일 때만).
    // 정답지는 오브젝트 쪽 +144 비트로 든다. 바뀔 때만 ADD/REMOVE 를 보내려고 들고 있다.
    std::set<uint16_t> friendsSent;
    // 이 사람에게 마지막으로 `36 RESET_FRIENDS` 를 보냈을 때의 팀. 팀이 이것과 다르면 다시 보낸다.
    uint8_t friendResetTeam = 0;

    // 스트림마다 따로 센다. 핑·퐁이 스트림 1 을 쓰는 것이 실측으로 확인됐다.
    StreamSeq seq[128];
};

// 접속할 때 주는 최대 체력 — **이제 직업·레벨이 정한다**(`GameBalance.h`).
//   dd d0 02 4b 00 4b 00   번호 720 · 75/75      (`0xDD REPORT_TOTAL_HEALTH`)
// ★ 그 75 는 「기본값」이 아니라 **위저드의 아레나 최대 체력**이었다
//   (`ARENA WizardMaxHealth = 75`). 「다른 상대는 150」도 풀린다 — 워리어였던 것이다.
//   여기 상수는 직업을 아직 못 읽은 자리(대조 장치 등)의 되돌림 값으로만 남긴다.
constexpr uint16_t kDefaultMaxHealth = 75;

// 죽고 나서 부활까지 ◑ — 30틱 + 15틱 = **45**. `GameBalance.h` kRespawnMinTicks 참조.
// ★ 전에 쓰던 150 은 **부활 직후 무적 시간**이었다. 값은 실재하는데 뜻이 달랐다.
constexpr uint32_t kRespawnDelayTicks = kRespawnMinTicks;

// 날아가는 것이 사라지기 전까지 가는 거리 ⭕ — **투사체 수명 × 속도**다.
//   목표를 안 문 주문은 `ARENA UnTargetedSpellLifetime = 15` 프레임을 산다
//   (gamedata.bin · opennox GAME4_3.c:5787). 한 틱에 12.0 이니 **180 유닛**.
// ★ 전에 쓰던 210 은 「걸음 서른 걸음쯤」으로 지어낸 값이었다.
// ⚠ 이름이 「주문 사거리」지만 실제로 쓰는 곳은 **투사체 비행 한계** 한 자리다.
constexpr double kSpellRange = kProjectileMaxTravel;

// ── 근접 공격 (왼버튼) ──────────────────────────────────────
//
// **왼버튼이 `CCAction`(6)이라는 것은 실측이다** — 두 출처가 맞았다:
//   ① 09-07 조작 실측 — 왼버튼을 누르니 코드 6 이 눌린 횟수만큼 왔다
//   ② opennox `noxnet/msg_player_input.go:210` — `CCAction = CtrlCode(6)`
// ⚠ 다만 **줍기는 아니다.** 왼버튼으로 물건 위에서 눌러도 `0x73 TRY_GET` 이 안 나갔다
//   (2회 시도 실패로 확인). 줍기는 다른 길이다.
//
// ⚠⚠ **아래 세 숫자는 실측이 아니다.** 주문 쪽과 같은 사정이다 —
//    09-06 덤프의 `0x42` 표본이 무슨 공격이 만든 값인지 가릴 수 없었다.
//    닿는 거리는 「검 한 자루쯤」, 다시 때리기까지는 「초당 두 대쯤」으로 둔 자리표시다.
// ── 날아가는 것 (투사체) ────────────────────────────────────
//
// 왜 필요한가 — 시전하는 순간 맞으면 **피할 수가 없다.** 녹스에서 파이어볼은
// 날아가고, 옆으로 비키면 안 맞는다. 그게 있어야 싸움이 싸움이 된다.
//
// ⚠⚠ **아래 둘도 실측이 아니다.** 덤프에 투사체의 자리가 안 실린다 —
//    `0xa4` 상태 갱신에 오브젝트로 들어오지만 그것이 파이어볼인지 가릴 종류 번호를
//    아직 못 짚었다. 걸음(7/틱)보다 서너 배 빠른 값으로 두었을 뿐이다.
// ── 불려 나온 것 (소환) ─────────────────────────────────────
//
// ★★ **이제 monster.bin 의 아레나 값을 쓴다**(`GameBalance.h` kMonsters).
//    종류 번호로 찾아 체력·피해·사거리·속도를 그 몬스터 것으로 준다.
//    아래 넷은 **표에 없는 종류의 되돌림 값**으로만 남는다(전에는 이것이 전부였다).
// ⚠ 속도는 thing 눈금(32로 나눠 쓴다) — Imp 120 → 3.75 유닛/프레임.
constexpr double kPetSpeed = 5.0;          // 되돌림 — 한 틱에 다가가는 거리
constexpr double kPetReach = 40.0;         // 되돌림 — 이 안에 들어오면 때린다
constexpr int kPetDamage = 8;              // 되돌림
constexpr uint16_t kPetHealth = 40;        // 되돌림

// ── 아레나 (점수 겨루기) ────────────────────────────────────
//
// ⚠ **점수 한도는 실측이 아니다.** 콘솔의 `set deaths {n}` 으로 서버가 정하는 값이고
//   (→ `Docs/CONSOLE.md`), 우리 캡처는 `Game Type: Arena` 에 한도 없이 돌았다.
//   열 명 죽이면 끝나는 것으로 두었을 뿐이다.
constexpr uint16_t kArenaKillLimit = 10;

// ★ 이 프레임 수만큼 아무것도 안 오면 세션을 걷어낸다. 30Hz 기준 30초. 8초로 뒀더니 **진짜 클라도 걸렸다**(2026-09-09).
//   정품 클라는 조용해도 핑을 계속 보내므로 살아 있으면 걸리지 않는다
//   (실측: 접속당 `0x06 CLIENT_PING` 이 꾸준히 온다).
// ⭕ 묶음 10-d — 정답지 `server.go:440` 은 **입력을 받는 중**(상태 비트 0x10)이면 **90초**,
//   아직 아니면 **30초**를 준다. 우리는 그 비트를 `inGame` 으로 대신한다
//   (`netOnPlayerInput` 이 그 비트가 없으면 입력을 버린다 — `server/network.go:178`).
constexpr uint32_t kSessionTimeoutTicks = 900;          // 30초 — 아직 판에 안 든 사람
constexpr uint32_t kSessionTimeoutTicksInGame = 2700;   // 90초 — 판에 든 사람

// 판 시계를 얼마마다 뿌리나. 실측 표본은 **2초 남짓 간격**이었다
// (15:05:08.299 · 10.538 · 12.709 · 14.867 — 2.2초씩). 30Hz 기준 그만큼으로 둔다.
// ✖ 09-21 폐기 — 판 시계를 **주기로 보내지 않는다**(정품 덤프·정답지 둘 다 「바뀔 때만」).
//   근거와 수치는 `Server.cpp` 의 「판 시계」 자리와 `Docs/PROTOCOL.md` 의 09-21 절.

// ── 깃발 (CTF) ──────────────────────────────────────────────
//
// ⚠⚠ **이 절에는 실측이 하나도 없다.** 종류 번호마저 이름 짐작이다 —
//    종류표에 `176 RedFlag`·`173 BlueFlag` 라는 이름이 있을 뿐 덤프에는 0건이다.
//    (표에서 확인한 것은 **복합이 아니라는 것** 하나뿐이고, 그건 짜임에만 쓴다.)
//
//    까닭은 하나다 — 우리 캡처는 `Game Type: Arena` 하나뿐이고
//    (→ `Docs/CONSOLE.md`), OpenNox 전용 서버는 **CTF 맵을 못 올린다**
//    (`panic: server should not load GUI` — 09-07 확인). 그래서 CTF 트래픽 표본이 0이다.
//    깃발 자리·닿는 거리·득점 한도·팀 배정 규칙은 **게임이 도는지 보려고 둔 자리표시**다.
constexpr uint16_t kFlagTypeRed = 176;
constexpr uint16_t kFlagTypeBlue = 173;
// ★ 닿는 거리는 **거리 상수가 아니라 부딪힘**이었다 — 깃발도 사람도
//   `EXTENT = CIRCLE 10`(⭕ thing.bin) 이니 두 반지름의 합 20 이다.
constexpr double kFlagTouchRange = kFlagTouchDist;
constexpr uint16_t kCaptureLimit = 3;

// ── 묶음 8-d 플래그볼 ⭕ thing.bin ───────────────────────────────
//   `GameBall` 241(COLLIDE BallCollide) · `GameBallStart` 243 · `Flag`(골대) 179.
//   ⚠ CTF 깃발(176·177)과 **다른 것**이다 — 저건 깃발, 이건 골대다
constexpr uint16_t kGameBallType = 241;

// ── 묶음 8-e 투표 ⭕ blob_587000.dat ─────────────────────────────
//   강퇴(갈래 0·1)에 필요한 표 = **4**(오프셋 229980).
//   곁들여 229984 기본 4 · 229988 `resetQuestMinVotes` 6 · 229992 `kickQuestPlayerMinVotes` 5.
//   갈래 2·3 은 `sub_506A20` 이 **숫자 6 을 그대로** 넣는다
constexpr uint8_t kVoteKickMinVotes = 4;

// ── 묶음 8-f 모드별 죽음 셈 ⭕ thing.bin · gamedata.bin ───────────
//   왕관 `Crown` 종류 **242**(무게 1 · `PICKUP = CrownPickup`) — ⭕ estate 맵에도 **3개** 있다.
//   ⭕ gamedata.bin 「# KotR definitions」 — 졸이 왕을 **1** · 왕이 졸을 **1** · 왕이 왕을 **3**
constexpr uint16_t kCrownType          = 242;
constexpr int32_t  kKotrPawnKillsKing  = 1;
constexpr int32_t  kKotrKingKillsPawn  = 1;
constexpr int32_t  kKotrKingKillsKing  = 3;

// ── 퀘스트 ──────────────────────────────────────────────────
//
// 「불려 나온 것」과 「맵에 사는 몬스터」는 **같은 것**으로 다룬다 —
// `Creature` 의 주인(`owner`)이 비어 있으면 아무 편도 아니고, 그래서 모두를 노린다.
// 사람 쪽에서도 주인이 자기가 아니니 때릴 수 있다. 규칙 하나로 둘이 풀린다.
//
// ⚠ **마릿수·자리·체력은 실측이 아니다.** 퀘스트 맵(`G_*`)을 올려 본 적이 없다 —
//   우리 캡처는 `estate.map`(Arena) 하나뿐이다.
constexpr int kQuestMonsters = 5;
constexpr double kQuestSpread = 300.0;   // 이만큼 벌려 세운다

// ★ 파이어볼 속도 ⭕ — `thing.bin Fireball SPEED = 192` ÷32 후 투사체라 ×2 = **12.0**.
//   전에 쓰던 26 은 「걸음보다 서너 배」로 둔 자리표시였다.
constexpr double kProjectileSpeed = kFireballPerFrame;
// ★ 맞는 반경 — 파이어볼은 점(`EXTENT = CENTER`)이고 대신 **광역**이다.
//   광역 안쪽 15 는 고정, 바깥쪽은 세기÷3. 여기는 「닿았나」를 가리는 값이라
//   광역 안쪽(15)에 사람 반지름(10)을 더해 둔다.
constexpr double kProjectileHitRadius = kFireballSplashInner + kPlayerRadius;   // 25

// ★★ 한 틱에 미는 거리가 판정 반경보다 **크면 표적을 뛰어넘는다.**
//   26 > 24 라 틱 사이로 지나가 버렸다(2026-09-08: 시전 12건에 피해 1건).
//   지금은 12 < 25 라 한 조각으로도 안 넘지만, 쪼개면 판정이 더 촘촘해진다.
//   ⚠ 속도나 반경을 만지면 **이 값도 같이 볼 것.** 지켜야 하는 조건은
//     `kProjectileSpeed / kProjectileSubSteps < kProjectileHitRadius` 다.
constexpr int kProjectileSubSteps = 4;

// 근접 — **맨손이 기본**(정품 아레나는 무기를 안 준다). 피해는 힘이 정하므로
// 상수가 아니다(`GameBalance.h` UnarmedDamage). 여기 둘은 사거리와 간격.
// ⚠ 공격 간격은 **파일에도 코드에도 없는 유일한 값**이다 — 몬스터는 25~35 프레임으로
//   적혀 있는데 사람 쪽만 없다. 공격 동작 길이가 정하는 것으로 보인다. 15 는 자리표시.
constexpr double kMeleeRange = kUnarmedRange;
constexpr uint32_t kMeleeCooldownTicks = 15;   // ⚠ 자리표시 — 30Hz 기준 0.5초

// 첫 세계 스냅샷을 접속 뒤 몇 틱 미룰지. 정품 조각 순서에서 잰 값이다 —
// `2b`(USE_MAP) 와 스냅샷 사이에 다섯 조각(`27 27 27 08 04`)이 지나간다.
constexpr int kSnapshotDelayTicks = 5;

// 중요 메시지 대기열(Session::importantQ) 값 셋.
//   다시 싣기 60틱 = 정답지 `gameFPS() * byte(1565125+12*pl)(=2) / rate(=1)` (GAME3_3.c:2039 · 1538줄 초깃값).
//   조각 한도 512B · 대기열 상한 256 은 **자리표시** — 정답지는 사람별 상한이 아니라 전체 풀(서버 3072개, 1601~1609줄)이
//   차면 가장 많이 쌓인 사람을 끊는다(1615줄 → `nox_xxx_importantCheckRate_4E52B0`).
constexpr uint32_t kImportantResendTicks = 60;
constexpr size_t kImportantPacketBudget = 512;
constexpr size_t kImportantQueueMax = 256;

// 벽 격자 한 칸의 월드 크기와 몸 반지름 (독립 서버 P3).
// ★ 둘 다 **닫혔다**(2026-09-10). 칸 23 은 실클라 돌진 3표본이 정합했고,
//   재구현 코드도 칸 크기로 23 을 나눈다(◑). 반지름은 thing.bin 에서 나왔다.
constexpr double kWallCell = 23.0;
// ★ 몸 반지름 ⭕ — `thing.bin NewPlayer EXTENT = CIRCLE 10`. 전에는 짐작한 12 였다.
constexpr double kBodyRadius = kPlayerRadius;

// 시야 반경 (P4-b). ⚠ 화면 반경보다 조금 넓게 잡은 자리표시 — 실측 아님.
constexpr double kAoiRadius = 700.0;
// 갱신 하나에 싣는 정적 오브젝트 상한 (P4-b). EncodeUpdate 는 512B 를 넘치면
// **0 을 돌려줘 갱신이 통째로 멎는다** — 정적이 많은 자리에서 사람 이동까지 같이
// 멈추는 사고를 막으려면 수를 잘라야 한다. 가까운 순으로 담아 화면에 보이는 것부터.
constexpr size_t kAoiMaxPerUpdate = 20;
// 시야에서 **빼는** 반경. 들일 때보다 넓다 — 경계에 서 있으면 같은 것이 매 틱
// 들어왔다 나갔다를 되풀이해(플래핑) 별칭과 조각을 헛되이 태운다. 20% 여유를 둔다.
// ⚠ 이 1.2 배는 실측이 아니라 우리 값이다.
constexpr double kAoiDropRadius = kAoiRadius * 1.2;

// 물건에 손이 닿는 거리. ➖ **우리 값이다** — 파일에도 코드에도 근거가 없다.
// 몸 반지름(10)과 벽 한 칸(23)을 놓고 「한 칸 안」으로 잡았다.
// 정품은 물건을 밟고 서서 줍는 꼴이라 이보다 짧을 수도 있다.
// ★★ **근거가 생겼다 (2026-09-11) — 정품 클라가 75 로 거른다.** ⭕
//   opennox/src/input_c.go:356-368 —
//       if v54 >= 75*75 { CursorPickupFar }      // 멀면 **커서만 바꾸고 안 보낸다**
//       else { … if 주버튼 눌림 { clientPickup(sprite) } }   // → 0x73 이 나간다
//   v54 는 **사람과 그 오브젝트 사이의 거리 제곱**이다.
//   ⚠ 전에는 우리 값 **23**(벽 한 칸)이었다. **클라가 75 까지 보내는데 서버가 23 에서
//     거부**하니 그 사이(23~75)에서는 **눌러도 아무 일도 안 났다**(pickTooFar_ 로 떨어진다).
//     「보이는데 못 줍는다」의 모습과 맞는다.
//   ★ 정품 **서버**는 거리를 아예 안 본다(sdecode.c:117-143). 거르는 쪽은 **클라**다.
//     우리는 안전장치로 남기되 **클라와 같은 문턱**을 쓴다.
constexpr double kPickupReach = 75.0;

// ★★ 마법부여 비트 — `0x5A REPORT_ENCHANT` 의 본문 4바이트가 이 비트 묶음이다.
//   ⭕ `opennox/src/client/drawable.go:735-739` — `(Buffs & (1 << v)) != 0`
//   ⭕ `opennox/src/server/spells_enchant.go:60` — `ENCHANT_INVULNERABLE = EnchantID(23)`
//   즉 무적은 **1 << 23**. 4-a4 부터 부활 무적은 진짜 버프 23(buffApplyTo 5×30틱·세기 5)이다.
constexpr uint32_t kEnchantInvulnerable = 1u << 23;

// 한 사람이 들 수 있는 물건 수의 **안전 한도**. 진짜 한도는 서버가 아니라 **클라가 쥔다** —
// 못 받으면 `0xF1 INVENTORY_FAIL` 로 되돌려 주므로 서버가 미리 자를 일이 아니다.
// 이 값은 규칙이 아니라 폭주 방지용 울타리다.
constexpr size_t kInventoryGuard = 64;

// 채팅 한 줄에 실을 수 있는 글자 수. 크기 칸이 **1바이트**라 255가 천장이고,
// 정품 표본의 최대가 250(`fa 00 00`)이었다. 그보다 길면 버린다.
constexpr size_t kChatMaxBytes = 250;

// 겨눈 자리가 이보다 오래되면 안 쓴다 — 몸 바라보는 쪽으로 되돌아간다.
// ➖ **우리 값이다.** 정품이 낡은 겨눔을 어떻게 다루는지는 모른다.
// 30Hz 기준 1초. 클라는 마우스가 움직일 때마다 보내므로(표본 18,700+) 짧아도 충분하다.
constexpr uint32_t kAimStaleTicks = 30;

// ★ 사람 자세 번호 ⭕ 소스 확증(`opennox/src/draw_debug.go:294` debugStates) —
//   0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · **4 WALK** · 5 WALK_AND_DRAG · **6 RUN** · 8 PICKUP
//   갱신 항목 덧붙이 바이트의 **하위 4비트**가 이 번호다.
// ★ **그림 칸 간격·칸 수는 서버가 안 정한다**(2026-09-11 화면으로 확정).
//   클라가 `AnimLoop` 로 `Frame()` 에서 칸을 뽑는다(`opennox/src/drawable_vector.go:119`).
//   서버가 굴려 `0x6B` 로 보내 봤지만 화면이 한 점도 안 바뀌었다.
//   서버가 할 일은 **자세 번호를 바르게 실어 보내는 것**뿐이다.

constexpr uint8_t kAnimIdle = 0;
constexpr uint8_t kAnimWalk = 4;
constexpr uint8_t kAnimRun = 6;    // ⭕ 정답지 상태 5(달리기) → 6 (`GAME4.c:1750` nox_common_mapPlrActionToStateId_4FA2B0)

// 판이 끝나고 다음 판을 열기까지. ➖ **우리 값이다** — 정품이 몇 초 뒤에 새 판을
// 여는지는 표본이 없다. 승자 화면을 볼 시간은 줘야 하므로 30Hz 기준 8초로 둔다.
// ★ 이것이 없어서 **판이 한 번 끝나면 그 프로세스에서 다시 못 열었다**(2026-09-11 감사).
constexpr uint32_t kRoundRestartTicks = 240;

class Server {
public:
    // 서버 찾기에 답할 원본. 머리 2바이트와 오프코드를 뺀 **본문**을 넣는다.
    // 토큰 자리만 요청 값으로 바꿔 그대로 돌려준다 — 모르는 자리를 건드리지 않는 것이
    // 「녹음 재생」의 요점이다.
    void SetInfoTemplate(const uint8_t* body, size_t n);

    // 접속을 받아줄 때 클라에게 줄 XOR 키. 실제 서버는 스스로 정하지만,
    // 덤프와 바이트를 맞춰보려면 그때 쓰인 키를 넣을 수 있어야 한다.
    void SetXorKey(uint8_t key) { xorKey_ = key; }

    // 다음 접속에 매길 번호. 실측은 1부터 올라갔다.
    void SetNextConnectId(uint32_t id) { nextConnectId_ = id; }

    // ★ 접속 대본 — 덤프 재생 전용.
    //
    // **키와 번호는 접속마다 다르다**(실측 0x70 · 0x2a). 서버가 스스로 정하는 값이라
    // 바이트를 맞춰보는 대상이 아니다. 그래서 덤프에서 뽑아 순서대로 주입한다.
    // 이걸 안 하면 두 번째 접속에서 키 한 자리 때문에 "다름"이 떠, 정작 봐야 할
    // **나머지 아홉 자리가 맞는다는 사실**이 묻힌다.
    // 접속 한 번에 정품이 쓴 값들. 전부 **서버가 스스로 정하는 값**이라
    // 바이트를 맞춰보는 대상이 아니다.
    struct AcceptScript {
        uint32_t connectId = 0;
        uint8_t xorKey = 0;
        uint32_t frame = 0;    // 그때의 서버 프레임
        uint16_t netCode = 0;  // 그 사람에게 준 오브젝트 번호
    };
    void SetAcceptScript(std::vector<AcceptScript> script) { script_ = std::move(script); }

    // ★ 접속을 받아준 뒤 한 번에 나가는 것 두 덩이.
    //
    // 정품은 `0x20 CLIENT_ACCEPT` 를 받으면 곧바로 둘을 잇달아 보낸다.
    //
    //   ① 216바이트 묶음 — 메시지 **여섯 개**가 한 조각에 들어 있다
    //        a9 INFORM(관전) 6 · 28 FULL_TIMESTAMP 5 · 2c JOIN_DATA 7 ·
    //        af GAME_SETTINGS 20 · b0 GAME_SETTINGS_2 49 · 2d NEW_PLAYER 129
    //   ② 41바이트 — `2b USE_MAP`(이름 32 + CRC 4 + 프레임 4)
    //
    // 안에 든 값 대부분은 **아직 내가 정하지 못한다**(설정·플레이어 정보).
    // 그래서 원본을 그대로 두고 **접속마다 달라지는 두 자리만** 바꿔 내보낸다 —
    // 그것이 프레임과 NetCode 다. 두 접속의 216바이트를 견줘 확인한 것이다.
    void SetJoinTemplate(const uint8_t* body, size_t n);
    void SetUseMapTemplate(const uint8_t* body, size_t n);
    // ★ 맵 지정 뒤에 이어 보낼 것들(정품이 보내는 105B INFORM · 142B 공지 · 205B INFORM).
    //   없으면 클라가 로딩에서 안 넘어간다.
    void SetPostMap(const std::vector<std::vector<uint8_t>>& v) { postMap_ = v; }
    // ★ 접속 직후 한 번 내보낼 첫 세계 스냅샷. 없으면 클라 화면이 검은 채로 남는다.
    // ★★ 스냅샷을 넣으면 **스폰 자리도 여기서 정해진다.** 정의는 Server.cpp.
    //   서두가 찍힌 그 맵의 그 자리라야 사람이 세계 오브젝트들과 같은 동네에 선다.
    void SetFirstSnapshot(const std::vector<uint8_t>& v);
    // ★ 진단용 — 켜면 매 틱 「내가 만든 것」 대신 **정품이 보냈던 조각**을 순서대로 내보낸다.
    void SetMirror(const std::vector<std::vector<uint8_t>>& v) {
        mirror_ = v;
        mirrorAt_ = 0;
        // ★★ 서두는 **녹음본이라 프레임이 고정**이다(늘 3219 에서 시작한다).
        //   그러니 접속 묶음이 말하는 프레임도 거기에 맞춰야 시계가 한 줄로 이어진다.
        //   안 맞추면 늦게 붙은 사람은 접속 때 받은 프레임이 서두보다 앞서서,
        //   그 뒤 갱신이 전부 **과거로 보여 버려진다** — 화면이 검다(2026-09-08).
        preludeFirstFrame_ = 0;
        for (const std::vector<uint8_t>& m : mirror_) {
            if (m.size() >= 3 && m[0] == 0x27) {   // 0x27 TIMESTAMP
                preludeFirstFrame_ = static_cast<uint32_t>(m[1]) |
                                     (static_cast<uint32_t>(m[2]) << 8);
                break;
            }
        }
    }
    bool MirrorOn() const { return !mirror_.empty(); }
    // ★ 진단용 — 끄면 매 틱 상태 갱신(0xa4)을 **안 보낸다.**
    //   왜 필요한가 : 정품(opennox)은 클라가 둘 붙어도 갱신을 **한 번도 안 보내는데**
    //   화면에 자기 캐릭터가 이름표까지 나온다(2026-09-08 대조). 내 서버는 갱신을
    //   매 틱 보내는데 아무것도 안 그려진다. 「내 갱신이 오히려 방해하나」를 가르는 자리다.
    void SetSendUpdates(bool v) { sendUpdates_ = v; }
    // ★ 첫 세계 스냅샷의 사람 항목을 이 사람 것으로 갈아 끼울지. 끄면 **녹음본 그대로** 나간다.
    //   「몸이 안 그려지는 게 갈아끼우기 탓인가」를 가르는 시험 손잡이다(--raw-snapshot).
    void SetPatchSnapshot(bool v) { patchSnapshot_ = v; }
    // ★ 진단용 — 끄면 **새로 들어온 사람의 장비(`cc(4c)`+`cc(60)`)를 먼저 와 있던
    //   사람들에게 안 알린다.** 이걸 켠 채로 정품 클라 둘을 붙이면 **둘째가 들어오는
    //   그 순간 첫째가 죽는다**(2026-09-08 실측: A 마지막 송신 23:44:26.942 ·
    //   B 접속 23:44:26.617 · 그 사이에 A 로 간 82바이트가 이 알림이다).
    //   가르는 자리 = 죽음이 **이 알림 탓인가, 사람 둘 실린 상태 갱신 탓인가**.
    void SetNotifyNewGear(bool v) { notifyNewGear_ = v; }
    // ★★★ 서두(녹음본) 안에 든 `cc(6a <번호2> 01 …)` 의 상태를 **0 으로 뒤집는다. 기본이 켜짐.**
    //
    //   `01` 은 상태 bit0 = **GUI 를 안 그린다**(오픈녹스 `nox_client_onClientStatusA`:
    //   `v&1` 이면 renderGUI=false). 녹음이 그 상태에서 떠진 것이라, 서두를 그대로
    //   흘리면 **접속하자마자 「HUD 숨겨라」가 나간다.** 남의 번호를 갈아 끼우듯
    //   이 값도 이 사람 것으로 고쳐야 하는 자리였다(2026-09-09, 서두 182번 자리).
    //
    //   실측 — 이 한 자리를 뒤집으니 화면 색이 **985 → 1335** 로 뛰고
    //   주문 세트·체력/마나 막대·초상·나침반이 한꺼번에 떴다. 그전에 세 번
    //   헛짚었다(`6a` 만 보내기 · `2d` 상태 0 보내기 · 둘의 순서 바꾸기).
    //   `--hud-off` 로 끄면 녹음본 값을 그대로 쓴다(대조용).
    void SetForceHudOn(bool v) { forceHudOn_ = v; }
    // ★ 새 사람이 들어올 때 먼저 와 있던 사람들에게 넘길 오프코드 묶음(`--peer-ops`).
    //   기본 `4c,60`. 「상대 몸이 안 그려진다」를 다시 컴파일 없이 쓸어보는 자리다.
    void SetPeerOps(const std::set<uint8_t>& v) { peerOps_ = v; }
    // ★ 스폰 간격(--spawn-spread). 크게 주면 「가려서 안 보이는 것」과
    //   「안 그려지는 것」이 갈린다. 단 45 를 넘기면 근접이 안 닿는다.
    void SetSpawnSpread(int v) { spawnSpread_ = v; }

    // ── 벽 충돌 (독립 서버 P3) ─────────────────────────────
    //
    // 맵의 벽 격자를 받아 걸음을 막는다. 격자 한 칸 = kWallCell 유닛.
    // ⚠ 배율 23 은 **녹스 격자 상수**(256칸×23≈5888)로 둔 값 — 실클라 벽 돌진(P3-b)으로
    //   검증하기 전까지는 짐작이다. wallOffX/Y 보정도 같은 자리에서 확인한다.
    void SetWalls(const std::vector<std::pair<uint8_t, uint8_t>>& cells) {
        walls_.assign(256 * 256, 0);
        for (const auto& c : cells) walls_[c.second * 256 + c.first] = 1;
        wallsOn_ = !cells.empty();
        // ★ 모양을 모르는 칸은 **X 자(방향 2, 대각선 둘 다)** 로 둔다 — 시험이 칸 좌표만 준다.
        wallTiles_.assign(256 * 256, WallTile{});
        for (const auto& c : cells) {
            WallTile& w = wallTiles_[c.second * 256 + c.first];
            w.present = true; w.dir = 2; w.tile = 0;
        }
        AIPathBakeWalls();   // 묶음 6-f(3) — 길 찾기 격자에도 같은 벽을 굽는다
    }
    // ★★ **벽 모양** (2026-09-13 묶음 2-b — 정답지 벽은 칸이 아니라 **칸 대각선 선분**이다).
    //   방향 0~10 이 어느 대각선(\ 왼쪽 · / 오른쪽)의 어느 구간(0~23 · 0~11.5 · 11.5~23)인지를
    //   `table_313272`(`GAME5.c:53`)가 정한다. 재질 = thing.bin WALL 순번(깃발 0x2 면 선 추적이 무시).
    //   ⚠ 문·창·비밀 벽·부서진 벽 깃발(Flags4)은 아직 없다 — 묶음 9.
    struct WallTileIn { uint8_t x = 0, y = 0, dir = 0, tile = 0; bool flag80 = false; };
    void SetWallTiles(const std::vector<WallTileIn>& cells) {
        wallTiles_.assign(256 * 256, WallTile{});
        walls_.assign(256 * 256, 0);
        for (const auto& c : cells) {
            WallTile& w = wallTiles_[c.y * 256 + c.x];
            w.present = true; w.dir = c.dir; w.tile = c.tile; w.flag80 = c.flag80;
            walls_[c.y * 256 + c.x] = 1;
        }
        wallsOn_ = !cells.empty();
        AIPathBakeWalls();   // 묶음 6-f(3) — 길 찾기 격자에도 같은 벽을 굽는다
    }
    void SetWallDefFlags(std::vector<uint32_t> f) { wallDefFlags_ = std::move(f); }
    // ★ 09-29 Z1-A — 벽 정의별 부수기 칸(`WallDefRaw` 주석) · 부서지는 벽·창문 칸(맵 두 구역 · `MapFile::ReadGridList`)
    struct WallDefExtra { uint16_t hitMat = 0; uint8_t health = 0; uint16_t breakSound = 0; };
    void SetWallDefExtras(std::vector<WallDefExtra> e) { wallDefExtra_ = std::move(e); }
    // 정답지 `nox_server_mapRWDestructableWalls_429530`(`GAME1_2.c:1006~1014`) — 벽이 있는 칸마다 깃발 8 · 번호 = 그때까지 센 수(0 부터) ·
    //   `nox_server_mapRWWindowWalls_4292C0`(`:916~919`) — 깃발 0x40 · 체력은 모든 벽이 정의의 Health41(`:1322`)
    void SetWallSpecials(const std::vector<std::pair<uint32_t, uint32_t>>& destructible,
                         const std::vector<std::pair<uint32_t, uint32_t>>& windows);
    // ★ 09-29 Z1-A — 벽을 때린 것(`Nox_xxx_mapDamageToWalls_534FC0` 의 who). 유닛이면 그 앞쪽(내적 > 0.5)만 · 거리는 그 자리에서 잰다 ·
    //   맞는 소리는 그 재질로 그 자리에서. present 가 거짓이면 who 없음(소리 없음 · 자리는 폭발 자리)
    struct WallHitter { bool present = false; bool unit = false; bool player = false; float x = 0, y = 0; uint8_t dir = 0; uint32_t material = 0;
                        std::string src; };   // ★ 09-29 B6 src — 임자 사슬 끝 사람(맞는 소리를 몬스터가 들을 거리의 출처)
    int DamageToMap(int gx, int gy, int dmg, uint8_t dtype, const WallHitter& who, std::vector<OutPacket>& out);
    int WallPreDestroy(int gx, int gy, std::vector<OutPacket>& out);
    bool MapDamageToWalls(int gx0, int gy0, int gx1, int gy1, float px, float py, float rad, int dmg, uint8_t dtype,
                          const WallHitter& who, std::vector<OutPacket>& out);
    void SendBrokenWalls(Session& s, std::vector<OutPacket>& out);   // 들어온 사람에게 이미 부서진 벽(`Nox_xxx_wall_4DF1E0`)
    WallHitter WallHitterOfAddr(const std::string& addr) const;       // 사람이면 유닛 who(자리·방향·재질) · 없으면 who 없음
    // 시험용
    bool DebugWallBroken(int gx, int gy) const { return gx >= 0 && gx < 256 && gy >= 0 && gy < 256 && !wallTiles_.empty() && wallTiles_[gy * 256 + gx].broken; }
    int DebugWallHp(int gx, int gy) const { return (gx >= 0 && gx < 256 && gy >= 0 && gy < 256 && !wallTiles_.empty()) ? wallTiles_[gy * 256 + gx].hp : -1; }
    unsigned long long wallGate(size_t i) const { return i < 5 ? wallGate_[i] : 0; }   // [0] 벽없음 · [1] 안부서지는벽(소리만) · [2] 깎음 · [3] 부서짐 · [4] 부서짐알림(받는 사람별)

    // ── 묶음 10-a 소리 ────────────────────────────────────────────────
    //
    // thing.bin `AUD ` 칸을 그대로 받는다(`ThingDb::Sounds`). 색인 = **소리 번호**.
    // ⭕ 실측 — AUD 항목 **927개가 이름표 1,023개에 100% 맞물린다**(빗나감 0).
    //   소리조각이 있는 것은 **810개**. 들리는 거리는 150~1500(기본 600)
    struct SoundDefIn { uint16_t id = 0; int maxDist = 600; uint8_t merged = 0; int samples = 0; uint16_t flags = 0; };
    void SetSoundDefs(const std::vector<SoundDefIn>& list);
    size_t SoundDefCount() const { return soundDefs_.size(); }

    // 소리 하나를 이 틱에 울린다 — `nox_xxx_audCreate_501A30`(자리) · `nox_xxx_aud_501960`(임자).
    //   `kind` 0 = 볼 수 있는 모두 · **1 = 같은 팀만**(code = 팀 번호) · **2 = 그 한 사람만**(code = 번호).
    //   `owner` 가 받는 사람 자신이면 오프코드가 `0xA7` 로 바뀐다

    // ── 묶음 10-c 음악 ────────────────────────────────────────────────
    //
    // 정답지 `NetMusic`·`NetMusicPushEvent`·`NetMusicPopEvent`·`NetMusicEvent`
    //   (`server/network.go:290~313`) — 넷 다 **3바이트**이고 **모두에게(255)** ·
    //   `NetSendPacketXxx1` 이라 **순번 봉투(cc)** 에 실린다.
    //     `e5 <곡 u8> <소리크기 u8>` · `e6 00 00` · `e7 00 00` · `e5 00 00`
    //
    // ⚠ **방아쇠는 노스스크립트뿐이다** — `ns.Music`·`MusicPushEvent`·`MusicPopEvent`·`MusicEvent`
    //   (`src/script_ns_audio.go`)가 유일한 부르는 자리다. 서버 안에서 저절로 울리는 데가 없다.
    //   우리 서버엔 스크립트가 없으므로 **9-a 비밀벽처럼 서버 API 로** 낸다.
    // ⚠ 협동(2048)이면 정답지는 **보내지 않고 host 가 제자리에서 튼다** — 전용 서버엔 그 자리가 없다

    // ── 묶음 10-d 떠남·강퇴 알림 ──────────────────────────────────────
    //
    // 정답지 `PlayerDisconnect`(`src/player.go:630~643`) · 시간 초과(`src/server.go:440~452`).
    //   · `OUTGOING_CLIENT` **0xAE** — **3바이트** `ae <번호 u16>` ·
    //     받는 이가 `pli | 0x80` 이라 **그 사람만 빼고 모두** · 봉투 없음(`NetSendPacketXxx0`, 중요도 0)
    //   · `KICK_NOTIFICATION` **0xC5** — **1바이트** · **쫓겨나는 그 사람에게만** ·
    //     대기열이 아니라 **연결에 바로**(`SendReliable`) · **까닭이 4 일 때만**
    //   · `TIMEOUT_NOTIFICATION` **0xC6** — **1바이트** · 그 사람에게 `SendReliable` · 그 뒤 까닭 3 으로 끊는다
    // ⚠ `SERVER_QUIT_ACK`(0xC8)은 **서버가 보내는 것이 아니다** — 클라가 `SERVER_QUIT` 에 되보내는 답이다
    //   (`network_client.go:140`). 서버는 **받아서** 끊는다(`network_server.go:104`)
    void SendOutgoingClient(uint16_t netCode, const std::string& except, std::vector<OutPacket>& out);

    // ── 묶음 10-e 일지·장 끝·관리자 답 ────────────────────────────────
    //
    // ⭕ `JOURNAL_MSG` **0xD5** — `netSendJournalAdd/Remove/Update_4D9440·4A0·500`(`GAME3_2.c:4731~4761`).
    //   **68바이트** `d5 <갈래> <이름 64바이트> <u16>` · 갈래 **1 더하기 · 2 빼기 · 3 고치기** ·
    //   `netSendPacket0`(봉투 없음 · 중요). ⚠ 빼기(2)에는 뒤 u16 을 **안 싣는다**(정답지가 안 쓴다)
    // ⭕ `CHAPTER_END` **0xD6** — `netSendChapterEnd_4D9560`. **3바이트** `d6 <장> <끝인가>`
    // ⚠ 둘 다 방아쇠가 **노스스크립트·싱글 캠페인**이라 음악처럼 **서버 API** 로 낸다
    enum class JournalKind : uint8_t { Add = 1, Remove = 2, Update = 3 };
    void SendJournal(Session& to, JournalKind kind, const std::string& name, uint16_t value,
                     std::vector<OutPacket>& out);
    void SendJournalAll(JournalKind kind, const std::string& name, uint16_t value,
                        std::vector<OutPacket>& out);
    void SendChapterEnd(Session& to, uint8_t chapter, bool ended, std::vector<OutPacket>& out);

    // ⭕ `SYSOP_RESULT` **0xBD** — `network_server.go:191~205`. 클라가 `SYSOP_PW`(0xBC · 비밀번호 20바이트)를
    //   보내면 **2바이트** `bd <맞으면 1, 아니면 0>` 으로 답한다.
    //   ⚠ **비밀번호가 비어 있으면 늘 틀린 것으로 본다**(`pass == "" || pass != got`)
    void SetSysopPassword(const std::string& pw) { sysopPassword_ = pw; }
    bool SysopOf(const std::string& addr) const {
        auto i = sessions_.find(addr);
        return i != sessions_.end() && i->second.sysop;
    }
    void MusicPlay(uint8_t music, uint8_t volume, std::vector<OutPacket>& out);
    void MusicPush(std::vector<OutPacket>& out);
    void MusicPop(std::vector<OutPacket>& out);
    void MusicEvent(std::vector<OutPacket>& out);
    // ★ 09-29 B6 — `src` = 소리를 낸 유닛(몬스터가 들을 거리의 출처 · `EventObj` 의 obj). 비면 `owner`(사람)를 쓴다
    void PlaySoundAt(uint16_t id, float x, float y, uint8_t kind = 0, uint32_t code = 0,
                     const std::string& owner = std::string(), const Target& src = Target());


    // ── 묶음 9-a 비밀벽 ────────────────────────────────────────────────
    //
    // 맵의 `SecretWalls` 칸(`MapFile::ReadSecretWalls`)을 받아 세운다.
    // **번호는 읽은 차례대로 0,1,2…** — 정답지 `GAME1_2.c:1130` 이 벽 `+10` 에 매기는 그 값이다
    // (`wallSecretCounterClear_4297B0` 이 맵마다 0 으로 되돌린다).
    // ⚠ 벽이 **실제로 있는 칸**에만 달린다 — 정답지도 `getWallAtGrid` 가 없으면 마디를 안 건다
    struct SecretWallIn { uint8_t gx = 0, gy = 0, flags = 0, state = 0, anim = 0; uint32_t waitSecs = 0; };
    void SetSecretWalls(const std::vector<SecretWallIn>& list);
    // ── 묶음 9-b 쏘는 덫 ──────────────────────────────────────────────
    //
    // 정답지 — 주기 `nox_xxx_updateShootingTrap_54F9A0`(`GAME5.c:6352`) ·
    //   살피기 `sub_54FBF0`·`sub_54FBB0`(`GAME5.c:6443·6456`) ·
    //   화살 만들기 `nox_xxx_createArrowTrapProjectile_54FA80`(`GAME5.c:6399`) ·
    //   FX `nox_xxx_sendArrowTrapFX_5238A0`(`GAME4_2.c:3966`).
    //
    // ⭕ **덫은 여섯 종류다**(thing.bin `INIT` 실측 · 전부 `UPDATE = SkullUpdate`) —
    //   `ArrowTrap1`[1 1] · `ArrowTrap2`[-1 1] 은 `MercArcherArrow` 를,
    //   `Skull1`[-1 -1] · `Skull2`[1 -1] · `Skull3`[-1 1] · `Skull4`[1 1] 은 `StrongFireball` 을 쏜다.
    // ⭕ **보는 쪽은 맵이 아니라 종류에 붙어 있다** — 오브젝트 초기자료는 종류의 것을 **복사**한 것이고
    //   (`server/object.go:343~346`) 맵의 기본 짐은 그 값을 안 싣는다. ⇒ 같은 종류는 전부 같은 쪽
    struct ShootingTrapIn {
        float x = 0, y = 0;
        uint8_t facing = 0;            // 방향표(blob 0x587000+230072)로 옮긴 각도 0~255
        std::string proj;              // 쏘는 것 이름(`UPDATE` 인자)
        uint8_t fxKind = 0;            // 0 = FX 없음(해골) · 1 = ArrowTrap1 · 2 = ArrowTrap2
        int damage = 0;                // 0 이 아니면 닿음 피해를 이 값으로 덮는다(화살 덫 = ArrowTrapDamage)
    };
    void SetShootingTraps(const std::vector<ShootingTrapIn>& list);
    size_t ShootingTrapCount() const { return traps_.size(); }



    // ★★ **오브젝트끼리 충돌** (2026-09-13 묶음 2-c — 정답지 이식).
    //   장애물 = 맵 오브젝트 중 COLLIDE 가 있고 NO_COLLIDE 가 아니며 IMMOBILE 인 것(나무·오벨리스크…).
    //   ⚠ 문(DoorCollide — 밀면 도는 문)은 묶음 9, 아이템(PickupCollide — 차여 굴러감)은 묶음 5.
    struct PhysBody {
        float x = 0, y = 0;
        uint8_t shape = 0;          // 2 원 · 3 상자
        float r = 0;
        float boxLT = 0, boxLB = 0, boxRT = 0, boxRB = 0;   // ShapeBox::Calc (`server/object_type.go:626`)
        uint32_t flags = 0;         // 오브젝트 깃발 비트
        uint32_t cls = 0;           // 클래스 비트
        float mass = 1.0f;
        // 묶음 3-e — 몬스터 SUBCLASS 비트(libs MonsterClassNames 순서)와 피해 함수(thing.bin DAMAGE · 없으면 DefaultDamage)
        uint32_t subclass = 0;
        uint8_t damageFn = 0;       // DamageFnKind
        bool hasCollide = true;     // COLLIDE 속성 — 없으면 걷기 충돌에서 빠진다(공격·피해는 받는다)
        // ★ 09-23 nine-defects 1-d — 장애물의 **닿음 피해**(`COLLIDE = DamageCollide <양> <이름>` · `collideDamageLoad_536E10`).
        //   양 0 이면 닿음 피해가 없다. 맵에 놓인 불(`FlameImmobile` 류)이 이것이다
        uint8_t contactDmgAmount = 0;
        uint8_t contactDmgType = 0;
        uint16_t wireType = 0;      // ★ 4-c3b2 — 장애물의 와이어 종류. 맵 불에 맞을 때 「때린 것」 재질을 이 번호로 찾는다(0 = 모름)
    };
    enum DamageFnKind : uint8_t { kDmgDefault = 0, kDmgPlayer, kDmgSkeleton, kDmgStone, kDmgMechGolem, kDmgFlammable, kDmgBlackPowder, kDmgOther };
    static uint8_t DamageFnOf(const std::string& name) {
        if (name.empty() || name == "DefaultDamage") return kDmgDefault;
        if (name == "PlayerDamage") return kDmgPlayer;
        if (name == "SkeletonDamage") return kDmgSkeleton;
        if (name == "StoneDamage") return kDmgStone;
        if (name == "MechGolemDamage") return kDmgMechGolem;
        if (name == "FlammableDamage") return kDmgFlammable;
        if (name == "BlackPowderDamage") return kDmgBlackPowder;
        return kDmgOther;
    }
    static uint32_t MonsterClassBitsOf(const std::string& raw) {
        static const char* const kNames[] = {
            "SMALL_MONSTER", "MEDIUM_MONSTER", "LARGE_MONSTER", "SHOPKEEPER", "NPC", "FEMALE_NPC",
            "UNDEAD", "MONITOR", "MIGRATE", "IMMUNE_POISON", "IMMUNE_FIRE", "IMMUNE_ELECTRICITY",
            "IMMUNE_FEAR", "BOMBER", "NO_TARGET", "NO_SPELL_TARGET", "HAS_SOUL", "WARCRY_STUN",
            "LOOK_AROUND", "WOUNDED_NPC",
        };
        uint32_t bits = 0;
        size_t start = 0;
        while (start <= raw.size()) {
            size_t e = raw.find('+', start);
            if (e == std::string::npos) e = raw.size();
            std::string tok = raw.substr(start, e - start);
            while (!tok.empty() && tok.back() == ' ') tok.pop_back();
            while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
            for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
                if (tok == kNames[i]) bits |= (1u << i);
            start = e + 1;
        }
        return bits;
    }
    static void CalcBox(PhysBody& b, float w, float h) {
        const float px = w * 0.35354999f, py = h * 0.35354999f;
        b.boxLT = -px + py; b.boxLB = -px - py; b.boxRT = px + py; b.boxRB = px - py;
    }
    void SetObstacles(std::vector<PhysBody> v) { obstacles_ = std::move(v); AIPathBakeObstacles(); }
    size_t ObstacleCount() const { return obstacles_.size(); }
    // 소환수 종류 번호 → 몸(모양·반지름·깃발·질량). main.cpp 가 thing.bin 에서 채운다.
    void SetTypeBody(uint16_t type, const PhysBody& b) { typeBodies_[type] = b; }
    // 사람 애니 칸 수 표(묶음 3-a) — main.cpp·시험이 thing.bin PlayerDraw STAT 에서 채운다.
    //   비어 있으면(0칸) 공격 상태가 첫 틱에 바로 판정하고 끝난다 — 정답지도 표가 0이면 그렇다.
    void SetPlayerAnimFrames(int i, int frames, int hold) {
        if (i >= 0 && i < kPlayerAnimSlots) { playerAnimFrames_[i][0] = frames; playerAnimFrames_[i][1] = hold; }
    }
    // 시험용 — 선 추적(Flag1, 걸음과 같은 깃발)
    // 시험용 — 편 거르기(`OwnersEnemy`)를 그대로 부른다. 둘째·넷째는 **뿌리가 몬스터인가**
    bool DebugOwnersEnemy(const std::string& a, bool aMonsterRoot, const std::string& b, bool bMonsterRoot) const {
        return OwnersEnemy(a, aMonsterRoot, b, bMonsterRoot);
    }
    bool DebugTraceRay(float x1, float y1, float x2, float y2) const { return MapTraceRay(x1, y1, x2, y2, 0x1); }
    bool DebugTraceRayFlags(float x1, float y1, float x2, float y2, uint8_t f) const { return MapTraceRay(x1, y1, x2, y2, f); }
    // 진단 — 가장 가까운 벽 선분까지 거리(±3칸). 벽 없으면 −1. 칸·방향도 돌려준다.
    double DebugWallDistance(double x, double y, int* outGx = nullptr, int* outGy = nullptr, int* outDir = nullptr) const;
    bool WallAt(double x, double y) const {
        if (!wallsOn_) return false;
        const int cx = static_cast<int>(x / kWallCell);
        const int cy = static_cast<int>(y / kWallCell);
        if (cx < 0 || cx > 255 || cy < 0 || cy > 255) return true;   // 맵 밖 = 벽
        return walls_[cy * 256 + cx] != 0;
    }
    // 이 자리에 설 수 있나 — 몸 반지름의 네 모서리가 벽 칸을 안 밟아야 한다.
    // ── 세계 정적 오브젝트 (독립 서버 P4-a) ────────
    //
    // 맵 파일에서 읽은 것들. 아직 갱신에 실지는 않는다(P4-b 에서 AOI 로) —
    // 지금은 스냅샷 조립(P5-c)과 AOI 의 바닥이다.
    struct WorldStatic {
        uint16_t netCode = 0;   // 서버가 붙인 번호 (맵 extent 그대로 쓰지 않는다 — 충돌 여지)
        uint16_t type = 0;      // 와이어 종류 번호
        int x = 0, y = 0;
        // 수식(마법부여)이 붙는 물건인가 — 무기·방어구·완드·깃발.
        // 주울 때 `4b`(5B) 대신 **`4c`(9B, 수식 4바이트)** 로 알려야 한다.
        // ⭕ `4c` 는 정품 표본 10건으로 짜임 확인(`4c cc 02 6d 04 ff bf ff ff`).
        // `main.cpp` 가 `thing.bin` 의 CLASS 로 정한다.
        bool modifiable = false;
        // ★ 쓰면 무슨 일이 나는가 — `thing.bin` 의 `USE = PotionUse 50` 에서 온다.
        //   ⭕ 값이 파일에 있다. 지어낸 숫자가 아니다.
        int useAmount = 0;
        // ★ 겉모습(P7) — 입으면 남들에게 `0x4f`~`0x52` 로 알릴 때 실을 **장비 비트**.
        //   `EquipBitFor(이름)` 가 채운다. **0 이면 겉모습을 안 보낸다**(모르는 것은 안 지어냄).
        uint32_t equipBit = 0;
        bool isWeapon = false;
        bool weaponClass = false;   // ★ 09-27 B3 — CLASS 에 WEAPON 또는 WAND(가방 항목으로 그대로 옮긴다)
        bool wandClass = false;     // ★ 09-27 B5 — CLASS 에 WAND
        uint32_t subclass = 0;      // ★ 09-27 B5 — 무기·지팡이의 SUBCLASS 비트 · 09-28 갑옷이면 갑옷 SUBCLASS
        // ★ 묶음 5 — **무게**(thing.bin `WEIGHT`). 줍기 한도가 이것의 합으로 걸린다
        //   (`object_pickup.go:70~79`). 정답지 기본은 255 이고 파일 값이 있으면 그것으로 덮는다
        //   (`server/object_type.go:202·212`).
        uint8_t weight = 255;
        // ★ 묶음 5 — **먹는 것인가**(CLASS 에 FOOD). 같은 종류를 셋(퀘스트·협동은 아홉)까지만 든다
        //   (`object_pickup.go:81~92`).
        bool isFood = false;
        // ★ 묶음 5-b — `SUBCLASS` 의 먹을거리 갈래 비트(`FoodClassBits`).
        //   물약이 무엇을 하는지가 여기 있다 — 체력·마나·독 해제·버프 여덟·방패.
        uint32_t foodBits = 0;
        // ★ 묶음 5-d — **쓸 때 부르는 함수**(`USE = ConsumeUse 10` 의 앞말).
        UseFn useFn = UseFn::None;
        // ★ 묶음 5-e — 충전·완드 주문·재사용 대기(`USE = WandCastUse 250 1.0 SPELL_PLASMA`)
        uint8_t charges = 0, chargesMax = 0;
        uint8_t wandSpell = 0;
        uint32_t wandCooldown = 0;
        // ★ 묶음 5-h — **금 더미**(`PICKUP = GoldPickup`)와 그 안의 금액.
        //   ⚠ 금액은 맵 오브젝트의 **초기자료**(`GoldInitData.Amount`, 오브젝트 `+692`)에서 온다.
        //     우리 맵 파서는 아직 오브젝트별 초기자료를 안 읽는다 → 지금은 0 이고, 시험이 직접 넣는다.
        //     ⭕ estate 맵에는 금이 **한 개도 없다**(기동 표시줄 물건 목록에 Gold 없음).
        bool goldPickup = false;
        uint32_t goldAmount = 0;
        // ★ 묶음 5-c — **줍는 순간 부르는 함수가 물약 갈래인가**(`PICKUP = PotionPickup`).
        //   참이면 줍는 그 자리에서 마실 수 있고, 마셨으면 가방에 안 들어간다.
        bool potionPickup = false;
        // ★ 09-28 — 버려도 안 사라지는 까닭(`dropDefault_4ED290` `GAME3_3.c:6440~6442`). main 이 thing.bin 으로 채운다
        DecayExempt decayExempt = DecayExempt::None;
        DropFn dropFn = DropFn::Default;   // ★ 09-28 — 버릴 때 부르는 함수 갈래(thing.bin `DROP`)
        uint8_t mods[4] = { 0xff, 0xff, 0xff, 0xff };   // ★ 09-29 — 수식 넷의 번호(`Session::Carried::mods` 주석) — 줍고 버려도 따라간다
        // ★ 09-28 — 사라질 프레임(`unitSetDecayTime_511660` → `+136`). 0 = 안 사라진다. 버릴 때만 건다
        uint32_t decayFrame = 0;
        // ★ 09-29 — 줍기 함수 갈래·무기 줍는 소리·활 재장전 칸(`Session::Carried` 같은 이름 주석). 화살통 화살 수는 `charges`·`chargesMax`(맵 짐 `AmmoXfer`)
        PickupFn pickupFn = PickupFn::Default;
        uint16_t pickupSound = 0;
        uint8_t bowReload = 0;
    };
    void SetWorldStatics(std::vector<WorldStatic> v) { statics_ = std::move(v); }
    static Session::Carried CarriedFromStatic(const WorldStatic& w);   // ★ 09-30 — 바닥 물건 → 가방 항목(줍기와 시험 물건 주기가 같이 씀)
    // ★ 09-30 사람 판 시험용 — 들어온 사람마다 세계를 받은 지 2초 뒤 이 종류들을 가방에(틀은 `SetItemTemplate` — main 이 맵 물건에서 뜬다)
    void SetGiveItems(std::vector<uint16_t> v) { giveItems_ = std::move(v); }
    unsigned long long testItemsGiven() const { return testItemsGiven_; }
    unsigned long long testItemsNoTemplate() const { return testItemsNoTemplate_; }

    // ── 다루는 것 — 문·오벨리스크 (P5) ────────────
    //
    // 집는 게 아니라 **다가가면 반응하는** 것들이다. estate 실측 —
    // WoodenDoor 3 · ArchedHalfDoor 2 · ObeliskPrimitive 2.
    struct WorldProp {
        uint16_t netCode = 0;
        // ★ 09-20 ⑩ — **클라가 어느 목록에서 찾나.** 클라는 번호의 윗비트(0x8000)로
        //   «맵에서 제가 만든 정적 그림»과 «서버가 소개한 동적 그림»을 가른다
        //   (`client/drawable.go:378~403`). 판·문처럼 `CLASS` 에 `IMMOBILE` 이 있으면 정적 쪽이다.
        //   안 세우면 동적 목록을 뒤지다 못 찾고 **조용히 버린다**(실측 — 클라 「그림 못 찾음」)
        bool staticLookup = false;
        uint16_t type = 0;
        int x = 0, y = 0;
        PropKind kind = PropKind::Door;
        bool open = false;          // 문만 쓴다
        // ★ 09-22 — 문의 **닫힘 각도**(0·8·16·24). 맵 짐 꼬리의 둘째 바이트다.
        //   전에는 모든 문을 0 으로 보내 **문 방향이 전부 틀렸다**
        uint8_t angle = kDoorClosedFallback;
        // ★ 09-29 Z1-A — 문 잠김(`ud+1` — 0 안 잠김 · 1 은 · 2 금 · 3 루비 · 4 사파이어 · 5 장치) · 팀(+52) · 부분류 GATE(`+12 & 4`)
        uint8_t lock = 0;
        uint8_t team = 0;
        bool gate = false;
        uint32_t teamMsgFrame = 0;  // +136 — 팀 문 「GateLockedMechanism」 다음 알림 프레임(`GAME5.c:2210~2212`)
        // ★★ 09-22 — 문이 **도는** 얼개(정답지 `GAME5.c:2193` + `sub_548B60`).
        //   `fine` 은 미세각 0~255, 클라에 보내는 거친각은 `32 × fine / 256`(0~31)이다.
        //   `spin` 은 사람이 민 각속도 — 걸음마다 미세각을 돌리고 그 자리에서 지운다
        uint8_t fine = 0;
        double spin = 0.0;
        uint32_t lastPushFrame = 0;
        uint32_t nextManaFrame = 0; // 오벨리스크만 쓴다
        // ★ 09-22 — 소리는 **마나 주는 주기와 다른 주기**다(정답지는 `fps/2`). 따로 센다
        uint32_t nextManaSndFrame = 0;
        // ★ 09-22 — 마나석 **잔량**(0~50). 쓰면 틱마다 1 줄고, 안 쓰면 0.5초마다 1 찬다
        uint8_t charge = 50;
        // ── 묶음 9-c 밟는 판(`PressurePlate`)만 쓴다 ──────────────────
        //   정답지 `nox_xxx_updateTrigger_53B1B0`(`GAME4_3.c:5415`) 의 갱신자료를 옮긴 것이다.
        //   **0 놓임 · 1 눌림 · 5 한 번 쓰고 끝**(`[0] & 2` 면 떼는 순간 5 로 간다)
        uint8_t  trigState = 0;       // +8
        uint32_t trigReleaseFrame = 0;// +136 — 이 프레임을 **넘겨야** 뗀 것으로 본다
        bool     trigPressed = false; // [0] & 1 — 이번 프레임에 뭐가 올라와 있나(주기 끝에 지운다)
        bool     trigOneShot = false; // [0] & 2 — ⚠ 맵 초기자료라 우리는 못 읽는다(늘 거짓)
        bool     trigWasEnabled = false;  // [0] & 8
        bool     enabled = true;      // 오브젝트 깃발 ENABLED(0x1000000)
        uint8_t  animFrame = 0;       // +132 — `servMarkObjAnimFrame` 이 넣는 칸(0 놓임 · 1 눌림)
        // ★ 묶음 10-b — 누르고 떼는 소리 번호. thing.bin `UPDATE = TriggerUpdate <누름> <뗌>` 의
        //   두 이름을 `SoundByName` 으로 바꾼 것이다(정답지 `v1[9]`·`v1[10]`).
        //   실측 — `PressurePlate` 는 `TriggerPressed`(750) · `TriggerReleased`(751)
        uint16_t sndPressed = 0, sndReleased = 0;
    };
    void SetWorldProps(std::vector<WorldProp> v) { props_ = std::move(v); }
    // 문 칸을 벽 표에 만들어 붙인다 — 정답지 `nox_xxx_doorAttachWall_410360`.
    //   맵을 읽은 뒤(벽·문 둘 다 들어온 뒤) 부른다. 두 번 불러도 같은 결과다
    void AttachDoorWalls();
    // ★ 09-29 Z1-A — 문의 벽 칸(`nox_xxx_XFerDoor_4F4CB0` `GAME3_3.c:11520~11526` — (자리 + 표값/2) × 0.043478262 를 버림 → +16·+20).
    //   정답지는 이 칸에 벽을 붙이고(`doorAttachWall_410360(v1, v4, v5)`) 짝 문 찾기(`fnFindCloseDoors_4E8340`)도 이 칸을 본다
    static void DoorWallCell(const WorldProp& p, int& gx, int& gy);
    size_t DoorCellDiff() const { return doorCellDiff_; }   // 옛 식(자리 ÷ 23)과 칸이 다른 문 수 — AttachDoorWalls 가 센다
    size_t DoorCellOldHadWall() const { return doorCellOldHadWall_; }   // 그중 옛 칸에 맵 벽이 원래 있던 수
    size_t DoorCellNewHadWall() const { return doorCellNewHadWall_; }   // 그중 정답지 칸에 맵 벽이 원래 있던 수
    // ★ 09-29 Z1-A — 열쇠 종류의 와이어 번호. `DoorCheckKey`(`server/object_door.go:57~92`)는 KEY 클래스 물건을 **종류 이름**으로 견준다
    //   (1 SilverKey · 2 GoldKey · 3 RubyKey · 4 SapphireKey). 와이어 번호는 이름과 1:1 이라 번호로 견준다
    void SetKeyTypes(uint16_t silver, uint16_t gold, uint16_t ruby, uint16_t sapphire) {
        keyWire_[1] = silver; keyWire_[2] = gold; keyWire_[3] = ruby; keyWire_[4] = sapphire;
    }
    // ★ 09-29 Z1-A — 문에 닿은 결과(`nox_xxx_collideDoor_4E8AC0` `GAME3_3.c:3383~3516` · 문짝 밀기 `GAME5.c:2191~2215`).
    //   닿음 = 갈래 합(Closed 제외 — Closed 는 「닫혀 있어서 봤다」의 수이고 그 아래 갈래로 다시 갈린다)
    enum class DoorTouch : uint8_t {
        NotClosed = 0,   // 닫힘 각도가 아니다 — 잠김을 안 본다(`:3407`)
        Unlocked,        // 잠김 0 — 할 일 없음(`:3430~3432`)
        Mechanism,       // 잠김 5 — 「DoorLockedMechanism」(1.5초에 한 번)
        KeyMissing,      // 열쇠 없음 — 소리 240/244 + `f0 21` 「DoorLockedKey」(1.5초에 한 번)
        Quiet,           // 1.5초 안이라 말 없이 끝(`:3415·3436·3453`)
        KeyUsed,         // 열쇠로 풀림 — 소리 234 · 열쇠 지움(`4d`)
        PartnerUnlocked, // 짝 문도 풀림(`fnFindCloseDoors_4E8340`) — 위 KeyUsed 의 덤(합에 안 넣는다)
        SpinLocked,      // 잠겨서 밀어도 안 돈다(`GAME5.c:2193` `!*(v4+1)`) — 합에 안 넣는다(닿음 처리와 따로다)
        TeamBlocked,     // 팀 문 — 다른 팀이 닫힌 문을 못 민다(`GAME5.c:2191~2192`) — 합에 안 넣는다
        Count
    };
    unsigned long long doorTouch(DoorTouch t) const { return t < DoorTouch::Count ? doorTouch_[static_cast<size_t>(t)] : 0; }
    unsigned long long berserkDoorLocked() const { return berserkDoorLocked_; }
    unsigned long long sightStat(SightStat s) const { return s < SightStat::Count ? sightStat_[static_cast<size_t>(s)] : 0; }   // ★ 09-29 B1
    unsigned long long b2Stat(B2Stat s) const { return s < B2Stat::Count ? b2Stat_[static_cast<size_t>(s)] : 0; }               // ★ 09-29 B2
    unsigned long long meleeStrike(MeleeStrikeWay w) const { return w < MeleeStrikeWay::Count ? meleeStrike_[static_cast<size_t>(w)] : 0; }   // ★ 09-29 B3
    unsigned long long attackTiming(AttackTiming t) const { return t < AttackTiming::Count ? attackTiming_[static_cast<size_t>(t)] : 0; }   // ★ 09-30
    unsigned long long shotNoAim() const { return shotNoAim_; }   // ★ 09-30
    unsigned long long bundleStat(BundleStat b) const { return b < BundleStat::Count ? bundleStat_[static_cast<size_t>(b)] : 0; }   // ★ 09-30
    // ★ 09-30 — 제때·어긋남 가리지 않고 본 시작→칸 틱 수의 가장 작은·큰 값(0 = 근접 · 1 = 쏘기) · 한 번도 없으면 -1
    int attackLagMin(int kind) const { return (kind == 0 || kind == 1) && attackLagSeen_[kind] ? static_cast<int>(attackLagMin_[kind]) : -1; }
    int attackLagMax(int kind) const { return (kind == 0 || kind == 1) && attackLagSeen_[kind] ? static_cast<int>(attackLagMax_[kind]) : -1; }
    unsigned long long brainStat(BrainStat b) const { return b < BrainStat::Count ? brainStat_[static_cast<size_t>(b)] : 0; }                   // ★ 09-29 B4
    unsigned long long animStat(AnimStat b) const { return b < AnimStat::Count ? animStat_[static_cast<size_t>(b)] : 0; }                        // ★ 09-29 B5
    unsigned long long monsterAnimSent(int idx) const { return idx >= 0 && idx < 16 ? monsterAnimSent_[idx] : 0; }                      // ★ 10-01 — 갱신 항목에 실은 애니 번호별
    unsigned long long summonOwnSent(int i) const { return i >= 0 && i < 3 ? summonOwnSent_[i] : 0; }                                  // ★ 10-01 — 0 `6c` · 1 `dd` · 2 `6d`
    // ★ 09-29 B5 — 종류별 몬스터 애니 표(thing.bin MonsterDraw STAT · 순번 0~15) — main 이 싣는다
    void SetMonsterAnim(uint16_t wire, int idx, int frames, int hold, bool loop) {
        if (idx < 0 || idx >= 16) return;
        MonsterAnimRow& r = monsterAnims_[wire][static_cast<size_t>(idx)];
        r.frames = static_cast<uint8_t>(frames); r.hold = static_cast<uint8_t>(hold); r.loop = loop;
    }
    size_t MonsterAnimTypes() const { return monsterAnims_.size(); }
    int DebugPetAnimFrame(size_t i) const { return i < pets_.size() ? pets_[i].animFrame : -1; }
    int DebugPetAnimTick(size_t i) const { return i < pets_.size() ? pets_[i].animTick : -1; }
    bool DebugPetAnimDone(size_t i) const { return i < pets_.size() && pets_[i].animDone; }
    int DebugPetAnimIndex(size_t i) const { return i < pets_.size() ? MonsterAnimIndex(pets_[i]) : -1; }
    // ★ 09-29 B6 — 소리 듣기
    unsigned long long hearStat(HearStat b) const { return b < HearStat::Count ? hearStat_[static_cast<size_t>(b)] : 0; }
    unsigned long long castStat(CastStat b) const { return b < CastStat::Count ? castStat_[static_cast<size_t>(b)] : 0; }   // ★ 09-29 B7
    size_t DebugPetSpellCount(size_t i) const { return i < pets_.size() ? pets_[i].spells.size() : 0; }
    bool DebugPetHasSpell(size_t i, uint8_t sp) const { return i < pets_.size() && MonsterHasSpell(pets_[i], sp); }
    void DebugSetPetCastNext(size_t i, uint32_t f) { if (i < pets_.size()) { pets_[i].castNextOffense = f; pets_[i].castNextEscape = f; } }
    uint32_t DebugPetCastNextOffense(size_t i) const { return i < pets_.size() ? pets_[i].castNextOffense : 0; }
    uint32_t DebugPetCastNextEscape(size_t i) const { return i < pets_.size() ? pets_[i].castNextEscape : 0; }
    // 몬스터가 연 지속 주문(주문 번호 · 시전자 번호)이 있나 · 그 대상
    bool DebugPetDurActive(uint8_t sp, uint16_t net) const { for (const auto& d : durSpells_) if (d.spell == sp && d.casterPet == net && !(d.flags88 & 1)) return true; return false; }
    uint16_t DebugPetNet(size_t i) const { return i < pets_.size() ? pets_[i].netCode : 0; }
    // 종류별 몬스터 아종 비트(thing.bin SUBCLASS — 0x200 IMMUNE_POISON · 0x400 IMMUNE_FIRE …) · 위험한 종류(1 불 FIRE · 2 독구름 · 3 DANGEROUS)
    void SetMonsterSubclass(uint16_t wire, uint32_t bits) { monsterSubclass_[wire] = bits; }
    void SetDangerType(uint16_t wire, uint8_t kind) { dangerTypes_[wire] = kind; }
    size_t DebugListenCount() const { return listens_.size(); }
    int DebugPetHeardSnd(size_t i) const { return i < pets_.size() ? pets_[i].heardSnd : -1; }
    uint32_t DebugPetHeardFrame(size_t i) const { return i < pets_.size() ? pets_[i].heardFrame : 0; }
    int DebugPetHeardDist(size_t i) const { return i < pets_.size() ? pets_[i].heardDist : -1; }
    void DebugSetPetHeard(size_t i, uint16_t snd, uint32_t frame, float x, float y) {
        if (i < pets_.size()) { pets_[i].heardSnd = snd; pets_[i].heardFrame = frame; pets_[i].heardX = x; pets_[i].heardY = y; }
    }
    void DebugSetSoundDef(uint16_t id, uint16_t flags, int maxDist) {   // 시험용 — 소리 하나의 깃발·거리(조각 1)
        if (id >= soundDefs_.size()) soundDefs_.resize(static_cast<size_t>(id) + 1);
        soundDefs_[id].flags = flags; soundDefs_[id].maxDist = maxDist; soundDefs_[id].samples = 1;
    }
    void DebugSetPetHealth(size_t i, uint16_t hp) { if (i < pets_.size()) pets_[i].health = hp; }
    void DebugSetPetMoveAttempt(size_t i, uint32_t f) { if (i < pets_.size()) pets_[i].moveAttemptFrame = f; }
    uint32_t DebugPetMoveAttempt(size_t i) const { return i < pets_.size() ? pets_[i].moveAttemptFrame : 0; }
    bool DebugPetFrustrated(size_t i) const { return i < pets_.size() && pets_[i].frustrated; }
    void DebugSetPetActionFrame(size_t i, uint32_t f) { if (i < pets_.size()) pets_[i].aiActionFrame = f; }
    bool DebugPetAlert(size_t i) const { return i < pets_.size() && pets_[i].alert; }
    void DebugAIClearStack(size_t i) { if (i < pets_.size()) AIClearStack(pets_[i]); }   // ★ 09-29 B2 — 명령이 쌓임을 비우는 자리(`ClearActionStack`)
    double DebugForceX(const std::string& addr) const { auto it = sessions_.find(addr); return it != sessions_.end() ? it->second.forceX : 0.0; }   // ★ 09-29 B3
    size_t DebugPetSeenCount(size_t i) const { return i < pets_.size() ? pets_[i].seen.size() : 0; }
    // 시험용 — 몬스터 자체 시야 거리(+1312 SightRange — 정답지는 스크립트 `SetSightRange`·명령 「지켜」가 바꾼다). 알아보기 거리 = max(250·640, 이것)
    void DebugSetPetSightRange(size_t i, float r) { if (i < pets_.size()) pets_[i].sightRange = r; }
    bool DebugPetCanSee(size_t i, const std::string& addr) const { return i < pets_.size() && CanInteractFrom(pets_[i], Target{ addr, -1 }); }
    void DebugTeleport(const std::string& addr, float x, float y) { TeleportTarget(Target{ addr, -1 }, x, y, nullptr); }
    int DebugPetEnemyPet(size_t i) const { return i < pets_.size() ? pets_[i].enemyPet : -1; }
    // 시험용
    void DebugSetDoorLock(uint16_t netCode, uint8_t lock) { for (auto& p : props_) if (p.netCode == netCode) p.lock = lock; }
    int PropLock(uint16_t netCode) const { for (const auto& p : props_) if (p.netCode == netCode) return p.lock; return -1; }
    size_t WorldPropCount() const { return props_.size(); }
    // 시험용 — 그 문이 열려 있나
    bool PropOpen(uint16_t netCode) const {
        for (const auto& p : props_) if (p.netCode == netCode) return p.open;
        return false;
    }
    // 시험용 — 밟는 판(9-c)
    int PlateAnim(uint16_t netCode) const {
        for (const auto& p : props_)
            if (p.netCode == netCode && p.kind == PropKind::Plate) return static_cast<int>(p.animFrame);
        return -1;
    }
    int PlateState(uint16_t netCode) const {
        for (const auto& p : props_)
            if (p.netCode == netCode && p.kind == PropKind::Plate) return static_cast<int>(p.trigState);
        return -1;
    }
    void DebugPlateEnable(uint16_t netCode, bool on) {
        for (auto& p : props_) if (p.netCode == netCode && p.kind == PropKind::Plate) p.enabled = on;
    }
    void DebugPlateOneShot(uint16_t netCode, bool on) {
        for (auto& p : props_) if (p.netCode == netCode && p.kind == PropKind::Plate) p.trigOneShot = on;
    }
    // 시험용 — 소리
    size_t DebugAudioQueued() const { return audioEvents_.size(); }
    void DebugFlushAudio(std::vector<OutPacket>& out) { FlushAudioEvents(out); }
    void DebugUpdateProps(std::vector<OutPacket>& out) { UpdateProps(out); }
    unsigned long long doorsOpened() const { return doorsOpened_; }
    double doorNearestDist() const { return doorNearestDist_; }
    unsigned long long doorResync() const { return doorResync_; }
    unsigned long long obeliskTold() const { return obeliskTold_; }
    // ★ 09-21 벽 튕김 재기 — 사람이 벽에 밀려난 걸음 수와 **그때 더해진 가속의 최대**.
    //   사용자가 화면으로 짚었다(「벽에 부딪힐 때 원래 저렇게 튕기지 않았던 것 같다」).
    //   벽 밀어내기 식·상수(100)·감쇠·반지름은 전수로 정답지와 맞다 — 그래서 **세서** 가른다.
    //   가속 100 은 걷기 힘(3.0)의 33배다. 그게 실제로 얼마나 자주·얼마나 크게 나오나를 본다
    unsigned long long wallPushSteps() const { return wallPushSteps_; }
    double wallPushMax() const { return wallPushMax_; }
    // ★★ 09-21 — **오브젝트가 민 것**을 벽과 따로 센다. 왜 갈라야 하나:
    //   맵을 붙이고 재니 사람 속도가 54.8(걷기 6·달리기 12)까지 갔는데 **벽 계수기는 0** 이었다.
    //   벽 하나만 세고 있으면 그 0 을 「튕김 없음」으로 읽는다 — 실제로는 가구·울타리가 민 것이다.
    //   갈래 셋(놓인 것·다른 사람·소환수)을 **따로** 센다 — 하나가 나머지를 삼키지 않게.
    unsigned long long bodyPushSteps(int k) const { return bodyPushSteps_[k]; }
    double bodyPushMax() const { return bodyPushMax_; }
    void CountBodyPush(int k, double dax, double day) {
        if (dax == 0.0 && day == 0.0) return;
        ++bodyPushSteps_[k];
        const double mag = std::sqrt(dax * dax + day * day);
        if (mag > bodyPushMax_) bodyPushMax_ = mag;
    }
    // ★ 09-21 — **가장 세게 민 짝**을 통째로 적어 둔다. 가속 284 만 보면 「겹쳤다」까지밖에 모르고
    //   **무엇에 · 얼마나 · 어느 반지름으로** 겹쳤는지를 알아야 「반지름이 크다」와
    //   「깊이 들어갔다」가 갈린다(둘의 고칠 자리가 전혀 다르다)
    // ★ 09-21 — **가장 세게 민 벽 칸**을 통째로 적어 둔다. 어떤 칸이 튕기는지 알아야
    //   「문이냐 · 창이냐 · 재질 깃발 0x2 냐」가 갈린다(정답지는 셋을 다르게 다룬다).
    struct WallPushWorst {
        double mag = 0, len = 0, px = 0, py = 0, cx = 0, cy = 0;
        int gx = -1, gy = -1, dir = -1, tile = -1;
        uint32_t f2 = 0; bool flag80 = false;
    };
    const WallPushWorst& wallPushWorst() const { return wallPushWorst_; }
    // ★★ 09-22 — **닿는 첫 걸음의 깊이.** 규칙대로면 걷기 힘 3.0 과 밀기 100×깊이 가 균형을 이뤄
    //   깊이는 **0.03** 에서 멈춰야 한다. 그런데 실측 첫 발길질이 1.9 였다.
    //   「직전 걸음엔 아무 벽도 안 밀었는데 이번 걸음에 갑자기 밀린」 순간만 골라 그 깊이를 본다 —
    //   그 값이 크면 **벽을 늦게 발견한 것**(칸 사각형이 틱 시작 자리 기준이라서)이 원인이다.
    // ★★ 09-22 — 대조군과 **똑같은 네 숫자**. 정답지는 벽·오브젝트 밀기가 전부 `Sub548600` 한 곳으로
    //   모이므로 거기서 잰다(`server/object.go`, `NOX_DBG_PUSH=1`). 우리도 사람에게 더해지는 밀기를
    //   전부 여기로 모아 같은 잣대로 낸다 — 아니면 「식은 같은데 값이 다르다」를 숫자로 못 말한다.
    // ★★ 09-22 — 사람 **무게**. 정답지 `GAME3_3.c:7701` 대로 힘에서 셈한다(위저드 15.6·컨저러 18.8·워리어 30).
    //   ⚠ 상수로 두면 안 된다 — 직업마다 다르다. 힘이 아직 안 들어왔으면(0) 정답지와 같은 바닥값 10 이 된다
    double PlayerMassOf(const Session& s) const { return PlayerMassFromStrength(s.strength); }
    // ★ 2-b2 — `sub_4E86E0`(`GAME3_3.c:3272~3306`) 무게를 실은 탄성 충돌. 둘 다 **옛 속도로** 새 속도를 셈한다.
    //   무게는 float(+120), 둘째 몫 계수(v9 = 2·m2/합)도 float 칸에 담긴다 — 그대로 옮겼다
    static void ElasticExchange(float m1, double& v1x, double& v1y, float m2, double& v2x, double& v2y) {
        const float sum = m2 + m1;
        const double k11 = (m1 - m2) / static_cast<double>(sum);
        const float k12 = (m2 + m2) / sum;
        const double k21 = (m2 - m1) / static_cast<double>(sum);
        const double k22 = (m1 + m1) / static_cast<double>(sum);
        const double n1x = v1x * k11 + k12 * v2x, n1y = v1y * k11 + k12 * v2y;
        const double n2x = k21 * v2x + k22 * v1x, n2y = k21 * v2y + k22 * v1y;
        v1x = n1x; v1y = n1y; v2x = n2x; v2y = n2y;
    }
    unsigned long long pushN() const { return pushN_; }
    double pushAvg() const { return pushN_ ? pushSum_ / pushN_ : 0.0; }
    double pushMax() const { return pushMax_; }
    unsigned long long pushBig() const { return pushBig_; }
    void NotePush(double mag) {
        if (mag <= 0.0) return;
        ++pushN_; pushSum_ += mag;
        if (mag > pushMax_) pushMax_ = mag;
        if (mag > 100.0) ++pushBig_;
    }
    double wallFirstTouchMax() const { return wallFirstTouchMax_; }
    double wallFirstTouchSum() const { return wallFirstTouchSum_; }
    unsigned long long wallFirstTouchN() const { return wallFirstTouchN_; }
    void NoteWallFirstTouch(double depth) {
        wallFirstTouchSum_ += depth; ++wallFirstTouchN_;
        if (depth > wallFirstTouchMax_) wallFirstTouchMax_ = depth;
    }
    void NoteWallPushWorst(double dax, double day, int gx, int gy, int dir,
                           double px, double py, double cx, double cy) const {
        if (dax == 0.0 && day == 0.0) return;
        const double mag = std::sqrt(dax * dax + day * day);
        if (mag <= wallPushWorst_.mag) return;
        const double ex = px - cx, ey = py - cy;
        wallPushWorst_.mag = mag;
        wallPushWorst_.len = std::sqrt(ex * ex + ey * ey);
        wallPushWorst_.px = px; wallPushWorst_.py = py;
        wallPushWorst_.cx = cx; wallPushWorst_.cy = cy;
        wallPushWorst_.gx = gx; wallPushWorst_.gy = gy; wallPushWorst_.dir = dir;
        const WallTile& w = wallTiles_[gy * 256 + gx];
        wallPushWorst_.tile = w.tile;
        wallPushWorst_.f2 = (w.tile < wallDefFlags_.size()) ? wallDefFlags_[w.tile] : 0;
        wallPushWorst_.flag80 = w.flag80;
    }
    // ★★ 09-21 밤 — **맨 처음 크게 밀린 순간**을 딱 한 번만 적는다.
    //   왜 최대값이 아니라 첫 번째인가 — 튕김은 되먹임이다(세게 밀림 → 빨라짐 → 더 깊이 박힘 → 더 세게).
    //   최대값은 **사슬의 끝**이라 늘 벽이 범인처럼 보인다. 사슬의 **첫 고리**를 알아야 고칠 자리가 나온다.
    //   가르는 잣대 = 그 직전 속도. 걷기 6 · 달리기 12 이하인데 크게 밀렸다면 **거기가 시작**이다.
    struct FirstKick {
        bool have = false; int kind = -1;      // 0 놓인것 · 1 사람 · 2 소환수 · 3 벽
        double accel = 0, depth = 0, speedBefore = 0, px = 0, py = 0;
        int gx = -1, gy = -1, dir = -1;
        unsigned long long frame = 0;
    };
    const FirstKick& firstKick() const { return firstKick_; }
    // ★ 09-21 밤 — 첫 발길질부터 **차례대로** 12개. 되먹임은 「한 장면」이 아니라 **수열**이라
    //   깊이가 1.5 → 4.8 → 19 로 불어나는지, 아니면 한 번 크고 마는지는 순서로만 보인다.
    size_t kickSeqCount() const { return kickSeqN_ < 12 ? kickSeqN_ : 12; }
    size_t kickSeqTotal() const { return kickSeqN_; }   // 분모 — 「12회뿐」과 「12회만 적었다」를 가른다
    const FirstKick& kickSeq(size_t i) const { return kickSeq_[i]; }
    void NoteFirstKick(int kind, double accel, double perUnit, double vx, double vy,
                       double px, double py, int gx, int gy, int dir, unsigned long long frame) {
        if (accel < 100.0) return;   // 100 = 걷기 힘 3.0 의 33배부터 「크게」로 본다
        const FirstKick k{ true, kind, accel, accel / perUnit,
                           std::sqrt(vx * vx + vy * vy), px, py, gx, gy, dir, frame };
        if (!firstKick_.have) firstKick_ = k;
        if (kickSeqN_ < 12) kickSeq_[kickSeqN_] = k;
        ++kickSeqN_;
    }
    struct BodyPushWorst { double mag = 0, r1 = 0, r2 = 0, len = 0, px = 0, py = 0, ox = 0, oy = 0; int kind = -1; };
    const BodyPushWorst& bodyPushWorst() const { return bodyPushWorst_; }
    void NoteBodyPushWorst(int k, double mag, double r1, double r2, double len,
                           double px, double py, double ox, double oy) {
        if (mag <= bodyPushWorst_.mag) return;
        bodyPushWorst_ = BodyPushWorst{ mag, r1, r2, len, px, py, ox, oy, k };
    }
    double playerSpeedMax() const { return playerSpeedMax_; }
    // ★ 09-21 — 사람이 **벽 칸 안**에 있던 걸음 수 / 전체 걸음. 튕김의 뿌리를 가른다:
    //   0 이면 「밖에서 여러 조각이 동시에 미는 것」이고, 0 이 아니면 「들어갔다 쫓겨나는 것」이다
    // ✖ 09-21 정정 — `playerInWallSteps` 는 **「벽 칸」 안**을 센다. 벽 칸은 23×23 이고 실제 벽은
    //   그 안의 **대각선 한 줄**이라, 칸 안이어도 벽 바깥일 수 있다. **튕김의 증거가 못 된다.**
    //   가르는 것은 아래 둘 — 벽까지 **실제 거리**의 최소, 그리고 **한 걸음에 민 조각 수**의 최대.
    unsigned long long playerInWallSteps() const { return playerInWallSteps_; }
    unsigned long long playerSteps() const { return playerSteps_; }
    // 벽 선까지 가장 가까웠던 거리(몸 반지름 10 보다 한참 작으면 **깊이 박힌 것**)
    double wallNearestLen() const { return wallNearestLen_; }
    // 한 걸음에 **동시에** 민 벽 조각 수의 최대(1 이면 깊이 문제 · 여럿이면 모서리에서 겹쳐 미는 것)
    unsigned long long wallPushPerStepMax() const { return wallPushPerStepMax_; }
    unsigned long long obeliskMana() const { return obeliskMana_; }
    unsigned long long obeliskNearIdle() const { return obeliskNearIdle_; }   // ★ 09-28 — 옆에 사람이 있어 회복을 건너뛰었고 줄 것도 없던 틱
    // ⑩ 밟는 판 — **0 을 「안 눌렸다」와 「안 셌다」로 가르려고** 09-20 에 넣었다.
    //   깜빡임을 보려면 먼저 「눌리긴 했나」가 서야 한다. 보낸 건수를 눌림·뗌으로 나눠 센다
    unsigned long long plateOn() const { return plateOn_; }
    unsigned long long plateOff() const { return plateOff_; }
    // 9-a 비밀벽 — 여닫음을 **몇 번 알렸나**. 없으면 화면이 안 바뀔 때 까닭을 못 가른다
    unsigned long long wallOpenSent() const { return wallOpenSent_; }
    unsigned long long wallCloseSent() const { return wallCloseSent_; }
    // ⑥ 떠돌기 — **갈래 둘을 따로 센다**(「갈래를 세라」). 하나만 세면 0 의 뜻이 안 갈린다:
    //   · roamAdvance = 도착해서 **이음을 타고 다음 길목으로** 넘어간 횟수
    //   · roamDeadEnd = 갈 이음이 없어 **떠돌기를 접은** 횟수("Reached dead end")
    //   둘 다 0 이면 애초에 **도착을 못 한 것**이다 — 셋을 같이 읽어야 뜻이 선다
    unsigned long long roamAdvance() const { return roamAdvance_; }
    unsigned long long roamDeadEnd() const { return roamDeadEnd_; }
    // 09-28 박쥐 — 가기(7·8)가 대상 오브젝트 자리로 고쳐 적힌 틱 수 · 대상이 안 보여 잊은 횟수(`AIRefreshStackArgs`)
    unsigned long long aiMoveTargetFollow() const { return aiMoveTargetFollow_; }
    unsigned long long aiMoveTargetForgot() const { return aiMoveTargetForgot_; }
    void SetAoi(bool on) { aoiOn_ = on; }
    // 크리처 시야 판정 — 사각형 반폭. 0 이면 꺼짐(모두 싣는 옛 동작).
    // 정답지 기본은 화면절반 + 100 (1280×960 이면 740 × 580, `player.go:325` · `server.go:533`).
    void SetCreatureAoi(double halfX, double halfY) { creatureAoiHalfX_ = halfX; creatureAoiHalfY_ = halfY; }
    // 진단 — 첫 사람이 들어오고 N 틱 뒤 크리처를 좌우 ±300 으로 한 번 옮긴다(사라짐 판정용). 0 이면 끔.
    void SetDebugCreatureAwayAfter(uint32_t ticks) { debugAwayAfter_ = ticks; }
    // 친구 알림(OBJECT_FRIEND_ADD/REMOVE)을 켠다. 기본 꺼짐 — 부분 구현이라(Server.cpp 친구 알림 주석).
    void SetFriends(bool on) { friendsOn_ = on; }
    // 진단 — 첫 사람이 들어오고 N 틱 뒤 **그 사람 소유 임프**를 사람 자리에 만들어 **고정**한다(친구 판정용). 0 이면 끔.
    void SetDebugOwnPetAfter(uint32_t ticks) { debugOwnPetAfter_ = ticks; }
    // 진단 — 같은 임프를 **주인 없이**(맵 몬스터 = 적) 사람 오른쪽 +25 에 고정하고 맞게 둔다(묶음 3-a 주먹 판정용).
    void SetDebugDummyAfter(uint32_t ticks) { debugOwnPetAfter_ = ticks; debugPetWild_ = true; }
    void SetDebugDummyDx(int dx) { debugDummyDx_ = dx; }
    // 진단 --debug-enchant — 첫 사람 들어오고 after 틱 뒤 버프를 끝없이(길이 0) 건다(묶음 4-b 화면 판정)
    void SetDebugEnchant(int buff, int power, uint32_t after) { debugEnchantBuff_ = buff; debugEnchantPower_ = power; debugEnchantAfter_ = after; }   // 진단 --debug-dummy-dx — 허수아비 오른쪽 거리(기본 25 · 4-a2 날리기 화면)
    // 진단 — N 틱마다 관전 아닌 첫 사람에게 주인 없는 것의 칼 피해 25(종류 0)를 준다(묶음 3-e 움찔·깎임 화면 판정용)
    void SetDebugHurtEvery(uint32_t ticks, bool front = false) { debugHurtEvery_ = ticks; debugHurtFront_ = front; }
    // ★ 묶음 3-c — 맵의 PlayerStart 자리(부활 자리 고르기 mapFindPlayerStart_4F7AB0). main.cpp 가 맵 오브젝트에서 채운다.
    void SetPlayerStarts(std::vector<std::pair<float, float>> v) { playerStarts_ = std::move(v); }
    // ★ 맵 길목 — 점멸이 여기서 목적지를 고른다(`waypoint_579F00`). 비었으면 시작 지점으로 되돌아간다.
    //   자리만 주면 번호를 1부터 매기고 **이음은 없는** 길목이 된다(시험용).
    void SetWaypoints(std::vector<std::pair<float, float>> v) {
        waypoints_.clear();
        waypoints_.reserve(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            Waypoint w;
            w.id = static_cast<uint32_t>(i + 1);
            w.x = v[i].first;
            w.y = v[i].second;
            waypoints_.push_back(std::move(w));
        }
        AIPathBakeWaypoints();
    }
    // ★ 09-20 묶음 6-j(2) — **이음까지** 실어 준다. main.cpp 가 맵에서 읽어 넘긴다.
    //   ⚠ 넘기기 전에 `flags & 1`·`flags2 & 0x80` 으로 걸러 둘 것 —
    //     우리가 쓰는 가면(mask)은 어디서나 0x80 이라 **목록에 있다 = 쓸 수 있다**가 된다.
    //     이음이 걸러진 길목을 가리키면 찾기가 실패하는데, 그게 곧 정답지의 `sub_547EE0` 거짓이다
    void SetWaypointsFull(std::vector<Waypoint> v) { waypoints_ = std::move(v); AIPathBakeWaypoints(); }
    size_t WaypointCount() const { return waypoints_.size(); }
    size_t PlayerStartCount() const { return playerStarts_.size(); }
    // 조각 E — 판을 아홉 개 돌리려면 맵마다 봇 자리를 알아야 한다. i 번째 시작 자리를 그대로 준다.
    bool PlayerStartAt(size_t i, float& x, float& y) const {
        if (i >= playerStarts_.size()) return false;
        x = playerStarts_[i].first; y = playerStarts_[i].second; return true;
    }
    // 자동 부활 서버 설정(nox_server_doPlayersAutoRespawn_40A5F0 · 0x5D4594+3584). 정답지 기본 0 — 클릭해야 살아난다.
    void SetAutoRespawn(bool on) { autoRespawn_ = on; }
    // ★ 묶음 3-d — TRY_COLLIDE 가 번호(또는 extent)로 오브젝트를 찾고, 그 종류의 충돌 함수(thing.bin COLLIDE 첫 낱말)를 고른다.
    struct MapObjRef {
        uint16_t netCode = 0;   // 우리가 매긴 번호(맵 색인 + 75)
        uint32_t extent = 0;    // 맵 안 고유 번호 — 클라가 IMMOBILE·CLIENT_PERSIST 에 윗비트를 붙여 보낸다(`GAME5_2.c:893`)
        uint16_t type = 0;
    };
    void SetMapObjects(std::vector<MapObjRef> v) { mapObjects_ = std::move(v); }
    void SetCollideFn(uint16_t type, std::string fn) { collideFn_[type] = std::move(fn); }
    // 무기 클래스 비트 → 종류 번호(죽음 알림의 무기 그림). main 이 thing.bin 이름으로 채운다
    void SetWeaponWire(uint32_t classBits, uint16_t wire) { weaponWire_[classBits] = wire; }
    unsigned long long tryCollides() const { return tryCollides_; }
    unsigned long long tryCollideDeferred() const { return tryCollideDeferred_; }
    // 진단 — 첫 사람이 들어오고 N 틱 뒤 그 사람 팀을 한 번 바꾼다(RESET_FRIENDS 판정용). 0 이면 끔.
    void SetDebugTeamAfter(uint32_t ticks) { debugTeamAfter_ = ticks; }
    void SetDebugTeamEvery(uint32_t ticks) { debugTeamEvery_ = ticks; }
    void SetDebugScore(uint32_t ticks, uint32_t val) { debugScoreAfter_ = ticks; debugScoreVal_ = val; }
    void SetDebugKill(uint32_t ticks) { debugKillAfter_ = ticks; }
    // ★ 09-20 진단 `--debug-fist N dx dy` — **서버가 직접** 첫 사람 자리에서 (dx,dy) 만큼 떨어진 곳에
    //   주먹을 떨어뜨린다. 클라로 시키면 시전 시각(접속 뒤 25초 + 주기)·벽·자기 피해 때문에 헛돈다(09-20 여덟 번).
    //   서버가 틱을 정해 쏘면 **녹화 시각을 그 틱에 맞출 수 있다**
    void SetDebugFist(uint32_t ticks, int dx, int dy) { debugFistAfter_ = ticks; debugFistDx_ = dx; debugFistDy_ = dy; }
    // ★ 09-20 진단 `--debug-warp N x y` — **첫 사람을 N 틱 뒤 그 자리로 한 번 옮긴다.**
    //   왜 — 문·밟는 판처럼 **다가가야** 알림이 나는 것들은, 시작 자리에 세워 두면 알림이
    //   **접속 0.2초 만에** 나가 클라가 준비 전이라 버린다(09-20 클라 덤프로 확인).
    //   그리고 우리는 **바뀔 때만** 보내므로 그 창에서 놓친 상태는 영영 다시 안 간다.
    //   서버가 시각을 정하면 「다 들어온 뒤에 다가가기」를 만들 수 있다(⑰ 주먹과 같은 수법).
    void SetDebugWarp(uint32_t ticks, int x, int y) { debugWarpAfter_ = ticks; debugWarpX_ = x; debugWarpY_ = y; }
    // ★ 09-20 진단 `--debug-wall N gx gy` — **N 틱 뒤 그 칸의 비밀벽을 연다.**
    //   왜 — 비밀벽은 **다가가서 열리는 것이 아니다.** 상태 기계는 깃발 4·8 이 돌리는데
    //   G_Castle 254개·G_Temple 73개가 **전부 0** 이다(09-20 실측) — 여는 것은 **맵 각본**이고
    //   각본은 플랜 v6 범위 밖이다. 그래서 「무엇이 여는가」와 「클라가 그려 주나」를 갈라,
    //   뒤엣것만 서버가 직접 열어 본다(⑰ 주먹·②⑩ 순간이동과 같은 수법)
    void SetDebugWall(uint32_t ticks, int gx, int gy) { debugWallAfter_ = ticks; debugWallGx_ = gx; debugWallGy_ = gy; }
    // ★ 09-20 진단 `--debug-pet-order <명령>` — `--debug-own-pet` 으로 만든 소환수에게 **명령을 준다.**
    //   왜 — 주인 있는 크리처는 **명령을 받아야** 움직인다(`EnactUnitOrder`). 명령이 없으면
    //   적이 15 까지 다가와도 **한 칸도 안 움직인다**(09-20 실측 50표본). ⑥ 「벽을 돌아가나」를
    //   보려면 **사냥(5)** 을 줘야 한다. 2 가만히 · 3 지켜 · 4 따라와 · 5 사냥
    void SetDebugPetOrder(uint8_t order) { debugPetOrder_ = order; }
    // ★ 09-20 진단 `--debug-mob N <종류> <x> <y>` — **N 틱 뒤 그 자리에 맵 몬스터 하나를 세운다.**
    //   왜 — ⑮ 「덫이 몬스터를 죽이나」를 **화면으로** 보려면 몬스터가 **덫 선 위**에 있어야 하는데,
    //   퀘스트 스폰은 **사람 자리를 기준**으로 잡혀서 늘 사람 옆에 나온다. 그러면 몬스터가 사람에게
    //   달라붙어 화면이 사람을 따라 흔들리고, 죽는 순간을 못 찍는다(09-20 에 **세 번** 실패).
    //   ⚠ 주인을 비워 만든다 — 그래야 **맵 몬스터**로 취급되어 덫 화살이 적으로 본다(⑮ 편 거르기)
    void SetDebugMob(uint32_t ticks, uint16_t type, int x, int y) {
        debugMobAfter_ = ticks; debugMobType_ = type; debugMobX_ = x; debugMobY_ = y;
    }
    // 진단 — 첫 사람이 들어오고 N 틱 뒤 경험치를 한 번 준다. 0 이면 끔.
    //   왜 — 경험치는 스크립트·퀘스트가 주는 것이라 아레나에서는 저절로 안 생긴다.
    //   ④대기 ⑨ 「HUD 경험치 막대가 차오르나」를 정품 화면으로 보려면 줄 길이 있어야 한다.
    void SetDebugXpAfter(uint32_t ticks, float amount) { debugXpAfter_ = ticks; debugXpAmount_ = amount; }
    // 진단 — 첫 사람이 들어오고 N 틱 뒤 주문 하나를 준다. 주문 0 이면 끔.
    //   ④대기 ⑨ 「주문책에 주문이 느나」용. announce=1 이라 받는 소리(226)도 함께 난다.
    void SetDebugGiveSpellAfter(uint32_t ticks, uint8_t spell) { debugSpellAfter_ = ticks; debugSpellId_ = spell; }
    // 진단 — 들어온 컨저러마다 60틱 뒤 박쥐·흰거미 둘·거미 안내서를 준다(09-24 사람 판 「물릴 때 맞는 소리」용).
    //   아레나에서 안내서는 맵 두루마리를 줍거나 저장 캐릭터에서만 오는데 우리는 둘 다 없어 부를 짐승이 없다
    void SetDebugConjurerGuides(bool on) { debugConjurerGuides_ = on; }

    // ── 독립 모드 (P5-c-2) ──────────────────────
    //
    // 녹음본(joinBody_·useMapBody_·firstSnapshot_) 없이 접속을 받는다.
    //   접속 묶음 = BuildJoinBundle(클라 97B 되돌려줌)
    //   USE_MAP    = BuildUseMap(아래 메타)
    //   스냅샷      = 27 + a4(현 세계) + BuildSnapshotTail(순번은 RenumberSeq 가 다시)
    // ⚠ 기본 꺼짐 — 실클라 검증(P5-c 졸업) 전에는 녹음 경로가 정본이다.
    void SetStandalone(const std::string& mapFileName, uint32_t mapCrc) {
        standalone_ = true;
        saMapName_ = mapFileName;
        saMapCrc_ = mapCrc;
    }
    size_t WorldStaticCount() const { return statics_.size(); }

    bool CanStand(double x, double y, double r) const {
        if (!wallsOn_) return true;
        return !WallAt(x - r, y) && !WallAt(x + r, y) && !WallAt(x, y - r) && !WallAt(x, y + r);
    }

    // ★ 남 항목 덧붙이 값(--peer-pose HH). 자기 것은 늘 0x40 이고 그건 그려진다.
    void SetPeerPose(uint8_t v) { peerPose_ = v; }
    // ★ 갱신 항목 순서 뒤집기(--peer-first). 가르는 시험용.
    void SetPeerFirst(bool v) { peerFirst_ = v; }
    // ★ 상대 몫 첫 스냅샷 통째 보내기(--peer-snapshot).
    void SetPeerSnapshot(bool v) { peerSnapshot_ = v; }
    // ★ 남의 종류 번호(--peer-type N). 0 이면 사람. 가르는 시험용.
    void SetPeerType(uint16_t v) { peerType_ = v; }
    // ★ 옛 동작(매 틱 전체번호 소개)으로 되돌리는 대조용 손잡이(--intro-every-tick).
    void SetIntroEveryTick(bool v) { introEveryTick_ = v; }
    // 클라에게서 받은 별칭 알림 수. 틱 로그에 찍어 「받고 있나」를 바로 본다.
    unsigned long long aliasHeard() const { return aliasHeard_; }
    // 접속 악수 계수기 셋 — 무적 사슬의 어느 고리가 되풀이되는지 가른다(페이즈 B 1-a).
    unsigned long long readyHeard() const { return readyHeard_; }
    unsigned long long useMapSent() const { return useMapSent_; }
    unsigned long long acceptHeard() const { return acceptHeard_; }
    // ★ 시전 갈래 — `PlayerSpell` 안에서 **조용히 빠지던** 자리들(2026-09-18)
    unsigned long long spellPlayerCalls() const { return spellPlayerCalls_; }
    unsigned long long spellLeafZero()    const { return spellLeafZero_; }
    unsigned long long spellIndZero()     const { return spellIndZero_; }
    unsigned long long spellNotLearned()  const { return spellNotLearned_; }
    unsigned long long spellFriendAtCursor() const { return spellFriendAtCursor_; }
    unsigned long long spellErrInform()  const { return spellErrInform_; }
    unsigned long long spellCastFailed() const { return spellCastFailed_; }
    unsigned long long fistTooMany() const { return fistTooMany_; }
    unsigned long long fistNoLos()   const { return fistNoLos_; }
    unsigned long long reliableHeard() const { return reliableHeard_; }
    unsigned long long ackSeqApplied() const { return ackSeqApplied_; }
    unsigned long long ackSeqDiffer() const { return ackSeqDiffer_; }
    // ★ 2026-09-18 B-1b — **스트림별로 갈라 센다.** 합계만 보면
    //   「클라가 보낸 스트림」과 「서버가 답한 스트림」이 어긋나도 안 드러난다.
    //   되받기는 **같은 스트림에 실려야** 뜻이 있다(StreamSeq 가 스트림마다 따로다).
    unsigned long long recvReliableOn(uint8_t st) const { return st < 8 ? recvRelStream_[st] : 0; }
    unsigned long long sendReliableOn(uint8_t st) const { return st < 8 ? sendRelStream_[st] : 0; }
    unsigned long long sendPlainOn(uint8_t st) const { return st < 8 ? sendPlainStream_[st] : 0; }
    unsigned long long ackValueCount(uint8_t v) const { return v < 32 ? ackValueHist_[v] : 0; }
    // ★ 09-21 — **되받기 값이 올라가나**. 옛 `ackSeqDiffer` 는 서로 다른 둘을 견주던 숫자라 뜻이 없었다
    //   (`Server.cpp` 되받기 가지 주석). 이것이 0 이면 클라의 신뢰 순번이 **멈춘** 것이다.
    unsigned long long ackSeqChanged() const { return ackSeqChanged_; }
    uint8_t ackLastSent() const { return ackLastSent_; }
    // B-1b 되받기 끄기(대조군) — `--no-ack-seq`
    void SetAckRecvSeq(bool v) { ackRecvSeq_ = v; }
    // 되받기를 **모든 스트림의 비신뢰**에 싣는다 — 클라가 읽는 쪽이 스트림 0 이라서(Emit 주석)
    void SetAckAnyStream(bool v) { ackAnyStream_ = v; }
    // 부활 무적 안 걸기(`--no-spawn-invuln`) — 화면 확인을 막는 무적을 우회한다
    void SetNoSpawnInvuln(bool v) { noSpawnInvuln_ = v; }
    unsigned long long spawnInvulnApplied() const { return spawnInvulnApplied_; }
    unsigned long long castRejDead() const { return castRejDead_; }
    unsigned long long castRejRange() const { return castRejRange_; }
    unsigned long long castRejUnknown() const { return castRejUnknown_; }
    unsigned long long castRejBusy() const { return castRejBusy_; }
    unsigned long long castRejClass() const { return castRejClass_; }
    unsigned long long castRejCant() const { return castRejCant_; }
    unsigned long long castAccepted() const { return castAccepted_; }
    unsigned long long priMsgSent() const { return priMsgSent_; }
    unsigned long long arrowGoneWall() const { return arrowGoneWall_; }
    unsigned long long arrowGoneWallAtBirth() const { return arrowGoneWallAtBirth_; }
    unsigned long long arrowGoneAge() const { return arrowGoneAge_; }
    // 벽에 닿은 화살의 **평균 수명(틱)** — 0 이면 잰 것이 없다
    double arrowLifeAvg() const { return arrowLifeCnt_ ? double(arrowLifeSum_) / double(arrowLifeCnt_) : 0.0; }
    size_t missilesPeak() const { return missilesPeak_; }
    unsigned long long trapFailRange() const { return trapFailRange_; }
    unsigned long long trapFailFacing() const { return trapFailFacing_; }
    unsigned long long trapFailWall() const { return trapFailWall_; }
    unsigned long long trapSeeOk() const { return trapSeeOk_; }

    // 서버의 현재 프레임. 진짜 서버는 30Hz 틱이 올리고,
    // 덤프를 재생할 때는 그때 나간 값을 넣어 준다(시점에 달린 값이라 재현이 안 된다).
    void SetFrame(uint32_t f) { frame_ = f; }
    // 접속한 사람에게 줄 오브젝트 번호. 이것도 서버가 정하는 값이다.
    void SetNetCode(uint16_t c) { netCode_ = c; }

    // ★ 들어오는 것이 이미 평문인가 — 덤프 재생 전용 스위치.
    //
    // 진짜 소켓에서는 키를 준 뒤 받는 것이 암호문이라 풀어야 한다.
    // 그런데 **덤프의 RECV 는 이미 평문이다** — 기록을 건 자리가 복호화 직후라서다
    // (Docs/PROTOCOL.md 「XOR 은 방향에 따라 다르다」). 그걸 또 풀면 되레 망가진다.
    // 이 스위치를 안 두고 돌렸을 때 `0x3f`(클라 입력) 32,450건이
    // `0x4f`(방어구 장착)로 읽혔다 — 정확히 `0x3f ^ 0x70` 이다.
    void SetInboundPlain(bool v) { inboundPlain_ = v; }

    // 이 주문이 몇 대미지인가. 공격 주문이 아니면 0.
    //
    // ⚠⚠ **숫자는 실측이 아니다.** 09-06 덤프에 `0x42` 표본이 −24·−36·−75·−127 로 있지만
    //    **어느 공격이 만든 값인지 가릴 수 없었다**(그 시각에 주문·근접이 섞여 있다).
    //    어느 주문이 공격이냐는 이름표(`SpellNames.cpp`)에서 가른 것이고,
    //    **값은 게임이 도는지 보려고 넣은 자리표시**다. 실측이 생기면 갈아 끼운다.

    // 이 주문이 날리는 것의 **와이어 종류 번호**. 공격이 아니면 0.
    //
    // ⚠⚠ **2026-09-07 정정 — 이건 실측이 아니라 이름 짐작이다.**
    //
    //   처음에 「종류표에서 찾았으니 실측」이라고 적었다. **틀렸다.**
    //   종류표(`/debug/nox/types`)에 `695 Fireball` 이라는 **이름이 있다**는 것과,
    //   게임이 파이어볼을 날릴 때 **그 번호를 쓴다**는 것은 다른 말이다.
    //   표에서 이름을 고른 것은 measurement 가 아니라 matching 이었다.
    //
    //   확인하려 했더니 —
    //     · 09-07 에 파이어볼을 네 번 쐈고 `0x79 TRY_SPELL` 이 **4건 와이어에 갔다.**
    //       그런데 `0xa4` 에 **새 오브젝트가 하나도 안 생겼고 `0xa9` 알림도 0건**이다
    //     · 09-06 덤프에도 695 는 **0건**. 대신 클라 덤프에 **`693 TitanFireball`** 이
    //       네 번 보이는데, 그중 셋은 좌표가 맵 밖(8669,80 · 59691,32)이라 헛맞음이다
    //
    //   → **어느 번호가 맞는지 아직 모른다.** 셋 다 복합이라 짜임은 어긋나지 않지만,
    //     클라가 엉뚱한 그림을 그릴 수 있다. `Docs/UNMEASURED.md` 참조.

    // 이 주문이 불러내는 것의 **와이어 종류 번호**. 소환 주문이 아니면 0.
    //
    // ⚠ **이것도 이름 짝짓기지 실측이 아니다** (09-07 정정).
    //   `SpellNames.cpp` 의 `SPELL_SUMMON_*` 과 종류표의 같은 이름을 맞춘 것이다.
    //   **이름이 같다고 게임이 그 번호를 쓰는지는 확인 못 했다** —
    //   투사체 쪽에서 같은 방식으로 고른 `695 Fireball` 이 네 덤프에 0건이었고,
    //   대신 `693 TitanFireball` 이 보였다(위 `ProjectileType` 주석).
    //   **소환 표본이 아예 없어** 여기는 대조할 거리조차 없다.
    //   짝지은 열둘이 전부 COMPLEX 인 것만은 표에서 확인했다 — 짜임은 안 어긋난다.
    static uint16_t SummonType(uint8_t spell);

    // 이 상대에게 준 XOR 키. 아직 안 줬으면 0.
    // 덤프의 SEND 를 사람이 읽으려면 이게 필요하다.
    uint8_t KeyFor(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : (it->second.keyed ? it->second.xorKey : 0);
    }

    // 들어온 조각 하나. 나갈 것은 out 에 붙인다.
    // 돌려주는 값 = 이 조각에서 읽어낸 메시지 수.
    size_t OnPacket(const std::string& from, const uint8_t* data, size_t n,
                    std::vector<OutPacket>& out);

    // ★ 시험용 — 접속 절차를 다 밟지 않고 게임 안 상태를 만든다.
    //
    // 진짜 흐름은 `OnPacket` 이 만든다. 여기는 **죽음·부활을 시험하려고** 둔 좁은 통로다.
    // 이게 없으면 죽음 시험을 하려고 216바이트 접속 묶음까지 흉내내야 한다.
    // 깃발 판을 차린다. 팀은 들어온 차례로 번갈아 준다.
    //
    // ⚠ **팀을 어떻게 나누는지는 실측이 아니다.** 진짜 녹스는 콘솔·메뉴로 고르게 한다.
    void StartCtf(int redX, int redY, int blueX, int blueY);

    // 팀을 접속한 차례로 번갈아 나눈다. 사람이 들어올 때마다 다시 부른다.
    // ⚠ `sessions_` 순회는 **주소 사전순**이라 들어온 차례가 아니다 — `joinOrder` 로 정렬한다.
    void AssignTeams();

    // 깃발 판 승자 `0x57`(8B) · 깃발 상태 HUD `0xd8`(6B).
    // ⭕ 둘 다 오픈녹스 클라 원본에서 짜임 확정(2026-09-11). 우리 덤프 표본은 0건인데
    //   그 까닭은 **정품 녹음에 CTF 판이 한 판도 없어서**다.
    // 문·오벨리스크를 매 틱 돌본다 (P5). 문은 다가가면 열리고, 오벨리스크는 마나를 채운다.
    void UpdateProps(std::vector<OutPacket>& out);

    void SendFlagWinner(uint8_t team, std::vector<OutPacket>& out);
    void SendFlagStatus(std::vector<OutPacket>& out);

    // 퀘스트 판을 차린다 — 주인 없는 몬스터를 `count` 마리 세운다.
    // 다 잡으면 판이 끝난다.
    //   ★ `spread` — 가운데에서 이만큼 벌려 세운다. 기본은 실측 아닌 자리표시 300.
    //     실클라로 돌릴 때는 **근접 사거리 안**(45)에 세워야 걷지 않고도 시험이 된다.
    void StartQuest(int count, int centerX, int centerY, double spread = kQuestSpread);
    // 지금 스테이지의 몬스터를 세운다. out 이 있으면 스테이지 알림도.
    void SpawnQuestStage(std::vector<OutPacket>* out);

    // 서두(녹음본)에서 읽어 낸 스폰 자리. 모드를 차릴 때 기준으로 쓴다.
    int spawnX() const { return spawnX_; }
    int spawnY() const { return spawnY_; }
    // 모드 판을 세울 자리 — 들어와 있는 사람들의 **가운데**(없으면 위 고정값).
    //   ★ 09-19 — 첫자리가 맵 PlayerStart 로 바뀐 뒤로 `spawnX_` 는 **사람이 선 자리가 아니다**.
    //   깃발(맵에 없을 때)·공·퀘스트 웨이브를 사람 곁에 놓던 하네스(`main.cpp`)가 이 값을 쓴다
    void PlayersCenter(int& x, int& y) const;
    // ★ 2026-09-15 — **시작 자리를 손으로 지정한다**(`--spawn-at X Y`).
    //   왜 — ④ 화면 확인은 「볼 것이 있는 자리」까지 가야 하는데, 맵의 `PlayerStart` 가
    //   그 자리와 멀면 매번 한참을 걸어야 한다. G_Castle 은 시작이 (1244,645)이고
    //   발판·덫·상자·문이 다 모인 자리는 (2493,2532)라 **2,200 넘게 떨어져 있다.**
    //   ⚠ 부활 자리는 `playerStarts_` 가 따로 고르므로 **첫 접속 자리만** 바뀐다.
    void SetSpawn(int x, int y) { spawnX_ = x; spawnY_ = y; spawnForced_ = true; }

    // ★ 시험용 — **스폰 자리를 아이템 자리로** 옮긴다. 없으면 false.
    //
    //   「어떤 조작이 줍기를 보내나」를 재려면 물건 위에 서 있어야 하는데 걷기가 자꾸 어긋난다.
    //
    // ⚠⚠ **반대로 하면 안 된다** — 아이템을 옮기면 **클라와 어긋난다.**
    //   클라는 맵을 읽어 **자기가 그 자리에 아이템을 만든다**(`main.cpp` 「번호 = 맵 색인+75」).
    //   서버가 좌표만 바꿔 보내면 클라 화면에는 옛 자리에 그대로 있고 발밑엔 아무것도 없다.
    //   2026-09-10 스윕 2·3차가 그래서 헛돌았다 — 왼클릭은 먹었는데(클릭 +1)
    //   **주울 물건이 화면에 없어서** 공격만 나갔다.
    bool DebugSpawnAtStatic() {
        if (statics_.empty()) return false;
        spawnX_ = statics_[0].x;
        spawnY_ = statics_[0].y;
        spawnForced_ = true;   // 09-19 — 안 걸면 첫자리가 맵 PlayerStart 로 가서 이 진단이 조용히 무시된다
        return true;
    }
    // ★ 09-27 B6 — 그 종류의 첫 바닥 물건 자리로 스폰(`--spawn-at-item <이름>`) · 번호를 돌려준다(자동 클라가 줍기·입기 코드로 쓴다)
    bool DebugSpawnAtStaticType(uint16_t wire, uint16_t& ncOut) {
        for (const auto& w : statics_) {
            if (w.type != wire) continue;
            spawnX_ = w.x; spawnY_ = w.y; spawnForced_ = true; ncOut = w.netCode;
            return true;
        }
        return false;
    }
    size_t FlagCount() const { return flags_.size(); }

    // ★ 지금 누가 어디 있나 — **하네스가 걸어가게 하려면 이게 있어야 한다.**
    //   정품 클라는 자기 자리를 밖으로 안 알려 준다(오픈녹스만 디버그 API 가 있다).
    //   서버는 이미 다 알고 있으니 로그로 내보내고, 하네스가 그걸 읽어 각을 잰다.
    //   짜임 — `사람 <번호>=<x>,<y>,<방향>` · `깃발 <팀>=<x>,<y>` · `몬스터 <번호>=<x>,<y>`
    std::string PositionsLine() const;

    // ★★ **둘이 서로 적인가** — 정품 `IsEnemyTo` 를 사람 둘에 맞춰 옮긴 것.
    //
    //   ⭕ 정품 `opennox/src/server/object.go:1667-1741` 의 핵심 줄만 따왔다 —
    //       자기 자신이면              → 적 아님
    //       주인이 같으면              → 적 아님   (내 소환수)
    //       **팀이 같으면**            → 적 아님
    //       **한쪽이라도 팀이 있으면** → **적**
    //       둘 다 몬스터면             → 적 아님
    //       그 밖                      → **적**
    //     (물고기·개구리·쥐·매혹 갈래는 3모드에 안 쓴다.)
    //
    //   ★ 왜 필요한가 — `0x41`(유닛 현재 체력)은 **적이 아닐 때만** 나간다
    //     (`GAME4_2.c:7152-7157`). 즉 **남의 머리 위 체력**이 이 판정에 달려 있다.
    //
    //   ➖ **퀘스트는 우리가 정한다** — 정품이 퀘스트에서 사람들을 어떻게 묶는지 못 봤다.
    //     협동 판이므로 **팀이 0 이어도 사람끼리는 아군**으로 본다.
    // ★ R4 — 「바뀌었으니 알려라」. 값이 바뀌는 자리에서 부른다.
    void MarkDirty(uint16_t netCode, PendingReport::What what);
    // 쌓인 것을 보내고 비운다 (틱 끝에서 한 번).
    void FlushPending(std::vector<OutPacket>& out);
    // 시험·로그용 — 0x41 을 몇 번 보냈나.
    unsigned long long unitHeightSent() const { return unitHeightSent_; }
    unsigned long long unitHealthSent() const { return unitHealthSent_; }
    // 조각 I — 체력이 가는 길은 **셋**이다. 하나만 세면 「안 갔다」와 「딴 길로 갔다」가 안 갈린다.
    unsigned long long healthDeltaSent() const { return healthDeltaSent_; }  // 42 줄어든 만큼(보는 사람마다)
    unsigned long long healthAbsSent() const { return healthAbsSent_; }      // 43 내 체력 절대값(본인만)
    // 42 가 **막힌** 횟수도 센다 — 0 이 「일이 없었다」인지 「관문에 걸렸다」인지 가른다
    unsigned long long healthDeltaHeld() const { return healthDeltaHeld_; }
    // ★ 조각 I — **피해가 어디서 사라지나.** `HurtTarget` 은 이탈구가 열한 개인데
    //   계수기는 무적 하나뿐이었다. 그래서 「안 맞았다」와 「막았다」와 「편이 아니다」가
    //   전부 같은 침묵으로 보였다. 이유별로 세어 그 침묵을 가른다.
    const std::map<std::string, unsigned long long>& damageStops() const { return damageStops_; }
    // 시험·로그용 — 0x5A 를 몇 번 보냈나.
    unsigned long long enchantSent() const { return enchantSent_; }

    bool IsEnemyTo(const Session& a, const Session& b) const;
    // 시험·로그용 — 주소로 부른다. 둘 중 하나라도 없으면 `false`(적이 아님).
    bool IsEnemyTo(const std::string& a, const std::string& b) const;

    int QuestLeft() const { return questTotal_ - questKilled_; }
    int QuestKilled() const { return questKilled_; }
    int QuestStage() const { return questStage_; }
    // 시험용 — 주인 없는 몬스터 하나를 죽인다(위치·근접에 안 기대고 스테이지 진행을 잰다).
    void DebugKillOneWild(std::vector<OutPacket>& out) {
        for (size_t i = 0; i < pets_.size(); ++i) {
            if (pets_[i].owner.empty()) { KillPet(i, out); return; }
        }
    }
    // 시험용 — 불려 나온 것 하나를 **깎는다**(죽이지 않는다). 넷코드를 돌려준다, 없으면 0.
    //
    // ★ 왜 필요한가 — `0x41`(유닛 현재 체력)이 **주인에게만** 가는지 재려면
    //   소환수 체력을 바꿔야 하는데, 실제 경로(투사체 명중)는 자리·각도에 기대 흔들린다.
    //   ⚠ 이 손잡이는 **`MarkDirty` 를 반드시 같이 부른다** — 안 그러면 실제 경로와 달라져
    //     「시험은 통과하는데 판에서는 안 되는」 자리가 생긴다.
    //   ⚠ 처음엔 「얼마 깎기」로 만들었다가 **Bomber 체력이 1 이라 아무것도 못 깎았다.**
    //     0 을 돌려주며 까닭을 안 알려 줘 한참 헤맸다 — 「손잡이가 먹었는지 찍어라」를 어긴 자리다.
    //     이제 **체력을 그 값으로 정한다** — 얼마가 남아 있든 시험이 선다.
    uint16_t DebugSetOwnedPetHealth(const std::string& owner, uint16_t hp) {
        for (auto& k : pets_) {
            if (k.owner != owner) continue;
            k.health = hp;
            MarkDirty(k.netCode, PendingReport::What::UnitHealth);
            return k.netCode;
        }
        return 0;
    }
    // 시험용 — 불려 나온 것 하나를 그 자리에 바로 세운다(시전 시간을 안 거친다).
    //   소환 한도처럼 「자리가 이미 차 있을 때」를 만들어야 하는 시험에 쓴다.
    void DebugSpawnPet(uint16_t type, float x, float y, const std::string& owner) {
        SpawnCreature(type, x, y, owner);
    }
    // ★ 4-c3b2 시험용 — 그 소환수가 `name` 발사체를 그 자리로 쏜다(AI 를 안 거친다)
    void DebugCreatureShoot(size_t i, const char* name, float tx, float ty, std::vector<OutPacket>& out) {
        if (i < pets_.size()) SpawnCreatureMissile(pets_[i], name, tx, ty, out);
    }
    // 시험용 — 그 사람의 소환수 체력(없으면 0). 위 손잡이가 왜 실패했는지 가르는 데 쓴다.
    uint16_t DebugOwnedPetHealth(const std::string& owner) const {
        for (const auto& k : pets_) if (k.owner == owner) return k.health;
        return 0;
    }
    uint16_t CapturesOf(uint8_t team) const { return team < 3 ? caps_[team] : 0; }
    // 묶음 8-d — 플래그볼 판을 차린다(깃발 판 `StartCtf` 와 같은 꼴)
    void StartFlagBall(double ballX, double ballY,
                       double goal1X, double goal1Y, double goal2X, double goal2Y);
    void AddBallStart(double x, double y) { ballStarts_.push_back({ x, y }); }
    size_t BallCount() const { return ball_.size(); }
    std::string BallCarrier() const { return ball_.empty() ? std::string() : ball_[0].carrier; }
    double BallX() const { return ball_.empty() ? 0.0 : ball_[0].x; }
    double BallY() const { return ball_.empty() ? 0.0 : ball_[0].y; }
    uint8_t TeamOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.team;
    }
    // 이 사람이 깃발을 들고 있나.
    bool Carrying(const std::string& addr) const {
        for (const auto& f : flags_) if (f.carrier == addr) return true;
        return false;
    }
    void DebugSetTeam(const std::string& addr, uint8_t team) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.team = team;
    }

    // ★ `observer` 를 켜면 **막 접속한 상태**(관전)로 만든다. 기본은 이미 놀고 있는 상태다 —
    //   시험 대부분은 싸움을 재는 것이라 관전 단계를 매번 거치게 하면 잡음만 는다.
    //   관전 풀기 자체는 `TestObserverExit` 에서 따로 잰다.
    // ★ 시험용 접속. 직업을 고를 수 있다 — **스탯이 직업으로 갈리므로**
    //   기본은 위저드로 둔다(옛 시험이 기대하던 체력 75 가 위저드 값이었다).
    void DebugEnterGame(const std::string& addr, uint16_t netCode, bool observer = false,
                        CharClass cls = CharClass::Wizard) {
        Session& s = SessionFor(addr);
        // ⚠ 진짜 접속 경로(`CLIENT_ACCEPT`)와 **같은 일을 해야** 한다 —
        //   `joinOrder` 를 여기서 안 매기면 팀이 사전순으로 갈린다(시험이 잡았다).
        if (!s.inGame) s.joinOrder = ++nextJoinOrder_;
        s.inGame = true;
        s.netCode = netCode;
        s.charClass = cls;
        s.charLevel = kMaxLevel;
        s.maxHealth = static_cast<uint16_t>(HealthOnline(cls));
        s.maxMana = static_cast<uint16_t>(ManaOnline(cls));
        s.strength = StrengthOnline(cls);
        s.carryCap = static_cast<uint16_t>(CarryCapacityOnline(cls));   // +490(묶음 5)
        s.health = s.maxHealth;
        s.mana = s.maxMana;
        // 09-14 — 「접속 + 스냅샷 꼬리까지 끝난 사람」이다: 꼬리가 dd·de·48·43 을 실었고(+2184 = 0 · +10 = 지금), sub_4EFF10 이 +6 = +4
        s.manaPrev = s.mana;
        s.healthReported = s.health;
        s.statsDirty = false;
        for (int k = 1; k < 137; ++k) s.spellLvl[k] = 3;   // 4-a — 온라인 serverSetAllSpells
        s.x = 1000;
        s.y = 1000;
        s.observer = observer;
        s.importantReady = true;
        s.importantImmediate = true;   // 시험 — Session::importantImmediate 주석
        // ★ 09-27 B1 — 진짜 접속 경로처럼 기본 장비를 가방에(틀이 없으면 안 실린다 — 틀을 안 넣는 옛 시험은 그대로)
        if (s.inventory.empty()) GiveDefaultGear(s);
    }
    // ★ 09-29 Z1-A 시험용 — 둘레 피해(끝의 벽 피해까지)를 곧바로
    void DebugDamageUnitsAround(float cx, float cy, float r1, float r2, int dmg, uint8_t dtype, std::vector<OutPacket>& out,
                                const WallHitter& wallWho = WallHitter{}) {
        DamageUnitsAround(cx, cy, r1, r2, dmg, dtype, std::string(), Target{}, out, HitterRef{}, nullptr, wallWho);
    }
    // ★ 09-29 시험용 — CLIENT_READY 처리(`nox_xxx_gameServerReadyMB_4DD180`)를 곧바로
    void DebugServerReady(const std::string& addr, std::vector<OutPacket>& out) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) GameServerReadyMB(it->second, out);
    }
    // 시험용 — 중요 메시지 경로를 진짜로(대기열·준비·확인) 돌리게 한다.
    void DebugImportantReal(const std::string& addr, bool ready) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.importantImmediate = false;
        it->second.importantReady = ready;
    }
    size_t DebugImportantPending(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.importantQ.size();
    }
    // 시험용 — 대기열에 맨 메시지(Packet0)로 실린 옵 op 가 몇 개인가(sub_4E5450 겹침 지우기를 잰다)
    size_t DebugImportantPendingOp(const std::string& addr, uint8_t op) const {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return 0;
        size_t n = 0;
        for (const auto& q : it->second.importantQ) if (!q.wire.empty() && q.wire[0] == op) ++n;
        return n;
    }
    // 시험용 — 갱신 알림 정답지화(09-14): 체력 채움(unitAdjustHP) · 마나 더하기/빼기(playerManaAdd/Sub) · +2184 세우기
    void DebugAdjustHP(const std::string& addr, int dv) { std::vector<OutPacket> o; UnitAdjustHP(Target{ addr, -1 }, dv, o); }
    void DebugManaAdd(const std::string& addr, int v) { auto it = sessions_.find(addr); if (it != sessions_.end()) PlayerManaAdd(it->second, v); }
    void DebugManaSub(const std::string& addr, int v) { auto it = sessions_.find(addr); if (it != sessions_.end()) PlayerManaSub(it->second, v); }
    void DebugSetStatsDirty(const std::string& addr) { auto it = sessions_.find(addr); if (it != sessions_.end()) it->second.statsDirty = true; }
    // 시험용 — 독 남은 양(4-c2a) · 감쇠 계수기를 줄여 「오래 지난 것」을 흉내낸다(감쇠 한 칸이 1000틱이라 그냥은 못 잰다)
    void DebugSetPoison(const std::string& addr, uint8_t v) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) i->second.poison = v;
    }
    void DebugSetHealth(const std::string& addr, uint16_t hp) {   // ★ 09-29 B3 시험용 — 여러 번 물리는 시험에서 살려 둔다
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) i->second.health = hp;
    }
    int DebugPoisonOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : static_cast<int>(it->second.poison);
    }
    void DebugSetPoisonDecay(const std::string& addr, int v) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.poisonDecay = static_cast<uint16_t>(v);
    }
    int DebugPoisonDecayOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : static_cast<int>(it->second.poisonDecay);
    }
    int DebugPetPoison(size_t i) const { return i < pets_.size() ? static_cast<int>(pets_[i].poison) : 0; }
    // 시험용 — 소환수에 독을 직접 얹는다. 커서로 소환수를 집으려면 **몸 반지름(10) 안**이라야 하는데
    //   시전 9틱 동안 소환수가 그보다 멀리 걸어가 버린다 — 커서 맞히기는 이 시험이 재려는 것이 아니다
    void DebugPoisonPet(size_t i, int amount) {
        if (i >= pets_.size()) return;
        std::vector<OutPacket> o;
        ActivatePoison(Target{ std::string(), static_cast<int>(i) }, amount, amount, o);
    }
    // 시험용 — 틱 밖에서 곧바로 되살린다(playerRespawn_4F7EF0). 「부활 직후 · 첫 갱신 전」 창을 여는 데 쓴다
    void DebugRespawn(const std::string& addr) { auto it = sessions_.find(addr); if (it == sessions_.end()) return; std::vector<OutPacket> o; PlayerRespawn(it->second, frame_, o); }
    // ★ 09-29 — 기본 장비의 수식 넷(평상복 = 캐릭터 색 · 그 밖 없음)
    static void DefaultGearMods(uint16_t type, const std::vector<uint8_t>& info97, uint8_t mods[4]);
    // ★ 09-29 시험용 — 다음 틱에 그 사람을 새로 온 사람으로 소개한다(`CLIENT_ACCEPT` 가 세우는 표시 · 시험 접속은 안 거친다)
    void DebugSetPendingIntro(const std::string& addr) { auto it = sessions_.find(addr); if (it != sessions_.end()) it->second.pendingIntro = true; }
    // ★ 09-29 시험용 — `to` 에게 `who` 의 입은 것 겉모습 전부(새로 온 사람 소개 자리의 몫)
    void DebugSendWornLooks(const std::string& to, const std::string& who, std::vector<OutPacket>& out) {
        auto t = sessions_.find(to); auto w = sessions_.find(who);
        if (t != sessions_.end() && w != sessions_.end()) SendWornLooks(t->second, w->second, out);
    }
    // ★ 09-28 B4b 시험용 — 부활시키고 나간 것을 받는다
    void DebugRespawnOut(const std::string& addr, std::vector<OutPacket>& out) { auto it = sessions_.find(addr); if (it != sessions_.end()) PlayerRespawn(it->second, frame_, out); }

    // ★ 시험용 — 첫 세계 스냅샷을 **다음 틱에** 이 사람에게 보내도록 예약한다.
    void DebugArmSnapshot(const std::string& addr) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.snapshotIn = 0;
    }

    // ★ 시험용 — 실제 자리(`posX`/`posY`)와 그것을 통째로 옮기기.
    //   `DebugMoveTo` 는 `x`/`y` 만 넣고 틱이 옮긴다 — 이쪽은 **둘 다** 넣는다
    double DebugPosX(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.posX; }
    double DebugPosY(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.posY; }
    // ★ 2-b2 시험용 — 속도(+80) · 옛 자리(+72)
    double DebugVelX(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.velX; }
    double DebugVelY(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.velY; }
    double DebugPrevX(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.prevX; }
    double DebugPrevY(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.prevY; }
    double DebugDistance(const std::string& p, const std::string& q) const {
        const double dx = DebugPosX(p) - DebugPosX(q), dy = DebugPosY(p) - DebugPosY(q);
        return std::sqrt(dx * dx + dy * dy);
    }
    void DebugSetPos(const std::string& a, double px, double py) {
        auto i = sessions_.find(a);
        if (i == sessions_.end()) return;
        i->second.posX = px; i->second.posY = py;
        i->second.x = static_cast<int>(px); i->second.y = static_cast<int>(py);
        i->second.forceX = i->second.forceY = 0.0;
    }

    // ★ 시험용 — 지금 자리. 벽 충돌 시험이 이걸로 잰다.
    int XOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.x;
    }
    int YOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.y;
    }

    // ★ 시험용 — 공격 상태 들여다보기(묶음 3-a). 상태 · 애니 번호 · 애니 칸
    uint8_t DebugStateOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0xff : it->second.moveState;
    }
    // 시험용 — 상태를 지금 프레임으로 새로 넣는다(4-a2 들어오는 Magic 보고 방패 올리기 · 가만 7틱 시계를 0 으로)
    void DebugSetState(const std::string& addr, uint8_t st) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.moveState = st;                          // 같은 상태여도 시작 프레임을 새로(PlayerSetState 는 같으면 안 건드린다)
        it->second.stateFrame = frame_;
        it->second.animFrame = 0;
    }
    // 시험용 — +520 마지막으로 때린 것의 갈래(3-d · 0 없음 · 1 사람 · 2 몬스터 · 3 무기·투사체)
    uint8_t DebugLastHitCls(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0xff : it->second.lastHit.cls;
    }
    // 때린 것의 자리(묶음 3-e2 막기 판정) — 정답지는 때린 오브젝트(투사체면 투사체)의 +72 지난 자리를 본다
    struct AttackSrc {
        float x = 0, y = 0;       // +72 PrevPos
        bool missile = false;     // 클래스 0x1 MISSILE
        uint32_t missileSub = 0;  // 투사체 SUBCLASS(0x70 이면 되받아치지 않음)
        // 3-d 죽음 알림 — a2(때린 이)가 몬스터면 그 종류 번호(+4, 사람이면 0) · a3(무기·투사체 오브젝트)가 있나와 그 종류 번호
        uint16_t byMonsterType = 0;
        int byPet = -1;           // 4-b1 — 때린 이가 소환수면 그 자리 번호(흡혈·감전 되갚기)
        // ★ 09-20 — **주인 사슬의 뿌리가 몬스터인가**(정답지 `object.go:1737`).
        //   덫 화살·환경 피해는 false — 그래서 맵 몬스터에게 적이 된다(`1740`)
        bool rootIsMonster = false;
        bool obj = false;
        uint16_t objType = 0;
    };
    // ★ 09-23 nine-defects 4-c3b1 — 맞는 소리의 **「때린 것」**(정답지 obj2 = a3 가 있으면 a3, 없으면 a2 · `GAME3_2.c:7257~7261`).
    //   `AttackSrc` 와 따로 둔다 — `AttackSrc` 는 막기 판정·마지막 때린 것(+520)에도 쓰여, 없던 자리에 새로 넘기면 그쪽 동작까지 바뀐다.
    //   부르는 자리마다 정답지 인자를 대조해 채운다(진행 파일 4-c3b 표) · 기본값 = 안 옮긴 자리(소리 없이 세기만)
    struct HitterRef {
        enum Kind : uint8_t { NotPorted = 0, None, Type };
        Kind kind = NotPorted;
        uint16_t type = 0;     // Type 이면 그 종류 번호 — 재질을 이 번호로 찾는다(사람 몸은 NewPlayer 번호)
        // ★ 4-c3b3b — 때린 이(a2)가 **몬스터**면 그 종류(아니면 0). 맞는 소리 맨 앞에서 그 몬스터 소리 모음의 맞히는 소리를 본다(`GAME3_2.c:7252~7254`)
        uint16_t monsterA2 = 0;
    };
    // ★ 시험용 — 피해 함수(3-e)를 바로 부른다. pet < 0 이면 사람(addr), 아니면 소환수 자리 번호
    bool DebugHurt(const std::string& addr, int pet, int dmg, const std::string& by, uint8_t dtype,
                   std::vector<OutPacket>& out, const AttackSrc* src = nullptr, HitterRef hitter = HitterRef{}) {
        return HurtTarget(Target{ pet < 0 ? addr : std::string(), pet }, dmg, out, by, dtype, src, hitter);
    }
    uint16_t DebugPetHealth(size_t i) const { return i < pets_.size() ? pets_[i].health : 0; }
    void DebugSetWeapon(const std::string& addr, int64_t classBits) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.debugWeapon = classBits;
    }
    bool IsInvulnerable(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.buffs.Has(23);
    }
    // 시험용 — 버프(4-a4)
    Target DebugFindObjectAtCursor(const std::string& addr) const { auto it = sessions_.find(addr); return it == sessions_.end() ? Target{} : FindObjectAtCursor(it->second); }
    void DebugAddForce(const std::string& addr, double fx, double fy) { auto it = sessions_.find(addr); if (it != sessions_.end()) { it->second.forceX += fx; it->second.forceY += fy; } }
    float DebugSpeedCur(const std::string& addr) const { auto it = sessions_.find(addr); return it == sessions_.end() ? 0.0f : it->second.speedCur; }
    uint8_t DebugFacingOf(const std::string& addr) const { auto it = sessions_.find(addr); return it == sessions_.end() ? 0 : it->second.facing; }
    bool DebugHasEnchant(const std::string& addr, int buff) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.buffs.Has(buff);
    }
    int DebugEnchantDur(const std::string& addr, int buff) const {
        auto it = sessions_.find(addr);
        return (it != sessions_.end() && buff >= 0 && buff < 32) ? it->second.buffs.dur[buff] : -1;
    }
    int DebugEnchantPower(const std::string& addr, int buff) const {
        auto it = sessions_.find(addr);
        return (it != sessions_.end() && buff >= 0 && buff < 32) ? it->second.buffs.power[buff] : -1;
    }
    void DebugApplyEnchant(const std::string& addr, int buff, int dur, int power) { ApplyEnchant(Target{ addr, -1 }, buff, dur, power); }
    void DebugBuffOff(const std::string& addr, int buff) { BuffOff(Target{ addr, -1 }, buff); }
    bool DebugPetHasEnchant(size_t i, int buff) const { return i < pets_.size() && pets_[i].buffs.Has(buff); }
    int DebugPetEnchantDur(size_t i, int buff) const {       // ★ 2-a2 시험용
        return (i < pets_.size() && buff >= 0 && buff < 32) ? pets_[i].buffs.dur[buff] : -1;
    }
    int DebugPetEnchantPower(size_t i, int buff) const {     // ★ 2-a2 시험용
        return (i < pets_.size() && buff >= 0 && buff < 32) ? pets_[i].buffs.power[buff] : -1;
    }
    uint16_t DebugPetNetCode(size_t i) const { return i < pets_.size() ? pets_[i].netCode : 0; }
    void DebugApplyEnchantPet(size_t i, int buff, int dur, int power) { ApplyEnchant(Target{ std::string(), static_cast<int>(i) }, buff, dur, power); }
    struct BuffConf {                    // spellBuffConf(spells_buffs.go:11)
        int dur = 0;                     // 고정 길이
        std::string durOpt;              // gamedata 이름(있으면 이 값)
        std::string durOptMulQuest;      // 퀘스트면 곱할 gamedata 이름
        bool durInSec = false;           // 초 단위 → ×30
        bool durLevelMul = false;        // 세기 곱
        bool once = false;               // 이미 걸려 있으면 안 건다
        bool defensive = false;          // 대상의 공격 지속 주문 취소
        std::string orig;                // 건 사람(Offensive 면 도운 사람 기록)
        bool offensive = false;
        uint8_t pointFx = 0;             // 대상 자리 점 효과 오프코드
    };
    int CastBuffSpell(uint8_t spell, int enc, int lvl, const Target& targ, const BuffConf& opts, std::vector<OutPacket>& out);   // castBuffSpell
    // gamedata.bin — main 이 읽어 넣는다(4-a4). 태그 = ARENA(협동 모드가 아니면 · serverBalance.Tag)
    void SetGamedata(const Gamedata& g) { gamedata_ = g; hasGamedata_ = true; }
    double BalanceFloat(const std::string& key) const { return gamedata_.Float("ARENA", key, 0.0); }
    double BalanceFloatInd(const std::string& key, int i) const {
        if (i < 0) return 0.0;
        const std::vector<double> arr = gamedata_.Array("ARENA", key);
        return (static_cast<size_t>(i) < arr.size()) ? arr[static_cast<size_t>(i)] : 0.0;
    }
    bool HasGamedata() const { return hasGamedata_; }
    uint8_t DebugStaminaOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.stamina;
    }
    uint8_t DebugAnimByteOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : PlayerAnimByte(it->second);
    }
    uint8_t DebugAttackAnimOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.attackAnim;
    }
    bool DebugPetXY(size_t i, double& x, double& y) const {
        if (i >= pets_.size()) return false;
        x = pets_[i].x; y = pets_[i].y;
        return true;
    }
    // ★ 시험용 — 소환수를 자리에 세운다. 소환수는 시전 9틱 사이에 걸어 나가 자리 시험이 흔들린다.
    bool DebugMovePet(size_t i, double x, double y) {
        if (i >= pets_.size()) return false;
        pets_[i].x = x; pets_[i].y = y;
        return true;
    }
    uint8_t DebugAnimFrameOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.animFrame;
    }
    // ★ 시험용 — 자리를 옮긴다. 사거리 시험에 쓴다.
    void DebugMoveTo(const std::string& addr, int x, int y) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.x = x;
        it->second.y = y;
    }

    // ★ 바라보는 쪽을 못 박는다 — **갱신 항목이 방향을 싣는지 재는 시험용**이다.
    //   진짜 값은 클라가 `PLAYER_INPUT` 코드 1 로 보낸다(`Server.cpp:1592`).
    //   ⚠ 이것 없이는 시험에서 방향을 못 바꾼다 — 2026-09-12 배선 때 돌연변이가
    //     안 잡혀(시험 5개가 이 자리를 안 봤다) 시험을 새로 쓰면서 같이 넣었다.
    // 시험용 — 든 물건의 충전을 직접 정한다(5-e·5-f).
    void DebugSetCharges(const std::string& addr, uint16_t netCode, uint8_t v) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        for (auto& c : it->second.inventory)
            if (c.netCode == netCode) { c.charges = v; if (c.chargesMax < v) c.chargesMax = v; return; }
    }
    void DebugSetFacing(const std::string& addr, uint8_t facing) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.facing = facing;
        it->second.facingPrev = facing;   // 3-a — 공격 방향은 지난 틱 끝 값(Direction1)을 본다. 「이미 그쪽을 보고 있었다」로 둔다
    }

    // ★ 피해를 준다. 체력이 0 이하가 되면 죽음을 알리고 부활을 예약한다.
    //
    // 나가는 것은 캡처에서 본 그대로다 —
    //   0x42 REPORT_HEALTH_DELTA  겉에 그냥      (실측 10건 전수가 비신뢰 패킷)
    //   0xE8 PLAYER_DIED          0xcc 껍데기 안  (실측 2건 전수)
    //
    // 돌려주는 값 = 이 피해로 죽었나.
    // `by` 는 때린 쪽의 주소다. 죽으면 그쪽 점수가 오른다.
    // 불·독처럼 임자가 없는 피해면 빈 문자열을 넣는다.
    // `hit` = 마지막으로 때린 것(+520, 3-d). 비었으면 `by` 로 채운다 — 사람이 맨손으로 때린 것(시험·자기 주문 자리)
    bool Damage(const std::string& addr, int amount, std::vector<OutPacket>& out,
                const std::string& by = std::string(), const Session::LastHit* hit = nullptr, bool ignoreInvuln = false);

    // 점수 = 정답지 +2136(`Lessons`). 자살·팀킬이면 **내려간다**
    int32_t KillsOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.score;
    }
    uint32_t DeathsOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0u : it->second.deaths;
    }
    // 판이 끝났나 (점수 한도에 닿았나).
    bool RoundOver() const { return roundOver_; }

    // 지금 살아 있나. 없는 상대면 거짓.
    bool IsAlive(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.inGame && it->second.respawnAtFrame == 0;
    }
    uint16_t HealthOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.health;
    }
    // 묶음 5-c — 그 직업이 이 먹을거리를 쓸 수 있나(`sub_57B370` 의 먹을거리 갈래).
    //   셈만 하는 순수 함수라 시험이 셈 자체를 들여다본다.
    static bool ClassCanUseFood(CharClass cls, uint32_t foodBits);
    // 시험용 — 금을 뺀다(`playerSubGold_4FA5D0`). 가게가 생기면 그쪽이 이 길을 쓴다.
    void DebugSubGold(const std::string& addr, uint32_t v, std::vector<OutPacket>& out) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        PlayerSubGold(it->second, v, out);
    }
    // 시험용 — 소환수 AI 쌓임 들여다보기·건드리기(6-a)
    // 묶음 6-b — 그 갈래가 **조건**인가(40~71). 셈만 하는 순수 함수라 시험이 표 자체를 본다.
    static bool AIIsCondition(uint8_t act);
    int DebugAIStackInd(size_t i) const { return i < pets_.size() ? pets_[i].aiStackInd : -1; }
    int DebugAIAction(size_t i, int at) const {
        if (i >= pets_.size() || at < 0 || at >= Creature::kAIStackMax) return -1;
        return pets_[i].aiStack[at].action;
    }
    bool DebugAIStarted(size_t i, int at) const {
        return i < pets_.size() && at >= 0 && at < Creature::kAIStackMax && pets_[i].aiStack[at].started;
    }
    bool DebugAIPush(size_t i, uint8_t act) { return i < pets_.size() && AIPush(pets_[i], act); }
    // 시험용 — 조건을 인자와 함께 올린다(6-b)
    bool DebugAIPushDep(size_t i, uint8_t act, const std::string& addr, int pet, uint32_t u, float f,
                        float x = 0, float y = 0) {
        if (i >= pets_.size()) return false;
        if (!AIPush(pets_[i], act, Target{ addr, pet }, x, y)) return false;
        pets_[i].aiStack[pets_[i].aiStackInd].argU32 = u;
        pets_[i].aiStack[pets_[i].aiStackInd].argF = f;
        return true;
    }
    void DebugAIProcDeps(size_t i, std::vector<OutPacket>& out) { if (i < pets_.size()) { aiOut_ = &out; AIProcDeps(pets_[i], out); aiOut_ = nullptr; } }
    void DebugAISetPrevPos(size_t i) { if (i < pets_.size()) { pets_[i].prevX = pets_[i].x; pets_[i].prevY = pets_[i].y; } }
    // 시험용 — 변신 「됐다」 표시·변신 상태(6-c)
    void DebugAISetMorphReady(size_t i, bool v) { if (i < pets_.size()) pets_[i].aiMorphReady = v; }
    bool DebugAIMorphed(size_t i) const { return i < pets_.size() && pets_[i].morphed; }
    uint32_t DebugAIActionFrame(size_t i) const { return i < pets_.size() ? pets_[i].aiActionFrame : 0; }
    // ── 묶음 6-e(1) 길 찾기 격자 색인 ──
    //   칸마다 깃발과 **세대**를 둔다. 색인을 다시 뜰 때 세대를 올려 묵은 자국을 가른다.
    struct AIIndexNode { uint32_t gen = 0; uint16_t flags = 0; uint32_t visit = 0; };   // visit = 정답지 `Index0`
    static constexpr int kAIIndexSize = 256;   // 벽 격자와 같다(`WallGridSize`)
    // ── 묶음 6-e(2) 길 훑기 ──
    //   `AIVisitNode` — 물결 사슬(`next`=Field8)과 온 길(`parent`=Field4)
    struct AIVisitNode { int16_t x = 0, y = 0; int parent = -1; int next = -1; uint8_t flags = 0; };
    static constexpr size_t kAIVisitMax = 1024;   // alloc.NewClassT("VisitNodes", …, 1024)
    static constexpr size_t kAIPointMax = 1024;   // alloc.Make([]types.Pointf{}, 1024)
    struct PathActor { bool airborne = false; uint32_t subclass = 0; int obstacleSkip = -1; };
    using PathFilter = std::function<bool(int, int)>;
    // 시험용 — 소환수의 보는 쪽·돌아가려는 쪽(6-d)
    int DebugPetFacing(size_t i) const { return i < pets_.size() ? pets_[i].facing : -1; }
    int DebugPetFacingWant(size_t i) const { return i < pets_.size() ? pets_[i].facingWant : -1; }
    void DebugSetPetFacing(size_t i, uint8_t f) { if (i < pets_.size()) { pets_[i].facing = f; pets_[i].facingWant = f; } }
    uint32_t DebugAIArgU32(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].argU32 : 0;
    }
    // 09-21 — **그 사람의 시계**를 시험이 직접 세운다. 전역 `frame_` 과 **다르게** 둬야
    //   「도장을 누구 시계로 찍나」가 갈린다(같으면 어느 쪽을 찍어도 시험이 통과한다)
    void DebugSetTickFrame(const std::string& addr, uint32_t v) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) { it->second.tickFrame = v; it->second.tickFrameSet = true; }
    }
    void DebugSendTimer(std::vector<OutPacket>& out) { SendTimer(out); }
    // 09-21 — 판 끝 상태를 시험이 직접 뒤집는다(시계가 **바뀔 때만** 나가는지 보려고)
    void DebugSetRoundOver(bool v) { roundOver_ = v; }
    // 09-21 — 그 사람이 믿고 있는 시계(와이어 도장과 견주려고)
    uint32_t DebugTickFrame(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : ClockOf(it->second);
    }
    // 묶음 6-j(2) — 떠돌기가 **지금 고른 길목 번호**. 기억이 서는지 시험이 직접 본다
    uint32_t DebugAIArgWp(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].argWp : 0;
    }
    int  DebugAIPop(size_t i) { return i < pets_.size() ? AIPop(pets_[i]) : -1; }
    void DebugAITick(size_t i, std::vector<OutPacket>& out) { if (i < pets_.size()) { aiOut_ = &out; AITick(pets_[i], out); aiOut_ = nullptr; } }
    // 시험용 — 길 찾기 격자 색인(6-e(1))
    void DebugPathReset() { AIPathResetIndex(); }
    // 시험용 — 길 따라 걷기(6-f(1))
    void DebugSetPetPath(size_t i, const std::vector<std::pair<float, float>>& pts) {
        if (i < pets_.size()) { pets_[i].path = pts; pets_[i].pathInd = 0; }
    }
    void DebugSetPetPathFromFind(size_t i) {
        if (i < pets_.size()) { pets_[i].path = aiPathPts_; pets_[i].pathInd = 0; }
    }
    void DebugSetPetPos(size_t i, double x, double y) {
        if (i < pets_.size()) { pets_[i].x = x; pets_[i].y = y; }
    }
    void DebugSetPetSpeed(size_t i, float v) { if (i < pets_.size()) pets_[i].speedCur = v; }
    bool DebugPetRunning(size_t i) const { return i < pets_.size() && pets_[i].running; }
    float DebugPetAggression(size_t i) const { return i < pets_.size() ? pets_[i].aggression : 0.0f; }
    float DebugPetSight(size_t i) const { return i < pets_.size() ? pets_[i].sightRange : 0.0f; }
    bool DebugPetOrdered(size_t i) const { return i < pets_.size() && pets_[i].ordered; }
    bool DebugPetDeadDone(size_t i) const { return i < pets_.size() && pets_[i].deadDone; }
    void DebugSetPetAggression(size_t i, float v) { if (i < pets_.size()) pets_[i].aggression = v; }
    void DebugSetPetRunning(size_t i, bool v) { if (i < pets_.size()) pets_[i].running = v; }
    bool DebugCreatureMove(size_t i) { return i < pets_.size() ? CreatureActuallyMove(pets_[i]) : false; }
    void DebugAIRandomWalk(size_t i) { if (i < pets_.size()) AIRandomWalk(pets_[i]); }
    // 시험용 — 근접 공격(6-g(2))
    void DebugAIStart(size_t i, uint8_t act, std::vector<OutPacket>& out) { if (i < pets_.size()) { aiOut_ = &out; AIStart(pets_[i], act, out); aiOut_ = nullptr; } }
    void DebugMonsterAnimUpdate(size_t i) { if (i < pets_.size()) MonsterAnimUpdate(pets_[i]); }   // ★ 09-30 — 틱 차례의 「애니 넘기기」만(행동 앞)
    void DebugSetPetNextHit(size_t i, uint32_t f) { if (i < pets_.size()) pets_[i].nextHitFrame = f; }
    uint32_t DebugPetNextHit(size_t i) const { return i < pets_.size() ? pets_[i].nextHitFrame : 0; }
    uint32_t DebugPetAttackStart(size_t i) const { return i < pets_.size() ? pets_[i].attackStartFrame : 0; }
    float DebugAIArgY(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].argY : 0.0f;
    }
    float DebugAIArgX(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].argX : 0.0f;
    }
    float DebugAIArgF(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].argF : 0.0f;
    }
    std::string DebugAITargetAddr(size_t i, int j) const {
        return (i < pets_.size() && j >= 0 && j < Creature::kAIStackMax) ? pets_[i].aiStack[j].targetAddr : std::string();
    }
    // 시험용 — 틱이 **스스로 싸움(15)을 쌓는 것**을 끈다.
    //   행동 하나만 떼어 보는 시험은 쌓임을 손으로 세우므로 이것이 필요하다
    //   끌 때는 **이미 굴러가던 쌓임도 비운다** — 안 그러면 앞 틱에 쌓인 사슬이 계속 돈다
    void DebugSetAutoFight(bool v) {
        debugNoAutoFight_ = !v;
        if (!v) for (auto& k : pets_) {
            for (int i = 0; i < Creature::kAIStackMax; ++i) k.aiStack[i] = Creature::AIStackItem{};
            k.aiStackInd = 0;
            k.path.clear();
            k.velX = 0; k.velY = 0;
            k.enemyAddr.clear(); k.enemyPet = -1;
        }
    }
    std::string DebugPetEnemy(size_t i) const { return i < pets_.size() ? pets_[i].enemyAddr : std::string(); }
    void DebugSetPetEnemy(size_t i, const std::string& addr, int pet) {
        if (i < pets_.size()) {
            Creature& k = pets_[i];
            k.enemyAddr = addr; k.enemyPet = pet;
            k.enemyNet = (addr.empty() && pet >= 0 && static_cast<size_t>(pet) < pets_.size()) ? pets_[static_cast<size_t>(pet)].netCode : 0;
            // ★ 09-29 B1·B2 — 정답지 지금 상대는 늘 보이는 목록에서 나온다 — 목록에도 넣어 알아보기가 다시 고를 때 남게 한다
            Creature::SeenRef r; r.addr = addr; r.petNet = k.enemyNet;
            bool have = false;
            for (const auto& s : k.seen) if ((!addr.empty() && s.addr == addr) || (addr.empty() && s.petNet == r.petNet)) have = true;
            if (!have && (!addr.empty() || r.petNet != 0)) k.seen.push_back(r);
        }
    }
    bool DebugCreatureTickMove(size_t i, double tx, double ty) {
        return i < pets_.size() ? CreatureTickMove(pets_[i], tx, ty) : false;
    }
    int  DebugCreatureSetMovePath(size_t i, float tx, float ty) {
        return i < pets_.size() ? CreatureSetMovePath(pets_[i], tx, ty) : -1;
    }
    void DebugCreatureDetailedPath(size_t i, float tx, float ty) {
        if (i < pets_.size()) CreatureSetDetailedPath(pets_[i], tx, ty);
    }
    int  DebugPetPathStatus(size_t i) const { return i < pets_.size() ? pets_[i].pathStatus : -1; }
    uint32_t DebugPetPathFrame(size_t i) const { return i < pets_.size() ? pets_[i].pathFrame : 0; }
    void DebugSetPetPathFrame(size_t i, uint32_t f) { if (i < pets_.size()) pets_[i].pathFrame = f; }
    float DebugPetPathGoalX(size_t i) const { return i < pets_.size() ? pets_[i].pathGoalX : 0.0f; }
    float DebugPetPathPtX(size_t i, size_t j) const {
        return (i < pets_.size() && j < pets_[i].path.size()) ? pets_[i].path[j].first : 0.0f;
    }
    float DebugPetPathPtY(size_t i, size_t j) const {
        return (i < pets_.size() && j < pets_[i].path.size()) ? pets_[i].path[j].second : 0.0f;
    }
    int  DebugPetPathInd(size_t i) const { return i < pets_.size() ? pets_[i].pathInd : -1; }
    size_t DebugPetPathCount(size_t i) const { return i < pets_.size() ? pets_[i].path.size() : 0; }
    double DebugPetVelX(size_t i) const { return i < pets_.size() ? pets_[i].velX : 0.0; }
    double DebugPetVelY(size_t i) const { return i < pets_.size() ? pets_[i].velY : 0.0; }
    void DebugPathBakeWalls() { AIPathBakeWalls(); }
    void DebugPathIndexObjects() { AIPathIndexObjects(); }
    void DebugPathMaybeIndex() { AIPathMaybeIndexObjects(); }
    void DebugPathInvalidate(bool alsoFrame) { AIPathInvalidate(alsoFrame); }
    bool DebugPathCalculated() const { return aiIndexCalc_; }
    void DebugSetFrame(uint32_t f) { frame_ = f; }
    uint32_t DebugFrame() const { return frame_; }
    uint16_t DebugPathFlags(int x, int y) { return AIPathFlagsAt(x, y); }
    void DebugPathSetFlags(int x, int y, uint16_t f) { if (AIIndexNode* n = AIPathIndexAt(x, y)) n->flags |= f; }
    void DebugPathStamp(int x, int y) { if (AIIndexNode* n = AIPathIndexAt(x, y)) n->gen = aiIndexGen_; }
    uint32_t DebugPathGen() const { return aiIndexGen_; }
    uint32_t DebugPathFrame() const { return aiIndexFrame_; }
    bool DebugPathBlockedFlags(bool air, int x, int y) { return AIPathBlockedFlags(air, x, y); }
    bool DebugPathBlockedByObj(bool air, uint32_t sub, int x, int y) { return AIPathBlockedByObj(air, sub, x, y); }
    bool DebugPathCanPass(bool air, uint32_t sub, int x, int y) { return AIPathCanPass(air, sub, x, y); }
    int  DebugWallDirAt(int x, int y) const { return WallDirAt(x, y); }
    // ★ 09-22 묶음 9 — 문 갈래를 시험에서 가르는 손잡이 둘.
    //   `DebugWallShapeCollide` 는 **밀어내기 쪽 조회**(0x40 만)다 — 문이면 −1 이 나와야 한다
    int  DebugWallShapeCollide(int x, int y) const { return WallShapeAt(x, y, 0x40); }
    // ★ 09-22 — 문의 미세각·거친각을 시험에서 읽는다(도는 모양을 수열로 보려고)
    int DebugPropFine(uint16_t netCode) const {
        for (const auto& q : props_) if (q.netCode == netCode) return q.fine;
        return -1;
    }
    // ★ 09-22 — 마나석 잔량을 시험에서 읽는다(0~50). 없으면 −1
    int DebugPropCharge(uint16_t netCode) const {
        for (const auto& p : props_) if (p.netCode == netCode) return p.charge;
        return -1;
    }
    void DebugSetPropOpen(uint16_t netCode, bool open) {
        for (auto& p : props_) if (p.netCode == netCode) p.open = open;
    }
    bool DebugDiagOk(int ii, int x, int y) const { return AIPathDiagOk(ii, x, y); }
    int DebugPathFind(bool awayFrom, bool air, uint32_t sub, float fx, float fy, float tx, float ty, int maxDepth) {
        return AIPathFind(awayFrom, PathActor{ air, sub, -1 }, fx, fy, tx, ty, PathFilter(), maxDepth);
    }
    int DebugPathFindF(bool awayFrom, bool air, uint32_t sub, float fx, float fy, float tx, float ty, int maxDepth,
                       const PathFilter& f) {
        return AIPathFind(awayFrom, PathActor{ air, sub, -1 }, fx, fy, tx, ty, f, maxDepth);
    }
    int DebugPathStatus() const { return aiPathStatus_; }
    size_t DebugPathLen() const { return aiPathCells_.size(); }
    int DebugPathCellX(size_t i) const { return i < aiPathCells_.size() ? aiPathCells_[i].first : -1; }
    int DebugPathCellY(size_t i) const { return i < aiPathCells_.size() ? aiPathCells_[i].second : -1; }
    size_t DebugPathPtCount() const { return aiPathPts_.size(); }
    float DebugPathPtX(size_t i) const { return i < aiPathPts_.size() ? aiPathPts_[i].first : -1.0f; }
    float DebugPathPtY(size_t i) const { return i < aiPathPts_.size() ? aiPathPts_[i].second : -1.0f; }
    size_t DebugPathVisitCount() const { return aiVisit_.size(); }
    uint32_t DebugPathLast() const { return aiIndexLast_; }
    // 시험용 — 경험치·레벨
    float DebugExp(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0f : i->second.experience; }
    int DebugLevel(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? -1 : i->second.level; }
    void DebugSendNotice(const char* text, std::vector<OutPacket>& out) { SendNotice(text, out); }   // ★ 09-30 묶음 넘침 시험용
    void DebugGiveExp(const std::string& a, float v, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) PlayerGiveExp(i->second, v, out);
    }
    // 시험용 — 죽음 셈의 갈래를 **곧바로** 밟는다.
    //   같은 편 때리기는 `OwnersEnemy` 가 피해 자리에서 **미리 막아** 팀킬 갈래까지 못 간다.
    //   도운 이도 `MarkAssist`(주문 자리)가 세우는 것이라 맨 피해로는 안 선다.
    //   그 둘을 재려면 셈만 따로 불러야 한다
    void DebugUpdateScore(const std::string& victim, const std::string& killer,
                          const std::string& assist, std::vector<OutPacket>& out) {
        auto v = sessions_.find(victim);
        if (v == sessions_.end()) return;
        v->second.lastHit = Session::LastHit{};
        if (!killer.empty()) { v->second.lastHit.cls = 1; v->second.lastHit.player = killer; }
        if (!assist.empty()) MarkAssist(assist, Target{ victim, -1 });
        PlayerUpdateScore(v->second, out);
    }
    // 묶음 8-a — 판 설정(접속 묶음의 `af`·`b0` 가 이것을 싫는다)
    const GameSettings& Settings() const { return gameSettings_; }
    void SetGameSettings(const GameSettings& g) { gameSettings_ = g; }
    void SetServerName(const std::string& n) { gameSettings_.name = n; }
    void SetGameFlags(uint32_t f) { gameSettings_.gameFlags = f; }
    void SetMaxPlayers(uint8_t n) { gameSettings_.maxPlayers = n; }

    size_t PlayerCount() const { size_t n = 0; for (const auto& kv : sessions_) if (kv.second.inGame) ++n; return n; }
    bool IsInGame(const std::string& x) const { auto i = sessions_.find(x); return i != sessions_.end() && i->second.inGame; }
    void DebugKick(const std::string& x, std::vector<OutPacket>& out) { KickPlayer(x, out); }

    // 시험용 — 판 끝나는 나머지 길
    void DebugRoundTimeUp(std::vector<OutPacket>& out) { RoundTimeUp(out); }
    void DebugCheckVictory(std::vector<OutPacket>& out) { CheckVictorySurvivors(out); }
    void DebugRoundCheckLimit(std::vector<OutPacket>& out) { RoundCheckLimit(out); }

    // 묶음 9-a — 비밀벽 여닫기. 정답지는 **노스스크립트**(`WallOpen`·`WallClose`·`WallToggle`)가
    //   부르는 자리다 — 우리 서버엔 스크립트가 없어 같은 몫을 여기에 낸다
    void WallOpen(int gx, int gy) { if (SecretWall* w = SecretWallAt(gx, gy)) SecretWallSetState(*w, 4); }
    void WallClose(int gx, int gy) { if (SecretWall* w = SecretWallAt(gx, gy)) SecretWallSetState(*w, 2); }
    void WallToggle(int gx, int gy) {
        SecretWall* w = SecretWallAt(gx, gy);
        if (!w) return;
        // `Wall.toggle` — **닫힌 것(1·2)이면 열고** 아니면 닫는다
        SecretWallSetState(*w, (w->state == 1 || w->state == 2) ? 4 : 2);
    }
    // 시험용 — 비밀벽
    size_t SecretWallCount() const { return secretWalls_.size(); }
    int SecretWallState(int gx, int gy) {
        const SecretWall* w = SecretWallAt(gx, gy);
        return w ? static_cast<int>(w->state) : -1;
    }
    int SecretWallAnim(int gx, int gy) {
        const SecretWall* w = SecretWallAt(gx, gy);
        return w ? static_cast<int>(w->anim) : -1;
    }
    // 시험용 — 쏘는 덫
    void DebugShootingTrapUpdate(std::vector<OutPacket>& out) { ShootingTrapUpdate(out); }
    bool DebugTrapArmed(size_t i) const { return i < traps_.size() && traps_[i].armed; }
    uint32_t DebugTrapFireIn(size_t i) const { return i < traps_.size() ? traps_[i].fireCountdown : 0u; }
    void DebugTrapEnable(size_t i, bool on) { if (i < traps_.size()) traps_[i].enabled = on; }
    // 조각 L — 알림 갈래를 손으로 걸어 본다(시험 11-j). 「안 옮긴 갈래를 걸면 조용한가」용.
    void DebugMarkDirty(uint16_t netCode, PendingReport::What what) { MarkDirty(netCode, what); }
    uint8_t DebugMissileCollideAmount(size_t i) const { return i < missiles_.size() ? missiles_[i].collideAmount : 0; }
    uint8_t DebugMissileDir(size_t i) const { return i < missiles_.size() ? missiles_[i].dir1 : 0; }
    float DebugMissileY(size_t i) const { return i < missiles_.size() ? missiles_[i].posY : 0.0f; }
    void DebugSecretWallUpdate(std::vector<OutPacket>& out) { SecretWallUpdate(out); }
    void DebugSecretWallSync(const std::string& addr, std::vector<OutPacket>& out) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) SecretWallSync(i->second, out);
    }
    void DebugSetDeaths(const std::string& x, uint32_t v) {
        auto i = sessions_.find(x); if (i != sessions_.end()) i->second.deaths = v;
    }
    // 시험용 — 모드별 죽음 셈
    bool DebugIsCrown(const std::string& x) const {
        auto i = sessions_.find(x); return i != sessions_.end() && IsCrown(i->second);
    }
    // 10-f — 시험용 완드. 충전이 0 이면 쓰려 할 때 222 가 울려야 한다
    void DebugGiveWand(const std::string& x, uint8_t spell, uint8_t charges,
                       uint8_t chargesMax, uint32_t cooldown) {
        auto i = sessions_.find(x);
        if (i == sessions_.end()) return;
        Session::Carried c;
        c.netCode = 60001;
        c.wandSpell = spell;
        c.charges = charges;
        c.chargesMax = chargesMax;
        c.wandCooldown = cooldown;
        i->second.inventory.push_back(c);
    }
    bool DebugUseWand(const std::string& x, std::vector<OutPacket>& out) {
        auto i = sessions_.find(x);
        if (i == sessions_.end()) return false;
        for (auto& c : i->second.inventory)
            if (c.wandSpell != 0) return UseWandCast(i->second, c, out);
        return false;
    }
    void DebugGiveCrown(const std::string& x) {
        auto i = sessions_.find(x);
        if (i == sessions_.end()) return;
        // ⚠ 상수가 아니라 **날숫자 242** 를 넣는다 —
        //   상수를 쓰면 상수를 바꾸는 돌연변이가 **양쪽을 같이 바꿔** 안 걸린다
        Session::Carried c; c.netCode = 60000; c.type = 242;
        i->second.inventory.push_back(c);
    }
    void DebugDeathScoring(const std::string& victim, const std::string& killer,
                           std::vector<OutPacket>& out) {
        auto v = sessions_.find(victim);
        if (v == sessions_.end()) return;
        v->second.lastHit = Session::LastHit{};
        if (!killer.empty()) { v->second.lastHit.cls = 1; v->second.lastHit.player = killer; }
        DeathScoring(v->second, out);
    }
    // 시험용 — 투표
    size_t DebugVoteCount() const { return votes_.size(); }
    size_t DebugVoteVoters(size_t i) const { return i < votes_.size() ? votes_[i].voters.size() : 0; }
    void DebugVoteCast(const std::string& who, uint8_t kind, const std::string& targetName,
                       std::vector<OutPacket>& out) {
        auto i = sessions_.find(who); if (i != sessions_.end()) VoteCast(i->second, kind, targetName, out);
    }
    void DebugVoteWithdraw(const std::string& who, uint8_t kind, const std::string& targetName,
                           std::vector<OutPacket>& out) {
        auto i = sessions_.find(who); if (i != sessions_.end()) VoteWithdraw(i->second, kind, targetName, out);
    }
    void DebugVoteUpdate(std::vector<OutPacket>& out) { VoteUpdate(out); }
    void DebugRemoveVote(size_t i, std::vector<OutPacket>& out) { RemoveVote(i, out); }
    void DebugSetName(const std::string& who, const std::string& n) {
        auto i = sessions_.find(who); if (i != sessions_.end()) i->second.name = n;
    }
    // 시험용 — 플래그볼
    void DebugUpdateBall(std::vector<OutPacket>& out) { UpdateBall(out); }
    uint8_t DebugTeamAfter(uint8_t id) const { return TeamAfter(id); }

    // 시험용 — 판 끝
    void DebugSetScore(const std::string& x, int32_t v) { auto i = sessions_.find(x); if (i != sessions_.end()) i->second.score = v; }
    void DebugRoundCheck(std::vector<OutPacket>& out) { RoundCheckLimit(out); }

    // 시험용 — 팀
    void DebugSendTeamState(std::vector<OutPacket>& out) { SendTeamState(out); }
    int DebugTeamScore(uint8_t id) { const Team* t = TeamById(id); return t ? t->score : -999; }
    void DebugTeamChangeScore(uint8_t id, int32_t v, std::vector<OutPacket>& out) { TeamChangeScore(id, v, out); }
    void DebugSendTeamRemove(uint8_t id, std::vector<OutPacket>& out) { SendTeamRemove(id, out); }
    size_t DebugTeamCount() const { return teams_.size(); }
    // 시험용 — 안내서
    int DebugGuideLvl(const std::string& a, uint8_t g) const {
        auto i = sessions_.find(a); return i == sessions_.end() || g >= 41 ? -1 : i->second.guideLvl[g];
    }
    bool DebugGuideAward(const std::string& a, uint8_t g, bool announce, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); return i != sessions_.end() && GuideAward(i->second, g, announce, out);
    }
    // 시험용 — 작살
    uint16_t DebugHarpoonBolt(const std::string& a) const {
        auto i = sessions_.find(a); return i == sessions_.end() ? 0 : i->second.harpoonBoltNc;
    }
    std::string DebugHarpoonTarget(const std::string& a) const {
        auto i = sessions_.find(a); return i == sessions_.end() ? std::string() : i->second.harpoonTarget.addr;
    }
    // 작살 갱신만 따로 굴린다 — 움직임을 안 섞고 관문만 재려고
    void DebugHarpoonUpdate(const std::string& a, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) HarpoonUpdate(i->second, out);
    }
    void DebugAdvanceFrame(uint32_t n) { frame_ += n; }
    void DebugHarpoonBreak(const std::string& a, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) HarpoonBreak(i->second, false, HarpoonEnd::Debug, out);
    }
    // 시험용 — 능력
    int DebugAbilLvl(const std::string& a, uint8_t k) const {
        auto i = sessions_.find(a); return i == sessions_.end() || k >= 137 ? -1 : i->second.spellLvl[k];
    }
    uint32_t DebugAbilCooldown(const std::string& a, uint8_t k) const {
        auto i = sessions_.find(a); return i == sessions_.end() || k >= kAbilityCount ? 0u : i->second.abilCooldown[k];
    }
    bool DebugAbilActive(const std::string& a, uint8_t k) const {
        auto i = sessions_.find(a); return i != sessions_.end() && k < kAbilityCount && i->second.abilActive[k];
    }
    bool DebugAbilAward(const std::string& a, uint8_t k, bool announce, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); return i != sessions_.end() && AbilityAward(i->second, k, announce, out);
    }
    bool DebugAbilDo(const std::string& a, uint8_t k, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); return i != sessions_.end() && AbilityDo(i->second, k, out);
    }
    void DebugAbilTick(const std::string& a, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) AbilityUpdate(i->second, out);
    }
    void DebugAbilReset(const std::string& a, uint8_t k, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) AbilityReset(i->second, k, out);
    }
    void DebugAbilCancelAll(const std::string& a, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); if (i != sessions_.end()) AbilityCancelAll(i->second, out);
    }
    void DebugSetOverweight(const std::string& a, bool v) {
        auto i = sessions_.find(a); if (i != sessions_.end()) i->second.overweight = v;
    }
    int DebugScore(const std::string& a) const {
        auto i = sessions_.find(a); return i == sessions_.end() ? 0 : i->second.score;
    }
    int DebugSpellLvl(const std::string& a, uint8_t sp) const {
        auto i = sessions_.find(a); return i == sessions_.end() || sp >= 137 ? -1 : i->second.spellLvl[sp];
    }
    void DebugSetSpellLvl(const std::string& a, uint8_t sp, uint8_t v) {
        auto i = sessions_.find(a); if (i != sessions_.end() && sp < 137) i->second.spellLvl[sp] = v;
    }
    bool DebugGrantSpell(const std::string& a, uint8_t sp, uint8_t announce, bool extra,
                         uint8_t forceLevel, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a);
        return i != sessions_.end() && SpellGrantToPlayer(i->second, sp, announce, extra, forceLevel, out);
    }
    bool DebugStatsDirty(const std::string& a) const {
        auto i = sessions_.find(a); return i != sessions_.end() && i->second.statsDirty;
    }
    void DebugClearStatsDirty(const std::string& a) {
        auto i = sessions_.find(a); if (i != sessions_.end()) i->second.statsDirty = false;
    }
    uint32_t GoldOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.gold;
    }
    uint16_t MaxHealthOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.maxHealth;
    }
    // 시험용 — 사람에게 독을 직접 얹는다(소환수용 `DebugPoisonPet` 의 짝).
    void DebugPoisonPlayer(const std::string& addr, int amount) {
        std::vector<OutPacket> o;
        ActivatePoison(Target{ addr, -1 }, amount, amount, o);
    }
    uint16_t ManaOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.mana;
    }
    // 시험용 — 마나를 가득 채운다.
    //   ⚠ 소환(75~114)은 **주문 정의의 마나를 안 쓴다** — blob_587000 217668 표(임프 30)다.
    //     그래서 `RegSpell(…, 0)` 으로도 공짜가 안 되고, 여러 번 부르면 마나가 말라
    //     「한도에 걸린 것」과 「마나가 없는 것」이 구별이 안 된다.
    void DebugFillMana(const std::string& addr) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.mana = it->second.maxMana;
    }
    size_t CarriedCount(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.inventory.size();
    }
    bool Carries(const std::string& addr, uint16_t netCode) const {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return false;
        for (const auto& c : it->second.inventory)
            if (c.netCode == netCode) return true;
        return false;
    }
    // 시험용 — 가방 통째(09-27 B1 기본 장비 번호·순서 대조)
    std::vector<Session::Carried> DebugInventory(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? std::vector<Session::Carried>{} : it->second.inventory;
    }

    // ── 주문 마나 비용 ─────────────────────────────────
    //
    // 주문 번호 → 마나. `thing.bin` 의 SPEL 섹션에서 오고 `main.cpp` 가 채운다
    // (`ThingDb::ManaByName(SpellName(번호))`). 값의 근거는 `ThingDb.h SpellRaw` 주석.
    //
    // ⚠⚠ **표가 비면 모든 주문이 공짜가 된다.** 그건 「비용이 0」이 아니라
    //   「표를 못 읽었다」는 뜻이다. 그래서 개수를 밖에서 볼 수 있게 둔다 —
    //   기동 로그가 0 을 찍으면 파일을 못 읽은 것이다.
    // ★ 묶음 4-a — 주문 정의(thing.bin SPEL 한 칸). 음소 나무도 여기서 자란다(server/spells.go createSpellFrom).
    //   ⚠ SetSpellCost 만 부른 주문은 정의가 없어 **시전이 안 된다**(정답지도 DefByInd nil → 코드 10).
    void SetSpellDef(uint8_t spell, uint8_t mana, uint32_t flags, const std::vector<uint8_t>& phonemes,
                     uint16_t castSound = 0, uint16_t onSound = 0, uint16_t offSound = 0);
    // ★ 묶음 10-f — 지속 주문의 소리. 정답지 `spells_dur.go:131~138`·`:39~43` —
    //   **걸 때**는 겨눈 주문(TARGETED 0x4)이면 `켜짐`, 아니면 `시전` 소리를 **대상 자리**에서,
    //   **끝날 때**는 `꺼짐` 소리를 **시전한 사람 자리**에서 울린다
    uint16_t SpellSoundOn(uint8_t spell) const {
        if (spell >= 137 || !spellDefs_[spell].valid) return 0;
        const SpellDef& d = spellDefs_[spell];
        return (d.flags & 0x4) ? d.onSound : d.castSound;   // things.SpellTargeted
    }
    uint16_t SpellSoundOff(uint8_t spell) const {
        return (spell < 137 && spellDefs_[spell].valid) ? spellDefs_[spell].offSound : 0;
    }
    uint32_t SpellFlags(uint8_t spell) const { return spell < 137 && spellDefs_[spell].valid ? spellDefs_[spell].flags : 0u; }
    size_t CastPending() const { return castList_.size(); }
    // ★ 4-a2 — 겨눈 주문이 날리는 "Magic" 오브젝트(thing.bin). speedRaw = SPEED 원값(정답지 /32 · MISSILE ×2)
    // 이름 → 와이어 종류. thing.bin 을 읽는 쪽(main)이 넣어 준다(ThingDb::WireByName) — 시험은 손으로 넣는다
    void SetNamedType(const std::string& name, uint16_t wire) { namedTypes_[name] = wire; }
    uint16_t NamedType(const std::string& name) const {
        auto it = namedTypes_.find(name);
        return it == namedTypes_.end() ? 0 : it->second;
    }
    // ★ 2026-09-18 ⑬ — **화살·쇠뇌살 종류.** 이 종류만 갱신에 「스프라이트 프레임」이 붙는다.
    //   정답지 `netSendPhantomPlrMb_5187E0`(GAME4_1.c:4198~4203) —
    //     `Class & 1`(MISSILE) **그리고** `SubClass & 0x30` 이면 `Direction1 >> 3` 을 싣고,
    //     아니면 `-1`. 그 `0x30` 은 무기 서브클래스의 **`ARROW`(0x10) | `BOLT`(0x20)** 다
    //     (`libs/object/subclass_weapon.go` 이름 순서 · 우리 `WeaponClassBitsOf` 와 같은 표).
    //   ⭕ 실측(`--dump-thing`) — `MercArcherArrow` = `ARROW` ⇒ 참 · `Fireball` = `MISSILE_COUNTERSPELL` ⇒ 거짓
    //   thing.bin 을 읽는 쪽(main)이 채운다. 시험은 손으로 넣는다.
    void AddArrowType(uint16_t wire) { if (wire) arrowTypes_.insert(wire); }
    bool IsArrowType(uint16_t wire) const { return arrowTypes_.count(wire) != 0; }
    size_t PlacedCount() const { return placed_.size(); }
    uint16_t DebugPlacedNetCode(size_t i) const { return i < placed_.size() ? placed_[i].netCode : 0; }
    bool DebugPlacedXY(size_t i, float& x, float& y) const {
        if (i >= placed_.size()) return false;
        x = placed_[i].x; y = placed_[i].y;
        return true;
    }
    uint8_t DebugPlacedKind(size_t i) const { return i < placed_.size() ? placed_[i].kind : 0; }
    float DebugPlacedZ(size_t i) const { return i < placed_.size() ? placed_[i].z : 0.0f; }
    uint16_t DebugPlacedType(size_t i) const { return i < placed_.size() ? placed_[i].type : 0; }
    // 마지막으로 받은 피해 종류(+524) — 불꽃 1 · 독 5 처럼 **무엇에 맞았나**를 시험이 가른다
    uint8_t DebugLastDamageType(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.lastDamageType;
    }
    // 날아가는 종류 하나 — thing.bin 에서 읽어 main 이 넣는다(이름으로 찾는다).
    struct ProjTypeInfo { uint16_t type = 0; float speed = 0; uint32_t flags = 0; uint8_t collideAmount = 0; };
    // ★ 09-23 nine-defects 2-a3 — 반주문에 지워지는 날아가는 것(thing.bin 서브클래스 `MISSILE_COUNTERSPELL`)의 종류.
    //   main 이 thing.bin 에서 넣고, 시험은 손으로 넣는다. Estate 기준 `Magic`·불덩이 셋·`Pixie` 다(`--dump-thing` 실측)
    void SetCounterMissileType(uint16_t wire) { counterMissileTypes_.insert(wire); }
    // ★ 2-a4 — 주문 쓰는 몬스터 종류(monster.bin `CAN_CAST_SPELLS`). main 이 `kMonstersCanCastSpells` 이름을 와이어 번호로 바꿔 넣는다
    void SetCastingMonsterType(uint16_t wire) { castingMonsterTypes_.insert(wire); }
    unsigned long long warcryCastsBroken() const { return warcryCastsBroken_; }    // ★ 2-a4 — 함성이 끊은 남의 시전
    unsigned long long warcryNullifiedMonsters() const { return warcryNullMon_; }  // ★ 2-a4 — 함성이 무효화를 건 몬스터
    unsigned long long warcryPeer(WarcryPeer k) const { return warcryPeer_[static_cast<size_t>(k)]; }   // ★ 09-24 H3
    unsigned long long WarcryPeerSum() const { unsigned long long n = 0; for (auto v : warcryPeer_) n += v; return n; }
    unsigned long long enchantSentAntiMagic() const { return enchantSentAnti_; }   // ★ 09-24 H3 — 반마법(29) 비트가 선 알림(0x5A) 수
    size_t CounterMissileTypeCount() const { return counterMissileTypes_.size(); }
    unsigned long long counterCasts() const { return counterCasts_; }         // 반주문을 부른 수
    unsigned long long counterMissiles() const { return counterMissiles_; }   // 지운 날아가는 것
    unsigned long long counterDurs() const { return counterDurs_; }           // 끊은 지속 주문
    unsigned long long counterEnchants() const { return counterEnchants_; }   // 끈 버프
    int DebugCastCounterSpell(const std::string& a, std::vector<OutPacket>& out) {
        auto i = sessions_.find(a); return i == sessions_.end() ? 0 : CastCounterSpell(i->second, out);
    }
    void SetProjType(const std::string& name, uint16_t type, int speedRaw, uint32_t flags, uint8_t collideAmount) {
        ProjTypeInfo p;
        p.type = type;
        p.speed = static_cast<float>(speedRaw) / 32.0f * 2.0f;   // SpeedBase = SPEED/32 · MISSILE 은 ×2
        p.flags = flags;
        p.collideAmount = collideAmount;
        projTypes_[name] = p;
    }
    const ProjTypeInfo* ProjType(const std::string& name) const {
        auto it = projTypes_.find(name);
        return it == projTypes_.end() ? nullptr : &it->second;
    }
    uint16_t DebugMissileType(size_t i) const { return i < missiles_.size() ? missiles_[i].type : 0; }
    float DebugMissileX(size_t i) const { return i < missiles_.size() ? missiles_[i].posX : 0.0f; }
    uint16_t DebugMissileNetCode(size_t i) const { return i < missiles_.size() ? missiles_[i].netCode : 0; }
    void SetMagicType(uint16_t type, int speedRaw, float mass, uint32_t flags) {
        magicType_.type = type;
        magicType_.flags = flags;                         // thing FLAGS(맞힘 거르기 sub_54E730)
        magicType_.speed = static_cast<float>(speedRaw) / 32.0f * 2.0f;
        magicType_.mass = mass > 0.0f ? mass : 1.0f;
    }
    size_t MissileCount() const { return missiles_.size(); }
    unsigned long long spellEffectDeferred() const { return spellEffectDeferred_; }
    Target DebugLastAcceptTarget() const { return lastAcceptTarget_; }
    unsigned long long DebugAcceptCount() const { return acceptCount_; }   // spellAccept 가 캡처 검사를 지난 횟수(시험용)
    // 시험용 — 지속 주문 틀(4-a3). createRet: -1 create 없음 · 0/1 돌려줄 값 · updateStopAt: -1 update 없음 · 0 늘 0 · N 번째에 1
    bool DebugDurNew(const std::string& caster, uint8_t spell, uint32_t dt, int createRet, int updateStopAt, bool withDestroy,
                     const Target& target, std::vector<OutPacket>& out);
    size_t DurCount() const { return durSpells_.size(); }
    bool DebugDurActive(uint8_t spell, const std::string& caster) const { return DurActive(spell, caster); }
    bool DebugDurCanceled(size_t i) const {
        size_t k = 0;
        for (const auto& d : durSpells_) if (k++ == i) return (d.flags88 & 1) != 0;
        return false;
    }
    int DebugDurDestroys() const { return debugDurDestroys_; }
    bool DebugDurFindByTarget(uint8_t spell, const Target& t) const { return DurFindByTarget(spell, t) != nullptr; }
    void DebugDurCancelOffensiveFor(const std::string& addr, std::vector<OutPacket>& out) { DurCancelOffensiveFor(addr, out); }
    void DebugPlayerCancelSpells(const std::string& addr, std::vector<OutPacket>& out) { PlayerCancelSpells(addr, out); }
    // 시험용 — **세기를 박아** 효과 함수만 부른다. 온라인은 늘 세기 3 이라 세기별 갈래를 시험할 길이 없다.
    bool DebugSpellAccept(const std::string& addr, uint8_t spell, int lvl, std::vector<OutPacket>& out) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return false;
        SpellArg arg;
        arg.x = static_cast<float>(it->second.aimX);
        arg.y = static_cast<float>(it->second.aimY);
        return SpellAccept(spell, it->second, addr, arg, lvl, out);
    }
    // 시험용 — 게임에서 빠진 것으로(오브젝트 지워짐 · inGame 끔)
    void DebugKickPlayer(const std::string& addr, std::vector<OutPacket>& out) { KickPlayer(addr, out); }
    void DebugSetMana(const std::string& addr, uint16_t v) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) { i->second.mana = v; i->second.manaPrev = v; }
    }
    bool DebugAbilityDo(const std::string& addr, uint8_t abil, std::vector<OutPacket>& out) {
        auto i = sessions_.find(addr);
        return i != sessions_.end() && AbilityDo(i->second, abil, out);
    }
    void DebugSendChapterEnd(const std::string& addr, uint8_t ch, bool ended, std::vector<OutPacket>& out) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) SendChapterEnd(i->second, ch, ended, out);
    }
    // 10-e — 시험용. 진짜 경로는 `0xBC` 조각을 받는 자리다
    bool DebugSysopTry(const std::string& addr, const std::string& pw, std::vector<OutPacket>& out) {
        auto i = sessions_.find(addr);
        if (i == sessions_.end()) return false;
        const bool okPw = !sysopPassword_.empty() && sysopPassword_ == pw;
        if (okPw) i->second.sysop = true;
        const uint8_t res[2] = { static_cast<uint8_t>(Op::SYSOP_RESULT),
                                 static_cast<uint8_t>(okPw ? 1 : 0) };
        EmitImportantRaw(i->second, res, sizeof(res), out);
        return okPw;
    }
    void DebugSetClosing(const std::string& addr) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) i->second.closing = true;
    }
    // 10-d — 시험용. 진짜 경로에서는 조각을 받을 때마다 새로 적혀 시간 초과가 안 난다
    void DebugTouch(const std::string& addr) {
        auto i = sessions_.find(addr);
        if (i != sessions_.end()) i->second.lastSeenFrame = frame_;
    }
    void DebugLeave(const std::string& addr) { auto it = sessions_.find(addr); if (it != sessions_.end()) it->second.inGame = false; }
    int DebugDurUpdates() const { return debugDurUpdates_; }
    uint8_t DebugDurSpellAt(size_t i) const {
        size_t k = 0;
        for (const auto& d : durSpells_) if (k++ == i) return d.spell;
        return 0;
    }
    // 시험용 — 날아가는 것을 손으로 옮긴다(좌표 관문 ⓒ 시험에 쓴다).
    //   ⚠ `posX`·`posY` 와 `newX`·`newY` 를 **함께** 맞춘다 — 갱신은 `posX` 를 싣고
    //     다음 틱의 움직임은 `newX` 에서 이어지므로 한쪽만 바꾸면 곧바로 되돌아온다.
    bool DebugSetMissileXY(size_t i, float x, float y) {
        if (i >= missiles_.size()) return false;
        missiles_[i].posX = missiles_[i].newX = missiles_[i].prevX = x;
        missiles_[i].posY = missiles_[i].newY = missiles_[i].prevY = y;
        return true;
    }
    bool DebugMissileXY(size_t i, float& x, float& y) const {
        if (i >= missiles_.size()) return false;
        x = missiles_[i].posX; y = missiles_[i].posY;
        return true;
    }
    // 시험용 — 직업만 바꾼다(스탯은 그대로 · 주문 직업 거르기를 보려고)
    void DebugSetClass(const std::string& addr, CharClass cls) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.charClass = cls;
    }
    // 시험용 — 서버 시계(마지막 틱 ms). 시험 도우미가 시계를 이어 돌릴 때
    uint32_t DebugNowMs() const { return lastTickMs_; }
    // 시험용 — 그 사람이 시전 중인가(+216)
    bool DebugCasting(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.castFrame != 0;
    }
    void SetSpellCost(uint8_t spell, uint8_t mana) { spellCost_[spell] = mana; }
    size_t SpellCostCount() const { return spellCost_.size(); }
    uint8_t SpellCost(uint8_t spell) const {
        auto it = spellCost_.find(spell);
        return it == spellCost_.end() ? 0 : it->second;
    }
    // 마나가 모자라 튕긴 시전 수 — 「눌렀는데 아무 일도 안 난다」를 로그로 가른다.
    unsigned long long spellsNoMana() const { return spellsNoMana_; }
    // 줍기 — 요청·성공·손이 안 닿음·되돌려 떨굼. 셋을 갈라야 「왜 안 주워지나」가 보인다.
    unsigned long long tryGets() const { return tryGets_; }
    unsigned long long picked() const { return picked_; }
    unsigned long long pickTooFar() const { return pickTooFar_; }
    unsigned long long pickNoCap() const { return pickNoCap_; }
    unsigned long long pickTooHeavy() const { return pickTooHeavy_; }
    unsigned long long pickTooMany() const { return pickTooMany_; }
    unsigned long long pickClassFail() const { return pickClassFail_; }
    unsigned long long drankOnPickup() const { return drankOnPickup_; }
    unsigned long long goldPicked() const { return goldPicked_; }
    unsigned long long dropped() const { return dropped_; }

    // ★ 묶음 5 — 그 사람의 **들 무게 밑천**(`+490`). 한도는 이 값의 **두 배**다.
    //   값이 어디서 오는지는 `Session::carryCap` 주석에 있다(thing.bin 이 아니라 힘에서 나온다).
    uint16_t CarryCapOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.carryCap;
    }
    // 시험용 — 밑천을 직접 정한다. 「밑천 0 이면 아무것도 못 든다」 갈래를 여는 데 쓴다
    //   (사람은 실제로 0 이 안 되지만, 정답지에 있는 문이라 그대로 옮겼다).
    void DebugSetCarryCap(const std::string& addr, uint16_t v) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.carryCap = v;
    }
    unsigned long long used() const { return used_; }
    // 쓰긴 했는데 **무슨 일이 나는지 모르는** 갈래(`ReadUse`·`WandUse`…). 지어내지 않은 자리의 수.
    unsigned long long useUnknown() const { return useUnknown_; }
    unsigned long long equipped() const { return equipped_; }
    unsigned long long dequipped() const { return dequipped_; }
    // 클라가 판 시계를 물어본 횟수. 전에는 이 조각을 통째로 버려 **0으로 보였다.**
    unsigned long long timerAsks() const { return timerAsks_; }
    unsigned long long chats() const { return chats_; }
    // 몇 판을 마쳤나. 0 이면 아직 한 판도 안 끝난 것이다.
    unsigned long long roundsPlayed() const { return roundsPlayed_; }
    // 깃발 HUD 를 몇 번 보냈나. **매 틱 수준으로 오르면 과잉이다**(0x5e 270배 사고 참조).
    unsigned long long flagStatusSent() const { return flagStatusSent_; }
    // 부활 무적이 막은 피해 횟수. 0 이면 무적이 한 번도 안 걸린 것이다.
    unsigned long long invulnBlocked() const { return invulnBlocked_; }
    unsigned long long appearanceSent() const { return appearanceSent_; }
    unsigned long long newPlayerGearBits() const { return newPlayerGearBits_; }   // ★ 09-28 — 2d 에 지금 갑옷·무기 비트를 실은 횟수
    // ★ 09-28 — 남을 소개한 2d 의 정보 97B 출처: [0] 녹음 그대로(Jack) · [1] 녹음에 그 사람 97B 를 덮음 · [2] 97B 로 조립
    unsigned long long newPlayerInfo(size_t i) const { return i < 3 ? newPlayerInfo_[i] : 0; }
    // 시험용 — `who` 를 `to` 에게 소개하는 `2d` 를 그대로 만든다(EmitNewPlayer 는 private)
    void DebugEmitNewPlayer(const std::string& to, const std::string& who, std::vector<OutPacket>& out) {
        auto t = sessions_.find(to); auto w = sessions_.find(who);
        if (t != sessions_.end() && w != sessions_.end()) EmitNewPlayer(t->second, w->second, 0, out);
    }
    // 시험용 — 이 사람에게 그 번호의 바닥 물건을 소개했나
    bool DebugStaticIntroduced(const std::string& addr, uint16_t netCode) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.staticIntroAt.count(netCode) != 0;
    }
    unsigned long long appearanceUnknown() const { return appearanceUnknown_; }
    unsigned long long fxSent() const { return fxSent_; }
    void SetNoSpellStart(bool on) { noSpellStart_ = on; }
    // ★ 진단 — 걷는 그림 칸을 **한 값으로 고정**한다(-1 이면 안 고정).
    //   서 있는 채로 칸만 바꿔 캡처하면 **위치가 안 변해** 자세를 나란히 견줄 수 있다.
    //   캡처 한 장에 2초가 걸려, 걸으면서 찍으면 캐릭터가 나무 뒤로 가 버린다(2026-09-11).
    // ★ 진단 — 사람 항목의 **자세 번호**를 못 박는다(-1 이면 안 함).
    //   걷기(4)는 서기(0)와 그림 차이가 작아 캡처로 안 갈린다. 죽음(2)처럼
    //   **눕는 자세**로 시험하면 먹는지 안 먹는지가 한눈에 난다.
    void SetForceAnimPose(int v) { forceAnimPose_ = v; }
    // ★★ 진단 — 사람 항목 덧붙이 **첫 바이트의 방향 3비트**를 못 박는다(0~7, -1 이면 안 함).
    //   자세(하위 4비트든 별도 바이트든)가 두 방식 다 화면을 못 바꿨다. 그래서 묻는다 —
    //   **덧붙이 바이트가 클라에 닿기는 하나?** 방향이 바뀌면 캐릭터가 다른 쪽을 보므로
    //   눈에 확 띈다. 방향도 안 먹으면 덧붙이 자체가 안 읽히는 것이다.
    void SetForceAnimDir(int v) { forceAnimDir_ = v; }
    // ★ 2026-09-18 ⑬ — 화살 **스프라이트 프레임을 못 박는다**(0~31, -1 이면 안 함).
    //   까닭 — 「프레임이 방향대로 바뀌나」를 정지 사진으로 판정하려면 **같은 자리에서
    //   프레임만 바꿔 찍어 픽셀을 대조**해야 한다(`--anim-dir` 로 사람 방향을 닫은 것과 같은 방식).
    void SetForceArrowFrame(int v) { forceArrowFrame_ = v; }
    int ForceArrowFrame() const { return forceArrowFrame_; }
    // 진단 — 화살을 난 자리에 세워 둔다(같은 자리에서 프레임만 바꿔 찍으려고)
    void SetArrowFreeze(bool v) { arrowFreeze_ = v; }
    // 진단 — N 틱마다 프레임을 0 ↔ 16 으로 번갈아 보낸다.
    //   ⚠ **판을 두 번 띄워 비교하면 안 된다** — 어느 덫이 언제 쐈는지가 판마다 달라
    //   화살 자리가 어긋나고, 그 어긋남이 잡음 바닥을 1,335 픽셀까지 올린다(09-18 실측).
    //   한 판 안에서 프레임만 바꿔야 배경도 화살 자리도 그대로다.
    void SetArrowFrameAlt(int ticks) { arrowFrameAlt_ = ticks; }
    // ★ 진단 — 들어올 때 체력(0 이면 안 건드림). **최대 체력은 그대로** 둔다.
    void SetStartHealth(int v) { startHealth_ = v; }
    // 진단 — 들어올 때 마나를 채운다(`--start-mana N`). 주먹(29) 같은 비싼 주문을
    //   무인으로 쏘려면 필요하다(09-18 실측 — 마나 5/125 라 6번 막혔다)
    void SetStartMana(int v) { startMana_ = v; }
    // 시험용 — 그 사람 팀에 득점을 넣는다(깃발을 실제로 나르지 않고 판끝을 재려고).
    void DebugCapture(const std::string& addr, int n) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end() || it->second.team >= 3) return;
        for (int i = 0; i < n; ++i) {
            ++caps_[it->second.team];
            if (!roundOver_ && caps_[it->second.team] >= kCaptureLimit) {
                roundOver_ = true;
                restartAtFrame_ = frame_ + kRoundRestartTicks;
            }
        }
    }
    // 겨눔을 받은 횟수. 0 이면 `0xac` 를 안 읽는 것이고 투사체가 몸 방향으로만 난다.
    unsigned long long aims() const { return aims_; }
    // 시험용 — 날아가는 것 하나의 자리. 없으면 (0,0).
    // 시험용 — 겨눈 자리를 직접 넣는다.
    void DebugAimAt(const std::string& addr, int x, int y) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.aimX = x;
        it->second.aimY = y;
        it->second.aimAtFrame = frame_;
        it->second.hasAim = true;
    }
    // ★ **닿은 피해 전부** — 근접·투사체·소환수를 다 센다.
    //   `hits()` 는 **투사체 명중만** 센다. 둘을 갈라야 「싸움이 안 붙었다」와
    //   「투사체만 안 맞았다」가 구분된다(09-10 오판의 자리).
    unsigned long long hurts() const { return hurts_; }
    unsigned long long contactDamageHits() const { return contactDamageHits_; }   // ★ 1-d — 맵 불 같은 장애물 닿음 피해
    bool IsEquipped(const std::string& addr, uint16_t netCode) const {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return false;
        for (const auto& c : it->second.inventory)
            if (c.netCode == netCode) return c.equipped;
        return false;
    }

    // ★ 틱 — 시각을 넣으면 주기가 지난 만큼 프레임을 올리고 상태를 뿌린다.
    //
    // 실측 주기는 **35.77ms(28Hz)** 였다(표본 27,619). 목표가 30Hz(33.3ms)인데
    // 못 따라간 것으로 보여, 여기서는 **30Hz 를 목표로** 둔다.
    // ⚠ 그 실측은 OpenNox 서버의 것이지 정품 호스트의 값이 아니다.
    //
    // 돌려주는 값 = 이번에 지나간 틱 수.
    int Tick(uint32_t nowMs, std::vector<OutPacket>& out);

    uint32_t frame() const { return frame_; }
    // ★ 게임에 들어와 있는 사람 수. **「서로 보임」이 안 될 때 여기부터 본다** —
    //   와이어를 뜯어 세면 내 디코더가 항목을 잘못 쪼개 읽어 헛것을 볼 수 있다(2026-09-08).
    size_t inGameCount() const {
        size_t n = 0;
        for (const auto& kv : sessions_) if (kv.second.inGame) ++n;
        return n;
    }
    // 마지막 틱에 상태 갱신에 실은 사람 수(마지막으로 만든 것 기준).
    size_t lastUpdateObjects() const { return lastUpdateObjects_; }
    // ★ 첫 사람의 `snapshotIn`. 「사람 1(갱신에 0)」이 나올 때 **왜 갱신을 안 만드는지**
    //   가르는 자리다 — 이 값이 0 이상이면 스냅샷을 기다리느라 매 틱 건너뛰고 있다.
    size_t lastLoopSeen() const { return lastLoopSeen_; }
    size_t lastSessions() const { return lastSessions_; }
    int firstSnapshotIn() const {
        for (const auto& kv : sessions_) if (kv.second.inGame) return kv.second.snapshotIn;
        return -99;
    }
    unsigned long long moves() const { return moves_; }
    // 쉬는 시간을 지나 실제로 나간 근접 공격 수(누른 횟수가 아니다).
    unsigned long long melees() const { return melees_; }
    unsigned long long code6Seen() const { return code6Seen_; }

    // 0 이상 n 미만의 난수. **재현 가능한 것으로 둔다** — 시험이 흔들리면 안 된다.
    //   (선형 합동법. 정품과 같은 난수열일 까닭이 없고, 필요한 것은 「가끔 발차기」뿐이다)
    uint32_t NextRandom(uint32_t n) {
        rngState_ = rngState_ * 1664525u + 1013904223u;
        return n ? (rngState_ >> 16) % n : 0;
    }
    // 0 이상 1 미만의 난수 실수(randomFloat 용). 같은 난수열을 쓴다.
    float NextRandomFloat() { return static_cast<float>(NextRandom(65536)) / 65536.0f; }
    // 날아가다 사람에게 맞은 횟수.
    // 지금 날아가고 있는 것의 수.
    // 지금 날아가고 있는 오브젝트(Magic·불덩이 …)의 수.
    size_t missiles() const { return missiles_.size(); }
    // ★★ 2026-09-16 — **만든 누계 · 사라진 누계**를 같이 센다.
    //   `missiles()`(지금 살아 있는 수)만으로는 「안 만들어졌다」와 「만들자마자 사라졌다」를 **못 가른다.**
    //   09-16 에 `나는것 0` 이 174틱 표본 전부에 찍혔는데 그게 어느 쪽인지 몰라 헛다리를 짚었다.
    //   셋을 나란히 놓으면 갈린다 — 만든 0 이면 안 쏜 것, 만든 N·사라진 N·지금 0 이면 즉사다.
    unsigned long long missilesMade() const { return missilesMade_; }
    unsigned long long missilesGone() const { return missilesGone_; }
    // ★★ **갱신에 실린 누계.** 「지금 살아 있는 수」는 30프레임 표본이라
    //   덫이 쏘는 박자와 어긋나면 **살아 있어도 0 으로 보인다**(09-16 에 이걸로 헛짚었다).
    //   「클라에 갔나」는 반드시 **누계**로 물어야 한다.
    unsigned long long missileEntriesSent() const { return missileEntriesSent_; }
    unsigned long long updateEncOk() const { return updateEncOk_; }
    unsigned long long updateEncFail() const { return updateEncFail_; }
    size_t updateEncFailObjsMax() const { return updateEncFailObjsMax_; }
    size_t updateEncOkObjsMax() const { return updateEncOkObjsMax_; }
    unsigned long long updateDroppedObjs() const { return updateDroppedObjs_; }
    unsigned long long updateDroppedTicks() const { return updateDroppedTicks_; }
    size_t updateDroppedMax() const { return updateDroppedMax_; }
    unsigned long long updateDroppedSnap() const { return updateDroppedSnap_; }
    unsigned long long abilHeard() const { return abilHeard_; }
    unsigned long long abilBadPacket() const { return abilBadPacket_; }
    unsigned long long abilObserver() const { return abilObserver_; }
    unsigned long long abilCalls() const { return abilCalls_; }
    unsigned long long abilBadNum() const { return abilBadNum_; }
    // ★ 09-23 — 합계는 **능력별 칸의 합**이다(따로 세지 않는다 — 두 벌이면 어긋나도 모른다)
    unsigned long long abilDead() const { return AbilGateSum(AbilGate::Dead); }
    unsigned long long abilNotWarrior() const { return AbilGateSum(AbilGate::NotWarrior); }
    unsigned long long abilExclusive() const { return AbilGateSum(AbilGate::Exclusive); }
    unsigned long long abilAlready() const { return AbilGateSum(AbilGate::Already); }
    unsigned long long abilMoveState() const { return AbilGateSum(AbilGate::MoveState); }
    unsigned long long abilNotLearned() const { return AbilGateSum(AbilGate::NotLearned); }
    unsigned long long abilOverweight() const { return AbilGateSum(AbilGate::Overweight); }
    unsigned long long abilCooldown2() const { return AbilGateSum(AbilGate::Cooldown); }
    unsigned long long abilOk() const { return AbilGateSum(AbilGate::Ok); }
    // 능력 하나(1~5)의 칸 · 번호 검사를 지나 들어온 수(= 그 능력 칸 아홉의 합이어야 한다)
    unsigned long long abilGate(uint8_t abil, AbilGate g) const {
        return abil < kAbilityCount && g < AbilGate::Count ? abilGate_[abil][static_cast<size_t>(g)] : 0;
    }
    unsigned long long abilIn(uint8_t abil) const { return abil < kAbilityCount ? abilIn_[abil] : 0; }
    unsigned long long AbilGateSum(AbilGate g) const {
        unsigned long long n = 0;
        for (int a = 0; a < kAbilityCount; ++a) n += abilGate_[a][static_cast<size_t>(g)];
        return n;
    }
    void SetNoSecondNewPlayer(bool v) { noSecondNewPlayer_ = v; }
    void SetForceHarpoon(bool v) { forceHarpoon_ = v; }
    void SetDumpHarpoon(bool v) { dumpHarpoon_ = v; }
    unsigned long long harpoonDoCalls() const { return harpoonDoCalls_; }
    unsigned long long harpoonNoType() const { return harpoonNoType_; }
    unsigned long long harpoonShot() const { return harpoonShot_; }
    unsigned long long harpoonAttached() const { return harpoonAttached_; }
    unsigned long long harpoonPullTicks() const { return harpoonPullTicks_; }
    // ★ 09-23 0-b — 작살이 무엇에 닿았나 · 줄이 왜 끊겼나(끊긴 것만 센다 — 화살이 없을 때 부른 것은 안 셈)
    unsigned long long harpoonTouch(HarpoonTouch t) const {
        return t < HarpoonTouch::Count ? harpoonTouch_[static_cast<size_t>(t)] : 0;
    }
    unsigned long long harpoonEnd(HarpoonEnd e) const {
        return e < HarpoonEnd::Count ? harpoonEnd_[static_cast<size_t>(e)] : 0;
    }
    unsigned long long abilValOff() const { return abilValOff_; }               // ★ 2-a1 — 켜짐 값 내림(함성 애니 끝)
    unsigned long long warcryShouts() const { return warcryShouts_; }           // ★ 2-a1 — 함성 칸 2→3
    unsigned long long warcryStunned() const { return warcryStunned_; }         // ★ 2-a2 — 외침에 기절한 몬스터
    unsigned long long berserkChargeTicks() const { return berserkChargeTicks_; } // ★ 2-b1 — 광전사가 민 틱
    unsigned long long berserkCollideCalls() const { return berserkCollideCalls_; } // ★ 2-b2 — 광전사 켜진 채 부딪힘 함수를 부른 횟수
    // ★ 09-26 M2 — 죽음(16) 걸린 사람이 산 사람에게 닿았을 때: [0] 남은 시간이 14초 이상이라 안 넘김 · [1] 넘김
    unsigned long long deathPass(bool passed) const { return deathPass_[passed ? 1 : 0]; }
    unsigned long long lavaHurts() const { return lavaHurts_; }   // ★ 09-26 M3 — 바닥 칸 6(용암) 피해 부름(사람 · 틱에 한 번)
    unsigned long long lavaHurtsPet() const { return lavaHurtsPet_; }   // ★ 09-27 S2 — 같은 것(소환수·맵 몬스터)
    // ★ 09-27 B1 — 접속 때 직업 기본 장비를 가방에: [0] 종류 틀이 없어 못 실음 · [1] 실음
    unsigned long long gearGiven(bool ok) const { return gearGiven_[ok ? 1 : 0]; }
    unsigned long long weaponNoStats() const { return weaponNoStats_; }   // ★ 09-27 B2
    unsigned long long equipGate(EquipGate g) const { return g < EquipGate::Count ? equipGate_[static_cast<size_t>(g)] : 0; }   // ★ 09-27 B3
    unsigned long long dequipGate(DequipGate g) const { return g < DequipGate::Count ? dequipGate_[static_cast<size_t>(g)] : 0; }   // ★ 09-27 B4
    unsigned long long dropGate(DropGate g) const { return g < DropGate::Count ? dropGate_[static_cast<size_t>(g)] : 0; }   // ★ 09-28
    unsigned long long decayGate(DecayGate g) const { return g < DecayGate::Count ? decayGate_[static_cast<size_t>(g)] : 0; }   // ★ 09-28
    unsigned long long decayExpired() const { return decayExpired_; }   // ★ 09-28 — 시계가 다 돼 지운 물건 수
    unsigned long long decayShadowSent() const { return decayShadowSent_; }   // ★ 09-28 — 그때 보낸 `33` 건수(받는 사람별)
    // ★ 09-28 B4b — 죽을 때: 남긴 평상복 · 나선 자리에 떨굼 · 몸 자리에 떨굼 / 부활 때: 지움 · 새로 줌 · 틀 없어 못 줌 · 협동·퀘스트 표 안 옮김
    unsigned long long deathDropKept() const { return deathDropKept_; }
    unsigned long long deathDropSpiral() const { return deathDropSpiral_; }
    unsigned long long deathDropAtBody() const { return deathDropAtBody_; }
    unsigned long long respawnGearDeleted() const { return respawnGearDeleted_; }
    unsigned long long respawnGearGiven() const { return respawnGearGiven_; }
    unsigned long long respawnGearNoTemplate() const { return respawnGearNoTemplate_; }
    unsigned long long respawnGearNotPorted() const { return respawnGearNotPorted_; }
    unsigned long long wornLookSent() const { return wornLookSent_; }       // ★ 09-29
    unsigned long long wornLookSkipped() const { return wornLookSkipped_; } // ★ 09-29
    unsigned long long introRedrawSent() const { return introRedrawSent_; } // ★ 09-29
    // ★ 09-28 B4b — 물건 종류의 몸 반지름(+176 · thing.bin 원 모양). 죽을 때 떨굴 자리 간격을 가른다(`GAME3_3.c:6730~6748`)
    void SetItemRadius(uint16_t type, float r) { itemRadius_[type] = r; }
    // ★ 09-28 — 줍고 저절로 입음: [0] 무기 듦 · [1] 갑옷 입음 · [2] 같은 부위 이미 입음(안 입음) · [3] 방패인데 두 손 무기(안 입음) · [4] 부위 모름(안 입음)
    unsigned long long autoEquip(size_t i) const { return i < 5 ? autoEquip_[i] : 0; }
    // 시험용 — 그 번호의 바닥 물건 자리(없으면 거짓)
    bool DebugStaticPos(uint16_t netCode, int& x, int& y) const {
        for (const auto& w : statics_) if (w.netCode == netCode) { x = w.x; y = w.y; return true; }
        return false;
    }
    unsigned long long wandChargesOnEquip() const { return wandChargesOnEquip_; }   // ★ 09-27 B5
    unsigned long long twoHandShieldOff() const { return twoHandShieldOff_; }
    unsigned long long shieldDropsTwoHand() const { return shieldDropsTwoHand_; }   // ★ 09-28 — 방패를 입어 내려놓게 한 두 손 무기
    unsigned long long wandSpellCut() const { return wandSpellCut_; }
    uint32_t DebugArmorEquip(const std::string& addr) const {                        // 시험용 — 플레이어 +0(갑옷 비트 합)
        auto it = sessions_.find(addr); return it == sessions_.end() ? 0u : ArmorEquipOf(it->second);
    }
    // ★ 09-27 B3 — modifier.bin `WEAPON_DEFINITIONS` 한 칸(종류 번호별): 쓸 수 있는 직업 비트(CLASSUSE — 워리어 1 · 위저드 2 · 컨저러 4) · 요구 힘.
    //   정답지 `server/modifiers.go:314~320`(Classes62 · ReqStrength60). main 이 복호본을 읽어 채운다 — 없는 종류는 직업 검사에서 거절(`object_pickup.go:113~118`)
    //   ★ 09-29 — 화살 셋(ArcherArrow·ArcherBolt·WeakArcherArrow)은 피해 식 `calcBoltDamage_4EF1E0` 이 이 정의의 DAMAGE_MIN(+72)·DAMAGE_COEFFICIENT(+64)를 쓴다
    void SetWeaponDef(uint16_t wire, uint8_t classes, uint16_t reqStrength, uint16_t damageMin = 0, float coeff = 0.0f) {
        weaponDef_[wire] = WeaponDef{ classes, reqStrength, damageMin, coeff };
    }
    unsigned long long bowShot(BowShot k) const { return k < BowShot::Count ? bowShot_[static_cast<size_t>(k)] : 0; }          // ★ 09-29
    unsigned long long arrowHit(ArrowHit k) const { return k < ArrowHit::Count ? arrowHit_[static_cast<size_t>(k)] : 0; }      // ★ 09-29
    unsigned long long weaponPick(WeaponPick k) const { return k < WeaponPick::Count ? weaponPick_[static_cast<size_t>(k)] : 0; }   // ★ 09-29
    unsigned long long quiverFromMap() const { return quiverFromMap_; }   // ★ 09-29 — 맵 짐에서 화살 수를 읽어 실은 바닥 화살통
    unsigned long long bowPoseEnded() const { return bowPoseEnded_; }     // ★ 09-29
    unsigned long long connectTailStops() const { return connectTailStops_; }   // ★ 09-29 ③
    int DebugArrowDamage(const std::string& owner, uint16_t arrowType) const { return ArrowDamage(owner, arrowType); }   // ★ 09-29
    void NoteQuiverFromMap() { ++quiverFromMap_; }
    // ★ 09-29 — 시험용: 가방 물건의 재장전 칸·화살 수를 바꾼다
    bool DebugSetBowReload(const std::string& addr, uint16_t nc, uint8_t v) {
        auto it = sessions_.find(addr); if (it == sessions_.end()) return false;
        for (auto& c : it->second.inventory) if (c.netCode == nc) { c.bowReload = v; return true; }
        return false;
    }
    bool DebugSetCharges(const std::string& addr, uint16_t nc, uint8_t cur, uint8_t max) {
        auto it = sessions_.find(addr); if (it == sessions_.end()) return false;
        for (auto& c : it->second.inventory) if (c.netCode == nc) { c.charges = cur; c.chargesMax = max; return true; }
        return false;
    }
    // ★ 09-29 — 갑옷 정의(modifier.bin `ARMOR_DEFINITIONS` — CLASSUSE · REQUIRED_STRENGTH · ARMOR_VALUE → `Modifier` +62·+60·+64)
    void SetArmorDef(uint16_t wire, uint8_t classes, uint16_t reqStrength, float value) { armorDef_[wire] = ArmorDef{ classes, reqStrength, value }; }
    size_t ArmorDefCount() const { return armorDef_.size(); }
    unsigned long long armorGate(ArmorGate g) const { return g < ArmorGate::Count ? armorGate_[static_cast<size_t>(g)] : 0; }
    float DebugArmorValue(const std::string& addr) const { auto it = sessions_.find(addr); return it == sessions_.end() ? 0.0f : ArmorValueOf(it->second); }
    size_t WeaponDefCount() const { return weaponDef_.size(); }
    // 시험용 — 든 무기(09-27 B2): 비트 · 종류 번호 · 무기 칸 번호
    uint32_t DebugWeaponEquip(const std::string& addr) const {
        auto it = sessions_.find(addr); return it == sessions_.end() ? 0u : WeaponEquipOf(it->second);
    }
    uint16_t DebugWeaponWire(const std::string& addr) const {
        auto it = sessions_.find(addr); return it == sessions_.end() ? 0 : WeaponWireOf(it->second);
    }
    uint16_t DebugWeaponNet(const std::string& addr) const {
        auto it = sessions_.find(addr); return it == sessions_.end() ? 0 : it->second.weaponNet;
    }
    unsigned long long berserkHit(BerserkHit h) const {                          // ★ 2-b2 — 그 갈래별
        return h < BerserkHit::Count ? berserkHit_[static_cast<size_t>(h)] : 0;
    }
    unsigned long long BerserkHitSum() const {
        unsigned long long n = 0;
        for (size_t i = 0; i < kBerserkHitCount; ++i) n += berserkHit_[i];
        return n;
    }
    unsigned long long wallContacts() const { return wallContacts_; }           // ★ 2-b2 — 벽 닿음 짝(사람 · 틱마다 한 번)
    // ★ 3-a — 점프
    unsigned long long jumpCodes() const { return jumpCodes_; }                 // 받은 코드 7(분모 — 죽음·관전 중 것도 센다)
    unsigned long long jumpIn() const { return jumpIn_; }                       // 조작 처리까지 온 것 = 갈래 합
    unsigned long long jumpGate(JumpGate g) const { return g < JumpGate::Count ? jumpGate_[static_cast<size_t>(g)] : 0; }
    unsigned long long JumpGateSum() const {
        unsigned long long n = 0;
        for (size_t i = 0; i < kJumpGateCount; ++i) n += jumpGate_[i];
        return n;
    }
    unsigned long long jumpForceTicks() const { return jumpForceTicks_; }       // 점프 중 앞으로 민 틱
    unsigned long long jumpHeldTicks() const { return jumpHeldTicks_; }         // 작살에 걸려 안 민 틱
    unsigned long long jumpLanded() const { return jumpLanded_; }               // 애니가 끝나 내려옴(상태 0)
    unsigned long long airborneOffIdle() const { return airborneOffIdle_; }     // 가만(13)이 떠 있음을 끔
    unsigned long long recoilBlockedAirborne() const { return recoilBlockedAirborne_; }   // 떠 있어 움찔(30)을 안 받음
    unsigned long long equakeSkipAirborne() const { return equakeSkipAirborne_; }       // ★ 3-b 지진이 뺀 떠 있는 것(사람·소환수)
    unsigned long long spellRefusedAirborne() const { return spellRefusedAirborne_; }   // ★ 3-b 떠 있어 거절한 시전
    // ★ 4-a — 휘두르기 소리(879 PunchMissing ~ 884 MaceMissing)를 부른 수. ⚠ 표에 소리조각이 없으면 `PlaySoundAt` 이 버린다 — 부름 ≠ 나감
    unsigned long long swingSound(uint16_t id) const { return (id >= 879 && id <= 884) ? swingSound_[id - 879] : 0; }
    // ★ 4-c2 — 사람이 맞을 때 타격 소리 갈래(부름 = 합)
    unsigned long long hitSoundCalls() const { return hitSoundCalls_; }
    uint16_t lastHitSound() const { return lastHitSound_; }                     // 마지막으로 낸 타격 소리 번호(시험용)
    // ★ 4-c3a — 사람 아닌 것(소환수·몬스터)이 맞을 때 기본 타격 소리 갈래(부름 = 합)
    unsigned long long defHitSoundCalls() const { return defHitSoundCalls_; }
    unsigned long long harpoonHitSoundCalls() const { return harpoonHitSoundCalls_; }   // ★ 4-c3b2b
    unsigned long long harpoonHitSoundWay(DefHitSoundWay w) const { return harpoonHitSoundWay_[static_cast<size_t>(w)]; }
    unsigned long long defHitSoundWay(DefHitSoundWay w) const {
        return w < DefHitSoundWay::Count ? defHitSoundWay_[static_cast<size_t>(w)] : 0;
    }
    unsigned long long DefHitSoundWaySum() const {
        unsigned long long n = 0;
        for (size_t i = 0; i < kDefHitSoundWayCount; ++i) n += defHitSoundWay_[i];
        return n;
    }
    // `Sub_532EC0` — 부서지는 재질 소리(8 나무 · 0x10 쇠 · 0x20 돌 · 0x40·0x400 흙 · 0x1000 마법 — 없으면 0)
    static uint16_t BreakableHitSound(uint32_t mat);
    // `532E20` 의 판단부(소리 번호 · 갈래) — 맞는 쪽 재질 · 체력 있나 · 피해 종류(+524) · 때린 것 있나 · 그 재질. 부르는 자리와 떼어 전수로 본다
    static uint16_t DefaultHitSound(uint32_t victimMat, bool hasHp, uint8_t dtype, bool hitterPresent, uint32_t hitterMat,
                                    DefHitSoundWay& way);
    unsigned long long hitSoundWay(HitSoundWay w) const { return w < HitSoundWay::Count ? hitSoundWay_[static_cast<size_t>(w)] : 0; }
    unsigned long long HitSoundWaySum() const {
        unsigned long long n = 0;
        for (size_t i = 0; i < kHitSoundWayCount; ++i) n += hitSoundWay_[i];
        return n;
    }
    // `Sub_532930`(`server/sound.go:31~161`) — 맞는 쪽 재질 × 때린 것 재질 → 소리 번호(없으면 0). 값이 **딱 그 비트**일 때만 맞는다
    static uint16_t HitSoundFor(uint32_t victimMat, uint32_t hitterMat);
    // ★ 4-c2 — thing.bin 재질(와이어 번호 색인) · 가슴갑옷(ARMOR + SUBCLASS BREASTPLATE)인가 · 사람 몸 종류(NewPlayer) 번호. main 이 넣는다
    // ★ 4-d4 — 발밑 바닥(맵 FloorMap + thing.bin EDGE·FLOR 재질). main 이 맵을 읽고 넣는다 — 비어 있으면 발소리 없음(바닥 없음으로 셈)
    void SetFloor(FloorGrid grid, std::vector<FloorGrid::EdgeDef> edges, std::vector<int32_t> materials) {
        floorGrid_ = std::move(grid); floorEdges_ = std::move(edges); floorMaterial_ = std::move(materials);
    }
    // 재질 → 바닥 종류(`object_update.go:164~220` switch — 2·0x4000 소리 없음 · 8 나무 · 64 흙 · 128 물 · 0x400 눈 · 0x800 진흙 · 그 밖 돌)
    static FootKind FootKindOf(int32_t mat) {
        switch (mat) {
            case 2: case 0x4000: return FootKind::Silent;
            case 8:     return FootKind::Wood;
            case 64:    return FootKind::Dirt;
            case 128:   return FootKind::Water;
            case 0x400: return FootKind::Snow;
            case 0x800: return FootKind::Mud;
            default:    return FootKind::Stone;
        }
    }
    static const char* FootSoundName(FootKind k, bool run) {
        switch (k) {
            case FootKind::Wood:  return run ? "RunOnWood" : "WalkOnWood";
            case FootKind::Dirt:  return run ? "RunOnDirt" : "WalkOnDirt";
            case FootKind::Water: return run ? "RunOnWater" : "WalkOnWater";
            case FootKind::Snow:  return run ? "RunOnSnow" : "WalkOnSnow";
            case FootKind::Mud:   return run ? "RunOnMud" : "WalkOnMud";
            case FootKind::Stone: return run ? "RunOnStone" : "WalkOnStone";
            default:              return nullptr;
        }
    }
    unsigned long long footRun(FootKind k) const { return footRun_[static_cast<size_t>(k)]; }
    unsigned long long footWalk(FootKind k) const { return footWalk_[static_cast<size_t>(k)]; }
    unsigned long long FootRunSum() const { unsigned long long n = 0; for (auto v : footRun_) n += v; return n; }
    unsigned long long FootWalkSum() const { unsigned long long n = 0; for (auto v : footWalk_) n += v; return n; }
    unsigned long long footTreadSilent() const { return footTreadSilent_; }
    unsigned long long hurtVoice(HurtVoice k) const { return hurtVoice_[static_cast<size_t>(k)]; }   // ★ 09-24
    unsigned long long HurtVoiceSum() const { unsigned long long n = 0; for (auto v : hurtVoice_) n += v; return n; }
    void DebugSetInfo97(const std::string& addr, const std::vector<uint8_t>& info) {   // ★ 09-28 시험용 — `CLIENT_ACCEPT` 로 받는 97B 대신
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.info97 = info;
    }
    void DebugSetFemale(const std::string& addr, bool female) {   // ★ 09-24 시험용 — 사람 정보 97바이트의 67번 칸(IsFemale)
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        if (it->second.info97.size() < 68) it->second.info97.resize(97, 0);
        it->second.info97[67] = female ? 1 : 0;
    }
    unsigned long long exertionLight() const { return exertionLight_; }
    // ★ 4-c3b3b — 종류 번호별 「그 종류 이름의 소리 모음에 맞히는 소리가 있고 조각이 1 이상」(main 이 soundset.bin 으로 채운다)
    void SetMonsterHitSounds(std::vector<uint8_t> byWire) { monsterHitSound_ = std::move(byWire); }
    // ★ 09-24 H1 — 종류 번호별 근접 「맞히는 소리」(+32 MELEE_ATTACK_HITTING_SOUND) 번호. 0 = 모음이 없거나 NULL(main 이 soundset.bin 으로 채운다)
    //   조각이 0 인 번호도 싣는다 — 그런 것은 `PlaySoundAt` 이 버린다(정답지 `aud_501960` 도 조각 0 이면 안 만든다)
    void SetMonsterMeleeHitSoundIds(std::vector<uint16_t> byWire) { monsterSnd_[7] = std::move(byWire); }
    // ★ 09-26 M4a — 다른 칸도 같은 꼴로(근접 시작 5 · 쏘기 시작 9 · 쏘기 10 · 죽음 14 — 정답지 `aud_501960(모음+칸, 몬스터, 0, 0)`)
    void SetMonsterSoundIds(int field, std::vector<uint16_t> byWire) { if (field >= 0 && field < 18) monsterSnd_[field] = std::move(byWire); }
    // ★ 09-27 B1 — 기본 장비의 가방 항목 틀(와이어 번호 → 틀 · 번호·입음은 `GiveDefaultGear` 가 채운다). main 이 맵 물건과 같은 칸 규칙으로 thing.bin 에서 채운다
    void SetItemTemplate(uint16_t wire, const Session::Carried& c) { itemTemplate_[wire] = c; }
    unsigned long long monsterSnd(int field, bool played) const { return (field >= 0 && field < 18) ? monsterSndCount_[field][played ? 1 : 0] : 0; }
    unsigned long long monsterMeleeHitSnd(bool played) const { return played ? monsterMeleeHitPlayed_ : monsterMeleeHitNone_; }
    void SetMaterialTables(std::vector<uint32_t> matByWire, std::vector<uint8_t> breastplateByWire, uint16_t newPlayerWire) {
        matByWire_ = std::move(matByWire); breastByWire_ = std::move(breastplateByWire); newPlayerWire_ = newPlayerWire;
    }
    uint32_t MaterialOfWire(uint16_t w) const { return w < matByWire_.size() ? matByWire_[w] : 0u; }
    void DebugGiveItemType(const std::string& x, uint16_t type) {   // ★ 4-c2 시험용 — 종류만 있는 물건 하나를 쥐여 준다
        auto i = sessions_.find(x);
        if (i == sessions_.end()) return;
        Session::Carried c; c.netCode = 60002; c.type = type;
        i->second.inventory.push_back(c);
    }
    void DebugGiveCarried(const std::string& x, const Session::Carried& c) {   // ★ 09-29 시험용 — 줍기 관문을 안 거치고 가방에 넣는다(퀘스트에서 주운 것 흉내)
        auto i = sessions_.find(x);
        if (i != sessions_.end()) i->second.inventory.push_back(c);
    }
    bool DebugAirborne(const std::string& a) const { auto i = sessions_.find(a); return i != sessions_.end() && i->second.airborne; }
    bool DebugAbilActiveVal(const std::string& a, uint8_t k) const {            // ★ 2-a1 시험용
        auto i = sessions_.find(a); return i != sessions_.end() && AbilityIsActiveVal(i->second, k);
    }
    bool DebugPlayerSetState(const std::string& a, uint8_t st) {                // ★ 2-a1 시험용 — 규칙을 거치는 상태 바꾸기
        auto i = sessions_.find(a); return i != sessions_.end() && PlayerSetState(i->second, st, frame_);
    }
    unsigned long long harpoonAimFound() const { return harpoonAimFound_; }     // ★ 1-b
    unsigned long long harpoonSteerTicks() const { return harpoonSteerTicks_; } // ★ 1-b
    // ★ 09-23 0-b — 능력이 꺼진 까닭 · 겹침을 **누가** 막았나(누른 것 × 켜져 있던 것, 1~3) · 능력 소리 부름
    unsigned long long abilEnded(uint8_t abil, AbilEnd e) const {
        return abil < kAbilityCount && e < AbilEnd::Count ? abilEnded_[abil][static_cast<size_t>(e)] : 0;
    }
    unsigned long long abilBlockedBy(uint8_t abil, uint8_t by) const {
        return abil < 4 && by < 4 ? abilBlockedBy_[abil][by] : 0;
    }
    // ★ 1-a — 달리려던 틱을 살금살금이 걷기로 눌렀다(`object_update.go:153` 의 `|| IsActive(TreadLightly)`)
    unsigned long long treadBlockedRun() const { return treadBlockedRun_; }
    unsigned long long abilSoundUseCalls(uint8_t abil) const { return abil < kAbilityCount ? abilSoundUseCalls_[abil] : 0; }
    unsigned long long abilSoundOffCalls(uint8_t abil) const { return abil < kAbilityCount ? abilSoundOffCalls_[abil] : 0; }
    // 주인 없는 둘의 갈래(⑮) — 둘 다 몬스터 / 한쪽이 몬스터가 아님(덫 화살·환경)
    // ★ 09-20 묶음 10 — **능력 소리**. thing.bin `ABIL` 칸에서 읽어 넣는다(`main.cpp`).
    //   정답지 `spells_abil.go:225~226` 는 쓸 때 `getSound(능력,0)`, 꺼질 때 `getSound(능력,2)` 를 낸다.
    //   전에는 아예 안 보내서 ⑧ 「능력 소리」가 사람 몫으로 남아 있었다(들을 것이 없었다)
    void SetAbilitySound(uint8_t abil, uint16_t useSnd, uint16_t offSnd) {
        if (abil == 0 || abil >= 6) return;
        abilSoundUse_[abil] = useSnd;
        abilSoundOff_[abil] = offSnd;
    }
    uint16_t AbilitySoundUse(uint8_t abil) const { return abil < 6 ? abilSoundUse_[abil] : 0; }
    // ★ 09-23 — 합계는 능력별 부름의 합(쓸 때 + 꺼질 때)
    unsigned long long abilSoundSent() const {
        unsigned long long n = 0;
        for (int a = 0; a < kAbilityCount; ++a) n += abilSoundUseCalls_[a] + abilSoundOffCalls_[a];
        return n;
    }
    unsigned long long wildBothMonster() const { return wildBothMonster_; }
    unsigned long long wildRootNotMonster() const { return wildRootNotMonster_; }
    unsigned long long spawnFromMap() const { return spawnFromMap_; }
    unsigned long long spawnFromDefault() const { return spawnFromDefault_; }
    unsigned long long joinInfo97Ok() const { return joinInfo97Ok_; }
    unsigned long long joinInfo97Null() const { return joinInfo97Null_; }
    size_t acceptBodyLenLast() const { return acceptBodyLenLast_; }
    // 지금 불려 나와 있는 것의 수.
    size_t pets() const { return pets_.size(); }
    // 불려 나온 것이 죽은 횟수.
    unsigned long long petsKilled() const { return petsKilled_; }
    // ── 조각 D(09-16) 신설 계수기 — 「눈이 없어 못 재던 것」을 재는 자리 ──
    //   ★ 소리는 **셋으로 갈라 센다.** 합치면 「안 불렀다」와 「불렀는데 표가 비어 버렸다」와
    //     「담겼는데 거리에서 잘렸다」가 한 숫자로 뭉개져 묶음 10 전체가 판정 불능이 된다.
    unsigned long long soundsQueued() const { return soundsQueued_; }    // PlaySoundAt 이 받아 담은 수
    unsigned long long soundsDropped() const { return soundsDropped_; }  // 표가 비어 그 자리에서 버린 수
    unsigned long long soundsSent() const { return soundsSent_; }        // 실제로 와이어에 나간 수
    unsigned long long musicSent() const { return musicSent_; }
    size_t trapsArmed() const { return trapsArmed_; }      // 지금 적을 보고 있는 쏘는 덫 수
    // 버린 소리를 **번호별로** — 수만 알면 「정답지대로 버림」과 「엉뚱한 번호를 부름」이 안 갈린다
    const std::map<uint16_t, unsigned long long>& soundsDroppedIds() const { return soundsDroppedIds_; }
    // 성장 — 레벨·경험치·지금 든 주문 칸 수. 진단 스위치가 먹었는지 보는 유일한 자리다
    float DebugExperience(const std::string& a) const {
        auto i = sessions_.find(a); return i == sessions_.end() ? -1.0f : i->second.experience;
    }
    // ⚠ 주문 **칸 수**로는 못 가른다 — 온라인은 들어올 때 137 칸을 전부 3 으로 채운다
    //   (`serverSetAllSpells`). 「받았나」는 **그 번호의 레벨**로만 갈린다 → `DebugSpellLvl`.
    size_t petCount() const { return pets_.size(); }
    size_t petsWithPath() const;                           // 길을 들고 있는 크리처 수
    void SetTickMs(uint32_t ms) { tickMs_ = ms; }

    // 읽었지만 아직 답을 안 만드는 오프코드의 집계.
    const std::map<uint8_t, unsigned long long>& seen() const { return seen_; }
    unsigned long long handled() const { return handled_; }
    // 끝까지 못 쪼개 통째로 넘긴 조각 수. 아직 못 읽는 메시지가 섞였다는 뜻이다.
    unsigned long long stopped() const { return stopped_; }
    // **무엇이 막았나** — 길이를 몰라 멈춘 자리의 오프코드별 집계.
    // 이것이 다음에 무엇을 옮겨야 하는지 알려주는 나침반이다.
    const std::map<uint8_t, unsigned long long>& stoppedByOp() const { return stoppedByOp_; }

    // ── 마지막 조각을 어떻게 읽었나 (진단) ─────────────────
    //
    // ★ 왜 필요한가 — 「메시지 2개, 보낸 조각 0개」가 나오는데 **그 둘이 무엇인지
    //   알 길이 없었다**(2026-09-10). 조각을 버린 것도 아니고(`stoppedByOp` 0건)
    //   크래시도 아닌데 답만 없는 상태다. 그 자리에서 짐작만 쌓였다.
    // ⚠⚠ 열쇠 상태를 **같이** 봐야 한다 — 복호가 어긋나면 쓰레기 바이트가
    //   **그럴듯한 오프코드로 보인다.** 「메시지 N개」만 보면 속는다.
    const std::vector<uint8_t>& LastFragOps() const { return lastFragOps_; }
    bool IsKeyed(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.keyed;
    }
    uint8_t KeyOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.xorKey;
    }

    // 시전된 주문 — 번호별 횟수. 번호는 opennox `spell` 패키지 ID 와 같다.
    const std::map<uint8_t, unsigned long long>& spells() const { return spells_; }
    // 그중 자기에게 건 것 (마지막 자리가 1 인 것)
    unsigned long long spellsOnSelf() const { return spellsOnSelf_; }

private:
    Session& SessionFor(const std::string& addr);

    // 사거리 안에서 가장 가까운 **다른** 사람. 없으면 빈 문자열.
    std::string NearestOther(const std::string& self, double range) const;

    // ★ 사거리 안에서 가장 가까운 **적** — 사람이든 불려 나온 것이든.
    //
    // `friendOwner` 는 「내 편의 주인」이다. 사람이 때릴 때는 자기 주소,
    // 불려 나온 것이 때릴 때는 부른 사람의 주소를 넣는다. 그러면
    // **자기 자신도, 자기가 부른 것도** 대상에서 빠진다.
    //
    // ⚠ 「부른 사람의 공격에도 소환수가 안 맞는다」는 **근거 없이 정한 것**이다.
    //   덤프에 소환 표본이 아예 없어 진짜 녹스가 어떻게 하는지 모른다.
    //   자기 소환수를 자기가 때려 죽이는 것이 이상해 보여 그렇게 뒀을 뿐이다.
    Target NearestTarget(const std::string& friendOwner, double x, double y,
                         double range) const;

    // 고른 대상을 깎는다 — 정답지 **피해 함수(+716)** 자리(묶음 3-e, 2026-09-13).
    //   사람 = PlayerDamage(`GAME3_3.c:67`) → damageDefaultProc(`GAME3_2.c:7017`) → unitDamageClear = Damage()
    //   몬스터 = thing.bin DAMAGE(없으면 DefaultDamage) → damageDefaultProc → 체력 · 0 이면 KillPet
    //   `by` = 때린 쪽의 **주인 사람 주소**. 비었으면 주인 없는 것(맵 몬스터)이 때린 것으로 본다.
    //   `dtype` = 정답지 피해 종류 번호(0 칼 · 1 불 · 2 망치·철퇴 · 9 전기 · 10 맨손 …).
    //   `Damage()` 는 그대로 **날 피해**를 적용한다(시험이 정확한 값을 넣는 자리다).
    // ★ 4-c3b2b — 돌려주는 값 = 정답지 피해 함수의 돌려주는 값(`CallDamage` — `server/object.go:1503`). **거짓 = 피해가 안 들어감**:
    //   죽음·관전·되받아치기·방패/대검/지팡이 막기(`GAME3_3.c:122·161·191·202·288·322·338`) · 마법 방패가 다 흡수(`GAME3_2.c:7326`) ·
    //   화약 피해 함수의 종류 아님(`GAME3_3.c:680`). **무적·같은 편·면역은 참**이다(`GAME3_3.c:129` · `GAME3_2.c:7069·7100·7131~7146`)
    bool HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                    const std::string& by = std::string(), uint8_t dtype = 0, const AttackSrc* src = nullptr,
                    HitterRef hitter = HitterRef{});
    // ★ 4-c3b1 — 「때린 것」 도우미. 사람 몸 = NewPlayer 번호 · 주소가 사람이 아니면(몬스터가 때린 이 등) 아직 안 옮김(4-c3b3)
    HitterRef HitterPlayerBody() const { HitterRef h; h.kind = HitterRef::Type; h.type = newPlayerWire_; return h; }
    HitterRef HitterByAddr(const std::string& addr) const { return sessions_.count(addr) ? HitterPlayerBody() : HitterRef{}; }
    // ★ 4-c3b3b — 소환수(몬스터)면 때린 것·때린 이 둘 다 그 몸(감전 되갚기 a2 = a1 · 닿음 감전 a2 = a3 = a1)
    HitterRef HitterUnit(const Target& t) const {
        if (!t.addr.empty()) return HitterByAddr(t.addr);
        if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) return HitterMonster(pets_[static_cast<size_t>(t.pet)]);
        return HitterRef{};
    }
    // ★ 4-c3b3b — 몬스터가 제 몸으로 때림(소환수 근접 `GAME5.c:2529~2531` — a2 = a3 = 그 몬스터 · 주인 있는 소환수도 몬스터다)
    static HitterRef HitterMonster(const Creature& k) { HitterRef h; h.kind = HitterRef::Type; h.type = k.type; h.monsterA2 = k.type; return h; }
    // ★ 4-c3b3b — 때린 이가 몬스터이고 그 소리 모음의 맞히는 소리에 조각이 있나(맞으면 맞는 쪽 소리를 안 낸다)
    bool MonsterHitSoundOwns(const HitterRef& h) const {
        return h.monsterA2 != 0 && h.monsterA2 < monsterHitSound_.size() && monsterHitSound_[h.monsterA2] != 0;
    }
    static HitterRef HitterNone() { HitterRef h; h.kind = HitterRef::None; return h; }
    // ★ 4-c3b2 — 때린 것 = 그 오브젝트 자신(날아가는 것·놓인 것·맵 불 — 정답지 a3 = a1). 종류를 모르면(0) 안 옮김으로 센다
    static HitterRef HitterObj(uint16_t type) { HitterRef h; if (type) { h.kind = HitterRef::Type; h.type = type; } return h; }
    // 방패를 올릴 까닭(들어오는 투사체) — `GAME4_3.c:2586` monsterTestBlockShield_533E70
    bool IncomingMissile(const Session& s) const;
    // ── 죽음·부활 (묶음 3-c) ──
    void PlayerRespawn(Session& s, uint32_t frame, std::vector<OutPacket>& out);   // playerRespawn_4F7EF0 + playerMakeDefItems(,1,0)
    bool FindPlayerStart(const Session& s, float& x, float& y);                  // mapFindPlayerStart_4F7AB0
    void ForgetDrawables(Session& s, std::vector<OutPacket>& out);                // unitMove 의 플레이어 몫 `ea <프레임4>`
    // ── 알림 (묶음 3-d) ──
    void SendKillInform(Session& s, std::vector<OutPacket>& out);                 // diePlayer_54D2B0 의 `a9 0e`
    void PlayerReportAnything(Session& s, std::vector<OutPacket>& out);           // playerReportAnything_4D9900 — 방어 49 · +2184 dd/de/48 · 체력 43 · 마나 45
    void PlayerManaAdd(Session& s, int v);                                       // playerManaAdd_4EEB80 — +6 = 이전 · 최대로 자름
    void PlayerManaSub(Session& s, int v);                                       // playerManaSub_4EEBF0 — +6 = 이전 · 0 아래 안 감
    void PlayerManaRefresh(Session& s);                                          // playerManaRefresh_4EECF0 — +6 = 이전 · +4 = 최대
    void ReportHealthDeltaTo(Session& viewer, uint16_t netCode, uint16_t hp, uint32_t lastDamageFrame,
                             std::map<std::string, uint16_t>& seen, std::vector<OutPacket>& out);   // netPlayerObjSend_518C30 a4 갈래 → netReportHealthDelta_4D8760
    void TryCollide(Session& s, uint16_t code, std::vector<OutPacket>& out);      // sdecode `0x7B` MSG_TRY_COLLIDE
    uint16_t WeaponWireOf(const Session& s) const;
    // ── 시전 틀 (묶음 4-a) ──
    struct SpellDef {
        bool valid = false;
        bool enabled = true;             // spellIsEnabled — 서버 설정으로 끌 수 있다(기본 켜짐)
        uint8_t mana = 0;
        uint32_t flags = 0;
        std::vector<uint8_t> phonemes;   // 파일 음소 + End(4)
        // ★ 묶음 10-f — 주문 소리 셋(thing.bin `SPEL` 칸에 **이름**으로 있다).
        //   정답지 `spellGetAud44_424800` — 0 시전 · 1 켜짐 · 2 꺼짐
        uint16_t castSound = 0, onSound = 0, offSound = 0;
    };
    struct PhonemeNode {                 // server.PhonemeLeaf
        int ind = 0;                     // 끝이면 주문 번호
        int next[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    };
    struct CastEntry {                   // magicEnt (spellByBookInsert_4FE340)
        std::string caster;              // [1] — 사람 주소(몬스터 시전은 묶음 6)
        uint32_t spells[5] = {};         // [2..6]
        uint8_t slot = 0;                // +28 지금 주문 칸
        bool glyph = false;              // +29 글리프 섞임
        int leaf = 0;                    // +32 음소 나무 자리
        uint8_t phoneme = 0;             // +36 음소 번호
        uint32_t nextFrame = 0;          // +40
        uint32_t gap = 3;                // +44 음소 간격
        bool targetSelf = false;         // +48 (TRY_SPELL 끝 바이트)
    };
    struct SpellArg {                    // server.SpellAcceptArg
        Target obj;
        float x = 0, y = 0;
    };
    bool SpellByBookInsert(Session& s, const uint32_t spells[5], int count, uint32_t gap, bool targetSelf, std::vector<OutPacket>& out);
    void SpellCastByBook(uint32_t frame, std::vector<OutPacket>& out);
    void PlayerSpell(Session& s, std::vector<OutPacket>& out);                       // player.go:462
    bool CastSpellByUser(uint8_t spell, int lvl, Session& s, const SpellArg& arg, std::vector<OutPacket>& out);
    bool SpellAccept(uint8_t spell, Session& s, const std::string& obj3, const SpellArg& arg, int lvl, std::vector<OutPacket>& out);   // spells.go:193 · a2 = s · obj3 = 주인
    int SpellCheckClass(const Session& s, uint8_t spell) const;                        // sub_4FD0E0
    int CheckPlrCantCastSpell(const Session& s, uint8_t spell, bool glyph) const;      // checkPlrCantCastSpell_4FD150
    int SpellManaTake(Session& s, uint8_t spell, int costKind, std::vector<OutPacket>& out);   // sub_4FCF90
    void SpellManaGive(Session& s, int mana, std::vector<OutPacket>& out);             // sub_4FD030
    int SpellPower(const Session& s, uint8_t spell) const;                             // spellGetPower_4FE7B0
    void InformSpell(Session& s, uint8_t code, uint32_t val, std::vector<OutPacket>& out);   // netInformTextMsg 0/1
    void ReportSpellStat(Session& s, uint32_t spell, uint8_t stat, std::vector<OutPacket>& out);   // netReportSpellStat_4D9630
    void SendPriMsg(Session& s, const char* key, std::vector<OutPacket>& out);          // netPriMsgToPlayer_4DA2C0 `a9 0f`
    Target FindObjectAtCursor(const Session& s) const;                                 // findObjectAtCursor_54AF40
    bool TargetIsEnemy(const Session& s, const Target& t) const;                       // IsEnemyTo(사람, 대상)
    bool SpellEffectPlaceholder(uint8_t spell, Session& s, const SpellArg& arg, std::vector<OutPacket>& out);
    // ── 날아가는 오브젝트 (묶음 4-a2 · 정답지 Objs.MissileList · server_update.go:72 updateUnitsCallUpdate) ──
    struct MissileObj {
        uint16_t netCode = 0;
        uint16_t type = 0;
        float posX = 0, posY = 0;        // +56 PosVec
        float newX = 0, newY = 0;        // +64 NewPos
        float prevX = 0, prevY = 0;      // +72 PrevPos
        float velX = 0, velY = 0;        // +80 VelVec
        float forceX = 0, forceY = 0;    // +88 ForceVec
        float damping = 1.0f;            // +112 Float28(MISSILE 1.0)
        float speed = 0;                 // +544 SpeedCur
        uint8_t dir1 = 0, dir2 = 0;      // +124 Direction1 · +126 Direction2
        uint32_t createdFrame = 0;       // +128
        uint32_t frame136 = 0;           // +136
        bool destroyed = false;          // +16 & 0x20
        uint32_t flags = 0;              // +16 오브젝트 깃발(thing FLAGS — Magic 은 AIRBORNE)
        Buffs buffs;                     // 시전자 적외선(21) 복사 — CanSee(4-a4)
        std::string owner;               // +508 주인(사람 주소)
        // ★ 09-20 — **쏜 크리처**의 자리 번호(사람이 쏘면 −1). 주인 사슬의 뿌리를 가리는 데 쓴다 —
        //   덫 화살과 **몬스터가 쏜 화살**은 둘 다 `owner` 가 비어 있어 이것 없이는 못 가른다
        int ownerPet = -1;
        // SpellProjectileUpdateData(server/object_spell.go:5)
        std::string ud0;                 // [0] 시전자(되받아치면 바뀐다)
        Target ud1;                      // [1] 대상
        std::string ud2;                 // [2] 처음 시전자
        uint8_t spell = 0;               // [3]
        int level = 0;                   // [4]
        // ★ 닿음 함수 갈래(묶음 4-c3 날리는 것) — thing.bin `COLLIDE` 가 정한다.
        //   0 = SpellProjectileCollide(Magic — 대상에 닿으면 주문 효과) · 1 = SparkExplosionCollide(불덩이 — 터진다)
        uint8_t collideKind = 0;
        uint8_t collideAmount = 0;       // SparkExplosionCollide 의 인자(불덩이 192 · 센 불덩이·거인 불덩이 255)
        int fromPet = -1;                // 묶음 6-g(3b) — 이것을 쏜 소환수(제 몸에는 안 맞는다)
        // ★ 2 = PixieCollide — 적에게 닿으면 인자만큼 피해(종류 11 충격)를 주고 **제가 사라진다**
        uint32_t expireFrame = 0;        // 0 이 아니면 이 프레임에 사라진다(픽시 — 30~90초)
    };
public:
    // ★ 09-19 — ⑬(화살이 진행 방향으로 기울어 보이나)을 **표로** 가르려고 연다.
    //   눈대중 두 프레임으로는 「같은 화살인가」가 안 갈렸다(33ms 에 100px 넘게 간다).
    //   서버가 아는 `dir1` 과 화면의 기울기를 1:1 로 맞대는 것이 유일하게 싼 길이다.
    //   ⚠ 자리가 여기인 까닭 — `MissileObj` 선언 **뒤**여야 반환형이 잡힌다.
    const std::vector<MissileObj>& missileList() const { return missiles_; }
private:
    struct MagicTypeInfo { uint16_t type = 0; float speed = 0; float mass = 1.0f; uint32_t flags = 0; };
    // ── 묶음 4-c3 — 주문이 **놓는** 오브젝트(불꽃·독구름 …). 안 움직이고 수명이 다하면 사라진다
    //   정답지: newObjectWithTypeInd → createAt_4DAA50 → 수명(unitSetDecayTime_511660 또는 갱신자료 첫 칸)
    struct PlacedObj {
        uint16_t netCode = 0;
        uint16_t type = 0;           // 와이어 종류(이름 → SetNamedType 으로 받는다)
        float x = 0, y = 0;
        std::string owner;           // 놓은 사람 — 피해 임자 사슬(findParentChainPlayer)
        uint32_t life = 0;           // 갱신자료 첫 칸 — 매 틱 하나씩 줄고 0 이면 지운다
        uint32_t nextHitFrame = 0;   // +136 다음 판정 프레임
        uint8_t kind = 0;            // 주기 함수 갈래 — 1 = 독구름(ToxicCloudUpdate) · 2 = 닿음 피해(DamageCollide)
                                     //                 3 = 떨어지는 운석(MeteorUpdate) · 4 = 유성우 뿌리개(MeteorShowerUpdate)
                                     //                 5 = 떨어지는 주먹(FistUpdate + FistCollide)
                                     //                 6 = 바깥으로 나가는 불꽃(FlameCleanse)
                                     //                 7 = 거미 낳는 자리(ArachnaphobiaUpdate)
        int lastZSent = -1000;   // 09-20 — 마지막으로 알린 높이(같으면 안 보낸다)
        bool landed = false;         // +16 비트 31 — 주먹이 한 번 땅에 닿았나(닿음 피해는 그 뒤에만)
        // ★ kind 6 — 바깥으로 나가는 불꽃(정화의 불꽃). 놓인 것 가운데 **처음으로 움직인다**.
        float velX = 0.0f, velY = 0.0f;      // +80 — 매 틱 자리에 더한다(감쇠 0)
        float originX = 0.0f, originY = 0.0f;// +156 — 시작점. 여기서 선 추적(65)이 막히면 사라진다
        uint32_t expireFrame = 0;            // +136 — 이 프레임이 되면 사라진다
        uint8_t collideFn = 0;               // 0 = DamageCollide · 1 = ManaDrainCollide(마나를 깎는다)
        // ★ kind 3·4 — 운석. `castMeteor_52D9D0` 이 높이 255 에 띄우고 z 속도 −MeteorSpeed 를 준다.
        //   높이가 0 이하가 되면 `meteorExplode_53D6E0` 이 터뜨린다.
        //   ⚠ 일반 떨어짐 논리(updateFallLogic · 중력·튐)는 아직 없다(묶음 5·9) — 운석은 제 값만 쓴다.
        float z = 0.0f, zVel = 0.0f;
        int damage = 0;              // 갱신자료 첫 칸 — MeteorDamage[세기-1]
        uint32_t createdFrame = 0;   // +128 — 유성우가 「5초 지났나」를 잰다
        // ★ kind 2 — thing.bin `COLLIDE = DamageCollide <양> <피해종류>` 를 그대로 담는다(collideDamageLoad_536E10).
        //   불꽃(MediumFlame)은 `DamageCollide 2 DAMAGE_FLAME` → 양 2 · 종류 1.
        uint8_t dmgAmount = 0;
        uint8_t dmgType = 0;
        float radius = 0.0f;         // EXTENT CIRCLE — 닿음 판정 반지름(불꽃 6)
    };
    bool CreateSpellFly(Session& s, Target target, uint8_t spell, std::vector<OutPacket>& out);   // createSpellFly_4FDDA0
    Target SpellFlySearchTarget(bool havePos, float px, float py, float mslX, float mslY, const std::string& mslOwner,
                                const std::string& mslSelf, uint32_t sflags, float dist, const Session* self, const Buffs* mslBuffs) const;   // server/spells.go:360
    void UpdateMissiles(uint32_t frame, std::vector<OutPacket>& out);                                 // updateUnitsCallUpdate 의 투사체 몫
    void DestroyMissile(size_t i, std::vector<OutPacket>& out);                                       // DelayedDelete → netReportDestroyObject
    void MissileChangeOwner(MissileObj& m, const std::string& own, uint32_t frame);                 // changeOwner_52BE40
    void GameServerReadyMB(Session& s, std::vector<OutPacket>& out);                                // gameServerReadyMB_4DD180
    int CastConfuse(const std::string& obj3, const Target& targ, int lvl);                          // 4-b4 52C1E0
    int CastStun(const std::string& obj3, const Target& targ, int lvl);                             // 52C2C0
    int CastFumble(const Target& targ);                                                              // 52C060
    int CastInversion(const Session& a4, const std::string& obj3);                                  // 52BEB0
    int CastCurePoison(uint8_t spell, Session& a2, const Target& targ, int lvl, std::vector<OutPacket>& out);   // 52CDB0
    // ── 묶음 4-b5a 즉발 회복 ──
    int CastLesserHeal(uint8_t spell, Session& caster, const Target& targ, std::vector<OutPacket>& out);         // sub_52DD50
    int CastRestoreHealth(const Target& targ, std::vector<OutPacket>& out);                                       // castSpellWinkORrestoreHealth_52BF20
    int CastRestoreMana(const Target& targ);                                                                      // sub_52BF50

    // ── 묶음 4-a3 지속 주문 틀(server/spells_dur.go · spells_dur.go) ──
    struct DurSpell;
    struct DurFns {                                  // New 의 create · update · destroy(없으면 nullptr)
        int (Server::*create)(DurSpell&, std::vector<OutPacket>&) = nullptr;
        int (Server::*update)(DurSpell&, std::vector<OutPacket>&) = nullptr;
        void (Server::*destroy)(DurSpell&, std::vector<OutPacket>&) = nullptr;
    };
    struct DurSpell {
        uint16_t id = 0;                 // +0 NewRaw 가 매긴 번호
        uint8_t spell = 0;               // +4
        int level = 0;                   // +8
        std::string obj12;               // +12 처음 시전자(spellAccept a2)
        std::string caster;              // +16 Caster16 주인(obj3) — 사람 주소(몬스터 시전은 묶음 7)
        bool flag20 = false;             // +20 글리프가 건 것(4-e)
        float posX = 0, posY = 0;        // +28 Pos
        Target field36;                  // +36 먼젓번 빔 대상 — 바뀌면 옛 빔을 끄고 새 빔을 켠다(마나 빨기 52E210)
        Target target;                   // +48 Target48(sa.Obj)
        // ★ 연쇄 번개 하위 조각의 **시작 쪽**(+16 Caster16 이 유닛인 경우). 사람이 아닐 수도 있어 Target 으로 둔다.
        //   비어 있으면 `caster`(사람 주소)를 쓴다.
        Target fromUnit;
        float pos2X = 0, pos2Y = 0;      // +52 Pos2(sa.Pos)
        uint32_t frame60 = 0;            // +60 만든 프레임
        uint32_t frame64 = 0;            // +64
        uint32_t frame68 = 0;            // +68 끝 프레임(dt + 만든 프레임 · dt 0 이면 60 과 같아 끝이 없다)
        float field72 = 0.0f;            // +72 소수 이월(회복·흡수 계수가 0.5 같은 소수라 틱마다 남는 몫을 쌓는다)
        int pool = 0;                    // +18(u32) 방패가 막아 낼 남은 양(ShieldHealth). 0 이 되면 주문이 끝난다
        uint32_t field80 = 0, field84 = 0;
        uint32_t flags88 = 0;            // +88 1 = 취소됨(다음 차례에 지운다)
        uint16_t casterPet = 0;          // ★ 09-29 B7 — 몬스터 시전자(번호) — 0 이면 사람(`caster`)
        DurFns fns;
        std::vector<DurSpell> sub108;    // +108 하위(연쇄 번개 조각 · 4-d)
    };
    bool DurNew(uint8_t spell, const std::string& a2, const std::string& obj3, bool u3Glyph, const SpellArg& arg, int lvl,
                const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out);                          // spellsDuration.New
    // ★ 09-29 B7 — 몬스터 시전(Server.cpp 「B7」 절)
    bool MonsterDurNew(Creature& k, uint8_t spell, const Target& target, const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out);
    void DurCancelForPet(uint8_t spell, uint16_t net, std::vector<OutPacket>& out);
    bool MonsterCastPick(Creature& k, uint32_t mask, const Target& target, uint32_t& nextFrame, int lo, int hi, CastStat stat);
    void MonsterCast(Creature& k, uint8_t spell, const Target& target);                                   // `MonsterCast` object_monster.go:569
    void MonsterActionCast(Creature& k, int a2, std::vector<OutPacket>& out);                             // `mobActionCast_5413B0`
    bool MonsterCastSpellNow(Creature& k, uint8_t spell, const Target& target, float x, float y, std::vector<OutPacket>& out);   // `mobCast_541300` + `castSpell`
    Target EnergyBoltSearchPet(const Creature& k, float range) const;
    bool MonsterHasSpell(const Creature& k, uint8_t spell) const { for (const auto& s : k.spells) if (s.first == spell) return true; return false; }
    int PetIndexByNet(uint16_t net) const { for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == net) return static_cast<int>(i); return -1; }
    void DurOnNewSpell(std::vector<OutPacket>& out);                                                    // onNewSpell
    void DurDestroy(std::list<DurSpell>::iterator it, std::vector<OutPacket>& out);                     // destroyDurSpell
    void DurCancel(DurSpell& d, std::vector<OutPacket>& out);                                           // CancelSpell(4FE9D0)
    void DurCancelFor(uint8_t spell, const std::string& obj, std::vector<OutPacket>& out);              // CancelFor(4FEB10)
    void DurCancelOffensiveFor(const std::string& obj, std::vector<OutPacket>& out);                    // CancelOffensiveFor(4FF310)
    bool DurActive(uint8_t spell, const std::string& obj) const;                                        // Sub4FEE50
    void PlayerCancelSpells(const std::string& obj, std::vector<OutPacket>& out);                       // playerCancelSpells_4FEAE0
    void CancelChannelSpells(const std::string& obj, std::vector<OutPacket>& out);                      // cancelAllSpells_4FEE90
    const DurSpell* DurFindByTarget(uint8_t spell, const Target& target) const;                         // sub_4FF2D0
    void NetStopRaySpell(const DurSpell& d, const Target& who, std::vector<OutPacket>& out);            // NetStopRaySpell
    void SpellCastByPlayer(uint32_t frame, std::vector<OutPacket>& out);                                // spellCastByPlayer(틱)
    // ── 묶음 4-b5b 지속 회복·흡수 ──
    int DurChannelLife(DurSpell& d, std::vector<OutPacket>& out);                                       // sub_52F460
    void NetStartRaySpell(const DurSpell& d, std::vector<OutPacket>& out);                              // netStartDurationRaySpell_4FF130
    // 묶음 5-f 플라스마(59) — 완드를 들고 있어야 돌고, 틱마다 충전을 하나 먹는다
    int DurPlasmaCreate(DurSpell& d, std::vector<OutPacket>& out);
    int DurPlasmaTick(DurSpell& d, std::vector<OutPacket>& out);
    void DurPlasmaDestroy(DurSpell& d, std::vector<OutPacket>& out);
    static int HeldWandIndex(const Session& s);
    Target PlasmaSearch(const Session& s, float range) const;
    int DurGreaterHealCreate(DurSpell& d, std::vector<OutPacket>& out);                                 // sub_52F220
    int DurGreaterHealTick(DurSpell& d, std::vector<OutPacket>& out);                                   // sub_52F2E0
    int DurDrainMana(DurSpell& d, std::vector<OutPacket>& out);                                         // nox_xxx_spellDrainMana_52E210
    // ── 묶음 4-c1 밀기·당기기·땅흔들기 ──
    void PushUnitsAround(float cx, float cy, float outer, float inner, float power);                    // mapPushUnitsAround_52E040 + unitPushAroundFn_52E0E0
    int CastPushPull(const Session& caster, int lvl, bool pull);                                        // castPush_52C000 · castPull_52BFA0
    int CastEarthquake(Session& caster, int lvl, std::vector<OutPacket>& out);                          // castEquake_52DE40 + equakeDamage_52DEC0
    void SendJiggle(float cx, float cy, int jiggle, std::vector<OutPacket>& out);                       // earthquakeSend_4D9110 → `97 <세기>` Kind1
    // ── 묶음 4-c3 놓는 오브젝트 ──
    // 수명으로 안 지우는 것(운석처럼 제 조건으로 사라지는 것)에 쓰는 값.
    static constexpr uint32_t kPlacedNoLife = 0xFFFFFFFFu;
    // 놓은 것을 가리킨다. 못 놓았으면 nullptr. ⚠ 돌려준 포인터는 **다음 PlaceObject 까지만** 쓸 것(벡터가 옮겨 앉는다).
    PlacedObj* PlaceObject(uint16_t type, float x, float y, const std::string& owner, uint32_t life, uint8_t kind,
                           uint8_t dmgAmount = 0, uint8_t dmgType = 0, float radius = 0.0f);
    void UpdatePlaced(std::vector<OutPacket>& out);                                                      // 놓인 것들의 주기 함수 + 수명
    void DestroyPlaced(size_t i, std::vector<OutPacket>& out);
    int CastToxicCloud(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);               // castToxicCloud_52DB60
    int CastBurn(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);                     // castBurn_52C3E0
    int CastFireball(Session& caster, int lvl, std::vector<OutPacket>& out);                             // castFireball_52C790
    int CastMeteor(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out);          // castMeteor_52D9D0
    int CastMeteorShower(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out);    // castMeteorShower_52D8A0
    void MeteorExplode(PlacedObj& o, std::vector<OutPacket>& out);                                       // meteorExplode_53D6E0
    int CastFist(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out);            // castFist_52D3C0
    int CastCleansingFlame(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out);        // spellCastCleansingFlame_52D5C0
    int CastArachnaphobia(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);            // spellArachna_52DC80
    int CastPixieSwarm(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out);            // castPixies_540440
    int CastDeathRay(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);                // castDeathRay(spell_death_ray.go:19)
    // ── 묶음 4-f 소환 ──
    // SummonSize — thing.bin 몬스터 갈래로 정한다: 작은 것 1 · 중간 2 · 큰 것 4(`object_monster.go:652`).
    //   모르는 종류는 1 로 본다.
    static constexpr int kSummonLimit = 4;   // summonLimit(`spell_summon.go:13`) — 크기 합 상한
    int SummonSizeOf(uint16_t type) const;
    // countControlledCreatures_500D10 — 그 사람이 **거느린** 것들의 크기 합.
    int CountControlledCreatures(const std::string& owner) const;
    int DurSummonCreate(DurSpell& d, std::vector<OutPacket>& out);                                     // summonStart_500DA0
    int DurSummonTick(DurSpell& d, std::vector<OutPacket>& out);                                       // summonFinish_5010D0
    void ReportAcquireCreature(Session& owner, const Creature& k, std::vector<OutPacket>& out);        // ★ 10-01 netReportAcquireCreature_4D91A0
    int CastMark(uint8_t spell, Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);    // sub_52CBD0
    int DurTeleMarkCreate(DurSpell& d, std::vector<OutPacket>& out);                                   // sub_5305D0
    int DurTeleMarkTick(DurSpell& d, std::vector<OutPacket>& out);                                     // sub_530650
    int DurTelePopCreate(DurSpell& d, std::vector<OutPacket>& out);                                    // castTele_530820
    int DurTelePopTick(DurSpell& d, std::vector<OutPacket>& out);                                      // sub_530880
    void SendTeleportFx(float ox, float oy, float nx, float ny, std::vector<OutPacket>& out);          // netSendPointFx 137 두 번
    int DurBlinkCreate(DurSpell& d, std::vector<OutPacket>& out);                                      // spellBlink2_530310
    int DurBlinkTick(DurSpell& d, std::vector<OutPacket>& out);                                        // spellBlink1_530380
    // teleportToMB_4E7190 — 그 자리로 옮긴다(물리 자리·옛 자리칸 둘 다).
    // ⚠ 사람을 옮기면 `out` 을 꼭 넘길 것 — `ea FORGET_DRAWABLES` 가 나가야 **제 캐릭터가 그려진다**
    //   (09-20 실측 · 구현부 주석). 펫만 옮기는 자리는 null 이어도 된다
    void TeleportTarget(const Target& t, float x, float y, std::vector<OutPacket>* out = nullptr);
    int DurShieldCreate(DurSpell& d, std::vector<OutPacket>& out);                                     // castShield1_52F5A0
    int DurShieldTick(DurSpell& d, std::vector<OutPacket>& out);                                       // sub_52F650
    void DurShieldDestroy(DurSpell& d, std::vector<OutPacket>& out);                                   // sub_52F670
    // unitShield_52F690 — 방패 주머니에서 `amount` 를 뺀다. 다 쓰면 그 주문을 취소한다.
    void ShieldDrain(const Target& t, int amount, std::vector<OutPacket>& out);
    // unitShieldReduceDamage_52F710 — 맞을 때 끼어들어 피해를 깎는다. 0 이 되면 아예 안 맞은 것이 된다.
    void ShieldReduceDamage(const Target& t, int& dmg, uint8_t dtype, const AttackSrc* src, std::vector<OutPacket>& out);
    // ── 묶음 4-d 지속 공격 ──
    // 대상의 자리. 없으면 false — 사람과 소환수를 한 자리에서 가른다.
    bool TargetXY(const Target& t, float& x, float& y) const {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            if (it == sessions_.end() || !it->second.inGame) return false;
            x = static_cast<float>(it->second.posX); y = static_cast<float>(it->second.posY);
            return true;
        }
        if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return false;
        x = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x);
        y = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y);
        return true;
    }
    Target EnergyBoltSearch(const Session& caster, float range) const;                                   // spellEnergyBoltSetTarget_52EC60
    int DurEnergyBoltCreate(DurSpell& d, std::vector<OutPacket>& out);                                   // spellEnergyBoltStop_52E820
    int DurEnergyBoltTick(DurSpell& d, std::vector<OutPacket>& out);                                     // spellEnergyBoltTick_52E850
    // lightningCanAttackCheck_52FF10 — `from` 자리에서 둘레 안 **가장 가까운 적**. 이미 고른 것(`taken`)은 건너뛴다.
    //   ⚠ 에너지볼트와 달리 **앞쪽만 보는 검사가 없다** — 아무 쪽이나 걸린다.
    Target LightningSearch(const Session& owner, const Target& from, float range,
                           const std::vector<Target>& taken) const;
    int DurChainLightningCreate(DurSpell& d, std::vector<OutPacket>& out);                               // onStartLightning_52F820
    int DurChainLightningTick(DurSpell& d, std::vector<OutPacket>& out);                                 // onFrameLightning_52F8A0
    void SpawnCreature(uint16_t type, float x, float y, const std::string& owner);                       // newObjectWithTypeInd + createAt
    void PlacedCollide(PlacedObj& o, std::vector<OutPacket>& out);                                       // 놓인 것의 닿음 함수(피해·마나 빨기)
    // mapDamageUnitsAround_4E25B0(`game.go:1407`) — 안쪽 r2 까지는 온전, 바깥 rr 까지 선형으로 준다.
    //   skip 은 이미 직접 맞은 것(두 번 때리지 않는다). 벽 너머(선 추적 1)는 안 닿는다.
    void DamageUnitsAround(float cx, float cy, float r1, float r2, int dmg, uint8_t dtype,
                           const std::string& by, const Target& skip, std::vector<OutPacket>& out,
                           HitterRef hitter = HitterRef{},    // ★ 4-c3b1 — 정답지 `who`(부르는 쪽마다 다르다 — 표)
                           const AttackSrc* whoSrc = nullptr,  // ★ 09-27 S1 — 피해 함수의 v9(= a3 없음 → who) · 막기 판정·때린 이 자리
                           const WallHitter& wallWho = WallHitter{});   // ★ 09-29 Z1-A — 끝의 벽 피해가 보는 who
    void FireballExplode(MissileObj& m, const Target& hit, std::vector<OutPacket>& out);                 // fireballCollide_4E9AC0
    // ── 묶음 4-c2a 독 ──
    uint8_t* PoisonOf(const Target& t);                                                                  // +540 자리
    bool ActivatePoison(const Target& t, int add, int cap, std::vector<OutPacket>& out);                 // activatePoison_4EE7E0
    void SetPoisonAmount(const Target& t, int amount, std::vector<OutPacket>& out);                      // setSomePoisonData_4EEA90
    void UpdatePoison(const Target& t, int amount, std::vector<OutPacket>& out);                         // updatePoison_4EE8F0
    void RemovePoison(const Target& t, std::vector<OutPacket>& out);                                     // removePoison_4EE9D0
    // 묶음 5-b — 가방에서 물약 쓰기(`usePotion_53EF70`). 하나라도 걸렸으면 참(= 물약이 없어진다)
    bool UsePotion(Session& s, const Session::Carried& c, std::vector<OutPacket>& out);
    // 묶음 5-c — 줍는 순간(`pickupPotion_4F37D0`). −1 못 줍는다 · 0 그냥 가방 · 1 그 자리에서 마셨다
    int PickupPotion(Session& s, const WorldStatic& w, std::vector<OutPacket>& out);
    // 묶음 5-d — 먹기(`useConsume_53EE10`) · 버섯(`useMushroom_53ECE0`) · 사이다(`useCiderConfuse_53EF00`)
    bool UseConsume(Session& s, const Session::Carried& c, std::vector<OutPacket>& out);
    bool UseMushroom(Session& s, std::vector<OutPacket>& out);
    bool UseCiderConfuse(Session& s, const Session::Carried& c, std::vector<OutPacket>& out);
    // 묶음 5-e — 완드로 주문 걸기(`useWandCastSpell_53F4F0`). 충전을 깎으므로 **가리킴**을 받는다
    bool UseWandCast(Session& s, Session::Carried& c, std::vector<OutPacket>& out);
    void SendCharges(Session& s, const Session::Carried& c, std::vector<OutPacket>& out);   // netReportCharges_4D82B0
    // 묶음 5-h — `4a REPORT_GOLD` = `<금 u32>` 5바이트(중요). sub_4D8870(`GAME3_2.c:4150`)
    // ── 묶음 6-a AI 행동 쌓기 ──
    void AIActionReset(Creature& k);   // 6-c — 프레임을 적으므로 정적이 아니다
    bool AIPush(Creature& k, uint8_t act, const Target& targ = Target{}, float x = 0, float y = 0);
    int  AIPop(Creature& k);
    void AITick(Creature& k, std::vector<OutPacket>& out);
    static bool AIHasAction(const Creature& k, uint8_t act);
    static uint8_t AIActionBelow(const Creature& k);   // sub_50A040 — 조건을 건너뛴 아래 행동
    void CreatureApplyVelocity(Creature& k);
    // 묶음 6-i — 소환수 명령
    void OrderUnit(Session& src, uint16_t netCode, uint8_t order);
    void EnactUnitOrder(Session& src, Creature& k, uint8_t order);
    void AIClearStack(Creature& k);
    // 묶음 6-g(3b) — 크리처가 `MISSILE_NAME` 오브젝트를 만들어 날린다
    bool SpawnCreatureMissile(Creature& k, const char* name, float tx, float ty, std::vector<OutPacket>& out);   // ★ 09-26 M4a — 만들었나
    bool MonsterSound(const Creature& k, int field);   // ★ 09-26 M4a — 소리 모음 칸 하나를 몬스터 자리에서 모두에게 · 냈으면 참
    void GiveDefaultGear(Session& s);   // ★ 09-27 B1 — 직업 기본 장비를 가방에(스냅샷 꼬리가 보내는 번호·순서 그대로)
    // ★ 09-27 B3 — 무기 들기 `53A420`(a3 = 알림 · a4 = 거절 소리) · 내려놓기(B4 — `53A140`) · 입기·벗기 알림(본인 0x60/0x61 + 남에게 겉모습)
    bool PlayerEquipWeapon(Session& s, uint16_t nc, bool failSound, std::vector<OutPacket>& out);
    // ★ 09-29 — 갑옷 입기·벗기·방어값(Server.cpp 「갑옷」 절)
    bool PlayerEquipArmor(Session& s, uint16_t nc, bool failSound, std::vector<OutPacket>& out);            // `playerEquipArmor_53E650`
    bool PlayerDequipArmor(Session& s, uint16_t nc, std::vector<OutPacket>& out, bool toSelf = true, bool toOthers = true);   // `sub_53E430`
    float ArmorValueOf(const Session& s) const;                                                             // `recalculateArmorVal_53E300`
    static bool IsArmorItem(const Session::Carried& c) { return c.equipBit != 0 && !c.isWeapon && !c.weaponClass; }
    bool PlayerDequipWeapon(Session& s, uint16_t nc, std::vector<OutPacket>& out, bool toSelf = true, bool toOthers = true);
    // ★ 09-29 활·석궁·화살통 — 가방 머리부터(우리 가방은 뒤에서부터) 첫 것 하나만 본다(정답지 고리가 첫 것에서 멈춘다)
    bool EquipBowFromBag(Session& s, std::vector<OutPacket>& out);                          // `sub_53A680` — 활·석궁(비트 0xC)을 듦
    void DequipQuiver(Session& s, bool toSelf, bool toOthers, std::vector<OutPacket>& out); // `sub_53A0F0` — 든 화살통을 내려놓음
    bool TryReloadQuiver(Session& s, std::vector<OutPacket>& out);                          // `playerTryReloadQuiver_539FF0` — 화살통(비트 2)을 듦
    bool ShootBowCrossbow(Session& s, Session::Carried& bow, std::vector<OutPacket>& out);  // `shootBowCrossbow1_539BD0`
    void ShootArrow(Session& s, Session::Carried* quiver, uint32_t kind, std::vector<OutPacket>& out);   // `shootBowCrossbow2_539D80`
    int ArrowDamage(const std::string& owner, uint16_t arrowType) const;                     // `calcBoltDamage_4EF1E0` + 0.5 버림
    void PlayWeaponPickupSound(const Session& s, uint16_t snd);                               // `sub_53A6C0`
    // toSelf = 거짓이면 본인 알림(0x60/0x61)은 빼고 남에게 겉모습만(버리기 `sub_4ED0C0` 가 a3 = 0 으로 부른다)
    // toOthers = 거짓이면 남에게 겉모습도 안 보낸다(09-28 B4b — 죽은 사람이 떨어뜨릴 때 a4 = 0)
    void ReportEquipChange(Session& s, const Session::Carried& c, bool wear, std::vector<OutPacket>& out, bool toSelf = true, bool toOthers = true);
    // ★ 09-28 B4b — 가방 `at` 번째를 (tx,ty) 바닥에 떨군다(`dropDefault_4ED290` 의 벗기·`4d`·가방에서 빼기·세계에 놓기·사라짐 시계).
    //   dead = 죽은 사람이면 벗을 때 남에게도 안 알린다(`GAME3_3.c:6321~6323`)
    void DropCarriedToWorld(Session& s, size_t at, double tx, double ty, bool dead, std::vector<OutPacket>& out);
    // ★ 09-28 B4b — 죽을 때 가방 떨어뜨리기(`nox_xxx_dropAllItems_4EDA40` · 입은 평상복만 남김)
    void DropAllItems(Session& s, std::vector<OutPacket>& out);
    // ★ 09-28 B4b — 부활 때 가방 정리 + 기본 장비 새로(`playerMakeDefItems_4EF7D0` 의 a3 = 0 갈래 · `GAME3_3.c:7969~8043`)
    void RespawnGear(Session& s, std::vector<OutPacket>& out);
    // ★ 09-29 — 입은 물건 하나의 겉모습 메시지(`sub_4D82F0` `GAME3_2.c:3841~3947`)를 buf 에 짓고 길이(0 = 무기·갑옷 아님)
    static size_t BuildWornLook(const Session& who, const Session::Carried& c, uint8_t buf[11]);
    // ★ 09-29 — 새로 온 사람에게 먼저 온 사람의 입은 것 전부(`sub_4DDE10` `GAME3_2.c:5670~5693`)
    void SendWornLooks(Session& to, const Session& who, std::vector<OutPacket>& out);
    void AIStart(Creature& k, uint8_t act, std::vector<OutPacket>& out);
    void AIUpdate(Creature& k, uint8_t act, std::vector<OutPacket>& out);
    void AIEnd(Creature& k, uint8_t act);
    void AICancel(Creature& k, uint8_t act);
    // 묶음 6-d — 한 점을 바라보게 돌린다(`sub_545240`). 다 봤으면 참
    bool AIFacePoint(Creature& k, float tx, float ty);
    // 묶음 6-f(1) — 길 위의 점 하나를 골라 그쪽으로 속도를 준다. 다 왔으면 참
    bool CreatureActuallyMove(Creature& k);
    // 묶음 6-f(2) — 길 내기·다시 내기
    static constexpr size_t kCreaturePathMax = 32;   // `MonsterUpdateData.Path [32]` — 실제로는 31개만 담긴다
    int  CreatureGenPath(Creature& k, float tx, float ty);
    void CreatureSetDetailedPath(Creature& k, float tx, float ty);
    int  CreatureSetMovePath(Creature& k, float tx, float ty);
    // 묶음 6-f(3) — 한 틱: 길을 내고 그 속도로 자리를 옮긴다. 다 왔으면 참
    bool CreatureTickMove(Creature& k, double tx, double ty);
    // 묶음 6-f(5) — 아무 데나 걷기(29)
    // 묶음 6-g(2b) — 때릴 수 있나를 보고 조건 사슬을 쌓는다
    void MonsterPushAttack(Creature& k, const Target& foe, float fx, float fy);
    uint8_t AIRandomWalkDir(Creature& k);
    void AIRandomWalk(Creature& k);
    void AIPathResetIndex();
    AIIndexNode* AIPathIndexAt(int x, int y);
    uint16_t AIPathFlagsAt(int x, int y);
    bool AIPathBlockedFlags(bool airborne, int x, int y);
    bool AIPathBlockedByObj(bool airborne, uint32_t subclass, int x, int y);
    bool AIPathCanPass(bool airborne, uint32_t subclass, int x, int y);
    void AIPathIndexObjects();
    void AIPathMaybeIndexObjects();
    void AIPathInvalidate(bool alsoFrame);
    void AIPathBakeWalls();
    // 묶음 6-j(2) — 길목 칸을 격자에 찍고(0x40), 거기서부터 물결로 찾는다
    bool BodyContains(const PhysBody& o, float qx, float qy) const;   // sub_547DB0
    void AIPathBakeObstacles();   // 묶음 6-e(2b·3) — 오브젝트 모양이 깃발 0x1(·0x2)을 찍는다
    void AIPathBakeWaypoints();
    // ★ 09-20 — **자리가 아니라 길목 번호**를 돌려준다(0 = 못 찾음).
    //   전에는 물결이 닿은 **칸 한가운데**를 줬는데, 길목의 진짜 자리와 최대 16 어긋나
    //   도착 판정(거리² ≤ 64 = 8)이 영영 안 선다. 정답지도 `wp+8`(길목의 자리)을 쓴다
    uint32_t AIPathFindWaypoint(const Creature& k, float& outX, float& outY);
    const Waypoint* WaypointById(uint32_t id) const;               // sub_547EE0 의 「있나」
    void RoamRemember(Creature& k, uint32_t id);                   // sub_545B00
    uint32_t RoamNextWaypoint(const Creature& k, uint32_t cur);    // sub_545C60
    // 묶음 6-e(2b) — 그 칸 벽의 방향 번호(0~10, 없으면 −1) · 대각 이음매 판정
    int  WallDirAt(int x, int y) const;
    bool AIPathDiagOk(int ii, int x1, int y1) const;
    bool MapTraceObstacles(int skipObstacle, float x1, float y1, float x2, float y2) const;
    void AIPathResetVisit();
    int  AIPathNewVisit(int x, int y, int parent, int next);
    void AIPathCollect(int node, bool hasExact, float ex, float ey);
    int  AIPathFind(bool awayFrom, const PathActor& act, float fromX, float fromY,
                    float toX, float toY, const PathFilter& fnc, int maxDepth);
    // 묶음 6-b — 쌓임에 붙은 **조건**을 위에서부터 본다. 틀리면 그 아래까지 빼 버린다
    void AIProcDeps(Creature& k, std::vector<OutPacket>& out);
    // 09-28 — 쌓임 항목의 자리를 대상 오브젝트 자리로 고쳐 적는다(`nox_xxx_mobAction_50A910`)
    void AIRefreshStackArgs(Creature& k);
    bool CanInteractFrom(const Creature& k, const Target& t) const;
    bool CanSeeFrom(const Creature& k, const Target& t) const;     // ★ 09-29 B1 — `CanSee`(버프만)
    // ★ 09-29 B1 — 알아보기(Server.cpp 「B1 알아보기」 절)
    void UpdateSight(Creature& k);
    bool SightResolve(const Creature::SeenRef& r, Target& t) const;
    bool TargetDeadOrGone(const Target& t) const;
    bool CreatureEnemyTo(const Creature& k, const Target& t) const;
    void SightLose(Creature& k, size_t i, SightStat why);
    void SightSeeEnemy(Creature& k, const Target& t);
    void SightPickEnemy(Creature& k);
    // ★ 09-29 B2 — 놀기·되갚기·소리 넷(Server.cpp 「B2」 절)
    void NoteMonsterStruck(const AttackSrc* src);
    void MonsterHeadUpdate(Creature& k);
    bool MonsterLookAtDamager(Creature& k);
    int  MonsterRetaliate(Creature& k);
    void MonsterIdleUpdate(Creature& k);
    // ★ 09-29 B3 — 근접 다시 고르기(Server.cpp 「B3」 절)
    Target MonsterPickMeleeTarget(const Creature& k, double meleeRange) const;
    void ObjectApplyForce(float fromX, float fromY, const Target& t, double force);
    int  MonsterStrike(Creature& k, const MeleeStrikeInfo& si, int dmg, std::vector<OutPacket>& out);
    // ★ 09-29 B4 — 두뇌 반응 층(Server.cpp 「B4」 절)
    void MonsterBrain(Creature& k, std::vector<OutPacket>& out);
    void MonsterPopAttackActions(Creature& k);
    int  CreatureGenRetreatPath(Creature& k, float fromX, float fromY);
    bool MonsterCanResumeAttack(const Creature& k) const;
    bool MonsterCheckDodgeables(Creature& k);                 // ★ B4b 옆걸음(`547C50`)
    Target MonsterEnemyAggro(const Creature& k, float r) const;   // ★ B4b `EnemyAggroXxx`
    // ★ 09-29 B5 — 애니 칸·걸음 소리(Server.cpp 「B5」 절)
    int  MonsterAnimIndex(const Creature& k) const;          // `mobActionToAnimation_533790`
    void MonsterAnimUpdate(Creature& k);                      // `updateNPCAnimData_50A850`
    void MonsterMoveAudio(Creature& k, AnimStat site);        // `monsterMoveAudio_534030`
    // ★ 09-29 B6 — 소리 듣기(Server.cpp 「B6」 절)
    struct ListenRec { uint16_t snd = 0; std::string srcAddr; uint16_t srcNet = 0; bool srcIsPet = false; float x = 0, y = 0; uint32_t frame = 0; };
    void MonsterListen(Creature& k);                          // `aiListenToSounds`
    bool MonsterShouldListen(const Creature& k, const ListenRec& r);   // `shouldUnitListen`
    int  MonsterTraceSound(const Creature& k, const ListenRec& r);     // `traceSound`
    bool MonsterHearReact(Creature& k);                       // `Sub_5466F0`
    bool MonsterLookAtHeard(Creature& k);                     // `sub_545DA0`
    bool LocationIsSafe(const Creature& k, float x, float y) const;   // 조건 69 · `unitIsDangerous_547120`
    uint32_t MonsterSubclass(uint16_t wire) const { auto it = monsterSubclass_.find(wire); return it == monsterSubclass_.end() ? 0u : it->second; }
    // 크리처(사본이어도 — 번호로 찾는다)를 대상 꼴로 · 목록에 없으면 빈 것
    Target PetTarget(const Creature& k) const {
        for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == k.netCode) return Target{ std::string(), static_cast<int>(i) };
        return Target{};
    }
    void SendGold(Session& s, std::vector<OutPacket>& out);
    // 묶음 7-a — 경험치 알림·주기·레벨 오름
    // 묶음 7-d — 능력(0xCD·0xCE·0xCF·0xEB)
    void SendAbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out);
    void SendAbilityState(Session& s, uint8_t abil, uint8_t state, std::vector<OutPacket>& out);
    void SendAbilityActive(Session& s, uint8_t abil, bool active, std::vector<OutPacket>& out);
    void SendAbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    bool AbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out);
    bool AbilityIsActive(const Session& s, uint8_t abil) const;
    // ★ 2-a1 — `IsActiveVal`(목록에 있고 켜짐 값이 서 있나) · `Sub4FC440`(값만 내린다)
    bool AbilityIsActiveVal(const Session& s, uint8_t abil) const {
        return abil < kAbilityCount && s.abilActive[abil] && s.abilActiveVal[abil];
    }
    void AbilityValOff(Session& s, uint8_t abil) {
        if (abil < kAbilityCount && s.abilActive[abil] && s.abilActiveVal[abil]) { s.abilActiveVal[abil] = false; ++abilValOff_; }
    }
    unsigned long long abilValOff_ = 0;        // ★ 2-a1 — 켜짐 값을 내린 횟수(함성 애니 끝)
    unsigned long long warcryShouts_ = 0;      // ★ 2-a1 — 함성 애니가 칸 2 → 3 을 넘은 틱(외침 효과가 붙을 자리)
    unsigned long long warcryStunned_ = 0;     // ★ 2-a2 — 외침에 붙잡힘(5)이 걸린 몬스터 수
    unsigned long long berserkChargeTicks_ = 0;   // ★ 2-b1 — 광전사가 앞으로 민 틱
    unsigned long long berserkCollideCalls_ = 0;  // ★ 2-b2
    unsigned long long deathPass_[2] = {};        // ★ 09-26 M2 — 안 넘김(시간 남음) · 넘김
    unsigned long long lavaHurts_ = 0;            // ★ 09-26 M3 — 바닥 칸 6(용암) 짝으로 피해 함수를 부른 수
    unsigned long long lavaHurtsPet_ = 0;         // ★ 09-27 S2 — 소환수·맵 몬스터 몫
    unsigned long long gearGiven_[2] = {};        // ★ 09-27 B1 — [틀 없어 못 실음, 실음]
    unsigned long long weaponNoStats_ = 0;        // ★ 09-27 B2 — 수치 표에 없는 무기로 판정 칸에 닿아 판정 없이 넘긴 수
    unsigned long long equipGate_[kEquipGateCount] = {};   // ★ 09-27 B3 — 무기 들기 관문별
    unsigned long long dequipGate_[kDequipGateCount] = {}; // ★ 09-27 B4 — 무기 내려놓기 나간 길별
    unsigned long long dropGate_[kDropGateCount] = {};     // ★ 09-28 — 버리기 나간 길별
    unsigned long long decayGate_[kDecayGateCount] = {};   // ★ 09-28 — 버릴 때 사라짐 시계 갈래별
    unsigned long long decayExpired_ = 0;                  // ★ 09-28 — 시계가 다 돼 지운 물건 수
    unsigned long long decayShadowSent_ = 0;               // ★ 09-28 — 그때 보낸 `33 OBJECT_IN_SHADOWS` 건수
    unsigned long long deathDropKept_ = 0, deathDropSpiral_ = 0, deathDropAtBody_ = 0;   // ★ 09-28 B4b 죽을 때 갈래별
    unsigned long long respawnGearDeleted_ = 0, respawnGearGiven_ = 0, respawnGearNoTemplate_ = 0, respawnGearNotPorted_ = 0;   // ★ 09-28 B4b 부활 때
    std::map<uint16_t, float> itemRadius_;                 // ★ 09-28 B4b 물건 종류 → 몸 반지름
    unsigned long long wornLookSent_ = 0, wornLookSkipped_ = 0;   // ★ 09-29 새로 온 사람에게 보낸 입은 것 겉모습 · 비트 몰라 뺀 것
    unsigned long long introRedrawSent_ = 0;                      // ★ 09-29 먼저 온 사람에게 보낸 새 사람의 `e9 … ff 01`
    unsigned long long autoEquip_[5] = {};                 // ★ 09-28 — 줍고 저절로 입음 갈래별(autoEquip 주석)
    unsigned long long wandChargesOnEquip_ = 0, twoHandShieldOff_ = 0, wandSpellCut_ = 0;   // ★ 09-27 B5 — 들 때 충전 알림 · 두 손 무기가 벗긴 방패 · 내려놓으며 끊은 지속 주문
    unsigned long long shieldDropsTwoHand_ = 0;   // ★ 09-28 — 방패를 입어 내려놓게 한 두 손 무기(`sub_53E600`)
    struct WeaponDef { uint8_t classes = 0; uint16_t reqStrength = 0; uint16_t damageMin = 0; float coeff = 0.0f; };
    unsigned long long bowShot_[kBowShotCount] = {};       // ★ 09-29
    unsigned long long arrowHit_[kArrowHitCount] = {};     // ★ 09-29
    unsigned long long weaponPick_[kWeaponPickCount] = {}; // ★ 09-29
    unsigned long long quiverFromMap_ = 0;                 // ★ 09-29
    unsigned long long bowPoseEnded_ = 0;                  // ★ 09-29 — 활 쏜 뒤 상태 14 → 13(1초)
    unsigned long long connectTailStops_ = 0;              // ★ 09-29 ③ — 게임 패킷 안 0x00 에서 읽기를 끝낸 수(전에는 0x04 로 답함)
    std::vector<uint16_t> giveItems_;                      // ★ 09-30 사람 판 시험용
    unsigned long long testItemsGiven_ = 0, testItemsNoTemplate_ = 0;
    void GiveTestItems(std::vector<OutPacket>& out);
    std::map<uint16_t, WeaponDef> weaponDef_;     // ★ 09-27 B3 — 종류 번호 → modifier.bin 무기 정의
    struct ArmorDef { uint8_t classes = 0; uint16_t reqStrength = 0; float value = 0.0f; };
    std::map<uint16_t, ArmorDef> armorDef_;       // ★ 09-29 — 종류 번호 → modifier.bin 갑옷 정의
    unsigned long long armorGate_[kArmorGateCount] = {};
    std::map<uint16_t, Session::Carried> itemTemplate_;   // ★ 09-27 B1 — 기본 장비 종류 틀
    unsigned long long jumpCodes_ = 0, jumpIn_ = 0, jumpGate_[kJumpGateCount] = {};   // ★ 3-a
    unsigned long long jumpForceTicks_ = 0, jumpHeldTicks_ = 0, jumpLanded_ = 0, airborneOffIdle_ = 0, recoilBlockedAirborne_ = 0;
    unsigned long long equakeSkipAirborne_ = 0, spellRefusedAirborne_ = 0;   // ★ 3-b
    unsigned long long swingSound_[6] = {};   // ★ 4-a — 휘두르기 소리 879~884 부른 수(번호 − 879)
    unsigned long long hitSoundCalls_ = 0, hitSoundWay_[kHitSoundWayCount] = {};   // ★ 4-c2
    uint16_t lastHitSound_ = 0;
    unsigned long long defHitSoundCalls_ = 0, defHitSoundWay_[kDefHitSoundWayCount] = {};   // ★ 4-c3a
    // ★ 4-c3a — `soundDefaultDamageSound_532E20` 를 소환수에 — 맞는 쪽 = 소환수 종류 재질 · 피해 종류(+524) · 때린 것(무기, 없으면 사람 몸)
    void PetDamageSound(const Creature& k, uint8_t dtype, HitterRef hitter);
    // ★ 4-c3b2b — 작살이 끊길 때 따로 내는 기본 타격 소리 `532E20(대상, 작살 화살)`. 맞는 쪽 재질 = 대상 오브젝트의 재질(사람은 NewPlayer ·
    //   가슴갑옷을 안 본다) · 피해 종류 = 대상의 +524(마지막 피해 종류 — 이번에 피해가 안 들어갔으면 **그 전 값**)
    void HarpoonDefaultHitSound(const Target& t, uint16_t boltType);
    unsigned long long harpoonHitSoundCalls_ = 0, harpoonHitSoundWay_[kDefHitSoundWayCount] = {};
    std::vector<uint32_t> matByWire_;
    std::vector<uint8_t> breastByWire_;
    std::vector<uint8_t> monsterHitSound_;   // ★ 4-c3b3b
    // ★ 09-26 M4a — 소리 모음 칸(0~17 · ThingDb `kSoundSetFieldNames` 순서)별 「종류 번호 → 소리 번호」(0 = 없음).
    //   칸 7(맞히는 소리)은 09-24 H1 의 표다(전에는 `monsterMeleeHitId_` 따로)
    std::vector<uint16_t> monsterSnd_[18];
    unsigned long long monsterSndCount_[18][2] = {};   // ★ 09-26 M4a — 칸별 [모음에없음, 냄]
    unsigned long long monsterMeleeHitPlayed_ = 0, monsterMeleeHitNone_ = 0;   // ★ 09-24 H1 — 냄 · 모음에 없음
    // ★ 4-d4 — 발밑 바닥과 발소리 계수기(달리기·걷기 × 바닥 종류) · 살금살금으로 안 낸 걷기 소리 · 달릴 때 힘쓰는 소리
    FloorGrid floorGrid_;
    std::vector<FloorGrid::EdgeDef> floorEdges_;
    std::vector<int32_t> floorMaterial_;
    unsigned long long footRun_[kFootKindCount] = {}, footWalk_[kFootKindCount] = {};
    unsigned long long footTreadSilent_ = 0, exertionLight_ = 0;
    unsigned long long hurtVoice_[kHurtVoiceCount] = {};   // ★ 09-24 「윽」
    void Footstep(const Session& s, bool run);
    uint16_t newPlayerWire_ = 0;
    // ★ 4-c2 — `nox_xxx_soundPlayerDamageSound_5328B0` — 맞는 사람의 가슴갑옷 재질(없으면 FLESH) × 때린 것(무기, 없으면 때린 이) 재질
    void PlayerDamageSound(const Session& victim, HitterRef hitter);
    // ★ 3-a — 사람의 오브젝트 깃발(+16). NewPlayer 는 FLAGS NULL 이고 지금 켜는 것은 AIRBORNE(0x4000) 하나다
    static uint32_t PlayerObjFlags(const Session& s) { return s.airborne ? 0x4000u : 0u; }
    uint32_t moveFrame_ = 0;                      // ★ 2-b2 — UpdatePlayerMovement 의 이 틱 프레임
    unsigned long long berserkHit_[kBerserkHitCount] = {};
    mutable unsigned long long wallContacts_ = 0;
    // ★ 2-b2 — `nox_xxx_unitMove_4E7010` 의 사람 몫(`object.go:693~731` SetPos) — 속도는 **그대로 둔다**
    void PlayerSetPos(Session& s, double x, double y, std::vector<OutPacket>& out);
    void WarcryShout(Session& s, std::vector<OutPacket>& out);   // ★ 2-a2 — 외침 효과(땅울림 · 몬스터 기절)
    int CastCounterSpell(Session& caster, std::vector<OutPacket>& out);   // ★ 2-a3 — castCounterSpell_52BBB0
    std::set<uint16_t> counterMissileTypes_;
    std::set<uint16_t> castingMonsterTypes_;   // ★ 2-a4
    unsigned long long warcryCastsBroken_ = 0, warcryNullMon_ = 0;
    unsigned long long warcryPeer_[kWarcryPeerCount] = {};   // ★ 09-24 H3
    unsigned long long enchantSentAnti_ = 0;                  // ★ 09-24 H3
    void WarcryBreakCasts(Session& s, double dist, std::vector<OutPacket>& out);   // ★ 2-a4 — nox_xxx_spell_4FE680
    unsigned long long counterCasts_ = 0, counterMissiles_ = 0, counterDurs_ = 0, counterEnchants_ = 0;
    bool AbilityDo(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    void AbilityEffect(Session& s, uint8_t abil, std::vector<OutPacket>& out);   // 7-e `do()`
    // 묶음 8-g — 판이 끝나는 나머지 길
    void RoundTimeUp(std::vector<OutPacket>& out);
    void CheckVictorySurvivors(std::vector<OutPacket>& out);

    // 묶음 8-f — 모드별 죽음 셈
    bool IsCrown(const Session& s) const;
    void KotrDeath(Session& victim, Session* killer, std::vector<OutPacket>& out);
    void ElimDeath(Session& victim, Session* killer, std::vector<OutPacket>& out);
    void DeathScoring(Session& victim, std::vector<OutPacket>& out);

    // ── 묶음 9-a 비밀벽 여닫기 ────────────────────────────────────────
    //
    // 정답지 — 마디 짜임 `mapRWSecretWalls_4297C0`(`GAME1_2.c:1064`) ·
    //   상태 기계 `nox_xxx_updateUnits_51B100_D`(`server__system__server.c:913`) ·
    //   사람별 알림 `sub_519660`(`server.go:598`) · 보내기 `sub_4DF120`·`sub_4DF180`(`GAME3_2.c:6108·6127`) ·
    //   여닫기 명령 `Wall.open`·`close`·`toggle`(`src/wall.go:152~202`, 스크립트 `WallOpen` 따위).
    //
    // 상태 — **1 닫힘 · 2 닫는 중 · 3 열림 · 4 여는 중**.
    //   · 여는 명령: 상태가 3·4 가 아니면 **4**(여는 중)로 · 닫는 명령: 1·2 가 아니면 **2**(닫는 중)로
    //   · 4 는 애니 칸을 **올려** 23 에 닿으면 3 · 2 는 **내려** 0 에 닿으면 1.
    //     둘 다 도착할 때 남은 프레임을 `기다릴 초 × 30` 으로 채운다
    //   · 1 에서 깃발 **4 와 8 이 둘 다** 있으면 남은 프레임을 세어 0 에서 스스로 4 로
    //   · 3 에서 깃발 **4 는 있고 8 은 없으면** 마찬가지로 스스로 2 로
    // 알림 — **1·4 는 닫힘 · 2·3 은 열림**. 즉 애니가 **끝난 뒤에야** 반대쪽이 나간다
    struct SecretWall {
        uint8_t  gx = 0, gy = 0;
        uint16_t id = 0;              // 벽 `+10` — 읽은 차례
        uint32_t waitSecs = 0;        // +16
        uint8_t  flags = 0;           // +20 — 2 선 추적 통과 · 4 저절로 · 8 열린 상태
        uint8_t  state = 1;           // +21
        uint8_t  anim = 0;            // +22 (0~23)
        uint32_t countdown = 0;       // +24 — 남은 프레임
    };
    std::vector<SecretWall> secretWalls_;
    void SendOpenWall(Session& to, uint16_t id, std::vector<OutPacket>& out);
    void SendCloseWall(Session& to, uint16_t id, std::vector<OutPacket>& out);
    void SecretWallUpdate(std::vector<OutPacket>& out);
    void SecretWallSync(Session& s, std::vector<OutPacket>& out);
    void SecretWallSetState(SecretWall& w, uint8_t st);
    SecretWall* SecretWallAt(int gx, int gy);

    // 묶음 9-b — 쏘는 덫(화살 덫·해골)
    struct ShootingTrap {
        float x = 0, y = 0;
        uint8_t facing = 0;
        std::string proj;
        uint8_t fxKind = 0;
        int damage = 0;
        uint32_t scanCountdown = 0;    // +0 — 0 이면 다시 살핀다(살핀 뒤 30 프레임)
        uint32_t fireCountdown = 0;    // +4 — 쏜 뒤 30 프레임
        bool armed = false;            // +8 — 지금 겨눈 것이 있나
        bool wasEnabled = false;       // +48 — 지난 프레임에 켜져 있었나
        bool enabled = true;           // 오브젝트 깃발 ENABLED(0x1000000 · 비트 24)
    };
    std::vector<ShootingTrap> traps_;
    void ShootingTrapUpdate(std::vector<OutPacket>& out);
    bool TrapSeesEnemy(const ShootingTrap& t) const;
    void TrapShoot(ShootingTrap& t, std::vector<OutPacket>& out);
    void SendArrowTrapFx(float x, float y, uint8_t kind, std::vector<OutPacket>& out);

    // 묶음 10-a — 소리
    struct SoundDef { int maxDist = 0; uint8_t merged = 0; int samples = 0; uint16_t flags = 0; };
    std::vector<SoundDef> soundDefs_;          // 색인 = 소리 번호(0 은 NULL)
    struct AudioEvent {
        uint16_t sound = 0;
        float x = 0, y = 0;
        uint8_t kind = 0;
        uint32_t code = 0;
        std::string owner;                     // 이 소리의 임자(사람 주소) — 있으면 그 사람에게는 0xA7
    };
    std::vector<AudioEvent> audioEvents_;
    int SoundFadePerc(uint16_t id, float sx, float sy, double px, double py) const;
    void FlushAudioEvents(std::vector<OutPacket>& out);

    std::string sysopPassword_;          // 10-e — 비어 있으면 늘 틀린 것으로 본다

    // 묶음 8-e — 투표(0xEE)
    struct Vote {
        uint8_t kind = 0;                 // [0] 0·1 강퇴 · 2 퀘스트 다시 시작 · 3 퀘스트에서 내보내기
        std::string target;               // [28] 겨눈 사람(갈래 2 는 없음)
        std::vector<std::string> voters;  // [8] 표를 던진 사람들 · [4] 는 그 개수
        uint8_t team = 0;                 // [16] 낸 사람의 팀
        bool teamOnly = false;            // [20] 같은 팀만 셀 것인가
        uint32_t startFrame = 0;          // [24]
    };
    void SendVoteGui(uint8_t val, std::vector<OutPacket>& out);
    void SendVoteClose(Session& s, std::vector<OutPacket>& out);
    Vote* FindVote(uint8_t kind, const std::string& target);
    void RemoveVote(size_t i, std::vector<OutPacket>& out);
    bool VotePasses(const Vote& v) const;
    void VoteCast(Session& voter, uint8_t kind, const std::string& targetName,
                  std::vector<OutPacket>& out);
    void VoteWithdraw(Session& voter, uint8_t kind, const std::string& targetName,
                      std::vector<OutPacket>& out);
    void VoteDropPlayer(const std::string& who, std::vector<OutPacket>& out);
    void VoteUpdate(std::vector<OutPacket>& out);
    void KickPlayer(const std::string& addr, std::vector<OutPacket>& out);

    // 묶음 8-d — 플래그볼
    void SendBallStatus(std::vector<OutPacket>& out);
    void SendFlagBallWinner(uint8_t teamId, std::vector<OutPacket>& out);
    void DropBall(const std::string& who, std::vector<OutPacket>& out);
    void UpdateBall(std::vector<OutPacket>& out);
    uint8_t TeamAfter(uint8_t teamId) const;

    // 묶음 8-c — 판 끝 알림(0x58 혼자 · 0x59 팀)
    void SendRoundWinner(uint8_t op, uint16_t who, uint8_t reason, std::vector<OutPacket>& out);
    void RoundCheckLimit(std::vector<OutPacket>& out);

    // 묶음 8-b — 팀 메시지(0xC4). 팀 기록은 정답지 팀 오브젝트의 칸을 그대로 옮긴 것
    struct Team {
        uint8_t  id = 0;          // +57 팀 번호
        int32_t  score = 0;       // +52 팀 점수(`netChangeTeamID` 가 놓는다)
        uint8_t  titleIdx = 0;    // +56 칭호 번호 — 클라가 이것으로 기본 이름을 고른다
        uint32_t field60 = 0;     // +60 뜻 미상(클라는 `sub_418830` 으로 넣기만 한다)
    };
    void SendTeamMsg(const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    void SendTeamCreate(const Team& t, std::vector<OutPacket>& out);
    void SendTeamAddMember(uint8_t teamId, uint16_t netCode, uint16_t spriteType,
                           std::vector<OutPacket>& out);
    void SendTeamRemove(uint8_t teamId, std::vector<OutPacket>& out);
    void SendTeamScore(uint8_t teamId, int32_t score, std::vector<OutPacket>& out);
    Team* TeamById(uint8_t id);
    void TeamChangeScore(uint8_t teamId, int32_t score, std::vector<OutPacket>& out);
    void SendTeamState(std::vector<OutPacket>& out);

    // 묶음 7-h — 안내서(0xD1)
    void SendGuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out);
    bool GuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out);
    // 묶음 7-f — 작살
    void SendHarpoonFx(uint8_t code, uint16_t n1, uint16_t n2, std::vector<OutPacket>& out);
    MissileObj* HarpoonBoltOf(const Session& s);
    void HarpoonDo(Session& s, std::vector<OutPacket>& out);
    void HarpoonBreak(Session& s, bool emitSound, HarpoonEnd why, std::vector<OutPacket>& out);
    bool HarpoonCollide(MissileObj& m, const Target& targ, std::vector<OutPacket>& out);
    void HarpoonUpdate(Session& s, std::vector<OutPacket>& out);                 // 7-g
    void HarpoonPull(const Session& owner, const Target& targ, float tx, float ty);
    void AbilityUpdate(Session& s, std::vector<OutPacket>& out);
    void AbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    void AbilityCancelAll(Session& s, std::vector<OutPacket>& out);
    void AbilityDisable(Session& s, uint8_t abil, std::vector<OutPacket>& out);   // ★ 1-a `serverAbilities::DisableAbility`
    // 묶음 7-c — 점수·죽은 횟수(0x4E)
    void SendLesson(Session& s, std::vector<OutPacket>& out);
    void ChangeScore(Session& s, int32_t delta, std::vector<OutPacket>& out);
    void IncrementDeaths(Session& s, std::vector<OutPacket>& out);
    void PlayerUpdateScore(Session& victim, std::vector<OutPacket>& out);
    // 묶음 7-b — 주문 받기(0x6F)
    void SendSpellAward(Session& s, uint8_t spell, uint8_t announce, bool extra,
                        std::vector<OutPacket>& out);
    bool SpellGrantToPlayer(Session& s, uint8_t spell, uint8_t announce, bool extra,
                            uint8_t forceLevel, std::vector<OutPacket>& out);
    void SendExperience(Session& s, std::vector<OutPacket>& out);
    bool PlayerCheckLevel(Session& s);
    void PlayerGiveExp(Session& s, float amount, std::vector<OutPacket>& out);
    void PlayerAddGold(Session& s, uint32_t v, std::vector<OutPacket>& out);   // playerAddGold_4FA590
    void PlayerSubGold(Session& s, uint32_t v, std::vector<OutPacket>& out);   // playerSubGold_4FA5D0
    void PoisonTick(std::vector<OutPacket>& out);                                                        // server.go:175~201 감쇠 + 피해
    int CastPoison(const std::string& obj3, const Target& targ, int lvl, std::vector<OutPacket>& out);   // castPoison_52C720
    Target DrainSearch(const Session& caster) const;                                                     // sub_52E610 + sub_52E660 고르기
    int DrainApply(Session& caster, const Target& t, int amount);                                        // sub_52E450
    int TargetMaxHealth(const Target& t) const;                                                          // unitGetMaxHP_4EE7A0
    bool CanInteract(const Session& obj, const Target& t) const;                                         // unitCanInteractWith_5370E0 = CanSee + 벽 시야추적
    bool TargetJustDamaged(const Target& t) const;                                                       // sub_4E6BD0 — 지난 1틱 안에 맞았나
    int TargetManaCur(const Target& t) const;                                                            // unitGetOldMana_4EEC80(이름과 달리 **지금** 마나 · 소환수 1000)
    int TargetManaMax(const Target& t) const;                                                            // playerGetMaxMana_4EECB0(사람만 · 아니면 0)
    int TargetHealth(const Target& t) const;                                                             // unitGetHP_4EE780
    bool TargetMovedFrom(const Target& t, float x, float y) const;                                       // sub_4FEA70 — x·y 어느 쪽이든 5 이상 움직였나
    // ── 묶음 4-a4 버프 틀 ──
    Buffs* BuffsOf(const Target& t);
    const Buffs* BuffsOf(const Target& t) const;
    uint16_t UnitNetCode(const Target& t) const;
    void ApplyEnchant(const Target& t, int buff, int dur, int power);   // buffApplyTo_4FF380
    void BuffOff(const Target& t, int buff);                            // spellBuffOff_4FF5B0
    void ClearBuffs(const Target& t);                                   // unitClearBuffs_4FF580
    void SetBuffFlags(const Target& t, uint32_t bits);                  // setUnitBuffFlags_4E48F0
    void UpdateUnitBuffs(const Target& t, std::vector<OutPacket>& out); // updateUnitBuffs_4FF620(updateUnitsCCC)
    uint8_t ConfusedDirection(const Session& s, uint32_t frame) const;  // playerConfusedGetDirection_4F7A40
    // CanSee(server/object.go:76) — obj 가 눈멀면 못 봄 · flags&1 아니면 투명 targ 는 적외선·느리면(속도 ≤ 6) 못 봄
    static bool CanSeeBuffs(const Buffs* obj, const Buffs* targ, double targVelX, double targVelY, int flags);
    void SendFxAllCli(float x, float y, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);   // netSendFxAllCli_523030
    Target DamageAttacker(const std::string& by, const AttackSrc* src) const;                               // 4-b1 a2
    void DamageShockBack(const Target& victim, const Buffs& vb, const Target& attacker, const AttackSrc* src, std::vector<OutPacket>& out);
    int DamageProtect(const Buffs& b, uint8_t dtype, int amount) const;                                     // 4DFE40·4DFF40
    void DamageVampirism(const Target& attacker, const Target& victim, int amount, std::vector<OutPacket>& out);
    void UnitAdjustHP(const Target& t, int dv, std::vector<OutPacket>& out);                                 // unitAdjustHP_4EE460
    void MarkAssist(const std::string& orig, const Target& targ);        // sub_4E7540(+3600 도운 사람) — 효과 묶음에서
    int DebugDurCreate(DurSpell& d, std::vector<OutPacket>& out);                                      // 시험용 create/update/destroy
    int DebugDurUpdate(DurSpell& d, std::vector<OutPacket>& out);
    void DebugDurDestroyFn(DurSpell& d, std::vector<OutPacket>& out);

    // 플레이어 +0 ArmorEquip — 입은 갑옷의 장비 비트 합.
    //   ★ 09-27 B5 — 기본 장비를 가방에 실은 사람은 **가방에서 입은 것**의 비트 합(정답지는 입을 때 |= · 벗을 때 &= ~ — 같은 표 `object_armor.go` armorTable).
    //   두 손 무기가 방패를 벗기면(`53A3D0`) 방패 막기(0x3000000)가 따라 꺼지게. 기본 장비를 못 실은 시험 서버는 옛 방식(직업 표)
    static uint32_t ArmorEquipOf(const Session& s) {
        if (!s.gearInBag) return nox::ArmorEquipOf(s.charClass);
        uint32_t bits = 0;
        for (const auto& c : s.inventory) if (c.equipped && !c.isWeapon) bits |= c.equipBit;
        return bits;
    }
    // 두 쪽(주인 기준)이 적인가 — `server/object.go:1667` IsEnemyTo 를 사람·소환수·맵 몬스터에 맞춘 판.
    //   wild = 주인 없는 몬스터(자기 자신이 주인)
    // ★ 09-20 — 둘째·넷째 인자는 「주인 사슬의 **뿌리가 몬스터인가**」다(전에는 「주인이 없나」였다).
    //   정답지 `object.go:1737` 는 **둘 다 몬스터일 때만** 적이 아니라고 하고, 그 밖(덫 화살·환경 피해처럼
    //   주인도 없고 몬스터도 아닌 것)은 `1740` 에서 **적**이 된다. 유닛은 「주인 없음 = 맵 몬스터」라 같은 값이지만,
    //   **때린 것이 투사체면 다르다** — 그 자리에서 `src->byMonsterType` 으로 가른다
    // 투사체의 주인 사슬 뿌리가 몬스터인가(크리처가 쏜 것만 참 · 덫 화살은 거짓)
    bool MissileRootIsMonster(const MissileObj& m) const;
    // ★ 4-c3b2 — 날아가는 것이 맞힐 때의 「때린 것」. 때린 이(a2 = 주인 사슬 뿌리)가 몬스터면 정답지는 그 몬스터의 소리 모음을
    //   먼저 보고 소리를 안 낼 수도 있다(`GAME3_2.c:7252~7254`) — 그건 4-c3b3 에서 옮기므로 그때까지 안 옮김으로 센다
    //   ★ 4-c3b3b — 이제 옮김: 뿌리가 맵 몬스터면 때린 것 = 발사체 · 때린 이 = 그 몬스터(`FindOwnerChainPlayer` — 사람이 없으면 사슬 끝)
    HitterRef HitterMissile(const MissileObj& m) const {
        HitterRef h = HitterObj(m.type);
        if (MissileRootIsMonster(m)) h.monsterA2 = pets_[static_cast<size_t>(m.ownerPet)].type;
        return h;
    }
    bool OwnersEnemy(const std::string& a, bool aMonsterRoot, const std::string& b, bool bMonsterRoot) const;

    // 판 시계를 뿌린다 — `0xd3 TIMER_STATUS`. 짜임은 실측이다(PROTOCOL.md 「판 시계」).
    void SendTimer(std::vector<OutPacket>& out);

    // 깃발 알림 하나 — `0xa9 INFORM` 의 갈래.
    //
    // ⚠ 속 짜임은 **opennox 원본을 옮긴 것**이지 와이어로 대조한 것이 아니다 —
    //   `InformTeamPlayer`(msg_inform.go:253) 가 `PlayerID 4 + TeamID 4`, 본문 8.
    //   **깃발 갈래(5·6·7)는 우리 덤프에 0건**이라 맞춰 볼 거리가 없었다.
    void SendFlagInform(InformCode code, uint16_t playerNetCode, uint8_t team,
                        std::vector<OutPacket>& out);

    // 글자 한 줄을 모두에게 — `0xa8 TEXT_MESSAGE`. 짜임은 실측이다.
    //
    // ⚠ **문구는 우리가 지은 것**이다. 진짜 녹스는 시스템 문구를 `0xa9` 갈래 15 로
    //   **CSF 열쇠**만 보내고 클라가 `nox.csf` 에서 찾는다(PROTOCOL.md 「알림」 절).
    //   퀘스트 완료에 해당하는 열쇠를 모르므로, **아는 통로인 채팅**으로 대신 알린다.
    void SendNotice(const char* text, std::vector<OutPacket>& out);

    // 깃발을 들고 있으면 그 자리에 떨어뜨린다 (죽었을 때).
    void DropFlag(const std::string& who, std::vector<OutPacket>& out);

    // 깃발 줍기·득점을 살핀다. 틱마다 부른다.
    void UpdateFlags(std::vector<OutPacket>& out);

    // 불려 나온 것 하나를 없애고 모두에게 알린다.
    void KillPet(size_t idx, std::vector<OutPacket>& out);

    // `0xcc SEQ_IMPORTANT` 껍데기에 싸서 내보낸다.
    //
    // ★ 신뢰 전송은 **패킷 머리 비트가 아니라 이 껍데기**가 맡는다 — 실측이 그렇다.
    //   09-06 덤프에서 `0xcc` 581건이 **전수 비신뢰 패킷**에 실려 있었다.
    //   (머리의 신뢰 비트는 접속 절차에서만 쓴다)
    // 판 시계 한 통(13바이트)을 만든다. 주기 발송과 `0xd4` 즉답이 같은 내용을 쓴다.
    void BuildTimerStatus(uint8_t msg[13], uint32_t recipientFrame) const;   // 도장 = **받는 사람의 시계**(09-21)
    // ★ 09-21 — **그 사람이 믿고 있는 지금 프레임.** 클라 시계는 우리가 `27 TIMESTAMP` 로 보내는
    //   `tickFrame` 을 따른다. 그 시계가 **아직 안 섰으면**(첫 `27` 전) 전역 `frame_` 이 맞다.
    //   프레임을 와이어에 찍는 자리는 **전역이 아니라 이것**을 써야 한다(`BuildTimerStatus` 주석)
    uint32_t ClockOf(const Session& s) const { return s.tickFrameSet ? s.tickFrame : frame_; }

    // 한 사람이 한 말을 모두에게. 머리는 **서버가 새로 짓는다**(남의 번호로 말하기 방지).
    void BroadcastChat(const Session& who, const uint8_t* text, size_t n,
                       std::vector<OutPacket>& out);

    // 이 사람이 지금 겨누는 쪽(라디안). 겨눔이 없거나 낡았으면 몸 바라보는 쪽.
    double AimRadians(const Session& s) const;

    // ⚠ 2026-09-13 부터 **바로 안 보낸다** — 세션의 importantQ 에 쌓고 FlushImportant 가 싣는다.
    // ★ 09-20 묶음 10 — **높이 알림**. 정답지 `netReportUnitHeight_4D9020`(`GAME3_2.c:4518~4566`) 의
    //   **4바이트 갈래**만 옮긴다 — `5e/5f | 번호 u16 | 높이 u8`. 6바이트 갈래(`9f`)는 칸 하나(+116)가
    //   뜻 미상이라 안 쓴다(지어내지 않는다). 클라는 이 값을 그림의 높이(+104)에 그대로 넣는다
    // 낙하 시작 알림(`9f`) — 주문 물체(주먹·운석)는 이쪽이다. 세 칸 모두 우리가 아는 값
    uint16_t PropWireCode(const WorldProp& p) const;
    void SendDeltaZStart(uint16_t netCode, float z, float zVel, float bounce, std::vector<OutPacket>& out);
    // ★ 09-20 ⑰ — **만든 틱에 보내면 클라가 버린다.** 오브젝트를 알리는 `a4 UPDATE_STREAM` 은
    //   틱 끝에서 조립되는데 `9f` 는 주문 처리 중(틱 앞쪽)에 나가, 클라가 아직 없는 그림을 찾다 만다
    //   (`cdecode.c:1972` `if (v5)` — 못 찾으면 조용히 버린다). 그래서 **다음 틱 머리로 미룬다.**
    struct PendingZ { uint16_t netCode; float z, zVel, bounce; };
    std::vector<PendingZ> pendingDeltaZ_;
    void QueueDeltaZStart(uint16_t netCode, float z, float zVel, float bounce);
    void FlushDeltaZStart(std::vector<OutPacket>& out);
    void SendUnitHeight(uint16_t netCode, float z, std::vector<OutPacket>& out);
    void EmitSeq(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    // 중요 대기열에 봉투 없이(Packet0) 싣는다 — 사라짐(32)·그늘(33) 등.
    void EmitImportantRaw(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    void EmitImportantRawSupersede(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);   // sub_4E5450 — 같은 옵 대기 지우고 Packet0
    // 틱마다 부른다. 준비(CLIENT_READY) 뒤에만 `[27 시각][aa][cc·맨 메시지…]` 한 조각을 낸다.
    void FlushImportant(Session& s, std::vector<OutPacket>& out);
    // 사람 한 틱(묶음 2-a 걷기 · 3-a 공격) — 상태·힘·적분. Server.cpp 정의 머리 주석 참조.
    //   frame = 이 걸음의 프레임(Tick 이 여러 틱을 몰아 돌 때 걸음마다 다르다).
    void UpdatePlayerMovement(uint32_t frame, std::vector<OutPacket>& out);
    // ── 공격 (묶음 3-a 맨손 · 3-b 무기) — 정답지 이식. Server.cpp 정의 머리 주석 참조 ──
    // playerAttack 이 짓는 피해 꾸러미(`GAME4_3.c` v82 36바이트) 중 판정이 읽는 칸
    struct AttackInfo {
        int dmg = 0;              // +0 float 을 (long long)(f + 0.5) 한 값 — sub_538510 이 피해 함수에 넘기는 꼴
        uint8_t type = 0;         // +4 피해 종류(맨손 10 · 칼 0 · 망치·철퇴 2)
        float range = 0;          // +8
        float px = 0, py = 0;     // +16 판정 중심(망치만 앞 35)
        bool hitsOverlap = false; // +24 — 참이면 ALLOW_OVERLAP 오브젝트도 맞는다
        bool weapon = false;      // +28 무기 오브젝트가 있나 — 없을 때만 훅 힘
        uint8_t anim = 0;         // 훅(25) 가르기용
    };
    bool PlayerSetState(Session& s, uint8_t st, uint32_t frame, std::vector<OutPacket>* out = nullptr);   // playerSetState_4FA020 중 쓰이는 갈래(out — 타원 방패 취소 STAT)
    void PlayerInputAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out);   // playerInputAttack_4F9C70
    bool PlayerAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out);   // playerAttack_538960
    bool PlayerTraceAttack(Session& s, const AttackInfo& ai, std::vector<OutPacket>& out);   // 538330
    void ReportStamina(Session& s, std::vector<OutPacket>& out);             // netReportStamina_4D8800 `47 <기력>`
    void EarthquakeSend(float x, float y, int jiggle, std::vector<OutPacket>& out);   // earthquakeSend_4D9110 `97 <흔들림>`
    static uint32_t WeaponEquipOf(const Session& s);                          // 플레이어 +4 WeaponEquip (09-27 B2 — 든 무기 칸에서)
    static const Session::Carried* HeldWeapon(const Session& s);             // ★ 09-27 B2 — 무기 칸이 가리키는 가방 항목(없으면 nullptr)
    static const WeaponStats* WeaponStatsOf(const Session& s);
    bool CursorOnFriend(const Session& s) const;                             // playerAimsAtEnemy_4F9DC0 의 거꾸로
    bool MapTraceVision(float x1, float y1, float x2, float y2) const;       // `server/map.go:522` (장애물 그늘 · 벽 9)
    bool MapTraceVisionSkip(float x1, float y1, float x2, float y2, int skipObstacle) const;
    static uint8_t PlayerAnimByte(const Session& s);                          // mapPlrActionToStateId_4FA2B0
    static bool PlayerAnimFrameSent(const Session& s);                        // netPlayerObjSend_518C30 의 +236 싣는 상태
    // ── 벽 충돌 (묶음 2-b) — 정답지 이식. Server.cpp 정의 머리 주석 참조 ──
    // sub_57B500 — `sflags` 는 정답지의 셋째 인자다. **0x40 = 충돌**(문을 무시) · **0x10 = 걸음 추적**(문을 본다).
    //   기본값을 0x40 으로 둔 것은 부르는 자리 대부분이 밀어내기라서다(`sub_54FFC0` 도 64 로만 부른다).
    int WallShapeAt(int gx, int gy, uint8_t sflags = 0x40) const;
    struct TraceHit { float x = 0, y = 0; int gx = 0, gy = 0; };   // 4-a2 — 막힌 점과 칸(MapTraceRayAt outPos·outGrid)
    bool MapTraceRay(float x1, float y1, float x2, float y2, uint8_t flags, TraceHit* hit = nullptr) const;   // MapTraceRayAt
    bool MapTraceRayTile(int gx, int gy, float x1, float y1, float x2, float y2, uint8_t flags, float* hx = nullptr, float* hy = nullptr) const;
    // 원 대 벽(sub_54FEF0) — 사각형은 프레임 시작 자리(bx,by)로, 거리는 지금 자리(nx,ny)로 잰다.
    //   밀어낸 힘을 (ax,ay) 에 더하고 벽 쪽으로 가던 속도를 깎는다. 닿았으면 참.
    //   ★ 2-b2 — lastGx·lastGy 를 주면 **가장 가까운 점을 찾은 마지막 칸**을 적는다(정답지 +296 — 밀기 전에 적는다)
    bool CircleVsWalls(double bx, double by, double r, double mass,
                       double nx, double ny, double& vx, double& vy, double& ax, double& ay,
                       int* lastGx = nullptr, int* lastGy = nullptr) const;
    // 오브젝트끼리(sub_548220 거르기·나누기 → 원 대 원 550D00 · 원 대 상자 54AD50). 자기(a1)만 민다.
    //   a1 = 움직이는 사람(지금 자리 nx,ny · 프레임 시작 bx,by · 속도 vx,vy), a2 = 상대(자리·속도).
    // 4-b1 — 부딪힘 기록(collSysAddCollision_548630)의 임자. kind 1 사람 · 2 소환수 · 3 장애물
    //   ★ 2-b2 — kind 0 = **벽**(정답지 짝의 둘째가 0). 벽 짝은 a.collide(0) 만 부르고 버프 몫·되부르기가 없다(`GAME5.c:2055~2066`)
    struct ContactRef {
        uint8_t kind = 0;
        std::string addr;
        int idx = -1;
    };
    // ★ 2-b2 — `PlayerCollide`(`GAME3_3.c:3188~3253`)의 광전사 갈래. other 가 null 이면 벽
    void PlayerBerserkCollide(Session& s, const ContactRef* other, std::vector<OutPacket>& out);
    void PlayerCollide(Session& s, const ContactRef* other, std::vector<OutPacket>& out);   // ★ 09-26 M2 — 광전사 갈래 + LABEL_26(죽음 넘기기)
    void BodyVsBody(const PhysBody& a1, double nx, double ny, double bx, double by, double vx, double vy,
                    const PhysBody& a2, double ox, double oy, double obx, double oby, double ovx, double ovy,
                    double& ax, double& ay, const ContactRef* ra = nullptr, const ContactRef* rb = nullptr) const;
    void AddContact(const ContactRef& a, const ContactRef& b) const;               // collSysAddCollision_548630(겹 없이)
    void CollideBuffs(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out);   // nox_xxx_collide_4FDF90
    // ★ 09-23 nine-defects 1-d — a1 의 **닿음 함수**(+696)를 (a1, a2) 로. 지금은 장애물의 `DamageCollide` 만 옮겼다
    //   ★ 2-b2 — a1 이 사람이면 `PlayerCollide` 의 광전사 갈래
    void ContactCollide(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out);
    unsigned long long contactDamageHits_ = 0;   // 장애물 닿음 피해를 준 횟수(맵 불)
    mutable std::vector<std::pair<ContactRef, ContactRef>> contacts_;              // 이 틱 부딪힘(allocHitArray 가 비움)
    // `2d NEW_PLAYER` 한 통. 묶음 안 129바이트를 떼어 **번호와 상태만** 갈아 끼운다.
    // 상태는 kNewPlayerStatusAt 주석 참조 — 0 이라야 클라가 HUD 를 그린다.
    void EmitNewPlayer(Session& to, const Session& who, uint8_t status,
                       std::vector<OutPacket>& out);

    // `6a REPORT_CLIENT_STATUS` 한 통 — **한 사람의 상태를 「받는 쪽 한 명」에게 알린다.**
    //
    // ★★★ **이게 남의 몸을 그리게 하는 열쇠다** (2026-09-09 오픈녹스 원본 대조).
    //   클라의 사람 그리기(`drawable_player.go` `DrawPlayer`)는 **자기가 아닌 사람**에 대해
    //   `if pl.Field3680 & 0x1 { return }` — 즉 **1번 비트가 서 있으면 아무것도 안 그린다.**
    //   그 비트를 세우는 곳은 `2d NEW_PLAYER` 인데
    //   (`network_client.go`: `pl.Field3680 |= data[112:]`) **OR 라서 0 을 다시 보내도 안 꺼진다.**
    //   끄는 길은 `6a` 하나뿐이다 — `cdecode.c` case 106 이
    //   `Field3680 &= ~0x423` 한 뒤 `|= 상태 & 0x423` 로 **갈아 끼운다.**
    //   그 자리의 「내 번호냐」 검사는 **HUD 를 켤지에만** 걸리고 상태 갱신은 누구 것이든 한다.
    //
    // ⚠ 전에는 이걸 **본인에게만** 보냈다. 그래서 먼저 들어온 쪽에는 나중 사람의 1번 비트가
    //   영영 서 있었고, 실제로 **한쪽에서만 상대가 보였다**(09-09 사람 눈 판정과 맞는다).
    void EmitClientStatus(Session& to, uint16_t netCode, uint32_t status,
                          std::vector<OutPacket>& out);

    // 위를 **놀고 있는 모두**에게. 관전을 풀 때 쓴다.
    void BroadcastClientStatus(uint16_t netCode, uint32_t status,
                               std::vector<OutPacket>& out);

    // 한 통을 **놀고 있는 모두**에게. `viaSeq` 면 `0xcc` 껍데기에 싸서, 아니면 생으로.
    //
    // ★ 왜 필요한가 — 피해·죽음·부활을 **맞은 본인에게만** 보내고 있었다.
    //   그래서 남이 맞는 것도 죽는 것도 되살아나는 것도 화면에 안 나타났다.
    //   두루 보내는 자리마다 손으로 반복문을 쓰던 것을 이 하나로 모은다.
    //
    // ⚠⚠ **번호가 실린 메시지만 이 길을 쓴다.** `0x43 REPORT_PLAYER_HEALTH` 처럼
    //   누구 것인지 안 실리는 메시지를 태우면 받는 쪽이 **제 것으로 읽어**
    //   남의 체력바가 망가진다. 태워도 되는 것 =
    //   `0x42`·`0xDD`·`0xE8`·`0xE9`·`0x2E`·`0x31`·`0x32` (전부 첫 두 바이트가 번호다).
    void Broadcast(bool viaSeq, const uint8_t* msg, size_t n,
                   std::vector<OutPacket>& out);

    // 세계에서 정적 하나를 뺀다(주웠을 때). 없으면 false.
    bool TakeStatic(uint16_t netCode);
    void DecayStatics(std::vector<OutPacket>& out);   // ★ 09-28 버린 물건 사라짐(`decay_511750`)
    uint32_t DropDecayFrame(DropFn fn, DecayExempt ex);   // ★ 09-28 버릴 때 사라질 프레임(0 = 안 사라짐) · 갈래 계수

    // 녹음본 조각 안의 `cc` 껍데기 번호를 **이 세션의 이어달기 번호로 다시 매긴다.**
    //
    // ★★★ **이걸 안 하면 스냅샷 뒤에 보낸 신뢰 메시지가 통째로 사라진다.**
    //   클라는 `cc` 번호로 순서를 맞추고 **이미 지나간 번호를 조용히 버린다**
    //   (`GAME2_3.c` `sub_48D5A0` → `sub_48D4F0`: 지금 기다리는 번호보다 만 개 안쪽으로
    //    작으면 버림 · `sub_48D660`: 기다리는 번호와 같은 것만 꺼내 쓰고 하나씩 올림).
    //
    //   내 녹음본은 그 녹음 때의 번호(0 · 3~22)를 그대로 담고 있는데
    //   서버도 0 부터 매기니 **두 줄기가 겹친다.** 2026-09-09 실측 —
    //   나중에 들어온 쪽이 스냅샷 뒤에 받은 `2d`(#3)·`6a`(#4)가 스냅샷의 같은 번호에
    //   먼저 먹혀 **버려졌고**, 그래서 **그쪽 화면에서만 상대가 안 보였다.**
    //   (먼저 온 쪽은 번호가 이미 #41 까지 올라가 있어 멀쩡했다 — 증상이 한쪽만이던 까닭)
    //
    // ⚠ 녹음본의 번호에는 **구멍도 있다**(1·2번이 없다). 그대로 두면 클라가
    //   그 자리에서 **30초**를 기다린다(`sub_48D660` 의 `0x7530`). 다시 매기면 구멍도 없어진다.
    void RenumberSeq(Session& s, std::vector<uint8_t>& frag);

    // 머리 2바이트를 붙여 한 조각을 만든다. keyed 면 XOR 을 건다.
    // ★ 09-30 — 게임 안 사람에게 가는 비신뢰·스트림 0·게임 메시지(첫 번호 ≥ 0x20)는 곧바로 안 만들고 그 사람 묶음에 담는다
    //   (`BundleSlotOf` · 틱 끝 `FlushBundles`) — 정답지 한 프레임 한 데이터그램(`network_lists.go:177~227`)
    void Emit(Session& s, bool reliable, uint8_t stream,
              const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    // 묶지 않고 곧바로 한 조각 — 신뢰 · 접속 제어 · 채팅(정답지 `conn.Flush()` · `network_server.go:149~151`) · 서두 재생 · 묶음 내보내기
    void EmitNow(Session& s, bool reliable, uint8_t stream,
                 const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    // ★ 09-30 틱 끝 — 사람마다 묶음을 [27][a4][잡다][aa+중요] 차례로 한 데이터그램에 싣는다
    void FlushBundles(std::vector<OutPacket>& out);

    std::map<std::string, Session> sessions_;
    std::map<uint8_t, unsigned long long> seen_;
    unsigned long long handled_ = 0;   // 답을 만든 메시지 수
    uint32_t nextJoinOrder_ = 0;       // 접속한 차례에 매기는 번호 (팀 나누기용)
    uint32_t restartAtFrame_ = 0;      // 이 프레임에 판을 다시 연다. 0 이면 예약 없음
    unsigned long long roundsPlayed_ = 0;
    unsigned long long flagStatusSent_ = 0;   // 깃발 HUD 를 보낸 횟수 (과잉 감시용)
    unsigned long long invulnBlocked_ = 0;    // 부활 무적으로 막은 피해 횟수
    unsigned long long missilesMade_ = 0;     // 날아가는 것을 만든 누계
    unsigned long long missilesGone_ = 0;     // 날아가는 것이 사라진 누계
    unsigned long long missileEntriesSent_ = 0;   // 갱신에 실린 누계
    // ★ 09-19 — 「항목에 담았다」와 「와이어에 나갔다」를 가른다.
    //   `EncodeUpdate` 는 512B 버퍼에 못 담으면 **0 을 돌려주고**, 그러면 그 틱 갱신이
    //   통째로 안 나간다(`Server.cpp` 의 `if (n && sendUpdates_)`).
    //   화살 44발이 한꺼번에 뜨는 덫밭에서 이 둘이 갈리는지 보려고 심는다.
    unsigned long long updateEncOk_ = 0;      // 인코딩 성공(= 와이어에 나감)
    unsigned long long updateEncFail_ = 0;    // 용량을 넘겨 0 이 나온 횟수
    size_t updateEncFailObjsMax_ = 0;         // 실패했을 때 항목 수의 최대
    size_t updateEncOkObjsMax_ = 0;           // 성공했을 때 항목 수의 최대
    // ★ 부분 인코딩으로 **이번 통에 안 실린** 항목. 무산은 사라졌지만 이쪽이 크면
    //   그 오브젝트는 그 틱에 클라로 안 가고 **깜빡일 수** 있다.
    unsigned long long updateDroppedObjs_ = 0;   // 잘린 항목 누계
    unsigned long long updateDroppedTicks_ = 0;  // 한 개라도 잘린 틱 수
    size_t updateDroppedMax_ = 0;                // 한 통에서 잘린 항목 최대
    // ★ `EncodeUpdate` 를 부르는 자리는 셋이다. **자리를 갈라 센다** — 합계만 보면
    //   어디서 잘렸는지 못 가른다.
    //     · 매 틱 갱신 `body[2048]` — 위 `updateDropped*`
    //     · 스냅샷     `ub[480]`    — 아래(접속 스냅샷 안 갱신 · 정적 소개는 상한과 짝)
    //     · 갈아끼우기 `buf[1024]`  — **안 센다.** 그 함수가 Server 멤버가 아니고
    //       입력이 정품 녹음본(480B 안쪽)이라 통이 두 배다. 근거가 생기면 인자로 뺄 것
    unsigned long long updateDroppedSnap_ = 0;
    // ★ 09-19 — ⑧ 워리어 능력. 「보냈는데 아무 일도 안 난다」를 가를 수가 없었다
    //   (서버 로그에 능력 흔적이 한 줄도 없었다). 받은 것 → 갈래별 거절 → 실행 순으로 센다
    unsigned long long abilHeard_ = 0;        // `7a TRY_ABILITY` 를 받은 횟수
    unsigned long long abilBadPacket_ = 0;    // 게임 밖이거나 본문 길이가 1 이 아님
    unsigned long long abilObserver_ = 0;     // 관전이라 무시
    unsigned long long abilCalls_ = 0;        // AbilityDo 까지 들어옴
    unsigned long long abilBadNum_ = 0;       // 번호가 1~5 밖
    // ★ 09-23 — 나머지 갈래는 **능력별 칸**이다(`AbilGate`). 등식 두 개로 빠진 자리를 잡는다 —
    //   ① Σ abilIn_ + abilBadNum_ = abilCalls_   ② 능력마다 abilIn_[a] = 그 능력 칸 아홉의 합
    unsigned long long abilIn_[kAbilityCount] = {};                       // 번호 검사를 지나 들어옴
    unsigned long long abilGate_[kAbilityCount][kAbilGateCount] = {};     // 어디로 나갔나
    // ★ 09-19 — 첫 접속 자리. `--spawn-at`·`--item-at-spawn` 으로 못 박았으면 그 값을 쓰고, 아니면
    //   **맵의 PlayerStart** 에 세운다(정답지 `newPlayer` · `player.go:416~426`).
    //   전에는 늘 덤프에서 온 고정값(bludeath 의 3864,2534)이라 **맵마다 엉뚱한 자리**였다
    bool spawnForced_ = false;
    // 주인 없는 둘의 갈래 — 둘 다 몬스터(적 아님) / 한쪽이 몬스터가 아님(적)
    uint16_t abilSoundUse_[6] = {0, 0, 0, 0, 0, 0};
    uint16_t abilSoundOff_[6] = {0, 0, 0, 0, 0, 0};
    // ★ 09-23 0-b — 능력 소리를 **부른** 횟수(버려졌는지는 `soundsDroppedIds_` 가 번호로 센다)
    unsigned long long abilSoundUseCalls_[kAbilityCount] = {};
    unsigned long long abilSoundOffCalls_[kAbilityCount] = {};
    unsigned long long abilEnded_[kAbilityCount][kAbilEndCount] = {};   // 켜진 능력이 어떻게 꺼졌나
    unsigned long long abilBlockedBy_[4][4] = {};                       // [누른 것][켜져 있던 것] — 겹침 거절
    unsigned long long treadBlockedRun_ = 0;                            // ★ 1-a — 살금살금이 달리기를 막은 틱
    mutable unsigned long long wildBothMonster_ = 0;
    mutable unsigned long long wildRootNotMonster_ = 0;
    unsigned long long spawnFromMap_ = 0;      // PlayerStart 에서 골랐다
    unsigned long long spawnFromDefault_ = 0;  // 고정값(또는 --spawn-at)을 썼다
    // ★ 09-19 — 검은 실루엣. 접속 묶음에 실을 **플레이어 정보 97B** 가 있었나
    // ★ 09-19 시험 스위치 — 관전 풀 때 `2d NEW_PLAYER` 를 **다시 안 보낸다**(`--no-2nd-newplayer`).
    //   그 두 번째 통은 `Field3680`(112번)을 갈아 끼우려고 우리가 넣은 우회인데,
    //   정답지는 보내지 않는다. 빼도 HUD 가 뜨면 우회를 없앨 수 있다
    //
    // ★★ 09-19 실측·정정 — **켜는 쪽(= 두 번째를 안 보내는 쪽)이 정답지와 같다.**
    //   끄고 돌리면 HUD 에 「Spell Z」 글자와 `A~G` 가 뜨는데, **정답지 화면에는 그것이 없다**
    //   (`_evidence/phaseE_20260919/silhouette/07·08`). 처음엔 「우리 이전 상태」를 기준으로 잡아
    //   「우회가 아직 필요하다」고 적었는데 **기준이 틀렸다. 기준은 정답지다.**
    //   ⚠ 그래도 기본값은 아직 `false`(우회 켬)로 둔다 — HUD 상태가 바뀌면 ⑧ 칸 판정 등
    //     다른 자리에 영향이 간다. 그쪽을 함께 볼 때 기본값을 뒤집을 것
    // ★★★★ 09-19 **기본값을 뒤집었다(false → true).** 까닭 셋 —
    //   ① 정답지는 `2d` 를 **한 번만** 보낸다(대조군 와이어 실측)
    //   ② HUD 가 정답지와 **같아진다**(끄면 「Spell Z」 글자·`A~G` 가 뜨는데 정답지엔 없다)
    //   ③ **장비가 보인다** — 두 번째 통이 **자기 장비를 지우고 있었다.**
    //      클라 둘로 찍으니 자기 캐릭터에 **파란 바지와 방패**가 나왔다(끄면 맨몸)
    //   옛 동작으로 되돌리려면 `--2nd-newplayer`
    bool noSecondNewPlayer_ = true;
    // ★ 09-19 ⑦ 작살 — 「쐈나 / 걸렸나 / 끌고 있나」를 갈라 센다.
    //   확인표는 이 자리를 「셋 중 가장 의심스럽다」고 적어 두었다(끌려오는 쪽이
    //   주인을 지나쳐 넘나드는지 = 우리 셈, 부드럽게 오면 우리가 틀린 것)
    // ★ 09-19 진단 스위치 — `--harpoon-force`(맞히지 않고 바로 건다) ·
    //   `--dump-harpoon`(걸린 동안 매 틱 두 사람 자리를 찍는다)
    bool forceHarpoon_ = false;
    bool dumpHarpoon_ = false;
    unsigned long long harpoonDoCalls_ = 0;   // 능력 3번이 뒷단까지 들어옴
    unsigned long long harpoonNoType_ = 0;    // HarpoonBolt 종류를 못 찾음
    unsigned long long harpoonShot_ = 0;      // 작살을 실제로 쐈다
    unsigned long long harpoonAttached_ = 0;  // 상대에 걸렸다
    unsigned long long harpoonPullTicks_ = 0; // 걸린 채로 지난 틱
    unsigned long long harpoonTouch_[kHarpoonTouchCount] = {};   // ★ 09-23 0-b — 무엇에 닿았나
    unsigned long long harpoonEnd_[kHarpoonEndCount] = {};       // ★ 09-23 0-b — 줄이 왜 끊겼나
    unsigned long long harpoonAimFound_ = 0;                     // ★ 1-b — 휘어 갈 상대를 잡았다
    unsigned long long harpoonSteerTicks_ = 0;                   // ★ 1-b — 그쪽으로 튼 틱
    unsigned long long joinInfo97Ok_ = 0;     // 97B 를 그대로 실었다
    unsigned long long joinInfo97Null_ = 0;   // **없어서 0 으로 채웠다**(색이 검정이 된다)
    size_t acceptBodyLenLast_ = 0;            // 마지막 CLIENT_ACCEPT 본문 길이
    unsigned long long appearanceSent_ = 0;   // 겉모습을 남에게 알린 횟수 (P7)
    unsigned long long newPlayerGearBits_ = 0;   // ★ 09-28 — 2d 104·108 에 지금 갑옷·무기 비트를 실은 횟수
    unsigned long long newPlayerInfo_[3] = {};   // ★ 09-28 — 2d 정보 97B 출처(녹음 그대로 · 덮음 · 조립)
    // ★ 09-28 — 게임 중 새로 놓인 바닥 물건(버리기·되돌리기). 시야 반경이 꺼진 판에서도 이것만은 반경 안에 한 번 소개한다
    std::set<uint16_t> freshStatics_;
    unsigned long long appearanceUnknown_ = 0;// 장비 비트를 몰라 못 알린 횟수
    unsigned long long fxSent_ = 0;           // 그린 이펙트 수
    // ★ 시전 알림 `0x70` 을 끄는 **진단 스위치**. **기본은 보낸다.**
    //
    //   2026-09-11 — 주문을 쓰면 클라가 게임에서 나가는 일이 있어 이분법을 돌렸다.
     //   `0x70` 만 끄니 클라가 붙어 있어 범인인 줄 알았는데, **대조군(켠 채)도 안 나갔다.**
    //   즉 **이분법이 성립하지 않았다** — 두 회차 모두 「걷지 않고 바로 시전」이었다.
    //   나간 회차(g9)는 **걷고 나서** 시전했다. 조건은 `0x70` 이 아니라 **걷기+시전**으로 보인다.
    //
    //   ⚠ 그러니 기본 동작은 **안 바꾼다.** 근거가 설 때까지 보내는 쪽을 유지한다.
    //     (한 번 기본을 끔으로 돌렸다가 `proto_test` 가 「0x70 이 둘에게 안 나감」으로 잡아냈다)
    bool noSpellStart_ = false;
    std::set<uint8_t> stopSampleShown_;   // 진단 — 멈춤 표본을 오프코드마다 한 번만
    int startHealth_ = 0;       // 진단 — 들어올 때 체력(0 이면 안 건드림)
    int startMana_ = 0;         // 진단 — 들어올 때 마나(0 이면 안 건드림)
    int forceAnimDir_ = -1;     // 진단 — 덧붙이 방향 3비트 고정(-1 이면 안 함)
    int forceArrowFrame_ = -1;  // 진단 — 화살 스프라이트 프레임 고정(-1 이면 안 함)
    bool arrowFreeze_ = false;  // 진단 — 화살을 난 자리에 세워 둔다
    int arrowFrameAlt_ = 0;     // 진단 — N 틱마다 프레임 0↔16 번갈기(0 이면 안 함)
    int forceAnimPose_ = -1;    // 진단 — 사람 자세 번호 고정(-1 이면 안 함)
    unsigned long long stopped_ = 0;   // 끝까지 못 쪼개 통째로 넘긴 조각 수
    std::map<uint8_t, unsigned long long> stoppedByOp_;
    std::vector<uint8_t> lastFragOps_;          // 마지막 조각에서 읽어낸 오프코드들(진단)
    std::map<uint8_t, uint8_t> spellCost_;      // 주문 번호 → 마나 (thing.bin SPEL)
    unsigned long long spellsNoMana_ = 0;       // 마나가 모자라 튕긴 시전
    unsigned long long tryGets_ = 0;            // `0x73` 을 받은 횟수
    unsigned long long picked_ = 0;             // 실제로 집어 든 것
    unsigned long long pickTooFar_ = 0;         // 손이 안 닿아 못 집은 것
    unsigned long long pickNoCap_ = 0;          // 묶음 5 — 들 밑천이 0 이라 못 집은 것
    unsigned long long pickTooHeavy_ = 0;       // 묶음 5 — 무게 한도를 넘겨 못 집은 것
    unsigned long long pickTooMany_ = 0;        // 묶음 5 — 같은 먹을거리를 너무 많이 들어 못 집은 것
    unsigned long long pickClassFail_ = 0;      // 묶음 5-c — 직업이 못 쓰는 것이라 못 집은 것
    unsigned long long drankOnPickup_ = 0;      // 묶음 5-c — 줍는 그 자리에서 마신 것
    unsigned long long goldPicked_ = 0;         // 묶음 5-h — 주운 금 더미 수
    bool aiStackChanged_ = false;               // 묶음 6-a — 이번 굴림에서 쌓임이 바뀌었나
    unsigned long long dropped_ = 0;            // 클라가 되돌려 도로 떨군 것
    unsigned long long used_ = 0;               // 실제로 효과가 난 쓰기
    unsigned long long useUnknown_ = 0;         // 갈래를 몰라 아무 일도 안 난 쓰기
    unsigned long long equipped_ = 0;
    unsigned long long dequipped_ = 0;
    unsigned long long timerAsks_ = 0;          // `0xd4` 를 받은 횟수
    unsigned long long chats_ = 0;              // 되돌려 방송한 채팅 수
    unsigned long long aims_ = 0;               // `0xac` 로 받은 겨눔 수
    unsigned long long hurts_ = 0;              // 닿은 피해 전부 (근접+투사체+소환수)

    std::vector<uint8_t> infoBody_;
    std::vector<uint8_t> joinBody_;     // 접속 직후 나가는 216바이트 묶음
    std::vector<uint8_t> useMapBody_;
    std::vector<std::vector<uint8_t>> postMap_;
    std::vector<uint8_t> firstSnapshot_;
    std::vector<std::vector<uint8_t>> mirror_;
    // 서두의 첫 TIMESTAMP 프레임. 0 이면 서두에 시각이 안 실린 것이다.
    uint32_t preludeFirstFrame_ = 0;
    bool sendUpdates_ = true;
    bool patchSnapshot_ = true;
    bool notifyNewGear_ = true;
    bool forceHudOn_ = true;
    // 새 사람이 들어왔을 때 **먼저 와 있던 사람들에게 넘길** 오프코드.
    // 기본은 장비 둘 — 더 넣으면 나빠진 적이 있어(2026-09-08) 손잡이로 뺐다.
    std::set<uint8_t> peerOps_{0x4c, 0x60};
    // 스폰 자리를 사람마다 몇 픽셀씩 벌리나. 기본 40 은 근접 사거리(45) 안이다.
    int spawnSpread_ = 40;
    std::vector<WorldStatic> statics_;
    std::vector<WorldProp> props_;
    // ★ 09-22 — 사람이 문에 **가장 가까이 간 거리**. 「문 0」의 분모다
    double doorNearestDist_ = 1e18;
    // ★ 09-22 `0xd7` — 500ms 주기와 **돌아가며 한 명** 뽑는 커서(정답지 `GAME3_2.c:1444`)
    uint32_t lastLatencyFrame_ = 0;
    size_t   latencyCursor_ = 0;
    unsigned long long doorsOpened_ = 0;
    unsigned long long doorResync_ = 0;   // 09-21 — 되맞춤으로 **따로 보낸** 문 알림 수(나중에 들어온 사람 몫)
    unsigned long long obeliskTold_ = 0;  // 09-21 — 접속한 사람에게 마나석 밝기를 알려 준 건수
    unsigned long long wallPushSteps_ = 0;   // 09-21 — 벽이 사람을 밀어낸 걸음 수
    double wallPushMax_ = 0.0;               // 그때 더해진 가속의 최대(걷기 힘 3.0 과 견줄 것)
    // 09-21 — 오브젝트가 민 걸음 수, 갈래별: [0]=놓인 것 [1]=다른 사람 [2]=소환수
    unsigned long long bodyPushSteps_[3] = { 0, 0, 0 };
    double bodyPushMax_ = 0.0;               // 그때 더해진 가속의 최대(벽과 같은 잣대)
    BodyPushWorst bodyPushWorst_;            // 그 최대를 만든 짝의 자세한 값
    FirstKick firstKick_;                    // 사슬의 첫 고리(한 번만 적는다)
    unsigned long long pushN_ = 0;           // 대조군과 맞댈 네 숫자 — 건수
    double pushSum_ = 0.0;                   //                      합(평균용)
    double pushMax_ = 0.0;                   //                      최대
    unsigned long long pushBig_ = 0;         //                      100 을 넘은 건수
    double wallFirstTouchMax_ = 0.0;         // 닿는 첫 걸음의 깊이 — 최대
    double wallFirstTouchSum_ = 0.0;         // 같은 것의 합(평균을 내려고)
    unsigned long long wallFirstTouchN_ = 0; // 분모 — 0 을 「없었다」로 읽지 않으려고 같이 찍는다
    FirstKick kickSeq_[12];                  // 그 뒤 차례대로 11개 더 — 증폭을 수열로 본다
    size_t kickSeqN_ = 0;                    // 분모 — 12 를 넘었는지 알아야 「12개뿐」을 안 믿는다
    mutable WallPushWorst wallPushWorst_;    // 가장 세게 민 벽 칸(CircleVsWalls 가 const 라 mutable)
public:
    unsigned long long traceBlocked() const { return traceBlocked_; }
private:
    unsigned long long traceBlocked_ = 0;    // 09-21 — 선 추적이 사람 걸음을 막은 횟수
    double playerSpeedMax_ = 0.0;            // 사람의 틱당 속도 최대(걷기 6 · 달리기 12 가 정상)
    unsigned long long playerInWallSteps_ = 0;  // 사람이 벽 칸 안에 있던 걸음
    unsigned long long playerSteps_ = 0;        // 그 분모 — 0 옆에 지나간 양을 같이 읽는다
    // ⚠ `CircleVsWalls` 가 const 라 계측 셋은 mutable 이다 — 물리는 안 건드리고 세기만 한다
    mutable double wallNearestLen_ = 1e18;              // 벽 선까지 최소 거리(반지름 10 과 견줄 것)
    mutable unsigned long long wallPushPerStepCur_ = 0; // 이번 걸음에 민 조각 수
    mutable unsigned long long wallPushPerStepMax_ = 0; // 그 최대
    unsigned long long obeliskMana_ = 0;
    unsigned long long obeliskNearIdle_ = 0;   // ★ 09-28 — 옆에 사람 있음 · 줄 것 없음(잔량 0 또는 마나 가득) → 회복도 소리도 없음
    unsigned long long plateOn_ = 0;    // ⑩ 판이 **눌렸다**고 보낸 건수(`0xb4` 칸 1)
    unsigned long long plateOff_ = 0;   // ⑩ 판을 **뗐다**고 보낸 건수(`0xb4` 칸 0)
    unsigned long long roamAdvance_ = 0;     // ⑥ 이음을 타고 다음 길목으로 넘어간 횟수
    unsigned long long roamDeadEnd_ = 0;     // ⑥ 갈 이음이 없어 떠돌기를 접은 횟수
    unsigned long long aiMoveTargetFollow_ = 0;   // 09-28 가기(7·8)가 대상 자리로 고쳐 적힌 틱 수
    unsigned long long aiMoveTargetForgot_ = 0;   // 09-28 가기(7·8)가 대상이 안 보여 대상을 잊은 횟수
    unsigned long long wallOpenSent_ = 0;    // 9-a 비밀벽 열림 알림 건수
    unsigned long long wallCloseSent_ = 0;   // 9-a 비밀벽 닫힘 알림 건수
    bool aoiOn_ = false;
    double creatureAoiHalfX_ = 0.0;   // 0 이면 크리처 시야 판정 꺼짐 (SetCreatureAoi)
    double creatureAoiHalfY_ = 0.0;
    uint32_t debugAwayAfter_ = 0;     // 진단 --creature-away-after (0 끔)
    uint32_t firstInGameFrame_ = 0;
    bool debugAwayDone_ = false;
    uint32_t debugTeamAfter_ = 0;     // 진단 --debug-team-after (0 끔)
    uint32_t debugTeamEvery_ = 0;     // 진단 --debug-team-every — N틱마다 팀을 1↔2 로 오간다 (0 끔)
    // ★ 09-19 `--debug-score N 점수` — 판 끝 사슬을 보려고 점수를 직접 준다(까닭은 구현부 주석)
    // ★ 09-19 `--debug-kill N` — N틱 뒤 첫 사람이 둘째를 죽인 것으로 친다(까닭은 구현부 주석)
    uint32_t debugFistAfter_ = 0;
    int debugFistDx_ = 40, debugFistDy_ = 0;
    bool debugFistDone_ = false;
    uint32_t debugWarpAfter_ = 0;               // 진단 --debug-warp (0 끔)
    int debugWarpX_ = 0, debugWarpY_ = 0;
    bool debugWarpDone_ = false;
    uint32_t debugWallAfter_ = 0;               // 진단 --debug-wall (0 끔)
    int debugWallGx_ = 0, debugWallGy_ = 0;
    bool debugWallDone_ = false;
    uint8_t debugPetOrder_ = 0;                 // 진단 --debug-pet-order (0 끔)
    uint32_t debugMobAfter_ = 0;                // 진단 --debug-mob (0 끔)
    uint16_t debugMobType_ = 0;
    int debugMobX_ = 0, debugMobY_ = 0;
    bool debugMobDone_ = false;
    uint32_t debugKillAfter_ = 0;
    bool     debugKillDone_ = false;
    uint32_t debugScoreAfter_ = 0;
    uint32_t debugScoreVal_ = 0;
    bool     debugScoreDone_ = false;
    bool debugTeamDone_ = false;
    uint32_t debugXpAfter_ = 0;       // 진단 --debug-xp (0 끔)
    float debugXpAmount_ = 0.0f;
    bool debugXpDone_ = false;
    uint32_t debugSpellAfter_ = 0;    // 진단 --debug-give-spell (주문 0 이면 끔)
    uint8_t debugSpellId_ = 0;
    bool debugSpellDone_ = false;
    bool debugConjurerGuides_ = false;   // 진단 --debug-conjurer-guides
    bool friendsOn_ = false;
    uint32_t debugOwnPetAfter_ = 0;   // 진단 --debug-own-pet (0 끔)
    bool debugPetWild_ = false;        // 진단 --debug-dummy — 주인 없음 · 맞음
    int debugDummyDx_ = 25;            // 진단 --debug-dummy-dx
    int debugEnchantBuff_ = -1, debugEnchantPower_ = 1;   // 진단 --debug-enchant
    uint32_t debugEnchantAfter_ = 0;
    bool debugEnchantDone_ = false;
    uint32_t debugHurtEvery_ = 0;      // 진단 --debug-hurt-every (0 끔)
    bool debugHurtFront_ = false;      // 진단 --debug-hurt-front — 앞 30 에서 때린 것으로
    std::vector<std::pair<float, float>> playerStarts_;
    std::vector<Waypoint> waypoints_;   // 맵 길목(점멸 목적지 · 떠돌기 길)   // 묶음 3-c PlayerStart 자리
    bool autoRespawn_ = false;         // 묶음 3-c 자동 부활(정답지 기본 꺼짐)
    SpellDef spellDefs_[137];                      // 묶음 4-a 주문 정의
    std::vector<PhonemeNode> phonemeTree_ = std::vector<PhonemeNode>(1);   // [0] = 뿌리
    std::vector<CastEntry> castList_;              // 묶음 4-a 시전 목록 — [0] 이 머리(가장 나중에 넣은 것)
    Target spellTarget_;                           // 묶음 4-a playerinfo +3640(Obj3640) — 시전이 끝난 순간 고른 대상
    std::vector<MissileObj> missiles_;             // 묶음 4-a2 날아가는 오브젝트
    std::vector<PlacedObj> placed_;                // 묶음 4-c3 주문이 놓는 오브젝트(불꽃·독구름)
    std::map<std::string, uint16_t> namedTypes_;   // 이름 → 와이어 종류(thing.bin 에서 받는다)
    std::set<uint16_t> arrowTypes_;                // 화살·쇠뇌살 — 이 종류만 갱신에 프레임이 붙는다(AddArrowType 주석)
    std::list<DurSpell> durSpells_;                // 묶음 4-a3 지속 주문 목록 — 머리에 넣는다(Add)
    Gamedata gamedata_;                            // 묶음 4-a4 gamedata.bin
    bool hasGamedata_ = false;
    uint16_t durLastId_ = 0;                       // NewRaw lastID
    int debugDurCreateRet_ = 0;                    // 시험용 create 가 돌려줄 값
    int debugDurUpdateStopAt_ = 0;                 // 시험용 update 가 이 횟수째에 1(0 이면 늘 0)
    int debugDurUpdates_ = 0, debugDurDestroys_ = 0;
    Target lastAcceptTarget_;                      // 시험용 — 마지막 spellAccept 의 대상
    unsigned long long acceptCount_ = 0;
    std::string lastAcceptCaster_;
    MagicTypeInfo magicType_;                      // 묶음 4-a2 thing "Magic"
    std::map<std::string, ProjTypeInfo> projTypes_;   // 묶음 4-c3 이름 → 날아가는 종류(불덩이 셋 …)
    unsigned long long spellEffectDeferred_ = 0;   // 효과 함수를 아직 안 옮긴 주문이 받아들여진 수
    std::vector<MapObjRef> mapObjects_;            // 묶음 3-d 맵 오브젝트 번호·extent·종류
    std::map<uint16_t, std::string> collideFn_;    // 묶음 3-d 종류 → 충돌 함수 이름
    std::map<uint32_t, uint16_t> weaponWire_;      // 묶음 3-d 무기 클래스 비트 → 종류 번호
    unsigned long long tryCollides_ = 0;           // `0x7B` 를 받아 대상을 찾은 횟수
    unsigned long long tryCollideDeferred_ = 0;    // 그 가운데 충돌 함수를 아직 안 옮긴 것
    bool debugNoAutoFight_ = false;   // 시험용 — 틱의 자동 싸움 쌓기를 끔
    uint16_t debugOwnPetNetCode_ = 0; // 만든 임프의 번호 — 이동 루프가 이것만 건너뛴다(고정)
    bool standalone_ = false;
    std::string saMapName_;
    uint32_t saMapCrc_ = 0;   // P4-c 실클라 검증 전까지 기본 꺼짐   // 맵 정적 오브젝트 (P4)
    std::vector<uint8_t> walls_;   // 256×256 벽 격자 (P3)
    std::vector<AIIndexNode> aiIndex_ = std::vector<AIIndexNode>(kAIIndexSize * kAIIndexSize);
    uint32_t aiIndexGen_ = 0;
    uint32_t aiIndexFrame_ = 0;
    bool aiIndexCalc_ = false;   // `calculated` — 이번에 이미 떴나
    uint32_t aiIndexLast_ = 0;   // `MapIndexLast` — **훑기마다** 하나 올려 「가 봤다」 자국으로 쓴다
    std::vector<AIVisitNode> aiVisit_;
    int aiPathStatus_ = 0;                          // 0 닿음 · 1 자리 다 씀 · 2 깊이·훑을 곳 없음
    std::vector<std::pair<int, int>> aiPathCells_;  // 찾은 길(칸)
    std::vector<std::pair<float, float>> aiPathPts_;
    bool wallsOn_ = false;
    // ★ 묶음 9-a — `secret` 은 `secretWalls_` 자리표(없으면 −1). 정답지 벽 구조의 `+28`(비밀벽 마디)과
    //   `Flags4 & 4`(비밀벽 깃발)를 한 칸으로 합친 것이다
    // ★ 09-22 — `door` 는 이 칸에 붙은 **문의 순번**(`props_` 색인). 정답지는 맵을 읽을 때
    //   `nox_xxx_doorAttachWall_410360`(`GAME1.c:692`)으로 문 자리에 **벽을 만들어** 붙이고
    //   `Flags4 |= 0x10`(FlagDoor)을 켠다. 그래서 정적 WallMap 에는 문 칸이 없다
    //   (Estate 실측 — 문 6개 중 그 칸에 벽이 있는 것은 1개뿐이었다)
    // ★ 09-29 Z1-A — breakable(깃발 8 · 맵 `DestructableWalls`) · broken(깃발 0x20 — 부서짐 · 그 뒤로는 벽이 없다) · window(깃발 0x40 · 맵 `WindowWalls`) ·
    //   hp(+7 · 벽 정의 Health41) · breakId(+10 · 부서지는 벽 목록의 차례 — 부서짐 알림 `3a <번호2>` 에 실린다)
    struct WallTile { uint8_t dir = 0; uint8_t tile = 0; bool flag80 = false; bool present = false; int16_t secret = -1; int16_t door = -1;
                      bool breakable = false; bool broken = false; bool window = false; uint8_t hp = 0; uint16_t breakId = 0; };
    std::vector<WallTile> wallTiles_;        // 256×256 벽 모양 (묶음 2-b)
    std::vector<uint32_t> wallDefFlags_;     // thing.bin WALL 순번 → 깃발
    std::vector<WallDefExtra> wallDefExtra_; // ★ 09-29 Z1-A thing.bin WALL 순번 → 맞는 소리 재질·체력·부서지는 소리
    unsigned long long wallGate_[5] = {};    // ★ 09-29 Z1-A 벽 피해 갈래(wallGate 주석)
    // ★ 09-29 Z1-A 문 잠김(DoorTouch 주석)
    void DoorCollide(size_t di, Session& s, std::vector<OutPacket>& out);   // nox_xxx_collideDoor_4E8AC0(문, 사람)
    void DoorUnlockQuest(size_t di, std::vector<OutPacket>& out);            // sub_4E8390 — 퀘스트면 모두에게 `f0 0f <extent>`
    uint16_t keyWire_[5] = {};
    uint32_t doorLockMsgFrame_ = 0;          // qword_5d4594_1567940 — 잠김 알림 1.5초 간격(정답지는 실시간 ms · 모든 문·사람이 하나를 같이 쓴다)
    unsigned long long doorTouch_[static_cast<size_t>(DoorTouch::Count)] = {};
    unsigned long long berserkDoorLocked_ = 0;
    size_t doorCellDiff_ = 0, doorCellOldHadWall_ = 0, doorCellNewHadWall_ = 0;
    // ★ 09-29 B1 알아보기
    unsigned long long sightStat_[kSightStatCount] = {};
    unsigned long long b2Stat_[kB2StatCount] = {};   // ★ 09-29 B2
    unsigned long long meleeStrike_[kMeleeStrikeWayCount] = {};   // ★ 09-29 B3
    unsigned long long attackTiming_[kAttackTimingCount] = {};    // ★ 09-30
    unsigned long long shotNoAim_ = 0;                            // ★ 09-30 — 쏘는 칸에서 겨눌 곳이 없어 안 쏜 수(대상 사라짐 · 자리 0)
    unsigned long long bundleStat_[kBundleStatCount] = {};        // ★ 09-30 한 틱 한 데이터그램
    uint32_t attackLagMin_[2] = {}, attackLagMax_[2] = {};
    bool attackLagSeen_[2] = {};
    void CountAttackTiming(const Creature& k, int kind, int frame);   // kind 0 근접(애니 1) · 1 쏘기(애니 3)
    unsigned long long brainStat_[kBrainStatCount] = {};          // ★ 09-29 B4
    unsigned long long animStat_[kAnimStatCount] = {};            // ★ 09-29 B5
    unsigned long long monsterAnimSent_[16] = {};                 // ★ 10-01 — 몬스터 갱신 항목에 실은 애니 번호(0~15)별 · 보는 사람마다 한 번씩
    unsigned long long summonOwnSent_[3] = {};                    // ★ 10-01 — 주인에게 보낸 소유 알림 0 `6c` 얻음 · 1 `dd` 총체력 · 2 `6d` 잃음
    struct MonsterAnimRow { uint8_t frames = 0, hold = 0; bool loop = false; };
    std::map<uint16_t, std::array<MonsterAnimRow, 16>> monsterAnims_;   // ★ 09-29 B5 — 갱신자료 +476 표(종류별)
    unsigned long long hearStat_[kHearStatCount] = {};            // ★ 09-29 B6
    unsigned long long castStat_[kCastStatCount] = {};            // ★ 09-29 B7
    // ★ 09-29 B7 — 크리처 틱 동안의 패킷 통로 — 끝내기·취소(`AIEnd`·`AICancel`)는 `out` 을 안 받는데 지속 시전을 끌 때 빔 끄기 패킷을 내야 한다
    std::vector<OutPacket>* aiOut_ = nullptr;
    std::vector<ListenRec> listens_;                              // ★ 09-29 B6 — 들을 거리(2프레임까지 산다 · 정답지 `listenHead`)
    std::map<uint16_t, uint32_t> monsterSubclass_;                // ★ 09-29 B6 — 종류 → 몬스터 아종 비트
    std::map<uint16_t, uint8_t> dangerTypes_;                     // ★ 09-29 B6 — 종류 → 위험 갈래(1 불 · 2 독구름 · 3 DANGEROUS)
    uint32_t sightForceFrame_ = 0xFFFFFFFFu;   // dword_5d4594_2487684 — 이 프레임엔 모든 몬스터가 다시 훑는다(순간이동·살금살금 켜기 다음 프레임)
    std::vector<PhysBody> obstacles_;        // 묶음 2-c 장애물
    std::map<uint16_t, PhysBody> typeBodies_;   // 소환수 종류 → 몸
    static constexpr int kPlayerAnimSlots = 55;     // ThingDb kPlayerAnimTypeCount 와 같은 길이
    int playerAnimFrames_[kPlayerAnimSlots][2] = {};   // [칸 수, 붙드는 틱] — PlayerAnimFrames(i)
    // 남 항목의 덧붙이 값(자세로 보인다). 정품 표본은 0x30 인데 그 표본의 상대는
    // 화면 밖이었다 — 그려지는 값이 따로 있을 수 있어 손잡이로 뺐다.
    unsigned long long unitHealthSent_ = 0;
    unsigned long long healthDeltaSent_ = 0;
    unsigned long long healthAbsSent_ = 0;
    unsigned long long healthDeltaHeld_ = 0;
    std::map<std::string, unsigned long long> damageStops_;
    unsigned long long unitHeightSent_ = 0;   // 5e/5f 를 몇 번 보냈나(⑰)
    unsigned long long enchantSent_ = 0;      // 0x5A 를 보낸 횟수 (R4)   // 0x41 을 보낸 횟수 (R4)
    int peerPose_ = -1;   // ⭕ 2026-09-13 — 기본은 덮지 않음(0x30 은 그때 상대의 방향이었다 · Server.cpp 남 항목 주석)
    // 갱신 항목 순서를 뒤집는다(남 먼저). 「자리 탓인가 누구냐 탓인가」를 가르는 시험용.
    bool peerFirst_ = false;
    // 새 사람이 들어올 때 **상대 몫의 첫 스냅샷을 통째로** 보낸다.
    // 「스프라이트는 스냅샷에서만 만들어진다」 가설을 가르는 자리.
    bool peerSnapshot_ = false;
    // 남을 사람 아닌 종류로 보내 본다(0 이면 사람 그대로). 가르는 시험용.
    uint16_t peerType_ = 0;
    // 사람 둘 이상일 때 매 틱 0xff 로 다시 소개할지. 기본 꺼짐 — 켜면 클라가
    // 갱신마다 별칭을 새로 만든다(2026-09-09 실측: 정품의 30배).
    // ★★ 기본이 **켜짐**이다. 끄면(별칭으로 보내면) **아무도 안 그려진다**(2026-09-09 사람 눈).
    //   내 별칭표가 클라 것과 어긋나서, 클라가 그 별칭을 엉뚱한 것으로 풀거나 못 푼다.
    //   제대로 고치려면 **클라가 보내오는 `0xa5 NEW_ALIAS` 를 받아 그 번호를 그대로 써야** 한다.
    // ★★ 기본이 **켜짐**이다 — 사람 항목을 매 틱 전체번호로 다시 소개한다.
    //
    //   끄면(클라가 알려 준 별칭으로 보내면) **아무도 안 그려진다**(2026-09-09 사람 눈).
    //   `0xa5 NEW_ALIAS` 를 받아 그 자리를 쓰게 고쳤는데도 그렇다 — 클라가 그 순간
    //   그 오브젝트에 물려 둔 자리와 여전히 어긋나는 것으로 보인다. 미결.
    //
    //   ★ 앞서 「매 틱 소개가 클라를 1분에 무너뜨린다」고 적었는데 **그건 틀렸다.**
    //     진짜 클라 하나 + 가짜 하나로 재니 이 설정으로 **3분 넘게 멀쩡했다**
    //     (별칭받음 8,630). 끊김은 **진짜 클라 둘을 한 PC 에서 돌릴 때** 나는 것이고
    //     갱신 짜임 탓이 아니다.
    bool introEveryTick_ = true;
    // 클라가 알려 준 별칭을 몇 번 받았나. 0 이면 NEW_ALIAS 를 못 받고 있는 것이다.
    unsigned long long aliasHeard_ = 0;

    // ── 접속 악수 계수기 셋 (2026-09-18, 페이즈 B 1-a) ─────────────────
    //
    // ★ 왜 **셋을 따로** 세나 — 무적이 안 풀리는 사슬이 이렇게 걸려 있다:
    //     USE_MAP 보냄 → 클라가 맵을 다시 읽음 → CLIENT_READY 다시 보냄
    //       → 받을 때마다 5초 무적을 다시 검 → 사실상 영구
    //   어느 고리가 되풀이되는지 갈라야 하는데, 한 계수기로는 **하나가 나머지를 삼킨다.**
    //   09-07 실측 = 정품 클라가 CLIENT_READY 를 **71번**(정품 서버엔 2번) 보냈다.
    //   09-18 opennox 클라 = 로그상 **1번**. 클라마다 다르므로 둘 다 세어 대조한다.
    unsigned long long readyHeard_ = 0;    // CLIENT_READY 받은 수 (기대: 정품 2)
    unsigned long long useMapSent_ = 0;    // USE_MAP 보낸 수     (기대: 접속당 1)
    unsigned long long acceptHeard_ = 0;   // CLIENT_ACCEPT 받은 수(기대: 접속당 1)
    unsigned long long spellPlayerCalls_ = 0;  // PlayerSpell 이 불린 수
    unsigned long long spellLeafZero_ = 0;     // 음소 잎이 0 (나무를 못 따라감)
    unsigned long long spellIndZero_ = 0;      // 잎은 있는데 주문 번호가 0
    unsigned long long spellNotLearned_ = 0;   // 그 주문을 안 배웠다
    unsigned long long spellFriendAtCursor_ = 0; // 커서에 아군이 잡혀 조용히 나갔다
    unsigned long long spellErrInform_ = 0;      // 직업·못씀 따위로 막혔다
    unsigned long long spellCastFailed_ = 0;     // 시전 함수가 거짓을 돌려줬다
    unsigned long long fistTooMany_ = 0;         // 주먹 — 이미 자기 것이 떠 있다
    unsigned long long fistNoLos_ = 0;           // 주먹 — 커서 자리가 안 보인다
    unsigned long long reliableHeard_ = 0; // 신뢰 비트가 선 조각을 받은 수 (B-1b)
    // 스트림 0~7 만 센다 — 그 위는 실제로 안 쓰인다(넘으면 안 센다는 뜻이니 로그에서 합이 안 맞으면 그것도 단서다)
    unsigned long long ackValueHist_[32] = {};   // 되받기로 실어 보낸 **값**의 분포(0~31)
    unsigned long long recvRelStream_[8] = {};   // 클라 → 서버 **신뢰**, 스트림별
    unsigned long long sendRelStream_[8] = {};   // 서버 → 클라 **신뢰**, 스트림별
    unsigned long long sendPlainStream_[8] = {}; // 서버 → 클라 **비신뢰**, 스트림별(되받기가 실리는 자리)
    unsigned long long ackSeqApplied_ = 0; // 되받기 가지를 탄 횟수 (0 이면 분기가 안 도는 것)
    unsigned long long ackSeqDiffer_ = 0;  // ✖ 09-21 폐기 — 뜻 없는 견줌이었다(되받기 가지 주석)
    unsigned long long ackSeqChanged_ = 0; // 되받기 **값이 직전과 달라진** 횟수 — 0 이면 클라 순번이 멈춤
    uint8_t ackLastSent_ = 0;              // 마지막으로 실어 보낸 되받기 값
    // B-1b 되받기 — ⚠ **기본 끔.** 09-18 실측에서 **효과가 없었다**(아래).
    //   켜려면 `--ack-seq`. 분기 자체는 도는 것을 계수기로 확인했으므로
    //   (되받기 37/다름 37) 배선이 아니라 **방식**이 틀린 것이다.
    //
    //   실측 표 — 세 회차가 **완전히 같은 숫자**였다(exe 는 소스보다 최신, 빌드 지문 확인함):
    //     끔(--no-ack-seq)   준비 20 · 신뢰받음 25
    //     켬(N 그대로)        준비 20 · 신뢰받음 25 · 되받기 37/다름 37
    //     켬(N+1)            준비 20 · 신뢰받음 25 · 되받기 37/다름 37
    //
    //   ⇒ 다음에 볼 것 = **스트림 번호**. 클라가 CLIENT_READY 를 보내는 스트림과
    //     우리가 답하는 스트림이 다르면 `s.seq[…]` 칸이 어긋나 되받기가 엉뚱한 데 실린다.
    //     계수기를 **스트림별로 갈라서** 세야 갈린다(「갈래를 세라」).
    // ★★★ 2026-09-18 — **증상은 없앨 수 있는데 정품 바이트와 어긋난다. 기본은 끔이다.**
    //
    //   켜면(`--ack-seq --ack-any-stream`) — `CLIENT_READY` 수신이 **18~25 → 2** 로 떨어지고
    //     클라 신뢰 큐가 0 으로 빈다(09-18 실측 · 재현 2회). 클라 재전송이 멈춘 것이다.
    //   ⚠ 그런데 **`replay_connect` 가 깨진다** — 정품 녹음에서 비신뢰 머리의 순번 바이트는
    //     **늘 `00`** 인데(XOR 0x70 을 풀면 `70` 이 `00`) 우리는 되받기 값을 싣게 된다.
    //     정품 녹음은 우리 최고 등급 근거이므로 **기본은 끔**으로 둔다.
    //   ⇒ 아직 모르는 것 — **정품은 무엇으로 클라 재전송을 멈추나.**
    //     녹음에 `0xab MSG_IMPORTANT_ACK` 가 8 건 있는데 그건 `0xcc SEQ_IMPORTANT` 전용이다.
    //     참고 — 그 녹음에서 클라는 `CLIENT_READY` 를 **4 번** 보냈다(2 번이 아니다).
    //     즉 정품도 몇 번은 재전송한다. 18~25 가 과한 것이지 2 가 정답이라는 근거는 없다.
    //   ⚠ 「켬」은 `ackAnyStream_` 과 **짝으로** 켜야 한다. 되받기를 스트림 1 에만 실으면
    //     클라가 그 패킷의 seq 를 안 읽어(09-18 실측: 받은 seq 분포에 4 가 0 건) 효과가 없다.
    bool ackRecvSeq_ = true;
    bool ackAnyStream_ = true;   // 되받기를 **스트림 0 비신뢰**에 싣는가(기본 켬 · Emit 주석의 정품 근거)
    // ★ 부활 무적을 아예 안 거는 진단 스위치(`--no-spawn-invuln`).
    //   CLIENT_READY 되풀이(B-1b 미결) 때문에 무적이 안 풀려 화면 확인이 통째로 막힌다.
    //   그 원인과 **따로** 나머지를 진행하려는 우회로다. 기본 끔.
    bool noSpawnInvuln_ = false;
    unsigned long long spawnInvulnApplied_ = 0;  // 부활 무적을 실제로 건 횟수(스위치 판정용)
    // ★ ⑰ 주문 거절 갈래 — 「주문 0」만으로는 어느 문에서 막히는지 못 가른다
    unsigned long long castRejDead_ = 0;     // 죽어 있음
    unsigned long long castRejRange_ = 0;    // 주문 번호가 범위 밖
    unsigned long long castRejUnknown_ = 0;  // 안 배운 주문
    unsigned long long castRejBusy_ = 0;     // 이미 시전 중
    unsigned long long castRejClass_ = 0;    // 직업이 못 씀
    unsigned long long castRejCant_ = 0;     // CheckPlrCantCastSpell
    unsigned long long castAccepted_ = 0;    // 대기열에 들어감
    // ★ 화면에 뜨는 글씨 — 사진에만 남아 놓치기 쉬워 로그에도 찍는다(키마다 처음 한 번)
    std::set<std::string> priMsgShown_;
    unsigned long long priMsgSent_ = 0;
    std::set<uint32_t> informShown_;   // 알림(code,val) 쌍 — 처음 한 번만 찍으려고
    // ★ ⑫ 덫 화살이 왜 사라지나 — 화면에 한 번도 안 그려지는 까닭을 가른다
    unsigned long long arrowGoneWall_ = 0;        // 벽에 닿아서
    unsigned long long arrowGoneWallAtBirth_ = 0; // 그중 태어나자마자(1틱 이내)
    unsigned long long arrowGoneAge_ = 0;         // 수명 40틱을 채워서
    unsigned long long arrowLifeSum_ = 0;         // 벽에 닿은 화살들의 수명 합(틱)
    unsigned long long arrowLifeCnt_ = 0;         // 그 개수 — 평균을 내려고
    size_t missilesPeak_ = 0;                     // 동시에 떠 있던 **최대** 수(로그 주기와 무관)
    // ★ ⑫ 덫이 겨누기에 실패한 **까닭별** 계수기. 하나로 세면 어느 문에서 막히는지 못 가른다.
    mutable unsigned long long trapFailRange_ = 0;   // ±350 밖
    mutable unsigned long long trapFailFacing_ = 0;  // 덫이 보는 쪽이 아님
    mutable unsigned long long trapFailWall_ = 0;    // 벽에 막힘
    mutable unsigned long long trapSeeOk_ = 0;       // 셋을 다 통과
    size_t lastUpdateObjects_ = 0;   // 마지막 갱신에 실은 사람 수
    // ★ 갱신을 만드는 세션 루프가 **실제로 몇 번 돌았나**. 「사람 1(갱신에 0)」이
    //   코드로 설명이 안 돼서 박은 계수기다 — 읽어서 두 번 못 찾았다(2026-09-08).
    size_t lastLoopSeen_ = 0;        // 루프가 본 세션 수(inGame 통과분)
    size_t lastSessions_ = 0;        // 그때의 sessions_.size()
    size_t mirrorAt_ = 0;   // 그 다음 41바이트 USE_MAP
    uint32_t frame_ = 0;
    uint16_t netCode_ = 0;
    uint8_t xorKey_ = 0x70;
    uint32_t nextConnectId_ = 1;
    bool inboundPlain_ = false;
    uint32_t tickMs_ = 33;      // 30Hz 목표
    uint32_t lastTickMs_ = 0;
    bool tickStarted_ = false;
    unsigned long long moves_ = 0;
    unsigned long long melees_ = 0;
    uint32_t rngState_ = 0x13572468u;   // NextRandom 의 씨앗 — 고정이라 시험이 재현된다
    // 진단(2026-09-10 sq1) — 코드6(왼클릭)이 분기에 몇 번 도달했나.
    // 클라는 231번 보냈는데 성사가 15에서 멎었다 — 유실이 파싱이냐 조건이냐를 가른다.
    unsigned long long code6Seen_ = 0;
    unsigned long long petsKilled_ = 0;
    unsigned long long soundsQueued_ = 0;
    unsigned long long soundsDropped_ = 0;
    unsigned long long soundsSent_ = 0;
    unsigned long long musicSent_ = 0;
    size_t trapsArmed_ = 0;
    std::map<uint16_t, unsigned long long> soundsDroppedIds_;
    // 묶음 8-a — 이 판의 설정. 접속 묶음의 `af`·`b0` 가 여기서 나온다
    GameSettings gameSettings_;
    std::vector<Team> teams_;   // 묶음 8-b — 이 판의 팀들
    std::vector<Vote> votes_;                         // 묶음 8-e
    std::vector<GameBall> ball_;                      // 묶음 8-d — 0 또는 1
    std::vector<BallGoal> goals_;
    std::vector<std::pair<double, double>> ballStarts_;
    bool roundOver_ = false;
    bool timerLastRoundOver_ = false;   // 09-21 — 판 끝 상태가 **바뀐 틱**에만 시계를 보낸다
    std::vector<Flag> flags_;
    uint16_t caps_[3] = {0, 0, 0};   // 팀별 득점 (1·2 만 쓴다)
    int questTotal_ = 0;
    int questKilled_ = 0;
    int questStage_ = 0;          // 지금 스테이지(0부터)
    int questCenterX_ = 0, questCenterY_ = 0;
    double questSpread_ = 0;
    bool questAnnounced_ = false; // 첫 스테이지 알림을 보냈나
    std::vector<Creature> pets_;
    // 날아가는 것에 매길 번호.
    // ⚠ **진짜 서버가 번호를 어떻게 매기는지는 모른다.** 캡처에서 사람이 715·720·728 을
    //   받은 것만 봤다. 사람과 안 부딪히게 넉넉히 띄운 자리에서 시작할 뿐이다.
    // 사람 오브젝트 번호. 정품이 713·715 를 쓰는 자리라 그 언저리에서 시작한다.
    uint16_t nextPlayerNetCode_ = 713;
    // ★★ 사람 하나가 쓰는 오브젝트 번호는 **다섯 칸**이다 — 본인 + 장비 넷.
    //   정품 2인 덤프에서 그대로 읽힌다(2026-09-09):
    //     사람 715 → 장비 716·717·718·719 │ 사람 720 → 장비 721·722·723·724
    //   내 서버는 사람만 713·714 로 하나씩 올리고 장비는 **녹음본 번호(716~719)를
    //   둘 다 그대로** 썼다. 같은 번호를 두 사람이 쓰니 클라가 하나만 그렸다 —
    //   「미니맵엔 둘인데 몸은 하나」의 원인이다.
    static constexpr uint16_t kNetCodeStride = 5;
    // ★★ 스폰 자리. **서두(첫 세계 스냅샷)의 머리 좌표에서 읽는다** — 서두가 찍힌 맵의
    //   그 자리다. 상수로 박아 두면 맵이 바뀌는 순간 **사람만 세계에서 멀리 떨어진다**.
    //   실측(2026-09-08): 서두는 estate 인데 스폰은 bludeath 좌표(3864,2534)라
    //   스냅샷 속 다른 오브젝트들이 (2500~3400, 3200~3800)에 있는데 사람만 저쪽에 있었다.
    //   ⚠ 이 파일 안에 「estate 좌표를 bludeath 에 쓰면 맵 밖」이라는 경고가 이미 있었다 —
    //     그 경고가 **반대 방향으로** 실현된 것이다.
    //   서두에서 못 읽으면 아래 기본값(09-05 bludeath 덤프 값)으로 떨어진다.
    int spawnX_ = 3864;
    int spawnY_ = 2534;
    uint16_t nextShotNetCode_ = 4096;
    std::map<uint8_t, unsigned long long> spells_;
    unsigned long long spellsOnSelf_ = 0;
    std::vector<AcceptScript> script_;   // 접속 대본
    size_t scriptAt_ = 0;
};

// 플레이어의 와이어 종류 번호. 실측으로 확인했다 —
// 상태 갱신 30,169건이 이 번호였고, thing.bin 712번 `NewPlayer`(복합)와 맞는다.
constexpr uint16_t kPlayerType = 713;

// 몬스터·소환수의 **대기 애니 번호**. 갱신 항목 덧붙이 한 바이트의 아래 4비트에 실린다.
//
// ★ 애니 번호표가 사람과 몬스터로 갈린다(`draw_debug.go` 의 두 표) —
//     사람   0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · 4 WALK …
//     몬스터 8 IDLE · 9 DIE · 10 DEAD · 11 HURT · 12 MOVE …
//   몬스터에 사람 번호(0)를 실으면 그 자리에 애니가 없어 **아무것도 안 그려진다.**
constexpr uint8_t kCreatureAnimIdle = 8;


// 한 걸음에 얼마나 미나. **2026-09-06 캡처로 실측했다** (전에는 지어낸 12.0 이었다).
//
//   표본 866 (바로 다음 틱, 프레임 차 1 짜리만)
//   가장 잦은 값 7 — 271회, 31.3%
//
// dx·dy 를 그대로 보면 길이는 일정한데 방향만 흩어진다. 잦은 것부터 —
//
//   (5,-5) 7.07 · (7,-2) 7.28 · (6,-4) 7.21 · (6,-3) 6.71 · (7,-3) 7.62
//
// 축만 딱 떨어지는 값이 아니라 6.3~7.6 에 퍼져 있는데, 이는 방향을 한 바퀴 256으로
// 나눠 쓰고 좌표를 정수로 자르기 때문이다. 아래 계산이 바로 그 짜임이라 값이 맞는다.
//
// ⚠ **느린 무리가 따로 있다** — 2.8~4.2 에 330건이 몰려 있고(빠른 쪽은 360건),
//    걷기와 달리기로 갈리는 것으로 보이나 아직 가리지 못했다. 지금은 잦은 쪽을 쓴다.
// ⚠ 클라의 예측과 맞는지는 **정품 클라로 눈으로 봐야** 안다. 아직 안 했다.
constexpr double kStepPerTick = 7.0;   // ⚠ 2026-09-13 부터 걷기에 안 쓴다 — 아래 걷기 물리 값으로 바뀜

// ★★ **걷기 물리 값** (2026-09-13 묶음 2-a · 정답지).
//   힘     = 바라보는 쪽 × 속도. 속도 = `thing.bin` NewPlayer SPEED 96 ÷ 32 = 3.0(`kPlayerWalkPerFrame`)
//            달리기면 ×2 — 커서가 몸에서 100 밖이거나 이미 달리는 중일 때(`object_update.go:150`)
//   적분   = 한 틱에 5번 · 한 번에 0.2 — 속도 += (힘 − 속도 × 감쇠) × 0.2 · 자리 += 속도 × 0.2
//            (`server.go:146` updateCollide · `GAME4_1.c:3218~3225`)
//   감쇠   = 투사체가 아니면 0.5(`server/object_type.go:344` Field13 → Float28 +112)
//   ⇒ 끝 속도 = 힘 ÷ 감쇠 — 걷기 6 · 달리기 12 유닛/틱. 위 7 과 느린 무리 2.8~4.2 의 옛 실측과는 **어긋난다**
//     (그 실측 도구의 해독이 다른 캡처에서 망가지는 것을 09-13 확인 — 관측대기).
constexpr int    kMoveSubSteps     = 5;
constexpr double kMoveSubStep      = 0.2;
constexpr double kPlayerDamping    = 0.5;
// ✖ 2026-09-22 폐기 — `kPlayerMass = 1.0` 은 **틀렸다.** thing.bin `NewPlayer MASS` 는 1 이 맞지만
//   정답지는 사람이 생길 때 `GAME3_3.c:7701` 로 **덮어쓴다**(10~30). 밀어내기 가속은 힘 ÷ 무게라
//   1 로 두면 같은 접촉에서 **15배** 튕겨 나간다. 무게는 이제 `PlayerMassOf(s)` 로 사람마다 셈한다
//   (식·검산은 `GameBalance.h` 의 `PlayerMassFromStrength`).
constexpr double kRunCursorDist    = 100.0;
constexpr uint8_t kPlayerStateWalk = 0;
constexpr uint8_t kPlayerStateRun  = 5;
constexpr uint8_t kPlayerStateIdle = 13;
constexpr uint8_t kPlayerStateAttack = 1;   // ⭕ 정답지 PlayerState1 — 공격(`object_update.go:224`)

// ★★ **맨손 공격 값** (2026-09-13 묶음 3-a · 정답지 `GAME4_3.c:4020~4066`).
//   애니   23 PUNCH_LEFT · 24 PUNCH_RIGHT(반반) · 워리어면 101분의 25 로 25 PUNCH_RIGHT_HOOK(`object.go:1453~1455`)
//   시각   (프레임 − 상태 시작) ÷ (붙드는 틱 + 1) 이 칸 수에 닿는 틱에 판정 — thing.bin 주먹 8칸 · 붙듦 0 → **8틱 뒤**
//   피해   힘 × 0.039999999 + 5(주먹) · + 10(훅) — float 로 담고 +0.5 버림(`:4037~4053` · sub_538510 `(long long)(f+0.5)`)
//   사거리 몸 반지름 + 20(`:4054`) — 전에 두던 45(지팡이와 같게 둔 우리 값)가 아니다
//   훅    맞은 쪽을 때린 쪽 반대로 힘 20(`GAME4_3.c:3683~3690` · objectApplyForce: 10×20÷질량 ÷ (거리+0.1))
constexpr float  kUnarmedReachExtra = 20.0f;
constexpr float  kUnarmedCoeffF     = 0.039999999f;
constexpr float  kHookForce         = 20.0f;
constexpr uint8_t kAnimPunchLeft = 23, kAnimPunchRight = 24, kAnimPunchHook = 25;
constexpr uint32_t kServerTickRate = 30;   // 정답지 s.TickRate() — 멀티 30(기력 회복 100/30 등)

// ★ 주문 시전(0x79)의 본문 길이. **2026-09-06 캡처 표본 83개 전수로 확정했다.**
//
//     79 │ 주문번호 1 │ 0 으로 채운 19 │ 대상 플래그 1
//
// 판정 = 22바이트(오프코드 포함) 뒤 자리가 83/83 모두 아는 오프코드였다
// (전부 0x3F PLAYER_INPUT). 다른 길이로 잘라 보면 78% 아래로 떨어진다.
constexpr size_t kTrySpellBodyLen = 21;

// 파이어볼의 주문 번호. opennox `spell` 패키지의 `SPELL_FIREBALL = ID(27)` 과 같다 —
// 와이어에 실려 온 번호가 그 표와 그대로 맞는 것을 캡처에서 확인했다.
constexpr uint8_t kSpellFireball = 27;


// SERVER_INFO 본문에서 토큰이 앉는 자리. 실측으로 확정했다.
constexpr size_t kInfoTokenOffset = 41;

// ── 접속 직후 216바이트 묶음에서 접속마다 달라지는 자리 ──────────
//
// 두 접속(키 0x70·0x2a)의 216바이트를 나란히 놓고 다른 자리만 뽑은 것이다.
// 묶음 안 메시지 경계와도 맞는다 — 6·5·7·20·49·129 로 딱 떨어진다.
constexpr size_t kJoinFrameA = 7;    // 28 FULL_TIMESTAMP 의 값 (LE32)
constexpr size_t kJoinNetCodeA = 12; // 2c JOIN_DATA 의 번호 (LE16)
constexpr size_t kJoinFrameB = 19;   // af GAME_SETTINGS 안의 프레임 (LE32)
constexpr size_t kJoinNetCodeB = 88; // 2d NEW_PLAYER 의 번호 (LE16)

// ★ 묶음 안 `2d NEW_PLAYER` 가 시작하는 자리와 길이.
//   앞의 다섯(6+5+7+20+49 = 87) 뒤에 129바이트로 붙어 216 으로 딱 떨어진다.
//   사람이 둘 이상일 때 **서로를 소개**하려면 이 조각만 떼어 다시 보내야 한다.
constexpr size_t kJoinNewPlayerAt = 87;
constexpr size_t kJoinNewPlayerLen = 129;

// ★★ `2d NEW_PLAYER` 안 **상태 플래그**(4바이트, LE). 오픈녹스 원본에서 자리를 얻었다:
//   `nox_xxx_netNewPlayerMakePacket_4DDA90` 이 `buf[112] = Field3680 & 0x423` 을 쓴다.
//     bit0 = **HUD 를 안 그린다**(`nox_client_onClientStatusA`: `v&1` → renderGUI=false)
//     bit1 = 관전 중 (`observeClear` 가 이 비트를 보고 카메라를 푼다)
//   내가 서두에서 베껴 쓰던 값이 `01 00 00 00` 이라 **HUD 가 영영 안 떴다**(2026-09-09 실측).
//   정품은 같은 사람에게 `2d` 를 두 번 보낸다 — 처음 `01`, 놀기 시작할 때 `00`.
constexpr size_t kNewPlayerStatusAt = 112;

// ★ NEW_PLAYER 안의 플레이어 정보 97바이트는 **거의 그대로 클라가 보낸 것**이다.
//   `0x20 CLIENT_ACCEPT` 본문의 앞 97바이트와 견주니 —
//     첫 접속  97자리 **전부 일치**
//     둘째 접속 95자리 일치, **89·91번 두 자리만 서버가 채웠다**(0x20·0x32)
//   그래서 서버가 지어낼 것은 그 두 자리뿐이고, 나머지는 받은 것을 옮기면 된다.
constexpr size_t kJoinPlayerInfo = 90;
constexpr size_t kJoinPlayerInfoLen = 97;

// ⚠ 아직 정체를 모르는 두 자리 (묶음 **본문** 기준. 조각으로는 여기에 2를 더한다)
//   첫 접속은 둘 다 0 이고 둘째 접속에서만 값이 붙었다. 접속 순서·팀·관전 같은
//   「그 사람의 상태」로 보이지만 확인 못 했다.
constexpr size_t kJoinUnknownA = kJoinPlayerInfo + 89;   // 179
constexpr size_t kJoinUnknownB = kJoinPlayerInfo + 91;   // 181

// USE_MAP 본문 — 이름 32 + CRC 4 + 프레임 4
constexpr size_t kUseMapFrame = 37;

}  // namespace nox
