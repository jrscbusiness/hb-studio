#include <gtest/gtest.h>

#include "ptz/visca.hpp"

using namespace hb::ptz;
using namespace hb::ptz::visca;

TEST(Visca, SpeedMapping) {
    EXPECT_EQ(panSpeedByte(0.0), 0x01);
    EXPECT_EQ(panSpeedByte(1.0), 0x18);
    EXPECT_EQ(panSpeedByte(-1.0), 0x18);
    EXPECT_EQ(tiltSpeedByte(1.0), 0x14);
    EXPECT_EQ(tiltSpeedByte(0.5), 1 + 10);  // lround(0.5*19)=10
    EXPECT_EQ(panSpeedByte(5.0), 0x18);     // clamped
    EXPECT_EQ(zoomSpeedNibble(1.0), 7);
    EXPECT_EQ(zoomSpeedNibble(0.0), 0);
}

TEST(Visca, PanTiltDrive) {
    EXPECT_EQ(panTiltDrive(1, -1.0, 1.0), (Bytes{0x81, 0x01, 0x06, 0x01, 0x18, 0x14, 0x01, 0x01, 0xFF}));
    EXPECT_EQ(panTiltDrive(1, 1.0, -1.0), (Bytes{0x81, 0x01, 0x06, 0x01, 0x18, 0x14, 0x02, 0x02, 0xFF}));
    EXPECT_EQ(panTiltStop(1), (Bytes{0x81, 0x01, 0x06, 0x01, 0x01, 0x01, 0x03, 0x03, 0xFF}));
    EXPECT_EQ(panTiltDrive(3, 0.0, 1.0)[0], 0x83);
}

TEST(Visca, ZoomPresetFocusIris) {
    EXPECT_EQ(zoomDrive(1, 0), (Bytes{0x81, 0x01, 0x04, 0x07, 0x00, 0xFF}));
    EXPECT_EQ(zoomDrive(1, 1.0), (Bytes{0x81, 0x01, 0x04, 0x07, 0x27, 0xFF}));
    EXPECT_EQ(zoomDrive(1, -1.0), (Bytes{0x81, 0x01, 0x04, 0x07, 0x37, 0xFF}));
    EXPECT_EQ(presetSet(1, 5), (Bytes{0x81, 0x01, 0x04, 0x3F, 0x01, 0x05, 0xFF}));
    EXPECT_EQ(presetRecall(1, 5), (Bytes{0x81, 0x01, 0x04, 0x3F, 0x02, 0x05, 0xFF}));
    EXPECT_EQ(presetReset(1, 5), (Bytes{0x81, 0x01, 0x04, 0x3F, 0x00, 0x05, 0xFF}));
    EXPECT_EQ(focusAuto(1), (Bytes{0x81, 0x01, 0x04, 0x38, 0x02, 0xFF}));
    EXPECT_EQ(focusManual(1), (Bytes{0x81, 0x01, 0x04, 0x38, 0x03, 0xFF}));
    EXPECT_EQ(focusStop(1), (Bytes{0x81, 0x01, 0x04, 0x08, 0x00, 0xFF}));
    EXPECT_EQ(focusFar(1, 0.0), (Bytes{0x81, 0x01, 0x04, 0x08, 0x20, 0xFF}));
    EXPECT_EQ(focusNear(1, 1.0), (Bytes{0x81, 0x01, 0x04, 0x08, 0x37, 0xFF}));
    EXPECT_EQ(irisUp(1), (Bytes{0x81, 0x01, 0x04, 0x0B, 0x02, 0xFF}));
    EXPECT_EQ(irisDown(1), (Bytes{0x81, 0x01, 0x04, 0x0B, 0x03, 0xFF}));
    EXPECT_EQ(inquiryPanTiltPosition(1), (Bytes{0x81, 0x09, 0x06, 0x12, 0xFF}));
    EXPECT_EQ(inquiryZoomPosition(1), (Bytes{0x81, 0x09, 0x04, 0x47, 0xFF}));
}

TEST(Visca, Absolute) {
    auto b = panTiltAbsolute(1, 1.0, 1.0, -2, 0x10);
    EXPECT_EQ(b, (Bytes{0x81, 0x01, 0x06, 0x02, 0x18, 0x14, 0x0F, 0x0F, 0x0F, 0x0E, 0x00, 0x00, 0x01, 0x00, 0xFF}));
    EXPECT_EQ(zoomDirect(1, 0x1234), (Bytes{0x81, 0x01, 0x04, 0x47, 0x01, 0x02, 0x03, 0x04, 0xFF}));
}

TEST(Visca, ParseAckCompletionError) {
    auto ack = parseMessage({0x90, 0x41, 0xFF});
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->kind, Message::Kind::Ack);
    EXPECT_EQ(ack->socket, 1);
    auto done = parseMessage({0x90, 0x52, 0xFF});
    ASSERT_TRUE(done);
    EXPECT_EQ(done->kind, Message::Kind::Completion);
    EXPECT_EQ(done->socket, 2);
    auto err = parseMessage({0x90, 0x60, 0x41, 0xFF});
    ASSERT_TRUE(err);
    EXPECT_EQ(err->kind, Message::Kind::Error);
    EXPECT_EQ(err->errorCode, 0x41);
    EXPECT_STREQ(errorName(0x02), "syntax error");
}

TEST(Visca, ParseRejectsGarbage) {
    EXPECT_FALSE(parseMessage({}));
    EXPECT_FALSE(parseMessage({0xFF}));
    EXPECT_FALSE(parseMessage({0x90, 0x41}));        // no terminator
    EXPECT_FALSE(parseMessage({0x10, 0x41, 0xFF}));  // top bit clear
    auto weird = parseMessage({0x90, 0x41, 0x01, 0xFF});
    ASSERT_TRUE(weird);
    EXPECT_EQ(weird->kind, Message::Kind::Unknown);
}

TEST(Visca, PositionInquiryReply) {
    auto m = parseMessage({0x90, 0x50, 0x0F, 0x0F, 0x0F, 0x0E, 0x00, 0x00, 0x01, 0x00, 0xFF});
    ASSERT_TRUE(m);
    int pan = 0, tilt = 0;
    ASSERT_TRUE(parsePanTiltPosition(*m, pan, tilt));
    EXPECT_EQ(pan, -2);
    EXPECT_EQ(tilt, 0x10);
    auto z = parseMessage({0x90, 0x50, 0x01, 0x02, 0x03, 0x04, 0xFF});
    ASSERT_TRUE(z);
    int zoom = 0;
    ASSERT_TRUE(parseZoomPosition(*z, zoom));
    EXPECT_EQ(zoom, 0x1234);
    EXPECT_FALSE(parseZoomPosition(*m, zoom));  // wrong length
}

TEST(Visca, ExtractFramesHandlesPartialAndMultiple) {
    Bytes buf{0x90, 0x41, 0xFF, 0x90, 0x51};
    auto f = extractFrames(buf);
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(buf, (Bytes{0x90, 0x51}));
    buf.push_back(0xFF);
    f = extractFrames(buf);
    ASSERT_EQ(f.size(), 1u);
    EXPECT_TRUE(buf.empty());
}

TEST(Visca, IpFraming) {
    auto d = wrapIp(IpType::Command, 0x01020304, panTiltStop(1));
    EXPECT_EQ(Bytes(d.begin(), d.begin() + 8), (Bytes{0x01, 0x00, 0x00, 0x09, 0x01, 0x02, 0x03, 0x04}));
    auto f = unwrapIp(d);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->type, 0x0100);
    EXPECT_EQ(f->sequence, 0x01020304u);
    EXPECT_EQ(f->payload, panTiltStop(1));
    EXPECT_EQ(ipSequenceReset(0), (Bytes{0x02, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0x01}));
}

TEST(Visca, IpUnwrapValidatesLength) {
    EXPECT_FALSE(unwrapIp({}));
    EXPECT_FALSE(unwrapIp(Bytes(7, 0)));
    auto d = wrapIp(IpType::Reply, 1, {0x90, 0x41, 0xFF});
    d.pop_back();
    EXPECT_FALSE(unwrapIp(d));  // declared length > actual
    d = wrapIp(IpType::Reply, 1, {0x90, 0x41, 0xFF});
    d.push_back(0);
    EXPECT_FALSE(unwrapIp(d));  // trailing bytes
    Bytes huge{0x01, 0x11, 0xFF, 0xFF, 0, 0, 0, 1};
    EXPECT_FALSE(unwrapIp(huge));
}
