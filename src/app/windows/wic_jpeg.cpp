#include "app/windows/wic_jpeg.hpp"

#include <objidl.h>
#include <ocidl.h>

using Microsoft::WRL::ComPtr;

namespace hb::win {

bool JpegEncoder::init() {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory_)));
}

bool JpegEncoder::encode(const video::Frame& f, int quality, std::vector<std::uint8_t>& out) {
    if (!factory_ || f.data.empty()) return false;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(factory_->CreateBitmapFromMemory(f.width, f.height, GUID_WICPixelFormat32bppBGRA, f.stride,
                                                static_cast<UINT>(f.data.size()),
                                                const_cast<BYTE*>(f.data.data()), &bmp)))
        return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory_->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&fr, &props))) return false;
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = static_cast<float>(quality) / 100.f;
    props->Write(1, &opt, &v);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(fr->Initialize(props.Get())) || FAILED(fr->SetSize(f.width, f.height)) ||
        FAILED(fr->SetPixelFormat(&pf)) || FAILED(fr->WriteSource(conv.Get(), nullptr)) ||
        FAILED(fr->Commit()) || FAILED(enc->Commit()))
        return false;

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &hg))) return false;
    const SIZE_T size = GlobalSize(hg);
    void* p = GlobalLock(hg);
    if (!p) return false;
    out.assign(static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + size);
    GlobalUnlock(hg);
    return true;
}

}  // namespace hb::win
