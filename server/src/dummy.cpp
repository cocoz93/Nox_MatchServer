// dummy.cpp — 부하 실험 더미. 정품 클라 대신 서버에 붙어 판을 치른다(정본 Docs/MO_DUMMY_PLAN_1109.md).
//
// 왜 패킷으로 만드나 — 재려는 것이 「서버가 소켓으로 받고 보내는 몫」이라서다. 서버 안 봇은 소켓을 안 지나가 그 몫이 안 잡힌다.
// 구조 — 스레드 하나가 더미 전부를 돈다. 더미마다 UDP 소켓 하나. 자기 차례가 되면 받은 것 다 읽기 → 메시지 나누기 → 행동 → 보내기.
//
// 정품 클라가 사람 조작 없이도 보내는 넷(Docs/MO_CLIENT_SELF_SEND_1109.md) — 입력 3f · 핑 06 · 중요 확인 ab · 별칭 a5.
// 하나라도 빠지면 서버가 실물과 다른 길을 탄다(ab 없으면 다시 싣기 · a5 없으면 전체 번호로 계속 소개).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nox/Alias.h"
#include "nox/GameBalance.h"
#include "nox/MapFile.h"
#include "nox/Opcode.h"
#include "nox/Server.h"
#include "nox/ThingDb.h"
#include "nox/ThingFlags.h"
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace {

using nox::Op;

constexpr int64_t kWheelUs = 33000;      // 한 바퀴 33ms — 서버 틱과 같고 정품 입력(초당 29~32번) 안에 든다
constexpr int kWheelSlots = 33;          // 한 바퀴를 1ms 칸으로 나눈다
// 33ms 를 이만큼 넘긴 것까지는 밀린 것으로 안 본다 — 마지막 칸(32ms)에 든 더미는 여유가 1ms 뿐이라 타이머가 조금 늦게 깨우기만 해도 33ms 를 넘긴다.
// 값은 칸 하나의 두 배 · 11-10 실물(더미 100명 · 300초) — 33ms 를 넘긴 200번 중 187번이 1ms 이하, 일한 시간은 바퀴당 평균 2.56ms
constexpr int64_t kLateToleranceUs = 2000;
constexpr int64_t kPingUs = 2000000;     // 정품 핑 간격 2초(R1 7바이트 81번 · Docs/MO_CLIENT_SELF_SEND_1109.md)
constexpr int64_t kJoinRetryUs = 1000000;   // 접속 시도 · 연결을 다시 보내는 간격(UDP 라 한 번이 사라질 수 있다)
// ★ 11-11 부하 측정 M0 조각 6 — 끝날 때 `0a` 를 보낸 뒤 받기만 하는 시간. 서버가 `0a` 를 처리해 그 세션에 보내기를 멈추기 전에 이미 보낸
//   조각까지 세려고. 한 틱(33ms) + 넘김 지연에, 한계 근처 실행에서 틱 완료가 수백 ms 밀려도 남는 값(내가 정함 · 실측 아님)
constexpr int64_t kCloseDrainUs = 1000000;
// ★ 11-11 M0 조각 8 — 방 여닫기(--cycle-sec)에서 `0a` 를 보낸 뒤 다시 들어오기 시작할 때까지. 서버는 빈 부하 방을 2초 뒤 닫는다
//   (부하 방 틀도 로비 방과 같은 `ApplyLobbyRoomFrame` 이라 `closeWhenEmpty` · `RoomManager.h` `kEmptyUsedRoomCloseMs`). 그 전에 들어오면
//   방 수가 상한이라 거절된다 — 거절돼도 1초마다 다시 시도해 스스로 풀리지만, 2초에 서버가 밀릴 몫을 더한 값(내가 정함 · 실측 아님)
constexpr int64_t kCycleRejoinUs = 5000000;
constexpr size_t kMaxOutBody = 1000;     // 한 조각에 싣는 본문 상한 — 서버 받기 버퍼 2048(NetworkThread.cpp:219) 안
// 싸움 값 — 프레임은 서버 프레임(33ms). 근거는 Docs/MO_DUMMY_PLAN_1109.md 2-4 · 2-5절.
constexpr double kMeleeReach = 50.0;              // 이 안이면 때린다 — 닿는 거리 55(몸 10 + 롱소드 45 · Server.cpp:17157)보다 조금 안쪽
constexpr uint32_t kAttackGapFrames = 30;         // 칼은 1초에 한 번(사용자와 정한 빈도)
constexpr uint32_t kDeathSkipFrames = 90;         // 죽은 상대를 대상에서 빼는 시간 — 갱신에 시체도 계속 실려서 안 빼면 시체를 쫓는다
constexpr uint32_t kReviveFirstFrames = 50;       // 서버가 클릭을 받는 때: 죽음 30틱 + 15틱 뒤(Server.cpp:5561-5569)
constexpr uint32_t kReviveGapFrames = 30;         // 코드 6 을 한 번만 보내면 놓칠 수 있어 되풀이한다
constexpr double kCursorAhead = 150.0;            // 커서가 몸에서 100 을 넘으면 달린다(Server.h kRunCursorDist)
constexpr uint32_t kStuckCheckFrames = 90;        // 3초마다 몸이 움직였나 본다
constexpr double kStuckMinGain = 20.0;            // 3초에 몸이 이만큼도 못 움직이면 막힌 것으로 본다(길 따라 돌아갈 때는 상대와 거리가 늘 수 있어 몸 움직임으로 본다)
constexpr uint32_t kDetourMaxFrames = 150;        // 지점으로 가는 시간의 상한 — 지점 앞에서도 막힐 수 있다
constexpr double kDetourArrive = 40.0;            // 지점에 이만큼 다가가면 다시 쫓는다
constexpr uint32_t kNoHurtFrames = 200;           // 거리 안인데 이만큼 상대 체력이 안 줄면 벽 너머로 본다 — 부활 무적 150틱(Server.cpp:6416)보다 길게
// ★ 11-12 판 멈춤 조사 — 체력이 줄었다는 42 도 죽음도 없이 이어진 우회가 5번 · 20번에 닿은 횟수를 센다(`detourRun5` · `detourRun20`).
//   멈춘 판은 150판에 1번꼴(×3 약 300판 중 2판)이라 판 수로는 고친 효과를 못 가린다 — 멈춤 전 단계를 센다.
//   우회 한 번 = 막힘 확인 3초(kStuckCheckFrames) + 지점 가기 최대 5초(kDetourMaxFrames).
//   5번 = 15~40초 싸움 없이 쫓기만 함 · 20번 = 1~2.7분 — 킬로 끝난 판은 10킬에 가운데 4분이라 이만큼 싸움이 끊기면 멈춤이다
constexpr int kDetourRunWarn = 5, kDetourRunStall = 20;
constexpr int kDetourRunShowMax = 200;            // 5 · 20번째 자리 줄의 상한 — 멈춘 더미가 많아도 줄이 끝없이 늘지 않게
// ★ 11-12 — 우회 없이 멈추는 꼴도 센다(`quiet60`). 고친 뒤 회귀 런 641초 판(`_out/obst/fix1` 방 2)은 5분 동안 칼질 0 · 서버 「사람 벽 짝」이
//   요약 줄 사이마다 약 230(앞뒤는 0~28)인데 그 런의 detourRun5 는 0 이었다(막힘 확인에 안 걸린 것으로 보임 · 까닭 미확인)
//   — 세는 자리가 우회 하나뿐이면 그 0 은 「멈춤 없음」이 아니다.
//   60초 = 킬로 끝난 판의 킬 사이(가운데 4분 ÷ 10킬 ≈ 24초)의 두 배 넘게 남이 맞은 42 도 죽음도 없음
constexpr uint32_t kQuietFrames = 1800;
constexpr int kQuietShowMax = 300;

// 전사 능력 — 플랜 2-5절. 재사용은 서버 값(GameBalance.h kAbilityDelay) · 거리는 잠정(플랜 「돌려 보고 D6 에서 정한다」).
struct AbilityInfo { uint8_t id; uint32_t delay; double minDist, maxDist; };
constexpr AbilityInfo kAbilities[3] = {{1, 300, 100.0, 300.0}, {2, 300, 0.0, 150.0}, {3, 150, 80.0, 250.0}};   // 광전사 돌진 · 함성 · 작살
constexpr uint32_t kAbilityGapFrames = 30;        // 한 능력을 보낸 뒤 다른 능력을 안 보내는 시간(플랜 2-5절)
constexpr uint32_t kAbilityMargin = 5;            // 재사용 끝을 서버보다 먼저 보지 않게 — 더미 프레임은 받은 것 기준이라 서버보다 늦고, 재사용 막힘은 0 이어야 한다

// 법사 값 — 플랜 2-6절. 주문 번호 · 깃발 · 마나는 플랜 표(원본 봇 설정 · nox_datadump --spells).
struct SpellInfo { uint32_t id; uint8_t selfFlag; int mana; };
constexpr SpellInfo kProtect{51, 1, 80}, kHaste{36, 1, 10}, kInvis{39, 1, 30};   // 자기 강화 — 셋 중 하나도 안 걸렸을 때만 하나
constexpr SpellInfo kConfuse{12, 0, 10};
constexpr SpellInfo kFireball{27, 0, 30}, kDeathRay{16, 0, 60};
constexpr SpellInfo kChain{43, 0, 30};            // 시작 값은 0 이고 30프레임 동안 1씩 최대 30 — 끝까지 쓸 만큼 있을 때만 보낸다
constexpr SpellInfo kBlink{4, 1, 10}, kHeal{41, 1, 30};
constexpr const SpellInfo* kBuffs[3] = {&kProtect, &kHaste, &kInvis};
constexpr uint32_t kBuffBitProtect = 1u << 26, kBuffBitHaste = 1u << 9, kBuffBitInvis = 1u << 0;   // 0x5A 비트(Server.cpp:7053 · :6967 · :6969)
constexpr uint32_t kSelfBuffMask = kBuffBitProtect | kBuffBitHaste | kBuffBitInvis;
constexpr uint32_t kSpellGapFrames = 60;          // 어떤 주문이든 2초에 하나(플랜 1-1절)
constexpr uint32_t kCastHoldFrames = 25;          // 음소가 끝날 때까지 커서를 상대 위에 두고 안 달린다 — 가장 긴 보호막이 24틱
constexpr uint32_t kChainHoldFrames = 60;         // 연쇄 번개는 서버가 30프레임 뒤 스스로 끝내는데(Server.cpp:7017) 달리면 끊긴다(:7757)
constexpr double kSightRange = 150.0;             // 이 안의 상대만 주문 대상(원본 기준 GAME4.c:1960)
constexpr double kBackOffIn = 50.0, kBackOffTo = 80.0;   // 상대가 50 안이면 80 까지 물러난다
constexpr double kBlinkIn = 25.0;                 // 25 안이면 점멸(원본 기준 GAME4.c:2020 의 50 × 0.5 · GAME5.c:1373)
constexpr int kRetreatPercent = 25, kRecoverPercent = 80;   // 후퇴 시작 · 복귀(원본 기준 GAME4.c:1949 · :1951)
constexpr uint32_t kRetreatMinFrames = 120, kRetreatMaxFrames = 180;   // 4~6초 도망(원본 기준 GAME5.c:263)
constexpr uint32_t kConfuseMinFrames = 90, kConfuseMaxFrames = 900;   // 혼란은 3~30초 난수로 쉰다(원본 기준 GAME4.c:2033-2034)
constexpr uint32_t kBlinkRestMaxFrames = 180;     // 점멸은 0~6초 난수로 쉰다(원본 기준 GAME4.c:2049-2052)

// 컨저러 값 — 플랜 2-7절. 쓰는 주문은 원본 컨저러 봇 설정(원본 기준 GAME4.c:1990-2016)에서 엠버 · 체럽 · 오거 소환과 반사를 뺀 것이다.
// 원본의 쉬는 시간은 주문이 아니라 분류마다 한 칸이다(GAME4.c:1998-2005) — 그래서 방해 둘(독 · 스턴)은 쉬는 시간을 같이 쓴다.
constexpr SpellInfo kPixie{58, 1, 30};            // 자기 강화 — 0~6초 난수로 쉰다
constexpr SpellInfo kPoison{60, 0, 10}, kStun{74, 0, 10};   // 방해 — 3~30초 난수로 쉬고, 둘 중 하나를 고른다
constexpr SpellInfo kFist{29, 0, 60}, kMeteor{52, 0, 30};   // 공격 — 부활마다 반반(가정 — 원본은 분류 안에서 난수로 고른다)
constexpr uint32_t kPixieRestMaxFrames = 180;
// 소환수 세 크기 — 컨저러 입장 순서대로 돌린다. 크기 합 상한은 서버의 kSummonLimit(4). 크기 값은 thing.bin SUBCLASS, 부르는 시간은 SummonDuration(플랜 2-7절 표).
struct SummonKind { SpellInfo spell; uint16_t type; int size; uint32_t frames; };
constexpr SummonKind kSummonKinds[3] = {
    {{82, 0, 15}, 1356, 1, 60},    // SmallAlbinoSpider
    {{77, 0, 60}, 1365, 2, 210},   // Bear
    {{78, 0, 60}, 1382, 4, 375},   // Beholder
};
constexpr int kSummonSizeLimit = 4;
constexpr uint32_t kSummonMarginFrames = 30;      // 음소(가장 긴 것 약 24틱)가 끝나야 부르는 시간이 시작되므로 그 몫에 여유를 조금 더한 값 — 앞 소환이 끝나기 전에 다음을 안 보내려고

// Estate 의 PlayerStart 14곳(서버 「[부활 자리]」 줄) — 맵이 보증하는 설 수 있는 자리라 막혔을 때 들를 곳으로 쓴다.
struct Point { int x, y; };
constexpr Point kWaypoints[] = {
    {3865, 2534}, {3174, 2468}, {2600, 2578}, {3292, 3410}, {2869, 2923}, {3046, 1705}, {2653, 2409},
    {3234, 2025}, {3572, 3043}, {2406, 3139}, {2304, 3499}, {2627, 3831}, {2782, 3505}, {3307, 2804},
};
constexpr int kWaypointCount = static_cast<int>(sizeof(kWaypoints) / sizeof(kWaypoints[0]));

// 길 찾기 값 — 벽 격자 한 칸은 23 유닛, 256칸(MapFile.h ReadWalls).
constexpr int kNavSize = 256;
constexpr int kNavCell = 23;
constexpr uint32_t kReplanFrames = 30;            // 1초에 한 번 길을 다시 찾는다 — 상대가 움직이고, 더미가 많아도 한 프레임에 몰리지 않게
constexpr int kLookAheadCells = 6;                // 길 위에서 곧장 갈 수 있는 먼 점을 고를 때 앞을 볼 칸 수
constexpr double kPathStepArrive = 23.0;          // 길의 점에 한 칸만큼 다가가면 다음 점으로 — 단 다음 점까지 몸 폭으로 트였을 때만(PickPathPoint)
constexpr double kPathPointReached = 8.0;         // 점에 이만큼 붙으면 몸 폭을 안 보고 넘긴다 — 칸 가운데끼리 잇는 줄은 벽 선과 11.5 이상 떨어져 있고, 붙고도 못 넘기면 그 점에 선 채 멈춘다
constexpr const char* kDefaultMapFile = "C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\Estate\\Estate.map";
constexpr const char* kDefaultThingFile = "C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin";   // 서버 --game-dir 기본과 같은 폴더(WorldBuild.h:170)

constexpr int64_t kStartRetryUs = 5000000;   // 대기 맵에 그대로 있으면 !start 를 다시 보내는 간격 — 채팅은 비신뢰라 방장 것이 사라지면 그 방이 영영 안 열린다

#define NOX_DUMMY_COUNTERS(X) \
    X(sentInput) X(sentPing) X(sentAck) X(sentAlias) X(sentMouse) X(sentSpell) X(sentAbility) X(sentStart) \
    X(recvDgram) X(recvBytes) X(decodeFail) X(dupImportant) X(lateTicks) X(sendFail) X(sentClose) X(closeSkipped) X(cycleLeave) X(cycleSkipped) \
    X(petsAcquired) X(petsLost) X(spellSkippedNoMana) X(retreats) X(heals) \
    X(sentAttack) X(sentRevive) X(sawDeath) X(selfDeath) X(revives) X(detours) X(detourRun5) X(detourRun20) X(quiet60) \
    X(castProtect) X(castHaste) X(castInvis) X(castConfuse) X(castFireball) X(castDeathRay) X(castChain) X(castBlink) \
    X(sentBerserk) X(sentRoar) X(sentHarpoon) X(firstBuffOk) X(firstBuffBad) X(buffOverlap) X(healedBack) X(statChecked) X(statMismatch) \
    X(castPixie) X(castPoison) X(castStun) X(castFist) X(castMeteor) X(castSummon) X(petTypeMismatch)

struct Counters {
#define X(n) uint64_t n = 0;
    NOX_DUMMY_COUNTERS(X)
#undef X
};

// ★ 11-11 부하 측정 M0 조각 7 — 더미를 스레드 여럿으로 돌린다(`--threads`). 셈은 스레드마다 따로 세고, 주 스레드가 1초마다 읽어 더한다.
//   한 칸을 쓰는 스레드는 하나뿐이라 읽고 · 더하고 · 쓰기를 relaxed 로 나눠도 잃는 몫이 없다(x64 에선 잠금 없는 mov · add · mov).
//   주 스레드는 도는 중에 읽으니 1초 줄은 그 순간의 값이고, [더미 요약]은 스레드를 다 기다린 뒤라 정확하다.
struct RelaxedCount {
    std::atomic<uint64_t> v{0};
    RelaxedCount& operator++() { v.store(v.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); return *this; }
    RelaxedCount& operator+=(uint64_t n) { v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); return *this; }
    RelaxedCount& operator--() { v.store(v.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed); return *this; }   // entered 만 쓴다(방 여닫기로 나갈 때)
    uint64_t Get() const { return v.load(std::memory_order_relaxed); }
};

// 스레드마다 한 벌 — 64바이트로 맞춰 옆 스레드 몫과 같은 캐시 줄에 안 놓이게 한다.
struct alignas(64) LiveCounters {
#define X(n) RelaxedCount n;
    NOX_DUMMY_COUNTERS(X)
#undef X
    RelaxedCount entered;                // 판에 든 더미 수 — 1초 줄의 「입장」. 주 스레드가 남의 스레드 더미의 phase 를 읽지 않게 따로 센다(방 여닫기로 나가면 뺀다)
    void AddTo(Counters& c) const {
#define X(n) c.n += n.Get();
        NOX_DUMMY_COUNTERS(X)
#undef X
    }
};

thread_local LiveCounters* t_count = nullptr;   // 더미 스레드가 시작할 때 제 몫을 건다(RunWorker)
std::atomic<int> g_decodeFailShown{0};
std::atomic<int> g_detourRunShown{0};             // [더미 우회 연속] 줄 수(kDetourRunShowMax)
std::atomic<int> g_quietShown{0};                 // [더미 싸움 끊김] 줄 수(kQuietShowMax)

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

enum class Behavior { Idle, Move, Fight };

struct Args {
    std::string host = "127.0.0.1";
    uint16_t port = 18590;
    int count = 1;
    int roomSize = 1;
    int reserveSeats = 0;                // 방마다 정품 클라 몫으로 비우는 자리 — 화면 확인 때만(더미 수 = (K − R) × 방 수)
    std::vector<int> classes{0};
    int durationSec = 150;
    int joinIntervalMs = 200;
    int startWaitMs = 3000;              // 방이 찬 것을 본 뒤 !start 까지 — 마지막 사람의 대기 맵 준비를 기다린다
    uint32_t seed = 1;
    int statMult = 1;                    // 서버에 준 --stat-mult 와 같은 값 — 후퇴 기준인 최대 체력을 안다(플랜 2-6절)
    double freqMult = 1.0;               // 빈도 배수 — 측정의 절반 · 2배(플랜 1-1절). 2 면 주문 · 칼 간격이 절반
    uint32_t SpellGap() const { return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kSpellGapFrames / freqMult))); }
    uint32_t AttackGap() const { return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kAttackGapFrames / freqMult))); }
    Behavior behavior = Behavior::Fight;   // 빠뜨렸을 때 조용히 가벼운 부하를 재지 않게 기본을 fight 로 둔다
    std::string mapFile = kDefaultMapFile;   // 벽을 돌아가려고 읽는 대전 맵
    std::string thingFile = kDefaultThingFile;   // 맵 물체의 모양 · 깃발(BakeNavObstacles)
    bool navObjects = true;                  // false(--no-nav-objects) = 맵 물체를 안 찍는 대조군 — 11-12 이전 지도(벽만)
    // 더미를 돌리는 스레드 수(M0 조각 7) — 더미 i 는 스레드 i % N 이 맡는다. 측정은 4(측정 플랜 4절: 전투 40명 한 바퀴 최대 2.94ms 라 1스레드는 약 450명부터 33ms 에 닿는다)
    int threads = 1;
    // 방 여닫기(측정 플랜 M7 · M0 조각 8) — 마지막 방 몫 더미가 이 초마다 `0a` 로 나가고 kCycleRejoinUs 뒤 다시 들어온다(0 = 끔).
    // 첫 나감은 마지막 더미가 입장을 시작한 때 + 이 초. 다시 들어온 방은 새 방 번호다(서버가 빈 방을 닫고 새로 만든다)
    int cycleSec = 0;
};

// Leaving = 방 여닫기(--cycle-sec)로 `0a` 를 보낸 뒤 받기만 하는 동안(M0 조각 8) — 끝나면 WaitTurn 으로 돌아가 다시 들어온다
enum class Phase { WaitTurn, TryJoin, Connect, Game, Leaving };

struct OtherPlayer {
    uint16_t net = 0;
    int x = 0, y = 0;
};

// 64바이트로 맞춘다(M0 조각 7) — 이웃한 더미 i · i+1 은 다른 스레드가 맡아서, 한 캐시 줄에 걸치면 서로 쓸 때마다 줄을 빼앗는다.
struct alignas(64) Dummy {
    int index = 0;
    int cls = 0;
    SOCKET sock = INVALID_SOCKET;
    Phase phase = Phase::WaitTurn;
    int64_t joinAtUs = 0;
    int64_t retryAtUs = 0;
    int64_t pingAtUs = 0;
    uint8_t key = 0;
    // ★ 머리 둘째 바이트는 「서버 신뢰 조각 가운데 다음에 받을 순번」이다 — 받은 순번이 아니라.
    //   받은 순번을 그대로 실으면 서버가 「그것부터 못 받았다」로 읽어 마지막 조각을 다시 보낸다(UdpServer.cpp:216-224 · :580-590).
    //   정품 녹음도 신뢰 0 · 1 · 2 를 받은 뒤 3 을 싣는다.
    uint8_t relNext = 0;
    uint32_t frame = 0;
    nox::AliasTable aliases;
    uint16_t selfNet = 0;
    int x = 0, y = 0;
    std::string mapName;                 // USE_MAP 이 알려 준 지금 맵
    std::string waitMapName;             // 처음 받은 맵 = 대기 맵
    int visibleOthers = 0;               // 마지막 갱신에 보인 다른 사람 수(판 중 입장 거절이라 방이 찬 뒤에 연다)
    int64_t fullSinceUs = 0;             // 방이 찬 것을 처음 본 때(0 = 아직)
    int64_t startAtUs = 0;               // 다음 !start 를 보낼 때
    uint8_t facing = 0;
    std::vector<OtherPlayer> others;     // 마지막 갱신의 다른 사람들(서버가 거리와 무관하게 싣는다 — Server.cpp:18697)
    std::vector<std::pair<uint16_t, uint32_t>> skip;   // 죽어서 대상에서 뺀 번호 · 풀리는 프레임
    bool dead = false;
    uint32_t reviveClickFrame = 0;       // 다음 부활 클릭을 보낼 프레임
    uint32_t attackReadyFrame = 0;       // 다음 칼질을 보낼 프레임
    bool wantMove = false;               // 이번 바퀴 입력에 실을 앞으로(코드 2)
    bool wantClick = false;              // 이번 바퀴 입력에 실을 클릭(코드 6)
    uint32_t stuckCheckFrame = 0;        // 다음 막힘 확인 프레임
    bool stuckHave = false;              // 지난 확인의 자리가 있나
    int stuckX = 0, stuckY = 0;          // 지난 확인의 자리
    std::vector<Point> path;             // 벽을 돌아가는 길(세상 좌표)
    int pathPos = 0;                     // 길에서 지금 향하는 점
    uint32_t pathReplanFrame = 0;        // 다음에 길을 다시 찾을 프레임
    int detourIndex = -1;                // 들르는 지점(-1 = 안 들름)
    int lastDetour = -1;                 // 바로 전에 들른 지점 — 같은 곳을 되풀이하지 않게
    int detourRun = 0;                   // 42 · 죽음 · 맵 옮김 없이 이어진 우회 수(kDetourRunWarn)
    uint32_t fightFrame = 0;             // 마지막으로 42 · 죽음 · 맵 옮김을 본 프레임(kQuietFrames)
    uint32_t quietNextFrame = 0;         // 다음 「싸움 끊김」을 셀 프레임
    uint32_t detourEndFrame = 0;
    uint16_t reachTarget = 0;            // 거리 안에 붙어 있는 상대(0 = 없음)
    uint32_t reachSinceFrame = 0;        // 붙은 때 · 그 상대 체력이 마지막으로 준 때
    uint16_t hurtNet = 0;                // 42 로 체력이 줄었다고 알려 온 번호
    uint32_t hurtFrame = 0;
    std::mt19937 rng;                    // 더미마다 씨앗이 달라 행동이 다르고, 실행끼리는 같다
    int hp = 0, mana = 0;                // 43 · 45 가 알려 준 지금 값
    int maxHp = 0, maxMana = 0;          // HealthOnline · ManaOnline(서버와 같은 --stat-mult)
    bool checkedHp43 = false, checkedDd = false, checkedDe = false;   // 입장 값을 표와 견준 것은 한 번만
    uint32_t selfBits = 0;               // 자기 0x5A 비트
    std::vector<std::pair<uint16_t, uint32_t>> enchants;   // 다른 사람의 0x5A 비트(투명 확인)
    uint32_t spellReadyFrame = 0;        // 다음 주문을 보낼 수 있는 프레임
    uint32_t castHoldEndFrame = 0;       // 음소가 끝날 때까지 커서를 상대에 두는 프레임
    uint32_t confuseReadyFrame = 0;
    uint32_t blinkReadyFrame = 0;
    uint32_t pixieReadyFrame = 0;
    int summonKind = 0;                  // 이 컨저러가 부르는 크기(kSummonKinds 번호)
    uint32_t summonEndFrame = 0;         // 이 프레임까지는 다음 소환을 안 보낸다
    std::vector<uint16_t> pets;          // 지금 거느린 소환수 번호(6c 받음 · 6d 잃음)
    int maxPetSize = 0;                  // 거느린 크기 합의 최댓값
    const SpellInfo* attackSpell = nullptr;   // 부활마다 고르는 공격 주문
    bool firstCastAfterRevive = false;   // 부활 뒤 첫 주문을 아직 안 보냈다
    bool retreating = false;
    uint32_t retreatRunEndFrame = 0;     // 이 프레임까지 도망, 그 뒤는 서서 소치유
    int retreatPoint = -1;               // 도망가는 지점
    bool backingOff = false;
    uint32_t abilReadyFrame[4] = {};     // 능력 번호별로 재사용이 끝나는 프레임
    uint32_t abilGapEndFrame = 0;
    std::vector<bool> seenImportant;     // 번호 65536칸 — 같은 번호를 두 번 받으면 서버의 다시 싣기다
    std::vector<uint8_t> out;            // 이번 바퀴에 실을 메시지(입력 뒤에 붙인다)
    // 방 여닫기(M0 조각 8) — 0 이면 이 더미는 안 나간다
    int64_t cycleAtUs = 0;               // 다음에 나갈 때
    int cyclePos = 0;                    // 방 안 차례 — 다시 들어올 때 입장 간격만큼 벌린다
    int64_t leaveDoneUs = 0;             // Leaving 이 끝나는 때(받기만 하는 1초)
    int64_t rejoinAtUs = 0;              // 다시 들어오기 시작할 때
};

void SendRaw(const Dummy& d, const uint8_t* p, size_t n) {
    // 실패를 세는 까닭 — 보낸 입력 합과 서버가 받은 입력 합이 어긋날 때 더미 쪽 소켓에서 사라진 몫을 가르려고
    if (send(d.sock, reinterpret_cast<const char*>(p), static_cast<int>(n), 0) == SOCKET_ERROR) ++t_count->sendFail;
}

// 키를 받은 뒤의 조각 — 머리 [01][다음에 받을 순번] + 메시지, 전체를 뒤집는다.
void SendGame(Dummy& d, const uint8_t* msg, size_t n) {
    std::vector<uint8_t> p(n + nox::kWireHeaderSize);
    p[0] = 0x01;
    p[1] = d.relNext;
    std::memcpy(p.data() + nox::kWireHeaderSize, msg, n);
    nox::ApplyXor(p.data(), p.size(), d.key);
    SendRaw(d, p.data(), p.size());
}

void Put16(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& o, uint32_t v) {
    Put16(o, v);
    Put16(o, v >> 16);
}

// 처음 보는 물체를 표에 넣고 알린다 — 원본 GAME2_3.c:1524-1532(머리 · 만료 없음) · :1624-1632(항목 · 프레임 + 60).
// 원본은 전체 번호를 받을 때마다 보낸다(같은 칸이어도 — 플랜 9절 「별칭 답 조건」).
void RegisterAlias(Dummy& d, uint16_t netCode, uint16_t type, bool head) {
    const int a = d.aliases.Generate(netCode, type, d.frame);
    if (a < 0) return;
    const uint32_t deadline = head ? 0xFFFFFFFFu : d.frame + nox::kAliasTTL;
    d.aliases.Set(a, netCode, type, deadline);
    d.out.push_back(static_cast<uint8_t>(Op::NEW_ALIAS));
    d.out.push_back(static_cast<uint8_t>(a));
    Put16(d.out, netCode);
    Put16(d.out, type);
    Put32(d.out, deadline);
    ++t_count->sentAlias;
}

void OnUpdate(Dummy& d, const nox::UpdateMsg& m) {
    // 갱신의 머리는 카메라다(번호 0 · 별칭 01 · Server.cpp:18635-18651) — 자리는 내 자리지만 번호는 안 알려 준다.
    if (m.headAlias == 0xff) RegisterAlias(d, m.headNetCode, m.headType, true);
    d.x = m.x;
    d.y = m.y;
    d.others.clear();
    for (const nox::ObjEntry& o : m.objects) {
        if (o.alias == 0xff) RegisterAlias(d, o.netCode, o.type, false);
        // 자기 번호를 2c 로 받기 전에는 아무도 안 센다 — 안 그러면 자기를 「다른 사람」으로 세어 거리 0 에서 칼을 휘두른다.
        if (d.selfNet != 0 && nox::ThingIsPlayer(o.type) && o.netCode != 0 && o.netCode != d.selfNet)
            d.others.push_back(OtherPlayer{o.netCode, o.x, o.y});
    }
    d.visibleOthers = static_cast<int>(d.others.size());
}

// 입장 때 받은 값이 표(HealthOnline · ManaOnline)와 같은지 한 번 — 더미와 서버의 --stat-mult 가 어긋나면 후퇴 기준이 틀어진다.
void CheckEntryStat(Dummy& d, bool& done, const char* what, int got, int want) {
    if (done) return;
    done = true;
    ++t_count->statChecked;
    if (got == want) return;
    ++t_count->statMismatch;
    std::printf("[더미 %d] 입장 값 다름 — %s 받음 %d · 기대 %d\n", d.index, what, got, want);
    std::fflush(stdout);
}

// 싸움이 있었다(42 · 죽음) 또는 맵을 옮겼다 — 싸움 끊김 시계를 새로 연다(kQuietFrames).
void MarkFight(Dummy& d) {
    d.fightFrame = d.frame;
    d.quietNextFrame = d.frame + kQuietFrames;
}

// 중요 메시지 봉투 하나의 내용 — 죽음 e8 <번호2> · 자기 체력 43 <체력2> · 마나 45 · 최대치 dd de · 강화 5A.
// 표에 없는 메시지를 만나면 그 뒤는 못 읽어 멈춘다(봉투 하나에 대개 메시지 하나라 잃는 것이 없다).
void OnImportantBody(Dummy& d, const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const size_t msz = nox::MessageSize(p + i, n - i, /*fromClient=*/false);
        if (msz == 0 || i + msz > n) return;
        if (p[i] == static_cast<uint8_t>(Op::PLAYER_DIED) && msz >= 3) {
            const uint16_t net = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));
            ++t_count->sawDeath;
            d.detourRun = 0;                 // 누가 죽었으면 싸움이 있었다
            MarkFight(d);
            if (net == d.selfNet) {
                ++t_count->selfDeath;
                d.dead = true;
                d.reviveClickFrame = d.frame + kReviveFirstFrames;
                d.detourIndex = -1;
                d.stuckHave = false;
                d.selfBits = 0;              // 죽으면 강화가 풀린다 — 5A 가 늦어도 부활 뒤 새로 걸게
                d.retreating = false;
                d.backingOff = false;
                d.castHoldEndFrame = d.frame;
                d.summonEndFrame = d.frame;  // 죽으면 부르던 것이 끊긴다(Server.cpp DurSummonTick)
            } else {
                for (size_t k = 0; k < d.skip.size();) {
                    const bool expired = static_cast<int32_t>(d.frame - d.skip[k].second) >= 0;
                    if (expired || d.skip[k].first == net) d.skip.erase(d.skip.begin() + static_cast<std::ptrdiff_t>(k));
                    else ++k;
                }
                d.skip.emplace_back(net, d.frame + kDeathSkipFrames);
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA) && msz >= 5) {
            d.hurtNet = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));   // 42 <번호 2> <증감 2> — 음수만 온다(Server.cpp:6711)
            d.hurtFrame = d.frame;
            d.detourRun = 0;                 // 누가 맞았으면 싸움이 있었다
            // 싸움 끊김 시계는 남이 맞았을 때만 새로 연다 — 42 는 자기 몫도 온다(갱신 순서에 자기도 든다 · Server.cpp:18692-18696).
            //   나만 맞는 동안(쫓는 쪽이 못 닿고 주문만 맞음)도 끊김으로 세려고
            if (d.hurtNet != d.selfNet) MarkFight(d);
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH) && msz >= 3) {
            const int hp = p[i + 1] | (p[i + 2] << 8);
            CheckEntryStat(d, d.checkedHp43, "43 체력", hp, d.maxHp);
            d.hp = hp;
            if (d.dead && hp > 0) {
                d.dead = false;
                ++t_count->revives;
                d.attackSpell = nullptr;     // 공격 주문은 부활마다 다시 고른다(원본 기준 GAME4.c:2035-2045)
                d.firstCastAfterRevive = d.cls == 1;   // 「부활 뒤 첫 주문은 자기 강화」는 법사 규칙이다 — 컨저러는 소환이 먼저(플랜 2-7절)
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_MANA) && msz >= 5) {
            // 45 <번호 2> <마나 2>(Server.cpp:6520)
            if (static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8)) == d.selfNet) d.mana = p[i + 3] | (p[i + 4] << 8);
        } else if ((p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH) || p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_MANA)) && msz >= 7) {
            // dd · de <번호 2> <값 2> <값 2> — 입장 꼬리가 싣는 최대치(SnapshotTail.cpp:141 · :144)
            if (static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8)) == d.selfNet) {
                const int v = p[i + 3] | (p[i + 4] << 8);
                if (p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH)) CheckEntryStat(d, d.checkedDd, "dd 최대 체력", v, d.maxHp);
                else CheckEntryStat(d, d.checkedDe, "de 최대 마나", v, d.maxMana);
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_ACQUIRE_CREATURE) && msz >= 5) {
            // 6c <번호 2> <종류 2> — 주인에게만 온다(Server.cpp ReportAcquireCreature)
            ++t_count->petsAcquired;
            d.pets.push_back(static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8)));
            const SummonKind& k = kSummonKinds[d.summonKind];
            if (static_cast<uint16_t>(p[i + 3] | (p[i + 4] << 8)) != k.type) ++t_count->petTypeMismatch;   // 주문과 종류의 짝은 이름 맞추기라(Server.cpp SummonType) 실제로 대조한다
            const int sum = static_cast<int>(d.pets.size()) * k.size;
            if (sum > d.maxPetSize) d.maxPetSize = sum;
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_LOSE_CREATURE) && msz >= 3) {
            // 6d <번호 2>
            ++t_count->petsLost;
            const uint16_t net = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));
            for (size_t k = 0; k < d.pets.size(); ++k)
                if (d.pets[k] == net) { d.pets.erase(d.pets.begin() + static_cast<std::ptrdiff_t>(k)); break; }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_ENCHANTMENT) && msz >= 7) {
            // 5A <번호 2> <비트 4>(Server.cpp:2296-2304) — 모든 사람에게 온다
            const uint16_t net = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));
            const uint32_t bits = static_cast<uint32_t>(p[i + 3]) | (static_cast<uint32_t>(p[i + 4]) << 8) |
                                  (static_cast<uint32_t>(p[i + 5]) << 16) | (static_cast<uint32_t>(p[i + 6]) << 24);
            if (net == d.selfNet) {
                d.selfBits = bits;
                const int on = ((bits & kBuffBitProtect) != 0) + ((bits & kBuffBitHaste) != 0) + ((bits & kBuffBitInvis) != 0);
                if (on >= 2) ++t_count->buffOverlap;
            } else {
                bool found = false;
                for (auto& e : d.enchants)
                    if (e.first == net) { e.second = bits; found = true; break; }
                if (!found) d.enchants.emplace_back(net, bits);
            }
        }
        i += msz;
    }
}

// 실패 지점 앞을 얼마나 찍나 — `ff` 항목 하나가 10바이트라 항목 네다섯 개와 끝 표식이 보이는 크기.
constexpr size_t kDecodeFailBefore = 48;

// before · prevOp · prevSize 는 받은 묶음을 나누다 실패했을 때만 준다 — 실패 지점 뒤만 찍으면 어디서 어긋났는지 모른다(`d6_10c` 의 `op=ff 남은 583` 1건).
void ReportDecodeFail(const Dummy& d, const uint8_t* b, size_t n, const char* why, size_t before = 0, uint8_t prevOp = 0, size_t prevSize = 0) {
    ++t_count->decodeFail;
    // 스레드 여럿이 함께 쓰는 값이라 원자적으로 — 먼저 읽어 보고 더해 20 넘긴 뒤로는 값을 더 올리지 않는다
    if (g_decodeFailShown.load(std::memory_order_relaxed) >= 20 || g_decodeFailShown.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    std::printf("[더미 %d] 못 읽음(%s) op=%02x 남은 %zu:", d.index, why, n ? b[0] : 0, n);
    for (size_t i = 0; i < n && i < 16; ++i) std::printf(" %02x", b[i]);
    if (before != 0) {
        const size_t show = before < kDecodeFailBefore ? before : kDecodeFailBefore;
        std::printf(" · 앞서 읽은 %zu · 직전 op=%02x 크기 %zu · 실패 지점 앞 %zu바이트:", before, prevOp, prevSize, show);
        for (size_t i = show; i > 0; --i) std::printf(" %02x", *(b - i));
    }
    std::printf("\n");
    std::fflush(stdout);
}

// 맵이 바뀌면 서버가 다시 세운다(RejoinReset) — 체력 · 마나가 차고 강화 · 재사용이 풀린다.
void ResetForMap(Dummy& d) {
    MarkFight(d);                        // 맵을 옮기면 싸움 끊김 시계를 새로 연다
    d.hp = d.maxHp;
    d.mana = d.maxMana;
    d.selfBits = 0;
    d.enchants.clear();
    d.spellReadyFrame = d.frame;
    d.castHoldEndFrame = d.frame;
    d.confuseReadyFrame = d.frame + kConfuseMinFrames + d.rng() % (kConfuseMaxFrames - kConfuseMinFrames + 1);
    d.blinkReadyFrame = d.frame;
    d.pixieReadyFrame = d.frame;
    d.summonEndFrame = d.frame;
    d.pets.clear();
    d.attackSpell = nullptr;
    d.firstCastAfterRevive = false;
    d.retreating = false;
    d.backingOff = false;
    d.abilGapEndFrame = d.frame;
    // 서버는 맵이 바뀌어도 능력 쉬는 시간을 되돌리지 않고(되돌리는 곳은 죽을 때 `AbilityCancelAll` 뿐) 판 끝 동안은 줄이지도 않아
    // (`AbilityUpdate` 는 사람 걸음 안이라 `frozen` 이면 안 돈다) 더미는 남은 값을 모른다 — 방금 쓴 셈 치고 한 번 기다린다.
    // 전에는 이 자리에서 지웠더니 둘째 판부터 시작 직후에만 「쉬는중」 거절이 났다(실물 d6_4 4건 · d6_10 8건 · 첫 판 0건).
    for (const AbilityInfo& ab : kAbilities) d.abilReadyFrame[ab.id] = d.frame + ab.delay + kAbilityMargin;
}

// 서버→클라 메시지 나누기 — opcount.cpp:133-197 과 같은 방식.
void WalkMessages(Dummy& d, const uint8_t* b, size_t all) {
    size_t eaten = 0;
    uint8_t prevOp = 0;                      // 해석 실패 기록용 — 직전에 읽은 메시지
    size_t prevSize = 0;
    while (eaten < all) {
        bool allZero = true;                 // 조각 끝의 0 채움은 메시지가 아니다
        for (size_t i = eaten; i < all; ++i)
            if (b[i] != 0) { allZero = false; break; }
        if (allZero) return;

        const uint8_t op = b[eaten];
        size_t msz = nox::MessageSize(b + eaten, all - eaten, /*fromClient=*/false);

        if (op == static_cast<uint8_t>(Op::IMPORTANT)) {
            // aa 뒤에는 cc 봉투와 봉투 없는 메시지(42 · 45)가 줄 선 순서대로 섞여 온다(Server.cpp:513-521 — 큐 항목을 그대로 잇는다).
            // 머리만 여기서 먹고 뒤는 아래 갈래가 하나씩 읽는다 — 봉투만 훑으면 봉투 없는 것을 만난 뒤의 봉투를 통째로 놓친다.
            msz = 1;
            // 정품은 aa 를 받을 때마다 같은 묶음 27 의 프레임으로 답한다(원본 기준 network_client.go:397-415).
            d.out.push_back(static_cast<uint8_t>(Op::IMPORTANT_ACK));
            Put32(d.out, d.frame);
            ++t_count->sentAck;
        } else if (op == static_cast<uint8_t>(Op::SEQ_IMPORTANT)) {
            if (eaten + 4 > all || eaten + 4 + b[eaten + 3] > all) { ReportDecodeFail(d, b + eaten, all - eaten, "봉투", eaten, prevOp, prevSize); return; }
            const size_t inner = b[eaten + 3];
            const uint16_t id = static_cast<uint16_t>(b[eaten + 1] | (b[eaten + 2] << 8));
            if (d.seenImportant[id]) ++t_count->dupImportant;
            else OnImportantBody(d, b + eaten + 4, inner);
            d.seenImportant[id] = true;
            d.seenImportant[static_cast<uint16_t>(id + 0x8000)] = false;   // 번호가 한 바퀴 돌 때를 대비해 반대쪽을 비운다
            msz = 4 + inner;
        } else if (msz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const nox::UpdateMsg m = nox::DecodeUpdate(b + eaten + 1, all - eaten - 1, &d.aliases);
            if (!m.ok) { ReportDecodeFail(d, b + eaten, all - eaten, "갱신", eaten, prevOp, prevSize); return; }
            msz = 1 + m.consumed;
            OnUpdate(d, m);
        } else if (op == static_cast<uint8_t>(Op::TIMESTAMP) && msz >= 3) {
            const uint16_t low = static_cast<uint16_t>(b[eaten + 1] | (b[eaten + 2] << 8));
            uint32_t f = (d.frame & 0xFFFF0000u) | low;
            if (low < 0x4000 && (d.frame & 0xFFFF) > 0xC000) f += 0x10000;   // 하위 16비트가 한 바퀴 돌았다
            d.frame = f;
        } else if (op == static_cast<uint8_t>(Op::FULL_TIMESTAMP) && msz >= 5) {
            d.frame = static_cast<uint32_t>(b[eaten + 1]) | (static_cast<uint32_t>(b[eaten + 2]) << 8) |
                      (static_cast<uint32_t>(b[eaten + 3]) << 16) | (static_cast<uint32_t>(b[eaten + 4]) << 24);
        } else if (op == static_cast<uint8_t>(Op::JOIN_DATA) && msz >= 3) {
            d.selfNet = static_cast<uint16_t>(b[eaten + 1] | (b[eaten + 2] << 8));   // 2c <번호 2> <0 4> — 자기 번호를 알려 주는 메시지(JoinBundle.h:10)
        } else if (op == static_cast<uint8_t>(Op::USE_MAP) && msz >= 2) {
            // 맵이 바뀌면 서버가 준비를 끈다(Server.cpp:1915) — 준비를 안 보내면 중요 메시지가 멈춘다.
            // 맵 파일 이름은 본문 첫 칸부터 최대 31글자(BuildUseMap · JoinBundle.cpp:170). 처음 받은 맵이 대기 맵이다.
            const char* name = reinterpret_cast<const char*>(b + eaten + 1);
            size_t nameLen = 0;
            while (nameLen < 31 && eaten + 1 + nameLen < all && name[nameLen] != 0) ++nameLen;
            d.mapName.assign(name, nameLen);
            if (d.waitMapName.empty()) d.waitMapName = d.mapName;
            d.visibleOthers = 0;
            d.fullSinceUs = 0;
            d.others.clear();
            d.skip.clear();
            d.dead = false;                 // 맵을 옮기면 서버가 다시 세운다(RejoinReset)
            d.detourIndex = -1;
            d.lastDetour = -1;
            d.detourRun = 0;
            d.path.clear();
            d.stuckHave = false;
            d.aliases.Clear();
            ResetForMap(d);
            const uint8_t ready = static_cast<uint8_t>(Op::CLIENT_READY);
            d.out.push_back(ready);
        } else if (msz != 0 && eaten + msz <= all) {
            OnImportantBody(d, b + eaten, msz);   // 봉투 없이 온 42 · 45 따위
        }

        if (msz == 0 || eaten + msz > all) { ReportDecodeFail(d, b + eaten, all - eaten, "길이", eaten, prevOp, prevSize); return; }
        prevOp = op;
        prevSize = msz;
        eaten += msz;
    }
}

void OnDatagram(Dummy& d, uint8_t* p, size_t n) {
    ++t_count->recvDgram;
    t_count->recvBytes += n;
    if (n < nox::kWireHeaderSize) return;

    if (d.phase == Phase::TryJoin) {             // 키를 받기 전 — 평문
        if (n == 3 && p[2] == static_cast<uint8_t>(Op::SERVER_JOIN_OK)) {
            const uint8_t connect[3] = {0xff, 0x00, 0x00};
            SendRaw(d, connect, sizeof(connect));
            d.phase = Phase::Connect;
            d.retryAtUs = NowUs() + kJoinRetryUs;
        }
        return;
    }
    if (d.phase == Phase::Connect) {
        if (n == 10 && p[2] == static_cast<uint8_t>(Op::ACCEPTED)) {
            d.key = p[9];
            d.relNext = static_cast<uint8_t>(p[1] + 1);
            // 0x20 + 153바이트. 본문 66번이 직업(GameBalance.h:52 · Server.cpp:4276-4278).
            uint8_t accept[154] = {};
            accept[0] = static_cast<uint8_t>(Op::CLIENT_ACCEPT);
            accept[1 + 66] = static_cast<uint8_t>(d.cls);
            // ★ 11-12 정품 화면 확인(표 118~120) — 이름 · 색을 0 으로 두면 서버가 그 97바이트를 남에게 그대로 소개해(Server.cpp:4351)
            //   정품 화면에 더미가 검은 피부 · 점수판 빈 이름으로 나왔다. 짜임은 원본 PlayerInfo(player.go:971-995):
            //   이름 +0(UTF-16 50바이트) · 여자 +67 · 색 +68~+87(머리 · 피부 · 콧수염 · 염소수염 · 턱수염 RGB 셋씩 · 바지 · 윗옷 둘 · 신발 둘 한 바이트씩).
            //   색은 정품 R1 실행의 접속 정보 그대로(`_out/bb_block/R1/server.log` `[접속정보97]` 줄 · git 밖) — 수염 셋이 피부와 같으면 수염 없음
            static constexpr uint8_t kRetailColors[20] = {0x73, 0x4d, 0x22, 0xda, 0x9a, 0x6e, 0xda, 0x9a, 0x6e, 0xda, 0x9a, 0x6e,
                                                          0xda, 0x9a, 0x6e, 0x0c, 0x07, 0x13, 0x17, 0x06};
            std::memcpy(accept + 1 + 68, kRetailColors, sizeof(kRetailColors));
            char name[16];
            const int nameLen = std::snprintf(name, sizeof(name), "Dummy%d", d.index + 1);   // 24글자 + 끝 0 안
            for (int i = 0; i < nameLen && i < 24; ++i) accept[1 + 2 * i] = static_cast<uint8_t>(name[i]);
            SendGame(d, accept, sizeof(accept));
            d.phase = Phase::Game;
            ++t_count->entered;                  // Connect → Game 은 한 번뿐이다(되돌아가는 길이 없다)
            d.pingAtUs = NowUs() + kPingUs;
        }
        return;
    }

    nox::ApplyXor(p, n, d.key);
    if ((p[0] & 0x80) != 0 && (p[0] & 0x7f) == 0) {
        // 신뢰 조각은 차례대로만 받는다(원본 conn.go:695-698). 앞선 것 · 지난 것은 버리면 서버가 다시 보낸다.
        if (p[1] != d.relNext) return;
        ++d.relNext;
    }
    WalkMessages(d, p + nox::kWireHeaderSize, n - nox::kWireHeaderSize);
}

void DrainSocket(Dummy& d, std::vector<uint8_t>& buf) {
    for (;;) {
        const int n = recv(d.sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;   // 서버가 아직 안 떴을 때 오는 ICMP 답 — 한 번씩 소비된다
            return;
        }
        OnDatagram(d, buf.data(), static_cast<size_t>(n));
    }
}

void SendTryJoin(const Dummy& d) {
    uint8_t tryJoin[100] = {};
    tryJoin[2] = static_cast<uint8_t>(Op::SERVER_TRY_JOIN);
    SendRaw(d, tryJoin, sizeof(tryJoin));
}

// 정품 입력 꼴 — 방향 칸 둘이 기본(정품 R1 14바이트 4,921번 · 칸이 왜 둘인지는 까닭 미상이라 바이트 그대로).
// 뒤에 붙는 항목 — 앞으로 = 코드 2 + 데이터 1바이트(서버는 데이터를 안 본다 · Server.cpp:3424) · 클릭 = 코드 6, 데이터 없음(Server.cpp:3445).
void PutInput(std::vector<uint8_t>& o, uint8_t facing, bool move, bool click) {
    const size_t at = o.size();
    const uint8_t m[12] = {static_cast<uint8_t>(Op::PLAYER_INPUT), 0x0a, 0x01, 0, 0, 0, facing, 0x01, 0, 0, facing, facing};
    o.insert(o.end(), m, m + sizeof(m));
    if (move) {
        const uint8_t forward[5] = {0x02, 0, 0, 0, 0x01};
        o.insert(o.end(), forward, forward + sizeof(forward));
    }
    if (click) {
        const uint8_t action[4] = {0x06, 0, 0, 0};
        o.insert(o.end(), action, action + sizeof(action));
    }
    o[at + 1] = static_cast<uint8_t>(o.size() - at - 2);
}

// 채팅 — [a8][번호 2][깃발 1][x 2][y 2][글자 수 1][미상 2][글자]. 깃발 0x02 = 한 바이트 글자(Wire.h TEXT_MESSAGE).
// 번호 · 좌표는 서버가 새로 지어서 0 으로 둔다(tools/match_smoke.ps1:172-178).
void PutChat(std::vector<uint8_t>& o, const char* text) {
    const size_t n = std::strlen(text);
    const size_t at = o.size();
    o.resize(at + 11 + n, 0);
    o[at] = static_cast<uint8_t>(Op::TEXT_MESSAGE);
    o[at + 3] = 0x02;
    o[at + 8] = static_cast<uint8_t>(n);
    std::memcpy(o.data() + at + 11, text, n);
}

// 방이 차면 !start. 방 안의 더미가 전부 보낸다 — 방장 것만 판을 열고 나머지는 안내 한 줄로 끝나서(RoomTick.cpp:835 · :854) 누가 방장인지 맞출 필요가 없다.
void MaybeStart(Dummy& d, int64_t nowUs, const Args& a) {
    if (a.roomSize < 2 || d.mapName.empty() || d.mapName != d.waitMapName || d.visibleOthers < a.roomSize - 1) {
        d.fullSinceUs = 0;
        return;
    }
    if (d.fullSinceUs == 0) {
        d.fullSinceUs = nowUs;
        d.startAtUs = nowUs + static_cast<int64_t>(a.startWaitMs) * 1000;
        return;
    }
    if (nowUs < d.startAtUs) return;
    PutChat(d.out, "!start");
    ++t_count->sentStart;
    d.startAtUs = nowUs + kStartRetryUs;
}

// 바라보는 값 — 0 이 +x, 64 가 +y, 0~255(DirVectors.h).
uint8_t FacingTo(int dx, int dy) {
    const double v = std::floor(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 128.0 / 3.14159265358979323846 + 0.5);
    return static_cast<uint8_t>(static_cast<int>(v) & 0xff);
}

bool FrameReached(const Dummy& d, uint32_t at) { return static_cast<int32_t>(d.frame - at) >= 0; }

double DistTo(const Dummy& d, int x, int y) {
    const double dx = static_cast<double>(x - d.x), dy = static_cast<double>(y - d.y);
    return std::sqrt(dx * dx + dy * dy);
}

// 가장 가까운 상대 — 막 죽은 상대는 뺀다.
const OtherPlayer* NearestTarget(const Dummy& d) {
    const OtherPlayer* best = nullptr;
    double bestDist = 0.0;
    for (const OtherPlayer& o : d.others) {
        bool skipped = false;
        for (const auto& k : d.skip)
            if (k.first == o.net && static_cast<int32_t>(d.frame - k.second) < 0) { skipped = true; break; }
        if (skipped) continue;
        const double dist = DistTo(d, o.x, o.y);
        if (!best || dist < bestDist) { best = &o; bestDist = dist; }
    }
    return best;
}

// 한 지점으로 달린다 — 힘은 늘 바라보는 쪽이라 방향을 돌리고(Server.cpp:3439-3442) 커서를 앞쪽 멀리 둔다(Server.cpp:5621).
void MoveToward(Dummy& d, int tx, int ty) {
    const int dx = tx - d.x, dy = ty - d.y;
    if (dx == 0 && dy == 0) return;
    d.facing = FacingTo(dx, dy);
    d.wantMove = true;
    const double len = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
    d.out.push_back(static_cast<uint8_t>(Op::MOUSE));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(d.x + std::lround(dx * kCursorAhead / len))));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(d.y + std::lround(dy * kCursorAhead / len))));
    ++t_count->sentMouse;
}

// 벽 격자 — 더미가 벽을 돌아가는 데만 쓴다. 서버 물리와 별개이고, 벽이 있는 칸 전체를 막힌 것으로 본다.
// (11-12부터 사람을 미는 맵 물체가 걸친 칸도 막힌 칸이다 — BakeNavObstacles)
// 왜 필요한가 — Estate 는 벽이 456칸인 저택이라 곧장 쫓으면 벽 양쪽에 서서 만나지 못한다(11-10 실물 — 300초 중 약 240초).
// 격자는 맵을 읽은 뒤 읽기만 하므로 스레드끼리 함께 쓴다.
struct NavGrid {
    bool ok = false;
    std::vector<uint8_t> blocked;
};
NavGrid g_nav;

// ★ 11-12 — 서버 벽 모양(칸 안 대각선 선분)을 그대로 싣는다. 칸 격자는 벽 칸 전체를 막을 뿐 「몸이 벽 선에 닿나」를 못 본다 —
//   벽 모서리를 질러가다 벽 선 9.2~9.9 에 붙어 멈췄다(nox-dummy-obstacle-progress.md 미결 1 A). 서버 판정 코드를 그대로 부르려고
//   `nox::Server` 하나에 벽만 넣는다(nox_datadump --walls 와 같은 길). 맵을 읽은 뒤 읽기만 하므로 스레드끼리 함께 쓴다.
std::unique_ptr<nox::Server> g_wallShape;

bool BodyClear(double x0, double y0, double x1, double y1);   // 아래 BakeNavObstacles 뒤 — 맵 물체 몸을 같이 본다

// 훑기에 쓰는 버퍼는 훑을 때마다 고쳐 쓰므로 스레드마다 한 벌(M0 조각 7) — 처음 길을 찾을 때 잡는다(약 0.75MB).
struct NavSearch {
    std::vector<uint32_t> seen;          // 이번 훑기에 가 봤나(훑을 때마다 지우지 않게 세대 번호로)
    std::vector<int32_t> from;           // 어느 칸에서 왔나
    std::vector<uint32_t> nearGoal;      // 도착 칸이 막혔을 때 도착으로 보는 둘레 칸(세대 번호)
    std::vector<int32_t> queue;
    uint32_t stamp = 0;
};
thread_local NavSearch t_navSearch;

bool CellBlocked(int cx, int cy) {
    return cx < 0 || cy < 0 || cx >= kNavSize || cy >= kNavSize || g_nav.blocked[static_cast<size_t>(cy * kNavSize + cx)] != 0;
}

// 두 점 사이에 벽 칸이 없나. 양 끝 칸은 안 본다 — 몸이 벽 칸 귀퉁이에 걸쳐 서 있어도 앞을 볼 수 있게.
bool GridClear(int x0, int y0, int x1, int y1) {
    if (!g_nav.ok) return true;
    const double dx = static_cast<double>(x1 - x0), dy = static_cast<double>(y1 - y0);
    const int steps = std::max(1, static_cast<int>(std::sqrt(dx * dx + dy * dy) / 8.0));   // 칸의 3분의 1 간격
    const int c0x = x0 / kNavCell, c0y = y0 / kNavCell, c1x = x1 / kNavCell, c1y = y1 / kNavCell;
    for (int i = 1; i < steps; ++i) {
        const int cx = static_cast<int>((x0 + dx * i / steps) / kNavCell);
        const int cy = static_cast<int>((y0 + dy * i / steps) / kNavCell);
        if ((cx == c0x && cy == c0y) || (cx == c1x && cy == c1y)) continue;
        if (CellBlocked(cx, cy)) return false;
    }
    return true;
}

// 너비 우선으로 칸을 훑어 길을 찾는다. 대각선은 옆 두 칸이 다 비었을 때만 — 벽 모서리를 뚫지 않게.
// 출발 · 도착 칸은 막혀 있어도 된다(몸이 벽 칸 귀퉁이에 걸쳐 설 수 있다).
// ★ 11-12 B — 막힌 칸에 선 몸은 둘레 칸이 다 막혀 길이 0점이 되고, 그러면 상대를 곧장 향해 같은 물체에 다시 걸렸다
//   (고친 지도의 멈춘 자리 넷 (3514,1788) · (2545,1892) · (2485,2397) · (3223,1724) 모두 길 0점 · 벽만이면 44 · 44 · 23점 · --nav-probe).
//   막힌 칸에 걸쳐 섰거나 막힌 칸의 상대를 쫓을 때, 그 칸 가운데는 벽 선 위 · 물체 안일 수 있다(벽 선까지 0.0).
//   ① 출발 칸이 막혔으면 둘레 kNavEscapeRing 칸 안의 빈칸 가운데 몸 폭으로 곧장 갈 수 있는 칸들에서 훑고,
//      도착 칸이 막혔으면 둘레 빈칸 가운데 도착 자리까지 몸 폭으로 트인 칸에 닿으면 도착으로 본다. 마지막 점은 도착 자리 그대로
//   ② 그래도 길이 없으면 막힌 쪽(도착만 → 출발만 → 둘 다 차례로) 칸 둘레 kNavEscapeRing 칸 안의 **물체 칸**(벽 칸은 아님)도
//      지나갈 수 있게 다시 훑는다 — 칸마다 9점으로 찍으면 몸이 지나는 틈도 닫힌다: 벽 홈 (3514,1788)의 출구는 소나무
//      `TreePine04` r12 · `TreePine08` r15 사이 35 폭인데 출구 칸이 모두 물체 칸이었다. 둥근 물체는 서버가 몸을 옆으로 밀어 비켜 가게 한다.
//      한쪽씩 여는 까닭 — 여는 물체 칸을 줄인다. 빈칸으로 빠져나갈 수 있는 쪽까지 열면 물체 칸을 지나는 더 짧은 길이 날 수 있다.
//      ② 에서도 출발은 ① 처럼 둘레의 트인 빈칸에서 훑는다 — 막힌 출발 칸에서 훑으면 길 첫 점이 그 칸 가운데가 되는데
//      (3473,1788) → (3504,1775)는 그 점이 벽 선 위(벽 선까지 0.0)였다 · --nav-probe.
//      둘 다 한꺼번에 열고 막힌 출발 칸에서 훑던 앞 판은 (2545,1892) · (3223,1724)에서도 고른 점의 몸 폭이 0이었다(지금 1)
//   ② 는 ① 로 길이 없고 출발 · 도착 칸이 막혔을 때만 — 빈칸에서 빈칸으로 가는 길은 전과 같다
constexpr int kNavEscapeRing = 2;   // 몸이 선 쪽 빈칸은 대개 바로 옆 칸이다 — 두 칸이면 큰 물체(반지름 18 · 상자 45x30)가 걸친 칸 하나를 건너도 닿는다
bool FindPathPass(int fromX, int fromY, int toX, int toY, bool relaxStart, bool relaxGoal, std::vector<Point>& out) {
    out.clear();
    const int sx = fromX / kNavCell, sy = fromY / kNavCell, gx = toX / kNavCell, gy = toY / kNavCell;
    const int start = sy * kNavSize + sx, goal = gy * kNavSize + gx;
    static const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const bool startBlocked = start != goal && CellBlocked(sx, sy), goalBlocked = start != goal && CellBlocked(gx, gy);
    // 지나갈 수 있는 칸 — ② 에서는 연 쪽(막힌 출발 · 도착 칸) 둘레의 물체 칸도
    auto open = [&](int cx, int cy) {
        if (!CellBlocked(cx, cy)) return true;
        if (g_nav.blocked[static_cast<size_t>(cy) * kNavSize + cx] != 2) return false;
        return (relaxStart && startBlocked && std::abs(cx - sx) <= kNavEscapeRing && std::abs(cy - sy) <= kNavEscapeRing) ||
               (relaxGoal && goalBlocked && std::abs(cx - gx) <= kNavEscapeRing && std::abs(cy - gy) <= kNavEscapeRing);
    };
    NavSearch& s = t_navSearch;
    if (s.seen.empty()) {
        s.seen.assign(static_cast<size_t>(kNavSize) * kNavSize, 0);
        s.from.assign(static_cast<size_t>(kNavSize) * kNavSize, -1);
        s.nearGoal.assign(static_cast<size_t>(kNavSize) * kNavSize, 0);
    }
    const uint32_t stamp = ++s.stamp;
    auto mid = [](int c) { return c * kNavCell + kNavCell / 2; };
    s.queue.clear();
    s.seen[static_cast<size_t>(start)] = stamp;
    s.from[static_cast<size_t>(start)] = -1;
    if (startBlocked) {
        for (int ry = -kNavEscapeRing; ry <= kNavEscapeRing; ++ry)
            for (int rx = -kNavEscapeRing; rx <= kNavEscapeRing; ++rx) {
                const int cx = sx + rx, cy = sy + ry;
                if ((rx == 0 && ry == 0) || CellBlocked(cx, cy) || !BodyClear(fromX, fromY, mid(cx), mid(cy))) continue;
                const int c = cy * kNavSize + cx;
                s.seen[static_cast<size_t>(c)] = stamp;
                s.from[static_cast<size_t>(c)] = -1;
                s.queue.push_back(c);
            }
    }
    const bool fromStartCell = s.queue.empty();   // 트인 둘레 빈칸이 없어 막힌 출발 칸 그대로에서 훑나
    if (fromStartCell) s.queue.push_back(start);
    bool goalNear = false;   // 막힌 도착 칸 대신 도착으로 볼 둘레 칸이 있나
    if (goalBlocked) {
        for (int ry = -kNavEscapeRing; ry <= kNavEscapeRing; ++ry)
            for (int rx = -kNavEscapeRing; rx <= kNavEscapeRing; ++rx) {
                const int cx = gx + rx, cy = gy + ry;
                if ((rx == 0 && ry == 0) || CellBlocked(cx, cy) || !BodyClear(mid(cx), mid(cy), toX, toY)) continue;
                s.nearGoal[static_cast<size_t>(cy * kNavSize + cx)] = stamp;
                goalNear = true;
            }
    }
    const bool goalEnter = !goalNear || relaxGoal;   // 막힌 도착 칸에 바로 들어가도 되나 — ① 에서 둘레 칸이 있으면 그 칸으로만 도착
    auto arrived = [&](int c) { return (goalNear && s.nearGoal[static_cast<size_t>(c)] == stamp) || (goalEnter && c == goal); };
    int found = start == goal ? goal : -1;
    for (int c : s.queue)
        if (found < 0 && arrived(c)) found = c;
    for (size_t head = 0; head < s.queue.size() && found < 0; ++head) {
        const int c = s.queue[head];
        const int cx = c % kNavSize, cy = c / kNavSize;
        for (int k = 0; k < 8 && found < 0; ++k) {
            const int nx = cx + kDx[k], ny = cy + kDy[k];
            if (nx < 0 || ny < 0 || nx >= kNavSize || ny >= kNavSize) continue;
            const int n = ny * kNavSize + nx;
            if (s.seen[static_cast<size_t>(n)] == stamp) continue;
            if (!(n == goal && goalEnter) && !open(nx, ny)) continue;
            if (k >= 4 && (!open(cx + kDx[k], cy) || !open(cx, cy + kDy[k]))) continue;
            s.seen[static_cast<size_t>(n)] = stamp;
            s.from[static_cast<size_t>(n)] = c;
            s.queue.push_back(n);
            if (arrived(n)) found = n;
        }
    }
    if (found < 0) return false;
    for (int c = found; c != -1; c = s.from[static_cast<size_t>(c)])
        out.push_back(Point{mid(c % kNavSize), mid(c / kNavSize)});
    std::reverse(out.begin(), out.end());
    // 막힌 출발 칸 그대로에서 훑었으면 그 칸 가운데(벽 선 위 · 물체 안일 수 있다)는 빼고 다음 칸부터
    if (startBlocked && fromStartCell && out.size() > 1) out.erase(out.begin());
    // 막힌 도착 칸의 가운데(벽 선 위 · 물체 안일 수 있다) 대신 도착 자리 그대로
    if (goalBlocked) {
        if (found == goal) out.back() = Point{toX, toY};
        else out.push_back(Point{toX, toY});
    }
    return true;
}

bool FindPath(int fromX, int fromY, int toX, int toY, std::vector<Point>& out) {
    out.clear();
    if (!g_nav.ok) return false;
    const int sx = fromX / kNavCell, sy = fromY / kNavCell, gx = toX / kNavCell, gy = toY / kNavCell;
    if (sx < 0 || sy < 0 || gx < 0 || gy < 0 || sx >= kNavSize || sy >= kNavSize || gx >= kNavSize || gy >= kNavSize) return false;
    if (FindPathPass(fromX, fromY, toX, toY, false, false, out)) return true;
    const bool sb = CellBlocked(sx, sy), gb = CellBlocked(gx, gy);
    if (gb && FindPathPass(fromX, fromY, toX, toY, false, true, out)) return true;
    if (sb && FindPathPass(fromX, fromY, toX, toY, true, false, out)) return true;
    return sb && gb && FindPathPass(fromX, fromY, toX, toY, true, true, out);
}

// 맵 물체의 몸 — 서버 `Server::PhysBody` 의 모양 칸만 옮겼다. 상자 네 값은 `Server::CalcBox`(Server.h:1755)와 같다.
struct NavBody {
    float x = 0, y = 0;
    int shape = 0;                       // 2 원 · 3 상자
    float r = 0;
    float lt = 0, lb = 0, rt = 0, rb = 0;
};

// 점이 몸 안에 드는가 — `Server::BodyContains`(Server.cpp:9319 · 정품 sub_547DB0)와 같은 식. 원은 거리제곱, 상자는 마름모 네 변.
bool NavBodyContains(const NavBody& o, float qx, float qy) {
    if (o.shape == 2) {
        const float dx = o.x - qx, dy = o.y - qy;
        return dy * dy + dx * dx <= o.r * o.r;
    }
    const float v5 = o.lt + o.x, v6 = o.lb + o.y;
    if ((v6 - v5 + qx - qy) * 0.70709997f < 0.0f && (o.rb + o.y - (o.rt + o.x) + qx - qy) * 0.70709997f > 0.0f) {
        const float v7 = o.rt + o.x, v8 = o.rb + o.y;
        return (v8 + v7 - qx - qy) * 0.70709997f > 0.0f && (v6 + v5 - qx - qy) * 0.70709997f < 0.0f;
    }
    return false;
}

// 찍은 맵 물체 몸 — 몸 폭 판정(BodyClear)이 물체를 비켜 가는지 본다. 네 값은 몸을 두르는 사각형(BodyRect)
struct NavBodyBox {
    NavBody body;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::vector<NavBodyBox> g_navBodies;

// ★ 11-12 판 멈춤 까닭 하나 — 맵 물체(돌기둥 · 나무)도 막힌 칸으로 찍는다. 벽만 찍던 때 법사가 격자에선 빈칸인 자리에서
//   동쪽 20 의 돌기둥 `CaveRockPillarShort2`(2565,1889) 쪽으로 13분 넘게 달렸다(`_out/stuck/b` 방 3 · nox_datadump --walls).
//   두 더미를 그 판의 두 자리에 세운 재현 시험(8방 10분): 벽만이면 죽음 0 · 공격 0 · 물체를 찍으면 처음 4분 죽음 102.
//   ⚠ 몸 반지름(10)은 안 더한다(정품 규칙 그대로) — 반지름 8 덤불 둘 사이 · 사선 벽 홈처럼 몸이 못 지나는 틈은 빈칸으로 남아
//     같은 시험에서 끝날 때 8방 중 7방이 다시 멈췄다(미결 · nox-dummy-obstacle-progress.md)
//   ① 고르기 — 서버가 사람과 부딪히게 싣는 장애물(`WorldBuild.cpp:1655-1664` 거르기 그대로) 가운데 **사람을 미는 것만**.
//      ALLOW_OVERLAP(0x8)이면 겹쳐도 안 밀고(`Server.cpp:5401` · `:5427`), NO_PUSH_CHARACTERS(0x2000)이면 사람(클래스 0x4)은 안 민다(`:5402` · `:5428`).
//      밀지 않는 것(`FlowersYellowSparse` · `FoliageDense1` 같은 BELOW+ALLOW_OVERLAP)까지 찍으면 지나갈 수 있는 길을 더미가 스스로 막는다
//   ② 찍기 — 서버 몬스터 길 격자와 같은 정품 규칙(`AIPathBakeObstacles` Server.cpp:9343 · object_ai_path.go:545) —
//      칸 안 아홉 점({2.3 · 11.5 · 20.7} 의 3×3) 가운데 하나라도 몸 안이면 막힌 칸
//   돌려주는 값 = 새로 막은 칸 수(벽 칸과 겹친 것은 빼고) · thing.bin 을 못 읽으면 −1(벽만 쓴다)
int BakeNavObstacles(nox::MapFile& mf, const std::string& thingPath, int& objCount) {
    objCount = 0;
    nox::ThingDb tdb;
    if (!tdb.Load(thingPath)) return -1;
    static const float kS[3] = {2.3f, 11.5f, 20.7f};
    int added = 0;
    for (const auto& o : mf.ReadObjects()) {
        const uint16_t w = tdb.WireByName(o.type);
        const nox::ThingInfo* t = w ? tdb.ByWire(w) : nullptr;
        if (!t || !t->hasCollide) continue;
        const uint32_t fl = nox::ParseObjectFlags(t->flagsRaw), cl = nox::ParseObjectClass(t->classRaw);
        if ((fl & 0x40) || !(cl & 0x400000) || (cl & 0x80) || (t->shapeKind != 2 && t->shapeKind != 3)) continue;   // WorldBuild.cpp:1661-1664
        if (fl & (0x8 | 0x2000)) continue;                                                                      // 사람을 안 민다
        NavBody b;
        b.x = o.x;
        b.y = o.y;
        b.shape = t->shapeKind;
        b.r = t->radius;
        if (b.shape == 3) {
            const float px = t->boxW * 0.35354999f, py = t->boxH * 0.35354999f;
            b.lt = -px + py; b.lb = -px - py; b.rt = px + py; b.rb = px - py;
        }
        ++objCount;
        // 몸을 두르는 사각형(`BodyRect` Server.cpp:5316) 안의 칸만 본다
        const float x0 = b.shape == 3 ? b.x + b.lb : b.x - b.r, y0 = b.shape == 3 ? b.y + b.lb : b.y - b.r;
        const float x1 = b.shape == 3 ? b.x + b.rt : b.x + b.r, y1 = b.shape == 3 ? b.y + b.rt : b.y + b.r;
        g_navBodies.push_back(NavBodyBox{b, x0, y0, x1, y1});
        const int gx0 = std::max(0, static_cast<int>(x0 / kNavCell)), gx1 = std::min(kNavSize - 1, static_cast<int>(x1 / kNavCell));
        const int gy0 = std::max(0, static_cast<int>(y0 / kNavCell)), gy1 = std::min(kNavSize - 1, static_cast<int>(y1 / kNavCell));
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
                bool hit = false;
                for (int a = 0; a < 3 && !hit; ++a)
                    for (int c = 0; c < 3 && !hit; ++c)
                        if (NavBodyContains(b, gx * static_cast<float>(kNavCell) + kS[c], gy * static_cast<float>(kNavCell) + kS[a])) hit = true;
                if (!hit) continue;
                uint8_t& cell = g_nav.blocked[static_cast<size_t>(gy) * kNavSize + gx];
                if (cell == 0) {
                    ++added;
                    cell = 2;   // 물체 칸(벽 칸은 1) — 막힌 칸이라는 뜻은 같고, --nav-probe 칸 지도가 가른다
                }
            }
    }
    return added;
}

// 몸 폭으로 곧장 갈 수 있나 — 가운데 줄과 좌우로 몸 반지름(10)만큼 비킨 두 줄이 서버 벽 선과 사람을 미는 맵 물체에 안 걸리면 참.
//   벽은 서버 걸음 추적과 같은 `MapTraceRay` 깃발 0x1(Server.cpp:5127 주석)이고, 물체는 정품 `BodyContains` 와 같은 식(NavBodyContains)으로
//   줄 위를 kBodyStep 간격으로 찍는다 — Estate 의 사람을 미는 물체 159개 중 가장 작은 것이 반지름 8 · 상자 한 변 12 라
//   3 간격 · 줄 사이 10 이면 줄 사이로 빠지지 않는다(_out/obst/estate_obstacles.txt).
//   좌우 두 줄은 양 끝 몸 반지름만큼을 빼고 본다 — 벽 · 물체에 붙어 선 몸(벽 선까지 9.2 · 8.5)은 떨어지는 쪽으로 가도
//   비킨 줄의 첫머리가 벽 너머 · 물체 안에서 시작해 늘 「막힘」이 되고, 붙어 선 상대에게 가는 줄은 끝머리가 그렇다
//   (끝머리를 빼자 둘레 빈칸 가운데 트인 칸이 (2485,2397) 도착 쪽 11 → 15 · (3223,1724) 출발 쪽 3 → 4 · --nav-probe.
//   소나무 · 벽 홈 자리 (3504,1775)에 선 상대는 그래도 14 가운데 0이라 FindPath ② 가 맡는다).
//   가운데 줄은 처음부터 끝까지 본다(몸 가운데는 서버가 밖으로 밀어 둔다)
constexpr double kBodyStep = 3.0;
bool BodyClear(double x0, double y0, double x1, double y1) {
    const double dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0) return true;
    const double r = nox::kPlayerRadius, ux = dx / len, uy = dy / len, nx = -uy * r, ny = ux * r;
    auto wallOk = [](double ax, double ay, double bx, double by) {
        return g_wallShape->DebugTraceRayFlags(static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(bx), static_cast<float>(by), 0x1);
    };
    if (g_wallShape) {
        if (!wallOk(x0, y0, x1, y1)) return false;
        if (len > 2 * r) {
            const double sx = x0 + ux * r, sy = y0 + uy * r, ex = x1 - ux * r, ey = y1 - uy * r;
            if (!wallOk(sx + nx, sy + ny, ex + nx, ey + ny) || !wallOk(sx - nx, sy - ny, ex - nx, ey - ny)) return false;
        }
    }
    const double bx0 = std::min(x0, x1) - r, bx1 = std::max(x0, x1) + r, by0 = std::min(y0, y1) - r, by1 = std::max(y0, y1) + r;
    const int steps = std::max(1, static_cast<int>(std::ceil(len / kBodyStep)));
    for (const NavBodyBox& o : g_navBodies) {
        if (o.x1 < bx0 || o.x0 > bx1 || o.y1 < by0 || o.y0 > by1) continue;
        for (int i = 0; i <= steps; ++i) {
            const double s = len * i / steps, px = x0 + ux * s, py = y0 + uy * s;
            if (NavBodyContains(o.body, static_cast<float>(px), static_cast<float>(py))) return false;
            if (s >= r && s <= len - r && (NavBodyContains(o.body, static_cast<float>(px + nx), static_cast<float>(py + ny)) ||
                           NavBodyContains(o.body, static_cast<float>(px - nx), static_cast<float>(py - ny)))) return false;
        }
    }
    return true;
}

// 맵을 읽어 격자를 채운다. 시작 지점끼리 길이 이어지는지 세어 함께 찍는다 — 격자가 잘못 깔리면 이 값이 무너진다.
bool LoadNav(const std::string& path, const std::string& thingPath, bool objects) {
    nox::MapFile mf;
    if (!mf.Load(path)) return false;
    const auto cells = mf.ReadWalls();
    g_nav.blocked.assign(static_cast<size_t>(kNavSize) * kNavSize, 0);
    for (const auto& w : cells) g_nav.blocked[static_cast<size_t>(w.y) * kNavSize + w.x] = 1;
    std::vector<nox::Server::WallTileIn> tiles;
    tiles.reserve(cells.size());
    for (const auto& w : cells) {
        nox::Server::WallTileIn c;
        c.x = w.x; c.y = w.y; c.dir = w.dir; c.tile = w.tile; c.flag80 = w.flag80;
        tiles.push_back(c);
    }
    g_wallShape = std::make_unique<nox::Server>();
    g_wallShape->SetWallTiles(tiles);
    g_navBodies.clear();
    int objCount = 0;
    const int objCells = objects ? BakeNavObstacles(mf, thingPath, objCount) : 0;
    if (!objects)
        std::printf("[더미] 맵 물체 안 찍음(--no-nav-objects · 대조군) — 벽만 돌아간다\n");
    else if (objCells < 0)
        std::printf("[더미] thing.bin 을 못 읽음(%s) — 맵 물체 없이 벽만 돌아간다\n", thingPath.c_str());
    else
        std::printf("[더미] 맵 물체 %d개 · 새로 막은 칸 %d칸\n", objCount, objCells);
    g_nav.ok = true;
    int free = 0, linked = 0, pairs = 0;
    std::vector<Point> tmp;
    for (int i = 0; i < kWaypointCount; ++i) {
        if (!CellBlocked(kWaypoints[i].x / kNavCell, kWaypoints[i].y / kNavCell)) ++free;
        for (int j = i + 1; j < kWaypointCount; ++j) {
            ++pairs;
            if (FindPath(kWaypoints[i].x, kWaypoints[i].y, kWaypoints[j].x, kWaypoints[j].y, tmp)) ++linked;
        }
    }
    std::printf("[더미] 벽 격자 %zu칸 · 시작 지점 %d/%d 빈칸 · 이어진 쌍 %d/%d — %s\n", cells.size(), free, kWaypointCount, linked, pairs, path.c_str());
    std::fflush(stdout);
    return true;
}

// 길 위에서 지금 향할 점 — 다다른 점은 넘기고, 앞 몇 칸 안에서 곧장 갈 수 있는 가장 먼 점. FollowPath 와 --nav-probe 가 같이 쓴다.
// ★ 11-12 A — 23 안의 점을 무조건 넘기던 때, 벽 모서리에 몸이 붙으면 꺾이는 점(20.2 거리)을 넘기고 그 너머 점을 노려
//   벽 선을 가로지르는 줄로 달렸다(대조군 벽 멈춤 셋 모두 · --nav-probe 「넘긴 끝 2 · 서버 가운데 줄 0」). 그래서 넘기기 · 앞 점 고르기 둘 다
//   다음 점까지 몸 폭(BodyClear)으로 트였을 때만 한다. 꺾이는 점에 먼저 가서 돌아 나간다.
int PickPathPoint(const std::vector<Point>& path, int& pathPos, int x, int y) {
    const int last = static_cast<int>(path.size()) - 1;
    auto distTo = [&](int k) {
        const double dx = static_cast<double>(path[static_cast<size_t>(k)].x - x), dy = static_cast<double>(path[static_cast<size_t>(k)].y - y);
        return std::sqrt(dx * dx + dy * dy);
    };
    while (pathPos < last) {
        const double dd = distTo(pathPos);
        if (dd >= kPathStepArrive) break;
        if (dd >= kPathPointReached && !BodyClear(x, y, path[static_cast<size_t>(pathPos + 1)].x, path[static_cast<size_t>(pathPos + 1)].y)) break;
        ++pathPos;
    }
    int pick = pathPos;
    for (int k = std::min(pathPos + kLookAheadCells, last); k > pathPos; --k) {
        if (GridClear(x, y, path[static_cast<size_t>(k)].x, path[static_cast<size_t>(k)].y) &&
            BodyClear(x, y, path[static_cast<size_t>(k)].x, path[static_cast<size_t>(k)].y)) {
            pick = k;
            break;
        }
    }
    return pick;
}

// 길을 따라 달린다 — 찾은 길에서 곧장 갈 수 있는 가장 먼 점을 향한다(칸을 하나씩 밟으면 지그재그가 된다).
// 길을 못 찾으면 곧장 간다.
void FollowPath(Dummy& d, int tx, int ty) {
    if (d.path.empty() || FrameReached(d, d.pathReplanFrame)) {
        // 길이 없으면 상대를 곧장 향하는 점 하나로 채운다 — 빈 채로 두면 매 프레임 칸 전체를 다시 훑어 더미 쪽 시간이 는다
        if (!FindPath(d.x, d.y, tx, ty, d.path)) d.path.assign(1, Point{tx, ty});
        d.pathPos = 0;
        d.pathReplanFrame = d.frame + kReplanFrames;
    }
    const int pick = PickPathPoint(d.path, d.pathPos, d.x, d.y);
    MoveToward(d, d.path[static_cast<size_t>(pick)].x, d.path[static_cast<size_t>(pick)].y);
}

// 막혔을 때 들를 지점 — 상대에 가장 가까운 지점. 바로 전 지점과 내 발밑 지점은 뺀다.
int PickWaypoint(const Dummy& d, int tx, int ty) {
    int best = -1;
    double bestDist = 0.0;
    for (int i = 0; i < kWaypointCount; ++i) {
        if (i == d.lastDetour || DistTo(d, kWaypoints[i].x, kWaypoints[i].y) < kDetourArrive * 2) continue;
        const double dx = static_cast<double>(kWaypoints[i].x - tx), dy = static_cast<double>(kWaypoints[i].y - ty);
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (best < 0 || dist < bestDist) { best = i; bestDist = dist; }
    }
    return best;
}

// 막혔다 — 시작 지점 하나를 잠깐 들른다.
void StartDetour(Dummy& d, int tx, int ty) {
    const int w = PickWaypoint(d, tx, ty);
    d.reachTarget = 0;
    if (w < 0) return;
    d.detourIndex = w;
    d.detourEndFrame = d.frame + kDetourMaxFrames;
    d.path.clear();
    ++t_count->detours;
    ++d.detourRun;
    if (d.detourRun == kDetourRunWarn) ++t_count->detourRun5;
    if (d.detourRun == kDetourRunStall) ++t_count->detourRun20;
    // 자리를 같이 찍는다 — 고친 뒤에도 남는 것이 한 자리에 몰리는지(맵 물체 · 벽 모양) 흩어지는지 가르려고
    if ((d.detourRun == kDetourRunWarn || d.detourRun == kDetourRunStall) &&
        g_detourRunShown.fetch_add(1, std::memory_order_relaxed) < kDetourRunShowMax) {
        std::printf("[더미 우회 연속] 더미 %d · %d번째 · 자리 (%d,%d) · 상대 (%d,%d) · 지점 %d\n",
                    d.index, d.detourRun, d.x, d.y, tx, ty, w);
        std::fflush(stdout);
    }
    FollowPath(d, kWaypoints[w].x, kWaypoints[w].y);
}

// 커서를 상대 위에 둔다 — 주문은 끝나는 순간의 커서로 대상을 정한다(Server.cpp:6842-6849).
void AimAt(Dummy& d, int x, int y) {
    d.facing = FacingTo(x - d.x, y - d.y);
    d.out.push_back(static_cast<uint8_t>(Op::MOUSE));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(x)));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(y)));
    ++t_count->sentMouse;
}

// 쫓아간다 — 벽이 없으면 곧장, 있으면 길 따라. 3초에 몸이 못 움직였으면 지점을 들른다.
// 곧장은 몸 폭으로도 트였을 때만(11-12 A — 칸 격자만 보면 벽 모서리 · 물체를 스치는 줄로 달려 걸린다)
void ChaseStep(Dummy& d, const OtherPlayer& t, bool clear) {
    d.reachTarget = 0;
    if (FrameReached(d, d.stuckCheckFrame)) {
        if (d.stuckHave && DistTo(d, d.stuckX, d.stuckY) < kStuckMinGain) {
            d.stuckHave = false;
            d.stuckCheckFrame = d.frame + kStuckCheckFrames;
            StartDetour(d, t.x, t.y);
            return;
        }
        d.stuckHave = true;
        d.stuckX = d.x;
        d.stuckY = d.y;
        d.stuckCheckFrame = d.frame + kStuckCheckFrames;
    }
    if (clear && BodyClear(d.x, d.y, t.x, t.y)) {
        d.path.clear();
        MoveToward(d, t.x, t.y);
    } else {
        FollowPath(d, t.x, t.y);
    }
}

// 근접 — 거리 안이면 때리고 밖이면 쫓는다. 전사와 마나가 떨어진 법사가 같이 쓴다.
void MeleeStep(Dummy& d, const Args& a, const OtherPlayer& t, double dist, bool clear) {
    if (dist > kMeleeReach || !clear) {
        ChaseStep(d, t, clear);
        return;
    }
    d.facing = FacingTo(t.x - d.x, t.y - d.y);
    d.stuckHave = false;
    // ★ 벽을 사이에 두고 붙으면 판정은 「닿음」이어도 피해가 안 들어간다(MapTraceRay · Server.cpp:17523).
    //   벽 격자로 거의 가려내지만 격자에 없는 것(잠긴 문 · 정적 물체)이 사이에 있을 수 있어 상대 체력이 안 줄면 자리를 옮긴다.
    if (d.reachTarget != t.net) {
        d.reachTarget = t.net;
        d.reachSinceFrame = d.frame;
    }
    if (d.hurtNet == t.net && static_cast<int32_t>(d.hurtFrame - d.reachSinceFrame) > 0) d.reachSinceFrame = d.hurtFrame;
    if (a.behavior == Behavior::Fight && FrameReached(d, d.reachSinceFrame + kNoHurtFrames)) {
        StartDetour(d, t.x, t.y);
        return;
    }
    if (a.behavior == Behavior::Fight && FrameReached(d, d.attackReadyFrame)) {
        d.wantClick = true;
        ++t_count->sentAttack;
        d.attackReadyFrame = d.frame + a.AttackGap();
    }
}

// 전사 능력 — 재사용이 끝났고 거리가 맞으면 보낸다. 시야 150 보다 먼저 본다(돌진 · 작살은 쫓는 중에 쓴다 · 플랜 2-5절).
void MaybeAbility(Dummy& d, const OtherPlayer& t, double dist, bool clear) {
    if (!clear || !FrameReached(d, d.abilGapEndFrame)) return;
    for (const AbilityInfo& ab : kAbilities) {
        if (!FrameReached(d, d.abilReadyFrame[ab.id]) || dist < ab.minDist || dist > ab.maxDist) continue;
        d.facing = FacingTo(t.x - d.x, t.y - d.y);   // 입력이 메시지보다 먼저 실려 이 방향을 본 뒤에 능력이 들어간다
        d.out.push_back(static_cast<uint8_t>(Op::TRY_ABILITY));
        d.out.push_back(ab.id);
        ++t_count->sentAbility;
        if (ab.id == 1) ++t_count->sentBerserk;
        else if (ab.id == 2) ++t_count->sentRoar;
        else ++t_count->sentHarpoon;
        d.abilReadyFrame[ab.id] = d.frame + ab.delay + kAbilityMargin;
        d.abilGapEndFrame = d.frame + kAbilityGapFrames;
        return;
    }
}

void CountCast(const SpellInfo& sp) {
    switch (sp.id) {
        case 51: ++t_count->castProtect; break;
        case 36: ++t_count->castHaste; break;
        case 39: ++t_count->castInvis; break;
        case 12: ++t_count->castConfuse; break;
        case 27: ++t_count->castFireball; break;
        case 16: ++t_count->castDeathRay; break;
        case 43: ++t_count->castChain; break;
        case 4: ++t_count->castBlink; break;
        case 41: ++t_count->heals; break;
        case 58: ++t_count->castPixie; break;
        case 60: ++t_count->castPoison; break;
        case 74: ++t_count->castStun; break;
        case 29: ++t_count->castFist; break;
        case 52: ++t_count->castMeteor; break;
        default:
            if (sp.id >= 75 && sp.id <= 114) ++t_count->castSummon;
            break;
    }
}

// 79 <주문 u32> <0 u32 × 4> <대상 깃발 u8> 22바이트(Server.cpp:4151-4163).
void CastSpell(Dummy& d, const Args& a, const SpellInfo& sp, uint32_t holdFrames) {
    const size_t at = d.out.size();
    d.out.resize(at + 22, 0);
    d.out[at] = static_cast<uint8_t>(Op::TRY_SPELL);
    d.out[at + 1] = static_cast<uint8_t>(sp.id);
    d.out[at + 21] = sp.selfFlag;
    ++t_count->sentSpell;
    CountCast(sp);
    if (d.firstCastAfterRevive) {
        d.firstCastAfterRevive = false;
        if (&sp == &kProtect || &sp == &kHaste || &sp == &kInvis) ++t_count->firstBuffOk;
        else ++t_count->firstBuffBad;
    }
    d.spellReadyFrame = d.frame + a.SpellGap();
    d.castHoldEndFrame = d.frame + holdFrames;
}

bool EnemyInvisible(const Dummy& d, uint16_t net) {
    for (const auto& e : d.enchants)
        if (e.first == net) return (e.second & kBuffBitInvis) != 0;
    return false;
}

// 도망갈 지점 — 상대 반대쪽 지점 중 상대에서 가장 먼 곳. 반대쪽에 없으면 전체에서 고른다.
// 곧장 뒤로 달리면 저택 벽에 막혀 4~6초를 못 채우므로 길 찾기가 되는 지점으로 간다.
int PickRetreatPoint(const Dummy& d, int ex, int ey) {
    for (int pass = 0; pass < 2; ++pass) {
        int best = -1;
        double bestDist = 0.0;
        for (int i = 0; i < kWaypointCount; ++i) {
            const Point w = kWaypoints[i];
            if (DistTo(d, w.x, w.y) < kDetourArrive * 2) continue;
            const double dot = static_cast<double>(w.x - d.x) * (d.x - ex) + static_cast<double>(w.y - d.y) * (d.y - ey);
            if (pass == 0 && dot <= 0.0) continue;
            const double dx = static_cast<double>(w.x - ex), dy = static_cast<double>(w.y - ey);
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (best < 0 || dist > bestDist) { best = i; bestDist = dist; }
        }
        if (best >= 0) return best;
    }
    return -1;
}

// 주문 하나를 고른다 — 자기 강화 → 점멸(붙었을 때) → 방해 → 공격(원본 기준 GAME4_3.c:1841-1844). 보냈으면 true.
bool TryCast(Dummy& d, const Args& a, const OtherPlayer& t, double dist, bool inSight) {
    if ((d.selfBits & kSelfBuffMask) == 0) {
        const int start = static_cast<int>(d.rng() % 3);
        for (int k = 0; k < 3; ++k) {
            const SpellInfo& sp = *kBuffs[(start + k) % 3];
            if (d.mana < sp.mana) continue;
            CastSpell(d, a, sp, kCastHoldFrames);
            return true;
        }
    }
    if (dist < kBlinkIn && FrameReached(d, d.blinkReadyFrame) && d.mana >= kBlink.mana) {
        CastSpell(d, a, kBlink, kCastHoldFrames);
        d.blinkReadyFrame = d.frame + d.rng() % (kBlinkRestMaxFrames + 1);
        return true;
    }
    if (!inSight) return false;
    AimAt(d, t.x, t.y);
    if (FrameReached(d, d.confuseReadyFrame) && !EnemyInvisible(d, t.net) && d.mana >= kConfuse.mana) {
        CastSpell(d, a, kConfuse, kCastHoldFrames);
        d.confuseReadyFrame = d.frame + kConfuseMinFrames + d.rng() % (kConfuseMaxFrames - kConfuseMinFrames + 1);
        return true;
    }
    if (d.attackSpell && d.mana >= d.attackSpell->mana) {
        CastSpell(d, a, *d.attackSpell, d.attackSpell == &kChain ? kChainHoldFrames : kCastHoldFrames);
        return true;
    }
    ++t_count->spellSkippedNoMana;
    d.spellReadyFrame = d.frame + a.SpellGap();
    return false;
}

// 소환 — 크기 합에 여유가 있고 앞 소환이 끝났으면 보낸다(플랜 2-7절). 보냈으면 true.
bool TrySummon(Dummy& d, const Args& a) {
    const SummonKind& k = kSummonKinds[d.summonKind];
    if (!FrameReached(d, d.summonEndFrame)) return false;
    if (static_cast<int>(d.pets.size()) * k.size + k.size > kSummonSizeLimit || d.mana < k.spell.mana) return false;
    CastSpell(d, a, k.spell, kCastHoldFrames);
    d.summonEndFrame = d.frame + k.frames + kSummonMarginFrames;
    return true;
}

// 컨저러 주문 하나를 고른다 — 소환 → 자기 강화 → 점멸(붙었을 때) → 방해 → 공격. 보냈으면 true.
bool TryCastConjurer(Dummy& d, const Args& a, const OtherPlayer& t, double dist, bool inSight) {
    if (TrySummon(d, a)) return true;
    if (FrameReached(d, d.pixieReadyFrame) && d.mana >= kPixie.mana) {
        CastSpell(d, a, kPixie, kCastHoldFrames);
        d.pixieReadyFrame = d.frame + d.rng() % (kPixieRestMaxFrames + 1);
        return true;
    }
    if (dist < kBlinkIn && FrameReached(d, d.blinkReadyFrame) && d.mana >= kBlink.mana) {
        CastSpell(d, a, kBlink, kCastHoldFrames);
        d.blinkReadyFrame = d.frame + d.rng() % (kBlinkRestMaxFrames + 1);
        return true;
    }
    if (!inSight) return false;
    AimAt(d, t.x, t.y);
    if (FrameReached(d, d.confuseReadyFrame) && !EnemyInvisible(d, t.net) && d.mana >= kPoison.mana) {   // 독 · 스턴 마나는 같다
        CastSpell(d, a, d.rng() % 2 ? kPoison : kStun, kCastHoldFrames);
        d.confuseReadyFrame = d.frame + kConfuseMinFrames + d.rng() % (kConfuseMaxFrames - kConfuseMinFrames + 1);
        return true;
    }
    if (d.attackSpell && d.mana >= d.attackSpell->mana) {
        CastSpell(d, a, *d.attackSpell, kCastHoldFrames);
        return true;
    }
    ++t_count->spellSkippedNoMana;
    d.spellReadyFrame = d.frame + a.SpellGap();
    return false;
}

// 법사 · 컨저러(플랜 2-6절) — 후퇴 → 주문 → 맨손 · 거리 유지 순서.
void DecideCaster(Dummy& d, const Args& a, const OtherPlayer& t, double dist, bool clear) {
    if (!FrameReached(d, d.castHoldEndFrame)) {   // 음소가 끝날 때까지
        AimAt(d, t.x, t.y);
        return;
    }
    const bool fight = a.behavior == Behavior::Fight;
    if (fight && !d.attackSpell) {               // 부활마다 하나(원본 기준 GAME4.c:2035-2045)
        const int r = static_cast<int>(d.rng() % 100);
        if (d.cls == 2) d.attackSpell = r < 50 ? &kFist : &kMeteor;
        else d.attackSpell = r < 33 ? &kFireball : (r < 66 ? &kDeathRay : &kChain);
    }
    if (fight && !d.retreating && d.hp > 0 && d.hp * 100 <= d.maxHp * kRetreatPercent) {
        d.retreating = true;
        d.retreatRunEndFrame = d.frame + kRetreatMinFrames + d.rng() % (kRetreatMaxFrames - kRetreatMinFrames + 1);
        d.retreatPoint = -1;
        d.backingOff = false;
        d.path.clear();
        ++t_count->retreats;
    }
    if (d.retreating) {
        if (!FrameReached(d, d.retreatRunEndFrame)) {
            // 후퇴 중에는 점멸을 먼저 본다(원본 기준 GAME5.c:260)
            if (FrameReached(d, d.spellReadyFrame) && FrameReached(d, d.blinkReadyFrame) && d.mana >= kBlink.mana) {
                CastSpell(d, a, kBlink, kCastHoldFrames);
                d.blinkReadyFrame = d.frame + d.rng() % (kBlinkRestMaxFrames + 1);
                return;
            }
            if (d.retreatPoint < 0 || DistTo(d, kWaypoints[d.retreatPoint].x, kWaypoints[d.retreatPoint].y) < kDetourArrive) {
                d.retreatPoint = PickRetreatPoint(d, t.x, t.y);
                d.path.clear();
            }
            if (d.retreatPoint >= 0) FollowPath(d, kWaypoints[d.retreatPoint].x, kWaypoints[d.retreatPoint].y);
            return;
        }
        if (d.hp * 100 >= d.maxHp * kRecoverPercent) {
            d.retreating = false;
            ++t_count->healedBack;
        } else if (d.mana < kHeal.mana) {
            d.retreating = false;                // 치유할 마나가 없다 — 맨손으로 돌아간다
        } else {
            if (FrameReached(d, d.spellReadyFrame)) CastSpell(d, a, kHeal, kCastHoldFrames);
            return;
        }
    }
    const bool inSight = dist <= kSightRange && clear;
    if (fight && FrameReached(d, d.spellReadyFrame) && (d.cls == 2 ? TryCastConjurer(d, a, t, dist, inSight) : TryCast(d, a, t, dist, inSight))) return;
    if (fight && d.attackSpell && d.mana < d.attackSpell->mana) {   // 마나를 다 썼다 — 물러나지 않고 맨손으로
        d.backingOff = false;
        MeleeStep(d, a, t, dist, clear);
        return;
    }
    if (!inSight) {
        d.backingOff = false;
        ChaseStep(d, t, clear);
        return;
    }
    d.stuckHave = false;
    if (d.backingOff && dist >= kBackOffTo) d.backingOff = false;
    else if (!d.backingOff && dist < kBackOffIn) d.backingOff = true;
    if (d.backingOff) {
        MoveToward(d, d.x + (d.x - t.x) * 4, d.y + (d.y - t.y) * 4);
        return;
    }
    AimAt(d, t.x, t.y);
}

// 이번 바퀴의 행동. 싸움은 판 맵에서만 — 대기 맵은 채팅 모드다(Server.cpp:4176).
void Decide(Dummy& d, const Args& a) {
    d.wantMove = d.wantClick = false;
    if (a.behavior == Behavior::Idle || d.mapName.empty() || d.mapName == d.waitMapName) return;
    if (d.dead) {
        if (FrameReached(d, d.reviveClickFrame)) {
            d.wantClick = true;
            ++t_count->sentRevive;
            d.reviveClickFrame = d.frame + kReviveGapFrames;
        }
        return;
    }
    const OtherPlayer* t = NearestTarget(d);
    if (FrameReached(d, d.quietNextFrame)) {
        // 60초마다 한 번 — 자리 · 상대 · 그때 하던 일을 같이 찍어 「어디서 무엇을 하다」 끊겼는지 가른다
        ++t_count->quiet60;
        d.quietNextFrame = d.frame + kQuietFrames;
        if (g_quietShown.fetch_add(1, std::memory_order_relaxed) < kQuietShowMax) {
            std::printf("[더미 싸움 끊김] 더미 %d(직업 %d) · %u초째 · 자리 (%d,%d) · 상대 %s(%d,%d) 거리 %.0f · 곧장 %d · 길 %zu점 · 우회 지점 %d · 연속 %d · 후퇴 %d · 물러남 %d\n",
                        d.index, d.cls, (d.frame - d.fightFrame) / 30, d.x, d.y, t ? "" : "없음", t ? t->x : 0, t ? t->y : 0,
                        t ? DistTo(d, t->x, t->y) : 0.0, t ? (GridClear(d.x, d.y, t->x, t->y) ? 1 : 0) : -1, d.path.size(),
                        d.detourIndex, d.detourRun, d.retreating ? 1 : 0, d.backingOff ? 1 : 0);
            std::fflush(stdout);
        }
    }
    if (!t) return;
    if (d.detourIndex >= 0) {
        const Point w = kWaypoints[d.detourIndex];
        if (!FrameReached(d, d.detourEndFrame) && DistTo(d, w.x, w.y) > kDetourArrive) {
            FollowPath(d, w.x, w.y);
            return;
        }
        d.lastDetour = d.detourIndex;
        d.detourIndex = -1;
        d.path.clear();
        d.stuckHave = false;
        d.stuckCheckFrame = d.frame;
    }
    const double dist = DistTo(d, t->x, t->y);
    const bool clear = GridClear(d.x, d.y, t->x, t->y);
    if (d.cls == 0) {
        if (a.behavior == Behavior::Fight) MaybeAbility(d, *t, dist, clear);
        MeleeStep(d, a, *t, dist, clear);
    } else {
        DecideCaster(d, a, *t, dist, clear);
    }
}

void FlushOut(Dummy& d, std::vector<uint8_t>& scratch) {
    scratch.clear();
    PutInput(scratch, d.facing, d.wantMove, d.wantClick);
    ++t_count->sentInput;
    // 입력을 맨 앞에 두고 쌓인 메시지를 잇는다. 상한을 넘으면 나눠 보낸다(메시지 경계에서).
    size_t pos = 0;
    while (pos < d.out.size()) {
        const size_t msz = nox::MessageSize(d.out.data() + pos, d.out.size() - pos, /*fromClient=*/true);
        if (msz == 0 || pos + msz > d.out.size()) {
            ReportDecodeFail(d, d.out.data() + pos, d.out.size() - pos, "보낼 메시지");
            break;
        }
        if (scratch.size() + msz > kMaxOutBody) {
            SendGame(d, scratch.data(), scratch.size());
            scratch.clear();
        }
        scratch.insert(scratch.end(), d.out.begin() + static_cast<std::ptrdiff_t>(pos),
                       d.out.begin() + static_cast<std::ptrdiff_t>(pos + msz));
        pos += msz;
    }
    d.out.clear();
    if (!scratch.empty()) SendGame(d, scratch.data(), scratch.size());
}

// ★ 11-11 부하 측정 M0 조각 8 — 방 여닫기(측정 플랜 M7). 끝날 때 닫기(RunWorker)와 같은 꼴로 `0a` 를 보내고 1초 받기만 한 뒤,
//   판 상태를 처음으로 되돌려 다시 들어온다. 소켓은 그대로 쓴다 — 서버는 `0a` 로 나간 세션을 주소 표에서 지워서
//   (`UdpServer.cpp` `RequestDisconnectSession`) 같은 주소의 다음 접속 시도는 새 세션이다. 정품 클라도 판을 나갔다 다시 들어올 때 프로그램은 그대로다.
void LeaveForCycle(Dummy& d, int64_t nowUs, const Args& a) {
    const uint8_t closeMsg[1] = {static_cast<uint8_t>(Op::CLIENT_CLOSE)};
    SendGame(d, closeMsg, sizeof(closeMsg));
    ++t_count->cycleLeave;
    --t_count->entered;
    d.phase = Phase::Leaving;
    d.leaveDoneUs = nowUs + kCloseDrainUs;
    d.rejoinAtUs = d.cycleAtUs + kCycleRejoinUs + static_cast<int64_t>(d.cyclePos) * a.joinIntervalMs * 1000;
    d.cycleAtUs += static_cast<int64_t>(a.cycleSec) * 1000000;
}

// 판 상태를 처음으로 — main 이 차린 값(직업 · 소환 크기 · 최대 체력)과 소켓 · 방 여닫기 값은 그대로 둔다. 난수는 이어서 쓴다(실행끼리 같다)
void ResetForRejoin(Dummy& d) {
    Dummy fresh;
    fresh.index = d.index;
    fresh.cls = d.cls;
    fresh.sock = d.sock;
    fresh.summonKind = d.summonKind;
    fresh.rng = d.rng;
    fresh.maxHp = d.maxHp;
    fresh.maxMana = d.maxMana;
    fresh.hp = d.maxHp;
    fresh.mana = d.maxMana;
    fresh.maxPetSize = d.maxPetSize;     // [더미 소환] 줄은 실행 전체의 최댓값
    fresh.cycleAtUs = d.cycleAtUs;
    fresh.cyclePos = d.cyclePos;
    fresh.joinAtUs = d.rejoinAtUs;
    fresh.seenImportant.assign(65536, false);
    d = std::move(fresh);
}

void Step(Dummy& d, int64_t nowUs, const Args& a, std::vector<uint8_t>& recvBuf, std::vector<uint8_t>& scratch) {
    if (d.cycleAtUs != 0 && nowUs >= d.cycleAtUs) {   // 방 여닫기 — 판 안이면 나간다. 아직 다시 못 들어왔으면 이번 차례는 건너뛰고 센다
        if (d.phase == Phase::Game) { LeaveForCycle(d, nowUs, a); return; }
        ++t_count->cycleSkipped;
        d.cycleAtUs += static_cast<int64_t>(a.cycleSec) * 1000000;
    }
    switch (d.phase) {
        case Phase::Leaving:
            DrainSocket(d, recvBuf);             // 받은 조각에 대한 답은 `d.out` 에 쌓일 뿐 나가지 않는다(끝날 때 닫기와 같다)
            if (nowUs >= d.leaveDoneUs) ResetForRejoin(d);
            return;
        case Phase::WaitTurn:
            if (nowUs < d.joinAtUs) return;
            SendTryJoin(d);
            d.phase = Phase::TryJoin;
            d.retryAtUs = nowUs + kJoinRetryUs;
            return;
        case Phase::TryJoin:
            DrainSocket(d, recvBuf);
            if (d.phase == Phase::TryJoin && nowUs >= d.retryAtUs) {
                SendTryJoin(d);
                d.retryAtUs = nowUs + kJoinRetryUs;
            }
            return;
        case Phase::Connect:
            DrainSocket(d, recvBuf);
            if (d.phase == Phase::Connect && nowUs >= d.retryAtUs) {
                const uint8_t connect[3] = {0xff, 0x00, 0x00};
                SendRaw(d, connect, sizeof(connect));
                d.retryAtUs = nowUs + kJoinRetryUs;
            }
            return;
        case Phase::Game:
            break;
    }
    DrainSocket(d, recvBuf);
    if (nowUs >= d.pingAtUs) {                   // 정품 핑은 7바이트 혼자 나간다
        const uint32_t t = static_cast<uint32_t>(nowUs / 1000);
        const uint8_t ping[5] = {static_cast<uint8_t>(Op::CLIENT_PING), static_cast<uint8_t>(t), static_cast<uint8_t>(t >> 8),
                                 static_cast<uint8_t>(t >> 16), static_cast<uint8_t>(t >> 24)};
        SendGame(d, ping, sizeof(ping));
        ++t_count->sentPing;
        d.pingAtUs += kPingUs;
    }
    MaybeStart(d, nowUs, a);
    Decide(d, a);
    FlushOut(d, scratch);
}

void WaitUntilUs(HANDLE timer, int64_t targetUs) {
    const int64_t now = NowUs();
    if (targetUs <= now) return;
    LARGE_INTEGER due;
    due.QuadPart = -(targetUs - now) * 10;       // 100ns 단위 · 음수 = 지금부터
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

// busyAvgMs · busyMaxMs 가 0 이상이면 줄 끝에 「바퀴 일한 시간」을 붙인다(1초 줄만 · 요약 줄은 안 붙인다 — 요약은 `이름=값` 을 통째로 읽는 곳이 있다)
void PrintCounters(const char* head, int sec, int entered, int total, const Counters& c, double busyAvgMs = -1.0, double busyMaxMs = -1.0) {
    std::printf("%s 초 %d · 입장 %d/%d", head, sec, entered, total);
#define X(n) std::printf(" %s=%llu", #n, static_cast<unsigned long long>(c.n));
    NOX_DUMMY_COUNTERS(X)
#undef X
    if (busyAvgMs >= 0.0) std::printf(" · 바퀴 일한 시간 평균 %.2fms · 최대 %.2fms", busyAvgMs, busyMaxMs);
    std::printf("\n");
    std::fflush(stdout);
}

Counters Delta(const Counters& now, const Counters& before) {
    Counters d;
#define X(n) d.n = now.n - before.n;
    NOX_DUMMY_COUNTERS(X)
#undef X
    return d;
}

// ★ 11-11 부하 측정 M0 조각 7 — 더미 스레드 하나의 몫. 스레드끼리 함께 쓰는 것은 읽기만 하는 Args · 벽 격자와,
//   더미 배열(저마다 맡은 칸만 만진다)뿐이다. 주 스레드는 셈(count)만 1초마다 읽는다.
struct Worker {
    int index = 0;
    int owned = 0;                                       // 맡은 더미 수
    std::vector<std::vector<int>> slots = std::vector<std::vector<int>>(kWheelSlots);   // 맡은 더미만 1ms 칸에
    LiveCounters count;
    // 한 바퀴에 일한 시간과 33ms 를 넘긴 정도 — lateTicks 가 오른 까닭이 「타이머가 조금 늦게 깨움」인지 「일이 33ms 에 안 들어감」인지 가른다
    uint64_t wheels = 0, busySumUs = 0, busyMaxUs = 0;
    uint64_t overBuckets[5] = {};           // 1ms 이하 · 2ms 이하 · 5ms 이하 · 10ms 이하 · 그 위
    // ★ 11-17 더미 한계(`Docs/MO_DUMMY_LIMIT_PLAN_1117.md` 7-②) — 위 셋은 실행 전체 한 줄이라 입장 구간이 섞인다.
    //   같은 값을 1초 단위로도 센다. 이 스레드가 더하고 주 스레드가 1초마다 0 으로 바꿔 가며 읽는다(그래서 원자 변수).
    //   셋을 따로 바꿔 읽으므로 1초 경계에 걸린 바퀴 하나가 합과 바퀴 수에 다르게 들 수 있다 — 한 초 약 30바퀴 중 하나라 평균이 그만큼 흔들릴 뿐이다
    std::atomic<uint64_t> secWheels{0}, secBusySumUs{0}, secBusyMaxUs{0};
};

// 더미 스레드 — 옛 단일 스레드 고리를 그대로 옮겼다. 시작 시각 t0 와 더미마다의 칸은 스레드 수와 상관없이 같다(--threads 1 이면 옛 꼴).
void RunWorker(Worker& w, std::vector<Dummy>& dummies, const Args& a, int64_t t0, int64_t endAt) {
    t_count = &w.count;
    // 서버와 같은 고해상도 대기 타이머(main.cpp:905) — Sleep(1) 은 약 15.5ms 를 자서 보내는 간격이 흔들린다.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        std::printf("[더미] 스레드 %d 정밀 타이머를 못 만들어 보통 타이머로 기다린다 — 간격이 약 15.6ms 단위로 흔들린다\n", w.index);
    }

    std::vector<uint8_t> recvBuf(65536);
    std::vector<uint8_t> scratch;
    scratch.reserve(2048);
    int64_t base = t0;

    for (;;) {
        int64_t busyUs = 0;
        for (int s = 0; s < kWheelSlots; ++s) {
            if (w.slots[static_cast<size_t>(s)].empty()) continue;
            WaitUntilUs(timer, base + static_cast<int64_t>(s) * 1000);
            const int64_t now = NowUs();
            for (int i : w.slots[static_cast<size_t>(s)]) Step(dummies[static_cast<size_t>(i)], now, a, recvBuf, scratch);
            busyUs += NowUs() - now;
        }
        const int64_t after = NowUs();
        const int64_t next = base + kWheelUs;
        ++w.wheels;
        w.busySumUs += static_cast<uint64_t>(busyUs);
        w.busyMaxUs = std::max(w.busyMaxUs, static_cast<uint64_t>(busyUs));
        w.secWheels.fetch_add(1, std::memory_order_relaxed);
        w.secBusySumUs.fetch_add(static_cast<uint64_t>(busyUs), std::memory_order_relaxed);
        for (uint64_t cur = w.secBusyMaxUs.load(std::memory_order_relaxed);
             static_cast<uint64_t>(busyUs) > cur &&
             !w.secBusyMaxUs.compare_exchange_weak(cur, static_cast<uint64_t>(busyUs), std::memory_order_relaxed);) {
        }
        if (after > next) {
            const int64_t over = after - next;
            ++w.overBuckets[over <= 1000 ? 0 : over <= 2000 ? 1 : over <= 5000 ? 2 : over <= 10000 ? 3 : 4];
        }
        if (after > next + kLateToleranceUs) {   // 한 바퀴가 33ms 를 넘겼다 — 몰아 보내지 않고 지금부터 다시 센다
            ++t_count->lateTicks;
            base = after;
        } else {
            base = next;
        }
        if (after >= endAt) break;
    }

    // ★ 11-11 부하 측정 M0 조각 6 — 끝날 때 접속 닫기(`0a`). 원본 클라 `SendClientClose`(`third_party/opennox-src/src/internal/netstr/conn.go:187-197`)처럼
    //   1바이트 `0a` 를 비신뢰로 보낸다 — 머리는 다른 게임 조각과 같다(`SendGame`). 서버는 접속 제어 조각 맨 앞의 `0a` 를 접속 닫기로 읽는다(`UdpServer.cpp` `CLIENT_CLOSE`).
    //   옛 꼴은 소켓만 닫아 판 안 사람이 90초 뒤 시간 초과로 빠졌다(`Server.h` `kSessionTimeoutMsInGame`) — 방 여닫기(측정 플랜 M7)가 안 되고,
    //   닫은 뒤 서버가 보낸 몫이 「서버가 보낸 조각 대 더미가 받은 조각」 대조에 섞였다.
    //   키를 받기 전(접속 중) 더미는 XOR 키가 없어 못 보낸다 — `closeSkipped` 로 센다(그 몫은 서버에서 30초 뒤 시간 초과).
    //   스레드마다 제 더미만 닫는다(M0 조각 7).
    for (const std::vector<int>& slot : w.slots) {
        for (int i : slot) {
            Dummy& d = dummies[static_cast<size_t>(i)];
            // 방 여닫기로 이미 `0a` 를 보냈거나(Leaving) 서버에 세션이 아직 없다(WaitTurn) — 닫을 것이 없다(M0 조각 8)
            if (d.phase == Phase::Leaving || d.phase == Phase::WaitTurn) continue;
            if (d.phase != Phase::Game) { ++t_count->closeSkipped; continue; }
            const uint8_t closeMsg[1] = {static_cast<uint8_t>(Op::CLIENT_CLOSE)};
            SendGame(d, closeMsg, sizeof(closeMsg));
            ++t_count->sentClose;
        }
    }
    // 그 뒤 받기만 한다(`kCloseDrainUs`). 받은 조각에 대한 답은 `d.out` 에 쌓일 뿐 나가지 않는다 — `FlushOut` 을 안 부른다
    const int64_t drainEnd = NowUs() + kCloseDrainUs;
    for (int64_t at = NowUs(); at < drainEnd; at += 5000) {
        WaitUntilUs(timer, at);
        for (const std::vector<int>& slot : w.slots)
            for (int i : slot) DrainSocket(dummies[static_cast<size_t>(i)], recvBuf);
    }
    CloseHandle(timer);
}

bool ParseArgs(int argc, char** argv, Args& a, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        const auto need = [&](int& out) {
            if (!v) { err = k + " 값 없음"; return false; }
            out = std::atoi(v);
            ++i;
            return true;
        };
        if (k == "--server") {
            if (!v) { err = "--server 값 없음"; return false; }
            std::string s = v;
            ++i;
            const size_t c = s.find(':');
            if (c != std::string::npos) {
                a.port = static_cast<uint16_t>(std::atoi(s.c_str() + c + 1));
                s.resize(c);
            }
            a.host = s;
        } else if (k == "--count") { if (!need(a.count)) return false; }
        else if (k == "--room-size") { if (!need(a.roomSize)) return false; }
        else if (k == "--reserve-seats") { if (!need(a.reserveSeats)) return false; }
        else if (k == "--duration-sec") { if (!need(a.durationSec)) return false; }
        else if (k == "--join-interval-ms") { if (!need(a.joinIntervalMs)) return false; }
        else if (k == "--start-wait-ms") { if (!need(a.startWaitMs)) return false; }
        else if (k == "--stat-mult") { if (!need(a.statMult)) return false; }
        else if (k == "--threads") { if (!need(a.threads)) return false; }
        else if (k == "--cycle-sec") { if (!need(a.cycleSec)) return false; }
        else if (k == "--freq-mult") {
            if (!v) { err = "--freq-mult 값 없음"; return false; }
            a.freqMult = std::atof(v);
            ++i;
        }
        else if (k == "--map-file") {
            if (!v) { err = "--map-file 값 없음"; return false; }
            a.mapFile = v;
            ++i;
        }
        else if (k == "--thing-file") {
            if (!v) { err = "--thing-file 값 없음"; return false; }
            a.thingFile = v;
            ++i;
        }
        else if (k == "--no-nav-objects") a.navObjects = false;
        else if (k == "--behavior") {
            if (!v) { err = "--behavior 값 없음"; return false; }
            const std::string b = v;
            ++i;
            if (b == "idle") a.behavior = Behavior::Idle;
            else if (b == "move") a.behavior = Behavior::Move;
            else if (b == "fight") a.behavior = Behavior::Fight;
            else { err = "--behavior 는 idle · move · fight"; return false; }
        }
        else if (k == "--seed") {
            int s = 0;
            if (!need(s)) return false;
            a.seed = static_cast<uint32_t>(s);
        } else if (k == "--classes") {
            if (!v) { err = "--classes 값 없음"; return false; }
            a.classes.clear();
            for (const char* p = v; *p;) {
                a.classes.push_back(std::atoi(p));
                const char* c = std::strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
            ++i;
        } else {
            err = "모르는 인자 " + k;
            return false;
        }
    }
    if (a.count < 1) { err = "--count 는 1 이상"; return false; }
    if (a.roomSize < 1) { err = "--room-size 는 1 이상"; return false; }
    if (a.reserveSeats < 0 || a.reserveSeats >= a.roomSize) { err = "--reserve-seats 는 0 이상 · 방당 인원 미만"; return false; }
    if (a.count % (a.roomSize - a.reserveSeats) != 0) { err = "더미 수가 (방당 인원 - 비울 자리)의 배수가 아니다"; return false; }
    if (a.statMult < 1 || a.statMult > 10) { err = "--stat-mult 는 1~10(서버와 같은 값)"; return false; }
    if (!(a.freqMult > 0.0) || a.freqMult > 4.0) { err = "--freq-mult 는 0 초과 4 이하"; return false; }
    // 맡은 더미가 없는 스레드는 바퀴에서 기다릴 칸이 없어 쉬지 않고 돈다 — 더미 수를 넘지 못하게 막는다
    if (a.threads < 1 || a.threads > 64 || a.threads > a.count) { err = "--threads 는 1~64 · 더미 수 이하"; return false; }
    if (a.cycleSec != 0) {
        // 비운 자리가 있으면 더미 무리가 서버 방 하나와 맞지 않는다 — 마지막 무리가 나가도 방이 안 빈다
        if (a.reserveSeats != 0) { err = "--cycle-sec 는 --reserve-seats 0 일 때만"; return false; }
        // 한 번 돌기 = 다시 들어오기까지(kCycleRejoinUs) + 입장 간격 × 방 인원 + !start 기다림 — 다음 나감 전에 판이 열려 있게 5초를 더한다
        const int64_t minMs = kCycleRejoinUs / 1000 + static_cast<int64_t>(a.roomSize) * a.joinIntervalMs + a.startWaitMs + 5000;
        if (a.cycleSec < 0 || static_cast<int64_t>(a.cycleSec) * 1000 < minMs) {
            err = "--cycle-sec 는 0(끔) 또는 " + std::to_string((minMs + 999) / 1000) + "초 이상";
            return false;
        }
        if (a.count / a.roomSize < 2) { err = "--cycle-sec 는 방이 2개 이상일 때만(나머지 방이 받는 영향을 잰다)"; return false; }
    }
    if (a.classes.empty()) { err = "--classes 가 비었다"; return false; }
    for (int c : a.classes)
        if (c < 0 || c > 2) { err = "직업은 0 전사 · 1 위저드 · 2 컨저러"; return false; }
    return true;
}

}  // namespace

// ★ 11-12 확인 도구 — 한 자리에서 더미가 무엇을 고르는지 찍는다(서버 없이 몇 초). 길을 새로 찾은 직후의 선택은 「선 자리 + 상대 자리」로
//   정해지므로 멈춘 자리(`[더미 싸움 끊김]` 줄)를 넣으면 그 판의 선택이 다시 나온다. 쓰기: nox_dummy --nav-probe X Y TX TY [--no-nav-objects]
int NavProbe(int argc, char** argv) {
    const int x = std::atoi(argv[2]), y = std::atoi(argv[3]), tx = std::atoi(argv[4]), ty = std::atoi(argv[5]);
    const bool objects = !(argc >= 7 && std::string(argv[6]) == "--no-nav-objects");
    if (!LoadNav(kDefaultMapFile, kDefaultThingFile, objects)) return 1;
    std::printf("[길 확인] 자리 (%d,%d) 칸 (%d,%d) 막힘 %d · 벽 선까지 %.1f · 상대 (%d,%d) · 곧장(칸) %d · 서버 가운데 줄 %d · 몸 폭 %d\n",
                x, y, x / kNavCell, y / kNavCell, CellBlocked(x / kNavCell, y / kNavCell) ? 1 : 0, g_wallShape->DebugWallDistance(x, y), tx, ty,
                GridClear(x, y, tx, ty) ? 1 : 0,
                g_wallShape->DebugTraceRayFlags(static_cast<float>(x), static_cast<float>(y), static_cast<float>(tx), static_cast<float>(ty), 0x5) ? 1 : 0,
                BodyClear(x, y, tx, ty) ? 1 : 0);
    // 막힌 칸이면 둘레 kNavEscapeRing 칸 안의 빈칸 수와 그 가운데 몸 폭으로 트인 칸 수(FindPath 가 쓰는 것과 같은 판정)
    auto ring = [&](int px, int py, bool fromSide, int& freeN, int& clearN) {
        freeN = clearN = 0;
        const int cx0 = px / kNavCell, cy0 = py / kNavCell;
        for (int ry = -kNavEscapeRing; ry <= kNavEscapeRing; ++ry)
            for (int rx = -kNavEscapeRing; rx <= kNavEscapeRing; ++rx) {
                const int cx = cx0 + rx, cy = cy0 + ry;
                if ((rx == 0 && ry == 0) || CellBlocked(cx, cy)) continue;
                ++freeN;
                const int mx = cx * kNavCell + kNavCell / 2, my = cy * kNavCell + kNavCell / 2;
                if (fromSide ? BodyClear(px, py, mx, my) : BodyClear(mx, my, px, py)) ++clearN;
            }
    };
    int sf = 0, sc = 0, gf = 0, gc = 0;
    ring(x, y, true, sf, sc);
    ring(tx, ty, false, gf, gc);
    // 둘레 칸 지도 — # 벽 칸 · x 물체 칸 · . 빈칸 · o 빈칸이고 몸 폭으로 곧장 가는 칸 · 가운데는 대문자(# → @ · x → X · . → +)
    for (int ry = -3; ry <= 3; ++ry) {
        std::string row;
        for (int rx = -3; rx <= 3; ++rx) {
            const int cx = x / kNavCell + rx, cy = y / kNavCell + ry;
            const uint8_t v = (cx < 0 || cy < 0 || cx >= kNavSize || cy >= kNavSize) ? 1 : g_nav.blocked[static_cast<size_t>(cy) * kNavSize + cx];
            char c = v == 1 ? '#' : (v == 2 ? 'x' : '.');
            if (v == 0 && BodyClear(x, y, cx * kNavCell + kNavCell / 2, cy * kNavCell + kNavCell / 2)) c = 'o';
            if (rx == 0 && ry == 0) c = v == 1 ? '@' : (v == 2 ? 'X' : '+');
            row += c;
        }
        std::printf("[길 확인]   칸 지도 %4d  %s\n", (y / kNavCell + ry) * kNavCell, row.c_str());
    }
    std::printf("[길 확인] 출발 둘레 빈칸 %d · 몸 폭 트임 %d / 도착 칸 막힘 %d · 둘레 빈칸 %d · 트임 %d\n", sf, sc,
                CellBlocked(tx / kNavCell, ty / kNavCell) ? 1 : 0, gf, gc);
    std::vector<Point> path;
    const bool ok = FindPath(x, y, tx, ty, path);
    std::printf("[길 확인] 길 %d · %zu점\n", ok ? 1 : 0, path.size());
    for (size_t i = 0; i < path.size() && i < 4; ++i)
        std::printf("[길 확인]   점 %zu (%d,%d) 칸 막힘 %d · 벽 선까지 %.1f\n", i, path[i].x, path[i].y,
                    CellBlocked(path[i].x / kNavCell, path[i].y / kNavCell) ? 1 : 0, g_wallShape->DebugWallDistance(path[i].x, path[i].y));
    if (path.empty()) return 0;
    int pos = 0;
    const int pick = PickPathPoint(path, pos, x, y);
    const Point q = path[static_cast<size_t>(pick)];
    std::printf("[길 확인] 넘긴 끝 %d · 고른 점 %d (%d,%d) · 서버 가운데 줄 %d · 몸 폭 %d\n", pos, pick, q.x, q.y,
                g_wallShape->DebugTraceRayFlags(static_cast<float>(x), static_cast<float>(y), static_cast<float>(q.x), static_cast<float>(q.y), 0x1) ? 1 : 0,
                BodyClear(x, y, q.x, q.y) ? 1 : 0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "--nav-probe") return NavProbe(argc, argv);
    Args a;
    std::string err;
    if (!ParseArgs(argc, argv, a, err)) {
        std::printf("인자 오류: %s\n", err.c_str());
        std::printf("쓰기: nox_dummy [--server 주소:포트] [--count N] [--room-size K] [--reserve-seats R] [--classes 0,1] [--duration-sec S] "
                    "[--join-interval-ms M] [--start-wait-ms M] [--behavior idle|move|fight] [--map-file 대전맵.map] [--thing-file thing.bin] [--no-nav-objects] [--seed S] "
                    "[--stat-mult N] [--freq-mult X] [--threads N] [--cycle-sec S]\n"
                    "     nox_dummy --nav-probe X Y TX TY [--no-nav-objects]   (한 자리의 길 선택만 찍는다)\n");
        return 1;
    }

    nox::SetStatMult(a.statMult);                // 최대 체력은 표에 서버와 같은 배수를 곱한 값이다
    if (a.behavior != Behavior::Idle && !LoadNav(a.mapFile, a.thingFile, a.navObjects))
        std::printf("[더미] 벽 격자를 못 읽음(%s) — 벽을 돌아가지 못하고 곧장 쫓기만 한다\n", a.mapFile.c_str());

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("WSAStartup 실패\n"); return 1; }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(a.port);
    if (inet_pton(AF_INET, a.host.c_str(), &to.sin_addr) != 1) { std::printf("주소를 못 읽음: %s\n", a.host.c_str()); return 1; }

    std::vector<Dummy> dummies(static_cast<size_t>(a.count));
    std::vector<std::unique_ptr<Worker>> workers;
    for (int t = 0; t < a.threads; ++t) {
        workers.push_back(std::make_unique<Worker>());
        workers.back()->index = t;
    }
    // 보내는 순간을 씨앗 난수로 흩뜨린다 — 서로 다른 PC 의 정품 클라는 같은 1ms 에 몰아 보내지 않는다.
    // 씨앗을 고정하므로 실행마다 같은 칸에 든다.
    std::mt19937 slotRng(a.seed);
    const int64_t t0 = NowUs();
    for (int i = 0; i < a.count; ++i) {
        Dummy& d = dummies[static_cast<size_t>(i)];
        d.index = i;
        d.cls = a.classes[static_cast<size_t>(i) % a.classes.size()];
        {   // 같은 방에서 이 더미보다 먼저 든 컨저러 수 — 세 크기를 입장 순서대로 돌린다(5명이면 2 · 2 · 1 · 플랜 1-2절)
            const int perRoom = std::max(1, a.roomSize - a.reserveSeats);
            const int first = i - i % perRoom;
            int before = 0;
            for (int j = first; j < i; ++j)
                if (a.classes[static_cast<size_t>(j) % a.classes.size()] == 2) ++before;
            d.summonKind = before % 3;
        }
        d.rng.seed(a.seed * 1000u + static_cast<uint32_t>(i));
        d.maxHp = nox::HealthOnline(static_cast<nox::CharClass>(d.cls));
        d.maxMana = nox::ManaOnline(static_cast<nox::CharClass>(d.cls));
        d.hp = d.maxHp;
        d.mana = d.maxMana;
        d.joinAtUs = t0 + static_cast<int64_t>(i) * a.joinIntervalMs * 1000;
        if (a.cycleSec > 0 && i >= a.count - a.roomSize) {   // 방 여닫기 — 마지막 방 몫만(비운 자리 0 이라 방 인원 = 무리 크기)
            d.cyclePos = i - (a.count - a.roomSize);
            d.cycleAtUs = t0 + static_cast<int64_t>(a.count - 1) * a.joinIntervalMs * 1000 + static_cast<int64_t>(a.cycleSec) * 1000000;
        }
        d.seenImportant.assign(65536, false);
        d.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (d.sock == INVALID_SOCKET) { std::printf("소켓 실패 %d\n", WSAGetLastError()); return 1; }
        u_long nonBlocking = 1;
        ioctlsocket(d.sock, FIONBIO, &nonBlocking);
        if (connect(d.sock, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) { std::printf("connect 실패 %d\n", WSAGetLastError()); return 1; }
        // 칸은 옛 꼴과 같은 난수 순서로 고르고, 스레드는 번호를 돌려 맡긴다 — 방이 차례로 들어와도 스레드마다 일이 고르다
        Worker& w = *workers[static_cast<size_t>(i % a.threads)];
        w.slots[slotRng() % kWheelSlots].push_back(i);
        ++w.owned;
    }

    const int64_t endAt = t0 + static_cast<int64_t>(a.durationSec) * 1000000;
    std::vector<std::thread> threads;
    threads.reserve(workers.size());
    for (const std::unique_ptr<Worker>& w : workers)
        threads.emplace_back(RunWorker, std::ref(*w), std::ref(dummies), std::cref(a), t0, endAt);

    // 주 스레드는 1초 줄만 찍는다 — 셈을 스레드마다 읽어 더한다. 보통 타이머로 충분하다(15.6ms 흔들림은 1초 줄에 상관없다)
    HANDLE statTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const auto sumCounters = [&] {
        Counters c;
        for (const std::unique_ptr<Worker>& w : workers) w->count.AddTo(c);
        return c;
    };
    const auto sumEntered = [&] {
        uint64_t n = 0;
        for (const std::unique_ptr<Worker>& w : workers) n += w->count.entered.Get();
        return static_cast<int>(n);
    };
    Counters printed;
    int sec = 0;
    for (int64_t nextStat = t0 + 1000000; nextStat <= endAt; nextStat += 1000000) {
        WaitUntilUs(statTimer, nextStat);
        ++sec;
        const Counters now = sumCounters();
        // 그 초의 바퀴 일한 시간 — 스레드마다 (합 ÷ 바퀴 수)와 최대를 내고 스레드 중 큰 것을 찍는다(플랜 7-② · 밀림은 가장 바쁜 스레드에서 먼저 난다)
        double secAvgMs = 0.0, secMaxMs = 0.0;
        for (const std::unique_ptr<Worker>& w : workers) {
            const uint64_t n = w->secWheels.exchange(0, std::memory_order_relaxed);
            const uint64_t sumUs = w->secBusySumUs.exchange(0, std::memory_order_relaxed);
            const uint64_t maxUs = w->secBusyMaxUs.exchange(0, std::memory_order_relaxed);
            if (n) secAvgMs = std::max(secAvgMs, static_cast<double>(sumUs) / static_cast<double>(n) / 1000.0);
            secMaxMs = std::max(secMaxMs, static_cast<double>(maxUs) / 1000.0);
        }
        PrintCounters("[더미]", sec, sumEntered(), a.count, Delta(now, printed), secAvgMs, secMaxMs);
        printed = now;
    }
    for (std::thread& t : threads) t.join();      // 스레드마다 0a 를 보내고 1초 받기까지 끝낸 뒤
    CloseHandle(statTimer);

    const auto countEntered = [&] {
        int n = 0;
        for (const Dummy& d : dummies)
            if (d.phase == Phase::Game) ++n;
        return n;
    };
    // 바퀴 값은 스레드마다 센 것을 더한다 — 바퀴 수 · 33ms 넘김은 합, 평균은 (스레드 하나의) 한 바퀴당, 최대는 스레드 중 가장 큰 것
    uint64_t wheels = 0, busySumUs = 0, busyMaxUs = 0;
    uint64_t overBuckets[5] = {};
    for (const std::unique_ptr<Worker>& w : workers) {
        wheels += w->wheels;
        busySumUs += w->busySumUs;
        busyMaxUs = std::max(busyMaxUs, w->busyMaxUs);
        for (int b = 0; b < 5; ++b) overBuckets[b] += w->overBuckets[b];
    }
    PrintCounters("[더미 요약]", sec, countEntered(), a.count, sumCounters());
    std::printf("[더미 바퀴] 스레드 %d · %llu바퀴 · 일한 시간 평균 %.2fms · 최대 %.2fms · 33ms 넘김 1ms 이하 %llu · 2ms 이하 %llu · 5ms 이하 %llu · 10ms 이하 %llu · 그 위 %llu\n",
                a.threads, static_cast<unsigned long long>(wheels), wheels ? static_cast<double>(busySumUs) / static_cast<double>(wheels) / 1000.0 : 0.0,
                static_cast<double>(busyMaxUs) / 1000.0, static_cast<unsigned long long>(overBuckets[0]),
                static_cast<unsigned long long>(overBuckets[1]), static_cast<unsigned long long>(overBuckets[2]),
                static_cast<unsigned long long>(overBuckets[3]), static_cast<unsigned long long>(overBuckets[4]));
    if (a.threads > 1) {   // 스레드끼리 일이 고른지 — 한 스레드만 밀리면 그 스레드가 맡은 방만 입력이 늦는다
        for (const std::unique_ptr<Worker>& w : workers)
            std::printf("[더미 스레드] %d · 맡은 더미 %d · %llu바퀴 · 일한 시간 평균 %.2fms · 최대 %.2fms · lateTicks %llu\n",
                        w->index, w->owned, static_cast<unsigned long long>(w->wheels),
                        w->wheels ? static_cast<double>(w->busySumUs) / static_cast<double>(w->wheels) / 1000.0 : 0.0,
                        static_cast<double>(w->busyMaxUs) / 1000.0, static_cast<unsigned long long>(w->count.lateTicks.Get()));
    }
    for (const Dummy& d : dummies) {
        if (d.cls != 2) continue;
        const SummonKind& k = kSummonKinds[d.summonKind];
        std::printf("[더미 소환] 더미 %d · 주문 %u · 크기 %d · 거느린 크기 합 최댓값 %d · 끝날 때 %zu마리\n", d.index, k.spell.id, k.size, d.maxPetSize, d.pets.size());
    }
    for (Dummy& d : dummies) closesocket(d.sock);
    WSACleanup();
    return 0;
}
