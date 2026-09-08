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
    // ★ `--updates N` — 뜯은 상태 갱신 앞 N 개를 항목까지 펼쳐 찍는다.
    //   「정품이 사람을 무슨 **종류 번호**로 싣나」를 눈으로 보는 자리다.
    long updShow = 0, updShown = 0;
    std::map<int, bool> want;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--recv-plain") == 0) { recvPlain = true; continue; }
        if (std::strcmp(argv[i], "--updates") == 0 && i + 1 < argc) {
            updShow = std::strtol(argv[++i], nullptr, 10);
            continue;
        }
        want[(int)std::strtol(argv[i], nullptr, 16)] = true;
    }

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("못 엶: %s\n", argv[1]); return 1; }

    static char buf[65536];
    nox::dump::Line ln;
    std::map<std::string, uint8_t> keys;          // 상대별 XOR 키
    std::map<int, long long> sendOps, recvOps;
    // 판독이 멈춘 오프코드. 0x1000 비트를 세워 클라→서버를 가른다.
    std::map<int, long long> stopOps;
    long long nSend = 0, nRecv = 0, preKey = 0, stallSend = 0, stallRecv = 0, partSend = 0, partRecv = 0, updUnread = 0, padTail = 0;  // ★ 멈춘 자리도 센다

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

        // ★★ **직접 훑는다.** `MessageIter` 는 길이표에 -1 인 오프코드를 만나면 선다.
        //   정품은 `0x27 TIMESTAMP` 뒤에 `0xa4 UPDATE_STREAM`(-1)을 붙여 보내므로,
        //   그대로 두면 조각의 93~98% 가 뒤쪽을 못 읽힌 채 「그건 0건」으로 보인다.
        //   `0xa4` 는 길이가 표에 없을 뿐 **뜯을 수는 있다** — DecodeUpdate 가
        //   먹은 바이트 수(consumed)를 돌려주니 그걸로 건너뛴다.
        const uint8_t* b = d.data() + nox::kWireHeaderSize;
        const size_t bodyAll = d.size() - nox::kWireHeaderSize;
        size_t eaten = 0;
        int got = 0;
        while (eaten < bodyAll) {
            // ★★ **조각 끝의 0 채움은 메시지가 아니다.** 남은 게 전부 0 이면 거기서 끝난다.
            //   이걸 안 가르면 `0x00 MSG_SERVER_CONNECT`(본문 0)로 읽혀
            //   **서버가 클라에게 CONNECT 를 11,020번 보낸 것처럼** 보인다(2026-09-08).
            {
                bool allZero = true;
                for (size_t i = eaten; i < bodyAll; ++i)
                    if (b[i] != 0) { allZero = false; break; }
                if (allZero) { padTail += bodyAll - eaten; eaten = bodyAll; break; }
            }
            const int op = b[eaten];
            // ⚠ **길이는 `MessageSize` 로 물을 것.** 원시 길이표(MsgBodyLenByDir)만 보면
            //   `0x3f PLAYER_INPUT` 처럼 **본문 첫 바이트에 길이가 든** 메시지를 못 다뤄
            //   클라→서버가 통째로 안 읽힌다(2026-09-08: RECV 96,290 중 94,581 멈춤).
            size_t msz = nox::MessageSize(b + eaten, bodyAll - eaten, !send);
            // ★★ `0xaa MSG_IMPORTANT` 는 **`0xcc` 껍데기들의 목록**이다.
            //   길이표의 「본문 5」는 껍데기 머리(cc | 번호2 | 속길이1)까지만 센 값이라
            //   두 번째 껍데기부터 어긋난다. 실측 짜임(2026-09-08):
            //     aa │ cc 17 00 07 │ 6a cb 02 00 00 00 00 │ cc 18 00 …
            //          머리 4        속 7바이트              다음 껍데기
            //   이걸 몰라 서버→클라 662건이 멈췄고, 어긋난 자리의 0 을
            //   `0x00 SERVER_CONNECT` 로 읽어 **3만 건이 헛것으로** 잡혔다.
            // ⚠ 길이표의 `0xAA = 5` 보다 **껍데기 순회를 우선**한다. 표의 5 는 껍데기
            //   머리까지만 센 값이라 두 번째 껍데기부터 어긋난다. 다만 `cc` 로 시작하지
            //   않는 형태(`aa 32 0d 02` 꼴)도 있어, 그때는 아래에서 표로 되돌아간다.
            //   ★ 표의 5 자체는 **건드리지 않는다** — 4 로 바꿔 봤더니 RECV 미판독이
            //     9 → 15,737 로 폭증하고 대조 장치가 「다름 16,730」으로 잡았다(2026-09-08).
            //     opennox 소스는 소비 5(본문 4)로 읽히지만 **우리 실측은 본문 5 다.**
            //   ★ **서버→클라에만** 쓴다. 클라→서버는 표의 5 가 실측으로 맞고,
            //     껍데기 순회를 그쪽까지 쓰면 RECV 미판독이 9 → 69 로 늘었다.
            if (op == (int)nox::Op::IMPORTANT && send) {
                size_t q = eaten + 1;
                bool sane = false;
                while (q + 4 <= bodyAll && b[q] == (uint8_t)nox::Op::SEQ_IMPORTANT) {
                    const size_t inner = b[q + 3];
                    if (q + 4 + inner > bodyAll) break;
                    // 껍데기 속 메시지도 센다 — 여기 REPORT_* 들이 들어 있다.
                    if (inner >= 1) {
                        const int iop = b[q + 4];
                        if (send) ++sendOps[iop]; else ++recvOps[iop];
                    }
                    q += 4 + inner;
                    sane = true;
                }
                if (sane) msz = q - eaten;
                // ★★ `cc` 로 시작하지 않으면 **본문 0** 이다 — 그 뒤가 바로 다음 메시지다.
                //   opennox `network_client.go:302` 의 `n = 1`(호스트가 아닐 때) 분기다.
                //   실측 표본이 그대로 맞는다(2026-09-08):
                //     aa 32 0d 02  →  0x32 OBJECT_OUT_OF_SIGHT(본문 2) + `0d 02`
                //     aa 31 d6 02  →  0x31 DESTROY_OBJECT(본문 2)     + `d6 02`
                else msz = 1;
            }
            if (msz == 0) {
                if (op == (int)nox::Op::UPDATE_STREAM) {
                    // ⚠ 별칭 표 없이 뜯는다 — 별칭으로만 온 항목은 종류를 모르니
                    //   디코더가 갈래를 넓게 연다. 길이를 얻는 데는 그걸로 족하다.
                    const nox::UpdateMsg um =
                        nox::DecodeUpdate(b + eaten + 1, bodyAll - eaten - 1, nullptr);
                    if (um.ok && um.consumed > 0) {
                        msz = 1 + um.consumed;
                        if (updShown < updShow) {
                            ++updShown;
                            // ★ 머리의 `flags`·`headFrame`·`headAnim` 까지 찍는다 —
                            //   여기에 「무엇을 어떤 자세로 그릴지」가 실릴 수 있다.
                            std::printf("  [갱신 %ld] %s %s  머리 별칭=%02x 번호=%u 종류=%u "
                                        "자리=(%d,%d) flags=%02x frame=%02x anim=%02x 항목 %zu개\n",
                                        updShown, ln.dir.c_str(), ln.peer.c_str(),
                                        um.headAlias, um.headNetCode, um.headType,
                                        um.x, um.y, um.flags, um.headFrame, um.headAnim,
                                        um.objects.size());
                            for (const auto& e : um.objects) {
                                std::printf("       별칭=%02x 번호=%-5u 종류=%-5u 자리=(%d,%d) 덧붙이=%d [",
                                            e.alias, e.netCode, e.type, e.x, e.y, e.extra);
                                // ★ 덧붙이 **값**까지 찍는다 — 방향·애니 프레임이 여기 들어 있다.
                                //   개수만 봐서는 「무엇을 그릴지」를 못 가른다.
                                for (int j = 0; j < e.extra && j < 3; ++j)
                                    std::printf("%02x ", e.extraBytes[j]);
                                std::printf("]\n");
                            }
                        }
                    }
                    else { ++updUnread; break; }
                } else {
                    // ★ **어디서 멈췄는지 센다.** 이게 없으면 「32% 를 못 읽는다」까지만 알고
                    //   무엇을 고쳐야 하는지는 모른다(2026-09-08).
                    const int key = send ? op : (op | 0x1000);
                    // 그 자리 바이트도 앞의 몇 건만 찍는다 — 길이를 눈으로 재는 자리다.
                    if (stopOps[key] < 8) {
                        std::printf("  ▲멈춤 %s op=0x%02x 남은%zu | ", ln.dir.c_str(), op,
                                    bodyAll - eaten);
                        for (size_t i = eaten; i < bodyAll && i < eaten + 20; ++i)
                            std::printf("%02x ", b[i]);
                        std::printf("\n");
                    }
                    ++stopOps[key];
                    break;   // 길이를 모르는 다른 오프코드 — 여기서 접는다
                }
            }
            const int bl = (int)msz - 1;
            if (eaten + msz > bodyAll) break;
            ++got;
            if (send) ++sendOps[op]; else ++recvOps[op];
            // ★★ `0xcc SEQ_IMPORTANT` 는 **껍데기**다 — cc | 번호 2 | 속길이 1 | 속 메시지.
            //   속을 안 더하면 REPORT_* 가 통째로 0건으로 보인다. 2026-09-06 에 그 0 을
            //   「서버가 장비를 안 줬다」로 읽어 밤을 새웠다. 여기서 같은 실수를 안 한다.
            if (op == (int)nox::Op::SEQ_IMPORTANT && bl >= 4) {
                const uint8_t* ib = b + eaten + 1;
                const int innerLen = ib[2];
                if (innerLen >= 1 && 3 + innerLen <= bl) {
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
                // ★ 조각 앞부분이 아니라 **그 메시지의 본문**을 찍는다.
                //   앞부분만 찍으면 뒤에 붙은 메시지는 영영 안 보인다(2026-09-08).
                std::printf("  ★ %s %s op=0x%02x 본문%d바이트 자리%zu/%zu | ", ln.dir.c_str(),
                            ln.peer.c_str(), op, bl, eaten, bodyAll);
                for (int i = 0; i < bl && i < 32; ++i)
                    std::printf("%02x ", b[eaten + 1 + i]);
                // ★★ **앞뒤 문맥**도 같이. 본문이 0 인 오프코드는 본문만 봐서는
                //   진짜인지 오독인지 못 가른다 — 앞에 무엇이 끝났고 뒤에 무엇이 오는지가 증거다.
                std::printf(" ‖앞 ");
                for (size_t i = (eaten >= 6 ? eaten - 6 : 0); i < eaten; ++i)
                    std::printf("%02x ", b[i]);
                std::printf("‖뒤 ");
                for (size_t i = eaten + 1 + (size_t)bl; i < bodyAll && i < eaten + 1 + bl + 8; ++i)
                    std::printf("%02x ", b[i]);
                std::printf("\n");
            }
            eaten += 1 + (size_t)bl;
        }
        // 키를 아직 못 받은 상대(서버 찾기 단계)는 「멈춤」이 아니다. 갈라 센다.
        if (got == 0) { if (!keyed) ++preKey; else if (send) ++stallSend; else ++stallRecv; }
        // ★★ **끝까지 못 읽은 조각**을 따로 센다. 이게 없으면 「멈춤 0」인데도
        //   조각의 절반을 못 본 채 「그 오프코드는 없었다」로 읽게 된다.
        //   실제로 그랬다 — 정품이 TIMESTAMP 뒤에 붙여 보내는 `0xa4`(길이 -1)에서
        //   파서가 멈추는데, 첫 메시지는 읽었으니 「멈춤」에 안 잡혔다.
        //   그 바람에 「정품은 상태 갱신을 안 보낸다」고 잘못 읽었다(2026-09-08).
        else if (eaten < bodyAll) { if (send) ++partSend; else ++partRecv; }
    }
    std::fclose(f);

    // ⚠ 0 을 「없었다」로 읽지 않으려면 멈춘 자리 수를 같이 봐야 한다.
    std::printf("\n조각 SEND %lld · RECV %lld · **한 메시지도 못 읽은 조각 %lld**\n",
                nSend, nRecv, stallSend + stallRecv);
    std::printf("   갈라 보면 SEND %lld · RECV %lld\n", stallSend, stallRecv);
    std::printf("   **끝까지 못 읽은 조각** SEND %lld · RECV %lld"
                "  (첫 메시지는 읽었으나 뒤가 남음)\n", partSend, partRecv);
    std::printf("   0xa4 를 못 뜯어 접은 조각 %lld · 끝의 채움 0 %lld바이트\n",
                updUnread, padTail);
    std::printf("   (키 받기 전이라 안 푼 조각 %lld — 이건 멈춤이 아니다)\n", preKey);
    // ★ **어느 오프코드에서 멈췄나.** 「몇 퍼센트를 못 읽는다」까지만 알면
    //   무엇을 고쳐야 하는지는 모른다. 고칠 자리를 이름으로 짚어 준다.
    if (!stopOps.empty()) {
        std::printf("\n-- 판독이 멈춘 자리 --\n");
        std::multimap<long long, int> sm;
        for (auto& p : stopOps) sm.insert({-p.second, p.first});
        int sn = 0;
        for (auto& p : sm) {
            if (++sn > 10) break;
            const int op = p.second & 0xff;
            std::printf("   %s 0x%02x  %-32s x %lld\n",
                        (p.second & 0x1000) ? "클라→서버" : "서버→클라",
                        op, nox::OpName((nox::Op)op), -p.first);
        }
    }
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
