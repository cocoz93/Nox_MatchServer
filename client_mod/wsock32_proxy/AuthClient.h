// AuthClient.h — 로그인 서버(nox_auth)에 이름 · 암호를 묻고 `apgar` 자리에 넣을 글 한 줄을 받아 오는 부품(WinHTTP · 막는 호출).
//   DLL(wsock32_proxy.cpp)과 시험(auth_client_test.cpp)이 같이 쓴다 — 시험이 본 코드가 곧 DLL 코드다(`LobbyLineFilter.h` 와 같은 꼴).
//   설계 Docs/LOGIN_AUTH_DESIGN.md 「로그인 한 번의 부르는 차례」 · 요청 · 답 꼴의 짝은 server/src/auth/AuthService.cpp 의 Login
//
// 요청 = POST <url> · 폼 `name=…&password=…`(바이트 단위 %XX 인코딩)
// 답   = 200 `NXL1<토큰 16진 114자>` 또는 400 · 401 · 503 `NXL!<까닭>` 한 줄
// 돌려주는 글(Result::line)은 늘 `apgar` 자리에 넣을 수 있는 두 꼴 가운데 하나다 — 서버 답이 이상하면 `NXL!server_error`, 못 닿으면 `NXL!unreachable`
//
// 한도 — 처음부터 끝까지 timeoutMs. 게임 주 스레드가 막히는 시간이라(5초 넘으면 윈도가 「응답 없음」으로 봄 — 설계 메모) 반드시 지켜야 한다.
//   WinHTTP 의 단계별 시간 제한은 합을 보장하지 않는다(10-26 시험 — 한도 1초에 응답 없음이 1.9초 · 헤더 0.7초 + 본문 0.7초가 그대로 성공).
//   그래서 호출은 일꾼 스레드가 하고, 부른 쪽은 마감까지만 기다린다. 마감이 오면 핸들을 닫아 일꾼의 호출을 끊는다
// TLS — 검사를 끄지 않는다(자작 인증서는 사람 1회 `auth_setup.ps1` 이 믿는 루트에 넣는다) · 프록시 안 탐 · 넘겨주기(리다이렉트) 안 따라감(암호가 딴 데로 가지 않게)
//   ★ 10-28 다른 PC — 지문 고정(`pinHex` = 서버 인증서 DER 의 SHA-256 16진 64자). 주면 윈도의 믿는 루트 · 이름 검사 대신 지문으로 서버를 가른다
//   (다른 PC 엔 자작 인증서를 그 PC 루트에 넣지 않고 · 서버 주소(공인 IP)가 인증서 이름에 없어도 됨). 머리만 먼저 보내 TLS 인사를 끝내고
//   지문이 맞을 때만 본문(암호)을 쓴다 — 틀리면 암호는 선에 안 나감. 지문을 줬는데 평문(http) 주소면 아예 안 붙는다. 안 주면 옛 길 그대로
//   (틀린 지문이면 상대는 본문 없는 `POST /login` 머리만 받을 수 있다 — 10-28 실측 nox_auth 기록 「400 규칙 밖」. 머리엔 이름 · 암호가 없다)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>   // ★ 10-28 PCCERT_CONTEXT(지문 고정) — WIN32_LEAN_AND_MEAN 이 빼므로 따로
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "nox/LoginLine.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace authclient {

constexpr DWORD kTimeoutMs = 3000;
constexpr size_t kPinBytes = 32;       // ★ 10-28 지문 = 서버 인증서(DER)의 SHA-256 — 16진 64자로 받는다(`tools/auth_pin.ps1` 이 찍는 값)
constexpr size_t kTokenHexLen = 114;   // LoginToken.h kLoginTokenSize(57) × 2 — auth_client_test 가 static_assert 로 대조한다
constexpr size_t kMaxReply = 1024;     // 이보다 긴 답은 로그인 서버의 답이 아니다

struct Result {
    std::string line;   // `apgar` 자리에 넣을 글(`NXL1…` 또는 `NXL!…`)
    int status = 0;     // HTTP 상태(못 닿았으면 0)
    DWORD error = 0;    // 못 닿았을 때의 WinHTTP 오류(GetLastError) · 마감이 와서 끊었으면 ERROR_WINHTTP_TIMEOUT
    DWORD ms = 0;       // 걸린 시간
};

namespace detail {

inline std::string FailLine(nox::LoginFail f) { return std::string(nox::kLoginFailMark) + nox::LoginFailName(f); }

// 바이트 단위 — 글자 하나가 공백 · & · = · % · 한글 바이트여도 폼의 칸 경계를 안 깬다. 영숫자와 -_.~ 만 그대로
inline std::string UrlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

inline bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\r' || s[b] == '\n' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 서버 답을 `apgar` 자리에 넣을 글로 — 상태와 본문이 서로 맞아야 믿는다(200 은 토큰만 · 400 401 503 은 아는 까닭만). 그 밖은 서버 오류
inline std::string Classify(int status, const std::string& rawBody)
{
    const std::string body = Trim(rawBody);
    if (status == 200) {
        bool ok = body.size() == nox::kLoginTokenMark.size() + kTokenHexLen && body.compare(0, nox::kLoginTokenMark.size(), nox::kLoginTokenMark) == 0;
        for (size_t i = nox::kLoginTokenMark.size(); ok && i < body.size(); ++i) ok = IsHex(body[i]);
        if (ok) return body;
    } else if (status == 400 || status == 401 || status == 503) {
        if (body.compare(0, nox::kLoginFailMark.size(), nox::kLoginFailMark) == 0) {
            nox::LoginFail f;
            if (nox::ParseLoginFail(std::string_view(body).substr(nox::kLoginFailMark.size()), f)) return body;
        }
    }
    return FailLine(nox::LoginFail::ServerError);
}

// ★ 10-28 지문 — 16진 64자(대소문자) → 32바이트. 꼴이 틀리면 false(빈칸 · 콜론 안 받음)
inline bool ParsePin(const std::string& hex, unsigned char out[kPinBytes])
{
    if (hex.size() != 2 * kPinBytes) return false;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (size_t i = 0; i < kPinBytes; ++i) {
        if (!IsHex(hex[2 * i]) || !IsHex(hex[2 * i + 1])) return false;
        out[i] = static_cast<unsigned char>(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
    }
    return true;
}

inline bool Sha256(const unsigned char* p, DWORD n, unsigned char out[kPinBytes])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0 &&
              BCryptHashData(h, const_cast<PUCHAR>(p), n, 0) == 0 && BCryptFinishHash(h, out, static_cast<ULONG>(kPinBytes), 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 이 요청의 서버 인증서가 지문과 같나 — TLS 인사가 끝난 뒤(WinHttpSendRequest 뒤)에 부른다
inline bool ServerCertMatches(HINTERNET req, const unsigned char pin[kPinBytes])
{
    PCCERT_CONTEXT cert = NULL;
    DWORD sz = sizeof(cert);
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &sz) || !cert) return false;
    unsigned char got[kPinBytes];
    const bool ok = Sha256(cert->pbCertEncoded, cert->cbCertEncoded, got) && memcmp(got, pin, kPinBytes) == 0;   // 지문은 비밀이 아니라 일정 시간 비교 불필요
    CertFreeCertificateContext(cert);
    return ok;
}

// 일꾼과 부른 쪽이 같이 쥐는 상태 — 일꾼이 늦게 끝나도 살아 있게 shared_ptr 로 둔다
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;   // 일꾼이 돌아왔다
    bool closed = false;     // 핸들을 닫았다(마감 취소 또는 마무리) — 그 뒤엔 새 핸들을 안 받는다
    HINTERNET ses = NULL, con = NULL, req = NULL;
    Result res;
    std::wstring url;
    std::string name, password;
    DWORD timeoutMs = kTimeoutMs;
    bool pinned = false;                 // ★ 10-28 지문 고정
    unsigned char pin[kPinBytes] = {};

    // 새 핸들을 맡긴다. 이미 닫힌 뒤면(마감이 먼저 옴) 바로 닫고 false
    bool Keep(HINTERNET& slot, HINTERNET h)
    {
        if (!h) return false;
        std::unique_lock<std::mutex> lk(mu);
        if (closed) {
            lk.unlock();
            WinHttpCloseHandle(h);
            return false;
        }
        slot = h;
        return true;
    }

    // 핸들을 한 번만 닫는다 — 일꾼의 WinHTTP 호출이 걸려 있으면 그 호출이 오류로 돌아온다. 락 밖에서 닫는다(닫는 동안 일꾼이 Keep 을 불러도 안 막힘)
    void CloseAll()
    {
        HINTERNET r, c, s;
        {
            std::lock_guard<std::mutex> g(mu);
            if (closed) return;
            closed = true;
            r = req;
            c = con;
            s = ses;
            req = con = ses = NULL;
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
    }
};

// 일꾼이 하는 일 — 막는 호출을 순서대로. 결과는 s.res
inline void Work(Shared& s)
{
    Result& res = s.res;
    res.line = FailLine(nox::LoginFail::Unreachable);
    auto bail = [&](DWORD err) { res.error = err ? err : ERROR_WINHTTP_OPERATION_CANCELLED; };

    wchar_t host[256] = {}, path[512] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(s.url.c_str(), 0, 0, &uc)) return bail(GetLastError());
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && uc.nScheme != INTERNET_SCHEME_HTTP) return bail(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    if (s.pinned && !https) return bail(ERROR_WINHTTP_SECURE_FAILURE);   // ★ 10-28 지문을 줬는데 평문 주소 — 붙지도 않는다

    HINTERNET h = WinHttpOpen(L"NoxOrigin-Client/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    DWORD err = GetLastError();
    if (!s.Keep(s.ses, h)) return bail(err);
    h = WinHttpConnect(s.ses, host, uc.nPort, 0);
    err = GetLastError();
    if (!s.Keep(s.con, h)) return bail(err);
    h = WinHttpOpenRequest(s.con, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    err = GetLastError();
    if (!s.Keep(s.req, h)) return bail(err);
    DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(s.req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));
    const int to = static_cast<int>(s.timeoutMs);
    WinHttpSetTimeouts(s.req, to, to, to, to);   // 일꾼이 스스로도 끝나게(합 한도는 부른 쪽이 지킨다)
    if (s.pinned) {   // ★ 10-28 — 윈도의 인증서 검사(믿는 루트 · 이름 · 기한 · 용도)를 지문 비교로 갈음한다. 비교는 본문 쓰기 앞
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(s.req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) return bail(GetLastError());
    }

    std::string body = "name=" + UrlEncode(s.name) + "&password=" + UrlEncode(s.password);
    SecureZeroMemory(&s.password[0], s.password.size());   // 암호 글은 인코딩한 뒤 바로 지운다
    static const wchar_t kHeader[] = L"Content-Type: application/x-www-form-urlencoded";
    BOOL sent;
    if (s.pinned) {
        // 머리만 먼저(TLS 인사 + 요청 줄) → 서버 인증서 지문이 맞을 때만 본문(암호)을 쓴다
        sent = WinHttpSendRequest(s.req, kHeader, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(body.size()), 0);
        if (sent && !ServerCertMatches(s.req, s.pin)) {
            SecureZeroMemory(&body[0], body.size());
            return bail(ERROR_WINHTTP_SECURE_INVALID_CERT);
        }
        DWORD wrote = 0;
        if (sent) sent = WinHttpWriteData(s.req, &body[0], static_cast<DWORD>(body.size()), &wrote) && wrote == body.size();
    } else {
        sent = WinHttpSendRequest(s.req, kHeader, static_cast<DWORD>(-1), &body[0], static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    }
    err = sent ? 0 : GetLastError();
    SecureZeroMemory(&body[0], body.size());
    if (!sent) return bail(err);
    if (!WinHttpReceiveResponse(s.req, NULL)) return bail(GetLastError());

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(s.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return bail(GetLastError());
    res.status = static_cast<int>(status);

    std::string reply;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(s.req, &avail)) return bail(GetLastError());
        if (avail == 0) break;   // 본문 끝
        if (reply.size() + avail > kMaxReply) {   // 로그인 서버의 답이 아니다 — 읽지 않고 서버 오류
            res.line = FailLine(nox::LoginFail::ServerError);
            return;
        }
        char buf[512];
        DWORD got = 0;
        if (!WinHttpReadData(s.req, buf, avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf)), &got)) return bail(GetLastError());
        if (got == 0) break;
        reply.append(buf, got);
    }
    res.line = Classify(res.status, reply);
    res.error = 0;
}

}  // namespace detail

// url 예: L"https://127.0.0.1:8443/login". 막는 호출 — timeoutMs 안에 돌아온다(스레드 정리까지 조금 넘을 수 있다)
//   ★ 10-28 pinHex — 비면 옛 길(윈도 인증서 검사) · 있으면 지문 고정(머리 주석). 꼴이 틀린 지문이면 붙지 않고 unreachable(옛 길로 안 물러섬)
inline Result Login(const std::wstring& url, const std::string& name, const std::string& password, DWORD timeoutMs = kTimeoutMs,
                    const std::string& pinHex = std::string())
{
    const ULONGLONG t0 = GetTickCount64();
    auto s = std::make_shared<detail::Shared>();
    if (!pinHex.empty()) {
        if (!detail::ParsePin(pinHex, s->pin)) {
            Result bad;
            bad.line = detail::FailLine(nox::LoginFail::Unreachable);
            bad.error = ERROR_INVALID_PARAMETER;
            return bad;
        }
        s->pinned = true;
    }
    s->url = url;
    s->name = name;
    s->password = password;
    s->timeoutMs = timeoutMs;

    std::thread worker([s] {
        detail::Work(*s);
        s->CloseAll();
        std::lock_guard<std::mutex> g(s->mu);
        s->finished = true;
        s->cv.notify_all();
    });

    bool done;
    {
        std::unique_lock<std::mutex> lk(s->mu);
        done = s->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return s->finished; });
    }
    if (!done) {
        s->CloseAll();   // 마감 — 일꾼이 쥔 WinHTTP 호출이 오류로 돌아온다
        std::unique_lock<std::mutex> lk(s->mu);
        const bool back = s->cv.wait_for(lk, std::chrono::milliseconds(1000), [&] { return s->finished; });
        lk.unlock();
        if (back) worker.join();
        else worker.detach();   // 끊었는데도 안 돌아오면 기다리지 않는다(일꾼은 shared_ptr 로 상태를 쥐고 혼자 끝난다)
    } else {
        worker.join();
    }

    Result r;
    {
        std::lock_guard<std::mutex> g(s->mu);
        if (done) {
            r = s->res;
        } else {
            r.line = detail::FailLine(nox::LoginFail::Unreachable);
            r.error = ERROR_WINHTTP_TIMEOUT;
        }
        if (!s->password.empty()) SecureZeroMemory(&s->password[0], s->password.size());
    }
    r.ms = static_cast<DWORD>(GetTickCount64() - t0);
    return r;
}

}  // namespace authclient
