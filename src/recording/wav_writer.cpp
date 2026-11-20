#include "recording/wav_writer.hpp"

#include "recording/pcm.hpp"

namespace hb::rec {

namespace detail {

static std::uint32_t channelMask(int ch) {
    switch (ch) {
        case 1: return 0x4;      // FC
        case 2: return 0x3;      // FL FR
        case 3: return 0x7;      // FL FR FC
        case 4: return 0x33;     // FL FR BL BR
        case 5: return 0x37;     // FL FR FC BL BR
        case 6: return 0x3F;     // 5.1
        case 7: return 0x13F;    // 6.1
        case 8: return 0x63F;    // 7.1
        default: return 0;       // unspecified speaker positions
    }
}

std::vector<std::uint8_t> buildWaveFormat(int channels, int sampleRate, int bits) {
    ByteBuf b;
    const bool isFloat = bits == 32;
    const bool ext = bits == 24 || channels > 2 || (isFloat && channels > 2);
    const unsigned blockAlign = static_cast<unsigned>(channels * (bits / 8));
    b.u16(ext ? 0xFFFE : (isFloat ? 3 : 1));
    b.u16(static_cast<unsigned>(channels));
    b.u32(static_cast<std::uint32_t>(sampleRate));
    b.u32(static_cast<std::uint32_t>(sampleRate) * blockAlign);
    b.u16(blockAlign);
    b.u16(static_cast<unsigned>(bits));
    if (ext) {
        b.u16(22);                 // cbSize
        b.u16(static_cast<unsigned>(bits));  // wValidBitsPerSample
        b.u32(channelMask(channels));
        // SubFormat GUID: {0000000x-0000-0010-8000-00AA00389B71}
        b.u16(isFloat ? 3 : 1);
        b.u16(0);
        static const std::uint8_t tail[] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                            0x00, 0x38, 0x9B, 0x71};
        b.bytes(tail, sizeof(tail));
    } else if (isFloat) {
        b.u16(0);  // cbSize
    }
    return b.v;
}

}  // namespace detail

WavWriter::~WavWriter() {
    std::string e;
    close(e);
}

bool WavWriter::open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat&,
                     const AudioFormat& audio, std::string& error) {
    if (!closed_) {
        error = "writer ja aberto";
        return false;
    }
    if (!audio.present() || audio.channels > 255) {
        error = "formato de audio invalido para WAV";
        return false;
    }
    bits_ = detail::normalizeBits(profile.audioBitsPerSample);
    floatFmt_ = bits_ == 32;
    channels_ = audio.channels;
    stats_ = {};
    dataBytes_ = 0;
    failed_ = false;
    if (!file_.open(pathUtf8, error)) return false;

    detail::ByteBuf h;
    h.fourcc("RIFF");
    h.u32(0);  // patched at close
    h.fourcc("WAVE");
    const auto fmt = detail::buildWaveFormat(channels_, audio.sampleRate, bits_);
    h.fourcc("fmt ");
    h.u32(static_cast<std::uint32_t>(fmt.size()));
    h.bytes(fmt.data(), fmt.size());
    if (floatFmt_) {  // non-PCM formats must carry a `fact` chunk
        h.fourcc("fact");
        h.u32(4);
        factOffset_ = h.size();
        h.u32(0);
    }
    h.fourcc("data");
    dataSizeOffset_ = h.size();
    h.u32(0);
    if (!file_.write(h.v, error)) {
        std::string ignore;
        file_.close(ignore);
        return false;
    }
    stats_.bytesWritten = file_.size();
    closed_ = false;
    return true;
}

bool WavWriter::writeAudio(const float* interleaved, std::size_t frames, std::int64_t, std::string& error) {
    if (closed_ || failed_) {
        error = "writer WAV fechado";
        return false;
    }
    if (frames == 0) return true;
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels_) *
                                static_cast<std::uint64_t>(bits_ / 8);
    if (dataBytes_ + bytes > 0xFFFFFFFFull - 4096) {
        failed_ = true;
        error = "limite de 4 GB do WAV atingido";
        return false;
    }
    const std::size_t samples = frames * static_cast<std::size_t>(channels_);
    scratch_.resize(static_cast<std::size_t>(bytes));
    detail::packPcm(interleaved, samples, bits_, scratch_.data());
    if (!file_.write(scratch_.data(), scratch_.size(), error)) {
        failed_ = true;
        return false;
    }
    dataBytes_ += bytes;
    stats_.audioFrames += frames;
    stats_.bytesWritten = file_.size();
    return true;
}

bool WavWriter::close(std::string& error) {
    if (closed_) return true;
    closed_ = true;
    bool ok = true;  // best effort even after a failed write: leave a valid header when possible
    std::string e;
    if (ok && (dataBytes_ & 1)) {  // RIFF chunks are word aligned
        const std::uint8_t pad = 0;
        ok = file_.write(&pad, 1, e);
    }
    if (ok) {
        std::uint8_t v[4];
        detail::putLe32(v, static_cast<std::uint32_t>(dataBytes_));
        ok = file_.patch(dataSizeOffset_, v, 4, e);
        if (ok) {
            detail::putLe32(v, static_cast<std::uint32_t>(file_.size() - 8));
            ok = file_.patch(4, v, 4, e);
        }
        if (ok && floatFmt_) {
            detail::putLe32(v, static_cast<std::uint32_t>(stats_.audioFrames));
            ok = file_.patch(factOffset_, v, 4, e);
        }
    }
    stats_.bytesWritten = file_.size();
    std::string ce;
    if (!file_.close(ce) && e.empty()) e = ce;
    if (!e.empty()) error = e;
    return ok && e.empty();
}

}  // namespace hb::rec
