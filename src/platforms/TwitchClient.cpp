#include "platforms/TwitchClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"

#include <algorithm>
#include <sstream>

namespace usc {

namespace {

std::string unescapeTag(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            switch (n) {
            case 's': out.push_back(' '); break;
            case ':': out.push_back(';'); break;
            case 'r': out.push_back('\r'); break;
            case 'n': out.push_back('\n'); break;
            default: out.push_back(n); break;
            }
        } else {
            out.push_back(v[i]);
        }
    }
    return out;
}

std::string tag(const TwitchClient::IrcLine& l, const std::string& key) {
    auto it = l.tags.find(key);
    return it == l.tags.end() ? std::string() : it->second;
}

} // namespace

TwitchClient::TwitchClient(Config cfg, MessageSink onMessage, StatusSink onStatus)
    : cfg_(std::move(cfg)), onMessage_(std::move(onMessage)), onStatus_(std::move(onStatus)) {}

TwitchClient::~TwitchClient() { stop(); }

void TwitchClient::start() {
    if (running_.exchange(true)) return;
    auto token = cfg_.tokens(false);
    if (!token) {
        onStatus_("error", "Not logged in - use Settings > Connections > Login with Twitch");
        running_ = false;
        return;
    }
    channel_ = util::toLower(util::trim(cfg_.channel.empty() ? token->login : cfg_.channel));
    if (!channel_.empty() && channel_[0] == '#') channel_.erase(0, 1);
    if (channel_.empty()) {
        onStatus_("error", "No channel configured");
        running_ = false;
        return;
    }

    WebSocketClient::Callbacks cb;
    cb.onOpen = [this] { onOpen(); };
    cb.onMessage = [this](const std::string& text) { onText(text); };
    cb.onClose = [this] {
        if (running_) onStatus_("connecting", "Disconnected, reconnecting...");
    };
    cb.onError = [this](const std::string& reason) { onStatus_("error", "Connection error: " + reason); };
    ws_ = std::make_unique<WebSocketClient>("wss://irc-ws.chat.twitch.tv:443", std::move(cb));
    onStatus_("connecting", "Connecting to #" + channel_);
    ws_->start();
}

void TwitchClient::stop() {
    if (!running_.exchange(false)) return;
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    onStatus_("disconnected", "");
}

void TwitchClient::send(const std::string& line) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (ws_) ws_->send(line + "\r\n");
}

void TwitchClient::onOpen() {
    // Re-read the token on every (re)connect so refreshed tokens are used.
    auto token = cfg_.tokens(false);
    if (!token) {
        onStatus_("error", "Not logged in");
        return;
    }
    send("CAP REQ :twitch.tv/tags twitch.tv/commands");
    send("PASS oauth:" + token->accessToken);
    send("NICK " + util::toLower(token->login.empty() ? "justinfan12345" : token->login));
    send("JOIN #" + channel_);
}

void TwitchClient::onText(const std::string& payload) {
    std::istringstream in(payload);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handleLine(line);
    }
}

TwitchClient::IrcLine TwitchClient::parseLine(const std::string& raw) {
    IrcLine l;
    size_t pos = 0;
    auto nextToken = [&]() {
        size_t sp = raw.find(' ', pos);
        std::string t = raw.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
        pos = sp == std::string::npos ? raw.size() : sp + 1;
        while (pos < raw.size() && raw[pos] == ' ') ++pos;
        return t;
    };
    if (pos < raw.size() && raw[pos] == '@') {
        std::string tags = nextToken().substr(1);
        std::istringstream ts(tags);
        std::string kv;
        while (std::getline(ts, kv, ';')) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) l.tags[kv] = "";
            else l.tags[kv.substr(0, eq)] = unescapeTag(kv.substr(eq + 1));
        }
    }
    if (pos < raw.size() && raw[pos] == ':') l.prefix = nextToken().substr(1);
    l.command = nextToken();
    while (pos < raw.size()) {
        if (raw[pos] == ':') {
            l.params.push_back(raw.substr(pos + 1));
            break;
        }
        l.params.push_back(nextToken());
    }
    return l;
}

ChatMessage TwitchClient::toChatMessage(const IrcLine& l) {
    ChatMessage m;
    m.platform = "twitch";
    m.channel = l.params.empty() ? "" : l.params[0];
    if (!m.channel.empty() && m.channel[0] == '#') m.channel.erase(0, 1);
    std::string text = l.params.size() > 1 ? l.params.back() : "";

    // /me messages arrive as CTCP ACTION.
    if (text.rfind("\x01" "ACTION ", 0) == 0) {
        text = text.substr(8);
        if (!text.empty() && text.back() == '\x01') text.pop_back();
    }

    m.id = tag(l, "id");
    m.userId = tag(l, "user-id");
    m.channelId = tag(l, "room-id");
    m.username = l.prefix.substr(0, l.prefix.find('!'));
    m.displayName = tag(l, "display-name");
    if (m.displayName.empty()) m.displayName = m.username;
    m.userColor = tag(l, "color");
    auto ts = tag(l, "tmi-sent-ts");
    m.timestamp = ts.empty() ? util::nowMs() : std::stoll(ts);

    std::string badges = tag(l, "badges");
    auto has = [&](const char* b) { return badges.find(b) != std::string::npos; };
    if (has("broadcaster/")) m.roles.push_back("broadcaster");
    if (has("moderator/")) m.roles.push_back("moderator");
    if (has("vip/")) m.roles.push_back("vip");
    if (has("subscriber/") || has("founder/")) m.roles.push_back("subscriber");
    if (has("partner/")) m.roles.push_back("verified");
    if (has("bot-badge/")) m.roles.push_back("bot");

    // emotes tag: "25:0-4,12-16/1902:6-10" (code point offsets, inclusive).
    struct Range { size_t start, end; std::string id; };
    std::vector<Range> ranges;
    std::istringstream es(tag(l, "emotes"));
    std::string emote;
    while (std::getline(es, emote, '/')) {
        auto colon = emote.find(':');
        if (colon == std::string::npos) continue;
        std::string id = emote.substr(0, colon);
        std::istringstream rs(emote.substr(colon + 1));
        std::string r;
        while (std::getline(rs, r, ',')) {
            auto dash = r.find('-');
            if (dash == std::string::npos) continue;
            try {
                ranges.push_back({std::stoul(r.substr(0, dash)), std::stoul(r.substr(dash + 1)), id});
            } catch (...) {
            }
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });

    auto cps = util::utf8Decode(text);
    size_t cursor = 0;
    for (const auto& r : ranges) {
        if (r.start < cursor || r.end >= cps.size()) continue;
        if (r.start > cursor) m.parts.push_back({MessagePart::Type::Text, util::utf8Encode(cps, cursor, r.start), ""});
        m.parts.push_back({MessagePart::Type::Emote, util::utf8Encode(cps, r.start, r.end + 1),
                           "https://static-cdn.jtvnw.net/emoticons/v2/" + r.id + "/default/dark/1.0"});
        cursor = r.end + 1;
    }
    if (cursor < cps.size()) m.parts.push_back({MessagePart::Type::Text, util::utf8Encode(cps, cursor, cps.size()), ""});
    return m;
}

void TwitchClient::handleLine(const std::string& raw) {
    IrcLine l = parseLine(raw);
    if (l.command == "PING") {
        send("PONG :" + (l.params.empty() ? std::string("tmi.twitch.tv") : l.params.back()));
    } else if (l.command == "PRIVMSG") {
        onMessage_(toChatMessage(l));
    } else if (l.command == "ROOMSTATE" || l.command == "366") {
        onStatus_("connected", "#" + channel_);
    } else if (l.command == "RECONNECT") {
        LOG_INFO("twitch", "server requested reconnect");
        if (ws_) ws_->reconnect();
    } else if (l.command == "NOTICE") {
        std::string text = l.params.empty() ? "" : l.params.back();
        if (text.find("authentication failed") != std::string::npos ||
            text.find("Improperly formatted auth") != std::string::npos) {
            LOG_WARN("twitch", "auth failed, refreshing token");
            onStatus_("error", "Authentication failed - refreshing token");
            if (!cfg_.tokens(true)) onStatus_("error", "Session expired - please log in with Twitch again");
            if (ws_) ws_->reconnect(); // reconnection re-sends PASS with the new token
        }
    } else if (l.command == "CLEARCHAT" || l.command == "CLEARMSG") {
        // Moderation events could be forwarded to hide messages; not needed yet.
    }
}

} // namespace usc
