#pragma once
// Adapter: the ONVIF client's IHttpClient seam on top of the shared hb::net::IHttpClient (portable, so it is
// unit-tested with hb::net::FakeHttpClient). On Windows makeDefaultHttpClient() uses it over WinHTTP.
#include <memory>

#include "net/http.hpp"
#include "onvif/http_client.hpp"

namespace hb::onvif {

std::unique_ptr<IHttpClient> makeOnvifHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client);

}  // namespace hb::onvif
