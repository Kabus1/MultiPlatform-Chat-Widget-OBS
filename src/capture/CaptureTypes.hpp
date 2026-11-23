#pragma once

// Portable types and decisions for the video capture relay. Nothing in here
// touches a device or Windows, so it is unit-tested on every platform.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usc::capture {

struct Device {
    std::string id;   // symbolic link (stable across restarts / USB ports)
    std::string name; // friendly name, e.g. "Game Capture 4K60 Pro MK.2"
};

// One mode the hardware itself offers (from the device's native media types).
struct Mode {
    int width = 0;
    int height = 0;
    uint32_t fpsNum = 0;
    uint32_t fpsDen = 1;
    std::string subtype; // "NV12", "YUY2", "MJPG", "P010", "RGB32", ...

    double fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0.0; }
    // Stable identifier saved in settings: "1920x1080@60000/1001:NV12".
    std::string key() const;
    nlohmann::json toJson() const;
    bool operator==(const Mode& o) const {
        return width == o.width && height == o.height && fpsNum == o.fpsNum && fpsDen == o.fpsDen && subtype == o.subtype;
    }
};

// Relay settings (settings.json "capture" section).
struct RelayConfig {
    bool enabled = false;
    std::string deviceId;
    std::string deviceName;
    std::string mode = "auto";            // "auto" or Mode::key()
    std::string outputResolution = "source"; // "source" | "2160p" | "1440p" | "1080p" | "720p"
    int outputFps = 0;                    // 0 = match source; otherwise a cap (60, 100, 120, 144...)
    bool keepAspect = true;               // letterbox instead of stretching
    std::string windowTitle = "USC Capture Output";

    static RelayConfig fromJson(const nlohmann::json& j);
    bool sameSession(const RelayConfig& o) const; // would a restart be needed?
};

// Cost of handling a subtype: lower is better. NV12/YUY2 go straight to the
// GPU; everything else needs a decoder/converter on the CPU.
int subtypeCost(const std::string& subtype);
bool isPassThrough(const std::string& subtype); // NV12 / YUY2

// Target output height for a resolution setting (0 = match source).
int targetHeight(const std::string& outputResolution);

// Chooses the device mode to open. Only ever returns one of `modes` (never an
// unsupported combination). `note` explains fallbacks, e.g. a saved mode the
// device no longer offers.
std::optional<Mode> chooseMode(const std::vector<Mode>& modes, const RelayConfig& cfg, std::string& note);

// Output (swap chain) size: source size, or the target height at the
// source's aspect ratio, rounded to even numbers. Never upscales above 4K.
std::pair<int, int> outputSize(const RelayConfig& cfg, int srcWidth, int srcHeight);

// Removes duplicates and sorts: biggest, fastest, cheapest first.
void normalizeModes(std::vector<Mode>& modes);

// Drops frames to honour an FPS cap without drifting.
class FramePacer {
public:
    explicit FramePacer(double targetFps = 0) { reset(targetFps); }
    void reset(double targetFps);
    // timestampUs: capture time of the frame (microseconds, monotonic).
    bool shouldPresent(int64_t timestampUs);

private:
    int64_t intervalUs_ = 0;
    int64_t nextDueUs_ = -1;
};

// Measures a rate over a sliding one-second window.
class RateMeter {
public:
    void tick(int64_t nowMs);
    double rate(int64_t nowMs) const;

private:
    std::vector<int64_t> times_;
};

} // namespace usc::capture
