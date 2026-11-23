#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace parcel::core {

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

[[nodiscard]] constexpr std::string_view ToString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    case LogLevel::Off:
        return "OFF";
    }
    return "UNKNOWN";
}

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view category;
    std::string_view message;
    std::chrono::system_clock::time_point time{};
    std::thread::id thread{};
};

class ILogger {
  public:
    virtual ~ILogger() = default;
    virtual void Log(const LogRecord& record) = 0;
};

class StderrLogger final : public ILogger {
  public:
    void Log(const LogRecord& record) override;

  private:
    std::mutex mutex_;
};

class Log final {
  public:
    static void SetLogger(std::shared_ptr<ILogger> logger);
    [[nodiscard]] static std::shared_ptr<ILogger> GetLogger();

    static void SetLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel Level() noexcept;
    [[nodiscard]] static bool IsEnabled(LogLevel level) noexcept;

    static void Write(LogLevel level, std::string_view category, std::string_view message);
};

} // namespace parcel::core
