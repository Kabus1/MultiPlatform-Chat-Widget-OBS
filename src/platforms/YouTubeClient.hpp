#pragma once

#include "platforms/PlatformClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace usc {

// Reads YouTube live chat through the YouTube Data API v3 (liveChatMessages)
// for the logged-in channel's broadcast, or a specific video id / URL.
//
// Stream detection, in order:
//   1. the configured video (if any) - an ended video never blocks 2-4
//   2. liveBroadcasts?broadcastStatus=active      (live now)
//   3. liveBroadcasts?broadcastStatus=upcoming    (preview / starting /
//      "Stream now" persistent broadcast): its chat is already open
//   4. nothing: "waiting", re-checked every 15s or immediately on Reconnect
class YouTubeClient : public PlatformClient {
public:
    struct Config {
        std::string videoId; // optional: id or any youtube URL
        int minPollMs = 6000;
        bool showBacklog = true;
        TokenProvider tokens;
        // Why no token is available (not logged in, login expired, ...).
        std::function<std::string()> authProblem;
    };

    struct Broadcast {
        std::string chatId;
        std::string videoId;
        std::string title;
        bool live = false;   // false = chat open, stream not started yet
    };

    YouTubeClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~YouTubeClient() override;

    void start() override;
    void stop() override;
    bool checkNow() override;

    // Accepts "dQw4w9WgXcQ", "https://youtube.com/watch?v=...", "youtu.be/...", ".../live/...".
    static std::string extractVideoId(const std::string& input);
    // Picks the best broadcast with an open chat from a liveBroadcasts.list
    // response (exposed for tests).
    static std::optional<Broadcast> pickBroadcast(const nlohmann::json& response, bool active);
    // Turns a YouTube API error reason into an actionable message.
    static std::string explainApiError(const std::string& reason, const std::string& channel);

private:
    struct Detection {
        std::optional<Broadcast> broadcast;
        int httpStatus = 200;    // first failing HTTP status, 200 if all calls succeeded
        std::string apiReason;   // YouTube error reason when httpStatus is not 2xx
        std::string notFound;    // why nothing was found (when no broadcast)
    };

    void run();
    bool sleepFor(int ms); // false when stopping; returns early on checkNow()
    Detection detect(const std::string& accessToken, const std::string& channel);
    std::string authMessage();

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace usc
