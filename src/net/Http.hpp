#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace usc::http {

struct Response {
    int status = 0;       // 0 = transport error
    std::string body;
    std::string error;    // transport error description
    std::string contentType;

    bool ok() const { return status >= 200 && status < 300; }
};

using Headers = std::vector<std::pair<std::string, std::string>>;
using Params = std::vector<std::pair<std::string, std::string>>;

// Blocking HTTPS helpers built on cpp-httplib. They honour HTTPS_PROXY and
// SSL_CERT_FILE from the environment so the app works behind corporate proxies.
Response get(const std::string& url, const Headers& headers = {}, int timeoutSec = 10);
Response postForm(const std::string& url, const Params& form, const Headers& headers = {}, int timeoutSec = 10);
Response postJson(const std::string& url, const std::string& json, const Headers& headers = {}, int timeoutSec = 10);

std::string buildQuery(const Params& params);

// Browser-like UA; some endpoints (Kick, Google's free TTS) reject unknown clients.
const char* browserUserAgent();

} // namespace usc::http
