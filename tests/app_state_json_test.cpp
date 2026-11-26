#include <gtest/gtest.h>

#include "app/state_json.hpp"
#include "core/json.hpp"

using namespace hb::app;

namespace {
StateSnapshot sample() {
    StateSnapshot s;
    s.version = "0.1.0+abc1234";
    s.pvw = 1;
    s.pgm = 0;
    s.transitioning = true;
    s.progress = 0.5;
    s.fps = 29.97;
    s.targetFps = 30;
    s.programWidth = 1920;
    s.programHeight = 1080;
    s.frames = 12345678901ULL;
    s.skipped = 2;
    s.tier = "high";
    s.cores = 8;
    s.ramGiB = 16;
    s.sourcesRecommended = 4;
    s.sourcesMax = 16;
    return s;
}
}  // namespace

TEST(StateJson, ExactLayoutWithNoSlotsNoLogNoToast) {
    const std::string j = buildStateJson(sample());
    EXPECT_EQ(j,
              "{\"t\":\"s\",\"pvw\":1,\"pgm\":0,\"transition\":true,\"progress\":0.500,\"fps\":29.97,\"target\":30,"
              "\"pw\":1920,\"ph\":1080,\"ver\":\"0.1.0+abc1234\",\"frames\":12345678901,\"skipped\":2,\"tier\":\"high\","
              "\"cores\":8,\"ramGiB\":16,\"srcRec\":4,\"srcMax\":16,"
              "\"vcam\":{\"supported\":false,\"installed\":false,\"active\":false,\"connected\":false,\"busy\":false,"
              "\"error\":false,\"sent\":0,\"message\":\"\"},\"toast\":null,\"slots\":[],\"log\":[]}");
}

TEST(StateJson, ToastSlotsAndLogAreEmittedAndEscaped) {
    StateSnapshot s = sample();
    s.vcam.supported = s.vcam.active = true;
    s.vcam.sent = 7;
    s.vcam.message = "say \"hi\"";
    s.toastId = 3;
    s.toast = "linha1\nlinha2";
    StateSlot a;  // empty slot (test source)
    StateSlot b;
    b.kind = "dev";
    b.status = "online";
    b.width = 1280;
    b.height = 720;
    b.fps = 29.94;
    b.label = "Cam \"A\"";
    b.deviceId = "dev:1";
    b.message = "ok";
    s.slots = {a, b};
    s.log = {{"12:00:01 [info] [engine] started", "info"}, {"12:00:02 [warn] [x] a\"b", "warn"}};
    const std::string j = buildStateJson(s);
    EXPECT_NE(j.find("\"toast\":{\"id\":3,\"text\":\"linha1\\nlinha2\"}"), std::string::npos);
    EXPECT_NE(j.find("\"slots\":[{\"k\":\"test\",\"st\":\"test\",\"en\":true,\"w\":0,\"h\":0,\"fps\":0.0,\"name\":\"\",\"id\":\"\",\"msg\":\"\"},"
                     "{\"k\":\"dev\",\"st\":\"online\",\"en\":true,\"w\":1280,\"h\":720,\"fps\":29.9,\"name\":\"Cam \\\"A\\\"\","
                     "\"id\":\"dev:1\",\"msg\":\"ok\"}]"),
              std::string::npos);
    EXPECT_NE(j.find("\"log\":[{\"t\":\"12:00:01 [info] [engine] started\",\"l\":\"info\"},"
                     "{\"t\":\"12:00:02 [warn] [x] a\\\"b\",\"l\":\"warn\"}]}"),
              std::string::npos);
    EXPECT_NE(j.find("\"vcam\":{\"supported\":true,\"installed\":false,\"active\":true"), std::string::npos);
    EXPECT_NE(j.find("\"sent\":7,\"message\":\"say \\\"hi\\\"\"}"), std::string::npos);
}

TEST(StateJson, ToastIdZeroMeansNull) {
    StateSnapshot s = sample();
    s.toastId = 0;
    s.toast = "ignored";
    EXPECT_NE(buildStateJson(s).find("\"toast\":null"), std::string::npos);
}

TEST(StateJson, ParsesAsJsonWithManySlots) {
    StateSnapshot s = sample();
    s.slots.resize(16);
    s.log.push_back({"x", "info"});
    hb::core::json::Value v;
    // The UI JSON.parse()s this every tick: it must be well formed.
    ASSERT_TRUE(hb::core::json::parse(buildStateJson(s), v));
    ASSERT_NE(v.get("slots"), nullptr);
    EXPECT_EQ(v.get("slots")->a.size(), 16u);
}
