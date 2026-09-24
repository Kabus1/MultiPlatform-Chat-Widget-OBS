#pragma once

#include "core/ChatMessage.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace usc {

class Settings;
class EventHub;
class Translator;

// Enriches every incoming chat message (language detection, translation line,
// TTS segments) on a worker thread, then publishes it to the dock.
class MessagePipeline {
public:
    MessagePipeline(Settings& settings, EventHub& hub, Translator& translator);
    ~MessagePipeline();

    void start();
    void stop();
    void submit(ChatMessage m);

    // Pure decision logic, separated so it can be unit-tested without network.
    struct Decision {
        bool showTranslation = false;
        nlohmann::json tts; // null or [{text, lang}]
    };
    static Decision decide(const ChatMessage& m, const nlohmann::json& settings, const std::string& speakText,
                           bool translated, const std::string& translation, const std::string& detectedLang);
    static bool wantsLookup(const ChatMessage& m, const nlohmann::json& settings);
    static std::string stripLinks(const std::string& text);

private:
    void run();
    void process(ChatMessage& m, size_t backlog);

    Settings& settings_;
    EventHub& hub_;
    Translator& translator_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ChatMessage> queue_;
    uint64_t seq_ = 0;
};

} // namespace usc
