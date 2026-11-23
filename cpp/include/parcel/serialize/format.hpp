#pragma once

#include "parcel/core/error.hpp"
#include "parcel/core/result.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace parcel::serialize {

using core::Result;

inline constexpr char kAssetMagic[4] = {'P', 'R', 'C', 'A'};
inline constexpr char kBundleMagic[4] = {'P', 'R', 'C', 'L'};
inline constexpr char kManifestMagic[4] = {'P', 'R', 'C', 'M'};

inline constexpr std::uint16_t kAssetVersion = 1;
inline constexpr std::uint16_t kBundleVersion = 1;
inline constexpr std::uint16_t kManifestVersion = 1;

inline constexpr std::uint16_t kAssetHeaderSize = 128;
inline constexpr std::uint16_t kBundleHeaderSize = 128;
inline constexpr std::uint16_t kManifestHeaderSize = 160;

inline constexpr std::uint64_t kNoBlob = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::size_t kMaxTableEntries = 10U * 1000U * 1000U;
inline constexpr std::uint32_t kMaxBlockSize = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kDefaultBlockSize = 256U * 1024U;
inline constexpr std::uint32_t kMaxTocRawSize = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxCompressedBlockSize = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxTableBytes = 512U * 1024U * 1024U;

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] constexpr std::uint64_t End() const noexcept { return offset + size; }

    [[nodiscard]] constexpr bool Contains(std::uint64_t value) const noexcept {
        return value >= offset && value < End();
    }

    [[nodiscard]] constexpr bool Overlaps(const ByteRange& other) const noexcept {
        return offset < other.End() && other.offset < End();
    }
};

[[nodiscard]] Result<void> CheckVersion(std::uint16_t actual, std::uint16_t expected,
                                        std::string_view format_name);

[[nodiscard]] Result<ByteRange> CheckRange(std::uint64_t file_size, std::uint64_t offset,
                                           std::uint64_t size, std::uint64_t alignment = 1,
                                           std::string_view what = "range");

[[nodiscard]] Result<ByteRange> CheckTableRange(std::uint64_t file_size, std::uint64_t offset,
                                                std::uint64_t count, std::uint64_t entry_size,
                                                std::uint64_t alignment = 1,
                                                std::string_view what = "table");

[[nodiscard]] Result<void> CheckNoOverlap(std::span<const ByteRange> ranges,
                                          std::string_view what);

} // namespace parcel::serialize
