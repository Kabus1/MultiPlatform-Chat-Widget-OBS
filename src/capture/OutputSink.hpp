#pragma once

// Output side of the relay: a top-level window rendered through a D3D11
// swap chain (what OBS "Game Capture" hooks), plus an optional small preview.

#include "capture/CaptureTypes.hpp"
#include "capture/VideoBackend.hpp"

#include <cstdint>
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
    std::string error;   // last rendering problem (shown in the UI)
    uint64_t presented = 0; // frames drawn and Present()ed (what OBS sees)
    uint64_t skipped = 0;   // replaced by a newer frame before they were drawn
    uint64_t failed = 0;    // could not be drawn (see `error`)
    double fps = 0;         // measured Present() rate
};

class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual bool supported() const = 0;
    // Creates the window + swap chain at outW x outH.
    virtual bool open(const std::string& title, int outW, int outH, bool keepAspect, std::string& error) = 0;
    // Called from the capture thread for frames the pacer lets through. Only
    // copies the frame into a latest-frame mailbox (never touches the GPU, never
    // blocks on it); a render thread draws and presents it. False = not open.
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
