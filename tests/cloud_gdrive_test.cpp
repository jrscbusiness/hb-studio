#include <gtest/gtest.h>

#include "cloud/cloud_util.hpp"
#include "cloud/gdrive_oauth.hpp"
#include "cloud/hash_util.hpp"
#include "cloud/gdrive_provider.hpp"
#include "cloud_test_util.hpp"

using namespace hb::cloud;
using namespace cloudtest;

namespace {
RandomBytesFn fixedRandom() {
    return [](std::size_t n) { return std::string(n, '\x42'); };
}
}  // namespace

TEST(CloudOAuth, AuthUrlHasPkceStateAndLoopbackRedirect) {
    GoogleOAuthFlow f({"my-client.apps.googleusercontent.com", "sec"}, fixedRandom());
    const std::string url = f.authUrl(51234);
    EXPECT_EQ(url.rfind("https://accounts.google.com/o/oauth2/v2/auth?", 0), 0u);
    EXPECT_NE(url.find("client_id=my-client.apps.googleusercontent.com"), std::string::npos);
    EXPECT_NE(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A51234"), std::string::npos);
    EXPECT_NE(url.find("scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file"), std::string::npos);
    EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(url.find("code_challenge=" + base64UrlEncode(sha256Raw(f.verifier()))), std::string::npos);
    EXPECT_NE(url.find("state=" + f.state()), std::string::npos);
    EXPECT_NE(url.find("access_type=offline"), std::string::npos);
    EXPECT_GE(f.verifier().size(), 43u);
    EXPECT_EQ(url.find("client_secret"), std::string::npos);  // the client secret only goes to the token endpoint
}

TEST(CloudOAuth, RedirectParsingValidatesState) {
    GoogleOAuthFlow f({"c", ""}, fixedRandom());
    f.authUrl(1000);
    auto ok = f.parseRedirect("GET /?state=" + f.state() + "&code=4%2F0AbC-d HTTP/1.1");
    ASSERT_TRUE(ok.ok);
    EXPECT_EQ(ok.code, "4/0AbC-d");
    EXPECT_FALSE(f.parseRedirect("GET /?state=WRONG&code=x HTTP/1.1").ok);   // CSRF
    EXPECT_FALSE(f.parseRedirect("GET /?code=x HTTP/1.1").ok);               // no state
    auto denied = f.parseRedirect("GET /?state=" + f.state() + "&error=access_denied HTTP/1.1");
    EXPECT_FALSE(denied.ok);
    EXPECT_NE(denied.error.find("negada"), std::string::npos);
    EXPECT_EQ(f.parseRedirect("GET /favicon.ico HTTP/1.1").error, "Pedido de retorno inesperado");
    EXPECT_FALSE(f.parseRedirect("POST /?state=" + f.state() + "&code=x HTTP/1.1").ok);
    EXPECT_FALSE(f.parseRedirect("garbage").ok);
}

TEST(CloudOAuth, TokenRequestsAndResponses) {
    GoogleOAuthFlow f({"cid", "csec"}, fixedRandom());
    f.authUrl(4242);
    auto req = f.tokenRequest("the code/1");
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.url, "https://oauth2.googleapis.com/token");
    EXPECT_NE(req.body.find("code=the%20code%2F1"), std::string::npos);
    EXPECT_NE(req.body.find("code_verifier=" + f.verifier()), std::string::npos);
    EXPECT_NE(req.body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(req.body.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A4242"), std::string::npos);
    auto ok = parseTokenResponse(resp(200, R"({"access_token":"AT","expires_in":3599,"refresh_token":"RT","token_type":"Bearer"})"));
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value().accessToken, "AT");
    EXPECT_EQ(ok.value().refreshToken, "RT");
    EXPECT_EQ(ok.value().expiresInSec, 3599);
    EXPECT_EQ(parseTokenResponse(resp(400, R"({"error":"invalid_grant"})")).error().kind, ErrorKind::Auth);
    EXPECT_EQ(parseTokenResponse(resp(503, "")).error().kind, ErrorKind::Transient);
    EXPECT_FALSE(parseTokenResponse(resp(200, "not json")).ok());
}

TEST(CloudOAuth, TokenSourceCachesAndRefreshesBeforeExpiry) {
    auto h = std::make_shared<FakeHttp>();
    int calls = 0;
    h->handler = [&](const HttpRequest& r) {
        ++calls;
        EXPECT_NE(r.body.find("grant_type=refresh_token"), std::string::npos);
        return resp(200, R"({"access_token":"AT)" + std::to_string(calls) + R"(","expires_in":3600})");
    };
    double now = 1000;
    GoogleTokenSource ts({"c", "s"}, "RT", h, [&] { return now; });
    EXPECT_EQ(ts.accessToken().value(), "AT1");
    EXPECT_EQ(ts.accessToken().value(), "AT1");
    EXPECT_EQ(calls, 1);
    now += 3600 - 30;  // inside the 60 s safety margin
    EXPECT_EQ(ts.accessToken().value(), "AT2");
    ts.invalidate();
    EXPECT_EQ(ts.accessToken().value(), "AT3");
    GoogleTokenSource none({"c", "s"}, "", h, [&] { return now; });
    EXPECT_EQ(none.accessToken().error().kind, ErrorKind::Auth);
}

namespace {
struct DriveFixture {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<GDriveProvider> prov;
    int tokenCalls = 0;
    DriveFixture(std::function<Result<HttpResponse>(const HttpRequest&)> api, std::string folderId = "FOLDER") {
        http->handler = [this, api](const HttpRequest& r) -> Result<HttpResponse> {
            if (r.url == "https://oauth2.googleapis.com/token") {
                ++tokenCalls;
                return resp(200, R"({"access_token":"AT","expires_in":3600})");
            }
            return api(r);
        };
        auto ts = std::make_shared<GoogleTokenSource>(GoogleOAuthConfig{"c", "s"}, "RT", http, [] { return 0.0; });
        GDriveConfig c;
        c.folderId = std::move(folderId);
        prov = std::make_shared<GDriveProvider>(c, http, ts);
    }
};
}  // namespace

TEST(CloudDrive, FolderLookupAndCreation) {
    int phase = 0;
    DriveFixture fx([&](const HttpRequest& r) -> Result<HttpResponse> {
        EXPECT_EQ(hdr(r, "Authorization"), "Bearer AT");
        if (r.method == "GET") { ++phase; return resp(200, R"({"files":[]})"); }
        ++phase;
        return resp(200, R"({"id":"NEWFOLDER"})");
    }, "");
    FileInfo f{"x", "2026-11-24/program/a.mp4", 10, "video/mp4"};
    auto s = fx.prov->begin(f);  // triggers folder search (empty) + creation, then the upload POST
    EXPECT_GE(phase, 2);
    EXPECT_EQ(fx.prov->folderId(), "NEWFOLDER");
    (void)s;
}

TEST(CloudDrive, ResumableUploadFlow) {
    DriveFixture fx([&](const HttpRequest& r) -> Result<HttpResponse> {
        if (r.method == "POST") {
            EXPECT_EQ(hdr(r, "X-Upload-Content-Length"), "600000");
            EXPECT_NE(r.body.find("\"name\":\"2026-11-24_program_a.mp4\""), std::string::npos);
            EXPECT_NE(r.body.find("\"parents\":[\"FOLDER\"]"), std::string::npos);
            return resp(200, "", {{"Location", "https://www.googleapis.com/upload/drive/v3/files?upload_id=XYZ"}});
        }
        const std::string cr = hdr(r, "Content-Range");
        if (cr == "bytes 0-262143/600000") return resp(308, "", {{"Range", "bytes=0-262143"}});
        if (cr == "bytes 262144-524287/600000") return resp(308, "", {{"Range", "bytes=0-524287"}});
        if (cr == "bytes 524288-599999/600000") return resp(200, R"({"id":"FILEID","md5Checksum":"abc","size":"600000"})");
        ADD_FAILURE() << "unexpected Content-Range " << cr;
        return resp(500);
    });
    FileInfo f{"x", "2026-11-24/program/a.mp4", 600000, "video/mp4"};
    auto s = fx.prov->begin(f);
    ASSERT_TRUE(s.ok()) << s.error().message;
    s.value().chunkSize = 262144;
    const std::string data(600000, 'z');
    auto a1 = fx.prov->putChunk(s.value(), f, 0, std::string_view(data).substr(0, 262144), nullptr);
    ASSERT_TRUE(a1.ok());
    EXPECT_EQ(a1.value().accepted, 262144u);
    EXPECT_FALSE(a1.value().done);
    auto a2 = fx.prov->putChunk(s.value(), f, 262144, std::string_view(data).substr(262144, 262144), nullptr);
    ASSERT_TRUE(a2.ok());
    auto a3 = fx.prov->putChunk(s.value(), f, 524288, std::string_view(data).substr(524288), nullptr);
    ASSERT_TRUE(a3.ok());
    EXPECT_TRUE(a3.value().done);
    EXPECT_EQ(a3.value().remoteId, "FILEID");
    EXPECT_FALSE(fx.prov->finish(s.value(), f, "different-md5").ok());   // MD5 mismatch is caught
    EXPECT_TRUE(fx.prov->finish(s.value(), f, "abc").ok());
    EXPECT_TRUE(fx.prov->verifiesWholeFileMd5());
}

TEST(CloudDrive, PartialAcceptanceAndResumeQuery) {
    DriveFixture fx([&](const HttpRequest& r) -> Result<HttpResponse> {
        const std::string cr = hdr(r, "Content-Range");
        if (cr == "bytes */1000") return resp(308, "", {{"Range", "bytes=0-399"}});
        if (cr == "bytes 0-499/1000") return resp(308, "", {{"Range", "bytes=0-299"}});   // server kept only 300
        return resp(500);
    });
    FileInfo f{"x", "a.mp4", 1000, ""};
    UploadSession s;
    s.uri = "https://www.googleapis.com/upload/drive/v3/files?upload_id=Q";
    s.chunkSize = 500;
    auto c = fx.prov->committed(s, f);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 400u);
    auto a = fx.prov->putChunk(s, f, 0, std::string(500, 'a'), nullptr);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().accepted, 300u);  // the engine continues from 300, not 500
}

TEST(CloudDrive, ErrorMappingAndTokenRefreshOn401) {
    auto run = [&](int status, const std::string& body) {
        DriveFixture fx([=](const HttpRequest&) { return resp(status, body); });
        UploadSession s;
        s.uri = "https://www.googleapis.com/upload/x";
        s.chunkSize = 10;
        FileInfo f{"x", "a", 10, ""};
        return fx.prov->putChunk(s, f, 0, "0123456789", nullptr).error();
    };
    EXPECT_EQ(run(403, R"({"error":{"message":"full","errors":[{"reason":"storageQuotaExceeded"}]}})").kind, ErrorKind::Quota);
    EXPECT_EQ(run(403, R"({"error":{"errors":[{"reason":"userRateLimitExceeded"}]}})").kind, ErrorKind::Transient);
    EXPECT_EQ(run(503, "").kind, ErrorKind::Transient);
    EXPECT_EQ(run(404, "").kind, ErrorKind::SessionLost);
    EXPECT_EQ(run(403, R"({"error":{"message":"API disabled"}})").kind, ErrorKind::Auth);

    int apiCalls = 0;
    DriveFixture fx([&](const HttpRequest&) -> Result<HttpResponse> {
        return ++apiCalls == 1 ? resp(401) : resp(200, R"({"id":"F","md5Checksum":"m"})");
    });
    UploadSession s;
    s.uri = "https://www.googleapis.com/upload/x";
    s.chunkSize = 10;
    FileInfo f{"x", "a", 10, ""};
    auto r = fx.prov->putChunk(s, f, 0, "0123456789", nullptr);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(apiCalls, 2);
    EXPECT_EQ(fx.tokenCalls, 2);  // the token was refreshed once after the 401
}

TEST(CloudDrive, StatByNameInFolder) {
    DriveFixture fx([&](const HttpRequest& r) -> Result<HttpResponse> {
        EXPECT_NE(r.url.find("%27FOLDER%27%20in%20parents"), std::string::npos);
        return resp(200, R"({"files":[{"id":"I","size":"123","md5Checksum":"m"}]})");
    });
    FileInfo f{"x", "d/a.mp4", 123, ""};
    auto r = fx.prov->stat(f);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().exists);
    EXPECT_EQ(r.value().size, 123u);
}

TEST(CloudDrive, ConnectionTestReportsAccountAndFreeSpace) {
    DriveFixture fx([&](const HttpRequest&) -> Result<HttpResponse> {
        return resp(200, R"({"user":{"emailAddress":"a@b.c"},"storageQuota":{"limit":"107374182400","usage":"1073741824"}})");
    });
    auto r = fx.prov->test();
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_NE(r.value().find("a@b.c"), std::string::npos);
    EXPECT_NE(r.value().find("99.0 GiB"), std::string::npos);
}

TEST(CloudUtil, EndpointParsing) {
    EXPECT_TRUE(parseEndpoint("https://s3.amazonaws.com/").valid);
    EXPECT_EQ(parseEndpoint("http://localhost:9000").port, 9000);
    // cleartext only toward the local network
    EXPECT_TRUE(parseEndpoint("http://192.168.1.20:9000").valid);
    EXPECT_TRUE(parseEndpoint("http://10.0.0.5").valid);
    EXPECT_TRUE(parseEndpoint("http://172.16.3.4").valid);
    EXPECT_TRUE(parseEndpoint("http://nas").valid);
    EXPECT_TRUE(parseEndpoint("http://minio.local:9000").valid);
    EXPECT_FALSE(parseEndpoint("http://s3.example.com").valid);
    EXPECT_FALSE(parseEndpoint("http://8.8.8.8").valid);
    EXPECT_FALSE(parseEndpoint("http://172.32.0.1").valid);
    EXPECT_FALSE(parseEndpoint("http://192.169.1.1").valid);
    EXPECT_EQ(parseEndpoint("play.min.io").scheme, "https");
    EXPECT_FALSE(parseEndpoint("https://u:p@host").valid);
    EXPECT_FALSE(parseEndpoint("https://host/path").valid);
    EXPECT_FALSE(parseEndpoint("https://host:99999").valid);
    EXPECT_FALSE(parseEndpoint("").valid);
    EXPECT_EQ(xmlText("<a><B>x &amp; y</B></a>", "B"), "x & y");
}
