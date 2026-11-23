#pragma once

// Output side of the relay: a top-level window rendered through a D3D11
// swap chain (what OBS "Game Capture" hooks), plus an optional small preview.

#include "capture/CaptureTypes.hpp"
#include "capture/VideoBackend.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace usc::capture {

struct OutputState {
    bool open = false;
    bool visible = false;
    bool minimized = false;
    int width = 0;   // swap chain (= captured) size
    int height = 0;
    std::string error;
};

class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual bool supported() const = 0;
    // Creates the window + swap chain at outW x outH.
    virtual bool open(const std::string& title, int outW, int outH, bool keepAspect, std::string& error) = 0;
    // Called from the capture thread for frames the pacer lets through.
    // Returns false if the frame could not be shown.
    virtual bool present(const FrameView& frame) = 0;
    virtual void close() = 0;
    virtual OutputState state() const = 0;
    // Preview: only produced while requested recently (low rate, small).
    virtual void requestPreview() = 0;
    virtual std::optional<std::string> previewJpeg() const = 0;
    // The user closed the window.
    virtual void onUserClosed(std::function<void()> cb) = 0;
};

std::unique_ptr<OutputSink> createOutputSink();

} // namespace usc::capture
