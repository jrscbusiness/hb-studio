// NdiSender: publishes the Program Output over the network as an NDI source. Portable (just calls
// through NdiBackend); the real sending ABI lives in ndi_backend_win.cpp.
#include "ndi/ndi.hpp"

#include <atomic>
#include <mutex>

namespace hb::ndi {

struct NdiSender::Impl {
    std::mutex mutex;
    void* handle = nullptr;
    std::atomic<bool> createAttempted{false};
};

NdiSender::NdiSender(std::string ndiName, std::shared_ptr<NdiBackend> backend)
    : name_(std::move(ndiName)), backend_(std::move(backend)), p_(std::make_unique<Impl>()) {}

NdiSender::~NdiSender() {
    std::lock_guard lock(p_->mutex);
    if (p_->handle) backend_->sendDestroy(p_->handle);
}

bool NdiSender::available() const { return backend_->available(); }

std::string NdiSender::unavailableMessage() const {
    if (backend_->available()) return "";
    return "NDI Runtime não encontrado. Instale gratuitamente em ndi.video/tools para enviar o Program "
           "como uma fonte NDI na rede; o restante do HB Studio funciona normalmente sem ele.";
}

bool NdiSender::active() const {
    std::lock_guard lock(p_->mutex);
    return p_->handle != nullptr;
}

void NdiSender::feed(const std::uint8_t* bgra, int width, int height, int stride, int fpsNum, int fpsDen) {
    if (!bgra || width <= 0 || height <= 0) return;
    std::lock_guard lock(p_->mutex);
    // Created lazily on the first real frame (not in the constructor): avoids creating/destroying
    // a send handle for callers that construct a sender but never actually feed it, and retries on
    // every call until it succeeds (e.g. the Runtime gets installed after HB Studio already started).
    if (!p_->handle) {
        if (!backend_->available()) return;
        p_->handle = backend_->sendCreate(name_);
        if (!p_->handle) return;
    }
    backend_->sendVideo(p_->handle, bgra, width, height, stride, fpsNum, fpsDen);
}

}  // namespace hb::ndi
