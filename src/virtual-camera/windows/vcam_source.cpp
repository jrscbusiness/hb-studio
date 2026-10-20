// HB Studio Virtual Camera - custom media source for the Windows 11 Frame Server.
//
// Loaded by the "Windows Camera Frame Server" service when a client (OBS, Camera
// app, browser...) opens the camera created by MFCreateVirtualCamera. Frames come
// from the HB Studio app through a named pipe (see vcam_protocol.hpp); with no
// producer connected an animated "NO SIGNAL" image is delivered so the camera
// can be validated independently of the app.
//
// Implemented from the public Frame Server custom-media-source contract. The
// overall object layout was informed by Microsoft's MIT-licensed Windows-Camera
// VirtualCamera sample (see LICENSES.md); no code is taken from GPL projects.
//
// Diagnostics: %ProgramData%\HBStudio\vcam.log (the folder is created by
// tools/vcam/install.ps1 and made writable for the service account).

#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vcam_protocol.hpp"

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

HINSTANCE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// {6F0F5B0E-3A52-4D7C-9B2B-5E0C7A1D4C31}
const CLSID CLSID_HBMediaSourceActivate = {
    0x6f0f5b0e, 0x3a52, 0x4d7c, {0x9b, 0x2b, 0x5e, 0x0c, 0x7a, 0x1d, 0x4c, 0x31}};

// ------------------------------------------------------------------ logging
void Log(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetSystemTime(&t);
    char line[640];
    _snprintf_s(line, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [pid %lu] %s\r\n", t.wYear,
                t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentProcessId(), msg);
    OutputDebugStringA(line);

    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\HBStudio\\vcam.log", dir);
    // The folder is writable by LocalService only (see installer): still, never follow a
    // reparse point, and stop logging once the file reaches 2 MB (no unbounded growth).
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(f, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow < 2u * 1024 * 1024) {
        DWORD written = 0;
        WriteFile(f, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    }
    CloseHandle(f);
}

#define HB_RETURN_IF_FAILED(expr)                                                  \
    do {                                                                           \
        HRESULT _hr = (expr);                                                      \
        if (FAILED(_hr)) {                                                         \
            Log("%s failed: 0x%08lX (line %d)", #expr, static_cast<unsigned long>(_hr), __LINE__); \
            return _hr;                                                            \
        }                                                                          \
    } while (0)

// ----------------------------------------------------------------- frame hub
// Named-pipe server that receives BGRA frames from the app.
//
// One hub per process (shared by every source/stream via Acquire/Release): the pipe
// name is claimed once with FILE_FLAG_FIRST_PIPE_INSTANCE and a single instance, so no
// other process can create a competing instance, and the same instance is re-armed
// after each producer disconnects (the name is never released in between).
// ACL: SYSTEM, Administrators and LocalService full control; the interactive user
// may only write data (no FILE_CREATE_PIPE_INSTANCE) - see docs/REVIEW-1.md A1.
// A producer that sends nothing for kIdleMs is disconnected (A2).
class FrameHub {
public:
    static FrameHub& Instance() {
        static FrameHub* hub = new FrameHub();  // intentionally leaked: never joins a thread during DLL unload
        return *hub;
    }

    void Acquire() {
        std::lock_guard life(m_life);
        if (m_refs++ == 0) {
            m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            m_thread = std::thread([this] { Run(); });
        }
    }

    void Release() {
        std::lock_guard life(m_life);
        if (m_refs > 0 && --m_refs == 0) StopLocked();
    }

    // Copies the newest frame if it is fresh. Returns false => show NO SIGNAL.
    bool CopyLatest(std::vector<uint8_t>& dst) {
        std::lock_guard lock(m_data);
        if (m_latest.empty()) return false;
        if (GetTickCount64() - m_lastFrameTick > 1000) return false;
        dst = m_latest;
        return true;
    }

private:
    static constexpr DWORD kIdleMs = 3000;      // no complete frame header within this => drop producer
    static constexpr DWORD kPayloadMs = 1500;   // stalled mid-frame

    void StopLocked() {  // m_life held
        if (!m_thread.joinable()) return;
        SetEvent(m_stop);
        m_thread.join();
        CloseHandle(m_stop);
        m_stop = nullptr;
    }

    bool ReadExact(HANDLE pipe, void* buf, DWORD bytes, DWORD timeoutMs) {
        auto* p = static_cast<uint8_t*>(buf);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool ok = true;
        while (bytes > 0 && ok) {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD got = 0;
            BOOL r = ReadFile(pipe, p, bytes, &got, &ov);
            if (!r) {
                if (GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
                HANDLE waits[2] = {ev, m_stop};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
                if (w != WAIT_OBJECT_0) {  // stop requested or timeout
                    CancelIoEx(pipe, &ov);
                    GetOverlappedResult(pipe, &ov, &got, TRUE);
                    if (w == WAIT_TIMEOUT) Log("FrameHub: producer idle/stalled, disconnecting");
                    ok = false;
                    break;
                }
                if (!GetOverlappedResult(pipe, &ov, &got, FALSE)) { ok = false; break; }
            }
            if (got == 0) { ok = false; break; }
            p += got;
            bytes -= got;
        }
        CloseHandle(ev);
        return ok;
    }

    void Run() {
        Log("FrameHub: pipe server starting");
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00100082;;;IU)", SDDL_REVISION_1,
                &sa.lpSecurityDescriptor, nullptr)) {
            Log("FrameHub: SDDL failed %lu", GetLastError());
            return;
        }
        HANDLE pipe = INVALID_HANDLE_VALUE;
        while (WaitForSingleObject(m_stop, 0) != WAIT_OBJECT_0) {
            if (pipe == INVALID_HANDLE_VALUE) {
                pipe = CreateNamedPipeW(
                    hb::vcam::kPipeName,
                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
                    4 * 1024 * 1024, 0, &sa);
                if (pipe == INVALID_HANDLE_VALUE) {
                    // Name already taken (stale instance or another process): never share it, retry later.
                    Log("FrameHub: CreateNamedPipe failed %lu (name in use?)", GetLastError());
                    if (WaitForSingleObject(m_stop, 2000) == WAIT_OBJECT_0) break;
                    continue;
                }
            }
            HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED ov{};
            ov.hEvent = ev;
            bool connected = false;
            if (ConnectNamedPipe(pipe, &ov)) {
                connected = true;
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (err == ERROR_IO_PENDING) {
                    HANDLE waits[2] = {ev, m_stop};
                    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) connected = true;
                    else CancelIoEx(pipe, &ov);
                } else {
                    Log("FrameHub: ConnectNamedPipe failed %lu", err);
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                }
            }
            CloseHandle(ev);

            if (connected) {
                Log("FrameHub: producer connected");
                std::vector<uint8_t> staging;
                for (;;) {
                    hb::vcam::FrameHeader h{};
                    if (!ReadExact(pipe, &h, sizeof(h), kIdleMs)) break;
                    const uint64_t expect = static_cast<uint64_t>(h.stride) * h.height;
                    if (h.magic != hb::vcam::kMagic || h.payloadBytes == 0 || h.payloadBytes > 64u * 1024 * 1024 ||
                        h.payloadBytes != expect) {
                        Log("FrameHub: bad header, dropping producer");
                        break;
                    }
                    staging.resize(h.payloadBytes);
                    if (!ReadExact(pipe, staging.data(), h.payloadBytes, kPayloadMs)) break;
                    if (h.width != hb::vcam::kWidth || h.height != hb::vcam::kHeight ||
                        h.stride != hb::vcam::kWidth * 4u) {
                        continue;  // size mismatch: ignore (NO SIGNAL keeps showing)
                    }
                    std::lock_guard lock(m_data);
                    m_latest.swap(staging);
                    m_lastFrameTick = GetTickCount64();
                }
                Log("FrameHub: producer disconnected");
                DisconnectNamedPipe(pipe);  // same instance is re-armed by the loop
            }
        }
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        LocalFree(sa.lpSecurityDescriptor);
        Log("FrameHub: pipe server stopped");
    }

    std::mutex m_life;   // serialises Acquire/Release/StopAll (start/stop of the thread)
    int m_refs = 0;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    std::mutex m_data;   // latest frame
    std::vector<uint8_t> m_latest;
    ULONGLONG m_lastFrameTick = 0;
};

// -------------------------------------------------------------- pixel convert
inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// BGRA8 -> NV12, BT.709 limited range.
void BgraToNv12(const uint8_t* src, int srcStride, int w, int h, uint8_t* dstY, int pitch,
                uint8_t* dstUV) {
    for (int y = 0; y < h; y += 2) {
        const uint8_t* s0 = src + static_cast<size_t>(y) * srcStride;
        const uint8_t* s1 = s0 + srcStride;
        uint8_t* y0 = dstY + static_cast<size_t>(y) * pitch;
        uint8_t* y1 = y0 + pitch;
        uint8_t* uv = dstUV + static_cast<size_t>(y / 2) * pitch;
        for (int x = 0; x < w; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            const uint8_t* px[4] = {s0 + x * 4, s0 + x * 4 + 4, s1 + x * 4, s1 + x * 4 + 4};
            uint8_t* yo[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; ++k) {
                const int b = px[k][0], g = px[k][1], r = px[k][2];
                *yo[k] = Clamp8(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                rs += r; gs += g; bs += b;
            }
            const int r = rs / 4, g = gs / 4, b = bs / 4;
            uv[x] = Clamp8(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            uv[x + 1] = Clamp8(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}

void FillNoSignal(uint8_t* dstY, int pitch, uint8_t* dstUV, int w, int h, uint64_t frame) {
    const int barW = 80;
    const int barX = static_cast<int>((frame * 16) % static_cast<uint64_t>(w));
    for (int y = 0; y < h; ++y) {
        uint8_t* row = dstY + static_cast<size_t>(y) * pitch;
        memset(row, 40, static_cast<size_t>(w));
        if (y > h / 3 && y < 2 * h / 3) {
            for (int x = barX; x < barX + barW && x < w; ++x) row[x] = 200;
        }
    }
    for (int y = 0; y < h / 2; ++y) memset(dstUV + static_cast<size_t>(y) * pitch, 128, static_cast<size_t>(w));
}

// ------------------------------------------------------------------- stream
constexpr DWORD kStreamId = 0;

class HBStream final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    HBStream() { g_objects++; }
    ~HBStream() override { g_objects--; }

    HRESULT Initialize(IMFMediaSource* parent) {
        m_parent = parent;

        ComPtr<IMFMediaType> mt;
        HB_RETURN_IF_FAILED(MFCreateMediaType(&mt));
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, hb::vcam::kWidth, hb::vcam::kHeight);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, hb::vcam::kFpsNum, hb::vcam::kFpsDen);
        MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        mt->SetUINT32(MF_MT_AVG_BITRATE, hb::vcam::kWidth * hb::vcam::kHeight * 12 * hb::vcam::kFpsNum);
        mt->SetUINT32(MF_MT_SAMPLE_SIZE, hb::vcam::kWidth * hb::vcam::kHeight * 3 / 2);
        mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

        HB_RETURN_IF_FAILED(MFCreateAttributes(&m_attrs, 4));
        HB_RETURN_IF_FAILED(SetCommonAttributes(m_attrs.Get()));
        HB_RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));

        IMFMediaType* types[1] = {mt.Get()};
        HB_RETURN_IF_FAILED(MFCreateStreamDescriptor(kStreamId, 1, types, &m_desc));
        ComPtr<IMFMediaTypeHandler> handler;
        HB_RETURN_IF_FAILED(m_desc->GetMediaTypeHandler(&handler));
        HB_RETURN_IF_FAILED(handler->SetCurrentMediaType(mt.Get()));
        HB_RETURN_IF_FAILED(SetCommonAttributes(m_desc.Get()));
        return S_OK;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q;
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            q = m_queue;
        }
        return q->GetEvent(flags, ev);  // may block: must not hold the lock
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_queue->BeginGetEvent(cb, state);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** ev) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_queue->EndGetEvent(res, ev);
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status,
                            const PROPVARIANT* value) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_queue->QueueEventParamVar(met, ext, status, value);
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** src) override {
        if (!src) return E_POINTER;
        *src = nullptr;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_parent.CopyTo(src);
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override {
        if (!sd) return E_POINTER;
        *sd = nullptr;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_desc.CopyTo(sd);
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        m_tokens.push_back(token);  // null token is allowed
        if (m_tokens.size() > 16) m_tokens.pop_front();
        return S_OK;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (m_state == state) return S_OK;
        switch (state) {
            case MF_STREAM_STATE_PAUSED:
                if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
                m_state = MF_STREAM_STATE_PAUSED;
                return S_OK;
            case MF_STREAM_STATE_RUNNING:
                return StartLocked(false);
            case MF_STREAM_STATE_STOPPED:
                StopLocked(false);
                return S_OK;
            default:
                return MF_E_INVALID_STATE_TRANSITION;
        }
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
        if (!state) return E_INVALIDARG;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *state = m_state;
        return S_OK;
    }

    // Called by the source.
    HRESULT Start() {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return StartLocked(true);
    }
    void Stop(bool sendEvent) {
        std::thread t;
        {
            std::lock_guard lock(m_mutex);
            StopLocked(sendEvent);
            m_quit = true;
            t = std::move(m_worker);
        }
        if (t.joinable()) t.join();
        ReleaseHub();
    }
    void Shutdown() {
        std::thread t;
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
            m_state = MF_STREAM_STATE_STOPPED;
            m_tokens.clear();
            if (m_queue) m_queue->Shutdown();
            t = std::move(m_worker);
        }
        if (t.joinable()) t.join();
        ReleaseHub();
        std::lock_guard lock(m_mutex);
        m_queue.Reset();
        m_parent.Reset();
        m_alloc.Reset();
    }
    HRESULT SetAllocator(IUnknown* a) {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (m_state == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        m_alloc.Reset();
        if (!a) return S_OK;
        return a->QueryInterface(IID_PPV_ARGS(&m_alloc));
    }
    HRESULT GetAttributes(IMFAttributes** out) {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_attrs.CopyTo(out);
    }
    HRESULT GetCurrentMediaType(IMFMediaType** mt) {
        ComPtr<IMFMediaTypeHandler> h;
        HB_RETURN_IF_FAILED(m_desc->GetMediaTypeHandler(&h));
        return h->GetCurrentMediaType(mt);
    }
    IMFStreamDescriptor* Descriptor() { return m_desc.Get(); }

private:
    void ReleaseHub() {
        bool held;
        {
            std::lock_guard lock(m_mutex);
            held = m_hubHeld;
            m_hubHeld = false;
        }
        if (held) FrameHub::Instance().Release();
    }

    static HRESULT SetCommonAttributes(IMFAttributes* a) {
        HB_RETURN_IF_FAILED(a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        HB_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId));
        HB_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        HB_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                                         MFFrameSourceTypes_Color));
        return S_OK;
    }

    HRESULT StartLocked(bool sendEvent) {
        if (!m_alloc) {
            // Frame Server normally provides the allocator; create our own otherwise.
            HB_RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_alloc)));
        }
        ComPtr<IMFMediaType> mt;
        HB_RETURN_IF_FAILED(GetCurrentMediaType(&mt));
        HB_RETURN_IF_FAILED(m_alloc->InitializeSampleAllocator(10, mt.Get()));
        if (sendEvent) HB_RETURN_IF_FAILED(m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
        m_state = MF_STREAM_STATE_RUNNING;
        if (!m_hubHeld) {
            FrameHub::Instance().Acquire();
            m_hubHeld = true;
        }
        m_quit = false;
        if (!m_worker.joinable()) m_worker = std::thread([this] { Pace(); });
        Log("stream started");
        return S_OK;
    }

    void StopLocked(bool sendEvent) {
        m_state = MF_STREAM_STATE_STOPPED;
        m_tokens.clear();
        if (sendEvent && m_queue) m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
        Log("stream stopped");
    }

    // Delivers one sample per frame period for each outstanding request.
    void Pace() {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(
            1'000'000'000LL * hb::vcam::kFpsDen / hb::vcam::kFpsNum);
        auto next = clock::now();
        std::vector<uint8_t> bgra;
        for (;;) {
            next += period;
            std::this_thread::sleep_until(next);
            if (clock::now() > next + period * 4) next = clock::now();  // fell behind: resync

            ComPtr<IUnknown> token;
            ComPtr<IMFVideoSampleAllocator> alloc;
            {
                std::lock_guard lock(m_mutex);
                if (m_shutdown || m_quit) return;
                if (m_state != MF_STREAM_STATE_RUNNING || m_tokens.empty()) continue;
                token = m_tokens.front();
                m_tokens.pop_front();
                alloc = m_alloc;
            }
            const bool live = FrameHub::Instance().CopyLatest(bgra);
            HRESULT hr = alloc ? Deliver(alloc.Get(), token.Get(), live ? &bgra : nullptr) : E_UNEXPECTED;
            if (FAILED(hr)) {
                Log("Deliver failed 0x%08lX", static_cast<unsigned long>(hr));
                // Keep the outstanding request: Frame Server counts them and would stall without it.
                std::lock_guard lock(m_mutex);
                if (!m_shutdown && m_state == MF_STREAM_STATE_RUNNING && m_tokens.size() < 16) m_tokens.push_front(token);
            }
        }
    }

    HRESULT Deliver(IMFVideoSampleAllocator* alloc, IUnknown* token, const std::vector<uint8_t>* bgra) {
        ComPtr<IMFSample> sample;
        HB_RETURN_IF_FAILED(alloc->AllocateSample(&sample));
        ComPtr<IMFMediaBuffer> buf;
        HB_RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buf));
        ComPtr<IMF2DBuffer2> buf2d;
        HB_RETURN_IF_FAILED(buf.As(&buf2d));
        BYTE* scanline0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        HB_RETURN_IF_FAILED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length));
        const int w = hb::vcam::kWidth, h = hb::vcam::kHeight;
        // NV12 = pitch*h (Y) + pitch*h/2 (interleaved UV, same pitch). Refuse anything smaller:
        // never deliver an unwritten buffer.
        const size_t need = static_cast<size_t>(pitch > 0 ? pitch : 0) * h * 3 / 2;
        if (pitch < w || length < need) {
            buf2d->Unlock2D();
            Log("Deliver: unexpected buffer layout pitch=%ld length=%lu", pitch, static_cast<unsigned long>(length));
            return MF_E_BUFFERTOOSMALL;
        }
        uint8_t* y = scanline0;
        uint8_t* uv = scanline0 + static_cast<size_t>(pitch) * h;
        if (bgra) {
            BgraToNv12(bgra->data(), w * 4, w, h, y, pitch, uv);
        } else {
            FillNoSignal(y, pitch, uv, w, h, m_frameNo);
        }
        buf2d->Unlock2D();
        buf->SetCurrentLength(static_cast<DWORD>(need));
        ++m_frameNo;
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetUINT32(MFSampleExtension_FrameCorruption, 0);
        sample->SetSampleDuration(10'000'000LL * hb::vcam::kFpsDen / hb::vcam::kFpsNum);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token);

        std::lock_guard lock(m_mutex);
        if (m_shutdown || !m_queue) return MF_E_SHUTDOWN;
        return m_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }

    std::mutex m_mutex;
    bool m_shutdown = false;
    bool m_quit = false;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    ComPtr<IMFMediaSource> m_parent;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFStreamDescriptor> m_desc;
    ComPtr<IMFAttributes> m_attrs;
    ComPtr<IMFVideoSampleAllocator> m_alloc;
    std::deque<ComPtr<IUnknown>> m_tokens;
    std::thread m_worker;
    bool m_hubHeld = false;   // guarded by m_mutex
    uint64_t m_frameNo = 0;
};

// ------------------------------------------------------------------- source
class HBSource final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                          IMFGetService, IKsControl, IMFSampleAllocatorControl> {
public:
    HBSource() { g_objects++; }
    ~HBSource() override { g_objects--; }

    HRESULT Initialize(IMFAttributes* activateAttrs) {
        std::lock_guard lock(m_mutex);
        HB_RETURN_IF_FAILED(MFCreateAttributes(&m_attrs, 2));
        if (activateAttrs) HB_RETURN_IF_FAILED(activateAttrs->CopyAllItems(m_attrs.Get()));

        // A Legacy sensor profile is mandatory for profile-unaware applications.
        ComPtr<IMFSensorProfileCollection> profiles;
        ComPtr<IMFSensorProfile> profile;
        HB_RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profiles));
        HB_RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
        HB_RETURN_IF_FAILED(profile->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))"));
        HB_RETURN_IF_FAILED(profiles->AddProfile(profile.Get()));
        HB_RETURN_IF_FAILED(m_attrs->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get()));

        HB_RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));

        m_stream = Make<HBStream>();
        if (!m_stream) return E_OUTOFMEMORY;
        HB_RETURN_IF_FAILED(m_stream->Initialize(this));
        IMFStreamDescriptor* descs[1] = {m_stream->Descriptor()};
        HB_RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descs, &m_pd));
        m_state = State::Stopped;
        Log("source initialized");
        return S_OK;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override {
        ComPtr<IMFMediaEventQueue> q;
        {
            std::lock_guard lock(m_mutex);
            HRESULT hr = CheckShutdown();
            if (FAILED(hr)) return hr;
            q = m_queue;
        }
        return q->GetEvent(flags, ev);
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        return FAILED(hr) ? hr : m_queue->BeginGetEvent(cb, state);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** ev) override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        return FAILED(hr) ? hr : m_queue->EndGetEvent(res, ev);
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status,
                            const PROPVARIANT* value) override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        return FAILED(hr) ? hr : m_queue->QueueEventParamVar(met, ext, status, value);
    }

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override {
        if (!pd) return E_POINTER;
        *pd = nullptr;
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        return FAILED(hr) ? hr : m_pd->Clone(pd);
    }
    STDMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        ComPtr<HBStream> stream;
        {
            std::lock_guard lock(m_mutex);
            m_state = State::Shutdown;
            m_attrs.Reset();
            m_pd.Reset();
            if (m_queue) {
                m_queue->Shutdown();
                m_queue.Reset();
            }
            stream = m_stream;
            m_stream.Reset();
        }
        if (stream) stream->Shutdown();
        Log("source shutdown");
        return S_OK;
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                       const PROPVARIANT* startPos) override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (!pd || !startPos) return E_INVALIDARG;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        if (m_state == State::Invalid) return MF_E_INVALID_STATE_TRANSITION;

        DWORD count = 0;
        HB_RETURN_IF_FAILED(pd->GetStreamDescriptorCount(&count));
        if (count != 1) return E_INVALIDARG;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        HB_RETURN_IF_FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &sd));

        PROPVARIANT startTime;
        PropVariantInit(&startTime);
        startTime.vt = VT_I8;
        startTime.hVal.QuadPart = MFGetSystemTime();

        if (selected) {
            HB_RETURN_IF_FAILED(m_pd->SelectStream(0));
            ComPtr<IUnknown> stream;
            HB_RETURN_IF_FAILED(m_stream.As(&stream));
            HB_RETURN_IF_FAILED(m_queue->QueueEventParamUnk(m_streamSelected ? MEUpdatedStream : MENewStream,
                                                            GUID_NULL, S_OK, stream.Get()));
            HB_RETURN_IF_FAILED(m_stream->Start());
            m_streamSelected = true;
        } else if (m_streamSelected) {
            HB_RETURN_IF_FAILED(m_pd->DeselectStream(0));
            m_stream->Stop(false);
            m_streamSelected = false;
        }
        HB_RETURN_IF_FAILED(m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime));
        m_state = State::Started;
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (m_state != State::Started) return MF_E_INVALID_STATE_TRANSITION;
        m_state = State::Stopped;
        m_stream->Stop(true);
        m_streamSelected = false;
        m_pd->DeselectStream(0);
        PROPVARIANT stopTime;
        PropVariantInit(&stopTime);
        stopTime.vt = VT_I8;
        stopTime.hVal.QuadPart = MFGetSystemTime();
        return m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime);
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** a) override {
        if (!a) return E_POINTER;
        *a = nullptr;
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        return FAILED(hr) ? hr : m_attrs.CopyTo(a);
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** a) override {
        if (!a) return E_POINTER;
        *a = nullptr;
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (id != kStreamId) return MF_E_NOT_FOUND;
        return m_stream->GetAttributes(a);
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return E_NOTIMPL; }

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* obj) override {
        if (!obj) return E_POINTER;
        *obj = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IKsControl: no custom controls.
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override {
        std::lock_guard lock(m_mutex);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (outputStreamId != kStreamId) return MF_E_NOT_FOUND;
        return m_stream->SetAllocator(allocator);
    }
    STDMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                   MFSampleAllocatorUsage* usage) override {
        if (!inputStreamId || !usage) return E_POINTER;
        if (outputStreamId != kStreamId) return MF_E_NOT_FOUND;
        *inputStreamId = outputStreamId;
        *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
        return S_OK;
    }

private:
    enum class State { Invalid, Stopped, Started, Shutdown };
    HRESULT CheckShutdown() const {
        if (m_state == State::Shutdown) return MF_E_SHUTDOWN;
        if (!m_queue || !m_stream) return E_UNEXPECTED;
        return S_OK;
    }

    std::mutex m_mutex;
    State m_state = State::Invalid;
    bool m_streamSelected = false;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFPresentationDescriptor> m_pd;
    ComPtr<IMFAttributes> m_attrs;
    ComPtr<HBStream> m_stream;
};

// ----------------------------------------------------------------- activator
// The CLSID passed to MFCreateVirtualCamera must be a class implementing
// IMFActivate; Frame Server fills it with attributes and calls ActivateObject.
class HBActivate final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    HBActivate() { g_objects++; }
    ~HBActivate() override { g_objects--; }

    HRESULT Initialize() {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        return MFCreateAttributes(&m_attrs, 4);
    }

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        Log("ActivateObject");
        auto source = Make<HBSource>();
        if (!source) return E_OUTOFMEMORY;
        HB_RETURN_IF_FAILED(source->Initialize(m_attrs.Get()));
        return source->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override { return S_OK; }

#define HB_FWD(call) { return m_attrs->call; }
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override HB_FWD(GetItem(k, v))
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override HB_FWD(GetItemType(k, t))
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override HB_FWD(CompareItem(k, v, r))
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override HB_FWD(Compare(o, m, r))
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override HB_FWD(GetUINT32(k, v))
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override HB_FWD(GetUINT64(k, v))
    STDMETHODIMP GetDouble(REFGUID k, double* v) override HB_FWD(GetDouble(k, v))
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override HB_FWD(GetGUID(k, v))
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override HB_FWD(GetStringLength(k, n))
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 cch, UINT32* n) override HB_FWD(GetString(k, s, cch, n))
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override HB_FWD(GetAllocatedString(k, s, n))
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override HB_FWD(GetBlobSize(k, n))
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 cb, UINT32* n) override HB_FWD(GetBlob(k, b, cb, n))
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override HB_FWD(GetAllocatedBlob(k, b, n))
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* o) override HB_FWD(GetUnknown(k, i, o))
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override HB_FWD(SetItem(k, v))
    STDMETHODIMP DeleteItem(REFGUID k) override HB_FWD(DeleteItem(k))
    STDMETHODIMP DeleteAllItems() override HB_FWD(DeleteAllItems())
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override HB_FWD(SetUINT32(k, v))
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override HB_FWD(SetUINT64(k, v))
    STDMETHODIMP SetDouble(REFGUID k, double v) override HB_FWD(SetDouble(k, v))
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override HB_FWD(SetGUID(k, v))
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override HB_FWD(SetString(k, v))
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 cb) override HB_FWD(SetBlob(k, b, cb))
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override HB_FWD(SetUnknown(k, u))
    STDMETHODIMP LockStore() override HB_FWD(LockStore())
    STDMETHODIMP UnlockStore() override HB_FWD(UnlockStore())
    STDMETHODIMP GetCount(UINT32* n) override HB_FWD(GetCount(n))
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override HB_FWD(GetItemByIndex(i, k, v))
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override HB_FWD(CopyAllItems(d))
#undef HB_FWD

private:
    ComPtr<IMFAttributes> m_attrs;
};

class HBClassFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto a = Make<HBActivate>();
        if (!a) return E_OUTOFMEMORY;
        HB_RETURN_IF_FAILED(a->Initialize());
        return a->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};

// ----------------------------------------------------------- registry helpers
std::wstring ClsidKey() {
    return std::wstring(L"Software\\Classes\\CLSID\\") + hb::vcam::kMediaSourceClsidString;
}

}  // namespace

// ------------------------------------------------------------------ DLL entry
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, CLSID_HBMediaSourceActivate)) return CLASS_E_CLASSNOTAVAILABLE;
    auto f = Make<HBClassFactory>();
    if (!f) return E_OUTOFMEMORY;
    return f->QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

// regsvr32 (elevated) writes HKLM so the Frame Server (LocalService) can find us.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const std::wstring key = ClsidKey();
    const wchar_t* name = L"HB Studio Virtual Camera Source";
    LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, REG_SZ, name,
                                static_cast<DWORD>((wcslen(name) + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    const std::wstring inproc = key + L"\\InprocServer32";
    s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr, REG_SZ, path,
                        static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    const wchar_t* model = L"Both";
    s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, inproc.c_str(), L"ThreadingModel", REG_SZ, model,
                        static_cast<DWORD>((wcslen(model) + 1) * sizeof(wchar_t)));
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    const std::wstring key = ClsidKey();
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}
