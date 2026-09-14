// MapFile.h — .map 파일을 서버가 직접 읽는다. 독립 서버 P2.
//
// 포맷 (opennox/libs maps 대조, 2026-09-10):
//   · Blowfish 키 19. **정렬이 암호 블록(8바이트) 단위**로 끼는 것이 thing.bin 과 다르다.
//   · 머리 = magic u32 (FADEFACE 새/FADEBEEF 옛) · 새 판이면 crc(정렬 u32, 블록 하나 소비)
//     · wallOffX u32 · wallOffY u32
//   · 섹션 = 이름(len8 문자열, 읽은 뒤 **다음 블록 경계로 정렬**) + 크기 u64 + 그만큼의 본문
//     이름들: MapInfo·WallMap·FloorMap·ObjectTOC·ObjectData·WayPoints·ScriptObject …
//
// P2-a 는 섹션 목록까지만 — 속을 뜯는 것은 2-b(오브젝트)·2-c(벽)에서.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nox {

struct MapSection {
    std::string name;
    size_t offset = 0;   // 복호 버퍼에서 본문이 시작하는 자리
    size_t size = 0;
};

// 길목(WayPoint) 하나 — 점멸·순간이동이 「어디로 보낼까」를 여기서 고른다.
//
// 형식(정답지 `nox_server_mapRWWaypoints_506260` · GAME4.c) —
//   u16 판 (≤ 4) · u32 개수 · 개수만큼 {
//     u32 번호 · 판<4 면 u32 x·y, 아니면 **f32 x·y**
//     판≥3 이면 u8 이름길이 + 이름
//     u32 깃발 · 판<4 면 u32 이음수, 아니면 **u8 이음수**
//     이음수만큼 { u32 상대번호 · 판≥2 면 u8 이음깃발(아니면 2) }
//   }
// ★ `flags2` 는 파일에 없다 — **오가는 이음깃발을 전부 OR** 한 값이다(`Sub_579A30`).
//   점멸은 `flags2 & 0x80` 이고 `flags & 1` 인 길목만 고른다.
struct MapWaypoint {
    uint32_t id = 0;
    float x = 0, y = 0;
    uint32_t flags = 0;
    uint8_t flags2 = 0;   // 오가는 이음깃발의 OR
};

// 맵에 놓인 오브젝트 하나 — 서버가 세계를 세우는 데 필요한 것만.
//
// ★ 항목의 짐(xfer)은 종류마다 다르지만 **머리 다섯은 공통**이다
//   (opennox/libs xfer_default.go: vers u16 · extent u32 · id u32 · x f32 · y f32).
//   서버는 머리만 읽고 나머지는 크기로 건너뛴다 — 인벤토리·마법부여 따위는
//   맵을 세우는 데 필요 없다.
struct MapObject {
    std::string type;    // 종류 이름 ("NewPlayer", "Tree" …)
    uint32_t extent = 0; // 맵 안 고유 번호
    float x = 0, y = 0;
};

class MapFile {
public:
    bool Load(const std::string& path);

    uint32_t Magic() const { return magic_; }
    // 맵 CRC — 서버가 USE_MAP 에 실어 보내는 바로 그 값이다(와이어 실측과 교차 검증).
    uint32_t Crc() const { return crc_; }
    uint32_t WallOffX() const { return wallOffX_; }
    uint32_t WallOffY() const { return wallOffY_; }
    const std::vector<MapSection>& Sections() const { return sections_; }
    const MapSection* Find(const std::string& name) const {
        for (const auto& s : sections_) if (s.name == name) return &s;
        return nullptr;
    }
    // 섹션 본문에 접근 — Find 로 받은 offset/size 와 짝
    const uint8_t* Data() const { return buf_.data(); }
    const std::string& Error() const { return error_; }

    // ObjectTOC + ObjectData 를 뜯어 맵의 오브젝트 목록을 돌려준다.
    // 실패면 빈 목록 — Error() 에 까닭.
    std::vector<MapObject> ReadObjects();
    // WayPoints 구역을 읽는다. 없거나 판이 4 를 넘으면 빈 목록.
    std::vector<MapWaypoint> ReadWaypoints();

    // ★ 진단 — 첫 항목 몇 개의 짐(원바이트)을 찍는다. 짐 머리 해석이 틀렸을 때
    //   **원바이트를 보고서** 자리표를 다시 잡는 용도다(2026-09-10 좌표 깨짐).
    void DumpFirstObjects(size_t count) const;

    // WallMap → 벽 목록. 격자 칸 좌표(바이트)다 — 월드 좌표가 아니다.
    //   한 칸 = 23 유닛 (녹스 격자 상수. 256칸 × 23 ≈ 5888 로 맵 범위와 맞는다)
    //   ⚠ 실제 배율·오프셋 검증은 P3(벽 돌진)에서 실클라로 한다.
    // ★ 2026-09-13 묶음 2-b — 재질(tile)과 방향 윗비트(flag80)를 더 담는다.
    //   정답지 `GAME1_2.c:1285~1318` nox_server_mapRWWallMap_429B20: [x][y][방향|0x80][재질][변형]…
    //   재질 = thing.bin WALL 순번(깃발 0x2 면 막지 않음) · 윗비트는 Flags4 |= 0x80.
    struct WallCell { uint8_t x = 0, y = 0, dir = 0, tile = 0, variation = 0; bool flag80 = false; };
    std::vector<WallCell> ReadWalls();

private:
    std::vector<uint8_t> buf_;
    std::vector<MapSection> sections_;
    uint32_t magic_ = 0;
    uint32_t crc_ = 0;
    uint32_t wallOffX_ = 0;
    uint32_t wallOffY_ = 0;
    std::string error_;
};

}  // namespace nox
