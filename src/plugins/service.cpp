#include "plugins/service.hpp"

#include <random>
#include <sstream>

#include "core/json.hpp"

namespace hb::plugins {

namespace {
using core::json::escape;

std::string newToken() {
    std::random_device rd;
    char b[40];
    std::snprintf(b, sizeof b, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    return b;
}

const char* typeName(PluginType t) {
    switch (t) {
        case PluginType::Source: return "source";
        case PluginType::Effect: return "effect";
        case PluginType::Theme: return "theme";
    }
    return "theme";
}
}  // namespace

PluginService::PluginService(std::string rootDirUtf8, std::string platformKey)
    : manager_(std::move(rootDirUtf8), std::move(platformKey)), runtime_(manager_) {}

void PluginService::start() {
    manager_.scan();
    runtime_.sync();
}

std::string PluginService::resultJson(bool ok, const std::string& message, const std::string& id) {
    return std::string("{\"t\":\"plugin_result\",\"ok\":") + (ok ? "true" : "false") + ",\"id\":\"" + escape(id) + "\",\"message\":\"" +
           escape(message) + "\"}";
}

std::string PluginService::beginInstall(const std::string& packagePath) {
    const Review r = manager_.inspect(packagePath);
    if (!r.ok) return "{\"t\":\"plugin_review\",\"ok\":false,\"error\":\"" + escape(r.error) + "\"}";
    const auto& m = r.info.manifest;
    const std::string token = newToken();
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_.begin(); it != pending_.end();)   // only the newest review stays valid
            it = pending_.erase(it);
        pending_[token] = {packagePath, r.info.packageSha256, now + ttl_};
    }
    std::ostringstream o;
    o << "{\"t\":\"plugin_review\",\"ok\":true,\"token\":\"" << token << "\",\"id\":\"" << escape(m.id) << "\",\"name\":\"" << escape(m.name)
      << "\",\"version\":\"" << escape(m.version) << "\",\"author\":\"" << escape(m.author) << "\",\"description\":\"" << escape(m.description)
      << "\",\"license\":\"" << escape(m.license) << "\",\"type\":\"" << typeName(m.type) << "\",\"native\":"
      << (m.type == PluginType::Theme ? "false" : "true") << ",\"files\":" << r.info.fileCount << ",\"size\":" << r.info.totalSize
      << ",\"replaces\":" << (r.replaces ? "true" : "false") << ",\"installedVersion\":\"" << escape(r.installedVersion) << "\",\"sha\":\""
      << r.info.packageSha256.substr(0, 16) << "\"}";
    return o.str();
}

std::string PluginService::confirmInstall(const std::string& token) {
    Pending p;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(token);
        if (it == pending_.end()) return resultJson(false, "esta confirmação não é mais válida; escolha o arquivo de novo");
        p = it->second;
        pending_.erase(it);                                    // single use
    }
    if (std::chrono::steady_clock::now() > p.expires) return resultJson(false, "a confirmação expirou; escolha o arquivo de novo");
    const Result r = manager_.install(p.path, p.sha);
    std::string id;
    if (r.ok) {
        const Review again = manager_.inspect(p.path);
        if (again.ok) id = again.info.manifest.id;
    }
    return resultJson(r.ok, r.message, id);
}

void PluginService::cancelInstall(const std::string& token) {
    std::lock_guard lock(mutex_);
    pending_.erase(token);
}

Result PluginService::setEnabled(const std::string& id, bool on) {
    Result r = manager_.setEnabled(id, on);
    if (r.ok && on) {
        runtime_.sync();                                      // loads the library (hash re-checked inside)
        InstalledPlugin p;
        if (manager_.find(id, p) && !p.enabled) {            // loading failed: the manager switched it off
            r.ok = false;
            r.message = p.error.empty() ? "o plugin não pôde ser carregado" : p.error;
        }
    }
    return r;
}

Result PluginService::remove(const std::string& id) { return manager_.remove(id); }

std::string PluginService::listJson() const {
    std::ostringstream o;
    o << "{\"t\":\"plugins\",\"ver\":" << manager_.version() << ",\"dir\":\"" << escape(manager_.rootDir()) << "\",\"list\":[";
    bool first = true;
    for (const auto& p : manager_.list()) {
        if (!first) o << ",";
        first = false;
        o << "{\"id\":\"" << escape(p.manifest.id) << "\",\"name\":\"" << escape(p.manifest.name) << "\",\"version\":\"" << escape(p.manifest.version)
          << "\",\"author\":\"" << escape(p.manifest.author) << "\",\"description\":\"" << escape(p.manifest.description) << "\",\"license\":\""
          << escape(p.manifest.license) << "\",\"type\":\"" << typeName(p.manifest.type) << "\",\"valid\":" << (p.valid ? "true" : "false")
          << ",\"supported\":" << (p.supported ? "true" : "false") << ",\"enabled\":" << (p.enabled ? "true" : "false")
          << ",\"loaded\":" << (p.loaded ? "true" : "false") << ",\"removed\":" << (p.pendingRemoval ? "true" : "false") << ",\"error\":\""
          << escape(p.error) << "\"}";
    }
    o << "]}";
    return o.str();
}

std::string PluginService::themeJson() const {
    std::ostringstream o;
    o << "{\"t\":\"theme\",\"id\":\"" << escape(manager_.activeThemeId()) << "\",\"scheme\":\"" << escape(manager_.activeColorScheme()) << "\",\"vars\":{";
    bool first = true;
    for (const auto& [k, v] : manager_.activeThemeVars()) {
        if (!validThemeVarName(k) || !validThemeColor(v)) continue;   // belt and braces: re-validated on the way out
        if (!first) o << ",";
        first = false;
        o << "\"" << escape(k) << "\":\"" << escape(v) << "\"";
    }
    o << "}}";
    return o.str();
}

}  // namespace hb::plugins
