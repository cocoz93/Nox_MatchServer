// RankNotice.h — 로비 줄 `NOXRANK WARN` · `BLOCKED` · `BANNED` 읽기와 안내 문구(머리 파일만 — 시험이 본 코드가 곧 DLL 코드).
//
// 왜 — 줄 서기 제한(플랜 11-05 7절)을 받은 사람이 까닭을 모르면 랭크 단추가 고장 난 줄 안다. 문구는 DLL 이 쥔다(한글을 로비 줄에 안 실음 · 플랜 9절 문구 그대로).
//   갈래가 넷이라 값을 돌려주는 함수로 두고 시험이 전수로 돈다
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
namespace ranknotice {
enum class Kind : uint8_t { None, WarnAccept, WarnMatch, Blocked, Banned, kCount };
struct Line {
    Kind kind = Kind::None;
    uint32_t value = 0;   // Blocked = 남은 초 · Banned = 끝 시각(유닉스 초)
};
// `sub` = `NOXRANK ` 뒤 글. 모르는 꼴이면 None — 낱말을 늘려도 옛 DLL 이 안 깨지는 규칙과 같게 조용히 넘긴다
inline bool ParseNumber(const char* s, uint32_t& out) {
    if (*s == '\0') return false;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + static_cast<uint64_t>(*s - '0');
        if (v > 0xffffffffull) return false;
    }
    if (v == 0) return false;   // 0 초 제한 · 0 시각 정지는 없다(0 = 정지 아님 — `LoginNotice.h`)
    out = static_cast<uint32_t>(v);
    return true;
}
inline Line Parse(const char* sub) {
    Line l;
    if (std::strcmp(sub, "WARN accept") == 0) l.kind = Kind::WarnAccept;
    else if (std::strcmp(sub, "WARN match") == 0) l.kind = Kind::WarnMatch;
    else if (std::strncmp(sub, "BLOCKED ", 8) == 0 && ParseNumber(sub + 8, l.value)) l.kind = Kind::Blocked;
    else if (std::strncmp(sub, "BANNED ", 7) == 0 && ParseNumber(sub + 7, l.value)) l.kind = Kind::Banned;
    return l;
}
inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::None: return "없음";
    case Kind::WarnAccept: return "경고(수락)";
    case Kind::WarnMatch: return "경고(판)";
    case Kind::Blocked: return "제한";
    case Kind::Banned: return "정지";
    case Kind::kCount: break;
    }
    return "?";
}
// 정지 문구는 `loginnotice::Text` 몫(게임 안 정지 안내와 한 벌) — 여기선 빈 글
inline std::wstring Text(Kind k, uint32_t value) {
    switch (k) {
    case Kind::WarnAccept: return L"매칭을 수락하지 않아 경고를 받았습니다. 반복하면 랭크 매칭이 제한됩니다.";
    case Kind::WarnMatch: return L"랭크 매칭이나 판을 끝까지 마치지 않아 경고를 받았습니다. 반복하면 랭크 매칭이 제한됩니다.";
    case Kind::Blocked: {
        wchar_t b[64];
        swprintf(b, 64, L"랭크 매칭 제한 중입니다. 남은 시간 %u분 %u초", value / 60, value % 60);
        return b;
    }
    case Kind::None:
    case Kind::Banned:
    case Kind::kCount: break;
    }
    return std::wstring();
}
}  // namespace ranknotice
