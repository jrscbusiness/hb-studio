#pragma once
// Lock-free single-producer / single-consumer ring buffer of floats.
// One thread calls write(), one (other) thread calls read(). Counters are
// observable from any thread. For interleaved audio, always read/write whole
// frames (multiples of the channel count) so frames never tear.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hb::audio {

class RingBuffer {
public:
    // Capacity is rounded up to a power of two (in floats).
    explicit RingBuffer(std::size_t minCapacity) {
        std::size_t c = 2;
        while (c < minCapacity) c <<= 1;
        buf_.assign(c, 0.0f);
        mask_ = c - 1;
    }

    std::size_t capacity() const noexcept { return buf_.size(); }

    // Producer. Writes as many floats as fit; the rest is dropped and counted
    // as an overrun. Returns floats written.
    std::size_t write(const float* src, std::size_t n) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t freeSpace = buf_.size() - static_cast<std::size_t>(head - tail);
        const std::size_t cnt = std::min(n, freeSpace);
        if (cnt < n) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
            droppedSamples_.fetch_add(n - cnt, std::memory_order_relaxed);
        }
        copyIn(head, src, cnt);
        head_.store(head + cnt, std::memory_order_release);
        return cnt;
    }

    // Consumer. Reads up to n floats. If fewer are available it counts an
    // underrun; the remainder of dst is left untouched.
    std::size_t read(float* dst, std::size_t n) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::size_t avail = static_cast<std::size_t>(head - tail);
        const std::size_t cnt = std::min(n, avail);
        if (cnt < n) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            missingSamples_.fetch_add(n - cnt, std::memory_order_relaxed);
        }
        copyOut(tail, dst, cnt);
        tail_.store(tail + cnt, std::memory_order_release);
        return cnt;
    }

    // Like read() but zero-fills what could not be read. Returns floats of real data.
    std::size_t readZeroFill(float* dst, std::size_t n) noexcept {
        const std::size_t got = read(dst, n);
        if (got < n) std::memset(dst + got, 0, (n - got) * sizeof(float));
        return got;
    }

    // Approximate fill level (exact when called from producer or consumer).
    std::size_t size() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }
    std::size_t freeSpace() const noexcept { return buf_.size() - size(); }

    std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }
    std::uint64_t missingSamples() const noexcept { return missingSamples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedSamples() const noexcept { return droppedSamples_.load(std::memory_order_relaxed); }
    void resetCounters() noexcept {
        underruns_ = 0; overruns_ = 0; missingSamples_ = 0; droppedSamples_ = 0;
    }

private:
    void copyIn(std::uint64_t pos, const float* src, std::size_t n) noexcept {
        const std::size_t i = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = std::min(n, buf_.size() - i);
        if (first) std::memcpy(&buf_[i], src, first * sizeof(float));
        if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(float));
    }
    void copyOut(std::uint64_t pos, float* dst, std::size_t n) noexcept {
        const std::size_t i = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = std::min(n, buf_.size() - i);
        if (first) std::memcpy(dst, &buf_[i], first * sizeof(float));
        if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(float));
    }

    std::vector<float> buf_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint64_t> underruns_{0}, overruns_{0}, missingSamples_{0}, droppedSamples_{0};
};

}  // namespace hb::audio
