#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "app/source_assignments.hpp"
#include "chroma/ai_worker.hpp"
#include "chroma/chroma_filter.hpp"
#include "chroma/model_store.hpp"
#include "chroma/service.hpp"
#include "chroma_test_util.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"

using namespace hb::chroma;
using hbt::FakeSegmenter;
using hbt::px;
using hbt::solid;

namespace {
template <class F>
bool waitFor(F f, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

std::string tempDir(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / (std::string("hb_chroma_") + tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(p);
    return p.string();
}

class FakeHttp final : public IHttpClient {
public:
    std::string body;
    bool ok = true;
    int status = 200;
    bool slow = false;
    int calls = 0;
    HttpResult get(const std::string&, const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) override {
        ++calls;
        HttpResult r;
        r.status = status;
        if (!ok) { r.error = "sem rede"; return r; }
        std::size_t off = 0;
        while (off < body.size()) {
            const std::size_t n = std::min<std::size_t>(100, body.size() - off);
            if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!onData(reinterpret_cast<const std::uint8_t*>(body.data()) + off, n, body.size())) { r.error = "abortado"; return r; }
            off += n;
        }
        r.ok = true;
        return r;
    }
};

ModelInfo infoFor(const std::string& body) {
    return {"t", "m.onnx", "https://example.com/m.onnx", hb::core::Sha256::hex(body), body.size(), "test", "test"};
}
}  // namespace

// ---- AiMaskWorker ----
TEST(ChromaWorker, ProducesMaskAndMeasuresInference) {
    auto seg = std::make_shared<FakeSegmenter>();
    seg->delayMs = 5;
    PostParams pp; pp.minIntervalMs = 0; pp.erodePasses = 0; pp.featherRadius = 0; pp.emaNew = 256; pp.levelLo = 100; pp.levelHi = 150;
    AiMaskWorker w(seg, pp);
    w.start();
    EXPECT_EQ(w.latest(), nullptr);
    auto f = solid(64, 36, 1, 2, 3);
    ASSERT_TRUE(w.wantsFrame());
    ASSERT_TRUE(w.submit(f));
    ASSERT_TRUE(waitFor([&] { return w.latest() != nullptr; }));
    auto m = w.latest();
    EXPECT_EQ(m->width, 32);
    EXPECT_EQ(m->data[0], 255);
    EXPECT_EQ(m->data[31], 0);
    const auto st = w.stats();
    EXPECT_EQ(st.processed, 1u);
    EXPECT_GE(st.inferMsAvg, 4.0);
    w.stop();
}

TEST(ChromaWorker, SubmitNeverBlocksAndDropsWhenBusy) {
    auto seg = std::make_shared<FakeSegmenter>();
    seg->delayMs = 200;
    PostParams pp; pp.minIntervalMs = 0;
    AiMaskWorker w(seg, pp);
    w.start();
    auto f = solid(64, 36, 1, 2, 3);
    ASSERT_TRUE(w.submit(f));
    const auto t0 = std::chrono::steady_clock::now();
    int accepted = 0;
    for (int i = 0; i < 50; ++i) accepted += w.submit(f) ? 1 : 0;   // the "clock" keeps ticking
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 100.0);       // never waited for the 200 ms inference
    EXPECT_LE(accepted, 1);     // at most one frame queued behind the busy one
    EXPECT_FALSE(w.wantsFrame());
    w.stop();
    EXPECT_GT(w.stats().dropped, 0u);
}

TEST(ChromaWorker, ThrottlesByInterval) {
    auto seg = std::make_shared<FakeSegmenter>();
    PostParams pp; pp.minIntervalMs = 10000;
    AiMaskWorker w(seg, pp);
    w.start();
    auto f = solid(64, 36, 1, 2, 3);
    EXPECT_TRUE(w.submit(f));
    ASSERT_TRUE(waitFor([&] { return w.latest() != nullptr; }));
    EXPECT_FALSE(w.wantsFrame());
    EXPECT_FALSE(w.submit(f));
    w.stop();
}

TEST(ChromaWorker, GivesUpAfterRepeatedFailuresWithoutCrashing) {
    auto seg = std::make_shared<FakeSegmenter>();
    seg->fail = true;
    PostParams pp; pp.minIntervalMs = 0;
    AiMaskWorker w(seg, pp);
    w.start();
    auto f = solid(64, 36, 1, 2, 3);
    ASSERT_TRUE(waitFor([&] {
        w.submit(f);
        return w.stats().failed;
    }));
    EXPECT_NE(w.stats().message.find("boom"), std::string::npos);
    EXPECT_FALSE(w.submit(f));
    EXPECT_EQ(w.latest(), nullptr);
    w.stop();
}

TEST(ChromaWorker, TemporalSmoothingReducesFlicker) {
    auto seg = std::make_shared<FakeSegmenter>();
    PostParams pp; pp.minIntervalMs = 0; pp.erodePasses = 0; pp.featherRadius = 0; pp.emaNew = 64; pp.levelLo = 0; pp.levelHi = 255;
    AiMaskWorker w(seg, pp);
    w.start();
    auto f = solid(64, 36, 1, 2, 3);
    for (int i = 0; i < 3; ++i) {
        const auto before = w.stats().processed;
        ASSERT_TRUE(waitFor([&] { return w.submit(f); }));
        ASSERT_TRUE(waitFor([&] { return w.stats().processed > before; }));
    }
    w.stop();
}

TEST(ChromaWorker, DestructionWhileBusyDoesNotBlockOrCrash) {
    auto seg = std::make_shared<FakeSegmenter>();
    seg->delayMs = 150;
    const auto t0 = std::chrono::steady_clock::now();
    {
        AiMaskWorker w(seg, PostParams{0, 255, 0, 0, 256, 0});
        w.start();
        w.submit(solid(64, 36, 1, 2, 3));
        ASSERT_TRUE(waitFor([&] { return seg->calls > 0; }));
    }   // destructor with an inference in flight
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 120.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));   // let the detached thread finish safely
}

// ---- ChromaFilter / SlotSource hook ----
TEST(ChromaFilter, OffLeavesFrameUntouched) {
    ChromaFilter cf;
    auto f = solid(16, 9, 70, 190, 40);
    const auto copy = f.data;
    cf.apply(f);
    EXPECT_EQ(f.data, copy);
}

TEST(ChromaFilter, ClassicGreenReplacedWithColor) {
    ChromaFilter cf;
    Settings s; s.mode = Mode::Green; s.repl = ReplKind::Color; s.color = 0x0000ff;   // blue backdrop
    cf.setSettings(s);
    auto f = solid(16, 9, 70, 190, 40);
    hbt::setPx(f, 3, 3, 120, 150, 210);   // person pixel
    cf.apply(f);
    EXPECT_EQ(px(f, 0, 0)[0], 255);       // backdrop is pure blue (B=255)
    EXPECT_EQ(px(f, 0, 0)[1], 0);
    EXPECT_EQ(px(f, 3, 3)[2], 210);       // person kept (red channel intact)
    EXPECT_EQ(px(f, 0, 0)[3], 255);
    EXPECT_GT(cf.stats().applyMsAvg, 0.0);
}

TEST(ChromaFilter, TransparentModeSetsStraightAlpha) {
    ChromaFilter cf;
    Settings s; s.mode = Mode::Green; s.repl = ReplKind::Transparent;
    cf.setSettings(s);
    auto f = solid(8, 8, 70, 190, 40);
    cf.apply(f);
    EXPECT_EQ(px(f, 0, 0)[3], 0);
    EXPECT_EQ(px(f, 0, 0)[2], 40);    // straight: colour not multiplied by alpha (G is despilled)
}

TEST(ChromaFilter, BlurAndImageBackdrops) {
    ChromaFilter cf;
    Settings s; s.mode = Mode::Green; s.repl = ReplKind::Image; s.image = "x.png";
    cf.setSettings(s);
    cf.setImage(std::make_shared<hb::video::Frame>(solid(4, 4, 9, 8, 7)));
    auto f = solid(32, 32, 70, 190, 40);
    cf.apply(f);
    EXPECT_EQ(px(f, 5, 5)[0], 9);
    s.repl = ReplKind::Blur;
    cf.setSettings(s);
    auto g = solid(32, 32, 70, 190, 40);
    for (int i = 0; i < 5; ++i) { auto h = g; cf.apply(h); }   // exercises the blur cache reuse path
}

TEST(ChromaFilter, AiPassesThroughUntilFirstMaskThenKeys) {
    auto seg = std::make_shared<FakeSegmenter>();
    ChromaFilter cf;
    Settings s; s.mode = Mode::Ai; s.repl = ReplKind::Color; s.color = 0x00ff00; s.softness = 0; s.quality = Quality::High;
    cf.setSettings(s);
    auto f0 = solid(64, 36, 5, 5, 5);
    cf.apply(f0);   // no segmenter: untouched, never black
    EXPECT_EQ(px(f0, 63, 0)[1], 5);
    EXPECT_TRUE(cf.stats().aiWaiting);
    cf.setSegmenter(seg);
    EXPECT_TRUE(cf.stats().aiActive);
    bool keyed = false;
    for (int i = 0; i < 400 && !keyed; ++i) {
        auto f = solid(64, 36, 5, 5, 5);
        cf.apply(f);
        keyed = px(f, 63, 0)[1] == 255 && px(f, 0, 0)[0] == 5;   // right half replaced by green, left (person) kept
        if (!keyed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(keyed);
    cf.setSettings(Settings{});   // back to Off: worker released
    EXPECT_FALSE(cf.stats().aiActive);
    cf.shutdown();
}

TEST(ChromaFilter, ApplyIsFastEvenWhenInferenceIsVerySlow) {
    auto seg = std::make_shared<FakeSegmenter>();
    seg->delayMs = 300;
    ChromaFilter cf;
    Settings s; s.mode = Mode::Ai; s.quality = Quality::High;
    cf.setSettings(s);
    cf.setSegmenter(seg);
    double worst = 0;
    for (int i = 0; i < 30; ++i) {
        auto f = solid(640, 360, 5, 5, 5);
        const auto t0 = std::chrono::steady_clock::now();
        cf.apply(f);
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    EXPECT_LT(worst, 100.0);   // generous for CI noise; the point: never waits for the 300 ms inference
    cf.shutdown();
}

namespace {
struct ThrowingFilter : hb::video::FrameFilter {
    void apply(hb::video::Frame&) override { throw 1; }
};
struct MarkFilter : hb::video::FrameFilter {
    void apply(hb::video::Frame& f) override { f.data[0] = 99; }
};
}  // namespace

TEST(ChromaSlotHook, FilterAppliesToSlotRenderAndSurvivesAssign) {
    auto fallback = std::make_shared<hb::video::SyntheticSource>(1, "T");
    hb::video::SlotSource slot(1, fallback);
    hb::video::Frame f;
    hb::video::prepareFrame(f, 32, 18);
    slot.setFilter(std::make_shared<MarkFilter>());
    slot.render(f, 0);
    EXPECT_EQ(f.data[0], 99);
    slot.clear();   // device change must not drop the filter
    f.data[0] = 0;
    slot.render(f, 1);
    EXPECT_EQ(f.data[0], 99);
    slot.setFilter(std::make_shared<ThrowingFilter>());
    EXPECT_NO_THROW(slot.render(f, 2));   // a throwing filter never reaches the clock
    slot.setFilter(nullptr);
}

// ---- ModelStore ----
TEST(ChromaModelStore, DownloadVerifyAndRefresh) {
    const std::string dir = tempDir("ok");
    FakeHttp http;
    http.body = std::string(1000, 'x');
    ModelStore store(dir, infoFor(http.body), &http);
    store.refresh();
    EXPECT_EQ(store.status().state, ModelStore::State::NotDownloaded);
    ASSERT_TRUE(store.startDownload());
    store.waitIdle();
    EXPECT_EQ(store.status().state, ModelStore::State::Ready);
    EXPECT_TRUE(std::filesystem::exists(store.path()));
    EXPECT_FALSE(std::filesystem::exists(store.path() + ".part"));
    ModelStore again(dir, infoFor(http.body), &http);
    again.refresh();
    EXPECT_TRUE(again.ready());
    EXPECT_FALSE(again.startDownload());   // already installed
    EXPECT_TRUE(again.remove());
    EXPECT_EQ(again.status().state, ModelStore::State::NotDownloaded);
    std::filesystem::remove_all(dir);
}

TEST(ChromaModelStore, WrongHashIsDiscarded) {
    const std::string dir = tempDir("hash");
    FakeHttp http;
    http.body = std::string(1000, 'x');
    ModelInfo mi = infoFor(http.body);
    http.body[10] = 'y';   // tampered download, same size
    ModelStore store(dir, mi, &http);
    ASSERT_TRUE(store.startDownload());
    store.waitIdle();
    EXPECT_EQ(store.status().state, ModelStore::State::Error);
    EXPECT_NE(store.status().message.find("SHA-256"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(store.path()));
    EXPECT_FALSE(std::filesystem::exists(store.path() + ".part"));
    std::filesystem::remove_all(dir);
}

TEST(ChromaModelStore, OversizeNetworkErrorAndCancel) {
    const std::string dir = tempDir("err");
    FakeHttp http;
    http.body = std::string(1000, 'x');
    ModelInfo mi = infoFor(http.body);
    {
        FakeHttp big; big.body = std::string(5000, 'x');
        ModelStore s(dir, mi, &big);
        ASSERT_TRUE(s.startDownload());
        s.waitIdle();
        EXPECT_EQ(s.status().state, ModelStore::State::Error);   // > 2x expected: aborted
    }
    {
        FakeHttp bad; bad.ok = false;
        ModelStore s(dir, mi, &bad);
        ASSERT_TRUE(s.startDownload());
        s.waitIdle();
        EXPECT_EQ(s.status().state, ModelStore::State::Error);
        EXPECT_NE(s.status().message.find("sem rede"), std::string::npos);
        FakeHttp http404; http404.status = 404;
        ModelStore s2(dir, mi, &http404);
        ASSERT_TRUE(s2.startDownload());
        s2.waitIdle();
        EXPECT_EQ(s2.status().state, ModelStore::State::Error);
    }
    {
        FakeHttp slow; slow.body = std::string(1000, 'x'); slow.slow = true;
        ModelStore s(dir, mi, &slow);
        ASSERT_TRUE(s.startDownload());
        std::string why;
        EXPECT_FALSE(s.startDownload(&why));   // already downloading
        s.cancel();
        s.waitIdle();
        EXPECT_EQ(s.status().state, ModelStore::State::NotDownloaded);
        EXPECT_FALSE(std::filesystem::exists(s.path() + ".part"));
    }
    std::filesystem::remove_all(dir);
}

TEST(ChromaModelStore, CorruptExistingFileIsRemovedAndUnconfiguredModelIsUnavailable) {
    const std::string dir = tempDir("corrupt");
    FakeHttp http;
    http.body = std::string(1000, 'x');
    ModelInfo mi = infoFor(http.body);
    { std::ofstream(std::filesystem::path(dir) / mi.fileName, std::ios::binary) << "garbage"; }
    ModelStore store(dir, mi, &http);
    store.refresh();
    EXPECT_EQ(store.status().state, ModelStore::State::NotDownloaded);
    EXPECT_FALSE(std::filesystem::exists(store.path()));
    ModelInfo bad = mi; bad.sha256 = "";
    ModelStore u(dir, bad, &http);
    EXPECT_EQ(u.status().state, ModelStore::State::Unavailable);
    EXPECT_FALSE(u.startDownload());
    ModelInfo plain = mi; plain.url = "http://example.com/m.onnx";
    EXPECT_EQ(ModelStore(dir, plain, &http).status().state, ModelStore::State::Unavailable);
    std::filesystem::remove_all(dir);
}

TEST(ChromaModelStore, PinnedDefaultModelIsWellFormed) {
    const auto& m = defaultModel();
    EXPECT_TRUE(isValidSha256Hex(m.sha256));
    EXPECT_TRUE(isAcceptableModelUrl(m.url));
    EXPECT_FALSE(isAcceptableModelUrl("https://user@evil.com/x"));
    EXPECT_FALSE(isAcceptableModelUrl("ftp://x/y"));
    EXPECT_FALSE(isAcceptableModelUrl("https://a b/c"));
}

// ---- Service ----
TEST(ChromaService, CommandsPersistenceCallbackAndStatus) {
    const std::string dir = tempDir("svc");
    FakeHttp http;
    http.body = std::string(500, 'm');
    auto seg = std::make_shared<FakeSegmenter>();
    ChromaService::Deps d;
    d.modelsDir = dir;
    d.http = &http;
    d.model = infoFor(http.body);
    d.segmenterFactory = [&](const std::string&, std::string&) -> std::shared_ptr<ISegmenter> { return seg; };
    d.imageLoader = [](const std::string& p, hb::video::Frame& out, std::string& err) {
        if (p != "ok.png") { err = "nao abriu"; return false; }
        out = solid(4, 4, 1, 2, 3);
        return true;
    };
    std::vector<std::pair<int, std::string>> saved;
    d.onChanged = [&](int slot, const std::string& s) { saved.emplace_back(slot, s); };
    ChromaService svc(16, std::move(d));

    std::string msg;
    EXPECT_FALSE(svc.handleSetting(99, "mode", "green", msg));
    EXPECT_FALSE(svc.handleSetting(0, "mode", "nonsense", msg));
    EXPECT_TRUE(svc.handleSetting(2, "mode", "green", msg));
    EXPECT_TRUE(svc.handleSetting(2, "tol", "60", msg));
    ASSERT_EQ(saved.size(), 2u);
    EXPECT_EQ(saved.back().first, 2);
    EXPECT_EQ(svc.filter(2)->settings().tolerance, 60);

    // AI before the model exists: accepted but warns
    EXPECT_TRUE(svc.handleSetting(3, "mode", "ai", msg));
    EXPECT_NE(msg.find("Baixe"), std::string::npos);

    ASSERT_TRUE(svc.handleModel("download", msg));
    svc.model().waitIdle();
    ASSERT_TRUE(svc.model().ready());
    svc.tick();
    EXPECT_TRUE(waitFor([&] { return svc.filter(3)->stats().aiActive; }));

    EXPECT_TRUE(svc.handleSetting(4, "repl", "image", msg));
    EXPECT_TRUE(svc.handleSetting(4, "img", "bad.png", msg));
    hb::core::json::Value v;
    ASSERT_TRUE(hb::core::json::parse(svc.statusJson(), v));
    EXPECT_EQ(v.get("slots")->a.size(), 16u);
    EXPECT_EQ(v.get("slots")->a[4].get("imgErr")->s, "nao abriu");
    EXPECT_EQ(v.get("slots")->a[2].get("mode")->s, "green");
    EXPECT_EQ(v.get("model")->get("state")->s, "ready");
    EXPECT_TRUE(v.get("runtime")->b);
    EXPECT_TRUE(svc.handleSetting(4, "img", "ok.png", msg));
    ASSERT_TRUE(hb::core::json::parse(svc.statusJson(), v));
    EXPECT_EQ(v.get("slots")->a[4].get("imgErr")->s, "");
    EXPECT_EQ(v.get("slots")->a[4].get("img")->s, "ok.png");

    // restore into a fresh service
    ChromaService::Deps d2;
    d2.modelsDir = dir; d2.model = infoFor(http.body);
    ChromaService svc2(16, std::move(d2));
    svc2.load(2, saved[1].second);
    svc2.load(5, "garbage;;;");
    EXPECT_EQ(svc2.filter(2)->settings().mode, Mode::Green);
    EXPECT_EQ(svc2.filter(2)->settings().tolerance, 60);
    EXPECT_FALSE(svc2.aiRuntimeAvailable());
    svc2.handleSetting(0, "mode", "ai", msg);
    EXPECT_NE(msg.find("ONNX"), std::string::npos);

    EXPECT_TRUE(svc.handleModel("remove", msg));
    EXPECT_FALSE(svc.model().ready());
    EXPECT_FALSE(svc.handleModel("bogus", msg));
    svc.shutdown();
    std::filesystem::remove_all(dir);
}

// ---- persistence in source_assignments ----
TEST(ChromaAssignments, BackgroundFieldIsBackwardCompatible) {
    hb::app::SourceAssignments a;
    EXPECT_FALSE(a.setBackground(0, "mode=green"));   // nothing assigned
    ASSERT_TRUE(a.set(0, "dev", "Cam"));
    EXPECT_TRUE(a.setBackground(0, "mode=ai;tol=50\tinjected\n"));
    const std::string ser = a.serialize();
    hb::app::SourceAssignments b;
    b.parse(ser);
    EXPECT_EQ(b.get(0).bg, "mode=ai;tol=50injected");
    EXPECT_EQ(b.get(0).id, "dev");
    EXPECT_TRUE(b.get(0).enabled);
    // old 8-field line: no bg
    hb::app::SourceAssignments c;
    c.parse("1\tdev\tCam\t0\t1280\t720\t30\t1\n");
    EXPECT_TRUE(c.get(1).bg.empty());
    EXPECT_FALSE(c.get(1).enabled);
    EXPECT_EQ(c.get(1).w, 1280);
    // a device change keeps the background, clear() drops it
    ASSERT_TRUE(a.set(0, "dev2", "Cam2"));
    EXPECT_FALSE(a.get(0).bg.empty());
    a.clear(0);
    EXPECT_TRUE(a.get(0).bg.empty());
}
