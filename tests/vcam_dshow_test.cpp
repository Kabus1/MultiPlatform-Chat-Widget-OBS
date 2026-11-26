// End-to-end test of USC Virtual Camera through DirectShow - what OBS,
// TikTok Studio or a browser do when they open a camera:
//   app side:  VirtualCamSink publishes frames (a counting test pattern)
//   camera:    usc_vcam.dll registered, found by the system device enumerator
//   consumers: two independent graphs (two "programs") negotiate 1280x720
//              YUY2 @ 30 fps and receive frames at the same time.
// Usage: usc_vcam_test.exe [--machine]   (default registers for this user)
// Exit code 0 = pass.
#include "capture/VirtualCamSink.hpp"

#include <windows.h>
#include <dshow.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// qedit.h is not shipped with current SDKs: the two interfaces we need.
static const CLSID CLSID_SampleGrabber_ = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const CLSID CLSID_NullRenderer_ = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const IID IID_ISampleGrabber_ = {0x6B652FFF, 0x11FE, 0x4fce, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
static const IID IID_ISampleGrabberCB_ = {0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

struct ISampleGrabberCB_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE* buffer, long len) = 0;
};
struct ISampleGrabber_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB_*, long which) = 0;
};

static int failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);                                                 \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

struct Counter : ISampleGrabberCB_ {
    std::atomic<long> frames{0}, wrongSize{0};
    std::atomic<int> lastY{-1}, lastU{-1};
    long expectBytes = 0;
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == IID_IUnknown || iid == IID_ISampleGrabberCB_) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* buf, long len) override {
        ++frames;
        if (len != expectBytes) ++wrongSize;
        if (buf && len >= 4) {
            lastY = buf[len / 2 & ~3];      // YUY2: Y0 U Y1 V
            lastU = buf[(len / 2 & ~3) + 1];
        }
        return S_OK;
    }
};

static IPin* firstPin(IBaseFilter* f, PIN_DIRECTION dir) {
    IEnumPins* e = nullptr;
    IPin* found = nullptr;
    if (FAILED(f->EnumPins(&e))) return nullptr;
    IPin* p = nullptr;
    while (!found && e->Next(1, &p, nullptr) == S_OK) {
        PIN_DIRECTION d;
        p->QueryDirection(&d);
        if (d == dir) found = p;
        else p->Release();
    }
    e->Release();
    return found;
}

static IBaseFilter* findCamera(const wchar_t* name) {
    ICreateDevEnum* devs = nullptr;
    IEnumMoniker* e = nullptr;
    IBaseFilter* filter = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                                reinterpret_cast<void**>(&devs))))
        return nullptr;
    if (devs->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &e, 0) == S_OK) {
        IMoniker* m = nullptr;
        while (!filter && e->Next(1, &m, nullptr) == S_OK) {
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void**>(&bag)))) {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr))) {
                    std::wprintf(L"  camera: %ls\n", v.bstrVal);
                    if (wcscmp(v.bstrVal, name) == 0)
                        m->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter));
                }
                VariantClear(&v);
                bag->Release();
            }
            m->Release();
        }
        e->Release();
    }
    devs->Release();
    return filter;
}

struct Consumer {
    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    Counter counter;
    bool ok = false;

    bool build(int w, int h) {
        IBaseFilter* cam = findCamera(L"USC Virtual Camera");
        EXPECT(cam, "USC Virtual Camera is not listed by the system device enumerator");
        if (!cam) return false;
        CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, reinterpret_cast<void**>(&graph));
        graph->AddFilter(cam, L"camera");
        IPin* out = firstPin(cam, PINDIR_OUTPUT);
        EXPECT(out, "camera has no output pin");

        // Capture pin category (OBS / Chromium look for it).
        IKsPropertySet* ks = nullptr;
        GUID cat = GUID_NULL;
        DWORD ret = 0;
        if (SUCCEEDED(out->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&ks)))) {
            ks->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &cat, sizeof(cat), &ret);
            ks->Release();
        }
        EXPECT(cat == PIN_CATEGORY_CAPTURE, "pin category is not PIN_CATEGORY_CAPTURE");

        // Capabilities + choose 1280x720 YUY2 @ 30 fps (as a consumer would).
        IAMStreamConfig* cfg = nullptr;
        out->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg));
        EXPECT(cfg, "no IAMStreamConfig");
        int count = 0, size = 0;
        cfg->GetNumberOfCapabilities(&count, &size);
        std::printf("  capabilities: %d\n", count);
        EXPECT(count >= 6 && size == sizeof(VIDEO_STREAM_CONFIG_CAPS), "count %d size %d", count, size);
        AM_MEDIA_TYPE* chosen = nullptr;
        std::vector<BYTE> caps(size_t(size > 0 ? size : 1));
        for (int i = 0; i < count && !chosen; ++i) {
            AM_MEDIA_TYPE* mt = nullptr;
            if (cfg->GetStreamCaps(i, &mt, caps.data()) != S_OK) continue;
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
            if (mt->subtype == MEDIASUBTYPE_YUY2 && vih->bmiHeader.biWidth == w && vih->bmiHeader.biHeight == h) {
                vih->AvgTimePerFrame = 333333;
                chosen = mt;
            } else {
                if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
                CoTaskMemFree(mt);
            }
        }
        EXPECT(chosen, "no %dx%d YUY2 capability", w, h);
        if (!chosen) return false;
        EXPECT(SUCCEEDED(cfg->SetFormat(chosen)), "SetFormat failed");

        // Sample grabber (accepts exactly that type) -> null renderer.
        IBaseFilter* grabberF = nullptr;
        IBaseFilter* sink = nullptr;
        ISampleGrabber_* grabber = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SampleGrabber_, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                                      reinterpret_cast<void**>(&grabberF));
        EXPECT(SUCCEEDED(hr), "SampleGrabber unavailable 0x%08lx", hr);
        grabberF->QueryInterface(IID_ISampleGrabber_, reinterpret_cast<void**>(&grabber));
        grabber->SetMediaType(chosen);
        counter.expectBytes = long(w) * h * 2;
        grabber->SetCallback(&counter, 1);
        graph->AddFilter(grabberF, L"grabber");
        CoCreateInstance(CLSID_NullRenderer_, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, reinterpret_cast<void**>(&sink));
        graph->AddFilter(sink, L"sink");
        IPin* gIn = firstPin(grabberF, PINDIR_INPUT);
        IPin* gOut = firstPin(grabberF, PINDIR_OUTPUT);
        IPin* sIn = firstPin(sink, PINDIR_INPUT);
        hr = graph->ConnectDirect(out, gIn, nullptr); // as OBS / Chromium do
        EXPECT(SUCCEEDED(hr), "camera -> grabber connect 0x%08lx", hr);
        hr = graph->ConnectDirect(gOut, sIn, nullptr);
        EXPECT(SUCCEEDED(hr), "grabber -> sink connect 0x%08lx", hr);
        AM_MEDIA_TYPE now{};
        if (SUCCEEDED(out->ConnectionMediaType(&now))) {
            auto* v = reinterpret_cast<VIDEOINFOHEADER*>(now.pbFormat);
            EXPECT(now.subtype == MEDIASUBTYPE_YUY2 && v->bmiHeader.biWidth == w && v->AvgTimePerFrame == 333333,
                   "connected with the wrong format");
            CoTaskMemFree(now.pbFormat);
        }
        graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&control));
        for (IUnknown* u : std::initializer_list<IUnknown*>{gIn, gOut, sIn, out, cfg, grabber, grabberF, sink, cam})
            if (u) u->Release();
        CoTaskMemFree(chosen->pbFormat);
        CoTaskMemFree(chosen);
        ok = SUCCEEDED(hr);
        return ok;
    }
    void release() {
        if (control) control->Release();
        if (graph) graph->Release();
        control = nullptr;
        graph = nullptr;
    }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress stays visible if something hangs
    bool machine = argc > 1 && std::string(argv[1]) == "--machine";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 1. Register the camera.
    std::string err;
    bool reg = usc::capture::vcamInstall(machine, err);
    if (!reg && !machine) { // e.g. a system where only machine-wide works
        std::printf("  per-user registration failed (%s), trying DllRegisterServer in-process\n", err.c_str());
    }
    EXPECT(reg, "registration failed: %s", err.c_str());
    auto st = usc::capture::vcamInstallState();
    std::printf("  installed: user=%d machine=%d pathMatches=%d dll=%s\n", st.userRegistered, st.machineRegistered,
                st.pathMatches, st.dllPath.c_str());

    // 2. The app publishes 1920x1080 @ 60 (the camera scales to what is asked).
    auto sink = usc::capture::createVirtualCamSink();
    EXPECT(sink->open(1920, 1080, 60, 1, err), "sink open: %s", err.c_str());
    std::atomic<bool> stop{false};
    std::atomic<int> pattern{100};
    std::thread writer([&] {
        std::vector<uint8_t> frame(size_t(1920) * 1080 * 3 / 2);
        int64_t t = 0;
        while (!stop) {
            std::fill(frame.begin(), frame.begin() + 1920 * 1080, uint8_t(pattern.load())); // Y
            std::fill(frame.begin() + 1920 * 1080, frame.end(), uint8_t(90));                // U = V = 90
            usc::capture::FrameView f;
            f.data = frame.data();
            f.size = frame.size();
            f.pitch = 1920;
            f.uvOffset = size_t(1920) * 1080;
            f.width = 1920;
            f.height = 1080;
            f.subtype = "NV12";
            f.timestampUs = (t += 16667);
            sink->push(f);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    // 3. Two consumers at the same time (two programs using the camera).
    Consumer a, b;
    bool builtA = a.build(1280, 720), builtB = b.build(640, 360);
    b.counter.expectBytes = 640 * 360 * 2;
    if (builtA && builtB) {
        a.control->Run();
        b.control->Run();
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        long fa = a.counter.frames, fb = b.counter.frames;
        std::printf("  consumer A (1280x720 YUY2 30fps): %ld frames, Y=%d U=%d\n", fa, a.counter.lastY.load(), a.counter.lastU.load());
        std::printf("  consumer B (640x360 YUY2 30fps):  %ld frames, Y=%d\n", fb, b.counter.lastY.load());
        EXPECT(fa >= 40 && fa <= 75, "A frame count %ld (expected ~60)", fa);
        EXPECT(fb >= 40 && fb <= 75, "B frame count %ld (expected ~60)", fb);
        EXPECT(a.counter.wrongSize == 0 && b.counter.wrongSize == 0, "wrong sample sizes");
        EXPECT(a.counter.lastY == 100 && a.counter.lastU == 90, "A pixels Y=%d U=%d", a.counter.lastY.load(), a.counter.lastU.load());
        EXPECT(b.counter.lastY == 100, "B pixels Y=%d", b.counter.lastY.load());

        std::printf("  step: live update\n");
        // 4. Live updates reach the consumers.
        pattern = 180;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT(a.counter.lastY == 180, "A did not follow the new frame (Y=%d)", a.counter.lastY.load());

        // 5. The app stops sending: "no signal" picture, the camera keeps running.
        std::printf("  step: app stops sending\n");
        stop = true;
        writer.join();
        sink->close();
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        long before = a.counter.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EXPECT(a.counter.frames > before, "frames stopped after the app went away");
        EXPECT(a.counter.lastY == 40, "placeholder expected (Y=%d)", a.counter.lastY.load());

        // 6. Stop -> Run again (programs do this when switching scenes/settings).
        std::printf("  step: stop / run\n");
        a.control->Stop();
        long stopped = a.counter.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT(a.counter.frames == stopped, "frames delivered while stopped");
        a.control->Run();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EXPECT(a.counter.frames > stopped + 5, "no frames after re-running");
        // 7. Pause -> Run (must not block: DirectShow pauses before stopping).
        std::printf("  step: pause / resume\n");
        auto t0 = std::chrono::steady_clock::now();
        a.control->Pause();
        EXPECT(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "Pause blocked");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        long paused = a.counter.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT(a.counter.frames <= paused + 1, "frames delivered while paused");
        a.control->Run();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EXPECT(a.counter.frames > paused + 5, "no frames after resuming");
        std::printf("  step: final stop\n");
        a.control->Stop();
        b.control->Stop();
    } else {
        stop = true;
        writer.join();
    }
    std::printf("  step: release graphs\n");
    a.release();
    b.release();
    sink.reset();
    std::printf("  step: unregister\n");
    usc::capture::vcamUninstall(machine, err);
    CoUninitialize();
    std::printf(failures ? "usc_vcam_test: %d FAILURE(S)\n" : "usc_vcam_test: all checks passed\n", failures);
    return failures ? 1 : 0;
}
