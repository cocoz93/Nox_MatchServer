// datadump.cpp — 게임 데이터 파일을 복호해 **속을 내놓는다.** P6 기획데이터 전수검사용.
//
// 왜 있나 — 데미지·사거리·부활시간 같은 「게임의 숫자」가 우리 서버에서 자리표시다.
// 그 값이 **파일에 있는지 없는지**를 가르려면 먼저 파일 속을 봐야 한다.
// 복호는 이미 검증된 NoxCrypt 를 그대로 쓴다(왕복 시험 + thing.bin 2,874종 전수 대조).
//
// ★ 판독 방식은 **꼴에 기대지 않는다** — 복호본에서 인쇄 가능한 글자 줄기를 전부 뽑는다.
//   monster.bin·modifier.bin·gamedata.bin 은 파서가 없어 꼴을 모르는데, 그 셋이야말로
//   이번 검사의 목표다. 「모르는 꼴이라 못 본다」가 되면 검사가 아니다.
//
// 쓰는 법
//   nox_datadump <파일> [--key N] [--out <경로>] [--strings] [--keys]
//                       [--near <글자>] [--span N] [--min N] [--hex N]
//
//     --out      복호본을 그대로 쓴다 (다른 도구로 훑을 때)
//     --strings  글자 줄기를 자리와 함께 전부 찍는다
//     --keys     "KEY = VALUE" 꼴 속성의 **키 이름을 전수 집계**한다 (많은 순)
//     --near     그 글자가 든 줄기 둘레(--span, 기본 400바이트)의 줄기를 찍는다
//     --min      줄기로 칠 최소 길이 (기본 4)
//     --hex      앞 N 바이트를 16진으로
//
//   키 번호는 파일 이름에서 정한다(NoxCrypt.h 의 표) — --key 로 덮어쓸 수 있다.
//
// ⚠ 원본 파일은 **읽기만** 한다.
#include "nox/MapFile.h"
#include "nox/NoxCrypt.h"
#include "nox/Server.h"
#include "nox/ThingDb.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// 파일 이름으로 키 번호를 고른다. 모르면 -1.
int KeyForName(const std::string& path) {
    std::string n = path;
    const size_t sl = n.find_last_of("\\/");
    if (sl != std::string::npos) n = n.substr(sl + 1);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "thing.bin")    return nox::kKeyThingBin;
    if (n == "gamedata.bin") return nox::kKeyGameData;
    if (n == "modifier.bin") return nox::kKeyModifier;
    if (n == "monster.bin")  return nox::kKeyMonster;
    if (n == "soundset.bin") return nox::kKeySoundSet;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".map") == 0) return nox::kKeyMap;
    return -1;
}

struct Run { size_t off; std::string text; };

// 인쇄 가능한 글자 줄기를 뽑는다. 탭·줄바꿈은 끊는 것으로 본다(속성은 한 줄짜리다).
std::vector<Run> Strings(const std::vector<uint8_t>& b, size_t minLen) {
    std::vector<Run> out;
    std::string cur;
    size_t start = 0;
    for (size_t i = 0; i <= b.size(); ++i) {
        const int c = (i < b.size()) ? b[i] : -1;
        const bool ok = (c >= 0x20 && c <= 0x7e);
        if (ok) {
            if (cur.empty()) start = i;
            cur.push_back(static_cast<char>(c));
        } else {
            if (cur.size() >= minLen) out.push_back({ start, cur });
            cur.clear();
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("쓰는 법: nox_datadump <파일> [--key N] [--out 경로] [--strings] [--keys]\n"
                    "                        [--near 글자] [--span N] [--min N] [--hex N]\n");
        return 2;
    }
    const std::string path = argv[1];
    int key = -1;
    std::string outPath, near;
    bool wantStrings = false, wantKeys = false;
    size_t minLen = 4, span = 400, hexN = 0;
    // 벽 지도 — `.map` 을 그림으로 본다. 「벽이냐 조종이냐」를 가르는 자리다.
    int wallX = -1, wallY = -1, wallW = 240, wallH = 120;
    for (int i = 2; i + 2 < argc; ++i) {
        if (std::string(argv[i]) == "--walls") {
            wallX = std::atoi(argv[i + 1]);
            wallY = std::atoi(argv[i + 2]);
            if (i + 4 < argc && argv[i + 3][0] != '-') {
                wallW = std::atoi(argv[i + 3]);
                wallH = std::atoi(argv[i + 4]);
            }
            break;
        }
    }
    // `--spells` — thing.bin 의 SPEL 섹션을 편다. 이름 · 마나 · 미상 둘.
    //
    // 마나 자리는 정품 콘솔 `list spells` 115줄과 **전수 대조로 확정**했다
    // (`_evidence/CONSOLE_list_spells.txt`). 미상 둘은 그대로 둔다.
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) != "--spells") continue;
        nox::ThingDb tdb;
        if (!tdb.Load(path)) {
            std::printf("thing.bin 을 못 읽음: %s (%s)\n", path.c_str(), tdb.Error().c_str());
            return 1;
        }
        std::printf("[주문] %zu개 — 이름 · 마나 · 값 · 깃발 · 음소\n", tdb.Spells().size());
        for (const auto& s : tdb.Spells()) {
            std::printf("  %-28s 마나 %3u  값 %5u  깃발 %08x  음소", s.name.c_str(), s.mana,
                        s.price, s.flags);
            for (uint8_t ph : s.phonemes) std::printf(" %u", ph);
            std::printf("\n");
        }
        return 0;
    }
    // `--pickup` — **우리 기준(CLASS)과 클라 기준(WEIGHT)이 어긋나는지** 잰다.
    //   클라는 `WEIGHT` 가 있는 물건만 집게 해 준다(`object_type.go:355-362`).
    //   우리는 CLASS(WEAPON·ARMOR·FOOD·PICKUP)로 걸러 왔다. 두 집합이 다르면
    //   **우리가 세계에 실은 물건을 클라가 영영 못 집는다.**
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) != "--pickup") continue;
        nox::ThingDb tdb;
        if (!tdb.Load(path)) {
            std::printf("thing.bin 을 못 읽음: %s (%s)\n", path.c_str(), tdb.Error().c_str());
            return 1;
        }
        size_t both = 0, classOnly = 0, weightOnly = 0, neither = 0;
        std::vector<std::string> classOnlyNames;
        for (size_t k = 1; k <= tdb.Count(); ++k) {
            const nox::ThingInfo* ti = tdb.ByWire(static_cast<uint16_t>(k));
            if (!ti) continue;
            if (ti->isPickup && ti->hasWeight) ++both;
            else if (ti->isPickup) {
                ++classOnly;
                if (classOnlyNames.size() < 30) classOnlyNames.push_back(ti->name);
            } else if (ti->hasWeight) ++weightOnly;
            else ++neither;
        }
        std::printf("[집기 기준 대조] 전체 %zu종\n", tdb.Count());
        std::printf("  둘 다 (우리도 클라도 집기)        %zu\n", both);
        std::printf("  ★ 우리만 (CLASS 는 맞는데 WEIGHT 없음) %zu  ← **클라가 못 집는다**\n", classOnly);
        std::printf("  클라만 (WEIGHT 는 있는데 CLASS 밖)  %zu  ← 우리가 안 싣는다\n", weightOnly);
        std::printf("  둘 다 아님                        %zu\n", neither);
        if (!classOnlyNames.empty()) {
            std::printf("  우리만인 것 (앞 %zu):", classOnlyNames.size());
            for (const auto& n2 : classOnlyNames) std::printf(" %s", n2.c_str());
            std::printf("\n");
        }
        return 0;
    }
    if (wallX >= 0) {
        nox::MapFile mf;
        if (!mf.Load(path)) {
            std::printf("맵을 못 읽음: %s (%s)\n", path.c_str(), mf.Error().c_str());
            return 1;
        }
        std::vector<std::pair<uint8_t, uint8_t>> ws;
        for (const auto& w : mf.ReadWalls()) ws.emplace_back(w.x, w.y);
        nox::Server srv;
        srv.SetWalls(ws);
        std::printf("[벽 지도] %s — 가운데 (%d,%d) · 폭 %d 높이 %d · 한 글자 = %d 유닛\n",
                    path.c_str(), wallX, wallY, wallW, wallH,
                    static_cast<int>(nox::kWallCell));
        std::printf("  # 벽 · . 빈 곳 · + 가운데 · o 몸(반지름 %d)이 설 수 있는 곳\n",
                    static_cast<int>(nox::kPlayerRadius));
        const int step = static_cast<int>(nox::kWallCell);
        for (int y = wallY - wallH; y <= wallY + wallH; y += step) {
            std::printf("  %5d ", y);
            for (int x = wallX - wallW; x <= wallX + wallW; x += step) {
                const bool mid = (std::abs(x - wallX) < step / 2) &&
                                 (std::abs(y - wallY) < step / 2);
                char c = srv.WallAt(x, y) ? '#'
                       : (srv.CanStand(x, y, nox::kPlayerRadius) ? 'o' : '.');
                if (mid) c = '+';
                std::printf("%c", c);
            }
            std::printf("\n");
        }
        // ★ 2026-09-13 묶음 2-b — 벽 **모양**(대각선 선분)으로 가운데에서 가장 가까운 벽까지 거리.
        //   걷다 멈춘 자리를 넣으면 반지름(10) 안팎이어야 「벽 선에 붙어 멈췄다」다.
        {
            std::vector<nox::Server::WallTileIn> tiles;
            for (const auto& w : mf.ReadWalls()) {
                nox::Server::WallTileIn t;
                t.x = w.x; t.y = w.y; t.dir = w.dir; t.tile = w.tile; t.flag80 = w.flag80;
                tiles.push_back(t);
            }
            nox::Server shape;
            shape.SetWallTiles(tiles);
            int gx = -1, gy = -1, dir = -1;
            const double dist = shape.DebugWallDistance(wallX, wallY, &gx, &gy, &dir);
            if (dist < 0) std::printf("  가장 가까운 벽 선분 — ±3칸 안에 없음\n");
            else std::printf("  가장 가까운 벽 선분 — 거리 %.1f · 칸 (%d,%d) 방향 %d\n", dist, gx, gy, dir);
        }
        // ★ 묶음 2-c — 폭·높이 안의 장애물(COLLIDE · NO_COLLIDE 아님 · IMMOBILE · 문 아님, main.cpp 와 같은 거르기)
        {
            nox::ThingDb tdb;
            if (tdb.Load("C:\\Program Files\\GOG Galaxy\\Games\\Nox\\thing.bin")) {
                for (const auto& o : mf.ReadObjects()) {
                    if (std::abs(o.x - wallX) > wallW || std::abs(o.y - wallY) > wallH) continue;
                    const uint16_t w = tdb.WireByName(o.type);
                    const nox::ThingInfo* t = w ? tdb.ByWire(w) : nullptr;
                    if (!t || !t->hasCollide) continue;
                    const uint32_t fl = nox::ParseObjectFlags(t->flagsRaw), cl = nox::ParseObjectClass(t->classRaw);
                    if ((fl & 0x40) || !(cl & 0x400000) || (cl & 0x80) || (t->shapeKind != 2 && t->shapeKind != 3)) continue;
                    std::printf("  장애물 %-22s (%.0f,%.0f) 모양 %d r%.0f 상자 %.0fx%.0f 깃발 %s\n", o.type.c_str(), o.x, o.y,
                                t->shapeKind, t->radius, t->boxW, t->boxH, t->flagsRaw.c_str());
                }
            }
        }
        return 0;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--key" && i + 1 < argc)          key = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc)     outPath = argv[++i];
        else if (a == "--near" && i + 1 < argc)    near = argv[++i];
        else if (a == "--span" && i + 1 < argc)    span = (size_t)std::atoi(argv[++i]);
        else if (a == "--min" && i + 1 < argc)     minLen = (size_t)std::atoi(argv[++i]);
        else if (a == "--hex" && i + 1 < argc)     hexN = (size_t)std::atoi(argv[++i]);
        else if (a == "--strings")                 wantStrings = true;
        else if (a == "--keys")                    wantKeys = true;
        else { std::printf("모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (key < 0) key = KeyForName(path);
    if (key < 0) {
        std::printf("키 번호를 모른다 — --key 로 줄 것 (%s)\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> b = nox::DecryptFile(path, key);
    if (b.empty()) {
        std::printf("복호 실패: %s (키 %d)\n", path.c_str(), key);
        return 1;
    }
    std::printf("[파일] %s — 키 %d · 복호 %zu바이트\n", path.c_str(), key, b.size());

    if (!outPath.empty()) {
        FILE* f = std::fopen(outPath.c_str(), "wb");
        if (!f) { std::printf("못 씀: %s\n", outPath.c_str()); return 1; }
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
        std::printf("[씀] %s\n", outPath.c_str());
    }
    if (hexN) {
        const size_t n = (hexN < b.size()) ? hexN : b.size();
        for (size_t i = 0; i < n; i += 16) {
            std::printf("%08zx ", i);
            for (size_t j = i; j < i + 16 && j < n; ++j) std::printf("%02x ", b[j]);
            std::printf(" |");
            for (size_t j = i; j < i + 16 && j < n; ++j)
                std::printf("%c", (b[j] >= 0x20 && b[j] <= 0x7e) ? b[j] : '.');
            std::printf("|\n");
        }
    }

    const std::vector<Run> runs = Strings(b, minLen);
    std::printf("[줄기] %zu개 (최소 길이 %zu)\n", runs.size(), minLen);

    if (wantKeys) {
        // "KEY = VALUE" 의 KEY 를 센다. 값이 숫자인 것도 따로 센다 —
        // **이번 검사에서 찾는 것은 「숫자를 담은 키」**다.
        std::map<std::string, size_t> all, numeric;
        for (const Run& r : runs) {
            const size_t eq = r.text.find(" = ");
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = r.text.substr(0, eq);
            // 키 앞에 붙은 공백·쓰레기 글자를 턴다
            while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
            if (k.empty()) continue;
            ++all[k];
            const std::string v = r.text.substr(eq + 3);
            bool num = !v.empty();
            for (char c : v) {
                if (!(std::isdigit((unsigned char)c) || c == '-' || c == '.' || c == ' ')) { num = false; break; }
            }
            if (num) ++numeric[k];
        }
        std::vector<std::pair<std::string, size_t>> v(all.begin(), all.end());
        std::sort(v.begin(), v.end(),
                  [](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b2) {
                      return a.second > b2.second;
                  });
        std::printf("[키 집계] %zu종 — (숫자값 건수/전체)\n", v.size());
        for (const auto& kv : v)
            std::printf("  %-28s %6zu  (숫자 %zu)\n", kv.first.c_str(), kv.second,
                        numeric.count(kv.first) ? numeric[kv.first] : 0);
    }

    if (!near.empty()) {
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].text.find(near) == std::string::npos) continue;
            const size_t lo = (runs[i].off > span) ? runs[i].off - span : 0;
            const size_t hi = runs[i].off + span;
            std::printf("--- %s @%08zx ---\n", runs[i].text.c_str(), runs[i].off);
            for (const Run& r : runs)
                if (r.off >= lo && r.off <= hi) std::printf("  %08zx  %s\n", r.off, r.text.c_str());
        }
    }

    if (wantStrings)
        for (const Run& r : runs) std::printf("%08zx  %s\n", r.off, r.text.c_str());
    return 0;
}
