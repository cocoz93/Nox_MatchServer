// MapFile.cpp — .map 섹션 워커. 짜임 근거 = opennox/libs maps reader.go (2026-09-10 대조).
//
// ★ 정렬 규칙이 이 파일의 전부다 — noxcrypt Reader 의 Align/ReadAligned 그대로:
//   · Align       = 오프셋을 다음 8의 배수로 올린다 (경계면 그대로)
//   · ReadAligned = Align 한 뒤 **블록 하나(8바이트)를 통째로 소비**하고 앞 N 만 쓴다
//   · 섹션 이름   = len8 문자열을 읽고 Align
//   · 섹션 크기   = u64 (이름 정렬 직후라 딱 한 블록)
#include "nox/MapFile.h"

#include <cstdio>

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

}  // namespace nox
