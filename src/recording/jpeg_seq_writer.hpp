#pragma once
#include <memory>

#include "recording/jpeg_encoder.hpp"
#include "recording/media_writer.hpp"
#include "recording/wav_writer.hpp"

namespace hb::rec {

// JPEG image sequence: the "path" is a FOLDER (created) holding frame_000001.jpg, frame_000002.jpg, ...
// and, beside them, audio.wav (PCM, optional). Frame numbering starts at 1 and follows the Recorder's
// constant frame rate (repeated frames are written as repeated files), so `ffmpeg -framerate N -i
// frame_%06d.jpg -i audio.wav` reassembles the programme in sync.
class JpegSeqWriter : public IMediaWriter {
public:
    ~JpegSeqWriter() override;
    bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
              const AudioFormat& audio, std::string& error) override;
    bool writeVideo(const std::uint8_t* bgra, int stride, std::int64_t ptsNs, std::string& error) override;
    bool writeAudio(const float* interleaved, std::size_t frames, std::int64_t ptsNs, std::string& error) override;
    bool close(std::string& error) override;
    WriterStats stats() const override;

private:
    std::string folder_;
    std::unique_ptr<JpegEncoder> jpeg_;
    std::unique_ptr<WavWriter> wav_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t frames_ = 0, bytes_ = 0, audioFramesFinal_ = 0;
    bool closed_ = true, failed_ = false;
};

}  // namespace hb::rec
