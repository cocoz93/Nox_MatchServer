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
#include <set>
#include <string>
#include <vector>

#include "nox/Alias.h"
#include "nox/GameBalance.h"   // 직업·레벨·정본 수치
#include "nox/Messages.h"   // InformCode
#include "nox/UpdateStream.h"
#include "nox/Wire.h"

namespace nox {

// 나갈 조각 하나.
// 물건을 쓰면 무슨 일이 나는가. `thing.bin` 의 `USE = <다루는이름> <숫자>` 에서 온다.
//
// ⚠ **숫자를 회복량으로 읽어도 되는 갈래는 둘뿐이다** — `PotionUse`(물약)·`ConsumeUse`(음식).
//   `WandUse 20` 의 20 은 회복이 아니라 완드 충전이고, `ReadUse`(책 70종)는 숫자가 없다.
//   그래서 「모르는 갈래」를 None 으로 두고 **아무 일도 안 일어나게** 한다 — 지어내지 않는다.
enum class UseKind : uint8_t { None = 0, Health, Mana };

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
    // ★★ 이 사람에게서 마지막으로 무언가를 받은 프레임. **끊긴 세션을 걷어내는 근거다.**
    //   지우는 자리가 아예 없어서 클라를 죽이면 세션이 그대로 남았고, 시험할 때마다
    //   「사람 3」 같은 유령이 끼어 판정이 흐려졌다(2026-09-09).
    uint32_t lastSeenFrame = 0;

    // 게임에 들어와 있는가 — 여기가 참이면 매 틱 상태를 받는다.
    bool inGame = false;
    // ★ 첫 세계 스냅샷을 몇 틱 뒤에 보낼지. 접속 직후 같은 묶음으로 보내면 클라가 못 쓴다
    //   (정품도 다른 조각 여덟 개가 지난 뒤 아홉째로 보낸다 — 2026-09-07 이분법).
    //   ⚠ 이 주석만 있고 **필드가 없었다.** 그래서 접속 처리에서 곧바로 보냈고 화면이 검었다.
    //     정품 조각 순서를 다시 재 보면(2026-09-08): `2b`(USE_MAP) 뒤로 `27 27 27 08 04` 가
    //     지나간 **여섯째**에 스냅샷이 온다. 그 사이가 클라가 맵을 읽는 시간이다.
    //   남은 틱 수. 0 이 되는 틱에 보내고 -1 로 잠근다(-1 = 보낼 것 없음 또는 이미 보냄).
    int snapshotIn = -1;
    // ★★ 이 사람에게 상태 갱신을 한 번이라도 보냈나. **첫 갱신만 머리를 전체번호(ff)로**
    //   보내고 그 뒤로는 별칭 `01` 한 바이트로 보낸다(정품 실측, 2026-09-08).
    //   ⚠ 앞서는 이 판정을 `preludeAt > 0`(서두 재생 진행도)으로 했는데, 재생을 안 쓰면
    //     늘 0 이라 **매 틱 사람을 처음 보는 것처럼 다시 소개**하고 있었다.
    bool sentFirstUpdate = false;
    // ★★ 접속 직후 클라는 **관전(observer)** 이다 — HUD(주문 5개·체력/마나 막대)가 없고
    //   화면에 자기 몸이 반투명하게만 보인다. 정품 서버는 **첫 왼클릭(입력 코드 6)** 에
    //   `6a REPORT_CLIENT_STATUS` 를 신뢰로 보내 관전을 푼다(2026-09-09 정품 대조:
    //   00:31:14 에 `cc(6a cb 02 00 00 00 00)` + `cc(ea 59 05 00 00)` 한 쌍).
    //   내 서버는 그 클릭을 근접 공격으로만 읽어 관전이 영영 안 풀렸다.
    bool observer = true;
    // 아직 서로 소개(`2d NEW_PLAYER`)를 안 주고받았다. 발송은 `Tick` 에서 한다 —
    // 접속 답장에 붙이면 정품이 그 자리에 보내는 것과 어긋난다.
    bool pendingIntro = false;
    // ★ 서두를 어디까지 흘렸나. **접속마다 따로** — 전역으로 두면 둘째 클라가 서두를 못 받는다.
    size_t preludeAt = 0;
    // ★★ 이 사람에게 흘려보낸 프레임. **접속마다 따로 센다.**
    //   전역 하나로 두면 늦게 붙은 사람의 시계가 서두(녹음본이라 늘 3219 로 고정)에서
    //   내 현재 프레임으로 크게 건너뛴다. 2026-09-08 두 클라 대조로 짚었다:
    //     첫째 3221 → 2609 (뒤로 612)   → 세계 정상
    //     둘째 3221 → 5199 (앞으로 1978 ≈ 66초) → 화면 검정
    //   별칭표(aliases)도 접속마다 따로이므로, 그 수명을 재는 프레임도 여기 것을 쓴다.
    uint32_t tickFrame = 0;
    bool tickFrameSet = false;
    int x = 0, y = 0;        // 맵 위 자리
    uint8_t facing = 0;      // 바라보는 쪽. 한 바퀴를 256으로 나눈 값

    // ── 체력과 죽음 (2026-09-07) ─────────────────────────
    // 짜임과 값은 캡처에서 얻었다(Docs/PROTOCOL.md 「죽음과 부활 ✅」·「체력」 절).
    uint16_t health = 0;
    uint16_t maxHealth = 0;

    // ── 캐릭터 (2026-09-10 기획데이터 이식) ───────────────
    // 접속 정보 97바이트의 자리 51·52 에서 읽는다(`GameBalance.h` 참조).
    // 체력·마나·힘은 **직업과 레벨이 정한다** — 전에는 셋 다 위저드 값으로 굳어 있었다.
    CharClass charClass = CharClass::Warrior;
    uint8_t   charLevel = kMaxLevel;
    // ★ 클라가 보낸 접속 정보 97바이트 그대로. **남에게 이 사람을 소개할 때** 쓴다
    //   (`2d NEW_PLAYER`). 독립 모드에는 녹음이 없어 이것 말고는 만들 재료가 없다.
    std::vector<uint8_t> info97;
    uint16_t  maxMana   = 0;
    // ★ **지금 마나.** 이 칸이 없어서 주문이 무한이었다 — `maxMana` 는 쓰기만 하고
    //   읽는 데가 없었다. 시전할 때 깎고 `0x45` 로 알린다.
    uint16_t  mana      = 0;

    // ★ **가진 것.** 이 칸이 없어서 주울 수도 입을 수도 없었다(계획서 벽②).
    //   세계에서 집어 든 물건이 여기로 옮겨 온다.
    struct Carried {
        uint16_t netCode = 0;   // 세계에 있을 때 쓰던 번호를 그대로 들고 온다
        uint16_t type = 0;
        bool modifiable = false;
        bool equipped = false;
        // 쓰면 무슨 일이 나는가 — 세계에 있을 때 정해진 것을 그대로 들고 온다.
        UseKind useKind = UseKind::None;
        int useAmount = 0;
    };
    std::vector<Carried> inventory;
    int       strength  = 0;       // 맨손·무기 피해 계산에 쓴다

    // `0x0a CLIENT_CLOSE` 를 받았다 — 다음 틱에 걷어낸다(30초 타임아웃을 안 기다린다).
    bool closing = false;

    // ★ **겨눈 자리** — `0xac MOUSE` 가 나르는 월드 좌표다(⭕ 정품 표본 18,700+).
    //   투사체가 여기를 향해 난다. 겨눔이 없거나 낡았으면 몸 방향으로 되돌아간다.
    //
    // ⚠ **「없음」을 `aimAtFrame == 0` 으로 나타내지 않는다.** 0 은 진짜 프레임 번호라
    //   첫 틱 전에 받은 겨눔이 영영 「없음」으로 읽힌다 — 시험이 그걸 잡았다.
    //   같은 함정을 `closing` 에서도 한 번 밟았다(2026-09-10, 하루에 둘).
    bool hasAim = false;
    int aimX = 0, aimY = 0;
    uint32_t aimAtFrame = 0;
    uint32_t respawnAtFrame = 0;   // 0 이면 살아 있다
    uint16_t seqImpId = 0;         // 0xcc 껍데기의 번호. 실측도 1씩 오른다
    uint32_t nextMeleeFrame = 0;   // 이 프레임 전에는 근접이 안 나간다
    uint8_t team = 0;              // 0 = 무소속 · 1 = 빨강 · 2 = 파랑
    // ★ **접속한 차례.** 팀을 번갈아 나눌 때 쓴다.
    //   ⚠ `sessions_` 를 그냥 돌면 **주소 문자열 사전순**이라 들어온 차례가 아니다 —
    //     포트가 어떻게 뽑히느냐에 따라 A·B 팀이 뒤집혔다(2026-09-11 감사).
    uint32_t joinOrder = 0;
    uint16_t kills = 0;            // 죽인 수 (아레나 점수)
    uint16_t deaths = 0;           // 죽은 수

    // 이 상대가 **자기 쪽에서 굴리고 있을** 별칭 표. 서버가 똑같이 굴려야
    // 어긋나지 않는다(Alias.h 참조).
    AliasTable aliases;

    // 정적 오브젝트를 이 사람에게 **마지막으로 소개(ff)한 프레임** (P4-b).
    //
    // ★★ 소개 스로틀의 근거다 — 스냅샷이 소개한 물체를 갱신이 매 틱 다시 소개하면
    //   (30/s × 20개) 클라 게임 루프가 **첫 프레임에 멎는다**(2026-09-10 ao2~ao4 실측:
    //   입력이 1,499B 로 한 번에 토해지고 침묵·화면 검정). 클라는 스냅샷 소개만으로
    //   별칭을 각 1번 알려 주므로(pa2 실측: 정적 20개 a5 각 ×1) 재소개가 필요 없다 —
    //   별칭이 아직/다시 없을 때만, 물체당 2초에 1번으로 눌러 소개한다.
    std::map<uint16_t, uint32_t> staticIntroAt;

    // 스트림마다 따로 센다. 핑·퐁이 스트림 1 을 쓰는 것이 실측으로 확인됐다.
    StreamSeq seq[128];
};

// 접속할 때 주는 최대 체력 — **이제 직업·레벨이 정한다**(`GameBalance.h`).
//   dd d0 02 4b 00 4b 00   번호 720 · 75/75      (`0xDD REPORT_TOTAL_HEALTH`)
// ★ 그 75 는 「기본값」이 아니라 **위저드의 아레나 최대 체력**이었다
//   (`ARENA WizardMaxHealth = 75`). 「다른 상대는 150」도 풀린다 — 워리어였던 것이다.
//   여기 상수는 직업을 아직 못 읽은 자리(대조 장치 등)의 되돌림 값으로만 남긴다.
constexpr uint16_t kDefaultMaxHealth = 75;

// 죽고 나서 부활까지 ◑ — 30틱 + 15틱 = **45**. `GameBalance.h` kRespawnMinTicks 참조.
// ★ 전에 쓰던 150 은 **부활 직후 무적 시간**이었다. 값은 실재하는데 뜻이 달랐다.
constexpr uint32_t kRespawnDelayTicks = kRespawnMinTicks;

// 날아가는 것이 사라지기 전까지 가는 거리 ⭕ — **투사체 수명 × 속도**다.
//   목표를 안 문 주문은 `ARENA UnTargetedSpellLifetime = 15` 프레임을 산다
//   (gamedata.bin · opennox GAME4_3.c:5787). 한 틱에 12.0 이니 **180 유닛**.
// ★ 전에 쓰던 210 은 「걸음 서른 걸음쯤」으로 지어낸 값이었다.
// ⚠ 이름이 「주문 사거리」지만 실제로 쓰는 곳은 **투사체 비행 한계** 한 자리다.
constexpr double kSpellRange = kProjectileMaxTravel;

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
// ★★ **이제 monster.bin 의 아레나 값을 쓴다**(`GameBalance.h` kMonsters).
//    종류 번호로 찾아 체력·피해·사거리·속도를 그 몬스터 것으로 준다.
//    아래 넷은 **표에 없는 종류의 되돌림 값**으로만 남는다(전에는 이것이 전부였다).
// ⚠ 속도는 thing 눈금(32로 나눠 쓴다) — Imp 120 → 3.75 유닛/프레임.
constexpr double kPetSpeed = 5.0;          // 되돌림 — 한 틱에 다가가는 거리
constexpr double kPetReach = 40.0;         // 되돌림 — 이 안에 들어오면 때린다
constexpr int kPetDamage = 8;              // 되돌림
constexpr uint16_t kPetHealth = 40;        // 되돌림

// ── 아레나 (점수 겨루기) ────────────────────────────────────
//
// ⚠ **점수 한도는 실측이 아니다.** 콘솔의 `set deaths {n}` 으로 서버가 정하는 값이고
//   (→ `Docs/CONSOLE.md`), 우리 캡처는 `Game Type: Arena` 에 한도 없이 돌았다.
//   열 명 죽이면 끝나는 것으로 두었을 뿐이다.
constexpr uint16_t kArenaKillLimit = 10;

// ★ 이 프레임 수만큼 아무것도 안 오면 세션을 걷어낸다. 30Hz 기준 30초. 8초로 뒀더니 **진짜 클라도 걸렸다**(2026-09-09).
//   정품 클라는 조용해도 핑을 계속 보내므로 살아 있으면 걸리지 않는다
//   (실측: 접속당 `0x06 CLIENT_PING` 이 꾸준히 온다).
constexpr uint32_t kSessionTimeoutTicks = 900;

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
// ★ 닿는 거리는 **거리 상수가 아니라 부딪힘**이었다 — 깃발도 사람도
//   `EXTENT = CIRCLE 10`(⭕ thing.bin) 이니 두 반지름의 합 20 이다.
constexpr double kFlagTouchRange = kFlagTouchDist;
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

// ★ 파이어볼 속도 ⭕ — `thing.bin Fireball SPEED = 192` ÷32 후 투사체라 ×2 = **12.0**.
//   전에 쓰던 26 은 「걸음보다 서너 배」로 둔 자리표시였다.
constexpr double kProjectileSpeed = kFireballPerFrame;
// ★ 맞는 반경 — 파이어볼은 점(`EXTENT = CENTER`)이고 대신 **광역**이다.
//   광역 안쪽 15 는 고정, 바깥쪽은 세기÷3. 여기는 「닿았나」를 가리는 값이라
//   광역 안쪽(15)에 사람 반지름(10)을 더해 둔다.
constexpr double kProjectileHitRadius = kFireballSplashInner + kPlayerRadius;   // 25

// ★★ 한 틱에 미는 거리가 판정 반경보다 **크면 표적을 뛰어넘는다.**
//   26 > 24 라 틱 사이로 지나가 버렸다(2026-09-08: 시전 12건에 피해 1건).
//   지금은 12 < 25 라 한 조각으로도 안 넘지만, 쪼개면 판정이 더 촘촘해진다.
//   ⚠ 속도나 반경을 만지면 **이 값도 같이 볼 것.** 지켜야 하는 조건은
//     `kProjectileSpeed / kProjectileSubSteps < kProjectileHitRadius` 다.
constexpr int kProjectileSubSteps = 4;

// 근접 — **맨손이 기본**(정품 아레나는 무기를 안 준다). 피해는 힘이 정하므로
// 상수가 아니다(`GameBalance.h` UnarmedDamage). 여기 둘은 사거리와 간격.
// ⚠ 공격 간격은 **파일에도 코드에도 없는 유일한 값**이다 — 몬스터는 25~35 프레임으로
//   적혀 있는데 사람 쪽만 없다. 공격 동작 길이가 정하는 것으로 보인다. 15 는 자리표시.
constexpr double kMeleeRange = kUnarmedRange;
constexpr uint32_t kMeleeCooldownTicks = 15;   // ⚠ 자리표시 — 30Hz 기준 0.5초

// 첫 세계 스냅샷을 접속 뒤 몇 틱 미룰지. 정품 조각 순서에서 잰 값이다 —
// `2b`(USE_MAP) 와 스냅샷 사이에 다섯 조각(`27 27 27 08 04`)이 지나간다.
constexpr int kSnapshotDelayTicks = 5;

// 벽 격자 한 칸의 월드 크기와 몸 반지름 (독립 서버 P3).
// ★ 둘 다 **닫혔다**(2026-09-10). 칸 23 은 실클라 돌진 3표본이 정합했고,
//   재구현 코드도 칸 크기로 23 을 나눈다(◑). 반지름은 thing.bin 에서 나왔다.
constexpr double kWallCell = 23.0;
// ★ 몸 반지름 ⭕ — `thing.bin NewPlayer EXTENT = CIRCLE 10`. 전에는 짐작한 12 였다.
constexpr double kBodyRadius = kPlayerRadius;

// 시야 반경 (P4-b). ⚠ 화면 반경보다 조금 넓게 잡은 자리표시 — 실측 아님.
constexpr double kAoiRadius = 700.0;
// 갱신 하나에 싣는 정적 오브젝트 상한 (P4-b). EncodeUpdate 는 512B 를 넘치면
// **0 을 돌려줘 갱신이 통째로 멎는다** — 정적이 많은 자리에서 사람 이동까지 같이
// 멈추는 사고를 막으려면 수를 잘라야 한다. 가까운 순으로 담아 화면에 보이는 것부터.
constexpr size_t kAoiMaxPerUpdate = 20;
// 시야에서 **빼는** 반경. 들일 때보다 넓다 — 경계에 서 있으면 같은 것이 매 틱
// 들어왔다 나갔다를 되풀이해(플래핑) 별칭과 조각을 헛되이 태운다. 20% 여유를 둔다.
// ⚠ 이 1.2 배는 실측이 아니라 우리 값이다.
constexpr double kAoiDropRadius = kAoiRadius * 1.2;

// 물건에 손이 닿는 거리. ➖ **우리 값이다** — 파일에도 코드에도 근거가 없다.
// 몸 반지름(10)과 벽 한 칸(23)을 놓고 「한 칸 안」으로 잡았다.
// 정품은 물건을 밟고 서서 줍는 꼴이라 이보다 짧을 수도 있다.
constexpr double kPickupReach = kWallCell;

// 한 사람이 들 수 있는 물건 수의 **안전 한도**. 진짜 한도는 서버가 아니라 **클라가 쥔다** —
// 못 받으면 `0xF1 INVENTORY_FAIL` 로 되돌려 주므로 서버가 미리 자를 일이 아니다.
// 이 값은 규칙이 아니라 폭주 방지용 울타리다.
constexpr size_t kInventoryGuard = 64;

// 채팅 한 줄에 실을 수 있는 글자 수. 크기 칸이 **1바이트**라 255가 천장이고,
// 정품 표본의 최대가 250(`fa 00 00`)이었다. 그보다 길면 버린다.
constexpr size_t kChatMaxBytes = 250;

// 겨눈 자리가 이보다 오래되면 안 쓴다 — 몸 바라보는 쪽으로 되돌아간다.
// ➖ **우리 값이다.** 정품이 낡은 겨눔을 어떻게 다루는지는 모른다.
// 30Hz 기준 1초. 클라는 마우스가 움직일 때마다 보내므로(표본 18,700+) 짧아도 충분하다.
constexpr uint32_t kAimStaleTicks = 30;

// 판이 끝나고 다음 판을 열기까지. ➖ **우리 값이다** — 정품이 몇 초 뒤에 새 판을
// 여는지는 표본이 없다. 승자 화면을 볼 시간은 줘야 하므로 30Hz 기준 8초로 둔다.
// ★ 이것이 없어서 **판이 한 번 끝나면 그 프로세스에서 다시 못 열었다**(2026-09-11 감사).
constexpr uint32_t kRoundRestartTicks = 240;

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
    // ★★ 스냅샷을 넣으면 **스폰 자리도 여기서 정해진다.** 정의는 Server.cpp.
    //   서두가 찍힌 그 맵의 그 자리라야 사람이 세계 오브젝트들과 같은 동네에 선다.
    void SetFirstSnapshot(const std::vector<uint8_t>& v);
    // ★ 진단용 — 켜면 매 틱 「내가 만든 것」 대신 **정품이 보냈던 조각**을 순서대로 내보낸다.
    void SetMirror(const std::vector<std::vector<uint8_t>>& v) {
        mirror_ = v;
        mirrorAt_ = 0;
        // ★★ 서두는 **녹음본이라 프레임이 고정**이다(늘 3219 에서 시작한다).
        //   그러니 접속 묶음이 말하는 프레임도 거기에 맞춰야 시계가 한 줄로 이어진다.
        //   안 맞추면 늦게 붙은 사람은 접속 때 받은 프레임이 서두보다 앞서서,
        //   그 뒤 갱신이 전부 **과거로 보여 버려진다** — 화면이 검다(2026-09-08).
        preludeFirstFrame_ = 0;
        for (const std::vector<uint8_t>& m : mirror_) {
            if (m.size() >= 3 && m[0] == 0x27) {   // 0x27 TIMESTAMP
                preludeFirstFrame_ = static_cast<uint32_t>(m[1]) |
                                     (static_cast<uint32_t>(m[2]) << 8);
                break;
            }
        }
    }
    bool MirrorOn() const { return !mirror_.empty(); }
    // ★ 진단용 — 끄면 매 틱 상태 갱신(0xa4)을 **안 보낸다.**
    //   왜 필요한가 : 정품(opennox)은 클라가 둘 붙어도 갱신을 **한 번도 안 보내는데**
    //   화면에 자기 캐릭터가 이름표까지 나온다(2026-09-08 대조). 내 서버는 갱신을
    //   매 틱 보내는데 아무것도 안 그려진다. 「내 갱신이 오히려 방해하나」를 가르는 자리다.
    void SetSendUpdates(bool v) { sendUpdates_ = v; }
    // ★ 첫 세계 스냅샷의 사람 항목을 이 사람 것으로 갈아 끼울지. 끄면 **녹음본 그대로** 나간다.
    //   「몸이 안 그려지는 게 갈아끼우기 탓인가」를 가르는 시험 손잡이다(--raw-snapshot).
    void SetPatchSnapshot(bool v) { patchSnapshot_ = v; }
    // ★ 진단용 — 끄면 **새로 들어온 사람의 장비(`cc(4c)`+`cc(60)`)를 먼저 와 있던
    //   사람들에게 안 알린다.** 이걸 켠 채로 정품 클라 둘을 붙이면 **둘째가 들어오는
    //   그 순간 첫째가 죽는다**(2026-09-08 실측: A 마지막 송신 23:44:26.942 ·
    //   B 접속 23:44:26.617 · 그 사이에 A 로 간 82바이트가 이 알림이다).
    //   가르는 자리 = 죽음이 **이 알림 탓인가, 사람 둘 실린 상태 갱신 탓인가**.
    void SetNotifyNewGear(bool v) { notifyNewGear_ = v; }
    // ★★★ 서두(녹음본) 안에 든 `cc(6a <번호2> 01 …)` 의 상태를 **0 으로 뒤집는다. 기본이 켜짐.**
    //
    //   `01` 은 상태 bit0 = **GUI 를 안 그린다**(오픈녹스 `nox_client_onClientStatusA`:
    //   `v&1` 이면 renderGUI=false). 녹음이 그 상태에서 떠진 것이라, 서두를 그대로
    //   흘리면 **접속하자마자 「HUD 숨겨라」가 나간다.** 남의 번호를 갈아 끼우듯
    //   이 값도 이 사람 것으로 고쳐야 하는 자리였다(2026-09-09, 서두 182번 자리).
    //
    //   실측 — 이 한 자리를 뒤집으니 화면 색이 **985 → 1335** 로 뛰고
    //   주문 세트·체력/마나 막대·초상·나침반이 한꺼번에 떴다. 그전에 세 번
    //   헛짚었다(`6a` 만 보내기 · `2d` 상태 0 보내기 · 둘의 순서 바꾸기).
    //   `--hud-off` 로 끄면 녹음본 값을 그대로 쓴다(대조용).
    void SetForceHudOn(bool v) { forceHudOn_ = v; }
    // ★ 새 사람이 들어올 때 먼저 와 있던 사람들에게 넘길 오프코드 묶음(`--peer-ops`).
    //   기본 `4c,60`. 「상대 몸이 안 그려진다」를 다시 컴파일 없이 쓸어보는 자리다.
    void SetPeerOps(const std::set<uint8_t>& v) { peerOps_ = v; }
    // ★ 스폰 간격(--spawn-spread). 크게 주면 「가려서 안 보이는 것」과
    //   「안 그려지는 것」이 갈린다. 단 45 를 넘기면 근접이 안 닿는다.
    void SetSpawnSpread(int v) { spawnSpread_ = v; }

    // ── 벽 충돌 (독립 서버 P3) ─────────────────────────────
    //
    // 맵의 벽 격자를 받아 걸음을 막는다. 격자 한 칸 = kWallCell 유닛.
    // ⚠ 배율 23 은 **녹스 격자 상수**(256칸×23≈5888)로 둔 값 — 실클라 벽 돌진(P3-b)으로
    //   검증하기 전까지는 짐작이다. wallOffX/Y 보정도 같은 자리에서 확인한다.
    void SetWalls(const std::vector<std::pair<uint8_t, uint8_t>>& cells) {
        walls_.assign(256 * 256, 0);
        for (const auto& c : cells) walls_[c.second * 256 + c.first] = 1;
        wallsOn_ = !cells.empty();
    }
    bool WallAt(double x, double y) const {
        if (!wallsOn_) return false;
        const int cx = static_cast<int>(x / kWallCell);
        const int cy = static_cast<int>(y / kWallCell);
        if (cx < 0 || cx > 255 || cy < 0 || cy > 255) return true;   // 맵 밖 = 벽
        return walls_[cy * 256 + cx] != 0;
    }
    // 이 자리에 설 수 있나 — 몸 반지름의 네 모서리가 벽 칸을 안 밟아야 한다.
    // ── 세계 정적 오브젝트 (독립 서버 P4-a) ────────
    //
    // 맵 파일에서 읽은 것들. 아직 갱신에 실지는 않는다(P4-b 에서 AOI 로) —
    // 지금은 스냅샷 조립(P5-c)과 AOI 의 바닥이다.
    struct WorldStatic {
        uint16_t netCode = 0;   // 서버가 붙인 번호 (맵 extent 그대로 쓰지 않는다 — 충돌 여지)
        uint16_t type = 0;      // 와이어 종류 번호
        int x = 0, y = 0;
        // 수식(마법부여)이 붙는 물건인가 — 무기·방어구·완드·깃발.
        // 주울 때 `4b`(5B) 대신 **`4c`(9B, 수식 4바이트)** 로 알려야 한다.
        // ⭕ `4c` 는 정품 표본 10건으로 짜임 확인(`4c cc 02 6d 04 ff bf ff ff`).
        // `main.cpp` 가 `thing.bin` 의 CLASS 로 정한다.
        bool modifiable = false;
        // ★ 쓰면 무슨 일이 나는가 — `thing.bin` 의 `USE = PotionUse 50` 에서 온다.
        //   ⭕ 값이 파일에 있다. 지어낸 숫자가 아니다.
        UseKind useKind = UseKind::None;
        int useAmount = 0;
    };
    void SetWorldStatics(std::vector<WorldStatic> v) { statics_ = std::move(v); }
    void SetAoi(bool on) { aoiOn_ = on; }

    // ── 독립 모드 (P5-c-2) ──────────────────────
    //
    // 녹음본(joinBody_·useMapBody_·firstSnapshot_) 없이 접속을 받는다.
    //   접속 묶음 = BuildJoinBundle(클라 97B 되돌려줌)
    //   USE_MAP    = BuildUseMap(아래 메타)
    //   스냅샷      = 27 + a4(현 세계) + BuildSnapshotTail(순번은 RenumberSeq 가 다시)
    // ⚠ 기본 꺼짐 — 실클라 검증(P5-c 졸업) 전에는 녹음 경로가 정본이다.
    void SetStandalone(const std::string& mapFileName, uint32_t mapCrc) {
        standalone_ = true;
        saMapName_ = mapFileName;
        saMapCrc_ = mapCrc;
    }
    size_t WorldStaticCount() const { return statics_.size(); }

    bool CanStand(double x, double y, double r) const {
        if (!wallsOn_) return true;
        return !WallAt(x - r, y) && !WallAt(x + r, y) && !WallAt(x, y - r) && !WallAt(x, y + r);
    }

    // ★ 남 항목 덧붙이 값(--peer-pose HH). 자기 것은 늘 0x40 이고 그건 그려진다.
    void SetPeerPose(uint8_t v) { peerPose_ = v; }
    // ★ 갱신 항목 순서 뒤집기(--peer-first). 가르는 시험용.
    void SetPeerFirst(bool v) { peerFirst_ = v; }
    // ★ 상대 몫 첫 스냅샷 통째 보내기(--peer-snapshot).
    void SetPeerSnapshot(bool v) { peerSnapshot_ = v; }
    // ★ 남의 종류 번호(--peer-type N). 0 이면 사람. 가르는 시험용.
    void SetPeerType(uint16_t v) { peerType_ = v; }
    // ★ 옛 동작(매 틱 전체번호 소개)으로 되돌리는 대조용 손잡이(--intro-every-tick).
    void SetIntroEveryTick(bool v) { introEveryTick_ = v; }
    // 클라에게서 받은 별칭 알림 수. 틱 로그에 찍어 「받고 있나」를 바로 본다.
    unsigned long long aliasHeard() const { return aliasHeard_; }

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

    // 팀을 접속한 차례로 번갈아 나눈다. 사람이 들어올 때마다 다시 부른다.
    // ⚠ `sessions_` 순회는 **주소 사전순**이라 들어온 차례가 아니다 — `joinOrder` 로 정렬한다.
    void AssignTeams();

    // 퀘스트 판을 차린다 — 주인 없는 몬스터를 `count` 마리 세운다.
    // 다 잡으면 판이 끝난다.
    //   ★ `spread` — 가운데에서 이만큼 벌려 세운다. 기본은 실측 아닌 자리표시 300.
    //     실클라로 돌릴 때는 **근접 사거리 안**(45)에 세워야 걷지 않고도 시험이 된다.
    void StartQuest(int count, int centerX, int centerY, double spread = kQuestSpread);

    // 서두(녹음본)에서 읽어 낸 스폰 자리. 모드를 차릴 때 기준으로 쓴다.
    int spawnX() const { return spawnX_; }
    int spawnY() const { return spawnY_; }

    // ★ 시험용 — **스폰 자리를 아이템 자리로** 옮긴다. 없으면 false.
    //
    //   「어떤 조작이 줍기를 보내나」를 재려면 물건 위에 서 있어야 하는데 걷기가 자꾸 어긋난다.
    //
    // ⚠⚠ **반대로 하면 안 된다** — 아이템을 옮기면 **클라와 어긋난다.**
    //   클라는 맵을 읽어 **자기가 그 자리에 아이템을 만든다**(`main.cpp` 「번호 = 맵 색인+75」).
    //   서버가 좌표만 바꿔 보내면 클라 화면에는 옛 자리에 그대로 있고 발밑엔 아무것도 없다.
    //   2026-09-10 스윕 2·3차가 그래서 헛돌았다 — 왼클릭은 먹었는데(클릭 +1)
    //   **주울 물건이 화면에 없어서** 공격만 나갔다.
    bool DebugSpawnAtStatic() {
        if (statics_.empty()) return false;
        spawnX_ = statics_[0].x;
        spawnY_ = statics_[0].y;
        return true;
    }
    size_t FlagCount() const { return flags_.size(); }

    // ★ 지금 누가 어디 있나 — **하네스가 걸어가게 하려면 이게 있어야 한다.**
    //   정품 클라는 자기 자리를 밖으로 안 알려 준다(오픈녹스만 디버그 API 가 있다).
    //   서버는 이미 다 알고 있으니 로그로 내보내고, 하네스가 그걸 읽어 각을 잰다.
    //   짜임 — `사람 <번호>=<x>,<y>,<방향>` · `깃발 <팀>=<x>,<y>` · `몬스터 <번호>=<x>,<y>`
    std::string PositionsLine() const;
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

    // ★ `observer` 를 켜면 **막 접속한 상태**(관전)로 만든다. 기본은 이미 놀고 있는 상태다 —
    //   시험 대부분은 싸움을 재는 것이라 관전 단계를 매번 거치게 하면 잡음만 는다.
    //   관전 풀기 자체는 `TestObserverExit` 에서 따로 잰다.
    // ★ 시험용 접속. 직업을 고를 수 있다 — **스탯이 직업으로 갈리므로**
    //   기본은 위저드로 둔다(옛 시험이 기대하던 체력 75 가 위저드 값이었다).
    void DebugEnterGame(const std::string& addr, uint16_t netCode, bool observer = false,
                        CharClass cls = CharClass::Wizard) {
        Session& s = SessionFor(addr);
        // ⚠ 진짜 접속 경로(`CLIENT_ACCEPT`)와 **같은 일을 해야** 한다 —
        //   `joinOrder` 를 여기서 안 매기면 팀이 사전순으로 갈린다(시험이 잡았다).
        if (!s.inGame) s.joinOrder = ++nextJoinOrder_;
        s.inGame = true;
        s.netCode = netCode;
        s.charClass = cls;
        s.charLevel = kMaxLevel;
        s.maxHealth = static_cast<uint16_t>(HealthOnline(cls));
        s.maxMana = static_cast<uint16_t>(ManaOnline(cls));
        s.strength = StrengthOnline(cls);
        s.health = s.maxHealth;
        s.mana = s.maxMana;
        s.x = 1000;
        s.y = 1000;
        s.observer = observer;
    }

    // ★ 시험용 — 첫 세계 스냅샷을 **다음 틱에** 이 사람에게 보내도록 예약한다.
    void DebugArmSnapshot(const std::string& addr) {
        auto it = sessions_.find(addr);
        if (it != sessions_.end()) it->second.snapshotIn = 0;
    }

    // ★ 시험용 — 지금 자리. 벽 충돌 시험이 이걸로 잰다.
    int XOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.x;
    }
    int YOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.y;
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
    uint16_t ManaOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.mana;
    }
    size_t CarriedCount(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.inventory.size();
    }
    bool Carries(const std::string& addr, uint16_t netCode) const {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return false;
        for (const auto& c : it->second.inventory)
            if (c.netCode == netCode) return true;
        return false;
    }

    // ── 주문 마나 비용 ─────────────────────────────────
    //
    // 주문 번호 → 마나. `thing.bin` 의 SPEL 섹션에서 오고 `main.cpp` 가 채운다
    // (`ThingDb::ManaByName(SpellName(번호))`). 값의 근거는 `ThingDb.h SpellRaw` 주석.
    //
    // ⚠⚠ **표가 비면 모든 주문이 공짜가 된다.** 그건 「비용이 0」이 아니라
    //   「표를 못 읽었다」는 뜻이다. 그래서 개수를 밖에서 볼 수 있게 둔다 —
    //   기동 로그가 0 을 찍으면 파일을 못 읽은 것이다.
    void SetSpellCost(uint8_t spell, uint8_t mana) { spellCost_[spell] = mana; }
    size_t SpellCostCount() const { return spellCost_.size(); }
    uint8_t SpellCost(uint8_t spell) const {
        auto it = spellCost_.find(spell);
        return it == spellCost_.end() ? 0 : it->second;
    }
    // 마나가 모자라 튕긴 시전 수 — 「눌렀는데 아무 일도 안 난다」를 로그로 가른다.
    unsigned long long spellsNoMana() const { return spellsNoMana_; }
    // 줍기 — 요청·성공·손이 안 닿음·되돌려 떨굼. 셋을 갈라야 「왜 안 주워지나」가 보인다.
    unsigned long long tryGets() const { return tryGets_; }
    unsigned long long picked() const { return picked_; }
    unsigned long long pickTooFar() const { return pickTooFar_; }
    unsigned long long dropped() const { return dropped_; }
    unsigned long long used() const { return used_; }
    // 쓰긴 했는데 **무슨 일이 나는지 모르는** 갈래(`ReadUse`·`WandUse`…). 지어내지 않은 자리의 수.
    unsigned long long useUnknown() const { return useUnknown_; }
    unsigned long long equipped() const { return equipped_; }
    unsigned long long dequipped() const { return dequipped_; }
    // 클라가 판 시계를 물어본 횟수. 전에는 이 조각을 통째로 버려 **0으로 보였다.**
    unsigned long long timerAsks() const { return timerAsks_; }
    unsigned long long chats() const { return chats_; }
    // 몇 판을 마쳤나. 0 이면 아직 한 판도 안 끝난 것이다.
    unsigned long long roundsPlayed() const { return roundsPlayed_; }
    // 시험용 — 그 사람 팀에 득점을 넣는다(깃발을 실제로 나르지 않고 판끝을 재려고).
    void DebugCapture(const std::string& addr, int n) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end() || it->second.team >= 3) return;
        for (int i = 0; i < n; ++i) {
            ++caps_[it->second.team];
            if (!roundOver_ && caps_[it->second.team] >= kCaptureLimit) {
                roundOver_ = true;
                restartAtFrame_ = frame_ + kRoundRestartTicks;
            }
        }
    }
    // 겨눔을 받은 횟수. 0 이면 `0xac` 를 안 읽는 것이고 투사체가 몸 방향으로만 난다.
    unsigned long long aims() const { return aims_; }
    // 시험용 — 날아가는 것 하나의 자리. 없으면 (0,0).
    std::pair<double, double> DebugShotAt(size_t i) const {
        if (i >= shots_.size()) return { 0.0, 0.0 };
        return { shots_[i].x, shots_[i].y };
    }
    // 시험용 — 겨눈 자리를 직접 넣는다.
    void DebugAimAt(const std::string& addr, int x, int y) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return;
        it->second.aimX = x;
        it->second.aimY = y;
        it->second.aimAtFrame = frame_;
        it->second.hasAim = true;
    }
    // ★ **닿은 피해 전부** — 근접·투사체·소환수를 다 센다.
    //   `hits()` 는 **투사체 명중만** 센다. 둘을 갈라야 「싸움이 안 붙었다」와
    //   「투사체만 안 맞았다」가 구분된다(09-10 오판의 자리).
    unsigned long long hurts() const { return hurts_; }
    bool IsEquipped(const std::string& addr, uint16_t netCode) const {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) return false;
        for (const auto& c : it->second.inventory)
            if (c.netCode == netCode) return c.equipped;
        return false;
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
    // ★ 게임에 들어와 있는 사람 수. **「서로 보임」이 안 될 때 여기부터 본다** —
    //   와이어를 뜯어 세면 내 디코더가 항목을 잘못 쪼개 읽어 헛것을 볼 수 있다(2026-09-08).
    size_t inGameCount() const {
        size_t n = 0;
        for (const auto& kv : sessions_) if (kv.second.inGame) ++n;
        return n;
    }
    // 마지막 틱에 상태 갱신에 실은 사람 수(마지막으로 만든 것 기준).
    size_t lastUpdateObjects() const { return lastUpdateObjects_; }
    // ★ 첫 사람의 `snapshotIn`. 「사람 1(갱신에 0)」이 나올 때 **왜 갱신을 안 만드는지**
    //   가르는 자리다 — 이 값이 0 이상이면 스냅샷을 기다리느라 매 틱 건너뛰고 있다.
    size_t lastLoopSeen() const { return lastLoopSeen_; }
    size_t lastSessions() const { return lastSessions_; }
    int firstSnapshotIn() const {
        for (const auto& kv : sessions_) if (kv.second.inGame) return kv.second.snapshotIn;
        return -99;
    }
    unsigned long long moves() const { return moves_; }
    // 쉬는 시간을 지나 실제로 나간 근접 공격 수(누른 횟수가 아니다).
    unsigned long long melees() const { return melees_; }
    unsigned long long code6Seen() const { return code6Seen_; }

    // 0 이상 n 미만의 난수. **재현 가능한 것으로 둔다** — 시험이 흔들리면 안 된다.
    //   (선형 합동법. 정품과 같은 난수열일 까닭이 없고, 필요한 것은 「가끔 발차기」뿐이다)
    uint32_t NextRandom(uint32_t n) {
        rngState_ = rngState_ * 1664525u + 1013904223u;
        return n ? (rngState_ >> 16) % n : 0;
    }
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

    // ── 마지막 조각을 어떻게 읽었나 (진단) ─────────────────
    //
    // ★ 왜 필요한가 — 「메시지 2개, 보낸 조각 0개」가 나오는데 **그 둘이 무엇인지
    //   알 길이 없었다**(2026-09-10). 조각을 버린 것도 아니고(`stoppedByOp` 0건)
    //   크래시도 아닌데 답만 없는 상태다. 그 자리에서 짐작만 쌓였다.
    // ⚠⚠ 열쇠 상태를 **같이** 봐야 한다 — 복호가 어긋나면 쓰레기 바이트가
    //   **그럴듯한 오프코드로 보인다.** 「메시지 N개」만 보면 속는다.
    const std::vector<uint8_t>& LastFragOps() const { return lastFragOps_; }
    bool IsKeyed(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it != sessions_.end() && it->second.keyed;
    }
    uint8_t KeyOf(const std::string& addr) const {
        auto it = sessions_.find(addr);
        return it == sessions_.end() ? 0 : it->second.xorKey;
    }

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
    // ★ 방어구는 **여기서** 깎는다 — 싸움 피해가 다 이 자리를 지난다(근접·주문·소환수).
    //   `Damage()` 는 그대로 **날 피해**를 적용한다(시험이 정확한 값을 넣는 자리다).
    //   피해 종류별로 방어가 먹는 정도가 다르다 — `GameBalance.h` DamageKind 주석.
    void HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                    const std::string& by = std::string(),
                    DamageKind kind = DamageKind::Physical);

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
    // 판 시계 한 통(13바이트)을 만든다. 주기 발송과 `0xd4` 즉답이 같은 내용을 쓴다.
    void BuildTimerStatus(uint8_t msg[13]) const;

    // 한 사람이 한 말을 모두에게. 머리는 **서버가 새로 짓는다**(남의 번호로 말하기 방지).
    void BroadcastChat(const Session& who, const uint8_t* text, size_t n,
                       std::vector<OutPacket>& out);

    // 이 사람이 지금 겨누는 쪽(라디안). 겨눔이 없거나 낡았으면 몸 바라보는 쪽.
    double AimRadians(const Session& s) const;

    void EmitSeq(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out);
    // `2d NEW_PLAYER` 한 통. 묶음 안 129바이트를 떼어 **번호와 상태만** 갈아 끼운다.
    // 상태는 kNewPlayerStatusAt 주석 참조 — 0 이라야 클라가 HUD 를 그린다.
    void EmitNewPlayer(Session& to, const Session& who, uint8_t status,
                       std::vector<OutPacket>& out);

    // `6a REPORT_CLIENT_STATUS` 한 통 — **한 사람의 상태를 「받는 쪽 한 명」에게 알린다.**
    //
    // ★★★ **이게 남의 몸을 그리게 하는 열쇠다** (2026-09-09 오픈녹스 원본 대조).
    //   클라의 사람 그리기(`drawable_player.go` `DrawPlayer`)는 **자기가 아닌 사람**에 대해
    //   `if pl.Field3680 & 0x1 { return }` — 즉 **1번 비트가 서 있으면 아무것도 안 그린다.**
    //   그 비트를 세우는 곳은 `2d NEW_PLAYER` 인데
    //   (`network_client.go`: `pl.Field3680 |= data[112:]`) **OR 라서 0 을 다시 보내도 안 꺼진다.**
    //   끄는 길은 `6a` 하나뿐이다 — `cdecode.c` case 106 이
    //   `Field3680 &= ~0x423` 한 뒤 `|= 상태 & 0x423` 로 **갈아 끼운다.**
    //   그 자리의 「내 번호냐」 검사는 **HUD 를 켤지에만** 걸리고 상태 갱신은 누구 것이든 한다.
    //
    // ⚠ 전에는 이걸 **본인에게만** 보냈다. 그래서 먼저 들어온 쪽에는 나중 사람의 1번 비트가
    //   영영 서 있었고, 실제로 **한쪽에서만 상대가 보였다**(09-09 사람 눈 판정과 맞는다).
    void EmitClientStatus(Session& to, uint16_t netCode, uint32_t status,
                          std::vector<OutPacket>& out);

    // 위를 **놀고 있는 모두**에게. 관전을 풀 때 쓴다.
    void BroadcastClientStatus(uint16_t netCode, uint32_t status,
                               std::vector<OutPacket>& out);

    // 한 통을 **놀고 있는 모두**에게. `viaSeq` 면 `0xcc` 껍데기에 싸서, 아니면 생으로.
    //
    // ★ 왜 필요한가 — 피해·죽음·부활을 **맞은 본인에게만** 보내고 있었다.
    //   그래서 남이 맞는 것도 죽는 것도 되살아나는 것도 화면에 안 나타났다.
    //   두루 보내는 자리마다 손으로 반복문을 쓰던 것을 이 하나로 모은다.
    //
    // ⚠⚠ **번호가 실린 메시지만 이 길을 쓴다.** `0x43 REPORT_PLAYER_HEALTH` 처럼
    //   누구 것인지 안 실리는 메시지를 태우면 받는 쪽이 **제 것으로 읽어**
    //   남의 체력바가 망가진다. 태워도 되는 것 =
    //   `0x42`·`0xDD`·`0xE8`·`0xE9`·`0x2E`·`0x31`·`0x32` (전부 첫 두 바이트가 번호다).
    void Broadcast(bool viaSeq, const uint8_t* msg, size_t n,
                   std::vector<OutPacket>& out);

    // 세계에서 정적 하나를 뺀다(주웠을 때). 없으면 false.
    bool TakeStatic(uint16_t netCode);

    // 녹음본 조각 안의 `cc` 껍데기 번호를 **이 세션의 이어달기 번호로 다시 매긴다.**
    //
    // ★★★ **이걸 안 하면 스냅샷 뒤에 보낸 신뢰 메시지가 통째로 사라진다.**
    //   클라는 `cc` 번호로 순서를 맞추고 **이미 지나간 번호를 조용히 버린다**
    //   (`GAME2_3.c` `sub_48D5A0` → `sub_48D4F0`: 지금 기다리는 번호보다 만 개 안쪽으로
    //    작으면 버림 · `sub_48D660`: 기다리는 번호와 같은 것만 꺼내 쓰고 하나씩 올림).
    //
    //   내 녹음본은 그 녹음 때의 번호(0 · 3~22)를 그대로 담고 있는데
    //   서버도 0 부터 매기니 **두 줄기가 겹친다.** 2026-09-09 실측 —
    //   나중에 들어온 쪽이 스냅샷 뒤에 받은 `2d`(#3)·`6a`(#4)가 스냅샷의 같은 번호에
    //   먼저 먹혀 **버려졌고**, 그래서 **그쪽 화면에서만 상대가 안 보였다.**
    //   (먼저 온 쪽은 번호가 이미 #41 까지 올라가 있어 멀쩡했다 — 증상이 한쪽만이던 까닭)
    //
    // ⚠ 녹음본의 번호에는 **구멍도 있다**(1·2번이 없다). 그대로 두면 클라가
    //   그 자리에서 **30초**를 기다린다(`sub_48D660` 의 `0x7530`). 다시 매기면 구멍도 없어진다.
    void RenumberSeq(Session& s, std::vector<uint8_t>& frag);

    // 머리 2바이트를 붙여 한 조각을 만든다. keyed 면 XOR 을 건다.
    void Emit(Session& s, bool reliable, uint8_t stream,
              const uint8_t* msg, size_t n, std::vector<OutPacket>& out);

    std::map<std::string, Session> sessions_;
    std::map<uint8_t, unsigned long long> seen_;
    unsigned long long handled_ = 0;   // 답을 만든 메시지 수
    uint32_t nextJoinOrder_ = 0;       // 접속한 차례에 매기는 번호 (팀 나누기용)
    uint32_t restartAtFrame_ = 0;      // 이 프레임에 판을 다시 연다. 0 이면 예약 없음
    unsigned long long roundsPlayed_ = 0;
    unsigned long long stopped_ = 0;   // 끝까지 못 쪼개 통째로 넘긴 조각 수
    std::map<uint8_t, unsigned long long> stoppedByOp_;
    std::vector<uint8_t> lastFragOps_;          // 마지막 조각에서 읽어낸 오프코드들(진단)
    std::map<uint8_t, uint8_t> spellCost_;      // 주문 번호 → 마나 (thing.bin SPEL)
    unsigned long long spellsNoMana_ = 0;       // 마나가 모자라 튕긴 시전
    unsigned long long tryGets_ = 0;            // `0x73` 을 받은 횟수
    unsigned long long picked_ = 0;             // 실제로 집어 든 것
    unsigned long long pickTooFar_ = 0;         // 손이 안 닿아 못 집은 것
    unsigned long long dropped_ = 0;            // 클라가 되돌려 도로 떨군 것
    unsigned long long used_ = 0;               // 실제로 효과가 난 쓰기
    unsigned long long useUnknown_ = 0;         // 갈래를 몰라 아무 일도 안 난 쓰기
    unsigned long long equipped_ = 0;
    unsigned long long dequipped_ = 0;
    unsigned long long timerAsks_ = 0;          // `0xd4` 를 받은 횟수
    unsigned long long chats_ = 0;              // 되돌려 방송한 채팅 수
    unsigned long long aims_ = 0;               // `0xac` 로 받은 겨눔 수
    unsigned long long hurts_ = 0;              // 닿은 피해 전부 (근접+투사체+소환수)

    std::vector<uint8_t> infoBody_;
    std::vector<uint8_t> joinBody_;     // 접속 직후 나가는 216바이트 묶음
    std::vector<uint8_t> useMapBody_;
    std::vector<std::vector<uint8_t>> postMap_;
    std::vector<uint8_t> firstSnapshot_;
    std::vector<std::vector<uint8_t>> mirror_;
    // 서두의 첫 TIMESTAMP 프레임. 0 이면 서두에 시각이 안 실린 것이다.
    uint32_t preludeFirstFrame_ = 0;
    bool sendUpdates_ = true;
    bool patchSnapshot_ = true;
    bool notifyNewGear_ = true;
    bool forceHudOn_ = true;
    // 새 사람이 들어왔을 때 **먼저 와 있던 사람들에게 넘길** 오프코드.
    // 기본은 장비 둘 — 더 넣으면 나빠진 적이 있어(2026-09-08) 손잡이로 뺐다.
    std::set<uint8_t> peerOps_{0x4c, 0x60};
    // 스폰 자리를 사람마다 몇 픽셀씩 벌리나. 기본 40 은 근접 사거리(45) 안이다.
    int spawnSpread_ = 40;
    std::vector<WorldStatic> statics_;
    bool aoiOn_ = false;
    bool standalone_ = false;
    std::string saMapName_;
    uint32_t saMapCrc_ = 0;   // P4-c 실클라 검증 전까지 기본 꺼짐   // 맵 정적 오브젝트 (P4)
    std::vector<uint8_t> walls_;   // 256×256 벽 격자 (P3)
    bool wallsOn_ = false;
    // 남 항목의 덧붙이 값(자세로 보인다). 정품 표본은 0x30 인데 그 표본의 상대는
    // 화면 밖이었다 — 그려지는 값이 따로 있을 수 있어 손잡이로 뺐다.
    uint8_t peerPose_ = 0x30;
    // 갱신 항목 순서를 뒤집는다(남 먼저). 「자리 탓인가 누구냐 탓인가」를 가르는 시험용.
    bool peerFirst_ = false;
    // 새 사람이 들어올 때 **상대 몫의 첫 스냅샷을 통째로** 보낸다.
    // 「스프라이트는 스냅샷에서만 만들어진다」 가설을 가르는 자리.
    bool peerSnapshot_ = false;
    // 남을 사람 아닌 종류로 보내 본다(0 이면 사람 그대로). 가르는 시험용.
    uint16_t peerType_ = 0;
    // 사람 둘 이상일 때 매 틱 f 로 다시 소개할지. 기본 꺼짐 — 켜면 클라가
    // 갱신마다 별칭을 새로 만든다(2026-09-09 실측: 정품의 30배).
    // ★★ 기본이 **켜짐**이다. 끄면(별칭으로 보내면) **아무도 안 그려진다**(2026-09-09 사람 눈).
    //   내 별칭표가 클라 것과 어긋나서, 클라가 그 별칭을 엉뚱한 것으로 풀거나 못 푼다.
    //   제대로 고치려면 **클라가 보내오는 `0xa5 NEW_ALIAS` 를 받아 그 번호를 그대로 써야** 한다.
    // ★★ 기본이 **켜짐**이다 — 사람 항목을 매 틱 전체번호로 다시 소개한다.
    //
    //   끄면(클라가 알려 준 별칭으로 보내면) **아무도 안 그려진다**(2026-09-09 사람 눈).
    //   `0xa5 NEW_ALIAS` 를 받아 그 자리를 쓰게 고쳤는데도 그렇다 — 클라가 그 순간
    //   그 오브젝트에 물려 둔 자리와 여전히 어긋나는 것으로 보인다. 미결.
    //
    //   ★ 앞서 「매 틱 소개가 클라를 1분에 무너뜨린다」고 적었는데 **그건 틀렸다.**
    //     진짜 클라 하나 + 가짜 하나로 재니 이 설정으로 **3분 넘게 멀쩡했다**
    //     (별칭받음 8,630). 끊김은 **진짜 클라 둘을 한 PC 에서 돌릴 때** 나는 것이고
    //     갱신 짜임 탓이 아니다.
    bool introEveryTick_ = true;
    // 클라가 알려 준 별칭을 몇 번 받았나. 0 이면 NEW_ALIAS 를 못 받고 있는 것이다.
    unsigned long long aliasHeard_ = 0;
    size_t lastUpdateObjects_ = 0;   // 마지막 갱신에 실은 사람 수
    // ★ 갱신을 만드는 세션 루프가 **실제로 몇 번 돌았나**. 「사람 1(갱신에 0)」이
    //   코드로 설명이 안 돼서 박은 계수기다 — 읽어서 두 번 못 찾았다(2026-09-08).
    size_t lastLoopSeen_ = 0;        // 루프가 본 세션 수(inGame 통과분)
    size_t lastSessions_ = 0;        // 그때의 sessions_.size()
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
    uint32_t rngState_ = 0x13572468u;   // NextRandom 의 씨앗 — 고정이라 시험이 재현된다
    // 진단(2026-09-10 sq1) — 코드6(왼클릭)이 분기에 몇 번 도달했나.
    // 클라는 231번 보냈는데 성사가 15에서 멎었다 — 유실이 파싱이냐 조건이냐를 가른다.
    unsigned long long code6Seen_ = 0;
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
    // 사람 오브젝트 번호. 정품이 713·715 를 쓰는 자리라 그 언저리에서 시작한다.
    uint16_t nextPlayerNetCode_ = 713;
    // ★★ 사람 하나가 쓰는 오브젝트 번호는 **다섯 칸**이다 — 본인 + 장비 넷.
    //   정품 2인 덤프에서 그대로 읽힌다(2026-09-09):
    //     사람 715 → 장비 716·717·718·719 │ 사람 720 → 장비 721·722·723·724
    //   내 서버는 사람만 713·714 로 하나씩 올리고 장비는 **녹음본 번호(716~719)를
    //   둘 다 그대로** 썼다. 같은 번호를 두 사람이 쓰니 클라가 하나만 그렸다 —
    //   「미니맵엔 둘인데 몸은 하나」의 원인이다.
    static constexpr uint16_t kNetCodeStride = 5;
    // ★★ 스폰 자리. **서두(첫 세계 스냅샷)의 머리 좌표에서 읽는다** — 서두가 찍힌 맵의
    //   그 자리다. 상수로 박아 두면 맵이 바뀌는 순간 **사람만 세계에서 멀리 떨어진다**.
    //   실측(2026-09-08): 서두는 estate 인데 스폰은 bludeath 좌표(3864,2534)라
    //   스냅샷 속 다른 오브젝트들이 (2500~3400, 3200~3800)에 있는데 사람만 저쪽에 있었다.
    //   ⚠ 이 파일 안에 「estate 좌표를 bludeath 에 쓰면 맵 밖」이라는 경고가 이미 있었다 —
    //     그 경고가 **반대 방향으로** 실현된 것이다.
    //   서두에서 못 읽으면 아래 기본값(09-05 bludeath 덤프 값)으로 떨어진다.
    int spawnX_ = 3864;
    int spawnY_ = 2534;
    uint16_t nextShotNetCode_ = 4096;
    std::map<uint8_t, unsigned long long> spells_;
    unsigned long long spellsOnSelf_ = 0;
    std::vector<AcceptScript> script_;   // 접속 대본
    size_t scriptAt_ = 0;
};

// 플레이어의 와이어 종류 번호. 실측으로 확인했다 —
// 상태 갱신 30,169건이 이 번호였고, thing.bin 712번 `NewPlayer`(복합)와 맞는다.
constexpr uint16_t kPlayerType = 713;

// 몬스터·소환수의 **대기 애니 번호**. 갱신 항목 덧붙이 한 바이트의 아래 4비트에 실린다.
//
// ★ 애니 번호표가 사람과 몬스터로 갈린다(`draw_debug.go` 의 두 표) —
//     사람   0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · 4 WALK …
//     몬스터 8 IDLE · 9 DIE · 10 DEAD · 11 HURT · 12 MOVE …
//   몬스터에 사람 번호(0)를 실으면 그 자리에 애니가 없어 **아무것도 안 그려진다.**
constexpr uint8_t kCreatureAnimIdle = 8;


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

// ★ 묶음 안 `2d NEW_PLAYER` 가 시작하는 자리와 길이.
//   앞의 다섯(6+5+7+20+49 = 87) 뒤에 129바이트로 붙어 216 으로 딱 떨어진다.
//   사람이 둘 이상일 때 **서로를 소개**하려면 이 조각만 떼어 다시 보내야 한다.
constexpr size_t kJoinNewPlayerAt = 87;
constexpr size_t kJoinNewPlayerLen = 129;

// ★★ `2d NEW_PLAYER` 안 **상태 플래그**(4바이트, LE). 오픈녹스 원본에서 자리를 얻었다:
//   `nox_xxx_netNewPlayerMakePacket_4DDA90` 이 `buf[112] = Field3680 & 0x423` 을 쓴다.
//     bit0 = **HUD 를 안 그린다**(`nox_client_onClientStatusA`: `v&1` → renderGUI=false)
//     bit1 = 관전 중 (`observeClear` 가 이 비트를 보고 카메라를 푼다)
//   내가 서두에서 베껴 쓰던 값이 `01 00 00 00` 이라 **HUD 가 영영 안 떴다**(2026-09-09 실측).
//   정품은 같은 사람에게 `2d` 를 두 번 보낸다 — 처음 `01`, 놀기 시작할 때 `00`.
constexpr size_t kNewPlayerStatusAt = 112;

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
