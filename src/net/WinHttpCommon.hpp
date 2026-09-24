#pragma once
// Shared WinHTTP helpers for the Windows HTTP and WebSocket backends.

#include <windows.h>
#include <winhttp.h>

#include <string>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif

namespace usc::winhttp {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// One process-wide session (thread-safe, keeps connections alive).
HINTERNET session();

struct ParsedUrl {
    std::wstring host;
    std::wstring path; // path + query
    INTERNET_PORT port = 0;
    bool secure = false;
};
bool parseUrl(const std::string& url, ParsedUrl& out); // http, https, ws, wss

std::string lastError(const char* what);

class Handle {
public:
    explicit Handle(HINTERNET h = nullptr) : h_(h) {}
    ~Handle() { if (h_) WinHttpCloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_;
};

} // namespace usc::winhttp
