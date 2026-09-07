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
#include "nox/Messages.h"   // InformCode
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace nox {

// 나갈 조각 하나.
struct OutPacket {
    std::string to;
    std::vector<uint8_t> data;
};

// 불려 나온 것 하나. 부른 사람 편이라 부른 사람은 안 때린다.
struct Creature {
    std::string owner;     // 부른 사람의 주소
    uint16_t netCode = 0;
    uint16_t type = 0;     // 와이어 종류 번호 (⚠ 종류표에서 이름으로 고른 것. 실측 아님)
    uint8_t facing = 0;
    double x = 0, y = 0;
    uint16_t health = 0;
    uint32_t nextHitFrame = 0;   // 이 프레임 전에는 못 때린다
};

// 깃발 하나.
//
// ⚠ **종류 번호는 실측이 아니라 이름 짐작이다** (09-07 자가검증에서 잡음).
//   종류표에 `176 RedFlag`·`173 BlueFlag` 라는 **이름이 있다**는 것뿐이고,
//   **네 덤프 어디에도 깃발 오브젝트가 0건**이다(CTF 로 돌린 적이 없다).
//   투사체 쪽에서 같은 방식으로 고른 `695 Fireball` 이 0건이었던 것과 같은 자리다.
//   ★ 다만 **둘 다 복합이 아니라는 것은 표에서 확인**했다 — 붙는 바이트가 없다.
struct Flag {
    uint8_t team = 0;          // 이 깃발의 임자 팀
    uint16_t netCode = 0;
    uint16_t type = 0;
    double homeX = 0, homeY = 0;   // 제자리
    double x = 0, y = 0;           // 지금 자리
    std::string carrier;           // 들고 있는 사람. 비었으면 바닥에 있다
    bool atHome = true;
};

// 공격이 닿을 수 있는 것 하나 — **사람이거나 불려 나온 것**이다.
//
// 왜 한 덩이로 묶나 — 근접·투사체·소환수가 저마다 「누구를 때리나」를 따로 고르면
// 셋이 조금씩 다르게 굴어 어긋난다. 고르는 자리를 하나로 모은다.
struct Target {
    std::string addr;   // 사람이면 그 주소
    int pet = -1;       // 불려 나온 것이면 그 자리 번호
    bool none() const { return addr.empty() && pet < 0; }
};

// 날아가는 것 하나. 쏜 사람은 안 맞는다.
struct Projectile {
    std::string owner;   // 쏜 사람의 주소
    uint8_t spell = 0;
    uint16_t netCode = 0;  // 클라에게 「이게 그거다」로 알릴 번호
    uint8_t facing = 0;    // 날아가는 쪽(한 바퀴를 256으로)
    double x = 0, y = 0;
    double vx = 0, vy = 0;
    double traveled = 0; // 지금까지 간 거리. 사거리를 넘으면 사라진다
    int damage = 0;
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
    // ★ 첫 세계 스냅샷을 몇 틱 뒤에 보낼지. 접속 직후 같은 묶음으로 보내면 클라가 못 쓴다
    //   (정품도 다른 조각 여덟 개가 지난 뒤 아홉째로 보낸다 — 2026-09-07 이분법).
    int snapDelay = -1;
    int x = 0, y = 0;        // 맵 위 자리
    uint8_t facing = 0;      // 바라보는 쪽. 한 바퀴를 256으로 나눈 값

    // ── 체력과 죽음 (2026-09-07) ─────────────────────────
    // 짜임과 값은 캡처에서 얻었다(Docs/PROTOCOL.md 「죽음과 부활 ✅」·「체력」 절).
    uint16_t health = 0;
    uint16_t maxHealth = 0;
    uint32_t respawnAtFrame = 0;   // 0 이면 살아 있다
    uint16_t seqImpId = 0;         // 0xcc 껍데기의 번호. 실측도 1씩 오른다
    uint32_t nextMeleeFrame = 0;   // 이 프레임 전에는 근접이 안 나간다
    uint8_t team = 0;              // 0 = 무소속 · 1 = 빨강 · 2 = 파랑
    uint16_t kills = 0;            // 죽인 수 (아레나 점수)
    uint16_t deaths = 0;           // 죽은 수

    // 이 상대가 **자기 쪽에서 굴리고 있을** 별칭 표. 서버가 똑같이 굴려야
    // 어긋나지 않는다(Alias.h 참조).
    AliasTable aliases;

    // 스트림마다 따로 센다. 핑·퐁이 스트림 1 을 쓰는 것이 실측으로 확인됐다.
    StreamSeq seq[128];
};

// 접속할 때 주는 최대 체력. **실측값이다** —
//   dd d0 02 4b 00 4b 00   번호 720 · 75/75      (`0xDD REPORT_TOTAL_HEALTH`)
// 다른 상대는 150 이었다(직업이나 레벨에 달린 것으로 보이나 확인 못 했다).
constexpr uint16_t kDefaultMaxHealth = 75;

// ⚠ **죽고 나서 얼마 만에 부활하나 — 이 값은 실측이 아니다.**
//   09-06 덤프의 죽음은 단 두 건이고, 그 뒤 같은 번호의 부활이 안 잡혔다.
//   그래서 잴 수가 없었다. 30Hz 기준 5초로 두었을 뿐이니 **캡처로 갈아 끼울 자리**다.
constexpr uint32_t kRespawnDelayTicks = 150;

// ⚠ **주문 사거리도 실측이 아니다.** 맞았는지 안 맞았는지 가릴 근거가 덤프에 없다
//   (`0x42` 가 나간 순간의 두 사람 거리를 짝지을 수 없었다). 걸음 7/틱 기준
//   서른 걸음쯤으로 두었을 뿐이다.
constexpr double kSpellRange = 210.0;

// ── 근접 공격 (왼버튼) ──────────────────────────────────────
//
// **왼버튼이 `CCAction`(6)이라는 것은 실측이다** — 두 출처가 맞았다:
//   ① 09-07 조작 실측 — 왼버튼을 누르니 코드 6 이 눌린 횟수만큼 왔다
//   ② opennox `noxnet/msg_player_input.go:210` — `CCAction = CtrlCode(6)`
// ⚠ 다만 **줍기는 아니다.** 왼버튼으로 물건 위에서 눌러도 `0x73 TRY_GET` 이 안 나갔다
//   (2회 시도 실패로 확인). 줍기는 다른 길이다.
//
// ⚠⚠ **아래 세 숫자는 실측이 아니다.** 주문 쪽과 같은 사정이다 —
//    09-06 덤프의 `0x42` 표본이 무슨 공격이 만든 값인지 가릴 수 없었다.
//    닿는 거리는 「검 한 자루쯤」, 다시 때리기까지는 「초당 두 대쯤」으로 둔 자리표시다.
// ── 날아가는 것 (투사체) ────────────────────────────────────
//
// 왜 필요한가 — 시전하는 순간 맞으면 **피할 수가 없다.** 녹스에서 파이어볼은
// 날아가고, 옆으로 비키면 안 맞는다. 그게 있어야 싸움이 싸움이 된다.
//
// ⚠⚠ **아래 둘도 실측이 아니다.** 덤프에 투사체의 자리가 안 실린다 —
//    `0xa4` 상태 갱신에 오브젝트로 들어오지만 그것이 파이어볼인지 가릴 종류 번호를
//    아직 못 짚었다. 걸음(7/틱)보다 서너 배 빠른 값으로 두었을 뿐이다.
// ── 불려 나온 것 (소환) ─────────────────────────────────────
//
// ⚠⚠ **아래 넷도 실측이 아니다.** 소환 표본이 덤프에 없다 — 09-06·09-07 어느 캡처에도
//    `SPELL_SUMMON_*` 이 안 왔다(주문 표본 83개가 전부 공격·이동·치유였다).
//    걸음(7/틱)보다 조금 느리게, 사람보다 약하게 둔 자리표시다.
constexpr double kPetSpeed = 5.0;          // 한 틱에 다가가는 거리
constexpr double kPetReach = 40.0;         // 이 안에 들어오면 때린다
constexpr int kPetDamage = 8;
constexpr uint16_t kPetHealth = 40;

// ── 아레나 (점수 겨루기) ────────────────────────────────────
//
// ⚠ **점수 한도는 실측이 아니다.** 콘솔의 `set deaths {n}` 으로 서버가 정하는 값이고
//   (→ `Docs/CONSOLE.md`), 우리 캡처는 `Game Type: Arena` 에 한도 없이 돌았다.
//   열 명 죽이면 끝나는 것으로 두었을 뿐이다.
constexpr uint16_t kArenaKillLimit = 10;

// 판 시계를 얼마마다 뿌리나. 실측 표본은 **2초 남짓 간격**이었다
// (15:05:08.299 · 10.538 · 12.709 · 14.867 — 2.2초씩). 30Hz 기준 그만큼으로 둔다.
constexpr uint32_t kTimerEveryTicks = 66;

// ── 깃발 (CTF) ──────────────────────────────────────────────
//
// ⚠⚠ **이 절에는 실측이 하나도 없다.** 종류 번호마저 이름 짐작이다 —
//    종류표에 `176 RedFlag`·`173 BlueFlag` 라는 이름이 있을 뿐 덤프에는 0건이다.
//    (표에서 확인한 것은 **복합이 아니라는 것** 하나뿐이고, 그건 짜임에만 쓴다.)
//
//    까닭은 하나다 — 우리 캡처는 `Game Type: Arena` 하나뿐이고
//    (→ `Docs/CONSOLE.md`), OpenNox 전용 서버는 **CTF 맵을 못 올린다**
//    (`panic: server should not load GUI` — 09-07 확인). 그래서 CTF 트래픽 표본이 0이다.
//    깃발 자리·닿는 거리·득점 한도·팀 배정 규칙은 **게임이 도는지 보려고 둔 자리표시**다.
constexpr uint16_t kFlagTypeRed = 176;
constexpr uint16_t kFlagTypeBlue = 173;
constexpr double kFlagTouchRange = 40.0;
constexpr uint16_t kCaptureLimit = 3;

// ── 퀘스트 ──────────────────────────────────────────────────
//
// 「불려 나온 것」과 「맵에 사는 몬스터」는 **같은 것**으로 다룬다 —
// `Creature` 의 주인(`owner`)이 비어 있으면 아무 편도 아니고, 그래서 모두를 노린다.
// 사람 쪽에서도 주인이 자기가 아니니 때릴 수 있다. 규칙 하나로 둘이 풀린다.
//
// ⚠ **마릿수·자리·체력은 실측이 아니다.** 퀘스트 맵(`G_*`)을 올려 본 적이 없다 —
//   우리 캡처는 `estate.map`(Arena) 하나뿐이다.
constexpr int kQuestMonsters = 5;
constexpr double kQuestSpread = 300.0;   // 이만큼 벌려 세운다

constexpr double kProjectileSpeed = 26.0;    // 한 틱에 미는 거리
constexpr double kProjectileHitRadius = 24.0; // 이 안에 들어오면 맞은 것으로 본다

constexpr double kMeleeRange = 45.0;
constexpr int kMeleeDamage = 12;
constexpr uint32_t kMeleeCooldownTicks = 15;   // 30Hz 기준 0.5초
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
    // ★ 맵 지정 뒤에 이어 보낼 것들(정품이 보내는 105B INFORM · 142B 공지 · 205B INFORM).
    //   없으면 클라가 로딩에서 안 넘어간다.
    void SetPostMap(const std::vector<std::vector<uint8_t>>& v) { postMap_ = v; }
    // ★ 접속 직후 한 번 내보낼 첫 세계 스냅샷. 없으면 클라 화면이 검은 채로 남는다.
    void SetFirstSnapshot(const std::vector<uint8_t>& v) { firstSnapshot_ = v; }
    // ★ 진단용 — 켜면 매 틱 「내가 만든 것」 대신 **정품이 보냈던 조각**을 순서대로 내보낸다.
    void SetMirror(const std::vector<std::vector<uint8_t>>& v) { mirror_ = v; mirrorAt_ = 0; }
    bool MirrorOn() const { return !mirror_.empty(); }

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

    // 이 주문이 몇 대미지인가. 공격 주문이 아니면 0.
    //
    // ⚠⚠ **숫자는 실측이 아니다.** 09-06 덤프에 `0x42` 표본이 −24·−36·−75·−127 로 있지만
    //    **어느 공격이 만든 값인지 가릴 수 없었다**(그 시각에 주문·근접이 섞여 있다).
    //    어느 주문이 공격이냐는 이름표(`SpellNames.cpp`)에서 가른 것이고,
    //    **값은 게임이 도는지 보려고 넣은 자리표시**다. 실측이 생기면 갈아 끼운다.
    static int SpellDamage(uint8_t spell);

    // 이 주문이 날리는 것의 **와이어 종류 번호**. 공격이 아니면 0.
    //
    // ⚠⚠ **2026-09-07 정정 — 이건 실측이 아니라 이름 짐작이다.**
    //
    //   처음에 「종류표에서 찾았으니 실측」이라고 적었다. **틀렸다.**
    //   종류표(`/debug/nox/types`)에 `695 Fireball` 이라는 **이름이 있다**는 것과,
    //   게임이 파이어볼을 날릴 때 **그 번호를 쓴다**는 것은 다른 말이다.
    //   표에서 이름을 고른 것은 measurement 가 아니라 matching 이었다.
    //
    //   확인하려 했더니 —
    //     · 09-07 에 파이어볼을 네 번 쐈고 `0x79 TRY_SPELL` 이 **4건 와이어에 갔다.**
    //       그런데 `0xa4` 에 **새 오브젝트가 하나도 안 생겼고 `0xa9` 알림도 0건**이다
    //     · 09-06 덤프에도 695 는 **0건**. 대신 클라 덤프에 **`693 TitanFireball`** 이
    //       네 번 보이는데, 그중 셋은 좌표가 맵 밖(8669,80 · 59691,32)이라 헛맞음이다
    //
    //   → **어느 번호가 맞는지 아직 모른다.** 셋 다 복합이라 짜임은 어긋나지 않지만,
    //     클라가 엉뚱한 그림을 그릴 수 있다. `Docs/UNMEASURED.md` 참조.
    static uint16_t ProjectileType(uint8_t spell);

    // 이 주문이 불러내는 것의 **와이어 종류 번호**. 소환 주문이 아니면 0.
    //
    // ⚠ **이것도 이름 짝짓기지 실측이 아니다** (09-07 정정).
    //   `SpellNames.cpp` 의 `SPELL_SUMMON_*` 과 종류표의 같은 이름을 맞춘 것이다.
    //   **이름이 같다고 게임이 그 번호를 쓰는지는 확인 못 했다** —
    //   투사체 쪽에서 같은 방식으로 고른 `695 Fireball` 이 네 덤프에 0건이었고,
    //   대신 `693 TitanFireball` 이 보였다(위 `ProjectileType` 주석).
    //   **소환 표본이 아예 없어** 여기는 대조할 거리조차 없다.
    //   짝지은 열둘이 전부 COMPLEX 인 것만은 표에서 확인했다 — 짜임은 안 어긋난다.
    static uint16_t SummonType(uint8_t spell);

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

    // ★ 시험용 — 접속 절차를 다 밟지 않고 게임 안 상태를 만든다.
    //
    // 진짜 흐름은 `OnPacket` 이 만든다. 여기는 **죽음·부활을 시험하려고** 둔 좁은 통로다.
    // 이게 없으면 죽음 시험을 하려고 216바이트 접속 묶음까지 흉내내야 한다.
    // 깃발 판을 차린다. 팀은 들어온 차례로 번갈아 준다.
    //
    // ⚠ **팀을 어떻게 나누는지는 실측이 아니다.** 진짜 녹스는 콘솔·메뉴로 고르게 한다.
    void StartCtf(int redX, int redY, int blueX, int blueY);

    // 퀘스트 판을 차린다 — 주인 없는 몬스터를 `count` 마리 세운다.
    // 다 잡으면 판이 끝난다.
    void StartQuest(int count, int centerX, int centerY);
    int QuestLeft() const { return questTotal_ - questKilled_; }
    int QuestKilled() const { return questKilled_; }
    uint16_t CapturesOf(uint8_t team) const { return team < 3 ? caps_[team] : 0; }
    uint8_t TeamOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.team;
    }
    // 이 사람이 깃발을 들고 있나.
    bool Carrying(const std::string& addr) const {
        for (const auto& f : flags_) if (f.carrier == addr) return true;
        return false;
    }
    void DebugSetTeam(const std::string& addr, uint8_t team) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.team = team;
    }

    void DebugEnterGame(const std::string& addr, uint16_t netCode) {
        Session& s = SessionFor(addr);
        s.inGame = true;
        s.netCode = netCode;
        s.maxHealth = kDefaultMaxHealth;
        s.health = s.maxHealth;
        s.x = 1000;
        s.y = 1000;
    }

    // ★ 시험용 — 자리를 옮긴다. 사거리 시험에 쓴다.
    void DebugMoveTo(const std::string& addr, int x, int y) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.x = x;
        it->second.y = y;
    }

    // ★ 피해를 준다. 체력이 0 이하가 되면 죽음을 알리고 부활을 예약한다.
    //
    // 나가는 것은 캡처에서 본 그대로다 —
    //   0x42 REPORT_HEALTH_DELTA  겉에 그냥      (실측 10건 전수가 비신뢰 패킷)
    //   0xE8 PLAYER_DIED          0xcc 껍데기 안  (실측 2건 전수)
    //
    // 돌려주는 값 = 이 피해로 죽었나.
    // `by` 는 때린 쪽의 주소다. 죽으면 그쪽 점수가 오른다.
    // 불·독처럼 임자가 없는 피해면 빈 문자열을 넣는다.
    bool Damage(const std::string& addr, int amount, std::vector<OutPacket>& out,
                const std::string& by = std::string());

    uint16_t KillsOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.kills;
    }
    uint16_t DeathsOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.deaths;
    }
    // 판이 끝났나 (점수 한도에 닿았나).
    bool RoundOver() const { return roundOver_; }

    // 지금 살아 있나. 없는 상대면 거짓.
    bool IsAlive(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.inGame && it->second.respawnAtFrame == 0;
    }
    uint16_t HealthOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.health;
    }

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
    // 쉬는 시간을 지나 실제로 나간 근접 공격 수(누른 횟수가 아니다).
    unsigned long long melees() const { return melees_; }
    // 날아가다 사람에게 맞은 횟수.
    unsigned long long hits() const { return hits_; }
    // 지금 날아가고 있는 것의 수.
    size_t flying() const { return shots_.size(); }
    // 지금 불려 나와 있는 것의 수.
    size_t pets() const { return pets_.size(); }
    // 불려 나온 것이 죽은 횟수.
    unsigned long long petsKilled() const { return petsKilled_; }
    void SetTickMs(uint32_t ms) { tickMs_ = ms; }

    // 읽었지만 아직 답을 안 만드는 오프코드의 집계.
    const std::map<uint8_t, unsigned long long>& seen() const { return seen_; }
    unsigned long long handled() const { return handled_; }
    // 끝까지 못 쪼개 통째로 넘긴 조각 수. 아직 못 읽는 메시지가 섞였다는 뜻이다.
    unsigned long long stopped() const { return stopped_; }
    // **무엇이 막았나** — 길이를 몰라 멈춘 자리의 오프코드별 집계.
    // 이것이 다음에 무엇을 옮겨야 하는지 알려주는 나침반이다.
    const std::map<uint8_t, unsigned long long>& stoppedByOp() const { return stoppedByOp_; }

    // 시전된 주문 — 번호별 횟수. 번호는 opennox `spell` 패키지 ID 와 같다.
    const std::map<uint8_t, unsigned long long>& spells() const { return spells_; }
    // 그중 자기에게 건 것 (마지막 자리가 1 인 것)
    unsigned long long spellsOnSelf() const { return spellsOnSelf_; }

private:
    Session& SessionFor(const std::string& addr);

    // 사거리 안에서 가장 가까운 **다른** 사람. 없으면 빈 문자열.
    std::string NearestOther(const std::string& self, double range) const;

    // ★ 사거리 안에서 가장 가까운 **적** — 사람이든 불려 나온 것이든.
    //
    // `friendOwner` 는 「내 편의 주인」이다. 사람이 때릴 때는 자기 주소,
    // 불려 나온 것이 때릴 때는 부른 사람의 주소를 넣는다. 그러면
    // **자기 자신도, 자기가 부른 것도** 대상에서 빠진다.
    //
    // ⚠ 「부른 사람의 공격에도 소환수가 안 맞는다」는 **근거 없이 정한 것**이다.
    //   덤프에 소환 표본이 아예 없어 진짜 녹스가 어떻게 하는지 모른다.
    //   자기 소환수를 자기가 때려 죽이는 것이 이상해 보여 그렇게 뒀을 뿐이다.
    Target NearestTarget(const std::string& friendOwner, double x, double y,
                         double range) const;

    // 고른 대상을 깎는다. 사람이면 Damage(), 불려 나온 것이면 그쪽 체력을 깎고
    // 0 이 되면 없애며 `0x31 DESTROY_OBJECT` 를 뿌린다.
    void HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                    const std::string& by = std::string());

    // 판 시계를 뿌린다 — `0xd3 TIMER_STATUS`. 짜임은 실측이다(PROTOCOL.md 「판 시계」).
    void SendTimer(std::vector<OutPacket>& out);

    // 깃발 알림 하나 — `0xa9 INFORM` 의 갈래.
    //
    // ⚠ 속 짜임은 **opennox 원본을 옮긴 것**이지 와이어로 대조한 것이 아니다 —
    //   `InformTeamPlayer`(msg_inform.go:253) 가 `PlayerID 4 + TeamID 4`, 본문 8.
    //   **깃발 갈래(5·6·7)는 우리 덤프에 0건**이라 맞춰 볼 거리가 없었다.
    void SendFlagInform(InformCode code, uint16_t playerNetCode, uint8_t team,
                        std::vector<OutPacket>& out);

    // 글자 한 줄을 모두에게 — `0xa8 TEXT_MESSAGE`. 짜임은 실측이다.
    //
    // ⚠ **문구는 우리가 지은 것**이다. 진짜 녹스는 시스템 문구를 `0xa9` 갈래 15 로
    //   **CSF 열쇠**만 보내고 클라가 `nox.csf` 에서 찾는다(PROTOCOL.md 「알림」 절).
    //   퀘스트 완료에 해당하는 열쇠를 모르므로, **아는 통로인 채팅**으로 대신 알린다.
    void SendNotice(const char* text, std::vector<OutPacket>& out);

    // 깃발을 들고 있으면 그 자리에 떨어뜨린다 (죽었을 때).
    void DropFlag(const std::string& who, std::vector<OutPacket>& out);

    // 깃발 줍기·득점을 살핀다. 틱마다 부른다.
    void UpdateFlags(std::vector<OutPacket>& out);

    // 불려 나온 것 하나를 없애고 모두에게 알린다.
    void KillPet(size_t idx, std::vector<OutPacket>& out);

    // `0xcc SEQ_IMPORTANT` 껍데기에 싸서 내보낸다.
    //
    // ★ 신뢰 전송은 **패킷 머리 비트가 아니라 이 껍데기**가 맡는다 — 실측이 그렇다.
    //   09-06 덤프에서 `0xcc` 581건이 **전수 비신뢰 패킷**에 실려 있었다.
    //   (머리의 신뢰 비트는 접속 절차에서만 쓴다)
    void EmitSeq(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);

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
    std::vector<uint8_t> useMapBody_;
    std::vector<std::vector<uint8_t>> postMap_;
    std::vector<uint8_t> firstSnapshot_;
    std::vector<std::vector<uint8_t>> mirror_;
    size_t mirrorAt_ = 0;   // 그 다음 41바이트 USE_MAP
    uint32_t frame_ = 0;
    uint16_t netCode_ = 0;
    uint8_t xorKey_ = 0x70;
    uint32_t nextConnectId_ = 1;
    bool inboundPlain_ = false;
    uint32_t tickMs_ = 33;      // 30Hz 목표
    uint32_t lastTickMs_ = 0;
    bool tickStarted_ = false;
    unsigned long long moves_ = 0;
    unsigned long long melees_ = 0;
    unsigned long long hits_ = 0;
    unsigned long long petsKilled_ = 0;
    bool roundOver_ = false;
    uint32_t nextTimerFrame_ = 0;
    std::vector<Flag> flags_;
    uint16_t caps_[3] = {0, 0, 0};   // 팀별 득점 (1·2 만 쓴다)
    int questTotal_ = 0;
    int questKilled_ = 0;
    std::vector<Projectile> shots_;
    std::vector<Creature> pets_;
    // 날아가는 것에 매길 번호.
    // ⚠ **진짜 서버가 번호를 어떻게 매기는지는 모른다.** 캡처에서 사람이 715·720·728 을
    //   받은 것만 봤다. 사람과 안 부딪히게 넉넉히 띄운 자리에서 시작할 뿐이다.
    uint16_t nextShotNetCode_ = 4096;
    std::map<uint8_t, unsigned long long> spells_;
    unsigned long long spellsOnSelf_ = 0;
    std::vector<AcceptScript> script_;   // 접속 대본
    size_t scriptAt_ = 0;
};

// 플레이어의 와이어 종류 번호. 실측으로 확인했다 —
// 상태 갱신 30,169건이 이 번호였고, thing.bin 712번 `NewPlayer`(복합)와 맞는다.
constexpr uint16_t kPlayerType = 713;

// 한 걸음에 얼마나 미나. **2026-09-06 캡처로 실측했다** (전에는 지어낸 12.0 이었다).
//
//   표본 866 (바로 다음 틱, 프레임 차 1 짜리만)
//   가장 잦은 값 7 — 271회, 31.3%
//
// dx·dy 를 그대로 보면 길이는 일정한데 방향만 흩어진다. 잦은 것부터 —
//
//   (5,-5) 7.07 · (7,-2) 7.28 · (6,-4) 7.21 · (6,-3) 6.71 · (7,-3) 7.62
//
// 축만 딱 떨어지는 값이 아니라 6.3~7.6 에 퍼져 있는데, 이는 방향을 한 바퀴 256으로
// 나눠 쓰고 좌표를 정수로 자르기 때문이다. 아래 계산이 바로 그 짜임이라 값이 맞는다.
//
// ⚠ **느린 무리가 따로 있다** — 2.8~4.2 에 330건이 몰려 있고(빠른 쪽은 360건),
//    걷기와 달리기로 갈리는 것으로 보이나 아직 가리지 못했다. 지금은 잦은 쪽을 쓴다.
// ⚠ 클라의 예측과 맞는지는 **정품 클라로 눈으로 봐야** 안다. 아직 안 했다.
constexpr double kStepPerTick = 7.0;

// ★ 주문 시전(0x79)의 본문 길이. **2026-09-06 캡처 표본 83개 전수로 확정했다.**
//
//     79 │ 주문번호 1 │ 0 으로 채운 19 │ 대상 플래그 1
//
// 판정 = 22바이트(오프코드 포함) 뒤 자리가 83/83 모두 아는 오프코드였다
// (전부 0x3F PLAYER_INPUT). 다른 길이로 잘라 보면 78% 아래로 떨어진다.
constexpr size_t kTrySpellBodyLen = 21;

// 파이어볼의 주문 번호. opennox `spell` 패키지의 `SPELL_FIREBALL = ID(27)` 과 같다 —
// 와이어에 실려 온 번호가 그 표와 그대로 맞는 것을 캡처에서 확인했다.
constexpr uint8_t kSpellFireball = 27;


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
