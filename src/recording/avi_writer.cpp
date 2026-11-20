#include "recording/avi_writer.hpp"

#include <algorithm>
#include <cstring>

#include "recording/pcm.hpp"
#include "recording/wav_writer.hpp"

namespace hb::rec {

namespace {
constexpr std::uint64_t kMaxFile = 0xFFFFFFFFull - (1u << 20);  // leave room for idx1 and rounding
}

AviMjpegWriter::~AviMjpegWriter() {
    std::string e;
    close(e);
}

bool AviMjpegWriter::open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video,
                          const AudioFormat& audio, std::string& error) {
    if (!closed_) {
        error = "writer ja aberto";
        return false;
    }
    if (!video.present() || video.fpsNum <= 0 || video.fpsDen <= 0) {
        error = "AVI Motion-JPEG exige video";
        return false;
    }
    hasAudio_ = profile.hasAudio && audio.present();
    if (hasAudio_ && audio.channels > 32) {
        error = "numero de canais nao suportado no AVI";
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
    video_ = video;
    audio_ = audio;
    blockAlign_ = hasAudio_ ? audio.channels * 2 : 0;  // AVI audio is always 16-bit PCM
    stats_ = {};
    index_.clear();
    pendingPcm_.clear();
    audioEmitted_ = audioQueued_ = 0;
    maxVideoChunk_ = maxAudioChunk_ = 0;
    failed_ = false;
    if (!file_.open(pathUtf8, error)) return false;

    detail::ByteBuf h;
    h.fourcc("RIFF");
    h.u32(0);
    h.fourcc("AVI ");
    const auto beginList = [&](const char* type) {
        h.fourcc("LIST");
        const std::size_t pos = h.size();
        h.u32(0);
        h.fourcc(type);
        return pos;
    };
    const auto endList = [&](std::size_t pos) {
        detail::putLe32(&h.v[pos], static_cast<std::uint32_t>(h.size() - pos - 4));
    };
    const std::size_t hdrl = beginList("hdrl");
    h.fourcc("avih");
    h.u32(56);
    const std::uint32_t usPerFrame =
        static_cast<std::uint32_t>(1000000.0 * video.fpsDen / video.fpsNum + 0.5);
    h.u32(usPerFrame);
    avihRatePos_ = h.size();
    h.u32(0);  // dwMaxBytesPerSec (patched)
    h.u32(0);  // padding granularity
    h.u32(0x10 | 0x100);  // AVIF_HASINDEX | AVIF_ISINTERLEAVED
    avihTotalPos_ = h.size();
    h.u32(0);  // total frames (patched)
    h.u32(0);  // initial frames
    h.u32(hasAudio_ ? 2 : 1);
    avihBufPos_ = h.size();
    h.u32(0);  // suggested buffer (patched)
    h.u32(static_cast<std::uint32_t>(video.width));
    h.u32(static_cast<std::uint32_t>(video.height));
    for (int i = 0; i < 4; ++i) h.u32(0);

    // --- video stream ---
    std::size_t strl = beginList("strl");
    h.fourcc("strh");
    h.u32(56);
    h.fourcc("vids");
    h.fourcc("MJPG");
    h.u32(0);  // flags
    h.u16(0), h.u16(0);  // priority, language
    h.u32(0);  // initial frames
    h.u32(static_cast<std::uint32_t>(video.fpsDen));  // scale
    h.u32(static_cast<std::uint32_t>(video.fpsNum));  // rate
    h.u32(0);                                          // start
    strhVLenPos_ = h.size();
    h.u32(0);  // length (patched)
    strhVBufPos_ = h.size();
    h.u32(0);  // suggested buffer (patched)
    h.u32(0xFFFFFFFFu);  // quality: default
    h.u32(0);            // sample size 0: one frame per chunk
    h.u16(0), h.u16(0), h.u16(static_cast<unsigned>(video.width)), h.u16(static_cast<unsigned>(video.height));
    h.fourcc("strf");
    h.u32(40);
    h.u32(40);  // BITMAPINFOHEADER
    h.u32(static_cast<std::uint32_t>(video.width));
    h.u32(static_cast<std::uint32_t>(video.height));
    h.u16(1);   // planes
    h.u16(24);  // bit count
    h.fourcc("MJPG");
    h.u32(static_cast<std::uint32_t>(video.width) * static_cast<std::uint32_t>(video.height) * 3);
    h.u32(0), h.u32(0), h.u32(0), h.u32(0);
    endList(strl);

    // --- audio stream ---
    if (hasAudio_) {
        strl = beginList("strl");
        h.fourcc("strh");
        h.u32(56);
        h.fourcc("auds");
        h.u32(0);  // handler
        h.u32(0);  // flags
        h.u16(0), h.u16(0);
        h.u32(0);
        h.u32(static_cast<std::uint32_t>(blockAlign_));                                        // scale
        h.u32(static_cast<std::uint32_t>(audio.sampleRate) * static_cast<std::uint32_t>(blockAlign_));  // rate
        h.u32(0);
        strhALenPos_ = h.size();
        h.u32(0);  // length in samples (patched)
        strhABufPos_ = h.size();
        h.u32(0);
        h.u32(0xFFFFFFFFu);
        h.u32(static_cast<std::uint32_t>(blockAlign_));  // sample size
        h.u16(0), h.u16(0), h.u16(0), h.u16(0);
        const auto fmt = detail::buildWaveFormat(audio.channels, audio.sampleRate, 16);
        h.fourcc("strf");
        h.u32(static_cast<std::uint32_t>(fmt.size()));
        h.bytes(fmt.data(), fmt.size());
        if (fmt.size() & 1) h.u8(0);
        endList(strl);
    }
    endList(hdrl);

    moviListSizePos_ = h.size() + 4;
    h.fourcc("LIST");
    h.u32(0);
    moviFourccPos_ = h.size();
    h.fourcc("movi");
    if (!file_.write(h.v, error)) {
        std::string ignore;
        file_.close(ignore);
        return false;
    }
    stats_.bytesWritten = file_.size();
    closed_ = false;
    return true;
}

bool AviMjpegWriter::fits(std::size_t add) const {
    return file_.size() + add + 16 * (index_.size() + 2) + 8 < kMaxFile;
}

bool AviMjpegWriter::writeChunk(const char* id, const std::uint8_t* data, std::size_t n, std::string& error) {
    if (!fits(n + 9)) {
        error = "limite de 4 GB do AVI atingido";
        return false;
    }
    chunkBuf_.clear();
    detail::ByteBuf b;
    b.v.swap(chunkBuf_);
    b.fourcc(id);
    b.u32(static_cast<std::uint32_t>(n));
    b.bytes(data, n);
    if (n & 1) b.u8(0);
    chunkBuf_.swap(b.v);
    IndexEntry e;
    std::memcpy(e.id, id, 4);
    e.flags = 0x10;  // AVIIF_KEYFRAME
    e.offset = static_cast<std::uint32_t>(file_.size() - moviFourccPos_);
    e.size = static_cast<std::uint32_t>(n);
    if (!file_.write(chunkBuf_, error)) return false;
    index_.push_back(e);
    stats_.bytesWritten = file_.size();
    return true;
}

bool AviMjpegWriter::emitAudio(std::uint64_t upToFrame, std::string& error) {
    const std::uint64_t target = std::min(upToFrame, audioQueued_);
    if (!hasAudio_ || target <= audioEmitted_) return true;
    const std::size_t frames = static_cast<std::size_t>(target - audioEmitted_);
    const std::size_t bytes = frames * static_cast<std::size_t>(blockAlign_);
    if (!writeChunk("01wb", pendingPcm_.data(), bytes, error)) return false;
    pendingPcm_.erase(pendingPcm_.begin(), pendingPcm_.begin() + static_cast<std::ptrdiff_t>(bytes));
    audioEmitted_ = target;
    maxAudioChunk_ = std::max<std::uint32_t>(maxAudioChunk_, static_cast<std::uint32_t>(bytes));
    stats_.audioFrames = audioEmitted_;
    return true;
}

bool AviMjpegWriter::writeVideo(const std::uint8_t* bgra, int stride, std::int64_t, std::string& error) {
    if (closed_ || failed_) {
        error = "writer AVI fechado";
        return false;
    }
    if (!jpeg_->encode(bgra, stride, jpegBuf_)) {
        failed_ = true;
        error = "falha ao codificar quadro JPEG";
        return false;
    }
    if (!writeChunk("00dc", jpegBuf_.data(), jpegBuf_.size(), error)) {
        failed_ = true;
        return false;
    }
    maxVideoChunk_ = std::max<std::uint32_t>(maxVideoChunk_, static_cast<std::uint32_t>(jpegBuf_.size()));
    ++stats_.videoFrames;
    if (hasAudio_) {
        // audio due by the end of this frame period: round(frames * rate * den / num)
        const std::uint64_t due = (stats_.videoFrames * static_cast<std::uint64_t>(audio_.sampleRate) *
                                       static_cast<std::uint64_t>(video_.fpsDen) +
                                   static_cast<std::uint64_t>(video_.fpsNum) / 2) /
                                  static_cast<std::uint64_t>(video_.fpsNum);
        if (!emitAudio(due, error)) {
            failed_ = true;
            return false;
        }
    }
    return true;
}

bool AviMjpegWriter::writeAudio(const float* in, std::size_t frames, std::int64_t, std::string& error) {
    if (closed_ || failed_) {
        error = "writer AVI fechado";
        return false;
    }
    if (!hasAudio_ || frames == 0) return true;
    const std::size_t old = pendingPcm_.size();
    const std::size_t bytes = frames * static_cast<std::size_t>(blockAlign_);
    pendingPcm_.resize(old + bytes);
    detail::packPcm(in, frames * static_cast<std::size_t>(audio_.channels), 16, pendingPcm_.data() + old);
    audioQueued_ += frames;
    return true;
}

bool AviMjpegWriter::close(std::string& error) {
    if (closed_) return true;
    closed_ = true;
    std::string e;
    bool ok = true;
    if (!failed_) ok = emitAudio(audioQueued_, e);
    std::uint8_t v[4];
    if (ok) {
        // idx1
        detail::ByteBuf idx;
        idx.fourcc("idx1");
        idx.u32(static_cast<std::uint32_t>(index_.size() * 16));
        for (const auto& it : index_) {
            idx.bytes(it.id, 4);
            idx.u32(it.flags);
            idx.u32(it.offset);
            idx.u32(it.size);
        }
        // `movi` list size: from its fourcc to the end of the last chunk
        const std::uint64_t moviEnd = file_.size();
        ok = file_.write(idx.v, e);
        if (ok) {
            detail::putLe32(v, static_cast<std::uint32_t>(moviEnd - moviFourccPos_));
            ok = file_.patch(moviListSizePos_, v, 4, e);
        }
        const auto patch32 = [&](std::uint64_t pos, std::uint32_t x) {
            if (!ok || pos == 0) return;
            detail::putLe32(v, x);
            ok = file_.patch(pos, v, 4, e);
        };
        patch32(4, static_cast<std::uint32_t>(file_.size() - 8));
        patch32(avihTotalPos_, static_cast<std::uint32_t>(stats_.videoFrames));
        patch32(avihBufPos_, std::max(maxVideoChunk_, maxAudioChunk_));
        patch32(strhVLenPos_, static_cast<std::uint32_t>(stats_.videoFrames));
        patch32(strhVBufPos_, maxVideoChunk_);
        if (hasAudio_) {
            patch32(strhALenPos_, static_cast<std::uint32_t>(audioEmitted_));
            patch32(strhABufPos_, maxAudioChunk_);
        }
        const double seconds = stats_.videoFrames * static_cast<double>(video_.fpsDen) / video_.fpsNum;
        if (seconds > 0) patch32(avihRatePos_, static_cast<std::uint32_t>(static_cast<double>(moviEnd) / seconds));
    }
    stats_.bytesWritten = file_.size();
    std::string ce;
    if (!file_.close(ce) && e.empty()) e = ce;
    if (!e.empty()) error = e;
    return ok && e.empty();
}

}  // namespace hb::rec
