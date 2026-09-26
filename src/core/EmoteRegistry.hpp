#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace usc {

// Third-party emotes (7TV, BetterTTV, FrankerFaceZ) arrive in chat as plain
// words ("KEKW", "catJAM"). The registry knows their names so the pipeline
// can render them as images and TTS can skip them.
class EmoteRegistry {
public:
    using EmoteList = std::vector<std::pair<std::string, std::string>>; // name -> image url

    ~EmoteRegistry();

    void loadGlobalAsync();
    // Loads a channel's emote sets once (twitch: 7TV+BTTV+FFZ, kick: 7TV).
    void ensureChannelAsync(const std::string& platform, const std::string& channelId);

    bool contains(const std::string& platform, const std::string& channelId, const std::string& word) const;
    std::string url(const std::string& platform, const std::string& channelId, const std::string& word) const;
    size_t size() const;

    void add(const std::string& scope, const EmoteList& emotes); // scope "" = global

    // Response parsers (exposed for tests).
    static EmoteList parse7tv(const nlohmann::json& j);   // emote set or user object
    static EmoteList parseBttv(const nlohmann::json& j);  // array or {channelEmotes, sharedEmotes}
    static EmoteList parseFfz(const nlohmann::json& j);   // {sets:{...}}

private:
    static std::string scopeKey(const std::string& platform, const std::string& channelId) {
        return platform + ":" + channelId;
    }
    void spawn(std::function<void()> fn);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> global_;
    std::map<std::string, std::unordered_map<std::string, std::string>> channels_;

    std::mutex threadsMutex_;
    std::set<std::string> requested_;
    std::vector<std::thread> threads_;
};

} // namespace usc
