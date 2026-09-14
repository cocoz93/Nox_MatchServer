// MapFile.cpp — .map 섹션 워커. 짜임 근거 = opennox/libs maps reader.go (2026-09-10 대조).
//
// ★ 정렬 규칙이 이 파일의 전부다 — noxcrypt Reader 의 Align/ReadAligned 그대로:
//   · Align       = 오프셋을 다음 8의 배수로 올린다 (경계면 그대로)
//   · ReadAligned = Align 한 뒤 **블록 하나(8바이트)를 통째로 소비**하고 앞 N 만 쓴다
//   · 섹션 이름   = len8 문자열을 읽고 Align
//   · 섹션 크기   = u64 (이름 정렬 직후라 딱 한 블록)
#include "nox/MapFile.h"

#include <cstdio>
#include <cstring>

#include "nox/NoxCrypt.h"

namespace nox {

bool MapFile::Load(const std::string& path) {
    buf_.clear();
    sections_.clear();
    error_.clear();

    buf_ = DecryptFile(path, kKeyMap);
    if (buf_.empty()) { error_ = "cannot read/decrypt " + path; return false; }

    size_t off = 0;
    auto need = [&](size_t k) {
        if (off + k > buf_.size()) { if (error_.empty()) error_ = "underrun at " + std::to_string(off); return false; }
        return true;
    };
    auto u32 = [&]() -> uint32_t {
        if (!need(4)) return 0;
        uint32_t v = buf_[off] | (buf_[off+1] << 8) | (buf_[off+2] << 16) |
                     (static_cast<uint32_t>(buf_[off+3]) << 24);
        off += 4; return v;
    };
    auto u64 = [&]() -> uint64_t {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | buf_[off + i];
        off += 8; return v;
    };
    auto align = [&]() { const size_t over = off % 8; if (over) off += 8 - over; };

    magic_ = u32();
    if (!error_.empty()) return false;
    if (magic_ == 0xFADEFACE) {
        align();               // crc 는 정렬 u32 — 블록 하나를 통째로 먹는다
        if (!need(8)) return false;
        crc_ = buf_[off] | (buf_[off + 1] << 8) | (buf_[off + 2] << 16) |
               (static_cast<uint32_t>(buf_[off + 3]) << 24);
        off += 8;
    } else if (magic_ != 0xFADEBEEF) {
        char h[32];
        std::snprintf(h, sizeof(h), "bad magic 0x%08x", magic_);
        error_ = h;
        return false;
    }
    wallOffX_ = u32();
    wallOffY_ = u32();
    if (!error_.empty()) return false;

    // 섹션 루프 — 이름(len8)+정렬, 크기 u64, 본문
    for (;;) {
        if (off >= buf_.size()) break;   // EOF = 정상 종료
        if (!need(1)) break;
        const uint8_t nameLen = buf_[off++];
        if (off + nameLen > buf_.size()) break;   // 끝의 0 채움에 닿았다
        std::string name(reinterpret_cast<const char*>(buf_.data() + off), nameLen);
        // 문자열은 0 에서 끊는다 (readStringFixed 규칙)
        const size_t z = name.find('\0');
        if (z != std::string::npos) name.resize(z);
        off += nameLen;
        align();
        if (name.empty()) break;         // 빈 이름 = 더 없다
        const uint64_t size = u64();
        if (!error_.empty()) return false;
        if (off + size > buf_.size()) {
            error_ = "section '" + name + "' size " + std::to_string(size) +
                     " overruns at " + std::to_string(off);
            return false;
        }
        MapSection s;
        s.name = std::move(name);
        s.offset = off;
        s.size = static_cast<size_t>(size);
        sections_.push_back(std::move(s));
        off += static_cast<size_t>(size);
    }

    if (sections_.empty()) { error_ = "no sections"; return false; }
    return true;
}

// ObjectTOC + ObjectData → 오브젝트 목록. 짜임 = maps/map_objects.go ReadObjects.
//
//   TOC  = vers u16 · cnt u16 · [번호 u16 + 이름(len8)]*
//   Data = vers u16 · [번호 u16(0=끝) · **채움** · 크기 u64 · 짐(크기바이트)]*
//
// ★ 채움 규칙이 함정이다 — binenc 의 `Align(+2)` 는 `ReadNext(8-(off+2)%8)` 인데
//   off 는 **vers 를 뗀 자리** 기준이라, 섹션 본문 기준으로 셈하면
//   `8 - (본문오프셋 % 8)` 이고 **경계에 서 있어도 8 을 먹는다.**
// ❌ 정정(2026-09-13 묶음 3-c) — **경계면 안 먹는다.** 위 규칙은 libs 쓰기 쪽 식이고 정답지 읽기는
//   `cryptfile.ReadAlignedU32`(internal/cryptfile/cryptfile.go:319 → File.ReadAligned) — 암호 블록(8바이트) 경계로
//   맞춰 읽으니 이미 경계면 채움 0 이다. 옛 규칙은 estate 161번째(ColorLight 가 딱 경계에서 끝남)에서 크기를 잘못 읽고
//   **「object overrun」으로 조용히 멈췄다** — 오브젝트 640개 중 161개만 읽혀 PlayerStart 14개가 통째로 빠졌다.
//   고친 규칙으로 640개가 섹션 끝 바이트에서 정확히 끝난다(`_evidence` 없이 scratchpad p31/mapobjs.py 로 전수 확인).
// WayPoints 구역 — 정답지 `nox_server_mapRWWaypoints_506260`(GAME4.c) 그대로 읽는다.
//   ⚠ `flags2` 는 **파일에 없다.** 오가는 이음깃발을 전부 OR 해서 만든다(`Sub_579A30`).
std::vector<MapWaypoint> MapFile::ReadWaypoints() {
    std::vector<MapWaypoint> out;
    const MapSection* sec = Find("WayPoints");
    if (!sec) return out;
    const uint8_t* p = buf_.data() + sec->offset;
    size_t off = 0;
    const size_t n = sec->size;
    bool bad = false;
    auto need = [&](size_t k) { if (off + k > n) { bad = true; return false; } return true; };
    auto u8v  = [&]() -> uint8_t  { if (!need(1)) return 0; return p[off++]; };
    auto u16v = [&]() -> uint16_t { if (!need(2)) return 0; uint16_t v = static_cast<uint16_t>(p[off] | (p[off + 1] << 8)); off += 2; return v; };
    auto u32v = [&]() -> uint32_t {
        if (!need(4)) return 0;
        uint32_t v = static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8)
                   | (static_cast<uint32_t>(p[off + 2]) << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
        off += 4;
        return v;
    };
    auto f32v = [&]() -> float { const uint32_t b = u32v(); float f; std::memcpy(&f, &b, 4); return f; };

    const uint16_t vers = u16v();
    if (bad || vers > 4) { error_ = "waypoint vers " + std::to_string(vers); return out; }
    const uint32_t cnt = u32v();
    if (bad) return out;
    // 이음은 뒤에서 OR 해야 하니 한 번 담아 둔다
    struct Link { uint32_t from; uint32_t to; uint8_t flag; };
    std::vector<Link> links;
    for (uint32_t i = 0; i < cnt && !bad; ++i) {
        MapWaypoint w;
        w.id = u32v();
        if (vers < 4) { w.x = static_cast<float>(u32v()); w.y = static_cast<float>(u32v()); }
        else          { w.x = f32v(); w.y = f32v(); }
        if (vers >= 3) {                                     // 이름 — 서버는 안 쓴다
            const uint8_t len = u8v();
            if (!need(len)) break;
            off += len;
        }
        w.flags = u32v();
        const uint32_t nlink = (vers < 4) ? u32v() : u8v();
        for (uint32_t k = 0; k < nlink && !bad; ++k) {
            Link l;
            l.from = w.id;
            l.to = u32v();
            l.flag = (vers >= 2) ? u8v() : static_cast<uint8_t>(2);
            links.push_back(l);
        }
        if (bad) break;
        out.push_back(w);
    }
    if (bad) { error_ = "waypoint underrun"; out.clear(); return out; }
    // Sub_579A30 — 오가는 이음깃발을 전부 OR
    for (const auto& l : links) {
        for (auto& w : out) {
            if (w.id == l.from || w.id == l.to) w.flags2 = static_cast<uint8_t>(w.flags2 | l.flag);
        }
    }
    return out;
}

std::vector<MapObject> MapFile::ReadObjects() {
    std::vector<MapObject> out;
    const MapSection* toc = Find("ObjectTOC");
    const MapSection* dat = Find("ObjectData");
    if (!toc || !dat) { error_ = "ObjectTOC/ObjectData missing"; return out; }

    // ── TOC
    std::vector<std::pair<uint16_t, std::string>> types;
    {
        const uint8_t* p = buf_.data() + toc->offset;
        size_t off = 0, n = toc->size;
        auto u16 = [&]() -> uint16_t {
            if (off + 2 > n) return 0;
            uint16_t v = p[off] | (p[off + 1] << 8); off += 2; return v;
        };
        const uint16_t vers = u16();
        if (vers > 1) { error_ = "toc vers " + std::to_string(vers); return out; }
        const uint16_t cnt = u16();
        for (uint16_t i = 0; i < cnt; ++i) {
            const uint16_t ind = u16();
            if (off >= n) { error_ = "toc underrun"; return out; }
            const uint8_t len = p[off++];
            if (off + len > n) { error_ = "toc name underrun"; return out; }
            types.emplace_back(ind, std::string(reinterpret_cast<const char*>(p + off), len));
            off += len;
        }
    }
    auto typeName = [&](uint16_t ind) -> const std::string* {
        for (const auto& t : types) if (t.first == ind) return &t.second;
        return nullptr;
    };

    // ── Data
    const uint8_t* p = buf_.data() + dat->offset;
    size_t off = 0;
    const size_t n = dat->size;
    auto u16 = [&]() -> uint16_t {
        if (off + 2 > n) { off = n + 1; return 0; }
        uint16_t v = p[off] | (p[off + 1] << 8); off += 2; return v;
    };
    const uint16_t vers = u16();
    if (vers > 1) { error_ = "data vers " + std::to_string(vers); return out; }

    for (;;) {
        const uint16_t ind = u16();
        if (ind == 0 || off > n) break;
        // ★ 채움 — 암호 블록 경계까지. 이미 경계면 0 (위 정정)
        off += (8 - (off % 8)) % 8;
        if (off + 8 > n) { error_ = "size underrun"; return out; }
        uint64_t sz = 0;
        for (int i = 7; i >= 0; --i) sz = (sz << 8) | p[off + i];
        off += 8;
        if (off + sz > n) { error_ = "object overrun"; return out; }

        const std::string* ty = typeName(ind);
        if (!ty) { error_ = "toc entry missing " + std::to_string(ind); return out; }

        // 짐 머리 — 두 꼴이 있다 (2026-09-10 estate 원바이트로 확정):
        //
        //   [Object 직접]      vers u16(61~64) · extent u32 · id u32 · x f32 · y f32 …
        //   [타입xfer + Object] 타입vers u16 · **Object vers u16(61~64)** · extent · id · x · y …
        //
        //   실측 — ObeliskPrimitive 짐 `3d 00 | 40 00 | fd 0b 00 00 | 00…00 | 00 10 58 45 …`
        //   = 타입vers 61 · Object vers 64 · extent 3069 · id 0 · x 3457.0 · y 3028.0.
        //   판별은 「어느 u16 이 61~64 인가」로 한다. 나머지 짐은 크기로 건너뛴다.
        if (sz >= 20) {
            const uint8_t* q = p + off;
            const uint16_t v0 = q[0] | (q[1] << 8);
            const uint16_t v1 = q[2] | (q[3] << 8);
            size_t h = SIZE_MAX;               // Object 머리가 시작하는 자리
            if (v1 >= 61 && v1 <= 64) h = 2;   // 타입xfer vers 가 앞에 하나 더
            else if (v0 >= 61 && v0 <= 64) h = 0;
            if (h != SIZE_MAX && sz >= h + 18) {
                MapObject o;
                o.type = *ty;
                o.extent = q[h + 2] | (q[h + 3] << 8) | (q[h + 4] << 16) |
                           (static_cast<uint32_t>(q[h + 5]) << 24);
                float fx, fy;
                std::memcpy(&fx, q + h + 10, 4);
                std::memcpy(&fy, q + h + 14, 4);
                o.x = fx;
                o.y = fy;
                out.push_back(std::move(o));
            }
        }
        off += static_cast<size_t>(sz);
    }
    return out;
}

// WallMap = 격자머리 18B(u16 + u32×4) + [벽 7B]* + 끝 0xff.
//   벽 7B = x · y · 방향(위 비트는 신호) · 재질 · 변형 · 미니맵 · 손댐 (map_walls.go)
std::vector<MapFile::WallCell> MapFile::ReadWalls() {
    std::vector<WallCell> out;
    const MapSection* ws = Find("WallMap");
    if (!ws) { error_ = "WallMap missing"; return out; }
    const uint8_t* p = buf_.data() + ws->offset;
    const size_t n = ws->size;
    size_t off = 18;   // 격자 머리
    for (;;) {
        if (off >= n) { error_ = "wall list unterminated"; out.clear(); return out; }
        const uint8_t x = p[off];
        if (x == 0xff) {
            ++off;
            // 종결 뒤에 남는 게 있으면 걸음이 밀린 것이다 (Go 도 trailing 을 오류로 본다)
            if (off != n) { error_ = "trailing wall data " + std::to_string(n - off); out.clear(); }
            return out;
        }
        if (off + 7 > n) { error_ = "wall entry underrun"; out.clear(); return out; }
        WallCell w;
        w.x = x;
        w.y = p[off + 1];
        w.dir = p[off + 2] & 0x7f;
        w.flag80 = (p[off + 2] & 0x80) != 0;
        w.tile = p[off + 3];
        w.variation = p[off + 4];
        out.push_back(w);
        off += 7;
    }
}

void MapFile::DumpFirstObjects(size_t count) const {
    const MapSection* toc = Find("ObjectTOC");
    const MapSection* dat = Find("ObjectData");
    if (!toc || !dat) return;
    const uint8_t* p = buf_.data() + dat->offset;
    const size_t n = dat->size;
    size_t off = 2;   // vers
    for (size_t k = 0; k < count && off + 2 <= n; ++k) {
        const uint16_t ind = p[off] | (p[off + 1] << 8);
        off += 2;
        if (ind == 0) break;
        off += 8 - (off % 8);
        if (off + 8 > n) break;
        uint64_t sz = 0;
        for (int i = 7; i >= 0; --i) sz = (sz << 8) | p[off + i];
        off += 8;
        if (off + sz > n) break;
        std::printf("    [짐%zu] 번호=%u 크기=%llu | ", k, ind,
                    static_cast<unsigned long long>(sz));
        for (size_t i = 0; i < sz && i < 40; ++i) std::printf("%02x ", p[off + i]);
        std::printf("\n");
        off += static_cast<size_t>(sz);
    }
}

}  // namespace nox
