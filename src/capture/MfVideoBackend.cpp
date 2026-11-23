// Windows capture backend: Media Foundation SourceReader in asynchronous mode.
#ifdef _WIN32

#include "capture/ComRef.hpp"
#include "capture/Guard.hpp"
#include "capture/VideoBackend.hpp"
#include "core/Log.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace usc::capture {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

// Errors the user can act on, in plain words.
std::string describe(HRESULT hr, const std::string& what) {
    std::string base = std::string(what) + " (" + hex(hr) + ")";
    switch (static_cast<unsigned long>(hr)) {
    case 0x80070005UL: // E_ACCESSDENIED
        return "Windows blocked camera access: allow it in Settings > Privacy & security > Camera > "
               "\"Let desktop apps access your camera\". " + base;
    case 0xC00D3704UL: // MF_E_HW_MFT_FAILED_START_STREAMING
    case 0xC00D3EA2UL: // MF_E_VIDEO_RECORDING_DEVICE_LOCKED
        return "The device is in use by another app (e.g. a Video Capture Device source in OBS or another "
               "program). Remove it there and capture this app's output window instead. " + base;
    case 0xC00D3E85UL: // MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
    case 0xC00D3EA3UL: // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
        return "The device was disconnected or taken over by another app. " + base;
    case 0xC00D5212UL: // MF_E_TOPO_CODEC_NOT_FOUND
    case 0xC00D6D60UL: // MF_E_TRANSFORM_TYPE_NOT_SET
        return "Windows has no converter for this device format - pick an NV12/YUY2 mode. " + base;
    default:
        return base;
    }
}

struct SubtypeName {
    const GUID* guid;
    const char* name;
};

const SubtypeName kSubtypes[] = {
    {&MFVideoFormat_NV12, "NV12"},   {&MFVideoFormat_YUY2, "YUY2"}, {&MFVideoFormat_MJPG, "MJPG"},
    {&MFVideoFormat_RGB32, "RGB32"}, {&MFVideoFormat_RGB24, "RGB24"}, {&MFVideoFormat_ARGB32, "ARGB32"},
    {&MFVideoFormat_P010, "P010"},   {&MFVideoFormat_I420, "I420"}, {&MFVideoFormat_IYUV, "IYUV"},
    {&MFVideoFormat_YV12, "YV12"},   {&MFVideoFormat_UYVY, "UYVY"},
};

std::string subtypeName(const GUID& g) {
    for (const auto& s : kSubtypes)
        if (IsEqualGUID(*s.guid, g)) return s.name;
    // Other FOURCC-based formats: Data1 is the FOURCC.
    char f[5] = {char(g.Data1 & 0xFF), char((g.Data1 >> 8) & 0xFF), char((g.Data1 >> 16) & 0xFF),
                 char((g.Data1 >> 24) & 0xFF), 0};
    for (int i = 0; i < 4; ++i)
        if (f[i] < 32 || f[i] > 126) return "0x" + hex(static_cast<HRESULT>(g.Data1)).substr(2);
    return f;
}

bool readMode(IMFMediaType* t, Mode& m) {
    GUID major{}, sub{};
    if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(major, MFMediaType_Video)) return false;
    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub))) return false;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return false;
    MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
    m.width = int(w);
    m.height = int(h);
    m.fpsNum = num;
    m.fpsDen = den ? den : 1;
    m.subtype = subtypeName(sub);
    return true;
}

HRESULT createSource(const std::string& id, ComRef<IMFMediaSource>& source) {
    ComRef<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(attrs.put(), 2);
    if (SUCCEEDED(hr))
        hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    std::wstring link = widen(id);
    if (SUCCEEDED(hr)) hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attrs.get(), source.put());
    return hr;
}

// State of one open capture session. Shared with the COM callback, so a late
// callback after close() can never touch freed memory.
struct Session {
    std::mutex mutex;
    std::condition_variable cv;
    bool running = false;
    bool readPending = false; // a ReadSample is outstanding
    int inCallback = 0;       // OnReadSample is executing (close() waits for 0)
    ComRef<IMFSourceReader> reader;
    ComRef<IMFMediaSource> source;
    VideoBackend::FrameSink onFrame;
    VideoBackend::ErrorSink onError;
    // Current delivered format (may change, e.g. HDMI input switches resolution).
    std::string subtype;
    int width = 0, height = 0;
    LONG defaultStride = 0;

    ComRef<IMFSourceReader> currentReader() {
        std::lock_guard<std::mutex> lock(mutex);
        return reader; // AddRef'd copy: close() may drop its own reference meanwhile
    }

    void readCurrentType() {
        ComRef<IMFMediaType> t;
        ComRef<IMFSourceReader> r = currentReader();
        if (r && SUCCEEDED(r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), t.put()))) {
            Mode m;
            if (readMode(t.get(), m)) {
                subtype = m.subtype;
                width = m.width;
                height = m.height;
            }
            UINT32 stride = 0;
            defaultStride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(INT32(stride)) : 0;
            if (defaultStride == 0) defaultStride = subtype == "YUY2" ? width * 2 : width;
        }
    }

    bool requestNext() {
        ComRef<IMFSourceReader> r;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || !reader) return false;
            r = reader;
            readPending = true;
        }
        HRESULT hr = r->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                readPending = false;
            }
            cv.notify_all();
            if (onError) onError(describe(hr, "Could not read from the device"));
            return false;
        }
        return true;
    }

    // Fills in the NV12 plane layout and checks that every byte a consumer
    // will read lies inside the buffer. Returns false for a bad frame.
    bool layout(FrameView& f) const {
        if (!f.data || f.pitch <= 0 || f.width <= 0 || f.height <= 0) return false;
        size_t pitch = size_t(f.pitch);
        if (subtype == "YUY2") {
            if (pitch < size_t(f.width) * 2) return false;
            return pitch * size_t(f.height) <= f.size;
        }
        if (subtype != "NV12" || pitch < size_t(f.width)) return false;
        size_t h = size_t(f.height), uvRows = (h + 1) / 2;
        // Drivers may pad the Y plane (e.g. 1080 -> 1088 rows): the UV plane
        // starts after all Y rows the buffer holds.
        size_t totalRows = f.size / pitch;
        size_t yRows = totalRows * 2 / 3;
        if (yRows < h || yRows * pitch + uvRows * pitch > f.size) yRows = h;
        f.uvOffset = yRows * pitch;
        return f.uvOffset + uvRows * pitch <= f.size;
    }

    void deliver(IMFSample* sample, LONGLONG ts100ns) {
        ComRef<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, buf.put()))) return;
        FrameView f;
        f.width = width;
        f.height = height;
        f.subtype = subtype;
        f.timestampUs = ts100ns / 10;
        ComRef<IMF2DBuffer2> b2;
        ComRef<IMF2DBuffer> b1;
        if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer2), b2.putVoid()))) {
            // Lock2DSize reports the real buffer extent next to the scanline pointer.
            BYTE *scan0 = nullptr, *start = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            if (FAILED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length))) return;
            if (scan0 && start && scan0 >= start && size_t(scan0 - start) <= length) {
                f.data = scan0;
                f.pitch = int(pitch);
                f.size = size_t(length) - size_t(scan0 - start);
                if (layout(f) && onFrame) onFrame(f);
                else badFrames++;
            }
            b2->Unlock2D();
        } else if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), b1.putVoid()))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (FAILED(b1->Lock2D(&scan0, &pitch))) return;
            // Top-down planar/packed YUV: scan0 is the start of the buffer.
            DWORD max = 0;
            buf->GetMaxLength(&max);
            f.data = scan0;
            f.pitch = int(pitch);
            f.size = max;
            if (pitch > 0 && layout(f) && onFrame) onFrame(f);
            else badFrames++;
            b1->Unlock2D();
        } else {
            BYTE* p = nullptr;
            DWORD maxLen = 0, len = 0;
            if (FAILED(buf->Lock(&p, &maxLen, &len))) return;
            f.data = p;
            f.size = len;
            f.pitch = int(defaultStride > 0 ? defaultStride : (subtype == "YUY2" ? width * 2 : width));
            if (layout(f) && onFrame) onFrame(f);
            else badFrames++;
            buf->Unlock();
        }
    }

    uint64_t badFrames = 0; // frames whose buffer did not match the format
};

class ReaderCallback : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(std::shared_ptr<Session> s) : session_(std::move(s)) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IMFSourceReaderCallback))) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override {
        auto& s = *session_;
        bool running;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.readPending = false;
            running = s.running;
            if (running) ++s.inCallback; // close() waits until we are out
        }
        if (!running) {
            s.cv.notify_all();
            return S_OK;
        }
        // Nothing may escape into Media Foundation's thread: an exception or
        // fault here would take the whole app down.
        std::string fault;
        guarded(
            [&] {
                if (FAILED(hr)) {
                    if (s.onError) s.onError(describe(hr, "The device stopped delivering video"));
                    return;
                }
                if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
                    if (s.onError) s.onError("The device stopped delivering video (unplugged or taken by another app).");
                    return;
                }
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) s.readCurrentType();
                if (sample) s.deliver(sample, ts);
                s.requestNext();
            },
            fault);
        if (!fault.empty() && s.onError) {
            std::string msg = "Frame handling failed: " + fault;
            guarded([&] { s.onError(msg); }, fault);
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            --s.inCallback;
        }
        s.cv.notify_all();
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    virtual ~ReaderCallback() = default;
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<Session> session_;
};

class MfVideoBackend : public VideoBackend {
public:
    MfVideoBackend() { started_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); }
    ~MfVideoBackend() override {
        close();
        if (started_) MFShutdown();
    }

    bool supported() const override { return started_; }

    std::vector<Device> enumerate(std::string& error) override {
        std::vector<Device> out;
        ComRef<IMFAttributes> attrs;
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFCreateAttributes(attrs.put(), 1);
        if (SUCCEEDED(hr))
            hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attrs.get(), &devices, &count);
        if (FAILED(hr)) {
            error = describe(hr, "Could not list video devices");
            return out;
        }
        for (UINT32 i = 0; i < count; ++i) {
            WCHAR* name = nullptr;
            WCHAR* link = nullptr;
            UINT32 len = 0;
            // Reading attributes does not activate (open) the device.
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) &&
                SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
                out.push_back({narrow(link), narrow(name)});
            CoTaskMemFree(name);
            CoTaskMemFree(link);
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        return out;
    }

    std::vector<Mode> probe(const std::string& id, std::string& error) override {
        std::vector<Mode> modes;
        ComRef<IMFMediaSource> source;
        HRESULT hr = createSource(id, source);
        if (FAILED(hr)) {
            error = describe(hr, "Could not open the device to read its modes");
            return modes;
        }
        ComRef<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromMediaSource(source.get(), nullptr, reader.put());
        if (SUCCEEDED(hr)) {
            for (DWORD i = 0;; ++i) {
                ComRef<IMFMediaType> t;
                if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, t.put()))) break;
                Mode m;
                if (readMode(t.get(), m)) modes.push_back(m);
            }
        } else {
            error = describe(hr, "Could not read the device's modes");
        }
        reader.reset();
        source->Shutdown(); // release the device right away
        normalizeModes(modes);
        if (modes.empty() && error.empty()) error = "The device reported no video modes.";
        return modes;
    }

    bool open(const std::string& id, const Mode& mode, FrameSink onFrame, ErrorSink onError, Mode& delivered,
              std::string& error) override {
        close();
        auto s = std::make_shared<Session>();
        s->onFrame = std::move(onFrame);
        s->onError = std::move(onError);
        HRESULT hr = createSource(id, s->source);
        if (FAILED(hr)) {
            error = describe(hr, "Could not open the device");
            return false;
        }
        // Session -> reader -> callback -> session is a reference cycle:
        // every failure path must drop the reader to break it.
        auto abort = [&](const std::string& msg) {
            s->reader.reset();
            if (s->source) s->source->Shutdown();
            s->source.reset();
            error = msg;
            return false;
        };
        auto* cb = new ReaderCallback(s);
        ComRef<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.put(), 3);
        if (SUCCEEDED(hr)) hr = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);
        // Only formats that are not NV12/YUY2 need Windows' decoder/converter.
        if (SUCCEEDED(hr) && !isPassThrough(mode.subtype))
            hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(s->source.get(), attrs.get(), s->reader.put());
        cb->Release(); // the reader holds its own reference
        if (FAILED(hr)) return abort(describe(hr, "Could not start reading from the device"));
        s->reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        s->reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        // 1. Put the device in one of its own native modes (never a made-up one).
        bool found = false;
        for (DWORD i = 0; !found; ++i) {
            ComRef<IMFMediaType> t;
            if (FAILED(s->reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, t.put()))) break;
            Mode m;
            if (readMode(t.get(), m) && m == mode) {
                hr = s->reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.get());
                found = SUCCEEDED(hr);
                if (!found) return abort(describe(hr, "The device refused its own mode " + mode.key()));
            }
        }
        if (!found) return abort("The device no longer offers " + mode.key() + " (re-probe the modes).");
        // 2. Formats other than NV12/YUY2 (MJPG, RGB, P010...): let Windows convert to NV12.
        if (!isPassThrough(mode.subtype)) {
            ComRef<IMFMediaType> t;
            hr = MFCreateMediaType(t.put());
            if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, UINT32(mode.width), UINT32(mode.height));
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, mode.fpsNum, mode.fpsDen);
            if (SUCCEEDED(hr)) hr = s->reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.get());
            if (FAILED(hr)) return abort(describe(hr, "Windows cannot convert " + mode.subtype + " to NV12"));
        }
        s->readCurrentType();
        delivered = mode;
        delivered.subtype = s->subtype;
        delivered.width = s->width;
        delivered.height = s->height;
        if (!isPassThrough(s->subtype)) return abort("Unexpected delivered format " + s->subtype);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->running = true;
        }
        if (!s->requestNext()) return abort("Could not start reading from the device");
        session_ = s;
        return true;
    }

    void close() override {
        auto s = std::move(session_);
        if (!s) return;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->running = false;
            // Let the in-flight read finish (at most one frame time)...
            s->cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return !s->readPending && s->inCallback == 0; });
        }
        // ...then release the device. Shutdown also completes a read that is
        // stuck because the input has no signal.
        if (s->source) s->source->Shutdown();
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return !s->readPending && s->inCallback == 0; });
        }
        // A callback that is still running past the timeouts keeps the Session
        // (shared_ptr) alive; with running == false it no longer touches the
        // reader, so releasing it here is safe.
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->reader.reset();
        }
        s->source.reset();
    }

private:
    bool started_ = false;
    std::shared_ptr<Session> session_;
};

} // namespace

std::unique_ptr<VideoBackend> createVideoBackend() { return std::make_unique<MfVideoBackend>(); }

} // namespace usc::capture

#endif
