// MatchHistoryScenario.h — 대전 기록 시험의 판 묶음(lobby_test 메모리 가짜 · db_test DB 워커가 같이 쓴다).
//   왜 — 판 줄을 읽는 코드가 저장소마다 따로라(메모리 = 구조체 · DB = SQL) 같은 판을 넣고 같은 줄이 나오는지 봐야 잣대가 안 갈린다
//   h1 · h2 · h3 · h4 = 계정 이름(DB 시험은 계정 행을 먼저 깐다)
#pragma once

#include <string>
#include <vector>

#include "nox/MatchRecord.h"

namespace nox::test {

inline MatchRecordPlayer HistSeat(const char* name, uint16_t netCode, int32_t score, uint32_t deaths, int kills = -1, uint8_t team = 0) {
    MatchRecordPlayer p;
    p.hasAccount = true;
    p.account = name;
    p.netCode = netCode;
    p.team = team;
    p.score = score;
    p.deaths = deaths;
    p.hasKills = kills >= 0;
    p.kills = kills >= 0 ? static_cast<uint32_t>(kills) : 0;
    return p;
}

inline MatchRecordPlayer HistLeft(const char* name, uint16_t netCode) {
    MatchRecordPlayer p;
    p.hasAccount = true;
    p.account = name;
    p.netCode = netCode;
    p.left = true;
    return p;
}

inline MatchRecord HistMatch(const std::string& key, const char* map, uint32_t end, uint32_t frames, bool ranked, const char* reason, const char* kind, int winnerId) {
    MatchRecord m;
    m.match = key;
    m.room = 1;
    m.roomName = "hist";
    m.map = map;
    m.mode = "arena";
    m.start = end > 100 ? end - 100 : 0;
    m.end = end;
    m.frames = frames;
    m.sec = frames / 30;
    m.endReason = reason;
    m.winnerKind = kind;
    m.hasWinnerId = winnerId >= 0;
    m.winnerId = winnerId >= 0 ? static_cast<uint32_t>(winnerId) : 0;
    m.ranked = ranked;
    return m;
}

// 표 차례 = 저장 차례(점수가 그 차례로 고쳐진다 — ★ 11-04 전적 검색). 끝난 시각은 안 본다 — 마지막 판(끝 시각 500)이 맨 위에 와야 한다
inline std::vector<MatchRecord> HistoryScenario() {
    std::vector<MatchRecord> v;
    for (int i = 0; i < 22; ++i) {   // h3 혼자 22판 — 표 한도 20 · 승패는 전체
        MatchRecord m = HistMatch("h-f" + std::to_string(i), "estate", 100 + static_cast<uint32_t>(i), 30, false, "score_player", "player", 103);
        m.players = {HistSeat("h3", 103, 1, 0, 1)};
        v.push_back(m);
    }
    {   // 판 1 — 랭크 중단. h1 나감(−10) · h2 남음(+10)
        MatchRecord m = HistMatch("h-1", "so_lod", 1000, 300, true, "aborted", "none", -1);
        m.players = {HistLeft("h1", 101), HistSeat("h2", 102, 0, 0, 0)};
        v.push_back(m);
    }
    {   // 판 2 — 랭크 끝. h1 이김(+10) · h2 짐(−10) → 둘 다 1000
        MatchRecord m = HistMatch("h-2", "so_lod", 2000, 510, true, "score_player", "player", 101);
        m.players = {HistSeat("h1", 101, 3, 1, 3), HistSeat("h2", 102, 1, 3, 1)};
        v.push_back(m);
    }
    {   // 판 3 — 일반 셋 + 관전. h2 9 · h3 5 · h1 2(킬 칸 없는 옛 줄) · 관전 점수 99 는 등수에 안 든다
        MatchRecord m = HistMatch("h-3", "estate", 3000, 600, false, "score_player", "player", 102);
        MatchRecordPlayer ob = HistSeat("h4", 104, 99, 0, 0);
        ob.observer = true;
        m.players = {HistSeat("h1", 101, 2, 4), HistSeat("h2", 102, 9, 0, 9), HistSeat("h3", 103, 5, 1, 5), ob};
        v.push_back(m);
    }
    {   // 판 4 — 일반 비김
        MatchRecord m = HistMatch("h-4", "estate", 3000, 900, false, "time_limit", "draw", -1);
        m.players = {HistSeat("h1", 101, 2, 2, 2), HistSeat("h2", 102, 2, 2, 2)};
        v.push_back(m);
    }
    {   // 판 5 — 일반 팀. h1 팀 1 이김 · h2 팀 2 짐
        MatchRecord m = HistMatch("h-5", "lod", 4000, 1800, false, "score_team", "team", 1);
        m.players = {HistSeat("h1", 101, 5, 1, 5, 1), HistSeat("h2", 102, 1, 5, 1, 2)};
        v.push_back(m);
    }
    {   // 판 6 — h1 은 관전만(기록에 안 나와야) · h4 이김
        MatchRecord m = HistMatch("h-6", "estate", 5000, 300, false, "score_player", "player", 104);
        MatchRecordPlayer ob = HistSeat("h1", 101, 0, 0, 0);
        ob.observer = true;
        m.players = {ob, HistSeat("h4", 104, 1, 0, 1)};
        v.push_back(m);
    }
    {   // 판 7 — 늦게 다시 보낸 옛 판(끝 시각은 이르고 저장은 맨 나중). 랭크 h2 이김 — 점수는 저장할 때 고쳐지므로 표에서도 맨 위여야 점수 칸이 맞다
        MatchRecord m = HistMatch("h-late", "so_lod", 500, 600, true, "score_player", "player", 102);
        m.players = {HistSeat("h1", 101, 0, 2, 0), HistSeat("h2", 102, 2, 0, 2)};
        v.push_back(m);
    }
    return v;
}

inline std::vector<std::string> HistoryExpectH1() {
    return {"NOXRANK RECSUM 1 990 2 1 2 1 1 h1",
            "NOXRANK RECROW L 1 990 -10 so_lod 0 2 20 500",
            "NOXRANK RECROW W 0 - - lod 5 1 60 4000",
            "NOXRANK RECROW D 0 - - estate 2 2 30 3000",
            "NOXRANK RECROW 3 0 - - estate - 4 20 3000",
            "NOXRANK RECROW W 1 1000 +10 so_lod 3 1 17 2000",
            "NOXRANK RECROW Q 1 990 -10 so_lod - - 10 1000",
            "NOXRANK RECEND 6"};
}

inline std::vector<std::string> HistoryExpectH2() {
    return {"NOXRANK RECSUM 1 1010 1 2 1 1 1 h2",
            "NOXRANK RECROW W 1 1010 +10 so_lod 2 0 20 500",
            "NOXRANK RECROW L 0 - - lod 1 5 60 4000",
            "NOXRANK RECROW D 0 - - estate 2 2 30 3000",
            "NOXRANK RECROW W 0 - - estate 9 0 20 3000",
            "NOXRANK RECROW L 1 1000 -10 so_lod 1 3 17 2000",
            "NOXRANK RECROW W 1 1010 +10 so_lod 0 0 10 1000",
            "NOXRANK RECEND 6"};
}

// h3 — 랭크 판 없음 · 판 3 의 2등 + 22판 승 · 표는 20줄(판 3 + 나중 19판)
inline std::vector<std::string> HistoryExpectH3() {
    std::vector<std::string> v = {"NOXRANK RECSUM 0 1000 0 0 0 22 1 h3", "NOXRANK RECROW 2 0 - - estate 5 1 20 3000"};
    for (int i = 21; i >= 3; --i) v.push_back("NOXRANK RECROW W 0 - - estate 1 0 1 " + std::to_string(100 + i));
    v.push_back("NOXRANK RECEND 20");
    return v;
}

inline std::vector<std::string> HistoryExpectNobody() {
    return {"NOXRANK RECSUM 0 1000 0 0 0 0 0 nobody", "NOXRANK RECEND 0"};
}

// ★ 11-04 전적 검색 — 계정 유무(`StoreAnswer::accountFound`). 저장소 둘이 같은 답이어야 한다
//   h4 = 관전 · 이김 · h5 = 판 없는 계정(시험이 미리 넣는다) · H1 = 대소문자만 다름(이름은 바이트 그대로 비교) · nobody = 계정 없음
struct HistoryAccountCase {
    const char* name;
    bool found;
};
inline std::vector<HistoryAccountCase> HistoryAccountCases() {
    return {{"h1", true}, {"h2", true}, {"h3", true}, {"h4", true}, {"h5", true}, {"H1", false}, {"nobody", false}};
}

}  // namespace nox::test
