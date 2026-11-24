#pragma once
// ChatHub: the in-memory heart of the unified chat. Receives normalized messages from all sources,
// de-duplicates by (platform,id), drops muted users, keeps a ring buffer of the last N messages,
// publishes every accepted message on the EventBus (`chat.message`), and prepares batched JSON
// for the UI. Optional JSONL persistence is OFF unless a log is attached (privacy).
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "chat/types.hpp"
#include "core/event_bus.hpp"

namespace hb::chat {

class IMessageLog {
public:
    virtual ~IMessageLog() = default;
    virtual void append(const ChatMessage& m) = 0;
};

// Appends one JSON object per line. Never throws; write failures are silently counted.
class FileMessageLog : public IMessageLog {
public:
    explicit FileMessageLog(std::string path) : path_(std::move(path)) {}
    void append(const ChatMessage& m) override;
    std::uint64_t failures() const { return failures_; }

private:
    std::string path_;
    std::uint64_t failures_ = 0;
    std::mutex m_;
};

// "platform:authorId" (or lower-cased author name when the platform gives no id).
std::string userKey(const ChatMessage& m);

class ChatHub {
public:
    static constexpr std::size_t kDefaultCapacity = 500;
    ChatHub(hb::core::EventBus* bus, std::size_t capacity = kDefaultCapacity);

    // Thread-safe. Returns how many messages were accepted (not duplicate, not muted).
    int ingest(std::vector<ChatMessage> msgs);

    // Pinning: publishes `overlay.chatPin` {author,text,platform}. Clearing publishes the same
    // topic with empty author/text (documented in docs/CHAT.md). false if seq is unknown.
    bool pin(std::uint64_t seq);
    void clearPin();

    // Local-only moderation (never talks to the platforms).
    bool muteUserOf(std::uint64_t seq);          // mutes the author of message `seq`
    void unmuteUser(const std::string& key);
    void setMuted(const std::vector<std::string>& keys);
    std::vector<std::string> muted() const;

    void setLog(std::unique_ptr<IMessageLog> log);  // nullptr = persistence off (default)
    void clearMessages();

    std::vector<ChatMessage> snapshot() const;       // oldest first
    std::size_t size() const;
    std::uint64_t totalAccepted() const;

    // Next UI batch as {"t":"chat_msgs","seq":N,"msgs":[...],"dropped":D}, at most maxMsgs messages
    // (when more are pending the OLDEST are dropped and counted, so a flood never grows memory or
    // floods the UI thread). Returns "" when nothing is pending. Batch `seq` increases by 1.
    std::string takeBatchJson(std::size_t maxMsgs = 50);
    // Full history for a (re)loaded UI: same shape with "reset":true.
    std::string historyJson(std::size_t maxMsgs = 200);

    static std::string messageJson(const ChatMessage& m);

private:
    hb::core::EventBus* bus_;
    std::size_t capacity_;
    mutable std::mutex m_;
    std::deque<ChatMessage> ring_;
    std::deque<ChatMessage> pending_;
    std::uint64_t dropped_ = 0;
    std::set<std::string> seen_;
    std::deque<std::string> seenOrder_;
    std::set<std::string> muted_;
    std::unique_ptr<IMessageLog> log_;
    std::uint64_t nextSeq_ = 1, batchSeq_ = 0, accepted_ = 0;
};

}  // namespace hb::chat
