#include "captions/audio_tap.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace hb::captions {

AudioTapQueue::AudioTapQueue(std::size_t capacityFrames) : ring_(std::max<std::size_t>(capacityFrames, 480) * 2), cap_(ring_.size() / 2) {}

void AudioTapQueue::push(const float* stereo, std::size_t frames) noexcept {
    if (!stereo || frames == 0) return;
    std::lock_guard lock(mu_);
    if (closed_) return;
    if (frames > cap_) {   // keep only the newest `cap_` frames
        dropped_ += frames - cap_;
        stereo += (frames - cap_) * 2;
        frames = cap_;
    }
    const std::size_t free = cap_ - count_;
    if (frames > free) {   // drop oldest
        const std::size_t drop = frames - free;
        head_ = (head_ + drop) % cap_;
        count_ -= drop;
        dropped_ += drop;
    }
    std::size_t tail = (head_ + count_) % cap_;
    const std::size_t first = std::min(frames, cap_ - tail);
    std::memcpy(&ring_[tail * 2], stereo, first * 2 * sizeof(float));
    if (frames > first) std::memcpy(&ring_[0], stereo + first * 2, (frames - first) * 2 * sizeof(float));
    count_ += frames;
    cv_.notify_one();
}

std::size_t AudioTapQueue::pop(float* out, std::size_t maxFrames) {
    std::lock_guard lock(mu_);
    const std::size_t n = std::min(maxFrames, count_);
    const std::size_t first = std::min(n, cap_ - head_);
    if (n) {
        std::memcpy(out, &ring_[head_ * 2], first * 2 * sizeof(float));
        if (n > first) std::memcpy(out + first * 2, &ring_[0], (n - first) * 2 * sizeof(float));
        head_ = (head_ + n) % cap_;
        count_ -= n;
    }
    return n;
}

bool AudioTapQueue::waitForData(int timeoutMs) {
    std::unique_lock lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return count_ > 0 || closed_; });
    return count_ > 0;
}

void AudioTapQueue::close() {
    { std::lock_guard lock(mu_); closed_ = true; }
    cv_.notify_all();
}

void AudioTapQueue::reopen() {
    std::lock_guard lock(mu_);
    closed_ = false;
}

void AudioTapQueue::clear() {
    std::lock_guard lock(mu_);
    head_ = 0;
    count_ = 0;
}

std::size_t AudioTapQueue::sizeFrames() const {
    std::lock_guard lock(mu_);
    return count_;
}

std::uint64_t AudioTapQueue::droppedFrames() const {
    std::lock_guard lock(mu_);
    return dropped_;
}

ProgramDownmixer::ProgramDownmixer() { reset(); }

void ProgramDownmixer::reset() {
    constexpr std::size_t kChunk = 4096;
    rs_ = std::make_unique<hb::audio::Resampler>(kProgramRate, 16000, 1, hb::audio::ResamplerQuality::Low, kChunk);
    mono_.assign(kChunk, 0.0f);
    res_.assign(rs_->maxOutputFrames(kChunk) + 8, 0.0f);
}

void ProgramDownmixer::process(const float* stereo, std::size_t frames, std::vector<float>& out) {
    if (!stereo) return;
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(mono_.size(), frames - done);
        for (std::size_t i = 0; i < n; ++i) mono_[i] = 0.5f * (stereo[(done + i) * 2] + stereo[(done + i) * 2 + 1]);
        const std::size_t produced = rs_->process(mono_.data(), n, res_.data(), res_.size());
        out.insert(out.end(), res_.begin(), res_.begin() + static_cast<std::ptrdiff_t>(produced));
        done += n;
    }
}

}  // namespace hb::captions
