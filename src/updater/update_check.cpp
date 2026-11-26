#include "updater/update_check.hpp"

#include <algorithm>
#include <cctype>

#include "core/json.hpp"
#include "marketplace/catalog.hpp"

namespace hb::updater {
namespace {

bool isHex(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

const core::json::Value* member(const core::json::Value& o, const char* key) {
    if (!o.isObj()) return nullptr;
    return o.get(key);
}

std::string strOf(const core::json::Value& o, const char* key, std::size_t maxLen) {
    const auto* v = member(o, key);
    if (!v || !v->isStr()) return {};
    if (v->s.size() > maxLen || !marketplace::isValidUtf8(v->s)) return {};
    return v->s;
}

// 7..40 hex chars (lowercased), or empty.
std::string asCommit(std::string_view s) {
    if (s.size() < 7 || s.size() > 40 || !isHex(s)) return {};
    return lower(s);
}

// "commit <40 hex>" as written by the CI release notes; used only when target_commitish is a branch name.
std::string commitFromNotes(std::string_view body) {
    const std::string_view key = "commit ";
    std::size_t pos = 0;
    while ((pos = body.find(key, pos)) != std::string_view::npos) {
        pos += key.size();
        std::size_t end = pos;
        while (end < body.size() && end - pos < 40 && std::isxdigit(static_cast<unsigned char>(body[end]))) ++end;
        const std::string c = asCommit(body.substr(pos, end - pos));
        if (!c.empty() && (end >= body.size() || !std::isalnum(static_cast<unsigned char>(body[end])))) return c;
    }
    return {};
}

bool githubHostOk(std::string_view url) {
    if (!marketplace::isSafeHttpsUrl(url)) return false;
    return urlHost(url) == "github.com";
}

}  // namespace

std::string urlHost(std::string_view url) {
    const std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || lower(url.substr(0, scheme.size())) != scheme) return {};
    std::string_view rest = url.substr(scheme.size());
    const std::size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos) authority = authority.substr(0, colon);
    return lower(authority);
}

std::string sanitizeNotes(std::string_view raw, std::size_t maxBytes) {
    std::string out;
    int blank = 0;
    std::string line;
    auto flush = [&] {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) {
            if (out.empty() || ++blank > 1) return;
            out += "\n";
            return;
        }
        blank = 0;
        out += line + "\n";
        line.clear();
    };
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\n') flush();
        else if (c == '\r') continue;
        else if (c == '\t') line += ' ';
        else if (c < 0x20 || c == 0x7F) continue;
        else line += ch;
    }
    flush();
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (!marketplace::isValidUtf8(out)) return {};   // broken encoding: show nothing rather than mojibake
    if (out.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // never split a code point
        out.resize(cut);
        out += "…";
    }
    return out;
}

ParseResult parseRelease(std::string_view json) {
    ParseResult r;
    if (json.empty() || json.size() > kMaxReleaseJsonBytes) { r.error = "resposta do GitHub vazia ou grande demais"; return r; }
    core::json::Value root;
    core::json::Limits lim;
    lim.maxBytes = kMaxReleaseJsonBytes;
    lim.maxDepth = 8;
    lim.maxArray = 256;
    if (!core::json::parse(json, root, lim) || !root.isObj()) { r.error = "resposta do GitHub inválida"; return r; }

    ReleaseInfo& rel = r.release;
    rel.tag = strOf(root, "tag_name", 100);
    rel.name = strOf(root, "name", 200);
    rel.publishedAt = strOf(root, "published_at", 40);
    if (const auto* p = member(root, "prerelease"); p && p->isBool()) rel.prerelease = p->b;
    const std::string body = strOf(root, "body", 16 * 1024);
    rel.notes = sanitizeNotes(body, kMaxNotesBytes);

    rel.targetCommit = asCommit(strOf(root, "target_commitish", 64));
    if (rel.targetCommit.empty()) rel.targetCommit = commitFromNotes(body);

    const auto* assets = member(root, "assets");
    if (assets && assets->isArr()) {
        std::size_t seen = 0;
        for (const auto& a : assets->a) {
            if (++seen > 64) break;
            if (!a.isObj()) continue;
            const std::string name = strOf(a, "name", 200);
            const std::string url = strOf(a, "browser_download_url", 1024);
            if (url.empty() || !githubHostOk(url)) continue;
            if (name == kInstallerAssetName && !rel.hasInstaller) {
                const auto* sz = member(a, "size");
                if (!sz || !sz->isNum() || !(sz->n >= 1) || sz->n > static_cast<double>(kMaxInstallerBytes)) continue;
                rel.installer.name = name;
                rel.installer.url = url;
                rel.installer.size = static_cast<std::uint64_t>(sz->n);
                const std::string digest = strOf(a, "digest", 100);
                if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64 && isHex(std::string_view(digest).substr(7)))
                    rel.installer.sha256 = lower(std::string_view(digest).substr(7));
                rel.hasInstaller = true;
            } else if (name == kChecksumAssetName && rel.checksumUrl.empty()) {
                rel.checksumUrl = url;
            }
        }
    }
    if (rel.tag.empty()) { r.error = "a resposta do GitHub não descreve uma versão"; return r; }
    r.ok = true;
    return r;
}

std::string commitOfVersion(std::string_view version) {
    const std::size_t plus = version.find('+');
    if (plus == std::string_view::npos) return {};
    return asCommit(version.substr(plus + 1));
}

UpdateState compareWithRelease(std::string_view currentVersion, const ReleaseInfo& release) {
    const std::string cur = commitOfVersion(currentVersion);
    if (cur.empty() || release.targetCommit.empty() || !release.hasInstaller) return UpdateState::Unknown;
    const std::string& rel = release.targetCommit;
    const std::size_t n = std::min(cur.size(), rel.size());
    return cur.compare(0, n, rel, 0, n) == 0 ? UpdateState::UpToDate : UpdateState::Available;
}

bool autoCheckDue(bool enabled, std::int64_t now, std::int64_t lastSuccess, std::int64_t lastAttempt) {
    if (!enabled) return false;
    // A clock that went backwards must not freeze the checks forever: treat future timestamps as "never".
    if (lastSuccess > now) lastSuccess = 0;
    if (lastAttempt > now) lastAttempt = 0;
    return now - lastSuccess >= kAutoCheckIntervalSec && now - lastAttempt >= kRetryAfterFailureSec;
}

bool manualCheckAllowed(std::int64_t now, std::int64_t lastAttempt) {
    if (lastAttempt > now) return true;
    return now - lastAttempt >= kManualCooldownSec;
}

std::string parseChecksumFile(std::string_view text, std::string_view assetName) {
    if (text.size() > kMaxChecksumBytes) return {};
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;
        if (line.size() < 64 || !isHex(line.substr(0, 64))) return {};
        std::string_view rest = line.substr(64);
        if (rest.empty()) return lower(line);
        if (rest.front() != ' ' && rest.front() != '\t') return {};
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' || rest.front() == '*')) rest.remove_prefix(1);
        if (rest != assetName) return {};
        return lower(line.substr(0, 64));
    }
    return {};
}

}  // namespace hb::updater
