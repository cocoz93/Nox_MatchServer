// BanCommand.h — 운영 명령 `nox_auth --ban` · `--ban-forever` · `--unban` · `--bans` 의 판단(Docs/MO_ACCOUNT_RANK_MAP_PLAN_1105.md 5절).
//
// 왜 따로 — `auth_main.cpp` 는 조립만 맡는다. 인자 짝 검사 · 저장소 부르기 · 찍을 줄을 여기 두면 시험이 메모리 저장소로 갈래를 전수로 돈다.
//   · 도는 서버와 따로 실행해 결과를 찍고 끝난다(포트 안 잡음). 도는 서버는 요청마다 DB 를 읽으므로 다음 요청부터 적용된다
//   · 사유 30자 — 런처 문구 「○월 ○일까지 정지되었습니다. 사유: …」가 판 안에서 두 줄을 넘지 않는 길이(캡처에서 31자가 판 폭의 약 81% — 한 줄 약 38자)
//   · 사유에 조절 글자 금지 — 답 둘째 줄(`LoginLine.h`)에 실린다
//   · 기간 1 ~ 3650일 — 더 긴 정지는 영구로. 끝 시각이 영구 표시값(`kBanForever`)에 닿지 않게
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "AccountStore.h"

namespace nox::auth {

constexpr size_t kBanReasonMaxChars = 30;
constexpr uint32_t kBanDaysMax = 3650;

enum class BanAction : uint8_t { None, Ban, BanForever, Unban, List, kCount };

struct BanCommand {
    BanAction action = BanAction::None;
    std::string name;     // UTF-8(`CommandLineToArgvW` → `auth_main.cpp`)
    uint32_t days = 0;
    std::string reason;   // UTF-8
};

// 인자 짝이 맞나. false = err 에 까닭 한 줄
bool CheckBanCommand(const BanCommand& c, std::string& err);

// 저장소를 불러 찍을 줄을 lines 에 쌓는다. false = 안 됨(까닭도 lines 에 · 종료 코드 1)
bool RunBanCommand(const BanCommand& c, IAccountStore& db, uint32_t nowUnix, std::vector<std::string>& lines);

// 끝 시각 글(`YYYY-MM-DD HH:MM` · 이 PC 시각) · 최댓값 = 「영구」
std::string BanUntilText(uint32_t untilUnix);

}  // namespace nox::auth
