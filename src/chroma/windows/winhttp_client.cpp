#include "chroma/windows/winhttp_client.hpp"

#include "chroma/net_adapter.hpp"
#include "net/http.hpp"

namespace hb::chroma {

WinHttpClient::WinHttpClient() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio";
    impl_ = makeChromaHttpOverNet(hb::net::makeWinHttpClient(o));
}

}  // namespace hb::chroma
