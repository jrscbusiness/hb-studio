#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Update check against the public GitHub "latest" pre-release (published by the CI with
// HBStudio-Setup.exe). Portable and unit-tested; the Windows glue (dialog, launching the installer)
// lives in app_main.cpp. Everything that comes from the network is UNTRUSTED: the parser is strict,
// bounded and never throws. See docs/ATUALIZACAO.md (pt-BR) for the threat model: the installer is
// currently NOT code-signed, so the SHA-256 published in the same release only detects corruption, it
// does not protect against a compromised release.
namespace hb::updater {

inline constexpr const char* kDefaultReleaseUrl = "https://api.github.com/repos/jrscbusiness/hb-studio/releases/tags/latest";
inline constexpr const char* kInstallerAssetName = "HBStudio-Setup.exe";
inline constexpr const char* kChecksumAssetName = "HBStudio-Setup.exe.sha256";
inline constexpr std::size_t kMaxReleaseJsonBytes = 1024 * 1024;
inline constexpr std::size_t kMaxChecksumBytes = 4096;
inline constexpr std::uint64_t kMaxInstallerBytes = 150ull * 1024 * 1024;   // today ~6 MB, ~18 MB with ONNX Runtime
inline constexpr std::size_t kMaxNotesBytes = 600;
inline constexpr std::int64_t kAutoCheckIntervalSec = 6 * 3600;
inline constexpr std::int64_t kRetryAfterFailureSec = 3600;
inline constexpr std::int64_t kManualCooldownSec = 20;

struct ReleaseAsset {
    std::string name, url;
    std::uint64_t size = 0;
    std::string sha256;   // lowercase hex from the API "digest" field ("sha256:..."), empty when absent
};

struct ReleaseInfo {
    std::string tag, name, publishedAt;
    std::string targetCommit;            // lowercase hex, 7..40 chars; empty when unknown
    std::string notes;                   // sanitized excerpt (valid UTF-8, no control chars, <= kMaxNotesBytes)
    bool prerelease = false;
    bool hasInstaller = false;
    ReleaseAsset installer;              // valid only when hasInstaller
    std::string checksumUrl;             // asset "HBStudio-Setup.exe.sha256", empty when the release has none
};

struct ParseResult {
    bool ok = false;
    std::string error;                   // pt-BR, short
    ReleaseInfo release;
};

// Parses the body of GET /repos/{owner}/{repo}/releases/tags/{tag}.
ParseResult parseRelease(std::string_view json);

// "0.3.0+abc1234" -> "abc1234"; empty when there is no build suffix or it is not 7..40 hex chars.
std::string commitOfVersion(std::string_view version);

enum class UpdateState { Unknown, UpToDate, Available };
// Unknown: the current build has no commit suffix, or the release does not say which commit it is, or it has
// no installer. Otherwise a prefix comparison of the two hashes (the shorter one must be a prefix of the other).
UpdateState compareWithRelease(std::string_view currentVersion, const ReleaseInfo& release);

// Policy helpers.
// Auto check: enabled, >= 6 h since the last success and >= 1 h since the last attempt (failures retry sooner).
bool autoCheckDue(bool enabled, std::int64_t now, std::int64_t lastSuccess, std::int64_t lastAttempt);
// Manual check: only throttled for kManualCooldownSec so a mashed button cannot hammer the API.
bool manualCheckAllowed(std::int64_t now, std::int64_t lastAttempt);

// Checksum file body ("<64 hex>  HBStudio-Setup.exe", "<hex> *name" or just the hex) -> lowercase hex or "".
// A line that names a different file is rejected.
std::string parseChecksumFile(std::string_view text, std::string_view assetName);

// Host of an https URL, lowercase ("" if the URL is not https or has no host).
std::string urlHost(std::string_view url);

// Truncates at a UTF-8 boundary, strips control characters (keeps \n), collapses blank runs. Exposed for tests.
std::string sanitizeNotes(std::string_view raw, std::size_t maxBytes);

}  // namespace hb::updater
