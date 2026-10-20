#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "ptz/camera_config.hpp"
#include "ptz/preset_manager.hpp"
#include "ptz/preset_store.hpp"
#include "ptz/provider_factory.hpp"

using namespace hb::ptz;

TEST(CameraConfig, NormalizeClampsEverything) {
    CameraPtzConfig c;
    c.presetCount = 3;
    c.presetBase = 7;
    c.protocol = PtzProtocol::ViscaSerial;
    c.address = 99;
    c.normalize();
    EXPECT_EQ(c.presetCount, 10);
    EXPECT_EQ(c.presetBase, 1);
    EXPECT_EQ(c.address, 7);
    c.presetCount = 1000;
    c.normalize();
    EXPECT_EQ(c.presetCount, 64);
    c.protocol = PtzProtocol::PelcoP;
    c.address = 77;
    c.normalize();
    EXPECT_EQ(c.address, 31);
}

TEST(CameraConfig, ButtonToDeviceNumbering) {
    CameraPtzConfig c;
    c.presetCount = 12;
    c.presetBase = 0;
    EXPECT_EQ(c.deviceId(0), 0);
    EXPECT_EQ(c.deviceId(11), 11);
    EXPECT_EQ(c.deviceId(12), -1);
    EXPECT_EQ(c.deviceId(-1), -1);
    c.presetBase = 1;
    EXPECT_EQ(c.deviceId(0), 1);
    EXPECT_EQ(c.deviceId(11), 12);
    EXPECT_EQ(c.buttonIndex(12), 11);
    EXPECT_EQ(c.buttonIndex(0), -1);
    EXPECT_EQ(c.buttonIndex(13), -1);
    EXPECT_EQ(c.effectivePort(), 52381);
}

TEST(PresetStore, RoundTripIncludingConfigNamesAndPositions) {
    PresetStore s;
    CameraPtzConfig c;
    c.cameraId = "cam\"1\\";
    c.protocol = PtzProtocol::PelcoD;
    c.host = "COM3";
    c.address = 4;
    c.presetCount = 20;
    c.presetBase = 0;
    c.storage = PresetStorage::Software;
    s.setConfig(c);
    PresetEntry e;
    e.id = 3;
    e.name = "Pulpito \xC3\xA7\n\"x\"";
    e.position = PtzPosition{-1234.5, 99, 0.125, PositionUnits::ViscaRaw};
    s.set(c.cameraId, e);
    s.set(c.cameraId, PresetEntry{1, "Home", std::nullopt});
    s.set("other", PresetEntry{2, "B", PtzPosition{0.1, 0.2, 0.3, PositionUnits::Normalized}});

    PresetStore t;
    ASSERT_EQ(t.fromJson(s.toJson()), LoadStatus::Ok);
    EXPECT_EQ(t.config(c.cameraId), s.config(c.cameraId));
    EXPECT_EQ(t.list(c.cameraId), s.list(c.cameraId));
    EXPECT_EQ(t.list("other"), s.list("other"));
    EXPECT_EQ(t.list(c.cameraId).front().id, 1);  // sorted
    EXPECT_EQ(t.cameras().size(), 2u);
}

TEST(PresetStore, JsonCarriesSchemaVersion) {
    EXPECT_NE(PresetStore{}.toJson().find("\"schema\": 1"), std::string::npos);
}

TEST(PresetStore, CorruptInputIsToleratedNeverThrows) {
    const char* bad[] = {"", "{", "not json", "[1,2,3]", "{\"schema\":1}", "{\"schema\":\"x\",\"cameras\":{}}",
                         "{\"schema\":1,\"cameras\":[]}", "{\"schema\":1,\"cameras\":{\"a\":{\"presets\":[{\"id\":1,",
                         "{\"schema\":1,\"cameras\":{}} trailing"};
    for (auto b : bad) {
        PresetStore s;
        s.set("x", PresetEntry{1, "a", std::nullopt});
        EXPECT_EQ(s.fromJson(b), LoadStatus::Corrupt) << b;
        EXPECT_TRUE(s.cameras().empty()) << b;
    }
    // Deep nesting must not overflow the stack.
    PresetStore s;
    EXPECT_EQ(s.fromJson(std::string(100000, '[')), LoadStatus::Corrupt);
}

TEST(PresetStore, BadEntriesAreSkippedGoodOnesKept) {
    PresetStore s;
    auto st = s.fromJson(R"({"schema":1,"cameras":{"c":{"presets":[
        {"id":1,"name":"ok"},{"id":-4,"name":"neg"},{"id":"x"},5,{"id":2.5},{"id":300},{"id":3,"name":7}]}}})");
    ASSERT_EQ(st, LoadStatus::Ok);
    auto l = s.list("c");
    ASSERT_EQ(l.size(), 2u);  // ids 1 and 3 (name of wrong type ignored)
    EXPECT_EQ(l[0].name, "ok");
    EXPECT_EQ(s.skippedEntries(), 5u);
}

TEST(PresetStore, NewerSchemaIsRefusedNotOverwritten) {
    PresetStore s;
    EXPECT_EQ(s.fromJson(R"({"schema":99,"cameras":{}})"), LoadStatus::UnsupportedVersion);
}

TEST(PresetStore, FileSaveLoadAndCorruptFileIsPreserved) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hb_ptz_preset_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "presets.json").string();

    PresetStore s;
    EXPECT_EQ(s.loadFromFile(path), LoadStatus::Missing);
    s.set("c", PresetEntry{1, "Home", std::nullopt});
    ASSERT_TRUE(s.saveToFile(path));
    ASSERT_TRUE(s.saveToFile(path));  // overwrite works
    PresetStore t;
    ASSERT_EQ(t.loadFromFile(path), LoadStatus::Ok);
    EXPECT_EQ(t.find("c", 1)->name, "Home");

    { std::ofstream(path, std::ios::trunc) << "{{{ garbage"; }
    PresetStore u;
    EXPECT_EQ(u.loadFromFile(path), LoadStatus::Corrupt);
    EXPECT_TRUE(fs::exists(path + ".corrupt"));
    EXPECT_FALSE(fs::exists(path));
    EXPECT_FALSE(s.saveToFile((dir / "no" / "such" / "dir.json").string()));  // failure is a bool, not a throw
    fs::remove_all(dir);
}

// ---------------- PresetManager ----------------
namespace {
class MemController : public PTZController {
public:
    PTZCapabilities caps;
    std::vector<std::string> log;
    PtzPosition pos{10, 20, 30, PositionUnits::ViscaRaw};
    const PTZCapabilities& capabilities() const override { return caps; }
    PtzResult connect() override { return {}; }
    PtzResult disconnect() override { return {}; }
    PtzResult panLeft() override { return {}; }
    PtzResult panRight() override { return {}; }
    PtzResult tiltUp() override { return {}; }
    PtzResult tiltDown() override { return {}; }
    PtzResult stop() override { return {}; }
    PtzResult zoomIn() override { return {}; }
    PtzResult zoomOut() override { return {}; }
    PtzResult setPanSpeed(double) override { return {}; }
    PtzResult setTiltSpeed(double) override { return {}; }
    PtzResult setZoomSpeed(double) override { return {}; }
    PtzResult gotoPreset(int id) override { log.push_back("goto" + std::to_string(id)); return {}; }
    PtzResult savePreset(int id) override { log.push_back("save" + std::to_string(id)); return {}; }
    PtzResult deletePreset(int id) override { log.push_back("del" + std::to_string(id)); return {}; }
    PtzReply<std::vector<PresetInfo>> getPresets() override { return {}; }
    PtzResult focusAuto() override { return {}; }
    PtzResult focusManual() override { return {}; }
    PtzResult focusNear() override { return {}; }
    PtzResult focusFar() override { return {}; }
    PtzResult irisOpen() override { return {}; }
    PtzResult irisClose() override { return {}; }
    PtzReply<CameraStatus> getCameraStatus() override {
        CameraStatus s;
        s.position = pos;
        return {PtzResult::success(), s};
    }
    PtzResult moveAbsolute(const PtzPosition& p) override {
        log.push_back("abs" + std::to_string(static_cast<int>(p.pan)));
        return {};
    }
};
}  // namespace

TEST(PresetManager, CameraStorageUsesNumberingBase) {
    MemController c;
    PresetStore store;
    CameraPtzConfig cfg;
    cfg.cameraId = "cam";
    cfg.presetBase = 0;
    cfg.presetCount = 10;
    store.setConfig(cfg);
    PresetManager m(c, store, "cam");
    ASSERT_TRUE(m.save(0, "Wide").ok());
    ASSERT_TRUE(m.recall(0).ok());
    EXPECT_EQ(c.log, (std::vector<std::string>{"save0", "goto0"}));
    EXPECT_EQ(m.label(0), "Wide");
    EXPECT_TRUE(m.isUsed(0));
    EXPECT_FALSE(m.isUsed(1));

    cfg.presetBase = 1;
    store.setConfig(cfg);
    c.log.clear();
    m.recall(0);
    EXPECT_EQ(c.log.back(), "goto1");
    EXPECT_EQ(m.recall(10).status, PtzStatus::Error);  // beyond configured count
    ASSERT_TRUE(m.clear(0).ok());
    EXPECT_EQ(c.log.back(), "del1");
}

TEST(PresetManager, SoftwareStorageKeepsPositionAndUsesAbsoluteMove) {
    MemController c;
    PresetStore store;
    CameraPtzConfig cfg;
    cfg.cameraId = "cam";
    cfg.storage = PresetStorage::Software;
    store.setConfig(cfg);
    PresetManager m(c, store, "cam");
    EXPECT_EQ(m.recall(2).status, PtzStatus::Error);  // empty
    ASSERT_TRUE(m.save(2, "Pulpit").ok());
    EXPECT_EQ(c.log.size(), 0u);  // nothing stored on the camera
    ASSERT_TRUE(m.recall(2).ok());
    EXPECT_EQ(c.log.back(), "abs10");
    ASSERT_TRUE(m.rename(2, "Altar").ok());
    EXPECT_EQ(m.label(2), "Altar");
    ASSERT_TRUE(m.clear(2).ok());
    EXPECT_FALSE(m.isUsed(2));
}

TEST(ProviderFactory, BuildsPerProtocolAndRefusesIncompleteConfig) {
    CameraPtzConfig c;
    c.protocol = PtzProtocol::ViscaIp;
    c.host = "127.0.0.1";
    auto p = makeProvider(c, nullptr, nullptr);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->name(), "visca-ip");
    c.host.clear();
    EXPECT_FALSE(makeProvider(c, nullptr, nullptr));
    c.protocol = PtzProtocol::PelcoD;
    EXPECT_FALSE(makeProvider(c, nullptr, nullptr));  // needs a transport
    c.protocol = PtzProtocol::OnvifPtz;
    EXPECT_FALSE(makeProvider(c, nullptr, nullptr));  // needs a backend
}
