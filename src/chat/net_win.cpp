// Windows implementations of the chat transports: WinHTTP (HTTPS client + text WebSocket) and a
// DPAPI secret store (now in src/net). NOT VALIDATED: written without access to a Windows compiler; the
// Windows CI build and a manual run against the real services are the verification (docs/CHAT.md).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <vector>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "chat/net.hpp"
#include "chat/net_adapter.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hb::chat {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) ::WinHttpCloseHandle(h); }
};

std::string winErr(const char* what) {
    return std::string(what) + " failed (WinHTTP error " + std::to_string(::GetLastError()) + ")";
}

struct ParsedUrl {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool crack(const std::string& url, ParsedUrl& out) {
    const std::wstring w = widen(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return false;
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return uc.nScheme == INTERNET_SCHEME_HTTPS || uc.nScheme == INTERNET_SCHEME_HTTP;
}

// Text WebSocket over WinHTTP (Windows 8+). A reader thread fills a queue so receive() can poll.
class WinChatSocket : public IChatSocket {
public:
    ~WinChatSocket() override { close(); }

    bool connect(const std::string& url, std::string& error) override {
        close();
        ParsedUrl u;
        if (!crack(url, u)) { error = "invalid URL"; return false; }
        session_ = ::WinHttpOpen(L"HBStudio-Chat/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) { error = winErr("WinHttpOpen"); return false; }
        ::WinHttpSetTimeouts(session_, 10000, 10000, 10000, 0);  // receive timeout 0 = wait forever (reader thread)
        conn_ = ::WinHttpConnect(session_, u.host.c_str(), u.port, 0);
        if (!conn_) { error = winErr("WinHttpConnect"); close(); return false; }
        HINTERNET req = ::WinHttpOpenRequest(conn_, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { error = winErr("WinHttpOpenRequest"); close(); return false; }
        if (!::WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(req, nullptr)) {
            error = winErr("websocket handshake");
            ::WinHttpCloseHandle(req);
            close();
            return false;
        }
        ws_ = ::WinHttpWebSocketCompleteUpgrade(req, 0);
        ::WinHttpCloseHandle(req);
        if (!ws_) { error = winErr("WinHttpWebSocketCompleteUpgrade"); close(); return false; }
        closed_ = false;
        stop_ = false;
        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    bool sendText(const std::string& text) override {
        std::lock_guard lock(sendM_);
        if (!ws_ || closed_) return false;
        return ::WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                      const_cast<char*>(text.data()), static_cast<DWORD>(text.size())) == ERROR_SUCCESS;
    }

    Recv receive(std::string& out, int timeoutMs) override {
        std::unique_lock lock(qm_);
        if (queue_.empty() && !closed_ && timeoutMs > 0)
            qcv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !queue_.empty() || closed_; });
        if (!queue_.empty()) {
            out = std::move(queue_.front());
            queue_.pop_front();
            return Recv::Message;
        }
        return closed_ ? Recv::Closed : Recv::Timeout;
    }

    void close() override {
        stop_ = true;
        {
            std::lock_guard lock(sendM_);
            // Closing the handles aborts a receive pending in the reader thread.
            if (ws_) { ::WinHttpCloseHandle(ws_); ws_ = nullptr; }
        }
        if (conn_) { ::WinHttpCloseHandle(conn_); conn_ = nullptr; }
        if (session_) { ::WinHttpCloseHandle(session_); session_ = nullptr; }
        if (reader_.joinable()) reader_.join();
        std::lock_guard lock(qm_);
        queue_.clear();
        closed_ = true;
    }

private:
    void readLoop() {
        HINTERNET ws = ws_;  // snapshot: close() nulls the member but only after this thread's call is aborted
        std::string assembling;
        std::vector<char> buf(8192);
        while (!stop_ && ws) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            const DWORD rc = ::WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type);
            if (rc != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                continue;  // IRC is text only
            assembling.append(buf.data(), got);
            if (assembling.size() > 1024 * 1024) break;  // hostile peer
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                std::lock_guard lock(qm_);
                if (queue_.size() < 5000) queue_.push_back(std::move(assembling));
                assembling.clear();
                qcv_.notify_all();
            }
        }
        std::lock_guard lock(qm_);
        closed_ = true;
        qcv_.notify_all();
    }

    HINTERNET session_ = nullptr, conn_ = nullptr, ws_ = nullptr;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    bool closed_ = true;   // guarded by qm_
    std::mutex qm_, sendM_;
    std::condition_variable qcv_;
    std::deque<std::string> queue_;
};

}  // namespace

std::unique_ptr<IChatHttp> makeDefaultChatHttp() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-Chat/1.0";
    return makeChatHttpOverNet(hb::net::makeWinHttpClient(o));
}
std::unique_ptr<IChatSocket> makeDefaultChatSocket() { return std::make_unique<WinChatSocket>(); }
std::unique_ptr<ISecretStore> makeDpapiSecretStore(const std::string& dir) {
    // No entropy, as before: existing chat_secrets files keep decrypting.
    std::shared_ptr<hb::net::ISecretStore> dpapi = hb::net::makeDpapiSecretStore({"", "HBStudio chat"});
    return makeFileSecretStore(dir, std::move(dpapi));
}

}  // namespace hb::chat

#endif  // _WIN32
