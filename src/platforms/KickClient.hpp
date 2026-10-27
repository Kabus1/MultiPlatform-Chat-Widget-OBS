#pragma once

#include "platforms/PlatformClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "net/WebSocketClient.hpp"

namespace usc {

// Kick exposes public chat through its Pusher WebSocket: no login is needed,
// only the channel's chatroom id, which we look up from the username.
//
// Connection flow:
//   1. chatroom id: manual override > cached result for this username >
//      kick.com API lookup (Cloudflare-protected, so the result is cached)
//   2. Pusher WebSocket (Origin: https://kick.com), subscribe to
//      chatrooms.{id}.v2 and chatroom_{id}
//   3. "connected" only after pusher_internal:subscription_succeeded; a
//      watchdog reports an error if that never arrives. Pusher error / close
//      codes 4000-4099 are fatal and stay visible instead of looping.
class KickClient : public PlatformClient {
public:
    struct Config {
        std::string username;
        std::string chatroomId;      // manual override
        std::string pusherKey;
        std::string pusherCluster;
        // Result of an earlier lookup (settings kick.resolved*).
        std::string cachedSlug;
        std::string cachedChatroomId;
        std::string cachedUserId;
        // Called after a successful lookup so the app can persist it.
        std::function<void(const std::string& slug, const std::string& chatroomId, const std::string& userId)> onResolved;
    };

    struct LookupResult {
        std::string chatroomId;
        std::string userId;
        std::string error;     // human-readable reason when chatroomId is empty
        bool blocked = false;  // Cloudflare challenge / bot protection
        bool notFound = false;
    };

    KickClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~KickClient() override;

    void start() override;
    void stop() override;

    // "https://kick.com/Name/" -> "name"
    static std::string normalizeSlug(const std::string& input);
    // Parses a kick.com channel / chatroom API response, or text the user
    // pasted from that page (JSON, or just the number).
    static LookupResult parseLookupResponse(int status, const std::string& body);
    // Parses a chat event payload (ChatMessageEvent v2 or legacy ChatMessageSentEvent).
    static std::optional<ChatMessage> parseChatEvent(const nlohmann::json& data);
    // Human-readable meaning of a Pusher error / close code.
    static std::string describePusherCode(int code, const std::string& message);

    // Kick's current public Pusher app (it rotates: 32cbd69e... was retired in
    // Oct 2026). Kick publishes it at web.kick.com/api/v1/realtime/connection.
    struct PusherConfig {
        std::string key;
        std::string cluster;
    };
    static constexpr const char* kDefaultPusherKey = "34bf7a0ff419a2a775b9";
    static constexpr const char* kDefaultPusherCluster = "us2";
    static bool isRetiredPusherKey(const std::string& key);
    // Parses {"data":{"connections":[{"provider":"pusher","credentials":{"app_key","cluster"}}]}}.
    static std::optional<PusherConfig> parseRealtimeConfig(const std::string& body);

private:
    void run();
    LookupResult lookup(const std::string& slug);
    void onText(const std::string& payload, WebSocketClient& ws);
    void onPusherClose(int code, const std::string& reason, WebSocketClient& ws);
    bool seen(const std::string& id);
    bool sleepFor(int ms);
    std::optional<PusherConfig> fetchPusherConfig();
    // Builds the socket URL; refreshes the key from Kick first when asked to.
    std::string pusherUrl();

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::unique_ptr<WebSocketClient> ws_;
    std::string chatroomId_;
    std::string channelUserId_; // broadcaster's Kick user id (7TV emotes)
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;

    // Subscription state, for the watchdog and status reporting.
    std::atomic<bool> subscribed_{false};
    std::atomic<int64_t> establishedAtMs_{0};
    std::atomic<bool> fatalError_{false}; // keep the error visible across reconnects

    std::mutex pusherMutex_;
    PusherConfig pusher_;                  // key / cluster in use
    std::atomic<bool> refreshPusherKey_{false};
    std::atomic<int> keyRefreshes_{0};     // consecutive 4001 rejections
    std::atomic<bool> keyErrorPending_{false};
    std::string lastKeyError_;

    std::mutex seenMutex_;
    std::unordered_set<std::string> seenIds_;
    std::deque<std::string> seenOrder_;
};

} // namespace usc
