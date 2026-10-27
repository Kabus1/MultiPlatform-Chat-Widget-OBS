#include "platforms/KickClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <algorithm>
#include <chrono>
#include <regex>

namespace usc {

using nlohmann::json;

namespace {

const int kSubscribeTimeoutMs = 15000;

std::string idToString(const json& v) {
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_string()) return v.get<std::string>();
    return {};
}

bool looksLikeCloudflare(const std::string& body) {
    return body.find("Just a moment") != std::string::npos || body.find("cf-chl") != std::string::npos ||
           body.find("challenge-platform") != std::string::npos || body.find("cf_chl_opt") != std::string::npos ||
           body.find("Attention Required") != std::string::npos;
}

} // namespace

KickClient::KickClient(Config cfg, MessageSink onMessage, StatusSink onStatus)
    : cfg_(std::move(cfg)), onMessage_(std::move(onMessage)), onStatus_(std::move(onStatus)) {}

KickClient::~KickClient() { stop(); }

void KickClient::start() {
    if (running_.exchange(true)) return;
    // Lookup + watchdog run on their own thread (blocking HTTP).
    worker_ = std::thread([this] { run(); });
}

void KickClient::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::unique_ptr<WebSocketClient> ws;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws = std::move(ws_);
    }
    if (ws) ws->stop();
    onStatus_("disconnected", "");
}

bool KickClient::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load(); });
    return running_;
}

std::string KickClient::normalizeSlug(const std::string& input) {
    std::string slug = util::toLower(util::trim(input));
    if (auto p = slug.find("kick.com/"); p != std::string::npos) slug = slug.substr(p + 9);
    if (!slug.empty() && slug[0] == '@') slug.erase(0, 1);
    if (auto p = slug.find_first_of("/?# "); p != std::string::npos) slug = slug.substr(0, p);
    return slug;
}

KickClient::LookupResult KickClient::parseLookupResponse(int status, const std::string& body) {
    LookupResult r;
    std::string trimmed = util::trim(body);
    if (status == 404) {
        r.notFound = true;
        r.error = "channel not found";
        return r;
    }
    if (status == 403 || status == 429 || status == 503 || looksLikeCloudflare(trimmed)) {
        r.blocked = true;
        r.error = "blocked by Kick's Cloudflare protection (HTTP " + std::to_string(status) + ")";
        return r;
    }
    // A bare number pasted by the user.
    static const std::regex digits(R"(^\d{1,12}$)");
    if (std::regex_match(trimmed, digits)) {
        r.chatroomId = trimmed;
        return r;
    }
    json j = json::parse(trimmed, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        // Pasted page text that is not pure JSON: look for the chatroom object.
        static const std::regex chatroomRe(R"re("chatroom"\s*:\s*\{[^{}]*?"id"\s*:\s*(\d+))re");
        static const std::regex userRe(R"re("user_id"\s*:\s*(\d+))re");
        std::smatch m;
        if (std::regex_search(trimmed, m, chatroomRe)) r.chatroomId = m[1];
        if (std::regex_search(trimmed, m, userRe)) r.userId = m[1];
        if (r.chatroomId.empty())
            r.error = status >= 200 && status < 300 ? "unexpected response (not JSON)"
                                                    : "HTTP " + std::to_string(status);
        return r;
    }
    // /api/v2/channels/{slug} and /api/v1/channels/{slug}
    if (j.contains("chatroom") && j["chatroom"].is_object()) r.chatroomId = idToString(j["chatroom"].value("id", json()));
    if (j.contains("user_id")) r.userId = idToString(j["user_id"]);
    // /api/v2/channels/{slug}/chatroom returns the chatroom object itself.
    if (r.chatroomId.empty() && j.contains("id") && (j.contains("chatable_type") || j.contains("chatable_id") ||
                                                     j.contains("slow_mode") || j.contains("chat_mode")))
        r.chatroomId = idToString(j["id"]);
    if (r.chatroomId.empty()) r.error = j.contains("message") ? j.value("message", std::string("no chatroom in response"))
                                                              : "no chatroom in response";
    return r;
}

KickClient::LookupResult KickClient::lookup(const std::string& slug) {
    const http::Headers headers = {
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Referer", "https://kick.com/" + slug},
        {"Sec-Fetch-Site", "same-origin"},
        {"Sec-Fetch-Mode", "cors"},
        {"Sec-Fetch-Dest", "empty"},
    };
    const std::string base = "https://kick.com/api/";
    const std::string enc = util::urlEncode(slug);
    LookupResult last;
    for (const std::string& url : {base + "v2/channels/" + enc, base + "v2/channels/" + enc + "/chatroom",
                                   base + "v1/channels/" + enc}) {
        auto resp = http::get(url, headers, 10);
        LookupResult r = resp.status == 0 ? LookupResult{{}, {}, resp.error, false, false}
                                          : parseLookupResponse(resp.status, resp.body);
        LOG_INFO("kick", "lookup ", url, " -> HTTP ", resp.status,
                 r.chatroomId.empty() ? " (" + r.error + ")" : " chatroom " + r.chatroomId);
        if (!r.chatroomId.empty() || r.notFound) return r;
        last = r;
    }
    return last;
}

std::string KickClient::describePusherCode(int code, const std::string& message) {
    std::string m = message.empty() ? "" : " - " + message;
    switch (code) {
    case 4001: return "Kick rejected the chat key even after fetching a new one" + m + ". Retrying every minute.";
    case 4003: return "Kick's Pusher app is disabled" + m;
    case 4004: return "Kick's Pusher app is over its connection quota" + m;
    case 4005: return "Pusher path not found" + m;
    case 4006: return "invalid Pusher protocol version" + m;
    case 4009: return "connection not authorised (origin rejected)" + m;
    case 4100: return "over capacity, retrying" + m;
    case 4200: return "server asked to reconnect" + m;
    case 4201: return "pong not received in time, reconnecting" + m;
    case 4202: return "connection closed after inactivity, reconnecting" + m;
    default: return "Pusher code " + std::to_string(code) + m;
    }
}

void KickClient::run() {
    const std::string slug = normalizeSlug(cfg_.username);
    std::string id = util::trim(cfg_.chatroomId);
    if (!id.empty()) {
        LOG_INFO("kick", "using manual chatroom id ", id);
    } else if (!slug.empty() && slug == cfg_.cachedSlug && !cfg_.cachedChatroomId.empty()) {
        id = cfg_.cachedChatroomId;
        channelUserId_ = cfg_.cachedUserId;
        LOG_INFO("kick", "using cached chatroom id ", id, " for ", slug);
    }

    int retryMs = 30000;
    while (running_ && id.empty()) {
        if (slug.empty()) {
            onStatus_("error", "Enter your Kick username in Settings > Connections");
            return;
        }
        onStatus_("connecting", "Looking up Kick channel '" + slug + "'...");
        LookupResult r = lookup(slug);
        // Some Kick slugs use '-' where the username has '_'.
        if (r.notFound && slug.find('_') != std::string::npos) {
            std::string dashed = slug;
            for (char& c : dashed)
                if (c == '_') c = '-';
            r = lookup(dashed);
        }
        if (!r.chatroomId.empty()) {
            id = r.chatroomId;
            channelUserId_ = r.userId;
            if (cfg_.onResolved) cfg_.onResolved(slug, id, r.userId);
            break;
        }
        if (r.notFound) {
            onStatus_("error", "Kick channel '" + slug + "' does not exist - check the username");
            return;
        }
        onStatus_("error", r.blocked ? "Kick blocked the automatic lookup (Cloudflare). Click \"Find chatroom ID\" "
                                       "in Settings > Connections > Kick - it only has to be done once."
                                     : "Kick lookup failed: " + r.error + ". Retrying...");
        if (!sleepFor(retryMs)) return;
        retryMs = std::min(retryMs * 2, 300000);
    }
    if (!running_) return;
    chatroomId_ = id;

    // Callbacks only run while the client is alive (stop() joins its thread).
    auto socket = std::make_shared<WebSocketClient*>(nullptr);
    WebSocketClient::Callbacks cb;
    cb.onOpen = [this] {
        subscribed_ = false;
        establishedAtMs_ = 0;
    };
    cb.onMessage = [this, socket](const std::string& text) { onText(text, **socket); };
    cb.onClose = [this, socket](int code, const std::string& reason) { onPusherClose(code, reason, **socket); };
    cb.onError = [this](const std::string& reason) {
        subscribed_ = false;
        if (!fatalError_) onStatus_("error", "Cannot reach Kick chat server: " + reason);
    };
    WebSocketClient::Options opt;
    // Same handshake as the kick.com web client.
    opt.headers = {{"Origin", "https://kick.com"}, {"User-Agent", http::browserUserAgent()}};
    // Kick's chat key rotates: ask Kick for the current one first; fall back to
    // the configured key (unless it is a retired one) or the built-in default.
    {
        PusherConfig chosen;
        if (auto live = fetchPusherConfig()) {
            chosen = *live;
        } else {
            std::string key = util::trim(cfg_.pusherKey);
            chosen.key = key.empty() || isRetiredPusherKey(key) ? kDefaultPusherKey : key;
            chosen.cluster = util::trim(cfg_.pusherCluster).empty() ? kDefaultPusherCluster : util::trim(cfg_.pusherCluster);
        }
        std::lock_guard<std::mutex> lock(pusherMutex_);
        pusher_ = chosen;
    }
    // Each reconnect rebuilds the URL, re-fetching the key after a 4001.
    opt.urlProvider = [this] { return pusherUrl(); };
    auto ws = std::make_unique<WebSocketClient>(pusherUrl(), std::move(cb), std::move(opt));
    *socket = ws.get();
    onStatus_("connecting", "Connecting to Kick chat (chatroom " + chatroomId_ + ")");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        ws_ = std::move(ws);
        ws_->start();
    }

    // Watchdog: connected to Pusher but the subscription is never confirmed.
    while (sleepFor(1000)) {
        int64_t since = establishedAtMs_;
        if (!subscribed_ && since > 0 && util::nowMs() - since > kSubscribeTimeoutMs) {
            establishedAtMs_ = 0;
            onStatus_("error", "Kick chat did not confirm the subscription to chatroom " + chatroomId_ +
                                   ". If this keeps happening, the chatroom ID may be wrong: use \"Find chatroom ID\".");
            std::lock_guard<std::mutex> lock(mutex_);
            if (ws_) ws_->reconnect();
        }
    }
}

bool KickClient::isRetiredPusherKey(const std::string& key) {
    return key == "32cbd69e4b950bf97679" || key == "eb1d5f283081a78b932c";
}

std::optional<KickClient::PusherConfig> KickClient::parseRealtimeConfig(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("data") || !j["data"].is_object()) return std::nullopt;
    const json& conns = j["data"].value("connections", json::array());
    static const std::regex keyRe(R"(^[A-Za-z0-9]{8,64}$)");
    static const std::regex clusterRe(R"(^[a-z0-9-]{2,20}$)");
    for (const auto& c : conns) {
        if (!c.is_object() || c.value("provider", std::string()) != "pusher") continue;
        const json cred = c.value("credentials", json::object());
        if (!cred.is_object()) continue;
        PusherConfig p;
        p.key = cred.value("app_key", std::string());
        p.cluster = cred.value("cluster", std::string());
        if (std::regex_match(p.key, keyRe) && std::regex_match(p.cluster, clusterRe)) return p;
    }
    return std::nullopt;
}

std::optional<KickClient::PusherConfig> KickClient::fetchPusherConfig() {
    // Kick's web client negotiates its realtime transport here; asking for
    // Pusher only returns the current public Pusher app key. No login needed.
    json body = {{"client", {{"id", util::randomToken(16)}, {"type", "web"}}},
                 {"capabilities", {{"accepted_providers", json::array({{{"provider", "pusher"}}})}}}};
    auto r = http::postJson("https://web.kick.com/api/v1/realtime/connection", body.dump(),
                            {{"Accept", "application/json"}, {"Origin", "https://kick.com"}, {"Referer", "https://kick.com/"}},
                            10);
    auto cfg = r.ok() ? parseRealtimeConfig(r.body) : std::nullopt;
    if (cfg)
        LOG_INFO("kick", "chat key from Kick: ", cfg->key, " (", cfg->cluster, ")");
    else
        LOG_WARN("kick", "could not get the current chat key from Kick (",
                 r.status == 0 ? r.error : "HTTP " + std::to_string(r.status), "), using the built-in one");
    return cfg;
}

std::string KickClient::pusherUrl() {
    if (refreshPusherKey_.exchange(false)) {
        auto live = fetchPusherConfig();
        std::lock_guard<std::mutex> lock(pusherMutex_);
        if (live && live->key != pusher_.key) {
            pusher_ = *live;
        } else if (pusher_.key != kDefaultPusherKey) {
            // Kick did not answer (or returned the rejected key): try the built-in one.
            pusher_ = {kDefaultPusherKey, kDefaultPusherCluster};
        }
    }
    std::lock_guard<std::mutex> lock(pusherMutex_);
    return "wss://ws-" + pusher_.cluster + ".pusher.com/app/" + pusher_.key +
           "?protocol=7&client=js&version=8.4.0&flash=false";
}

void KickClient::onPusherClose(int code, const std::string& reason, WebSocketClient& ws) {
    subscribed_ = false;
    establishedAtMs_ = 0;
    if (!running_) return;
    LOG_WARN("kick", "chat socket closed: code ", code, reason.empty() ? "" : " (" + reason + ")");
    // 4001 = app key unknown: Kick rotated its key. Fetch the new one and retry
    // right away, a couple of times, before reporting it as a hard error.
    const bool keyRejected = keyErrorPending_.exchange(false) || code == 4001;
    if (keyRejected) {
        if (keyRefreshes_++ < 2) {
            refreshPusherKey_ = true;
            ws.setNextRetryDelay(1000);
            onStatus_("connecting", "Kick changed its chat key - getting the new one...");
            return;
        }
        code = 4001; // still rejected after refreshing: report it below
    }
    // Pusher protocol: 4000-4099 = don't retry the same way, 4100-4199 = retry
    // after a pause, 4200-4299 = retry now. Keep fatal errors on screen.
    if (code >= 4000 && code <= 4099) {
        fatalError_ = true;
        refreshPusherKey_ = keyRejected; // keep asking Kick for a new key on each retry
        onStatus_("error", describePusherCode(code, reason.empty() && keyRejected ? lastKeyError_ : reason));
        ws.setNextRetryDelay(60000);
        return;
    }
    if (code >= 4100 && code <= 4199) ws.setNextRetryDelay(5000);
    if (!fatalError_)
        onStatus_("connecting", code >= 4000 ? describePusherCode(code, reason) : "Disconnected, reconnecting...");
}

bool KickClient::seen(const std::string& id) {
    if (id.empty()) return false;
    std::lock_guard<std::mutex> lock(seenMutex_);
    if (!seenIds_.insert(id).second) return true;
    seenOrder_.push_back(id);
    if (seenOrder_.size() > 500) {
        seenIds_.erase(seenOrder_.front());
        seenOrder_.pop_front();
    }
    return false;
}

void KickClient::onText(const std::string& payload, WebSocketClient& ws) {
    json j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    const std::string event = j.value("event", "");
    // Pusher double-encodes: "data" is usually a JSON string.
    auto data = [&]() -> json {
        if (!j.contains("data")) return json::object();
        if (j["data"].is_string()) {
            json d = json::parse(j["data"].get<std::string>(), nullptr, false);
            return d.is_discarded() ? json::object() : d;
        }
        return j["data"];
    };
    auto send = [&ws](const json& out) { ws.send(out.dump()); };

    if (event == "pusher:connection_established") {
        establishedAtMs_ = util::nowMs();
        // Both the current and the older channel name; duplicates are dropped.
        for (const std::string& ch : {"chatrooms." + chatroomId_ + ".v2", "chatroom_" + chatroomId_})
            send({{"event", "pusher:subscribe"}, {"data", {{"auth", ""}, {"channel", ch}}}});
        if (!fatalError_) onStatus_("connecting", "Joining Kick chatroom " + chatroomId_ + "...");
    } else if (event == "pusher_internal:subscription_succeeded") {
        if (!subscribed_.exchange(true)) {
            fatalError_ = false;
            keyRefreshes_ = 0;
            onStatus_("connected", (util::trim(cfg_.username).empty() ? std::string("chatroom ") : cfg_.username + " - chatroom ") +
                                       chatroomId_);
        }
    } else if (event == "pusher:ping") {
        send({{"event", "pusher:pong"}, {"data", json::object()}});
    } else if (event == "pusher:error") {
        json d = data();
        int code = d.is_object() ? d.value("code", 0) : 0;
        std::string msg = d.is_object() ? d.value("message", std::string()) : d.dump();
        LOG_WARN("kick", "pusher error ", code, ": ", msg);
        if (code == 4001) {
            // Rotated key: handled when the socket closes (fetch new key, retry).
            keyErrorPending_ = true;
            lastKeyError_ = msg;
            return;
        }
        if (code >= 4000 && code <= 4099) fatalError_ = true;
        onStatus_("error", describePusherCode(code, msg));
    } else if (event == "pusher:subscription_error" || event == "pusher_internal:subscription_error") {
        onStatus_("error", "Kick refused the chatroom subscription: " + data().dump());
    } else if (event == "App\\Events\\ChatMessageEvent" || event == "App\\Events\\ChatMessageSentEvent") {
        try {
            if (auto m = parseChatEvent(data())) {
                if (seen(m->id)) return;
                m->channelId = channelUserId_;
                onMessage_(std::move(*m));
            }
        } catch (const std::exception& e) {
            LOG_WARN("kick", "bad chat event: ", e.what());
        }
    }
}

std::optional<ChatMessage> KickClient::parseChatEvent(const json& in) {
    // Legacy ChatMessageSentEvent: {message:{id, message, ...}, user:{username, ...}}
    json data = in;
    if (in.contains("message") && in["message"].is_object() && in.contains("user")) {
        const json& msg = in["message"];
        const json& user = in["user"];
        data = {{"id", idToString(msg.value("id", json()))},
                {"content", msg.value("message", std::string())},
                {"sender", {{"id", user.value("id", json())},
                            {"username", user.value("username", std::string())},
                            {"slug", user.value("slug", user.value("username", std::string()))},
                            {"identity", user.value("identity", json::object())}}}};
    }

    ChatMessage m;
    m.platform = "kick";
    m.id = idToString(data.value("id", json()));
    const json sender = data.value("sender", json::object());
    if (sender.contains("id")) m.userId = idToString(sender["id"]);
    m.username = sender.value("slug", sender.value("username", std::string()));
    m.displayName = sender.value("username", m.username);
    const json identity = sender.value("identity", json::object());
    if (identity.is_object()) {
        m.userColor = identity.value("color", std::string());
        for (const auto& b : identity.value("badges", json::array())) {
            std::string t = b.value("type", "");
            if (t == "broadcaster" || t == "moderator" || t == "vip" || t == "verified") m.roles.push_back(t);
            else if (t == "subscriber" || t == "founder" || t == "og") m.roles.push_back("subscriber");
            else if (t == "bot") m.roles.push_back("bot");
        }
    }
    m.timestamp = util::nowMs();

    // Emotes are inline tokens: [emote:37226:KEKW]
    std::string content = data.value("content", std::string());
    static const std::regex emoteRe(R"(\[emote:(\d+):([^\]]*)\])");
    auto begin = std::sregex_iterator(content.begin(), content.end(), emoteRe);
    size_t cursor = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const auto& match = *it;
        size_t pos = static_cast<size_t>(match.position(0));
        if (pos > cursor) m.parts.push_back({MessagePart::Type::Text, content.substr(cursor, pos - cursor), ""});
        m.parts.push_back({MessagePart::Type::Emote, match[2].str(),
                           "https://files.kick.com/emotes/" + match[1].str() + "/fullsize"});
        cursor = pos + static_cast<size_t>(match.length(0));
    }
    if (cursor < content.size()) m.parts.push_back({MessagePart::Type::Text, content.substr(cursor), ""});
    if (m.parts.empty()) return std::nullopt;
    return m;
}

} // namespace usc
