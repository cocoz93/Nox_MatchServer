// ThingDb.h — thing.bin 을 서버가 **직접** 읽는다. 독립 서버 P1.
//
// 지금까지는 필요한 비트만 Go 로 뽑아 C++ 표(ThingFlags.cpp)로 구워 썼다.
// 이 파서가 서면 게임 데이터를 굽는 중간 단계가 없어진다.
//
// 포맷 (opennox/libs things 대조, 2026-09-10):
//   · 파일 전체가 Blowfish(키 7)로 잠겨 있다 — NoxCrypt.h
//   · 복호하면 섹션들 — 이름 4글자가 **거꾸로** 적힘 ("THNG" → "GNHT")
//     FLOR·EDGE·WALL·AUD ·AVNT·SPEL·ABIL·IMAG·THNG, 끝은 0 채움
//   · THNG 하나 = 이름(len8) + 속성(len8 문자열 "KEY = VALUE")들 + 빈 속성
//     - CLASS 값은 '+' 로 이어짐, COMPLEX 가 끼면 복합
//     - EXTENT 값 "CIRCLE r" 이 충돌 반지름
//     - DRAW 는 이름(len8) + **8정렬 u64 크기** + 그만큼의 짐 — 크기로 건너뛴다
//
// ★ 검증 = 기존 표와 전수 대조(proto_test TestThingDb) — 이름·복합이 한 종이라도
//   어긋나면 걸음이 밀린 것이다. 2,874종이 전부 맞아야 통과.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nox {

// SPEL 섹션 한 칸 — 이름과 그 뒤 세 바이트.
//
// ★ **첫 바이트가 마나 비용이다 (⭕ 확정, 2026-09-10).**
//   짐작이 아니라 전수 대조로 정했다 — 정품 콘솔 `list spells` 가 뱉은
//   `_evidence/CONSOLE_list_spells.txt` 115줄의 `Cost:NNN` 과
//   여기서 읽은 115개가 **값 하나까지 다 같다**(`proto_test TestSpellMana`).
//   ⚠ 콘솔은 보이는 이름(`Ring of Fire`) 차례고 파일은 속이름(`SPELL_CLEANSING_FLAME`)
//     차례라 줄 순서가 다르다. 그래서 **정렬해서 전수 대조**한다.
// ★ 2026-09-13 묶음 4-a — 칸 전체를 정답지 순서로 담는다(opennox libs `things/spell.go` ReadSpellsSect):
//   Str8 이름 · U8 마나 · U16 값 · U8 음소 수 · 음소 U8×n(0~8) · ImageRef×2 · U32 깃발 · Str8 제목 · Str16 설명
//   · Str8 시전 소리 · Str8 켜짐 소리 · Str8 꺼짐 소리("NULL" 이면 없음)
//   ⚠ 전에 「뜻 미상 두 바이트」로 둔 것은 **값(U16)** 이었다.
struct SpellRaw {
    std::string name;      // 속이름 — `SPELL_FIREBALL` 꼴
    uint8_t mana = 0;      // ⭕ 마나 비용
    uint16_t price = 0;    // 값(가게)
    std::vector<uint8_t> phonemes;   // 몸짓 음소(0~8, 4 = End) — 파일 그대로(End 는 서버가 덧붙인다)
    uint32_t flags = 0;    // things.SpellFlags(0x20 OFFENSIVE · 0x4 TARGETED …)
    std::string title;
    std::string castSound, onSound, offSound;   // 소리 이름(묶음 10)
};

// WALL 섹션 한 칸 — 벽 재질 정의 (2026-09-13 묶음 2-b).
//   ⭕ 깃발 0x2 면 **막지 않는 벽**이다(`server/wall.go:1083` mapTraceRayImpl `flags2&0x2 != 0 → nil`).
//   0x1 은 시야선용(Flag7) — 묶음 1 ⓓ3 에서 쓴다.
// ★ 묶음 10-a — thing.bin `AUD ` 칸 한 항목. 소리마다 **얼마나 멀리 들리나**와
//   **소리조각이 있나**가 여기 있다(`server/audio_event.go:300~322`).
//   ⚠ `samples` 가 0 이면 정답지는 그 소리를 **아예 안 만든다**
struct SoundDefRaw {
    std::string name;
    uint16_t flags = 0;       // 몬스터가 듣는 갈래를 가르는 깃발(0x20 걷기 · 0x40 달리기)
    int      maxDist = 600;   // 15 × i16 — 파일 값이 0 이하면 정답지 기본 600
    // ★ 0 이면 **그때그때 다 보낸다**. 0 이 아니면 그 소리는 **가장 큰 것 N개까지만** 간다
    //   (`addToList` 가 세기 내림차순으로 줄을 세우고 `EachEventBitmap` 이 N개에서 끊는다).
    //   실측 — `SecretWallOpen` 1 · `TriggerPressed` 5
    uint8_t  merged = 0;      // = 정답지 `Field20`
    int      samples = 0;     // 소리조각 수(= 정답지 `Field12`)
};

// ★ 2026-09-20 묶음 10 — thing.bin `ABIL` 칸 한 항목(능력 다섯).
//   ⭕ 정답지 `spells_abil.go:424~482 thingsRead` 가 읽는 차례 그대로다 —
//     속이름 · 값1 · 그림 셋 · 이름표 · 설명 · **소리 셋**(쓸 때 · 켜질 때 · 꺼질 때).
//   ⚠ 전에는 이 칸을 통째로 건너뛰어(`SkipABIL`) **능력 소리를 아예 못 보냈다**(⑧, 09-20 실측 「담김 0」)
struct AbilityRaw {
    std::string name;                        // `ABILITY_BERSERKER_CHARGE` 꼴
    std::string useSound, onSound, offSound; // 소리 이름(NULL 이면 빈 문자열)
};
struct WallDefRaw {
    std::string name;
    uint32_t flags = 0;
    // ★ 09-29 Z1-A — 벽 부수기가 보는 칸(`server/wall.go:60~82` ReadWall).
    //   hitMat = Field36(맞을 때 소리 표의 「맞는 쪽 재질」 · `Sub_532FE0`·`Sub_533010`) · health = Health41(부서지는 벽의 체력 · `mapWallGetHpByTile_410E20`) ·
    //   breakSound = Sound683(비면 `WallDestroyed` · `WallDef.BreakSound`)
    uint16_t hitMat = 0;
    uint8_t health = 0;
    std::string breakSound;
};
// ★ 09-23 nine-defects 4-d1 — 바닥 정의(thing.bin FLOR · `nox_thing_read_FLOR_411540` `GAME1.c:1060~1108`). 순번 = 바닥 번호(최대 176).
//   `material` = `field_36` — 발소리가 이 값으로 갈린다(`object_update.go:164~220` — 2·0x4000 소리 없음 · 8 나무 · 64 흙 · 128 물 · 0x400 눈 · 0x800 진흙 · 그 밖 돌)
struct FloorDefRaw {
    std::string name;
    int32_t material = 0;
};
// ★ 09-23 4-d3 — 바닥 가장자리 정의(thing.bin EDGE · `nox_thing_read_EDGE_411850` `GAME1.c:1152~1202`). 순번 = 가장자리 번호(최대 64).
//   칸 찾기의 `sub_411490` 이 읽는 두 바이트 — `w` = +28696(파일에서 마지막 u8 · v19) · `h` = +28697(그 앞 u8 · v17)
struct EdgeDefRaw {
    std::string name;
    uint8_t w = 0, h = 0;
};

// 사람 애니 한 칸 — PlayerDraw STAT 섹션(2026-09-13 묶음 3-a).
//   ⭕ 정답지 `server/object_type.go:1065~1080` — 이름을 playerAnimTypes 순번으로 바꿔
//     [FramesPerDir, Field8] 을 담고, `PlayerAnimFrames(i)` 가 그대로 돌려준다.
//   공격 판정 시각 = (지금 − 상태 시작) / (hold + 1) ≥ frames (`GAME4_3.c:4025~4031`).
struct PlayerAnimFrames {
    int frames = 0;   // FramesPerDir
    int hold = 0;     // Field8 — 한 칸을 몇 틱 더 붙드나
};
constexpr int kPlayerAnimTypeCount = 55;   // playerAnimTypes 길이(`server/object_type.go:1093~1149`)
// 순번 → STAT 이름. 모르는 순번이면 nullptr.
const char* PlayerAnimTypeName(int i);

struct ThingInfo {
    std::string name;
    float mass = 0.0f;   // MASS — 0 이면 파일에 없음(정답지 기본 1)
    bool hasSpeed = false;   // SPEED 가 있나(4-a2)
    int speed = 0;           // SPEED 원값 — 정답지 SpeedBase = speed/32 · MISSILE ×2
    // ★ 2026-09-13 묶음 2-c — 오브젝트끼리 충돌에 쓰는 셋.
    int shapeKind = 0;          // EXTENT — 0 없음 · 1 CENTER · 2 CIRCLE · 3 BOX (`server/object_type.go:599`)
    float boxW = 0.0f, boxH = 0.0f;
    std::string flagsRaw;       // FLAGS 원문 ('+' 로 이음)
    std::string materialRaw;    // ★ 09-23 nine-defects 4-c1 — MATERIAL 원문('+' 로 이음) · 없으면 빈 칸 → 재질 NONE(0x4000)
    bool hasCollide = false;    // COLLIDE 속성이 있나
    std::string collideRaw;
    bool complex = false;
    // EXTENT CIRCLE 의 반지름. CIRCLE 이 아니면(BOX·CENTER·없음) 0.
    float radius = 0.0f;
    // 자주 쓰는 클래스만 비트로.
    bool isMonster = false;
    bool isPlayer = false;
    bool isMissile = false;
    bool isObstacle = false;
    bool isImmobile = false;
    // 갱신·스냅샷에 실어도 되는 것 (P4-d). 정품이 네트워크로 부르는 정적은
    // **아이템뿐**이라(2026-09-10 오픈녹스 ref 전수 집계: 물약·무기·사과 — 장식 0건)
    // 이 비트로 거른다. 장식(나무·불꽃)을 실으면 클라에 이중 실체가 생겨 죽는다.
    bool isPickup = false;
    // ★★ **클라가 「집을 수 있나」를 정하는 진짜 기준.**
    //   `client/object_type.go:355-362` — WEIGHT 를 읽는 바로 그 자리에서
    //   `typ.ObjClass |= object.ClassPickup` 을 한다.
    //   **WEIGHT 가 없으면 클라가 영영 못 집는다** — 서버가 뭘 보내든 상관없다.
    //   ⚠ 우리 `isPickup` 은 CLASS 로 거른다. 기준이 다르므로 어긋날 수 있다.
    bool hasWeight = false;
    int weight = 0;
    // ★ 묶음 5 — **들 수 있는 무게의 바탕값**(`CARRYCAPACITY`).
    //   `server/object.go:330` 이 `obj.CarryCapacity = t.CarryCap` 로 그대로 옮긴다.
    //   실제 한도는 이 값의 **두 배**다 — `pickup.c` 가 `무게 > 밑천×2 − 이미 든 무게` 로 막는다
    //   (`object_pickup.go:75` · `script_ns_object.go:530` 도 `×2` 를 「최대」로 돌려준다).
    //   ⭕ 사람은 100(= 한도 200) — thing.bin `INIT = PlayerInit` 칸에서 파서로 읽었다.
    //   0 이면 아예 못 든다(`object_pickup.go:23·68`).
    int carryCap = 0;
    // ★ 묶음 5-c — **주울 때 부르는 함수 이름**(`PICKUP = PotionPickup` 꼴).
    //   정답지 등록표(`legacy/object_pickup.go:26~49`) 열여섯 — thing.bin 실측 분포는
    //   DefaultPickup 84 · ArmorPickup 54 · WeaponPickup 50 · **PotionPickup 38** ·
    //   AudEventPickup 28 · FoodPickup 16 · SpellBookPickup 12 · OblivionPickup 8 ·
    //   TrapPickup 6 · GoldPickup 6 · AmmoPickup 4 · CrownPickup 2.
    //   갈래마다 「줍는 순간 무슨 일이 나는가」가 다르다 — 물약은 **그 자리에서 마실 수도** 있다.
    std::string pickupFn;
    // ★ 09-28 — **버릴 때 부르는 함수 이름**(`DROP = ArmorDrop` 꼴 · 원문 그대로). 실측 분포 —
    //   DefaultDrop 35 · WeaponDrop 31 · ArmorDrop 27 · PotionDrop 19 · FoodDrop 9 · AudEventDrop … · TrapDrop 2
    std::string dropFn;
    // ★ 2026-09-18 ⑫ — **그리기 함수 이름**(`DRAW = VectorAnimateDraw` 꼴).
    //   클라는 이 이름으로 그리는 함수를 고른다(`client/drawable_draw.go:47` `drawFuncs[name]`).
    //   화살이 화면에 안 그려지는 까닭을 가르려면 **어느 함수를 타는지**부터 알아야 한다.
    std::string drawFn;
    // CLASS 원문 — 필터 설계·진단용. '+' 로 이어진 값 그대로.
    std::string classRaw;
    // SUBCLASS 원문 — 물약이 체력인지 마나인지가 여기 있다(`POTION+HEALTH_POTION`).
    std::string subclassRaw;
    // ★ **쓰면 무슨 일이 나는가.** `USE = PotionUse 50` 을 갈라 담는다.
    //   ⭕ 값이 파일에 있다 — 빨강·파랑 물약이 둘 다 50, 작은 물약이 3, 음식이 10.
    //   ⚠ 숫자의 뜻은 갈래마다 다르다. **회복량으로 읽어도 되는 것은
    //     `PotionUse`·`ConsumeUse` 뿐**이고, `WandUse 20` 의 20 은 회복이 아니다.
    std::string useHandler;
    int useArg = 0;
    // ★ 묶음 5-e — `USE` 의 **뒷말 전체**(처리기 이름을 뺀 나머지). 갈래마다 칸 수가 다르다:
    //     `WandCastUse 250 1.0 SPELL_PLASMA`      — 충전 250 · 재사용 1.0초 · 걸 주문
    //     `WandUse 10 Fireball 1.0 MULTI_SHOT SmallFireballWand 6`
    //   `useArg` 는 그 중 **첫 숫자**다(완드는 충전 수). 나머지는 여기서 갈래별로 읽는다.
    std::string useArgsRaw;
    // ★ 맞았을 때 부르는 피해 함수 이름(DAMAGE = DefaultDamage 꼴 · 묶음 3-e). 없으면 빈 문자열.
    //   정답지 `legacy/object_damage.go` 등록표: DefaultDamage · PlayerDamage · SkeletonDamage · StoneDamage ·
    //   MechGolemDamage · FlammableDamage · BlackPowderDamage · ArmorDamage · WeaponDamage · BallDamage · MonsterGeneratorDamage
    std::string damageFn;
    // ★ 주기·닿음 함수 이름과 인자(묶음 4-c3). `UPDATE = LifetimeUpdate 90` · `COLLIDE = DamageCollide 5 FIRE` 꼴.
    //   정답지는 이 이름으로 함수를 붙인다(`legacy/object_update.go`·`object_collide.go` 등록표).
    //   불꽃·독구름 같은 「주문이 놓는 것」이 무슨 일을 하는지가 여기 적혀 있다 — **짐작하지 말고 이 값을 볼 것**.
    std::string updateFn;
    std::string updateArgs;
    std::string collideFn;
    std::string collideArgs;
    // ★ 묶음 9-b — `INIT = SkullInit 0 -1` 꼴. **덫이 보는 쪽이 맵이 아니라 여기 있다.**
    //   정답지에서 오브젝트의 초기자료(`+692`)는 **그 종류의 초기자료를 그대로 복사한 것**이고
    //   (`server/object.go:343~346`), 맵의 기본 짐(`DefaultXfer`)은 그 값을 안 싣는다.
    //   ⇒ 같은 종류의 덫은 **전부 같은 쪽**을 본다
    std::string initFn;
    std::string initArgs;
};

// '+' 로 이은 이름 목록 → 비트 (2026-09-13 묶음 2-c). 이름 순서 = 비트 순서
//   (opennox libs `object/flags.go` FlagsNames · `object/class.go` ClassNames). 모르는 이름·NULL 은 0.
uint32_t ParseObjectFlags(const std::string& raw);
uint32_t ParseObjectClass(const std::string& raw);
// ★ 09-23 nine-defects 4-c1 — 재질(opennox libs `object/material.go` MaterialNames — FLESH 1 · CLOTH 2 · ANIMAL_HIDE 4 · WOOD 8 ·
//   METAL 0x10 · STONE 0x20 · EARTH 0x40 · LIQUID 0x80 · GLASS 0x100 · PAPER 0x200 · SNOW 0x400 · MUD 0x800 · MAGIC 0x1000 ·
//   DIAMOND 0x2000 · NONE 0x4000). **MATERIAL 줄이 없으면 NONE**(`server/object_type.go:196`), 있으면 0 에서 OR(`:256~264`)
uint32_t ParseMaterial(const std::string& raw);
// ★ 09-23 4-c2 — 갑옷 SUBCLASS(libs `object/subclass_armor.go` — HELMET 1 · SHIELD 2 · BREASTPLATE 4 · ARM_ARMOR 8 · PANTS 0x10 ·
//   BOOTS 0x20 · SHIRT 0x40 · LEG_ARMOR 0x80 · BACK 0x100). ARMOR 클래스 물건의 SUBCLASS 원문에만 쓴다
uint32_t ParseArmorClass(const std::string& raw);

// ★ 09-23 nine-defects 4-c3b3a — soundset.bin(암호 5) — 몬스터 소리 모음(`nox_xxx_parseSoundSetBin_424170` · `GAME1_1.c:4741~4773`).
//   꼴 = 모음 이름 → (칸 이름 · 소리 이름) 짝 … → `END`. 모르는 칸 이름이면 정답지도 읽기 실패(`:4766~4768`).
//   칸 18 가지와 자리(+4 부터 4씩)는 정답지 이름표(`blob_587000.dat` 0x587000+64704 — 파일을 열어 읽음)의 순서 그대로다.
//   몬스터는 **자기 종류 이름과 같은 이름의 모음**을 받는다(`unit_monster.go:100~101` · 같은 이름이 둘이면 **뒤에 적힌 것** — 목록 앞에 붙여 앞에서 찾는다 `:4757~4760·4818`)
constexpr int kSoundSetFields = 18;
extern const char* const kSoundSetFieldNames[kSoundSetFields];
constexpr int kSoundSetMeleeHitting = 7;   // +32 MELEE_ATTACK_HITTING_SOUND — 때린 이가 몬스터일 때 맞는 쪽 소리를 가르는 칸(`GAME3_2.c:7252~7254`)
struct SoundSet {
    std::string name;
    std::string sound[kSoundSetFields];    // 소리 이름 그대로("NULL" 포함) · 안 적힌 칸은 빈 글
};
// 풀린 바이트를 읽는다. 실패면 false · err 에 까닭
bool ParseSoundSets(const std::vector<uint8_t>& plain, std::vector<SoundSet>& out, std::string* err);
// 이름으로 찾기 — 같은 이름이 여럿이면 뒤에 적힌 것(정답지 목록 순서). 없으면 nullptr
const SoundSet* FindSoundSet(const std::vector<SoundSet>& sets, const std::string& name);
// 파일을 풀어(암호 5) 읽는다 — main 은 `windows.h` 가 `DecryptFile` 이름을 바꿔치기하므로 이것을 부른다
bool LoadSoundSets(const std::string& path, std::vector<SoundSet>& out, std::string* err);

class ThingDb {
public:
    // thing.bin 을 읽는다. 실패면 false — reasons_ 에 까닭이 남는다.
    bool Load(const std::string& path);

    // ★ 와이어 종류 번호 = 순번 + 1 (실측 — genthings.go 의 대조 주석).
    //   0 번은 비어 있다.
    const ThingInfo* ByWire(uint16_t wire) const {
        if (wire == 0 || wire > things_.size()) return nullptr;
        return &things_[wire - 1];
    }
    size_t Count() const { return things_.size(); }

    // 이름 → 와이어 번호. 맵 파일은 오브젝트를 이름으로 담으니(P4)
    // 서버가 갱신에 실으려면 이 변환이 있어야 한다. 못 찾으면 0.
    uint16_t WireByName(const std::string& name) const {
        for (size_t i = 0; i < things_.size(); ++i)
            if (things_[i].name == name) return static_cast<uint16_t>(i + 1);
        return 0;
    }
    const std::string& Error() const { return error_; }

    // ── 주문 (SPEL 섹션) ────────────────────────────────
    const std::vector<SpellRaw>& Spells() const { return spells_; }
    // ── 벽 재질 (WALL 섹션) ── 맵 벽 재질 바이트 = 이 순번
    const std::vector<WallDefRaw>& Walls() const { return walls_; }
    const std::vector<FloorDefRaw>& Floors() const { return floors_; }   // ★ 4-d1
    const std::vector<EdgeDefRaw>& Edges() const { return edges_; }      // ★ 4-d3
    const std::vector<SoundDefRaw>& Sounds() const { return sounds_; }
    // ── 능력 (ABIL 섹션) ── 능력 소리 셋
    const std::vector<AbilityRaw>& Abilities() const { return abilities_; }
    // ── 사람 애니 (THNG 의 DRAW = PlayerDraw) ── 없는 순번은 {0,0}(정답지도 0,0)
    const PlayerAnimFrames& PlayerAnim(int i) const {
        static const PlayerAnimFrames kNone;
        return (i >= 0 && i < kPlayerAnimTypeCount) ? playerAnims_[i] : kNone;
    }
    int PlayerAnimCount() const { return playerAnimSeen_; }

    // 속이름으로 마나 비용을 찾는다. 없으면 `have` 가 false — **0 과 「모름」을 가른다.**
    // ⚠ 이걸 안 가르면 표에 없는 주문이 「공짜」가 된다.
    uint8_t ManaByName(const std::string& name, bool* have = nullptr) const {
        for (const auto& s : spells_) {
            if (s.name != name) continue;
            if (have) *have = true;
            return s.mana;
        }
        if (have) *have = false;
        return 0;
    }

private:
    std::vector<ThingInfo> things_;
    std::vector<SpellRaw> spells_;
    std::vector<WallDefRaw> walls_;
    std::vector<FloorDefRaw> floors_;   // ★ 4-d1
    std::vector<EdgeDefRaw> edges_;     // ★ 4-d3
    std::vector<SoundDefRaw> sounds_;
    std::vector<AbilityRaw> abilities_;
    PlayerAnimFrames playerAnims_[kPlayerAnimTypeCount];
    int playerAnimSeen_ = 0;   // 표에 담긴 STAT 수(진단)
    std::string error_;
};

}  // namespace nox
