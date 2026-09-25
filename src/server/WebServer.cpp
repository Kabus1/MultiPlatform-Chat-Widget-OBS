#include "server/WebServer.hpp"

#include "core/App.hpp"
#include "core/Log.hpp"
#include "core/Util.hpp"
#include "server/EmbeddedAssets.hpp"

#include <httplib.h>

namespace usc {

using nlohmann::json;

namespace {

const char* mimeFor(const std::string& path) {
    auto ends = [&](const char* ext) {
        std::string e(ext);
        return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

std::string authPage(bool ok, const std::string& message) {
    return std::string(R"(<!doctype html><html><head><meta charset="utf-8"><title>Unified Stream Chat</title>
<style>body{font-family:system-ui,sans-serif;background:#0e0e12;color:#eee;display:grid;place-items:center;height:100vh;margin:0}
.card{background:#1a1a22;padding:32px 40px;border-radius:12px;text-align:center;max-width:520px}
h1{margin:0 0 8px;font-size:22px;color:)") +
           (ok ? "#4ade80" : "#f87171") + R"(}p{color:#aaa}</style></head><body><div class="card"><h1>)" +
           (ok ? "Connected!" : "Login failed") + "</h1><p>" + htmlEscape(message) +
           "</p><p>You can close this tab and return to OBS.</p></div>" +
           (ok ? "<script>setTimeout(()=>window.close(),2500)</script>" : "") + "</body></html>";
}

} // namespace

WebServer::WebServer(App& app, std::string webDir) : app_(app), webDir_(std::move(webDir)) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start(int port) {
    svr_ = std::make_unique<httplib::Server>();
    routes();
    if (!svr_->bind_to_port("127.0.0.1", port)) {
        LOG_ERROR("server", "cannot listen on 127.0.0.1:", port, " (port in use? change it with --port)");
        return false;
    }
    thread_ = std::thread([this] { svr_->listen_after_bind(); });
    return true;
}

void WebServer::stop() {
    if (stopping_.exchange(true)) return;
    if (svr_) svr_->stop();
    if (thread_.joinable()) thread_.join();
}

void WebServer::routes() {
    auto& s = *svr_;

    // Only answer requests addressed to localhost (defends against DNS
    // rebinding), and require a custom header on state-changing calls so
    // other websites cannot drive the API via cross-site requests.
    s.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        std::string host = req.get_header_value("Host");
        auto colon = host.rfind(':');
        if (colon != std::string::npos && host.find(']', colon) == std::string::npos) host.resize(colon);
        if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") {
            res.status = 403;
            res.set_content("forbidden host", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method == "POST" && req.get_header_value("X-USC") != "1") {
            res.status = 403;
            res.set_content("missing X-USC header", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // --- static UI ---------------------------------------------------------
    if (!webDir_.empty()) {
        s.set_mount_point("/", webDir_);
        LOG_INFO("server", "serving UI from ", webDir_);
    } else {
        for (const auto& a : assets::all()) {
            std::string path = a.path;
            auto handler = [a, path](const httplib::Request&, httplib::Response& res) {
                res.set_content(reinterpret_cast<const char*>(a.data), a.size, mimeFor(path));
                res.set_header("Cache-Control", "no-cache");
            };
            s.Get(path, handler);
            if (path == "/index.html") {
                s.Get("/", handler);
                s.Get("/overlay", handler);
            }
        }
    }

    // --- API ---------------------------------------------------------------
    s.Get("/api/settings", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, app_.settings().snapshot());
    });

    s.Post("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
        json patch = json::parse(req.body, nullptr, false);
        if (patch.is_discarded() || !patch.is_object()) return sendJson(res, {{"error", "invalid JSON"}}, 400);
        sendJson(res, app_.applySettings(patch));
    });

    s.Post("/api/settings/reset", [this](const httplib::Request&, httplib::Response& res) {
        // Keep credentials, reset everything else.
        json cur = app_.settings().snapshot();
        json d = Settings::defaults();
        for (const char* p : {"twitch", "youtube"}) {
            d[p]["clientId"] = cur[p]["clientId"];
            d[p]["clientSecret"] = cur[p]["clientSecret"];
            d[p]["enabled"] = cur[p]["enabled"];
        }
        d["kick"]["username"] = cur["kick"]["username"];
        d["kick"]["enabled"] = cur["kick"]["enabled"];
        d["google"] = cur["google"];
        d["audio"] = cur["audio"]; // keep the chosen speaker across resets
        d["window"] = cur["window"];
        d["server"] = cur["server"];
        sendJson(res, app_.applySettings(d));
    });

    s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, app_.status()); });

    s.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
        auto sub = app_.hub().subscribe();
        auto greeted = std::make_shared<bool>(false);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, sub, greeted](size_t, httplib::DataSink& sink) {
                if (stopping_ || sub->closed()) return false;
                std::string out;
                if (!*greeted) {
                    *greeted = true;
                    out = "retry: 2000\nevent: hello\ndata: " + app_.status().dump() + "\n\n";
                } else if (auto f = sub->next(15000)) {
                    out = *f;
                } else {
                    out = ": keepalive\n\n";
                }
                return sink.write(out.data(), out.size());
            },
            [this, sub](bool) { app_.hub().unsubscribe(sub); });
    });

    s.Get("/api/tts", [this](const httplib::Request& req, httplib::Response& res) {
        std::string text = req.get_param_value("text");
        std::string lang = req.get_param_value("lang");
        if (text.empty() || util::utf8Length(text) > 1000) {
            res.status = 400;
            return;
        }
        json g = app_.settings().snapshot()["google"];
        auto audio = app_.tts().synthesize(text, lang, g.value("apiKey", ""), g.value("cloudVoice", ""));
        if (!audio) {
            res.status = 502;
            res.set_content("tts failed", "text/plain");
            return;
        }
        res.set_header("Cache-Control", "max-age=3600");
        res.set_content(*audio, "audio/mpeg");
    });

    s.Post("/api/auth/:platform/login", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        std::string url = app_.beginLogin(req.path_params.at("platform"), error);
        if (url.empty()) return sendJson(res, {{"error", error}}, 400);
        sendJson(res, {{"url", url}});
    });

    s.Post("/api/auth/:platform/logout", [this](const httplib::Request& req, httplib::Response& res) {
        app_.logout(req.path_params.at("platform"));
        sendJson(res, app_.status());
    });

    s.Get("/auth/:platform/callback", [this](const httplib::Request& req, httplib::Response& res) {
        std::string platform = req.path_params.at("platform");
        if (req.has_param("error")) {
            std::string e = req.get_param_value("error_description");
            res.set_content(authPage(false, e.empty() ? req.get_param_value("error") : e), "text/html; charset=utf-8");
            return;
        }
        std::string error = app_.finishLogin(platform, req.get_param_value("code"), req.get_param_value("state"));
        res.set_content(authPage(error.empty(), error.empty() ? "Your " + platform + " account is linked." : error),
                        "text/html; charset=utf-8");
    });

    // --- native audio output ---------------------------------------------
    s.Get("/api/audio/devices", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, app_.audio().devices());
    });
    s.Post("/api/audio/test", [this](const httplib::Request&, httplib::Response& res) {
        app_.testVoice();
        sendJson(res, {{"ok", true}});
    });
    s.Post("/api/audio/skip", [this](const httplib::Request&, httplib::Response& res) {
        app_.audio().skip();
        sendJson(res, {{"ok", true}});
    });
    s.Post("/api/audio/clear", [this](const httplib::Request&, httplib::Response& res) {
        app_.audio().clear();
        sendJson(res, {{"ok", true}});
    });

    // --- links -----------------------------------------------------------
    s.Get("/api/links", [this](const httplib::Request&, httplib::Response& res) { sendJson(res, app_.links()); });
    // Opens one of our own links in the system browser (only known targets,
    // never an arbitrary URL).
    s.Post("/api/open", [this](const httplib::Request& req, httplib::Response& res) {
        json links = app_.links();
        std::string target = req.get_param_value("target");
        if (!links.contains(target)) return sendJson(res, {{"error", "unknown link"}}, 400);
        util::openInSystemBrowser(links[target].get<std::string>());
        sendJson(res, {{"ok", true}});
    });

    s.Post("/api/reconnect", [this](const httplib::Request& req, httplib::Response& res) {
        app_.reconnect(req.get_param_value("platform"));
        sendJson(res, {{"ok", true}});
    });

    s.Post("/api/test", [this](const httplib::Request&, httplib::Response& res) {
        app_.injectTestMessages();
        sendJson(res, {{"ok", true}});
    });

    s.Post("/api/clear", [this](const httplib::Request&, httplib::Response& res) {
        app_.clearChat();
        sendJson(res, {{"ok", true}});
    });

    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        LOG_ERROR("server", what);
        sendJson(res, {{"error", what}}, 500);
    });
}

} // namespace usc
