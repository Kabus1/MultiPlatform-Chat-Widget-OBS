#include "services/AudioPlayer.hpp"

#include "core/Log.hpp"
#include "core/Settings.hpp"
#include "services/TtsService.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <chrono>

namespace usc {

using nlohmann::json;

struct AudioPlayer::Engine {
    ma_context context{};
    bool contextReady = false;
    ma_engine engine{};
    bool engineReady = false;
    bool started = false;
};

AudioPlayer::AudioPlayer(Settings& settings, TtsService& tts) : settings_(settings), tts_(tts) {}

namespace {

// Stable, backend-specific id string for a device ("" if the backend has none).
std::string deviceKey(ma_backend backend, const ma_device_id& id) {
    switch (backend) {
    case ma_backend_wasapi: {
        std::string s = "wasapi:";
        for (auto c : id.wasapi) {
            if (!c) break;
            s.push_back(c < 128 ? static_cast<char>(c) : '?'); // endpoint ids are ASCII
        }
        return s;
    }
    case ma_backend_coreaudio: return std::string("coreaudio:") + id.coreaudio;
    case ma_backend_alsa: return std::string("alsa:") + id.alsa;
    case ma_backend_pulseaudio: return std::string("pulse:") + id.pulse;
    default: return {};
    }
}

} // namespace

AudioPlayer::~AudioPlayer() { stop(); }

void AudioPlayer::start() {
    if (running_.exchange(true)) return;
    reopen_ = true; // restore the saved output device right away
    worker_ = std::thread([this] {
        applySavedDevice(true);
        run();
    });
}

void AudioPlayer::reapplyDevice() {
    reopen_ = true;
    cv_.notify_all();
}

void AudioPlayer::applySavedDevice(bool startup) {
    reopen_ = false;
    json audio = settings_.snapshot()["audio"];
    if (audio.value("output", std::string("app")) != "app") return;
    std::lock_guard<std::mutex> lock(engineMutex_);
    releaseEngine(); // force a fresh open with the current setting
    std::string error;
    bool ok = ensureEngine(audio.value("deviceId", ""), audio.value("deviceName", ""), error);
    std::lock_guard<std::mutex> lk(mutex_);
    lastError_ = ok ? std::string() : error;
    if (ok && startup) LOG_INFO("audio", "restored saved output device: ", activeDevice_);
}

void AudioPlayer::stop() {
    if (!running_.exchange(false)) return;
    skip_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(engineMutex_);
    releaseEngine();
    if (engine_ && engine_->contextReady) ma_context_uninit(&engine_->context);
    engine_.reset();
}

void AudioPlayer::enqueue(const json& segments) {
    if (!segments.is_array() || segments.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t max = static_cast<size_t>(std::max<int64_t>(1, settings_.snapshot()["tts"].value("maxQueue", 10)));
        while (queue_.size() >= max) queue_.pop_front(); // stay close to live chat
        queue_.push_back(segments);
    }
    cv_.notify_one();
}

void AudioPlayer::skip() { skip_ = true; }

void AudioPlayer::clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    skip_ = true;
}

void AudioPlayer::releaseEngine() {
    if (engine_ && engine_->engineReady) {
        ma_engine_uninit(&engine_->engine);
        engine_->engineReady = false;
        engine_->started = false;
    }
    openedFor_.clear();
}

// Caller holds engineMutex_.
bool AudioPlayer::ensureEngine(const std::string& deviceId, const std::string& deviceName, std::string& error) {
    if (!engine_) engine_ = std::make_unique<Engine>();
    if (!engine_->contextReady) {
        if (ma_context_init(nullptr, 0, nullptr, &engine_->context) != MA_SUCCESS) {
            error = "no audio backend available";
            return false;
        }
        engine_->contextReady = true;
    }
    // Re-open when the chosen device changed (or first use).
    const std::string wanted = deviceId + "|" + deviceName;
    const bool wantsDefault = deviceId.empty() && deviceName.empty();
    // Reuse the open device unless the setting changed, or we fell back to
    // the default and the chosen device may have been plugged back in.
    if (engine_->engineReady && openedFor_ == wanted && !fellBack_) return true;
    releaseEngine();

    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    ma_device_id* id = nullptr;
    ma_device_id chosen{};
    std::string chosenName, chosenKey;
    if (!wantsDefault &&
        ma_context_get_devices(&engine_->context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        // Prefer the exact system id; fall back to the display name (ids can
        // change after driver reinstalls, names after renames).
        for (int pass = 0; pass < 2 && !id; ++pass) {
            for (ma_uint32 i = 0; i < count; ++i) {
                std::string key = deviceKey(engine_->context.backend, infos[i].id);
                bool match = pass == 0 ? (!deviceId.empty() && key == deviceId)
                                       : (!deviceName.empty() && deviceName == infos[i].name);
                if (match) {
                    chosen = infos[i].id;
                    chosenName = infos[i].name;
                    chosenKey = key;
                    id = &chosen;
                    break;
                }
            }
        }
    }
    if (!wantsDefault && !id && !fellBack_)
        LOG_WARN("audio", "output device '", deviceName, "' not found, using system default");
    // Keep settings.json in sync if the device was found under a new id/name.
    if (id && (chosenKey != deviceId || chosenName != deviceName) && !chosenKey.empty())
        settings_.applyPatch({{"audio", {{"deviceId", chosenKey}, {"deviceName", chosenName}}}});

    ma_engine_config cfg = ma_engine_config_init();
    cfg.pContext = &engine_->context;
    cfg.pPlaybackDeviceID = id;
    cfg.noAutoStart = MA_TRUE; // only run the device while speaking
    if (ma_engine_init(&cfg, &engine_->engine) != MA_SUCCESS) {
        error = "cannot open audio device";
        return false;
    }
    engine_->engineReady = true;
    // Match the (possibly just updated) settings so we don't reopen next time.
    openedFor_ = (id && !chosenKey.empty()) ? chosenKey + "|" + chosenName : wanted;
    fellBack_ = !wantsDefault && !id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeDevice_ = id ? chosenName : std::string("(default)");
    }
    LOG_INFO("audio", "TTS output: ", id ? chosenName : std::string("system default"));
    return true;
}

json AudioPlayer::devices() {
    json out = {{"devices", json::array()}};
    json audio = settings_.snapshot()["audio"];
    std::string selected = audio.value("deviceName", "");
    std::string selectedId = audio.value("deviceId", "");
    out["selected"] = selected;
    out["selectedId"] = selectedId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out["active"] = activeDevice_;
        out["lastError"] = lastError_;
    }
    // A throw-away context, so listing never waits for a message being spoken.
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
        out["error"] = "no audio backend available";
        return out;
    }
    out["backend"] = ma_get_backend_name(ctx.backend);
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    bool found = selected.empty() && selectedId.empty();
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            std::string key = deviceKey(ctx.backend, infos[i].id);
            out["devices"].push_back({{"id", key}, {"name", infos[i].name}, {"isDefault", infos[i].isDefault != 0}});
            if ((!selectedId.empty() && key == selectedId) || (!selected.empty() && selected == infos[i].name)) found = true;
        }
    } else {
        out["error"] = "could not list devices";
    }
    ma_context_uninit(&ctx);
    out["missing"] = !found; // saved device is currently unplugged
    return out;
}

bool AudioPlayer::playSegment(const std::string& text, const std::string& lang, const json& s) {
    const json& g = s["google"];
    auto mp3 = tts_.synthesize(text, lang, g.value("apiKey", ""), g.value("cloudVoice", ""));
    if (!mp3 || skip_) return false;

    std::lock_guard<std::mutex> lock(engineMutex_);
    std::string error;
    bool ok = ensureEngine(s["audio"].value("deviceId", ""), s["audio"].value("deviceName", ""), error);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!ok && lastError_ != error) LOG_WARN("audio", error);
        lastError_ = error;
    }
    if (!ok) return false;
    if (!engine_->started) {
        ma_engine_start(&engine_->engine);
        engine_->started = true;
    }

    ma_decoder decoder;
    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(mp3->data(), mp3->size(), &dcfg, &decoder) != MA_SUCCESS) {
        LOG_WARN("audio", "could not decode TTS audio");
        return false;
    }
    ma_sound sound;
    if (ma_sound_init_from_data_source(&engine_->engine, &decoder, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &sound) !=
        MA_SUCCESS) {
        ma_decoder_uninit(&decoder);
        return false;
    }
    const json& tts = s["tts"];
    ma_sound_set_volume(&sound, static_cast<float>(std::clamp(tts.value("volume", 1.0), 0.0, 1.0)));
    ma_sound_set_pitch(&sound, static_cast<float>(std::clamp(tts.value("rate", 1.0), 0.5, 2.0)));
    std::string channel = s["audio"].value("channel", std::string("both"));
    ma_sound_set_pan(&sound, channel == "left" ? -1.0f : channel == "right" ? 1.0f : 0.0f);
    ma_sound_start(&sound);
    while (running_ && !skip_ && !ma_sound_at_end(&sound))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ma_sound_stop(&sound);
    ma_sound_uninit(&sound);
    ma_decoder_uninit(&decoder);
    return true;
}

void AudioPlayer::run() {
    while (running_) {
        json segments;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                // Idle: stop the device so it costs nothing between messages.
                bool idle = !cv_.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return !running_ || !queue_.empty() || reopen_.load(); });
                if (reopen_) {
                    lock.unlock();
                    applySavedDevice(false);
                    continue;
                }
                if (idle) {
                    lock.unlock();
                    std::lock_guard<std::mutex> el(engineMutex_);
                    if (engine_ && engine_->started) {
                        ma_engine_stop(&engine_->engine);
                        engine_->started = false;
                    }
                    continue;
                }
                if (!running_) return;
            }
            segments = std::move(queue_.front());
            queue_.pop_front();
            skip_ = false;
        }
        for (const auto& seg : segments) {
            if (!running_ || skip_) break;
            json s = settings_.snapshot();
            if (s["audio"].value("muted", false)) break;
            playSegment(seg.value("text", ""), seg.value("lang", "en"), s);
        }
    }
}

} // namespace usc
