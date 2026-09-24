#pragma once

#include <functional>
#include <memory>
#include <string>

namespace usc {

// Text WebSocket client with automatic reconnection (1s..30s backoff).
// Windows: native WinHTTP (TLS via SChannel, no extra libraries).
// Linux/macOS: IXWebSocket + OpenSSL.
class WebSocketClient {
public:
    struct Callbacks {
        std::function<void()> onOpen;                        // after every (re)connect
        std::function<void(const std::string&)> onMessage;   // one complete text message
        std::function<void()> onClose;                       // connection lost; will reconnect
        std::function<void(const std::string&)> onError;
    };

    WebSocketClient(std::string url, Callbacks callbacks);
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    void start();
    void stop();                          // joins the background thread; no callbacks afterwards
    bool send(const std::string& text);
    void reconnect();                     // drop the current connection; safe from callbacks

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usc
