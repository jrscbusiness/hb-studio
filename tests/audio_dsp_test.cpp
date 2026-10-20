#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "audio/delay_line.hpp"
#include "audio/drift_compensator.hpp"
#include "audio/level_publisher.hpp"
#include "audio/resampler.hpp"
#include "audio/ring_buffer.hpp"
#include "audio/sources.hpp"
#include "audio_test_util.hpp"

using namespace hb::audio;
using namespace hb::audio::test;

// ------------------------------------------------------------- RingBuffer
TEST(RingBuffer, WrapAroundAndOrder) {
    RingBuffer rb(8);
    EXPECT_EQ(rb.capacity(), 8u);
    float in[6] = {1, 2, 3, 4, 5, 6}, out[6] = {};
    EXPECT_EQ(rb.write(in, 6), 6u);
    EXPECT_EQ(rb.read(out, 4), 4u);
    EXPECT_EQ(rb.write(in, 6), 6u);  // wraps
    EXPECT_EQ(rb.size(), 8u);
    float all[8] = {};
    EXPECT_EQ(rb.read(all, 8), 8u);
    const float expect[8] = {5, 6, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(all[i], expect[i]);
    EXPECT_EQ(rb.underruns(), 0u);
    EXPECT_EQ(rb.overruns(), 0u);
}

TEST(RingBuffer, OverrunAndUnderrunCounters) {
    RingBuffer rb(4);
    float in[6] = {1, 2, 3, 4, 5, 6}, out[8];
    EXPECT_EQ(rb.write(in, 6), 4u);
    EXPECT_EQ(rb.overruns(), 1u);
    EXPECT_EQ(rb.droppedSamples(), 2u);
    EXPECT_EQ(rb.read(out, 6), 4u);
    EXPECT_EQ(rb.underruns(), 1u);
    EXPECT_EQ(rb.missingSamples(), 2u);
    EXPECT_EQ(rb.readZeroFill(out, 3), 0u);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(rb.underruns(), 2u);
    rb.resetCounters();
    EXPECT_EQ(rb.underruns(), 0u);
}

TEST(RingBuffer, ThreadedSpscKeepsOrderWithoutLoss) {
    RingBuffer rb(1024);
    constexpr std::size_t kTotal = 2'000'000;
    std::thread prod([&] {
        std::vector<float> chunk(100);
        std::size_t next = 0;
        while (next < kTotal) {
            const std::size_t n = std::min<std::size_t>(chunk.size(), kTotal - next);
            for (std::size_t i = 0; i < n; ++i) chunk[i] = static_cast<float>(next + i);
            std::size_t sent = 0;
            while (sent < n) {
                // retry the remainder so nothing is dropped (counts as overrun, tolerated)
                sent += rb.write(chunk.data() + sent, n - sent);
                if (sent < n) std::this_thread::yield();
            }
            next += n;
        }
    });
    std::size_t expected = 0;
    bool ok = true;
    std::vector<float> buf(77);
    while (expected < kTotal) {
        const std::size_t avail = rb.size();
        if (avail == 0) { std::this_thread::yield(); continue; }
        const std::size_t n = rb.read(buf.data(), std::min(avail, buf.size()));
        for (std::size_t i = 0; i < n; ++i, ++expected)
            if (buf[i] != static_cast<float>(expected)) ok = false;
    }
    prod.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(rb.underruns(), 0u);
}

// -------------------------------------------------------------- Resampler
namespace {
std::vector<float> resampleSine(double inRate, double hz, ResamplerQuality q, double seconds = 1.0,
                                double outRate = 48000.0) {
    Resampler rs(inRate, outRate, 2, q);
    const std::size_t frames = static_cast<std::size_t>(inRate * seconds);
    auto x = stereoSine(hz, 0.5, inRate, frames);
    std::vector<float> out;
    std::vector<float> tmp(rs.maxOutputFrames(480) * 2);
    for (std::size_t off = 0; off < frames; off += 480) {  // odd chunking on purpose
        const std::size_t n = std::min<std::size_t>(480, frames - off);
        const std::size_t got = rs.process(x.data() + 2 * off, n, tmp.data(), tmp.size() / 2);
        out.insert(out.end(), tmp.begin(), tmp.begin() + 2 * got);
    }
    return out;
}
}  // namespace

TEST(Resampler, OutputLengthFollowsRatio) {
    for (double r : {44100.0, 32000.0, 96000.0}) {
        auto y = resampleSine(r, 1000, ResamplerQuality::Medium, 2.0);
        // the kernel look-ahead (taps/2 input frames) is not yet emitted at the end
        EXPECT_NEAR(static_cast<double>(y.size() / 2), 2.0 * 48000, 40.0) << r;
        EXPECT_LE(y.size() / 2, 2u * 48000u + 2u) << r;
    }
}

TEST(Resampler, SnrOfSineAcrossRatesAndQualities) {
    struct Row { double rate; ResamplerQuality q; double minSnr; const char* name; };
    const Row rows[] = {
        {44100, ResamplerQuality::High, 100, "44.1k High"},
        {44100, ResamplerQuality::Medium, 85, "44.1k Medium"},
        {44100, ResamplerQuality::Low, 60, "44.1k Low"},
        {32000, ResamplerQuality::High, 100, "32k High"},
        {96000, ResamplerQuality::High, 100, "96k High"},
        {96000, ResamplerQuality::Medium, 85, "96k Medium"},
    };
    for (const Row& r : rows) {
        auto y = resampleSine(r.rate, 1000, r.q);
        auto l = channel(y, 0, 2000, 2000);  // skip start-up / tail transients
        const double snr = sineFitSnrDb(l, 1000, 48000);
        std::printf("[measure] resampler %-13s SNR(1 kHz) = %.1f dB\n", r.name, snr);
        EXPECT_GT(snr, r.minSnr) << r.name;
    }
    auto lin = channel(resampleSine(44100, 1000, ResamplerQuality::Linear), 0, 2000, 2000);
    const double linSnr = sineFitSnrDb(lin, 1000, 48000);
    std::printf("[measure] resampler 44.1k Linear  SNR(1 kHz) = %.1f dB\n", linSnr);
    auto sinc = channel(resampleSine(44100, 1000, ResamplerQuality::High), 0, 2000, 2000);
    EXPECT_GT(sineFitSnrDb(sinc, 1000, 48000), linSnr + 30.0);
}

TEST(Resampler, SnrAtHighFrequency) {
    auto y = resampleSine(44100, 15000, ResamplerQuality::High);
    const double snr = sineFitSnrDb(channel(y, 0, 2000, 2000), 15000, 48000);
    std::printf("[measure] resampler 44.1k High SNR(15 kHz) = %.1f dB\n", snr);
    EXPECT_GT(snr, 90);
}

TEST(Resampler, AliasingIsRejectedWhenDownsampling) {
    // 30 kHz at 96 kHz is above the 24 kHz output Nyquist: it would alias to 18 kHz.
    auto y = resampleSine(96000, 30000, ResamplerQuality::High);
    auto l = channel(y, 0, 2000, 2000);
    const double inDb = 20 * std::log10(0.5 / std::sqrt(2.0));
    const double outDb = rmsDb(l);
    std::printf("[measure] 96k->48k, 30 kHz tone rejected by %.1f dB\n", inDb - outDb);
    EXPECT_LT(outDb, inDb - 80.0);
    auto m = channel(resampleSine(96000, 30000, ResamplerQuality::Medium), 0, 2000, 2000);
    EXPECT_LT(rmsDb(m), inDb - 60.0);
}

TEST(Resampler, UnityDcGainAndStereoIndependence) {
    Resampler rs(44100, 48000, 2, ResamplerQuality::Medium);
    std::vector<float> in(4410 * 2);
    for (std::size_t i = 0; i < 4410; ++i) { in[2 * i] = 0.25f; in[2 * i + 1] = -0.5f; }
    std::vector<float> out(rs.maxOutputFrames(4410) * 2);
    const std::size_t n = rs.process(in.data(), 4410, out.data(), out.size() / 2);
    ASSERT_GT(n, 1000u);
    EXPECT_NEAR(out[2 * (n - 1)], 0.25f, 1e-5f);
    EXPECT_NEAR(out[2 * (n - 1) + 1], -0.5f, 1e-5f);
}

TEST(Resampler, PullModeMatchesPushMode) {
    auto x = stereoSine(1000, 0.5, 44100, 44100);
    Resampler a(44100, 48000, 2, ResamplerQuality::Medium), b(44100, 48000, 2, ResamplerQuality::Medium);
    std::vector<float> ya(a.maxOutputFrames(44100) * 2);
    const std::size_t na = a.process(x.data(), 44100, ya.data(), ya.size() / 2);
    std::vector<float> yb(20000 * 2);
    std::size_t consumed = 0;
    b.pull([&](float* dst, std::size_t n) {
        for (std::size_t i = 0; i < n * 2; ++i) dst[i] = consumed + i < x.size() ? x[consumed + i] : 0.0f;
        consumed += n * 2;
    }, yb.data(), 20000);
    ASSERT_GT(na, 20000u);
    for (std::size_t i = 0; i < 20000 * 2; ++i) ASSERT_NEAR(ya[i], yb[i], 1e-6f) << i;
}

TEST(Resampler, RatioTrimChangesConsumption) {
    Resampler a(48000, 48000, 2, ResamplerQuality::Linear), b(48000, 48000, 2, ResamplerQuality::Linear);
    b.setRatioTrimPpm(1000.0);
    std::vector<float> in(48000 * 2, 0.1f), oa(a.maxOutputFrames(48000) * 2), ob(oa.size());
    const std::size_t na = a.process(in.data(), 48000, oa.data(), oa.size() / 2);
    const std::size_t nb = b.process(in.data(), 48000, ob.data(), ob.size() / 2);
    EXPECT_NEAR(static_cast<double>(na - nb), 48.0, 3.0);  // 1000 ppm of one second
}

// ------------------------------------------------------- DriftCompensator
TEST(DriftCompensator, ConvergesOnClockMismatchWithoutUnderruns) {
    constexpr int kRate = 48000;
    constexpr std::size_t kTarget = 2400;  // 50 ms
    const double mismatchPpm = 150.0;      // producer clock faster than consumer
    RingBuffer ring(16384 * 2);
    DriftCompensator dc(kRate, 2, kTarget, ResamplerQuality::Linear);
    std::vector<float> chunk(kTarget * 2 + 2000, 0.1f), out(480 * 2);  // >= prefill size (was an out-of-bounds read)
    // prefill to target
    ring.write(chunk.data(), kTarget * 2);
    double produced = 0, minFill = 1e9, maxFill = 0;
    const int ticks = 20000;  // 10 ms each -> 200 s
    for (int t = 0; t < ticks; ++t) {
        produced += 480.0 * (1.0 + mismatchPpm * 1e-6);
        const std::size_t whole = static_cast<std::size_t>(produced);
        produced -= static_cast<double>(whole);
        ring.write(chunk.data(), whole * 2);
        dc.pull(ring, out.data(), 480);
        if (t > 15000) {
            const double fill = static_cast<double>(ring.size() / 2);
            minFill = std::min(minFill, fill);
            maxFill = std::max(maxFill, fill);
        }
    }
    std::printf("[measure] drift: ppm=%.1f (true %.0f), steady fill %.0f..%.0f (target %zu), under=%llu over=%llu\n",
                dc.currentPpm(), mismatchPpm, minFill, maxFill, kTarget,
                static_cast<unsigned long long>(ring.underruns()),
                static_cast<unsigned long long>(ring.overruns()));
    EXPECT_NEAR(dc.currentPpm(), mismatchPpm, 15.0);
    // Fill is sampled after each pull; the controller regulates the level seen at
    // the start of a pull, i.e. one 480-frame block higher.
    EXPECT_NEAR(minFill, static_cast<double>(kTarget) - 480.0, 120.0);
    EXPECT_NEAR(maxFill, static_cast<double>(kTarget) - 480.0, 120.0);
    EXPECT_EQ(ring.underruns(), 0u);
    EXPECT_EQ(ring.overruns(), 0u);
}

TEST(DriftCompensator, NegativeMismatchAndPriming) {
    RingBuffer ring(16384 * 2);
    DriftCompensator dc(48000, 2, 1200, ResamplerQuality::Linear);
    std::vector<float> chunk(8000 * 2, 0.1f), out(480 * 2, 1.0f);
    dc.pull(ring, out.data(), 480);  // empty ring: not primed -> silence, no underrun counted
    EXPECT_FALSE(dc.primed());
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(ring.underruns(), 0u);
    ring.write(chunk.data(), 1200 * 2);
    double produced = 0;
    for (int t = 0; t < 20000; ++t) {
        produced += 480.0 * (1.0 - 100e-6);
        const std::size_t whole = static_cast<std::size_t>(produced);
        produced -= static_cast<double>(whole);
        ring.write(chunk.data(), whole * 2);
        dc.pull(ring, out.data(), 480);
    }
    EXPECT_TRUE(dc.primed());
    EXPECT_NEAR(dc.currentPpm(), -100.0, 15.0);
    EXPECT_EQ(ring.underruns(), 0u);
}

// -------------------------------------------------------------- DelayLine
TEST(DelayLine, ImpulseDelayedExactly) {
    for (double ms : {0.0, 1.0, 40.0, 250.0, 2000.0}) {
        DelayLine d;
        d.setDelayMs(ms);
        d.reset();
        const std::size_t samples = static_cast<std::size_t>(std::llround(ms * 48.0));
        std::vector<float> x((samples + 200) * 2, 0.0f);
        x[2 * 10] = 1.0f;
        x[2 * 10 + 1] = 0.5f;
        d.process(x.data(), x.size() / 2);
        EXPECT_FLOAT_EQ(x[2 * (10 + samples)], 1.0f) << ms;
        EXPECT_FLOAT_EQ(x[2 * (10 + samples) + 1], 0.5f) << ms;
        double energy = 0;
        for (float v : x) energy += v * v;
        EXPECT_NEAR(energy, 1.25, 1e-6) << ms;
    }
}

TEST(DelayLine, ClampsToRange) {
    DelayLine d;
    d.setDelayMs(99999);
    EXPECT_DOUBLE_EQ(d.delayMs(), 2000.0);
    d.setDelayMs(-5);
    EXPECT_DOUBLE_EQ(d.delayMs(), 0.0);
}

TEST(DelayLine, ChangingDelayDoesNotClick) {
    const std::size_t frames = 48000;
    auto x = stereoSine(1000, 0.5, 48000, frames);
    DelayLine d;
    std::vector<float> y(x.size());
    std::size_t pos = 0;
    int step = 0;
    const double delays[] = {0, 37.3, 500, 12.7, 1999, 0};
    while (pos < frames) {
        const std::size_t n = 480;
        if (pos % 4800 == 0 && step < 6) d.setDelayMs(delays[step++]);
        std::copy(x.begin() + 2 * pos, x.begin() + 2 * (pos + n), y.begin() + 2 * pos);
        d.process(y.data() + 2 * pos, n);
        pos += n;
    }
    // Skip the first 2 s of silence that long delays expose? Output is
    // continuous anyway: compare against the sine slope bound.
    const double base = 0.5 * 2 * kPi * 1000 / 48000;  // max step of the pure sine
    const double worst = maxAbsDiff(channel(y, 0));
    std::printf("[measure] delay changes: max sample step %.4f (pure sine %.4f)\n", worst, base);
    EXPECT_LT(worst, base * 2.0);
    // A hard switch between two phases would step by up to ~1.0.
}

// ---------------------------------------------------------------- Sources
TEST(Sources, ToneLevelFrequencyAndPhaseContinuity) {
    ToneSource t(1000.0, -20.0f);
    std::vector<float> a(480 * 2), all;
    for (int i = 0; i < 100; ++i) { t.pull(a.data(), 480); all.insert(all.end(), a.begin(), a.end()); }
    auto l = channel(all, 0);
    EXPECT_NEAR(rmsDb(l), -20.0 - 3.0103, 0.02);
    EXPECT_GT(sineFitSnrDb(l, 1000, 48000), 100.0);  // block seams are seamless
    EXPECT_EQ(all[0], all[1]);
    t.setLevelDb(-6.0f);
    t.pull(a.data(), 480);
    t.pull(a.data(), 480);
    float mx = 0;
    for (float v : a) mx = std::max(mx, std::fabs(v));
    EXPECT_NEAR(mx, 0.5012f, 0.01f);
}

TEST(Sources, SilenceIsZero) {
    SilenceSource s;
    std::vector<float> a(100, 1.0f);
    EXPECT_EQ(s.pull(a.data(), 50), 50u);
    for (float v : a) EXPECT_EQ(v, 0.0f);
}

TEST(Sources, PinkNoiseSpectrumSlopeAndLevel) {
    PinkNoiseSource p(-20.0f, 7);
    std::vector<float> buf(65536 * 2 * 4);
    p.pull(buf.data(), 65536 * 4);
    auto l = channel(buf, 0);
    EXPECT_NEAR(rmsDb(l), -20.0, 0.5);
    // Average 4 periodograms; check octave band energies are equal (-3 dB/oct power density).
    const std::size_t N = 65536;
    std::vector<double> psd(N / 2, 0.0);
    for (int seg = 0; seg < 4; ++seg) {
        std::vector<std::complex<double>> a(N);
        for (std::size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / N);
            a[i] = l[seg * N + i] * w;
        }
        fft(a);
        for (std::size_t k = 0; k < N / 2; ++k) psd[k] += std::norm(a[k]);
    }
    auto band = [&](double f0, double f1) {
        double e = 0;
        for (std::size_t k = static_cast<std::size_t>(f0 * N / 48000); k < static_cast<std::size_t>(f1 * N / 48000); ++k) e += psd[k];
        return 10 * std::log10(e);
    };
    const double ref = band(250, 500);
    for (double f : {125.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0})
        EXPECT_NEAR(band(f, 2 * f), ref, 1.0) << f;
    // Reproducible.
    PinkNoiseSource q(-20.0f, 7);
    std::vector<float> b2(2000);
    q.pull(b2.data(), 1000);
    for (int i = 0; i < 2000; ++i) ASSERT_EQ(b2[i], buf[i]);
    // L and R are independent.
    double corr = 0;
    for (std::size_t i = 0; i < 20000; ++i) corr += buf[2 * i] * buf[2 * i + 1];
    EXPECT_LT(std::fabs(corr / 20000), 0.002);
}

// --------------------------------------------------------- LevelPublisher
TEST(LevelPublisher, PublishesRateLimitedWithStringFields) {
    Mixer mixer(MixerConfig::broadcastDefault(2));
    hb::core::EventBus bus;
    int count = 0;
    hb::core::Event last;
    bus.subscribe("audio.levels", [&](const hb::core::Event& e) { ++count; last = e; });
    LevelPublisher pub(mixer, bus, 10.0);
    EXPECT_TRUE(pub.tick(100.0));
    EXPECT_FALSE(pub.tick(100.05));
    EXPECT_FALSE(pub.tick(100.099));
    EXPECT_TRUE(pub.tick(100.11));
    EXPECT_EQ(count, 2);
    EXPECT_EQ(last.fields.at("channels"), "5");
    EXPECT_EQ(last.fields.at("ch.0.name"), "CAM 1");
    EXPECT_EQ(last.fields.at("ch.4.name"), "SYSTEM");
    EXPECT_EQ(last.fields.at("program.lufs.m"), "-inf");

    ToneSource tone(1000.0, -20.0f);
    mixer.attachSource(0, &tone);
    mixer.setSolo(1, false);
    std::vector<float> out(480 * 2);
    for (int i = 0; i < 100; ++i) mixer.process(out.data(), 480);
    EXPECT_TRUE(pub.tick(101.0));
    EXPECT_EQ(last.fields.at("ch.0.peak"), "-20.0");
    EXPECT_EQ(last.fields.at("ch.1.peak"), "-inf");
}
