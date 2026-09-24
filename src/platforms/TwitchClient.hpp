#pragma once

#include "platforms/PlatformClient.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "net/WebSocketClient.hpp"

namespace usc {

// Reads Twitch chat over IRC-over-WebSocket, authenticated with the user's
// OAuth token (scope chat:read).
class TwitchClient : public PlatformClient {
public:
    struct Config {
        std::string channel; // empty = logged-in user's channel
        TokenProvider tokens;
    };

    struct IrcLine {
        std::map<std::string, std::string> tags;
        std::string prefix;
        std::string command;
        std::vector<std::string> params; // last one is the trailing param
    };

    TwitchClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~TwitchClient() override;

    void start() override;
    void stop() override;

    static IrcLine parseLine(const std::string& line);
    // Builds a ChatMessage from a PRIVMSG line (exposed for tests).
    static ChatMessage toChatMessage(const IrcLine& line);

private:
    void onOpen();
    void onText(const std::string& payload);
    void handleLine(const std::string& line);
    void send(const std::string& line);

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::unique_ptr<WebSocketClient> ws_;
    std::string channel_;
    std::atomic<bool> running_{false};
    std::mutex sendMutex_;
};

} // namespace usc
