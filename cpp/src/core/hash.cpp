#include "parcel/core/hash.hpp"

#include "parcel/core/platform_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace parcel::core {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

[[nodiscard]] constexpr std::uint32_t Rotr(std::uint32_t value, unsigned shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

class Sha256Context {
  public:
    Sha256Context() : state_(kSha256InitialState) {}

    void Update(const std::uint8_t* data, std::size_t size) noexcept {
        total_bytes_ += static_cast<std::uint64_t>(size);

        if (buffer_size_ != 0) {
            const std::size_t to_copy = std::min(size, buffer_.size() - buffer_size_);
            std::memcpy(buffer_.data() + buffer_size_, data, to_copy);
            buffer_size_ += to_copy;
            data += to_copy;
            size -= to_copy;

            if (buffer_size_ == buffer_.size()) {
                Transform(buffer_.data());
                buffer_size_ = 0;
            }
        }

        while (size >= buffer_.size()) {
            Transform(data);
            data += buffer_.size();
            size -= buffer_.size();
        }

        if (size != 0) {
            std::memcpy(buffer_.data(), data, size);
            buffer_size_ = size;
        }
    }

    [[nodiscard]] Hash256 Finalize() noexcept {
        const std::uint64_t bit_length = total_bytes_ * 8U;

        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.end(),
                      std::uint8_t{0});
            Transform(buffer_.data());
            buffer_size_ = 0;
        }

        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56,
                  std::uint8_t{0});

        for (std::size_t i = 0; i < 8; ++i) {
            buffer_[63 - i] = static_cast<std::uint8_t>(bit_length >> (i * 8U));
        }
        Transform(buffer_.data());

        Hash256 result;
        for (std::size_t i = 0; i < state_.size(); ++i) {
            result.bytes[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24U);
            result.bytes[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
            result.bytes[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
            result.bytes[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return result;
    }

  private:
    void Transform(const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16; ++i) {
            schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                          (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                          (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                          static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                Rotr(schedule[i - 15], 7) ^ Rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3U);
            const std::uint32_t s1 =
                Rotr(schedule[i - 2], 17) ^ Rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10U);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + kSha256Constants[i] + schedule[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] constexpr std::array<std::uint32_t, 256> MakeCrc32Table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
        }
        table[i] = value;
    }
    return table;
}

constexpr auto kCrc32Table = MakeCrc32Table();

} // namespace

bool Hash256::IsZero() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0; });
}

std::string Hash256::ToHex() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = kHex[bytes[i] >> 4U];
        result[i * 2 + 1] = kHex[bytes[i] & 0x0fU];
    }
    return result;
}

Result<Hash256> Hash256::FromHex(std::string_view hex) {
    if (hex.size() != 64) {
        return MakeError(ErrorCode::InvalidArgument, "SHA-256 hex string must be 64 characters");
    }

    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };

    Hash256 result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        const int high = nibble(hex[i * 2]);
        const int low = nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return MakeError(ErrorCode::InvalidArgument,
                             "SHA-256 hex string contains invalid data");
        }
        result.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

std::uint32_t Crc32(std::span<const std::byte> data, std::uint32_t seed) noexcept {
    std::uint32_t crc = seed ^ 0xffffffffU;
    for (const std::byte value : data) {
        const auto index = static_cast<std::uint8_t>(crc ^ std::to_integer<std::uint8_t>(value));
        crc = (crc >> 8U) ^ kCrc32Table[index];
    }
    return crc ^ 0xffffffffU;
}

std::uint32_t Crc32(std::string_view data, std::uint32_t seed) noexcept {
    return Crc32(std::as_bytes(std::span(data.data(), data.size())), seed);
}

Hash256 Sha256(std::span<const std::byte> data) noexcept {
    Sha256Context context;
    context.Update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return context.Finalize();
}

Hash256 Sha256(std::string_view data) noexcept {
    return Sha256(std::as_bytes(std::span(data.data(), data.size())));
}

Result<Hash256> Sha256File(std::string_view path) {
    PlatformFile filesystem;
    auto file_result = filesystem.OpenRead(path);
    if (!file_result) {
        return std::move(file_result).TakeError();
    }

    auto& file = *file_result.Value();
    Sha256Context context;
    std::array<std::byte, 64 * 1024> buffer{};

    for (;;) {
        auto read_result = file.Read(buffer);
        if (!read_result) {
            return std::move(read_result).TakeError();
        }
        const std::size_t read = read_result.Value();
        if (read == 0) {
            break;
        }
        context.Update(reinterpret_cast<const std::uint8_t*>(buffer.data()), read);
    }

    return context.Finalize();
}

} // namespace parcel::core
