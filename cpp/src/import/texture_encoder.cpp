#include "texture_encoder.hpp"

#include "parcel/core/error.hpp"
#include "parcel/import/meta.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

#include "bc7enc.h"
#include "stb_dxt.h"

namespace parcel::import::detail {
namespace {

using core::ErrorCode;
using core::MakeError;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsCompressed(assets::TextureFormat format) {
    return format == assets::TextureFormat::BC1 || format == assets::TextureFormat::BC3 ||
           format == assets::TextureFormat::BC5 || format == assets::TextureFormat::BC7;
}

std::size_t BytesPerPixel(assets::TextureFormat format) {
    switch (format) {
    case assets::TextureFormat::RGBA8:
    case assets::TextureFormat::BGRA8:
        return 4;
    case assets::TextureFormat::RGBA16F:
        return 8;
    case assets::TextureFormat::RGBA32F:
        return 16;
    default:
        return 0;
    }
}

std::size_t BlockBytes(assets::TextureFormat format) {
    switch (format) {
    case assets::TextureFormat::BC1:
        return 8;
    case assets::TextureFormat::BC3:
    case assets::TextureFormat::BC5:
    case assets::TextureFormat::BC7:
        return 16;
    default:
        return 0;
    }
}

std::uint16_t FloatToHalf(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t magnitude = bits & 0x7fffffffU;
    if (magnitude >= 0x7f800000U) {
        const std::uint16_t payload = (magnitude & 0x007fffffU) != 0 ? 0x0200U : 0U;
        return static_cast<std::uint16_t>(sign | 0x7c00U | payload);
    }
    if (magnitude >= 0x47800000U) {
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    if (magnitude < 0x33000000U) {
        return sign;
    }

    int exponent = static_cast<int>(magnitude >> 23U) - 112;
    std::uint32_t mantissa = magnitude & 0x007fffffU;
    if (exponent <= 0) {
        const int shift = 1 - exponent;
        if (shift >= 24) {
            return sign;
        }
        mantissa = (mantissa | 0x00800000U) >> static_cast<unsigned>(shift);
        mantissa = (mantissa + 0x1000U) >> 13U;
        return static_cast<std::uint16_t>(sign | mantissa);
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7c00U);
    }
    mantissa = (mantissa + 0x1000U) >> 13U;
    if (mantissa == 0x400U) {
        mantissa = 0;
        ++exponent;
        if (exponent >= 31) {
            return static_cast<std::uint16_t>(sign | 0x7c00U);
        }
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(exponent) << 10U) |
                                      mantissa);
}

void StoreHalf(std::byte* output, float value) {
    const std::uint16_t half = FloatToHalf(value);
    output[0] = static_cast<std::byte>(half & 0xffU);
    output[1] = static_cast<std::byte>((half >> 8U) & 0xffU);
}

void StoreFloat(std::byte* output, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    output[0] = static_cast<std::byte>(bits & 0xffU);
    output[1] = static_cast<std::byte>((bits >> 8U) & 0xffU);
    output[2] = static_cast<std::byte>((bits >> 16U) & 0xffU);
    output[3] = static_cast<std::byte>((bits >> 24U) & 0xffU);
}

float LoadFloat(const std::byte* input) {
    const std::uint32_t bits = std::to_integer<std::uint32_t>(input[0]) |
                               (std::to_integer<std::uint32_t>(input[1]) << 8U) |
                               (std::to_integer<std::uint32_t>(input[2]) << 16U) |
                               (std::to_integer<std::uint32_t>(input[3]) << 24U);
    return std::bit_cast<float>(bits);
}

core::Result<void> ConvertByteMip(const assets::TextureMip& source, assets::TextureFormat format,
                                  assets::TextureMip& destination) {
    if (format == assets::TextureFormat::RGBA8 || format == assets::TextureFormat::BGRA8) {
        destination.width = source.width;
        destination.height = source.height;
        destination.row_pitch = source.width * 4U;
        destination.data.resize(static_cast<std::size_t>(source.height) * destination.row_pitch);
        for (std::uint32_t y = 0; y < source.height; ++y) {
            const std::byte* row =
                source.data.data() + static_cast<std::size_t>(y) * source.row_pitch;
            std::byte* output =
                destination.data.data() + static_cast<std::size_t>(y) * destination.row_pitch;
            for (std::uint32_t x = 0; x < source.width; ++x) {
                const std::byte* pixel = row + static_cast<std::size_t>(x) * 4U;
                std::byte* target = output + static_cast<std::size_t>(x) * 4U;
                if (format == assets::TextureFormat::BGRA8) {
                    target[0] = pixel[2];
                    target[1] = pixel[1];
                    target[2] = pixel[0];
                } else {
                    target[0] = pixel[0];
                    target[1] = pixel[1];
                    target[2] = pixel[2];
                }
                target[3] = pixel[3];
            }
        }
        return {};
    }

    destination.width = source.width;
    destination.height = source.height;
    destination.row_pitch = source.width * static_cast<std::uint32_t>(BytesPerPixel(format));
    destination.data.resize(static_cast<std::size_t>(source.height) * destination.row_pitch);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const std::byte* row = source.data.data() + static_cast<std::size_t>(y) * source.row_pitch;
        std::byte* output =
            destination.data.data() + static_cast<std::size_t>(y) * destination.row_pitch;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const std::byte* pixel = row + static_cast<std::size_t>(x) * 4U;
            std::byte* target = output + static_cast<std::size_t>(x) * BytesPerPixel(format);
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const float value =
                    static_cast<float>(std::to_integer<std::uint8_t>(pixel[channel])) / 255.0F;
                if (format == assets::TextureFormat::RGBA16F) {
                    StoreHalf(target + channel * 2U, value);
                } else {
                    StoreFloat(target + channel * 4U, value);
                }
            }
        }
    }
    return {};
}

core::Result<void> ConvertFloatMip(const assets::TextureMip& source, assets::TextureFormat format,
                                   assets::TextureMip& destination) {
    destination.width = source.width;
    destination.height = source.height;
    destination.row_pitch = source.width * static_cast<std::uint32_t>(BytesPerPixel(format));
    destination.data.resize(static_cast<std::size_t>(source.height) * destination.row_pitch);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const std::byte* row = source.data.data() + static_cast<std::size_t>(y) * source.row_pitch;
        std::byte* output =
            destination.data.data() + static_cast<std::size_t>(y) * destination.row_pitch;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const std::byte* pixel = row + static_cast<std::size_t>(x) * 16U;
            std::byte* target = output + static_cast<std::size_t>(x) * BytesPerPixel(format);
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const float value = LoadFloat(pixel + channel * 4U);
                if (format == assets::TextureFormat::RGBA16F) {
                    StoreHalf(target + channel * 2U, value);
                } else {
                    StoreFloat(target + channel * 4U, value);
                }
            }
        }
    }
    return {};
}

void EnsureBc7Initialized() {
    static std::once_flag once;
    std::call_once(once, bc7enc_compress_block_init);
}

int DxtMode(TextureQuality quality) {
    return quality == TextureQuality::Fast ? STB_DXT_NORMAL : STB_DXT_HIGHQUAL;
}

bc7enc_compress_block_params Bc7Params(TextureQuality quality, bool srgb) {
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (srgb) {
        bc7enc_compress_block_params_init_perceptual_weights(&params);
    } else {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    if (quality == TextureQuality::Fast) {
        params.m_uber_level = 0;
        params.m_try_least_squares = BC7ENC_FALSE;
        params.m_max_partitions_mode = 0;
    } else if (quality == TextureQuality::Best) {
        params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
    }
    return params;
}

core::Result<void> EncodeCompressedMip(const assets::TextureMip& source,
                                       assets::TextureFormat format, TextureQuality quality,
                                       bool srgb, assets::TextureMip& destination) {
    const std::size_t block_bytes = BlockBytes(format);
    const std::uint32_t blocks_x = (source.width + 3U) / 4U;
    const std::uint32_t blocks_y = (source.height + 3U) / 4U;
    destination.width = source.width;
    destination.height = source.height;
    destination.row_pitch = blocks_x * static_cast<std::uint32_t>(block_bytes);
    destination.data.assign(static_cast<std::size_t>(blocks_y) * destination.row_pitch,
                            std::byte{0});

    if (format == assets::TextureFormat::BC7) {
        EnsureBc7Initialized();
    }
    const bc7enc_compress_block_params bc7_params = format == assets::TextureFormat::BC7
                                                        ? Bc7Params(quality, srgb)
                                                        : bc7enc_compress_block_params{};

    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            std::array<std::uint8_t, 64> rgba{};
            std::array<std::uint8_t, 16> red{};
            std::array<std::uint8_t, 16> green{};
            for (std::uint32_t py = 0; py < 4U; ++py) {
                for (std::uint32_t px = 0; px < 4U; ++px) {
                    const std::uint32_t sx = std::min(source.width - 1U, bx * 4U + px);
                    const std::uint32_t sy = std::min(source.height - 1U, by * 4U + py);
                    const std::byte* pixel = source.data.data() +
                                             static_cast<std::size_t>(sy) * source.row_pitch +
                                             static_cast<std::size_t>(sx) * 4U;
                    const std::size_t index = static_cast<std::size_t>(py) * 4U + px;
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        rgba[index * 4U + channel] = std::to_integer<std::uint8_t>(pixel[channel]);
                    }
                    red[index] = std::to_integer<std::uint8_t>(pixel[0]);
                    green[index] = std::to_integer<std::uint8_t>(pixel[1]);
                }
            }

            const std::size_t offset = (static_cast<std::size_t>(by) * destination.row_pitch) +
                                       static_cast<std::size_t>(bx) * block_bytes;
            auto* output = reinterpret_cast<unsigned char*>(destination.data.data() + offset);
            switch (format) {
            case assets::TextureFormat::BC1:
                stb_compress_dxt_block(output, rgba.data(), 0, DxtMode(quality));
                break;
            case assets::TextureFormat::BC3:
                stb_compress_dxt_block(output, rgba.data(), 1, DxtMode(quality));
                break;
            case assets::TextureFormat::BC5:
                stb_compress_bc4_block(output, red.data());
                stb_compress_bc4_block(output + 8U, green.data());
                break;
            case assets::TextureFormat::BC7:
                bc7enc_compress_block(output, rgba.data(), &bc7_params);
                break;
            default:
                return MakeError(ErrorCode::Unsupported, "texture format is not block compressed");
            }
        }
    }
    return {};
}

core::Result<void> ValidateSource(const assets::TextureMip& mip, bool source_is_float) {
    const std::size_t bytes_per_pixel = source_is_float ? 16U : 4U;
    const std::size_t required_row = static_cast<std::size_t>(mip.width) * bytes_per_pixel;
    if (mip.width == 0 || mip.height == 0 || mip.row_pitch < required_row) {
        return MakeError(ErrorCode::Internal, "texture mip layout is invalid");
    }
    const std::size_t required_size = static_cast<std::size_t>(mip.height) * mip.row_pitch;
    if (mip.data.size() < required_size) {
        return MakeError(ErrorCode::Internal, "texture mip data is truncated");
    }
    return {};
}

} // namespace

core::Result<TextureEncodingOptions>
ParseTextureEncodingOptions(const assets::JsonValue& settings) {
    TextureEncodingOptions options;
    options.format = Lower(SettingString(settings, "format", "auto"));
    options.compression = Lower(SettingString(settings, "compression", "none"));
    const std::string quality = Lower(SettingString(settings, "compressionQuality", "balanced"));

    const bool format_known = options.format == "auto" || options.format == "rgba8" ||
                              options.format == "bgra8" || options.format == "rgba16f" ||
                              options.format == "rgba32f";
    if (!format_known) {
        return MakeError(ErrorCode::Unsupported, "texture format is not supported");
    }
    const bool compression_known = options.compression == "none" || options.compression == "auto" ||
                                   options.compression == "bc1" || options.compression == "bc3" ||
                                   options.compression == "bc5" || options.compression == "bc7";
    if (!compression_known) {
        return MakeError(ErrorCode::Unsupported, "texture compression is not supported");
    }
    if (quality == "fast") {
        options.quality = TextureQuality::Fast;
    } else if (quality == "balanced") {
        options.quality = TextureQuality::Balanced;
    } else if (quality == "best") {
        options.quality = TextureQuality::Best;
    } else {
        return MakeError(ErrorCode::Unsupported, "texture compression quality is not supported");
    }
    if (options.compression != "none" && options.format != "auto") {
        return MakeError(ErrorCode::InvalidArgument,
                         "texture format cannot be combined with compression");
    }
    return options;
}

core::Result<assets::TextureFormat> ResolveTextureFormat(const TextureEncodingOptions& options,
                                                         bool hdr, bool has_alpha) {
    if (options.compression == "none") {
        if (hdr) {
            if (options.format == "auto" || options.format == "rgba32f") {
                return assets::TextureFormat::RGBA32F;
            }
            if (options.format == "rgba16f") {
                return assets::TextureFormat::RGBA16F;
            }
            return MakeError(ErrorCode::Unsupported, "HDR textures require RGBA16F or RGBA32F");
        }
        if (options.format == "auto") {
            return assets::TextureFormat::RGBA8;
        }
        if (options.format == "bgra8") {
            return assets::TextureFormat::BGRA8;
        }
        if (options.format == "rgba16f") {
            return assets::TextureFormat::RGBA16F;
        }
        if (options.format == "rgba32f") {
            return assets::TextureFormat::RGBA32F;
        }
        return assets::TextureFormat::RGBA8;
    }

    if (hdr) {
        return MakeError(ErrorCode::Unsupported, "HDR textures cannot use block compression");
    }
    if (options.compression == "auto") {
        return has_alpha ? assets::TextureFormat::BC3 : assets::TextureFormat::BC1;
    }
    if (options.compression == "bc1") {
        return assets::TextureFormat::BC1;
    }
    if (options.compression == "bc3") {
        return assets::TextureFormat::BC3;
    }
    if (options.compression == "bc5") {
        return assets::TextureFormat::BC5;
    }
    return assets::TextureFormat::BC7;
}

bool TextureHasTransparency(const assets::TextureMip& mip) {
    if (mip.row_pitch < mip.width * 4U ||
        mip.data.size() < static_cast<std::size_t>(mip.height) * mip.row_pitch) {
        return false;
    }
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        const std::byte* row = mip.data.data() + static_cast<std::size_t>(y) * mip.row_pitch;
        for (std::uint32_t x = 0; x < mip.width; ++x) {
            if (std::to_integer<std::uint8_t>(row[x * 4U + 3U]) != 255U) {
                return true;
            }
        }
    }
    return false;
}

core::Result<void> EncodeTextureMips(std::vector<assets::TextureMip>& mips, bool source_is_float,
                                     assets::TextureFormat format, TextureQuality quality,
                                     bool srgb) {
    if (mips.empty()) {
        return MakeError(ErrorCode::Internal, "texture has no mip levels");
    }
    if (source_is_float && (format == assets::TextureFormat::RGBA8 ||
                            format == assets::TextureFormat::BGRA8 || IsCompressed(format))) {
        return MakeError(ErrorCode::Unsupported,
                         "HDR textures require a floating-point output format");
    }
    if (!source_is_float && BytesPerPixel(format) == 0 && !IsCompressed(format)) {
        return MakeError(ErrorCode::Unsupported, "texture format is not supported");
    }

    std::vector<assets::TextureMip> encoded;
    encoded.reserve(mips.size());
    for (const assets::TextureMip& source : mips) {
        auto valid = ValidateSource(source, source_is_float);
        if (!valid) {
            return std::move(valid).TakeError();
        }
        assets::TextureMip destination;
        if (IsCompressed(format)) {
            auto result = EncodeCompressedMip(source, format, quality, srgb, destination);
            if (!result) {
                return std::move(result).TakeError();
            }
        } else if (source_is_float) {
            if (format != assets::TextureFormat::RGBA16F &&
                format != assets::TextureFormat::RGBA32F) {
                return MakeError(ErrorCode::Unsupported, "texture format is not supported");
            }
            auto result = ConvertFloatMip(source, format, destination);
            if (!result) {
                return std::move(result).TakeError();
            }
        } else {
            auto result = ConvertByteMip(source, format, destination);
            if (!result) {
                return std::move(result).TakeError();
            }
        }
        encoded.push_back(std::move(destination));
    }
    mips = std::move(encoded);
    return {};
}

} // namespace parcel::import::detail
