#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace usc {

struct Translation {
    bool ok = false;
    std::string text;
    std::string detectedLang; // Google language code, e.g. "en", "ar", "zh-CN"
    std::string error;
};

// Google Translate. With an API key it uses Cloud Translation v2; without one
// it uses the public translate.googleapis.com endpoint (no key needed).
class Translator {
public:
    // sourceLang "auto" lets Google detect it; "en" forces English (used for
    // chat slang Google would otherwise mis-detect).
    Translation translate(const std::string& text, const std::string& targetLang, const std::string& apiKey,
                          const std::string& sourceLang = "auto");

private:
    Translation freeEndpoint(const std::string& text, const std::string& target, const std::string& source);
    Translation cloudV2(const std::string& text, const std::string& target, const std::string& apiKey,
                        const std::string& source);

    std::mutex mutex_;
    int64_t pausedUntilMs_ = 0; // free endpoint rate-limited (HTTP 429): back off
    std::unordered_map<std::string, Translation> cache_;
};

} // namespace usc
