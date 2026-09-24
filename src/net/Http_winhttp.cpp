// Windows backend for net/Http.hpp: WinHTTP. HTTPS uses the Windows
// certificate store and the system proxy, so no OpenSSL is needed.
#include "net/Http.hpp"

#include "core/Util.hpp"
#include "net/WinHttpCommon.hpp"

#include <mutex>
#include <vector>

namespace usc {

namespace winhttp {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

HINTERNET session() {
    static HINTERNET s = [] {
        std::wstring ua = widen(http::browserUserAgent());
        // Automatic proxy (Windows 8.1+); fall back to the classic default.
        HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)
            h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            DWORD flags = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
        }
        return h;
    }();
    return s;
}

bool parseUrl(const std::string& url, ParsedUrl& out) {
    std::string u = url;
    bool ws = false;
    if (u.rfind("wss://", 0) == 0) { u = "https://" + u.substr(6); ws = true; }
    else if (u.rfind("ws://", 0) == 0) { u = "http://" + u.substr(5); ws = true; }
    (void)ws;
    std::wstring w = widen(u);
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    c.dwHostNameLength = static_cast<DWORD>(-1);
    c.dwUrlPathLength = static_cast<DWORD>(-1);
    c.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c)) return false;
    out.host.assign(c.lpszHostName, c.dwHostNameLength);
    out.path.assign(c.lpszUrlPath, c.dwUrlPathLength);
    if (c.lpszExtraInfo) out.path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.port = c.nPort;
    out.secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

} // namespace winhttp

namespace http {

namespace {

Response request(const wchar_t* method, const std::string& url, const Headers& headers, const std::string& body,
                 const std::string& contentType, int timeoutSec) {
    using namespace winhttp;
    Response out;
    ParsedUrl u;
    if (!session() || !parseUrl(url, u)) {
        out.error = "invalid URL or WinHTTP unavailable";
        return out;
    }
    Handle conn(WinHttpConnect(session(), u.host.c_str(), u.port, 0));
    if (!conn) {
        out.error = lastError("connect");
        return out;
    }
    Handle req(WinHttpOpenRequest(conn.get(), method, u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        out.error = lastError("open request");
        return out;
    }
    int ms = timeoutSec * 1000;
    WinHttpSetTimeouts(req.get(), ms, ms, ms, ms);

    std::string hdrs;
    for (const auto& [k, v] : headers) hdrs += k + ": " + v + "\r\n";
    if (!contentType.empty()) hdrs += "Content-Type: " + contentType + "\r\n";
    std::wstring whdrs = widen(hdrs);

    if (!WinHttpSendRequest(req.get(), whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
                            whdrs.empty() ? 0 : static_cast<DWORD>(-1),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        out.error = lastError("request");
        return out;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t ct[256];
    DWORD ctSize = sizeof(ct);
    if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctSize,
                            WINHTTP_NO_HEADER_INDEX))
        out.contentType = narrow(std::wstring(ct, ctSize / sizeof(wchar_t)));

    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
            out.error = lastError("read");
            return out;
        }
        if (avail == 0) break;
        buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.get(), buf.data(), avail, &read)) {
            out.error = lastError("read");
            return out;
        }
        out.body.append(buf.data(), read);
    }
    out.status = static_cast<int>(status);
    return out;
}

Headers withUserAgent(Headers h) {
    // The session already sends a browser User-Agent; drop duplicates.
    Headers out;
    for (auto& kv : h)
        if (util::toLower(kv.first) != "user-agent") out.push_back(std::move(kv));
    return out;
}

} // namespace

const char* browserUserAgent() {
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/128.0.0.0 Safari/537.36";
}

std::string buildQuery(const Params& params) {
    std::string q;
    for (const auto& [k, v] : params) {
        if (!q.empty()) q += '&';
        q += util::urlEncode(k) + '=' + util::urlEncode(v);
    }
    return q;
}

Response get(const std::string& url, const Headers& headers, int timeoutSec) {
    return request(L"GET", url, withUserAgent(headers), {}, {}, timeoutSec);
}

Response postForm(const std::string& url, const Params& form, const Headers& headers, int timeoutSec) {
    return request(L"POST", url, withUserAgent(headers), buildQuery(form), "application/x-www-form-urlencoded", timeoutSec);
}

Response postJson(const std::string& url, const std::string& json, const Headers& headers, int timeoutSec) {
    return request(L"POST", url, withUserAgent(headers), json, "application/json", timeoutSec);
}

} // namespace http

} // namespace usc
