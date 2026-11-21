#pragma once
// Internal, header-only helpers shared by mf_video_encoder.cpp / mf_audio_encoder.cpp. Not part of
// the frozen mf_encoder.hpp contract - purely an implementation detail of the two .cpp files that
// include it. Everything here is `inline` so it's safe for both translation units to include it.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

namespace hb::win::mfcommon {

using Microsoft::WRL::ComPtr;

// RAII for COM + Media Foundation startup on the calling thread - same pattern already used by
// src/recording/windows/mf_writers.cpp's MfScope and src/app/windows/dshow_capture.cpp's
// CoInitializeEx/CoUninitialize pairing in DShowSource::run(): initialise COM (COINIT_MULTITHREADED)
// and MF per-thread, for the lifetime of one encoder instance's start()/stop() cycle, rather than
// once globally for the process.
struct MfScope {
    bool com = false, mf = false;
    MfScope() {
        const HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(h);  // RPC_E_CHANGED_MODE: already initialised differently elsewhere, fine
        mf = SUCCEEDED(MFStartup(MF_VERSION));
    }
    ~MfScope() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

inline std::string hrText(const char* what, HRESULT hr) {
    char b[192];
    std::snprintf(b, sizeof b, "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

inline std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::string narrowUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Checks MF_TRANSFORM_ASYNC on `mft`'s attribute store and, if set, unlocks asynchronous operation
// (MF_TRANSFORM_ASYNC_UNLOCK) and hands back its IMFMediaEventGenerator. Required before touching
// an async MFT's ProcessInput/ProcessOutput at all - calling them without unlocking first is
// documented to fail. Hardware encoder MFTs (NVENC/QSV/AMF) are commonly asynchronous; the in-box
// software H.264/AAC encoders are not, so this is a no-op (isAsync left false) for those.
inline bool setupAsyncIfNeeded(const ComPtr<IMFTransform>& mft, bool& isAsync, ComPtr<IMFMediaEventGenerator>& events) {
    isAsync = false;
    events.Reset();
    ComPtr<IMFAttributes> attrs;
    if (FAILED(mft->GetAttributes(&attrs)) || !attrs) return true;  // no attribute store: treat as sync
    UINT32 async = 0;
    attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
    if (!async) return true;
    const HRESULT hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    if (FAILED(hr)) return false;
    isAsync = true;
    return SUCCEEDED(mft.As(&events));
}

// Drains every event already queued on `events` (never blocks): calls `onNeedInput` once per
// METransformNeedInput and `onHaveOutput` once per METransformHaveOutput, stopping and returning
// false the moment either returns false. Other event types are ignored.
template <typename NeedInputFn, typename HaveOutputFn>
inline bool pumpAsyncEventsNonBlocking(const ComPtr<IMFMediaEventGenerator>& events, std::string& error,
                                       NeedInputFn&& onNeedInput, HaveOutputFn&& onHaveOutput) {
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
        if (FAILED(hr)) {
            error = hrText("Falha ao processar eventos do codificador", hr);
            return false;
        }
        MediaEventType met = MEUnknown;
        ev->GetType(&met);
        if (met == METransformNeedInput) {
            if (!onNeedInput()) return false;
        } else if (met == METransformHaveOutput) {
            if (!onHaveOutput()) return false;
        }
    }
}

}  // namespace hb::win::mfcommon
