// RoomTick.cpp — 방의 판 차리기 · 틱 뒤 진단(`CRoom::StartModeIfReady` · `CRoom::TickDiagnostics`).
//
// ★ 10-14 플랜 B 작업 9c — 옛 `main.cpp` 의 `RoomRun`(방 번호를 열쇠로 `main` 이 들던 방 몫 상태)과 두 함수를 방(`CRoom`)으로 옮겼다
//   (`Docs/ROOMS_DESIGN.md` 원칙 「방의 상태는 방 안에만」 · 「실행 파일에만 있는 코드는 조립뿐」). 몸통은 글자 그대로 —
//   옛 인자 `srv` · `rr` 자리는 첫머리의 같은 이름 별칭(방의 게임 · 방 설정 · 방 멤버)이 받는다.
//   진단 도우미 둘(`PrintAbilityBreakdown` · `AbilityFingerprint`)도 같이 옮겼다 — 이 두 함수만 쓴다
#include <algorithm>
#include <cstdio>
#include <string>

#include "nox/ModeRules.h"
#include "nox/Opcode.h"
#include "nox/RoomManager.h"
#include "nox/Server.h"
#include "nox/SpellNames.h"
#include "nox/WorldBuild.h"

namespace {

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


}  // namespace

// ★ 10-12 플랜 B 작업 7c — 판 차리기(옛 `main` 게임 루프 안 블록을 그대로 옮김). 방마다 부른다
// ★ 10-14 작업 9c — 옛 `StartModeIfReady(srv, rr)`. 상태는 방 멤버
void nox::CRoom::StartModeIfReady() {
    nox::Server& srv = _game;
    const RoomSettings& opt = _settings;
    const WorldExtras& extras = _extras;
    bool& modeStarted = _modeStarted;
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
// ★ 10-14 작업 9c — 옛 `RoomTickDiagnostics(srv, rr, made)`. 상태는 방 멤버
void nox::CRoom::TickDiagnostics(size_t made) {
    nox::Server& srv = _game;
    const RoomSettings& opt = _settings;
    unsigned long long& tracePetsInWall = _tracePetsInWall;
    unsigned long long& tracePetsSamples = _tracePetsSamples;
    const int64_t bot0Sid = _bot0Sid;
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
        unsigned long long& lastAbilFp = _lastAbilFp;   // ★ 10-12 작업 7c — 옛 `static`(방마다로) · ★ 10-14 작업 9c — 방 멤버
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
                    // ★ 09-21 — 벽 튕김 재기(화면에서 짚음). 걷기 힘은 3.0 · 걷기 속도 6 이 정상
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

const char* nox::CRoom::CmdOutcomeName(CmdOutcome o) {
    switch (o) {
        case CmdOutcome::Started: return "시작";
        case CmdOutcome::NotHost: return "방장 아님";
        case CmdOutcome::NotWaiting: return "대기 아님";
        case CmdOutcome::TooFew: return "인원 모자람";
        case CmdOutcome::NoMap: return "맵 없음";
        case CmdOutcome::Unknown: return "모르는 명령";
        case CmdOutcome::kCount: break;
    }
    return "?";
}

// ★ 10-16 플랜 B 작업 12b — 채팅 명령. 알림 문구는 영문(정품 글꼴 · 코드페이지 걱정 없이 — 방 이름을 영문으로 바꾼 10-16 과 같은 까닭)
void nox::CRoom::HandleChatCommands() {
    _game.SwapOutChatCommands(_cmdBuf);
    for (const Server::ChatCommand& c : _cmdBuf) {
        // 첫 낱말만 · 소문자
        std::string word;
        for (const char ch : c.text) {
            if (ch == ' ') break;
            word.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        CmdOutcome r = CmdOutcome::Unknown;
        if (word == "!start") {
            const RoomMember* h = Host();
            if (!h || h->sessionId != c.sessionId) r = CmdOutcome::NotHost;
            else if (_phase != RoomPhase::Waiting) r = CmdOutcome::NotWaiting;
            else if (_game.inGameCount() < kMinPlayers) r = CmdOutcome::TooFew;
            else if (!CanStartMatch()) r = CmdOutcome::NoMap;   // ★ 13b — 대전 맵 · 대기 맵 원본이 맵 목록에 있어야
            else r = CmdOutcome::Started;
        }
        ++_cmd[static_cast<size_t>(r)];
        char line[160];
        switch (r) {
            case CmdOutcome::Started:
                _game.SendNotice("Match starting!");
                StartMatch();   // ★ 13b — 대전 맵으로(접속 유지) · 판
                break;
            case CmdOutcome::NoMap:
                _game.SendNoticeTo(c.sessionId, "No battle map on this server.");
                break;
            case CmdOutcome::NotHost: {
                const RoomMember* h = Host();
                std::snprintf(line, sizeof(line), "Only the host (%s) can start.", h && !h->account.empty() ? h->account.c_str() : "?");
                _game.SendNoticeTo(c.sessionId, line);
                break;
            }
            case CmdOutcome::NotWaiting:
                _game.SendNoticeTo(c.sessionId, "A match is already in progress.");
                break;
            case CmdOutcome::TooFew:
                std::snprintf(line, sizeof(line), "Need at least %zu players to start.", kMinPlayers);
                _game.SendNoticeTo(c.sessionId, line);
                break;
            case CmdOutcome::Unknown:
                _game.SendNoticeTo(c.sessionId, "Unknown command. Host: !start");
                break;
            case CmdOutcome::kCount:
                break;
        }
        _game.Log("[명령] %s — 세션 %lld · %s · 상태 %s · 게임 안 %zu\n", word.c_str(), static_cast<long long>(c.sessionId), CmdOutcomeName(r),
                  RoomPhaseName(_phase), _game.inGameCount());
    }
    _cmdBuf.clear();
}

// ★ 10-16 플랜 B 작업 13b — 판 열기 · 판 끝 뒤 대기로(`Docs/ROOMS_DESIGN.md` 결정 1 · 「10-16 조사」)
std::shared_ptr<const MapOriginal> nox::CRoom::FindMap(const std::string& path) const {
    if (!_maps || path.empty()) return nullptr;
    return _maps->Find(MapKeyOf(path));
}

void nox::CRoom::StartMatch() {
    const std::shared_ptr<const MapOriginal> battle = FindMap(_settings.battleMapPath);
    const std::shared_ptr<const MapOriginal> wait = FindMap(_settings.mapPath);
    if (!battle || !wait) return;   // 부르는 쪽이 `CanStartMatch` 로 걸렀다
    // ★ 10-16 작업 13d — 시험 손잡이 — 점수 한도(전환이 보내는 `af` 에 실리게 먼저). 대기 맵으로 돌아가도 그대로 둔다(채팅 모드는 점수가 안 오른다 — 13c)
    if (_settings.battleScoreLimit > 0) _game.SetLimitValue(static_cast<uint8_t>(std::min(_settings.battleScoreLimit, 255)));
    _game.SwitchMap(battle->tables, battle->initial, nox::ModeGameFlags(_settings.battleMode));
    _game.SetNextRoundMap(wait->tables, wait->initial, nox::ModeGameFlags(_settings.mode));   // 판 끝 → 대기 맵(판 끝 A)
    _curMapKey = battle->key;
    _curMode = _settings.battleMode;
    _phase = RoomPhase::Playing;
    // ★ 10-19 MO 5단계 작업 2 — 판 결과용: 판 번호 · 시작 프레임(프레임은 맵을 바꿔도 안 되돌아감) · 시각 · 명단(게임 안 사람 + 로비 이름)
    ++_matchNo;
    _matchStartFrame = _game.frame();
    _matchStartUnix = _unixNow();
    _matchMapKey = battle->key;
    _matchMode = _settings.battleMode;
    _roster.clear();
    std::vector<RoundStanding> ps;
    std::vector<RoundTeamScore> ts;
    _game.SnapshotStandings(ps, ts);
    for (const RoundStanding& p : ps) {
        RosterEntry e;
        e.sessionId = p.sessionId;
        e.netCode = p.netCode;
        for (const RoomMember& m : _members) if (m.sessionId == p.sessionId) e.account = m.account;
        _roster.push_back(std::move(e));
    }
    _game.Log("[판] 시작 — 대전 맵 %s(%s) · 끝나면 대기 맵 %s · 사람 %zu\n", battle->key.c_str(), _settings.battleMode.c_str(), wait->key.c_str(),
              _game.inGameCount());
}

// ★ 10-19 MO 5단계 작업 2 — 머리 주석은 `RoomManager.h` `CollectRoundOutcome`. 방 관리자가 틱 뒤(판 상태를 바꾸기 전에) 부른다
bool nox::CRoom::CollectRoundOutcome() {
    const unsigned long long seq = _game.RoundOutcomeSeq();
    if (seq == _seenOutcomeSeq) return false;
    if (seq - _seenOutcomeSeq > 1) _outcomesSkipped += seq - _seenOutcomeSeq - 1;   // 한 칸이라 앞 것은 덮였다
    _seenOutcomeSeq = seq;
    switch (_phase) {
        case RoomPhase::Waiting:
            ++_waitingRoundEnds;   // 대기 맵(채팅 모드)의 판 끝 — 판 결과가 아니다
            return true;
        case RoomPhase::Playing:
            break;
    }
    PushMatchResult(false, _game.LastRoundOutcome());
    return true;
}

// 판 결과 하나 — 명단 차례로 사람마다 끝에 게임 안에 있었나(점수) · 없었나(나감), 명단 밖인데 게임 안에 있던 사람은 뒤에
void nox::CRoom::PushMatchResult(bool aborted, const RoundOutcome& o) {
    MatchResult r;
    r.epoch = _epoch;
    r.roomId = _id;
    r.matchNo = _matchNo;
    r.roomName = _settings.roomName;
    r.mapKey = _matchMapKey;
    r.mode = _matchMode;
    r.startUnix = _matchStartUnix;
    r.endUnix = _unixNow();
    r.frames = o.frame - _matchStartFrame;
    r.aborted = aborted;
    r.reason = o.reason;
    r.winner = aborted ? RoundWinnerKind::None : o.winner;
    r.winnerId = aborted ? 0 : o.winnerId;
    r.limit = o.limit;
    r.timeLimitFrames = o.timeLimitFrames;
    r.teams = o.teams;
    std::vector<bool> used(o.players.size(), false);
    for (const RosterEntry& e : _roster) {
        MatchPlayer p;
        p.account = e.account;
        p.sessionId = e.sessionId;
        p.netCode = e.netCode;
        p.left = true;
        for (size_t i = 0; i < o.players.size(); ++i) {
            if (used[i] || o.players[i].sessionId != e.sessionId) continue;
            used[i] = true;
            p.left = false;
            p.team = o.players[i].team;
            p.score = o.players[i].score;
            p.deaths = o.players[i].deaths;
            p.observer = o.players[i].observer;
        }
        r.players.push_back(std::move(p));
    }
    for (size_t i = 0; i < o.players.size(); ++i) {
        if (used[i]) continue;
        const RoundStanding& s = o.players[i];
        MatchPlayer p;
        for (const RoomMember& m : _members) if (m.sessionId == s.sessionId) p.account = m.account;
        p.sessionId = s.sessionId;
        p.netCode = s.netCode;
        p.team = s.team;
        p.score = s.score;
        p.deaths = s.deaths;
        p.observer = s.observer;
        r.players.push_back(std::move(p));
    }
    if (r.winner == RoundWinnerKind::Player)
        for (const MatchPlayer& p : r.players)
            if (p.netCode == r.winnerId && !p.left) r.winnerAccount = p.account;
    ++_matchResultsMade;
    _results.push_back(std::move(r));
}

void nox::CRoom::OnRoundRestarted() {
    switch (_phase) {
        case RoomPhase::Waiting: return;   // 대기 중에 판 끝이 났다(점수 한도 등) — 게임이 같은 맵을 다시 열었다. 방 상태는 그대로
        case RoomPhase::Playing: break;
    }
    // 게임이 걸어 둔 다음 판 맵(대기 맵)으로 이미 바꿨다(`Server` 판 다시 열기 자리) — 방 상태만 대기로
    _phase = RoomPhase::Waiting;
    _curMapKey = _settings.mapPath.empty() ? std::string() : MapKeyOf(_settings.mapPath);
    _curMode = _settings.mode;
    _game.SendNotice("Back to the waiting room. Host: !start");
    _game.Log("[판] 끝 — 대기 맵 %s 로 돌아옴(지금 맵 %s)\n", _curMapKey.c_str(), _game.MapFileName().c_str());
}

// ★ 10-16 결함 ④ — 머리 주석은 `RoomManager.h` `CheckMatchHeadcount`. 방 관리자가 틱 뒤(나감을 걷어 낸 뒤 · 판 끝 처리 뒤) 부른다
void nox::CRoom::CheckMatchHeadcount() {
    switch (_phase) {
        case RoomPhase::Waiting: return;
        case RoomPhase::Playing: break;
    }
    if (_game.RoundOver()) return;                          // 판 끝 쉬는 중 — 다시 열 때 걸어 둔 대기 맵으로 간다(`OnRoundRestarted`)
    const size_t left = _game.inGameCount();
    if (left >= kMinPlayers) return;
    const std::shared_ptr<const MapOriginal> wait = FindMap(_settings.mapPath);
    if (!wait) return;                                      // 판을 열 때 `CanStartMatch` 가 봤다 — 여기 올 일 없음
    // ★ 10-19 MO 5단계 작업 2 — 중단 결과는 대기 맵으로 바꾸기 **앞에서**(바꾸면 `ReopenMap` → `RestartPlayers` 가 점수를 지운다)
    {
        RoundOutcome now;
        now.frame = _game.frame();
        now.limit = _game.Settings().limitValue;
        now.timeLimitFrames = _game.Settings().timeLimitFrames;
        _game.SnapshotStandings(now.players, now.teams);
        PushMatchResult(true, now);
    }
    _game.SwitchMap(wait->tables, wait->initial, nox::ModeGameFlags(_settings.mode));
    _game.ClearNextRoundMap();                              // 그 맵으로 이미 왔다 — 다음 판 끝에 또 바꾸지 않게
    _phase = RoomPhase::Waiting;
    _curMapKey = wait->key;
    _curMode = _settings.mode;
    ++_matchAborts;
    _game.SendNotice("Not enough players. Back to the waiting room. Host: !start");
    _game.Log("[판] 사람 모자람(게임 안 %zu < %zu) — 대기 맵 %s 로\n", left, kMinPlayers, wait->key.c_str());
}
