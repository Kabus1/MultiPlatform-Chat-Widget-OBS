// Linux / macOS backend: IXWebSocket (TLS through OpenSSL).
#include "net/WebSocketClient.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace usc {

struct WebSocketClient::Impl {
    Callbacks cb;
    ix::WebSocket ws;
};

WebSocketClient::WebSocketClient(std::string url, Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    ix::initNetSystem();
    impl_->cb = std::move(callbacks);
    impl_->ws.setUrl(url);
    impl_->ws.enableAutomaticReconnection();
    impl_->ws.setMinWaitBetweenReconnectionRetries(1000);
    impl_->ws.setMaxWaitBetweenReconnectionRetries(30000);
    impl_->ws.setPingInterval(60);
    Impl* impl = impl_.get();
    impl_->ws.setOnMessageCallback([impl](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            if (impl->cb.onOpen) impl->cb.onOpen();
            break;
        case ix::WebSocketMessageType::Message:
            if (impl->cb.onMessage) impl->cb.onMessage(msg->str);
            break;
        case ix::WebSocketMessageType::Close:
            if (impl->cb.onClose) impl->cb.onClose();
            break;
        case ix::WebSocketMessageType::Error:
            if (impl->cb.onError) impl->cb.onError(msg->errorInfo.reason);
            break;
        default: break;
        }
    });
}

WebSocketClient::~WebSocketClient() { stop(); }

void WebSocketClient::start() { impl_->ws.start(); }

void WebSocketClient::stop() { impl_->ws.stop(); }

bool WebSocketClient::send(const std::string& text) { return impl_->ws.sendText(text).success; }

void WebSocketClient::reconnect() { impl_->ws.close(); } // automatic reconnection kicks in

} // namespace usc
