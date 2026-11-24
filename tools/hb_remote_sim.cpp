// Developer tool: runs the REAL remote-control server on 127.0.0.1 with a fake switcher behind it,
// so the mobile page can be driven by a real browser (tools/ui/remote_ui_test.js).
//   hb_remote_sim [port]   prints "READY <port> <pin>" then one line per command received.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "remote/service.hpp"

int main(int argc, char** argv) {
    using namespace hb::remote;
    std::mutex mu;
    RemoteState st;
    for (int i = 0; i < 16; ++i) st.sources.push_back({"CAM " + std::to_string(i + 1), i == 3 ? "nosignal" : "online"});
    st.strips = {{"Mic 1", -6, false, false, -20}, {"Mic 2", 0, true, false, -50}};
    st.dests = {{"d1", "YouTube", "youtube", false, false}};
    st.pvw = 1;
    ServiceOptions opt;
    opt.bindOverride = {"127.0.0.1"};
    opt.preferredPort = argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 0;
    RemoteService svc(
        [&](const std::string& c) {
            std::lock_guard l(mu);
            std::printf("CMD %s\n", c.c_str());
            std::fflush(stdout);
            auto num = [&](std::size_t from) { return std::atoi(c.c_str() + from); };
            if (c == "cut") std::swap(st.pvw, st.pgm);
            else if (c.rfind("preview|", 0) == 0) st.pvw = num(8);
            else if (c.rfind("program|", 0) == 0) st.pgm = num(8);
            else if (c == "rec|start") st.recording = true;
            else if (c == "rec|stop") st.recording = false;
            else if (c.rfind("xmitstart|", 0) == 0) st.dests[0].running = st.dests[0].publishing = true;
            else if (c.rfind("xmitstop|", 0) == 0) st.dests[0].running = st.dests[0].publishing = false;
        },
        {}, opt);
    std::string err;
    if (!svc.enable(err)) {
        std::fprintf(stderr, "enable failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("READY %u %s\n", svc.port(), svc.auth().pin().c_str());
    std::fflush(stdout);
    std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
    for (;;) {
        {
            std::lock_guard l(mu);
            svc.pushState(st);
        }
        if (svc.wantsThumbs()) {
            svc.pushThumb(0, jpeg);
            svc.pushThumb(1, jpeg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
