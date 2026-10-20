#include <gtest/gtest.h>

#include <cmath>

#include <pugixml.hpp>

#include "onvif/crypto.hpp"
#include "onvif/messages.hpp"
#include "onvif/soap.hpp"
#include "onvif/time_util.hpp"

using namespace hb::onvif;

namespace {

pugi::xml_document parseOk(const std::string& xml) {
    pugi::xml_document doc;
    auto r = doc.load_string(xml.c_str());
    EXPECT_TRUE(r) << r.description() << "\n" << xml;
    return doc;
}

SoapBuilder fixedBuilder(std::int64_t now) {
    return SoapBuilder([now] { return now; }, [] { return base64Decode("LKqI6G/AikKCQrN0zqZFlg==").value(); });
}

}  // namespace

TEST(Soap, EnvelopeIsWellFormedSoap12) {
    const auto xml = buildEnvelope("<Foo xmlns=\"urn:x\"/>");
    auto doc = parseOk(xml);
    auto env = doc.child("s:Envelope");
    ASSERT_TRUE(env);
    EXPECT_STREQ(env.attribute("xmlns:s").value(), "http://www.w3.org/2003/05/soap-envelope");
    EXPECT_FALSE(env.child("s:Header"));
    EXPECT_TRUE(env.child("s:Body").child("Foo"));
}

TEST(Soap, SecurityHeaderCarriesDigest) {
    auto b = fixedBuilder(1284623445);  // 2010-09-16T07:50:45Z
    Credentials c{"user", "userpassword"};
    const auto xml = b.build(messages::getDeviceInformation(), &c);
    auto doc = parseOk(xml);
    auto tok = doc.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken");
    ASSERT_TRUE(tok);
    EXPECT_STREQ(tok.child("wsse:Username").text().get(), "user");
    EXPECT_STREQ(tok.child("wsse:Password").text().get(), "tuOSpGlFlIXsozq4HFNeeGeFLEI=");
    EXPECT_NE(std::string(tok.child("wsse:Password").attribute("Type").value()).find("#PasswordDigest"), std::string::npos);
    EXPECT_STREQ(tok.child("wsse:Nonce").text().get(), "LKqI6G/AikKCQrN0zqZFlg==");
    EXPECT_STREQ(tok.child("wsu:Created").text().get(), "2010-09-16T07:50:45Z");
    EXPECT_EQ(xml.find("userpassword"), std::string::npos);  // digest mode never sends the password
}

TEST(Soap, ClockOffsetShiftsCreated) {
    auto b = fixedBuilder(1284623445);
    b.setClockOffsetSeconds(3600);  // camera is one hour ahead
    Credentials c{"user", "userpassword"};
    auto doc = parseOk(b.build(messages::getProfiles(), &c));
    EXPECT_STREQ(doc.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken").child("wsu:Created").text().get(),
                 "2010-09-16T08:50:45Z");
}

TEST(Soap, PasswordTextMode) {
    auto b = fixedBuilder(0);
    b.setPasswordType(PasswordType::Text);
    Credentials c{"u", "p<&>w"};
    const auto xml = b.build(messages::getProfiles(), &c);
    auto doc = parseOk(xml);
    auto pw = doc.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken").child("wsse:Password");
    EXPECT_STREQ(pw.text().get(), "p<&>w");
    EXPECT_NE(std::string(pw.attribute("Type").value()).find("#PasswordText"), std::string::npos);
}

TEST(Soap, GetSystemDateAndTimeIsUnauthenticated) {
    auto b = fixedBuilder(0);
    Credentials c{"u", "p"};
    const auto xml = b.build(messages::getSystemDateAndTime(), &c);
    EXPECT_EQ(xml.find("Security"), std::string::npos);
    EXPECT_EQ(b.build(messages::getProfiles(), nullptr).find("Security"), std::string::npos);
}

TEST(Soap, EscapesUserInput) {
    EXPECT_EQ(xmlEscape("a<b>&\"'"), "a&lt;b&gt;&amp;&quot;&apos;");
    EXPECT_EQ(xmlEscape(std::string("a\x01" "b")), "ab");
    auto b = fixedBuilder(0);
    Credentials c{"<admin>", "x"};
    auto doc = parseOk(b.build(messages::ptzGotoPreset("tok<1>", "p&1"), &c));
    EXPECT_STREQ(doc.child("s:Envelope").child("s:Header").child("wsse:Security").child("wsse:UsernameToken").child("wsse:Username").text().get(),
                 "<admin>");
    EXPECT_STREQ(doc.child("s:Envelope").child("s:Body").child("GotoPreset").child("ProfileToken").text().get(), "tok<1>");
}

TEST(Messages, AllBodiesAreWellFormedWithActions) {
    using namespace messages;
    const Message all[] = {getDeviceInformation(), getSystemDateAndTime(), getCapabilities(), getServices(true), getProfiles(),
                           getStreamUri("p1"), ptzContinuousMove("p1", 0.5, -0.5, 0, 2), ptzStop("p1"), ptzGotoPreset("p1", "1"),
                           ptzSetPreset("p1", "Home"), ptzSetPreset("p1", "Home", "3"), ptzRemovePreset("p1", "1"), ptzGetPresets("p1"),
                           ptzGetStatus("p1"), imagingGetSettings("v1"), imagingMoveFocus("v1", 0.3), imagingStop("v1")};
    for (const auto& m : all) {
        EXPECT_FALSE(m.action.empty());
        auto doc = parseOk(buildEnvelope(m.bodyXml));
        auto op = doc.child("s:Envelope").child("s:Body").first_child();
        ASSERT_TRUE(op) << m.action;
        // Action ends with the operation element name.
        const std::string a = m.action, n = op.name();
        EXPECT_EQ(a.substr(a.size() - n.size()), n);
    }
    EXPECT_EQ(getStreamUri("p1").action, "http://www.onvif.org/ver10/media/wsdl/GetStreamUri");
    EXPECT_EQ(ptzContinuousMove("p", 0, 0, 0).action, "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove");
    EXPECT_EQ(soapContentType("urn:a"), "application/soap+xml; charset=utf-8; action=\"urn:a\"");
}

TEST(Messages, ContinuousMoveValuesAreClampedAndLocaleFree) {
    const auto m = messages::ptzContinuousMove("p1", 5.0, -9.0, 0.25, 1.5);
    EXPECT_NE(m.bodyXml.find("x=\"1.0000\" y=\"-1.0000\""), std::string::npos) << m.bodyXml;
    EXPECT_NE(m.bodyXml.find("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"0.2500\"/>"), std::string::npos);
    EXPECT_NE(m.bodyXml.find("<Timeout>PT1.5S</Timeout>"), std::string::npos);
    const auto nan = messages::ptzContinuousMove("p1", std::nan(""), 0, 0);
    EXPECT_NE(nan.bodyXml.find("x=\"0.0000\""), std::string::npos);
    EXPECT_EQ(nan.bodyXml.find("Timeout"), std::string::npos);
}
