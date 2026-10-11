// WorldBuild.cpp — 세계 차리기(맵·벽·물체·몬스터·소리·접속 묶음·서버 설정).
//
// ★ 10-08 — `main()` 안에 있던 차리기를 **글자 그대로** 옮겼다(`Docs/LAYERS_DESIGN.md` 조각 1).
//   옮긴 몸통은 `git diff --color-moved` 로 그대로인 것을 확인할 수 있다. 바뀐 것은 둘뿐이다 —
//     ① 실행 인자를 `ServerOptions` 에서 같은 이름의 참조로 받는다(아래 함수 머리)
//     ② `--dump-sections`(섹션만 찍고 끝내는 진단)는 서버를 안 띄우는 일이라 `main` 에 남겼다
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
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
#include "nox/DumpSniff.h"
#include "nox/MsgLen.h"
#include "nox/Recording.h"
#include "nox/ModeRules.h"
#include "nox/Server.h"
#include "nox/MapFile.h"
#include "nox/ThingDb.h"
#include "nox/SpellNames.h"
#ifdef DecryptFile          // windows.h 가 DecryptFile → DecryptFileA 로 바꾼다(링크 오류 LNK2019)
#undef DecryptFile
#endif
#include "nox/NoxCrypt.h"   // ★ 09-27 B3 — modifier.bin 복호

#include "WorldBuild.h"

namespace {

// 재생 조각들이 무슨 메시지로 이뤄졌는지 센다. 이분법에 들어가기 전에 지형부터 본다.
void MirrorStat(const nox::Server& srv, const std::vector<std::vector<uint8_t>>& v) {   // ★ 10-12 작업 7b — 줄은 방 로그로
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
    srv.Log("[재생 조각] %zu개 · 못 쪼갠 조각 %d개\n", v.size(), unparsed);
    for (const auto& kv : hist)
        srv.Log("   0x%02x %-30s %d\n", kv.first,
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

}  // namespace

int BuildRoomWorld(nox::Server& srv, const ServerOptions& opt, const OpenSessionFn& openSession, WorldExtras& out) {
    // 실행 인자 — 옮긴 몸통이 쓰는 것만 같은 이름으로 받는다.
    const auto& mirror = opt.mirror;
    const auto& mirrorLimit = opt.mirrorLimit;
    const auto& prelude = opt.prelude;
    const auto& noUpdate = opt.noUpdate;
    const auto& rawSnapshot = opt.rawSnapshot;
    const auto& noNewGear = opt.noNewGear;
    const auto& noSnapshot = opt.noSnapshot;
    const auto& noSpawnInvuln = opt.noSpawnInvuln;
    const auto& hudOff = opt.hudOff;
    const auto& peerOps = opt.peerOps;
    const auto& introEveryTick = opt.introEveryTick;
    const auto& spawnSpread = opt.spawnSpread;
    const auto& peerFirst = opt.peerFirst;
    const auto& peerSnapshot = opt.peerSnapshot;
    const auto& peerType = opt.peerType;
    const auto& peerPose = opt.peerPose;
    const auto& mode = opt.mode;
    const auto& mapPath = opt.mapPath;
    const auto& standalone = opt.standalone;
    const auto& aoi = opt.aoi;
    const auto& creatureAoiHalf = opt.creatureAoiHalf;
    const auto& creatureAwayAfter = opt.creatureAwayAfter;
    const auto& friends = opt.friends;
    const auto& debugDummy = opt.debugDummy;
    const auto& debugDummyDx = opt.debugDummyDx;
    const auto& debugEnchantB = opt.debugEnchantB;
    const auto& debugEnchantP = opt.debugEnchantP;
    const auto& debugEnchantN = opt.debugEnchantN;
    const auto& debugHurtEvery = opt.debugHurtEvery;
    const auto& debugHurtFront = opt.debugHurtFront;
    const auto& autoRespawn = opt.autoRespawn;
    const auto& debugOwnPet = opt.debugOwnPet;
    const auto& debugPetOrder = opt.debugPetOrder;
    const auto& debugMobAfter = opt.debugMobAfter;
    const auto& debugMobType = opt.debugMobType;
    const auto& debugMobX = opt.debugMobX;
    const auto& debugMobY = opt.debugMobY;
    const auto& debugTeamAfter = opt.debugTeamAfter;
    const auto& debugTeamEvery = opt.debugTeamEvery;
    const auto& debugFistAfter = opt.debugFistAfter;
    const auto& debugFistDx = opt.debugFistDx;
    const auto& debugFistDy = opt.debugFistDy;
    const auto& debugWarpAfter = opt.debugWarpAfter;
    const auto& debugWarpX = opt.debugWarpX;
    const auto& debugWarpY = opt.debugWarpY;
    const auto& debugWallAfter = opt.debugWallAfter;
    const auto& debugWallGx = opt.debugWallGx;
    const auto& debugWallGy = opt.debugWallGy;
    const auto& debugKillAfter = opt.debugKillAfter;
    const auto& debugScoreAfter = opt.debugScoreAfter;
    const auto& debugScoreVal = opt.debugScoreVal;
    const auto& spawnAtX = opt.spawnAtX;
    const auto& spawnAtY = opt.spawnAtY;
    const auto& bots = opt.bots;
    const auto& debugXpAfter = opt.debugXpAfter;
    const auto& debugXpAmount = opt.debugXpAmount;
    const auto& debugSpellAfter = opt.debugSpellAfter;
    const auto& debugSpellId = opt.debugSpellId;
    const auto& debugConjurerGuides = opt.debugConjurerGuides;
    const auto& noObstacles = opt.noObstacles;
    const auto& itemAtSpawn = opt.itemAtSpawn;
    const auto& spawnAtItem = opt.spawnAtItem;
    const auto& giveItemNames = opt.giveItemNames;
    const auto& startHealth = opt.startHealth;
    const auto& startMana = opt.startMana;
    const auto& forceDir = opt.forceDir;
    const auto& forceArrowFrame = opt.forceArrowFrame;
    const auto& arrowFreeze = opt.arrowFreeze;
    const auto& noSecondNewPlayer = opt.noSecondNewPlayer;
    const auto& secondNewPlayer = opt.secondNewPlayer;
    const auto& forceHarpoon = opt.forceHarpoon;
    const auto& dumpHarpoon = opt.dumpHarpoon;
    const auto& arrowFrameAlt = opt.arrowFrameAlt;
    const auto& forcePose = opt.forcePose;
    const auto& spellStart = opt.spellStart;
    std::string dumpPath = opt.dumpPath;   // 비었으면 아래에서 기본 덤프 자리로 채운다
    // ★ 09-20 9-b — 덫 자리를 바깥에 둔다. 길목을 읽은 **뒤**에 「덫 옆 설 자리」를 찍기 위함
    std::vector<std::pair<float, float>> trapPos;
    auto& mapFlagPos = out.mapFlagPos;

    if (!mapPath.empty()) {
        nox::MapFile mf;
        if (!mf.Load(mapPath)) {
            srv.Log("맵을 못 읽음: %s (%s)\n", mapPath.c_str(), mf.Error().c_str());
            return 1;
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
            srv.Log("[맵] 비밀벽 %zu개 중 벽이 있는 칸 %zu개\n", sec.size(), srv.SecretWallCount());
            // ★ 09-20 9-a — **세상 좌표**로 앞 8곳을 찍는다. `--spawn-at`·`--debug-warp` 에 넣을 값이다
            //   (칸 → 세상 = 칸 × 23 + 11.5 · `Server.cpp:8946` 과 같은 셈)
            if (!sec.empty()) {
                srv.Log("[비밀벽 자리]");
                for (size_t k = 0; k < sec.size() && k < 8; ++k)
                    srv.Log(" (%d,%d 칸%d,%d)", static_cast<int>(sec[k].gx * 23 + 11),
                                static_cast<int>(sec[k].gy * 23 + 11),
                                static_cast<int>(sec[k].gx), static_cast<int>(sec[k].gy));
                srv.Log("  — 모두 %zu곳\n", sec.size());
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
                srv.Log("[비밀벽 깃발] 4(스스로 움직임) %zu · 8(열린 채 시작) %zu · 둘 다 %zu · 지금 열림 %zu\n",
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
                srv.Log("[벽] 모양 %zu칸 · 재질 %zu종 · 방향 표 밖 %zu칸 · 광전사 세게(깃발&5) 재질 %zu종·%zu칸\n",
                            cells.size(), flags.size(), badDir, hardDefs, hardCells);
                // ★ 09-29 Z1-A — 벽 정의의 부수기 칸(맞는 소리 재질·체력·부서지는 소리) + 맵의 부서지는 벽·창문 칸
                {
                    std::vector<nox::Server::WallDefExtra> ex;
                    size_t withHp = 0, noSnd = 0;
                    for (const auto& d : wdb.Walls()) {
                        nox::Server::WallDefExtra e;
                        e.hitMat = d.hitMat;
                        e.health = d.health;
                        e.breakSound = d.breakSound.empty() ? 0 : nox::SoundByName(d.breakSound.c_str());
                        if (!d.breakSound.empty() && e.breakSound == 0) ++noSnd;
                        if (d.health > 0) ++withHp;
                        ex.push_back(e);
                    }
                    srv.SetWallDefExtras(std::move(ex));
                    const auto destr = mf.ReadGridList("DestructableWalls", 1);
                    const auto win = mf.ReadGridList("WindowWalls", 2);
                    srv.SetWallSpecials(destr, win);
                    srv.Log("[벽 부수기] 정의 체력 있는 것 %zu/%zu종 · 부서지는 소리 이름 못 찾음 %zu · 맵 부서지는 벽 %zu칸 · 창문 %zu칸\n",
                                withHp, wdb.Walls().size(), noSnd, destr.size(), win.size());
                }
            } else {
                srv.Log("[벽] 모양 %zu칸 · ⚠ thing.bin 을 못 읽어 재질 깃발 없음(모두 막는 벽으로 봄)\n", cells.size());
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
        size_t decayExemptKinds[4] = {};        // ★ 09-28 — 바닥 물건의 「10초 규칙 예외」 까닭별(없음·RESPAWN·FLAG·Glyph)
        size_t dropFnKinds[5] = {};             // ★ 09-28 — 바닥 물건의 버리기 함수 갈래별(Default·갑옷무기·물약·음식·안옮김)
        std::set<uint16_t> giveTemplated;       // ★ 09-30 — `--give-items` 틀을 뜬 종류
        size_t ammoRead = 0, ammoBad = 0, ammoSum = 0, ammoMaxSum = 0, ammoRest = 0;   // ★ 09-29 — 화살통 짐: 읽음 · 못 읽음(0/0) · 화살 합 · 최대 합 · 짐 뒤 남은 바이트 합
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
                srv.Log("[종류] %s = %u%s\n", nm, static_cast<unsigned>(w), w ? "" : " ⚠ 못 찾음");
            }
            // ★ 09-23 nine-defects 4-d1 — 바닥 정의(FLOR)의 재질 분포. 발소리가 이 값으로 갈린다(`object_update.go:164~220`)
            {
                std::map<int32_t, int> byMat;
                for (const auto& f : tdb.Floors()) ++byMat[f.material];
                srv.Log("[바닥] 정의 %zu개 · 재질별:", tdb.Floors().size());
                for (const auto& kv : byMat) srv.Log(" 0x%x×%d", static_cast<unsigned>(kv.first), kv.second);
                srv.Log("\n");
            }
            // ★ 4-c3 — 주문이 **날리는** 것(불덩이 셋). 빠르기·깃발·닿음 인자를 전부 thing.bin 에서 읽는다.
            //   `COLLIDE = SparkExplosionCollide 192` 의 숫자가 터짐 크기를 다 정하므로 **짐작하지 않는다**.
            //   ★ 묶음 6-g(3b) — **크리처가 쏘는 것**도 같은 자리에서 읽는다.
            //     `ImpShot` 은 `SPEED = 400` · `COLLIDE = ProjectileCollide 5` 다
            //     ★ 묶음 9-b — **쏘는 덫이 쏘는 것**도 여기서 읽는다.
            //       `MercArcherArrow` 는 `SPEED = 800` · `COLLIDE = MonsterArrowCollide 10 10` 이다
            //     ★ 09-29 — **사람이 쏘는 화살**(활 ArcherArrow · 석궁 ArcherBolt · 화살통 없는 WeakArcherArrow — `shootBowCrossbow2_539D80`)
            for (const char* nm : { "Fireball", "StrongFireball", "TitanFireball", "ImpShot", "HarpoonBolt",
                                    "MercArcherArrow", "ArcherArrow", "ArcherBolt", "WeakArcherArrow" }) {
                const uint16_t w = tdb.WireByName(nm);
                const nox::ThingInfo* t2 = w ? tdb.ByWire(w) : nullptr;
                int amount = 0;
                if (t2 && !t2->collideArgs.empty()) amount = std::atoi(t2->collideArgs.c_str());
                if (t2) srv.SetProjType(nm, w, t2->speed, nox::ParseObjectFlags(t2->flagsRaw),
                                        static_cast<uint8_t>(amount < 0 ? 0 : (amount > 255 ? 255 : amount)));
                srv.Log("[날리기 종류] %s = %u · SPEED %d · 닿음 %s %d · 그리기 %s%s\n", nm, static_cast<unsigned>(w),
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
                srv.Log("[화살 종류] %zu가지 (프레임 바이트가 붙는다)%s%s\n", nArrow,
                            names.empty() ? "" : " — ", names.c_str());
            }
            std::vector<nox::Server::WorldStatic> st;
            std::vector<nox::Server::WorldProp> props;
            size_t propExtentTooBig = 0;   // ⑩ extent 가 15비트를 넘는 것(정적 조회로 못 가리킨다)
            size_t doorsSeen = 0, doorsLocked = 0, doorsUnread = 0;   // ★ 09-29 Z1-A 문 · 그중 잠긴 것(광전사가 부딪히는 문) · 못 읽은 것
            size_t doorLockKinds[7] = {};
            size_t doorsExtra = 0, doorsTeam = 0, doorsAngleBad = 0, doorsAngleFixed = 0, doorsRestOdd = 0, doorsClosedDiff = 0;
            const auto objs = mf.ReadObjects();
            if (!mf.Error().empty())
                srv.Log("[맵] ⚠ 오브젝트 읽기가 도중에 멈춤: %s — 읽은 %zu개만 쓴다\n", mf.Error().c_str(), objs.size());
            // ★ 2026-09-13 묶음 3-c — 부활 자리(PlayerStart). 정답지 mapFindPlayerStart_4F7AB0 가 이 목록에서 고른다.
            {
                std::vector<std::pair<float, float>> starts;
                for (const auto& o : objs)
                    if (o.type == "PlayerStart") starts.push_back({ o.x, o.y });
                srv.Log("[부활 자리] PlayerStart %zu곳 —", starts.size());
                // ★ 09-20 — **좌표도 찍는다.** 화면 판정에서 「방 안에 선 자리」가 늘 필요한데
                //   아무 데나 `--spawn-at` 하면 **바닥이 없는 암반**에 서서 화면이 통째로 검다(실측).
                //   PlayerStart 는 맵이 보증하는 「설 수 있는 자리」다
                for (size_t k = 0; k < starts.size() && k < 14; ++k)
                    srv.Log(" (%d,%d)", static_cast<int>(starts[k].first), static_cast<int>(starts[k].second));
                srv.Log("\n");
                // ★ 09-28 — 부서지는 벽·창문 칸 수와, 부활 자리마다 **가장 가까운 부서지는 벽**(세상 좌표 = 칸 × 23 + 11).
                //   정품 서버 판에서 벽 부서짐 와이어를 녹음하려면 사람이 그 벽을 찾아가야 한다(Z1-A 준비)
                {
                    const std::string errBefore = mf.Error();
                    const auto destr = mf.ReadGridList("DestructableWalls", 1);
                    const auto win = mf.ReadGridList("WindowWalls", 2);
                    if (mf.Error() != errBefore) srv.Log("[벽 칸 읽기] ⚠ %s\n", mf.Error().c_str());
                    srv.Log("[부서지는벽] %zu칸 · [창문벽] %zu칸", destr.size(), win.size());
                    if (!destr.empty()) {
                        srv.Log(" · 부활 자리마다 가장 가까운 부서지는 벽:");
                        for (size_t k = 0; k < starts.size() && k < 8; ++k) {
                            double best = 1e18; int bx = 0, by = 0;
                            for (const auto& g : destr) {
                                const double wx = g.first * 23.0 + 11.0, wy = g.second * 23.0 + 11.0;
                                const double d2 = (wx - starts[k].first) * (wx - starts[k].first) + (wy - starts[k].second) * (wy - starts[k].second);
                                if (d2 < best) { best = d2; bx = static_cast<int>(wx); by = static_cast<int>(wy); }
                            }
                            srv.Log(" [%d,%d → (%d,%d) %.0f]", static_cast<int>(starts[k].first), static_cast<int>(starts[k].second),
                                        bx, by, std::sqrt(best));
                        }
                    }
                    srv.Log("\n");
                }
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
                        srv.Log("[소리·메움] 230(ManaRecharge) — thing.bin 에 없어 손으로 채움"
                                    "(정품 와이어 근거 · 거리 600 은 정답지 기본값)\n");
                    }
                }
                srv.SetSoundDefs(sd);
                srv.Log("[소리] AUD %zu개 · 이름표에 맞물림 %zu · 소리조각 있는 것 %zu%s\n",
                            tdb.Sounds().size(), sd.size(), withSamples,
                            miss ? " ⚠ 이름표에 없는 것 있음" : "");
                // ★ 09-29 B6 — 몬스터가 듣는 소리는 깃발이 0 이 아닌 것뿐이다(`unit_ai.go:290` NewSound) — 0x10 아군 것도 · 0x20 걷기(문턱 89) · 0x40 달리기(문턱 20)
                {
                    size_t nf = 0, f10 = 0, f20 = 0, f40 = 0;
                    for (const auto& in : sd) {
                        if (in.flags == 0) continue;
                        ++nf; if (in.flags & 0x10) ++f10; if (in.flags & 0x20) ++f20; if (in.flags & 0x40) ++f40;
                        if (std::getenv("NOX_DUMP_SOUND_FLAGS")) srv.Log("  [소리깃발] %u %s 깃발 0x%x 거리 %d\n", (unsigned)in.id, nox::SoundName(in.id), (unsigned)in.flags, in.maxDist);
                    }
                    srv.Log("[소리듣기] 깃발 있는 소리 %zu개 — 0x10 %zu · 0x20 %zu · 0x40 %zu\n", nf, f10, f20, f40);
                }
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
                    srv.Log("[소리·정품이쓴것] %zu개 중 우리 표에 **없는 것**:%s\n",
                                sizeof(seen) / sizeof(seen[0]), missing.empty() ? " (none)" : missing.c_str());
                }
                // ★ 09-22 사용자 보고(「소리가 안 들린다」) — **우리가 부르는 번호가 표에 있나**를 찍는다.
                //   `PlaySoundAt` 은 소리조각이 0 이면 조용히 버리고, `maxDist` 가 0 이면 **거리에서 잘린다**.
                //   둘은 증상이 같아서(안 들림) 숫자로 갈라야 한다
                // ★ 09-23 nine-defects 4-b — **능력 소리 170~185**(쓸 때·켜짐·꺼질 때 × 다섯)를 한 줄로. 「조각 0 으로 선언」과
                //   「표에 아예 없음」(230 같은 경우 — 정품이 따로 보낼 수 있다)을 가른다. `-` = 표에 없음
                {
                    srv.Log("[소리·능력]");
                    for (uint16_t q = 170; q <= 185; ++q) {
                        int samples = -1;
                        for (const auto& in : sd) if (in.id == q) { samples = in.samples; break; }
                        if (samples < 0) srv.Log(" %u(%s)-", (unsigned)q, nox::SoundName(q));
                        else srv.Log(" %u(%s)%d", (unsigned)q, nox::SoundName(q), samples);
                    }
                    srv.Log("\n");
                }
                // ★ 09-23 nine-defects 4-c2 — 타격 소리 848~872(`<때린 것>Hit<맞는 쪽>`)의 조각 수 — 0 이면 불러도 버려진다
                {
                    srv.Log("[소리·타격]");
                    size_t zero = 0;
                    for (uint16_t q = 848; q <= 872; ++q) {
                        int samples = -1;
                        for (const auto& in : sd) if (in.id == q) { samples = in.samples; break; }
                        if (samples <= 0) { ++zero; srv.Log(" %u(%s)%s", (unsigned)q, nox::SoundName(q), samples < 0 ? "-" : "0"); }
                    }
                    srv.Log(" · 조각 0 또는 없음 %zu / 25\n", zero);
                }
                // ★ 09-23 nine-defects 4-c3b3a — soundset.bin(암호 5)의 몬스터 소리 모음. 때린 이가 몬스터면 그 모음의 「맞히는 소리」
                //   (MELEE_ATTACK_HITTING_SOUND · +32)에 조각이 있을 때 맞는 쪽 소리를 안 낸다(`GAME3_2.c:7252~7254` — 4-c3b3b 에서 쓴다).
                //   여기서는 읽히는지·몇 벌인지·부를 수 있는 소환수 열셋의 그 소리와 조각 수를 찍는다
                {
                    std::vector<nox::SoundSet> sets;
                    std::string err;
                    const bool ok = nox::LoadSoundSets("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\soundset.bin", sets, &err);
                    auto samplesOf = [&](const std::string& nm) {
                        const uint16_t id = nm.empty() ? 0 : nox::SoundByName(nm.c_str());
                        if (id == 0) return -1;
                        for (const auto& in : sd) if (in.id == id) return in.samples;
                        return -1;
                    };
                    size_t written = 0, withSamples2 = 0, nameless = 0;
                    for (const auto& s : sets) {
                        const std::string& h = s.sound[nox::kSoundSetMeleeHitting];
                        if (h.empty() || h == "NULL") continue;
                        ++written;
                        if (nox::SoundByName(h.c_str()) == 0) ++nameless;
                        if (samplesOf(h) > 0) ++withSamples2;
                    }
                    srv.Log("[소리모음] %s · %zu벌 · 맞히는 소리 적힌 것 %zu(조각 있음 %zu · 이름표에 없음 %zu)",
                                ok ? "읽음" : ("⚠ 못 읽음 " + err).c_str(), sets.size(), written, withSamples2, nameless);
                    srv.Log(" · 소환수:");
                    for (const auto& ms : nox::kMonsters) {
                        const nox::SoundSet* s = nox::FindSoundSet(sets, ms.name);
                        if (!s) { srv.Log(" %s=모음없음", ms.name); continue; }
                        const std::string& h = s->sound[nox::kSoundSetMeleeHitting];
                        srv.Log(" %s=%s(%d)", ms.name, h.empty() ? "(안 적힘)" : h.c_str(), samplesOf(h));
                    }
                    // ★ 4-c3b3b — 종류 번호별 「그 이름의 모음에 맞히는 소리가 있고 조각 1 이상」 표 → 서버(몬스터가 때릴 때 맞는 쪽 소리를 가른다)
                    std::vector<uint8_t> owns(tdb.Count() + 1, 0);
                    size_t nOwn = 0;
                    for (size_t w = 1; w <= tdb.Count(); ++w) {
                        const nox::ThingInfo* t = tdb.ByWire(static_cast<uint16_t>(w));
                        const nox::SoundSet* s = t ? nox::FindSoundSet(sets, t->name) : nullptr;
                        if (s && samplesOf(s->sound[nox::kSoundSetMeleeHitting]) > 0) { owns[w] = 1; ++nOwn; }
                    }
                    srv.Log(" · 종류 %zu개가 맞히는 소리를 가짐\n", nOwn);
                    srv.SetMonsterHitSounds(std::move(owns));
                    // ★ 09-24 H1 — 같은 칸의 **번호** 표(몬스터가 근접으로 때린 뒤 그 소리를 낸다 · `GAME4_3.c:2087~2094`)
                    std::vector<uint16_t> hitIds(tdb.Count() + 1, 0);
                    size_t nId = 0;
                    for (size_t w = 1; w <= tdb.Count(); ++w) {
                        const nox::ThingInfo* t = tdb.ByWire(static_cast<uint16_t>(w));
                        const nox::SoundSet* s = t ? nox::FindSoundSet(sets, t->name) : nullptr;
                        const std::string& h = s ? s->sound[nox::kSoundSetMeleeHitting] : std::string();
                        if (h.empty() || h == "NULL") continue;
                        hitIds[w] = nox::SoundByName(h.c_str());
                        if (hitIds[w] != 0) ++nId;
                    }
                    srv.Log("[소리모음] 근접 맞히는 소리 번호 있는 종류 %zu개 · Bat=%u AlbinoSpider=%u SmallSpider=%u\n", nId,
                                (unsigned)hitIds[tdb.WireByName("Bat")], (unsigned)hitIds[tdb.WireByName("AlbinoSpider")],
                                (unsigned)hitIds[tdb.WireByName("SmallSpider")]);
                    srv.SetMonsterMeleeHitSoundIds(std::move(hitIds));
                    // ★ 09-26 M4a — 같은 꼴로 칸 넷 더: 근접 시작 5 · 쏘기 시작 9 · 쏘기 10 · 죽음 14(`kSoundSetFieldNames` 순서)
                    // ★ 09-29 B5 — **칸 전부**(맞히는 소리 7 은 위에서). ✖ 전에는 5·9·10·14·16 만 실어서 B2~B4a 가 부르는 칸
                    //   (도발 1 · 놀 때 3 · 교전 4 · 빗나감 8 · 도망 11 · 후퇴 12 · 다침 15)과 걸음 17 이 **실서버에서 한 번도 안 났다**
                    //   (시험은 「났음+번호 없음」을 합쳐 세어 못 잡았다 — 시험 표에는 번호를 안 싣기 때문)
                    srv.Log("[소리모음] 칸별 번호 있는 종류 —");
                    for (int field = 0; field < nox::kSoundSetFields; ++field) {
                        if (field == nox::kSoundSetMeleeHitting) continue;
                        std::vector<uint16_t> ids(tdb.Count() + 1, 0);
                        size_t n = 0;
                        for (size_t w = 1; w <= tdb.Count(); ++w) {
                            const nox::ThingInfo* t = tdb.ByWire(static_cast<uint16_t>(w));
                            const nox::SoundSet* s = t ? nox::FindSoundSet(sets, t->name) : nullptr;
                            const std::string& h = s ? s->sound[field] : std::string();
                            if (h.empty() || h == "NULL") continue;
                            ids[w] = nox::SoundByName(h.c_str());
                            if (ids[w] != 0) ++n;
                        }
                        srv.Log(" %s %zu", nox::kSoundSetFieldNames[field], n);
                        srv.SetMonsterSoundIds(field, std::move(ids));
                    }
                    srv.Log("\n");
                }
                // ★ 09-23 nine-defects — 새로 부르게 된 번호도 같이 본다: 171 BerserkerCrash(2-b2) · 힘쓰는 소리 남·여(3-a) · 휘두르기 879~884(4-a)
                for (uint16_t q : { (uint16_t)230, (uint16_t)237, (uint16_t)239, (uint16_t)307, (uint16_t)171,
                                    nox::SoundByName("HumanMaleExertionHeavy"), nox::SoundByName("HumanFemaleExertionHeavy"),
                                    (uint16_t)879, (uint16_t)880, (uint16_t)881, (uint16_t)882, (uint16_t)883, (uint16_t)884 }) {
                    bool found = false;
                    for (const auto& in : sd)
                        if (in.id == q) {
                            srv.Log("[소리·쓰는것] %u(%s) — 조각 %d · 들리는거리 %d%s\n",
                                        (unsigned)q, nox::SoundName(q), in.samples, in.maxDist,
                                        (in.samples <= 0 || in.maxDist <= 0) ? "  ⚠ 이래서 안 들린다" : "");
                            found = true; break;
                        }
                    if (!found) {
                        srv.Log("[소리·쓰는것] %u(%s) — ⚠ **표에 아예 없다**\n", (unsigned)q, nox::SoundName(q));
                        // 이름이 다를 뿐일 수 있다 — 비슷한 이름을 같이 찍어 고를 수 있게 한다
                        srv.Log("[소리·비슷한이름]");
                        for (const auto& d : tdb.Sounds())
                            if (d.name.find("Mana") != std::string::npos ||
                                d.name.find("Recharge") != std::string::npos ||
                                d.name.find("Obelisk") != std::string::npos)
                                srv.Log(" %s(%u·조각%d)", d.name.c_str(),
                                            (unsigned)nox::SoundByName(d.name.c_str()), d.samples);
                        srv.Log("\n");
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
                    srv.Log("[맵] 쏘는 덫 %zu개 · 화살 피해 %d\n", traps.size(), arrowDmg);
                    // ★ 2026-09-18 ⑫ — **덫이 어디 있는지** 한 줄로 찍는다.
                    //   「겨눈 덫 0」이 나왔을 때 「덫이 안 도는 것」과 「사람이 사정거리
                    //   밖인 것」을 못 가른다. 좌표가 있어야 갈린다(덫 사정 ±350).
                    //   앞 여덟만 — 100개를 다 찍으면 로그가 읽히지 않는다.
                    // ★ 09-20 ⑮ — **방향도 찍는다.** 덫은 겨눈 뒤 **제 방향으로만** 쏜다
                    //   (`TrapShoot` — 대상 쪽이 아니다). 화살 선 위에 세우려면 방향이 있어야 한다.
                    //   앞 여덟만 — 100개를 다 찍으면 로그가 안 읽힌다
                    srv.Log("[덫 자리]");
                    {
                        size_t k = 0;
                        for (const auto& tr : traps) {
                            if (k < 8) srv.Log(" %d,%d(방향%u)", static_cast<int>(tr.x), static_cast<int>(tr.y),
                                                   static_cast<unsigned>(tr.facing));
                            else srv.Log(" %d,%d", static_cast<int>(tr.x), static_cast<int>(tr.y));
                            ++k;
                        }
                    }
                    srv.Log("\n");
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
                    srv.Log("[길목 이음] 길목 %zu개 · 나가는 이음 %zu개 · 막다른 길 %zu개\n",
                                allWps.size(), linkN, deadEnd);
                }
                srv.Log("[길목] 점멸이 고를 수 있는 곳 %zu곳 —", wps.size());
                // ★ 09-20 — **좌표도 찍는다.** 길목은 맵 제작자가 「걸을 수 있다」고 찍어 둔 자리라
                //   화면 판정에서 **설 자리**로 쓸 수 있다(`CanStand` 는 벽만 보고 바닥은 못 본다)
                for (size_t k = 0; k < wps.size() && k < 12; ++k)
                    srv.Log(" (%d,%d)", static_cast<int>(wps[k].first), static_cast<int>(wps[k].second));
                srv.Log("\n");
                // ★ 09-23 nine-defects 4-d2 — 맵 바닥 칸(FloorMap)과 그 재질 분포(4-d1 바닥 정의로). 발소리가 이 재질로 갈린다
                {
                    const auto floor = mf.ReadFloor();
                    std::map<int32_t, size_t> byMat;
                    size_t sides = 0, badImg = 0;
                    for (const auto& c : floor)
                        for (const auto* s : { &c.first, &c.second }) {
                            if (!s->present) continue;
                            ++sides;
                            if (s->image < tdb.Floors().size()) ++byMat[tdb.Floors()[s->image].material];
                            else ++badImg;
                        }
                    srv.Log("[바닥칸] %s · 칸 %zu · 쪽 %zu(번호가 정의 밖 %zu) · 재질별:", mf.Error().empty() ? "읽음" : ("⚠ " + mf.Error()).c_str(),
                                floor.size(), sides, badImg);
                    for (const auto& kv : byMat) srv.Log(" 0x%x×%zu", static_cast<unsigned>(kv.first), kv.second);
                    // 두 쪽이 다 찼는데 재질이 다른 칸 — 두 쪽 읽는 순서가 틀렸다면 **소리가 바뀌는 칸**은 이것뿐이다
                    size_t bothDiff = 0;
                    for (const auto& c : floor)
                        if (c.first.present && c.second.present && c.first.image < tdb.Floors().size() && c.second.image < tdb.Floors().size() &&
                            tdb.Floors()[c.first.image].material != tdb.Floors()[c.second.image].material) ++bothDiff;
                    srv.Log(" · 두 쪽 재질이 다른 칸 %zu\n", bothDiff);
                    // ★ 4-d3 — 칸 찾기를 **길목**(맵 제작자가 걸을 수 있다고 찍은 자리)마다 돌린다. 쪽이 빈 칸이 나오면
                    //   두 쪽 순서(4-d2)나 칸 찾기가 틀린 것이다 — 걸을 수 있는 자리는 바닥이 있어야 한다
                    nox::FloorGrid grid;
                    grid.Build(floor);
                    std::vector<nox::FloorGrid::EdgeDef> edefs;
                    for (const auto& e : tdb.Edges()) { nox::FloorGrid::EdgeDef d; d.w = e.w; d.h = e.h; edefs.push_back(d); }
                    size_t why[static_cast<size_t>(nox::FloorGrid::Why::Count)] = {};
                    std::map<int32_t, size_t> wpMat;
                    for (const auto& wp : wps) {
                        const auto hit = grid.TileAt(static_cast<float>(wp.first), static_cast<float>(wp.second), edefs);
                        ++why[static_cast<size_t>(hit.why)];
                        if (hit.tile >= 0 && static_cast<size_t>(hit.tile) < tdb.Floors().size()) ++wpMat[tdb.Floors()[static_cast<size_t>(hit.tile)].material];
                    }
                    srv.Log("[바닥칸·길목] 가장자리 정의 %zu개 · 길목 %zu곳 → 범위밖 %zu · 쪽빔 %zu · 쪽 %zu · 가장자리 %zu · 재질별:",
                                edefs.size(), wps.size(), why[0], why[1], why[2], why[3]);
                    for (const auto& kv : wpMat) srv.Log(" 0x%x×%zu", static_cast<unsigned>(kv.first), kv.second);
                    srv.Log("\n");
                    // ★ 09-27 V4 — 용암(바닥 **번호 6** — 서버 바닥 피해가 보는 값 · 재질 0x80 은 다른 바닥도 쓸 수 있다) 쪽 수와
                    //   그 위에 있는 길목(걸을 수 있다고 찍힌 자리)을 찍는다 — 용암 판에서 사람을 세울 자리
                    {
                        size_t lavaSides = 0;
                        for (const auto& c : floor)
                            for (const auto* s : { &c.first, &c.second }) if (s->present && s->image == 6) ++lavaSides;
                        std::vector<std::pair<int, int>> onLava;
                        for (const auto& wp : wps)
                            if (grid.TileAt(static_cast<float>(wp.first), static_cast<float>(wp.second), edefs).tile == 6)
                                onLava.push_back({ static_cast<int>(wp.first), static_cast<int>(wp.second) });
                        srv.Log("[용암칸] 바닥 번호 6 쪽 %zu · 그 위 길목 %zu곳 —", lavaSides, onLava.size());
                        for (size_t k = 0; k < onLava.size() && k < 8; ++k) srv.Log(" (%d,%d)", onLava[k].first, onLava[k].second);
                        srv.Log("\n");
                    }
                    // ★ 4-d4 — 발소리가 쓰도록 서버에 넣는다
                    std::vector<int32_t> mats;
                    for (const auto& f : tdb.Floors()) mats.push_back(f.material);
                    srv.SetFloor(std::move(grid), std::move(edefs), std::move(mats));
                }
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
                        srv.Log("[비밀벽 설자리] 벽 칸%d,%d (%d,%d) ← 가장 가까운 길목 (%d,%d) 거리 %d\n",
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
                            srv.Log("[벽낀 짝] (%d,%d) ↔ (%d,%d) 거리 %d — 서로 안 보인다\n",
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
                        srv.Log("[덫 설자리] 덫 (%d,%d) ← 가장 가까운 길목 (%d,%d) 거리 %d\n",
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
                        srv.Log("[문아님] 이름에 Door 가 있지만 클래스 DOOR 가 아니다 — %s (%d,%d)\n",
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
                        // ★ 09-29 Z1-A — 문 짐을 정답지 짜임대로 읽는다(`nox::ReadDoorXfer` — 각도 · 잠김 · 팀).
                        //   ✖ 전에는 「각도 = tail[1]」로 읽었다 — 짐 머리 첫 바이트가 0 일 때만 맞고, 0 이 아니면
                        //     (이름·팀·스크립트가 낀 문) **깃발 첫 바이트를 각도로** 읽었다(G_Castle 6 · Fortress 6 · LostTomb 4 …)
                        nox::DoorXfer dx;
                        const bool dxOk = isDoor && nox::ReadDoorXfer(objs[i], dx);
                        if (isDoor) {
                            ++doorsSeen;
                            if (!dxOk) {
                                ++doorsUnread;                    // 짐이 짧거나 판이 어긋난다 — 잠김을 못 읽었다
                            } else {
                                if (dx.lock != 0) ++doorsLocked;
                                ++doorLockKinds[dx.lock <= 5 ? dx.lock : 6];   // 0 안 잠김 · 1~4 열쇠(은·금·루비·사파이어) · 5 장치 · 6 그 밖
                                if (dx.extra) ++doorsExtra;
                                if (dx.team != 0) ++doorsTeam;
                                if (dx.angle > 31) ++doorsAngleBad;
                                if (objs[i].tail.size() >= 2 && objs[i].tail[1] != (dx.angle & 31)) ++doorsAngleFixed;
                                // 짐 끝 확인 — 셋째 u32 까지 읽었으니 가방이 없으면 남는 게 없다
                                if (dx.rest != 0) ++doorsRestOdd;
                                if (dx.closed != dx.angle) ++doorsClosedDiff;
                                wp.lock = static_cast<uint8_t>(dx.lock > 255 ? 255 : dx.lock);
                                wp.team = dx.team;
                            }
                            // `+12 & 4` = 부분류 GATE(`subclass_other.go:8` 셋째) — 잠김 소리 244(GateLocked)와 240(WoodenDoorLocked)을 가른다
                            wp.gate = ti && ti->subclassRaw.find("GATE") != std::string::npos;
                        }
                        if (dxOk) {
                            // 닫힘 각도 = 셋째 u32(+4) · 지금 각도 = 첫 u32(+12) — 58맵 전부 둘이 같다(부팅 줄 「지금 각도≠닫힘 각도 0」)
                            wp.angle = static_cast<uint8_t>(dx.closed & 31);
                            // 미세각은 거친각 × 8 에서 시작한다(거친각 = 32 × 미세 / 256 · 정답지 +40 = (각도 << 8) / 32)
                            wp.fine = static_cast<uint8_t>((static_cast<int>(dx.angle & 31) * 8) & 255);
                            // ⚠ 여는 거리는 **반지름을 같이 찍는다** — 진짜로 문 몸에서 나온 42 와
                            //   못 찾아 떨어진 기본값 42 가 숫자로는 똑같아서 구분이 안 된다
                            srv.Log("[문각도] 번호 %04x %s (%d,%d) — 닫힘 %u · 열림 %u · 여는거리 %.0f(문반지름 %s)\n",
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
                            srv.Log("[완드] ⚠ %s 의 주문 %s 을 못 찾음\n", objs[i].type.c_str(), nm);
                    }
                }
                w2.equipBit = ebit;
                w2.isWeapon = isW;
                // ★ 09-27 B3 — 무기 들기 첫 관문(`obj_class & 0x1001000` = WEAPON|WAND)
                w2.weaponClass = ti->classRaw.find("WEAPON") != std::string::npos ||
                                 ti->classRaw.find("WAND")   != std::string::npos;
                // ★ 09-27 B5 — 지팡이 클래스(0x1000) · 무기·지팡이 SUBCLASS 비트(충전 알림·지속 주문 끊기가 본다)
                w2.wandClass = ti->classRaw.find("WAND") != std::string::npos;
                if (w2.weaponClass) w2.subclass = nox::WeaponClassBitsOf(ti->subclassRaw);
                // ★ 09-28 — 갑옷이면 갑옷 SUBCLASS 비트(투구 1·방패 2·가슴 4·…) — 줍기 자동 입기가 「같은 부위를 입었나」를 본다(`armorHaveSameSubclass_53E7B0`)
                else if (ti->classRaw.find("ARMOR") != std::string::npos) w2.subclass = nox::ParseArmorClass(ti->subclassRaw);
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
                w2.pickupFn = pf;   // ★ 09-29 — 무기·화살통 줍기 갈래(`sub_53A720` · `pickupAmmo_4F3B00`)
                // ★ 09-29 — 무기 줍는 소리 `sub_53A6C0`(`GAME4_3.c:5014~5031`) — 지팡이 클래스(0x1000) 830 · 재질 쇠(0x10) 842 · 나무(8) 844 · 그 밖 없음
                {
                    const uint32_t mat = nox::ParseMaterial(ti->materialRaw);
                    if (ti->classRaw.find("WAND") != std::string::npos) w2.pickupSound = 830;
                    else if (mat & 0x10) w2.pickupSound = 842;
                    else if (mat & 0x8) w2.pickupSound = 844;
                }
                // ★ 09-29 — 화살통의 화살 수·최대 — 맵 짐(`nox_xxx_XFerAmmo_4F6B20` `GAME4.c:509~597`)에서. 쓰기 함수가 AmmoUse 인 것만
                //   짐 판 = 앞 u16(`typeVers` — `GAME4.c:534~537` 60 을 넘으면 실패) · 40 미만이면 옛 짜임(`readObjectOldVer_4F4170` `GAME3_3.c:11259~11261`)이라
                //   우리 읽개가 못 읽는다 — 그런 것은 0/0 으로 두고 세어서 알린다
                if (ti->useHandler == "AmmoUse") {
                    nox::AmmoXfer ax;
                    if (objs[i].typeVers >= 40 && objs[i].typeVers <= 60 && nox::ReadAmmoXfer(objs[i], ax)) {
                        w2.charges = ax.count;
                        w2.chargesMax = ax.max;
                        srv.NoteQuiverFromMap();
                        ++ammoRead;
                        ammoSum += ax.count;
                        ammoMaxSum += ax.max;
                        ammoRest += ax.rest;
                    } else {
                        ++ammoBad;
                    }
                }
                // ★ 09-28 — 버려도 안 사라지는 까닭(`GAME3_3.c:6440~6442`). 오브젝트 깃발은 thing.bin FLAGS 가 밑이다 —
                //   맵 파일 깃발은 0x11408162 안의 비트만 덮으므로(`mapReadWriteObjData_4F4530` `:11296~11306`) RESPAWN(0x80000)은 종류 값 그대로
                {
                    bool flagClass = false;
                    for (size_t p = 0; p <= ti->classRaw.size();) {
                        const size_t e = ti->classRaw.find('+', p);
                        const std::string tok = ti->classRaw.substr(p, e == std::string::npos ? std::string::npos : e - p);
                        if (tok == "FLAG") flagClass = true;
                        if (e == std::string::npos) break;
                        p = e + 1;
                    }
                    if (nox::ParseObjectFlags(ti->flagsRaw) & 0x80000u) w2.decayExempt = nox::DecayExempt::Respawn;
                    else if (flagClass) w2.decayExempt = nox::DecayExempt::FlagClass;
                    else if (objs[i].type == "Glyph") w2.decayExempt = nox::DecayExempt::Glyph;
                    ++decayExemptKinds[static_cast<size_t>(w2.decayExempt)];
                    w2.dropFn = nox::DropFnOf(ti->dropFn);   // ★ 09-28 — 갑옷·무기·물약·음식은 25초로 다시 건다(RESPAWN 과 상관없이)
                    ++dropFnKinds[static_cast<size_t>(w2.dropFn)];
                }
                // ★ 09-30 — `--give-items` 에 든 종류면 이 바닥 물건으로 가방 틀을 뜬다(첫 것 하나 · 기본 장비 여섯은 부활 틀이라 뺀다)
                if (!giveItemNames.empty() && !giveTemplated.count(wire) &&
                    std::find(giveItemNames.begin(), giveItemNames.end(), objs[i].type) != giveItemNames.end() &&
                    wire != nox::kItemStreetShirt && wire != nox::kItemStreetPants && wire != nox::kItemStreetSneakers &&
                    wire != nox::kItemWizardRobe && wire != nox::kItemWoodenShield && wire != nox::kItemLongsword) {
                    srv.SetItemTemplate(wire, nox::Server::CarriedFromStatic(w2));
                    giveTemplated.insert(wire);
                }
                st.push_back(w2);
            }
            nStat = st.size();
            srv.SetWorldStatics(std::move(st));
            if (!giveItemNames.empty()) {   // ★ 09-30 사람 판 시험용
                std::vector<uint16_t> give;
                std::string missing;
                for (const auto& nm : giveItemNames) {
                    const uint16_t gw = tdb.WireByName(nm);
                    if (gw != 0 && giveTemplated.count(gw)) give.push_back(gw);
                    else missing += " " + nm;
                }
                srv.SetGiveItems(give);
                srv.Log("[시험물건] 들어오면 가방에 %zu개 넣음%s%s\n", give.size(), missing.empty() ? "" : " · ⚠ 이 맵 바닥에 없어 못 줌:", missing.c_str());
            }
            if (!spawnAtItem.empty()) {   // ★ 09-27 B6
                uint16_t nc = 0;
                const uint16_t wire = tdb.WireByName(spawnAtItem);
                if (wire != 0 && srv.DebugSpawnAtStaticType(wire, nc))
                    srv.Log("[시험] 스폰 자리를 %s(종류 %u · 번호 %u) 자리 (%d,%d) 로\n", spawnAtItem.c_str(),
                                (unsigned)wire, (unsigned)nc, srv.spawnX(), srv.spawnY());
                else
                    srv.Log("[시험] ⚠ 바닥에 %s 가 없다(종류 %u) — 이 판은 무효다\n", spawnAtItem.c_str(), (unsigned)wire);
                std::fflush(stdout);
            }

            // ★ 09-27 B1 — 직업 기본 장비의 가방 항목 틀. 맵 물건(위)과 같은 칸 규칙으로 thing.bin 에서 채운다
            //   (수식 갈래 · 장비 비트 · 무게). 서버는 접속 때 이 틀로 기본 장비를 가방에 싣는다(`Server::GiveDefaultGear`)
            {
                srv.Log("[기본장비] 가방 틀 —");
                for (uint16_t gw : { nox::kItemStreetShirt, nox::kItemStreetPants, nox::kItemStreetSneakers,
                                     nox::kItemWizardRobe, nox::kItemWoodenShield, nox::kItemLongsword }) {
                    const char* nm = nox::LoadoutItemName(gw);
                    const nox::ThingInfo* ti = tdb.ByWire(gw);
                    if (!nm || !ti || ti->name != nm) {   // 와이어 번호(종류표)와 thing.bin 이름이 어긋나면 싣지 않는다
                        srv.Log(" %u(✗ 이름 다름)", (unsigned)gw);
                        continue;
                    }
                    nox::Session::Carried c;
                    bool isW = false;
                    c.equipBit = nox::EquipBitFor(nm, &isW);
                    c.isWeapon = isW;
                    c.modifiable =
                        ti->classRaw.find("WEAPON") != std::string::npos ||
                        ti->classRaw.find("ARMOR")  != std::string::npos ||
                        ti->classRaw.find("WAND")   != std::string::npos ||
                        ti->classRaw.find("FLAG")   != std::string::npos;
                    c.weight = static_cast<uint8_t>(ti->weight < 0 ? 0 : (ti->weight > 255 ? 255 : ti->weight));
                    c.weaponClass = ti->classRaw.find("WEAPON") != std::string::npos ||   // ★ 09-27 B3
                                    ti->classRaw.find("WAND")   != std::string::npos;
                    c.wandClass = ti->classRaw.find("WAND") != std::string::npos;          // ★ 09-27 B5
                    if (c.weaponClass) c.subclass = nox::WeaponClassBitsOf(ti->subclassRaw);
                    else if (ti->classRaw.find("ARMOR") != std::string::npos) c.subclass = nox::ParseArmorClass(ti->subclassRaw);   // ★ 09-28
                    c.dropFn = nox::DropFnOf(ti->dropFn);   // ★ 09-28 — 버리기 함수(로브·롱소드·나무방패는 갑옷무기 → 25초) · RESPAWN 은 기본 장비라 지운 채(`GAME3_3.c:7891`)
                    srv.Log(" %s(무게 %d%s · 비트 %#x%s · 부위 %#x · 버리기 %s)", nm, (int)c.weight, ti->hasWeight ? "" : "·WEIGHT없음",
                                (unsigned)c.equipBit, c.isWeapon ? (c.weaponClass ? " · 무기" : " · 무기(클래스 아님)") : "",
                                (unsigned)c.subclass, ti->dropFn.empty() ? "(없음)" : ti->dropFn.c_str());
                    srv.SetItemTemplate(gw, c);
                }
                srv.Log("\n");
            }

            // ★ 09-27 B3 — modifier.bin(암호 13) `WEAPON_DEFINITIONS` → 무기 들기의 직업(CLASSUSE)·힘(REQUIRED_STRENGTH) 관문.
            //   복호본은 글자다 — `<이름>` 줄 · `KEY = VALUE;` 줄들 · `END` 줄(실측 — `nox_datadump modifier.bin --out`).
            //   정답지 `server/modifiers.go:273~331` 이 같은 두 칸을 읽는다(CLASSUSE 는 `+` 로 이은 Warrior·Wizard·Conjurer → 비트 0·1·2 `:314`).
            //   이름 → 종류 번호는 정답지처럼 이름으로(`ClientTypeByID` `:122`) · `ARMOR_DEFINITIONS` 부터는 갑옷이라 여기서 끊는다
            {
                const std::vector<uint8_t> mb = nox::DecryptFile("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\modifier.bin", nox::kKeyModifier);
                const std::string txt(mb.begin(), mb.end());
                const size_t armorAt = txt.find("ARMOR_DEFINITIONS");
                const std::string wsec = txt.substr(0, armorAt == std::string::npos ? txt.size() : armorAt);
                auto trim = [](std::string v) {
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == ';')) v.pop_back();
                    size_t i = 0;
                    while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
                    return v.substr(i);
                };
                std::string cur;
                uint8_t classes = 0;
                int reqStr = 0;
                int dmgMin = 0;          // ★ 09-29 — DAMAGE_MIN(+72) · DAMAGE_COEFFICIENT(+64) — 화살 피해 식(`calcBoltDamage_4EF1E0`)이 쓴다
                double dmgCoeff = 0.0;   //   `0.0f;` 꼴도 있다(ArcherBolt) — atof 가 0 으로 읽는다
                size_t nDef = 0, nNoWire = 0, nBadClass = 0;
                size_t pos = 0;
                while (pos < wsec.size()) {
                    size_t e = wsec.find('\n', pos);
                    if (e == std::string::npos) e = wsec.size();
                    const std::string line = trim(wsec.substr(pos, e - pos));
                    pos = e + 1;
                    if (line.empty() || line == "WEAPON_DEFINITIONS") continue;
                    if (line == "END") {
                        if (!cur.empty()) {
                            const uint16_t wire = tdb.WireByName(cur);
                            if (wire == 0) ++nNoWire;
                            else {
                                srv.SetWeaponDef(wire, classes, static_cast<uint16_t>(reqStr < 0 ? 0 : reqStr),
                                                 static_cast<uint16_t>(dmgMin < 0 ? 0 : dmgMin), static_cast<float>(dmgCoeff));
                                ++nDef;
                            }
                            if (cur == "Longsword" || cur == "GreatSword" || cur == "StaffWooden" || cur == "ForceWand" || cur == "Bow" ||
                                cur == "CrossBow" || cur == "Quiver" || cur == "ArcherArrow" || cur == "ArcherBolt" || cur == "WeakArcherArrow")
                                srv.Log("[무기정의] %s(종류 %u) — 직업 비트 %u · 요구 힘 %d · 피해 최소 %d · 계수 %.3f\n",
                                            cur.c_str(), (unsigned)wire, (unsigned)classes, reqStr, dmgMin, dmgCoeff);
                        }
                        cur.clear();
                        continue;
                    }
                    const size_t eq = line.find('=');
                    if (eq == std::string::npos) { cur = line; classes = 0; reqStr = 0; dmgMin = 0; dmgCoeff = 0.0; continue; }   // 새 정의의 이름 줄
                    const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
                    if (key == "DAMAGE_MIN") dmgMin = std::atoi(val.c_str());
                    else if (key == "DAMAGE_COEFFICIENT") dmgCoeff = std::atof(val.c_str());
                    if (key == "CLASSUSE") {
                        size_t s0 = 0;
                        while (s0 <= val.size()) {
                            size_t s1 = val.find('+', s0);
                            if (s1 == std::string::npos) s1 = val.size();
                            const std::string tok = trim(val.substr(s0, s1 - s0));
                            if (tok == "WARRIOR") classes |= 1;
                            else if (tok == "WIZARD") classes |= 2;
                            else if (tok == "CONJURER") classes |= 4;
                            else ++nBadClass;   // 정답지는 모르는 이름이면 오류(`modParseBitList`)
                            s0 = s1 + 1;
                        }
                    } else if (key == "REQUIRED_STRENGTH") {
                        reqStr = std::atoi(val.c_str());
                    }
                }
                srv.Log("[무기정의] modifier.bin %zu바이트 · 무기 %zu개 실음 · 종류표에 없음 %zu · 모르는 직업 이름 %zu\n",
                            mb.size(), nDef, nNoWire, nBadClass);
                // ★ 09-29 — `ARMOR_DEFINITIONS` → 갑옷 입기의 직업·힘 관문과 방어값(ARMOR_VALUE → `Modifier` +64 · `parseWeaponOrArmorDef412D40` `modifiers.go:325~329`).
                //   절은 다음 절 이름(ENCHANTMENT·EFFECTIVENESS·MATERIAL) 줄에서 끝난다
                if (armorAt != std::string::npos) {
                    std::string asec = txt.substr(armorAt);
                    for (const char* nextSec : { "\nENCHANTMENT", "\nEFFECTIVENESS", "\nMATERIAL" }) {
                        const size_t cut = asec.find(nextSec);
                        if (cut != std::string::npos) asec = asec.substr(0, cut);
                    }
                    std::string acur;
                    uint8_t aclasses = 0;
                    int areq = 0;
                    double aval = 0.0;
                    size_t nArm = 0, nArmNoWire = 0;
                    size_t apos = 0;
                    while (apos < asec.size()) {
                        size_t e = asec.find('\n', apos);
                        if (e == std::string::npos) e = asec.size();
                        const std::string line = trim(asec.substr(apos, e - apos));
                        apos = e + 1;
                        if (line.empty() || line == "ARMOR_DEFINITIONS") continue;
                        if (line == "END") {
                            if (!acur.empty()) {
                                const uint16_t wire = tdb.WireByName(acur);
                                if (wire == 0) ++nArmNoWire;
                                else { srv.SetArmorDef(wire, aclasses, static_cast<uint16_t>(areq < 0 ? 0 : areq), static_cast<float>(aval)); ++nArm; }
                                if (acur == "StreetShirt" || acur == "StreetPants" || acur == "StreetSneakers" || acur == "WizardRobe" || acur == "WoodenShield" || acur == "SteelShield" || acur == "LeatherHelm")
                                    srv.Log("[갑옷정의] %s(종류 %u) — 직업 비트 %u · 요구 힘 %d · 방어 %.3f\n", acur.c_str(), (unsigned)wire, (unsigned)aclasses, areq, aval);
                            }
                            acur.clear();
                            continue;
                        }
                        const size_t eq = line.find('=');
                        if (eq == std::string::npos) { acur = line; aclasses = 0; areq = 0; aval = 0.0; continue; }
                        const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
                        if (key == "CLASSUSE") {
                            size_t s0 = 0;
                            while (s0 <= val.size()) {
                                size_t s1 = val.find('+', s0);
                                if (s1 == std::string::npos) s1 = val.size();
                                const std::string tok = trim(val.substr(s0, s1 - s0));
                                if (tok == "WARRIOR") aclasses |= 1;
                                else if (tok == "WIZARD") aclasses |= 2;
                                else if (tok == "CONJURER") aclasses |= 4;
                                else ++nBadClass;
                                s0 = s1 + 1;
                            }
                        } else if (key == "REQUIRED_STRENGTH") {
                            areq = std::atoi(val.c_str());
                        } else if (key == "ARMOR_VALUE") {
                            aval = std::atof(val.c_str());
                        }
                    }
                    srv.Log("[갑옷정의] 갑옷 %zu개 실음 · 종류표에 없음 %zu\n", nArm, nArmNoWire);
                }
            }

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
                srv.Log("[들 무게] thing.bin NewPlayer CARRYCAPACITY %d (오브젝트 기본값 — 사람은 힘에서 다시 셈: 위저드 %d → 한도 %d)\n",
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
                srv.Log("[들 무게] 맵 물건 무게 —");
                for (const auto& kv : wsum)
                    srv.Log(" %s %d(×%d)", kv.first.c_str(), kv.second.first, kv.second.second);
                srv.Log("\n");
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
                    b.wireType = w;                                // ★ 4-c3b2 — 맵 불에 맞을 때 「때린 것」 재질
                    // ★ 09-23 nine-defects 1-d — 닿음 피해(`collideDamageLoad_536E10`): 첫 칸 = 양(u8 로 자름) · 둘째 칸 = 종류 이름.
                    //   이름을 못 찾으면(18) 정답지는 읽기 실패로 본다 — 우리는 피해를 안 싣고 드러낸다
                    if (t2->collideFn == "DamageCollide") {
                        const std::string& args = t2->collideArgs;
                        const size_t sp = args.find(' ');
                        const int amount = std::atoi(args.c_str());
                        const int kind = nox::DamageTypeByName(sp == std::string::npos ? std::string() : args.substr(sp + 1));
                        if (kind >= nox::kDamageTypeCount) {
                            srv.Log("[닿음피해] ⚠ %s — 종류 이름을 못 읽음(`%s`) · 피해를 안 싣는다\n", o.type.c_str(), args.c_str());
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
                    srv.Log("[닿음피해] 장애물 %zu개 —", n);
                    for (const auto& kv : hazardNames) srv.Log(" %sx%d", kv.first.c_str(), kv.second);
                    srv.Log(" · 자리(앞 %zu개)", hazardSpots.size());
                    for (const auto& sp : hazardSpots) srv.Log(" %s", sp.c_str());
                    srv.Log("\n");
                }
                srv.Log("[장애물] %zu개 / %zu종 —", obs.size(), obsNames.size());
                for (const auto& kv : obsNames) srv.Log(" %sx%d", kv.first.c_str(), kv.second);
                srv.Log("\n");
                // ★ 09-20 ② — **상자 자리**를 따로 찍는다. 「몸이 막히나」를 보려면 좌표가 있어야 한다
                {
                    size_t nChest = 0;
                    for (const auto& o : objs) {
                        if (o.type.find("Chest") == std::string::npos) continue;
                        if (nChest == 0) srv.Log("[상자 자리]");
                        if (nChest < 6) srv.Log(" %s(%d,%d)", o.type.c_str(),
                                                    static_cast<int>(o.x), static_cast<int>(o.y));
                        ++nChest;
                    }
                    if (nChest) { srv.Log("  — 모두 %zu개\n", nChest); std::fflush(stdout); }
                }
                if (noObstacles) {
                    srv.Log("[진단스위치] --no-obstacles — 장애물 %zu개를 싣지 않음(대조군)\n", obs.size());
                    obs.clear();
                }
                srv.SetObstacles(std::move(obs));
                // ★ 09-28 B4b — 물건 몸 반지름(원 모양만 · +176). 죽을 때 떨굴 자리 간격 = 가장 큰 반지름 × 2 + 6(`GAME3_3.c:6730~6748`)
                {
                    size_t nR = 0;
                    for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                        const nox::ThingInfo* t2 = tdb.ByWire(w);
                        if (!t2 || !t2->hasWeight || t2->shapeKind != 2) continue;
                        srv.SetItemRadius(w, t2->radius);
                        ++nR;
                    }
                    const nox::ThingInfo* rb = tdb.ByWire(nox::kItemWizardRobe);
                    srv.Log("[물건몸] 원 모양 반지름 %zu종 · 로브 %s %.0f\n", nR,
                                rb ? (rb->shapeKind == 2 ? "원" : "원아님") : "없음", rb ? rb->radius : 0.0f);
                }
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
                    srv.Log("[나는몬스터] AIRBORNE 몬스터 %zu종 —%s%s\n", nFly, names.c_str(), nFly > 12 ? " …" : "");
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
                        srv.Log("[날리기] Magic 종류 %u · SPEED %d · MASS %.1f\n", mw, mt->speed, mt->mass);
                    }
                }
                srv.Log("[충돌 함수] 종류 %zu개 · 맵 오브젝트 %zu개 —", nFn, refs.size());
                for (const auto& kv : fnNames) srv.Log(" %sx%d", kv.first.c_str(), kv.second);
                srv.Log("\n");
                srv.SetMapObjects(std::move(refs));
            }
            // ★ 2026-09-13 묶음 3-a — 사람 애니 칸 수(PlayerDraw STAT). 맨손 공격이 끝나는 틱이 이 값이다.
            for (int i = 0; i < nox::kPlayerAnimTypeCount; ++i)
                srv.SetPlayerAnimFrames(i, tdb.PlayerAnim(i).frames, tdb.PlayerAnim(i).hold);
            // ★ 09-29 B5 — 몬스터 애니 표(MonsterDraw STAT) — 걸음 소리가 애니 칸을 본다(`monsterMoveAudio_534030`)
            {
                for (size_t w = 1; w <= tdb.Count(); ++w) {
                    const nox::ThingInfo* t = tdb.ByWire(static_cast<uint16_t>(w));
                    if (!t || !t->hasMonsterDraw) continue;
                    for (int i = 0; i < 16; ++i)
                        srv.SetMonsterAnim(static_cast<uint16_t>(w), i, t->monsterAnims[i].frames, t->monsterAnims[i].hold, t->monsterAnims[i].loop);
                }
                // ★ 09-29 B6 — 몬스터 아종 비트(SUBCLASS — libs `object/subclass_monster.go` 차례 · 0x200 IMMUNE_POISON · 0x400 IMMUNE_FIRE)
                //   · 위험한 종류(조건 69 — CLASS FIRE · 독구름 두 이름 · CLASS DANGEROUS — `unitIsDangerous_547120`)
                {
                    static const char* const kMonSub[] = {
                        "SMALL_MONSTER", "MEDIUM_MONSTER", "LARGE_MONSTER", "SHOPKEEPER", "NPC", "FEMALE_NPC",
                        "UNDEAD", "MONITOR", "MIGRATE", "IMMUNE_POISON", "IMMUNE_FIRE", "IMMUNE_ELECTRICITY",
                        "IMMUNE_FEAR", "BOMBER", "NO_TARGET", "NO_SPELL_TARGET", "HAS_SOUL", "WARCRY_STUN",
                        "LOOK_AROUND", "WOUNDED_NPC" };
                    auto hasTok = [](const std::string& raw, const char* tok) {
                        size_t a = 0;
                        while (a <= raw.size()) {
                            size_t e = raw.find('+', a);
                            if (e == std::string::npos) e = raw.size();
                            std::string s = raw.substr(a, e - a);
                            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                            if (s == tok) return true;
                            if (e == raw.size()) break;
                            a = e + 1;
                        }
                        return false;
                    };
                    size_t nSub = 0, nFireImm = 0, dn[4] = {};
                    std::string fireImm;
                    for (size_t w = 1; w <= tdb.Count(); ++w) {
                        const nox::ThingInfo* t = tdb.ByWire(static_cast<uint16_t>(w));
                        if (!t) continue;
                        if (t->isMonster) {
                            uint32_t bits = 0;
                            for (size_t i = 0; i < sizeof(kMonSub) / sizeof(kMonSub[0]); ++i) if (hasTok(t->subclassRaw, kMonSub[i])) bits |= 1u << i;
                            if (bits) { srv.SetMonsterSubclass(static_cast<uint16_t>(w), bits); ++nSub; }
                            if (bits & 0x400) { ++nFireImm; if (fireImm.size() < 80) { fireImm += ' '; fireImm += t->name; } }
                        }
                        uint8_t d = 0;
                        if (hasTok(t->classRaw, "FIRE")) d = 1;
                        else if (t->name == "ToxicCloud" || t->name == "SmallToxicCloud") d = 2;
                        else if (hasTok(t->classRaw, "DANGEROUS")) d = 3;
                        if (d) { srv.SetDangerType(static_cast<uint16_t>(w), d); ++dn[d]; }
                    }
                    srv.Log("[몬스터아종] 아종 비트 있는 몬스터 %zu · 불에 안 탐 %zu(%s) · 위험한 종류 불 %zu · 독구름 %zu · DANGEROUS %zu\n",
                                nSub, nFireImm, fireImm.c_str(), dn[1], dn[2], dn[3]);
                }
                const nox::ThingInfo* lg = tdb.ByWire(tdb.WireByName("GiantLeech"));
                srv.Log("[몬스터애니] MonsterDraw 종류 %zu개 · GiantLeech 걷기(12) %d칸/붙듦 %d/되풀이 %d · 근접(1) %d칸/붙듦 %d\n",
                            srv.MonsterAnimTypes(),
                            lg ? lg->monsterAnims[12].frames : -1, lg ? lg->monsterAnims[12].hold : -1, lg ? (int)lg->monsterAnims[12].loop : -1,
                            lg ? lg->monsterAnims[1].frames : -1, lg ? lg->monsterAnims[1].hold : -1);
                // ★ 09-29 B7 — 비홀더 시전 애니(7 CAST_SPELL) — 시전은 이 애니가 쏘기 칸(monster.bin +216 = 4)에 닿을 때(`mobActionCast_5413B0`)
                const nox::ThingInfo* bh = tdb.ByWire(tdb.WireByName("Beholder"));
                srv.Log("[몬스터애니] Beholder 시전(7) %d칸/붙듦 %d/되풀이 %d · 놀기(8) %d칸\n",
                            bh ? bh->monsterAnims[7].frames : -1, bh ? bh->monsterAnims[7].hold : -1, bh ? (int)bh->monsterAnims[7].loop : -1,
                            bh ? bh->monsterAnims[8].frames : -1);
                // ★ 09-30 — 열셋의 근접(1)·쏘기(3)·죽어가기(9) 애니 — 근접은 때리는 칸(monster.bin +108)·쏘기는 쏘는 칸(+216)에서, 둘 다 애니가 끝나면 빠진다.
                //   표 없음(MonsterDraw 없음)이면 근접·쏘기가 안 빠지고, 칸이 칸 수 밖이면 안 맞는다 — 그 수를 같이 센다
                {
                    int noDraw = 0, meleeOut = 0, shotOut = 0, loopAtk = 0;   // 되풀이 애니면 끝(+483)이 안 와 근접·쏘기가 안 빠진다
                    for (const auto& ms : nox::kMonsters) {
                        const nox::ThingInfo* t = tdb.ByWire(ms.wireType);
                        if (!t || !t->hasMonsterDraw) { ++noDraw; srv.Log("[몬스터칸] %s 표 없음\n", ms.name); continue; }
                        const int hitF = nox::MonsterMeleeFrameOf(ms.wireType), shotF = nox::MonsterMissileFrameOf(ms.wireType);
                        const bool melee = nox::MeleeStrikeOf(ms.wireType).fn != nox::StrikeFn::None && ms.meleeRange > 0.0;
                        const bool shoot = ms.missileName != nullptr;
                        if (melee && hitF >= t->monsterAnims[1].frames) ++meleeOut;
                        if (shoot && shotF >= t->monsterAnims[3].frames) ++shotOut;
                        if ((melee && t->monsterAnims[1].loop) || (shoot && t->monsterAnims[3].loop)) ++loopAtk;
                        srv.Log("[몬스터칸] %-17s 근접(1) %2d칸/붙듦 %d/되풀이 %d%s · 쏘기(3) %2d칸/붙듦 %d/되풀이 %d%s · 죽어가기(9) %2d칸/붙듦 %d/되풀이 %d\n",
                                    ms.name, t->monsterAnims[1].frames, t->monsterAnims[1].hold, (int)t->monsterAnims[1].loop,
                                    melee ? (" 때림칸 " + std::to_string(hitF)).c_str() : "",
                                    t->monsterAnims[3].frames, t->monsterAnims[3].hold, (int)t->monsterAnims[3].loop,
                                    shoot ? (" 쏨칸 " + std::to_string(shotF)).c_str() : "",
                                    t->monsterAnims[9].frames, t->monsterAnims[9].hold, (int)t->monsterAnims[9].loop);
                    }
                    srv.Log("[몬스터칸] 열셋 중 표 없음 %d · 때림칸이 칸 수 밖 %d · 쏨칸이 칸 수 밖 %d · 공격 애니가 되풀이 %d\n", noDraw, meleeOut, shotOut, loopAtk);
                }
            }
            // ★ 09-23 2-a1 — 함성(46)·광전사(45) 애니 길이도 찍는다. 함성 애니가 끝나면 켜짐 값이 내려가 광전사·작살을 더는 안 막으므로
            //   「함성 지속 120 중 얼마 동안 막나」가 이 값으로 정해진다(플랜 v2 에서 미확인이던 것)
            //   ★ 3-a — 점프(3) 애니 길이 = 떠 있는 틱 수(칸 × (붙듦+1))
            srv.Log("[애니] 사람 STAT %d개 — 주먹 %d칸/붙듦 %d · 훅 %d칸/붙듦 %d · 함성(46) %d칸/붙듦 %d · 광전사(45) %d칸/붙듦 %d · 점프(3) %d칸/붙듦 %d\n",
                        tdb.PlayerAnimCount(),
                        tdb.PlayerAnim(23).frames, tdb.PlayerAnim(23).hold,
                        tdb.PlayerAnim(25).frames, tdb.PlayerAnim(25).hold,
                        tdb.PlayerAnim(46).frames, tdb.PlayerAnim(46).hold,
                        tdb.PlayerAnim(45).frames, tdb.PlayerAnim(45).hold,
                        tdb.PlayerAnim(3).frames, tdb.PlayerAnim(3).hold);
            // ★ 09-23 nine-defects 4-c1 — 재질(MATERIAL). 타격 소리가 「맞는 쪽 재질 × 때린 것 재질」로 갈린다(`server/sound.go:31~161`).
            //   재질별 종류 수 + 우리 무기 표 여덟의 재질 + 갑옷(ARMOR) 재질 분포를 찍는다 — 읽혔다는 근거다
            {
                static const char* const kMat[15] = { "FLESH", "CLOTH", "ANIMAL_HIDE", "WOOD", "METAL", "STONE", "EARTH", "LIQUID",
                                                      "GLASS", "PAPER", "SNOW", "MUD", "MAGIC", "DIAMOND", "NONE" };
                size_t perBit[15] = {}, declared = 0, armorPerBit[15] = {}, armors = 0, unknown = 0, nullDecl = 0;
                std::string unknownRaw;
                for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                    const nox::ThingInfo* t2 = tdb.ByWire(w);
                    if (!t2) continue;
                    if (!t2->materialRaw.empty()) ++declared;
                    const uint32_t m = nox::ParseMaterial(t2->materialRaw);
                    // 선언했는데 이름을 하나도 못 알아본 것 — 조용히 0 이 되면 「재질 없음」과 안 갈린다.
                    //   `NULL` 은 정답지가 **정상 값 0** 으로 받는다(libs `enum/enum.go:66`) — 따로 센다
                    if (!t2->materialRaw.empty() && m == 0) {
                        if (t2->materialRaw == "NULL") ++nullDecl;
                        else if (++unknown <= 3) unknownRaw += " " + t2->materialRaw;
                    }
                    const bool armor = (nox::ParseObjectClass(t2->classRaw) & 0x2000000) != 0;
                    if (armor) ++armors;
                    for (int b = 0; b < 15; ++b) if (m & (1u << b)) { ++perBit[b]; if (armor) ++armorPerBit[b]; }
                }
                srv.Log("[재질] MATERIAL 선언 %zu종 / %zu · NULL(재질 0) %zu · 못 읽은 이름 %zu%s —", declared, tdb.Count(), nullDecl,
                            unknown, unknownRaw.c_str());
                for (int b = 0; b < 15; ++b) if (perBit[b]) srv.Log(" %s %zu", kMat[b], perBit[b]);
                srv.Log(" · 갑옷 %zu종:", armors);
                for (int b = 0; b < 15; ++b) if (armorPerBit[b]) srv.Log(" %s %zu", kMat[b], armorPerBit[b]);
                srv.Log(" · 무기:");
                for (const auto& wp : nox::kWeapons) {
                    const nox::ThingInfo* t2 = tdb.ByWire(tdb.WireByName(wp.name));
                    srv.Log(" %s=%s", wp.name, t2 ? (t2->materialRaw.empty() ? "(없음→NONE)" : t2->materialRaw.c_str()) : "?");
                }
                const nox::ThingInfo* np = tdb.ByWire(tdb.WireByName("NewPlayer"));
                srv.Log(" · NewPlayer=%s\n", np ? (np->materialRaw.empty() ? "(없음→NONE)" : np->materialRaw.c_str()) : "?");
                // ★ 4-c2 — 타격 소리 표: 와이어 번호별 재질 · 가슴갑옷(ARMOR + BREASTPLATE)인가 · 사람 몸(NewPlayer) 번호
                std::vector<uint32_t> matByWire(static_cast<size_t>(tdb.Count()) + 1, 0u);
                std::vector<uint8_t> breastByWire(static_cast<size_t>(tdb.Count()) + 1, 0);
                size_t nBreast = 0;
                for (uint16_t w = 1; w <= tdb.Count(); ++w) {
                    const nox::ThingInfo* t2 = tdb.ByWire(w);
                    if (!t2) continue;
                    matByWire[w] = nox::ParseMaterial(t2->materialRaw);
                    if ((nox::ParseObjectClass(t2->classRaw) & 0x2000000) && (nox::ParseArmorClass(t2->subclassRaw) & 0x4)) {
                        breastByWire[w] = 1;
                        ++nBreast;
                    }
                }
                const uint16_t npWire = tdb.WireByName("NewPlayer");
                // 직업 기본 장비 여섯이 무엇으로 읽히나 — 와이어 번호가 thing.bin 색인과 같다는 것도 같이 본다(이름 → 번호 대조)
                srv.Log("[타격소리표] 가슴갑옷 %zu종 · NewPlayer 번호 %u · 기본 장비:", nBreast, (unsigned)npWire);
                const std::pair<const char*, uint16_t> kLoad[] = {
                    { "StreetShirt", nox::kItemStreetShirt }, { "StreetPants", nox::kItemStreetPants },
                    { "StreetSneakers", nox::kItemStreetSneakers }, { "WizardRobe", nox::kItemWizardRobe },
                    { "WoodenShield", nox::kItemWoodenShield }, { "Longsword", nox::kItemLongsword } };
                for (const auto& ld : kLoad) {
                    const uint16_t w = tdb.WireByName(ld.first);
                    srv.Log(" %s(번호 %u%s·재질 0x%x%s)", ld.first, (unsigned)w, w == ld.second ? "" : " ⚠표와다름",
                                w < matByWire.size() ? matByWire[w] : 0u, (w < breastByWire.size() && breastByWire[w]) ? "·가슴갑옷" : "");
                }
                // ★ 4-c3a — 부를 수 있는 소환수 열셋의 재질(맞으면 기본 타격 소리 `532E20` 가 이 재질로 간다 · 나무·돌·흙이면 부서지는 소리)
                srv.Log(" · 소환수:");
                for (const auto& ms : nox::kMonsters) {
                    const uint16_t w = tdb.WireByName(ms.name);
                    srv.Log(" %s=0x%x", ms.name, w < matByWire.size() ? matByWire[w] : 0u);
                }
                // ★ 4-c3b2 — 날아가는 것·놓인 것·맵 불이 맞히면 이 재질이 「때린 것」이다. NONE(0x4000)이면 사람은 표에 조합이 없어 소리 없음 · 소환수는 거름
                srv.Log(" · 때린 물건:");
                for (const char* nm : { "HarpoonBolt", "Fireball", "StrongFireball", "TitanFireball", "ImpShot", "MercArcherArrow", "Pixie",
                                        "ToxicCloud", "SmallFist", "MediumFist", "LargeFist", "MediumFlame", "FlameCleanse", "BlueFlameCleanse",
                                        "SmallFlameImmobile", "MediumFlameImmobile", "FlameImmobile" }) {
                    const uint16_t w = tdb.WireByName(nm);
                    srv.Log(" %s=%s0x%x", nm, w ? "" : "⚠없음 ", w < matByWire.size() ? matByWire[w] : 0u);
                }
                srv.Log("\n");
                srv.SetMaterialTables(std::move(matByWire), std::move(breastByWire), npWire);
            }
            if (!props.empty()) {
                srv.Log("[다루는 것] 문·오벨리스크 %zu개 — 다가가면 열리고, 옆에 서면 마나가 찬다\n", props.size());
                // ★ 2026-09-18 ② — **자리를 찍는다.** 무인 걷기가 「문 0」으로 끝났을 때
                //   「문이 안 열리는 것」과 「문 근처에 못 간 것」을 못 가른다.
                //   이 좌표를 `--spawn-at` 에 넣으면 문 앞에서 시작할 수 있다.
                srv.Log("[문 자리] 앞 6개 —");
                for (size_t k = 0; k < props.size() && k < 6; ++k)
                    srv.Log(" (%d,%d)", static_cast<int>(props[k].x), static_cast<int>(props[k].y));
                srv.Log("\n");
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
                    srv.Log("[문·벽] 문 %zu개 중 **그 칸에 벽 %zu개 · 둘레 한 칸까지 %zu개**\n", nDoor, onWall, near1);
                }
                // ⑩ — 정적 조회(윗비트)를 쓰는 것이 몇이나 되나. **0 이면 뭔가 틀린 것**이다
                {
                    size_t nStat = 0;
                    for (const auto& w : props) if (w.staticLookup) ++nStat;
                    srv.Log("[판·문 조회] 정적(윗비트) %zu개 / 모두 %zu개\n", nStat, props.size());
                }
                // ★ ④⑩ — **밟는 판**만 따로 찍는다. 그 위를 왕복해야 보는 항목이라
                //   `--spawn-at` 에 넣을 좌표가 필요하다(문 자리에 섞이면 못 고른다).
                {
                    size_t nPlate = 0;
                    for (const auto& w : props) {
                        if (w.kind != nox::PropKind::Plate) continue;
                        if (nPlate == 0) srv.Log("[밟는 판 자리]");
                        if (nPlate < 8) srv.Log(" (%d,%d 번호%u)", static_cast<int>(w.x), static_cast<int>(w.y),
                                                    static_cast<unsigned>(w.netCode));
                        ++nPlate;
                    }
                    if (nPlate) srv.Log("  — 모두 %zu개\n", nPlate);
                }
                // ⑩ — 번호가 15비트를 넘으면 클라 정적 조회로 못 가리킨다. **0 이 아니면 알린다**
                if (propExtentTooBig)
                    srv.Log("[판·문 번호] ⚠ extent 가 15비트를 넘는 것 %zu개 — 그것들은 클라가 못 찾는다\n",
                                propExtentTooBig);
                srv.Log("[문 잠김] 문 %zu개 중 잠긴 것 %zu개 · 못 읽음 %zu — 종류 안잠김 %zu · 은 %zu · 금 %zu · 루비 %zu · 사파이어 %zu · 장치 %zu · 그밖 %zu\n",
                            doorsSeen, doorsLocked, doorsUnread, doorLockKinds[0], doorLockKinds[1], doorLockKinds[2],
                            doorLockKinds[3], doorLockKinds[4], doorLockKinds[5], doorLockKinds[6]);   // ★ 09-29 Z1-A
                srv.Log("[문 짐] 사이 자료 낀 문 %zu · 팀 있는 문 %zu · 각도 31 넘음 %zu · 옛 읽기와 각도 다름 %zu · 짐 끝 어긋남 %zu · 지금 각도≠닫힘 각도 %zu\n",
                            doorsExtra, doorsTeam, doorsAngleBad, doorsAngleFixed, doorsRestOdd, doorsClosedDiff);
                srv.SetWorldProps(std::move(props));
                // ★ 09-22 묶음 9 — 문을 **제 칸의 벽으로** 붙인다(정답지 `doorAttachWall`).
                //   벽과 문이 **둘 다 들어온 뒤**에 불러야 한다
                if (!getenv("NOX_NO_DOOR_WALLS")) srv.AttachDoorWalls();   // A/B 스위치(09-22)
                srv.Log("[문·벽] 문짝 가운데 칸에 벽을 붙였다 — 닫힌 문이 걸음을 막는다(밀어내기는 안 한다) · 옛 식(경첩 칸)과 칸이 다른 문 %zu(옛 칸에 맵 벽 %zu · 새 칸에 맵 벽 %zu)\n",
                            srv.DoorCellDiff(), srv.DoorCellOldHadWall(), srv.DoorCellNewHadWall());
                // ★ 09-29 Z1-A — 열쇠 종류(잠김 1~4)의 와이어 번호(`DoorCheckKey` 는 이름으로 견준다)
                srv.SetKeyTypes(tdb.WireByName("SilverKey"), tdb.WireByName("GoldKey"),
                                tdb.WireByName("RubyKey"), tdb.WireByName("SapphireKey"));
                srv.Log("[열쇠] 은 %u · 금 %u · 루비 %u · 사파이어 %u(와이어 번호 — 0 이면 못 찾음)\n",
                            (unsigned)tdb.WireByName("SilverKey"), (unsigned)tdb.WireByName("GoldKey"),
                            (unsigned)tdb.WireByName("RubyKey"), (unsigned)tdb.WireByName("SapphireKey"));
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
                        srv.Log("[주문 음소없음] %d(%s)\n", id, nm2);
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
            srv.Log("[능력 소리] thing.bin ABIL %zu칸 · 다섯 중 %d개에 쓸 때 소리가 붙었다\n",
                        tdb.Abilities().size(), wired);
        }
        srv.Log("[맵] %s — 벽 %zu칸 · 정적 %zu개(변환 실패 %zu) · 충돌 켜짐\n",
                    mapPath.c_str(), ws.size(), nStat, nSkip);
        srv.Log("[주문] 마나 표 %zu개 — 0 이면 thing.bin 을 못 읽은 것이고 주문이 공짜가 된다\n",
                    srv.SpellCostCount());
        // ★ 09-29 B7 — 비홀더가 쥐는 주문 둘의 깃발(몬스터가 고르는 조건 0x10 · 겨눔 0x4 · 공격 0x20 · 지속 — `castSpell` 갈래)
        srv.Log("[주문] 깃발 — 24(전기 광선) 0x%x · 4(점멸) 0x%x\n", srv.SpellFlags(24), srv.SpellFlags(4));
        // ★ 2026-09-14 묶음 4-a4 — gamedata.bin(기획 상수 · 버프 길이 등). 태그 ARENA
        {
            nox::Gamedata gd;
            if (gd.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\gamedata.bin")) {
                srv.SetGamedata(gd);
                srv.Log("[기획값] gamedata.bin %zu개 · ARENA TargetedSpellLifetime %.0f · InvulnerabilityEnchantDuration %.0f\n",
                            gd.Count(), gd.Float("ARENA", "TargetedSpellLifetime"), gd.Float("ARENA", "InvulnerabilityEnchantDuration"));
            } else {
                srv.Log("[기획값] ⚠ gamedata.bin 못 읽음 — %s\n", gd.Error().c_str());
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
            srv.Log("[반주문] 둘레 %.0f · 지워지는 날아가는 것 %zu종(앞 8:%s) · 끄는 주문 %d개 —%s\n",
                        srv.BalanceFloat("CounterspellRange"), srv.CounterMissileTypeCount(), mis.c_str(), nSp, spells.c_str());
            // ★ 2-a4 — 주문 쓰는 몬스터(monster.bin `CAN_CAST_SPELLS`, `GameBalance.h` 표) → 와이어 번호. 함성 무효화가 이것만 본다
            int nCast = 0;
            std::string miss;
            for (const char* nm : nox::kMonstersCanCastSpells) {
                const uint16_t w = tdb.WireByName(nm);
                if (w) { srv.SetCastingMonsterType(w); ++nCast; }
                else { miss += " "; miss += nm; }
            }
            srv.Log("[주문쓰는몬스터] %d / %zu종%s%s\n", nCast, sizeof(nox::kMonstersCanCastSpells) / sizeof(nox::kMonstersCanCastSpells[0]),
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
            srv.Log("[모드] %s — gameFlags 0x%05X (클라에 가는 값 0x%05X)\n",
                        mode.c_str(), gf, gf & 0x7FFF0u);
        }
        // ★ 아이템이 아니라 걸러진 것 — **맵에 무엇이 있는지 한 번은 봐야 한다.**
        //   문·오벨리스크·레버가 여기 있으면 그것이 다음에 살릴 것이다.
        // ★ 아이템인데 클라가 못 집는 것 — 있으면 이름을 찍는다(조용히 버리지 않는다)
        if (!noWeight.empty()) {
            srv.Log("[집기 불가] CLASS 는 아이템인데 WEIGHT 가 없어 뺀 것 —");
            for (const auto& kv : noWeight) srv.Log(" %sx%d", kv.first.c_str(), kv.second);
            srv.Log("\n");
        }
        // ★ 09-28 — 버린 물건 10초 사라짐의 예외가 맵 물건에 몇 개씩 붙었나(합 = 바닥 물건 수)
        srv.Log("[바닥물건 사라짐] 10초 규칙 걸림 %zu · 10초 예외(RESPAWN %zu · FLAG %zu · Glyph %zu) / %zu"
                    " · 버리기 함수: 기본 %zu · 갑옷무기 %zu · 물약 %zu · 음식 %zu · 안옮김 %zu\n",
                    decayExemptKinds[0], decayExemptKinds[1], decayExemptKinds[2], decayExemptKinds[3], nStat,
                    dropFnKinds[0], dropFnKinds[1], dropFnKinds[2], dropFnKinds[3], dropFnKinds[4]);
        srv.Log("[화살통] 맵 짐 읽음 %zu · 못 읽음 %zu(0/0 으로 둠) · 화살 합 %zu · 최대 합 %zu · 짐 뒤 남은 바이트 합 %zu\n", ammoRead, ammoBad, ammoSum, ammoMaxSum, ammoRest);
        if (!notPickup.empty()) {
            size_t total = 0;
            for (const auto& kv : notPickup) total += static_cast<size_t>(kv.second);
            srv.Log("[맵 밖] 아이템 아닌 것 %zu개 / %zu종 —", total, notPickup.size());
            for (const auto& kv : notPickup) srv.Log(" %sx%d", kv.first.c_str(), kv.second);
            srv.Log("\n");
        }
        // ★ 2026-09-18 ④⑯ — **맵에 그려진 판 물건의 자리**를 찍는다.
        //   우리 서버는 맵의 깃발을 **안 읽고** 제 자리에 놓으므로, 그림과 실물이
        //   얼마나 어긋나는지 보려면 **맵 쪽 좌표**가 있어야 한다.
        //   (확인표 ⑯ 은 「맵 깃발 4개」라고 적고 있었는데 실측은 2개였다 — 09-18 정정)
        if (!mapMatchObjs.empty()) {
            srv.Log("[맵 판물건] %zu개 —", mapMatchObjs.size());
            for (const auto& t : mapMatchObjs) srv.Log(" %s", t.c_str());
            srv.Log("\n");
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
            if (!bare.empty() && bare.size() < 9) out.lanMapName = bare;   // ★ 10-12 작업 7a — 틀에 넣는 것은 프로세스 몫
            srv.Log("[독립 모드] 접속 묶음·USE_MAP·스냅샷을 조립으로 보냄 (%s crc=%08x)\n",
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
            srv.Log("[시야] 정적 오브젝트를 반경 %d 안만 갱신에 실음\n",
                        static_cast<int>(nox::kAoiRadius));
        }
        // ★ 켠 스위치를 찍는다 — 위 「여기가 깨져 있었다」 주석의 교훈.
        if (creatureAoiHalf != 0.0) {
            const double hx = creatureAoiHalf < 0.0 ? 740.0 : creatureAoiHalf;
            const double hy = creatureAoiHalf < 0.0 ? 580.0 : creatureAoiHalf;
            srv.SetCreatureAoi(hx, hy);
            srv.Log("[크리처시야] 켬 — 들어옴 ±%d×±%d · 나감 ±%d×±%d (시야선 없음, 거리만)\n",
                        static_cast<int>(hx), static_cast<int>(hy),
                        static_cast<int>(hx * 1.2), static_cast<int>(hy * 1.2));
            std::fflush(stdout);
        }
        if (friends) {
            srv.SetFriends(true);
            srv.Log("[친구] 켬 — 자기 소환수·같은 팀을 OBJECT_FRIEND_ADD 로 알림 (적대 판정 부분 구현)\n");
            std::fflush(stdout);
        }
        if (autoRespawn) {
            srv.SetAutoRespawn(true);
            srv.Log("[설정] 자동 부활 켬 — 죽고 45틱 뒤 클릭 없이 살아난다\n");
        }
        if (debugHurtEvery > 0) {
            srv.SetDebugHurtEvery(static_cast<uint32_t>(debugHurtEvery), debugHurtFront);
            srv.Log("[진단스위치] %d 틱마다 첫 사람에게 피해 25(종류 0 · 주인 없음)%s\n", debugHurtEvery,
                        debugHurtFront ? " · 앞 30 에서" : "");
        }
        if (debugEnchantB >= 0) {
            srv.SetDebugEnchant(debugEnchantB, debugEnchantP, static_cast<uint32_t>(debugEnchantN));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 버프 %d(세기 %d) 끝없이\n", debugEnchantN, debugEnchantB, debugEnchantP);
        }
        if (debugDummy > 0) {
            srv.SetDebugDummyAfter(static_cast<uint32_t>(debugDummy));
            srv.SetDebugDummyDx(debugDummyDx);
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주인 없는 임프 허수아비를 오른쪽 +%d 에 고정\n", debugDummy, debugDummyDx);
        }
        if (debugOwnPet > 0) {
            srv.SetDebugOwnPetAfter(static_cast<uint32_t>(debugOwnPet));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 그 사람 소유 임프를 사람 자리에 고정\n", debugOwnPet);
            std::fflush(stdout);
        }
        if (debugPetOrder > 0) {
            srv.SetDebugPetOrder(static_cast<uint8_t>(debugPetOrder));
            srv.Log("[진단스위치] 소환수에게 명령 %d 를 준다(2가만히·3지켜·4따라와·5사냥)\n", debugPetOrder);
            std::fflush(stdout);
        }
        if (debugKillAfter > 0) {
            srv.SetDebugKill(static_cast<uint32_t>(debugKillAfter));
            srv.Log("[진단스위치] %d 틱 뒤 첫 사람이 둘째를 죽인다\n", debugKillAfter);
        }
        if (debugFistAfter > 0) {
            srv.SetDebugFist(static_cast<uint32_t>(debugFistAfter), debugFistDx, debugFistDy);
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주먹을 그 사람 +(%d,%d) 에 떨어뜨린다\n",
                        debugFistAfter, debugFistDx, debugFistDy);
            std::fflush(stdout);
        }
        if (debugMobAfter > 0 && debugMobType > 0) {
            srv.SetDebugMob(static_cast<uint32_t>(debugMobAfter), static_cast<uint16_t>(debugMobType),
                            debugMobX, debugMobY);
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 몬스터 %d 를 %d,%d 에 세운다\n",
                        debugMobAfter, debugMobType, debugMobX, debugMobY);
            std::fflush(stdout);
        }
        if (debugWallAfter > 0) {
            srv.SetDebugWall(static_cast<uint32_t>(debugWallAfter), debugWallGx, debugWallGy);
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 비밀벽 칸 %d,%d 을 연다\n",
                        debugWallAfter, debugWallGx, debugWallGy);
            std::fflush(stdout);
        }
        if (debugWarpAfter > 0) {
            srv.SetDebugWarp(static_cast<uint32_t>(debugWarpAfter), debugWarpX, debugWarpY);
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 %d,%d 로 옮긴다\n",
                        debugWarpAfter, debugWarpX, debugWarpY);
            std::fflush(stdout);
        }
        if (debugScoreAfter > 0) {
            srv.SetDebugScore(static_cast<uint32_t>(debugScoreAfter), static_cast<uint32_t>(debugScoreVal));
            srv.Log("[진단스위치] %d 틱 뒤 첫 사람 점수를 %d 로\n", debugScoreAfter, debugScoreVal);
        }
        if (debugTeamEvery > 0) {
            srv.SetDebugTeamEvery(static_cast<uint32_t>(debugTeamEvery));
            srv.Log("[진단스위치] %d 틱마다 첫 사람의 팀을 1<->2 로 오감\n", debugTeamEvery);
        }
        if (debugTeamAfter > 0) {
            srv.SetDebugTeamAfter(static_cast<uint32_t>(debugTeamAfter));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 그 사람 팀을 한 번 바꿈\n", debugTeamAfter);
            std::fflush(stdout);
        }
        if (spawnAtX >= 0 && spawnAtY >= 0) {
            // 첫 접속 자리와 **부활 자리를 함께** 옮긴다 — 죽어도 그 자리로 돌아와야 판정을 이어간다.
            srv.SetSpawn(spawnAtX, spawnAtY);
            srv.SetPlayerStarts({ { static_cast<float>(spawnAtX), static_cast<float>(spawnAtY) } });
            // ★ ①.5 — **걸을 수 있는 자리인지 같이 찍는다.** 09-16 에 발판 무리 한가운데를
            //   `CanStand` 확인 없이 골랐다가 「못 움직인다」를 불렀다. 값이 안 보이면 또 그런다.
            const bool standOk = srv.CanStand(spawnAtX, spawnAtY, nox::kPlayerRadius);
            srv.Log("[시작 자리] 손으로 지정 — (%d, %d) · 부활도 같은 자리 · 설 수 있나 %s\n",
                        spawnAtX, spawnAtY, standOk ? "예" : "**아니오**");
            std::fflush(stdout);
        }
        for (size_t bi = 0; bi < bots.size(); ++bi) {
            // ★ 10-12 작업 7a — 봇은 통신 층 세션이 있어야 선다. 세션을 여는 함수가 없으면(통신 스레드가 이미 도는 뒤에 차리는 방) 안 세운다
            if (!openSession) {
                srv.Log("[진단스위치] 봇은 서버가 켜질 때 차린 방에만 선다 — 안 세움\n");
                std::fflush(stdout);
                break;
            }
            // ★ 자리·번호·편을 정하는 **셈은 `ModeRules.h` 가 한다**(시험이 닿는 자리).
            //   여기 남는 것은 그 값을 서버에 **꽂는 배선**뿐이다.
            const nox::BotPlan plan =
                nox::PlanBot(bi, bots[bi].x, bots[bi].y, srv.PlayerStartCount());
            if (!plan.place) {
                if (bots[bi].x == 0 && bots[bi].y == 0) {
                    srv.Log("[진단스위치] 봇 자리 자동 — 이 맵엔 PlayerStart 가 없다. 봇 안 세움\n");
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
            const int64_t botSid = srv.DebugEnterGame(openSession(bot), bot, plan.netCode, /*observer=*/false, bc);   // ★ 10-08 조각 4 — 세션 번호
            // ⚠ 자리는 **둘 다** 맞춰야 한다 — `DebugMoveTo` 는 `x`·`y` 만, `DebugSetPos` 는 `posX`·`posY` 만 옮긴다.
            //   덫 겨눔·소리 거리는 `posX`·`posY` 로 재고, 걸음·갱신은 `x`·`y` 를 본다(09-15 에 한 번 데었다)
            srv.DebugMoveTo(botSid, bx, by);
            srv.DebugSetPos(botSid, bx, by);
            srv.DebugSetTeam(botSid, plan.team);
            srv.Log("[진단스위치] 가짜 사람 세움 — (%d, %d) · 직업 %s · 번호 %u · 편 %d\n",
                        bx, by, bots[bi].cls.c_str(), (unsigned)plan.netCode, (int)plan.team);
            std::fflush(stdout);
        }
        if (debugXpAfter > 0) {
            srv.SetDebugXpAfter(static_cast<uint32_t>(debugXpAfter), static_cast<float>(debugXpAmount));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 경험치 %.0f 한 번\n", debugXpAfter, debugXpAmount);
            std::fflush(stdout);
        }
        if (debugSpellAfter > 0 && debugSpellId > 0) {
            srv.SetDebugGiveSpellAfter(static_cast<uint32_t>(debugSpellAfter),
                                       static_cast<uint8_t>(debugSpellId));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 주문 %d(%s) 한 번\n", debugSpellAfter,
                        debugSpellId, nox::SpellName(static_cast<uint8_t>(debugSpellId)));
            std::fflush(stdout);
        }
        if (debugConjurerGuides) {
            srv.SetDebugConjurerGuides(true);
            srv.Log("[진단스위치] 들어온 컨저러에게 60틱 뒤 안내서 넷(1 박쥐·7·8 흰거미·24 거미)\n");
            std::fflush(stdout);
        }
        if (creatureAwayAfter > 0) {
            srv.SetDebugCreatureAwayAfter(static_cast<uint32_t>(creatureAwayAfter));
            srv.Log("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 크리처를 좌우 ±300 으로 옮김\n", creatureAwayAfter);
            std::fflush(stdout);
        }
        if (spellStart) srv.SetNoSpellStart(false);
        // ★ 켠 스위치를 찍는다. 안 찍으면 「줬는데 안 먹었다」를 못 가른다.
        if (forcePose >= 0 || forceDir >= 0 || startHealth > 0) {
            srv.Log("[진단스위치] 자세=%d · 방향=%d · 시작체력=%d\n",
                        forcePose, forceDir, startHealth);
            std::fflush(stdout);
        }
        if (forcePose >= 0) srv.SetForceAnimPose(forcePose);
        if (forceDir  >= 0) srv.SetForceAnimDir(forceDir);
        if (forceArrowFrame >= 0) {
            srv.SetForceArrowFrame(forceArrowFrame);
            srv.Log("[진단] 화살 프레임을 %d 로 못 박는다\n", forceArrowFrame);
        }
        if (forceHarpoon) srv.SetForceHarpoon(true);
        if (dumpHarpoon) srv.SetDumpHarpoon(true);
        if (secondNewPlayer) {
            srv.SetNoSecondNewPlayer(false);
            srv.Log("[진단] 옛 동작 — 관전 풀 때 2d NEW_PLAYER 를 다시 보낸다 (--2nd-newplayer)\n");
        }
        if (noSecondNewPlayer) {
            srv.SetNoSecondNewPlayer(true);
            srv.Log("[진단] 관전 풀 때 2d NEW_PLAYER 를 다시 안 보낸다 (--no-2nd-newplayer)\n");
        }
        if (arrowFreeze) {
            srv.SetArrowFreeze(true);
            srv.Log("[진단] 화살을 난 자리에 세워 둔다 (--arrow-freeze)\n");
        }
        if (arrowFrameAlt > 0) {
            srv.SetArrowFrameAlt(arrowFrameAlt);
            srv.Log("[진단] 화살 프레임을 %d 틱마다 0↔16 으로 번갈아 보낸다\n", arrowFrameAlt);
        }
        if (startHealth > 0) srv.SetStartHealth(startHealth);
        if (startMana > 0) srv.SetStartMana(startMana);
        if (itemAtSpawn) {
            // 스폰 자리를 아이템 자리로. 걷기를 빼고 **조작만** 재려는 것이다.
            // ⚠ **아이템을 옮기는 것이 아니다** — 클라가 맵대로 만든 실체와 어긋난다.
            if (srv.DebugSpawnAtStatic())
                srv.Log("[시험] 스폰 자리를 아이템 자리(%d,%d)로 옮겼다 — 걷지 않고 줍기 조작을 잰다\n",
                            srv.spawnX(), srv.spawnY());
            else
                srv.Log("[시험] ⚠ 아이템이 없다 — 스윕이 무효다\n");
        }
    }
    // ★ 10-12 작업 7a — 이 자리에 있던 서버 찾기 답의 틀 넣기(`SetInfoTemplate`)는 프로세스 몫이라 `ConfigureNetwork` 로 옮겼다.

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
                    // ★ 10-12 작업 7a — 틀을 다시 만들어 넣는 것은 프로세스 몫(`ConfigureNetwork`)이 한 번에 한다
                    out.lanMapName.assign(mn, mlen);
                    srv.Log("소개 맵 이름을 서두에 맞춤: %s\n", out.lanMapName.c_str());
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
                srv.Log("서두 %zu개를 흘려보낸 뒤 내 생성으로 이어받는다\n", pre.size());
            }
            if (mirror) {
                std::vector<std::vector<uint8_t>> m = setup.mirror;
                if (mirrorLimit > 0 && static_cast<size_t>(mirrorLimit) < m.size())
                    m.resize(static_cast<size_t>(mirrorLimit));
                MirrorStat(srv, m);
                srv.SetMirror(m);
            }
            srv.Log("덤프에서 뽑음 — 접속 묶음 %zu바이트 · 맵 지정 %zu바이트 · 뒤따르는 것 %zu개\n",
                        setup.joinBody.size(), setup.useMapBody.size(), setup.postMap.size());
        } else if (standalone) {
            srv.Log("[독립 모드] 녹음 서두를 안 읽는다 — 접속 묶음·맵 지정·스냅샷 전부 조립\n");
        } else {
            srv.Log("[경고] 접속 묶음·맵 지정을 못 얻었다. 클라는 접속만 되고 게임에 못 들어간다.\n");
            srv.Log("  찾아본 덤프: %s\n", dumpPath.empty() ? "(없음)" : dumpPath.c_str());
            srv.Log("  --dump <서버쪽 덤프> 로 지정할 것\n");
        }
        // ── 서두와 무관한 일반 설정 — 독립 모드에서도 적용돼야 하므로 밖에 둔다.
        srv.SetPatchSnapshot(!rawSnapshot);
        srv.SetSendUpdates(!noUpdate);
        srv.SetNotifyNewGear(!noNewGear);
        // ★ 10-12 작업 7a — 신뢰 순번 되받기 둘(`SetAckRecvSeq` · `SetAckAnyStream`)은 통신 층 설정이라 `ConfigureNetwork` 로 옮겼다
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
            srv.Log("남에게 넘길 오프코드 %zu개\n", ops.size());
        }
    }
    return 0;
}

int ConfigureNetwork(nox::CUdpServer& net, const ServerOptions& opt, nox::MsgServerInfo& tmpl, const WorldExtras& firstRoom) {
    // 서버 찾기 답의 틀 — 옛 자리에서는 방 차리기 중간에 한 번, 녹음 서두의 맵이면 한 번 더 넣었다(나중 것이 남는다).
    //   통신 스레드를 띄우기 전이라 그 사이에 나가는 답이 없으므로 마지막 값을 한 번 넣는 것과 같다
    if (!firstRoom.lanMapName.empty()) tmpl.mapName = firstRoom.lanMapName;
    uint8_t info[256] = {};
    nox::Writer w(info, sizeof(info));
    if (!tmpl.Encode(w) || !w.ok()) {
        std::printf("소개 원본을 만들지 못했다\n");
        return 1;
    }
    net.SetInfoTemplate(info, w.written());
    net.SetAckRecvSeq(opt.ackSeq);      // B-1b — 신뢰 순번 되받기(기본 끔 · `--ack-seq` 로 켬)
    net.SetAckAnyStream(opt.ackAnyStream);
    return 0;
}
