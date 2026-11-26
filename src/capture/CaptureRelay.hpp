#pragma once

// Video capture relay: reads one capture device (Elgato etc.) and shows it in
// a Game-Capturable output window. All device/GPU work happens on one worker
// thread fed by a command queue, so HTTP handlers and the UI never block.

#include "capture/CaptureTypes.hpp"
#include "capture/OutputSink.hpp"
#include "capture/VideoBackend.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace usc::capture {

class CaptureRelay {
public:
    // onStatus: a status snapshot changed (published as the "capture" SSE event).
    // onDisabled: the relay turned itself off (window closed by the user, or a
    //   fatal error) -> persist enabled=false so the next launch does not retry.
    // markerPath: crash marker; exists only while a session is starting (and
    //   for its first seconds). Found at launch = the app died while starting.
    CaptureRelay(std::function<void(const nlohmann::json&)> onStatus, std::function<void()> onDisabled,
                 std::string markerPath = "");

    // True once (and clears it) if the previous run died while the relay was
    // starting a session.
    static bool consumeCrashMarker(const std::string& markerPath);
    // Message shown in the relay's status until the next Start.
    void setNotice(const std::string& text);
    // Returns a message when another program (OBS) uses the device with this
    // friendly name. The relay then never opens it - and steps aside if OBS
    // starts using it while the relay runs. Set before start().
    void setConflictCheck(std::function<std::string(const std::string& deviceName)> fn) { conflict_ = std::move(fn); }
    ~CaptureRelay();

    void start();
    void stop();

    // New settings; restarts the session only if something relevant changed.
    void apply(const RelayConfig& cfg);
    void refreshDevices();
    nlohmann::json status() const;
    // Cached modes for a device; if unknown, schedules a probe and returns
    // {"probing": true} (the "capture" event announces the result).
    nlohmann::json modes(const std::string& deviceId);
    std::optional<std::string> preview();

private:
    enum class CmdType { Apply, Enumerate, Probe, SessionError, UserClosed, Quit };
    struct Cmd {
        CmdType type;
        RelayConfig cfg;
        std::string arg;
    };

    void push(Cmd c);
    void run();
    void handle(const Cmd& c);
    void tick(); // watchdog + periodic stats while a session runs
    void startSession();
    void stopSession();
    void fail(const std::string& msg, bool retryable);
    void enumerateNow();
    std::vector<Mode> modesFor(const std::string& deviceId, std::string& error);
    void onFrame(const FrameView& f);
    void runGuarded(const char* what, const std::function<void()>& fn);
    void setMarker(bool on);
    nlohmann::json snapshotLocked() const;
    void publish();

    std::function<void(const nlohmann::json&)> onStatus_;
    std::function<void()> onDisabled_;
    std::string markerPath_;
    std::function<std::string(const std::string&)> conflict_;
    std::string activeDeviceName_; // (stateMutex_)
    bool markerOn_ = false;     // worker thread only
    bool faulted_ = false;      // a crash was prevented: no new sessions until restart
    std::unique_ptr<VideoBackend> backend_;
    std::unique_ptr<OutputSink> output_;

    // command queue
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Cmd> queue_;
    std::thread worker_;
    bool started_ = false;

    // state (guarded by stateMutex_)
    mutable std::mutex stateMutex_;
    RelayConfig cfg_;
    std::string state_ = "off"; // off | starting | running | no-signal | error
    std::string detail_;
    std::string note_;
    bool sessionOpen_ = false;
    std::string activeDeviceId_;
    std::optional<Mode> selected_;
    std::optional<Mode> delivered_;
    int outW_ = 0, outH_ = 0;
    double fpsCap_ = 0;
    std::vector<Device> devices_;
    std::string devicesError_;
    bool enumerating_ = false;
    std::map<std::string, std::vector<Mode>> modeCache_;
    std::map<std::string, std::string> probeErrors_;
    std::map<std::string, bool> probing_;
    int64_t retryAtMs_ = 0;
    int failures_ = 0;
    int64_t lastPublishMs_ = 0;

    // frame statistics (guarded by statsMutex_; touched on the capture thread)
    mutable std::mutex statsMutex_;
    FramePacer pacer_;
    RateMeter captureRate_;
    uint64_t frames_ = 0, rejected_ = 0, paced_ = 0;
    int64_t lastFrameMs_ = 0;
    int64_t sessionStartMs_ = 0;
};

} // namespace usc::capture
