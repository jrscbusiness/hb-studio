#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "diagnostics/redactor.hpp"
#include "diagnostics/report.hpp"

using namespace hb::diag;

namespace {
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

Redactor makeRedactor() {
    Redactor r;
    r.addSecret("sk-live-ABCDEF1234567890");
    r.addSecret("live_123456_AbCdEfGhIjKl");
    r.addPathAlias("C:\\Users\\Maria Silva\\AppData\\Local", "%LOCALAPPDATA%");
    r.addPathAlias("C:\\Users\\Maria Silva", "%USERPROFILE%");
    return r;
}
}  // namespace

TEST(DiagRedactor, ExactSecretsInAnyEncodingNeverSurvive) {
    const Redactor r = makeRedactor();
    const std::string in = "key=sk-live-ABCDEF1234567890 enc=sk-live-ABCDEF1234567890 json=\"live_123456_AbCdEfGhIjKl\"";
    const std::string out = r.redact(in);
    EXPECT_FALSE(has(out, "ABCDEF1234567890"));
    EXPECT_FALSE(has(out, "AbCdEfGhIjKl"));
    Redactor r2;
    r2.addSecret("pa/ss\"w+rd 1");
    const std::string out2 = r2.redact("a pa/ss\"w+rd 1 b pa%2Fss%22w%2Brd%201 c pa/ss\\\"w+rd 1");
    EXPECT_FALSE(has(out2, "pa/ss"));
    EXPECT_FALSE(has(out2, "pa%2Fss"));
}

TEST(DiagRedactor, KnownCredentialShapes) {
    const Redactor r = makeRedactor();
    const std::string out = r.redact(
        "rtmp://live.example.com/app/streamkey_0123456789 rtsp://admin:hunter2@192.168.1.50:554/s "
        "Authorization: Bearer eyJhbGciOi.payloadpayload.sigsigsig password=topsecret token=abc123def");
    EXPECT_FALSE(has(out, "streamkey_0123456789"));
    EXPECT_FALSE(has(out, "hunter2"));
    EXPECT_FALSE(has(out, "192.168.1.50"));
    EXPECT_FALSE(has(out, "topsecret"));
    EXPECT_FALSE(has(out, "abc123def"));
    EXPECT_FALSE(has(out, "eyJhbGciOi"));
}

TEST(DiagRedactor, UserNamePathsAreReplaced) {
    const Redactor r = makeRedactor();
    const std::string out = r.redact(
        "C:\\Users\\Maria Silva\\AppData\\Local\\HBStudio\\xmit.json and c:/users/maria silva/Videos/x.mp4 "
        "and D:\\Users\\Other\\Docs and /home/joao/file");
    EXPECT_FALSE(has(out, "Maria"));
    EXPECT_FALSE(has(out, "Silva"));
    EXPECT_FALSE(has(out, "Other"));
    EXPECT_FALSE(has(out, "joao"));
    EXPECT_TRUE(has(out, "%LOCALAPPDATA%\\HBStudio\\xmit.json"));
    EXPECT_TRUE(has(out, "%USERPROFILE%"));
}

TEST(DiagRedactor, NetworkIdentifiers) {
    const std::string out = redactNetworkIds(
        "dev 10.0.0.7:8080 and 192.168.100.200. v6 fe80::1ff:fe23:4567:890a mac aa:bb:cc:dd:ee:ff "
        "mail me.name+x@sub.example.org clock 12:34:56 version 10.0.19045.5371 loop 127.0.0.1 ratio 3:4");
    EXPECT_FALSE(has(out, "10.0.0.7"));
    EXPECT_FALSE(has(out, "192.168.100.200"));
    EXPECT_FALSE(has(out, "fe80"));
    EXPECT_FALSE(has(out, "aa:bb"));
    EXPECT_FALSE(has(out, "example.org"));
    EXPECT_TRUE(has(out, "12:34:56"));         // a clock is not an address
    EXPECT_TRUE(has(out, "10.0.19045.5371"));  // an OS version is not an IP
    EXPECT_TRUE(has(out, "127.0.0.1"));
    EXPECT_TRUE(has(out, "3:4"));
}

TEST(DiagRedactor, LongOpaqueTokensGoButWordsStay) {
    const std::string out = redactLongTokens("id 0123456789abcdef0123456789abcdefAAAA and Supercalifragilisticexpialidocious_longword_only");
    EXPECT_FALSE(has(out, "0123456789abcdef"));
    EXPECT_TRUE(has(out, "Supercalifragilisticexpialidocious_longword_only"));
}

TEST(DiagRedactor, HostileInputsAreLinearAndSafe) {
    const Redactor r = makeRedactor();
    EXPECT_NO_THROW(r.redact(std::string(1 << 20, ':')));
    EXPECT_NO_THROW(r.redact(std::string(1 << 20, '@')));
    EXPECT_NO_THROW(r.redact(std::string(1 << 20, 'a')));
    EXPECT_NO_THROW(r.redact(std::string("\0\0\xff\xfe", 4)));
    // a secret beyond redactSecrets' 8 KiB truncation point must still be caught
    std::string big(20000, 'x');
    big += " password=hunter2secret";
    EXPECT_FALSE(has(r.redact(big), "hunter2secret"));
    // short "secrets" must not mangle everything
    Redactor tiny;
    tiny.addSecret("a");
    EXPECT_EQ(tiny.redact("banana"), "banana");
}

TEST(DiagUrl, OnlySchemeAndHostSurvive) {
    EXPECT_EQ(urlSchemeHost("rtmps://user:pw@live.example.com:443/app/KEY?x=1"), "rtmps://live.example.com:443");
    EXPECT_EQ(urlSchemeHost("srt://host:9000?streamid=SECRET"), "srt://host:9000");
    EXPECT_EQ(urlSchemeHost("not a url"), "");
    EXPECT_EQ(urlSchemeHost("rtmp:///only-path"), "");
}

namespace {
DiagnosticsInput hostileInput() {
    DiagnosticsInput in;
    in.appVersion = "0.3.0+abc1234";
    in.windowsVersion = "Windows 10.0 build 19045";
    in.cpuName = "Test CPU @ 3.0GHz";
    in.logicalCores = 8;
    in.ramGiB = 15.9;
    in.gpus = {"Test GPU"};
    in.monitors = {"1920x1080 principal"};
    in.videoDevices = {{"USB Camera", "online", "C:\\Users\\Maria Silva\\cap"}, {"NDI: CAM-IP", "online", "rtsp://admin:hunter2@10.1.2.3/x"}};
    in.audioDevices = {{"Mic", "online", ""}};
    in.features = {{"Chat", "ligado", false}};
    in.settings = {{"apiKey", "sk-live-ABCDEF1234567890", true},
                   {"streamKey", "live_123456_AbCdEfGhIjKl", true},
                   {"folder", "C:\\Users\\Maria Silva\\Videos\\HB Studio", false},
                   {"note", "line1\nline2", false}};
    DestinationLine d;
    d.name = "YouTube";
    d.platform = "youtube";
    d.protocol = "rtmps";
    d.serverUrl = "rtmps://a.rtmp.youtube.com:443/live2/live_123456_AbCdEfGhIjKl?k=zzz";
    d.enabled = d.urlSet = d.keySet = true;
    d.videoKbps = 6000;
    d.audioKbps = 160;
    d.status = "erro ao publicar em rtmps://a.rtmp.youtube.com/live2/live_123456_AbCdEfGhIjKl";
    in.destinations = {d};
    const auto now = std::chrono::system_clock::now();
    in.log.push_back({hb::core::Level::Info, "chat", "login ok token=oauth-token-XYZ987654", now});
    in.log.push_back({hb::core::Level::Error, "stream", "falha: key live_123456_AbCdEfGhIjKl at 203.0.113.9", now});
    in.log.push_back({hb::core::Level::Warning, "cloud", "S3 secret sk-live-ABCDEF1234567890 em C:\\Users\\Maria Silva\\x", now});
    in.previewQuality = "alta";
    in.outputFormat = "1920x1080 @ 30 fps";
    in.extraErrors = {{"CAM 2", "sem sinal em 192.168.0.44", ""}};
    return in;
}
}  // namespace

TEST(DiagReport, SecretsNeverAppearAnywhere) {
    const Redactor r = makeRedactor();
    const std::string rep = buildReport(hostileInput(), r);
    for (const char* leak : {"sk-live-ABCDEF1234567890", "ABCDEF1234567890", "live_123456_AbCdEfGhIjKl", "AbCdEfGhIjKl", "hunter2",
                             "oauth-token-XYZ987654", "Maria", "Silva", "203.0.113.9", "192.168.0.44", "10.1.2.3", "k=zzz"})
        EXPECT_FALSE(has(rep, leak)) << leak << "\n" << rep;
    // useful content survives
    EXPECT_TRUE(has(rep, "0.3.0+abc1234"));
    EXPECT_TRUE(has(rep, "Windows 10.0 build 19045"));
    EXPECT_TRUE(has(rep, "rtmps://a.rtmp.youtube.com:443"));
    EXPECT_TRUE(has(rep, "chave: (definida)"));
    EXPECT_TRUE(has(rep, "- apiKey: "));   // value printed only as (definido)/redacted, never raw
    EXPECT_TRUE(has(rep, "%USERPROFILE%"));
    EXPECT_TRUE(has(rep, "alta"));
    EXPECT_TRUE(has(rep, "1920x1080 @ 30 fps"));
}

TEST(DiagReport, LastErrorsPerModuleAndLogTail) {
    DiagnosticsInput in = hostileInput();
    in.logTailLines = 2;
    const auto errs = lastErrorsPerModule(in.log);
    ASSERT_EQ(errs.size(), 2u);   // chat only logged Info
    EXPECT_EQ(errs[0].module, "cloud");
    EXPECT_EQ(errs[1].module, "stream");
    const std::string rep = buildReport(in, makeRedactor());
    EXPECT_TRUE(has(rep, "Últimos erros por módulo"));
    EXPECT_TRUE(has(rep, "- stream"));
    EXPECT_TRUE(has(rep, "- CAM 2"));
    EXPECT_FALSE(has(rep, "login ok"));   // outside the 2-line tail
}

TEST(DiagReport, EmptyInputStillProducesAReport) {
    const std::string rep = buildReport(DiagnosticsInput{}, Redactor{});
    EXPECT_TRUE(has(rep, "HB Studio - diagnóstico"));
    EXPECT_TRUE(has(rep, "(vazio)"));
}

TEST(DiagReport, WriteTextFile) {
    const std::string path = ::testing::TempDir() + "hb_diag_test.txt";
    std::string err;
    EXPECT_TRUE(writeTextFile(path, "olá\n", err)) << err;
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        EXPECT_EQ(ss.str(), "olá\n");
    }
    std::remove(path.c_str());
    EXPECT_FALSE(writeTextFile("", "x", err));
    EXPECT_FALSE(writeTextFile(::testing::TempDir() + "no_such_dir_hb/x.txt", "x", err));
}
