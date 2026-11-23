#pragma once

#include "parcel/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace parcel::core {

struct Guid128 {
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool IsZero() const noexcept;
    [[nodiscard]] std::string ToHex() const;
    [[nodiscard]] static Guid128 FromBytes(std::span<const std::byte, 16> bytes) noexcept;
    [[nodiscard]] static Result<Guid128> FromHex(std::string_view hex);

    auto operator<=>(const Guid128&) const = default;
};

struct Guid128Hash {
    [[nodiscard]] std::size_t operator()(const Guid128& guid) const noexcept;
};

using AssetGuid = Guid128;

} // namespace parcel::core
