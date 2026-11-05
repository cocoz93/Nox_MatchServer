// RankPenalty.h — 랭크 수락 취소 · 이탈 횟수와 줄 서기 제한(플랜 11-05 7절).
//
// 왜 — 창이 뜨면 거절하거나 판을 나가는 사람 때문에 남은 사람이 매번 다시 기다렸다. 값(사용자 11-05) = 1회 경고 → 2회 1분 → 3회 5분 → 4~9회 15분 → 10회 계정 정지 1일.
//   · 횟수 하나에 이유를 전부 합친다 · 대기가 끝나도 횟수는 이어진다 · 랭크 판을 끝까지 마치거나 제한이 풀린 뒤 30분이면 0
//   · 열쇠 = 이름 — 판 결과에 이름만 실리고(`MatchRecord.h`) 재접속해도 횟수가 남아야 해서 접속 칸에 안 둔다
//   · 로비를 다시 켜면 초기화(새 테이블 없음) — 틀려도 사용자에게 유리한 쪽
//   · 소켓 · DB 를 모른다 · 판단만 한다 — 줄로 바꾸고 정지를 넣는 일은 `Lobby` 몫 · 로비 스레드 전용(잠금 없음)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
namespace nox::wol {
enum class RankStrike : uint8_t { AcceptTimeout, Declined, ListLeft, Closed, ReadyLeft, NoShow, MatchLeft, kCount };
constexpr size_t kRankStrikeCount = static_cast<size_t>(RankStrike::kCount);
const char* RankStrikeName(RankStrike why);

constexpr uint32_t kRankPenaltyResetSec = 30 * 60;      // 제한이 풀린 뒤(경고만 받았으면 그 시각부터) 이만큼이면 0 — 사용자 11-05
constexpr uint32_t kRankPenaltyBanCount = 10;           // 이 횟수에 계정 정지
constexpr uint32_t kRankPenaltyBanSec = 24 * 60 * 60;   // 정지 길이 1일 — 사용자 11-05
constexpr uint32_t kRankPenaltySweepSec = 5 * 60;       // 지난 칸 지우는 간격(값은 로그인 시도 제한과 같게 · 제 판단)
constexpr const char* kRankAutoBanReason = "랭크 매칭 거절·이탈 반복";   // `account_bans.reason`(플랜 9절)

struct PenaltyResult {
    enum class Kind : uint8_t { Warn, Block, Ban } kind = Kind::Warn;
    uint32_t blockSec = 0;   // Block 만
    uint32_t count = 0;      // 이번까지 센 횟수
};
// 횟수 → 단계. `blockOverrideSec` > 0 = 시험 인자 `--rank-penalty-sec` — 제한 시간만 줄인다(30분 · 정지 1일은 그대로 — 그 둘은 이 인자로 보려는 화면과 무관)
PenaltyResult PenaltyStep(uint32_t count, uint32_t blockOverrideSec = 0);

class CRankPenalty {
public:
    explicit CRankPenalty(uint32_t blockOverrideSec = 0) : blockOverrideSec_(blockOverrideSec) {}
    PenaltyResult Strike(const std::string& name, RankStrike why, uint32_t now);
    uint32_t BlockedForSec(const std::string& name, uint32_t now) const;
    void Clear(const std::string& name);
    // 안내를 아직 못 받은 1회 경고(끊김 · 안 들어옴 · 판 나가기 — 플랜 7절 「다음 랭크 단추에서」)를 꺼낸다. 한 번 꺼내면 지운다
    bool TakeWarnPending(const std::string& name, uint32_t now);
    // 0 = 칸 없음이거나 30분이 지남
    uint32_t CountOf(const std::string& name, uint32_t now) const;
    void Sweep(uint32_t now);
    size_t Size() const { return names_.size(); }
    uint64_t Struck(RankStrike why) const { return struck_[static_cast<size_t>(why)]; }
private:
    struct Entry {
        uint32_t count = 0;
        uint32_t blockUntil = 0;   // 유닉스 초
        uint32_t resetAt = 0;      // 이 시각부터 0
        bool warnPending = false;
    };
    std::map<std::string, Entry> names_;
    std::array<uint64_t, kRankStrikeCount> struck_{};
    uint32_t blockOverrideSec_;
    uint32_t nextSweep_ = 0;
};
}  // namespace nox::wol
