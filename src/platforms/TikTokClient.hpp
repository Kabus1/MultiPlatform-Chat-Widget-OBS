#pragma once

#include "platforms/PlatformClient.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "net/WebSocketClient.hpp"

namespace usc {

// TikTok LIVE chat (read-only), via TikTok's internal "webcast" protocol -
// the same one TikTok-Live-Connector / TikTokLive use. TikTok offers no
// public LIVE-chat API.
//
//   1. resolveRoom():   @username -> room id + live status (tiktok.com)
//   2. fetchConnect():  room id -> WebSocket URL, cursor and cookies. This
//                       needs TikTok's anti-bot signatures, so it goes through
//                       a signing server (default Euler Stream, API key).
//   3. WebSocket:       binary WebcastPushFrame protobufs, gzip payloads;
//                       we reply with ack frames and send a heartbeat every 10s.
//
// Steps 1 and 2 are the parts TikTok changes most often; they are isolated
// here so they can be fixed without touching the rest of the app.
class TikTokClient : public PlatformClient {
public:
    struct Config {
        std::string username;   // @handle
        std::string signServer; // e.g. https://tiktok.eulerstream.com
        std::string apiKey;     // signing server key
    };

    // --- protocol (static so it can be unit-tested) ------------------------
    struct PushFrame {
        uint64_t seqId = 0;
        uint64_t logId = 0;
        std::string payloadEncoding; // "pb"
        std::string payloadType;     // "msg" | "ack" | "hb" | "im_enter_room"
        std::string payload;
        std::map<std::string, std::string> headers; // e.g. compress_type=gzip
    };
    struct FetchResult {
        std::vector<std::pair<std::string, std::string>> messages; // (method, payload)
        std::string cursor;
        std::string internalExt;
        std::string pushServer;
        std::map<std::string, std::string> routeParams;
        bool needsAck = false;
        int heartbeatMs = 10000;
    };
    struct RoomInfo {
        std::string roomId;
        bool live = false;
        bool found = false;
    };

    static std::optional<PushFrame> parsePushFrame(std::string_view data);
    static std::string encodePushFrame(const PushFrame& f);
    static std::optional<FetchResult> parseFetchResult(std::string_view data);
    static std::optional<ChatMessage> parseChatMessage(std::string_view payload);
    // WebcastControlMessage: true when the stream has ended.
    static bool isStreamEnd(std::string_view payload);
    // From tiktok.com/api-live/user/room JSON or the @user/live HTML page.
    static RoomInfo parseRoomInfo(const std::string& body);
    static bool gunzip(std::string_view in, std::string& out);
    static std::string heartbeatFrame(const std::string& roomId);
    static std::string enterRoomFrame(const std::string& roomId);

    TikTokClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~TikTokClient() override;

    void start() override;
    void stop() override;

private:
    struct ConnectInfo {
        std::string wsUrl;
        std::string cookies;
    };

    void run();
    RoomInfo resolveRoom(std::string& error);
    std::optional<ConnectInfo> fetchConnect(const std::string& roomId, std::string& error);
    void onFrame(const std::string& data);
    bool sleepFor(int ms);

    Config cfg_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<WebSocketClient> ws_;
    std::string roomId_;
    std::string cookies_; // from tiktok.com + the signing server
    std::atomic<bool> streamEnded_{false};
    std::atomic<int64_t> lastFrameMs_{0};
};

} // namespace usc
