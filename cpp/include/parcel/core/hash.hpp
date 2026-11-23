#pragma once

#include "parcel/core/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace parcel::core {

struct Hash256 {
    std::array<std::uint8_t, 32> bytes{};

    [[nodiscard]] bool IsZero() const noexcept;
    [[nodiscard]] std::string ToHex() const;
    [[nodiscard]] static Result<Hash256> FromHex(std::string_view hex);

    friend bool operator==(const Hash256& lhs, const Hash256& rhs) noexcept {
        return lhs.bytes == rhs.bytes;
    }

    friend bool operator!=(const Hash256& lhs, const Hash256& rhs) noexcept {
        return !(lhs == rhs);
    }
};

[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t Crc32(std::string_view data, std::uint32_t seed = 0) noexcept;

[[nodiscard]] Hash256 Sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Hash256 Sha256(std::string_view data) noexcept;

[[nodiscard]] Result<Hash256> Sha256File(std::string_view path);

} // namespace parcel::core
