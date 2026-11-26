// NOT VALIDATED: written without access to a Windows compiler; verify on Windows CI / real hardware.
#ifdef _WIN32
#include "captions/windows/winhttp_client.hpp"

namespace hb::captions::win {

namespace {
std::shared_ptr<hb::net::IHttpClient> makeClient() {
    hb::net::HttpClientOptions o;
    o.userAgent = "HBStudio-Captions/1.0";
    return hb::net::makeWinHttpClient(o);
}
}  // namespace

WinHttpClient::WinHttpClient() : NetBackedHttp(makeClient()) {}

}  // namespace hb::captions::win
#endif
