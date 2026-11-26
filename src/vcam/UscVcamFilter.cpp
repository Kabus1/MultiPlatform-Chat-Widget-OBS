// usc_vcam.dll - "USC Virtual Camera": a DirectShow video capture source.
//
// Programs that list cameras through DirectShow (OBS "Video Capture Device",
// Chromium/Electron apps such as TikTok LIVE Studio, Zoom, Discord, Teams
// desktop...) load this DLL into their own process. Each instance reads the
// newest frame the app published in shared memory (vcam/VcamShared.hpp) and
// delivers it at the negotiated size / rate / format. Any number of programs
// can use the camera at the same time: nothing here owns a device.
//
// Written without the DirectShow base classes: one filter, one output pin
// (IPin, IAMStreamConfig, IKsPropertySet), a push thread per running graph.
#ifdef _WIN32

#include "vcam/VcamFrame.hpp"
#include "vcam/VcamShared.hpp"

#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <mmsystem.h>
#include <olectl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace usc::vcam;

// {9A3C1F5E-4B7D-4E2A-9C61-5D8E2F7A1B34}
const CLSID CLSID_UscVirtualCamera = {0x9a3c1f5e, 0x4b7d, 0x4e2a, {0x9c, 0x61, 0x5d, 0x8e, 0x2f, 0x7a, 0x1b, 0x34}};
const GUID MEDIASUBTYPE_I420_ = {0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const wchar_t kFilterName[] = L"USC Virtual Camera";
const wchar_t kPinName[] = L"Capture";

HINSTANCE g_module = nullptr;
std::atomic<long> g_objects{0}; // live COM objects (DllCanUnloadNow)
std::atomic<long> g_locks{0};

// ------------------------------------------------------------------ media types

struct Format {
    int width = 1920, height = 1080;
    REFERENCE_TIME interval = 166667; // 100 ns units (60 fps)
    PixelFormat pixel = PixelFormat::NV12;
};

const GUID& subtypeOf(PixelFormat p) {
    switch (p) {
    case PixelFormat::YUY2: return MEDIASUBTYPE_YUY2;
    case PixelFormat::I420: return MEDIASUBTYPE_I420_;
    default: return MEDIASUBTYPE_NV12;
    }
}

DWORD fourccOf(PixelFormat p) {
    switch (p) {
    case PixelFormat::YUY2: return MAKEFOURCC('Y', 'U', 'Y', '2');
    case PixelFormat::I420: return MAKEFOURCC('I', '4', '2', '0');
    default: return MAKEFOURCC('N', 'V', '1', '2');
    }
}

void freeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void deleteMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    freeMediaType(*mt);
    CoTaskMemFree(mt);
}

HRESULT copyMediaType(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    dst = src;
    if (src.cbFormat) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) {
            dst.cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    }
    if (dst.pUnk) dst.pUnk->AddRef();
    return S_OK;
}

// Fills `mt` (owning its format block) for a format.
HRESULT makeMediaType(const Format& f, AM_MEDIA_TYPE& mt) {
    ZeroMemory(&mt, sizeof(mt));
    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return E_OUTOFMEMORY;
    ZeroMemory(vih, sizeof(*vih));
    const DWORD bytes = DWORD(frameBytes(f.pixel, f.width, f.height));
    vih->AvgTimePerFrame = f.interval;
    vih->dwBitRate = DWORD(std::min<double>(4294967295.0, double(bytes) * 8 * (1e7 / double(f.interval))));
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = f.width;
    vih->bmiHeader.biHeight = f.height;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = f.pixel == PixelFormat::YUY2 ? 16 : 12;
    vih->bmiHeader.biCompression = fourccOf(f.pixel);
    vih->bmiHeader.biSizeImage = bytes;
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = subtypeOf(f.pixel);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = bytes;
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
    return S_OK;
}

// Accepts any size / rate in one of our three formats (the pin scales).
bool parseMediaType(const AM_MEDIA_TYPE* mt, Format& out) {
    if (!mt || mt->majortype != MEDIATYPE_Video) return false;
    PixelFormat p;
    if (mt->subtype == MEDIASUBTYPE_NV12) p = PixelFormat::NV12;
    else if (mt->subtype == MEDIASUBTYPE_YUY2) p = PixelFormat::YUY2;
    else if (mt->subtype == MEDIASUBTYPE_I420_ || mt->subtype == MEDIASUBTYPE_IYUV) p = PixelFormat::I420;
    else return false;
    const BITMAPINFOHEADER* bmi = nullptr;
    REFERENCE_TIME interval = 0;
    if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER) && mt->pbFormat) {
        auto* v = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        bmi = &v->bmiHeader;
        interval = v->AvgTimePerFrame;
    } else if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2) && mt->pbFormat) {
        auto* v = reinterpret_cast<const VIDEOINFOHEADER2*>(mt->pbFormat);
        bmi = &v->bmiHeader;
        interval = v->AvgTimePerFrame;
    } else {
        return false;
    }
    int w = bmi->biWidth, h = std::abs(bmi->biHeight);
    if (w < 2 || h < 2 || w > kMaxWidth || h > kMaxHeight || (w & 1) || (h & 1)) return false;
    out.width = w;
    out.height = h;
    out.interval = interval > 0 ? std::max<REFERENCE_TIME>(interval, 41667) /* <= 240 fps */ : 333333;
    out.pixel = p;
    return true;
}

// ------------------------------------------------------------------ shared memory (reader side)

class SharedSource {
public:
    ~SharedSource() { close(); }

    // Re-tries opening at most once a second (the app may start later).
    const void* map() {
        if (view_) return view_;
        ULONGLONG now = GetTickCount64();
        if (now - lastTry_ < 1000 && lastTry_) return nullptr;
        lastTry_ = now;
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
        if (!mapping_) return nullptr;
        view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kRegionBytes);
        if (!view_) close();
        return view_;
    }

    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

private:
    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
    ULONGLONG lastTry_ = 0;
};

// Formats offered: what the app publishes, plus common sizes at or below it.
std::vector<Format> offeredFormats() {
    int nw = 1920, nh = 1080;
    REFERENCE_TIME nativeInterval = 166667;
    SharedSource src;
    if (const void* base = src.map()) {
        FrameReader r(base);
        uint32_t w, h, fn, fd;
        if (r.format(w, h, fn, fd)) {
            nw = int(w) & ~1;
            nh = int(h) & ~1;
            nativeInterval = REFERENCE_TIME(std::max(41667.0, 1e7 * fd / fn));
        }
    }
    std::vector<std::pair<int, int>> sizes = {{nw, nh}};
    for (auto s : {std::pair<int, int>{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {640, 360}})
        if (s.first * s.second <= nw * nh && !(s.first == nw && s.second == nh)) sizes.push_back(s);
    std::vector<REFERENCE_TIME> rates = {nativeInterval};
    if (nativeInterval < 333333 - 1000) rates.push_back(333333); // also offer 30 fps
    std::vector<Format> out;
    for (PixelFormat p : {PixelFormat::NV12, PixelFormat::YUY2, PixelFormat::I420})
        for (auto s : sizes)
            for (auto r : rates) out.push_back({s.first, s.second, r, p});
    return out;
}

// ------------------------------------------------------------------ enumerators

class EnumMediaTypes : public IEnumMediaTypes {
public:
    EnumMediaTypes(std::vector<Format> formats, size_t pos = 0) : formats_(std::move(formats)), pos_(pos) { ++g_objects; }
    virtual ~EnumMediaTypes() { --g_objects; }

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** out, ULONG* fetched) override {
        if (!out) return E_POINTER;
        ULONG n = 0;
        while (n < count && pos_ < formats_.size()) {
            auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!mt || FAILED(makeMediaType(formats_[pos_], *mt))) {
                CoTaskMemFree(mt);
                break;
            }
            out[n++] = mt;
            ++pos_;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        pos_ = std::min(formats_.size(), pos_ + count);
        return pos_ < formats_.size() ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER;
        *out = new EnumMediaTypes(formats_, pos_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::vector<Format> formats_;
    size_t pos_;
};

class EnumPins : public IEnumPins {
public:
    explicit EnumPins(IPin* pin, bool done = false) : pin_(pin), done_(done) {
        pin_->AddRef();
        ++g_objects;
    }
    virtual ~EnumPins() {
        pin_->Release();
        --g_objects;
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG count, IPin** out, ULONG* fetched) override {
        if (!out) return E_POINTER;
        ULONG n = 0;
        if (count > 0 && !done_) {
            out[0] = pin_;
            pin_->AddRef();
            done_ = true;
            n = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        if (count > 0) done_ = true;
        return done_ && count > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override {
        done_ = false;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** out) override {
        if (!out) return E_POINTER;
        *out = new EnumPins(pin_, done_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    IPin* pin_;
    bool done_;
};

// ------------------------------------------------------------------ filter + pin

class Filter;

class OutputPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    explicit OutputPin(Filter* filter) : filter_(filter) {}
    ~OutputPin() {
        stopStreaming();
        disconnectInternal();
        freeMediaType(mt_);
        if (preferredSet_) freeMediaType(preferred_);
    }

    // IUnknown: a separate identity, lifetime of the filter.
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override {
        if (!pin) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        *pin = connected_;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        connected_->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!connected_) {
            ZeroMemory(pmt, sizeof(*pmt));
            return VFW_E_NOT_CONNECTED;
        }
        return copyMediaType(*pmt, mt_);
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override {
        if (!dir) return E_POINTER;
        *dir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
        if (!*id) return E_OUTOFMEMORY;
        memcpy(*id, kPinName, sizeof(kPinName));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override {
        Format f;
        return parseMediaType(pmt, f) ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER;
        *out = new ::EnumMediaTypes(formatsWithPreferredFirst());
        return S_OK;
    }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override {
        Format f;
        if (!pmt) return E_POINTER;
        if (!parseMediaType(pmt, f)) return VFW_E_INVALIDMEDIATYPE;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (connected_) {
            // Same format: fine. Anything else needs a reconnect by the graph.
            Format cur;
            if (parseMediaType(&mt_, cur) && cur.width == f.width && cur.height == f.height &&
                cur.pixel == f.pixel && cur.interval == f.interval)
                return S_OK;
            return VFW_E_ALREADY_CONNECTED;
        }
        if (preferredSet_) freeMediaType(preferred_);
        preferredSet_ = SUCCEEDED(makeMediaType(f, preferred_));
        return S_OK;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** out) override {
        if (!out) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!mt) return E_OUTOFMEMORY;
        HRESULT hr = connected_ ? copyMediaType(*mt, mt_)
                     : preferredSet_ ? copyMediaType(*mt, preferred_)
                                     : makeMediaType(offeredFormats().front(), *mt);
        if (FAILED(hr)) {
            CoTaskMemFree(mt);
            return hr;
        }
        *out = mt;
        return S_OK;
    }
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override {
        if (!count || !size) return E_POINTER;
        caps_ = offeredFormats();
        *count = int(caps_.size());
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** out, BYTE* scc) override {
        if (!out || !scc) return E_POINTER;
        if (caps_.empty()) caps_ = offeredFormats();
        if (index < 0 || size_t(index) >= caps_.size()) return S_FALSE;
        const Format& f = caps_[size_t(index)];
        auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!mt) return E_OUTOFMEMORY;
        if (FAILED(makeMediaType(f, *mt))) {
            CoTaskMemFree(mt);
            return E_OUTOFMEMORY;
        }
        auto* c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
        ZeroMemory(c, sizeof(*c));
        c->guid = FORMAT_VideoInfo;
        c->VideoStandard = 0;
        c->InputSize = {f.width, f.height};
        c->MinCroppingSize = c->MaxCroppingSize = {f.width, f.height};
        c->CropGranularityX = c->CropGranularityY = 1;
        c->CropAlignX = c->CropAlignY = 1;
        c->MinOutputSize = c->MaxOutputSize = {f.width, f.height};
        c->OutputGranularityX = c->OutputGranularityY = 0;
        c->MinFrameInterval = f.interval;
        c->MaxFrameInterval = 10000000; // anything down to 1 fps
        const LONG bits = LONG(frameBytes(f.pixel, f.width, f.height) * 8);
        c->MinBitsPerSecond = bits;
        c->MaxBitsPerSecond = LONG(std::min<double>(2147483647.0, double(bits) * (1e7 / double(f.interval))));
        *out = mt;
        return S_OK;
    }

    // IKsPropertySet: identifies this as a capture pin.
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD size, DWORD* returned) override {
        if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (returned) *returned = sizeof(GUID);
        if (!data) return returned ? S_OK : E_POINTER;
        if (size < sizeof(GUID)) return E_UNEXPECTED;
        *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
        return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override {
        if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
        if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
        if (support) *support = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }

    // Called by the filter on state changes.
    HRESULT active();                                // Stopped -> Paused: commit the allocator
    void inactive();                                 // -> Stopped: stop pushing, decommit
    HRESULT run(REFERENCE_TIME start, IReferenceClock* clock);
    void pauseStreaming() { paused_ = true; } // never blocks (see Filter::Pause)
    void stopStreaming();
    bool isConnected() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return connected_ != nullptr;
    }

private:
    std::vector<Format> formatsWithPreferredFirst() {
        std::vector<Format> f = offeredFormats();
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Format p;
        if (preferredSet_ && parseMediaType(&preferred_, p)) f.insert(f.begin(), p);
        return f;
    }
    HRESULT tryConnect(IPin* receivePin, const AM_MEDIA_TYPE& mt);
    HRESULT decideAllocator(IMemInputPin* input, const Format& f);
    void disconnectInternal();
    void pushLoop(Format f, IMemAllocator* alloc, IMemInputPin* input, IReferenceClock* clock, HANDLE stop,
                  HANDLE resume);
    void fill(const Format& f, BYTE* out);

    Filter* filter_;
    std::recursive_mutex mutex_;
    IPin* connected_ = nullptr;
    IMemInputPin* input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE mt_{};
    AM_MEDIA_TYPE preferred_{};
    bool preferredSet_ = false;
    std::vector<Format> caps_;

    // streaming
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE resumeEvent_ = nullptr;          // auto-reset: Run after Pause
    std::atomic<bool> paused_{false};
    std::atomic<REFERENCE_TIME> start_{0};  // stream start of the current Run
    SharedSource source_;
    std::vector<uint8_t> latest_, scaled_;
    int latestW_ = 0, latestH_ = 0;
    uint64_t latestFrame_ = 0;
};

class Filter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    Filter() : pin_(this) { ++g_objects; }
    virtual ~Filter() {
        if (state_ != State_Stopped) pin_.inactive();
        if (clock_) clock_->Release();
        --g_objects;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IPersist || iid == IID_IMediaFilter || iid == IID_IBaseFilter)
            *ppv = static_cast<IBaseFilter*>(this);
        else if (iid == IID_IAMFilterMiscFlags)
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* clsid) override {
        if (!clsid) return E_POINTER;
        *clsid = CLSID_UscVirtualCamera;
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (state_ != State_Stopped) pin_.inactive();
        state_ = State_Stopped;
        stateSnapshot_ = state_;
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (state_ == State_Stopped) {
            HRESULT hr = pin_.active();
            if (FAILED(hr)) return hr;
        } else if (state_ == State_Running) {
            // Must not wait for the push thread: DirectShow pauses the graph
            // (renderer first) before stopping it, and a paused renderer keeps
            // our thread inside its Receive() until it runs or stops. The
            // thread just stops sending; Stop() ends it later.
            pin_.pauseStreaming();
        }
        state_ = State_Paused;
        stateSnapshot_ = state_;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (state_ == State_Stopped) {
            HRESULT hr = Pause();
            if (FAILED(hr)) return hr;
        }
        start_ = start;
        if (state_ != State_Running) {
            HRESULT hr = pin_.run(start, clock_);
            if (FAILED(hr)) return hr;
        }
        state_ = State_Running;
        stateSnapshot_ = state_;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* state) override {
        if (!state) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        *state = state_;
        // A live source produces nothing while paused.
        return state_ == State_Paused ? VFW_S_CANT_CUE : S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (clock) clock->AddRef();
        if (clock_) clock_->Release();
        clock_ = clock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override {
        if (!clock) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        *clock = clock_;
        if (clock_) clock_->AddRef();
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** out) override {
        if (!out) return E_POINTER;
        *out = new ::EnumPins(&pin_);
        return S_OK;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override {
        if (!id || !pin) return E_POINTER;
        if (wcscmp(id, kPinName) != 0) {
            *pin = nullptr;
            return VFW_E_NOT_FOUND;
        }
        *pin = &pin_;
        pin_.AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override {
        if (!info) return E_POINTER;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        wcsncpy(info->achName, name_.empty() ? kFilterName : name_.c_str(), MAX_FILTER_NAME - 1);
        info->achName[MAX_FILTER_NAME - 1] = 0;
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        graph_ = graph; // weak: a filter never holds its graph
        name_ = name ? name : L"";
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

    // Lock-free read for the pin (avoids pin -> filter lock order).
    FILTER_STATE state() const { return stateSnapshot_.load(); }

private:
    friend class OutputPin;
    std::atomic<ULONG> refs_{1};
    std::recursive_mutex mutex_;
    FILTER_STATE state_ = State_Stopped;
    std::atomic<FILTER_STATE> stateSnapshot_{State_Stopped};
    IReferenceClock* clock_ = nullptr;
    IFilterGraph* graph_ = nullptr;
    std::wstring name_;
    REFERENCE_TIME start_ = 0;
    OutputPin pin_;
};


// ------------------------------------------------------------------ OutputPin implementation

STDMETHODIMP OutputPin::QueryInterface(REFIID iid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IPin)
        *ppv = static_cast<IPin*>(this);
    else if (iid == IID_IAMStreamConfig)
        *ppv = static_cast<IAMStreamConfig*>(this);
    else if (iid == IID_IKsPropertySet)
        *ppv = static_cast<IKsPropertySet*>(this);
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) OutputPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) OutputPin::Release() { return filter_->Release(); }

STDMETHODIMP OutputPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcsncpy(info->achName, kPinName, MAX_PIN_NAME - 1);
    info->achName[MAX_PIN_NAME - 1] = 0;
    return S_OK;
}

STDMETHODIMP OutputPin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt) {
    if (!receivePin) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;

    auto attempt = [&](const Format& f) {
        AM_MEDIA_TYPE mt;
        if (FAILED(makeMediaType(f, mt))) return E_OUTOFMEMORY;
        HRESULT hr = tryConnect(receivePin, mt);
        freeMediaType(mt);
        return hr;
    };
    // Fully specified by the caller: exactly that (or nothing).
    if (pmt && pmt->majortype != GUID_NULL && pmt->subtype != GUID_NULL && pmt->formattype != GUID_NULL && pmt->pbFormat) {
        Format f;
        if (!parseMediaType(pmt, f)) return VFW_E_TYPE_NOT_ACCEPTED;
        return attempt(f);
    }
    // Ours, the one chosen with IAMStreamConfig::SetFormat first.
    for (const Format& f : formatsWithPreferredFirst()) {
        if (pmt && pmt->majortype != GUID_NULL && pmt->majortype != MEDIATYPE_Video) break;
        if (pmt && pmt->subtype != GUID_NULL && pmt->subtype != subtypeOf(f.pixel)) continue;
        if (SUCCEEDED(attempt(f))) return S_OK;
    }
    // Then whatever the receiver proposes that we can produce.
    IEnumMediaTypes* e = nullptr;
    if (SUCCEEDED(receivePin->EnumMediaTypes(&e)) && e) {
        AM_MEDIA_TYPE* m = nullptr;
        while (e->Next(1, &m, nullptr) == S_OK) {
            Format f;
            bool ok = parseMediaType(m, f);
            deleteMediaType(m);
            if (ok && SUCCEEDED(attempt(f))) {
                e->Release();
                return S_OK;
            }
        }
        e->Release();
    }
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT OutputPin::tryConnect(IPin* receivePin, const AM_MEDIA_TYPE& mt) {
    connected_ = receivePin;
    connected_->AddRef();
    copyMediaType(mt_, mt);
    HRESULT hr = receivePin->ReceiveConnection(static_cast<IPin*>(this), &mt);
    if (SUCCEEDED(hr)) {
        IMemInputPin* input = nullptr;
        hr = receivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input));
        if (SUCCEEDED(hr) && input) {
            Format f;
            parseMediaType(&mt, f);
            hr = decideAllocator(input, f);
            if (SUCCEEDED(hr)) {
                input_ = input;
                return S_OK;
            }
            input->Release();
        }
        receivePin->Disconnect();
    }
    connected_->Release();
    connected_ = nullptr;
    freeMediaType(mt_);
    ZeroMemory(&mt_, sizeof(mt_));
    return FAILED(hr) ? hr : E_FAIL;
}

HRESULT OutputPin::decideAllocator(IMemInputPin* input, const Format& f) {
    const long need = long(frameBytes(f.pixel, f.width, f.height));
    ALLOCATOR_PROPERTIES req{};
    input->GetAllocatorRequirements(&req);
    ALLOCATOR_PROPERTIES want{};
    want.cBuffers = std::max<long>(req.cBuffers, 3);
    want.cbBuffer = std::max<long>(req.cbBuffer, need);
    want.cbAlign = std::max<long>(req.cbAlign, 1);
    want.cbPrefix = req.cbPrefix;
    auto use = [&](IMemAllocator* a) {
        ALLOCATOR_PROPERTIES actual{};
        if (FAILED(a->SetProperties(&want, &actual)) || actual.cbBuffer < need) return false;
        return SUCCEEDED(input->NotifyAllocator(a, FALSE));
    };
    IMemAllocator* a = nullptr;
    if (SUCCEEDED(input->GetAllocator(&a)) && a) {
        if (use(a)) {
            allocator_ = a;
            return S_OK;
        }
        a->Release();
        a = nullptr;
    }
    if (SUCCEEDED(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                   reinterpret_cast<void**>(&a))) && a) {
        if (use(a)) {
            allocator_ = a;
            return S_OK;
        }
        a->Release();
    }
    return VFW_E_NO_ALLOCATOR;
}

STDMETHODIMP OutputPin::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!connected_) return S_FALSE;
    if (filter_->state() != State_Stopped) return VFW_E_NOT_STOPPED;
    disconnectInternal();
    return S_OK;
}

void OutputPin::disconnectInternal() {
    stopStreaming();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (allocator_) {
        allocator_->Decommit();
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (input_) {
        input_->Release();
        input_ = nullptr;
    }
    if (connected_) {
        connected_->Release();
        connected_ = nullptr;
    }
    freeMediaType(mt_);
    ZeroMemory(&mt_, sizeof(mt_));
}

HRESULT OutputPin::active() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!connected_ || !allocator_) return S_OK; // unconnected pins are simply idle
    return allocator_->Commit();
}

void OutputPin::inactive() {
    stopStreaming();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (allocator_) allocator_->Decommit();
}

HRESULT OutputPin::run(REFERENCE_TIME start, IReferenceClock* clock) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!connected_ || !allocator_ || !input_) return S_OK;
    start_ = start;
    if (thread_.joinable()) { // Paused -> Running: the thread is still there
        paused_ = false;
        SetEvent(resumeEvent_);
        return S_OK;
    }
    Format f;
    if (!parseMediaType(&mt_, f)) return VFW_E_INVALIDMEDIATYPE;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    resumeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_ || !resumeEvent_) return HRESULT_FROM_WIN32(GetLastError());
    paused_ = false;
    allocator_->AddRef();
    input_->AddRef();
    if (clock) clock->AddRef();
    thread_ = std::thread([this, f, a = allocator_, in = input_, clock, stop = stopEvent_, resume = resumeEvent_] {
        pushLoop(f, a, in, clock, stop, resume);
    });
    return S_OK;
}

// Only from Stop() (downstream filters are already stopped, so a Receive()
// returns at once) and from teardown.
void OutputPin::stopStreaming() {
    std::thread t;
    HANDLE ev, resume;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        SetEvent(stopEvent_);
        // Unblocks a GetBuffer() waiting for a free sample.
        if (allocator_) allocator_->Decommit();
        t = std::move(thread_);
        ev = stopEvent_;
        resume = resumeEvent_;
        stopEvent_ = resumeEvent_ = nullptr;
    }
    t.join();
    CloseHandle(ev);
    CloseHandle(resume);
}

void OutputPin::pushLoop(Format f, IMemAllocator* alloc, IMemInputPin* input, IReferenceClock* clock, HANDLE stop,
                         HANDLE resume) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1); // 1 ms waits, so 60-144 fps pacing is even
    using clk = std::chrono::steady_clock;
    const auto interval = std::chrono::nanoseconds(int64_t(f.interval) * 100);
    const long bytes = long(frameBytes(f.pixel, f.width, f.height));
    auto next = clk::now();
    uint64_t n = 0;
    for (;;) {
        if (paused_) { // live source: nothing is sent while paused
            HANDLE events[2] = {stop, resume};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            next = clk::now();
            continue;
        }
        auto now = clk::now();
        DWORD waitMs = next > now ? DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()) : 0;
        if (WaitForSingleObject(stop, waitMs) == WAIT_OBJECT_0) break;
        if (clk::now() < next) continue; // woke a little early: finish the wait

        IMediaSample* sample = nullptr;
        if (FAILED(alloc->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample) break; // decommitted: stopping
        BYTE* data = nullptr;
        if (SUCCEEDED(sample->GetPointer(&data)) && data && sample->GetSize() >= bytes) {
            fill(f, data);
            sample->SetActualDataLength(bytes);
        } else {
            sample->SetActualDataLength(0);
        }
        // Stream time from the graph clock (a live source stamps "now").
        REFERENCE_TIME ts = REFERENCE_TIME(n) * f.interval;
        REFERENCE_TIME clockNow = 0;
        if (clock && SUCCEEDED(clock->GetTime(&clockNow))) ts = std::max<REFERENCE_TIME>(0, clockNow - start_.load());
        REFERENCE_TIME te = ts + f.interval;
        sample->SetTime(&ts, &te);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(n == 0);
        sample->SetPreroll(FALSE);
        HRESULT hr = input->Receive(sample);
        sample->Release();
        if (paused_) continue; // paused while we were inside Receive()
        if (hr != S_OK) break; // downstream is stopping or failed
        ++n;
        next += interval;
        if (clk::now() - next > interval * 4) next = clk::now(); // after a stall, don't burst
    }
    timeEndPeriod(1);
    input->Release();
    alloc->Release();
    if (clock) clock->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

void OutputPin::fill(const Format& f, BYTE* out) {
    const void* base = source_.map();
    bool have = false;
    if (base) {
        FrameReader reader(base);
        if (reader.writerAlive(int64_t(GetTickCount64()))) {
            if (latestW_ == 0 || reader.frameCounter() != latestFrame_) {
                if (latest_.size() < kSlotBytes) latest_.resize(kSlotBytes);
                int w = 0, h = 0;
                uint64_t n = 0;
                if (reader.readLatest(latest_.data(), latest_.size(), w, h, n)) {
                    latestW_ = w;
                    latestH_ = h;
                    latestFrame_ = n;
                }
            }
            have = latestW_ > 0;
        } else {
            latestW_ = latestH_ = 0; // the app stopped sending: "no signal"
        }
    }
    if (!have) {
        fillPlaceholder(f.pixel, f.width, f.height, out);
        return;
    }
    const uint8_t* nv12 = latest_.data();
    if (latestW_ != f.width || latestH_ != f.height) {
        scaled_.resize(frameBytes(PixelFormat::NV12, f.width, f.height));
        scaleNV12(latest_.data(), latestW_, latestH_, scaled_.data(), f.width, f.height);
        nv12 = scaled_.data();
    }
    convertFromNV12(nv12, f.width, f.height, f.pixel, out);
}

// ------------------------------------------------------------------ class factory

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; } // static object
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* f = new (std::nothrow) Filter();
        if (!f) return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(iid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

Factory g_factory;

// ------------------------------------------------------------------ registration

std::wstring clsidString() {
    wchar_t buf[64];
    StringFromGUID2(CLSID_UscVirtualCamera, buf, 64);
    return buf;
}

std::wstring modulePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(g_module, buf, DWORD(std::size(buf)));
    return std::wstring(buf, n);
}

LSTATUS setString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    LSTATUS st = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (st != ERROR_SUCCESS) return st;
    st = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st;
}

HRESULT writeClsid(HKEY root) {
    const std::wstring key = L"Software\\Classes\\CLSID\\" + clsidString();
    LSTATUS st = setString(root, key, nullptr, kFilterName);
    if (st == ERROR_SUCCESS) st = setString(root, key + L"\\InprocServer32", nullptr, modulePath());
    if (st == ERROR_SUCCESS) st = setString(root, key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return HRESULT_FROM_WIN32(st);
}

const wchar_t kVideoInputInstance[] = L"Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\";

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

} // namespace

// ------------------------------------------------------------------ exports

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_UscVirtualCamera) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// For all users (regsvr32, administrator): class + DirectShow video input
// category entry through the filter mapper (with full FilterData).
extern "C" HRESULT __stdcall DllRegisterServer() {
    HRESULT hr = writeClsid(HKEY_LOCAL_MACHINE);
    if (FAILED(hr)) return hr;
    ComScope com;
    IFilterMapper2* mapper = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                          reinterpret_cast<void**>(&mapper));
    if (FAILED(hr)) return hr;
    static const REGPINTYPES types[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NV12},
                                        {&MEDIATYPE_Video, &MEDIASUBTYPE_YUY2},
                                        {&MEDIATYPE_Video, &MEDIASUBTYPE_I420_}};
    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = 3;
    pin.lpMediaType = types;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
    REGFILTER2 rf{};
    rf.dwVersion = 2;
    rf.dwMerit = MERIT_DO_NOT_USE;
    rf.cPins2 = 1;
    rf.rgPins2 = &pin;
    hr = mapper->RegisterFilter(CLSID_UscVirtualCamera, kFilterName, nullptr, &CLSID_VideoInputDeviceCategory, nullptr, &rf);
    mapper->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    ComScope com;
    IFilterMapper2* mapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                   reinterpret_cast<void**>(&mapper)))) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_UscVirtualCamera);
        mapper->Release();
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"Software\\Classes\\CLSID\\" + clsidString()).c_str());
    return S_OK;
}

// For the current user only (no administrator rights): the same keys under
// HKEY_CURRENT_USER. Programs running as this user see the camera.
extern "C" HRESULT __stdcall UscVcamRegisterUser() {
    HRESULT hr = writeClsid(HKEY_CURRENT_USER);
    if (FAILED(hr)) return hr;
    const std::wstring inst = kVideoInputInstance + clsidString();
    LSTATUS st = setString(HKEY_CURRENT_USER, inst, L"FriendlyName", kFilterName);
    if (st == ERROR_SUCCESS) st = setString(HKEY_CURRENT_USER, inst, L"CLSID", clsidString());
    return HRESULT_FROM_WIN32(st);
}

extern "C" HRESULT __stdcall UscVcamUnregisterUser() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (kVideoInputInstance + clsidString()).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\CLSID\\" + clsidString()).c_str());
    return S_OK;
}

#endif
