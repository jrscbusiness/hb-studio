#include <gtest/gtest.h>

#include <vector>

#include "captions/cloud_recognizer.hpp"

using namespace hb::captions;

namespace {
struct FakeHttp : IHttpPoster {
    HttpResponse resp;
    std::string url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    HttpResponse post(const std::string& u, const std::vector<std::pair<std::string, std::string>>& h, const std::string& b, int) override {
        url = u; headers = h; body = b;
        return resp;
    }
};
std::string header(const FakeHttp& h, const std::string& k) {
    for (const auto& p : h.headers) if (p.first == k) return p.second;
    return {};
}
}  // namespace

TEST(CaptionsCloud, WavHeaderAndSamples) {
    const float pcm[3] = {0.0f, 1.0f, -2.0f};   // out of range is clamped
    const std::string w = encodeWav16(pcm, 3, 16000);
    ASSERT_EQ(w.size(), 44u + 6u);
    EXPECT_EQ(w.substr(0, 4), "RIFF");
    EXPECT_EQ(w.substr(8, 8), "WAVEfmt ");
    EXPECT_EQ(static_cast<unsigned char>(w[24]), 0x80);   // 16000 = 0x3E80 little endian
    EXPECT_EQ(static_cast<unsigned char>(w[25]), 0x3E);
    EXPECT_EQ(static_cast<unsigned char>(w[40]), 6);      // data size
    const auto s1 = static_cast<std::int16_t>(static_cast<unsigned char>(w[46]) | (static_cast<unsigned char>(w[47]) << 8));
    const auto s2 = static_cast<std::int16_t>(static_cast<unsigned char>(w[48]) | (static_cast<unsigned char>(w[49]) << 8));
    EXPECT_EQ(s1, 32767);
    EXPECT_EQ(s2, -32767);
}

TEST(CaptionsCloud, MultipartLayout) {
    const std::string b = buildMultipart("BND", {{"model", "m"}}, "file", "a.wav", "audio/wav", "DATA");
    EXPECT_EQ(b,
              "--BND\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nm\r\n"
              "--BND\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\nDATA\r\n"
              "--BND--\r\n");
}

TEST(CaptionsCloud, UrlValidationAndJoin) {
    EXPECT_EQ(validateBaseUrl("https://api.openai.com/v1"), "");
    EXPECT_EQ(validateBaseUrl("http://localhost:8080/v1"), "");
    EXPECT_EQ(validateBaseUrl("http://127.0.0.1:9000"), "");
    EXPECT_NE(validateBaseUrl("http://example.com/v1"), "");
    EXPECT_NE(validateBaseUrl("ftp://x"), "");
    EXPECT_NE(validateBaseUrl("https://"), "");
    EXPECT_NE(validateBaseUrl("https://a b"), "");
    EXPECT_EQ(transcriptionsUrl("https://h/v1/"), "https://h/v1/audio/transcriptions");
    EXPECT_EQ(transcriptionsUrl("https://h/v1"), "https://h/v1/audio/transcriptions");
}

TEST(CaptionsCloud, ParseResponse) {
    std::string t;
    EXPECT_TRUE(parseTranscriptionJson("{\"text\":\"ol\\u00e1 mundo\"}", t));
    EXPECT_EQ(t, "olá mundo");
    EXPECT_FALSE(parseTranscriptionJson("{\"error\":1}", t));
    EXPECT_FALSE(parseTranscriptionJson("not json", t));
    EXPECT_FALSE(parseTranscriptionJson("[1]", t));
}

TEST(CaptionsCloud, RecognizeBuildsRequestAndParses) {
    FakeHttp http;
    http.resp.status = 200;
    http.resp.body = "{\"text\":\"boa noite\"}";
    CloudConfig cfg;
    cfg.baseUrl = "https://api.example.com/v1";
    cfg.apiKey = "sk-secret";
    CloudRecognizer r(cfg, &http);
    std::string err;
    ASSERT_TRUE(r.load(err)) << err;
    EXPECT_FALSE(r.supportsPartials());
    std::vector<float> pcm(16000, 0.1f);
    RecognizeRequest req;
    req.pcm = pcm.data();
    req.samples = pcm.size();
    req.language = "pt";
    const auto res = r.recognize(req);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.text, "boa noite");
    EXPECT_EQ(http.url, "https://api.example.com/v1/audio/transcriptions");
    EXPECT_EQ(header(http, "Authorization"), "Bearer sk-secret");
    EXPECT_NE(header(http, "Content-Type").find("multipart/form-data; boundary="), std::string::npos);
    EXPECT_NE(http.body.find("name=\"language\"\r\n\r\npt"), std::string::npos);
    EXPECT_NE(http.body.find("RIFF"), std::string::npos);
    req.language = "auto";   // auto: language field omitted
    r.recognize(req);
    EXPECT_EQ(http.body.find("name=\"language\""), std::string::npos);
}

TEST(CaptionsCloud, ErrorsAreFriendlyAndNeverLeakTheKey) {
    FakeHttp http;
    CloudConfig cfg;
    cfg.baseUrl = "https://api.example.com/v1";
    cfg.apiKey = "sk-secret";
    CloudRecognizer r(cfg, &http);
    std::vector<float> pcm(1600, 0.1f);
    RecognizeRequest req;
    req.pcm = pcm.data();
    req.samples = pcm.size();
    for (int status : {401, 404, 500}) {
        http.resp.status = status;
        const auto res = r.recognize(req);
        EXPECT_FALSE(res.ok);
        EXPECT_EQ(res.error.find("sk-secret"), std::string::npos);
        EXPECT_NE(res.error.find(std::to_string(status)), std::string::npos);
    }
    http.resp.status = 0;
    http.resp.error = "timeout";
    EXPECT_NE(r.recognize(req).error.find("timeout"), std::string::npos);
    http.resp = {};
    http.resp.status = 200;
    http.resp.body = "<html>";
    EXPECT_FALSE(r.recognize(req).ok);
}

TEST(CaptionsCloud, LoadValidatesConfig) {
    FakeHttp http;
    std::string err;
    CloudConfig cfg;
    cfg.baseUrl = "http://evil.example.com";
    EXPECT_FALSE(CloudRecognizer(cfg, &http).load(err));
    cfg.baseUrl = "https://api.example.com/v1";
    EXPECT_FALSE(CloudRecognizer(cfg, &http).load(err));   // https needs a key
    EXPECT_EQ(err, "informe a chave da API");
    cfg.baseUrl = "http://localhost:8000/v1";
    EXPECT_TRUE(CloudRecognizer(cfg, &http).load(err));    // local server: key optional
    EXPECT_FALSE(CloudRecognizer(cfg, nullptr).load(err));
}
