#pragma once
// Per-platform chat settings persisted as JSON (chat.json). NEVER contains secrets: tokens and the
// OAuth client secret live in the ISecretStore; only masked hints are ever shown.
#include <string>
#include <vector>

namespace hb::chat {

struct ChatSettings {
    std::string youtubeVideoId;
    std::string youtubeClientId;   // a Google OAuth client id is not secret (it is visible in the auth URL)
    std::string facebookLiveVideoId;
    std::string twitchChannel;     // lower-case login
    bool persistMessages = false;  // OFF by default (privacy): appends messages to chat_log.jsonl
    std::vector<std::string> muted;

    std::string toJson() const;
    // false if not valid JSON; unknown/oversized fields are ignored/clamped.
    static bool fromJson(const std::string& json, ChatSettings& out);
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// Input validation for the UI's save form. Return the cleaned value, or "" if invalid.
std::string cleanVideoId(const std::string& in);      // accepts a bare id or a youtube.com/watch?v= / youtu.be / live URL
std::string cleanGraphId(const std::string& in);      // digits and '_' (Facebook ids like 1234_5678)
std::string cleanTwitchChannel(const std::string& in);// strips '#', lower-cases, [a-z0-9_]{1,25}
std::string cleanClientId(const std::string& in);     // ".apps.googleusercontent.com" style, no spaces

}  // namespace hb::chat
