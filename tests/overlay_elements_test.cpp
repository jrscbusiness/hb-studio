#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>

#include "core/event_bus.hpp"
#include "overlay/manager.hpp"
#include "video/frame_ops.hpp"

using namespace hb::overlay;

namespace {

struct Fixture {
    std::int64_t t = 100000;
    hb::core::EventBus bus;
    std::unique_ptr<OverlayManager> mgr;
    hb::video::Frame frame, base;
    Fixture() {
        mgr = std::make_unique<OverlayManager>(&bus, [this] { return t; });
        hb::video::prepareFrame(base, 640, 360);
        for (std::size_t i = 0; i < base.data.size(); i += 4) { base.data[i] = 30; base.data[i + 1] = 60; base.data[i + 2] = 90; base.data[i + 3] = 255; }
    }
    // Renders the program hook over a fresh copy of the base frame; returns the number of changed pixels.
    int draw() {
        frame = base;
        mgr->programHook()(frame, 0);
        return diff();
    }
    int diff() const {
        int n = 0;
        for (std::size_t i = 0; i < frame.data.size(); i += 4)
            if (std::memcmp(&frame.data[i], &base.data[i], 4) != 0) ++n;
        return n;
    }
    std::string add(const std::string& type, std::function<void(ElementConfig&)> tweak = nullptr) {
        ElementConfig c = defaultConfig(type);
        if (tweak) tweak(c);
        return mgr->upsert(c);
    }
};

struct StubLayer : Layer {
    std::string name; int z; bool throws;
    std::vector<std::string>* log;
    StubLayer(std::string n, int zz, std::vector<std::string>* l, bool th = false) : name(std::move(n)), z(zz), throws(th), log(l) {}
    std::string id() const override { return name; }
    bool visible() const override { return true; }
    int zOrder() const override { return z; }
    Placement placement() const override { return {}; }
    bool active(const RenderContext&) const override { return true; }
    void render(hb::video::Frame&, const RenderContext&) override { log->push_back(name); if (throws) throw 1; }
};

}  // namespace

TEST(LayerStack, ZOrderReplaceRemoveAndIsolation) {
    LayerStack s;
    std::vector<std::string> log;
    s.add(std::make_shared<StubLayer>("b", 5, &log));
    s.add(std::make_shared<StubLayer>("a", 1, &log, true));  // throws: must not stop the others
    s.add(std::make_shared<StubLayer>("c", 5, &log));
    hb::video::Frame f; hb::video::prepareFrame(f, 4, 4);
    RenderContext ctx; ctx.width = 4; ctx.height = 4;
    s.renderOver(f, ctx);
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "c"}));  // z ascending, ties by insertion
    log.clear();
    s.add(std::make_shared<StubLayer>("b", 0, &log));  // same id replaces and keeps its slot, new z
    EXPECT_EQ(s.size(), 3u);
    s.renderOver(f, ctx);
    EXPECT_EQ(log, (std::vector<std::string>{"b", "a", "c"}));
    EXPECT_TRUE(s.remove("a"));
    EXPECT_FALSE(s.remove("a"));
    EXPECT_EQ(s.ids(), (std::vector<std::string>{"b", "c"}));
    EXPECT_TRUE(s.anyActive(ctx));
}

TEST(Overlay, NothingOnAirLeavesFrameUntouched) {
    Fixture fx;
    fx.add("timer"); fx.add("scoreboard"); fx.add("lowerthird");
    EXPECT_EQ(fx.draw(), 0);
}

TEST(Overlay, TimerFadesInRendersAndFadesOut) {
    Fixture fx;
    const auto id = fx.add("timer", [](ElementConfig& c) { c.durationSec = 90; c.animMs = 400; });
    ASSERT_FALSE(id.empty());
    fx.mgr->setOn(id, true);
    EXPECT_EQ(fx.draw(), 0);  // progress 0 at the very instant
    fx.t += 200;
    const int half = fx.draw();
    EXPECT_GT(half, 100);
    fx.t += 300;
    const int full = fx.draw();
    EXPECT_EQ(full, half);  // same card area (fade only changes alpha)
    // all changes are inside the top-right quadrant (pivot top-right)
    bool outside = false;
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            if ((x < 320 || y > 120) && std::memcmp(&fx.frame.data[(y * 640 + x) * 4], &fx.base.data[0], 4) != 0) outside = true;
    EXPECT_FALSE(outside);
    fx.mgr->setOn(id, false);
    fx.t += 200;
    EXPECT_GT(fx.draw(), 0);  // still fading out
    fx.t += 300;
    EXPECT_EQ(fx.draw(), 0);
}

TEST(Overlay, CardIsCachedAndRebuiltOnlyOnChange) {
    Fixture fx;
    const auto id = fx.add("timer");
    fx.mgr->setOn(id, true);
    fx.t += 1000;
    fx.draw();
    auto el = fx.mgr->element(id);
    const auto r0 = el->rasterCount();
    EXPECT_EQ(r0, 1u);
    for (int i = 0; i < 10; ++i) { fx.t += 30; fx.draw(); }  // 300 ms, same displayed second
    EXPECT_EQ(el->rasterCount(), r0);
    fx.mgr->timerCommand(id, "start", 0);
    fx.t += 1500;
    fx.draw();
    EXPECT_EQ(el->rasterCount(), r0 + 1);  // the second changed
    fx.t += 100;
    fx.draw();
    EXPECT_EQ(el->rasterCount(), r0 + 1);
}

TEST(Overlay, TimerCommandsThroughUiProtocolAndState) {
    Fixture fx;
    const auto id = fx.add("timer", [](ElementConfig& c) { c.durationSec = 10; });
    EXPECT_TRUE(fx.mgr->handleCommand("ovtimer|" + id + "|start"));
    fx.t += 3000;
    auto st = fx.mgr->stateJson();
    EXPECT_NE(st.find("\"clock\":\"00:07\""), std::string::npos) << st;
    EXPECT_NE(st.find("\"running\":true"), std::string::npos);
    fx.mgr->handleCommand("ovtimer|" + id + "|pause");
    fx.t += 5000;
    EXPECT_NE(fx.mgr->stateJson().find("\"clock\":\"00:07\""), std::string::npos);
    fx.mgr->handleCommand("ovtimer|" + id + "|set|120");
    EXPECT_NE(fx.mgr->stateJson().find("\"clock\":\"02:00\""), std::string::npos);
    fx.mgr->handleCommand("ovtimer|" + id + "|add|-30");
    EXPECT_NE(fx.mgr->stateJson().find("\"clock\":\"01:30\""), std::string::npos);
    fx.mgr->handleCommand("ovtimer|" + id + "|start");
    fx.t += 200000;
    EXPECT_NE(fx.mgr->stateJson().find("\"finished\":true"), std::string::npos);
    fx.mgr->handleCommand("ovtimer|" + id + "|reset");
    EXPECT_NE(fx.mgr->stateJson().find("\"clock\":\"02:00\""), std::string::npos);
    EXPECT_FALSE(fx.mgr->handleCommand("cut"));  // not an overlay command
    EXPECT_TRUE(fx.mgr->handleCommand("ovtimer|nonexistent|start"));  // recognised, ignored
    EXPECT_TRUE(fx.mgr->handleCommand("ovtimer"));                    // malformed: no crash
}

TEST(Overlay, BlinkAtZeroAlternatesAlpha) {
    Fixture fx;
    const auto id = fx.add("timer", [](ElementConfig& c) { c.durationSec = 1; c.animMs = 0; c.blinkAtZero = true; });
    fx.mgr->setOn(id, true);
    fx.mgr->timerCommand(id, "start", 0);
    fx.t += 2000;  // finished; (t/500)%2 decides the phase
    std::vector<int> sums;
    for (int k = 0; k < 4; ++k) {
        fx.draw();
        long s = 0;
        for (std::size_t i = 0; i < fx.frame.data.size(); i += 4) s += fx.frame.data[i + 2];
        sums.push_back(static_cast<int>(s / 1000));
        fx.t += 500;
    }
    EXPECT_NE(sums[0], sums[1]);
    EXPECT_EQ(sums[0], sums[2]);
}

TEST(Overlay, ScoreboardScoresAndPeriod) {
    Fixture fx;
    const auto id = fx.add("scoreboard", [](ElementConfig& c) { c.animMs = 0; c.showClock = true; c.durationSec = 600; c.period = "1T"; });
    fx.mgr->handleCommand("ovscore|" + id + "|a|1");
    fx.mgr->handleCommand("ovscore|" + id + "|a");  // default delta +1
    fx.mgr->handleCommand("ovscore|" + id + "|b|1");
    fx.mgr->handleCommand("ovscore|" + id + "|b|-5");  // clamped at 0
    auto cfg = fx.mgr->element(id)->config();
    EXPECT_EQ(cfg.scoreA, 2);
    EXPECT_EQ(cfg.scoreB, 0);
    fx.mgr->handleCommand("ovperiod|" + id + "|2º tempo | prorrogação");  // '|' inside the text is kept
    EXPECT_EQ(fx.mgr->element(id)->config().period, "2\xC2\xBA tempo | prorroga\xC3\xA7\xC3\xA3o");
    fx.mgr->setOn(id, true);
    EXPECT_GT(fx.draw(), 500);
    const int before = fx.draw();
    fx.mgr->handleCommand("ovscore|" + id + "|a|7");
    EXPECT_GT(fx.draw(), 0);
    (void)before;
    EXPECT_EQ(fx.mgr->element(id)->rasterCount(), 2u);  // rebuilt once for the score change
    fx.mgr->handleCommand("ovscore|" + id + "|swap");
    EXPECT_EQ(fx.mgr->element(id)->config().scoreB, 9);
    fx.mgr->handleCommand("ovscore|" + id + "|reset");
    EXPECT_EQ(fx.mgr->element(id)->config().scoreA, 0);
}

TEST(Overlay, QrElementDrawsScannableLookingCard) {
    Fixture fx;
    const auto id = fx.add("qr", [](ElementConfig& c) { c.animMs = 0; c.qrText = "https://example.com/doar"; c.qrCaption.clear(); c.qrSize = 0.4; });
    fx.mgr->setOn(id, true);
    EXPECT_GT(fx.draw(), 1000);
    int white = 0, black = 0;
    for (std::size_t i = 0; i < fx.frame.data.size(); i += 4) {
        if (fx.frame.data[i] == 255 && fx.frame.data[i + 1] == 255 && fx.frame.data[i + 2] == 255) ++white;
        if (fx.frame.data[i] == 0 && fx.frame.data[i + 1] == 0 && fx.frame.data[i + 2] == 0) ++black;
    }
    EXPECT_GT(white, 3000);
    EXPECT_GT(black, 800);
    EXPECT_NE(fx.mgr->stateJson().find("\"qrOk\":true"), std::string::npos);
    // empty / oversize payload: reported as not OK, draws nothing, no crash
    auto bad = fx.add("qr", [](ElementConfig& c) { c.animMs = 0; c.qrText.clear(); });
    EXPECT_NE(fx.mgr->stateJson().find("\"qrOk\":false"), std::string::npos);
    fx.mgr->setOn(id, false);
    fx.mgr->setOn(bad, true);
    EXPECT_EQ(fx.draw(), 0);
}

TEST(Overlay, LowerThirdSlidesInFromTheLeft) {
    Fixture fx;
    const auto id = fx.add("lowerthird", [](ElementConfig& c) { c.animMs = 1000; c.animIn = "slide-left"; });
    fx.mgr->setOn(id, true);
    fx.t += 100;
    fx.draw();
    int minX = 640;
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            if (std::memcmp(&fx.frame.data[(y * 640 + x) * 4], &fx.base.data[0], 4) != 0) minX = std::min(minX, x);
    EXPECT_LT(minX, 20);  // already entering from the very left edge
    fx.t += 1000;
    fx.draw();
    minX = 640;
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            if (std::memcmp(&fx.frame.data[(y * 640 + x) * 4], &fx.base.data[0], 4) != 0) minX = std::min(minX, x);
    EXPECT_NEAR(minX, static_cast<int>(0.04 * 640), 2);  // settled at x = 4%
}

TEST(Overlay, CaptionsFromBusWrapKeepTwoLinesAndFadeAfterSilence) {
    Fixture fx;
    const auto id = fx.add("captions", [](ElementConfig& c) { c.animMs = 0; c.holdSec = 3; c.size = 0.05; });
    fx.mgr->setOn(id, true);
    EXPECT_EQ(fx.draw(), 0);  // on-air but nothing said yet
    fx.bus.publish({"caption.text", {{"text", "ola mundo"}, {"final", "0"}, {"lang", "pt-BR"}}});
    const int partial = fx.draw();
    EXPECT_GT(partial, 200);
    fx.bus.publish({"caption.text", {{"text", "ola mundo"}, {"final", "1"}, {"lang", "pt-BR"}}});
    fx.bus.publish({"caption.text", {{"text", "esta e uma frase muito longa que precisa quebrar em varias linhas para caber na tela"}, {"final", "1"}, {"lang", "pt-BR"}}});
    const int many = fx.draw();
    EXPECT_GT(many, partial);
    // never more than 2 lines: card height is bounded (2 lines of ~18 px + padding)
    int minY = 360, maxY = 0;
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            if (std::memcmp(&fx.frame.data[(y * 640 + x) * 4], &fx.base.data[0], 4) != 0) { minY = std::min(minY, y); maxY = std::max(maxY, y); }
    EXPECT_LT(maxY - minY, 100);
    fx.t += 2900;
    EXPECT_GT(fx.draw(), 0);  // still inside the hold time
    fx.t += 500;
    const int fading = fx.draw();
    EXPECT_GT(fading, 0);     // fading out (hold 3 s + 0.6 s fade)
    fx.t += 1000;
    EXPECT_EQ(fx.draw(), 0);  // gone
    // speech resumes after silence: starts fresh, old text not repeated
    fx.bus.publish({"caption.text", {{"text", "novo"}, {"final", "0"}}});
    EXPECT_GT(fx.draw(), 0);
    EXPECT_LT(fx.diff(), many);
}

TEST(Overlay, CaptionsIgnoreMalformedEvents) {
    Fixture fx;
    const auto id = fx.add("captions", [](ElementConfig& c) { c.animMs = 0; });
    fx.mgr->setOn(id, true);
    fx.bus.publish({"caption.text", {}});                      // no text field
    fx.bus.publish({"caption.text", {{"text", "\x01\xFF\xFE"}}});  // garbage bytes
    fx.bus.publish({"caption.text", {{"text", std::string(100000, 'x')}}});  // huge
    fx.draw();  // must not crash
    SUCCEED();
}

TEST(Overlay, ChatPinAppearsThenAutoHides) {
    Fixture fx;
    const auto id = fx.add("chatpin", [](ElementConfig& c) { c.animMs = 100; c.holdSec = 5; });
    EXPECT_EQ(fx.draw(), 0);
    fx.bus.publish({"overlay.chatPin", {{"author", "Maria"}, {"text", "Parabens pela live!"}, {"platform", "youtube"}}});
    fx.t += 200;
    EXPECT_GT(fx.draw(), 300);  // pin turns the element on by itself
    EXPECT_TRUE(fx.mgr->element(id)->isOn(fx.t));
    fx.t += 4000;
    EXPECT_GT(fx.draw(), 300);
    fx.t += 1000;  // 5.2 s since the pin: hide timer fired, fading out
    fx.draw();
    EXPECT_FALSE(fx.mgr->element(id)->isOn(fx.t));
    fx.t += 300;
    EXPECT_EQ(fx.draw(), 0);
    fx.bus.publish({"overlay.chatPin", {{"author", "Joao"}, {"text", ""}}});  // empty text ignored
    EXPECT_EQ(fx.draw(), 0);
}

TEST(Overlay, PersistenceRoundTripAndHostileInput) {
    Fixture fx;
    auto a = fx.add("timer", [](ElementConfig& c) { c.name = "Meu timer"; c.durationSec = 777; c.textColor = 0x123456; });
    auto b = fx.add("lowerthird", [](ElementConfig& c) { c.title = "Jo\xC3\xA3o \"da\" Silva\\"; });
    fx.mgr->setPreviewOverlay(true);
    const std::string path = ::testing::TempDir() + "hb_overlays_test.json";
    ASSERT_TRUE(fx.mgr->save(path));
    OverlayManager other(nullptr, [] { return 0; });
    ASSERT_TRUE(other.load(path));
    EXPECT_TRUE(other.previewOverlay());
    ASSERT_EQ(other.ids().size(), 2u);
    EXPECT_EQ(other.element(a)->config().durationSec, 777);
    EXPECT_EQ(other.element(a)->config().textColor, 0x123456u);
    EXPECT_EQ(other.element(a)->config().name, "Meu timer");
    EXPECT_EQ(other.element(b)->config().title, "Jo\xC3\xA3o \"da\" Silva\\");
    EXPECT_FALSE(other.element(a)->isOn(0));  // on-air state never persists
    EXPECT_FALSE(other.takeDirty());          // loading is not a change to save
    std::remove(path.c_str());

    EXPECT_FALSE(other.load(path));                       // missing file
    EXPECT_FALSE(other.loadJsonText("{not json"));         // corrupt
    EXPECT_FALSE(other.loadJsonText("[1,2]"));
    EXPECT_TRUE(other.loadJsonText(R"({"elements":[{"type":"nope","id":"x"},{"id":"ok1","type":"timer","size":9999,"x":"bad","durationSec":-5,"bgAlpha":7,"animIn":"zoom","format":"x"},7,null]})"));
    auto ok = other.element("ok1");
    ASSERT_TRUE(ok);
    const auto c = ok->config();
    EXPECT_LE(c.size, 0.3);
    EXPECT_EQ(c.durationSec, 0);
    EXPECT_LE(c.bgAlpha, 1.0);
    EXPECT_EQ(c.animIn, "fade");
    EXPECT_EQ(c.format, "auto");
    EXPECT_EQ(other.element("x"), nullptr);
}

TEST(Overlay, UiSaveCommandCreatesUpdatesDeletesAndReorders) {
    Fixture fx;
    EXPECT_TRUE(fx.mgr->handleCommand(R"(ovsave|{"type":"timer","name":"A | B","durationSec":60})"));
    ASSERT_EQ(fx.mgr->ids().size(), 1u);
    const auto id = fx.mgr->ids()[0];
    EXPECT_EQ(fx.mgr->element(id)->config().name, "A | B");
    EXPECT_TRUE(fx.mgr->takeDirty());
    EXPECT_TRUE(fx.mgr->handleCommand("ovsave|{\"id\":\"" + id + "\",\"type\":\"timer\",\"name\":\"Novo\",\"durationSec\":90}"));
    EXPECT_EQ(fx.mgr->ids().size(), 1u);
    EXPECT_EQ(fx.mgr->element(id)->config().name, "Novo");
    EXPECT_EQ(fx.mgr->element(id)->config().durationSec, 90);
    // changing the type of an existing id is ignored (type is identity)
    fx.mgr->handleCommand("ovsave|{\"id\":\"" + id + "\",\"type\":\"qr\",\"name\":\"X\"}");
    EXPECT_EQ(fx.mgr->element(id)->config().type, "timer");
    fx.mgr->handleCommand("ovnew|lowerthird");
    fx.mgr->handleCommand("ovnew|bogus");
    ASSERT_EQ(fx.mgr->ids().size(), 2u);
    const auto second = fx.mgr->ids()[1];
    fx.mgr->handleCommand("ovmove|" + second + "|down");
    EXPECT_EQ(fx.mgr->ids()[0], second);
    fx.mgr->handleCommand("ovmove|" + second + "|up");
    EXPECT_EQ(fx.mgr->ids()[1], second);
    fx.mgr->handleCommand("ovsave|{broken");
    fx.mgr->handleCommand("ovsave");
    fx.mgr->handleCommand("ovdel|" + id);
    EXPECT_EQ(fx.mgr->ids().size(), 1u);
    fx.mgr->handleCommand("ovpreview|1");
    EXPECT_TRUE(fx.mgr->previewOverlay());
    for (int i = 0; i < 40; ++i) fx.mgr->handleCommand("ovnew|timer");
    EXPECT_LE(fx.mgr->ids().size(), OverlayManager::kMaxElements);
}

TEST(Overlay, PreviewOptionAndHookLifetime) {
    Fixture fx;
    const auto id = fx.add("lowerthird", [](ElementConfig& c) { c.animMs = 0; });
    fx.mgr->setOn(id, true);
    fx.frame = fx.base;
    fx.mgr->renderPreview(fx.frame);
    EXPECT_EQ(fx.diff(), 0);  // option off: preview stays clean
    fx.mgr->setPreviewOverlay(true);
    fx.mgr->renderPreview(fx.frame);
    EXPECT_GT(fx.diff(), 0);
    auto hook = fx.mgr->programHook();
    fx.mgr.reset();  // manager gone: the hook (held by the engine) must stay safe
    fx.frame = fx.base;
    hook(fx.frame, 1);
    EXPECT_GT(fx.diff(), 0);
}

TEST(Overlay, TinyAndOddCanvasSizesDoNotCrash) {
    Fixture fx;
    for (auto type : {"timer", "scoreboard", "qr", "lowerthird", "captions", "chatpin"}) {
        auto id = fx.add(type, [](ElementConfig& c) { c.animMs = 0; c.qrText = "abc"; });
        fx.mgr->setOn(id, true);
    }
    fx.bus.publish({"caption.text", {{"text", "teste"}}});
    fx.bus.publish({"overlay.chatPin", {{"author", "a"}, {"text", "b"}}});
    for (auto sz : {std::pair{1, 1}, std::pair{7, 3}, std::pair{64, 36}, std::pair{4000, 100}}) {
        hb::video::Frame f; hb::video::prepareFrame(f, sz.first, sz.second);
        fx.mgr->programHook()(f, 0);
    }
    hb::video::Frame none;
    fx.mgr->programHook()(none, 0);
    SUCCEED();
}
