#include "plugins/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "core/json.hpp"

namespace hb::plugins {

namespace {

bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool hasControl(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Names Windows treats as devices, with or without an extension.
bool reservedWindowsName(const std::string& component) {
    std::string base = lowerAscii(component.substr(0, component.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static const std::set<std::string> r = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                            "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return r.count(base) != 0;
}

bool text(const core::json::Value& o, const char* key, std::size_t maxLen, bool required, std::string& out, std::string& err,
          const char* label) {
    const auto* v = o.get(key);
    if (!v) {
        if (required) err = std::string("campo obrigatório ausente: ") + label;
        return !required;
    }
    if (!v->isStr() || hasControl(v->s) || v->s.size() > maxLen || (required && v->s.empty())) {
        err = std::string("valor inválido em: ") + label;
        return false;
    }
    out = v->s;
    return true;
}

}  // namespace

bool validPluginId(const std::string& id) {
    if (id.size() < 3 || id.size() > 64) return false;
    if (!isLowerAlnum(id.front()) || !isLowerAlnum(id.back())) return false;
    for (char c : id)
        if (!(isLowerAlnum(c) || c == '.' || c == '-' || c == '_')) return false;
    if (id.find("..") != std::string::npos) return false;
    // first dot-separated part must not be a Windows device name ("con.example" would be one)
    return !reservedWindowsName(id);
}

bool validVersion(const std::string& v) {
    // MAJOR.MINOR.PATCH with an optional -prerelease/+build suffix
    std::size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        const std::size_t st = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i == st || i - st > 6) return false;
        if (part < 2) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    if (i == v.size()) return true;
    if (v[i] != '-' && v[i] != '+') return false;
    ++i;
    if (i >= v.size() || v.size() - i > 32) return false;
    for (; i < v.size(); ++i)
        if (!(std::isalnum(static_cast<unsigned char>(v[i])) || v[i] == '.' || v[i] == '-')) return false;
    return true;
}

bool validLibraryFileName(const std::string& f) {
    if (f.empty() || f.size() > 64) return false;
    for (unsigned char c : f)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    if (f.front() == '.' || f.back() == '.' || f.find("..") != std::string::npos) return false;
    if (reservedWindowsName(f)) return false;
    const std::string l = lowerAscii(f);
    auto ends = [&](const char* e) { return l.size() > std::char_traits<char>::length(e) && l.compare(l.size() - std::char_traits<char>::length(e), std::string::npos, e) == 0; };
    return ends(".dll") || ends(".so") || ends(".dylib");
}

bool validSha256Hex(const std::string& h) {
    if (h.size() != 64) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool safeEntryName(const std::string& name, int maxDepth) {
    if (name.empty() || name.size() > 200) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            const std::string comp = name.substr(start, i - start);
            start = i + 1;
            if (i == name.size() && comp.empty()) break;       // trailing '/': directory entry
            if (comp.empty() || comp == "." || comp == "..") return false;
            if (comp.size() > 100 || comp.back() == '.' || comp.back() == ' ') return false;
            if (reservedWindowsName(comp)) return false;
            if (++depth > maxDepth) return false;
        } else {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                return false;
        }
    }
    return depth > 0;
}

bool validThemeVarName(const std::string& n) {
    static const std::set<std::string> allowed = {"--bg", "--panel", "--surface", "--surface-2", "--glass", "--line", "--line-2",
                                                  "--ink", "--ink-2", "--muted", "--blue", "--blue-2", "--sky", "--mist",
                                                  "--navy", "--on-accent", "--warn", "--warn-ink", "--warn-bg", "--warn-line"};
    return allowed.count(n) != 0;
}

bool validThemeColor(const std::string& v) {
    if (v.empty() || v.size() > 48) return false;
    if (v[0] == '#') {
        const std::size_t n = v.size() - 1;
        if (n != 3 && n != 4 && n != 6 && n != 8) return false;
        for (std::size_t i = 1; i < v.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(v[i]))) return false;
        return true;
    }
    for (const char* p : {"rgb(", "rgba("}) {
        const std::string pre = p;
        if (v.compare(0, pre.size(), pre) == 0 && v.back() == ')') {
            for (std::size_t i = pre.size(); i + 1 < v.size(); ++i) {
                const char c = v[i];
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == ',' || c == '.' || c == ' ' || c == '%' || c == '/')) return false;
            }
            return v.size() > pre.size() + 1;
        }
    }
    return false;
}

ManifestResult parseManifest(std::string_view jsonText) {
    ManifestResult r;
    core::json::Value root;
    core::json::Limits lim;
    lim.maxBytes = 64 * 1024;
    lim.maxDepth = 6;
    lim.maxArray = 256;
    if (!core::json::parse(jsonText, root, lim) || !root.isObj()) {
        r.error = "manifest.json inválido ou grande demais";
        return r;
    }
    Manifest m;
    const auto* schema = root.get("schema");
    if (!schema || !schema->isNum() || schema->n != 1) {
        r.error = "versão de esquema do manifesto não suportada (esperado 1)";
        return r;
    }
    std::string err;
    if (!text(root, "id", 64, true, m.id, err, "id") || !text(root, "name", 80, true, m.name, err, "name") ||
        !text(root, "version", 40, true, m.version, err, "version") || !text(root, "author", 80, false, m.author, err, "author") ||
        !text(root, "description", 400, false, m.description, err, "description") ||
        !text(root, "license", 40, false, m.license, err, "license")) {
        r.error = err;
        return r;
    }
    if (!validPluginId(m.id)) { r.error = "id inválido (use a-z, 0-9, '.', '-', '_', de 3 a 64 caracteres)"; return r; }
    if (!validVersion(m.version)) { r.error = "versão inválida (use MAJOR.MINOR.PATCH)"; return r; }

    std::string type;
    if (!text(root, "type", 16, true, type, err, "type")) { r.error = err; return r; }

    if (type == "source" || type == "effect") {
        m.type = type == "source" ? PluginType::Source : PluginType::Effect;
        const auto* api = root.get("apiVersion");
        if (!api || !api->isNum() || api->n != 1) { r.error = "apiVersion não suportada (esperado 1)"; return r; }
        m.apiVersion = 1;
        const auto* libs = root.get("library");
        const auto* sums = root.get("sha256");
        if (!libs || !libs->isObj() || libs->o.empty() || !sums || !sums->isObj()) {
            r.error = "plugin nativo precisa de \"library\" e \"sha256\"";
            return r;
        }
        for (const auto& [plat, f] : libs->o) {
            if (plat.size() > 24 || !f.isStr() || !validLibraryFileName(f.s)) { r.error = "nome de biblioteca inválido"; return r; }
            m.libraries[plat] = f.s;
        }
        for (const auto& [file, h] : sums->o) {
            if (!h.isStr() || !validSha256Hex(h.s) || !validLibraryFileName(file)) { r.error = "hash sha256 inválido"; return r; }
            m.sha256[file] = h.s;
        }
        for (const auto& [plat, f] : m.libraries)
            if (!m.sha256.count(f)) { r.error = "falta o sha256 de uma biblioteca (integridade obrigatória)"; return r; }
    } else if (type == "theme") {
        m.type = PluginType::Theme;
        const auto* th = root.get("theme");
        const auto* vars = th ? th->get("vars") : nullptr;
        if (!th || !th->isObj() || !vars || !vars->isObj() || vars->o.empty()) { r.error = "tema precisa de theme.vars"; return r; }
        for (const auto& [k, v] : vars->o) {
            if (!validThemeVarName(k)) { r.error = "variável de tema não permitida: " + k.substr(0, 40); return r; }
            if (!v.isStr() || !validThemeColor(v.s)) { r.error = "cor inválida em " + k; return r; }
            m.themeVars[k] = v.s;
        }
        if (const auto* cs = th->get("colorScheme")) {
            if (!cs->isStr() || (cs->s != "light" && cs->s != "dark")) { r.error = "colorScheme deve ser \"light\" ou \"dark\""; return r; }
            m.colorScheme = cs->s;
        }
    } else {
        r.error = "tipo de plugin desconhecido (use \"source\", \"effect\" ou \"theme\")";
        return r;
    }
    r.ok = true;
    r.manifest = std::move(m);
    return r;
}

}  // namespace hb::plugins
