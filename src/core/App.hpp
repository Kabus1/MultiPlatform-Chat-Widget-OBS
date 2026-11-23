#pragma once

#include "core/EmoteRegistry.hpp"
#include "core/EventHub.hpp"
#include "core/MessagePipeline.hpp"
#include "core/Settings.hpp"
#include "core/TokenStore.hpp"
#include "integrations/ObsWatcher.hpp"
#include "net/OAuth.hpp"
#include "platforms/PlatformClient.hpp"
#include "services/AudioPlayer.hpp"
#include "services/Translator.hpp"
#include "services/TtsService.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace usc {

// Owns every service and the three platform connections; the WebServer
// calls into it for the dock's REST API.
class App {
public:
    App(const std::string& configDir, int portOverride);
    ~App();

    void start();
    void shutdown();

    int port() const { return port_; }
    Settings& settings() { return settings_; }
    EventHub& hub() { return hub_; }
    TtsService& tts() { return tts_; }
    AudioPlayer& audio() { return audio_; }

    // Speaks a short sample on the configured output (app or browser).
    void testVoice();
    // Links the dock / overlay can be embedded with.
    nlohmann::json links() const;

    nlohmann::json status();
    nlohmann::json applySettings(const nlohmann::json& patch);
    void reconnect(const std::string& platform); // "" = all

    // OAuth (platform = "twitch" | "youtube")
    std::string beginLogin(const std::string& platform, std::string& error);
    std::string finishLogin(const std::string& platform, const std::string& code, const std::string& state);
    void logout(const std::string& platform);
    std::string redirectUri(const std::string& platform) const;

    void injectTestMessages();
    void clearChat();

    // Kick: chatroom id from text the user pasted (from the kickLookup page,
    // or just the number), and forgetting the cached id ("Re-detect").
    nlohmann::json kickResolveFromText(const std::string& text);
    void kickForgetChatroom();

private:
    std::optional<TokenSet> validToken(const std::string& platform, bool forceRefresh);
    std::optional<oauth::Provider> provider(const std::string& platform, std::string& error,
                                            const std::string& forClientId = "");
    // Why there is no usable token right now (shown in the platform status).
    std::string authProblem(const std::string& platform);
    void restartPlatform(const std::string& platform);
    void setStatus(const std::string& platform, const std::string& state, const std::string& detail);
    void fetchAccountInfo(const std::string& platform, TokenSet& t);

    Settings settings_;
    TokenStore tokens_;
    EventHub hub_;
    Translator translator_;
    EmoteRegistry emotes_; // must be declared before pipeline_
    TtsService tts_;
    MessagePipeline pipeline_;
    AudioPlayer audio_;
    int port_;

    std::mutex clientsMutex_;
    std::map<std::string, std::unique_ptr<PlatformClient>> clients_;
    std::map<std::string, nlohmann::json> platformConfig_; // config each client was built with

    std::mutex statusMutex_;
    std::map<std::string, nlohmann::json> status_;

    std::mutex authMutex_;
    struct PendingLogin {
        std::string platform;
        std::string verifier; // PKCE code_verifier
        int64_t createdAt = 0;
    };
    std::map<std::string, PendingLogin> pendingStates_; // OAuth state -> attempt
    std::map<std::string, std::string> authError_;      // platform -> last auth problem
    std::map<std::string, int64_t> refreshRetryAtMs_;   // back-off after a failed refresh
    std::map<std::string, int> refreshFailures_;        // consecutive failed refreshes

    void restartObs();
    void onObsStreamState(bool streaming);
    std::mutex obsMutex_;
    std::unique_ptr<ObsWatcher> obs_;
    nlohmann::json obsConfig_;
};

} // namespace usc
