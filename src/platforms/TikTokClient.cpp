#include "platforms/TikTokClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"
#include "net/Protobuf.hpp"

#include <nlohmann/json.hpp>

#include <miniz.h>

#include <chrono>
#include <cstdlib>
#include <regex>

namespace usc {

using nlohmann::json;

// Protobuf field numbers of TikTok's webcast schema (as documented by the
// open-source TikTok-Live-Connector project). Kept in one place because
// TikTok occasionally renumbers them.
namespace field {
// WebcastPushFrame
constexpr uint32_t kFrameSeqId = 1, kFrameLogId = 2, kFrameHeaders = 5, kFrameEncoding = 6, kFrameType = 7,
                   kFramePayload = 8;
// ProtoMessageFetchResult
constexpr uint32_t kResMessages = 1, kResCursor = 2, kResInternalExt = 5, kResRouteParams = 7, kResHeartbeat = 8,
                   kResNeedsAck = 9, kResPushServer = 10;
// BaseProtoMessage
constexpr uint32_t kMsgMethod = 1, kMsgPayload = 2;
// WebcastChatMessage / CommonMessageData / User
constexpr uint32_t kChatCommon = 1, kChatUser = 2, kChatContent = 3;
constexpr uint32_t kCommonMsgId = 2, kCommonCreateTime = 4;
constexpr uint32_t kUserId = 1, kUserNickname = 3, kUserUniqueId = 38;
// WebcastControlMessage
constexpr uint32_t kControlAction = 2;
constexpr uint64_t kControlStreamEnded = 3;
// Map entry
constexpr uint32_t kKey = 1, kValue = 2;
} // namespace field

namespace {

const char* kDefaultPushServer = "wss://webcast-ws.tiktok.com/webcast/im/push/v2/";

std::string str(std::string_view v) { return std::string(v.data(), v.size()); }

std::pair<std::string, std::string> mapEntry(std::string_view data) {
    pb::Reader r(data);
    pb::Field f;
    std::string k, v;
    while (r.next(f)) {
        if (f.number == field::kKey && f.wire == pb::Bytes) k = str(f.bytes);
        else if (f.number == field::kValue && f.wire == pb::Bytes) v = str(f.bytes);
    }
    return {k, v};
}

// "name=value; Path=/; ..." lines -> "name=value; name2=value2"
std::string mergeCookies(const std::string& existing, const std::vector<std::string>& setCookies) {
    std::map<std::string, std::string> jar;
    auto add = [&](const std::string& pair) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) return;
        std::string k = util::trim(pair.substr(0, eq));
        if (!k.empty()) jar[k] = util::trim(pair.substr(eq + 1));
    };
    size_t start = 0;
    while (start < existing.size()) {
        size_t end = existing.find(';', start);
        add(existing.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    for (const auto& line : setCookies) add(line.substr(0, line.find(';')));
    std::string out;
    for (const auto& [k, v] : jar) out += (out.empty() ? "" : "; ") + k + "=" + v;
    return out;
}

} // namespace

// --------------------------------------------------------------------------
// Protocol helpers
// --------------------------------------------------------------------------

std::optional<TikTokClient::PushFrame> TikTokClient::parsePushFrame(std::string_view data) {
    PushFrame f;
    pb::Reader r(data);
    pb::Field x;
    while (r.next(x)) {
        switch (x.number) {
        case field::kFrameSeqId: f.seqId = x.varint; break;
        case field::kFrameLogId: f.logId = x.varint; break;
        case field::kFrameHeaders:
            if (x.wire == pb::Bytes) f.headers.insert(mapEntry(x.bytes));
            break;
        case field::kFrameEncoding: f.payloadEncoding = str(x.bytes); break;
        case field::kFrameType: f.payloadType = str(x.bytes); break;
        case field::kFramePayload: f.payload = str(x.bytes); break;
        default: break;
        }
    }
    if (!r.ok() || f.payloadType.empty()) return std::nullopt;
    return f;
}

std::string TikTokClient::encodePushFrame(const PushFrame& f) {
    pb::Writer w;
    if (f.seqId) w.varint(field::kFrameSeqId, f.seqId);
    if (f.logId) w.varint(field::kFrameLogId, f.logId);
    for (const auto& [k, v] : f.headers) {
        pb::Writer e;
        e.bytes(field::kKey, k).bytes(field::kValue, v);
        w.bytes(field::kFrameHeaders, e.str());
    }
    w.bytes(field::kFrameEncoding, f.payloadEncoding);
    w.bytes(field::kFrameType, f.payloadType);
    w.bytes(field::kFramePayload, f.payload);
    return w.str();
}

std::optional<TikTokClient::FetchResult> TikTokClient::parseFetchResult(std::string_view data) {
    FetchResult res;
    pb::Reader r(data);
    pb::Field x;
    while (r.next(x)) {
        switch (x.number) {
        case field::kResMessages: {
            pb::Reader m(x.bytes);
            pb::Field y;
            std::string method, payload;
            while (m.next(y)) {
                if (y.number == field::kMsgMethod && y.wire == pb::Bytes) method = str(y.bytes);
                else if (y.number == field::kMsgPayload && y.wire == pb::Bytes) payload = str(y.bytes);
            }
            if (!method.empty()) res.messages.emplace_back(std::move(method), std::move(payload));
            break;
        }
        case field::kResCursor: res.cursor = str(x.bytes); break;
        case field::kResInternalExt: res.internalExt = str(x.bytes); break;
        case field::kResRouteParams:
            if (x.wire == pb::Bytes) res.routeParams.insert(mapEntry(x.bytes));
            break;
        case field::kResHeartbeat:
            if (x.wire == pb::Varint && x.varint > 0) res.heartbeatMs = static_cast<int>(x.varint);
            break;
        case field::kResNeedsAck: res.needsAck = x.varint != 0; break;
        case field::kResPushServer: res.pushServer = str(x.bytes); break;
        default: break;
        }
    }
    if (!r.ok()) return std::nullopt;
    return res;
}

std::optional<ChatMessage> TikTokClient::parseChatMessage(std::string_view payload) {
    ChatMessage m;
    m.platform = "tiktok";
    pb::Reader r(payload);
    pb::Field x;
    while (r.next(x)) {
        if (x.number == field::kChatCommon && x.wire == pb::Bytes) {
            pb::Reader c(x.bytes);
            pb::Field y;
            while (c.next(y)) {
                if (y.number == field::kCommonMsgId && y.wire == pb::Varint) m.id = "tt-" + std::to_string(y.varint);
                if (y.number == field::kCommonCreateTime && y.wire == pb::Varint && y.varint > 1000000000000ULL)
                    m.timestamp = static_cast<int64_t>(y.varint);
            }
        } else if (x.number == field::kChatUser && x.wire == pb::Bytes) {
            pb::Reader u(x.bytes);
            pb::Field y;
            while (u.next(y)) {
                if (y.number == field::kUserId && y.wire == pb::Varint) m.userId = std::to_string(y.varint);
                else if (y.number == field::kUserNickname && y.wire == pb::Bytes) m.displayName = str(y.bytes);
                else if (y.number == field::kUserUniqueId && y.wire == pb::Bytes) m.username = str(y.bytes);
            }
        } else if (x.number == field::kChatContent && x.wire == pb::Bytes) {
            m.parts.push_back({MessagePart::Type::Text, str(x.bytes), ""});
        }
    }
    if (!r.ok() || m.parts.empty() || m.parts[0].text.empty()) return std::nullopt;
    if (m.username.empty()) m.username = m.displayName;
    if (m.displayName.empty()) m.displayName = m.username;
    if (m.timestamp == 0) m.timestamp = util::nowMs();
    return m;
}

bool TikTokClient::isStreamEnd(std::string_view payload) {
    pb::Reader r(payload);
    pb::Field x;
    while (r.next(x))
        if (x.number == field::kControlAction && x.wire == pb::Varint) return x.varint == field::kControlStreamEnded;
    return false;
}

TikTokClient::RoomInfo TikTokClient::parseRoomInfo(const std::string& body) {
    RoomInfo info;
    json j = json::parse(body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        const json data = j.value("data", json::object());
        const json user = data.is_object() ? data.value("user", json::object()) : json::object();
        if (user.is_object() && user.contains("roomId")) {
            const json& id = user["roomId"];
            info.roomId = id.is_string() ? id.get<std::string>() : id.is_number() ? std::to_string(id.get<uint64_t>()) : "";
            info.found = true;
            int status = user.value("status", 0);
            if (data.contains("liveRoom") && data["liveRoom"].is_object()) status = data["liveRoom"].value("status", status);
            info.live = status == 2 && !info.roomId.empty() && info.roomId != "0";
        }
        return info;
    }
    // HTML fallback (tiktok.com/@user/live): embedded state JSON.
    static const std::regex roomRe(R"re("roomId"\s*:\s*"(\d+)")re");
    static const std::regex statusRe(R"re("roomId"\s*:\s*"\d+"[^{}]{0,400}?"status"\s*:\s*(\d+))re");
    std::smatch m;
    if (std::regex_search(body, m, roomRe)) {
        info.found = true;
        info.roomId = m[1];
        std::smatch s;
        info.live = std::regex_search(body, s, statusRe) ? s[1] == "2" : info.roomId != "0";
    }
    return info;
}

bool TikTokClient::gunzip(std::string_view in, std::string& out) {
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    size_t n = in.size();
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return false;
    const unsigned char flags = p[3];
    size_t pos = 10;
    if (flags & 4) { // FEXTRA
        if (pos + 2 > n) return false;
        pos += 2 + (p[pos] | (p[pos + 1] << 8));
    }
    if (flags & 8) while (pos < n && p[pos++]) {}   // FNAME
    if (flags & 16) while (pos < n && p[pos++]) {}  // FCOMMENT
    if (flags & 2) pos += 2;                         // FHCRC
    if (pos >= n - 8) return false;
    size_t outLen = 0;
    void* buf = tinfl_decompress_mem_to_heap(p + pos, n - pos - 8, &outLen, 0); // raw deflate
    if (!buf) return false;
    out.assign(static_cast<const char*>(buf), outLen);
    mz_free(buf);
    return true;
}

std::string TikTokClient::heartbeatFrame(const std::string& roomId) {
    PushFrame f;
    f.payloadEncoding = "pb";
    f.payloadType = "hb";
    f.payload = pb::Writer().varint(1, std::strtoull(roomId.c_str(), nullptr, 10)).str();
    return encodePushFrame(f);
}

std::string TikTokClient::enterRoomFrame(const std::string& roomId) {
    // ImEnterRoomMessage: roomId=1, liveId=4, identity=5, filterWelcomeMsg=9
    pb::Writer msg;
    msg.varint(1, std::strtoull(roomId.c_str(), nullptr, 10)).varint(4, 12).bytes(5, "audience").bytes(9, "0");
    PushFrame f;
    f.payloadEncoding = "pb";
    f.payloadType = "im_enter_room";
    f.payload = msg.str();
    return encodePushFrame(f);
}

// --------------------------------------------------------------------------
// Client
// --------------------------------------------------------------------------

TikTokClient::TikTokClient(Config cfg, MessageSink onMessage, StatusSink onStatus)
    : cfg_(std::move(cfg)), onMessage_(std::move(onMessage)), onStatus_(std::move(onStatus)) {}

TikTokClient::~TikTokClient() { stop(); }

void TikTokClient::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run(); });
}

void TikTokClient::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::unique_ptr<WebSocketClient> ws;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws = std::move(ws_);
    }
    if (ws) ws->stop();
    onStatus_("disconnected", "");
}

bool TikTokClient::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load(); });
    return running_;
}

TikTokClient::RoomInfo TikTokClient::resolveRoom(std::string& error) {
    std::string user = util::trim(cfg_.username);
    if (auto p = user.find("tiktok.com/"); p != std::string::npos) user = user.substr(p + 11);
    if (!user.empty() && user[0] == '@') user.erase(0, 1);
    if (auto p = user.find_first_of("/?# "); p != std::string::npos) user = user.substr(0, p);

    const http::Headers headers = {{"Accept-Language", "en-US,en;q=0.9"}, {"Referer", "https://www.tiktok.com/"}};
    auto r = http::get("https://www.tiktok.com/api-live/user/room/?" +
                           http::buildQuery({{"aid", "1988"}, {"app_name", "tiktok_web"}, {"device_platform", "web_pc"},
                                             {"app_language", "en"}, {"browser_language", "en-US"},
                                             {"sourceType", "54"}, {"uniqueId", user}}),
                       headers, 10);
    cookies_ = mergeCookies(cookies_, r.headerValues("Set-Cookie"));
    RoomInfo info = r.ok() ? parseRoomInfo(r.body) : RoomInfo{};
    if (!info.found) {
        auto page = http::get("https://www.tiktok.com/@" + util::urlEncode(user) + "/live", headers, 10);
        cookies_ = mergeCookies(cookies_, page.headerValues("Set-Cookie"));
        if (page.ok()) info = parseRoomInfo(page.body);
        if (!info.found)
            error = page.status == 0 ? page.error
                                     : "could not find @" + user + " (HTTP " + std::to_string(page.status) + ")";
    }
    return info;
}

std::optional<TikTokClient::ConnectInfo> TikTokClient::fetchConnect(const std::string& roomId, std::string& error) {
    std::string server = util::trim(cfg_.signServer);
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty()) {
        error = "no signing server configured";
        return std::nullopt;
    }
    http::Params q = {{"client", "ttlive-node"}, {"room_id", roomId}, {"unique_id", cfg_.username}};
    if (!cfg_.apiKey.empty()) q.push_back({"apiKey", cfg_.apiKey});
    http::Headers h;
    if (!cfg_.apiKey.empty()) h.push_back({"x-api-key", cfg_.apiKey});
    auto r = http::get(server + "/webcast/fetch?" + http::buildQuery(q), h, 15);
    if (r.status == 401 || r.status == 403) {
        error = "signing server refused the request (HTTP " + std::to_string(r.status) +
                ") - check the TikTok API key in Settings";
        return std::nullopt;
    }
    if (r.status == 429) {
        error = "signing server rate limit reached (HTTP 429)";
        return std::nullopt;
    }
    if (!r.ok()) {
        error = r.status == 0 ? r.error : "signing server HTTP " + std::to_string(r.status);
        return std::nullopt;
    }
    for (const char* name : {"x-set-tt-cookie", "Set-Cookie"}) {
        std::vector<std::string> lines;
        for (const auto& v : r.headerValues(name)) {
            // x-set-tt-cookie packs several cookies into one "a=b; c=d" value.
            size_t start = 0;
            while (start < v.size()) {
                size_t end = v.find(';', start);
                lines.push_back(util::trim(v.substr(start, end == std::string::npos ? std::string::npos : end - start)));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        cookies_ = mergeCookies(cookies_, lines);
    }

    ConnectInfo info;
    info.cookies = cookies_;
    // Some signing servers answer with JSON, others with the raw protobuf.
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        for (const char* k : {"wsUrl", "ws_url", "websocketUrl", "url"})
            if (j.contains(k) && j[k].is_string()) info.wsUrl = j[k].get<std::string>();
        for (const char* k : {"cookies", "cookie"})
            if (j.contains(k) && j[k].is_string()) info.cookies = mergeCookies(cookies_, {j[k].get<std::string>()});
        if (info.wsUrl.empty()) {
            error = "signing server response has no WebSocket URL";
            return std::nullopt;
        }
        return info;
    }
    auto fetch = parseFetchResult(r.body);
    if (!fetch) {
        error = "unrecognised signing server response";
        return std::nullopt;
    }
    http::Params params = {
        {"aid", "1988"}, {"app_language", "en-US"}, {"app_name", "tiktok_web"}, {"browser_language", "en-US"},
        {"browser_name", "Mozilla"}, {"browser_online", "true"}, {"browser_platform", "Win32"},
        {"cookie_enabled", "true"}, {"device_platform", "web"}, {"did_rule", "3"}, {"fetch_rule", "1"},
        {"identity", "audience"}, {"live_id", "12"}, {"resp_content_type", "protobuf"}, {"version_code", "180800"},
        {"webcast_sdk_version", "1.3.0"}, {"update_version_code", "1.3.0"}, {"compress", "gzip"},
        {"heartbeatDuration", "0"}, {"sup_ws_ds_opt", "1"}, {"room_id", roomId}, {"cursor", fetch->cursor},
        {"internal_ext", fetch->internalExt},
    };
    for (const auto& [k, v] : fetch->routeParams) params.push_back({k, v});
    info.wsUrl = (fetch->pushServer.empty() ? std::string(kDefaultPushServer) : fetch->pushServer) + "?" +
                 http::buildQuery(params);
    return info;
}

void TikTokClient::onFrame(const std::string& data) {
    lastFrameMs_ = util::nowMs();
    auto frame = parsePushFrame(data);
    if (!frame || frame->payloadType != "msg") return;
    std::string payload = std::move(frame->payload);
    std::string inflated;
    if (gunzip(payload, inflated)) payload = std::move(inflated);
    auto res = parseFetchResult(payload);
    if (!res) return;
    for (const auto& [method, body] : res->messages) {
        if (method == "WebcastChatMessage") {
            if (auto m = parseChatMessage(body)) onMessage_(std::move(*m));
        } else if (method == "WebcastControlMessage" && isStreamEnd(body)) {
            streamEnded_ = true;
        }
    }
    if (res->needsAck) {
        PushFrame ack;
        ack.logId = frame->logId;
        ack.payloadEncoding = "pb";
        ack.payloadType = "ack";
        ack.payload = res->internalExt;
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_) ws_->sendBinary(encodePushFrame(ack));
    }
}

void TikTokClient::run() {
    const std::string handle = "@" + util::trim(cfg_.username);
    if (util::trim(cfg_.username).empty()) {
        onStatus_("error", "Enter the TikTok username in Settings > Connections");
        return;
    }
    while (running_) {
        onStatus_("connecting", "Looking up " + handle + "...");
        std::string error;
        RoomInfo room = resolveRoom(error);
        if (!room.found) {
            onStatus_("error", "TikTok: " + error + " - retrying in 60s");
            if (!sleepFor(60000)) return;
            continue;
        }
        if (!room.live) {
            onStatus_("waiting", handle + " is not live - checking again in 60s");
            if (!sleepFor(60000)) return;
            continue;
        }
        roomId_ = room.roomId;
        auto info = fetchConnect(roomId_, error);
        if (!info) {
            onStatus_("error", "TikTok: " + error);
            if (!sleepFor(60000)) return;
            continue;
        }

        streamEnded_ = false;
        lastFrameMs_ = util::nowMs();
        WebSocketClient::Callbacks cb;
        cb.onOpen = [this, handle] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ws_) ws_->sendBinary(enterRoomFrame(roomId_));
            onStatus_("connected", handle + " (live)");
        };
        cb.onBinary = [this](const std::string& d) { onFrame(d); };
        cb.onClose = [this](int code, const std::string& reason) {
            if (running_) onStatus_("connecting", "Disconnected (" + std::to_string(code) + (reason.empty() ? "" : " " + reason) + "), reconnecting...");
        };
        cb.onError = [this](const std::string& reason) { onStatus_("error", "TikTok connection error: " + reason); };
        WebSocketClient::Options opt;
        opt.headers = {{"Cookie", info->cookies},
                       {"User-Agent", http::browserUserAgent()},
                       {"Origin", "https://www.tiktok.com"}};
        // Each reconnect needs a fresh cursor / signature.
        opt.urlProvider = [this]() -> std::string {
            std::string err;
            auto fresh = fetchConnect(roomId_, err);
            return fresh ? fresh->wsUrl : std::string();
        };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_ = std::make_unique<WebSocketClient>(info->wsUrl, std::move(cb), std::move(opt));
            ws_->start();
        }

        // Heartbeat + liveness watchdog.
        while (sleepFor(10000)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ws_) ws_->sendBinary(heartbeatFrame(roomId_));
            }
            if (streamEnded_) break;
            if (util::nowMs() - lastFrameMs_ > 90000) {
                LOG_WARN("tiktok", "no data for 90s, reconnecting");
                lastFrameMs_ = util::nowMs();
                std::lock_guard<std::mutex> lock(mutex_);
                if (ws_) ws_->reconnect();
            }
        }
        std::unique_ptr<WebSocketClient> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old = std::move(ws_);
        }
        if (old) old->stop();
        if (!running_) return;
        onStatus_("waiting", handle + "'s live stream ended - checking again in 60s");
        if (!sleepFor(60000)) return;
    }
}

} // namespace usc
