#include "captions/net_adapter.hpp"

#include <filesystem>
#include <fstream>

namespace hb::captions {
namespace {

std::string netErr(const hb::net::HttpResponse& r, const char* what) {
    return std::string(what) + " (erro WinHTTP " + std::to_string(r.nativeError) + ")";
}

}  // namespace

bool NetBackedHttp::download(const std::string& url, const std::string& destPathUtf8,
                             const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string& error) {
    namespace net = hb::net;
    if (!client_) { error = "endereço de download inválido"; return false; }
    const std::filesystem::path dest{std::u8string(destPathUtf8.begin(), destPathUtf8.end())};

    net::HttpRequest n;
    n.method = "GET";
    n.url = url;
    n.httpsOnly = true;
    n.timeouts = net::Timeouts{15000, 15000, 30000, 60000};
    n.redirects = net::RedirectPolicy::FollowNoDowngrade;  // Hugging Face -> its CDN; https -> http refused
    n.sinkMinStatus = n.sinkMaxStatus = 200;
    n.readBodyOutsideSink = false;

    std::ofstream out;
    std::string sinkError;  // set when the sink itself stops the transfer
    std::uint64_t got = 0;
    auto openOut = [&]() -> bool {
        if (out.is_open()) return true;
        out.open(dest, std::ios::binary | std::ios::trunc);
        if (!out) { sinkError = "não foi possível criar o arquivo do modelo"; return false; }
        return true;
    };
    n.onData = [&](const char* d, std::size_t len, std::uint64_t total) {
        if (!openOut()) return false;
        out.write(d, static_cast<std::streamsize>(len));
        if (!out) { sinkError = "disco cheio ou sem permissão de escrita"; return false; }
        got += len;
        if (progress && !progress(got, total)) { sinkError = "cancelado"; return false; }
        return true;
    };

    const net::HttpResponse r = client_->send(n);
    switch (r.error) {
        case net::ErrorKind::None:
            break;
        case net::ErrorKind::InvalidRequest:
            error = "endereço de download inválido";
            return false;
        case net::ErrorKind::Cancelled:
            error = sinkError.empty() ? "cancelado" : sinkError;
            return false;
        default:
            error = netErr(r, r.errorStage.empty() ? "falha de rede" : "sem resposta do servidor");
            return false;
    }
    if (r.status != 200) { error = "servidor respondeu HTTP " + std::to_string(r.status); return false; }
    if (!openOut()) { error = sinkError; return false; }  // empty body: the file is still created
    out.close();
    if (r.contentLength != 0 && got != r.contentLength) { error = "download incompleto"; return false; }
    return true;
}

HttpResponse NetBackedHttp::post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                                 const std::string& body, int timeoutMs) {
    namespace net = hb::net;
    HttpResponse resp;
    if (!client_) { resp.error = "endereço inválido"; return resp; }
    net::HttpRequest n;
    n.method = "POST";
    n.url = url;
    n.headers = headers;
    n.body = body;
    const int to = timeoutMs < 1000 ? 1000 : (timeoutMs > 120000 ? 120000 : timeoutMs);
    n.timeouts = net::Timeouts::all(to);
    n.maxResponseBytes = 4u << 20;  // a transcription reply is tiny; refuse anything huge
    n.overflow = net::Overflow::Truncate;
    const net::HttpResponse r = client_->send(n);
    resp.status = r.status;
    resp.body = r.body;
    if (r.error == net::ErrorKind::InvalidRequest) resp.error = "endereço inválido";
    else if (r.error != net::ErrorKind::None) resp.error = netErr(r, r.status == 0 ? "sem resposta" : "leitura");
    return resp;
}

}  // namespace hb::captions
