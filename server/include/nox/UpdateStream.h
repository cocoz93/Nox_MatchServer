// UpdateStream.h — 상태 갱신(`0xa4 MSG_UPDATE_STREAM`)을 뜯는다.
//
// **게임 중 트래픽의 알맹이다.** 매 틱 `0x27`(시각)과 짝지어 나간다.
//
// 근거는 추측이 아니라 클라의 실제 파서다. 기계번역이라 변수 이름은 `v5`·`v14` 뿐이지만
// **몇 바이트를 읽고 어디서 갈리는지는 그대로 읽힌다.**
//
//   src/legacy/GAME2_3.c
//     nox_xxx_netCliProcUpdateStream_494A60   ← 머리
//     nox_xxx_netCliUpdateStream2_494C30      ← 항목 하나 (되풀이)
//
// 짜임
//
//   머리   [별칭 1B | ff + 번호 2B + 종류 2B]
//          x 2B · y 2B · 깃발 1B · (깃발&0x80 이면 프레임 1B) · 애니 1B
//   항목   00 00 00                       → 끝
//          [00] 이 앞에 붙으면 절대 좌표 모드
//          [별칭 1B | ff + 번호 2B + 종류 2B]
//          절대면 x 2B·y 2B / 상대면 dx 1B·dy 1B (부호 있음)
//          **복합 오브젝트면** 방향·애니 1B, 최상위 비트면 프레임 1B 더,
//          스프라이트의 또 다른 속성이면 1B 더 (그 조건만 아직 미상)
//
// ★ 상대 좌표는 **머리가 아니라 직전 항목** 기준으로 쌓인다 (C 의 `*a3 += v15`).
// ★ 좌표가 0~6000 을 벗어나면 클라도 거기서 멈춘다. 우리도 같은 자리에서 멈춘다.
// ★ 복합이 아니면 **한 바이트도 안 붙는다** — 이것이 자유도를 없애 100% 판독을 만들었다.
//
// ⚠ `opennox/libs` 의 Go 디코더는 이 자리와 어긋난다 — `decodeUpdateStream = false` 로
//   꺼져 있어 검증된 적이 없다. 믿을 것은 C 쪽이다.
#pragma once

#include <cstdint>
#include <vector>

#include "nox/Alias.h"
#include "nox/ThingFlags.h"

namespace nox {

// 좌표 유효 범위 — 클라가 이 밖이면 파싱을 접는다 (GAME2_3.c 의 `> 6000` 검사).
constexpr int kMaxCoord = 6000;

struct ObjEntry {
    uint8_t alias = 0;       // 0xff 면 전체 번호로 온 것
    uint16_t netCode = 0;
    uint16_t type = 0;
    bool absolute = false;   // 좌표가 절대값으로 왔나
    int x = 0, y = 0;
    int extra = 0;           // 좌표 뒤에 더 붙은 바이트 수
    uint8_t extraBytes[3] = {};   // 그 바이트들 (뜻은 아직 다 모른다)
    int8_t dx = 0, dy = 0;        // 상대 모드일 때 실린 값 그대로
};

struct UpdateMsg {
    uint8_t headAlias = 0;
    uint16_t headNetCode = 0;
    uint16_t headType = 0;
    int x = 0, y = 0;
    uint8_t flags = 0;
    uint8_t headFrame = 0;   // flags&0x80 일 때만 실린다
    uint8_t headAnim = 0;
    std::vector<ObjEntry> objects;
    size_t consumed = 0;     // 이 메시지가 먹은 바이트 (끝 표식 포함)
    bool ok = false;         // 끝 표식까지 깨끗하게 닿았나
};

// 상태 갱신 하나를 뜯는다. `data` 는 오프코드(0xa4) **다음**부터다.
// `aliases` 는 별칭으로 온 오브젝트의 종류를 찾는 데 쓴다(없으면 nullptr).
UpdateMsg DecodeUpdate(const uint8_t* data, size_t n, const AliasTable* aliases);

// 뜯은 것을 **다시 바이트로** 짠다. 오프코드는 안 붙인다(본문만).
// 돌려주는 값 = 쓴 바이트 수, 0 이면 실패.
//
// ★ 왜 필요한가 — 뜯기만 해서는 「읽을 줄 안다」까지다. 서버는 **만들어 보내야** 한다.
//   그리고 원본을 뜯어 다시 짠 것이 **바이트까지 같으면** 짜임을 다 안다는 증명이 된다.
size_t EncodeUpdate(const UpdateMsg& m, uint8_t* out, size_t cap);

}  // namespace nox
