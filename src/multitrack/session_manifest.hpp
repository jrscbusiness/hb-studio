#pragma once
// session.json: everything an editor (or a script) needs to put the tracks of one recording session back in
// sync: start wall-clock, per-track offsets on the shared session clock, formats, devices and the honest
// counters (dropped / repeated frames, inserted silence, drift). Schema 1, UTF-8, pretty printed.
#include <cstdint>
#include <string>
#include <vector>

namespace hb::multitrack {

struct ManifestSegment {
    int index = 1;
    std::string file;                 // relative to the session folder, '/' separators
    std::int64_t startNs = 0;         // session time of the first frame / sample of the file (media time zero)
    std::uint64_t startUnit = 0;      // same instant as a video slot / audio sample index on the session timeline
    std::uint64_t units = 0;          // video slots or audio sample frames in the file
    std::uint64_t bytes = 0;
    bool closed = false;              // finalised properly (false: still open, or the app died before closing)
};

struct ManifestTrack {
    std::string id;                   // "program", "CAM01", "CH01"
    std::string kind;                 // "program" | "video" | "audio"
    std::string name;                 // label shown to the user (camera name / channel name)
    std::string device;               // capture device name, when known
    std::string codec;                // "h264", "pcm_s24le", "pcm_f32le", "flac", ...
    int sourceIndex = -1;             // camera slot / mixer strip (0-based)
    int width = 0, height = 0, fpsNum = 0, fpsDen = 0;
    int sampleRate = 0, channels = 0, bitsPerSample = 0;
    std::string tap;                  // audio: "pre-fader" | "post-fader"
    std::string state;                // "recording" | "finished" | "error" | "disk-stopped" | "not-started"
    std::string error;
    std::int64_t startOffsetNs = 0;   // first frame/sample of the FIRST file relative to the session start
    std::uint64_t leadingSilenceFrames = 0;   // audio: silence the writer inserted so sample 0 == session start
    std::uint64_t units = 0, repeated = 0, droppedQueue = 0, droppedTimeline = 0, silenceFrames = 0, jumped = 0;
    double driftPpm = 0;              // audio: average correction of the source clock vs the session clock
    std::vector<ManifestSegment> segments;
};

struct SessionManifest {
    int schema = 1;
    std::string app = "HB Studio";
    std::string appVersion;
    std::string sessionName;
    std::string state = "recording";  // "recording" | "finished" | "interrupted"
    std::int64_t startWallUnixMs = 0;
    std::int64_t monotonicT0Ns = 0;   // steady_clock reading at session time zero (informational)
    std::int64_t durationNs = 0;
    int rotationSeconds = 0;          // 0 = no rotation
    std::string syncNote;
    std::vector<ManifestTrack> tracks;
    std::vector<std::string> warnings;
};

// "2026-11-24T14:05:33.250Z"
std::string unixMsToIsoUtc(std::int64_t ms);
std::string manifestToJson(const SessionManifest& m);

}  // namespace hb::multitrack
