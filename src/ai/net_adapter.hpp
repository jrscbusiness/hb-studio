#pragma once
// Adapter: the AI client's small IAiHttp seam on top of the shared hb::net::IHttpClient. Portable, so it is
// unit-tested with hb::net::FakeHttpClient. The AI-specific policy (timeouts, 8 MiB cap, Retry-After,
// SSE streaming only for 2xx) lives here; the WinHTTP code itself lives once in src/net.
#include <memory>

#include "ai/http.hpp"
#include "net/http.hpp"

namespace hb::ai {

std::shared_ptr<IAiHttp> makeAiHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client);

}  // namespace hb::ai
