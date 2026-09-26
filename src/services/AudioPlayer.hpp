#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

    void enqueue(const nlohmann::json& segments); // [{text, lang}, ...]
    // Re-opens the output device from settings right away (after a change).
    void reapplyDevice();
    void skip();   // stop the message being spoken now
    void clear();  // drop everything queued and stop

private:
    struct Engine;
    void run();
    bool playSegment(const std::string& text, const std::string& lang, const nlohmann::json& s);
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
    std::deque<nlohmann::json> queue_;

    std::mutex engineMutex_; // guards engine_ + context use
    std::unique_ptr<Engine> engine_;
    bool fellBack_ = false;    // chosen device missing, default used (engineMutex_)
    std::string openedFor_;    // deviceName setting the engine was opened for (engineMutex_)
    std::string activeDevice_; // device actually opened, "(default)" if fallback (mutex_)
    std::string lastError_;    // (mutex_)
};

} // namespace usc
