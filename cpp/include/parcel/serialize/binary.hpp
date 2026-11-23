#pragma once

#include "parcel/core/error.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace parcel::serialize {

using core::Result;

inline constexpr std::size_t kDefaultMaxStringBytes = 16U * 1024U * 1024U;

class BinaryWriter {
  public:
    explicit BinaryWriter(std::size_t reserve = 0);

    [[nodiscard]] Result<void> WriteU8(std::uint8_t value);
    [[nodiscard]] Result<void> WriteU16(std::uint16_t value);
    [[nodiscard]] Result<void> WriteU32(std::uint32_t value);
    [[nodiscard]] Result<void> WriteU64(std::uint64_t value);
    [[nodiscard]] Result<void> WriteI32(std::int32_t value);
    [[nodiscard]] Result<void> WriteI64(std::int64_t value);
    [[nodiscard]] Result<void> WriteF32(float value);
    [[nodiscard]] Result<void> WriteF64(double value);
    [[nodiscard]] Result<void> WriteBool(bool value);
    [[nodiscard]] Result<void> WriteBytes(std::span<const std::byte> value);
    [[nodiscard]] Result<void> WriteString(std::string_view value);
    [[nodiscard]] Result<void> WriteCString(std::string_view value);
    [[nodiscard]] Result<void> WriteHash256(const core::Hash256& value);
    [[nodiscard]] Result<void> WriteGuid128(const core::Guid128& value);
    [[nodiscard]] Result<void> WriteZero(std::size_t count);
    [[nodiscard]] Result<void> Align(std::size_t alignment, std::byte fill = std::byte{0});

    [[nodiscard]] Result<void> PatchU8(std::size_t offset, std::uint8_t value);
    [[nodiscard]] Result<void> PatchU16(std::size_t offset, std::uint16_t value);
    [[nodiscard]] Result<void> PatchU32(std::size_t offset, std::uint32_t value);
    [[nodiscard]] Result<void> PatchU64(std::size_t offset, std::uint64_t value);
    [[nodiscard]] Result<void> PatchHash256(std::size_t offset, const core::Hash256& value);
    [[nodiscard]] Result<void> PatchGuid128(std::size_t offset, const core::Guid128& value);
    [[nodiscard]] Result<void> PatchBytes(std::size_t offset, std::span<const std::byte> value);

    [[nodiscard]] std::size_t Tell() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] std::span<const std::byte> Data() const noexcept;
    [[nodiscard]] std::vector<std::byte> Release() noexcept;

    void Reserve(std::size_t capacity);
    void Clear() noexcept;

  private:
    [[nodiscard]] Result<void> Append(std::span<const std::byte> value);
    [[nodiscard]] Result<void> IsPatchable(std::size_t offset, std::size_t size) const;

    std::vector<std::byte> data_;
};

class BinaryReader {
  public:
    explicit BinaryReader(std::span<const std::byte> data) noexcept;

    [[nodiscard]] Result<std::uint8_t> ReadU8();
    [[nodiscard]] Result<std::uint16_t> ReadU16();
    [[nodiscard]] Result<std::uint32_t> ReadU32();
    [[nodiscard]] Result<std::uint64_t> ReadU64();
    [[nodiscard]] Result<std::int32_t> ReadI32();
    [[nodiscard]] Result<std::int64_t> ReadI64();
    [[nodiscard]] Result<float> ReadF32();
    [[nodiscard]] Result<double> ReadF64();
    [[nodiscard]] Result<bool> ReadBool();
    [[nodiscard]] Result<std::span<const std::byte>> ReadBytes(std::size_t count);
    [[nodiscard]] Result<std::string_view> ReadString(
        std::size_t max_bytes = kDefaultMaxStringBytes);
    [[nodiscard]] Result<std::string_view> ReadCString(
        std::size_t max_bytes = kDefaultMaxStringBytes);
    [[nodiscard]] Result<core::Hash256> ReadHash256();
    [[nodiscard]] Result<core::Guid128> ReadGuid128();
    [[nodiscard]] Result<void> Skip(std::size_t count);
    [[nodiscard]] Result<BinaryReader> ReadSubReader(std::size_t count);

    [[nodiscard]] std::size_t Position() const noexcept;
    [[nodiscard]] std::size_t Remaining() const noexcept;
    [[nodiscard]] bool IsAtEnd() const noexcept;
    [[nodiscard]] std::span<const std::byte> Data() const noexcept;

  private:
    [[nodiscard]] const std::byte* Require(std::size_t count);

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};

} // namespace parcel::serialize
