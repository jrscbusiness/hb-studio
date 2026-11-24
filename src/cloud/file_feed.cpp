#include "cloud/file_feed.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace hb::cloud {

EventBusFeed::EventBusFeed(hb::core::EventBus& bus) : bus_(bus) {
    id_ = bus_.subscribe("recording.fileClosed", [this](const hb::core::Event& e) {
        ClosedFile f;
        if (!parse(e, f)) return;
        FileSink sink;
        {
            std::lock_guard lock(mu_);
            sink = sink_;
        }
        if (sink) sink(f);
    });
}

EventBusFeed::~EventBusFeed() { bus_.unsubscribe(id_); }

void EventBusFeed::setSink(FileSink sink) {
    std::lock_guard lock(mu_);
    sink_ = std::move(sink);
}

bool EventBusFeed::parse(const hb::core::Event& e, ClosedFile& out) {
    auto it = e.fields.find("path");
    if (it == e.fields.end() || it->second.empty() || it->second.size() > 4096) return false;
    out = {};
    out.path = it->second;
    out.kind = "program";
    if (auto k = e.fields.find("kind"); k != e.fields.end()) {
        if (k->second != "program" && k->second != "iso-video" && k->second != "iso-audio") return false;
        out.kind = k->second;
    }
    if (auto i = e.fields.find("index"); i != e.fields.end()) out.index = std::atoi(i->second.c_str());
    out.closedByProducer = true;
    return true;
}

std::string FolderWatchFeed::guessKind(const std::string& path) {
    std::string n = baseName(path);
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("iso") == std::string::npos) return "program";
    const std::string e = fileExtension(path);
    return (e == "wav" || e == "flac") ? "iso-audio" : "iso-video";
}

void FolderWatchFeed::poll() {
    if (!o_.fs || !o_.folder || !o_.wallNow) return;
    const std::string dir = o_.folder();
    if (dir.empty()) return;
    const std::int64_t now = o_.wallNow();
    const auto files = o_.fs->listFiles(dir);
    std::set<std::string> present(files.begin(), files.end());
    for (auto it = seen_.begin(); it != seen_.end();) it = present.count(it->first) ? std::next(it) : seen_.erase(it);
    for (const auto& p : files) {
        const std::string ext = fileExtension(p);
        if (std::find(o_.extensions.begin(), o_.extensions.end(), ext) == o_.extensions.end()) continue;
        const FileStat st = o_.fs->stat(p);
        if (!st.exists || st.isDir) continue;
        auto it = seen_.find(p);
        if (it == seen_.end()) {
            Seen s;
            s.size = st.size;
            s.mtime = st.mtime;
            s.changedAt = now;
            s.baseline = firstScan_;
            it = seen_.emplace(p, s).first;
        } else if (it->second.size != st.size || it->second.mtime != st.mtime) {
            it->second.size = st.size;
            it->second.mtime = st.mtime;
            it->second.changedAt = now;
            it->second.baseline = false;
            it->second.reported = false;
        }
        Seen& s = it->second;
        if (s.baseline || s.reported || s.size == 0) continue;
        if (now - s.changedAt < o_.stableSeconds || now - s.mtime < o_.stableSeconds) continue;
        if (o_.fs->isLocked(p)) continue;
        s.reported = true;
        if (sink_) {
            ClosedFile f;
            f.path = p;
            f.kind = guessKind(p);
            f.closedByProducer = false;
            sink_(f);
        }
    }
    firstScan_ = false;
}

}  // namespace hb::cloud
