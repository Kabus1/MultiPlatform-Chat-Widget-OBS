#include "net/Http.hpp"

#include "core/Util.hpp"

#include <httplib.h>

#include <cstdlib>
#include <memory>

namespace usc::http {

namespace {

struct UrlParts {
    std::string origin; // scheme://host[:port]
    std::string path;   // /path?query
};

UrlParts splitUrl(const std::string& url) {
    auto schemeEnd = url.find("://");
    size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    auto pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) return {url, "/"};
    return {url.substr(0, pathStart), url.substr(pathStart)};
}

const char* envAny(std::initializer_list<const char*> names) {
    for (auto n : names)
        if (const char* v = std::getenv(n); v && *v) return v;
    return nullptr;
}

std::unique_ptr<httplib::Client> makeClient(const std::string& origin, int timeoutSec) {
    auto cli = std::make_unique<httplib::Client>(origin);
    cli->set_connection_timeout(timeoutSec, 0);
    cli->set_read_timeout(timeoutSec, 0);
    cli->set_write_timeout(timeoutSec, 0);
    cli->set_follow_location(true);
    cli->enable_server_certificate_verification(true);
    if (const char* ca = envAny({"SSL_CERT_FILE"})) cli->set_ca_cert_path(ca);

    if (const char* proxy = envAny({"HTTPS_PROXY", "https_proxy"})) {
        // Accepts http://[user:pass@]host:port
        std::string p = proxy;
        if (auto s = p.find("://"); s != std::string::npos) p = p.substr(s + 3);
        if (auto slash = p.find('/'); slash != std::string::npos) p = p.substr(0, slash);
        std::string user, pass;
        if (auto at = p.rfind('@'); at != std::string::npos) {
            std::string cred = p.substr(0, at);
            p = p.substr(at + 1);
            auto colon = cred.find(':');
            user = util::urlDecode(cred.substr(0, colon));
            if (colon != std::string::npos) pass = util::urlDecode(cred.substr(colon + 1));
        }
        auto colon = p.rfind(':');
        std::string host = p.substr(0, colon);
        int port = colon == std::string::npos ? 80 : std::atoi(p.c_str() + colon + 1);
        cli->set_proxy(host, port);
        if (!user.empty()) cli->set_proxy_basic_auth(user, pass);
    }
    return cli;
}

httplib::Headers toHeaders(const Headers& h) {
    httplib::Headers out;
    bool hasUa = false;
    for (const auto& [k, v] : h) {
        out.emplace(k, v);
        if (util::toLower(k) == "user-agent") hasUa = true;
    }
    if (!hasUa) out.emplace("User-Agent", browserUserAgent());
    return out;
}

Response convert(const httplib::Result& r) {
    Response out;
    if (!r) {
        out.error = httplib::to_string(r.error());
        return out;
    }
    out.status = r->status;
    out.body = r->body;
    out.contentType = r->get_header_value("Content-Type");
    return out;
}

} // namespace

const char* browserUserAgent() {
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/128.0.0.0 Safari/537.36";
}

std::string buildQuery(const Params& params) {
    std::string q;
    for (const auto& [k, v] : params) {
        if (!q.empty()) q += '&';
        q += util::urlEncode(k) + '=' + util::urlEncode(v);
    }
    return q;
}

Response get(const std::string& url, const Headers& headers, int timeoutSec) {
    auto parts = splitUrl(url);
    auto cli = makeClient(parts.origin, timeoutSec);
    return convert(cli->Get(parts.path, toHeaders(headers)));
}

Response postForm(const std::string& url, const Params& form, const Headers& headers, int timeoutSec) {
    auto parts = splitUrl(url);
    auto cli = makeClient(parts.origin, timeoutSec);
    return convert(cli->Post(parts.path, toHeaders(headers), buildQuery(form), "application/x-www-form-urlencoded"));
}

Response postJson(const std::string& url, const std::string& json, const Headers& headers, int timeoutSec) {
    auto parts = splitUrl(url);
    auto cli = makeClient(parts.origin, timeoutSec);
    return convert(cli->Post(parts.path, toHeaders(headers), json, "application/json"));
}

} // namespace usc::http
