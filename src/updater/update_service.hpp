#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "marketplace/downloader.hpp"
#include "updater/update_check.hpp"

namespace hb::updater {

struct UpdaterConfig {
    std::string releaseUrl = kDefaultReleaseUrl;   // https GET .../releases/tags/latest (overridable for forks)
    std::string dataDirUtf8;                       // settings (updates.json) + downloads/ ; empty = nothing is persisted/downloaded
    std::string appVersion;                        // e.g. "0.3.0+abc1234"
};

struct StagedInstaller {
    std::string pathUtf8;   // chosen by the app, never by the UI or the network
    std::string sha256;     // computed locally after the download
    std::uint64_t size = 0;
    std::string tag, commit;
    bool verifiedAgainstPublishedHash = false;   // always true for a staged file (an unverifiable one is refused)
};

struct DownloadOutcome {
    bool ok = false;
    std::string error;      // pt-BR
    bool cancelled = false;
    StagedInstaller staged;
};

// Check + download half of the self-update. Thread-safe; check()/downloadInstaller() block (run them off the UI
// thread). It NEVER runs the installer: the host does that, after its own native confirmation dialog.
class UpdateService {
public:
    using NowFn = std::function<std::int64_t()>;   // epoch seconds (injectable for tests)
    using ProgressFn = std::function<void(int percent)>;

    UpdateService(marketplace::IHttpDownloader& http, UpdaterConfig config, NowFn now = nullptr);

    void load();                                   // reads updates.json (missing/corrupt = defaults)
    bool autoCheckEnabled() const;
    void setAutoCheck(bool on);
    // Empty restores the default. Only https URLs on api.github.com are accepted; returns false otherwise.
    bool setReleaseUrl(const std::string& url);
    bool dueForAutoCheck() const;
    bool busy() const { return checking_ || downloading_; }

    // Asks GitHub for the release and updates the state. `manual` ignores the 6 h interval but is still throttled.
    void check(bool manual);
    // "Depois": hides the notice for the current release (a newer release shows again).
    void dismiss();

    DownloadOutcome downloadInstaller(const ProgressFn& progress);
    void cancelDownload() { cancel_ = true; }
    // Re-hashes the staged file right before it is run. false + pt-BR error if anything changed.
    bool verifyStaged(std::string& error) const;
    StagedInstaller staged() const;
    void discardStaged();

    // {"t":"upd_state",...} for the CONFIG card (no secrets, never touches the network).
    std::string stateJson() const;
    UpdateState state() const;

private:
    void saveLocked() const;
    std::int64_t now() const { return now_ ? now_() : 0; }
    UpdateState stateLocked() const;

    marketplace::IHttpDownloader& http_;
    UpdaterConfig cfg_;
    NowFn now_;
    mutable std::mutex m_;
    bool autoCheck_ = true;
    std::int64_t lastSuccess_ = 0, lastAttempt_ = 0;
    std::string dismissedCommit_;
    bool hasRelease_ = false;
    ReleaseInfo release_;
    std::string error_;
    std::string phase_ = "idle";   // idle | download | verify | ready | error
    int pct_ = 0;
    StagedInstaller staged_;
    bool hasStaged_ = false;
    std::atomic<bool> checking_{false}, downloading_{false}, cancel_{false};
};

}  // namespace hb::updater
