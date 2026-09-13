// Gamedata.h — gamedata.bin(기획 상수)을 서버가 직접 읽는다. 묶음 4-a4.
//
// 정답지: opennox libs `balance/balance.go` readGamedata · opennox `server/gamedata.go` serverBalance
//   · 파일 = Blowfish 키 8 · 줄마다 `[SOLO|ARENA] 이름 = 값[, 값…];` · `#` 줄은 설명
//   · 값이 `;` 로 안 끝나면 다음 줄을 이어 붙인다(사이에 `#` 줄이 끼면 거기서 끝난 것으로)
//   · 숫자 끝 `f` 는 뗀다 · 값 하나면 Float, 여럿이면 Array · 이름은 소문자로
//   · 찾기: 태그 값이 먼저, 없으면 전역 값
//   · 서버 태그 = 협동(coop) 이면 SOLO, 아니면 ARENA(serverBalance.Tag)
#pragma once

#include <map>
#include <string>
#include <vector>

namespace nox {

class Gamedata {
public:
    // 파일을 읽는다. 실패면 false — Error() 에 까닭.
    bool Load(const std::string& path);
    // 복호된 본문을 바로 넣는다(시험용).
    bool Parse(const std::string& text);

    // Float(tag, key) — 없으면 def. 배열이면 첫 값(balance.File.Float)
    double Float(const std::string& tag, const std::string& key, double def = 0.0) const;
    // Array(tag, key) — 없으면 빈 배열. 값 하나면 한 칸
    std::vector<double> Array(const std::string& tag, const std::string& key) const;
    bool Has(const std::string& tag, const std::string& key) const { return Find(tag, key) != nullptr; }
    size_t Count() const;
    const std::string& Error() const { return error_; }

private:
    using Config = std::map<std::string, std::vector<double>>;
    const std::vector<double>* Find(const std::string& tag, const std::string& key) const;
    Config global_;
    std::map<std::string, Config> tags_;
    std::string error_;
};

}  // namespace nox
