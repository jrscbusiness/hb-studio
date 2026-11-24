#pragma once
// Normalized chat model shared by every platform adapter (YouTube, Facebook, Instagram, Twitch).
// Everything here is portable, pure logic: no network, no clocks, no threads.
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hb::chat {

using TimePoint = std::chrono::steady_clock::time_point;
using Millis = std::chrono::milliseconds;

enum class Platform { YouTube, Facebook, Instagram, Twitch };
inline constexpr int kPlatformCount = 4;

const char* platformKey(Platform p);     // "youtube" | "facebook" | "instagram" | "twitch"
const char* platformLabel(Platform p);   // "YouTube" ...
bool parsePlatform(std::string_view key, Platform& out);

// Bit mask of author roles/badges.
enum Badge : unsigned { kBadgeMod = 1, kBadgeOwner = 2, kBadgeMember = 4, kBadgeSub = 8, kBadgeVerified = 16 };

struct EmoteRange {          // Twitch IRC `emotes` tag; offsets are in Unicode code points of `text`
    std::string id;
    int start = 0, end = 0;  // inclusive
};

struct ChatMessage {
    Platform platform = Platform::Twitch;
    std::string id;          // platform message id (may be empty for sources that have none; hub synthesizes one)
    std::string authorId;    // platform user id (may be empty)
    std::string author;      // display name
    std::string avatarUrl;   // optional; never downloaded by this module
    std::string text;
    std::int64_t tsMs = 0;   // Unix epoch milliseconds (platform time when known, else receive time)
    unsigned badges = 0;
    std::vector<EmoteRange> emotes;
    std::uint64_t seq = 0;   // assigned by ChatHub; 0 = not yet ingested
};

enum class SourceState {
    Disconnected,  // not started (or stopped by the operator)
    Connecting,
    Connected,
    Backoff,       // transient error, retrying with exponential backoff
    NeedsAuth,     // missing/expired credentials: the operator must (re)authorize
    Failed,        // permanent error until reconfigured/reconnected (bad id, chat disabled, ...)
    Ended,         // the live broadcast/chat ended
    Unavailable,   // platform offers no way to read this (Instagram): explanatory, not an error
};
const char* sourceStateName(SourceState s);

struct SourceStatus {
    SourceState state = SourceState::Disconnected;
    std::string detail;       // short pt-BR explanation, safe to show (never contains secrets)
    std::string lastError;    // last error text, redacted
    std::int64_t quotaUnits = 0;  // API quota units spent this session (YouTube only), else 0
};

// Cleans text from an external source: drops control characters (newline/tab become a space),
// replaces invalid UTF-8 bytes with '?', truncates to maxBytes on a code point boundary.
std::string sanitizeText(std::string_view in, std::size_t maxBytes);

// "!command" messages (what the UI's "hide commands" filter hides).
bool looksLikeCommand(std::string_view text);
// Well-known chat bots (case-insensitive).
bool isKnownBot(std::string_view author);

// Last 4 characters behind a bullet run; "" for an empty secret. Used for masked hints only.
std::string maskTail(const std::string& secret);

// ISO-8601 / RFC-3339 ("2024-05-01T12:34:56.789Z", "+0000"/"+00:00" offsets) -> epoch ms. false if invalid.
bool parseIso8601Ms(std::string_view s, std::int64_t& out);

}  // namespace hb::chat
