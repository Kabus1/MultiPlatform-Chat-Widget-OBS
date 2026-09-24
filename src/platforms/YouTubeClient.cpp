#include "platforms/YouTubeClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <regex>

namespace usc {

using nlohmann::json;

namespace {
const char* kApi = "https://www.googleapis.com/youtube/v3";

std::string apiError(const http::Response& r) {
    if (r.status == 0) return r.error;
    try {
        json j = json::parse(r.body);
        const auto& e = j.at("error");
        std::string reason;
        if (e.contains("errors") && !e["errors"].empty()) reason = e["errors"][0].value("reason", "");
        return reason.empty() ? e.value("message", "HTTP " + std::to_string(r.status)) : reason;
    } catch (...) {
        return "HTTP " + std::to_string(r.status);
    }
}
} // namespace

YouTubeClient::YouTubeClient(Config cfg, MessageSink onMessage, StatusSink onStatus)
    : cfg_(std::move(cfg)), onMessage_(std::move(onMessage)), onStatus_(std::move(onStatus)) {}

YouTubeClient::~YouTubeClient() { stop(); }

void YouTubeClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void YouTubeClient::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    onStatus_("disconnected", "");
}

bool YouTubeClient::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load(); });
    return running_;
}

std::string YouTubeClient::extractVideoId(const std::string& input) {
    std::string s = util::trim(input);
    static const std::regex patterns[] = {
        std::regex(R"([?&]v=([A-Za-z0-9_-]{11}))"),
        std::regex(R"(youtu\.be/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/(?:live|shorts|embed)/([A-Za-z0-9_-]{11}))"),
    };
    std::smatch m;
    for (const auto& re : patterns)
        if (std::regex_search(s, m, re)) return m[1];
    return s;
}

std::string YouTubeClient::resolveLiveChatId(const std::string& accessToken, int& httpStatus) {
    http::Headers auth = {{"Authorization", "Bearer " + accessToken}};
    http::Response r;
    std::string videoId = extractVideoId(cfg_.videoId);
    if (!videoId.empty()) {
        r = http::get(std::string(kApi) + "/videos?" +
                          http::buildQuery({{"part", "liveStreamingDetails"}, {"id", videoId}}),
                      auth);
    } else {
        r = http::get(std::string(kApi) + "/liveBroadcasts?" +
                          http::buildQuery({{"part", "snippet"}, {"broadcastStatus", "active"},
                                            {"broadcastType", "all"}, {"maxResults", "5"}}),
                      auth);
    }
    httpStatus = r.status;
    if (!r.ok()) {
        onStatus_("error", "YouTube API: " + apiError(r));
        return {};
    }
    try {
        json j = json::parse(r.body);
        for (const auto& item : j.value("items", json::array())) {
            std::string id = videoId.empty()
                                 ? item["snippet"].value("liveChatId", "")
                                 : item.value("liveStreamingDetails", json::object()).value("activeLiveChatId", "");
            if (!id.empty()) return id;
        }
    } catch (const std::exception& e) {
        LOG_WARN("youtube", "bad broadcast response: ", e.what());
    }
    return {};
}

void YouTubeClient::run() {
    while (running_) {
        auto token = cfg_.tokens(false);
        if (!token) {
            onStatus_("error", "Not logged in - use Settings > Connections > Login with YouTube");
            if (!sleepFor(15000)) return;
            continue;
        }

        onStatus_("connecting", "Looking for an active live stream...");
        int status = 0;
        std::string chatId = resolveLiveChatId(token->accessToken, status);
        if (status == 401) {
            cfg_.tokens(true);
            if (!sleepFor(2000)) return;
            continue;
        }
        if (chatId.empty()) {
            if (status >= 200 && status < 300)
                onStatus_("waiting", cfg_.videoId.empty() ? "No active live stream - retrying in 30s"
                                                           : "That video has no active live chat - retrying in 30s");
            if (!sleepFor(30000)) return;
            continue;
        }

        onStatus_("connected", token->login.empty() ? "Live chat" : token->login);
        std::string pageToken;
        bool first = true;
        while (running_) {
            token = cfg_.tokens(false);
            if (!token) break;
            http::Params q = {{"liveChatId", chatId}, {"part", "snippet,authorDetails"}, {"maxResults", "200"}};
            if (!pageToken.empty()) q.push_back({"pageToken", pageToken});
            auto r = http::get(std::string(kApi) + "/liveChat/messages?" + http::buildQuery(q),
                               {{"Authorization", "Bearer " + token->accessToken}});
            if (r.status == 401) {
                cfg_.tokens(true);
                continue;
            }
            if (!r.ok()) {
                std::string err = apiError(r);
                LOG_WARN("youtube", "poll failed: ", err);
                if (err == "liveChatEnded" || err == "liveChatNotFound" || err == "liveChatDisabled") {
                    onStatus_("waiting", "Live chat ended");
                    break;
                }
                if (err == "quotaExceeded" || err == "rateLimitExceeded") {
                    onStatus_("error", "YouTube API quota exceeded - pausing 5 minutes");
                    if (!sleepFor(300000)) return;
                    continue;
                }
                onStatus_("error", "YouTube: " + err);
                if (!sleepFor(10000)) return;
                continue;
            }

            int waitMs = cfg_.minPollMs;
            try {
                json j = json::parse(r.body);
                pageToken = j.value("nextPageToken", pageToken);
                waitMs = std::max(cfg_.minPollMs, j.value("pollingIntervalMillis", 0));
                for (const auto& item : j.value("items", json::array())) {
                    const json& sn = item.value("snippet", json::object());
                    const json& au = item.value("authorDetails", json::object());
                    std::string type = sn.value("type", "");
                    std::string text;
                    if (type == "textMessageEvent") {
                        text = sn.value("textMessageDetails", json::object()).value("messageText", "");
                    } else if (type == "superChatEvent") {
                        const json& sc = sn.value("superChatDetails", json::object());
                        text = "[" + sc.value("amountDisplayString", std::string("Super Chat")) + "] " +
                               sc.value("userComment", "");
                    } else if (type == "superStickerEvent") {
                        text = "[" + sn.value("superStickerDetails", json::object()).value("amountDisplayString", std::string("Super Sticker")) + "]";
                    } else {
                        continue; // memberships, deletions, polls...
                    }
                    if (first && !cfg_.showBacklog) continue;

                    ChatMessage m;
                    m.platform = "youtube";
                    m.id = item.value("id", "");
                    m.userId = au.value("channelId", "");
                    m.displayName = au.value("displayName", "");
                    m.username = m.displayName;
                    m.timestamp = util::nowMs();
                    m.history = first;
                    if (au.value("isChatOwner", false)) m.roles.push_back("broadcaster");
                    if (au.value("isChatModerator", false)) m.roles.push_back("moderator");
                    if (au.value("isChatSponsor", false)) m.roles.push_back("member");
                    if (au.value("isVerified", false)) m.roles.push_back("verified");
                    m.parts.push_back({MessagePart::Type::Text, text, ""});
                    onMessage_(std::move(m));
                }
            } catch (const std::exception& e) {
                LOG_WARN("youtube", "bad chat response: ", e.what());
            }
            first = false;
            if (!sleepFor(waitMs)) return;
        }
    }
}

} // namespace usc
