// LoginNotice.h — 자동 진행이 로그인 화면에서 돌아왔을 때 띄울 안내 고르기(머리 파일만 — 시험이 본 코드가 곧 DLL 코드).
//
// 왜 — 열쇠 거절이 아니면 모두 「서버에 연결할 수 없습니다」를 띄웠다. 정지된 사람이 그 글을 보면 다시 눌러도 안 되는 까닭을 모른다
//   (플랜 11-05 5절 · 문구는 9절). 갈래가 넷이라 값을 돌려주는 함수로 두고 시험이 전수로 돈다
//   · 끝 시각은 시 단위로 올린다 — 14시 37분 끝을 「14시까지」라 적으면 14시 10분에 다시 눌러도 막힌다
#pragma once

#include <cstdint>
#include <ctime>
#include <cwchar>
#include <string>

#include "nox/LoginLine.h"

namespace loginnotice {

enum class Kind : uint8_t { Unreachable, Expired, Banned, BannedForever, kCount };

// 정지가 먼저 — 정지 답에선 열쇠 거절 표시가 서지 않지만, 둘 다 서도 다시 로그인해 풀리는 일이 아니다
inline Kind Pick(bool keyInvalid, uint32_t bannedUntil) {
    if (bannedUntil == nox::kBanForever) return Kind::BannedForever;
    if (bannedUntil != 0) return Kind::Banned;
    return keyInvalid ? Kind::Expired : Kind::Unreachable;
}

inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::Unreachable: return "서버 연결";
    case Kind::Expired: return "로그인 만료(key_invalid)";
    case Kind::Banned: return "계정 정지";
    case Kind::BannedForever: return "영구 정지";
    case Kind::kCount: break;
    }
    return "?";
}

inline std::wstring Text(Kind k, uint32_t bannedUntil) {
    switch (k) {
    case Kind::Unreachable: return L"서버에 연결할 수 없습니다. 잠시 후 온라인을 다시 눌러 주세요.";
    case Kind::Expired: return L"로그인이 만료되었습니다. 런처에서 다시 로그인해 주세요.";   // ★ 11-04 플랜1 — 열쇠 거절만. 다시 눌러도 안 되는 실패를 「잠시 후 다시」로 안내하면 사람이 갇힌다
    case Kind::BannedForever: return L"계정이 영구 정지되었습니다.";
    case Kind::Banned: {
        const std::time_t t = static_cast<std::time_t>(static_cast<uint64_t>(bannedUntil) + 3599u);
        std::tm tm{};
        if (localtime_s(&tm, &t) != 0) break;
        wchar_t b[64];
        swprintf(b, 64, L"계정이 %d월 %d일 %d시까지 정지되었습니다.", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
        return b;
    }
    case Kind::kCount: break;
    }
    return L"계정이 정지되었습니다.";
}

}  // namespace loginnotice
