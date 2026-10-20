#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rtsp/types.hpp"

namespace hb::rtsp {

struct Challenge {
    enum class Scheme { Basic, Digest } scheme = Scheme::Basic;
    std::string realm, nonce, opaque, algorithm;  // algorithm empty = MD5
    std::vector<std::string> qop;                 // e.g. {"auth","auth-int"}
    bool stale = false;
};

// Parses one or more WWW-Authenticate header values (each may hold several challenges).
// Unknown schemes are skipped.
std::vector<Challenge> parseChallenges(const std::vector<std::string>& headerValues);

// RFC 2617/7616 digest response (MD5 / MD5-sess). qop empty = legacy RFC 2069 form.
std::string digestResponse(const Challenge& c, const Credentials& cred, std::string_view method,
                           std::string_view uri, std::string_view qop, std::string_view nc,
                           std::string_view cnonce);

std::string basicHeaderValue(const Credentials& cred);  // "Basic base64(user:pass)"

// Holds the credentials and the last server challenge and produces the Authorization header
// for every following request. Passwords are never exposed through any accessor or log.
class Authenticator {
public:
    using NonceGen = std::function<std::string()>;
    Authenticator(Credentials cred, NonceGen cnonce) : cred_(std::move(cred)), cnonce_(std::move(cnonce)) {}

    // Digest is preferred over Basic. Returns false when no usable challenge is present
    // (e.g. only SHA-256 digest, or no credentials configured).
    bool setChallenge(const std::vector<std::string>& wwwAuthenticate);
    bool hasChallenge() const { return have_; }
    bool hasCredentials() const { return !cred_.empty(); }
    bool lastChallengeStale() const { return chal_.stale; }
    // Empty string if there is no challenge yet.
    std::string header(std::string_view method, std::string_view uri);

private:
    Credentials cred_;
    NonceGen cnonce_;
    Challenge chal_;
    bool have_ = false;
    unsigned nc_ = 0;
    std::string cnonceValue_;
};

}  // namespace hb::rtsp
