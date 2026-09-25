#pragma once

#include "core/ChatMessage.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace usc {

// Decides whether a chat message comes from a bot (Nightbot, BotRix,
// StreamElements, ...). Bot messages are dropped before they are shown,
// translated or read aloud.
struct BotFilter {
    // Returns the reason a message is filtered ("known bot", "bot badge", ...)
    // or an empty string when it should be kept. `bots` is settings["bots"].
    static std::string reason(const ChatMessage& m, const nlohmann::json& bots);

    // Lower-cases and strips '@', spaces and a trailing "(bot)" marker.
    static std::string normalizeName(const std::string& name);
};

} // namespace usc
