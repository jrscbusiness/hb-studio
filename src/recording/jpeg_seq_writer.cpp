#include "recording/jpeg_seq_writer.hpp"

#include <cstdio>
#include <filesystem>

#include "recording/file_io.hpp"

namespace hb::rec {

JpegSeqWriter::~JpegSeqWriter() {
    std::string e;
    close(e);
}

bool JpegSeqWriter::open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
                         const AudioFormat& audio, std::string& error) {
    if (!closed_) {
        error = "writer ja aberto";
        return false;
    }
    if (!video.present()) {
        error = "sequencia JPEG exige video";
        return false;
    }
    JpegOptions jo;
    jo.quality = profile.videoQuality;
    jo.chroma420 = true;
    jpeg_ = std::make_unique<JpegEncoder>(video.width, video.height, jo);
    if (!jpeg_->valid()) {
        error = "dimensoes de video invalidas para JPEG";
        return false;
    }
    std::error_code ec;
    const auto dir = detail::pathFromUtf8(pathUtf8);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "nao foi possivel criar a pasta da sequencia";
        return false;
    }
    folder_ = pathUtf8;
    wav_.reset();
    if (profile.hasAudio && audio.present()) {
        wav_ = std::make_unique<WavWriter>();
        RecordingProfile wp = profile;
        wp.audioBitsPerSample = profile.audioBitsPerSample;
        if (!wav_->open(detail::pathToUtf8(dir / "audio.wav"), wp, video, audio, error)) {
            wav_.reset();
            return false;
        }
    }
    frames_ = bytes_ = 0;
    failed_ = false;
    closed_ = false;
    return true;
}

bool JpegSeqWriter::writeVideo(const std::uint8_t* bgra, int stride, std::int64_t, std::string& error) {
    if (closed_ || failed_) {
        error = "writer de sequencia fechado";
        return false;
    }
    if (!jpeg_->encode(bgra, stride, buf_)) {
        failed_ = true;
        error = "falha ao codificar quadro JPEG";
        return false;
    }
    char name[40];
    std::snprintf(name, sizeof(name), "frame_%06llu.jpg", static_cast<unsigned long long>(frames_ + 1));
    detail::OutFile f;
    const std::string path = detail::pathToUtf8(detail::pathFromUtf8(folder_) / name);
    if (!f.open(path, error) || !f.write(buf_, error) || !f.close(error)) {
        failed_ = true;
        return false;
    }
    ++frames_;
    bytes_ += buf_.size();
    return true;
}

bool JpegSeqWriter::writeAudio(const float* in, std::size_t frames, std::int64_t pts, std::string& error) {
    if (closed_ || failed_) {
        error = "writer de sequencia fechado";
        return false;
    }
    if (!wav_) return true;
    if (!wav_->writeAudio(in, frames, pts, error)) {
        failed_ = true;
        return false;
    }
    return true;
}

bool JpegSeqWriter::close(std::string& error) {
    if (closed_) return true;
    closed_ = true;
    bool ok = true;
    if (wav_) {
        ok = wav_->close(error);
        bytes_ += wav_->stats().bytesWritten;
        audioFramesFinal_ = wav_->stats().audioFrames;
        wav_.reset();
    }
    return ok;
}

WriterStats JpegSeqWriter::stats() const {
    WriterStats s;
    s.videoFrames = frames_;
    s.audioFrames = wav_ ? wav_->stats().audioFrames : audioFramesFinal_;
    s.bytesWritten = bytes_ + (wav_ ? wav_->stats().bytesWritten : 0);
    return s;
}

}  // namespace hb::rec
