#include "rtsp/auth.hpp"

#include <cctype>
#include <cstdio>

#include "rtsp/base64.hpp"
#include "rtsp/md5.hpp"

namespace hb::rtsp {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
// Split on commas outside double quotes.
std::vector<std::string> splitTop(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && q && i + 1 < s.size()) {
            cur += c;
            cur += s[++i];
            continue;
        }
        if (c == '"') q = !q;
        if (c == ',' && !q) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}
std::string unquote(std::string_view v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string o;
        for (std::size_t i = 1; i + 1 < v.size(); ++i) {
            if (v[i] == '\\' && i + 2 < v.size()) ++i;
            o += v[i];
        }
        return o;
    }
    return std::string(v);
}
void applyParam(Challenge& c, std::string_view key, const std::string& val) {
    std::string k = lower(std::string(trim(key)));
    if (k == "realm") c.realm = val;
    else if (k == "nonce") c.nonce = val;
    else if (k == "opaque") c.opaque = val;
    else if (k == "algorithm") c.algorithm = val;
    else if (k == "stale") c.stale = lower(val) == "true";
    else if (k == "qop") {
        std::string t;
        for (char ch : val + ",") {
            if (ch == ',' || ch == ' ') {
                if (!t.empty()) c.qop.push_back(lower(t));
                t.clear();
            } else {
                t += ch;
            }
        }
    }
}
}  // namespace

std::vector<Challenge> parseChallenges(const std::vector<std::string>& values) {
    std::vector<Challenge> out;
    for (const auto& value : values) {
        Challenge* cur = nullptr;
        bool skipping = false;
        for (const auto& seg : splitTop(value)) {
            std::string_view s = trim(seg);
            if (s.empty()) continue;
            auto eq = s.find('=');
            auto sp = s.find_first_of(" \t");
            bool startsChallenge = (eq == std::string_view::npos) || (sp != std::string_view::npos && sp < eq);
            if (startsChallenge) {
                std::string scheme = lower(std::string(s.substr(0, sp)));
                std::string_view rest = sp == std::string_view::npos ? std::string_view{} : trim(s.substr(sp));
                if (scheme == "basic" || scheme == "digest") {
                    Challenge c;
                    c.scheme = scheme == "basic" ? Challenge::Scheme::Basic : Challenge::Scheme::Digest;
                    out.push_back(std::move(c));
                    cur = &out.back();
                    skipping = false;
                    auto e2 = rest.find('=');
                    if (e2 != std::string_view::npos) applyParam(*cur, rest.substr(0, e2), unquote(rest.substr(e2 + 1)));
                } else {
                    cur = nullptr;
                    skipping = true;
                }
            } else if (cur && !skipping) {
                applyParam(*cur, s.substr(0, eq), unquote(s.substr(eq + 1)));
            }
        }
    }
    return out;
}

std::string digestResponse(const Challenge& c, const Credentials& cred, std::string_view method,
                           std::string_view uri, std::string_view qop, std::string_view nc,
                           std::string_view cnonce) {
    std::string ha1 = md5Hex(cred.username + ":" + c.realm + ":" + cred.password);
    if (lower(c.algorithm) == "md5-sess") ha1 = md5Hex(ha1 + ":" + c.nonce + ":" + std::string(cnonce));
    std::string ha2 = md5Hex(std::string(method) + ":" + std::string(uri));
    if (qop.empty()) return md5Hex(ha1 + ":" + c.nonce + ":" + ha2);
    return md5Hex(ha1 + ":" + c.nonce + ":" + std::string(nc) + ":" + std::string(cnonce) + ":" +
                  std::string(qop) + ":" + ha2);
}

std::string basicHeaderValue(const Credentials& cred) {
    std::string raw = cred.username + ":" + cred.password;
    return "Basic " + base64Encode(ByteView(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()));
}

bool Authenticator::setChallenge(const std::vector<std::string>& headers) {
    if (cred_.empty()) return false;
    auto all = parseChallenges(headers);
    const Challenge* digest = nullptr;
    const Challenge* basic = nullptr;
    for (auto& c : all) {
        if (c.scheme == Challenge::Scheme::Digest) {
            std::string alg = lower(c.algorithm);
            bool algOk = alg.empty() || alg == "md5" || alg == "md5-sess";
            bool qopOk = c.qop.empty();
            for (auto& q : c.qop)
                if (q == "auth") qopOk = true;
            if (algOk && qopOk && !c.nonce.empty() && !digest) digest = &c;
        } else if (!basic) {
            basic = &c;
        }
    }
    const Challenge* pick = digest ? digest : basic;
    if (!pick) return false;
    chal_ = *pick;
    have_ = true;
    nc_ = 0;
    cnonceValue_.clear();
    return true;
}

std::string Authenticator::header(std::string_view method, std::string_view uri) {
    if (!have_) return {};
    if (chal_.scheme == Challenge::Scheme::Basic) return basicHeaderValue(cred_);
    bool useQop = false;
    for (auto& q : chal_.qop)
        if (q == "auth") useQop = true;
    char ncbuf[16];
    std::string qop, cnonce;
    if (useQop) {
        ++nc_;
        std::snprintf(ncbuf, sizeof ncbuf, "%08x", nc_);
        if (cnonceValue_.empty()) cnonceValue_ = cnonce_ ? cnonce_() : "0a4f113b";
        cnonce = cnonceValue_;
        qop = "auth";
    } else {
        ncbuf[0] = 0;
    }
    std::string resp = digestResponse(chal_, cred_, method, uri, qop, ncbuf, cnonce);
    std::string h = "Digest username=\"" + cred_.username + "\", realm=\"" + chal_.realm + "\", nonce=\"" +
                    chal_.nonce + "\", uri=\"" + std::string(uri) + "\", response=\"" + resp + "\"";
    if (!chal_.algorithm.empty()) h += ", algorithm=" + chal_.algorithm;
    if (!chal_.opaque.empty()) h += ", opaque=\"" + chal_.opaque + "\"";
    if (useQop) h += ", qop=auth, nc=" + std::string(ncbuf) + ", cnonce=\"" + cnonce + "\"";
    return h;
}

}  // namespace hb::rtsp
