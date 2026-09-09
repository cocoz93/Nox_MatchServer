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

class MapFile {
public:
    bool Load(const std::string& path);

    uint32_t Magic() const { return magic_; }
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

private:
    std::vector<uint8_t> buf_;
    std::vector<MapSection> sections_;
    uint32_t magic_ = 0;
    uint32_t wallOffX_ = 0;
    uint32_t wallOffY_ = 0;
    std::string error_;
};

}  // namespace nox
