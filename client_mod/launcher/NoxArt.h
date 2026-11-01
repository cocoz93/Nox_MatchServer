// NoxArt.h — 런처 화면에 쓸 정품 그림을 실행 때 사용자 PC 의 게임 폴더에서 뽑는다(런처 P10 · 11-01 사용자 결정 1 · 리포에 그림 안 넣음).
//   video.idx + video.bag → 그림 14장을 PNG 로 · default.fnt 는 그대로 복사 → %LOCALAPPDATA%\NoxOnline\art(화면이 https://art.nox/ 로 읽음)
//   한 번 뽑으면 art.stamp(판 번호 · 원본 크기)가 같은 동안 다시 안 뽑는다. 못 뽑으면 false — 화면은 정품 그림 없는 모양으로 뜬다(로그인은 그대로 됨)
//
//   형식은 opennox/libs(MIT · THIRD_PARTY.md)의 bag · nxz · noximage/pcx 를 읽고 C++ 로 옮겼다.
//   그림 이름 → 번호는 thing.bin 의 IMAG 표에 있지만 thing.bin 은 암호화되어 있어 복호 표가 필요하다. 런처엔 필요한 번호만 박고(GOG 판 기준)
//   크기가 표와 다르면 다른 판으로 보고 포기한다.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace noxart {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // 줄마다 왼쪽부터 R G B A · 곱하지 않은 알파
};

// ── NXZ 풀기(허프만 + LZ77 · 비트는 앞쪽부터) — libs/nxz
class NxzReader {
public:
    NxzReader(const uint8_t* data, size_t size) : d_(data), n_(size), win_(kWindow)
    {
        static const int kInitial[16][2] = { { 2, 0x00 },  { 3, 0x04 },  { 3, 0x0C },  { 4, 0x14 }, { 4, 0x24 }, { 4, 0x34 }, { 4, 0x44 }, { 4, 0x54 },
                                             { 4, 0x64 },  { 4, 0x74 },  { 4, 0x84 },  { 4, 0x94 }, { 4, 0xA4 }, { 5, 0xB4 }, { 5, 0xD4 }, { 5, 0xF4 } };
        for (int i = 0; i < 16; ++i) {
            bits_[i] = kInitial[i][0];
            offs_[i] = kInitial[i][1];
        }
        int pos = 0;
        for (; pos < 16; ++pos) sym_[pos] = pos + 0x100;
        sym_[pos++] = 0;
        sym_[pos++] = 0x20;
        sym_[pos++] = 0x30;
        sym_[pos++] = 0xFF;
        for (int i = 1; i <= 0x111; ++i) {
            if (i == 0x20 || i == 0x30 || i == 0xFF || (i >= 0x100 && i < 0x110)) continue;
            sym_[pos++] = i;
        }
    }

    bool Decode(std::vector<uint8_t>& out, size_t size)
    {
        out.clear();
        out.reserve(size + 512);
        static const int kLen[8][2] = { { 1, 0x008 }, { 2, 0x00A }, { 3, 0x00E }, { 4, 0x016 }, { 5, 0x026 }, { 6, 0x046 }, { 7, 0x086 }, { 8, 0x106 } };
        static const int kDist[8][2] = { { 0, 0x00 }, { 0, 0x01 }, { 1, 0x02 }, { 2, 0x04 }, { 3, 0x08 }, { 4, 0x10 }, { 5, 0x20 }, { 6, 0x40 } };
        while (out.size() < size) {
            uint32_t code = 0, ind = 0;
            if (!Bits(4, code)) return false;
            if (!Bits(bits_[code], ind)) return false;
            ind += offs_[code];
            if (ind >= kSymbols) return false;
            const int sym = sym_[ind];
            ++cnt_[sym];
            if (sym < 0x100) {
                Emit(out, static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 0x110) {
                if (!Rebuild()) return false;
                continue;
            }
            if (sym > 0x110) continue;   // libs 와 같이 그냥 넘김
            uint32_t len = 4;
            if (sym < 0x108) {
                len += sym - 0x100;
            } else {
                uint32_t more = 0;
                const int* t = kLen[sym - 0x108];
                if (!Bits(t[0], more)) return false;
                len += t[1] + more;
            }
            uint32_t dc = 0, low = 0;
            if (!Bits(3, dc)) return false;
            if (!Bits(kDist[dc][0] + 9, low)) return false;
            const uint32_t dist = (static_cast<uint32_t>(kDist[dc][1]) << 9) + low;
            const uint32_t from = wpos_ - dist;
            for (uint32_t i = 0; i < len; ++i) Emit(out, win_[(from + i) & (kWindow - 1)]);
        }
        out.resize(size);
        return true;
    }

private:
    static constexpr uint32_t kSymbols = 274, kWindow = 0x10000;

    void Emit(std::vector<uint8_t>& out, uint8_t b)
    {
        out.push_back(b);
        win_[wpos_ & (kWindow - 1)] = b;
        ++wpos_;
    }

    bool Bits(int n, uint32_t& v)
    {
        v = 0;
        if (n < 0 || n > 24) return false;
        for (int i = 0; i < n; ++i) {
            const size_t byte = bit_ >> 3;
            if (byte >= n_) return false;
            v = (v << 1) | ((d_[byte] >> (7 - (bit_ & 7))) & 1u);
            ++bit_;
        }
        return true;
    }

    // 많이 나온 기호가 앞으로(같으면 번호가 큰 쪽이 앞) · 셈 반으로 · 다음 표는 스트림에서
    bool Rebuild()
    {
        int order[kSymbols];
        for (uint32_t i = 0; i < kSymbols; ++i) order[i] = static_cast<int>(i);
        std::sort(order, order + kSymbols, [this](int a, int b) {
            const int64_t ka = (static_cast<int64_t>(cnt_[a]) << 16) + a, kb = (static_cast<int64_t>(cnt_[b]) << 16) + b;
            return ka > kb;
        });
        for (uint32_t i = 0; i < kSymbols; ++i) sym_[i] = order[i];
        for (uint32_t i = 0; i < kSymbols; ++i) cnt_[i] /= 2;
        int bits = 0, offs = 0;
        for (int i = 0; i < 16; ++i) {
            for (;;) {
                uint32_t b = 0;
                if (!Bits(1, b)) return false;
                if (b) break;
                ++bits;
            }
            if (bits > 24) return false;
            bits_[i] = bits;
            offs_[i] = offs;
            offs += 1 << bits;
        }
        return true;
    }

    const uint8_t* d_;
    size_t n_;
    size_t bit_ = 0;
    int sym_[kSymbols] = {};
    int cnt_[kSymbols] = {};
    int bits_[16] = {};
    int offs_[16] = {};
    std::vector<uint8_t> win_;
    uint32_t wpos_ = 0;
};

// ── 그림 한 장 풀기 — libs/noximage/pcx. 머리 17바이트(폭 · 높이 · 자리 x · y · 1바이트) 뒤에 줄마다 [칸 종류][개수] 묶음
inline uint32_t ReadU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

inline void Put(Image& im, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || x >= im.w) return;
    uint8_t* p = &im.rgba[(static_cast<size_t>(y) * im.w + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

inline bool DecodeImage(const uint8_t* d, size_t n, int type, Image& im)
{
    if (n < 17) return false;
    const int w = static_cast<int>(ReadU32(d)), h = static_cast<int>(ReadU32(d + 4));
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) return false;
    im.w = w;
    im.h = h;
    im.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    size_t i = 17;
    if (type == 2 || type == 7) {   // 줄 묶음 없이 RGBA5551
        if (n - i < static_cast<size_t>(w) * h * 2) return false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x, i += 2) {
                const uint16_t c = static_cast<uint16_t>(d[i] | (d[i + 1] << 8));
                auto f = [](int v) { const uint8_t b = static_cast<uint8_t>(v & 0xF8); return b == 0xF8 ? static_cast<uint8_t>(0xFF) : b; };
                Put(im, x, y, f(c >> 7), f(c >> 2), f(c << 3), 0xFF);
            }
        }
        return true;
    }
    if (type != 3 && type != 4 && type != 5 && type != 6 && type != 8) return false;
    for (int y = 0; y < h; ++y) {
        int x = 0;
        while (x < w) {
            if (n - i < 2) return false;
            const uint8_t op = d[i], dx = d[i + 1];
            i += 2;
            switch (op & 0xF) {
                case 1:   // 빈칸
                case 6:
                    break;
                case 2:
                case 7:   // 16비트 색(종류 8 = RGB565 · 나머지 RGB555)
                    if (n - i < static_cast<size_t>(dx) * 2) return false;
                    for (int k = 0; k < dx; ++k, i += 2) {
                        const uint16_t c = static_cast<uint16_t>(d[i] | (d[i + 1] << 8));
                        if (type == 8) {
                            Put(im, x + k, y, static_cast<uint8_t>((c >> 8) & 0xF8), static_cast<uint8_t>((c >> 3) & 0xFC), static_cast<uint8_t>((c << 3) & 0xF8), 0xFF);
                        } else {
                            Put(im, x + k, y, static_cast<uint8_t>((c >> 7) & 0xF8), static_cast<uint8_t>((c >> 2) & 0xF8), static_cast<uint8_t>((c << 3) & 0xF8), 0xFF);
                        }
                    }
                    break;
                case 4:   // 재질 칸(회색으로 — 메뉴 그림엔 거의 없음)
                    if (type == 8 || n - i < dx) return false;
                    for (int k = 0; k < dx; ++k, ++i) Put(im, x + k, y, d[i], d[i], d[i], 0xFF);
                    break;
                case 5:   // RGBA4444
                    if (type == 8 || n - i < static_cast<size_t>(dx) * 2) return false;
                    for (int k = 0; k < dx; ++k, i += 2) {
                        const uint16_t c = static_cast<uint16_t>(d[i] | (d[i + 1] << 8));
                        Put(im, x + k, y, static_cast<uint8_t>((c >> 8) & 0xF0), static_cast<uint8_t>((c >> 4) & 0xF0), static_cast<uint8_t>(c & 0xF0), static_cast<uint8_t>((c << 4) & 0xF0));
                    }
                    break;
                default:
                    return false;
            }
            x += dx;
        }
    }
    return true;
}

// ── PNG 쓰기(압축 없는 deflate 블록 — 어차피 내 PC 캐시)
inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

inline void PutBE(std::vector<uint8_t>& o, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s));
}

inline void Chunk(std::vector<uint8_t>& o, const char* tag, const std::vector<uint8_t>& body)
{
    PutBE(o, static_cast<uint32_t>(body.size()));
    const size_t start = o.size();
    o.insert(o.end(), tag, tag + 4);
    o.insert(o.end(), body.begin(), body.end());
    PutBE(o, ~Crc32(&o[start], o.size() - start));
}

inline std::vector<uint8_t> EncodePng(const Image& im)
{
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(im.w) * 4 + 1) * im.h);
    for (int y = 0; y < im.h; ++y) {
        raw.push_back(0);
        const uint8_t* row = &im.rgba[static_cast<size_t>(y) * im.w * 4];
        raw.insert(raw.end(), row, row + static_cast<size_t>(im.w) * 4);
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t i = 0; i < raw.size() || i == 0;) {
        const size_t len = (std::min<size_t>)(raw.size() - i, 65535);
        z.push_back(i + len >= raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + len);
        i += len;
        if (len == 0) break;
    }
    PutBE(z, (b << 16) | a);
    std::vector<uint8_t> o = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> hdr;
    PutBE(hdr, static_cast<uint32_t>(im.w));
    PutBE(hdr, static_cast<uint32_t>(im.h));
    hdr.insert(hdr.end(), { 8, 6, 0, 0, 0 });
    Chunk(o, "IHDR", hdr);
    Chunk(o, "IDAT", z);
    Chunk(o, "IEND", {});
    return o;
}

// ── 파일
inline bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart < (1 << 30);
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, NULL) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

inline bool WriteAll(const std::wstring& path, const uint8_t* p, size_t n)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = n == 0 || (WriteFile(h, p, static_cast<DWORD>(n), &wrote, NULL) && wrote == n);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline long long FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

// ── video.bag
class Bag {
public:
    ~Bag()
    {
        if (bag_ != INVALID_HANDLE_VALUE) CloseHandle(bag_);
    }

    bool Open(const std::wstring& gameDir)
    {
        std::vector<uint8_t> idx;
        if (!ReadAll(gameDir + L"\\video.idx", idx) || idx.size() < 24) return false;
        const uint32_t magic = ReadU32(idx.data());
        if (magic != 0xFAEDBCEBu && magic != 0xFAEDBCEAu) return false;
        const bool comp = magic == 0xFAEDBCEBu;
        const uint32_t segs = ReadU32(idx.data() + 8);
        size_t p = 24;
        uint32_t offs = 0;
        auto need = [&](size_t k) { return idx.size() - p >= k; };
        for (uint32_t s = 0; s < segs; ++s) {
            if (!need(comp ? 16 : 12)) return false;
            p += 4;
            Segment seg;
            seg.size = ReadU32(&idx[p]);
            p += 4;
            seg.sizeComp = seg.size;
            if (comp) {
                seg.sizeComp = ReadU32(&idx[p]);
                p += 4;
            }
            int32_t cnt = static_cast<int32_t>(ReadU32(&idx[p]));
            p += 4;
            seg.bagOffset = offs;
            offs += seg.sizeComp;
            segs_.push_back(seg);
            if (cnt == -1) cnt = 1;
            uint32_t soff = 0;
            for (int32_t j = 0; j < cnt; ++j) {
                if (!need(1)) return false;
                const size_t nameLen = idx[p++];
                if (!need(nameLen + 9)) return false;
                p += nameLen;
                Rec r;
                r.type = idx[p++];
                r.seg = s;
                r.offset = soff;
                r.size = ReadU32(&idx[p]);   // 16비트 판 길이
                p += 8;                      // 뒤 4바이트는 8비트 판(video8.bag) 길이
                soff += r.size;
                recs_.push_back(r);
            }
        }
        bag_ = CreateFileW((gameDir + L"\\video.bag").c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        return bag_ != INVALID_HANDLE_VALUE;
    }

    size_t ImageCount() const { return recs_.size(); }

    bool Load(size_t index, Image& im)
    {
        if (index >= recs_.size()) return false;
        const Rec& r = recs_[index];
        if (!LoadSegment(r.seg)) return false;
        if (r.offset > data_.size() || r.size > data_.size() - r.offset) return false;
        return DecodeImage(data_.data() + r.offset, r.size, r.type, im);
    }

private:
    struct Segment { uint32_t bagOffset = 0, size = 0, sizeComp = 0; };
    struct Rec { uint32_t seg = 0, offset = 0, size = 0; int type = 0; };

    bool LoadSegment(uint32_t s)
    {
        if (s == cached_) return true;
        const Segment& seg = segs_[s];
        std::vector<uint8_t> src(seg.sizeComp);
        LARGE_INTEGER at;
        at.QuadPart = seg.bagOffset;
        DWORD got = 0;
        if (!SetFilePointerEx(bag_, at, NULL, FILE_BEGIN)) return false;
        if (!src.empty() && (!ReadFile(bag_, src.data(), seg.sizeComp, &got, NULL) || got != seg.sizeComp)) return false;
        if (seg.size == seg.sizeComp) {
            data_.swap(src);
        } else {
            auto rd = std::make_unique<NxzReader>(src.data(), src.size());
            if (!rd->Decode(data_, seg.size)) return false;
        }
        cached_ = s;
        return true;
    }

    std::vector<Segment> segs_;
    std::vector<Rec> recs_;
    HANDLE bag_ = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> data_;
    uint32_t cached_ = 0xFFFFFFFFu;
};

// ── 런처가 쓰는 그림(GOG 판 video.bag 번호 · thing.bin IMAG 로 확인한 값)
struct ArtEntry { const wchar_t* file; size_t index; int w, h; };
inline const ArtEntry* ArtTable(size_t& count)
{
    static const ArtEntry kArt[] = {
        { L"WolapiLoginBG", 14602, 640, 480 },  { L"CopyrightScreen", 14597, 640, 480 }, { L"UICheckBox", 14740, 12, 12 },
        { L"UICheckBoxLit", 14741, 12, 12 },    { L"UIButtonHuge", 14742, 198, 40 },     { L"UIButtonHugeLit", 14743, 198, 40 },
        { L"UIButtonHugeDis", 14746, 198, 40 }, { L"BorderCornerUL", 15070, 13, 13 },    { L"BorderCornerUR", 15071, 13, 13 },
        { L"BorderCornerLR", 15072, 13, 13 },   { L"BorderCornerLL", 15073, 13, 13 },    { L"BorderHorizontal", 15074, 20, 6 },
        { L"BorderVertical", 15076, 6, 20 },
    };
    count = sizeof(kArt) / sizeof(kArt[0]);
    return kArt;
}

constexpr const char* kArtVersion = "1";

inline std::string StampFor(const std::wstring& gameDir)
{
    return std::string("v") + kArtVersion + " " + std::to_string(FileSize(gameDir + L"\\video.bag")) + " " + std::to_string(FileSize(gameDir + L"\\video.idx")) + " " +
           std::to_string(FileSize(gameDir + L"\\default.fnt"));
}

inline bool EnsureDir(const std::wstring& dir)
{
    if (CreateDirectoryW(dir.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos || !EnsureDir(dir.substr(0, cut))) return false;
    return CreateDirectoryW(dir.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// 게임 폴더 → 그림 폴더. 이미 같은 원본에서 뽑았으면 아무것도 안 한다
inline bool Extract(const std::wstring& gameDir, const std::wstring& artDir)
{
    if (FileSize(gameDir + L"\\video.bag") < 0 || FileSize(gameDir + L"\\video.idx") < 0 || FileSize(gameDir + L"\\default.fnt") < 0) return false;
    const std::string stamp = StampFor(gameDir);
    const std::wstring stampPath = artDir + L"\\art.stamp";
    std::vector<uint8_t> old;
    if (ReadAll(stampPath, old) && std::string(old.begin(), old.end()) == stamp) return true;
    if (!EnsureDir(artDir)) return false;
    Bag bag;
    if (!bag.Open(gameDir)) return false;
    size_t count = 0;
    const ArtEntry* table = ArtTable(count);
    for (size_t i = 0; i < count; ++i) {
        Image im;
        if (!bag.Load(table[i].index, im) || im.w != table[i].w || im.h != table[i].h) return false;
        const std::vector<uint8_t> png = EncodePng(im);
        if (!WriteAll(artDir + L"\\" + table[i].file + L".png", png.data(), png.size())) return false;
    }
    std::vector<uint8_t> fnt;
    if (!ReadAll(gameDir + L"\\default.fnt", fnt) || !WriteAll(artDir + L"\\default.fnt", fnt.data(), fnt.size())) return false;
    return WriteAll(stampPath, reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size());
}

}  // namespace noxart
