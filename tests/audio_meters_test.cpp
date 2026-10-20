#include <gtest/gtest.h>

#include <cstdio>

#include "audio/meters.hpp"
#include "audio_test_util.hpp"

using namespace hb::audio;
using namespace hb::audio::test;

namespace {
double toDb(double lin) { return 20.0 * std::log10(lin); }
}

TEST(PeakMeter, AttackHoldAndDecay) {
    PeakMeter m;
    std::vector<float> loud(480 * 2, 0.0f), quiet(480 * 2, 0.0f);
    loud[10] = 1.0f;  // L peak = 0 dBFS
    m.process(loud.data(), 480);
    EXPECT_FLOAT_EQ(m.levelDb(0), 0.0f);
    EXPECT_TRUE(std::isinf(m.levelDb(1)));
    for (int i = 0; i < 50; ++i) m.process(quiet.data(), 480);  // 0.5 s
    EXPECT_NEAR(m.levelDb(0), -10.0f, 0.1f);  // 20 dB/s decay
    EXPECT_FLOAT_EQ(m.holdDb(0), 0.0f);       // still held (hold = 1.5 s)
    for (int i = 0; i < 150; ++i) m.process(quiet.data(), 480);  // to 2.0 s
    EXPECT_LT(m.holdDb(0), 0.0f);             // hold expired and falling
    for (int i = 0; i < 1000; ++i) m.process(quiet.data(), 480);
    EXPECT_TRUE(std::isinf(m.levelDb(0)));
}

TEST(RmsMeter, FullScaleSineIsMinus3dB) {
    RmsMeter m;
    auto x = stereoSine(1000, 1.0, 48000, 48000);
    m.process(x.data(), 48000);
    EXPECT_NEAR(m.rmsDb(0), -3.0103f, 0.01f);
    EXPECT_NEAR(m.rmsDb(1), -3.0103f, 0.01f);
}

TEST(RmsMeter, WindowForgetsOldSignal) {
    RmsMeter m(48000, 0.3);
    auto x = stereoSine(1000, 1.0, 48000, 48000);
    m.process(x.data(), 48000);
    std::vector<float> z(48000 * 2, 0.0f);
    m.process(z.data(), 48000);
    EXPECT_TRUE(std::isinf(m.rmsDb(0)));
}

TEST(TruePeak, DetectsIntersamplePeak) {
    // fs/4 tone with 45 degree phase: every sample is +-0.7071 (sample peak
    // -3.01 dBFS) but the reconstructed waveform peaks at 1.0 (0 dBTP).
    TruePeakMeter tp;
    PeakMeter pk;
    auto x = stereoSine(12000, 1.0, 48000, 4800, kPi / 4.0);
    tp.process(x.data(), 4800);
    pk.process(x.data(), 4800);
    EXPECT_NEAR(pk.levelDb(0), -3.01f, 0.05f);
    std::printf("[measure] fs/4 @45deg: sample peak %.2f dBFS, true peak %.3f dBTP\n", pk.levelDb(0), tp.maxDb(0));
    EXPECT_NEAR(tp.maxDb(0), 0.0f, 0.15f);
    EXPECT_NEAR(tp.maxDb(1), 0.0f, 0.15f);
}

TEST(TruePeak, NeverBelowSamplePeakAndUnityDc) {
    TruePeakMeter tp;
    auto x = stereoSine(997, 0.5, 48000, 4800, 0.3);
    tp.process(x.data(), 4800);
    EXPECT_GE(tp.maxDb(0), -6.0206f - 0.01f);
    EXPECT_NEAR(tp.maxDb(0), -6.02f, 0.05f);
    // A slow (100 Hz) sine passes through the 4x interpolator at unity gain.
    TruePeakMeter slow;
    auto y = stereoSine(100, 0.5, 48000, 4800);
    slow.process(y.data(), 4800);
    EXPECT_NEAR(slow.maxDb(0), -6.0206f, 0.03f);
}

TEST(KWeighting, ExactCoefficientsAt48k) {
    const KWeightCoeffs k = designKWeighting(48000.0);
    EXPECT_NEAR(k.shelf.b0, 1.53512485958697, 1e-12);
    EXPECT_NEAR(k.shelf.a1, -1.69065929318241, 1e-12);
    EXPECT_NEAR(k.highpass.a2, 0.99007225036621, 1e-12);
}

TEST(KWeighting, DerivedDesignMatchesTabulated48k) {
    // 48000.0001 forces the analytic path; it must reproduce the published numbers.
    const KWeightCoeffs a = designKWeighting(48000.0), b = designKWeighting(48000.0001);
    EXPECT_NEAR(a.shelf.b0, b.shelf.b0, 2e-6);
    EXPECT_NEAR(a.shelf.b1, b.shelf.b1, 2e-6);
    EXPECT_NEAR(a.shelf.b2, b.shelf.b2, 2e-6);
    EXPECT_NEAR(a.shelf.a1, b.shelf.a1, 2e-6);
    EXPECT_NEAR(a.shelf.a2, b.shelf.a2, 2e-6);
    EXPECT_NEAR(a.highpass.a1, b.highpass.a1, 2e-6);
    EXPECT_NEAR(a.highpass.a2, b.highpass.a2, 2e-6);
}

TEST(KWeighting, FrequencyResponse) {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        const KWeightCoeffs k = designKWeighting(fs);
        auto gainDb = [&](double hz) {
            return toDb(biquadMagnitude(k.shelf, hz, fs) * biquadMagnitude(k.highpass, hz, fs));
        };
        EXPECT_NEAR(gainDb(997), 0.691, 0.02) << fs;   // the reason for the -0.691 LKFS offset
        EXPECT_NEAR(gainDb(10000), 4.0, 0.3) << fs;    // head-related shelf
        EXPECT_LT(gainDb(20), -1.0) << fs;             // RLB high-pass
        EXPECT_NEAR(gainDb(100), -1.14, 0.1) << fs;  // RLB roll-off still visible at 100 Hz
        EXPECT_NEAR(gainDb(1000), 0.69, 0.05) << fs;
    }
}

namespace {
float measureStereoSine(double hz, double ampDb, bool both, double seconds, float* shortTerm) {
    LoudnessMeter m;
    const std::size_t frames = static_cast<std::size_t>(seconds * 48000);
    auto x = stereoSine(hz, std::pow(10.0, ampDb / 20.0), 48000, frames, 0.0, both, true);
    for (std::size_t off = 0; off < frames; off += 480) m.process(x.data() + 2 * off, 480);
    if (shortTerm) *shortTerm = m.shortTermLufs();
    return m.momentaryLufs();
}
}  // namespace

TEST(Loudness, EbuTech3341Style_1kHzMinus23) {
    float st = 0;
    const float mom = measureStereoSine(1000, -23.0, true, 20.0, &st);
    std::printf("[measure] 1 kHz stereo -23 dBFS/ch: M=%.3f LUFS S=%.3f LUFS\n", mom, st);
    EXPECT_NEAR(mom, -23.0f, 0.1f);
    EXPECT_NEAR(st, -23.0f, 0.1f);
}

TEST(Loudness, Minus33AndMonoChannel) {
    float st = 0;
    EXPECT_NEAR(measureStereoSine(1000, -33.0, true, 20.0, &st), -33.0f, 0.1f);
    EXPECT_NEAR(st, -33.0f, 0.1f);
    // Left channel only: power halves -> -3.01 LU.
    EXPECT_NEAR(measureStereoSine(1000, -23.0, false, 10.0, nullptr), -26.01f, 0.1f);
}

TEST(Loudness, AnalyticMatchAtOtherFrequencies) {
    // Expected = -0.691 + 10log10(sum_ch A^2/2 * |Hk(f)|^2), |Hk| from the biquads.
    const KWeightCoeffs k = designKWeighting(48000.0);
    for (double hz : {100.0, 500.0, 4000.0, 10000.0}) {
        const double A = 0.1;
        const double h = biquadMagnitude(k.shelf, hz, 48000) * biquadMagnitude(k.highpass, hz, 48000);
        const double expected = -0.691 + 10.0 * std::log10(2.0 * A * A / 2.0 * h * h);
        float st = 0;
        const float mom = measureStereoSine(hz, toDb(A), true, 6.0, &st);
        EXPECT_NEAR(mom, expected, 0.05) << hz;
        EXPECT_NEAR(st, expected, 0.05) << hz;
    }
}

TEST(Loudness, SilenceAndWindows) {
    LoudnessMeter m;
    EXPECT_TRUE(std::isinf(m.momentaryLufs()));
    std::vector<float> z(48000 * 2, 0.0f);
    m.process(z.data(), 48000);
    EXPECT_TRUE(std::isinf(m.momentaryLufs()));

    // 4 s of -23 then 0.4 s of silence: momentary drops to -inf, short-term remains.
    LoudnessMeter n;
    auto x = stereoSine(1000, std::pow(10.0, -23.0 / 20.0), 48000, 48000 * 4);
    n.process(x.data(), 48000 * 4);
    std::vector<float> z2(19200 * 2, 0.0f);
    n.process(z2.data(), 19200);
    EXPECT_TRUE(std::isinf(n.momentaryLufs()) || n.momentaryLufs() < -60.0f);
    EXPECT_GT(n.shortTermLufs(), -25.0f);
    EXPECT_LT(n.shortTermLufs(), -23.0f);
}
