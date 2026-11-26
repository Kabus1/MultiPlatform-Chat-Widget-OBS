#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usc {

class Settings;
class TtsService;

// Native TTS playback (miniaudio): lists the system's audio output devices
// and plays Google TTS audio on the device chosen in settings.audio.
// The device is remembered in settings.json by its system id (WASAPI endpoint
// id / CoreAudio UID / ALSA or Pulse name) with its display name as a
// fallback. It is restored and opened as soon as the app starts; if it is
// unplugged the system default is used until it comes back.
class AudioPlayer {
public:
    AudioPlayer(Settings& settings, TtsService& tts);
    ~AudioPlayer();

    void start();
    void stop();

    // {devices:[{name,isDefault}], selected, active, missing, backend, error}
    nlohmann::json devices();

    // Speech queue. Audio for upcoming messages is fetched from Google in the
    // background (2 fetchers) while the current one plays, so there is no
    // network wait between messages; messages older than tts.maxDelaySec are
    // dropped instead of being read late.
    void enqueue(const nlohmann::json& segments); // [{text, lang}, ...], spoken in arrival order
    // Ordered slots: reserve() when a chat message arrives (keeps its place in
    // line), fill() once its text/language is known (translation may still be
    // running for earlier/later messages). fill() with null/[] cancels.
    uint64_t reserve(int64_t createdMs);
    void fill(uint64_t ticket, const nlohmann::json& segments);
    // {queued, spoken, droppedStale, droppedOverflow, fetchFailed, lastLagMs}
    nlohmann::json stats();
    // Joins consecutive segments of the same language into one request
    // ("name:" + message) - one Google call and no gap between the clips.
    static nlohmann::json mergeSegments(const nlohmann::json& segments);
    // Replaces Google TTS for the fetchers (tests / offline use). Set before start().
    using Synthesizer = std::function<std::optional<std::string>(const std::string& text, const std::string& lang)>;
    void setSynthesizer(Synthesizer fn) { synth_ = std::move(fn); }
    // Re-opens the output device from settings right away (after a change).
    void reapplyDevice();
    void skip();   // stop the message being spoken now
    void clear();  // drop everything queued and stop

private:
    struct Engine;
    struct Clip {
        std::string text, lang;
        int state = 0; // 0 waiting, 1 fetching, 2 ready, 3 failed
        std::string mp3;
    };
    struct Item {
        uint64_t ticket = 0;
        int64_t createdMs = 0;
        bool filled = false;
        std::vector<Clip> clips;
    };
    void run();
    void fetchLoop();
    void pushItemLocked(std::shared_ptr<Item> item, const nlohmann::json& tts);
    bool playAudio(const std::string& mp3, const nlohmann::json& s);
    bool ensureEngine(const std::string& deviceId, const std::string& deviceName, std::string& error);
    void applySavedDevice(bool startup);
    void releaseEngine();

    Settings& settings_;
    TtsService& tts_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> skip_{false};
    std::atomic<bool> reopen_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Item>> queue_; // waiting to be spoken (mutex_)
    std::shared_ptr<Item> current_;            // being spoken (mutex_)
    std::vector<std::thread> fetchers_;
    Synthesizer synth_;
    uint64_t nextTicket_ = 0;
    uint64_t spoken_ = 0, droppedStale_ = 0, droppedOverflow_ = 0, fetchFailed_ = 0; // (mutex_)
    int64_t lastLagMs_ = 0; // arrival -> start of speech of the last message (mutex_)

    std::mutex engineMutex_; // guards engine_ + context use
    std::unique_ptr<Engine> engine_;
    bool fellBack_ = false;    // chosen device missing, default used (engineMutex_)
    std::string openedFor_;    // deviceName setting the engine was opened for (engineMutex_)
    std::string activeDevice_; // device actually opened, "(default)" if fallback (mutex_)
    std::string lastError_;    // (mutex_)
};

} // namespace usc
