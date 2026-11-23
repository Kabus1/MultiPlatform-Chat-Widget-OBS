#include "capture/CaptureRelay.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"

#ifdef _WIN32
#include <objbase.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>

using nlohmann::json;

namespace usc::capture {

namespace {
constexpr int kTickMs = 500;
constexpr int kNoSignalMs = 2500;   // no frames for this long -> "no-signal"
constexpr int kStatsPublishMs = 1000;
constexpr int kRetryFirstMs = 5000;
constexpr int kRetryMaxMs = 30000;
} // namespace

CaptureRelay::CaptureRelay(std::function<void(const json&)> onStatus, std::function<void()> onUserClosed)
    : onStatus_(std::move(onStatus)), onUserClosed_(std::move(onUserClosed)), backend_(createVideoBackend()),
      output_(createOutputSink()) {
    // The window thread only queues a command: it must never wait on the
    // worker, which joins the window thread when it closes the output.
    output_->onUserClosed([this] { push({CmdType::UserClosed, {}, ""}); });
}

CaptureRelay::~CaptureRelay() { stop(); }

void CaptureRelay::start() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (started_) return;
    started_ = true;
    worker_ = std::thread([this] { run(); });
}

void CaptureRelay::stop() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!started_) return;
        started_ = false;
        queue_.push_back({CmdType::Quit, {}, ""});
    }
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CaptureRelay::push(Cmd c) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!started_) return;
        queue_.push_back(std::move(c));
    }
    queueCv_.notify_all();
}

void CaptureRelay::apply(const RelayConfig& cfg) { push({CmdType::Apply, cfg, ""}); }

void CaptureRelay::refreshDevices() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (enumerating_) return;
        enumerating_ = true;
    }
    push({CmdType::Enumerate, {}, ""});
}

json CaptureRelay::modes(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    json out = {{"device", deviceId}, {"modes", json::array()}, {"probing", false}, {"error", ""}};
    if (auto it = modeCache_.find(deviceId); it != modeCache_.end()) {
        for (const auto& m : it->second) out["modes"].push_back(m.toJson());
        return out;
    }
    if (auto it = probeErrors_.find(deviceId); it != probeErrors_.end()) {
        out["error"] = it->second;
        probeErrors_.erase(it); // the next request tries again
        return out;
    }
    out["probing"] = true;
    if (!probing_[deviceId]) {
        probing_[deviceId] = true;
        // push() locks only the queue mutex: no lock-order issue.
        Cmd c{CmdType::Probe, {}, deviceId};
        std::lock_guard<std::mutex> q(queueMutex_);
        if (started_) queue_.push_back(std::move(c));
        queueCv_.notify_all();
    }
    return out;
}

std::optional<std::string> CaptureRelay::preview() {
    output_->requestPreview();
    return output_->previewJpeg();
}

json CaptureRelay::status() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return snapshotLocked();
}

json CaptureRelay::snapshotLocked() const {
    json j;
    j["supported"] = backend_->supported() && output_->supported();
    j["enabled"] = cfg_.enabled;
    j["state"] = state_;
    j["detail"] = detail_;
    j["note"] = note_;
    j["deviceId"] = cfg_.deviceId;
    j["deviceName"] = cfg_.deviceName;
    bool present = false;
    for (const auto& d : devices_)
        if (d.id == cfg_.deviceId || (!cfg_.deviceName.empty() && d.name == cfg_.deviceName)) present = true;
    j["devicePresent"] = present;
    j["selectedMode"] = selected_ ? selected_->toJson() : json(nullptr);
    j["deliveredFormat"] = delivered_ ? delivered_->subtype : "";
    j["output"] = {{"width", outW_}, {"height", outH_}, {"fpsCap", fpsCap_}, {"title", cfg_.windowTitle}};
    {
        std::lock_guard<std::mutex> s(statsMutex_);
        int64_t now = util::nowMs();
        j["measured"] = {{"captureFps", std::round(captureRate_.rate(now) * 10) / 10},
                         {"outputFps", std::round(outputRate_.rate(now) * 10) / 10},
                         {"frames", frames_},
                         {"presented", presented_},
                         {"dropped", dropped_},
                         {"paced", paced_},
                         {"lastFrameAgoMs", lastFrameMs_ ? now - lastFrameMs_ : -1}};
    }
    OutputState os = output_->state();
    j["window"] = {{"open", os.open}, {"visible", os.visible}, {"minimized", os.minimized}, {"error", os.error}};
    j["devices"] = json::array();
    for (const auto& d : devices_) j["devices"].push_back({{"id", d.id}, {"name", d.name}});
    j["devicesError"] = devicesError_;
    j["enumerating"] = enumerating_;
    return j;
}

void CaptureRelay::publish() {
    json snap;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snap = snapshotLocked();
        lastPublishMs_ = util::nowMs();
    }
    if (onStatus_) onStatus_(snap);
}

// ------------------------------------------------------------------ worker

void CaptureRelay::run() {
#ifdef _WIN32
    // Media Foundation objects created here are free-threaded (MTA).
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    enumerateNow();
    publish();
    for (;;) {
        std::optional<Cmd> cmd;
        bool active;
        {
            // Read before taking the queue lock (lock order: state -> queue).
            std::lock_guard<std::mutex> s(stateMutex_);
            active = cfg_.enabled;
        }
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            // Idle (relay off): sleep until a command arrives - no polling.
            if (active)
                queueCv_.wait_for(lock, std::chrono::milliseconds(kTickMs), [&] { return !queue_.empty(); });
            else
                queueCv_.wait(lock, [&] { return !queue_.empty(); });
            if (!queue_.empty()) {
                cmd = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (cmd) {
            if (cmd->type == CmdType::Quit) break;
            try {
                handle(*cmd);
            } catch (const std::exception& e) {
                fail(std::string("Internal error: ") + e.what(), false);
            }
        }
        tick();
    }
    stopSession();
#ifdef _WIN32
    if (SUCCEEDED(co)) CoUninitialize();
#endif
}

void CaptureRelay::handle(const Cmd& c) {
    switch (c.type) {
    case CmdType::Apply: {
        bool restart;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            restart = !cfg_.sameSession(c.cfg) || (c.cfg.enabled && state_ == "error");
            cfg_ = c.cfg;
            if (restart) {
                failures_ = 0;
                retryAtMs_ = 0;
            }
        }
        if (!restart) {
            publish();
            return;
        }
        stopSession();
        bool enabled;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            enabled = cfg_.enabled;
        }
        if (enabled) {
            startSession();
        } else {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = "off";
            detail_.clear();
            note_.clear();
        }
        publish();
        return;
    }
    case CmdType::Enumerate:
        enumerateNow();
        publish();
        return;
    case CmdType::Probe: {
        std::string error;
        std::vector<Mode> m = modesFor(c.arg, error);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            probing_.erase(c.arg);
            if (m.empty()) probeErrors_[c.arg] = error.empty() ? "The device reported no video modes" : error;
        }
        publish();
        return;
    }
    case CmdType::SessionError:
        stopSession();
        fail(c.arg, true);
        return;
    case CmdType::UserClosed:
        stopSession();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            cfg_.enabled = false;
            state_ = "off";
            detail_ = "Output window closed";
        }
        publish();
        if (onUserClosed_) onUserClosed_(); // persists enabled=false (re-enters apply(): a no-op)
        return;
    case CmdType::Quit:
        return;
    }
}

void CaptureRelay::tick() {
    int64_t now = util::nowMs();
    bool doRetry = false, changed = false, doPublish = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!cfg_.enabled) return;
        if (state_ == "error" && retryAtMs_ > 0 && now >= retryAtMs_) {
            retryAtMs_ = 0;
            doRetry = true;
        } else if (sessionOpen_) {
            int64_t last, startedAt;
            {
                std::lock_guard<std::mutex> s(statsMutex_);
                last = lastFrameMs_;
                startedAt = sessionStartMs_;
            }
            int64_t since = now - std::max(last, startedAt);
            if ((state_ == "running" || state_ == "starting") && since > kNoSignalMs) {
                state_ = "no-signal";
                detail_ = "The device delivers no frames - check the HDMI source, cable and that the "
                          "console/PC output is on";
                changed = true;
            } else if (state_ == "no-signal" && last > 0 && now - last < kNoSignalMs) {
                state_ = "running";
                detail_ = "";
                changed = true;
            }
            doPublish = changed || now - lastPublishMs_ >= kStatsPublishMs;
        }
    }
    if (doRetry) {
        enumerateNow(); // the device may have been re-plugged into another port
        startSession();
        publish();
        return;
    }
    if (changed) LOG_INFO("capture", "state: no-signal/running transition");
    if (doPublish) publish();
}

void CaptureRelay::enumerateNow() {
    std::string error;
    std::vector<Device> list = backend_->enumerate(error);
    std::lock_guard<std::mutex> lock(stateMutex_);
    devices_ = std::move(list);
    devicesError_ = error;
    enumerating_ = false;
}

std::vector<Mode> CaptureRelay::modesFor(const std::string& deviceId, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (auto it = modeCache_.find(deviceId); it != modeCache_.end()) return it->second;
        // Never open the device we are streaming from a second time.
        if (sessionOpen_ && activeDeviceId_ == deviceId) {
            error = "The device is in use by the relay";
            return {};
        }
    }
    std::vector<Mode> m = backend_->probe(deviceId, error);
    if (!m.empty()) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        modeCache_[deviceId] = m;
    }
    return m;
}

void CaptureRelay::startSession() {
    RelayConfig cfg;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cfg = cfg_;
        state_ = "starting";
        detail_ = "Opening the device";
        note_.clear();
    }
    publish();
    if (!backend_->supported() || !output_->supported()) {
        std::string e;
        backend_->enumerate(e);
        return fail(e.empty() ? "Video capture is not supported on this system" : e, false);
    }
    if (cfg.deviceId.empty() && cfg.deviceName.empty()) return fail("Choose a capture device first", false);

    // Find the device: by id, else by name (the id changes with the USB port).
    std::string id;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& d : devices_)
            if (d.id == cfg.deviceId) id = d.id;
        if (id.empty())
            for (const auto& d : devices_)
                if (!cfg.deviceName.empty() && d.name == cfg.deviceName) id = d.id;
    }
    if (id.empty()) {
        enumerateNow();
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& d : devices_)
            if (d.id == cfg.deviceId || (!cfg.deviceName.empty() && d.name == cfg.deviceName)) id = d.id;
    }
    if (id.empty()) return fail("\"" + (cfg.deviceName.empty() ? cfg.deviceId : cfg.deviceName) +
                                    "\" is not connected (or its driver is not installed)",
                                true);

    std::string error;
    std::vector<Mode> modes = modesFor(id, error);
    if (modes.empty()) return fail(error.empty() ? "The device reported no video modes" : error, true);
    std::string note;
    std::optional<Mode> mode = chooseMode(modes, cfg, note);
    if (!mode) return fail("None of the device's modes can be used", false);
    auto [outW, outH] = outputSize(cfg, mode->width, mode->height);
    // A cap at or above the source rate changes nothing: present every frame.
    double cap = (cfg.outputFps > 0 && cfg.outputFps < mode->fps() - 0.5) ? double(cfg.outputFps) : 0.0;

    if (!output_->open(cfg.windowTitle, outW, outH, cfg.keepAspect, error)) return fail(error, false);
    {
        std::lock_guard<std::mutex> s(statsMutex_);
        pacer_.reset(cap);
        captureRate_ = RateMeter{};
        outputRate_ = RateMeter{};
        frames_ = presented_ = dropped_ = paced_ = 0;
        lastFrameMs_ = 0;
        sessionStartMs_ = util::nowMs();
    }
    Mode delivered;
    bool ok = backend_->open(
        id, *mode, [this](const FrameView& f) { onFrame(f); },
        [this](const std::string& msg) { push({CmdType::SessionError, {}, msg}); }, delivered, error);
    if (!ok) {
        output_->close();
        // "In use" / privacy / unplugged can change by themselves: retry.
        return fail(error, true);
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        sessionOpen_ = true;
        activeDeviceId_ = id;
        selected_ = *mode;
        delivered_ = delivered;
        outW_ = outW;
        outH_ = outH;
        fpsCap_ = cap;
        note_ = note;
        state_ = "running";
        detail_ = "";
        failures_ = 0;
    }
    LOG_INFO("capture", "relay running: ", mode->key(), " -> ", outW, "x", outH,
             cap > 0 ? " capped at " + std::to_string(int(cap)) + " fps" : std::string());
}

void CaptureRelay::stopSession() {
    bool wasOpen;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        wasOpen = sessionOpen_;
        sessionOpen_ = false;
    }
    // Order matters: stop frame delivery before the window/GPU go away.
    backend_->close();
    output_->close();
    std::lock_guard<std::mutex> lock(stateMutex_);
    selected_.reset();
    delivered_.reset();
    outW_ = outH_ = 0;
    fpsCap_ = 0;
    activeDeviceId_.clear();
    if (state_ != "error") {
        state_ = "off";
        if (wasOpen) detail_.clear();
    }
}

void CaptureRelay::fail(const std::string& msg, bool retryable) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = "error";
        ++failures_;
        if (retryable && cfg_.enabled) {
            int delay = std::min(kRetryMaxMs, kRetryFirstMs << std::min(failures_ - 1, 3));
            retryAtMs_ = util::nowMs() + delay;
            detail_ = msg + " - retrying in " + std::to_string(delay / 1000) + " s";
        } else {
            retryAtMs_ = 0;
            detail_ = msg;
        }
    }
    LOG_WARN("capture", msg);
    publish();
}

void CaptureRelay::onFrame(const FrameView& f) {
    int64_t now = util::nowMs();
    bool show;
    {
        std::lock_guard<std::mutex> s(statsMutex_);
        ++frames_;
        lastFrameMs_ = now;
        captureRate_.tick(now);
        show = pacer_.shouldPresent(f.timestampUs);
        if (!show) ++paced_;
    }
    if (!show) return;
    bool ok = output_->present(f); // GPU upload + convert + Present (never waits for vsync)
    std::lock_guard<std::mutex> s(statsMutex_);
    if (ok) {
        ++presented_;
        outputRate_.tick(now);
    } else {
        ++dropped_;
    }
}

} // namespace usc::capture
