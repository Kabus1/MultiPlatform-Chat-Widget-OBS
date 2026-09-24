#pragma once

#include "core/ChatMessage.hpp"
#include "core/TokenStore.hpp"

#include <functional>
#include <optional>
#include <string>

namespace usc {

using MessageSink = std::function<void(ChatMessage)>;
// state: "disabled" | "disconnected" | "connecting" | "connected" | "waiting" | "error"
using StatusSink = std::function<void(const std::string& state, const std::string& detail)>;
// Returns a usable token, refreshing it first when forceRefresh or near expiry.
using TokenProvider = std::function<std::optional<TokenSet>(bool forceRefresh)>;

class PlatformClient {
public:
    virtual ~PlatformClient() = default;
    virtual void start() = 0;
    virtual void stop() = 0; // must be idempotent and join all threads
};

} // namespace usc
