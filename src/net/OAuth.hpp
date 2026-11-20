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
    bool pkce = false;          // send an S256 code_challenge (RFC 7636)
    std::string revokeUrl;      // token revocation endpoint, if any
};

Provider twitch(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri);
Provider google(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri);

// Authorization-code flow: open buildAuthUrl() in the system browser, the
// provider redirects to our local /auth/<name>/callback with ?code=.
// With p.pkce, pass the challenge from codeChallenge(verifier).
std::string buildAuthUrl(const Provider& p, const std::string& state, const std::string& codeChallenge = "");

// PKCE helpers.
std::string makeCodeVerifier();
std::string codeChallenge(const std::string& verifier);

struct Result {
    std::optional<TokenSet> tokens;
    std::string error;       // human-readable
    std::string oauthError;  // the provider's "error" code, e.g. "invalid_grant", "invalid_client"
    int httpStatus = 0;      // 0 = no response (network problem)

    // The refresh token itself is dead (expired, revoked, password change):
    // only a new login can fix it. Anything else is worth retrying.
    bool grantRevoked() const { return oauthError == "invalid_grant"; }
};

Result exchangeCode(const Provider& p, const std::string& code, const std::string& codeVerifier = "");
// Best effort: tells the provider to invalidate the token (used on logout).
void revoke(const Provider& p, const std::string& token);
// Keeps the old refresh token when the provider does not rotate it.
Result refresh(const Provider& p, const std::string& refreshToken);

} // namespace usc::oauth
