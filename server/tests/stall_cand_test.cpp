// stall_cand_test.cpp — ★ 11-18 M-1 조각 3. 게임 스레드 멈춤 후보 두 곳에 「안 끝나는 입력」과 「끝나는 입력」을 넣어
//   한도 안에 끝나는지만 본다(근거 · 후보 판정 `MyDocs/claude-plans/nox-stall-candidates-progress.md` 2-a).
//
//   후보 ① `Server::MapTraceRay` 기울기 걸음(가파른 · 완만한 두 갈래) — 끝점 x(y) 가 −0.1 이하면 걸음이 −0.1 에 갇힌다
//          (`trunc(−0.1/23)` 이 0 칸이라 다음 걸음도 0 칸 끝 − 0.1). 시작점만 칸 밖 검사를 한다
//   후보 ② `Server::UpdateMissiles` 작살 물체 갈래 — `HarpoonCollide` 가 거짓인데 화살이 안 지워지면 `++i` 없이 같은 색인을 다시 본다.
//          주인이 나간 뒤 날던 작살이 사람에 맞는 판이 그 길이다(벽 갈래는 번호로 다시 확인해 지우므로 끝난다)
//          ✖ 조각 3 실측으로 폐기 — 다시 볼 때 움직임부터 다시 타서 화살이 한 걸음 더 나아가 몸을 벗어나므로 끝난다(`Harpoon` 끝 주석)
//
//   갈래마다 스레드 하나 — 서버도 그 스레드 안에서 만든다. 안 끝난 스레드는 밖에서 멈출 수 없으므로 떼어 두고,
//   다 본 뒤 이 프로세스를 `std::_Exit` 로 닫는다(남의 프로세스를 죽이지 않는다).
//   끝나는 갈래를 먼저 돌린다 — 갇힌 스레드가 코어를 먹기 전에 끝나는 쪽 시간을 잰다.
//
//   ⚠ 「짐작」 칸은 조각 2 판정이 내다본 값이다(작살 갈래만 조각 3 실측으로 뒤집음). 고친 뒤에는 안 끝나는 갈래의 짐작을 「끝남」으로 뒤집는다.
//     그래서 ctest 에 안 건다(지금 짐작대로면 갇힌 스레드 셋이 프로세스가 닫힐 때까지 코어를 하나씩 쓴다)
//
//   돌려주는 값 = 짐작과 다른 갈래 수(0 = 다 짐작대로).
//   인자: --limit-ms N   한도(기본 3000) — 끝나는 갈래는 수 ms 라 PC 가 바빠도 한도에 안 닿는다
//         --only 이름    그 갈래 하나만
//         --hold-ms N    다 본 뒤 닫기 전에 기다림 — 바깥 호출 사슬 도구(nox_stack --pid)로 갇힌 스레드를 찍을 때
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "nox/Server.h"
#include "nox/SyncHost.h"

namespace {

// 후보 ① — 선 추적 한 번. 벽이 없는 빈 방이라 칸마다 「안 막힘」이고, 걸음이 끝점을 넘어야 빠진다
std::string Ray(float x1, float y1, float x2, float y2) {
    nox::CSyncHost srv;
    (void)srv->DebugTraceRay(x1, y1, x2, y2);
    return std::string();
}

// 후보 ② — `proto_test.cpp` `TestHarpoon` 과 같은 차림: 워리어 a(1000,1000)가 +x 쪽 위저드 b(1100,1000)를 겨눠 작살을 쏜다.
//   kick = 쏜 직후 a 를 내보냄
enum class Setup { HitB, Wall, Away };   // b 에 맞음 · b 를 치우고 앞 칸에 벽 · b 를 치움(아무것도 안 맞음 — 걸음 기준선)

struct Flight {
    std::string fail;                    // 전제가 안 섰으면 그 사정(빈 글 = 전제 섬)
    std::vector<float> adv;              // 틱마다 화살(하나뿐 — 색인 0)이 x 로 나아간 만큼(앞뒤 어느 쪽이든 화살이 없으면 0)
    int hitTick = -1;                    // 주인없음 닿음이 늘어난 첫 틱
    unsigned long long noOwner = 0, attached = 0;
    size_t left = 0;                     // 30틱 뒤 남은 화살 수
};

Flight Fly(bool kick, Setup setup) {
    Flight f;
    const std::string a = "10.11.18.1:1181";
    const std::string b = "10.11.18.2:1182";
    nox::CSyncHost srv;
    const int64_t sa = srv.DebugEnterGame(a, 728, false, nox::CharClass::Warrior);
    const int64_t sb = srv.DebugEnterGame(b, 729, false, nox::CharClass::Wizard);
    srv->DebugMoveTo(sa, 1000, 1000);
    srv->DebugMoveTo(sb, 1100, 1000);
    srv->SetProjType("HarpoonBolt", 526, 800, 0x4000, 0);   // `proto_test.cpp` RegHarpoonBolt 그대로(thing.bin 실측 종류 526 · SPEED 800)
    std::vector<nox::OutPacket> out;
    srv.Tick(0, out); srv.Tick(34, out);
    srv->DebugAbilAward(sa, 3, false); srv.TakeOut(out);
    srv->DebugSetFacing(sa, 0);                              // +x 쪽
    if (setup != Setup::HitB) srv->DebugMoveTo(sb, 5000, 5000);                // 상대를 치운다
    if (setup == Setup::Wall) srv->SetWalls({ { uint8_t{ 47 }, uint8_t{ 43 } } });   // 앞 +x 쪽 칸(`TestHarpoon` ⑥ 그대로)
    if (!srv->DebugAbilDo(sa, 3)) { f.fail = "전제 — 작살 쓰기가 실패했다"; return f; }
    srv.TakeOut(out);
    if (srv->missiles() != 1 || srv->DebugHarpoonBolt(sa) == 0) { f.fail = "전제 — 화살이 하나 안 생겼다"; return f; }
    if (kick) {
        srv->DebugKickPlayer(sa); srv.TakeOut(out);
        if (srv->DebugHarpoonBolt(sa) != 0 || srv->missiles() != 1) { f.fail = "전제 — 내보냈는데 주인이 남았거나 화살이 사라졌다"; return f; }
    }
    uint32_t ms = 68;
    for (int t = 0; t < 30; ++t) {
        const bool had = srv->missiles() == 1;
        const float x0 = had ? srv->DebugMissileX(0) : 0.0f;
        const unsigned long long n0 = srv->harpoonTouch(nox::HarpoonTouch::NoOwner);
        ms += 34; srv.Tick(ms, out);                         // ← 조각 2 짐작대로면 kick · b 에 맞음 판은 여기서 안 돌아온다
        f.adv.push_back((had && srv->missiles() == 1) ? srv->DebugMissileX(0) - x0 : 0.0f);
        if (f.hitTick < 0 && srv->harpoonTouch(nox::HarpoonTouch::NoOwner) > n0) f.hitTick = t;
    }
    f.noOwner = srv->harpoonTouch(nox::HarpoonTouch::NoOwner);
    f.attached = srv->harpoonTouch(nox::HarpoonTouch::Attached);
    f.left = srv->missiles();
    return f;
}

//   돌려주는 글 = 전제가 안 섰거나 끝났는데 그 갈래의 까닭이 아닐 때 그 사정(빈 글 = 까닭대로)
std::string Harpoon(bool kick, Setup setup) {
    const Flight f = Fly(kick, setup);
    if (!f.fail.empty()) return f.fail;
    char why[260];
    if (!kick) {
        // 꽂힌 뒤 관문(다 끌어옴 등)으로 30틱 안에 끊길 수 있어 「지금 꽂혀 있나」 대신 「한 번 꽂혔나」를 본다
        if (f.attached == 1 && f.noOwner == 0) return std::string();
        std::snprintf(why, sizeof(why), "주인이 있는데 b 에 한 번 안 꽂혔다(걸림 %llu · 주인없음 %llu)", f.attached, f.noOwner);
        return why;
    }
    if (setup == Setup::Wall) {
        if (f.noOwner == 1 && f.left == 0) return std::string();
        std::snprintf(why, sizeof(why), "벽 갈래 — 주인없음 닿음 %llu(1 이어야) · 남은 화살 %zu(0 이어야)", f.noOwner, f.left);
        return why;
    }
    // ★ 조각 3 실측 — 끝난다. 같은 색인을 다시 볼 때 움직임(`Server.cpp` ③ `posX = newX` · `newX += vel`)부터 다시 타서
    //   화살이 한 걸음 더 나아가 몸 밖으로 나간다 → 맞은 틱에 아무것도 안 맞는 판(기준선)보다 한 걸음 더 간다.
    //   그 한 걸음이 보여야 이 까닭으로 끝난 것이다(주인없음 닿음이 1 = 두 번째 볼 때는 몸 밖)
    const Flight base = Fly(true, Setup::Away);
    if (!base.fail.empty()) return "기준선 " + base.fail;
    const int h = f.hitTick;
    const float extra = (h >= 0) ? f.adv[static_cast<size_t>(h)] - base.adv[static_cast<size_t>(h)] : 0.0f;
    const float step = (h >= 0 && h + 1 < 30) ? base.adv[static_cast<size_t>(h + 1)] : 0.0f;   // 기준선의 다음 틱 걸음
    const bool oneMore = step > 0.0f && extra > 0.5f * step;
    if (f.noOwner == 1 && f.left == 1 && base.noOwner == 0 && oneMore) return std::string();
    std::snprintf(why, sizeof(why), "주인없음 닿음 %llu(1) · 남은 화살 %zu(1) · 기준선 닿음 %llu(0) · 맞은 틱 %d 에 기준선보다 %.1f 더 감(걸음 %.1f 의 반 넘어야)",
                  f.noOwner, f.left, base.noOwner, h, extra, step);
    return why;
}

struct Case {
    const char* name;
    bool guessEnds;                       // 조각 2 판정이 내다본 값(harpoon_kick_hit 만 조각 3 실측)
    std::function<std::string()> run;
};

// 스레드가 갇히면 이 칸을 끝까지 쓰므로 지우지 않는다(프로세스가 닫힐 때 같이 사라진다)
struct Slot {
    std::atomic<bool> done{ false };
    std::string note;
};

}  // namespace

int main(int argc, char** argv) {
    long limitMs = 3000, holdMs = 0;
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--limit-ms") && i + 1 < argc) limitMs = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--hold-ms") && i + 1 < argc) holdMs = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else { std::printf("모르는 인자 '%s'\n", argv[i]); return 2; }
    }

    // 끝나는 갈래 먼저. 시작점은 둘 다 1번 칸(x 또는 y = 30) — 0번 칸에 들어선 다음 걸음이 −0.1 이 된다
    const std::vector<Case> cases = {
        { "ray_steep_end",        true,  [] { return Ray(30, 100, 0.5f, 300); } },    // 끝점 x 0.5  — −0.1 에서 빠짐
        { "ray_steep_edge_end",   true,  [] { return Ray(30, 100, -0.05f, 300); } },  // 끝점 x −0.05 — −0.1 < −0.05 라 빠짐(경계 안쪽)
        { "ray_gentle_end",       true,  [] { return Ray(100, 30, 300, 0.5f); } },    // 완만한 갈래(y 로 걸음) 끝점 y 0.5
        { "harpoon_owner_attach", true,  [] { return Harpoon(false, Setup::HitB); } }, // 주인 그대로 — b 에 꽂힘
        { "harpoon_kick_wall",    true,  [] { return Harpoon(true, Setup::Wall); } },  // 주인 나감 · 벽에 닿음 — 벽 갈래가 번호로 지움
        // 주인 나감 · b 에 닿음 — ★ 조각 2 짐작은 「안 끝남」이었으나 조각 3 실측(11-18)으로 뒤집음. 끝나는 까닭은 `Harpoon` 끝 주석
        { "harpoon_kick_hit",     true,  [] { return Harpoon(true, Setup::HitB); } },
        { "ray_steep_edge_hang",  false, [] { return Ray(30, 100, -0.1f, 300); } },   // 끝점 x −0.1 — 경계 그 값(−0.09 로 바꾸면 끝남 — 조각 3 일부러 틀리기)
        { "ray_steep_hang",       false, [] { return Ray(30, 100, -5, 300); } },      // 끝점 x −5
        { "ray_gentle_hang",      false, [] { return Ray(100, 30, 300, -5); } },      // 끝점 y −5
    };

    int differ = 0, ran = 0;
    for (const Case& c : cases) {
        if (only && std::strcmp(only, c.name) != 0) continue;
        ++ran;
        Slot* slot = new Slot;
        const auto t0 = std::chrono::steady_clock::now();
        auto fn = c.run;
        std::thread th([slot, fn] {
            slot->note = fn();
            slot->done.store(true, std::memory_order_release);
        });
        auto elapsed = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        };
        while (!slot->done.load(std::memory_order_acquire) && elapsed() < limitMs)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool ended = slot->done.load(std::memory_order_acquire);
        const long long ms = elapsed();
        if (ended) th.join(); else th.detach();
        const bool asGuessed = (ended == c.guessEnds) && (!ended || slot->note.empty());
        if (!asGuessed) ++differ;
        std::printf("[%s] %-22s %s %lldms · 짐작 %s%s%s\n", asGuessed ? "짐작대로" : "짐작과 다름", c.name,
                    ended ? "끝남" : "안 끝남(한도)", ms, c.guessEnds ? "끝남" : "안 끝남",
                    (ended && !slot->note.empty()) ? " · " : "", ended ? slot->note.c_str() : "");
        std::fflush(stdout);
    }
    if (only && ran == 0) { std::printf("그런 갈래가 없다 '%s'\n", only); return 2; }
    std::printf("\n갈래 %d · 짐작과 다름 %d · 한도 %ldms\n", ran, differ, limitMs);
    std::fflush(stdout);
    if (holdMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    std::_Exit(differ);
}
