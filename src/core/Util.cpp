#include "core/Util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace usc::util {

std::string urlEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<char32_t> utf8Decode(std::string_view s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string utf8Encode(const std::vector<char32_t>& cps, size_t begin, size_t end) {
    std::string out;
    end = std::min(end, cps.size());
    for (size_t i = begin; i < end; ++i) {
        char32_t cp = cps[i];
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
    return out;
}

size_t utf8Length(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::vector<std::string> splitForSpeech(const std::string& text, size_t maxChars) {
    std::vector<std::string> chunks;
    auto cps = utf8Decode(text);
    size_t pos = 0;
    auto isBreak = [](char32_t c) {
        return c == U' ' || c == U'\n' || c == U'\t' || c == U'.' || c == U',' || c == U'!' ||
               c == U'?' || c == U';' || c == U':' || c == U'،' /* Arabic comma */ ||
               c == U'؟' /* Arabic question mark */ || c == U'。';
    };
    while (pos < cps.size()) {
        size_t end = std::min(pos + maxChars, cps.size());
        if (end < cps.size()) {
            size_t cut = end;
            while (cut > pos + maxChars / 2 && !isBreak(cps[cut - 1])) --cut;
            if (cut > pos + maxChars / 2) end = cut;
        }
        std::string piece = trim(utf8Encode(cps, pos, end));
        if (!piece.empty()) chunks.push_back(std::move(piece));
        pos = end;
    }
    return chunks;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string randomToken(size_t bytes) {
    // random_device is backed by the OS CSPRNG (rand_s / BCrypt on Windows,
    // getrandom or /dev/urandom elsewhere); only used for OAuth state values.
    std::random_device rd;
    std::vector<unsigned char> buf(bytes);
    for (auto& b : buf) b = static_cast<unsigned char>(rd() & 0xFF);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

std::string base64Decode(std::string_view in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '-') c = '+';
        if (c == '_') c = '/';
        auto p = chars.find(static_cast<char>(c));
        if (p == std::string::npos) continue; // skips '=' and whitespace
        val = (val << 6) + static_cast<int>(p);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t nowSec() { return nowMs() / 1000; }

bool openInSystemBrowser(const std::string& url) {
#ifdef _WIN32
    auto r = reinterpret_cast<intptr_t>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
#else
    // The URL is built by us (fixed provider endpoint + url-encoded params), but
    // quote it anyway so the shell never interprets '&'.
    std::string quoted = "'" + replaceAll(url, "'", "'\\''") + "'";
#ifdef __APPLE__
    std::string cmd = "open " + quoted;
#else
    std::string cmd = "xdg-open " + quoted + " >/dev/null 2>&1 &";
#endif
    return std::system(cmd.c_str()) == 0;
#endif
}

std::string defaultConfigDir() {
    namespace fs = std::filesystem;
    fs::path base;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) base = fs::path(appdata) / "UnifiedStreamChat";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) base = fs::path(home) / "Library/Application Support/UnifiedStreamChat";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) base = fs::path(xdg) / "unified-stream-chat";
    else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".config/unified-stream-chat";
#endif
    if (base.empty()) base = fs::current_path() / "config";
    return base.string();
}

} // namespace usc::util
