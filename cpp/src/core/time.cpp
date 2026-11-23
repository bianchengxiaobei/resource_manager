#include "parcel/core/time.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace parcel::core {

std::string FormatUtcTimestamp(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(value - seconds).count();

    const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif

    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << millis << 'Z';
    return stream.str();
}

} // namespace parcel::core
