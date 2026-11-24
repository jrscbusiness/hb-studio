#pragma once
// Shared vocabulary of the cloud-backup module: errors, HTTP transport abstraction, the provider
// interface. Everything here is portable (GCC/MSVC) and free of network/OS dependencies.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hb::cloud {

using KV = std::pair<std::string, std::string>;

// How the upload engine reacts to a failure.
enum class ErrorKind {
    Transient,    // network, 5xx, 429, timeouts: retry with exponential backoff
    Auth,         // bad/expired credentials, clock skew: needs the user (no automatic retry)
    Quota,        // remote storage full / daily limit / account suspended: needs the user
    SessionLost,  // the remote multipart/resumable session no longer exists: restart that file
    Local,        // local file missing/unreadable: needs the user
    Permanent,    // anything else that retrying cannot fix (bad bucket name, object too large...)
    Cancelled,    // aborted on purpose (cancel, pause while streaming, kill switch): not a failure
};

const char* errorKindName(ErrorKind k);

struct Error {
    ErrorKind kind = ErrorKind::Transient;
    std::string message;  // pt-BR, safe to show in the UI (never contains secrets)
    int httpStatus = 0;
};

inline Error makeError(ErrorKind k, std::string msg, int http = 0) {
    Error e;
    e.kind = k;
    e.message = std::move(msg);
    e.httpStatus = http;
    return e;
}

struct Unit {};

template <class T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : v_(std::move(error)) {}  // NOLINT(google-explicit-constructor)
    bool ok() const { return std::holds_alternative<T>(v_); }
    explicit operator bool() const { return ok(); }
    const T& value() const { return std::get<T>(v_); }
    T& value() { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }

private:
    std::variant<T, Error> v_;
};

// Called by the transport right BEFORE it puts `bytes` more request-body bytes on the wire.
// Implementations throttle (block) and return false to abort the transfer (cancel, pause because
// the live stream started, kill switch). Must be thread-safe.
class ITransferGate {
public:
    virtual ~ITransferGate() = default;
    virtual bool acquire(std::size_t bytes) = 0;
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;  // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers;  // Host/Content-Length are added by the transport
    std::string body;
    std::chrono::milliseconds timeout{60000};
    ITransferGate* gate = nullptr;  // optional; consulted while sending the body
};

struct HttpResponse {
    int status = 0;
    std::string body;  // capped by the transport (4 MiB)
    std::vector<std::pair<std::string, std::string>> headers;
};

// Case-insensitive header lookup; "" if absent.
std::string headerValue(const HttpResponse& r, std::string_view name);

// Transport failures (DNS, TLS, connection reset, timeout) are Error{Transient}; an abort requested
// through the gate is Error{Cancelled}. ANY HTTP status (4xx/5xx too) is a successful HttpResponse.
class ICloudHttp {
public:
    virtual ~ICloudHttp() = default;
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

// ---- provider-facing model ----

struct FileInfo {
    std::string path;        // local UTF-8 path
    std::string remoteName;  // object name relative to the provider's root, '/' separated
    std::uint64_t size = 0;
    std::string contentType = "application/octet-stream";
};

struct ChunkAck {
    std::uint32_t index = 0;      // S3: part number (1-based)
    std::uint64_t offset = 0;
    std::uint64_t size = 0;       // bytes of this chunk (as sent)
    std::uint64_t accepted = 0;   // bytes the server durably accepted from this chunk (== size normally)
    std::string etag;             // as returned (unquoted)
    std::string md5;              // hex MD5 of the chunk (S3)
    std::string sha256;           // hex SHA-256 of the chunk (S3)
    bool done = false;            // the whole object is complete (resumable providers' last chunk)
    std::string remoteId;         // Drive file id
    std::string remoteMd5;        // Drive md5Checksum of the whole file
};

// Provider-defined state of one resumable upload, persisted in the journal so it survives restarts.
struct UploadSession {
    std::string id;     // S3 UploadId ("" = single PUT); unused for Drive
    std::string uri;    // Drive resumable session URI
    std::uint64_t chunkSize = 0;
    std::vector<ChunkAck> acks;  // S3: the parts already stored (needed to complete the upload)
    std::string remoteId;        // Drive file id once done
    std::string remoteMd5;
};

struct RemoteStat {
    bool exists = false;
    std::uint64_t size = 0;
    std::string etag;
    std::string md5;
    std::string remoteId;
    std::string note;  // e.g. "ETag final não confere" (soft integrity warnings)
};

class ICloudProvider {
public:
    virtual ~ICloudProvider() = default;
    virtual const char* id() const = 0;  // "s3" | "gdrive"
    // Verifies credentials/bucket by doing a real (tiny) round trip. Returns a pt-BR message.
    virtual Result<std::string> test() = 0;
    // For deduplication: does the object already exist (with its size)?
    virtual Result<RemoteStat> stat(const FileInfo& f) = 0;
    virtual Result<UploadSession> begin(const FileInfo& f) = 0;
    // Bytes safely stored contiguously from offset 0 (resume after a restart). May shrink s.acks.
    virtual Result<std::uint64_t> committed(UploadSession& s, const FileInfo& f) = 0;
    virtual Result<ChunkAck> putChunk(UploadSession& s, const FileInfo& f, std::uint64_t offset, std::string_view data,
                                      ITransferGate* gate) = 0;
    // Completes the object (S3: CompleteMultipartUpload) and verifies it. `localMd5Hex` is the MD5
    // of the whole local file, "" unless verifiesWholeFileMd5().
    virtual Result<RemoteStat> finish(UploadSession& s, const FileInfo& f, const std::string& localMd5Hex) = 0;
    virtual bool verifiesWholeFileMd5() const { return false; }
    virtual void abort(const UploadSession& s, const FileInfo& f) noexcept = 0;  // best effort
};

}  // namespace hb::cloud
