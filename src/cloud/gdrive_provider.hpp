#pragma once
// Google Drive provider: resumable upload (Drive API v3), scope drive.file. Files go to a folder
// "HB Studio" (created on demand; with drive.file the app only sees what it created). Request
// building / response parsing only: tested with a fake transport. NOT validated against Google.
#include <memory>
#include <string>

#include "cloud/cloud_types.hpp"
#include "cloud/gdrive_oauth.hpp"

namespace hb::cloud {

struct GDriveConfig {
    std::string folderName = "HB Studio";
    std::string folderId;  // cached after the first creation/lookup (persisted by the service)
    std::uint64_t chunkSize = 8ull * 1024 * 1024;  // multiple of 256 KiB (Drive requirement)
};

class GDriveProvider : public ICloudProvider {
public:
    GDriveProvider(GDriveConfig cfg, std::shared_ptr<ICloudHttp> http, std::shared_ptr<GoogleTokenSource> tokens);

    const char* id() const override { return "gdrive"; }
    Result<std::string> test() override;
    Result<RemoteStat> stat(const FileInfo& f) override;
    Result<UploadSession> begin(const FileInfo& f) override;
    Result<std::uint64_t> committed(UploadSession& s, const FileInfo& f) override;
    Result<ChunkAck> putChunk(UploadSession& s, const FileInfo& f, std::uint64_t offset, std::string_view data, ITransferGate* gate) override;
    Result<RemoteStat> finish(UploadSession& s, const FileInfo& f, const std::string& localMd5Hex) override;
    bool verifiesWholeFileMd5() const override { return true; }
    void abort(const UploadSession& s, const FileInfo& f) noexcept override;

    const std::string& folderId() const { return cfg_.folderId; }
    // Drive file name for a remote name: the date/kind folders are flattened into the name
    // ("2026-11-24/program/a.mp4" -> "2026-11-24_program_a.mp4").
    static std::string driveName(const std::string& remoteName);

private:
    Result<HttpResponse> authed(HttpRequest req);  // adds the bearer token, refreshes once on 401
    Result<std::string> ensureFolder();

    GDriveConfig cfg_;
    std::shared_ptr<ICloudHttp> http_;
    std::shared_ptr<GoogleTokenSource> tokens_;
};

}  // namespace hb::cloud
