#include "platforms/YouTubeClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <algorithm>
#include <regex>

namespace usc {

using nlohmann::json;

namespace {

const char* kApi = "https://www.googleapis.com/youtube/v3";
const int kWaitingRecheckMs = 15000;   // no stream found: look again after this
const int kUpcomingRecheckMs = 60000;  // chat open but not live: re-detect (a different broadcast may go live)

std::string apiReason(const http::Response& r) {
    if (r.status == 0) return "network: " + r.error;
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        const json& e = j["error"];
        if (e.contains("errors") && e["errors"].is_array() && !e["errors"].empty()) {
            std::string reason = e["errors"][0].value("reason", "");
            if (!reason.empty()) return reason;
        }
        std::string msg = e.value("message", "");
        if (!msg.empty()) return msg;
    }
    return "HTTP " + std::to_string(r.status);
}

int lifecycleRank(const std::string& s) {
    // Lower is better. Only states where the chat is meaningful.
    if (s == "live") return 0;
    if (s == "liveStarting") return 1;
    if (s == "testing") return 2;  // streaming to the preview in YouTube Studio
    if (s == "ready") return 3;    // scheduled / "Stream now" waiting for the encoder
    if (s == "created") return 4;
    return 99;                     // complete, revoked, ...
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

bool YouTubeClient::checkNow() {
    if (!running_) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = true;
    }
    cv_.notify_all();
    return true;
}

bool YouTubeClient::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load() || wake_.load(); });
    wake_ = false;
    return running_;
}

std::string YouTubeClient::authMessage() {
    std::string m = cfg_.authProblem ? cfg_.authProblem() : std::string();
    return m.empty() ? "Not logged in - click Login with YouTube" : m;
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

std::optional<YouTubeClient::Broadcast> YouTubeClient::pickBroadcast(const json& response, bool active) {
    if (!response.is_object() || !response.contains("items") || !response["items"].is_array()) return std::nullopt;
    std::optional<Broadcast> best;
    int bestRank = 100;
    for (const auto& item : response["items"]) {
        const json sn = item.value("snippet", json::object());
        const json st = item.value("status", json::object());
        std::string chat = sn.value("liveChatId", "");
        if (chat.empty()) continue;
        std::string life = st.value("lifeCycleStatus", active ? std::string("live") : std::string("ready"));
        int rank = lifecycleRank(life);
        if (rank >= 99) continue;
        if (rank < bestRank) {
            bestRank = rank;
            best = Broadcast{chat, item.value("id", ""), sn.value("title", ""), active || life == "live"};
        }
    }
    return best;
}

std::string YouTubeClient::explainApiError(const std::string& reason, const std::string& channel) {
    const std::string ch = channel.empty() ? "this channel" : "channel '" + channel + "'";
    if (reason == "liveStreamingNotEnabled")
        return "Live streaming is not enabled on " + ch + " (YouTube Studio > Go live enables it; it can take 24h).";
    if (reason == "insufficientPermissions" || reason == "forbidden" || reason == "ACCESS_TOKEN_SCOPE_INSUFFICIENT")
        return "The YouTube login does not allow reading " + ch + ". Logout and Login with YouTube again, allowing access.";
    if (reason == "quotaExceeded" || reason == "dailyLimitExceeded")
        return "YouTube API daily quota used up (resets at midnight Pacific time). Raise 'Minimum poll interval' to use less.";
    if (reason == "rateLimitExceeded" || reason == "userRateLimitExceeded") return "YouTube API rate limit - slowing down.";
    if (reason == "accessNotConfigured" || reason == "SERVICE_DISABLED")
        return "YouTube Data API v3 is not enabled for the app's Google Cloud project.";
    if (reason == "liveChatDisabled") return "Live chat is turned off for this stream.";
    if (reason.rfind("network:", 0) == 0) return "No connection to YouTube (" + reason.substr(9) + ").";
    return "YouTube API error: " + reason;
}

YouTubeClient::Detection YouTubeClient::detect(const std::string& accessToken, const std::string& channel) {
    Detection d;
    const http::Headers auth = {{"Authorization", "Bearer " + accessToken}};
    auto fail = [&](const http::Response& r) {
        d.httpStatus = r.status;
        d.apiReason = apiReason(r);
        return d;
    };

    // 1. A specific video, if configured. If it has no open chat (ended,
    //    wrong id), fall through to the channel's own broadcasts instead of
    //    getting stuck on it.
    std::string videoNote;
    const std::string videoId = extractVideoId(cfg_.videoId);
    if (!videoId.empty()) {
        auto r = http::get(std::string(kApi) + "/videos?" +
                               http::buildQuery({{"part", "snippet,liveStreamingDetails"}, {"id", videoId}}),
                           auth);
        if (!r.ok()) return fail(r);
        json j = json::parse(r.body, nullptr, false);
        const json items = j.is_object() ? j.value("items", json::array()) : json::array();
        if (items.empty()) {
            videoNote = "Video '" + videoId + "' was not found. ";
        } else {
            const json ls = items[0].value("liveStreamingDetails", json::object());
            const json sn = items[0].value("snippet", json::object());
            std::string chat = ls.value("activeLiveChatId", "");
            if (!chat.empty()) {
                bool live = ls.contains("actualStartTime") && !ls.contains("actualEndTime");
                d.broadcast = Broadcast{chat, videoId, sn.value("title", ""), live};
                return d;
            }
            videoNote = ls.contains("actualEndTime") ? "Video '" + videoId + "' has ended. "
                                                      : "Video '" + videoId + "' has no open live chat. ";
        }
    }

    // 2. Live right now.
    auto active = http::get(std::string(kApi) + "/liveBroadcasts?" +
                                http::buildQuery({{"part", "snippet,status"}, {"broadcastStatus", "active"},
                                                  {"broadcastType", "all"}, {"maxResults", "10"}}),
                            auth);
    if (!active.ok()) return fail(active);
    if ((d.broadcast = pickBroadcast(json::parse(active.body, nullptr, false), true))) return d;

    // 3. About to go live / in preview / "Stream now" persistent broadcast.
    auto upcoming = http::get(std::string(kApi) + "/liveBroadcasts?" +
                                  http::buildQuery({{"part", "snippet,status"}, {"broadcastStatus", "upcoming"},
                                                    {"broadcastType", "all"}, {"maxResults", "25"}}),
                              auth);
    if (!upcoming.ok()) return fail(upcoming);
    if ((d.broadcast = pickBroadcast(json::parse(upcoming.body, nullptr, false), false))) return d;

    d.notFound = videoNote + "No live or upcoming stream on " +
                 (channel.empty() ? std::string("the logged-in channel") : "channel '" + channel + "'") +
                 ". If you stream on a different channel (Brand Account), click Logout, then Login with YouTube "
                 "and pick that channel.";
    return d;
}

void YouTubeClient::run() {
    while (running_) {
        auto token = cfg_.tokens(false);
        if (!token) {
            onStatus_("error", authMessage());
            if (!sleepFor(kWaitingRecheckMs)) return;
            continue;
        }
        const std::string channel = token->login;

        onStatus_("connecting", "Checking " + (channel.empty() ? std::string("your channel") : "'" + channel + "'") +
                                    " for a live stream...");
        Detection det = detect(token->accessToken, channel);
        if (det.httpStatus == 401) {
            // Access token expired early or was revoked: renew it once, re-check.
            token = cfg_.tokens(true);
            if (!token) {
                onStatus_("error", authMessage());
                if (!sleepFor(kWaitingRecheckMs)) return;
                continue;
            }
            det = detect(token->accessToken, channel);
        }
        if (det.httpStatus < 200 || det.httpStatus >= 300) {
            LOG_WARN("youtube", "detection failed: HTTP ", det.httpStatus, " ", det.apiReason);
            onStatus_("error", det.httpStatus == 401 ? authMessage() : explainApiError(det.apiReason, channel));
            int pause = det.apiReason == "quotaExceeded" || det.apiReason == "dailyLimitExceeded" ? 300000 : 30000;
            if (!sleepFor(pause)) return;
            continue;
        }
        if (!det.broadcast) {
            onStatus_("waiting", det.notFound + " Checking again every 15s (Reconnect checks now).");
            if (!sleepFor(kWaitingRecheckMs)) return;
            continue;
        }

        Broadcast b = *det.broadcast;
        auto announce = [&](const Broadcast& br) {
            std::string what = br.title.empty() ? std::string() : " \"" + br.title + "\"";
            onStatus_("connected", br.live ? "Live" + what + (channel.empty() ? "" : " - " + channel)
                                           : "Chat open" + what + " (stream not live yet; it will continue when you go live)");
        };
        announce(b);
        LOG_INFO("youtube", "chat ", b.chatId, " video ", b.videoId, b.live ? " (live)" : " (upcoming)");

        std::string pageToken;
        bool first = true;
        int64_t lastDetectMs = util::nowMs();
        while (running_) {
            token = cfg_.tokens(false);
            if (!token) {
                onStatus_("error", authMessage());
                break;
            }
            // While waiting in an upcoming broadcast's chat, keep checking: the
            // stream may start on a different broadcast ("Stream now" vs event).
            if (!b.live && util::nowMs() - lastDetectMs > kUpcomingRecheckMs) {
                lastDetectMs = util::nowMs();
                Detection again = detect(token->accessToken, channel);
                if (again.broadcast && again.broadcast->chatId != b.chatId && again.broadcast->live) {
                    LOG_INFO("youtube", "switching to the broadcast that went live");
                    break; // outer loop picks it up immediately
                }
                if (again.broadcast && again.broadcast->chatId == b.chatId && again.broadcast->live) {
                    b.live = true;
                    announce(b);
                }
            }

            http::Params q = {{"liveChatId", b.chatId}, {"part", "snippet,authorDetails"}, {"maxResults", "200"}};
            if (!pageToken.empty()) q.push_back({"pageToken", pageToken});
            auto r = http::get(std::string(kApi) + "/liveChat/messages?" + http::buildQuery(q),
                               {{"Authorization", "Bearer " + token->accessToken}});
            if (r.status == 401) {
                if (!cfg_.tokens(true)) {
                    onStatus_("error", authMessage());
                    break;
                }
                continue;
            }
            if (!r.ok()) {
                std::string err = apiReason(r);
                LOG_WARN("youtube", "poll failed: ", err);
                if (err == "liveChatEnded" || err == "liveChatNotFound") {
                    onStatus_("connecting", "Stream ended - looking for the next one...");
                    break; // re-detect right away
                }
                onStatus_("error", explainApiError(err, channel));
                int pause = err == "quotaExceeded" || err == "dailyLimitExceeded" ? 300000 : 10000;
                if (!sleepFor(pause)) return;
                continue;
            }

            int waitMs = cfg_.minPollMs;
            json j = json::parse(r.body, nullptr, false);
            if (j.is_object()) {
                pageToken = j.value("nextPageToken", pageToken);
                waitMs = std::max(cfg_.minPollMs, j.value("pollingIntervalMillis", 0));
                for (const auto& item : j.value("items", json::array())) {
                    const json sn = item.value("snippet", json::object());
                    const json au = item.value("authorDetails", json::object());
                    std::string type = sn.value("type", "");
                    std::string text;
                    if (type == "textMessageEvent") {
                        text = sn.value("textMessageDetails", json::object()).value("messageText", "");
                    } else if (type == "superChatEvent") {
                        const json sc = sn.value("superChatDetails", json::object());
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
            }
            first = false;
            if (!sleepFor(waitMs)) return;
        }
    }
}

} // namespace usc
