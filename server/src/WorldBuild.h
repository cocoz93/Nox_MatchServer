// WorldBuild.h — 실행 인자 묶음과 세계 차리기.
//
// ★ 10-08 — `main()` 안에 있던 실행 인자 지역 변수와 세계 차리기(맵·설정·접속 묶음)를 옮겨 왔다
//   (`Docs/LAYERS_DESIGN.md` 조각 1). 플랜 B 에서 방마다 같은 차리기를 부르려면 `main()` 밖에 있어야 한다.
//   필드의 기본값과 주석은 `main()` 에 있던 것 그대로다.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "nox/Messages.h"

namespace nox { class Server; }

struct BotWant { int x = -1, y = -1; std::string cls = "warrior"; };

struct ServerOptions {
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
    int debugTeamAfter = 0;         // 진단 — 첫 사람 들어오고 N 틱 뒤 그 사람 팀을 한 번 바꿈 (0 끔)
    int debugTeamEvery = 0;         // 진단 — N틱마다 팀을 1↔2 로 오간다 (0 끔)
    int debugFistAfter = 0, debugFistDx = 40, debugFistDy = 0;
    int debugWarpAfter = 0, debugWarpX = 0, debugWarpY = 0;
    int debugWallAfter = 0, debugWallGx = 0, debugWallGy = 0;
    int debugKillAfter = 0;         // 진단 — N틱 뒤 첫 사람이 둘째를 죽인다 (0 끔)
    int debugScoreAfter = 0;        // 진단 — N틱 뒤 첫 사람 점수를 올린다 (0 끔)
    int debugScoreVal = 0;
    int debugCrashAfter = 0;        // 진단 --debug-crash N — N 틱 뒤 일부러 죽는다(크래시 덤프 확인 · 0 끔 · 10-08)
    int spawnAtX = -1, spawnAtY = -1;   // --spawn-at X Y — 시작·부활 자리를 손으로 지정 (음수면 끔)
    // --debug-bot X Y [직업] — 가짜 사람을 세운다 (음수면 끔 · **여러 번 쓸 수 있다**).
    //   0 0 은 「맵의 시작 자리에 세워라」다. 여러 번 쓰면 시작 자리를 하나씩 옮겨 가며 쓴다.
    std::vector<BotWant> bots;
    int debugXpAfter = 0;           // 진단 --debug-xp N 양 — N 틱 뒤 경험치 한 번 (0 끔)
    double debugXpAmount = 0.0;
    int debugSpellAfter = 0;        // 진단 --debug-give-spell N 주문 — N 틱 뒤 주문 하나 (0 끔)
    int debugSpellId = 0;
    bool debugConjurerGuides = false;   // 진단 --debug-conjurer-guides (09-24)
    bool noObstacles = false;       // 진단 — 장애물 안 실음(묶음 2-c 대조군)
    bool itemAtSpawn = false;
    std::string spawnAtItem;   // ★ 09-27 B6 — `--spawn-at-item <이름>`
    std::vector<std::string> giveItemNames;   // ★ 09-30 — `--give-items <이름,이름…>`(사람 판 시험용)
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
};

// 세계를 차리며 얻어 루프가 다시 쓰는 것.
struct WorldExtras {
    // ★ 2026-09-18 ④⑯ — 맵에 그려진 **깃발 자리**(`Flag` 오브젝트). 둘 이상이면
    //   CTF 를 거기에 차린다. 전에는 늘 스폰 옆에 손으로 놓아 **그림과 실물이 전혀
    //   다른 자리**였다(CapFlag 실측 — 맵 1058,931·3841,3714 ↔ 우리 3828,2534·4104,2534).
    //   맵 로드 블록 밖에서도 봐야 해서 여기에 둔다.
    std::vector<std::pair<int, int>> mapFlagPos;
};

// 실행 인자대로 `srv` 에 세계를 차린다. 0 이면 다 됐고, 아니면 그 값으로 끝낸다(`main` 의 종료 코드).
//   `tmpl`(서버 찾기 답의 틀)은 여기서 맵 이름을 채워 서버에 넘긴다.
int BuildWorld(nox::Server& srv, const ServerOptions& opt, nox::MsgServerInfo& tmpl, WorldExtras& out);
