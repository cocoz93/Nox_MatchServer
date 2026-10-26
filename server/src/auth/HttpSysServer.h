// HttpSysServer.h — 윈도 HTTP 서버(http.sys · HTTP Server API v2) 얇은 감싸기. 로그인 서버(nox_auth)가 쓴다.
//
// 왜 http.sys 인가(Docs/LOGIN_AUTH_DESIGN.md 「플랜을 짜며 고른 것」) — 바깥 라이브러리 0 · TLS 를 윈도가 한다(Schannel).
//   대가 = 포트에 인증서 묶기 · 주소 예약을 관리자가 한 번(`tools/auth_setup.ps1`). 안 돼 있으면 `Open` 이 5(접근 거부)로 실패한다
// 요청 큐 하나를 일꾼 여럿이 같이 받는다 — `Receive` 는 막는 꼴(요청이 올 때까지 그 스레드가 잔다) · `Close` 가 모두 깨운다.
// 길 · 규칙은 모른다(`CAuthService` 몫). 머리 파일에 http.h 를 안 넣는다 — 부르는 쪽(시험)이 윈도 HTTP 머리를 모르게
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nox::auth {

struct HttpRequest {
    uint64_t id = 0;            // http.sys 요청 번호(답할 때 씀)
    std::string method;         // "GET" · "POST" …
    std::string path;           // 물음표 앞까지(예: "/login")
    std::string body;           // 한도 `kMaxBody`
    uint32_t peerIp = 0;        // 상대 IPv4(a<<24|…) — IPv6 루프백 · IPv4 매핑은 IPv4 로 · 그 밖 IPv6 은 0
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;   // ★ 조각 3 — 그 밖 머리(콘텐츠 보안 정책 등 · 이름 · 값 ASCII)
};

enum class ReceiveResult : uint8_t {
    Ok,
    TooLarge,   // 본문이 한도를 넘음 — 이미 413 으로 답했음(부르는 쪽은 다음 요청으로)
    Closed,     // `Close` 됨 — 일꾼은 끝낸다
    Error,      // 그 밖(기록하고 다음 요청으로)
};

class CHttpSysServer {
public:
    static constexpr size_t kMaxBody = 4096;   // 가입 · 로그인 폼은 수백 바이트 — 근거 없는 값(넉넉히)

    CHttpSysServer() = default;
    ~CHttpSysServer();
    CHttpSysServer(const CHttpSysServer&) = delete;
    CHttpSysServer& operator=(const CHttpSysServer&) = delete;

    // `url` 예: L"https://+:8443/". 실패면 `err`(윈도 오류 번호 · 뜻)
    bool Open(const std::wstring& url, std::string& err);
    ReceiveResult Receive(HttpRequest& out, std::string& err);
    bool Send(uint64_t id, const HttpResponse& r, std::string& err);
    // 받기 중인 일꾼을 모두 깨운다(Receive 가 Closed) · 두 번 불러도 됨. 손잡이를 닫는 것은 소멸자(일꾼을 다 거둔 뒤)
    void Close();
    // 0.0.0.0:<port> 에 인증서가 묶여 있나 — 없으면 `Open` 은 되어도 TLS 손잡기에서 클라가 끊긴다(그래서 뜰 때 미리 본다)
    static bool SslBound(uint16_t port, std::string& err);

private:
    void* queue_ = nullptr;     // HANDLE
    uint64_t session_ = 0;      // HTTP_SERVER_SESSION_ID
    uint64_t group_ = 0;        // HTTP_URL_GROUP_ID
    std::wstring url_;
    bool initialized_ = false;
    bool urlAdded_ = false;
    bool closed_ = false;
};

}  // namespace nox::auth
