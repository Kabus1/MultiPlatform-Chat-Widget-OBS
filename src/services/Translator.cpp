#include "services/Translator.hpp"

#include "core/Log.hpp"
#include "net/Http.hpp"

#include <nlohmann/json.hpp>

namespace usc {

using nlohmann::json;

Translation Translator::translate(const std::string& text, const std::string& targetLang, const std::string& apiKey) {
    const std::string key = targetLang + '\x1f' + text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    }
    Translation t = apiKey.empty() ? freeEndpoint(text, targetLang) : cloudV2(text, targetLang, apiKey);
    if (!t.ok) {
        LOG_WARN("translate", t.error);
        return t;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() > 1000) cache_.clear();
    cache_[key] = t;
    return t;
}

Translation Translator::freeEndpoint(const std::string& text, const std::string& target) {
    Translation out;
    auto r = http::get("https://translate.googleapis.com/translate_a/single?" +
                       http::buildQuery({{"client", "gtx"}, {"sl", "auto"}, {"tl", target}, {"dt", "t"}, {"q", text}}));
    if (!r.ok()) {
        out.error = r.status == 0 ? r.error : "translate HTTP " + std::to_string(r.status);
        return out;
    }
    try {
        // [[["translated","original",...], ...], null, "en", ...]
        json j = json::parse(r.body);
        for (const auto& seg : j.at(0))
            if (seg.is_array() && !seg.empty() && seg[0].is_string()) out.text += seg[0].get<std::string>();
        if (j.size() > 2 && j[2].is_string()) out.detectedLang = j[2].get<std::string>();
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = std::string("unexpected translate response: ") + e.what();
    }
    return out;
}

Translation Translator::cloudV2(const std::string& text, const std::string& target, const std::string& apiKey) {
    Translation out;
    json body = {{"q", text}, {"target", target}, {"format", "text"}};
    auto r = http::postJson("https://translation.googleapis.com/language/translate/v2?key=" + apiKey, body.dump());
    if (!r.ok()) {
        out.error = r.status == 0 ? r.error : "Cloud Translation HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 200);
        return out;
    }
    try {
        json j = json::parse(r.body);
        const json& t = j.at("data").at("translations").at(0);
        out.text = t.value("translatedText", "");
        out.detectedLang = t.value("detectedSourceLanguage", "");
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = std::string("unexpected Cloud Translation response: ") + e.what();
    }
    return out;
}

} // namespace usc
