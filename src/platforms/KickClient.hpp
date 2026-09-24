#pragma once

#include "platforms/PlatformClient.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "net/WebSocketClient.hpp"

namespace usc {

// Kick exposes public chat through its Pusher WebSocket: no login is needed,
// only the channel's chatroom id, which we look up from the username.
class KickClient : public PlatformClient {
public:
    struct Config {
        std::string username;
        std::string chatroomId; // optional override
        std::string pusherKey;
        std::string pusherCluster;
    };

    KickClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~KickClient() override;

    void start() override;
    void stop() override;

    // Parses the ChatMessageEvent payload (exposed for tests).
    static std::optional<ChatMessage> parseChatEvent(const nlohmann::json& data);

private:
    void run();
    std::string lookupChatroomId(std::string& error);
    void onText(const std::string& payload, WebSocketClient& ws);

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::unique_ptr<WebSocketClient> ws_;
    std::string chatroomId_;
    std::thread resolver_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
};

} // namespace usc
