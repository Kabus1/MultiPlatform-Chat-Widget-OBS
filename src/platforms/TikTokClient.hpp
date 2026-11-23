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

// TikTok LIVE chat (read-only), connected like a viewer's browser - no
// TikTok developer account, no API key, no signing server:
//
//   1. live check:  GET tiktok.com/api-live/user/room?uniqueId=<user>
//                   -> room id; live when liveRoom.status or user.status == 2
//   2. ttwid:       anonymous cookie from GET https://www.tiktok.com/
//                   (redirects not followed; TikTok sets it intermittently)
//   3. WebSocket:   wss://webcast-ws.tiktok.com/webcast/im/ws_proxy/
//                   ws_reuse_supplement/?room_id=..&identity=audience&...
//                   (no X-Bogus / msToken / signature), Cookie: ttwid
//   4. frames:      heartbeat + im_enter_room, then WebcastPushFrame
//                   protobufs with gzip payloads; ack when asked.
//
// "connected" means TikTok accepted us into the room (first frame after
// entering), not just that a socket opened. A signing server (e.g. Euler
// Stream) is only used as an optional fallback when an API key is set.
class TikTokClient : public PlatformClient {
public:
    struct Config {
        std::string username;   // @handle
        std::string signServer; // optional fallback, e.g. https://tiktok.eulerstream.com
        std::string apiKey;     // optional: only used with signServer
        int idleMaxMs = 300000; // not live: checks back off up to this
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
        bool found = false;     // the account exists
        bool notFound = false;  // TikTok says the user does not exist
        bool blocked = false;   // HTTP 403/429: rate-limited / blocked
        std::string error;
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
    // Signing-free WebSocket URL (viewer connection).
    static std::string buildDirectWsUrl(const std::string& roomId, double lastRtt);
    // "ttwid=...; Path=/..." Set-Cookie lines -> ttwid value ("" if absent).
    static std::string extractTtwid(const std::vector<std::string>& setCookies);
    static std::string normalizeUser(const std::string& input);
    static std::string enterRoomFrame(const std::string& roomId);

    TikTokClient(Config cfg, MessageSink onMessage, StatusSink onStatus);
    ~TikTokClient() override;

    void start() override;
    void stop() override;
    bool checkNow() override;

private:
    struct ConnectInfo {
        std::string wsUrl;
        std::string cookies;
    };

    void run();
    RoomInfo resolveRoom();
    std::string fetchTtwid(std::string& error);
    std::optional<ConnectInfo> fetchConnect(const std::string& roomId, std::string& error); // signing server
    void onFrame(const std::string& data);
    bool sleepFor(int ms); // returns early on checkNow()
    bool inBurst() const;
    int nextIdleDelay();

    Config cfg_;
    std::string user_;
    MessageSink onMessage_;
    StatusSink onStatus_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<WebSocketClient> ws_;
    std::string roomId_;
    std::string cookies_;                // signing-server mode
    std::string ttwid_;                  // direct mode
    std::atomic<bool> needFreshTtwid_{false};
    std::atomic<bool> streamEnded_{false};
    std::atomic<bool> joined_{false};    // TikTok answered after im_enter_room
    std::atomic<int64_t> lastFrameMs_{0};
    std::atomic<uint64_t> chatCount_{0};
    std::atomic<int64_t> burstUntilMs_{0};
    int idleDelayMs_ = 60000;
};

} // namespace usc
