// GameBalance.cpp — 표를 찾는 자리. 값 자체는 헤더에 출처와 함께 있다.
#include "nox/GameBalance.h"

namespace nox {

const MonsterStats* FindMonster(uint16_t wireType) {
    for (const MonsterStats& m : kMonsters)
        if (m.wireType == wireType) return &m;
    return nullptr;
}

namespace {

struct EquipBit { const char* name; uint32_t bit; };

// ◑ opennox `server/object_weapon.go` weaponTable — 원문 순서 그대로 27칸.
constexpr EquipBit kWeaponBits[] = {
    { "Bow", 0x4 },                  { "CrossBow", 0x8 },
    { "Longsword", 0x200 },          { "GreatSword", 0x400 },
    { "MorningStar", 0x800 },        { "BattleAxe", 0x1000 },
    { "OgreAxe", 0x2000 },           { "Sword", 0x100 },
    { "WarHammer", 0x4000 },         { "RoundChakram", 0x40 },
    { "FanChakram", 0x80 },          { "Quiver", 0x2 },
    { "StaffWooden", 0x8000 },       { "SulphorousFlareWand", 0x10000 },
    { "SulphorousShowerWand", 0x20000 },
    // ⚠ 원문이 LesserFireballWand 와 DemonsBreathWand 에 **같은 비트**(0x80000)를 준다.
    { "LesserFireballWand", 0x80000 }, { "DemonsBreathWand", 0x80000 },
    { "DeathRayWand", 0x400000 },    { "InfinitePainWand", 0x200000 },
    { "FireStormWand", 0x100000 },   { "ForceWand", 0x40000 },
    { "OblivionHalberd", 0x800000 }, { "OblivionHeart", 0x1000000 },
    { "OblivionWierdling", 0x2000000 }, { "OblivionOrb", 0x4000000 },
    { "Flag", 0x1 },
};

// ◑ opennox `server/object_armor.go` armorTable — 원문 순서 그대로 26칸.
constexpr EquipBit kArmorBits[] = {
    { "LeatherArmor", 0x8000 },      { "ChainTunic", 0x10000 },
    { "Breastplate", 0x20000 },      { "LeatherHelm", 0x200000 },
    { "SteelHelm", 0x400000 },       { "SteelShield", 0x2000000 },
    { "WoodenShield", 0x1000000 },   { "StreetShirt", 0x400 },
    { "StreetPants", 0x4 },          { "StreetSneakers", 0x1 },
    { "LeatherBoots", 0x40 },        { "LeatherArmoredBoots", 0x80 },
    { "PlateBoots", 0x100 },         { "MedievalPants", 0x8 },
    { "LeatherLeggings", 0x10 },     { "ChainLeggings", 0x20 },
    { "PlateLeggings", 0x200 },      { "MedievalShirt", 0x800 },
    { "WizardRobe", 0x4000 },        { "LeatherArmbands", 0x1000 },
    { "PlateArms", 0x2000 },         { "MedievalCloak", 0x2 },
    { "ChainCoif", 0x40000 },        { "WizardHelm", 0x80000 },
    { "ConjurerHelm", 0x100000 },    { "OrnateHelm", 0x800000 },
};

bool SameName(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

}  // namespace

uint32_t EquipBitFor(const char* thingName, bool* isWeapon) {
    for (const EquipBit& e : kWeaponBits) {
        if (SameName(e.name, thingName)) {
            if (isWeapon) *isWeapon = true;
            return e.bit;
        }
    }
    for (const EquipBit& e : kArmorBits) {
        if (SameName(e.name, thingName)) {
            if (isWeapon) *isWeapon = false;
            return e.bit;
        }
    }
    if (isWeapon) *isWeapon = false;
    return 0;   // 모르면 0 — 겉모습을 안 보낸다 (짐작으로 안 채운다)
}

}  // namespace nox
