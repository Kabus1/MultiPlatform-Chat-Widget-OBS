// Windows output: a normal top-level window whose content is a D3D11 flip-model
// swap chain - the path OBS "Game Capture" (capture specific window) hooks.
#ifdef _WIN32

#include "capture/ComRef.hpp"
#include "capture/OutputSink.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <mutex>
#include <thread>
#include <vector>

namespace usc::capture {

namespace {

const wchar_t* kClassName = L"USCCaptureOutputWindow";
constexpr UINT WM_APP_CLOSE = WM_APP + 1;
constexpr int kPreviewW = 640, kPreviewH = 360;
constexpr int kPreviewIntervalMs = 200;   // 5 fps, only while someone looks at it
constexpr int kPreviewIdleMs = 3000;      // stop making previews after this

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

DXGI_FORMAT dxgiFormat(const std::string& subtype) {
    return subtype == "YUY2" ? DXGI_FORMAT_YUY2 : DXGI_FORMAT_NV12;
}

// A video processor converting/scaling from one input size to one output size.
struct Processor {
    ComRef<ID3D11VideoProcessorEnumerator> enumerator;
    ComRef<ID3D11VideoProcessor> processor;
    int inW = 0, inH = 0, outW = 0, outH = 0;
};

class D3DOutputWindow : public OutputSink {
public:
    ~D3DOutputWindow() override { close(); }

    bool supported() const override { return true; }

    bool open(const std::string& title, int outW, int outH, bool keepAspect, std::string& error) override {
        close();
        outW_ = outW;
        outH_ = outH;
        keepAspect_ = keepAspect;
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            lastError_.clear();
        }

        // --- device (video support needed for the GPU color conversion) ---
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                       device_.put(), nullptr, context_.put());
        if (FAILED(hr)) {
            error = "No Direct3D 11 GPU with video processing available (" + hex(hr) + ")";
            return false;
        }
        if (FAILED(device_->QueryInterface(IID_ID3D11VideoDevice, videoDevice_.putVoid())) ||
            FAILED(context_->QueryInterface(IID_ID3D11VideoContext, videoContext_.putVoid()))) {
            error = "The GPU driver has no Direct3D 11 video processor";
            releaseDevice();
            return false;
        }

        // --- window on its own thread (its message loop must never wait on us) ---
        std::wstring wtitle = widen(title);
        {
            std::unique_lock<std::mutex> lock(windowMutex_);
            windowReady_ = false;
            ui_ = std::thread([this, wtitle] { windowThread(wtitle); });
            windowCv_.wait_for(lock, std::chrono::seconds(5), [&] { return windowReady_; });
        }
        if (!hwnd_) {
            error = "Could not create the output window";
            closeWindow();
            releaseDevice();
            return false;
        }

        // --- swap chain at the full output resolution ---
        ComRef<IDXGIDevice> dxgiDevice;
        ComRef<IDXGIAdapter> adapter;
        ComRef<IDXGIFactory2> factory;
        hr = device_->QueryInterface(IID_IDXGIDevice, dxgiDevice.putVoid());
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(adapter.put());
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_IDXGIFactory2, factory.putVoid());
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = UINT(outW);
        sd.Height = UINT(outH);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH; // window size never changes the captured size
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateSwapChainForHwnd(device_.get(), hwnd_, &sd, nullptr, nullptr, swapChain_.put());
            if (FAILED(hr)) { // Windows 8.1: no FLIP_DISCARD
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                hr = factory->CreateSwapChainForHwnd(device_.get(), hwnd_, &sd, nullptr, nullptr, swapChain_.put());
            }
        }
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        if (SUCCEEDED(hr)) hr = swapChain_->GetBuffer(0, IID_ID3D11Texture2D, backBuffer_.putVoid());
        if (FAILED(hr)) {
            error = "Could not create the output swap chain (" + hex(hr) + ")";
            close();
            return false;
        }
        // Valid (black) content right away, so OBS finds a hookable window
        // even before the first frame arrives.
        clearBackBuffer();
        swapChain_->Present(0, 0);
        open_ = true;
        LOG_INFO("capture", "output window ", outW, "x", outH, " ready");
        return true;
    }

    bool present(const FrameView& f) override {
        std::vector<uint8_t> previewPixels; // BGR24, encoded after unlocking
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            if (!swapChain_ || !f.data || f.width <= 0 || f.height <= 0) return false;
            if (!ensureInput(f) || !upload(f)) return false;
            if (!ensureProcessor(main_, f.width, f.height, outW_, outH_) || !ensureOutputView()) return false;
            blit(main_, outputView_.get(), f.width, f.height, outW_, outH_, keepAspect_);
            HRESULT hr = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ok = false; // GPU busy: drop rather than block the capture thread
            } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                lastError_ = "The GPU was reset (driver update / sleep) - restart the relay";
                ok = false;
            } else {
                occluded_ = hr == DXGI_STATUS_OCCLUDED;
                ok = SUCCEEDED(hr);
            }
            // Preview: only while requested recently, small and slow.
            int64_t now = util::nowMs();
            if (now - previewRequestedMs_ < kPreviewIdleMs && now - lastPreviewMs_ >= kPreviewIntervalMs) {
                lastPreviewMs_ = now;
                readPreview(f, previewPixels);
            }
        }
        if (!previewPixels.empty()) {
            std::string jpeg = encodeJpeg(previewPixels, kPreviewW, kPreviewH);
            if (!jpeg.empty()) {
                std::lock_guard<std::mutex> lock(previewMutex_);
                previewJpeg_ = std::move(jpeg);
            }
        }
        return ok;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            open_ = false;
            outputView_.reset();
            inputView_.reset();
            inputTexture_.reset();
            previewView_.reset();
            previewInputView_.reset();
            previewInputFor_ = nullptr;
            previewTexture_.reset();
            previewStaging_.reset();
            main_ = Processor{};
            preview_ = Processor{};
            backBuffer_.reset();
            if (context_) {
                context_->ClearState();
                context_->Flush();
            }
            swapChain_.reset(); // before the window goes away
        }
        closeWindow();
        releaseDevice();
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewJpeg_.clear();
    }

    OutputState state() const override {
        OutputState s;
        s.open = open_;
        HWND h = hwnd_;
        if (h) {
            s.visible = IsWindowVisible(h) != FALSE;
            s.minimized = IsIconic(h) != FALSE;
        }
        s.width = outW_;
        s.height = outH_;
        std::lock_guard<std::mutex> lock(deviceMutex_);
        s.error = lastError_;
        return s;
    }

    void requestPreview() override { previewRequestedMs_ = util::nowMs(); }

    std::optional<std::string> previewJpeg() const override {
        std::lock_guard<std::mutex> lock(previewMutex_);
        if (previewJpeg_.empty()) return std::nullopt;
        return previewJpeg_;
    }

    void onUserClosed(std::function<void()> cb) override { onUserClosed_ = std::move(cb); }

private:
    // ---------------------------------------------------------------- window
    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<D3DOutputWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CLOSE:
            // The user closed it: tell the relay (which stops and then closes
            // the window through WM_APP_CLOSE). Never destroy the window
            // while the swap chain still uses it.
            if (self && self->onUserClosed_) self->onUserClosed_();
            return 0;
        case WM_APP_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1; // D3D owns the content: no GDI flicker
        case WM_PAINT:
            ValidateRect(h, nullptr);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void windowThread(std::wstring title) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &D3DOutputWindow::wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc); // fails harmlessly if already registered

        // A comfortable on-screen size; the captured size is the swap chain's.
        int clientW = std::min(outW_, 960);
        int clientH = std::max(1, clientW * outH_ / std::max(1, outW_));
        RECT r{0, 0, clientW, clientH};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        // Plain top-level window: no WS_EX_TOOLWINDOW / WS_EX_LAYERED, so OBS
        // lists it under Game Capture > Capture specific window.
        HWND h = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
        if (h) ShowWindow(h, SW_SHOWNOACTIVATE);
        {
            std::lock_guard<std::mutex> lock(windowMutex_);
            hwnd_ = h;
            windowReady_ = true;
        }
        windowCv_.notify_all();
        if (!h) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void closeWindow() {
        HWND h = hwnd_;
        if (h) PostMessageW(h, WM_APP_CLOSE, 0, 0);
        if (ui_.joinable()) ui_.join();
        hwnd_ = nullptr;
    }

    void releaseDevice() {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        videoContext_.reset();
        videoDevice_.reset();
        context_.reset();
        device_.reset();
    }

    // ---------------------------------------------------------------- d3d
    void clearBackBuffer() {
        ComRef<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device_->CreateRenderTargetView(backBuffer_.get(), nullptr, rtv.put()))) {
            const float black[4] = {0, 0, 0, 1};
            context_->ClearRenderTargetView(rtv.get(), black);
        }
    }

    bool ensureInput(const FrameView& f) {
        if (inputTexture_ && inW_ == f.width && inH_ == f.height && inFormat_ == f.subtype) return true;
        inputView_.reset();
        outputView_.reset();
        previewView_.reset();
        previewInputView_.reset(); // views of the old texture / processors
        previewInputFor_ = nullptr;
        inputTexture_.reset();
        main_ = Processor{};
        preview_ = Processor{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(f.width);
        td.Height = UINT(f.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dxgiFormat(f.subtype);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, inputTexture_.put());
        if (FAILED(hr)) { // some drivers only allow decoder-bound video surfaces
            td.BindFlags = D3D11_BIND_DECODER;
            hr = device_->CreateTexture2D(&td, nullptr, inputTexture_.put());
        }
        if (FAILED(hr)) {
            lastError_ = "The GPU cannot hold " + f.subtype + " " + std::to_string(f.width) + "x" +
                         std::to_string(f.height) + " frames (" + hex(hr) + ")";
            return false;
        }
        inW_ = f.width;
        inH_ = f.height;
        inFormat_ = f.subtype;
        return true;
    }

    bool upload(const FrameView& f) {
        const uint8_t* src = f.data;
        UINT pitch = UINT(f.pitch);
        if (f.subtype == "NV12") {
            size_t ySize = size_t(f.pitch) * size_t(f.height);
            if (f.uvOffset != ySize) {
                // Padded Y plane: make it contiguous (Y rows, then UV rows).
                repack_.resize(ySize + ySize / 2);
                std::memcpy(repack_.data(), f.data, ySize);
                std::memcpy(repack_.data() + ySize, f.data + f.uvOffset, ySize / 2);
                src = repack_.data();
            }
        }
        context_->UpdateSubresource(inputTexture_.get(), 0, nullptr, src, pitch, 0);
        return true;
    }

    bool ensureProcessor(Processor& p, int inW, int inH, int outW, int outH) {
        if (p.processor && p.inW == inW && p.inH == inH && p.outW == outW && p.outH == outH) return true;
        p = Processor{};
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = UINT(inW);
        cd.InputHeight = UINT(inH);
        cd.OutputWidth = UINT(outW);
        cd.OutputHeight = UINT(outH);
        cd.InputFrameRate = {60, 1};
        cd.OutputFrameRate = {60, 1};
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, p.enumerator.put());
        UINT support = 0;
        if (SUCCEEDED(hr)) hr = p.enumerator->CheckVideoProcessorFormat(dxgiFormat(inFormat_), &support);
        if (SUCCEEDED(hr) && !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) hr = E_NOTIMPL;
        if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(p.enumerator.get(), 0, p.processor.put());
        if (FAILED(hr)) {
            lastError_ = "The GPU video processor cannot convert " + inFormat_ + " (" + hex(hr) + ")";
            p = Processor{};
            return false;
        }
        p.inW = inW;
        p.inH = inH;
        p.outW = outW;
        p.outH = outH;
        // Views depend on the enumerator.
        if (&p == &main_) {
            inputView_.reset();
            outputView_.reset();
        } else {
            previewView_.reset();
            previewInputView_.reset();
            previewInputFor_ = nullptr;
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (&p == &main_ &&
            FAILED(hr = videoDevice_->CreateVideoProcessorInputView(inputTexture_.get(), p.enumerator.get(), &iv,
                                                                    inputView_.put()))) {
            lastError_ = "Could not attach the frame to the video processor (" + hex(hr) + ")";
            p = Processor{};
            return false;
        }
        return true;
    }

    bool ensureOutputView() {
        if (outputView_) return true;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        HRESULT hr = videoDevice_->CreateVideoProcessorOutputView(backBuffer_.get(), main_.enumerator.get(), &ov,
                                                                  outputView_.put());
        if (FAILED(hr)) lastError_ = "Could not render into the output window (" + hex(hr) + ")";
        return SUCCEEDED(hr);
    }

    void blit(Processor& p, ID3D11VideoProcessorOutputView* target, int inW, int inH, int outW, int outH,
              bool keepAspect) {
        ID3D11VideoProcessor* vp = p.processor.get();
        videoContext_->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE); // no "enhancements"
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.YCbCr_Matrix = inH >= 720 ? 1 : 0; // BT.709 for HD, BT.601 for SD
        in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext_->VideoProcessorSetStreamColorSpace(vp, 0, &in);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        videoContext_->VideoProcessorSetOutputColorSpace(vp, &out);

        RECT src{0, 0, inW, inH};
        RECT full{0, 0, outW, outH};
        RECT dst = full;
        if (keepAspect) { // letterbox / pillarbox
            double s = std::min(double(outW) / inW, double(outH) / inH);
            int w = int(inW * s), h = int(inH * s);
            dst = RECT{(outW - w) / 2, (outH - h) / 2, (outW - w) / 2 + w, (outH - h) / 2 + h};
        }
        videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src);
        videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
        videoContext_->VideoProcessorSetOutputTargetRect(vp, TRUE, &full);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1.0f;
        videoContext_->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputViewFor(p);
        videoContext_->VideoProcessorBlt(vp, target, 0, 1, &stream);
    }

    ID3D11VideoProcessorInputView* inputViewFor(Processor& p) {
        if (&p == &main_) return inputView_.get();
        if (!previewInputView_ || previewInputFor_ != p.enumerator.get()) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
            iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            previewInputView_.reset();
            videoDevice_->CreateVideoProcessorInputView(inputTexture_.get(), p.enumerator.get(), &iv,
                                                        previewInputView_.put());
            previewInputFor_ = p.enumerator.get();
        }
        return previewInputView_.get();
    }

    void readPreview(const FrameView& f, std::vector<uint8_t>& bgr) {
        if (!previewTexture_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kPreviewW;
            td.Height = kPreviewH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, previewTexture_.put()))) return;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, previewStaging_.put()))) return;
        }
        if (!ensureProcessor(preview_, f.width, f.height, kPreviewW, kPreviewH)) return;
        if (!previewView_) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(videoDevice_->CreateVideoProcessorOutputView(previewTexture_.get(), preview_.enumerator.get(),
                                                                    &ov, previewView_.put())))
                return;
        }
        blit(preview_, previewView_.get(), f.width, f.height, kPreviewW, kPreviewH, true);
        context_->CopyResource(previewStaging_.get(), previewTexture_.get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(previewStaging_.get(), 0, D3D11_MAP_READ, 0, &map))) return;
        bgr.resize(size_t(kPreviewW) * kPreviewH * 3);
        for (int y = 0; y < kPreviewH; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
            uint8_t* dst = bgr.data() + size_t(y) * kPreviewW * 3;
            for (int x = 0; x < kPreviewW; ++x) {
                dst[x * 3 + 0] = row[x * 4 + 0];
                dst[x * 3 + 1] = row[x * 4 + 1];
                dst[x * 3 + 2] = row[x * 4 + 2];
            }
        }
        context_->Unmap(previewStaging_.get(), 0);
    }

    std::string encodeJpeg(const std::vector<uint8_t>& bgr, int w, int h) {
        // Capture callbacks run on Media Foundation's (MTA) threads; make
        // sure COM is initialised on this one for WIC.
        HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string out;
        {
            ComRef<IWICImagingFactory> factory;
            ComRef<IStream> stream;
            ComRef<IWICBitmapEncoder> encoder;
            ComRef<IWICBitmapFrameEncode> frame;
            ComRef<IPropertyBag2> props;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                          factory.putVoid());
            if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
            if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put());
            if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
            if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.put(), props.put());
            if (SUCCEEDED(hr)) hr = frame->Initialize(props.get());
            if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(w), UINT(h));
            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
            if (SUCCEEDED(hr))
                hr = frame->WritePixels(UINT(h), UINT(w * 3), UINT(bgr.size()), const_cast<BYTE*>(bgr.data()));
            if (SUCCEEDED(hr)) hr = frame->Commit();
            if (SUCCEEDED(hr)) hr = encoder->Commit();
            HGLOBAL global = nullptr;
            STATSTG st{};
            if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.get(), &global);
            if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
            if (SUCCEEDED(hr)) {
                if (void* p = GlobalLock(global)) {
                    out.assign(static_cast<const char*>(p), size_t(st.cbSize.QuadPart));
                    GlobalUnlock(global);
                }
            }
        }
        if (SUCCEEDED(coInit)) CoUninitialize();
        return out;
    }

    // window
    std::thread ui_;
    std::atomic<HWND> hwnd_{nullptr};
    std::mutex windowMutex_;
    std::condition_variable windowCv_;
    bool windowReady_ = false;
    std::function<void()> onUserClosed_;

    // d3d (guarded by deviceMutex_)
    mutable std::mutex deviceMutex_;
    ComRef<ID3D11Device> device_;
    ComRef<ID3D11DeviceContext> context_;
    ComRef<ID3D11VideoDevice> videoDevice_;
    ComRef<ID3D11VideoContext> videoContext_;
    ComRef<IDXGISwapChain1> swapChain_;
    ComRef<ID3D11Texture2D> backBuffer_;
    ComRef<ID3D11VideoProcessorOutputView> outputView_;
    ComRef<ID3D11Texture2D> inputTexture_;
    ComRef<ID3D11VideoProcessorInputView> inputView_;
    Processor main_, preview_;
    ComRef<ID3D11Texture2D> previewTexture_, previewStaging_;
    ComRef<ID3D11VideoProcessorOutputView> previewView_;
    ComRef<ID3D11VideoProcessorInputView> previewInputView_;
    void* previewInputFor_ = nullptr;
    std::vector<uint8_t> repack_;
    int inW_ = 0, inH_ = 0;
    std::string inFormat_;
    int outW_ = 0, outH_ = 0;
    bool keepAspect_ = true;
    std::atomic<bool> open_{false};
    std::atomic<bool> occluded_{false};
    std::string lastError_;
    std::atomic<int64_t> previewRequestedMs_{0};
    int64_t lastPreviewMs_ = 0;

    mutable std::mutex previewMutex_;
    std::string previewJpeg_;
};

} // namespace

std::unique_ptr<OutputSink> createOutputSink() { return std::make_unique<D3DOutputWindow>(); }

} // namespace usc::capture

#endif
