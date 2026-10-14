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
    // ★ 10-14 플랜 B 작업 9d — 방 관리자는 방 없이 시작한다. 실행 인자의 방은 아래에서 같은 입구(`CreateRoom`)로 만든다(`srv` 없앰)
    nox::CRoomManager rooms;

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
        // ★ 10-14 작업 9d — 열린 방식의 방은 아래에서 실행 인자의 방을 만든 뒤 정한다(그 방이 처음 만든 방이라 번호 0)
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
    // ★ 10-14 작업 9c — 그 상태는 방(`CRoom`)이 든다 — 방 설정 · 세계 차리기 결과 · 판 차리기 · 진단
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
    // ★ 10-14 플랜 B 작업 9d — 방 만들기 입구 하나(`CreateRoom`) — 방 관리자가 읽기 전용 데이터를 가리키고 방 설정으로 세계까지 차린다.
    //   실행 인자의 방(단독 모드 · 녹음 모드)도 이 입구로. 봇 세션은 통신 층이 여는데 그것은 네트워크 스레드를 띄우기 전에만 된다 —
    //   그래서 이 방을 만든 뒤 여는 함수를 비운다(뒤에 만드는 방은 봇을 안 세운다)
    WorldData world;
    world.files = &gameFiles;
    world.tables = gameTables;
    world.maps = &maps;
    world.openSession = [&udp](const std::string& addr) { return udp.OpenSession(addr); };
    rooms.SetWorldData(world);
    rooms.SetLogRooms(true);   // ★ 10-14 작업 9e — 방을 만들고 닫을 때 한 줄(`[방] N 만듦` · `닫음`)
    int buildRc = 0;
    const int roomId = rooms.CreateRoom(opt.room, &buildRc);
    if (roomId == nox::CRoomManager::kNoRoom) return buildRc;
    world.openSession = nullptr;
    rooms.SetWorldData(world);
    if (opt.noJoinToken) rooms.SetOpenEntry(roomId);
    nox::CRoom& room0 = rooms.Room(roomId);
    if (const int rc = ConfigureNetwork(udp, opt, tmpl, room0.Extras())) return rc;
    // 초당 `[성장]` 줄의 봇 번호 — 통신 층 값이라 스레드를 띄우기 전에 읽어 둔다
    room0.SetBotSession(net.Network().SessionIdOf("bot0"));

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

    // ★ 10-14 작업 9d — 맵 처음 상태 담기 · `[맵처음]` 줄은 방 차리기 끝으로 옮겼다(`BuildRoom` — 방마다)
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
        rooms.ForEachRoom([&](nox::CRoom& room) { room.StartModeIfReady(); });

        // 틱 앞 — 받은 조각에 대한 답 · 판 차리기가 쌓은 요청을 먼저 넘긴다
        rooms.SwapOutNetworkRequests(netRequests);
        net.PushRequests(netRequests);
        const int ticks = rooms.BeginTick(nox::NowMs());   // ★ 10-09 자가검증 ② — 옛 `GetTickCount`(15.6ms 단위) 대신 `Clock.h`
        rooms.ForEachRoom([&](nox::CRoom& room) {   // ★ 10-12 작업 7c — 방마다(그 방의 설정 · 그 방의 프레임)
            const RoomSettings& rs = room.Settings();
            if (rs.debugCrashAfter > 0 && room.Game().frame() >= static_cast<uint32_t>(rs.debugCrashAfter)) {
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
            rooms.ForEachRoom([&](nox::CRoom& room) { room.MarkSendSeen(); });
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
            rooms.ForEachRoom([&](nox::CRoom& room) { room.TickDiagnostics(room.SendsSinceMark()); });
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
