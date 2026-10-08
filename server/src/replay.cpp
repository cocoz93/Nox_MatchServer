// replay.cpp — 대조 장치. 잡아둔 덤프를 내 서버에 먹이고 바이트로 채점한다.
//
// 왜 필요한가 — 지금까지 최종 판정이 **스크린샷 육안**이었다. 관문마다 사람이
// 게임을 켜야 해서 무인 진행이 거기서 끊겼다. 덤프에는 「클라가 보낸 것」과
// 「정품 호스트가 답한 것」이 나란히 있으니, 앞엣것을 내 서버에 넣고
// 나온 것을 뒤엣것과 견주면 **클라 없이 채점**된다.
//
//   덤프의 RECV ──▶ Server::OnPacket ──▶ 내가 만든 조각
//                                            │  바이트 비교
//   덤프의 SEND ───────────────────────────▶ 정품이 만든 조각
//
// 쓰는 법:
//   nox_replay <덤프파일> [--limit N] [--show N] [--verbose]
//
// ⚠ 정답지는 **서버 쪽에서 뜬 덤프**여야 한다. 클라 쪽 덤프는 방향이 뒤집혀 있다.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nox/DumpSniff.h"
#include "nox/Opcode.h"
#include "nox/Server.h"
#include "nox/Wire.h"

namespace {

// 덤프를 읽는 부분은 진짜 서버(main.cpp)와 나눠 쓴다 — nox/DumpSniff.h
using nox::dump::Line;
using nox::dump::ParseLine;
using nox::dump::Setup;
using nox::dump::Sniff;

std::string HexOf(const std::vector<uint8_t>& v, size_t max = 32) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < v.size() && i < max; ++i) {
        std::snprintf(b, sizeof(b), "%02x ", v[i]);
        s += b;
    }
    if (v.size() > max) s += "...";
    else if (!s.empty()) s.pop_back();
    return s;
}


}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 한글이 CP949 로 오독되지 않게.
    std::system("");
#endif
    const char* path = nullptr;
    long limit = 0;
    int show = 5;
    bool verbose = false;
    int trace = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) limit = std::atol(argv[++i]);
        else if (a == "--show" && i + 1 < argc) show = std::atoi(argv[++i]);
        else if (a == "--verbose") verbose = true;
        else if (a == "--trace" && i + 1 < argc) trace = std::atoi(argv[++i]);
        else if (!path) path = argv[i];
        else { std::printf("모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (!path) {
        std::printf("쓰는 법: nox_replay <덤프파일> [--limit N] [--show N] [--verbose]\n");
        return 2;
    }

    Setup setup;
    if (!Sniff(path, setup)) {
        std::printf("덤프에서 서버 소개(0x0d)를 못 찾았다. 서버 쪽 덤프가 맞는가?\n");
        return 1;
    }
    std::printf("덤프에서 뽑은 조건\n");
    std::printf("  소개 본문 %zu바이트 · 접속 묶음 %zu바이트 · 맵 지정 %zu바이트\n",
                setup.infoBody.size(), setup.joinBody.size(), setup.useMapBody.size());
    for (auto& a : setup.accepts)
        std::printf("  접속 — 번호 %u · 키 0x%02x · 프레임 %u · 오브젝트번호 %u\n",
                    a.connectId, a.xorKey, a.frame, a.netCode);
    std::printf("\n");

    nox::Server srv;
    // 덤프의 RECV 는 이미 평문이다. 이걸 안 켜면 서버가 한 번 더 풀어 망가뜨린다.
    srv.Network().SetInboundPlain(true);
    srv.Network().SetInfoTemplate(setup.infoBody.data(), setup.infoBody.size());
    srv.Network().SetAcceptScript(setup.accepts);
    if (!setup.joinBody.empty())
        srv.SetJoinTemplate(setup.joinBody.data(), setup.joinBody.size());
    if (!setup.useMapBody.empty())
        srv.SetUseMapTemplate(setup.useMapBody.data(), setup.useMapBody.size());

    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("덤프를 못 연다: %s\n", path); return 1; }

    // 내가 만들었지만 아직 덤프와 맞춰보지 않은 것들. 상대별로 줄 세운다.
    std::map<std::string, std::vector<std::vector<uint8_t>>> pending;

    unsigned long long nLines = 0, nRecv = 0, nSend = 0;
    unsigned long long nSame = 0, nDiff = 0, nMissing = 0, nExtra = 0;
    unsigned long long nExplained = 0;   // 인원 자리
    unsigned long long nJoinUnk = 0;     // 접속 묶음의 모르는 두 자리
    std::map<uint8_t, unsigned long long> missingByOp;
    int shown = 0;
    int traced = 0;

    char buf[65536];
    Line ln;
    std::vector<nox::OutPacket> out;
    while (std::fgets(buf, sizeof(buf), f)) {
        if (limit && static_cast<long>(nLines) >= limit) break;
        if (!ParseLine(buf, ln)) continue;
        ++nLines;

        if (ln.dir == "RECV") {
            ++nRecv;
            // ★ 추적 — 못 쪼갠 조각을 어디까지 읽었는지 눈으로 본다.
            //   개별 오프코드를 고쳐도 수치가 안 움직일 때, **앞에서 이미 어긋난 것**을
            //   찾는 유일한 길이다.
            if (traced < trace && ln.data.size() > nox::kWireHeaderSize) {
                nox::MessageIter t(ln.data.data() + nox::kWireHeaderSize,
                                   ln.data.size() - nox::kWireHeaderSize, true);
                std::vector<std::pair<uint8_t, size_t>> got;
                while (t.Next()) got.emplace_back(t.op(), t.bodyLen());
                if (t.stopped()) {
                    ++traced;
                    std::printf("[추적 %d] %s len=%zu  머리 %02x %02x\n", traced,
                                ln.peer.c_str(), ln.data.size(), ln.data[0], ln.data[1]);
                    for (auto& g : got)
                        std::printf("    0x%02x %-30s 본문 %zu\n", g.first,
                                    nox::OpName(static_cast<nox::Op>(g.first)), g.second);
                    std::printf("    ✗ 0x%02x %s 에서 막힘 — 남은 %zu바이트\n",
                                static_cast<uint8_t>(t.stopOp()),
                                nox::OpName(static_cast<nox::Op>(t.stopOp())), t.stopRest());
                    std::printf("      %s\n",
                                HexOf(std::vector<uint8_t>(
                                          ln.data.end() - static_cast<long>(t.stopRest()),
                                          ln.data.end()), 24).c_str());
                }
            }
            out.clear();
            srv.OnPacket(ln.peer, ln.data.data(), ln.data.size(), out);
            for (auto& p : out) pending[p.to].push_back(std::move(p.data));
            continue;
        }

        ++nSend;
        auto& q = pending[ln.peer];
        if (q.empty()) {
            // 정품은 보냈는데 나는 안 만든 것. 아직 안 만든 자리다.
            // ⚠ 키를 준 뒤의 SEND 는 암호문이라, 풀지 않고 [2]를 오프코드로 읽으면
            //    엉뚱한 이름이 나온다. 실제로 `0x27`(타임스탬프)이 키에 따라
            //    `0x57`·`0x0d` 로 흩어져 「깃발 승자 27,619건」 같은 통계가 나왔다.
            ++nMissing;
            if (ln.data.size() >= 3)
                ++missingByOp[static_cast<uint8_t>(ln.data[2] ^ srv.Network().KeyFor(ln.peer))];
            continue;
        }
        std::vector<uint8_t> mine = std::move(q.front());
        q.erase(q.begin());

        // ★ 설명되는 차이 하나 — 서버 소개의 「현재 인원」 자리.
        //
        // 정품 호스트는 **자기 자신이 플레이어**라 그 값이 접속 상황을 따라 움직인다.
        // 내 것은 전용 서버라 호스트 몫이 없다. 구조가 달라서 원래 다른 자리다.
        // 세션 관리를 붙일 때까지 이 한 자리만 다른 것은 실패로 세지 않는다.
        // (그 자리가 무엇인지는 Docs/PROTOCOL.md 「인원 자리」 참조)
        // ★ 설명되는 차이 둘 — 접속 묶음의 「아직 모르는 두 자리」.
        //
        // 플레이어 정보 97자리 중 95자리는 클라가 보낸 것을 되돌린 것이라 맞는데,
        // 두 자리는 서버가 스스로 채운다. 무엇인지 아직 모른다(Server.h 참조).
        // 그 둘 말고 다 같으면 실패로 세지 않되, **몇 자리가 남았는지는 드러내 둔다.**
        // ⚠ 키를 준 뒤의 조각은 **암호문**이다. 오프코드를 볼 때 풀어야 한다.
        //   (자리는 그대로라 「어디가 다른가」는 암호문에서 세도 된다)
        const uint8_t peerKey = srv.Network().KeyFor(ln.peer);
        bool joinUnknownOnly = false;
        if (mine.size() == ln.data.size() && mine.size() == 218 &&
            (mine[2] ^ peerKey) == static_cast<uint8_t>(nox::Op::INFORM)) {
            joinUnknownOnly = true;
            for (size_t i = 0; i < mine.size(); ++i) {
                if (mine[i] == ln.data[i]) continue;
                const size_t body = i - nox::kWireHeaderSize;
                if (body != nox::kJoinUnknownA && body != nox::kJoinUnknownB) {
                    joinUnknownOnly = false;
                    break;
                }
            }
        }

        const bool infoPlayersOnly =
            mine.size() == ln.data.size() && mine.size() > 3 &&
            mine[2] == static_cast<uint8_t>(nox::Op::SERVER_INFO) &&
            std::memcmp(mine.data(), ln.data.data(), 3) == 0 &&
            std::memcmp(mine.data() + 4, ln.data.data() + 4, mine.size() - 4) == 0;

        if (mine == ln.data) {
            ++nSame;
            if (verbose)
                std::printf("[같음] %s len=%zu  %s\n", ln.peer.c_str(), mine.size(),
                            HexOf(mine).c_str());
        } else if (infoPlayersOnly) {
            ++nExplained;
            if (verbose)
                std::printf("[인원 자리만 다름] %s  정품 %02x / 내것 %02x\n",
                            ln.peer.c_str(), ln.data[3], mine[3]);
        } else if (joinUnknownOnly) {
            ++nJoinUnk;
            std::printf("[접속 묶음 — 아직 모르는 두 자리만 다름] %s  "
                        "179번 %02x/%02x · 181번 %02x/%02x  (정품/내것)\n",
                        ln.peer.c_str(),
                        ln.data[nox::kWireHeaderSize + nox::kJoinUnknownA],
                        mine[nox::kWireHeaderSize + nox::kJoinUnknownA],
                        ln.data[nox::kWireHeaderSize + nox::kJoinUnknownB],
                        mine[nox::kWireHeaderSize + nox::kJoinUnknownB]);
        } else {
            ++nDiff;
            if (shown < show) {
                ++shown;
                std::printf("[다름] %s\n  정품 len=%zu  %s\n  내것 len=%zu  %s\n",
                            ln.peer.c_str(), ln.data.size(), HexOf(ln.data).c_str(),
                            mine.size(), HexOf(mine).c_str());
                // 어디서부터 갈리는지 짚어준다.
                const size_t n = ln.data.size() < mine.size() ? ln.data.size() : mine.size();
                for (size_t i = 0; i < n; ++i) {
                    if (ln.data[i] != mine[i]) {
                        std::printf("  → %zu번 자리부터 갈린다 (정품 %02x / 내것 %02x)\n",
                                    i, ln.data[i], mine[i]);
                        break;
                    }
                }
            }
        }
    }
    std::fclose(f);

    for (auto& kv : pending) nExtra += kv.second.size();

    std::printf("\n=== 채점 ===\n");
    std::printf("  읽은 줄 %llu (받은 것 %llu / 정품이 보낸 것 %llu)\n", nLines, nRecv, nSend);
    std::printf("  바이트까지 같음   %llu\n", nSame);
    std::printf("  인원 자리만 다름  %llu   (전용 서버라 원래 다르다)\n", nExplained);
    std::printf("  다름              %llu\n", nDiff);
    std::printf("  아직 안 만든 것   %llu\n", nMissing);
    std::printf("  내가 더 만든 것   %llu\n", nExtra);
    std::printf("  답을 만든 메시지  %llu\n", srv.handled() + srv.Network().handled());   // ★ 10-08 조각 3 — 접속 제어는 통신 층이 센다
    std::printf("  못 쪼개 통째로 넘긴 조각 %llu   (아직 못 읽는 메시지가 섞인 것)\n",
                srv.stopped() + srv.Network().stopped());

    // 많은 것부터 보여준다. 오프코드 순으로 늘어놓으면 정작 큰 것이 아래로 밀린다.
    auto dump = [](const char* title, const std::map<uint8_t, unsigned long long>& m,
                   size_t top) {
        if (m.empty()) return;
        std::vector<std::pair<uint8_t, unsigned long long>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        std::printf("\n%s\n", title);
        for (size_t i = 0; i < v.size(); ++i) {
            if (i >= top) { std::printf("  ... 그 밖 %zu가지\n", v.size() - i); break; }
            std::printf("  0x%02x  %-34s %llu\n", v[i].first,
                        nox::OpName(static_cast<nox::Op>(v[i].first)), v[i].second);
        }
    };
    dump("아직 안 만든 것 — 오프코드별 (많은 것부터)", missingByOp, 12);
    // ★ 10-08 조각 3 — 접속 제어 조각(0x20 미만)은 통신 층이 센다. 둘을 합쳐 보인다.
    auto merged = [](std::map<uint8_t, unsigned long long> a, const std::map<uint8_t, unsigned long long>& b) {
        for (const auto& kv : b) a[kv.first] += kv.second;
        return a;
    };
    dump("★ 여기서 막혀 조각을 통째로 넘겼다 — 다음에 옮길 것", merged(srv.stoppedByOp(), srv.Network().stoppedByOp()), 12);
    dump("받은 메시지 — 오프코드별 (많은 것부터)", merged(srv.seen(), srv.Network().seen()), 15);

    // 판정 — 만든 자리에서 어긋난 것이 없어야 통과다.
    // 「아직 안 만든 것」은 실패가 아니다. 안 지나간 길일 뿐이다.
    std::printf("\n%s\n", nDiff == 0 ? "[통과] 답을 만든 자리는 전부 바이트가 같다"
                                     : "[실패] 어긋난 자리가 있다");
    return nDiff == 0 ? 0 : 1;
}
