// Server.h — 접속을 받는 서버 로직. **소켓을 모른다.**
//
// 왜 소켓을 떼어놨나 — 판정을 사람 눈에서 떼기 위해서다.
// 여기가 `recvfrom` 을 직접 부르면 채점하려고 매번 클라를 띄워야 한다.
// 들어온 바이트를 넣으면 나갈 바이트가 나오는 모양이라야
// **덤프를 재생해 기계로 채점**할 수 있다 (server/src/replay.cpp).
//
//   [진짜 소켓]  main.cpp    ─┐
//                             ├─▶  Server::OnPacket  ─▶  나갈 조각들
//   [덤프 재생]  replay.cpp  ─┘
//
// 지금 답하는 것은 접속 절차 세 자리뿐이다. 그 밖의 오프코드는 세기만 한다.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nox/Alias.h"
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace nox {

// 나갈 조각 하나.
struct OutPacket {
    std::string to;
    std::vector<uint8_t> data;
};

// ★ 스트림 하나의 순번 — 실측으로 밝힌 규칙이다.
//
//   **신뢰 전송은 순번을 하나 써서 올리고, 비신뢰는 「직전 신뢰가 쓴 값」을 그대로 싣는다.**
//
// 정품 호스트의 한 접속을 풀어 늘어놓으면 그대로 보인다:
//
//   80 00 1f  신뢰   순번 0   ← 쓰고 올린다
//   00 00 04  비신뢰 순번 0   ← 직전 신뢰가 쓴 값
//   80 01 a9  신뢰   순번 1
//   80 02 2b  신뢰   순번 2
//   00 02 a9  비신뢰 순번 2   ← 다시 직전 신뢰 값
//   00 02 27  비신뢰 순번 2
//   80 03 c2  신뢰   순번 3
//   00 03 a9  비신뢰 순번 3
//
// 신뢰를 한 번도 안 보낸 스트림은 0 이다(서버 찾기만 오간 세션이 그렇다).
struct StreamSeq {
    uint8_t next = 0;   // 다음 신뢰 전송이 쓸 값
    uint8_t last = 0;   // 직전 신뢰 전송이 쓴 값 — 비신뢰가 싣는다
};

// 상대 하나. 주소:포트로 가른다.
//
// ⚠ **접속 도중에 클라가 포트를 바꾼다.** 실측에서 TRY_JOIN 은 54721 에서,
//    그 다음 CONNECT 는 63073 에서 왔다. 프로토콜이 그렇게 생겼다
//    (Docs/PROTOCOL.md 「접속 절차」의 "여기서 클라가 새 포트로 옮긴다").
//    그래서 한 사람이 세션 둘로 보인다. 지금은 그대로 둔다.
struct Session {
    std::string addr;
    uint8_t xorKey = 0;      // 0이면 아직 평문
    bool keyed = false;      // 키를 준 뒤인가
    uint32_t connectId = 0;  // SERVER_ACCEPT 에 실어 보낸 번호
    uint32_t frame = 0;      // 이 사람에게 실어 보낼 프레임
    uint16_t netCode = 0;    // 이 사람에게 준 오브젝트 번호
    unsigned long long recvPackets = 0;

    // 게임에 들어와 있는가 — 여기가 참이면 매 틱 상태를 받는다.
    bool inGame = false;
    int x = 0, y = 0;        // 맵 위 자리
    uint8_t facing = 0;      // 바라보는 쪽. 한 바퀴를 256으로 나눈 값

    // 이 상대가 **자기 쪽에서 굴리고 있을** 별칭 표. 서버가 똑같이 굴려야
    // 어긋나지 않는다(Alias.h 참조).
    AliasTable aliases;

    // 스트림마다 따로 센다. 핑·퐁이 스트림 1 을 쓰는 것이 실측으로 확인됐다.
    StreamSeq seq[128];
};

class Server {
public:
    // 서버 찾기에 답할 원본. 머리 2바이트와 오프코드를 뺀 **본문**을 넣는다.
    // 토큰 자리만 요청 값으로 바꿔 그대로 돌려준다 — 모르는 자리를 건드리지 않는 것이
    // 「녹음 재생」의 요점이다.
    void SetInfoTemplate(const uint8_t* body, size_t n);

    // 접속을 받아줄 때 클라에게 줄 XOR 키. 실제 서버는 스스로 정하지만,
    // 덤프와 바이트를 맞춰보려면 그때 쓰인 키를 넣을 수 있어야 한다.
    void SetXorKey(uint8_t key) { xorKey_ = key; }

    // 다음 접속에 매길 번호. 실측은 1부터 올라갔다.
    void SetNextConnectId(uint32_t id) { nextConnectId_ = id; }

    // ★ 접속 대본 — 덤프 재생 전용.
    //
    // **키와 번호는 접속마다 다르다**(실측 0x70 · 0x2a). 서버가 스스로 정하는 값이라
    // 바이트를 맞춰보는 대상이 아니다. 그래서 덤프에서 뽑아 순서대로 주입한다.
    // 이걸 안 하면 두 번째 접속에서 키 한 자리 때문에 "다름"이 떠, 정작 봐야 할
    // **나머지 아홉 자리가 맞는다는 사실**이 묻힌다.
    // 접속 한 번에 정품이 쓴 값들. 전부 **서버가 스스로 정하는 값**이라
    // 바이트를 맞춰보는 대상이 아니다.
    struct AcceptScript {
        uint32_t connectId = 0;
        uint8_t xorKey = 0;
        uint32_t frame = 0;    // 그때의 서버 프레임
        uint16_t netCode = 0;  // 그 사람에게 준 오브젝트 번호
    };
    void SetAcceptScript(std::vector<AcceptScript> script) { script_ = std::move(script); }

    // ★ 접속을 받아준 뒤 한 번에 나가는 것 두 덩이.
    //
    // 정품은 `0x20 CLIENT_ACCEPT` 를 받으면 곧바로 둘을 잇달아 보낸다.
    //
    //   ① 216바이트 묶음 — 메시지 **여섯 개**가 한 조각에 들어 있다
    //        a9 INFORM(관전) 6 · 28 FULL_TIMESTAMP 5 · 2c JOIN_DATA 7 ·
    //        af GAME_SETTINGS 20 · b0 GAME_SETTINGS_2 49 · 2d NEW_PLAYER 129
    //   ② 41바이트 — `2b USE_MAP`(이름 32 + CRC 4 + 프레임 4)
    //
    // 안에 든 값 대부분은 **아직 내가 정하지 못한다**(설정·플레이어 정보).
    // 그래서 원본을 그대로 두고 **접속마다 달라지는 두 자리만** 바꿔 내보낸다 —
    // 그것이 프레임과 NetCode 다. 두 접속의 216바이트를 견줘 확인한 것이다.
    void SetJoinTemplate(const uint8_t* body, size_t n);
    void SetUseMapTemplate(const uint8_t* body, size_t n);

    // 서버의 현재 프레임. 진짜 서버는 30Hz 틱이 올리고,
    // 덤프를 재생할 때는 그때 나간 값을 넣어 준다(시점에 달린 값이라 재현이 안 된다).
    void SetFrame(uint32_t f) { frame_ = f; }
    // 접속한 사람에게 줄 오브젝트 번호. 이것도 서버가 정하는 값이다.
    void SetNetCode(uint16_t c) { netCode_ = c; }

    // ★ 들어오는 것이 이미 평문인가 — 덤프 재생 전용 스위치.
    //
    // 진짜 소켓에서는 키를 준 뒤 받는 것이 암호문이라 풀어야 한다.
    // 그런데 **덤프의 RECV 는 이미 평문이다** — 기록을 건 자리가 복호화 직후라서다
    // (Docs/PROTOCOL.md 「XOR 은 방향에 따라 다르다」). 그걸 또 풀면 되레 망가진다.
    // 이 스위치를 안 두고 돌렸을 때 `0x3f`(클라 입력) 32,450건이
    // `0x4f`(방어구 장착)로 읽혔다 — 정확히 `0x3f ^ 0x70` 이다.
    void SetInboundPlain(bool v) { inboundPlain_ = v; }

    // 이 상대에게 준 XOR 키. 아직 안 줬으면 0.
    // 덤프의 SEND 를 사람이 읽으려면 이게 필요하다.
    uint8_t KeyFor(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : (it->second.keyed ? it->second.xorKey : 0);
    }

    // 들어온 조각 하나. 나갈 것은 out 에 붙인다.
    // 돌려주는 값 = 이 조각에서 읽어낸 메시지 수.
    size_t OnPacket(const std::string& from, const uint8_t* data, size_t n,
                    std::vector<OutPacket>& out);

    // ★ 틱 — 시각을 넣으면 주기가 지난 만큼 프레임을 올리고 상태를 뿌린다.
    //
    // 실측 주기는 **35.77ms(28Hz)** 였다(표본 27,619). 목표가 30Hz(33.3ms)인데
    // 못 따라간 것으로 보여, 여기서는 **30Hz 를 목표로** 둔다.
    // ⚠ 그 실측은 OpenNox 서버의 것이지 정품 호스트의 값이 아니다.
    //
    // 돌려주는 값 = 이번에 지나간 틱 수.
    int Tick(uint32_t nowMs, std::vector<OutPacket>& out);

    uint32_t frame() const { return frame_; }
    unsigned long long moves() const { return moves_; }
    void SetTickMs(uint32_t ms) { tickMs_ = ms; }

    // 읽었지만 아직 답을 안 만드는 오프코드의 집계.
    const std::map<uint8_t, unsigned long long>& seen() const { return seen_; }
    unsigned long long handled() const { return handled_; }
    // 끝까지 못 쪼개 통째로 넘긴 조각 수. 아직 못 읽는 메시지가 섞였다는 뜻이다.
    unsigned long long stopped() const { return stopped_; }
    // **무엇이 막았나** — 길이를 몰라 멈춘 자리의 오프코드별 집계.
    // 이것이 다음에 무엇을 옮겨야 하는지 알려주는 나침반이다.
    const std::map<uint8_t, unsigned long long>& stoppedByOp() const { return stoppedByOp_; }

private:
    Session& SessionFor(const std::string& addr);

    // 머리 2바이트를 붙여 한 조각을 만든다. keyed 면 XOR 을 건다.
    void Emit(Session& s, bool reliable, uint8_t stream,
              const uint8_t* msg, size_t n, std::vector<OutPacket>& out);

    std::map<std::string, Session> sessions_;
    std::map<uint8_t, unsigned long long> seen_;
    unsigned long long handled_ = 0;   // 답을 만든 메시지 수
    unsigned long long stopped_ = 0;   // 끝까지 못 쪼개 통째로 넘긴 조각 수
    std::map<uint8_t, unsigned long long> stoppedByOp_;

    std::vector<uint8_t> infoBody_;
    std::vector<uint8_t> joinBody_;     // 접속 직후 나가는 216바이트 묶음
    std::vector<uint8_t> useMapBody_;   // 그 다음 41바이트 USE_MAP
    uint32_t frame_ = 0;
    uint16_t netCode_ = 0;
    uint8_t xorKey_ = 0x70;
    uint32_t nextConnectId_ = 1;
    bool inboundPlain_ = false;
    uint32_t tickMs_ = 33;      // 30Hz 목표
    uint32_t lastTickMs_ = 0;
    bool tickStarted_ = false;
    unsigned long long moves_ = 0;
    std::vector<AcceptScript> script_;   // 접속 대본
    size_t scriptAt_ = 0;
};

// 플레이어의 와이어 종류 번호. 실측으로 확인했다 —
// 상태 갱신 30,169건이 이 번호였고, thing.bin 712번 `NewPlayer`(복합)와 맞는다.
constexpr uint16_t kPlayerType = 713;

// 한 걸음에 얼마나 미나. **실측이 아니다** — 눈으로 확인할 만한 값으로 잡아둔 것이고,
// 정품과 맞춰보려면 캡처가 필요하다.
constexpr double kStepPerTick = 12.0;

// SERVER_INFO 본문에서 토큰이 앉는 자리. 실측으로 확정했다.
constexpr size_t kInfoTokenOffset = 41;

// ── 접속 직후 216바이트 묶음에서 접속마다 달라지는 자리 ──────────
//
// 두 접속(키 0x70·0x2a)의 216바이트를 나란히 놓고 다른 자리만 뽑은 것이다.
// 묶음 안 메시지 경계와도 맞는다 — 6·5·7·20·49·129 로 딱 떨어진다.
constexpr size_t kJoinFrameA = 7;    // 28 FULL_TIMESTAMP 의 값 (LE32)
constexpr size_t kJoinNetCodeA = 12; // 2c JOIN_DATA 의 번호 (LE16)
constexpr size_t kJoinFrameB = 19;   // af GAME_SETTINGS 안의 프레임 (LE32)
constexpr size_t kJoinNetCodeB = 88; // 2d NEW_PLAYER 의 번호 (LE16)

// ★ NEW_PLAYER 안의 플레이어 정보 97바이트는 **거의 그대로 클라가 보낸 것**이다.
//   `0x20 CLIENT_ACCEPT` 본문의 앞 97바이트와 견주니 —
//     첫 접속  97자리 **전부 일치**
//     둘째 접속 95자리 일치, **89·91번 두 자리만 서버가 채웠다**(0x20·0x32)
//   그래서 서버가 지어낼 것은 그 두 자리뿐이고, 나머지는 받은 것을 옮기면 된다.
constexpr size_t kJoinPlayerInfo = 90;
constexpr size_t kJoinPlayerInfoLen = 97;

// ⚠ 아직 정체를 모르는 두 자리 (묶음 **본문** 기준. 조각으로는 여기에 2를 더한다)
//   첫 접속은 둘 다 0 이고 둘째 접속에서만 값이 붙었다. 접속 순서·팀·관전 같은
//   「그 사람의 상태」로 보이지만 확인 못 했다.
constexpr size_t kJoinUnknownA = kJoinPlayerInfo + 89;   // 179
constexpr size_t kJoinUnknownB = kJoinPlayerInfo + 91;   // 181

// USE_MAP 본문 — 이름 32 + CRC 4 + 프레임 4
constexpr size_t kUseMapFrame = 37;

}  // namespace nox
