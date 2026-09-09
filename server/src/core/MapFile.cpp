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
        // ★ 채움 — 경계여도 8 을 먹는다 (위 주석)
        off += 8 - (off % 8);
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
