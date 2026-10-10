// IniFile.h — 설정 파일(INI 꼴) 읽기. 게임 서버와 로비가 같이 쓴다(`nox_common`).
//
//   [section]
//   key = value        ; 또는 # 로 시작하는 줄은 주석
//
// 이름(구역·키)은 대소문자를 가리지 않는다. 값은 앞뒤 공백만 걷고 그대로 둔다.
// 같은 구역의 같은 키가 두 번 나오면 읽기를 실패로 본다 — 어느 값을 쓸지 몰래 고르지 않는다.
// 구역 안의 키 차례는 파일 차례대로 남긴다(로비의 방 목록처럼 차례가 뜻을 가지는 구역이 있다).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace nox {

class CIniFile {
public:
    bool LoadFile(const std::string& path);
    bool LoadText(const std::string& text);
    const std::string& Error() const { return _error; }   // 실패한 까닭(줄 번호 포함)

    bool Has(const std::string& section, const std::string& key) const;
    std::string Get(const std::string& section, const std::string& key, const std::string& def = {}) const;
    // 그 구역의 (키, 값)을 파일 차례대로. 키는 소문자로 바뀌지 않은 원래 글자
    std::vector<std::pair<std::string, std::string>> Section(const std::string& section) const;

private:
    struct Entry {
        std::string section;   // 소문자
        std::string keyLower;
        std::string key;
        std::string value;
    };
    std::vector<Entry> _entries;
    std::string _error;
};

}  // namespace nox
