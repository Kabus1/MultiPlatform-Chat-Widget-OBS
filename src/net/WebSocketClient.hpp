#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace usc {

// WebSocket client with automatic reconnection (1s..30s backoff).
// Windows: native WinHTTP (TLS via SChannel, no extra libraries).
// Linux/macOS: IXWebSocket + OpenSSL.
class WebSocketClient {
public:
    struct Callbacks {
        std::function<void()> onOpen;                                   // after every (re)connect
        std::function<void(const std::string&)> onMessage;              // one complete text message
        std::function<void(const std::string&)> onBinary;               // binary message (falls back to onMessage)
        std::function<void(int code, const std::string& reason)> onClose; // connection lost; will reconnect
        std::function<void(const std::string&)> onError;                // connect / transport failure
    };

    struct Options {
        // Extra handshake headers (Origin, User-Agent, Cookie...).
        std::vector<std::pair<std::string, std::string>> headers;
        // Called before every (re)connect to get a fresh URL; empty = keep the last one.
        std::function<std::string()> urlProvider;
    };

    WebSocketClient(std::string url, Callbacks callbacks, Options options = {});
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void start();
    void stop();                              // joins the background thread; no callbacks afterwards
    bool send(const std::string& text);
    bool sendBinary(const std::string& data);
    void reconnect();                         // drop the current connection; safe from callbacks
    // Wait this long before the next reconnect attempt (e.g. after a server
    // error that says "don't come back right away"). Safe from callbacks.
    void setNextRetryDelay(int ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usc
