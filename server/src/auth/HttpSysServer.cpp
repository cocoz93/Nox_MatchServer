// HttpSysServer.cpp — http.sys 감싸기. 설명은 `HttpSysServer.h` 머리.
#include "HttpSysServer.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <http.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace nox::auth {

namespace {

const HTTPAPI_VERSION kVersion = HTTPAPI_VERSION_2;

// 윈도 오류 글은 넓은 글자로 받아 UTF-8 로(좁은 판은 949 라 UTF-8 콘솔에서 깨진다 — 10-26 실행)
std::string WinErr(const char* what, ULONG e) {
    wchar_t wmsg[256] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   wmsg, 256, nullptr);
    for (wchar_t* p = wmsg; *p; ++p)
        if (*p == L'\r' || *p == L'\n') *p = L' ';
    char msg[768] = {};
    WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, msg, sizeof(msg), nullptr, nullptr);
    return std::string(what) + " 실패 " + std::to_string(e) + " " + msg;
}

const char* Reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return status < 400 ? "OK" : "Error";
    }
}

uint32_t PeerIpv4(const SOCKADDR* a) {
    if (!a) return 0;
    if (a->sa_family == AF_INET) return ntohl(reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr);
    if (a->sa_family == AF_INET6) {
        const in6_addr& v6 = reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&v6)) return 0x7f000001u;   // ::1 → 127.0.0.1(로비는 IPv4 만 듣는다)
        if (IN6_IS_ADDR_V4MAPPED(&v6))
            return (uint32_t(v6.u.Byte[12]) << 24) | (uint32_t(v6.u.Byte[13]) << 16) | (uint32_t(v6.u.Byte[14]) << 8) | v6.u.Byte[15];
    }
    return 0;
}

}  // namespace

CHttpSysServer::~CHttpSysServer() {
    Close();
    if (urlAdded_) HttpRemoveUrlFromUrlGroup(group_, url_.c_str(), 0);
    if (group_) HttpCloseUrlGroup(group_);
    if (queue_) HttpCloseRequestQueue(static_cast<HANDLE>(queue_));
    if (session_) HttpCloseServerSession(session_);
    if (initialized_) HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
}

bool CHttpSysServer::Open(const std::wstring& url, std::string& err) {
    ULONG rc = HttpInitialize(kVersion, HTTP_INITIALIZE_SERVER, nullptr);
    if (rc != NO_ERROR) return err = WinErr("HttpInitialize", rc), false;
    initialized_ = true;
    HTTP_SERVER_SESSION_ID sid = 0;
    if ((rc = HttpCreateServerSession(kVersion, &sid, 0)) != NO_ERROR) return err = WinErr("HttpCreateServerSession", rc), false;
    session_ = sid;
    HTTP_URL_GROUP_ID gid = 0;
    if ((rc = HttpCreateUrlGroup(sid, &gid, 0)) != NO_ERROR) return err = WinErr("HttpCreateUrlGroup", rc), false;
    group_ = gid;
    HANDLE q = nullptr;
    if ((rc = HttpCreateRequestQueue(kVersion, nullptr, nullptr, 0, &q)) != NO_ERROR) return err = WinErr("HttpCreateRequestQueue", rc), false;
    queue_ = q;
    HTTP_BINDING_INFO bi = {};
    bi.Flags.Present = 1;
    bi.RequestQueueHandle = q;
    if ((rc = HttpSetUrlGroupProperty(gid, HttpServerBindingProperty, &bi, sizeof(bi))) != NO_ERROR)
        return err = WinErr("HttpSetUrlGroupProperty", rc), false;
    if ((rc = HttpAddUrlToUrlGroup(gid, url.c_str(), 0, 0)) != NO_ERROR) {
        err = WinErr("HttpAddUrlToUrlGroup", rc);
        if (rc == ERROR_ACCESS_DENIED) err += "— 주소 예약 없음(관리자로 tools/auth_setup.ps1 한 번 · 사람 1회)";
        if (rc == ERROR_ALREADY_EXISTS) err += "— 이 주소를 이미 누가 들음(nox_auth 가 둘?)";
        return false;
    }
    url_ = url;
    urlAdded_ = true;
    return true;
}

bool CHttpSysServer::SslBound(uint16_t port, std::string& err) {
    ULONG rc = HttpInitialize(kVersion, HTTP_INITIALIZE_CONFIG, nullptr);
    if (rc != NO_ERROR) return err = WinErr("HttpInitialize(설정)", rc), false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    HTTP_SERVICE_CONFIG_SSL_QUERY q = {};
    q.QueryDesc = HttpServiceConfigQueryExact;
    q.KeyDesc.pIpPort = reinterpret_cast<PSOCKADDR>(&a);
    std::vector<BYTE> out(4096);
    ULONG got = 0;
    rc = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &q, sizeof(q), out.data(), static_cast<ULONG>(out.size()), &got, nullptr);
    HttpTerminate(HTTP_INITIALIZE_CONFIG, nullptr);
    if (rc != NO_ERROR) {
        const std::string what = "인증서 묶음 찾기(0.0.0.0:" + std::to_string(port) + ")";
        err = WinErr(what.c_str(), rc) + "— 관리자로 tools/auth_setup.ps1 한 번(사람 1회)";
        return false;
    }
    return true;
}

ReceiveResult CHttpSysServer::Receive(HttpRequest& out, std::string& err) {
    std::vector<uint64_t> buf(16 * 1024 / sizeof(uint64_t));   // HTTP_REQUEST 는 8바이트 맞춤
    HTTP_REQUEST_ID rid = HTTP_NULL_ID;
    for (;;) {
        PHTTP_REQUEST r = reinterpret_cast<PHTTP_REQUEST>(buf.data());
        const ULONG cap = static_cast<ULONG>(buf.size() * sizeof(uint64_t));
        ULONG got = 0;
        ULONG rc = HttpReceiveHttpRequest(static_cast<HANDLE>(queue_), rid, HTTP_RECEIVE_REQUEST_FLAG_COPY_BODY, r, cap, &got, nullptr);
        if (rc == ERROR_MORE_DATA) {   // 머리가 길다 — 같은 요청을 큰 버퍼로 다시
            rid = r->RequestId;
            buf.resize(got / sizeof(uint64_t) + 1);
            continue;
        }
        if (rc == ERROR_CONNECTION_INVALID && rid != HTTP_NULL_ID) {   // 다시 받는 사이 상대가 끊음
            rid = HTTP_NULL_ID;
            continue;
        }
        if (rc == ERROR_OPERATION_ABORTED || rc == ERROR_INVALID_HANDLE) return ReceiveResult::Closed;
        if (rc != NO_ERROR) {
            err = WinErr("HttpReceiveHttpRequest", rc);
            return ReceiveResult::Error;
        }

        out = HttpRequest{};
        out.id = r->RequestId;
        switch (r->Verb) {
        case HttpVerbGET: out.method = "GET"; break;
        case HttpVerbPOST: out.method = "POST"; break;
        case HttpVerbHEAD: out.method = "HEAD"; break;
        default: out.method = "OTHER"; break;
        }
        out.path.assign(r->pRawUrl ? r->pRawUrl : "", r->RawUrlLength);
        const size_t qm = out.path.find('?');
        if (qm != std::string::npos) out.path.resize(qm);
        out.peerIp = PeerIpv4(r->Address.pRemoteAddress);

        bool tooLarge = false;
        for (USHORT i = 0; i < r->EntityChunkCount; ++i) {
            const HTTP_DATA_CHUNK& ch = r->pEntityChunks[i];
            if (ch.DataChunkType != HttpDataChunkFromMemory) continue;
            out.body.append(static_cast<const char*>(ch.FromMemory.pBuffer), ch.FromMemory.BufferLength);
        }
        if (r->Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) {   // 버퍼에 다 안 들어온 본문
            char more[1024];
            for (;;) {
                ULONG n = 0;
                rc = HttpReceiveRequestEntityBody(static_cast<HANDLE>(queue_), out.id, 0, more, sizeof(more), &n, nullptr);
                if (rc == NO_ERROR || rc == ERROR_HANDLE_EOF) out.body.append(more, n);
                if (out.body.size() > kMaxBody) {
                    tooLarge = true;
                    break;
                }
                if (rc != NO_ERROR) break;
            }
        }
        if (tooLarge || out.body.size() > kMaxBody) {
            HttpResponse big;
            big.status = 413;
            big.body = "too large";
            std::string e2;
            Send(out.id, big, e2);
            return ReceiveResult::TooLarge;
        }
        return ReceiveResult::Ok;
    }
}

bool CHttpSysServer::Send(uint64_t id, const HttpResponse& r, std::string& err) {
    HTTP_RESPONSE resp = {};
    resp.StatusCode = static_cast<USHORT>(r.status);
    const char* reason = Reason(r.status);
    resp.pReason = reason;
    resp.ReasonLength = static_cast<USHORT>(std::strlen(reason));
    auto& ct = resp.Headers.KnownHeaders[HttpHeaderContentType];
    ct.pRawValue = r.contentType.c_str();
    ct.RawValueLength = static_cast<USHORT>(r.contentType.size());
    static const char kNoStore[] = "no-store";   // 토큰 · 결과 화면을 브라우저 · 사이 장치가 담아 두지 않게
    auto& cc = resp.Headers.KnownHeaders[HttpHeaderCacheControl];
    cc.pRawValue = kNoStore;
    cc.RawValueLength = static_cast<USHORT>(sizeof(kNoStore) - 1);
    std::vector<HTTP_UNKNOWN_HEADER> extra(r.headers.size());
    for (size_t i = 0; i < r.headers.size(); ++i) {
        extra[i].pName = r.headers[i].first.c_str();
        extra[i].NameLength = static_cast<USHORT>(r.headers[i].first.size());
        extra[i].pRawValue = r.headers[i].second.c_str();
        extra[i].RawValueLength = static_cast<USHORT>(r.headers[i].second.size());
    }
    resp.Headers.UnknownHeaderCount = static_cast<USHORT>(extra.size());
    resp.Headers.pUnknownHeaders = extra.empty() ? nullptr : extra.data();
    HTTP_DATA_CHUNK chunk = {};
    if (!r.body.empty()) {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = const_cast<char*>(r.body.data());
        chunk.FromMemory.BufferLength = static_cast<ULONG>(r.body.size());
        resp.EntityChunkCount = 1;
        resp.pEntityChunks = &chunk;
    }
    ULONG sent = 0;
    const ULONG rc = HttpSendHttpResponse(static_cast<HANDLE>(queue_), id, 0, &resp, nullptr, &sent, nullptr, 0, nullptr, nullptr);
    if (rc != NO_ERROR) {
        err = WinErr("HttpSendHttpResponse", rc);
        return false;
    }
    return true;
}

void CHttpSysServer::Close() {
    if (queue_ && !closed_) {
        closed_ = true;
        HttpShutdownRequestQueue(static_cast<HANDLE>(queue_));   // 받기 중인 일꾼이 모두 ERROR_OPERATION_ABORTED 로 깬다
    }
}

}  // namespace nox::auth
