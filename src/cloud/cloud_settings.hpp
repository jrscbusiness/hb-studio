#pragma once
// Non-secret cloud-backup settings (JSON file) + the tiny ISecretStore interface for credentials.
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace hb::cloud {

// Credentials never live in the settings file: they go through an ISecretStore (Windows: DPAPI,
// per user). The UI only ever receives a masked hint.
class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual bool put(const std::string& name, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& name) = 0;
    virtual bool erase(const std::string& name) = 0;
};

// For tests and for platforms without a protected store (NOT persistent, never written to disk).
class MemorySecretStore : public ISecretStore {
public:
    bool put(const std::string& n, const std::string& v) override;
    std::optional<std::string> get(const std::string& n) override;
    bool erase(const std::string& n) override;

private:
    std::mutex mu_;
    std::map<std::string, std::string> m_;
};

inline constexpr const char* kSecS3Access = "cloud.s3.accessKey";
inline constexpr const char* kSecS3Secret = "cloud.s3.secretKey";
inline constexpr const char* kSecGClientSecret = "cloud.gdrive.clientSecret";
inline constexpr const char* kSecGRefresh = "cloud.gdrive.refreshToken";

struct CloudSettings {
    bool enabled = false;               // global kill switch / master on-off (OFF until the user configures it)
    std::string provider = "s3";        // s3 | gdrive
    // S3-compatible
    std::string s3Endpoint, s3Region = "us-east-1", s3Bucket, s3Prefix, s3Addressing = "auto";
    int partSizeMiB = 8;                // 5..512
    // Google Drive
    std::string gClientId, gFolderName = "HB Studio", gFolderId, gAccount;
    // behaviour
    double bandwidthMbps = 0;           // 0 = unlimited
    bool pauseWhileStreaming = true;
    bool upProgram = true, upIsoVideo = false, upIsoAudio = false;
    bool deleteAfterUpload = false;     // OFF by default: never delete local recordings unless asked
    bool watchFolder = true;            // folder-watch fallback next to the recorder's own events
    int stableSeconds = 30;             // 10..600
};

CloudSettings sanitize(CloudSettings s);
std::string settingsToJson(const CloudSettings& s);
bool settingsFromJson(const std::string& json, CloudSettings& out);   // false on malformed JSON
bool loadSettings(const std::string& path, CloudSettings& out);
bool saveSettings(const std::string& path, const CloudSettings& s);

// "" if empty, else a bullet run + the last 4 chars. (Same idea as the TRANSMISSÃO stream keys.)
std::string maskHint(const std::string& secret);

}  // namespace hb::cloud
