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
struct SpellRaw {
    std::string name;      // 속이름 — `SPELL_FIREBALL` 꼴
    uint8_t mana = 0;      // ⭕ 마나 비용
    uint8_t rest[2] = { 0, 0 };   // 뜻 미상. 값이 흩어져 있어 짐작하지 않는다
};

struct ThingInfo {
    std::string name;
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
    // CLASS 원문 — 필터 설계·진단용. '+' 로 이어진 값 그대로.
    std::string classRaw;
};

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
    std::string error_;
};

}  // namespace nox
