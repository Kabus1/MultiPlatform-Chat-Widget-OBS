#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

namespace usc {

// JSON-backed settings. Every section the dock's settings sidebar shows lives
// here; unknown keys are dropped and missing keys fall back to defaults().
class Settings {
public:
    explicit Settings(std::string path);

    static nlohmann::json defaults();

    void load();
    bool save() const;

    nlohmann::json snapshot() const;
    // Applies an RFC 7386 JSON merge patch (what the dock POSTs) and saves.
    nlohmann::json applyPatch(const nlohmann::json& patch);

    const std::string& path() const { return path_; }

private:
    static nlohmann::json sanitize(const nlohmann::json& in);

    std::string path_;
    mutable std::mutex mutex_;
    nlohmann::json data_;
};

} // namespace usc
