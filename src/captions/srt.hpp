#pragma once
// SubRip (.srt) writer for the session transcript. Pure; the only I/O is saveSrtFile().
#include <cstdint>
#include <string>
#include <vector>

namespace hb::captions {

struct SrtCue {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::string text;   // '\n' separates lines
};

// "HH:MM:SS,mmm" (negative values clamp to 0).
std::string formatSrtTime(std::int64_t ms);

class SrtBuilder {
public:
    // Cues are kept in call order. Empty text is ignored; start is pushed to the previous end when
    // they would overlap; a cue shorter than minDurationMs is stretched (capped by maxDurationMs).
    void add(std::int64_t startMs, std::int64_t endMs, const std::string& text);
    std::size_t size() const { return cues_.size(); }
    const std::vector<SrtCue>& cues() const { return cues_; }
    // Whole file, "\r\n" line endings, blank line after each cue.
    std::string str() const;
    void clear() { cues_.clear(); }

    std::int64_t minDurationMs = 800;
    std::int64_t maxDurationMs = 8000;

private:
    std::vector<SrtCue> cues_;
};

// Writes UTF-8 text to `pathUtf8` (via a ".tmp" file then rename). Returns false on failure.
bool saveSrtFile(const std::string& pathUtf8, const std::string& content);

}  // namespace hb::captions
