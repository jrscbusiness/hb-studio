#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "recording/file_io.hpp"
#include "recording/jpeg_encoder.hpp"
#include "recording/media_writer.hpp"

namespace hb::rec {

// AVI 1.0 (RIFF 'AVI ') with Motion-JPEG video (fourcc MJPG, every frame a keyframe) and 16-bit PCM
// audio, `idx1` index at the end. Audio is interleaved after each video frame (the samples that belong to
// that frame period), so players can stream the file. Constant frame rate: pts are ignored, the Recorder
// guarantees one writeVideo per frame slot. Limited to ~4 GB (the Recorder splits before that).
class AviMjpegWriter : public IMediaWriter {
public:
    ~AviMjpegWriter() override;
    bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
              const AudioFormat& audio, std::string& error) override;
    bool writeVideo(const std::uint8_t* bgra, int stride, std::int64_t ptsNs, std::string& error) override;
    bool writeAudio(const float* interleaved, std::size_t frames, std::int64_t ptsNs, std::string& error) override;
    bool close(std::string& error) override;
    WriterStats stats() const override { return stats_; }

private:
    struct IndexEntry {
        char id[4];
        std::uint32_t flags, offset, size;
    };
    bool writeChunk(const char* id, const std::uint8_t* data, std::size_t n, std::string& error);
    bool emitAudio(std::uint64_t upToFrame, std::string& error);
    bool fits(std::size_t add) const;

    detail::OutFile file_;
    WriterStats stats_;
    VideoFormat video_;
    AudioFormat audio_;
    std::unique_ptr<JpegEncoder> jpeg_;
    std::vector<std::uint8_t> jpegBuf_, chunkBuf_, pendingPcm_;
    std::vector<IndexEntry> index_;
    std::uint64_t moviListSizePos_ = 0, moviFourccPos_ = 0;
    std::uint64_t avihTotalPos_ = 0, avihBufPos_ = 0, avihRatePos_ = 0, strhVLenPos_ = 0, strhVBufPos_ = 0,
                  strhALenPos_ = 0, strhABufPos_ = 0;
    std::uint64_t audioEmitted_ = 0, audioQueued_ = 0;
    std::uint32_t maxVideoChunk_ = 0, maxAudioChunk_ = 0;
    int blockAlign_ = 0;
    bool hasAudio_ = false, closed_ = true, failed_ = false;
    std::vector<std::uint8_t> pcmScratch_;
};

}  // namespace hb::rec
