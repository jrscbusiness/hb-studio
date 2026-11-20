#include "plugins/native_source.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hb::plugins {

namespace {

// ---- guarded calls into plugin code -------------------------------------------------------------
// Windows: an access violation inside the plugin is turned into a failed call instead of killing the
// whole application (best effort: it cannot protect against heap corruption done by the plugin).
#ifdef _WIN32
bool sehInvoke(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
bool sehInvoke(void (*fn)(void*), void* ctx) {
    fn(ctx);
    return true;
}
#endif

template <class F>
bool guarded(F&& f) noexcept {
    struct Thunk {
        F* f;
        bool threw = false;
    } t{&f};
    auto tramp = [](void* c) {
        auto* th = static_cast<Thunk*>(c);
        try {
            (*th->f)();
        } catch (...) {
            th->threw = true;
        }
    };
    return sehInvoke(tramp, &t) && !t.threw;
}

std::string safeStr(const char* p, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c < 0x20 || c == 0x7F) continue;
        s += static_cast<char>(c);
    }
    return s;
}

}  // namespace

bool NativePlugin::validSourceId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

NativePlugin::~NativePlugin() {
    if (hasSource_ && api_.shutdown) guarded([&] { api_.shutdown(); });
    if (hasFx_ && fx_.shutdown) guarded([&] { fx_.shutdown(); });
}

std::shared_ptr<NativePlugin> NativePlugin::fromApi(const hb_plugin_v1& api, const std::string& pluginId, std::string& error) {
    if (api.api_version != HB_PLUGIN_API_VERSION) {
        error = "versão da API do plugin não suportada";
        return nullptr;
    }
    if (api.struct_size != sizeof(hb_plugin_v1)) {
        error = "estrutura do plugin com tamanho inesperado";
        return nullptr;
    }
    if (!api.list_sources || !api.open_source || !api.render || !api.close_source) {
        error = "o plugin não implementa funções obrigatórias";
        return nullptr;
    }
    std::shared_ptr<NativePlugin> p(new NativePlugin());
    p->id_ = pluginId;
    p->api_ = api;
    p->hasSource_ = true;
    return p;
}

std::shared_ptr<NativePlugin> NativePlugin::load(const std::string& libPath, const std::string& pluginId, std::string& error) {
    auto lib = std::make_shared<SharedLibrary>();
    if (!lib->load(libPath, error)) return nullptr;
    std::shared_ptr<NativePlugin> p(new NativePlugin());
    p->id_ = pluginId;
    p->lib_ = lib;

    if (auto* get = reinterpret_cast<hb_plugin_get_v1_fn>(lib->symbol("hb_plugin_get_v1"))) {
        hb_plugin_v1 api;
        std::memset(&api, 0, sizeof api);
        api.struct_size = sizeof api;
        api.api_version = HB_PLUGIN_API_VERSION;
        int rc = -1;
        if (!guarded([&] { rc = get(&api); }) || rc != 0) {
            error = "hb_plugin_get_v1 falhou";
            return nullptr;
        }
        auto v = fromApi(api, pluginId, error);
        if (!v) return nullptr;
        p->api_ = v->api_;
        p->hasSource_ = true;
        v->hasSource_ = false;   // `v` must not call shutdown(): `p` owns the API now
    }
    if (auto* get = reinterpret_cast<hb_plugin_get_effects_v1_fn>(lib->symbol("hb_plugin_get_effects_v1"))) {
        hb_effects_v1 fx;
        std::memset(&fx, 0, sizeof fx);
        fx.struct_size = sizeof fx;
        fx.api_version = HB_PLUGIN_API_VERSION;
        int rc = -1;
        if (!guarded([&] { rc = get(&fx); }) || rc != 0) {
            error = "hb_plugin_get_effects_v1 falhou";
            return nullptr;
        }
        if (fx.api_version != HB_PLUGIN_API_VERSION || fx.struct_size != sizeof fx || !fx.count || !fx.describe ||
            !fx.create || !fx.process || !fx.destroy || !fx.set_param) {
            error = "interface de efeitos do plugin inválida";
            return nullptr;
        }
        p->fx_ = fx;
        p->hasFx_ = true;
    }
    if (!p->hasSource_ && !p->hasFx_) {
        error = "a biblioteca não exporta hb_plugin_get_v1 nem hb_plugin_get_effects_v1";
        return nullptr;
    }
    return p;
}

bool NativePlugin::listSources(std::vector<SourceInfo>& out, std::string& error) {
    out.clear();
    if (!hasSource_) return true;
    std::lock_guard lock(listMutex_);
    std::vector<hb_source_desc> buf(HB_PLUGIN_MAX_SOURCES);
    std::memset(buf.data(), 0, buf.size() * sizeof(hb_source_desc));
    int n = -1;
    if (!guarded([&] { n = api_.list_sources(buf.data(), HB_PLUGIN_MAX_SOURCES); })) {
        error = "o plugin falhou ao listar fontes";
        return false;
    }
    if (n < 0) {
        error = "o plugin informou erro ao listar fontes";
        return false;
    }
    n = std::min(n, HB_PLUGIN_MAX_SOURCES);
    for (int i = 0; i < n; ++i) {
        SourceInfo s;
        s.id = safeStr(buf[static_cast<std::size_t>(i)].id, HB_PLUGIN_STR_MAX);
        s.name = safeStr(buf[static_cast<std::size_t>(i)].name, HB_PLUGIN_STR_MAX);
        if (!validSourceId(s.id)) continue;                         // ignore entries we cannot address safely
        if (s.name.empty()) s.name = s.id;
        if (std::any_of(out.begin(), out.end(), [&](const SourceInfo& o) { return o.id == s.id; })) continue;
        out.push_back(std::move(s));
    }
    return true;
}

std::shared_ptr<video::LiveSource> NativePlugin::open(const std::string& sourceId, std::string& error, SourceLimits limits) {
    if (!hasSource_ || !validSourceId(sourceId)) {
        error = "fonte inválida";
        return nullptr;
    }
    std::vector<SourceInfo> list;
    if (!listSources(list, error)) return nullptr;
    auto it = std::find_if(list.begin(), list.end(), [&](const SourceInfo& s) { return s.id == sourceId; });
    if (it == list.end()) {
        error = "fonte não encontrada no plugin";
        return nullptr;
    }
    void* handle = nullptr;
    std::lock_guard lock(listMutex_);
    if (!guarded([&] { handle = api_.open_source(sourceId.c_str()); }) || !handle) {
        error = "o plugin não conseguiu abrir a fonte";
        return nullptr;
    }
    return std::make_shared<PluginSource>(shared_from_this(), handle, it->name, limits);
}

PluginSource::PluginSource(std::shared_ptr<NativePlugin> plugin, void* handle, std::string name, SourceLimits limits)
    : plugin_(std::move(plugin)), handle_(handle), name_(std::move(name)), limits_(limits) {}

PluginSource::~PluginSource() {
    std::lock_guard lock(mutex_);
    void* h = handle_;
    handle_ = nullptr;
    if (h) guarded([&] { plugin_->api_.close_source(h); });
}

void PluginSource::render(video::Frame& f, std::uint64_t index) const {
    auto fillDark = [&] {
        for (std::size_t i = 0; i + 3 < f.data.size(); i += 4) {
            f.data[i] = 43; f.data[i + 1] = 18; f.data[i + 2] = 7; f.data[i + 3] = 255;
        }
    };
    const std::size_t need = static_cast<std::size_t>(f.stride) * static_cast<std::size_t>(std::max(f.height, 0));
    if (f.width <= 0 || f.height <= 0 || f.stride < f.width * 4 || f.data.size() < need) return;

    std::lock_guard lock(mutex_);
    if (faulted_ || !handle_) {
        fillDark();
        return;
    }
    int rc = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = guarded([&] { rc = plugin_->api_.render(handle_, f.data.data(), f.width, f.height, f.stride, index); });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rendered_ = true;

    if (!ok) {
        faulted_ = true;
        fault_ = "o plugin travou ao gerar imagem e foi desligado";
        fillDark();
        return;
    }
    slowRun_ = ms > limits_.slowMs ? slowRun_ + 1 : 0;
    if (slowRun_ >= limits_.maxSlowInARow) {
        faulted_ = true;
        fault_ = "o plugin está lento demais para tempo real e foi desligado";
        fillDark();
        return;
    }
    if (rc < 0) {
        if (++errorRun_ >= limits_.maxErrorsInARow) {
            faulted_ = true;
            fault_ = "o plugin reportou erros repetidos e foi desligado";
        }
        fillDark();
        return;
    }
    errorRun_ = 0;
    if (rc == HB_RENDER_NO_SIGNAL) {
        noSignal_ = true;
        fillDark();
        return;
    }
    noSignal_ = false;
    lastOk_ = std::chrono::steady_clock::now();
    for (int y = 0; y < f.height; ++y) {                    // plugins often leave alpha at 0
        std::uint8_t* row = f.data.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < f.width; ++x) row[x * 4 + 3] = 255;
    }
    if (w_ == 0 && plugin_->api_.get_info) {                // informational only, asked once
        int w = 0, h = 0;
        double fps = 0;
        if (guarded([&] { (void)plugin_->api_.get_info(handle_, &w, &h, &fps); }) && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            w_ = w;
            h_ = h;
            fps_ = std::clamp(fps, 0.0, 1000.0);
        }
    }
}

video::SourceStatus PluginSource::status() const {
    using S = video::SourceStatus::State;
    std::lock_guard lock(mutex_);
    video::SourceStatus s;
    s.width = w_;
    s.height = h_;
    s.fps = fps_;
    if (faulted_) {
        s.state = S::Error;
        s.message = fault_;
    } else if (!rendered_) {
        s.state = S::Connecting;
    } else if (noSignal_) {
        s.state = S::NoSignal;
        s.message = "O plugin informou que a fonte está sem imagem.";
    } else {
        s.state = S::Online;
    }
    return s;
}

}  // namespace hb::plugins
