#include <gtest/gtest.h>

#include <filesystem>

#include "captions/settings.hpp"

using namespace hb::captions;
namespace fs = std::filesystem;

TEST(CaptionsSettings, ApplyValidatesEveryKey) {
    CaptionSettings s;
    EXPECT_TRUE(applySetting(s, "language", "en"));
    EXPECT_EQ(s.language, "en");
    EXPECT_FALSE(applySetting(s, "language", "xx"));
    EXPECT_EQ(s.language, "en");
    EXPECT_TRUE(applySetting(s, "engine", "cloud"));
    EXPECT_FALSE(applySetting(s, "engine", "magic"));
    EXPECT_TRUE(applySetting(s, "model", "medio"));
    EXPECT_FALSE(applySetting(s, "model", "../../etc"));
    EXPECT_TRUE(applySetting(s, "profanity", "1"));
    EXPECT_TRUE(s.profanityFilter);
    EXPECT_FALSE(applySetting(s, "profanity", "talvez"));
    EXPECT_TRUE(applySetting(s, "maxChars", "30"));
    EXPECT_FALSE(applySetting(s, "maxChars", "5"));
    EXPECT_FALSE(applySetting(s, "maxChars", "abc"));
    EXPECT_FALSE(applySetting(s, "maxLines", "9"));
    EXPECT_TRUE(applySetting(s, "cloudUrl", "https://x.example/v1"));
    EXPECT_FALSE(applySetting(s, "cloudUrl", "http://x.example/v1"));
    EXPECT_FALSE(applySetting(s, "cloudKey", std::string("a\nb")));
    EXPECT_TRUE(applySetting(s, "cloudKey", "sk-1"));
    EXPECT_FALSE(applySetting(s, "desconhecido", "1"));
    EXPECT_EQ(s.maxCharsPerLine, 30);
}

TEST(CaptionsSettings, JsonRoundTripAndTolerance) {
    CaptionSettings s;
    s.language = "es";
    s.model = "medio";
    s.saveSrt = false;
    s.cloudKey = "k";
    CaptionSettings r;
    ASSERT_TRUE(settingsFromJson(settingsToJson(s), r));
    EXPECT_EQ(r.language, "es");
    EXPECT_EQ(r.model, "medio");
    EXPECT_FALSE(r.saveSrt);
    EXPECT_EQ(r.cloudKey, "k");
    // invalid values fall back to defaults; unknown keys ignored
    ASSERT_TRUE(settingsFromJson("{\"language\":\"zz\",\"maxChars\":1000,\"extra\":1,\"profanity\":true}", r));
    EXPECT_EQ(r.language, "pt");
    EXPECT_EQ(r.maxCharsPerLine, 42);
    EXPECT_TRUE(r.profanityFilter);
    EXPECT_FALSE(settingsFromJson("garbage", r));
}

TEST(CaptionsSettings, FileHandling) {
    const fs::path dir = fs::temp_directory_path() / "hb_captions_settings_test";
    fs::create_directories(dir);
    const std::string p = (dir / "captions.json").string();
    CaptionSettings s;
    EXPECT_TRUE(loadSettings(p, s));   // missing file -> defaults
    EXPECT_EQ(s.language, "pt");
    s.language = "fr";
    ASSERT_TRUE(saveSettings(p, s));
    CaptionSettings r;
    ASSERT_TRUE(loadSettings(p, r));
    EXPECT_EQ(r.language, "fr");
    std::error_code ec;
    fs::remove_all(dir, ec);
    EXPECT_FALSE(saveSettings("", s));
}

TEST(CaptionsSettings, LanguageList) {
    std::size_t n = 0;
    const char* const* l = supportedLanguages(n);
    EXPECT_GE(n, 5u);
    EXPECT_EQ(std::string(l[0]), "pt");   // Portuguese first
    EXPECT_TRUE(isSupportedLanguage("auto"));
}
