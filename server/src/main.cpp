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
    int debugTeamAfter = 0;         // 진단 — 첫 사람 들어오고 N 틱 뒤 그 사람 팀을 한 번 바꿈 (0 끔)
    bool noObstacles = false;       // 진단 — 장애물 안 실음(묶음 2-c 대조군)
    bool itemAtSpawn = false;
    int startHealth = 0;       // 진단 — 들어올 때 체력(0 이면 안 건드림)
    int forceDir = -1;         // 진단 — 덧붙이 방향 3비트 고정
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
            if (mode != "arena" && mode != "ctf" && mode != "quest") {
                std::printf("모르는 모드: %s (arena · ctf · quest)\n", mode.c_str());
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
        } else if (a == "--start-health" && i + 1 < argc) {
            // 진단 — 들어올 때 체력을 낮춘다. 죽음·부활을 재려고.
            startHealth = std::atoi(argv[++i]);
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
        } else if (a == "--debug-dummy-dx" && i + 1 < argc) {
            // 진단 — 허수아비 오른쪽 거리(기본 25). 묶음 4-a2 날리기 화면 — 멀리 두면 Magic 이 날아가는 게 보인다.
            debugDummyDx = std::atoi(argv[++i]);
        } else if (a == "--debug-enchant" && i + 3 < argc) {
            // 진단 — 묶음 4-b 화면 판정. 첫 사람(관전 푼 뒤)에게 버프 B 를 세기 P 로 N 틱 뒤 끝없이 건다.
            debugEnchantB = std::atoi(argv[++i]);
            debugEnchantP = std::atoi(argv[++i]);
            debugEnchantN = std::atoi(argv[++i]);
        } else if (a == "--debug-team-after" && i + 1 < argc) {
            // 진단 — RESET_FRIENDS 판정용. 첫 사람의 팀을 N 틱 뒤 한 번 바꾼다.
            debugTeamAfter = std::atoi(argv[++i]);
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

    if (!mapPath.empty()) {
        nox::MapFile mf;
        if (!mf.Load(mapPath)) {
            std::printf("맵을 못 읽음: %s (%s)\n", mapPath.c_str(), mf.Error().c_str());
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
            std::printf("[맵] 비밀벽 %zu개 중 벽이 있는 칸 %zu개\n", sec.size(), srv.SecretWallCount());
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
                std::printf("[벽] 모양 %zu칸 · 재질 %zu종 · 방향 표 밖 %zu칸\n", cells.size(), flags.size(), badDir);
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
                std::printf("[날리기 종류] %s = %u · SPEED %d · 닿음 %s %d%s\n", nm, static_cast<unsigned>(w),
                            t2 ? t2->speed : 0, t2 ? t2->collideFn.c_str() : "?", amount, t2 ? "" : " ⚠ 못 찾음");
            }
            std::vector<nox::Server::WorldStatic> st;
            std::vector<nox::Server::WorldProp> props;
            const auto objs = mf.ReadObjects();
            if (!mf.Error().empty())
                std::printf("[맵] ⚠ 오브젝트 읽기가 도중에 멈춤: %s — 읽은 %zu개만 쓴다\n", mf.Error().c_str(), objs.size());
            // ★ 2026-09-13 묶음 3-c — 부활 자리(PlayerStart). 정답지 mapFindPlayerStart_4F7AB0 가 이 목록에서 고른다.
            {
                std::vector<std::pair<float, float>> starts;
                for (const auto& o : objs)
                    if (o.type == "PlayerStart") starts.push_back({ o.x, o.y });
                std::printf("[부활 자리] PlayerStart %zu곳\n", starts.size());
                srv.SetPlayerStarts(std::move(starts));
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
                srv.SetSoundDefs(sd);
                std::printf("[소리] AUD %zu개 · 이름표에 맞물림 %zu · 소리조각 있는 것 %zu%s\n",
                            tdb.Sounds().size(), sd.size(), withSamples,
                            miss ? " ⚠ 이름표에 없는 것 있음" : "");
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
                }
                if (!traps.empty())
                    std::printf("[맵] 쏘는 덫 %zu개 · 화살 피해 %d\n", traps.size(), arrowDmg);
                srv.SetShootingTraps(traps);
            }
            // ★ 4-e — 맵 길목(WayPoints). 점멸이 여기서 목적지를 고른다.
            //   `flags2 & 0x80`(오가는 이음이 있음) 이고 `flags & 1`(켜짐)인 것만.
            {
                std::vector<std::pair<float, float>> wps;
                for (const auto& w : mf.ReadWaypoints())
                    if ((w.flags2 & 0x80) && (w.flags & 1)) wps.push_back({ w.x, w.y });
                std::printf("[길목] 점멸이 고를 수 있는 곳 %zu곳\n", wps.size());
                srv.SetWaypoints(std::move(wps));
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
                    const bool isDoor = objs[i].type.find("Door") != std::string::npos;
                    const bool isObelisk = objs[i].type.find("Obelisk") != std::string::npos;
                    // ★ 2026-09-15 묶음 9-c — **밟는 판**(`PressurePlate`).
                    //   ⚠ 보이지 않는 `Trigger`(735)는 정답지도 **아무것도 안 보내므로** 안 싣는다
                    const bool isPlate = (objs[i].type == "PressurePlate");
                    if (isDoor || isObelisk || isPlate) {
                        nox::Server::WorldProp wp;
                        wp.netCode = static_cast<uint16_t>(i + 75);
                        wp.type = wire;
                        wp.x = static_cast<int>(objs[i].x);
                        wp.y = static_cast<int>(objs[i].y);
                        wp.kind = isPlate ? nox::PropKind::Plate
                                          : (isDoor ? nox::PropKind::Door : nox::PropKind::Obelisk);
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
                    obs.push_back(b);
                    ++obsNames[o.type];
                }
                std::printf("[장애물] %zu개 / %zu종 —", obs.size(), obsNames.size());
                for (const auto& kv : obsNames) std::printf(" %sx%d", kv.first.c_str(), kv.second);
                std::printf("\n");
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
                    if (!t2 || !t2->isMonster) continue;
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
            std::printf("[애니] 사람 STAT %d개 — 주먹 %d칸/붙듦 %d · 훅 %d칸/붙듦 %d\n", tdb.PlayerAnimCount(),
                        tdb.PlayerAnim(23).frames, tdb.PlayerAnim(23).hold,
                        tdb.PlayerAnim(25).frames, tdb.PlayerAnim(25).hold);
            if (!props.empty()) {
                std::printf("[다루는 것] 문·오벨리스크 %zu개 — 다가가면 열리고, 옆에 서면 마나가 찬다\n", props.size());
                srv.SetWorldProps(std::move(props));
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
                    srv.SetSpellDef(static_cast<uint8_t>(id), sr.mana, sr.flags, sr.phonemes);
                    break;
                }
            }
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
        if (debugTeamAfter > 0) {
            srv.SetDebugTeamAfter(static_cast<uint32_t>(debugTeamAfter));
            std::printf("[진단스위치] 첫 사람 들어오고 %d 틱 뒤 그 사람 팀을 한 번 바꿈\n", debugTeamAfter);
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
        if (startHealth > 0) srv.SetStartHealth(startHealth);
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
        const int needPlayers = (mode == "ctf") ? 2 : 1;
        if (!modeStarted && mode != "arena" && srv.inGameCount() >= needPlayers) {
            modeStarted = true;
            const int mx = srv.spawnX() + 20, my = srv.spawnY();   // 두 사람 사이
            if (mode == "ctf") {
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
                const int redX  = placeFlag(mx - ctfSpan, my, mx);
                const int blueX = placeFlag(mx + ctfSpan, my, mx);
                srv.StartCtf(redX, my, blueX, my);
                std::printf("[모드] 깃발 — (%d,%d)·(%d,%d), 득점 한도 %u%s\n",
                            redX, my, blueX, my, (unsigned)nox::kCaptureLimit,
                            (redX != mx - ctfSpan || blueX != mx + ctfSpan)
                                ? "  ← 벽을 피해 당김" : "");
            } else {
                srv.StartQuest(questCount, mx, my, questSpread);
                // ⚠ **`questCount` 는 이제 안 쓰는 값이다** — 웨이브 마릿수는 스테이지 표가
                //   정한다(`StartQuest` 주석 참조). 전에는 여기서 그 값을 「몬스터 N마리」로
                //   찍어 **실제와 다른 수를 보여 줬다**(설정 4 · 실제 2).
                //   ★ 실제로 몇 마리가 섰는지는 바로 위 `[스테이지]` 줄이 말한다.
                std::printf("[모드] 퀘스트 — (%d,%d) 반경 %.0f · 마릿수는 위 [스테이지] 줄 참조\n",
                            mx, my, questSpread);
            }
            std::fflush(stdout);
        }

        const int ticks = srv.Tick(GetTickCount(), outs);
        if (ticks > 0) {
            nTicks += static_cast<unsigned long long>(ticks);
            const size_t made = outs.size();
            flush();
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
                            "주문 %llu(마나막힘 %llu) · 나는것 %zu · 피해 %llu · 별칭받음 %llu%s"
                            " · 판끝 %s (누적 틱 %llu)\n",
                            srv.firstSnapshotIn(), srv.lastLoopSeen(), srv.lastSessions(),
                            srv.frame(), made, srv.inGameCount(), srv.lastUpdateObjects(),
                            srv.moves(), srv.melees(), srv.code6Seen(),
                            nSpell, srv.spellsNoMana(), srv.missiles(), srv.hurts(), srv.aliasHeard(), modeInfo,
                            srv.RoundOver() ? "예" : "아니오", nTicks);
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
                std::printf("[물건] 줍기요청 %llu(성공 %llu·멂 %llu) · 쓰기 %llu(모르는갈래 %llu)"
                            " · 입기 %llu · 벗기 %llu · 되돌림 %llu\n",
                            srv.tryGets(), srv.picked(), srv.pickTooFar(),
                            srv.used(), srv.useUnknown(),
                            srv.equipped(), srv.dequipped(), srv.dropped());
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
