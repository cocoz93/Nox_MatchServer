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
#include "nox/JoinBundle.h"   // 묶음 8-a GameSettings
#include "nox/Messages.h"   // InformCode
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

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
//   ⚠ 아직 안 옮긴 것 — Armor·Weapon·Food·SpellBook·Trap·Ammo·Crown·Treasure·Oblivion·AnkhTradable
enum class PickupFn : uint8_t { Default = 0, Potion, Gold };
inline PickupFn PickupFnOf(const std::string& name) {
    if (name == "PotionPickup") return PickupFn::Potion;
    if (name == "GoldPickup") return PickupFn::Gold;
    return PickupFn::Default;
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
enum class PropKind : uint8_t { Door = 0, Obelisk };

// 문이 다가가면 열리는 거리 · 오벨리스크가 마나를 채워 주는 거리.
// ➖ **우리 값이다.** 정품 수치는 파일에도 캡처에도 없다.
// 문은 벽 한 칸(23)의 세 배쯤, 오벨리스크는 「옆에 선다」니 그보다 가깝게 잡았다.
constexpr double kDoorOpenDist = 70.0;
constexpr double kObeliskDist = 50.0;
// 오벨리스크가 한 틱에 채우는 마나. ➖ 우리 값 — gamedata 에 오벨리스크 항목이 없고
// 주석이 가리키는 `OblivionStaffRechargeRate = 2` 하나가 지팡이 쪽에서 겸한다.
// 30Hz 에 2 면 초당 60 이라 너무 빠르다. 여섯 틱에 1 로 둔다(초당 5).
constexpr uint32_t kObeliskEveryTicks = 6;
constexpr int kObeliskManaPerTick = 1;
// 문 각도 — `0xb2` 의 셋째 바이트. 정품 표본에 `00`·`10`·`18` 이 보였다(`CAP_server_0906`).
// ◑ 값의 뜻(각도 단위)은 모른다. **닫힘 0 · 열림 0x18** 두 값만 쓴다.
constexpr uint8_t kDoorClosed = 0x00;
constexpr uint8_t kDoorOpen = 0x18;
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
//     비트         | 보내는 것        | 오프코드 | 우리
//     `0x10000`    | 그림 칸          | `0x6B`   | ❌ 안 쓴다(클라가 굴린다 — 2026-09-11 실측)
//     `0x20000`    | 유닛 현재 체력   | `0x41`   | `UnitHealth`  ← 먼저 붙인다
//     `0x40000`    | 숨김             | `0x38`   | `Hidden`   (아직)
//     `0x400000`   | 높이             | `0x9F`   | `Height`   (아직)
//     `0x800000`   | 마법부여         | `0x5A`   | `Enchant`  (아직)
//     `0x2000000`  | 팀 기지          | `0x67`   | `TeamBase` (아직)
struct PendingReport {
    enum class What : uint8_t { UnitHealth, Hidden, Height, Enchant, TeamBase };
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
    };
    AIStackItem aiStack[kAIStackMax];
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
    };
    std::vector<Carried> inventory;
    int       strength  = 0;       // 맨손·무기 피해 계산에 쓴다
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
    uint8_t attackAnim = 0;        // 플레이어 +8 (Field8) — 공격 애니 번호(23·24 주먹 · 25 훅). 0 이면 새로 뽑는다
    uint32_t attackReadyFrame = 0; // 갱신 데이터 +0 (Field0) — 이 프레임 전에는 상태 1 로 못 들어간다. 0 이면 바로 가능
    uint32_t stateFrame = 0;       // 오브젝트 +136 (Field34) — 상태가 바뀐 프레임
    uint8_t animFrame = 0;         // 갱신 데이터 +236 (Field59_0) — 스냅샷에 실는 애니 칸
    uint8_t facingPrev = 0;        // 오브젝트 +124 (Direction1) — 지난 틱 끝의 바라보는 쪽. 공격 방향 판정이 이것을 쓴다
    uint8_t facingInput = 0;       // 이번 틱에 온 CCOrientation — ImplB 에서 facing 으로 옮긴다
    bool hasFacingInput = false;
    double forceX = 0, forceY = 0; // 남이 준 힘(+88 ForceVec 몫) — 적분 때 더하고 지운다
    double prevX = 0, prevY = 0;   // +72 PrevPos — 지난 틱 시작 자리(막기 판정이 때린 쪽의 이 자리를 본다, 3-e2)
    int64_t debugWeapon = -1;      // 시험용 바꿔 들기 — −1 기본 장비 · 0 맨손 · 그 밖 kWeapons 클래스 비트(주워 들기는 묶음 5)
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
    uint32_t importantAcked = 0;   // 진단 — 확인받아 지운 수
    uint32_t importantResent = 0;  // 진단 — 다시 실은 수
    uint8_t team = 0;              // 0 = 무소속 · 1 = 빨강 · 2 = 파랑
    // ★ **접속한 차례.** 팀을 번갈아 나눌 때 쓴다.
    //   ⚠ `sessions_` 를 그냥 돌면 **주소 문자열 사전순**이라 들어온 차례가 아니다 —
    //     포트가 어떻게 뽑히느냐에 따라 A·B 팀이 뒤집혔다(2026-09-11 감사).
    uint32_t joinOrder = 0;
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
constexpr uint32_t kSessionTimeoutTicks = 900;

// 판 시계를 얼마마다 뿌리나. 실측 표본은 **2초 남짓 간격**이었다
// (15:05:08.299 · 10.538 · 12.709 · 14.867 — 2.2초씩). 30Hz 기준 그만큼으로 둔다.
constexpr uint32_t kTimerEveryTicks = 66;

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
    bool DebugTraceRay(float x1, float y1, float x2, float y2) const { return MapTraceRay(x1, y1, x2, y2, 0x1); }
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
    };
    void SetWorldStatics(std::vector<WorldStatic> v) { statics_ = std::move(v); }

    // ── 다루는 것 — 문·오벨리스크 (P5) ────────────
    //
    // 집는 게 아니라 **다가가면 반응하는** 것들이다. estate 실측 —
    // WoodenDoor 3 · ArchedHalfDoor 2 · ObeliskPrimitive 2.
    struct WorldProp {
        uint16_t netCode = 0;
        uint16_t type = 0;
        int x = 0, y = 0;
        PropKind kind = PropKind::Door;
        bool open = false;          // 문만 쓴다
        uint32_t nextManaFrame = 0; // 오벨리스크만 쓴다
    };
    void SetWorldProps(std::vector<WorldProp> v) { props_ = std::move(v); }
    size_t WorldPropCount() const { return props_.size(); }
    // 시험용 — 그 문이 열려 있나
    bool PropOpen(uint16_t netCode) const {
        for (const auto& p : props_) if (p.netCode == netCode) return p.open;
        return false;
    }
    unsigned long long doorsOpened() const { return doorsOpened_; }
    unsigned long long obeliskMana() const { return obeliskMana_; }
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
    void SetWaypoints(std::vector<std::pair<float, float>> v) { waypoints_ = std::move(v); AIPathBakeWaypoints(); }
    size_t WaypointCount() const { return waypoints_.size(); }
    size_t PlayerStartCount() const { return playerStarts_.size(); }
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
        return true;
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
    // 시험·로그용 —  x41 을 몇 번 보냈나.
    unsigned long long unitHealthSent() const { return unitHealthSent_; }
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
    // 시험용 — 그 사람의 소환수 체력(없으면 0). 위 손잡이가 왜 실패했는지 가르는 데 쓴다.
    uint16_t DebugOwnedPetHealth(const std::string& owner) const {
        for (const auto& k : pets_) if (k.owner == owner) return k.health;
        return 0;
    }
    uint16_t CapturesOf(uint8_t team) const { return team < 3 ? caps_[team] : 0; }
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

    // ★ 시험용 — 첫 세계 스냅샷을 **다음 틱에** 이 사람에게 보내도록 예약한다.
    void DebugArmSnapshot(const std::string& addr) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.snapshotIn = 0;
    }

    // ★ 시험용 — 실제 자리(`posX`/`posY`)와 그것을 통째로 옮기기.
    //   `DebugMoveTo` 는 `x`/`y` 만 넣고 틱이 옮긴다 — 이쪽은 **둘 다** 넣는다
    double DebugPosX(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.posX; }
    double DebugPosY(const std::string& a) const { auto i = sessions_.find(a); return i == sessions_.end() ? 0.0 : i->second.posY; }
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
        bool obj = false;
        uint16_t objType = 0;
    };
    // ★ 시험용 — 피해 함수(3-e)를 바로 부른다. pet < 0 이면 사람(addr), 아니면 소환수 자리 번호
    void DebugHurt(const std::string& addr, int pet, int dmg, const std::string& by, uint8_t dtype,
                   std::vector<OutPacket>& out, const AttackSrc* src = nullptr) {
        HurtTarget(Target{ pet < 0 ? addr : std::string(), pet }, dmg, out, by, dtype, src);
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
    void DebugAIProcDeps(size_t i, std::vector<OutPacket>& out) { if (i < pets_.size()) AIProcDeps(pets_[i], out); }
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
    int  DebugAIPop(size_t i) { return i < pets_.size() ? AIPop(pets_[i]) : -1; }
    void DebugAITick(size_t i, std::vector<OutPacket>& out) { if (i < pets_.size()) AITick(pets_[i], out); }
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
    void DebugAIStart(size_t i, uint8_t act, std::vector<OutPacket>& out) { if (i < pets_.size()) AIStart(pets_[i], act, out); }
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
        if (i < pets_.size()) { pets_[i].enemyAddr = addr; pets_[i].enemyPet = pet; }
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
        auto i = sessions_.find(a); if (i != sessions_.end()) HarpoonBreak(i->second, false, out);
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
    void SetSpellDef(uint8_t spell, uint8_t mana, uint32_t flags, const std::vector<uint8_t>& phonemes);
    uint32_t SpellFlags(uint8_t spell) const { return spell < 137 && spellDefs_[spell].valid ? spellDefs_[spell].flags : 0u; }
    size_t CastPending() const { return castList_.size(); }
    // ★ 4-a2 — 겨눈 주문이 날리는 "Magic" 오브젝트(thing.bin). speedRaw = SPEED 원값(정답지 /32 · MISSILE ×2)
    // 이름 → 와이어 종류. thing.bin 을 읽는 쪽(main)이 넣어 준다(ThingDb::WireByName) — 시험은 손으로 넣는다
    void SetNamedType(const std::string& name, uint16_t wire) { namedTypes_[name] = wire; }
    uint16_t NamedType(const std::string& name) const {
        auto it = namedTypes_.find(name);
        return it == namedTypes_.end() ? 0 : it->second;
    }
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
    void DebugLeave(const std::string& addr) { auto it = sessions_.find(addr); if (it != sessions_.end()) it->second.inGame = false; }
    int DebugDurUpdates() const { return debugDurUpdates_; }
    uint8_t DebugDurSpellAt(size_t i) const {
        size_t k = 0;
        for (const auto& d : durSpells_) if (k++ == i) return d.spell;
        return 0;
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
    // ★ 진단 — 들어올 때 체력(0 이면 안 건드림). **최대 체력은 그대로** 둔다.
    void SetStartHealth(int v) { startHealth_ = v; }
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
    // 지금 불려 나와 있는 것의 수.
    size_t pets() const { return pets_.size(); }
    // 불려 나온 것이 죽은 횟수.
    unsigned long long petsKilled() const { return petsKilled_; }
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
    void HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                    const std::string& by = std::string(), uint8_t dtype = 0, const AttackSrc* src = nullptr);
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
        DurFns fns;
        std::vector<DurSpell> sub108;    // +108 하위(연쇄 번개 조각 · 4-d)
    };
    bool DurNew(uint8_t spell, const std::string& a2, const std::string& obj3, bool u3Glyph, const SpellArg& arg, int lvl,
                const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out);                          // spellsDuration.New
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
    int CastMark(uint8_t spell, Session& caster, const SpellArg& arg, std::vector<OutPacket>& out);    // sub_52CBD0
    int DurTeleMarkCreate(DurSpell& d, std::vector<OutPacket>& out);                                   // sub_5305D0
    int DurTeleMarkTick(DurSpell& d, std::vector<OutPacket>& out);                                     // sub_530650
    int DurTelePopCreate(DurSpell& d, std::vector<OutPacket>& out);                                    // castTele_530820
    int DurTelePopTick(DurSpell& d, std::vector<OutPacket>& out);                                      // sub_530880
    void SendTeleportFx(float ox, float oy, float nx, float ny, std::vector<OutPacket>& out);          // netSendPointFx 137 두 번
    int DurBlinkCreate(DurSpell& d, std::vector<OutPacket>& out);                                      // spellBlink2_530310
    int DurBlinkTick(DurSpell& d, std::vector<OutPacket>& out);                                        // spellBlink1_530380
    // teleportToMB_4E7190 — 그 자리로 옮긴다(물리 자리·옛 자리칸 둘 다).
    void TeleportTarget(const Target& t, float x, float y);
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
                           const std::string& by, const Target& skip, std::vector<OutPacket>& out);
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
    void SpawnCreatureMissile(Creature& k, const char* name, float tx, float ty, std::vector<OutPacket>& out);
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
    bool AIPathFindWaypoint(const Creature& k, float& outX, float& outY);
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
    bool CanInteractFrom(const Creature& k, const Target& t) const;
    void SendGold(Session& s, std::vector<OutPacket>& out);
    // 묶음 7-a — 경험치 알림·주기·레벨 오름
    // 묶음 7-d — 능력(0xCD·0xCE·0xCF·0xEB)
    void SendAbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out);
    void SendAbilityState(Session& s, uint8_t abil, uint8_t state, std::vector<OutPacket>& out);
    void SendAbilityActive(Session& s, uint8_t abil, bool active, std::vector<OutPacket>& out);
    void SendAbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    bool AbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out);
    bool AbilityIsActive(const Session& s, uint8_t abil) const;
    bool AbilityDo(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    void AbilityEffect(Session& s, uint8_t abil, std::vector<OutPacket>& out);   // 7-e `do()`
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
    void HarpoonBreak(Session& s, bool emitSound, std::vector<OutPacket>& out);
    bool HarpoonCollide(MissileObj& m, const Target& targ, std::vector<OutPacket>& out);
    void HarpoonUpdate(Session& s, std::vector<OutPacket>& out);                 // 7-g
    void HarpoonPull(const Session& owner, const Target& targ, float tx, float ty);
    void AbilityUpdate(Session& s, std::vector<OutPacket>& out);
    void AbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out);
    void AbilityCancelAll(Session& s, std::vector<OutPacket>& out);
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

    static uint32_t ArmorEquipOf(const Session& s) { return nox::ArmorEquipOf(s.charClass); }
    // 두 쪽(주인 기준)이 적인가 — `server/object.go:1667` IsEnemyTo 를 사람·소환수·맵 몬스터에 맞춘 판.
    //   wild = 주인 없는 몬스터(자기 자신이 주인)
    bool OwnersEnemy(const std::string& a, bool aWild, const std::string& b, bool bWild) const;

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
    void BuildTimerStatus(uint8_t msg[13]) const;

    // 한 사람이 한 말을 모두에게. 머리는 **서버가 새로 짓는다**(남의 번호로 말하기 방지).
    void BroadcastChat(const Session& who, const uint8_t* text, size_t n,
                       std::vector<OutPacket>& out);

    // 이 사람이 지금 겨누는 쪽(라디안). 겨눔이 없거나 낡았으면 몸 바라보는 쪽.
    double AimRadians(const Session& s) const;

    // ⚠ 2026-09-13 부터 **바로 안 보낸다** — 세션의 importantQ 에 쌓고 FlushImportant 가 싣는다.
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
    static uint32_t WeaponEquipOf(const Session& s);                          // 플레이어 +4 WeaponEquip (지금은 직업 기본 장비)
    static const WeaponStats* WeaponStatsOf(const Session& s);
    bool CursorOnFriend(const Session& s) const;                             // playerAimsAtEnemy_4F9DC0 의 거꾸로
    bool MapTraceVision(float x1, float y1, float x2, float y2) const;       // `server/map.go:522` (장애물 그늘 · 벽 9)
    bool MapTraceVisionSkip(float x1, float y1, float x2, float y2, int skipObstacle) const;
    static uint8_t PlayerAnimByte(const Session& s);                          // mapPlrActionToStateId_4FA2B0
    static bool PlayerAnimFrameSent(const Session& s);                        // netPlayerObjSend_518C30 의 +236 싣는 상태
    // ── 벽 충돌 (묶음 2-b) — 정답지 이식. Server.cpp 정의 머리 주석 참조 ──
    int WallShapeAt(int gx, int gy) const;                                   // sub_57B500 (문·창·비밀 없음)
    struct TraceHit { float x = 0, y = 0; int gx = 0, gy = 0; };   // 4-a2 — 막힌 점과 칸(MapTraceRayAt outPos·outGrid)
    bool MapTraceRay(float x1, float y1, float x2, float y2, uint8_t flags, TraceHit* hit = nullptr) const;   // MapTraceRayAt
    bool MapTraceRayTile(int gx, int gy, float x1, float y1, float x2, float y2, uint8_t flags, float* hx = nullptr, float* hy = nullptr) const;
    // 원 대 벽(sub_54FEF0) — 사각형은 프레임 시작 자리(bx,by)로, 거리는 지금 자리(nx,ny)로 잰다.
    //   밀어낸 힘을 (ax,ay) 에 더하고 벽 쪽으로 가던 속도를 깎는다. 닿았으면 참.
    bool CircleVsWalls(double bx, double by, double r, double mass,
                       double nx, double ny, double& vx, double& vy, double& ax, double& ay) const;
    // 오브젝트끼리(sub_548220 거르기·나누기 → 원 대 원 550D00 · 원 대 상자 54AD50). 자기(a1)만 민다.
    //   a1 = 움직이는 사람(지금 자리 nx,ny · 프레임 시작 bx,by · 속도 vx,vy), a2 = 상대(자리·속도).
    // 4-b1 — 부딪힘 기록(collSysAddCollision_548630)의 임자. kind 1 사람 · 2 소환수 · 3 장애물
    struct ContactRef {
        uint8_t kind = 0;
        std::string addr;
        int idx = -1;
    };
    void BodyVsBody(const PhysBody& a1, double nx, double ny, double bx, double by, double vx, double vy,
                    const PhysBody& a2, double ox, double oy, double obx, double oby, double ovx, double ovy,
                    double& ax, double& ay, const ContactRef* ra = nullptr, const ContactRef* rb = nullptr) const;
    void AddContact(const ContactRef& a, const ContactRef& b) const;               // collSysAddCollision_548630(겹 없이)
    void CollideBuffs(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out);   // nox_xxx_collide_4FDF90
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
    void Emit(Session& s, bool reliable, uint8_t stream,
              const uint8_t* msg, size_t n, std::vector<OutPacket>& out);

    std::map<std::string, Session> sessions_;
    std::map<uint8_t, unsigned long long> seen_;
    unsigned long long handled_ = 0;   // 답을 만든 메시지 수
    uint32_t nextJoinOrder_ = 0;       // 접속한 차례에 매기는 번호 (팀 나누기용)
    uint32_t restartAtFrame_ = 0;      // 이 프레임에 판을 다시 연다. 0 이면 예약 없음
    unsigned long long roundsPlayed_ = 0;
    unsigned long long flagStatusSent_ = 0;   // 깃발 HUD 를 보낸 횟수 (과잉 감시용)
    unsigned long long invulnBlocked_ = 0;    // 부활 무적으로 막은 피해 횟수
    unsigned long long appearanceSent_ = 0;   // 겉모습을 남에게 알린 횟수 (P7)
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
    int forceAnimDir_ = -1;     // 진단 — 덧붙이 방향 3비트 고정(-1 이면 안 함)
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
    unsigned long long doorsOpened_ = 0;
    unsigned long long obeliskMana_ = 0;
    bool aoiOn_ = false;
    double creatureAoiHalfX_ = 0.0;   // 0 이면 크리처 시야 판정 꺼짐 (SetCreatureAoi)
    double creatureAoiHalfY_ = 0.0;
    uint32_t debugAwayAfter_ = 0;     // 진단 --creature-away-after (0 끔)
    uint32_t firstInGameFrame_ = 0;
    bool debugAwayDone_ = false;
    uint32_t debugTeamAfter_ = 0;     // 진단 --debug-team-after (0 끔)
    bool debugTeamDone_ = false;
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
    std::vector<std::pair<float, float>> waypoints_;   // 맵 길목(점멸 목적지)   // 묶음 3-c PlayerStart 자리
    bool autoRespawn_ = false;         // 묶음 3-c 자동 부활(정답지 기본 꺼짐)
    SpellDef spellDefs_[137];                      // 묶음 4-a 주문 정의
    std::vector<PhonemeNode> phonemeTree_ = std::vector<PhonemeNode>(1);   // [0] = 뿌리
    std::vector<CastEntry> castList_;              // 묶음 4-a 시전 목록 — [0] 이 머리(가장 나중에 넣은 것)
    Target spellTarget_;                           // 묶음 4-a playerinfo +3640(Obj3640) — 시전이 끝난 순간 고른 대상
    std::vector<MissileObj> missiles_;             // 묶음 4-a2 날아가는 오브젝트
    std::vector<PlacedObj> placed_;                // 묶음 4-c3 주문이 놓는 오브젝트(불꽃·독구름)
    std::map<std::string, uint16_t> namedTypes_;   // 이름 → 와이어 종류(thing.bin 에서 받는다)
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
    struct WallTile { uint8_t dir = 0; uint8_t tile = 0; bool flag80 = false; bool present = false; };
    std::vector<WallTile> wallTiles_;        // 256×256 벽 모양 (묶음 2-b)
    std::vector<uint32_t> wallDefFlags_;     // thing.bin WALL 순번 → 깃발
    std::vector<PhysBody> obstacles_;        // 묶음 2-c 장애물
    std::map<uint16_t, PhysBody> typeBodies_;   // 소환수 종류 → 몸
    static constexpr int kPlayerAnimSlots = 55;     // ThingDb kPlayerAnimTypeCount 와 같은 길이
    int playerAnimFrames_[kPlayerAnimSlots][2] = {};   // [칸 수, 붙드는 틱] — PlayerAnimFrames(i)
    // 남 항목의 덧붙이 값(자세로 보인다). 정품 표본은 0x30 인데 그 표본의 상대는
    // 화면 밖이었다 — 그려지는 값이 따로 있을 수 있어 손잡이로 뺐다.
    unsigned long long unitHealthSent_ = 0;
    unsigned long long enchantSent_ = 0;      // 0x5A 를 보낸 횟수 (R4)   //  x41 을 보낸 횟수 (R4)
    int peerPose_ = -1;   // ⭕ 2026-09-13 — 기본은 덮지 않음(0x30 은 그때 상대의 방향이었다 · Server.cpp 남 항목 주석)
    // 갱신 항목 순서를 뒤집는다(남 먼저). 「자리 탓인가 누구냐 탓인가」를 가르는 시험용.
    bool peerFirst_ = false;
    // 새 사람이 들어올 때 **상대 몫의 첫 스냅샷을 통째로** 보낸다.
    // 「스프라이트는 스냅샷에서만 만들어진다」 가설을 가르는 자리.
    bool peerSnapshot_ = false;
    // 남을 사람 아닌 종류로 보내 본다(0 이면 사람 그대로). 가르는 시험용.
    uint16_t peerType_ = 0;
    // 사람 둘 이상일 때 매 틱 f 로 다시 소개할지. 기본 꺼짐 — 켜면 클라가
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
    // 묶음 8-a — 이 판의 설정. 접속 묶음의 `af`·`b0` 가 여기서 나온다
    GameSettings gameSettings_;
    std::vector<Team> teams_;   // 묶음 8-b — 이 판의 팀들
    bool roundOver_ = false;
    uint32_t nextTimerFrame_ = 0;
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
constexpr double kPlayerMass       = 1.0;   // ⭕ thing.bin NewPlayer MASS 1 (proto_test TestThingDb 가 찍는다)
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
