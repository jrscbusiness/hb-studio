#include "audio_io/device_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

namespace hb::audio_io {

const char* streamStateName(StreamState s) noexcept {
    switch (s) {
        case StreamState::Idle: return "idle";
        case StreamState::Connecting: return "connecting";
        case StreamState::Online: return "online";
        case StreamState::NoSignal: return "nosignal";
        case StreamState::Error: return "error";
    }
    return "idle";
}

DeviceStream::DeviceStream(DeviceKind kind, std::string deviceId, StreamOptions options)
    : kind_(kind), deviceId_(std::move(deviceId)), opt_(std::move(options)) {
    if (!opt_.opener) opt_.opener = defaultDeviceOpener();
    opt_.noSignalMs = std::max(50, opt_.noSignalMs);
    opt_.backoffInitialMs = std::max(1, opt_.backoffInitialMs);
    opt_.backoffMaxMs = std::max(opt_.backoffInitialMs, opt_.backoffMaxMs);
    message_ = "Parado.";
}

DeviceStream::~DeviceStream() { shutdown(); }

std::int64_t DeviceStream::nowNs() const {
    if (opt_.clockNs) return opt_.clockNs();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void DeviceStream::noteData(std::size_t frames) noexcept {
    try {
        lastData_.store(nowNs(), std::memory_order_relaxed);
    } catch (...) {
    }
    frames_.fetch_add(frames, std::memory_order_relaxed);
}

void DeviceStream::setStatus(StreamState st, std::string message) {
    std::lock_guard lock(statusMutex_);
    state_ = st;
    message_ = std::move(message);
}

StreamStatus DeviceStream::status() const {
    StreamStatus s;
    {
        std::lock_guard lock(statusMutex_);
        s.state = state_;
        s.message = message_;
        s.sampleRate = rate_;
        s.nativeChannels = chans_;
        s.opens = opens_;
    }
    s.deviceId = deviceId_;
    s.frames = frames_.load(std::memory_order_relaxed);
    fillCounters(s);
    return s;
}

void DeviceStream::start() {
    {
        std::lock_guard lock(tickMutex_);
        if (running_) return;
        running_ = true;
        nextAttempt_ = 0;
        backoffMs_ = opt_.backoffInitialMs;
        setStatus(StreamState::Connecting, "Conectando ao dispositivo...");
    }
    if (opt_.supervisor) {
        {
            std::lock_guard lock(wakeMutex_);
            stopThread_ = false;
        }
        thread_ = std::thread([this] { loop(); });  // opens the device without blocking the caller
    }
}

void DeviceStream::loop() {
    for (;;) {
        try {
            tick();
        } catch (...) {
        }
        std::unique_lock lock(wakeMutex_);
        if (wake_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopThread_; })) return;
    }
}

void DeviceStream::shutdown() { stop(); }

void DeviceStream::stop() {
    {
        std::lock_guard lock(wakeMutex_);
        stopThread_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(tickMutex_);
    running_ = false;
    closeDevice();
    {
        std::lock_guard sl(statusMutex_);
        rate_ = chans_ = 0;
    }
    setStatus(StreamState::Idle, "Parado.");
}

void DeviceStream::closeDevice() {
    if (!dev_) return;
    dev_.reset();  // joins the OS audio thread: no callback runs after this line
    onClosed();
    std::lock_guard sl(statusMutex_);
    rate_ = chans_ = 0;
}

void DeviceStream::attemptOpen(std::int64_t now) {
    OpenRequest req;
    req.kind = kind_;
    req.deviceId = deviceId_;
    req.backend = opt_.backend;
    req.periodMs = opt_.periodMs;
    std::string err;
    std::unique_ptr<RawDevice> dev;
    try {
        dev = opt_.opener(req, err);
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "falha inesperada";
    }
    if (dev) {
        try {
            const int rate = dev->sampleRate();
            RawCallbacks cb = onOpened(rate);
            lastData_.store(INT64_MIN, std::memory_order_relaxed);
            if (dev->start(std::move(cb), err)) {
                dev_ = std::move(dev);
                openedAt_ = now;
                backoffMs_ = opt_.backoffInitialMs;
                {
                    std::lock_guard sl(statusMutex_);
                    rate_ = rate;
                    chans_ = dev_->nativeChannels();
                    ++opens_;
                }
                setStatus(StreamState::Connecting, "Conectando ao dispositivo...");
                return;
            }
            dev.reset();
            onClosed();
        } catch (const std::exception& e) {
            err = e.what();
            dev.reset();
        } catch (...) {
            err = "falha inesperada";
            dev.reset();
        }
    }
    if (err.empty()) err = "motivo desconhecido";
    const int waitMs = backoffMs_;
    nextAttempt_ = now + static_cast<std::int64_t>(waitMs) * 1000000;
    backoffMs_ = std::min(backoffMs_ * 2, opt_.backoffMaxMs);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", std::ceil(waitMs / 100.0) / 10.0);
    std::string secs = buf;
    std::replace(secs.begin(), secs.end(), '.', ',');
    setStatus(StreamState::Error, "Erro: " + err + " Nova tentativa em " + secs + " s.");
}

void DeviceStream::tick() {
    try {
        std::lock_guard lock(tickMutex_);
        if (!running_) return;
        const std::int64_t now = nowNs();
        if (!dev_) {
            if (now >= nextAttempt_) attemptOpen(now);
            return;
        }
        if (!dev_->alive()) {
            closeDevice();
            backoffMs_ = opt_.backoffInitialMs;
            nextAttempt_ = now + static_cast<std::int64_t>(backoffMs_) * 1000000;
            setStatus(StreamState::Error, "Erro: o dispositivo foi desconectado ou parou de responder. Reconectando...");
            return;
        }
        const std::int64_t last = lastData_.load(std::memory_order_relaxed);
        const std::int64_t limit = static_cast<std::int64_t>(opt_.noSignalMs) * 1000000;
        if (last != INT64_MIN) {
            if (now - last <= limit) setStatus(StreamState::Online, "Online.");
            else setStatus(StreamState::NoSignal, "Sem sinal: o dispositivo parou de enviar áudio.");
        } else if (now - openedAt_ > limit) {
            setStatus(StreamState::NoSignal, "Sem sinal: o dispositivo está aberto mas não envia áudio.");
        } else {
            setStatus(StreamState::Connecting, "Conectando ao dispositivo...");
        }
    } catch (...) {
        // never propagate: a supervisor failure must not take the application down
    }
}

}  // namespace hb::audio_io
