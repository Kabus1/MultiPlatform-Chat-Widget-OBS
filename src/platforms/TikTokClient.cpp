#include "platforms/TikTokClient.hpp"

#include "core/Log.hpp"
#include "core/Util.hpp"
#include "net/Http.hpp"
#include "net/Protobuf.hpp"

#include <nlohmann/json.hpp>

#include <miniz.h>

#include <chrono>
#include <cstdio>
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
constexpr uint64_t kControlStreamEnded = 3, kControlStreamEndedBan = 4;
// Map entry
constexpr uint32_t kKey = 1, kValue = 2;
} // namespace field

namespace {


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
        if (x.number == field::kControlAction && x.wire == pb::Varint)
            return x.varint == field::kControlStreamEnded || x.varint == field::kControlStreamEndedBan;
    return false;
}

TikTokClient::RoomInfo TikTokClient::parseRoomInfo(const std::string& body) {
    RoomInfo info;
    json j = json::parse(body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        const int64_t code = j.contains("statusCode") && j["statusCode"].is_number() ? j["statusCode"].get<int64_t>() : 0;
        if (code == 19881007) { // user_not_found
            info.notFound = true;
            info.error = "account not found";
            return info;
        }
        if (code != 0) {
            info.error = "TikTok answered statusCode " + std::to_string(code) +
                         (j.contains("message") && j["message"].is_string() ? " (" + j["message"].get<std::string>() + ")" : "");
            return info;
        }
        const json data = j.value("data", json::object());
        const json user = data.is_object() ? data.value("user", json::object()) : json::object();
        if (!user.is_object() || user.empty()) {
            info.error = "unexpected answer (no user data)";
            return info;
        }
        info.found = true;
        if (user.contains("roomId")) {
            const json& id = user["roomId"];
            info.roomId = id.is_string() ? id.get<std::string>() : id.is_number() ? std::to_string(id.get<uint64_t>()) : "";
        }
        // Live when either the user or the room reports status 2.
        const int userStatus = user.value("status", 0);
        int roomStatus = 0;
        if (data.contains("liveRoom") && data["liveRoom"].is_object()) roomStatus = data["liveRoom"].value("status", 0);
        info.live = (userStatus == 2 || roomStatus == 2) && !info.roomId.empty() && info.roomId != "0";
        return info;
    }
    // HTML fallback (tiktok.com/@user/live): embedded state JSON.
    static const std::regex roomRe(R"re("roomId"\s*:\s*"(\d+)")re");
    static const std::regex statusRe(R"re("roomId"\s*:\s*"\d+"[^{}]{0,400}?"status"\s*:\s*(\d+))re");
    std::smatch m;
    if (std::regex_search(body, m, roomRe)) {
        info.found = true;
        info.roomId = m[1];
        std::smatch st;
        info.live = std::regex_search(body, st, statusRe) ? st[1] == "2" : info.roomId != "0";
    } else {
        info.error = "could not read the live status";
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
// Viewer-style connection helpers
// --------------------------------------------------------------------------

namespace {
const int kBurstRecheckMs = 15000;   // after Reconnect / OBS "stream started"
const int kBurstDurationMs = 180000;
const int kIdleFirstMs = 60000;
const int kHeartbeatMs = 10000;
const int kStaleMs = 60000;          // no frame at all for this long: reconnect
const int kJoinTimeoutMs = 30000;    // socket open but TikTok never answered

const char* kDirectHost = "webcast-ws.tiktok.com";
} // namespace

std::string TikTokClient::normalizeUser(const std::string& input) {
    std::string u = util::trim(input);
    if (auto p = u.find("tiktok.com/"); p != std::string::npos) u = u.substr(p + 11);
    if (!u.empty() && u[0] == '@') u.erase(0, 1);
    if (auto p = u.find_first_of("/?# "); p != std::string::npos) u = u.substr(0, p);
    return util::toLower(u);
}

std::string TikTokClient::extractTtwid(const std::vector<std::string>& setCookies) {
    for (const auto& line : setCookies) {
        std::string l = util::trim(line);
        if (l.rfind("ttwid=", 0) != 0) continue;
        std::string v = util::trim(l.substr(6, l.find(';') == std::string::npos ? std::string::npos : l.find(';') - 6));
        if (!v.empty()) return v;
    }
    return {};
}

std::string TikTokClient::buildDirectWsUrl(const std::string& roomId, double lastRtt) {
    char rtt[32];
    std::snprintf(rtt, sizeof(rtt), "%.3f", lastRtt);
    http::Params q = {
        {"version_code", "180800"},
        {"device_platform", "web"},
        {"cookie_enabled", "true"},
        {"screen_width", "1920"},
        {"screen_height", "1080"},
        {"browser_language", "en-US"},
        {"browser_platform", "Win32"},
        {"browser_name", "Mozilla"},
        {"browser_version", "5.0 (Windows NT 10.0; Win64; x64)"},
        {"browser_online", "true"},
        {"tz_name", "UTC"},
        {"app_name", "tiktok_web"},
        {"sup_ws_ds_opt", "1"},
        {"update_version_code", "2.0.0"},
        {"compress", "gzip"},
        {"webcast_language", "en"},
        {"ws_direct", "1"},
        {"aid", "1988"},
        {"live_id", "12"},
        {"app_language", "en"},
        {"client_enter", "1"},
        {"room_id", roomId},
        {"identity", "audience"},
        {"history_comment_count", "6"},
        {"last_rtt", rtt},
        {"heartbeat_duration", std::to_string(kHeartbeatMs)},
        {"resp_content_type", "protobuf"},
        {"did_rule", "3"},
    };
    return std::string("wss://") + kDirectHost + "/webcast/im/ws_proxy/ws_reuse_supplement/?" + http::buildQuery(q);
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

bool TikTokClient::checkNow() {
    if (!running_) return false;
    burstUntilMs_ = util::nowMs() + kBurstDurationMs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = true;
    }
    cv_.notify_all();
    return true;
}

bool TikTokClient::sleepFor(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return !running_.load() || wake_.load(); });
    wake_ = false;
    return running_;
}

bool TikTokClient::inBurst() const { return util::nowMs() < burstUntilMs_; }

int TikTokClient::nextIdleDelay() {
    if (inBurst()) return kBurstRecheckMs;
    int d = idleDelayMs_;
    idleDelayMs_ = std::min(idleDelayMs_ * 2, std::max(kIdleFirstMs, cfg_.idleMaxMs));
    return d;
}

TikTokClient::RoomInfo TikTokClient::resolveRoom() {
    const http::Headers headers = {{"Accept-Language", "en-US,en;q=0.9"}};
    auto r = http::get("https://www.tiktok.com/api-live/user/room?" +
                           http::buildQuery({{"aid", "1988"}, {"app_name", "tiktok_web"}, {"device_platform", "web_pc"},
                                             {"app_language", "en"}, {"browser_language", "en-US"}, {"region", "US"},
                                             {"user_is_login", "false"}, {"sourceType", "54"},
                                             {"staleTime", "600000"}, {"uniqueId", user_}}),
                       headers, 10);
    RoomInfo info;
    if (r.status == 403 || r.status == 429) {
        info.blocked = true;
        info.error = "TikTok is rate-limiting the live check (HTTP " + std::to_string(r.status) + ")";
        return info;
    }
    if (r.status == 0) {
        info.error = "no connection to TikTok (" + r.error + ")";
        return info;
    }
    if (r.ok()) info = parseRoomInfo(r.body);
    if (!info.found && !info.notFound) {
        // Fallback: the public live page embeds the same state.
        auto page = http::get("https://www.tiktok.com/@" + util::urlEncode(user_) + "/live", headers, 10);
        if (page.ok()) {
            RoomInfo fromPage = parseRoomInfo(page.body);
            if (fromPage.found) info = fromPage;
        }
        if (!info.found && info.error.empty()) info.error = "HTTP " + std::to_string(r.status);
    }
    return info;
}

std::string TikTokClient::fetchTtwid(std::string& error) {
    // TikTok sets the anonymous ttwid cookie on only some anonymous requests:
    // retry a few times. Redirects are not followed, the cookie is on the first answer.
    for (int attempt = 0; attempt < 8 && running_; ++attempt) {
        auto r = http::get("https://www.tiktok.com/", {{"Accept-Language", "en-US,en;q=0.9"}}, 10, false);
        if (r.status == 0) {
            error = "no connection to tiktok.com (" + r.error + ")";
            return {};
        }
        std::string v = extractTtwid(r.headerValues("Set-Cookie"));
        if (!v.empty()) return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
    error = "TikTok did not hand out a session cookie (ttwid) after 8 tries";
    return {};
}

std::optional<TikTokClient::ConnectInfo> TikTokClient::fetchConnect(const std::string& roomId, std::string& error) {
    std::string server = util::trim(cfg_.signServer);
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty()) {
        error = "no signing server configured";
        return std::nullopt;
    }
    http::Params q = {{"client", "ttlive-node"}, {"room_id", roomId}, {"unique_id", user_}};
    if (!cfg_.apiKey.empty()) q.push_back({"apiKey", cfg_.apiKey});
    http::Headers h;
    if (!cfg_.apiKey.empty()) h.push_back({"x-api-key", cfg_.apiKey});
    auto r = http::get(server + "/webcast/fetch?" + http::buildQuery(q), h, 15);
    if (!r.ok()) {
        error = r.status == 0 ? r.error : "signing server HTTP " + std::to_string(r.status);
        return std::nullopt;
    }
    for (const char* name : {"x-set-tt-cookie", "Set-Cookie"}) {
        std::vector<std::string> lines;
        for (const auto& v : r.headerValues(name)) {
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
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        for (const char* k : {"wsUrl", "ws_url", "websocketUrl", "url"})
            if (j.contains(k) && j[k].is_string()) info.wsUrl = j[k].get<std::string>();
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
    std::string url = buildDirectWsUrl(roomId, 150.0);
    url += "&" + http::buildQuery({{"cursor", fetch->cursor}, {"internal_ext", fetch->internalExt}});
    for (const auto& [k, v] : fetch->routeParams) url += "&" + http::buildQuery({{k, v}});
    info.wsUrl = url;
    return info;
}

void TikTokClient::onFrame(const std::string& data) {
    lastFrameMs_ = util::nowMs();
    auto frame = parsePushFrame(data);
    if (!frame) return;
    // Any answer after im_enter_room proves TikTok let us into the room.
    if (!joined_.exchange(true))
        onStatus_("connected", "Listening to @" + user_ + "'s LIVE (room " + roomId_ + ")");
    if (frame->payloadType != "msg") return; // hb / im_enter_room_resp
    std::string payload = std::move(frame->payload);
    std::string inflated;
    if (gunzip(payload, inflated)) payload = std::move(inflated);
    auto res = parseFetchResult(payload);
    if (!res) return;
    for (const auto& [method, body] : res->messages) {
        if (method == "WebcastChatMessage") {
            if (auto m = parseChatMessage(body)) {
                ++chatCount_;
                onMessage_(std::move(*m));
            }
        } else if (method == "WebcastControlMessage" && isStreamEnd(body)) {
            streamEnded_ = true;
        }
    }
    if (res->needsAck && !res->internalExt.empty()) {
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
    user_ = normalizeUser(cfg_.username);
    if (user_.empty()) {
        onStatus_("error", "Enter the TikTok username in Settings > Connections");
        return;
    }
    const std::string handle = "@" + user_;
    const bool useSigner = !util::trim(cfg_.apiKey).empty() && !util::trim(cfg_.signServer).empty();
    burstUntilMs_ = util::nowMs() + kBurstDurationMs;

    auto waitText = [](int ms) {
        return ms >= 60000 ? std::to_string(ms / 60000) + " min" : std::to_string(ms / 1000) + "s";
    };

    while (running_) {
        // ---- 1. Is the user live? (one light request) -------------------
        onStatus_("connecting", "Checking whether " + handle + " is live...");
        RoomInfo room = resolveRoom();
        if (room.notFound) {
            onStatus_("error", "TikTok account " + handle + " does not exist - check the username");
            if (!sleepFor(600000)) return;
            continue;
        }
        if (!room.found) {
            int wait = std::max(nextIdleDelay(), room.blocked ? 120000 : 0);
            onStatus_("error", "Could not check " + handle + ": " + room.error + " - retrying in " + waitText(wait));
            if (!sleepFor(wait)) return;
            continue;
        }
        if (!room.live) {
            int wait = nextIdleDelay();
            onStatus_("waiting", handle + " is not live. Next check in " + waitText(wait) +
                                     " - press Reconnect (or start streaming in OBS) to check now.");
            if (!sleepFor(wait)) return;
            continue;
        }
        idleDelayMs_ = kIdleFirstMs;
        roomId_ = room.roomId;

        // ---- 2. Connection details (no keys needed) ---------------------
        std::string url, cookie;
        if (useSigner) {
            std::string error;
            auto info = fetchConnect(roomId_, error);
            if (!info) {
                onStatus_("error", "Signing server failed (" + error + ") - using the direct connection instead");
            } else {
                url = info->wsUrl;
                cookie = info->cookies;
            }
        }
        if (url.empty()) {
            if (ttwid_.empty() || needFreshTtwid_.exchange(false)) {
                onStatus_("connecting", handle + " is live - getting a viewer session...");
                std::string error;
                ttwid_ = fetchTtwid(error);
                if (ttwid_.empty()) {
                    onStatus_("error", "Could not start a TikTok viewer session: " + error + " - retrying in 1 min");
                    if (!sleepFor(60000)) return;
                    continue;
                }
            }
            url = buildDirectWsUrl(roomId_, 100.0 + (util::nowMs() % 100000) / 1000.0);
            cookie = "ttwid=" + ttwid_;
        }

        // ---- 3. WebSocket ------------------------------------------------
        joined_ = false;
        streamEnded_ = false;
        chatCount_ = 0;
        lastFrameMs_ = util::nowMs();
        int64_t connectStartMs = util::nowMs();
        WebSocketClient::Callbacks cb;
        cb.onOpen = [this, handle] {
            joined_ = false;
            lastFrameMs_ = util::nowMs();
            std::lock_guard<std::mutex> lock(mutex_);
            if (ws_) {
                ws_->sendBinary(heartbeatFrame(roomId_));
                ws_->sendBinary(enterRoomFrame(roomId_));
            }
            onStatus_("connecting", "Joining " + handle + "'s LIVE (room " + roomId_ + ")...");
        };
        cb.onBinary = [this](const std::string& d) { onFrame(d); };
        cb.onClose = [this](int code, const std::string& reason) {
            joined_ = false;
            if (running_)
                onStatus_("connecting", "TikTok closed the connection (" + std::to_string(code) +
                                            (reason.empty() ? "" : " " + reason) + ") - reconnecting...");
        };
        cb.onError = [this](const std::string& reason) {
            joined_ = false;
            if (reason.find("DEVICE_BLOCKED") != std::string::npos) {
                needFreshTtwid_ = true; // the watchdog rebuilds the connection with a new session
                onStatus_("error", "TikTok flagged the viewer session (DEVICE_BLOCKED) - starting a new one");
            } else {
                onStatus_("error", "TikTok connection failed: " + reason);
            }
        };
        WebSocketClient::Options opt;
        opt.headers = {{"User-Agent", http::browserUserAgent()},
                       {"Cookie", cookie},
                       {"Origin", "https://www.tiktok.com"},
                       {"Referer", "https://www.tiktok.com/"},
                       {"Accept-Language", "en-US,en;q=0.9"},
                       {"Cache-Control", "no-cache"}};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_ = std::make_unique<WebSocketClient>(url, std::move(cb), std::move(opt));
            ws_->start();
        }

        // ---- 4. Heartbeat + watchdog: are we really listening? -----------
        int joinFailures = 0;
        uint64_t reportedChats = 0;
        while (sleepFor(kHeartbeatMs)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ws_) ws_->sendBinary(heartbeatFrame(roomId_));
            }
            if (streamEnded_ || needFreshTtwid_) break;
            const int64_t now = util::nowMs();
            if (joined_) {
                joinFailures = 0;
                connectStartMs = now;
                if (now - lastFrameMs_ > kStaleMs) {
                    LOG_WARN("tiktok", "no data for 60s, reconnecting");
                    onStatus_("connecting", "No data from TikTok for 60s - reconnecting...");
                    joined_ = false;
                    lastFrameMs_ = now;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (ws_) ws_->reconnect();
                } else if (chatCount_ != reportedChats) {
                    reportedChats = chatCount_;
                    onStatus_("connected", "Listening to " + handle + "'s LIVE (room " + roomId_ + ") - " +
                                               std::to_string(reportedChats) + " chat message(s) so far");
                }
            } else if (now - connectStartMs > kJoinTimeoutMs) {
                // Socket up (or retrying) but TikTok never let us in: is the
                // stream still on? If yes, try again with a fresh session.
                connectStartMs = now;
                RoomInfo again = resolveRoom();
                if (again.found && !again.live) {
                    streamEnded_ = true;
                    break;
                }
                if (++joinFailures >= 3) {
                    onStatus_("error", "TikTok did not accept the connection to room " + roomId_ +
                                           " - retrying with a new viewer session");
                    needFreshTtwid_ = true;
                    break;
                }
            }
        }
        std::unique_ptr<WebSocketClient> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old = std::move(ws_);
        }
        if (old) old->stop();
        if (!running_) return;
        if (streamEnded_) {
            idleDelayMs_ = kIdleFirstMs;
            onStatus_("waiting", handle + "'s LIVE ended - checking again in 1 min");
            if (!sleepFor(kIdleFirstMs)) return;
        }
    }
}

} // namespace usc
