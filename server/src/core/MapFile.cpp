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
    // ★ 09-20 묶음 6-j(1) — 이음을 **길목마다 남긴다**(전에는 OR 만 하고 버렸다).
    //   떠돌기가 「다음 길목」을 고르는 데 쓴다(`sub_545C60`).
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
            MapWaypointLink l;
            l.to = u32v();
            l.flag = (vers >= 2) ? u8v() : static_cast<uint8_t>(2);
            w.links.push_back(l);
        }
        if (bad) break;
        out.push_back(std::move(w));
    }
    if (bad) { error_ = "waypoint underrun"; out.clear(); return out; }
    // Sub_579A30 — 오가는 이음깃발을 전부 OR (`server/waypoint.go:47`)
    //   나가는 쪽(`wp.Points`)과 **나를 가리키는 쪽**을 둘 다 센다. 이음 자체는 단방향으로 남는다
    for (const auto& src : out) {
        for (const auto& l : src.links) {
            for (auto& w : out) {
                if (w.id == src.id || w.id == l.to) w.flags2 = static_cast<uint8_t>(w.flags2 | l.flag);
            }
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
                // ★ 09-22 — 머리(h+18) 뒤를 그대로 남긴다. 뜻은 종류마다 다르다
                if (sz > h + 18) o.tail.assign(q + h + 18, q + sz);
                o.objVers = (h == 2) ? v1 : v0;       // ★ 09-29 Z1-A
                o.typeVers = (h == 2) ? v0 : 0;
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

// ★ 09-23 nine-defects 4-d2 — FloorMap(선언 주석 참고)
std::vector<MapFile::FloorCell> MapFile::ReadFloor() {
    std::vector<FloorCell> out;
    const MapSection* fs = Find("FloorMap");
    if (!fs) { error_ = "FloorMap missing"; return out; }
    const uint8_t* p = buf_.data() + fs->offset;
    const size_t n = fs->size;
    size_t off = 0;
    auto need = [&](size_t k) { return off + k <= n; };
    auto u8 = [&]() { return p[off++]; };
    auto u16 = [&]() { const uint16_t v = static_cast<uint16_t>(p[off] | (p[off + 1] << 8)); off += 2; return v; };
    if (!need(2 + 16)) { error_ = "FloorMap too short"; return out; }
    const uint16_t ver = u16();
    if (ver != 4) { error_ = "FloorMap version " + std::to_string(ver); return out; }
    off += 16;                                            // u32×4(격자 범위 — 읽기만 하고 안 씀)
    auto readSide = [&](FloorSide& s) {
        if (!need(6)) return false;
        s.present = true;
        s.image = u8();
        s.variant = u16();
        s.a = u8();
        s.b = u8();
        const uint8_t ne = u8();
        for (uint8_t k = 0; k < ne; ++k) {
            if (!need(5)) return false;
            FloorEdge e;
            e.image = u8(); e.variant = u16(); e.a = u8(); e.b = u8();
            s.edges.push_back(e);
        }
        return true;
    };
    for (;;) {
        if (!need(2)) { error_ = "FloorMap unterminated"; out.clear(); return out; }
        const uint16_t k = u16();
        if (k == 0xFFFF) {
            if (off != n) { error_ = "trailing floor data " + std::to_string(n - off); out.clear(); }
            return out;
        }
        FloorCell c;
        c.x = static_cast<uint8_t>((k >> 8) & 0x7F);
        c.y = static_cast<uint8_t>(k & 0x7F);
        if ((k & 0x8000u) && !readSide(c.first)) { error_ = "floor tile underrun"; out.clear(); return out; }
        if ((k & 0x80u) && !readSide(c.second)) { error_ = "floor tile underrun"; out.clear(); return out; }
        out.push_back(std::move(c));
    }
}

// ★ 09-23 nine-defects 4-d3 — 칸 찾기(선언 주석 참고)
void FloorGrid::Build(const std::vector<MapFile::FloorCell>& cells) {
    grid_.assign(128 * 128, MapFile::FloorCell{});
    for (const auto& c : cells) grid_[static_cast<size_t>(c.x) * 128 + c.y] = c;   // 엔진 [가로][세로] — 가로·세로 둘 다 0x7F 안
}

bool FloorGrid::EdgeCovers(int px, int py, int code) {
    const int v2 = px, v3 = py;
    bool v4, v5, v6, v7;
    if (v2 == v3) { v4 = true; v5 = true; } else if (v2 >= v3) { v4 = true; v5 = false; } else { v4 = false; v5 = true; }
    if (v2 == 46 - v3) { v6 = true; v7 = true; } else if (v2 >= 46 - v3) { v6 = true; v7 = false; } else { v6 = false; v7 = true; }
    switch (code) {
        case 0:  return v5 && v6;
        case 1:  return v5;
        case 2:  return v5 && v7;
        case 3:  return v6;
        case 4:  return v7;
        case 5:  return v6 && v4;
        case 6:  return v4;
        case 7:  return v4 && v7;
        case 8:  return v5 || v6;
        case 9:  return v5 || v7;
        case 10: return v4 || v7;
        case 11: return v4 || v6;
        default: return false;
    }
}

int FloorGrid::EdgeCode(int w, int h, int dir) {
    const int v2 = w, v3 = h, a2 = dir;
    if (v2 == 3 && v3 == 3) return a2;
    if (!a2) return 0;
    if (a2 <= v2 - 2) return 1;
    if (a2 == v2 - 1) return 2;
    const int v5 = v2 + 2 * v3 - 4;
    if (a2 < v5) return (((static_cast<unsigned char>(v2) ^ static_cast<unsigned char>(a2)) & 1) != 0) + 3;
    if (a2 == v5) return 5;
    if (a2 > 2 * (v3 + v2) - 6) {
        if (a2 == 2 * (v3 + v2) - 5) return 7;
        return a2 + 2 * (6 - v3 - v2);
    }
    return 6;
}

FloorGrid::Hit FloorGrid::TileAt(float x, float y, const std::vector<EdgeDef>& edges) const {
    Hit h;
    if (grid_.empty()) return h;
    const float v12 = static_cast<float>((static_cast<double>(x) + 11.5) * 0.021739131);
    const float v13 = static_cast<float>((static_cast<double>(y) + 11.5) * 0.021739131);
    const int i = static_cast<int>(v12);
    const int j = static_cast<int>(v13);
    const float v14 = static_cast<float>(static_cast<double>(x) + 11.5);
    const float v15 = static_cast<float>(static_cast<double>(y) + 11.5);
    const int v4 = static_cast<int>(v14) % 46;
    const int v5 = static_cast<int>(v15) % 46;
    if (i - 1 <= 0 || i >= 127 || j - 1 <= 0 || j >= 127) return h;   // -1
    // 마름모 네 삼각형 — 쪽 하나를 고르고, 가장자리가 있으면 `sub_411350` 이 덧칸 번호로 바꿀 수 있다
    const MapFile::FloorSide* side = nullptr;
    int px = 0, py = 0;
    if (v4 <= v5) {
        if (46 - v4 <= v5) { side = &grid_[static_cast<size_t>(i) * 128 + j].second;     px = v4;      py = v5 - 23; }
        else               { side = &grid_[static_cast<size_t>(i - 1) * 128 + j].first; px = v4 + 23; py = v5; }
    } else if (46 - v4 <= v5) { side = &grid_[static_cast<size_t>(i) * 128 + j].first;  px = v4 - 23; py = v5; }
    else                      { side = &grid_[static_cast<size_t>(i) * 128 + (j - 1)].second; px = v4; py = v5 + 23; }
    h.tile = side->image;                                  // 비었으면 0(엔진도 그 칸을 0 으로 둔다)
    h.why = side->present ? Why::Side : Why::Absent;
    // `sub_411350` — 목록을 차례로 보며 모양 안이면 그 가장자리 번호(마지막에 맞은 것이 남는다)
    for (const auto& e : side->edges) {
        if (e.a >= edges.size()) continue;                 // ⚠ 정의 밖 가장자리 번호 — 엔진은 다른 메모리를 읽는다. 우리는 건너뜀
        const int code = EdgeCode(edges[e.a].w, edges[e.a].h, e.b);
        if (EdgeCovers(px, py, code)) { h.tile = e.image; h.why = Why::Edge; }
    }
    return h;
}

// SecretWalls = 판 u16 + 개수 u16 + [비밀벽 23B]*  (판 1 이면 13B — 실측에는 없다)
std::vector<MapFile::SecretWall> MapFile::ReadSecretWalls() {
    std::vector<SecretWall> out;
    const MapSection* ss = Find("SecretWalls");
    if (!ss) return out;                       // 없는 맵도 오류가 아니다(다만 실측 157/157 은 다 있다)
    const uint8_t* p = buf_.data() + ss->offset;
    const size_t n = ss->size;
    if (n < 4) { error_ = "SecretWalls too short"; return out; }
    const uint16_t ver = static_cast<uint16_t>(p[0] | (p[1] << 8));
    const uint16_t cnt = static_cast<uint16_t>(p[2] | (p[3] << 8));
    if (ver > 2) { error_ = "SecretWalls version " + std::to_string(ver); return out; }
    const size_t stride = (ver >= 2) ? 23u : 13u;
    if (4 + static_cast<size_t>(cnt) * stride != n) {
        error_ = "SecretWalls size mismatch";
        return out;
    }
    auto u32 = [](const uint8_t* q) {
        return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
               (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
    };
    out.reserve(cnt);
    for (uint16_t k = 0; k < cnt; ++k) {
        const uint8_t* e = p + 4 + static_cast<size_t>(k) * stride;
        SecretWall w;
        w.gx = static_cast<uint8_t>(u32(e));
        w.gy = static_cast<uint8_t>(u32(e + 4));
        w.waitSecs = u32(e + 8);
        w.flags = e[12];
        if (ver >= 2) { w.state = e[13]; w.anim = e[14]; }
        out.push_back(w);
    }
    return out;
}

// ★ 09-28 — `DestructableWalls`·`WindowWalls` 칸(선언 주석 참고). u16 판(≤maxVer) · u16 개수 · {u32 x · u32 y}×개수
std::vector<std::pair<uint32_t, uint32_t>> MapFile::ReadGridList(const char* section, uint16_t maxVer) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const MapSection* ss = Find(section);
    if (!ss) return out;
    const uint8_t* p = buf_.data() + ss->offset;
    const size_t n = ss->size;
    if (n < 4) { error_ = std::string(section) + " too short"; return out; }
    const uint16_t ver = static_cast<uint16_t>(p[0] | (p[1] << 8));
    const uint16_t cnt = static_cast<uint16_t>(p[2] | (p[3] << 8));
    if (ver > maxVer) { error_ = std::string(section) + " version " + std::to_string(ver); return out; }
    if (4 + static_cast<size_t>(cnt) * 8 != n) { error_ = std::string(section) + " size mismatch"; return out; }
    auto u32 = [](const uint8_t* q) {
        return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
               (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
    };
    out.reserve(cnt);
    for (uint16_t k = 0; k < cnt; ++k) {
        const uint8_t* e = p + 4 + static_cast<size_t>(k) * 8;
        out.push_back({ u32(e), u32(e + 4) });
    }
    return out;
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

// ★ 09-29 Z1-A — 문 짐 읽기(MapFile.h 선언 주석). 사이 자료의 짜임은 `mapReadWriteObjData_4F4530`(`GAME3_3.c:11290~11370`)
//   · 스크립트 몫은 `nox_xxx_xferReadScriptHandler_4F5580`(`server__script__script.c:21~74` — 판 u16 · 길이 u32 · 글 · u32)
bool ReadDoorXfer(const MapObject& o, DoorXfer& d) {
    const std::vector<uint8_t>& t = o.tail;
    size_t k = 0;
    auto need = [&](size_t n) { return k + n <= t.size(); };
    auto u8 = [&]() -> uint32_t { return t[k++]; };
    auto u16 = [&]() -> uint32_t { const uint32_t v = t[k] | (t[k + 1] << 8); k += 2; return v; };
    auto u32 = [&]() -> uint32_t {
        const uint32_t v = t[k] | (t[k + 1] << 8) | (t[k + 2] << 16) | (static_cast<uint32_t>(t[k + 3]) << 24);
        k += 4; return v;
    };
    if (!need(1)) return false;
    d = DoorXfer{};
    if (u8() != 0) {                                  // `:11291~11294` — 0 이면 곧장 돌아간다
        d.extra = true;
        if (!need(4 + 1)) return false;
        u32();                                        // 깃발(`:11297`)
        const uint32_t nameLen = u8();                // 이름(`:11319~11325`)
        if (!need(nameLen + 1 + 1 + 2)) return false;
        k += nameLen;
        d.team = static_cast<uint8_t>(u8());          // +52(`:11326`)
        u8();                                         // 가방 물건 수(`:11331`)
        const uint32_t links = u16();                 // `:11341~11348` — u32 × 수
        if (!need(links * 4 + 4)) return false;
        k += links * 4;
        u32();                                        // XStatus(`:11357`)
        if (o.objVers >= 63) {                        // `:11360`
            if (!need(2 + 4)) return false;
            if (u16() > 1) return false;
            const uint32_t len = u32();
            if (len >= 0x400 || !need(len + 4)) return false;
            k += len;
            u32();
        }
        if (o.objVers >= 64) {                        // `:11361~11363`
            if (!need(4)) return false;
            u32();
        }
    }
    if (!need(8)) return false;
    d.angle = u32();                                  // `:11508`
    d.lock = u32();                                   // `:11509`
    d.closed = d.angle;                               // `:11510~11511` — 판 < 41
    if (o.typeVers >= 41) {                           // `:11513`
        if (!need(4)) return false;
        d.closed = u32();
    }
    d.rest = t.size() - k;
    return true;
}

// ★ 09-29 — 화살통 짐 `nox_xxx_XFerAmmo_4F6B20`(`GAME4.c:509~597`) — 판(u16, 우리 `tail` 앞) · 공통 머리(`mapReadWriteObjData_4F4530` — 위 문과 같은 짜임) ·
//   수식 넷(u8 길이 + 이름) · 화살 수(u8 → 쓰기 짐 [1]) · 최대(u8 → [0]). 퀘스트면 [2] 를 0 으로(우리는 늘 0)
bool ReadAmmoXfer(const MapObject& o, AmmoXfer& a) {
    const std::vector<uint8_t>& t = o.tail;
    size_t k = 0;
    auto need = [&](size_t n) { return k + n <= t.size(); };
    auto u8 = [&]() -> uint32_t { return t[k++]; };
    auto u16 = [&]() -> uint32_t { const uint32_t v = t[k] | (t[k + 1] << 8); k += 2; return v; };
    auto u32 = [&]() -> uint32_t {
        const uint32_t v = t[k] | (t[k + 1] << 8) | (t[k + 2] << 16) | (static_cast<uint32_t>(t[k + 3]) << 24);
        k += 4; return v;
    };
    if (!need(1)) return false;
    a = AmmoXfer{};
    if (u8() != 0) {
        if (!need(4 + 1)) return false;
        u32();
        const uint32_t nameLen = u8();
        if (!need(nameLen + 1 + 1 + 2)) return false;
        k += nameLen;
        u8();
        u8();
        const uint32_t links = u16();
        if (!need(links * 4 + 4)) return false;
        k += links * 4;
        u32();
        if (o.objVers >= 63) {
            if (!need(2 + 4)) return false;
            if (u16() > 1) return false;
            const uint32_t len = u32();
            if (len >= 0x400 || !need(len + 4)) return false;
            k += len;
            u32();
        }
        if (o.objVers >= 64) {
            if (!need(4)) return false;
            u32();
        }
    }
    for (int m = 0; m < 4; ++m) {                     // 수식 넷(`:541~557`)
        if (!need(1)) return false;
        const uint32_t len = u8();
        if (!need(len)) return false;
        k += len;
    }
    if (!need(2)) return false;
    a.count = static_cast<uint8_t>(u8());             // v13 → [1]
    a.max = static_cast<uint8_t>(u8());               // v12 → [0]
    a.rest = t.size() - k;
    return true;
}

}  // namespace nox
