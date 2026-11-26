#include "onvif/net_adapter.hpp"

#include <algorithm>

namespace hb::onvif {
namespace {

class NetBackedHttp : public IHttpClient {
public:
    explicit NetBackedHttp(std::shared_ptr<hb::net::IHttpClient> c) : client_(std::move(c)) {}

    Result<HttpResponse> send(const HttpRequest& req) override {
        namespace net = hb::net;
        if (!client_) return makeError(ErrorCode::Transport, "HTTP client unavailable");
        net::HttpRequest n;
        n.method = req.method;
        n.url = req.url;
        n.headers = req.headers;
        n.body = req.body;
        const int t = static_cast<int>(std::min<long long>(req.timeout.count(), 600000));
        n.timeouts = net::Timeouts::all(t);
        n.maxResponseBytes = kMaxHttpResponseBytes;
        n.overflow = net::Overflow::Fail;
        const net::HttpResponse r = client_->send(n);
        switch (r.error) {
            case net::ErrorKind::None:
                break;
            case net::ErrorKind::InvalidRequest:
                return makeError(ErrorCode::InvalidArgument, r.errorText);
            case net::ErrorKind::Timeout:
                return makeError(ErrorCode::Timeout, r.errorText);
            default:
                return makeError(ErrorCode::Transport, r.errorText);
        }
        HttpResponse out;
        out.status = r.status;
        out.body = r.body;
        out.headers = r.headers;
        return out;
    }

private:
    std::shared_ptr<hb::net::IHttpClient> client_;
};

}  // namespace

std::unique_ptr<IHttpClient> makeOnvifHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client) {
    return std::make_unique<NetBackedHttp>(std::move(client));
}

#ifdef _WIN32
std::unique_ptr<IHttpClient> makeDefaultHttpClient() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-ONVIF/1.0";
    o.proxy = hb::net::ProxyMode::None;  // cameras live on the LAN
    return makeOnvifHttpOverNet(hb::net::makeWinHttpClient(o));
}
#endif

}  // namespace hb::onvif
