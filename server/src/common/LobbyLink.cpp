// LobbyLink.cpp — 로비 ↔ 게임 서버 연결 규약. 설명은 `LobbyLink.h` 머리.
#include "nox/LobbyLink.h"

#include <cstring>
#include <utility>
#include <vector>

namespace nox {

namespace {

const char* const kWords[] = {"HELLO", "AUTH", "REGISTER", "ROOM", "ROOM_GONE", "CREATE", "CREATED", "CREATE_FAILED", "RESULT"};
static_assert(sizeof(kWords) / sizeof(kWords[0]) == static_cast<size_t>(LinkKind::kCount), "낱말 표 = 갈래 수");

constexpr uint32_t kMaxRoomId = 0xffff;   // 입장 토큰의 방 칸이 16비트(`JoinToken.h`)

// 빈칸 하나마다 자른다 — 빈 칸도 남긴다(빈칸 두 개 · 꼬리 빈칸은 칸 수가 달라져 걸린다)
std::vector<std::string_view> Split(std::string_view s) {
    std::vector<std::string_view> t;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find(' ', p);
        if (q == std::string_view::npos) {
            t.push_back(s.substr(p));
            return t;
        }
        t.push_back(s.substr(p, q - p));
        p = q + 1;
    }
}

// 십진 수(부호 · 앞 0 여럿 · 빈 칸 없음) · `max` 이하
bool ParseNum(std::string_view s, uint32_t max, uint32_t& out) {
    if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
    uint64_t v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<uint64_t>(ch - '0');
    }
    if (v > max) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool IsHexText(std::string_view s, size_t len) {
    if (s.size() != len) return false;
    for (const char ch : s)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    return true;
}

int Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool MustEscape(unsigned char c) { return c <= 0x20 || c == 0x7f || c == '%' || c == '~'; }

JoinKey Derive(const JoinKey& tokenKey, const std::string& label) {
    JoinKey k{};
    if (!HmacSha256(tokenKey.data(), tokenKey.size(), reinterpret_cast<const uint8_t*>(label.data()), label.size(), k.data()))
        k.fill(0);   // BCrypt 실패 — 0 키는 상대와 맞지 않아 확인 · 토큰이 실패로 드러난다
    return k;
}

}  // namespace

const char* LinkKindWord(LinkKind k) {
    const size_t i = static_cast<size_t>(k);
    return i < static_cast<size_t>(LinkKind::kCount) ? kWords[i] : "?";
}

const char* LinkParseName(LinkParse p) {
    switch (p) {
        case LinkParse::Ok: return "통과";
        case LinkParse::Empty: return "빈 줄";
        case LinkParse::UnknownWord: return "모르는 낱말";
        case LinkParse::FieldCount: return "칸 수";
        case LinkParse::BadNumber: return "수";
        case LinkParse::BadText: return "글 칸";
        case LinkParse::BadHex: return "16진";
        case LinkParse::BadAddress: return "주소";
        case LinkParse::kCount: break;
    }
    return "?";
}

std::string EncodeLinkText(const std::string& s) {
    if (s.empty()) return "~";
    static const char kDigits[] = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size());
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (MustEscape(c)) {
            o += '%';
            o += kDigits[c >> 4];
            o += kDigits[c & 0x0f];
        } else {
            o += ch;
        }
    }
    return o;
}

bool DecodeLinkText(std::string_view s, std::string& out) {
    out.clear();
    if (s == "~") return true;
    if (s.empty()) return false;   // 빈 글은 `~` 로만 온다
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '%') {
            if (i + 2 >= s.size()) return false;   // `%` 뒤 두 글자가 모자람
            const int hi = Nibble(s[i + 1]), lo = Nibble(s[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
            continue;
        }
        if (MustEscape(c)) return false;   // 날 `~` · 제어 바이트(빈칸은 이미 칸 나누기에서 갈렸다)
        out += static_cast<char>(c);
    }
    return true;
}

std::string LinkHello(const std::string& nonceHex) { return std::string(kWords[0]) + " " + nonceHex; }
std::string LinkAuth(const std::string& sigHex) { return std::string(kWords[1]) + " " + sigHex; }

std::string LinkRegister(uint32_t epoch, uint32_t ip, uint16_t port) {
    return std::string(kWords[2]) + " " + std::to_string(epoch) + " " + Ipv4ToString(ip) + " " + std::to_string(port);
}

std::string LinkRoomLine(const LinkRoom& r) {
    return std::string(kWords[3]) + " " + std::to_string(r.id) + " " + std::to_string(r.players) + " " +
           std::to_string(r.maxPlayers) + " " + (r.playing ? "1" : "0") + " " + EncodeLinkText(r.name) + " " +
           EncodeLinkText(r.creator) + " " + EncodeLinkText(r.host) + " " + EncodeLinkText(r.map) + " " + EncodeLinkText(r.mode);
}

std::string LinkRoomGone(uint32_t id) { return std::string(kWords[4]) + " " + std::to_string(id); }

std::string LinkCreate(uint32_t req, const std::string& creator) {
    return std::string(kWords[5]) + " " + std::to_string(req) + " " + EncodeLinkText(creator);
}

std::string LinkCreated(uint32_t req, uint32_t id) {
    return std::string(kWords[6]) + " " + std::to_string(req) + " " + std::to_string(id);
}

std::string LinkCreateFailed(uint32_t req, const std::string& reason) {
    return std::string(kWords[7]) + " " + std::to_string(req) + " " + EncodeLinkText(reason);
}

bool LinkResult(const std::string& json, std::string& line) {
    if (json.empty() || json.find_first_of("\r\n") != std::string::npos) return false;
    std::string l = std::string(kWords[8]) + " " + json;
    if (!LinkLineFits(l)) return false;
    line = std::move(l);
    return true;
}

LinkParse ParseLinkLine(std::string_view line, LinkLine& out) {
    out = LinkLine{};
    if (line.empty()) return LinkParse::Empty;
    // RESULT 는 꼬리 전체가 JSON(빈칸이 든다) — 칸 나누기 전에 뗀다
    const size_t sp = line.find(' ');
    const std::string_view word = line.substr(0, sp);
    size_t k = 0;
    while (k < static_cast<size_t>(LinkKind::kCount) && word != kWords[k]) ++k;
    if (k == static_cast<size_t>(LinkKind::kCount)) return LinkParse::UnknownWord;
    out.kind = static_cast<LinkKind>(k);
    if (out.kind == LinkKind::Result) {
        if (sp == std::string_view::npos || sp + 1 >= line.size()) return LinkParse::FieldCount;
        out.text.assign(line.substr(sp + 1));
        return LinkParse::Ok;
    }
    const auto t = Split(line);
    auto want = [&](size_t n) { return t.size() == n; };
    switch (out.kind) {
        case LinkKind::Hello:
        case LinkKind::Auth: {
            if (!want(2)) return LinkParse::FieldCount;
            const size_t len = out.kind == LinkKind::Hello ? kLinkNonceBytes * 2 : 64;
            if (!IsHexText(t[1], len)) return LinkParse::BadHex;
            out.text.assign(t[1]);
            return LinkParse::Ok;
        }
        case LinkKind::Register: {
            if (!want(4)) return LinkParse::FieldCount;
            uint32_t port = 0;
            if (!ParseNum(t[1], 0xffffffffu, out.epoch) || !ParseNum(t[3], 0xffff, port) || port == 0) return LinkParse::BadNumber;
            if (!ParseIpv4(std::string(t[2]), out.ip)) return LinkParse::BadAddress;
            out.port = static_cast<uint16_t>(port);
            return LinkParse::Ok;
        }
        case LinkKind::Room: {
            if (!want(10)) return LinkParse::FieldCount;
            uint32_t playing = 0;
            LinkRoom& r = out.room;
            if (!ParseNum(t[1], kMaxRoomId, r.id) || !ParseNum(t[2], 0xffffffffu, r.players) ||
                !ParseNum(t[3], 0xffffffffu, r.maxPlayers) || !ParseNum(t[4], 1, playing))
                return LinkParse::BadNumber;
            r.playing = playing != 0;
            if (!DecodeLinkText(t[5], r.name) || !DecodeLinkText(t[6], r.creator) || !DecodeLinkText(t[7], r.host) ||
                !DecodeLinkText(t[8], r.map) || !DecodeLinkText(t[9], r.mode))
                return LinkParse::BadText;
            return LinkParse::Ok;
        }
        case LinkKind::RoomGone:
            if (!want(2)) return LinkParse::FieldCount;
            return ParseNum(t[1], kMaxRoomId, out.room.id) ? LinkParse::Ok : LinkParse::BadNumber;
        case LinkKind::Create:
        case LinkKind::CreateFailed:
            if (!want(3)) return LinkParse::FieldCount;
            if (!ParseNum(t[1], 0xffffffffu, out.req)) return LinkParse::BadNumber;
            return DecodeLinkText(t[2], out.text) ? LinkParse::Ok : LinkParse::BadText;
        case LinkKind::Created:
            if (!want(3)) return LinkParse::FieldCount;
            return ParseNum(t[1], 0xffffffffu, out.req) && ParseNum(t[2], kMaxRoomId, out.room.id) ? LinkParse::Ok
                                                                                                    : LinkParse::BadNumber;
        case LinkKind::Result:
        case LinkKind::kCount:
            break;
    }
    return LinkParse::UnknownWord;
}

JoinKey LinkAuthKey(const JoinKey& tokenKey) { return Derive(tokenKey, "nox-link-auth-v1"); }

JoinKey GameServerTokenKey(const JoinKey& tokenKey, uint32_t epoch, uint32_t ip, uint16_t port) {
    return Derive(tokenKey, "nox-gs-token-v1|" + std::to_string(epoch) + "|" + Ipv4ToString(ip) + "|" + std::to_string(port));
}

std::string NewLinkNonceHex() {
    uint8_t b[kLinkNonceBytes];
    if (!RandomBytes(b, sizeof(b))) return {};
    return ToHex(b, sizeof(b));
}

std::string LinkAuthSig(const JoinKey& authKey, const std::string& nonceHex) {
    uint8_t mac[32];
    if (!HmacSha256(authKey.data(), authKey.size(), reinterpret_cast<const uint8_t*>(nonceHex.data()), nonceHex.size(), mac))
        return {};
    return ToHex(mac, sizeof(mac));
}

bool CheckLinkAuth(const JoinKey& authKey, const std::string& nonceHex, const std::string& sigHex) {
    uint8_t want[32], got[32];
    const std::string w = LinkAuthSig(authKey, nonceHex);
    if (w.empty() || !ParseHex(w, want, sizeof(want)) || !ParseHex(sigHex, got, sizeof(got))) return false;
    uint8_t diff = 0;   // 같은 시간에 끝나는 비교(`VerifyJoinToken` 과 같은 꼴)
    for (size_t i = 0; i < sizeof(want); ++i) diff |= static_cast<uint8_t>(want[i] ^ got[i]);
    return diff == 0;
}

}  // namespace nox
