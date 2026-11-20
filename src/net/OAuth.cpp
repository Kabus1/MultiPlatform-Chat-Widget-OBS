#include "net/OAuth.hpp"

#include "core/Util.hpp"
#include "net/Http.hpp"

#include <nlohmann/json.hpp>

namespace usc::oauth {

using nlohmann::json;

Provider twitch(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri) {
    return {"twitch",
            "https://id.twitch.tv/oauth2/authorize",
            "https://id.twitch.tv/oauth2/token",
            {"chat:read"},
            {{"force_verify", "false"}},
            clientId, clientSecret, redirectUri,
            false,
            ""};
}

Provider google(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri) {
    return {"youtube",
            "https://accounts.google.com/o/oauth2/v2/auth",
            "https://oauth2.googleapis.com/token",
            {"https://www.googleapis.com/auth/youtube.readonly"},
            // offline + consent: always returns a refresh token, so the login
            // survives restarts. select_account lets the user pick the
            // Google account / Brand Account that owns the YouTube channel.
            {{"access_type", "offline"}, {"prompt", "consent select_account"}, {"include_granted_scopes", "true"}},
            clientId, clientSecret, redirectUri,
            true,
            "https://oauth2.googleapis.com/revoke"};
}

std::string makeCodeVerifier() { return util::randomToken(32); } // 64 chars of [0-9a-f]

std::string codeChallenge(const std::string& verifier) { return util::base64UrlEncode(util::sha256(verifier)); }

std::string buildAuthUrl(const Provider& p, const std::string& state, const std::string& challenge) {
    std::string scope;
    for (const auto& s : p.scopes) scope += (scope.empty() ? "" : " ") + s;
    http::Params q = {
        {"client_id", p.clientId},
        {"redirect_uri", p.redirectUri},
        {"response_type", "code"},
        {"scope", scope},
        {"state", state},
    };
    for (const auto& kv : p.extraAuthParams) q.push_back(kv);
    if (p.pkce && !challenge.empty()) {
        q.push_back({"code_challenge", challenge});
        q.push_back({"code_challenge_method", "S256"});
    }
    return p.authUrl + "?" + http::buildQuery(q);
}

namespace {

Result parseTokenResponse(const http::Response& r, const std::string& previousRefresh) {
    Result out;
    out.httpStatus = r.status;
    if (!r.ok()) {
        if (r.status == 0) {
            out.error = "no connection to the login server (" + r.error + ")";
            return out;
        }
        // OAuth errors: {"error": "invalid_grant", "error_description": "..."}
        json e = json::parse(r.body, nullptr, false);
        if (e.is_object()) {
            if (e.contains("error") && e["error"].is_string()) out.oauthError = e["error"].get<std::string>();
            std::string desc = e.value("error_description", e.value("message", std::string()));
            out.error = (out.oauthError.empty() ? "HTTP " + std::to_string(r.status) : out.oauthError) +
                        (desc.empty() ? "" : ": " + desc);
        } else {
            out.error = "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 200);
        }
        return out;
    }
    try {
        json j = json::parse(r.body);
        TokenSet t;
        t.accessToken = j.value("access_token", "");
        t.refreshToken = j.value("refresh_token", previousRefresh);
        if (j.contains("expires_in") && j["expires_in"].is_number())
            t.expiresAt = util::nowSec() + j["expires_in"].get<int64_t>();
        if (t.accessToken.empty()) {
            out.error = "no access_token in response";
            return out;
        }
        out.tokens = t;
    } catch (const std::exception& e) {
        out.error = std::string("bad token response: ") + e.what();
    }
    return out;
}

} // namespace

Result exchangeCode(const Provider& p, const std::string& code, const std::string& codeVerifier) {
    http::Params form = {
        {"client_id", p.clientId},
        {"code", code},
        {"grant_type", "authorization_code"},
        {"redirect_uri", p.redirectUri},
    };
    if (!p.clientSecret.empty()) form.push_back({"client_secret", p.clientSecret});
    if (!codeVerifier.empty()) form.push_back({"code_verifier", codeVerifier});
    auto r = http::postForm(p.tokenUrl, form);
    Result res = parseTokenResponse(r, "");
    if (res.tokens) res.tokens->clientId = p.clientId;
    return res;
}

void revoke(const Provider& p, const std::string& token) {
    if (p.revokeUrl.empty() || token.empty()) return;
    http::postForm(p.revokeUrl, {{"token", token}}, {}, 5);
}

Result refresh(const Provider& p, const std::string& refreshToken) {
    http::Params form = {
        {"client_id", p.clientId},
        {"refresh_token", refreshToken},
        {"grant_type", "refresh_token"},
    };
    if (!p.clientSecret.empty()) form.push_back({"client_secret", p.clientSecret});
    Result res = parseTokenResponse(http::postForm(p.tokenUrl, form), refreshToken);
    if (res.tokens) res.tokens->clientId = p.clientId;
    return res;
}

} // namespace usc::oauth
