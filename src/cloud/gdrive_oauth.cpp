#include "cloud/gdrive_oauth.hpp"

#include <mutex>

#include "cloud/cloud_util.hpp"
#include "cloud/hash_util.hpp"
#include "core/json.hpp"

namespace hb::cloud {

namespace {
using Json = hb::core::json::Value;

std::string jstr(const Json& o, const char* k) {
    const Json* v = o.get(k);
    return v && v->isStr() ? v->s : std::string();
}

std::string queryParam(const std::string& query, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const auto eq = pair.find('=');
        const std::string k = pair.substr(0, eq);
        if (k == name) {
            std::string v = eq == std::string::npos ? "" : pair.substr(eq + 1);
            std::string o;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '%' && i + 2 < v.size() + 0 && i + 2 <= v.size() - 1 + 0) {
                    const std::string raw = hexDecode(v.substr(i + 1, 2));
                    if (raw.size() == 1) { o += raw; i += 2; continue; }
                }
                o += v[i] == '+' ? ' ' : v[i];
            }
            return o;
        }
        pos = amp + 1;
    }
    return {};
}
}  // namespace

Result<TokenSet> parseTokenResponse(const HttpResponse& r) {
    Json j;
    const bool parsed = hb::core::json::parse(r.body, j) && j.isObj();
    if (r.status / 100 != 2) {
        const std::string err = parsed ? jstr(j, "error") : "";
        if (err == "invalid_grant")
            return makeError(ErrorKind::Auth, "A autorização do Google expirou ou foi revogada: conecte a conta novamente", r.status);
        if (err == "invalid_client" || err == "unauthorized_client")
            return makeError(ErrorKind::Auth, "Client ID/segredo do Google recusados: confira as credenciais do seu projeto", r.status);
        if (r.status >= 500 || r.status == 429)
            return makeError(ErrorKind::Transient, "Serviço de autenticação do Google indisponível", r.status);
        return makeError(ErrorKind::Auth, "O Google recusou a autorização (" + sanitizeMessage(err) + ")", r.status);
    }
    if (!parsed) return makeError(ErrorKind::Transient, "Resposta inválida do Google", r.status);
    TokenSet t;
    t.accessToken = jstr(j, "access_token");
    t.refreshToken = jstr(j, "refresh_token");
    if (const Json* e = j.get("expires_in"); e && e->isNum()) t.expiresInSec = static_cast<long>(e->n);
    if (t.accessToken.empty()) return makeError(ErrorKind::Transient, "Resposta do Google sem access_token", r.status);
    if (t.expiresInSec <= 0) t.expiresInSec = 3000;
    return t;
}

namespace {
HttpRequest formPost(const std::vector<KV>& fields) {
    HttpRequest req;
    req.method = "POST";
    req.url = kGoogleTokenEndpoint;
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) req.body += '&';
        req.body += fields[i].first + "=" + formEncode(fields[i].second);
    }
    req.timeout = std::chrono::seconds(30);
    return req;
}
}  // namespace

HttpRequest refreshTokenRequest(const GoogleOAuthConfig& cfg, const std::string& refreshToken) {
    std::vector<KV> f = {{"client_id", cfg.clientId}, {"refresh_token", refreshToken}, {"grant_type", "refresh_token"}};
    if (!cfg.clientSecret.empty()) f.emplace_back("client_secret", cfg.clientSecret);
    return formPost(f);
}

GoogleOAuthFlow::GoogleOAuthFlow(GoogleOAuthConfig cfg, RandomBytesFn random) : cfg_(std::move(cfg)), random_(std::move(random)) {}

std::string GoogleOAuthFlow::authUrl(int port) {
    verifier_ = base64UrlEncode(random_(32));   // 43 chars, within RFC 7636's 43..128
    challenge_ = base64UrlEncode(sha256Raw(verifier_));
    state_ = base64UrlEncode(random_(16));
    redirectUri_ = "http://127.0.0.1:" + std::to_string(port);
    return std::string(kGoogleAuthEndpoint) + "?client_id=" + formEncode(cfg_.clientId) + "&redirect_uri=" + formEncode(redirectUri_) +
           "&response_type=code&scope=" + formEncode(kDriveScope) + "&code_challenge=" + challenge_ +
           "&code_challenge_method=S256&state=" + state_ + "&access_type=offline&prompt=consent";
}

GoogleOAuthFlow::Redirect GoogleOAuthFlow::parseRedirect(const std::string& line) const {
    Redirect r;
    // "GET /?a=b HTTP/1.1"
    const auto sp1 = line.find(' ');
    const auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (line.rfind("GET ", 0) != 0 || sp2 == std::string::npos) {
        r.error = "Pedido de retorno inválido";
        return r;
    }
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto q = target.find('?');
    const std::string path = target.substr(0, q);
    if (path != "/" || q == std::string::npos) {
        r.error = "Pedido de retorno inesperado";
        return r;
    }
    const std::string query = target.substr(q + 1);
    if (state_.empty() || queryParam(query, "state") != state_) {
        r.error = "Retorno do Google com 'state' inválido (possível tentativa de falsificação): autorização descartada";
        return r;
    }
    if (const std::string e = queryParam(query, "error"); !e.empty()) {
        r.error = e == "access_denied" ? "Autorização negada no navegador" : "O Google devolveu um erro: " + sanitizeMessage(e, 60);
        return r;
    }
    r.code = queryParam(query, "code");
    if (r.code.empty()) {
        r.error = "Retorno do Google sem código de autorização";
        return r;
    }
    r.ok = true;
    return r;
}

HttpRequest GoogleOAuthFlow::tokenRequest(const std::string& code) const {
    std::vector<KV> f = {{"code", code},
                         {"client_id", cfg_.clientId},
                         {"code_verifier", verifier_},
                         {"grant_type", "authorization_code"},
                         {"redirect_uri", redirectUri_}};
    if (!cfg_.clientSecret.empty()) f.emplace_back("client_secret", cfg_.clientSecret);
    return formPost(f);
}

struct GoogleTokenSource::Impl {
    GoogleOAuthConfig cfg;
    std::string refresh;
    std::shared_ptr<ICloudHttp> http;
    NowFn now;
    std::mutex mu;
    std::string token;
    double expiresAt = 0;
};

GoogleTokenSource::GoogleTokenSource(GoogleOAuthConfig cfg, std::string refreshToken, std::shared_ptr<ICloudHttp> http, NowFn now)
    : impl_(std::make_shared<Impl>()) {
    impl_->cfg = std::move(cfg);
    impl_->refresh = std::move(refreshToken);
    impl_->http = std::move(http);
    impl_->now = std::move(now);
}

Result<std::string> GoogleTokenSource::accessToken() {
    std::lock_guard lock(impl_->mu);
    const double t = impl_->now();
    if (!impl_->token.empty() && t < impl_->expiresAt - 60) return impl_->token;
    if (impl_->refresh.empty()) return makeError(ErrorKind::Auth, "Google Drive não conectado: conecte a conta nas configurações");
    auto r = impl_->http->send(refreshTokenRequest(impl_->cfg, impl_->refresh));
    if (!r.ok()) return r.error();
    auto ts = parseTokenResponse(r.value());
    if (!ts.ok()) return ts.error();
    impl_->token = ts.value().accessToken;
    impl_->expiresAt = t + static_cast<double>(ts.value().expiresInSec);
    return impl_->token;
}

void GoogleTokenSource::invalidate() {
    std::lock_guard lock(impl_->mu);
    impl_->token.clear();
    impl_->expiresAt = 0;
}

}  // namespace hb::cloud
