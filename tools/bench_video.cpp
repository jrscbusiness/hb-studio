// hb_bench: per-frame cost of the video hot paths (portable, no GPU, no Windows).
//   cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DHB_BUILD_BENCH=ON && cmake --build build-bench --target hb_bench
//   build-bench/hb_bench [--quick] [--filter text]
// Output: one line per case with the median and the best time per frame (ms) and the frames/s the
// median allows on ONE core. Not part of ctest (timings are machine dependent).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "app/engine.hpp"
#include "overlay/blend.hpp"
#include "scene/compositor.hpp"
#include "switcher/switcher.hpp"
#include "video/frame_ops.hpp"
#include "video/pixel_convert.hpp"
#include "video/program_output.hpp"

using namespace hb;
using Clock = std::chrono::steady_clock;

namespace {

bool g_quick = false;
std::string g_filter;

struct Res { const char* name; int w, h; };
const Res kRes[] = {{"720p", 1280, 720}, {"1080p", 1920, 1080}, {"4K", 3840, 2160}};

std::uint32_t g_rng = 0x12345678u;
std::uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// Camera-like content: smooth gradients plus noise, opaque alpha.
void fillNoisy(video::Frame& f, int w, int h) {
    video::prepareFrame(f, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = f.data.data() + static_cast<std::size_t>(y) * f.stride + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 255 / w + (rnd() & 7));
            p[1] = static_cast<std::uint8_t>(y * 255 / h + (rnd() & 7));
            p[2] = static_cast<std::uint8_t>((x + y) & 255);
            p[3] = 255;
        }
}

void run(const std::string& name, const std::function<void()>& fn) {
    if (!g_filter.empty() && name.find(g_filter) == std::string::npos) return;
    for (int i = 0; i < 2; ++i) fn();  // warm-up (allocations, caches)
    std::vector<double> t;
    const double budgetMs = g_quick ? 60.0 : 400.0;
    const auto start = Clock::now();
    while (t.size() < 5 || (std::chrono::duration<double, std::milli>(Clock::now() - start).count() < budgetMs && t.size() < 400)) {
        const auto a = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    const double med = t[t.size() / 2];
    std::printf("%-46s median %9.3f ms  best %9.3f ms  (%8.1f fps/core)\n", name.c_str(), med, t.front(), 1000.0 / med);
    std::fflush(stdout);
}

class NoiseSource final : public video::Source {
public:
    explicit NoiseSource(int seed) : name_("n" + std::to_string(seed)) {}
    const std::string& name() const override { return name_; }
    bool isTestSource() const override { return true; }
    void render(video::Frame& f, std::uint64_t i) const override {
        // Cheap deterministic pattern (a real source copies a captured frame: this is about a memcpy).
        const std::size_t n = f.data.size();
        std::uint8_t* d = f.data.data();
        const std::uint8_t v = static_cast<std::uint8_t>(i);
        for (std::size_t k = 0; k < n; ++k) d[k] = static_cast<std::uint8_t>(k + v);
    }
private:
    std::string name_;
};

void benchFrameOps() {
    for (const Res& r : kRes) {
        video::Frame srcSame;
        fillNoisy(srcSame, r.w, r.h);
        // fitFrame: 1:1, 4:3 pillarbox, upscale from 720p, downscale from 4K, into a r.w x r.h target.
        video::Frame dst;
        video::prepareFrame(dst, r.w, r.h);
        run(std::string("fitFrame 1:1 -> ") + r.name, [&] { video::fitFrame(srcSame, dst); });
        video::Frame s43;
        fillNoisy(s43, r.h * 4 / 3, r.h);
        run(std::string("fitFrame 4:3 pillarbox -> ") + r.name, [&] { video::fitFrame(s43, dst); });
        if (r.h != 720) {
            video::Frame s720;
            fillNoisy(s720, 1280, 720);
            run(std::string("fitFrame 720p upscale -> ") + r.name, [&] { video::fitFrame(s720, dst); });
        }
        if (r.h != 2160) {
            video::Frame s4k;
            fillNoisy(s4k, 3840, 2160);
            run(std::string("fitFrame 4K downscale -> ") + r.name, [&] { video::fitFrame(s4k, dst); });
        }

        // downscaleBox: the UI preview path (program -> monitor) and the tile path (source -> tile).
        video::Frame small;
        run(std::string("downscaleBox ") + r.name + " -> 640x360", [&] { video::downscaleBox(srcSame, small, 640, 360); });
        run(std::string("downscaleBox ") + r.name + " -> 960x540", [&] { video::downscaleBox(srcSame, small, 960, 540); });
        run(std::string("downscaleBox ") + r.name + " -> 192x108", [&] { video::downscaleBox(srcSame, small, 192, 108); });
        run(std::string("downscaleBox ") + r.name + " -> 320x180", [&] { video::downscaleBox(srcSame, small, 320, 180); });

        // mixFrames: DISSOLVE.
        video::Frame a, b, out;
        fillNoisy(a, r.w, r.h);
        fillNoisy(b, r.w, r.h);
        run(std::string("mixFrames dissolve ") + r.name, [&] { video::mixFrames(a, b, out, 100); });
    }
}

void benchConvert() {
    for (const Res& r : kRes) {
        std::vector<std::uint8_t> yuy2(static_cast<std::size_t>(r.w) * 2 * r.h), nv(static_cast<std::size_t>(r.w) * r.h * 3 / 2);
        for (auto& v : yuy2) v = static_cast<std::uint8_t>(rnd());
        for (auto& v : nv) v = static_cast<std::uint8_t>(rnd());
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(r.w) * r.h * 4);
        run(std::string("YUY2->BGRA ") + r.name, [&] { video::convertYuy2ToBgra(yuy2.data(), r.w, r.h, r.w * 2, dst.data()); });
        run(std::string("UYVY->BGRA ") + r.name, [&] { video::convertUyvyToBgra(yuy2.data(), r.w, r.h, r.w * 2, dst.data()); });
        run(std::string("NV12->BGRA ") + r.name, [&] {
            video::convertNv12ToBgra(nv.data(), r.w, nv.data() + static_cast<std::size_t>(r.w) * r.h, r.w, r.w, r.h, dst.data());
        });
    }
}

void benchOverlay() {
    for (const Res& r : kRes) {
        video::Frame f;
        fillNoisy(f, r.w, r.h);
        // Lower third: ~60% of the width, 15% of the height, mixed transparent / opaque / translucent pixels.
        overlay::Bitmap lt;
        lt.resize(r.w * 6 / 10, r.h * 15 / 100);
        for (std::size_t i = 0; i + 3 < lt.bgra.size(); i += 4) {
            lt.bgra[i] = static_cast<std::uint8_t>(rnd()); lt.bgra[i + 1] = static_cast<std::uint8_t>(rnd());
            lt.bgra[i + 2] = static_cast<std::uint8_t>(rnd());
            const std::uint32_t k = rnd() & 7;
            lt.bgra[i + 3] = k == 0 ? 0 : k < 3 ? 255 : 150;
        }
        run(std::string("overlay blendBitmap lower-third ") + r.name, [&] { overlay::blendBitmap(f, lt, r.w / 5, r.h * 75 / 100); });
        run(std::string("overlay blendBitmap lower-third op=.6 ") + r.name, [&] { overlay::blendBitmap(f, lt, r.w / 5, r.h * 75 / 100, 0.6); });
        overlay::Bitmap full;
        full.resize(r.w, r.h);
        for (std::size_t i = 0; i + 3 < full.bgra.size(); i += 4) {
            full.bgra[i] = static_cast<std::uint8_t>(rnd()); full.bgra[i + 1] = static_cast<std::uint8_t>(rnd());
            full.bgra[i + 2] = static_cast<std::uint8_t>(rnd()); full.bgra[i + 3] = 160;
        }
        run(std::string("overlay blendBitmap full-frame ") + r.name, [&] { overlay::blendBitmap(f, full, 0, 0); });
        run(std::string("overlay fillRect translucent full ") + r.name, [&] { overlay::fillRect(f, 0, 0, r.w, r.h, 0x203040, 120); });
    }
}

void benchScene() {
    for (const Res& r : kRes) {
        scene::Scene sc;
        sc.bg.type = scene::Background::Type::Gradient;
        auto mk = [](int slot, double x, double y, double w, double h, double radius, bool border, bool shadow, scene::FitMode fit) {
            scene::Layer l;
            l.slot = slot; l.rect = {x, y, w, h}; l.fit = fit; l.radius = radius;
            if (border) l.border.width = 0.006;
            if (shadow) { l.shadow.blur = 0.02; l.shadow.opacity = 0.5; }
            return l;
        };
        sc.layers = {mk(0, 0.03, 0.05, 0.62, 0.9, 0.04, true, true, scene::FitMode::Fill),
                     mk(1, 0.68, 0.05, 0.29, 0.28, 0.08, true, true, scene::FitMode::Crop),
                     mk(2, 0.68, 0.38, 0.29, 0.28, 0.0, false, false, scene::FitMode::Fit)};
        std::vector<double> aspects(3, 16.0 / 9);
        auto cs = scene::compileScene(sc, r.w, r.h, aspects);
        video::Frame out;
        video::prepareFrame(out, r.w, r.h);
        auto render = [&](const scene::Layer&, video::Frame& f, std::uint64_t i) {
            const std::uint8_t v = static_cast<std::uint8_t>(i);
            for (std::size_t k = 0; k < f.data.size(); ++k) f.data[k] = static_cast<std::uint8_t>(k * 7 + v);
            for (std::size_t k = 3; k < f.data.size(); k += 4) f.data[k] = 255;
            return true;
        };
        std::uint64_t idx = 0;
        run(std::string("scene composeScene 3 layers ") + r.name, [&] { scene::composeScene(*cs, sc, render, out, idx++); });
        // Source cost removed: what is left is background copy + shadows + masked blends + rings.
        auto renderNop = [&](const scene::Layer&, video::Frame&, std::uint64_t) { return true; };
        run(std::string("scene composeScene blend-only ") + r.name, [&] { scene::composeScene(*cs, sc, renderNop, out, idx++); });
    }
}

void benchSwitcherAndProgram() {
    for (const Res& r : kRes) {
        std::vector<std::shared_ptr<const video::Source>> srcs = {std::make_shared<NoiseSource>(1), std::make_shared<NoiseSource>(2)};
        switcher::Switcher sw(srcs);
        video::Frame out;
        video::prepareFrame(out, r.w, r.h);
        std::uint64_t idx = 0;
        run(std::string("switcher renderProgram cut ") + r.name, [&] { sw.renderProgram(out, idx++); });
        sw.setPreview(1);
        sw.setTBarPosition(0.5f);
        run(std::string("switcher renderProgram dissolve ") + r.name, [&] { sw.renderProgram(out, idx++); });
    }
    // ProgramOutput: the real clock thread at 30 and 60 fps; consumer timestamps give the real fps and the worst jitter.
    for (const Res& r : kRes) {
        for (int fps : {30, 60}) {
            const std::string name = std::string("ProgramOutput tick ") + r.name + " @" + std::to_string(fps);
            if (!g_filter.empty() && name.find(g_filter) == std::string::npos) continue;
            video::ProgramFormat fmt;
            fmt.width = r.w; fmt.height = r.h; fmt.fps = {fps, 1};
            video::ProgramOutput po(fmt);
            std::vector<std::shared_ptr<const video::Source>> srcs = {std::make_shared<NoiseSource>(1)};
            switcher::Switcher sw(srcs);
            po.setRenderer([&](video::Frame& f, std::uint64_t i) { sw.renderProgram(f, i); });
            std::vector<double> gaps;
            Clock::time_point last{};
            po.addConsumer([&](const std::shared_ptr<const video::Frame>&) {
                const auto now = Clock::now();
                if (last.time_since_epoch().count()) gaps.push_back(std::chrono::duration<double, std::milli>(now - last).count());
                last = now;
            });
            po.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(g_quick ? 400 : 1500));
            po.stop();
            if (gaps.empty()) continue;
            std::sort(gaps.begin(), gaps.end());
            std::printf("%-46s median gap %7.2f ms  p99 %7.2f ms  produced %4llu  skipped %llu\n", name.c_str(),
                        gaps[gaps.size() / 2], gaps[std::min(gaps.size() - 1, gaps.size() * 99 / 100)],
                        static_cast<unsigned long long>(po.framesProduced()),
                        static_cast<unsigned long long>(po.clockStats().skippedTicks));
        }
    }
}

void benchUiPath() {
    // The UI frame path without the JPEG encoder (Windows-only WIC): Engine::renderUiFrame for every kind of tile,
    // with a 1080p Program running.
    video::ProgramFormat fmt;
    app::Engine eng(fmt);
    eng.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    video::Frame out;
    for (const char* q : {"baixa", "alta", "maxima"}) {
        eng.setPreviewQuality(q);
        for (const char* id : {"pgm", "pvw", "s0"})
            run(std::string("UI renderUiFrame ") + id + " [" + q + "] (program 1080p)", [&] { eng.renderUiFrame(id, out); });
    }
    eng.setPreviewQuality("alta");
    run("UI feeder tick: pgm+pvw+8 tiles [alta]", [&] {
        eng.renderUiFrame("pgm", out);
        eng.renderUiFrame("pvw", out);
        for (int i = 0; i < 8; ++i) eng.renderUiFrame("s" + std::to_string(i), out);
    });
    eng.stop();
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--quick")) g_quick = true;
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) g_filter = argv[++i];
    }
    std::printf("# hb_bench (%d hardware threads)\n", static_cast<int>(std::thread::hardware_concurrency()));
    benchFrameOps();
    benchConvert();
    benchOverlay();
    benchScene();
    benchSwitcherAndProgram();
    benchUiPath();
    return 0;
}
