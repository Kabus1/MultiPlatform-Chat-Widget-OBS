#include "core/App.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"
#include "platforms/KickClient.hpp"
#include "platforms/TwitchClient.hpp"
#include "platforms/YouTubeClient.hpp"

#include <filesystem>

namespace usc {

using nlohmann::json;

namespace {
const char* kPlatforms[] = {"twitch", "youtube", "kick"};
}

App::App(const std::string& configDir, int portOverride)
    : settings_((std::filesystem::path(configDir) / "settings.json").string()),
      tokens_((std::filesystem::path(configDir) / "tokens.json").string()),
      hub_(100),
      pipeline_(settings_, hub_, translator_) {
    settings_.load();
    tokens_.load();
    port_ = portOverride > 0 ? portOverride : settings_.snapshot()["server"].value("port", 8787);
    settings_.save(); // writes defaults on first run so users can find the file
    for (const char* p : kPlatforms) status_[p] = {{"state", "disabled"}, {"detail", ""}};
}

App::~App() { shutdown(); }

void App::start() {
    pipeline_.start();
    for (const char* p : kPlatforms) restartPlatform(p);
}

void App::shutdown() {
    std::map<std::string, std::unique_ptr<PlatformClient>> clients;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients.swap(clients_);
    }
    for (auto& [name, c] : clients) c->stop();
    pipeline_.stop();
    hub_.closeAll();
}

std::string App::redirectUri(const std::string& platform) const {
    return "http://localhost:" + std::to_string(port_) + "/auth/" + platform + "/callback";
}

std::optional<oauth::Provider> App::provider(const std::string& platform, std::string& error) {
    json s = settings_.snapshot();
    if (platform != "twitch" && platform != "youtube") {
        error = "unknown platform";
        return std::nullopt;
    }
    std::string id = util::trim(s[platform].value("clientId", ""));
    std::string secret = util::trim(s[platform].value("clientSecret", ""));
    if (id.empty() || secret.empty()) {
        error = "Enter the " + std::string(platform == "twitch" ? "Twitch" : "Google") +
                " Client ID and Client Secret first (see README: Creating API credentials)";
        return std::nullopt;
    }
    return platform == "twitch" ? oauth::twitch(id, secret, redirectUri(platform))
                                : oauth::google(id, secret, redirectUri(platform));
}

std::optional<TokenSet> App::validToken(const std::string& platform, bool forceRefresh) {
    std::lock_guard<std::mutex> lock(authMutex_);
    auto t = tokens_.get(platform);
    if (!t) return std::nullopt;
    if (!(forceRefresh || t->expiringSoon(util::nowSec()))) return t;
    if (t->refreshToken.empty()) return forceRefresh ? std::nullopt : t;

    std::string error;
    auto p = provider(platform, error);
    if (!p) return t;
    auto r = oauth::refresh(*p, t->refreshToken);
    if (!r.tokens) {
        LOG_WARN("auth", platform, " token refresh failed: ", r.error);
        // 400/401 from the token endpoint means the grant was revoked.
        if (r.error.rfind("HTTP 4", 0) == 0) {
            tokens_.erase(platform);
            return std::nullopt;
        }
        return t;
    }
    r.tokens->login = t->login;
    r.tokens->userId = t->userId;
    tokens_.set(platform, *r.tokens);
    LOG_INFO("auth", platform, " token refreshed");
    return r.tokens;
}

std::string App::beginLogin(const std::string& platform, std::string& error) {
    auto p = provider(platform, error);
    if (!p) return {};
    std::string state = util::randomToken();
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        pendingStates_[state] = platform;
    }
    std::string url = oauth::buildAuthUrl(*p, state);
    // OAuth runs in the system browser: Google refuses sign-in inside
    // embedded browsers such as the OBS dock.
    if (!util::openInSystemBrowser(url)) LOG_WARN("auth", "could not open a browser; open this URL manually:\n", url);
    return url;
}

void App::fetchAccountInfo(const std::string& platform, TokenSet& t) {
    json s = settings_.snapshot();
    if (platform == "twitch") {
        auto r = http::get("https://api.twitch.tv/helix/users",
                           {{"Authorization", "Bearer " + t.accessToken}, {"Client-Id", s["twitch"].value("clientId", "")}});
        if (r.ok()) {
            try {
                json u = json::parse(r.body)["data"][0];
                t.login = u.value("login", "");
                t.userId = u.value("id", "");
            } catch (...) {
            }
        }
    } else {
        auto r = http::get("https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true",
                           {{"Authorization", "Bearer " + t.accessToken}});
        if (r.ok()) {
            try {
                json c = json::parse(r.body)["items"][0];
                t.userId = c.value("id", "");
                t.login = c["snippet"].value("title", "");
            } catch (...) {
            }
        }
    }
}

std::string App::finishLogin(const std::string& platform, const std::string& code, const std::string& state) {
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        auto it = pendingStates_.find(state);
        if (it == pendingStates_.end() || it->second != platform) return "Invalid or expired login attempt (state mismatch). Please try again.";
        pendingStates_.erase(it);
    }
    std::string error;
    auto p = provider(platform, error);
    if (!p) return error;
    auto r = oauth::exchangeCode(*p, code);
    if (!r.tokens) return "Token exchange failed: " + r.error;
    fetchAccountInfo(platform, *r.tokens);
    tokens_.set(platform, *r.tokens);
    LOG_INFO("auth", platform, " logged in as ", r.tokens->login);

    // Logging in implies the user wants this platform on.
    if (!settings_.snapshot()[platform].value("enabled", false))
        applySettings({{platform, {{"enabled", true}}}});
    else
        restartPlatform(platform);
    return {};
}

void App::logout(const std::string& platform) {
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        tokens_.erase(platform);
    }
    restartPlatform(platform);
}

void App::setStatus(const std::string& platform, const std::string& state, const std::string& detail) {
    json snapshot;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_[platform] = {{"state", state}, {"detail", detail}};
        snapshot = status_;
    }
    LOG_INFO(platform.c_str(), state, detail.empty() ? "" : " - ", detail);
    hub_.publish("status", {{"platform", platform}, {"state", state}, {"detail", detail}});
}

json App::status() {
    json platforms;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        for (auto& [k, v] : status_) platforms[k] = v;
    }
    for (const char* p : {"twitch", "youtube"}) {
        auto t = tokens_.get(p);
        platforms[p]["loggedIn"] = t.has_value();
        platforms[p]["account"] = t ? t->login : "";
        platforms[p]["redirectUri"] = redirectUri(p);
    }
    return {{"version", APP_VERSION}, {"port", port_}, {"platforms", platforms}, {"configPath", settings_.path()}};
}

json App::applySettings(const json& patch) {
    json s = settings_.applyPatch(patch);
    // Only reconnect the platforms whose connection settings actually changed.
    for (const char* p : kPlatforms) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            changed = platformConfig_[p] != s[p];
        }
        if (changed) restartPlatform(p);
    }
    hub_.publish("settings", s);
    return s;
}

void App::reconnect(const std::string& platform) {
    for (const char* p : kPlatforms)
        if (platform.empty() || platform == p) restartPlatform(p);
}

void App::restartPlatform(const std::string& platform) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    if (auto it = clients_.find(platform); it != clients_.end()) {
        it->second->stop();
        clients_.erase(it);
    }
    json s = settings_.snapshot();
    const json& cfg = s[platform];
    platformConfig_[platform] = cfg;
    if (!cfg.value("enabled", false)) {
        setStatus(platform, "disabled", "");
        return;
    }

    MessageSink sink = [this](ChatMessage m) { pipeline_.submit(std::move(m)); };
    StatusSink status = [this, platform](const std::string& st, const std::string& d) { setStatus(platform, st, d); };
    TokenProvider tokens = [this, platform](bool force) { return validToken(platform, force); };

    std::unique_ptr<PlatformClient> client;
    if (platform == "twitch") {
        client = std::make_unique<TwitchClient>(TwitchClient::Config{cfg.value("channel", ""), tokens}, sink, status);
    } else if (platform == "youtube") {
        client = std::make_unique<YouTubeClient>(
            YouTubeClient::Config{cfg.value("videoId", ""), std::max(2000, cfg.value("minPollMs", 6000)),
                                  cfg.value("showBacklog", true), tokens},
            sink, status);
    } else {
        client = std::make_unique<KickClient>(
            KickClient::Config{cfg.value("username", ""), cfg.value("chatroomId", ""),
                               cfg.value("pusherKey", ""), cfg.value("pusherCluster", "us2")},
            sink, status);
    }
    client->start();
    clients_[platform] = std::move(client);
}

void App::injectTestMessages() {
    auto make = [](const char* platform, const char* user, const char* color, std::vector<MessagePart> parts,
                   std::vector<std::string> roles) {
        ChatMessage m;
        m.platform = platform;
        m.username = user;
        m.displayName = user;
        m.userColor = color;
        m.parts = std::move(parts);
        m.roles = std::move(roles);
        m.isTest = true;
        return m;
    };
    using T = MessagePart::Type;
    pipeline_.submit(make("twitch", "PixelKnight", "#1E90FF",
                          {{T::Text, "Hello everyone! This stream is amazing ", ""},
                           {T::Emote, "Kappa", "https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0"}},
                          {"subscriber"}));
    pipeline_.submit(make("youtube", "@سارة_العربية", "", {{T::Text, "السلام عليكم، ما هي اللعبة القادمة؟", ""}}, {"member"}));
    pipeline_.submit(make("kick", "RocketFan99", "#53FC18",
                          {{T::Text, "What settings are you using for this game? ", ""},
                           {T::Emote, "KEKW", "https://files.kick.com/emotes/37226/fullsize"}},
                          {"moderator"}));
}

void App::clearChat() {
    hub_.clearHistory();
    hub_.publish("clear", json::object());
}

} // namespace usc
