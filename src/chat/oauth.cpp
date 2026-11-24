#include "chat/oauth.hpp"

#include <random>

#include "core/json.hpp"
#include "core/sha256.hpp"

namespace hb::chat {

std::string urlEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string urlDecode(std::string_view s) {
    auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() + 0 && hv(s[i + 1]) >= 0 && hv(s[i + 2]) >= 0) {
            o += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string base64Url(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63];
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63];
    }
    return o;
}

std::string pkceChallengeS256(const std::string& verifier) {
    const std::string hex = hb::core::Sha256::hex(verifier);
    std::string raw;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto hv = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        raw += static_cast<char>(hv(hex[i]) * 16 + hv(hex[i + 1]));
    }
    return base64Url(raw);
}

RandomFn defaultRandom() {
    return [](unsigned char* p, std::size_t n) {
        std::random_device rd;  // OS entropy (rand_s on MSVC, /dev/urandom on glibc)
        for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(rd());
    };
}

std::string randomUrlSafe(std::size_t bytes, const RandomFn& rnd) {
    std::string raw(bytes, '\0');
    rnd(reinterpret_cast<unsigned char*>(raw.data()), bytes);
    return base64Url(raw);
}

AuthRequest buildGoogleAuthRequest(const std::string& clientId, const std::string& redirectUri, const RandomFn& rnd) {
    AuthRequest r;
    r.verifier = randomUrlSafe(48, rnd);  // 64 chars (RFC 7636 allows 43..128)
    r.state = randomUrlSafe(24, rnd);
    r.url = std::string(kGoogleAuthEndpoint) + "?client_id=" + urlEncode(clientId) + "&redirect_uri=" +
            urlEncode(redirectUri) + "&response_type=code&scope=" + urlEncode(kYouTubeReadonlyScope) +
            "&code_challenge=" + pkceChallengeS256(r.verifier) + "&code_challenge_method=S256&state=" +
            urlEncode(r.state) + "&access_type=offline&prompt=consent";
    return r;
}

RedirectResult parseLoopbackRequest(std::string_view raw) {
    RedirectResult r;
    const std::size_t eol = raw.find_first_of("\r\n");
    std::string_view line = raw.substr(0, eol == std::string_view::npos ? raw.size() : eol);
    if (line.substr(0, 4) != "GET ") return r;
    line.remove_prefix(4);
    const std::size_t sp = line.find(' ');
    std::string_view target = line.substr(0, sp);
    const std::size_t q = target.find('?');
    if (q == std::string_view::npos) return r;
    std::string_view query = target.substr(q + 1);
    std::size_t p = 0;
    while (p <= query.size()) {
        std::size_t e = query.find('&', p);
        if (e == std::string_view::npos) e = query.size();
        std::string_view kv = query.substr(p, e - p);
        const std::size_t eq = kv.find('=');
        if (eq != std::string_view::npos) {
            const std::string k = urlDecode(kv.substr(0, eq));
            const std::string v = urlDecode(kv.substr(eq + 1));
            if (v.size() > 2048) { p = e + 1; continue; }
            if (k == "code") r.code = v;
            else if (k == "state") r.state = v;
            else if (k == "error") r.error = v;
        }
        p = e + 1;
    }
    r.ok = !r.code.empty() || !r.error.empty();
    return r;
}

bool parseTokenResponse(std::string_view json, TokenSet& out, std::string& error) {
    using Json = hb::core::json::Value;
    out = TokenSet{};
    error.clear();
    Json root;
    if (!hb::core::json::parse(json, root) || !root.isObj()) { error = "resposta inválida"; return false; }
    auto s = [&](const char* k) { const Json* v = root.get(k); return v && v->isStr() ? v->s : std::string(); };
    if (const Json* e = root.get("error"); e && e->isStr()) {
        error = e->s;  // OAuth error code, e.g. invalid_grant; the description is user-facing text, no secrets
        return false;
    }
    out.accessToken = s("access_token");
    out.refreshToken = s("refresh_token");
    if (const Json* v = root.get("expires_in"); v && v->isNum() && v->n > 0 && v->n < 1e7)
        out.expiresInSec = static_cast<long long>(v->n);
    if (out.accessToken.empty()) { error = "resposta sem access_token"; return false; }
    if (out.expiresInSec == 0) out.expiresInSec = 3600;
    return true;
}

void GoogleTokenManager::setClient(std::string clientId, std::string clientSecret) {
    std::lock_guard lock(m_);
    clientId_ = std::move(clientId);
    clientSecret_ = std::move(clientSecret);
    access_.clear();
}

bool GoogleTokenManager::hasRefreshToken() const {
    std::string t;
    return secrets_.get(kYouTubeRefreshKey, t) && !t.empty();
}

bool GoogleTokenManager::post(const std::string& body, TokenSet& out, std::string& error, int& status) {
    HttpRequest req;
    req.method = "POST";
    req.url = kGoogleTokenEndpoint;
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    req.body = body;
    req.timeoutMs = 10000;
    const HttpResponse r = http_.send(req);
    status = r.status;
    if (!r.transportOk) { error = "rede: " + r.error; return false; }
    return parseTokenResponse(r.body, out, error);
}

bool GoogleTokenManager::exchangeCode(const std::string& code, const std::string& verifier,
                                      const std::string& redirectUri, TimePoint now, std::string& error) {
    std::string cid, csec;
    {
        std::lock_guard lock(m_);
        cid = clientId_;
        csec = clientSecret_;
    }
    if (cid.empty()) { error = "client id não configurado"; return false; }
    std::string body = "grant_type=authorization_code&code=" + urlEncode(code) + "&client_id=" + urlEncode(cid) +
                       "&redirect_uri=" + urlEncode(redirectUri) + "&code_verifier=" + urlEncode(verifier);
    if (!csec.empty()) body += "&client_secret=" + urlEncode(csec);
    TokenSet t;
    int status = 0;
    if (!post(body, t, error, status)) return false;
    if (t.refreshToken.empty()) { error = "o Google não devolveu refresh token (revogue o acesso e tente de novo)"; return false; }
    if (!secrets_.put(kYouTubeRefreshKey, t.refreshToken)) { error = "não foi possível guardar o token com segurança"; return false; }
    std::lock_guard lock(m_);
    access_ = t.accessToken;
    expiresAt_ = now + std::chrono::seconds(t.expiresInSec);
    return true;
}

void GoogleTokenManager::signOut() {
    secrets_.erase(kYouTubeRefreshKey);
    std::lock_guard lock(m_);
    access_.clear();
}

void GoogleTokenManager::invalidate() {
    std::lock_guard lock(m_);
    access_.clear();
}

bool GoogleTokenManager::accessToken(TimePoint now, std::string& token, std::string& error, bool& needsAuth) {
    needsAuth = false;
    std::string cid, csec;
    {
        std::lock_guard lock(m_);
        if (!access_.empty() && now + std::chrono::seconds(60) < expiresAt_) { token = access_; return true; }
        cid = clientId_;
        csec = clientSecret_;
    }
    std::string refresh;
    if (!secrets_.get(kYouTubeRefreshKey, refresh) || refresh.empty() || cid.empty()) {
        needsAuth = true;
        error = cid.empty() ? "client id do Google não configurado" : "YouTube não autorizado";
        return false;
    }
    std::string body = "grant_type=refresh_token&refresh_token=" + urlEncode(refresh) + "&client_id=" + urlEncode(cid);
    if (!csec.empty()) body += "&client_secret=" + urlEncode(csec);
    TokenSet t;
    int status = 0;
    if (!post(body, t, error, status)) {
        // invalid_grant = revoked, or (apps in "Testing" status) the 7-day refresh token expired.
        if (error == "invalid_grant" || error == "invalid_client" || error == "unauthorized_client") {
            needsAuth = true;
            if (error == "invalid_grant") secrets_.erase(kYouTubeRefreshKey);
            error = "autorização do YouTube expirou ou foi revogada (" + error + ")";
        }
        return false;
    }
    if (!t.refreshToken.empty()) secrets_.put(kYouTubeRefreshKey, t.refreshToken);
    std::lock_guard lock(m_);
    access_ = t.accessToken;
    expiresAt_ = now + std::chrono::seconds(t.expiresInSec);
    token = access_;
    return true;
}

}  // namespace hb::chat
