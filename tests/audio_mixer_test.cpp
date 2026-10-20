#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "audio/limiter.hpp"
#include "audio/mixer.hpp"
#include "audio/sources.hpp"
#include "audio_test_util.hpp"

using namespace hb::audio;
using namespace hb::audio::test;

namespace {
constexpr float kSqrtHalf = 0.70710678f;

std::vector<float> dc(float v, std::size_t frames) { return std::vector<float>(frames * 2, v); }

MixerConfig cfgN(int n) {
    MixerConfig c;
    for (int i = 0; i < n; ++i) c.channelNames.push_back("CH" + std::to_string(i));
    return c;
}

// Runs `frames` frames in 480-frame blocks and returns the output.
std::vector<float> run(Mixer& m, const std::vector<std::vector<float>>& in, std::size_t frames) {
    std::vector<float> out(frames * 2);
    std::vector<const float*> ptrs(in.size());
    for (std::size_t off = 0; off < frames; off += 480) {
        const std::size_t n = std::min<std::size_t>(480, frames - off);
        for (std::size_t c = 0; c < in.size(); ++c) ptrs[c] = in[c].data() + 2 * off;
        m.process(ptrs.data(), out.data() + 2 * off, n);
    }
    return out;
}
float tailL(const std::vector<float>& o) { return o[o.size() - 2]; }
float tailR(const std::vector<float>& o) { return o[o.size() - 1]; }
}  // namespace

TEST(FaderCurve, EndpointsMonotonicAndRoundTrip) {
    EXPECT_TRUE(std::isinf(faderPositionToDb(0.0f)));
    EXPECT_LT(faderPositionToDb(0.0f), 0.0f);
    EXPECT_NEAR(faderPositionToDb(0.75f), 0.0f, 1e-5f);
    EXPECT_NEAR(faderPositionToDb(1.0f), 10.0f, 1e-5f);
    EXPECT_NEAR(faderPositionToDb(0.5f), -20.0f, 1e-4f);
    EXPECT_NEAR(faderPositionToDb(2.0f), 10.0f, 1e-5f);  // clamped
    float prev = faderPositionToDb(0.0f);
    for (int i = 1; i <= 1000; ++i) {
        const float p = static_cast<float>(i) / 1000.0f, d = faderPositionToDb(p);
        EXPECT_GT(d, prev) << p;
        prev = d;
    }
    for (float db : {-100.0f, -60.0f, -33.0f, -10.0f, 0.0f, 3.0f, 10.0f})
        EXPECT_NEAR(faderPositionToDb(dbToFaderPosition(db)), db, 1e-3f) << db;
}

TEST(PanLaw, ConstantPowerAndMinus3dBAtCentre) {
    float l, r;
    panGains(0.0f, l, r);
    EXPECT_NEAR(20 * std::log10(l), -3.0103, 0.001);
    EXPECT_NEAR(l, r, 1e-6f);
    for (int i = -100; i <= 100; ++i) {
        panGains(static_cast<float>(i) / 100.0f, l, r);
        EXPECT_NEAR(l * l + r * r, 1.0f, 1e-5f);
    }
    panGains(-1.0f, l, r);
    EXPECT_NEAR(l, 1.0f, 1e-6f);
    EXPECT_NEAR(r, 0.0f, 1e-6f);
    panGains(1.0f, l, r);
    EXPECT_NEAR(r, 1.0f, 1e-6f);
}

TEST(Mixer, GainFaderPanMath) {
    Mixer m(cfgN(1));
    m.setGainDb(0, 6.0206f);        // x2
    m.setFaderDb(0, -6.0206f);      // x0.5
    auto out = run(m, {dc(0.1f, 4800)}, 4800);
    EXPECT_NEAR(tailL(out), 0.1f * kSqrtHalf, 1e-4f);  // centre pan = -3 dB
    EXPECT_NEAR(tailR(out), 0.1f * kSqrtHalf, 1e-4f);

    m.setPan(0, -1.0f);
    out = run(m, {dc(0.1f, 4800)}, 4800);
    EXPECT_NEAR(tailL(out), 0.1f, 1e-4f);
    EXPECT_NEAR(tailR(out), 0.0f, 1e-5f);

    m.setPan(0, 1.0f);
    out = run(m, {dc(0.1f, 4800)}, 4800);
    EXPECT_NEAR(tailL(out), 0.0f, 1e-5f);
    EXPECT_NEAR(tailR(out), 0.1f, 1e-4f);
}

TEST(Mixer, FaderPositionAndMinusInfinity) {
    Mixer m(cfgN(1));
    m.setFaderPosition(0, 0.0f);
    EXPECT_TRUE(std::isinf(m.faderDb(0)));
    auto out = run(m, {dc(0.5f, 4800)}, 4800);
    EXPECT_EQ(tailL(out), 0.0f);
    m.setFaderPosition(0, 0.75f);
    out = run(m, {dc(0.5f, 4800)}, 4800);
    EXPECT_NEAR(tailL(out), 0.5f * kSqrtHalf, 1e-4f);
    m.setFaderDb(0, 40.0f);  // clamped to +10 dB
    EXPECT_NEAR(m.faderDb(0), 10.0f, 1e-6f);
}

TEST(Mixer, SumsChannelsOnProgramBus) {
    Mixer m(cfgN(3));
    auto out = run(m, {dc(0.1f, 4800), dc(0.1f, 4800), dc(0.2f, 4800)}, 4800);
    EXPECT_NEAR(tailL(out), 0.4f * kSqrtHalf, 1e-4f);
}

TEST(Mixer, MuteAndSoloInPlace) {
    Mixer m(cfgN(3));
    std::vector<std::vector<float>> in = {dc(0.1f, 4800), dc(0.2f, 4800), dc(0.4f, 4800)};
    m.setMute(0, true);
    EXPECT_NEAR(tailL(run(m, in, 4800)), 0.6f * kSqrtHalf, 1e-4f);
    m.setMute(0, false);

    m.setSolo(1, true);
    EXPECT_NEAR(tailL(run(m, in, 4800)), 0.2f * kSqrtHalf, 1e-4f);
    m.setSolo(2, true);
    EXPECT_NEAR(tailL(run(m, in, 4800)), 0.6f * kSqrtHalf, 1e-4f);
    m.setMute(2, true);  // mute wins over solo
    EXPECT_NEAR(tailL(run(m, in, 4800)), 0.2f * kSqrtHalf, 1e-4f);
    m.setMute(2, false);
    m.setSolo(1, false);
    m.setSolo(2, false);
    EXPECT_NEAR(tailL(run(m, in, 4800)), 0.7f * kSqrtHalf, 1e-4f);
}

TEST(Mixer, FaderStepIsRampedNotZippered) {
    Mixer m(cfgN(1));
    const std::size_t frames = 48000;
    auto in = stereoSine(1000, 0.5, 48000, frames);
    std::vector<float> out(frames * 2);
    const float* p[1];
    for (std::size_t off = 0; off < frames; off += 480) {
        if (off == 24000) m.setFaderDb(0, -std::numeric_limits<float>::infinity());
        p[0] = in.data() + 2 * off;
        m.process(p, out.data() + 2 * off, 480);
    }
    const double base = 0.5 * kSqrtHalf * 2 * kPi * 1000 / 48000;  // max slope of the steady sine
    const double worst = maxAbsDiff(channel(out, 0));
    std::printf("[measure] fader to -inf: max sample step %.4f (steady sine %.4f; hard cut would be ~%.2f)\n",
                worst, base, 0.5 * kSqrtHalf);
    EXPECT_LT(worst, base * 1.25);
    // fully silent after ramp (10 ms) + limiter latency
    for (std::size_t i = 24000 + 480 + 100; i < frames; ++i) ASSERT_EQ(out[2 * i], 0.0f) << i;
}

TEST(Mixer, RampReachesTargetWithinRampTime) {
    Mixer m(cfgN(1));
    run(m, {dc(0.5f, 4800)}, 4800);
    m.setFaderDb(0, -20.0f);
    auto out = run(m, {dc(0.5f, 960)}, 960);  // 20 ms
    EXPECT_GT(out[2 * 100], out[2 * 400]);     // still moving
    EXPECT_NEAR(tailL(out), 0.5f * 0.1f * kSqrtHalf, 1e-4f);
}

TEST(Mixer, LimiterNeverExceedsCeiling) {
    MixerConfig c = cfgN(4);
    Mixer m(c);  // ceiling -1 dBFS
    const float ceil = dbToLinear(-1.0f);
    std::vector<std::vector<float>> in;
    for (int i = 0; i < 4; ++i) in.push_back(stereoSine(440.0 + 330 * i, 0.9, 48000, 48000));
    for (int i = 0; i < 4; ++i) m.setFaderDb(i, 6.0f);
    auto out = run(m, in, 48000);
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    std::printf("[measure] 4 hot channels summed (~+8 dBFS): limited peak = %.4f (ceiling %.4f)\n", peak, ceil);
    EXPECT_LE(peak, ceil + 1e-4f);
    EXPECT_GT(peak, ceil * 0.97f);  // limiter works at the ceiling, not far below it
    EXPECT_LT(m.programLevels().limiterReductionDb, -3.0f);
    EXPECT_LE(m.programLevels().peakDb[0], -1.0f + 0.01f);
    EXPECT_LE(m.programLevels().truePeakDb[0], 1.0f);  // inter-sample overs are small
}

TEST(Mixer, LimiterModesOffAndSoftClip) {
    Mixer m(cfgN(2));
    m.setLimiterMode(LimiterMode::Off);
    std::vector<std::vector<float>> in = {dc(0.9f, 4800), dc(0.9f, 4800)};
    auto out = run(m, in, 4800);
    EXPECT_NEAR(tailL(out), 1.8f * kSqrtHalf, 1e-4f);  // > 1.0, untouched
    m.setLimiterMode(LimiterMode::SoftClip);
    out = run(m, in, 4800);
    EXPECT_LT(tailL(out), dbToLinear(-1.0f));
    EXPECT_GT(tailL(out), 0.85f);
}

TEST(Limiter, NoJumpsOnSuddenLoudBurst) {
    Limiter lim;
    std::vector<float> x(4800 * 2, 0.0f);
    auto burst = stereoSine(1000, 3.0, 48000, 1200);
    std::copy(burst.begin(), burst.end(), x.begin() + 2 * 2000);
    for (std::size_t i = 0; i < 2000; ++i) x[2 * i] = x[2 * i + 1] = 0.1f * std::sin(0.13f * static_cast<float>(i));
    lim.process(x.data(), 4800);
    const double base = 1.0 * 2 * kPi * 1000 / 48000;
    double worst = 0;
    for (std::size_t i = 2001; i < 4800; ++i) worst = std::max(worst, std::fabs(double(x[2 * i]) - x[2 * i - 2]));
    for (std::size_t i = 0; i < 4800; ++i) ASSERT_LE(std::fabs(x[2 * i]), dbToLinear(-1.0f) + 1e-5f);
    std::printf("[measure] limiter burst: worst sample step %.4f (unity-sine slope bound %.4f)\n", worst, base);
    EXPECT_LT(worst, 0.2);
}

TEST(Limiter, SoftClipIsMonotonicBoundedAndTransparentBelowKnee) {
    LimiterConfig cfg;
    cfg.mode = LimiterMode::SoftClip;
    Limiter lim(48000, cfg);
    const std::size_t n = 4001;
    std::vector<float> x(n * 2);
    for (std::size_t i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = -4.0f + 8.0f * static_cast<float>(i) / (n - 1);
    std::vector<float> ref = x;
    lim.process(x.data(), n);
    const std::size_t d = lim.latencyFrames();
    const float ceil = dbToLinear(-1.0f), knee = ceil * dbToLinear(-6.0f);
    float prev = -10;
    for (std::size_t i = d; i < n; ++i) {
        const float y = x[2 * i], in = ref[2 * (i - d)];
        EXPECT_GE(y, prev);
        prev = y;
        EXPECT_LT(std::fabs(y), ceil + 1e-6f);
        if (std::fabs(in) < knee) EXPECT_NEAR(y, in, 1e-6f);
    }
}

TEST(Mixer, SetterThreadSafetyUnderProcessing) {
    Mixer m(MixerConfig::broadcastDefault(4));
    ToneSource tone(1000.0, -12.0f);
    PinkNoiseSource pink(-18.0f);
    m.attachSource(0, &tone);
    m.attachSource(5, &pink);
    std::atomic<bool> stop{false};
    std::thread ui([&] {
        unsigned i = 0;
        while (!stop.load()) {
            ++i;
            const std::size_t ch = i % m.channelCount();
            m.setFaderPosition(ch, (i % 101) / 100.0f);
            m.setPan(ch, ((i % 21) - 10) / 10.0f);
            m.setMute(ch, i % 7 == 0);
            m.setSolo((i + 3) % m.channelCount(), i % 11 == 0);
            m.setGainDb(ch, static_cast<float>(i % 13) - 6.0f);
            m.setLimiterMode(static_cast<LimiterMode>(i % 3));
            m.setMasterFaderDb(-static_cast<float>(i % 5));
            if (i % 50 == 0) m.resetMeters();
            (void)m.programLevels();
            (void)m.channelLevels(ch);
        }
    });
    std::vector<float> out(480 * 2);
    bool finite = true;
    for (int b = 0; b < 3000; ++b) {
        m.process(out.data(), 480);
        for (float v : out) if (!std::isfinite(v) || std::fabs(v) > 4.0f) finite = false;
    }
    stop = true;
    ui.join();
    EXPECT_TRUE(finite);
}

TEST(Mixer, PullsAttachedSourcesAndMeters) {
    Mixer m(MixerConfig::broadcastDefault(1));
    ToneSource tone(1000.0, -20.0f);
    m.attachSource(0, &tone);
    std::vector<float> out(480 * 2);
    for (int i = 0; i < 200; ++i) m.process(out.data(), 480);
    const ChannelLevels cl = m.channelLevels(0);
    EXPECT_NEAR(cl.peakDb[0], -20.0f, 0.1f);
    EXPECT_TRUE(std::isinf(m.channelLevels(1).peakDb[0]));
    const ProgramLevels pl = m.programLevels();
    EXPECT_NEAR(pl.peakDb[0], -23.0f, 0.2f);  // -20 dBFS tone, centre pan -3 dB
    EXPECT_NEAR(pl.rmsDb[0], -26.0f, 0.2f);
    EXPECT_NEAR(pl.momentaryLufs, -0.691f + 10 * std::log10(2.0 * std::pow(10.0, -2.6) * 1.1725), 0.15f);
    std::printf("[measure] mixer program: peak %.2f rms %.2f TP %.2f M %.2f S %.2f\n", pl.peakDb[0],
                pl.rmsDb[0], pl.truePeakDb[0], pl.momentaryLufs, pl.shortTermLufs);
    m.resetMeters();
    m.process(out.data(), 480);
}
