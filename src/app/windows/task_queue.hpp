#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace hb::win {

// One background thread that runs slow jobs (device enumeration, closing capture graphs) in order,
// so the UI thread and the Program clock never wait for a camera driver.
class TaskQueue {
public:
    TaskQueue() : thread_([this] { loop(); }) {}
    ~TaskQueue() {
        {
            std::lock_guard lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();   // runs the jobs that are still queued first
    }
    void post(std::function<void()> job) {
        {
            std::lock_guard lock(m_);
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return quit_ || !q_.empty(); });
                if (q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            try { job(); } catch (...) {}
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace hb::win
