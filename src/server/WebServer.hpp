#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace usc {

class App;

// Local HTTP server (127.0.0.1 only) that serves the dock UI, a JSON API, a
// server-sent-events stream of chat messages and the TTS audio proxy.
class WebServer {
public:
    WebServer(App& app, std::string webDir);
    ~WebServer();

    bool start(int port);
    void stop();

private:
    void routes();

    App& app_;
    std::string webDir_; // non-empty: serve web/ from disk (UI development)
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

} // namespace usc
