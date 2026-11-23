#include "core/App.hpp"

#include "core/BuiltinCredentials.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"
#include "platforms/KickClient.hpp"
#include "platforms/TikTokClient.hpp"
#include "platforms/TwitchClient.hpp"
#include "platforms/YouTubeClient.hpp"

#include <filesystem>

namespace usc {

using nlohmann::json;

namespace {
const char* kPlatforms[] = {"twitch", "youtube", "kick", "tiktok"};

// Connection-relevant part of a platform's settings: cached lookup results
// (kick.resolved*) are written by the client itself and must not trigger a
// reconnect.
json connectionConfig(json cfg) {
    if (cfg.is_object())
        for (const char* k : {"resolvedSlug", "resolvedChatroomId", "resolvedUserId"}) cfg.erase(k);
    return cfg;
}
} // namespace

App::App(const std::string& configDir, int portOverride)
    : settings_((std::filesystem::path(configDir) / "settings.json").string()),
      tokens_((std::filesystem::path(configDir) / "tokens.json").string()),
      hub_(100),
      pipeline_(settings_, hub_, translator_, &emotes_),
      audio_(settings_, tts_) {
    pipeline_.setSpeechSink([this](const json& segments) { audio_.enqueue(segments); });
    settings_.load();
    tokens_.load();
    port_ = portOverride > 0 ? portOverride : settings_.snapshot()["server"].value("port", 8787);
    // Kick retired its old Pusher key: replace it in existing settings files.
    if (KickClient::isRetiredPusherKey(settings_.snapshot()["kick"].value("pusherKey", "")))
        settings_.applyPatch({{"kick", {{"pusherKey", KickClient::kDefaultPusherKey},
                                        {"pusherCluster", KickClient::kDefaultPusherCluster}}}});
    settings_.save(); // writes defaults on first run so users can find the file
    for (const char* p : kPlatforms) status_[p] = {{"state", "disabled"}, {"detail", ""}};
    capture_ = std::make_unique<capture::CaptureRelay>(
        [this](const json& st) { hub_.publish("capture", st); },
        // The user closed the output window: remember that, so it stays closed.
        [this] { applySettings({{"capture", {{"enabled", false}}}}); });
}

App::~App() { shutdown(); }

void App::start() {
    audio_.start(); // also restores and opens the saved output device
    pipeline_.start();
    if (settings_.snapshot()["ui"].value("thirdPartyEmotes", true)) emotes_.loadGlobalAsync();
    for (const char* p : kPlatforms) restartPlatform(p);
    restartObs();
    capture_->start(); // idle (no thread wake-ups) until the relay is enabled
    captureConfig_ = settings_.snapshot()["capture"];
    capture_->apply(capture::RelayConfig::fromJson(captureConfig_));
}

void App::restartObs() {
    std::lock_guard<std::mutex> lock(obsMutex_);
    if (obs_) {
        obs_->stop();
        obs_.reset();
    }
    json cfg = settings_.snapshot()["obs"];
    obsConfig_ = cfg;
    if (!cfg.value("enabled", true)) {
        setStatus("obs", "disabled", "");
        return;
    }
    ObsWatcher::Config oc;
    oc.host = cfg.value("host", "127.0.0.1");
    oc.port = cfg.value("port", 4455);
    oc.password = cfg.value("password", "");
    obs_ = std::make_unique<ObsWatcher>(
        oc, [this](bool streaming) { onObsStreamState(streaming); },
        [this](const std::string& st, const std::string& d) { setStatus("obs", st, d); });
    obs_->start();
}

void App::onObsStreamState(bool streaming) {
    if (!streaming) return; // the platforms notice the end on their own
    // Event-driven: a stream just started, so look for it now (YouTube /
    // TikTok switch to quick checks for a few minutes) instead of polling.
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const char* p : {"youtube", "tiktok"})
        if (auto it = clients_.find(p); it != clients_.end()) it->second->checkNow();
}

void App::shutdown() {
    std::map<std::string, std::unique_ptr<PlatformClient>> clients;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients.swap(clients_);
    }
    for (auto& [name, c] : clients) c->stop();
    {
        std::lock_guard<std::mutex> lock(obsMutex_);
        if (obs_) obs_->stop();
        obs_.reset();
    }
    if (capture_) capture_->stop(); // releases the device before the process exits
    pipeline_.stop();
    audio_.stop();
    hub_.closeAll();
}

std::string App::redirectUri(const std::string& platform) const {
    return "http://localhost:" + std::to_string(port_) + "/auth/" + platform + "/callback";
}

namespace {

struct GoogleClient {
    std::string id;
    std::string secret;
    std::string source; // "built-in" | "custom"
};

// Which Google OAuth client to use. Built-in by default; the user's own only
// when "Use my own Google credentials" is on. A login is always refreshed
// with the client that created it (forClientId), so changing credentials in
// Settings never breaks an existing login.
std::optional<GoogleClient> pickGoogleClient(const json& yt, const std::string& forClientId, std::string& error) {
    const std::string builtinId = builtin::kGoogleClientId;
    const std::string builtinSecret = builtin::kGoogleClientSecret;
    const std::string customId = util::trim(yt.value("clientId", ""));
    const std::string customSecret = util::trim(yt.value("clientSecret", ""));
    const bool customOn = yt.value("useCustomCredentials", false) && !customId.empty();

    const std::string id = !forClientId.empty() ? forClientId : customOn ? customId : builtinId;
    std::string secret;
    if (id == builtinId) {
        // The secret entered in Settings is accepted for the built-in client
        // too, for builds made without USC_GOOGLE_CLIENT_SECRET.
        secret = !builtinSecret.empty() ? builtinSecret
                 : (customId.empty() || customId == builtinId) ? customSecret
                                                               : std::string();
    } else if (id == customId) {
        secret = customSecret;
    }
    if (secret.empty()) {
        error = !forClientId.empty() && forClientId != builtinId && forClientId != customId
                    ? "Your saved YouTube login belongs to another Google client (" + forClientId +
                          "). Click Login with YouTube once to move it to the current one."
                    : "This build has no Google client secret. Enter it once under Settings > Connections > "
                      "YouTube > Google client (it is saved), or build with USC_GOOGLE_CLIENT_SECRET.";
        return std::nullopt;
    }
    return GoogleClient{id, secret, id == builtinId ? "built-in" : "custom"};
}

const char* kGrantRevokedHelp =
    "Google ended the saved YouTube login (invalid_grant: the refresh token expired or was revoked). "
    "Click Login with YouTube once. If this happens about every 7 days, the Google OAuth app is in "
    "\"Testing\" mode: in Google Cloud Console > Google Auth Platform > Audience, click \"Publish app\" "
    "so logins stop expiring.";

} // namespace

std::optional<oauth::Provider> App::provider(const std::string& platform, std::string& error,
                                             const std::string& forClientId) {
    json s = settings_.snapshot();
    if (platform == "youtube") {
        auto c = pickGoogleClient(s["youtube"], forClientId, error);
        if (!c) return std::nullopt;
        return oauth::google(c->id, c->secret, redirectUri(platform));
    }
    if (platform != "twitch") {
        error = "unknown platform";
        return std::nullopt;
    }
    std::string id = util::trim(s[platform].value("clientId", ""));
    std::string secret = util::trim(s[platform].value("clientSecret", ""));
    if (id.empty() || secret.empty()) {
        error = "Enter the Twitch Client ID and Client Secret first (see README: Creating API credentials)";
        return std::nullopt;
    }
    return oauth::twitch(id, secret, redirectUri(platform));
}

std::string App::authProblem(const std::string& platform) {
    std::lock_guard<std::mutex> lock(authMutex_);
    if (auto it = authError_.find(platform); it != authError_.end()) return it->second;
    if (!tokens_.get(platform))
        return platform == "youtube" ? "Not logged in - click Login with YouTube (only once: the login is remembered)"
                                     : "Not logged in - use Settings > Connections > Login with Twitch";
    return {};
}

std::optional<TokenSet> App::validToken(const std::string& platform, bool forceRefresh) {
    std::lock_guard<std::mutex> lock(authMutex_);
    auto t = tokens_.get(platform);
    if (!t) return std::nullopt;
    const int64_t now = util::nowSec();
    const bool expired = t->expiresAt != 0 && now >= t->expiresAt;
    if (!(forceRefresh || t->expiringSoon(now))) return t;
    // Still-valid token: hand it out even if the early refresh below fails.
    auto fallback = [&]() -> std::optional<TokenSet> {
        return expired || forceRefresh ? std::nullopt : std::optional<TokenSet>(t);
    };
    if (t->refreshToken.empty()) return fallback();
    // After a failed refresh, wait instead of hammering the token endpoint.
    if (util::nowMs() < refreshRetryAtMs_[platform]) return fallback();

    std::string error;
    auto p = provider(platform, error, t->clientId);
    if (!p) {
        authError_[platform] = error;
        refreshRetryAtMs_[platform] = util::nowMs() + 60000;
        return fallback();
    }
    auto r = oauth::refresh(*p, t->refreshToken);
    if (!r.tokens) {
        LOG_WARN("auth", platform, " token refresh failed: ", r.error);
        if (r.grantRevoked()) {
            // The refresh token itself is dead: only a new login helps.
            tokens_.erase(platform);
            authError_[platform] = platform == "youtube" ? kGrantRevokedHelp
                                                         : "Twitch ended the saved login. Click Login with Twitch again.";
            return std::nullopt;
        }
        // Anything else (no network, Google outage, rate limit, rejected
        // client credentials) is temporary or fixable: keep the login.
        authError_[platform] =
            r.oauthError == "invalid_client" || r.oauthError == "unauthorized_client"
                ? "Google rejected the app's client ID/secret (" + r.oauthError +
                      "). Your login is kept; check Settings > Connections > YouTube > Google client."
                : "Could not renew the login (" + r.error + "). Your login is kept; retrying in 30s.";
        // 30s, 1, 2, 4, 8 min ... up to 10 min between attempts.
        int failures = ++refreshFailures_[platform];
        int64_t delay = std::min<int64_t>(int64_t(30000) << std::min(failures - 1, 5), 600000);
        refreshRetryAtMs_[platform] = util::nowMs() + delay;
        return fallback();
    }
    r.tokens->login = t->login;
    r.tokens->userId = t->userId;
    if (r.tokens->clientId.empty()) r.tokens->clientId = p->clientId;
    tokens_.set(platform, *r.tokens);
    authError_.erase(platform);
    refreshRetryAtMs_.erase(platform);
    refreshFailures_.erase(platform);
    LOG_INFO("auth", platform, " token refreshed");
    return r.tokens;
}

std::string App::beginLogin(const std::string& platform, std::string& error) {
    auto p = provider(platform, error);
    if (!p) return {};
    std::string state = util::randomToken();
    std::string verifier = p->pkce ? oauth::makeCodeVerifier() : std::string();
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        // Forget abandoned attempts older than 15 minutes.
        for (auto it = pendingStates_.begin(); it != pendingStates_.end();)
            it = util::nowSec() - it->second.createdAt > 900 ? pendingStates_.erase(it) : std::next(it);
        pendingStates_[state] = {platform, verifier, util::nowSec()};
    }
    std::string url = oauth::buildAuthUrl(*p, state, verifier.empty() ? "" : oauth::codeChallenge(verifier));
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
    PendingLogin pending;
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        auto it = pendingStates_.find(state);
        if (it == pendingStates_.end() || it->second.platform != platform)
            return "Invalid or expired login attempt (state mismatch). Please try again.";
        pending = it->second;
        pendingStates_.erase(it);
    }
    std::string error;
    auto p = provider(platform, error);
    if (!p) return error;
    auto r = oauth::exchangeCode(*p, code, pending.verifier);
    if (!r.tokens) return "Token exchange failed: " + r.error;
    if (r.tokens->refreshToken.empty())
        LOG_WARN("auth", platform, " login returned no refresh token: it will last only about an hour");
    fetchAccountInfo(platform, *r.tokens);
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        tokens_.set(platform, *r.tokens); // persisted in tokens.json until Logout
        authError_.erase(platform);
        refreshRetryAtMs_.erase(platform);
    }
    LOG_INFO("auth", platform, " logged in as ", r.tokens->login, " (client ", r.tokens->clientId, ")");

    // Logging in implies the user wants this platform on.
    if (!settings_.snapshot()[platform].value("enabled", false))
        applySettings({{platform, {{"enabled", true}}}});
    else
        restartPlatform(platform);
    return {};
}

void App::logout(const std::string& platform) {
    std::optional<TokenSet> t;
    {
        std::lock_guard<std::mutex> lock(authMutex_);
        t = tokens_.get(platform);
        tokens_.erase(platform);
        authError_.erase(platform);
        refreshRetryAtMs_.erase(platform);
    }
    // Tell Google too, so the login is really gone (best effort).
    if (t) {
        std::string error;
        if (auto p = provider(platform, error, t->clientId))
            oauth::revoke(*p, t->refreshToken.empty() ? t->accessToken : t->refreshToken);
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
        std::string problem;
        {
            std::lock_guard<std::mutex> lock(authMutex_);
            if (auto it = authError_.find(p); it != authError_.end()) problem = it->second;
        }
        platforms[p]["authError"] = problem;
    }
    {
        // Which Google client a YouTube login uses (built-in unless overridden).
        std::string err;
        auto c = pickGoogleClient(settings_.snapshot()["youtube"], "", err);
        platforms["youtube"]["credentials"] = c ? c->source : "missing";
        platforms["youtube"]["credentialsError"] = c ? "" : err;
        platforms["youtube"]["builtinClientId"] = builtin::kGoogleClientId;
    }
    json kick = settings_.snapshot()["kick"];
    platforms["kick"]["chatroomId"] = !kick.value("chatroomId", "").empty() ? kick.value("chatroomId", "")
                                                                            : kick.value("resolvedChatroomId", "");
    platforms["kick"]["chatroomSource"] = !kick.value("chatroomId", "").empty()           ? "manual"
                                          : !kick.value("resolvedChatroomId", "").empty() ? "cached"
                                                                                           : "";
    return {{"version", APP_VERSION},
            {"port", port_},
            {"platforms", platforms},
            {"configPath", settings_.path()},
            {"botsFiltered", pipeline_.botsFiltered()},
            {"links", links()}};
}

json App::applySettings(const json& patch) {
    json before = settings_.snapshot()["audio"];
    json s = settings_.applyPatch(patch);
    // Muting or moving TTS to the browser silences anything queued natively.
    if ((s["audio"].value("muted", false) && !before.value("muted", false)) ||
        s["audio"].value("output", "") != before.value("output", ""))
        audio_.clear();
    // A newly chosen speaker is opened immediately, not on the next message.
    if (s["audio"].value("deviceId", "") != before.value("deviceId", "") ||
        s["audio"].value("deviceName", "") != before.value("deviceName", "") ||
        s["audio"].value("output", "") != before.value("output", ""))
        audio_.reapplyDevice();
    if (s["ui"].value("thirdPartyEmotes", true)) emotes_.loadGlobalAsync(); // no-op once loaded
    // Only reconnect the platforms whose connection settings actually changed.
    for (const char* p : kPlatforms) {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            changed = platformConfig_[p] != connectionConfig(s[p]);
        }
        if (changed) restartPlatform(p);
    }
    bool obsChanged;
    {
        std::lock_guard<std::mutex> lock(obsMutex_);
        obsChanged = obsConfig_ != s["obs"];
    }
    if (obsChanged) restartObs();
    {
        std::lock_guard<std::mutex> lock(obsMutex_);
        if (captureConfig_ != s["capture"]) {
            captureConfig_ = s["capture"];
            capture_->apply(capture::RelayConfig::fromJson(captureConfig_)); // queued, never blocks
        }
    }
    hub_.publish("settings", s);
    return s;
}

void App::reconnect(const std::string& platform) {
    for (const char* p : kPlatforms) {
        if (!platform.empty() && platform != p) continue;
        // Clients that poll (YouTube) re-check immediately instead of being
        // torn down; everything else is restarted.
        bool checked = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (auto it = clients_.find(p); it != clients_.end()) checked = it->second->checkNow();
        }
        if (checked) {
            {
                std::lock_guard<std::mutex> lock(authMutex_);
                refreshRetryAtMs_.erase(p); // also retry a failed login renewal now
            }
            setStatus(p, "connecting", "Checking now...");
        } else {
            restartPlatform(p);
        }
    }
}

void App::restartPlatform(const std::string& platform) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    if (auto it = clients_.find(platform); it != clients_.end()) {
        it->second->stop();
        clients_.erase(it);
    }
    json s = settings_.snapshot();
    const json& cfg = s[platform];
    platformConfig_[platform] = connectionConfig(cfg);
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
                                  cfg.value("showBacklog", true), tokens,
                                  [this] { return authProblem("youtube"); },
                                  std::max(1, cfg.value("idleCheckMaxMinutes", 10)) * 60000},
            sink, status);
    } else if (platform == "kick") {
        KickClient::Config kc;
        kc.username = cfg.value("username", "");
        kc.chatroomId = cfg.value("chatroomId", "");
        kc.pusherKey = cfg.value("pusherKey", "");
        kc.pusherCluster = cfg.value("pusherCluster", "us2");
        kc.cachedSlug = cfg.value("resolvedSlug", "");
        kc.cachedChatroomId = cfg.value("resolvedChatroomId", "");
        kc.cachedUserId = cfg.value("resolvedUserId", "");
        // Runs on the Kick worker thread: persist only, never restart from here.
        kc.onResolved = [this](const std::string& slug, const std::string& chatroom, const std::string& user) {
            json s = settings_.applyPatch(
                {{"kick", {{"resolvedSlug", slug}, {"resolvedChatroomId", chatroom}, {"resolvedUserId", user}}}});
            hub_.publish("settings", s);
        };
        client = std::make_unique<KickClient>(std::move(kc), sink, status);
    } else {
        client = std::make_unique<TikTokClient>(
            TikTokClient::Config{cfg.value("username", ""), cfg.value("signServer", ""), cfg.value("apiKey", "")},
            sink, status);
    }
    client->start();
    clients_[platform] = std::move(client);
}

json App::kickResolveFromText(const std::string& text) {
    std::string slug = KickClient::normalizeSlug(settings_.snapshot()["kick"].value("username", ""));
    if (slug.empty()) return {{"ok", false}, {"error", "Enter the Kick username first"}};
    auto r = KickClient::parseLookupResponse(200, text);
    if (r.chatroomId.empty())
        return {{"ok", false},
                {"error", r.blocked ? "That is Cloudflare's check page, not the channel data. Wait for the page to finish "
                                      "loading, then copy everything again."
                                    : "No chatroom ID found in the pasted text."}};
    applySettings({{"kick", {{"resolvedSlug", slug}, {"resolvedChatroomId", r.chatroomId}, {"resolvedUserId", r.userId}}}});
    restartPlatform("kick");
    return {{"ok", true}, {"chatroomId", r.chatroomId}};
}

void App::kickForgetChatroom() {
    applySettings({{"kick", {{"resolvedSlug", ""}, {"resolvedChatroomId", ""}, {"resolvedUserId", ""}}}});
    restartPlatform("kick");
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
    // Slang: the Arabic line shows a real translation ("I don't know, brother...").
    pipeline_.submit(make("twitch", "NightOwl", "#FF7F50", {{T::Text, "idk bro, gg wp ngl", ""}}, {}));
    // Arabic dialect: shown and read exactly as written, never translated.
    pipeline_.submit(make("kick", "أبو_فهد", "", {{T::Text, "هاي شباب شلونكم", ""}}, {}));
    // Shown in chat, but never read aloud: contains a link.
    pipeline_.submit(make("kick", "RocketFan99", "#53FC18",
                          {{T::Text, "Clip of that play 🔥 kick.com/rocketfan99/clips", ""}}, {}));
    // Should never appear: removed by the bot filter (when enabled).
    pipeline_.submit(make("twitch", "Nightbot", "#7C7CE1",
                          {{T::Text, "Follow the channel! Type !discord for the server link.", ""}}, {"moderator"}));
}

json App::links() const {
    std::string base = "http://localhost:" + std::to_string(port_);
    json links = {
        {"dock", base + "/"},
        {"overlay", base + "/overlay"},
        {"overlayVoice", base + "/overlay?tts=1"},
        {"window", base + "/?app=1"},
    };
    // Kick channel data, opened in the user's own browser (passes Cloudflare)
    // for the one-time manual chatroom lookup.
    std::string slug = KickClient::normalizeSlug(settings_.snapshot()["kick"].value("username", ""));
    if (!slug.empty()) links["kickLookup"] = "https://kick.com/api/v2/channels/" + util::urlEncode(slug);
    return links;
}

void App::testVoice() {
    json s = settings_.snapshot();
    std::string lang = s["translation"].value("targetLang", std::string("ar"));
    std::string text = lang == "ar" ? "مرحبا! هذا اختبار لصوت القراءة." : "Hello! This is a text to speech test.";
    json segments = json::array({{{"text", text}, {"lang", lang}}});
    if (s["audio"].value("output", std::string("app")) == "app") {
        audio_.clear();
        audio_.enqueue(segments);
    } else {
        hub_.publish("speak", segments); // pages in browser mode play it
    }
}

void App::clearChat() {
    hub_.clearHistory();
    hub_.publish("clear", json::object());
}

} // namespace usc
