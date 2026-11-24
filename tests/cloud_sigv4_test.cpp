#include <gtest/gtest.h>

#include "cloud/hash_util.hpp"
#include "cloud/sigv4.hpp"

using namespace hb::cloud;

TEST(CloudHash, Md5Rfc1321Vectors) {
    EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5Hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"), "57edf4a22be3c955ac49da2e2107b67a");
    Md5 m;  // incremental == one-shot, across block boundaries
    const std::string big(1000, 'x');
    for (std::size_t i = 0; i < big.size(); i += 7) m.update(big.data() + i, std::min<std::size_t>(7, big.size() - i));
    EXPECT_EQ(m.finishHex(), md5Hex(big));
}

TEST(CloudHash, HmacSha256Rfc4231) {
    EXPECT_EQ(hexEncode(hmacSha256("Jefe", "what do ya want for nothing?")), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // key longer than the block size is hashed first (RFC 4231 case 6)
    EXPECT_EQ(hexEncode(hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(CloudHash, Base64AndPkce) {
    EXPECT_EQ(base64Encode("foobar"), "Zm9vYmFy");
    EXPECT_EQ(base64Encode("fo"), "Zm8=");
    EXPECT_EQ(base64Decode("Zm9vYmE="), "fooba");
    EXPECT_EQ(base64UrlEncode(std::string("\xfb\xff", 2)), "-_8");
    // RFC 7636 appendix B
    EXPECT_EQ(base64UrlEncode(sha256Raw("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(CloudHash, UriEncode) {
    EXPECT_EQ(uriEncode("a b/c$d~", false), "a%20b/c%24d~");
    EXPECT_EQ(uriEncode("a/b", true), "a%2Fb");
    EXPECT_EQ(uriEncode("\xE1\x88\xB4", true), "%E1%88%B4");
}

TEST(CloudSigV4, AmzDateFormat) {
    EXPECT_EQ(formatAmzDate(std::chrono::system_clock::time_point(std::chrono::seconds(1440938160))), "20150830T123600Z");
    EXPECT_EQ(formatAmzDate(std::chrono::system_clock::time_point(std::chrono::seconds(0))), "19700101T000000Z");
}

namespace {
SigV4Input suite(const std::string& method, const std::string& path, std::vector<KV> query = {}, std::vector<KV> extra = {},
                 const std::string& body = "") {
    SigV4Input in;
    in.method = method;
    in.canonicalPath = path;
    in.query = std::move(query);
    in.headers = {{"Host", "example.amazonaws.com"}, {"X-Amz-Date", "20150830T123600Z"}};
    for (auto& e : extra) in.headers.push_back(e);
    in.payloadHash = sha256Hex(body);
    in.accessKey = "AKIDEXAMPLE";
    in.secretKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    in.region = "us-east-1";
    in.service = "service";
    in.amzDate = "20150830T123600Z";
    return in;
}
}  // namespace

// Vectors below are the official AWS SigV4 test suite files (.creq / .sts / .authz).
TEST(CloudSigV4, SuiteGetVanilla) {
    const auto o = signV4(suite("GET", "/"));
    EXPECT_EQ(o.canonicalRequest,
              "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(o.stringToSign,
              "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\nbb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63");
    EXPECT_EQ(o.authorization,
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date, "
              "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

TEST(CloudSigV4, SuitePostVanilla) {
    const auto o = signV4(suite("POST", "/"));
    EXPECT_EQ(o.stringToSign.substr(o.stringToSign.rfind('\n') + 1), "553f88c9e4d10fc9e109e2aeb65f030801b70c2f6468faca261d401ae622fc87");
    EXPECT_EQ(o.signature, "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b");
}

TEST(CloudSigV4, SuiteQueryOrderKeyCase) {
    const auto o = signV4(suite("GET", "/", {{"Param2", "value2"}, {"Param1", "value1"}}));
    EXPECT_EQ(o.canonicalRequest.substr(0, 31), "GET\n/\nParam1=value1&Param2=value2");
    EXPECT_EQ(o.stringToSign.substr(o.stringToSign.rfind('\n') + 1), "816cd5b414d056048ba4f7c5386d6e0533120fb1fcfa93762cf0fc39e2cf19e0");
    EXPECT_EQ(o.signature, "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500");
}

TEST(CloudSigV4, SuiteUtf8Path) {
    const auto o = signV4(suite("GET", uriEncode("/\xE1\x88\xB4", false)));
    EXPECT_EQ(o.canonicalRequest.substr(0, 20), "GET\n/%E1%88%B4\n\nhost");
    EXPECT_EQ(o.signature, "8318018e0b0f223aa2bbf98705b62bb787dc9c0e678f255a891fd03141be5d85");
}

TEST(CloudSigV4, SuiteHeaderValueTrim) {
    const auto o = signV4(suite("GET", "/", {}, {{"My-Header1", "  value1"}, {"My-Header2", "\"a   b   c\""}}));
    EXPECT_NE(o.canonicalRequest.find("my-header2:\"a b c\"\n"), std::string::npos);
    EXPECT_EQ(o.signedHeaders, "host;my-header1;my-header2;x-amz-date");
    EXPECT_EQ(o.signature, "acc3ed3afb60bb290fc8d2dd0098b9911fcaa05412b367055dee359757a9c736");
}

TEST(CloudSigV4, SuitePostFormUrlencoded) {
    const auto o = signV4(suite("POST", "/", {}, {{"Content-Type", "application/x-www-form-urlencoded"}}, "Param1=value1"));
    EXPECT_EQ(o.signedHeaders, "content-type;host;x-amz-date");
    EXPECT_EQ(o.signature, "ff11897932ad3f4e8b18135d722051e5ac45fc38421b1da7b9d196a0fe09473a");
}

// Example from the S3 documentation ("Signature Calculations for the Authorization Header").
TEST(CloudSigV4, S3DocsGetObjectWithRange) {
    SigV4Input in;
    in.method = "GET";
    in.canonicalPath = "/test.txt";
    in.headers = {{"host", "examplebucket.s3.amazonaws.com"},
                  {"range", "bytes=0-9"},
                  {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
                  {"x-amz-date", "20130524T000000Z"}};
    in.payloadHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    in.accessKey = "AKIAIOSFODNN7EXAMPLE";
    in.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    in.region = "us-east-1";
    in.service = "s3";
    in.amzDate = "20130524T000000Z";
    EXPECT_EQ(signV4(in).signature, "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST(CloudSigV4, S3DocsPutObject) {
    SigV4Input in;
    in.method = "PUT";
    in.canonicalPath = uriEncode("/test$file.text", false);
    in.headers = {{"date", "Fri, 24 May 2013 00:00:00 GMT"},
                  {"host", "examplebucket.s3.amazonaws.com"},
                  {"x-amz-content-sha256", "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"},
                  {"x-amz-date", "20130524T000000Z"},
                  {"x-amz-storage-class", "REDUCED_REDUNDANCY"}};
    in.payloadHash = sha256Hex("Welcome to Amazon S3.");
    EXPECT_EQ(in.payloadHash, "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
    in.accessKey = "AKIAIOSFODNN7EXAMPLE";
    in.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    in.region = "us-east-1";
    in.service = "s3";
    in.amzDate = "20130524T000000Z";
    EXPECT_EQ(signV4(in).signature, "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}
