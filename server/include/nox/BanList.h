// BanList.h — 로비가 쥔 활성 정지 목록(플랜 11-05 6절 「즉시 차단」).
//
// 왜 — 운영 명령(`nox_auth --ban`)은 로비와 다른 프로세스라 로비가 알아챌 길이 테이블뿐이다. 로비가 활성 정지 전체를 5초마다 읽어 이 목록을 통째로 바꾼다.
//   · 전체를 읽는 까닭 — 어디까지 읽었는지 기억할 칸이 필요 없고 풀기 · 로비 재시작도 저절로 맞는다(활성 정지는 몇 행 안 될 것 · 제 판단)
//   · `Add` = 줄 서기 10회째(플랜 7절) — 로비가 바로 아는 정지. 테이블에 쓰기 전에도 막히게 따로 쥐고, 읽은 목록에 나타나면 그쪽으로 넘어간다(그래야 `--unban` 이 먹는다).
//     테이블에 행이 없는 이름은 끝 시각까지 남는다
//   · 소켓 · DB 를 모른다 · 로비 스레드 전용(잠금 없음)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nox::wol {

struct BanEntry {
    std::string name;     // 계정 이름(바이트 그대로 — 로비 이름과 같은 잣대)
    uint32_t until = 0;   // 끝 시각(유닉스 초 · 최댓값 = 영구 `kBanForever`)
};

class CBanList {
public:
    // 읽은 목록으로 바꾼다. 돌려주는 값 = 새로 막을 이름(앞 목록에도 `Add` 에도 없던 것). 지난 `Add` 는 여기서 지운다
    std::vector<std::string> Replace(const std::vector<BanEntry>& rows, uint32_t now);
    void Add(const std::string& name, uint32_t until);
    // 0 = 정지 아님. 끝 시각이 지났으면 다음 읽기 전에도 0
    uint32_t UntilOf(const std::string& name, uint32_t now) const;
    size_t ReadCount() const { return read_.size(); }
    size_t AddedCount() const { return added_.size(); }

private:
    std::map<std::string, uint32_t> read_;
    std::map<std::string, uint32_t> added_;
};

}  // namespace nox::wol
