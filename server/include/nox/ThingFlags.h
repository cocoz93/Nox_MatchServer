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

// 표에 있는 번호인가. 모르는 번호는 덧붙이 크기를 넘겨짚으면 안 된다.
bool ThingKnown(uint16_t wireType);

}  // namespace nox
