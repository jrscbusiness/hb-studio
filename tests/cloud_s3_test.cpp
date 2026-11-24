#include <gtest/gtest.h>

#include "cloud/hash_util.hpp"
#include "cloud/s3_provider.hpp"
#include "cloud_test_util.hpp"

using namespace hb::cloud;
using namespace cloudtest;

namespace {
S3Config cfg() {
    S3Config c;
    c.endpoint = "https://s3.us-east-005.backblazeb2.com";
    c.region = "us-east-005";
    c.bucket = "meu-bucket";
    c.prefix = "/igreja//cultos/";
    c.accessKey = "AKIDEXAMPLE";
    c.secretKey = "SECRET-KEY-NEVER-LOGGED";
    c.partSize = 5 * 1024 * 1024;
    return c;
}
std::shared_ptr<S3Provider> make(std::shared_ptr<FakeHttp> h, S3Config c = cfg()) {
    return std::make_shared<S3Provider>(c, h, [] { return std::chrono::system_clock::time_point(std::chrono::seconds(1440938160)); });
}
std::string etagOf(std::string_view d) { return "\"" + md5Hex(d) + "\""; }
}  // namespace

TEST(CloudS3, ConfigValidation) {
    EXPECT_TRUE(validateS3Config(cfg()).empty());
    auto c = cfg();
    c.endpoint = "ftp://x";
    EXPECT_FALSE(validateS3Config(c).empty());
    c = cfg(); c.bucket = "ab"; EXPECT_FALSE(validateS3Config(c).empty());
    c = cfg(); c.secretKey.clear(); EXPECT_FALSE(validateS3Config(c).empty());
    c = cfg(); c.endpoint = "https://host/with/path"; EXPECT_FALSE(validateS3Config(c).empty());
    // the error text never echoes credentials
    c = cfg(); c.bucket = "x"; EXPECT_EQ(validateS3Config(c).find("SECRET"), std::string::npos);
}

TEST(CloudS3, ObjectKeyAndAddressing) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200); };
    auto p = make(h);
    EXPECT_EQ(p->objectKey("2026-11-24/program/a b.mp4"), "igreja/cultos/2026-11-24/program/a b.mp4");
    EXPECT_EQ(p->objectKey("../../x/./y"), "igreja/cultos/x/y");   // no traversal out of the prefix
    FileInfo f{"x", "dir/a b.mp4", 10, "video/mp4"};
    p->stat(f);
    EXPECT_EQ(h->reqs[0].url, "https://s3.us-east-005.backblazeb2.com/meu-bucket/igreja/cultos/dir/a%20b.mp4");  // path style by default
    auto c = cfg();
    c.endpoint = "https://s3.us-east-1.amazonaws.com";
    auto h2 = std::make_shared<FakeHttp>();
    h2->handler = [](const HttpRequest&) { return resp(200); };
    make(h2, c)->stat(f);
    EXPECT_EQ(h2->reqs[0].url, "https://meu-bucket.s3.us-east-1.amazonaws.com/igreja/cultos/dir/a%20b.mp4");  // AWS auto -> virtual host
}

TEST(CloudS3, RequestsAreSignedAndPayloadHashed) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200, "", {{"ETag", "\"" + md5Hex("hello") + "\""}}); };
    auto p = make(h);
    FileInfo f{"x", "a.mp4", 5, "video/mp4"};
    UploadSession s;
    s.chunkSize = 5;
    auto r = p->putChunk(s, f, 0, "hello", nullptr);
    ASSERT_TRUE(r.ok());
    const auto& q = h->reqs[0];
    EXPECT_EQ(q.method, "PUT");
    EXPECT_EQ(hdr(q, "x-amz-content-sha256"), sha256Hex("hello"));
    EXPECT_EQ(hdr(q, "x-amz-date"), "20150830T123600Z");
    const std::string auth = hdr(q, "Authorization");
    EXPECT_EQ(auth.rfind("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-005/s3/aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=", 0), 0u);
    EXPECT_EQ(auth.find("SECRET"), std::string::npos);  // the secret key never goes on the wire
    EXPECT_EQ(q.body, "hello");
}

TEST(CloudS3, SmallFileIsSinglePut) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest& r) { return resp(200, "", {{"ETag", etagOf(r.body)}}); };
    auto p = make(h);
    FileInfo f{"x", "a.wav", 11, "audio/wav"};
    auto s = p->begin(f);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s.value().id.empty());
    EXPECT_TRUE(h->reqs.empty());  // no network call for begin()
    auto a = p->putChunk(s.value(), f, 0, "hello world", nullptr);
    ASSERT_TRUE(a.ok());
    EXPECT_TRUE(a.value().done);
    auto fin = p->finish(s.value(), f, "");
    ASSERT_TRUE(fin.ok());
    EXPECT_EQ(h->reqs.size(), 1u);
}

TEST(CloudS3, MultipartFlow) {
    auto h = std::make_shared<FakeHttp>();
    std::string completeBody;
    h->handler = [&](const HttpRequest& r) -> Result<HttpResponse> {
        if (r.method == "POST" && r.url.find("?uploads") != std::string::npos)
            return resp(200, "<InitiateMultipartUploadResult><UploadId>UP&amp;1</UploadId></InitiateMultipartUploadResult>");
        if (r.method == "PUT") return resp(200, "", {{"ETag", etagOf(r.body)}});
        if (r.method == "POST") {
            completeBody = r.body;
            return resp(200, "<CompleteMultipartUploadResult><ETag>\"abc-2\"</ETag></CompleteMultipartUploadResult>");
        }
        if (r.method == "HEAD") return resp(200, "", {{"Content-Length", "11534336"}});
        return resp(500);
    };
    auto p = make(h);
    const std::uint64_t size = 11 * 1024 * 1024 / 2 * 2;  // 11 MiB
    FileInfo f{"x", "big.mp4", size, "video/mp4"};
    auto s = p->begin(f);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().id, "UP&1");
    EXPECT_EQ(s.value().chunkSize, 5u * 1024 * 1024);
    const std::string a(5 * 1024 * 1024, 'a'), b(5 * 1024 * 1024, 'b'), c(size - 10 * 1024 * 1024, 'c');
    ASSERT_TRUE(p->putChunk(s.value(), f, 0, a, nullptr).ok());
    ASSERT_TRUE(p->putChunk(s.value(), f, a.size(), b, nullptr).ok());
    auto last = p->putChunk(s.value(), f, a.size() + b.size(), c, nullptr);
    ASSERT_TRUE(last.ok());
    EXPECT_EQ(last.value().index, 3u);
    EXPECT_NE(h->reqs[1].url.find("partNumber=1&uploadId=UP%261"), std::string::npos);  // query sorted + encoded
    auto fin = p->finish(s.value(), f, "");
    ASSERT_TRUE(fin.ok()) << fin.error().message;
    EXPECT_NE(completeBody.find("<PartNumber>3</PartNumber>"), std::string::npos);
    EXPECT_NE(completeBody.find(md5Hex(b)), std::string::npos);
    EXPECT_FALSE(fin.value().note.empty());  // the fake "abc-2" final ETag does not match: soft warning, not failure
}

TEST(CloudS3, CorruptedPartEtagIsRetriedNotAccepted) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200, "", {{"ETag", "\"00000000000000000000000000000000\""}}); };
    auto p = make(h);
    FileInfo f{"x", "a.mp4", 5, "video/mp4"};
    UploadSession s;
    s.chunkSize = 5;
    auto r = p->putChunk(s, f, 0, "hello", nullptr);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, ErrorKind::Transient);
}

TEST(CloudS3, KmsEtagsAreNotComparedToMd5) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200, "", {{"ETag", "\"11111111111111111111111111111111\""}, {"x-amz-server-side-encryption", "aws:kms"}}); };
    auto p = make(h);
    FileInfo f{"x", "a.mp4", 5, "video/mp4"};
    UploadSession s;
    s.chunkSize = 5;
    EXPECT_TRUE(p->putChunk(s, f, 0, "hello", nullptr).ok());
}

TEST(CloudS3, ErrorMapping) {
    auto run = [&](int status, const std::string& body) {
        auto h = std::make_shared<FakeHttp>();
        h->handler = [=](const HttpRequest&) { return resp(status, body); };
        FileInfo f{"x", "a.mp4", 5, "video/mp4"};
        UploadSession s;
        s.chunkSize = 5;
        return make(h)->putChunk(s, f, 0, "hello", nullptr).error();
    };
    auto xml = [](const char* code) { return std::string("<Error><Code>") + code + "</Code><Message>m</Message></Error>"; };
    EXPECT_EQ(run(403, xml("SignatureDoesNotMatch")).kind, ErrorKind::Auth);
    EXPECT_EQ(run(403, xml("InvalidAccessKeyId")).kind, ErrorKind::Auth);
    EXPECT_EQ(run(403, xml("RequestTimeTooSkewed")).kind, ErrorKind::Auth);
    EXPECT_EQ(run(404, xml("NoSuchBucket")).kind, ErrorKind::Permanent);
    EXPECT_EQ(run(404, xml("NoSuchUpload")).kind, ErrorKind::SessionLost);
    EXPECT_EQ(run(503, xml("SlowDown")).kind, ErrorKind::Transient);
    EXPECT_EQ(run(500, "").kind, ErrorKind::Transient);
    EXPECT_EQ(run(400, xml("EntityTooLarge")).kind, ErrorKind::Permanent);
    EXPECT_EQ(run(403, xml("AccountProblem")).kind, ErrorKind::Auth);
    EXPECT_EQ(run(403, xml("StorageQuotaExceeded")).kind, ErrorKind::Quota);
    EXPECT_EQ(run(507, "").kind, ErrorKind::Quota);
    EXPECT_EQ(run(403, xml("storage_cap_exceeded")).kind, ErrorKind::Quota);
    EXPECT_EQ(run(301, xml("PermanentRedirect")).kind, ErrorKind::Permanent);
}

TEST(CloudS3, CompleteWith200ButErrorBodyIsAnError) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200, "<Error><Code>InternalError</Code><Message>x</Message></Error>"); };
    auto p = make(h);
    FileInfo f{"x", "a.mp4", 10, "video/mp4"};
    UploadSession s;
    s.id = "u";
    s.chunkSize = 5;
    ChunkAck a1, a2;
    a1.index = 1; a1.size = 5; a1.etag = "e1";
    a2.index = 2; a2.size = 5; a2.etag = "e2";
    s.acks = {a1, a2};
    auto r = p->finish(s, f, "");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, ErrorKind::Transient);
}

TEST(CloudS3, FinishDetectsMissingPartsAndSizeMismatch) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200); };
    auto p = make(h);
    FileInfo f{"x", "a.mp4", 10, "video/mp4"};
    UploadSession s;
    s.id = "u";
    s.chunkSize = 5;
    ChunkAck a2;
    a2.index = 2; a2.size = 5;
    s.acks = {a2};
    auto r = p->finish(s, f, "");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, ErrorKind::SessionLost);
    EXPECT_TRUE(h->reqs.empty());
}

TEST(CloudS3, HugeFilesScaleThePartSize) {
    auto h = std::make_shared<FakeHttp>();
    auto p = make(h);
    const std::uint64_t tib = 1024ull * 1024 * 1024 * 1024;
    const std::uint64_t chunk = p->chunkSizeFor(2 * tib);
    EXPECT_LE((2 * tib + chunk - 1) / chunk, kS3MaxParts);
    EXPECT_EQ(chunk % (1024 * 1024), 0u);
    EXPECT_EQ(p->chunkSizeFor(100), 5u * 1024 * 1024);
    FileInfo f{"x", "a", 6 * tib, ""};
    EXPECT_EQ(p->begin(f).error().kind, ErrorKind::Permanent);  // beyond 5 TiB
}

TEST(CloudS3, CommittedListsPartsAndKeepsOnlyTheContiguousRun) {
    auto h = std::make_shared<FakeHttp>();
    const std::uint64_t cs = 5 * 1024 * 1024;
    h->handler = [&](const HttpRequest& r) -> Result<HttpResponse> {
        EXPECT_EQ(r.method, "GET");
        const std::string m1 = md5Hex("a"), m2 = md5Hex("b"), m4 = md5Hex("d");
        const std::string sz = std::to_string(cs);
        return resp(200, "<ListPartsResult><IsTruncated>false</IsTruncated><Part><PartNumber>1</PartNumber><ETag>\"" + m1 + "\"</ETag><Size>" + sz +
                             "</Size></Part><Part><PartNumber>2</PartNumber><ETag>\"" + m2 + "\"</ETag><Size>" + sz +
                             "</Size></Part><Part><PartNumber>4</PartNumber><ETag>\"" + m4 + "\"</ETag><Size>" + sz + "</Size></Part></ListPartsResult>");
    };
    auto p = make(h);
    FileInfo f{"x", "big.mp4", 4 * cs, ""};
    UploadSession s;
    s.id = "u";
    s.chunkSize = cs;
    auto c = p->committed(s, f);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 2 * cs);  // part 4 is orphaned (part 3 is missing): re-uploaded
    EXPECT_EQ(s.acks.size(), 2u);
}

TEST(CloudS3, StatTreats403And404AsAbsent) {
    for (int st : {403, 404}) {
        auto h = std::make_shared<FakeHttp>();
        h->handler = [=](const HttpRequest&) { return resp(st); };
        FileInfo f{"x", "a.mp4", 5, ""};
        auto r = make(h)->stat(f);
        ASSERT_TRUE(r.ok());
        EXPECT_FALSE(r.value().exists);
    }
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200, "", {{"Content-Length", "42"}, {"ETag", "\"x\""}}); };
    FileInfo f{"x", "a.mp4", 42, ""};
    auto r = make(h)->stat(f);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().exists);
    EXPECT_EQ(r.value().size, 42u);
}

TEST(CloudS3, ConnectionTestWritesThenDeletes) {
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest& r) -> Result<HttpResponse> {
        if (r.method == "PUT") return resp(200, "", {{"ETag", etagOf(r.body)}});
        return resp(204);
    };
    auto r = make(h)->test();
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().find("Conexão OK"), std::string::npos);
    ASSERT_EQ(h->reqs.size(), 2u);
    EXPECT_EQ(h->reqs[1].method, "DELETE");
    // no delete permission: still OK, with an honest note
    h->handler = [](const HttpRequest& r) -> Result<HttpResponse> {
        if (r.method == "PUT") return resp(200, "", {{"ETag", etagOf(r.body)}});
        return resp(403, "<Error><Code>AccessDenied</Code></Error>");
    };
    auto r2 = make(h)->test();
    ASSERT_TRUE(r2.ok());
    EXPECT_NE(r2.value().find("exclusão"), std::string::npos);
    // bad credentials
    h->handler = [](const HttpRequest&) { return resp(403, "<Error><Code>SignatureDoesNotMatch</Code></Error>"); };
    auto r3 = make(h)->test();
    ASSERT_FALSE(r3.ok());
    EXPECT_EQ(r3.error().kind, ErrorKind::Auth);
}

TEST(CloudS3, TransferGateAbortBecomesCancelled) {
    struct Deny : ITransferGate { bool acquire(std::size_t) override { return false; } } deny;
    auto h = std::make_shared<FakeHttp>();
    h->handler = [](const HttpRequest&) { return resp(200); };
    FileInfo f{"x", "a.mp4", 5, ""};
    UploadSession s;
    s.chunkSize = 5;
    auto r = make(h)->putChunk(s, f, 0, "hello", &deny);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
}
