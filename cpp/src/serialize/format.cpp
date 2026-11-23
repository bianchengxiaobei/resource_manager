#include "parcel/serialize/format.hpp"

#include <string>

namespace parcel::serialize {

Result<void> CheckVersion(std::uint16_t actual, std::uint16_t expected,
                          std::string_view format_name) {
    if (actual != expected) {
        return core::MakeError(core::ErrorCode::Unsupported,
                               std::string(format_name) + " version is not supported: " +
                                   std::to_string(actual) + " (expected " +
                                   std::to_string(expected) + ")");
    }
    return {};
}

Result<ByteRange> CheckRange(std::uint64_t file_size, std::uint64_t offset, std::uint64_t size,
                             std::uint64_t alignment, std::string_view what) {
    if (alignment > 1 && (offset % alignment) != 0) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               std::string(what) + " is not aligned");
    }
    if (offset > file_size || size > file_size - offset) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               std::string(what) + " is out of bounds");
    }
    return ByteRange{offset, size};
}

Result<ByteRange> CheckTableRange(std::uint64_t file_size, std::uint64_t offset,
                                  std::uint64_t count, std::uint64_t entry_size,
                                  std::uint64_t alignment, std::string_view what) {
    if (count > kMaxTableEntries) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               std::string(what) + " entry count exceeds the limit");
    }
    if (entry_size != 0 && count > std::numeric_limits<std::uint64_t>::max() / entry_size) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               std::string(what) + " byte size overflows");
    }
    const std::uint64_t size = count * entry_size;
    if (size > kMaxTableBytes) {
        return core::MakeError(core::ErrorCode::Corrupt,
                               std::string(what) + " byte size exceeds the limit");
    }
    return CheckRange(file_size, offset, size, alignment, what);
}

Result<void> CheckNoOverlap(std::span<const ByteRange> ranges, std::string_view what) {
    std::vector<ByteRange> sorted(ranges.begin(), ranges.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const ByteRange& lhs, const ByteRange& rhs) { return lhs.offset < rhs.offset; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1].Overlaps(sorted[i])) {
            return core::MakeError(core::ErrorCode::Corrupt,
                                   std::string(what) + " ranges overlap");
        }
    }
    return {};
}

} // namespace parcel::serialize
