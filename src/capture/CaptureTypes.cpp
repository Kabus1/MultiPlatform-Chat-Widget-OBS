#include "capture/CaptureTypes.hpp"

#include <cctype>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace usc::capture {

using nlohmann::json;

std::string Mode::key() const {
    return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(fpsNum) + "/" +
           std::to_string(fpsDen) + ":" + subtype;
}

json Mode::toJson() const {
    char fps[16];
    std::snprintf(fps, sizeof(fps), "%.2f", this->fps());
    return {{"key", key()}, {"width", width}, {"height", height}, {"fps", std::stod(fps)},
            {"subtype", subtype}, {"passThrough", isPassThrough(subtype)}};
}

namespace {
// Settings come from a user-editable file: a wrong type falls back to the
// default instead of throwing (json::value() throws on a type mismatch).
bool getBool(const json& j, const char* k, bool d) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : d;
}
std::string getStr(const json& j, const char* k, const std::string& d) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : d;
}
int getInt(const json& j, const char* k, int d) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return d;
    double v = it->get<double>();
    return v < 0 ? 0 : v > 1000 ? 1000 : int(v);
}
} // namespace

RelayConfig RelayConfig::fromJson(const json& j) {
    RelayConfig c;
    if (!j.is_object()) return c;
    c.enabled = getBool(j, "enabled", false);
    c.deviceId = getStr(j, "deviceId", "");
    c.deviceName = getStr(j, "deviceName", "");
    c.mode = getStr(j, "mode", "auto");
    if (c.mode.empty()) c.mode = "auto";
    c.outputResolution = getStr(j, "outputResolution", "source");
    c.outputFps = getInt(j, "outputFps", 0);
    c.keepAspect = getBool(j, "keepAspect", true);
    c.windowTitle = getStr(j, "windowTitle", "USC Capture Output");
    c.virtualCam = getBool(j, "virtualCam", true);
    c.outputWindow = getBool(j, "outputWindow", false);
    if (c.windowTitle.empty()) c.windowTitle = "USC Capture Output";
    return c;
}

bool RelayConfig::sameSession(const RelayConfig& o) const {
    return enabled == o.enabled && deviceId == o.deviceId && mode == o.mode &&
           outputResolution == o.outputResolution && outputFps == o.outputFps && keepAspect == o.keepAspect &&
           windowTitle == o.windowTitle && virtualCam == o.virtualCam && outputWindow == o.outputWindow;
}

bool isOwnVirtualCamera(const std::string& name) { return name == "USC Virtual Camera"; }

bool isVirtualDevice(const std::string& link, const std::string& name) {
    std::string l, n;
    for (char c : link) l += char(std::tolower(static_cast<unsigned char>(c)));
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    return l.rfind("\\\\?\\swd#", 0) == 0 || n.find("virtual") != std::string::npos || isOwnVirtualCamera(name);
}

CaptureError classifyCaptureError(const std::string& m) {
    auto has = [&](const char* code) { return m.find(code) != std::string::npos; };
    // MF_E_HW_MFT_FAILED_START_STREAMING, MF_E_VIDEO_RECORDING_DEVICE_LOCKED,
    // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED: another program owns the device.
    if (has("0xC00D3704") || has("0xC00D3EA2") || has("0xC00D3EA3")) return CaptureError::Busy;
    if (has("0x80070005")) return CaptureError::Privacy; // E_ACCESSDENIED
    if (has("0xC00D3E85")) return CaptureError::Gone;    // MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
    return CaptureError::Other;
}

int subtypeCost(const std::string& s) {
    if (s == "NV12") return 0;
    if (s == "YUY2") return 1;
    if (s == "P010" || s == "I420" || s == "IYUV" || s == "YV12" || s == "UYVY") return 2;
    if (s == "RGB32" || s == "ARGB32" || s == "RGB24") return 3;
    if (s == "MJPG") return 4; // needs a JPEG decode per frame
    return 5;
}

bool isPassThrough(const std::string& s) { return s == "NV12" || s == "YUY2"; }

int targetHeight(const std::string& r) {
    if (r == "2160p" || r == "4k") return 2160;
    if (r == "1440p") return 1440;
    if (r == "1080p") return 1080;
    if (r == "720p") return 720;
    return 0;
}

void normalizeModes(std::vector<Mode>& modes) {
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        if (a.height != b.height) return a.height > b.height;
        if (a.width != b.width) return a.width > b.width;
        if (std::abs(a.fps() - b.fps()) > 0.01) return a.fps() > b.fps();
        return subtypeCost(a.subtype) < subtypeCost(b.subtype);
    });
    modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
}

std::optional<Mode> chooseMode(const std::vector<Mode>& modes, const RelayConfig& cfg, std::string& note) {
    note.clear();
    if (modes.empty()) return std::nullopt;
    if (cfg.mode != "auto" && !cfg.mode.empty()) {
        for (const auto& m : modes)
            if (m.key() == cfg.mode) return m;
        note = "The saved capture mode (" + cfg.mode + ") is not offered by this device now - using automatic selection.";
    }

    const int wantH = targetHeight(cfg.outputResolution); // 0 = as large as possible
    const double wantFps = cfg.outputFps;                  // 0 = as fast as possible
    auto fits = [&](const Mode& m) { return wantH == 0 || m.height <= wantH; };
    bool anyFits = std::any_of(modes.begin(), modes.end(), fits);

    // Lexicographic preference:
    //  1. height within the output target (no capturing more than is shown)
    //  2. reaches the FPS target
    //  3. larger height (detail)
    //  4. FPS: closest above the cap (no wasted frames), or highest without a cap
    //  5. cheaper pixel format (NV12/YUY2 over MJPG)
    auto better = [&](const Mode& a, const Mode& b) {
        if (anyFits && fits(a) != fits(b)) return fits(a);
        if (!anyFits && a.height != b.height) return a.height < b.height; // nothing fits: smallest above
        bool aFast = wantFps <= 0 || a.fps() >= wantFps - 0.5;
        bool bFast = wantFps <= 0 || b.fps() >= wantFps - 0.5;
        if (aFast != bFast) return aFast;
        if (a.height != b.height) return a.height > b.height;
        if (std::abs(a.fps() - b.fps()) > 0.5) { // 59.94 ~ 60: let the format decide
            if (wantFps > 0 && aFast) return a.fps() < b.fps(); // both reach the cap: less waste
            return a.fps() > b.fps();
        }
        return subtypeCost(a.subtype) < subtypeCost(b.subtype);
    };
    return *std::min_element(modes.begin(), modes.end(), better);
}

std::pair<int, int> outputSize(const RelayConfig& cfg, int srcW, int srcH) {
    if (srcW <= 0 || srcH <= 0) return {1920, 1080};
    int h = targetHeight(cfg.outputResolution);
    if (h == 0) h = srcH;
    h = std::min(h, 2160);
    int w = int(std::lround(double(h) * srcW / srcH));
    w = std::min(w, 4096);
    return {w & ~1, h & ~1};
}

void FramePacer::reset(double targetFps) {
    intervalUs_ = targetFps > 0 ? int64_t(1e6 / targetFps) : 0;
    nextDueUs_ = -1;
}

bool FramePacer::shouldPresent(int64_t ts) {
    if (intervalUs_ <= 0) return true;
    // Allow 10% early so 120 fps capped at 120 never drops due to jitter.
    if (nextDueUs_ >= 0 && ts < nextDueUs_ - intervalUs_ / 10) return false;
    // Advance by whole intervals; resync after a gap so we never burst.
    nextDueUs_ = nextDueUs_ < 0 || ts - nextDueUs_ > intervalUs_ ? ts + intervalUs_ : nextDueUs_ + intervalUs_;
    return true;
}

void RateMeter::tick(int64_t now) {
    times_.push_back(now);
    if (times_.size() > 512) times_.erase(times_.begin(), times_.begin() + 256);
}

double RateMeter::rate(int64_t now) const {
    int n = 0;
    for (auto it = times_.rbegin(); it != times_.rend() && now - *it <= 1000; ++it) ++n;
    return n;
}

} // namespace usc::capture
