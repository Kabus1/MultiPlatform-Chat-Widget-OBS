#include "services/TtsService.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <nlohmann/json.hpp>

#include <map>

namespace usc {

using nlohmann::json;

std::optional<std::string> TtsService::synthesize(const std::string& text, const std::string& lang,
                                                  const std::string& apiKey, const std::string& voice) {
    if (text.empty()) return std::nullopt;
    const std::string key = lang + '\x1f' + voice + '\x1f' + (apiKey.empty() ? "f" : "c") + '\x1f' + text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.second);
            return it->second.first;
        }
    }
    auto audio = apiKey.empty() ? freeEndpoint(text, lang) : cloud(text, lang, apiKey, voice);
    if (!audio) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.find(key) == cache_.end()) {
        lru_.push_front(key);
        cache_[key] = {*audio, lru_.begin()};
        while (lru_.size() > 64) {
            cache_.erase(lru_.back());
            lru_.pop_back();
        }
    }
    return audio;
}

std::optional<std::string> TtsService::freeEndpoint(const std::string& text, const std::string& lang) {
    // The public endpoint accepts at most ~200 characters per request; MP3
    // frames can simply be concatenated to join the pieces.
    auto chunks = util::splitForSpeech(text, 180);
    std::string audio;
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto r = http::get("https://translate.google.com/translate_tts?" +
                           http::buildQuery({{"ie", "UTF-8"}, {"client", "tw-ob"}, {"tl", lang.empty() ? "en" : lang},
                                             {"q", chunks[i]}, {"total", std::to_string(chunks.size())},
                                             {"idx", std::to_string(i)},
                                             {"textlen", std::to_string(util::utf8Length(chunks[i]))}}),
                           {{"Referer", "https://translate.google.com/"}});
        if (!r.ok()) {
            LOG_WARN("tts", "Google TTS failed (", lang, "): ", r.status == 0 ? r.error : "HTTP " + std::to_string(r.status));
            return std::nullopt;
        }
        audio += r.body;
    }
    return audio;
}

std::string TtsService::cloudLocale(const std::string& lang) {
    static const std::map<std::string, std::string> m = {
        {"ar", "ar-XA"}, {"en", "en-US"}, {"es", "es-ES"}, {"fr", "fr-FR"}, {"de", "de-DE"},
        {"it", "it-IT"}, {"pt", "pt-BR"}, {"ru", "ru-RU"}, {"tr", "tr-TR"}, {"ja", "ja-JP"},
        {"ko", "ko-KR"}, {"zh-CN", "cmn-CN"}, {"zh-TW", "cmn-TW"}, {"hi", "hi-IN"}, {"id", "id-ID"},
        {"nl", "nl-NL"}, {"pl", "pl-PL"}, {"sv", "sv-SE"}, {"uk", "uk-UA"}, {"vi", "vi-VN"},
        {"fa", "fa-IR"}, {"ur", "ur-IN"}, {"he", "he-IL"}, {"iw", "he-IL"}, {"th", "th-TH"},
    };
    auto it = m.find(lang);
    return it == m.end() ? lang : it->second;
}

std::optional<std::string> TtsService::cloud(const std::string& text, const std::string& lang,
                                             const std::string& apiKey, const std::string& voice) {
    std::string locale = cloudLocale(lang.empty() ? "en" : lang);
    json v = {{"languageCode", locale}};
    // A configured voice only applies when it matches the language being spoken.
    if (!voice.empty() && voice.rfind(locale, 0) == 0) v["name"] = voice;
    json body = {{"input", {{"text", text}}}, {"voice", v}, {"audioConfig", {{"audioEncoding", "MP3"}}}};
    auto r = http::postJson("https://texttospeech.googleapis.com/v1/text:synthesize?key=" + apiKey, body.dump(), {}, 15);
    if (!r.ok()) {
        LOG_WARN("tts", "Cloud TTS failed: ", r.status == 0 ? r.error : r.body.substr(0, 200));
        return std::nullopt;
    }
    try {
        return util::base64Decode(json::parse(r.body).at("audioContent").get<std::string>());
    } catch (const std::exception& e) {
        LOG_WARN("tts", "bad Cloud TTS response: ", e.what());
        return std::nullopt;
    }
}

} // namespace usc
