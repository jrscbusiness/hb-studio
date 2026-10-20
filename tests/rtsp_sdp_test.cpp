#include <gtest/gtest.h>

#include "rtsp/base64.hpp"
#include "rtsp/sdp.hpp"
#include "rtsp_test_support.hpp"

using namespace hb::rtsp;
using namespace hb::rtsp::test;

TEST(Sdp, TypicalCameraH264) {
    auto sdp = parseSdp(defaultSdp264());
    ASSERT_TRUE(sdp);
    EXPECT_EQ(sdp->control, "*");
    ASSERT_EQ(sdp->media.size(), 2u);
    const auto& v = sdp->media[0];
    EXPECT_EQ(v.type, "video");
    EXPECT_EQ(v.proto, "RTP/AVP");
    EXPECT_EQ(v.control, "trackID=1");
    ASSERT_EQ(v.payloadTypes.size(), 1u);
    EXPECT_EQ(v.rtpmap.at(96).encoding, "H264");
    EXPECT_EQ(v.rtpmap.at(96).clockRate, 90000);
    EXPECT_DOUBLE_EQ(v.framerate, 25.0);
    EXPECT_EQ(v.width, 1920);
    EXPECT_EQ(v.height, 1080);

    auto sel = selectVideo(*sdp);
    ASSERT_TRUE(sel);
    EXPECT_EQ(sel->codec, Codec::H264);
    EXPECT_EQ(sel->payloadType, 96);
    ASSERT_EQ(sel->fmtp.paramSets.size(), 2u);
    EXPECT_EQ(sel->fmtp.paramSets[0], sps264());
    EXPECT_EQ(sel->fmtp.paramSets[1], pps264());
    EXPECT_EQ(sel->fmtp.packetizationMode, 1);
    EXPECT_EQ(sel->fmtp.profileLevelId, "42C01F");
}

TEST(Sdp, H265WithSeparateSprops) {
    std::string sdp =
        "v=0\ns=x\nm=video 0 RTP/AVP 98\na=rtpmap:98 H265/90000\n"
        "a=fmtp:98 sprop-vps=" + base64Encode(vps265()) + "; sprop-sps=" + base64Encode(sps265()) +
        "; sprop-pps=" + base64Encode(pps265()) + "\na=control:stream=0\na=cliprect:0,0,1080,1920\n";
    auto p = parseSdp(sdp);  // LF-only line endings
    ASSERT_TRUE(p);
    auto sel = selectVideo(*p);
    ASSERT_TRUE(sel);
    EXPECT_EQ(sel->codec, Codec::H265);
    ASSERT_EQ(sel->fmtp.paramSets.size(), 3u);
    EXPECT_EQ(sel->fmtp.paramSets[0], vps265());  // VPS, SPS, PPS order
    EXPECT_EQ(sel->fmtp.paramSets[1], sps265());
    EXPECT_EQ(sel->fmtp.paramSets[2], pps265());
    EXPECT_EQ(sel->media->width, 1920);
    EXPECT_EQ(sel->media->height, 1080);
}

TEST(Sdp, FramesizeAttributeAndHevcAlias) {
    auto p = parseSdp("v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 HEVC/90000\r\na=framesize:96 1280-720\r\na=x-framerate:29.97\r\n");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->media[0].width, 1280);
    EXPECT_EQ(p->media[0].height, 720);
    EXPECT_NEAR(p->media[0].framerate, 29.97, 1e-9);
    EXPECT_EQ(selectVideo(*p)->codec, Codec::H265);
}

TEST(Sdp, MultiplePrimaryParameterSetsAndGarbageEntries) {
    std::string sdp = "v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                      "a=fmtp:96 sprop-parameter-sets=" + base64Encode(sps264()) + ",!!!notbase64," + base64Encode(pps264()) +
                      "," + base64Encode(pps264()) + "\r\n";
    auto sel = selectVideo(*parseSdp(sdp));
    ASSERT_TRUE(sel);
    EXPECT_EQ(sel->fmtp.paramSets.size(), 3u);  // invalid entry skipped
}

TEST(Sdp, SkipsNonVideoAndUnsupportedCodecs) {
    auto p = parseSdp("v=0\r\nm=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n"
                      "m=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\n");
    ASSERT_TRUE(p);
    EXPECT_FALSE(selectVideo(*p));
}

TEST(Sdp, NoVideoRtpmapMeansNoSelection) {
    auto p = parseSdp("v=0\r\nm=video 0 RTP/AVP 96\r\n");  // dynamic PT without rtpmap
    ASSERT_TRUE(p);
    EXPECT_FALSE(selectVideo(*p));
}

TEST(Sdp, RejectsNonSdpAndOversize) {
    EXPECT_FALSE(parseSdp("<html>not sdp</html>"));
    EXPECT_FALSE(parseSdp(""));
    EXPECT_FALSE(parseSdp(std::string(300 * 1024, 'v')));
}

TEST(Sdp, MalformedLinesAreIgnored) {
    auto p = parseSdp("v=0\r\nm=video\r\nm=video 0 RTP/AVP 96 abc 999\r\na=rtpmap:xx H264\r\na=rtpmap:96 H264/90000\r\n"
                      "a=fmtp:96\r\na=x-dimensions:abc\r\na=framerate:-3\r\n");
    ASSERT_TRUE(p);
    ASSERT_EQ(p->media.size(), 1u);  // the bare "m=video" line is dropped
    EXPECT_EQ(p->media[0].payloadTypes, std::vector<int>({96}));
    EXPECT_EQ(p->media[0].width, 0);
    EXPECT_EQ(p->media[0].framerate, 0);
}

TEST(Sdp, ManyMediaSectionsBounded) {
    std::string s = "v=0\r\n";
    for (int i = 0; i < 200; ++i) s += "m=video 0 RTP/AVP 96\r\n";
    auto p = parseSdp(s);
    ASSERT_TRUE(p);
    EXPECT_LE(p->media.size(), 32u);
}

TEST(Sdp, SessionLevelControlAndDonDiffDetection) {
    auto f = parseH265Fmtp("sprop-max-don-diff=2; sprop-sps=AAAA");
    EXPECT_EQ(f.spropMaxDonDiff, 2);
}
