#include "streaming/windows/mf_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "streaming/windows/aac_asc.hpp"
#include "streaming/windows/mf_common.hpp"

// Real IMFTransform-driven AAC encoding. See mf_video_encoder.cpp's header note - same reasoning
// (MFT_CATEGORY_AUDIO_ENCODER directly, not IMFSinkWriter).
namespace hb::win {

using Microsoft::WRL::ComPtr;
using namespace mfcommon;

namespace {

constexpr UINT32 kAacBlockSize = 1024;  // AAC-LC's fixed frame size in samples/channel

// The Windows AAC encoder only accepts a handful of average byte rates for 44.1/48 kHz -
// mirrors src/recording/windows/mf_writers.cpp's aacBytesPerSecond() exactly (same in-box
// encoder, same constraint); duplicated rather than shared since mf_writers.cpp is out of scope.
UINT32 aacBytesPerSecond(int kbps) {
    static const int allowed[] = {96, 128, 160, 192};
    int best = allowed[0];
    for (int a : allowed)
        if (std::abs(a - kbps) < std::abs(best - kbps)) best = a;
    return static_cast<UINT32>(best) * 125;  // kbps -> bytes/s
}

// Finds any MFT_CATEGORY_AUDIO_ENCODER MFT that produces MFAudioFormat_AAC. There's normally just
// the one in-box Microsoft AAC encoder; unlike the H.264 side there's no `encoderPref` to honour
// here (IAacEncoder::start() takes none), so the first one enumerated is used.
bool pickAacActivate(ComPtr<IMFActivate>& chosen) {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, &outInfo, &acts, &n);
    if (FAILED(hr) || n == 0) {
        if (acts) CoTaskMemFree(acts);
        return false;
    }
    chosen = acts[0];
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return true;
}

class MfAacEncoder final : public IAacEncoder {
public:
    ~MfAacEncoder() override { stop(); }

    bool start(int sampleRate, int channels, int bitrateKbps) override {
        stop();
        error_.clear();
        if (sampleRate <= 0 || channels < 1 || channels > 2) {
            error_ = "Taxa de amostragem ou número de canais inválido para o codificador AAC.";
            return false;
        }
        sampleRate_ = sampleRate;
        channels_ = channels;

        scope_ = std::make_unique<MfScope>();
        if (!scope_->mf) {
            error_ = "Media Foundation indisponível neste Windows.";
            scope_.reset();
            return false;
        }

        ComPtr<IMFActivate> activate;
        if (!pickAacActivate(activate)) {
            error_ = "Nenhum codificador AAC disponível neste computador.";
            scope_.reset();
            return false;
        }

        HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&mft_));
        if (FAILED(hr) || !mft_) {
            error_ = hrText("Não foi possível iniciar o codificador AAC", hr);
            scope_.reset();
            return false;
        }

        if (!setupAsyncIfNeeded(mft_, isAsync_, events_)) {
            error_ = "Não foi possível preparar o codificador AAC assíncrono.";
            mft_.Reset();
            scope_.reset();
            return false;
        }

        if (!configureOutputType(bitrateKbps) || !configureInputType()) {
            mft_.Reset();
            events_.Reset();
            scope_.reset();
            return false;
        }

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        pending_.clear();
        framesEncoded_ = 0;
        ascBytes_.clear();
        haveAsc_ = false;
        pendingNeedInput_ = false;
        running_ = true;
        return true;
    }

    void stop() override {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_.Reset();
        }
        events_.Reset();
        scope_.reset();
        running_ = false;
        pendingNeedInput_ = false;
    }

    bool isRunning() const override { return running_; }

    bool encode(const float* interleaved, std::size_t frames, std::uint32_t timestampMs,
               const std::function<void(const EncodedAudioUnit&)>& onUnit) override {
        (void)timestampMs;  // AAC's fixed 1024-sample blocksize spans multiple encode() calls, so
                            // output timestamps are derived from the running sample count instead
                            // (see emitUnitsFromSample) - the caller's timestampMs is consistent
                            // with that running count anyway (audio is fed without gaps).
        if (!running_ || !mft_) {
            error_ = "Codificador AAC não está em execução.";
            return false;
        }
        if (!interleaved && frames > 0) {
            error_ = "Áudio inválido para o codificador AAC.";
            return false;
        }
        if (frames > 0) {
            const std::size_t samples = frames * static_cast<std::size_t>(channels_);
            pending_.insert(pending_.end(), interleaved, interleaved + samples);
        }

        while (pending_.size() >= static_cast<std::size_t>(kAacBlockSize) * static_cast<std::size_t>(channels_)) {
            const std::size_t blockSamples = static_cast<std::size_t>(kAacBlockSize) * static_cast<std::size_t>(channels_);
            ComPtr<IMFSample> sample = buildInputSample(pending_.data(), blockSamples);
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(blockSamples));
            if (!sample) return false;

            if (isAsync_) {
                if (!feedAsync(sample, onUnit)) return false;
            } else {
                HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
                if (hr == MF_E_NOTACCEPTING) {
                    if (!drainOutputSync(onUnit)) return false;
                    hr = mft_->ProcessInput(0, sample.Get(), 0);
                }
                if (FAILED(hr)) {
                    error_ = hrText("Falha ao enviar áudio ao codificador AAC", hr);
                    return false;
                }
                if (!drainOutputSync(onUnit)) return false;
            }
            framesEncoded_ += kAacBlockSize;
        }
        return true;
    }

    bool audioSpecificConfig(std::vector<std::uint8_t>& out) const override {
        if (!haveAsc_) return false;
        out = ascBytes_;
        return true;
    }

    std::string lastError() const override { return error_; }

private:
    bool configureOutputType(int bitrateKbps) {
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate_));
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels_));
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aacBytesPerSecond(bitrateKbps));
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                        // 0 = raw_data_block, no ADTS
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);   // AAC-LC - matches mf_writers.cpp
        const HRESULT hr = mft_->SetOutputType(0, out.Get(), 0);
        if (FAILED(hr)) {
            error_ = hrText("O codificador AAC recusou o formato de saída", hr);
            return false;
        }
        return true;
    }

    // Tries to get the MFT to accept float32 input directly (what IAacEncoder::encode() is handed,
    // so this avoids a conversion); falls back to 16-bit PCM - the shape mf_writers.cpp's
    // IMFSinkWriter-driven AAC stream already proves this exact in-box encoder accepts on this
    // machine class - if float32 isn't offered. Either way, `pcmFallback_` records which was used
    // so encode() knows whether to convert samples before building each input sample.
    bool configureInputType() {
        for (DWORD idx = 0;; ++idx) {
            ComPtr<IMFMediaType> cand;
            const HRESULT hr = mft_->GetInputAvailableType(0, idx, &cand);
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
            GUID sub{};
            if (FAILED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFAudioFormat_Float) continue;
            cand->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate_));
            cand->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels_));
            cand->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
            const UINT32 blockAlign = static_cast<UINT32>(channels_) * 4;
            cand->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
            cand->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(sampleRate_) * blockAlign);
            if (SUCCEEDED(mft_->SetInputType(0, cand.Get(), 0))) {
                pcmFallback_ = false;
                return true;
            }
        }
        // TODO(verify-on-windows): the MF AAC encoder MFT's GetInputAvailableType() enumeration is
        // not confirmed here to ever actually include a Float32 entry (vs. only ever offering
        // 16-bit PCM, which is what mf_writers.cpp's SinkWriter-mediated use of this same encoder
        // has proven works) - this PCM16 fallback path is accordingly the one most likely to be
        // exercised in practice. If `pcmFallback_` ends up permanently true in testing, that
        // confirms Float32 input isn't actually offered, which is useful to know but not a bug.
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate_));
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels_));
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(channels_) * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(sampleRate_) * static_cast<UINT32>(channels_) * 2);
        const HRESULT hr = mft_->SetInputType(0, in.Get(), 0);
        if (FAILED(hr)) {
            error_ = hrText("O codificador AAC recusou o formato de entrada", hr);
            return false;
        }
        pcmFallback_ = true;
        return true;
    }

    ComPtr<IMFSample> buildInputSample(const float* interleaved, std::size_t samples) {
        const std::size_t frames = samples / static_cast<std::size_t>(channels_);
        const DWORD bytes = static_cast<DWORD>(samples * (pcmFallback_ ? 2 : 4));
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
        if (FAILED(hr)) {
            error_ = hrText("memória do áudio", hr);
            return nullptr;
        }
        BYTE* dst = nullptr;
        hr = buf->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) {
            error_ = hrText("acesso ao buffer de áudio", hr);
            return nullptr;
        }
        if (pcmFallback_) {
            auto* o = reinterpret_cast<std::int16_t*>(dst);
            for (std::size_t i = 0; i < samples; ++i) {
                float v = interleaved[i];
                if (!(v == v)) v = 0.f;  // NaN guard
                v = std::clamp(v, -1.f, 1.f);
                o[i] = static_cast<std::int16_t>(std::lrintf(v * 32767.f));
            }
        } else {
            std::memcpy(dst, interleaved, bytes);
        }
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        const LONGLONG t = static_cast<LONGLONG>(framesEncoded_) * 10000000LL / sampleRate_;
        sample->SetSampleTime(t);
        sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000LL / sampleRate_);
        return sample;
    }

    enum class OutputStep { kProduced, kNeedMoreInput, kError };

    OutputStep processOneOutput(const std::function<void(const EncodedAudioUnit&)>& onUnit) {
        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(0, &info);
        const bool mftProvidesSample = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        ComPtr<IMFSample> outSample;
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        if (!mftProvidesSample) {
            DWORD sz = info.cbSize;
            if (sz == 0) sz = 8192;  // one 1024-sample AAC frame is always far smaller than this
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(sz, &buf))) {
                error_ = "Memória insuficiente para o áudio codificado AAC.";
                return OutputStep::kError;
            }
            MFCreateSample(&outSample);
            outSample->AddBuffer(buf.Get());
            outBuf.pSample = outSample.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &outBuf, &status);
        if (outBuf.pEvents) outBuf.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return OutputStep::kNeedMoreInput;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            HRESULT h2 = mft_->GetOutputAvailableType(0, 0, &t);
            if (SUCCEEDED(h2)) h2 = mft_->SetOutputType(0, t.Get(), 0);
            if (FAILED(h2)) {
                error_ = hrText("Falha ao renegociar o formato de saída AAC", h2);
                return OutputStep::kError;
            }
            return OutputStep::kProduced;
        }
        if (FAILED(hr)) {
            error_ = hrText("Falha ao obter o áudio codificado AAC", hr);
            return OutputStep::kError;
        }
        if (mftProvidesSample) outSample.Attach(outBuf.pSample);
        captureAscOnce();
        emitUnitFromSample(outSample.Get(), onUnit);
        return OutputStep::kProduced;
    }

    bool drainOutputSync(const std::function<void(const EncodedAudioUnit&)>& onUnit) {
        for (;;) {
            switch (processOneOutput(onUnit)) {
                case OutputStep::kProduced: continue;
                case OutputStep::kNeedMoreInput: return true;
                case OutputStep::kError: return false;
            }
        }
    }

    bool feedAsync(const ComPtr<IMFSample>& sample, const std::function<void(const EncodedAudioUnit&)>& onUnit) {
        if (!pumpAsyncEventsNonBlocking(
                events_, error_, [this] { pendingNeedInput_ = true; return true; },
                [this, &onUnit] { return processOneOutput(onUnit) != OutputStep::kError; }))
            return false;

        bool fed = false;
        if (pendingNeedInput_) {
            const HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
            if (FAILED(hr)) {
                error_ = hrText("Falha ao enviar áudio ao codificador AAC", hr);
                return false;
            }
            pendingNeedInput_ = false;
            fed = true;
        }
        // See mf_video_encoder.cpp's identical TODO(verify-on-windows) on its own bounded poll -
        // same reasoning; an async AAC MFT is unusual but not impossible (a hardware audio DSP).
        for (int attempt = 0; !fed && attempt < 50; ++attempt) {
            ComPtr<IMFMediaEvent> ev;
            HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) {
                error_ = hrText("Falha ao processar eventos do codificador AAC", hr);
                return false;
            }
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                hr = mft_->ProcessInput(0, sample.Get(), 0);
                if (FAILED(hr)) {
                    error_ = hrText("Falha ao enviar áudio ao codificador AAC", hr);
                    return false;
                }
                fed = true;
            } else if (met == METransformHaveOutput) {
                if (processOneOutput(onUnit) == OutputStep::kError) return false;
            }
        }
        if (!fed) {
            error_ = "O codificador AAC não respondeu a tempo.";
            return false;
        }
        return pumpAsyncEventsNonBlocking(
            events_, error_, [this] { pendingNeedInput_ = true; return true; },
            [this, &onUnit] { return processOneOutput(onUnit) != OutputStep::kError; });
    }

    void captureAscOnce() {
        if (haveAsc_) return;
        ComPtr<IMFMediaType> outType;
        if (FAILED(mft_->GetOutputCurrentType(0, &outType)) || !outType) return;
        UINT32 size = 0;
        if (FAILED(outType->GetBlobSize(MF_MT_USER_DATA, &size)) || size == 0) return;
        std::vector<std::uint8_t> blob(size);
        if (FAILED(outType->GetBlob(MF_MT_USER_DATA, blob.data(), size, nullptr))) return;
        if (aac::extractAudioSpecificConfig(blob, ascBytes_)) haveAsc_ = true;
    }

    void emitUnitFromSample(IMFSample* sample, const std::function<void(const EncodedAudioUnit&)>& onUnit) {
        if (!sample) return;
        ComPtr<IMFMediaBuffer> contig;
        if (FAILED(sample->ConvertToContiguousBuffer(&contig)) || !contig) return;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(contig->Lock(&data, nullptr, &len))) return;
        EncodedAudioUnit out;
        out.raw.assign(data, data + len);
        contig->Unlock();

        LONGLONG outTime = 0;
        if (SUCCEEDED(sample->GetSampleTime(&outTime)) && outTime >= 0) {
            out.timestampMs = static_cast<std::uint32_t>(outTime / 10000);
        } else {
            // Falls back to the running sample-count-derived timestamp of the block this output
            // corresponds to (the block just fed, since AAC MFTs emit one output per input block
            // with no reordering) rather than the caller's encode() timestampMs, matching this
            // encoder's internal blocking (see encode()'s note).
            out.timestampMs = static_cast<std::uint32_t>(static_cast<std::uint64_t>(framesEncoded_) * 1000ULL / static_cast<std::uint32_t>(sampleRate_));
        }
        onUnit(out);
    }

    int sampleRate_ = 48000, channels_ = 2;
    std::unique_ptr<MfScope> scope_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    bool isAsync_ = false;
    bool pendingNeedInput_ = false;
    bool running_ = false;
    bool pcmFallback_ = true;
    std::vector<float> pending_;        // leftover interleaved samples not yet forming a full 1024-frame block
    std::uint64_t framesEncoded_ = 0;    // total sample frames fed into the MFT so far (not counting `pending_`)
    std::vector<std::uint8_t> ascBytes_;
    bool haveAsc_ = false;
    std::string error_;
};

}  // namespace

std::unique_ptr<IAacEncoder> makeAacEncoder() { return std::make_unique<MfAacEncoder>(); }

}  // namespace hb::win
