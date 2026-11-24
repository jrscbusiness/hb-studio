#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "marketplace/catalog.hpp"
#include "marketplace/downloader.hpp"

namespace hb::marketplace {

inline constexpr const char* kDefaultCatalogUrl = "https://raw.githubusercontent.com/jrscbusiness/hb-studio/main/marketplace/index.json";

struct MarketplaceConfig {
    std::string catalogUrl = kDefaultCatalogUrl;
    std::string cacheDirUtf8;       // last good index.json + ETag
    std::string downloadDirUtf8;    // temp .hbplugin files (purged at the start of every install)
    std::string appVersion;         // e.g. "0.3.0+abc1234"
    std::string platform = "windows-x64";
};

// Hooks into the existing plugin system (kept as callbacks so this module does not depend on it):
struct PluginHooks {
    std::function<std::map<std::string, std::string>()> installedVersions;   // id -> version
    std::function<std::string(const std::string& packagePath)> beginInstall;  // PluginService::beginInstall
    std::function<void(const std::string& token)> cancelInstall;              // PluginService::cancelInstall
    // Template entries (pure data): import the verified hb-templates file; returns {ok, message pt-BR}.
    std::function<std::pair<bool, std::string>(const std::string& jsonPath)> importTemplate;
};

struct InstallOutcome {
    bool ok = false;
    std::string message;        // pt-BR, when !ok (or a short status)
    std::string reviewJson;     // the plugin system's {"t":"plugin_review",...} (set when the review dialog should open)
    bool imported = false;      // template entries: already imported, no review dialog
    std::string contextJson;    // {"t":"mkt_review",...}: marketplace facts for the dialog (verified, native)
};

// Catalog fetching + caching + the download/verify half of the install flow.
//   refresh()          fetch (ETag, size/time limits), validate, cache; offline -> last good copy
//   catalogJson()      what the UI shows (does NOT touch the network)
//   install(id)        pick version -> download -> check size + SHA-256 against the catalog -> hand the file to
//                      PluginService::beginInstall. The user's consent dialog is ALWAYS the next step; nothing is
//                      ever installed or enabled from here.
// All methods are thread-safe; refresh()/install() block (run them off the UI thread).
class MarketplaceService {
public:
    MarketplaceService(IHttpDownloader& http, MarketplaceConfig config, PluginHooks hooks);

    void refresh();
    std::string catalogJson() const;

    using ProgressCb = std::function<void(const std::string& phase, int percent)>;   // "download" | "verify" | "review"
    InstallOutcome install(const std::string& id, const ProgressCb& progress);
    void cancelInstall() { cancel_ = true; }
    bool installing() const { return installing_; }

    // Empty or non-https URLs are refused (returns false); empty `url` restores the default.
    bool setCatalogUrl(const std::string& url);
    std::string catalogUrl() const;

private:
    struct State {
        bool loaded = false;        // a catalog is available (fresh or cached)
        bool fromCache = false;
        bool offline = false;       // the last refresh could not reach the server
        std::string error;          // pt-BR, shown in the UI (empty when fine)
        Catalog catalog;
    };
    void applyCached(State& st, const std::string& url);
    bool loadCache(const std::string& url, std::string& body, std::string& etag) const;
    void saveCache(const std::string& url, const std::string& body, const std::string& etag) const;

    IHttpDownloader& http_;
    MarketplaceConfig cfg_;
    PluginHooks hooks_;
    mutable std::mutex mutex_;
    State state_;
    std::atomic<bool> installing_{false};
    std::atomic<bool> cancel_{false};
};

}  // namespace hb::marketplace
