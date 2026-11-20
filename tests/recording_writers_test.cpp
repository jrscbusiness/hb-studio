#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>

#include "recording/avi_writer.hpp"
#include "recording/flac_writer.hpp"
#include "recording/jpeg_encoder.hpp"
#include "recording/jpeg_seq_writer.hpp"
#include "recording/md5.hpp"
#include "recording/media_writer.hpp"
#include "recording/wav_writer.hpp"
#include "recording_test_util.hpp"

using namespace hb::rec;
using namespace hb::rectest;

namespace {

std::string hex(const std::array<std::uint8_t, 16>& d) {
    char b[3];
    std::string s;
    for (auto c : d) {
        std::snprintf(b, sizeof(b), "%02x", c);
        s += b;
    }
    return s;
}

std::string md5hex(const std::string& s) {
    hb::rec::detail::Md5 m;
    m.update(s.data(), s.size());
    return hex(m.finish());
}

RecordingProfile registered(const std::string& id) {
    registerPortableWriters();
    RecordingProfile p;
    WriterFactory f;
    EXPECT_TRUE(WriterRegistry::instance().find(id, p, f)) << id;
    return p;
}

void snap(std::vector<float>& v, int bits) {  // make every sample an exact multiple of 2^-(bits-1)
    const double sc = std::ldexp(1.0, bits - 1);
    for (auto& x : v) x = static_cast<float>(std::floor(static_cast<double>(x) * sc + 0.5) / sc);
}

int expectedInt(float x, int bits) {  // independent reference quantiser (inputs are exact multiples)
    const double s = std::ldexp(1.0, bits - 1);
    return static_cast<int>(std::floor(static_cast<double>(x) * s + 0.5));
}

}  // namespace

// ---------------------------------------------------------------- MD5 / registry
TEST(RecordingMd5, Rfc1321Vectors) {
    EXPECT_EQ(md5hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5hex("a"), "0cc175b9c0f1b6a831c399e269772661");
    EXPECT_EQ(md5hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(md5hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a");
    // incremental in odd pieces must equal one shot
    std::string big(1000, 'x');
    hb::rec::detail::Md5 m;
    for (std::size_t i = 0; i < big.size(); i += 7) m.update(big.data() + i, std::min<std::size_t>(7, big.size() - i));
    EXPECT_EQ(hex(m.finish()), md5hex(big));
}

TEST(RecordingRegistry, PortableProfilesRegisteredOnceWithPtBrTexts) {
    registerPortableWriters();
    registerPortableWriters();  // idempotent
    const auto profiles = WriterRegistry::instance().profiles();
    int found = 0;
    for (const char* id : {"wav", "wav-16", "wav-f32", "flac", "flac-16", "avi-mjpeg", "jpeg-seq"}) {
        int count = 0;
        for (const auto& p : profiles)
            if (p.id == id) {
                ++count;
                EXPECT_FALSE(p.label.empty());
                EXPECT_FALSE(p.description.empty());
                EXPECT_TRUE(p.kind == "audio" || p.kind == "video");
                EXPECT_EQ(p.kind == "video", p.hasVideo);
            }
        EXPECT_EQ(count, 1) << id;
        found += count;
    }
    EXPECT_EQ(found, 7);
}

// ---------------------------------------------------------------- WAV
namespace {
struct WavCase {
    int bits, channels, rate;
};
void roundTripWav(const WavCase& c, std::size_t frames, const char* profileId) {
    TempDir dir("wav");
    RecordingProfile p = registered(profileId);
    p.audioBitsPerSample = c.bits;
    WavWriter w;
    std::string err;
    const std::string path = dir.utf8("out.wav");
    ASSERT_TRUE(w.open(path, p, {}, AudioFormat{c.rate, c.channels}, err)) << err;
    // exact multiples of 2^-(bits-1) so that the reference quantiser is unambiguous
    std::vector<float> pcm(frames * static_cast<std::size_t>(c.channels));
    std::uint32_t seed = 7;
    const int range = c.bits == 16 ? 32768 : 8388608;
    for (auto& x : pcm) {
        seed = seed * 1664525u + 1013904223u;
        const int v = static_cast<int>(seed >> 8) % range;
        x = static_cast<float>(v) / static_cast<float>(range);
    }
    // two pieces, to exercise the streaming path
    const std::size_t half = frames / 2;
    ASSERT_TRUE(w.writeAudio(pcm.data(), half, 0, err)) << err;
    ASSERT_TRUE(w.writeAudio(pcm.data() + half * static_cast<std::size_t>(c.channels), frames - half, 0, err)) << err;
    ASSERT_TRUE(w.close(err)) << err;
    EXPECT_TRUE(w.close(err));  // idempotent
    EXPECT_EQ(w.stats().audioFrames, frames);
    const auto data = readFile(hb::rec::detail::pathFromUtf8(path));
    EXPECT_EQ(w.stats().bytesWritten, data.size());
    const WavInfo info = parseWav(data);
    ASSERT_TRUE(info.ok) << info.error;
    EXPECT_EQ(info.channels, c.channels);
    EXPECT_EQ(info.sampleRate, c.rate);
    EXPECT_EQ(info.bits, c.bits);
    EXPECT_EQ(info.frames, frames);
    const bool ext = c.bits == 24 || c.channels > 2;
    EXPECT_EQ(info.formatTag == 0xFFFE, ext);
    if (ext) {
        EXPECT_NE(info.channelMask, 0u);
    }
    EXPECT_EQ(info.subFormat, c.bits == 32 ? 3u : 1u);
    if (c.bits == 32) {
        EXPECT_TRUE(info.hasFact);
        ASSERT_EQ(info.floats.size(), pcm.size());
        EXPECT_EQ(info.floats, pcm);
    } else {
        ASSERT_EQ(info.ints.size(), pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) ASSERT_EQ(info.ints[i], expectedInt(pcm[i], c.bits)) << i;
    }
}
}  // namespace

TEST(RecordingWav, Pcm16Stereo) { roundTripWav({16, 2, 48000}, 5000, "wav-16"); }
TEST(RecordingWav, Pcm16Mono44100) { roundTripWav({16, 1, 44100}, 777, "wav-16"); }
TEST(RecordingWav, Pcm24StereoUsesExtensible) { roundTripWav({24, 2, 48000}, 5001, "wav"); }
TEST(RecordingWav, Pcm24MonoOddDataSizeIsPadded) { roundTripWav({24, 1, 48000}, 1001, "wav"); }
TEST(RecordingWav, Float32Stereo) { roundTripWav({32, 2, 96000}, 3000, "wav-f32"); }
TEST(RecordingWav, SixChannelsExtensible16) { roundTripWav({16, 6, 48000}, 1234, "wav-16"); }
TEST(RecordingWav, EightChannelsFloat) { roundTripWav({32, 8, 48000}, 500, "wav-f32"); }

TEST(RecordingWav, ClampsAndSanitisesNaN) {
    TempDir dir("wavclamp");
    RecordingProfile p = registered("wav-16");
    WavWriter w;
    std::string err;
    const std::string path = dir.utf8("c.wav");
    ASSERT_TRUE(w.open(path, p, {}, AudioFormat{48000, 1}, err));
    const float in[5] = {2.0f, -2.0f, std::nanf(""), 1.0f, -1.0f};
    ASSERT_TRUE(w.writeAudio(in, 5, 0, err));
    ASSERT_TRUE(w.close(err));
    const auto info = parseWav(readFile(hb::rec::detail::pathFromUtf8(path)));
    ASSERT_TRUE(info.ok) << info.error;
    EXPECT_EQ(info.ints, (std::vector<std::int32_t>{32767, -32768, 0, 32767, -32768}));
}

TEST(RecordingWav, ErrorsAreReturnedNotThrown) {
    TempDir dir("waverr");
    RecordingProfile p = registered("wav");
    WavWriter w;
    std::string err;
    EXPECT_FALSE(w.open(dir.utf8("no/such/dir/x.wav"), p, {}, AudioFormat{48000, 2}, err));
    EXPECT_FALSE(err.empty());
    WavWriter w2;
    err.clear();
    EXPECT_FALSE(w2.open(dir.utf8("x.wav"), p, {}, AudioFormat{}, err));  // no audio format
    EXPECT_FALSE(err.empty());
    float s[2] = {0, 0};
    err.clear();
    EXPECT_FALSE(w2.writeAudio(s, 1, 0, err));  // never opened
    EXPECT_TRUE(w2.close(err));
}

TEST(RecordingWav, Utf8PathWithAccentsAndSpaces) {
    TempDir dir("wavutf8");
    RecordingProfile p = registered("wav-16");
    WavWriter w;
    std::string err;
    const std::string path = dir.utf8("Gravação ção — 日本語.wav");
    ASSERT_TRUE(w.open(path, p, {}, AudioFormat{48000, 2}, err)) << err;
    const float s[4] = {0.25f, -0.25f, 0.5f, -0.5f};
    ASSERT_TRUE(w.writeAudio(s, 2, 0, err));
    ASSERT_TRUE(w.close(err));
    EXPECT_TRUE(std::filesystem::exists(hb::rec::detail::pathFromUtf8(path)));
    EXPECT_TRUE(parseWav(readFile(hb::rec::detail::pathFromUtf8(path))).ok);
}

TEST(RecordingWav, EmptyFileIsValid) {
    TempDir dir("wavempty");
    WavWriter w;
    std::string err;
    const std::string path = dir.utf8("e.wav");
    ASSERT_TRUE(w.open(path, registered("wav"), {}, AudioFormat{48000, 2}, err));
    ASSERT_TRUE(w.close(err));
    const auto info = parseWav(readFile(hb::rec::detail::pathFromUtf8(path)));
    ASSERT_TRUE(info.ok) << info.error;
    EXPECT_EQ(info.frames, 0u);
}

// ---------------------------------------------------------------- FLAC
namespace {
struct FlacResult {
    FlacInfo info;
    std::uint64_t wavBytes = 0, flacBytes = 0;
};

// Writes `pcm` (interleaved float) as FLAC and checks everything that can be checked from the bytes.
FlacResult flacRoundTrip(const std::vector<float>& pcm, int channels, int rate, int bits, std::size_t chunk) {
    FlacResult res;
    TempDir dir("flac");
    RecordingProfile p = registered(bits == 16 ? "flac-16" : "flac");
    FlacWriter w;
    std::string err;
    const std::string path = dir.utf8("t.flac");
    EXPECT_TRUE(w.open(path, p, {}, AudioFormat{rate, channels}, err)) << err;
    const std::size_t frames = pcm.size() / static_cast<std::size_t>(channels);
    for (std::size_t off = 0; off < frames; off += chunk) {
        const std::size_t n = std::min(chunk, frames - off);
        EXPECT_TRUE(w.writeAudio(pcm.data() + off * static_cast<std::size_t>(channels), n, 0, err)) << err;
    }
    EXPECT_TRUE(w.close(err)) << err;
    EXPECT_TRUE(w.close(err));
    EXPECT_EQ(w.stats().audioFrames, frames);
    const auto bytes = readFile(hb::rec::detail::pathFromUtf8(path));
    EXPECT_EQ(w.stats().bytesWritten, bytes.size());
    res.flacBytes = bytes.size();
    res.wavBytes = frames * static_cast<std::size_t>(channels) * static_cast<std::size_t>(bits / 8) + 44;
    res.info = parseFlac(bytes);
    const FlacInfo& f = res.info;
    EXPECT_TRUE(f.ok) << f.error;
    if (!f.ok) return res;
    EXPECT_EQ(f.channels, channels);
    EXPECT_EQ(f.sampleRate, rate);
    EXPECT_EQ(f.bps, bits);
    EXPECT_EQ(f.totalSamples, frames);
    EXPECT_EQ(f.samples.size(), pcm.size());
    // lossless: sample by sample against the reference quantiser
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < pcm.size() && i < f.samples.size(); ++i)
        if (f.samples[i] != expectedInt(pcm[i], bits)) ++mismatches;
    EXPECT_EQ(mismatches, 0u);
    // STREAMINFO MD5 = MD5 of the little-endian interleaved PCM
    std::string raw;
    for (auto s : f.samples) {
        const std::uint32_t u = static_cast<std::uint32_t>(s);
        raw.push_back(static_cast<char>(u & 0xFF));
        raw.push_back(static_cast<char>((u >> 8) & 0xFF));
        if (bits == 24) raw.push_back(static_cast<char>((u >> 16) & 0xFF));
    }
    hb::rec::detail::Md5 m;
    m.update(raw.data(), raw.size());
    const auto digest = m.finish();
    EXPECT_EQ(std::memcmp(digest.data(), f.md5, 16), 0) << "STREAMINFO MD5 mismatch";
    // block size bookkeeping
    EXPECT_EQ(f.frames, static_cast<int>((frames + 4095) / 4096));
    if (frames > 4096) {
        EXPECT_EQ(f.minBlock, 4096);
        EXPECT_EQ(f.maxBlock, 4096);
    } else if (frames > 0) {
        EXPECT_EQ(f.minBlock, static_cast<int>(frames));
        EXPECT_EQ(f.maxBlock, static_cast<int>(frames));
    }
    EXPECT_GT(f.maxFrame, 0);
    EXPECT_LE(f.minFrame, f.maxFrame);
    return res;
}

std::vector<float> quantisedAudio(std::size_t frames, int channels, int rate, int bits, float noise) {
    auto v = makeAudio(0, frames, rate, channels, noise);
    const double s = std::ldexp(1.0, bits - 1);
    for (auto& x : v) x = static_cast<float>(std::floor(static_cast<double>(x) * s + 0.5) / s);  // exact multiples
    return v;
}
}  // namespace

TEST(RecordingFlac, Stereo16LosslessAndMd5) {
    const auto pcm = quantisedAudio(48000 * 2 + 123, 2, 48000, 16, 0.002f);
    const auto r = flacRoundTrip(pcm, 2, 48000, 16, 1600);
    ASSERT_TRUE(r.info.ok);
    EXPECT_GT(r.info.fixedSubframes, 0);
    std::printf("[ measure ] FLAC 16-bit stereo sine+noise: %llu -> %llu bytes (%.1f%% of WAV)\n",
                static_cast<unsigned long long>(r.wavBytes), static_cast<unsigned long long>(r.flacBytes),
                100.0 * double(r.flacBytes) / double(r.wavBytes));
    EXPECT_LT(r.flacBytes, r.wavBytes * 7 / 10);
}

TEST(RecordingFlac, Stereo24LosslessAnd24BitNoise) {
    const auto pcm = quantisedAudio(48000 + 5, 2, 48000, 24, 0.0005f);
    const auto r = flacRoundTrip(pcm, 2, 48000, 24, 4096);
    ASSERT_TRUE(r.info.ok);
    std::printf("[ measure ] FLAC 24-bit stereo sine+noise: %llu -> %llu bytes (%.1f%% of WAV)\n",
                static_cast<unsigned long long>(r.wavBytes), static_cast<unsigned long long>(r.flacBytes),
                100.0 * double(r.flacBytes) / double(r.wavBytes));
}

TEST(RecordingFlac, MonoAndMultichannel) {
    flacRoundTrip(quantisedAudio(10000, 1, 44100, 16, 0.01f), 1, 44100, 16, 777);
    flacRoundTrip(quantisedAudio(9000, 6, 48000, 24, 0.01f), 6, 48000, 24, 5000);
    flacRoundTrip(quantisedAudio(9000, 8, 96000, 16, 0.01f), 8, 96000, 16, 4096);
}

TEST(RecordingFlac, DigitalSilenceIsTiny) {
    std::vector<float> pcm(48000 * 2, 0.0f);
    const auto r = flacRoundTrip(pcm, 2, 48000, 16, 4096);
    ASSERT_TRUE(r.info.ok);
    EXPECT_EQ(r.info.constantSubframes, r.info.frames * 2);
    EXPECT_LT(r.flacBytes, 600u);
}

TEST(RecordingFlac, FullScaleNoiseFallsBackToVerbatimAndStaysLossless) {
    std::vector<float> pcm(20000 * 2);
    std::uint32_t seed = 99;
    for (auto& x : pcm) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>(static_cast<int>(seed >> 16) - 32768) / 32768.0f;
    }
    const auto r = flacRoundTrip(pcm, 2, 48000, 16, 4096);
    ASSERT_TRUE(r.info.ok);
    EXPECT_GT(r.info.verbatimSubframes, 0);
    EXPECT_LT(r.flacBytes, r.wavBytes * 105 / 100);  // incompressible: tiny overhead at most
}

TEST(RecordingFlac, ExtremesAndClipping) {
    std::vector<float> pcm;
    for (int i = 0; i < 9000; ++i) {
        pcm.push_back(i % 2 ? 1.0f : -1.0f);   // +full scale clips to 32767
        pcm.push_back(i % 3 ? -1.0f : 1.0f);
    }
    TempDir dir("flacext");
    FlacWriter w;
    std::string err;
    const std::string path = dir.utf8("e.flac");
    ASSERT_TRUE(w.open(path, registered("flac-16"), {}, AudioFormat{48000, 2}, err));
    ASSERT_TRUE(w.writeAudio(pcm.data(), pcm.size() / 2, 0, err));
    ASSERT_TRUE(w.close(err));
    const auto f = parseFlac(readFile(hb::rec::detail::pathFromUtf8(path)));
    ASSERT_TRUE(f.ok) << f.error;
    for (std::size_t i = 0; i < pcm.size(); ++i) ASSERT_EQ(f.samples[i], pcm[i] > 0 ? 32767 : -32768) << i;
}

TEST(RecordingFlac, ShortAndEmptyStreams) {
    flacRoundTrip(quantisedAudio(1, 2, 48000, 16, 0.0f), 2, 48000, 16, 1);
    flacRoundTrip(quantisedAudio(100, 2, 48000, 16, 0.1f), 2, 48000, 16, 100);
    flacRoundTrip(quantisedAudio(4096, 2, 48000, 16, 0.1f), 2, 48000, 16, 4096);
    flacRoundTrip(quantisedAudio(4097, 2, 48000, 16, 0.1f), 2, 48000, 16, 4097);
    TempDir dir("flacempty");
    FlacWriter w;
    std::string err;
    const std::string path = dir.utf8("z.flac");
    ASSERT_TRUE(w.open(path, registered("flac"), {}, AudioFormat{48000, 2}, err));
    ASSERT_TRUE(w.close(err));
    const auto f = parseFlac(readFile(hb::rec::detail::pathFromUtf8(path)));
    ASSERT_TRUE(f.ok) << f.error;
    EXPECT_EQ(f.totalSamples, 0u);
}

TEST(RecordingFlac, UsesStereoDecorrelation) {
    // identical channels: side is all zero -> mid/side (or left/side) must be chosen and compress ~2x better
    auto mono = quantisedAudio(20000, 1, 48000, 16, 0.003f);
    std::vector<float> st;
    for (float x : mono) {
        st.push_back(x);
        st.push_back(x);
    }
    const auto r = flacRoundTrip(st, 2, 48000, 16, 4096);
    ASSERT_TRUE(r.info.ok);
    EXPECT_GT(r.info.stereoModes[1] + r.info.stereoModes[2] + r.info.stereoModes[3], 0);
}

TEST(RecordingFlac, RejectsUnsupportedFormats) {
    TempDir dir("flacbad");
    FlacWriter w;
    std::string err;
    EXPECT_FALSE(w.open(dir.utf8("a.flac"), registered("flac"), {}, AudioFormat{48000, 9}, err));
    EXPECT_FALSE(err.empty());
    FlacWriter w2;
    EXPECT_FALSE(w2.open(dir.utf8("no/dir/a.flac"), registered("flac"), {}, AudioFormat{48000, 2}, err));
}

// Optional cross-check with the real flac/ffmpeg tools when present (not required to pass in CI).
TEST(RecordingFlac, ExternalDecoderAgreesIfAvailable) {
    if (std::system("ffmpeg -version > /dev/null 2>&1") != 0) GTEST_SKIP() << "ffmpeg not installed";
    TempDir dir("flacext2");
    const auto pcm = quantisedAudio(48000, 2, 48000, 16, 0.002f);
    FlacWriter w;
    std::string err;
    const std::string path = dir.utf8("x.flac");
    ASSERT_TRUE(w.open(path, registered("flac-16"), {}, AudioFormat{48000, 2}, err));
    ASSERT_TRUE(w.writeAudio(pcm.data(), 48000, 0, err));
    ASSERT_TRUE(w.close(err));
    const std::string out = dir.utf8("x.s16");
    const std::string cmd = "ffmpeg -v error -y -i '" + path + "' -f s16le '" + out + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "ffmpeg rejected the FLAC (CRC/MD5 error?)";
    const auto raw = readFile(hb::rec::detail::pathFromUtf8(out));
    ASSERT_EQ(raw.size(), pcm.size() * 2);
    for (std::size_t i = 0; i < pcm.size(); ++i)
        ASSERT_EQ(static_cast<int>(static_cast<std::int16_t>(le16(&raw[i * 2]))), expectedInt(pcm[i], 16)) << i;
}

// ---------------------------------------------------------------- JPEG
TEST(RecordingJpeg, QualityTradesSizeForFidelity) {
    const auto frame = makeFrame(640, 360, 3);
    double prevPsnr = 0;
    std::size_t prevSize = 0;
    for (int q : {20, 50, 75, 90, 100}) {
        JpegOptions o;
        o.quality = q;
        std::vector<std::uint8_t> jpg;
        ASSERT_TRUE(encodeJpegBgra(frame->data.data(), 640, 360, frame->stride, o, jpg));
        const auto img = decodeJpeg(jpg);
        ASSERT_TRUE(img.ok) << img.error;
        EXPECT_EQ(img.width, 640);
        EXPECT_EQ(img.height, 360);
        EXPECT_EQ(img.compSampling[0][0], 2);  // 4:2:0
        const double psnr = psnrRgbVsBgra(img.rgb, *frame);
        std::printf("[ measure ] JPEG 4:2:0 q=%3d 640x360: %7zu bytes, PSNR %.2f dB\n", q, jpg.size(), psnr);
        EXPECT_GE(psnr, prevPsnr - 0.01) << "q=" << q;
        EXPECT_GE(jpg.size(), prevSize) << "q=" << q;
        prevPsnr = psnr;
        prevSize = jpg.size();
    }
}

TEST(RecordingJpeg, DocumentedMinimumPsnr) {
    // Documented guarantee (docs/RECORDING.md): >= 30 dB at q85 on a hard synthetic frame (hard colour
    // edges + 1-pixel-class stripes, 4:2:0), >= 38 dB for 4:4:4 at q95.
    const auto frame = makeFrame(1280, 720, 5);
    std::vector<std::uint8_t> jpg;
    JpegOptions o420;
    o420.quality = 85;
    ASSERT_TRUE(encodeJpegBgra(frame->data.data(), 1280, 720, frame->stride, o420, jpg));
    auto img = decodeJpeg(jpg);
    ASSERT_TRUE(img.ok) << img.error;
    const double p420 = psnrRgbVsBgra(img.rgb, *frame);
    const std::size_t s420 = jpg.size();
    JpegOptions o444;
    o444.quality = 95;
    o444.chroma420 = false;
    ASSERT_TRUE(encodeJpegBgra(frame->data.data(), 1280, 720, frame->stride, o444, jpg));
    img = decodeJpeg(jpg);
    ASSERT_TRUE(img.ok) << img.error;
    EXPECT_EQ(img.compSampling[0][0], 1);
    const double p444 = psnrRgbVsBgra(img.rgb, *frame);
    std::printf("[ measure ] JPEG 1280x720: 4:2:0 q85 %zu bytes %.2f dB | 4:4:4 q95 %zu bytes %.2f dB\n", s420, p420,
                jpg.size(), p444);
    EXPECT_GE(p420, 30.0);
    EXPECT_GE(p444, 38.0);
}

TEST(RecordingJpeg, OddSizesAndSmallImages) {
    for (auto [w, h] : {std::pair{1, 1}, {7, 5}, {17, 33}, {100, 3}, {2, 64}}) {
        const auto frame = makeFrame(std::max(w, 66), std::max(h, 8), 1);  // makeFrame needs >64 wide; crop below
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(&bgra[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4], &frame->data[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame->stride)], static_cast<std::size_t>(w) * 4);
        for (bool c420 : {true, false}) {
            JpegOptions o;
            o.quality = 95;
            o.chroma420 = c420;
            std::vector<std::uint8_t> jpg;
            ASSERT_TRUE(encodeJpegBgra(bgra.data(), w, h, w * 4, o, jpg)) << w << "x" << h;
            const auto img = decodeJpeg(jpg);
            ASSERT_TRUE(img.ok) << w << "x" << h << ": " << img.error;
            EXPECT_EQ(img.width, w);
            EXPECT_EQ(img.height, h);
        }
    }
}

TEST(RecordingJpeg, FlatImagesAndStuffingOfFFBytes) {
    // white / black / noise: exercises 0xFF byte stuffing in the entropy data
    std::vector<std::uint8_t> bgra(256 * 256 * 4);
    std::uint32_t seed = 5;
    for (std::size_t i = 0; i < bgra.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        bgra[i] = static_cast<std::uint8_t>(seed >> 24);
    }
    JpegOptions o;
    o.quality = 100;
    std::vector<std::uint8_t> jpg;
    ASSERT_TRUE(encodeJpegBgra(bgra.data(), 256, 256, 256 * 4, o, jpg));
    bool stuffed = false;
    for (std::size_t i = 0; i + 1 < jpg.size(); ++i)
        if (jpg[i] == 0xFF && jpg[i + 1] == 0x00) stuffed = true;
    EXPECT_TRUE(stuffed);
    const auto img = decodeJpeg(jpg);
    ASSERT_TRUE(img.ok) << img.error;
    std::fill(bgra.begin(), bgra.end(), 255);
    ASSERT_TRUE(encodeJpegBgra(bgra.data(), 256, 256, 256 * 4, o, jpg));
    const auto white = decodeJpeg(jpg);
    ASSERT_TRUE(white.ok);
    for (auto v : white.rgb) ASSERT_GE(v, 253);
}

TEST(RecordingJpeg, StructureIsStandard) {
    const auto frame = makeFrame(128, 80, 0);
    JpegEncoder enc(128, 80, JpegOptions{});
    ASSERT_TRUE(enc.valid());
    std::vector<std::uint8_t> a, b;
    ASSERT_TRUE(enc.encode(frame->data.data(), frame->stride, a));
    ASSERT_TRUE(enc.encode(frame->data.data(), frame->stride, b));  // encoder is reusable and deterministic
    EXPECT_EQ(a, b);
    EXPECT_EQ(a[0], 0xFF);
    EXPECT_EQ(a[1], 0xD8);
    EXPECT_EQ(a[a.size() - 2], 0xFF);
    EXPECT_EQ(a[a.size() - 1], 0xD9);
    EXPECT_EQ(std::memcmp(&a[6], "JFIF", 4), 0);
    // walk the segments: every marker is one of the expected baseline ones, 4 DHT tables with 12/162 symbols
    std::size_t pos = 2;
    int dht = 0, dqt = 0, sof = 0;
    while (pos + 4 <= a.size()) {
        ASSERT_EQ(a[pos], 0xFF);
        const int m = a[pos + 1];
        const std::size_t len = (std::size_t(a[pos + 2]) << 8) | a[pos + 3];
        if (m == 0xC4) {
            std::size_t p = pos + 4;
            while (p < pos + 2 + len) {
                int total = 0;
                for (int i = 0; i < 16; ++i) total += a[p + 1 + static_cast<std::size_t>(i)];
                EXPECT_TRUE(total == 12 || total == 162) << total;
                if (total == 162) {  // every (run,size) symbol of the AC table must be present exactly once
                    std::vector<int> seen(256, 0);
                    for (int i = 0; i < total; ++i) ++seen[a[p + 17 + static_cast<std::size_t>(i)]];
                    for (int run = 0; run < 16; ++run)
                        for (int size = 1; size <= 10; ++size) EXPECT_EQ(seen[static_cast<std::size_t>(run * 16 + size)], 1);
                    EXPECT_EQ(seen[0x00], 1);
                    EXPECT_EQ(seen[0xF0], 1);
                }
                p += 17 + static_cast<std::size_t>(total);
                ++dht;
            }
        } else if (m == 0xDB) {
            ++dqt;
        } else if (m == 0xC0) {
            ++sof;
        } else if (m == 0xDA) {
            break;
        }
        pos += 2 + len;
    }
    EXPECT_EQ(dht, 4);
    EXPECT_EQ(dqt, 1);
    EXPECT_EQ(sof, 1);
}

TEST(RecordingJpeg, RejectsBadInput) {
    JpegEncoder bad(0, 10, JpegOptions{});
    EXPECT_FALSE(bad.valid());
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(bad.encode(nullptr, 0, out));
    JpegEncoder ok(16, 16, JpegOptions{});
    std::vector<std::uint8_t> px(16 * 16 * 4);
    EXPECT_FALSE(ok.encode(px.data(), 10, out));  // stride too small
    EXPECT_FALSE(ok.encode(nullptr, 64, out));
}

TEST(RecordingJpeg, ExternalDecoderAgreesIfAvailable) {
    if (std::system("ffprobe -version > /dev/null 2>&1") != 0) GTEST_SKIP() << "ffprobe not installed";
    TempDir dir("jpgext");
    const auto frame = makeFrame(320, 180, 2);
    std::vector<std::uint8_t> jpg;
    ASSERT_TRUE(encodeJpegBgra(frame->data.data(), 320, 180, frame->stride, JpegOptions{}, jpg));
    const std::string path = dir.utf8("a.jpg");
    {
        std::ofstream f(hb::rec::detail::pathFromUtf8(path), std::ios::binary);
        f.write(reinterpret_cast<const char*>(jpg.data()), static_cast<std::streamsize>(jpg.size()));
    }
    const std::string cmd = "ffmpeg -v error -y -i '" + path + "' -f null - 2>&1";
    EXPECT_EQ(std::system(cmd.c_str()), 0);
}

// ---------------------------------------------------------------- AVI
TEST(RecordingAvi, StructureIndexAudioAndVideoReadBack) {
    TempDir dir("avi");
    constexpr int W = 320, H = 180, FRAMES = 20, RATE = 48000;
    RecordingProfile p = registered("avi-mjpeg");
    AviMjpegWriter w;
    std::string err;
    const std::string path = dir.utf8("clip.avi");
    ASSERT_TRUE(w.open(path, p, VideoFormat{W, H, 30, 1}, AudioFormat{RATE, 2}, err)) << err;
    std::vector<std::shared_ptr<hb::video::Frame>> frames;
    std::vector<float> allAudio;
    for (int i = 0; i < FRAMES; ++i) {
        frames.push_back(makeFrame(W, H, static_cast<std::uint64_t>(i)));
        // audio arrives in arbitrary pieces (not aligned to video frames)
        auto a = makeAudio(static_cast<std::size_t>(i) * 1600, 1600, RATE, 2, 0.001f);
        snap(a, 16);
        allAudio.insert(allAudio.end(), a.begin(), a.end());
        ASSERT_TRUE(w.writeAudio(a.data(), 1000, 0, err)) << err;
        ASSERT_TRUE(w.writeAudio(a.data() + 2000, 600, 0, err)) << err;
        ASSERT_TRUE(w.writeVideo(frames.back()->data.data(), frames.back()->stride, 0, err)) << err;
    }
    ASSERT_TRUE(w.close(err)) << err;
    EXPECT_TRUE(w.close(err));
    EXPECT_EQ(w.stats().videoFrames, static_cast<std::uint64_t>(FRAMES));
    EXPECT_EQ(w.stats().audioFrames, static_cast<std::uint64_t>(FRAMES) * 1600);

    const auto bytes = readFile(hb::rec::detail::pathFromUtf8(path));
    EXPECT_EQ(w.stats().bytesWritten, bytes.size());
    const AviInfo avi = parseAvi(bytes);
    ASSERT_TRUE(avi.ok) << avi.error;
    EXPECT_EQ(avi.width, static_cast<std::uint32_t>(W));
    EXPECT_EQ(avi.height, static_cast<std::uint32_t>(H));
    EXPECT_EQ(avi.usPerFrame, 33333u);
    EXPECT_EQ(avi.totalFrames, static_cast<std::uint32_t>(FRAMES));
    EXPECT_EQ(avi.streams, 2u);
    EXPECT_NE(avi.flags & 0x10u, 0u);  // AVIF_HASINDEX
    EXPECT_EQ(avi.vCompression, "MJPG");
    EXPECT_EQ(avi.vHandler, "MJPG");
    EXPECT_EQ(avi.vScale, 1u);
    EXPECT_EQ(avi.vRate, 30u);
    EXPECT_EQ(avi.vLength, static_cast<std::uint32_t>(FRAMES));
    EXPECT_EQ(avi.aChannels, 2);
    EXPECT_EQ(avi.aSampleRate, RATE);
    EXPECT_EQ(avi.aBits, 16);
    EXPECT_EQ(avi.aTag, 1);
    EXPECT_EQ(avi.aScale, 4u);
    EXPECT_EQ(avi.aRate, static_cast<std::uint32_t>(RATE) * 4);
    EXPECT_EQ(avi.aLength, static_cast<std::uint32_t>(FRAMES) * 1600);
    ASSERT_EQ(avi.videoFrames.size(), static_cast<std::size_t>(FRAMES));
    // audio is interleaved per frame period: video, audio, video, audio...
    ASSERT_EQ(avi.chunks.size(), static_cast<std::size_t>(FRAMES) * 2);
    for (std::size_t i = 0; i < avi.chunks.size(); ++i) EXPECT_EQ(avi.chunks[i].id, i % 2 ? "01wb" : "00dc");
    for (const auto& c : avi.index) EXPECT_NE(c.flags & 0x10u, 0u);
    // audio exact (16-bit quantisation of the generator output)
    ASSERT_EQ(avi.audio.size(), allAudio.size());
    for (std::size_t i = 0; i < allAudio.size(); ++i) ASSERT_EQ(avi.audio[i], expectedInt(allAudio[i], 16)) << i;
    // video: decode a few frames and compare
    for (int i : {0, 7, FRAMES - 1}) {
        const auto img = decodeJpeg(avi.videoFrames[static_cast<std::size_t>(i)]);
        ASSERT_TRUE(img.ok) << img.error;
        EXPECT_GE(psnrRgbVsBgra(img.rgb, *frames[static_cast<std::size_t>(i)]), 28.0) << i;
    }
}

TEST(RecordingAvi, FractionalFrameRateKeepsAudioLocked) {
    TempDir dir("avi2997");
    AviMjpegWriter w;
    std::string err;
    const std::string path = dir.utf8("ntsc.avi");
    ASSERT_TRUE(w.open(path, registered("avi-mjpeg"), VideoFormat{64, 64, 30000, 1001}, AudioFormat{48000, 2}, err)) << err;
    const auto frame = makeFrame(64 + 2, 64, 1);
    std::vector<std::uint8_t> crop(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) std::memcpy(&crop[static_cast<std::size_t>(y) * 256], &frame->data[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame->stride)], 256);
    const int N = 300;  // 10.01 s
    for (int i = 0; i < N; ++i) {
        const auto a = makeAudio(static_cast<std::size_t>(i) * 1602, 1602, 48000);
        ASSERT_TRUE(w.writeAudio(a.data(), 1602, 0, err));
        ASSERT_TRUE(w.writeVideo(crop.data(), 256, 0, err));
    }
    ASSERT_TRUE(w.close(err));
    const auto avi = parseAvi(readFile(hb::rec::detail::pathFromUtf8(path)));
    ASSERT_TRUE(avi.ok) << avi.error;
    EXPECT_EQ(avi.vScale, 1001u);
    EXPECT_EQ(avi.vRate, 30000u);
    EXPECT_EQ(avi.usPerFrame, 33367u);
    // audio chunk sizes follow round(n * 48000 * 1001 / 30000) so that audio never drifts from video
    std::uint64_t audioSoFar = 0;
    int vframes = 0;
    for (const auto& c : avi.chunks) {
        if (c.id == "00dc") {
            ++vframes;
        } else if (&c == &avi.chunks.back()) {  // close() flushes the audio that is left
            audioSoFar += c.size / 4;
            EXPECT_EQ(audioSoFar, static_cast<std::uint64_t>(N) * 1602);
        } else {
            audioSoFar += c.size / 4;
            const std::uint64_t due = (static_cast<std::uint64_t>(vframes) * 48000 * 1001 + 15000) / 30000;
            EXPECT_EQ(audioSoFar, std::min<std::uint64_t>(due, static_cast<std::uint64_t>(N) * 1602));
        }
    }
}

TEST(RecordingAvi, VideoOnlyAndMonoAndErrors) {
    TempDir dir("avi3");
    const auto frame = makeFrame(96, 72, 0);
    {
        RecordingProfile p = registered("avi-mjpeg");
        p.hasAudio = false;
        AviMjpegWriter w;
        std::string err;
        const std::string path = dir.utf8("v.avi");
        ASSERT_TRUE(w.open(path, p, VideoFormat{96, 72, 25, 1}, AudioFormat{}, err)) << err;
        ASSERT_TRUE(w.writeVideo(frame->data.data(), frame->stride, 0, err));
        ASSERT_TRUE(w.close(err));
        const auto avi = parseAvi(readFile(hb::rec::detail::pathFromUtf8(path)));
        ASSERT_TRUE(avi.ok) << avi.error;
        EXPECT_EQ(avi.streams, 1u);
        EXPECT_EQ(avi.videoFrames.size(), 1u);
    }
    {
        AviMjpegWriter w;
        std::string err;
        const std::string path = dir.utf8("m.avi");
        ASSERT_TRUE(w.open(path, registered("avi-mjpeg"), VideoFormat{96, 72, 25, 1}, AudioFormat{44100, 1}, err));
        const auto a = makeAudio(0, 1764, 44100, 1);
        ASSERT_TRUE(w.writeAudio(a.data(), 1764, 0, err));
        ASSERT_TRUE(w.writeVideo(frame->data.data(), frame->stride, 0, err));
        ASSERT_TRUE(w.close(err));
        const auto avi = parseAvi(readFile(hb::rec::detail::pathFromUtf8(path)));
        ASSERT_TRUE(avi.ok) << avi.error;
        EXPECT_EQ(avi.audioFrames, 1764u);
    }
    {
        AviMjpegWriter w;
        std::string err;
        EXPECT_FALSE(w.open(dir.utf8("x.avi"), registered("avi-mjpeg"), VideoFormat{}, AudioFormat{48000, 2}, err));
        EXPECT_FALSE(w.open(dir.utf8("nodir/x.avi"), registered("avi-mjpeg"), VideoFormat{96, 72, 25, 1}, AudioFormat{48000, 2}, err));
        EXPECT_FALSE(err.empty());
    }
}

// ---------------------------------------------------------------- JPEG sequence
TEST(RecordingJpegSeq, FolderWithNumberedFramesAndWav) {
    TempDir dir("seq");
    JpegSeqWriter w;
    std::string err;
    const std::string folder = dir.utf8("sequência 1");
    RecordingProfile p = registered("jpeg-seq");
    ASSERT_TRUE(w.open(folder, p, VideoFormat{128, 72, 30, 1}, AudioFormat{48000, 2}, err)) << err;
    std::vector<std::shared_ptr<hb::video::Frame>> fr;
    for (int i = 0; i < 5; ++i) {
        fr.push_back(makeFrame(128, 72, static_cast<std::uint64_t>(i)));
        ASSERT_TRUE(w.writeVideo(fr.back()->data.data(), fr.back()->stride, 0, err)) << err;
        const auto a = makeAudio(static_cast<std::size_t>(i) * 1600, 1600, 48000);
        ASSERT_TRUE(w.writeAudio(a.data(), 1600, 0, err));
    }
    ASSERT_TRUE(w.close(err));
    EXPECT_TRUE(w.close(err));
    EXPECT_EQ(w.stats().videoFrames, 5u);
    EXPECT_EQ(w.stats().audioFrames, 8000u);
    const auto dirPath = hb::rec::detail::pathFromUtf8(folder);
    for (int i = 1; i <= 5; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%06d.jpg", i);
        const auto img = decodeJpeg(readFile(dirPath / name));
        ASSERT_TRUE(img.ok) << name << ": " << img.error;
        EXPECT_GE(psnrRgbVsBgra(img.rgb, *fr[static_cast<std::size_t>(i - 1)]), 25.0);
    }
    EXPECT_FALSE(std::filesystem::exists(dirPath / "frame_000006.jpg"));
    const auto wav = parseWav(readFile(dirPath / "audio.wav"));
    ASSERT_TRUE(wav.ok) << wav.error;
    EXPECT_EQ(wav.frames, 8000u);
    EXPECT_EQ(wav.bits, 16);
}
