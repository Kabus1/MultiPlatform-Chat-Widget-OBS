#pragma once

// Second output of the capture relay: "USC Virtual Camera".
//
// The relay owns the capture card (one program per card is a Windows rule)
// and publishes every frame - uncompressed NV12, optionally scaled - into
// shared memory. usc_vcam.dll, loaded by OBS / TikTok Studio / any
// DirectShow camera user, reads it. No window, no Game Capture, about one
// frame of latency, any number of programs at once.

#include "capture/VideoBackend.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace usc::capture {

class VirtualCamSink {
public:
    virtual ~VirtualCamSink() = default;
    virtual bool supported() const = 0;
    // Publishes width x height at fpsNum/fpsDen (what the camera advertises).
    virtual bool open(int width, int height, uint32_t fpsNum, uint32_t fpsDen, std::string& error) = 0;
    // Called on the capture thread for each frame the pacer lets through.
    virtual void push(const FrameView& frame) = 0;
    // Readers switch to their "no signal" picture.
    virtual void close() = 0;
    virtual uint64_t framesWritten() const = 0;
    virtual double fps() const = 0;
    // Small JPEG preview, produced only while requested recently.
    virtual void requestPreview() = 0;
    virtual std::optional<std::string> previewJpeg() const = 0;
};

std::unique_ptr<VirtualCamSink> createVirtualCamSink();

// Registration of usc_vcam.dll (lives next to the executable).
struct VcamInstallState {
    bool supported = false;        // Windows
    std::string dllPath;           // expected location ("" if missing)
    bool userRegistered = false;   // HKCU (this user)
    bool machineRegistered = false;// HKLM (all users, via regsvr32)
    std::string registeredPath;    // DLL the registration points to
    bool pathMatches = false;      // registration points at this app's DLL
    nlohmann::json toJson() const;
};
VcamInstallState vcamInstallState();
// allUsers: runs "regsvr32 usc_vcam.dll" elevated (one UAC prompt);
// otherwise registers for the current user (no administrator rights).
bool vcamInstall(bool allUsers, std::string& error);
bool vcamUninstall(bool allUsers, std::string& error);

} // namespace usc::capture
