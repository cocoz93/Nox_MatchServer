// verify_update.cpp — 상태 갱신을 내 C++ 파서로 뜯어 성공률을 낸다.
//
// 왜 성공률로 재나 — 상태 갱신은 **길이가 안 적혀 있다.** 끝 표식(`00 00 00`)까지
// 항목을 하나씩 따라가야 크기를 알 수 있고, 한 자리라도 어긋나면 표식에 못 닿는다.
// 그래서 「끝까지 닿은 비율」이 곧 「짜임을 제대로 아는가」의 판정이 된다.
//
// ⚠ **「끝 표식에 닿았다」가 곧 검증은 아니다.** 09-05 에 그걸로 속았다 —
//   99.7% 가 나왔는데 좌표라고 읽은 값이 `(1, 64)` 로 2만 7천 번이었다. 자리 수만
//   우연히 맞은 것이다. 그래서 이 도구는 성공률과 함께 **좌표가 몇 가지나 나왔는지**도
//   같이 낸다. 한 값에 몰려 있으면 끼워 맞춘 것이다.
//
//   nox_verify <서버덤프>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nox/Alias.h"
#include "nox/Opcode.h"
#include "nox/ThingFlags.h"
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace {

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct Line {
    bool send = false;
    std::string peer;
    std::vector<uint8_t> data;
    bool plain = false;   // 이미 평문이라 XOR 을 풀면 안 된다
};

// ★★★ **덤프 꼴이 둘이다** (2026-09-11 — 13MB 짜리를 못 읽고 있었다).
//
//   ⑴ 한 줄 꼴  `... SEND <peer> len=N | aa bb cc ...`      ← 암호문, 키로 풀어야 한다
//   ⑵ 두 줄 꼴  `13:10:45 S->C <peer> len=N op=0x0c MSG_...`
//                `  00 00 0c 00 ...`                          ← **평문**, 바이트가 다음 줄
//
//   `CAP_GEN2.txt`(13MB)가 ⑵ 꼴인데 파서가 ⑴만 알아서 **갱신 스트림 0건**이 나왔다.
//   그것을 「정품엔 없나 보다」로 읽을 뻔했다 — **0 은 「없었다」와 「못 읽었다」를 안 가른다.**
//   ⑵ 꼴은 `op=` 이 이미 적혀 있다는 것 자체가 **뜨는 쪽이 이미 풀어 놓았다**는 뜻이다.
struct PendingHdr {
    bool valid = false;
    bool send = false;
    std::string peer;
    size_t want = 0;
};

bool ParseHdr2(const char* s, PendingHdr& out) {
    const char* p = std::strstr(s, " S->C ");
    if (p) out.send = true;
    else {
        p = std::strstr(s, " C->S ");
        if (!p) return false;
        out.send = false;
    }
    p += 6;
    const char* lenAt = std::strstr(p, " len=");
    if (!lenAt) return false;
    out.peer.assign(p, lenAt - p);
    const int want = std::atoi(lenAt + 5);
    if (want <= 0) return false;
    out.want = static_cast<size_t>(want);
    out.valid = true;
    return true;
}

// 16진 바이트만 있는 줄인가 (두 줄 꼴의 둘째 줄)
bool ParseHexOnly(const char* s, size_t want, std::vector<uint8_t>& out) {
    out.clear();
    const char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    while (out.size() < want) {
        while (*p == ' ') ++p;
        const int hi = (*p >= '0' && *p <= '9') ? *p - '0'
                     : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                     : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        if (hi < 0) break;
        const char c2 = p[1];
        const int lo = (c2 >= '0' && c2 <= '9') ? c2 - '0'
                     : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10
                     : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
        if (lo < 0) break;
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p += 2;
    }
    return out.size() == want;
}

bool ParseLine(const char* s, Line& out) {
    const char* p = std::strstr(s, " RECV ");
    if (p) out.send = false;
    else {
        p = std::strstr(s, " SEND ");
        if (!p) return false;
        out.send = true;
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
    while (out.data.size() < static_cast<size_t>(want)) {
        while (*p == ' ') ++p;
        const int hi = HexVal(p[0]);
        if (hi < 0) break;
        const int lo = HexVal(p[1]);
        if (lo < 0) break;
        out.data.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p += 2;
    }
    return out.data.size() == static_cast<size_t>(want);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    std::system("");
#endif
    if (argc < 2) {
        std::printf("쓰는 법: nox_verify <서버덤프>\n");
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("덤프를 못 연다: %s\n", argv[1]); return 1; }

    std::map<std::string, uint8_t> keys;
    nox::AliasTable aliases;
    uint32_t frame = 0;

    unsigned long long total = 0, good = 0, objs = 0, roundTrip = 0;
    int badShown = 0;
    std::map<uint16_t, unsigned long long> byType;
    std::map<int, unsigned long long> extraHist;
    int playerExtraShown = 0;   // 사람 덧붙이를 몇 건 찍었나 (너무 많이 안 찍게)
    std::set<std::pair<int, int>> coords;   // 좌표가 몇 가지나 나오나 — 끼워맞춤 감별용
    int aliasMax = 0, netMin = -1, netMax = -1;
    unsigned long long overrun = 0;   // 길이가 남은 바이트를 넘어 멈춘 자리
    unsigned long long budgetOut = 0; // 갈래 예산이 바닥나 그만둔 조각

    // 걸음 속도 실측용 — 오브젝트별 마지막 자리와 그때 프레임
    struct LastPos { int x, y; uint32_t frame; };
    std::map<uint16_t, LastPos> lastPos;
    std::map<int, unsigned long long> stepHist;
    double stepSum = 0, stepMax = 0;
    unsigned long long stepCnt = 0;
    // ★ 바로 다음 틱(프레임 차 1)에서만 잰 것 — 이것이 진짜 걸음 속도다.
    //   사이가 벌어진 표본은 「가만히 있다가 움직인 것」이 섞여 값을 낮춘다.
    std::map<int, unsigned long long> step1Hist;
    std::vector<double> step1All;
    // dx·dy 를 그대로 센다 — 거리만 보면 축 이동과 대각선이 섞여 봉우리가 갈린다.
    std::map<std::pair<int, int>, unsigned long long> step1DxDy;

    char buf[65536];
    Line ln;
    PendingHdr hdr;
    while (std::fgets(buf, sizeof(buf), f)) {
        if (!ParseLine(buf, ln)) {
            // ⑵ 두 줄 꼴 — 머리를 잡아 두었다가 다음 줄의 16진을 붙인다.
            PendingHdr h;
            if (ParseHdr2(buf, h)) { hdr = h; continue; }
            if (hdr.valid) {
                std::vector<uint8_t> bytes;
                const bool okHex = ParseHexOnly(buf, hdr.want, bytes);
                const bool wasSend = hdr.send;
                const std::string peer = hdr.peer;
                hdr.valid = false;              // 머리는 한 번만 쓴다
                if (!okHex) continue;
                ln.send = wasSend;
                ln.peer = peer;
                ln.data.swap(bytes);
                ln.plain = true;                // 이 꼴은 이미 평문이다
            } else {
                continue;
            }
        } else {
            ln.plain = false;
        }
        if (ln.data.size() < 3) continue;

        std::vector<uint8_t> d = ln.data;
        // SEND 는 암호화 이후라 풀어야 한다. RECV 는 복호화 직후라 이미 평문이다.
        if (ln.send && !ln.plain) {
            auto k = keys.find(ln.peer);
            if (k != keys.end() && k->second)
                nox::ApplyXor(d.data(), d.size(), k->second);
        }

        const uint8_t* p = d.data() + nox::kWireHeaderSize;
        size_t left = d.size() - nox::kWireHeaderSize;
        while (left > 0) {
            const uint8_t op = p[0];

            if (op == static_cast<uint8_t>(nox::Op::UPDATE_STREAM)) {
                ++total;
                nox::UpdateMsg m = nox::DecodeUpdate(p + 1, left - 1, &aliases);
                if (!m.ok) {
                    if (m.budgetOut) ++budgetOut;   // 갈래가 너무 많아 그만둔 것
                    break;                          // 못 뜯으면 이 조각은 여기서 끝
                }
                ++good;
                objs += m.objects.size();

                // ★ 왕복 — 뜯은 것을 다시 짜서 원본과 견준다.
                //   여기까지 같아야 「짜임을 다 안다」고 할 수 있다.
                //   뜯기만 되는 것은 「읽을 줄 안다」까지고, 서버는 **만들어 보내야** 한다.
                {
                    std::vector<uint8_t> re(m.consumed + 16);
                    const size_t rn = nox::EncodeUpdate(m, re.data(), re.size());
                    if (rn == m.consumed && std::memcmp(re.data(), p + 1, rn) == 0) {
                        ++roundTrip;
                    } else if (badShown < 3) {
                        ++badShown;
                        std::printf("[왕복 어긋남] 원본 %zu바이트 / 다시 짠 것 %zu바이트\n",
                                    m.consumed, rn);
                    }
                }

                for (const auto& e : m.objects) {
                    if (e.alias == 0xff && e.netCode != 0) {
                        // 처음 보는 오브젝트는 표에 올린다. 클라가 하는 그대로다.
                        const int a = aliases.Assign(e.netCode, e.type, frame);
                        (void)a;
                        if (netMin < 0 || e.netCode < netMin) netMin = e.netCode;
                        if (e.netCode > netMax) netMax = e.netCode;
                    } else if (e.alias > aliasMax) {
                        aliasMax = e.alias;
                    }
                    if (e.type) ++byType[e.type];
                    ++extraHist[e.extra];
                    // ★★ **사람 항목의 덧붙이 바이트를 그대로 찍는다** (2026-09-11).
                    //   「몇 바이트냐」는 알아냈는데 **그 값이 무슨 뜻이냐**를 모른다.
                    //   우리는 둘째 바이트에 자세를 싣고 있지만 화면이 안 바뀐다.
                    //   정품이 거기 무엇을 넣는지 보면 끝난다. 표본이 적으니 전부 찍는다.
                    if (e.type == 713 && e.extra > 0 && playerExtraShown < 40) {
                        ++playerExtraShown;
                        std::printf("  [사람 덧붙이] 별칭 0x%02x · %d바이트 ·",
                                    e.alias, e.extra);
                        for (int q = 0; q < e.extra && q < 3; ++q)
                            std::printf(" %02x", e.extraBytes[q]);
                        std::printf("   (자리 %d,%d)\n", e.x, e.y);
                    }
                    if (coords.size() < 200000) coords.insert({e.x, e.y});

                    // ★ 걸음 속도 실측 — 플레이어가 한 틱에 얼마나 움직이나.
                    //   내 서버는 이 값을 지어내고 있다. 실측으로 바꿔야 한다.
                    if (e.type == 713 && e.netCode) {
                        auto pv = lastPos.find(e.netCode);
                        if (pv != lastPos.end() && pv->second.frame != frame &&
                            frame > pv->second.frame) {
                            const int dx = e.x - pv->second.x;
                            const int dy = e.y - pv->second.y;
                            const uint32_t df = frame - pv->second.frame;
                            if (dx || dy) {
                                const double d =
                                    std::sqrt(static_cast<double>(dx * dx + dy * dy)) / df;
                                ++stepHist[static_cast<int>(d + 0.5)];
                                stepSum += d;
                                ++stepCnt;
                                if (d > stepMax) stepMax = d;
                                if (df == 1) {
                                    ++step1Hist[static_cast<int>(d + 0.5)];
                                    step1All.push_back(d);
                                    ++step1DxDy[{dx, dy}];
                                }
                            }
                        }
                        lastPos[e.netCode] = {e.x, e.y, frame};
                    }
                }
                // ⚠ left 는 부호가 없다. 남은 것보다 크게 빼면 거대한 값이 되어
                //    while(left>0) 이 안 끝나고 버퍼 밖을 읽는다. 먼저 막는다.
                if (1 + m.consumed > left) { ++overrun; break; }
                p += 1 + m.consumed;
                left -= 1 + m.consumed;
                continue;
            }

            // 별칭 등록 — 서버가 직접 알려주는 자리
            if (op == static_cast<uint8_t>(nox::Op::NEW_ALIAS) && left >= 10) {
                const uint8_t alias = p[1];
                const uint16_t nc = static_cast<uint16_t>(p[2] | (p[3] << 8));
                const uint16_t ty = static_cast<uint16_t>(p[4] | (p[5] << 8));
                aliases.Set(alias, nc, ty, frame + nox::kAliasTTL);
            }
            // 시각 — 별칭 임대의 기준이 된다
            if (op == static_cast<uint8_t>(nox::Op::TIMESTAMP) && left >= 3)
                frame = static_cast<uint32_t>(p[1] | (p[2] << 8));
            // 접속 수락 — 이 뒤로 그 상대는 암호문이다
            if (op == static_cast<uint8_t>(nox::Op::SERVER_ACCEPT) && left >= 6 && ln.send)
                keys[ln.peer] = p[5];

            const size_t n = nox::MessageSize(p, left, !ln.send);
            if (n == 0) break;
            // ⚠ 길이표가 남은 것보다 큰 값을 주면 여기서 멈춰야 한다 (위와 같은 이유).
            //    길이를 새로 등록할 때 이 자리가 없으면 조각 하나가 도구를 멈춰 세운다.
            if (n > left) { ++overrun; break; }
            p += n;
            left -= n;
        }
    }
    std::fclose(f);

    std::printf("=== 상태 갱신 판독 ===\n");
    std::printf("  만난 것   %llu\n", total);
    std::printf("  끝까지 읽음 %llu   (%.1f%%)\n", good,
                total ? 100.0 * static_cast<double>(good) / static_cast<double>(total) : 0.0);
    std::printf("  오브젝트  %llu\n", objs);
    std::printf("  다시 짜서 원본과 같음 %llu   (%.1f%%)\n", roundTrip,
                good ? 100.0 * static_cast<double>(roundTrip) / static_cast<double>(good) : 0.0);
    std::printf("  별칭 최대 %d · 오브젝트 번호 %d~%d\n", aliasMax, netMin, netMax);
    std::printf("  좌표 가짓수 %zu   (한 값에 몰려 있으면 끼워 맞춘 것이다)\n", coords.size());
    if (overrun)
        std::printf("  ⚠ 길이가 남은 바이트를 넘어 멈춘 조각 %llu개 — 길이표를 의심할 것\n", overrun);
    std::printf("  못 읽은 %llu개 중 갈래 예산이 바닥난 것 %llu개\n",
                total - good, budgetOut);

    std::printf("\n걸음 속도 — 플레이어가 한 틱에 움직인 거리\n");
    if (stepCnt) {
        std::printf("  표본 %llu · 평균 %.2f · 최대 %.2f\n", stepCnt,
                    stepSum / static_cast<double>(stepCnt), stepMax);
        for (const auto& kv : stepHist) {
            std::printf("    %2d 안팎  %llu\n", kv.first, kv.second);
        }
    }
    // ★ 프레임 차 1 짜리만 — 여기가 판정 자리다.
    //   가만히 있다 움직인 표본이 섞이면 값이 낮게 깎인다.
    if (!step1All.empty()) {
        std::sort(step1All.begin(), step1All.end());
        const double med = step1All[step1All.size() / 2];
        double sum1 = 0;
        for (double v : step1All) sum1 += v;
        std::printf("\n  ★ 바로 다음 틱에서만 (프레임 차 1) — 표본 %zu\n", step1All.size());
        std::printf("    평균 %.2f · 중앙값 %.2f · 최소 %.2f · 최대 %.2f\n",
                    sum1 / static_cast<double>(step1All.size()), med, step1All.front(),
                    step1All.back());
        unsigned long long topN = 0;
        int topAt = 0;
        for (const auto& kv : step1Hist) {
            std::printf("    %2d 안팎  %llu\n", kv.first, kv.second);
            if (kv.second > topN) { topN = kv.second; topAt = kv.first; }
        }
        std::printf("    → 가장 잦은 값 %d (%llu회, %.1f%%)\n", topAt, topN,
                    100.0 * static_cast<double>(topN) / static_cast<double>(step1All.size()));

        // dx·dy 를 잦은 것부터 — 축 이동인지 대각선인지가 여기서 보인다.
        std::vector<std::pair<std::pair<int, int>, unsigned long long>> dd(step1DxDy.begin(),
                                                                          step1DxDy.end());
        std::sort(dd.begin(), dd.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("\n    dx·dy 로 보면 (잦은 것부터, %zu가지)\n", dd.size());
        for (size_t i = 0; i < dd.size() && i < 12; ++i) {
            const int ax = dd[i].first.first, ay = dd[i].first.second;
            const double len = std::sqrt(static_cast<double>(ax * ax + ay * ay));
            const char* kind = (ax == 0 || ay == 0) ? "축" : "대각";
            std::printf("      (%4d,%4d)  %-4s 길이 %6.2f   %llu회\n", ax, ay, kind, len,
                        dd[i].second);
        }
    } else {
        std::printf("  움직인 표본이 없다 — 이 덤프에서는 못 잰다\n");
    }

    std::printf("\n덧붙이 바이트 — 몇 바이트가 붙었나\n");
    for (const auto& kv : extraHist)
        std::printf("  %d바이트  %llu\n", kv.first, kv.second);

    std::printf("\n실린 종류 — 많은 것부터\n");
    std::vector<std::pair<uint16_t, unsigned long long>> v(byType.begin(), byType.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < v.size() && i < 10; ++i)
        std::printf("  종류 %-5u %s  %llu\n", v[i].first,
                    nox::ThingIsComplex(v[i].first) ? "복합" : "단순", v[i].second);
    std::printf("  ... 모두 %zu가지\n", v.size());

    const double pct = total ? 100.0 * static_cast<double>(good) / static_cast<double>(total) : 0.0;
    const bool rtOk = (good == roundTrip);
    if (pct >= 99.9 && rtOk)
        std::printf("\n[통과] 끝까지 읽고, 다시 짜면 원본과 바이트가 같다\n");
    else if (pct >= 99.9)
        std::printf("\n[미달] 읽기는 되는데 다시 짠 것이 원본과 다르다\n");
    else
        std::printf("\n[미달] 아직 못 읽는 자리가 있다\n");
    return (pct >= 99.9 && rtOk) ? 0 : 1;
}
