#pragma once

#include "parcel/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace parcel::serialize {

using core::Result;

enum class CompressionCodec : std::uint8_t {
    None = 0,
    Lz4 = 1,
    Zstd = 2,
};

struct CompressionOptions {
    int level = 3;
};

[[nodiscard]] constexpr std::string_view ToString(CompressionCodec codec) noexcept {
    switch (codec) {
    case CompressionCodec::None:
        return "None";
    case CompressionCodec::Lz4:
        return "Lz4";
    case CompressionCodec::Zstd:
        return "Zstd";
    }
    return "Unknown";
}

[[nodiscard]] Result<CompressionCodec> ParseCompressionCodec(std::uint8_t value);

[[nodiscard]] Result<std::size_t> CompressionBound(CompressionCodec codec,
                                                   std::size_t input_size);

[[nodiscard]] Result<std::vector<std::byte>> Compress(
    CompressionCodec codec, std::span<const std::byte> input,
    const CompressionOptions& options = {});

[[nodiscard]] Result<std::vector<std::byte>> Decompress(
    CompressionCodec codec, std::span<const std::byte> compressed, std::size_t expected_size);

} // namespace parcel::serialize
