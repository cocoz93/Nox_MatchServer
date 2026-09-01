// ByteBuffer.h — 와이어 바이트를 읽고 쓰는 최소 도구.
//
// 녹스 프로토콜은 전부 리틀엔디안이고, 문자열은 길이 없이 고정 칸에 넣고
// 남는 자리를 0으로 채우는 방식이다. 그 두 가지만 있으면 된다.
//
// 경계를 넘으면 무조건 false를 돌려주고 쓰기/읽기를 하지 않는다.
// 남의 클라가 보낸 바이트를 다루는 자리라 길이를 믿으면 안 된다.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace nox {

// 고정 칸에 들어간 C 문자열을 읽는다. 0을 만나거나 칸이 끝나면 멈춘다.
inline std::string ReadFixedString(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

// 고정 칸을 0으로 밀고 문자열을 넣는다. 칸보다 길면 잘린다.
// 끝의 0을 위해 한 바이트를 남긴다.
inline void WriteFixedString(uint8_t* p, size_t n, const std::string& s) {
    std::memset(p, 0, n);
    if (n == 0) return;
    const size_t len = s.size() < n - 1 ? s.size() : n - 1;
    std::memcpy(p, s.data(), len);
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t offset() const { return off_; }
    size_t remaining() const { return size_ - off_; }
    bool ok() const { return ok_; }
    bool need(size_t n) const { return ok_ && remaining() >= n; }

    bool U8(uint8_t& out) {
        if (!need(1)) return fail();
        out = data_[off_++];
        return true;
    }

    bool U16(uint16_t& out) {
        if (!need(2)) return fail();
        out = static_cast<uint16_t>(data_[off_]) |
              static_cast<uint16_t>(data_[off_ + 1]) << 8;
        off_ += 2;
        return true;
    }

    bool U32(uint32_t& out) {
        if (!need(4)) return fail();
        out = static_cast<uint32_t>(data_[off_]) |
              static_cast<uint32_t>(data_[off_ + 1]) << 8 |
              static_cast<uint32_t>(data_[off_ + 2]) << 16 |
              static_cast<uint32_t>(data_[off_ + 3]) << 24;
        off_ += 4;
        return true;
    }

    bool Bytes(uint8_t* out, size_t n) {
        if (!need(n)) return fail();
        std::memcpy(out, data_ + off_, n);
        off_ += n;
        return true;
    }

    bool Skip(size_t n) {
        if (!need(n)) return fail();
        off_ += n;
        return true;
    }

    // 고정 칸 문자열. 칸 전체를 소비한다.
    bool FixedString(std::string& out, size_t n) {
        if (!need(n)) return fail();
        out = ReadFixedString(data_ + off_, n);
        off_ += n;
        return true;
    }

    // UTF-16LE 고정 칸. 녹스는 플레이어 이름에만 쓴다.
    // 지금은 아스키 범위만 옮긴다 — 한글 이름이 필요해지면 그때 넓힌다.
    bool FixedString16(std::string& out, size_t n) {
        if (!need(n)) return fail();
        out.clear();
        for (size_t i = 0; i + 1 < n; i += 2) {
            const uint16_t c = static_cast<uint16_t>(data_[off_ + i]) |
                               static_cast<uint16_t>(data_[off_ + i + 1]) << 8;
            if (c == 0) break;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        off_ += n;
        return true;
    }

    // 남은 바이트 전부를 C 문자열로. 메시지 끝에 붙는 가변 이름에 쓴다.
    bool TailString(std::string& out) {
        if (!ok_) return false;
        out = ReadFixedString(data_ + off_, remaining());
        off_ = size_;
        return true;
    }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const uint8_t* data_;
    size_t size_;
    size_t off_ = 0;
    bool ok_ = true;
};

class Writer {
public:
    Writer(uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t written() const { return off_; }
    bool ok() const { return ok_; }
    bool need(size_t n) const { return ok_ && size_ - off_ >= n; }

    bool U8(uint8_t v) {
        if (!need(1)) return fail();
        data_[off_++] = v;
        return true;
    }

    bool U16(uint16_t v) {
        if (!need(2)) return fail();
        data_[off_++] = static_cast<uint8_t>(v);
        data_[off_++] = static_cast<uint8_t>(v >> 8);
        return true;
    }

    bool U32(uint32_t v) {
        if (!need(4)) return fail();
        data_[off_++] = static_cast<uint8_t>(v);
        data_[off_++] = static_cast<uint8_t>(v >> 8);
        data_[off_++] = static_cast<uint8_t>(v >> 16);
        data_[off_++] = static_cast<uint8_t>(v >> 24);
        return true;
    }

    bool Bytes(const uint8_t* p, size_t n) {
        if (!need(n)) return fail();
        std::memcpy(data_ + off_, p, n);
        off_ += n;
        return true;
    }

    // 채우지 않은 자리. 정체를 모르는 칸은 0으로 두고 캡처로 확인한다.
    bool Zero(size_t n) {
        if (!need(n)) return fail();
        std::memset(data_ + off_, 0, n);
        off_ += n;
        return true;
    }

    bool FixedString(const std::string& s, size_t n) {
        if (!need(n)) return fail();
        WriteFixedString(data_ + off_, n, s);
        off_ += n;
        return true;
    }

    bool FixedString16(const std::string& s, size_t n) {
        if (!need(n)) return fail();
        std::memset(data_ + off_, 0, n);
        for (size_t i = 0; i < s.size() && (i + 1) * 2 <= n; ++i) {
            data_[off_ + i * 2] = static_cast<uint8_t>(s[i]);
            data_[off_ + i * 2 + 1] = 0;
        }
        off_ += n;
        return true;
    }

    // 끝에 붙는 가변 문자열. 0으로 닫는다.
    bool TailString(const std::string& s) {
        if (!need(s.size() + 1)) return fail();
        std::memcpy(data_ + off_, s.data(), s.size());
        off_ += s.size();
        data_[off_++] = 0;
        return true;
    }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    uint8_t* data_;
    size_t size_;
    size_t off_ = 0;
    bool ok_ = true;
};

} // namespace nox
