// Clock.h — 서버 시계(ms · 32비트로 돈다). 네트워크 스레드(타이머 · 이벤트에 넣은 시각)와 게임 루프(틱 · 넘김 지연)가 같이 쓴다.
//
// ★ 10-09 플랜 A 5d 자가검증 ② — 옛 `GetTickCount`·`GetTickCount64` 는 약 15.6ms 단위로만 오른다(윈도 기본 타이머 간격).
//   게임 루프가 틱 마감에 정확히 깨어도 그 시계가 아직 안 올라 있으면 틱이 안 돈다. 그래서 `steady_clock`
//   (MSVC 는 `QueryPerformanceCounter`)으로 센다. 두 스레드가 한 시계를 써야 넘김 지연(넣은 시각 → 처리한 시각)이 뜻을 가진다.
#pragma once

#include <chrono>
#include <cstdint>

namespace nox {

inline uint32_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace nox
