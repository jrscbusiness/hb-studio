#include <gtest/gtest.h>

#include "recording/file_naming.hpp"
#include "recording_test_util.hpp"

using namespace hb::rec;
using namespace hb::rectest;

static std::filesystem::path hbPath(const std::string& u) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(u.data()), u.size()));
}

TEST(RecordingNaming, TemplateExpansion) {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 9;
    t.tm_mday = 21;
    t.tm_hour = 14;
    t.tm_min = 5;
    t.tm_sec = 33;
    EXPECT_EQ(expandNameTemplate("HBStudio_{date}_{time}", t, 1), "HBStudio_2026-10-21_14-05-33");
    EXPECT_EQ(expandNameTemplate("take {n} {n}", t, 7), "take 7 7");
    EXPECT_EQ(expandNameTemplate("{unknown}", t, 1), "{unknown}");
}

TEST(RecordingNaming, SanitizeRemovesTraversalSeparatorsAndInvalidChars) {
    EXPECT_EQ(sanitizeFileName("../../etc/passwd"), "____etc_passwd");
    EXPECT_EQ(sanitizeFileName("a/b\\c"), "a_b_c");
    EXPECT_EQ(sanitizeFileName("C:\\Windows\\x"), "C__Windows_x");
    EXPECT_EQ(sanitizeFileName("a<b>c:d\"e|f?g*h"), "a_b_c_d_e_f_g_h");
    EXPECT_EQ(sanitizeFileName("tab\there\nnew"), "tabherenew");
    EXPECT_EQ(sanitizeFileName("name. . "), "name");
    EXPECT_EQ(sanitizeFileName(".hidden"), "hidden");
    EXPECT_EQ(sanitizeFileName(""), "HBStudio");
    EXPECT_EQ(sanitizeFileName("  "), "HBStudio");
    EXPECT_EQ(sanitizeFileName("../../etc/passwd").find(".."), std::string::npos);
    EXPECT_EQ(sanitizeFileName("..."), "_");
    EXPECT_EQ(sanitizeFileName("a..b"), "a_b");
    for (const char* n : {"CON", "con", "NUL", "aux", "COM1", "lpt9", "PRN", "con.txt"})
        EXPECT_EQ(sanitizeFileName(n)[0], '_') << n;
    EXPECT_EQ(sanitizeFileName("COM10"), "COM10");  // not a device name
    EXPECT_EQ(sanitizeFileName("Gravação ção"), "Gravação ção");
}

TEST(RecordingNaming, SanitizeCapsLengthOnUtf8Boundary) {
    std::string s;
    for (int i = 0; i < 200; ++i) s += "ç";  // 2 bytes each
    const std::string r = sanitizeFileName(s);
    EXPECT_LE(r.size(), 120u);
    EXPECT_EQ(r.size() % 2, 0u);  // never cut in the middle of a character
}

TEST(RecordingNaming, NeverOverwritesExistingFiles) {
    TempDir dir("naming");
    const std::string f = dir.folderUtf8();
    const std::string p1 = uniqueOutputPath(f, "rec", "wav");
    { std::ofstream(hbPath(p1)) << "x"; }
    const std::string p2 = uniqueOutputPath(f, "rec", "wav");
    EXPECT_NE(p1, p2);
    EXPECT_NE(p2.find("rec (2).wav"), std::string::npos);
    { std::ofstream(hbPath(p2)) << "x"; }
    EXPECT_NE(uniqueOutputPath(f, "rec", "wav").find("rec (3).wav"), std::string::npos);
    // folders (no extension)
    std::filesystem::create_directories(hbPath(uniqueOutputPath(f, "seq", "")));
    EXPECT_NE(uniqueOutputPath(f, "seq", "").find("seq (2)"), std::string::npos);
}

TEST(RecordingNaming, CounterTemplatePicksFirstFreeNumber) {
    TempDir dir("naming2");
    std::tm t{};
    const std::string f = dir.folderUtf8();
    EXPECT_EQ(firstFreeName(f, "take_{n}", t, "wav"), "take_1");
    { std::ofstream(hbPath(dir.utf8("take_1.wav"))) << "x"; }
    { std::ofstream(hbPath(dir.utf8("take_2.wav"))) << "x"; }
    EXPECT_EQ(firstFreeName(f, "take_{n}", t, "wav"), "take_3");
}
