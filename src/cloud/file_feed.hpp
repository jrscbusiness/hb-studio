#pragma once
// Sources of "this recording file is finished" notifications for the upload engine.
//  - EventBusFeed: the recorder publishes `recording.fileClosed` {path, kind, index} (authoritative).
//  - FolderWatchFeed: fallback that polls the recordings folder and reports files that stopped growing.
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cloud/file_system.hpp"
#include "core/event_bus.hpp"

namespace hb::cloud {

struct ClosedFile {
    std::string path;
    std::string kind = "program";  // program | iso-video | iso-audio
    int index = 0;
    bool closedByProducer = false;  // true: the recorder said so; false: inferred by the folder watcher
};

using FileSink = std::function<void(const ClosedFile&)>;

class IFileFeed {
public:
    virtual ~IFileFeed() = default;
    virtual void setSink(FileSink sink) = 0;
};

class EventBusFeed : public IFileFeed {
public:
    explicit EventBusFeed(hb::core::EventBus& bus);
    ~EventBusFeed() override;
    void setSink(FileSink sink) override;
    // Pure parsing step (tests): false if the event is malformed (no path / unknown kind).
    static bool parse(const hb::core::Event& e, ClosedFile& out);

private:
    hb::core::EventBus& bus_;
    hb::core::SubscriptionId id_ = 0;
    std::mutex mu_;
    FileSink sink_;
};

class FolderWatchFeed : public IFileFeed {
public:
    struct Options {
        std::shared_ptr<IFileSystem> fs;
        std::function<std::string()> folder;        // current recordings folder (UTF-8)
        std::function<std::int64_t()> wallNow;      // epoch seconds
        int stableSeconds = 30;
        std::vector<std::string> extensions = {"mp4", "mkv", "mov", "wav", "flac", "avi"};
    };
    explicit FolderWatchFeed(Options o) : o_(std::move(o)) {}
    void setSink(FileSink sink) override { sink_ = std::move(sink); }

    // One scan. The FIRST scan only records the files that already exist (never uploads history);
    // afterwards a file is reported once it is unchanged for stableSeconds and not locked.
    void poll();

    // "program" unless the name says ISO (then audio-only extensions are iso-audio, others iso-video).
    static std::string guessKind(const std::string& path);

private:
    struct Seen {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::int64_t changedAt = 0;  // when we first observed this size/mtime
        bool baseline = false;       // existed before the first scan and never changed
        bool reported = false;
    };
    Options o_;
    FileSink sink_;
    std::map<std::string, Seen> seen_;
    bool firstScan_ = true;
};

}  // namespace hb::cloud
