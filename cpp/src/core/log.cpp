#include "parcel/core/log.hpp"

#include "parcel/core/time.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace parcel::core {
namespace {

struct LogState {
    std::mutex mutex;
    std::shared_ptr<ILogger> logger;
    std::atomic<LogLevel> level{LogLevel::Info};

    LogState() : logger(std::make_shared<StderrLogger>()) {}
};

LogState& State() {
    static LogState state;
    return state;
}

int ToInt(std::string_view value) {
    if (value.size() > static_cast<std::size_t>(0x7fffffff)) {
        return 0x7fffffff;
    }
    return static_cast<int>(value.size());
}

} // namespace

void StderrLogger::Log(const LogRecord& record) {
    const std::string timestamp = FormatUtcTimestamp(record.time);
    const std::string level(ToString(record.level));
    const std::string category =
        record.category.empty() ? std::string("parcel") : std::string(record.category);
    const auto thread_hash = std::hash<std::thread::id>{}(record.thread);

    std::lock_guard lock(mutex_);
    std::fprintf(stderr, "%s [%s] [%s] [tid=%zu] %.*s\n", timestamp.c_str(), level.c_str(),
                 category.c_str(), static_cast<std::size_t>(thread_hash), ToInt(record.message),
                 record.message.data());
    std::fflush(stderr);
}

void Log::SetLogger(std::shared_ptr<ILogger> logger) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    state.logger = std::move(logger);
    if (!state.logger) {
        state.logger = std::make_shared<StderrLogger>();
    }
}

std::shared_ptr<ILogger> Log::GetLogger() {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.logger;
}

void Log::SetLevel(LogLevel level) noexcept {
    State().level.store(level, std::memory_order_relaxed);
}

LogLevel Log::Level() noexcept { return State().level.load(std::memory_order_relaxed); }

bool Log::IsEnabled(LogLevel level) noexcept {
    const LogLevel current = Level();
    return level != LogLevel::Off && current != LogLevel::Off &&
           static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(current);
}

void Log::Write(LogLevel level, std::string_view category, std::string_view message) {
    if (!IsEnabled(level)) {
        return;
    }

    const auto logger = GetLogger();
    if (!logger) {
        return;
    }

    LogRecord record;
    record.level = level;
    record.category = category;
    record.message = message;
    record.time = std::chrono::system_clock::now();
    record.thread = std::this_thread::get_id();
    logger->Log(record);
}

} // namespace parcel::core
