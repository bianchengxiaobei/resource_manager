#include <catch_amalgamated.hpp>

#include "parcel/core/time.hpp"

#include <chrono>
#include <string>

using parcel::core::FakeClock;
using parcel::core::FormatUtcTimestamp;
using parcel::core::SystemClock;

TEST_CASE("FakeClock advances deterministically", "[core][time]") {
    FakeClock clock;
    const auto start = clock.Now();

    clock.Advance(std::chrono::milliseconds(15));
    REQUIRE(clock.Now() == start + std::chrono::milliseconds(15));

    clock.Advance(std::chrono::milliseconds(-5));
    REQUIRE(clock.Now() == start + std::chrono::milliseconds(10));

    const auto replacement = start + std::chrono::seconds(3);
    clock.SetNow(replacement);
    REQUIRE(clock.Now() == replacement);
}

TEST_CASE("SystemClock moves forward", "[core][time]") {
    SystemClock clock;
    const auto first = clock.Now();
    const auto second = clock.Now();
    REQUIRE(second >= first);
}

TEST_CASE("UTC timestamp format is stable", "[core][time]") {
    const auto epoch = std::chrono::system_clock::time_point{};
    REQUIRE(FormatUtcTimestamp(epoch) == "1970-01-01T00:00:00.000Z");

    const auto later = epoch + std::chrono::milliseconds(1234);
    REQUIRE(FormatUtcTimestamp(later) == "1970-01-01T00:00:01.234Z");
}
