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

struct AliasSlot {
    uint16_t netCode = 0;
    uint16_t type = 0;
    uint32_t deadline = 0;
    bool used = false;
};

class AliasTable {
public:
    void Clear() {
        for (auto& s : slots_) s = AliasSlot{};
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
        slots_[alias] = AliasSlot{netCode, type, deadline, true};
    }

    // 자리를 잡고 그 자리에 등록까지 한다. 서버가 내보낼 때 쓰는 길.
    int Assign(uint16_t netCode, uint16_t type, uint32_t frame) {
        const int a = Generate(netCode, type, frame);
        if (a < 0) return -1;
        Set(a, netCode, type, frame + kAliasTTL);
        return a;
    }

    // ★★ **클라가 알려 준 별칭을 오브젝트로 되찾는다.**
    //
    //   왜 필요한가 — 별칭은 클라가 굴리고 `0xa5 NEW_ALIAS` 로 서버에 알려 준다.
    //   서버가 같은 규칙으로 따라 굴리게 해 뒀지만 실제로는 어긋났다 —
    //   별칭으로 보내면 **아무도 안 그려졌다**(2026-09-09 사람 눈 판정). 그래서 매 틱
    //   전체번호로 다시 소개했고, 그러니 클라가 초당 서른 번 오브젝트를 새로 등록해
    //   1분쯤에 끊겼다(`a5` 3,073 대 갱신 2,129 · 정품은 갱신당 0.03).
    //   **따라 굴리지 말고 클라가 말해 준 것을 그대로 쓴다.**
    //   ⚠ 임대가 지난 자리는 안 쓴다 — 클라가 그 자리를 다른 것에 내줬을 수 있다.
    int FindByObject(uint16_t netCode, uint16_t type, uint32_t frame) const {
        for (int a = kAliasMin; a <= kAliasMax; ++a) {
            const AliasSlot& s = slots_[a];
            if (s.used && s.netCode == netCode && s.type == type && s.deadline >= frame) return a;
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

private:
    AliasSlot slots_[256];
};

}  // namespace nox
