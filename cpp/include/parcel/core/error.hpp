#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace parcel::core {

enum class ErrorCode : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    NotFound,
    Io,
    Corrupt,
    HashMismatch,
    Timeout,
    Canceled,
    Unsupported,
    Internal,
    Http,
};

[[nodiscard]] constexpr std::string_view ToString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "Ok";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::Io:
        return "Io";
    case ErrorCode::Corrupt:
        return "Corrupt";
    case ErrorCode::HashMismatch:
        return "HashMismatch";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::Canceled:
        return "Canceled";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::Internal:
        return "Internal";
    case ErrorCode::Http:
        return "Http";
    }
    return "Unknown";
}

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
    std::string package;
    std::string bundle;
    std::string asset;
    std::string path;
    int http_status = 0;

    Error() = default;

    explicit Error(ErrorCode error_code, std::string error_message = {})
        : code(error_code), message(std::move(error_message)) {}

    [[nodiscard]] bool IsOk() const noexcept { return code == ErrorCode::Ok; }

    [[nodiscard]] std::string ToString() const {
        std::string result(parcel::core::ToString(code));
        if (!message.empty()) {
            result += ": ";
            result += message;
        }
        if (!package.empty()) {
            result += " [package=";
            result += package;
            result += ']';
        }
        if (!bundle.empty()) {
            result += " [bundle=";
            result += bundle;
            result += ']';
        }
        if (!asset.empty()) {
            result += " [asset=";
            result += asset;
            result += ']';
        }
        if (!path.empty()) {
            result += " [path=";
            result += path;
            result += ']';
        }
        if (http_status != 0) {
            result += " [http=";
            result += std::to_string(http_status);
            result += ']';
        }
        return result;
    }

    Error& WithPackage(std::string value) {
        package = std::move(value);
        return *this;
    }

    Error& WithBundle(std::string value) {
        bundle = std::move(value);
        return *this;
    }

    Error& WithAsset(std::string value) {
        asset = std::move(value);
        return *this;
    }

    Error& WithPath(std::string value) {
        path = std::move(value);
        return *this;
    }

    Error& WithHttpStatus(int value) {
        http_status = value;
        return *this;
    }
};

[[nodiscard]] inline Error MakeError(ErrorCode code, std::string message = {}) {
    return Error(code, std::move(message));
}

} // namespace parcel::core
