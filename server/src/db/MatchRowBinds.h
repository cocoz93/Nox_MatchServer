// MatchRowBinds.h — 저장된 판 줄 읽기 · 합계 쓰기의 칸 묶기(로비 DB 워커 · 다시 계산 도구가 같이 쓴다 · 이 폴더 안에서만).
//   왜 — 줄을 읽는 SQL 칸과 묶는 코드가 두 곳에 있으면 판정의 입력이 갈린다(합계를 다시 계산한 값 ≠ 저장 때 올린 값)
#pragma once

#include <cstdint>

#include "MySqlBind.h"
#include "nox/LobbyStore.h"   // 칸 한도 · `MatchHistoryRow` · `MatchTotals`

// 칸 차례 = `MatchRowBinds::Bind`. FROM 에 p(match_players) · m(matches) 가 있어야 한다.
//   나간 사람은 점수가 NULL 이라 `higher` 0 · 부 질의는 기본 열쇠 앞부분(match_id)으로 찾는다
#define NOX_SQL_MATCH_ROW_COLUMNS                                                                                                                     \
    "m.ranked, m.map_key, m.end_unix, m.frames, m.end_reason, m.winner_kind, m.winner_id, p.state, p.net_code, p.team, p.kills, p.deaths, p.rating_delta, " \
    "(SELECT COUNT(*) FROM match_players o WHERE o.match_id = p.match_id AND o.observer = 0 AND o.state = 'finished' AND o.score > p.score), "             \
    "(SELECT COUNT(*) FROM match_players o WHERE o.match_id = p.match_id AND o.observer = 0)"

namespace nox::db {

struct MatchRowBinds {
    static constexpr size_t kColumns = 15;
    // 글 칸 = 표 칸 한도(ASCII) + 끝 · 넘으면 꺼내기가 잘림으로 실패한다(저장 때 `MatchFitsStore` 가 막아 안 생김)
    char mapBuf[wol::kMapKeyBytes + 1];
    char reasonBuf[wol::kEndReasonBytes + 1];
    char kindBuf[wol::kWinnerKindBytes + 1];
    char stateBuf[9];
    unsigned long mapLen = 0, reasonLen = 0, kindLen = 0, stateLen = 0;
    uint8_t ranked = 0, team = 0;
    uint16_t netCode = 0;
    uint32_t endUnix = 0, frames = 0, winnerId = 0, kills = 0, deaths = 0;
    int32_t delta = 0;
    uint64_t higher = 0, seats = 0;
    bool nullWinner = false, nullTeam = false, nullKills = false, nullDeaths = false, nullDelta = false;

    // `r` 는 0 으로 비운 칸 `kColumns` 개
    void Bind(MYSQL_BIND* r) {
        auto str = [](MYSQL_BIND& b, char* buf, size_t cap, unsigned long& got) {
            b.buffer_type = MYSQL_TYPE_STRING;
            b.buffer = buf;
            b.buffer_length = static_cast<unsigned long>(cap);
            b.length = &got;
        };
        U8(r[0], ranked);
        str(r[1], mapBuf, sizeof(mapBuf), mapLen);
        U32(r[2], endUnix);
        U32(r[3], frames);
        str(r[4], reasonBuf, sizeof(reasonBuf), reasonLen);
        str(r[5], kindBuf, sizeof(kindBuf), kindLen);
        U32(r[6], winnerId);
        r[6].is_null = &nullWinner;
        str(r[7], stateBuf, sizeof(stateBuf), stateLen);
        U16(r[8], netCode);
        U8(r[9], team);
        r[9].is_null = &nullTeam;
        U32(r[10], kills);
        r[10].is_null = &nullKills;
        U32(r[11], deaths);
        r[11].is_null = &nullDeaths;
        I32(r[12], delta);
        r[12].is_null = &nullDelta;
        U64(r[13], higher);
        U64(r[14], seats);
    }
    wol::MatchHistoryRow Row() const {
        wol::MatchHistoryRow row;
        row.ranked = ranked != 0;
        row.map.assign(mapBuf, mapLen);
        row.end = endUnix;
        row.frames = frames;
        row.endReason.assign(reasonBuf, reasonLen);
        row.winnerKind.assign(kindBuf, kindLen);
        row.hasWinnerId = !nullWinner;
        row.winnerId = nullWinner ? 0 : winnerId;
        row.left = std::string(stateBuf, stateLen) == "left";
        row.netCode = netCode;
        row.team = nullTeam ? 0 : team;
        row.hasKills = !nullKills;
        row.kills = nullKills ? 0 : kills;
        row.hasDeaths = !nullDeaths;
        row.deaths = nullDeaths ? 0 : deaths;
        row.hasDelta = !nullDelta;
        row.delta = nullDelta ? 0 : delta;
        row.higher = static_cast<uint32_t>(higher);
        row.seats = static_cast<uint32_t>(seats);
        return row;
    }
};

// 합계 쓰기 칸 — 넣을 칸 일곱 + 겹칠 때 칸 여섯(`kUpsTotals` · 다시 계산의 덮어쓰기가 같은 차례)
constexpr size_t kMatchTotalsParams = 13;
inline void BindMatchTotals(MYSQL_BIND* p, const uint64_t& accountId, const wol::MatchTotals& t, const uint32_t& now) {
    U64(p[0], accountId);
    U32(p[1], t.games);
    U32(p[2], t.rankedWins);
    U32(p[3], t.rankedLosses);
    U32(p[4], t.normalWins);
    U32(p[5], t.normalLosses);
    U32(p[6], now);
    U32(p[7], t.games);
    U32(p[8], t.rankedWins);
    U32(p[9], t.rankedLosses);
    U32(p[10], t.normalWins);
    U32(p[11], t.normalLosses);
    U32(p[12], now);
}

}  // namespace nox::db
