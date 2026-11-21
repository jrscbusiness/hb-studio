#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace hb::app {

// One outbound RTMP(S)/SRT destination for the TRANSMISSÃO tab. There is no OAuth login with any
// platform here - "integration" with YouTube/Facebook/Twitch/etc. means a preset that pre-fills the
// well-known ingest server URL for that platform; the user still pastes their OWN stream key from
// their own account there, exactly like OBS Studio's built-in "services" list. Platforms that only
// offer live ingestion through a vetted-partner API (e.g. Instagram) cannot be reached this way and
// are intentionally not offered as presets; "Custom RTMP/RTMPS/SRT" always covers any server that
// does accept a stream key.
struct StreamDestination {
    std::string id;      // stable, assigned once by the store; empty only for a not-yet-added entry
    std::string name;
    // A preset name selects the server URL template shown to the user, nothing more (see the big
    // comment above) - "custom" means the user typed their own serverUrl.
    std::string platform = "custom";
    std::string protocol = "rtmp";  // rtmp | rtmps | srt
    std::string serverUrl;
    std::string streamKey;          // never serialised back to the UI in full, see uiJson()/maskKey()
    bool enabled = true;            // included in "start all"; a disabled destination is skipped
    std::string encoder = "auto";   // auto | x264 | nvenc | qsv | amf (passed through to the encoder)
    int videoKbps = 6000;
    int audioKbps = 160;
    // A destination normally shares the program's own resolution/fps; set useOwnFormat to override
    // with width/height/fpsNum/fpsDen (e.g. a platform capped at 1080p30 while others run 4K60).
    bool useOwnFormat = false;
    int width = 1920, height = 1080;
    int fpsNum = 30, fpsDen = 1;

    bool empty() const { return id.empty(); }
};

// A short, non-reversible hint for a secret: "" if empty, else the last 4 characters prefixed with
// a bullet run, e.g. "••••7f2a" - enough for the user to tell two keys apart without ever re-showing
// a stored key in full (requirement: never display a complete key after it has been saved).
std::string maskSecret(const std::string& secret);

// Up to kMax destinations, persisted as JSON (keys included - the file itself must be protected by
// normal OS/file permissions in the per-user app-data folder, same trust boundary as every other
// settings file this app writes; nothing here claims OS-independent secret storage).
class StreamDestinations {
public:
    static constexpr int kMax = 16;

    std::vector<StreamDestination> list() const;
    StreamDestination get(const std::string& id) const;   // empty() if not found
    // Adds (id empty) or updates (id matches an existing entry) a destination; returns the id, or
    // "" if full (adding) / not found (updating) / rejected (empty name/serverUrl).
    std::string upsert(StreamDestination d);
    bool remove(const std::string& id);
    bool setEnabled(const std::string& id, bool enabled);

    unsigned version() const;

    std::string toJson() const;
    bool fromJson(const std::string& json);
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // {"t":"xmit_dest","list":[...]}: every field except streamKey, which becomes "keySet" (bool)
    // and "keyHint" (maskSecret's output) - the one JSON representation the UI is ever allowed to see.
    std::string uiJson() const;

private:
    mutable std::mutex mutex_;
    std::vector<StreamDestination> d_;
    unsigned version_ = 1;
};

}  // namespace hb::app
