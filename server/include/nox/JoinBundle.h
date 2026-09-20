// JoinBundle.h — 접속 묶음(216B)과 USE_MAP(41B)을 **필드에서 조립**한다. 독립 서버 P5.
//
// 지금까지는 녹음본에서 뗀 바이트에 네 자리(프레임 둘·번호 둘)와 플레이어 정보만
// 갈아 끼웠다. 이 조립이 서면 **녹음 파일 없이** 접속을 받는다.
//
// 여섯 메시지 (2026-09-10 복호본 실측 · PROTOCOL.md 「접속 직후」 절):
//
//   a9 INFORM(관전 시작)   6B   a9 0c 01 00 00 00          — 불변
//   28 FULL_TIMESTAMP      5B   28 <프레임 u32>
//   2c JOIN_DATA           7B   2c <번호 u16> <0 u32>
//   af GAME_SETTINGS      20B   af <프레임 u32> <버전 u32=0x0001039a> <설정 …>  — 프레임만 가변
//   b0 GAME_SETTINGS_2    49B   b0 "User Game" + 0×7 + ff×28 + 0×4              — 불변(방 이름)
//   2d NEW_PLAYER        129B   2d <번호 u16> <플레이어 정보 97B> <뒤 29B>
//                               정보 97B 는 **클라 CLIENT_ACCEPT 에서 그대로 되돌린다**
//                               (지어내지 않는다 — 두 접속 대조로 확인된 규칙)
//
// USE_MAP:  2b <맵이름 32B 고정(0채움)> <맵 CRC u32> <프레임 u32>
//           CRC 는 .map 머리의 값 그대로 — MapFile::Crc() 와 짝.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nox/GameBalance.h"   // CharClass

namespace nox {

// ── 묶음 8-a 게임 설정 ────────────────────────────────────────────
//
// 정답지 `sendSettings`(`src/network.go:444~500`).
//   · `af <프레임 u32> <클라 버전 u32> <게임 깃발 & 0x7FFF0 u32> <서브 깃발 u32>
//        <정원 u8> <a u8> <b u8>` — **20바이트**
//   · `b0 <방 이름 16B> <값 u32 ×5> <바이트 ×4> <u32> <남은 시간 u32>` — **49바이트**
//
// ★ 전에는 두 메시지가 **녹음본에서 뗀 고정 배열**이었다. 그 바이트를 정답지 칸에
//   하나씩 맞춰 보니 **전부 들어맞았다** — 아래 기본값이 그 실측이다.
//   ⭕ 게임 깃발 `0x00032100` = 아레나(0x100) + 온라인(0x2000) + 퀘스트아님(0x10000) + 0x20000.
//     `noxflags` 이름표와 한 칸씩 맞다(`common/flags/gameflags.go`)
//   ⭕ 정원 32 · 서브 깃발 2 · `a` 15 · `b` 0 · 방 이름 "User Game"
//   ⭕ `b0` 의 값 다섯과 바이트 넷과 u32 는 전부 **0xFF...**(안 정해짐) · 남은 시간 0
//
// ⛔ `UPDATE_GUI_GAME_SETTINGS`(0xB1)는 **안 옮긴다** — 몸이 `Settings2` **58바이트 통째**인데
//   (`server/settings.go:9`) 그중 앞 24바이트와 꼬리 4바이트를 **우리가 잰 적이 없다**.
//   접속 때가 아니라 **설정이 바뀔 때** 나가는 것이라 녹음에도 없다. 지어내지 않는다
struct GameSettings {
    uint32_t clientVersion = 0x0001039au;   // 클라 setVersion 이 읽는 값
    uint32_t gameFlags     = 0x00032100u;   // `GetGame() & 0x7FFF0`
    uint32_t subFlags      = 2;             // `getServerSubFlags_409E60`
    uint8_t  maxPlayers    = 32;            // `getServerMaxPlayers`
    uint8_t  limitValue    = 15;            // `servGamedataGet(Field52)`
    uint8_t  limitKind     = 0;             // `Sub_40A180(Field52)`
    std::string name       = "User Game";   // `getServerName` — 16바이트 칸(넘치면 자른다)
    uint32_t vals[5]       = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };  // Field24.Vals
    uint8_t  bytes4[4]     = { 0xFF, 0xFF, 0xFF, 0xFF };   // Field44
    uint32_t field48       = 0xFFFFFFFFu;
    uint32_t timeLeft      = 0;             // 시간 제한이 걸렸을 때만 `Sub_40A230()`
    // ⚠ 묶음 8-g — 판 시간 한도(프레임). **0 이면 한도 없음**.
    //   우리 캡처는 한도 없이 돌았고 남은 시간이 음수로 내려갔다 — 값은 **실측이 아니라 설정**이다
    uint32_t timeLimitFrames = 0;
};

// `af` 20바이트 · `b0` 49바이트를 각각 짓는다
std::vector<uint8_t> BuildGameSettings(const GameSettings& g, uint32_t frame);
std::vector<uint8_t> BuildGameSettings2(const GameSettings& g);

constexpr size_t kJoinBundleSize = 216;
constexpr size_t kUseMapSize = 41;
constexpr size_t kNewPlayerSize = 129;   // 2d + 번호2 + 정보97 + 꼬리29


// 접속 묶음 216B. info97 이 null 이면 그 97자리는 0 — **실전에서는 꼭 채울 것**
// (이름·직업·외모가 전부 여기서 나온다).
std::vector<uint8_t> BuildJoinBundle(uint16_t netCode, uint32_t frame,
                                     const uint8_t* info97,
                                     const GameSettings& g = GameSettings{});

// USE_MAP 41B. mapName 은 "estate.map" 꼴(32자 미만).
std::vector<uint8_t> BuildUseMap(const std::string& mapName, uint32_t mapCrc,
                                 uint32_t frame);

// 2d NEW_PLAYER 하나만 — **남을 소개할 때**. 독립 모드에는 녹음이 없어 이것으로 만든다.
std::vector<uint8_t> BuildNewPlayer(uint16_t netCode, const uint8_t* info97);

// 플레이어 정보 97B 의 **색 자리(+68~+87)가 전부 0 이면** 정품 녹음 실측값으로 메운다.
// 무인 접속은 색 선택 화면을 건너뛰어 클라가 0 만 보내고, 그대로 실으면 캐릭터가 검게 그려진다.
// 까닭과 값의 근거는 구현부 주석에 있다(`JoinBundle.cpp`).
void FillDefaultColorsIfBlank(uint8_t* info97);

// 위 메우기를 켤지(기본 **끔**).
// 「검은 실루엣」의 뿌리는 색이 아니라 **두 번째 `2d NEW_PLAYER`** 였다(`--2nd-newplayer` 참조).
//
// ✖ **정정 09-20 — 「메우면 정답지와 달라진다」는 틀렸다.**
//   그때 대조군도 **같은 무인 클라**로 붙였으니 양쪽 다 색을 0 으로 보냈다. 「정답지 캐릭터엔
//   피부가 없다」가 아니라 **내 클라가 색을 안 보낸 것**을 양쪽에서 똑같이 본 것뿐이다
//   (대조군이 내 계측의 맹점까지 같이 복제한 꼴 — 관측 수단이 같으면 대조가 안 된다).
// ⭕ 색은 클라의 **저장된 캐릭터 프로필**에서 온다(opennox `gameloop.go:424~432`
//   `save.Player.SkinColor` …). 우리 자동 접속 클라는 캐릭터를 만든 적이 없어 프로필이 비었다.
//   ⇒ 사람이 캐릭터를 만들어 들어오면 색이 실린다. **서버가 고칠 것은 없다** — 기본 끔이 맞다.
// ⚠ 다만 **촬영·판정 주행에는 `--fill-colors` 를 켤 것.** 안 켜면 캐릭터가 검은 실루엣으로 찍혀
//   그림·차림을 보는 항목이 전부 안 보인다(09-20 에 사용자가 화면을 보고 짚었다).
extern bool fillColorsOn;

// 첫 스냅샷의 cc 꼬리부(381B) — 사람 하나를 세우는 고정 절차.
// ⚠ 장비 색·오벨리스크 대상 등 낱값은 estate 실측 그대로다(뜻 미상).
// ★ 2026-09-13 묶음 3-c — `ea FORGET_DRAWABLES` 와 `e9 PLAYER_RESPAWN` 의 4바이트는 **프레임**이다
//   (정답지 object.go:728 · GAME3_3.c:8065). 녹음 값 0x4b17 · 0x4b1f 는 그 판의 프레임이었고
//   e9 의 `1f 4b` 를 「체력 자리」로 읽은 것은 위저드 체력 75(0x4b) 와 우연히 겹친 오독이었다.
//   클라는 이 프레임보다 오래 안 본 정적 그림을 지운다(sub_45A670) — 받는 사람의 지금 프레임을 넣을 것.
// ⚠ `enchantBits` 는 **그 사람의 지금 버프 비트**다. 전에는 녹음본 값 `0x00800000`(비트 23 무적)을
//   박아 두어, 클라가 **접속 내내 사람을 무적으로 보고 파란 보호막을 돌렸다**(09-20 실측·고침).
// ⚠ `mapName` 도 같은 까닭이다 — `b1` 에 녹음본 `"estate"` 가 박혀 있어 **어느 맵에서든 estate 라고**
//   말하고 있었다(09-20 고침). 비우면 녹음본 값 그대로 간다(바이트 대조 시험용).
std::vector<uint8_t> BuildSnapshotTail(uint16_t netCode, CharClass cls, uint32_t eaFrame, uint32_t e9Frame,
                                       uint32_t enchantBits = 0, const std::string& mapName = std::string());

}  // namespace nox
