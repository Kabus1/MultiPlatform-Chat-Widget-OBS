#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace usc {

class EmoteRegistry;

// Text cleaning applied to everything the TTS engine is asked to say.
struct TtsTextFilter {
    struct Options {
        bool stripEmoji = true;       // Unicode emoji, symbols, box/braille art
        bool guessEmoteWords = true;  // "catJAM", "monkaS", "xqcL" style tokens
        nlohmann::json extraEmoteWords = nlohmann::json::array(); // user list
        const EmoteRegistry* registry = nullptr; // 7TV / BTTV / FFZ names
        std::string platform;         // registry lookup scope
        std::string channelId;
    };

    // True if the text contains a URL or a bare domain ("discord.gg/abc", "www.x.com").
    static bool containsLink(const std::string& text);
    static std::string stripLinks(const std::string& text);

    // Removes emotes (Kick [emote:..] tokens, YouTube :shortcodes:, known
    // emote words, third-party emotes), emoji and symbol noise, and collapses
    // whitespace. Returns what is left to be spoken (may be empty).
    static std::string clean(const std::string& text, const Options& opt);

    static bool isBuiltinEmoteWord(const std::string& word);
    static bool looksLikeEmoteCode(const std::string& word);
};

} // namespace usc
