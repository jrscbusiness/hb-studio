#include "cloud/gdrive_provider.hpp"

#include <cstdlib>

#include "cloud/cloud_util.hpp"
#include "core/json.hpp"

namespace hb::cloud {

namespace {
using Json = hb::core::json::Value;

constexpr const char* kApi = "https://www.googleapis.com/drive/v3/files";
constexpr const char* kUpload = "https://www.googleapis.com/upload/drive/v3/files";

std::string jstr(const Json& o, const char* k) {
    const Json* v = o.get(k);
    return v && v->isStr() ? v->s : std::string();
}

std::string qEscape(const std::string& s) {  // inside a Drive q='...' literal
    std::string o;
    for (char c : s) {
        if (c == '\'' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

Error driveError(const HttpResponse& r) {
    const int st = r.status;
    Json j;
    std::string reason, message;
    if (hb::core::json::parse(r.body, j) && j.isObj()) {
        if (const Json* e = j.get("error"); e && e->isObj()) {
            message = sanitizeMessage(jstr(*e, "message"));
            if (const Json* errs = e->get("errors"); errs && errs->isArr() && !errs->a.empty()) reason = jstr(errs->a[0], "reason");
            if (reason.empty()) reason = jstr(*e, "status");
        }
    }
    const std::string detail = " (HTTP " + std::to_string(st) + (reason.empty() ? "" : ", " + reason) + ")";
    if (reason == "storageQuotaExceeded")
        return makeError(ErrorKind::Quota, "O Google Drive está sem espaço: libere espaço ou compre mais armazenamento" + detail, st);
    if (reason == "dailyLimitExceeded" || reason == "uploadLimitExceeded" || reason == "dailyLimitExceededUnreg")
        return makeError(ErrorKind::Quota, "Limite diário do Google Drive atingido (o envio continua amanhã se você tentar de novo)" + detail, st);
    if (reason == "userRateLimitExceeded" || reason == "rateLimitExceeded" || st == 429 || st >= 500 || st == 408)
        return makeError(ErrorKind::Transient, "Google Drive ocupado, tentando de novo" + detail, st);
    if (st == 401) return makeError(ErrorKind::Auth, "Autorização do Google recusada: conecte a conta novamente" + detail, st);
    if (st == 403)
        return makeError(ErrorKind::Auth, "Acesso negado pelo Google Drive (a API do Drive está ativada no seu projeto?) " + message + detail, st);
    if (st == 404) return makeError(ErrorKind::SessionLost, "Sessão de envio não encontrada no Google Drive" + detail, st);
    return makeError(ErrorKind::Permanent, "Google Drive recusou o pedido: " + message + detail, st);
}

}  // namespace

GDriveProvider::GDriveProvider(GDriveConfig cfg, std::shared_ptr<ICloudHttp> http, std::shared_ptr<GoogleTokenSource> tokens)
    : cfg_(std::move(cfg)), http_(std::move(http)), tokens_(std::move(tokens)) {
    const std::uint64_t q = 256 * 1024;
    cfg_.chunkSize = std::max<std::uint64_t>(q, cfg_.chunkSize / q * q);
    if (cfg_.folderName.empty()) cfg_.folderName = "HB Studio";
}

std::string GDriveProvider::driveName(const std::string& remoteName) {
    std::string o;
    for (char c : remoteName) o += (c == '/' || c == '\\') ? '_' : c;
    return o;
}

Result<HttpResponse> GDriveProvider::authed(HttpRequest req) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto tok = tokens_->accessToken();
        if (!tok.ok()) return tok.error();
        HttpRequest r = req;
        r.headers.emplace_back("Authorization", "Bearer " + tok.value());
        auto resp = http_->send(r);
        if (resp.ok() && resp.value().status == 401 && attempt == 0) {
            tokens_->invalidate();
            continue;
        }
        return resp;
    }
    return makeError(ErrorKind::Auth, "Autorização do Google recusada");
}

Result<std::string> GDriveProvider::ensureFolder() {
    {
        std::lock_guard lock(mu_);
        if (!cfg_.folderId.empty()) return cfg_.folderId;
    }
    HttpRequest find;
    find.method = "GET";
    const std::string q = "name = '" + qEscape(cfg_.folderName) + "' and mimeType = 'application/vnd.google-apps.folder' and trashed = false";
    find.url = std::string(kApi) + "?q=" + formEncode(q) + "&fields=" + formEncode("files(id,name)") + "&pageSize=1&spaces=drive";
    auto r = authed(find);
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return driveError(r.value());
    Json j;
    if (hb::core::json::parse(r.value().body, j) && j.isObj()) {
        if (const Json* files = j.get("files"); files && files->isArr() && !files->a.empty()) {
            const std::string id = jstr(files->a[0], "id");
            if (!id.empty()) {
                std::lock_guard lock(mu_);
                cfg_.folderId = id;
                return id;
            }
        }
    }
    HttpRequest mk;
    mk.method = "POST";
    mk.url = std::string(kApi) + "?fields=id";
    mk.headers = {{"Content-Type", "application/json; charset=UTF-8"}};
    mk.body = "{\"name\":\"" + hb::core::json::escape(cfg_.folderName) + "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
    auto m = authed(mk);
    if (!m.ok()) return m.error();
    if (m.value().status / 100 != 2) return driveError(m.value());
    Json mj;
    if (!hb::core::json::parse(m.value().body, mj) || !mj.isObj() || jstr(mj, "id").empty())
        return makeError(ErrorKind::Transient, "Resposta inesperada ao criar a pasta no Google Drive");
    {
        std::lock_guard lock(mu_);
        cfg_.folderId = jstr(mj, "id");
    }
    return jstr(mj, "id");
}

Result<std::string> GDriveProvider::test() {
    HttpRequest about;
    about.method = "GET";
    about.url = "https://www.googleapis.com/drive/v3/about?fields=" + formEncode("user(emailAddress),storageQuota(limit,usage)");
    auto r = authed(about);
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return driveError(r.value());
    std::string who;
    std::string quota;
    Json j;
    if (hb::core::json::parse(r.value().body, j) && j.isObj()) {
        if (const Json* u = j.get("user"); u && u->isObj()) who = sanitizeMessage(jstr(*u, "emailAddress"), 80);
        if (const Json* sq = j.get("storageQuota"); sq && sq->isObj()) {
            const std::string lim = jstr(*sq, "limit"), use = jstr(*sq, "usage");
            if (!lim.empty() && !use.empty()) {
                const double free = (std::strtod(lim.c_str(), nullptr) - std::strtod(use.c_str(), nullptr)) / (1024.0 * 1024.0 * 1024.0);
                char buf[64];
                std::snprintf(buf, sizeof buf, " Espaço livre: %.1f GiB.", free);
                quota = buf;
            }
        }
    }
    auto folder = ensureFolder();
    if (!folder.ok()) return folder.error();
    return "Conexão OK" + (who.empty() ? std::string() : " (" + who + ")") + ": pasta \"" + cfg_.folderName + "\" pronta." + quota;
}

Result<RemoteStat> GDriveProvider::stat(const FileInfo& f) {
    auto folder = ensureFolder();
    if (!folder.ok()) return folder.error();
    HttpRequest req;
    req.method = "GET";
    const std::string q = "name = '" + qEscape(driveName(f.remoteName)) + "' and '" + folder.value() + "' in parents and trashed = false";
    req.url = std::string(kApi) + "?q=" + formEncode(q) + "&fields=" + formEncode("files(id,size,md5Checksum)") + "&pageSize=1&spaces=drive";
    auto r = authed(req);
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return driveError(r.value());
    RemoteStat s;
    Json j;
    if (hb::core::json::parse(r.value().body, j) && j.isObj()) {
        if (const Json* files = j.get("files"); files && files->isArr() && !files->a.empty()) {
            s.exists = true;
            s.remoteId = jstr(files->a[0], "id");
            s.size = std::strtoull(jstr(files->a[0], "size").c_str(), nullptr, 10);
            s.md5 = jstr(files->a[0], "md5Checksum");
        }
    }
    return s;
}

Result<UploadSession> GDriveProvider::begin(const FileInfo& f) {
    if (f.size == 0) return makeError(ErrorKind::Local, "Arquivo vazio");
    auto folder = ensureFolder();
    if (!folder.ok()) return folder.error();
    HttpRequest req;
    req.method = "POST";
    req.url = std::string(kUpload) + "?uploadType=resumable&fields=" + formEncode("id,size,md5Checksum");
    req.headers = {{"Content-Type", "application/json; charset=UTF-8"},
                   {"X-Upload-Content-Type", f.contentType},
                   {"X-Upload-Content-Length", std::to_string(f.size)}};
    req.body = "{\"name\":\"" + hb::core::json::escape(driveName(f.remoteName)) + "\",\"parents\":[\"" +
               hb::core::json::escape(folder.value()) + "\"]}";
    auto r = authed(req);
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return driveError(r.value());
    UploadSession s;
    s.uri = headerValue(r.value(), "location");
    s.chunkSize = cfg_.chunkSize;
    if (s.uri.rfind("https://", 0) != 0) return makeError(ErrorKind::Transient, "O Google Drive não devolveu o endereço da sessão de envio");
    return s;
}

namespace {
// "bytes=0-12345" -> 12346 (bytes committed); no header -> 0.
std::uint64_t committedFromRange(const HttpResponse& r) {
    const std::string h = headerValue(r, "range");
    const auto dash = h.rfind('-');
    if (h.empty() || dash == std::string::npos) return 0;
    return std::strtoull(h.c_str() + dash + 1, nullptr, 10) + 1;
}
}  // namespace

Result<std::uint64_t> GDriveProvider::committed(UploadSession& s, const FileInfo& f) {
    if (s.uri.empty()) return makeError(ErrorKind::SessionLost, "Sessão de envio ausente");
    HttpRequest req;
    req.method = "PUT";
    req.url = s.uri;
    req.headers = {{"Content-Range", "bytes */" + std::to_string(f.size)}};
    auto r = authed(req);
    if (!r.ok()) return r.error();
    const auto& resp = r.value();
    if (resp.status == 308) return committedFromRange(resp);
    if (resp.status == 200 || resp.status == 201) {
        Json j;
        if (hb::core::json::parse(resp.body, j) && j.isObj()) {
            s.remoteId = jstr(j, "id");
            s.remoteMd5 = jstr(j, "md5Checksum");
        }
        return f.size;  // already complete
    }
    return driveError(resp);
}

Result<ChunkAck> GDriveProvider::putChunk(UploadSession& s, const FileInfo& f, std::uint64_t offset, std::string_view data, ITransferGate* gate) {
    if (data.empty()) return makeError(ErrorKind::Local, "Parte vazia");
    if (s.uri.empty()) return makeError(ErrorKind::SessionLost, "Sessão de envio ausente");
    HttpRequest req;
    req.method = "PUT";
    req.url = s.uri;
    req.headers = {{"Content-Range", "bytes " + std::to_string(offset) + "-" + std::to_string(offset + data.size() - 1) + "/" + std::to_string(f.size)}};
    req.body = std::string(data);
    req.gate = gate;
    req.timeout = std::chrono::minutes(10);
    auto r = authed(req);
    if (!r.ok()) return r.error();
    const auto& resp = r.value();
    ChunkAck a;
    a.offset = offset;
    a.size = data.size();
    a.index = static_cast<std::uint32_t>(offset / std::max<std::uint64_t>(1, s.chunkSize) + 1);
    if (resp.status == 308) {
        const std::uint64_t total = committedFromRange(resp);
        if (total < offset) return makeError(ErrorKind::SessionLost, "O servidor perdeu parte dos dados enviados: reiniciando a sincronia", 308);
        a.accepted = std::min<std::uint64_t>(total - offset, a.size);
        if (a.accepted == 0) return makeError(ErrorKind::Transient, "O servidor não aceitou a parte, tentando de novo", 308);
        return a;
    }
    if (resp.status == 200 || resp.status == 201) {
        Json j;
        if (hb::core::json::parse(resp.body, j) && j.isObj()) {
            a.remoteId = jstr(j, "id");
            a.remoteMd5 = jstr(j, "md5Checksum");
        }
        a.accepted = a.size;
        a.done = true;
        s.remoteId = a.remoteId;
        s.remoteMd5 = a.remoteMd5;
        return a;
    }
    return driveError(resp);
}

Result<RemoteStat> GDriveProvider::finish(UploadSession& s, const FileInfo& f, const std::string& localMd5Hex) {
    RemoteStat st;
    st.exists = true;
    st.size = f.size;
    st.remoteId = s.remoteId;
    st.md5 = s.remoteMd5;
    if (s.remoteId.empty()) return makeError(ErrorKind::Transient, "O Google Drive não confirmou a conclusão do envio");
    if (!localMd5Hex.empty() && !s.remoteMd5.empty() && localMd5Hex != s.remoteMd5)
        return makeError(ErrorKind::Permanent, "O MD5 do arquivo no Google Drive difere do arquivo local (envio corrompido): use TENTAR DE NOVO");
    if (s.remoteMd5.empty()) st.note = "O Google Drive não informou o MD5: integridade não conferida.";
    return st;
}

void GDriveProvider::abort(const UploadSession& s, const FileInfo&) noexcept {
    if (s.uri.empty()) return;
    try {
        HttpRequest req;
        req.method = "DELETE";
        req.url = s.uri;
        req.timeout = std::chrono::seconds(20);
        authed(req);
    } catch (...) {
    }
}

}  // namespace hb::cloud
