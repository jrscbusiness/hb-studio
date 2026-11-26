#include "marketplace/net_adapter.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "marketplace/catalog.hpp"

namespace hb::marketplace {
namespace {

namespace fs = std::filesystem;
namespace net = hb::net;
using Clock = std::chrono::steady_clock;

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string printableAscii(const std::string& in, std::size_t max) {
    std::string out;
    for (unsigned char c : in)
        if (c >= 0x20 && c < 0x7F && out.size() < max) out += static_cast<char>(c);  // no CR/LF injection
    return out;
}

std::string errorText(const net::HttpResponse& r) {
    if (r.error == net::ErrorKind::InvalidRequest) return "endereço de download não permitido (somente https)";
    switch (r.failure) {
        case net::Failure::Timeout: return "tempo esgotado";
        case net::Failure::Tls: return "falha na verificação do certificado HTTPS";
        case net::Failure::NameNotResolved: return "servidor não encontrado (sem internet?)";
        case net::Failure::CannotConnect: return "não foi possível conectar ao servidor";
        case net::Failure::ConnectionError: return "conexão interrompida";
        case net::Failure::RedirectFailed: return "redirecionamento recusado";
        case net::Failure::Other: break;
    }
    if (r.error == net::ErrorKind::Timeout) return "tempo esgotado";
    if (r.nativeError == 0 && r.errorText.find("unavailable") != std::string::npos) return "download HTTPS indisponível nesta plataforma";
    return "erro de rede " + std::to_string(r.nativeError);
}

net::HttpRequest baseRequest(const std::string& url, std::chrono::milliseconds timeout, const char* accept) {
    net::HttpRequest n;
    n.method = "GET";
    n.url = url;
    n.httpsOnly = true;
    n.headers.emplace_back("Accept", accept);
    n.timeouts = net::Timeouts::all(static_cast<int>(std::min<long long>(timeout.count(), 600000)));
    n.redirects = net::RedirectPolicy::FollowNoDowngrade;
    n.maxRedirects = 5;
    n.sinkMinStatus = n.sinkMaxStatus = 200;
    n.readBodyOutsideSink = false;  // 304 / errors carry no body we care about
    return n;
}

class NetBackedDownloader : public IHttpDownloader {
public:
    explicit NetBackedDownloader(std::shared_ptr<net::IHttpClient> c) : client_(std::move(c)) {}

    FetchResponse fetch(const FetchRequest& rq) override {
        FetchResponse out;
        if (!client_) { out.error = "não foi possível iniciar o WinHTTP"; return out; }
        if (!isSafeHttpsUrl(rq.url)) { out.error = "endereço de download não permitido (somente https)"; return out; }
        net::HttpRequest n = baseRequest(rq.url, rq.timeout, "application/json");
        if (!rq.ifNoneMatch.empty()) n.headers.emplace_back("If-None-Match", printableAscii(rq.ifNoneMatch, rq.ifNoneMatch.size()));
        const auto deadline = Clock::now() + rq.timeout;
        bool tooBig = false, timedOut = false;
        n.onData = [&](const char* d, std::size_t len, std::uint64_t total) {
            if (Clock::now() > deadline) { timedOut = true; return false; }
            if (total > rq.maxBytes || out.body.size() + len > rq.maxBytes) { tooBig = true; return false; }
            out.body.append(d, len);
            return true;
        };
        const net::HttpResponse r = client_->send(n);
        if (tooBig) { out = {}; out.error = "resposta grande demais"; return out; }
        if (timedOut) { out = {}; out.error = "tempo esgotado"; return out; }
        if (r.error != net::ErrorKind::None) { out = {}; out.error = errorText(r); return out; }
        if (r.status == 200 && r.contentLength > rq.maxBytes) { out = {}; out.error = "resposta grande demais"; return out; }
        out.transportOk = true;
        out.status = r.status;
        out.etag = printableAscii(r.header("ETag"), 255);
        if (out.status != 200) out.body.clear();
        return out;
    }

    FileResult downloadToFile(const FileRequest& rq, const ProgressFn& progress) override {
        FileResult out;
        if (!client_) { out.error = "não foi possível iniciar o WinHTTP"; return out; }
        if (rq.maxBytes == 0 || rq.destPathUtf8.empty()) { out.error = "pedido de download inválido"; return out; }
        if (!isSafeHttpsUrl(rq.url)) { out.error = "endereço de download não permitido (somente https)"; return out; }
        const fs::path dest = pathFromUtf8(rq.destPathUtf8);
        fs::path part = dest;
        part += ".part";
        std::error_code ec;
        const auto deadline = Clock::now() + rq.timeout;

        net::HttpRequest n = baseRequest(rq.url, rq.timeout, "application/octet-stream, */*");
        std::ofstream f;
        std::string sinkError;
        std::uint64_t received = 0;
        auto openPart = [&]() {
            if (f.is_open()) return true;
            f.open(part, std::ios::binary | std::ios::trunc);
            if (!f) { sinkError = "não foi possível gravar o arquivo temporário"; return false; }
            return true;
        };
        n.onData = [&](const char* d, std::size_t len, std::uint64_t total) {
            if (total > rq.maxBytes) { sinkError = "o arquivo é maior que o tamanho declarado no catálogo"; return false; }
            if (!openPart()) return false;
            if (Clock::now() > deadline) { sinkError = "tempo esgotado"; return false; }
            received += len;
            if (received > rq.maxBytes) { sinkError = "o arquivo é maior que o tamanho declarado no catálogo"; return false; }
            f.write(d, static_cast<std::streamsize>(len));
            if (!f) { sinkError = "falha ao gravar o arquivo (disco cheio?)"; return false; }
            if (progress && !progress(received, total ? total : rq.maxBytes)) { out.cancelled = true; sinkError = "cancelado"; return false; }
            return true;
        };
        const net::HttpResponse r = client_->send(n);
        out.status = r.status;
        auto fail = [&](const std::string& msg) {
            if (f.is_open()) f.close();
            fs::remove(part, ec);
            out.error = msg;
            return out;
        };
        if (!sinkError.empty()) return fail(sinkError);
        if (r.error != net::ErrorKind::None) {
            out.status = r.error == net::ErrorKind::InvalidRequest ? 0 : r.status;
            return fail(errorText(r));
        }
        if (r.status != 200) return fail("o servidor respondeu HTTP " + std::to_string(r.status));
        if (r.contentLength > rq.maxBytes) return fail("o arquivo é maior que o tamanho declarado no catálogo");
        if (!openPart()) return fail(sinkError);  // empty body: the file is still created
        f.close();
        if (!f) return fail("falha ao gravar o arquivo");
        out.bytes = received;
        fs::remove(dest, ec);
        fs::rename(part, dest, ec);
        if (ec) {
            fs::remove(part, ec);
            out.error = "não foi possível finalizar o arquivo";
            out.bytes = 0;
            return out;
        }
        out.ok = true;
        return out;
    }

private:
    std::shared_ptr<net::IHttpClient> client_;
};

}  // namespace

std::unique_ptr<IHttpDownloader> makeDownloaderOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_unique<NetBackedDownloader>(std::move(client));
}

}  // namespace hb::marketplace
