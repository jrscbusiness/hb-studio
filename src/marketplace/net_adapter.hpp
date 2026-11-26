#pragma once
// Adapter: the marketplace's IHttpDownloader on top of the shared hb::net::IHttpClient. Portable, so it is
// unit-tested with hb::net::FakeHttpClient. Policy kept here: only https URLs that pass isSafeHttpsUrl(),
// redirects capped at 5 (never https -> http), size cap enforced while receiving, a total time budget,
// "<dest>.part" renamed only when complete, pt-BR messages.
#include <memory>

#include "marketplace/downloader.hpp"
#include "net/http.hpp"

namespace hb::marketplace {

std::unique_ptr<IHttpDownloader> makeDownloaderOverNet(std::shared_ptr<hb::net::IHttpClient> client);

}  // namespace hb::marketplace
