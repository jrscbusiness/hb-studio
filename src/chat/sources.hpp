#pragma once
// Platform adapters behind IChatSource. Each adapter is poll-driven with an injectable clock: the
// caller passes `now` to poll(); an adapter whose next poll is not due returns an empty vector
// immediately. connect()/disconnect()/poll() are called from ONE thread (the runner);
// status() and configure() are safe from any thread.
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "chat/net.hpp"
#include "chat/oauth.hpp"
#include "chat/types.hpp"

namespace hb::chat {

class IChatSource {
public:
    virtual ~IChatSource() = default;
    virtual Platform platform() const = 0;
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual std::vector<ChatMessage> poll(TimePoint now) = 0;
    virtual SourceStatus status() const = 0;
};

using EpochMsFn = std::function<std::int64_t()>;
std::int64_t systemEpochMs();

// Remembers the last N ids (bounded), for backlog skipping and per-poll dedupe inside a source.
class RecentIds {
public:
    explicit RecentIds(std::size_t cap = 2000) : cap_(cap) {}
    bool insert(const std::string& id);  // true if it was new
    void clear() { set_.clear(); order_.clear(); }

private:
    std::size_t cap_;
    std::set<std::string> set_;
    std::deque<std::string> order_;
};

// Status plumbing shared by the adapters.
class SourceBase : public IChatSource {
public:
    SourceStatus status() const override;

protected:
    void setState(SourceState s, std::string detail, std::string error = {});
    void addQuota(int units);
    mutable std::mutex sm_;
    SourceStatus st_;
};

// ---------------------------------------------------------------- YouTube
struct YouTubeConfig {
    std::string videoId;     // watch?v=<id>: the live broadcast's video id
    std::string liveChatId;  // optional override (skips the videos.list lookup)
    bool skipBacklog = true; // drop the history returned by the first page (see docs/CHAT.md)
};

class YouTubeSource : public SourceBase {
public:
    YouTubeSource(IChatHttp& http, IAccessTokenProvider& tokens, EpochMsFn epochMs = systemEpochMs)
        : http_(http), tokens_(tokens), epochMs_(std::move(epochMs)) {}
    void configure(YouTubeConfig cfg);
    Platform platform() const override { return Platform::YouTube; }
    void connect() override;
    void disconnect() override;
    std::vector<ChatMessage> poll(TimePoint now) override;

    // Quota costs (YouTube Data API v3 quota table).
    static constexpr int kListCost = 5;    // liveChatMessages.list
    static constexpr int kVideosCost = 1;  // videos.list

private:
    void applyPending();
    void fail(TimePoint now, const std::string& what, bool permanent, SourceState state = SourceState::Backoff);
    IChatHttp& http_;
    IAccessTokenProvider& tokens_;
    EpochMsFn epochMs_;
    std::mutex cfgM_;
    YouTubeConfig pending_;
    bool dirty_ = true;
    // poll-thread state
    YouTubeConfig cfg_;
    bool wanted_ = false;
    TimePoint nextPollAt_{};
    std::string chatId_, pageToken_;
    bool firstPage_ = true;
    Backoff backoff_{Millis(3000), Millis(120000)};
    RecentIds seen_;
};

// ---------------------------------------------------------------- Facebook
struct FacebookConfig {
    std::string liveVideoId;
    bool skipBacklog = true;
    int pollIntervalMs = 5000;
};

class FacebookSource : public SourceBase {
public:
    // tokenFn is called at poll time (secrets are not copied into the adapter).
    FacebookSource(IChatHttp& http, std::function<std::string()> tokenFn, EpochMsFn epochMs = systemEpochMs)
        : http_(http), tokenFn_(std::move(tokenFn)), epochMs_(std::move(epochMs)) {}
    void configure(FacebookConfig cfg);
    Platform platform() const override { return Platform::Facebook; }
    void connect() override;
    void disconnect() override;
    std::vector<ChatMessage> poll(TimePoint now) override;

private:
    void applyPending();
    IChatHttp& http_;
    std::function<std::string()> tokenFn_;
    EpochMsFn epochMs_;
    std::mutex cfgM_;
    FacebookConfig pending_;
    bool dirty_ = true;
    FacebookConfig cfg_;
    bool wanted_ = false;
    TimePoint nextPollAt_{};
    bool firstPage_ = true;
    Backoff backoff_{Millis(5000), Millis(300000)};
    RecentIds seen_;
};

// ---------------------------------------------------------------- Instagram
// Instagram offers NO polling API for live comments. The only supported channel is the
// `live_comments` webhook (needs a public HTTPS endpoint + app review), which a desktop app cannot
// host on its own. This adapter is therefore permanently "Unavailable" in the UI; it only turns
// webhook bodies handed to feedWebhook() (e.g. by a relay the operator runs) into messages.
class InstagramWebhookSource : public SourceBase {
public:
    explicit InstagramWebhookSource(EpochMsFn epochMs = systemEpochMs) : epochMs_(std::move(epochMs)) { refreshStatus(); }
    Platform platform() const override { return Platform::Instagram; }
    void connect() override { refreshStatus(); }
    void disconnect() override { refreshStatus(); }
    std::vector<ChatMessage> poll(TimePoint now) override;
    // Thread-safe. Returns the number of messages accepted (0 for invalid payloads).
    int feedWebhook(const std::string& body);

private:
    void refreshStatus();
    EpochMsFn epochMs_;
    std::mutex qm_;
    std::vector<ChatMessage> queue_;
    std::size_t received_ = 0;
};

// ---------------------------------------------------------------- Twitch
struct TwitchConfig {
    std::string channel;  // login name, with or without '#'
};

// Anonymous read-only IRC over secure WebSocket (justinfan nick, no token needed).
class TwitchSource : public SourceBase {
public:
    TwitchSource(IChatSocket& socket, EpochMsFn epochMs = systemEpochMs) : sock_(socket), epochMs_(std::move(epochMs)) {}
    void configure(TwitchConfig cfg);
    Platform platform() const override { return Platform::Twitch; }
    void connect() override;
    void disconnect() override;
    std::vector<ChatMessage> poll(TimePoint now) override;

    static bool validChannel(const std::string& c);  // 1..25 chars [a-z0-9_]
    static constexpr const char* kUrl = "wss://irc-ws.chat.twitch.tv:443";

private:
    void applyPending();
    void drop(TimePoint now, const std::string& why);
    IChatSocket& sock_;
    EpochMsFn epochMs_;
    std::mutex cfgM_;
    TwitchConfig pending_;
    bool dirty_ = true;
    TwitchConfig cfg_;
    std::string channel_;
    bool wanted_ = false, open_ = false;
    TimePoint nextAttemptAt_{}, lastActivity_{};
    Backoff backoff_{Millis(2000), Millis(60000)};
};

}  // namespace hb::chat
