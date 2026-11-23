// Windows output: a normal top-level window whose content is a D3D11 flip-model
// swap chain - the path OBS "Game Capture" (capture specific window) hooks.
//
// Threads:
//   capture thread (Media Foundation callback) -> present(): copies the frame
//       into a latest-frame mailbox and returns at once. Never touches the GPU.
//   render thread -> uploads the newest frame, converts YUV->RGB with a pixel
//       shader, draws it (scaled / letterboxed) into the swap chain, Present().
//   window thread -> message loop of the output window.
//
// The colour conversion is a plain pixel shader on ordinary R8 / R8G8 / RGBA
// textures (no ID3D11VideoProcessor, no NV12/YUY2 GPU surfaces), so it works
// the same on every Direct3D 10/11 GPU and driver.
#ifdef _WIN32

#include "capture/ComRef.hpp"
#include "capture/Guard.hpp"
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
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace usc::capture {

namespace {

const wchar_t* kClassName = L"USCCaptureOutputWindow";
constexpr UINT WM_APP_CLOSE = WM_APP + 1;
constexpr int kPreviewW = 640, kPreviewH = 360;
constexpr int kPreviewIntervalMs = 200; // 5 fps, only while someone looks at it
constexpr int kPreviewIdleMs = 3000;    // stop making previews after this

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

// --------------------------------------------------------------- shaders
// One full-screen triangle; NV12 = Y (R8) + interleaved UV (R8G8, half size);
// YUY2 = one RGBA texel per two pixels (Y0 U Y1 V).
const char kShaderSource[] = R"(
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vsMain(uint id : SV_VertexID) {
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
cbuffer Color : register(b0) { float4 rowR; float4 rowG; float4 rowB; };
SamplerState smp : register(s0);
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
Texture2D<float4> texP  : register(t2);
float4 toRgb(float y, float u, float v) {
    float4 yuv = float4(y, u, v, 1.0);
    return float4(saturate(dot(rowR, yuv)), saturate(dot(rowG, yuv)), saturate(dot(rowB, yuv)), 1.0);
}
float4 psNV12(VO i) : SV_Target {
    float2 c = texUV.Sample(smp, i.uv);
    return toRgb(texY.Sample(smp, i.uv), c.x, c.y);
}
float4 psYUY2(VO i) : SV_Target {
    uint w, h;
    texP.GetDimensions(w, h);
    int px = min(int(i.uv.x * float(w * 2)), int(w * 2 - 1));
    int py = min(int(i.uv.y * float(h)), int(h - 1));
    float4 t = texP.Load(int3(px >> 1, py, 0));
    return toRgb((px & 1) ? t.z : t.x, t.y, t.w);
}
)";

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// d3dcompiler_47.dll is loaded once and never unloaded: the ID3DBlob objects
// it returns live in that DLL, so FreeLibrary before releasing them (as an
// earlier version did) jumped into unloaded code - an access violation on
// every Start.
D3DCompileFn d3dCompile() {
    static D3DCompileFn fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); // in-box since Windows 8.1
        return m ? reinterpret_cast<D3DCompileFn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile")))
                 : nullptr;
    }();
    return fn;
}

// Limited-range YCbCr -> full-range RGB, as rows applied to (Y, Cb, Cr, 1).
struct ColorRows {
    float r[4], g[4], b[4];
};
ColorRows colorRows(bool bt709) {
    const float ys = 255.0f / 219.0f, yo = -16.0f / 219.0f; // Y' = ys*Y + yo
    const float cs = 255.0f / 224.0f;                        // C' = cs*(C - co)
    const float co = 128.0f / 255.0f;                        // chroma zero point
    float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f, kg = 1.0f - kr - kb;
    float rv = 2.0f * (1.0f - kr) * cs;           // R = Y' + rv*Cr'
    float bu = 2.0f * (1.0f - kb) * cs;           // B = Y' + bu*Cb'
    float gu = -bu * kb / kg, gv = -rv * kr / kg; // G = Y' + gu*Cb' + gv*Cr'
    ColorRows c{};
    c.r[0] = ys; c.r[1] = 0;  c.r[2] = rv; c.r[3] = yo - co * rv;
    c.g[0] = ys; c.g[1] = gu; c.g[2] = gv; c.g[3] = yo - co * (gu + gv);
    c.b[0] = ys; c.b[1] = bu; c.b[2] = 0;  c.b[3] = yo - co * bu;
    return c;
}

// A frame copied out of the capture callback (planes packed with their pitch).
struct CpuFrame {
    std::vector<uint8_t> plane0; // NV12: Y   YUY2: packed
    std::vector<uint8_t> plane1; // NV12: UV
    int pitch = 0;
    int width = 0, height = 0;
    std::string subtype;
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
        setError("");
        presented_ = skipped_ = failed_ = 0;
        {
            std::lock_guard<std::mutex> lock(rateMutex_);
            rate_ = RateMeter{};
        }

        if (!createDevice(error) || !createPipeline(error)) {
            releaseAll();
            return false;
        }

        // Window on its own thread (its message loop must never wait on us).
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
            releaseAll();
            return false;
        }
        if (!createSwapChain(error)) {
            closeWindow();
            releaseAll();
            return false;
        }
        // Valid (black) content right away, so OBS finds a hookable window
        // even before the first frame arrives.
        const float black[4] = {0, 0, 0, 1};
        context_->ClearRenderTargetView(backBufferRtv_.get(), black);
        swapChain_->Present(0, 0);

        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            quit_ = false;
            hasPending_ = false;
        }
        faulted_ = false;
        render_ = std::thread([this] { renderThread(); });
        {
            std::lock_guard<std::mutex> input(inputMutex_);
            open_ = true;
        }
        LOG_INFO("capture", "output window ", outW, "x", outH, " ready");
        return true;
    }

    bool present(const FrameView& f) override {
        // Serialised with open()/close(): a late capture callback can never
        // write into the mailbox while the output is being torn down.
        std::lock_guard<std::mutex> input(inputMutex_);
        if (!open_ || faulted_ || !f.data || f.width <= 0 || f.height <= 0 || f.pitch <= 0) return false;
        // Copy into the spare buffer without holding the mailbox lock.
        CpuFrame& dst = spare_;
        dst.width = f.width;
        dst.height = f.height;
        dst.pitch = f.pitch;
        dst.subtype = f.subtype;
        if (f.subtype == "NV12") {
            size_t ySize = size_t(f.pitch) * size_t(f.height);
            size_t uvSize = size_t(f.pitch) * size_t((f.height + 1) / 2);
            size_t uvOff = f.uvOffset ? f.uvOffset : ySize;
            if (size_t(f.pitch) < size_t(f.width) || uvOff < ySize || uvOff + uvSize > f.size) {
                ++failed_;
                setError("Frame buffer is smaller than its NV12 format says - frame skipped");
                return false;
            }
            dst.plane0.assign(f.data, f.data + ySize);
            dst.plane1.assign(f.data + uvOff, f.data + uvOff + uvSize);
        } else if (f.subtype == "YUY2") {
            if (size_t(f.pitch) < size_t(f.width) * 2 || size_t(f.pitch) * size_t(f.height) > f.size) {
                ++failed_;
                setError("Frame buffer is smaller than its YUY2 format says - frame skipped");
                return false;
            }
            dst.plane0.assign(f.data, f.data + size_t(f.pitch) * size_t(f.height));
            dst.plane1.clear();
        } else {
            ++failed_;
            setError("Unsupported frame format " + f.subtype + " (expected NV12 or YUY2)");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            std::swap(spare_, pending_);
            if (hasPending_) ++skipped_; // the render thread had not taken the previous one
            hasPending_ = true;
        }
        mailboxCv_.notify_one();
        return true;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> input(inputMutex_); // waits for a present() in progress
            open_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            quit_ = true;
        }
        mailboxCv_.notify_all();
        if (render_.joinable()) render_.join();
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
        backBufferRtv_.reset();
        swapChain_.reset(); // before the window goes away
        closeWindow();
        releaseAll();
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
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            s.error = lastError_;
        }
        s.faulted = faulted_;
        s.presented = presented_;
        s.skipped = skipped_;
        s.failed = failed_;
        {
            std::lock_guard<std::mutex> lock(rateMutex_);
            s.fps = rate_.rate(util::nowMs());
        }
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
    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        if (!e.empty() && e != lastError_) LOG_WARN("capture", "output: ", e);
        lastError_ = e;
    }

    // ---------------------------------------------------------------- setup
    bool createDevice(std::string& error) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, 4, D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
        if (hr == E_INVALIDARG) // Windows 7 runtime without 11_1
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels + 1, 3, D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
        if (FAILED(hr)) {
            error = "No Direct3D 10/11 GPU available (" + hex(hr) + ")";
            return false;
        }
        return true;
    }

    bool createPipeline(std::string& error) {
        D3DCompileFn compile = d3dCompile();
        if (!compile) {
            error = "d3dcompiler_47.dll is missing (install the Windows updates / DirectX runtime)";
            return false;
        }
        auto build = [&](const char* entry, const char* target, ComRef<ID3DBlob>& out) {
            ComRef<ID3DBlob> errors;
            HRESULT hr = compile(kShaderSource, sizeof(kShaderSource) - 1, "usc_capture", nullptr, nullptr, entry,
                                 target, 0, 0, out.put(), errors.put());
            if (FAILED(hr)) {
                error = std::string("Shader ") + entry + " failed: " +
                        (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                              errors->GetBufferSize())
                                : hex(hr));
                return false;
            }
            return true;
        };
        ComRef<ID3DBlob> vs, psNv12, psYuy2;
        if (!build("vsMain", "vs_4_0", vs) || !build("psNV12", "ps_4_0", psNv12) ||
            !build("psYUY2", "ps_4_0", psYuy2))
            return false;
        HRESULT hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put());
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(psNv12->GetBufferPointer(), psNv12->GetBufferSize(), nullptr,
                                            psNv12_.put());
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(psYuy2->GetBufferPointer(), psYuy2->GetBufferSize(), nullptr,
                                            psYuy2_.put());
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(hr)) hr = device_->CreateSamplerState(&sd, sampler_.put());
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ColorRows);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&bd, nullptr, colorBuffer_.put());
        if (FAILED(hr)) {
            error = "Could not create the GPU colour converter (" + hex(hr) + ")";
            return false;
        }
        colorFor709_ = -1;
        return true;
    }

    bool createSwapChain(std::string& error) {
        ComRef<IDXGIDevice> dxgiDevice;
        ComRef<IDXGIAdapter> adapter;
        ComRef<IDXGIFactory2> factory;
        HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.putVoid());
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(adapter.put());
        if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.putVoid());
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = UINT(outW_);
        sd.Height = UINT(outH_);
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
        if (SUCCEEDED(hr)) {
            ComRef<IDXGIDevice1> d1;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice1), d1.putVoid())))
                d1->SetMaximumFrameLatency(1); // newest frame on screen, not 3 frames behind
        }
        ComRef<ID3D11Texture2D> back;
        if (SUCCEEDED(hr)) hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.putVoid());
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(back.get(), nullptr, backBufferRtv_.put());
        if (FAILED(hr)) {
            error = "Could not create the output swap chain (" + hex(hr) + ")";
            return false;
        }
        return true;
    }

    void releaseAll() {
        texY_.reset();
        texUV_.reset();
        texP_.reset();
        srvY_.reset();
        srvUV_.reset();
        srvP_.reset();
        previewTex_.reset();
        previewRtv_.reset();
        previewStaging_.reset();
        colorBuffer_.reset();
        sampler_.reset();
        psYuy2_.reset();
        psNv12_.reset();
        vs_.reset();
        backBufferRtv_.reset();
        swapChain_.reset();
        context_.reset();
        device_.reset();
        texW_ = texH_ = 0;
        texFormat_.clear();
    }

    // ---------------------------------------------------------------- render thread
    void renderThread() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC (preview JPEG)
        CpuFrame current;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mailboxMutex_);
                mailboxCv_.wait(lock, [&] { return quit_ || hasPending_; });
                if (quit_) break;
                std::swap(current, pending_);
                hasPending_ = false;
            }
            bool ok = false;
            std::string fault;
            if (!guarded([&] { ok = renderFrame(current); }, fault)) {
                // A driver/GPU fault: stop drawing (the relay stops the session)
                // instead of letting it take the whole app down.
                ++failed_;
                faulted_ = true;
                setError("Rendering stopped: " + fault);
                break;
            }
            if (ok) {
                ++presented_;
                std::lock_guard<std::mutex> lock(rateMutex_);
                rate_.tick(util::nowMs());
            } else {
                ++failed_;
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    bool ensureTextures(const CpuFrame& f) {
        if (texW_ == f.width && texH_ == f.height && texFormat_ == f.subtype) return true;
        texY_.reset();
        texUV_.reset();
        texP_.reset();
        srvY_.reset();
        srvUV_.reset();
        srvP_.reset();
        texW_ = texH_ = 0;
        auto make = [&](int w, int h, DXGI_FORMAT fmt, ComRef<ID3D11Texture2D>& tex,
                        ComRef<ID3D11ShaderResourceView>& srv) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(w);
            td.Height = UINT(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device_->CreateTexture2D(&td, nullptr, tex.put());
            if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(tex.get(), nullptr, srv.put());
            if (FAILED(hr))
                setError("Could not create a " + std::to_string(w) + "x" + std::to_string(h) + " GPU texture (" +
                         hex(hr) + ")");
            return SUCCEEDED(hr);
        };
        bool ok;
        if (f.subtype == "NV12")
            ok = make(f.width, f.height, DXGI_FORMAT_R8_UNORM, texY_, srvY_) &&
                 make((f.width + 1) / 2, (f.height + 1) / 2, DXGI_FORMAT_R8G8_UNORM, texUV_, srvUV_);
        else
            ok = make((f.width + 1) / 2, f.height, DXGI_FORMAT_R8G8B8A8_UNORM, texP_, srvP_);
        if (!ok) return false;
        texW_ = f.width;
        texH_ = f.height;
        texFormat_ = f.subtype;
        return true;
    }

    void bindAndDraw(ID3D11RenderTargetView* rtv, int targetW, int targetH, bool keepAspect, const CpuFrame& f) {
        const float black[4] = {0, 0, 0, 1};
        context_->ClearRenderTargetView(rtv, black);
        D3D11_VIEWPORT vp{};
        vp.Width = float(targetW);
        vp.Height = float(targetH);
        vp.MaxDepth = 1.0f;
        if (keepAspect) { // letterbox / pillarbox
            double s = std::min(double(targetW) / f.width, double(targetH) / f.height);
            vp.Width = float(std::round(f.width * s));
            vp.Height = float(std::round(f.height * s));
            vp.TopLeftX = float(std::floor((targetW - vp.Width) / 2));
            vp.TopLeftY = float(std::floor((targetH - vp.Height) / 2));
        }
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->RSSetViewports(1, &vp);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.get(), nullptr, 0);
        ID3D11Buffer* cb = colorBuffer_.get();
        context_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* smp = sampler_.get();
        context_->PSSetSamplers(0, 1, &smp);
        if (f.subtype == "NV12") {
            ID3D11ShaderResourceView* srvs[2] = {srvY_.get(), srvUV_.get()};
            context_->PSSetShaderResources(0, 2, srvs);
            context_->PSSetShader(psNv12_.get(), nullptr, 0);
        } else {
            ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, srvP_.get()};
            context_->PSSetShaderResources(0, 3, srvs);
            context_->PSSetShader(psYuy2_.get(), nullptr, 0);
        }
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        context_->PSSetShaderResources(0, 3, none);
    }

    bool renderFrame(const CpuFrame& f) {
        if (!ensureTextures(f)) return false;
        // Upload: one copy per plane, straight from the packed CPU buffers.
        if (f.subtype == "NV12") {
            context_->UpdateSubresource(texY_.get(), 0, nullptr, f.plane0.data(), UINT(f.pitch), 0);
            context_->UpdateSubresource(texUV_.get(), 0, nullptr, f.plane1.data(), UINT(f.pitch), 0);
        } else {
            context_->UpdateSubresource(texP_.get(), 0, nullptr, f.plane0.data(), UINT(f.pitch), 0);
        }
        int want709 = f.height >= 720 ? 1 : 0; // BT.709 for HD, BT.601 for SD
        if (want709 != colorFor709_) {
            ColorRows rows = colorRows(want709 == 1);
            context_->UpdateSubresource(colorBuffer_.get(), 0, nullptr, &rows, 0, 0);
            colorFor709_ = want709;
        }

        bindAndDraw(backBufferRtv_.get(), outW_, outH_, keepAspect_, f);
        // Blocking Present on this dedicated thread: with frame latency 1 it
        // waits at most about one refresh; the capture thread is never held up.
        HRESULT hr = swapChain_->Present(0, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            setError("The GPU was reset (driver update / sleep) - press Stop, then Start (" +
                     hex(device_->GetDeviceRemovedReason()) + ")");
            return false;
        }
        if (FAILED(hr)) {
            setError("Present failed (" + hex(hr) + ")");
            return false;
        }
        setError("");

        int64_t now = util::nowMs();
        if (now - previewRequestedMs_ < kPreviewIdleMs && now - lastPreviewMs_ >= kPreviewIntervalMs) {
            lastPreviewMs_ = now;
            makePreview(f);
        }
        return true;
    }

    void makePreview(const CpuFrame& f) {
        if (!previewTex_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kPreviewW;
            td.Height = kPreviewH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, previewTex_.put()))) return;
            if (FAILED(device_->CreateRenderTargetView(previewTex_.get(), nullptr, previewRtv_.put()))) return;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, previewStaging_.put()))) return;
        }
        bindAndDraw(previewRtv_.get(), kPreviewW, kPreviewH, true, f);
        context_->CopyResource(previewStaging_.get(), previewTex_.get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(previewStaging_.get(), 0, D3D11_MAP_READ, 0, &map))) return;
        std::vector<uint8_t> bgr(size_t(kPreviewW) * kPreviewH * 3);
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
        std::string jpeg = encodeJpeg(bgr, kPreviewW, kPreviewH);
        if (!jpeg.empty()) {
            std::lock_guard<std::mutex> lock(previewMutex_);
            previewJpeg_ = std::move(jpeg);
        }
    }

    static std::string encodeJpeg(const std::vector<uint8_t>& bgr, int w, int h) {
        std::string out;
        ComRef<IWICImagingFactory> factory;
        ComRef<IStream> stream;
        ComRef<IWICBitmapEncoder> encoder;
        ComRef<IWICBitmapFrameEncode> frame;
        ComRef<IPropertyBag2> props;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IWICImagingFactory), factory.putVoid());
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
        return out;
    }

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

    // window
    std::thread ui_;
    std::atomic<HWND> hwnd_{nullptr};
    std::mutex windowMutex_;
    std::condition_variable windowCv_;
    bool windowReady_ = false;
    std::function<void()> onUserClosed_;

    // d3d: created in open(), then used only by the render thread
    ComRef<ID3D11Device> device_;
    ComRef<ID3D11DeviceContext> context_;
    ComRef<IDXGISwapChain1> swapChain_;
    ComRef<ID3D11RenderTargetView> backBufferRtv_;
    ComRef<ID3D11VertexShader> vs_;
    ComRef<ID3D11PixelShader> psNv12_, psYuy2_;
    ComRef<ID3D11SamplerState> sampler_;
    ComRef<ID3D11Buffer> colorBuffer_;
    int colorFor709_ = -1;
    ComRef<ID3D11Texture2D> texY_, texUV_, texP_;
    ComRef<ID3D11ShaderResourceView> srvY_, srvUV_, srvP_;
    int texW_ = 0, texH_ = 0;
    std::string texFormat_;
    ComRef<ID3D11Texture2D> previewTex_, previewStaging_;
    ComRef<ID3D11RenderTargetView> previewRtv_;
    int outW_ = 0, outH_ = 0;
    bool keepAspect_ = true;
    std::atomic<bool> open_{false};
    std::atomic<bool> faulted_{false};
    std::mutex inputMutex_; // present() vs open()/close()

    // latest-frame mailbox (capture thread -> render thread)
    std::thread render_;
    std::mutex mailboxMutex_;
    std::condition_variable mailboxCv_;
    CpuFrame spare_, pending_; // spare_ is touched only by the capture thread
    bool hasPending_ = false;
    bool quit_ = false;

    // stats
    std::atomic<uint64_t> presented_{0}, skipped_{0}, failed_{0};
    mutable std::mutex rateMutex_;
    RateMeter rate_;
    mutable std::mutex errorMutex_;
    std::string lastError_;

    // preview
    std::atomic<int64_t> previewRequestedMs_{0};
    int64_t lastPreviewMs_ = 0;
    mutable std::mutex previewMutex_;
    std::string previewJpeg_;
};

} // namespace

std::unique_ptr<OutputSink> createOutputSink() { return std::make_unique<D3DOutputWindow>(); }

} // namespace usc::capture

#endif
