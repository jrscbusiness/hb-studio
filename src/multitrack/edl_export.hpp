#pragma once
// CMX 3600 EDL export, one list per track (an EDL carries a single video/audio lane, so a multitrack
// session becomes one list per track: import each into its own timeline lane at the same start).
// Source timecode starts at 00:00:00:00 in every file (the files are not timecoded); record timecode is the
// session time of the file's first frame/sample, so importing all lists at timeline zero re-syncs the tracks.
// NOT validated against Resolve/Premiere/Final Cut (no access here): see docs/GRAVACAO_MULTIPISTA.md.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "multitrack/session_manifest.hpp"

namespace hb::multitrack {

// `frames` real frames at fpsNum/fpsDen -> "HH:MM:SS:FF" (or "HH:MM:SS;FF" when dropFrame).
// Drop-frame is only defined for 30000/1001 (drops 2 per minute) and 60000/1001 (drops 4); for any other
// rate `dropFrame` is ignored.
std::string framesToTimecode(std::uint64_t frames, int fpsNum, int fpsDen, bool dropFrame);
// True when the EDL for this rate should be drop-frame (29.97 / 59.94).
bool usesDropFrame(int fpsNum, int fpsDen);

struct EdlFile {
    std::string relativePath;   // "edl/CAM01.edl"
    std::string content;        // CRLF line endings
};

// `tcFpsNum/Den`: the timeline rate used for the audio lists (video lists use the track's own rate).
std::vector<EdlFile> buildSessionEdls(const SessionManifest& m, int tcFpsNum, int tcFpsDen);

}  // namespace hb::multitrack
