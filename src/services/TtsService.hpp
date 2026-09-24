#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace usc {

// Google text-to-speech, returned as MP3 bytes that the dock plays.
// With an API key: Cloud Text-to-Speech (optionally a named voice, e.g.
// WaveNet). Without: Google Translate's public TTS endpoint.
class TtsService {
public:
    std::optional<std::string> synthesize(const std::string& text, const std::string& lang,
                                          const std::string& apiKey, const std::string& voice);

    // Maps Google Translate language codes to Cloud TTS locale codes.
    static std::string cloudLocale(const std::string& lang);

private:
    std::optional<std::string> freeEndpoint(const std::string& text, const std::string& lang);
    std::optional<std::string> cloud(const std::string& text, const std::string& lang,
                                     const std::string& apiKey, const std::string& voice);

    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::pair<std::string, std::list<std::string>::iterator>> cache_;
};

} // namespace usc
