#pragma once

#include "core/EventHub.hpp"
#include "core/MessagePipeline.hpp"
#include "core/Settings.hpp"
#include "core/TokenStore.hpp"
#include "net/OAuth.hpp"
#include "platforms/PlatformClient.hpp"
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

private:
    std::optional<TokenSet> validToken(const std::string& platform, bool forceRefresh);
    std::optional<oauth::Provider> provider(const std::string& platform, std::string& error);
    void restartPlatform(const std::string& platform);
    void setStatus(const std::string& platform, const std::string& state, const std::string& detail);
    void fetchAccountInfo(const std::string& platform, TokenSet& t);

    Settings settings_;
    TokenStore tokens_;
    EventHub hub_;
    Translator translator_;
    TtsService tts_;
    MessagePipeline pipeline_;
    int port_;

    std::mutex clientsMutex_;
    std::map<std::string, std::unique_ptr<PlatformClient>> clients_;
    std::map<std::string, nlohmann::json> platformConfig_; // config each client was built with

    std::mutex statusMutex_;
    std::map<std::string, nlohmann::json> status_;

    std::mutex authMutex_;
    std::map<std::string, std::string> pendingStates_; // state -> platform
};

} // namespace usc
