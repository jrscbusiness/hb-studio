#pragma once
// Glue of the cloud-backup feature: settings + secrets + provider factory + upload engine + file
// feeds + UI command handling. Portable: every platform piece (HTTP, secret store, browser launch,
// thread priority) is injected, so the whole thing runs against fakes in the unit tests.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cloud/cloud_settings.hpp"
#include "cloud/cloud_types.hpp"
#include "cloud/file_feed.hpp"
#include "cloud/gdrive_oauth.hpp"
#include "cloud/upload_engine.hpp"

namespace hb::cloud {

struct CloudServiceDeps {
    std::string dataDir;                              // cloud.json + cloud_journal.json live here
    std::shared_ptr<ICloudHttp> http;                 // required
    std::shared_ptr<ISecretStore> secrets;            // required (Windows: DPAPI)
    std::shared_ptr<IFileSystem> fs;                  // default: std::filesystem
    hb::core::EventBus* bus = nullptr;                // subscribes to recording.fileClosed
    std::function<std::string()> recordingsFolder;    // for the folder-watch fallback
    std::function<bool()> streamingActive;
    std::function<void(const std::string&)> openBrowser;  // OAuth consent page
    std::function<void()> threadInit;                 // background priority for the upload thread
    RandomBytesFn random;                             // default: std::random_device
    std::function<std::int64_t()> wallNow;
    std::function<double()> monoNow;
    std::function<void(double)> sleepFn;
    bool startThreads = true;                         // tests turn this off and drive things by hand
};

class CloudService {
public:
    explicit CloudService(CloudServiceDeps deps);
    ~CloudService();

    void start();
    void stop();

    // "cloudsave|{json}", "cloudclear|s3", "cloudtest", "cloudgoogle|login", "cloudretry|id|all",
    // "cloudcancel|id", "cloudclearfinished", "cloudscan". Returns a toast text ("" = none). Never throws.
    std::string handle(const std::vector<std::string>& args);

    // {"t":"cloud_state",...}: the only representation the UI sees (secrets reduced to hints).
    std::string stateJson();
    unsigned version() const;

    CloudSettings settings() const;
    UploadEngine& engine() { return *engine_; }
    void scanFolderNow() { if (watch_) watch_->poll(); }
    void joinAsync();  // waits for a running test/login (tests, shutdown)

private:
    void applySettings(bool rebuild);
    std::shared_ptr<ICloudProvider> buildProvider(std::string& status);
    void onClosed(const ClosedFile& f);
    void runTest();
    void runLogin();
    void startAsync(std::function<void()> fn);
    void refreshHints();
    void scanLoop();
    void setTest(const std::string& st, const std::string& msg);
    void setAuth(const std::string& st, const std::string& msg);

    CloudServiceDeps d_;
    mutable std::mutex mu_;
    CloudSettings s_;
    std::string cfgPath_;
    std::string status_;
    std::string hintS3Access_, hintS3Secret_, hintGClient_, hintGRefresh_;
    std::string testState_ = "idle", testMsg_, authState_ = "idle", authMsg_;
    std::shared_ptr<ICloudProvider> provider_;
    std::unique_ptr<UploadEngine> engine_;
    std::unique_ptr<EventBusFeed> busFeed_;
    std::unique_ptr<FolderWatchFeed> watch_;
    std::mutex watchMu_;
    std::atomic<unsigned> version_{1};

    std::mutex asyncMu_;
    std::thread async_;
    std::atomic<bool> asyncBusy_{false};
    std::atomic<bool> stopping_{false};
    std::thread scanner_;
    std::mutex scanMu_;
    std::condition_variable scanCv_;
};

}  // namespace hb::cloud
