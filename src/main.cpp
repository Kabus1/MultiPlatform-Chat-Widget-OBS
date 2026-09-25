#include "core/App.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"
#include "desktop/DesktopWindow.hpp"
#include "net/Http.hpp"
#include "server/WebServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

void usage() {
    std::cout << "Unified Stream Chat " APP_VERSION " - YouTube + Twitch + Kick chat for OBS\n\n"
                 "Usage: unified_stream_chat [options]\n"
                 "  --no-window      run only the local server (OBS dock / browser source), no app window\n"
                 "  --port <n>       HTTP port (default 8787, or server.port in settings.json)\n"
                 "  --config <dir>   settings/tokens directory (default: " << usc::util::defaultConfigDir() << ")\n"
                 "  --web <dir>      serve the UI from a folder instead of the embedded copy (development)\n"
                 "  --open           also open the dock page in the default browser\n"
                 "  --help           show this help\n";
}

// The Windows build is a GUI program (no console window pops up). When it is
// started from a terminal, print there; otherwise log to <config>/app.log.
void setupWindowsOutput(const std::string& configDir) {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    std::string log = (std::filesystem::path(configDir) / "app.log").string();
    std::freopen(log.c_str(), "w", stderr);
    std::freopen(log.c_str(), "a", stdout);
#else
    (void)configDir;
#endif
}

bool instanceRunning(int port) {
    auto r = usc::http::get("http://127.0.0.1:" + std::to_string(port) + "/api/status", {}, 2);
    return r.ok() && r.body.find("\"version\"") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    int port = 0;
    std::string configDir = usc::util::defaultConfigDir();
    std::string webDir;
    bool open = false;
    bool window = usc::desktop::available();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") port = std::stoi(next());
        else if (a == "--config") configDir = next();
        else if (a == "--web") webDir = next();
        else if (a == "--open") open = true;
        else if (a == "--no-window" || a == "--headless") window = false;
        else if (a == "--help" || a == "-h") {
            setupWindowsOutput(configDir);
            usage();
            return 0;
        } else {
            setupWindowsOutput(configDir);
            std::cerr << "unknown option " << a << "\n";
            usage();
            return 2;
        }
    }
    setupWindowsOutput(configDir);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    usc::App app(configDir, port);
    usc::WebServer server(app, webDir);
    if (!server.start(app.port())) {
        // Already running (e.g. started earlier or from OBS): just show
        // another window onto that instance instead of failing.
        if (window && instanceRunning(app.port())) {
            std::string error;
            auto w = app.settings().snapshot()["window"];
            usc::desktop::WindowOptions opt;
            opt.url = app.links()["window"].get<std::string>();
            opt.width = w.value("width", 440);
            opt.height = w.value("height", 780);
            opt.alwaysOnTop = w.value("alwaysOnTop", false);
            return usc::desktop::run(opt, gStop, error) ? 0 : 1;
        }
        return 1;
    }
    app.start();

    auto links = app.links();
    std::cout << "\n  Unified Stream Chat " APP_VERSION " is running.\n"
              << "  OBS Custom Browser Dock:   " << links["dock"].get<std::string>() << "\n"
              << "  OBS Browser Source:        " << links["overlay"].get<std::string>() << "\n"
              << "  Browser Source with voice: " << links["overlayVoice"].get<std::string>() << "\n"
              << "  Settings file: " << app.settings().path() << "\n"
              << (window ? "  Close the window to quit.\n\n" : "  Press Ctrl+C to quit.\n\n");
    if (open) usc::util::openInSystemBrowser(links["dock"].get<std::string>());

    bool windowShown = false;
    if (window) {
        auto w = app.settings().snapshot()["window"];
        usc::desktop::WindowOptions opt;
        opt.url = links["window"].get<std::string>();
        opt.width = w.value("width", 440);
        opt.height = w.value("height", 780);
        opt.alwaysOnTop = w.value("alwaysOnTop", false);
        opt.onResize = [&app](int width, int height) {
            if (width >= 200 && height >= 200) app.applySettings({{"window", {{"width", width}, {"height", height}}}});
        };
        std::string error;
        windowShown = usc::desktop::run(opt, gStop, error); // blocks until the window closes
        if (!windowShown) {
            LOG_WARN("app", "cannot open the app window (", error,
                     "). Install the Microsoft Edge WebView2 Runtime. Opening the browser instead.");
            usc::util::openInSystemBrowser(links["dock"].get<std::string>());
        }
    }
    // Server-only mode, or the window could not be created: run until Ctrl+C.
    if (!windowShown)
        while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    LOG_INFO("app", "shutting down");
    app.shutdown(); // closes SSE streams so the server can stop promptly
    server.stop();
    return 0;
}
