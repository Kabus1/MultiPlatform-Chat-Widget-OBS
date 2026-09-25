#include "core/MessagePipeline.hpp"

#include "core/BotFilter.hpp"
#include "core/EventHub.hpp"
#include "core/Log.hpp"
#include "core/Settings.hpp"
#include "core/Util.hpp"
#include "services/Translator.hpp"

#include <algorithm>
#include <regex>

namespace usc {

using nlohmann::json;

namespace {

// Language codes are compared on their primary subtag ("zh-CN" ~ "zh").
std::string primaryLang(const std::string& code) {
    return util::toLower(code.substr(0, code.find('-')));
}

bool ttsActiveFor(const ChatMessage& m, const json& tts) {
    if (!tts.value("enabled", false) || m.history) return false;
    if (!tts.value("platforms", json::object()).value(m.platform, true)) return false;
    std::string user = util::toLower(m.username);
    std::string display = util::toLower(m.displayName);
    for (const auto& ignored : tts.value("ignoredUsers", json::array())) {
        if (!ignored.is_string()) continue;
        std::string i = util::toLower(util::trim(ignored.get<std::string>()));
        if (!i.empty() && i[0] == '@') i.erase(0, 1);
        if (i == user || i == display || "@" + i == display) return false;
    }
    return true;
}

// Rough script-based fallback when Google's language detection is unavailable.
std::string guessLang(const std::string& text) {
    size_t arabic = 0, letters = 0;
    for (char32_t c : util::utf8Decode(text)) {
        if (c >= 0x0600 && c <= 0x06FF) ++arabic, ++letters;
        else if ((c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z')) ++letters;
    }
    return letters && arabic * 2 >= letters ? "ar" : "en";
}

} // namespace

MessagePipeline::MessagePipeline(Settings& settings, EventHub& hub, Translator& translator)
    : settings_(settings), hub_(hub), translator_(translator) {}

MessagePipeline::~MessagePipeline() { stop(); }

void MessagePipeline::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run(); });
}

void MessagePipeline::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void MessagePipeline::submit(ChatMessage m) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (m.id.empty()) m.id = m.platform + "-" + std::to_string(util::nowMs()) + "-" + std::to_string(++seq_);
        if (m.timestamp == 0) m.timestamp = util::nowMs();
        queue_.push_back(std::move(m));
    }
    cv_.notify_one();
}

void MessagePipeline::run() {
    while (true) {
        ChatMessage m;
        size_t backlog;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            m = std::move(queue_.front());
            queue_.pop_front();
            backlog = queue_.size();
        }
        json s = settings_.snapshot();
        std::string why = BotFilter::reason(m, s["bots"]);
        if (!why.empty()) {
            ++botsFiltered_;
            LOG_INFO("bots", "dropped ", m.platform, " message from ", m.displayName, " (", why, ")");
            continue; // never shown, translated or spoken
        }
        process(m, backlog);
        const json& audio = s["audio"];
        m.ttsTarget = audio.value("output", std::string("app"));
        if (!m.tts.is_null() && m.ttsTarget == "app" && !audio.value("muted", false) && speechSink_)
            speechSink_(m.tts);
        hub_.publish("chat", m.toJson(), true);
    }
}

std::string MessagePipeline::stripLinks(const std::string& text) {
    static const std::regex url(R"((https?://|www\.)\S+)", std::regex::icase);
    return util::trim(std::regex_replace(text, url, ""));
}

bool MessagePipeline::wantsLookup(const ChatMessage& m, const json& s) {
    const json& tr = s["translation"];
    bool display = tr.value("enabled", false) && (tr.value("showForEnglish", false) || tr.value("showForAllLanguages", false));
    return display || ttsActiveFor(m, s["tts"]);
}

MessagePipeline::Decision MessagePipeline::decide(const ChatMessage& m, const json& s, const std::string& speakText,
                                                  bool translated, const std::string& translation,
                                                  const std::string& detectedLang) {
    Decision d;
    const json& tr = s["translation"];
    const json& tts = s["tts"];
    const std::string target = tr.value("targetLang", std::string("ar"));
    const std::string src = primaryLang(detectedLang);
    const bool foreign = translated && !src.empty() && src != primaryLang(target) &&
                         util::toLower(util::trim(translation)) != util::toLower(util::trim(speakText));

    // Display: English message -> translated line right under it.
    if (tr.value("enabled", false) && foreign)
        d.showTranslation = (tr.value("showForEnglish", true) && src == "en") || tr.value("showForAllLanguages", false);

    if (!ttsActiveFor(m, tts)) return d;

    std::string original = speakText;
    if (tts.value("skipCommands", true) && !original.empty() && original[0] == '!') return d;
    if (tts.value("skipLinks", true)) original = stripLinks(original);

    // Skip long messages entirely (limit is on the original text).
    int maxChars = tts.value("maxChars", 0);
    if (maxChars > 0 && util::utf8Length(original) > static_cast<size_t>(maxChars)) return d;

    std::string sayText = original;
    std::string sayLang = detectedLang.empty() || detectedLang == "auto" ? guessLang(original) : detectedLang;
    if (tts.value("mode", std::string("original")) == "translated" && foreign) {
        sayText = tts.value("skipLinks", true) ? stripLinks(translation) : translation;
        sayLang = target;
    }

    json segments = json::array();
    if (tts.value("readUsername", true)) {
        std::string name = m.displayName.empty() ? m.username : m.displayName;
        if (!name.empty() && name[0] == '@') name.erase(0, 1);
        std::string tpl = tts.value("usernameTemplate", std::string("{user}:"));
        std::string said = util::trim(util::replaceAll(tpl.empty() ? "{user}" : tpl, "{user}", name));
        if (!said.empty()) segments.push_back({{"text", said}, {"lang", sayLang}});
    }
    if (tts.value("readMessage", true) && !util::trim(sayText).empty())
        segments.push_back({{"text", sayText}, {"lang", sayLang}});

    // A lone username without the message is pointless when the message was
    // dropped (e.g. only emotes/links) but the user asked to read messages.
    if (tts.value("readMessage", true) && util::trim(sayText).empty()) return d;
    if (!segments.empty()) d.tts = segments;
    return d;
}

void MessagePipeline::process(ChatMessage& m, size_t backlog) {
    json s = settings_.snapshot();
    std::string text = m.speakableText();

    bool translated = false;
    Translation t;
    // Under heavy load, skip lookups so the chat never lags behind.
    if (!text.empty() && wantsLookup(m, s) && backlog < 40) {
        t = translator_.translate(text, s["translation"].value("targetLang", std::string("ar")),
                                  s["google"].value("apiKey", std::string()));
        translated = t.ok;
    }
    m.detectedLang = t.detectedLang;
    m.translation = t.text;
    Decision d = decide(m, s, text, translated, t.text, t.detectedLang);
    m.showTranslation = d.showTranslation;
    m.tts = d.tts;
}

} // namespace usc
