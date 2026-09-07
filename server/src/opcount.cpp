// opcount.cpp — 덤프의 오프코드를 방향별로 센다.
//
// 왜 만드나 — 손으로 XOR 을 풀다 두 번 틀렸다(포트가 바뀌어 키가 안 붙고,
// IP 로 묶으니 접속 셋의 키가 섞였다). 프로젝트가 이미 쥔 파서를 그대로 쓴다.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "nox/DumpSniff.h"
#include "nox/MsgLen.h"
#include "nox/Opcode.h"
#include "nox/Wire.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("쓰기: opcount <덤프> [찾을오프코드hex ...]\n"); return 1; }

    // ★★ 덤프가 **어디서 떴는지**에 따라 클라→서버가 평문일 수 있다.
    //   서버 자신이 뜬 덤프(SNAP_server_*)는 **받은 것을 푼 뒤** 적으므로 RECV 가 평문이고,
    //   프록시가 뜬 덤프(CAP_*)는 와이어 그대로라 양쪽 다 암호문이다.
    //   이걸 안 가르고 RECV 에까지 XOR 을 씌웠더니 96,290 중 26,500 이 멈췄고
    //   클라가 FX_TELEPORT 를 51,941번 보낸 것처럼 보였다(2026-09-08).
    //   지문 = 접속 직후 158바이트 줄에 `4a 00 61 00 63 00 6b 00`("Jack")이 **그냥 보인다**.
    bool recvPlain = false;
    std::map<int, bool> want;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--recv-plain") == 0) { recvPlain = true; continue; }
        want[(int)std::strtol(argv[i], nullptr, 16)] = true;
    }

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("못 엶: %s\n", argv[1]); return 1; }

    static char buf[65536];
    nox::dump::Line ln;
    std::map<std::string, uint8_t> keys;          // 상대별 XOR 키
    std::map<int, long long> sendOps, recvOps;
    long long nSend = 0, nRecv = 0, preKey = 0, stallSend = 0, stallRecv = 0;  // ★ 멈춘 자리도 센다

    while (std::fgets(buf, sizeof(buf), f)) {
        if (!nox::dump::ParseLine(buf, ln)) continue;
        std::vector<uint8_t> d = ln.data;
        if (d.size() < 3) continue;

        const bool send = (ln.dir == "SEND");
        // 수락 조각은 평문이고 여기에 키가 실린다
        if (send && d.size() == 10 && d[2] == (uint8_t)nox::Op::ACCEPTED) {
            keys[ln.peer] = d[9];
        }
        // ⚠ **IP 로 폴백하면 안 된다.** 접속이 여럿이면 남의 키를 씌우고,
        //   서버 찾기 단계는 아직 평문인데 거기까지 뒤집어 망친다.
        //   그렇게 했더니 클라→서버 상위가 FX_TELEPORT 51,941 이 나왔다(2026-09-08).
        //   수락은 클라가 포트를 바꾼 **뒤**의 그 포트로 나가므로, 정확히 맞춰도 붙는다.
        auto k = keys.find(ln.peer);
        const bool decrypt = (send || !recvPlain);
        if (decrypt && k != keys.end() && k->second)
            nox::ApplyXor(d.data(), d.size(), k->second);
        const bool keyed = (k != keys.end());

        if (send) ++nSend; else ++nRecv;

        nox::MessageIter it(d.data() + nox::kWireHeaderSize,
                            d.size() - nox::kWireHeaderSize, !send);
        int got = 0;
        while (it.Next()) {
            ++got;
            const int op = (int)it.op();
            if (send) ++sendOps[op]; else ++recvOps[op];
            // ★★ `0xcc SEQ_IMPORTANT` 는 **껍데기**다 — cc | 번호 2 | 속길이 1 | 속 메시지.
            //   속을 안 더하면 REPORT_* 가 통째로 0건으로 보인다. 2026-09-06 에 그 0 을
            //   「서버가 장비를 안 줬다」로 읽어 밤을 새웠다. 여기서 같은 실수를 안 한다.
            if (op == (int)nox::Op::SEQ_IMPORTANT && it.bodyLen() >= 4) {
                const uint8_t* ib = it.body();
                const int innerLen = ib[2];
                if (innerLen >= 1 && (size_t)(3 + innerLen) <= it.bodyLen()) {
                    const int iop = ib[3];
                    if (send) ++sendOps[iop]; else ++recvOps[iop];
                    if (want.count(iop)) {
                        std::printf("  ★속 %s %s op=0x%02x(껍데기 0xcc 안) | ",
                                    ln.dir.c_str(), ln.peer.c_str(), iop);
                        for (int i = 0; i < innerLen && i < 16; ++i) std::printf("%02x ", ib[3 + i]);
                        std::printf("\n");
                    }
                }
            }
            if (want.count(op)) {
                std::printf("  ★ %s %s op=0x%02x len=%zu | ", ln.dir.c_str(),
                            ln.peer.c_str(), op, d.size());
                for (size_t i = 0; i < d.size() && i < 24; ++i) std::printf("%02x ", d[i]);
                std::printf("\n");
            }
        }
        // 키를 아직 못 받은 상대(서버 찾기 단계)는 「멈춤」이 아니다. 갈라 센다.
        if (got == 0) { if (!keyed) ++preKey; else if (send) ++stallSend; else ++stallRecv; }
    }
    std::fclose(f);

    // ⚠ 0 을 「없었다」로 읽지 않으려면 멈춘 자리 수를 같이 봐야 한다.
    std::printf("\n조각 SEND %lld · RECV %lld · **한 메시지도 못 읽은 조각 %lld**\n",
                nSend, nRecv, stallSend + stallRecv);
    std::printf("   갈라 보면 SEND %lld · RECV %lld\n", stallSend, stallRecv);
    std::printf("   (키 받기 전이라 안 푼 조각 %lld — 이건 멈춤이 아니다)\n", preKey);
    std::printf("\n-- 서버→클라 상위 --\n");
    std::multimap<long long, int> a;
    for (auto& p : sendOps) a.insert({-p.second, p.first});
    int n = 0;
    for (auto& p : a) { if (++n > 14) break;
        std::printf("   0x%02x  %-34s x %lld\n", p.second, nox::OpName((nox::Op)p.second), -p.first); }
    std::printf("\n-- 클라→서버 상위 --\n");
    std::multimap<long long, int> b;
    for (auto& p : recvOps) b.insert({-p.second, p.first});
    n = 0;
    for (auto& p : b) { if (++n > 14) break;
        std::printf("   0x%02x  %-34s x %lld\n", p.second, nox::OpName((nox::Op)p.second), -p.first); }

    for (auto& w : want) {
        std::printf("\n찾던 0x%02x : 서버→클라 %lld건 · 클라→서버 %lld건\n",
                    w.first, sendOps.count(w.first) ? sendOps[w.first] : 0,
                    recvOps.count(w.first) ? recvOps[w.first] : 0);
    }
    return 0;
}
