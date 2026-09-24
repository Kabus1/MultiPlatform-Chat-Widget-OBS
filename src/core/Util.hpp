#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace usc::util {

std::string urlEncode(std::string_view s);
std::string urlDecode(std::string_view s);
std::string toLower(std::string s);
std::string trim(std::string_view s);

// UTF-8 helpers (chat text is always UTF-8).
std::vector<char32_t> utf8Decode(std::string_view s);
std::string utf8Encode(const std::vector<char32_t>& cps, size_t begin, size_t end);
size_t utf8Length(std::string_view s);

// Splits text into pieces of at most maxChars code points, preferring
// whitespace / punctuation boundaries (Google's free TTS endpoint caps input).
std::vector<std::string> splitForSpeech(const std::string& text, size_t maxChars);

// Replaces every occurrence of `from` with `to`.
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

std::string randomToken(size_t bytes = 16);
std::string base64Decode(std::string_view in);

int64_t nowMs();
int64_t nowSec();

// Opens a URL in the user's default system browser (OAuth must not run inside
// the OBS/CEF dock because Google blocks embedded browsers).
bool openInSystemBrowser(const std::string& url);

std::string defaultConfigDir();

} // namespace usc::util
