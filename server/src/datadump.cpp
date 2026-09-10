// datadump.cpp — 게임 데이터 파일을 복호해 **속을 내놓는다.** P6 기획데이터 전수검사용.
//
// 왜 있나 — 데미지·사거리·부활시간 같은 「게임의 숫자」가 우리 서버에서 자리표시다.
// 그 값이 **파일에 있는지 없는지**를 가르려면 먼저 파일 속을 봐야 한다.
// 복호는 이미 검증된 NoxCrypt 를 그대로 쓴다(왕복 시험 + thing.bin 2,874종 전수 대조).
//
// ★ 판독 방식은 **꼴에 기대지 않는다** — 복호본에서 인쇄 가능한 글자 줄기를 전부 뽑는다.
//   monster.bin·modifier.bin·gamedata.bin 은 파서가 없어 꼴을 모르는데, 그 셋이야말로
//   이번 검사의 목표다. 「모르는 꼴이라 못 본다」가 되면 검사가 아니다.
//
// 쓰는 법
//   nox_datadump <파일> [--key N] [--out <경로>] [--strings] [--keys]
//                       [--near <글자>] [--span N] [--min N] [--hex N]
//
//     --out      복호본을 그대로 쓴다 (다른 도구로 훑을 때)
//     --strings  글자 줄기를 자리와 함께 전부 찍는다
//     --keys     "KEY = VALUE" 꼴 속성의 **키 이름을 전수 집계**한다 (많은 순)
//     --near     그 글자가 든 줄기 둘레(--span, 기본 400바이트)의 줄기를 찍는다
//     --min      줄기로 칠 최소 길이 (기본 4)
//     --hex      앞 N 바이트를 16진으로
//
//   키 번호는 파일 이름에서 정한다(NoxCrypt.h 의 표) — --key 로 덮어쓸 수 있다.
//
// ⚠ 원본 파일은 **읽기만** 한다.
#include "nox/NoxCrypt.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// 파일 이름으로 키 번호를 고른다. 모르면 -1.
int KeyForName(const std::string& path) {
    std::string n = path;
    const size_t sl = n.find_last_of("\\/");
    if (sl != std::string::npos) n = n.substr(sl + 1);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "thing.bin")    return nox::kKeyThingBin;
    if (n == "gamedata.bin") return nox::kKeyGameData;
    if (n == "modifier.bin") return nox::kKeyModifier;
    if (n == "monster.bin")  return nox::kKeyMonster;
    if (n == "soundset.bin") return nox::kKeySoundSet;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".map") == 0) return nox::kKeyMap;
    return -1;
}

struct Run { size_t off; std::string text; };

// 인쇄 가능한 글자 줄기를 뽑는다. 탭·줄바꿈은 끊는 것으로 본다(속성은 한 줄짜리다).
std::vector<Run> Strings(const std::vector<uint8_t>& b, size_t minLen) {
    std::vector<Run> out;
    std::string cur;
    size_t start = 0;
    for (size_t i = 0; i <= b.size(); ++i) {
        const int c = (i < b.size()) ? b[i] : -1;
        const bool ok = (c >= 0x20 && c <= 0x7e);
        if (ok) {
            if (cur.empty()) start = i;
            cur.push_back(static_cast<char>(c));
        } else {
            if (cur.size() >= minLen) out.push_back({ start, cur });
            cur.clear();
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("쓰는 법: nox_datadump <파일> [--key N] [--out 경로] [--strings] [--keys]\n"
                    "                        [--near 글자] [--span N] [--min N] [--hex N]\n");
        return 2;
    }
    const std::string path = argv[1];
    int key = -1;
    std::string outPath, near;
    bool wantStrings = false, wantKeys = false;
    size_t minLen = 4, span = 400, hexN = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--key" && i + 1 < argc)          key = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc)     outPath = argv[++i];
        else if (a == "--near" && i + 1 < argc)    near = argv[++i];
        else if (a == "--span" && i + 1 < argc)    span = (size_t)std::atoi(argv[++i]);
        else if (a == "--min" && i + 1 < argc)     minLen = (size_t)std::atoi(argv[++i]);
        else if (a == "--hex" && i + 1 < argc)     hexN = (size_t)std::atoi(argv[++i]);
        else if (a == "--strings")                 wantStrings = true;
        else if (a == "--keys")                    wantKeys = true;
        else { std::printf("모르는 인자: %s\n", a.c_str()); return 2; }
    }
    if (key < 0) key = KeyForName(path);
    if (key < 0) {
        std::printf("키 번호를 모른다 — --key 로 줄 것 (%s)\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> b = nox::DecryptFile(path, key);
    if (b.empty()) {
        std::printf("복호 실패: %s (키 %d)\n", path.c_str(), key);
        return 1;
    }
    std::printf("[파일] %s — 키 %d · 복호 %zu바이트\n", path.c_str(), key, b.size());

    if (!outPath.empty()) {
        FILE* f = std::fopen(outPath.c_str(), "wb");
        if (!f) { std::printf("못 씀: %s\n", outPath.c_str()); return 1; }
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
        std::printf("[씀] %s\n", outPath.c_str());
    }
    if (hexN) {
        const size_t n = (hexN < b.size()) ? hexN : b.size();
        for (size_t i = 0; i < n; i += 16) {
            std::printf("%08zx ", i);
            for (size_t j = i; j < i + 16 && j < n; ++j) std::printf("%02x ", b[j]);
            std::printf(" |");
            for (size_t j = i; j < i + 16 && j < n; ++j)
                std::printf("%c", (b[j] >= 0x20 && b[j] <= 0x7e) ? b[j] : '.');
            std::printf("|\n");
        }
    }

    const std::vector<Run> runs = Strings(b, minLen);
    std::printf("[줄기] %zu개 (최소 길이 %zu)\n", runs.size(), minLen);

    if (wantKeys) {
        // "KEY = VALUE" 의 KEY 를 센다. 값이 숫자인 것도 따로 센다 —
        // **이번 검사에서 찾는 것은 「숫자를 담은 키」**다.
        std::map<std::string, size_t> all, numeric;
        for (const Run& r : runs) {
            const size_t eq = r.text.find(" = ");
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = r.text.substr(0, eq);
            // 키 앞에 붙은 공백·쓰레기 글자를 턴다
            while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
            if (k.empty()) continue;
            ++all[k];
            const std::string v = r.text.substr(eq + 3);
            bool num = !v.empty();
            for (char c : v) {
                if (!(std::isdigit((unsigned char)c) || c == '-' || c == '.' || c == ' ')) { num = false; break; }
            }
            if (num) ++numeric[k];
        }
        std::vector<std::pair<std::string, size_t>> v(all.begin(), all.end());
        std::sort(v.begin(), v.end(),
                  [](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b2) {
                      return a.second > b2.second;
                  });
        std::printf("[키 집계] %zu종 — (숫자값 건수/전체)\n", v.size());
        for (const auto& kv : v)
            std::printf("  %-28s %6zu  (숫자 %zu)\n", kv.first.c_str(), kv.second,
                        numeric.count(kv.first) ? numeric[kv.first] : 0);
    }

    if (!near.empty()) {
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].text.find(near) == std::string::npos) continue;
            const size_t lo = (runs[i].off > span) ? runs[i].off - span : 0;
            const size_t hi = runs[i].off + span;
            std::printf("--- %s @%08zx ---\n", runs[i].text.c_str(), runs[i].off);
            for (const Run& r : runs)
                if (r.off >= lo && r.off <= hi) std::printf("  %08zx  %s\n", r.off, r.text.c_str());
        }
    }

    if (wantStrings)
        for (const Run& r : runs) std::printf("%08zx  %s\n", r.off, r.text.c_str());
    return 0;
}
