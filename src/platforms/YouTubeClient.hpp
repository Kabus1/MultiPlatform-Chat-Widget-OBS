#pragma once

#include "platforms/PlatformClient.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace usc {

// Polls the YouTube Data API v3 liveChatMessages endpoint for the logged-in
// account's active broadcast (or a specific video id / URL).
class YouTubeClient : public PlatformClient {
public:
    struct Config {
        std::string videoId; // optional: id or any youtube URL
        int minPollMs = 6000;
        bool showBacklog = true;
        TokenProvider tokens;
    };

    YouTubeClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~YouTubeClient() override;

    void start() override;
    void stop() override;

    // Accepts "dQw4w9WgXcQ", "https://youtube.com/watch?v=...", "youtu.be/...", ".../live/...".
    static std::string extractVideoId(const std::string& input);

private:
    void run();
    bool sleepFor(int ms); // false when stopping
    std::string resolveLiveChatId(const std::string& accessToken, int& httpStatus);

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace usc
