#include <catch_amalgamated.hpp>

#include "parcel/core/result.hpp"

#include <memory>
#include <string>

using parcel::core::Error;
using parcel::core::ErrorCode;
using parcel::core::Result;

namespace {

Result<int> ParsePositive(int value) {
    if (value <= 0) {
        return Error(ErrorCode::InvalidArgument, "value must be positive");
    }
    return value;
}

Result<int> DoublePositive(int value) {
    int parsed = 0;
    PARCEL_TRY_ASSIGN(parsed, ParsePositive(value));
    return parsed * 2;
}

Result<void> RequirePositive(int value) {
    if (value <= 0) {
        return Error(ErrorCode::InvalidArgument, "value must be positive");
    }
    return {};
}

} // namespace

TEST_CASE("Result carries values and errors", "[core][result]") {
    auto value = ParsePositive(7);
    REQUIRE(value.HasValue());
    REQUIRE(value.Value() == 7);

    auto error = ParsePositive(0);
    REQUIRE_FALSE(error.HasValue());
    REQUIRE(error.GetError().code == ErrorCode::InvalidArgument);
    REQUIRE(error.GetError().message == "value must be positive");
}

TEST_CASE("Result supports move-only values", "[core][result]") {
    Result<std::unique_ptr<int>> result(std::make_unique<int>(42));
    REQUIRE(result.HasValue());
    auto value = std::move(result).Value();
    REQUIRE(value != nullptr);
    REQUIRE(*value == 42);
}

TEST_CASE("Result<void> represents success and failure", "[core][result]") {
    auto success = RequirePositive(3);
    REQUIRE(success.HasValue());

    auto failure = RequirePositive(-1);
    REQUIRE_FALSE(failure.HasValue());
    REQUIRE(failure.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("PARCEL_TRY propagates errors", "[core][result]") {
    auto success = DoublePositive(4);
    REQUIRE(success.HasValue());
    REQUIRE(success.Value() == 8);

    auto failure = DoublePositive(-4);
    REQUIRE_FALSE(failure.HasValue());
    REQUIRE(failure.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Result error context is preserved", "[core][result]") {
    Error error(ErrorCode::Io, "open failed");
    error.WithPackage("main").WithBundle("ui").WithAsset("texture").WithPath("a/b").WithHttpStatus(
        503);

    Result<int> result(std::move(error));
    REQUIRE_FALSE(result.HasValue());
    REQUIRE(result.GetError().package == "main");
    REQUIRE(result.GetError().bundle == "ui");
    REQUIRE(result.GetError().asset == "texture");
    REQUIRE(result.GetError().path == "a/b");
    REQUIRE(result.GetError().http_status == 503);
}
