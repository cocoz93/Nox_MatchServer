// Gamedata.cpp — gamedata.bin 읽기(opennox libs balance/balance.go readGamedata 그대로)
#include "nox/Gamedata.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "nox/NoxCrypt.h"

namespace nox {

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// bytes.TrimSpace + bytes.Trim("\x00")
std::string TrimLine(const std::string& s) {
    size_t a = 0, b = s.size();
    auto sp = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (a < b && sp(s[a])) ++a;
    while (b > a && sp(s[b - 1])) --b;
    while (a < b && s[a] == '\0') ++a;
    while (b > a && s[b - 1] == '\0') --b;
    return s.substr(a, b - a);
}

}  // namespace

bool Gamedata::Load(const std::string& path) {
    const std::vector<uint8_t> buf = DecryptFile(path, kKeyGameData);
    if (buf.empty()) {
        error_ = "복호 실패 또는 빈 파일: " + path;
        return false;
    }
    return Parse(std::string(buf.begin(), buf.end()));
}

bool Gamedata::Parse(const std::string& text) {
    global_.clear();
    tags_.clear();
    error_.clear();
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string raw = TrimLine(lines[i]);
        if (raw.empty() || raw[0] == '#') continue;
        const size_t eq = raw.find('=');
        if (eq == std::string::npos || eq == 0) {
            error_ = "잘못된 줄: " + raw;
            return false;
        }
        const std::string key = TrimLine(raw.substr(0, eq));
        std::string val = TrimLine(raw.substr(eq + 1));
        std::vector<std::string> fields;
        {
            std::istringstream ks(key);
            std::string f;
            while (ks >> f) fields.push_back(f);
        }
        if (fields.empty() || fields.size() > 2) {
            error_ = "잘못된 이름: " + raw;
            return false;
        }
        std::string tag, name;
        if (fields.size() == 1) name = fields[0];
        else { tag = fields[0]; name = fields[1]; }
        name = Lower(name);
        // `;` 로 끝날 때까지 다음 줄을 잇는다 · 사이에 `#` 줄이 오면 `;` 를 붙이고 끝
        while (val.empty() || val.back() != ';') {
            if (i + 1 >= lines.size()) break;
            ++i;
            const std::string next = TrimLine(lines[i]);
            if (!next.empty() && next[0] == '#') { val.push_back(';'); break; }
            val.push_back(' ');
            val += next;
        }
        if (val.empty() || val.back() != ';') {
            error_ = "값 끝에 ; 없음: " + key;
            return false;
        }
        val.pop_back();
        std::vector<double> out;
        std::string tok;
        auto flush = [&]() -> bool {
            if (tok.empty()) return true;
            if (tok.back() == 'f') tok.pop_back();
            char* end = nullptr;
            const double d = std::strtod(tok.c_str(), &end);
            if (tok.empty() || end == nullptr || *end != '\0') {
                error_ = "숫자가 아님: " + tok + " (" + key + ")";
                return false;
            }
            out.push_back(d);
            tok.clear();
            return true;
        };
        for (char c : val) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
                if (!flush()) return false;
            } else {
                tok.push_back(c);
            }
        }
        if (!flush()) return false;
        Config& dst = tag.empty() ? global_ : tags_[tag];
        dst[name] = out;
    }
    return true;
}

const std::vector<double>* Gamedata::Find(const std::string& tag, const std::string& key) const {
    const std::string k = Lower(key);
    auto t = tags_.find(tag);
    if (t != tags_.end()) {
        auto v = t->second.find(k);
        if (v != t->second.end()) return &v->second;
    }
    auto g = global_.find(k);
    return g != global_.end() ? &g->second : nullptr;
}

double Gamedata::Float(const std::string& tag, const std::string& key, double def) const {
    const std::vector<double>* v = Find(tag, key);
    if (!v || v->empty()) return def;
    return (*v)[0];
}

std::vector<double> Gamedata::Array(const std::string& tag, const std::string& key) const {
    const std::vector<double>* v = Find(tag, key);
    return v ? *v : std::vector<double>();
}

size_t Gamedata::Count() const {
    size_t n = global_.size();
    for (const auto& kv : tags_) n += kv.second.size();
    return n;
}

}  // namespace nox
