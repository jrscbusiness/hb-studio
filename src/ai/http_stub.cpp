// Non-Windows build: there is no real transport (the app is Windows-only); portable code and tests
// use fakes. This keeps the library linkable and fails with a clear message if ever used.
#ifndef _WIN32

#include "ai/http.hpp"

namespace hb::ai {
namespace {

class UnavailableHttp : public IAiHttp {
public:
    HttpResponse send(const HttpRequest&) override {
        HttpResponse r;
        r.transportError = "transporte HTTP indisponível nesta plataforma";
        return r;
    }
};

}  // namespace

std::shared_ptr<IAiHttp> makeDefaultAiHttp() { return std::make_shared<UnavailableHttp>(); }

}  // namespace hb::ai

#endif  // !_WIN32
