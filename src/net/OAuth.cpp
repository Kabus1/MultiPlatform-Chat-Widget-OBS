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
            clientId, clientSecret, redirectUri};
}

Provider google(const std::string& clientId, const std::string& clientSecret, const std::string& redirectUri) {
    return {"youtube",
            "https://accounts.google.com/o/oauth2/v2/auth",
            "https://oauth2.googleapis.com/token",
            {"https://www.googleapis.com/auth/youtube.readonly"},
            {{"access_type", "offline"}, {"prompt", "consent"}},
            clientId, clientSecret, redirectUri};
}

std::string buildAuthUrl(const Provider& p, const std::string& state) {
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
    return p.authUrl + "?" + http::buildQuery(q);
}

namespace {

Result parseTokenResponse(const http::Response& r, const std::string& previousRefresh) {
    Result out;
    if (!r.ok()) {
        out.error = r.status == 0 ? r.error : ("HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 300));
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

Result exchangeCode(const Provider& p, const std::string& code) {
    auto r = http::postForm(p.tokenUrl, {
        {"client_id", p.clientId},
        {"client_secret", p.clientSecret},
        {"code", code},
        {"grant_type", "authorization_code"},
        {"redirect_uri", p.redirectUri},
    });
    return parseTokenResponse(r, "");
}

Result refresh(const Provider& p, const std::string& refreshToken) {
    auto r = http::postForm(p.tokenUrl, {
        {"client_id", p.clientId},
        {"client_secret", p.clientSecret},
        {"refresh_token", refreshToken},
        {"grant_type", "refresh_token"},
    });
    return parseTokenResponse(r, refreshToken);
}

} // namespace usc::oauth
