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

constexpr size_t kJoinBundleSize = 216;
constexpr size_t kUseMapSize = 41;
constexpr size_t kNewPlayerSize = 129;   // 2d + 번호2 + 정보97 + 꼬리29


// 접속 묶음 216B. info97 이 null 이면 그 97자리는 0 — **실전에서는 꼭 채울 것**
// (이름·직업·외모가 전부 여기서 나온다).
std::vector<uint8_t> BuildJoinBundle(uint16_t netCode, uint32_t frame,
                                     const uint8_t* info97);

// USE_MAP 41B. mapName 은 "estate.map" 꼴(32자 미만).
std::vector<uint8_t> BuildUseMap(const std::string& mapName, uint32_t mapCrc,
                                 uint32_t frame);

// 2d NEW_PLAYER 하나만 — **남을 소개할 때**. 독립 모드에는 녹음이 없어 이것으로 만든다.
std::vector<uint8_t> BuildNewPlayer(uint16_t netCode, const uint8_t* info97);

// 첫 스냅샷의 cc 꼬리부(381B) — 사람 하나를 세우는 고정 절차.
// ⚠ 장비 색·오벨리스크 대상 등 낱값은 estate 실측 그대로다(뜻 미상).
// ★ 2026-09-13 묶음 3-c — `ea FORGET_DRAWABLES` 와 `e9 PLAYER_RESPAWN` 의 4바이트는 **프레임**이다
//   (정답지 object.go:728 · GAME3_3.c:8065). 녹음 값 0x4b17 · 0x4b1f 는 그 판의 프레임이었고
//   e9 의 `1f 4b` 를 「체력 자리」로 읽은 것은 위저드 체력 75(0x4b) 와 우연히 겹친 오독이었다.
//   클라는 이 프레임보다 오래 안 본 정적 그림을 지운다(sub_45A670) — 받는 사람의 지금 프레임을 넣을 것.
std::vector<uint8_t> BuildSnapshotTail(uint16_t netCode, CharClass cls, uint32_t eaFrame, uint32_t e9Frame);

}  // namespace nox
