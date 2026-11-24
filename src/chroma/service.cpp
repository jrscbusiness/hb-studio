#include "chroma/service.hpp"

#include <cstdio>

#include "core/json.hpp"

namespace hb::chroma {

namespace {
std::string baseName(const std::string& p) {
    const auto i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}
std::string colorHex(std::uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "#%06x", static_cast<unsigned>(c & 0xFFFFFF));
    return b;
}
}  // namespace

ChromaService::ChromaService(int slots, Deps deps)
    : deps_(std::move(deps)), store_(deps_.modelsDir, deps_.model, deps_.http) {
    const int n = slots > 0 ? slots : 0;
    for (int i = 0; i < n; ++i) filters_.push_back(std::make_shared<ChromaFilter>());
    imageErr_.assign(static_cast<std::size_t>(n), std::string());
    store_.refresh();
}

ChromaService::~ChromaService() { shutdown(); }

void ChromaService::shutdown() {
    store_.cancel();
    store_.waitIdle();
    if (loader_.joinable()) loader_.join();
    for (auto& f : filters_) f->shutdown();
}

std::shared_ptr<ChromaFilter> ChromaService::filter(int slot) const {
    if (slot < 0 || slot >= slotCount()) return nullptr;
    return filters_[static_cast<std::size_t>(slot)];
}

void ChromaService::loadImageFor(int slot, const std::string& path) {
    auto f = filter(slot);
    if (!f) return;
    std::string err;
    std::shared_ptr<video::Frame> img;
    if (path.empty()) {
        // cleared: nothing to load
    } else if (!deps_.imageLoader) {
        err = "Imagem de fundo indisponível nesta plataforma.";
    } else {
        auto fr = std::make_shared<video::Frame>();
        try {
            if (deps_.imageLoader(path, *fr, err) && fr->width > 0 && fr->height > 0) img = fr;
            else if (err.empty()) err = "Não foi possível abrir a imagem.";
        } catch (...) {
            err = "Falha ao decodificar a imagem.";
        }
    }
    f->setImage(img);
    std::lock_guard lock(m_);
    imageErr_[static_cast<std::size_t>(slot)] = err;
}

void ChromaService::load(int slot, const std::string& serialized) {
    auto f = filter(slot);
    if (!f || serialized.empty()) return;
    const Settings s = parseSettings(serialized);
    f->setSettings(s);
    if (s.repl == ReplKind::Image && !s.image.empty()) loadImageFor(slot, s.image);
    ensureSegmenter();
}

void ChromaService::reset(int slot) {
    auto f = filter(slot);
    if (!f) return;
    f->setSettings(Settings{});
    f->setImage(nullptr);
}

bool ChromaService::handleSetting(int slot, const std::string& key, const std::string& value, std::string& message) {
    auto f = filter(slot);
    if (!f) return false;
    Settings s = f->settings();
    if (!setField(s, key, value)) return false;
    if (s.mode == Mode::Ai) {
        if (!aiRuntimeAvailable()) {
            message = "A IA não está disponível neste build (sem ONNX Runtime). Use o chroma verde/azul.";
        } else if (!store_.ready()) {
            message = "Baixe o modelo de IA primeiro (botão na seção Fundo).";
        }
    }
    f->setSettings(s);
    if (key == "img" || (key == "repl" && s.repl == ReplKind::Image)) loadImageFor(slot, s.image);
    if (deps_.onChanged) deps_.onChanged(slot, serializeSettings(s));
    ensureSegmenter();
    return true;
}

bool ChromaService::handleModel(const std::string& action, std::string& message) {
    if (action == "download") {
        std::string why;
        if (!store_.startDownload(&why)) {
            message = "Não foi possível baixar o modelo: " + why + ".";
            return false;
        }
        message = "Baixando o modelo de IA…";
        return true;
    }
    if (action == "cancel") {
        store_.cancel();
        return true;
    }
    if (action == "remove") {
        // Detach the segmenter from every slot first so no worker keeps the file's model alive.
        for (auto& f : filters_) f->setSegmenter(nullptr);
        {
            std::lock_guard lock(m_);
            segmenter_.reset();
            aiState_ = AiState::Idle;
            aiMsg_.clear();
        }
        if (!store_.remove()) {
            message = "Não foi possível remover o modelo agora.";
            return false;
        }
        message = "Modelo de IA removido.";
        return true;
    }
    return false;
}

void ChromaService::ensureSegmenter() {
    if (!deps_.segmenterFactory || !store_.ready()) return;
    {
        std::lock_guard lock(m_);
        if (aiState_ != AiState::Idle) return;
        // Only pay for the runtime once at least one slot actually wants AI.
        bool want = false;
        for (auto& f : filters_) want = want || f->settings().mode == Mode::Ai;
        if (!want) return;
        aiState_ = AiState::Loading;
    }
    if (loader_.joinable()) loader_.join();
    const std::string path = store_.path();
    loader_ = std::thread([this, path] {
        std::string err;
        std::shared_ptr<ISegmenter> seg;
        try {
            seg = deps_.segmenterFactory(path, err);
        } catch (const std::exception& e) {
            err = e.what();
        } catch (...) {
            err = "exceção desconhecida";
        }
        {
            std::lock_guard lock(m_);
            if (seg) {
                segmenter_ = seg;
                aiState_ = AiState::Ready;
                aiMsg_.clear();
            } else {
                aiState_ = AiState::Error;
                aiMsg_ = "Falha ao iniciar a IA: " + err;
            }
        }
        if (seg)
            for (auto& f : filters_) f->setSegmenter(seg);
    });
}

void ChromaService::tick() {
    ensureSegmenter();
    // A freshly attached filter (or a re-downloaded model after "remove") picks the segmenter up here.
    std::shared_ptr<ISegmenter> seg;
    {
        std::lock_guard lock(m_);
        if (aiState_ == AiState::Ready) seg = segmenter_;
    }
    if (seg)
        for (auto& f : filters_) f->setSegmenter(seg);   // no-op when unchanged
}

std::string ChromaService::statusJson() {
    namespace js = core::json;
    const auto ms = store_.status();
    const char* mstate = "none";
    switch (ms.state) {
        case ModelStore::State::Ready: mstate = "ready"; break;
        case ModelStore::State::Downloading: mstate = "downloading"; break;
        case ModelStore::State::Error: mstate = "error"; break;
        case ModelStore::State::Unavailable: mstate = "unavailable"; break;
        default: break;
    }
    const char* astate = "idle";
    std::string amsg;
    {
        std::lock_guard lock(m_);
        astate = aiState_ == AiState::Loading ? "loading" : aiState_ == AiState::Ready ? "ready" : aiState_ == AiState::Error ? "error" : "idle";
        amsg = aiMsg_;
    }
    char nb[160];
    std::snprintf(nb, sizeof nb, "\"received\":%llu,\"total\":%llu,\"mb\":%.2f",
                  static_cast<unsigned long long>(ms.received), static_cast<unsigned long long>(ms.total),
                  static_cast<double>(store_.info().sizeBytes) / (1024.0 * 1024.0));
    std::string j = std::string("{\"t\":\"chroma\",\"runtime\":") + (aiRuntimeAvailable() ? "true" : "false") +
                    ",\"images\":" + (deps_.imageLoader ? "true" : "false") + ",\"model\":{\"state\":\"" + mstate + "\"," + nb +
                    ",\"msg\":\"" + js::escape(ms.message) + "\",\"license\":\"" + js::escape(store_.info().license) +
                    "\"},\"ai\":{\"state\":\"" + astate + "\",\"msg\":\"" + js::escape(amsg) + "\"},\"slots\":[";
    for (int i = 0; i < slotCount(); ++i) {
        const auto f = filters_[static_cast<std::size_t>(i)];
        const Settings s = f->settings();
        const auto st = f->stats();
        std::string ierr;
        {
            std::lock_guard lock(m_);
            ierr = imageErr_[static_cast<std::size_t>(i)];
        }
        // "Slow" means the computer cannot keep up with the quality level chosen: the AI inference
        // takes longer than ~90% of the interval it was supposed to fit in, or the filter itself
        // eats a big part of the 33 ms frame budget on the rendering thread.
        const bool slowAi = st.aiActive && st.inferences >= 5 && st.inferMsAvg > 0.9 * qualityParams(s.quality).minIntervalMs;
        const bool slowApply = st.applyMsAvg > 15.0;
        char nums[256];
        std::snprintf(nums, sizeof nums,
                      "\"tol\":%d,\"soft\":%d,\"spill\":%d,\"blur\":%d,\"applyMs\":%.1f,\"inferMs\":%.1f,\"inferences\":%llu,\"dropped\":%llu",
                      s.tolerance, s.softness, s.spill, s.blur, st.applyMsAvg, st.inferMsAvg,
                      static_cast<unsigned long long>(st.inferences), static_cast<unsigned long long>(st.dropped));
        if (i) j += ",";
        j += std::string("{\"mode\":\"") + modeName(s.mode) + "\",\"q\":\"" + qualityName(s.quality) + "\",\"repl\":\"" +
             replName(s.repl) + "\",\"color\":\"" + colorHex(s.color) + "\"," + nums + ",\"img\":\"" +
             js::escape(baseName(s.image)) + "\",\"imgErr\":\"" + js::escape(ierr) + "\",\"aiActive\":" +
             (st.aiActive ? "true" : "false") + ",\"aiWaiting\":" + (st.aiWaiting ? "true" : "false") + ",\"aiFailed\":" +
             (st.aiFailed ? "true" : "false") + ",\"aiMsg\":\"" + js::escape(st.aiMessage) + "\",\"slowAi\":" +
             (slowAi ? "true" : "false") + ",\"slowApply\":" + (slowApply ? "true" : "false") + "}";
    }
    j += "]}";
    return j;
}

}  // namespace hb::chroma
