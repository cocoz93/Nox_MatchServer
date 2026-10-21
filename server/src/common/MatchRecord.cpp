// MatchRecord.cpp — 판 결과 JSON 읽기. 설명은 `MatchRecord.h` 머리 · 쓰개는 `MatchRecorder.cpp` `FormatMatchResultJson`.
#include "nox/MatchRecord.h"

#include <limits>
#include <utility>

namespace nox {

namespace {

// 읽은 값 하나(나무) — 줄 하나(4KB 안)라 통째로 읽고 칸을 찾는다
struct JVal {
    enum class T : uint8_t { Null, Bool, Int, Str, Arr, Obj };
    T t = T::Null;
    bool b = false;
    int64_t i = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
};

constexpr int kMaxDepth = 8;   // 쓰개는 셋(바깥 · players · 사람)까지 — 넉넉히

class JReader {
public:
    explicit JReader(std::string_view s) : s_(s) {}
    // 값 하나 + 꼬리 빈칸뿐이어야 통과
    MatchJsonParse Read(JVal& v) {
        Ws();
        if (!Value(v, 0)) return err_;
        Ws();
        return p_ == s_.size() ? MatchJsonParse::Ok : MatchJsonParse::Syntax;
    }

private:
    bool Fail(MatchJsonParse e) {
        err_ = e;
        return false;
    }
    void Ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }
    bool Lit(const char* w) {
        size_t n = 0;
        while (w[n]) ++n;
        if (s_.substr(p_, n) != std::string_view(w, n)) return false;
        p_ += n;
        return true;
    }
    bool Value(JVal& v, int depth) {
        if (depth > kMaxDepth) return Fail(MatchJsonParse::Syntax);
        if (p_ >= s_.size()) return Fail(MatchJsonParse::Syntax);
        const char c = s_[p_];
        if (c == '{') return Object(v, depth);
        if (c == '[') return Array(v, depth);
        if (c == '"') {
            v.t = JVal::T::Str;
            return String(v.s);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.t = JVal::T::Int;
            return Number(v.i);
        }
        if (Lit("true")) { v.t = JVal::T::Bool; v.b = true; return true; }
        if (Lit("false")) { v.t = JVal::T::Bool; v.b = false; return true; }
        if (Lit("null")) { v.t = JVal::T::Null; return true; }
        return Fail(MatchJsonParse::Syntax);
    }
    bool Object(JVal& v, int depth) {
        v.t = JVal::T::Obj;
        ++p_;   // {
        Ws();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        for (;;) {
            Ws();
            if (p_ >= s_.size() || s_[p_] != '"') return Fail(MatchJsonParse::Syntax);
            std::string key;
            if (!String(key)) return false;
            for (const auto& kv : v.o)
                if (kv.first == key) return Fail(MatchJsonParse::Syntax);   // 같은 열쇠 둘 — 어느 쪽인지 갈리지 않게
            Ws();
            if (p_ >= s_.size() || s_[p_] != ':') return Fail(MatchJsonParse::Syntax);
            ++p_;
            Ws();
            JVal child;
            if (!Value(child, depth + 1)) return false;
            v.o.emplace_back(std::move(key), std::move(child));
            Ws();
            if (p_ >= s_.size()) return Fail(MatchJsonParse::Syntax);
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == '}') { ++p_; return true; }
            return Fail(MatchJsonParse::Syntax);
        }
    }
    bool Array(JVal& v, int depth) {
        v.t = JVal::T::Arr;
        ++p_;   // [
        Ws();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        for (;;) {
            Ws();
            JVal child;
            if (!Value(child, depth + 1)) return false;
            v.a.push_back(std::move(child));
            Ws();
            if (p_ >= s_.size()) return Fail(MatchJsonParse::Syntax);
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == ']') { ++p_; return true; }
            return Fail(MatchJsonParse::Syntax);
        }
    }
    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool String(std::string& out) {
        ++p_;   // "
        out.clear();
        while (p_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[p_++]);
            if (c == '"') return true;
            if (c < 0x20) return Fail(MatchJsonParse::Syntax);   // 날 제어 바이트(쓰개는 \u00XX 로 쓴다)
            if (c != '\\') {
                out += static_cast<char>(c);   // UTF-8 바이트 그대로
                continue;
            }
            if (p_ >= s_.size()) return Fail(MatchJsonParse::Syntax);
            const char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (p_ + 4 > s_.size()) return Fail(MatchJsonParse::Syntax);
                    int v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int h = Hex(s_[p_ + k]);
                        if (h < 0) return Fail(MatchJsonParse::Syntax);
                        v = (v << 4) | h;
                    }
                    p_ += 4;
                    if (v > 0xff) return Fail(MatchJsonParse::Value);   // 쓰개는 바이트만 \u00XX 로 쓴다
                    out += static_cast<char>(v);
                    break;
                }
                default: return Fail(MatchJsonParse::Syntax);
            }
        }
        return Fail(MatchJsonParse::Syntax);   // 닫는 따옴표 없음
    }
    // 정수만(쓰개는 std::to_string) — 앞 0 · 소수 · 지수 · int64 넘음은 문법 틀림
    bool Number(int64_t& out) {
        const bool neg = s_[p_] == '-';
        if (neg) ++p_;
        if (p_ >= s_.size() || s_[p_] < '0' || s_[p_] > '9') return Fail(MatchJsonParse::Syntax);
        if (s_[p_] == '0' && p_ + 1 < s_.size() && s_[p_ + 1] >= '0' && s_[p_ + 1] <= '9') return Fail(MatchJsonParse::Syntax);
        uint64_t v = 0;
        while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') {
            const uint64_t d = static_cast<uint64_t>(s_[p_] - '0');
            if (v > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - d) / 10) return Fail(MatchJsonParse::Syntax);
            v = v * 10 + d;
            ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E')) return Fail(MatchJsonParse::Syntax);
        out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
        return true;
    }

    std::string_view s_;
    size_t p_ = 0;
    MatchJsonParse err_ = MatchJsonParse::Syntax;
};

// 칸 꺼내기 — 처음 걸린 칸 이름을 남긴다
class Fields {
public:
    explicit Fields(std::string* field) : field_(field) {}
    MatchJsonParse err = MatchJsonParse::Ok;

    const JVal* Get(const JVal& obj, const char* key, JVal::T t, bool required = true) {
        if (err != MatchJsonParse::Ok) return nullptr;
        for (const auto& kv : obj.o) {
            if (kv.first != key) continue;
            if (kv.second.t != t) return Set(MatchJsonParse::Type, key);
            return &kv.second;
        }
        return required ? Set(MatchJsonParse::Missing, key) : nullptr;
    }
    bool Has(const JVal& obj, const char* key) const {
        for (const auto& kv : obj.o)
            if (kv.first == key) return true;
        return false;
    }
    void Str(const JVal& obj, const char* key, std::string& out, bool nonEmpty = false) {
        if (const JVal* v = Get(obj, key, JVal::T::Str)) {
            out = v->s;
            if (nonEmpty && out.empty()) Set(MatchJsonParse::Value, key);
        }
    }
    template <class U> void Int(const JVal& obj, const char* key, U& out) {
        if (const JVal* v = Get(obj, key, JVal::T::Int)) {
            if (v->i < static_cast<int64_t>(std::numeric_limits<U>::min()) || v->i > static_cast<int64_t>(std::numeric_limits<U>::max()))
                Set(MatchJsonParse::Value, key);
            else
                out = static_cast<U>(v->i);
        }
    }
    // 글 또는 null — 있음 표시와 같이
    void StrOrNull(const JVal& obj, const char* key, bool& has, std::string& out) {
        if (err != MatchJsonParse::Ok) return;
        for (const auto& kv : obj.o) {
            if (kv.first != key) continue;
            if (kv.second.t == JVal::T::Null) { has = false; return; }
            if (kv.second.t != JVal::T::Str) { Set(MatchJsonParse::Type, key); return; }
            has = true;
            out = kv.second.s;
            return;
        }
        Set(MatchJsonParse::Missing, key);
    }
    std::nullptr_t Set(MatchJsonParse e, const char* key) {
        if (err == MatchJsonParse::Ok) {
            err = e;
            if (field_) *field_ = key;
        }
        return nullptr;
    }

private:
    std::string* field_;
};

}  // namespace

const char* MatchJsonParseName(MatchJsonParse p) {
    switch (p) {
        case MatchJsonParse::Ok: return "통과";
        case MatchJsonParse::Syntax: return "문법";
        case MatchJsonParse::Missing: return "빠진 칸";
        case MatchJsonParse::Type: return "타입";
        case MatchJsonParse::Value: return "값";
        case MatchJsonParse::kCount: break;
    }
    return "?";
}

MatchJsonParse ParseMatchRecordJson(std::string_view json, MatchRecord& out, std::string* field) {
    out = MatchRecord{};
    if (field) field->clear();
    JVal root;
    const MatchJsonParse syn = JReader(json).Read(root);
    if (syn != MatchJsonParse::Ok) return syn;
    Fields f(field);
    if (root.t != JVal::T::Obj) {
        f.Set(MatchJsonParse::Type, "(바깥)");
        return f.err;
    }

    f.Str(root, "match", out.match, /*nonEmpty=*/true);
    f.Int(root, "room", out.room);
    f.Str(root, "roomName", out.roomName);
    f.Str(root, "map", out.map);
    f.Str(root, "mode", out.mode);
    f.Int(root, "start", out.start);
    f.Int(root, "end", out.end);
    f.Int(root, "frames", out.frames);
    f.Int(root, "sec", out.sec);
    f.Str(root, "endReason", out.endReason, /*nonEmpty=*/true);
    f.Int(root, "limit", out.limit);
    f.Int(root, "timeLimitSec", out.timeLimitSec);

    // 승자 — 갈래마다 실리는 칸이 다르다(`FormatMatchResultJson` 의 switch)
    if (const JVal* w = f.Get(root, "winner", JVal::T::Obj)) {
        f.Str(*w, "kind", out.winnerKind);
        if (f.err == MatchJsonParse::Ok) {
            const std::string& k = out.winnerKind;
            if (k == "player" || k == "team") {
                out.hasWinnerId = true;
                f.Int(*w, "id", out.winnerId);
                if (k == "player") f.StrOrNull(*w, "account", out.hasWinnerAccount, out.winnerAccount);
            } else if (k != "none" && k != "draw") {
                f.Set(MatchJsonParse::Value, "winner.kind");
            }
        }
    }

    if (const JVal* ps = f.Get(root, "players", JVal::T::Arr)) {
        for (const JVal& pv : ps->a) {
            if (f.err != MatchJsonParse::Ok) break;
            if (pv.t != JVal::T::Obj) { f.Set(MatchJsonParse::Type, "players[]"); break; }
            MatchRecordPlayer p;
            f.StrOrNull(pv, "account", p.hasAccount, p.account);
            f.Int(pv, "netCode", p.netCode);
            std::string state;
            f.Str(pv, "state", state);
            if (f.err != MatchJsonParse::Ok) break;
            if (state == "left") {
                p.left = true;   // 나간 사람은 팀 · 점수 · 죽은 수가 안 실린다
            } else if (state == "finished") {
                f.Int(pv, "team", p.team);
                f.Int(pv, "score", p.score);
                f.Int(pv, "deaths", p.deaths);
                if (const JVal* ob = f.Get(pv, "observer", JVal::T::Bool, /*required=*/false)) p.observer = ob->b;
            } else {
                f.Set(MatchJsonParse::Value, "players[].state");
            }
            out.players.push_back(std::move(p));
        }
    }

    if (f.Has(root, "teams")) {
        if (const JVal* ts = f.Get(root, "teams", JVal::T::Arr)) {
            for (const JVal& tv : ts->a) {
                if (f.err != MatchJsonParse::Ok) break;
                if (tv.t != JVal::T::Obj) { f.Set(MatchJsonParse::Type, "teams[]"); break; }
                MatchRecordTeam t;
                f.Int(tv, "id", t.id);
                f.Int(tv, "score", t.score);
                f.Int(tv, "caps", t.caps);
                out.teams.push_back(t);
            }
        }
    }
    return f.err;
}

}  // namespace nox
