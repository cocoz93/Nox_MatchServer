// rankmatch_test.cpp — 1:1 랭크 점수 묶기 판단(`RankMatch.h`) 시험. 플랜 `Docs/MO_RANK_MATCH_PLAN_1107.md` 4-3 갈래 · 6절 `rankmatch_test` 줄.
//
// 소켓 · 로비 없이 줄과 시각을 넣고 짝을 본다. 짝은 있다 · 없다가 아니라 짝 목록 전체(개수 · 누구 · 차례)를 맞춰 본다.
// 마지막 절은 아무 줄 2,000개로 성질 다섯(한 사람 한 짝 · 짝 수 × 2 + 남은 수 = 줄 수 · 짝의 앞은 오래 기다린 쪽 · 짝은 허용 차 안 · 남은 둘은 허용 차 밖)을 본다.
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "nox/RankMatch.h"

namespace {

int g_checks = 0;
int g_fails = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  [실패] %s\n", what.c_str());
    }
}

using nox::wol::AllowedGap;
using nox::wol::PickPairs;
using nox::wol::RankMatchRule;
using nox::wol::RankWaiter;
using Pairs = std::vector<std::pair<int, int>>;

constexpr uint32_t kSec = 1000;

std::string Show(const Pairs& p) {
    std::string s = "[";
    for (const auto& [a, b] : p) s += "(" + std::to_string(a) + "," + std::to_string(b) + ")";
    return s + "]";
}

void Expect(const Pairs& got, const Pairs& want, const std::string& what) {
    Check(got == want, what + " — 받음 " + Show(got) + " · 기대 " + Show(want));
}

// ① 허용 차 — min(시작 + 늘림 × ⌊기다림 ÷ 간격⌋, 상한)
void GapTests() {
    const RankMatchRule r;
    Check(r.startGap == 100 && r.growGap == 50 && r.growEveryMs == 10000 && r.capGap == 200, "① 기본값 100 · +50 · 10초 · 200(플랜 1절)");
    Check(AllowedGap(0, r) == 100, "① 0초 = 100");
    Check(AllowedGap(9999, r) == 100, "① 9.999초 = 100");
    Check(AllowedGap(10000, r) == 150, "① 10초 = 150");
    Check(AllowedGap(19999, r) == 150, "① 19.999초 = 150");
    Check(AllowedGap(20000, r) == 200, "① 20초 = 200");
    Check(AllowedGap(30000, r) == 200, "① 30초 = 200(상한)");
    Check(AllowedGap(0xFFFFFFFFu, r) == 200, "① 49일 = 200(곱이 32비트를 넘어도 상한)");

    RankMatchRule big;
    big.startGap = 300;
    Check(AllowedGap(0, big) == 200, "① 시작 300 > 상한 200 = 200");

    RankMatchRule still;
    still.growEveryMs = 0;
    Check(AllowedGap(60000, still) == 100, "① 간격 0 = 늘리지 않음(0 으로 안 나눔)");

    RankMatchRule wide;
    wide.capGap = 400;
    Check(AllowedGap(50000, wide) == 350 && AllowedGap(60000, wide) == 400 && AllowedGap(70000, wide) == 400, "① 상한 400 이면 50초 350 · 60초 400 · 70초 400");
}

// ② 짝 — 갈래마다 짝 목록 전체를 본다
void PairTests() {
    const RankMatchRule r;
    const uint32_t now = 1000000;

    Expect(PickPairs({}, now, r), {}, "② 줄 0명 = 짝 0");
    Expect(PickPairs({{1, 1000, now}}, now, r), {}, "② 줄 1명 = 짝 0");

    // 차가 허용 차와 같으면 묶고 1점 넘으면 안 묶음(0초 · 허용 100)
    Expect(PickPairs({{1, 1000, now}, {2, 1100, now}}, now, r), {{1, 2}}, "② 0초 차 100 = 묶음");
    Expect(PickPairs({{1, 1000, now}, {2, 1101, now}}, now, r), {}, "② 0초 차 101 = 안 묶음");
    Expect(PickPairs({{1, 1100, now}, {2, 1000, now}}, now, r), {{1, 2}}, "② 0초 차 −100(높은 쪽이 기준) = 묶음");

    // 시간이 지나 허용 차가 넓어지면 묶음 — 기준(오래 기다린 쪽)의 시간으로 잰다
    const std::vector<RankWaiter> late = {{1, 1000, 0}, {2, 1150, 0}};
    Expect(PickPairs(late, 9999, r), {}, "② 9.999초 차 150 = 안 묶음");
    Expect(PickPairs(late, 10000, r), {{1, 2}}, "② 10초 차 150 = 묶음");
    const std::vector<RankWaiter> mixed = {{1, 1000, 0}, {2, 1150, 9000}};   // 2 는 늦게 섬
    Expect(PickPairs(mixed, 10000, r), {{1, 2}}, "② 기준 10초 · 상대 1초 · 차 150 = 묶음(오래 기다린 쪽 시간)");

    // 차가 200 넘으면 언제까지나 짝 0
    const std::vector<RankWaiter> far = {{1, 1000, 0}, {2, 1201, 0}};
    Expect(PickPairs(far, 30 * kSec, r), {}, "② 30초 차 201 = 짝 0");
    Expect(PickPairs(far, 3600 * kSec, r), {}, "② 1시간 차 201 = 짝 0");
    Expect(PickPairs({{1, 1000, 0}, {2, 1200, 0}}, 3600 * kSec, r), {{1, 2}}, "② 1시간 차 200 = 묶음(상한과 같음)");

    // 셋 이상 — 가장 오래 기다린 사람이 가장 가까운 사람을 고름
    // A 1000(0초) · B 1090(5초) · C 1010(8초) · 지금 10초 → A 가 C(차 10)를 고름 · B 남음
    Expect(PickPairs({{1, 1000, 0}, {2, 1090, 5 * kSec}, {3, 1010, 8 * kSec}}, 10 * kSec, r), {{1, 3}}, "② 셋 — 기준이 가장 가까운 사람");
    // 넷 — 두 짝: A 1000 · B 1300 · C 1020 · D 1290 → (A,C) · (B,D)
    Expect(PickPairs({{1, 1000, 0}, {2, 1300, 1 * kSec}, {3, 1020, 2 * kSec}, {4, 1290, 3 * kSec}}, 4 * kSec, r), {{1, 3}, {2, 4}}, "② 넷 — 두 짝");

    // 차가 같으면 더 오래 기다린 쪽 — 줄 차례로 새로운 쪽을 앞에 둬도
    // A 1000(0초) · C 950(6초) · B 1050(3초) → A 가 B(3초에 섬 · 더 오래 기다림)를 고름
    Expect(PickPairs({{1, 1000, 0}, {3, 950, 6 * kSec}, {2, 1050, 3 * kSec}}, 10 * kSec, r), {{1, 2}}, "② 같은 차 — 더 오래 기다린 쪽(줄 차례는 뒤)");
    Expect(PickPairs({{1, 1000, 0}, {2, 1050, 3 * kSec}, {3, 950, 6 * kSec}}, 10 * kSec, r), {{1, 2}}, "② 같은 차 — 더 오래 기다린 쪽(줄 차례도 앞)");
    // 줄 선 시각도 같으면 줄 차례
    Expect(PickPairs({{1, 1000, 0}, {3, 950, 5 * kSec}, {2, 1050, 5 * kSec}}, 10 * kSec, r), {{1, 3}}, "② 같은 차 · 같은 시각 — 줄 차례 앞");

    // 기준이 못 잡히면 다음 기준으로 — A 1000(가장 오래) 은 100 안에 아무도 없음 · B 1500 · C 1550 이 묶임
    Expect(PickPairs({{1, 1000, 0}, {2, 1500, 1 * kSec}, {3, 1550, 2 * kSec}}, 3 * kSec, r), {{2, 3}}, "② 기준 못 잡힘 → 다음 기준");
    // 앞에서 못 잡힌 사람을 나중 기준이 고르지 않음 — A 1000(20초 · 허용 200) 은 B 1250 과 250 · B(0초 · 허용 100) 의 가장 가까운 사람이 A 여도 안 묶음
    Expect(PickPairs({{1, 1000, 0}, {2, 1250, 20 * kSec}}, 20 * kSec, r), {}, "② 못 잡힌 기준을 나중 기준이 안 고름");

    // 되돌린 사람은 처음 시각 — 줄 차례가 시각 순서와 어긋나도 시각으로 기준을 잡음
    // 플랜 예: D(20초에 섬)가 되돌림으로 맨 앞 · C(8초에 섬) · E(25초에 섬) · 지금 30초
    // 기준 C(22초 · 허용 200): E 1050(차 100) 이 D 1000(차 150)보다 가까움 → (C,E) · D 남음
    // 줄 차례로 기준을 잡으면 D(10초 · 허용 150) 가 E(차 50)를 골라 (D,E) 가 된다
    Expect(PickPairs({{4, 1000, 20 * kSec}, {3, 1150, 8 * kSec}, {5, 1050, 25 * kSec}}, 30 * kSec, r), {{3, 5}}, "② 되돌린 사람 — 기준은 줄 차례가 아니라 처음 시각");

    // 시계가 32비트를 넘어 돌아도 기다린 시간으로 견줌 — 줄 선 시각 0xFFFFF000(돌기 4.096초 전) · 지금 = 그 10초 뒤(5904)
    const uint32_t before = 0xFFFFF000u;
    const uint32_t after = before + 10 * kSec;   // 돌아서 작은 값
    Check(after < before, "② 시계 돎 — 지금 값이 줄 선 값보다 작음(시험 전제)");
    Expect(PickPairs({{1, 1000, before}, {2, 1150, before}}, after, r), {{1, 2}}, "② 시계 돎 — 10초 차 150 = 묶음");
    // 돌기 전에 선 사람(C)이 돈 뒤에 선 사람(B)보다 오래 기다림 — 값 크기로 견주면 B 가 기준이 된다
    Expect(PickPairs({{2, 1080, 5 * kSec}, {1, 1000, before}, {3, 1060, 6 * kSec}}, 8 * kSec, r), {{1, 3}}, "② 시계 돎 — 돌기 전에 선 사람이 기준");

    // 같은 사람이 두 짝에 안 들어감 — 셋이 모두 서로 가까워도 짝 하나 · 남은 하나
    const Pairs three = PickPairs({{1, 1000, 0}, {2, 1001, 0}, {3, 1002, 0}}, 0, r);
    Expect(three, {{1, 2}}, "② 셋이 모두 가까움 = 짝 하나");
    // 다섯 — 짝 둘 · 남은 하나
    const Pairs five = PickPairs({{1, 1000, 0}, {2, 1010, 0}, {3, 1020, 0}, {4, 1030, 0}, {5, 1040, 0}}, 0, r);
    Expect(five, {{1, 2}, {3, 4}}, "② 다섯 = 짝 둘 · 남은 하나");
}

// ③ 아무 줄 — 성질 다섯. 시각 · 점수 · 사람 수를 고정된 씨앗으로 섞는다(돌릴 때마다 같은 줄)
struct Lcg {
    uint32_t s;
    uint32_t Next() { return s = s * 1664525u + 1013904223u; }
    uint32_t Below(uint32_t n) { return (Next() >> 8) % n; }
};

void PropertyTests() {
    Lcg rng{12345};
    int queues = 0, people = 0, pairsSeen = 0, leftPairsSeen = 0;
    int dupFail = 0, countFail = 0, inGapFail = 0, outGapFail = 0, baseFail = 0;
    for (int t = 0; t < 2000; ++t) {
        RankMatchRule r;
        if (t % 4 == 1) r.capGap = 400;
        if (t % 4 == 2) { r.startGap = 0; r.growGap = 30; r.growEveryMs = 3000; }
        const uint32_t now = rng.Next();   // 시계가 도는 자리도 섞임
        const size_t n = rng.Below(9);
        std::vector<RankWaiter> q;
        for (size_t i = 0; i < n; ++i) q.push_back({static_cast<int>(i + 1), static_cast<int32_t>(800 + rng.Below(600)), now - rng.Below(40 * kSec)});
        const Pairs got = PickPairs(q, now, r);
        ++queues;
        people += static_cast<int>(n);
        pairsSeen += static_cast<int>(got.size());

        auto find = [&](int conn) -> const RankWaiter& {
            for (const RankWaiter& w : q) if (w.conn == conn) return w;
            return q.front();
        };
        std::vector<int> used(n + 1, 0);
        for (const auto& [a, b] : got) {
            ++used[a];
            ++used[b];
            const RankWaiter& wa = find(a);
            const RankWaiter& wb = find(b);
            const uint32_t waitedA = now - wa.sinceMs, waitedB = now - wb.sinceMs;
            if (waitedA < waitedB) ++baseFail;   // 기준(앞)이 둘 중 오래 기다린 쪽
            const int32_t gap = wa.rating > wb.rating ? wa.rating - wb.rating : wb.rating - wa.rating;
            if (gap > AllowedGap(waitedA, r)) ++inGapFail;
        }
        int rest = 0;
        for (size_t c = 1; c <= n; ++c) {
            if (used[c] > 1) ++dupFail;
            if (used[c] == 0) ++rest;
        }
        if (static_cast<int>(got.size()) * 2 + rest != static_cast<int>(n)) ++countFail;
        // 남은 둘은 둘 중 오래 기다린 쪽의 허용 차보다 멀다
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                if (used[q[i].conn] || used[q[j].conn]) continue;
                ++leftPairsSeen;
                const uint32_t wi = now - q[i].sinceMs, wj = now - q[j].sinceMs;
                const int32_t gap = q[i].rating > q[j].rating ? q[i].rating - q[j].rating : q[j].rating - q[i].rating;
                if (gap <= AllowedGap(wi > wj ? wi : wj, r)) ++outGapFail;
            }
        }
    }
    Check(dupFail == 0, "③ 한 사람이 두 짝에 안 들어감(" + std::to_string(dupFail) + ")");
    Check(countFail == 0, "③ 짝 수 × 2 + 남은 수 = 줄 수(" + std::to_string(countFail) + ")");
    Check(baseFail == 0, "③ 짝의 앞 = 둘 중 오래 기다린 쪽(" + std::to_string(baseFail) + ")");
    Check(inGapFail == 0, "③ 짝의 차 ≤ 기준의 허용 차(" + std::to_string(inGapFail) + ")");
    Check(outGapFail == 0, "③ 남은 둘의 차 > 오래 기다린 쪽의 허용 차(" + std::to_string(outGapFail) + ")");
    // 성질 시험이 실제로 무엇을 지나갔나 — 0 이면 「틀림 없음」이 아니라 「안 봤음」. 아래 선은 이 씨앗에서 본 값(사람 7,926 · 짝 3,013 · 남은 둘 605)의 절반쯤
    Check(queues == 2000 && people > 4000 && pairsSeen > 1500 && leftPairsSeen > 300,
          "③ 지나간 양 — 줄 " + std::to_string(queues) + " · 사람 " + std::to_string(people) + " · 짝 " + std::to_string(pairsSeen) + " · 남은 둘 " + std::to_string(leftPairsSeen));
}

}  // namespace

int main() {
    GapTests();
    PairTests();
    PropertyTests();
    std::printf("%s — 확인 %d건 · 실패 %d건\n", g_fails ? "[실패]" : "[통과]", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
