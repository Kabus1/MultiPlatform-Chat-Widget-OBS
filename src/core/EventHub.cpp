#include "core/EventHub.hpp"

#include <algorithm>
#include <chrono>

namespace usc {

std::optional<std::string> EventHub::Subscription::next(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return std::nullopt;
    std::string s = std::move(queue_.front());
    queue_.pop_front();
    return s;
}

std::string EventHub::frame(const std::string& event, const nlohmann::json& data) {
    // dump() never emits raw newlines, so a single data: line is valid SSE.
    return "event: " + event + "\ndata: " + data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
}

std::shared_ptr<EventHub::Subscription> EventHub::subscribe() {
    auto sub = std::make_shared<Subscription>();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& h : history_) sub->queue_.push_back(h.frame);
    subs_.push_back(sub);
    return sub;
}

void EventHub::unsubscribe(const std::shared_ptr<Subscription>& sub) {
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
}

std::string EventHub::replayFrame(const std::string& event, const nlohmann::json& data) {
    // Replayed frames must never re-trigger TTS on reconnecting clients.
    nlohmann::json replay = data;
    if (replay.is_object()) {
        replay["history"] = true;
        replay["tts"] = nullptr;
    }
    return frame(event, replay);
}

// Caller holds mutex_.
void EventHub::broadcastLocked(const std::string& f) {
    for (auto& s : subs_) {
        {
            std::lock_guard<std::mutex> sl(s->mutex_);
            if (s->queue_.size() > 1000) s->queue_.pop_front(); // slow client, drop oldest
            s->queue_.push_back(f);
        }
        s->cv_.notify_one();
    }
}

void EventHub::publish(const std::string& event, const nlohmann::json& data, bool keepInHistory) {
    std::string f = frame(event, data);
    std::lock_guard<std::mutex> lock(mutex_);
    if (keepInHistory) {
        std::string id = data.is_object() ? data.value("id", std::string()) : std::string();
        history_.push_back({id, replayFrame(event, data)});
        while (history_.size() > historyLimit_) history_.pop_front();
    }
    broadcastLocked(f);
}

void EventHub::updateChat(const nlohmann::json& message) {
    std::string f = frame("chat-update", message);
    std::string id = message.is_object() ? message.value("id", std::string()) : std::string();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!id.empty())
        for (auto it = history_.rbegin(); it != history_.rend(); ++it)
            if (it->id == id) {
                it->frame = replayFrame("chat", message);
                break;
            }
    broadcastLocked(f);
}

void EventHub::clearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

void EventHub::closeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : subs_) {
        {
            std::lock_guard<std::mutex> sl(s->mutex_);
            s->closed_ = true;
        }
        s->cv_.notify_all();
    }
    subs_.clear();
}

} // namespace usc
