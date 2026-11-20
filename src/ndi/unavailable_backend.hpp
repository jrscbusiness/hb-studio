#pragma once
// A safe no-op NdiBackend, used whenever the real NDI Runtime cannot be used: on every platform
// other than Windows, and on Windows itself when no installed Runtime could be found/loaded.
// Every method is a harmless no-op per the NdiBackend contract (ndi.hpp): null handle, empty
// list, "timeout" (false, no error) — never a crash, never a null-pointer dereference downstream.
#include "ndi/ndi.hpp"

namespace hb::ndi {

class UnavailableNdiBackend final : public NdiBackend {
public:
    explicit UnavailableNdiBackend(std::string reason) : reason_(std::move(reason)) {}

    bool available() const override { return false; }
    std::string runtimeVersion() const override { return ""; }

    void* findCreate() override { return nullptr; }
    void findDestroy(void*) override {}
    std::vector<NdiSourceInfo> findCurrentSources(void*) override { return {}; }

    void* recvCreate(const NdiSourceInfo&) override { return nullptr; }
    void recvDestroy(void*) override {}
    bool recvCaptureVideo(void*, int, std::vector<std::uint8_t>&, int&, int&, double&, std::string& error) override {
        // Not a hard failure from the caller's point of view (NdiSource reports NoSignal/Connecting
        // from an empty backend rather than a scary "Error" state): behave like a permanent timeout.
        (void)error;
        return false;
    }

    void* sendCreate(const std::string&) override { return nullptr; }
    void sendDestroy(void*) override {}
    void sendVideo(void*, const std::uint8_t*, int, int, int, int, int) override {}

    const std::string& reason() const { return reason_; }

private:
    std::string reason_;
};

}  // namespace hb::ndi
