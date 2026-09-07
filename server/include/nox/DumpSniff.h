// DumpSniff.h — 덤프에서 「서버가 되려면 있어야 하는 것」을 뽑는다.
//
// 왜 공용인가 — 대조 장치(replay.cpp)는 이걸 갖고 있었는데 진짜 서버(main.cpp)는
// 없었다. 그래서 클라가 붙어도 `Connected!` 에서 멈췄다(2026-09-07 실측).
// 시험은 하네스 안에서만 통과했고 본체에는 배선이 빠져 있던 것이다.
// 같은 일이 다시 벌어지지 않게 한 곳에 둔다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nox/Server.h"

namespace nox {
namespace dump {

// 덤프 한 줄. 방향·상대·바이트.
struct Line {
    std::string dir;    // "RECV" 또는 "SEND"
    std::string peer;
    std::vector<uint8_t> data;
};

// 12:15:09.262 RECV 172.17.192.1:54721 len=12 | 00 00 0c ... | ....
//
// 정규식을 안 쓴다 — 6만 줄이 넘어 눈에 띄게 느려진다.
bool ParseLine(const char* s, Line& out);

// 정품 호스트가 쓴 조건. 내 서버가 **같은 조건**이라야 바이트가 맞는지 물을 수 있다.
struct Setup {
    std::vector<uint8_t> infoBody;
    std::vector<uint8_t> joinBody;     // 접속 직후 216바이트 묶음
    std::vector<uint8_t> useMapBody;   // 그 다음 41바이트 USE_MAP
    bool haveInfo = false;
    // 접속마다 정품이 쓴 값들. 순서대로 쌓는다.
    std::vector<Server::AcceptScript> accepts;
};

// ⚠ 수락 뒤의 SEND 는 **암호문**이라 상대별 키로 풀어야 읽힌다.
//   키는 수락 조각(평문)에 실려 오니, 순서대로 훑으면 자연히 알게 된다.
// ⚠ 덤프는 **서버 쪽에서 뜬 것**이라야 한다. 클라 쪽은 방향이 뒤집혀 있다.
bool Sniff(const char* path, Setup& out);

}  // namespace dump
}  // namespace nox
