#pragma once
// OAuth 2.0 for installed apps (RFC 8252 loopback redirect + PKCE RFC 7636), as used for the
// YouTube Live Streaming API. The user supplies their OWN Google Cloud OAuth client id (Desktop
// app type); HB Studio ships no client id. Pure logic + a token manager over injected transports.
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "chat/net.hpp"

namespace hb::chat {

inline constexpr const char* kGoogleAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
inline constexpr const char* kGoogleTokenEndpoint = "https://oauth2.googleapis.com/token";
inline constexpr const char* kYouTubeReadonlyScope = "https://www.googleapis.com/auth/youtube.readonly";

std::string urlEncode(std::string_view s);   // RFC 3986 unreserved kept
std::string urlDecode(std::string_view s);   // also '+' -> ' '
std::string base64Url(const std::string& bytes);                 // no padding
std::string pkceChallengeS256(const std::string& verifier);      // base64url(SHA-256(verifier))

using RandomFn = std::function<void(unsigned char*, std::size_t)>;
RandomFn defaultRandom();                                        // std::random_device based
// 43+ char URL-safe random string (PKCE verifier / state).
std::string randomUrlSafe(std::size_t bytes, const RandomFn& rnd);

struct AuthRequest {
    std::string url;       // open this in the system browser
    std::string verifier;  // keep; needed for the token exchange
    std::string state;     // keep; must match the redirect
};
AuthRequest buildGoogleAuthRequest(const std::string& clientId, const std::string& redirectUri, const RandomFn& rnd);

struct RedirectResult {
    bool ok = false;      // a request that carries either code or error was recognised
    std::string code;
    std::string state;
    std::string error;    // e.g. "access_denied"
};
// Parses the first line of the browser's request to the loopback listener ("GET /?code=..&state=.. HTTP/1.1").
RedirectResult parseLoopbackRequest(std::string_view rawRequest);

struct TokenSet {
    std::string accessToken;
    std::string refreshToken;  // may be empty on refresh responses
    long long expiresInSec = 0;
};
// false (with `error` = Google's error code/description, no secrets) if the body is not a token response.
bool parseTokenResponse(std::string_view json, TokenSet& out, std::string& error);

class IAccessTokenProvider {
public:
    virtual ~IAccessTokenProvider() = default;
    // Returns a valid access token (refreshing if it expires within 60 s). On failure `needsAuth`
    // tells whether the operator must authorize again (no/revoked refresh token) vs. a transient error.
    virtual bool accessToken(TimePoint now, std::string& token, std::string& error, bool& needsAuth) = 0;
    virtual void invalidate() = 0;  // the API said 401: drop the cached access token
};

inline constexpr const char* kYouTubeRefreshKey = "youtube.refreshToken";

class GoogleTokenManager : public IAccessTokenProvider {
public:
    GoogleTokenManager(IChatHttp& http, ISecretStore& secrets) : http_(http), secrets_(secrets) {}
    void setClient(std::string clientId, std::string clientSecret);   // secret may be empty
    bool hasRefreshToken() const;
    // Exchanges an authorization code (from the loopback redirect) and stores the refresh token.
    bool exchangeCode(const std::string& code, const std::string& verifier, const std::string& redirectUri,
                      TimePoint now, std::string& error);
    void signOut();  // forgets tokens (best effort; the grant stays in the user's Google account page)

    bool accessToken(TimePoint now, std::string& token, std::string& error, bool& needsAuth) override;
    void invalidate() override;

private:
    bool post(const std::string& body, TokenSet& out, std::string& error, int& status);
    IChatHttp& http_;
    ISecretStore& secrets_;
    mutable std::mutex m_;
    std::string clientId_, clientSecret_;
    std::string access_;
    TimePoint expiresAt_{};
};

}  // namespace hb::chat
