#include "updater/update_service.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "core/json.hpp"
#include "core/sha256.hpp"
#include "marketplace/catalog.hpp"

namespace hb::updater {
namespace {

namespace fs = std::filesystem;
namespace js = core::json;

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8Of(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

js::Value jStr(const std::string& s) { js::Value v; v.t = js::Value::T::Str; v.s = s; return v; }
js::Value jNum(double n) { js::Value v; v.t = js::Value::T::Num; v.n = n; return v; }
js::Value jBool(bool b) { js::Value v; v.t = js::Value::T::Bool; v.b = b; return v; }
js::Value jObj() { js::Value v; v.t = js::Value::T::Obj; return v; }

constexpr const char* kApiHost = "api.github.com";

// Streams the file through SHA-256. Returns false if it cannot be read or is larger than maxBytes.
bool hashFile(const fs::path& p, std::uint64_t maxBytes, std::string& hex, std::uint64_t& size, bool& isPe) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    core::Sha256 h;
    std::vector<char> buf(64 * 1024);
    size = 0;
    isPe = false;
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        if (size == 0 && n >= 2 && buf[0] == 'M' && buf[1] == 'Z') isPe = true;
        size += static_cast<std::uint64_t>(n);
        if (size > maxBytes) return false;
        h.update(buf.data(), static_cast<std::size_t>(n));
    }
    hex = h.hexDigest();
    return true;
}

bool readSmallFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(kMaxChecksumBytes + 1, '\0');
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out.size() <= kMaxChecksumBytes;
}

std::int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

UpdateService::UpdateService(marketplace::IHttpDownloader& http, UpdaterConfig config, NowFn now)
    : http_(http), cfg_(std::move(config)), now_(now ? std::move(now) : NowFn(systemNow)) {}

void UpdateService::load() {
    std::lock_guard lock(m_);
    if (cfg_.dataDirUtf8.empty()) return;
    std::ifstream f(pathFromUtf8(cfg_.dataDirUtf8) / "updates.json", std::ios::binary);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    js::Value v;
    js::Limits lim;
    lim.maxBytes = 64 * 1024;
    if (!js::parse(text, v, lim) || !v.isObj()) return;
    if (const auto* a = v.get("autoCheck"); a && a->isBool()) autoCheck_ = a->b;
    if (const auto* a = v.get("lastCheck"); a && a->isNum() && a->n >= 0 && a->n < 4e12) lastSuccess_ = static_cast<std::int64_t>(a->n);
    if (const auto* a = v.get("dismissed"); a && a->isStr() && a->s.size() <= 40) dismissedCommit_ = a->s;
    if (const auto* a = v.get("releaseUrl"); a && a->isStr() && marketplace::isSafeHttpsUrl(a->s) && urlHost(a->s) == kApiHost) cfg_.releaseUrl = a->s;
}

void UpdateService::saveLocked() const {
    if (cfg_.dataDirUtf8.empty()) return;
    std::error_code ec;
    const fs::path dir = pathFromUtf8(cfg_.dataDirUtf8);
    fs::create_directories(dir, ec);
    js::Value v = jObj();
    v.o["autoCheck"] = jBool(autoCheck_);
    v.o["lastCheck"] = jNum(static_cast<double>(lastSuccess_));
    v.o["dismissed"] = jStr(dismissedCommit_);
    v.o["releaseUrl"] = jStr(cfg_.releaseUrl == kDefaultReleaseUrl ? std::string() : cfg_.releaseUrl);
    const fs::path tmp = dir / "updates.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << js::dump(v, true);
    }
    fs::rename(tmp, dir / "updates.json", ec);
    if (ec) fs::remove(tmp, ec);
}

bool UpdateService::autoCheckEnabled() const {
    std::lock_guard lock(m_);
    return autoCheck_;
}

void UpdateService::setAutoCheck(bool on) {
    std::lock_guard lock(m_);
    autoCheck_ = on;
    saveLocked();
}

bool UpdateService::setReleaseUrl(const std::string& url) {
    std::lock_guard lock(m_);
    if (url.empty()) {
        cfg_.releaseUrl = kDefaultReleaseUrl;
    } else {
        if (!marketplace::isSafeHttpsUrl(url) || urlHost(url) != kApiHost) return false;
        cfg_.releaseUrl = url;
    }
    hasRelease_ = false;   // the old answer belonged to another endpoint
    release_ = {};
    error_.clear();
    saveLocked();
    return true;
}

bool UpdateService::dueForAutoCheck() const {
    std::lock_guard lock(m_);
    return autoCheckDue(autoCheck_, now(), lastSuccess_, lastAttempt_);
}

UpdateState UpdateService::stateLocked() const { return hasRelease_ ? compareWithRelease(cfg_.appVersion, release_) : UpdateState::Unknown; }

UpdateState UpdateService::state() const {
    std::lock_guard lock(m_);
    return stateLocked();
}

void UpdateService::check(bool manual) {
    std::string url;
    {
        std::lock_guard lock(m_);
        if (checking_ || downloading_) return;
        const std::int64_t t = now();
        if (manual && !manualCheckAllowed(t, lastAttempt_)) {
            error_ = "Aguarde alguns segundos antes de verificar de novo.";
            return;
        }
        checking_ = true;
        lastAttempt_ = t;
        url = cfg_.releaseUrl;
    }
    marketplace::FetchRequest rq;
    rq.url = url;
    rq.maxBytes = kMaxReleaseJsonBytes;
    rq.timeout = std::chrono::milliseconds(15000);
    const marketplace::FetchResponse resp = http_.fetch(rq);

    std::lock_guard lock(m_);
    checking_ = false;
    if (!resp.transportOk) {
        error_ = "Não foi possível consultar o GitHub: " + resp.error;
    } else if (resp.status == 403 || resp.status == 429) {
        error_ = "O GitHub limitou as consultas sem login (limite por hora). Tente novamente mais tarde.";
    } else if (resp.status == 404) {
        error_ = "Nenhuma versão publicada foi encontrada (release \"latest\").";
    } else if (resp.status != 200) {
        error_ = "O GitHub respondeu HTTP " + std::to_string(resp.status) + ".";
    } else {
        ParseResult pr = parseRelease(resp.body);
        if (!pr.ok) {
            error_ = pr.error;
        } else {
            // A different release invalidates a previously staged installer.
            if (hasStaged_ && pr.release.targetCommit != staged_.commit) { hasStaged_ = false; phase_ = "idle"; pct_ = 0; }
            release_ = std::move(pr.release);
            hasRelease_ = true;
            lastSuccess_ = now();
            error_.clear();
        }
    }
    saveLocked();
}

void UpdateService::dismiss() {
    std::lock_guard lock(m_);
    if (hasRelease_) dismissedCommit_ = release_.targetCommit;
    saveLocked();
}

StagedInstaller UpdateService::staged() const {
    std::lock_guard lock(m_);
    return hasStaged_ ? staged_ : StagedInstaller{};
}

void UpdateService::discardStaged() {
    std::lock_guard lock(m_);
    if (hasStaged_ && !staged_.pathUtf8.empty()) {
        std::error_code ec;
        fs::remove(pathFromUtf8(staged_.pathUtf8), ec);
    }
    hasStaged_ = false;
    staged_ = {};
    phase_ = "idle";
    pct_ = 0;
}

DownloadOutcome UpdateService::downloadInstaller(const ProgressFn& progress) {
    DownloadOutcome out;
    ReleaseInfo rel;
    fs::path dir;
    {
        std::lock_guard lock(m_);
        auto fail = [&](const std::string& msg) {
            out.error = msg;
            error_ = msg;
            phase_ = "error";
            return out;
        };
        if (downloading_ || checking_) return fail("Já existe uma operação em andamento.");
        if (cfg_.dataDirUtf8.empty()) return fail("Pasta de dados indisponível: não dá para guardar o instalador.");
        if (stateLocked() != UpdateState::Available) return fail("Não há versão nova para baixar. Verifique as atualizações primeiro.");
        rel = release_;
        dir = pathFromUtf8(cfg_.dataDirUtf8) / "downloads";
        downloading_ = true;
        cancel_ = false;
        hasStaged_ = false;
        staged_ = {};
        phase_ = "download";
        pct_ = 0;
        error_.clear();
    }

    auto finish = [&](const std::string& err, bool cancelled = false) {
        std::lock_guard lock(m_);
        downloading_ = false;
        out.cancelled = cancelled;
        if (!err.empty()) {
            out.ok = false;
            out.error = err;
            error_ = cancelled ? std::string() : err;
            phase_ = cancelled ? "idle" : "error";
            pct_ = 0;
        }
        return out;
    };

    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path exe = dir / kInstallerAssetName;
    const fs::path sumFile = dir / kChecksumAssetName;
    for (const fs::path& p : {exe, sumFile}) {
        fs::remove(p, ec);
        fs::path part = p;
        part += ".part";
        fs::remove(part, ec);
    }

    // 1) Published SHA-256: the API "digest" and/or the HBStudio-Setup.exe.sha256 asset. No hash = no auto-update.
    std::string expected = rel.installer.sha256;
    if (!rel.checksumUrl.empty()) {
        marketplace::FileRequest fr;
        fr.url = rel.checksumUrl;
        fr.destPathUtf8 = utf8Of(sumFile);
        fr.maxBytes = kMaxChecksumBytes;
        fr.timeout = std::chrono::milliseconds(30000);
        const marketplace::FileResult fres = http_.downloadToFile(fr, nullptr);
        if (fres.ok) {
            std::string text;
            const std::string fromFile = readSmallFile(sumFile, text) ? parseChecksumFile(text, kInstallerAssetName) : std::string();
            fs::remove(sumFile, ec);
            if (fromFile.empty()) return finish("O arquivo de checksum (.sha256) da versão está inválido; atualização recusada.");
            if (!expected.empty() && expected != fromFile)
                return finish("Os dois SHA-256 publicados para o instalador divergem; atualização recusada.");
            expected = fromFile;
        } else if (expected.empty()) {
            return finish("Não foi possível baixar o checksum da versão: " + fres.error);
        }
    }
    if (expected.empty())
        return finish("Esta versão não publica o SHA-256 do instalador; a atualização automática foi recusada. Baixe manualmente pela página de versões.");

    // 2) The installer itself: size cap = the size the release declares, enforced while receiving.
    marketplace::FileRequest fr;
    fr.url = rel.installer.url;
    fr.destPathUtf8 = utf8Of(exe);
    fr.maxBytes = rel.installer.size;
    fr.timeout = std::chrono::milliseconds(15 * 60 * 1000);
    const marketplace::FileResult res = http_.downloadToFile(fr, [&](std::uint64_t got, std::uint64_t total) {
        const std::uint64_t t = total ? total : rel.installer.size;
        const int pct = t ? static_cast<int>(std::min<std::uint64_t>(99, got * 100 / t)) : 0;
        {
            std::lock_guard lock(m_);
            pct_ = pct;
        }
        if (progress) progress(pct);
        return !cancel_.load();
    });
    if (!res.ok) {
        fs::remove(exe, ec);
        return finish(res.cancelled ? "Download cancelado." : "Falha no download: " + res.error, res.cancelled);
    }
    {
        std::lock_guard lock(m_);
        phase_ = "verify";
    }

    // 3) Verify: exact declared size, PE header, SHA-256 against the published value.
    std::string actual;
    std::uint64_t size = 0;
    bool pe = false;
    if (!hashFile(exe, rel.installer.size, actual, size, pe)) {
        fs::remove(exe, ec);
        return finish("Não foi possível ler o instalador baixado.");
    }
    if (size != rel.installer.size) {
        fs::remove(exe, ec);
        return finish("O tamanho do instalador baixado não confere com o publicado (download incompleto?).");
    }
    if (!pe) {
        fs::remove(exe, ec);
        return finish("O arquivo baixado não é um executável do Windows.");
    }
    if (actual != expected) {
        fs::remove(exe, ec);
        return finish("O SHA-256 do instalador não confere com o publicado (arquivo corrompido ou adulterado). Nada foi executado.");
    }

    std::lock_guard lock(m_);
    downloading_ = false;
    staged_.pathUtf8 = utf8Of(exe);
    staged_.sha256 = actual;
    staged_.size = size;
    staged_.tag = rel.tag;
    staged_.commit = rel.targetCommit;
    staged_.verifiedAgainstPublishedHash = true;
    hasStaged_ = true;
    phase_ = "ready";
    pct_ = 100;
    out.ok = true;
    out.staged = staged_;
    return out;
}

bool UpdateService::verifyStaged(std::string& error) const {
    StagedInstaller s;
    {
        std::lock_guard lock(m_);
        if (!hasStaged_) { error = "Nenhum instalador verificado está disponível."; return false; }
        s = staged_;
    }
    std::string hex;
    std::uint64_t size = 0;
    bool pe = false;
    if (!hashFile(pathFromUtf8(s.pathUtf8), kMaxInstallerBytes, hex, size, pe)) { error = "O instalador baixado não pode mais ser lido."; return false; }
    if (!pe || size != s.size || hex != s.sha256) {
        error = "O instalador baixado mudou depois da verificação; nada foi executado.";
        return false;
    }
    return true;
}

std::string UpdateService::stateJson() const {
    std::lock_guard lock(m_);
    js::Value v = jObj();
    v.o["t"] = jStr("upd_state");
    v.o["enabled"] = jBool(autoCheck_);
    v.o["current"] = jStr(cfg_.appVersion);
    v.o["lastCheck"] = jNum(static_cast<double>(lastSuccess_));
    v.o["checking"] = jBool(checking_.load());
    v.o["releaseUrl"] = jStr(cfg_.releaseUrl);
    v.o["defaultUrl"] = jStr(kDefaultReleaseUrl);
    v.o["error"] = jStr(error_);
    const UpdateState st = stateLocked();
    v.o["state"] = jStr(st == UpdateState::Available ? "available" : st == UpdateState::UpToDate ? "uptodate" : "unknown");
    if (hasRelease_) {
        js::Value r = jObj();
        r.o["tag"] = jStr(release_.tag);
        r.o["commit"] = jStr(release_.targetCommit.substr(0, 7));
        r.o["notes"] = jStr(release_.notes);
        r.o["published"] = jStr(release_.publishedAt);
        r.o["size"] = jNum(static_cast<double>(release_.installer.size));
        r.o["hasInstaller"] = jBool(release_.hasInstaller);
        r.o["hashPublished"] = jBool(!release_.installer.sha256.empty() || !release_.checksumUrl.empty());
        v.o["release"] = std::move(r);
        v.o["dismissed"] = jBool(!dismissedCommit_.empty() && dismissedCommit_ == release_.targetCommit);
    }
    js::Value d = jObj();
    d.o["phase"] = jStr(phase_);
    d.o["pct"] = jNum(pct_);
    v.o["download"] = std::move(d);
    return js::dump(v, false);
}

}  // namespace hb::updater
