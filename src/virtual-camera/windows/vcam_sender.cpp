#include "virtual-camera/windows/vcam_sender.hpp"

#include <chrono>

#include "virtual-camera/windows/vcam_protocol.hpp"

namespace hb::vcam {

namespace {
bool writeAll(HANDLE h, const void* data, DWORD bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (bytes > 0) {
        DWORD written = 0;
        if (!WriteFile(h, p, bytes, &written, nullptr) || written == 0) return false;
        p += written;
        bytes -= written;
    }
    return true;
}
}  // namespace

void PipeFrameSender::start() {
    {
        std::lock_guard lock(mutex_);
        if (running_) return;
        running_ = true;
    }
    consumerId_ = program_.addConsumer([this](const std::shared_ptr<const video::Frame>& f) {
        {
            std::lock_guard lock(mutex_);
            latest_ = f;
            ++latestSeq_;
        }
        cv_.notify_one();
    });
    thread_ = std::thread([this] { run(); });
}

void PipeFrameSender::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    program_.removeConsumer(consumerId_);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_ = false;
}

void PipeFrameSender::run() {
    using namespace std::chrono_literals;
    std::uint64_t lastSent = 0;
    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!running_) return;
        }
        HANDLE pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            // No camera client yet (source not loaded) or all instances busy.
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, 300ms, [this] { return !running_; });
            continue;
        }
        connected_ = true;
        for (;;) {
            std::shared_ptr<const video::Frame> frame;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return !running_ || latestSeq_ != lastSent; });
                if (!running_) break;
                frame = latest_;
                lastSent = latestSeq_;
            }
            if (!frame || frame->width != kWidth || frame->height != kHeight) continue;
            FrameHeader h{kMagic, static_cast<std::uint32_t>(frame->width),
                          static_cast<std::uint32_t>(frame->height),
                          static_cast<std::uint32_t>(frame->stride),
                          static_cast<std::uint32_t>(frame->data.size())};
            if (!writeAll(pipe, &h, sizeof(h)) ||
                !writeAll(pipe, frame->data.data(), static_cast<DWORD>(frame->data.size()))) {
                break;  // camera closed: go back to reconnecting
            }
            sent_.fetch_add(1);
        }
        connected_ = false;
        CloseHandle(pipe);
    }
}

}  // namespace hb::vcam
