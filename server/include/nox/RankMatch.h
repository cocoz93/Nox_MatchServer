// RankMatch.h — 1:1 랭크 줄에서 점수가 가까운 둘을 묶는 판단(플랜 `Docs/MO_RANK_MATCH_PLAN_1107.md` 4절).
//
// 왜 — 먼저 온 둘을 묶으면 점수 차가 커도 붙는다. 가장 오래 기다린 사람부터, 허용 차 안에서 점수가 가장 가까운 사람과 묶는다.
//   허용 차는 100 에서 시작해 10초마다 +50 · 200 에서 멈춘다(사용자 11-07 — 「400점 차를 잡아 주는 건 좀 그렇다」) ·
//   200 안에 아무도 없으면 계속 기다린다. 점수 차 100 이면 낮은 쪽이 이길 확률 36% · 200 이면 24%(Elo 식 — 플랜 5-1)
// 짜임 — 소켓 · DB · 시계 · 상태가 없다. 줄과 지금 시각을 넣으면 짝만 돌려준다. 줄에서 빼고 넣는 일 · 수락 창 · 방 요청은 `Lobby` 몫
//   (줄을 따로 쥐는 클래스로 만들면 줄 상태가 로비와 둘로 나뉜다 — `ROOMS_DESIGN.md:7` 의 교훈)
// 시각 — 로비 시계(`LobbyConfig::nowMs` · 32비트 ms 로 돈다)라서 「줄 선 시각」끼리 크기를 견주지 않고 「지금 − 줄 선 시각」(기다린 ms)으로 견준다
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
namespace nox::wol {

struct RankWaiter {
    int conn = 0;
    int32_t rating = 0;
    uint32_t sinceMs = 0;   // 처음 줄 선 시각(되돌림 · 수락 거절로 줄에 돌아와도 그대로 — 플랜 2절)
};

// 값은 설정 `[rank] start_gap · grow_gap · grow_sec · cap_gap` 으로 바꾼다 — 1차 측정 때 더미로 다시 맞출 값(플랜 1절)
struct RankMatchRule {
    int32_t startGap = 100;         // 줄 서자마자 묶는 점수 차 — 낮은 쪽 이길 확률 36%
    int32_t growGap = 50;           // 간격마다 늘리는 차
    uint32_t growEveryMs = 10000;   // 늘리는 간격(설정은 초 · 1초 이상은 부르는 쪽이 본다)
    int32_t capGap = 200;           // 이 차를 넘겨 묶지 않는다 — 낮은 쪽 이길 확률 24%(사용자 11-07)
};

// min(시작 + 늘림 × ⌊기다림 ÷ 간격⌋, 상한). 시작이 상한보다 크면 상한 · 간격 0 이면 늘리지 않는다(설정 검사를 빠져나온 값이 0 으로 나누지 않게)
inline int32_t AllowedGap(uint32_t waitedMs, const RankMatchRule& rule) {
    int64_t gap = rule.startGap;
    if (rule.growEveryMs != 0) gap += static_cast<int64_t>(rule.growGap) * (waitedMs / rule.growEveryMs);
    return static_cast<int32_t>(std::min<int64_t>(gap, rule.capGap));
}

// 짝 = (기준 conn, 고른 conn) — 기준은 둘 중 오래 기다린 쪽. 한 사람은 많아야 한 짝 · 짝 수 × 2 + 남은 수 = 줄 수
//   1. 기다린 시간 순으로 다시 세운다(같으면 줄 차례) — 되돌림이 사람을 줄 맨 앞에 끼워 줄 차례가 시각 순서와 어긋날 수 있다
//      (예: 0초에 선 A 와 20초에 선 D 가 묶였다가 A 가 거절하면 D 가 8초에 선 C 보다 앞에 온다)
//   2. 아직 안 묶인 사람 중 가장 오래 기다린 사람이 기준 · 허용 차 = AllowedGap(그 사람이 기다린 시간)
//   3. 안 묶인 다른 사람 중 점수 차가 가장 작은 사람 — 같은 차가 여럿이면 더 오래 기다린 사람
//   4. 그 차가 허용 차 이하면 묶는다 · 넘으면 그 기준은 이번에 안 묶이고 다음 기준으로
//   못 잡힌 기준이 다른 사람을 막지 않는다 — 그와 허용 차 안에 아무도 없으니 막아도 얻는 것이 없다. 나중 기준의 허용 차는 앞 기준보다 넓지 않아
//   앞에서 못 잡힌 사람을 고르는 일이 없다 ⇒ 남은 두 사람은 언제나 둘 중 오래 기다린 쪽의 허용 차보다 멀다(시험이 아무 줄로 본다)
//   n 명에 n² 비교 — 로비 바퀴(최대 100ms 마다)에서 느리면 점수 순 배열의 양옆만 보도록 바꾼다(실측 없음 · 1차 측정 때 본다)
inline std::vector<std::pair<int, int>> PickPairs(const std::vector<RankWaiter>& queue, uint32_t nowMs, const RankMatchRule& rule) {
    struct Item {
        size_t at;           // 줄 차례
        uint32_t waited;     // 기다린 ms
    };
    std::vector<Item> order;
    order.reserve(queue.size());
    for (size_t i = 0; i < queue.size(); ++i) order.push_back({i, nowMs - queue[i].sinceMs});
    std::stable_sort(order.begin(), order.end(), [](const Item& a, const Item& b) { return a.waited > b.waited; });

    std::vector<bool> taken(order.size(), false);
    std::vector<std::pair<int, int>> pairs;
    for (size_t b = 0; b < order.size(); ++b) {
        if (taken[b]) continue;
        const int64_t baseRating = queue[order[b].at].rating;
        size_t best = order.size();
        int64_t bestGap = 0;
        for (size_t o = 0; o < order.size(); ++o) {
            if (o == b || taken[o]) continue;
            const int64_t d = queue[order[o].at].rating - baseRating;
            const int64_t gap = d < 0 ? -d : d;
            // `order` 가 기다린 순이라 앞에서 찾은 사람이 더 오래 기다렸다 — 같은 차면 바꾸지 않는다
            if (best == order.size() || gap < bestGap) {
                best = o;
                bestGap = gap;
            }
        }
        if (best == order.size() || bestGap > AllowedGap(order[b].waited, rule)) continue;
        taken[b] = taken[best] = true;
        pairs.emplace_back(queue[order[b].at].conn, queue[order[best].at].conn);
    }
    return pairs;
}

}  // namespace nox::wol
