#include "ai/net_adapter.hpp"

#include <algorithm>

namespace hb::ai {
namespace {

class NetBackedAiHttp : public IAiHttp {
public:
    explicit NetBackedAiHttp(std::shared_ptr<hb::net::IHttpClient> c) : client_(std::move(c)) {}

    HttpResponse send(const HttpRequest& req) override {
        HttpResponse out;
        try {
            sendImpl(req, out);
        } catch (...) {
            out = HttpResponse{};
            out.transportError = "erro inesperado no transporte HTTP";
        }
        return out;
    }

private:
    void sendImpl(const HttpRequest& req, HttpResponse& out) {
        if (!client_) {
            out.transportError = "transporte HTTP indisponível nesta plataforma";
            return;
        }
        namespace net = hb::net;
        net::HttpRequest n;
        n.method = req.method;
        n.url = req.url;
        n.headers = req.headers;
        n.body = req.body;
        const int t = static_cast<int>(std::min<long long>(std::max<long long>(req.timeout.count(), 1000), 600000));
        n.timeouts = net::Timeouts::all(t);
        n.bufferedBodyDeadline = std::max(req.timeout, std::chrono::milliseconds(1));
        n.maxResponseBytes = kMaxAiResponseBytes;
        n.overflow = net::Overflow::Fail;
        if (req.onData)
            n.onData = [&req](const char* d, std::size_t len, std::uint64_t) { return req.onData(std::string(d, len)); };

        const net::HttpResponse r = client_->send(n);
        out.status = r.status;
        out.body = r.body;
        const std::string ra = r.header("Retry-After");
        if (!ra.empty()) out.headers.emplace_back("Retry-After", ra);

        switch (r.error) {
            case net::ErrorKind::None:
            case net::ErrorKind::Cancelled:  // the streaming consumer aborted on purpose: a normal end
                return;
            case net::ErrorKind::InvalidRequest:
                out.status = 0;
                out.transportError = "endereço inválido";
                return;
            case net::ErrorKind::TooLarge:
                out.status = 0;
                out.transportError = "resposta grande demais";
                return;
            case net::ErrorKind::Timeout:
            case net::ErrorKind::Transport:
                out.status = 0;
                out.timedOut = r.error == net::ErrorKind::Timeout;
                if (!r.errorStage.empty()) {
                    out.transportError = r.errorStage + " falhou";
                    if (r.nativeError != 0) out.transportError += " (WinHTTP " + std::to_string(r.nativeError) + ")";
                } else if (out.timedOut) {
                    out.transportError = "tempo esgotado";
                } else if (r.errorText.find("unavailable") != std::string::npos) {
                    out.transportError = "transporte HTTP indisponível nesta plataforma";
                } else {
                    out.transportError = "erro inesperado no transporte HTTP";
                }
                return;
        }
    }

    std::shared_ptr<hb::net::IHttpClient> client_;
};

}  // namespace

std::shared_ptr<IAiHttp> makeAiHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_shared<NetBackedAiHttp>(std::move(client));
}

std::shared_ptr<IAiHttp> makeDefaultAiHttp() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-AI/1.0";
    o.proxy = hb::net::ProxyMode::Automatic;
    return makeAiHttpOverNet(hb::net::makeWinHttpClient(o));
}

}  // namespace hb::ai
