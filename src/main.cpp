#include "core/App.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"
#include "server/WebServer.hpp"

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

void usage() {
    std::cout << "Unified Stream Chat " APP_VERSION " - YouTube + Twitch + Kick chat dock for OBS\n\n"
                 "Usage: unified_stream_chat [options]\n"
                 "  --port <n>       HTTP port (default 8787, or server.port in settings.json)\n"
                 "  --config <dir>   settings/tokens directory (default: " << usc::util::defaultConfigDir() << ")\n"
                 "  --web <dir>      serve the UI from a folder instead of the embedded copy (development)\n"
                 "  --open           open the dock page in the default browser on start\n"
                 "  --help           show this help\n";
}

} // namespace

int main(int argc, char** argv) {
    int port = 0;
    std::string configDir = usc::util::defaultConfigDir();
    std::string webDir;
    bool open = false;

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
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown option " << a << "\n";
            usage();
            return 2;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    ix::initNetSystem();

    usc::App app(configDir, port);
    usc::WebServer server(app, webDir);
    if (!server.start(app.port())) return 1;
    app.start();

    std::string url = "http://localhost:" + std::to_string(app.port()) + "/";
    std::cout << "\n  Unified Stream Chat is running.\n"
              << "  OBS: Docks > Custom Browser Docks... > add URL  " << url << "\n"
              << "  Overlay (Browser Source, transparent):          " << url << "overlay\n"
              << "  Settings file: " << app.settings().path() << "\n"
              << "  Press Ctrl+C to quit.\n\n";
    if (open) usc::util::openInSystemBrowser(url);

    while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    LOG_INFO("app", "shutting down");
    app.shutdown(); // closes SSE streams so the server can stop promptly
    server.stop();
    ix::uninitNetSystem();
    return 0;
}
