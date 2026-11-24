#pragma once
// S3-compatible object storage provider (AWS S3, Backblaze B2, Cloudflare R2, Wasabi, MinIO...):
// SigV4 signing + multipart upload. Pure request building/response parsing on top of ICloudHttp, so
// it is fully unit-tested with a fake transport. NOT validated against any real service (see
// docs/BACKUP_NUVEM.md).
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "cloud/cloud_types.hpp"

namespace hb::cloud {

struct S3Config {
    std::string endpoint;            // https://s3.us-east-005.backblazeb2.com  |  https://<acct>.r2.cloudflarestorage.com ...
    std::string region = "us-east-1";
    std::string bucket;
    std::string prefix;              // optional "folder", e.g. "igreja/cultos"
    std::string addressing = "auto"; // auto | path | virtual
    std::string accessKey, secretKey;
    std::uint64_t partSize = 8ull * 1024 * 1024;  // 5 MiB .. 512 MiB; scaled up automatically for huge files
};

// "" if usable, else a pt-BR explanation of what is wrong (never echoes the keys).
std::string validateS3Config(const S3Config& c);

inline constexpr std::uint64_t kS3MinPartSize = 5ull * 1024 * 1024;       // S3 limit (except the last part)
inline constexpr std::uint64_t kS3MaxParts = 10000;                        // S3 limit
inline constexpr std::uint64_t kS3MaxObjectSize = 5ull * 1024 * 1024 * 1024 * 1024;  // 5 TiB

class S3Provider : public ICloudProvider {
public:
    using NowFn = std::function<std::chrono::system_clock::time_point()>;
    S3Provider(S3Config cfg, std::shared_ptr<ICloudHttp> http, NowFn now = [] { return std::chrono::system_clock::now(); });

    const char* id() const override { return "s3"; }
    Result<std::string> test() override;
    Result<RemoteStat> stat(const FileInfo& f) override;
    Result<UploadSession> begin(const FileInfo& f) override;
    Result<std::uint64_t> committed(UploadSession& s, const FileInfo& f) override;
    Result<ChunkAck> putChunk(UploadSession& s, const FileInfo& f, std::uint64_t offset, std::string_view data, ITransferGate* gate) override;
    Result<RemoteStat> finish(UploadSession& s, const FileInfo& f, const std::string& localMd5Hex) override;
    void abort(const UploadSession& s, const FileInfo& f) noexcept override;

    // Exposed for tests: the object key for a remote name (prefix applied).
    std::string objectKey(const std::string& remoteName) const;
    // Chunk size chosen for a file of `size` bytes (honours the 10000-part limit).
    std::uint64_t chunkSizeFor(std::uint64_t size) const;

private:
    Result<HttpResponse> call(const std::string& method, const std::string& key, std::vector<KV> query, std::string body,
                              std::vector<KV> extraHeaders, ITransferGate* gate, std::chrono::milliseconds timeout);

    S3Config cfg_;
    std::shared_ptr<ICloudHttp> http_;
    NowFn now_;
    std::string scheme_, host_;  // endpoint host (without bucket)
    int port_ = 0;
    bool virtualHost_ = false;
};

}  // namespace hb::cloud
