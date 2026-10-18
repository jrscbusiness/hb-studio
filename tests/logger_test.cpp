#include <gtest/gtest.h>

#include <sstream>

#include "core/logger.hpp"

using namespace hb::core;

namespace {
struct ThrowingSink : LogSink {
    void write(const LogRecord&) override { throw std::runtime_error("disk full"); }
};
}

TEST(Logger, RedactsBeforeAnySink) {
    Logger log;
    auto mem = std::make_shared<MemorySink>();
    log.addSink(mem);
    log.info("rtsp", "url rtsp://admin:pw123@10.0.0.9/s password=zzz");
    std::ostringstream out;
    mem->exportTo(out);
    EXPECT_EQ(out.str().find("pw123"), std::string::npos);
    EXPECT_EQ(out.str().find("zzz"), std::string::npos);
    EXPECT_NE(out.str().find("[INFO] [rtsp]"), std::string::npos);
}

TEST(Logger, MinLevelFilters) {
    Logger log;
    auto mem = std::make_shared<MemorySink>();
    log.addSink(mem);
    log.setMinLevel(Level::Warning);
    log.debug("m", "d");
    log.info("m", "i");
    log.warning("m", "w");
    log.error("m", "e");
    log.critical("m", "c");
    EXPECT_EQ(mem->snapshot().size(), 3u);
}

TEST(Logger, FailingSinkDoesNotBreakOthersOrCaller) {
    Logger log;
    auto mem = std::make_shared<MemorySink>();
    log.addSink(std::make_shared<ThrowingSink>());
    log.addSink(mem);
    EXPECT_NO_THROW(log.error("m", "x"));
    EXPECT_EQ(mem->snapshot().size(), 1u);
}

TEST(Logger, MemorySinkIsARingBuffer) {
    MemorySink mem(3);
    for (int i = 0; i < 10; ++i)
        mem.write({Level::Info, "m", std::to_string(i), std::chrono::system_clock::now()});
    auto s = mem.snapshot();
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s.front().message, "7");
    EXPECT_EQ(s.back().message, "9");
}
