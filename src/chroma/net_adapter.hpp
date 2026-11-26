#pragma once
// Adapter: chroma's IHttpClient (model download) on top of the shared hb::net::IHttpClient. Portable, so it is
// unit-tested with hb::net::FakeHttpClient. HTTPS only, redirects followed without downgrade, status 200 only.
#include <memory>

#include "chroma/model_store.hpp"
#include "net/http.hpp"

namespace hb::chroma {

std::unique_ptr<IHttpClient> makeChromaHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client);

}  // namespace hb::chroma
