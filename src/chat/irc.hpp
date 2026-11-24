#pragma once
// Pure IRC (RFC 1459 + IRCv3 message tags) line parser and the Twitch PRIVMSG -> ChatMessage mapper.
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chat/types.hpp"

namespace hb::chat {

struct IrcMessage {
    std::map<std::string, std::string> tags;  // values already unescaped
    std::string prefix;                       // "nick!user@host" or server name, without ':'
    std::string command;                      // upper-cased as received ("PRIVMSG", "001", ...)
    std::vector<std::string> params;          // trailing parameter (after " :") is the last element
};

inline constexpr std::size_t kMaxIrcLine = 8192;

// Parses ONE line (without CRLF). false for empty/oversized/malformed lines. Never throws.
bool parseIrcLine(std::string_view line, IrcMessage& out);

// Splits a WebSocket frame/TCP chunk into lines (CRLF or LF), dropping empty lines.
std::vector<std::string> splitIrcLines(std::string_view chunk);

// IRCv3 tag value unescaping (\: \s \\ \r \n).
std::string unescapeIrcTag(std::string_view v);

// "25:0-4,12-16/1902:6-10" -> ranges; malformed pieces are skipped.
std::vector<EmoteRange> parseTwitchEmotes(std::string_view tag);

// "broadcaster/1,moderator/1,subscriber/12" -> Badge bits.
unsigned parseTwitchBadges(std::string_view tag);

// Maps a Twitch PRIVMSG to the normalized model. nullopt if `m` is not a usable PRIVMSG.
// `receivedMs` is used when the `tmi-sent-ts` tag is missing. "/me" (ACTION) is unwrapped.
std::optional<ChatMessage> twitchToChatMessage(const IrcMessage& m, std::int64_t receivedMs);

}  // namespace hb::chat
