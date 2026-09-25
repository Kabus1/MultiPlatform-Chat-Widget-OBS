#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace usc::desktop {

struct WindowOptions {
    std::string url;
    int width = 440;
    int height = 780;
    bool alwaysOnTop = false;
    // Called from the page (window.uscSaveSize) when the user resizes.
    std::function<void(int width, int height)> onResize;
};

// True when the app was built with the native window (WebView2 on Windows,
// WKWebView on macOS, WebKitGTK on Linux).
bool available();

// Shows the chat UI in a native window on the calling (main) thread and
// blocks until it is closed or `stop` becomes true. Returns false if no
// window could be created (e.g. WebView2 runtime missing); `error` says why.
bool run(const WindowOptions& options, const std::atomic<bool>& stop, std::string& error);

} // namespace usc::desktop
