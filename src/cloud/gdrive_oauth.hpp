#pragma once
// Google OAuth2 "installed app" flow with PKCE and a loopback redirect (RFC 8252 / RFC 7636), using
// the USER'S OWN Google Cloud client id. Scope: drive.file (only files this app creates).
// Pure request builders + response parsers + a token cache; the socket listener and the browser
// launch live elsewhere (loopback_listener.*, app code). NOT validated against Google.
#include <functional>
#include <memory>
#include <string>

#include "cloud/cloud_types.hpp"

namespace hb::cloud {

inline constexpr const char* kDriveScope = "https://www.googleapis.com/auth/drive.file";
inline constexpr const char* kGoogleAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
inline constexpr const char* kGoogleTokenEndpoint = "https://oauth2.googleapis.com/token";

struct GoogleOAuthConfig {
    std::string clientId;
    std::string clientSecret;  // Desktop-type clients still get one; Google asks for it on token calls
};

struct TokenSet {
    std::string accessToken;
    std::string refreshToken;  // empty on a refresh response (Google reuses the old one)
    long expiresInSec = 0;
};

// Parses a token endpoint response. Errors: invalid_grant -> Auth ("reautorize"), 5xx -> Transient.
Result<TokenSet> parseTokenResponse(const HttpResponse& r);
HttpRequest refreshTokenRequest(const GoogleOAuthConfig& cfg, const std::string& refreshToken);

using RandomBytesFn = std::function<std::string(std::size_t)>;  // cryptographically secure bytes

class GoogleOAuthFlow {
public:
    GoogleOAuthFlow(GoogleOAuthConfig cfg, RandomBytesFn random);

    // Authorization URL for the browser; `port` is the loopback listener's port. Creates the PKCE
    // verifier and the anti-CSRF state (one flow = one use).
    std::string authUrl(int port);

    struct Redirect {
        bool ok = false;
        std::string code;
        std::string error;  // pt-BR
    };
    // `requestLine` = "GET /?code=...&state=... HTTP/1.1" received on the loopback listener.
    Redirect parseRedirect(const std::string& requestLine) const;

    HttpRequest tokenRequest(const std::string& code) const;

    const std::string& verifier() const { return verifier_; }
    const std::string& state() const { return state_; }

private:
    GoogleOAuthConfig cfg_;
    std::string verifier_, challenge_, state_, redirectUri_;
    RandomBytesFn random_;
};

// Caches the short-lived access token; refreshes it shortly before expiry. Thread-safe.
class GoogleTokenSource {
public:
    using NowFn = std::function<double()>;  // monotonic seconds
    GoogleTokenSource(GoogleOAuthConfig cfg, std::string refreshToken, std::shared_ptr<ICloudHttp> http, NowFn now);
    Result<std::string> accessToken();
    void invalidate();  // after a 401

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace hb::cloud
