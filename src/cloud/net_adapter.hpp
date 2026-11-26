#pragma once
// Adapters of the cloud module's small seams (ICloudHttp, name-keyed ISecretStore) onto the shared src/net
// layer. Portable, so they are unit-tested with hb::net::FakeHttpClient / FakeSecretStore. The cloud policy
// (no redirects, 15/30/60 s timeouts, 4 MiB reply cap, bandwidth gate per 32 KiB slice) lives here; the
// WinHTTP and DPAPI code lives once in src/net.
#include <memory>
#include <string>

#include "cloud/cloud_settings.hpp"
#include "cloud/cloud_types.hpp"
#include "net/http.hpp"
#include "net/secret_store.hpp"

namespace hb::cloud {

std::shared_ptr<ICloudHttp> makeCloudHttpOverNet(std::shared_ptr<hb::net::IHttpClient> client);

// Name-keyed store persisted at `path` (UTF-8): a JSON file {"v":1,"items":{name: base64(blob)}} written via
// tmp + rename, where each blob is protected by `protector` (DPAPI on Windows).
std::shared_ptr<ISecretStore> makeFileSecretStore(const std::string& path, std::shared_ptr<hb::net::ISecretStore> protector);

}  // namespace hb::cloud
