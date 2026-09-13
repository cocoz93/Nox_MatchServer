// ThingDb.cpp — thing.bin 파서. 짜임 근거는 전부 opennox/libs things (2026-09-10 대조).
//
// 걸음이 한 바이트라도 밀리면 그 뒤 이름들이 전부 깨진다 — 그래서 검증이
// 「이름 2,874종 전수 대조」다. 밀림은 조용히 못 지나간다.
#include "nox/ThingDb.h"

#include <cctype>
#include <cstring>
#include <algorithm>

#include "nox/NoxCrypt.h"

namespace nox {
namespace {

// 복호된 버퍼 위를 걷는 커서. Go Reader 의 read/skip 과 1:1.
struct Cur {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t off = 0;
    bool bad = false;
    std::string* err = nullptr;

    bool Need(size_t k) {
        if (bad) return false;
        if (off + k > n) {
            bad = true;
            if (err && err->empty()) *err = "buffer underrun at " + std::to_string(off);
            return false;
        }
        return true;
    }
    void Fail(const std::string& why) {
        if (!bad && err && err->empty()) *err = why + " at " + std::to_string(off);
        bad = true;
    }
    bool Eof() const { return off >= n; }
    uint8_t U8() { if (!Need(1)) return 0; return p[off++]; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = p[off] | (p[off+1] << 8); off += 2; return v; }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = p[off] | (p[off+1] << 8) | (p[off+2] << 16) |
                     (static_cast<uint32_t>(p[off+3]) << 24);
        off += 4; return v;
    }
    // ★ 8정렬 u64 — Go readU64align 그대로: **스트림 오프셋을 8의 배수로 맞춘 뒤** LE 8바이트.
    uint64_t U64Align() {
        const size_t over = off % 8;
        if (over) { if (!Need(8 - over)) return 0; off += 8 - over; }
        if (!Need(8)) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[off + i];
        off += 8; return v;
    }
    void Skip(size_t k) { if (Need(k)) off += k; }
    void SkipBytes8() { const uint8_t sz = U8(); Skip(sz); }
    void SkipBytes16() { const uint16_t sz = U16(); Skip(sz); }
    std::string Str8() {
        const uint8_t sz = U8();
        if (!Need(sz)) return {};
        std::string s(reinterpret_cast<const char*>(p + off), sz);
        off += sz; return s;
    }
    // 섹션 이름 — 4바이트를 읽어 **뒤집는다** (파일에 거꾸로 적혀 있다)
    std::string Sect() {
        if (!Need(4)) return {};
        char c[4] = { static_cast<char>(p[off+3]), static_cast<char>(p[off+2]),
                      static_cast<char>(p[off+1]), static_cast<char>(p[off]) };
        off += 4;
        return std::string(c, 4);
    }
    bool CheckEnd() {
        const std::string s = Sect();
        if (bad) return false;
        if (s != "END ") { Fail("expected END, got '" + s + "'"); return false; }
        return true;
    }
};

// image.go skipImageRef — i32 가 -1 이면 종류1+이름(len8)이 더 붙는다
void SkipImageRef(Cur& c) {
    const int32_t ind = static_cast<int32_t>(c.U32());
    if (ind != -1) return;
    c.Skip(1);
    c.SkipBytes8();
}
void SkipImageRefs8(Cur& c) {
    const uint8_t k = c.U8();
    for (int i = 0; i < k && !c.bad; ++i) SkipImageRef(c);
}

// floor.go
void SkipFLOR(Cur& c) {
    c.Skip(4);
    c.SkipBytes8();
    c.Skip(3 * 1 + 2 * 4 + 1);
    const int n1 = c.U8(), n2 = c.U8(), n3 = c.U8();
    c.Skip(1);
    const int n = n1 * n2 * n3;
    for (int i = 0; i < n && !c.bad; ++i) SkipImageRef(c);
    c.CheckEnd();
}

// edge.go
void SkipEDGE(Cur& c) {
    c.Skip(4);
    c.SkipBytes8();
    c.Skip(2 * 4 + 1);
    const int n1 = c.U8();
    c.Skip(2);
    const int n2 = c.U8(), n3 = c.U8();
    const int n = 2 * n1 * (n2 + n3);
    for (int i = 0; i < n && !c.bad; ++i) SkipImageRef(c);
    c.CheckEnd();
}

// wall.go — ★ 2026-09-13 이름과 깃발을 담는다(`server/wall.go:60` ReadWall: 이름 뒤 첫 u32 가 Flags32).
//   맵 벽의 재질 바이트가 이 목록의 순번이다(`mapTraceRayImpl` 의 `DefByInd(wl.Tile1)`).
void SkipWALL(Cur& c, std::vector<WallDefRaw>* out) {
    c.Skip(4);
    WallDefRaw d;
    d.name = c.Str8();
    d.flags = c.U32();
    c.Skip(2 * 4 + 2 * 1);
    if (out && !c.bad) out->push_back(d);
    const uint64_t v1 = c.U64Align();
    for (int i = 0; i < static_cast<int>(static_cast<uint8_t>(v1)) && !c.bad; ++i) c.SkipBytes8();
    for (int i = 0; i < 3; ++i) c.SkipBytes8();
    c.Skip(1);
    for (int i = 0; i < 15 && !c.bad; ++i) {
        const uint64_t v2 = c.U64Align();
        for (int j = 0; j < static_cast<int>(static_cast<uint8_t>(v2)) && !c.bad; ++j) {
            for (int k = 0; k < 4 && !c.bad; ++k) {
                c.Skip(2 * 4);
                SkipImageRef(c);
            }
        }
    }
    c.CheckEnd();
}

// audio.go — ⚠ END 가 없다
void SkipAUD(Cur& c) {
    const int32_t n = static_cast<int32_t>(c.U32());
    for (int i = 0; i < n && !c.bad; ++i) {
        c.SkipBytes8();
        c.Skip(9);
        for (;;) {
            const uint8_t sz = c.U8();
            if (c.bad || sz == 0) break;
            c.Skip(sz);
        }
    }
}

// avnt.go — 종류 바이트마다 짐 크기가 다르다
void SkipAVNT(Cur& c) {
    c.SkipBytes8();
    for (;;) {
        const int8_t v = static_cast<int8_t>(c.U8());
        if (c.bad) return;
        switch (v) {
            case 0: return;
            case 1: case 2: case 3: case 4: case 5: c.Skip(1); break;
            case 6: case 9: case 10: c.Skip(2); break;
            case 7:
                for (;;) {
                    const uint8_t k = c.U8();
                    if (c.bad || k == 0) break;
                    c.Skip(k);
                }
                break;
            case 8: c.Skip(8); break;
            default: c.Fail("unknown AVNT type " + std::to_string(v)); return;
        }
    }
}

// spell.go SkipSpellsSect
//
// ★ 이제 **건너뛰지 않고 읽는다** — 이름과 이름 뒤 세 바이트를 담는다.
//   **첫 바이트가 마나 비용이다** — 정품 콘솔 `list spells` 115줄과 전수 일치
//   (`_evidence/CONSOLE_list_spells.txt`, 2026-09-10). 나머지 둘은 뜻을 모른다.
void SkipSPEL(Cur& c, std::vector<SpellRaw>* outSpells) {
    const uint32_t n = c.U32();
    for (uint32_t i = 0; i < n && !c.bad; ++i) {
        SpellRaw sr;
        sr.name    = c.Str8();
        sr.mana    = c.U8();
        sr.rest[0] = c.U8();
        sr.rest[1] = c.U8();
        if (outSpells && !c.bad) outSpells->push_back(sr);
        const uint8_t sz = c.U8();
        for (int j = 0; j < sz && !c.bad; ++j) {
            const uint8_t v = c.U8();
            if (v >= 9) { c.Fail("invalid spell code " + std::to_string(v)); return; }
        }
        for (int k = 0; k < 2; ++k) SkipImageRef(c);
        c.Skip(4);
        c.SkipBytes8();
        c.SkipBytes16();
        c.SkipBytes8();
        c.SkipBytes8();
        c.SkipBytes8();
    }
}

// ability.go
void SkipABIL(Cur& c) {
    const uint32_t n = c.U32();
    for (uint32_t i = 0; i < n && !c.bad; ++i) {
        c.SkipBytes8();
        c.Skip(1);
        for (int j = 0; j < 3; ++j) SkipImageRef(c);
        c.SkipBytes8();
        c.SkipBytes16();
        c.SkipBytes8();
        c.SkipBytes8();
        c.SkipBytes8();
    }
}

// image.go skipIMAG
void SkipIMAG(Cur& c) {
    const uint32_t n = c.U32();
    for (uint32_t i = 0; i < n && !c.bad; ++i) {
        c.SkipBytes8();
        const int8_t typ = static_cast<int8_t>(c.U8());
        if (typ == 1) {
            SkipImageRef(c);
        } else if (typ == 2) {
            const uint8_t frames = c.U8();
            c.Skip(1);
            c.SkipBytes8();
            for (int j = 0; j < frames && !c.bad; ++j) SkipImageRef(c);
        } else {
            c.Fail("invalid image type " + std::to_string(typ));
            return;
        }
    }
}

std::string Upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// '+' 로 이어진 클래스 값을 하나씩
void ParseClasses(const std::string& val, ThingInfo& t) {
    size_t start = 0;
    while (start <= val.size()) {
        size_t e = val.find('+', start);
        if (e == std::string::npos) e = val.size();
        const std::string c = Upper(Trim(val.substr(start, e - start)));
        if (c == "COMPLEX") t.complex = true;
        else if (c == "MONSTER") t.isMonster = true;
        else if (c == "PLAYER") t.isPlayer = true;
        else if (c == "MISSILE") t.isMissile = true;
        else if (c == "OBSTACLE") t.isObstacle = true;
        else if (c == "IMMOBILE") t.isImmobile = true;
        // 갱신에 실어도 되는 아이템 (P4-d — ThingDb.h isPickup 주석 참조).
        //   후보 셋은 정품 갱신 실측 집합(물약·무기·방어구·사과·화살통)의 CLASS 를
        //   TestPickupClass 로 찍어 확정한다.
        else if (c == "WEAPON" || c == "ARMOR" || c == "FOOD" || c == "PICKUP")
            t.isPickup = true;
        start = e + 1;
        if (e == val.size()) break;
    }
    if (!t.classRaw.empty()) t.classRaw += "+";   // CLASS 속성이 여러 줄일 수 있다
    t.classRaw += val;
}

// "CIRCLE r" 만 잡는다 — BOX·CENTER·NULL 은 반지름 0
void ParseExtent(const std::string& val, ThingInfo& t) {
    const std::string v = Trim(val);
    // ★ 2026-09-13 묶음 2-c — 상자도 담는다(`server/object_type.go:286` things.Box → Shape.Box W·H).
    if (Upper(v.substr(0, 3)) == "BOX") {
        t.shapeKind = 3;
        double w = 0, h = 0;
        std::sscanf(v.c_str() + 3, "%lf %lf", &w, &h);
        t.boxW = static_cast<float>(w);
        t.boxH = static_cast<float>(h);
        return;
    }
    if (Upper(v.substr(0, 6)) == "CENTER") { t.shapeKind = 1; return; }
    if (Upper(v.substr(0, 6)) != "CIRCLE") return;
    t.shapeKind = 2;
    t.radius = static_cast<float>(std::atof(v.c_str() + 6));
}

// thing.go fixThingAttrs — 한 속성에 '=' 가 여럿 든 깨진 줄을 "키 = 값" 짝으로 되살린다
std::vector<std::string> FixAttrs(const std::string& attr) {
    std::vector<std::string> sub;
    size_t start = 0;
    for (;;) {
        size_t e = attr.find('=', start);
        if (e == std::string::npos) { sub.push_back(Trim(attr.substr(start))); break; }
        sub.push_back(Trim(attr.substr(start, e - start)));
        start = e + 1;
    }
    std::vector<std::string> kvs;
    for (size_t i = 0; i < sub.size(); ++i) {
        if (i == 0 || i + 1 == sub.size()) { kvs.push_back(sub[i]); continue; }
        const size_t sp = sub[i].rfind(' ');
        if (sp == std::string::npos) { kvs.push_back(sub[i]); continue; }
        kvs.push_back(Trim(sub[i].substr(0, sp)));
        kvs.push_back(Trim(sub[i].substr(sp + 1)));
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < kvs.size(); i += 2) {
        if (i + 1 >= kvs.size()) out.push_back(kvs[i]);
        else out.push_back(kvs[i] + " = " + kvs[i + 1]);
    }
    return out;
}

// PlayerDraw 짐 — libs `things/thing_draw.go:335` readPlayerDraw 그대로.
//   STAT = 이름(len8) · FramesPerDir(u8) · Field8(u8) · 이름(len8)
//   SEQU = 이름(len8) · 8방향 × 직전 STAT 의 FramesPerDir 개 그림 참조
//   END  = 끝. 모르는 섹션이면 멈춘다(바깥에서 크기로 건너뛰므로 걸음은 안 밀린다).
void ReadPlayerDraw(Cur& c, size_t end, PlayerAnimFrames* anims, int* seen) {
    int lastFrames = 0;
    while (!c.bad && c.off < end) {
        const std::string sect = c.Sect();
        if (c.bad || sect == "END ") return;
        if (sect == "STAT") {
            const std::string name = c.Str8();
            const int frames = c.U8();
            const int hold = c.U8();
            c.SkipBytes8();
            if (c.bad) return;
            lastFrames = frames;
            for (int i = 0; i < kPlayerAnimTypeCount; ++i) {
                if (name != PlayerAnimTypeName(i)) continue;
                anims[i].frames = frames;
                anims[i].hold = hold;
                ++*seen;
                break;
            }
        } else if (sect == "SEQU") {
            c.SkipBytes8();
            for (int d = 0; d < 8 && !c.bad; ++d)
                for (int j = 0; j < lastFrames && !c.bad; ++j) SkipImageRef(c);
        } else {
            return;
        }
    }
}

// THNG 하나. Go ReadThingSect 에서 서버에 필요한 것만 남긴 판.
bool ReadThing(Cur& c, ThingInfo& t, PlayerAnimFrames* anims, int* animSeen) {
    t.name = c.Str8();
    if (c.bad) return false;
    for (;;) {
        const std::string attr = c.Str8();
        if (c.bad) return false;
        if (attr.empty()) return true;

        std::vector<std::string> attrs{attr};
        // '=' 가 두 개 넘게 든 깨진 줄 — Go 와 같은 되살리기를 거친다
        if (static_cast<int>(std::count(attr.begin(), attr.end(), '=')) > 1)
            attrs = FixAttrs(attr);

        for (const std::string& a : attrs) {
            const size_t eq = a.find('=');
            const std::string key = Upper(Trim(a.substr(0, eq)));
            const std::string val = (eq == std::string::npos) ? "" : Trim(a.substr(eq + 1));

            if (key == "DRAW") {
                // ★ 이름 + 8정렬 u64 크기 + 그만큼의 짐 — **크기로 통째 건너뛴다.**
                //   Go 는 이름별 구조 skip 을 두지만 default 가 skip(sectSz)다.
                //   크기가 짐과 안 맞으면 다음 이름들이 깨져 전수 대조에서 바로 걸린다.
                const std::string dname = c.Str8();
                const uint64_t sz = c.U64Align();
                // ★ 2026-09-13 묶음 3-a — PlayerDraw 만은 STAT(애니 칸 수)을 읽는다. 공격 시각이 애니 길이다.
                //   정답지도 PlayerDraw 가 나올 때마다 표를 덮어쓴다(`server/object_type.go:1065`) — 같게 둔다.
                //   다 읽든 말든 **끝 자리는 크기로 맞춘다** — 걸음이 밀리지 않게.
                if (dname == "PlayerDraw" && !c.bad) {
                    const size_t start = c.off;
                    Cur sub = c;
                    sub.err = nullptr;   // 읽다 틀려도 본 커서의 오류로 번지지 않게
                    sub.n = std::min(c.n, start + static_cast<size_t>(sz));
                    ReadPlayerDraw(sub, sub.n, anims, animSeen);
                }
                c.Skip(static_cast<size_t>(sz));
            } else if (key == "MENUICON" || key == "PRETTYIMAGE") {
                SkipImageRef(c);
            } else if (key == "CLASS") {
                ParseClasses(val, t);
            } else if (key == "EXTENT") {
                ParseExtent(val, t);
            } else if (key == "FLAGS") {
                // 오브젝트 깃발 원문 — '+' 로 이어짐(`NO_COLLIDE+ALLOW_OVERLAP` 꼴). 충돌 거르기(sub_548360)에 쓴다.
                if (!t.flagsRaw.empty()) t.flagsRaw += "+";
                t.flagsRaw += val;
            } else if (key == "COLLIDE") {
                // 충돌 함수가 있나 — 정답지는 **두 오브젝트 다 있어야** 부딪힌다(`GAME5.c:1930` `!v4 || !v5 → 0`).
                t.hasCollide = true;
                t.collideRaw = val;
            } else if (key == "MASS") {
                // 질량 — 벽에 밀릴 때 힘을 이것으로 나눈다(`server/object.go:1183` Sub548600 = dp / Mass).
                //   없으면 정답지 기본 1(`server/object_type.go:197`).
                t.mass = static_cast<float>(std::atof(val.c_str()));
            } else if (key == "WEIGHT") {
                // ★★ **클라는 「집을 수 있나」를 `WEIGHT` 로 정한다** — CLASS 가 아니다.
                //   `client/object_type.go:355-362` 이 WEIGHT 를 읽는 바로 그 자리에서
                //   `typ.ObjClass |= object.ClassPickup` 을 한다.
                //   즉 **WEIGHT 가 없는 물건은 클라가 영영 못 집는다.**
                //   우리 `isPickup` 은 CLASS(WEAPON·ARMOR·FOOD·PICKUP)로 거르므로
                //   기준이 다르다 — 어긋나는지 재려고 값을 따로 담는다.
                t.weight = std::atoi(val.c_str());
                t.hasWeight = true;
            } else if (key == "SUBCLASS") {
                // 물약이 체력인지 마나인지가 여기 있다 — `POTION+HEALTH_POTION`.
                if (!t.subclassRaw.empty()) t.subclassRaw += "+";
                t.subclassRaw += val;
            } else if (key == "USE") {
                // ★ **쓰면 무슨 일이 나는가가 파일에 있다** — `USE = PotionUse 50`.
                //   앞이 다루는 이름, 뒤가 숫자다. 숫자가 없는 갈래도 있다(`ReadUse`).
                //   ⚠ 숫자의 뜻은 갈래마다 다르다. 회복량으로 읽어도 되는 것은
                //     `PotionUse`·`ConsumeUse` 뿐이다.
                const size_t sp = val.find(' ');
                if (sp == std::string::npos) {
                    t.useHandler = val;
                } else {
                    t.useHandler = Trim(val.substr(0, sp));
                    t.useArg = std::atoi(Trim(val.substr(sp + 1)).c_str());
                }
            }
            // 그 밖의 키(HEALTH·SPEED·FLAGS…)는 값이 문자열 안에 들어 있어 걸음에 영향 없다
            if (c.bad) return false;
        }
    }
}

}  // namespace

namespace {
uint32_t ParseNameBits(const std::string& raw, const char* const* names, int count) {
    uint32_t bits = 0;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t e = raw.find('+', start);
        if (e == std::string::npos) e = raw.size();
        const std::string tok = raw.substr(start, e - start);
        for (int i = 0; i < count; ++i) {
            if (tok == names[i]) { bits |= (1u << i); break; }
        }
        start = e + 1;
    }
    return bits;
}
const char* const kFlagNames[32] = {
    "BELOW", "NO_UPDATE", "ACTIVE", "ALLOW_OVERLAP", "SHORT", "DESTROYED", "NO_COLLIDE", "MISSILE_HIT",
    "EQUIPPED", "PARTITIONED", "NO_COLLIDE_OWNER", "OWNER_VISIBLE", "EDIT_VISIBLE", "NO_PUSH_CHARACTERS", "AIRBORNE", "DEAD",
    "SHADOW", "FALLING", "IN_HOLE", "RESPAWN", "ON_OBJECT", "SIGHT_DESTROY", "TRANSIENT", "BOUNCY",
    "ENABLED", "PENDING", "TRANSLUCENT", "STILL", "NO_AUTO_DROP", "FLICKER", "SELECTED", "MARKED",
};
const char* const kClassNames[32] = {
    "MISSILE", "MONSTER", "PLAYER", "OBSTACLE", "FOOD", "EXIT", "KEY", "DOOR",
    "INFO_BOOK", "TRIGGER", "TRANSPORTER", "HOLE", "WAND", "FIRE", "ELEVATOR", "ELEVATOR_SHAFT",
    "DANGEROUS", "MONSTERGENERATOR", "READABLE", "LIGHT", "SIMPLE", "COMPLEX", "IMMOBILE", "VISIBLE_ENABLE",
    "WEAPON", "ARMOR", "NOT_STACKABLE", "TREASURE", "FLAG", "CLIENT_PERSIST", "CLIENT_PREDICT", "PICKUP",
};
}  // namespace

const char* PlayerAnimTypeName(int i) {
    // `server/object_type.go:1093~1149` playerAnimTypes 순서 · 이름은 libs `player/anims.go`
    static const char* const kNames[kPlayerAnimTypeCount] = {
        "IDLE", "DIE", "DEAD", "JUMP", "WALK", "WALK_AND_DRAG", "RUN", "RUNNING_JUMP", "PICKUP",
        "DODGE_LEFT", "DODGE_RIGHT", "ELECTROCUTED", "FALL", "TRIP", "GET_UP", "LAUGH", "POINT",
        "SIT", "SLEEP", "TALK", "TAUNT", "CAST_SPELL", "CONCENTRATE", "PUNCH_LEFT", "PUNCH_RIGHT",
        "PUNCH_RIGHT_HOOK", "MACE_STRIKE", "SWORD_STRIKE", "LONG_SWORD_STRIKE", "STAFF_STRIKE",
        "STAFF_BLOCK", "STAFF_SPELL_BLAST", "STAFF_THRUST", "SHOOT_BOW", "SHOOT_CROSSBOW",
        "AXE_STRIKE", "GREAT_SWORD_PARRY", "GREAT_SWORD_STRIKE", "GREAT_SWORD_IDLE", "HAMMER_STRIKE",
        "RAISE_SHIELD", "RECOIL_FORWARD", "RECOIL_BACKWARD", "RECOIL_SHIELD", "CHAKRAM_STRIKE",
        "BERSERKER_CHARGE", "WARCRY", "GREAT_SWORD_BLOCK_LEFT", "GREAT_SWORD_BLOCK_RIGHT",
        "GREAT_SWORD_BLOCK_DOWN", "ELECTRIC_ZAP", "DUST", "RECOIL", "SNEAK", "HARPOONTHROW",
    };
    return (i >= 0 && i < kPlayerAnimTypeCount) ? kNames[i] : nullptr;
}

uint32_t ParseObjectFlags(const std::string& raw) { return ParseNameBits(raw, kFlagNames, 32); }
uint32_t ParseObjectClass(const std::string& raw) { return ParseNameBits(raw, kClassNames, 32); }

bool ThingDb::Load(const std::string& path) {
    things_.clear();
    walls_.clear();
    error_.clear();
    for (auto& a : playerAnims_) a = PlayerAnimFrames{};
    playerAnimSeen_ = 0;

    const std::vector<uint8_t> buf = DecryptFile(path, kKeyThingBin);
    if (buf.empty()) { error_ = "cannot read/decrypt " + path; return false; }

    Cur c;
    c.p = buf.data();
    c.n = buf.size();
    c.err = &error_;

    for (;;) {
        if (c.Eof()) break;
        const std::string sect = c.Sect();
        if (c.bad) break;
        if (sect == "FLOR") SkipFLOR(c);
        else if (sect == "EDGE") SkipEDGE(c);
        else if (sect == "WALL") SkipWALL(c, &walls_);
        else if (sect == "AUD ") SkipAUD(c);
        else if (sect == "AVNT") SkipAVNT(c);
        else if (sect == "SPEL") SkipSPEL(c, &spells_);
        else if (sect == "ABIL") SkipABIL(c);
        else if (sect == "IMAG") SkipIMAG(c);
        else if (sect == "THNG") {
            ThingInfo t;
            if (!ReadThing(c, t, playerAnims_, &playerAnimSeen_)) break;
            things_.push_back(std::move(t));
        } else if (sect == std::string("\0\0\0\0", 4)) {
            break;   // 끝의 0 채움
        } else {
            c.Fail("unsupported section '" + sect + "'");
            break;
        }
        if (c.bad) break;
    }

    if (c.bad) { things_.clear(); return false; }
    if (things_.empty()) { error_ = "no THNG sections"; return false; }
    return true;
}

}  // namespace nox
