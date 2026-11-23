// Non-Windows builds: the relay is a Windows feature (Media Foundation +
// Direct3D 11). These stubs keep the rest of the app identical.
#ifndef _WIN32

#include "capture/OutputSink.hpp"
#include "capture/VideoBackend.hpp"

namespace usc::capture {

namespace {
class NullBackend : public VideoBackend {
public:
    bool supported() const override { return false; }
    std::vector<Device> enumerate(std::string& error) override {
        error = "Video capture relay is available on Windows only";
        return {};
    }
    std::vector<Mode> probe(const std::string&, std::string& error) override {
        error = "Video capture relay is available on Windows only";
        return {};
    }
    bool open(const std::string&, const Mode&, FrameSink, ErrorSink, Mode&, std::string& error) override {
        error = "Video capture relay is available on Windows only";
        return false;
    }
    void close() override {}
};

class NullSink : public OutputSink {
public:
    bool supported() const override { return false; }
    bool open(const std::string&, int, int, bool, std::string& error) override {
        error = "Output window is available on Windows only";
        return false;
    }
    bool present(const FrameView&) override { return false; }
    void close() override {}
    OutputState state() const override { return {}; }
    void requestPreview() override {}
    std::optional<std::string> previewJpeg() const override { return std::nullopt; }
    void onUserClosed(std::function<void()>) override {}
};
} // namespace

std::unique_ptr<VideoBackend> createVideoBackend() { return std::make_unique<NullBackend>(); }
std::unique_ptr<OutputSink> createOutputSink() { return std::make_unique<NullSink>(); }

} // namespace usc::capture

#endif
