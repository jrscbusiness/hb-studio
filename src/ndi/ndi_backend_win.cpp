// Real NDI backend: Windows-only, dynamic loading of whatever NDI Runtime the user has installed
// (free download at ndi.video/tools). HB Studio never ships, statically links, or builds against
// the NDI SDK (see LICENSES.md) -- everything below is HB Studio's own reconstruction of NDI's
// publicly documented, stable binary interface: struct layouts and exported symbol names, never
// NDI's own source code or header comments.
//
// Public documentation consulted while writing this file:
//   - https://docs.ndi.video/all/developing-with-ndi/sdk/dynamic-loading-of-ndi-libraries
//     (confirms the dynamic-loading model and the DLL name `Processing.NDI.Lib.x64.dll`)
//   - https://docs.ndi.video/all/developing-with-ndi/sdk/platform-considerations
//   - https://docs.rs/ndi-sys (Rust bindings generated with `bindgen` directly against the real
//     NDI SDK headers; used ONLY to cross-check struct field order and the numeric values below --
//     e.g. NDIlib_recv_color_format_BGRX_BGRA == 0, NDIlib_recv_bandwidth_highest == 100,
//     NDIlib_frame_type_video == 1, NDIlib_frame_type_status_change == 100). The FourCC values were
//     independently re-derived from NDI's own documented packing
//     (`NDIlib_FourCC_type_make(a,b,c,d) = a | b<<8 | c<<16 | d<<24`) and cross-checked bit-for-bit
//     against the bindgen output (e.g. 'B','G','R','A' -> 0x41524742 == 1095911234), which is strong
//     independent confirmation that both sources agree on the real ABI.
//   - https://docs.rs/crate/gst-plugin-ndi (open-source GStreamer NDI element): confirms the
//     per-symbol GetProcAddress/dlsym loading strategy used here (as an alternative to NDI's own
//     documented single `NDIlib_vX_load()` entry point, which returns a table of the very same
//     function pointers) and the `NDI_RUNTIME_DIR_V6` / `NDI_RUNTIME_DIR_V5` environment variables.
//
// No code or comments from any of the above were copied; only the (factual, non-copyrightable) ABI
// shapes -- field names/order/types and exported symbol names -- were reconstructed from them.
//
// Note on ndi.hpp's own comment ("...via its documented NDIlib_v4_load dynamic-loading entry
// point"): that name was a placeholder written before this file researched the real ABI. Public
// documentation and the OBS/DistroAV NDI plugin's own runtime logs (an NDI 6.1.1.0 Runtime loading
// successfully) confirm the real exported entry point for that table-of-function-pointers approach
// is `NDIlib_v5_load`, not `v4`. This file does not call it at all -- see above, it resolves each
// function it needs by name instead, which needs no assumption about the (much larger, harder to
// verify) `NDIlib_v5` struct layout. Functionally equivalent, same DLL, same documented model.
#include "ndi/ndi.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "ndi/unavailable_backend.hpp"

namespace hb::ndi {

namespace ndi_abi {

// ---- structs: layout reconstructed from public documentation (see file header) -----------------
// NDIlib_source_t: a name and NDI's own (opaque to us) address string. The real header has a union
// of {p_ip_address, p_url_address} in that second slot; we only ever read/write it as the URL form,
// which is safe either way (the SDK itself treats both names as the same bytes).
struct NDIlib_source_t {
    const char* p_ndi_name;
    const char* p_url_address;
};

struct NDIlib_find_create_t {
    bool show_local_sources;
    const char* p_groups;
    const char* p_extra_ips;
};

struct NDIlib_recv_create_v3_t {
    NDIlib_source_t source_to_connect_to;
    std::int32_t color_format;   // NDIlib_recv_color_format_e
    std::int32_t bandwidth;      // NDIlib_recv_bandwidth_e
    bool allow_video_fields;
    const char* p_ndi_recv_name;
};

// Field order confirmed against bindgen-generated bindings of the real SDK header (see file header).
struct NDIlib_video_frame_v2_t {
    std::int32_t xres;
    std::int32_t yres;
    std::uint32_t FourCC;  // NDIlib_FourCC_video_type_e
    std::int32_t frame_rate_N;
    std::int32_t frame_rate_D;
    float picture_aspect_ratio;
    std::int32_t frame_format_type;  // NDIlib_frame_format_type_e
    std::int64_t timecode;
    std::uint8_t* p_data;
    std::int32_t line_stride_or_data_size_in_bytes;
    const char* p_metadata;
    std::int64_t timestamp;
};

constexpr std::int32_t kColorFormatBgrxBgra = 0;  // NDIlib_recv_color_format_BGRX_BGRA
constexpr std::int32_t kBandwidthHighest = 100;   // NDIlib_recv_bandwidth_highest

constexpr std::int32_t kFrameTypeNone = 0;
constexpr std::int32_t kFrameTypeVideo = 1;

constexpr std::uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}
// Verified bit-for-bit against bindgen output: fourCc('B','G','R','A') == 1095911234.
constexpr std::uint32_t kFourCcBGRA = fourCc('B', 'G', 'R', 'A');
constexpr std::uint32_t kFourCcBGRX = fourCc('B', 'G', 'R', 'X');
constexpr std::uint32_t kFourCcRGBA = fourCc('R', 'G', 'B', 'A');
constexpr std::uint32_t kFourCcRGBX = fourCc('R', 'G', 'B', 'X');
constexpr std::uint32_t kFourCcUYVY = fourCc('U', 'Y', 'V', 'Y');

// ---- exported function signatures ---------------------------------------------------------------
// On x64 Windows there is a single calling convention, so no explicit __cdecl/__stdcall is needed
// (this backend, like the rest of HB Studio's Windows app, is x64-only).
using Initialize_fn = bool (*)();
using Version_fn = const char* (*)();
using FindCreateV2_fn = void* (*)(const NDIlib_find_create_t*);
using FindDestroy_fn = void (*)(void*);
using FindGetCurrentSources_fn = const NDIlib_source_t* (*)(void*, std::uint32_t*);
using RecvCreateV3_fn = void* (*)(const NDIlib_recv_create_v3_t*);
using RecvDestroy_fn = void (*)(void*);
// p_audio_data/p_metadata are always passed as nullptr here: the public contract documents every
// frame-type pointer as independently optional ("pass NULL for anything you don't need"), so we
// never need the audio/metadata struct layouts at all.
using RecvCaptureV2_fn = std::int32_t (*)(void*, NDIlib_video_frame_v2_t*, void*, void*, std::uint32_t);
using RecvFreeVideoV2_fn = void (*)(void*, NDIlib_video_frame_v2_t*);

}  // namespace ndi_abi

namespace {

std::wstring getEnvW(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf, n);
}

HMODULE tryLoad(std::wstring dir) {
    if (dir.empty()) return nullptr;
    if (dir.back() != L'\\') dir += L'\\';
    dir += L"Processing.NDI.Lib.x64.dll";
    return LoadLibraryW(dir.c_str());
}

// Search order: the environment variables the NDI installer itself sets (newest runtime first),
// then the well-known default install folders as a best-effort fallback, then whatever the normal
// Windows DLL search path turns up (PATH, System32, the app's own folder, ...).
HMODULE loadNdiLibrary() {
    for (const wchar_t* var : {L"NDI_RUNTIME_DIR_V6", L"NDI_RUNTIME_DIR_V5"})
        if (HMODULE m = tryLoad(getEnvW(var))) return m;

    const std::wstring programFiles = getEnvW(L"ProgramFiles");
    if (!programFiles.empty()) {
        for (const wchar_t* sub : {L"\\NDI\\NDI 6 Runtime\\v6", L"\\NDI\\NDI 5 Runtime\\v5", L"\\NDI\\NDI 6 Runtime",
                                    L"\\NDI\\NDI 5 Runtime"})
            if (HMODULE m = tryLoad(programFiles + sub)) return m;
    }
    return LoadLibraryW(L"Processing.NDI.Lib.x64.dll");  // last resort: rely on the OS search path
}

// Converts one captured frame to top-down BGRA8. Only the FourCCs our own recv request
// (`NDIlib_recv_color_format_BGRX_BGRA`) can plausibly hand back are implemented as a fast path;
// anything else (and any internally-inconsistent stride/size) is rejected rather than risking an
// out-of-bounds read -- the caller then treats this call as "nothing this time", never garbage.
bool convertToBgra(const ndi_abi::NDIlib_video_frame_v2_t& vf, std::vector<std::uint8_t>& out) {
    const int w = vf.xres, h = vf.yres;
    if (w <= 0 || h <= 0 || !vf.p_data) return false;
    const std::uint8_t* src = vf.p_data;
    auto clampByte = [](int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); };

    if (vf.FourCC == ndi_abi::kFourCcBGRA || vf.FourCC == ndi_abi::kFourCcBGRX) {
        const int stride = vf.line_stride_or_data_size_in_bytes > 0 ? vf.line_stride_or_data_size_in_bytes : w * 4;
        if (stride < w * 4) return false;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* srow = src + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            std::uint8_t* drow = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
            std::memcpy(drow, srow, static_cast<std::size_t>(w) * 4);
            if (vf.FourCC == ndi_abi::kFourCcBGRX)
                for (int x = 0; x < w; ++x) drow[x * 4 + 3] = 255;
        }
        return true;
    }
    if (vf.FourCC == ndi_abi::kFourCcRGBA || vf.FourCC == ndi_abi::kFourCcRGBX) {
        const int stride = vf.line_stride_or_data_size_in_bytes > 0 ? vf.line_stride_or_data_size_in_bytes : w * 4;
        if (stride < w * 4) return false;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* srow = src + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            std::uint8_t* drow = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
            for (int x = 0; x < w; ++x) {
                drow[x * 4 + 0] = srow[x * 4 + 2];
                drow[x * 4 + 1] = srow[x * 4 + 1];
                drow[x * 4 + 2] = srow[x * 4 + 0];
                drow[x * 4 + 3] = (vf.FourCC == ndi_abi::kFourCcRGBX) ? std::uint8_t{255} : srow[x * 4 + 3];
            }
        }
        return true;
    }
    if (vf.FourCC == ndi_abi::kFourCcUYVY) {  // BT.601, defensive fallback: our own request never asks for this
        const int stride = vf.line_stride_or_data_size_in_bytes > 0 ? vf.line_stride_or_data_size_in_bytes : w * 2;
        if (stride < w * 2) return false;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* srow = src + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            std::uint8_t* drow = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
            for (int x = 0; x + 1 < w; x += 2) {
                const int u = srow[x * 2 + 0], y0 = srow[x * 2 + 1], v = srow[x * 2 + 2], y1 = srow[x * 2 + 3];
                for (int k = 0; k < 2; ++k) {
                    const int yy = (k == 0 ? y0 : y1);
                    const int c = yy - 16, d = u - 128, e = v - 128;
                    const int r = (298 * c + 409 * e + 128) >> 8;
                    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                    const int b = (298 * c + 516 * d + 128) >> 8;
                    std::uint8_t* dp = drow + static_cast<std::size_t>(x + k) * 4;
                    dp[0] = clampByte(b);
                    dp[1] = clampByte(g);
                    dp[2] = clampByte(r);
                    dp[3] = 255;
                }
            }
        }
        return true;
    }
    return false;
}

class WindowsNdiBackend final : public NdiBackend {
public:
    explicit WindowsNdiBackend(HMODULE lib) : lib_(lib) { resolve(); }
    // Intentionally never FreeLibrary(): a receive/find thread may still be inside a call when the
    // process starts tearing down static objects down; same "never unload while the process runs"
    // policy as hb::plugins::SharedLibrary.

    bool available() const override { return ok_; }
    std::string runtimeVersion() const override {
        if (!ok_ || !pVersion_) return "";
        const char* v = nullptr;
        try {
            v = pVersion_();
        } catch (...) {
            return "";
        }
        return v ? std::string(v) : std::string();
    }

    void* findCreate() override {
        if (!ok_) return nullptr;
        ndi_abi::NDIlib_find_create_t cfg{};
        cfg.show_local_sources = true;
        cfg.p_groups = nullptr;
        cfg.p_extra_ips = nullptr;
        try {
            return pFindCreate_(&cfg);
        } catch (...) {
            return nullptr;
        }
    }
    void findDestroy(void* handle) override {
        if (ok_ && handle) {
            try {
                pFindDestroy_(handle);
            } catch (...) {
            }
        }
    }
    std::vector<NdiSourceInfo> findCurrentSources(void* handle) override {
        std::vector<NdiSourceInfo> out;
        if (!ok_ || !handle) return out;
        std::uint32_t n = 0;
        const ndi_abi::NDIlib_source_t* list = nullptr;
        try {
            list = pFindGetCurrentSources_(handle, &n);
        } catch (...) {
            return out;
        }
        if (!list || n == 0 || n > 4096) return out;  // 4096: sanity cap against a corrupt count
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            NdiSourceInfo info;
            if (list[i].p_ndi_name) info.name = list[i].p_ndi_name;
            if (list[i].p_url_address) info.urlAddress = list[i].p_url_address;
            if (!info.name.empty()) out.push_back(std::move(info));
        }
        return out;
    }

    void* recvCreate(const NdiSourceInfo& source) override {
        if (!ok_ || source.name.empty()) return nullptr;
        ndi_abi::NDIlib_recv_create_v3_t cfg{};
        cfg.source_to_connect_to.p_ndi_name = source.name.c_str();
        cfg.source_to_connect_to.p_url_address = source.urlAddress.empty() ? nullptr : source.urlAddress.c_str();
        cfg.color_format = ndi_abi::kColorFormatBgrxBgra;
        cfg.bandwidth = ndi_abi::kBandwidthHighest;
        cfg.allow_video_fields = false;
        cfg.p_ndi_recv_name = "HB Studio";
        try {
            return pRecvCreate_(&cfg);
        } catch (...) {
            return nullptr;
        }
    }
    void recvDestroy(void* handle) override {
        if (ok_ && handle) {
            try {
                pRecvDestroy_(handle);
            } catch (...) {
            }
        }
    }
    bool recvCaptureVideo(void* handle, int timeoutMs, std::vector<std::uint8_t>& bgra, int& width, int& height,
                           double& fps, std::string& error) override {
        if (!ok_ || !handle) {
            error = "Receptor NDI inválido.";
            return false;
        }
        ndi_abi::NDIlib_video_frame_v2_t vf{};
        std::int32_t type = ndi_abi::kFrameTypeNone;
        try {
            type = pRecvCapture_(handle, &vf, nullptr, nullptr, static_cast<std::uint32_t>(std::max(0, timeoutMs)));
        } catch (...) {
            error = "Erro inesperado ao capturar vídeo NDI.";
            return false;
        }
        if (type != ndi_abi::kFrameTypeVideo) return false;  // timeout, audio, metadata, status-change: try again

        const bool converted = convertToBgra(vf, bgra);
        if (converted) {
            width = vf.xres;
            height = vf.yres;
            fps = (vf.frame_rate_D > 0) ? static_cast<double>(vf.frame_rate_N) / vf.frame_rate_D : 0.0;
        }
        try {
            if (pRecvFreeVideo_) pRecvFreeVideo_(handle, &vf);
        } catch (...) {
        }
        if (!converted) bgra.clear();
        return converted;
    }

    // TODO(ndi-output): real NDI sending (NDIlib_send_create_v2 / NDIlib_send_send_video_v2) is not
    // implemented yet on this backend — Program output over NDI is not available until this lands.
    // Deliberately safe no-ops in the meantime (never a crash, never a dangling handle): sendCreate
    // always reports failure, so NdiSender::active() stays false and the UI shows this as "not
    // available" rather than silently pretending to send.
    void* sendCreate(const std::string&) override { return nullptr; }
    void sendDestroy(void*) override {}
    void sendVideo(void*, const std::uint8_t*, int, int, int, int, int) override {}

private:
    void resolve() {
        auto sym = [this](const char* name) { return GetProcAddress(lib_, name); };
        pInitialize_ = reinterpret_cast<ndi_abi::Initialize_fn>(sym("NDIlib_initialize"));
        pVersion_ = reinterpret_cast<ndi_abi::Version_fn>(sym("NDIlib_version"));
        pFindCreate_ = reinterpret_cast<ndi_abi::FindCreateV2_fn>(sym("NDIlib_find_create_v2"));
        pFindDestroy_ = reinterpret_cast<ndi_abi::FindDestroy_fn>(sym("NDIlib_find_destroy"));
        pFindGetCurrentSources_ = reinterpret_cast<ndi_abi::FindGetCurrentSources_fn>(sym("NDIlib_find_get_current_sources"));
        pRecvCreate_ = reinterpret_cast<ndi_abi::RecvCreateV3_fn>(sym("NDIlib_recv_create_v3"));
        pRecvDestroy_ = reinterpret_cast<ndi_abi::RecvDestroy_fn>(sym("NDIlib_recv_destroy"));
        pRecvCapture_ = reinterpret_cast<ndi_abi::RecvCaptureV2_fn>(sym("NDIlib_recv_capture_v2"));
        pRecvFreeVideo_ = reinterpret_cast<ndi_abi::RecvFreeVideoV2_fn>(sym("NDIlib_recv_free_video_v2"));

        ok_ = pFindCreate_ && pFindDestroy_ && pFindGetCurrentSources_ && pRecvCreate_ && pRecvDestroy_ && pRecvCapture_ &&
              pRecvFreeVideo_;
        if (ok_ && pInitialize_) {
            try {
                ok_ = pInitialize_();  // false means "this CPU doesn't support what NDI needs" (rare, old CPUs)
            } catch (...) {
                ok_ = false;
            }
        }
    }

    HMODULE lib_;
    bool ok_ = false;
    ndi_abi::Initialize_fn pInitialize_ = nullptr;
    ndi_abi::Version_fn pVersion_ = nullptr;
    ndi_abi::FindCreateV2_fn pFindCreate_ = nullptr;
    ndi_abi::FindDestroy_fn pFindDestroy_ = nullptr;
    ndi_abi::FindGetCurrentSources_fn pFindGetCurrentSources_ = nullptr;
    ndi_abi::RecvCreateV3_fn pRecvCreate_ = nullptr;
    ndi_abi::RecvDestroy_fn pRecvDestroy_ = nullptr;
    ndi_abi::RecvCaptureV2_fn pRecvCapture_ = nullptr;
    ndi_abi::RecvFreeVideoV2_fn pRecvFreeVideo_ = nullptr;
};

}  // namespace

std::shared_ptr<NdiBackend> defaultBackend() {
    static const std::shared_ptr<NdiBackend> instance = [] {
        HMODULE lib = loadNdiLibrary();
        if (!lib)
            return std::shared_ptr<NdiBackend>(std::make_shared<UnavailableNdiBackend>(
                "NDI Runtime não encontrado. Instale gratuitamente em ndi.video/tools."));
        auto backend = std::make_shared<WindowsNdiBackend>(lib);
        if (!backend->available())
            return std::shared_ptr<NdiBackend>(std::make_shared<UnavailableNdiBackend>(
                "NDI Runtime encontrado, mas não foi possível usá-lo (versão incompatível?)."));
        return std::shared_ptr<NdiBackend>(backend);
    }();
    return instance;
}

}  // namespace hb::ndi
