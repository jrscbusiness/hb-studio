#include "cloud/sigv4.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "cloud/hash_util.hpp"

namespace hb::cloud {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Trim + collapse runs of spaces (SigV4 header value canonicalisation).
std::string trimAll(const std::string& v) {
    std::string o;
    bool space = false;
    for (char c : v) {
        if (c == ' ' || c == '\t') {
            space = !o.empty();
            continue;
        }
        if (space) o += ' ';
        space = false;
        o += c;
    }
    return o;
}

}  // namespace

std::string canonicalQueryString(const std::vector<KV>& query) {
    std::vector<KV> enc;
    enc.reserve(query.size());
    for (const auto& kv : query) enc.emplace_back(uriEncode(kv.first, true), uriEncode(kv.second, true));
    std::sort(enc.begin(), enc.end());
    std::string o;
    for (std::size_t i = 0; i < enc.size(); ++i) {
        if (i) o += '&';
        o += enc[i].first + "=" + enc[i].second;
    }
    return o;
}

std::string formatAmzDate(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const long long secs = duration_cast<seconds>(t.time_since_epoch()).count();
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    // Howard Hinnant's civil_from_days.
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld%02lld%02lldT%02lld%02lld%02lldZ", y, m, d, rem / 3600, (rem % 3600) / 60, rem % 60);
    return buf;
}

SigV4Output signV4(const SigV4Input& in) {
    SigV4Output out;
    std::vector<KV> hs;
    for (const auto& h : in.headers) hs.emplace_back(lower(h.first), trimAll(h.second));
    std::sort(hs.begin(), hs.end());
    std::string canonHeaders;
    for (std::size_t i = 0; i < hs.size(); ++i) {
        canonHeaders += hs[i].first + ":" + hs[i].second + "\n";
        if (i) out.signedHeaders += ';';
        out.signedHeaders += hs[i].first;
    }
    out.canonicalRequest = in.method + "\n" + in.canonicalPath + "\n" + canonicalQueryString(in.query) + "\n" + canonHeaders + "\n" +
                           out.signedHeaders + "\n" + in.payloadHash;
    const std::string date = in.amzDate.substr(0, 8);
    const std::string scope = date + "/" + in.region + "/" + in.service + "/aws4_request";
    out.stringToSign = "AWS4-HMAC-SHA256\n" + in.amzDate + "\n" + scope + "\n" + sha256Hex(out.canonicalRequest);
    const std::string kDate = hmacSha256("AWS4" + in.secretKey, date);
    const std::string kRegion = hmacSha256(kDate, in.region);
    const std::string kService = hmacSha256(kRegion, in.service);
    const std::string kSigning = hmacSha256(kService, "aws4_request");
    out.signature = hexEncode(hmacSha256(kSigning, out.stringToSign));
    out.authorization = "AWS4-HMAC-SHA256 Credential=" + in.accessKey + "/" + scope + ", SignedHeaders=" + out.signedHeaders +
                        ", Signature=" + out.signature;
    return out;
}

}  // namespace hb::cloud
