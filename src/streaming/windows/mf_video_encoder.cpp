#include "streaming/windows/mf_encoder.hpp"

#include <codecapi.h>
#include <icodecapi.h>

#include <algorithm>
#include <cstring>

#include "streaming/windows/mf_common.hpp"
#include "streaming/windows/nal_utils.hpp"

// Real IMFTransform-driven H.264 encoding. See mf_encoder.hpp's header comment for why this talks
// to MFT_CATEGORY_VIDEO_ENCODER directly instead of going through IMFSinkWriter like
// src/recording/windows/mf_writers.cpp's (different) H.264 encoder does.
namespace hb::win {

using Microsoft::WRL::ComPtr;
using namespace mfcommon;

namespace {

std::string classifyEncoderName(const std::string& friendlyNameLower) {
    if (friendlyNameLower.find("nvenc") != std::string::npos || friendlyNameLower.find("nvidia") != std::string::npos) return "nvenc";
    if (friendlyNameLower.find("quick sync") != std::string::npos || friendlyNameLower.find("intel") != std::string::npos) return "qsv";
    if (friendlyNameLower.find("amd") != std::string::npos || friendlyNameLower.find("amf") != std::string::npos) return "amf";
    return "x264";  // Microsoft's in-box software H.264 MFT, or anything else unrecognised
}

bool prefMatches(const std::string& prefLower, const std::string& nameLower) {
    if (prefLower == "nvenc") return nameLower.find("nvenc") != std::string::npos || nameLower.find("nvidia") != std::string::npos;
    if (prefLower == "qsv") return nameLower.find("quick sync") != std::string::npos || nameLower.find("intel") != std::string::npos;
    if (prefLower == "amf") return nameLower.find("amd") != std::string::npos || nameLower.find("amf") != std::string::npos;
    // "auto" / "x264" / anything else: prefer the known-working software encoder.
    return nameLower.find("microsoft") != std::string::npos || nameLower.find("x264") != std::string::npos;
}

// Enumerates MFT_CATEGORY_VIDEO_ENCODER MFTs that produce H.264, picks the one matching
// `encoderPref` by friendly-name substring, and falls back to whichever is first/available if the
// preference isn't present - never fails outright just because a *specific* encoder is missing.
bool pickEncoderActivate(const std::string& encoderPref, ComPtr<IMFActivate>& chosen, std::string& chosenKind) {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    // MFT_ENUM_FLAG_HARDWARE picks up GPU encoders (nvenc/qsv/amf, when their driver registers an
    // MFT); MFT_ENUM_FLAG_SYNCMFT picks up the in-box Microsoft software H.264 encoder, which is
    // NOT flagged HARDWARE. SORTANDFILTER ranks results (hardware first) and drops duplicates.
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                                 &outInfo, &acts, &n);
    if (FAILED(hr) || n == 0) {
        if (acts) CoTaskMemFree(acts);
        return false;
    }

    const std::string prefLower = toLowerAscii(encoderPref);
    int bestIdx = -1;
    std::string bestNameLower;
    for (UINT32 i = 0; i < n && bestIdx < 0; ++i) {
        WCHAR* nm = nullptr;
        UINT32 len = 0;
        std::string nameLower;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm) {
            nameLower = toLowerAscii(narrowUtf8(nm));
            CoTaskMemFree(nm);
        }
        if (prefMatches(prefLower, nameLower)) {
            bestIdx = static_cast<int>(i);
            bestNameLower = nameLower;
        }
    }
    if (bestIdx < 0) {
        // Preferred encoder not present: fall back to whatever IS available (index 0, the
        // highest-ranked after SORTANDFILTER) rather than failing.
        bestIdx = 0;
        WCHAR* nm = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm) {
            bestNameLower = toLowerAscii(narrowUtf8(nm));
            CoTaskMemFree(nm);
        }
    }
    chosen = acts[static_cast<UINT32>(bestIdx)];  // ComPtr::operator= AddRefs
    chosenKind = classifyEncoderName(bestNameLower);
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return true;
}

class MfH264Encoder final : public IH264Encoder {
public:
    ~MfH264Encoder() override { stop(); }

    bool start(int width, int height, int fpsNum, int fpsDen, int bitrateKbps, const std::string& encoderPref) override {
        stop();
        error_.clear();
        if (width <= 0 || height <= 0 || fpsNum <= 0 || fpsDen <= 0) {
            error_ = "Resolução ou taxa de quadros inválida para o codificador H.264.";
            return false;
        }
        width_ = width;
        height_ = height;
        fpsNum_ = fpsNum;
        fpsDen_ = fpsDen;

        scope_ = std::make_unique<MfScope>();
        if (!scope_->mf) {
            error_ = "Media Foundation indisponível neste Windows.";
            scope_.reset();
            return false;
        }

        ComPtr<IMFActivate> activate;
        std::string pickedKind;
        if (!pickEncoderActivate(encoderPref, activate, pickedKind)) {
            error_ = "Nenhum codificador H.264 disponível neste computador.";
            scope_.reset();
            return false;
        }

        HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&mft_));
        if (FAILED(hr) || !mft_) {
            error_ = hrText("Não foi possível iniciar o codificador H.264 escolhido", hr);
            scope_.reset();
            return false;
        }
        activeName_ = pickedKind;

        if (!setupAsyncIfNeeded(mft_, isAsync_, events_)) {
            error_ = "Não foi possível preparar o codificador H.264 assíncrono (NVENC/Quick Sync/AMF).";
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
        configureGopBestEffort();
        configureLowLatencyBestEffort();

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        sps_.clear();
        pps_.clear();
        sawSps_ = sawPps_ = false;
        pendingNeedInput_ = false;
        lastInputTimestampMs_ = 0;
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

    bool encode(const std::uint8_t* bgra, int width, int height, int stride, std::uint32_t timestampMs,
               const std::function<void(const EncodedVideoUnit&)>& onUnit) override {
        if (!running_ || !mft_) {
            error_ = "Codificador H.264 não está em execução.";
            return false;
        }
        if (!bgra || width != width_ || height != height_ || stride < width * 4) {
            error_ = "Quadro de vídeo inválido para o codificador H.264.";
            return false;
        }
        lastInputTimestampMs_ = timestampMs;
        ComPtr<IMFSample> sample = buildInputSample(bgra, stride, timestampMs);
        if (!sample) return false;

        if (isAsync_) return encodeAsync(sample, onUnit);

        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!drainOutputSync(onUnit)) return false;
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            error_ = hrText("Falha ao enviar o quadro ao codificador H.264", hr);
            return false;
        }
        return drainOutputSync(onUnit);
    }

    bool spsPps(std::vector<std::uint8_t>& sps, std::vector<std::uint8_t>& pps) const override {
        if (!sawSps_ || !sawPps_) return false;
        sps = sps_;
        pps = pps_;
        return true;
    }

    std::string activeEncoderName() const override { return activeName_; }
    std::string lastError() const override { return error_; }

private:
    bool configureOutputType(int bitrateKbps) {
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(std::max(1, bitrateKbps)) * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fpsNum_), static_cast<UINT32>(fpsDen_));
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // Main, not High: broad compatibility with RTMP destinations/players, deliberately
        // different from mf_writers.cpp's local-MP4 writer, which prefers High since a local file
        // is never pickier than a live RTMP player/relay about the profile used.
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        out->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, gopFrames());
        const HRESULT hr = mft_->SetOutputType(0, out.Get(), 0);
        if (FAILED(hr)) {
            error_ = hrText("O codificador H.264 recusou o formato de saída", hr);
            return false;
        }
        return true;
    }

    bool configureInputType() {
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fpsNum_), static_cast<UINT32>(fpsDen_));
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width_) * 4);  // positive = top-down rows
        const HRESULT hr = mft_->SetInputType(0, in.Get(), 0);
        if (FAILED(hr)) {
            // TODO(verify-on-windows): the in-box Microsoft software H.264 MFT is known to accept
            // RGB32 input directly (mf_writers.cpp's IMFSinkWriter-driven H.264 stream proves this
            // on this same machine class, via the same underlying MFT). Whether hardware encoder
            // MFTs (NVENC/Quick Sync/AMF) also accept RGB32 directly, vs. requiring NV12 and an
            // internal/explicit colour-conversion step, is NOT confirmed here - if start() starts
            // failing specifically with a hardware `encoderPref` on real hardware, this is the
            // first place to look (add an NV12 conversion + MFVideoFormat_NV12 input type as a
            // fallback).
            error_ = hrText("O codificador H.264 recusou o formato de entrada (BGRA)", hr);
            return false;
        }
        return true;
    }

    UINT32 gopFrames() const {
        const int fps = std::max(1, fpsNum_ / std::max(1, fpsDen_));
        return static_cast<UINT32>(fps * 2);  // ~2 seconds between keyframes
    }

    void configureGopBestEffort() {
        ComPtr<ICodecAPI> codec;
        if (FAILED(mft_.As(&codec)) || !codec) return;  // many MFTs (esp. hardware ones) don't implement ICodecAPI at all
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = gopFrames();
        codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);  // best-effort; ignored/rejected by many MFTs, fine either way
        VariantClear(&v);
    }

    // Tunes the encoder for live streaming latency rather than offline-encode efficiency. None of
    // this is required to produce valid H.264 - it is why a live stream can visibly lag or stutter
    // even when the network and CPU both have headroom:
    //  - AVLowLatencyMode / AVEncCommonLowLatency: tells the MFT to skip internal look-ahead
    //    buffering (several encoders, hardware ones especially, buffer a handful of frames ahead
    //    by default to improve compression, which shows up as end-to-end delay on a live stream).
    //  - AVEncCommonRateControlMode = CBR: a steady bitrate the network path can pace against,
    //    instead of VBR bursts that can momentarily exceed the upload link and force buffering
    //    downstream (on the platform's ingest side or the viewer's player).
    //  - AVEncMPVDefaultBPictureCount = 0: B-frames require buffering future frames to encode a
    //    past one out of order, which is pure added latency with no benefit for a live source.
    //  - AVEncCommonRealTime: tells the encoder it must keep up with a live clock rather than
    //    prioritizing quality, so it degrades quality under load instead of falling behind and
    //    accumulating a backlog (which looks exactly like "lag e travamento").
    // Every SetValue here is independently best-effort: hardware MFTs commonly reject or ignore
    // properties they don't implement, and that must never fail the whole encoder.
    void configureLowLatencyBestEffort() {
        ComPtr<ICodecAPI> codec;
        if (FAILED(mft_.As(&codec)) || !codec) return;
        auto setBool = [&](const GUID& key, bool val) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL;
            v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
            codec->SetValue(&key, &v);
            VariantClear(&v);
        };
        auto setU32 = [&](const GUID& key, ULONG val) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = val;
            codec->SetValue(&key, &v);
            VariantClear(&v);
        };
        setBool(CODECAPI_AVLowLatencyMode, true);
        setBool(CODECAPI_AVEncCommonLowLatency, true);
        setBool(CODECAPI_AVEncCommonRealTime, true);
        setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    ComPtr<IMFSample> buildInputSample(const std::uint8_t* bgra, int stride, std::uint32_t timestampMs) {
        const DWORD bytes = static_cast<DWORD>(width_) * 4 * static_cast<DWORD>(height_);
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
        if (FAILED(hr)) {
            error_ = hrText("memória do quadro de vídeo", hr);
            return nullptr;
        }
        BYTE* dst = nullptr;
        hr = buf->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) {
            error_ = hrText("acesso ao buffer do quadro de vídeo", hr);
            return nullptr;
        }
        for (int y = 0; y < height_; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) * 4,
                       bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride), static_cast<std::size_t>(width_) * 4);
        }
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(timestampMs) * 10000);
        sample->SetSampleDuration(10000000LL * fpsDen_ / fpsNum_);
        return sample;
    }

    bool renegotiateOutputType() {
        ComPtr<IMFMediaType> t;
        HRESULT hr = mft_->GetOutputAvailableType(0, 0, &t);
        if (SUCCEEDED(hr)) hr = mft_->SetOutputType(0, t.Get(), 0);
        if (FAILED(hr)) {
            error_ = hrText("Falha ao renegociar o formato de saída H.264", hr);
            return false;
        }
        return true;
    }

    enum class OutputStep { kProduced, kNeedMoreInput, kError };

    // Pulls exactly one ready output sample (if any) via a single ProcessOutput call and emits its
    // NAL units. kProduced also covers a handled MF_E_TRANSFORM_STREAM_CHANGE (renegotiated, caller
    // should call this again to actually get the sample) so both callers below can just loop on it.
    OutputStep processOneOutput(const std::function<void(const EncodedVideoUnit&)>& onUnit) {
        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(0, &info);
        const bool mftProvidesSample = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        ComPtr<IMFSample> outSample;
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        if (!mftProvidesSample) {
            DWORD sz = info.cbSize;
            if (sz == 0) sz = static_cast<DWORD>(width_) * static_cast<DWORD>(height_) * 2 + 65536;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(sz, &buf))) {
                error_ = "Memória insuficiente para o quadro codificado H.264.";
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
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) return renegotiateOutputType() ? OutputStep::kProduced : OutputStep::kError;
        if (FAILED(hr)) {
            error_ = hrText("Falha ao obter o quadro codificado H.264", hr);
            return OutputStep::kError;
        }
        if (mftProvidesSample) outSample.Attach(outBuf.pSample);
        emitUnitsFromSample(outSample.Get(), onUnit);
        return OutputStep::kProduced;
    }

    bool drainOutputSync(const std::function<void(const EncodedVideoUnit&)>& onUnit) {
        for (;;) {
            switch (processOneOutput(onUnit)) {
                case OutputStep::kProduced: continue;     // more may be ready (or a stream-change retry) - keep draining
                case OutputStep::kNeedMoreInput: return true;
                case OutputStep::kError: return false;
            }
        }
    }

    bool encodeAsync(const ComPtr<IMFSample>& sample, const std::function<void(const EncodedVideoUnit&)>& onUnit) {
        if (!pumpAsyncEventsNonBlocking(
                events_, error_, [this] { pendingNeedInput_ = true; return true; },
                [this, &onUnit] { return processOneOutput(onUnit) != OutputStep::kError; }))
            return false;

        bool fed = false;
        if (pendingNeedInput_) {
            const HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
            if (FAILED(hr)) {
                error_ = hrText("Falha ao enviar o quadro ao codificador H.264", hr);
                return false;
            }
            pendingNeedInput_ = false;
            fed = true;
        }
        // TODO(verify-on-windows): async hardware encoder MFTs (NVENC/Quick Sync/AMF) are expected
        // to signal METransformNeedInput promptly after NOTIFY_BEGIN_STREAMING and after each prior
        // ProcessInput. This bounded poll (rather than an indefinite blocking GetEvent call) avoids
        // hanging the capture thread if a given MFT never signals it; the cost, if that ever
        // happens in practice, is dropping this one frame rather than hanging - watch for
        // intermittently dropped frames specifically with a hardware `encoderPref` on real
        // hardware as the symptom that this bound needs revisiting (e.g. raising the retry count
        // or switching to a dedicated encoder thread with a real blocking wait).
        for (int attempt = 0; !fed && attempt < 50; ++attempt) {
            ComPtr<IMFMediaEvent> ev;
            HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) {
                error_ = hrText("Falha ao processar eventos do codificador H.264", hr);
                return false;
            }
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                hr = mft_->ProcessInput(0, sample.Get(), 0);
                if (FAILED(hr)) {
                    error_ = hrText("Falha ao enviar o quadro ao codificador H.264", hr);
                    return false;
                }
                fed = true;
            } else if (met == METransformHaveOutput) {
                if (processOneOutput(onUnit) == OutputStep::kError) return false;
            }
        }
        if (!fed) {
            error_ = "O codificador H.264 não respondeu a tempo (NVENC/Quick Sync/AMF ocupado ou indisponível).";
            return false;
        }
        return pumpAsyncEventsNonBlocking(
            events_, error_, [this] { pendingNeedInput_ = true; return true; },
            [this, &onUnit] { return processOneOutput(onUnit) != OutputStep::kError; });
    }

    void emitUnitsFromSample(IMFSample* sample, const std::function<void(const EncodedVideoUnit&)>& onUnit) {
        if (!sample) return;
        ComPtr<IMFMediaBuffer> contig;
        if (FAILED(sample->ConvertToContiguousBuffer(&contig)) || !contig) return;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(contig->Lock(&data, nullptr, &len))) return;
        auto nalus = nal::splitAnnexB(data, len);
        contig->Unlock();

        UINT32 cleanPoint = 0;
        const bool haveCleanPoint = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint));
        bool sampleIsKey = haveCleanPoint && cleanPoint != 0;
        if (!haveCleanPoint) {
            for (auto& u : nalus) {
                if (nal::nalUnitType(u) == nal::kNalTypeIdrSlice) {
                    sampleIsKey = true;
                    break;
                }
            }
        }

        // Prefer the output sample's own time if the MFT set one; otherwise reuse the input
        // timestamp fed for the frame that produced it (encoders here aren't expected to reorder
        // frames (no B-frames), so the two should coincide in practice anyway).
        std::uint32_t tsMs = lastInputTimestampMs_;
        LONGLONG outTime = 0;
        if (SUCCEEDED(sample->GetSampleTime(&outTime)) && outTime >= 0) tsMs = static_cast<std::uint32_t>(outTime / 10000);

        for (auto& u : nalus) {
            const int type = nal::nalUnitType(u);
            if (type == nal::kNalTypeSps) {
                sps_ = u;
                sawSps_ = true;
            } else if (type == nal::kNalTypePps) {
                pps_ = u;
                sawPps_ = true;
            }
            EncodedVideoUnit out;
            out.nalu = std::move(u);
            out.keyframe = sampleIsKey;
            out.timestampMs = tsMs;
            onUnit(out);
        }
    }

    int width_ = 0, height_ = 0, fpsNum_ = 1, fpsDen_ = 1;
    std::unique_ptr<MfScope> scope_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    bool isAsync_ = false;
    bool pendingNeedInput_ = false;
    bool running_ = false;
    std::vector<std::uint8_t> sps_, pps_;
    bool sawSps_ = false, sawPps_ = false;
    std::uint32_t lastInputTimestampMs_ = 0;
    std::string activeName_;
    std::string error_;
};

}  // namespace

std::unique_ptr<IH264Encoder> makeH264Encoder() { return std::make_unique<MfH264Encoder>(); }

}  // namespace hb::win
