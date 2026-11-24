#include "teleprompter/store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "teleprompter/script.hpp"

namespace fs = std::filesystem;

namespace hb::teleprompter {

namespace {
bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
}  // namespace

std::string sanitizeName(const std::string& name) {
    std::string out;
    bool lastSpace = true;   // also trims leading blanks
    for (unsigned char c : name) {
        const bool blank = c < 0x20 || c == 0x7F || c == ' ';
        if (blank) {
            if (!lastSpace) out += ' ';
            lastSpace = true;
        } else {
            out += static_cast<char>(c);
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    if (!isValidUtf8(out)) {   // a UI should never send this; keep names printable anyway
        std::string ascii;
        for (unsigned char c : out) ascii += c < 0x80 ? static_cast<char>(c) : '?';
        out = ascii;
    }
    return out.empty() ? std::string("Sem título") : out;
}

bool isValidId(const std::string& id) {
    if (id.size() < 2 || id.size() > 10 || id[0] != 's') return false;
    return std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool ScriptStore::ensureDir() {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    return !ec && fs::is_directory(dir_, ec);
}

bool ScriptStore::writeAtomic(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {   // some platforms refuse to replace: remove then retry once
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        return false;
    }
    return true;
}

std::vector<ScriptStore::Entry> ScriptStore::readIndex() {
    std::vector<Entry> out;
    std::string data;
    if (readFile(dir_ / "index.tsv", data)) {
        std::istringstream in(data);
        std::string line;
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            Entry e{line.substr(0, tab), line.substr(tab + 1)};
            if (!isValidId(e.id) || out.size() >= kMaxScripts) continue;
            std::error_code ec;
            if (!fs::exists(textPath(e.id), ec)) continue;   // stale index line
            e.name = sanitizeName(e.name);
            out.push_back(std::move(e));
        }
    }
    // Adopt .txt files the index does not know (index lost/edited by hand): name = id.
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".txt") continue;
        std::string stem;
        try {
            stem = p.stem().string();   // may throw on Windows for names not representable in the ANSI code page
        } catch (...) {
            continue;
        }
        if (!isValidId(stem) || out.size() >= kMaxScripts) continue;
        const bool known = std::any_of(out.begin(), out.end(), [&](const Entry& e) { return e.id == stem; });
        if (!known) out.push_back({stem, stem});
    }
    return out;
}

bool ScriptStore::writeIndex(const std::vector<Entry>& e) {
    std::string s;
    for (const auto& x : e) s += x.id + "\t" + x.name + "\n";
    return writeAtomic(dir_ / "index.tsv", s);
}

std::string ScriptStore::uniqueName(const std::vector<Entry>& e, const std::string& wanted, const std::string& exceptId) const {
    auto taken = [&](const std::string& n) {
        return std::any_of(e.begin(), e.end(), [&](const Entry& x) { return x.id != exceptId && lowerAscii(x.name) == lowerAscii(n); });
    };
    if (!taken(wanted)) return wanted;
    for (int i = 2; i < 1000; ++i) {
        std::string base = wanted;
        const std::string suffix = " (" + std::to_string(i) + ")";
        if (base.size() + suffix.size() > kMaxNameBytes) {
            std::size_t cut = kMaxNameBytes - suffix.size();
            while (cut > 0 && (static_cast<unsigned char>(base[cut]) & 0xC0) == 0x80) --cut;
            base.resize(cut);
        }
        if (!taken(base + suffix)) return base + suffix;
    }
    return wanted;
}

std::vector<ScriptInfo> ScriptStore::list() {
    std::vector<ScriptInfo> out;
    if (!ensureDir()) return out;
    for (const auto& e : readIndex()) {
        ScriptInfo i;
        i.id = e.id;
        i.name = e.name;
        std::string text;
        if (readFile(textPath(e.id), text)) {
            i.bytes = static_cast<std::int64_t>(text.size());
            i.words = parseScript(text).totalWords;
        }
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const ScriptInfo& a, const ScriptInfo& b) {
        const auto la = lowerAscii(a.name), lb = lowerAscii(b.name);
        return la != lb ? la < lb : a.id < b.id;
    });
    return out;
}

std::string ScriptStore::create(const std::string& name, const std::string& text) {
    if (!ensureDir() || text.size() > kMaxScriptBytes) return {};
    auto idx = readIndex();
    if (idx.size() >= kMaxScripts) return {};
    long maxN = 0;
    for (const auto& e : idx) maxN = std::max(maxN, std::stol(e.id.substr(1)));
    if (maxN >= 999999999L) return {};
    Entry e{"s" + std::to_string(maxN + 1), uniqueName(idx, sanitizeName(name), "")};
    if (!writeAtomic(textPath(e.id), text)) return {};
    idx.push_back(e);
    if (!writeIndex(idx)) {
        std::error_code ec;
        fs::remove(textPath(e.id), ec);
        return {};
    }
    return e.id;
}

std::string ScriptStore::importText(const std::string& name, const std::string& rawText) {
    return create(name, normalizeText(rawText));
}

bool ScriptStore::load(const std::string& id, std::string& out) {
    if (!isValidId(id)) return false;
    return readFile(textPath(id), out);
}

bool ScriptStore::save(const std::string& id, const std::string& text) {
    if (!isValidId(id) || text.size() > kMaxScriptBytes || !exists(id)) return false;
    return writeAtomic(textPath(id), text);
}

bool ScriptStore::exists(const std::string& id) {
    if (!isValidId(id)) return false;
    std::error_code ec;
    return fs::exists(textPath(id), ec);
}

std::string ScriptStore::nameOf(const std::string& id) {
    for (const auto& e : readIndex())
        if (e.id == id) return e.name;
    return {};
}

bool ScriptStore::rename(const std::string& id, const std::string& name) {
    if (!isValidId(id)) return false;
    auto idx = readIndex();
    for (auto& e : idx) {
        if (e.id != id) continue;
        e.name = uniqueName(idx, sanitizeName(name), id);
        return writeIndex(idx);
    }
    return false;
}

bool ScriptStore::remove(const std::string& id) {
    if (!isValidId(id)) return false;
    auto idx = readIndex();
    const auto it = std::find_if(idx.begin(), idx.end(), [&](const Entry& e) { return e.id == id; });
    if (it == idx.end()) return false;
    idx.erase(it);
    std::error_code ec;
    fs::remove(textPath(id), ec);
    return writeIndex(idx);
}

}  // namespace hb::teleprompter
