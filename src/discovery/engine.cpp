#include "discovery/engine.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace hb::discovery {
namespace {

struct Shared {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    SourceResult result;
    std::string exception;
};

template <class T>
void addUnique(std::vector<T>& dst, const std::vector<T>& src) {
    for (const auto& v : src)
        if (std::find(dst.begin(), dst.end(), v) == dst.end()) dst.push_back(v);
}

}  // namespace

std::vector<DiscoveredDevice> mergeDevices(const std::vector<DiscoveredDevice>& in) {
    std::vector<DiscoveredDevice> out;
    for (const auto& d : in) {
        auto it = std::find_if(out.begin(), out.end(), [&](const DiscoveredDevice& o) {
            return (!d.id.empty() && d.id == o.id) || (!d.ip.empty() && d.ip == o.ip);
        });
        if (it == out.end()) {
            out.push_back(d);
            continue;
        }
        if (it->name.empty() || it->name == it->ip) it->name = d.name.empty() ? it->name : d.name;
        if (it->manufacturer.empty()) it->manufacturer = d.manufacturer;
        if (it->ip.empty()) it->ip = d.ip;
        addUnique(it->protocols, d.protocols);
        addUnique(it->xaddrs, d.xaddrs);
        it->ptz = it->ptz || d.ptz;
        if (it->state == DeviceState::Unreachable) it->state = d.state;
    }
    return out;
}

void DiscoveryEngine::addSource(std::shared_ptr<IDiscoverySource> source, std::chrono::milliseconds timeout) {
    if (source) sources_.push_back({std::move(source), timeout});
}

DiscoveryReport DiscoveryEngine::run() {
    using Clock = std::chrono::steady_clock;
    struct Running {
        std::shared_ptr<Shared> shared;
        CancelToken cancel;
    };
    std::vector<Running> running;
    const auto start = Clock::now();
    for (const auto& e : sources_) {
        Running r{std::make_shared<Shared>(), CancelToken{}};
        // The thread owns shared_ptrs to everything it touches, so abandoning it is safe.
        std::thread([shared = r.shared, source = e.source, cancel = r.cancel] {
            SourceResult res;
            std::string ex;
            try {
                res = source->discover(cancel);
            } catch (const std::exception& err) {
                ex = err.what();
            } catch (...) {
                ex = "unknown exception";
            }
            std::lock_guard<std::mutex> lock(shared->m);
            shared->result = std::move(res);
            shared->exception = std::move(ex);
            shared->done = true;
            shared->cv.notify_all();
        }).detach();
        running.push_back(std::move(r));
    }

    DiscoveryReport report;
    std::vector<DiscoveredDevice> all;
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        auto& r = running[i];
        SourceReport sr;
        sr.name = sources_[i].source->name();
        std::unique_lock<std::mutex> lock(r.shared->m);
        const bool finished = r.shared->cv.wait_until(lock, start + sources_[i].timeout, [&] { return r.shared->done; });
        sr.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        if (!finished) {
            r.cancel.cancel();
            sr.status = SourceReport::Status::TimedOut;
            sr.message = "timed out after " + std::to_string(sources_[i].timeout.count()) + " ms";
        } else if (!r.shared->exception.empty()) {
            sr.status = SourceReport::Status::Exception;
            sr.message = r.shared->exception;
        } else {
            for (auto d : r.shared->result.devices) {
                if (d.source.empty()) d.source = sr.name;
                all.push_back(std::move(d));
                ++sr.deviceCount;
            }
            if (!r.shared->result.ok()) {
                sr.status = SourceReport::Status::Failed;
                sr.message = r.shared->result.error;
            }
        }
        report.sources.push_back(std::move(sr));
    }
    report.devices = mergeDevices(all);
    return report;
}

}  // namespace hb::discovery
