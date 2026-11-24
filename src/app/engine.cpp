#include "app/engine.hpp"

#include <algorithm>

#include "video/frame_ops.hpp"

namespace hb::app {

const std::vector<PreviewQualityPreset>& Engine::previewQualityPresets() {
    static const std::vector<PreviewQualityPreset> kPresets = {
        {"baixa", "Baixa (mais leve)", 320, 180, 96, 54},
        {"media", "Média", 480, 270, 144, 81},
        {"alta", "Alta (padrão)", 640, 360, 192, 108},
        {"maxima", "Máxima (computadores potentes)", 960, 540, 256, 144},
    };
    return kPresets;
}

std::string Engine::previewQuality() const {
    const auto& presets = previewQualityPresets();
    const int i = previewQualityIndex_.load();
    return (i >= 0 && static_cast<std::size_t>(i) < presets.size()) ? presets[static_cast<std::size_t>(i)].id
                                                                     : presets[kDefaultPreviewQualityIndex].id;
}

bool Engine::setPreviewQuality(const std::string& id) {
    const auto& presets = previewQualityPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (id == presets[i].id) {
            previewQualityIndex_.store(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

Engine::Engine(video::ProgramFormat format) {
    log_ = std::make_shared<core::MemorySink>(500);
    logger_.addSink(log_);
    hw_ = core::detectHardware();
    tier_ = core::classify(hw_);

    for (int i = 0; i < kSourceCount; ++i) {
        auto test = std::make_shared<video::SyntheticSource>(i + 1, "CAM " + std::to_string(i + 1));
        auto slot = std::make_shared<video::SlotSource>(i + 1, test);
        slots_.push_back(slot);
        sources_.push_back(slot);
    }
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

bool Engine::assignSlot(int slot, std::shared_ptr<const video::LiveSource> device, std::string deviceId,
                        std::string deviceName) {
    if (slot < 0 || slot >= kSourceCount || !device) return false;
    slots_[static_cast<std::size_t>(slot)]->assign(std::move(device), deviceId, deviceName);
    logger_.info("input", "CAM " + std::to_string(slot + 1) + " <- " + deviceName);
    return true;
}

bool Engine::clearSlot(int slot) {
    if (slot < 0 || slot >= kSourceCount) return false;
    slots_[static_cast<std::size_t>(slot)]->clear();
    logger_.info("input", "CAM " + std::to_string(slot + 1) + " <- fonte de teste");
    return true;
}

// ---- FEATURE: chroma ----
bool Engine::setSlotFilter(int slot, std::shared_ptr<video::FrameFilter> filter) {
    if (slot < 0 || slot >= kSourceCount) return false;
    slots_[static_cast<std::size_t>(slot)]->setFilter(std::move(filter));
    return true;
}
// ---- /FEATURE: chroma ----

video::SlotSource::Info Engine::slotInfo(int slot) const {
    if (slot < 0 || slot >= kSourceCount) return {};
    return slots_[static_cast<std::size_t>(slot)]->info();
}

bool Engine::renderUiFrame(const std::string& id, video::Frame& out) {
    const auto uiIndex = program_->framesProduced();
    const auto& preset = previewQualityPresets()[static_cast<std::size_t>(
        std::clamp(previewQualityIndex_.load(), 0, static_cast<int>(previewQualityPresets().size()) - 1))];
    if (id == "pgm") {
        auto latest = program_->latest();
        if (!latest) return false;
        video::downscaleBox(*latest, out, preset.monitorW, preset.monitorH);
        return true;
    }
    if (id == "pvw") {
        video::prepareFrame(out, preset.monitorW, preset.monitorH);
        switcher_->renderPreview(out, uiIndex);
        return true;
    }
    if (id.size() >= 2 && id[0] == 's') {
        // "s0".."s15": digits-only after the 's' (kSourceCount can exceed 9, so this can't just
        // read a single character like it used to when it was always one digit).
        int i = 0;
        for (std::size_t k = 1; k < id.size(); ++k) {
            if (id[k] < '0' || id[k] > '9') return false;
            i = i * 10 + (id[k] - '0');
        }
        if (i >= kSourceCount) return false;
        video::prepareFrame(out, preset.tileW, preset.tileH);
        sources_[static_cast<std::size_t>(i)]->render(out, uiIndex);
        return true;
    }
    return false;
}

}  // namespace hb::app
