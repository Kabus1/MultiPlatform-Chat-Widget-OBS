#include "desktop/DesktopWindow.hpp"

#ifdef USC_HAS_WEBVIEW

#include "core/Log.hpp"

#include <nlohmann/json.hpp>
#include <webview/webview.h>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace usc::desktop {

namespace {

void setAlwaysOnTop(webview::webview& w, bool onTop) {
#ifdef _WIN32
    auto hwnd = w.window();
    if (hwnd.ok() && hwnd.value())
        SetWindowPos(static_cast<HWND>(hwnd.value()), onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    (void)w;
    (void)onTop;
#endif
}

} // namespace

bool available() { return true; }

bool run(const WindowOptions& opt, const std::atomic<bool>& stop, std::string& error) {
#ifdef _WIN32
    // Browser-mode TTS must be able to start audio without a click.
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--autoplay-policy=no-user-gesture-required");
#endif
    try {
        webview::webview w(false, nullptr);
        w.set_title("Unified Stream Chat");
        w.set_size(opt.width, opt.height, WEBVIEW_HINT_NONE);

        // JS -> native bridges used only when the page runs inside the window.
        w.bind("uscSetOnTop", [&w](const std::string& req) -> std::string {
            auto args = nlohmann::json::parse(req, nullptr, false);
            setAlwaysOnTop(w, args.is_array() && !args.empty() && args[0].is_boolean() && args[0].get<bool>());
            return "true";
        });
        w.bind("uscSaveSize", [&opt](const std::string& req) -> std::string {
            auto args = nlohmann::json::parse(req, nullptr, false);
            if (opt.onResize && args.is_array() && args.size() == 2 && args[0].is_number() && args[1].is_number())
                opt.onResize(args[0].get<int>(), args[1].get<int>());
            return "true";
        });

        setAlwaysOnTop(w, opt.alwaysOnTop);
        w.navigate(opt.url);

        // Ctrl+C / console close: ask the UI thread to close the window.
        std::atomic<bool> closed{false};
        std::thread watcher([&] {
            while (!closed && !stop) std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (stop && !closed) w.dispatch([&w] { w.terminate(); });
        });
        w.run();
        closed = true;
        watcher.join();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace usc::desktop

#else // no native window compiled in

namespace usc::desktop {
bool available() { return false; }
bool run(const WindowOptions&, const std::atomic<bool>&, std::string& error) {
    error = "this build has no desktop window support";
    return false;
}
} // namespace usc::desktop

#endif
