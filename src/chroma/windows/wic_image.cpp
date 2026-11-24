#include "chroma/windows/wic_image.hpp"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hb::chroma {

namespace {
// Initialises COM for this call if the thread has not done so (balanced on scope exit). A thread
// already initialised in the other apartment mode (RPC_E_CHANGED_MODE) can still use COM.
struct ComScope {
    bool owns = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (owns) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
}  // namespace

bool loadImageWic(const std::string& utf8Path, video::Frame& out, std::string& err) {
    const std::wstring wpath = widenUtf8(utf8Path);
    if (wpath.empty()) {
        err = "Caminho da imagem inválido.";
        return false;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(wpath), ec);
    if (ec) {
        err = "Não foi possível abrir o arquivo da imagem.";
        return false;
    }
    if (size > 50ull * 1024 * 1024) {
        err = "Imagem grande demais (limite de 50 MB).";
        return false;
    }

    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        err = "Decodificador de imagens do Windows indisponível.";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder))) {
        err = "Formato de imagem não reconhecido.";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 8192 || h > 8192) {
        err = "Imagem inválida ou grande demais (máximo 8192 px por lado).";
        return false;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        err = "Não foi possível converter a imagem.";
        return false;
    }
    video::Frame tmp;
    tmp.width = static_cast<int>(w);
    tmp.height = static_cast<int>(h);
    tmp.stride = static_cast<int>(w) * 4;
    tmp.format = video::PixelFormat::BGRA8;
    tmp.data.assign(static_cast<std::size_t>(tmp.stride) * h, 0);
    if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(tmp.stride), static_cast<UINT>(tmp.data.size()), tmp.data.data()))) {
        err = "Não foi possível ler os pixels da imagem.";
        return false;
    }
    for (std::size_t i = 3; i < tmp.data.size(); i += 4) tmp.data[i] = 255;   // 32bppBGR leaves X undefined: make it opaque
    out = std::move(tmp);
    return true;
}

}  // namespace hb::chroma
