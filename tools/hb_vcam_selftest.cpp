// hb_vcam_selftest: exercises hb_vcam_source.dll in-process, the way the Frame
// Server does (class factory -> IMFActivate -> media source -> stream ->
// samples), without needing Windows 11 or an installed camera.
//
// Checks: COM interfaces, source/stream start events, NO SIGNAL frames, and
// that frames produced by the Program Output arrive through the named pipe and
// are converted to the expected NV12 pixels.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "video/program_output.hpp"
#include "virtual-camera/windows/vcam_protocol.hpp"
#include "virtual-camera/windows/vcam_sender.hpp"

using Microsoft::WRL::ComPtr;

static int g_failures = 0;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, msg);         \
            ++g_failures;                                             \
        } else {                                                      \
            std::printf("ok   : %s\n", msg);                          \
        }                                                             \
    } while (0)
#define CHECK_HR(expr)                                                                            \
    do {                                                                                          \
        HRESULT _hr = (expr);                                                                     \
        if (FAILED(_hr)) {                                                                        \
            std::printf("FAIL line %d: %s -> 0x%08lX\n", __LINE__, #expr, (unsigned long)_hr);    \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

static const CLSID kClsid = {0x6f0f5b0e, 0x3a52, 0x4d7c, {0x9b, 0x2b, 0x5e, 0x0c, 0x7a, 0x1d, 0x4c, 0x31}};

// Polls for an event of the wanted type (ignores others).
static ComPtr<IMFMediaEvent> waitEvent(IMFMediaEventGenerator* gen, MediaEventType want, int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (FAILED(hr)) return nullptr;
        MediaEventType t{};
        ev->GetType(&t);
        if (t == want) return ev;
    }
    return nullptr;
}

// Requests one sample and returns Y at (x, y) and the buffer length.
static bool grabSample(IMFMediaStream* stream, int x, int y, int* outY, DWORD* outLen) {
    if (FAILED(stream->RequestSample(nullptr))) return false;
    auto ev = waitEvent(stream, MEMediaSample, 2000);
    if (!ev) return false;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (FAILED(ev->GetValue(&pv)) || pv.vt != VT_UNKNOWN) return false;
    ComPtr<IMFSample> sample;
    pv.punkVal->QueryInterface(IID_PPV_ARGS(&sample));
    PropVariantClear(&pv);
    if (!sample) return false;
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMF2DBuffer2> b2;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&b2))) return false;
    BYTE *s0 = nullptr, *start = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    if (FAILED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &s0, &pitch, &start, &len))) return false;
    *outY = s0[static_cast<size_t>(y) * pitch + x];
    *outLen = len;
    b2->Unlock2D();
    return true;
}

int main() {
    CHECK_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK_HR(MFStartup(MF_VERSION));

    HMODULE dll = LoadLibraryW(L"hb_vcam_source.dll");
    CHECK(dll != nullptr, "load hb_vcam_source.dll");
    if (!dll) return 1;
    auto getClass = reinterpret_cast<LPFNGETCLASSOBJECT>(GetProcAddress(dll, "DllGetClassObject"));
    CHECK(getClass != nullptr, "export DllGetClassObject");
    CHECK(GetProcAddress(dll, "DllRegisterServer") && GetProcAddress(dll, "DllUnregisterServer"),
          "exports DllRegisterServer/DllUnregisterServer");

    ComPtr<IClassFactory> factory;
    CHECK_HR(getClass(kClsid, IID_PPV_ARGS(&factory)));
    ComPtr<IMFActivate> activate;
    CHECK_HR(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate)));
    ComPtr<IMFAttributes> asAttrs;
    CHECK(SUCCEEDED(activate.As(&asAttrs)), "activator is also IMFAttributes");
    activate->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);  // attribute store works

    ComPtr<IMFMediaSource> source;
    CHECK_HR(activate->ActivateObject(IID_PPV_ARGS(&source)));

    ComPtr<IMFMediaSourceEx> ex;
    ComPtr<IMFGetService> svc;
    ComPtr<IKsControl> ks;
    ComPtr<IMFSampleAllocatorControl> ctrl;
    ComPtr<IMFMediaEventGenerator> gen;
    CHECK(SUCCEEDED(source.As(&ex)), "QI IMFMediaSourceEx");
    CHECK(SUCCEEDED(source.As(&svc)), "QI IMFGetService");
    CHECK(SUCCEEDED(source.As(&ks)), "QI IKsControl");
    CHECK(SUCCEEDED(source.As(&ctrl)), "QI IMFSampleAllocatorControl");
    CHECK(SUCCEEDED(source.As(&gen)), "QI IMFMediaEventGenerator");

    DWORD chars = 0;
    CHECK_HR(source->GetCharacteristics(&chars));
    CHECK((chars & MFMEDIASOURCE_IS_LIVE) != 0, "source is live");

    ComPtr<IMFPresentationDescriptor> pd;
    CHECK_HR(source->CreatePresentationDescriptor(&pd));
    DWORD nStreams = 0;
    CHECK_HR(pd->GetStreamDescriptorCount(&nStreams));
    CHECK(nStreams == 1, "one stream descriptor");
    BOOL sel = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    CHECK_HR(pd->GetStreamDescriptorByIndex(0, &sel, &sd));
    ComPtr<IMFMediaTypeHandler> th;
    ComPtr<IMFMediaType> mt;
    CHECK_HR(sd->GetMediaTypeHandler(&th));
    CHECK_HR(th->GetCurrentMediaType(&mt));
    GUID sub{};
    UINT32 w = 0, h = 0;
    mt->GetGUID(MF_MT_SUBTYPE, &sub);
    MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
    CHECK(sub == MFVideoFormat_NV12 && w == 1920 && h == 1080, "media type NV12 1920x1080");
    CHECK_HR(pd->SelectStream(0));

    ComPtr<IMFVideoSampleAllocator> alloc;
    CHECK_HR(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&alloc)));
    DWORD inId = 99;
    MFSampleAllocatorUsage usage{};
    CHECK_HR(ctrl->GetAllocatorUsage(0, &inId, &usage));
    CHECK_HR(ctrl->SetDefaultAllocator(0, alloc.Get()));

    PROPVARIANT pos;
    PropVariantInit(&pos);
    CHECK_HR(source->Start(pd.Get(), nullptr, &pos));

    auto newStream = waitEvent(gen.Get(), MENewStream, 3000);
    CHECK(newStream != nullptr, "event MENewStream");
    auto started = waitEvent(gen.Get(), MESourceStarted, 3000);
    CHECK(started != nullptr, "event MESourceStarted");
    if (!newStream) return 1;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    newStream->GetValue(&pv);
    ComPtr<IMFMediaStream> stream;
    if (pv.vt == VT_UNKNOWN) pv.punkVal->QueryInterface(IID_PPV_ARGS(&stream));
    PropVariantClear(&pv);
    CHECK(stream != nullptr, "MENewStream carries IMFMediaStream");
    if (!stream) return 1;

    // Phase 1: no producer -> NO SIGNAL (Y background = 40 at a corner).
    int yv = -1;
    DWORD len = 0;
    bool got = grabSample(stream.Get(), 5, 5, &yv, &len);
    CHECK(got, "received a sample (no producer)");
    CHECK(len >= 1920u * 1080u * 3 / 2, "sample buffer holds a full NV12 frame");
    CHECK(yv == 40, "NO SIGNAL background luma == 40");

    // Phase 2: Program Output -> pipe -> source.
    {
        hb::video::ProgramOutput program({hb::vcam::kWidth, hb::vcam::kHeight, {30, 1}});
        hb::vcam::PipeFrameSender sender(program);
        sender.start();
        program.start();

        bool white = false, blue = false;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end && !(white && blue)) {
            int ya = 0, yb = 0;
            DWORD l = 0;
            if (grabSample(stream.Get(), 4, 4, &ya, &l) && ya > 150) white = true;  // first bar = white
            if (grabSample(stream.Get(), 1910, 4, &yb, &l) && yb > 20 && yb < 80) blue = true;  // last bar = blue
        }
        CHECK(sender.connected(), "producer connected to the pipe");
        CHECK(white, "first bar arrives as white (high luma)");
        CHECK(blue, "last bar arrives as blue (low luma)");
        program.stop();
        sender.stop();
    }

    CHECK_HR(source->Stop());
    CHECK_HR(source->Shutdown());
    stream.Reset();
    source.Reset();
    MFShutdown();
    std::printf(g_failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
