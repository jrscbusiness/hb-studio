#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <thread>

#include "app/engine.hpp"
#include "app/source_assignments.hpp"
#include "video/frame_ops.hpp"

using namespace hb;

namespace {
// A fake device: a solid colour frame of its own native size, scaled with fitFrame (like a real one).
class FakeDevice final : public video::LiveSource {
public:
    FakeDevice(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r, video::SourceStatus::State st = video::SourceStatus::State::Online)
        : name_("fake"), st_(st) {
        video::prepareFrame(native_, w, h);
        for (size_t i = 0; i < native_.data.size(); i += 4) { native_.data[i] = b; native_.data[i + 1] = g; native_.data[i + 2] = r; native_.data[i + 3] = 0; }
    }
    const std::string& name() const override { return name_; }
    void render(video::Frame& f, std::uint64_t) const override { video::fitFrame(native_, f); }
    video::SourceStatus status() const override { video::SourceStatus s; s.state = st_; s.width = native_.width; s.height = native_.height; return s; }
private:
    std::string name_;
    video::Frame native_;
    video::SourceStatus::State st_;
};
}  // namespace

TEST(FitFrame, SameAspectFillsEverythingAndForcesOpaqueAlpha) {
    video::Frame src, dst;
    video::prepareFrame(src, 16, 9);
    for (size_t i = 0; i < src.data.size(); i += 4) { src.data[i] = 10; src.data[i + 1] = 20; src.data[i + 2] = 30; src.data[i + 3] = 0; }
    video::prepareFrame(dst, 160, 90);
    video::fitFrame(src, dst);
    for (size_t i = 0; i < dst.data.size(); i += 4) {
        ASSERT_EQ(dst.data[i], 10); ASSERT_EQ(dst.data[i + 1], 20); ASSERT_EQ(dst.data[i + 2], 30); ASSERT_EQ(dst.data[i + 3], 255);
    }
}

TEST(FitFrame, FourByThreeIsPillarboxedInSixteenByNine) {
    video::Frame src, dst;
    video::prepareFrame(src, 40, 30);
    std::fill(src.data.begin(), src.data.end(), std::uint8_t{200});
    video::prepareFrame(dst, 160, 90);
    video::fitFrame(src, dst);
    auto px = [&](int x, int y) { return dst.data[static_cast<size_t>(y) * dst.stride + x * 4]; };
    EXPECT_EQ(px(2, 45), 0);      // left bar
    EXPECT_EQ(px(157, 45), 0);    // right bar
    EXPECT_EQ(px(80, 45), 200);   // image
    EXPECT_EQ(px(40, 45), 200);   // image starts at x=20 for a 120-wide picture
    EXPECT_EQ(px(10, 45), 0);
}

TEST(FitFrame, DegenerateInputsAreSafe) {
    video::Frame empty, dst, src;
    video::prepareFrame(dst, 32, 18);
    EXPECT_NO_THROW(video::fitFrame(empty, dst));
    for (auto b : dst.data) (void)b;
    video::prepareFrame(src, 1, 1);
    EXPECT_NO_THROW(video::fitFrame(src, dst));
    video::Frame bad;
    bad.width = 100; bad.height = 100; bad.stride = 400;        // claims 40 KB but holds nothing
    EXPECT_NO_THROW(video::fitFrame(bad, dst));
    video::Frame zero;
    EXPECT_NO_THROW(video::fitFrame(src, zero));
    video::Frame huge;
    video::prepareFrame(huge, 4096, 4);
    video::Frame tiny;
    video::prepareFrame(tiny, 2, 2);
    EXPECT_NO_THROW(video::fitFrame(huge, tiny));
}

TEST(Slots, AssignRoutesRenderingAndClearGoesBackToTestSource) {
    app::Engine e({320, 180, {60, 1}});
    video::Frame test, live;
    video::prepareFrame(test, 64, 36);
    video::prepareFrame(live, 64, 36);
    e.switcher().source(0).render(test, 0);
    EXPECT_TRUE(e.slotInfo(0).live == false);

    auto dev = std::make_shared<FakeDevice>(1920, 1080, 1, 2, 3);
    ASSERT_TRUE(e.assignSlot(0, dev, "dev#1", "OBS Virtual Camera"));
    e.switcher().source(0).render(live, 0);
    EXPECT_EQ(live.data[0], 1); EXPECT_EQ(live.data[1], 2); EXPECT_EQ(live.data[2], 3); EXPECT_EQ(live.data[3], 255);
    auto info = e.slotInfo(0);
    EXPECT_TRUE(info.live);
    EXPECT_EQ(info.label, "OBS Virtual Camera");
    EXPECT_EQ(info.deviceId, "dev#1");
    EXPECT_EQ(info.status.width, 1920);
    EXPECT_FALSE(e.switcher().source(0).isTestSource());

    ASSERT_TRUE(e.clearSlot(0));
    video::Frame again;
    video::prepareFrame(again, 64, 36);
    e.switcher().source(0).render(again, 0);
    EXPECT_EQ(again.data, test.data);
    EXPECT_FALSE(e.slotInfo(0).live);
}

TEST(Slots, SixteenSlotsAreAllUsable) {
    app::Engine e;
    EXPECT_EQ(app::Engine::kSourceCount, 16);
    auto dev = std::make_shared<FakeDevice>(16, 9, 1, 2, 3);
    EXPECT_TRUE(e.assignSlot(15, dev, "id15", "Segundo banco, última câmera"));
    EXPECT_TRUE(e.slotInfo(15).live);
    EXPECT_EQ(e.slotInfo(15).label, "Segundo banco, última câmera");
    video::Frame f;
    EXPECT_TRUE(e.renderUiFrame("s15", f));
    EXPECT_TRUE(e.clearSlot(15));
    EXPECT_FALSE(e.slotInfo(15).live);
}

TEST(Slots, InvalidArgumentsAreRejected) {
    app::Engine e;
    auto dev = std::make_shared<FakeDevice>(16, 9, 0, 0, 0);
    EXPECT_FALSE(e.assignSlot(-1, dev, "x", "x"));
    EXPECT_FALSE(e.assignSlot(app::Engine::kSourceCount, dev, "x", "x"));
    EXPECT_FALSE(e.assignSlot(0, nullptr, "x", "x"));
    EXPECT_FALSE(e.clearSlot(99));
    EXPECT_FALSE(e.slotInfo(99).live);
}

TEST(Slots, LiveDeviceReachesProgramThroughTheSwitcher) {
    app::Engine e({320, 180, {60, 1}});
    e.assignSlot(2, std::make_shared<FakeDevice>(640, 480, 9, 8, 7), "id", "Webcam");
    e.start();
    e.switcher().setPreview(2);
    e.switcher().cut();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto f = e.program().latest();
    ASSERT_TRUE(f);
    // centre pixel of a 4:3 picture pillarboxed into 16:9 is the device colour
    const size_t c = (static_cast<size_t>(f->height / 2) * f->stride) + static_cast<size_t>(f->width / 2) * 4;
    EXPECT_EQ(f->data[c], 9); EXPECT_EQ(f->data[c + 1], 8); EXPECT_EQ(f->data[c + 2], 7);
    EXPECT_EQ(f->data[0], 0);   // black bar at the left edge
    e.stop();
}

TEST(Assignments, RoundTripSanitiseAndHostileFiles) {
    app::SourceAssignments a;
    EXPECT_TRUE(a.set(0, "\\\\?\\usb#vid_046d&pid_0825#abc", "Logitech C270"));
    EXPECT_TRUE(a.set(7, "OBS Virtual Camera", "OBS Virtual Camera"));
    EXPECT_FALSE(a.set(app::SourceAssignments::kSlots, "x", "x"));
    EXPECT_FALSE(a.set(-1, "x", "x"));
    EXPECT_FALSE(a.set(1, "", "x"));
    EXPECT_FALSE(a.set(1, "\t\n", "x"));    // sanitises to empty
    EXPECT_TRUE(a.set(2, "id\twith\ttabs\nand newlines", "na\tme"));
    EXPECT_EQ(a.get(2).id, "idwithtabsand newlines");
    EXPECT_EQ(a.get(2).name, "name");

    app::SourceAssignments b;
    b.parse(a.serialize());
    EXPECT_EQ(b.get(0).id, a.get(0).id);
    EXPECT_EQ(b.get(0).name, "Logitech C270");
    EXPECT_EQ(b.get(7).name, "OBS Virtual Camera");
    EXPECT_TRUE(b.get(3).empty());

    app::SourceAssignments c;
    c.parse("garbage\n99\tx\ty\n-1\tx\ty\n3\t\t\n4\tonly-id\n\t\t\n5\tok\tname\tmore\n");
    EXPECT_TRUE(c.get(3).empty());
    EXPECT_EQ(c.get(4).id, "only-id");
    EXPECT_EQ(c.get(4).name, "");
    EXPECT_EQ(c.get(5).id, "ok");
    EXPECT_EQ(c.get(5).name, "namemore");   // the stray tab was dropped, nothing leaked into other fields
    std::string big(100000, 'a');
    big.insert(0, "1\t");
    EXPECT_NO_THROW(c.parse(big));
    EXPECT_LE(c.get(1).id.size(), 512u);
}

TEST(Assignments, FilePersistence) {
    const std::string path = "assign_test.txt";
    app::SourceAssignments a;
    a.set(1, "dev-1", "Cam A");
    ASSERT_TRUE(a.save(path));
    app::SourceAssignments b;
    ASSERT_TRUE(b.load(path));
    EXPECT_EQ(b.get(1).name, "Cam A");
    std::remove(path.c_str());
    EXPECT_FALSE(b.load("nonexistent.txt"));
    EXPECT_EQ(b.get(1).name, "Cam A");   // untouched when the file is missing
}
