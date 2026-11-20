#pragma once
// NDI (Network Device Interface) camera discovery and ingestion.
// FROZEN INTERFACE: src/ndi/ (implementation) and the application (wiring into CÂMERAS) are
// written against this header.
//
// HB Studio never bundles or statically links the NDI SDK/Runtime (its license does not allow
// redistributing the Runtime DLL, and the stable public struct layout this header's real backend
// binds against is reconstructed from NDI's own published ABI documentation, not copied from their
// source). Instead, on Windows, the real backend dynamically loads whatever NDI Runtime the user
// has installed (free download from ndi.video) via its documented `NDIlib_v4_load` dynamic-loading
// entry point. If no Runtime is installed, `available()` is false and a clear pt-BR message
// explains how to get one; nothing else in the app is affected.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video/source.hpp"

namespace hb::ndi {

struct NdiSourceInfo {
    std::string name;      // "PC-DO-ROGERIO (Camera 1)" — shown to the operator
    std::string urlAddress;  // NDI's internal address string; opaque, only meaningful to the backend
};

// Abstracts everything the module needs from an NDI implementation, so the discovery/ingestion
// logic is fully unit-testable without the real Runtime installed (a FakeNdiBackend can simulate
// sources and frames). The real backend (Windows only) dynamically loads the installed NDI Runtime.
class NdiBackend {
public:
    virtual ~NdiBackend() = default;

    // False if the real NDI Runtime could not be loaded (not installed, wrong architecture, ...).
    // A backend that is unavailable must make every other method a safe no-op (null handle /
    // empty list / timeout), never crash.
    virtual bool available() const = 0;
    virtual std::string runtimeVersion() const = 0;  // "" if unknown/unavailable

    // Discovery. Returns null on failure (never on "no sources yet" — that's an empty vector from
    // findCurrentSources). Implementations should discover both mDNS-advertised sources and ones
    // behind an NDI Discovery Server, if the installed Runtime is configured for one; that's
    // entirely the Runtime's own behaviour, this interface just exposes "whatever it currently
    // knows about".
    virtual void* findCreate() = 0;
    virtual void findDestroy(void* handle) = 0;
    virtual std::vector<NdiSourceInfo> findCurrentSources(void* handle) = 0;

    // Receiver: connects to one named source. Returns null on failure.
    virtual void* recvCreate(const NdiSourceInfo& source) = 0;
    virtual void recvDestroy(void* handle) = 0;
    // Blocks up to timeoutMs waiting for the next video frame (audio/metadata frames, if any, are
    // consumed internally and skipped). Returns false on timeout or a recoverable hiccup (`error`
    // empty in that case) and true with `bgra` resized to width*height*4 bytes (top-down, BGRA8,
    // already colour-converted from whatever NDI delivered — UYVY, NV12, etc. — by the backend)
    // on a real frame. Returns false with a non-empty `error` only for something the caller should
    // treat as a hard failure (recv handle gone bad, format we truly cannot convert, ...).
    virtual bool recvCaptureVideo(void* handle, int timeoutMs, std::vector<std::uint8_t>& bgra, int& width,
                                  int& height, double& fps, std::string& error) = 0;
};

// Real backend: Windows-only dynamic load of the installed NDI Runtime. Safe to call on any
// platform/if the Runtime is missing — available() just comes back false. Shared because discovery
// and every open source hold onto the same loaded library.
std::shared_ptr<NdiBackend> defaultBackend();

// Continuously (re)lists the NDI sources currently visible on the network. One instance is enough
// for the whole app (CÂMERAS tab lists its sources()); each NdiSource below opens its own receiver
// independently once the operator assigns one to a CAM slot.
class NdiDiscovery {
public:
    explicit NdiDiscovery(std::shared_ptr<NdiBackend> backend = defaultBackend());
    ~NdiDiscovery();
    NdiDiscovery(const NdiDiscovery&) = delete;
    NdiDiscovery& operator=(const NdiDiscovery&) = delete;

    bool available() const;                  // backend().available()
    std::string unavailableMessage() const;   // pt-BR, e.g. "Instale o NDI Runtime em ndi.video/tools..."

    // Current best-known list; refreshed internally (a background thread polls the backend every
    // ~1s while at least one caller has asked at least once — no polling before the first call).
    std::vector<NdiSourceInfo> sources() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// A video::LiveSource backed by one NDI sender on the network. Thread-safe; render() never blocks
// on the network (it scales the most recently received frame; NoSignal until the first one
// arrives, Error if the receiver can't be created or the backend reports a hard failure).
class NdiSource final : public video::LiveSource {
public:
    NdiSource(NdiSourceInfo source, std::shared_ptr<NdiBackend> backend = defaultBackend());
    ~NdiSource() override;
    NdiSource(const NdiSource&) = delete;
    NdiSource& operator=(const NdiSource&) = delete;

    void start();  // idempotent; spawns the receive thread
    void stop();   // idempotent; joins it

    const std::string& name() const override { return name_; }
    void render(video::Frame& frame, std::uint64_t index) const override;
    video::SourceStatus status() const override;

private:
    void run();

    NdiSourceInfo source_;
    std::string name_;
    std::shared_ptr<NdiBackend> backend_;
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace hb::ndi
