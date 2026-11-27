#include "texture_decode.hpp"

#include "parcel/core/error.hpp"
#include "parcel/import/meta.hpp"
#include "texture_encoder.hpp"

#include <algorithm>
#include <cmath>
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

constexpr float kMaxBakeCoordinate = 1.0e7f;

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

std::int64_t WrapTexel(std::int64_t index, int size, assets::TextureWrap wrap) noexcept {
    const std::int64_t dimension = static_cast<std::int64_t>(size);
    if (dimension <= 1) {
        return 0;
    }
    switch (wrap) {
    case assets::TextureWrap::Repeat: {
        std::int64_t wrapped = index % dimension;
        if (wrapped < 0) {
            wrapped += dimension;
        }
        return wrapped;
    }
    case assets::TextureWrap::Mirror: {
        const std::int64_t period = dimension * 2;
        std::int64_t wrapped = index % period;
        if (wrapped < 0) {
            wrapped += period;
        }
        return wrapped < dimension ? wrapped : period - 1 - wrapped;
    }
    case assets::TextureWrap::Clamp:
    default:
        return std::clamp(index, std::int64_t{0}, dimension - 1);
    }
}

void BakeBytePixels(const stbi_uc* source, int width, int height, const TextureBakeOptions& bake,
                    stbi_uc* output) {
    const std::size_t source_pitch = static_cast<std::size_t>(width) * 4;
    const std::size_t output_pitch = source_pitch;
    const auto fetch = [&](std::int64_t x, std::int64_t y, std::size_t channel) -> float {
        const std::int64_t source_x = WrapTexel(x, width, bake.wrap_s);
        const std::int64_t source_y = WrapTexel(y, height, bake.wrap_t);
        const std::size_t offset = static_cast<std::size_t>(source_y) * source_pitch +
                                   static_cast<std::size_t>(source_x) * 4 + channel;
        return static_cast<float>(source[offset]);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec2 uv{(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(height)};
            const Vec2 transformed = bake.transform.Apply(uv);
            const float source_x = transformed.x * static_cast<float>(width) - 0.5f;
            const float source_y = transformed.y * static_cast<float>(height) - 0.5f;
            const std::size_t output_offset =
                static_cast<std::size_t>(y) * output_pitch + static_cast<std::size_t>(x) * 4;
            if (!std::isfinite(source_x) || !std::isfinite(source_y) ||
                std::abs(source_x) > kMaxBakeCoordinate ||
                std::abs(source_y) > kMaxBakeCoordinate) {
                std::memset(output + output_offset, 0, 4);
                continue;
            }

            if (!bake.bilinear) {
                const auto source_x_index = static_cast<std::int64_t>(std::floor(source_x + 0.5f));
                const auto source_y_index = static_cast<std::int64_t>(std::floor(source_y + 0.5f));
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const float value = fetch(source_x_index, source_y_index, channel);
                    output[output_offset + channel] =
                        static_cast<stbi_uc>(std::clamp(std::lround(value), 0L, 255L));
                }
                continue;
            }

            const auto source_x_index = static_cast<std::int64_t>(std::floor(source_x));
            const auto source_y_index = static_cast<std::int64_t>(std::floor(source_y));
            const float fraction_x = source_x - std::floor(source_x);
            const float fraction_y = source_y - std::floor(source_y);
            const float bottom_weight = 1.0f - fraction_y;
            const float right_weight = 1.0f - fraction_x;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const float bottom_left = fetch(source_x_index, source_y_index, channel);
                const float bottom_right = fetch(source_x_index + 1, source_y_index, channel);
                const float top_left = fetch(source_x_index, source_y_index + 1, channel);
                const float top_right = fetch(source_x_index + 1, source_y_index + 1, channel);
                const float value = bottom_left * right_weight * bottom_weight +
                                    bottom_right * fraction_x * bottom_weight +
                                    top_left * right_weight * fraction_y +
                                    top_right * fraction_x * fraction_y;
                output[output_offset + channel] =
                    static_cast<stbi_uc>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
}

void BakeFloatPixels(const float* source, int width, int height, const TextureBakeOptions& bake,
                     float* output) {
    const std::size_t source_pitch = static_cast<std::size_t>(width) * 4;
    const std::size_t output_pitch = source_pitch;
    const auto fetch = [&](std::int64_t x, std::int64_t y, std::size_t channel) -> float {
        const std::int64_t source_x = WrapTexel(x, width, bake.wrap_s);
        const std::int64_t source_y = WrapTexel(y, height, bake.wrap_t);
        const std::size_t offset = static_cast<std::size_t>(source_y) * source_pitch +
                                   static_cast<std::size_t>(source_x) * 4 + channel;
        return source[offset];
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec2 uv{(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(height)};
            const Vec2 transformed = bake.transform.Apply(uv);
            const float source_x = transformed.x * static_cast<float>(width) - 0.5f;
            const float source_y = transformed.y * static_cast<float>(height) - 0.5f;
            const std::size_t output_offset =
                static_cast<std::size_t>(y) * output_pitch + static_cast<std::size_t>(x) * 4;
            if (!std::isfinite(source_x) || !std::isfinite(source_y) ||
                std::abs(source_x) > kMaxBakeCoordinate ||
                std::abs(source_y) > kMaxBakeCoordinate) {
                std::memset(output + output_offset, 0, 4 * sizeof(float));
                continue;
            }

            if (!bake.bilinear) {
                const auto source_x_index = static_cast<std::int64_t>(std::floor(source_x + 0.5f));
                const auto source_y_index = static_cast<std::int64_t>(std::floor(source_y + 0.5f));
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    output[output_offset + channel] =
                        fetch(source_x_index, source_y_index, channel);
                }
                continue;
            }

            const auto source_x_index = static_cast<std::int64_t>(std::floor(source_x));
            const auto source_y_index = static_cast<std::int64_t>(std::floor(source_y));
            const float fraction_x = source_x - std::floor(source_x);
            const float fraction_y = source_y - std::floor(source_y);
            const float bottom_weight = 1.0f - fraction_y;
            const float right_weight = 1.0f - fraction_x;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const float bottom_left = fetch(source_x_index, source_y_index, channel);
                const float bottom_right = fetch(source_x_index + 1, source_y_index, channel);
                const float top_left = fetch(source_x_index, source_y_index + 1, channel);
                const float top_right = fetch(source_x_index + 1, source_y_index + 1, channel);
                output[output_offset + channel] = bottom_left * right_weight * bottom_weight +
                                                  bottom_right * fraction_x * bottom_weight +
                                                  top_left * right_weight * fraction_y +
                                                  top_right * fraction_x * fraction_y;
            }
        }
    }
}

} // namespace

core::Result<std::shared_ptr<assets::TextureAsset>>
DecodeTextureAsset(std::span<const std::byte> bytes, const assets::JsonValue& settings,
                   std::string_view source_label, const TextureBakeOptions* bake) {
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

    const float* final_float_pixels = float_pixels;
    const stbi_uc* final_byte_pixels = byte_pixels;
    std::vector<float> baked_float_pixels;
    std::vector<stbi_uc> baked_byte_pixels;
    if (bake != nullptr && !bake->transform.IsIdentity()) {
        const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
        if (hdr) {
            baked_float_pixels.resize(pixel_count * 4);
            BakeFloatPixels(float_pixels, width, height, *bake, baked_float_pixels.data());
            final_float_pixels = baked_float_pixels.data();
        } else {
            baked_byte_pixels.resize(pixel_count * 4);
            BakeBytePixels(byte_pixels, width, height, *bake, baked_byte_pixels.data());
            final_byte_pixels = baked_byte_pixels.data();
        }
    }

    std::vector<assets::TextureMip> mips;
    const bool built = hdr ? BuildFloatMips(mips, final_float_pixels, width, height, generate_mips)
                           : BuildByteMips(mips, final_byte_pixels, width, height, generate_mips);
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
