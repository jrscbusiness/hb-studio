#pragma once
// AWS Signature Version 4 (Authorization header flavour). Pure functions, verified in the unit tests
// against the official AWS SigV4 test-suite vectors (get-vanilla, post-vanilla, header trimming,
// query ordering, UTF-8 path, form-urlencoded POST) and the S3 documentation examples.
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "cloud/cloud_types.hpp"

namespace hb::cloud {

struct SigV4Input {
    std::string method;         // "GET", "PUT"...
    std::string canonicalPath;  // the path exactly as it will be sent: already percent-encoded
    std::vector<KV> query;      // RAW (unencoded) names/values; sorted + encoded here
    std::vector<KV> headers;    // the headers to sign; MUST include host and x-amz-date
    std::string payloadHash;    // lowercase hex SHA-256 of the body, or "UNSIGNED-PAYLOAD"
    std::string accessKey, secretKey;
    std::string region, service;  // service: "s3"
    std::string amzDate;          // YYYYMMDDTHHMMSSZ
};

struct SigV4Output {
    std::string canonicalRequest;
    std::string stringToSign;
    std::string signature;      // hex
    std::string signedHeaders;  // "host;x-amz-content-sha256;x-amz-date"
    std::string authorization;  // full Authorization header value
};

SigV4Output signV4(const SigV4Input& in);

// name=value pairs encoded (RFC 3986, space = %20), sorted by name then value, joined by '&'.
std::string canonicalQueryString(const std::vector<KV>& query);

// "YYYYMMDDTHHMMSSZ" in UTC for the given instant.
std::string formatAmzDate(std::chrono::system_clock::time_point t);

}  // namespace hb::cloud
