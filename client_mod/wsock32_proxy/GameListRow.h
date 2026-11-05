#pragma once
// ★ 11-06 플랜1 조각 6 ㉓ 게임 목록 줄 — 정품 서버 항목의 값을 화면 글로 바꾸는 판단(DLL 본체 · `auth_client_test` 가 같이 씀).
//   상태 칸은 「열림 · 닫힘」 둘뿐이다(사용자 11-05) — 정품의 「다찼음」도 못 들어가는 방이라 닫힘으로 묶고 까닭은 플레이어 칸(8/8)이 보여 준다.
//   맵 칸은 정품의 핑 칸 자리다. 핑은 이 서버에서 늘 `--` 라 비어 있는 것과 같다.
#include <cstddef>

namespace gamelist {

constexpr unsigned char kFlagClosed = 0x10;   // 서버 항목 +0x64 — 정품 줄 함수 · 필터가 보는 닫힘 비트(`0x43b6c9` · `0x49c162`)

enum class Status : unsigned char { Open, Closed };

inline Status StatusOf(unsigned char flags, unsigned char players, unsigned char maxPlayers)
{
    // 꽉 찬 방 비교는 정품 줄 함수와 같다(`0x43b71b` — 인원이 최대보다 작아야 열림)
    return (flags & kFlagClosed) != 0 || players >= maxPlayers ? Status::Closed : Status::Open;
}

inline const wchar_t* StatusText(Status s) { return s == Status::Open ? L"열림" : L"닫힘"; }

// 서버 항목 +111 의 맵 칸(방 정보의 9바이트 — 끝 글자가 0 이 아닐 수 있다) → 화면 글. 보이는 ASCII 만 옮기고 그 밖의 바이트에서 멈춘다.
// 옮긴 글이 없으면 false(호출자는 정품 글을 그대로 둔다)
inline bool MapText(const char* raw, wchar_t* out, size_t outCount)
{
    constexpr size_t kMapBytes = 9;
    size_t n = 0;
    for (; n < kMapBytes && n + 1 < outCount; ++n) {
        const unsigned char c = static_cast<unsigned char>(raw[n]);
        if (c < 0x20 || c > 0x7e) break;
        out[n] = static_cast<wchar_t>(c);
    }
    if (outCount != 0) out[n] = 0;
    return n != 0;
}

// 닫힌 방이 목록에서 사라지지 않게 정품 필터 값을 어떻게 고치나(필터 함수 `0x49c100` 의 세 갈래 — 역어셈블 기준).
// 모드 0 = 안 거름 · 1 = 닫힘 · 암호를 무조건 거름 · 2 = 값 묶음(+4 가 켜졌을 때 닫힘을 거름)
enum class FilterFix : unsigned char { None, ModeOff, ClosedOff };

inline FilterFix FilterFixOf(unsigned long mode, bool closedOn)
{
    if (mode == 1) return FilterFix::ModeOff;
    if (mode == 2 && closedOn) return FilterFix::ClosedOff;
    return FilterFix::None;
}

}  // namespace gamelist
