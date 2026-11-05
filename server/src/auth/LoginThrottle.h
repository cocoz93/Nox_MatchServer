// LoginThrottle.h — 로그인 시도 제한(플랜 Docs/MO_ACCOUNT_RANK_MAP_PLAN_1105.md 3절).
//
// 왜 — 암호가 최소 4자라 대입에 약하고, 암호 확인 한 번이 해시 약 650ms 라 일꾼 넷이 초당 약 6번이면 꽉 찬다.
//   그래서 해시를 돌리기 전에 막는다. 이미 로그인한 사람은 열쇠 길(`/token`)이라 여기를 안 지난다 —
//   남이 일부러 잠가도 막히는 것은 새 PC 의 암호 로그인뿐이다
// 값
//   · 계정별 — 5회째 1분 · 풀린 뒤 또 틀리면 5분 · 그다음부터 15분. 성공하거나 마지막 실패에서 1시간이면 0
//   · 주소별 — 로그인 실패 + 가입 시도를 합쳐 20회째부터 10분. 마지막 실패에서 1시간이면 0.
//     성공해도 안 지운다(제 계정 하나로 주소의 셈을 지우며 남의 계정을 대입하지 못하게)
// 소켓 · DB 를 모른다 — 이름 · 주소 · 시각만 받는다. 없는 이름은 계정 칸을 안 만든다(없는 이름을 쏟아부어 메모리가 자라지 않게 — 주소별로만 셈)
// 저장은 이 프로세스 메모리 — 다시 켜면 초기화. 일꾼 여럿이 같이 쓰므로 잠금 하나를 두되 해시 동안은 쥐지 않는다(부르는 쪽이 해시 앞뒤로 따로 부름)
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace nox::auth {

constexpr uint32_t kThrottleResetSec = 3600;    // 마지막 실패에서 이만큼 지나면 0 — 플랜 1절 「1시간은 제 제안」
constexpr uint32_t kAddressLimit = 20;          // 주소별 — 이 횟수째부터 잠금
constexpr uint32_t kAddressLockSec = 600;       // 주소별 잠금 10분
constexpr uint32_t kThrottlePruneEverySec = 300;   // 지난 칸을 지우는 간격(주소가 많이 와도 메모리가 안 자라게 · 시간 측정 값이 아니라 충분히 드문 값)

// 잠근 쪽 — 런처 문구가 다르다(9절 「암호를 여러 번 틀렸습니다」 · 「시도가 너무 많습니다」)
enum class ThrottleScope : uint8_t { Account, Address };

// 계정별 — 실패 횟수 → 잠금 초(0 = 안 잠금). 값을 돌려주는 함수라 시험이 전수로 돈다
uint32_t AccountLockSec(uint32_t failCount);
// 주소별 — 실패 횟수 → 잠금 초
uint32_t AddressLockSec(uint32_t failCount);

class CLoginThrottle {
public:
    // 0 = 안 막힘. 계정 · 주소 중 긴 쪽(같으면 계정). scope 는 막혔을 때만 적는다
    uint32_t RetryAfterSec(const std::string& name, uint32_t ip, uint32_t now, ThrottleScope* scope = nullptr);
    uint32_t AddressRetryAfterSec(uint32_t ip, uint32_t now);   // 가입 — 이름이 아직 계정이 아니므로 주소만 본다
    // name = nullptr — 없는 이름 · 가입 시도(주소별로만 셈).
    // 이미 잠긴 칸은 안 센다 — 일꾼 넷이 같이 해시를 돌던 요청이 뒤늦게 돌아와 한 단계 더 잠그지 않게
    void OnFail(const std::string* name, uint32_t ip, uint32_t now);
    void OnSuccess(const std::string& name, uint32_t now);

    // 시험 — 칸 수(없는 이름이 칸을 안 만드는지 · 지난 칸이 지워지는지)
    size_t AccountSlots() const;
    size_t AddressSlots() const;

private:
    struct Slot {
        uint32_t fails = 0;
        uint32_t lastFail = 0;
        uint32_t lockedUntil = 0;
    };
    static void Count(Slot& s, uint32_t now, uint32_t (*lockSec)(uint32_t));
    template <class Map, class Key>
    static uint32_t Left(Map& m, const Key& k, uint32_t now);
    void Prune(uint32_t now);

    mutable std::mutex mu_;
    std::map<std::string, Slot> accounts_;
    std::map<uint32_t, Slot> addresses_;
    uint32_t lastPrune_ = 0;
};

}  // namespace nox::auth
