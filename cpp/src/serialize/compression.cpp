#include "parcel/serialize/compression.hpp"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <limits>
#include <string>

namespace parcel::serialize {

Result<CompressionCodec> ParseCompressionCodec(std::uint8_t value) {
    switch (value) {
    case static_cast<std::uint8_t>(CompressionCodec::None):
        return CompressionCodec::None;
    case static_cast<std::uint8_t>(CompressionCodec::Lz4):
        return CompressionCodec::Lz4;
    case static_cast<std::uint8_t>(CompressionCodec::Zstd):
        return CompressionCodec::Zstd;
    default:
        return core::MakeError(core::ErrorCode::Unsupported, "unknown compression codec");
    }
}

Result<std::size_t> CompressionBound(CompressionCodec codec, std::size_t input_size) {
    switch (codec) {
    case CompressionCodec::None:
        return input_size;
    case CompressionCodec::Lz4: {
        if (input_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return core::MakeError(core::ErrorCode::InvalidArgument,
                                   "LZ4 input exceeds its 32-bit size limit");
        }
        const int bound = LZ4_compressBound(static_cast<int>(input_size));
        if (bound <= 0) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "LZ4 compression bound failed");
        }
        return static_cast<std::size_t>(bound);
    }
    case CompressionCodec::Zstd:
        return ZSTD_compressBound(input_size);
    }
    return core::MakeError(core::ErrorCode::Unsupported, "unknown compression codec");
}

Result<std::vector<std::byte>> Compress(CompressionCodec codec,
                                        std::span<const std::byte> input,
                                        const CompressionOptions& options) {
    if (codec == CompressionCodec::None) {
        return std::vector<std::byte>(input.begin(), input.end());
    }

    if (input.empty()) {
        return std::vector<std::byte>{};
    }

    auto bound = CompressionBound(codec, input.size());
    if (!bound) {
        return std::move(bound).TakeError();
    }

    std::vector<std::byte> output(bound.Value());
    switch (codec) {
    case CompressionCodec::Lz4: {
        const int result = LZ4_compress_default(
            reinterpret_cast<const char*>(input.data()),
            reinterpret_cast<char*>(output.data()), static_cast<int>(input.size()),
            static_cast<int>(output.size()));
        if (result <= 0) {
            return core::MakeError(core::ErrorCode::Internal, "LZ4 compression failed");
        }
        output.resize(static_cast<std::size_t>(result));
        return output;
    }
    case CompressionCodec::Zstd: {
        const std::size_t result =
            ZSTD_compress(output.data(), output.size(), input.data(), input.size(), options.level);
        if (ZSTD_isError(result) != 0U) {
            return core::MakeError(core::ErrorCode::Internal,
                                   std::string("Zstd compression failed: ") +
                                       ZSTD_getErrorName(result));
        }
        output.resize(result);
        return output;
    }
    case CompressionCodec::None:
        break;
    }
    return core::MakeError(core::ErrorCode::Unsupported, "unknown compression codec");
}

Result<std::vector<std::byte>> Decompress(CompressionCodec codec,
                                          std::span<const std::byte> compressed,
                                          std::size_t expected_size) {
    if (codec == CompressionCodec::None) {
        if (compressed.size() != expected_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "uncompressed size does not match the table");
        }
        return std::vector<std::byte>(compressed.begin(), compressed.end());
    }

    if (expected_size == 0) {
        if (!compressed.empty()) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "non-empty compressed data for an empty block");
        }
        return std::vector<std::byte>{};
    }

    std::vector<std::byte> output(expected_size);
    switch (codec) {
    case CompressionCodec::Lz4: {
        if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            expected_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "LZ4 compressed or uncompressed size exceeds 32-bit limit");
        }
        const int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(compressed.data()),
            reinterpret_cast<char*>(output.data()), static_cast<int>(compressed.size()),
            static_cast<int>(output.size()));
        if (result < 0 || static_cast<std::size_t>(result) != expected_size) {
            return core::MakeError(core::ErrorCode::Corrupt, "LZ4 decompression failed");
        }
        return output;
    }
    case CompressionCodec::Zstd: {
        const std::size_t result =
            ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(result) != 0U) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   std::string("Zstd decompression failed: ") +
                                       ZSTD_getErrorName(result));
        }
        if (result != expected_size) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   "Zstd decompressed size does not match the table");
        }
        return output;
    }
    case CompressionCodec::None:
        break;
    }
    return core::MakeError(core::ErrorCode::Unsupported, "unknown compression codec");
}

} // namespace parcel::serialize
