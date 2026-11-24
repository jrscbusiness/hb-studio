#pragma once
// Persistent, resumable upload queue. One background thread (low priority) uploads one chunk of one
// file at a time: a stream that starts, a cancel or the kill switch is honoured within a chunk, and
// the live broadcast always wins. All time and I/O is injected, so everything is unit-testable.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cloud/cloud_types.hpp"
#include "cloud/file_system.hpp"
#include "cloud/throttle.hpp"

namespace hb::cloud {

enum class JobState { Queued, Uploading, Done, Error, Cancelled };
const char* jobStateName(JobState s);

struct Job {
    std::string id;
    std::string path;    // local, UTF-8
    std::string kind;    // program | iso-video | iso-audio
    int index = 0;
    std::string remoteName;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool closedByProducer = false;
    JobState state = JobState::Queued;
    int attempts = 0;           // consecutive failed attempts (reset on progress)
    std::int64_t nextAt = 0;    // epoch seconds: not before
    std::uint64_t done = 0;     // bytes safely stored remotely
    std::string error;          // last error / waiting reason (pt-BR)
    std::string errorKind;
    std::string note;           // soft info ("já existia", integrity warnings)
    std::int64_t createdAt = 0, finishedAt = 0;
    bool hasSession = false;
    UploadSession session;
    std::string remoteId;
};

struct EngineOptions {
    std::string journalPath;  // "" = in memory only
    std::shared_ptr<IFileSystem> fs;
    std::function<std::int64_t()> wallNow;    // epoch seconds
    std::function<double()> monoNow;          // monotonic seconds (throttle, rate)
    std::function<void(double)> sleepFn;      // blocks for N seconds (throttle waits); default: std::this_thread
    std::function<bool()> streamingActive;    // is a live stream running?
    std::function<void()> threadInit;         // runs first on the worker thread (Windows: background priority)
    int maxAttempts = 8;
    double backoffBaseSec = 5, backoffCapSec = 600;
    int stableSeconds = 30;                   // a file not declared closed must be unchanged this long
    std::vector<std::string> extensions = {"mp4", "mkv", "mov", "wav", "flac", "avi"};
    std::size_t maxJobs = 500;
};

struct Totals {
    int queued = 0, uploading = 0, done = 0, error = 0, cancelled = 0;
    std::uint64_t bytesTotal = 0, bytesDone = 0;  // over queued+uploading+error+done jobs of this journal
    double bytesPerSec = 0;
    bool paused = false;
    std::string pauseReason;
};

class UploadEngine {
public:
    explicit UploadEngine(EngineOptions opts);
    ~UploadEngine() { stop(); }
    UploadEngine(const UploadEngine&) = delete;
    UploadEngine& operator=(const UploadEngine&) = delete;

    void setProvider(std::shared_ptr<ICloudProvider> p);  // nullptr = nothing is uploaded
    void setBandwidthMbps(double mbps);                    // 0 = unlimited
    void setPauseWhileStreaming(bool on) { pauseWhileStreaming_ = on; }
    void setEnabled(bool on);                              // global kill switch (false = stop everything now)
    void setDeleteAfterUpload(bool on) { deleteAfter_ = on; }
    bool enabled() const { return enabled_; }

    enum class Enqueue { Added, Duplicate, Rejected };
    Enqueue enqueue(const std::string& path, const std::string& kind, int index, bool closedByProducer, std::string* why = nullptr);

    bool retry(const std::string& id);   // Error/Cancelled -> Queued, from scratch of the attempt counter
    int retryAll();                      // every Error job
    bool cancel(const std::string& id);  // Queued/Uploading/Error -> Cancelled
    int clearFinished();                 // forgets Done and Cancelled entries

    // Processes at most one chunk of one job. Returns true if it did any work. Tests drive this directly.
    bool runOnce();

    void start();
    void stop();
    void wake();

    std::vector<Job> jobs() const;
    Totals totals();
    unsigned version() const { return version_; }  // bumps on every state change (not on byte progress)
    bool saveJournal();
    bool loadJournal();

    // Pure helpers (public for tests)
    static std::string remoteNameFor(const std::string& path, const std::string& kind, std::int64_t mtime);
    double backoffFor(int attempts) const;

private:
    class Gate;
    bool paused(std::string* reason) const;
    bool abortRequested() const;
    void process(Job& j, const std::shared_ptr<ICloudProvider>& prov);
    void fail(Job& j, const Error& e, const std::shared_ptr<ICloudProvider>& prov);
    void finalize(Job& j, const std::shared_ptr<ICloudProvider>& prov, const FileInfo& fi);
    bool md5OfFile(const std::string& path, std::uint64_t size, std::string& out);
    void touch() { ++version_; }
    void saveLocked();
    void workerLoop();
    void pruneLocked();
    std::int64_t now() const;

    EngineOptions o_;
    mutable std::mutex mu_;
    std::vector<Job> jobs_;
    std::shared_ptr<ICloudProvider> provider_;
    std::set<std::string> resync_;      // job ids loaded from the journal: ask the provider what is stored
    std::set<std::string> dedupeDone_;  // job ids whose remote existence was already checked this run
    std::uint64_t nextId_ = 1;
    std::string currentId_;
    std::atomic<bool> cancelCurrent_{false};
    std::atomic<bool> enabled_{true}, pauseWhileStreaming_{true}, deleteAfter_{false}, stopping_{false};
    std::atomic<std::uint64_t> inflight_{0}, sentTotal_{0};
    std::atomic<unsigned> version_{1};
    TokenBucket bucket_;
    std::mutex rateMu_;
    std::deque<std::pair<double, std::uint64_t>> rate_;

    std::thread worker_;
    std::mutex cvMu_;
    std::condition_variable cv_;
};

}  // namespace hb::cloud
