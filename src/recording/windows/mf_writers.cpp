#include "recording/windows/mf_writers.hpp"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
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

// Finds a matching (output type, input type) pair for `subtype` (e.g. MFAudioFormat_MP3) by
// asking a standalone instance of the encoder itself, rather than guessing one or handing it our
// own hand-built type: IMFSinkWriterEx::GetTransformForStream only exposes a SinkWriter stream's
// internal transform from *inside* a WriteSample call (confirmed on CI: fails immediately here,
// long before BeginWriting), so it cannot help discover anything up front, and SetOutputType()
// rejects a hand-built type even when the fields look right (also confirmed on CI) — some in-box
// encoders only accept one of their own exactly-enumerated GetOutputAvailableType() templates.
// So: enumerate the MFT's own output templates, override only rate/channels/bitrate on each
// (desiredAvgBytesPerSec, 0 = leave whatever the template already has), and take the first one
// SetOutputType() actually accepts; GetInputAvailableType() on that configured instance is then
// the exact PCM type it wants. Returns false (leaving `error` with every step's HRESULT) if
// nothing in the whole search works.
bool discoverEncoderTypes(const GUID& category, const GUID& subtype, UINT32 rate, UINT32 ch,
                          UINT32 desiredAvgBytesPerSec, ComPtr<IMFMediaType>& outType,
                          ComPtr<IMFMediaType>& inType, std::string& error) {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, subtype};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_ALL, nullptr, &outInfo, &acts, &n);
    if (FAILED(hr) || n == 0) {
        error = "nenhum codificador encontrado na enumeração";
        if (acts) CoTaskMemFree(acts);
        return false;
    }
    bool ok = false;
    for (UINT32 i = 0; i < n && !ok; ++i) {
        ComPtr<IMFTransform> mft;
        if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)))) { acts[i]->Release(); continue; }
        for (DWORD idx = 0; idx < 32 && !ok; ++idx) {
            ComPtr<IMFMediaType> cand;
            if (FAILED(mft->GetOutputAvailableType(0, idx, &cand))) break;
            cand->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            cand->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            if (desiredAvgBytesPerSec) cand->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, desiredAvgBytesPerSec);
            hr = mft->SetOutputType(0, cand.Get(), 0);
            if (FAILED(hr)) {
                char b[48];
                std::snprintf(b, sizeof b, "SetOutput#%lu=0x%08lX ", static_cast<unsigned long>(idx), static_cast<unsigned long>(hr));
                error += b;
                continue;
            }
            ComPtr<IMFMediaType> in;
            hr = mft->GetInputAvailableType(0, 0, &in);
            if (FAILED(hr)) {
                char b[48];
                std::snprintf(b, sizeof b, "GetInput#%lu=0x%08lX ", static_cast<unsigned long>(idx), static_cast<unsigned long>(hr));
                error += b;
                continue;
            }
            // Overriding only these two would leave MF_MT_AUDIO_BLOCK_ALIGNMENT and
            // MF_MT_AUDIO_AVG_BYTES_PER_SECOND stale (computed by the encoder for whatever
            // rate/channels it enumerated by default) — an internally inconsistent PCM type that
            // a real SinkWriter stream's stricter validation can reject even though the lenient
            // GetInputAvailableType() call above did not. Recompute both from the (possibly
            // template-default) bits-per-sample together with our actual rate/channels.
            UINT32 bits = 16;
            in->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (bits == 0) bits = 16;
            const UINT32 blockAlign = ch * (bits / 8);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * blockAlign);
            outType = cand;
            inType = in;
            ok = true;
        }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    return ok;
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
        std::string diag;   // every attempt's outcome, so a failure is diagnosable from CI logs alone

        // MP3 (the in-box encoder wraps a legacy ACM codec) rejects both a hand-built output type
        // (even though AddStream() itself accepted it) and a hand-built PCM input type — confirmed
        // on CI, not guessed. So for MP3, discover a matching (output, input) pair from a
        // standalone instance of the real encoder first (see discoverEncoderTypes): enumerate its
        // own GetOutputAvailableType() templates, only override rate/channels/bitrate on each, and
        // use the first one it actually accepts via SetOutputType — then its GetInputAvailableType
        // is the exact PCM shape it wants. AAC already works with a hand-built type (M4A passed on
        // every CI run so far), so it keeps that simpler path.
        ComPtr<IMFMediaType> out, in;
        if (kind_ == Kind::Mp3) {
            std::string derr;
            if (discoverEncoderTypes(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_MP3, rate, ch,
                                     mp3BytesPerSecond(p.audioBitrateKbps), out, in, derr)) {
                diag += "Discovered ";
            } else {
                diag += "Discover=[" + derr + "] ";
            }
        }
        if (!out) {
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
        }
        HRESULT hr = sw_->AddStream(out.Get(), &aStream_);
        if (FAILED(hr)) { error = hrText("codificador de áudio indisponível neste computador", hr) + " [" + diag + "]"; return false; }

        HRESULT last = E_FAIL;
        auto note = [&](const char* what, HRESULT h) {
            char b[96];
            std::snprintf(b, sizeof b, "%s=0x%08lX ", what, static_cast<unsigned long>(h));
            diag += b;
        };
        if (in) {
            last = sw_->SetInputMediaType(aStream_, in.Get(), nullptr);
            note("Discovered", last);
            if (SUCCEEDED(last)) { hr = last; goto inputTypeSet; }
        }
        // Fallback for anything the discovery above could not find/accept, or didn't apply to
        // (AAC): a few hand-built PCM shapes.
        {
            auto makeIn = [&](bool blockAlign, bool preferWfx) {
                ComPtr<IMFMediaType> t;
                MFCreateMediaType(&t);
                t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
                if (blockAlign) {
                    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * 2);
                    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * 2);
                }
                if (preferWfx) t->SetUINT32(MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1);
                return t;
            };
            for (const bool preferWfx : {false, true})
                for (const bool blockAlign : {true, false}) {
                    last = sw_->SetInputMediaType(aStream_, makeIn(blockAlign, preferWfx).Get(), nullptr);
                    note((std::string("Hand") + (preferWfx ? "W" : "w") + (blockAlign ? "B" : "b")).c_str(), last);
                    if (SUCCEEDED(last)) { hr = last; goto inputTypeSet; }
                }
        }
        hr = last;
    inputTypeSet:
        if (FAILED(hr)) { error = hrText("formato de entrada de áudio recusado", hr) + " [" + diag + "]"; return false; }
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

// Real end-to-end confirmation that MP3 recording actually works on this machine. Three
// successively deeper checks — a CLSID being registered (hasEncoder), and a standalone MFT
// instance accepting a negotiated (output, input) type pair (discoverEncoderTypes) — were each
// confirmed on CI to NOT predict whether a real SinkWriter stream will accept that same type
// (it kept refusing it with the identical HRESULT regardless). Rather than add a fourth guess,
// just run the real thing: open an actual MfWriter(Kind::Mp3) against a throwaway temp file,
// through the exact code path recording will use, and see if it genuinely works.
bool mp3ReallyWorks() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto path = fs::temp_directory_path(ec) / L"hb_mp3_probe_tmp.mp3";
    if (ec) return false;
    fs::remove(path, ec);
    bool ok;
    {
        MfWriter w(Kind::Mp3);
        RecordingProfile prof;
        prof.audioBitrateKbps = 192;
        std::string err;
        ok = w.open(path.string(), prof, VideoFormat{}, AudioFormat{48000, 2}, err);
        std::string ignore;
        w.close(ignore);
    }
    fs::remove(path, ec);
    return ok;
}

EncoderAvailability probeEncoders() {
    EncoderAvailability a;
    MfScope scope;
    if (!scope.mf) return a;
    a.h264 = hasEncoder(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_H264);
    a.hevc = hasEncoder(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_HEVC);
    a.aac = hasEncoder(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC);
    a.mp3 = mp3ReallyWorks();
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
