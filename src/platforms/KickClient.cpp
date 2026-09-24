#include "platforms/KickClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <regex>

namespace usc {

using nlohmann::json;

KickClient::KickClient(Config cfg, MessageSink onMessage, StatusSink onStatus)
    : cfg_(std::move(cfg)), onMessage_(std::move(onMessage)), onStatus_(std::move(onStatus)) {}

KickClient::~KickClient() { stop(); }

void KickClient::start() {
    if (running_.exchange(true)) return;
    // Resolving the chatroom does blocking HTTP, keep it off the caller's thread.
    resolver_ = std::thread([this] { run(); });
}

void KickClient::stop() {
    if (!running_.exchange(false)) return;
    if (resolver_.joinable()) resolver_.join();
    std::unique_ptr<ix::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws = std::move(ws_);
    }
    if (ws) ws->stop();
    onStatus_("disconnected", "");
}

std::string KickClient::lookupChatroomId(std::string& error) {
    std::string slug = util::toLower(util::trim(cfg_.username));
    // Accept a pasted channel URL too.
    if (auto p = slug.find("kick.com/"); p != std::string::npos) slug = slug.substr(p + 9);
    if (auto p = slug.find_first_of("/?#"); p != std::string::npos) slug = slug.substr(0, p);
    if (slug.empty()) {
        error = "Enter your Kick username in Settings > Connections";
        return {};
    }
    for (const char* ver : {"v2", "v1"}) {
        auto r = http::get(std::string("https://kick.com/api/") + ver + "/channels/" + util::urlEncode(slug),
                           {{"Accept", "application/json"}, {"Referer", "https://kick.com/" + slug}});
        if (r.status == 404) {
            error = "Kick channel '" + slug + "' not found";
            return {};
        }
        if (!r.ok()) {
            error = r.status == 0 ? r.error : "HTTP " + std::to_string(r.status);
            continue;
        }
        try {
            json j = json::parse(r.body);
            if (j.contains("chatroom") && j["chatroom"].contains("id")) {
                const json& id = j["chatroom"]["id"];
                return id.is_number() ? std::to_string(id.get<int64_t>()) : id.get<std::string>();
            }
        } catch (...) {
            error = "Kick API returned a non-JSON page (likely Cloudflare)";
        }
    }
    error = "Could not look up chatroom (" + error +
            "). Set the Chatroom ID manually in Settings > Connections > Kick.";
    return {};
}

void KickClient::run() {
    onStatus_("connecting", "Looking up Kick channel...");
    std::string id = util::trim(cfg_.chatroomId);
    while (running_ && id.empty()) {
        std::string error;
        id = lookupChatroomId(error);
        if (id.empty()) {
            onStatus_("error", error);
            for (int i = 0; i < 300 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!running_) return;
    chatroomId_ = id;

    auto ws = std::make_unique<ix::WebSocket>();
    ws->setUrl("wss://ws-" + cfg_.pusherCluster + ".pusher.com/app/" + cfg_.pusherKey +
               "?protocol=7&client=js&version=8.4.0&flash=false");
    ws->enableAutomaticReconnection();
    ws->setMaxWaitBetweenReconnectionRetries(30000);
    ix::WebSocket* socket = ws.get(); // callbacks only run while the socket is alive
    ws->setOnMessageCallback([this, socket](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Message: onText(msg->str, *socket); break;
        case ix::WebSocketMessageType::Close:
            if (running_) onStatus_("connecting", "Disconnected, reconnecting...");
            break;
        case ix::WebSocketMessageType::Error:
            onStatus_("error", "Connection error: " + msg->errorInfo.reason);
            break;
        default: break;
        }
    });
    onStatus_("connecting", "Connecting to chatroom " + chatroomId_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    ws_ = std::move(ws);
    ws_->start();
}

void KickClient::onText(const std::string& payload, ix::WebSocket& ws) {
    json j;
    try {
        j = json::parse(payload);
    } catch (...) {
        return;
    }
    std::string event = j.value("event", "");
    auto send = [&ws](const json& out) { ws.send(out.dump()); };
    if (event == "pusher:connection_established") {
        send({{"event", "pusher:subscribe"}, {"data", {{"auth", ""}, {"channel", "chatrooms." + chatroomId_ + ".v2"}}}});
    } else if (event == "pusher_internal:subscription_succeeded") {
        onStatus_("connected", util::trim(cfg_.username).empty() ? "chatroom " + chatroomId_ : cfg_.username);
    } else if (event == "pusher:ping") {
        send({{"event", "pusher:pong"}, {"data", json::object()}});
    } else if (event == "pusher:error") {
        onStatus_("error", "Pusher error: " + j.value("data", json::object()).dump());
    } else if (event == "App\\Events\\ChatMessageEvent") {
        try {
            // Pusher double-encodes: data is a JSON string.
            json data = j["data"].is_string() ? json::parse(j["data"].get<std::string>()) : j["data"];
            if (auto m = parseChatEvent(data)) onMessage_(std::move(*m));
        } catch (const std::exception& e) {
            LOG_WARN("kick", "bad chat event: ", e.what());
        }
    }
}

std::optional<ChatMessage> KickClient::parseChatEvent(const json& data) {
    ChatMessage m;
    m.platform = "kick";
    m.id = data.value("id", "");
    const json& sender = data.value("sender", json::object());
    if (sender.contains("id")) m.userId = sender["id"].dump();
    m.username = sender.value("slug", sender.value("username", ""));
    m.displayName = sender.value("username", m.username);
    const json& identity = sender.value("identity", json::object());
    m.userColor = identity.value("color", "");
    for (const auto& b : identity.value("badges", json::array())) {
        std::string t = b.value("type", "");
        if (t == "broadcaster" || t == "moderator" || t == "vip" || t == "verified") m.roles.push_back(t);
        else if (t == "subscriber" || t == "founder" || t == "og") m.roles.push_back("subscriber");
    }
    m.timestamp = util::nowMs();

    // Emotes are inline tokens: [emote:37226:KEKW]
    std::string content = data.value("content", "");
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
