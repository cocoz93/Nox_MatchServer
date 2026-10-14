// main.cpp — 진짜 소켓을 `nox::Server` 에 붙인다.
//
// **여기에는 프로토콜이 없다.** 받은 바이트를 그대로 넘기고, 나온 바이트를 그대로 보낸다.
// 서버가 무엇에 어떻게 답하는지는 `server/src/UdpServer.cpp`(접속 제어 · 와이어)와 `server/src/Server.cpp`(게임)에 있고,
// 같은 로직을 대조 장치(`server/src/replay.cpp`)가 덤프로 돌려 채점한다.
//
//   [진짜 소켓]  main.cpp    ─▶  CNetworkThread(소켓 · CUdpServer) ⇄ 큐 둘 ⇄ nox::Server   — 스레드 둘
//   [덤프 재생]  replay.cpp  ─▶  CSyncHost(CUdpServer + nox::Server)                       — 한 스레드
//   (10-09 플랜 A 5d — 옛 그림의 `Server::OnPacket` 은 없어졌다. `Docs/LAYERS_DESIGN.md`)
//
// 지금 답하는 것 = 서버 찾기 · 접속 시도 · 접속 수락 · 핑.
//
//   nox_server.exe [--port 18590] [--name "MY SERVER"] [--selftest] [--quiet]
//                  --config <설정 파일>([token] key) | --no-join-token(토큰 검사 끄기 — 시험 도구 · LAN 실행)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
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
#include "nox/NetworkThread.h"
#include "nox/RoomManager.h"
#include "nox/IniFile.h"
#include "nox/Clock.h"
#include "nox/MapFile.h"
#include "nox/ThingDb.h"
#include "nox/SpellNames.h"
#ifdef DecryptFile          // windows.h 가 DecryptFile → DecryptFileA 로 바꾼다(링크 오류 LNK2019)
#undef DecryptFile
#endif
#include "nox/NoxCrypt.h"   // ★ 09-27 B3 — modifier.bin 복호

#include "nox/WorldBuild.h"
// ★ 10-08 — 크래시 덤프(ServerCore 사본). 넣기만 하면 전역 인스턴스가 처리기를 건다 —
//   덤프는 exe 옆 `CrashDump\` 에 남고 프로세스는 종료 코드 1 로 끝난다(`Docs/LAYERS_DESIGN.md` 조각 2).
//   사본은 원본 그대로 두므로, 그쪽 처리기의 안 쓰는 매개변수 경고(C4100)만 여기서 끈다.
#pragma warning(push)
#pragma warning(disable: 4100)
#include "CrashDump.h"
#pragma warning(pop)

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
    // ★ 10-09 — 줄 하나를 다 만든 뒤 한 번에 찍는다(나눠 찍으면 네트워크 스레드의 `[받음]` 이 그 사이에 낀다 — 10-09 실측
    //   `… 돌진틱 0[받음 13366] …`)
    std::string line;
    char b[1024];
    std::snprintf(b, sizeof(b), "[능력별] 번호밖 %llu", srv.abilBadNum()); line += b;
    for (uint8_t a = 1; a < nox::kAbilityCount; ++a) {
        unsigned long long sum = 0;
        for (size_t g = 0; g < nox::kAbilGateCount; ++g) sum += srv.abilGate(a, static_cast<G>(g));
        std::snprintf(b, sizeof(b), " | %u%s 들어옴 %llu%s = 실행 %llu + 거절{죽음 %llu·워리어아님 %llu·겹침 %llu",
                      (unsigned)a, kName[a], srv.abilIn(a), sum == srv.abilIn(a) ? "" : " ⚠칸합불일치",
                      srv.abilGate(a, G::Ok), srv.abilGate(a, G::Dead), srv.abilGate(a, G::NotWarrior),
                      srv.abilGate(a, G::Exclusive)); line += b;
        if (a <= 3) {                                        // 같이 못 켜는 셋만 「누가 막았나」가 있다
            std::snprintf(b, sizeof(b), "("); line += b;
            bool first = true;
            for (uint8_t o = 1; o <= 3; ++o) {
                if (o == a) continue;
                std::snprintf(b, sizeof(b), "%s%s %llu", first ? "" : "·", kName[o], srv.abilBlockedBy(a, o)); line += b;
                first = false;
            }
            std::snprintf(b, sizeof(b), ")"); line += b;
        }
        std::snprintf(b, sizeof(b), "·깃발 %llu", srv.abilGate(a, G::CtfFlag)); line += b;   // ★ 2-b1
        std::snprintf(b, sizeof(b), "·이미켜짐 %llu·상태12 %llu·안배움 %llu·짐넘침 %llu·쉬는중 %llu}"
                      " 꺼짐{시간 %llu·사라짐 %llu·되돌림 %llu·전부 %llu·작살끊김 %llu·동작이끔 %llu}"
                      " 소리부름{쓸때 %llu·꺼질때 %llu}",
                      srv.abilGate(a, G::Already), srv.abilGate(a, G::MoveState), srv.abilGate(a, G::NotLearned),
                      srv.abilGate(a, G::Overweight), srv.abilGate(a, G::Cooldown),
                      srv.abilEnded(a, E::Expired), srv.abilEnded(a, E::Gone), srv.abilEnded(a, E::Reset),
                      srv.abilEnded(a, E::CancelAll), srv.abilEnded(a, E::HarpoonBreak), srv.abilEnded(a, E::Disabled),
                      srv.abilSoundUseCalls(a), srv.abilSoundOffCalls(a)); line += b;
        if (a == 4) { std::snprintf(b, sizeof(b), " 달리기막음 %llu", srv.treadBlockedRun()); line += b; }   // ★ 1-a — 살금살금이 걷기로 누른 틱
        // ★ 2-a1 — 함성 애니가 외치는 칸(2→3)을 넘은 수 · 애니가 끝나 켜짐 값을 내린 수
        if (a == 2) { std::snprintf(b, sizeof(b), " 외침칸 %llu·애니끝 %llu·기절몬스터 %llu", srv.warcryShouts(), srv.abilValOff(), srv.warcryStunned()); line += b; }
        if (a == 1) { std::snprintf(b, sizeof(b), " 돌진틱 %llu", srv.berserkChargeTicks()); line += b; }   // ★ 2-b1 — 광전사가 앞으로 민 틱
        if (a == 1) {                                        // ★ 2-b2 — 켜진 채 부딪힘 함수를 부른 수 = 갈래 합
            using B = nox::BerserkHit;
            std::snprintf(b, sizeof(b), " 부딪힘 %llu%s = {유닛 %llu·물체 %llu·벽세게 %llu·벽살짝 %llu·가벼워지나감 %llu·겹쳐지나감 %llu·문 %llu}",
                          srv.berserkCollideCalls(), srv.BerserkHitSum() == srv.berserkCollideCalls() ? "" : " ⚠칸합불일치",
                          srv.berserkHit(B::Unit), srv.berserkHit(B::Object), srv.berserkHit(B::WallHard),
                          srv.berserkHit(B::WallSoft), srv.berserkHit(B::Light), srv.berserkHit(B::Overlap), srv.berserkHit(B::Door)); line += b;
        }
    }
    srv.Log("%s\n", line.c_str());
    using T = nox::HarpoonTouch;
    using H = nox::HarpoonEnd;
    // ★ 4-c3b2b — 닿음·끊김에 「피해안들어감」 · 끊길 때 따로 내는 기본 타격 소리(`532E20`) 칸
    srv.Log("[작살갈래] 쏨 %llu · 휘기{잡음 %llu·튼틱 %llu} · 닿음{주인없음 %llu·벽 %llu·자기 %llu·적아님 %llu·피해안들어감 %llu·걸림 %llu}"
                " · 끊김{새로쏨 %llu·벽 %llu·적아님 %llu·피해안들어감 %llu·화살없음 %llu·주인죽음 %llu·상대사라짐 %llu·상대죽음 %llu"
                "·멀어짐 %llu·다끌어옴 %llu·수명 %llu·안움직임 %llu·시야막힘 %llu·너무멀리날아감 %llu·휘던상대죽음 %llu·시험 %llu}"
                " · 끊길때소리 %llu = {냄 %llu·부서짐 %llu·거름 %llu·표없음 %llu·안옮김 %llu}\n",
                srv.harpoonShot(), srv.harpoonAimFound(), srv.harpoonSteerTicks(),
                srv.harpoonTouch(T::NoOwner), srv.harpoonTouch(T::Wall), srv.harpoonTouch(T::Self),
                srv.harpoonTouch(T::NotEnemy), srv.harpoonTouch(T::Refused), srv.harpoonTouch(T::Attached),
                srv.harpoonEnd(H::Replaced), srv.harpoonEnd(H::Wall), srv.harpoonEnd(H::NotEnemy), srv.harpoonEnd(H::Refused),
                srv.harpoonEnd(H::BoltGone), srv.harpoonEnd(H::OwnerDead), srv.harpoonEnd(H::TargetGone),
                srv.harpoonEnd(H::TargetDead), srv.harpoonEnd(H::TooFar), srv.harpoonEnd(H::Arrived),
                srv.harpoonEnd(H::Lifetime), srv.harpoonEnd(H::Stuck), srv.harpoonEnd(H::NoSight),
                srv.harpoonEnd(H::OverFlight), srv.harpoonEnd(H::AimDead), srv.harpoonEnd(H::Debug),
                srv.harpoonHitSoundCalls(), srv.harpoonHitSoundWay(nox::DefHitSoundWay::Played),
                srv.harpoonHitSoundWay(nox::DefHitSoundWay::Breakable), srv.harpoonHitSoundWay(nox::DefHitSoundWay::Filtered),
                srv.harpoonHitSoundWay(nox::DefHitSoundWay::TableEmpty), srv.harpoonHitSoundWay(nox::DefHitSoundWay::NotPorted));
    // ★ 1-d — 맵 불 같은 장애물의 닿음 피해. 옆에 전체 피해(`hurts`)를 같이 찍는다 — 0 이 「안 닿았다」인지 「못 셌다」인지 가르려고
    //   ⚠ 「부름」은 양 1 짜리 불의 짝수 프레임(피해 0)까지 센다 — 정답지도 피해 함수를 0 으로 부른다
    srv.Log("[맵닿음] 장애물 닿음 피해 부름 %llu회 · 전체 피해 %llu회 · 사람 벽 짝 %llu회\n", srv.contactDamageHits(), srv.hurts(),
                srv.wallContacts());   // ★ 2-b2 — 벽 짝은 광전사 벽 갈래의 분모
    // ★ 3-a — 점프. 「코드7 받음」이 분모다 — 사람이 휠을 눌렀는데 0 이면 **7 이 점프라는 가정부터** 틀린 것이다
    using J = nox::JumpGate;
    srv.Log("[점프] 코드7 받음 %llu · 들어옴 %llu%s = {뜀 %llu·못움직임 %llu·광전사 %llu·함성 %llu·짐넘침 %llu·기력모자람 %llu}"
                " · 민틱 %llu · 작살에잡혀안민틱 %llu · 내려옴 %llu · 가만이끔 %llu · 움찔막음 %llu · 지진뺌 %llu · 시전막음 %llu\n",
                srv.jumpCodes(), srv.jumpIn(), srv.JumpGateSum() == srv.jumpIn() ? "" : " ⚠칸합불일치",
                srv.jumpGate(J::Ok), srv.jumpGate(J::CantMove), srv.jumpGate(J::Berserk), srv.jumpGate(J::Warcry),
                srv.jumpGate(J::Overweight), srv.jumpGate(J::NoStamina),
                srv.jumpForceTicks(), srv.jumpHeldTicks(), srv.jumpLanded(), srv.airborneOffIdle(), srv.recoilBlockedAirborne(),
                srv.equakeSkipAirborne(), srv.spellRefusedAirborne());   // ★ 3-b
    // ★ 4-a — 휘두르기 소리. 판정 수(근접)가 분모 — 빗나간 판정마다 그 갈래 소리 하나(망치는 판정마다 882)
    // ★ 4-c2 — 사람이 맞을 때 소리 갈래(부름 = 합 · 「안옮김」 = 때린 것을 안 넘기는 자리 — 진단용 때리기 · 종류 모르는 오브젝트)
    // ★ 4-c3b3b — 「몬스터소리」 = 때린 이가 몬스터이고 그 소리 모음의 맞히는 소리에 조각이 있어 맞는 쪽 소리를 안 냄
    using HW = nox::HitSoundWay;
    srv.Log("[휘두름] 판정 %llu · 소리부름{879주먹 %llu·880롱소드 %llu·881칼 %llu·882망치 %llu·883도끼 %llu·884철퇴 %llu}"
                " · 사람맞는소리 %llu%s = {냄 %llu·표없음 %llu·때린것없음 %llu·몬스터소리 %llu·안옮김 %llu}\n",
                srv.melees(), srv.swingSound(879), srv.swingSound(880), srv.swingSound(881), srv.swingSound(882),
                srv.swingSound(883), srv.swingSound(884), srv.hitSoundCalls(),
                srv.HitSoundWaySum() == srv.hitSoundCalls() ? "" : " ⚠칸합불일치",
                srv.hitSoundWay(HW::Played), srv.hitSoundWay(HW::TableEmpty), srv.hitSoundWay(HW::NoHitter),
                srv.hitSoundWay(HW::MonsterSound), srv.hitSoundWay(HW::NotPorted));
    // ★ 4-c3a — 소환수가 맞을 때 기본 타격 소리 갈래(부름 = 합)
    using DW = nox::DefHitSoundWay;
    srv.Log("[소환수맞는소리] 부름 %llu%s = {냄 %llu·부서짐 %llu·거름 %llu·표없음 %llu·몬스터소리 %llu·안옮김 %llu}\n", srv.defHitSoundCalls(),
                srv.DefHitSoundWaySum() == srv.defHitSoundCalls() ? "" : " ⚠칸합불일치",
                srv.defHitSoundWay(DW::Played), srv.defHitSoundWay(DW::Breakable), srv.defHitSoundWay(DW::Filtered),
                srv.defHitSoundWay(DW::TableEmpty), srv.defHitSoundWay(DW::MonsterSound), srv.defHitSoundWay(DW::NotPorted));
    // ★ 09-24 H1 — 몬스터가 근접으로 때린 뒤 그 모음의 맞히는 소리(`GAME4_3.c:2087~2094`) · 「모음에없음」 = 번호 0(모음 없음·NULL)
    srv.Log("[몬스터근접소리] 때림 %llu = {맞히는소리 냄 %llu·모음에없음 %llu}\n",
                srv.monsterMeleeHitSnd(true) + srv.monsterMeleeHitSnd(false), srv.monsterMeleeHitSnd(true), srv.monsterMeleeHitSnd(false));
    // ★ 09-24 H3 — 함성이 둘레 사람을 본 경우(시전자 제외) · 「걸림」 = 반마법(29)이 섰다 · 알림 = 29 비트가 실린 0x5A 수(받는 사람마다 1)
    using WP = nox::WarcryPeer;
    srv.Log("[함성둘레] 사람 %llu = {밖·죽음 %llu·워리어 %llu·관전 %llu·같은편 %llu·멀리 %llu·안보임 %llu·거절 %llu·걸림 %llu} · 반마법 알림 %llu\n",
                srv.WarcryPeerSum(), srv.warcryPeer(WP::NotInGame), srv.warcryPeer(WP::Warrior), srv.warcryPeer(WP::Observer),
                srv.warcryPeer(WP::SameTeam), srv.warcryPeer(WP::OutOfRange), srv.warcryPeer(WP::NoSight), srv.warcryPeer(WP::Rejected),
                srv.warcryPeer(WP::Applied), srv.enchantSentAntiMagic());
    // ★ 09-26 M2·M3 — 닿으면 죽음 넘기기(넘김 · 시간이 남아 안 넘김) · 바닥 칸 6(용암) 피해 부름
    srv.Log("[닿음·바닥] 죽음 넘김 %llu · 안넘김 %llu · 용암 피해 %llu · 용암 피해(소환수) %llu\n", srv.deathPass(true), srv.deathPass(false),
                srv.lavaHurts(), srv.lavaHurtsPet());   // ★ 09-27 S2 — 소환수·맵 몬스터 몫
    // ★ 09-26 M4a — 몬스터 소리 모음 칸별 냄/모음에없음
    srv.Log("[몬스터소리] 근접시작 %llu/%llu · 쏘기시작 %llu/%llu · 쏘기 %llu/%llu · 죽음 %llu/%llu · 명령대답 %llu/%llu (냄/모음에없음)\n",
                srv.monsterSnd(5, true), srv.monsterSnd(5, false), srv.monsterSnd(9, true), srv.monsterSnd(9, false),
                srv.monsterSnd(10, true), srv.monsterSnd(10, false), srv.monsterSnd(14, true), srv.monsterSnd(14, false),
                srv.monsterSnd(16, true), srv.monsterSnd(16, false));
    // ★ 09-28 박쥐 — 공격 사슬 가기(7)가 대상을 따라 자리를 고친 틱 수 · 대상이 안 보여 잊은 횟수
    srv.Log("[몬스터쫓기] 따라감 %llu · 잊음 %llu\n", srv.aiMoveTargetFollow(), srv.aiMoveTargetForgot());
    {   // ★ 09-29 B1 — 알아보기. 후보 = 적아님 + 이미 + 부채꼴밖 + 안보임 + 찼음 + 넣음 · 뺌 = 여섯 까닭 합
        using SS = nox::SightStat;
        srv.Log("[알아보기] 훑기 %llu · 후보 %llu = 적아님 %llu + 이미 %llu + 부채꼴밖 %llu + 안보임 %llu + 찼음 %llu + 넣음 %llu · "
                    "뺌 죽음 %llu · 버프 %llu · 멀어짐 %llu · 옮겨짐 %llu · 시야선 %llu · 밀림 %llu · 발견 소리 %llu\n",
                    srv.sightStat(SS::Scan), srv.sightStat(SS::Cand), srv.sightStat(SS::NotEnemy), srv.sightStat(SS::Already),
                    srv.sightStat(SS::Cone), srv.sightStat(SS::Los), srv.sightStat(SS::FullFar), srv.sightStat(SS::Added),
                    srv.sightStat(SS::LostGone), srv.sightStat(SS::LostCantSee), srv.sightStat(SS::LostFar), srv.sightStat(SS::LostMoved),
                    srv.sightStat(SS::LostRecheck), srv.sightStat(SS::LostReplaced), srv.sightStat(SS::Spot));
        using B2 = nox::B2Stat;   // ★ 09-29 B2 — 놀기·되갚기·소리
        srv.Log("[몬스터놀기] 맞음 %llu(다침소리 %llu) · 도발 %llu(쉼 %llu) · 놀때소리 %llu · 놀기{싸움 %llu·때린데봄 %llu·지난데봄 %llu·그대로 %llu} · "
                    "되갚기{싸움 %llu·적아님 %llu·떠돌기 %llu} · 싸움 시작 %llu · 끝 %llu\n",
                    srv.b2Stat(B2::Injured), srv.b2Stat(B2::InjuredSound), srv.b2Stat(B2::Taunt), srv.b2Stat(B2::TauntQuiet), srv.b2Stat(B2::IdleSound),
                    srv.b2Stat(B2::IdleFight), srv.b2Stat(B2::IdleLookDamager), srv.b2Stat(B2::IdleFaceBack), srv.b2Stat(B2::IdleStay),
                    srv.b2Stat(B2::RetFight), srv.b2Stat(B2::RetNotEnemy), srv.b2Stat(B2::RetRoam), srv.b2Stat(B2::FightStart), srv.b2Stat(B2::FightEnd));
        using MW = nox::MeleeStrikeWay;   // ★ 09-29 B3 — 근접 함수 끝남 갈래
        srv.Log("[몬스터근접] 맞힘 %llu · 대상없음 %llu · 시야막힘 %llu · 폭탄병 %llu · (독 %llu · 유령도망 %llu) · 빗나감소리 %llu\n",
                    srv.meleeStrike(MW::Hit), srv.meleeStrike(MW::NoTarget), srv.meleeStrike(MW::Blocked), srv.meleeStrike(MW::Bomber),
                    srv.meleeStrike(MW::Poisoned), srv.meleeStrike(MW::GhostFlee), srv.meleeStrike(MW::MissSound));
        using AT = nox::AttackTiming;     // ★ 09-30 — 시작 → 칸 틱 수(기대 칸 × (붙듦+1) − 1 ~ 칸 × (붙듦+1))
        srv.Log("[몬스터시점] 근접 제때 %llu · 어긋남 %llu · 끝칸두번째 %llu (틱 %d~%d) · 쏘기 제때 %llu · 어긋남 %llu · 끝칸두번째 %llu (틱 %d~%d · 겨눌곳없음 %llu)\n",
                    srv.attackTiming(AT::MeleeOnTime), srv.attackTiming(AT::MeleeOff), srv.attackTiming(AT::MeleeLastRepeat),
                    srv.attackLagMin(0), srv.attackLagMax(0),
                    srv.attackTiming(AT::ShotOnTime), srv.attackTiming(AT::ShotOff), srv.attackTiming(AT::ShotLastRepeat),
                    srv.attackLagMin(1), srv.attackLagMax(1), srv.shotNoAim());
        using BS2 = nox::BundleStat;      // ★ 09-30 한 틱 한 데이터그램
        srv.Log("[묶음] 낸 조각 %llu · 담음 시각 %llu·갱신 %llu·잡다 %llu·중요 %llu · 넘쳐 끊음 %llu(더 낸 조각 %llu · 혼자 넘음 %llu) · 곧바로 스트림 %llu·제어 %llu·게임밖 %llu·시험 %llu·채팅 %llu·서두 %llu\n",
                    srv.bundleStat(BS2::Datagrams), srv.bundleStat(BS2::InTs), srv.bundleStat(BS2::InUpd), srv.bundleStat(BS2::InMisc),
                    srv.bundleStat(BS2::InImp), srv.bundleStat(BS2::Overflow), srv.bundleStat(BS2::OverExtra), srv.bundleStat(BS2::OverSize),
                    srv.bundleStat(BS2::DirectStream), srv.bundleStat(BS2::DirectCtrl), srv.bundleStat(BS2::DirectNotInGame),
                    srv.bundleStat(BS2::DirectTest), srv.bundleStat(BS2::DirectChat), srv.bundleStat(BS2::DirectMirror));
        using BS = nox::BrainStat;   // ★ 09-29 B4 — 두뇌 반응 층
        srv.Log("[몬스터두뇌] 돎 %llu · 혼란 %llu · 두려움 %llu · 후퇴 %llu(반주문 %llu) · 도망 길 못냄 %llu · 지키기공격 %llu · 거리두기 %llu · "
                    "답답함 %llu(옆걸음 %llu · 기다림 %llu) · 먹기 %llu\n",
                    srv.brainStat(BS::Runs), srv.brainStat(BS::Confused), srv.brainStat(BS::Fear), srv.brainStat(BS::Retreat),
                    srv.brainStat(BS::RetreatAnti), srv.brainStat(BS::FleeNoPath), srv.brainStat(BS::AggroAtWill), srv.brainStat(BS::KeepDistance),
                    srv.brainStat(BS::Frustrated), srv.brainStat(BS::Sidestep), srv.brainStat(BS::WaitStuck), srv.brainStat(BS::Eat));
        using AS = nox::AnimStat;    // ★ 09-29 B5 — 애니 칸·걸음 소리
        srv.Log("[몬스터애니] 표없음 %llu · 칸0 %llu · 넘김 %llu(되풀이 %llu · 끝 %llu) · 걸음 부름 가기 %llu/도망 %llu/걷기 %llu · 칸맞음 %llu · 소리 %llu(번호없음 %llu)\n",
                    srv.animStat(AS::NoTable), srv.animStat(AS::NoFrames), srv.animStat(AS::Advance), srv.animStat(AS::Wrap), srv.animStat(AS::Done),
                    srv.animStat(AS::StepMoveTo), srv.animStat(AS::StepFlee), srv.animStat(AS::StepWalk), srv.animStat(AS::StepHit),
                    srv.monsterSnd(17, true), srv.monsterSnd(17, false));
        {   // ★ 10-01 — 갱신 항목에 실은 애니 번호별(보는 사람 × 틱) · 1 근접 · 3 쏘기 · 7 주문 · 8 가만 · 9 죽어감 · 10 죽음 · 12 걷기 · 13 달리기
            unsigned long long all = 0;
            std::string row;
            for (int i = 0; i < 16; ++i) {
                all += srv.monsterAnimSent(i);
                if (srv.monsterAnimSent(i)) row += " " + std::to_string(i) + ":" + std::to_string(srv.monsterAnimSent(i));
            }
            srv.Log("[몬스터애니보냄] 항목 %llu ·%s\n", all, row.empty() ? " (없음)" : row.c_str());
        }
        // ★ 10-01 — 주인에게 보낸 소유 알림(소환 끝 = 6c·dd 한 쌍 · 소환수 죽음 = 6d)
        srv.Log("[소유알림] 6c 얻음 %llu · dd 총체력 %llu · 6d 잃음 %llu\n",
                    srv.summonOwnSent(0), srv.summonOwnSent(1), srv.summonOwnSent(2));
        // ★ 10-01 — 소환 효과(부르기 시작 = 7e · 세우기 전에 끊김 = 7f) · 소환 한 번에 하나씩(보는 사람 수와 무관)
        srv.Log("[소환효과] 7e 시작 %llu · 7f 끊김 %llu\n", srv.summonFxSent(0), srv.summonFxSent(1));
        // ★ 10-01 — 클라 대체 무기 알림 `e0`(비움 · 가방에 없음 · 맞음 · 직업·힘 안 맞아 `e0 0 01` 되돌림)
        srv.Log("[대체무기] 비움 %llu · 없음 %llu · 맞음 %llu · 되돌림 %llu\n", srv.secondaryWeaponStat(0),
                    srv.secondaryWeaponStat(1), srv.secondaryWeaponStat(2), srv.secondaryWeaponStat(3));
        // ★ 10-01 — 시체(죽어도 안 지움 → 썩어 지움 · DESTROY_WHEN_DEAD 는 31 에서 바로)
        srv.Log("[시체] 죽음 %llu · 썩어 지움 %llu · 즉시 지움 %llu · 동시 시체 최대 %llu\n",
                    srv.corpseStat(0), srv.corpseStat(1), srv.corpseStat(2), srv.corpseStat(3));
        using HS = nox::HearStat;    // ★ 09-29 B6 — 소리 듣기
        srv.Log("[소리듣기] 기록 %llu · 귀막힘 %llu · 아군무시 %llu · 작음 %llu · 벽반감 %llu(작아짐 %llu) · 들음 %llu · 가봄 %llu · 바라봄 %llu · 못감 %llu · 조건59 %llu · 조건69 %llu\n",
                    srv.hearStat(HS::Recorded), srv.hearStat(HS::Deaf), srv.hearStat(HS::NotListen), srv.hearStat(HS::Quiet), srv.hearStat(HS::WallHalved),
                    srv.hearStat(HS::WallQuiet), srv.hearStat(HS::Heard), srv.hearStat(HS::ReactGo), srv.hearStat(HS::ReactLook), srv.hearStat(HS::ReactSkip),
                    srv.hearStat(HS::Cond59), srv.hearStat(HS::Cond69));
        using CS = nox::CastStat;    // ★ 09-29 B7 — 몬스터 주문
        srv.Log("[몬스터주문] 고름 공격 %llu · 달아남 %llu · 제몸 %llu · 되받기 %llu · 가까워 점멸 %llu · 시전소리 %llu · 시전 %llu(반주문 %llu) · 받음 %llu · 안옮김 %llu · 빔맞힘 %llu · 움직여끊김 %llu\n",
                    srv.castStat(CS::PickOffense), srv.castStat(CS::PickEscape), srv.castStat(CS::PickBuff), srv.castStat(CS::PickReact), srv.castStat(CS::BlinkNear),
                    srv.castStat(CS::InitSound), srv.castStat(CS::Cast), srv.castStat(CS::CastAnti), srv.castStat(CS::Accepted), srv.castStat(CS::NotPorted),
                    srv.castStat(CS::BeamHit), srv.castStat(CS::BeamMoved));
    }
    // ★ 09-29 Z1-A — 벽 피해 갈래: 벽없음(홀수칸·문·부서짐 포함) · 안부서지는벽(소리만) · 깎음 · 부서짐 · 부서짐 알림(받는 사람별)
    srv.Log("[벽피해] 벽없음 %llu · 안부서지는벽 %llu · 깎음 %llu · 부서짐 %llu · 알림 %llu\n",
                srv.wallGate(0), srv.wallGate(1), srv.wallGate(2), srv.wallGate(3), srv.wallGate(4));
    // ★ 09-28 — 버릴 때 사라짐 시계(걸림 · 협동·퀘스트 · 예외 셋) · 시계가 다 돼 지운 수
    {
        using DG = nox::DecayGate;
        srv.Log("[버린물건사라짐] 10초 %llu · 25초 %llu · 안 걺(협동·퀘스트 %llu · RESPAWN %llu · FLAG %llu · Glyph %llu · 함수안옮김 %llu) · 지움 %llu(33 보냄 %llu)\n",
                    srv.decayGate(DG::Armed), srv.decayGate(DG::Armed25), srv.decayGate(DG::CoopOrQuest), srv.decayGate(DG::Respawn),
                    srv.decayGate(DG::FlagClass), srv.decayGate(DG::Glyph), srv.decayGate(DG::NotPorted), srv.decayExpired(),
                    srv.decayShadowSent());
    }
    // ★ 09-28 B4b — 죽을 때 가방 떨어뜨리기 · 부활 때 기본 장비
    srv.Log("[죽음가방] 남긴 평상복 %llu · 둘레에 %llu · 몸 자리에 %llu · [부활장비] 지움 %llu · 줌 %llu · 틀없음 %llu · 협동퀘스트안옮김 %llu"
                " · [입은것겉모습] 보냄 %llu · 비트몰라뺌 %llu\n",
                srv.deathDropKept(), srv.deathDropSpiral(), srv.deathDropAtBody(), srv.respawnGearDeleted(), srv.respawnGearGiven(),
                srv.respawnGearNoTemplate(), srv.respawnGearNotPorted(), srv.wornLookSent(), srv.wornLookSkipped());
    // ★ 09-23 nine-defects 4-d4 — 발소리(달리기 RUN 2·8칸 · 걷기 WALK 3·9칸 × 발밑 바닥 종류). 「바닥없음」 = 칸 찾기 −1·정의 밖
    using FK = nox::FootKind;
    srv.Log("[발소리] 달리기 %llu = {없음 %llu·나무 %llu·흙 %llu·물 %llu·눈 %llu·진흙 %llu·돌 %llu·바닥없음 %llu}"
                " · 걷기 %llu = {없음 %llu·나무 %llu·흙 %llu·물 %llu·눈 %llu·진흙 %llu·돌 %llu·바닥없음 %llu} · 살금살금으로 안냄 %llu · 달릴때힘쓰는소리 %llu\n",
                srv.FootRunSum(), srv.footRun(FK::Silent), srv.footRun(FK::Wood), srv.footRun(FK::Dirt), srv.footRun(FK::Water),
                srv.footRun(FK::Snow), srv.footRun(FK::Mud), srv.footRun(FK::Stone), srv.footRun(FK::NoTile),
                srv.FootWalkSum(), srv.footWalk(FK::Silent), srv.footWalk(FK::Wood), srv.footWalk(FK::Dirt), srv.footWalk(FK::Water),
                srv.footWalk(FK::Snow), srv.footWalk(FK::Mud), srv.footWalk(FK::Stone), srv.footWalk(FK::NoTile),
                srv.footTreadSilent(), srv.exertionLight());
    // ★ 09-24 — 맞은 사람의 「윽」(`object_update.go:80~107`)
    srv.Log("[신음] %llu = {가벼움 %llu·중간 %llu·심함 %llu·독 %llu}\n", srv.HurtVoiceSum(),
                srv.hurtVoice(nox::HurtVoice::Light), srv.hurtVoice(nox::HurtVoice::Medium), srv.hurtVoice(nox::HurtVoice::Heavy),
                srv.hurtVoice(nox::HurtVoice::Poison));
}

// 능력·작살 계수기가 바뀌었나를 보는 지문 — `--quiet` 에서도 **바뀐 때만** 위 두 줄을 찍으려고 쓴다
unsigned long long AbilityFingerprint(const nox::Server& srv) {
    unsigned long long n = srv.abilCalls() + srv.harpoonShot() + srv.treadBlockedRun() + srv.harpoonAimFound() +
                           srv.contactDamageHits() + srv.warcryShouts() + srv.abilValOff() + srv.berserkChargeTicks() +
                           srv.berserkCollideCalls() + srv.jumpCodes() + srv.jumpLanded() + srv.melees() +
                           srv.FootRunSum() + srv.FootWalkSum() + srv.footTreadSilent() +   // ★ 4-d4
                           srv.HurtVoiceSum() +                                                // ★ 09-24
                           srv.monsterMeleeHitSnd(true) + srv.monsterMeleeHitSnd(false) +      // ★ 09-24 H1
                           srv.WarcryPeerSum() + srv.enchantSentAntiMagic() +                  // ★ 09-24 H3
                           srv.deathPass(true) + srv.deathPass(false) + srv.lavaHurts() + srv.lavaHurtsPet() +   // ★ 09-26 M2·M3 · 09-27 S2
                           srv.monsterSnd(5, true) + srv.monsterSnd(9, true) + srv.monsterSnd(10, true) + srv.monsterSnd(14, true) + srv.monsterSnd(16, true);   // ★ M4a·M4b
    for (uint8_t a = 1; a < nox::kAbilityCount; ++a)
        for (size_t e = 0; e < nox::kAbilEndCount; ++e) n += srv.abilEnded(a, static_cast<nox::AbilEnd>(e));
    for (size_t t = 0; t < nox::kHarpoonTouchCount; ++t) n += srv.harpoonTouch(static_cast<nox::HarpoonTouch>(t));
    for (size_t h = 0; h < nox::kHarpoonEndCount; ++h) n += srv.harpoonEnd(static_cast<nox::HarpoonEnd>(h));
    return n;
}

// ★ 10-12 플랜 B 작업 7c — 방마다 main 이 드는 것(판 차리기 · 진단). 방(`CRoom`)은 라이브러리라 실행 인자 묶음과
//   이 파일의 진단 도우미를 모른다 — 그래서 이 상태는 실행 파일 쪽에 방 번호로 둔다(옛 `main` 의 「루프가 들고 다니는 상태」)
struct RoomRun {
    const RoomSettings* opt = nullptr;      // 이 방의 설정(실행 인자로 차린 방 = 실행 인자의 방 설정 · ★ 10-14 작업 9a)
    WorldExtras extras;                     // 월드 차리기가 남긴 것(맵 깃발 자리 · 서버 찾기 답의 맵 이름)
    int64_t bot0Sid = -1;                   // `[성장]` 줄의 봇 세션 — 통신 층 값이라 스레드를 띄우기 전에 읽어 둔다
    bool modeStarted = false;
    unsigned long long tracePetsInWall = 0;   // ③ 크리처가 **벽 칸 안**에 있던 표본 수
    unsigned long long tracePetsSamples = 0;  // 그 분모 — 0 옆에 늘 지나간 양을 같이 읽는다
    unsigned long long lastAbilFp = 0;      // 조용한 주행의 능력 갈래 지문(옛 루프 안 `static`)
    unsigned long long sendSeen = 0;        // 틱 앞에 본 이 방의 보내기 요청 누계 — 이 틱의 `made` = 틱 뒤 − 이것
};

// ★ 10-12 플랜 B 작업 7c — 판 차리기(옛 `main` 게임 루프 안 블록을 그대로 옮김). 방마다 부른다
void StartModeIfReady(nox::Server& srv, RoomRun& rr) {
    const RoomSettings& opt = *rr.opt;
    const WorldExtras& extras = rr.extras;
    bool& modeStarted = rr.modeStarted;
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
    const int needPlayers = nox::ModeNeedPlayers(opt.mode);
    if (!modeStarted && opt.mode != "arena" && srv.inGameCount() >= needPlayers) {
        modeStarted = true;
        // ★ 09-19 — 전에는 `spawnX() + 20`(두 사람 사이)였다. 사람이 그 고정값에 서던 때의 셈이라,
        //   첫자리가 맵 PlayerStart 로 바뀐 뒤로는 **사람들의 실제 가운데**를 쓴다
        int mx = 0, my = 0;
        srv.PlayersCenter(mx, my);
        srv.Log("[모드] 판 자리 = 사람들 가운데 %d,%d\n", mx, my);
        // ★ 갈래는 `ModeRules.h` 가 **값으로** 정한다. 여기 `if/else` 로 두었을 때
        //   `else` 하나가 여섯 모드를 삼켰고 시험이 그것을 못 봤다(조각 E·G).
        const nox::ModeSetup setup = nox::ModeStartKind(opt.mode);
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
            if (extras.mapFlagPos.size() >= 2) {
                srv.StartCtf(extras.mapFlagPos[0].first, extras.mapFlagPos[0].second,
                             extras.mapFlagPos[1].first, extras.mapFlagPos[1].second);
                srv.Log("[모드] 깃발 자리 = **맵에 그려진 것**을 씀 (%d,%d)·(%d,%d)\n",
                            extras.mapFlagPos[0].first, extras.mapFlagPos[0].second,
                            extras.mapFlagPos[1].first, extras.mapFlagPos[1].second);
            } else {
            const int redX  = placeFlag(mx - opt.ctfSpan, my, mx);
            const int blueX = placeFlag(mx + opt.ctfSpan, my, mx);
            srv.StartCtf(redX, my, blueX, my);
            srv.Log("[모드] 깃발 — (%d,%d)·(%d,%d), 득점 한도 %u%s\n",
                        redX, my, blueX, my, (unsigned)nox::kCaptureLimit,
                        (redX != mx - opt.ctfSpan || blueX != mx + opt.ctfSpan)
                            ? "  ← 벽을 피해 당김" : "");
            }
        } else if (setup == nox::ModeSetup::FlagBall) {
            // 공은 가운데, 골대는 깃발과 같은 간격으로 양옆
            srv.StartFlagBall(mx, my, mx - opt.ctfSpan, my, mx + opt.ctfSpan, my);
            srv.Log("[모드] 플래그볼 — 공 (%d,%d) · 골대 (%d,%d)·(%d,%d)\n",
                        mx, my, mx - opt.ctfSpan, my, mx + opt.ctfSpan, my);
        } else if (setup == nox::ModeSetup::Quest) {
            srv.StartQuest(opt.questCount, mx, my, opt.questSpread);
            // ⚠ **`questCount` 는 이제 안 쓰는 값이다** — 웨이브 마릿수는 스테이지 표가
            //   정한다(`StartQuest` 주석 참조). 전에는 여기서 그 값을 「몬스터 N마리」로
            //   찍어 **실제와 다른 수를 보여 줬다**(설정 4 · 실제 2).
            //   ★ 실제로 몇 마리가 섰는지는 바로 위 `[스테이지]` 줄이 말한다.
            srv.Log("[모드] 퀘스트 — (%d,%d) 반경 %.0f · 마릿수는 위 [스테이지] 줄 참조\n",
                        mx, my, opt.questSpread);
        } else {
            // ★★ 여기가 비어 있어야 하는 모드들이다 — `elimination`·`kotr`·`chat`.
            //   전에는 이 갈래가 없어서 **`else` 가 전부 `StartQuest` 로 떨어졌고**,
            //   `--mode elimination` 을 켜면 퀘스트 몬스터 두 마리가 나와 사람을 때렸다
            //   (조각 E 실측 — `[스테이지] 1/5` · 크리처 2마리 · 피해 4).
            //   판 규칙은 게임플래그로 이미 켜져 있고, 여기서 더 놓을 것이 없다.
            srv.Log("[모드] %s — 판에 따로 놓는 것 없음(규칙은 gameFlags 로 이미 켜짐)\n",
                        opt.mode.c_str());
        }
        std::fflush(stdout);
    }
}

// ★ 10-12 플랜 B 작업 7c — 방 하나의 틱 뒤 진단(옛 `main` 게임 루프 안 블록을 옮김). `made` = 이 틱에 이 방이 낸 보내기 요청 수.
//   통신 층 · 스레드 지표 줄(`[순번·스트림]` · `[다시보냄]` · `[세션]` · `[스레드]`)과 옛 `[틱]` · `[버림]`의 통신 층 칸은 `main` 의 프로세스 몫 줄로 갔다
void RoomTickDiagnostics(nox::Server& srv, RoomRun& rr, size_t made) {
    const RoomSettings& opt = *rr.opt;
    unsigned long long& tracePetsInWall = rr.tracePetsInWall;
    unsigned long long& tracePetsSamples = rr.tracePetsSamples;
    const int64_t bot0Sid = rr.bot0Sid;
    // ★ 09-20 6-j(3) — 크리처 추적. **조용한 주행에서도 찍는다**(판정에 쓰는 줄이라)
    if (opt.tracePets > 0 && (srv.frame() % static_cast<uint32_t>(opt.tracePets)) == 0) {
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
            srv.Log("[소환수길] 틱 %u · %zu번 (%d,%d) 맨위행동 %d 길목 %u"
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
    if (opt.quiet && made > 0 && (srv.frame() % 300) == 0) {
        unsigned long long& lastAbilFp = rr.lastAbilFp;   // ★ 10-12 작업 7c — 옛 `static`(방마다로)
        const unsigned long long fp = AbilityFingerprint(srv);
        if (fp != lastAbilFp) {
            PrintAbilityBreakdown(srv);
            std::fflush(stdout);
            lastAbilFp = fp;
        }
    }
    // 매 틱 찍으면 화면이 흐르니 1초에 한 번만.
    if (!opt.quiet && made > 0 && (srv.frame() % 30) == 0) {
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
        if (opt.mode == "ctf") {
            std::snprintf(modeInfo, sizeof(modeInfo), " · 깃발 %zu개 득점 %u:%u",
                          srv.FlagCount(), (unsigned)srv.CapturesOf(1),
                          (unsigned)srv.CapturesOf(2));
        } else if (opt.mode == "quest") {
            std::snprintf(modeInfo, sizeof(modeInfo),
                          " · 스테이지 %d · 이 웨이브 남음 %d(잡음 %d)",
                          srv.QuestStage() + 1, srv.QuestLeft(), srv.QuestKilled());
        }
        srv.Log("[틱] snapIn=%d 루프본것=%zu/%zu · 프레임 %u · 조각 %zu개 · 사람 %zu(갱신에 %zu) · "
                    "걸음 %llu · 근접 %llu(클릭 %llu) · "
                    "주문 %llu(마나막힘 %llu) · 나는것 %zu(만든 %llu · 사라짐 %llu · 갱신에 %llu) · 피해 %llu · 별칭받음 %llu%s"
                    " · 악수(수락 %llu·맵지정 %llu·준비 %llu·무적건 %llu)"
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
                    " · 판끝 %s\n",
                    srv.firstSnapshotIn(), srv.lastLoopSeen(), srv.lastSessions(),
                    srv.frame(), made, srv.inGameCount(), srv.lastUpdateObjects(),
                    srv.moves(), srv.melees(), srv.code6Seen(),
                    nSpell, srv.spellsNoMana(), srv.missiles(), srv.missilesMade(), srv.missilesGone(), srv.missileEntriesSent(), srv.hurts(), srv.aliasHeard(), modeInfo,
                    srv.acceptHeard(), srv.useMapSent(), srv.readyHeard(), srv.spawnInvulnApplied(),
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
                    srv.RoundOver() ? "예" : "아니오");
        PrintAbilityBreakdown(srv);                  // ★ 09-23 0-c — 위 합계의 능력별·작살 갈래
        // ★ 09-23 0-c — **1초에 한 번 비운다.** 하네스(`crashrun.ps1`)는 stdout 을 파일로 돌리고
        //   끝에 서버를 강제로 끈다 — C 런타임이 4KB 씩 모아 쓰므로 그때 남은 것이 통째로 사라졌다
        //   (nd0c 판: 30초 판에 파일이 `[받음 1]` 에서 끝나고 `[틱]` 줄 0). 이 블록의 나머지 줄은 다음 초에 나간다
        std::fflush(stdout);
        // ★ 09-19 진단 ⑬ — 화살 목록과 **실은 프레임의 분포**.
        //   화면 캡처의 기울기와 1:1 로 맞대는 자리다. 분포가 한 칸뿐이면
        //   「모든 화살이 같은 각도로 그려진다」가 서버 쪽에서 이미 정해진 것이다.
        if (opt.dumpArrows) {
            const auto& ms = srv.missileList();
            std::map<unsigned, unsigned> byFrame;
            for (const auto& mo : ms) ++byFrame[static_cast<unsigned>(mo.dir1 >> 3)];
            // ★ 10-09 — 여러 번에 나눠 만드는 줄은 다 만든 뒤 한 번에 찍는다(아래 `[소리]` · `[성장]` · `[피해]` ·
            //   `[발길질 차례]` · `[버림]` 도 같다). 나눠 찍으면 네트워크 스레드의 `[받음]` 이 그 사이에 낀다
            //   (10-09 실측 — `[버림] 조각 6400개 —[받음 6410] …`)
            char b[128];
            std::snprintf(b, sizeof(b), "[화살목록] %zu개 · 본디프레임 분포", ms.size());
            std::string line = b;
            for (const auto& kv : byFrame) {
                std::snprintf(b, sizeof(b), " %u:%u", kv.first, kv.second);
                line += b;
            }
            size_t shown = 0;
            for (const auto& mo : ms) {
                if (shown++ >= 8) break;
                // ⚠ 이 값은 **원래 방향에서 나온 프레임**이다. `--arrow-frame` 으로
                //   못 박으면 와이어에는 그 값이 실리지만 여기 숫자는 안 바뀐다
                //   (덮어쓰기는 보낼 때 `FillEntry` 호출부에서 한다).
                std::snprintf(b, sizeof(b), " | %u (%d,%d) dir1=%u→본디프레임 %u",
                              static_cast<unsigned>(mo.netCode),
                              static_cast<int>(mo.posX), static_cast<int>(mo.posY),
                              static_cast<unsigned>(mo.dir1),
                              static_cast<unsigned>(mo.dir1 >> 3));
                line += b;
            }
            srv.Log("%s\n", line.c_str());
        }

        // ★ 자리 한 줄 — 하네스가 이걸 읽어 걸어갈 각을 잰다(Server.h PositionsLine 참조).
        srv.Log("%s\n", srv.PositionsLine().c_str());

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
            srv.Log("%s\n", sp.c_str());
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
        srv.Log("[물건] 줍기요청 %llu(성공 %llu·멂 %llu) · 쓰기 %llu(모르는갈래 %llu)"
                    " · 입기 %llu · 벗기 %llu · 되돌림 %llu"
                    " · 거절(한도 %llu·무거움 %llu·개수 %llu·직업 %llu)"
                    " · 즉시마심 %llu · 금 %llu · 기본장비(실음 %llu·틀없음 %llu) · 표없는무기판정 %llu\n",
                    srv.tryGets(), srv.picked(), srv.pickTooFar(),
                    srv.used(), srv.useUnknown(),
                    srv.equipped(), srv.dequipped(), srv.dropped(),
                    srv.pickNoCap(), srv.pickTooHeavy(), srv.pickTooMany(), srv.pickClassFail(),
                    srv.drankOnPickup(), srv.goldPicked(),
                    srv.gearGiven(true), srv.gearGiven(false), srv.weaponNoStats());
        // ★ 09-27 B3 — 무기 들기 관문별(정답지 `53A420` 순서)
        srv.Log("[무기들기] 무기아님 %llu · 이미 %llu · 함성광전사 %llu · 직업 %llu · 힘 %llu · 활없음 %llu · 내려놓기실패 %llu · 듦 %llu\n",
                    srv.equipGate(nox::EquipGate::NotWeapon), srv.equipGate(nox::EquipGate::Already),
                    srv.equipGate(nox::EquipGate::Ability), srv.equipGate(nox::EquipGate::ClassFail),
                    srv.equipGate(nox::EquipGate::StrengthFail), srv.equipGate(nox::EquipGate::BowNotFound),
                    srv.equipGate(nox::EquipGate::DequipFail), srv.equipGate(nox::EquipGate::Equipped));
        // ★ 09-27 B4 — 무기 내려놓기(`53A140`) 나간 길별 + 공격 중 석궁 벗기 요청 버림
        srv.Log("[무기내려놓기] 무기아님 %llu · 든것아님 %llu · 화살통 %llu · 내려놓음 %llu · 공격중석궁 %llu"
                    " · (B5) 들때충전알림 %llu · 두손무기가벗긴방패 %llu · 내려놓으며끊은주문 %llu · (09-28) 방패가내려놓게한두손무기 %llu\n",
                    srv.dequipGate(nox::DequipGate::NotWeapon), srv.dequipGate(nox::DequipGate::NotHeld),
                    srv.dequipGate(nox::DequipGate::QuiverOff), srv.dequipGate(nox::DequipGate::Dequipped),
                    srv.dequipGate(nox::DequipGate::CrossbowAttacking),
                    srv.wandChargesOnEquip(), srv.twoHandShieldOff(), srv.wandSpellCut(), srv.shieldDropsTwoHand());
        // ★ 09-29 — 활·석궁 쏘기 · 사람 화살 닿음 · 무기·화살통 줍기
        srv.Log("[활] 쏨 %llu · 앞막힘 %llu · 재장전중 %llu · 화살통바꿔듦 %llu · 화살통없음 %llu · 쏜뒤자세끝 %llu"
                    " · [화살닿음] 쏜이 %llu · 죽은것 %llu · 피해 %llu · 안들어감 %llu · 쇠뇌살뚫음 %llu · 장애물 %llu · 벽 %llu · 수명 %llu"
                    " · [무기줍기] 직업 %llu · 화살통합침 %llu · 화살통저절로 %llu · 충전알림 %llu · 소리 %llu · 맵화살통 %llu\n",
                    srv.bowShot(nox::BowShot::Shot), srv.bowShot(nox::BowShot::Blocked), srv.bowShot(nox::BowShot::Reloading),
                    srv.bowShot(nox::BowShot::Reloaded), srv.bowShot(nox::BowShot::NoQuiver), srv.bowPoseEnded(),
                    srv.arrowHit(nox::ArrowHit::Owner), srv.arrowHit(nox::ArrowHit::Dead), srv.arrowHit(nox::ArrowHit::Hurt),
                    srv.arrowHit(nox::ArrowHit::NoHurt), srv.arrowHit(nox::ArrowHit::BoltPass), srv.arrowHit(nox::ArrowHit::Obstacle),
                    srv.arrowHit(nox::ArrowHit::Wall), srv.arrowHit(nox::ArrowHit::Age),
                    srv.weaponPick(nox::WeaponPick::ClassFail), srv.weaponPick(nox::WeaponPick::AmmoMerged),
                    srv.weaponPick(nox::WeaponPick::QuiverAutoEquip), srv.weaponPick(nox::WeaponPick::ChargesReport),
                    srv.weaponPick(nox::WeaponPick::Sound), srv.quiverFromMap());
        // ★ 09-28 — 버리기(`TRY_DROP` → `drop_4ED810`) 나간 길별
        srv.Log("[버리기] 관전 %llu · 가방에없음 %llu · 벽 %llu · 버림 %llu"
                    " · [줍고입음] 무기 %llu · 갑옷 %llu · 같은부위라안입음 %llu · 두손무기라방패안입음 %llu · 부위모름 %llu · [2d차림실음] %llu"
                    " · [2d정보] 녹음그대로 %llu · 진짜로덮음 %llu · 조립 %llu\n",
                    srv.dropGate(nox::DropGate::Observer), srv.dropGate(nox::DropGate::NotInBag),
                    srv.dropGate(nox::DropGate::Wall), srv.dropGate(nox::DropGate::Dropped),
                    srv.autoEquip(0), srv.autoEquip(1), srv.autoEquip(2), srv.autoEquip(3), srv.autoEquip(4),
                    srv.newPlayerGearBits(), srv.newPlayerInfo(0), srv.newPlayerInfo(1), srv.newPlayerInfo(2));
        // ★★ 조각 D — **여기서부터 넉 줄이 「눈이 없던 자리」다.**
        //   계수기 41개 중 24개가 한 번도 안 찍히고 있었다(조각 C). 아래가 그 24개를
        //   묶음별로 묶어 낸 것이고, 각 줄은 **하나를 물으면 하나가 답하도록** 갈라 놨다.
        //   ⚠ 소리 셋은 합치지 말 것 — 「안 불렀다」·「불렀는데 표가 비어 버렸다」·
        //     「담겼는데 거리에서 잘렸다」가 한 숫자면 묶음 10 이 통째로 판정 불능이 된다.
        {   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다(위 `[화살목록]` 주석)
            char b[256];
            std::snprintf(b, sizeof(b), "[소리] 담김 %llu(표없어버림 %llu) · 나감 %llu · 음악 %llu",
                          srv.soundsQueued(), srv.soundsDropped(), srv.soundsSent(), srv.musicSent());
            std::string line = b;
            // ★ **버린 소리는 번호까지 찍는다.** 수만 알면 「정답지대로 버린 것」(소리조각 0)과
            //   「우리가 엉뚱한 번호를 부른 것」이 안 갈린다. 조각 E 에서 진단 판 하나만
            //   「표없어버림 1」이 떴는데 **무엇인지 알 길이 없었다**.
            if (!srv.soundsDroppedIds().empty()) {
                line += " — 버린 번호";
                for (const auto& kv : srv.soundsDroppedIds()) {
                    std::snprintf(b, sizeof(b), " %u(%s)x%llu", (unsigned)kv.first, nox::SoundName(kv.first), kv.second);
                    line += b;
                }
            }
            srv.Log("%s\n", line.c_str());
        }
        // ★ **성장 한 줄** — 진단 스위치(`--debug-xp`·`--debug-give-spell`)가 먹었는지
        //   보는 유일한 자리다. 없을 때는 판 G 가 통째로 판정 불능이었다(조각 C 13·14번).
        if (!opt.bots.empty()) {
            const int64_t b0 = bot0Sid;   // ★ 10-08 조각 4 — 세션 번호(10-09 조각 5d-2b — 스레드를 띄우기 전에 읽어 둔 것)
            char b[256];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다(위 `[화살목록]` 주석)
            std::snprintf(b, sizeof(b), "[성장] 레벨 %d · 경험치 %.0f",
                          srv.DebugLevel(b0), (double)srv.DebugExperience(b0));
            std::string line = b;
            if (opt.debugSpellId > 0) {
                std::snprintf(b, sizeof(b), " · 준 주문 %d(%s) 레벨 %d", opt.debugSpellId,
                              nox::SpellName(static_cast<uint8_t>(opt.debugSpellId)),
                              srv.DebugSpellLvl(b0, static_cast<uint8_t>(opt.debugSpellId)));
                line += b;
            }
            srv.Log("%s\n", line.c_str());
        }
        // ★ 조각 I — 체력은 **세 길**로 간다. 「41 남의 체력」만 찍던 때는
        //   `피해 14 · 체력알림 1` 이 「안 보낸다」인지 「딴 길로 간다」인지 안 갈렸다.
        srv.Log("[싸움] 겨눔 %llu · 부딪힘 %llu(미룸 %llu) · 무적막힘 %llu"
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
            char b[256];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다(위 `[화살목록]` 주석)
            std::snprintf(b, sizeof(b), "[피해] 주인없음갈래(둘다몬스터 %llu·뿌리가몬스터아님 %llu) 막힌 곳 —",
                          srv.wildBothMonster(), srv.wildRootNotMonster());
            std::string line = b;
            for (const auto& kv : srv.damageStops()) {
                std::snprintf(b, sizeof(b), " %llu", kv.second);
                line += ' ';
                line += kv.first;
                line += b;
            }
            srv.Log("%s\n", line.c_str());
        }
        srv.Log("[크리처] %zu마리(길 든 것 %zu) · 길 계산됨 %s(상태 %d) · 겨눈 덫 %zu"
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
            char b[256];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다(위 `[화살목록]` 주석)
            std::snprintf(b, sizeof(b), "[발길질 차례] 모두 %zu회 중 앞 %zu회 —", srv.kickSeqTotal(), srv.kickSeqCount());
            std::string line = b;
            for (size_t k = 0; k < srv.kickSeqCount(); ++k) {
                const auto& q = srv.kickSeq(k);
                std::snprintf(b, sizeof(b), " %zu)%s 깊이%.1f 속도%.1f 프레임%llu", k + 1,
                              q.kind == 3 ? "벽" : "물건", q.depth, q.speedBefore, q.frame);
                line += b;
            }
            srv.Log("%s\n", line.c_str());
        }
        srv.Log("[판끝멈춤] 멈춘 틱 %llu\n", srv.roundFrozenTicks());   // ★ 10-01 R1
        srv.Log("[판] 판 수 %llu · 깃발알림 %llu · 말 %llu · 시계요청 %llu"
                    " · FX %llu · 겉모습 %llu(모르는 것 %llu)"
                    " · 문 %llu(되맞춤 %llu·가장가까이 %.0f) · 오벨리스크 %llu(옆인데줄것없음 %llu) · **밟는판(눌림 %llu·뗌 %llu)**"
                    " · **비밀벽(열림 %llu·닫힘 %llu)**"
                    " · 자기주문 %llu · 미룬주문효과 %llu\n",
                    srv.roundsPlayed(), srv.flagStatusSent(), srv.chats(), srv.timerAsks(),
                    srv.fxSent(), srv.appearanceSent(), srv.appearanceUnknown(),
                    srv.doorsOpened(), srv.doorResync(), srv.doorNearestDist(), srv.obeliskMana(), srv.obeliskNearIdle(), srv.plateOn(), srv.plateOff(),
                    srv.wallOpenSent(), srv.wallCloseSent(),
                    srv.spellsOnSelf(), srv.spellEffectDeferred());
        // ★ 10-08 조각 3 — 접속 제어 조각의 버림은 통신 층이 센다. 둘을 합쳐 찍는다.
        //   ★ 10-12 작업 7c — 이제 방 줄은 게임 몫만 찍고, 통신 층 몫은 프로세스 줄 `[버림·통신]` 으로 갈랐다
        if (!srv.stoppedByOp().empty()) {
            char b[128];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다(위 `[화살목록]` 주석)
            std::snprintf(b, sizeof(b), "[버림] 조각 %llu개 —", srv.stopped());
            std::string line = b;
            for (const auto* byOp : { &srv.stoppedByOp() })
            for (const auto& kv : *byOp) {
                std::snprintf(b, sizeof(b), " %02x(%s)x%llu", kv.first,
                              nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
                line += b;
            }
            srv.Log("%s\n", line.c_str());
        }
        // ★ 09-29 ③ — 게임 패킷 안의 0x00(0 채움)에서 읽기를 끝낸 수(전에는 하나하나 0x04 로 답했다)
        srv.Log("[0채움] 게임 패킷 안 0x00 에서 읽기 끝 %llu\n", srv.connectTailStops());
        std::fflush(stdout);
    }
}

}  // namespace

// ★ 10-09 조각 5d-2c — 닫기 신호. 콘솔 Ctrl+C · Ctrl+Break · 창 닫기 · 로그오프 · 끄기가 오면 게임 루프가 다음 바퀴에 빠져나가
//   정상 종료한다. 창 닫기 등은 이 함수가 돌아오는 순간 프로세스가 끝나므로 닫기 절차가 끝날 때까지(최대 4초) 기다린다.
static std::atomic<bool> g_stopRequested{ false };
static HANDLE g_shutdownDone = nullptr;
static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    g_stopRequested.store(true);
    if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_shutdownDone)
        WaitForSingleObject(g_shutdownDone, 4000);
    return TRUE;
}

int main(int argc, char** argv) {
    // 한글 출력이 CP949 로 오독되지 않게. printf 는 /utf-8 로 컴파일된 UTF-8 을 낸다.
    SetConsoleOutputCP(CP_UTF8);

    // ★ 10-08 — 실행 인자는 `ServerOptions`(`WorldBuild.h`)로 옮겼다. 기본값·주석도 거기에 있다.
    //   세계 차리기(`BuildWorld`)와 아래 루프가 같이 읽는다.
    ServerOptions opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") {
            return SelfTest();
        } else if (a == "--quiet") {
            opt.quiet = true;   // ★ 10-14 작업 9a — 프로세스 줄과 방 진단 줄 둘 다
            opt.room.quiet = true;
        } else if (a == "--config" && i + 1 < argc) {
            opt.configPath = argv[++i];
        } else if (a == "--no-join-token") {
            opt.noJoinToken = true;
        } else if (a == "--port" && i + 1 < argc) {
            opt.port = std::atoi(argv[++i]);
        } else if (a == "--name" && i + 1 < argc) {
            opt.name = argv[++i];
        } else if (a == "--prelude") {
            opt.room.prelude = true;
        } else if (a == "--no-update") {
            // ★ 진단용 — 상태 갱신을 끈다. Server.h 의 SetSendUpdates 주석 참조.
            opt.room.noUpdate = true;
        } else if (a == "--raw-snapshot") {
            opt.room.rawSnapshot = true;
        } else if (a == "--peer-ops" && i + 1 < argc) {
            opt.room.peerOps = argv[++i];
        } else if (a == "--intro-every-tick") {
            opt.room.introEveryTick = true;
        } else if (a == "--alias-intro") {
            // 진단용 — 이미 소개한 사람을 별칭으로 보낸다. 지금은 아무도 안 그려진다.
            opt.room.introEveryTick = false;
        } else if (a == "--peer-type" && i + 1 < argc) {
            opt.room.peerType = std::atoi(argv[++i]);
        } else if (a == "--peer-snapshot") {
            opt.room.peerSnapshot = true;
        } else if (a == "--peer-first") {
            opt.room.peerFirst = true;
        } else if (a == "--peer-pose" && i + 1 < argc) {
            opt.room.peerPose = (int)std::strtol(argv[++i], nullptr, 16);
        } else if (a == "--spawn-spread" && i + 1 < argc) {
            opt.room.spawnSpread = std::atoi(argv[++i]);
        } else if (a == "--hud-off") {
            opt.room.hudOff = true;
        } else if (a == "--no-snapshot") {
            opt.room.noSnapshot = true;
        } else if (a == "--no-spawn-invuln") {
            // 부활 무적을 아예 안 건다. CLIENT_READY 되풀이로 무적이 안 풀려
            // 화면 확인이 막히는 것을 **원인 규명과 따로** 우회한다(Server.h 주석).
            opt.room.noSpawnInvuln = true;
        } else if (a == "--ack-any-stream") {
            // B-1b — 되받기를 **스트림 가리지 않고** 비신뢰에 싣는다.
            //   까닭 — 클라는 스트림 0 패킷의 seq 만 읽는다(09-18 실측. Server::Emit 주석).
            //   `--ack-seq` 와 같이 켠다.
            opt.ackAnyStream = true;
        } else if (a == "--no-ack-seq") {
            // 대조군 — 되받기를 끈다. 켜고 끈 차이를 재려고 남겨 둔 스위치다
            //   (09-18 실측 — 끄면 CLIENT_READY 를 18~25 번 받는다).
            opt.ackSeq = false;
            opt.ackAnyStream = false;
        } else if (a == "--ack-seq") {
            // B-1b — 클라가 보낸 신뢰 순번 +1 을 답 머리에 싣는다.
            // ⚠ **기본 끔** — 09-18 실측에서 효과가 없었다(Server.h ackRecvSeq_ 주석의 표).
            opt.ackSeq = true;
        } else if (a == "--no-newgear") {
            // ★ 진단용 — 새 사람 장비 알림을 끈다. Server.h 의 SetNotifyNewGear 주석 참조.
            opt.room.noNewGear = true;
        } else if (a == "--mirror") {
            opt.room.mirror = true;
        } else if (a == "--mirror-limit" && i + 1 < argc) {
            opt.room.mirror = true;
            opt.room.mirrorLimit = std::atoi(argv[++i]);
        } else if (a == "--mode" && i + 1 < argc) {
            opt.room.mode = argv[++i];
            if (nox::ModeGameFlags(opt.room.mode) == 0) {
                std::printf("모르는 모드: %s\n", opt.room.mode.c_str());
                std::printf("  판이 도는 것   arena · ctf · quest\n");
                std::printf("  규칙만 선 것   elimination · kotr · flagball · chat · coop · coopteam\n");
                return 2;
            }
        } else if (a == "--quest-count" && i + 1 < argc) {
            opt.room.questCount = std::atoi(argv[++i]);
        } else if (a == "--quest-spread" && i + 1 < argc) {
            opt.room.questSpread = std::atof(argv[++i]);
        } else if (a == "--map" && i + 1 < argc) {
            opt.room.mapPath = argv[++i];
        } else if (a == "--standalone") {
            // 독립 모드 — 녹음 없이 접속 묶음·USE_MAP·스냅샷을 조립(P5-c-2). --map 필수.
            opt.room.standalone = true;
        } else if (a == "--dump-sections") {
            // 진단 — 맵 섹션 목록만 찍고 끝낸다(--map 필수). 서버를 안 띄운다.
            opt.dumpSections = true;
        } else if (a == "--dump-thing" && i + 1 < argc) {
            // 진단 — 종류 하나의 **클래스·서브클래스·플래그 원문**을 찍고 끝낸다.
            //   ⚠ 이름 대신 `#<와이어번호>` 도 받는다(`--dump-thing #528`).
            //   만든 까닭 — 「화살이 정답지의 어느 갈래를 타나」를 따질 때마다
            //   thing.bin 을 눈으로 뒤지고 있었다. 갈래는 **클래스 비트가 정한다**
            //   (`GAME4_1.c:4539~4550` — 0x400000 / 0x200000 → 2·4·그밖 / 0x100000).
            opt.dumpThing = argv[++i];
        } else if (a == "--start-mana" && i + 1 < argc) {
            // 진단 — 들어올 때 마나를 채운다. 비싼 주문을 무인으로 쏘려면 필요하다.
            opt.room.startMana = std::atoi(argv[++i]);
        } else if (a == "--start-health" && i + 1 < argc) {
            // 진단 — 들어올 때 체력을 낮춘다. 죽음·부활을 재려고.
            opt.room.startHealth = std::atoi(argv[++i]);
        } else if (a == "--arrow-frame-alt" && i + 1 < argc) {
            // 진단 ⑬ — N 틱마다 화살 프레임을 0↔16 으로 번갈아 보낸다.
            //   한 판 안에서 바꿔야 배경·화살 자리가 그대로라 픽셀 대조가 깨끗하다.
            opt.room.arrowFrameAlt = std::atoi(argv[++i]);
        } else if (a == "--arrow-freeze") {
            // 진단 ⑬ — 화살을 난 자리에 세워 둔다. `--arrow-frame` 과 짝으로 쓴다.
            opt.room.arrowFreeze = true;
        } else if (a == "--2nd-newplayer") {
            // 옛 동작 — 관전 풀 때 `2d NEW_PLAYER` 를 **다시 보낸다**(09-19 이전 기본값).
            //   ⚠ 그러면 자기 장비가 화면에서 사라진다. 되돌릴 일이 있을 때만 쓸 것
            opt.room.secondNewPlayer = true;
        } else if (a == "--harpoon-force") {
            opt.room.forceHarpoon = true;
        } else if (a == "--dump-harpoon") {
            opt.room.dumpHarpoon = true;
        } else if (a == "--no-2nd-newplayer") {
            // 진단 — 관전 풀 때 `2d NEW_PLAYER` 를 다시 안 보낸다(정답지는 안 보낸다)
            opt.room.noSecondNewPlayer = true;
        } else if (a == "--dump-arrows") {
            // 진단 ⑬ — 틱 로그마다 화살 목록(번호·자리·방향·실은 프레임)과
            //   **프레임 분포**를 찍는다. 화면 기울기와 1:1 로 맞대려는 것이고,
            //   「각도가 하나뿐인가」는 분포 한 줄로 갈린다.
            opt.room.dumpArrows = true;
        } else if (a == "--arrow-frame" && i + 1 < argc) {
            // 진단 ⑬ — 화살 스프라이트 프레임을 못 박는다(0~31). 같은 자리에서
            //   프레임만 바꿔 찍어 화면이 실제로 달라지는지 픽셀로 대조하려고.
            opt.room.forceArrowFrame = std::atoi(argv[++i]);
        } else if (a == "--anim-dir" && i + 1 < argc) {
            // 진단 — 사람 항목 덧붙이의 방향 3비트를 못 박는다(0~7).
            opt.room.forceDir = std::atoi(argv[++i]);
        } else if (a == "--anim-pose" && i + 1 < argc) {
            // 진단 — 사람 자세 번호를 못 박는다(0 서기 · 2 죽음 · 4 걷기).
            opt.room.forcePose = std::atoi(argv[++i]);
        } else if (a == "--spell-start") {
            // ★ 시전 알림(0x70)을 **켠다.** 기본은 끔 — Server.h 의 noSpellStart_ 주석 참조.
            opt.room.spellStart = true;
        } else if (a == "--creature-aoi") {
            // 크리처 시야 판정 — 정답지 기본 사각형(화면절반+100 = 740×580)을 넘으면 안 싣고 사라짐을 보낸다.
            opt.room.creatureAoiHalf = -1.0;
        } else if (a == "--creature-aoi-half" && i + 1 < argc) {
            // 진단 — 위를 **판정용 정사각형 반폭 N** 으로 켠다. 작게 주면 몬스터가 사각형 밖에서 시작해
            //   「안 보이다가 다가오면 나타난다」를 화면으로 볼 수 있다.
            opt.room.creatureAoiHalf = std::atof(argv[++i]);
        } else if (a == "--creature-away-after" && i + 1 < argc) {
            // 진단 — 「사라짐」을 화면으로 보려고 크리처를 사각형 밖으로 옮긴다. 걷기를 빼 버린다.
            opt.room.creatureAwayAfter = std::atoi(argv[++i]);
        } else if (a == "--friends") {
            // 친구 알림 — 자기 소환수·같은 팀을 친구로 알린다. 클라는 커서·선택 원 색을 이것으로 가른다.
            opt.room.friends = true;
        } else if (a == "--debug-own-pet" && i + 1 < argc) {
            // 진단 — 친구 판정용. 소환 조작을 빼 버리고 그 사람 소유 임프를 사람 자리에 고정한다.
            opt.room.debugOwnPet = std::atoi(argv[++i]);
        } else if (a == "--auto-respawn") {
            // 자동 부활 서버 설정(정답지 기본 꺼짐 — 죽은 뒤 클릭해야 살아난다 · 묶음 3-c)
            opt.room.autoRespawn = true;
        } else if (a == "--debug-hurt-front") {
            // 진단 — 위 피해를 바라보는 쪽 30 앞에서 때린 것으로(방패 막기 판정 · 묶음 3-e2)
            opt.room.debugHurtFront = true;
        } else if (a == "--debug-hurt-every" && i + 1 < argc) {
            // 진단 — 피해 함수 판정용(묶음 3-e). N 틱마다 첫 사람에게 피해 25(움찔 문턱 20 넘김).
            opt.room.debugHurtEvery = std::atoi(argv[++i]);
        } else if (a == "--debug-dummy" && i + 1 < argc) {
            // 진단 — 주먹 판정용(묶음 3-a). 첫 사람 N 틱 뒤 주인 없는 임프를 오른쪽 +25 에 고정(맞으면 깎이고 죽는다).
            opt.room.debugDummy = std::atoi(argv[++i]);
        } else if (a == "--debug-pet-order" && i + 1 < argc) {
            // 진단 — `--debug-own-pet` 소환수에게 명령을 준다(2 가만히·3 지켜·4 따라와·**5 사냥**).
            //   ⚠ 명령이 없으면 주인 있는 크리처는 **한 칸도 안 움직인다**(09-20 실측)
            opt.room.debugPetOrder = std::atoi(argv[++i]);
        } else if (a == "--debug-dummy-dx" && i + 1 < argc) {
            // 진단 — 허수아비 오른쪽 거리(기본 25). 묶음 4-a2 날리기 화면 — 멀리 두면 Magic 이 날아가는 게 보인다.
            opt.room.debugDummyDx = std::atoi(argv[++i]);
        } else if (a == "--debug-enchant" && i + 3 < argc) {
            // 진단 — 묶음 4-b 화면 판정. 첫 사람(관전 푼 뒤)에게 버프 B 를 세기 P 로 N 틱 뒤 끝없이 건다.
            opt.room.debugEnchantB = std::atoi(argv[++i]);
            opt.room.debugEnchantP = std::atoi(argv[++i]);
            opt.room.debugEnchantN = std::atoi(argv[++i]);
        } else if (a == "--debug-bot" && i + 2 < argc) {
            // ★★ 2026-09-16 신설 — **가짜 사람**을 세운다.
            //   왜 — 계수기가 `made > 0` 일 때만 찍히는데, 사람이 없으면 내보낼 게 없어
            //   **틱 줄이 아예 안 나온다**(실측). 그래서 ③.5 「실물로 닫기」가 **사람 없이는 성립을 안 한다.**
            //   봇을 세우면 덫이 겨누고 갱신이 돌아 계수기가 살아난다 — 무인으로 판정할 수 있게 된다.
            //   ⚠ 봇 주소는 `addrOf` 에 없으므로 나가는 조각은 `flush` 에서 **조용히 버려진다**(main.cpp 의 `continue`).
            //     (10-09 조각 5d-2b — 그 자리는 이제 네트워크 스레드의 보내기 `NetworkThread.cpp` ⑤)
            //     즉 망으로는 아무것도 안 나간다. 서버 안 상태와 계수기만 움직인다.
            BotWant bw;
            bw.x = std::atoi(argv[++i]);
            bw.y = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') bw.cls = argv[++i];
            opt.room.bots.push_back(bw);
        } else if (a == "--spawn-at" && i + 2 < argc) {
            // ④ 화면 확인용 — 볼 것이 있는 자리에서 바로 시작한다(걸어가는 시간을 없앤다).
            opt.room.spawnAtX = std::atoi(argv[++i]);
            opt.room.spawnAtY = std::atoi(argv[++i]);
        } else if (a == "--debug-xp" && i + 2 < argc) {
            // 진단 — ④대기 ⑨. 첫 사람에게 N 틱 뒤 경험치를 한 번 준다(HUD 막대가 차는지).
            opt.room.debugXpAfter = std::atoi(argv[++i]);
            opt.room.debugXpAmount = std::atof(argv[++i]);
        } else if (a == "--debug-give-spell" && i + 2 < argc) {
            // 진단 — ④대기 ⑨. 첫 사람에게 N 틱 뒤 주문 하나를 준다(주문책이 느는지).
            opt.room.debugSpellAfter = std::atoi(argv[++i]);
            opt.room.debugSpellId = std::atoi(argv[++i]);
        } else if (a == "--debug-conjurer-guides") {
            // 진단 — 09-24 사람 판. 들어온 컨저러에게 박쥐·흰거미 둘·거미 안내서를 준다(부를 짐승이 있어야 「물릴 때 소리」를 본다).
            opt.room.debugConjurerGuides = true;
        } else if (a == "--debug-team-after" && i + 1 < argc) {
            // 진단 — RESET_FRIENDS 판정용. 첫 사람의 팀을 N 틱 뒤 한 번 바꾼다.
            opt.room.debugTeamAfter = std::atoi(argv[++i]);
        } else if (a == "--fill-colors") {
            // 진단 — 빈 캐릭터 색을 정품 녹음 실측값으로 메운다(기본 끔).
            //   ⚠ 켜면 **살색 피부가 드러나 정답지와 달라진다**(09-19 화면 대조)
            nox::fillColorsOn = true;
        } else if (a == "--debug-fist" && i + 3 < argc) {
            // 진단 — N틱 뒤 **서버가 직접** 첫 사람 자리 + (dx,dy) 에 주먹을 떨어뜨린다(⑰ 촬영용)
            opt.room.debugFistAfter = std::atoi(argv[++i]);
            opt.room.debugFistDx = std::atoi(argv[++i]);
            opt.room.debugFistDy = std::atoi(argv[++i]);
        } else if (a == "--debug-mob" && i + 4 < argc) {
            // 진단 — N틱 뒤 그 자리에 맵 몬스터 하나(종류 와이어 · 예 1358 거머리)를 세운다(⑮ 촬영용).
            //   ⚠ 퀘스트 스폰은 **사람 자리 기준**이라 몬스터가 사람에게 붙는다 — 이건 자리를 못 박는다
            opt.room.debugMobAfter = std::atoi(argv[++i]);
            opt.room.debugMobType = std::atoi(argv[++i]);
            opt.room.debugMobX = std::atoi(argv[++i]);
            opt.room.debugMobY = std::atoi(argv[++i]);
        } else if (a == "--trace-pets" && i + 1 < argc) {
            // 진단 — N틱마다 크리처의 **자리·맨 위 행동·고른 길목**을 찍는다(⑥ 6-j(3) 주행용).
            //   ⚠ 「안 움직인다」를 보려면 **자리와 고른 길목을 같이** 봐야 한다.
            //     자리만 보면 「길이 막혔나」와 「같은 곳을 또 골랐나」가 안 갈린다(09-20 에 그 함정에 빠졌다)
            opt.room.tracePets = std::atoi(argv[++i]);
        } else if (a == "--debug-wall" && i + 3 < argc) {
            // 진단 — N틱 뒤 그 **칸**(gx,gy)의 비밀벽을 연다(9-a — 클라가 그려 주나만 본다)
            opt.room.debugWallAfter = std::atoi(argv[++i]);
            opt.room.debugWallGx = std::atoi(argv[++i]);
            opt.room.debugWallGy = std::atoi(argv[++i]);
        } else if (a == "--debug-warp" && i + 3 < argc) {
            // 진단 — N틱 뒤 첫 사람을 그 자리로 한 번 옮긴다(문·밟는 판처럼 「다가가야」 보는 것용)
            opt.room.debugWarpAfter = std::atoi(argv[++i]);
            opt.room.debugWarpX = std::atoi(argv[++i]);
            opt.room.debugWarpY = std::atoi(argv[++i]);
        } else if (a == "--debug-kill" && i + 1 < argc) {
            // 진단 — N틱 뒤 첫 사람이 둘째를 죽인 것으로 친다(킬→점수→판끝 사슬 확인용)
            opt.room.debugKillAfter = std::atoi(argv[++i]);
        } else if (a == "--debug-score" && i + 2 < argc) {
            // 진단 — N틱 뒤 첫 사람의 점수를 <점수> 로 만든다(판 끝 사슬 확인용)
            opt.room.debugScoreAfter = std::atoi(argv[++i]);
            opt.room.debugScoreVal = std::atoi(argv[++i]);
        } else if (a == "--debug-team-every" && i + 1 < argc) {
            // 진단 ④ — N틱마다 첫 사람의 팀을 1↔2 로 오간다(친구 ADD 뒤 REMOVE 도 보려고)
            opt.room.debugTeamEvery = std::atoi(argv[++i]);
        } else if (a == "--no-obstacles") {
            // 진단 — 장애물(오브젝트끼리 충돌)을 싣지 않는다. 묶음 2-c 화면 판정의 대조군.
            opt.room.noObstacles = true;
        } else if (a == "--aoi") {
            // 시야 반경(P4-b) — 갱신 스트림에 맵 정적 오브젝트를 반경 안만 싣는다. --map 필수.
            opt.room.aoi = true;
        } else if (a == "--spawn-at-item" && i + 1 < argc) {
            // ★ 09-27 B6 — 이름으로 고른 바닥 물건(첫 것) 자리로 스폰하고 그 번호를 찍는다.
            //   자동 클라(`NOX_AUTO_CODES`)가 그 번호로 줍기·입기를 보내 무기 들기 관문을 **진짜 와이어로** 탄다
            opt.room.spawnAtItem = argv[++i];
        } else if (a == "--give-items" && i + 1 < argc) {
            // ★ 09-30 사람 판 시험용 — `Bow,Quiver,Quiver,CrossBow` 처럼 쉼표로. 들어온 사람마다 세계를 받은 지 2초 뒤 가방에 넣는다.
            //   틀은 이 맵의 바닥 물건에서 뜬다(맵에 없는 종류는 못 준다) · 기본 장비 여섯 종류는 부활 틀을 건드리지 않게 뺀다
            std::string list = argv[++i];
            for (size_t p = 0; p <= list.size();) {
                const size_t e = list.find(',', p);
                const std::string nm = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!nm.empty()) opt.room.giveItemNames.push_back(nm);
                if (e == std::string::npos) break;
                p = e + 1;
            }
        } else if (a == "--item-at-spawn") {
            // ★ 시험용 — 아이템 하나를 **스폰 자리로 옮긴다.**
            //
            //   왜 — 「어떤 조작이 줍기를 보내나」를 재려면 물건 위에 서 있어야 하는데,
            //   걸어가는 길이 자꾸 어긋난다(2026-09-10 스윕 1차: 도착 실패로 127유닛 밖에서
            //   재는 바람에 30가지가 전부 0건으로 나왔다. 그건 「없었다」가 아니라 「못 쟀다」다).
            //   재는 대상은 **조작**이지 걷기가 아니니, 걷기를 빼 버린다.
            opt.room.itemAtSpawn = true;
        } else if (a == "--ctf-span" && i + 1 < argc) {
            opt.room.ctfSpan = std::atoi(argv[++i]);
        } else if (a == "--dump" && i + 1 < argc) {
            opt.room.dumpPath = argv[++i];
        } else if (a == "--game-dir" && i + 1 < argc) {
            opt.gameDir = argv[++i];   // ★ 10-12 작업 7e-2 — 게임 폴더(thing.bin 등). 기본 = GOG 설치 경로
        } else if (a == "--debug-crash" && i + 1 < argc) {
            // 진단 — N 틱 뒤 일부러 죽는다. 크래시 덤프가 실제로 남는지 보는 용도(10-08)
            opt.room.debugCrashAfter = std::atoi(argv[++i]);
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
    tmpl.serverName = opt.name;

    // ★ 서버 로직은 `nox::Server` 가 들고 있다. 여기는 소켓만 붙인다.
    //   같은 로직을 대조 장치(server/src/replay.cpp)가 덤프로 돌려 채점한다.
    //
    // 서버 찾기에 답할 원본은 녹음을 쓰되, 이름만 인자로 바꿔 넣는다.
    // ★ 10-09 조각 5d-2b — 스레드 둘. 네트워크 스레드(소켓 · 통신 층 — `CNetworkThread`)와 게임(이 스레드)이 큐 둘로만 만난다
    //   (`Docs/LAYERS_DESIGN.md` 「네트워크 스레드」). 통신 층 설정과 봇 세션 번호는 스레드를 띄우기 전에 한다.
    //   (5d-1b 까지는 한 스레드 조립 `CSyncHost` 였다 — 시험 · 덤프 재생은 그대로 그것을 쓴다)
    nox::CNetworkThread net;
    // ★ 10-09 플랜 B 작업 6a — 게임 스레드는 방 관리자(`CRoomManager`)를 돌린다. 지금은 방 하나(방 0)라 동작은 그대로이고,
    //   `srv` 는 방 0 의 게임이다(세계 차리기 · 진단 줄). 이벤트 · 틱 · 요청 · 다음 틱 시각은 방 관리자를 거친다
    nox::CRoomManager rooms;
    nox::Server& srv = rooms.Game();

    // ★★ **`--standalone` 은 맵이 있어야 성립한다.** 접속 묶음에 맵 이름과 CRC 를
    //   싣기 때문이다(`SetStandalone`). 맵이 없으면 `standalone_` 이 안 켜지고,
    //   그러면 `CLIENT_ACCEPT` 가 **조용히 아무것도 안 보내는 분기**로 빠진다.
    //
    // ⚠ 2026-09-10 — 이걸로 실클라 회귀 네 판을 날렸다. 증상이 「접속 요청 158바이트에
    //   답이 0개」였는데 로그 어디에도 까닭이 없어 코드 회귀를 의심하고 이분법까지 돌렸다.
    //   **안 되는 것을 조용히 지나가게 두면 다음 사람이 반나절을 쓴다.** 여기서 죽인다.
    if (opt.room.standalone && opt.room.mapPath.empty()) {
        std::printf("[멈춤] --standalone 에는 --map 이 있어야 한다.\n"
                    "       맵이 없으면 접속 묶음을 만들 수 없어 **아무도 못 들어온다**\n"
                    "       (클라가 접속 요청을 되풀이하다 20초 뒤 조용히 끊는다).\n"
                    "  보기: --standalone --map \"C:\\...\\maps\\estate\\Estate.map\"\n");
        return 2;
    }

    if (opt.dumpSections && opt.room.mapPath.empty()) {
        std::printf("[멈춤] --dump-sections 에는 --map 이 있어야 한다.\n");
        return 2;
    }

    // ★ 진단 --dump-thing — 맵 없이 돌아야 하므로 맵 처리보다 **앞**에 둔다.
    //   찍는 것 = 클래스 원문·비트 · 서브클래스 원문 · 플래그 원문 · 복합 여부 · 그리기 함수.
    //   갈래를 정하는 것은 클래스 비트다(`GAME4_1.c:4539~4550`) —
    //     0x400000 → netSpriteUpdate · 0x200000 → (2 몬스터 / 4 사람 / 그밖 팬텀) · 0x100000 → 단순
    if (!opt.dumpThing.empty()) {
        nox::ThingDb tdb;
        if (!tdb.Load(opt.gameDir + "\\thing.bin")) {   // ★ 10-12 작업 7e-2 — 박힌 경로 대신 `--game-dir`
            std::printf("[멈춤] thing.bin 을 못 읽었다.\n");
            return 1;
        }
        uint16_t wire = 0;
        if (opt.dumpThing[0] == '#') wire = static_cast<uint16_t>(std::atoi(opt.dumpThing.c_str() + 1));
        else                     wire = tdb.WireByName(opt.dumpThing);
        const nox::ThingInfo* ti = wire ? tdb.ByWire(wire) : nullptr;
        if (!ti) {
            std::printf("[멈춤] 그런 종류가 없다: %s\n", opt.dumpThing.c_str());
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
        // ★ 09-29 Z1-A — 닿음·줍기·버리기 함수와 무게 유무(열쇠가 왜 세계에 안 실리나를 보려고)
        std::printf("  COLLIDE    %s %s\n", ti->collideFn.empty() ? "(없음)" : ti->collideFn.c_str(), ti->collideArgs.c_str());
        std::printf("  PICKUP     %s · DROP %s · WEIGHT %s\n", ti->pickupFn.empty() ? "(없음)" : ti->pickupFn.c_str(),
                    ti->dropFn.empty() ? "(없음)" : ti->dropFn.c_str(), ti->hasWeight ? "있음" : "없음");
        // ★ 09-29 — 쓰기 함수(화살통 AmmoUse · 활 등 — 쓰기 짐이 화살 수·재장전 칸) · 초기 함수
        std::printf("  USE        %s %s · INIT %s %s\n", ti->useHandler.empty() ? "(없음)" : ti->useHandler.c_str(), ti->useArgsRaw.c_str(),
                    ti->initFn.empty() ? "(없음)" : ti->initFn.c_str(), ti->initArgs.c_str());
        std::fflush(stdout);
        return 0;
    }

    // ★ 진단 --dump-sections — 섹션 목록만 찍고 **여기서 끝낸다**(서버를 안 띄운다).
    //   한 줄에 하나, 탭으로 갈라 찍는다 — 157개를 돌려 표로 합치려고.
    //     SEC <맵이름> <섹션이름> <크기>
    //   ⚠ 10-08 — 세계 차리기를 `BuildWorld` 로 옮기면서 이 진단만 여기 남겼다(서버를 안 띄우는 일이라).
    if (opt.dumpSections) {
        nox::MapFile mf;
        if (!mf.Load(opt.room.mapPath)) {
            std::printf("맵을 못 읽음: %s (%s)\n", opt.room.mapPath.c_str(), mf.Error().c_str());
            return 1;
        }
        std::string base = opt.room.mapPath;
        const size_t sl = base.find_last_of("\\/");
        if (sl != std::string::npos) base = base.substr(sl + 1);
        for (const auto& s : mf.Sections())
            std::printf("SEC\t%s\t%s\t%zu\n", base.c_str(), s.name.c_str(), s.size);
        std::fflush(stdout);
        return 0;
    }

    // ★ 10-10 토큰 작업 2 — 입장 방식. 키가 없으면 뜨지 않는다 — 설정 하나 빠뜨린 서버가 아무나 받지 않게(`Docs/TOKEN_DESIGN.md`).
    //   진단(위 `--dump-*`)과 `--selftest` 는 서버를 안 띄우므로 이보다 앞에서 끝난다.
    if (opt.noJoinToken) {
        rooms.SetOpenEntry();
        std::printf("입장: 토큰 검사 끔(--no-join-token) — 모두 방 0. 녹음 재생 · 시험 도구 · LAN 실행용\n");
    } else {
        if (opt.configPath.empty()) {
            std::printf("[멈춤] 입장 토큰 키가 없다 — --config <설정 파일>([token] key = 16진 64자).\n"
                        "       시험 도구 · LAN 실행이면 --no-join-token(모두 방 0)\n");
            return 2;
        }
        nox::CIniFile ini;
        if (!ini.LoadFile(opt.configPath)) {
            std::printf("[멈춤] 설정 파일을 못 읽었다 — %s\n", ini.Error().c_str());
            return 2;
        }
        nox::JoinKey key{};
        if (!nox::ParseJoinKey(ini.Get("token", "key"), key)) {
            std::printf("[멈춤] 설정 파일의 [token] key 가 16진 64자가 아니다 — %s\n", opt.configPath.c_str());
            return 2;
        }
        rooms.SetJoinKey(key);
        rooms.SetLogJoins(true);
        std::printf("입장: 토큰 방식 — 설정 %s\n", opt.configPath.c_str());
    }
    std::fflush(stdout);

    // ★ 10-08 — 세계 차리기(맵·설정·접속 묶음)는 `BuildWorld`(`WorldBuild.cpp`)로 옮겼다.
    // ★ 10-12 플랜 B 작업 7a — 방 몫(`BuildRoomWorld`)과 프로세스 몫(`ConfigureNetwork`)으로 나눴다.
    //   방 몫이 먼저다 — 서버 찾기 답에 실을 맵 이름을 첫 방에서 받는다. 봇 세션은 통신 층이 연다(스레드를 띄우기 전이라 여기서 부른다)
    // ★ 10-12 플랜 B 작업 7c — 방마다 main 이 드는 것(`RoomRun` — 판 차리기 · 진단). 지금은 실행 인자의 방(방 0) 하나
    std::map<int, RoomRun> roomRuns;
    RoomRun& run0 = roomRuns[nox::CRoomManager::kFirstRoom];
    run0.opt = &opt.room;
    WorldExtras& extras = run0.extras;
    nox::CUdpServer& udp = net.Network();
    // ★ 10-12 플랜 B 작업 7e-2 — 게임 데이터는 맵 방이 있을 때만 서버가 뜰 때 한 번 읽어 표 한 벌로(녹음 모드는 지금처럼 표 없이)
    GameFiles gameFiles;
    std::shared_ptr<const nox::Server::GameTables> gameTables;
    MapCatalog maps;   // ★ 10-12 플랜 B 작업 7e-4b — 서버가 맡는 맵의 원본들(방은 이것에 붙는다)
    if (!opt.room.mapPath.empty()) {
        LoadGameFiles(opt.gameDir, gameFiles);
        gameTables = BuildGameTables(gameFiles);
        maps = BuildMapCatalog(gameFiles, gameTables, MapListFromOptions(opt));   // 목록의 출처는 `MapListFromOptions` 주석(지금 `--map` 하나)
    }
    if (const int rc = BuildRoomWorld(srv, opt.room, gameFiles, gameTables, maps, [&udp](const std::string& addr) { return udp.OpenSession(addr); }, extras)) return rc;
    if (const int rc = ConfigureNetwork(udp, opt, tmpl, extras)) return rc;
    // 초당 `[성장]` 줄의 봇 번호 — 통신 층 값이라 스레드를 띄우기 전에 읽어 둔다
    run0.bot0Sid = net.Network().SessionIdOf("bot0");

    // ★ 10-09 조각 5d-2b — 소켓은 네트워크 스레드가 연다(옛 이 자리의 `WSAStartup` · `socket` · `bind` — 실패 문구 그대로)
    {
        std::string netErr;
        if (!net.Open("", static_cast<uint16_t>(opt.port), netErr)) {
            std::printf("%s\n", netErr.c_str());
            return 1;
        }
    }

    std::printf("접속 대기 — UDP %d, 이름 \"%s\"\n", opt.port, opt.name.c_str());
    std::printf("답하는 것: 서버 찾기 · 접속 시도 · 접속 수락 · 핑\n");
    std::fflush(stdout);

    // ★ 10-09 조각 5d-2b — 받기 · 주소 · 보내기는 네트워크 스레드 몫이다(옛 `keyOf` · `addrOf` · `flush` 는 `NetworkThread.cpp`).
    std::vector<nox::NetworkEvent> netEvents;
    std::vector<nox::NetworkRequest> netRequests;
    unsigned long long nTicks = 0;
    // 초당 `[스레드]` 줄 — 이벤트 넘김 지연(큐에 넣은 때 → 게임이 처리한 때) · 틱 시간(`RunTick`)
    unsigned long long evCount = 0, evLatSum = 0, tickRuns = 0;
    uint32_t evLatMax = 0;
    double tickMsSum = 0.0, tickMsMax = 0.0;
    // ★ 10-09 플랜 A 5d 자가검증 ② — 틱 간격(앞 `RunTick` 시작 → 이번 시작). 목표 33ms — 루프가 깨는 때가 거칠면 여기서 흔들린다
    std::chrono::steady_clock::time_point prevTickStart{};
    double tickGapSum = 0.0, tickGapMin = 0.0, tickGapMax = 0.0;
    unsigned long long tickGaps = 0;
    // 게임 루프가 돈 바퀴 수 — 틱(30) + 이벤트로 깬 수 근처여야 한다. 기다림이 빠지면(헛돎) 수만 단위로 뛴다
    unsigned long long loopTurns = 0;

    // ★ 10-01 R2 — 맵을 다 넣었으니 처음 상태를 담아 둔다(판 다시 열 때 되돌릴 사본 · `Server::CaptureMapInitial`)
    // ★ 10-12 플랜 B 작업 7e-4b — 맵 방은 맵 원본이 담아 둔 처음 상태를 같이 가리킨다(`Server::AttachMap` — 담기 셈도 거기서).
    //   제 것을 담는 것은 맵 없는 방(녹음 모드)뿐
    if (opt.room.mapPath.empty()) srv.CaptureMapInitial();
    srv.Log("[맵처음] 담음 — 바닥 물건 %zu · 문·오벨리스크 %zu\n", srv.WorldStaticCount(), srv.WorldPropCount());
    std::fflush(stdout);
    net.SetVerbose(!opt.quiet);
    net.Start();   // ★ 10-09 조각 5d-2b — 여기서부터 받기 · 보내기는 네트워크 스레드
    // ★ 10-09 조각 5d-2c — 닫기 신호 둘 — 콘솔(위 `OnConsoleCtrl`)과 이름 있는 이벤트 `nox_server_stop_<pid>`
    //   (콘솔 프로그램은 `taskkill` 의 창 닫기 신호를 못 받는다 — `서버클라_실행.bat` 은 이 이벤트를 켜고, 안 꺼지면 그때 강제로 끈다)
    g_shutdownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    wchar_t stopName[64];
    swprintf(stopName, 64, L"nox_server_stop_%lu", GetCurrentProcessId());
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName);
    // ★ 10-09 플랜 A 5d 자가검증 ② — 틱 마감 타이머. 정밀 타이머(윈도 10 1803~)라야 마감에 1ms 안으로 깬다 —
    //   보통 타이머 · `Sleep` 은 윈도 기본 간격(약 15.6ms) 단위로 깬다(10-09 실측 — `Sleep(1)` 이 중앙 15.5ms를 잤다)
    HANDLE tickTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tickTimer) {
        tickTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        std::printf("[루프] 정밀 타이머를 못 만들어 보통 타이머로 기다린다 — 틱 간격이 약 15.6ms 단위로 흔들린다\n");
        std::fflush(stdout);
    }

    // ★ 10-09 조각 5d-2b — 게임 스레드. 쌓인 이벤트를 통째로 받아 처리하고(받은 조각 · 접속 · 닫기 · 접속 시도 · 시간 초과),
    //   틱을 돌리고, 그 사이 쌓인 통신 요청을 통째로 넘긴다. 옛 이 자리는 `select`(5ms) · `recvfrom` 으로 직접 받았다.
    //   받은 조각마다 찍던 `[받음]` · 열쇠 진단과 접속 제어 쪽 `[처음]` 은 네트워크 스레드가 찍는다.
    //   ★ 10-09 플랜 A 5d 자가검증 ② — 한 바퀴 끝에 「이벤트가 들어옴」(`EventsReadyHandle`) · 「다음 틱 마감」(`tickTimer`) 중
    //   먼저 오는 것을 기다린다. 닫기 신호는 늦어도 다음 틱(33ms)에 맨 위에서 본다.
    std::map<uint8_t, bool> told;            // ★ 10-12 작업 7c — `[처음]` 줄을 낸 오프코드(프로세스 몫 — 옛 루프 안 `static`)
    unsigned long long procMade = 0;         // ★ 10-12 작업 7c — 이 1초 동안 낸 보내기 요청(프로세스 몫 초당 줄의 문턱)
    uint32_t procLastMs = nox::NowMs();      //   그 줄을 마지막으로 찍은 때
    for (;;) {
        if (g_stopRequested.load() || WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;   // ★ 5d-2c — 닫기 신호
        ++loopTurns;
        net.SwapOutNetworkEvents(netEvents);
        if (!netEvents.empty()) {
            const uint32_t nowEv = nox::NowMs();   // 넣은 시각(`queuedMs`)과 같은 시계(`Clock.h`)
            for (const nox::NetworkEvent& ev : netEvents) {
                const uint32_t lat = nowEv - ev.queuedMs;
                ++evCount; evLatSum += lat; if (lat > evLatMax) evLatMax = lat;
                rooms.OnNetworkEvent(ev);
            }
            netEvents.clear();
            // ★★ **클라가 처음 보낸 오프코드를 그때그때 알린다** — 게임 오프코드 쪽(옛 받은 조각마다 찍던 줄 · ⚠ `--quiet` 여도 찍는다)
            // ★ 10-12 플랜 B 작업 7c — 방마다 본 것을 모아 프로세스 몫 한 줄로(옛 꼴은 방 0 것만 읽었다). `told` 는 루프 밖(옛 `static`)
            rooms.ForEachRoom([&](const nox::CRoom& room) {
                for (const auto& kv : room.Game().seen()) {
                    if (told.count(kv.first)) continue;
                    told[kv.first] = true;
                    std::printf("[처음] 클라가 보낸 새 오프코드 0x%02x (%s)\n",
                                kv.first, nox::OpName(static_cast<nox::Op>(kv.first)));
                    std::fflush(stdout);
                }
            });
        }

        // ★ 10-12 플랜 B 작업 7c — 판 차리기는 방마다(`StartModeIfReady` — 옛 이 자리의 블록을 옮김)
        rooms.ForEachRoom([&](nox::CRoom& room) { StartModeIfReady(room.Game(), roomRuns.at(room.Id())); });

        // 틱 앞 — 받은 조각에 대한 답 · 판 차리기가 쌓은 요청을 먼저 넘긴다
        rooms.SwapOutNetworkRequests(netRequests);
        net.PushRequests(netRequests);
        const int ticks = rooms.BeginTick(nox::NowMs());   // ★ 10-09 자가검증 ② — 옛 `GetTickCount`(15.6ms 단위) 대신 `Clock.h`
        rooms.ForEachRoom([&](nox::CRoom& room) {   // ★ 10-12 작업 7c — 방마다(그 방의 설정 · 그 방의 프레임)
            const RoomRun& rr = roomRuns.at(room.Id());
            if (rr.opt->debugCrashAfter > 0 && room.Game().frame() >= static_cast<uint32_t>(rr.opt->debugCrashAfter)) {
                room.Game().Log("[진단스위치] --debug-crash — 프레임 %u 에서 일부러 죽는다\n", room.Game().frame());
                std::fflush(stdout);
                CRASH("--debug-crash");
            }
        });
        if (ticks > 0) {
            nTicks += static_cast<unsigned long long>(ticks);
            const auto tickStart = std::chrono::steady_clock::now();
            if (prevTickStart != std::chrono::steady_clock::time_point{}) {
                const double gap = std::chrono::duration<double, std::milli>(tickStart - prevTickStart).count();
                if (tickGaps == 0 || gap < tickGapMin) tickGapMin = gap;
                if (gap > tickGapMax) tickGapMax = gap;
                tickGapSum += gap; ++tickGaps;
            }
            prevTickStart = tickStart;
            // ★ 10-12 작업 7c — 틱 앞의 방마다 보내기 요청 누계(틱 뒤와 견줘 이 틱에 그 방이 낸 수)
            rooms.ForEachRoom([&](nox::CRoom& room) { roomRuns.at(room.Id()).sendSeen = room.SendRequests(); });
            rooms.RunTick();
            const double tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();
            tickMsSum += tickMs; ++tickRuns; if (tickMs > tickMsMax) tickMsMax = tickMs;
            // 그 틱의 통신 요청을 한 묶음으로 넘긴다. `made` = 그 틱의 보내기 요청 수(옛 「나간 조각 수」 —
            //   다시 보내기는 이제 네트워크 스레드 몫이라 빠진다). 초당 줄은 전처럼 이것이 0 이 아닐 때만 찍는다
            rooms.SwapOutNetworkRequests(netRequests);
            size_t made = 0;
            for (const nox::NetworkRequest& r : netRequests) if (r.type == nox::NetworkRequest::Type::SEND_MSG) ++made;
            net.PushRequests(netRequests);
            // ★ 10-12 플랜 B 작업 7c — 방마다 진단(`RoomTickDiagnostics` — 옛 이 자리의 블록을 옮김)
            rooms.ForEachRoom([&](nox::CRoom& room) {
                RoomRun& rr = roomRuns.at(room.Id());
                RoomTickDiagnostics(room.Game(), rr, static_cast<size_t>(room.SendRequests() - rr.sendSeen));
            });
            // ★ 10-12 플랜 B 작업 7c — 프로세스 몫 초당 줄(통신 층 · 스레드 지표). 방 줄과 따로 1초마다 — 방마다 프레임이 달라
            //   방의 문턱(`frame % 30`)에 묶을 수 없다. 그 1초 동안 보낸 것이 있을 때만(옛 문턱 `made > 0` 과 같은 뜻)
            procMade += made;
            if (!opt.quiet && procMade > 0 && nox::NowMs() - procLastMs >= 1000) {
                procLastMs = nox::NowMs();
                procMade = 0;
                // ★ 10-09 조각 5d-2a — 통신 층 지표는 사본으로 찍는다(5d-2 부터 네트워크 스레드 값이라 직접 읽지 않는다)
                const nox::CUdpStats ns = net.LatestNetworkStats();
                // ★ 10-12 작업 7c — 옛 `[틱]` 줄의 통신 층 칸(신뢰받음 · 되받기 · 누적 틱)을 옮겨 왔다. 스트림별 셋은 아래 `[순번·스트림]` 이 전부 찍는다
                // ★ 09-21 — 되받기는 「몇 번 실었나」보다 **값이 올라가나**가 뜻이 있다.
                //   옛 「다름」은 `ack`(클라 순번+1)과 `q.last`(우리 스트림0 순번)를 견주던
                //   숫자라 거의 늘 달랐다 — 결함이 아닌데 결함처럼 읽혔다(09-21 정정).
                //   그리고 **스트림별로 갈라** 찍는다 — 되받기는 같은 스트림에 실려야 뜻이 있다
                std::printf("[통신] 신뢰받음 %llu · 되받기 %llu[값바뀜 %llu·마지막 %u] · 누적 틱 %llu\n",
                            ns.reliableHeard, ns.ackSeqApplied, ns.ackSeqChanged, static_cast<unsigned>(ns.ackLastSent), nTicks);
                // ★ 10-08 조각 3 — 접속 제어 조각의 버림은 통신 층이 센다(게임 몫은 방 줄 `[버림]`)
                if (!ns.stoppedByOp.empty()) {
                    char b[128];   // ★ 10-09 — 다 만든 뒤 한 번에 찍는다
                    std::snprintf(b, sizeof(b), "[버림·통신] 조각 %llu개 —", ns.stopped);
                    std::string line = b;
                    for (const auto& kv : ns.stoppedByOp) {
                        std::snprintf(b, sizeof(b), " %02x(%s)x%llu", kv.first,
                                      nox::OpName(static_cast<nox::Op>(kv.first)), kv.second);
                        line += b;
                    }
                    std::printf("%s\n", line.c_str());
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
                        if (!ns.recvRelStream[st]) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.recvRelStream[st]);
                        sl += sb;
                    }
                    sl += " · 보낸신뢰";
                    for (uint8_t st = 0; st < 8; ++st) {
                        if (!ns.sendRelStream[st]) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.sendRelStream[st]);
                        sl += sb;
                    }
                    sl += " · 보낸비신뢰";
                    for (uint8_t st = 0; st < 8; ++st) {
                        if (!ns.sendPlainStream[st]) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)st, ns.sendPlainStream[st]);
                        sl += sb;
                    }
                    // ★ 되받기로 **어떤 값**을 실었나. 값이 한두 개뿐이면 `recvSeq` 가 안 올라가는 것이다
                    sl += " · 되받기값";
                    bool any = false;
                    for (uint8_t v = 0; v < 32; ++v) {
                        if (!ns.ackValueHist[v]) continue;
                        std::snprintf(sb, sizeof(sb), " %u:%llu", (unsigned)v, ns.ackValueHist[v]);
                        sl += sb;
                        any = true;
                    }
                    if (!any) sl += " (없음)";
                    std::printf("%s\n", sl.c_str());
                }
                // ★ 10-08 조각 3b — 신뢰 패킷 다시 보내기. 「다시 보냄」만 있고 「확인으로 지움」이 0 이면
                //   클라의 확인 순번을 못 읽는 것이다(UdpServer.cpp Ack)
                // ★ 10-09 조각 5b — 「타이머」는 힙에 든 항목 수(꺼낼 때 버릴 항목 포함). 세션 수 + 확인 기다리는 조각 수 근처에서
                //   머물러야 한다 — 버릴 항목은 제 마감(2초 · 한도 시간)에 빠진다. 계속 불어나면 버릴 항목이 안 빠지는 것이다
                std::printf("[다시보냄] 다시 보냄 %llu · 확인으로 지움 %llu · 바로 다시 보낼 표시 %llu · 한도 넘쳐 버림 %llu · 대기 %zu · 타이머 %zu\n",
                            ns.reliableResent, ns.reliableAcked,
                            ns.reliableRetryMarked, ns.reliableQueueDropped,
                            ns.reliablePending, ns.timerCount);
                // ★ 10-08 조각 4 — 지운 세션 번호로 보내려다 버린 수. 0 이 아니면 게임이 지운 사람에게 아직 보내고 있다
                std::printf("[세션] 없는 번호로 보내려다 버림 %llu · 접속 전 상대 걷어냄 %llu · 시간 초과로 넘김 %llu · 입장 요청 %llu · 거절 %llu · 답할 때 없음 %llu\n",
                            ns.sendNoSession, ns.unannouncedSwept,
                            ns.sessionTimedOut,    // ★ 10-09 조각 5a — 통신 층이 시간 초과로 게임에 넘긴 세션 수
                            ns.joinRequested, ns.joinRejected,
                            ns.joinRespondNoSession);   // ★ 10-09 조각 5c — 접속 시도를 게임에 물은 수 · 거절 · 답할 때 세션이 없던 수
                // ★ 10-09 조각 5d-2b — 스레드 둘의 지표(이 1초 동안). 「큐 버림」은 0 이 정상 · 넘김 지연은 틱 하나(33ms) 안이 정상
                {
                    const nox::CNetworkThreadStats ts = net.LatestThreadStats();
                    std::printf("[스레드] 이벤트 넘김 지연 평균 %.1fms · 최대 %ums(%llu건) · 틱 시간 평균 %.2fms · 최대 %.2fms(%llu틱)"
                                " · 틱 간격 최소 %.1fms · 평균 %.1fms · 최대 %.1fms(%llu) · 게임 루프 %llu바퀴"
                                " · 네트워크 깨어남 %llu · 받음 %llu · 보냄 %llu · 한 번에 비운 최대 %llu · 받기 오류 %llu"
                                " · 큐 버림(이벤트 %llu · 요청 %llu) · 큐 남음(이벤트 %zu · 요청 %zu)\n",
                                evCount ? static_cast<double>(evLatSum) / static_cast<double>(evCount) : 0.0, evLatMax, evCount,
                                tickRuns ? tickMsSum / static_cast<double>(tickRuns) : 0.0, tickMsMax, tickRuns,
                                tickGapMin, tickGaps ? tickGapSum / static_cast<double>(tickGaps) : 0.0, tickGapMax, tickGaps, loopTurns,
                                ts.wakes, ts.datagramsIn, ts.datagramsOut, ts.maxDatagramsPerWake, ts.recvErrors,
                                ts.eventsDropped, ts.requestsDropped, ts.eventsQueued, ts.requestsQueued);
                    evCount = evLatSum = tickRuns = 0; evLatMax = 0; tickMsSum = tickMsMax = 0.0;
                    tickGaps = 0; tickGapSum = tickGapMin = tickGapMax = 0.0; loopTurns = 0;
                }
                std::fflush(stdout);
            }
        }
        // ★ 10-09 플랜 A 5d 자가검증 ② — 기다림. 옛 `Sleep(1)`(5d-2b — 실제로는 약 15.5ms 잠)은 받은 것을 평균 8ms 늦게 처리했고
        //   틱 간격이 16~48ms로 흔들렸다(10-09 실측). 마감이 이미 지났으면 기다리지 않고 바로 다음 바퀴(밀린 틱을 몰아서 돈다)
        uint32_t tickDue = 0;
        if (rooms.NextTickDue(tickDue)) {
            const int32_t left = static_cast<int32_t>(tickDue - nox::NowMs());
            if (left > 0) {
                LARGE_INTEGER rel;
                rel.QuadPart = -static_cast<LONGLONG>(left) * 10000;   // 100ns 단위 · 음수 = 지금부터
                const HANDLE waits[2] = { static_cast<HANDLE>(net.EventsReadyHandle()), tickTimer };
                if (tickTimer && SetWaitableTimer(tickTimer, &rel, 0, nullptr, nullptr, FALSE))
                    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                else
                    WaitForSingleObject(waits[0], static_cast<DWORD>(left));   // 타이머가 없으면 이벤트만 시간 한도로(헛돌지 않게)
            }
        }
    }
    // ★ 10-09 조각 5d-2c — 정상 종료(`Docs/LAYERS_DESIGN.md` 「네트워크 스레드」) — 쌓인 요청을 넘기고 → 네트워크 스레드가 그것을 보낸 뒤
    //   키를 준 사람마다 `SERVER_CLOSE` 를 보내고 멈춘다 → 끝(종료 코드 0). 지금 게임에는 닫을 때 저장할 것이 없다.
    std::printf("[종료] 닫기 신호 — 쌓인 요청을 보내고 사람마다 SERVER_CLOSE 를 보낸 뒤 네트워크 스레드를 멈춘다\n");
    std::fflush(stdout);
    rooms.SwapOutNetworkRequests(netRequests);
    net.PushRequests(netRequests);
    net.Stop(/*closeSessions=*/true);
    {
        const nox::CUdpStats ns = net.LatestNetworkStats();
        const nox::CNetworkThreadStats ts = net.LatestThreadStats();
        std::printf("[종료] 끝 — 서버 닫힘 알림 %llu명 · 보낸 조각 %llu · 큐 버림(이벤트 %llu · 요청 %llu)\n",
                    ns.serverCloseSent, ts.datagramsOut, ts.eventsDropped, ts.requestsDropped);
        std::fflush(stdout);
    }
    if (stopEvent) CloseHandle(stopEvent);
    if (tickTimer) CloseHandle(tickTimer);
    SetEvent(g_shutdownDone);
    return 0;
}
