// Alias.h — 오브젝트를 1바이트로 부르는 장치. **이 프로토콜의 심장이다.**
//
// 상태 갱신에서 한 오브젝트의 이동이 3바이트로 간다(별칭 1 + dx·dy 2).
// 처음 보는 것만 9바이트다. 그게 되는 이유가 이 표다.
//
// 근거 = src/legacy/GAME5_2.c 의 `nox_xxx_cliGenerateAlias_57B9A0`.
//
//   칸 하나 — 번호(2) 종류(2) 만료프레임(4). **1~254 번만 쓴다**(0·255 예약).
//   배정 — **번호의 하위 바이트에서 시작**해 선형으로 훑는다.
//     · 같은 (번호, 종류) 칸이 있으면 그 자리를 다시 쓴다
//     · 아니면 빈 칸이나 **임대가 끝난 칸**을 뺏는다
//     · 한 바퀴 돌아 제자리면 실패
//   만료 — 등록할 때 **지금 프레임 + 60**. 30Hz 면 2초다.
//
// ★★ **서버도 똑같은 규칙으로 배정해야 한다.** 클라가 자기 쪽에서 같은 계산으로
//    표를 만들기 때문에, 한 칸만 어긋나면 그때부터 엉뚱한 것을 그린다.
//
// ★ 임대라서 시야를 벗어난 오브젝트의 자리는 저절로 회수된다 — 관심영역과 잘 맞는다.
#pragma once

#include <cstdint>
#include <cstring>

namespace nox {

constexpr int kAliasMin = 1;
constexpr int kAliasMax = 254;   // 255 는 「전체 번호로 보낸다」는 표시라 못 쓴다
constexpr uint32_t kAliasTTL = 60;   // 프레임. 30Hz 기준 2초

// ★ 11-13 — 클라가 `a5` 에 적어 준 기한(클라 프레임)을 **서버 프레임으로 옮긴다.**
//   서버는 클라에 프레임의 아래 16비트만 보낸다(`27 TIMESTAMP` · Server.cpp 「① 시각」).
//   클라는 그 16비트에 제 윗자리를 붙여 프레임을 만든다(`network_client.go:188-211`) — 그래서
//   클라 프레임은 **아래 16비트만 서버와 같고 윗자리는 다를 수 있다**(서버가 오래 돌아 65536 을 넘었을 때).
//   기한을 그대로 적으면 그 차이만큼 이미 지난 값이 되어 별칭을 한 번도 못 쓰고,
//   매 틱 전체 번호로 다시 소개하게 된다 — 09-10 사고(FindByObject 주석)의 까닭 후보.
//   그래서 아래 16비트 차이(±32768 프레임 ≈ ±18분)만 서버 프레임에 더한다.
//   `0xFFFFFFFF` 는 머리(자기 카메라) 별칭이라 기한이 없다(`GAME2_3.c:1526-1531`).
//   11-13 자동 클라 2분 실측(Server.h `aliasLeadMin_`) — 독립 모드(`--standalone`)는 클라 프레임 앞섬 0~0 ·
//   긴 이름 102번에 `a5` 102번(갱신당 0.033). 녹음 스냅샷 모드(`wirecmp` 기본)는 스냅샷 속 `27`(녹음 프레임 19,233)을
//   클라가 받아 앞섬 ~18,000 이 되고, 그보다 작은 우리 `27` 은 버린다(`network_client.go:212`) — 그래도 옮겨 적으면
//   양쪽 기한이 같은 값(19,293)이 되어 어긋나지 않는다.
inline uint32_t ClientDeadlineToServer(uint32_t clientDeadline, uint32_t serverFrame) {
    if (clientDeadline == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    const int d = static_cast<int16_t>(static_cast<uint16_t>(clientDeadline - serverFrame));
    if (d < 0 && static_cast<uint32_t>(-d) > serverFrame) return 0;   // 서버 프레임 0 밑 — 이미 지났다
    return serverFrame + static_cast<uint32_t>(d);                     // d < 0 이면 부호 없는 덧셈이 빼기가 된다
}

struct AliasSlot {
    uint16_t netCode = 0;
    uint16_t type = 0;
    uint32_t deadline = 0;
    bool used = false;
};

// ★ 11-15 최적화 4차 개선 — **번호의 아래 바이트로 칸을 묶어 둔다**(`head_` · `next_`).
//   `FindByObject` 는 받는 사람 × 보이는 것마다 부르는데, 전에는 칸 254개를 1번부터 훑었다 —
//   192방 M8 에서 이것을 부르는 `FillEntry` 가 게임 스레드의 13.4%(이 함수는 표에 따로 안 잡혀 몫은 그 안 일부).
//   같은 번호의 칸은 모두 아래 바이트가 같으니 그 묶음 하나에 다 들고, 묶음 안은 **칸 번호 오름차순**이라
//   묶음을 앞에서부터 보면 「1번부터 훑어 처음 맞는 칸」과 같은 칸이 나온다. 쓰지 않는 칸은 어느 묶음에도 없다
//   (전에도 `used` 가 거짓이면 안 맞았다). 묶음을 바꾸는 곳은 `Set` · `Clear` 뿐 — 칸을 쓰는 길이 그 둘뿐이라서다.
class AliasTable {
public:
    void Clear() {
        for (auto& s : slots_) s = AliasSlot{};
        std::memset(head_, 0, sizeof(head_));
        std::memset(next_, 0, sizeof(next_));
    }

    // 클라와 똑같은 규칙으로 자리를 고른다. 못 잡으면 -1.
    int Generate(uint16_t netCode, uint16_t type, uint32_t frame) const {
        int start = static_cast<int>(static_cast<uint8_t>(netCode));
        if (start == 255 || start == 0) start = kAliasMin;
        int v = start;
        for (;;) {
            const AliasSlot& s = slots_[v];
            if (s.netCode == netCode && s.type == type) return v;   // 이미 이 오브젝트 자리
            if (!s.used || s.deadline < frame) return v;            // 비었거나 임대가 끝났다
            ++v;
            if (v == 255) v = kAliasMin;
            if (v == start) return -1;                              // 한 바퀴 돌았다
        }
    }

    void Set(int alias, uint16_t netCode, uint16_t type, uint32_t deadline) {
        if (alias < kAliasMin || alias > kAliasMax) return;
        AliasSlot& s = slots_[alias];
        if (s.used) Unlink(alias, static_cast<uint8_t>(s.netCode));   // 옛 번호의 묶음에서 뺀다
        s = AliasSlot{netCode, type, deadline, true};
        Link(alias, static_cast<uint8_t>(netCode));
    }

    // 자리를 잡고 그 자리에 등록까지 한다. 서버가 내보낼 때 쓰는 길.
    int Assign(uint16_t netCode, uint16_t type, uint32_t frame) {
        const int a = Generate(netCode, type, frame);
        if (a < 0) return -1;
        Set(a, netCode, type, frame + kAliasTTL);
        return a;
    }

    // ★★ **클라가 알려 준 별칭을 오브젝트로 되찾는다 — 클라가 적어 준 기한 안에서만, 늘리지 않고.**
    //
    //   ★★ 11-13 정정 — 쓸 때마다 임대를 밀던 것(아래 09-10)을 뺐다.
    //   원본 서버는 클라가 적어 준 기한을 그대로 두고 살아 있을 때만 쓴다(`sdecode.c:278-279` 적기 ·
    //   `network_lists.go:119-132` `sub_57B930` 쓰기 — 늘리는 줄이 없다). 클라도 별칭 항목을 받을 때
    //   기한을 안 늘린다(`GAME2_3.c:1634-1639`). 서버만 밀면 클라 쪽 칸은 2초에 끝나 새 물체에 넘어가는데
    //   서버는 옛 뜻으로 그 별칭을 보낸다 → 종류가 달라 덧붙이 길이가 어긋나 클라가 그 뒤를 못 읽는다
    //   (11-13 첫째 묶음 256방 — 더미 decodeFail 판마다 5~6 · 세 판 버림으로 묶음 멈춤).
    //   기한이 지나면 전체 번호로 다시 소개하고, 클라는 그걸 받을 때마다 같은 (번호,종류) 칸으로
    //   `a5` 를 다시 보낸다(`GAME2_3.c:1624-1632` · `GAME5_2.c:1621-1631`).
    //   ⚠ 아래 09-10 기록의 「클라가 a5 를 또 안 보낸다」는 원본 클라 코드와 어긋나고, 같은 주석의
    //     09-09 기록(매 틱 재소개에 a5 3,073 — 재소개마다 a5 가 왔다)과도 어긋난다. 그때 죽은 까닭은 미상 —
    //     후보는 클라 기한과 서버 프레임의 윗자리 어긋남(`ClientDeadlineToServer` 주석 · 이제 옮겨 적는다).
    //     11-13 독립 모드 자동 클라 2분에서는 긴 이름 102번에 `a5` 102번 — 같은 (번호,종류)여도 다시 보냈다.
    //
    //   왜 필요한가 — 별칭은 클라가 굴리고 `0xa5 NEW_ALIAS` 로 서버에 알려 준다.
    //   서버가 같은 규칙으로 따라 굴리게 해 뒀지만 실제로는 어긋났다 —
    //   별칭으로 보내면 **아무도 안 그려졌다**(2026-09-09 사람 눈 판정). 그래서 매 틱
    //   전체번호로 다시 소개했고, 그러니 클라가 초당 서른 번 오브젝트를 새로 등록해
    //   1분쯤에 끊겼다(`a5` 3,073 대 갱신 2,129 · 정품은 갱신당 0.03).
    //   **따라 굴리지 말고 클라가 말해 준 것을 그대로 쓴다.**
    //
    //   ★★ [11-13 에 뺀 옛 설계 — 기록으로 둔다] 임대 연장(2026-09-10 ao2 실측) — 연장 없이는 **2초 만에 별칭을 버리고
    //   전체번호 재소개로 돌아간다**(별칭 사용 60틱 → 그 뒤 소개 1,482건). 클라는
    //   같은 (번호,종류)가 계속 보이면 **같은 칸을 다시 쓰고 `a5` 를 또 보내지 않으니**
    //   서버는 영영 별칭을 못 되찾고, 매 틱 21개 재소개에 클라가 ~20초 만에 죽었다.
    //   서버가 그 별칭으로 계속 보내는 동안은 클라 쪽 칸도 살아 있다 — 쓸 때마다 민다.
    //   ⚠ 임대가 **이미** 지난 자리는 여전히 안 쓴다 — 시야 밖으로 나가 한동안 안 보낸
    //     물체의 칸은 클라가 다른 것에 내줬을 수 있다.
    //   ★ 11-15 최적화 4차 개선 — 칸 254개 대신 이 번호의 묶음만 오름차순으로 본다(클래스 머리 주석). 돌려주는 칸은 전과 같다.
    int FindByObject(uint16_t netCode, uint16_t type, uint32_t frame) {
        for (int a = head_[static_cast<uint8_t>(netCode)]; a != 0; a = next_[a]) {
            const AliasSlot& s = slots_[a];
            if (s.netCode == netCode && s.type == type && s.deadline >= frame)
                return a;   // ★ 11-13 — 기한을 늘리지 않는다(위 정정)
        }
        return -1;
    }

    bool Lookup(int alias, uint16_t& netCode, uint16_t& type) const {
        if (alias < kAliasMin || alias > kAliasMax) return false;
        const AliasSlot& s = slots_[alias];
        if (!s.used) return false;
        netCode = s.netCode;
        type = s.type;
        return true;
    }

    // ⚠ **시야에서 빠졌다고 칸을 잊지 말 것** (2026-09-13 — 넣었다가 되돌림).
    //
    //   정답지 서버(`network_lists.go:119` `sub_57B930`)는 사라짐을 보내도 **표를 안 건드린다** —
    //   (번호,종류)가 맞고 임대가 살아 있으면 그 별칭, 아니면 255(전체 번호)일 뿐이다.
    //   클라도 `OBJECT_OUT_OF_SIGHT` 에 스프라이트만 지우고 칸은 둔다(cdecode.c case 50).
    //   ★ 여기서 칸을 잊으면 **FindByObject 주석의 사고가 재현된다** — 전체 번호로 다시 소개해도
    //     클라는 옛 칸을 재사용하고 `a5` 를 안 보내 서버가 영영 별칭을 못 되찾고, 매 틱 재소개로 죽는다.

private:
    // 묶음에 넣는다 — 칸 번호가 커지는 차례를 지킨다(묶음엔 아래 바이트가 같은 칸만 들어 앞에서부터 자리를 찾는다)
    void Link(int alias, uint8_t bucket) {
        uint8_t* p = &head_[bucket];
        while (*p != 0 && *p < alias) p = &next_[*p];
        next_[alias] = *p;
        *p = static_cast<uint8_t>(alias);
    }
    void Unlink(int alias, uint8_t bucket) {
        uint8_t* p = &head_[bucket];
        while (*p != 0 && *p != alias) p = &next_[*p];
        if (*p == 0) return;   // 없다 — Set · Clear 만 묶음을 바꾸니 안 생긴다
        *p = next_[alias];
        next_[alias] = 0;
    }

    AliasSlot slots_[256];
    uint8_t head_[256] = {};   // 번호 아래 바이트 → 그 묶음의 첫 칸(0 = 없음 · 칸 0 은 안 쓴다)
    uint8_t next_[256] = {};   // 칸 → 같은 묶음의 다음 칸(0 = 끝)
};

}  // namespace nox
