#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace usc {

class WebSocketClient;

// Listens to OBS through its built-in WebSocket server (obs-websocket v5,
// OBS 28+: Tools > WebSocket Server Settings). Only local traffic: no API
// quota, no polling of any streaming platform. When OBS starts streaming,
// onStreamState(true) lets the platform clients check for the new stream
// right away instead of polling while idle.
class ObsWatcher {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 4455;
        std::string password; // empty when authentication is off in OBS
    };
    using StreamSink = std::function<void(bool streaming)>;
    using StatusSink = std::function<void(const std::string& state, const std::string& detail)>;

    ObsWatcher(Config cfg, StreamSink onStreamState, StatusSink onStatus);
    ~ObsWatcher();

    void start();
    void stop();
    bool streaming() const { return streaming_; }

    // obs-websocket v5 authentication string:
    // base64(sha256(base64(sha256(password + salt)) + challenge))
    static std::string authResponse(const std::string& password, const std::string& salt, const std::string& challenge);

private:
    void onText(const std::string& payload);
    void setStreaming(bool on);
    // Reports a status only when it changes: OBS being closed must not spam
    // the log / UI on every (cheap, local) reconnect attempt.
    void report(const std::string& state, const std::string& detail);

    Config cfg_;
    StreamSink onStreamState_;
    StatusSink onStatus_;
    std::unique_ptr<WebSocketClient> ws_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> authFailed_{false};
    std::mutex statusMutex_;
    std::string lastStatus_;
};

} // namespace usc
