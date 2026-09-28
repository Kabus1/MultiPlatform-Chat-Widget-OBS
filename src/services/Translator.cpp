#include "services/Translator.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <nlohmann/json.hpp>

namespace usc {

using nlohmann::json;

Translation Translator::translate(const std::string& text, const std::string& targetLang, const std::string& apiKey,
                                  const std::string& sourceLang) {
    const std::string source = sourceLang.empty() ? "auto" : sourceLang;
    const std::string key = source + '\x1f' + targetLang + '\x1f' + text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
        if (apiKey.empty() && util::nowMs() < pausedUntilMs_) {
            Translation busy;
            busy.error = "Google free translate is rate-limiting, paused";
            return busy;
        }
    }
    Translation t = apiKey.empty() ? freeEndpoint(text, targetLang, source) : cloudV2(text, targetLang, apiKey, source);
    if (t.ok && source != "auto") t.detectedLang = source;
    if (!t.ok) {
        LOG_WARN("translate", t.error);
        return t;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() > 1000) cache_.clear();
    cache_[key] = t;
    return t;
}

Translation Translator::freeEndpoint(const std::string& text, const std::string& target, const std::string& source) {
    Translation out;
    auto r = http::get("https://translate.googleapis.com/translate_a/single?" +
                       http::buildQuery({{"client", "gtx"}, {"sl", source}, {"tl", target}, {"dt", "t"}, {"q", text}}));
    if (!r.ok()) {
        out.error = r.status == 0 ? r.error : "translate HTTP " + std::to_string(r.status);
        if (r.status == 429) {
            // Too many requests: stop asking for a minute instead of hammering
            // (a Google Cloud API key in Settings avoids this limit).
            std::lock_guard<std::mutex> lock(mutex_);
            pausedUntilMs_ = util::nowMs() + 60000;
            out.error += " (rate limited - pausing translation for 60s; add a Google Cloud API key to avoid this)";
        }
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

Translation Translator::cloudV2(const std::string& text, const std::string& target, const std::string& apiKey,
                                const std::string& source) {
    Translation out;
    json body = {{"q", text}, {"target", target}, {"format", "text"}};
    if (source != "auto") body["source"] = source;
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
