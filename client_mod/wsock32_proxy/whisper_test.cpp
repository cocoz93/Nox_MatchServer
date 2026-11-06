// whisper_test.cpp — WhisperCore.h 표 시험(DLL 과 같은 코드). test.bat 이 돌린다. 플랜 `Docs/MO_WHISPER_PLAN_1105.md` 8절 whisper_test 줄.
//
// 확인할 것
//   ① UTF-16 ↔ UTF-8 — 한글 · 대리 글자 짝 왕복 · 틀린 바이트 · 짝 없는 대리 글자는 U+FFFD · 글자 수 · 80자 자르기가 짝을 안 가름
//   ② 명령 해석 표 — /w /W /ㅈ · /r /R /ㄱ · 빈칸 여럿 · 이름 규칙 · 자기에게 · /who 같은 다른 글 · 제어 글자 · 갈래마다 개수
//   ③ ChooseShow 전수(보임 2 × 있음 2 × 판 상태 6) · 갈래 개수
//   ④ 판 가운데 나누기 경계(40 · 41 · 빈칸 자리 · 대리 글자 · 머리 + 80자)
//   ⑤ 낼 것 상자 — 20건 넘침(오래된 것부터) · 대화방은 한꺼번에 · 판 가운데 1.7초 경계 · 나눈 줄 차례 · 들어온 수 = 낸 수 + 버림 + 쥔 수
//   ⑥ 답장 상대(받기 전 · 두 사람에게 받은 뒤 · 내가 보낸 뒤) · 서버 답 차례(TO · NOUSER · LIMIT 이 기다리던 대상) · 끊김 = 8번
//   ⑦ 문구 꼴(7절 3 · 4 · 5 · 6 · 8 머리 · 7 · 9 · 10 · 11 머리 없음 · 대화방 인자)
//   ⑧ NextRetryMs 경계 · ShouldRetry 전수 · AUTHNO 까닭 → 멈춤 갈래 · ★ 조각 4 /token 답 줄 → 토큰 · 멈춤 · 다시
//   ⑨ 서버 줄 해석 · NOXCHAT 줄 해석
#include <cstdio>
#include <string>
#include <vector>

#include "WhisperCore.h"

using namespace whisper;

static int g_fail = 0, g_checks = 0;

static void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_fail;
    }
}

static const std::wstring kHan = L"안녕";   // U+C548 U+B155
static const std::wstring kEmoji = L"\xD83D\xDE00";   // U+1F600 — 대리 글자 짝

static void TestUtf()
{
    Check(ToUtf8(L"abc") == "abc", "① ASCII");
    Check(ToUtf8(kHan) == "\xEC\x95\x88\xEB\x85\x95", "① 한글 → UTF-8 세 바이트씩");
    Check(ToUtf8(kEmoji) == "\xF0\x9F\x98\x80", "① 대리 글자 짝 → 네 바이트");
    Check(ToWide(ToUtf8(kHan + kEmoji + L"x")) == kHan + kEmoji + L"x", "① 왕복");
    Check(ToUtf8(std::wstring(1, static_cast<wchar_t>(0xD83D))) == "\xEF\xBF\xBD", "① 짝 없는 앞 대리 글자 → U+FFFD");
    Check(ToUtf8(std::wstring(1, static_cast<wchar_t>(0xDE00)) + L"a") == "\xEF\xBF\xBD" "a", "① 짝 없는 뒤 대리 글자 → U+FFFD");
    const std::wstring bad = ToWide(std::string("\xC0\x80" "a" "\xED\xA0\x80" "b" "\xF5\x80\x80\x80" "\xE3\x81", 15));
    size_t fffd = 0;
    for (wchar_t c : bad) fffd += c == static_cast<wchar_t>(0xFFFD);
    Check(bad.find(L'a') != std::wstring::npos && bad.find(L'b') != std::wstring::npos && fffd == 2 + 3 + 4 + 2, "① 틀린 바이트(겹친 꼴 · 대리 글자 · F5 · 덜 온 줄)마다 U+FFFD · 뒤 글자는 살림");
    Check(ToWide(std::string("\xE0\x80\xAF" "c", 4)) == std::wstring(3, static_cast<wchar_t>(0xFFFD)) + L"c", "① 세 바이트 겹친 꼴(「/」 를 길게 쓴 것) → 바이트마다 U+FFFD");
    Check(ToWide(std::string("\xF0\x80\x80\xAF" "d", 5)) == std::wstring(4, static_cast<wchar_t>(0xFFFD)) + L"d", "① 네 바이트 겹친 꼴 → 바이트마다 U+FFFD");
    Check(CountChars(kHan + kEmoji) == 3, "① 글자 수 — 짝은 하나");
    const std::wstring s79 = std::wstring(79, L'a') + kEmoji;   // 80자 = 81 단위
    Check(HeadChars(s79, kTextMax) == s79, "① 80자(짝이 끝) — 그대로");
    Check(HeadChars(s79 + L"b", kTextMax) == s79, "① 81자 — 80자에서 · 짝을 안 가름");
    Check(HeadChars(std::wstring(81, L'한'), kTextMax).size() == 80, "① 한글 81자 → 80");
}

struct CmdRow {
    const wchar_t* in;
    const char* me;
    Cmd cmd;
    const char* target;
    const wchar_t* text;
};

static void TestParse()
{
    const CmdRow rows[] = {
        {L"/w qa2 안녕 하세요", "qa1", Cmd::Whisper, "qa2", L"안녕 하세요"},
        {L"/W qa2 hi", "qa1", Cmd::Whisper, "qa2", L"hi"},
        {L"/\x3148 qa2 hi", "qa1", Cmd::Whisper, "qa2", L"hi"},   // /ㅈ
        {L"/w   qa2   hi  ", "qa1", Cmd::Whisper, "qa2", L"hi  "},   // 빈칸 여럿 · 뒤 빈칸은 그대로
        {L"/w QA2 hi", "qa2", Cmd::Whisper, "QA2", L"hi"},   // 바이트 비교 — 대소문자가 다르면 남
        {L"/w qa2 hi", "", Cmd::Whisper, "qa2", L"hi"},       // 내 이름 모름 — 자기에게 검사 안 함
        {L"/w abcdefghi x", "qa1", Cmd::Whisper, "abcdefghi", L"x"},   // 9자
        {L"/w qa2 a\tb", "qa1", Cmd::Whisper, "qa2", L"a b"},   // 제어 글자 → 빈칸
        {L"/w\tqa2 hi", "qa1", Cmd::Whisper, "qa2", L"hi"},
        {L"/w qa1 hi", "qa1", Cmd::ToSelf, "", L""},
        {L"/w", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w ", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w qa2", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w qa2   ", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w qa2 \t\r\n", "qa1", Cmd::WhisperForm, "", L""},   // 바꾸면 빈칸뿐
        {L"/w abcdefghij x", "qa1", Cmd::WhisperForm, "", L""},   // 10자
        {L"/w q_a x", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w 한글 x", "qa1", Cmd::WhisperForm, "", L""},
        {L"/w \x3142\x3141\x0032 x", "qa1", Cmd::WhisperForm, "", L""},   // 한글 입력 상태로 친 qa2(ㅂㅁ2)
        {L"/r 고마워", "qa1", Cmd::Reply, "", L"고마워"},
        {L"/R ok", "qa1", Cmd::Reply, "", L"ok"},
        {L"/\x3131 ok", "qa1", Cmd::Reply, "", L"ok"},   // /ㄱ
        {L"/r    ok", "qa1", Cmd::Reply, "", L"ok"},
        {L"/r", "qa1", Cmd::ReplyForm, "", L""},
        {L"/r   ", "qa1", Cmd::ReplyForm, "", L""},
        {L"hello", "qa1", Cmd::Plain, "", L""},
        {L"", "qa1", Cmd::Plain, "", L""},
        {L"/", "qa1", Cmd::Plain, "", L""},
        {L"/who", "qa1", Cmd::Plain, "", L""},
        {L"/rr x", "qa1", Cmd::Plain, "", L""},
        {L"/x qa2 hi", "qa1", Cmd::Plain, "", L""},
        {L" /w qa2 hi", "qa1", Cmd::Plain, "", L""},   // 앞 빈칸 — 명령 아님
        {L"w qa2 hi", "qa1", Cmd::Plain, "", L""},
    };
    size_t count[static_cast<size_t>(Cmd::kCount)] = {};
    for (const CmdRow& r : rows) {
        const Parsed p = ParseCommand(r.in, r.me);
        ++count[static_cast<size_t>(p.cmd)];
        char what[200];
        std::snprintf(what, sizeof(what), "② 명령 「%s」 → %s (기대 %s)", ToUtf8(r.in).c_str(), CmdName(p.cmd), CmdName(r.cmd));
        Check(p.cmd == r.cmd && p.target == r.target && p.text == r.text, what);
    }
    Check(ParseCommand(nullptr, "qa1").cmd == Cmd::Plain, "② 빈 포인터 → 일반");
    const size_t want[] = {8, 9, 4, 9, 2, 1};   // Plain · Whisper · Reply · WhisperForm · ReplyForm · ToSelf
    static_assert(sizeof(want) / sizeof(want[0]) == static_cast<size_t>(Cmd::kCount), "갈래 수");
    for (size_t i = 0; i < static_cast<size_t>(Cmd::kCount); ++i) {
        char what[120];
        std::snprintf(what, sizeof(what), "② 갈래 개수 %s = %zu (기대 %zu)", CmdName(static_cast<Cmd>(i)), count[i], want[i]);
        Check(count[i] == want[i], what);
    }
    const Parsed longW = ParseCommand((L"/w qa2 " + std::wstring(100, L'가')).c_str(), "qa1");
    Check(longW.cmd == Cmd::Whisper && longW.text == std::wstring(80, L'가'), "② 귓속말 글 80자로 자름");
    const Parsed longR = ParseCommand((L"/r " + std::wstring(100, L'b')).c_str(), "qa1");
    Check(longR.cmd == Cmd::Reply && longR.text == std::wstring(80, L'b'), "② 답장 글 80자로 자름");
    Check(TrimListName(" qa2") == "qa2" && TrimListName("  qa2  ") == "qa2" && TrimListName("qa2") == "qa2" && TrimListName("   ").empty() &&
              TrimListName(nullptr).empty(),
          "② 명단 이름 앞뒤 빈칸");
}

// ★ 조각 5 — 대화방 명단에서 고른 줄로 친 글(정품 비공개 0x41f430 의 이름 · 글)
struct ListRow {
    const char* name;
    const wchar_t* text;
    const char* me;
    Cmd cmd;
    const char* target;
    const wchar_t* out;
};
static void TestParseList()
{
    const ListRow rows[] = {
        {" qa2", L"안녕", "qa1", Cmd::Whisper, "qa2", L"안녕"},   // 명단 이름은 앞에 빈칸
        {"qa2  ", L"  hi", "qa1", Cmd::Whisper, "qa2", L"hi"},    // 글 앞 빈칸은 /w 처럼 건너뜀
        {"qa2", L"a\tb", "qa1", Cmd::Whisper, "qa2", L"a b"},     // 제어 글자 → 빈칸
        {"qa2", L"%s %n", "qa1", Cmd::Whisper, "qa2", L"%s %n"},
        {"qa1", L"hi", "", Cmd::Whisper, "qa1", L"hi"},           // 내 이름 모름
        {"qa2", L"/w qa3 x", "qa1", Cmd::Whisper, "qa2", L"/w qa3 x"},   // 글 안의 명령 꼴은 그냥 글
        {" qa1", L"hi", "qa1", Cmd::ToSelf, "", L""},             // 내 줄을 고름 → 11번
        {"", L"hello world", "qa1", Cmd::WhisperForm, "", L""},   // 빈 이름 — 글의 첫 낱말이 대상이 되면 안 됨
        {"   ", L"hi", "qa1", Cmd::WhisperForm, "", L""},
        {nullptr, L"hi", "qa1", Cmd::WhisperForm, "", L""},
        {"qa2", L"", "qa1", Cmd::WhisperForm, "", L""},
        {"qa2", L"   ", "qa1", Cmd::WhisperForm, "", L""},
        {"qa2", L"\r\n", "qa1", Cmd::WhisperForm, "", L""},
        {"qa2", nullptr, "qa1", Cmd::WhisperForm, "", L""},
        {"@qa2", L"hi", "qa1", Cmd::WhisperForm, "", L""},       // 계정 규칙 밖 꼴
        {"abcdefghij", L"x", "qa1", Cmd::WhisperForm, "", L""},   // 10자
        {"q a", L"x", "qa1", Cmd::WhisperForm, "", L""},          // 가운데 빈칸
        {"\xEA\xB0\x80", L"x", "qa1", Cmd::WhisperForm, "", L""},   // 한글 바이트(가)
    };
    size_t count[static_cast<size_t>(Cmd::kCount)] = {};
    for (const ListRow& r : rows) {
        const Parsed p = ParseListWhisper(r.name, r.text, r.me);
        ++count[static_cast<size_t>(p.cmd)];
        char what[220];
        std::snprintf(what, sizeof(what), "②-2 고른 줄 「%s」 글 「%s」 → %s (기대 %s)", r.name ? r.name : "(null)", r.text ? ToUtf8(r.text).c_str() : "(null)",
                      CmdName(p.cmd), CmdName(r.cmd));
        Check(p.cmd == r.cmd && p.target == r.target && p.text == r.out, what);
    }
    const size_t want[] = {0, 6, 0, 11, 0, 1};   // Plain · Whisper · Reply · WhisperForm · ReplyForm · ToSelf — 고른 줄은 일반 채팅 · 답장이 없다
    static_assert(sizeof(want) / sizeof(want[0]) == static_cast<size_t>(Cmd::kCount), "갈래 수");
    for (size_t i = 0; i < static_cast<size_t>(Cmd::kCount); ++i) {
        char what[120];
        std::snprintf(what, sizeof(what), "②-2 고른 줄 갈래 개수 %s = %zu (기대 %zu)", CmdName(static_cast<Cmd>(i)), count[i], want[i]);
        Check(count[i] == want[i], what);
    }
    const Parsed longL = ParseListWhisper(" qa2", (std::wstring(100, L'가')).c_str(), "qa1");
    Check(longL.cmd == Cmd::Whisper && longL.text == std::wstring(80, L'가'), "②-2 고른 줄 글 80자로 자름");
}

static void TestChooseShow()
{
    size_t count[static_cast<size_t>(Where::kCount)] = {};
    for (int vis = 0; vis < 2; ++vis)
        for (int has = 0; has < 2; ++has)
            for (int play = -1; play <= 4; ++play) {
                const Where w = ChooseShow(vis != 0, has != 0, play);
                ++count[static_cast<size_t>(w)];
                const Where want = (vis && has) ? Where::Room : (play == 3 ? Where::Center : Where::Hold);   // 3 = 정품 판 상태(0x42ca50 · 조사 r1) — 상수를 쓰면 상수가 틀려도 안 걸린다
                char what[120];
                std::snprintf(what, sizeof(what), "③ 보임 %d · 있음 %d · 판 상태 %d → %s", vis, has, play, WhereName(w));
                Check(w == want, what);
            }
    // 24칸 = 대화방 6(보임 · 있음 × 판 상태 6) · 판 가운데 3(나머지 셋 × 판 상태 3) · 쥐기 15
    Check(count[0] == 6 && count[1] == 3 && count[2] == 15, "③ 갈래 개수 대화방 6 · 판 가운데 3 · 쥐기 15");
}

static std::wstring Join(const std::vector<std::wstring>& v)
{
    std::wstring s;
    for (const auto& x : v) s += x;
    return s;
}

static void TestSplit()
{
    const std::wstring a40(40, L'a');
    Check(SplitForCenter(a40).size() == 1, "④ 40자 — 한 줄");
    const auto s41 = SplitForCenter(a40 + L"b");
    Check(s41.size() == 2 && s41[0] == a40 && s41[1] == L"b", "④ 41자 빈칸 없음 — 40자에서");
    const std::wstring sp = std::wstring(25, L'a') + L" " + std::wstring(25, L'b');   // 빈칸이 25번 자리(앞쪽 절반 20 뒤)
    const auto ss = SplitForCenter(sp);
    Check(ss.size() == 2 && ss[0] == std::wstring(25, L'a') && ss[1] == std::wstring(25, L'b'), "④ 빈칸에서 끊고 빈칸은 버림");
    const std::wstring early = std::wstring(10, L'a') + L" " + std::wstring(40, L'b');   // 빈칸이 앞쪽 절반 안 — 빈칸에서 안 끊음
    const auto se = SplitForCenter(early);
    Check(se.size() == 2 && se[0] == early.substr(0, 40), "④ 빈칸이 앞쪽 절반 안이면 40자에서");
    const std::wstring at40 = a40 + L" " + std::wstring(5, L'b');   // 끊는 자리가 곧 빈칸
    const auto s4 = SplitForCenter(at40);
    Check(s4.size() == 2 && s4[0] == a40 && s4[1] == L"bbbbb", "④ 41번째가 빈칸 — 거기서");
    const std::wstring em = std::wstring(39, L'a') + kEmoji + L"cc";   // 40번째 글자가 짝
    const auto sem = SplitForCenter(em);
    Check(sem.size() == 2 && sem[0] == std::wstring(39, L'a') + kEmoji && sem[1] == L"cc", "④ 대리 글자 짝을 안 가름");
    Notice n{Kind::Received, "abcdefghi", std::wstring(80, L'가')};
    const auto full = SplitForCenter(CenterOf(n));
    bool each = true;
    for (const auto& l : full) each = each && CountChars(l) <= kCenterLineMax && !l.empty();
    Check(full.size() == 3 && each && Join(full) == CenterOf(n).substr(0, full[0].size()) + full[1] + full[2], "④ 머리 + 80자 — 세 줄 · 줄마다 40자 이하");
    std::wstring rejoin;
    for (const auto& l : SplitForCenter(sp)) rejoin += (rejoin.empty() ? L"" : L" ") + l;
    Check(rejoin == sp, "④ 빈칸에서 끊은 것은 빈칸을 넣어 이으면 원래 글");
}

static void TestBox()
{
    CShowBox box;
    for (int i = 0; i < 25; ++i) box.Push({Kind::Received, "qa" + std::to_string(i), L"x"});
    Check(box.Held() == 20 && box.GetStats().dropped == 5, "⑤ 25건 → 쥠 20 · 버림 5");
    std::vector<std::string> roomNames;
    box.Pump(Where::Hold, 0, [&](const Notice& n) { roomNames.push_back(n.name); }, [&](const std::wstring&) { Check(false, "⑤ 쥐기에서 판 가운데"); });
    Check(roomNames.empty() && box.Held() == 20, "⑤ 쥐기 — 아무것도 안 냄");
    box.Pump(Where::Room, 0, [&](const Notice& n) { roomNames.push_back(n.name); }, [&](const std::wstring&) { Check(false, "⑤ 대화방에서 판 가운데"); });
    Check(roomNames.size() == 20 && roomNames.front() == "qa5" && roomNames.back() == "qa24" && box.Held() == 0, "⑤ 대화방 — 한꺼번에 · 오래된 다섯을 버린 차례");
    // 판 가운데 — 짧은 글 둘 + 나뉘는 글 하나(세 줄)
    box.Push({Kind::Received, "qa2", L"one"});
    box.Push({Kind::Sent, "qa3", L"two"});
    box.Push({Kind::Received, "qa4", std::wstring(80, L'z')});
    std::vector<std::pair<uint64_t, std::wstring>> shown;
    auto pump = [&](uint64_t t) {
        box.Pump(Where::Center, t, [&](const Notice&) { Check(false, "⑤ 판 가운데에서 대화방"); }, [&](const std::wstring& l) { shown.push_back({t, l}); });
    };
    pump(1000);
    pump(2699);
    pump(2700);
    pump(4399);
    pump(4400);
    pump(6100);
    pump(7800);
    pump(9500);
    Check(shown.size() == 5, "⑤ 판 가운데 — 1.7초마다 한 줄(1699ms 는 안 냄)");
    if (shown.size() == 5) {
        Check(shown[0].first == 1000 && shown[0].second == L"[qa2 님의 귓속말] one", "⑤ 첫 줄은 바로");
        Check(shown[1].first == 2700 && shown[1].second == L"[qa3 님에게 귓속말] two", "⑤ 1700ms 에 둘째");
        Check(shown[2].first == 4400 && shown[3].first == 6100 && shown[4].first == 7800, "⑤ 나눈 줄도 1.7초씩");
        Check(shown[2].second + shown[3].second + shown[4].second ==
                  Join(SplitForCenter(CenterOf({Kind::Received, "qa4", std::wstring(80, L'z')}))),
              "⑤ 나눈 줄 차례");
    }
    // 반쯤 낸 글이 대화방으로 넘어감
    box.Push({Kind::Received, "qa5", std::wstring(80, L'y')});
    shown.clear();
    pump(20000);
    size_t roomN = 0;
    box.Pump(Where::Room, 20001, [&](const Notice& n) { roomN += n.name == "qa5"; }, [&](const std::wstring&) {});
    Check(shown.size() == 1 && roomN == 1 && box.Held() == 0, "⑤ 판 가운데 첫 줄 뒤 대화방이 되면 대화방에 통째로");
    const CShowBox::Stats& st = box.GetStats();
    Check(st.pushed == 29 && st.room == 21 && st.center == 3 && st.dropped == 5 && st.pushed == st.room + st.center + st.dropped + box.Held(),
          "⑤ 들어온 29 = 대화방 21 + 판 가운데 3 + 버림 5 + 쥠 0");
    // 넘쳐 맨 앞(반쯤 낸 글)을 버리면 다음 글은 첫 줄부터
    CShowBox b2;
    b2.Push({Kind::Received, "qa1", std::wstring(80, L'k')});
    std::vector<std::wstring> c2;
    b2.Pump(Where::Center, 0, [](const Notice&) {}, [&](const std::wstring& l) { c2.push_back(l); });
    for (int i = 0; i < 20; ++i) b2.Push({Kind::Received, "qb", L"n"});
    b2.Pump(Where::Center, 5000, [](const Notice&) {}, [&](const std::wstring& l) { c2.push_back(l); });
    Check(c2.size() == 2 && c2[1] == L"[qb 님의 귓속말] n" && b2.GetStats().dropped == 1, "⑤ 반쯤 낸 글을 넘침으로 버리면 다음 글 첫 줄부터");
    // 반쯤 낸 글을 대화방으로 넘기면 다음 글은 판 가운데 첫 줄부터
    CShowBox b3;
    const Notice m{Kind::Received, "qa2", std::wstring(80, L'm')};
    b3.Push({Kind::Received, "qa1", std::wstring(80, L'k')});
    std::vector<std::wstring> c3;
    b3.Pump(Where::Center, 0, [](const Notice&) {}, [&](const std::wstring& l) { c3.push_back(l); });
    b3.Pump(Where::Room, 1, [](const Notice&) {}, [&](const std::wstring& l) { c3.push_back(l); });
    b3.Push(m);
    b3.Pump(Where::Center, 5000, [](const Notice&) {}, [&](const std::wstring& l) { c3.push_back(l); });
    Check(c3.size() == 2 && c3[1] == SplitForCenter(CenterOf(m))[0], "⑤ 대화방으로 넘긴 뒤 다음 글은 판 가운데 첫 줄부터");
}

static void TestState()
{
    CWhisperState st;
    std::vector<Notice> show;
    Check(st.OnInput(ParseCommand(L"/r hi", "qa1"), true, show).empty() && show.size() == 1 && show[0].kind == Kind::NoReplyTarget,
          "⑥ 받기 전 /r → 9번");
    show.clear();
    Check(st.OnServerLine(ParseServerLine("F qa2 안녕"), show) && st.ReplyTarget() == "qa2" && show.size() == 1 &&
              show[0].kind == Kind::Received && show[0].name == "qa2" && show[0].text == kHan,
          "⑥ F → 받음 · 답장 상대 qa2");
    show.clear();
    st.OnServerLine(ParseServerLine("F qa3 x"), show);
    Check(st.ReplyTarget() == "qa3", "⑥ 둘째 사람에게 받으면 그 사람");
    show.clear();
    Check(st.OnInput(ParseCommand(L"/w qa5 yo", "qa1"), true, show) == "W qa5 yo" && show.empty() && st.Pending() == 1, "⑥ /w → W 줄 · 표시는 답 뒤");
    Check(st.ReplyTarget() == "qa3", "⑥ 내가 보낸 귓속말로는 답장 상대가 안 바뀜");
    Check(st.OnInput(ParseCommand(L"/r 고마워", "qa1"), true, show) == "W qa3 \xEA\xB3\xA0\xEB\xA7\x88\xEC\x9b\x8C" && st.Pending() == 2,
          "⑥ /r → 답장 상대에게 W(UTF-8)");
    Check(st.OnInput(ParseCommand(L"/w nobody x", "qa1"), true, show) == "W nobody x" && st.Pending() == 3, "⑥ 셋째 W");
    Check(st.OnInput(ParseCommand(L"/w qa6 y", "qa1"), true, show) == "W qa6 y" && st.Pending() == 4, "⑥ 넷째 W");
    st.OnServerLine(ParseServerLine("TO qa5 yo"), show);
    st.OnServerLine(ParseServerLine("TO qa3 고마워"), show);
    st.OnServerLine(ParseServerLine("NOUSER nobody"), show);
    st.OnServerLine(ParseServerLine("LIMIT"), show);
    Check(show.size() == 4 && show[0].kind == Kind::Sent && show[0].name == "qa5" && show[1].kind == Kind::Sent && show[1].name == "qa3" &&
              show[2].kind == Kind::NoUser && show[2].name == "nobody" && show[3].kind == Kind::Limited && show[3].name == "qa6" && st.Pending() == 0,
          "⑥ 답 차례 — TO · TO · NOUSER · LIMIT(이름 = 기다리던 qa6)");
    show.clear();
    st.OnServerLine(ParseServerLine("LIMIT"), show);
    Check(show.size() == 1 && show[0].kind == Kind::Limited && show[0].name.empty() && CenterOf(show[0]) == FixedText(Kind::Limited),
          "⑥ 기다리는 것 없이 LIMIT — 머리 없이");
    show.clear();
    Check(st.OnInput(ParseCommand(L"/w qa7 a", "qa1"), false, show).empty() && show.size() == 1 && show[0].kind == Kind::NoConn && show[0].name == "qa7" &&
              st.Pending() == 0,
          "⑥ 연결 없음 /w → 8번(대상 머리)");
    show.clear();
    Check(st.OnInput(ParseCommand(L"/r a", "qa1"), false, show).empty() && show.size() == 1 && show[0].kind == Kind::NoConn && show[0].name == "qa3",
          "⑥ 연결 없음 /r → 8번(답장 상대 머리)");
    show.clear();
    st.OnInput(ParseCommand(L"/w qa8 a", "qa1"), true, show);
    st.OnInput(ParseCommand(L"/w qa9 b", "qa1"), true, show);
    st.OnDisconnect(show);
    Check(show.size() == 2 && show[0].kind == Kind::NoConn && show[0].name == "qa8" && show[1].name == "qa9" && st.Pending() == 0 && st.ReplyTarget() == "qa3",
          "⑥ 끊김 — 답 못 받은 둘에 8번 · 답장 상대는 남음");
    show.clear();
    st.OnInput(ParseCommand(L"/w qa1 a", "qa1"), true, show);
    st.OnInput(ParseCommand(L"/w", "qa1"), true, show);
    st.OnInput(ParseCommand(L"/r", "qa1"), true, show);
    Check(show.size() == 3 && show[0].kind == Kind::ToSelf && show[1].kind == Kind::WhisperForm && show[2].kind == Kind::ReplyForm && st.Pending() == 0,
          "⑥ 자기에게 · 꼴 틀림 둘 — 안 보냄");
    show.clear();
    Check(st.OnInput(ParseCommand(L"hello", "qa1"), true, show).empty() && show.empty(), "⑥ 일반 채팅은 아무것도 안 함");
    const char* conn[] = {"AUTHOK", "AUTHNO token", "KICKED", "PING", "PONG", "XYZ"};
    for (const char* l : conn) {
        char what[80];
        std::snprintf(what, sizeof(what), "⑥ 연결 줄 %s 는 상태가 안 받음", l);
        Check(!st.OnServerLine(ParseServerLine(l), show) && show.empty(), what);
    }
}

static void TestText()
{
    Check(CenterOf({Kind::Received, "qa2", L"글"}) == L"[qa2 님의 귓속말] 글", "⑦ 3번 판 받음");
    Check(CenterOf({Kind::Sent, "qa2", L"글"}) == L"[qa2 님에게 귓속말] 글", "⑦ 4번 판 보냄");
    Check(CenterOf({Kind::NoUser, "nobody", L""}) == L"[nobody 님에게 귓속말] 접속 중이 아닙니다.", "⑦ 5번(7절 예시 그대로)");
    Check(CenterOf({Kind::Limited, "qa2", L""}) == L"[qa2 님에게 귓속말] 너무 빠릅니다. 잠시 뒤에 보내 주세요.", "⑦ 6번");
    Check(CenterOf({Kind::NoConn, "qa2", L""}) == L"[qa2 님에게 귓속말] 귓속말 서버에 연결되지 않았습니다.", "⑦ 8번");
    Check(CenterOf({Kind::WhisperForm, "", L""}) == L"귓속말: /w 이름 할 말", "⑦ 7번 머리 없음");
    Check(CenterOf({Kind::NoReplyTarget, "", L""}) == L"답장할 상대가 없습니다.", "⑦ 9번");
    Check(CenterOf({Kind::ReplyForm, "", L""}) == L"답장: /r 할 말", "⑦ 10번");
    Check(CenterOf({Kind::ToSelf, "", L""}) == L"자기에게는 귓속말을 보낼 수 없습니다.", "⑦ 11번");
    size_t fixed = 0;
    for (size_t i = 0; i < static_cast<size_t>(Kind::kCount); ++i) fixed += FixedText(static_cast<Kind>(i)) != nullptr;
    Check(fixed == 7 && FixedText(Kind::Received) == nullptr && FixedText(Kind::Sent) == nullptr, "⑦ 정해진 글 7 · 받음 · 보냄은 사람 글");
    const RoomArgs r0 = RoomOf({Kind::Received, "qa2", L"hi"});
    const RoomArgs r1 = RoomOf({Kind::Sent, "qa2", L"hi"});
    const RoomArgs r5 = RoomOf({Kind::NoUser, "nobody", L""});
    const RoomArgs r9 = RoomOf({Kind::NoReplyTarget, "", L""});
    Check(!r0.plain && r0.name == "qa2" && r0.text == L"hi" && r0.sent == 0, "⑦ 대화방 받음 — 받음 0");
    Check(!r1.plain && r1.sent == 1 && !r5.plain && r5.sent == 1 && r5.text == FixedText(Kind::NoUser), "⑦ 대화방 보냄 · 5번 — 보냄 1(보낸 꼴)");
    Check(r9.plain && r9.name.empty() && r9.text == FixedText(Kind::NoReplyTarget), "⑦ 대화방 9번 — 머리 없음");
}

static void TestRetry()
{
    const uint32_t want[] = {3000, 6000, 12000, 24000, 30000, 30000, 30000};
    for (uint32_t f = 0; f < 7; ++f) {
        char what[80];
        std::snprintf(what, sizeof(what), "⑧ 실패 %u번 → %u", f, NextRetryMs(f, 0));
        Check(NextRetryMs(f, 0) == want[f], what);
    }
    Check(NextRetryMs(0, 999) == 3999 && NextRetryMs(0, 1000) == 3000 && NextRetryMs(4, 1999) == 30999, "⑧ 흩음 0~999 · 넘는 난수는 나머지");
    Check(NextRetryMs(1000000, 0) == 30000 && NextRetryMs(0xffffffffu, 0xffffffffu) == 30000 + 0xffffffffu % 1000, "⑧ 아주 큰 실패 수도 30초(넘침 없음)");
    size_t retry = 0;
    for (size_t i = 0; i < static_cast<size_t>(Drop::kCount); ++i) {
        const Drop d = static_cast<Drop>(i);
        const bool want2 = d == Drop::ConnectFail || d == Drop::Lost || d == Drop::TokenUnreachable || d == Drop::AuthToken || d == Drop::AuthReplay;
        char what[80];
        std::snprintf(what, sizeof(what), "⑧ %s → %s", DropName(d), ShouldRetry(d) ? "다시" : "멈춤");
        Check(ShouldRetry(d) == want2, what);
        retry += ShouldRetry(d);
    }
    Check(retry == 5 && static_cast<size_t>(Drop::kCount) - retry == 4, "⑧ 다시 5 · 멈춤 4(토큰 거절 · 정지 · 이름 꼴 · KICKED)");
    Check(DropOfAuthNo("token") == Drop::AuthToken && DropOfAuthNo("replay") == Drop::AuthReplay && DropOfAuthNo("banned") == Drop::AuthBanned &&
              DropOfAuthNo("form") == Drop::AuthForm && DropOfAuthNo("again") == Drop::Lost && DropOfAuthNo("") == Drop::Lost,
          "⑧ AUTHNO 까닭 다섯 + 빈칸");
    // ★ 조각 4 — /token 답 줄 → 토큰 · 멈춤 · 다시(로그인 서버 까닭 낱말 전부 + 꼴 밖)
    struct TokCase { const char* line; bool ok; const char* hex; Drop why; };
    const TokCase tc[] = {
        {"NXL1abc123", true, "abc123", Drop::kCount},
        {"NXL1", false, "", Drop::TokenUnreachable},              // 토큰 글 없음
        {"NXL!key_invalid", false, "", Drop::TokenRefused},       // 401
        {"NXL!banned", false, "", Drop::TokenRefused},            // 403
        {"NXL!bad_input", false, "", Drop::TokenRefused},         // 400
        {"NXL!unreachable", false, "", Drop::TokenUnreachable},   // DLL 이 못 닿음
        {"NXL!server_error", false, "", Drop::TokenUnreachable},  // 503 · 모르는 답
        {"NXL!too_many_tries", false, "", Drop::TokenUnreachable},   // 429
        {"NXL!launcher_required", false, "", Drop::TokenUnreachable},
        {"NXL!key_invalidx", false, "", Drop::TokenUnreachable},  // 낱말과 앞만 같음
        {"", false, "", Drop::TokenUnreachable},
        {"key_invalid", false, "", Drop::TokenUnreachable},       // 머리 없음
    };
    size_t refused = 0;
    for (const TokCase& c : tc) {
        std::string hex = "x";
        Drop why = Drop::kCount;
        const bool ok = TokenOfLine(c.line, hex, why);
        char what[120];
        std::snprintf(what, sizeof(what), "⑧ 토큰 답 「%s」 → %s", c.line, ok ? "토큰" : DropName(why));
        Check(ok == c.ok && hex == c.hex && why == c.why, what);
        refused += !ok && why == Drop::TokenRefused;
    }
    Check(refused == 3, "⑧ 토큰 답 멈춤 셋(key_invalid · banned · bad_input)");
}

static void TestLines()
{
    const ServerLine f = ParseServerLine("F qa2 안녕 하세요");
    Check(f.kind == Line::From && f.name == "qa2" && f.text == "안녕 하세요", "⑨ F — 글은 첫 빈칸 뒤 전부");
    const ServerLine t = ParseServerLine("TO qa2  두 칸");
    Check(t.kind == Line::To && t.text == " 두 칸", "⑨ TO — 글 앞 빈칸 그대로");
    Check(ParseServerLine("F qa2").kind == Line::Unknown && ParseServerLine("F q_a x").kind == Line::Unknown && ParseServerLine("TO").kind == Line::Unknown,
          "⑨ F · TO 꼴 틀림 → 모름");
    Check(ParseServerLine("NOUSER nobody").kind == Line::NoUser && ParseServerLine("NOUSER").kind == Line::Unknown &&
              ParseServerLine("NOUSER a b").kind == Line::Unknown,
          "⑨ NOUSER");
    Check(ParseServerLine("AUTHNO banned").kind == Line::AuthNo && ParseServerLine("AUTHNO banned").text == "banned" &&
              ParseServerLine("AUTHNO").kind == Line::Unknown && ParseServerLine("AUTHNO a b").kind == Line::Unknown,
          "⑨ AUTHNO");
    Check(ParseServerLine("AUTHOK").kind == Line::AuthOk && ParseServerLine("LIMIT").kind == Line::Limit && ParseServerLine("KICKED").kind == Line::Kicked &&
              ParseServerLine("PING").kind == Line::Ping && ParseServerLine("PONG").kind == Line::Pong && ParseServerLine("AUTHOK x").kind == Line::Unknown &&
              ParseServerLine("").kind == Line::Unknown,
          "⑨ 낱말 하나짜리 줄 · 뒤에 붙으면 모름");
    struct ChatRow {
        const char* in;
        bool ok;
        uint32_t ip;
        uint16_t port;
    };
    const ChatRow rows[] = {
        {"NOXCHAT 192.168.0.9:4020", true, 0xC0A80009u, 4020},
        {"NOXCHAT 127.0.0.1:1", true, 0x7F000001u, 1},
        {"NOXCHAT 0.0.0.0:65535", true, 0, 65535},
        {"NOXCHAT 256.0.0.1:4020", false, 0, 0},
        {"NOXCHAT 1.2.3:4020", false, 0, 0},
        {"NOXCHAT 1.2.3.4", false, 0, 0},
        {"NOXCHAT 1.2.3.4:", false, 0, 0},
        {"NOXCHAT 1.2.3.4:0", false, 0, 0},
        {"NOXCHAT 1.2.3.4:65536", false, 0, 0},
        {"NOXCHAT 1.2.3.4:4020 ", false, 0, 0},
        {"NOXCHAT 1.2.3.4:4020x", false, 0, 0},
        {"NOXCHAT  1.2.3.4:4020", false, 0, 0},
        {"NOXCHAT 1..3.4:4020", false, 0, 0},
        {"NOXCHAT 1.2.3.4:0004020", false, 0, 0},   // 여섯 자리 넘음
        {"NOXCHATX 1.2.3.4:4020", false, 0, 0},
        {"NOXTOKEN 1.2.3.4:4020", false, 0, 0},
    };
    for (const ChatRow& r : rows) {
        uint32_t ip = 0;
        uint16_t port = 0;
        const bool ok = ParseChatLine(r.in, ip, port);
        char what[120];
        std::snprintf(what, sizeof(what), "⑨ 「%s」 → %s", r.in, ok ? "읽음" : "틀림");
        Check(ok == r.ok && (!ok || (ip == r.ip && port == r.port)), what);
    }
}

int main()
{
    TestUtf();
    TestParse();
    TestParseList();
    TestChooseShow();
    TestSplit();
    TestBox();
    TestState();
    TestText();
    TestRetry();
    TestLines();
    std::printf("%s (확인 %d건 · 실패 %d건)\n", g_fail ? "WHISPER FAIL" : "WHISPER PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
