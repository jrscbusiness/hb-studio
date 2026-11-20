#include "plugins/manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#include "core/json.hpp"
#include "core/sha256.hpp"
#include "plugins/fs_util.hpp"

namespace hb::plugins {

namespace fs = std::filesystem;

namespace {

std::string randomToken() {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    char b[40];
    std::snprintf(b, sizeof b, "%08x%08x%04x", rd(), rd(), counter.fetch_add(1) & 0xFFFF);
    return b;
}

bool readSmallFile(const fs::path& p, std::string& out, std::size_t maxBytes) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0 || static_cast<std::size_t>(n) > maxBytes) return false;
    out.resize(static_cast<std::size_t>(n));
    f.seekg(0);
    return static_cast<bool>(f.read(out.data(), n)) || n == 0;
}

bool hashFile(const fs::path& p, std::string& hex) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    core::Sha256 h;
    char buf[64 * 1024];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) h.update(buf, static_cast<std::size_t>(f.gcount()));
    if (f.bad()) return false;
    hex = h.hexDigest();
    return true;
}

}  // namespace

PluginManager::PluginManager(std::string rootDirUtf8, std::string platformKey)
    : root_(std::move(rootDirUtf8)), platform_(std::move(platformKey)) {}

InstalledPlugin* PluginManager::findLocked(const std::string& id) {
    for (auto& p : items_)
        if (p.manifest.id == id) return &p;
    return nullptr;
}
const InstalledPlugin* PluginManager::findLocked(const std::string& id) const {
    for (const auto& p : items_)
        if (p.manifest.id == id) return &p;
    return nullptr;
}

void PluginManager::loadStateLocked() {
    enabled_.clear();
    std::string text;
    core::json::Value v;
    if (!readSmallFile(pathFromUtf8(root_) / "state.json", text, 256 * 1024) || !core::json::parse(text, v) || !v.isObj()) return;
    const auto* en = v.get("enabled");
    if (!en || !en->isArr()) return;
    for (const auto& e : en->a)
        if (e.isStr() && validPluginId(e.s)) enabled_.insert(e.s);
}

void PluginManager::saveStateLocked() const {
    std::error_code ec;
    fs::create_directories(pathFromUtf8(root_), ec);
    std::ostringstream o;
    o << "{\"schema\":1,\"enabled\":[";
    bool first = true;
    for (const auto& id : enabled_) {
        o << (first ? "" : ",") << "\"" << core::json::escape(id) << "\"";
        first = false;
    }
    o << "]}";
    const fs::path path = pathFromUtf8(root_) / "state.json";
    const fs::path tmp = pathFromUtf8(root_) / "state.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << o.str();
    }
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
}

void PluginManager::scan() {
    std::lock_guard lock(mutex_);
    const fs::path root = pathFromUtf8(root_);
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::remove_all(root / ".staging", ec);          // leftovers of an interrupted install
    loadStateLocked();

    const std::set<std::string> loadedBefore = [&] {
        std::set<std::string> s;
        for (const auto& p : items_)
            if (p.loaded) s.insert(p.manifest.id);
        return s;
    }();
    items_.clear();
    std::set<std::string> present;
    for (const auto& de : fs::directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code e2;
        if (!de.is_directory(e2)) continue;
        const std::string dirName = pathToUtf8(de.path().filename());
        if (dirName.empty() || dirName[0] == '.') continue;
        if (fs::exists(de.path() / ".pending-delete", e2)) {   // removal requested while the library was loaded
            fs::remove_all(de.path(), e2);
            continue;
        }
        InstalledPlugin ip;
        ip.dir = pathToUtf8(de.path());
        std::string text;
        if (!readSmallFile(de.path() / "manifest.json", text, kMaxManifestBytes)) {
            ip.valid = false;
            ip.error = "manifest.json ausente ou ilegível";
            ip.manifest.id = dirName;
            ip.manifest.name = dirName;
        } else {
            auto r = parseManifest(text);
            if (!r.ok) {
                ip.valid = false;
                ip.error = "manifesto inválido: " + r.error;
                ip.manifest.id = dirName;
                ip.manifest.name = dirName;
            } else if (r.manifest.id != dirName) {
                ip.valid = false;
                ip.error = "a pasta não corresponde ao id do plugin";
                ip.manifest.id = dirName;
                ip.manifest.name = dirName;
            } else {
                ip.manifest = r.manifest;
                if (ip.manifest.type == PluginType::Source) {
                    const std::string lib = ip.manifest.libraryFor(platform_);
                    ip.supported = !lib.empty() && fs::exists(de.path() / pathFromUtf8(lib), e2);
                    if (!ip.supported) ip.error = "este plugin não tem versão para " + platform_;
                }
            }
        }
        if (present.count(ip.manifest.id)) continue;
        present.insert(ip.manifest.id);
        ip.enabled = ip.valid && ip.supported && enabled_.count(ip.manifest.id) != 0;
        ip.loaded = loadedBefore.count(ip.manifest.id) != 0;
        items_.push_back(std::move(ip));
    }
    std::sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.manifest.name < b.manifest.name; });
    // forget enabled ids whose plugin vanished or became invalid
    for (auto it = enabled_.begin(); it != enabled_.end();) {
        const auto* p = findLocked(*it);
        if (!p || !p->valid || !p->supported) it = enabled_.erase(it);
        else ++it;
    }
    ++version_;
}

std::vector<InstalledPlugin> PluginManager::list() const {
    std::lock_guard lock(mutex_);
    return items_;
}

bool PluginManager::find(const std::string& id, InstalledPlugin& out) const {
    std::lock_guard lock(mutex_);
    const auto* p = findLocked(id);
    if (!p) return false;
    out = *p;
    return true;
}

unsigned PluginManager::version() const {
    std::lock_guard lock(mutex_);
    return version_;
}

Review PluginManager::inspect(const std::string& packagePath) const {
    Review r;
    auto pr = inspectPackage(packagePath, platform_);
    if (!pr.ok) {
        r.error = pr.error;
        return r;
    }
    if (pr.info.manifest.type == PluginType::Source && !pr.info.supportedHere) {
        r.error = "este plugin não tem versão para " + platform_;
        return r;
    }
    r.ok = true;
    r.info = pr.info;
    InstalledPlugin cur;
    if (find(pr.info.manifest.id, cur)) {
        r.replaces = true;
        r.installedVersion = cur.manifest.version;
    }
    return r;
}

Result PluginManager::install(const std::string& packagePath, const std::string& expectedSha) {
    Result res;
    std::vector<std::uint8_t> bytes;
    std::string err;
    if (!readFileBytes(packagePath, kMaxPackageBytes, bytes, err)) {
        res.message = err;
        return res;
    }
    std::lock_guard lock(mutex_);
    const fs::path root = pathFromUtf8(root_);
    std::error_code ec;
    fs::create_directories(root / ".staging", ec);
    const fs::path staging = root / ".staging" / randomToken();

    PackageInfo info;
    if (!extractPackageBytes(bytes, platform_, expectedSha, pathToUtf8(staging), info, err)) {
        fs::remove_all(staging, ec);
        res.message = err;
        return res;
    }
    if (info.manifest.type == PluginType::Source && !info.supportedHere) {
        fs::remove_all(staging, ec);
        res.message = "este plugin não tem versão para " + platform_;
        return res;
    }
    const std::string id = info.manifest.id;
    const fs::path finalDir = root / pathFromUtf8(id);
    if (const auto* cur = findLocked(id); cur && cur->loaded) {
        fs::remove_all(staging, ec);
        res.message = "o plugin está em uso: feche e reabra o HB Studio e instale a atualização de novo";
        return res;
    }
    fs::path old;
    if (fs::exists(finalDir, ec)) {
        old = root / ".staging" / (randomToken() + "-old");
        fs::rename(finalDir, old, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            res.message = "não foi possível substituir a versão instalada (arquivo em uso?)";
            return res;
        }
    }
    fs::rename(staging, finalDir, ec);
    if (ec) {
        if (!old.empty()) fs::rename(old, finalDir, ec);   // roll back
        fs::remove_all(staging, ec);
        res.message = "não foi possível concluir a instalação";
        return res;
    }
    if (!old.empty()) fs::remove_all(old, ec);
    enabled_.erase(id);               // new code always starts disabled
    saveStateLocked();
    // refresh the in-memory list entry without dropping runtime flags of other plugins
    InstalledPlugin ip;
    ip.manifest = info.manifest;
    ip.dir = pathToUtf8(finalDir);
    ip.supported = info.manifest.type != PluginType::Source || info.supportedHere;
    if (auto* cur = findLocked(id)) *cur = ip;
    else items_.push_back(ip);
    std::sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.manifest.name < b.manifest.name; });
    ++version_;
    res.ok = true;
    res.message = "\"" + info.manifest.name + "\" instalado. Ele está desativado: ative-o quando quiser usar.";
    return res;
}

Result PluginManager::setEnabled(const std::string& id, bool on) {
    Result res;
    std::lock_guard lock(mutex_);
    auto* p = findLocked(id);
    if (!p || !p->valid) {
        res.message = "plugin não encontrado ou inválido";
        return res;
    }
    if (p->pendingRemoval) {
        res.message = "este plugin foi removido e some ao reiniciar";
        return res;
    }
    if (on) {
        if (!p->supported) {
            res.message = p->error.empty() ? "plugin não suportado neste sistema" : p->error;
            return res;
        }
        if (p->manifest.type == PluginType::Source) {   // refuse tampered libraries before anything loads them
            const std::string lib = p->manifest.libraryFor(platform_);
            const auto want = p->manifest.sha256.find(lib);
            std::string have;
            if (want == p->manifest.sha256.end() || !hashFile(pathFromUtf8(p->dir) / pathFromUtf8(lib), have) || have != want->second) {
                res.message = "a biblioteca do plugin não confere com o sha256 do manifesto: ativação recusada";
                return res;
            }
        } else {                                          // one theme at a time
            for (auto& q : items_)
                if (q.manifest.type == PluginType::Theme && q.manifest.id != id && q.enabled) {
                    q.enabled = false;
                    enabled_.erase(q.manifest.id);
                }
        }
        p->enabled = true;
        enabled_.insert(id);
        p->error.clear();
    } else {
        p->enabled = false;
        enabled_.erase(id);
    }
    saveStateLocked();
    ++version_;
    res.ok = true;
    res.message = on ? "plugin ativado" : (p->loaded ? "plugin desativado (a biblioteca é descarregada ao reiniciar)" : "plugin desativado");
    return res;
}

Result PluginManager::remove(const std::string& id) {
    Result res;
    std::lock_guard lock(mutex_);
    auto* p = findLocked(id);
    if (!p) {
        res.message = "plugin não encontrado";
        return res;
    }
    std::error_code ec;
    enabled_.erase(id);
    p->enabled = false;
    if (p->loaded) {   // the OS will not let us delete a loaded DLL: finish at the next start
        std::ofstream(pathFromUtf8(p->dir) / ".pending-delete") << "1";
        p->pendingRemoval = true;
        res.message = "plugin removido; os arquivos são apagados ao reiniciar o HB Studio";
    } else {
        fs::remove_all(pathFromUtf8(p->dir), ec);
        if (ec) {
            std::ofstream(pathFromUtf8(p->dir) / ".pending-delete") << "1";
            p->pendingRemoval = true;
            res.message = "plugin removido; os arquivos são apagados ao reiniciar o HB Studio";
        } else {
            items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const auto& q) { return q.manifest.id == id; }), items_.end());
            res.message = "plugin removido";
        }
    }
    saveStateLocked();
    ++version_;
    res.ok = true;
    return res;
}

std::map<std::string, std::string> PluginManager::activeThemeVars() const {
    std::lock_guard lock(mutex_);
    for (const auto& p : items_)
        if (p.enabled && p.valid && p.manifest.type == PluginType::Theme) return p.manifest.themeVars;
    return {};
}
std::string PluginManager::activeColorScheme() const {
    std::lock_guard lock(mutex_);
    for (const auto& p : items_)
        if (p.enabled && p.valid && p.manifest.type == PluginType::Theme) return p.manifest.colorScheme;
    return {};
}
std::string PluginManager::activeThemeId() const {
    std::lock_guard lock(mutex_);
    for (const auto& p : items_)
        if (p.enabled && p.valid && p.manifest.type == PluginType::Theme) return p.manifest.id;
    return {};
}

bool PluginManager::verifiedLibraryPath(const std::string& id, std::string& pathOut, std::string& err) const {
    std::lock_guard lock(mutex_);
    const auto* p = findLocked(id);
    if (!p || !p->valid || p->manifest.type != PluginType::Source || !p->supported) {
        err = "plugin não encontrado ou sem biblioteca para este sistema";
        return false;
    }
    const std::string lib = p->manifest.libraryFor(platform_);
    const auto want = p->manifest.sha256.find(lib);
    const fs::path path = pathFromUtf8(p->dir) / pathFromUtf8(lib);
    std::string have;
    if (want == p->manifest.sha256.end() || !hashFile(path, have) || have != want->second) {
        err = "a biblioteca não confere com o sha256 do manifesto (arquivo alterado?)";
        return false;
    }
    pathOut = pathToUtf8(path);
    return true;
}

std::vector<std::string> PluginManager::enabledNativeIds() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    for (const auto& p : items_)
        if (p.enabled && p.valid && p.supported && p.manifest.type == PluginType::Source) ids.push_back(p.manifest.id);
    return ids;
}

void PluginManager::markLoaded(const std::string& id, bool loaded) {
    std::lock_guard lock(mutex_);
    if (auto* p = findLocked(id)) {
        p->loaded = loaded;
        ++version_;
    }
}

void PluginManager::setRuntimeError(const std::string& id, const std::string& message) {
    std::lock_guard lock(mutex_);
    if (auto* p = findLocked(id)) {
        p->error = message;
        if (!message.empty()) {
            p->enabled = false;       // a plugin that failed to load is switched off
            enabled_.erase(id);
            saveStateLocked();
        }
        ++version_;
    }
}

}  // namespace hb::plugins
