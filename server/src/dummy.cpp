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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "nox/Alias.h"
#include "nox/GameBalance.h"
#include "nox/MapFile.h"
#include "nox/Opcode.h"
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
constexpr size_t kMaxOutBody = 1000;     // 한 조각에 싣는 본문 상한 — 서버 받기 버퍼 2048(NetworkThread.cpp:218) 안
// 싸움 값 — 프레임은 서버 프레임(33ms). 근거는 Docs/MO_DUMMY_PLAN_1109.md 2-4 · 2-5절.
constexpr double kMeleeReach = 50.0;              // 이 안이면 때린다 — 닿는 거리 55(몸 10 + 롱소드 45 · Server.cpp:17135)보다 조금 안쪽
constexpr uint32_t kAttackGapFrames = 30;         // 칼은 1초에 한 번(사용자와 정한 빈도)
constexpr uint32_t kDeathSkipFrames = 90;         // 죽은 상대를 대상에서 빼는 시간 — 갱신에 시체도 계속 실려서 안 빼면 시체를 쫓는다
constexpr uint32_t kReviveFirstFrames = 50;       // 서버가 클릭을 받는 때: 죽음 30틱 + 15틱 뒤(Server.cpp:5553-5561)
constexpr uint32_t kReviveGapFrames = 30;         // 코드 6 을 한 번만 보내면 놓칠 수 있어 되풀이한다
constexpr double kCursorAhead = 150.0;            // 커서가 몸에서 100 을 넘으면 달린다(Server.h kRunCursorDist)
constexpr uint32_t kStuckCheckFrames = 90;        // 3초마다 몸이 움직였나 본다
constexpr double kStuckMinGain = 20.0;            // 3초에 몸이 이만큼도 못 움직이면 막힌 것으로 본다(길 따라 돌아갈 때는 상대와 거리가 늘 수 있어 몸 움직임으로 본다)
constexpr uint32_t kDetourMaxFrames = 150;        // 지점으로 가는 시간의 상한 — 지점 앞에서도 막힐 수 있다
constexpr double kDetourArrive = 40.0;            // 지점에 이만큼 다가가면 다시 쫓는다
constexpr uint32_t kNoHurtFrames = 200;           // 거리 안인데 이만큼 상대 체력이 안 줄면 벽 너머로 본다 — 부활 무적 150틱(Server.cpp:6397)보다 길게

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
constexpr uint32_t kBuffBitProtect = 1u << 26, kBuffBitHaste = 1u << 9, kBuffBitInvis = 1u << 0;   // 0x5A 비트(Server.cpp:7034 · :6948 · :6951)
constexpr uint32_t kSelfBuffMask = kBuffBitProtect | kBuffBitHaste | kBuffBitInvis;
constexpr uint32_t kSpellGapFrames = 60;          // 어떤 주문이든 2초에 하나(플랜 1-1절)
constexpr uint32_t kCastHoldFrames = 25;          // 음소가 끝날 때까지 커서를 상대 위에 두고 안 달린다 — 가장 긴 보호막이 24틱
constexpr uint32_t kChainHoldFrames = 60;         // 연쇄 번개는 서버가 30프레임 뒤 스스로 끝내는데(Server.cpp:6998) 달리면 끊긴다(:7736)
constexpr double kSightRange = 150.0;             // 이 안의 상대만 주문 대상(원본 기준 GAME4.c:1960)
constexpr double kBackOffIn = 50.0, kBackOffTo = 80.0;   // 상대가 50 안이면 80 까지 물러난다
constexpr double kBlinkIn = 25.0;                 // 25 안이면 점멸(원본 기준 GAME4.c:2020)
constexpr int kRetreatPercent = 25, kRecoverPercent = 80;   // 후퇴 시작 · 복귀(원본 기준 GAME4.c:1949 · :1951)
constexpr uint32_t kRetreatMinFrames = 120, kRetreatMaxFrames = 180;   // 4~6초 도망(원본 기준 GAME5.c:263)
constexpr uint32_t kConfuseMinFrames = 90, kConfuseMaxFrames = 900;   // 혼란은 3~30초 난수로 쉰다(원본 기준 GAME4.c:2033-2034)
constexpr uint32_t kBlinkRestMaxFrames = 180;     // 점멸은 0~6초 난수로 쉰다(원본 기준 GAME4.c:2049-2052)

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
constexpr double kPathStepArrive = 23.0;          // 길의 점에 한 칸만큼 다가가면 다음 점으로
constexpr const char* kDefaultMapFile = "C:\\Program Files\\GOG Galaxy\\Games\\Nox\\maps\\Estate\\Estate.map";

constexpr int64_t kStartRetryUs = 5000000;   // 대기 맵에 그대로 있으면 !start 를 다시 보내는 간격 — 채팅은 비신뢰라 방장 것이 사라지면 그 방이 영영 안 열린다

#define NOX_DUMMY_COUNTERS(X) \
    X(sentInput) X(sentPing) X(sentAck) X(sentAlias) X(sentMouse) X(sentSpell) X(sentAbility) X(sentStart) \
    X(recvDgram) X(recvBytes) X(decodeFail) X(dupImportant) X(lateTicks) X(sendFail) \
    X(petsAcquired) X(petsLost) X(spellSkippedNoMana) X(retreats) X(heals) \
    X(sentAttack) X(sentRevive) X(sawDeath) X(selfDeath) X(revives) X(detours) \
    X(castProtect) X(castHaste) X(castInvis) X(castConfuse) X(castFireball) X(castDeathRay) X(castChain) X(castBlink) \
    X(sentBerserk) X(sentRoar) X(sentHarpoon) X(firstBuffOk) X(firstBuffBad) X(buffOverlap) X(healedBack) X(statChecked) X(statMismatch)

struct Counters {
#define X(n) uint64_t n = 0;
    NOX_DUMMY_COUNTERS(X)
#undef X
};

Counters g_count;
int g_decodeFailShown = 0;

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
};

enum class Phase { WaitTurn, TryJoin, Connect, Game };

struct OtherPlayer {
    uint16_t net = 0;
    int x = 0, y = 0;
};

struct Dummy {
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
    std::vector<OtherPlayer> others;     // 마지막 갱신의 다른 사람들(서버가 거리와 무관하게 싣는다 — Server.cpp:18663)
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
};

void SendRaw(const Dummy& d, const uint8_t* p, size_t n) {
    // 실패를 세는 까닭 — 보낸 입력 합과 서버가 받은 입력 합이 어긋날 때 더미 쪽 소켓에서 사라진 몫을 가르려고
    if (send(d.sock, reinterpret_cast<const char*>(p), static_cast<int>(n), 0) == SOCKET_ERROR) ++g_count.sendFail;
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
    ++g_count.sentAlias;
}

void OnUpdate(Dummy& d, const nox::UpdateMsg& m) {
    // 갱신의 머리는 카메라다(번호 0 · 별칭 01 · Server.cpp:18602-18618) — 자리는 내 자리지만 번호는 안 알려 준다.
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
    ++g_count.statChecked;
    if (got == want) return;
    ++g_count.statMismatch;
    std::printf("[더미 %d] 입장 값 다름 — %s 받음 %d · 기대 %d\n", d.index, what, got, want);
    std::fflush(stdout);
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
            ++g_count.sawDeath;
            if (net == d.selfNet) {
                ++g_count.selfDeath;
                d.dead = true;
                d.reviveClickFrame = d.frame + kReviveFirstFrames;
                d.detourIndex = -1;
                d.stuckHave = false;
                d.selfBits = 0;              // 죽으면 강화가 풀린다 — 5A 가 늦어도 부활 뒤 새로 걸게
                d.retreating = false;
                d.backingOff = false;
                d.castHoldEndFrame = d.frame;
            } else {
                for (size_t k = 0; k < d.skip.size();) {
                    const bool expired = static_cast<int32_t>(d.frame - d.skip[k].second) >= 0;
                    if (expired || d.skip[k].first == net) d.skip.erase(d.skip.begin() + static_cast<std::ptrdiff_t>(k));
                    else ++k;
                }
                d.skip.emplace_back(net, d.frame + kDeathSkipFrames);
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA) && msz >= 5) {
            d.hurtNet = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));   // 42 <번호 2> <증감 2> — 음수만 온다(Server.cpp:6692)
            d.hurtFrame = d.frame;
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH) && msz >= 3) {
            const int hp = p[i + 1] | (p[i + 2] << 8);
            CheckEntryStat(d, d.checkedHp43, "43 체력", hp, d.maxHp);
            d.hp = hp;
            if (d.dead && hp > 0) {
                d.dead = false;
                ++g_count.revives;
                d.attackSpell = nullptr;     // 공격 주문은 부활마다 다시 고른다(원본 기준 GAME4.c:2035-2045)
                d.firstCastAfterRevive = true;
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_MANA) && msz >= 5) {
            // 45 <번호 2> <마나 2>(Server.cpp:6502)
            if (static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8)) == d.selfNet) d.mana = p[i + 3] | (p[i + 4] << 8);
        } else if ((p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH) || p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_MANA)) && msz >= 7) {
            // dd · de <번호 2> <값 2> <값 2> — 입장 꼬리가 싣는 최대치(SnapshotTail.cpp:141 · :144)
            if (static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8)) == d.selfNet) {
                const int v = p[i + 3] | (p[i + 4] << 8);
                if (p[i] == static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH)) CheckEntryStat(d, d.checkedDd, "dd 최대 체력", v, d.maxHp);
                else CheckEntryStat(d, d.checkedDe, "de 최대 마나", v, d.maxMana);
            }
        } else if (p[i] == static_cast<uint8_t>(Op::REPORT_ENCHANTMENT) && msz >= 7) {
            // 5A <번호 2> <비트 4>(Server.cpp:2291-2299) — 모든 사람에게 온다
            const uint16_t net = static_cast<uint16_t>(p[i + 1] | (p[i + 2] << 8));
            const uint32_t bits = static_cast<uint32_t>(p[i + 3]) | (static_cast<uint32_t>(p[i + 4]) << 8) |
                                  (static_cast<uint32_t>(p[i + 5]) << 16) | (static_cast<uint32_t>(p[i + 6]) << 24);
            if (net == d.selfNet) {
                d.selfBits = bits;
                const int on = ((bits & kBuffBitProtect) != 0) + ((bits & kBuffBitHaste) != 0) + ((bits & kBuffBitInvis) != 0);
                if (on >= 2) ++g_count.buffOverlap;
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

void ReportDecodeFail(const Dummy& d, const uint8_t* b, size_t n, const char* why) {
    ++g_count.decodeFail;
    if (g_decodeFailShown >= 20) return;
    ++g_decodeFailShown;
    std::printf("[더미 %d] 못 읽음(%s) op=%02x 남은 %zu:", d.index, why, n ? b[0] : 0, n);
    for (size_t i = 0; i < n && i < 16; ++i) std::printf(" %02x", b[i]);
    std::printf("\n");
    std::fflush(stdout);
}

// 맵이 바뀌면 서버가 다시 세운다(RejoinReset) — 체력 · 마나가 차고 강화 · 재사용이 풀린다.
void ResetForMap(Dummy& d) {
    d.hp = d.maxHp;
    d.mana = d.maxMana;
    d.selfBits = 0;
    d.enchants.clear();
    d.spellReadyFrame = d.frame;
    d.castHoldEndFrame = d.frame;
    d.confuseReadyFrame = d.frame + kConfuseMinFrames + d.rng() % (kConfuseMaxFrames - kConfuseMinFrames + 1);
    d.blinkReadyFrame = d.frame;
    d.attackSpell = nullptr;
    d.firstCastAfterRevive = false;
    d.retreating = false;
    d.backingOff = false;
    d.abilGapEndFrame = d.frame;
    // 서버는 맵이 바뀌어도 능력 쉬는 시간을 되돌리지 않고(되돌리는 곳은 부활 때 `AbilityCancelAll` 뿐) 판 끝 동안은 줄이지도 않아
    // (`AbilityUpdate` 는 사람 걸음 안이라 `frozen` 이면 안 돈다) 더미는 남은 값을 모른다 — 방금 쓴 셈 치고 한 번 기다린다.
    // 전에는 이 자리에서 지웠더니 둘째 판부터 시작 직후에만 「쉬는중」 거절이 났다(실물 d6_4 4건 · d6_10 8건 · 첫 판 0건).
    for (const AbilityInfo& ab : kAbilities) d.abilReadyFrame[ab.id] = d.frame + ab.delay + kAbilityMargin;
}

// 서버→클라 메시지 나누기 — opcount.cpp:133-197 과 같은 방식.
void WalkMessages(Dummy& d, const uint8_t* b, size_t all) {
    size_t eaten = 0;
    while (eaten < all) {
        bool allZero = true;                 // 조각 끝의 0 채움은 메시지가 아니다
        for (size_t i = eaten; i < all; ++i)
            if (b[i] != 0) { allZero = false; break; }
        if (allZero) return;

        const uint8_t op = b[eaten];
        size_t msz = nox::MessageSize(b + eaten, all - eaten, /*fromClient=*/false);

        if (op == static_cast<uint8_t>(Op::IMPORTANT)) {
            // aa 뒤에는 cc 봉투와 봉투 없는 메시지(42 · 45)가 줄 선 순서대로 섞여 온다(Server.cpp:510-518 — 큐 항목을 그대로 잇는다).
            // 머리만 여기서 먹고 뒤는 아래 갈래가 하나씩 읽는다 — 봉투만 훑으면 봉투 없는 것을 만난 뒤의 봉투를 통째로 놓친다.
            msz = 1;
            // 정품은 aa 를 받을 때마다 같은 묶음 27 의 프레임으로 답한다(원본 기준 network_client.go:302-318).
            d.out.push_back(static_cast<uint8_t>(Op::IMPORTANT_ACK));
            Put32(d.out, d.frame);
            ++g_count.sentAck;
        } else if (op == static_cast<uint8_t>(Op::SEQ_IMPORTANT)) {
            if (eaten + 4 > all || eaten + 4 + b[eaten + 3] > all) { ReportDecodeFail(d, b + eaten, all - eaten, "봉투"); return; }
            const size_t inner = b[eaten + 3];
            const uint16_t id = static_cast<uint16_t>(b[eaten + 1] | (b[eaten + 2] << 8));
            if (d.seenImportant[id]) ++g_count.dupImportant;
            else OnImportantBody(d, b + eaten + 4, inner);
            d.seenImportant[id] = true;
            d.seenImportant[static_cast<uint16_t>(id + 0x8000)] = false;   // 번호가 한 바퀴 돌 때를 대비해 반대쪽을 비운다
            msz = 4 + inner;
        } else if (msz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const nox::UpdateMsg m = nox::DecodeUpdate(b + eaten + 1, all - eaten - 1, &d.aliases);
            if (!m.ok) { ReportDecodeFail(d, b + eaten, all - eaten, "갱신"); return; }
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
            // 맵이 바뀌면 서버가 준비를 끈다(Server.cpp:1910) — 준비를 안 보내면 중요 메시지가 멈춘다.
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
            d.path.clear();
            d.stuckHave = false;
            d.aliases.Clear();
            ResetForMap(d);
            const uint8_t ready = static_cast<uint8_t>(Op::CLIENT_READY);
            d.out.push_back(ready);
        } else if (msz != 0 && eaten + msz <= all) {
            OnImportantBody(d, b + eaten, msz);   // 봉투 없이 온 42 · 45 따위
        }

        if (msz == 0 || eaten + msz > all) { ReportDecodeFail(d, b + eaten, all - eaten, "길이"); return; }
        eaten += msz;
    }
}

void OnDatagram(Dummy& d, uint8_t* p, size_t n) {
    ++g_count.recvDgram;
    g_count.recvBytes += n;
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
            // 0x20 + 153바이트. 본문 66번이 직업(GameBalance.h:52 · Server.cpp:4268-4270).
            uint8_t accept[154] = {};
            accept[0] = static_cast<uint8_t>(Op::CLIENT_ACCEPT);
            accept[1 + 66] = static_cast<uint8_t>(d.cls);
            SendGame(d, accept, sizeof(accept));
            d.phase = Phase::Game;
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
// 뒤에 붙는 항목 — 앞으로 = 코드 2 + 데이터 1바이트(서버는 데이터를 안 본다 · Server.cpp:3416) · 클릭 = 코드 6, 데이터 없음(Server.cpp:3437).
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
    ++g_count.sentStart;
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

// 한 지점으로 달린다 — 힘은 늘 바라보는 쪽이라 방향을 돌리고(Server.cpp:3431-3434) 커서를 앞쪽 멀리 둔다(Server.cpp:5613).
void MoveToward(Dummy& d, int tx, int ty) {
    const int dx = tx - d.x, dy = ty - d.y;
    if (dx == 0 && dy == 0) return;
    d.facing = FacingTo(dx, dy);
    d.wantMove = true;
    const double len = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
    d.out.push_back(static_cast<uint8_t>(Op::MOUSE));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(d.x + std::lround(dx * kCursorAhead / len))));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(d.y + std::lround(dy * kCursorAhead / len))));
    ++g_count.sentMouse;
}

// 벽 격자 — 더미가 벽을 돌아가는 데만 쓴다. 서버 물리와 별개이고, 벽이 있는 칸 전체를 막힌 것으로 본다.
// 왜 필요한가 — Estate 는 벽이 456칸인 저택이라 곧장 쫓으면 벽 양쪽에 서서 만나지 못한다(11-10 실물 — 300초 중 약 240초).
struct NavGrid {
    bool ok = false;
    std::vector<uint8_t> blocked;
    std::vector<uint32_t> seen;          // 이번 훑기에 가 봤나(훑을 때마다 지우지 않게 세대 번호로)
    std::vector<int32_t> from;           // 어느 칸에서 왔나
    std::vector<int32_t> queue;
    uint32_t stamp = 0;
};
NavGrid g_nav;

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
bool FindPath(int fromX, int fromY, int toX, int toY, std::vector<Point>& out) {
    out.clear();
    if (!g_nav.ok) return false;
    const int sx = fromX / kNavCell, sy = fromY / kNavCell, gx = toX / kNavCell, gy = toY / kNavCell;
    if (sx < 0 || sy < 0 || gx < 0 || gy < 0 || sx >= kNavSize || sy >= kNavSize || gx >= kNavSize || gy >= kNavSize) return false;
    const int start = sy * kNavSize + sx, goal = gy * kNavSize + gx;
    static const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const uint32_t stamp = ++g_nav.stamp;
    g_nav.queue.clear();
    g_nav.queue.push_back(start);
    g_nav.seen[static_cast<size_t>(start)] = stamp;
    g_nav.from[static_cast<size_t>(start)] = -1;
    for (size_t head = 0; head < g_nav.queue.size() && g_nav.seen[static_cast<size_t>(goal)] != stamp; ++head) {
        const int c = g_nav.queue[head];
        const int cx = c % kNavSize, cy = c / kNavSize;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + kDx[k], ny = cy + kDy[k];
            if (nx < 0 || ny < 0 || nx >= kNavSize || ny >= kNavSize) continue;
            const int n = ny * kNavSize + nx;
            if (g_nav.seen[static_cast<size_t>(n)] == stamp) continue;
            if (n != goal && CellBlocked(nx, ny)) continue;
            if (k >= 4 && (CellBlocked(cx + kDx[k], cy) || CellBlocked(cx, cy + kDy[k]))) continue;
            g_nav.seen[static_cast<size_t>(n)] = stamp;
            g_nav.from[static_cast<size_t>(n)] = c;
            g_nav.queue.push_back(n);
        }
    }
    if (g_nav.seen[static_cast<size_t>(goal)] != stamp) return false;
    for (int c = goal; c != -1; c = g_nav.from[static_cast<size_t>(c)])
        out.push_back(Point{(c % kNavSize) * kNavCell + kNavCell / 2, (c / kNavSize) * kNavCell + kNavCell / 2});
    std::reverse(out.begin(), out.end());
    return true;
}

// 맵을 읽어 격자를 채운다. 시작 지점끼리 길이 이어지는지 세어 함께 찍는다 — 격자가 잘못 깔리면 이 값이 무너진다.
bool LoadNav(const std::string& path) {
    nox::MapFile mf;
    if (!mf.Load(path)) return false;
    const auto cells = mf.ReadWalls();
    g_nav.blocked.assign(static_cast<size_t>(kNavSize) * kNavSize, 0);
    g_nav.seen.assign(static_cast<size_t>(kNavSize) * kNavSize, 0);
    g_nav.from.assign(static_cast<size_t>(kNavSize) * kNavSize, -1);
    for (const auto& w : cells) g_nav.blocked[static_cast<size_t>(w.y) * kNavSize + w.x] = 1;
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

// 길을 따라 달린다 — 찾은 길에서 곧장 갈 수 있는 가장 먼 점을 향한다(칸을 하나씩 밟으면 지그재그가 된다).
// 길을 못 찾으면 곧장 간다.
void FollowPath(Dummy& d, int tx, int ty) {
    if (d.path.empty() || FrameReached(d, d.pathReplanFrame)) {
        // 길이 없으면 상대를 곧장 향하는 점 하나로 채운다 — 빈 채로 두면 매 프레임 칸 전체를 다시 훑어 더미 쪽 시간이 는다
        if (!FindPath(d.x, d.y, tx, ty, d.path)) d.path.assign(1, Point{tx, ty});
        d.pathPos = 0;
        d.pathReplanFrame = d.frame + kReplanFrames;
    }
    const int last = static_cast<int>(d.path.size()) - 1;
    while (d.pathPos < last && DistTo(d, d.path[static_cast<size_t>(d.pathPos)].x, d.path[static_cast<size_t>(d.pathPos)].y) < kPathStepArrive) ++d.pathPos;
    int pick = d.pathPos;
    for (int k = std::min(d.pathPos + kLookAheadCells, last); k > d.pathPos; --k) {
        if (GridClear(d.x, d.y, d.path[static_cast<size_t>(k)].x, d.path[static_cast<size_t>(k)].y)) {
            pick = k;
            break;
        }
    }
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
    ++g_count.detours;
    FollowPath(d, kWaypoints[w].x, kWaypoints[w].y);
}

// 커서를 상대 위에 둔다 — 주문은 끝나는 순간의 커서로 대상을 정한다(Server.cpp:6824-6831).
void AimAt(Dummy& d, int x, int y) {
    d.facing = FacingTo(x - d.x, y - d.y);
    d.out.push_back(static_cast<uint8_t>(Op::MOUSE));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(x)));
    Put16(d.out, static_cast<uint16_t>(static_cast<int16_t>(y)));
    ++g_count.sentMouse;
}

// 쫓아간다 — 벽이 없으면 곧장, 있으면 길 따라. 3초에 몸이 못 움직였으면 지점을 들른다.
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
    if (clear) {
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
    // ★ 벽을 사이에 두고 붙으면 판정은 「닿음」이어도 피해가 안 들어간다(MapTraceRay · Server.cpp:17501).
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
        ++g_count.sentAttack;
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
        ++g_count.sentAbility;
        if (ab.id == 1) ++g_count.sentBerserk;
        else if (ab.id == 2) ++g_count.sentRoar;
        else ++g_count.sentHarpoon;
        d.abilReadyFrame[ab.id] = d.frame + ab.delay + kAbilityMargin;
        d.abilGapEndFrame = d.frame + kAbilityGapFrames;
        return;
    }
}

void CountCast(const SpellInfo& sp) {
    switch (sp.id) {
        case 51: ++g_count.castProtect; break;
        case 36: ++g_count.castHaste; break;
        case 39: ++g_count.castInvis; break;
        case 12: ++g_count.castConfuse; break;
        case 27: ++g_count.castFireball; break;
        case 16: ++g_count.castDeathRay; break;
        case 43: ++g_count.castChain; break;
        case 4: ++g_count.castBlink; break;
        case 41: ++g_count.heals; break;
        default: break;
    }
}

// 79 <주문 u32> <0 u32 × 4> <대상 깃발 u8> 22바이트(Server.cpp:4144-4156).
void CastSpell(Dummy& d, const Args& a, const SpellInfo& sp, uint32_t holdFrames) {
    const size_t at = d.out.size();
    d.out.resize(at + 22, 0);
    d.out[at] = static_cast<uint8_t>(Op::TRY_SPELL);
    d.out[at + 1] = static_cast<uint8_t>(sp.id);
    d.out[at + 21] = sp.selfFlag;
    ++g_count.sentSpell;
    CountCast(sp);
    if (d.firstCastAfterRevive) {
        d.firstCastAfterRevive = false;
        if (&sp == &kProtect || &sp == &kHaste || &sp == &kInvis) ++g_count.firstBuffOk;
        else ++g_count.firstBuffBad;
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
    ++g_count.spellSkippedNoMana;
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
        d.attackSpell = r < 33 ? &kFireball : (r < 66 ? &kDeathRay : &kChain);
    }
    if (fight && !d.retreating && d.hp > 0 && d.hp * 100 <= d.maxHp * kRetreatPercent) {
        d.retreating = true;
        d.retreatRunEndFrame = d.frame + kRetreatMinFrames + d.rng() % (kRetreatMaxFrames - kRetreatMinFrames + 1);
        d.retreatPoint = -1;
        d.backingOff = false;
        d.path.clear();
        ++g_count.retreats;
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
            ++g_count.healedBack;
        } else if (d.mana < kHeal.mana) {
            d.retreating = false;                // 치유할 마나가 없다 — 맨손으로 돌아간다
        } else {
            if (FrameReached(d, d.spellReadyFrame)) CastSpell(d, a, kHeal, kCastHoldFrames);
            return;
        }
    }
    const bool inSight = dist <= kSightRange && clear;
    if (fight && FrameReached(d, d.spellReadyFrame) && TryCast(d, a, t, dist, inSight)) return;
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

// 이번 바퀴의 행동. 싸움은 판 맵에서만 — 대기 맵은 채팅 모드다(Server.cpp:4168).
void Decide(Dummy& d, const Args& a) {
    d.wantMove = d.wantClick = false;
    if (a.behavior == Behavior::Idle || d.mapName.empty() || d.mapName == d.waitMapName) return;
    if (d.dead) {
        if (FrameReached(d, d.reviveClickFrame)) {
            d.wantClick = true;
            ++g_count.sentRevive;
            d.reviveClickFrame = d.frame + kReviveGapFrames;
        }
        return;
    }
    const OtherPlayer* t = NearestTarget(d);
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
    ++g_count.sentInput;
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

void Step(Dummy& d, int64_t nowUs, const Args& a, std::vector<uint8_t>& recvBuf, std::vector<uint8_t>& scratch) {
    switch (d.phase) {
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
        ++g_count.sentPing;
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

void PrintCounters(const char* head, int sec, int entered, int total, const Counters& c) {
    std::printf("%s 초 %d · 입장 %d/%d", head, sec, entered, total);
#define X(n) std::printf(" %s=%llu", #n, static_cast<unsigned long long>(c.n));
    NOX_DUMMY_COUNTERS(X)
#undef X
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
    if (a.classes.empty()) { err = "--classes 가 비었다"; return false; }
    for (int c : a.classes)
        if (c < 0 || c > 2) { err = "직업은 0 전사 · 1 위저드 · 2 컨저러"; return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::string err;
    if (!ParseArgs(argc, argv, a, err)) {
        std::printf("인자 오류: %s\n", err.c_str());
        std::printf("쓰기: nox_dummy [--server 주소:포트] [--count N] [--room-size K] [--reserve-seats R] [--classes 0,1] [--duration-sec S] "
                    "[--join-interval-ms M] [--start-wait-ms M] [--behavior idle|move|fight] [--map-file 대전맵.map] [--seed S] "
                    "[--stat-mult N] [--freq-mult X]\n");
        return 1;
    }

    nox::SetStatMult(a.statMult);                // 최대 체력은 표에 서버와 같은 배수를 곱한 값이다
    if (a.behavior != Behavior::Idle && !LoadNav(a.mapFile))
        std::printf("[더미] 벽 격자를 못 읽음(%s) — 벽을 돌아가지 못하고 곧장 쫓기만 한다\n", a.mapFile.c_str());

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::printf("WSAStartup 실패\n"); return 1; }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(a.port);
    if (inet_pton(AF_INET, a.host.c_str(), &to.sin_addr) != 1) { std::printf("주소를 못 읽음: %s\n", a.host.c_str()); return 1; }

    std::vector<Dummy> dummies(static_cast<size_t>(a.count));
    std::vector<std::vector<int>> slots(kWheelSlots);
    // 보내는 순간을 씨앗 난수로 흩뜨린다 — 서로 다른 PC 의 정품 클라는 같은 1ms 에 몰아 보내지 않는다.
    // 씨앗을 고정하므로 실행마다 같은 칸에 든다.
    std::mt19937 slotRng(a.seed);
    const int64_t t0 = NowUs();
    for (int i = 0; i < a.count; ++i) {
        Dummy& d = dummies[static_cast<size_t>(i)];
        d.index = i;
        d.cls = a.classes[static_cast<size_t>(i) % a.classes.size()];
        d.rng.seed(a.seed * 1000u + static_cast<uint32_t>(i));
        d.maxHp = nox::HealthOnline(static_cast<nox::CharClass>(d.cls));
        d.maxMana = nox::ManaOnline(static_cast<nox::CharClass>(d.cls));
        d.hp = d.maxHp;
        d.mana = d.maxMana;
        d.joinAtUs = t0 + static_cast<int64_t>(i) * a.joinIntervalMs * 1000;
        d.seenImportant.assign(65536, false);
        d.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (d.sock == INVALID_SOCKET) { std::printf("소켓 실패 %d\n", WSAGetLastError()); return 1; }
        u_long nonBlocking = 1;
        ioctlsocket(d.sock, FIONBIO, &nonBlocking);
        if (connect(d.sock, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) { std::printf("connect 실패 %d\n", WSAGetLastError()); return 1; }
        slots[slotRng() % kWheelSlots].push_back(i);
    }

    // 서버와 같은 고해상도 대기 타이머(main.cpp:850) — Sleep(1) 은 약 15.5ms 를 자서 보내는 간격이 흔들린다.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        std::printf("[더미] 정밀 타이머를 못 만들어 보통 타이머로 기다린다 — 간격이 약 15.6ms 단위로 흔들린다\n");
    }

    std::vector<uint8_t> recvBuf(65536);
    std::vector<uint8_t> scratch;
    scratch.reserve(2048);
    Counters printed;
    int64_t base = t0;
    int64_t nextStat = t0 + 1000000;
    const int64_t endAt = t0 + static_cast<int64_t>(a.durationSec) * 1000000;
    int sec = 0;

    const auto countEntered = [&] {
        int n = 0;
        for (const Dummy& d : dummies)
            if (d.phase == Phase::Game) ++n;
        return n;
    };

    // 한 바퀴에 일한 시간과 33ms 를 넘긴 정도 — lateTicks 가 오른 까닭이 「타이머가 조금 늦게 깨움」인지 「일이 33ms 에 안 들어감」인지 가른다
    uint64_t wheels = 0, busySumUs = 0, busyMaxUs = 0;
    uint64_t overBuckets[5] = {};           // 1ms 이하 · 2ms 이하 · 5ms 이하 · 10ms 이하 · 그 위

    for (;;) {
        int64_t busyUs = 0;
        for (int s = 0; s < kWheelSlots; ++s) {
            if (slots[static_cast<size_t>(s)].empty()) continue;
            WaitUntilUs(timer, base + static_cast<int64_t>(s) * 1000);
            const int64_t now = NowUs();
            for (int i : slots[static_cast<size_t>(s)]) Step(dummies[static_cast<size_t>(i)], now, a, recvBuf, scratch);
            busyUs += NowUs() - now;
        }
        const int64_t after = NowUs();
        const int64_t next = base + kWheelUs;
        ++wheels;
        busySumUs += static_cast<uint64_t>(busyUs);
        busyMaxUs = std::max(busyMaxUs, static_cast<uint64_t>(busyUs));
        if (after > next) {
            const int64_t over = after - next;
            ++overBuckets[over <= 1000 ? 0 : over <= 2000 ? 1 : over <= 5000 ? 2 : over <= 10000 ? 3 : 4];
        }
        if (after > next + kLateToleranceUs) {   // 한 바퀴가 33ms 를 넘겼다 — 몰아 보내지 않고 지금부터 다시 센다
            ++g_count.lateTicks;
            base = after;
        } else {
            base = next;
        }
        if (after >= nextStat) {
            ++sec;
            PrintCounters("[더미]", sec, countEntered(), a.count, Delta(g_count, printed));
            printed = g_count;
            nextStat += 1000000;
        }
        if (after >= endAt) break;
    }

    PrintCounters("[더미 요약]", sec, countEntered(), a.count, g_count);
    std::printf("[더미 바퀴] %llu바퀴 · 일한 시간 평균 %.2fms · 최대 %.2fms · 33ms 넘김 1ms 이하 %llu · 2ms 이하 %llu · 5ms 이하 %llu · 10ms 이하 %llu · 그 위 %llu\n",
                static_cast<unsigned long long>(wheels), wheels ? static_cast<double>(busySumUs) / static_cast<double>(wheels) / 1000.0 : 0.0,
                static_cast<double>(busyMaxUs) / 1000.0, static_cast<unsigned long long>(overBuckets[0]),
                static_cast<unsigned long long>(overBuckets[1]), static_cast<unsigned long long>(overBuckets[2]),
                static_cast<unsigned long long>(overBuckets[3]), static_cast<unsigned long long>(overBuckets[4]));
    for (Dummy& d : dummies) closesocket(d.sock);
    WSACleanup();
    return 0;
}
