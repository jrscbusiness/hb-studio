#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "captions/audio_tap.hpp"

using namespace hb::captions;

TEST(CaptionsTap, FifoOrderAndWrap) {
    AudioTapQueue q(1000);
    std::vector<float> a(600 * 2), out(1000 * 2);
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(i);
    q.push(a.data(), 600);
    EXPECT_EQ(q.pop(out.data(), 400), 400u);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[799], 799.0f);
    q.push(a.data(), 600);   // wraps around the ring end
    EXPECT_EQ(q.sizeFrames(), 800u);
    EXPECT_EQ(q.pop(out.data(), 1000), 800u);
    EXPECT_EQ(out[0], 800.0f);                 // remaining of first block
    EXPECT_EQ(out[199 * 2], 1198.0f);
    EXPECT_EQ(out[200 * 2], 0.0f);             // second block starts
    EXPECT_EQ(q.droppedFrames(), 0u);
}

TEST(CaptionsTap, DropsOldestWhenFull) {
    AudioTapQueue q(1000);
    std::vector<float> a(800 * 2, 1.0f), b(800 * 2, 2.0f), out(1000 * 2);
    q.push(a.data(), 800);
    q.push(b.data(), 800);   // 600 overflow -> oldest 600 dropped
    EXPECT_EQ(q.droppedFrames(), 600u);
    EXPECT_EQ(q.sizeFrames(), 1000u);
    q.pop(out.data(), 1000);
    EXPECT_EQ(out[0], 1.0f);                    // 200 survivors of the old block
    EXPECT_EQ(out[199 * 2], 1.0f);
    EXPECT_EQ(out[200 * 2], 2.0f);
}

TEST(CaptionsTap, OversizedBlockKeepsNewest) {
    AudioTapQueue q(1000);
    std::vector<float> a(1500 * 2), out(1000 * 2);
    for (std::size_t i = 0; i < 1500; ++i) { a[i * 2] = static_cast<float>(i); a[i * 2 + 1] = 0; }
    q.push(a.data(), 1500);
    EXPECT_EQ(q.droppedFrames(), 500u);
    q.pop(out.data(), 1000);
    EXPECT_EQ(out[0], 500.0f);
}

TEST(CaptionsTap, WaitReturnsFalseWhenEmptyAndClosePreventsPush) {
    AudioTapQueue q(1000);
    EXPECT_FALSE(q.waitForData(1));
    q.close();
    float s[2] = {1, 1};
    q.push(s, 1);
    EXPECT_EQ(q.sizeFrames(), 0u);
    q.reopen();
    q.push(s, 1);
    EXPECT_TRUE(q.waitForData(1));
    q.clear();
    EXPECT_EQ(q.sizeFrames(), 0u);
    q.push(nullptr, 5);   // ignored
    q.push(s, 0);
}

TEST(CaptionsTap, DownmixerProduces16kMonoAtThirdOfTheFrames) {
    ProgramDownmixer d;
    std::vector<float> in(48000 * 2), out;
    for (std::size_t i = 0; i < 48000; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
        in[i * 2] = v;
        in[i * 2 + 1] = v;
    }
    for (std::size_t off = 0; off < 48000; off += 480) d.process(in.data() + off * 2, 480, out);
    EXPECT_NEAR(static_cast<double>(out.size()), 16000.0, 8.0);
    double peak = 0;
    for (std::size_t i = 4000; i < out.size(); ++i) peak = std::max(peak, static_cast<double>(std::fabs(out[i])));
    EXPECT_NEAR(peak, 0.5, 0.06);   // 440 Hz passes the low-pass with ~unity gain, L+R averaged
}

TEST(CaptionsTap, DownmixerAveragesChannels) {
    ProgramDownmixer d;
    std::vector<float> in(4800 * 2), out;
    for (std::size_t i = 0; i < 4800; ++i) { in[i * 2] = 0.8f; in[i * 2 + 1] = -0.8f; }   // out of phase: mono = 0
    d.process(in.data(), 4800, out);
    double peak = 0;
    for (float v : out) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    EXPECT_LT(peak, 1e-4);
}
