#pragma once

#include "core/TokenStore.hpp"

#include <optional>
#include <string>
#include <vector>

namespace usc::oauth {

struct Provider {
    std::string name;       // "twitch" | "youtube"
    std::string authUrl;
    std::string tokenUrl;
    std::vector<std::string> scopes;
    std::vector<std::pair<std::string, std::string>> extraAuthParams;
    std::string clientId;
    std::string clientSecret;
    std::string redirectUri;
};

Provider twitch(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri);
Provider google(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri);

// Authorization-code flow: open buildAuthUrl() in the system browser, the
// provider redirects to our local /auth/<name>/callback with ?code=.
std::string buildAuthUrl(const Provider& p, const std::string& state);

struct Result {
    std::optional<TokenSet> tokens;
    std::string error;
};

Result exchangeCode(const Provider& p, const std::string& code);
// Keeps the old refresh token when the provider does not rotate it.
Result refresh(const Provider& p, const std::string& refreshToken);

} // namespace usc::oauth
