#include "core/TokenStore.hpp"

#include "core/Log.hpp"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace usc {

using nlohmann::json;

void TokenStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream f(path_);
    if (!f) return;
    try {
        data_ = json::parse(f);
        if (!data_.is_object()) data_ = json::object();
    } catch (const std::exception& e) {
        LOG_WARN("tokens", "failed to parse token store: ", e.what());
        data_ = json::object();
    }
}

std::optional<TokenSet> TokenStore::get(const std::string& provider) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data_.contains(provider)) return std::nullopt;
    const json& j = data_[provider];
    TokenSet t;
    t.accessToken = j.value("accessToken", "");
    t.refreshToken = j.value("refreshToken", "");
    t.expiresAt = j.value("expiresAt", int64_t{0});
    t.login = j.value("login", "");
    t.userId = j.value("userId", "");
    if (t.accessToken.empty()) return std::nullopt;
    return t;
}

void TokenStore::set(const std::string& provider, const TokenSet& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[provider] = {
        {"accessToken", t.accessToken},
        {"refreshToken", t.refreshToken},
        {"expiresAt", t.expiresAt},
        {"login", t.login},
        {"userId", t.userId},
    };
    saveLocked();
}

void TokenStore::erase(const std::string& provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(provider);
    saveLocked();
}

void TokenStore::saveLocked() const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    {
        std::ofstream f(path_, std::ios::trunc);
        if (!f) {
            LOG_WARN("tokens", "cannot write ", path_);
            return;
        }
        f << data_.dump(2);
    }
#ifndef _WIN32
    chmod(path_.c_str(), 0600); // tokens are credentials
#endif
}

} // namespace usc
