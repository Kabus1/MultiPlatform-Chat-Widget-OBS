#pragma once

// Device side of the relay: enumeration, mode probing and frame delivery.
// Windows: Media Foundation (MfVideoBackend.cpp). Elsewhere: a stub.

#include "capture/CaptureTypes.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace usc::capture {

// A frame as delivered by the device (system memory, valid only during the
// callback). NV12: Y plane `height` rows, then the UV plane at `uvOffset`.
struct FrameView {
    const uint8_t* data = nullptr;
    int pitch = 0;          // bytes per row
    size_t uvOffset = 0;    // NV12: byte offset of the UV plane
    int width = 0;
    int height = 0;
    std::string subtype;    // "NV12" | "YUY2"
    int64_t timestampUs = 0;
};

class VideoBackend {
public:
    using FrameSink = std::function<void(const FrameView&)>;
    using ErrorSink = std::function<void(const std::string&)>;

    virtual ~VideoBackend() = default;
    virtual bool supported() const = 0;
    // Lists capture devices WITHOUT opening them (safe while streaming).
    virtual std::vector<Device> enumerate(std::string& error) = 0;
    // Opens the device briefly to read its native modes, then releases it.
    virtual std::vector<Mode> probe(const std::string& deviceId, std::string& error) = 0;
    // Opens the device in exactly `mode` and starts asynchronous reading.
    // `delivered` receives the format frames will actually arrive in.
    virtual bool open(const std::string& deviceId, const Mode& mode, FrameSink onFrame, ErrorSink onError,
                      Mode& delivered, std::string& error) = 0;
    // Stops reading and releases the device (blocks until callbacks are done).
    virtual void close() = 0;
};

std::unique_ptr<VideoBackend> createVideoBackend();

} // namespace usc::capture
