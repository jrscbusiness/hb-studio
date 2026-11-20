#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "recording/file_io.hpp"
#include "recording/md5.hpp"
#include "recording/media_writer.hpp"

namespace hb::rec {

// Lossless FLAC encoder (own implementation, no libFLAC): 4096-sample frames, constant / verbatim /
// fixed-predictor (orders 0-4) subframes with partitioned Rice residuals, stereo decorrelation
// (left/side, right/side, mid/side chosen per frame), CRC-8/CRC-16, STREAMINFO with the MD5 signature
// of the unencoded audio, patched at close(). 16 or 24 bits, 1-8 channels. Audio only.
class FlacWriter : public IMediaWriter {
public:
    static constexpr int kBlockSize = 4096;

    ~FlacWriter() override;
    bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
              const AudioFormat& audio, std::string& error) override;
    bool writeVideo(const std::uint8_t*, int, std::int64_t, std::string&) override { return true; }
    bool writeAudio(const float* interleaved, std::size_t frames, std::int64_t ptsNs, std::string& error) override;
    bool close(std::string& error) override;
    WriterStats stats() const override { return stats_; }

private:
    bool encodeBlock(int n, std::string& error);
    void writeStreamInfo(std::vector<std::uint8_t>& out, bool final) const;

    detail::OutFile file_;
    WriterStats stats_;
    int channels_ = 0, bps_ = 16, sampleRate_ = 0;
    std::vector<std::vector<std::int32_t>> pending_;  // per channel
    std::vector<std::int32_t> sideBuf_, midBuf_;
    std::vector<std::uint8_t> frameBuf_;
    detail::Md5 md5_;
    std::uint64_t totalSamples_ = 0, frameNumber_ = 0;
    std::uint32_t minFrameBytes_ = 0xFFFFFF, maxFrameBytes_ = 0;
    int firstBlockSize_ = 0, lastBlockSize_ = 0;
    bool closed_ = true, failed_ = false;
};

}  // namespace hb::rec
