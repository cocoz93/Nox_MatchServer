// WolGameInfo.h — 웨스트우드 온라인 로비의 방 정보 덩어리 (MO 2단계 로비, Docs/LOBBY_DESIGN.md).
//
// 로비는 방 목록 줄(`326`)과 방 참가 답(`332`)에 69바이트 방 정보를 싣고,
// 정품 클라는 그걸 풀어 목록 화면에 방 이름·맵·인원을 띄운다.
//   · 칸 배치 = opennox `xwis@v0.1.1` gameinfo.go:335-409 (MarshalBinary)
//   · 포장   = 같은 모듈 crypt.go — 로비 규약은 줄바꿈으로 메시지를 가르므로 원본 바이트
//              0x0A·0x0D·0x00 이 줄 안에 나오면 안 된다. 그래서 69바이트를 7비트씩 잘라
//              바이트마다 최상위 비트를 1로 세워 싣는다(78바이트). 보안용 암호화가 아니다.
//   · 정답지 = 1단계 녹음(XWIS 원래 줄)의 실제 덩어리 6개 — 풀었다 다시 포장하면 바이트까지
//              같다(`lobby_gameinfo` 시험)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nox::wol {

// 맵 종류 — 깃발 칸(바이트 63-64)의 0x1ff0 자리 (gameinfo.go:198-207)
enum : uint16_t {
    kMapKotr        = 0x0010,
    kMapCtf         = 0x0020,
    kMapFlagBall    = 0x0040,
    kMapChat        = 0x0080,
    kMapArena       = 0x0100,
    kMapElimination = 0x0400,
    kMapCoop        = 0x0A00,
    kMapQuest       = 0x1000,
    kMapTypeMask    = 0x1ff0,
};

// 방 정보 한 벌. 기본값은 녹음 다섯 방에서 공통으로 본 값이다.
struct GameInfo {
    uint8_t  access     = 0;       // 0 열림 · 1 닫힘 · 2 비공개 (바이트 0 상위 4비트)
    uint8_t  disallow   = 0;       // 막은 직업 1 전사 · 2 마법사 · 4 소환사 (바이트 0 하위 4비트)
    uint8_t  unk1       = 0xff;    // 뜻 모름 — 녹음 다섯 방 모두 0xff
    uint8_t  resolution = 0;       // 0 640x480 · 1 800x600 · 2 1024x768 · 3 1280x1024
    bool     limitRes   = false;   // 바이트 2 의 0x80
    uint8_t  players    = 0;
    uint8_t  maxPlayers = 0;
    uint16_t minPing    = 0xffff;  // 0xffff = 제한 없음
    uint16_t maxPing    = 0xffff;
    uint16_t unk2       = 0x489e;  // 뜻 모름 — 녹음 다섯 방 모두 0x489e
    std::string map;               // 앞 9바이트만 실린다. 풀 때는 첫 0 앞까지 — 0 뒤 찌꺼기는 버린다
    std::string name;              // 앞 15바이트만 실린다. 〃 (녹음 quiteplaz 방의 맵 칸이 "gather\0h\0")
    std::array<uint8_t, 28> unk3;  // 뜻 모름 — 녹음 넷은 전부 0xff, 하나(tak7)는 군데군데 0 비트
    uint16_t flags      = 0x2007 | kMapArena;  // 맵 종류 | 나머지 깃발 — 녹음 다섯 방 모두 0x2107
    uint16_t fragLimit  = 0;
    uint16_t timeLimit  = 0;       // ⚠ 상위 6비트는 포장에서 잘린다(Pack 참고)

    GameInfo() { unk3.fill(0xff); }
};

constexpr size_t kRawSize    = 69;  // 칸을 다 채운 날것
constexpr size_t kPackedSize = 78;  // 7비트 포장 — 552비트 중 앞 546비트만 실린다(opennox 와 같음)

// 포장 앞에 붙는 머리. `326` 줄에선 그 앞에 다시 "128:" 이 붙는다(녹음 그대로).
constexpr std::string_view kHeader{":G1P3\x9a\x03\x01", 8};
constexpr std::string_view kListPrefix{"128:", 4};

using Raw = std::array<uint8_t, kRawSize>;

// 칸 ↔ 날것 69바이트
Raw Marshal(const GameInfo& g);
GameInfo Unmarshal(const Raw& raw);

// 날것 69바이트 ↔ 머리 + 포장 86바이트 (칸은 안 본다)
std::string PackRaw(const Raw& raw);
// PackRaw 의 역. 머리가 다르거나 길이가 86이 아니거나 최상위 비트가 빠진 조각이 있으면 false.
bool UnpackRaw(std::string_view payload, Raw& raw);

// 위 둘을 이은 것. `332` 답엔 Pack 결과 그대로, `326` 줄엔 kListPrefix 를 앞에 붙여 싣는다.
std::string Pack(const GameInfo& g);
bool Unpack(std::string_view payload, GameInfo& out);

// 모드 이름(`arena` · `chat` …) → 깃발 칸의 맵 종류(`kMap*`). 모르는 이름이면 false.
//   ★ 10-14 플랜 B 작업 8d — 옛 `lobby_main.cpp` 의 `ParseMode` 를 옮겼다(채널이 방 설정의 모드로 방 정보를 그린다)
bool ModeFlagsOf(std::string_view mode, uint16_t& bits);

}  // namespace nox::wol
