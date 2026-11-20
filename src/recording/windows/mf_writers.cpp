#include "recording/windows/mf_writers.hpp"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace hb::rec {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string hrText(const char* what, HRESULT hr) {
    char b[160];
    std::snprintf(b, sizeof b, "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

// RAII for COM + Media Foundation startup on the calling thread.
struct MfScope {
    bool com = false, mf = false;
    MfScope() {
        const HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(h);                       // RPC_E_CHANGED_MODE: already initialised differently, fine
        mf = SUCCEEDED(MFStartup(MF_VERSION));
    }
    ~MfScope() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

bool hasEncoder(const GUID& category, const GUID& majorType, const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO out{majorType, subtype};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_ALL, nullptr, &out, &acts, &n);
    if (SUCCEEDED(hr) && acts) {
        for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return SUCCEEDED(hr) && n > 0;
}

// The Windows AAC encoder only accepts these average byte rates for 44.1/48 kHz stereo.
UINT32 aacBytesPerSecond(int kbps) {
    static const int allowed[] = {96, 128, 160, 192};
    int best = allowed[0];
    for (int a : allowed)
        if (std::abs(a - kbps) < std::abs(best - kbps)) best = a;
    return static_cast<UINT32>(best * 125);   // kbps -> bytes/s
}

UINT32 mp3BytesPerSecond(int kbps) {
    static const int allowed[] = {128, 160, 192, 256, 320};
    int best = allowed[0];
    for (int a : allowed)
        if (std::abs(a - kbps) < std::abs(best - kbps)) best = a;
    return static_cast<UINT32>(best * 125);
}

enum class Kind { Mp4H264, Mp4Hevc, M4aAac, Mp3 };

class MfWriter final : public IMediaWriter {
public:
    explicit MfWriter(Kind k) : kind_(k) {}
    ~MfWriter() override {
        std::string e;
        close(e);
    }

    bool open(const std::string& pathUtf8, const RecordingProfile& profile, const VideoFormat& video, const AudioFormat& audio,
              std::string& error) override {
        if (opened_) {
            error = "gravador já aberto";
            return false;
        }
        const bool wantVideo = kind_ == Kind::Mp4H264 || kind_ == Kind::Mp4Hevc;
        if (wantVideo && !video.present()) { error = "formato de vídeo ausente"; return false; }
        if (wantVideo && (video.width % 2 || video.height % 2)) { error = "largura e altura do vídeo precisam ser pares"; return false; }
        if (!audio.present() || audio.channels < 1 || audio.channels > 2) { error = "áudio precisa ser mono ou estéreo"; return false; }
        if (kind_ != Kind::Mp3 && audio.sampleRate != 44100 && audio.sampleRate != 48000) {
            error = "o codificador AAC do Windows só aceita 44,1 ou 48 kHz";
            return false;
        }
        if (kind_ == Kind::Mp3 && audio.sampleRate != 44100 && audio.sampleRate != 48000) {
            error = "o codificador MP3 do Windows só aceita 44,1 ou 48 kHz";
            return false;
        }

        scope_ = std::make_unique<MfScope>();
        if (!scope_->mf) { error = "Media Foundation indisponível neste Windows"; return false; }
        path_ = pathUtf8;
        video_ = video;
        audio_ = audio;
        hasVideo_ = wantVideo;

        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 4);
        if (FAILED(hr)) { error = hrText("MFCreateAttributes", hr); return false; }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, kind_ == Kind::Mp3 ? MFTranscodeContainerType_MP3 : MFTranscodeContainerType_MPEG4);

        hr = MFCreateSinkWriterFromURL(widen(pathUtf8).c_str(), nullptr, attrs.Get(), &sw_);
        if (FAILED(hr)) { error = hrText("não foi possível criar o arquivo de saída", hr); return false; }

        if (wantVideo && !addVideoStream(profile, error)) return false;
        if (!addAudioStream(profile, error)) return false;
        hr = sw_->BeginWriting();
        if (FAILED(hr)) { error = hrText("falha ao iniciar a gravação (codificador indisponível?)", hr); return false; }
        opened_ = true;
        return true;
    }

    bool writeVideo(const std::uint8_t* bgra, int stride, std::int64_t ptsNs, std::string& error) override {
        if (!opened_ || !hasVideo_) { error = "gravador não está aberto para vídeo"; return false; }
        const int w = video_.width, h = video_.height;
        if (!bgra || stride < w * 4) { error = "quadro inválido"; return false; }
        const DWORD bytes = static_cast<DWORD>(w) * 4 * static_cast<DWORD>(h);
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
        if (FAILED(hr)) { error = hrText("memória do quadro", hr); return false; }
        BYTE* dst = nullptr;
        hr = buf->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) { error = hrText("buffer do quadro", hr); return false; }
        for (int y = 0; y < h; ++y) std::memcpy(dst + static_cast<std::size_t>(y) * w * 4, bgra + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(w) * 4);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ComPtr<IMFSample> smp;
        MFCreateSample(&smp);
        smp->AddBuffer(buf.Get());
        LONGLONG t = ptsNs / 100;
        if (t <= lastVideo_) t = lastVideo_ + 1;                // MF needs strictly increasing times
        lastVideo_ = t;
        smp->SetSampleTime(t);
        smp->SetSampleDuration(static_cast<LONGLONG>(10000000LL * video_.fpsDen / std::max(1, video_.fpsNum)));
        hr = sw_->WriteSample(vStream_, smp.Get());
        if (FAILED(hr)) { error = hrText("falha ao gravar um quadro de vídeo", hr); return false; }
        ++stats_.videoFrames;
        return true;
    }

    bool writeAudio(const float* in, std::size_t frames, std::int64_t ptsNs, std::string& error) override {
        if (!opened_) { error = "gravador não está aberto"; return false; }
        if (!in || frames == 0) return true;
        const std::size_t samples = frames * static_cast<std::size_t>(audio_.channels);
        const DWORD bytes = static_cast<DWORD>(samples * 2);
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
        if (FAILED(hr)) { error = hrText("memória do áudio", hr); return false; }
        BYTE* dst = nullptr;
        hr = buf->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) { error = hrText("buffer do áudio", hr); return false; }
        auto* o = reinterpret_cast<std::int16_t*>(dst);
        for (std::size_t i = 0; i < samples; ++i) {
            float v = in[i];
            if (!(v == v)) v = 0.f;                              // NaN guard
            v = std::clamp(v, -1.f, 1.f);
            o[i] = static_cast<std::int16_t>(std::lrintf(v * 32767.f));
        }
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ComPtr<IMFSample> smp;
        MFCreateSample(&smp);
        smp->AddBuffer(buf.Get());
        LONGLONG t = ptsNs / 100;
        if (t <= lastAudio_) t = lastAudio_ + 1;
        lastAudio_ = t;
        smp->SetSampleTime(t);
        smp->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000LL / audio_.sampleRate);
        hr = sw_->WriteSample(aStream_, smp.Get());
        if (FAILED(hr)) { error = hrText("falha ao gravar áudio", hr); return false; }
        stats_.audioFrames += frames;
        return true;
    }

    bool close(std::string& error) override {
        if (closed_) return true;
        closed_ = true;
        bool ok = true;
        if (sw_ && opened_) {
            const HRESULT hr = sw_->Finalize();
            if (FAILED(hr)) { error = hrText("falha ao finalizar o arquivo", hr); ok = false; }
        }
        sw_.Reset();
        scope_.reset();
        return ok;
    }

    WriterStats stats() const override {
        WriterStats s = stats_;
        std::error_code ec;
        const auto sz = std::filesystem::file_size(std::filesystem::path(std::u8string(path_.begin(), path_.end())), ec);
        if (!ec) s.bytesWritten = static_cast<std::uint64_t>(sz);
        return s;
    }

private:
    bool addVideoStream(const RecordingProfile& p, std::string& error) {
        const GUID sub = kind_ == Kind::Mp4Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, sub);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::clamp(p.videoBitrateKbps, 500, 100000)) * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(video_.width), static_cast<UINT32>(video_.height));
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(video_.fpsNum), static_cast<UINT32>(video_.fpsDen));
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (kind_ == Kind::Mp4H264) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        out->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING,
                       static_cast<UINT32>(std::max(1, p.keyframeIntervalSec) * std::max(1, video_.fpsNum / std::max(1, video_.fpsDen))));
        HRESULT hr = sw_->AddStream(out.Get(), &vStream_);
        if (FAILED(hr)) { error = hrText("codificador de vídeo indisponível neste computador", hr); return false; }

        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(video_.width), static_cast<UINT32>(video_.height));
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(video_.fpsNum), static_cast<UINT32>(video_.fpsDen));
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(video_.width * 4));   // positive = top-down rows
        hr = sw_->SetInputMediaType(vStream_, in.Get(), nullptr);
        if (FAILED(hr)) { error = hrText("formato de entrada de vídeo recusado", hr); return false; }
        return true;
    }

    bool addAudioStream(const RecordingProfile& p, std::string& error) {
        const UINT32 ch = static_cast<UINT32>(audio_.channels), rate = static_cast<UINT32>(audio_.sampleRate);
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        if (kind_ == Kind::Mp3) {
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, mp3BytesPerSecond(p.audioBitrateKbps));
            out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        } else {
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytesPerSecond(p.audioBitrateKbps));
            out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
            out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        }
        HRESULT hr = sw_->AddStream(out.Get(), &aStream_);
        if (FAILED(hr)) { error = hrText("codificador de áudio indisponível neste computador", hr); return false; }

        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 2);
        hr = sw_->SetInputMediaType(aStream_, in.Get(), nullptr);
        if (FAILED(hr)) { error = hrText("formato de entrada de áudio recusado", hr); return false; }
        return true;
    }

    Kind kind_;
    std::unique_ptr<MfScope> scope_;
    ComPtr<IMFSinkWriter> sw_;
    DWORD vStream_ = 0, aStream_ = 0;
    std::string path_;
    VideoFormat video_;
    AudioFormat audio_;
    bool hasVideo_ = false, opened_ = false, closed_ = false;
    LONGLONG lastVideo_ = -1, lastAudio_ = -1;
    WriterStats stats_;
};

}  // namespace

EncoderAvailability probeEncoders() {
    EncoderAvailability a;
    MfScope scope;
    if (!scope.mf) return a;
    a.h264 = hasEncoder(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_H264);
    a.hevc = hasEncoder(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_HEVC);
    a.aac = hasEncoder(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC);
    a.mp3 = hasEncoder(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_MP3);
    return a;
}

std::vector<std::string> registerWindowsWriters() {
    std::vector<std::string> ids;
    const EncoderAvailability a = probeEncoders();
    auto add = [&](RecordingProfile p, Kind k) {
        if (WriterRegistry::instance().add(p, [k] { return std::make_unique<MfWriter>(k); })) ids.push_back(p.id);
    };
    if (a.h264 && a.aac) {
        RecordingProfile p;
        p.id = "mp4-h264-aac";
        p.label = "MP4 — H.264 + AAC (recomendado)";
        p.description = "Compatível com praticamente tudo: YouTube, redes sociais, celulares e editores de vídeo.";
        p.extension = "mp4";
        p.kind = "video";
        p.videoBitrateKbps = 8000;
        p.audioBitrateKbps = 192;
        p.audioSampleRate = 48000;
        add(p, Kind::Mp4H264);
    }
    if (a.hevc && a.aac) {
        RecordingProfile p;
        p.id = "mp4-hevc-aac";
        p.label = "MP4 — H.265/HEVC + AAC (menor tamanho)";
        p.description = "Mesma qualidade com arquivos cerca de metade do tamanho. Exige codificador HEVC (placa de vídeo moderna).";
        p.extension = "mp4";
        p.kind = "video";
        p.videoBitrateKbps = 5000;
        p.audioBitrateKbps = 192;
        p.audioSampleRate = 48000;
        add(p, Kind::Mp4Hevc);
    }
    if (a.aac) {
        RecordingProfile p;
        p.id = "m4a-aac";
        p.label = "M4A — áudio AAC";
        p.description = "Somente áudio, compacto e de boa qualidade (podcasts, pregações, ensaios).";
        p.extension = "m4a";
        p.hasVideo = false;
        p.kind = "audio";
        p.audioBitrateKbps = 192;
        p.audioSampleRate = 48000;
        add(p, Kind::M4aAac);
    }
    if (a.mp3) {
        RecordingProfile p;
        p.id = "mp3";
        p.label = "MP3 — áudio";
        p.description = "Somente áudio no formato mais universal.";
        p.extension = "mp3";
        p.hasVideo = false;
        p.kind = "audio";
        p.audioBitrateKbps = 192;
        p.audioSampleRate = 48000;
        add(p, Kind::Mp3);
    }
    return ids;
}

}  // namespace hb::rec
