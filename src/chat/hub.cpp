#include "chat/hub.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "core/json.hpp"
#include "core/sha256.hpp"

namespace hb::chat {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string esc(const std::string& s) { return hb::core::json::escape(s); }
}  // namespace

std::string userKey(const ChatMessage& m) {
    return std::string(platformKey(m.platform)) + ":" + (m.authorId.empty() ? lower(m.author) : m.authorId);
}

std::string ChatHub::messageJson(const ChatMessage& m) {
    std::ostringstream o;
    o << "{\"n\":" << m.seq << ",\"p\":\"" << platformKey(m.platform) << "\",\"a\":\"" << esc(m.author) << "\",\"uk\":\""
      << esc(userKey(m)) << "\",\"x\":\"" << esc(m.text) << "\",\"ts\":" << m.tsMs << ",\"b\":" << m.badges;
    if (!m.avatarUrl.empty()) o << ",\"av\":\"" << esc(m.avatarUrl) << "\"";
    if (looksLikeCommand(m.text)) o << ",\"cmd\":1";
    if (m.platform == Platform::Twitch || m.platform == Platform::YouTube) {
        if (isKnownBot(m.author)) o << ",\"bot\":1";
    }
    o << "}";
    return o.str();
}

void FileMessageLog::append(const ChatMessage& m) {
    std::lock_guard lock(m_);
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    if (!f) { ++failures_; return; }
    f << ChatHub::messageJson(m) << "\n";
    if (!f.good()) ++failures_;
}

ChatHub::ChatHub(hb::core::EventBus* bus, std::size_t capacity)
    : bus_(bus), capacity_(std::clamp<std::size_t>(capacity, 10, 5000)) {}

int ChatHub::ingest(std::vector<ChatMessage> msgs) {
    int accepted = 0;
    std::vector<ChatMessage> published;
    {
        std::lock_guard lock(m_);
        for (auto& m : msgs) {
            if (m.id.empty())  // sources without ids (rare): synthesize a stable one
                m.id = "h" + hb::core::Sha256::hex(m.author + "\x1f" + m.text + "\x1f" + std::to_string(m.tsMs)).substr(0, 16);
            const std::string dedupeKey = std::string(platformKey(m.platform)) + ":" + m.id;
            if (seen_.count(dedupeKey)) continue;
            seen_.insert(dedupeKey);
            seenOrder_.push_back(dedupeKey);
            while (seenOrder_.size() > capacity_ * 8) {
                seen_.erase(seenOrder_.front());
                seenOrder_.pop_front();
            }
            if (muted_.count(userKey(m))) continue;
            m.seq = nextSeq_++;
            ++accepted_;
            ++accepted;
            ring_.push_back(m);
            while (ring_.size() > capacity_) ring_.pop_front();
            pending_.push_back(m);
            while (pending_.size() > capacity_) { pending_.pop_front(); ++dropped_; }
            if (log_) log_->append(m);
            published.push_back(std::move(m));
        }
    }
    // Publish outside the lock: handlers may call back into the hub (e.g. pin).
    if (bus_) {
        for (const auto& m : published) {
            hb::core::Event e;
            e.topic = "chat.message";
            e.fields = {{"platform", platformKey(m.platform)}, {"author", m.author}, {"text", m.text},
                        {"id", m.id}, {"ts", std::to_string(m.tsMs)}};
            bus_->publish(e);
        }
    }
    return accepted;
}

bool ChatHub::pin(std::uint64_t seq) {
    ChatMessage found;
    bool ok = false;
    {
        std::lock_guard lock(m_);
        for (const auto& m : ring_)
            if (m.seq == seq) { found = m; ok = true; break; }
    }
    if (!ok) return false;
    if (bus_) {
        hb::core::Event e;
        e.topic = "overlay.chatPin";
        e.fields = {{"author", found.author}, {"text", found.text}, {"platform", platformKey(found.platform)}};
        bus_->publish(e);
    }
    return true;
}

void ChatHub::clearPin() {
    if (!bus_) return;
    hb::core::Event e;
    e.topic = "overlay.chatPin";
    e.fields = {{"author", ""}, {"text", ""}, {"platform", ""}};
    bus_->publish(e);
}

bool ChatHub::muteUserOf(std::uint64_t seq) {
    std::lock_guard lock(m_);
    for (const auto& m : ring_)
        if (m.seq == seq) {
            if (muted_.size() < 500) muted_.insert(userKey(m));
            return true;
        }
    return false;
}
void ChatHub::unmuteUser(const std::string& key) {
    std::lock_guard lock(m_);
    muted_.erase(key);
}
void ChatHub::setMuted(const std::vector<std::string>& keys) {
    std::lock_guard lock(m_);
    muted_.clear();
    for (const auto& k : keys)
        if (muted_.size() < 500 && !k.empty() && k.size() <= 160) muted_.insert(k);
}
std::vector<std::string> ChatHub::muted() const {
    std::lock_guard lock(m_);
    return {muted_.begin(), muted_.end()};
}

void ChatHub::setLog(std::unique_ptr<IMessageLog> log) {
    std::lock_guard lock(m_);
    log_ = std::move(log);
}

void ChatHub::clearMessages() {
    std::lock_guard lock(m_);
    ring_.clear();
    pending_.clear();
}

std::vector<ChatMessage> ChatHub::snapshot() const {
    std::lock_guard lock(m_);
    return {ring_.begin(), ring_.end()};
}
std::size_t ChatHub::size() const {
    std::lock_guard lock(m_);
    return ring_.size();
}
std::uint64_t ChatHub::totalAccepted() const {
    std::lock_guard lock(m_);
    return accepted_;
}

std::string ChatHub::takeBatchJson(std::size_t maxMsgs) {
    std::vector<ChatMessage> batch;
    std::uint64_t seq, dropped;
    {
        std::lock_guard lock(m_);
        if (pending_.empty() && dropped_ == 0) return "";
        maxMsgs = std::max<std::size_t>(maxMsgs, 1);
        while (pending_.size() > maxMsgs) { pending_.pop_front(); ++dropped_; }
        batch.assign(pending_.begin(), pending_.end());
        pending_.clear();
        dropped = dropped_;
        dropped_ = 0;
        seq = ++batchSeq_;
    }
    std::ostringstream o;
    o << "{\"t\":\"chat_msgs\",\"seq\":" << seq << ",\"dropped\":" << dropped << ",\"msgs\":[";
    for (std::size_t i = 0; i < batch.size(); ++i) o << (i ? "," : "") << messageJson(batch[i]);
    o << "]}";
    return o.str();
}

std::string ChatHub::historyJson(std::size_t maxMsgs) {
    std::vector<ChatMessage> h;
    std::uint64_t seq;
    {
        std::lock_guard lock(m_);
        const std::size_t n = std::min(maxMsgs, ring_.size());
        h.assign(ring_.end() - static_cast<std::ptrdiff_t>(n), ring_.end());
        pending_.clear();   // the history already contains everything pending
        dropped_ = 0;
        seq = ++batchSeq_;
    }
    std::ostringstream o;
    o << "{\"t\":\"chat_msgs\",\"seq\":" << seq << ",\"reset\":true,\"dropped\":0,\"msgs\":[";
    for (std::size_t i = 0; i < h.size(); ++i) o << (i ? "," : "") << messageJson(h[i]);
    o << "]}";
    return o.str();
}

}  // namespace hb::chat
