#include "integrations/ObsCameraFeed.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"

#include <algorithm>
#include <chrono>

namespace usc {

using nlohmann::json;

namespace {

bool getBool(const json& j, const char* k, bool d) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : d;
}
std::string getStr(const json& j, const char* k, const std::string& d) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : d;
}

} // namespace

ObsCameraFeed::Config ObsCameraFeed::Config::fromJson(const json& j) {
    Config c;
    if (!j.is_object()) return c;
    c.enabled = getBool(j, "enabled", false);
    c.sceneName = util::trim(getStr(j, "sceneName", c.sceneName));
    if (c.sceneName.empty()) c.sceneName = "USC TikTok Feed";
    c.sourceName = util::trim(getStr(j, "sourceName", ""));
    c.autoStartVirtualCam = getBool(j, "autoStartVirtualCam", false);
    return c;
}

// ------------------------------------------------------------------ helpers

bool ObsCameraFeed::isVideoCaptureKind(const std::string& kind) {
    static const char* kinds[] = {"dshow_input",     "av_capture_input",     "av_capture_input_v2",
                                  "macos-avcapture", "macos-avcapture-fast", "v4l2_input"};
    for (const char* k : kinds)
        if (kind == k) return true;
    return false;
}

std::string ObsCameraFeed::deviceFromSettings(const std::string& kind, const json& settings) {
    if (!settings.is_object()) return {};
    if (kind == "dshow_input") {
        // "Elgato 4K S:\\?\usb#vid_0fd9&pid_...": friendly name, ':', device path.
        std::string id = getStr(settings, "video_device_id", "");
        size_t cut = id.find(":\\");
        return util::trim(cut == std::string::npos ? id : id.substr(0, cut));
    }
    if (kind.rfind("av_capture_input", 0) == 0 || kind.rfind("macos-avcapture", 0) == 0) {
        std::string n = getStr(settings, "device_name", "");
        return util::trim(n.empty() ? getStr(settings, "device", "") : n);
    }
    return util::trim(getStr(settings, "device_id", "")); // v4l2: /dev/videoN
}

bool ObsCameraFeed::sameDevice(const std::string& a, const std::string& b) {
    std::string x = util::toLower(util::trim(a)), y = util::toLower(util::trim(b));
    return !x.empty() && x == y;
}

std::optional<ObsCameraFeed::CaptureInput> ObsCameraFeed::pickInput(const std::vector<CaptureInput>& inputs,
                                                                    const std::string& preferred) {
    if (inputs.empty()) return std::nullopt;
    if (!preferred.empty())
        for (const auto& i : inputs)
            if (i.name == preferred) return i;
    for (const auto& i : inputs)
        if (util::toLower(i.name).find("elgato") != std::string::npos ||
            util::toLower(i.device).find("elgato") != std::string::npos)
            return i;
    return inputs.front();
}

// ------------------------------------------------------------------ lifecycle

ObsCameraFeed::ObsCameraFeed(std::function<void(const json&)> onStatus) : onStatus_(std::move(onStatus)) {}

ObsCameraFeed::~ObsCameraFeed() { stop(); }

void ObsCameraFeed::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    worker_ = std::thread([this] { run(); });
}

void ObsCameraFeed::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    obs_.reset();
}

void ObsCameraFeed::attach(std::shared_ptr<ObsWatcher> obs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obs_ = std::move(obs);
        obsReady_ = obs_ && obs_->ready();
        if (!obsReady_) {
            inputs_.clear();
            chosen_.clear();
            sceneReady_ = false;
            vcamActive_ = false;
        }
    }
    cv_.notify_all();
    publish();
}

void ObsCameraFeed::apply(const Config& cfg) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = cfg.enabled != cfg_.enabled || cfg.sceneName != cfg_.sceneName || cfg.sourceName != cfg_.sourceName;
        bool autoOn = cfg.autoStartVirtualCam && !cfg_.autoStartVirtualCam;
        cfg_ = cfg;
        if (changed) {
            needInventory_ = true;
            needSetup_ = cfg_.enabled;
        }
        if (autoOn && obsReady_) vcamRequest_ = 1;
    }
    cv_.notify_all();
}

void ObsCameraFeed::onObsReady(bool ready) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obsReady_ = ready;
        if (ready) {
            needInventory_ = true;
            needSetup_ = cfg_.enabled;
            needVcamStatus_ = true;
            if (cfg_.autoStartVirtualCam) vcamRequest_ = 1;
        } else {
            inputs_.clear();
            chosen_.clear();
            sceneReady_ = false;
            vcamActive_ = false;
            needInventory_ = needSetup_ = needVcamStatus_ = false;
            vcamRequest_ = -1;
        }
    }
    cv_.notify_all();
    if (!ready) publish();
}

void ObsCameraFeed::onObsEvent(const std::string& type, const json& data) {
    if (type == "VirtualcamStateChanged") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            vcamActive_ = data.value("outputActive", false);
        }
        publish();
        return;
    }
    if (type == "InputCreated" || type == "InputRemoved" || type == "InputNameChanged" ||
        type == "InputSettingsChanged") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            needInventory_ = true;
        }
        cv_.notify_all();
    }
}

void ObsCameraFeed::setupNow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needInventory_ = true;
        needSetup_ = true;
        needVcamStatus_ = true;
    }
    cv_.notify_all();
}

void ObsCameraFeed::setVirtualCam(bool on) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        vcamRequest_ = on ? 1 : 0;
    }
    cv_.notify_all();
}

// ------------------------------------------------------------------ worker

void ObsCameraFeed::run() {
    for (;;) {
        bool inventory, setup, vcamStatus;
        int vcam;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return !running_ ||
                       (obsReady_ && (needInventory_ || needSetup_ || needVcamStatus_ || vcamRequest_ != -1));
            });
            if (!running_) return;
            inventory = needInventory_;
            setup = needSetup_;
            vcamStatus = needVcamStatus_;
            vcam = vcamRequest_;
            needInventory_ = needSetup_ = needVcamStatus_ = false;
            vcamRequest_ = -1;
        }
        try {
            if (inventory || setup) refreshInventory();
            if (setup) ensureScene();
            if (vcam != -1) {
                refreshVirtualCam();
                bool active;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    active = vcamActive_;
                }
                if (vcam == 1 && !active) {
                    std::string err;
                    if (!call("StartVirtualCam", json::object(), err)) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        error_ = "Could not start OBS Virtual Camera: " + err;
                    }
                } else if (vcam == 0 && active) {
                    std::string err;
                    call("StopVirtualCam", json::object(), err);
                }
                vcamStatus = true;
            }
            if (vcamStatus) refreshVirtualCam();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::string("OBS feed: ") + e.what();
        }
        publish();
    }
}

std::optional<json> ObsCameraFeed::call(const std::string& type, const json& data, std::string& error, int timeoutMs) {
    std::shared_ptr<ObsWatcher> obs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        obs = obs_;
    }
    if (!obs || !obs->ready()) {
        error = "OBS is not connected";
        return std::nullopt;
    }
    struct Box {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false;
        json data;
        std::string err;
    };
    auto box = std::make_shared<Box>();
    bool sent = obs->request(type, data, [box](bool ok, const json& d, const std::string& e) {
        {
            std::lock_guard<std::mutex> lock(box->m);
            box->done = true;
            box->ok = ok;
            box->data = d;
            box->err = e;
        }
        box->cv.notify_all();
    });
    if (!sent) {
        error = "OBS is not connected";
        return std::nullopt;
    }
    std::unique_lock<std::mutex> lock(box->m);
    if (!box->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return box->done; })) {
        error = "OBS did not answer " + type;
        return std::nullopt;
    }
    if (!box->ok) {
        error = box->err;
        return std::nullopt;
    }
    return box->data;
}

void ObsCameraFeed::refreshInventory() {
    std::string err;
    auto list = call("GetInputList", json::object(), err);
    if (!list) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not read OBS sources: " + err;
        return;
    }
    std::vector<CaptureInput> found;
    for (const auto& in : list->value("inputs", json::array())) {
        std::string kind = in.value("unversionedInputKind", in.value("inputKind", std::string()));
        if (!isVideoCaptureKind(kind) && !isVideoCaptureKind(in.value("inputKind", std::string()))) continue;
        CaptureInput ci;
        ci.name = in.value("inputName", "");
        ci.kind = kind;
        std::string e2;
        if (auto st = call("GetInputSettings", {{"inputName", ci.name}}, e2))
            ci.device = deviceFromSettings(kind, st->value("inputSettings", json::object()));
        if (!ci.name.empty()) found.push_back(std::move(ci));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    inputs_ = std::move(found);
    auto pick = pickInput(inputs_, cfg_.sourceName);
    chosen_ = pick ? pick->name : std::string();
    if (error_.rfind("Could not read OBS sources", 0) == 0) error_.clear();
}

void ObsCameraFeed::ensureScene() {
    std::string scene, source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scene = cfg_.sceneName;
        source = chosen_;
        sceneReady_ = false;
    }
    auto setError = [&](const std::string& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = e;
        LOG_WARN("obs", e);
    };
    if (source.empty())
        return setError("OBS has no Video Capture Device source yet. Add your capture card in OBS "
                        "(Sources > + > Video Capture Device), then press Set up again.");
    std::string err;
    auto scenes = call("GetSceneList", json::object(), err);
    if (!scenes) return setError("Could not read OBS scenes: " + err);
    bool exists = false;
    for (const auto& sc : scenes->value("scenes", json::array()))
        if (sc.value("sceneName", "") == scene) exists = true;
    if (!exists && !call("CreateScene", {{"sceneName", scene}}, err))
        return setError("Could not create the scene \"" + scene + "\": " + err);

    // The scene only *references* the existing source: OBS keeps a single
    // handle on the device, so nothing is opened twice.
    if (!call("GetSceneItemId", {{"sceneName", scene}, {"sourceName", source}}, err)) {
        auto created = call("CreateSceneItem", {{"sceneName", scene}, {"sourceName", source}, {"sceneItemEnabled", true}}, err);
        if (!created) return setError("Could not add \"" + source + "\" to \"" + scene + "\": " + err);
        int itemId = created->value("sceneItemId", 0);
        if (auto video = call("GetVideoSettings", json::object(), err)) {
            json transform = {{"positionX", 0},
                              {"positionY", 0},
                              {"alignment", 5}, // top-left
                              {"boundsType", "OBS_BOUNDS_SCALE_INNER"},
                              {"boundsAlignment", 0},
                              {"boundsWidth", std::max(1.0, video->value("baseWidth", 1920.0))},
                              {"boundsHeight", std::max(1.0, video->value("baseHeight", 1080.0))}};
            call("SetSceneItemTransform", {{"sceneName", scene}, {"sceneItemId", itemId}, {"sceneItemTransform", transform}},
                 err); // fitting is cosmetic: ignore a failure
        }
        LOG_INFO("obs", "TikTok feed scene \"", scene, "\" now shows \"", source, "\"");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sceneReady_ = true;
    error_.clear();
}

void ObsCameraFeed::refreshVirtualCam() {
    std::string err;
    auto st = call("GetVirtualCamStatus", json::object(), err);
    std::lock_guard<std::mutex> lock(mutex_);
    if (st) vcamActive_ = st->value("outputActive", false);
}

// ------------------------------------------------------------------ status

json ObsCameraFeed::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json inputs = json::array();
    for (const auto& i : inputs_) inputs.push_back({{"name", i.name}, {"kind", i.kind}, {"device", i.device}});
    std::string device;
    for (const auto& i : inputs_)
        if (i.name == chosen_) device = i.device;
    return {{"obsConnected", obsReady_},
            {"enabled", cfg_.enabled},
            {"sceneName", cfg_.sceneName},
            {"sceneReady", sceneReady_},
            {"source", chosen_},
            {"sourceDevice", device},
            {"sourcePreferred", cfg_.sourceName},
            {"inputs", inputs},
            {"virtualCamActive", vcamActive_},
            {"autoStartVirtualCam", cfg_.autoStartVirtualCam},
            {"error", error_}};
}

void ObsCameraFeed::publish() {
    if (onStatus_) onStatus_(status());
}

std::string ObsCameraFeed::conflictFor(const std::string& deviceName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!obsReady_) return {};
    for (const auto& i : inputs_)
        if (sameDevice(i.device, deviceName))
            return "OBS uses \"" + deviceName + "\" (source \"" + i.name +
                   "\"). A capture card can only be opened by one program: keep it in OBS and pick "
                   "\"OBS Virtual Camera\" in TikTok Studio (see TikTok feed above).";
    return {};
}

} // namespace usc
