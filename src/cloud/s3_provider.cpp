#include "cloud/s3_provider.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "cloud/cloud_util.hpp"
#include "cloud/hash_util.hpp"
#include "cloud/sigv4.hpp"

namespace hb::cloud {

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool dnsBucket(const std::string& b) {
    if (b.size() < 3 || b.size() > 63) return false;
    for (char c : b)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-')) return false;
    return b.front() != '-' && b.back() != '-';
}

std::string normalizePrefix(std::string p) {
    // Keep it a plain relative "folder/": no leading slashes, no "..", single trailing '/'.
    std::string o;
    std::string seg;
    auto flush = [&] {
        if (!seg.empty() && seg != "." && seg != "..") o += seg + "/";
        seg.clear();
    };
    for (char c : p) {
        if (c == '/' || c == '\\') flush();
        else if (static_cast<unsigned char>(c) >= 0x20) seg += c;
    }
    flush();
    return o;
}

Error s3Error(const HttpResponse& r) {
    const int st = r.status;
    const std::string code = xmlText(r.body, "Code");
    const std::string msg = sanitizeMessage(xmlText(r.body, "Message"));
    const std::string detail = " (HTTP " + std::to_string(st) + (code.empty() ? "" : ", " + code) + ")";
    auto lowerHas = [&](const char* needle) {
        std::string c = code;
        for (auto& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return c.find(needle) != std::string::npos;
    };
    if (st == 507 || lowerHas("quota") || lowerHas("storagelimit") || lowerHas("storage_cap") || lowerHas("insufficientstorage") ||
        lowerHas("transaction_cap") || lowerHas("usage_cap"))
        return makeError(ErrorKind::Quota, "Cota/limite de armazenamento do provedor atingido: libere espaço ou aumente o plano" + detail, st);
    if (code == "NoSuchUpload" || code == "InvalidPart" || code == "InvalidPartOrder")
        return makeError(ErrorKind::SessionLost, "A sessão de envio expirou ou foi cancelada no servidor" + detail, st);
    if (code == "RequestTimeTooSkewed")
        return makeError(ErrorKind::Auth, "O relógio do computador está desajustado: acerte a data/hora do Windows" + detail, st);
    if (code == "PermanentRedirect" || code == "AuthorizationHeaderMalformed" || code == "IllegalLocationConstraintException" ||
        st == 301 || st == 302 || st == 307)
        return makeError(ErrorKind::Permanent, "Região ou endpoint incorretos: confira a região e o endereço do provedor" + detail, st);
    if (code == "NoSuchBucket")
        return makeError(ErrorKind::Permanent, "O bucket não existe nesse endpoint/região" + detail, st);
    if (code == "SignatureDoesNotMatch")
        return makeError(ErrorKind::Auth, "Assinatura recusada: confira a chave secreta (e a região)" + detail, st);
    if (code == "InvalidAccessKeyId")
        return makeError(ErrorKind::Auth, "Chave de acesso (Access Key ID) não reconhecida" + detail, st);
    if (st == 401 || st == 403 || code == "AccessDenied" || code == "ExpiredToken" || code == "InvalidToken" || code == "AllAccessDisabled")
        return makeError(ErrorKind::Auth, "Acesso negado: a chave não tem permissão de escrita neste bucket" + detail, st);
    if (code == "EntityTooLarge")
        return makeError(ErrorKind::Permanent, "Arquivo grande demais para o provedor" + detail, st);
    if (st == 408 || st == 429 || st >= 500 || code == "RequestTimeout" || code == "SlowDown" || code == "InternalError" ||
        code == "ServiceUnavailable" || code == "OperationAborted")
        return makeError(ErrorKind::Transient, "Servidor ocupado ou indisponível, tentando de novo" + detail, st);
    if (st >= 400) return makeError(ErrorKind::Permanent, "Requisição recusada pelo provedor: " + msg + detail, st);
    return makeError(ErrorKind::Transient, "Resposta inesperada do provedor" + detail, st);
}

// S3 may answer 200 OK with an <Error> document (CompleteMultipartUpload, CopyObject).
bool bodyIsError(const HttpResponse& r) { return r.body.find("<Error>") != std::string::npos && r.body.find("<Code>") != std::string::npos; }

}  // namespace

std::string validateS3Config(const S3Config& c) {
    const Endpoint e = parseEndpoint(c.endpoint);
    if (!e.valid) return "Endereço (endpoint) inválido: use algo como https://s3.us-east-1.amazonaws.com";
    if (c.bucket.size() < 3 || c.bucket.size() > 63) return "Nome do bucket inválido (3 a 63 caracteres)";
    for (unsigned char ch : c.bucket)
        if (!(std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_')) return "Nome do bucket com caracteres inválidos";
    if (c.region.empty() || c.region.size() > 40) return "Região inválida";
    for (unsigned char ch : c.region)
        if (!(std::isalnum(ch) || ch == '-' || ch == '_')) return "Região com caracteres inválidos";
    if (c.accessKey.empty() || c.secretKey.empty()) return "Informe a chave de acesso e a chave secreta";
    if (c.partSize < kS3MinPartSize) return "Tamanho de parte mínimo é 5 MiB";
    return {};
}

S3Provider::S3Provider(S3Config cfg, std::shared_ptr<ICloudHttp> http, NowFn now)
    : cfg_(std::move(cfg)), http_(std::move(http)), now_(std::move(now)) {
    const Endpoint e = parseEndpoint(cfg_.endpoint);
    scheme_ = e.scheme;
    host_ = e.host;
    port_ = e.port;
    cfg_.prefix = normalizePrefix(cfg_.prefix);
    cfg_.partSize = std::clamp<std::uint64_t>(cfg_.partSize, kS3MinPartSize, 512ull * 1024 * 1024);
    if (cfg_.addressing == "virtual") virtualHost_ = true;
    else if (cfg_.addressing == "path") virtualHost_ = false;
    else virtualHost_ = endsWith(host_, ".amazonaws.com") && dnsBucket(cfg_.bucket);
}

std::string S3Provider::objectKey(const std::string& remoteName) const {
    std::string n;
    for (const auto& seg : [&] {
             std::vector<std::string> v;
             std::string cur;
             for (char c : remoteName) {
                 if (c == '/' || c == '\\') { v.push_back(cur); cur.clear(); }
                 else cur += c;
             }
             v.push_back(cur);
             return v;
         }()) {
        if (seg.empty() || seg == "." || seg == "..") continue;
        if (!n.empty()) n += '/';
        n += seg;
    }
    return cfg_.prefix + n;
}

std::uint64_t S3Provider::chunkSizeFor(std::uint64_t size) const {
    std::uint64_t chunk = cfg_.partSize;
    const std::uint64_t maxParts = kS3MaxParts - 500;  // headroom
    if ((size + chunk - 1) / chunk > maxParts) {
        chunk = (size + maxParts - 1) / maxParts;
        const std::uint64_t mib = 1024 * 1024;
        chunk = (chunk + mib - 1) / mib * mib;
    }
    return chunk;
}

Result<HttpResponse> S3Provider::call(const std::string& method, const std::string& key, std::vector<KV> query, std::string body,
                                      std::vector<KV> extraHeaders, ITransferGate* gate, std::chrono::milliseconds timeout) {
    std::string hostHeader = virtualHost_ ? cfg_.bucket + "." + host_ : host_;
    if (port_ != 0 && !((scheme_ == "https" && port_ == 443) || (scheme_ == "http" && port_ == 80))) hostHeader += ":" + std::to_string(port_);
    std::string path;
    if (virtualHost_) path = "/" + uriEncode(key, false);
    else path = "/" + uriEncode(cfg_.bucket, true) + (key.empty() ? "" : "/" + uriEncode(key, false));

    const std::string payloadHash = sha256Hex(body);
    const std::string amzDate = formatAmzDate(now_());

    SigV4Input in;
    in.method = method;
    in.canonicalPath = path;
    in.query = query;
    in.headers = {{"host", hostHeader}, {"x-amz-content-sha256", payloadHash}, {"x-amz-date", amzDate}};
    in.payloadHash = payloadHash;
    in.accessKey = cfg_.accessKey;
    in.secretKey = cfg_.secretKey;
    in.region = cfg_.region;
    in.service = "s3";
    in.amzDate = amzDate;
    const SigV4Output sig = signV4(in);

    HttpRequest req;
    req.method = method;
    req.url = scheme_ + "://" + hostHeader + path;
    const std::string qs = canonicalQueryString(query);
    if (!qs.empty()) req.url += "?" + qs;
    req.headers = {{"x-amz-content-sha256", payloadHash}, {"x-amz-date", amzDate}, {"Authorization", sig.authorization}};
    for (auto& h : extraHeaders) req.headers.push_back(std::move(h));
    req.body = std::move(body);
    req.gate = gate;
    req.timeout = timeout;
    return http_->send(req);
}

Result<std::string> S3Provider::test() {
    if (const std::string bad = validateS3Config(cfg_); !bad.empty()) return makeError(ErrorKind::Permanent, bad);
    const std::string key = cfg_.prefix + ".hbstudio-teste.txt";
    const std::string payload = "HB Studio: teste de conexao\n";
    auto put = call("PUT", key, {}, payload, {{"Content-Type", "text/plain"}}, nullptr, std::chrono::seconds(30));
    if (!put.ok()) return put.error();
    if (put.value().status / 100 != 2) return s3Error(put.value());
    std::string note;
    const std::string etag = unquoteEtag(headerValue(put.value(), "etag"));
    if (isHex32(etag) && etag != md5Hex(payload)) note = " Aviso: o ETag devolvido não confere com o conteúdo enviado.";
    auto del = call("DELETE", key, {}, "", {}, nullptr, std::chrono::seconds(30));
    if (!del.ok() || (del.value().status / 100 != 2 && del.value().status != 404))
        note += " (Não foi possível apagar o arquivo de teste .hbstudio-teste.txt: a chave não tem permissão de exclusão; pode apagá-lo manualmente.)";
    if (scheme_ == "http") note += " Atenção: conexão SEM criptografia (http).";
    return "Conexão OK: escrita no bucket \"" + cfg_.bucket + "\" verificada." + note;
}

Result<RemoteStat> S3Provider::stat(const FileInfo& f) {
    auto r = call("HEAD", objectKey(f.remoteName), {}, "", {}, nullptr, std::chrono::seconds(30));
    if (!r.ok()) return r.error();
    const auto& resp = r.value();
    RemoteStat s;
    if (resp.status / 100 == 2) {
        s.exists = true;
        const std::string len = headerValue(resp, "content-length");
        s.size = len.empty() ? 0 : std::strtoull(len.c_str(), nullptr, 10);
        s.etag = unquoteEtag(headerValue(resp, "etag"));
        return s;
    }
    // 404 = absent. 403 = absent OR no s3:ListBucket permission: indistinguishable, so "unknown" is
    // treated as "not there" (dedupe is best effort; real credential problems surface on upload).
    if (resp.status == 404 || resp.status == 403) return s;
    return s3Error(resp);
}

Result<UploadSession> S3Provider::begin(const FileInfo& f) {
    if (f.size == 0) return makeError(ErrorKind::Local, "Arquivo vazio");
    if (f.size > kS3MaxObjectSize) return makeError(ErrorKind::Permanent, "Arquivo maior que o limite de 5 TiB do S3");
    UploadSession s;
    s.chunkSize = chunkSizeFor(f.size);
    if (f.size <= s.chunkSize) return s;  // small file: one PutObject
    auto r = call("POST", objectKey(f.remoteName), {{"uploads", ""}}, "", {{"Content-Type", f.contentType}}, nullptr, std::chrono::seconds(60));
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return s3Error(r.value());
    s.id = xmlText(r.value().body, "UploadId");
    if (s.id.empty()) return makeError(ErrorKind::Transient, "Resposta sem UploadId do servidor");
    return s;
}

Result<std::uint64_t> S3Provider::committed(UploadSession& s, const FileInfo& f) {
    if (s.id.empty()) return std::uint64_t{0};  // single PUT: nothing is resumable
    std::vector<ChunkAck> parts;
    std::string marker;
    for (int page = 0; page < 20; ++page) {  // 20 x 1000 parts >= S3's 10000-part maximum
        std::vector<KV> q = {{"uploadId", s.id}, {"max-parts", "1000"}};
        if (!marker.empty()) q.emplace_back("part-number-marker", marker);
        auto r = call("GET", objectKey(f.remoteName), q, "", {}, nullptr, std::chrono::seconds(60));
        if (!r.ok()) return r.error();
        if (r.value().status / 100 != 2) return s3Error(r.value());
        for (const auto& blk : xmlBlocks(r.value().body, "Part")) {
            ChunkAck a;
            a.index = static_cast<std::uint32_t>(std::strtoul(xmlText(blk, "PartNumber").c_str(), nullptr, 10));
            a.size = std::strtoull(xmlText(blk, "Size").c_str(), nullptr, 10);
            a.accepted = a.size;
            a.etag = unquoteEtag(xmlText(blk, "ETag"));
            if (isHex32(a.etag)) a.md5 = a.etag;
            if (a.index > 0) parts.push_back(std::move(a));
        }
        if (xmlText(r.value().body, "IsTruncated") != "true") break;
        marker = xmlText(r.value().body, "NextPartNumberMarker");
        if (marker.empty()) break;
    }
    std::sort(parts.begin(), parts.end(), [](const ChunkAck& a, const ChunkAck& b) { return a.index < b.index; });
    std::vector<ChunkAck> good;
    for (auto& p : parts) {
        if (p.index != good.size() + 1 || p.size != s.chunkSize) break;  // only the contiguous run of full parts
        p.offset = static_cast<std::uint64_t>(good.size()) * s.chunkSize;
        good.push_back(std::move(p));
    }
    s.acks = std::move(good);
    return static_cast<std::uint64_t>(s.acks.size()) * s.chunkSize;
}

Result<ChunkAck> S3Provider::putChunk(UploadSession& s, const FileInfo& f, std::uint64_t offset, std::string_view data, ITransferGate* gate) {
    if (data.empty()) return makeError(ErrorKind::Local, "Parte vazia");
    ChunkAck ack;
    ack.offset = offset;
    ack.size = data.size();
    ack.md5 = md5Hex(data);
    ack.sha256 = sha256Hex(data);

    std::vector<KV> q;
    if (!s.id.empty()) {
        if (s.chunkSize == 0 || offset % s.chunkSize != 0) return makeError(ErrorKind::Permanent, "Deslocamento de parte inválido");
        ack.index = static_cast<std::uint32_t>(offset / s.chunkSize + 1);
        if (ack.index > kS3MaxParts) return makeError(ErrorKind::Permanent, "Arquivo excede 10000 partes");
        q = {{"partNumber", std::to_string(ack.index)}, {"uploadId", s.id}};
    } else {
        if (offset != 0) return makeError(ErrorKind::Permanent, "Envio simples começa no offset 0");
        ack.index = 1;
    }
    auto r = call("PUT", objectKey(f.remoteName), q, std::string(data), {{"Content-Type", f.contentType}}, gate, std::chrono::minutes(10));
    if (!r.ok()) return r.error();
    const auto& resp = r.value();
    if (resp.status / 100 != 2) return s3Error(resp);
    ack.etag = unquoteEtag(headerValue(resp, "etag"));
    const std::string sse = headerValue(resp, "x-amz-server-side-encryption");
    const bool kms = sse.find("kms") != std::string::npos;
    if (!kms && isHex32(ack.etag) && ack.etag != ack.md5)
        return makeError(ErrorKind::Transient, "A parte chegou corrompida (ETag diferente do MD5 local): reenviando", resp.status);
    ack.accepted = ack.size;
    ack.done = s.id.empty();
    // Keep one ack per part number.
    s.acks.erase(std::remove_if(s.acks.begin(), s.acks.end(), [&](const ChunkAck& a) { return a.index == ack.index; }), s.acks.end());
    s.acks.push_back(ack);
    return ack;
}

Result<RemoteStat> S3Provider::finish(UploadSession& s, const FileInfo& f, const std::string&) {
    RemoteStat st;
    st.exists = true;
    st.size = f.size;
    if (s.id.empty()) {
        if (!s.acks.empty()) st.etag = s.acks.back().etag;
        return st;
    }
    std::vector<ChunkAck> parts = s.acks;
    std::sort(parts.begin(), parts.end(), [](const ChunkAck& a, const ChunkAck& b) { return a.index < b.index; });
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].index != i + 1) return makeError(ErrorKind::SessionLost, "Partes faltando no envio: reiniciando o arquivo");
        total += parts[i].size;
    }
    if (total != f.size) return makeError(ErrorKind::SessionLost, "Tamanho das partes não confere com o arquivo: reiniciando");

    std::string xml = "<CompleteMultipartUpload>";
    std::string md5Concat;
    bool haveAllMd5 = true;
    for (const auto& p : parts) {
        xml += "<Part><PartNumber>" + std::to_string(p.index) + "</PartNumber><ETag>\"" + p.etag + "\"</ETag></Part>";
        const std::string raw = isHex32(p.md5) ? hexDecode(p.md5) : std::string();
        if (raw.size() != 16) haveAllMd5 = false;
        md5Concat += raw;
    }
    xml += "</CompleteMultipartUpload>";
    auto r = call("POST", objectKey(f.remoteName), {{"uploadId", s.id}}, xml, {{"Content-Type", "application/xml"}}, nullptr, std::chrono::minutes(5));
    if (!r.ok()) return r.error();
    if (r.value().status / 100 != 2) return s3Error(r.value());
    if (bodyIsError(r.value())) return s3Error(HttpResponse{500, r.value().body, {}});
    st.etag = unquoteEtag(xmlText(r.value().body, "ETag"));
    if (haveAllMd5 && st.etag.find('-') != std::string::npos) {
        const std::string expected = md5Hex(md5Concat) + "-" + std::to_string(parts.size());
        if (expected != st.etag)
            st.note = "O ETag final difere do esperado (alguns provedores calculam de outro jeito); o tamanho foi conferido.";
    }
    // Confirm the stored size with a HEAD (needs no special permission beyond GetObject/HeadObject).
    auto h = call("HEAD", objectKey(f.remoteName), {}, "", {}, nullptr, std::chrono::seconds(30));
    if (h.ok() && h.value().status / 100 == 2) {
        const std::string len = headerValue(h.value(), "content-length");
        if (!len.empty() && std::strtoull(len.c_str(), nullptr, 10) != f.size)
            return makeError(ErrorKind::SessionLost, "O objeto no servidor tem tamanho diferente do arquivo local: reenviando");
    } else {
        st.note += (st.note.empty() ? "" : " ") + std::string("Tamanho final não pôde ser conferido (sem permissão de leitura).");
    }
    return st;
}

void S3Provider::abort(const UploadSession& s, const FileInfo& f) noexcept {
    if (s.id.empty()) return;
    try {
        call("DELETE", objectKey(f.remoteName), {{"uploadId", s.id}}, "", {}, nullptr, std::chrono::seconds(20));
    } catch (...) {
    }
}

}  // namespace hb::cloud
