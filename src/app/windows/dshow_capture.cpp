#include "app/windows/dshow_capture.hpp"

#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <map>

#include "app/windows/win_util.hpp"
#include "video/frame_ops.hpp"

using Microsoft::WRL::ComPtr;

namespace hb::win {

namespace {

// ---- qedit.h is gone from modern SDKs; the Sample Grabber itself still ships in quartz.dll. ----
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long length) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* bufferSize, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long whichMethodToCallback) = 0;
};

const CLSID kClsidSampleGrabber = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
const CLSID kClsidNullRenderer = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

void freeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) { mt.pUnk->Release(); mt.pUnk = nullptr; }
}

std::string readProp(IPropertyBag* bag, const wchar_t* key) {
    VARIANT v;
    VariantInit(&v);
    std::string out;
    if (SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal) out = narrow(v.bstrVal);
    VariantClear(&v);
    return out;
}

bool looksVirtual(const std::string& name) {
    std::string l = name;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* k : {"virtual", "obs", "vcam", "droidcam", "manycam", "ndi", "snap camera", "xsplit", "epoccam", "iriun", "camo", "mmhmm"})
        if (l.find(k) != std::string::npos) return true;
    return false;
}

bool isOurOwnCamera(const std::string& name) {
    std::string l = name;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("hb studio") != std::string::npos;
}

struct Found {
    VideoDevice dev;
    ComPtr<IMoniker> moniker;
};

// Enumerates monikers; ids stay stable and unique (duplicate names get "#2", "#3"...).
std::vector<Found> enumerateMonikers() {
    std::vector<Found> out;
    ComPtr<ICreateDevEnum> de;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&de)))) return out;
    ComPtr<IEnumMoniker> em;
    if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) != S_OK || !em) return out;  // S_FALSE: none
    std::map<std::string, int> seen;
    ComPtr<IMoniker> mon;
    while (em->Next(1, &mon, nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            std::string name = readProp(bag.Get(), L"FriendlyName");
            std::string path = readProp(bag.Get(), L"DevicePath");
            if (!name.empty() && !isOurOwnCamera(name)) {
                std::string id = path.empty() ? name : path;
                const int n = ++seen[id];
                if (n > 1) id += "#" + std::to_string(n);
                Found f;
                f.dev = {id, name, looksVirtual(name)};
                f.moniker = mon;
                out.push_back(std::move(f));
            }
        }
        mon.Reset();
    }
    return out;
}

// Sample grabber callback. Its lifetime is owned by DShowSource (the graph is torn down first).
struct GrabCb final : ISampleGrabberCB {
    explicit GrabCb(DShowSource* o) : owner(o) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) { *ppv = static_cast<ISampleGrabberCB*>(this); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* buf, long len) override {
        try { owner->onBuffer(buf, len); } catch (...) {}
        return S_OK;
    }
    DShowSource* owner;
};

}  // namespace

std::vector<VideoDevice> enumerateVideoDevices() {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<VideoDevice> out;
    try {
        for (auto& f : enumerateMonikers()) out.push_back(f.dev);
    } catch (...) {
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return out;
}

// TODO(camera-resolution): enumerate real formats via IAMStreamConfig on the device's capture pin.
// Returning empty is a safe, honest default - every caller already treats "no formats" as "this
// device doesn't advertise a list, use automatic" rather than an error.
std::vector<VideoFormat> enumerateDeviceFormats(const std::string&) { return {}; }

DShowSource::DShowSource(std::string deviceId, std::string displayName, VideoFormat requested)
    : id_(std::move(deviceId)), name_(std::move(displayName)), requested_(requested) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

DShowSource::~DShowSource() {
    stop();
    if (stopEvent_) CloseHandle(stopEvent_);
}

void DShowSource::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        failed_ = false;
        error_.clear();
        running_ = false;
    }
    ResetEvent(stopEvent_);
    thread_ = std::thread([this] { run(); });
}

void DShowSource::stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
}

void DShowSource::run() {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto fail = [this](const std::string& msg) {
        std::lock_guard lock(mutex_);
        failed_ = true;
        error_ = msg;
    };
    try {
        // 1) find the device again by id
        ComPtr<IMoniker> moniker;
        for (auto& f : enumerateMonikers())
            if (f.dev.id == id_) { moniker = f.moniker; break; }
        if (!moniker) { fail("Dispositivo não encontrado (desconectado?)."); goto done; }

        {
            ComPtr<IBaseFilter> src;
            if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&src))) || !src) {
                fail("Não foi possível abrir o dispositivo (em uso por outro programa?).");
                goto done;
            }
            ComPtr<IGraphBuilder> graph;
            ComPtr<ICaptureGraphBuilder2> cgb;
            ComPtr<IBaseFilter> grabberFilter, nullFilter;
            if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))) ||
                FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb))) ||
                FAILED(CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter))) ||
                FAILED(CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullFilter)))) {
                fail("Componentes de vídeo do Windows (DirectShow) indisponíveis.");
                goto done;
            }
            cgb->SetFiltergraph(graph.Get());
            graph->AddFilter(src.Get(), L"Source");
            graph->AddFilter(grabberFilter.Get(), L"Grabber");
            graph->AddFilter(nullFilter.Get(), L"Null");

            ComPtr<ISampleGrabber> grabber;
            grabberFilter.As(&grabber);
            AM_MEDIA_TYPE want{};
            want.majortype = MEDIATYPE_Video;
            want.subtype = MEDIASUBTYPE_RGB32;   // forces a colour converter into the graph: BGRA out
            if (!grabber || FAILED(grabber->SetMediaType(&want))) { fail("SampleGrabber indisponível."); goto done; }

            // TODO(camera-resolution): if requested_ is non-empty, QueryInterface the capture pin
            // for IAMStreamConfig and SetFormat() here, before RenderStream connects it - falling
            // back to automatic (today's behaviour) if the exact format isn't supported.
            // capture pin first, then preview, then any pin (software cameras differ)
            HRESULT hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src.Get(), grabberFilter.Get(), nullFilter.Get());
            if (FAILED(hr)) hr = cgb->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, src.Get(), grabberFilter.Get(), nullFilter.Get());
            if (FAILED(hr)) hr = cgb->RenderStream(nullptr, &MEDIATYPE_Video, src.Get(), grabberFilter.Get(), nullFilter.Get());
            if (FAILED(hr)) {
                char b[96];
                snprintf(b, sizeof b, "Não foi possível conectar o vídeo do dispositivo (0x%08lX).", static_cast<unsigned long>(hr));
                fail(b);
                goto done;
            }

            AM_MEDIA_TYPE mt{};
            if (SUCCEEDED(grabber->GetConnectedMediaType(&mt)) && mt.formattype == FORMAT_VideoInfo && mt.pbFormat &&
                mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
                std::lock_guard lock(mutex_);
                width_ = vih->bmiHeader.biWidth;
                height_ = std::abs(vih->bmiHeader.biHeight);
                bottomUp_ = vih->bmiHeader.biHeight > 0;
            }
            freeMediaType(mt);
            {
                std::lock_guard lock(mutex_);
                if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384) {
                    failed_ = true;
                    error_ = "Formato de vídeo inesperado.";
                }
            }
            if (failed_) goto done;

            GrabCb cb(this);
            grabber->SetBufferSamples(FALSE);
            grabber->SetOneShot(FALSE);
            grabber->SetCallback(&cb, 1);   // 1 = BufferCB

            ComPtr<IMediaControl> control;
            graph.As(&control);
            if (!control || FAILED(control->Run())) {
                grabber->SetCallback(nullptr, 0);
                fail("O dispositivo não iniciou. Verifique Configurações › Privacidade e segurança › Câmera (permitir apps da área de trabalho) ou se outro programa o usa.");
                goto done;
            }
            {
                std::lock_guard lock(mutex_);
                running_ = true;
                runningSince_ = std::chrono::steady_clock::now();
            }
            WaitForSingleObject(stopEvent_, INFINITE);   // frames arrive on the streaming thread
            control->Stop();
            grabber->SetCallback(nullptr, 0);
        }
    } catch (...) {
        fail("Erro inesperado ao capturar o dispositivo.");
    }
done:
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    if (SUCCEEDED(ci)) CoUninitialize();
}

void DShowSource::onBuffer(const std::uint8_t* data, long length) {
    int w, h;
    bool bottomUp;
    {
        std::lock_guard lock(mutex_);
        w = width_;
        h = height_;
        bottomUp = bottomUp_;
    }
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    if (w <= 0 || h <= 0 || !data || length < 0 || static_cast<std::size_t>(length) < stride * static_cast<std::size_t>(h)) return;

    auto f = std::make_shared<video::Frame>();
    video::prepareFrame(*f, w, h);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* s = data + stride * static_cast<std::size_t>(bottomUp ? h - 1 - y : y);
        std::uint8_t* d = f->data.data() + stride * static_cast<std::size_t>(y);
        std::memcpy(d, s, stride);
        for (int x = 0; x < w; ++x) d[x * 4 + 3] = 255;   // many cameras leave alpha at 0
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    latest_ = std::move(f);
    lastFrame_ = now;
    ++frames_;
    if (fpsFrames_ == 0) fpsT0_ = now;
    ++fpsFrames_;
    const double dt = std::chrono::duration<double>(now - fpsT0_).count();
    if (dt >= 1.0) {
        fps_ = static_cast<double>(fpsFrames_) / dt;
        fpsFrames_ = 0;
    }
}

void DShowSource::render(video::Frame& out, std::uint64_t) const {
    std::shared_ptr<const video::Frame> f;
    std::chrono::steady_clock::time_point last;
    {
        std::lock_guard lock(mutex_);
        f = latest_;
        last = lastFrame_;
    }
    const bool fresh = f && (std::chrono::steady_clock::now() - last) < std::chrono::milliseconds(1500);
    if (fresh) {
        video::fitFrame(*f, out);
        return;
    }
    // no signal: calm dark blue, never garbage
    for (std::size_t i = 0; i + 3 < out.data.size(); i += 4) {
        out.data[i] = 43; out.data[i + 1] = 18; out.data[i + 2] = 7; out.data[i + 3] = 255;
    }
}

video::SourceStatus DShowSource::status() const {
    using S = video::SourceStatus::State;
    video::SourceStatus s;
    std::lock_guard lock(mutex_);
    s.width = width_;
    s.height = height_;
    s.fps = fps_;
    const auto now = std::chrono::steady_clock::now();
    if (failed_) {
        s.state = S::Error;
        s.message = error_;
    } else if (!running_) {
        s.state = S::Connecting;
    } else if (frames_ == 0) {
        s.state = (now - runningSince_) < std::chrono::seconds(3) ? S::Connecting : S::NoSignal;
        if (s.state == S::NoSignal) s.message = "Sem imagem. Se for uma câmera virtual (ex.: OBS), inicie-a no programa de origem.";
    } else if (now - lastFrame_ > std::chrono::milliseconds(1500)) {
        s.state = S::NoSignal;
        s.fps = 0;
        s.message = "Sem imagem. Se for uma câmera virtual (ex.: OBS), inicie-a no programa de origem.";
    } else {
        s.state = S::Online;
    }
    return s;
}

}  // namespace hb::win
