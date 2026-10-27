// Linux / macOS backend: IXWebSocket (TLS through OpenSSL).
#include "net/WebSocketClient.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>

namespace usc {

struct WebSocketClient::Impl {
    Callbacks cb;
    Options opt;
    ix::WebSocket ws;
    std::atomic<int> nextDelayMs{0};

    void applyDelay() {
        int d = nextDelayMs.exchange(0);
        if (d > 0) {
            ws.setMinWaitBetweenReconnectionRetries(static_cast<uint32_t>(d));
            ws.setMaxWaitBetweenReconnectionRetries(static_cast<uint32_t>(std::max(d, 30000)));
        }
    }
};

WebSocketClient::WebSocketClient(std::string url, Callbacks callbacks, Options options)
    : impl_(std::make_unique<Impl>()) {
    ix::initNetSystem();
    impl_->cb = std::move(callbacks);
    impl_->opt = std::move(options);
    if (impl_->opt.urlProvider) {
        std::string fresh = impl_->opt.urlProvider();
        if (!fresh.empty()) url = fresh;
    }
    impl_->ws.setUrl(url);
    ix::WebSocketHttpHeaders headers;
    for (const auto& [k, v] : impl_->opt.headers) headers[k] = v;
    if (!headers.empty()) impl_->ws.setExtraHeaders(headers);
    impl_->ws.enableAutomaticReconnection();
    impl_->ws.setMinWaitBetweenReconnectionRetries(1000);
    impl_->ws.setMaxWaitBetweenReconnectionRetries(30000);
    impl_->ws.setPingInterval(60);
    Impl* impl = impl_.get();
    impl_->ws.setOnMessageCallback([impl](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            // Restore normal backoff after a deliberate long pause.
            impl->ws.setMinWaitBetweenReconnectionRetries(1000);
            impl->ws.setMaxWaitBetweenReconnectionRetries(30000);
            if (impl->cb.onOpen) impl->cb.onOpen();
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary && impl->cb.onBinary) impl->cb.onBinary(msg->str);
            else if (impl->cb.onMessage) impl->cb.onMessage(msg->str);
            break;
        case ix::WebSocketMessageType::Close:
            if (impl->cb.onClose) impl->cb.onClose(msg->closeInfo.code, msg->closeInfo.reason);
            impl->applyDelay();
            if (impl->opt.urlProvider) {
                std::string fresh = impl->opt.urlProvider();
                if (!fresh.empty()) impl->ws.setUrl(fresh);
            }
            break;
        case ix::WebSocketMessageType::Error:
            if (impl->cb.onError) impl->cb.onError(msg->errorInfo.reason);
            impl->applyDelay();
            break;
        default: break;
        }
    });
}

WebSocketClient::~WebSocketClient() { stop(); }

void WebSocketClient::start() { impl_->ws.start(); }

void WebSocketClient::stop() { impl_->ws.stop(); }

bool WebSocketClient::send(const std::string& text) { return impl_->ws.sendText(text).success; }

bool WebSocketClient::sendBinary(const std::string& data) { return impl_->ws.sendBinary(data).success; }

void WebSocketClient::reconnect() { impl_->ws.close(); } // automatic reconnection kicks in

void WebSocketClient::setNextRetryDelay(int ms) { impl_->nextDelayMs = ms; }

} // namespace usc
