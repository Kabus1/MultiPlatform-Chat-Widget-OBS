#pragma once

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace usc {

// Fan-out of server-sent events to every connected dock / overlay page.
class EventHub {
public:
    class Subscription {
    public:
        // Blocks up to timeoutMs for the next serialized SSE frame.
        std::optional<std::string> next(int timeoutMs);
        bool closed() const { return closed_; }

    private:
        friend class EventHub;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::string> queue_;
        bool closed_ = false;
    };

    explicit EventHub(size_t chatHistory = 100) : historyLimit_(chatHistory) {}

    std::shared_ptr<Subscription> subscribe(); // replays recent chat
    void unsubscribe(const std::shared_ptr<Subscription>& sub);

    void publish(const std::string& event, const nlohmann::json& data, bool keepInHistory = false);
    void clearHistory();
    void closeAll();

private:
    static std::string frame(const std::string& event, const nlohmann::json& data);

    std::mutex mutex_;
    std::vector<std::shared_ptr<Subscription>> subs_;
    std::deque<std::string> history_;
    size_t historyLimit_;
};

} // namespace usc
