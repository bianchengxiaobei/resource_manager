#include <catch_amalgamated.hpp>

#include "parcel/core/log.hpp"

#include <mutex>
#include <string>
#include <vector>

using parcel::core::ILogger;
using parcel::core::Log;
using parcel::core::LogLevel;
using parcel::core::LogRecord;

namespace {

class CaptureLogger final : public ILogger {
  public:
    void Log(const LogRecord& record) override {
        std::lock_guard lock(mutex_);
        records_.push_back(record);
    }

    std::vector<LogRecord> Records() const {
        std::lock_guard lock(mutex_);
        return records_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

} // namespace

TEST_CASE("Log filters by level and forwards records", "[core][log]") {
    auto logger = std::make_shared<CaptureLogger>();
    Log::SetLogger(logger);
    Log::SetLevel(LogLevel::Warn);

    Log::Write(LogLevel::Info, "test", "hidden");
    Log::Write(LogLevel::Warn, "test", "visible");
    Log::Write(LogLevel::Error, "test", "error");

    const auto records = logger->Records();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].level == LogLevel::Warn);
    REQUIRE(records[0].category == "test");
    REQUIRE(records[0].message == "visible");
    REQUIRE(records[1].level == LogLevel::Error);

    Log::SetLevel(LogLevel::Info);
    Log::SetLogger(nullptr);
}

TEST_CASE("Log level Off disables all records", "[core][log]") {
    auto logger = std::make_shared<CaptureLogger>();
    Log::SetLogger(logger);
    Log::SetLevel(LogLevel::Off);

    Log::Write(LogLevel::Fatal, "test", "hidden");

    REQUIRE(logger->Records().empty());
    Log::SetLevel(LogLevel::Info);
    Log::SetLogger(nullptr);
}
