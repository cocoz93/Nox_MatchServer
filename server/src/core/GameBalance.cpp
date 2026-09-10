// GameBalance.cpp — 표를 찾는 자리. 값 자체는 헤더에 출처와 함께 있다.
#include "nox/GameBalance.h"

namespace nox {

const MonsterStats* FindMonster(uint16_t wireType) {
    for (const MonsterStats& m : kMonsters)
        if (m.wireType == wireType) return &m;
    return nullptr;
}

}  // namespace nox
