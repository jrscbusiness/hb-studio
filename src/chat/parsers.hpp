#pragma once
// Pure JSON -> normalized message parsers for the HTTP based platforms. No network, no clocks.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chat/types.hpp"

namespace hb::chat {

// ---- YouTube Live Streaming API ----
struct YouTubeChatPage {
    std::vector<ChatMessage> messages;  // oldest first, only displayable events
    std::string nextPageToken;
    std::int64_t pollingIntervalMillis = 0;  // 0 if absent
    bool offline = false;                    // `offlineAt` present: the broadcast ended
    int skippedEvents = 0;                   // deleted/banned/poll/... events we do not display
};
// false if `json` is not a liveChatMessageListResponse-shaped object.
bool parseYouTubeChatPage(std::string_view json, std::int64_t receivedMs, YouTubeChatPage& out);

// videos.list?part=liveStreamingDetails -> activeLiveChatId ("" when the video has none/not live).
bool parseYouTubeActiveChatId(std::string_view json, std::string& chatId);

// Google API error body: {"error":{"code":403,"message":"..","errors":[{"reason":"liveChatEnded"}]}}
struct ApiError {
    int code = 0;
    std::string reason;   // first errors[].reason (YouTube) or ""
    std::string message;
};
bool parseGoogleError(std::string_view json, ApiError& out);

// ---- Meta Graph API ----
struct GraphCommentsPage {
    std::vector<ChatMessage> messages;  // as returned (the source reorders to oldest-first)
};
// Facebook live video `comments` edge. Handles a missing `from` (Facebook hides commenters in some
// cases): author becomes "Usuário do Facebook".
bool parseFacebookComments(std::string_view json, std::int64_t receivedMs, GraphCommentsPage& out);

// Graph API error: {"error":{"message":"..","type":"OAuthException","code":190,"error_subcode":463}}
struct GraphError {
    int code = 0;
    int subcode = 0;
    std::string message;
};
bool parseGraphError(std::string_view json, GraphError& out);

// Instagram `live_comments` webhook payload (NOT validated against a live Meta webhook, see
// docs/CHAT.md): {"object":"instagram","entry":[{"id":..,"changes":[{"field":"live_comments","value":{...}}]}]}
bool parseInstagramLiveWebhook(std::string_view json, std::int64_t receivedMs, std::vector<ChatMessage>& out);

}  // namespace hb::chat
