#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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

    // Request results / events arrive on the WebSocket thread: handlers must
    // be quick and must not wait for another request.
    using ResponseFn = std::function<void(bool ok, const nlohmann::json& data, const std::string& error)>;
    using EventFn = std::function<void(const std::string& type, const nlohmann::json& data)>;
    using ReadyFn = std::function<void(bool ready)>; // identified (true) / connection lost (false)

    ObsWatcher(Config cfg, StreamSink onStreamState, StatusSink onStatus);
    ~ObsWatcher();

    // Set before start().
    void setEventHandler(EventFn fn) { onEvent_ = std::move(fn); }
    void setReadyHandler(ReadyFn fn) { onReady_ = std::move(fn); }

    void start();
    void stop();
    bool streaming() const { return streaming_; }
    bool ready() const { return identified_; }

    // obs-websocket v5 request (op 6). False if not connected; otherwise `cb`
    // is called exactly once: with the response, or ok=false if the
    // connection drops first.
    bool request(const std::string& type, const nlohmann::json& data, ResponseFn cb);

    // obs-websocket v5 authentication string:
    // base64(sha256(base64(sha256(password + salt)) + challenge))
    static std::string authResponse(const std::string& password, const std::string& salt, const std::string& challenge);

private:
    void onText(const std::string& payload);
    void setStreaming(bool on);
    // Reports a status only when it changes: OBS being closed must not spam
    // the log / UI on every (cheap, local) reconnect attempt.
    void report(const std::string& state, const std::string& detail);
    void failPending(const std::string& why);
    void setReady(bool ready);

    Config cfg_;
    StreamSink onStreamState_;
    StatusSink onStatus_;
    std::unique_ptr<WebSocketClient> ws_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> authFailed_{false};
    std::mutex statusMutex_;
    std::string lastStatus_;
    EventFn onEvent_;
    ReadyFn onReady_;
    std::atomic<bool> identified_{false};
    std::mutex wsMutex_; // ws_ pointer vs request() from other threads
    std::mutex pendingMutex_;
    std::map<std::string, ResponseFn> pending_;
    uint64_t nextRequest_ = 0;
};

} // namespace usc
