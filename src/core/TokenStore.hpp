#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace usc {

struct TokenSet {
    std::string accessToken;
    std::string refreshToken;
    int64_t expiresAt = 0; // unix seconds, 0 = unknown
    std::string login;     // account name, for display
    std::string userId;

    bool expiringSoon(int64_t nowSec) const { return expiresAt != 0 && nowSec > expiresAt - 120; }
};

// OAuth tokens persisted separately from settings (tokens.json).
class TokenStore {
public:
    explicit TokenStore(std::string path) : path_(std::move(path)) {}

    void load();
    std::optional<TokenSet> get(const std::string& provider) const;
    void set(const std::string& provider, const TokenSet& t);
    void erase(const std::string& provider);

private:
    void saveLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    nlohmann::json data_ = nlohmann::json::object();
};

} // namespace usc
