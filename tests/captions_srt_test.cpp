#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "captions/srt.hpp"

using namespace hb::captions;

TEST(CaptionsSrt, TimeFormat) {
    EXPECT_EQ(formatSrtTime(0), "00:00:00,000");
    EXPECT_EQ(formatSrtTime(61005), "00:01:01,005");
    EXPECT_EQ(formatSrtTime(3723456), "01:02:03,456");
    EXPECT_EQ(formatSrtTime(-5), "00:00:00,000");
}

TEST(CaptionsSrt, BuildsNumberedCuesWithCrlf) {
    SrtBuilder b;
    b.add(1000, 3000, "Olá\nmundo");
    b.add(3500, 6000, "Segunda");
    EXPECT_EQ(b.str(),
              "1\r\n00:00:01,000 --> 00:00:03,000\r\nOlá\r\nmundo\r\n\r\n"
              "2\r\n00:00:03,500 --> 00:00:06,000\r\nSegunda\r\n\r\n");
}

TEST(CaptionsSrt, SanitizesAndFixesTiming) {
    SrtBuilder b;
    b.add(0, 100, "  \n\n");   // empty: ignored
    EXPECT_EQ(b.size(), 0u);
    b.add(1000, 1100, "curto");   // stretched to min duration
    EXPECT_EQ(b.cues()[0].endMs, 1800);
    b.add(1200, 5000, "a\r\n\r\nb");   // overlaps previous: start pushed; blank line removed
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b.cues()[1].startMs, 1800);
    EXPECT_EQ(b.cues()[1].text, "a\nb");
    b.add(10000, 60000, "longo");   // capped
    EXPECT_EQ(b.cues()[2].endMs - b.cues()[2].startMs, 8000);
}

TEST(CaptionsSrt, SaveFileRoundTrip) {
    const auto dir = std::filesystem::temp_directory_path() / "hb_captions_srt_test";
    std::filesystem::create_directories(dir);
    const auto p = dir / "out.srt";
    ASSERT_TRUE(saveSrtFile(p.string(), "abc\r\n"));
    {
        std::ifstream f(p, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(s, "abc\r\n");
    }   // closed before removal (Windows)
    ASSERT_TRUE(saveSrtFile(p.string(), "def"));   // overwrite
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    EXPECT_FALSE(saveSrtFile("", "x"));
}
