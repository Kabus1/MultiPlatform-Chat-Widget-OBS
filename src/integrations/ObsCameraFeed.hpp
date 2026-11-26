#pragma once

// TikTok Studio feed through OBS - no second owner for the capture card.
//
// A capture card (Elgato etc.) can be opened by one program at a time. OBS
// keeps it as a normal "Video Capture Device" source (direct, no Game
// Capture), and TikTok Studio selects "OBS Virtual Camera" as its camera.
// This module drives OBS over obs-websocket:
//   - inventory: which capture sources OBS has and which devices they use
//     (read-only; also used so our own capture relay never takes a device
//     OBS is using);
//   - setup (on request): a scene "USC TikTok Feed" that *references* the
//     existing capture source - OBS still opens the device only once - fitted
//     to the canvas;
//   - OBS Virtual Camera start / stop (optionally automatic when OBS connects).
//
// All OBS calls run on this module's own thread; nothing blocks the UI, the
// HTTP server or the OBS WebSocket thread.

#include "integrations/ObsWatcher.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace usc {

class ObsCameraFeed {
public:
    struct Config {
        bool enabled = false;               // keep the feed scene in place (set by "Set up")
        std::string sceneName = "USC TikTok Feed";
        std::string sourceName;             // "" = pick the capture source automatically
        bool autoStartVirtualCam = false;   // start OBS Virtual Camera when OBS connects

        static Config fromJson(const nlohmann::json& j);
    };

    // A video capture source in OBS.
    struct CaptureInput {
        std::string name;   // OBS source name, e.g. "Elgato 4K S"
        std::string kind;   // dshow_input, av_capture_input, v4l2_input...
        std::string device; // device friendly name it uses ("" if unknown)
    };

    explicit ObsCameraFeed(std::function<void(const nlohmann::json&)> onStatus);
    ~ObsCameraFeed();

    void start();
    void stop();

    // The current OBS connection (null while OBS is not configured). Safe to
    // call when the App re-creates its ObsWatcher.
    void attach(std::shared_ptr<ObsWatcher> obs);
    void apply(const Config& cfg);

    // Wire these to the ObsWatcher handlers.
    void onObsReady(bool ready);
    void onObsEvent(const std::string& type, const nlohmann::json& data);

    void setupNow();                 // create / repair the feed scene
    void setVirtualCam(bool on);     // start / stop OBS Virtual Camera
    nlohmann::json status() const;

    // Message if OBS uses a capture device with this friendly name (our relay
    // must then not open it), "" otherwise.
    std::string conflictFor(const std::string& deviceName) const;

    // ---- pure helpers (unit-tested) ----
    static bool isVideoCaptureKind(const std::string& kind);
    // Device friendly name from a capture source's settings.
    static std::string deviceFromSettings(const std::string& kind, const nlohmann::json& settings);
    // Which source feeds TikTok: the configured one, else one whose name or
    // device mentions Elgato, else the first capture source.
    static std::optional<CaptureInput> pickInput(const std::vector<CaptureInput>& inputs, const std::string& preferred);
    static bool sameDevice(const std::string& a, const std::string& b);

private:
    void run();
    // Synchronous OBS request from this module's thread (bounded wait).
    std::optional<nlohmann::json> call(const std::string& type, const nlohmann::json& data, std::string& error,
                                       int timeoutMs = 4000);
    void refreshInventory();
    void ensureScene();
    void refreshVirtualCam();
    void publish();

    std::function<void(const nlohmann::json&)> onStatus_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_ = false;
    std::shared_ptr<ObsWatcher> obs_;
    Config cfg_;
    // work flags
    bool needInventory_ = false, needSetup_ = false, needVcamStatus_ = false;
    int vcamRequest_ = -1; // -1 none, 0 stop, 1 start
    // state
    bool obsReady_ = false;
    std::vector<CaptureInput> inputs_;
    std::string chosen_;
    bool sceneReady_ = false;
    bool vcamActive_ = false;
    std::string error_;
    std::string note_;
};

} // namespace usc
