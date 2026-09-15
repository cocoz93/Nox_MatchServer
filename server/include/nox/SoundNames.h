// SoundNames.h — 소리 번호 ↔ 이름. 묶음 10-a 소리 와이어가 쓴다.
//
// 왜 필요한가 — thing.bin 의 `AUD ` 칸은 소리를 **이름**으로 적고,
// 와이어는 **번호**를 실는다. 그 사이를 이 표가 쟇는다.
//
// ⭕ 이름이 **정확히 1,023개**다(0 번은 `NULL`) — 와이어가 아래 **10비트**를
// 소리 번호로 쓰는 까닭이 이것이다(`번호 | 세기 << 10`).
//
// ⭕ 정답지 `common/sound/sound.go` 의 `soundNames` 를 **기계로 뽑은 것**이다.
// 손으로 고치지 말 것.
#pragma once

#include <cstdint>

namespace nox {

// 색인 = 소리 번호. 0 은 "NULL" 이다.
extern const char* const kSoundNames[];
constexpr int kSoundCount = 1023;

// 범위를 벗어나면 "" 를 돌려준다.
const char* SoundName(int id);
// 이름 → 번호. 모르면 **0**(NULL).
uint16_t SoundByName(const char* name);

}  // namespace nox
