#pragma once

#include "core/ChatMessage.hpp"
#include "core/WorkerPool.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace usc {

class Settings;
class EventHub;
class Translator;
class EmoteRegistry;

// Enriches every incoming chat message (language detection, translation line,
// TTS segments) and publishes it to the dock.
//
// Latency rules:
//  - the message text is published at once; network lookups (translation /
//    language detection) never delay it - their result follows as a
//    "chat-update" event;
//  - lookups run on a small pool, so one slow Google request never holds up
//    the messages behind it;
//  - native speech keeps chat order through reserved slots in the speech
//    queue (reserve on arrival, fill when the lookup is done).
class MessagePipeline {
public:
    MessagePipeline(Settings& settings, EventHub& hub, Translator& translator, EmoteRegistry* emotes = nullptr);
    ~MessagePipeline();

    void start();
    void stop();
    void submit(ChatMessage m);

    // Native playback (audio.output == "app"): ordered slots in the speech queue.
    struct SpeechHooks {
        std::function<uint64_t(int64_t createdMs)> reserve;
        std::function<void(uint64_t ticket, const nlohmann::json& segments)> fill; // null = nothing to say
    };
    void setSpeechHooks(SpeechHooks hooks) { speech_ = std::move(hooks); }
    uint64_t botsFiltered() const { return botsFiltered_; }

    // Pure decision logic, separated so it can be unit-tested without network.
    struct Decision {
        bool showTranslation = false;
        nlohmann::json tts; // null or [{text, lang}]
    };
    static Decision decide(const ChatMessage& m, const nlohmann::json& settings, const std::string& speakText,
                           bool translated, const std::string& translation, const std::string& detectedLang,
                           const EmoteRegistry* emotes = nullptr,
                           const std::string& normalizedText = std::string());
    // Turns words that are 7TV / BTTV / FFZ emotes into emote parts.
    static void markThirdPartyEmotes(ChatMessage& m, const EmoteRegistry& emotes);
    static bool wantsLookup(const ChatMessage& m, const nlohmann::json& settings);
    static std::string stripLinks(const std::string& text);

private:
    void run();
    // Translation / detection (when allowed), decision, speech slot, publish.
    void complete(ChatMessage& m, const nlohmann::json& s, uint64_t ticket, bool alreadyPublished, bool lookup);

    Settings& settings_;
    EventHub& hub_;
    Translator& translator_;
    EmoteRegistry* emotes_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ChatMessage> queue_;
    uint64_t seq_ = 0;
    SpeechHooks speech_;
    WorkerPool lookups_{3};
    std::atomic<uint64_t> botsFiltered_{0};
};

} // namespace usc
