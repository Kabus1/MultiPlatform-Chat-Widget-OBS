#include "capture/VirtualCamSink.hpp"

#include "capture/CaptureTypes.hpp"
#include "vcam/VcamFrame.hpp"
#include "vcam/VcamShared.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include "capture/ComRef.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>

#include <vector>
#endif

namespace usc::capture {

nlohmann::json VcamInstallState::toJson() const {
    return {{"supported", supported},
            {"dllPath", dllPath},
            {"dllFound", !dllPath.empty()},
            {"userRegistered", userRegistered},
            {"machineRegistered", machineRegistered},
            {"installed", userRegistered || machineRegistered},
            {"registeredPath", registeredPath},
            {"pathMatches", pathMatches}};
}

#ifndef _WIN32

namespace {
class NullVcam : public VirtualCamSink {
public:
    bool supported() const override { return false; }
    bool open(int, int, uint32_t, uint32_t, std::string& error) override {
        error = "USC Virtual Camera is available on Windows only";
        return false;
    }
    void push(const FrameView&) override {}
    void close() override {}
    uint64_t framesWritten() const override { return 0; }
    double fps() const override { return 0; }
    void requestPreview() override {}
    std::optional<std::string> previewJpeg() const override { return std::nullopt; }
};
} // namespace

std::unique_ptr<VirtualCamSink> createVirtualCamSink() { return std::make_unique<NullVcam>(); }
VcamInstallState vcamInstallState() { return {}; }
bool vcamInstall(bool, std::string& error) {
    error = "USC Virtual Camera is available on Windows only";
    return false;
}
bool vcamUninstall(bool, std::string& error) {
    error = "USC Virtual Camera is available on Windows only";
    return false;
}

#else

namespace {

constexpr int kPreviewW = 640, kPreviewH = 360;
constexpr int64_t kPreviewIntervalMs = 200;
constexpr int64_t kPreviewIdleMs = 3000;
const wchar_t kClsid[] = L"{9A3C1F5E-4B7D-4E2A-9C61-5D8E2F7A1B34}";

std::string narrowW(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring dllPathW() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(MAX_PATH * 2));
    std::wstring exe(buf, n);
    size_t slash = exe.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) + L"usc_vcam.dll";
}

std::wstring readDefault(HKEY root, const std::wstring& path) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

bool keyExists(HKEY root, const std::wstring& path) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::string encodeJpeg(const std::vector<uint8_t>& bgr, int w, int h) {
    std::string out;
    ComRef<IWICImagingFactory> factory;
    ComRef<IStream> stream;
    ComRef<IWICBitmapEncoder> encoder;
    ComRef<IWICBitmapFrameEncode> frame;
    ComRef<IPropertyBag2> props;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  factory.putVoid());
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put());
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.put(), props.put());
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(w), UINT(h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(UINT(h), UINT(w * 3), UINT(bgr.size()), const_cast<BYTE*>(bgr.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    HGLOBAL global = nullptr;
    STATSTG st{};
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.get(), &global);
    if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
    if (SUCCEEDED(hr))
        if (void* p = GlobalLock(global)) {
            out.assign(static_cast<const char*>(p), size_t(st.cbSize.QuadPart));
            GlobalUnlock(global);
        }
    return out;
}

class SharedMemoryVcam : public VirtualCamSink {
public:
    ~SharedMemoryVcam() override {
        close();
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
    }

    bool supported() const override { return true; }

    bool open(int width, int height, uint32_t fpsNum, uint32_t fpsDen, std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (width < 2 || height < 2 || width > vcam::kMaxWidth || height > vcam::kMaxHeight) {
            error = "USC Virtual Camera supports up to 3840x2160";
            return false;
        }
        if (!view_) {
            // Pagefile-backed, session namespace ("Local\"): every program of
            // this user session can open it; nothing system-wide.
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                          DWORD(uint64_t(vcam::kRegionBytes) >> 32),
                                          DWORD(vcam::kRegionBytes & 0xFFFFFFFFu), vcam::kMappingName);
            if (!mapping_) {
                error = "Could not create the virtual camera's shared memory (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, vcam::kRegionBytes);
            if (!view_) {
                error = "Could not map the virtual camera's shared memory (error " + std::to_string(GetLastError()) + ")";
                CloseHandle(mapping_);
                mapping_ = nullptr;
                return false;
            }
        }
        writer_ = std::make_unique<vcam::FrameWriter>(view_);
        writer_->init(GetCurrentProcessId());
        writer_->setFormat(uint32_t(width), uint32_t(height), fpsNum, fpsDen ? fpsDen : 1);
        width_ = width;
        height_ = height;
        frames_ = 0;
        rate_ = RateMeter{};
        LOG_INFO("vcam", "USC Virtual Camera: ", width, "x", height, " @ ", fpsNum, "/", fpsDen);
        return true;
    }

    void push(const FrameView& f) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writer_ || !f.data || f.width <= 0 || f.height <= 0) return;
        const bool nv12 = f.subtype == "NV12", yuy2 = f.subtype == "YUY2";
        if (!nv12 && !yuy2) return;
        if (nv12 && f.uvOffset + size_t(f.pitch) * size_t((f.height + 1) / 2) > f.size) return;
        if (yuy2 && size_t(f.pitch) * size_t(f.height) > f.size) return;
        uint8_t* slot = writer_->begin(width_, height_);
        if (!slot) return;
        if (f.width == width_ && f.height == height_) {
            if (nv12) vcam::packNV12(f.data, f.pitch, f.uvOffset, f.width, f.height, slot);
            else vcam::yuy2ToNV12(f.data, f.pitch, f.width, f.height, slot);
        } else {
            staging_.resize(vcam::frameBytes(vcam::PixelFormat::NV12, f.width, f.height));
            if (nv12) vcam::packNV12(f.data, f.pitch, f.uvOffset, f.width, f.height, staging_.data());
            else vcam::yuy2ToNV12(f.data, f.pitch, f.width, f.height, staging_.data());
            vcam::scaleNV12(staging_.data(), f.width, f.height, slot, width_, height_);
        }
        const int64_t now = int64_t(GetTickCount64());
        writer_->commit(f.timestampUs * 10, now);
        ++frames_;
        rate_.tick(now);
        if (now - previewRequestedMs_.load() < kPreviewIdleMs && now - lastPreviewMs_ >= kPreviewIntervalMs) {
            lastPreviewMs_ = now;
            makePreview(slot);
        }
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_) writer_->detach(); // readers show "no signal" (mapping stays for a quick restart)
        writer_.reset();
        std::lock_guard<std::mutex> p(previewMutex_);
        previewJpeg_.clear();
    }

    uint64_t framesWritten() const override { return frames_; }
    double fps() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return rate_.rate(int64_t(GetTickCount64()));
    }
    void requestPreview() override { previewRequestedMs_ = int64_t(GetTickCount64()); }
    std::optional<std::string> previewJpeg() const override {
        std::lock_guard<std::mutex> lock(previewMutex_);
        if (previewJpeg_.empty()) return std::nullopt;
        return previewJpeg_;
    }

private:
    // Caller holds mutex_. Runs on the capture thread, 5 times a second at most.
    void makePreview(const uint8_t* nv12) {
        small_.resize(vcam::frameBytes(vcam::PixelFormat::NV12, kPreviewW, kPreviewH));
        vcam::scaleNV12(nv12, width_, height_, small_.data(), kPreviewW, kPreviewH);
        bgr_.resize(size_t(kPreviewW) * kPreviewH * 3);
        vcam::nv12ToBgr24(small_.data(), kPreviewW, kPreviewH, bgr_.data());
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string jpeg = encodeJpeg(bgr_, kPreviewW, kPreviewH);
        if (SUCCEEDED(co)) CoUninitialize();
        if (jpeg.empty()) return;
        std::lock_guard<std::mutex> p(previewMutex_);
        previewJpeg_ = std::move(jpeg);
    }

    mutable std::mutex mutex_;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    std::unique_ptr<vcam::FrameWriter> writer_;
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> staging_, small_, bgr_;
    std::atomic<uint64_t> frames_{0};
    RateMeter rate_;
    std::atomic<int64_t> previewRequestedMs_{0};
    int64_t lastPreviewMs_ = 0;
    mutable std::mutex previewMutex_;
    std::string previewJpeg_;
};

} // namespace

std::unique_ptr<VirtualCamSink> createVirtualCamSink() { return std::make_unique<SharedMemoryVcam>(); }

VcamInstallState vcamInstallState() {
    VcamInstallState s;
    s.supported = true;
    const std::wstring dll = dllPathW();
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) s.dllPath = narrowW(dll);
    const std::wstring server = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32";
    const std::wstring instance =
        std::wstring(L"Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\") + kClsid;
    std::wstring userPath = readDefault(HKEY_CURRENT_USER, server);
    std::wstring machinePath = readDefault(HKEY_LOCAL_MACHINE, server);
    s.userRegistered = !userPath.empty() && keyExists(HKEY_CURRENT_USER, instance);
    s.machineRegistered = !machinePath.empty() && keyExists(HKEY_LOCAL_MACHINE, instance);
    std::wstring active = s.userRegistered ? userPath : machinePath;
    s.registeredPath = narrowW(active);
    s.pathMatches = !active.empty() && _wcsicmp(active.c_str(), dll.c_str()) == 0;
    return s;
}

namespace {

bool callExport(const char* name, std::string& error) {
    const std::wstring dll = dllPathW();
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        error = "usc_vcam.dll was not found next to the app (" + narrowW(dll) + ")";
        return false;
    }
    using Fn = HRESULT(__stdcall*)();
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    HRESULT hr = fn ? fn() : E_NOINTERFACE;
    FreeLibrary(m);
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " (0x%08lX)", static_cast<unsigned long>(hr));
        error = std::string(name) + " failed" + buf;
        return false;
    }
    return true;
}

// regsvr32 [/u] /s "<dll>" as administrator: one UAC prompt, waits for it.
bool runRegsvr32Elevated(bool unregister, std::string& error) {
    const std::wstring dll = dllPathW();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "usc_vcam.dll was not found next to the app (" + narrowW(dll) + ")";
        return false;
    }
    std::wstring args = std::wstring(unregister ? L"/u " : L"") + L"/s \"" + dll + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD e = GetLastError();
        error = e == ERROR_CANCELLED ? "The administrator prompt was cancelled"
                                     : "Could not start regsvr32 (error " + std::to_string(e) + ")";
        return false;
    }
    DWORD wait = WaitForSingleObject(sei.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (wait != WAIT_OBJECT_0) {
        error = "regsvr32 did not finish";
        return false;
    }
    if (code != 0) {
        error = "regsvr32 failed (exit code " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

} // namespace

bool vcamInstall(bool allUsers, std::string& error) {
    bool ok = allUsers ? runRegsvr32Elevated(false, error) : callExport("UscVcamRegisterUser", error);
    if (ok) LOG_INFO("vcam", "USC Virtual Camera registered ", allUsers ? "for all users" : "for this user");
    return ok;
}

bool vcamUninstall(bool allUsers, std::string& error) {
    return allUsers ? runRegsvr32Elevated(true, error) : callExport("UscVcamUnregisterUser", error);
}

#endif

} // namespace usc::capture
