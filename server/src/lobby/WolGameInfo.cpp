// WolGameInfo.cpp — 방 정보 덩어리 만들기·풀기. 칸 배치와 포장 규칙은 WolGameInfo.h 머리 주석.
#include "nox/WolGameInfo.h"

#include <algorithm>
#include <cstring>

namespace nox::wol {

namespace {

void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// 고정 칸에 글자를 싣는다 — 넘치면 자르고 모자라면 0으로 채운다(gameinfo.go:383 copy 와 같다).
void PutText(uint8_t* p, size_t cap, const std::string& s) {
    std::memset(p, 0, cap);
    std::memcpy(p, s.data(), std::min(cap, s.size()));
}

// 고정 칸의 글자 — 첫 0 앞까지(gameinfo.go:464 IndexByte).
std::string GetText(const uint8_t* p, size_t cap) {
    size_t n = 0;
    while (n < cap && p[n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

}  // namespace

Raw Marshal(const GameInfo& g) {
    Raw r{};
    r[0] = static_cast<uint8_t>(((g.access << 4) & 0xF0) | (g.disallow & 0x0F));
    r[1] = g.unk1;
    r[2] = static_cast<uint8_t>((g.limitRes ? 0x80 : 0) | (g.resolution & 0x0F));
    r[3] = g.players;
    r[4] = g.maxPlayers;
    PutU16(&r[5], g.minPing);
    PutU16(&r[7], g.maxPing);
    PutU16(&r[9], g.unk2);
    PutText(&r[11], 9, g.map);
    PutText(&r[20], 15, g.name);
    std::copy(g.unk3.begin(), g.unk3.end(), r.begin() + 35);
    PutU16(&r[63], g.flags);
    PutU16(&r[65], g.fragLimit);
    PutU16(&r[67], g.timeLimit);
    return r;
}

GameInfo Unmarshal(const Raw& raw) {
    GameInfo g;
    g.access     = static_cast<uint8_t>((raw[0] & 0xF0) >> 4);
    g.disallow   = static_cast<uint8_t>(raw[0] & 0x0F);
    g.unk1       = raw[1];
    g.limitRes   = (raw[2] & 0x80) != 0;
    g.resolution = static_cast<uint8_t>(raw[2] & 0x0F);
    g.players    = raw[3];
    g.maxPlayers = raw[4];
    g.minPing    = GetU16(&raw[5]);
    g.maxPing    = GetU16(&raw[7]);
    g.unk2       = GetU16(&raw[9]);
    g.map        = GetText(&raw[11], 9);
    g.name       = GetText(&raw[20], 15);
    std::copy(raw.begin() + 35, raw.begin() + 63, g.unk3.begin());
    g.flags      = GetU16(&raw[63]);
    g.fragLimit  = GetU16(&raw[65]);
    g.timeLimit  = GetU16(&raw[67]);
    return g;
}

// 69바이트를 낮은 비트부터 이어 붙여 7비트씩 끊고, 끊은 조각마다 최상위 비트를 세운다(crypt.go encryptTo).
//   ⚠ 552비트 중 7로 나눠떨어지는 546비트(78조각)만 싣고 남는 6비트는 버린다 — opennox 가 그렇고,
//     녹음 덩어리 여섯도 모두 78바이트다. 버려지는 건 시간 제한 칸의 상위 6비트다.
std::string PackRaw(const Raw& raw) {
    std::string out(kHeader);
    uint32_t acc = 0;  // 모아 둔 비트
    int n = 0;         // 그 개수
    for (uint8_t b : raw) {
        acc |= static_cast<uint32_t>(b) << n;
        n += 8;
        while (n >= 7) {
            out.push_back(static_cast<char>(0x80 | (acc & 0x7f)));
            acc >>= 7;
            n -= 7;
        }
    }
    return out;
}

// PackRaw 의 역(crypt.go decrypt). 78조각 = 546비트 = 68바이트 + 2비트라 마지막 바이트는 아래 2비트만 온다.
bool UnpackRaw(std::string_view payload, Raw& raw) {
    if (payload.size() != kHeader.size() + kPackedSize) return false;
    if (payload.substr(0, kHeader.size()) != kHeader) return false;
    raw.fill(0);
    uint32_t acc = 0;
    int n = 0;
    size_t w = 0;
    for (size_t i = kHeader.size(); i < payload.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(payload[i]);
        if ((c & 0x80) == 0) return false;   // 포장 조각은 늘 최상위 비트가 1이다
        acc |= static_cast<uint32_t>(c & 0x7f) << n;
        n += 7;
        while (n >= 8 && w < kRawSize) {
            raw[w++] = static_cast<uint8_t>(acc);
            acc >>= 8;
            n -= 8;
        }
    }
    if (w < kRawSize) raw[w] = static_cast<uint8_t>(acc);
    return true;
}

std::string Pack(const GameInfo& g) {
    return PackRaw(Marshal(g));
}

bool Unpack(std::string_view payload, GameInfo& out) {
    Raw raw;
    if (!UnpackRaw(payload, raw)) return false;
    out = Unmarshal(raw);
    return true;
}

// ★ 10-14 작업 8d — 옛 `lobby_main.cpp` `ParseMode` 의 표 그대로(우리 서버의 `coopteam` 은 WOL 표에 없다 — 모르는 이름)
bool ModeFlagsOf(std::string_view mode, uint16_t& bits) {
    static const struct { const char* name; uint16_t bits; } kModes[] = {
        {"kotr", kMapKotr},   {"ctf", kMapCtf},     {"flagball", kMapFlagBall}, {"chat", kMapChat},
        {"arena", kMapArena}, {"elimination", kMapElimination}, {"coop", kMapCoop}, {"quest", kMapQuest},
    };
    for (const auto& m : kModes) {
        if (mode == m.name) {
            bits = m.bits;
            return true;
        }
    }
    return false;
}

}  // namespace nox::wol
