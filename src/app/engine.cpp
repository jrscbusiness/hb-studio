#include "app/engine.hpp"

#include "video/frame_ops.hpp"

namespace hb::app {

Engine::Engine(video::ProgramFormat format) {
    log_ = std::make_shared<core::MemorySink>(500);
    logger_.addSink(log_);
    hw_ = core::detectHardware();
    tier_ = core::classify(hw_);

    for (int i = 0; i < kSourceCount; ++i)
        sources_.push_back(std::make_shared<video::SyntheticSource>(i + 1, "CAM " + std::to_string(i + 1)));
    switcher_ = std::make_unique<switcher::Switcher>(sources_, &bus_);
    program_ = std::make_unique<video::ProgramOutput>(format, &bus_);
    program_->setRenderer([this](video::Frame& f, std::uint64_t i) { switcher_->renderProgram(f, i); });

    bus_.subscribe("switcher.*", [this](const core::Event& e) {
        logger_.info("switcher", e.topic + " pvw=" + e.fields.at("pvw") + " pgm=" + e.fields.at("pgm"));
    });
    bus_.subscribe("transition.*", [this](const core::Event& e) { logger_.info("switcher", e.topic); });
    bus_.subscribe("program.*", [this](const core::Event& e) { logger_.info("program", e.topic); });
    logger_.info("engine", std::string("perfil ") + core::tierName(tier_) + ", " +
                               std::to_string(hw_.logicalCores) + " núcleos");
}

void Engine::start() {
    lastSample_ = std::chrono::steady_clock::now();
    lastProduced_ = program_->framesProduced();
    program_->start();
}

void Engine::stop() { program_->stop(); }

Engine::Snapshot Engine::snapshot() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto produced = program_->framesProduced();
    const double dt = std::chrono::duration<double>(now - lastSample_).count();
    if (dt >= 0.5) {
        fps_ = static_cast<double>(produced - lastProduced_) / dt;
        lastProduced_ = produced;
        lastSample_ = now;
    }
    Snapshot s;
    s.sw = switcher_->state();
    s.fps = program_->running() ? fps_ : 0.0;
    s.produced = produced;
    s.skipped = program_->clockStats().skippedTicks;
    s.tier = core::tierName(tier_);
    s.cores = hw_.logicalCores;
    s.ramGiB = static_cast<unsigned>(hw_.ramBytes >> 30);
    const auto& p = core::presetFor(tier_);
    s.sourcesRecommended = p.recommendedSources;
    s.sourcesMax = p.hardMaxSources;
    s.targetFps = static_cast<int>(program_->format().fps.value() + 0.5);
    return s;
}

bool Engine::renderUiFrame(const std::string& id, video::Frame& out) {
    const auto uiIndex = program_->framesProduced();
    if (id == "pgm") {
        auto latest = program_->latest();
        if (!latest) return false;
        video::downscaleBox(*latest, out, kMonitorW, kMonitorH);
        return true;
    }
    if (id == "pvw") {
        video::prepareFrame(out, kMonitorW, kMonitorH);
        switcher_->renderPreview(out, uiIndex);
        return true;
    }
    if (id.size() >= 2 && id[0] == 's') {
        const int i = id[1] - '0';
        if (i < 0 || i >= kSourceCount) return false;
        video::prepareFrame(out, kTileW, kTileH);
        sources_[static_cast<std::size_t>(i)]->render(out, uiIndex);
        return true;
    }
    return false;
}

}  // namespace hb::app
