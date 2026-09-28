#include "core/Settings.hpp"

#include "core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <functional>

namespace usc {

using nlohmann::json;

Settings::Settings(std::string path) : path_(std::move(path)), data_(defaults()) {}

json Settings::defaults() {
    return {
        {"server", {{"port", 8787}}},
        {"twitch", {
            {"enabled", false},
            {"clientId", ""},
            {"clientSecret", ""},
            {"channel", ""}, // empty = the logged-in account's own channel
        }},
        {"youtube", {
            {"enabled", false},
            {"clientId", ""},
            {"clientSecret", ""},
            {"videoId", ""},   // empty = the logged-in account's active broadcast
            {"minPollMs", 6000}, // protects the 10k/day API quota
            {"showBacklog", true},
        }},
        {"kick", {
            {"enabled", false},
            {"username", ""},
            {"chatroomId", ""}, // optional manual override if Kick's API is blocked
            {"pusherKey", "32cbd69e4b950bf97679"},
            {"pusherCluster", "us2"},
        }},
        {"google", {
            // Optional. Empty = Google's free public Translate / TTS endpoints.
            // Set a Google Cloud API key to use the official Cloud Translation
            // v2 + Cloud Text-to-Speech APIs instead.
            {"apiKey", ""},
            {"cloudVoice", ""}, // e.g. "ar-XA-Wavenet-B" (Cloud TTS only)
        }},
        {"translation", {
            {"enabled", true},
            {"targetLang", "ar"},
            {"showForEnglish", true},       // English message -> Arabic line below it
            {"showForAllLanguages", false}, // any language != target gets a line
            {"preserveArabic", true},       // Arabic (any dialect) is never translated or rewritten
            {"expandSlang", true},          // "idk" -> "I don't know" before translating
            {"customSlang", json::array()}, // extra "abbr=expansion" entries
        }},
        {"tts", {
            {"enabled", false},
            {"readUsername", true},
            {"readMessage", true},
            {"mode", "original"},          // "original" | "translated"
            {"usernameTemplate", "{user}:"},
            {"maxChars", 200},             // longer messages are skipped entirely (0 = no limit)
            {"skipCommands", true},        // messages starting with '!'
            {"skipLinkMessages", true},    // never read a message that contains a link
            {"skipLinks", true},           // (if the above is off) remove URLs from the text
            {"skipBots", true},            // never read bots, even if the bot filter display is off
            {"stripEmoji", true},          // remove emoji / symbols before speaking
            {"guessEmoteWords", true},     // drop "catJAM"-style emote codes (Twitch/Kick)
            {"emoteWords", json::array()}, // extra words never to read
            {"volume", 1.0},
            {"rate", 1.0},
            {"maxQueue", 10},
            {"platforms", {{"twitch", true}, {"youtube", true}, {"kick", true}}},
            {"ignoredUsers", json::array()}, // real users who should not be read aloud
        }},
        {"audio", {
            // "app": the program plays TTS itself on the chosen output device
            //        (works with the window closed and never doubles up).
            // "browser": the dock / window page plays it (old behaviour).
            {"output", "app"},
            {"deviceName", ""},   // empty = system default output device
            {"deviceId", ""},     // backend device id (preferred match; name is the fallback)
            {"channel", "both"},  // "both" | "left" | "right"
            {"muted", false},
        }},
        {"bots", {
            // Messages from bots are dropped completely: not shown, not read.
            {"enabled", true},
            {"knownBots", json::array({
                "nightbot", "streamelements", "streamlabs", "moobot", "fossabot", "wizebot",
                "botrix", "botrixoficial", "kickbot", "sery_bot", "soundalerts", "commanderroot",
                "streamholics", "pretzelrocks", "own3d", "deepbot", "phantombot", "coebot",
                "ankhbot", "streamlootsbot", "lolrankbot", "creatisbot", "kofistreambot",
                "tangiabot", "blerp", "songlistbot", "restreambot", "mixitupbot", "botisimo",
                "kicklet", "kickbotapp", "throneappbot"})},
            {"customBots", json::array()},     // user-added names
            {"allowedUsers", json::array()},   // never treated as bots
            {"nameSuffixRule", true},          // names ending in "bot" / "_bot" count as bots
            {"useBadges", true},               // platform "bot" badges
            {"hideCommands", false},           // also hide "!command" messages from viewers
        }},
        {"window", {
            {"width", 440},
            {"height", 780},
            {"alwaysOnTop", false},
        }},
        {"ui", {
            {"theme", "dark"},
            {"fontSize", 15},
            {"showLogos", true},
            {"showPlatformColors", true},
            {"useUserColors", true},
            {"showTimestamps", false},
            {"showBadges", true},
            {"thirdPartyEmotes", true}, // 7TV / BTTV / FFZ emotes shown as images
            {"maxMessages", 150},
            {"animate", true},
            {"colors", {{"twitch", "#9146FF"}, {"youtube", "#FF0033"}, {"kick", "#53FC18"}}},
        }},
    };
}

json Settings::sanitize(const json& in) {
    // Start from defaults and copy over only known keys with matching types.
    json out = defaults();
    if (!in.is_object()) return out;
    std::function<void(json&, const json&)> merge = [&](json& dst, const json& src) {
        for (auto it = dst.begin(); it != dst.end(); ++it) {
            if (!src.contains(it.key())) continue;
            const json& v = src.at(it.key());
            json& d = it.value();
            if (d.is_object() && v.is_object()) {
                // Objects of free-form keys (colors, platforms) and fixed ones.
                merge(d, v);
            } else if (d.is_array() && v.is_array()) {
                d = v;
            } else if (d.is_boolean() && v.is_boolean()) {
                d = v;
            } else if (d.is_number() && v.is_number()) {
                d = d.is_number_integer() ? json(v.get<int64_t>()) : json(v.get<double>());
            } else if (d.is_string() && v.is_string()) {
                d = v;
            }
        }
    };
    merge(out, in);
    return out;
}

void Settings::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream f(path_);
    if (!f) {
        data_ = defaults();
        return;
    }
    try {
        json j = json::parse(f);
        data_ = sanitize(j);
    } catch (const std::exception& e) {
        LOG_WARN("settings", "failed to parse ", path_, ": ", e.what(), " (using defaults)");
        data_ = defaults();
    }
}

bool Settings::save() const {
    json copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = data_;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << copy.dump(2);
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec) LOG_WARN("settings", "save failed: ", ec.message());
    return !ec;
}

json Settings::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}

json Settings::applyPatch(const json& patch) {
    json result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        json merged = data_;
        merged.merge_patch(patch);
        data_ = sanitize(merged);
        result = data_;
    }
    save();
    return result;
}

} // namespace usc
