#include "audio/plugin_effect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace hb::audio {
namespace {

// Reads a fixed buffer that must contain a NUL, be valid UTF-8 and free of control characters.
bool readString(const char* buf, std::size_t cap, std::string& out) {
    const void* nul = std::memchr(buf, 0, cap);
    if (nul == nullptr) return false;
    const std::size_t len = static_cast<std::size_t>(static_cast<const char*>(nul) - buf);
    std::size_t i = 0;
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        std::size_t extra;
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) extra = 0;
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + extra >= len) return false;
        for (std::size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(buf[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    out.assign(buf, len);
    return true;
}

bool okTypeId(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool okParamId(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool fail(std::string& error, const std::string& msg) {
    error = msg;
    return false;
}

}  // namespace

bool describePluginEffects(const PluginEffectLibrary& lib, std::vector<PluginEffectDesc>& out, std::string& error) {
    out.clear();
    error.clear();
    const hb_effects_v1& a = lib.api;
    if (!a.count || !a.describe || !a.create || !a.set_param || !a.process || !a.reset || !a.destroy)
        return fail(error, "plugin API incomplete: missing function pointer");
    std::vector<PluginEffectDesc> result;
    try {
        const int count = a.count();
        if (count < 0 || count > kPluginMaxEffects)
            return fail(error, "invalid effect count: " + std::to_string(count));
        std::set<std::string> seenTypes;
        for (int idx = 0; idx < count; ++idx) {
            const std::string at = "effect #" + std::to_string(idx) + ": ";
            hb_effect_desc d;
            hb_effect_param ps[HB_EFFECT_MAX_PARAMS];
            std::memset(&d, 0, sizeof d);
            std::memset(ps, 0, sizeof ps);
            const int rc = a.describe(idx, &d, ps, HB_EFFECT_MAX_PARAMS);
            if (rc != 0) return fail(error, at + "describe failed (code " + std::to_string(rc) + ")");
            PluginEffectDesc e;
            if (!readString(d.type_id, sizeof d.type_id, e.typeId) || !okTypeId(e.typeId))
                return fail(error, at + "invalid type_id");
            if (!seenTypes.insert(e.typeId).second) return fail(error, at + "duplicate type_id " + e.typeId);
            if (!readString(d.name, sizeof d.name, e.name) || e.name.empty()) return fail(error, at + "invalid name");
            if (d.param_count < 0 || d.param_count > HB_EFFECT_MAX_PARAMS)
                return fail(error, at + "invalid param_count " + std::to_string(d.param_count));
            std::set<std::string> seenParams;
            for (int pi = 0; pi < d.param_count; ++pi) {
                const std::string pat = at + "param #" + std::to_string(pi) + ": ";
                const hb_effect_param& p = ps[pi];
                EffectParamInfo info;
                if (!readString(p.id, sizeof p.id, info.id) || !okParamId(info.id)) return fail(error, pat + "invalid id");
                if (!seenParams.insert(info.id).second) return fail(error, pat + "duplicate id " + info.id);
                if (!readString(p.label, sizeof p.label, info.label) || info.label.empty())
                    return fail(error, pat + "invalid label");
                if (!readString(p.unit, sizeof p.unit, info.unit)) return fail(error, pat + "invalid unit");
                if (!std::isfinite(p.min_value) || !std::isfinite(p.max_value) || !std::isfinite(p.default_value))
                    return fail(error, pat + "non-finite range");
                if (!(p.min_value < p.max_value)) return fail(error, pat + "min must be < max");
                if (p.default_value < p.min_value || p.default_value > p.max_value)
                    return fail(error, pat + "default outside range");
                if (p.logarithmic != 0 && p.min_value <= 0.f) return fail(error, pat + "logarithmic needs min > 0");
                info.min = p.min_value;
                info.max = p.max_value;
                info.def = p.default_value;
                info.logarithmic = p.logarithmic != 0;
                e.params.push_back(std::move(info));
            }
            result.push_back(std::move(e));
        }
    } catch (const std::exception& ex) {
        return fail(error, std::string("plugin threw: ") + ex.what());
    } catch (...) {
        return fail(error, "plugin threw an unknown exception");
    }
    out = std::move(result);
    return true;
}

PluginEffect::PluginEffect(std::shared_ptr<PluginEffectLibrary> lib, int index, PluginEffectDesc desc)
    : lib_(std::move(lib)), index_(index), desc_(std::move(desc)), values_(desc_.params.size()),
      pushed_(desc_.params.size(), std::numeric_limits<float>::quiet_NaN()) {
    for (std::size_t i = 0; i < desc_.params.size(); ++i) values_[i].store(desc_.params[i].def);
}

PluginEffect::~PluginEffect() { destroyInstance(); }

void PluginEffect::destroyInstance() noexcept {
    if (inst_ != nullptr && lib_) {
        try { lib_->api.destroy(inst_); } catch (...) {}
    }
    inst_ = nullptr;
}

int PluginEffect::indexOf(const std::string& id) const noexcept {
    for (std::size_t i = 0; i < desc_.params.size(); ++i)
        if (desc_.params[i].id == id) return static_cast<int>(i);
    return -1;
}

void PluginEffect::setParam(const std::string& id, float value) {
    const int i = indexOf(id);
    if (i < 0 || !std::isfinite(value)) return;
    const auto& p = desc_.params[static_cast<std::size_t>(i)];
    values_[static_cast<std::size_t>(i)].store(std::min(std::max(value, p.min), p.max), std::memory_order_relaxed);
}

float PluginEffect::getParam(const std::string& id) const {
    const int i = indexOf(id);
    return i < 0 ? 0.f : values_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
}

void PluginEffect::prepare(int sampleRate, int channels) {
    destroyInstance();
    channels_ = std::min(std::max(channels, 1), 32);
    consecutiveBad_ = 0;
    bypass_.store(false, std::memory_order_relaxed);
    std::fill(pushed_.begin(), pushed_.end(), std::numeric_limits<float>::quiet_NaN());
    try {
        inst_ = lib_->api.create(index_, std::max(sampleRate, 1), channels_);
    } catch (...) {
        inst_ = nullptr;
    }
    if (inst_ == nullptr) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        bypass_.store(true, std::memory_order_relaxed);
    }
}

void PluginEffect::process(float* d, std::size_t frames) {
    if (d == nullptr || frames == 0) return;
    const std::size_t n = frames * static_cast<std::size_t>(channels_);
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(d[i])) d[i] = 0.f;
    if (inst_ == nullptr || bypass_.load(std::memory_order_relaxed)) return;
    if (frames > static_cast<std::size_t>(std::numeric_limits<int>::max())) return;

    bool bad = false;
    try {
        for (std::size_t i = 0; i < pushed_.size(); ++i) {
            const float v = values_[i].load(std::memory_order_relaxed);
            if (v != pushed_[i]) {
                lib_->api.set_param(inst_, static_cast<int>(i), v);
                pushed_[i] = v;
            }
        }
        lib_->api.process(inst_, d, static_cast<int>(frames));
    } catch (...) {
        bad = true;
    }
    if (!bad)
        for (std::size_t i = 0; i < n; ++i)
            if (!std::isfinite(d[i])) { bad = true; break; }
    if (bad) {
        std::fill(d, d + n, 0.f);
        errors_.fetch_add(1, std::memory_order_relaxed);
        if (++consecutiveBad_ >= kPluginMaxConsecutiveBadBlocks) bypass_.store(true, std::memory_order_relaxed);
    } else {
        consecutiveBad_ = 0;
    }
}

void PluginEffect::reset() {
    if (inst_ == nullptr) return;
    try { lib_->api.reset(inst_); } catch (...) { errors_.fetch_add(1, std::memory_order_relaxed); }
}

std::unique_ptr<Effect> createPluginEffect(std::shared_ptr<PluginEffectLibrary> lib, int index,
                                           const PluginEffectDesc& desc) {
    if (!lib || index < 0) return nullptr;
    return std::make_unique<PluginEffect>(std::move(lib), index, desc);
}

}  // namespace hb::audio
