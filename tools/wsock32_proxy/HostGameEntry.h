// HostGameEntry.h — 정품 `Host Game` 을 「내 방 들어가기」로 돌릴 때 쓰는 것(10-16 플랜 B 작업 11).
//   ① 내 방의 방 정보(로비 방 목록 끝에 끼우는 `326` 줄과 목록 항목이 같은 값을 쓴다)
//   ② 정품 클라의 방 목록 항목(169바이트 — OpenNox `nox_gui_server_ent_t` `defs.h:1195`) 짓기
//   ③ 그 항목이 가리키는 WOL 채널(WOLAPI `Channel` — 32비트 268바이트. 형식 정보는 64비트 배치라 272) 짓기
//   DLL(wsock32_proxy.cpp)과 시험(hostgame_test.cpp)이 같이 쓴다 — 시험이 본 코드가 곧 DLL 코드다.
//
// 정답지 = 정품 클라 메모리에서 뜬 실물 항목 · 채널(10-16 파일럿 — seed 방, `_evidence/_rooms_check_20261016/memhost5_ref2.txt`).
//   그 판에 이 규칙으로 지은 것과 바이트 차이 0(포인터 칸 · 주소 글자 뒤 찌꺼기 뺌). 같은 판에 지은 항목으로 seed 없이 새 방에 들어갔다.
//   · 항목 +100~+168 = 방 정보 날것 69바이트 그대로(`WolGameInfo` Marshal) — 날것의 `unk2 0x489e` 는 게임 포트 18590 이다
//   · `326` 의 `41681088` = 채널 reserved = 항목 +44 (두 short 192 · 636 — 월드맵 자리로 보임)
//   · 방 정보 머리 `:G1P3` 뒤 세 바이트(9a 03 01) = 항목 +48 = 채널 exInfo · 채널 주제 칸 = 머리를 뗀 포장 78바이트
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "nox/WolGameInfo.h"

namespace hostgame {

constexpr size_t kEntrySize = 169;
constexpr size_t kChannelSize = 268;
constexpr size_t kChannelNameSize = 17;   // 채널 +52 — '#' 없이 · 끝 0 포함
constexpr uint16_t kGamePort = 18590;
constexpr uint32_t kLocation = 41681088;  // 우리 로비 `Lobby::SendList` 의 같은 칸(녹음 다섯 방 중 셋의 값)
constexpr uint32_t kNoPing = 9999;        // 항목 +8 · +96 — 실물이 9999(잰 핑 없음)
constexpr uint32_t kChannelType = 37;     // `326` 셋째 수 · `LIST 37 37`

// 내 방의 방 정보 — 서버 로비 틀(`main.cpp` 최대 31 · 1024x768)에 새 방 값(이름 「<닉>'s game」 · 대기 맵 So_LOD · 채팅).
//   목록에만 보이는 값이다 — 들어간 방의 실제 맵은 채널이 정한다(`--wait-maps`)
inline nox::wol::GameInfo MyRoomInfo(const std::string& nick) {
    nox::wol::GameInfo g;
    g.maxPlayers = 31;
    g.resolution = 2;
    g.name = nick + "'s game";   // 이름 칸 15바이트 — 넘치면 포장이 자른다
    g.map = "so_lod";
    g.flags = static_cast<uint16_t>((g.flags & ~nox::wol::kMapTypeMask) | nox::wol::kMapChat);
    return g;
}

inline void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }   // x86 — 작은 쪽 먼저

// 방 정보 머리 `:G1P3` 뒤 세 바이트(항목 +48 · 채널 +224 에 그대로 실린다)
inline uint32_t HeaderTail() {
    const std::string_view h = nox::wol::kHeader;
    return static_cast<uint8_t>(h[5]) | (static_cast<uint8_t>(h[6]) << 8) | (static_cast<uint8_t>(h[7]) << 16);
}

// 목록 항목 — self = 이 항목 주소(목록 고리 둘이 자기를 가리킴 · memhost4 와 같음) · chan = 채널 주소 · addr = 게임 서버 주소 글자
inline void BuildEntry(uint8_t* e, uint32_t self, uint32_t chan, const std::string& addr, const nox::wol::Raw& raw) {
    std::memset(e, 0, kEntrySize);
    Put32(e + 0, self);
    Put32(e + 4, self);
    Put32(e + 8, kNoPing);
    std::memcpy(e + 12, addr.data(), (std::min)(addr.size(), size_t{15}));   // 16칸 · 끝 0
    Put32(e + 32, chan);
    Put32(e + 44, kLocation);
    Put32(e + 48, HeaderTail());
    Put32(e + 96, kNoPing);
    std::memcpy(e + 100, raw.data(), raw.size());   // 69바이트 — +168 까지
}

// WOL 채널 — name 은 '#' 없이(16자까지) · users = `326` 의 사람 수 · ip = 게임 서버 IPv4 정수(a<<24|b<<16|c<<8|d)
inline void BuildChannel(uint8_t* c, const std::string& name, uint32_t users, uint32_t ip, const nox::wol::Raw& raw) {
    std::memset(c, 0, kChannelSize);
    Put32(c + 0, kChannelType);   // type
    Put32(c + 4, 1);              // minUsers
    Put32(c + 8, 32);             // maxUsers
    Put32(c + 12, users);         // currentUsers
    Put32(c + 20, 1);             // tournament
    Put32(c + 24, 1);             // ingame
    Put32(c + 28, 128);           // flags — `326` 끝 칸의 `128:`
    Put32(c + 32, kLocation);     // reserved
    Put32(c + 36, ip);            // ipaddr
    Put32(c + 40, 0xFFFFFFFFu);   // latency -1
    std::memcpy(c + 52, name.data(), (std::min)(name.size(), kChannelNameSize - 1));
    const std::string packed = nox::wol::PackRaw(raw);   // 머리 8 + 포장 78
    std::memcpy(c + 69, packed.data() + nox::wol::kHeader.size(), packed.size() - nox::wol::kHeader.size());   // topic(81칸)
    Put32(c + 224, HeaderTail());   // exInfo
}

}  // namespace hostgame
