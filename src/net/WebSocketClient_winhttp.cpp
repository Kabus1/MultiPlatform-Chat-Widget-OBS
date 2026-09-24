// Windows backend: WinHTTP's built-in WebSocket support (Windows 8+).
// TLS, certificates and system proxy settings come from Windows itself.
#include "net/WebSocketClient.hpp"

#include "net/WinHttpCommon.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace usc {

struct WebSocketClient::Impl {
    std::string url;
    Callbacks cb;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> dropRequested{false};
    std::mutex mutex; // guards socket
    HINTERNET socket = nullptr;
    std::mutex sleepMutex;
    std::condition_variable sleepCv;

    void run();
    HINTERNET connect(std::string& error);
    void readLoop(HINTERNET ws);
    void closeSocket();
};

HINTERNET WebSocketClient::Impl::connect(std::string& error) {
    winhttp::ParsedUrl u;
    if (!winhttp::parseUrl(url, u)) {
        error = "invalid URL";
        return nullptr;
    }
    winhttp::Handle conn(WinHttpConnect(winhttp::session(), u.host.c_str(), u.port, 0));
    if (!conn) {
        error = winhttp::lastError("connect");
        return nullptr;
    }
    winhttp::Handle req(WinHttpOpenRequest(conn.get(), L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        error = winhttp::lastError("open request");
        return nullptr;
    }
    if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        error = winhttp::lastError("handshake");
        return nullptr;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        error = "handshake rejected (HTTP " + std::to_string(status) + ")";
        return nullptr;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req.get(), 0);
    if (!ws) error = winhttp::lastError("upgrade");
    return ws; // the request handle may be closed once upgraded
}

void WebSocketClient::Impl::readLoop(HINTERNET ws) {
    std::vector<char> buf(64 * 1024);
    std::string message;
    while (running && !dropRequested) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &read, &type);
        if (err != ERROR_SUCCESS) {
            if (running && !dropRequested && cb.onError) cb.onError("receive failed (" + std::to_string(err) + ")");
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return;
        message.append(buf.data(), read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (cb.onMessage) cb.onMessage(message);
            message.clear();
        }
        // *_FRAGMENT_BUFFER_TYPE: keep accumulating
    }
}

void WebSocketClient::Impl::closeSocket() {
    std::lock_guard<std::mutex> lock(mutex);
    if (socket) {
        // Closing the handle also unblocks a pending WinHttpWebSocketReceive.
        WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket);
        socket = nullptr;
    }
}

void WebSocketClient::Impl::run() {
    int backoffMs = 1000;
    while (running) {
        std::string error;
        HINTERNET ws = connect(error);
        if (ws) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                socket = ws;
            }
            backoffMs = 1000;
            dropRequested = false;
            if (cb.onOpen) cb.onOpen();
            readLoop(ws);
            closeSocket();
            if (!running) return;
            if (cb.onClose) cb.onClose();
        } else if (cb.onError) {
            cb.onError(error);
        }
        std::unique_lock<std::mutex> lock(sleepMutex);
        sleepCv.wait_for(lock, std::chrono::milliseconds(backoffMs), [&] { return !running.load(); });
        backoffMs = std::min(backoffMs * 2, 30000);
    }
}

WebSocketClient::WebSocketClient(std::string url, Callbacks callbacks) : impl_(std::make_unique<Impl>()) {
    impl_->url = std::move(url);
    impl_->cb = std::move(callbacks);
}

WebSocketClient::~WebSocketClient() { stop(); }

void WebSocketClient::start() {
    if (impl_->running.exchange(true)) return;
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

void WebSocketClient::stop() {
    if (!impl_->running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->sleepMutex);
    }
    impl_->sleepCv.notify_all();
    impl_->closeSocket();
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool WebSocketClient::send(const std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->socket) return false;
    return WinHttpWebSocketSend(impl_->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<char*>(text.data()), static_cast<DWORD>(text.size())) == ERROR_SUCCESS;
}

void WebSocketClient::reconnect() {
    // May be called from inside onMessage (the reader thread): only signal and
    // shut the socket down; the reader loop then exits and reconnects.
    impl_->dropRequested = true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->socket) WinHttpWebSocketShutdown(impl_->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}

} // namespace usc
