// Windows factory of the marketplace downloader: the portable adapter (net_adapter.cpp) over the shared
// WinHTTP client of src/net. NOT VALIDATED: written without access to a Windows compiler (docs/MARKETPLACE.md).
//
// Security properties (by construction, review these first):
//   * only https:// URLs that pass isSafeHttpsUrl() are accepted (net_adapter.cpp);
//   * src/net never overrides certificate / host name verification (there is deliberately no switch);
//   * TLS 1.2 / 1.3 only (set below); redirects capped at 5 and never allowed to downgrade to http;
//   * the size cap is enforced while reading, not after;
//   * the file is written to "<dest>.part" and renamed only when complete.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include "marketplace/net_adapter.hpp"

namespace hb::marketplace {

std::unique_ptr<IHttpDownloader> makeDefaultDownloader() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-Marketplace/1.0";
    o.proxy = hb::net::ProxyMode::Automatic;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    o.secureProtocols = protocols;
    return makeDownloaderOverNet(hb::net::makeWinHttpClient(o));
}

}  // namespace hb::marketplace

#endif  // _WIN32
