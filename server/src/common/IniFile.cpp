// IniFile.cpp — 설정 파일 읽기. 규칙은 IniFile.h 머리.
#include "nox/IniFile.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace nox {

static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool CIniFile::LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        _error = "열 수 없음: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return LoadText(ss.str());
}

bool CIniFile::LoadText(const std::string& textIn) {
    _entries.clear();
    _error.clear();
    std::string text = textIn;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);   // 메모장이 붙이는 UTF-8 BOM

    std::string section;
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = Trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (line.back() != ']') {
                _error = std::to_string(lineNo) + "번 줄: 구역 이름이 ] 로 안 끝남";
                return false;
            }
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            _error = std::to_string(lineNo) + "번 줄: = 가 없음";
            return false;
        }
        Entry e;
        e.section = section;
        e.key = Trim(line.substr(0, eq));
        e.keyLower = Lower(e.key);
        e.value = Trim(line.substr(eq + 1));
        if (e.key.empty()) {
            _error = std::to_string(lineNo) + "번 줄: 키가 비었음";
            return false;
        }
        if (Has(section, e.key)) {
            _error = std::to_string(lineNo) + "번 줄: [" + section + "] " + e.key + " 가 두 번 나옴";
            return false;
        }
        _entries.push_back(std::move(e));
    }
    return true;
}

bool CIniFile::Has(const std::string& section, const std::string& key) const {
    const std::string s = Lower(section), k = Lower(key);
    for (const Entry& e : _entries)
        if (e.section == s && e.keyLower == k) return true;
    return false;
}

std::string CIniFile::Get(const std::string& section, const std::string& key, const std::string& def) const {
    const std::string s = Lower(section), k = Lower(key);
    for (const Entry& e : _entries)
        if (e.section == s && e.keyLower == k) return e.value;
    return def;
}

std::vector<std::pair<std::string, std::string>> CIniFile::Section(const std::string& section) const {
    const std::string s = Lower(section);
    std::vector<std::pair<std::string, std::string>> out;
    for (const Entry& e : _entries)
        if (e.section == s) out.emplace_back(e.key, e.value);
    return out;
}

}  // namespace nox
