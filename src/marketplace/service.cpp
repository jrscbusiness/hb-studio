#include "marketplace/service.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "core/json.hpp"
#include "core/sha256.hpp"

namespace hb::marketplace {

namespace fs = std::filesystem;
using core::json::escape;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string newName() {
    std::random_device rd;
    char b[40];
    std::snprintf(b, sizeof b, "hbm-%08x%08x", rd(), rd());
    return b;
}

bool readCapped(const fs::path& p, std::string& out, std::size_t cap) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(cap + 1, '\0');
    f.read(out.data(), static_cast<std::streamsize>(cap + 1));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out.size() <= cap;
}

void jsonList(std::ostringstream& o, const std::vector<std::string>& v) {
    o << "[";
    for (std::size_t i = 0; i < v.size(); ++i) o << (i ? "," : "") << "\"" << escape(v[i]) << "\"";
    o << "]";
}

InstallOutcome fail(const std::string& msg) {
    InstallOutcome o;
    o.message = msg;
    return o;
}

}  // namespace

MarketplaceService::MarketplaceService(IHttpDownloader& http, MarketplaceConfig config, PluginHooks hooks)
    : http_(http), cfg_(std::move(config)), hooks_(std::move(hooks)) {
    if (!isSafeHttpsUrl(cfg_.catalogUrl)) cfg_.catalogUrl = kDefaultCatalogUrl;
}

bool MarketplaceService::setCatalogUrl(const std::string& url) {
    std::lock_guard lock(mutex_);
    if (url.empty()) {
        cfg_.catalogUrl = kDefaultCatalogUrl;
        return true;
    }
    if (!isSafeHttpsUrl(url)) return false;
    cfg_.catalogUrl = url;
    return true;
}

std::string MarketplaceService::catalogUrl() const {
    std::lock_guard lock(mutex_);
    return cfg_.catalogUrl;
}

bool MarketplaceService::loadCache(const std::string& url, std::string& body, std::string& etag) const {
    if (cfg_.cacheDirUtf8.empty()) return false;
    const fs::path dir = fromUtf8(cfg_.cacheDirUtf8);
    std::string meta;
    if (!readCapped(dir / "index.meta", meta, 4096) || !readCapped(dir / "index.json", body, kMaxCatalogBytes)) return false;
    const std::size_t nl = meta.find('\n');
    if (nl == std::string::npos || meta.substr(0, nl) != url) return false;   // cache belongs to another catalog URL
    etag = meta.substr(nl + 1);
    while (!etag.empty() && (etag.back() == '\n' || etag.back() == '\r')) etag.pop_back();
    return true;
}

void MarketplaceService::saveCache(const std::string& url, const std::string& body, const std::string& etag) const {
    if (cfg_.cacheDirUtf8.empty()) return;
    std::error_code ec;
    const fs::path dir = fromUtf8(cfg_.cacheDirUtf8);
    fs::create_directories(dir, ec);
    auto writeAtomic = [&](const char* name, const std::string& data) {
        fs::path tmp = dir / (std::string(name) + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        std::error_code e2;
        fs::remove(dir / name, e2);
        fs::rename(tmp, dir / name, e2);
        if (e2) fs::remove(tmp, e2);
    };
    // Body first, meta last: a crash in between leaves a cache that fails the url/etag match, never a wrong ETag.
    std::string safeEtag;
    for (unsigned char c : etag)
        if (c >= 0x20 && c < 0x7F) safeEtag += static_cast<char>(c);
    fs::remove(dir / "index.meta", ec);
    writeAtomic("index.json", body);
    writeAtomic("index.meta", url + "\n" + safeEtag + "\n");
}

void MarketplaceService::refresh() {
    std::string url;
    {
        std::lock_guard lock(mutex_);
        url = cfg_.catalogUrl;
    }
    std::string cBody, cEtag;
    Catalog cached;
    bool cacheOk = false;
    if (loadCache(url, cBody, cEtag)) {
        ParseResult pr = parseCatalog(cBody);   // the disk is not trusted either
        if (pr.ok) {
            cached = std::move(pr.catalog);
            cacheOk = true;
        }
    }

    FetchRequest rq;
    rq.url = url;
    rq.maxBytes = kMaxCatalogBytes;
    if (cacheOk) rq.ifNoneMatch = cEtag;
    const FetchResponse resp = http_.fetch(rq);

    State st;
    auto useCache = [&](bool offline, const std::string& msg) {
        st.loaded = cacheOk;
        st.fromCache = cacheOk;
        st.offline = offline;
        st.error = msg;
        if (cacheOk) st.catalog = cached;
    };
    if (!resp.transportOk) {
        useCache(true, cacheOk ? "Sem conexão com o catálogo; mostrando a última cópia salva."
                               : "Não foi possível acessar o catálogo (" + resp.error + "). Verifique sua conexão com a internet.");
    } else if (resp.status == 304 && cacheOk) {
        st.loaded = true;
        st.fromCache = true;
        st.catalog = cached;
    } else if (resp.status == 200) {
        ParseResult pr = parseCatalog(resp.body);
        if (pr.ok) {
            st.loaded = true;
            st.catalog = std::move(pr.catalog);
            saveCache(url, resp.body, resp.etag);
        } else {
            useCache(false, "O catálogo recebido é inválido (" + pr.error + ")" + (cacheOk ? "; usando a cópia anterior." : "."));
        }
    } else {
        useCache(true, "O servidor do catálogo respondeu HTTP " + std::to_string(resp.status) + (cacheOk ? "; mostrando a última cópia salva." : "."));
    }
    std::lock_guard lock(mutex_);
    state_ = std::move(st);
}

std::string MarketplaceService::catalogJson() const {
    State st;
    std::string url;
    {
        std::lock_guard lock(mutex_);
        st = state_;
        url = cfg_.catalogUrl;
    }
    const std::map<std::string, std::string> installed = hooks_.installedVersions ? hooks_.installedVersions() : std::map<std::string, std::string>{};
    std::ostringstream o;
    o << "{\"t\":\"mkt_catalog\",\"loaded\":" << (st.loaded ? "true" : "false") << ",\"offline\":" << (st.offline ? "true" : "false")
      << ",\"fromCache\":" << (st.fromCache ? "true" : "false") << ",\"error\":\"" << escape(st.error) << "\",\"dropped\":" << st.catalog.dropped
      << ",\"installing\":" << (installing_ ? "true" : "false") << ",\"catalogUrl\":\"" << escape(url) << "\",\"defaultUrl\":\"" << kDefaultCatalogUrl
      << "\",\"list\":[";
    bool first = true;
    for (const auto& e : st.catalog.entries) {
        const Selection sel = selectVersion(e, cfg_.appVersion, cfg_.platform);
        const auto it = installed.find(e.id);
        const std::string instVer = it == installed.end() ? std::string() : it->second;
        const InstallStatus status = it == installed.end() ? InstallStatus::Available : installStatus(sel, instVer.empty() ? "0.0.0" : instVer);
        if (!first) o << ",";
        first = false;
        o << "{\"id\":\"" << escape(e.id) << "\",\"name\":\"" << escape(e.name) << "\",\"author\":\"" << escape(e.author) << "\",\"desc\":\""
          << escape(e.descriptionPt.empty() ? e.descriptionEn : e.descriptionPt) << "\",\"cat\":\"" << categoryName(e.category) << "\",\"type\":\""
          << packageTypeName(e.type) << "\",\"native\":" << (e.native() ? "true" : "false") << ",\"verified\":" << (e.verified ? "true" : "false")
          << ",\"license\":\"" << escape(e.license) << "\",\"homepage\":\"" << escape(e.homepage) << "\",\"repository\":\"" << escape(e.repository)
          << "\",\"tags\":";
        jsonList(o, e.tags);
        o << ",\"screenshots\":";
        jsonList(o, e.screenshots);
        o << ",\"permissions\":";
        jsonList(o, e.permissions);
        o << ",\"compatible\":" << (sel.version ? "true" : "false") << ",\"reason\":\"" << escape(sel.reason) << "\",\"version\":\""
          << escape(sel.version ? sel.version->version : std::string()) << "\",\"size\":" << (sel.version ? sel.version->size : 0)
          << ",\"minApp\":\"" << escape(sel.version ? sel.version->minAppVersion : std::string()) << "\",\"notes\":\""
          << escape(sel.version ? sel.version->notes : std::string()) << "\",\"released\":\""
          << escape(sel.version ? sel.version->releasedAt : std::string()) << "\",\"status\":\"" << installStatusName(status)
          << "\",\"installedVersion\":\"" << escape(instVer) << "\"}";
    }
    o << "]}";
    return o.str();
}

InstallOutcome MarketplaceService::install(const std::string& id, const ProgressCb& progress) {
    bool expected = false;
    if (!installing_.compare_exchange_strong(expected, true)) return fail("Já existe uma instalação em andamento.");
    struct Guard {
        std::atomic<bool>& f;
        ~Guard() { f = false; }
    } guard{installing_};
    cancel_ = false;
    auto report = [&](const char* phase, int pct) {
        if (progress) progress(phase, pct);
    };

    CatalogEntry entry;
    bool found = false;
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : state_.catalog.entries)
            if (e.id == id) {
                entry = e;
                found = true;
                break;
            }
    }
    if (!found) return fail("Este plugin não está no catálogo atual. Atualize a lista e tente de novo.");
    const Selection sel = selectVersion(entry, cfg_.appVersion, cfg_.platform);
    if (!sel.version) return fail("Este plugin não pode ser instalado: " + sel.reason + ".");
    const VersionEntry ver = *sel.version;
    if (!hooks_.beginInstall) return fail("Instalação indisponível.");

    // Temp area: only the newest review is ever valid, so older downloads can go.
    std::error_code ec;
    const fs::path dir = fromUtf8(cfg_.downloadDirUtf8);
    if (cfg_.downloadDirUtf8.empty()) return fail("Pasta temporária indisponível.");
    fs::create_directories(dir, ec);
    if (ec) return fail("Não foi possível criar a pasta temporária de download.");
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string ext = toUtf8(de.path().extension());
        if (ext == ".hbplugin" || ext == ".part") {
            std::error_code e2;
            fs::remove(de.path(), e2);
        }
    }
    const fs::path dest = dir / (newName() + ".hbplugin");   // the name never comes from the catalog or the URL
    const std::string destUtf8 = toUtf8(dest);
    auto discard = [&] {
        std::error_code e2;
        fs::remove(dest, e2);
    };

    bool downloaded = false;
    std::string lastErr = "nenhum endereço de download";
    int lastPct = -1;
    for (const auto& url : ver.downloadUrls) {
        FileRequest fr;
        fr.url = url;
        fr.destPathUtf8 = destUtf8;
        fr.maxBytes = ver.size;   // the catalog's declared size is a hard cap
        const FileResult fres = http_.downloadToFile(fr, [&](std::uint64_t got, std::uint64_t total) {
            if (cancel_) return false;
            const int pct = total ? static_cast<int>(std::min<std::uint64_t>(100, got * 100 / total)) : 0;
            if (pct != lastPct) {
                lastPct = pct;
                report("download", pct);
            }
            return true;
        });
        if (fres.ok) {
            downloaded = true;
            break;
        }
        discard();
        if (fres.cancelled || cancel_) return fail("Download cancelado.");
        lastErr = fres.error.empty() ? "erro desconhecido" : fres.error;
    }
    if (!downloaded) return fail("Falha no download: " + lastErr + ".");

    if (cancel_) {
        discard();
        return fail("Download cancelado.");
    }
    // ---- integrity: size, then SHA-256, both pinned by the catalog ----
    report("verify", 100);
    const auto actualSize = fs::file_size(dest, ec);
    if (ec || actualSize != ver.size) {
        discard();
        return fail("O tamanho do arquivo baixado não confere com o catálogo. Instalação cancelada.");
    }
    {
        std::ifstream f(dest, std::ios::binary);
        core::Sha256 h;
        std::vector<char> buf(64 * 1024);
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (f.gcount() > 0) h.update(buf.data(), static_cast<std::size_t>(f.gcount()));
        }
        if (h.hexDigest() != ver.sha256) {
            discard();
            return fail("A assinatura SHA-256 do arquivo baixado não confere com a do catálogo. Instalação cancelada por segurança.");
        }
    }

    // ---- hand over to the existing review/consent flow (never bypassed) ----
    report("review", 100);
    const std::string review = hooks_.beginInstall(destUtf8);
    core::json::Value rv;
    if (!core::json::parse(review, rv) || !rv.isObj() || !rv.get("ok") || !rv.get("ok")->isBool()) {
        discard();
        return fail("O pacote baixado não pôde ser revisado.");
    }
    if (!rv.get("ok")->b) {
        discard();
        InstallOutcome o;
        o.message = "O pacote baixado é inválido.";
        o.reviewJson = review;   // lets the UI show the plugin system's own error in the dialog
        return o;
    }
    auto str = [&](const char* k) {
        const auto* v = rv.get(k);
        return v && v->isStr() ? v->s : std::string();
    };
    const std::string token = str("token");
    if (str("id") != entry.id || str("type") != packageTypeName(entry.type) || str("version") != ver.version ||
        str("sha") != ver.sha256.substr(0, 16)) {
        if (hooks_.cancelInstall) hooks_.cancelInstall(token);
        discard();
        return fail("O conteúdo do pacote não corresponde ao que o catálogo anunciava (id, tipo ou versão diferentes). Instalação cancelada.");
    }
    InstallOutcome o;
    o.ok = true;
    o.reviewJson = review;
    o.contextJson = std::string("{\"t\":\"mkt_review\",\"id\":\"") + escape(entry.id) + "\",\"verified\":" + (entry.verified ? "true" : "false") +
                    ",\"native\":" + (entry.native() ? "true" : "false") + ",\"homepage\":\"" + escape(entry.homepage) + "\"}";
    return o;
}

}  // namespace hb::marketplace
