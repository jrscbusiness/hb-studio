#pragma once
// Platform pieces of the cloud module (Windows: WinHTTP, DPAPI, exclusive-open probe, background
// thread priority). The Windows implementations are in cloud_platform_win.cpp and are NOT VALIDATED
// (written without a Windows compiler); the POSIX file has safe stand-ins so the module still builds.
#include <memory>
#include <string>

#include "cloud/cloud_settings.hpp"
#include "cloud/cloud_types.hpp"
#include "cloud/file_system.hpp"

namespace hb::cloud {

// WinHTTP client streaming the body in slices through the ITransferGate. POSIX: a client that
// always fails with a clear "indisponível nesta plataforma" error.
std::shared_ptr<ICloudHttp> makePlatformHttp();

// DPAPI (CryptProtectData, current user) backed store persisted at `path`. POSIX: nullptr
// (callers fall back to MemorySecretStore, i.e. credentials are not persisted).
std::shared_ptr<ISecretStore> makePlatformSecretStore(const std::string& path);

// std::filesystem + a Windows exclusive-open probe for isLocked().
std::shared_ptr<IFileSystem> makePlatformFileSystem();

// Lowers CPU + I/O priority of the calling thread (Windows: THREAD_MODE_BACKGROUND_BEGIN).
void setBackgroundThreadPriority();

}  // namespace hb::cloud
