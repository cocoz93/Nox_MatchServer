// DumpSniff.cpp — replay.cpp 가 갖고 있던 것을 그대로 옮겼다. 동작을 바꾸지 않았다.
#include "nox/DumpSniff.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "nox/Opcode.h"
#include "nox/Wire.h"

namespace nox {
namespace dump {

namespace {

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool ParseLine(const char* s, Line& out) {
    const char* p = std::strstr(s, " RECV ");
    if (p) out.dir = "RECV";
    else {
        p = std::strstr(s, " SEND ");
        if (!p) return false;
        out.dir = "SEND";
    }
    p += 6;

    const char* lenAt = std::strstr(p, " len=");
    if (!lenAt) return false;
    out.peer.assign(p, lenAt - p);

    const int want = std::atoi(lenAt + 5);
    if (want <= 0) return false;

    const char* bar = std::strstr(lenAt, " | ");
    if (!bar) return false;
    p = bar + 3;

    out.data.clear();
    out.data.reserve(static_cast<size_t>(want));
    while (out.data.size() < static_cast<size_t>(want)) {
        while (*p == ' ') ++p;
        const int hi = HexVal(p[0]);
        if (hi < 0) break;
        const int lo = HexVal(p[1]);
        if (lo < 0) break;
        out.data.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p += 2;
    }
    // 길이가 안 맞으면 버린다. 반쯤 읽은 줄로 채점하면 판정이 거짓이 된다.
    return out.data.size() == static_cast<size_t>(want);
}

bool Sniff(const char* path, Setup& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    // 덤프 한 줄이 길다. replay.cpp 와 같은 크기를 쓴다.
    static char buf[65536];
    Line ln;
    std::map<std::string, uint8_t> keys;   // 상대별 XOR 키
    std::map<std::string, size_t> slot;    // 상대 → accepts 의 자리
    while (std::fgets(buf, sizeof(buf), f)) {
        if (!ParseLine(buf, ln)) continue;
        if (ln.dir != "SEND") continue;
        std::vector<uint8_t> d = ln.data;
        if (d.size() < 3) continue;

        // 접속 수락은 평문이다 — 여기서 키를 얻는다.
        // ACCEPTED(2바이트) + SERVER_ACCEPT(6바이트) 가 붙어 10바이트다.
        if (d.size() == 10 &&
            d[2] == static_cast<uint8_t>(Op::ACCEPTED) &&
            d[4] == static_cast<uint8_t>(Op::SERVER_ACCEPT)) {
            Server::AcceptScript a;
            a.connectId = static_cast<uint32_t>(d[5]) |
                          (static_cast<uint32_t>(d[6]) << 8) |
                          (static_cast<uint32_t>(d[7]) << 16) |
                          (static_cast<uint32_t>(d[8]) << 24);
            a.xorKey = d[9];
            // 수락은 재전송된다. 같은 번호가 또 오면 새 접속이 아니다.
            if (out.accepts.empty() || out.accepts.back().connectId != a.connectId) {
                slot[ln.peer] = out.accepts.size();
                out.accepts.push_back(a);
            }
            keys[ln.peer] = a.xorKey;
            continue;
        }

        // 그 밖의 것은 키가 있으면 풀어서 본다.
        auto k = keys.find(ln.peer);
        if (k != keys.end() && k->second)
            ApplyXor(d.data(), d.size(), k->second);

        // 서버 소개 — 머리 2 + 오프코드 1 을 뺀 나머지가 본문
        if (!out.haveInfo && d[2] == static_cast<uint8_t>(Op::SERVER_INFO)) {
            out.infoBody.assign(d.begin() + 3, d.end());
            out.haveInfo = true;
        }

        // 접속 직후 216바이트 묶음 — 첫 메시지가 INFORM 이다
        if (d.size() == 218 && d[2] == static_cast<uint8_t>(Op::INFORM)) {
            if (out.joinBody.empty()) out.joinBody.assign(d.begin() + 2, d.end());
            auto it = slot.find(ln.peer);
            if (it != slot.end()) {
                // 이 접속에서 정품이 쓴 프레임과 오브젝트 번호를 뽑아 둔다.
                const uint8_t* b = d.data() + kWireHeaderSize;
                Server::AcceptScript& a = out.accepts[it->second];
                a.frame = static_cast<uint32_t>(b[kJoinFrameA]) |
                          (static_cast<uint32_t>(b[kJoinFrameA + 1]) << 8) |
                          (static_cast<uint32_t>(b[kJoinFrameA + 2]) << 16) |
                          (static_cast<uint32_t>(b[kJoinFrameA + 3]) << 24);
                a.netCode = static_cast<uint16_t>(
                    b[kJoinNetCodeA] | (b[kJoinNetCodeA + 1] << 8));
            }
        }

        // USE_MAP — 41바이트 본문
        if (out.useMapBody.empty() && d.size() == 43 &&
            d[2] == static_cast<uint8_t>(Op::USE_MAP)) {
            out.useMapBody.assign(d.begin() + 2, d.end());
            continue;
        }

        // ★ 맵 지정 뒤 조각 전부를 순서대로 (mirror 용). 앞의 600개만.
        if (!out.useMapBody.empty() && out.mirror.size() < 600)
            out.mirror.push_back(std::vector<uint8_t>(d.begin() + 2, d.end()));

        // ★ 맵 지정 뒤에 이어지는 INFORM(0xa9)·공지(0xc2). 앞의 4개만 챙긴다.
        //   정품 09-05 순서: 218B INFORM → 43B USE_MAP → 105B INFORM → 142B 공지 → 205B INFORM
        if (!out.useMapBody.empty() && out.postMap.size() < 4 &&
            (d[2] == static_cast<uint8_t>(Op::INFORM) ||
             d[2] == static_cast<uint8_t>(Op::XFER_MSG))) {
            out.postMap.push_back(std::vector<uint8_t>(d.begin() + 2, d.end()));
        }

        // ★★ 첫 「세계 스냅샷」 — 맵 지정 뒤 처음 오는 큰 TIMESTAMP 조각이다.
        //    속에 UPDATE_STREAM 이 통째로 들어 있고 **이것 하나가 없으면 화면이 검다**.
        //    2026-09-07 이분법으로 짚었다: 앞 8개까지는 검정, 9번째(이것)에서 HUD 가 뜬다.
        //    이걸 보낸 뒤부터는 내가 만드는 매 틱 갱신으로 세계가 유지된다(실측).
        if (!out.useMapBody.empty() && out.firstSnapshot.empty() &&
            d[2] == static_cast<uint8_t>(Op::TIMESTAMP) && d.size() > 100) {
            out.firstSnapshot.assign(d.begin() + 2, d.end());
        }
    }
    std::fclose(f);
    return out.haveInfo;
}

}  // namespace dump
}  // namespace nox
