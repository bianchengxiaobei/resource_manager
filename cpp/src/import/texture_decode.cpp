#include "texture_decode.hpp"

#include "parcel/core/error.hpp"
#include "parcel/import/meta.hpp"
#include "texture_encoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "stb_image.h"

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

bool DimensionsValid(int width, int height) {
    constexpr int kMaxDimension = 16384;
    constexpr std::int64_t kMaxPixels = 64LL * 1000LL * 1000LL;
    return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension &&
           static_cast<std::int64_t>(width) * height <= kMaxPixels;
}

bool BuildByteMips(std::vector<assets::TextureMip>& mips, const stbi_uc* pixels, int width,
                   int height, bool generate_mips) {
    const std::size_t row_pitch = static_cast<std::size_t>(width) * 4;
    assets::TextureMip base;
    base.width = static_cast<std::uint32_t>(width);
    base.height = static_cast<std::uint32_t>(height);
    base.row_pitch = static_cast<std::uint32_t>(row_pitch);
    base.data.resize(row_pitch * static_cast<std::size_t>(height));
    std::memcpy(base.data.data(), pixels, base.data.size());
    mips.push_back(std::move(base));
    if (!generate_mips) {
        return true;
    }

    while (width > 1 || height > 1) {
        const int next_width = std::max(1, width / 2);
        const int next_height = std::max(1, height / 2);
        const assets::TextureMip& previous = mips.back();
        assets::TextureMip next;
        next.width = static_cast<std::uint32_t>(next_width);
        next.height = static_cast<std::uint32_t>(next_height);
        next.row_pitch = static_cast<std::uint32_t>(next_width * 4);
        next.data.resize(static_cast<std::size_t>(next_width) * next_height * 4);
        for (int y = 0; y < next_height; ++y) {
            for (int x = 0; x < next_width; ++x) {
                std::uint32_t sum[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(width - 1, x * 2 + dx);
                        const int sy = std::min(height - 1, y * 2 + dy);
                        const std::size_t offset =
                            static_cast<std::size_t>(sy) * previous.row_pitch +
                            static_cast<std::size_t>(sx) * 4;
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += std::to_integer<std::uint8_t>(previous.data[offset + c]);
                        }
                    }
                }
                const std::size_t offset =
                    static_cast<std::size_t>(y) * next.row_pitch + static_cast<std::size_t>(x) * 4;
                for (int c = 0; c < 4; ++c) {
                    next.data[offset + c] = static_cast<std::byte>((sum[c] + 2U) / 4U);
                }
            }
        }
        mips.push_back(std::move(next));
        width = next_width;
        height = next_height;
    }
    return true;
}

bool BuildFloatMips(std::vector<assets::TextureMip>& mips, const float* pixels, int width,
                    int height, bool generate_mips) {
    const std::size_t row_pitch = static_cast<std::size_t>(width) * 4 * sizeof(float);
    assets::TextureMip base;
    base.width = static_cast<std::uint32_t>(width);
    base.height = static_cast<std::uint32_t>(height);
    base.row_pitch = static_cast<std::uint32_t>(row_pitch);
    base.data.resize(row_pitch * static_cast<std::size_t>(height));
    std::memcpy(base.data.data(), pixels, base.data.size());
    mips.push_back(std::move(base));
    if (!generate_mips) {
        return true;
    }

    while (width > 1 || height > 1) {
        const int next_width = std::max(1, width / 2);
        const int next_height = std::max(1, height / 2);
        const assets::TextureMip& previous = mips.back();
        assets::TextureMip next;
        next.width = static_cast<std::uint32_t>(next_width);
        next.height = static_cast<std::uint32_t>(next_height);
        next.row_pitch = static_cast<std::uint32_t>(next_width * 4 * sizeof(float));
        next.data.resize(static_cast<std::size_t>(next_width) * next_height * 4 * sizeof(float));
        for (int y = 0; y < next_height; ++y) {
            for (int x = 0; x < next_width; ++x) {
                float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(width - 1, x * 2 + dx);
                        const int sy = std::min(height - 1, y * 2 + dy);
                        const std::size_t offset =
                            static_cast<std::size_t>(sy) * previous.row_pitch +
                            static_cast<std::size_t>(sx) * 4 * sizeof(float);
                        for (int c = 0; c < 4; ++c) {
                            float value = 0.0f;
                            std::memcpy(&value, previous.data.data() + offset + c * sizeof(float),
                                        sizeof(float));
                            sum[c] += value;
                        }
                    }
                }
                const std::size_t offset = static_cast<std::size_t>(y) * next.row_pitch +
                                           static_cast<std::size_t>(x) * 4 * sizeof(float);
                for (int c = 0; c < 4; ++c) {
                    const float value = sum[c] * 0.25f;
                    std::memcpy(next.data.data() + offset + c * sizeof(float), &value,
                                sizeof(float));
                }
            }
        }
        mips.push_back(std::move(next));
        width = next_width;
        height = next_height;
    }
    return true;
}

} // namespace

core::Result<std::shared_ptr<assets::TextureAsset>>
DecodeTextureAsset(std::span<const std::byte> bytes, const assets::JsonValue& settings,
                   std::string_view source_label) {
    const auto fail = [&](ErrorCode code, std::string message) {
        core::Error error = MakeError(code, std::move(message));
        if (!source_label.empty()) {
            error.WithPath(std::string(source_label));
        }
        return error;
    };
    if (bytes.empty()) {
        return fail(ErrorCode::InvalidArgument, "texture source is empty");
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail(ErrorCode::InvalidArgument, "texture source is too large");
    }

    const bool generate_mips = SettingBool(settings, "generateMips", true);
    const bool srgb = SettingBool(settings, "sRGB", true);
    auto encoding_options = detail::ParseTextureEncodingOptions(settings);
    if (!encoding_options) {
        core::Error error = std::move(encoding_options).TakeError();
        if (!source_label.empty()) {
            error.WithPath(std::string(source_label));
        }
        return error;
    }

    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int size = static_cast<int>(bytes.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    bool hdr = false;
    float* float_pixels = nullptr;
    stbi_uc* byte_pixels = nullptr;
    {
        static std::mutex stbi_mutex;
        const std::lock_guard lock(stbi_mutex);
        hdr = stbi_is_hdr_from_memory(data, size) != 0;
        if (hdr) {
            float_pixels = stbi_loadf_from_memory(data, size, &width, &height, &channels, 4);
        } else {
            byte_pixels = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
        }
    }

    if ((hdr ? static_cast<void*>(float_pixels) : static_cast<void*>(byte_pixels)) == nullptr ||
        !DimensionsValid(width, height)) {
        stbi_image_free(float_pixels);
        stbi_image_free(byte_pixels);
        return fail(ErrorCode::Corrupt,
                    hdr ? "failed to decode HDR texture" : "failed to decode texture");
    }

    std::vector<assets::TextureMip> mips;
    const bool built = hdr ? BuildFloatMips(mips, float_pixels, width, height, generate_mips)
                           : BuildByteMips(mips, byte_pixels, width, height, generate_mips);
    stbi_image_free(float_pixels);
    stbi_image_free(byte_pixels);
    if (!built) {
        return fail(ErrorCode::Internal, "failed to build texture mips");
    }

    const bool has_alpha = !hdr && encoding_options.Value().compression == "auto" &&
                           detail::TextureHasTransparency(mips.front());
    auto format = detail::ResolveTextureFormat(encoding_options.Value(), hdr, has_alpha);
    if (!format) {
        core::Error error = std::move(format).TakeError();
        if (!source_label.empty()) {
            error.WithPath(std::string(source_label));
        }
        return error;
    }
    auto encoded = detail::EncodeTextureMips(mips, hdr, format.Value(),
                                             encoding_options.Value().quality, srgb);
    if (!encoded) {
        core::Error error = std::move(encoded).TakeError();
        if (!source_label.empty()) {
            error.WithPath(std::string(source_label));
        }
        return error;
    }

    auto asset = std::make_shared<assets::TextureAsset>();
    asset->format = format.Value();
    asset->sRGB = hdr ? false : srgb;
    asset->mips = std::move(mips);
    asset->mip_count = static_cast<std::uint32_t>(asset->mips.size());
    return asset;
}

} // namespace parcel::import
