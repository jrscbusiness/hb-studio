#pragma once
// ChatService: wires hub + the four sources + settings + secrets + OAuth flow, runs one worker
// thread per platform and a UI flusher, and handles the UI commands. Portable: every platform
// dependency (HTTP, WebSocket, secret store, browser launch) is injected.
//
// UI -> host commands ("name|arg|rest"):
//   chatget                          -> chat_settings/chat_status + history
//   chatsave|<platform>|<json>       save per-platform settings (blank secret keeps the stored one)
//   chatclear|<platform>             forget the stored secret(s) of that platform
//   chatconnect|<platform>, chatdisconnect|<platform>
//   chatauth|youtube, chatauthcancel start/cancel the OAuth loopback flow
//   chatpin|<n>, chatunpin           pin message n on the broadcast / clear the highlight
//   chatmute|<n>, chatunmute|<userKey>, chatclearfeed, chatopt|persist|0/1
// host -> UI: chat_status (keyed, replaces), chat_msgs (batches with sequence numbers, never replaced).
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "chat/hub.hpp"
#include "chat/loopback.hpp"
#include "chat/net.hpp"
#include "chat/oauth.hpp"
#include "chat/settings.hpp"
#include "chat/sources.hpp"

namespace hb::chat {

struct ChatDeps {
    hb::core::EventBus* bus = nullptr;
    IChatHttp* http = nullptr;        // nullptr: networking unavailable (sources report errors)
    IChatSocket* socket = nullptr;
    ISecretStore* secrets = nullptr;  // nullptr: in-memory only (secrets lost on exit)
    std::string settingsPath;         // "" = do not persist
    std::string logPath;              // JSONL, only used while persistMessages is on
    std::function<void(const std::string& key, std::string json)> post;   // host -> UI (thread-safe)
    std::function<void(const std::string& text)> toast;
    std::function<bool(const std::string& url)> openUrl;                  // opens the system browser
    EpochMsFn epochMs = systemEpochMs;
    RandomFn random = defaultRandom();
    int authTimeoutMs = 180000;
    int flushIntervalMs = 250;
};

class ChatService {
public:
    explicit ChatService(ChatDeps deps);
    ~ChatService();
    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    void start();   // spawns the worker threads (idempotent)
    void stop();    // joins them (idempotent; the destructor calls it)

    bool handleCommand(const std::string& msg);   // true if `msg` was a chat command
    void postAll();                               // settings + status + history (on UI "ready")
    int ingestInstagramWebhook(const std::string& body);

    ChatHub& hub() { return hub_; }
    // Synchronous single pass of every source (used by tests instead of threads).
    void pumpOnce(TimePoint now);
    std::string settingsJson() const;   // {"t":"chat_settings",...}  masked hints only
    std::string statusJson() const;     // {"t":"chat_status",...}

private:
    void stepSource(int idx, TimePoint now);
    void runnerLoop(int idx);
    void flusherLoop();
    void setWanted(Platform p, bool want);
    void applySettings();
    void persistSettings();
    void startAuth();
    void authThreadMain();
    std::string secretHint(const char* key) const;
    bool saveSecret(const char* key, const std::string& v);
    void say(const std::string& t) const { if (deps_.toast) deps_.toast(t); }
    IChatSource* source(Platform p);

    ChatDeps deps_;
    MemorySecretStore memSecrets_;
    ISecretStore* secrets_;
    ChatHub hub_;
    std::unique_ptr<IChatHttp> nullHttp_;
    std::unique_ptr<IChatSocket> nullSocket_;
    GoogleTokenManager tokens_;
    YouTubeSource yt_;
    FacebookSource fb_;
    InstagramWebhookSource ig_;
    TwitchSource tw_;

    mutable std::mutex m_;
    ChatSettings settings_;
    std::string authState_ = "idle";      // idle | waiting | working | ok | error
    std::string authMessage_;

    std::atomic<bool> quit_{false}, started_{false};
    std::atomic<bool> wanted_[kPlatformCount];
    bool connected_[kPlatformCount] = {false, false, false, false};  // runner-thread (or pumpOnce) only
    std::vector<std::thread> threads_;
    std::mutex cvm_;
    std::condition_variable cv_;

    std::atomic<bool> authRunning_{false}, authCancel_{false};
    std::thread authThread_;
    std::string lastStatusSent_;
};

}  // namespace hb::chat
