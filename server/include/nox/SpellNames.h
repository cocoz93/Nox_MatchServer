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

}  // namespace nox
