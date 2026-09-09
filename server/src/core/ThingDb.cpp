// ThingDb.cpp — thing.bin 파서. 짜임 근거는 전부 opennox/libs things (2026-09-10 대조).
//
// 걸음이 한 바이트라도 밀리면 그 뒤 이름들이 전부 깨진다 — 그래서 검증이
// 「이름 2,874종 전수 대조」다. 밀림은 조용히 못 지나간다.
#include "nox/ThingDb.h"

#include <cctype>
#include <cstring>

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

// wall.go
void SkipWALL(Cur& c) {
    c.Skip(4);
    c.SkipBytes8();
    c.Skip(3 * 4 + 2 * 1);
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
void SkipSPEL(Cur& c) {
    const uint32_t n = c.U32();
    for (uint32_t i = 0; i < n && !c.bad; ++i) {
        c.SkipBytes8();
        c.Skip(1 + 2);
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
    if (Upper(v.substr(0, 6)) != "CIRCLE") return;
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

// THNG 하나. Go ReadThingSect 에서 서버에 필요한 것만 남긴 판.
bool ReadThing(Cur& c, ThingInfo& t) {
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
                c.Str8();
                const uint64_t sz = c.U64Align();
                c.Skip(static_cast<size_t>(sz));
            } else if (key == "MENUICON" || key == "PRETTYIMAGE") {
                SkipImageRef(c);
            } else if (key == "CLASS") {
                ParseClasses(val, t);
            } else if (key == "EXTENT") {
                ParseExtent(val, t);
            }
            // 그 밖의 키(HEALTH·SPEED·FLAGS…)는 값이 문자열 안에 들어 있어 걸음에 영향 없다
            if (c.bad) return false;
        }
    }
}

}  // namespace

bool ThingDb::Load(const std::string& path) {
    things_.clear();
    error_.clear();

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
        else if (sect == "WALL") SkipWALL(c);
        else if (sect == "AUD ") SkipAUD(c);
        else if (sect == "AVNT") SkipAVNT(c);
        else if (sect == "SPEL") SkipSPEL(c);
        else if (sect == "ABIL") SkipABIL(c);
        else if (sect == "IMAG") SkipIMAG(c);
        else if (sect == "THNG") {
            ThingInfo t;
            if (!ReadThing(c, t)) break;
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
