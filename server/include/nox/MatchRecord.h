// MatchRecord.h — 판 결과 JSON 읽기(MO 7단계 조각 4 — `Docs/ACCOUNT_DB_DESIGN.md` 「플랜을 짜며 고른 것」).
//
// 왜 — 판 결과는 게임 서버가 JSON 한 줄로 지어(`MatchRecorder.cpp` `FormatMatchResultJson`) 연결 줄 `RESULT` 로 로비에 보낸다.
//   로비가 DB 표 칸으로 나눠 넣으려면 읽어야 한다. 로비(`nox_lobby_core`)는 게임 규약(`nox_proto`)에 기대지 않으므로
//   판 결과 구조체(`MatchResult`)를 못 쓴다 — 칸만 따로 든 `MatchRecord` 로 읽는다(쓰개 ↔ 읽개 왕복은 proto_test 가 칸 전부 대조).
//   대안 「MySQL 이 JSON 을 통째로 받아 JSON_TABLE 로 풂」은 `\u00XX` 가 글자로 풀려 이름 바이트 칸(VARBINARY)과 어긋나 안 씀
// 읽는 꼴은 쓰개가 내는 것 하나뿐 — 객체 · 배열 · 글 · 정수 · null · 참거짓. 소수 · 지수는 쓰개가 안 내므로 문법 틀림으로 본다
// 글의 `\u00XX` 는 바이트 XX 로 되돌린다 — 쓰개는 UTF-8 이 아닌 글(코드페이지 949 이름 등)과 제어 바이트만 그렇게 쓴다(`PutJsonString`).
//   그래서 이름은 게임 서버가 쥔 바이트 그대로 돌아온다. `Ā` 이상은 쓰개가 안 내고 바이트 뜻이 갈리므로 값 틀림으로 본다
// 모르는 칸은 넘긴다(쓰개가 칸을 더해도 로비가 안 깨지게). 빠진 칸 · 타입 · 값 범위는 갈래마다 센다(시험은 개수로 본다)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nox {

// 읽기 결과 — 갈래마다 따로
enum class MatchJsonParse : uint8_t {
    Ok,
    Syntax,    // JSON 꼴이 아님(닫히지 않음 · 꼬리 · 날 제어 바이트 · 소수 · 앞 0 · 같은 열쇠 둘 · 깊이)
    Missing,   // 있어야 할 칸이 없음
    Type,      // 칸의 타입이 다름(글 자리에 수 등)
    Value,     // 값이 범위 밖(음수 시각 · netCode 65536 · 모르는 낱말 · `Ā` 이상)
    kCount
};
const char* MatchJsonParseName(MatchJsonParse p);

struct MatchRecordPlayer {
    bool hasAccount = false;   // 로비 이름이 있나(null = 로비를 안 거친 사람 — 기록기가 netCode 로 부른다)
    std::string account;       // 바이트 그대로
    uint16_t netCode = 0;
    bool left = false;         // state — finished / left
    uint8_t team = 0;          // 아래 넷은 finished 만 실린다(left 면 0)
    int32_t score = 0;         // 음수 있음(아레나 자살)
    uint32_t deaths = 0;
    bool observer = false;     // 실리면 늘 true
};

struct MatchRecordTeam {
    uint8_t id = 0;
    int32_t score = 0;
    uint16_t caps = 0;         // 깃발 득점
};

struct MatchRecord {
    std::string match;              // 판 번호 <게임 서버가 뜬 초>-<방>-<판>(빈 것은 값 틀림)
    int32_t room = 0;
    std::string roomName;           // 바이트 그대로 · 만들 때 이름
    std::string map;
    std::string mode;
    uint32_t start = 0;             // 유닉스 초
    uint32_t end = 0;
    uint32_t frames = 0;
    uint32_t sec = 0;
    std::string endReason;          // 갈래 이름 · 중단이면 aborted(낱말은 쓰개 몫 — 비지 않은지만 본다)
    std::string winnerKind;         // none · player · team · draw
    bool hasWinnerId = false;       // player · team 만
    uint32_t winnerId = 0;          // player = netCode · team = 팀 번호
    bool hasWinnerAccount = false;  // player 이고 로비 이름이 있을 때
    std::string winnerAccount;
    uint32_t limit = 0;
    uint32_t timeLimitSec = 0;
    std::vector<MatchRecordPlayer> players;
    std::vector<MatchRecordTeam> teams;   // 팀 판만(칸이 없으면 빈 것)
};

// 실패면 `out` 은 어디까지 채웠는지 모른다 — 쓰지 말 것. 단 `match` 만은 믿어도 된다(★ 7단계 조각 5 — 칸 가운데 맨 먼저 읽고,
//   비었으면 못 읽은 것 — 로비가 「못 읽음」 답 `SAVED <판 번호> bad` 에 싣는다). `field` 에 처음 걸린 칸 이름(기록용 · 없어도 됨)
MatchJsonParse ParseMatchRecordJson(std::string_view json, MatchRecord& out, std::string* field = nullptr);

}  // namespace nox
