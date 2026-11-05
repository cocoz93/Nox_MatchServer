// BanCommand.cpp — 운영 명령의 판단. 설명은 `BanCommand.h` 머리.
#include "BanCommand.h"

#include <cstdio>
#include <ctime>

#include "nox/LoginLine.h"

namespace nox::auth {

namespace {

// UTF-8 글자 수(이어지는 바이트 10xxxxxx 를 안 셈) · 조절 글자가 있으면 false
bool ReasonChars(const std::string& s, size_t& chars) {
    chars = 0;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return false;
        if ((c & 0xC0) != 0x80) ++chars;
    }
    return true;
}

bool RunList(IAccountStore& db, uint32_t nowUnix, std::vector<std::string>& lines) {
    std::vector<BanListRow> rows;
    if (!db.ListActiveBans(nowUnix, rows)) {
        lines.push_back("[실패] 정지 목록 — DB");
        return false;
    }
    lines.push_back("지금 유효한 정지 " + std::to_string(rows.size()) + "건");
    for (const BanListRow& r : rows)
        lines.push_back("  " + r.name + " · 끝 " + BanUntilText(r.untilUnix) + " · 사유 " + r.reason + " · 넣은 쪽 " + r.source + " · 넣은 때 " + BanUntilText(r.createdUnix));
    return true;
}

}  // namespace

std::string BanUntilText(uint32_t untilUnix) {
    if (untilUnix == kBanForever) return "영구";
    const std::time_t t = static_cast<std::time_t>(untilUnix);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return "?";
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return b;
}

bool CheckBanCommand(const BanCommand& c, std::string& err) {
    const bool wantsReason = c.action == BanAction::Ban || c.action == BanAction::BanForever;
    if ((c.action == BanAction::List) != c.name.empty()) {
        err = c.action == BanAction::List ? "--bans 에는 이름이 없다" : "계정 이름이 없다";
        return false;
    }
    if (c.action == BanAction::Ban ? (c.days == 0 || c.days > kBanDaysMax) : c.days != 0) {
        err = c.action == BanAction::Ban ? "--days 는 1 ~ " + std::to_string(kBanDaysMax) + " 이다(더 길면 --ban-forever)" : "--days 는 --ban 에만 쓴다";
        return false;
    }
    if (!wantsReason) {
        if (!c.reason.empty()) {
            err = "--reason 은 --ban · --ban-forever 에만 쓴다";
            return false;
        }
        return true;
    }
    size_t chars = 0;
    if (!ReasonChars(c.reason, chars)) {
        err = "--reason 에 조절 글자(줄 바꿈 등)가 있다";
        return false;
    }
    if (chars == 0 || chars > kBanReasonMaxChars) {
        err = "--reason 은 1 ~ " + std::to_string(kBanReasonMaxChars) + "자다(지금 " + std::to_string(chars) + "자)";
        return false;
    }
    return true;
}

bool RunBanCommand(const BanCommand& c, IAccountStore& db, uint32_t nowUnix, std::vector<std::string>& lines) {
    switch (c.action) {
    case BanAction::List: return RunList(db, nowUnix, lines);
    case BanAction::Ban:
    case BanAction::BanForever:
    case BanAction::Unban: break;
    case BanAction::None:
    case BanAction::kCount:
        lines.push_back("[실패] 명령이 없다");
        return false;
    }
    AccountRow row;
    switch (db.Find(c.name, row)) {
    case FindResult::Found: break;
    case FindResult::NotFound:
        lines.push_back("[실패] 없는 계정: " + c.name);
        return false;
    case FindResult::Failed:
    case FindResult::kCount:
        lines.push_back("[실패] 계정 찾기 — DB");
        return false;
    }
    if (c.action == BanAction::Unban) {
        unsigned lifted = 0;
        if (!db.LiftBans(row.id, nowUnix, lifted)) {
            lines.push_back("[실패] 풀기 — DB");
            return false;
        }
        lines.push_back(lifted ? "풀림 — " + c.name + " · 행 " + std::to_string(lifted) + "개" : "풀 정지 없음 — " + c.name);
        return true;
    }
    const uint32_t until = c.action == BanAction::BanForever ? kBanForever : nowUnix + c.days * 86400u;
    if (!db.InsertBan(row.id, until, c.reason, std::string(kBanSourceAdmin), nowUnix)) {
        lines.push_back("[실패] 정지 넣기 — DB");
        return false;
    }
    lines.push_back("정지 — " + c.name + " · 끝 " + BanUntilText(until) + " · 사유 " + c.reason);
    return true;
}

}  // namespace nox::auth
