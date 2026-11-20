#include "ndi/ndi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace hb::ndi {

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kPollSlice = std::chrono::milliseconds(10);  // granularity of the stop check
}  // namespace

struct NdiDiscovery::Impl {
    explicit Impl(std::shared_ptr<NdiBackend> b) : backend(std::move(b)) {}
    ~Impl() {
        stopFlag.store(true);
        if (thread.joinable()) thread.join();
    }

    void ensureStarted() {
        bool expected = false;
        if (started.compare_exchange_strong(expected, true)) thread = std::thread([this] { run(); });
    }

    // Owns one `find` handle for the lifetime of the thread: created once, destroyed once. Between
    // polls it only asks the backend for its current snapshot (cheap; the backend does the actual
    // mDNS/Discovery-Server listening in the background on its own).
    void run() {
        void* handle = nullptr;
        while (!stopFlag.load(std::memory_order_relaxed)) {
            if (!handle) handle = backend->findCreate();  // retried every poll until it succeeds
            if (handle) {
                auto list = backend->findCurrentSources(handle);
                std::lock_guard lock(mutex);
                sources = std::move(list);
            }
            auto remaining = kPollInterval;
            while (remaining.count() > 0 && !stopFlag.load(std::memory_order_relaxed)) {
                const auto step = std::min(remaining, kPollSlice);
                std::this_thread::sleep_for(step);
                remaining -= step;
            }
        }
        if (handle) backend->findDestroy(handle);
    }

    std::shared_ptr<NdiBackend> backend;
    std::atomic<bool> started{false};
    std::atomic<bool> stopFlag{false};
    std::thread thread;
    mutable std::mutex mutex;
    std::vector<NdiSourceInfo> sources;
};

NdiDiscovery::NdiDiscovery(std::shared_ptr<NdiBackend> backend) : p_(std::make_unique<Impl>(std::move(backend))) {}
NdiDiscovery::~NdiDiscovery() = default;

bool NdiDiscovery::available() const { return p_->backend->available(); }

std::string NdiDiscovery::unavailableMessage() const {
    if (p_->backend->available()) return "";
    return "NDI Runtime não encontrado. Instale gratuitamente em ndi.video/tools para detectar câmeras NDI "
           "na rede; o restante do HB Studio funciona normalmente sem ele.";
}

std::vector<NdiSourceInfo> NdiDiscovery::sources() const {
    p_->ensureStarted();
    std::lock_guard lock(p_->mutex);
    return p_->sources;
}

}  // namespace hb::ndi
