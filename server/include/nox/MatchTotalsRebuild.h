// MatchTotalsRebuild.h — 승패 합계 표(`match_totals`)를 판 행에서 다시 계산한다(`Docs/RECORD_SEARCH_PLAN.md` 조각 1 · `tools/db/006_match_totals.sql`).
//
// 왜 — 합계는 파생 값이고 판 행(`matches` · `match_players`)이 정본이다. 옛 판을 처음 채울 때 · 판정 규칙이 바뀌었을 때
//   저장 때와 같은 판정(`MatchTotals::Add`)으로 덮어쓴다. 운영 프로세스에 일회성 작업을 안 섞으려고 실행 파일 `nox_match_totals` 로 뺐다
//   · 한 트랜잭션 — 전부 0 으로 → 판 행 읽기 → 계정마다 덮어쓰기. 실패하면 되돌아가 표는 그대로
//   · `nox_mo` 계정엔 DELETE 가 없어 행을 안 지운다 — 판이 사라진 계정은 0 행으로 남는다(읽는 쪽엔 행 없음과 같다)
//   · 로비가 꺼진 때 돌린다. 떠 있어 어긋나도 파생 값이라 다시 돌리면 맞는다
// MySQL 머리 파일은 .cpp 에만 — 부르는 쪽은 MySQL 을 모른다
#pragma once

#include <string>

#include "nox/DbConfig.h"

namespace nox::db {

struct MatchTotalsRebuildResult {
    unsigned long long existing = 0;   // 시작할 때 0 으로 돌린 행
    unsigned long long rows = 0;       // 읽은 판 줄(관전 아님 · 계정 있는 사람)
    unsigned long long accounts = 0;   // 합계를 쓴 계정
};

// `cfg` 의 DB 에 붙어 다시 계산. 실패면 false · `err`(표는 그대로)
bool RebuildMatchTotals(const DbConfig& cfg, MatchTotalsRebuildResult& out, std::string& err);

}  // namespace nox::db
