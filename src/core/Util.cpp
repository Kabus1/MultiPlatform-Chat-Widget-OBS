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

std::string base64UrlEncode(std::string_view bytes) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t val = 0;
    int bits = -6;
    for (unsigned char c : bytes) {
        val = (val << 8) | c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(chars[((val << 8) >> (bits + 8)) & 0x3F]);
    return out;
}

std::string sha256(std::string_view data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    std::string msg(data);
    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + i * 4);
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
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
