#pragma once

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
    Translation translate(const std::string& text, const std::string& targetLang, const std::string& apiKey);

private:
    Translation freeEndpoint(const std::string& text, const std::string& target);
    Translation cloudV2(const std::string& text, const std::string& target, const std::string& apiKey);

    std::mutex mutex_;
    std::unordered_map<std::string, Translation> cache_;
};

} // namespace usc
