#include "core/EmoteRegistry.hpp"

#include "core/Log.hpp"
#include "net/Http.hpp"

namespace usc {

using nlohmann::json;

EmoteRegistry::~EmoteRegistry() {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

void EmoteRegistry::spawn(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(threadsMutex_);
    threads_.emplace_back([fn = std::move(fn)] {
        try {
            fn();
        } catch (const std::exception& e) {
            LOG_WARN("emotes", e.what());
        }
    });
}

namespace {
json fetchJson(const std::string& url) {
    auto r = http::get(url, {{"Accept", "application/json"}}, 8);
    if (!r.ok()) return nullptr;
    return json::parse(r.body, nullptr, false);
}
} // namespace

EmoteRegistry::EmoteList EmoteRegistry::parse7tv(const json& j) {
    EmoteList out;
    const json* set = &j;
    if (j.contains("emote_set") && j["emote_set"].is_object()) set = &j["emote_set"];
    if (!set->contains("emotes") || !(*set)["emotes"].is_array()) return out;
    for (const auto& e : (*set)["emotes"]) {
        std::string name = e.value("name", ""), id = e.value("id", "");
        if (!name.empty() && !id.empty()) out.emplace_back(name, "https://cdn.7tv.app/emote/" + id + "/1x.webp");
    }
    return out;
}

EmoteRegistry::EmoteList EmoteRegistry::parseBttv(const json& j) {
    EmoteList out;
    auto take = [&](const json& arr) {
        if (!arr.is_array()) return;
        for (const auto& e : arr) {
            std::string code = e.value("code", ""), id = e.value("id", "");
            if (!code.empty() && !id.empty()) out.emplace_back(code, "https://cdn.betterttv.net/emote/" + id + "/1x");
        }
    };
    if (j.is_array()) take(j);
    else if (j.is_object()) {
        take(j.value("channelEmotes", json::array()));
        take(j.value("sharedEmotes", json::array()));
    }
    return out;
}

EmoteRegistry::EmoteList EmoteRegistry::parseFfz(const json& j) {
    EmoteList out;
    if (!j.is_object() || !j.contains("sets") || !j["sets"].is_object()) return out;
    for (const auto& [id, set] : j["sets"].items()) {
        (void)id;
        for (const auto& e : set.value("emoticons", json::array())) {
            std::string name = e.value("name", "");
            std::string url;
            const json urls = e.value("urls", json::object());
            if (urls.contains("1") && urls["1"].is_string()) url = urls["1"].get<std::string>();
            if (url.rfind("//", 0) == 0) url = "https:" + url;
            if (!name.empty()) out.emplace_back(name, url);
        }
    }
    return out;
}

void EmoteRegistry::add(const std::string& scope, const EmoteList& emotes) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& target = scope.empty() ? global_ : channels_[scope];
    for (const auto& [name, url] : emotes) target[name] = url;
}

void EmoteRegistry::loadGlobalAsync() {
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        if (!requested_.insert("global").second) return;
    }
    spawn([this] {
        size_t before = size();
        add("", parse7tv(fetchJson("https://7tv.io/v3/emote-sets/global")));
        add("", parseBttv(fetchJson("https://api.betterttv.net/3/cached/emotes/global")));
        add("", parseFfz(fetchJson("https://api.frankerfacez.com/v1/set/global")));
        LOG_INFO("emotes", "loaded ", size() - before, " global 7TV/BTTV/FFZ emotes");
    });
}

void EmoteRegistry::ensureChannelAsync(const std::string& platform, const std::string& channelId) {
    if (channelId.empty() || (platform != "twitch" && platform != "kick")) return;
    std::string scope = scopeKey(platform, channelId);
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        if (!requested_.insert(scope).second) return;
    }
    spawn([this, platform, channelId, scope] {
        EmoteList all;
        auto append = [&](EmoteList l) { all.insert(all.end(), l.begin(), l.end()); };
        append(parse7tv(fetchJson("https://7tv.io/v3/users/" + platform + "/" + channelId)));
        if (platform == "twitch") {
            append(parseBttv(fetchJson("https://api.betterttv.net/3/cached/users/twitch/" + channelId)));
            append(parseFfz(fetchJson("https://api.frankerfacez.com/v1/room/id/" + channelId)));
        }
        add(scope, all);
        LOG_INFO("emotes", "loaded ", all.size(), " channel emotes for ", scope);
    });
}

bool EmoteRegistry::contains(const std::string& platform, const std::string& channelId, const std::string& word) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (global_.count(word)) return true;
    auto it = channels_.find(scopeKey(platform, channelId));
    return it != channels_.end() && it->second.count(word);
}

std::string EmoteRegistry::url(const std::string& platform, const std::string& channelId, const std::string& word) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = channels_.find(scopeKey(platform, channelId));
    if (it != channels_.end())
        if (auto e = it->second.find(word); e != it->second.end()) return e->second;
    if (auto g = global_.find(word); g != global_.end()) return g->second;
    return {};
}

size_t EmoteRegistry::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    size_t n = global_.size();
    for (const auto& [k, v] : channels_) n += v.size();
    return n;
}

} // namespace usc
