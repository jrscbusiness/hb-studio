#include "marketplace/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "core/json.hpp"
#include "marketplace/semver.hpp"

namespace hb::marketplace {

using core::json::Value;

namespace {

constexpr std::size_t kMaxWarnings = 25;
constexpr std::size_t kMaxVersions = 50;

bool hasControl(std::string_view s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool allDigitsAndDots(std::string_view s) {
    for (char c : s)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) return false;
    return true;
}

bool validHex64(std::string_view h) {
    if (h.size() != 64) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool validPlatform(std::string_view p) {
    if (p.size() < 3 || p.size() > 24) return false;
    for (char c : p)
        if (!(isLowerAlnum(c) || c == '-')) return false;
    return true;
}

// Reads an optional/required string with a length cap, no control characters, valid UTF-8.
bool getStr(const Value& o, const char* key, std::size_t maxLen, bool required, std::string& out, std::string& err) {
    const Value* v = o.get(key);
    if (!v) {
        if (required) err = std::string("campo ausente: ") + key;
        return !required;
    }
    if (!v->isStr() || v->s.size() > maxLen || (required && v->s.empty()) || hasControl(v->s) || !isValidUtf8(v->s)) {
        err = std::string("valor inválido em: ") + key;
        return false;
    }
    out = v->s;
    return true;
}

bool getStrList(const Value& o, const char* key, std::size_t maxItems, std::size_t maxLen, bool urls, std::vector<std::string>& out,
                std::string& err, bool required = false) {
    const Value* v = o.get(key);
    if (!v) {
        if (required) err = std::string("campo ausente: ") + key;
        return !required;
    }
    if (!v->isArr() || v->a.size() > maxItems || (required && v->a.empty())) {
        err = std::string("lista inválida em: ") + key;
        return false;
    }
    for (const auto& item : v->a) {
        if (!item.isStr() || item.s.empty() || item.s.size() > maxLen || hasControl(item.s) || !isValidUtf8(item.s) ||
            (urls && !isSafeHttpsUrl(item.s))) {
            err = std::string("item inválido em: ") + key;
            return false;
        }
        out.push_back(item.s);
    }
    return true;
}

bool parseVersion(const Value& v, VersionEntry& out, std::string& err) {
    if (!v.isObj()) { err = "versão não é um objeto"; return false; }
    SemVer sv;
    if (!getStr(v, "version", 64, true, out.version, err)) return false;
    if (!parseSemVer(out.version, sv)) { err = "versão inválida (use MAJOR.MINOR.PATCH)"; return false; }
    std::string min;
    if (!getStr(v, "minAppVersion", 64, false, min, err)) return false;
    if (!min.empty()) {
        if (!parseSemVer(min, sv)) { err = "minAppVersion inválida"; return false; }
        out.minAppVersion = min;
    }
    if (!getStrList(v, "platforms", 8, 24, false, out.platforms, err, true)) return false;
    for (const auto& p : out.platforms)
        if (!validPlatform(p)) { err = "plataforma inválida"; return false; }
    if (!getStrList(v, "downloadUrls", 4, 2048, true, out.downloadUrls, err, true)) return false;
    if (!getStr(v, "sha256", 64, true, out.sha256, err)) return false;
    if (!validHex64(out.sha256)) { err = "sha256 inválido (64 hex minúsculos)"; return false; }
    const Value* sz = v.get("size");
    if (!sz || !sz->isNum() || !std::isfinite(sz->n) || sz->n < 1 || sz->n > static_cast<double>(kMaxPackageBytes) || sz->n != std::floor(sz->n)) {
        err = "size inválido (inteiro de 1 a 64 MB)";
        return false;
    }
    out.size = static_cast<std::uint64_t>(sz->n);
    if (!getStr(v, "releasedAt", 32, false, out.releasedAt, err)) return false;
    if (!getStr(v, "notes", 300, false, out.notes, err)) return false;
    return true;
}

bool parseEntry(const Value& v, CatalogEntry& e, std::string& err) {
    if (!v.isObj()) { err = "entrada não é um objeto"; return false; }
    if (!getStr(v, "id", 64, true, e.id, err)) return false;
    if (!validCatalogId(e.id)) { err = "id inválido"; return false; }
    if (!getStr(v, "name", 80, true, e.name, err) || !getStr(v, "author", 80, true, e.author, err)) return false;

    const Value* d = v.get("description");
    if (!d || !d->isObj()) { err = "description deve ser {\"pt-BR\":..., \"en\":...}"; return false; }
    if (!getStr(*d, "pt-BR", 600, false, e.descriptionPt, err) || !getStr(*d, "en", 600, false, e.descriptionEn, err)) return false;
    if (e.descriptionPt.empty() && e.descriptionEn.empty()) { err = "description vazia"; return false; }

    std::string cat, type;
    if (!getStr(v, "category", 24, true, cat, err) || !getStr(v, "type", 16, true, type, err)) return false;
    if (cat == "theme") e.category = Category::Theme;
    else if (cat == "video-source") e.category = Category::VideoSource;
    else if (cat == "audio-effect") e.category = Category::AudioEffect;
    else if (cat == "graphic") e.category = Category::Graphic;
    else if (cat == "template") e.category = Category::Template;
    else { err = "category desconhecida"; return false; }
    if (type == "theme") e.type = PackageType::Theme;
    else if (type == "source") e.type = PackageType::Source;
    else if (type == "effect") e.type = PackageType::Effect;
    else { err = "type desconhecido"; return false; }
    // theme <-> theme, video-source <-> source, audio-effect <-> effect; graphic/template ride on any package type.
    if ((e.category == Category::Theme && e.type != PackageType::Theme) ||
        (e.category == Category::VideoSource && e.type != PackageType::Source) ||
        (e.category == Category::AudioEffect && e.type != PackageType::Effect)) {
        err = "category e type incompatíveis";
        return false;
    }

    if (!getStrList(v, "tags", 8, 24, false, e.tags, err)) return false;
    if (!getStr(v, "license", 64, true, e.license, err)) return false;
    if (!getStr(v, "homepage", 2048, false, e.homepage, err) || !getStr(v, "repository", 2048, false, e.repository, err)) return false;
    if ((!e.homepage.empty() && !isSafeHttpsUrl(e.homepage)) || (!e.repository.empty() && !isSafeHttpsUrl(e.repository))) {
        err = "homepage/repository precisam ser URLs https";
        return false;
    }
    if (const Value* ver = v.get("verified")) {
        if (!ver->isBool()) { err = "verified deve ser booleano"; return false; }
        e.verified = ver->b;
    }
    if (!getStrList(v, "screenshots", 6, 2048, true, e.screenshots, err)) return false;

    if (!v.get("permissions")) { err = "campo ausente: permissions"; return false; }
    if (!getStrList(v, "permissions", 8, 24, false, e.permissions, err)) return false;
    {
        static const std::set<std::string> known{"native-code", "network", "filesystem", "hardware-capture"};
        bool declaresNative = false;
        for (const auto& p : e.permissions) {
            if (!known.count(p)) { err = "permissão desconhecida"; return false; }
            if (p == "native-code") declaresNative = true;
        }
        if (declaresNative != e.native()) { err = "permissions inconsistente com type (código nativo deve ser declarado)"; return false; }
    }

    const Value* vs = v.get("versions");
    if (!vs || !vs->isArr() || vs->a.empty() || vs->a.size() > kMaxVersions) { err = "versions deve ter de 1 a 50 itens"; return false; }
    std::set<std::string> seen;
    for (const auto& item : vs->a) {
        VersionEntry ve;
        if (!parseVersion(item, ve, err)) return false;
        if (!seen.insert(ve.version).second) { err = "versão duplicada"; return false; }
        e.versions.push_back(std::move(ve));
    }
    return true;
}

}  // namespace

bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = 0;
        unsigned cp = 0;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + static_cast<std::size_t>(n) >= s.size()) return false;
        for (int k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += static_cast<std::size_t>(n) + 1;
    }
    return true;
}

bool validCatalogId(std::string_view id) {
    if (id.size() < 3 || id.size() > 64) return false;
    if (!isLowerAlnum(id.front()) || !isLowerAlnum(id.back())) return false;
    for (char c : id)
        if (!(isLowerAlnum(c) || c == '.' || c == '-' || c == '_')) return false;
    if (id.find("..") != std::string_view::npos) return false;
    // Windows device names as first dot-separated part ("con.example")
    std::string first(id.substr(0, id.find('.')));
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                     "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : reserved)
        if (first == r) return false;
    return true;
}

bool isSafeHttpsUrl(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() <= kScheme.size() || url.size() > 2048) return false;
    for (std::size_t i = 0; i < kScheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != kScheme[i]) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7F || c == '\\' || c == '"' || c == '<' || c == '>' || c == '`') return false;
    std::string_view rest = url.substr(kScheme.size());
    const std::size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('[') != std::string_view::npos) return false;
    std::string_view host = authority;
    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        const std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return false;
        for (char c : port)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        if (std::stoi(std::string(port)) < 1 || std::stoi(std::string(port)) > 65535) return false;
    }
    if (host.empty() || host.size() > 253 || host.find('.') == std::string_view::npos) return false;   // also rejects "localhost"
    if (allDigitsAndDots(host)) return false;                                                           // IPv4 literal
    std::size_t labelLen = 0;
    for (char c : host) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.';
        if (!ok) return false;
        if (c == '.') {
            if (labelLen == 0) return false;
            labelLen = 0;
        } else if (++labelLen > 63) {
            return false;
        }
    }
    if (labelLen == 0) return false;   // trailing dot / empty label
    std::string lower(host);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto ends = [&](const char* suf) {
        const std::string s(suf);
        return lower.size() >= s.size() && lower.compare(lower.size() - s.size(), s.size(), s) == 0;
    };
    return !(ends(".local") || ends(".localhost") || ends(".internal") || ends(".lan"));
}

const char* categoryName(Category c) {
    switch (c) {
        case Category::Theme: return "theme";
        case Category::VideoSource: return "video-source";
        case Category::AudioEffect: return "audio-effect";
        case Category::Graphic: return "graphic";
        case Category::Template: return "template";
    }
    return "theme";
}

const char* packageTypeName(PackageType t) {
    switch (t) {
        case PackageType::Theme: return "theme";
        case PackageType::Source: return "source";
        case PackageType::Effect: return "effect";
    }
    return "theme";
}

ParseResult parseCatalog(std::string_view text) {
    ParseResult r;
    if (text.empty()) { r.error = "catálogo vazio"; return r; }
    if (text.size() > kMaxCatalogBytes) { r.error = "catálogo grande demais"; return r; }
    Value root;
    core::json::Limits lim;
    lim.maxBytes = kMaxCatalogBytes;
    lim.maxDepth = 8;
    lim.maxArray = kMaxCatalogEntries + 64;
    if (!core::json::parse(text, root, lim) || !root.isObj()) { r.error = "o catálogo não é um JSON válido"; return r; }
    const Value* sv = root.get("schemaVersion");
    if (!sv || !sv->isNum() || sv->n != std::floor(sv->n) || sv->n < 1 || sv->n > 1000) { r.error = "catálogo sem schemaVersion válido"; return r; }
    r.catalog.schemaVersion = static_cast<int>(sv->n);
    if (r.catalog.schemaVersion > kSupportedSchemaVersion) {
        r.error = "o catálogo usa um formato mais novo; atualize o HB Studio para usar o Marketplace";
        return r;
    }
    std::string ignored;
    getStr(root, "generatedAt", 32, false, r.catalog.generatedAt, ignored);
    const Value* list = root.get("plugins");
    if (!list || !list->isArr()) { r.error = "o catálogo não tem a lista \"plugins\""; return r; }
    if (list->a.size() > kMaxCatalogEntries) { r.error = "o catálogo tem entradas demais"; return r; }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < list->a.size(); ++i) {
        CatalogEntry e;
        std::string err;
        if (!parseEntry(list->a[i], e, err)) {
            ++r.catalog.dropped;
            if (r.catalog.warnings.size() < kMaxWarnings) r.catalog.warnings.push_back("entrada " + std::to_string(i + 1) + " ignorada: " + err);
            continue;
        }
        if (!ids.insert(e.id).second) {
            ++r.catalog.dropped;
            if (r.catalog.warnings.size() < kMaxWarnings) r.catalog.warnings.push_back("entrada " + std::to_string(i + 1) + " ignorada: id repetido");
            continue;
        }
        r.catalog.entries.push_back(std::move(e));
    }
    r.ok = true;
    return r;
}

Selection selectVersion(const CatalogEntry& e, std::string_view appVersion, std::string_view platform) {
    Selection sel;
    SemVer app;
    const bool appOk = parseSemVer(appVersion, app);
    const VersionEntry* best = nullptr;
    SemVer bestV;
    bool anyPlatform = false;
    SemVer lowestMin;
    bool haveMin = false;
    std::string lowestMinStr;
    for (const auto& v : e.versions) {
        if (std::find(v.platforms.begin(), v.platforms.end(), platform) == v.platforms.end()) continue;
        SemVer cur, mn;
        if (!parseSemVer(v.version, cur) || !parseSemVer(v.minAppVersion, mn)) continue;
        if (!cur.pre.empty()) continue;                       // prereleases are never offered automatically
        anyPlatform = true;
        if (!haveMin || compareSemVer(mn, lowestMin) < 0) { lowestMin = mn; lowestMinStr = v.minAppVersion; haveMin = true; }
        if (!appOk || compareSemVer(mn, app) > 0) continue;   // fail closed if the app version is unknown
        if (!best || compareSemVer(cur, bestV) > 0) { best = &v; bestV = cur; }
    }
    sel.version = best;
    if (!best) sel.reason = anyPlatform ? "requer o HB Studio " + lowestMinStr + " ou mais novo" : "não disponível para esta plataforma";
    return sel;
}

InstallStatus installStatus(const Selection& sel, std::string_view installedVersion) {
    if (installedVersion.empty()) return InstallStatus::Available;
    if (sel.version && isNewerVersion(sel.version->version, installedVersion)) return InstallStatus::UpdateAvailable;
    return InstallStatus::Installed;
}

const char* installStatusName(InstallStatus s) {
    switch (s) {
        case InstallStatus::Available: return "available";
        case InstallStatus::Installed: return "installed";
        case InstallStatus::UpdateAvailable: return "update";
    }
    return "available";
}

}  // namespace hb::marketplace
