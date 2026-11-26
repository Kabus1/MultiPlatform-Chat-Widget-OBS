#include "integrations/ObsWatcher.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/WebSocketClient.hpp"

namespace usc {

using nlohmann::json;

namespace {
constexpr int kOpHello = 0, kOpIdentify = 1, kOpIdentified = 2, kOpEvent = 5, kOpRequest = 6, kOpRequestResponse = 7;
constexpr int kEventInputs = 1 << 3;  // EventSubscription::Inputs (InputCreated / Removed / ...)
constexpr int kEventOutputs = 1 << 6; // EventSubscription::Outputs (StreamStateChanged, VirtualcamStateChanged)
} // namespace

std::string ObsWatcher::authResponse(const std::string& password, const std::string& salt, const std::string& challenge) {
    std::string secret = util::base64Encode(util::sha256(password + salt));
    return util::base64Encode(util::sha256(secret + challenge));
}

ObsWatcher::ObsWatcher(Config cfg, StreamSink onStreamState, StatusSink onStatus)
    : cfg_(std::move(cfg)), onStreamState_(std::move(onStreamState)), onStatus_(std::move(onStatus)) {}

ObsWatcher::~ObsWatcher() { stop(); }

void ObsWatcher::setReady(bool ready) {
    bool was = identified_.exchange(ready);
    if (was != ready && onReady_) onReady_(ready);
}

void ObsWatcher::failPending(const std::string& why) {
    std::map<std::string, ResponseFn> pending;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.swap(pending_);
    }
    for (auto& [id, cb] : pending) cb(false, nlohmann::json::object(), why);
}

bool ObsWatcher::request(const std::string& type, const json& data, ResponseFn cb) {
    std::lock_guard<std::mutex> wsLock(wsMutex_);
    if (!ws_ || !identified_) return false;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        id = "usc-" + std::to_string(++nextRequest_);
        pending_[id] = std::move(cb);
    }
    json d = {{"requestType", type}, {"requestId", id}};
    if (data.is_object() && !data.empty()) d["requestData"] = data;
    if (ws_->send(json{{"op", kOpRequest}, {"d", d}}.dump())) return true;
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.erase(id);
    return false;
}

void ObsWatcher::report(const std::string& state, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        std::string key = state + '\x1f' + detail;
        if (key == lastStatus_) return;
        lastStatus_ = key;
    }
    onStatus_(state, detail);
}

void ObsWatcher::start() {
    if (ws_) return;
    WebSocketClient::Callbacks cb;
    cb.onMessage = [this](const std::string& text) { onText(text); };
    cb.onClose = [this](int code, const std::string& reason) {
        bool wasStreaming = streaming_.exchange(false);
        (void)wasStreaming;
        setReady(false);
        failPending("OBS connection closed");
        if (code == 4009) { // AuthenticationFailed
            authFailed_ = true;
            report("error", "OBS rejected the WebSocket password - check it in Settings > Advanced > OBS");
            ws_->setNextRetryDelay(60000);
        } else if (!authFailed_) {
            report("connecting", "OBS closed the connection" + (reason.empty() ? std::string() : " (" + reason + ")"));
        }
    };
    cb.onError = [this](const std::string&) {
        // OBS not running / WebSocket server off: stay quiet, keep retrying
        // slowly (local connection only, costs nothing).
        if (!authFailed_)
            report("waiting", "OBS not found on " + cfg_.host + ":" + std::to_string(cfg_.port) +
                                     " (start OBS and enable Tools > WebSocket Server Settings)");
    };
    ws_ = std::make_unique<WebSocketClient>("ws://" + cfg_.host + ":" + std::to_string(cfg_.port), std::move(cb));
    report("connecting", "Looking for OBS...");
    ws_->start();
}

void ObsWatcher::stop() {
    identified_ = false; // new requests are refused from here on
    if (ws_) ws_->stop(); // joins its thread: no callbacks afterwards
    {
        std::lock_guard<std::mutex> lock(wsMutex_);
        ws_.reset();
    }
    failPending("OBS connection stopped");
}

void ObsWatcher::setStreaming(bool on) {
    bool was = streaming_.exchange(on);
    report("connected", on ? "OBS is streaming" : "OBS connected (not streaming)");
    if (on != was) {
        LOG_INFO("obs", on ? "stream started" : "stream stopped");
        if (onStreamState_) onStreamState_(on);
    }
}

void ObsWatcher::onText(const std::string& payload) {
    json j = json::parse(payload, nullptr, false);
    if (!j.is_object()) return;
    const int op = j.value("op", -1);
    const json d = j.value("d", json::object());
    if (op == kOpHello) {
        json identify = {{"rpcVersion", 1}, {"eventSubscriptions", kEventOutputs | kEventInputs}};
        if (d.contains("authentication") && d["authentication"].is_object()) {
            if (cfg_.password.empty()) {
                authFailed_ = true;
                report("error", "OBS WebSocket needs a password: enter it in Settings > Advanced > OBS");
                ws_->setNextRetryDelay(60000);
                return;
            }
            const json& a = d["authentication"];
            identify["authentication"] = authResponse(cfg_.password, a.value("salt", ""), a.value("challenge", ""));
        }
        ws_->send(json{{"op", kOpIdentify}, {"d", identify}}.dump());
    } else if (op == kOpIdentified) {
        authFailed_ = false;
        report("connected", "OBS connected");
        // Already streaming when we connected?
        ws_->send(json{{"op", kOpRequest}, {"d", {{"requestType", "GetStreamStatus"}, {"requestId", "usc-stream"}}}}.dump());
        setReady(true);
    } else if (op == kOpRequestResponse && d.value("requestId", "") == "usc-stream") {
        setStreaming(d.value("responseData", json::object()).value("outputActive", false));
    } else if (op == kOpRequestResponse) {
        ResponseFn cb;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pending_.find(d.value("requestId", ""));
            if (it == pending_.end()) return;
            cb = std::move(it->second);
            pending_.erase(it);
        }
        const json st = d.value("requestStatus", json::object());
        const bool ok = st.value("result", false);
        std::string err;
        if (!ok)
            err = st.value("comment", std::string("request failed")) + " (code " + std::to_string(st.value("code", 0)) + ")";
        cb(ok, d.value("responseData", json::object()), err);
    } else if (op == kOpEvent) {
        const std::string type = d.value("eventType", "");
        const json ev = d.value("eventData", json::object());
        if (type == "StreamStateChanged") {
            const std::string state = ev.value("outputState", "");
            if (state == "OBS_WEBSOCKET_OUTPUT_STARTED") setStreaming(true);
            else if (state == "OBS_WEBSOCKET_OUTPUT_STOPPED") setStreaming(false);
        }
        if (onEvent_) onEvent_(type, ev);
    }
}

} // namespace usc
