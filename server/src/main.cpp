// main.cpp — 진짜 소켓을 `nox::Server` 에 붙인다.
//
// **여기에는 프로토콜이 없다.** 받은 바이트를 그대로 넘기고, 나온 바이트를 그대로 보낸다.
// 서버가 무엇에 어떻게 답하는지는 전부 `server/src/Server.cpp` 에 있고,
// 같은 로직을 대조 장치(`server/src/replay.cpp`)가 덤프로 돌려 채점한다.
//
//   [진짜 소켓]  main.cpp    ─┐
//                             ├─▶  Server::OnPacket  ─▶  나갈 조각들
//   [덤프 재생]  replay.cpp  ─┘
//
// 지금 답하는 것 = 서버 찾기 · 접속 시도 · 접속 수락 · 핑.
//
//   nox_server.exe [--port 18590] [--name "MY SERVER"] [--selftest] [--quiet]
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nox/Messages.h"
#include "nox/Opcode.h"
#include <map>
#include "nox/DumpSniff.h"
#include "nox/MsgLen.h"
#include "nox/Recording.h"
#include "nox/ModeRules.h"
#include "nox/Server.h"
#include "nox/MapFile.h"
#include "nox/ThingDb.h"
#include "nox/SpellNames.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

void DumpHex(const char* tag, const uint8_t* p, size_t n) {
    std::printf("%s len=%zu\n  ", tag, n);
    for (size_t i = 0; i < n; ++i) {
        std::printf("%02x ", p[i]);
        if ((i + 1) % 22 == 0 && i + 1 < n) std::printf("\n  ");
    }
    std::printf("\n");
}

// 재생 조각들이 무슨 메시지로 이뤄졌는지 센다. 이분법에 들어가기 전에 지형부터 본다.
void MirrorStat(const std::vector<std::vector<uint8_t>>& v) {
    std::map<uint8_t, int> hist;
    int unparsed = 0;
    for (const auto& fr : v) {
        size_t i = 0;
        bool ok = true;
        while (i < fr.size()) {
            const uint8_t op = fr[i];
            const int bl = nox::MsgBodyLenByDir(op, /*fromClient=*/false);
            if (bl < 0) { ok = false; break; }
            hist[op]++;
            i += 1 + static_cast<size_t>(bl);
            if (i > fr.size()) { ok = false; break; }
        }
        if (!ok) ++unparsed;
    }
    std::printf("[재생 조각] %zu개 · 못 쪼갠 조각 %d개\n", v.size(), unparsed);
    for (const auto& kv : hist)
        std::printf("   0x%02x %-30s %d\n", kv.first,
                    nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
}
// 덤프 기본 자리 — exe 는 <리포>\build\Release\ 에 있으니 두 단 위가 리포다.
std::string DefaultDumpPath() {
    char exe[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return {};
    std::string p(exe);
    for (int i = 0; i < 3; ++i) {          // 파일명 · Release · build
        const size_t at = p.find_last_of("\\/");
        if (at == std::string::npos) return {};
        p.resize(at);
    }
    return p + "\\_evidence\\CAP_arena_0907_ok.txt";
}

// 녹음을 구조체로 읽는다. 실패하면 배치 해석이 틀린 것이다.
bool LoadRecorded(nox::MsgServerInfo& out) {
    nox::Reader r(nox::kRecordedServerInfo + nox::kHeaderSize,
                  nox::kRecordedServerInfoSize - nox::kHeaderSize);
    return out.Decode(r) && r.ok();
}

// 구조체를 다시 바이트로. 머리 3바이트를 앞에 붙인다.
size_t BuildInfo(const nox::MsgServerInfo& info, uint8_t* buf, size_t cap) {
    if (cap < nox::kHeaderSize) return 0;
    buf[0] = nox::kRecordedServerInfo[0];
    buf[1] = nox::kRecordedServerInfo[1];
    buf[2] = static_cast<uint8_t>(nox::Op::SERVER_INFO);
    nox::Writer w(buf + nox::kHeaderSize, cap - nox::kHeaderSize);
    if (!info.Encode(w) || !w.ok()) return 0;
    return nox::kHeaderSize + w.written();
}

// 녹음 → 구조체 → 바이트 가 원본과 한 바이트도 다르지 않은지 본다.
// 여기서 어긋나면 배치 해석이 틀린 것이므로 서버를 띄우면 안 된다.
int SelfTest() {
    nox::MsgServerInfo info;
    if (!LoadRecorded(info)) {
        std::printf("[FAIL] 녹음을 구조체로 읽지 못했다\n");
        return 1;
    }
    std::printf("읽은 값 — 현재인원=%u 최대인원=%u 맵=\"%s\" 이름=\"%s\" token=0x%08X\n",
                info.playersCur, info.playersMax, info.mapName.c_str(),
                info.serverName.c_str(), info.token);

    uint8_t buf[256] = {};
    const size_t n = BuildInfo(info, buf, sizeof(buf));
    if (n != nox::kRecordedServerInfoSize) {
        std::printf("[FAIL] 길이가 다르다: %zu != %zu\n", n, nox::kRecordedServerInfoSize);
        return 1;
    }
    if (std::memcmp(buf, nox::kRecordedServerInfo, n) != 0) {
        std::printf("[FAIL] 바이트가 다르다\n");
        DumpHex("  원본", nox::kRecordedServerInfo, n);
        DumpHex("  결과", buf, n);
        return 1;
    }
    std::printf("[OK] 녹음 왕복 %zu바이트 일치\n", n);

    // Token 이 본문 41번 자리에 리틀엔디안으로 들어가는지
    info.token = 0x11223344u;
    const size_t n2 = BuildInfo(info, buf, sizeof(buf));
    const uint8_t want[4] = {0x44, 0x33, 0x22, 0x11};
    if (n2 != n || std::memcmp(buf + nox::kHeaderSize + 41, want, 4) != 0) {
        std::printf("[FAIL] Token 자리가 41이 아니다\n");
        return 1;
    }
    std::printf("[OK] Token 이 본문 41번 자리에 리틀엔디안으로 들어간다\n");
    return 0;
}

// ★ 2026-09-16 조각 G — `ModeGameFlags` 는 **`nox/ModeRules.h` 로 옮겼다.**
//   여기 있는 동안은 시험이 한 번도 못 불렀다(`proto_test` 는 `main.cpp` 를 안 링크한다).
//   그 사각지대에서 묶음 8 규칙이 도달 불가였고, 모드 갈래의 `else` 가 여섯 모드를 삼켰다.

// ★ 09-23 nine-defects 0-c — **능력별·작살 갈래 줄.** 사람 판의 「실행 11 · 겹침 12」가 어느 능력인지
//   몰라서 A·S·F 판정이 한 번 뒤집혔다. 능력마다 「들어옴 = 거절 칸 합 + 실행」 등식을 같이 찍고,
//   어긋나면 ⚠ 를 붙인다(세는 자리가 빠졌다는 뜻). 0 인 칸도 다 찍는다 — 「들어옴」이 분모다
void PrintAbilityBreakdown(const nox::Server& srv) {
    static const char* const kName[nox::kAbilityCount] = { "", "광전사", "함성", "작살", "살금살금", "늑대눈" };
    using G = nox::AbilGate;
    using E = nox::AbilEnd;
    std::printf("[능력별] 번호밖 %llu", srv.abilBadNum());
    for (uint8_t a = 1; a < nox::kAbilityCount; ++a) {
        unsigned long long sum = 0;
        for (size_t g = 0; g < nox::kAbilGateCount; ++g) sum += srv.abilGate(a, static_cast<G>(g));
        std::printf(" | %u%s 들어옴 %llu%s = 실행 %llu + 거절{죽음 %llu·워리어아님 %llu·겹침 %llu",
                    (unsigned)a, kName[a], srv.abilIn(a), sum == srv.abilIn(a) ? "" : " ⚠칸합불일치",
                    srv.abilGate(a, G::Ok), srv.abilGate(a, G::Dead), srv.abilGate(a, G::NotWarrior),
                    srv.abilGate(a, G::Exclusive));
        if (a <= 3) {                                        // 같이 못 켜는 셋만 「누가 막았나」가 있다
            std::printf("(");
            bool first = true;
            for (uint8_t o = 1; o <= 3; ++o) {
                if (o == a) continue;
                std::printf("%s%s %llu", first ? "" : "·", kName[o], srv.abilBlockedBy(a, o));
                first = false;
            }
            std::printf(")");
        }
        std::printf("·깃발 %llu", srv.abilGate(a, G::CtfFlag));   // ★ 2-b1
        std::printf("·이미켜짐 %llu·상태12 %llu·안배움 %llu·짐넘침 %llu·쉬는중 %llu}"
                    " 꺼짐{시간 %llu·사라짐 %llu·되돌림 %llu·전부 %llu·작살끊김 %llu·동작이끔 %llu}"
                    " 소리부름{쓸때 %llu·꺼질때 %llu}",
                    srv.abilGate(a, G::Already), srv.abilGate(a, G::MoveState), srv.abilGate(a, G::NotLearned),
                    srv.abilGate(a, G::Overweight), srv.abilGate(a, G::Cooldown),
                    srv.abilEnded(a, E::Expired), srv.abilEnded(a, E::Gone), srv.abilEnded(a, E::Reset),
                    srv.abilEnded(a, E::CancelAll), srv.abilEnded(a, E::HarpoonBreak), srv.abilEnded(a, E::Disabled),
                    srv.abilSoundUseCalls(a), srv.abilSoundOffCalls(a));
        if (a == 4) std::printf(" 달리기막음 %llu", srv.treadBlockedRun());   // ★ 1-a — 살금살금이 걷기로 누른 틱
        // ★ 2-a1 — 함성 애니가 외치는 칸(2→3)을 넘은 수 · 애니가 끝나 켜짐 값을 내린 수
        if (a == 2) std::printf(" 외침칸 %llu·애니끝 %llu·기절몬스터 %llu", srv.warcryShouts(), srv.abilValOff(), srv.warcryStunned());
        if (a == 1) std::printf(" 돌진틱 %llu", srv.berserkChargeTicks());   // ★ 2-b1 — 광전사가 앞으로 민 틱
        if (a == 1) {                                        // ★ 2-b2 — 켜진 채 부딪힘 함수를 부른 수 = 갈래 합
            using B = nox::BerserkHit;
            std::printf(" 부딪힘 %llu%s = {유닛 %llu·물체 %llu·벽세게 %llu·벽살짝 %llu·가벼워지나감 %llu·겹쳐지나감 %llu·문 %llu}",
                        srv.berserkCollideCalls(), srv.BerserkHitSum() == srv.berserkCollideCalls() ? "" : " ⚠칸합불일치",
                        srv.berserkHit(B::Unit), srv.berserkHit(B::Object), srv.berserkHit(B::WallHard),
                        srv.berserkHit(B::WallSoft), srv.berserkHit(B::Light), srv.berserkHit(B::Overlap), srv.berserkHit(B::Door));
        }
    }
    std::printf("\n");
    using T = nox::HarpoonTouch;
    using H = nox::HarpoonEnd;
    std::printf("[작살갈래] 쏨 %llu · 휘기{잡음 %llu·튼틱 %llu} · 닿음{주인없음 %llu·벽 %llu·자기 %llu·적아님 %llu·걸림 %llu}"
                " · 끊김{새로쏨 %llu·벽 %llu·적아님 %llu·화살없음 %llu·주인죽음 %llu·상대사라짐 %llu·상대죽음 %llu"
                "·멀어짐 %llu·다끌어옴 %llu·수명 %llu·안움직임 %llu·시야막힘 %llu·너무멀리날아감 %llu·휘던상대죽음 %llu·시험 %llu}\n",
                srv.harpoonShot(), srv.harpoonAimFound(), srv.harpoonSteerTicks(),
                srv.harpoonTouch(T::NoOwner), srv.harpoonTouch(T::Wall), srv.harpoonTouch(T::Self),
                srv.harpoonTouch(T::NotEnemy), srv.harpoonTouch(T::Attached),
                srv.harpoonEnd(H::Replaced), srv.harpoonEnd(H::Wall), srv.harpoonEnd(H::NotEnemy),
                srv.harpoonEnd(H::BoltGone), srv.harpoonEnd(H::OwnerDead), srv.harpoonEnd(H::TargetGone),
                srv.harpoonEnd(H::TargetDead), srv.harpoonEnd(H::TooFar), srv.harpoonEnd(H::Arrived),
                srv.harpoonEnd(H::Lifetime), srv.harpoonEnd(H::Stuck), srv.harpoonEnd(H::NoSight),
                srv.harpoonEnd(H::OverFlight), srv.harpoonEnd(H::AimDead), srv.harpoonEnd(H::Debug));
    // ★ 1-d — 맵 불 같은 장애물의 닿음 피해. 옆에 전체 피해(`hurts`)를 같이 찍는다 — 0 이 「안 닿았다」인지 「못 셌다」인지 가르려고
    //   ⚠ 「부름」은 양 1 짜리 불의 짝수 프레임(피해 0)까지 센다 — 정답지도 피해 함수를 0 으로 부른다
    std::printf("[맵닿음] 장애물 닿음 피해 부름 %llu회 · 전체 피해 %llu회 · 사람 벽 짝 %llu회\n", srv.contactDamageHits(), srv.hurts(),
                srv.wallContacts());   // ★ 2-b2 — 벽 짝은 광전사 벽 갈래의 분모
    // ★ 3-a — 점프. 「코드7 받음」이 분모다 — 사람이 휠을 눌렀는데 0 이면 **7 이 점프라는 가정부터** 틀린 것이다
    using J = nox::JumpGate;
    std::printf("[점프] 코드7 받음 %llu · 들어옴 %llu%s = {뜀 %llu·못움직임 %llu·광전사 %llu·함성 %llu·짐넘침 %llu·기력모자람 %llu}"
                " · 민틱 %llu · 작살에잡혀안민틱 %llu · 내려옴 %llu · 가만이끔 %llu · 움찔막음 %llu · 지진뺌 %llu · 시전막음 %llu\n",
                srv.jumpCodes(), srv.jumpIn(), srv.JumpGateSum() == srv.jumpIn() ? "" : " ⚠칸합불일치",
                srv.jumpGate(J::Ok), srv.jumpGate(J::CantMove), srv.jumpGate(J::Berserk), srv.jumpGate(J::Warcry),
                srv.jumpGate(J::Overweight), srv.jumpGate(J::NoStamina),
                srv.jumpForceTicks(), srv.jumpHeldTicks(), srv.jumpLanded(), srv.airborneOffIdle(), srv.recoilBlockedAirborne(),
                srv.equakeSkipAirborne(), srv.spellRefusedAirborne());   // ★ 3-b
}

// 능력·작살 계수기가 바뀌었나를 보는 지문 — `--quiet` 에서도 **바뀐 때만** 위 두 줄을 찍으려고 쓴다
unsigned long long AbilityFingerprint(const nox::Server& srv) {
    unsigned long long n = srv.abilCalls() + srv.harpoonShot() + srv.treadBlockedRun() + srv.harpoonAimFound() +
                           srv.contactDamageHits() + srv.warcryShouts() + srv.abilValOff() + srv.berserkChargeTicks() +
                           srv.berserkCollideCalls() + srv.jumpCodes() + srv.jumpLanded();
    for (uint8_t a = 1; a < nox::kAbilityCount; ++a)
        for (size_t e = 0; e < nox::kAbilEndCount; ++e) n += srv.abilEnded(a, static_cast<nox::AbilEnd>(e));
    for (size_t t = 0; t < nox::kHarpoonTouchCount; ++t) n += srv.harpoonTouch(static_cast<nox::HarpoonTouch>(t));
    for (size_t h = 0; h < nox::kHarpoonEndCount; ++h) n += srv.harpoonEnd(static_cast<nox::HarpoonEnd>(h));
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    // 한글 출력이 CP949 로 오독되지 않게. printf 는 /utf-8 로 컴파일된 UTF-8 을 낸다.
    SetConsoleOutputCP(CP_UTF8);

    int port = nox::kGamePort;
    // 이름에 공백을 쓰지 않는다. Start-Process 등에서 인자가 쪼개진다.
    std::string name = "MY_SERVER";
    bool quiet = false;
    std::string dumpPath;   // 접속 묶음·맵 지정을 뽑아올 덤프
    bool mirror = false;    // 진단 — 정품이 보냈던 조각을 그대로 재생
    int mirrorLimit = 0;    // 이분법용 — 앞 N 개만 재생 (0 이면 전부)
    bool prelude = false;   // 서두 — 정품 앞 9개를 흘려보낸 뒤 내 생성으로
    bool noUpdate = false;
    // 첫 세계 스냅샷을 **녹음본 그대로** 내보낸다(사람 항목을 안 갈아 끼움).
    // 「몸이 안 그려지는 게 갈아끼우기 탓인가」를 가르는 시험용.
    bool rawSnapshot = false;
    // 새로 들어온 사람의 장비를 먼저 와 있던 사람들에게 **안 알린다.**
    // 「둘째가 들어오는 순간 첫째가 죽는 것」이 이 알림 탓인지 가르는 시험용.
    bool noNewGear = false;
    // 첫 세계 스냅샷을 아예 안 보낸다. HUD 실종이 이 배선 탓인지 가르는 시험용.
    bool noSnapshot = false;
    bool ackSeq = true;        // B-1b — 신뢰 순번 되받기(**기본 켬** — 정품 녹음 근거는 Server::Emit 주석)
    bool ackAnyStream = true;  // B-1b — 되받기를 스트림 0 비신뢰에(**기본 켬**)
    bool noSpawnInvuln = false;  // 부활 무적 안 걸기(`--no-spawn-invuln`) — 화면 확인 우회로
    // 서두 안 cc(6a … 01) 의 상태를 0 으로 뒤집는 것을 **끈다**(녹음본 값 그대로).
    // 기본은 뒤집는 쪽이다 — Server.h 의 SetForceHudOn 주석 참조.
    bool hudOff = false;
    // 새 사람이 들어올 때 남들에게 넘길 오프코드 (쉼표로, 16진). 빈 문자열이면 기본값.
    std::string peerOps;
    // ★★ 기본을 **서버 쪽 기본값(켜짐)에 맞춘다.** 전에는 여기서 false 로 두고
    //   인자와 무관하게 늘 `SetIntroEveryTick(...)` 을 불러 **헤더 기본값을 덮고 있었다** —
    //   그래서 `--intro-every-tick` 을 준 회차만 몸이 그려지고 나머지는 안 그려졌다.
    //   「회차마다 다르다」로 보이던 것의 정체가 이것이다(2026-09-09).
    //   끄려면 `--alias-intro` 를 준다.
    bool introEveryTick = true;
    int spawnSpread = 0;
    bool peerFirst = false;
    bool peerSnapshot = false;
    int peerType = 0;
    int peerPose = -1;
    // ★ 어떤 판을 차릴까 — `arena`(기본) · `ctf`(깃발) · `quest`(몬스터 잡기).
    //   전에는 깃발·퀘스트가 **단위시험에서만** 섰다(`StartCtf`·`StartQuest` 를 아무도 안 불렀다).
    //   실클라로 돌려 보려면 켜는 손잡이가 있어야 한다(2026-09-09).
    std::string mode = "arena";
    // ★ 독립 서버 P3 — .map 을 직접 읽어 벽 충돌을 켤다. 비면 종전처럼 벽 없음.
    std::string mapPath;
    // ★ 2026-09-18 ④⑯ — 맵에 그려진 **깃발 자리**(`Flag` 오브젝트). 둘 이상이면
    //   CTF 를 거기에 차린다. 전에는 늘 스폰 옆에 손으로 놓아 **그림과 실물이 전혀
    //   다른 자리**였다(CapFlag 실측 — 맵 1058,931·3841,3714 ↔ 우리 3828,2534·4104,2534).
    //   맵 로드 블록 밖에서도 봐야 해서 여기에 둔다.
    std::vector<std::pair<int, int>> mapFlagPos;
    // ★ 09-20 9-b — 덫 자리를 바깥에 둔다. 길목을 읽은 **뒤**에 「덫 옆 설 자리」를 찍기 위함
    std::vector<std::pair<float, float>> trapPos;
    bool standalone = false;
    // 진단 — 맵 섹션 목록(이름·크기)만 찍고 **서버를 안 띄우고 끝낸다**. 157개를 배치로 돌리려고.
    //   왜 서버가 해야 하나 — .map 은 Blowfish 로 암호화돼 있어 바깥 도구로는 못 읽고,
    //   복호와 섹션 워커가 이미 여기 있다(MapFile::Load 가 **모든 섹션**을 담는다. 쓰는 건 5개뿐).
    bool dumpSections = false;
    std::string dumpThing;      // 진단 --dump-thing — 종류 하나의 클래스·서브클래스를 찍는다
    bool aoi = false;
    double creatureAoiHalf = 0.0;   // 크리처 시야 판정 반폭 — 0 꺼짐 · <0 정답지 기본(740×580) · >0 판정용 정사각형
    int creatureAwayAfter = 0;      // 진단 — 첫 사람 들어오고 N 틱 뒤 크리처를 ±300 으로 옮김 (0 끔)
    bool friends = false;           // 친구 알림(OBJECT_FRIEND_ADD/REMOVE) — 부분 구현이라 기본 끔
    int debugDummy = 0;             // 진단 — 첫 사람 N 틱 뒤 주인 없는 임프 허수아비(0 끔)
    int debugDummyDx = 25;          // 진단 --debug-dummy-dx
    int debugEnchantB = -1, debugEnchantP = 1, debugEnchantN = 0;   // 진단 --debug-enchant B P N
    int debugHurtEvery = 0;         // 진단 — N 틱마다 첫 사람에게 피해 25(0 끔)
    bool debugHurtFront = false;    // 진단 — 그 피해를 앞 30 에서 때린 것으로
    bool autoRespawn = false;       // 자동 부활(정답지 기본 꺼짐)
    int debugOwnPet = 0;            // 진단 — 첫 사람 들어오고 N 틱 뒤 그 사람 소유 임프를 사람 자리에 고정 (0 끔)
    int debugPetOrder = 0;          // 진단 --debug-pet-order (0 끔 · 5 사냥)
    int debugMobAfter = 0, debugMobType = 0, debugMobX = 0, debugMobY = 0;   // 진단 --debug-mob
    int tracePets = 0;              // 진단 --trace-pets N (0 끔) — N틱마다 크리처 자리·행동·길목
    unsigned long long tracePetsInWall = 0;   // ③ 크리처가 **벽 칸 안**에 있던 표본 수
    unsigned long long tracePetsSamples = 0;  // 그 분모 — 0 옆에 늘 지나간 양을 같이 읽는다
    int debugTeamAfter = 0;         // 진단 — 첫 사람 들어오고 N 틱 뒤 그 사람 팀을 한 번 바꿈 (0 끔)
    int debugTeamEvery = 0;         // 진단 — N틱마다 팀을 1↔2 로 오간다 (0 끔)
    int debugFistAfter = 0, debugFistDx = 40, debugFistDy = 0;
    int debugWarpAfter = 0, debugWarpX = 0, debugWarpY = 0;
    int debugWallAfter = 0, debugWallGx = 0, debugWallGy = 0;
    int debugKillAfter = 0;         // 진단 — N틱 뒤 첫 사람이 둘째를 죽인다 (0 끔)
    int debugScoreAfter = 0;        // 진단 — N틱 뒤 첫 사람 점수를 올린다 (0 끔)
    int debugScoreVal = 0;
    int spawnAtX = -1, spawnAtY = -1;   // --spawn-at X Y — 시작·부활 자리를 손으로 지정 (음수면 끔)
    // --debug-bot X Y [직업] — 가짜 사람을 세운다 (음수면 끔 · **여러 번 쓸 수 있다**).
    //   0 0 은 「맵의 시작 자리에 세워라」다. 여러 번 쓰면 시작 자리를 하나씩 옮겨 가며 쓴다.
    struct BotWant { int x = -1, y = -1; std::string cls = "warrior"; };
    std::vector<BotWant> bots;
    int debugXpAfter = 0;           // 진단 --debug-xp N 양 — N 틱 뒤 경험치 한 번 (0 끔)
    double debugXpAmount = 0.0;
    int debugSpellAfter = 0;        // 진단 --debug-give-spell N 주문 — N 틱 뒤 주문 하나 (0 끔)
    int debugSpellId = 0;
    bool noObstacles = false;       // 진단 — 장애물 안 실음(묶음 2-c 대조군)
    bool itemAtSpawn = false;
    int startHealth = 0;       // 진단 — 들어올 때 체력(0 이면 안 건드림)
    int startMana = 0;         // 진단 — 들어올 때 마나(0 이면 안 건드림)
    int forceDir = -1;         // 진단 — 덧붙이 방향 3비트 고정
    int forceArrowFrame = -1;  // 진단 ⑬ — 화살 스프라이트 프레임 고정
    bool arrowFreeze = false;  // 진단 ⑬ — 화살을 난 자리에 세워 둔다
    bool dumpArrows = false;   // 진단 ⑬ — 화살 목록·프레임 분포를 틱마다 찍는다
    bool noSecondNewPlayer = false;  // (옛 스위치 — 이제 기본이라 남겨만 둔다)
    bool secondNewPlayer = false;    // `--2nd-newplayer` — 옛 동작으로 되돌린다
    bool forceHarpoon = false;       // 진단 ⑦ — 작살을 맞히지 않고 바로 건다
    bool dumpHarpoon = false;        // 진단 ⑦ — 걸린 동안 매 틱 자리를 찍는다
    int arrowFrameAlt = 0;     // 진단 ⑬ — N 틱마다 프레임 0↔16 번갈기
    int forcePose = -1;        // 진단 — 사람 자세 번호 고정(-1 이면 안 함)
    bool spellStart = false;   // 시전 알림 0x70 을 켜는 손잡이 (기본 끔)   // 시험용 — 아이템 하나를 스폰 자리로 옮긴다
    int questCount = 4;
    // ⚠ 기본 300 은 실측 아닌 자리표시다. 실클라 시험에서는 **근접 사거리(45) 안**에 세운다 —
    //   안 그러면 걸어가 붙이는 조종까지 필요해져 판정이 흐려진다.
    double questSpread = 300.0;
    // ★ 깃발 진영을 가운데에서 이만큼씩 벌린다. 사람이 **걸어가 뺏어 와야** 판이 된다.
    int ctfSpan = 220;
    bool modeStarted = false;   // 남 항목 덧붙이 값. -1 이면 서버 기본(0x30)   // 0 이면 서버 기본값(40)

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") {
            return SelfTest();
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--name" && i + 1 < argc) {
            name = argv[++i];
        } else if (a == "--prelude") {
            prelude = true;
        } else if (a == "--no-update") {
            // ★ 진단용 — 상태 갱신을 끈다. Server.h 의 SetSendUpdates 주석 참조.
            noUpdate = true;
        } else if (a == "--raw-snapshot") {
            rawSnapshot = true;
        } else if (a == "--peer-ops" && i + 1 < argc) {
            peerOps = argv[++i];
        } else if (a == "--intro-every-tick") {
            introEveryTick = true;
        } else if (a == "--alias-intro") {
            // 진단용 — 이미 소개한 사람을 별칭으로 보낸다. 지금은 아무도 안 그려진다.
            introEveryTick = false;
        } else if (a == "--peer-type" && i + 1 < argc) {
            peerType = std::atoi(argv[++i]);
        } else if (a == "--peer-snapshot") {
            peerSnapshot = true;
        } else if (a == "--peer-first") {
            peerFirst = true;
        } else if (a == "--peer-pose" && i + 1 < argc) {
            peerPose = (int)std::strtol(argv[++i], nullptr, 16);
        } else if (a == "--spawn-spread" && i + 1 < argc) {
            spawnSpread = std::atoi(argv[++i]);
        } else if (a == "--hud-off") {
            hudOff = true;
        } else if (a == "--no-snapshot") {
            noSnapshot = true;
        } else if (a == "--no-spawn-invuln") {
            // 부활 무적을 아예 안 건다. CLIENT_READY 되풀이로 무적이 안 풀려
            // 화면 확인이 막히는 것을 **원인 규명과 따로** 우회한다(Server.h 주석).
            noSpawnInvuln = true;
        } else if (a == "--ack-any-stream") {
            // B-1b — 되받기를 **스트림 가리지 않고** 비신뢰에 싣는다.
            //   까닭 — 클라는 스트림 0 패킷의 seq 만 읽는다(09-18 실측. Server::Emit 주석).
            //   `--ack-seq` 와 같이 켠다.
            ackAnyStream = true;
        } else if (a == "--no-ack-seq") {
            // 대조군 — 되받기를 끈다. 켜고 끈 차이를 재려고 남겨 둔 스위치다
            //   (09-18 실측 — 끄면 CLIENT_READY 를 18~25 번 받는다).
            ackSeq = false;
            ackAnyStream = false;
        } else if (a == "--ack-seq") {
            // B-1b — 클라가 보낸 신뢰 순번 +1 을 답 머리에 싣는다.
            // ⚠ **기본 끔** — 09-18 실측에서 효과가 없었다(Server.h ackRecvSeq_ 주석의 표).
            ackSeq = true;
        } else if (a == "--no-newgear") {
            // ★ 진단용 — 새 사람 장비 알림을 끈다. Server.h 의 SetNotifyNewGear 주석 참조.
            noNewGear = true;
        } else if (a == "--mirror") {
            mirror = true;
        } else if (a == "--mirror-limit" && i + 1 < argc) {
            mirror = true;
            mirrorLimit = std::atoi(argv[++i]);
        } else if (a == "--mode" && i + 1 < argc) {
            mode = argv[++i];
            if (nox::ModeGameFlags(mode) == 0) {
                std::printf("모르는 모드: %s\n", mode.c_str());
                std::printf("  판이 도는 것   arena · ctf · quest\n");
                std::printf("  규칙만 선 것   elimination · kotr · flagball · chat · coop · coopteam\n");
                return 2;
            }
        } else if (a == "--quest-count" && i + 1 < argc) {
            questCount = std::atoi(argv[++i]);
        } else if (a == "--quest-spread" && i + 1 < argc) {
            questSpread = std::atof(argv[++i]);
        } else if (a == "--map" && i + 1 < argc) {
            mapPath = argv[++i];
        } else if (a == "--standalone") {
            // 독립 모드 — 녹음 없이 접속 묶음·USE_MAP·스냅샷을 조립(P5-c-2). --map 필수.
            standalone = true;
        } else if (a == "--dump-sections") {
            // 진단 — 맵 섹션 목록만 찍고 끝낸다(--map 필수). 서버를 안 띄운다.
            dumpSections = true;
        } else if (a == "--dump-thing" && i + 1 < argc) {
            // 진단 — 종류 하나의 **클래스·서브클래스·플래그 원문**을 찍고 끝낸다.
            //   ⚠ 이름 대신 `#<와이어번호>` 도 받는다(`--dump-thing #528`).
            //   만든 까닭 — 「화살이 정답지의 어느 갈래를 타나」를 따질 때마다
            //   thing.bin 을 눈으로 뒤지고 있었다. 갈래는 **클래스 비트가 정한다**
            //   (`GAME4_1.c:4539~4550` — 0x400000 / 0x200000 → 2·4·그밖 / 0x100000).
            dumpThing = argv[++i];
        } else if (a == "--start-mana" && i + 1 < argc) {
            // 진단 — 들어올 때 마나를 채운다. 비싼 주문을 무인으로 쏘려면 필요하다.
            startMana = std::atoi(argv[++i]);
        } else if (a == "--start-health" && i + 1 < argc) {
            // 진단 — 들어올 때 체력을 낮춘다. 죽음·부활을 재려고.
            startHealth = std::atoi(argv[++i]);
        } else if (a == "--arrow-frame-alt" && i + 1 < argc) {
            // 진단 ⑬ — N 틱마다 화살 프레임을 0↔16 으로 번갈아 보낸다.
            //   한 판 안에서 바꿔야 배경·화살 자리가 그대로라 픽셀 대조가 깨끗하다.
            arrowFrameAlt = std::atoi(argv[++i]);
        } else if (a == "--arrow-freeze") {
            // 진단 ⑬ — 화살을 난 자리에 세워 둔다. `--arrow-frame` 과 짝으로 쓴다.
            arrowFreeze = true;
        } else if (a == "--2nd-newplayer") {
            // 옛 동작 — 관전 풀 때 `2d NEW_PLAYER` 를 **다시 보낸다**(09-19 이전 기본값).
            //   ⚠ 그러면 자기 장비가 화면에서 사라진다. 되돌릴 일이 있을 때만 쓸 것
            secondNewPlayer = true;
        } else if (a == "--harpoon-force") {
            forceHarpoon = true;
        } else if (a == "--dump-harpoon") {
            dumpHarpoon = true;
        } else if (a == "--no-2nd-newplayer") {
            // 진단 — 관전 풀 때 `2d NEW_PLAYER` 를 다시 안 보낸다(정답지는 안 보낸다)
            noSecondNewPlayer = true;
        } else if (a == "--dump-arrows") {
            // 진단 ⑬ — 틱 로그마다 화살 목록(번호·자리·방향·실은 프레임)과
            //   **프레임 분포**를 찍는다. 화면 기울기와 1:1 로 맞대려는 것이고,
            //   「각도가 하나뿐인가」는 분포 한 줄로 갈린다.
            dumpArrows = true;
        } else if (a == "--arrow-frame" && i + 1 < argc) {
            // 진단 ⑬ — 화살 스프라이트 프레임을 못 박는다(0~31). 같은 자리에서
            //   프레임만 바꿔 찍어 화면이 실제로 달라지는지 픽셀로 대조하려고.
            forceArrowFrame = std::atoi(argv[++i]);
        } else if (a == "--anim-dir" && i + 1 < argc) {
            // 진단 — 사람 항목 덧붙이의 방향 3비트를 못 박는다(0~7).
            forceDir = std::atoi(argv[++i]);
        } else if (a == "--anim-pose" && i + 1 < argc) {
            // 진단 — 사람 자세 번호를 못 박는다(0 서기 · 2 죽음 · 4 걷기).
            forcePose = std::atoi(argv[++i]);
        } else if (a == "--spell-start") {
            // ★ 시전 알림(0x70)을 **켠다.** 기본은 끔 — Server.h 의 noSpellStart_ 주석 참조.
            spellStart = true;
        } else if (a == "--creature-aoi") {
            // 크리처 시야 판정 — 정답지 기본 사각형(화면절반+100 = 740×580)을 넘으면 안 싣고 사라짐을 보낸다.
            creatureAoiHalf = -1.0;
        } else if (a == "--creature-aoi-half" && i + 1 < argc) {
            // 진단 — 위를 **판정용 정사각형 반폭 N** 으로 켠다. 작게 주면 몬스터가 사각형 밖에서 시작해
            //   「안 보이다가 다가오면 나타난다」를 화면으로 볼 수 있다.
            creatureAoiHalf = std::atof(argv[++i]);
        } else if (a == "--creature-away-after" && i + 1 < argc) {
            // 진단 — 「사라짐」을 화면으로 보려고 크리처를 사각형 밖으로 옮긴다. 걷기를 빼 버린다.
            creatureAwayAfter = std::atoi(argv[++i]);
        } else if (a == "--friends") {
            // 친구 알림 — 자기 소환수·같은 팀을 친구로 알린다. 클라는 커서·선택 원 색을 이것으로 가른다.
            friends = true;
        } else if (a == "--debug-own-pet" && i + 1 < argc) {
            // 진단 — 친구 판정용. 소환 조작을 빼 버리고 그 사람 소유 임프를 사람 자리에 고정한다.
            debugOwnPet = std::atoi(argv[++i]);
        } else if (a == "--auto-respawn") {
            // 자동 부활 서버 설정(정답지 기본 꺼짐 — 죽은 뒤 클릭해야 살아난다 · 묶음 3-c)
            autoRespawn = true;
        } else if (a == "--debug-hurt-front") {
            // 진단 — 위 피해를 바라보는 쪽 30 앞에서 때린 것으로(방패 막기 판정 · 묶음 3-e2)
            debugHurtFront = true;
        } else if (a == "--debug-hurt-every" && i + 1 < argc) {
            // 진단 — 피해 함수 판정용(묶음 3-e). N 틱마다 첫 사람에게 피해 25(움찔 문턱 20 넘김).
            debugHurtEvery = std::atoi(argv[++i]);
        } else if (a == "--debug-dummy" && i + 1 < argc) {
            // 진단 — 주먹 판정용(묶음 3-a). 첫 사람 N 틱 뒤 주인 없는 임프를 오른쪽 +25 에 고정(맞으면 깎이고 죽는다).
            debugDummy = std::atoi(argv[++i]);
        } else if (a == "--debug-pet-order" && i + 1 < argc) {
            // 진단 — `--debug-own-pet` 소환수에게 명령을 준다(2 가만히·3 지켜·4 따라와·**5 사냥**).
            //   ⚠ 명령이 없으면 주인 있는 크리처는 **한 칸도 안 움직인다**(09-20 실측)
            debugPetOrder = std::atoi(argv[++i]);
        } else if (a == "--debug-dummy-dx" && i + 1 < argc) {
            // 진단 — 허수아비 오른쪽 거리(기본 25). 묶음 4-a2 날리기 화면 — 멀리 두면 Magic 이 날아가는 게 보인다.
            debugDummyDx = std::atoi(argv[++i]);
        } else if (a == "--debug-enchant" && i + 3 < argc) {
            // 진단 — 묶음 4-b 화면 판정. 첫 사람(관전 푼 뒤)에게 버프 B 를 세기 P 로 N 틱 뒤 끝없이 건다.
            debugEnchantB = std::atoi(argv[++i]);
            debugEnchantP = std::atoi(argv[++i]);
            debugEnchantN = std::atoi(argv[++i]);
        } else if (a == "--debug-bot" && i + 2 < argc) {
            // ★★ 2026-09-16 신설 — **가짜 사람**을 세운다.
            //   왜 — 계수기가 `made > 0` 일 때만 찍히는데, 사람이 없으면 내보낼 게 없어
            //   **틱 줄이 아예 안 나온다**(실측). 그래서 ③.5 「실물로 닫기」가 **사람 없이는 성립을 안 한다.**
            //   봇을 세우면 덫이 겨누고 갱신이 돌아 계수기가 살아난다 — 무인으로 판정할 수 있게 된다.
            //   ⚠ 봇 주소는 `addrOf` 에 없으므로 나가는 조각은 `flush` 에서 **조용히 버려진다**(main.cpp 의 `continue`).
            //     즉 망으로는 아무것도 안 나간다. 서버 안 상태와 계수기만 움직인다.
            BotWant bw;
            bw.x = std::atoi(argv[++i]);
            bw.y = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') bw.cls = argv[++i];
            bots.push_back(bw);
        } else if (a == "--spawn-at" && i + 2 < argc) {
            // ④ 화면 확인용 — 볼 것이 있는 자리에서 바로 시작한다(걸어가는 시간을 없앤다).
            spawnAtX = std::atoi(argv[++i]);
            spawnAtY = std::atoi(argv[++i]);
        } else if (a == "--debug-xp" && i + 2 < argc) {
            // 진단 — ④대기 ⑨. 첫 사람에게 N 틱 뒤 경험치를 한 번 준다(HUD 막대가 차는지).
            debugXpAfter = std::atoi(argv[++i]);
            debugXpAmount = std::atof(argv[++i]);
        } else if (a == "--debug-give-spell" && i + 2 < argc) {
            // 진단 — ④대기 ⑨. 첫 사람에게 N 틱 뒤 주문 하나를 준다(주문책이 느는지).
            debugSpellAfter = std::atoi(argv[++i]);
            debugSpellId = std::atoi(argv[++i]);
        } else if (a == "--debug-team-after" && i + 1 < argc) {
            // 진단 — RESET_FRIENDS 판정용. 첫 사람의 팀을 N 틱 뒤 한 번 바꾼다.
            debugTeamAfter = std::atoi(argv[++i]);
        } else if (a == "--fill-colors") {
            // 진단 — 빈 캐릭터 색을 정품 녹음 실측값으로 메운다(기본 끔).
            //   ⚠ 켜면 **살색 피부가 드러나 정답지와 달라진다**(09-19 화면 대조)
            nox::fillColorsOn = true;
        } else if (a == "--debug-fist" && i + 3 < argc) {
            // 진단 — N틱 뒤 **서버가 직접** 첫 사람 자리 + (dx,dy) 에 주먹을 떨어뜨린다(⑰ 촬영용)
            debugFistAfter = std::atoi(argv[++i]);
            debugFistDx = std::atoi(argv[++i]);
            debugFistDy = std::atoi(argv[++i]);
        } else if (a == "--debug-mob" && i + 4 < argc) {
            // 진단 — N틱 뒤 그 자리에 맵 몬스터 하나(종류 와이어 · 예 1358 거머리)를 세운다(⑮ 촬영용).
            //   ⚠ 퀘스트 스폰은 **사람 자리 기준**이라 몬스터가 사람에게 붙는다 — 이건 자리를 못 박는다
            debugMobAfter = std::atoi(argv[++i]);
            debugMobType = std::atoi(argv[++i]);
            debugMobX = std::atoi(argv[++i]);
            debugMobY = std::atoi(argv[++i]);
        } else if (a == "--trace-pets" && i + 1 < argc) {
            // 진단 — N틱마다 크리처의 **자리·맨 위 행동·고른 길목**을 찍는다(⑥ 6-j(3) 주행용).
            //   ⚠ 「안 움직인다」를 보려면 **자리와 고른 길목을 같이** 봐야 한다.
            //     자리만 보면 「길이 막혔나」와 「같은 곳을 또 골랐나」가 안 갈린다(09-20 에 그 함정에 빠졌다)
            tracePets = std::atoi(argv[++i]);
        } else if (a == "--debug-wall" && i + 3 < argc) {
            // 진단 — N틱 뒤 그 **칸**(gx,gy)의 비밀벽을 연다(9-a — 클라가 그려 주나만 본다)
            debugWallAfter = std::atoi(argv[++i]);
            debugWallGx = std::atoi(argv[++i]);
            debugWallGy = std::atoi(argv[++i]);
        } else if (a == "--debug-warp" && i + 3 < argc) {
            // 진단 — N틱 뒤 첫 사람을 그 자리로 한 번 옮긴다(문·밟는 판처럼 「다가가야」 보는 것용)
            debugWarpAfter = std::atoi(argv[++i]);
            debugWarpX = std::atoi(argv[++i]);
            debugWarpY = std::atoi(argv[++i]);
        } else if (a == "--debug-kill" && i + 1 < argc) {
            // 진단 — N틱 뒤 첫 사람이 둘째를 죽인 것으로 친다(킬→점수→판끝 사슬 확인용)
            debugKillAfter = std::atoi(argv[++i]);
        } else if (a == "--debug-score" && i + 2 < argc) {
            // 진단 — N틱 뒤 첫 사람의 점수를 <점수> 로 만든다(판 끝 사슬 확인용)
            debugScoreAfter = std::atoi(argv[++i]);
            debugScoreVal = std::atoi(argv[++i]);
        } else if (a == "--debug-team-every" && i + 1 < argc) {
            // 진단 ④ — N틱마다 첫 사람의 팀을 1↔2 로 오간다(친구 ADD 뒤 REMOVE 도 보려고)
            debugTeamEvery = std::atoi(argv[++i]);
        } else if (a == "--no-obstacles") {
            // 진단 — 장애물(오브젝트끼리 충돌)을 싣지 않는다. 묶음 2-c 화면 판정의 대조군.
            noObstacles = true;
        } else if (a == "--aoi") {
            // 시야 반경(P4-b) — 갱신 스트림에 맵 정적 오브젝트를 반경 안만 싣는다. --map 필수.
            aoi = true;
        } else if (a == "--item-at-spawn") {
            // ★ 시험용 — 아이템 하나를 **스폰 자리로 옮긴다.**
            //
            //   왜 — 「어떤 조작이 줍기를 보내나」를 재려면 물건 위에 서 있어야 하는데,
            //   걸어가는 길이 자꾸 어긋난다(2026-09-10 스윕 1차: 도착 실패로 127유닛 밖에서
            //   재는 바람에 30가지가 전부 0건으로 나왔다. 그건 「없었다」가 아니라 「못 쟀다」다).
            //   재는 대상은 **조작**이지 걷기가 아니니, 걷기를 빼 버린다.
            itemAtSpawn = true;
        } else if (a == "--ctf-span" && i + 1 < argc) {
            ctfSpan = std::atoi(argv[++i]);
        } else if (a == "--dump" && i + 1 < argc) {
            dumpPath = argv[++i];
        } else {
            std::printf("모르는 인자: %s\n", a.c_str());
            return 2;
        }
    }

    nox::MsgServerInfo tmpl;
    if (!LoadRecorded(tmpl)) {
        std::printf("녹음을 읽지 못했다. --selftest 로 확인할 것\n");
        return 1;
    }
    tmpl.serverName = name;

    // ★ 서버 로직은 `nox::Server` 가 들고 있다. 여기는 소켓만 붙인다.
    //   같은 로직을 대조 장치(server/src/replay.cpp)가 덤프로 돌려 채점한다.
    //
    // 서버 찾기에 답할 원본은 녹음을 쓰되, 이름만 인자로 바꿔 넣는다.
    nox::Server srv;

    // ★★ **`--standalone` 은 맵이 있어야 성립한다.** 접속 묶음에 맵 이름과 CRC 를
    //   싣기 때문이다(`SetStandalone`). 맵이 없으면 `standalone_` 이 안 켜지고,
    //   그러면 `CLIENT_ACCEPT` 가 **조용히 아무것도 안 보내는 분기**로 빠진다.
    //
    // ⚠ 2026-09-10 — 이걸로 실클라 회귀 네 판을 날렸다. 증상이 「접속 요청 158바이트에
    //   답이 0개」였는데 로그 어디에도 까닭이 없어 코드 회귀를 의심하고 이분법까지 돌렸다.
    //   **안 되는 것을 조용히 지나가게 두면 다음 사람이 반나절을 쓴다.** 여기서 죽인다.
    if (standalone && mapPath.empty()) {
        std::printf("[멈춤] --standalone 에는 --map 이 있어야 한다.\n"
                    "       맵이 없으면 접속 묶음을 만들 수 없어 **아무도 못 들어온다**\n"
                    "       (클라가 접속 요청을 되풀이하다 20초 뒤 조용히 끊는다).\n"
                    "  보기: --standalone --map \"C:\\...\\maps\\estate\\Estate.map\"\n");
        return 2;
    }

    if (dumpSections && mapPath.empty()) {
        std::printf("[멈춤] --dump-sections 에는 --map 이 있어야 한다.\n");
        return 2;
    }

    // ★ 진단 --dump-thing — 맵 없이 돌아야 하므로 맵 처리보다 **앞**에 둔다.
    //   찍는 것 = 클래스 원문·비트 · 서브클래스 원문 · 플래그 원문 · 복합 여부 · 그리기 함수.
    //   갈래를 정하는 것은 클래스 비트다(`GAME4_1.c:4539~4550`) —
    //     0x400000 → netSpriteUpdate · 0x200000 → (2 몬스터 / 4 사람 / 그밖 팬텀) · 0x100000 → 단순
    if (!dumpThing.empty()) {
        nox::ThingDb tdb;
        if (!tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
            std::printf("[멈춤] thing.bin 을 못 읽었다.\n");
            return 1;
        }
        uint16_t wire = 0;
        if (dumpThing[0] == '#') wire = static_cast<uint16_t>(std::atoi(dumpThing.c_str() + 1));
        else                     wire = tdb.WireByName(dumpThing);
        const nox::ThingInfo* ti = wire ? tdb.ByWire(wire) : nullptr;
        if (!ti) {
            std::printf("[멈춤] 그런 종류가 없다: %s\n", dumpThing.c_str());
            return 1;
        }
        const uint32_t cl = nox::ParseObjectClass(ti->classRaw);
        std::printf("종류  %s  (와이어 %u)\n", ti->name.c_str(), wire);
        std::printf("  클래스     %s\n             = 0x%08X\n", ti->classRaw.c_str(), cl);
        std::printf("  서브클래스 %s\n", ti->subclassRaw.empty() ? "(없음)" : ti->subclassRaw.c_str());
        std::printf("  플래그     %s\n", ti->flagsRaw.empty() ? "(없음)" : ti->flagsRaw.c_str());
        std::printf("  그리기     %s\n", ti->drawFn.empty() ? "(없음)" : ti->drawFn.c_str());
        std::printf("  복합표     %s\n", nox::ThingIsComplex(wire) ? "복합(덧붙이 붙음)" : "단순(덧붙이 없음)");
        // 갈래를 말로 적어 준다 — 매번 비트를 손으로 풀지 않으려고
        const char* lane = (cl & 0x400000) ? "netSpriteUpdate (정적·문·레버)"
                         : (cl & 0x200000) ? ((cl & 2) ? "netSendComplexObject (몬스터)"
                                           : (cl & 4) ? "netPlayerObjSend (사람)"
                                                      : "netSendPhantomPlrMb (그 밖 — 화살이 여기)")
                         : (cl & 0x100000) ? "netSendSimpleObj (단순)"
                                           : "(갱신 안 보냄)";
        std::printf("  정답지 갈래 %s\n", lane);
        // ★ 09-22 — **무게·몸**을 같이 찍는다. 사람 무게를 상수 1 로 박아 둔 것이 튕김의 뿌리였다
        //   (정답지 실측 15.60 · `NOX_DBG_PUSH=1` 로 대조군에서 읽었다). 0 이면 파일에 없다는 뜻이다
        std::printf("  무게(MASS) %.2f%s · 몸 모양 %d · 반지름 %.1f\n",
                    ti->mass, ti->mass == 0.0f ? "  ⚠ 파일에 없음 → 정답지 기본 1" : "",
                    ti->shapeKind, ti->radius);
        // ★ 09-22 — UPDATE·INIT 인자. **소리 이름이 여기 적혀 있는 종류가 있다**(밟는 판이 그랬다)
        std::printf("  UPDATE     %s\n", ti->updateFn.empty() ? "(없음)" : ti->updateFn.c_str());
        std::printf("  UPDATE 인자 %s\n", ti->updateArgs.empty() ? "(없음)" : ti->updateArgs.c_str());
        std::fflush(stdout);
        return 0;
    }

    if (!mapPath.empty()) {
        nox::MapFile mf;
        if (!mf.Load(mapPath)) {
            std::printf("맵을 못 읽음: %s (%s)\n", mapPath.c_str(), mf.Error().c_str());
            return 1;
        }
        // ★ 진단 --dump-sections — 섹션 목록만 찍고 **여기서 끝낸다**(서버를 안 띄운다).
        //   한 줄에 하나, 탭으로 갈라 찍는다 — 157개를 돌려 표로 합치려고.
        //     SEC <맵이름> <섹션이름> <크기>
        if (dumpSections) {
            std::string base = mapPath;
            const size_t sl = base.find_last_of("\\/");
            if (sl != std::string::npos) base = base.substr(sl + 1);
            for (const auto& s : mf.Sections())
                std::printf("SEC\t%s\t%s\t%zu\n", base.c_str(), s.name.c_str(), s.size);
            std::fflush(stdout);
            return 0;
        }
        const auto cells = mf.ReadWalls();
        // ★ 2026-09-13 묶음 2-b — 벽 **모양**(방향·재질)까지 넘긴다. 칸 좌표만 넘기던 SetWalls 는 시험용으로 남는다.
        std::vector<nox::Server::WallTileIn> ws;
        ws.reserve(cells.size());
        for (const auto& w : cells) {
            nox::Server::WallTileIn t;
            t.x = w.x; t.y = w.y; t.dir = w.dir; t.tile = w.tile; t.flag80 = w.flag80;
            ws.push_back(t);
        }
        srv.SetWallTiles(ws);
        {
            // ★ 2026-09-15 묶음 9-a — 비밀벽. 157개 맵 전부에 `SecretWalls` 칸이 있다(전수 확인)
            const auto sec = mf.ReadSecretWalls();
            std::vector<nox::Server::SecretWallIn> sw;
            sw.reserve(sec.size());
            for (const auto& w : sec) {
                nox::Server::SecretWallIn t;
                t.gx = w.gx; t.gy = w.gy; t.flags = w.flags;
                t.state = w.state; t.anim = w.anim; t.waitSecs = w.waitSecs;
                sw.push_back(t);
            }
            srv.SetSecretWalls(sw);
            std::printf("[맵] 비밀벽 %zu개 중 벽이 있는 칸 %zu개\n", sec.size(), srv.SecretWallCount());
            // ★ 09-20 9-a — **세상 좌표**로 앞 8곳을 찍는다. `--spawn-at`·`--debug-warp` 에 넣을 값이다
            //   (칸 → 세상 = 칸 × 23 + 11.5 · `Server.cpp:8946` 과 같은 셈)
            if (!sec.empty()) {
                std::printf("[비밀벽 자리]");
                for (size_t k = 0; k < sec.size() && k < 8; ++k)
                    std::printf(" (%d,%d 칸%d,%d)", static_cast<int>(sec[k].gx * 23 + 11),
                                static_cast<int>(sec[k].gy * 23 + 11),
                                static_cast<int>(sec[k].gx), static_cast<int>(sec[k].gy));
                std::printf("  — 모두 %zu곳\n", sec.size());
                // ★ 09-20 9-a — **스스로 여닫는 벽이 몇이나 되나.** 상태 기계는 깃발 4·8 이 정한다
                //   (`Server.cpp:8968·8982`). 둘 다 0 이면 **맵 각본이 열어 주기 전엔 아무 일도 안 난다** —
                //   각본은 플랜 v6 범위 밖이므로 그 맵에서는 9-a 를 걷기로 못 본다
                size_t f4 = 0, f8 = 0, both = 0, openNow = 0;
                for (const auto& w : sec) {
                    if (w.flags & 4) ++f4;
                    if (w.flags & 8) ++f8;
                    if ((w.flags & 4) && (w.flags & 8)) ++both;
                    if (w.state == 2 || w.state == 3) ++openNow;
                }
                std::printf("[비밀벽 깃발] 4(스스로 움직임) %zu · 8(열린 채 시작) %zu · 둘 다 %zu · 지금 열림 %zu\n",
                            f4, f8, both, openNow);
                // ⚠ 「설 자리」는 여기서 안 찾는다 — `CanStand` 는 **벽만** 보고 바닥은 못 봐서
                //   **바닥 없는 암반**을 골랐다(09-20 에 세 번 당함). 길목(WayPoints)을 읽은 뒤에 찾는다
                std::fflush(stdout);
            }
        }
        {
            // 벽 재질 깃발 — thing.bin WALL 섹션(0x2 = 막지 않음). 방향이 표(0~10) 밖인 벽 수도 찍는다.
            nox::ThingDb wdb;
            size_t badDir = 0;
            for (const auto& w : cells) if (w.dir > 10) ++badDir;
            if (wdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
                std::vector<uint32_t> flags;
                for (const auto& d : wdb.Walls()) flags.push_back(d.flags);
                srv.SetWallDefFlags(flags);
                // ★ 2-b2 — 광전사가 **세게** 부딪히는 재질(깃발 &5)이 몇 종이고, 그런 칸이 이 맵에 몇 개인가
                size_t hardDefs = 0, hardCells = 0;
                for (uint32_t f : flags) if (f & 0x5) ++hardDefs;
                for (const auto& w : cells) if (w.tile < flags.size() && (flags[w.tile] & 0x5)) ++hardCells;
                std::printf("[벽] 모양 %zu칸 · 재질 %zu종 · 방향 표 밖 %zu칸 · 광전사 세게(깃발&5) 재질 %zu종·%zu칸\n",
                            cells.size(), flags.size(), badDir, hardDefs, hardCells);
            } else {
                std::printf("[벽] 모양 %zu칸 · ⚠ thing.bin 을 못 읽어 재질 깃발 없음(모두 막는 벽으로 봄)\n", cells.size());
            }
        }
        // 정적 오브젝트 — **아이템만, 정품 번호로** (P4-d).
        //
        // ★ 두 규칙 다 실측이다 (2026-09-10):
        //   ① 정품 갱신·스냅샷에 실리는 정적은 아이템뿐(물약·무기·사과 — 장식 0건).
        //      나무·불꽃을 새 번호로 소개하면 클라에 이중 실체가 생겨 **첫 프레임에
        //      죽는다**(ao1~ao5, 문법·좌표 전수 유효였는데도).
        //   ② 번호 = 맵 ObjectData 색인 + 75 (BluePotion 101→176 · FanChakram 105→180).
        //      클라가 맵을 읽으며 같은 번호를 만들어 두므로 이 번호라야 실체가 합쳐진다.
        //      ⚠ +75 는 estate 실측 — 다른 맵은 미확인.
        nox::ThingDb tdb;
        size_t nStat = 0, nSkip = 0;
        std::map<std::string, int> notPickup;   // 아이템이 아니라 걸러진 것 — 이름별로
        // ★ 2026-09-18 ④⑯ — 맵에 **그려진** 판 물건(깃발·왕관·공)의 자리.
        //   우리 서버는 맵 깃발을 안 읽고 제 자리에 놓으므로, 그림과 실물이 얼마나
        //   어긋나는지 보려면 맵 쪽 좌표가 있어야 한다.
        std::vector<std::string> mapMatchObjs;
        std::map<std::string, int> noWeight;    // 아이템인데 WEIGHT 가 없어 걸러진 것
        if (tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
            // ★ 4-c3 — 주문이 **이름으로** 놓는 오브젝트의 와이어 종류를 넘겨준다
            //   (정답지도 `getNameId("ToxicCloud")` 처럼 이름으로 찾는다). 못 찾으면 0 이라 안 놓인다
            for (const char* nm : { "ToxicCloud", "MediumFlame", "Meteor", "MeteorShower", "MeteorExplode",
                                    "SmallFist", "MediumFist", "LargeFist",
                                    "SmallFlameCleanse", "MediumFlameCleanse", "FlameCleanse", "LargeFlameCleanse",
                                    "SmallBlueFlameCleanse", "MediumBlueFlameCleanse", "BlueFlameCleanse", "LargeBlueFlameCleanse",
                                    "ArachnaphobiaFocus", "SmallSpider", "Pixie" }) {
                const uint16_t w = tdb.WireByName(nm);
                srv.SetNamedType(nm, w);
                std::printf("[종류] %s = %u%s\n", nm, static_cast<unsigned>(w), w ? "" : " ⚠ 못 찾음");
            }
            // ★ 4-c3 — 주문이 **날리는** 것(불덩이 셋). 빠르기·깃발·닿음 인자를 전부 thing.bin 에서 읽는다.
            //   `COLLIDE = SparkExplosionCollide 192` 의 숫자가 터짐 크기를 다 정하므로 **짐작하지 않는다**.
            //   ★ 묶음 6-g(3b) — **크리처가 쏘는 것**도 같은 자리에서 읽는다.
            //     `ImpShot` 은 `SPEED = 400` · `COLLIDE = ProjectileCollide 5` 다
            //     ★ 묶음 9-b — **쏘는 덫이 쏘는 것**도 여기서 읽는다.
            //       `MercArcherArrow` 는 `SPEED = 800` · `COLLIDE = MonsterArrowCollide 10 10` 이다
            for (const char* nm : { "Fireball", "StrongFireball", "TitanFireball", "ImpShot", "HarpoonBolt",
                                    "MercArcherArrow" }) {
                const uint16_t w = tdb.WireByName(nm);
                const nox::ThingInfo* t2 = w ? tdb.ByWire(w) : nullptr;
                int amount = 0;
                if (t2 && !t2->collideArgs.empty()) amount = std::atoi(t2->collideArgs.c_str());
                if (t2) srv.SetProjType(nm, w, t2->speed, nox::ParseObjectFlags(t2->flagsRaw),
                                        static_cast<uint8_t>(amount < 0 ? 0 : (amount > 255 ? 255 : amount)));
                std::printf("[날리기 종류] %s = %u · SPEED %d · 닿음 %s %d · 그리기 %s%s\n", nm, static_cast<unsigned>(w),
                            t2 ? t2->speed : 0, t2 ? t2->collideFn.c_str() : "?", amount,
                            // ⑫ — 클라는 이 이름으로 그리는 함수를 고른다(`drawFuncs[name]`).
                            //   비어 있거나 모르는 이름이면 클라가 **아무것도 안 그린다**
                            (t2 && !t2->drawFn.empty()) ? t2->drawFn.c_str() : "(없음)",
                            t2 ? "" : " ⚠ 못 찾음");
            }
            // ★ 2026-09-18 ⑬ — **화살·쇠뇌살을 전수로 골라 서버에 알린다.**
            //   이 종류만 갱신에 스프라이트 프레임이 한 바이트 더 붙는다(Server.h `AddArrowType`).
            //   조건은 정답지 그대로 — `MISSILE`(클래스 0x1) **그리고** `ARROW|BOLT`(서브 0x30).
            //   ⚠ 이름으로 못 박지 않는다 — 맵마다 다른 화살이 나올 수 있고,
            //     「이름 여섯 개」로 박아 두었다가 빠뜨린 전례가 이 리포에 이미 있다.
            {
                size_t nArrow = 0;
                std::string names;
                for (uint16_t w = 1; w < nox::kThingCount; ++w) {
                    const nox::ThingInfo* t3 = tdb.ByWire(w);
                    if (!t3) continue;
                    if (!(nox::ParseObjectClass(t3->classRaw) & 0x1)) continue;          // MISSILE 아님
                    if (!(nox::WeaponClassBitsOf(t3->subclassRaw) & 0x30)) continue;     // ARROW|BOLT 아님
                    srv.AddArrowType(w);
                    ++nArrow;
                    if (nArrow <= 8) { if (!names.empty()) names += " "; names += t3->name; }
                }
                std::printf("[화살 종류] %zu가지 (프레임 바이트가 붙는다)%s%s\n", nArrow,
                            names.empty() ? "" : " — ", names.c_str());
            }
            std::vector<nox::Server::WorldStatic> st;
            std::vector<nox::Server::WorldProp> props;
            size_t propExtentTooBig = 0;   // ⑩ extent 가 15비트를 넘는 것(정적 조회로 못 가리킨다)
            const auto objs = mf.ReadObjects();
            if (!mf.Error().empty())
                std::printf("[맵] ⚠ 오브젝트 읽기가 도중에 멈춤: %s — 읽은 %zu개만 쓴다\n", mf.Error().c_str(), objs.size());
            // ★ 2026-09-13 묶음 3-c — 부활 자리(PlayerStart). 정답지 mapFindPlayerStart_4F7AB0 가 이 목록에서 고른다.
            {
                std::vector<std::pair<float, float>> starts;
                for (const auto& o : objs)
                    if (o.type == "PlayerStart") starts.push_back({ o.x, o.y });
                std::printf("[부활 자리] PlayerStart %zu곳 —", starts.size());
                // ★ 09-20 — **좌표도 찍는다.** 화면 판정에서 「방 안에 선 자리」가 늘 필요한데
                //   아무 데나 `--spawn-at` 하면 **바닥이 없는 암반**에 서서 화면이 통째로 검다(실측).
                //   PlayerStart 는 맵이 보증하는 「설 수 있는 자리」다
                for (size_t k = 0; k < starts.size() && k < 14; ++k)
                    std::printf(" (%d,%d)", static_cast<int>(starts[k].first), static_cast<int>(starts[k].second));
                std::printf("\n");
                srv.SetPlayerStarts(std::move(starts));
            }
            // ★ ④⑯ — 맵에 그려진 판 물건의 자리를 모아 둔다(출력은 아래 「맵 판물건」).
            for (const auto& o : objs) {
                if (o.type.find("Flag") == std::string::npos &&
                    o.type.find("Crown") == std::string::npos &&
                    o.type.find("GameBall") == std::string::npos) continue;
                char b[96];
                std::snprintf(b, sizeof(b), "%s=%d,%d", o.type.c_str(),
                              static_cast<int>(o.x), static_cast<int>(o.y));
                mapMatchObjs.push_back(b);
                // ⚠ `FlagMarker`(표시)는 빼고 **`Flag`(진짜 깃발)만** 모은다 —
                //   둘이 같은 자리에 겹쳐 있어 안 가르면 네 개로 센다.
                if (o.type == "Flag")
                    mapFlagPos.push_back({ static_cast<int>(o.x), static_cast<int>(o.y) });
            }
            // ★ 2026-09-15 묶음 10-a — **소리 표**. thing.bin `AUD ` 칸 927개가
            //   이름표 1,023개에 **100% 맞물린다**(빗나감 0 · 전수 확인).
            //   ⚠ 이 표가 없으면 **소리가 한 개도 안 나간다**(소리조각 수 0 이 정답지의 첫 문이다)
            {
                std::vector<nox::Server::SoundDefIn> sd;
                size_t miss = 0, withSamples = 0;
                for (const auto& d : tdb.Sounds()) {
                    const uint16_t id = nox::SoundByName(d.name.c_str());
                    if (id == 0) { ++miss; continue; }
                    nox::Server::SoundDefIn in;
                    in.id = id;
                    in.maxDist = d.maxDist;
                    in.merged = d.merged;
                    in.samples = d.samples;
                    in.flags = d.flags;
                    if (d.samples > 0) ++withSamples;
                    sd.push_back(in);
                }
                // ★★ 09-22 — **정품이 보내는데 thing.bin 에 없는 소리**를 손으로 채운다. 지금은 하나다.
                //   사슬 — 사용자가 「마나석 소리가 안 난다」고 짚었다 → 정답지는 소리 **230**(ManaRecharge)을
                //   낸다(`GAME4_3.c:6461`) → 그런데 thing.bin AUD **927개에 그 이름이 없다** →
                //   그러면 조각 0 이라 `PlaySoundAt` 이 버린다 →
                //   **대조군 서버에 계수기를 붙여 보니 정답지도 똑같이 0 으로 읽는다**
                //   (`[AUD] 선언 927개 · 230 조각 0 · 237 조각 3`) →
                //   **그런데 정품 와이어에는 230 이 10번 나간다**(`CAP_server_0906` 의 a6·a7 610건을 풀어 셈).
                //   ⇒ 대전제상 **④ 정품이 ③ C 대조보다 위**다. 정품을 따른다.
                //   거리 600 은 지어낸 값이 아니라 **정답지가 모든 소리에 주는 기본값**이다
                //   (`server/audio_event.go:75` `p.MaxDist = 600`).
                {
                    bool has230 = false;
                    for (const auto& in : sd) if (in.id == 230) { has230 = true; break; }
                    if (!has230) {
                        nox::Server::SoundDefIn in;
                        in.id = 230; in.maxDist = 600; in.merged = 0; in.samples = 1; in.flags = 0;
                        sd.push_back(in);
                        std::printf("[소리·메움] 230(ManaRecharge) — thing.bin 에 없어 손으로 채움"
                                    "(정품 와이어 근거 · 거리 600 은 정답지 기본값)\n");
                    }
                }
                srv.SetSoundDefs(sd);
                std::printf("[소리] AUD %zu개 · 이름표에 맞물림 %zu · 소리조각 있는 것 %zu%s\n",
                            tdb.Sounds().size(), sd.size(), withSamples,
                            miss ? " ⚠ 이름표에 없는 것 있음" : "");
                // ★ 09-22 — **정품이 실제로 보낸 번호** 중 우리 표에 없는 것을 센다.
                //   `_evidence/CAP_server_0906.txt` 의 `a6`·`a7` 610건을 풀어 얻은 번호들이다.
                //   우리 표에 없으면 `PlaySoundAt` 이 조용히 버린다 — 「소리가 안 들린다」의 뿌리
                {
                    const uint16_t seen[] = { 276,131,270,322,879,190,186,301,188,192,300,302,230,41,319,232,231,69 };
                    std::string missing;
                    for (uint16_t q : seen) {
                        bool ok = false;
                        for (const auto& in : sd) if (in.id == q && in.samples > 0) { ok = true; break; }
                        if (!ok) { missing += " "; missing += std::to_string(q); missing += "("; missing += nox::SoundName(q); missing += ")"; }
                    }
                    std::printf("[소리·정품이쓴것] %zu개 중 우리 표에 **없는 것**:%s\n",
                                sizeof(seen) / sizeof(seen[0]), missing.empty() ? " (none)" : missing.c_str());
                }
                // ★ 09-22 사용자 보고(「소리가 안 들린다」) — **우리가 부르는 번호가 표에 있나**를 찍는다.
                //   `PlaySoundAt` 은 소리조각이 0 이면 조용히 버리고, `maxDist` 가 0 이면 **거리에서 잘린다**.
                //   둘은 증상이 같아서(안 들림) 숫자로 갈라야 한다
                for (uint16_t q : { (uint16_t)230, (uint16_t)237, (uint16_t)239, (uint16_t)307 }) {
                    bool found = false;
                    for (const auto& in : sd)
                        if (in.id == q) {
                            std::printf("[소리·쓰는것] %u(%s) — 조각 %d · 들리는거리 %d%s\n",
                                        (unsigned)q, nox::SoundName(q), in.samples, in.maxDist,
                                        (in.samples <= 0 || in.maxDist <= 0) ? "  ⚠ 이래서 안 들린다" : "");
                            found = true; break;
                        }
                    if (!found) {
                        std::printf("[소리·쓰는것] %u(%s) — ⚠ **표에 아예 없다**\n", (unsigned)q, nox::SoundName(q));
                        // 이름이 다를 뿐일 수 있다 — 비슷한 이름을 같이 찍어 고를 수 있게 한다
                        std::printf("[소리·비슷한이름]");
                        for (const auto& d : tdb.Sounds())
                            if (d.name.find("Mana") != std::string::npos ||
                                d.name.find("Recharge") != std::string::npos ||
                                d.name.find("Obelisk") != std::string::npos)
                                std::printf(" %s(%u·조각%d)", d.name.c_str(),
                                            (unsigned)nox::SoundByName(d.name.c_str()), d.samples);
                        std::printf("\n");
                    }
                }
            }
            // ★ 2026-09-15 묶음 9-b — **쏘는 덫 여섯 종류**(전부 `UPDATE = SkullUpdate`).
            //   보는 쪽은 **맵이 아니라 `thing.bin` 의 `INIT = SkullInit <dx> <dy>`** 에 있다
            //   (오브젝트 초기자료는 종류의 것을 복사한 것 · `server/object.go:343~346`).
            //   (dx,dy) → 각도 표는 blob `0x587000 + 230072` 를 **직접 떠서** 읽은 값이다.
            {
                auto dirAngle = [](int dx, int dy) -> int {
                    if (dy < 0) return dx < 0 ? 160 : (dx == 0 ? 192 : 224);
                    if (dy == 0) return dx < 0 ? 128 : 0;
                    return dx < 0 ? 96 : (dx == 0 ? 64 : 32);
                };
                // ⭕ `ArrowTrapDamage` 는 gamedata.bin 실측 **25**(ARENA·SOLO 둘 다). 못 읽으면 0 — 지어내지 않는다
                int arrowDmg = 0;
                {
                    nox::Gamedata gdTrap;
                    if (gdTrap.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\gamedata.bin"))
                        arrowDmg = static_cast<int>(gdTrap.Float("ARENA", "ArrowTrapDamage", 0.0));
                }
                std::vector<nox::Server::ShootingTrapIn> traps;
                for (const auto& o : objs) {
                    const uint16_t w = tdb.WireByName(o.type);
                    const nox::ThingInfo* t2 = w ? tdb.ByWire(w) : nullptr;
                    if (!t2 || t2->updateFn != "SkullUpdate" || t2->initArgs.empty()) continue;
                    int dx = 0, dy = 0;
                    if (std::sscanf(t2->initArgs.c_str(), "%d %d", &dx, &dy) != 2) continue;
                    nox::Server::ShootingTrapIn tr;
                    tr.x = o.x;
                    tr.y = o.y;
                    tr.facing = static_cast<uint8_t>(dirAngle(dx, dy));
                    tr.proj = t2->updateArgs;
                    // FX 와 피해 덮어쓰기는 **화살 덫에만** 붙는다(`GAME5.c:6377~6382·6432~6435`)
                    if (o.type == "ArrowTrap1") { tr.fxKind = 1; tr.damage = arrowDmg; }
                    else if (o.type == "ArrowTrap2") { tr.fxKind = 2; tr.damage = arrowDmg; }
                    traps.push_back(tr);
                    trapPos.push_back({ tr.x, tr.y });
                }
                if (!traps.empty()) {
                    std::printf("[맵] 쏘는 덫 %zu개 · 화살 피해 %d\n", traps.size(), arrowDmg);
                    // ★ 2026-09-18 ⑫ — **덫이 어디 있는지** 한 줄로 찍는다.
                    //   「겨눈 덫 0」이 나왔을 때 「덫이 안 도는 것」과 「사람이 사정거리
                    //   밖인 것」을 못 가른다. 좌표가 있어야 갈린다(덫 사정 ±350).
                    //   앞 여덟만 — 100개를 다 찍으면 로그가 읽히지 않는다.
                    // ★ 09-20 ⑮ — **방향도 찍는다.** 덫은 겨눈 뒤 **제 방향으로만** 쏜다
                    //   (`TrapShoot` — 대상 쪽이 아니다). 화살 선 위에 세우려면 방향이 있어야 한다.
                    //   앞 여덟만 — 100개를 다 찍으면 로그가 안 읽힌다
                    std::printf("[덫 자리]");
                    {
                        size_t k = 0;
                        for (const auto& tr : traps) {
                            if (k < 8) std::printf(" %d,%d(방향%u)", static_cast<int>(tr.x), static_cast<int>(tr.y),
                                                   static_cast<unsigned>(tr.facing));
                            else std::printf(" %d,%d", static_cast<int>(tr.x), static_cast<int>(tr.y));
                            ++k;
                        }
                    }
                    std::printf("\n");
                }
                srv.SetShootingTraps(traps);
            }
            // ★ 4-e — 맵 길목(WayPoints). 점멸이 여기서 목적지를 고른다.
            //   `flags2 & 0x80`(오가는 이음이 있음) 이고 `flags & 1`(켜짐)인 것만.
            {
                const auto allWps = mf.ReadWaypoints();
                std::vector<std::pair<float, float>> wps;
                for (const auto& w : allWps)
                    if ((w.flags2 & 0x80) && (w.flags & 1)) wps.push_back({ w.x, w.y });
                // ★ 09-20 묶음 6-j(1) — **이음을 세어 찍는다.** 떠돌기가 다음 길목을 고르는 밑천이라
                //   「0 이면 못 고른다」가 바로 보여야 한다. 막다른 길(나가는 이음 0)도 같이 센다
                {
                    size_t linkN = 0, deadEnd = 0;
                    for (const auto& w : allWps) { linkN += w.links.size(); if (w.links.empty()) ++deadEnd; }
                    std::printf("[길목 이음] 길목 %zu개 · 나가는 이음 %zu개 · 막다른 길 %zu개\n",
                                allWps.size(), linkN, deadEnd);
                }
                std::printf("[길목] 점멸이 고를 수 있는 곳 %zu곳 —", wps.size());
                // ★ 09-20 — **좌표도 찍는다.** 길목은 맵 제작자가 「걸을 수 있다」고 찍어 둔 자리라
                //   화면 판정에서 **설 자리**로 쓸 수 있다(`CanStand` 는 벽만 보고 바닥은 못 본다)
                for (size_t k = 0; k < wps.size() && k < 12; ++k)
                    std::printf(" (%d,%d)", static_cast<int>(wps[k].first), static_cast<int>(wps[k].second));
                std::printf("\n");
                // ★ 09-20 9-a — 비밀벽마다 **가장 가까운 길목**을 찍는다. 그게 「거기서 보려면 어디에 설까」의 답이다.
                //   ⚠ `CanStand` 로 찾으면 안 된다 — 그건 **벽만** 보고 바닥은 못 봐서 암반을 고른다(세 번 당함)
                if (!wps.empty()) {
                    const auto sec2 = mf.ReadSecretWalls();
                    size_t shown = 0;
                    for (const auto& w : sec2) {
                        if (shown >= 4) break;
                        const double wx = w.gx * 23.0 + 11.5, wy = w.gy * 23.0 + 11.5;
                        double best = 1e18; size_t bi = 0;
                        for (size_t k = 0; k < wps.size(); ++k) {
                            const double dx = wps[k].first - wx, dy = wps[k].second - wy;
                            const double d2 = dx * dx + dy * dy;
                            if (d2 < best) { best = d2; bi = k; }
                        }
                        std::printf("[비밀벽 설자리] 벽 칸%d,%d (%d,%d) ← 가장 가까운 길목 (%d,%d) 거리 %d\n",
                                    static_cast<int>(w.gx), static_cast<int>(w.gy),
                                    static_cast<int>(wx), static_cast<int>(wy),
                                    static_cast<int>(wps[bi].first), static_cast<int>(wps[bi].second),
                                    static_cast<int>(std::sqrt(best)));
                        ++shown;
                    }
                    std::fflush(stdout);
                }
                // ★ 09-20 ⑥ — **벽이 낀 길목 짝**을 찾아 찍는다. 소환수가 「벽을 돌아가나」를 보려면
                //   서로 **안 보이는**(선 추적이 막히는) 두 자리가 있어야 한다.
                //   ⚠ 너무 멀면 적을 못 찾고, 너무 가까우면 돌아갈 것도 없다 — 200~500 만 본다
                {
                    size_t shown = 0;
                    for (size_t i = 0; i < wps.size() && shown < 3; ++i) {
                        for (size_t j = i + 1; j < wps.size() && shown < 3; ++j) {
                            const double dx = wps[j].first - wps[i].first, dy = wps[j].second - wps[i].second;
                            const double dist = std::sqrt(dx * dx + dy * dy);
                            if (dist < 200.0 || dist > 500.0) continue;
                            if (srv.DebugTraceRay(wps[i].first, wps[i].second, wps[j].first, wps[j].second)) continue;  // 보이면 벽이 안 낀 것
                            std::printf("[벽낀 짝] (%d,%d) ↔ (%d,%d) 거리 %d — 서로 안 보인다\n",
                                        static_cast<int>(wps[i].first), static_cast<int>(wps[i].second),
                                        static_cast<int>(wps[j].first), static_cast<int>(wps[j].second),
                                        static_cast<int>(dist));
                            ++shown;
                        }
                    }
                    std::fflush(stdout);
                }
                // ★ 09-20 9-b — 덫도 같은 셈. **덫 옆에 설 자리**가 있어야 화살이 오는 걸 본다
                //   (덫 사정 ±350 이라 너무 멀면 안 쏘고, 너무 가까우면 화살 궤적이 안 보인다)
                if (!wps.empty() && !trapPos.empty()) {
                    size_t shown = 0;
                    for (const auto& t : trapPos) {
                        if (shown >= 4) break;
                        double best = 1e18; size_t bi = 0;
                        for (size_t k = 0; k < wps.size(); ++k) {
                            const double dx = wps[k].first - t.first, dy = wps[k].second - t.second;
                            const double d2 = dx * dx + dy * dy;
                            if (d2 < best) { best = d2; bi = k; }
                        }
                        const int dist = static_cast<int>(std::sqrt(best));
                        if (dist > 350) continue;          // 덫 사정 밖이면 볼 수 없다
                        std::printf("[덫 설자리] 덫 (%d,%d) ← 가장 가까운 길목 (%d,%d) 거리 %d\n",
                                    static_cast<int>(t.first), static_cast<int>(t.second),
                                    static_cast<int>(wps[bi].first), static_cast<int>(wps[bi].second), dist);
                        ++shown;
                    }
                    std::fflush(stdout);
                }
                // ★ 09-20 묶음 6-j(2) — **이음까지 실어 준다.** 거르는 눈은 위 `wps` 와 같다
                //   (`flags&1` 켜짐 · `flags2&0x80` 오가는 이음 있음) — 걸러진 길목을 가리키는
                //   이음은 서버에서 못 찾게 되는데, 그게 정답지 `sub_547EE0` 의 거짓과 같은 뜻이다
                {
                    std::vector<nox::Waypoint> full;
                    full.reserve(wps.size());
                    for (const auto& w : allWps) {
                        if (!((w.flags2 & 0x80) && (w.flags & 1))) continue;
                        nox::Waypoint sw;
                        sw.id = w.id;
                        sw.x = w.x;
                        sw.y = w.y;
                        for (const auto& l : w.links) sw.links.push_back({ l.to, l.flag });
                        full.push_back(std::move(sw));
                    }
                    srv.SetWaypointsFull(std::move(full));
                }
            }
            for (size_t i = 0; i < objs.size(); ++i) {
                const uint16_t wire = tdb.WireByName(objs[i].type);
                const nox::ThingInfo* ti = wire ? tdb.ByWire(wire) : nullptr;
                if (!ti) { ++nSkip; continue; }
                // ★ 아이템이 아닌 것도 **이름을 세어 둔다.** 전에는 조용히 버려서
                //   「맵에 무엇이 있는지」를 아무도 몰랐다 — estate 161개 중 149개가
                //   여기서 사라지는데 문·오벨리스크가 그 안에 있는지조차 못 봤다.
                // ★★ **문과 오벨리스크는 세계에 싣는다** (P5, 2026-09-11).
                //   estate 실측 — WoodenDoor 3 · ArchedHalfDoor 2 · ObeliskPrimitive 2.
                //   아이템이 아니라 여태 조용히 버려졌다. 이것들은 집는 게 아니라
                //   **다루는** 것이다 — 문은 다가가면 열리고, 오벨리스크는 옆에 서면 마나가 찬다.
                {
                    // ★ 09-23 nine-defects 1-c — 문은 **이름이 아니라 클래스 DOOR(0x80)** 로 가른다.
                    //   이름에 "Door" 가 든 것만 실었더니 Estate 의 `Dilapidated`(DOOR+IMMOBILE · DoorDraw/DoorUpdate)가
                    //   빠졌다 — 맵의 `DoorCollide` 7 · 실은 문 6(09-23 부팅 로그). 사람 판 「문을 그냥 통과」
                    //   (우리가 아는 문에 142 안으로 간 적 없음)의 후보다. 문 몸 싣기(아래 「문 종류도 싣는다」)도 같은 기준이다
                    const bool isDoor = (nox::ParseObjectClass(ti->classRaw) & 0x80) != 0;
                    if (!isDoor && objs[i].type.find("Door") != std::string::npos)   // 옛 기준과 갈리는 것은 드러낸다
                        std::printf("[문아님] 이름에 Door 가 있지만 클래스 DOOR 가 아니다 — %s (%d,%d)\n",
                                    objs[i].type.c_str(), static_cast<int>(objs[i].x), static_cast<int>(objs[i].y));
                    const bool isObelisk = objs[i].type.find("Obelisk") != std::string::npos;
                    // ★ 2026-09-15 묶음 9-c — **밟는 판**(`PressurePlate`).
                    //   ⚠ 보이지 않는 `Trigger`(735)는 정답지도 **아무것도 안 보내므로** 안 싣는다
                    const bool isPlate = (objs[i].type == "PressurePlate");
                    if (isDoor || isObelisk || isPlate) {
                        nox::Server::WorldProp wp;
                        // ★★ 09-20 ⑩ — **번호는 맵의 `extent` 다.** 지어내면 클라가 못 찾는다.
                        //   클라는 이 물건들을 **맵 파일에서 제가 만들고**(`GAME3.c:3516`
                        //   `clientLoadSomeObject_4AC6E0`), 그때 `Drawable+128`(= `NetCode32`) 에
                        //   **오브젝트 기록의 첫 u32 = extent** 를 넣는다(`GAME3.c:3370~3380`).
                        //   전에는 `i + 75` 를 썼다 — 실측으로 클라가 **「그림 못 찾음」** 이었다.
                        //   ⚠ 정적 조회는 **15비트**(`code & 0x7FFF`)라 그 위는 못 가리킨다
                        if (objs[i].extent > 0x7FFFu) ++propExtentTooBig;
                        wp.netCode = static_cast<uint16_t>(objs[i].extent & 0x7FFFu);
                        // ⑩ 클라가 «정적» 목록에서 찾게 할지 — `CLASS` 에 `IMMOBILE` 이 있으면 그렇다
                        wp.staticLookup = ti && ti->isImmobile;
                        wp.type = wire;
                        wp.x = static_cast<int>(objs[i].x);
                        wp.y = static_cast<int>(objs[i].y);
                        wp.kind = isPlate ? nox::PropKind::Plate
                                          : (isDoor ? nox::PropKind::Door : nox::PropKind::Obelisk);
                        // ★ 09-22 — 문 종류 이름을 찍는다(문의 **몸**을 thing.bin 에서 찾으려면 이름이 있어야 한다)
                        // ⭕ Estate 의 문 두 종류 — `WoodenDoor`(306) · `ArchedHalfDoor`(309).
                        //   둘 다 `DOOR+IMMOBILE` · 몸은 **원 반지름 32** · thing.bin 에 MASS 없음(기본 1)
                        // ★ 09-22 — **문 짐을 찍는다.** 정품 캡처가 문 번호별 각도를 주므로
                        //   (`8bdf 00 · 8be0 10 · 8be1 18 · 8be2 18 · 8be3 08`) 어느 바이트가 각도인지 맞댈 수 있다
                        if (isDoor && objs[i].tail.size() >= 2) {
                            wp.angle = objs[i].tail[1];
                            // 미세각은 거친각 × 8 에서 시작한다(거친각 = 32 × 미세 / 256)
                            wp.fine = static_cast<uint8_t>((static_cast<int>(wp.angle) * 8) & 255);
                            // ⚠ 여는 거리는 **반지름을 같이 찍는다** — 진짜로 문 몸에서 나온 42 와
                            //   못 찾아 떨어진 기본값 42 가 숫자로는 똑같아서 구분이 안 된다
                            std::printf("[문각도] 번호 %04x %s (%d,%d) — 닫힘 %u · 열림 %u · 여는거리 %.0f(문반지름 %s)\n",
                                        (unsigned)((objs[i].extent & 0x7FFFu) | 0x8000u), objs[i].type.c_str(),
                                        static_cast<int>(objs[i].x), static_cast<int>(objs[i].y),
                                        (unsigned)wp.angle, (unsigned)nox::DoorOpenAngle(wp.angle),
                                        ti && ti->radius > 0.0f ? nox::kBodyRadius + ti->radius : nox::kDoorOpenDist,
                                        ti && ti->radius > 0.0f
                                            ? (std::to_string(static_cast<int>(ti->radius))).c_str()
                                            : "못 찾음 → 기본값");
                        }
                        // ★ 묶음 10-b — 누르고 떼는 소리는 `UPDATE = TriggerUpdate <누름> <뗌>` 에 있다.
                        //   ⭕ 실측 — `PressurePlate` 는 `TriggerPressed`(750) · `TriggerReleased`(751)
                        if (isPlate && ti) {
                            const std::string& ua = ti->updateArgs;
                            const size_t sp = ua.find(' ');
                            if (sp != std::string::npos) {
                                wp.sndPressed = nox::SoundByName(ua.substr(0, sp).c_str());
                                wp.sndReleased = nox::SoundByName(ua.substr(sp + 1).c_str());
                            }
                        }
                        props.push_back(wp);
                        continue;
                    }
                }
                // ★★ 묶음 5-h 정정 — **금은 이 두 문을 다 못 지난다.**
                //   `Gold` 는 `CLASS = LIGHT+SIMPLE` 이라 우리 `isPickup` 이 거짓이고, `WEIGHT` 도 **없다**.
                //   그런데도 금은 세계에 있어야 한다 — 커서로 집는 게 아니라 **밟고 지나가면 들어오기**
                //   때문이다(`COLLIDE = PickupCollide`). 클라의 `ClassPickup`(WEIGHT) 기준은
                //   **커서로 집는 것**에만 걸리는 문이다. 그래서 금은 두 문을 비켜 간다.
                //   ⇒ 5-h 에서 「estate 에 금이 없다」고 적었는데, **우리 문이 막고 있던 것**도 겹쳐 있었다.
                const bool isGold = (nox::PickupFnOf(ti->pickupFn) == nox::PickupFn::Gold);
                if (!isGold) {
                if (!ti->isPickup) { ++notPickup[objs[i].type]; continue; }
                // ★★ **클라가 집을 수 있는 것만 싣는다.**
                //   클라는 `WEIGHT` 가 있는 물건에만 `ClassPickup` 을 붙인다
                //   (`client/object_type.go:355-362`, 2026-09-11 소스 확인).
                //   우리 CLASS 기준과 어긋나는 것이 10종 있다(화살·차크람 같은 날아가는 것).
                //   실어 봐야 클라가 못 집으니 세계에 놓지 않는다.
                if (!ti->hasWeight) { ++noWeight[objs[i].type]; continue; }
                }   // isGold 면 위 두 문을 비켜 간다
                // 겉모습(P7) — 입었을 때 남에게 알릴 장비 비트. 표에 없으면 0(안 알림).
                bool isW = false;
                const uint32_t ebit = nox::EquipBitFor(objs[i].type.c_str(), &isW);
                nox::Server::WorldStatic w2;
                w2.netCode = static_cast<uint16_t>(i + 75);
                w2.type = wire;
                w2.x = static_cast<int>(objs[i].x);
                w2.y = static_cast<int>(objs[i].y);
                // 수식이 붙는 갈래인가 — 주울 때 `4b` 가 아니라 `4c` 로 알려야 한다.
                // CLASS 원문으로 가른다(`WEAPON`·`ARMOR`·`WAND`·`FLAG`).
                w2.modifiable =
                    ti->classRaw.find("WEAPON") != std::string::npos ||
                    ti->classRaw.find("ARMOR")  != std::string::npos ||
                    ti->classRaw.find("WAND")   != std::string::npos ||
                    ti->classRaw.find("FLAG")   != std::string::npos;
                // 쓰면 무슨 일이 나는가 — `USE = PotionUse 50` 을 그대로 옮긴다.
                // ⚠ 숫자를 회복량으로 읽어도 되는 갈래는 둘뿐이다(ThingDb.h 주석).
                //   ⚠ 「무엇을 하는가」는 이제 `SUBCLASS` 비트가 정한다(아래 `foodBits`) —
                //     예전엔 SUBCLASS 글자에 "MANA" 가 들었나로 갈랐는데, 그러면 갈래 열일곱을 못 가른다.
                // ★ 묶음 5-d — **무엇을 부를지**는 `USE` 의 앞말이 정한다. 값은 갈래마다 뜻이 다르다.
                //   ⚠ `ConsumeUse` 값은 **음수일 수 있다**(실측 −15) — 먹으면 깎이는 음식이다.
                w2.useFn = nox::UseFnOf(ti->useHandler);
                if (w2.useFn != nox::UseFn::None) w2.useAmount = ti->useArg;
                // ★ 묶음 5-e — `WandCastUse <충전> <초> <주문이름>` 을 갈라 담는다.
                //   ⭕ 실측 — `250 1.0 SPELL_PLASMA` · `5 1.0 SPELL_DEATH_RAY` ·
                //     `60 1.0 SPELL_CHAIN_LIGHTNING` · `3 0.2 SPELL_FORCE_OF_NATURE`.
                //   주문 번호는 **SPEL 섹션의 순번**이다(우리 주문 번호와 같은 셈).
                if (w2.useFn == nox::UseFn::WandCast) {
                    int ch = 0; float sec = 0.0f; char nm[64] = { 0 };
                    if (std::sscanf(ti->useArgsRaw.c_str(), "%d %f %63s", &ch, &sec, nm) == 3) {
                        w2.chargesMax = static_cast<uint8_t>(ch < 0 ? 0 : (ch > 255 ? 255 : ch));
                        w2.charges = w2.chargesMax;
                        w2.wandCooldown = static_cast<uint32_t>(sec * nox::kServerTickRate);
                        for (size_t si = 0; si < tdb.Spells().size(); ++si)
                            if (tdb.Spells()[si].name == nm) { w2.wandSpell = static_cast<uint8_t>(si); break; }
                        if (w2.wandSpell == 0)
                            std::printf("[완드] ⚠ %s 의 주문 %s 을 못 찾음\n", objs[i].type.c_str(), nm);
                    }
                }
                w2.equipBit = ebit;
                w2.isWeapon = isW;
                // ★ 묶음 5 — 줍기 한도를 재는 두 값. 여기까지 온 물건은 `hasWeight` 가 참이다.
                w2.weight = static_cast<uint8_t>(ti->weight < 0 ? 0 : (ti->weight > 255 ? 255 : ti->weight));
                w2.isFood = ti->classRaw.find("FOOD") != std::string::npos;
                // ★ 묶음 5-b — 물약이 무엇을 하는지는 `SUBCLASS` 비트에 있다(`POTION+HEALTH_POTION` 꼴).
                w2.foodBits = nox::FoodClassBitsOf(ti->subclassRaw);
                // ★ 묶음 5-c — 줍는 순간 부르는 함수. 물약 갈래만 그 자리에서 마실 수 있다.
                // ★ 묶음 5-h — 금 더미도 같은 표에서 갈린다.
                //   ⚠ 금액은 맵 오브젝트 **초기자료**(GoldInitData.Amount)라 아직 못 읽는다(0 으로 둔다).
                const nox::PickupFn pf = nox::PickupFnOf(ti->pickupFn);
                w2.potionPickup = (pf == nox::PickupFn::Potion);
                w2.goldPickup = (pf == nox::PickupFn::Gold);
                st.push_back(w2);
            }
            nStat = st.size();
            srv.SetWorldStatics(std::move(st));

            // ★ 묶음 5 — 사람이 들 수 있는 무게의 **밑천**. thing.bin 의 사람 칸에서 읽는다.
            //   한도는 이것의 두 배다. 못 찾으면 그대로 두고(기본 100) 표시줄에 알린다.
            {
                // ⭕ 사람 종류 이름은 **`NewPlayer`** 다(thing.bin `GNHT` 다음 이름 · `CLASS = PLAYER+COMPLEX+LIGHT`).
                //   ⚠⚠ 다만 이 `CARRYCAPACITY = 100` 은 **사람의 들 무게가 아니다** — 오브젝트를 만들 때의
                //     기본값일 뿐이고(`server/object.go:330`), 사람은 접속 때 `plrReadVals_4EEDC0` 가
                //     **힘에서 다시 계산해 덮어쓴다**(`GAME3_3.c:7704` · 위저드 1650). 그래서 여기서는
                //     **찍어 보이기만** 하고 서버에 넣지 않는다. 실제 값은 `Session::carryCap` 이다.
                const uint16_t pw = tdb.WireByName("NewPlayer");
                const nox::ThingInfo* pt = pw ? tdb.ByWire(pw) : nullptr;
                std::printf("[들 무게] thing.bin NewPlayer CARRYCAPACITY %d (오브젝트 기본값 — 사람은 힘에서 다시 셈: 위저드 %d → 한도 %d)\n",
                            pt ? pt->carryCap : -1,
                            nox::CarryCapacityOnline(nox::CharClass::Wizard),
                            nox::CarryCapacityOnline(nox::CharClass::Wizard) * 2);
                // 맵에 놓인 물건의 무게를 이름별로 한 줄 — 「몇 개나 들 수 있나」를 눈으로 재려고.
                std::map<std::string, std::pair<int, int>> wsum;   // 이름 → {무게, 개수}
                for (const auto& o : objs) {
                    const uint16_t w3 = tdb.WireByName(o.type);
                    const nox::ThingInfo* t3 = w3 ? tdb.ByWire(w3) : nullptr;
                    if (!t3 || !t3->isPickup || !t3->hasWeight) continue;
                    auto& e = wsum[o.type];
                    e.first = t3->weight;
                    ++e.second;
                }
                std::printf("[들 무게] 맵 물건 무게 —");
                for (const auto& kv : wsum)
                    std::printf(" %s %d(×%d)", kv.first.c_str(), kv.second.first, kv.second.second);
                std::printf("\n");
            }

            // ★ 2026-09-13 묶음 2-c — **장애물**(오브젝트끼리 충돌). COLLIDE 가 있고 NO_COLLIDE 가 아닌
            //   IMMOBILE(질량 1e10, `server/object_type.go:337`) 오브젝트. 문·아이템은 뺀다(묶음 9·5 — Server.h 주석).
            {
                std::vector<nox::Server::PhysBody> obs;
                std::map<std::string, int> obsNames;
                std::map<std::string, int> hazardNames;   // ★ 1-d — 닿음 피해가 실린 장애물(종류(양 종류번호) 별)
                std::vector<std::string> hazardSpots;     // ★ 1-d — 그 자리 앞 몇 개
                for (const auto& o : objs) {
                    const uint16_t w = tdb.WireByName(o.type);
                    const nox::ThingInfo* t2 = w ? tdb.ByWire(w) : nullptr;
                    if (!t2 || !t2->hasCollide) continue;
                    const uint32_t fl = nox::ParseObjectFlags(t2->flagsRaw);
                    const uint32_t cl = nox::ParseObjectClass(t2->classRaw);
                    if (fl & 0x40) continue;                      // NO_COLLIDE
                    if (!(cl & 0x400000)) continue;               // IMMOBILE 만
                    if (cl & 0x80) continue;                      // 문 — 묶음 9
                    if (t2->shapeKind != 2 && t2->shapeKind != 3) continue;
                    nox::Server::PhysBody b;
                    b.x = o.x; b.y = o.y;
                    b.shape = static_cast<uint8_t>(t2->shapeKind);
                    b.r = t2->radius;
                    if (b.shape == 3) nox::Server::CalcBox(b, t2->boxW, t2->boxH);
                    b.flags = fl; b.cls = cl;
                    b.mass = 1e10f;
                    // ★ 09-23 nine-defects 1-d — 닿음 피해(`collideDamageLoad_536E10`): 첫 칸 = 양(u8 로 자름) · 둘째 칸 = 종류 이름.
                    //   이름을 못 찾으면(18) 정답지는 읽기 실패로 본다 — 우리는 피해를 안 싣고 드러낸다
                    if (t2->collideFn == "DamageCollide") {
                        const std::string& args = t2->collideArgs;
                        const size_t sp = args.find(' ');
                        const int amount = std::atoi(args.c_str());
                        const int kind = nox::DamageTypeByName(sp == std::string::npos ? std::string() : args.substr(sp + 1));
                        if (kind >= nox::kDamageTypeCount) {
                            std::printf("[닿음피해] ⚠ %s — 종류 이름을 못 읽음(`%s`) · 피해를 안 싣는다\n", o.type.c_str(), args.c_str());
                        } else {
                            b.contactDmgAmount = static_cast<uint8_t>(amount);
                            b.contactDmgType = static_cast<uint8_t>(kind);
                            ++hazardNames[o.type + "(" + std::to_string(static_cast<unsigned>(b.contactDmgAmount)) +
                                          " 종류" + std::to_string(kind) + ")"];
                            // 자리도 몇 개 적는다 — 사람이 「어느 불」인지 찾고, `--spawn-at` 으로 실물 시험을 하려면 좌표가 있어야 한다
                            if (hazardSpots.size() < 8)
                                hazardSpots.push_back(o.type + "(" + std::to_string(static_cast<int>(o.x)) + "," +
                                                      std::to_string(static_cast<int>(o.y)) + ")");
                        }
                    }
                    obs.push_back(b);
                    ++obsNames[o.type];
                }
                if (!hazardNames.empty()) {
                    size_t n = 0;
                    for (const auto& kv : hazardNames) n += static_cast<size_t>(kv.second);
                    std::printf("[닿음피해] 장애물 %zu개 —", n);
                    for (const auto& kv : hazardNames) std::printf(" %sx%d", kv.first.c_str(), kv.second);
                    std::printf(" · 자리(앞 %zu개)", hazardSpots.size());
                    for (const auto& sp : hazardSpots) std::printf(" %s", sp.c_str());
                    std::printf("\n");
                }
                std::printf("[장애물] %zu개 / %zu종 —", obs.size(), obsNames.size());
                for (const auto& kv : obsNames) std::printf(" %sx%d", kv.first.c_str(), kv.second);
                std::printf("\n");
                // ★ 09-20 ② — **상자 자리**를 따로 찍는다. 「몸이 막히나」를 보려면 좌표가 있어야 한다
                {
                    size_t nChest = 0;
                    for (const auto& o : objs) {
                        if (o.type.find("Chest") == std::string::npos) continue;
                        if (nChest == 0) std::printf("[상자 자리]");
                        if (nChest < 6) std::printf(" %s(%d,%d)", o.type.c_str(),
                                                    static_cast<int>(o.x), static_cast<int>(o.y));
                        ++nChest;
                    }
                    if (nChest) { std::printf("  — 모두 %zu개\n", nChest); std::fflush(stdout); }
                }
                if (noObstacles) {
                    std::printf("[진단스위치] --no-obstacles — 장애물 %zu개를 싣지 않음(대조군)\n", obs.size());
                    obs.clear();
                }
                srv.SetObstacles(std::move(obs));
                // 소환수 몸 — 몬스터 종류 전부(원만)
                for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                    const nox::ThingInfo* t2 = tdb.ByWire(w);
                    // ★ 2026-09-13 묶음 3-e — COLLIDE 없는 몬스터·상자 몸도 싣는다. 공격 고르기(sub_5180B0)와 피해 함수는
                    //   충돌 함수와 무관하다. 걷기 충돌은 hasCollide 로 따로 거른다(`GAME5.c:1930` 둘 다 있어야 부딪힘).
                    // ★ 09-22 — **문 종류도 싣는다.** 문 여는 거리가 「사람 반지름 + 문 반지름」이라
                    //   그 몸이 필요하다. 전에는 몬스터만 실어 조회가 조용히 헛돌았다(기본값으로 떨어짐)
                    if (!t2) continue;
                    const bool isDoorType = (nox::ParseObjectClass(t2->classRaw) & 0x80) != 0;
                    if (!t2->isMonster && !isDoorType) continue;
                    nox::Server::PhysBody b;
                    b.shape = static_cast<uint8_t>((t2->shapeKind == 2 || t2->shapeKind == 3) ? t2->shapeKind : 0);
                    b.r = t2->radius;
                    if (b.shape == 3) nox::Server::CalcBox(b, t2->boxW, t2->boxH);
                    b.flags = nox::ParseObjectFlags(t2->flagsRaw);
                    b.cls = nox::ParseObjectClass(t2->classRaw);
                    b.mass = (t2->mass > 0.0f) ? t2->mass : 1.0f;   // 정답지 기본 1(`server/object_type.go:197`)
                    b.subclass = nox::Server::MonsterClassBitsOf(t2->subclassRaw);
                    b.damageFn = nox::Server::DamageFnOf(t2->damageFn);
                    b.hasCollide = t2->hasCollide;
                    srv.SetTypeBody(w, b);
                }
                // ★ 3-b — 날아다니는 몬스터(FLAGS AIRBORNE 0x4000) — 지진이 빼는 종류다. 이름 앞 몇 개를 같이 찍는다
                {
                    size_t nFly = 0;
                    std::string names;
                    for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                        const nox::ThingInfo* t2 = tdb.ByWire(w);
                        if (!t2 || !t2->isMonster || !(nox::ParseObjectFlags(t2->flagsRaw) & 0x4000)) continue;
                        if (nFly < 12) names += " " + t2->name;
                        ++nFly;
                    }
                    std::printf("[나는몬스터] AIRBORNE 몬스터 %zu종 —%s%s\n", nFly, names.c_str(), nFly > 12 ? " …" : "");
                }
            }
            // ★ 2026-09-13 묶음 3-d — TRY_COLLIDE 가 찾을 것(맵 오브젝트 번호·extent·종류 · 종류별 충돌 함수 이름)
            //   · 죽음 알림의 무기 그림(무기 클래스 비트 → 종류 번호)
            {
                auto firstWord = [](const std::string& raw) {
                    size_t b = raw.find_first_not_of(" \t");
                    if (b == std::string::npos) return std::string();
                    size_t e = raw.find_first_of(" \t", b);
                    return raw.substr(b, e == std::string::npos ? std::string::npos : e - b);
                };
                std::vector<nox::Server::MapObjRef> refs;
                std::map<std::string, int> fnNames;   // 맵에 놓인 것의 충돌 함수별 개수(진단)
                for (size_t i = 0; i < objs.size(); ++i) {
                    const uint16_t w = tdb.WireByName(objs[i].type);
                    if (!w) continue;
                    nox::Server::MapObjRef r;
                    r.netCode = static_cast<uint16_t>(i + 75);
                    r.extent = objs[i].extent;
                    r.type = w;
                    refs.push_back(r);
                    const nox::ThingInfo* t2 = tdb.ByWire(w);
                    if (t2 && t2->hasCollide) ++fnNames[firstWord(t2->collideRaw)];
                }
                size_t nFn = 0;
                for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                    const nox::ThingInfo* t2 = tdb.ByWire(w);
                    if (!t2 || !t2->hasCollide) continue;
                    srv.SetCollideFn(w, firstWord(t2->collideRaw));
                    ++nFn;
                }
                for (const auto& wp : nox::kWeapons) srv.SetWeaponWire(wp.classBits, tdb.WireByName(wp.name));
                // ★ 4-a2 — 겨눈 주문이 날리는 "Magic"(SPEED · MASS)
                if (const uint16_t mw = tdb.WireByName("Magic")) {
                    const nox::ThingInfo* mt = tdb.ByWire(mw);
                    if (mt) {
                        srv.SetMagicType(mw, mt->hasSpeed ? mt->speed : 0, mt->mass, nox::ParseObjectFlags(mt->flagsRaw));
                        std::printf("[날리기] Magic 종류 %u · SPEED %d · MASS %.1f\n", mw, mt->speed, mt->mass);
                    }
                }
                std::printf("[충돌 함수] 종류 %zu개 · 맵 오브젝트 %zu개 —", nFn, refs.size());
                for (const auto& kv : fnNames) std::printf(" %sx%d", kv.first.c_str(), kv.second);
                std::printf("\n");
                srv.SetMapObjects(std::move(refs));
            }
            // ★ 2026-09-13 묶음 3-a — 사람 애니 칸 수(PlayerDraw STAT). 맨손 공격이 끝나는 틱이 이 값이다.
            for (int i = 0; i < nox::kPlayerAnimTypeCount; ++i)
                srv.SetPlayerAnimFrames(i, tdb.PlayerAnim(i).frames, tdb.PlayerAnim(i).hold);
            // ★ 09-23 2-a1 — 함성(46)·광전사(45) 애니 길이도 찍는다. 함성 애니가 끝나면 켜짐 값이 내려가 광전사·작살을 더는 안 막으므로
            //   「함성 지속 120 중 얼마 동안 막나」가 이 값으로 정해진다(플랜 v2 에서 미확인이던 것)
            //   ★ 3-a — 점프(3) 애니 길이 = 떠 있는 틱 수(칸 × (붙듦+1))
            std::printf("[애니] 사람 STAT %d개 — 주먹 %d칸/붙듦 %d · 훅 %d칸/붙듦 %d · 함성(46) %d칸/붙듦 %d · 광전사(45) %d칸/붙듦 %d · 점프(3) %d칸/붙듦 %d\n",
                        tdb.PlayerAnimCount(),
                        tdb.PlayerAnim(23).frames, tdb.PlayerAnim(23).hold,
                        tdb.PlayerAnim(25).frames, tdb.PlayerAnim(25).hold,
                        tdb.PlayerAnim(46).frames, tdb.PlayerAnim(46).hold,
                        tdb.PlayerAnim(45).frames, tdb.PlayerAnim(45).hold,
                        tdb.PlayerAnim(3).frames, tdb.PlayerAnim(3).hold);
            if (!props.empty()) {
                std::printf("[다루는 것] 문·오벨리스크 %zu개 — 다가가면 열리고, 옆에 서면 마나가 찬다\n", props.size());
                // ★ 2026-09-18 ② — **자리를 찍는다.** 무인 걷기가 「문 0」으로 끝났을 때
                //   「문이 안 열리는 것」과 「문 근처에 못 간 것」을 못 가른다.
                //   이 좌표를 `--spawn-at` 에 넣으면 문 앞에서 시작할 수 있다.
                std::printf("[문 자리] 앞 6개 —");
                for (size_t k = 0; k < props.size() && k < 6; ++k)
                    std::printf(" (%d,%d)", static_cast<int>(props[k].x), static_cast<int>(props[k].y));
                std::printf("\n");
                // ★ 09-22 — **문 자리에 벽 칸이 있나.** 정답지 `Sub_57B500` 은 충돌에서 문을 무시하고(0x10 미설정)
                //   추적에서는 열렸으면 통과시킨다. 그 갈래를 우리도 만들지 정하려고 **먼저 셌다.**
                //   ⭕ Estate 실측 — 문 6개 중 **그 칸에 벽은 1개**, 둘레 한 칸까지는 6개.
                //     ⇒ 문은 대개 **벽의 빈틈에 놓인 물건**이지 벽 칸이 아니다. 그 갈래는 여기선 1건뿐이라
                //       **만들 값이 없다.** 진짜 구멍은 따로다 — `BodyVsBody`(`Server.cpp`)가 문 충돌을
                //       통째로 건너뛰고 장애물 목록도 문을 빼서 **닫힌 문을 그냥 통과한다**(묶음 9).
                //   0 옆에 **분모**(문 개수)를 같이 찍는다
                {
                    size_t onWall = 0, near1 = 0, nDoor = 0;
                    for (const auto& p : props) {
                        if (p.kind != nox::PropKind::Door) continue;
                        ++nDoor;
                        const int cx = static_cast<int>(p.x) / 23, cy = static_cast<int>(p.y) / 23;
                        if (srv.DebugWallDirAt(cx, cy) >= 0) ++onWall;
                        bool n = false;
                        for (int dy = -1; dy <= 1 && !n; ++dy)
                            for (int dx = -1; dx <= 1 && !n; ++dx)
                                if (srv.DebugWallDirAt(cx + dx, cy + dy) >= 0) n = true;
                        if (n) ++near1;
                    }
                    std::printf("[문·벽] 문 %zu개 중 **그 칸에 벽 %zu개 · 둘레 한 칸까지 %zu개**\n", nDoor, onWall, near1);
                }
                // ⑩ — 정적 조회(윗비트)를 쓰는 것이 몇이나 되나. **0 이면 뭔가 틀린 것**이다
                {
                    size_t nStat = 0;
                    for (const auto& w : props) if (w.staticLookup) ++nStat;
                    std::printf("[판·문 조회] 정적(윗비트) %zu개 / 모두 %zu개\n", nStat, props.size());
                }
                // ★ ④⑩ — **밟는 판**만 따로 찍는다. 그 위를 왕복해야 보는 항목이라
                //   `--spawn-at` 에 넣을 좌표가 필요하다(문 자리에 섞이면 못 고른다).
                {
                    size_t nPlate = 0;
                    for (const auto& w : props) {
                        if (w.kind != nox::PropKind::Plate) continue;
                        if (nPlate == 0) std::printf("[밟는 판 자리]");
                        if (nPlate < 8) std::printf(" (%d,%d 번호%u)", static_cast<int>(w.x), static_cast<int>(w.y),
                                                    static_cast<unsigned>(w.netCode));
                        ++nPlate;
                    }
                    if (nPlate) std::printf("  — 모두 %zu개\n", nPlate);
                }
                // ⑩ — 번호가 15비트를 넘으면 클라 정적 조회로 못 가리킨다. **0 이 아니면 알린다**
                if (propExtentTooBig)
                    std::printf("[판·문 번호] ⚠ extent 가 15비트를 넘는 것 %zu개 — 그것들은 클라가 못 찾는다\n",
                                propExtentTooBig);
                srv.SetWorldProps(std::move(props));
                // ★ 09-22 묶음 9 — 문을 **제 칸의 벽으로** 붙인다(정답지 `doorAttachWall`).
                //   벽과 문이 **둘 다 들어온 뒤**에 불러야 한다
                if (!getenv("NOX_NO_DOOR_WALLS")) srv.AttachDoorWalls();   // A/B 스위치(09-22)
                std::printf("[문·벽] 문 칸에 벽을 붙였다 — 닫힌 문이 걸음을 막는다(밀어내기는 안 한다)\n");
            }

            // ★ 주문 마나 비용 — 같은 파일의 SPEL 섹션에서 온다.
            //   주문 번호(와이어)로 표를 만든다: 번호 → 속이름 → 마나.
            //   ⚠ 여기가 0 이면 **모든 주문이 공짜**가 된다. 아래 로그로 바로 보인다.
            // ★ 2026-09-13 묶음 4-a — 비용만이 아니라 **정의 전체**(깃발·음소)를 넣는다. 음소 나무가 여기서 자란다.
            for (int id = 0; id < nox::kSpellCount; ++id) {
                const char* nm2 = nox::SpellName(id);
                if (!nm2 || !*nm2) continue;
                for (const auto& sr : tdb.Spells()) {
                    if (sr.name != nm2) continue;
                    // ★ 10-f — 소리 셋도 같이 넣는다(이름 → 번호)
                    srv.SetSpellDef(static_cast<uint8_t>(id), sr.mana, sr.flags, sr.phonemes,
                                    nox::SoundByName(sr.castSound.c_str()),
                                    nox::SoundByName(sr.onSound.c_str()),
                                    nox::SoundByName(sr.offSound.c_str()));
                    // ★ 2026-09-18 ⑰ — **음소가 없는 주문**을 찍는다.
                    //   음소 나무를 못 따라가면 시전이 「모름」으로 끝난다(`Server.cpp:4731`).
                    //   컨저러 주먹(29)이 「받음 7 · 주문 0」 으로 끝난 까닭 후보다.
                    if (sr.phonemes.empty()) {
                        std::printf("[주문 음소없음] %d(%s)\n", id, nm2);
                    }
                    break;
                }
            }
        }
        // ★ 2026-09-20 묶음 10 — **능력 소리**를 thing.bin `ABIL` 칸에서 넣는다.
        //   이름은 정답지 `server/spells_abil.go:196~199 AbilityNames` 차례 그대로(1~5).
        //   전에는 이 칸을 통째로 건너뛰어 능력을 써도 소리가 한 건도 안 나갔다(⑧)
        {
            static const char* const kAbilNames[6] = {
                "", "ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY",
                "ABILITY_HARPOON", "ABILITY_TREAD_LIGHTLY", "ABILITY_EYE_OF_THE_WOLF",
            };
            int wired = 0;
            for (uint8_t n = 1; n <= 5; ++n) {
                for (const auto& ar : tdb.Abilities()) {
                    if (ar.name != kAbilNames[n]) continue;
                    srv.SetAbilitySound(n, nox::SoundByName(ar.useSound.c_str()),
                                        nox::SoundByName(ar.offSound.c_str()));
                    if (nox::SoundByName(ar.useSound.c_str()) != 0) ++wired;
                    break;
                }
            }
            std::printf("[능력 소리] thing.bin ABIL %zu칸 · 다섯 중 %d개에 쓸 때 소리가 붙었다\n",
                        tdb.Abilities().size(), wired);
        }
        std::printf("[맵] %s — 벽 %zu칸 · 정적 %zu개(변환 실패 %zu) · 충돌 켜짐\n",
                    mapPath.c_str(), ws.size(), nStat, nSkip);
        std::printf("[주문] 마나 표 %zu개 — 0 이면 thing.bin 을 못 읽은 것이고 주문이 공짜가 된다\n",
                    srv.SpellCostCount());
        // ★ 2026-09-14 묶음 4-a4 — gamedata.bin(기획 상수 · 버프 길이 등). 태그 ARENA
        {
            nox::Gamedata gd;
            if (gd.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\gamedata.bin")) {
                srv.SetGamedata(gd);
                std::printf("[기획값] gamedata.bin %zu개 · ARENA TargetedSpellLifetime %.0f · InvulnerabilityEnchantDuration %.0f\n",
                            gd.Count(), gd.Float("ARENA", "TargetedSpellLifetime"), gd.Float("ARENA", "InvulnerabilityEnchantDuration"));
            } else {
                std::printf("[기획값] ⚠ gamedata.bin 못 읽음 — %s\n", gd.Error().c_str());
            }
        }
        // ★ 09-23 nine-defects 2-a3 — **반주문 재료.** ① 지워지는 날아가는 것 = 클래스 `MISSILE` · 서브클래스 `MISSILE_COUNTERSPELL`
        //   (정답지 `obj.SubClass().AsMissile().Has(MissileMissileCounterSpell)`) ② 반주문이 끄는 주문 = SPEL 깃발 `CAN_COUNTER`(0x40000).
        //   ②를 이름으로 찍는다 — 함성 외침은 같은 틱에 기절(STUN)을 걸고 반주문을 부르므로, STUN 이 여기 있으면 기절이 도로 풀린다
        {
            auto hasTok = [](const std::string& raw, const char* tok) {
                size_t st = 0;
                while (st <= raw.size()) {
                    size_t e = raw.find('+', st);
                    if (e == std::string::npos) e = raw.size();
                    std::string t = raw.substr(st, e - st);
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
                    if (t == tok) return true;
                    st = e + 1;
                }
                return false;
            };
            std::string mis;
            for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                const nox::ThingInfo* t2 = tdb.ByWire(w);
                if (!t2 || !hasTok(t2->classRaw, "MISSILE") || !hasTok(t2->subclassRaw, "MISSILE_COUNTERSPELL")) continue;
                srv.SetCounterMissileType(w);
                if (srv.CounterMissileTypeCount() <= 8) { mis += " "; mis += t2->name; }
            }
            std::string spells;
            int nSp = 0;
            for (int id = 1; id < nox::kSpellCount; ++id) {
                if (!(srv.SpellFlags(static_cast<uint8_t>(id)) & 0x40000)) continue;
                ++nSp;
                spells += " "; spells += nox::SpellName(id);
            }
            std::printf("[반주문] 둘레 %.0f · 지워지는 날아가는 것 %zu종(앞 8:%s) · 끄는 주문 %d개 —%s\n",
                        srv.BalanceFloat("CounterspellRange"), srv.CounterMissileTypeCount(), mis.c_str(), nSp, spells.c_str());
            // ★ 2-a4 — 주문 쓰는 몬스터(monster.bin `CAN_CAST_SPELLS`, `GameBalance.h` 표) → 와이어 번호. 함성 무효화가 이것만 본다
            int nCast = 0;
            std::string miss;
            for (const char* nm : nox::kMonstersCanCastSpells) {
                const uint16_t w = tdb.WireByName(nm);
                if (w) { srv.SetCastingMonsterType(w); ++nCast; }
                else { miss += " "; miss += nm; }
            }
            std::printf("[주문쓰는몬스터] %d / %zu종%s%s\n", nCast, sizeof(nox::kMonstersCanCastSpells) / sizeof(nox::kMonstersCanCastSpells[0]),
                        miss.empty() ? "" : " · ⚠ thing.bin 에 없음:", miss.c_str());
        }
        // ★★ 2026-09-15 — **모드 깃발을 서버에 실제로 걸어 준다.**
        //   여기가 없어서 묶음 8 규칙(탈락전·왕죽이기·깃발 판 끝)이 시험에서만 켜졌다.
        //   `gameSettings_.gameFlags` 하나가 **두 군데**로 간다 —
        //     ㉮ 판정: `DeathScoring`(0x100·0x10·0x400) · `RoundTimeUp`(0x60·0x400) ·
        //        `CheckVictorySurvivors`(0x400) · `RoundCheckLimit`(0x400·0x200)
        //     ㉯ 와이어: `BuildJoinBundle` 이 `& 0x7FFF0` 으로 가려 클라에 알린다(`JoinBundle.cpp:41`)
        {
            const uint32_t gf = nox::ModeGameFlags(mode);
            srv.SetGameFlags(gf);
            std::printf("[모드] %s — gameFlags 0x%05X (클라에 가는 값 0x%05X)\n",
                        mode.c_str(), gf, gf & 0x7FFF0u);
        }
        // ★ 아이템이 아니라 걸러진 것 — **맵에 무엇이 있는지 한 번은 봐야 한다.**
        //   문·오벨리스크·레버가 여기 있으면 그것이 다음에 살릴 것이다.
        // ★ 아이템인데 클라가 못 집는 것 — 있으면 이름을 찍는다(조용히 버리지 않는다)
        if (!noWeight.empty()) {
            std::printf("[집기 불가] CLASS 는 아이템인데 WEIGHT 가 없어 뺀 것 —");
            for (const auto& kv : noWeight) std::printf(" %sx%d", kv.first.c_str(), kv.second);
            std::printf("\n");
        }
        if (!notPickup.empty()) {
            size_t total = 0;
            for (const auto& kv : notPickup) total += static_cast<size_t>(kv.second);
            std::printf("[맵 밖] 아이템 아닌 것 %zu개 / %zu종 —", total, notPickup.size());
            for (const auto& kv : notPickup) std::printf(" %sx%d", kv.first.c_str(), kv.second);
            std::printf("\n");
        }
        // ★ 2026-09-18 ④⑯ — **맵에 그려진 판 물건의 자리**를 찍는다.
        //   우리 서버는 맵의 깃발을 **안 읽고** 제 자리에 놓으므로, 그림과 실물이
        //   얼마나 어긋나는지 보려면 **맵 쪽 좌표**가 있어야 한다.
        //   (확인표 ⑯ 은 「맵 깃발 4개」라고 적고 있었는데 실측은 2개였다 — 09-18 정정)
        if (!mapMatchObjs.empty()) {
            std::printf("[맵 판물건] %zu개 —", mapMatchObjs.size());
            for (const auto& t : mapMatchObjs) std::printf(" %s", t.c_str());
            std::printf("\n");
        }
        if (standalone) {
            std::string nm = mapPath;
            const size_t sl = nm.find_last_of("\\/");
            if (sl != std::string::npos) nm = nm.substr(sl + 1);
            for (char& ch : nm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            srv.SetStandalone(nm, mf.Crc());
            // 소개(SERVER_INFO)의 맵 이름도 이 맵으로 — 칸이 9바이트라 확장자를 뗀다.
            std::string bare = nm;
            const size_t dot2 = bare.find('.');
            if (dot2 != std::string::npos) bare.resize(dot2);
            if (!bare.empty() && bare.size() < 9) tmpl.mapName = bare;
            std::printf("[독립 모드] 접속 묶음·USE_MAP·스냅샷을 조립으로 보냄 (%s crc=%08x)\n",
                        nm.c_str(), mf.Crc());
        }
        // ★★★★ **여기가 깨져 있었다** (2026-09-11 발견·수리).
        //   아래 세 뭉치가 **`if (aoi)` 블록 안에** 들어가 있었다 — 즉 `--aoi` 를 안 주면
        //   `--spell-start` 도 `--anim-pose` 도 `--anim-dir` 도 **한 번도 적용되지 않았다.**
        //   ⚠ 문법이 멀쩡해 **빌드도 시험 5/5 도 통과했다.** 그래서 오래 안 보였다.
        //   ★ 잡아낸 것은 「**켠 스위치를 찍는다**」 한 줄이다 — `[진단스위치]` 가 안 나와서
        //     비로소 「줬는데 안 먹었다」를 알았다. 계측을 만들고 **그 답이 읽히는지**까지
        //     본 것이 값을 했다.
        if (aoi) {
            srv.SetAoi(true);
            std::printf("[시야] 정적 오브젝트를 반경 %d 안만 갱신에 실음\n",
                        static_cast<int>(nox::kAoiRadius));
        }
        // ★ 켠 스위치를 찍는다 — 위 「여기가 깨져 있었다」 주석의 교훈.
        if (creatureAoiHalf != 0.0) {
            const double hx = creatureAoiHalf < 0.0 ? 740.0 : creatureAoiHalf;
            const double hy = creatureAoiHalf < 0.0 ? 580.0 : creatureAoiHalf;
            srv.SetCreatureAoi(hx, hy);
            std::printf("[크리처시야] 켬 — 들어옴 ±%d×±%d · 나감 ±%d×±%d (시야선 없음, 거리만)\n",
                        static_cast<int>(hx), static_cast<int>(hy),
                        static_cast<int>(hx * 1.2), static_cast<int>(hy * 1.2));
            std::fflush(stdout);
        }
        if (friends) {
            srv.SetFriends(true);
            std::printf("[친구] 켬 — 자기 소환수·같은 팀을 OBJECT_FRIEND_ADD 로 알림 (적대 판정 부분 구현)\n");
            std::fflush(stdout);
        }
        if (autoRespawn) {
            srv.SetAutoRespawn(true);
            std::printf("[설정] 자동 부활 켬 — 죽고 45틱 뒤 클릭 없이 살아난다\n");
        }
        if (debugHurtEvery > 0) {
            srv.SetDebugHurtEvery(static_cast<uint32_t>(debugHurtEvery), debugHurtFront);
            std::printf("[진단스위치] %d 틱마다 첫 사람에게 피해 25(종류 0 · 주인 없음)%s\n", debugHurtEvery,
                        debugHurtFront ? " · 앞 30 에서" : "");
        }
        if (debugEnchantB >= 0) {
            srv.SetDebugEnchant(debugEnchantB, debugEnchantP, static_cast<uint32_t>(debugEnchantN));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 버프 %d(세기 %d) 끝없이\n", debugEnchantN, debugEnchantB, debugEnchantP);
        }
        if (debugDummy > 0) {
            srv.SetDebugDummyAfter(static_cast<uint32_t>(debugDummy));
            srv.SetDebugDummyDx(debugDummyDx);
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주인 없는 임프 허수아비를 오른쪽 +%d 에 고정\n", debugDummy, debugDummyDx);
        }
        if (debugOwnPet > 0) {
            srv.SetDebugOwnPetAfter(static_cast<uint32_t>(debugOwnPet));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 그 사람 소유 임프를 사람 자리에 고정\n", debugOwnPet);
            std::fflush(stdout);
        }
        if (debugPetOrder > 0) {
            srv.SetDebugPetOrder(static_cast<uint8_t>(debugPetOrder));
            std::printf("[진단스위치] 소환수에게 명령 %d 를 준다(2가만히·3지켜·4따라와·5사냥)\n", debugPetOrder);
            std::fflush(stdout);
        }
        if (debugKillAfter > 0) {
            srv.SetDebugKill(static_cast<uint32_t>(debugKillAfter));
            std::printf("[진단스위치] %d 틱 뒤 첫 사람이 둘째를 죽인다\n", debugKillAfter);
        }
        if (debugFistAfter > 0) {
            srv.SetDebugFist(static_cast<uint32_t>(debugFistAfter), debugFistDx, debugFistDy);
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주먹을 그 사람 +(%d,%d) 에 떨어뜨린다\n",
                        debugFistAfter, debugFistDx, debugFistDy);
            std::fflush(stdout);
        }
        if (debugMobAfter > 0 && debugMobType > 0) {
            srv.SetDebugMob(static_cast<uint32_t>(debugMobAfter), static_cast<uint16_t>(debugMobType),
                            debugMobX, debugMobY);
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 몬스터 %d 를 %d,%d 에 세운다\n",
                        debugMobAfter, debugMobType, debugMobX, debugMobY);
            std::fflush(stdout);
        }
        if (debugWallAfter > 0) {
            srv.SetDebugWall(static_cast<uint32_t>(debugWallAfter), debugWallGx, debugWallGy);
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 비밀벽 칸 %d,%d 을 연다\n",
                        debugWallAfter, debugWallGx, debugWallGy);
            std::fflush(stdout);
        }
        if (debugWarpAfter > 0) {
            srv.SetDebugWarp(static_cast<uint32_t>(debugWarpAfter), debugWarpX, debugWarpY);
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 %d,%d 로 옮긴다\n",
                        debugWarpAfter, debugWarpX, debugWarpY);
            std::fflush(stdout);
        }
        if (debugScoreAfter > 0) {
            srv.SetDebugScore(static_cast<uint32_t>(debugScoreAfter), static_cast<uint32_t>(debugScoreVal));
            std::printf("[진단스위치] %d 틱 뒤 첫 사람 점수를 %d 로\n", debugScoreAfter, debugScoreVal);
        }
        if (debugTeamEvery > 0) {
            srv.SetDebugTeamEvery(static_cast<uint32_t>(debugTeamEvery));
            std::printf("[진단스위치] %d 틱마다 첫 사람의 팀을 1<->2 로 오감\n", debugTeamEvery);
        }
        if (debugTeamAfter > 0) {
            srv.SetDebugTeamAfter(static_cast<uint32_t>(debugTeamAfter));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 그 사람 팀을 한 번 바꿈\n", debugTeamAfter);
            std::fflush(stdout);
        }
        if (spawnAtX >= 0 && spawnAtY >= 0) {
            // 첫 접속 자리와 **부활 자리를 함께** 옮긴다 — 죽어도 그 자리로 돌아와야 판정을 이어간다.
            srv.SetSpawn(spawnAtX, spawnAtY);
            srv.SetPlayerStarts({ { static_cast<float>(spawnAtX), static_cast<float>(spawnAtY) } });
            // ★ ①.5 — **걸을 수 있는 자리인지 같이 찍는다.** 09-16 에 발판 무리 한가운데를
            //   `CanStand` 확인 없이 골랐다가 「못 움직인다」를 불렀다. 값이 안 보이면 또 그런다.
            const bool standOk = srv.CanStand(spawnAtX, spawnAtY, nox::kPlayerRadius);
            std::printf("[시작 자리] 손으로 지정 — (%d, %d) · 부활도 같은 자리 · 설 수 있나 %s\n",
                        spawnAtX, spawnAtY, standOk ? "예" : "**아니오**");
            std::fflush(stdout);
        }
        for (size_t bi = 0; bi < bots.size(); ++bi) {
            // ★ 자리·번호·편을 정하는 **셈은 `ModeRules.h` 가 한다**(시험이 닿는 자리).
            //   여기 남는 것은 그 값을 서버에 **꽂는 배선**뿐이다.
            const nox::BotPlan plan =
                nox::PlanBot(bi, bots[bi].x, bots[bi].y, srv.PlayerStartCount());
            if (!plan.place) {
                if (bots[bi].x == 0 && bots[bi].y == 0) {
                    std::printf("[진단스위치] 봇 자리 자동 — 이 맵엔 PlayerStart 가 없다. 봇 안 세움\n");
                    std::fflush(stdout);
                }
                continue;
            }
            int bx = plan.x, by = plan.y;
            if (plan.useStart) {
                float sx = 0.0f, sy = 0.0f;
                if (!srv.PlayerStartAt(plan.startIndex, sx, sy)) continue;
                bx = static_cast<int>(sx); by = static_cast<int>(sy);
            }
            nox::CharClass bc = nox::CharClass::Warrior;
            if (bots[bi].cls == "wizard")        bc = nox::CharClass::Wizard;
            else if (bots[bi].cls == "conjurer") bc = nox::CharClass::Conjurer;
            const std::string bot = nox::BotAddr(bi);
            srv.DebugEnterGame(bot, plan.netCode, /*observer=*/false, bc);
            // ⚠ 자리는 **둘 다** 맞춰야 한다 — `DebugMoveTo` 는 `x`·`y` 만, `DebugSetPos` 는 `posX`·`posY` 만 옮긴다.
            //   덫 겨눔·소리 거리는 `posX`·`posY` 로 재고, 걸음·갱신은 `x`·`y` 를 본다(09-15 에 한 번 데었다)
            srv.DebugMoveTo(bot, bx, by);
            srv.DebugSetPos(bot, bx, by);
            srv.DebugSetTeam(bot, plan.team);
            std::printf("[진단스위치] 가짜 사람 세움 — (%d, %d) · 직업 %s · 번호 %u · 편 %d\n",
                        bx, by, bots[bi].cls.c_str(), (unsigned)plan.netCode, (int)plan.team);
            std::fflush(stdout);
        }
        if (debugXpAfter > 0) {
            srv.SetDebugXpAfter(static_cast<uint32_t>(debugXpAfter), static_cast<float>(debugXpAmount));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 경험치 %.0f 한 번\n", debugXpAfter, debugXpAmount);
            std::fflush(stdout);
        }
        if (debugSpellAfter > 0 && debugSpellId > 0) {
            srv.SetDebugGiveSpellAfter(static_cast<uint32_t>(debugSpellAfter),
                                       static_cast<uint8_t>(debugSpellId));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주문 %d(%s) 한 번\n", debugSpellAfter,
                        debugSpellId, nox::SpellName(static_cast<uint8_t>(debugSpellId)));
            std::fflush(stdout);
        }
        if (creatureAwayAfter > 0) {
            srv.SetDebugCreatureAwayAfter(static_cast<uint32_t>(creatureAwayAfter));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 크리처를 좌우 ±300 으로 옮김\n", creatureAwayAfter);
            std::fflush(stdout);
        }
        if (spellStart) srv.SetNoSpellStart(false);
        // ★ 켠 스위치를 찍는다. 안 찍으면 「줬는데 안 먹었다」를 못 가른다.
        if (forcePose >= 0 || forceDir >= 0 || startHealth > 0) {
            std::printf("[진단스위치] 자세=%d · 방향=%d · 시작체력=%d\n",
                        forcePose, forceDir, startHealth);
            std::fflush(stdout);
        }
        if (forcePose >= 0) srv.SetForceAnimPose(forcePose);
        if (forceDir  >= 0) srv.SetForceAnimDir(forceDir);
        if (forceArrowFrame >= 0) {
            srv.SetForceArrowFrame(forceArrowFrame);
            std::printf("[진단] 화살 프레임을 %d 로 못 박는다\n", forceArrowFrame);
        }
        if (forceHarpoon) srv.SetForceHarpoon(true);
        if (dumpHarpoon) srv.SetDumpHarpoon(true);
        if (secondNewPlayer) {
            srv.SetNoSecondNewPlayer(false);
            std::printf("[진단] 옛 동작 — 관전 풀 때 2d NEW_PLAYER 를 다시 보낸다 (--2nd-newplayer)\n");
        }
        if (noSecondNewPlayer) {
            srv.SetNoSecondNewPlayer(true);
            std::printf("[진단] 관전 풀 때 2d NEW_PLAYER 를 다시 안 보낸다 (--no-2nd-newplayer)\n");
        }
        if (arrowFreeze) {
            srv.SetArrowFreeze(true);
            std::printf("[진단] 화살을 난 자리에 세워 둔다 (--arrow-freeze)\n");
        }
        if (arrowFrameAlt > 0) {
            srv.SetArrowFrameAlt(arrowFrameAlt);
            std::printf("[진단] 화살 프레임을 %d 틱마다 0↔16 으로 번갈아 보낸다\n", arrowFrameAlt);
        }
        if (startHealth > 0) srv.SetStartHealth(startHealth);
        if (startMana > 0) srv.SetStartMana(startMana);
        if (itemAtSpawn) {
            // 스폰 자리를 아이템 자리로. 걷기를 빼고 **조작만** 재려는 것이다.
            // ⚠ **아이템을 옮기는 것이 아니다** — 클라가 맵대로 만든 실체와 어긋난다.
            if (srv.DebugSpawnAtStatic())
                std::printf("[시험] 스폰 자리를 아이템 자리(%d,%d)로 옮겼다 — 걷지 않고 줍기 조작을 잰다\n",
                            srv.spawnX(), srv.spawnY());
            else
                std::printf("[시험] ⚠ 아이템이 없다 — 스윕이 무효다\n");
        }
    }
    {
        uint8_t info[256] = {};
        nox::Writer w(info, sizeof(info));
        if (!tmpl.Encode(w) || !w.ok()) {
            std::printf("소개 원본을 만들지 못했다\n");
            return 1;
        }
        srv.SetInfoTemplate(info, w.written());
    }

    // ★★ 접속 뒤에 내려줄 것 — 접속 묶음(216바이트)과 맵 지정(41바이트).
    //    이게 비어 있으면 Server.cpp 의 CLIENT_ACCEPT 가 아무것도 안 보내고,
    //    클라는 `Connected!` 화면에서 멈춘 채 자기소개만 재전송한다(2026-09-07 실측).
    //    대조 장치(replay.cpp)만 갖고 있던 배선이라 본체에서 빠져 있었다.
    {
        if (dumpPath.empty()) dumpPath = DefaultDumpPath();
        nox::dump::Setup setup;
        // ★ 독립 모드면 녹음 서두를 **아예 안 채운다** — 조립 분기(Server.cpp)가 전부
        //   「비어 있을 때만」 게이트라, 기본 녹음이 채우면 조립이 한 번도 안 돈다.
        //   실제로 sa1·sa2(2026-09-10)의 「독립 스냅샷」은 바이트가 녹음과 동일했다 —
        //   졸업 판정이 이 배선 하나로 뒤집혔다.
        if (!standalone &&
            !dumpPath.empty() && nox::dump::Sniff(dumpPath.c_str(), setup) &&
            !setup.joinBody.empty() && !setup.useMapBody.empty()) {
            srv.SetJoinTemplate(setup.joinBody.data(), setup.joinBody.size());
            srv.SetUseMapTemplate(setup.useMapBody.data(), setup.useMapBody.size());
            // ★★ **소개의 맵 이름을 서두의 맵과 맞춘다.** 소개 원본은 녹음(so_druid)인데
            //   서두를 다른 덤프(estate)로 바꾸면 **둘이 어긋난다** — 클라 로그에
            //   `M:"so_druid"` 로 뜨는데 실제로 읽는 맵은 estate 다(2026-09-08).
            //   `useMapBody` 는 오프코드부터라 이름은 1번부터 시작한다.
            if (setup.useMapBody.size() > 10) {
                const char* mn = reinterpret_cast<const char*>(setup.useMapBody.data()) + 1;
                size_t mlen = strnlen(mn, setup.useMapBody.size() - 1);
                // ⚠ `USE_MAP` 은 `estate.map` 처럼 **확장자까지** 담는데(10글자),
                //   소개(`SERVER_INFO`)의 칸은 9바이트다. `.map` 을 떼야 들어간다.
                //   이걸 안 떼서 첫 시도가 조용히 안 먹었다(2026-09-08).
                const char* dot = static_cast<const char*>(memchr(mn, '.', mlen));
                if (dot) mlen = static_cast<size_t>(dot - mn);
                if (mlen > 0 && mlen < 9) {
                    tmpl.mapName.assign(mn, mlen);
                    uint8_t info2[256] = {};
                    nox::Writer w2(info2, sizeof(info2));
                    if (tmpl.Encode(w2) && w2.ok()) {
                        srv.SetInfoTemplate(info2, w2.written());
                        std::printf("소개 맵 이름을 서두에 맞춤: %s\n", tmpl.mapName.c_str());
                    }
                }
            }
            srv.SetPostMap(setup.postMap);
            // ★★★ 첫 세계 스냅샷 — 뽑아만 두고 **넘기는 줄이 없었다**(2026-09-08).
            //   이 한 조각에 살려내기·장비·최대 체력/마나가 다 실려 있어,
            //   빠지면 이름표만 뜨고 몸이 없다.
            //   ★ `--no-snapshot` 으로 끌 수 있다 — 09-08 05:42 에 아레나가 한 판 돌 때는
            //     이 배선이 아직 없었고 그때는 HUD(주문 5개·체력/마나 막대)가 떠 있었다.
            //     「HUD 가 사라진 게 이 스냅샷 탓인가」를 가르는 자리다.
            if (!noSnapshot) srv.SetFirstSnapshot(setup.firstSnapshot);
            // ★★ 서두(prelude) — 정품이 접속 뒤 보낸 **앞 9개 조각**을 그대로 흘려보내고,
            //   그 뒤부터 내 생성으로 이어받는다. 이걸 빼면 클라 화면이 검은 채로 남는다.
            //   2026-09-07 이분법으로 9가 경계임을 확인했다(8개면 검정, 9개면 세계가 뜬다).
            //   ⚠ 임시 발판이다 — 9번(첫 세계 스냅샷)을 내가 만들게 되면 없앤다.
            if (!mirror && prelude) {
                std::vector<std::vector<uint8_t>> pre = setup.mirror;
                if (pre.size() > 9) pre.resize(9);
                srv.SetMirror(pre);
                std::printf("서두 %zu개를 흘려보낸 뒤 내 생성으로 이어받는다\n", pre.size());
            }
            if (mirror) {
                std::vector<std::vector<uint8_t>> m = setup.mirror;
                if (mirrorLimit > 0 && static_cast<size_t>(mirrorLimit) < m.size())
                    m.resize(static_cast<size_t>(mirrorLimit));
                MirrorStat(m);
                srv.SetMirror(m);
            }
            std::printf("덤프에서 뽑음 — 접속 묶음 %zu바이트 · 맵 지정 %zu바이트 · 뒤따르는 것 %zu개\n",
                        setup.joinBody.size(), setup.useMapBody.size(), setup.postMap.size());
        } else if (standalone) {
            std::printf("[독립 모드] 녹음 서두를 안 읽는다 — 접속 묶음·맵 지정·스냅샷 전부 조립\n");
        } else {
            std::printf("[경고] 접속 묶음·맵 지정을 못 얻었다. 클라는 접속만 되고 게임에 못 들어간다.\n");
            std::printf("  찾아본 덤프: %s\n", dumpPath.empty() ? "(없음)" : dumpPath.c_str());
            std::printf("  --dump <서버쪽 덤프> 로 지정할 것\n");
        }
        // ── 서두와 무관한 일반 설정 — 독립 모드에서도 적용돼야 하므로 밖에 둔다.
        srv.SetPatchSnapshot(!rawSnapshot);
        srv.SetSendUpdates(!noUpdate);
        srv.SetNotifyNewGear(!noNewGear);
        srv.SetAckRecvSeq(ackSeq);      // B-1b — 신뢰 순번 되받기(기본 끔 · `--ack-seq` 로 켬)
        srv.SetAckAnyStream(ackAnyStream);
        srv.SetNoSpawnInvuln(noSpawnInvuln);   // 화면 확인 우회 — 부활 무적 안 걸기
        srv.SetForceHudOn(!hudOff);
        if (spawnSpread > 0) srv.SetSpawnSpread(spawnSpread);
        if (peerPose >= 0) srv.SetPeerPose((uint8_t)peerPose);
        srv.SetPeerFirst(peerFirst);
        srv.SetPeerSnapshot(peerSnapshot);
        if (peerType > 0) srv.SetPeerType((uint16_t)peerType);
        srv.SetIntroEveryTick(introEveryTick);
        if (!peerOps.empty()) {
            std::set<uint8_t> ops;
            size_t at = 0;
            while (at < peerOps.size()) {
                size_t comma = peerOps.find(',', at);
                if (comma == std::string::npos) comma = peerOps.size();
                const std::string tok = peerOps.substr(at, comma - at);
                if (!tok.empty())
                    ops.insert(static_cast<uint8_t>(std::strtol(tok.c_str(), nullptr, 16)));
                at = comma + 1;
            }
            srv.SetPeerOps(ops);
            std::printf("남에게 넘길 오프코드 %zu개\n", ops.size());
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 실패\n");
        return 1;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        std::printf("소켓 실패\n");
        return 1;
    }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::printf("bind 실패 (%d). 포트 %d 를 이미 누가 쓰고 있는가?\n",
                    WSAGetLastError(), port);
        return 1;
    }

    std::printf("접속 대기 — UDP %d, 이름 \"%s\"\n", port, name.c_str());
    std::printf("답하는 것: 서버 찾기 · 접속 시도 · 접속 수락 · 핑\n");
    std::fflush(stdout);

    // 주소를 문자열로 만든다. `Server` 는 이 문자열로 상대를 가른다.
    auto keyOf = [](const sockaddr_in& a) {
        char ip[64] = {};
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(a.sin_port));
        return std::string(buf);
    };

    uint8_t in[2048];
    std::map<std::string, sockaddr_in> addrOf;   // 문자열 → 진짜 주소
    std::vector<nox::OutPacket> outs;
    unsigned long long nRecv = 0, nSent = 0, nTicks = 0;

    // 나갈 조각을 실제로 내보낸다. 틱과 수신 양쪽에서 쓴다.
    auto flush = [&]() {
        for (const auto& p : outs) {
            auto it = addrOf.find(p.to);
            if (it == addrOf.end()) continue;
            sendto(s, reinterpret_cast<const char*>(p.data.data()),
                   static_cast<int>(p.data.size()), 0,
                   reinterpret_cast<sockaddr*>(&it->second), sizeof(it->second));
            ++nSent;
        }
        outs.clear();
    };

    // ★ `recvfrom` 을 그냥 부르면 패킷이 올 때까지 막혀서 **틱이 안 돈다.**
    //   기다리는 시간을 틱 주기로 잘라 두고, 그 사이사이에 틱을 돌린다.
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{};
        tv.tv_usec = 5000;   // 5ms — 틱 주기보다 촘촘히 깨어난다
        const int ready = select(0, &rd, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(s, &rd)) {
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            const int n = recvfrom(s, reinterpret_cast<char*>(in), sizeof(in), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0) {
                ++nRecv;
                const std::string who = keyOf(from);
                addrOf[who] = from;
                const size_t got = srv.OnPacket(who, in, static_cast<size_t>(n), outs);
                const size_t made = outs.size();
                flush();

                // ★★ **클라가 처음 보낸 오프코드를 그때그때 알린다.**
                //
                //   왜 — 「어떤 조작이 무슨 메시지를 보내나」를 알 길이 없었다.
                //   `0x73 TRY_GET`(줍기)이 왼버튼이 아니라는 것까지는 알아냈지만
                //   **그럼 무엇이냐**는 짐작뿐이었고, 그래서 「사람이 눌러 봐야 한다」로 넘겼다.
                //   이 한 줄이면 키를 하나씩 눌러 보며 **조작 ↔ 메시지 대응표**를 자동으로 만든다.
                //
                //   ⚠ `--quiet` 여도 찍는다. 드물게 나오고, 나오는 순간이 곧 답이다.
                {
                    static std::map<uint8_t, bool> told;
                    for (const auto& kv : srv.seen()) {
                        if (told.count(kv.first)) continue;
                        told[kv.first] = true;
                        std::printf("[처음] 클라가 보낸 새 오프코드 0x%02x (%s)\n",
                                    kv.first, nox::OpName(static_cast<nox::Op>(kv.first)));
                        std::fflush(stdout);
                    }
                }

                if (!quiet) {
                    // ★ 한 메시지도 못 읽었으면 그 바이트를 찍는다.
        //   「0개」를 그냥 지나치면 파서가 멈춘 자리를 영영 못 본다.
        if (got == 0 && n > 3) DumpHex("  [못 읽음]", in, static_cast<size_t>(n));
        std::printf("[받음 %llu] %s  %d바이트 → 메시지 %zu개, 보낸 조각 %zu개\n",
                                nRecv, who.c_str(), n, got, made);
                    // ★ **뜯었는데 답이 없으면 무엇을 봤는지 찍는다.**
                    //   2026-09-10: 접속 요청 158바이트가 「메시지 2개, 보낸 조각 0개」로
                    //   나오는데 그 둘이 무엇인지 알 길이 없어 반나절을 짐작으로 보냈다.
                    //   조각을 버린 것도 아니고(`[버림]` 0건) 크래시도 아닌데 답만 없었다.
                    //   ⚠ 열쇠 상태를 같이 찍는다 — **복호가 어긋나면 쓰레기가
                    //     그럴듯한 오프코드 두 개로 보인다.** 「메시지 2개」의 함정이 그것이다.
                    if (got > 0 && made == 0 && n > 3) {
                        std::printf("     ↳ 본 것:");
                        for (uint8_t o : srv.LastFragOps())
                            std::printf(" %02x(%s)", o, nox::OpName(static_cast<nox::Op>(o)));
                        std::printf("  · 열쇠 %s 0x%02x\n",
                                    srv.IsKeyed(who) ? "있음" : "없음", srv.KeyOf(who));
                    }
                    std::fflush(stdout);
                }
            }
        }

        // ★★ **판을 차리는 자리** — 사람이 다 들어온 뒤라야 한다.
        //   `StartCtf` 는 **그때 게임 안에 있는 사람에게만** 팀을 준다. 접속 전에 부르면
        //   팀이 0 으로 남아 깃발 판정이 통째로 건너뛴다(`UpdateFlags` 의 `s.team == 0` 걸림).
        //
        // ★★★ **정정(2026-09-11) — 퀘스트는 혼자서도 열려야 한다.**
        //   전에는 모드를 가리지 않고 **둘 이상**을 기다렸다. 그래서 졸업시험(클라 하나)에서
        //   `StartQuest` 가 **한 번도 안 불렸고**, 몬스터가 0 마리라 「맞고 죽기」를 90초
        //   내내 못 쟀다. ⚠ 그런데 틱에는 **「스테이지 1」이 찍혀** 마치 판이 도는 것처럼
        //   보였다 — 그 값은 `questStage_ + 1` 일 뿐이라 **표시가 사람을 속였다.**
        //   지문은 **`이 웨이브 남음 0(잡음 0)`** 과 `[스테이지]` 줄 0건이었다.
        //   ⭕ 정품 NoxQuest 는 **1~4인 협동**이라 혼자 시작이 정상이다.
        //   깃발(CTF)은 두 편이 있어야 하므로 둘 이상을 그대로 기다린다.
        const int needPlayers = nox::ModeNeedPlayers(mode);
        if (!modeStarted && mode != "arena" && srv.inGameCount() >= needPlayers) {
            modeStarted = true;
            // ★ 09-19 — 전에는 `spawnX() + 20`(두 사람 사이)였다. 사람이 그 고정값에 서던 때의 셈이라,
            //   첫자리가 맵 PlayerStart 로 바뀐 뒤로는 **사람들의 실제 가운데**를 쓴다
            int mx = 0, my = 0;
            srv.PlayersCenter(mx, my);
            std::printf("[모드] 판 자리 = 사람들 가운데 %d,%d\n", mx, my);
            // ★ 갈래는 `ModeRules.h` 가 **값으로** 정한다. 여기 `if/else` 로 두었을 때
            //   `else` 하나가 여섯 모드를 삼켰고 시험이 그것을 못 봤다(조각 E·G).
            const nox::ModeSetup setup = nox::ModeStartKind(mode);
            if (setup == nox::ModeSetup::Ctf) {
                // ★ **정상 배치** — 두 진영을 갈라 놓는다. 사람이 **걸어가 뺏어 와야** 득점이다.
                //   ⚠ 전에는 둘 다 닿는 자리(사람 사이)에 뒀다. 걸어가는 조종이 없어서였는데,
                //     그러면 줍자마자 득점이 되어 **판이 아니라 배선 확인**밖에 안 된다.
                //     2026-09-10 에 걷기 조종을 만들었으니 제자리로 돌린다(`--ctf-span` 으로 조절).
                // ★★ **깃발은 「걸어서 닿는 자리」에만 놓는다** (2026-09-10).
                //   닿는 거리를 정본(20 = 두 반지름의 합)으로 줄이자 판이 안 돌았다 —
                //   사람이 29~32 까지 가고 더 못 갔다(gd2·gd3).
                //   벽 지도를 찍어 보니(`nox_datadump --walls`) estate 스폰 동쪽에
                //   **대각 벽**이 지나가고 깃발이 **그 너머**였다.
                //   전에는 닿는 거리가 40 이라 **벽 너머로 손이 닿아** 판이 돌았고,
                //   그래서 이 배치 결함이 가려져 있었다.
                // ★ 그래서 「그 자리에 설 수 있나」가 아니라 **「가운데에서 거기까지
                //   걸어갈 수 있나」**를 본다. 막히면 막히기 직전으로 당긴다.
                auto placeFlag = [&](int wantX, int wantY, int fromX) {
                    const int dir = (wantX > fromX) ? 1 : -1;
                    int good = fromX;
                    for (int x = fromX; dir > 0 ? x <= wantX : x >= wantX; x += dir * 4) {
                        if (!srv.CanStand(x, wantY, nox::kPlayerRadius)) break;
                        good = x;
                    }
                    // 벽에 바짝 붙지 않게 한 몸 물린다
                    if (good != wantX) good -= dir * static_cast<int>(nox::kPlayerRadius * 2);
                    return good;
                };
                // ★★ 2026-09-18 ④⑯ — **맵에 깃발이 그려져 있으면 그 자리를 쓴다.**
                //   전에는 늘 스폰 옆에 손으로 놓아, 클라 화면의 깃발 그림과
                //   서버가 아는 자리가 **전혀 달랐다**(CapFlag 실측 —
                //   맵 1058,931 · 3841,3714 ↔ 우리 3828,2534 · 4104,2534).
                //   맵에 없으면(Estate 등) 종전대로 손으로 놓는다 — 아무 맵에서나
                //   판정하려는 하네스이므로 그 길은 남긴다(`ModeRules.h` 주석).
                if (mapFlagPos.size() >= 2) {
                    srv.StartCtf(mapFlagPos[0].first, mapFlagPos[0].second,
                                 mapFlagPos[1].first, mapFlagPos[1].second);
                    std::printf("[모드] 깃발 자리 = **맵에 그려진 것**을 씀 (%d,%d)·(%d,%d)\n",
                                mapFlagPos[0].first, mapFlagPos[0].second,
                                mapFlagPos[1].first, mapFlagPos[1].second);
                } else {
                const int redX  = placeFlag(mx - ctfSpan, my, mx);
                const int blueX = placeFlag(mx + ctfSpan, my, mx);
                srv.StartCtf(redX, my, blueX, my);
                std::printf("[모드] 깃발 — (%d,%d)·(%d,%d), 득점 한도 %u%s\n",
                            redX, my, blueX, my, (unsigned)nox::kCaptureLimit,
                            (redX != mx - ctfSpan || blueX != mx + ctfSpan)
                                ? "  ← 벽을 피해 당김" : "");
                }
            } else if (setup == nox::ModeSetup::FlagBall) {
                // 공은 가운데, 골대는 깃발과 같은 간격으로 양옆
                srv.StartFlagBall(mx, my, mx - ctfSpan, my, mx + ctfSpan, my);
                std::printf("[모드] 플래그볼 — 공 (%d,%d) · 골대 (%d,%d)·(%d,%d)\n",
                            mx, my, mx - ctfSpan, my, mx + ctfSpan, my);
            } else if (setup == nox::ModeSetup::Quest) {
                srv.StartQuest(questCount, mx, my, questSpread);
                // ⚠ **`questCount` 는 이제 안 쓰는 값이다** — 웨이브 마릿수는 스테이지 표가
                //   정한다(`StartQuest` 주석 참조). 전에는 여기서 그 값을 「몬스터 N마리」로
                //   찍어 **실제와 다른 수를 보여 줬다**(설정 4 · 실제 2).
                //   ★ 실제로 몇 마리가 섰는지는 바로 위 `[스테이지]` 줄이 말한다.
                std::printf("[모드] 퀘스트 — (%d,%d) 반경 %.0f · 마릿수는 위 [스테이지] 줄 참조\n",
                            mx, my, questSpread);
            } else {
                // ★★ 여기가 비어 있어야 하는 모드들이다 — `elimination`·`kotr`·`chat`.
                //   전에는 이 갈래가 없어서 **`else` 가 전부 `StartQuest` 로 떨어졌고**,
                //   `--mode elimination` 을 켜면 퀘스트 몬스터 두 마리가 나와 사람을 때렸다
                //   (조각 E 실측 — `[스테이지] 1/5` · 크리처 2마리 · 피해 4).
                //   판 규칙은 게임플래그로 이미 켜져 있고, 여기서 더 놓을 것이 없다.
                std::printf("[모드] %s — 판에 따로 놓는 것 없음(규칙은 gameFlags 로 이미 켜짐)\n",
                            mode.c_str());
            }
            std::fflush(stdout);
        }

        const int ticks = srv.Tick(GetTickCount(), outs);
        if (ticks > 0) {
            nTicks += static_cast<unsigned long long>(ticks);
            const size_t made = outs.size();
            flush();
            // ★ 09-20 6-j(3) — 크리처 추적. **조용한 주행에서도 찍는다**(판정에 쓰는 줄이라)
            if (tracePets > 0 && (srv.frame() % static_cast<uint32_t>(tracePets)) == 0) {
                for (size_t pi = 0; pi < srv.petCount(); ++pi) {
                    double px = 0, py = 0;
                    if (!srv.DebugPetXY(pi, px, py)) continue;
                    const int top = srv.DebugAIStackInd(pi);
                    // ⚠ 길목 번호는 **떠돌기(10) 항목**에 있다 — 맨 위는 그 위에 얹힌 가기(7)다.
                    //   맨 위만 읽으면 늘 0 이 나와 「안 고른다」로 오독한다
                    uint32_t wp = 0;
                    for (int j = top; j >= 0; --j)
                        if (srv.DebugAIAction(pi, j) == 10) { wp = srv.DebugAIArgWp(pi, j); break; }
                    // ★ 09-21 ③ — **벽 칸 안에 있나**를 같이 센다.
                    //   ③ 의 남은 물음이 「벽을 뚫지 않나」인데, 자리만 찍어서는 **벽을 지나갔는지**
                    //   눈으로 세야 한다. 칸을 직접 물으면 **기계가 센다** — 0 이면 한 번도 안 들어갔다.
                    //   ⚠ 이건 「벽 칸을 밟았나」지 「벽을 넘어갔나」가 아니다.
                    //     한 틱에 벽을 통째로 건너뛰면 이 셈은 0 으로 남는다 — 그래서
                    //     **표본 사이 최대 이동**도 같이 찍는다(한 칸 23 보다 크면 의심)
                    const bool inWall = srv.WallAt(px, py);
                    if (inWall) ++tracePetsInWall;
                    ++tracePetsSamples;
                    // ⚠ 「벽칸」은 정상이면 **늘 0** 이라, 그것만 찍으면 「계측이 안 도는 것」과 구별이 안 된다.
                    //   그래서 **가장 가까운 벽까지 거리**를 옆에 같이 찍는다 — 이 값이 표본마다 변하면
                    //   벽 자료가 실려 있고 질의가 살아 있다는 뜻이다(0 옆에 지나간 양을 같이 읽는다).
                    const double wallDist = srv.DebugWallDistance(px, py);
                    std::printf("[소환수길] 틱 %u · %zu번 (%d,%d) 맨위행동 %d 길목 %u"
                                " · 벽칸 %s(누적 %llu/%llu · 가장가까운벽 %.0f)"
                                " · 이음탐 %llu 막다른길 %llu\n",
                                srv.frame(), pi, static_cast<int>(px), static_cast<int>(py),
                                top >= 0 ? srv.DebugAIAction(pi, top) : -1, wp,
                                inWall ? "예" : "아니오", tracePetsInWall, tracePetsSamples, wallDist,
                                srv.roamAdvance(), srv.roamDeadEnd());
                }
            }
            // ★ 09-23 nine-defects 0-c — `--quiet`(판정 하네스 `judge.ps1`)에서도 능력·작살 갈래 두 줄은
            //   **바뀐 때만** 10초에 한 번 찍는다. 판정 판은 늘 `--quiet` 라 아래 1초 줄이 꺼져 있어,
            //   다음 사람 판에서 능력별 숫자가 안 나올 뻔했다
            if (quiet && made > 0 && (srv.frame() % 300) == 0) {
                static unsigned long long lastAbilFp = 0;
                const unsigned long long fp = AbilityFingerprint(srv);
                if (fp != lastAbilFp) {
                    PrintAbilityBreakdown(srv);
                    std::fflush(stdout);
                    lastAbilFp = fp;
                }
            }
            // 매 틱 찍으면 화면이 흐르니 1초에 한 번만.
            if (!quiet && made > 0 && (srv.frame() % 30) == 0) {
                // ★ 걸음·근접 누계를 같이 찍는다 — 클라가 보낸 조작이 **실제로 반영됐나**를
                //   보는 유일한 자리다. 와이어에 걸음 코드가 152번 보여도 서버는 두 번만
                //   밀 수 있다(2026-09-08 실측). 둘을 나란히 놓아야 그 어긋남이 보인다.
                // ★ 주문·날아가는 것·맞은 수도 같이. 「주문은 나갔는데 아무도 안 맞는다」가
                //   실제로 있었다(2026-09-08: 시전 12건 · 피해 1건). 셋을 나란히 놓아야
                //   시전에서 멈췄나, 날아가다 사라졌나, 맞았는데 피해가 0이냐가 갈린다.
                unsigned long long nSpell = 0;
                for (const auto& sp : srv.spells()) nSpell += sp.second;
                // ★ 판이 끝났나도 같이 — 아레나의 끝조건이 「한 판이 시작되고 끝난다」라서
                //   이 한 글자가 그 판정이다. 승리 조각(0x58)은 속 짜임이 미상이라
                //   클라 화면에 안 그려질 수 있으니, 서버 쪽에서 보이게 둔다.
                // ★ 「사람」 = 게임에 들어와 있는 수 / 갱신에 실은 수.
                //   **둘이 서로 보이는지**를 가르는 자리다. 와이어를 뜯어 세면
                //   내 디코더가 항목을 잘못 쪼개 헛것을 볼 수 있다(2026-09-08 세 번).
                // ★ 모드별 진척 — 아레나가 아니면 이 값이 「판이 도는가」의 판정이다.
                char modeInfo[96] = "";
                if (mode == "ctf") {
                    std::snprintf(modeInfo, sizeof(modeInfo), " · 깃발 %zu개 득점 %u:%u",
                                  srv.FlagCount(), (unsigned)srv.CapturesOf(1),
                                  (unsigned)srv.CapturesOf(2));
                } else if (mode == "quest") {
                    std::snprintf(modeInfo, sizeof(modeInfo),
                                  " · 스테이지 %d · 이 웨이브 남음 %d(잡음 %d)",
                                  srv.QuestStage() + 1, srv.QuestLeft(), srv.QuestKilled());
                }
                std::printf("[틱] snapIn=%d 루프본것=%zu/%zu · 프레임 %u · 조각 %zu개 · 사람 %zu(갱신에 %zu) · "
                            "걸음 %llu · 근접 %llu(클릭 %llu) · "
                            "주문 %llu(마나막힘 %llu) · 나는것 %zu(만든 %llu · 사라짐 %llu · 갱신에 %llu) · 피해 %llu · 별칭받음 %llu%s"
                            // ★ 09-21 — 되받기는 「몇 번 실었나」보다 **값이 올라가나**가 뜻이 있다.
                            //   옛 「다름」은 `ack`(클라 순번+1)과 `q.last`(우리 스트림0 순번)를 견주던
                            //   숫자라 거의 늘 달랐다 — 결함이 아닌데 결함처럼 읽혔다(09-21 정정).
                            //   그리고 **스트림별로 갈라** 찍는다 — 되받기는 같은 스트림에 실려야 뜻이 있다
                            " · 악수(수락 %llu·맵지정 %llu·준비 %llu·신뢰받음 %llu"
                            "·되받기 %llu[값바뀜 %llu·마지막 %u]·무적건 %llu)"
                            " · 스트림별(받은신뢰 0:%llu 1:%llu 2:%llu · 보낸신뢰 0:%llu 1:%llu 2:%llu"
                            " · 보낸비신뢰 0:%llu 1:%llu 2:%llu)"
                            " · 시전거절(죽음 %llu·범위 %llu·안배움 %llu·시전중 %llu·직업 %llu·못씀 %llu→받음 %llu)"
                            // ★ 시전 뒷단 갈래 — 「받음은 있는데 발사가 0」일 때 어디서 새는지 가른다
                            " · 시전뒷단(부름 %llu·잎0 %llu·번호0 %llu·안배움 %llu·커서아군 %llu·막힘 %llu·시전실패 %llu[주먹 중복 %llu·안보임 %llu])"
                            " · 화살사라짐(벽 %llu[태어나자마자 %llu]·수명 %llu·평균 %.1f틱·**동시최대 %zu**)"
                            // ★ 09-19 — 「항목에 담았다」 ≠ 「와이어에 나갔다」.
                            //   512B 를 넘기면 EncodeUpdate 가 0 을 돌려주고 그 틱은 통째로 안 나간다
                            " · 갱신와이어(나감 %llu·**무산 %llu**[그때 항목 최대 %zu]·나간 것 항목 최대 %zu"
                            "·잘린 항목 %llu[%llu틱·한통최대 %zu]·스냅샷쪽 잘림 %llu)"
                            // ★ 09-19 ⑧ 능력 — 받은 것부터 실행까지 갈래를 갈라 센다
                            " · 능력(받음 %llu[꼴틀림 %llu·관전 %llu] · 들어옴 %llu · **실행 %llu**"
                            " · 거절[번호 %llu·죽음 %llu·워리어아님 %llu·겹침 %llu·이미켜짐 %llu"
                            "·상태12 %llu·안배움 %llu·짐넘침 %llu·쉬는중 %llu])"
                            // ★ 09-19 검은 실루엣 — 접속 묶음에 플레이어 정보 97B 가 실렸나
                            " · 접속정보(실음 %llu·**0으로채움 %llu**·마지막 ACCEPT 본문 %zuB)"
                            " · 첫자리(맵 시작점 %llu·고정값 %llu)"
                            " · 높이알림 %llu"
                            // ★ 09-19 ⑦ 작살 — 뒷단 갈래
                            " · 작살(들어옴 %llu·종류모름 %llu·**쏨 %llu**·걸림 %llu·끄는틱 %llu)"
                            " · 판끝 %s (누적 틱 %llu)\n",
                            srv.firstSnapshotIn(), srv.lastLoopSeen(), srv.lastSessions(),
                            srv.frame(), made, srv.inGameCount(), srv.lastUpdateObjects(),
                            srv.moves(), srv.melees(), srv.code6Seen(),
                            nSpell, srv.spellsNoMana(), srv.missiles(), srv.missilesMade(), srv.missilesGone(), srv.missileEntriesSent(), srv.hurts(), srv.aliasHeard(), modeInfo,
                            srv.acceptHeard(), srv.useMapSent(), srv.readyHeard(), srv.reliableHeard(),
                            srv.ackSeqApplied(), srv.ackSeqChanged(),
                            static_cast<unsigned>(srv.ackLastSent()), srv.spawnInvulnApplied(),
                            srv.recvReliableOn(0), srv.recvReliableOn(1), srv.recvReliableOn(2),
                            srv.sendReliableOn(0), srv.sendReliableOn(1), srv.sendReliableOn(2),
                            srv.sendPlainOn(0), srv.sendPlainOn(1), srv.sendPlainOn(2),
                            srv.castRejDead(), srv.castRejRange(), srv.castRejUnknown(),
                            srv.castRejBusy(), srv.castRejClass(), srv.castRejCant(), srv.castAccepted(),
                            srv.spellPlayerCalls(), srv.spellLeafZero(), srv.spellIndZero(), srv.spellNotLearned(),
                            srv.spellFriendAtCursor(), srv.spellErrInform(), srv.spellCastFailed(),
                            srv.fistTooMany(), srv.fistNoLos(),
                            srv.arrowGoneWall(), srv.arrowGoneWallAtBirth(), srv.arrowGoneAge(), srv.arrowLifeAvg(), srv.missilesPeak(),
                            srv.updateEncOk(), srv.updateEncFail(), srv.updateEncFailObjsMax(), srv.updateEncOkObjsMax(),
                            srv.updateDroppedObjs(), srv.updateDroppedTicks(), srv.updateDroppedMax(),
                            srv.updateDroppedSnap(),
                            srv.abilHeard(), srv.abilBadPacket(), srv.abilObserver(),
                            srv.abilCalls(), srv.abilOk(),
                            srv.abilBadNum(), srv.abilDead(), srv.abilNotWarrior(),
                            srv.abilExclusive(), srv.abilAlready(), srv.abilMoveState(),
                            srv.abilNotLearned(), srv.abilOverweight(), srv.abilCooldown2(),
                            srv.joinInfo97Ok(), srv.joinInfo97Null(), srv.acceptBodyLenLast(),
                            srv.spawnFromMap(), srv.spawnFromDefault(),
                            srv.unitHeightSent(),
                            srv.harpoonDoCalls(), srv.harpoonNoType(), srv.harpoonShot(),
                            srv.harpoonAttached(), srv.harpoonPullTicks(),
                            srv.RoundOver() ? "예" : "아니오", nTicks);
                PrintAbilityBreakdown(srv);                  // ★ 09-23 0-c — 위 합계의 능력별·작살 갈래
                // ★ 09-23 0-c — **1초에 한 번 비운다.** 하네스(`crashrun.ps1`)는 stdout 을 파일로 돌리고
                //   끝에 서버를 강제로 끈다 — C 런타임이 4KB 씩 모아 쓰므로 그때 남은 것이 통째로 사라졌다
                //   (nd0c 판: 30초 판에 파일이 `[받음 1]` 에서 끝나고 `[틱]` 줄 0). 이 블록의 나머지 줄은 다음 초에 나간다
                std::fflush(stdout);
                // ★ 09-19 진단 ⑬ — 화살 목록과 **실은 프레임의 분포**.
                //   화면 캡처의 기울기와 1:1 로 맞대는 자리다. 분포가 한 칸뿐이면
                //   「모든 화살이 같은 각도로 그려진다」가 서버 쪽에서 이미 정해진 것이다.
                if (dumpArrows) {
                    const auto& ms = srv.missileList();
                    std::map<unsigned, unsigned> byFrame;
                    for (const auto& mo : ms) ++byFrame[static_cast<unsigned>(mo.dir1 >> 3)];
                    std::printf("[화살목록] %zu개 · 본디프레임 분포", ms.size());
                    for (const auto& kv : byFrame) std::printf(" %u:%u", kv.first, kv.second);
                    size_t shown = 0;
                    for (const auto& mo : ms) {
                        if (shown++ >= 8) break;
                        // ⚠ 이 값은 **원래 방향에서 나온 프레임**이다. `--arrow-frame` 으로
                        //   못 박으면 와이어에는 그 값이 실리지만 여기 숫자는 안 바뀐다
                        //   (덮어쓰기는 보낼 때 `FillEntry` 호출부에서 한다).
                        std::printf(" | %u (%d,%d) dir1=%u→본디프레임 %u",
                                    static_cast<unsigned>(mo.netCode),
                                    static_cast<int>(mo.posX), static_cast<int>(mo.posY),
                                    static_cast<unsigned>(mo.dir1),
                                    static_cast<unsigned>(mo.dir1 >> 3));
                    }
                    std::printf("\n");
                }
                // ★★ B-1b — **신뢰 순번을 스트림별로 갈라 찍는다** (2026-09-18).
                //   합계만 보면 「클라가 보낸 스트림」과 「서버가 답한 스트림」이 어긋나도
                //   드러나지 않는다. 되받기는 **같은 스트림에 실려야** 뜻이 있다
                //   (StreamSeq 가 스트림마다 따로다 — `Server::Emit`).
                //   읽는 법 — 클라가 신뢰를 보낸 스트림에 서버 **비신뢰** 답이 없으면
                //   되받기를 실을 자리가 아예 없다는 뜻이다.
                {
                    std::string sl = "[순번·스트림] 받은신뢰";
                    char sb[48];
                    for (uint8_t st = 0; st < 8; ++st) {
                        if (!srv.recvReliableOn(st)) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, srv.recvReliableOn(st));
                        sl += sb;
                    }
                    sl += " · 보낸신뢰";
                    for (uint8_t st = 0; st < 8; ++st) {
                        if (!srv.sendReliableOn(st)) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, srv.sendReliableOn(st));
                        sl += sb;
                    }
                    sl += " · 보낸비신뢰";
                    for (uint8_t st = 0; st < 8; ++st) {
                        if (!srv.sendPlainOn(st)) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, srv.sendPlainOn(st));
                        sl += sb;
                    }
                    // ★ 되받기로 **어떤 값**을 실었나. 값이 한두 개뿐이면 `recvSeq` 가 안 올라가는 것이다
                    sl += " · 되받기값";
                    bool any = false;
                    for (uint8_t v = 0; v < 32; ++v) {
                        if (!srv.ackValueCount(v)) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)v, srv.ackValueCount(v));
                        sl += sb;
                        any = true;
                    }
                    if (!any) sl += " (없음)";
                    std::printf("%s\n", sl.c_str());
                }

                // ★ 자리 한 줄 — 하네스가 이걸 읽어 걸어갈 각을 잰다(Server.h PositionsLine 참조).
                std::printf("%s\n", srv.PositionsLine().c_str());

                // ★★ **무엇을 썼나 한 줄** (2026-09-12).
                //   09-12 회귀에서 wizard 가 「주문 226」이었는데 **무엇을 226 번 썼는지
                //   알 길이 없었다.** 집계(`spells_`)는 진작 있었는데 **출력이 없었다** —
                //   `[시전]` 줄은 앞 여섯 개만 찍으므로 전수가 아니다.
                //   ⚠ 이 한 줄이 없으면 「공격 주문을 쐈다」와 「치유만 걸었다」가
                //     로그에서 구분이 안 된다. 번호는 `SpellNames.cpp` 로 되짚는다.
                if (!srv.spells().empty()) {
                    std::string sp = "[주문칸]";
                    char sb[40];
                    for (const auto& kv : srv.spells()) {
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)kv.first,
                                      (unsigned long long)kv.second);
                        sp += sb;
                    }
                    std::printf("%s\n", sp.c_str());
                }

                // ★ **버린 것 한 줄** — 길이를 모르는 메시지를 만나면 그 조각을 통째로
                //   폐기한다(`Server.cpp` 의 `probe.stopped()`). 사람은 눌렀는데
                //   아무 일도 안 일어나고 로그에도 안 남던 것이 이것이다.
                //   여기에 찍어 두면 **놀다가 무엇이 버려지는지**가 바로 보인다.
                //   ⚠ 옆의 이름은 참고용이다. 진짜 판정은 오프코드 번호로 한다.
                // ★ 물건 다루기 — 요청·성공·손이 안 닿음을 **갈라서** 찍는다.
                //   합치면 「안 주워진다」의 까닭이 안 보인다: 요청이 0 이면 조작을 못 찾은 것이고,
                //   요청은 있는데 성공이 0 이면 거리·번호 문제다.
                // ★★ **늘 찍는다** — 전에는 `tryGets()||used()||equipped()` 일 때만 찍었다.
                //   그런데 바로 위 주석이 말하는 판정이 **「요청이 0 이면 조작을 못 찾은 것」**이다.
                //   즉 **가장 알고 싶은 0 을 안 찍고 있었다.** 줄이 아예 없으면
                //   「0 이었나」와 「안 쟀나」를 못 가른다(2026-09-11에 이 자리에서 걸렸다).
                // ★★ 조각 D(09-16) — **거절 갈래 넷을 갈라 찍는다.** 앞서 이 줄은
                //   「요청 − 성공 − 멂」의 나머지를 **한 덩어리로 삼켜** 「왜 안 주워졌나」가
                //   한도인지 무게인지 개수인지 직업인지 하나도 안 보였다(조각 C 2번).
                std::printf("[물건] 줍기요청 %llu(성공 %llu·멂 %llu) · 쓰기 %llu(모르는갈래 %llu)"
                            " · 입기 %llu · 벗기 %llu · 되돌림 %llu"
                            " · 거절(한도 %llu·무거움 %llu·개수 %llu·직업 %llu)"
                            " · 즉시마심 %llu · 금 %llu\n",
                            srv.tryGets(), srv.picked(), srv.pickTooFar(),
                            srv.used(), srv.useUnknown(),
                            srv.equipped(), srv.dequipped(), srv.dropped(),
                            srv.pickNoCap(), srv.pickTooHeavy(), srv.pickTooMany(), srv.pickClassFail(),
                            srv.drankOnPickup(), srv.goldPicked());
                // ★★ 조각 D — **여기서부터 넉 줄이 「눈이 없던 자리」다.**
                //   계수기 41개 중 24개가 한 번도 안 찍히고 있었다(조각 C). 아래가 그 24개를
                //   묶음별로 묶어 낸 것이고, 각 줄은 **하나를 물으면 하나가 답하도록** 갈라 놨다.
                //   ⚠ 소리 셋은 합치지 말 것 — 「안 불렀다」·「불렀는데 표가 비어 버렸다」·
                //     「담겼는데 거리에서 잘렸다」가 한 숫자면 묶음 10 이 통째로 판정 불능이 된다.
                std::printf("[소리] 담김 %llu(표없어버림 %llu) · 나감 %llu · 음악 %llu",
                            srv.soundsQueued(), srv.soundsDropped(), srv.soundsSent(), srv.musicSent());
                // ★ **버린 소리는 번호까지 찍는다.** 수만 알면 「정답지대로 버린 것」(소리조각 0)과
                //   「우리가 엉뚱한 번호를 부른 것」이 안 갈린다. 조각 E 에서 진단 판 하나만
                //   「표없어버림 1」이 떴는데 **무엇인지 알 길이 없었다**.
                if (!srv.soundsDroppedIds().empty()) {
                    std::printf(" — 버린 번호");
                    for (const auto& kv : srv.soundsDroppedIds())
                        std::printf(" %u(%s)x%llu", (unsigned)kv.first, nox::SoundName(kv.first), kv.second);
                }
                std::printf("\n");
                // ★ **성장 한 줄** — 진단 스위치(`--debug-xp`·`--debug-give-spell`)가 먹었는지
                //   보는 유일한 자리다. 없을 때는 판 G 가 통째로 판정 불능이었다(조각 C 13·14번).
                if (!bots.empty()) {
                    const std::string b0 = "bot0";
                    std::printf("[성장] 레벨 %d · 경험치 %.0f",
                                srv.DebugLevel(b0), (double)srv.DebugExperience(b0));
                    if (debugSpellId > 0)
                        std::printf(" · 준 주문 %d(%s) 레벨 %d", debugSpellId,
                                    nox::SpellName(static_cast<uint8_t>(debugSpellId)),
                                    srv.DebugSpellLvl(b0, static_cast<uint8_t>(debugSpellId)));
                    std::printf("\n");
                }
                // ★ 조각 I — 체력은 **세 길**로 간다. 「41 남의 체력」만 찍던 때는
                //   `피해 14 · 체력알림 1` 이 「안 보낸다」인지 「딴 길로 간다」인지 안 갈렸다.
                std::printf("[싸움] 겨눔 %llu · 부딪힘 %llu(미룸 %llu) · 무적막힘 %llu"
                            " · 체력 41남 %llu·42줄어듦 %llu(막힘 %llu)·43내것 %llu"
                            " · 마법알림 %llu · 소환수죽음 %llu\n",
                            srv.aims(), srv.tryCollides(), srv.tryCollideDeferred(),
                            srv.invulnBlocked(), srv.unitHealthSent(),
                            srv.healthDeltaSent(), srv.healthDeltaHeld(), srv.healthAbsSent(),
                            srv.enchantSent(), srv.petsKilled());
                // ★ 조각 I — **피해가 어디서 사라지나.** 합이 `피해` 와 맞아야 셈이 닫힌다.
                if (!srv.damageStops().empty()) {
                    // ★ 09-20 — 주인 없는 둘의 갈래도 같이 찍는다(⑮ 덫 화살 ↔ 맵 몬스터).
                    //   둘 다 몬스터면 적이 아니고, 한쪽이 몬스터가 아니면(덫 화살) 적이다
                    std::printf("[피해] 주인없음갈래(둘다몬스터 %llu·뿌리가몬스터아님 %llu) 막힌 곳 —",
                                srv.wildBothMonster(), srv.wildRootNotMonster());
                    for (const auto& kv : srv.damageStops())
                        std::printf(" %s %llu", kv.first.c_str(), kv.second);
                    std::printf("\n");
                }
                std::printf("[크리처] %zu마리(길 든 것 %zu) · 길 계산됨 %s(상태 %d) · 겨눈 덫 %zu"
                            " · 덫판정(통과 %llu·멂 %llu·방향 %llu·벽 %llu)"
                            " · **떠돌기(이음탐 %llu·막다른길 %llu)**"
                            // ★ 09-21 — 벽 튕김 재기(사용자가 화면으로 짚음). 걷기 힘은 3.0 · 걷기 속도 6 이 정상
                            " · **벽밀림(걸음 %llu·최대가속 %.0f·속도최대 %.1f"
                            "·벽까지최소 %.1f·동시조각최대 %llu·벽칸 %llu/%llu)**"
                            // ★ 09-21 — **오브젝트가 민 것**은 벽과 따로 센다. 벽만 세면 0 을
                            //   「튕김 없음」으로 읽는다(속도 54.8 을 만든 것이 이쪽이었다)
                            " · **몸밀림(놓인것 %llu·사람 %llu·소환수 %llu·최대가속 %.0f"
                            " | 그때 반지름 %.0f+%.0f=%.0f·거리 %.1f·겹침 %.1f·사람(%.0f,%.0f)·물건(%.0f,%.0f))**"
                            // ★ 09-21 — 가장 세게 민 **벽 칸**의 정체. 문·창·재질 깃발을 가르는 자리다
                            " · **최악벽(가속 %.0f·거리 %.1f·칸 %d,%d·방향 %d·재질 %d[깃발 0x%X·secret %d]·사람(%.0f,%.0f))**"
                            " · **추적막음 %llu/%llu걸음**"
                            // ★ 09-22 — 닿는 **첫 걸음**의 깊이. 규칙대로면 0.03 근처여야 한다
                            " · **첫닿음깊이(평균 %.2f·최대 %.2f·건수 %llu)**"
                            // ★ 09-22 — 대조군(`NOX_DBG_PUSH=1` 의 [밀기])과 **똑같은 네 숫자**
                            " · **[밀기] 건수 %llu · 평균 %.1f · 최대 %.1f · 100넘음 %llu**"
                            // ★ 09-21 밤 — 되먹임 사슬의 **첫 고리**. 직전 속도가 12 이하면 거기가 시작이다
                            " · **첫발길질(%s·가속 %.0f·깊이 %.1f·**직전속도 %.1f**·프레임 %llu·칸 %d,%d 방향 %d·자리(%.0f,%.0f))**\n",
                            srv.petCount(), srv.petsWithPath(),
                            srv.DebugPathCalculated() ? "예" : "아니오", srv.DebugPathStatus(),
                            srv.trapsArmed(),
                            srv.trapSeeOk(), srv.trapFailRange(), srv.trapFailFacing(), srv.trapFailWall(),
                            srv.roamAdvance(), srv.roamDeadEnd(),
                            srv.wallPushSteps(), srv.wallPushMax(), srv.playerSpeedMax(),
                            srv.wallNearestLen(), srv.wallPushPerStepMax(),
                            srv.playerInWallSteps(), srv.playerSteps(),
                            srv.bodyPushSteps(0), srv.bodyPushSteps(1), srv.bodyPushSteps(2),
                            srv.bodyPushMax(),
                            srv.bodyPushWorst().r1, srv.bodyPushWorst().r2,
                            srv.bodyPushWorst().r1 + srv.bodyPushWorst().r2,
                            srv.bodyPushWorst().len,
                            srv.bodyPushWorst().r1 + srv.bodyPushWorst().r2 - srv.bodyPushWorst().len,
                            srv.bodyPushWorst().px, srv.bodyPushWorst().py,
                            srv.bodyPushWorst().ox, srv.bodyPushWorst().oy,
                            srv.wallPushWorst().mag, srv.wallPushWorst().len,
                            srv.wallPushWorst().gx, srv.wallPushWorst().gy,
                            srv.wallPushWorst().dir, srv.wallPushWorst().tile,
                            srv.wallPushWorst().f2, srv.wallPushWorst().flag80 ? 1 : 0,
                            srv.wallPushWorst().px, srv.wallPushWorst().py,
                            srv.traceBlocked(), srv.playerSteps(),
                            srv.wallFirstTouchN() ? srv.wallFirstTouchSum() / srv.wallFirstTouchN() : 0.0,
                            srv.wallFirstTouchMax(), srv.wallFirstTouchN(),
                            srv.pushN(), srv.pushAvg(), srv.pushMax(), srv.pushBig(),
                            !srv.firstKick().have ? "없음"
                              : (srv.firstKick().kind == 3 ? "벽"
                              : (srv.firstKick().kind == 0 ? "놓인것"
                              : (srv.firstKick().kind == 1 ? "사람" : "소환수"))),
                            srv.firstKick().accel, srv.firstKick().depth,
                            srv.firstKick().speedBefore, srv.firstKick().frame,
                            srv.firstKick().gx, srv.firstKick().gy, srv.firstKick().dir,
                            srv.firstKick().px, srv.firstKick().py);
                // ★ 09-21 밤 — 발길질을 **차례대로**. 깊이가 불어나면 되먹임이고, 안 불어나면 딴 원인이다
                if (srv.kickSeqCount()) {
                    std::printf("[발길질 차례] 모두 %zu회 중 앞 %zu회 —", srv.kickSeqTotal(), srv.kickSeqCount());
                    for (size_t k = 0; k < srv.kickSeqCount(); ++k) {
                        const auto& q = srv.kickSeq(k);
                        std::printf(" %zu)%s 깊이%.1f 속도%.1f 프레임%llu", k + 1,
                                    q.kind == 3 ? "벽" : "물건", q.depth, q.speedBefore, q.frame);
                    }
                    std::printf("\n");
                }
                std::printf("[판] 판 수 %llu · 깃발알림 %llu · 말 %llu · 시계요청 %llu"
                            " · FX %llu · 겉모습 %llu(모르는 것 %llu)"
                            " · 문 %llu(되맞춤 %llu·가장가까이 %.0f) · 오벨리스크 %llu · **밟는판(눌림 %llu·뗌 %llu)**"
                            " · **비밀벽(열림 %llu·닫힘 %llu)**"
                            " · 자기주문 %llu · 미룬주문효과 %llu\n",
                            srv.roundsPlayed(), srv.flagStatusSent(), srv.chats(), srv.timerAsks(),
                            srv.fxSent(), srv.appearanceSent(), srv.appearanceUnknown(),
                            srv.doorsOpened(), srv.doorResync(), srv.doorNearestDist(), srv.obeliskMana(), srv.plateOn(), srv.plateOff(),
                            srv.wallOpenSent(), srv.wallCloseSent(),
                            srv.spellsOnSelf(), srv.spellEffectDeferred());
                if (!srv.stoppedByOp().empty()) {
                    std::printf("[버림] 조각 %llu개 —", srv.stopped());
                    for (const auto& kv : srv.stoppedByOp()) {
                        std::printf(" %02x(%s)x%llu", kv.first,
                                    nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
                    }
                    std::printf("\n");
                }
                std::fflush(stdout);
            }
        }
    }
}
