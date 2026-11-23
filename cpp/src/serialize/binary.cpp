#include "parcel/serialize/binary.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace parcel::serialize {

BinaryWriter::BinaryWriter(std::size_t reserve) {
    if (reserve != 0) {
        data_.reserve(reserve);
    }
}

Result<void> BinaryWriter::Append(std::span<const std::byte> value) {
    if (value.size() > std::numeric_limits<std::size_t>::max() - data_.size()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "binary writer size overflow");
    }
    data_.insert(data_.end(), value.begin(), value.end());
    return {};
}

Result<void> BinaryWriter::WriteU8(std::uint8_t value) {
    return Append(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), 1));
}

Result<void> BinaryWriter::WriteU16(std::uint16_t value) {
    std::byte bytes[2] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
    };
    return Append(bytes);
}

Result<void> BinaryWriter::WriteU32(std::uint32_t value) {
    std::byte bytes[4] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
    };
    return Append(bytes);
}

Result<void> BinaryWriter::WriteU64(std::uint64_t value) {
    std::byte bytes[8] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
        static_cast<std::byte>((value >> 32U) & 0xFFU),
        static_cast<std::byte>((value >> 40U) & 0xFFU),
        static_cast<std::byte>((value >> 48U) & 0xFFU),
        static_cast<std::byte>((value >> 56U) & 0xFFU),
    };
    return Append(bytes);
}

Result<void> BinaryWriter::WriteI32(std::int32_t value) {
    return WriteU32(std::bit_cast<std::uint32_t>(value));
}

Result<void> BinaryWriter::WriteI64(std::int64_t value) {
    return WriteU64(std::bit_cast<std::uint64_t>(value));
}

Result<void> BinaryWriter::WriteF32(float value) {
    return WriteU32(std::bit_cast<std::uint32_t>(value));
}

Result<void> BinaryWriter::WriteF64(double value) {
    return WriteU64(std::bit_cast<std::uint64_t>(value));
}

Result<void> BinaryWriter::WriteBool(bool value) {
    return WriteU8(value ? 1U : 0U);
}

Result<void> BinaryWriter::WriteBytes(std::span<const std::byte> value) {
    return Append(value);
}

Result<void> BinaryWriter::WriteString(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "string is too large");
    }
    if (auto result = WriteU32(static_cast<std::uint32_t>(value.size())); !result) {
        return result;
    }
    return Append(std::as_bytes(std::span(value.data(), value.size())));
}

Result<void> BinaryWriter::WriteCString(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "C string contains NUL");
    }
    if (auto result = Append(std::as_bytes(std::span(value.data(), value.size()))); !result) {
        return result;
    }
    return WriteU8(0);
}

Result<void> BinaryWriter::WriteHash256(const core::Hash256& value) {
    return Append(std::as_bytes(std::span(value.bytes.data(), value.bytes.size())));
}

Result<void> BinaryWriter::WriteGuid128(const core::Guid128& value) {
    return Append(std::as_bytes(std::span(value.bytes.data(), value.bytes.size())));
}

Result<void> BinaryWriter::WriteZero(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() - data_.size()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "binary writer size overflow");
    }
    data_.insert(data_.end(), count, std::byte{0});
    return {};
}

Result<void> BinaryWriter::Align(std::size_t alignment, std::byte fill) {
    if (alignment == 0) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "alignment must be non-zero");
    }
    const std::size_t remainder = data_.size() % alignment;
    if (remainder == 0) {
        return {};
    }
    const std::size_t padding = alignment - remainder;
    if (padding > std::numeric_limits<std::size_t>::max() - data_.size()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "binary writer size overflow");
    }
    data_.insert(data_.end(), padding, fill);
    return {};
}

Result<void> BinaryWriter::IsPatchable(std::size_t offset, std::size_t size) const {
    if (offset > data_.size() || size > data_.size() - offset) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "patch range is out of bounds");
    }
    return {};
}

Result<void> BinaryWriter::PatchBytes(std::size_t offset, std::span<const std::byte> value) {
    if (auto result = IsPatchable(offset, value.size()); !result) {
        return result;
    }
    std::copy(value.begin(), value.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset));
    return {};
}

Result<void> BinaryWriter::PatchU8(std::size_t offset, std::uint8_t value) {
    const std::byte bytes[1] = {static_cast<std::byte>(value)};
    return PatchBytes(offset, bytes);
}

Result<void> BinaryWriter::PatchU16(std::size_t offset, std::uint16_t value) {
    const std::byte bytes[2] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
    };
    return PatchBytes(offset, bytes);
}

Result<void> BinaryWriter::PatchU32(std::size_t offset, std::uint32_t value) {
    const std::byte bytes[4] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
    };
    return PatchBytes(offset, bytes);
}

Result<void> BinaryWriter::PatchU64(std::size_t offset, std::uint64_t value) {
    const std::byte bytes[8] = {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
        static_cast<std::byte>((value >> 32U) & 0xFFU),
        static_cast<std::byte>((value >> 40U) & 0xFFU),
        static_cast<std::byte>((value >> 48U) & 0xFFU),
        static_cast<std::byte>((value >> 56U) & 0xFFU),
    };
    return PatchBytes(offset, bytes);
}

Result<void> BinaryWriter::PatchHash256(std::size_t offset, const core::Hash256& value) {
    return PatchBytes(offset, std::as_bytes(std::span(value.bytes.data(), value.bytes.size())));
}

Result<void> BinaryWriter::PatchGuid128(std::size_t offset, const core::Guid128& value) {
    return PatchBytes(offset, std::as_bytes(std::span(value.bytes.data(), value.bytes.size())));
}

std::size_t BinaryWriter::Tell() const noexcept {
    return data_.size();
}

std::size_t BinaryWriter::Size() const noexcept {
    return data_.size();
}

std::span<const std::byte> BinaryWriter::Data() const noexcept {
    return data_;
}

std::vector<std::byte> BinaryWriter::Release() noexcept {
    return std::move(data_);
}

void BinaryWriter::Reserve(std::size_t capacity) {
    data_.reserve(capacity);
}

void BinaryWriter::Clear() noexcept {
    data_.clear();
}

BinaryReader::BinaryReader(std::span<const std::byte> data) noexcept : data_(data) {}

const std::byte* BinaryReader::Require(std::size_t count) {
    if (count > data_.size() - position_) {
        return nullptr;
    }
    return data_.data() + position_;
}

Result<std::uint8_t> BinaryReader::ReadU8() {
    const std::byte* bytes = Require(1);
    if (bytes == nullptr) {
        return core::MakeError(core::ErrorCode::Corrupt, "unexpected end of binary data");
    }
    position_ += 1;
    return std::to_integer<std::uint8_t>(bytes[0]);
}

Result<std::uint16_t> BinaryReader::ReadU16() {
    const std::byte* bytes = Require(2);
    if (bytes == nullptr) {
        return core::MakeError(core::ErrorCode::Corrupt, "unexpected end of binary data");
    }
    position_ += 2;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[0]) |
        (std::to_integer<std::uint16_t>(bytes[1]) << 8U));
}

Result<std::uint32_t> BinaryReader::ReadU32() {
    const std::byte* bytes = Require(4);
    if (bytes == nullptr) {
        return core::MakeError(core::ErrorCode::Corrupt, "unexpected end of binary data");
    }
    position_ += 4;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes[i]) << (8U * i);
    }
    return value;
}

Result<std::uint64_t> BinaryReader::ReadU64() {
    const std::byte* bytes = Require(8);
    if (bytes == nullptr) {
        return core::MakeError(core::ErrorCode::Corrupt, "unexpected end of binary data");
    }
    position_ += 8;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= std::to_integer<std::uint64_t>(bytes[i]) << (8U * i);
    }
    return value;
}

Result<std::int32_t> BinaryReader::ReadI32() {
    auto value = ReadU32();
    if (!value) {
        return std::move(value).TakeError();
    }
    return std::bit_cast<std::int32_t>(value.Value());
}

Result<std::int64_t> BinaryReader::ReadI64() {
    auto value = ReadU64();
    if (!value) {
        return std::move(value).TakeError();
    }
    return std::bit_cast<std::int64_t>(value.Value());
}

Result<float> BinaryReader::ReadF32() {
    auto value = ReadU32();
    if (!value) {
        return std::move(value).TakeError();
    }
    return std::bit_cast<float>(value.Value());
}

Result<double> BinaryReader::ReadF64() {
    auto value = ReadU64();
    if (!value) {
        return std::move(value).TakeError();
    }
    return std::bit_cast<double>(value.Value());
}

Result<bool> BinaryReader::ReadBool() {
    auto value = ReadU8();
    if (!value) {
        return std::move(value).TakeError();
    }
    if (value.Value() > 1U) {
        return core::MakeError(core::ErrorCode::Corrupt, "invalid boolean value");
    }
    return value.Value() != 0;
}

Result<std::span<const std::byte>> BinaryReader::ReadBytes(std::size_t count) {
    const std::byte* bytes = Require(count);
    if (bytes == nullptr) {
        return core::MakeError(core::ErrorCode::Corrupt, "unexpected end of binary data");
    }
    position_ += count;
    return std::span<const std::byte>(bytes, count);
}

Result<std::string_view> BinaryReader::ReadString(std::size_t max_bytes) {
    auto length = ReadU32();
    if (!length) {
        return std::move(length).TakeError();
    }
    const std::size_t size = length.Value();
    if (size > max_bytes) {
        return core::MakeError(core::ErrorCode::Corrupt, "string exceeds maximum length");
    }
    auto bytes = ReadBytes(size);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return std::string_view(reinterpret_cast<const char*>(bytes.Value().data()), size);
}

Result<std::string_view> BinaryReader::ReadCString(std::size_t max_bytes) {
    const std::size_t available = std::min(max_bytes, Remaining());
    const std::size_t start = position_;
    for (std::size_t i = 0; i < available; ++i) {
        if (data_[start + i] == std::byte{0}) {
            position_ = start + i + 1;
            return std::string_view(reinterpret_cast<const char*>(data_.data() + start), i);
        }
    }
    return core::MakeError(core::ErrorCode::Corrupt, "unterminated C string");
}

Result<core::Hash256> BinaryReader::ReadHash256() {
    auto bytes = ReadBytes(32);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    core::Hash256 hash;
    for (std::size_t i = 0; i < hash.bytes.size(); ++i) {
        hash.bytes[i] = std::to_integer<std::uint8_t>(bytes.Value()[i]);
    }
    return hash;
}

Result<core::Guid128> BinaryReader::ReadGuid128() {
    auto bytes = ReadBytes(16);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return core::Guid128::FromBytes(
        std::span<const std::byte, 16>(bytes.Value().data(), 16));
}

Result<void> BinaryReader::Skip(std::size_t count) {
    auto bytes = ReadBytes(count);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return {};
}

Result<BinaryReader> BinaryReader::ReadSubReader(std::size_t count) {
    auto bytes = ReadBytes(count);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return BinaryReader(bytes.Value());
}

std::size_t BinaryReader::Position() const noexcept {
    return position_;
}

std::size_t BinaryReader::Remaining() const noexcept {
    return data_.size() - position_;
}

bool BinaryReader::IsAtEnd() const noexcept {
    return position_ == data_.size();
}

std::span<const std::byte> BinaryReader::Data() const noexcept {
    return data_;
}

} // namespace parcel::serialize
