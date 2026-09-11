// ThingFlags.h — 종류 번호가 「복합 오브젝트」인지 답한다.
//
// 상태 갱신에서 좌표 뒤에 바이트가 더 붙느냐가 이 한 비트로 갈린다.
// 복합이 아니면 **한 바이트도 안 붙는다.**
//
// 표는 ThingFlags.cpp 에 있고 **기계로 뽑는다** (tools/dumpscan -genthings).
#pragma once

#include <cstdint>

namespace nox {

extern const uint16_t kThingCount;
extern const uint8_t kThingComplexBits[];

// 와이어 종류 번호가 복합 오브젝트인가. 모르는 번호는 false.
bool ThingIsComplex(uint16_t wireType);

// 와이어 종류 번호가 **사람**인가.
//
// ★ 왜 여기 있나 — 상태 갱신에서 **사람은 덧붙이가 한 바이트 더 붙는다.**
//   클라(GAME2_3.c:1685)는 ObjClass & 4 면 자세를 **별도 바이트**로 읽고,
//   아니면 방향 바이트의 하위 4비트에서 읽는다. 사람이 바로 그 Class & 4 다
//   (draw_debug.go:87 — 그 가지에서만 사람 자세표를 쓴다).
//   그래서 이 물음이 **엔트리 길이를 가른다.**
bool ThingIsPlayer(uint16_t wireType);

// 표에 있는 번호인가. 모르는 번호는 덧붙이 크기를 넘겨짚으면 안 된다.
bool ThingKnown(uint16_t wireType);

}  // namespace nox
