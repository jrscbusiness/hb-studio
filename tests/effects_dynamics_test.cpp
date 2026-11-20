// Numerical tests of hb.compressor, hb.gate, hb.limiter, hb.deesser.
#include <gtest/gtest.h>

#include <iostream>

#include "audio/effects/builtin_effects.hpp"
#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

namespace {
void measure(const std::string& what, double value) { std::cout << "[MEASURE] " << what << " = " << value << "\n"; }

// Amplitude (dBFS, peak-equivalent) of the component at `hz`, via a single-bin DFT over frames [from, to).
double toneDb(const std::vector<float>& v, int ch, int sr, double hz, std::size_t from, std::size_t to) {
    double re = 0, im = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2.0 * kPi * hz * static_cast<double>(i) / sr;
        const double x = v[i * static_cast<std::size_t>(ch)];
        re += x * std::cos(w);
        im += x * std::sin(w);
    }
    return db(2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from));
}

// Compressor output level (dBFS peak-equivalent) for a 1 kHz sine of `inDb`.
double compressorOut(Effect& e, double inDb) {
    e.reset();
    auto v = sine(1000.0, std::pow(10.0, inDb / 20.0), 48000, 48000, 2);
    run(e, v, 2);
    return db(rms(v, 2, 24000, 48000) * std::sqrt(2.0));
}
}  // namespace

// ------------------------------------------------------------------ compressor
TEST(EffectsCompressor, StaticCurveHardKneeBothDetectors) {
    for (int detect = 0; detect < 2; ++detect)
        for (float ratio : {2.f, 4.f, 10.f, 20.f}) {
            auto e = make("hb.compressor");
            setAll(*e, {{"threshold_db", -20.f}, {"ratio", ratio}, {"knee_db", 0.f}, {"attack_ms", 1.f},
                        {"release_ms", 100.f}, {"detect", static_cast<float>(detect)}});
            for (double in : {-50.0, -30.0, -20.0, -12.0, -6.0, 0.0}) {
                const double expect = in <= -20.0 ? in : -20.0 + (in + 20.0) / ratio;
                const double out = compressorOut(*e, in);
                EXPECT_NEAR(out, expect, detect ? 0.5 : 0.35) << "detect=" << detect << " ratio=" << ratio << " in=" << in;
                if (detect == 0 && ratio == 4.f && (in == -6.0 || in == 0.0))
                    measure("compressor RMS thr=-20 ratio=4:1 in=" + std::to_string(static_cast<int>(in)) + " dB -> out (dB)", out);
            }
        }
}

TEST(EffectsCompressor, SoftKneeCurveAndRatioOneIsTransparent) {
    auto e = make("hb.compressor");
    setAll(*e, {{"threshold_db", -20.f}, {"ratio", 4.f}, {"knee_db", 6.f}, {"detect", 0.f}});
    // at the threshold the knee already reduces by (1-1/R) W / 4 = 0.5625 dB
    EXPECT_NEAR(compressorOut(*e, -20.0), -20.0 - 0.5625, 0.15);
    // below the knee: untouched; above: hard-knee line
    EXPECT_NEAR(compressorOut(*e, -30.0), -30.0, 0.1);
    EXPECT_NEAR(compressorOut(*e, -10.0), -17.5, 0.2);
    // gain reduction is monotonic with input level
    double prev = -1;
    for (double in = -40; in <= 0; in += 4) {
        compressorOut(*e, in);
        const double gr = e->gainReductionDb();
        EXPECT_GE(gr, prev - 0.05);
        prev = gr;
    }
    auto one = make("hb.compressor");
    one->setParam("ratio", 1.f);
    EXPECT_NEAR(compressorOut(*one, -3.0), -3.0, 0.05);
}

TEST(EffectsCompressor, MakeupGainAndGainReductionMeter) {
    auto e = make("hb.compressor");
    setAll(*e, {{"threshold_db", -20.f}, {"ratio", 4.f}, {"knee_db", 0.f}, {"makeup_db", 6.f}});
    const double out = compressorOut(*e, -10.0);
    EXPECT_NEAR(out, -17.5 + 6.0, 0.35);
    EXPECT_NEAR(e->gainReductionDb(), 7.5, 0.35);
    measure("compressor GR meter at -10 dB in, thr -20, 4:1 (dB; ideal 7.5)", e->gainReductionDb());
    // silence: meter falls back to 0
    std::vector<float> z(2 * 96000, 0.f);
    run(*e, z, 2);
    EXPECT_NEAR(e->gainReductionDb(), 0.0, 0.01);
}

namespace {
// Time (ms) for the GR meter to cross `frac` of its final value after a level step, 1-frame blocks.
struct Step { double tAttack63, tRelease37, finalGr; };
Step measureTimes(float atk, float rel) {
    auto e = make("hb.compressor");
    setAll(*e, {{"threshold_db", -20.f}, {"ratio", 20.f}, {"knee_db", 0.f}, {"attack_ms", atk},
                {"release_ms", rel}, {"detect", 1.f}});
    const int sr = 48000;
    float s[2] = {0.01f, 0.01f};  // -40 dB DC: below threshold
    for (int i = 0; i < sr / 10; ++i) { s[0] = s[1] = 0.01f; e->process(s, 1); }
    Step r{};
    const double target = 19.0;  // (0 - (-20)) * (1 - 1/20)
    int n = 0;
    for (; n < sr * 2; ++n) {
        s[0] = s[1] = 1.0f;
        e->process(s, 1);
        if (e->gainReductionDb() >= 0.632 * target) break;
    }
    r.tAttack63 = n * 1000.0 / sr;
    for (int i = 0; i < sr * 3; ++i) { s[0] = s[1] = 1.0f; e->process(s, 1); }
    r.finalGr = e->gainReductionDb();
    n = 0;
    for (; n < sr * 6; ++n) {
        s[0] = s[1] = 0.01f;
        e->process(s, 1);
        if (e->gainReductionDb() <= 0.368 * target) break;
    }
    r.tRelease37 = n * 1000.0 / sr;
    return r;
}
}  // namespace

TEST(EffectsCompressor, AttackAndReleaseTimeConstantsMeasured) {
    const float atks[] = {2.f, 10.f, 50.f};
    const float rels[] = {50.f, 120.f, 500.f};
    for (int i = 0; i < 3; ++i) {
        const Step s = measureTimes(atks[i], rels[i]);
        EXPECT_NEAR(s.finalGr, 19.0, 0.1);
        EXPECT_NEAR(s.tAttack63, atks[i], atks[i] * 0.06 + 0.05) << "attack " << atks[i];
        EXPECT_NEAR(s.tRelease37, rels[i], rels[i] * 0.06 + 0.05) << "release " << rels[i];
        if (i == 1) {
            measure("compressor attack 10 ms: time to 63.2% GR (ms)", s.tAttack63);
            measure("compressor release 120 ms: time to 36.8% GR (ms)", s.tRelease37);
        }
    }
}

TEST(EffectsCompressor, StereoLinkedKeepsImage) {
    auto e = make("hb.compressor");
    setAll(*e, {{"threshold_db", -30.f}, {"ratio", 8.f}, {"detect", 1.f}});
    auto v = sine(1000.0, 0.8, 48000, 48000, 2);
    for (std::size_t i = 0; i < 48000; ++i) v[2 * i + 1] *= 0.25f;  // R is 12 dB lower
    run(*e, v, 2);
    const double l = rms(v, 2, 24000, 48000, 0), r = rms(v, 2, 24000, 48000, 1);
    EXPECT_NEAR(db(l / r), 12.04, 0.1);   // same gain applied to both channels
}

// ------------------------------------------------------------------ gate
TEST(EffectsGate, OpensClosesHoldsAndReportsReduction) {
    const int sr = 48000;
    auto e = make("hb.gate");
    setAll(*e, {{"threshold_db", -40.f}, {"range_db", 60.f}, {"attack_ms", 1.f}, {"hold_ms", 100.f}, {"release_ms", 50.f}});
    auto seg = [&](double ampDb, double ms) {
        auto v = sine(1000.0, std::pow(10.0, ampDb / 20.0), sr, static_cast<std::size_t>(ms * sr / 1000), 2);
        run(*e, v, 2, 48);
        return v;
    };
    // closed: -60 dB signal is attenuated by ~60 dB
    auto v = seg(-60, 300);
    const double closed = db(rms(v, 2, v.size() / 2 - 4800, v.size() / 2) * std::sqrt(2.0)) - (-60.0);
    EXPECT_NEAR(closed, -60.0, 1.0);
    EXPECT_NEAR(e->gainReductionDb(), 60.0, 1.0);
    measure("gate closed: attenuation of -60 dB signal (dB; ideal -60)", closed);

    // opens fast: 5 ms after the -20 dB burst begins the gain is within 0.5 dB of unity
    v = seg(-20, 30);
    const std::size_t at = static_cast<std::size_t>(0.006 * sr);
    const double openG = db(rms(v, 2, at, at + 480) * std::sqrt(2.0)) - (-20.0);
    EXPECT_GT(openG, -0.5);
    EXPECT_NEAR(e->gainReductionDb(), 60.0, 61.0);  // finite and in range

    // signal drops below threshold: gate stays open during the hold time (100 ms)...
    v = seg(-60, 80);
    const double holdG = db(rms(v, 2, 2400, 3600) * std::sqrt(2.0)) - (-60.0);
    EXPECT_GT(holdG, -1.0);
    // ...then closes with the release time constant
    v = seg(-60, 700);
    const double lateG = db(rms(v, 2, v.size() / 2 - 4800, v.size() / 2) * std::sqrt(2.0)) - (-60.0);
    EXPECT_LT(lateG, -50.0);
    EXPECT_GT(e->gainReductionDb(), 50.0);
}

TEST(EffectsGate, RangeZeroIsTransparentAndRangeSetsFloor) {
    auto e = make("hb.gate");
    setAll(*e, {{"threshold_db", -20.f}, {"range_db", 0.f}});
    auto v = sine(1000.0, 0.001, 48000, 24000, 2);   // way below threshold
    run(*e, v, 2);
    EXPECT_NEAR(db(rms(v, 2, 12000, 24000) * std::sqrt(2.0)), -60.0, 0.1);
    EXPECT_NEAR(e->gainReductionDb(), 0.0, 0.01);

    auto f = make("hb.gate");
    setAll(*f, {{"threshold_db", -20.f}, {"range_db", 20.f}});
    auto w = sine(1000.0, 0.01, 48000, 48000, 2);
    run(*f, w, 2);
    EXPECT_NEAR(db(rms(w, 2, 24000, 48000) * std::sqrt(2.0)), -40.0 - 20.0, 0.2);
}

TEST(EffectsGate, ThresholdHysteresisNoChatterOnNoisyLevelAtThreshold) {
    auto e = make("hb.gate");
    setAll(*e, {{"threshold_db", -30.f}, {"range_db", 40.f}, {"hold_ms", 20.f}, {"release_ms", 20.f}});
    // amplitude just around threshold with AM ripple: count transitions of the GR meter
    const int sr = 48000;
    auto v = sine(1000.0, std::pow(10.0, -29.0 / 20.0), sr, sr, 2);
    for (std::size_t i = 0; i < v.size() / 2; ++i) {
        const float am = 1.f + 0.15f * std::sin(2.f * 3.14159265f * 7.f * static_cast<float>(i) / sr);
        v[2 * i] *= am; v[2 * i + 1] *= am;
    }
    int flips = 0;
    bool open = false, first = true;
    for (std::size_t pos = 0; pos < v.size() / 2; pos += 48) {
        e->process(v.data() + pos * 2, 48);
        const bool o = e->gainReductionDb() < 20.0;
        if (!first && o != open) ++flips;
        open = o; first = false;
    }
    EXPECT_LE(flips, 3);
}

// ------------------------------------------------------------------ limiter
namespace {
double maxAbs(const std::vector<float>& v) {
    double m = 0;
    for (float x : v) m = std::max(m, static_cast<double>(std::fabs(x)));
    return m;
}
}  // namespace

TEST(EffectsLimiter, NeverExceedsCeilingSineBurstImpulsesSquare) {
    for (int sr : {44100, 48000, 96000})
        for (int ch : {1, 2, 8})
            for (float ceilDb : {-1.f, -6.f, -0.1f}) {
                const double ceil = std::pow(10.0, ceilDb / 20.0);
                auto e = make("hb.limiter", sr, ch);
                e->setParam("ceiling_db", ceilDb);
                // +12 dB sine
                auto s = sine(997.0, std::pow(10.0, 12.0 / 20.0), sr, static_cast<std::size_t>(sr), ch);
                run(*e, s, ch, 300);
                EXPECT_LE(maxAbs(s), ceil + 1e-7) << "sine sr=" << sr << " ch=" << ch;
                // bursts of loud noise separated by silence
                std::vector<float> b;
                for (int k = 0; k < 20; ++k) {
                    auto n = noise(static_cast<std::size_t>(sr / 50), ch, 1.f + 9.f * static_cast<float>(k % 5), 100 + static_cast<unsigned>(k));
                    b.insert(b.end(), n.begin(), n.end());
                    b.insert(b.end(), static_cast<std::size_t>(sr / 100) * static_cast<std::size_t>(ch), 0.f);
                }
                run(*e, b, ch, 480);
                EXPECT_LE(maxAbs(b), ceil + 1e-7) << "burst";
                // isolated and alternating impulses, square wave
                std::vector<float> im(static_cast<std::size_t>(sr) * static_cast<std::size_t>(ch), 0.f);
                for (std::size_t i = 100; i + 1 < static_cast<std::size_t>(sr); i += 997)
                    for (int c = 0; c < ch; ++c) im[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = (i % 2 ? 8.f : -8.f);
                for (std::size_t i = static_cast<std::size_t>(sr) / 2; i < static_cast<std::size_t>(sr) / 2 + 2000; ++i)
                    im[i * static_cast<std::size_t>(ch)] = ((i / 24) % 2) ? 3.f : -3.f;
                run(*e, im, ch, 111);
                EXPECT_LE(maxAbs(im), ceil + 1e-7) << "impulses";
            }
}

TEST(EffectsLimiter, TransparentBelowCeilingWithExactLatency) {
    for (int sr : {44100, 48000, 96000}) {
        auto e = make("hb.limiter", sr, 2);
        const int L1 = hb::audio::fx::limiterLatencyFrames(sr);  // reported latency in frames
        auto in = sine(440.0, 0.3, sr, 4800, 2);   // -10.5 dB < ceiling -1 dB
        auto out = in;
        run(*e, out, 2, 123);
        // the reported latency is the look-ahead delay: out[n] == in[n - L1]
        for (std::size_t n = static_cast<std::size_t>(L1); n < 4800; ++n)
            ASSERT_NEAR(out[2 * n], in[2 * (n - static_cast<std::size_t>(L1))], 1e-6) << n << " sr=" << sr;
        EXPECT_EQ(e->gainReductionDb(), 0.f);
        if (sr == 48000) measure("limiter look-ahead latency @48k (frames)", L1);
    }
}

TEST(EffectsLimiter, ReducesGainSmoothlyAndRecovers) {
    const int sr = 48000;
    auto e = make("hb.limiter", sr, 2);
    e->setParam("ceiling_db", -3.f);
    e->setParam("release_ms", 50.f);
    auto loud = sine(500.0, 2.0, sr, sr / 2, 2);
    run(*e, loud, 2);
    EXPECT_GT(e->gainReductionDb(), 5.0);       // -3 dB ceiling vs 6 dB signal: ~9 dB
    EXPECT_NEAR(maxAbs(std::vector<float>(loud.begin() + 24000, loud.end())), std::pow(10.0, -3.0 / 20.0), 0.02);
    measure("limiter GR on +6 dB sine / -3 dBFS ceiling (dB; ideal 9.0)", e->gainReductionDb());
    // no hard steps: per-sample difference of the limited sine stays near a sine's own slope
    double maxStep = 0;
    for (std::size_t i = 1; i < loud.size() / 2; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(loud[2 * i] - loud[2 * (i - 1)])));
    EXPECT_LT(maxStep, 0.2);
    // then quiet signal: gain recovers to unity within ~5 release times
    auto quiet = sine(500.0, 0.1, sr, sr / 2, 2);
    run(*e, quiet, 2);
    EXPECT_NEAR(db(rms(quiet, 2, 20000, 24000) * std::sqrt(2.0)), -20.0, 0.1);
    EXPECT_NEAR(e->gainReductionDb(), 0.0, 0.1);
}

// ------------------------------------------------------------------ de-esser
TEST(EffectsDeesser, ReducesSibilanceBandAndPreservesLowEnd) {
    const int sr = 48000;
    auto e = make("hb.deesser", sr, 2);
    setAll(*e, {{"freq", 6500.f}, {"threshold_db", -30.f}, {"ratio", 4.f}});
    auto lo = sine(200.0, 0.1, sr, sr, 2);     // -20 dB "voice body"
    auto hi = sine(6500.0, std::pow(10.0, -10.0 / 20.0), sr, sr, 2);  // -10 dB sibilance
    std::vector<float> v(lo.size());
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = lo[i] + hi[i];
    run(*e, v, 2);
    const double loOut = toneDb(v, 2, sr, 200.0, sr / 2, sr);
    const double hiOut = toneDb(v, 2, sr, 6500.0, sr / 2, sr);
    EXPECT_NEAR(loOut, -20.0, 0.25);
    EXPECT_NEAR(hiOut, -10.0 - 15.0, 1.5);   // (-10 - (-30)) * (1 - 1/4) = 15 dB reduction
    measure("deesser 6.5k @-10 dB, thr -30, 4:1: output level (dB; ideal -25)", hiOut);
    measure("deesser 200 Hz @-20 dB: output level (dB; ideal -20)", loOut);
    EXPECT_NEAR(e->gainReductionDb(), 15.0, 1.5);
}

TEST(EffectsDeesser, BelowThresholdAndRatioOneAreTransparent) {
    const int sr = 48000;
    auto e = make("hb.deesser", sr, 2);
    e->setParam("threshold_db", -30.f);
    auto v = sine(6500.0, std::pow(10.0, -45.0 / 20.0), sr, sr, 2);
    auto ref = v;
    run(*e, v, 2);
    for (std::size_t i = 0; i < v.size(); i += 2) ASSERT_NEAR(v[i], ref[i], 1e-6);
    auto r1 = make("hb.deesser", sr, 2);
    setAll(*r1, {{"ratio", 1.f}, {"threshold_db", -60.f}});
    auto w = sine(6500.0, 0.5, sr, sr, 2);
    auto wr = w;
    run(*r1, w, 2);
    for (std::size_t i = 0; i < w.size(); i += 2) ASSERT_NEAR(w[i], wr[i], 1e-5);
}

TEST(EffectsDeesser, ThreeKHzCentreAlsoWorksAndOffBandIsLessAffected) {
    const int sr = 48000;
    auto e = make("hb.deesser", sr, 2);
    setAll(*e, {{"freq", 3000.f}, {"threshold_db", -40.f}, {"ratio", 10.f}});
    EXPECT_LT(gainDbAt(*e, 3000.0, sr, 0.3), -12.0);
    const double off = gainDbAt(*e, 12000.0, sr, 0.3);  // detector sees much less 12 kHz energy
    EXPECT_GT(off, -6.0);
}
