#ifndef _WIN32
#include "cloud/cloud_platform.hpp"

namespace hb::cloud {

namespace {
class NoHttp : public ICloudHttp {
public:
    Result<HttpResponse> send(const HttpRequest&) override {
        return makeError(ErrorKind::Permanent, "Transporte HTTPS indisponível nesta plataforma (o backup em nuvem só funciona no Windows)");
    }
};
}  // namespace

std::shared_ptr<ICloudHttp> makePlatformHttp() { return std::make_shared<NoHttp>(); }
std::shared_ptr<ISecretStore> makePlatformSecretStore(const std::string&) { return nullptr; }
std::shared_ptr<IFileSystem> makePlatformFileSystem() { return makeStdFileSystem(); }
void setBackgroundThreadPriority() {}

}  // namespace hb::cloud
#endif
