#pragma once
#include <cstdint>
#include <vector>

#include "recording/file_io.hpp"
#include "recording/media_writer.hpp"

namespace hb::rec {

namespace detail {
// Payload of a `fmt ` chunk / WAVEFORMATEX(TENSIBLE) for PCM (bits 16/24) or IEEE float (bits 32).
// Uses WAVE_FORMAT_EXTENSIBLE for 24 bits, more than 2 channels, or float with more than 2 channels.
std::vector<std::uint8_t> buildWaveFormat(int channels, int sampleRate, int bits);
}  // namespace detail

// RIFF/WAVE writer: PCM 16/24 bits or float32, any channel count. Audio only (writeVideo is ignored).
// The data chunk is limited to ~4 GB (the Recorder splits files before that).
class WavWriter : public IMediaWriter {
public:
    ~WavWriter() override;
    bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
              const AudioFormat& audio, std::string& error) override;
    bool writeVideo(const std::uint8_t*, int, std::int64_t, std::string&) override { return true; }
    bool writeAudio(const float* interleaved, std::size_t frames, std::int64_t ptsNs, std::string& error) override;
    bool close(std::string& error) override;
    WriterStats stats() const override { return stats_; }

private:
    detail::OutFile file_;
    WriterStats stats_;
    int channels_ = 0, bits_ = 16;
    std::uint64_t dataBytes_ = 0;
    std::uint64_t dataSizeOffset_ = 0, factOffset_ = 0;
    bool floatFmt_ = false, closed_ = true, failed_ = false;
    std::vector<std::uint8_t> scratch_;
};

}  // namespace hb::rec
