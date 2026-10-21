// LobbyStore.h — 로비의 저장소 경계(MO 7단계 조각 4 — `Docs/ACCOUNT_DB_DESIGN.md` 「누가 무엇을 맡나」).
//
// 왜 — 계정 · 판 기록의 정본은 DB(MySQL)지만 로비 규약(`Lobby` · `RoomMirror`)은 DB 를 모른다. 이 경계만 안다.
//   · 요청은 곧바로 돌아오고 답은 나중에 `TakeAnswers` 로 — 로비 스레드는 DB 를 기다리지 않는다(새 방 미룬 답과 같은 틀 `Lobby.h`).
//     로비 루프가 바퀴마다 꺼내 갈래(로그인 · 판 저장)대로 넘긴다
//   · 구현 둘 — DB 워커(조각 7 `nox_db` · 로비 실행 파일이 조립) · 메모리 가짜(`CMemoryLobbyStore` — `--no-db` · 시험).
//     메모리 가짜도 답을 다음 바퀴에 낸다(10-21 정할 것 b — 소켓 시험이 DB 와 같은 미룸 · 뒷줄 쌓기 · SAVED 길을 지나게)
//   · 요청 번호는 부르는 쪽이 매긴다 — 갈래마다 따로라 답에 갈래가 실린다
//   · 계정 이름은 바이트 그대로 같음으로 찾는다(대소문자를 가린다 — 정품 로비가 `Wn` · `wn` 을 같게 보는지 미확인 · 계획 차이 6)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "nox/MatchRecord.h"

namespace nox::wol {

enum class StoreAnswerKind : uint8_t { Login, SaveMatch, kCount };
// 계정 확인 — 실패는 저장소가 답을 못 낸 것(DB 끊김 · 시간 초과). 이름 규칙(길이 등)은 로비가 묻기 전에 가른다
enum class LoginResult : uint8_t { Ok, Failed, kCount };
// 판 저장 — 못 넣음(Failed)이면 로비는 SAVED 를 안 보낸다(게임 서버가 30초 뒤 다시 · `LobbyClient.h` `kResultResendMs`)
//   ★ 7단계 조각 7 — Invalid = 저장소 칸에 안 맞는 판(글 길이 · UTF-8 아닌 원문 · 사람 수). 다시 보내도 같으므로 로비는 SAVED bad
//     (Failed 로 두면 게임 서버가 30초마다 끝없이 다시 보낸다 — MySQL 엄격 모드는 넘는 글을 거절한다)
enum class SaveResult : uint8_t { Saved, Duplicate, Invalid, Failed, kCount };

// 판 기록 표 칸 한도(바이트 — `tools/db/001_init.sql` 과 같은 값 · 글 칸은 ASCII 라 글자 수 = 바이트 수)
constexpr size_t kMatchKeyBytes = 48;
constexpr size_t kRoomNameBytes = 64;
constexpr size_t kMapKeyBytes = 64;
constexpr size_t kModeBytes = 32;
constexpr size_t kEndReasonBytes = 32;
constexpr size_t kWinnerKindBytes = 8;
constexpr size_t kAccountNameBytes = 16;   // accounts.name · match_players.name · matches.winner_name
constexpr size_t kMaxMatchPlayers = 255;   // match_players.seat TINYINT UNSIGNED
// 판 하나가 저장소 칸에 맞나(안 맞으면 `why` 에 처음 걸린 칸) — 메모리 가짜 · DB 워커가 같은 잣대로
bool MatchFitsStore(const MatchRecord& rec, const std::string& rawJson, std::string& why);

// 판을 보낸 게임 서버 — 등록 주소 · 포트(판 번호는 게임 서버 하나 안에서만 유일해 이것과 묶어 겹침을 가른다)
struct MatchSource {
    uint32_t ip = 0;     // 등록 줄 값 그대로(0 = 로비 쪽 주소)
    uint16_t port = 0;
};

struct StoreAnswer {
    StoreAnswerKind kind = StoreAnswerKind::kCount;
    uint32_t req = 0;
    LoginResult login = LoginResult::kCount;   // Login
    uint64_t accountId = 0;                    // Login Ok — 계정 번호
    bool created = false;                      // Login Ok — 처음 보는 이름이라 만듦(아이디만 — 결정 2)
    SaveResult save = SaveResult::kCount;      // SaveMatch
    std::string why;                           // 실패 까닭(기록용)
};

class ILobbyStore {
public:
    virtual ~ILobbyStore() = default;
    // 이름으로 계정 찾기 · 없으면 만들기 · 마지막 로그인 적기
    virtual void CheckLogin(uint32_t req, const std::string& name) = 0;
    // 판 하나(한 트랜잭션) — 같은 (게임 서버 · 판 번호)가 이미 있으면 Duplicate · `rawJson` = 받은 줄 그대로(파일 줄 대조용)
    virtual void SaveMatch(uint32_t req, const MatchSource& src, const MatchRecord& rec, const std::string& rawJson) = 0;
    virtual void TakeAnswers(std::vector<StoreAnswer>& out) = 0;   // 덧붙인다
};

// 메모리 가짜 — 답은 다음 `TakeAnswers` 에(DB 워커의 「다른 스레드가 나중에」를 바퀴 하나로 흉내)
class CMemoryLobbyStore final : public ILobbyStore {
public:
    void CheckLogin(uint32_t req, const std::string& name) override;
    void SaveMatch(uint32_t req, const MatchSource& src, const MatchRecord& rec, const std::string& rawJson) override;
    void TakeAnswers(std::vector<StoreAnswer>& out) override;

    // 시험 손잡이 — 저장소가 답을 못 내는 길(DB 끊김 흉내)
    void SetFailLogins(bool on) { failLogins_ = on; }
    void SetFailSaves(bool on) { failSaves_ = on; }

    size_t AccountCount() const { return accounts_.size(); }
    uint64_t AccountId(const std::string& name) const;   // 0 = 없음
    size_t MatchCount() const { return matches_.size(); }
    const MatchRecord* FindMatch(const MatchSource& src, const std::string& match) const;   // 없으면 nullptr

private:
    using MatchKey = std::tuple<uint32_t, uint16_t, std::string>;
    std::vector<StoreAnswer> pending_;
    std::map<std::string, uint64_t> accounts_;
    uint64_t nextAccount_ = 1;
    std::map<MatchKey, MatchRecord> matches_;
    bool failLogins_ = false;
    bool failSaves_ = false;
};

}  // namespace nox::wol
