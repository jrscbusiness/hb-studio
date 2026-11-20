#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "app/ptz_settings.hpp"

using namespace hb::app;

TEST(PtzSettings, DefaultsAreSane) {
    PtzSettings s;
    auto c = s.config(0);
    EXPECT_EQ(c.protocol, "onvif");
    EXPECT_EQ(c.slotCount, 10);
    EXPECT_EQ(c.presetBase, 1);
    EXPECT_EQ(c.storage, "camera");
    EXPECT_EQ(defaultPtzPort("onvif"), 80);
    EXPECT_EQ(defaultPtzPort("visca-ip"), 52381);
    EXPECT_EQ(defaultPtzPort("pelco-d"), 0);
}

TEST(PtzSettings, SanitizeClampsEverything) {
    PtzCameraConfig c;
    c.protocol = "rm -rf";
    c.host = "192.168.1.5 \"<x>\"";
    c.port = 99999;
    c.address = 0;
    c.slotCount = 5000;
    c.presetBase = 7;
    c.storage = "??";
    c.speed = 0;
    auto s = sanitize(c);
    EXPECT_EQ(s.protocol, "onvif");
    EXPECT_EQ(s.host, "192.168.1.5x");
    EXPECT_EQ(s.port, 65535);
    EXPECT_EQ(s.address, 1);
    EXPECT_EQ(s.slotCount, PtzSettings::kMaxSlots);
    EXPECT_EQ(s.presetBase, 1);
    EXPECT_EQ(s.storage, "camera");
    EXPECT_EQ(s.speed, 1);
    c.slotCount = 1;
    EXPECT_EQ(sanitize(c).slotCount, 10);  // never fewer than 10 preset buttons
}

TEST(PtzSettings, ApplyToAllAndVersion) {
    PtzSettings s;
    const auto v0 = s.version();
    PtzCameraConfig c;
    c.protocol = "visca-ip";
    c.slotCount = 16;
    c.presetBase = 0;
    s.applyToAll(c);
    EXPECT_GT(s.version(), v0);
    for (int i = 0; i < PtzSettings::kCameras; ++i) {
        EXPECT_EQ(s.config(i).protocol, "visca-ip");
        EXPECT_EQ(s.config(i).slotCount, 16);
        EXPECT_EQ(s.config(i).presetBase, 0);
    }
    EXPECT_FALSE(s.setConfig(PtzSettings::kCameras, c));
    EXPECT_FALSE(s.setConfig(-1, c));
}

TEST(PtzSettings, PresetNamesAreSanitised) {
    PtzSettings s;
    EXPECT_TRUE(s.setPresetName(2, 3, "  Púlpito\n\t "));
    EXPECT_EQ(s.presetName(2, 3), "Púlpito");
    s.setPresetName(2, 4, std::string(100, 'x'));
    EXPECT_EQ(s.presetName(2, 4).size(), 24u);
    s.setPresetName(2, 3, "");
    EXPECT_EQ(s.presetName(2, 3), "");
    EXPECT_FALSE(s.setPresetName(0, 64, "x"));
    EXPECT_FALSE(s.setPresetName(PtzSettings::kCameras, 0, "x"));
    EXPECT_EQ(s.presetName(99, 99), "");
}

TEST(PtzSettings, NameTruncationNeverCutsAUtf8CodePoint) {
    PtzSettings s;
    std::string name;
    for (int i = 0; i < 40; ++i) name += "é";          // 2 bytes per character
    s.setPresetName(0, 0, name);
    const auto r = s.presetName(0, 0);
    EXPECT_EQ(r.size(), 48u);                         // 24 characters x 2 bytes, whole code points only
    PtzSettings t;
    EXPECT_TRUE(t.fromJson(s.toJson()));              // and the result is still valid JSON/UTF-8 text
    EXPECT_EQ(t.presetName(0, 0), r);
}

TEST(PtzSettings, JsonRoundTrip) {
    PtzSettings a;
    PtzCameraConfig c;
    c.protocol = "pelco-d";
    c.host = "10.0.0.7";
    c.port = 2000;
    c.address = 12;
    c.slotCount = 32;
    c.presetBase = 0;
    c.storage = "software";
    c.speed = 80;
    c.invertTilt = true;
    a.setConfig(5, c);
    a.setPresetName(5, 0, "Altar \"principal\"");
    a.setPresetName(5, 31, "Banda ✝");

    PtzSettings b;
    ASSERT_TRUE(b.fromJson(a.toJson()));
    auto r = b.config(5);
    EXPECT_EQ(r.protocol, "pelco-d");
    EXPECT_EQ(r.host, "10.0.0.7");
    EXPECT_EQ(r.port, 2000);
    EXPECT_EQ(r.address, 12);
    EXPECT_EQ(r.slotCount, 32);
    EXPECT_EQ(r.presetBase, 0);
    EXPECT_EQ(r.storage, "software");
    EXPECT_EQ(r.speed, 80);
    EXPECT_TRUE(r.invertTilt);
    EXPECT_FALSE(r.invertPan);
    EXPECT_EQ(b.presetName(5, 0), "Altar \"principal\"");
    EXPECT_EQ(b.presetName(5, 31), "Banda ✝");
}

TEST(PtzSettings, CorruptOrHostileInputIsRejectedWithoutChange) {
    PtzSettings s;
    s.setPresetName(0, 0, "keep");
    for (const char* bad : {"", "{", "[1,2", "{\"cameras\":5}", "nope", "{\"cameras\":[{\"names\":[1,2,3]}]", "\x01\x02"})
        EXPECT_FALSE(s.fromJson(bad)) << bad;
    EXPECT_EQ(s.presetName(0, 0), "keep");
    std::string deep(5000, '[');
    EXPECT_FALSE(s.fromJson(deep));  // bounded recursion, no stack overflow
    EXPECT_FALSE(s.fromJson("{\"cameras\":[{\"names\":[\"a\\"));
}

TEST(PtzSettings, PartialAndExtraFieldsToleratedAndClamped) {
    PtzSettings s;
    ASSERT_TRUE(s.fromJson(R"({"cameras":[{"protocol":"visca-ip","slots":9999,"unknown":{"x":[1,2]}}, 7, {"speed":-4}],"extra":true})"));
    EXPECT_EQ(s.config(0).protocol, "visca-ip");
    EXPECT_EQ(s.config(0).slotCount, 64);
    EXPECT_EQ(s.config(1).protocol, "onvif");  // non-object entry skipped
    EXPECT_EQ(s.config(2).speed, 1);
}

TEST(PtzSettings, SaveLoadFileAndMissingFile) {
    const std::string path = "ptz_settings_test.json";
    PtzSettings a;
    a.setPresetName(1, 1, "Louvor");
    ASSERT_TRUE(a.save(path));
    PtzSettings b;
    ASSERT_TRUE(b.load(path));
    EXPECT_EQ(b.presetName(1, 1), "Louvor");
    std::remove(path.c_str());
    EXPECT_FALSE(b.load("does_not_exist.json"));
    { std::ofstream(path) << "garbage{{{"; }
    PtzSettings c;
    EXPECT_FALSE(c.load(path));
    EXPECT_EQ(c.config(0).slotCount, 10);
    std::remove(path.c_str());
}

TEST(PtzSettings, ParseConfigFromUiAndUiJson) {
    PtzCameraConfig c;
    ASSERT_TRUE(PtzSettings::parseConfig(R"({"protocol":"visca-ip","host":"1.2.3.4","port":52381,"slots":16,"base":0,"storage":"software","speed":70,"invPan":true})", c));
    EXPECT_EQ(c.slotCount, 16);
    EXPECT_TRUE(c.invertPan);
    EXPECT_FALSE(PtzSettings::parseConfig("{bad", c));
    PtzSettings s;
    s.setConfig(0, c);
    const auto ui = s.uiJson();
    EXPECT_NE(ui.find("\"t\":\"ptz\""), std::string::npos);
    EXPECT_NE(ui.find("\"defPort\":52381"), std::string::npos);
    EXPECT_NE(ui.find("\"connected\":false"), std::string::npos);
}
