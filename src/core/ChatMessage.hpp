#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace usc {

struct MessagePart {
    enum class Type { Text, Emote };
    Type type = Type::Text;
    std::string text; // text, or emote name
    std::string url;  // emote image url
};

struct ChatMessage {
    std::string id;
    std::string platform; // "twitch" | "youtube" | "kick"
    std::string channel;
    std::string userId;
    std::string username;    // login / handle
    std::string displayName; // what is shown in chat
    std::string userColor;   // platform-provided name color (may be empty)
    std::vector<std::string> roles; // broadcaster, moderator, vip, subscriber, member, verified
    std::vector<MessagePart> parts;
    int64_t timestamp = 0;   // ms since epoch
    bool history = false;    // backlog fetched on connect: shown but never spoken
    bool isTest = false;

    // Filled in by the MessagePipeline.
    std::string detectedLang;
    std::string translation;
    bool showTranslation = false;
    nlohmann::json tts; // null, or [{text, lang}, ...] segments to be spoken by the dock

    // Full message text with emote names inline.
    std::string plainText() const;
    // Text without emotes, used for translation / TTS.
    std::string speakableText() const;

    nlohmann::json toJson() const;
};

inline std::string ChatMessage::plainText() const {
    std::string out;
    for (const auto& p : parts) out += p.text;
    return out;
}

inline std::string ChatMessage::speakableText() const {
    std::string out;
    for (const auto& p : parts) {
        if (p.type == MessagePart::Type::Text) out += p.text;
        else out += ' ';
    }
    // Collapse whitespace introduced by removed emotes.
    std::string collapsed;
    bool space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !collapsed.empty();
        } else {
            if (space) collapsed.push_back(' ');
            collapsed.push_back(c);
            space = false;
        }
    }
    return collapsed;
}

inline nlohmann::json ChatMessage::toJson() const {
    nlohmann::json parts_ = nlohmann::json::array();
    for (const auto& p : parts) {
        if (p.type == MessagePart::Type::Emote)
            parts_.push_back({{"type", "emote"}, {"name", p.text}, {"url", p.url}});
        else
            parts_.push_back({{"type", "text"}, {"text", p.text}});
    }
    return {
        {"id", id},
        {"platform", platform},
        {"channel", channel},
        {"userId", userId},
        {"username", username},
        {"displayName", displayName.empty() ? username : displayName},
        {"userColor", userColor},
        {"roles", roles},
        {"parts", parts_},
        {"timestamp", timestamp},
        {"history", history},
        {"test", isTest},
        {"lang", detectedLang},
        {"translation", showTranslation ? translation : std::string()},
        {"tts", tts},
    };
}

} // namespace usc
