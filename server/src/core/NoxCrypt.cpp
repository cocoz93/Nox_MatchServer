// NoxCrypt.cpp — Blowfish 복호. 표는 BlowfishTables.h(기계 생성).
//
// 알고리즘은 표준 그대로다 — 참조 = golang.org/x/crypto/blowfish (block.go·cipher.go).
//   · ExpandKey: p[i] ^= 키를 4바이트씩 큰 자리부터 돌려 가며 XOR,
//     그 뒤 0 블록을 암호화해 가며 p 와 s 를 차례로 갈아 끼운다
//   · 암호화: 16라운드 Feistel + 마지막 스왑·화이트닝
//
// ★ 검증(돌연변이 포함)은 proto_test 의 TestNoxCrypt 가 한다 —
//   thing.bin 머리 복호가 "FLOR"/"EDGE"/"THNG" 같은 **거꾸로 적힌 섹션 이름**으로
//   시작하는지, 그리고 왕복(복호→암호)이 원문과 같은지.
#include "nox/NoxCrypt.h"

#include <cstdio>
#include "nox/BlowfishTables.h"

namespace nox {

void Blowfish::Init(const uint8_t* key, size_t keyLen) {
    for (int i = 0; i < 18; ++i) p_[i] = bf::kP[i];
    for (int i = 0; i < 256; ++i) {
        s_[0][i] = bf::kS0[i];
        s_[1][i] = bf::kS1[i];
        s_[2][i] = bf::kS2[i];
        s_[3][i] = bf::kS3[i];
    }
    // 키를 4바이트씩(부족하면 되감아) p 에 XOR
    size_t j = 0;
    for (int i = 0; i < 18; ++i) {
        uint32_t d = 0;
        for (int k = 0; k < 4; ++k) {
            d = (d << 8) | key[j];
            j = (j + 1) % keyLen;
        }
        p_[i] ^= d;
    }
    // 0 블록을 굴려 가며 표를 갈아 끼운다
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        EncryptWords(l, r);
        p_[i] = l;
        p_[i + 1] = r;
    }
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 256; i += 2) {
            EncryptWords(l, r);
            s_[b][i] = l;
            s_[b][i + 1] = r;
        }
    }
}

uint32_t Blowfish::Feistel(uint32_t x) const {
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) +
           s_[3][x & 0xFF];
}

void Blowfish::EncryptWords(uint32_t& l, uint32_t& r) const {
    uint32_t xl = l, xr = r;
    xl ^= p_[0];
    for (int i = 1; i < 17; i += 2) {
        xr ^= Feistel(xl) ^ p_[i];
        xl ^= Feistel(xr) ^ p_[i + 1];
    }
    // 위 루프가 (xl,xr) 를 번갈아 굴렸으므로 마지막 스왑이 이 대입에 녹아 있다
    l = xr ^ p_[17];
    r = xl;
}

// 큰 자리 먼저(BE) — golang blowfish 가 이 순서다
static uint32_t RdBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
static void WrBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void Blowfish::EncryptBlock(uint8_t* b) const {
    uint32_t l = RdBE32(b), r = RdBE32(b + 4);
    EncryptWords(l, r);
    WrBE32(b, l);
    WrBE32(b + 4, r);
}

void Blowfish::DecryptBlock(uint8_t* b) const {
    uint32_t xl = RdBE32(b), xr = RdBE32(b + 4);
    // 복호 = p 를 거꾸로
    xl ^= p_[17];
    for (int i = 16; i > 0; i -= 2) {
        xr ^= Feistel(xl) ^ p_[i];
        xl ^= Feistel(xr) ^ p_[i - 1];
    }
    WrBE32(b, xr ^ p_[0]);
    WrBE32(b + 4, xl);
}

std::vector<uint8_t> DecryptFile(const std::string& path, int keyIndex) {
    std::vector<uint8_t> out;
    if (keyIndex < 0 || keyIndex > (896 - 56) / 28) return out;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return out; }
    out.resize(static_cast<size_t>(sz));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);

    // 모자란 꼬리는 버린다 (헤더 주석의 규칙)
    out.resize(got - (got % 8));

    Blowfish c;
    c.Init(bf::kNoxKeys + 28 * keyIndex, 56);
    for (size_t i = 0; i + 8 <= out.size(); i += 8) c.DecryptBlock(out.data() + i);
    return out;
}

}  // namespace nox
