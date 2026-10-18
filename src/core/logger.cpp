#include "core/logger.hpp"

#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>

#include "core/redact.hpp"

namespace hb::core {

const char* levelName(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
        case Level::Critical: return "CRITICAL";
    }
    return "?";
}

std::string formatRecord(const LogRecord& r) {
    const auto t = std::chrono::system_clock::to_time_t(r.time);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        r.time.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
       << std::setw(3) << ms << "Z [" << levelName(r.level) << "] [" << r.module
       << "] " << r.message;
    return os.str();
}

void MemorySink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    records_.push_back(record);
    while (records_.size() > capacity_) records_.pop_front();
}

std::vector<LogRecord> MemorySink::snapshot() const {
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.end()};
}

void MemorySink::exportTo(std::ostream& out) const {
    for (const auto& r : snapshot()) out << formatRecord(r) << '\n';
}

void StreamSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    out_ << formatRecord(record) << '\n';
}

void Logger::setMinLevel(Level level) {
    std::lock_guard lock(mutex_);
    minLevel_ = level;
}

void Logger::addSink(std::shared_ptr<LogSink> sink) {
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::log(Level level, std::string_view module, std::string_view message) {
    std::vector<std::shared_ptr<LogSink>> sinks;
    {
        std::lock_guard lock(mutex_);
        if (level < minLevel_) return;
        sinks = sinks_;
    }
    LogRecord record{level, redactSecrets(module), redactSecrets(message),
                     std::chrono::system_clock::now()};
    for (const auto& s : sinks) {
        try {
            s->write(record);
        } catch (...) {
        }
    }
}

}  // namespace hb::core
