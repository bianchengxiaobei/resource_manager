#include "parcel/core/guid.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace parcel::core {
namespace {

[[nodiscard]] constexpr std::uint8_t HexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    return 0xFFU;
}

} // namespace

bool Guid128::IsZero() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0; });
}

std::string Guid128::ToHex() const {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.resize(32);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = kDigits[bytes[i] >> 4U];
        result[i * 2 + 1] = kDigits[bytes[i] & 0x0FU];
    }
    return result;
}

Guid128 Guid128::FromBytes(std::span<const std::byte, 16> bytes) noexcept {
    Guid128 result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        result.bytes[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    return result;
}

Result<Guid128> Guid128::FromHex(std::string_view hex) {
    std::array<char, 32> digits{};
    std::size_t digit_count = 0;
    for (char value : hex) {
        if (value == '-') {
            continue;
        }
        if (digit_count >= digits.size()) {
            return MakeError(ErrorCode::InvalidArgument, "GUID has too many hex digits");
        }
        digits[digit_count++] = value;
    }

    if (digit_count != digits.size()) {
        return MakeError(ErrorCode::InvalidArgument, "GUID must contain exactly 32 hex digits");
    }

    Guid128 result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        const std::uint8_t high = HexDigit(digits[i * 2]);
        const std::uint8_t low = HexDigit(digits[i * 2 + 1]);
        if (high == 0xFFU || low == 0xFFU) {
            return MakeError(ErrorCode::InvalidArgument, "GUID contains non-hex characters");
        }
        result.bytes[i] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return result;
}

std::size_t Guid128Hash::operator()(const Guid128& guid) const noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffsetBasis;
    for (std::uint8_t value : guid.bytes) {
        hash ^= value;
        hash *= kPrime;
    }
    return static_cast<std::size_t>(hash);
}

} // namespace parcel::core
