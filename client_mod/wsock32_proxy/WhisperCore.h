// WhisperCore.h — 귓속말 DLL 쪽 판단(머리 파일만 · 소켓 · 스레드 · 게임 주소 · 윈도 함수 없음 — whisper_test 가 본 코드가 곧 DLL 코드).
//   플랜 `Docs/MO_WHISPER_PLAN_1105.md` 5절(DLL) · 7절(문구).
//
// 왜 떼어 두나 — 귓속말은 명령 해석 · 표시 자리 고르기 · 쥐고 있기 · 다시 붙기 간격처럼 갈래가 많다. 게임 안에서만 돌면 갈래를 전수로 못 돌리므로
//   값을 돌려주는 함수로 두고 시험이 표로 돈다(`RankNotice.h` · `LoginNotice.h` 와 같은 짜임). 소켓 · 둘째 스레드 · 게임 함수 부르기는 wsock32_proxy.cpp
//
// 채팅 서버 규약은 `server/include/nox/ChatHub.h` 머리 주석(UTF-8 줄). 게임 글은 UTF-16(wchar_t)이라 바꾸기를 여기서 직접 한다
//   (MultiByteToWideChar 를 안 써야 시험이 같은 코드를 돈다 · 틀린 바이트 · 짝 없는 대리 글자는 U+FFFD)
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace whisper {

static_assert(sizeof(wchar_t) == 2, "게임 글은 UTF-16 — MSVC 의 wchar_t");

constexpr size_t kNameMax = 9;             // `AccountRules.h` kAccountNameMax 와 같은 값(DLL 이 서버 머리 파일을 안 끌어오게 따로 둔다)
constexpr size_t kTextMax = 80;            // `ChatHub.h` kWhisperMaxChars — DLL 과 서버가 둘 다 자른다(플랜 2절 · 글자 수)
constexpr size_t kCenterLineMax = 40;      // 판 가운데 한 줄(플랜 5절 — 줄바꿈이 없고 60자가 682 게임 픽셀 · 머리 + 80자는 1024 폭에 안 들어감)
constexpr size_t kHoldMax = 20;            // 표시 못 하는 화면에서 쥐는 글(플랜 2절 — 넘치면 오래된 것부터 버리고 셈)
constexpr uint64_t kCenterGapMs = 1700;    // 판 가운데는 3칸 · 5초 — 5초를 3으로 나눈 간격(플랜 5절)
constexpr int kPlayStateInGame = 3;        // 0x42ca50 의 답 — 3 = 판(검은 불러오기 화면도 3 · 임시 DLL 기록)
constexpr uint32_t kRetryFirstMs = 3000;   // 끊기면 3초 뒤(게임 서버 → 로비 재연결과 같은 값)
constexpr uint32_t kRetryMaxMs = 30000;
constexpr uint32_t kRetryJitterMs = 1000;  // 0~999 를 더한다 — 채팅 서버가 다시 켜질 때 모두가 같은 순간에 붙지 않게

// ─────────────── UTF-16 ↔ UTF-8

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline bool IsHighSurrogate(uint32_t u) { return u >= 0xD800 && u <= 0xDBFF; }
inline bool IsLowSurrogate(uint32_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

inline std::string ToUtf8(const std::wstring& w) {
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint16_t>(w[i]);
        if (IsHighSurrogate(cp) && i + 1 < w.size() && IsLowSurrogate(static_cast<uint16_t>(w[i + 1]))) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint16_t>(w[i + 1]) - 0xDC00);
            ++i;
        } else if (IsHighSurrogate(cp) || IsLowSurrogate(cp)) {
            cp = 0xFFFD;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

inline std::wstring ToWide(const std::string& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t b = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (b < 0x80) { cp = b; n = 1; }
        else if (b >= 0xC2 && b <= 0xDF) { cp = b & 0x1Fu; n = 2; }
        else if (b >= 0xE0 && b <= 0xEF) { cp = b & 0x0Fu; n = 3; }
        else if (b >= 0xF0 && b <= 0xF4) { cp = b & 0x07u; n = 4; }
        bool ok = n != 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) {
            const uint8_t c = static_cast<uint8_t>(s[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (ok && ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;   // 겹친 꼴 · 대리 글자 · 범위 밖
        if (!ok) { out.push_back(static_cast<wchar_t>(0xFFFD)); ++i; continue; }   // 한 바이트만 버리고 다음에서 다시 맞춘다
        if (cp >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
        i += n;
    }
    return out;
}

// from 에서 글자(대리 글자 짝은 하나) n 개 뒤의 자리 — 짝을 가르지 않는다
inline size_t AdvanceChars(const std::wstring& w, size_t from, size_t n) {
    size_t i = from;
    for (size_t k = 0; k < n && i < w.size(); ++k)
        i += (IsHighSurrogate(static_cast<uint16_t>(w[i])) && i + 1 < w.size() && IsLowSurrogate(static_cast<uint16_t>(w[i + 1]))) ? 2 : 1;
    return i;
}
inline size_t CountChars(const std::wstring& w) {
    size_t n = 0;
    for (size_t i = 0; i < w.size(); i = AdvanceChars(w, i, 1)) ++n;
    return n;
}
inline std::wstring HeadChars(const std::wstring& w, size_t maxChars) { return w.substr(0, AdvanceChars(w, 0, maxChars)); }

// ─────────────── 명령 해석

inline bool IsName(const std::string& n) {   // 계정 이름 규칙(영문 · 숫자 1~9자 · `AccountRules.cpp` CheckAccountName 과 같은 판단)
    if (n.empty() || n.size() > kNameMax) return false;
    for (const char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

enum class Cmd : uint8_t {
    Plain,         // 귓속말 명령이 아님 — 원래 채팅으로 그대로
    Whisper,       // /w 이름 글
    Reply,         // /r 글
    WhisperForm,   // /w 꼴 틀림(이름 없음 · 이름 규칙 밖 · 글 없음) → 7번
    ReplyForm,     // /r 글 없음 → 10번
    ToSelf,        // 대상이 내 이름 → 11번(보내지 않음)
    kCount
};
inline const char* CmdName(Cmd c) {
    switch (c) {
    case Cmd::Plain: return "일반 채팅";
    case Cmd::Whisper: return "귓속말";
    case Cmd::Reply: return "답장";
    case Cmd::WhisperForm: return "귓속말 꼴 틀림";
    case Cmd::ReplyForm: return "답장 꼴 틀림";
    case Cmd::ToSelf: return "자기에게";
    case Cmd::kCount: break;
    }
    return "?";
}

struct Parsed {
    Cmd cmd = Cmd::Plain;
    std::string target;   // Whisper 만
    std::wstring text;    // Whisper · Reply — 80자로 자른 글
};

// `/w` `/W` `/ㅈ`(한글 입력 상태에서 친 w) · `/r` `/R` `/ㄱ`(한글 r) 다음은 빈칸이나 끝이어야 한다(`/who` 같은 다른 글은 일반 채팅).
//   이름 앞뒤 빈칸은 여럿이어도 되고, 글은 첫 글자부터 끝까지 그대로(뒤 빈칸 포함 — 서버와 같게). myName 이 비면 자기에게 검사를 안 한다
//   제어 글자(0x00~0x1F · 0x7F)는 먼저 빈칸으로 — 서버도 빈칸으로 바꾸므로(`ChatHub.cpp` OnWhisper) 바꾼 뒤 글이 비면 여기서 꼴 틀림으로 막는다.
//   안 막으면 서버가 빈 글로 보고 답을 안 줘(`BadLine`) 답 차례(CWhisperState 의 기다리는 줄)가 어긋나고, 줄바꿈이 섞이면 줄이 둘로 갈린다
inline Parsed ParseCommand(const wchar_t* raw, const std::string& myName) {
    Parsed p;
    if (raw == nullptr || raw[0] != L'/') return p;
    std::wstring clean(raw);
    for (wchar_t& ch : clean)
        if (ch < 0x20 || ch == 0x7F) ch = L' ';
    const wchar_t* t = clean.c_str();
    const wchar_t c = t[1];
    const bool isW = c == L'w' || c == L'W' || c == static_cast<wchar_t>(0x3148);   // ㅈ
    const bool isR = c == L'r' || c == L'R' || c == static_cast<wchar_t>(0x3131);   // ㄱ
    if ((!isW && !isR) || (t[2] != L'\0' && t[2] != L' ')) return p;
    const wchar_t* s = t + 2;
    while (*s == L' ') ++s;
    if (isR) {
        if (*s == L'\0') { p.cmd = Cmd::ReplyForm; return p; }
        p.cmd = Cmd::Reply;
        p.text = HeadChars(s, kTextMax);
        return p;
    }
    const wchar_t* e = s;
    std::string name;
    bool ascii = true;
    for (; *e != L'\0' && *e != L' '; ++e) {
        if (*e > 0x7F) ascii = false;
        else name.push_back(static_cast<char>(*e));
    }
    while (*e == L' ') ++e;
    if (!ascii || !IsName(name) || *e == L'\0') { p.cmd = Cmd::WhisperForm; return p; }
    if (!myName.empty() && name == myName) { p.cmd = Cmd::ToSelf; return p; }   // 이름은 바이트 그대로 비교(서버 `ChatHub.h` 와 같게)
    p.cmd = Cmd::Whisper;
    p.target = name;
    p.text = HeadChars(e, kTextMax);
    return p;
}

// 사용자 줄(대화방 명단)의 이름은 앞에 빈칸이 붙어 온다(임시 DLL 기록) — 정품 비공개 요청 0x41f430 의 이름 인자
inline std::string TrimListName(const char* name) {
    std::string n = name ? name : "";
    const size_t a = n.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    const size_t b = n.find_last_not_of(' ');
    return n.substr(a, b - a + 1);
}

// ─────────────── 표시 자리

enum class Where : uint8_t { Room, Center, Hold, kCount };
inline const char* WhereName(Where w) {
    switch (w) {
    case Where::Room: return "대화방 줄";
    case Where::Center: return "판 가운데";
    case Where::Hold: return "쥐고 있기";
    case Where::kCount: break;
    }
    return "?";
}
// 대화방 창이 있고 보이면 대화방 줄 · 판이면 판 가운데 · 그 밖(게임 목록 · 메뉴 · 숨은 대화방)은 쥐고 있다가 다음에 낸다(플랜 2절).
//   창이 없을 때 읽은 숨김 깃발은 뜻이 없으므로 roomExists 가 먼저다
inline Where ChooseShow(bool roomVisible, bool roomExists, int playState) {
    if (roomExists && roomVisible) return Where::Room;
    if (playState == kPlayStateInGame) return Where::Center;
    return Where::Hold;
}

// ─────────────── 낼 것(플랜 7절 번호)

enum class Kind : uint8_t {
    Received,        // 3 · 1 — 받은 귓속말(이름 = 보낸이)
    Sent,            // 4 · 2 — 서버 TO(이름 = 대상 · 글 = 서버가 자른 그대로)
    NoUser,          // 5 — 보낸 꼴 + 접속 중이 아닙니다.
    Limited,         // 6 — 보낸 꼴 + 너무 빠릅니다.
    NoConn,          // 8 — 보낸 꼴 + 연결 없음
    WhisperForm,     // 7 — 머리 없음
    NoReplyTarget,   // 9 — 머리 없음
    ReplyForm,       // 10 — 머리 없음
    ToSelf,          // 11 — 머리 없음
    kCount
};
inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::Received: return "받음";
    case Kind::Sent: return "보냄";
    case Kind::NoUser: return "없는 사람";
    case Kind::Limited: return "너무 빠름";
    case Kind::NoConn: return "연결 없음";
    case Kind::WhisperForm: return "귓속말 꼴";
    case Kind::NoReplyTarget: return "답장 상대 없음";
    case Kind::ReplyForm: return "답장 꼴";
    case Kind::ToSelf: return "자기에게";
    case Kind::kCount: break;
    }
    return "?";
}
// 정해진 글(7절) — 받음 · 보냄은 사람이 쓴 글이라 없음
inline const wchar_t* FixedText(Kind k) {
    switch (k) {
    case Kind::NoUser: return L"접속 중이 아닙니다.";
    case Kind::Limited: return L"너무 빠릅니다. 잠시 뒤에 보내 주세요.";
    case Kind::NoConn: return L"귓속말 서버에 연결되지 않았습니다.";
    case Kind::WhisperForm: return L"귓속말: /w 이름 할 말";
    case Kind::NoReplyTarget: return L"답장할 상대가 없습니다.";
    case Kind::ReplyForm: return L"답장: /r 할 말";
    case Kind::ToSelf: return L"자기에게는 귓속말을 보낼 수 없습니다.";
    case Kind::Received:
    case Kind::Sent:
    case Kind::kCount: break;
    }
    return nullptr;
}

struct Notice {
    Kind kind = Kind::Received;
    std::string name;     // 받음 = 보낸이 · 보냄 · 5 · 6 · 8 = 대상 · 그 밖은 빈칸
    std::wstring text;    // 받음 · 보냄만 — 나머지는 FixedText
};
inline std::wstring BodyOf(const Notice& n) {
    const wchar_t* f = FixedText(n.kind);
    return f ? std::wstring(f) : n.text;
}

// 대화방 줄 0x446d20(이름, 글, 1, 받음 0 · 보냄 1)의 인자 — 머리 꼴은 정품 문구 1 · 2번(`ui_apply.ps1` $csfSwaps)이 붙인다.
//   이름이 없으면 plain(대상 없는 안내 7 · 9 · 10 · 11 — 대화방에 머리 없이 내는 길은 DLL 몫)
struct RoomArgs {
    bool plain = false;
    std::string name;
    std::wstring text;
    int sent = 0;
};
inline RoomArgs RoomOf(const Notice& n) {
    RoomArgs r;
    r.text = BodyOf(n);
    if (n.name.empty()) { r.plain = true; return r; }
    r.name = n.name;
    r.sent = n.kind == Kind::Received ? 0 : 1;
    return r;
}
// 판 가운데 한 줄(나누기 전) — 3 `[qa2 님의 귓속말] 글` · 4 · 5 · 6 · 8 `[qa2 님에게 귓속말] 글` · 7 · 9 · 10 · 11 머리 없음(7절 아래 문단).
//   글꼴에 → 가 없어 화살표는 안 쓴다
inline std::wstring CenterOf(const Notice& n) {
    if (n.name.empty()) return BodyOf(n);
    const std::wstring who = ToWide(n.name);
    return (n.kind == Kind::Received ? L"[" + who + L" 님의 귓속말] " : L"[" + who + L" 님에게 귓속말] ") + BodyOf(n);
}

// 판 화면은 줄바꿈이 없어 40자마다 나눈다 — 끊는 자리 앞쪽 절반 안에 빈칸이 있으면 빈칸에서(빈칸은 버림) · 없으면 40자에서.
//   ★ 플랜 5절은 「두 줄로」라 적었으나 머리 약 20자 + 80자는 40자로 나누면 세 줄이라 줄 수를 묶지 않는다
inline std::vector<std::wstring> SplitForCenter(const std::wstring& line, size_t maxChars = kCenterLineMax) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (CountChars(line.substr(i)) > maxChars) {
        const size_t hard = AdvanceChars(line, i, maxChars);
        const size_t half = AdvanceChars(line, i, maxChars / 2);
        size_t cut = hard, next = hard;
        for (size_t k = hard; k > half; --k)
            if (line[k] == L' ') { cut = k; next = k + 1; break; }   // 끊는 자리(hard)가 빈칸이어도 여기서 잡힌다
        out.push_back(line.substr(i, cut - i));
        i = next;
    }
    out.push_back(line.substr(i));
    return out;
}

// ─────────────── 낼 것 상자 — 쥐고 있기(20건) · 판 가운데 1.7초 간격

class CShowBox {
public:
    struct Stats {
        uint64_t pushed = 0;    // 들어온 수
        uint64_t room = 0;      // 대화방 줄로 낸 수
        uint64_t center = 0;    // 판 가운데로 다 낸 수(나눈 줄이 다 나간 때 하나)
        uint64_t dropped = 0;   // 20건이 넘쳐 버린 수
    };   // 들어온 수 = 대화방 + 판 가운데 + 버림 + Held() — 시험이 이 합을 본다

    void Push(Notice n) {
        if (q_.size() >= kHoldMax) {
            q_.pop_front();
            ++stats_.dropped;
            part_ = 0;
        }
        q_.push_back(std::move(n));
        ++stats_.pushed;
    }
    // 매 틀 주 스레드에서 — Room 이면 쥔 것을 차례대로 다 · Center 면 1.7초에 한 줄 · Hold 면 아무것도.
    //   판 가운데로 반쯤 낸 글이 대화방 줄로 넘어가면 그 글은 대화방에 통째로 한 번 더 나온다(화면이 바뀌는 순간뿐 · 버리는 것보다 낫다)
    template <class RoomSink, class CenterSink>
    void Pump(Where where, uint64_t nowMs, RoomSink&& room, CenterSink&& center) {
        if (where == Where::Room) {
            while (!q_.empty()) {
                room(q_.front());
                q_.pop_front();
                ++stats_.room;
            }
            part_ = 0;
            return;
        }
        if (where != Where::Center || q_.empty()) return;
        if (haveCenter_ && nowMs - lastCenterMs_ < kCenterGapMs) return;
        const std::vector<std::wstring> lines = SplitForCenter(CenterOf(q_.front()));
        center(lines[part_]);
        haveCenter_ = true;
        lastCenterMs_ = nowMs;
        if (++part_ >= lines.size()) {
            q_.pop_front();
            part_ = 0;
            ++stats_.center;
        }
    }
    size_t Held() const { return q_.size(); }
    const Stats& GetStats() const { return stats_; }

private:
    std::deque<Notice> q_;
    size_t part_ = 0;   // 맨 앞 글을 판 가운데로 몇 줄 냈나
    bool haveCenter_ = false;
    uint64_t lastCenterMs_ = 0;
    Stats stats_;
};

// ─────────────── 채팅 서버 줄

enum class Line : uint8_t { Unknown, AuthOk, AuthNo, From, To, NoUser, Limit, Kicked, Ping, Pong, kCount };
struct ServerLine {
    Line kind = Line::Unknown;
    std::string name;   // From · To · NoUser
    std::string text;   // From · To = 글(UTF-8) · AuthNo = 까닭 낱말
};
inline ServerLine ParseServerLine(const std::string& l) {
    ServerLine s;
    const size_t sp = l.find(' ');
    const std::string w = l.substr(0, sp);
    const std::string rest = sp == std::string::npos ? std::string() : l.substr(sp + 1);
    if (sp == std::string::npos) {
        if (w == "AUTHOK") s.kind = Line::AuthOk;
        else if (w == "LIMIT") s.kind = Line::Limit;
        else if (w == "KICKED") s.kind = Line::Kicked;
        else if (w == "PING") s.kind = Line::Ping;
        else if (w == "PONG") s.kind = Line::Pong;
        return s;
    }
    if (w == "AUTHNO" && !rest.empty() && rest.find(' ') == std::string::npos) { s.kind = Line::AuthNo; s.text = rest; return s; }
    if (w == "NOUSER" && IsName(rest)) { s.kind = Line::NoUser; s.name = rest; return s; }
    if (w == "F" || w == "TO") {
        const size_t sp2 = rest.find(' ');
        if (sp2 == std::string::npos || !IsName(rest.substr(0, sp2))) return s;
        s.kind = w == "F" ? Line::From : Line::To;
        s.name = rest.substr(0, sp2);
        s.text = rest.substr(sp2 + 1);
    }
    return s;
}

// ─────────────── 입력 · 답 차례 — 답장 상대 · 서버 답을 기다리는 보낸 글(게임을 끄면 사라짐 · 다시 붙어도 답장 상대는 남음)

class CWhisperState {
public:
    // 귓속말 명령 하나(Plain 은 부르지 말 것) → 서버로 보낼 줄(빈 글자 = 안 보냄) · 낼 안내는 show 에.
    //   connected = AUTHOK 를 받은 연결이 있음. 보낸 줄의 화면 표시는 서버 답(TO · NOUSER · LIMIT)이 온 뒤 한 번만(보낸 쪽 화면이 받는 쪽과 같게)
    std::string OnInput(const Parsed& p, bool connected, std::vector<Notice>& show) {
        std::string target;
        switch (p.cmd) {
        case Cmd::Plain:
        case Cmd::kCount:
            return std::string();
        case Cmd::WhisperForm: show.push_back({Kind::WhisperForm, "", L""}); return std::string();
        case Cmd::ReplyForm: show.push_back({Kind::ReplyForm, "", L""}); return std::string();
        case Cmd::ToSelf: show.push_back({Kind::ToSelf, "", L""}); return std::string();
        case Cmd::Reply:
            if (replyTo_.empty()) { show.push_back({Kind::NoReplyTarget, "", L""}); return std::string(); }
            target = replyTo_;
            break;
        case Cmd::Whisper:
            target = p.target;
            break;
        }
        if (!connected) { show.push_back({Kind::NoConn, target, L""}); return std::string(); }
        pending_.push_back(target);
        return "W " + target + " " + ToUtf8(p.text);
    }
    // F · TO · NOUSER · LIMIT 이면 true(낼 것은 show 에) · 그 밖은 연결 쪽(스레드) 몫이라 false
    bool OnServerLine(const ServerLine& s, std::vector<Notice>& show) {
        switch (s.kind) {
        case Line::From:
            replyTo_ = s.name;   // 받을 때마다 보낸이로 — 내가 보낸 귓속말로는 안 바뀜(플랜 5절)
            show.push_back({Kind::Received, s.name, ToWide(s.text)});
            return true;
        case Line::To:
            PopPending();
            show.push_back({Kind::Sent, s.name, ToWide(s.text)});
            return true;
        case Line::NoUser:
            PopPending();
            show.push_back({Kind::NoUser, s.name, L""});
            return true;
        case Line::Limit:   // 서버 LIMIT 줄엔 이름이 없다 — W 하나에 답이 꼭 하나라 기다리던 맨 앞이 그 대상
            show.push_back({Kind::Limited, PopPending(), L""});
            return true;
        case Line::Unknown:
        case Line::AuthOk:
        case Line::AuthNo:
        case Line::Kicked:
        case Line::Ping:
        case Line::Pong:
        case Line::kCount:
            break;
        }
        return false;
    }
    // 연결이 끊김 — 답을 못 받은 보낸 글은 말없이 사라지지 않게 8번(서버가 이미 전했을 수도 있다 · 못 가린다)
    void OnDisconnect(std::vector<Notice>& show) {
        for (const std::string& t : pending_) show.push_back({Kind::NoConn, t, L""});
        pending_.clear();
    }
    const std::string& ReplyTarget() const { return replyTo_; }
    size_t Pending() const { return pending_.size(); }

private:
    std::string PopPending() {
        if (pending_.empty()) return std::string();
        std::string t = pending_.front();
        pending_.pop_front();
        return t;
    }
    std::string replyTo_;
    std::deque<std::string> pending_;
};

// ─────────────── 다시 붙기

// 실패가 이어진 수(붙으면 0) → 다음 시도까지 — 3 · 6 · 12 · 24 · 최대 30초 + 0~999ms(jitter 는 부르는 쪽 난수 · 나머지만 씀)
inline uint32_t NextRetryMs(uint32_t failures, uint32_t jitter) {
    uint32_t base = kRetryFirstMs;
    for (uint32_t i = 0; i < failures && base < kRetryMaxMs; ++i) base *= 2;
    if (base > kRetryMaxMs) base = kRetryMaxMs;
    return base + jitter % kRetryJitterMs;
}

enum class Drop : uint8_t {
    ConnectFail,        // 채팅 서버에 못 붙음
    Lost,               // 붙은 뒤 끊김 · 받기 오류 · 모르는 AUTHNO
    TokenUnreachable,   // /token 이 로그인 서버에 못 닿음 · 시간 초과 · 그 밖의 답
    TokenRefused,       // /token 401 · 403(열쇠 거절 · 정지 — 다른 PC 에서 암호를 바꾼 경우도)
    AuthToken,          // AUTHNO token — 채팅 서버가 토큰을 거절(새 토큰으로 다시)
    AuthReplay,         // AUTHNO replay — 같은 토큰(새 토큰으로 다시)
    AuthBanned,         // AUTHNO banned — 정지
    AuthForm,           // AUTHNO form — 이름 꼴(다시 해도 같음)
    Kicked,             // KICKED — 같은 이름이 새로 붙음(다른 게임이 가져감)
    kCount
};
inline const char* DropName(Drop d) {
    switch (d) {
    case Drop::ConnectFail: return "못 붙음";
    case Drop::Lost: return "끊김";
    case Drop::TokenUnreachable: return "토큰 못 받음";
    case Drop::TokenRefused: return "토큰 거절";
    case Drop::AuthToken: return "AUTHNO token";
    case Drop::AuthReplay: return "AUTHNO replay";
    case Drop::AuthBanned: return "AUTHNO banned";
    case Drop::AuthForm: return "AUTHNO form";
    case Drop::Kicked: return "KICKED";
    case Drop::kCount: break;
    }
    return "?";
}
// false 면 다음 NOXCHAT 줄(로비에 다시 로그인)까지 멈춤 — 그동안 /w 는 8번(플랜 5절 「멈춤」)
inline bool ShouldRetry(Drop d) {
    switch (d) {
    case Drop::ConnectFail:
    case Drop::Lost:
    case Drop::TokenUnreachable:
    case Drop::AuthToken:
    case Drop::AuthReplay:
        return true;
    case Drop::TokenRefused:
    case Drop::AuthBanned:
    case Drop::AuthForm:
    case Drop::Kicked:
    case Drop::kCount:
        break;
    }
    return false;
}
inline Drop DropOfAuthNo(const std::string& why) {
    if (why == "token") return Drop::AuthToken;
    if (why == "replay") return Drop::AuthReplay;
    if (why == "banned") return Drop::AuthBanned;
    if (why == "form") return Drop::AuthForm;
    return Drop::Lost;   // again 등 — DLL 은 AUTH 를 한 번만 보내므로 오지 않을 줄
}

// 로비 줄 `NOXCHAT a.b.c.d:포트`(`Lobby.cpp` FinishLogin) → ip(a 가 가장 높은 바이트) · port. 꼴이 하나라도 틀리면 false
inline bool ParseChatLine(const std::string& l, uint32_t& ip, uint16_t& port) {
    static const char kHead[] = "NOXCHAT ";
    if (l.compare(0, sizeof(kHead) - 1, kHead) != 0) return false;
    size_t i = sizeof(kHead) - 1;
    uint32_t v = 0;
    for (int part = 0; part < 5; ++part) {
        uint32_t n = 0;
        size_t digits = 0;
        for (; i < l.size() && l[i] >= '0' && l[i] <= '9' && digits < 6; ++i, ++digits) n = n * 10 + static_cast<uint32_t>(l[i] - '0');
        if (digits == 0) return false;
        if (part < 4) {
            if (n > 255 || i >= l.size() || l[i] != (part < 3 ? '.' : ':')) return false;
            v = (v << 8) | n;
            ++i;
        } else {
            if (n == 0 || n > 65535 || i != l.size()) return false;
            ip = v;
            port = static_cast<uint16_t>(n);
        }
    }
    return true;
}

}  // namespace whisper
