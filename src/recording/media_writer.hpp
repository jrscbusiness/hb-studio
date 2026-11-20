#pragma once
// Contract shared by the portable writers/recorder (src/recording/), the Windows Media Foundation
// writers (src/recording/windows/) and the application.  FROZEN INTERFACE.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hb::rec {

struct VideoFormat {
    int width = 0, height = 0;
    int fpsNum = 30, fpsDen = 1;
    bool present() const { return width > 0 && height > 0; }
};
struct AudioFormat {
    int sampleRate = 0, channels = 0;
    bool present() const { return sampleRate > 0 && channels > 0; }
};

// One selectable output format. Tunables are shared by all profiles; each writer uses what applies.
struct RecordingProfile {
    std::string id;           // "mp4-h264-aac"
    std::string label;        // "MP4 — H.264 + AAC (recomendado)"
    std::string description;  // one line, pt-BR
    std::string extension;    // "mp4" (no dot)
    bool hasVideo = true;
    bool hasAudio = true;
    std::string kind;         // "video" | "audio" (UI grouping)

    int videoBitrateKbps = 8000;     // H.264 / HEVC / WMV
    int videoQuality = 85;           // MJPEG, JPEG sequence: 1..100
    int audioBitrateKbps = 192;      // AAC / MP3 / WMA
    int audioSampleRate = 48000;     // output rate: 44100 / 48000 / 96000
    int audioBitsPerSample = 24;     // WAV / FLAC: 16 / 24 / 32 (32 = float, WAV only)
    int keyframeIntervalSec = 2;
};

struct WriterStats {
    std::uint64_t videoFrames = 0;
    std::uint64_t audioFrames = 0;   // sample frames (per channel)
    std::uint64_t bytesWritten = 0;
};

// A writer is used by ONE thread (the recorder's writer thread). Timestamps are nanoseconds since the
// start of the recording. Errors are returned, never thrown; after a false return the writer is dead.
class IMediaWriter {
public:
    virtual ~IMediaWriter() = default;
    virtual bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
                      const AudioFormat& audio, std::string& error) = 0;
    // BGRA8, top-down, `stride` bytes per row.
    virtual bool writeVideo(const std::uint8_t* bgra, int stride, std::int64_t ptsNs, std::string& error) = 0;
    // Interleaved float32 in [-1, 1]; `frames` sample frames; pts of the first one.
    virtual bool writeAudio(const float* interleaved, std::size_t frames, std::int64_t ptsNs, std::string& error) = 0;
    // Finalises headers/indexes. Safe to call more than once. No writeX after close.
    virtual bool close(std::string& error) = 0;
    virtual WriterStats stats() const = 0;
};

using WriterFactory = std::function<std::unique_ptr<IMediaWriter>()>;

// Process-wide table of available formats. Portable writers register themselves in
// registerPortableWriters(); the Windows app adds the Media Foundation ones.
class WriterRegistry {
public:
    static WriterRegistry& instance();
    bool add(const RecordingProfile& profile, WriterFactory factory);   // false: duplicate id
    std::vector<RecordingProfile> profiles() const;                     // registration order
    bool find(const std::string& id, RecordingProfile& profile, WriterFactory& factory) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<RecordingProfile, WriterFactory>> items_;
};

// Defined by the portable recording module (src/recording/): wav, flac, avi-mjpeg, ...
void registerPortableWriters();

}  // namespace hb::rec
