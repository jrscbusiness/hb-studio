#pragma once
#include <chrono>
#include <deque>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hb::core {

enum class Level { Debug, Info, Warning, Error, Critical };

const char* levelName(Level level);

struct LogRecord {
    Level level;
    std::string module;
    std::string message;  // already redacted
    std::chrono::system_clock::time_point time;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

std::string formatRecord(const LogRecord& record);

// Keeps the last N records in memory; used for the in-app log viewer and
// for "export logs".
class MemorySink : public LogSink {
public:
    explicit MemorySink(std::size_t capacity = 5000) : capacity_(capacity) {}
    void write(const LogRecord& record) override;
    std::vector<LogRecord> snapshot() const;
    void exportTo(std::ostream& out) const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<LogRecord> records_;
};

class StreamSink : public LogSink {
public:
    explicit StreamSink(std::ostream& out) : out_(out) {}
    void write(const LogRecord& record) override;

private:
    std::ostream& out_;
    std::mutex mutex_;
};

// Every message passes through redactSecrets() before reaching any sink, so
// credentials cannot leak regardless of which sink is attached.
// Sink failures are swallowed: logging must never take the app down.
class Logger {
public:
    void setMinLevel(Level level);
    void addSink(std::shared_ptr<LogSink> sink);
    void log(Level level, std::string_view module, std::string_view message);

    void debug(std::string_view m, std::string_view msg) { log(Level::Debug, m, msg); }
    void info(std::string_view m, std::string_view msg) { log(Level::Info, m, msg); }
    void warning(std::string_view m, std::string_view msg) { log(Level::Warning, m, msg); }
    void error(std::string_view m, std::string_view msg) { log(Level::Error, m, msg); }
    void critical(std::string_view m, std::string_view msg) { log(Level::Critical, m, msg); }

private:
    std::mutex mutex_;
    Level minLevel_ = Level::Info;
    std::vector<std::shared_ptr<LogSink>> sinks_;
};

}  // namespace hb::core
