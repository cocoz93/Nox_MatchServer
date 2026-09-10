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
    bool itemAtSpawn = false;   // 시험용 — 아이템 하나를 스폰 자리로 옮긴다
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
        std::vector<std::pair<uint8_t, uint8_t>> ws;
        ws.reserve(cells.size());
        for (const auto& w : cells) ws.emplace_back(w.x, w.y);
        srv.SetWalls(ws);
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
            std::vector<nox::Server::WorldStatic> st;
            std::vector<nox::Server::WorldProp> props;
            const auto objs = mf.ReadObjects();
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
                    if (isDoor || isObelisk) {
                        nox::Server::WorldProp wp;
                        wp.netCode = static_cast<uint16_t>(i + 75);
                        wp.type = wire;
                        wp.x = static_cast<int>(objs[i].x);
                        wp.y = static_cast<int>(objs[i].y);
                        wp.kind = isDoor ? nox::PropKind::Door : nox::PropKind::Obelisk;
                        props.push_back(wp);
                        continue;
                    }
                }
                if (!ti->isPickup) { ++notPickup[objs[i].type]; continue; }
                // ★★ **클라가 집을 수 있는 것만 싣는다.**
                //   클라는 `WEIGHT` 가 있는 물건에만 `ClassPickup` 을 붙인다
                //   (`client/object_type.go:355-362`, 2026-09-11 소스 확인).
                //   우리 CLASS 기준과 어긋나는 것이 10종 있다(화살·차크람 같은 날아가는 것).
                //   실어 봐야 클라가 못 집으니 세계에 놓지 않는다.
                if (!ti->hasWeight) { ++noWeight[objs[i].type]; continue; }
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
                if (ti->useHandler == "PotionUse" || ti->useHandler == "ConsumeUse") {
                    const bool mana =
                        ti->subclassRaw.find("MANA") != std::string::npos;
                    w2.useKind = mana ? nox::UseKind::Mana : nox::UseKind::Health;
                    w2.useAmount = ti->useArg;
                }
                w2.equipBit = ebit;
                w2.isWeapon = isW;
                st.push_back(w2);
            }
            nStat = st.size();
            srv.SetWorldStatics(std::move(st));
            if (!props.empty()) {
                std::printf("[다루는 것] 문·오벨리스크 %zu개 — 다가가면 열리고, 옆에 서면 마나가 찬다\n", props.size());
                srv.SetWorldProps(std::move(props));
            }

            // ★ 주문 마나 비용 — 같은 파일의 SPEL 섹션에서 온다.
            //   주문 번호(와이어)로 표를 만든다: 번호 → 속이름 → 마나.
            //   ⚠ 여기가 0 이면 **모든 주문이 공짜**가 된다. 아래 로그로 바로 보인다.
            for (int id = 0; id < nox::kSpellCount; ++id) {
                const char* nm2 = nox::SpellName(id);
                if (!nm2 || !*nm2) continue;
                bool have = false;
                const uint8_t cost = tdb.ManaByName(nm2, &have);
                if (have) srv.SetSpellCost(static_cast<uint8_t>(id), cost);
            }
        }
        std::printf("[맵] %s — 벽 %zu칸 · 정적 %zu개(변환 실패 %zu) · 충돌 켜짐\n",
                    mapPath.c_str(), ws.size(), nStat, nSkip);
        std::printf("[주문] 마나 표 %zu개 — 0 이면 thing.bin 을 못 읽은 것이고 주문이 공짜가 된다\n",
                    srv.SpellCostCount());
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
        if (aoi) {
            srv.SetAoi(true);
            std::printf("[시야] 정적 오브젝트를 반경 %d 안만 갱신에 실음\n",
                        static_cast<int>(nox::kAoiRadius));
        }
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
        if (!modeStarted && mode != "arena" && srv.inGameCount() >= 2) {
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
                std::printf("[모드] 퀘스트 — 몬스터 %d마리 (%d,%d) 반경 %.0f\n",
                            questCount, mx, my, questSpread);
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
                            "주문 %llu · 나는것 %zu · 피해 %llu(투사체명중 %llu) · 별칭받음 %llu%s"
                            " · 판끝 %s (누적 틱 %llu)\n",
                            srv.firstSnapshotIn(), srv.lastLoopSeen(), srv.lastSessions(),
                            srv.frame(), made, srv.inGameCount(), srv.lastUpdateObjects(),
                            srv.moves(), srv.melees(), srv.code6Seen(),
                            nSpell, srv.flying(), srv.hurts(), srv.hits(), srv.aliasHeard(), modeInfo,
                            srv.RoundOver() ? "예" : "아니오", nTicks);
                // ★ 자리 한 줄 — 하네스가 이걸 읽어 걸어갈 각을 잰다(Server.h PositionsLine 참조).
                std::printf("%s\n", srv.PositionsLine().c_str());

                // ★ **버린 것 한 줄** — 길이를 모르는 메시지를 만나면 그 조각을 통째로
                //   폐기한다(`Server.cpp` 의 `probe.stopped()`). 사람은 눌렀는데
                //   아무 일도 안 일어나고 로그에도 안 남던 것이 이것이다.
                //   여기에 찍어 두면 **놀다가 무엇이 버려지는지**가 바로 보인다.
                //   ⚠ 옆의 이름은 참고용이다. 진짜 판정은 오프코드 번호로 한다.
                // ★ 물건 다루기 — 요청·성공·손이 안 닿음을 **갈라서** 찍는다.
                //   합치면 「안 주워진다」의 까닭이 안 보인다: 요청이 0 이면 조작을 못 찾은 것이고,
                //   요청은 있는데 성공이 0 이면 거리·번호 문제다.
                if (srv.tryGets() || srv.used() || srv.equipped()) {
                    std::printf("[물건] 줍기요청 %llu(성공 %llu·멂 %llu) · 쓰기 %llu(모르는갈래 %llu)"
                                " · 입기 %llu · 벗기 %llu · 되돌림 %llu\n",
                                srv.tryGets(), srv.picked(), srv.pickTooFar(),
                                srv.used(), srv.useUnknown(),
                                srv.equipped(), srv.dequipped(), srv.dropped());
                }
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
