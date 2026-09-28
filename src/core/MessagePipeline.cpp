#include "core/MessagePipeline.hpp"

#include "core/BotFilter.hpp"
#include "core/EmoteRegistry.hpp"
#include "core/TtsTextFilter.hpp"
#include "core/EventHub.hpp"
#include "core/LanguageTools.hpp"
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

MessagePipeline::MessagePipeline(Settings& settings, EventHub& hub, Translator& translator, EmoteRegistry* emotes)
    : settings_(settings), hub_(hub), translator_(translator), emotes_(emotes) {}

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

std::string MessagePipeline::stripLinks(const std::string& text) { return TtsTextFilter::stripLinks(text); }

void MessagePipeline::markThirdPartyEmotes(ChatMessage& m, const EmoteRegistry& emotes) {
    std::vector<MessagePart> out;
    for (auto& part : m.parts) {
        if (part.type != MessagePart::Type::Text) {
            out.push_back(std::move(part));
            continue;
        }
        // Split on spaces, keeping them, and swap whole-word emote names.
        std::string pending;
        const std::string& t = part.text;
        size_t i = 0;
        while (i < t.size()) {
            size_t j = t.find(' ', i);
            if (j == std::string::npos) j = t.size();
            std::string word = t.substr(i, j - i);
            std::string url = word.empty() ? std::string() : emotes.url(m.platform, m.channelId, word);
            if (!url.empty()) {
                if (!pending.empty()) out.push_back({MessagePart::Type::Text, pending, ""});
                pending.clear();
                out.push_back({MessagePart::Type::Emote, word, url});
            } else {
                pending += word;
            }
            if (j < t.size()) pending += ' ';
            i = j + 1;
        }
        if (!pending.empty()) out.push_back({MessagePart::Type::Text, pending, ""});
    }
    m.parts = std::move(out);
}

bool MessagePipeline::wantsLookup(const ChatMessage& m, const json& s) {
    const json& tr = s["translation"];
    bool display = tr.value("enabled", false) && (tr.value("showForEnglish", false) || tr.value("showForAllLanguages", false));
    return display || ttsActiveFor(m, s["tts"]);
}

MessagePipeline::Decision MessagePipeline::decide(const ChatMessage& m, const json& s, const std::string& speakText,
                                                  bool translated, const std::string& translation,
                                                  const std::string& detectedLang, const EmoteRegistry* emotes,
                                                  const std::string& normalizedText) {
    Decision d;
    const json& tr = s["translation"];
    const json& tts = s["tts"];
    const std::string target = tr.value("targetLang", std::string("ar"));
    // Arabic (any dialect) is kept exactly as written: never translated,
    // never replaced by a "standard" rewrite, always spoken with the Arabic voice.
    const bool arabicText = tr.value("preserveArabic", true) && lang::isArabicText(speakText);
    const std::string src = arabicText ? std::string("ar") : primaryLang(detectedLang);
    // What was actually sent to Google (slang expanded), for the echo check.
    const std::string basis = normalizedText.empty() ? speakText : normalizedText;
    const std::string trNorm = util::toLower(util::trim(translation));
    const bool foreign = !arabicText && translated && !src.empty() && src != primaryLang(target) &&
                         !trNorm.empty() && trNorm != util::toLower(util::trim(basis)) &&
                         trNorm != util::toLower(util::trim(speakText));

    // Display: English message -> translated line right under it.
    if (tr.value("enabled", false) && foreign)
        d.showTranslation = (tr.value("showForEnglish", true) && src == "en") || tr.value("showForAllLanguages", false);

    if (!ttsActiveFor(m, tts)) return d;

    // Bots are never read, even when the display filter is switched off.
    if (tts.value("skipBots", true)) {
        json bots = s["bots"];
        bots["enabled"] = true;
        bots["hideCommands"] = false;
        if (!BotFilter::reason(m, bots).empty()) return d;
    }

    std::string original = util::trim(speakText);
    if (tts.value("skipCommands", true) && !original.empty() && original[0] == '!') return d;

    // Any message with a link is skipped entirely (not just the link).
    if (tts.value("skipLinkMessages", true) &&
        (TtsTextFilter::containsLink(m.plainText()) || TtsTextFilter::containsLink(speakText)))
        return d;
    if (tts.value("skipLinks", true)) original = TtsTextFilter::stripLinks(original);

    // Remove emotes / emoji so they are never spoken.
    TtsTextFilter::Options fo;
    fo.stripEmoji = tts.value("stripEmoji", true);
    fo.guessEmoteWords = tts.value("guessEmoteWords", true) && m.platform != "youtube";
    fo.extraEmoteWords = tts.value("emoteWords", json::array());
    fo.registry = emotes;
    fo.platform = m.platform;
    fo.channelId = m.channelId;
    original = TtsTextFilter::clean(original, fo);

    // Skip long messages entirely (limit is on the text that would be read).
    int maxChars = tts.value("maxChars", 0);
    if (maxChars > 0 && util::utf8Length(original) > static_cast<size_t>(maxChars)) return d;

    std::string sayText = original;
    std::string sayLang = arabicText ? std::string("ar")
                          : detectedLang.empty() || detectedLang == "auto" ? guessLang(original) : detectedLang;
    // English slang is read in full ("idk" -> "I don't know") in original mode.
    if (!arabicText && src == "en" && !normalizedText.empty() && normalizedText != speakText) {
        std::string expanded = TtsTextFilter::clean(
            tts.value("skipLinks", true) ? TtsTextFilter::stripLinks(normalizedText) : normalizedText, fo);
        if (!expanded.empty()) sayText = expanded;
    }
    if (tts.value("mode", std::string("original")) == "translated" && foreign) {
        std::string tr2 = tts.value("skipLinks", true) ? TtsTextFilter::stripLinks(translation) : translation;
        sayText = TtsTextFilter::clean(tr2, fo);
        sayLang = target;
    }

    json segments = json::array();
    if (tts.value("readUsername", true)) {
        TtsTextFilter::Options nameOpt; // names: drop emoji only, never guess emote words
        nameOpt.stripEmoji = fo.stripEmoji;
        nameOpt.guessEmoteWords = false;
        std::string name = m.displayName.empty() ? m.username : m.displayName;
        if (!name.empty() && name[0] == '@') name.erase(0, 1);
        name = TtsTextFilter::clean(util::replaceAll(name, "_", " "), nameOpt);
        std::string tpl = tts.value("usernameTemplate", std::string("{user}:"));
        std::string said = name.empty() ? std::string()
                                        : util::trim(util::replaceAll(tpl.empty() ? "{user}" : tpl, "{user}", name));
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
    if (emotes_ && s["ui"].value("thirdPartyEmotes", true)) {
        emotes_->ensureChannelAsync(m.platform, m.channelId);
        markThirdPartyEmotes(m, *emotes_);
    }
    std::string text = m.speakableText();
    const json& tr = s["translation"];
    const std::string target = tr.value("targetLang", std::string("ar"));
    const std::string apiKey = s["google"].value("apiKey", std::string());

    // Arabic text is recognised locally and never sent for translation.
    const bool arabic = tr.value("preserveArabic", true) && lang::isArabicText(text);

    // English slang / abbreviations are expanded before translating, so the
    // Arabic line shows a real translation instead of the abbreviation echoed back.
    lang::SlangResult slang;
    slang.text = text;
    if (!arabic && tr.value("expandSlang", true) && lang::isLatinOnly(text))
        slang = lang::expandSlang(text, tr.value("customSlang", json::array()));

    bool translated = false;
    Translation t;
    // Under heavy load, skip lookups so the chat never lags behind.
    if (!arabic && !text.empty() && wantsLookup(m, s) && backlog < 40) {
        const bool forceEnglish = slang.replaced > 0 && slang.englishLike;
        t = translator_.translate(slang.text, target, apiKey, forceEnglish ? "en" : "auto");
        // Short slang is often mis-detected (e.g. as Indonesian); retry as English.
        if (t.ok && !forceEnglish && slang.replaced > 0 && primaryLang(t.detectedLang) != "en" &&
            primaryLang(t.detectedLang) != primaryLang(target)) {
            Translation en = translator_.translate(slang.text, target, apiKey, "en");
            if (en.ok) t = en;
        }
        translated = t.ok;
    }
    m.detectedLang = arabic ? std::string("ar") : t.detectedLang;
    // Clearly English slang stays English even if the translation call failed.
    if (m.detectedLang.empty() && slang.replaced > 0 && slang.englishLike) m.detectedLang = "en";
    m.translation = t.text;
    Decision d = decide(m, s, text, translated, t.text, m.detectedLang, emotes_,
                        slang.replaced > 0 ? slang.text : std::string());
    m.showTranslation = d.showTranslation;
    m.tts = d.tts;
}

} // namespace usc
