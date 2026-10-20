#include "audio/meters.hpp"

#include <algorithm>
#include <cmath>

namespace hb::audio {

// ---------------------------------------------------------------- PeakMeter
PeakMeter::PeakMeter(PeakMeterConfig cfg) : cfg_(cfg) {}

void PeakMeter::reset() noexcept {
    level_ = {kNegInfDb, kNegInfDb};
    hold_ = {kNegInfDb, kNegInfDb};
    holdTimer_ = {0, 0};
}

void PeakMeter::process(const float* in, std::size_t frames) noexcept {
    float pk[2] = {0, 0};
    for (std::size_t i = 0; i < frames; ++i) {
        pk[0] = std::max(pk[0], std::fabs(in[2 * i]));
        pk[1] = std::max(pk[1], std::fabs(in[2 * i + 1]));
    }
    update(pk[0], pk[1], frames);
}

void PeakMeter::update(float peakL, float peakR, std::size_t frames) noexcept {
    const double dt = static_cast<double>(frames) / cfg_.sampleRate;
    const float fall = static_cast<float>(cfg_.decayDbPerSecond * dt);
    const float pk[2] = {peakL, peakR};
    for (int c = 0; c < 2; ++c) {
        const float db = linearToDb(pk[c]);
        float lv = std::max(db, level_[c] - fall);
        if (lv < -144.0f) lv = kNegInfDb;
        level_[c] = lv;
        if (db >= hold_[c]) {
            hold_[c] = db;
            holdTimer_[c] = cfg_.holdSeconds;
        } else if (holdTimer_[c] > 0) {
            holdTimer_[c] -= dt;
        } else {
            float h = std::max(lv, hold_[c] - fall);
            if (h < -144.0f) h = kNegInfDb;
            hold_[c] = h;
        }
    }
}

// ----------------------------------------------------------------- RmsMeter
RmsMeter::RmsMeter(int sampleRate, double windowSeconds)
    : n_(std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate * windowSeconds))) {
    sq_[0].assign(n_, 0.0);
    sq_[1].assign(n_, 0.0);
}

void RmsMeter::reset() noexcept {
    std::fill(sq_[0].begin(), sq_[0].end(), 0.0);
    std::fill(sq_[1].begin(), sq_[1].end(), 0.0);
    sum_ = {0, 0};
    pos_ = 0;
}

void RmsMeter::process(const float* in, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const double x = in[2 * i + c];
            const double s = x * x;
            sum_[c] += s - sq_[c][pos_];
            sq_[c][pos_] = s;
        }
        if (++pos_ == n_) {
            pos_ = 0;
            // Re-sum once per window so running-sum rounding never accumulates.
            for (int c = 0; c < 2; ++c) {
                double t = 0;
                for (double v : sq_[c]) t += v;
                sum_[c] = t;
            }
        }
    }
}

float RmsMeter::rmsDb(int ch) const noexcept {
    const double ms = std::max(0.0, sum_[ch]) / static_cast<double>(n_);
    if (ms < 1e-20) return kNegInfDb;
    return static_cast<float>(10.0 * std::log10(ms));
}

// ------------------------------------------------------------ TruePeakMeter
namespace {
// BS.1770-4 Annex 2: 48-tap FIR as 4 polyphase branches of 12 taps.
// Branch p, applied to x[n-11..n] (oldest first), yields the output at
// fractional position p/4. Branches 0/3 and 1/2 are mirror images.
constexpr float kTpPhase[4][12] = {
    {0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f,
     -0.0594482421875f, 0.1373291015625f, 0.9721679687500f, -0.1022949218750f,
     0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f},
    {-0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f,
     -0.1665039062500f, 0.4650878906250f, 0.7797851562500f, -0.2003173828125f,
     0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f},
    {-0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f,
     -0.2003173828125f, 0.7797851562500f, 0.4650878906250f, -0.1665039062500f,
     0.0891113281250f, -0.0517578125000f, 0.0292968750000f, -0.0291748046875f},
    {-0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f,
     -0.1022949218750f, 0.9721679687500f, 0.1373291015625f, -0.0594482421875f,
     0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f},
};
}  // namespace

void TruePeakMeter::reset() noexcept {
    for (auto& h : hist_) h.fill(0.0f);
    pos_ = {0, 0};
    max_ = {0, 0};
}

void TruePeakMeter::process(const float* in, std::size_t frames) noexcept {
    for (int c = 0; c < 2; ++c) {
        auto& h = hist_[c];
        int pos = pos_[c];
        float mx = max_[c];
        for (std::size_t i = 0; i < frames; ++i) {
            const float x = in[2 * i + c];
            h[pos] = x;
            h[pos + kTaps] = x;
            pos = (pos + 1) % kTaps;
            // Window oldest..newest is h[pos .. pos+11].
            const float* w = &h[pos];
            for (int p = 0; p < 4; ++p) {
                float acc = 0;
                for (int k = 0; k < kTaps; ++k) acc += kTpPhase[p][k] * w[k];
                mx = std::max(mx, std::fabs(acc));
            }
        }
        pos_[c] = pos;
        max_[c] = mx;
    }
}

float TruePeakMeter::maxDb(int ch) const noexcept { return linearToDb(max_[ch]); }

// -------------------------------------------------------------- K-weighting
KWeightCoeffs designKWeighting(double fs) {
    KWeightCoeffs k;
    if (fs == 48000.0) {
        k.shelf = {1.53512485958697, -2.69169618940638, 1.19839281085285,
                   -1.69065929318241, 0.73248077421585};
        k.highpass = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
        return k;
    }
    {  // stage 1: high shelf, +4 dB
        const double G = 3.999843853973347, Q = 0.7071752369554196, fc = 1681.9744509555319;
        const double K = std::tan(kPi * fc / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        k.shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        k.shelf.b1 = 2.0 * (K * K - Vh) / a0;
        k.shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        k.shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        k.shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {  // stage 2: RLB high-pass
        const double Q = 0.5003270373238773, fc = 38.13547087613982;
        const double K = std::tan(kPi * fc / fs);
        const double a0 = 1.0 + K / Q + K * K;
        k.highpass = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    }
    return k;
}

// ------------------------------------------------------------ LoudnessMeter
LoudnessMeter::LoudnessMeter(int sampleRate)
    : stepFrames_(static_cast<std::size_t>(std::max(1, sampleRate / 10))) {
    const KWeightCoeffs k = designKWeighting(sampleRate);
    for (int c = 0; c < 2; ++c) {
        shelf_[c].setCoeffs(k.shelf);
        hp_[c].setCoeffs(k.highpass);
    }
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (int c = 0; c < 2; ++c) {
        shelf_[c].reset();
        hp_[c].reset();
        ring_[c].fill(0.0);
    }
    acc_ = {0, 0};
    accFrames_ = 0;
    head_ = 0;
    count_ = 0;
}

void LoudnessMeter::process(const float* in, std::size_t frames) noexcept {
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const double y = hp_[c].process(shelf_[c].process(in[2 * i + c]));
            acc_[c] += y * y;
        }
        if (++accFrames_ == stepFrames_) {
            for (int c = 0; c < 2; ++c) {
                ring_[c][head_] = acc_[c] / static_cast<double>(stepFrames_);
                acc_[c] = 0;
            }
            head_ = (head_ + 1) % kSteps;
            count_ = std::min(count_ + 1, kSteps);
            accFrames_ = 0;
        }
    }
}

float LoudnessMeter::window(std::size_t steps) const noexcept {
    const std::size_t n = std::min(steps, count_);
    if (n == 0) return kNegInfDb;
    double sum = 0;  // channel weights G = 1.0 for L and R
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t idx = (head_ + kSteps - 1 - k) % kSteps;
        sum += ring_[0][idx] + ring_[1][idx];
    }
    const double ms = sum / static_cast<double>(n);
    if (ms < 1e-20) return kNegInfDb;
    return static_cast<float>(-0.691 + 10.0 * std::log10(ms));
}

}  // namespace hb::audio
