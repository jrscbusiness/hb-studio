// Frequency-response tests of hb.highpass, hb.lowpass and hb.eq3 (measured through process()).
#include <gtest/gtest.h>

#include <iostream>

#include "effects_test_util.hpp"

using namespace hb::audio;
using namespace hb::test;

namespace {
void measure(const std::string& what, double value) { std::cout << "[MEASURE] " << what << " = " << value << "\n"; }

}  // namespace

TEST(EffectsFilters, HighpassCutoffIsMinus3dBAndSlopesAreRight) {
    for (int sr : {44100, 48000, 96000})
        for (float fc : {40.f, 80.f, 250.f, 500.f})
            for (int slope = 0; slope < 2; ++slope) {
                auto e = make("hb.highpass", sr, 2);
                setAll(*e, {{"freq", fc}, {"slope", static_cast<float>(slope)}});
                const double g = gainDbAt(*e, fc, sr);
                EXPECT_NEAR(g, -3.0, 1.0) << "sr=" << sr << " fc=" << fc << " slope=" << slope;
                if (sr == 48000 && fc == 80.f) measure(std::string("highpass fc=80 slope=") + (slope ? "24" : "12") + " gain@fc (dB)", g);
                EXPECT_NEAR(gainDbAt(*e, fc * 20.0, sr), 0.0, 0.1);
                const double a = gainDbAt(*e, fc / 4, sr), b = gainDbAt(*e, fc / 8, sr);
                const double expect = slope ? 24.0 : 12.0;
                EXPECT_NEAR(a - b, expect, 1.0) << "slope per octave, fc=" << fc << " slope=" << slope;
                if (sr == 48000 && fc == 80.f) measure(std::string("highpass slope=") + (slope ? "24" : "12") + " measured dB/oct", a - b);
            }
}

TEST(EffectsFilters, LowpassCutoffIsMinus3dBAndSlopesAreRight) {
    for (int sr : {44100, 48000, 96000})
        for (float fc : {2000.f, 8000.f, 15000.f, 20000.f})
            for (int slope = 0; slope < 2; ++slope) {
                if (fc > 0.45 * sr) continue;
                auto e = make("hb.lowpass", sr, 2);
                setAll(*e, {{"freq", fc}, {"slope", static_cast<float>(slope)}});
                const double g = gainDbAt(*e, fc, sr);
                EXPECT_NEAR(g, -3.0, 1.0) << "sr=" << sr << " fc=" << fc << " slope=" << slope;
                if (sr == 48000 && fc == 8000.f) measure(std::string("lowpass fc=8000 slope=") + (slope ? "24" : "12") + " gain@fc (dB)", g);
                EXPECT_NEAR(gainDbAt(*e, fc / 20.0, sr), 0.0, 0.1);
            }
    // slope measured in the stop band (fc = 2 kHz; 4 and 8 kHz at 96 kHz: little bilinear warping)
    for (int slope = 0; slope < 2; ++slope) {
        auto e = make("hb.lowpass", 96000, 2);
        setAll(*e, {{"freq", 2000.f}, {"slope", static_cast<float>(slope)}});
        const double a = gainDbAt(*e, 4000, 96000), b = gainDbAt(*e, 8000, 96000);
        EXPECT_NEAR(a - b, slope ? 24.0 : 12.0, 1.0);
    }
}

TEST(EffectsFilters, MonoAndEightChannelsFilterAllChannelsIdentically) {
    for (int ch : {1, 8}) {
        auto e = make("hb.highpass", 48000, ch);
        e->setParam("freq", 200.f);
        EXPECT_NEAR(gainDbAt(*e, 200, 48000, 0.1, ch), -3.0, 1.0);
        EXPECT_NEAR(gainDbAt(*e, 5000, 48000, 0.1, ch), 0.0, 0.1);
    }
}

TEST(EffectsFilters, SweepingCutoffAndSlopeDoesNotClick) {
    const int sr = 48000;
    for (const char* id : {"hb.highpass", "hb.lowpass", "hb.eq3"}) {
        auto e = make(id, sr, 2);
        auto v = sine(1000.0, 0.5, sr, sr, 2);
        const std::size_t frames = v.size() / 2;
        // settle
        std::vector<float> warm = sine(1000.0, 0.5, sr, 4800, 2);
        e->process(warm.data(), 4800);
        double maxJump = 0;
        float prev = 0;
        for (std::size_t pos = 0; pos < frames; pos += 160) {
            const float t = static_cast<float>(pos) / static_cast<float>(frames);
            if (std::string(id) == "hb.eq3") {
                e->setParam("mid_gain", 12.f * (t < 0.5f ? 1.f : -1.f));
                e->setParam("low_gain", 15.f * (t < 0.3f ? 1.f : -1.f));
                e->setParam("high_gain", 15.f * (t < 0.7f ? -1.f : 1.f));
            } else {
                e->setParam("freq", std::string(id) == "hb.highpass" ? (t < 0.5f ? 100.f : 500.f) : (t < 0.5f ? 16000.f : 2000.f));
                e->setParam("slope", t < 0.5f ? 0.f : 1.f);
            }
            e->process(v.data() + pos * 2, std::min<std::size_t>(160, frames - pos));
            for (std::size_t i = pos; i < std::min(pos + 160, frames); ++i) {
                maxJump = std::max(maxJump, static_cast<double>(std::fabs(v[2 * i] - prev)));
                prev = v[2 * i];
            }
        }
        // A 1 kHz, 0.5 amplitude sine changes by at most 0.065 per sample; even with +-15 dB gain steps the
        // smoothed filters must stay far below a step discontinuity (> 0.5).
        EXPECT_LT(maxJump, 0.45) << id;
    }
}

TEST(EffectsEq3, FlatWhenAllGainsZero) {
    auto e = make("hb.eq3");
    auto v = noise(9600, 2, 0.5f, 11);
    auto ref = v;
    run(*e, v, 2);
    for (std::size_t i = 0; i < v.size(); ++i) ASSERT_NEAR(v[i], ref[i], 1e-5f);
}

TEST(EffectsEq3, ShelfGainsAtDcAndNyquist) {
    for (int sr : {44100, 48000, 96000}) {
        for (float g : {-15.f, -6.f, 6.f, 15.f}) {
            auto lo = make("hb.eq3", sr, 2);
            setAll(*lo, {{"low_freq", 100.f}, {"low_gain", g}});
            std::vector<float> v(static_cast<std::size_t>(sr) * 2, 0.1f);
            run(*lo, v, 2);
            const double dc = db(v.back() / 0.1);
            EXPECT_NEAR(dc, g, 0.15) << "sr=" << sr;
            if (sr == 48000 && g == 15.f) measure("eq3 low shelf +15 dB @100Hz: DC gain (dB)", dc);

            auto hi = make("hb.eq3", sr, 2);
            setAll(*hi, {{"high_freq", 8000.f}, {"high_gain", g}});
            std::vector<float> w(static_cast<std::size_t>(sr) * 2);
            for (std::size_t i = 0; i < static_cast<std::size_t>(sr); ++i) w[2 * i] = w[2 * i + 1] = (i % 2 ? -0.1f : 0.1f);
            run(*hi, w, 2);
            const double ny = db(std::fabs(w[w.size() - 2]) / 0.1);
            EXPECT_NEAR(ny, g, 0.15) << "sr=" << sr;
            if (sr == 48000 && g == 15.f) measure("eq3 high shelf +15 dB @8kHz: Nyquist gain (dB)", ny);
        }
    }
}

TEST(EffectsEq3, ShelfCornerIsHalfGainAndMidBandPeaksAtCenter) {
    auto e = make("hb.eq3");
    setAll(*e, {{"low_freq", 200.f}, {"low_gain", 12.f}});
    EXPECT_NEAR(gainDbAt(*e, 200, 48000), 6.0, 0.7);   // shelf midpoint (RBJ, S = 1)
    EXPECT_NEAR(gainDbAt(*e, 8000, 48000), 0.0, 0.3);  // other bands untouched

    for (float q : {0.7f, 2.f, 6.f}) {
        auto m = make("hb.eq3");
        setAll(*m, {{"mid_freq", 1000.f}, {"mid_gain", 10.f}, {"mid_q", q}});
        const double c = gainDbAt(*m, 1000, 48000);
        EXPECT_NEAR(c, 10.0, 0.15) << "q=" << q;
        if (q == 2.f) measure("eq3 mid +10 dB Q=2 @1kHz (dB)", c);
        EXPECT_NEAR(gainDbAt(*m, 30, 48000), 0.0, 1.5);
        EXPECT_NEAR(gainDbAt(*m, 20000, 48000), 0.0, 1.5);
        // higher Q -> narrower: one octave away gets less boost
        EXPECT_LT(gainDbAt(*m, 2000, 48000), c);
    }
    auto cut = make("hb.eq3");
    setAll(*cut, {{"mid_freq", 3000.f}, {"mid_gain", -15.f}, {"mid_q", 1.f}});
    EXPECT_NEAR(gainDbAt(*cut, 3000, 48000), -15.0, 0.2);
}

TEST(EffectsEq3, GainChangeIsSmoothedNoZipper) {
    auto e = make("hb.eq3");
    auto v = sine(500.0, 0.3, 48000, 48000, 2);
    e->process(v.data(), 4800);
    e->setParam("mid_gain", 15.f);
    e->setParam("mid_freq", 500.f);
    double maxJump = 0;
    float prev = v[2 * 4799];
    for (std::size_t pos = 4800; pos < 48000; pos += 480) {
        e->process(v.data() + pos * 2, 480);
        for (std::size_t i = pos; i < pos + 480; ++i) {
            maxJump = std::max(maxJump, static_cast<double>(std::fabs(v[2 * i] - prev)));
            prev = v[2 * i];
        }
    }
    // final amplitude ~0.3*5.6 = 1.7; per-sample change of such a sine at 500 Hz <= 0.11 + transient
    EXPECT_LT(maxJump, 0.25);
}
