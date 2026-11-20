// SpellNames.h — 주문 번호 ↔ 이름. `0x79 TRY_SPELL` 을 읽을 때 쓴다.
//
// 왜 필요한가 — 시전 메시지는 본문 첫 바이트에 **주문 번호만** 싣는다.
// 번호만 보면 무엇을 쐈는지 로그에 남길 수 없다.
//
// 이 번호는 opennox `spell` 패키지의 ID 와 그대로 같다. 실측으로 다섯을 확인했다 —
// 4 점멸 · 27 파이어볼 · 41 약한치유 · 43 연쇄번개 · 51 보호막.
//
// 표 자체는 SpellNames.cpp 에 있고 **기계로 뽑는다** (tools/dumpscan -genspells).
// 손으로 고치지 말 것.
#pragma once

namespace nox {

// 색인 = 주문 번호. 0 과 빈 자리는 "" 다.
extern const char* const kSpellNames[];
extern const int kSpellCount;

// 범위를 벗어나면 "" 를 돌려준다.
const char* SpellName(int id);

// ★ 11-20 트레일러 위저드 — 이름 → 주문 번호(이 표의 색인). 없으면 0.
//   정답지 `nox_xxx_spellNameToN_4243F0`(지팡이 주문 읽기 `GAME4_3.c:3065`)와 같은 일.
//   ✖ thing.bin `SPEL` 칸의 순번은 이 번호가 아니다 — 실측 `SPELL_CHAIN_LIGHTNING` 이 SPEL 순번 32(= 우리 표 `SPELL_FREEZE`)였다.
//   표(SpellNames.cpp)는 기계로 뽑으므로 함수는 이 머리 파일에 둔다.
inline int SpellIdByName(const char* name) {
    if (!name || !*name) return 0;
    for (int i = 1; i < kSpellCount; ++i) {
        const char* a = kSpellNames[i];
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return i;
    }
    return 0;
}

}  // namespace nox
