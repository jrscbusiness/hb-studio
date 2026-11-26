#include "chroma/net_adapter.hpp"

namespace hb::chroma {
namespace {

class NetBackedChromaHttp : public IHttpClient {
public:
    explicit NetBackedChromaHttp(std::shared_ptr<hb::net::IHttpClient> c) : client_(std::move(c)) {}

    HttpResult get(const std::string& url,
                   const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>& onData) override {
        namespace net = hb::net;
        HttpResult r;
        if (!client_) {
            r.error = "Falha de rede";
            return r;
        }
        net::HttpRequest n;
        n.method = "GET";
        n.url = url;
        n.httpsOnly = true;  // never plain HTTP for a download we then execute
        n.timeouts = net::Timeouts{10000, 10000, 20000, 30000};
        n.redirects = net::RedirectPolicy::FollowNoDowngrade;
        n.sinkMinStatus = n.sinkMaxStatus = 200;
        n.readBodyOutsideSink = false;
        n.onData = [&](const char* d, std::size_t len, std::uint64_t total) {
            return onData(reinterpret_cast<const std::uint8_t*>(d), len, total);
        };
        const net::HttpResponse resp = client_->send(n);
        r.status = resp.status;
        switch (resp.error) {
            case net::ErrorKind::None:
                break;
            case net::ErrorKind::InvalidRequest:
                r.error = resp.errorText.find("https only") != std::string::npos ? "Somente HTTPS é permitido" : "URL inválida";
                return r;
            case net::ErrorKind::Cancelled:
                r.error = "Interrompido";  // caller aborted (cancel / size cap): ok stays false
                return r;
            default:
                r.error = "Falha de rede (" + (resp.errorStage.empty() ? resp.errorText : resp.errorStage) + ", erro WinHTTP " +
                          std::to_string(resp.nativeError) + ")";
                return r;
        }
        if (resp.status != 200) {
            r.error = "HTTP " + std::to_string(resp.status);
            return r;
        }
        r.ok = true;
        return r;
    }

private:
    std::shared_ptr<hb::net::IHttpClient> client_;
};

}  // namespace

std::unique_ptr<IHttpClient> makeChromaHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_unique<NetBackedChromaHttp>(std::move(client));
}

}  // namespace hb::chroma
