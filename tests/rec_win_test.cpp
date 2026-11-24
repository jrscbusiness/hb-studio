// Windows-only: writes 2 s of synthetic A/V with each Media Foundation writer, then re-reads the file with the
// MF Source Reader to prove it is a valid, decodable container with the expected streams and duration.
#include <gtest/gtest.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <filesystem>
#include <vector>

#include "recording/windows/mf_writers.hpp"

using Microsoft::WRL::ComPtr;
using namespace hb::rec;

namespace {
struct Probe { bool ok = false; bool video = false, audio = false; double seconds = 0; };

Probe inspect(const std::filesystem::path& p) {
    Probe r;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    {
        ComPtr<IMFSourceReader> rd;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(p.wstring().c_str(), nullptr, &rd))) {
            r.ok = true;
            ComPtr<IMFMediaType> t;
            r.video = SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t));
            t.Reset();
            r.audio = SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &t));
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8)
                r.seconds = static_cast<double>(v.uhVal.QuadPart) / 1e7;
            PropVariantClear(&v);
        }
    }
    MFShutdown();
    CoUninitialize();
    return r;
}

void roundTrip(const std::string& id, bool video) {
    registerWindowsWriters();
    RecordingProfile prof;
    WriterFactory f;
    if (!WriterRegistry::instance().find(id, prof, f)) GTEST_SKIP() << "codificador indisponível neste Windows: " << id;
    auto w = f();
    const auto path = std::filesystem::temp_directory_path() / ("hb_rec_test_" + id + "." + prof.extension);
    std::filesystem::remove(path);
    VideoFormat vf{video ? 640 : 0, video ? 360 : 0, 30, 1};
    AudioFormat af{48000, 2};
    std::string err;
    ASSERT_TRUE(w->open(path.string(), prof, vf, af, err)) << err;
    std::vector<std::uint8_t> frame(640 * 360 * 4);
    std::vector<float> aud(1600 * 2);
    long long samples = 0;
    for (int i = 0; i < 60; ++i) {
        if (video) {
            for (size_t k = 0; k < frame.size(); k += 4) { frame[k] = static_cast<uint8_t>(i * 4); frame[k + 1] = 128; frame[k + 2] = static_cast<uint8_t>(k / 1000); frame[k + 3] = 255; }
            ASSERT_TRUE(w->writeVideo(frame.data(), 640 * 4, i * 33333333LL, err)) << err;
        }
        for (int n = 0; n < 1600; ++n) {
            const float s = 0.3f * std::sin(2.f * 3.14159265f * 440.f * static_cast<float>(samples + n) / 48000.f);
            aud[2 * n] = aud[2 * n + 1] = s;
        }
        ASSERT_TRUE(w->writeAudio(aud.data(), 1600, samples * 1000000000LL / 48000, err)) << err;
        samples += 1600;
    }
    ASSERT_TRUE(w->close(err)) << err;
    const Probe p = inspect(path);
    EXPECT_TRUE(p.ok) << "o arquivo não abre no Source Reader";
    EXPECT_EQ(p.video, video);
    EXPECT_TRUE(p.audio);
    EXPECT_NEAR(p.seconds, 2.0, 0.3);
    EXPECT_GT(std::filesystem::file_size(path), 1000u);
    std::filesystem::remove(path);
}
}  // namespace

TEST(RecWin, Mp4H264) { roundTrip("mp4-h264-aac", true); }
TEST(RecWin, Mp4Hevc) { roundTrip("mp4-hevc-aac", true); }
TEST(RecWin, M4a) { roundTrip("m4a-aac", false); }
TEST(RecWin, Mp3) { roundTrip("mp3", false); }

// ---- FEATURE: multitrack ----
// The ISO writer is video-only: the file must open, have a video stream, no audio stream, and ~2 s.
TEST(RecWin, Mp4H264IsoVideoOnly) {
    registerWindowsWriters();
    RecordingProfile prof;
    WriterFactory f;
    if (!WriterRegistry::instance().find("mp4-h264-iso", prof, f)) GTEST_SKIP() << "codificador H.264 indisponivel";
    auto w = f();
    const auto path = std::filesystem::temp_directory_path() / "hb_rec_test_iso.mp4";
    std::filesystem::remove(path);
    std::string err;
    ASSERT_TRUE(w->open(path.string(), prof, VideoFormat{640, 360, 30, 1}, AudioFormat{}, err)) << err;
    std::vector<std::uint8_t> frame(640 * 360 * 4, 128);
    for (int i = 0; i < 60; ++i) {
        for (size_t k = 0; k < frame.size(); k += 4) frame[k] = static_cast<std::uint8_t>(i * 4);
        ASSERT_TRUE(w->writeVideo(frame.data(), 640 * 4, i * 33333333LL, err)) << err;
    }
    ASSERT_TRUE(w->writeAudio(nullptr, 0, 0, err));
    ASSERT_TRUE(w->close(err)) << err;
    const Probe p = inspect(path);
    EXPECT_TRUE(p.ok);
    EXPECT_TRUE(p.video);
    EXPECT_FALSE(p.audio);
    EXPECT_NEAR(p.seconds, 2.0, 0.3);
    std::filesystem::remove(path);
}
// ---- /FEATURE ----
