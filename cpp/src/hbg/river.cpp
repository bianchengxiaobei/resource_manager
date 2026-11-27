#include "parcel/hbg/river.hpp"

#include "core/bounds.h"
#include "parcel/assets/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace parcel_hbg {
namespace {

struct Curve {
    std::vector<hbg::Vec2> points;
    float potential = 0.0f;
    float width = 0.0f;
};

struct Graph {
    std::vector<hbg::Vec2> axis;
    float width = 0.0f;
    Curve left_bank;
    Curve right_bank;
};

struct FlowSample {
    hbg::Vec2 velocity;
    float potential = 0.0f;
    bool inside = false;
};

struct MeshSettings {
    std::uint32_t segments = 320;
    std::uint32_t cross_segments = 12;
    float base_height = 0.05f;
    float uv_length = 180.0f;
};

constexpr float kPi = 3.14159265358979323846f;

hbg::Bounds ComputeBounds(const std::vector<hbg::asset::MeshVertex>& vertices) {
    hbg::Bounds bounds;
    for (const hbg::asset::MeshVertex& vertex : vertices) {
        bounds.expand(vertex.position);
    }
    return bounds;
}

void RecomputeSceneBounds(hbg::render::RenderScene& scene) {
    scene.bounds = hbg::Bounds{};
    for (const hbg::render::RenderObject& object : scene.objects) {
        if (object.worldBounds.valid()) {
            scene.bounds.expand(object.worldBounds);
        }
    }
}

hbg::Vec2 Subtract(hbg::Vec2 lhs, hbg::Vec2 rhs) { return hbg::Vec2{lhs.x - rhs.x, lhs.y - rhs.y}; }

hbg::Vec2 Add(hbg::Vec2 lhs, hbg::Vec2 rhs) { return hbg::Vec2{lhs.x + rhs.x, lhs.y + rhs.y}; }

hbg::Vec2 Scale(hbg::Vec2 value, float factor) {
    return hbg::Vec2{value.x * factor, value.y * factor};
}

float Dot(hbg::Vec2 lhs, hbg::Vec2 rhs) { return lhs.x * rhs.x + lhs.y * rhs.y; }

float LengthSq(hbg::Vec2 value) { return Dot(value, value); }

float Length(hbg::Vec2 value) { return std::sqrt(LengthSq(value)); }

hbg::Vec2 Normalize(hbg::Vec2 value) {
    const float magnitude = Length(value);
    return magnitude > 1.0e-8f ? Scale(value, 1.0f / magnitude) : hbg::Vec2{0.0f, 0.0f};
}

float PointSegmentDistanceSq(hbg::Vec2 point, hbg::Vec2 start, hbg::Vec2 end, float* segment_t);

float PointSegmentDistanceSq(hbg::Vec2 point, hbg::Vec2 start, hbg::Vec2 end) {
    return PointSegmentDistanceSq(point, start, end, nullptr);
}

float PointSegmentDistanceSq(hbg::Vec2 point, hbg::Vec2 start, hbg::Vec2 end, float* segment_t) {
    const hbg::Vec2 segment = Subtract(end, start);
    const float length_sq = Dot(segment, segment);
    if (length_sq <= 1.0e-12f) {
        if (segment_t != nullptr) {
            *segment_t = 0.0f;
        }
        const hbg::Vec2 delta = Subtract(point, start);
        return Dot(delta, delta);
    }
    const float t = std::clamp(Dot(Subtract(point, start), segment) / length_sq, 0.0f, 1.0f);
    if (segment_t != nullptr) {
        *segment_t = t;
    }
    const hbg::Vec2 delta = Subtract(point, Add(start, Scale(segment, t)));
    return Dot(delta, delta);
}

float DistanceToCurvePoints(const std::vector<hbg::Vec2>& points, hbg::Vec2 position) {
    if (points.empty()) {
        return std::numeric_limits<float>::infinity();
    }
    if (points.size() == 1) {
        return Length(Subtract(position, points.front()));
    }
    float best = std::numeric_limits<float>::infinity();
    for (std::size_t index = 1; index < points.size(); ++index) {
        best = std::min(best, PointSegmentDistanceSq(position, points[index - 1], points[index]));
    }
    return std::sqrt(best);
}

float DistanceToAxis(const std::vector<hbg::Vec2>& axis, hbg::Vec2 position) {
    return DistanceToCurvePoints(axis, position);
}

std::vector<hbg::Vec2> OffsetCurve(const std::vector<hbg::Vec2>& axis, float offset) {
    std::vector<hbg::Vec2> result;
    result.reserve(axis.size());
    for (std::size_t index = 0; index < axis.size(); ++index) {
        const hbg::Vec2 previous = axis[index > 0 ? index - 1 : index];
        const hbg::Vec2 next = axis[index + 1 < axis.size() ? index + 1 : index];
        hbg::Vec2 tangent = Normalize(Subtract(next, previous));
        if (Dot(tangent, tangent) <= 1.0e-12f) {
            tangent = hbg::Vec2{0.0f, 1.0f};
        }
        const hbg::Vec2 normal{-tangent.y, tangent.x};
        result.push_back(Add(axis[index], Scale(normal, offset)));
    }
    return result;
}

Graph MakeGraph(std::vector<hbg::Vec2> axis, float width) {
    Graph graph;
    graph.width = width;
    graph.axis = std::move(axis);
    if (graph.axis.size() < 2 || width <= 0.0f) {
        return graph;
    }
    const float half_width = width * 0.5f;
    graph.left_bank.points = OffsetCurve(graph.axis, half_width);
    graph.left_bank.potential = 0.0f;
    graph.left_bank.width = width;
    graph.right_bank.points = OffsetCurve(graph.axis, -half_width);
    graph.right_bank.potential = 1.0f;
    graph.right_bank.width = width;
    return graph;
}

Graph MeanderGraph(float length, float width, float amplitude, float waves,
                   std::uint32_t segments) {
    const std::uint32_t count = std::max(segments, 1u);
    std::vector<hbg::Vec2> axis;
    axis.reserve(static_cast<std::size_t>(count) + 1);
    for (std::uint32_t index = 0; index <= count; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(count);
        const float y = -length * 0.5f + length * t;
        axis.emplace_back(amplitude * std::sin(2.0f * kPi * waves * t), y);
    }
    return MakeGraph(std::move(axis), width);
}

float SmoothStep01(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SmoothFunc(float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return 6.0f * std::pow(t, 5.0f) - 15.0f * std::pow(t, 4.0f) + 10.0f * std::pow(t, 3.0f);
}

hbg::Vec3 Scale3(const hbg::Vec3& value, float factor);
float Dot3(const hbg::Vec3& lhs, const hbg::Vec3& rhs);

std::uint32_t RiverPerlinHash(std::uint32_t seed) {
    std::uint32_t value = (seed ^ 12345391U) * 2654435769U;
    value ^= (value << 6) ^ (value >> 26);
    value *= 2654435769U;
    value += (value << 5) ^ (value >> 12);
    return value;
}

float RiverPerlinHashFloat(std::uint32_t seed, float low, float high) {
    return (high - low) * static_cast<float>(RiverPerlinHash(seed)) / 4294967295.0f + low;
}

float RiverLerp(float value0, float value1, float factor) {
    return (1.0f - factor) * value0 + factor * value1;
}

float RiverTrilerp(float v000, float v100, float v010, float v110, float v001, float v101,
                   float v011, float v111, float fx, float fy, float fz) {
    const float low0 = RiverLerp(v000, v100, fx);
    const float high0 = RiverLerp(v010, v110, fx);
    const float low1 = RiverLerp(v001, v101, fx);
    const float high1 = RiverLerp(v011, v111, fx);
    return RiverLerp(RiverLerp(low0, high0, fy), RiverLerp(low1, high1, fy), fz);
}

class RiverPerlinNoise {
  public:
    explicit RiverPerlinNoise(std::uint32_t seed = 171717U) {
        for (int index = 0; index < kTableSize; ++index) {
            hbg::Vec3 direction;
            float length_sq = 0.0f;
            do {
                direction = hbg::Vec3{RiverPerlinHashFloat(seed++, -1.0f, 1.0f),
                                      RiverPerlinHashFloat(seed++, -1.0f, 1.0f),
                                      RiverPerlinHashFloat(seed++, -1.0f, 1.0f)};
                length_sq = Dot3(direction, direction);
            } while (length_sq > 1.0f || length_sq == 0.0f);
            basis_[index] = Scale3(direction, 1.0f / std::sqrt(length_sq));
            permutation_[index] = index;
        }
        for (int index = 1; index < kTableSize; ++index) {
            const int other =
                static_cast<int>(RiverPerlinHash(seed++) % static_cast<std::uint32_t>(index + 1));
            std::swap(permutation_[index], permutation_[other]);
        }
    }

    float operator()(float x, float y, float z, int period_xy, int period_z) const {
        const float floor_x = std::floor(x);
        const float floor_y = std::floor(y);
        const float floor_z = std::floor(z);
        const int i = static_cast<int>(floor_x);
        const int j = static_cast<int>(floor_y);
        const int k = static_cast<int>(floor_z);
        const hbg::Vec3& n000 = basis_[HashIndex(i, j, k, period_xy, period_z)];
        const hbg::Vec3& n100 = basis_[HashIndex(i + 1, j, k, period_xy, period_z)];
        const hbg::Vec3& n010 = basis_[HashIndex(i, j + 1, k, period_xy, period_z)];
        const hbg::Vec3& n110 = basis_[HashIndex(i + 1, j + 1, k, period_xy, period_z)];
        const hbg::Vec3& n001 = basis_[HashIndex(i, j, k + 1, period_xy, period_z)];
        const hbg::Vec3& n101 = basis_[HashIndex(i + 1, j, k + 1, period_xy, period_z)];
        const hbg::Vec3& n011 = basis_[HashIndex(i, j + 1, k + 1, period_xy, period_z)];
        const hbg::Vec3& n111 = basis_[HashIndex(i + 1, j + 1, k + 1, period_xy, period_z)];
        const float fx = x - floor_x;
        const float fy = y - floor_y;
        const float fz = z - floor_z;
        const float sx = fx * fx * fx * (10.0f - fx * (15.0f - fx * 6.0f));
        const float sy = fy * fy * fy * (10.0f - fy * (15.0f - fy * 6.0f));
        const float sz = fz * fz * fz * (10.0f - fz * (15.0f - fz * 6.0f));
        return RiverTrilerp(fx * n000.x + fy * n000.y + fz * n000.z,
                            (fx - 1.0f) * n100.x + fy * n100.y + fz * n100.z,
                            fx * n010.x + (fy - 1.0f) * n010.y + fz * n010.z,
                            (fx - 1.0f) * n110.x + (fy - 1.0f) * n110.y + fz * n110.z,
                            fx * n001.x + fy * n001.y + (fz - 1.0f) * n001.z,
                            (fx - 1.0f) * n101.x + fy * n101.y + (fz - 1.0f) * n101.z,
                            fx * n011.x + (fy - 1.0f) * n011.y + (fz - 1.0f) * n011.z,
                            (fx - 1.0f) * n111.x + (fy - 1.0f) * n111.y + (fz - 1.0f) * n111.z, sx,
                            sy, sz);
    }

  private:
    static constexpr int kTableSize = 256;

    std::uint32_t HashIndex(int i, int j, int k, int period_xy, int period_z) const {
        return static_cast<std::uint32_t>(
            permutation_[(permutation_[(permutation_[i % period_xy] + j) % period_xy] +
                          k % period_z) %
                         period_xy]);
    }

    std::array<hbg::Vec3, kTableSize> basis_{};
    std::array<int, kTableSize> permutation_{};
};

int RiverWrapIndex(int value, int period) {
    const int wrapped = value % period;
    return wrapped < 0 ? wrapped + period : wrapped;
}

struct RiverTextureFrames {
    std::vector<std::uint8_t> base_color;
    std::vector<std::vector<std::uint8_t>> normals;
    std::uint32_t size = 0;
};

float WrapUnit(float value) { return value - std::floor(value); }

std::vector<float> BuildRiverWaveHeightFrame(const RiverSettings& settings, std::uint32_t frame,
                                             std::uint32_t frame_count,
                                             const RiverPerlinNoise& noise) {
    const std::uint32_t size = std::clamp(settings.wave_tile_size, 8U, 512U);
    std::vector<float> heights(static_cast<std::size_t>(size) * size, 0.0f);
    const float phase =
        frame_count > 1 ? static_cast<float>(frame) / static_cast<float>(frame_count) * 4.0f : 0.0f;
    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            float height = 0.0f;
            int frequency = 32;
            float amplitude = 1.0f;
            for (int octave = 0; octave < 4; ++octave) {
                height += noise(u * static_cast<float>(frequency),
                                v * static_cast<float>(frequency), phase, frequency, 4) *
                          amplitude;
                frequency *= 2;
                amplitude *= 0.5f;
            }
            heights[static_cast<std::size_t>(y) * size + x] = height;
        }
    }
    return heights;
}

float SampleWaveHeightTile(const std::vector<float>& heights, std::uint32_t size, hbg::Vec2 uv) {
    if (heights.empty() || size == 0) {
        return 0.0f;
    }
    const float x = WrapUnit(uv.x) * static_cast<float>(size) - 0.5f;
    const float y = WrapUnit(uv.y) * static_cast<float>(size) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const std::uint32_t ix0 =
        static_cast<std::uint32_t>(RiverWrapIndex(x0, static_cast<int>(size)));
    const std::uint32_t iy0 =
        static_cast<std::uint32_t>(RiverWrapIndex(y0, static_cast<int>(size)));
    const std::uint32_t ix1 =
        static_cast<std::uint32_t>(RiverWrapIndex(x0 + 1, static_cast<int>(size)));
    const std::uint32_t iy1 =
        static_cast<std::uint32_t>(RiverWrapIndex(y0 + 1, static_cast<int>(size)));
    const auto at = [&heights, size](std::uint32_t xIndex, std::uint32_t yIndex) {
        return heights[static_cast<std::size_t>(yIndex) * size + xIndex];
    };
    const float low = at(ix0, iy0) * (1.0f - tx) + at(ix1, iy0) * tx;
    const float high = at(ix0, iy1) * (1.0f - tx) + at(ix1, iy1) * tx;
    return low * (1.0f - ty) + high * ty;
}

void BuildRiverBaseColorPixels(std::uint32_t size, std::vector<std::uint8_t>& pixels) {
    pixels.assign(static_cast<std::size_t>(size) * size * 4U, 255);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const std::size_t index = static_cast<std::size_t>(y) * size + x;
            const float bank =
                SmoothStep01((u - 0.02f) / 0.14f) * SmoothStep01((1.0f - u - 0.02f) / 0.14f);
            const float depth = SmoothStep01((bank - 0.10f) / 0.75f);
            const float grain =
                0.94f + 0.06f * std::sin(u * 41.0f + v * 73.0f) * std::sin(u * 19.0f - v * 29.0f);
            const float shallow[3] = {0.30f, 0.62f, 0.80f};
            const float deep[3] = {0.10f, 0.35f, 0.62f};
            for (int channel = 0; channel < 3; ++channel) {
                const float color = shallow[channel] * (1.0f - depth) + deep[channel] * depth;
                pixels[index * 4U + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
                    std::clamp(color * grain, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            pixels[index * 4U + 3U] =
                static_cast<std::uint8_t>(std::clamp(depth, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

void BuildRiverNormalFrame(const RiverSettings& settings, const std::vector<float>& wave_heights,
                           std::uint32_t size, std::vector<std::uint8_t>& pixels) {
    pixels.assign(static_cast<std::size_t>(size) * size * 4U, 255);
    std::vector<float> heights(static_cast<std::size_t>(size) * size, 0.0f);
    const float step = 2.0f / static_cast<float>(size);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) * step;
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) * step;
            heights[static_cast<std::size_t>(y) * size + x] = SampleWaveHeightTile(
                wave_heights, std::clamp(settings.wave_tile_size, 8U, 512U), hbg::Vec2{u, v});
        }
    }

    const float wave_slope = std::clamp(settings.wave_slope, 0.0f, 1.0f);
    const float wave_slope_factor = std::clamp(settings.wave_slope_factor, 0.0f, 16.0f);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const std::uint32_t x0 = static_cast<std::uint32_t>(
                RiverWrapIndex(static_cast<int>(x) - 1, static_cast<int>(size)));
            const std::uint32_t x1 = static_cast<std::uint32_t>(
                RiverWrapIndex(static_cast<int>(x) + 1, static_cast<int>(size)));
            const std::uint32_t y0 = static_cast<std::uint32_t>(
                RiverWrapIndex(static_cast<int>(y) - 1, static_cast<int>(size)));
            const std::uint32_t y1 = static_cast<std::uint32_t>(
                RiverWrapIndex(static_cast<int>(y) + 1, static_cast<int>(size)));
            const std::size_t index = static_cast<std::size_t>(y) * size + x;
            const float bank =
                SmoothStep01((u - 0.02f) / 0.14f) * SmoothStep01((1.0f - u - 0.02f) / 0.14f);
            const float damper = 0.18f + 0.82f * bank;
            const float dx = heights[static_cast<std::size_t>(y) * size + x1] -
                             heights[static_cast<std::size_t>(y) * size + x0];
            const float dy = heights[static_cast<std::size_t>(y1) * size + x] -
                             heights[static_cast<std::size_t>(y0) * size + x];
            const float slope_x = dx * 2.88f * wave_slope * damper * wave_slope_factor;
            const float slope_y = dy * 2.88f * wave_slope * damper * wave_slope_factor;
            const float inverse_length =
                1.0f / std::sqrt(slope_x * slope_x + slope_y * slope_y + 1.0f);
            pixels[index * 4U + 0U] =
                static_cast<std::uint8_t>((slope_x * inverse_length * 0.5f + 0.5f) * 255.0f + 0.5f);
            pixels[index * 4U + 1U] =
                static_cast<std::uint8_t>((slope_y * inverse_length * 0.5f + 0.5f) * 255.0f + 0.5f);
            pixels[index * 4U + 2U] =
                static_cast<std::uint8_t>((inverse_length * 0.5f + 0.5f) * 255.0f + 0.5f);
            pixels[index * 4U + 3U] = 255;
        }
    }
}

RiverTextureFrames BuildRiverTextureFrames(const RiverSettings& settings) {
    RiverTextureFrames frames;
    frames.size = std::clamp(settings.texture_size, 32U, 512U);
    if (frames.size == 0) {
        return frames;
    }
    BuildRiverBaseColorPixels(frames.size, frames.base_color);
    const std::uint32_t frame_count = std::clamp(settings.wave_time_loop, 1U, 16U);
    frames.normals.resize(frame_count);
    const RiverPerlinNoise noise(171717U);
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::vector<float> heights =
            BuildRiverWaveHeightFrame(settings, frame, frame_count, noise);
        BuildRiverNormalFrame(settings, heights, frames.size, frames.normals[frame]);
    }
    return frames;
}

std::uint32_t AppendRiverImage(hbg::render::RenderScene& scene,
                               std::vector<MaterializedTexture>& material_images,
                               std::string_view name, std::uint32_t size,
                               const std::vector<std::uint8_t>& pixels, bool srgb) {
    hbg::asset::ImageAsset image;
    image.name = std::string(name);
    image.width = size;
    image.height = size;
    image.mipCount = 1;
    image.format = hbg::gfx::TextureFormat::RGBA8;
    image.pixels = pixels;
    const std::uint32_t image_index = static_cast<std::uint32_t>(scene.images.size());
    scene.images.push_back(std::move(image));

    auto storage = std::make_shared<std::vector<std::byte>>(pixels.size());
    if (!pixels.empty()) {
        std::memcpy(storage->data(), pixels.data(), pixels.size());
    }
    MaterializedTexture texture;
    texture.desc.name = std::string(name);
    texture.desc.width = static_cast<std::uint16_t>(size);
    texture.desc.height = static_cast<std::uint16_t>(size);
    texture.desc.layers = 1;
    texture.desc.mipCount = 1;
    texture.desc.msaaSamples = 1;
    texture.desc.format = hbg::gfx::TextureFormat::RGBA8;
    texture.desc.flags = BGFX_SAMPLER_UVW_CLAMP | (srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE);
    texture.storage = storage;
    texture.desc.data = storage->data();
    texture.desc.dataSize = static_cast<std::uint32_t>(storage->size());
    material_images.push_back(std::move(texture));
    return image_index;
}

bool SamplePotential(const Graph& graph, hbg::Vec2 position, float& potential) {
    if (DistanceToAxis(graph.axis, position) > graph.width * 0.5f ||
        graph.left_bank.points.empty() || graph.right_bank.points.empty()) {
        return false;
    }
    const float max_width = std::max(graph.left_bank.width, graph.right_bank.width);
    if (max_width <= 0.0f) {
        return false;
    }
    const float left_distance =
        std::min(DistanceToCurvePoints(graph.left_bank.points, position), max_width);
    const float right_distance =
        std::min(DistanceToCurvePoints(graph.right_bank.points, position), max_width);
    const float distances[2] = {left_distance, right_distance};
    const float potentials[2] = {graph.left_bank.potential, graph.right_bank.potential};
    float weights[2] = {0.0f, 0.0f};
    for (int bank = 0; bank < 2; ++bank) {
        const float kernel = SmoothFunc(1.0f - distances[bank] / max_width);
        float product = 1.0f;
        for (int other = 0; other < 2; ++other) {
            if (other != bank) {
                product *= std::pow(std::max(distances[other], 1.0e-4f), 1.3f);
            }
        }
        weights[bank] = product * kernel;
    }
    const float denominator = weights[0] + weights[1];
    if (denominator <= 1.0e-8f) {
        return false;
    }
    potential = (weights[0] * potentials[0] + weights[1] * potentials[1]) / denominator;
    return true;
}

FlowSample SampleFlow(const Graph& graph, hbg::Vec2 position) {
    FlowSample result;
    if (DistanceToAxis(graph.axis, position) > graph.width * 0.5f) {
        return result;
    }
    const float step = std::max(graph.width / 8.0f, 0.5f);
    float p0 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
    float p3 = 0.0f;
    if (!SamplePotential(graph, position, p0) ||
        !SamplePotential(graph, hbg::Vec2{position.x + step, position.y}, p1) ||
        !SamplePotential(graph, hbg::Vec2{position.x, position.y + step}, p2) ||
        !SamplePotential(graph, hbg::Vec2{position.x + step, position.y + step}, p3)) {
        return result;
    }
    const float inverse = 1.0f / (4.0f * step);
    result.velocity = hbg::Vec2{-(p2 - p0 + p3 - p1) * inverse, (p3 - p2 + p1 - p0) * inverse};
    result.potential = p0;
    result.inside = std::isfinite(result.velocity.x) && std::isfinite(result.velocity.y) &&
                    std::isfinite(result.potential);
    return result;
}

struct RiverFlowField {
    std::vector<hbg::Vec2> axis;
    std::vector<hbg::Vec2> velocity;
    float half_width = 0.0f;

    [[nodiscard]] bool valid() const {
        return axis.size() >= 2 && velocity.size() == axis.size() && half_width > 0.0f;
    }
};

RiverFlowField BuildFlowField(const Graph& graph) {
    RiverFlowField field;
    if (graph.axis.size() < 2 || graph.width <= 0.0f) {
        return field;
    }
    field.axis = graph.axis;
    field.half_width = graph.width * 0.5f;
    field.velocity.reserve(field.axis.size());
    for (std::size_t index = 0; index < field.axis.size(); ++index) {
        const FlowSample sample = SampleFlow(graph, field.axis[index]);
        if (sample.inside) {
            field.velocity.push_back(sample.velocity);
            continue;
        }
        const hbg::Vec2 previous = field.axis[index > 0 ? index - 1 : index];
        const hbg::Vec2 next = field.axis[index + 1 < field.axis.size() ? index + 1 : index];
        const hbg::Vec2 tangent = Normalize(Subtract(next, previous));
        field.velocity.push_back(Scale(tangent, 0.02f));
    }
    return field;
}

std::size_t NearestAxisSegment(const std::vector<hbg::Vec2>& axis, hbg::Vec2 position) {
    float best_distance = std::numeric_limits<float>::infinity();
    std::size_t best_segment = 1;
    for (std::size_t index = 1; index < axis.size(); ++index) {
        const float distance = PointSegmentDistanceSq(position, axis[index - 1], axis[index]);
        if (distance < best_distance) {
            best_distance = distance;
            best_segment = index;
        }
    }
    return best_segment;
}

FlowSample SampleFlowField(const RiverFlowField& field, hbg::Vec2 position,
                           std::size_t& segment_hint) {
    FlowSample result;
    if (!field.valid()) {
        return result;
    }
    constexpr std::size_t kSearchWindow = 8;
    const std::size_t last_segment = field.axis.size() - 1;
    const std::size_t begin = segment_hint > kSearchWindow ? segment_hint - kSearchWindow : 1;
    const std::size_t end = std::min(last_segment, segment_hint + kSearchWindow);
    float best_distance = std::numeric_limits<float>::infinity();
    float best_t = 0.0f;
    std::size_t best_segment = segment_hint;
    for (std::size_t index = begin; index <= end; ++index) {
        float segment_t = 0.0f;
        const float distance =
            PointSegmentDistanceSq(position, field.axis[index - 1], field.axis[index], &segment_t);
        if (distance < best_distance) {
            best_distance = distance;
            best_t = segment_t;
            best_segment = index;
        }
    }
    if (best_segment == begin || best_segment == end) {
        for (std::size_t index = 1; index <= last_segment; ++index) {
            float segment_t = 0.0f;
            const float distance = PointSegmentDistanceSq(position, field.axis[index - 1],
                                                          field.axis[index], &segment_t);
            if (distance < best_distance) {
                best_distance = distance;
                best_t = segment_t;
                best_segment = index;
            }
        }
    }
    segment_hint = best_segment;
    if (std::sqrt(best_distance) > field.half_width) {
        return result;
    }
    const hbg::Vec2& start_velocity = field.velocity[best_segment - 1];
    const hbg::Vec2& end_velocity = field.velocity[best_segment];
    result.velocity = hbg::Vec2{RiverLerp(start_velocity.x, end_velocity.x, best_t),
                                RiverLerp(start_velocity.y, end_velocity.y, best_t)};
    result.inside = true;
    return result;
}

hbg::Vec2 SampleAxis(const std::vector<hbg::Vec2>& axis, float t, hbg::Vec2& tangent) {
    if (axis.size() < 2) {
        tangent = hbg::Vec2{0.0f, 1.0f};
        return axis.empty() ? hbg::Vec2{0.0f, 0.0f} : axis.front();
    }
    float total_length = 0.0f;
    for (std::size_t index = 1; index < axis.size(); ++index) {
        total_length += Length(Subtract(axis[index], axis[index - 1]));
    }
    if (total_length <= 1.0e-6f) {
        tangent = hbg::Vec2{0.0f, 1.0f};
        return axis.front();
    }
    const float target = std::clamp(t, 0.0f, 1.0f) * total_length;
    float accumulated = 0.0f;
    for (std::size_t index = 1; index < axis.size(); ++index) {
        const hbg::Vec2 delta = Subtract(axis[index], axis[index - 1]);
        const float segment_length = Length(delta);
        if (accumulated + segment_length >= target || index + 1 == axis.size()) {
            const float local =
                segment_length > 1.0e-6f ? (target - accumulated) / segment_length : 0.0f;
            tangent = Normalize(delta);
            return Add(axis[index - 1], Scale(delta, std::clamp(local, 0.0f, 1.0f)));
        }
        accumulated += segment_length;
    }
    tangent = Normalize(Subtract(axis.back(), axis[axis.size() - 2]));
    return axis.back();
}

hbg::asset::Mesh BuildMesh(const Graph& graph, const MeshSettings& settings,
                           std::uint32_t material_index) {
    hbg::asset::Mesh mesh;
    mesh.name = "RiverSurface";
    if (graph.axis.size() < 2 || graph.width <= 0.0f || settings.segments == 0 ||
        settings.cross_segments == 0) {
        return mesh;
    }

    const std::uint32_t segments = settings.segments;
    const std::uint32_t cross_segments = settings.cross_segments;
    const std::uint32_t stride = cross_segments + 1;
    const float half_width = graph.width * 0.5f;
    hbg::Vec2 first_tangent;
    const hbg::Vec2 first_position = SampleAxis(graph.axis, 0.0f, first_tangent);
    mesh.vertices.reserve(static_cast<std::size_t>(segments + 1) * stride);
    for (std::uint32_t segment = 0; segment <= segments; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segments);
        hbg::Vec2 tangent;
        const hbg::Vec2 center = SampleAxis(graph.axis, t, tangent);
        const hbg::Vec2 normal{-tangent.y, tangent.x};
        for (std::uint32_t cross = 0; cross <= cross_segments; ++cross) {
            const float u = static_cast<float>(cross) / static_cast<float>(cross_segments);
            const float offset = (u * 2.0f - 1.0f) * half_width;
            const hbg::Vec2 point = Add(center, Scale(normal, offset));
            hbg::asset::MeshVertex vertex;
            vertex.position = hbg::Vec3{point.x, settings.base_height, point.y};
            vertex.normal = hbg::Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent = hbg::Vec4{tangent.x, 0.0f, tangent.y, 1.0f};
            vertex.texcoord0 =
                hbg::Vec2{u, (point.y - first_position.y) / std::max(settings.uv_length, 1.0e-3f)};
            mesh.vertices.push_back(vertex);
        }
    }

    mesh.indices.reserve(static_cast<std::size_t>(segments) * cross_segments * 6);
    for (std::uint32_t segment = 0; segment < segments; ++segment) {
        for (std::uint32_t cross = 0; cross < cross_segments; ++cross) {
            const std::uint32_t a = segment * stride + cross;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + stride;
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }

    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    mesh.bounds = ComputeBounds(mesh.vertices);
    hbg::asset::SubMesh sub_mesh;
    sub_mesh.indexOffset = 0;
    sub_mesh.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    sub_mesh.materialIndex = material_index;
    sub_mesh.bounds = mesh.bounds;
    mesh.submeshes.push_back(sub_mesh);
    return mesh;
}

struct RiverFoamPixels {
    std::vector<std::uint8_t> pixels;
    std::uint32_t size = 0;
};

RiverFoamPixels BuildRiverFoamPixels(std::uint32_t requested_size, std::uint32_t seed) {
    RiverFoamPixels result;
    result.size = std::clamp(requested_size, 32U, 128U);
    result.pixels.assign(static_cast<std::size_t>(result.size) * result.size * 4U, 0);
    const RiverPerlinNoise noise(seed == 0 ? 171717U : seed);
    for (std::uint32_t y = 0; y < result.size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(result.size);
        for (std::uint32_t x = 0; x < result.size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(result.size);
            float value = 0.0f;
            int frequency = 4;
            float amplitude = 0.58f;
            for (int octave = 0; octave < 3; ++octave) {
                value += noise(u * static_cast<float>(frequency), v * static_cast<float>(frequency),
                               0.37f, frequency, 4) *
                         amplitude;
                frequency *= 2;
                amplitude *= 0.5f;
            }
            const float dx = u * 2.0f - 1.0f;
            const float dy = v * 2.0f - 1.0f;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float edge = SmoothStep01((1.0f - distance) / 0.65f);
            const float wisps = SmoothStep01(((value * 0.5f + 0.5f) - 0.18f) / 0.72f);
            const float alpha = edge * (0.30f + 0.70f * wisps);
            const std::size_t index = (static_cast<std::size_t>(y) * result.size + x) * 4U;
            result.pixels[index + 0U] = static_cast<std::uint8_t>(
                std::clamp(0.86f + value * 0.08f, 0.0f, 1.0f) * 255.0f + 0.5f);
            result.pixels[index + 1U] = static_cast<std::uint8_t>(
                std::clamp(0.93f + value * 0.05f, 0.0f, 1.0f) * 255.0f + 0.5f);
            result.pixels[index + 2U] = 255;
            result.pixels[index + 3U] =
                static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return result;
}

class RiverFoamRandom {
  public:
    explicit RiverFoamRandom(std::uint32_t seed) : state_(seed == 0 ? 0x9e3779b9U : seed) {}

    float Next01() {
        state_ = RiverPerlinHash(state_ + 0x9e3779b9U);
        return static_cast<float>(state_ & 0x00ffffffU) / 16777216.0f;
    }

  private:
    std::uint32_t state_ = 0x9e3779b9U;
};

struct RiverFoamParticle {
    hbg::Vec2 position;
    hbg::Vec2 spawn_position;
    hbg::Vec2 direction;
    hbg::Vec2 spawn_direction;
    float radius = 0.0f;
    float half_length = 0.0f;
    float half_width = 0.0f;
    std::size_t segment_hint = 1;
    std::size_t spawn_segment = 1;
    float age = 0.0f;
    float life = 1.0f;
    float fade = 0.0f;
    bool active = true;
};

std::vector<RiverFoamParticle> BuildRiverFoamParticles(const Graph& graph,
                                                       const RiverSettings& settings) {
    std::vector<RiverFoamParticle> particles;
    const RiverParticleSettings& particle_settings = settings.particles;
    const std::uint32_t count =
        particle_settings.enabled ? std::clamp(particle_settings.max_particles, 0U, 1024U) : 0U;
    if (count == 0 || graph.axis.size() < 2 || graph.width <= 0.0f) {
        return particles;
    }
    const float life =
        std::max(particle_settings.fade_in_seconds + particle_settings.active_seconds +
                     particle_settings.fade_out_seconds,
                 0.01f);
    RiverFoamRandom random(particle_settings.seed);
    particles.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const float jitter = (random.Next01() - 0.5f) / static_cast<float>(count);
        const float t = std::clamp(
            (static_cast<float>(index) + 0.5f) / static_cast<float>(count) + jitter, 0.0f, 1.0f);
        hbg::Vec2 tangent;
        const hbg::Vec2 center = SampleAxis(graph.axis, t, tangent);
        const hbg::Vec2 side{-tangent.y, tangent.x};
        const float lateral = (random.Next01() * 2.0f - 1.0f) * graph.width * 0.42f;
        hbg::Vec2 position = Add(center, Scale(side, lateral));
        hbg::Vec2 direction = Normalize(tangent);
        const float base_radius =
            std::max(particle_settings.screen_radius * graph.width / 512.0f, 0.2f);
        const float radius = base_radius * (0.60f + random.Next01() * 0.80f);
        for (int step = 0; step < 8; ++step) {
            const FlowSample sample = SampleFlow(graph, position);
            if (!sample.inside) {
                break;
            }
            const float speed = Length(sample.velocity);
            if (speed > 1.0e-6f) {
                direction = Scale(sample.velocity, 1.0f / speed);
            }
            const float advance = std::min(
                speed * std::max(particle_settings.speed_scale, 0.0f) * 0.02f, radius * 1.25f);
            const hbg::Vec2 next = Add(position, Scale(direction, advance));
            if (!SampleFlow(graph, next).inside) {
                break;
            }
            position = next;
        }
        const float age = random.Next01() * life;
        RiverFoamParticle particle;
        particle.position = position;
        particle.spawn_position = position;
        particle.segment_hint = NearestAxisSegment(graph.axis, position);
        particle.spawn_segment = particle.segment_hint;
        particle.direction = direction;
        particle.spawn_direction = direction;
        particle.radius = radius;
        particle.half_length = radius * (0.90f + random.Next01() * 0.70f);
        particle.half_width = radius * (0.35f + random.Next01() * 0.25f);
        particle.age = age;
        particle.life = life;
        particle.fade = RiverLifecycleFade(age, particle_settings.fade_in_seconds,
                                           particle_settings.active_seconds,
                                           particle_settings.fade_out_seconds);
        particles.push_back(particle);
    }
    return particles;
}

hbg::Vec4 TransformRiverVec4(const hbg::Mat4& matrix, const hbg::Vec4& value) {
    return hbg::Vec4{matrix.values[0] * value.x + matrix.values[4] * value.y +
                         matrix.values[8] * value.z + matrix.values[12] * value.w,
                     matrix.values[1] * value.x + matrix.values[5] * value.y +
                         matrix.values[9] * value.z + matrix.values[13] * value.w,
                     matrix.values[2] * value.x + matrix.values[6] * value.y +
                         matrix.values[10] * value.z + matrix.values[14] * value.w,
                     matrix.values[3] * value.x + matrix.values[7] * value.y +
                         matrix.values[11] * value.z + matrix.values[15] * value.w};
}

bool ScreenToWaterPlane(const RiverView& view, float base_height, hbg::Vec2 screen,
                        hbg::Vec2& water_position) {
    if (!view.valid()) {
        return false;
    }
    const float ndc_x = 2.0f * screen.x / static_cast<float>(view.viewport_width) - 1.0f;
    const float ndc_y = 1.0f - 2.0f * screen.y / static_cast<float>(view.viewport_height);
    const hbg::Vec4 far_point =
        TransformRiverVec4(view.inv_view_proj, hbg::Vec4{ndc_x, ndc_y, 1.0f, 1.0f});
    if (far_point.w <= 1.0e-6f) {
        return false;
    }
    const float inverse_w = 1.0f / far_point.w;
    const float dx = far_point.x * inverse_w - view.camera_position.x;
    const float dy = far_point.y * inverse_w - view.camera_position.y;
    const float dz = far_point.z * inverse_w - view.camera_position.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length <= 1.0e-6f || std::abs(dy / length) <= 1.0e-5f) {
        return false;
    }
    const float t = (base_height - view.camera_position.y) / (dy / length);
    if (!(t > 0.0f) || !std::isfinite(t)) {
        return false;
    }
    water_position = hbg::Vec2{view.camera_position.x + dx / length * t,
                               view.camera_position.z + dz / length * t};
    return std::isfinite(water_position.x) && std::isfinite(water_position.y);
}

bool ProjectRiverPoint(const RiverView& view, const hbg::Vec3& world, hbg::Vec2& screen) {
    if (!view.valid()) {
        return false;
    }
    const hbg::Vec4 clip =
        TransformRiverVec4(view.view_proj, hbg::Vec4{world.x, world.y, world.z, 1.0f});
    if (clip.w <= 1.0e-6f) {
        return false;
    }
    const float inverse_w = 1.0f / clip.w;
    const float ndc_x = clip.x * inverse_w;
    const float ndc_y = clip.y * inverse_w;
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        return false;
    }
    screen = hbg::Vec2{(ndc_x * 0.5f + 0.5f) * static_cast<float>(view.viewport_width),
                       (0.5f - ndc_y * 0.5f) * static_cast<float>(view.viewport_height)};
    return true;
}

std::vector<hbg::Vec2> BuildScreenPoissonSamples(const RiverView& view, float radius,
                                                 std::uint32_t max_samples, RiverFoamRandom& random,
                                                 hbg::Vec2 screen_min, hbg::Vec2 screen_max) {
    std::vector<hbg::Vec2> accepted;
    const float width = screen_max.x - screen_min.x;
    const float height = screen_max.y - screen_min.y;
    if (!view.valid() || max_samples == 0 || width <= 0.0f || height <= 0.0f) {
        return accepted;
    }
    const float spacing = std::max(2.0f * radius, 1.0f);
    const int grid_width = std::max(1, static_cast<int>(std::ceil(width / spacing)));
    const int grid_height = std::max(1, static_cast<int>(std::ceil(height / spacing)));
    std::vector<std::vector<std::uint32_t>> cells(static_cast<std::size_t>(grid_width) *
                                                  static_cast<std::size_t>(grid_height));
    const float overlap_sq = 0.96f * 4.0f * radius * radius;
    const std::uint32_t max_attempts = std::max(1024U, max_samples * 16U);
    const std::uint32_t capacity_attempts =
        static_cast<std::uint32_t>(grid_width) * static_cast<std::uint32_t>(grid_height) * 4U;
    const std::uint32_t attempts = std::min(max_attempts, std::max(1024U, capacity_attempts));
    accepted.reserve(std::min<std::size_t>(max_samples, 1024U));
    for (std::uint32_t attempt = 0; attempt < attempts && accepted.size() < max_samples;
         ++attempt) {
        const hbg::Vec2 point{
            screen_min.x + random.Next01() * width,
            screen_min.y + random.Next01() * height,
        };
        const int cell_x =
            std::clamp(static_cast<int>((point.x - screen_min.x) / spacing), 0, grid_width - 1);
        const int cell_y =
            std::clamp(static_cast<int>((point.y - screen_min.y) / spacing), 0, grid_height - 1);
        bool overlap = false;
        for (int y = std::max(0, cell_y - 1);
             y <= std::min(grid_height - 1, cell_y + 1) && !overlap; ++y) {
            for (int x = std::max(0, cell_x - 1); x <= std::min(grid_width - 1, cell_x + 1); ++x) {
                const std::vector<std::uint32_t>& cell =
                    cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(grid_width) +
                          static_cast<std::size_t>(x)];
                for (std::uint32_t index : cell) {
                    const hbg::Vec2 delta = Subtract(accepted[index], point);
                    if (LengthSq(delta) < overlap_sq) {
                        overlap = true;
                        break;
                    }
                }
                if (overlap) {
                    break;
                }
            }
        }
        if (overlap) {
            continue;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(accepted.size());
        accepted.push_back(point);
        cells[static_cast<std::size_t>(cell_y) * static_cast<std::size_t>(grid_width) +
              static_cast<std::size_t>(cell_x)]
            .push_back(index);
    }
    return accepted;
}

std::vector<hbg::Vec2> BuildRiverScreenPoissonSamples(const RiverSettings& settings,
                                                      const RiverFlowField& flow,
                                                      const RiverView& view,
                                                      std::uint32_t max_samples,
                                                      RiverFoamRandom& random) {
    std::vector<hbg::Vec2> river_samples;
    if (!flow.valid() || max_samples == 0) {
        return river_samples;
    }
    hbg::Vec2 screen_min{std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::infinity()};
    hbg::Vec2 screen_max{-std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    bool bounds_valid = false;
    for (std::size_t index = 0; index < flow.axis.size(); ++index) {
        const hbg::Vec2 previous = flow.axis[index > 0 ? index - 1 : index];
        const hbg::Vec2 next = flow.axis[index + 1 < flow.axis.size() ? index + 1 : index];
        const hbg::Vec2 tangent = Normalize(Subtract(next, previous));
        const hbg::Vec2 side{-tangent.y, tangent.x};
        for (float lateral : {-flow.half_width, 0.0f, flow.half_width}) {
            const hbg::Vec2 world = Add(flow.axis[index], Scale(side, lateral));
            hbg::Vec2 screen;
            if (!ProjectRiverPoint(view, hbg::Vec3{world.x, settings.base_height, world.y},
                                   screen)) {
                continue;
            }
            screen_min.x = std::min(screen_min.x, screen.x);
            screen_min.y = std::min(screen_min.y, screen.y);
            screen_max.x = std::max(screen_max.x, screen.x);
            screen_max.y = std::max(screen_max.y, screen.y);
            bounds_valid = true;
        }
    }
    if (!bounds_valid) {
        return river_samples;
    }
    const float margin = std::max(settings.particles.screen_radius * 2.0f, 1.0f);
    screen_min.x = std::min(screen_min.x - margin, static_cast<float>(view.viewport_width));
    screen_min.y = std::min(screen_min.y - margin, static_cast<float>(view.viewport_height));
    screen_max.x = std::max(screen_max.x + margin, 0.0f);
    screen_max.y = std::max(screen_max.y + margin, 0.0f);
    screen_min.x = std::max(screen_min.x, 0.0f);
    screen_min.y = std::max(screen_min.y, 0.0f);
    screen_max.x = std::min(screen_max.x, static_cast<float>(view.viewport_width));
    screen_max.y = std::min(screen_max.y, static_cast<float>(view.viewport_height));
    float radius = std::clamp(settings.particles.screen_radius, 1.0f, 64.0f);
    while (radius >= 1.0f) {
        const std::uint32_t sample_capacity = std::max(max_samples, max_samples * 8U);
        const std::vector<hbg::Vec2> samples = BuildScreenPoissonSamples(
            view, radius, sample_capacity, random, screen_min, screen_max);
        river_samples.clear();
        for (const hbg::Vec2& screen : samples) {
            hbg::Vec2 water;
            if (!ScreenToWaterPlane(view, settings.base_height, screen, water)) {
                continue;
            }
            std::size_t segment_hint = NearestAxisSegment(flow.axis, water);
            if (!SampleFlowField(flow, water, segment_hint).inside) {
                continue;
            }
            river_samples.push_back(screen);
            if (river_samples.size() >= max_samples) {
                break;
            }
        }
        if (river_samples.size() >= max_samples) {
            break;
        }
        radius *= 0.7f;
    }
    return river_samples;
}

bool BuildScreenSpaceFoamParticle(const RiverSettings& settings, const RiverFlowField& flow,
                                  const RiverView& view, hbg::Vec2 screen, RiverFoamRandom& random,
                                  RiverFoamParticle& particle) {
    hbg::Vec2 water;
    if (!ScreenToWaterPlane(view, settings.base_height, screen, water)) {
        return false;
    }
    std::size_t segment_hint = NearestAxisSegment(flow.axis, water);
    const FlowSample sample = SampleFlowField(flow, water, segment_hint);
    if (!sample.inside) {
        return false;
    }
    hbg::Vec2 direction = Normalize(sample.velocity);
    if (LengthSq(direction) <= 1.0e-12f) {
        const hbg::Vec2 start = flow.axis[segment_hint > 0 ? segment_hint - 1 : 0];
        const hbg::Vec2 end = flow.axis[std::min(segment_hint, flow.axis.size() - 1)];
        direction = Normalize(Subtract(end, start));
        if (LengthSq(direction) <= 1.0e-12f) {
            direction = hbg::Vec2{0.0f, 1.0f};
        }
    }
    const float radius =
        std::max(settings.particles.screen_radius * flow.half_width * 2.0f / 512.0f, 0.2f);
    const float life =
        std::max(settings.particles.fade_in_seconds + settings.particles.active_seconds +
                     settings.particles.fade_out_seconds,
                 0.01f);
    particle = {};
    particle.position = water;
    particle.spawn_position = water;
    particle.direction = direction;
    particle.spawn_direction = direction;
    particle.segment_hint = segment_hint;
    particle.spawn_segment = segment_hint;
    particle.radius = radius;
    particle.half_length = radius * (0.90f + random.Next01() * 0.70f);
    particle.half_width = radius * (0.35f + random.Next01() * 0.25f);
    particle.age = random.Next01() * life;
    particle.life = life;
    particle.fade =
        RiverLifecycleFade(particle.age, settings.particles.fade_in_seconds,
                           settings.particles.active_seconds, settings.particles.fade_out_seconds);
    particle.active = true;
    return true;
}

bool SeedRiverFoamScreenSpace(const RiverSettings& settings, const RiverFlowField& flow,
                              const RiverView& view, std::vector<RiverFoamParticle>& particles,
                              std::uint32_t& screen_particle_count, RiverFoamRandom& random) {
    if (particles.empty() || !flow.valid()) {
        return false;
    }
    const std::vector<hbg::Vec2> samples = BuildRiverScreenPoissonSamples(
        settings, flow, view, static_cast<std::uint32_t>(particles.size()), random);
    if (samples.empty()) {
        return false;
    }
    const float life =
        std::max(settings.particles.fade_in_seconds + settings.particles.active_seconds +
                     settings.particles.fade_out_seconds,
                 0.01f);
    RiverFoamParticle hidden;
    hidden.active = false;
    hidden.fade = 0.0f;
    hidden.life = life;
    hidden.position = flow.axis.front();
    hidden.spawn_position = flow.axis.front();
    hidden.direction = hbg::Vec2{0.0f, 1.0f};
    hidden.spawn_direction = hidden.direction;
    hidden.segment_hint = 1;
    hidden.spawn_segment = 1;
    hidden.radius =
        std::max(settings.particles.screen_radius * flow.half_width * 2.0f / 512.0f, 0.2f);
    std::vector<RiverFoamParticle> seeded(particles.size(), hidden);
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < samples.size() && count < seeded.size(); ++index) {
        RiverFoamParticle particle;
        if (!BuildScreenSpaceFoamParticle(settings, flow, view, samples[index], random, particle)) {
            continue;
        }
        seeded[count] = particle;
        ++count;
    }
    if (count == 0) {
        return false;
    }
    particles = std::move(seeded);
    screen_particle_count = count;
    return true;
}

bool RespawnRiverFoamParticle(const RiverSettings& settings, const RiverFlowField& flow,
                              const RiverView& view, RiverFoamRandom& random,
                              RiverFoamParticle& particle) {
    if (!view.valid()) {
        return false;
    }
    for (int attempt = 0; attempt < 32; ++attempt) {
        const hbg::Vec2 screen{
            random.Next01() * static_cast<float>(view.viewport_width),
            random.Next01() * static_cast<float>(view.viewport_height),
        };
        RiverFoamParticle candidate;
        if (!BuildScreenSpaceFoamParticle(settings, flow, view, screen, random, candidate)) {
            continue;
        }
        candidate.age = 0.0f;
        candidate.fade = RiverLifecycleFade(0.0f, settings.particles.fade_in_seconds,
                                            settings.particles.active_seconds,
                                            settings.particles.fade_out_seconds);
        particle = candidate;
        return true;
    }
    particle.active = false;
    particle.fade = 0.0f;
    return false;
}

hbg::asset::Mesh BuildRiverFoamMesh(const std::vector<RiverFoamParticle>& particles,
                                    const RiverSettings& settings, std::uint32_t material_index,
                                    float& min_fade, float& max_fade) {
    hbg::asset::Mesh mesh;
    mesh.name = "RiverFoam";
    min_fade = std::numeric_limits<float>::infinity();
    max_fade = 0.0f;
    if (particles.empty()) {
        return mesh;
    }
    const float y = settings.base_height + 0.035f;
    mesh.vertices.reserve(particles.size() * 4U);
    mesh.indices.reserve(particles.size() * 6U);
    for (const RiverFoamParticle& particle : particles) {
        const hbg::Vec2 side{-particle.direction.y, particle.direction.x};
        const hbg::Vec2 along = Scale(particle.direction, particle.half_length);
        const hbg::Vec2 across = Scale(side, particle.half_width);
        const hbg::Vec2 corners[4] = {
            Subtract(Subtract(particle.position, along), across),
            Add(Subtract(particle.position, across), along),
            Add(Add(particle.position, along), across),
            Add(Subtract(particle.position, along), across),
        };
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (std::uint32_t corner = 0; corner < 4; ++corner) {
            hbg::asset::MeshVertex vertex;
            vertex.position = hbg::Vec3{corners[corner].x, y, corners[corner].y};
            vertex.normal = hbg::Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent = hbg::Vec4{particle.direction.x, 0.0f, particle.direction.y, 1.0f};
            vertex.texcoord0 =
                hbg::Vec2{corner == 1 || corner == 2 ? 1.0f : 0.0f, corner >= 2 ? 1.0f : 0.0f};
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
        min_fade = std::min(min_fade, particle.fade);
        max_fade = std::max(max_fade, particle.fade);
    }
    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    mesh.bounds = ComputeBounds(mesh.vertices);
    hbg::asset::SubMesh sub_mesh;
    sub_mesh.indexOffset = 0;
    sub_mesh.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    sub_mesh.materialIndex = material_index;
    sub_mesh.bounds = mesh.bounds;
    mesh.submeshes.push_back(sub_mesh);
    if (!std::isfinite(min_fade)) {
        min_fade = 0.0f;
    }
    return mesh;
}

struct RiverFoamBinding {
    std::vector<RiverFoamParticle> particles;
    std::uint32_t mesh_index = UINT32_MAX;
    std::uint32_t object_index = UINT32_MAX;
    std::uint32_t target_index = UINT32_MAX;
};

bool WriteRiverFoamMorph(hbg::render::RenderScene& scene, const RiverFoamBinding& binding,
                         RiverStats& stats) {
    if (binding.mesh_index >= scene.meshes.size() || binding.target_index == UINT32_MAX) {
        return false;
    }
    hbg::asset::Mesh& mesh = scene.meshes[binding.mesh_index];
    if (binding.target_index >= mesh.morphTargets.size() ||
        mesh.vertices.size() != binding.particles.size() * 4U) {
        return false;
    }
    hbg::asset::MorphTarget& target = mesh.morphTargets[binding.target_index];
    target.positionDeltas.resize(mesh.vertices.size());
    target.hasNormals = false;
    target.hasTangents = false;
    float min_fade = std::numeric_limits<float>::infinity();
    float max_fade = 0.0f;
    float max_displacement = 0.0f;
    for (std::size_t particle_index = 0; particle_index < binding.particles.size();
         ++particle_index) {
        const RiverFoamParticle& particle = binding.particles[particle_index];
        const float fade = particle.active ? std::clamp(particle.fade, 0.0f, 1.0f) : 0.0f;
        const hbg::Vec2 side{-particle.direction.y, particle.direction.x};
        const hbg::Vec2 along = Scale(particle.direction, particle.half_length);
        const hbg::Vec2 across = Scale(side, particle.half_width);
        const hbg::Vec2 corners[4] = {
            Subtract(Subtract(particle.position, along), across),
            Add(Subtract(particle.position, across), along),
            Add(Add(particle.position, along), across),
            Add(Subtract(particle.position, along), across),
        };
        for (std::uint32_t corner = 0; corner < 4U; ++corner) {
            const std::size_t vertex_index = particle_index * 4U + corner;
            const hbg::Vec3& base = mesh.vertices[vertex_index].position;
            const hbg::Vec2 target_position =
                Add(particle.position, Scale(Subtract(corners[corner], particle.position), fade));
            hbg::Vec3 delta{
                target_position.x - base.x,
                0.0f,
                target_position.y - base.z,
            };
            max_displacement = std::max(max_displacement, Length(hbg::Vec2{delta.x, delta.z}));
            target.positionDeltas[vertex_index] = delta;
        }
        min_fade = std::min(min_fade, fade);
        max_fade = std::max(max_fade, fade);
    }
    if (!std::isfinite(min_fade)) {
        min_fade = 0.0f;
    }
    stats.foam_min_fade = min_fade;
    stats.foam_max_fade = max_fade;
    stats.foam_max_displacement = max_displacement;
    return true;
}

hbg::Vec3 Subtract3(const hbg::Vec3& lhs, const hbg::Vec3& rhs) {
    return hbg::Vec3{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

hbg::Vec3 Scale3(const hbg::Vec3& value, float factor) {
    return hbg::Vec3{value.x * factor, value.y * factor, value.z * factor};
}

float Dot3(const hbg::Vec3& lhs, const hbg::Vec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

hbg::Vec3 Cross3(const hbg::Vec3& lhs, const hbg::Vec3& rhs) {
    return hbg::Vec3{lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
                     lhs.x * rhs.y - lhs.y * rhs.x};
}

void AddTo(hbg::Vec3& target, const hbg::Vec3& value) {
    target.x += value.x;
    target.y += value.y;
    target.z += value.z;
}

hbg::Vec3 Normalize3(const hbg::Vec3& value, const hbg::Vec3& fallback) {
    const float magnitude = std::sqrt(Dot3(value, value));
    if (magnitude <= 1.0e-8f) {
        return fallback;
    }
    return hbg::Vec3{value.x / magnitude, value.y / magnitude, value.z / magnitude};
}

bool ComputeTerrainDelta(const Graph& graph, const MeshSettings& mesh,
                         const RiverSettings& settings,
                         const std::vector<hbg::asset::MeshVertex>& vertices,
                         std::vector<float>& delta) {
    delta.clear();
    if (!settings.deform || vertices.empty() || graph.axis.size() < 2 || graph.width <= 0.0f ||
        settings.bed_depth <= 0.0f) {
        return false;
    }
    const float half_width = graph.width * 0.5f;
    const float bed_half_width = half_width * std::max(settings.bed_width_factor, 0.0f);
    const float bank_half_width = half_width * std::max(settings.bank_width_factor, 0.0f);
    if (bank_half_width <= bed_half_width + 1.0e-4f) {
        return false;
    }
    const float bed_floor = mesh.base_height - settings.bed_depth;
    delta.assign(vertices.size(), 0.0f);
    bool any_changed = false;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const hbg::Vec3& base = vertices[index].position;
        const float distance = DistanceToAxis(graph.axis, hbg::Vec2{base.x, base.z});
        if (distance >= bank_half_width) {
            continue;
        }
        const float weight = distance <= bed_half_width
                                 ? 1.0f
                                 : 1.0f - SmoothStep01((distance - bed_half_width) /
                                                       (bank_half_width - bed_half_width));
        const float offset = (bed_floor - base.y) * weight;
        if ((!settings.fill_bed && offset >= 0.0f) || std::abs(offset) <= 1.0e-5f) {
            continue;
        }
        delta[index] = offset;
        any_changed = true;
    }
    return any_changed;
}

bool ApplyTerrainDelta(const std::vector<hbg::asset::MeshVertex>& base_vertices,
                       const std::vector<float>& delta, const std::vector<std::uint32_t>& indices,
                       std::vector<hbg::asset::MeshVertex>& vertices) {
    if (base_vertices.empty() || base_vertices.size() != delta.size()) {
        return false;
    }
    vertices = base_vertices;
    std::vector<std::uint8_t> changed(vertices.size(), 0);
    bool any_changed = false;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (std::abs(delta[index]) <= 1.0e-5f) {
            continue;
        }
        vertices[index].position.y = base_vertices[index].position.y + delta[index];
        changed[index] = 1;
        any_changed = true;
    }
    if (!any_changed) {
        return false;
    }
    if (indices.empty()) {
        return true;
    }

    std::vector<std::uint8_t> normal_affected = changed;
    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        const std::uint32_t ia = indices[index];
        const std::uint32_t ib = indices[index + 1];
        const std::uint32_t ic = indices[index + 2];
        if (ia >= changed.size() || ib >= changed.size() || ic >= changed.size()) {
            continue;
        }
        if (changed[ia] == 0 && changed[ib] == 0 && changed[ic] == 0) {
            continue;
        }
        normal_affected[ia] = 1;
        normal_affected[ib] = 1;
        normal_affected[ic] = 1;
    }
    std::vector<hbg::Vec3> normals(vertices.size(), hbg::Vec3{0.0f, 0.0f, 0.0f});
    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        const std::uint32_t ia = indices[index];
        const std::uint32_t ib = indices[index + 1];
        const std::uint32_t ic = indices[index + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
            continue;
        }
        const hbg::Vec3 normal = Cross3(Subtract3(vertices[ib].position, vertices[ia].position),
                                        Subtract3(vertices[ic].position, vertices[ia].position));
        AddTo(normals[ia], normal);
        AddTo(normals[ib], normal);
        AddTo(normals[ic], normal);
    }
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (normal_affected[index] != 0) {
            vertices[index].normal = Normalize3(normals[index], base_vertices[index].normal);
        }
    }

    std::vector<hbg::Vec3> tangent_sums(vertices.size(), hbg::Vec3{0.0f, 0.0f, 0.0f});
    std::vector<std::uint8_t> tangent_affected(vertices.size(), 0);
    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        const std::uint32_t ia = indices[index];
        const std::uint32_t ib = indices[index + 1];
        const std::uint32_t ic = indices[index + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size() ||
            (normal_affected[ia] == 0 && normal_affected[ib] == 0 && normal_affected[ic] == 0)) {
            continue;
        }
        const hbg::Vec3 edge1 = Subtract3(vertices[ib].position, vertices[ia].position);
        const hbg::Vec3 edge2 = Subtract3(vertices[ic].position, vertices[ia].position);
        const hbg::Vec2 duv1{vertices[ib].texcoord0.x - vertices[ia].texcoord0.x,
                             vertices[ib].texcoord0.y - vertices[ia].texcoord0.y};
        const hbg::Vec2 duv2{vertices[ic].texcoord0.x - vertices[ia].texcoord0.x,
                             vertices[ic].texcoord0.y - vertices[ia].texcoord0.y};
        const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (std::abs(determinant) <= 1.0e-8f) {
            continue;
        }
        const hbg::Vec3 tangent =
            Scale3(Subtract3(Scale3(edge1, duv2.y), Scale3(edge2, duv1.y)), 1.0f / determinant);
        AddTo(tangent_sums[ia], tangent);
        AddTo(tangent_sums[ib], tangent);
        AddTo(tangent_sums[ic], tangent);
        tangent_affected[ia] = 1;
        tangent_affected[ib] = 1;
        tangent_affected[ic] = 1;
    }
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (tangent_affected[index] == 0) {
            continue;
        }
        const hbg::Vec3 normal = vertices[index].normal;
        const hbg::Vec3 projected =
            Subtract3(tangent_sums[index], Scale3(normal, Dot3(normal, tangent_sums[index])));
        hbg::Vec3 fallback{base_vertices[index].tangent.x, base_vertices[index].tangent.y,
                           base_vertices[index].tangent.z};
        if (Dot3(fallback, fallback) <= 1.0e-8f) {
            fallback = hbg::Vec3{1.0f, 0.0f, 0.0f};
        }
        const hbg::Vec3 tangent = Normalize3(projected, fallback);
        const float sign = base_vertices[index].tangent.w < 0.0f ? -1.0f : 1.0f;
        vertices[index].tangent = hbg::Vec4{tangent.x, tangent.y, tangent.z, sign};
    }
    return true;
}

bool ReadFloat(const parcel::assets::JsonValue& object, const char* key, float& value,
               std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsNumber() || !std::isfinite(node->number_value)) {
        error = std::string("river.") + key + " must be a finite number";
        return false;
    }
    value = static_cast<float>(node->number_value);
    return true;
}

bool ReadUint(const parcel::assets::JsonValue& object, const char* key, std::uint32_t& value,
              std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsNumber() || !std::isfinite(node->number_value) || node->number_value < 0.0) {
        error = std::string("river.") + key + " must be a non-negative number";
        return false;
    }
    value = static_cast<std::uint32_t>(node->number_value);
    return true;
}

bool ReadBool(const parcel::assets::JsonValue& object, const char* key, bool& value,
              std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsBool()) {
        error = std::string("river.") + key + " must be a boolean";
        return false;
    }
    value = node->bool_value;
    return true;
}

bool ReadString(const parcel::assets::JsonValue& object, const char* key, std::string& value,
                std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsString()) {
        error = std::string("river.") + key + " must be a string";
        return false;
    }
    value = node->string_value;
    return true;
}

bool ReadColor(const parcel::assets::JsonValue& object, const char* key, hbg::Vec4& value,
               std::string& error) {
    const parcel::assets::JsonValue* node = object.Find(key);
    if (node == nullptr) {
        return true;
    }
    if (!node->IsArray() || node->array_value.size() < 3 || node->array_value.size() > 4) {
        error = std::string("river.") + key + " must be a color array";
        return false;
    }
    for (const parcel::assets::JsonValue& component : node->array_value) {
        if (!component.IsNumber() || !std::isfinite(component.number_value)) {
            error = std::string("river.") + key + " must contain finite numbers";
            return false;
        }
    }
    value = hbg::Vec4{static_cast<float>(node->array_value[0].number_value),
                      static_cast<float>(node->array_value[1].number_value),
                      static_cast<float>(node->array_value[2].number_value),
                      node->array_value.size() > 3
                          ? static_cast<float>(node->array_value[3].number_value)
                          : 1.0f};
    return true;
}

bool ParseSettings(const parcel::assets::JsonValue& object, RiverSettings& settings,
                   std::string& error) {
    if (!ReadFloat(object, "length", settings.length, error) ||
        !ReadFloat(object, "width", settings.width, error) ||
        !ReadFloat(object, "amplitude", settings.amplitude, error) ||
        !ReadFloat(object, "waves", settings.waves, error) ||
        !ReadUint(object, "segments", settings.segments, error) ||
        !ReadUint(object, "crossSegments", settings.cross_segments, error) ||
        !ReadFloat(object, "baseHeight", settings.base_height, error) ||
        !ReadFloat(object, "uvLength", settings.uv_length, error) ||
        !ReadString(object, "material", settings.material_name, error) ||
        !ReadColor(object, "baseColor", settings.base_color, error) ||
        !ReadColor(object, "materialTint", settings.material_tint, error) ||
        !ReadFloat(object, "metallic", settings.metallic, error) ||
        !ReadFloat(object, "roughness", settings.roughness, error) ||
        !ReadBool(object, "deform", settings.deform, error) ||
        !ReadBool(object, "terrainFillBed", settings.fill_bed, error) ||
        !ReadString(object, "terrain", settings.terrain_mesh_name, error) ||
        !ReadFloat(object, "terrainCarveDepth", settings.bed_depth, error) ||
        !ReadFloat(object, "terrainBedWidth", settings.bed_width_factor, error) ||
        !ReadFloat(object, "terrainBankWidth", settings.bank_width_factor, error) ||
        !ReadUint(object, "textureSize", settings.texture_size, error) ||
        !ReadUint(object, "waveTextureSize", settings.wave_tile_size, error) ||
        !ReadUint(object, "waveTimeLoop", settings.wave_time_loop, error) ||
        !ReadFloat(object, "waveTimeStep", settings.wave_time_step, error) ||
        !ReadFloat(object, "waveSlope", settings.wave_slope, error) ||
        !ReadFloat(object, "waveSlopeFactor", settings.wave_slope_factor, error) ||
        !ReadFloat(object, "particleWaveLength", settings.wave_length, error) ||
        !ReadFloat(object, "particleSpeed", settings.particles.speed_scale, error) ||
        !ReadFloat(object, "particleRadius", settings.particles.screen_radius, error) ||
        !ReadFloat(object, "particleFadeIn", settings.particles.fade_in_seconds, error) ||
        !ReadFloat(object, "particleActive", settings.particles.active_seconds, error) ||
        !ReadFloat(object, "particleFadeOut", settings.particles.fade_out_seconds, error) ||
        !ReadUint(object, "particleCapacity", settings.particles.max_particles, error) ||
        !ReadUint(object, "foamTextureSize", settings.particles.texture_size, error)) {
        return false;
    }
    if (!std::isfinite(settings.length) || settings.length <= 0.0f ||
        !std::isfinite(settings.width) || settings.width <= 0.0f) {
        error = "river length and width must be positive";
        return false;
    }
    settings.segments = std::clamp(settings.segments, 2u, 4096u);
    settings.cross_segments = std::clamp(settings.cross_segments, 1u, 128u);
    settings.uv_length = std::max(settings.uv_length, 1.0e-3f);
    settings.metallic = std::clamp(settings.metallic, 0.0f, 1.0f);
    settings.roughness = std::clamp(settings.roughness, 0.0f, 1.0f);
    settings.bed_depth = std::max(settings.bed_depth, 0.0f);
    settings.bed_width_factor = std::max(settings.bed_width_factor, 0.0f);
    settings.bank_width_factor = std::max(settings.bank_width_factor, 0.0f);
    settings.texture_size = std::clamp(settings.texture_size, 32u, 512u);
    settings.wave_tile_size = std::clamp(settings.wave_tile_size, 8u, 512u);
    settings.wave_time_loop = std::clamp(settings.wave_time_loop, 1u, 16u);
    settings.wave_time_step = std::clamp(settings.wave_time_step, 0.01f, 60.0f);
    settings.wave_slope = std::clamp(settings.wave_slope, 0.0f, 1.0f);
    settings.wave_slope_factor = std::clamp(settings.wave_slope_factor, 0.0f, 16.0f);
    settings.wave_length = std::max(settings.wave_length, 1.0f);
    settings.particles.max_particles = std::clamp(settings.particles.max_particles, 0u, 1024u);
    settings.particles.speed_scale = std::clamp(settings.particles.speed_scale, 0.0f, 4000.0f);
    settings.particles.screen_radius = std::clamp(settings.particles.screen_radius, 1.0f, 64.0f);
    settings.particles.fade_in_seconds = std::max(settings.particles.fade_in_seconds, 0.01f);
    settings.particles.active_seconds = std::max(settings.particles.active_seconds, 0.0f);
    settings.particles.fade_out_seconds = std::max(settings.particles.fade_out_seconds, 0.01f);
    settings.particles.texture_size = std::clamp(settings.particles.texture_size, 32u, 128u);
    return true;
}

std::uint32_t FindMaterial(const hbg::render::RenderScene& scene, const std::string& name) {
    for (std::size_t index = 0; index < scene.materials.size(); ++index) {
        if (scene.materials[index].name == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return UINT32_MAX;
}

bool ApplyFoamLayer(hbg::render::RenderScene& scene, const Graph& graph,
                    const RiverSettings& settings,
                    std::vector<MaterializedTexture>& material_images, RiverStats& stats,
                    RiverFoamBinding& binding, std::string* error) {
    if (!settings.particles.enabled || settings.particles.max_particles == 0) {
        return true;
    }
    const std::vector<RiverFoamParticle> particles = BuildRiverFoamParticles(graph, settings);
    if (particles.empty()) {
        return true;
    }
    if (material_images.size() != scene.images.size()) {
        if (error != nullptr) {
            *error = "river foam image storage does not match the render scene";
        }
        return false;
    }
    const std::uint32_t texture_size = std::clamp(settings.particles.texture_size, 32U, 128U);
    const RiverFoamPixels foam_pixels = BuildRiverFoamPixels(texture_size, settings.particles.seed);
    stats.foam_texture_image_index = AppendRiverImage(scene, material_images, "RiverFoam",
                                                      foam_pixels.size, foam_pixels.pixels, true);
    stats.foam_texture_size = foam_pixels.size;

    std::uint32_t material_index = FindMaterial(scene, "M_RiverFoam");
    if (material_index == UINT32_MAX) {
        hbg::asset::Material material;
        material.name = "M_RiverFoam";
        material_index = static_cast<std::uint32_t>(scene.materials.size());
        scene.materials.push_back(std::move(material));
    }
    hbg::asset::Material& material = scene.materials[material_index];
    material.type = hbg::asset::MaterialType::Lit;
    material.alphaMode = hbg::asset::AlphaMode::Mask;
    material.renderQueue = hbg::asset::kRenderQueueAlphaTest;
    material.alphaCutoff = 0.2f;
    material.doubleSided = true;
    material.baseColorFactor = hbg::Vec4{0.86f, 0.94f, 1.0f, 0.85f};
    material.metallic = 0.0f;
    material.roughness = 0.92f;
    hbg::asset::TextureSampler sampler;
    sampler.wrapS = hbg::asset::TextureWrap::Clamp;
    sampler.wrapT = hbg::asset::TextureWrap::Clamp;
    sampler.mipFilter = hbg::asset::TextureMipFilter::Linear;
    material.baseColorTexture =
        hbg::asset::TextureRef{static_cast<int>(stats.foam_texture_image_index), 0, sampler};

    float min_fade = 0.0f;
    float max_fade = 0.0f;
    hbg::asset::Mesh mesh =
        BuildRiverFoamMesh(particles, settings, material_index, min_fade, max_fade);
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        if (error != nullptr) {
            *error = "river foam mesh is empty";
        }
        return false;
    }
    hbg::asset::MorphTarget target;
    target.name = "RiverFoamFlow";
    target.positionDeltas.assign(mesh.vertices.size(), hbg::Vec3{});
    const std::uint32_t target_index = static_cast<std::uint32_t>(mesh.morphTargets.size());
    mesh.morphTargets.push_back(std::move(target));

    hbg::render::RenderObject object;
    object.name = "RiverFoam";
    object.meshIndex = static_cast<std::uint32_t>(scene.meshes.size());
    object.worldBounds = mesh.bounds;
    object.morphWeights.assign(1, 0.0f);
    object.morphWeights[0] = 1.0f;
    stats.foam_particle_count = static_cast<std::uint32_t>(particles.size());
    stats.foam_mesh_index = object.meshIndex;
    stats.foam_material_index = material_index;
    stats.foam_vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    stats.foam_index_count = static_cast<std::uint32_t>(mesh.indices.size());
    stats.foam_min_fade = min_fade;
    stats.foam_max_fade = max_fade;
    stats.foam_dynamic = true;
    stats.foam_morph_target_index = target_index;
    binding.particles = std::move(particles);
    binding.mesh_index = object.meshIndex;
    binding.object_index = static_cast<std::uint32_t>(scene.objects.size());
    binding.target_index = target_index;
    scene.meshes.push_back(std::move(mesh));
    stats.foam_object_index = binding.object_index;
    scene.objects.push_back(std::move(object));
    if (!WriteRiverFoamMorph(scene, binding, stats)) {
        if (error != nullptr) {
            *error = "river foam morph target is invalid";
        }
        return false;
    }
    stats.foam_generated = true;
    return true;
}

void UpdateObjectBounds(hbg::render::RenderScene& scene, std::uint32_t mesh_index,
                        const hbg::Bounds& bounds) {
    for (hbg::render::RenderObject& object : scene.objects) {
        if (object.meshIndex == mesh_index) {
            object.worldBounds = hbg::transformBounds(object.world, bounds);
        }
    }
}

bool ApplyTerrainDeformation(hbg::render::RenderScene& scene, const Graph& graph,
                             const MeshSettings& mesh_settings, const RiverSettings& settings,
                             RiverStats& stats) {
    if (!settings.deform || settings.terrain_mesh_name.empty()) {
        return false;
    }
    std::uint32_t terrain_mesh_index = UINT32_MAX;
    for (std::size_t index = 0; index < scene.meshes.size(); ++index) {
        if (scene.meshes[index].name == settings.terrain_mesh_name) {
            terrain_mesh_index = static_cast<std::uint32_t>(index);
            break;
        }
    }
    if (terrain_mesh_index == UINT32_MAX) {
        stats.terrain_mesh_name = settings.terrain_mesh_name;
        return false;
    }
    hbg::asset::Mesh& terrain = scene.meshes[terrain_mesh_index];
    stats.terrain_mesh_name = terrain.name;
    stats.terrain_vertex_count = static_cast<std::uint32_t>(terrain.vertices.size());
    std::vector<float> delta;
    if (!ComputeTerrainDelta(graph, mesh_settings, settings, terrain.vertices, delta)) {
        return false;
    }
    float min_delta = 0.0f;
    float max_delta = 0.0f;
    std::uint32_t changed = 0;
    for (float value : delta) {
        if (std::abs(value) <= 1.0e-5f) {
            continue;
        }
        min_delta = changed == 0 ? value : std::min(min_delta, value);
        max_delta = changed == 0 ? value : std::max(max_delta, value);
        ++changed;
    }
    if (changed == 0) {
        return false;
    }
    std::vector<hbg::asset::MeshVertex> deformed;
    if (!ApplyTerrainDelta(terrain.vertices, delta, terrain.indices, deformed)) {
        return false;
    }
    terrain.vertices = std::move(deformed);
    terrain.bounds = ComputeBounds(terrain.vertices);
    for (hbg::asset::SubMesh& sub_mesh : terrain.submeshes) {
        sub_mesh.bounds = terrain.bounds;
    }
    UpdateObjectBounds(scene, terrain_mesh_index, terrain.bounds);
    stats.terrain_deformed = true;
    stats.terrain_changed_vertex_count = changed;
    stats.terrain_min_delta = min_delta;
    stats.terrain_max_delta = max_delta;
    return true;
}

} // namespace

struct RiverScene::Impl {
    RiverSettings settings;
    RiverFoamBinding foam;
    RiverFlowField flow;
    RiverView view;
    std::vector<std::uint32_t> surface_mesh_indices;
    std::uint32_t surface_object_index = UINT32_MAX;
    std::uint32_t foam_random_seed = 0x9e3779b9U;
    float normal_frame_time = 0.0f;
    bool foam_screen_space = false;
};

RiverScene::RiverScene() = default;
RiverScene::~RiverScene() = default;
RiverScene::RiverScene(RiverScene&&) noexcept = default;
RiverScene& RiverScene::operator=(RiverScene&&) noexcept = default;

float RiverLifecycleFade(float age, float fade_in, float active, float fade_out) {
    const float clamped_age = std::max(age, 0.0f);
    const float fade_in_end = std::max(fade_in, 0.0f);
    const float fade_out_start = fade_in_end + std::max(active, 0.0f);
    if (fade_in_end > 0.0f && clamped_age < fade_in_end) {
        return std::clamp(clamped_age / fade_in_end, 0.0f, 1.0f);
    }
    if (clamped_age < fade_out_start) {
        return 1.0f;
    }
    if (fade_out <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(1.0f - (clamped_age - fade_out_start) / fade_out, 0.0f, 1.0f);
}

bool RiverScene::Apply(hbg::render::RenderScene& scene, std::string_view sidecar_json,
                       std::string* error, std::vector<MaterializedTexture>* material_images) {
    stats_ = {};
    auto parsed = parcel::assets::ParseJson(sidecar_json);
    if (!parsed) {
        if (error != nullptr) {
            *error = "river sidecar json: " + parsed.GetError().message;
        }
        return false;
    }
    const parcel::assets::JsonValue* river = parsed.Value().Find("river");
    if (river == nullptr) {
        river = &parsed.Value();
    }
    if (!river->IsObject()) {
        if (error != nullptr) {
            *error = "river must be an object";
        }
        return false;
    }
    RiverSettings settings;
    std::string parse_error;
    if (!ParseSettings(*river, settings, parse_error)) {
        if (error != nullptr) {
            *error = "river sidecar: " + parse_error;
        }
        return false;
    }
    return Apply(scene, settings, error, material_images);
}

bool RiverScene::Apply(hbg::render::RenderScene& scene, const RiverSettings& settings,
                       std::string* error, std::vector<MaterializedTexture>* material_images) {
    impl_.reset();
    stats_ = {};
    if (!std::isfinite(settings.length) || settings.length <= 0.0f ||
        !std::isfinite(settings.width) || settings.width <= 0.0f) {
        if (error != nullptr) {
            *error = "river length and width must be positive";
        }
        return false;
    }
    const Graph graph = MeanderGraph(settings.length, settings.width, settings.amplitude,
                                     settings.waves, settings.segments);
    if (graph.axis.size() < 2 || graph.left_bank.points.empty() ||
        graph.right_bank.points.empty()) {
        if (error != nullptr) {
            *error = "river graph is empty";
        }
        return false;
    }

    MeshSettings mesh_settings;
    mesh_settings.segments = std::clamp(settings.segments, 2u, 4096u);
    mesh_settings.cross_segments = std::clamp(settings.cross_segments, 1u, 128u);
    mesh_settings.base_height = settings.base_height;
    mesh_settings.uv_length = std::max(settings.uv_length, 1.0e-3f);

    const bool textured = material_images != nullptr;
    std::uint32_t base_color_image_index = UINT32_MAX;
    std::vector<std::uint32_t> normal_image_indices;
    RiverTextureFrames texture_frames;
    if (textured) {
        if (material_images->size() != scene.images.size()) {
            if (error != nullptr) {
                *error = "river material image storage does not match the render scene";
            }
            return false;
        }
        texture_frames = BuildRiverTextureFrames(settings);
        if (texture_frames.size == 0 || texture_frames.base_color.empty() ||
            texture_frames.normals.empty()) {
            if (error != nullptr) {
                *error = "river texture generation failed";
            }
            return false;
        }
        base_color_image_index =
            AppendRiverImage(scene, *material_images, "RiverBaseColor", texture_frames.size,
                             texture_frames.base_color, true);
        normal_image_indices.reserve(texture_frames.normals.size());
        for (std::size_t frame = 0; frame < texture_frames.normals.size(); ++frame) {
            const std::string name =
                frame == 0 ? "RiverNormal" : "RiverNormalFrame" + std::to_string(frame);
            normal_image_indices.push_back(AppendRiverImage(scene, *material_images, name,
                                                            texture_frames.size,
                                                            texture_frames.normals[frame], false));
        }
        stats_.base_color_image_index = base_color_image_index;
        stats_.normal_image_index = normal_image_indices.front();
        stats_.textures_generated = true;
        stats_.texture_size = texture_frames.size;
    }

    hbg::asset::TextureSampler sampler;
    sampler.wrapS = hbg::asset::TextureWrap::Repeat;
    sampler.wrapT = hbg::asset::TextureWrap::Repeat;
    sampler.mipFilter = hbg::asset::TextureMipFilter::Linear;
    const std::uint32_t frame_count =
        textured ? static_cast<std::uint32_t>(normal_image_indices.size()) : 1U;
    std::vector<std::uint32_t> material_indices;
    material_indices.reserve(frame_count);
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        const std::string material_name =
            frame == 0 ? settings.material_name
                       : settings.material_name + ".frame" + std::to_string(frame);
        std::uint32_t material_index = FindMaterial(scene, material_name);
        if (material_index == UINT32_MAX) {
            hbg::asset::Material material;
            material.name = material_name;
            material_index = static_cast<std::uint32_t>(scene.materials.size());
            scene.materials.push_back(std::move(material));
        }
        hbg::asset::Material& material = scene.materials[material_index];
        material.type = hbg::asset::MaterialType::Lit;
        material.alphaMode = hbg::asset::AlphaMode::Blend;
        material.renderQueue = hbg::asset::kRenderQueueTransparent;
        material.doubleSided = true;
        material.baseColorFactor = hbg::Vec4{settings.base_color.x * settings.material_tint.x,
                                             settings.base_color.y * settings.material_tint.y,
                                             settings.base_color.z * settings.material_tint.z,
                                             settings.base_color.w * settings.material_tint.w};
        material.metallic = std::clamp(settings.metallic, 0.0f, 1.0f);
        material.roughness = std::clamp(settings.roughness, 0.0f, 1.0f);
        material.normalScale = 0.75f;
        if (textured) {
            material.baseColorTexture =
                hbg::asset::TextureRef{static_cast<int>(base_color_image_index), 0, sampler};
            material.normalTexture =
                hbg::asset::TextureRef{static_cast<int>(normal_image_indices[frame]), 0, sampler};
            material.baseColorFactor =
                hbg::Vec4{1.0f, 1.0f, 1.0f, std::min(settings.base_color.w, 0.9f)};
            material.normalScale = 0.35f;
        } else {
            material.baseColorTexture = {};
            material.normalTexture = {};
        }
        material_indices.push_back(material_index);
    }
    stats_.material_index = material_indices.front();
    stats_.normal_frame_count = frame_count;
    stats_.normal_frame_index = 0;
    stats_.normal_frame_time = 0.0f;
    stats_.normal_frame_step = std::clamp(settings.wave_time_step, 0.01f, 60.0f);

    hbg::asset::Mesh mesh = BuildMesh(graph, mesh_settings, material_indices.front());
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        if (error != nullptr) {
            *error = "river mesh is empty";
        }
        return false;
    }

    std::vector<std::uint32_t> surface_mesh_indices;
    surface_mesh_indices.reserve(frame_count);
    stats_.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    stats_.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        hbg::asset::Mesh frame_mesh = mesh;
        frame_mesh.name =
            frame == 0 ? "RiverSurface" : "RiverSurface.Frame" + std::to_string(frame);
        for (hbg::asset::SubMesh& sub_mesh : frame_mesh.submeshes) {
            sub_mesh.materialIndex = material_indices[frame];
        }
        surface_mesh_indices.push_back(static_cast<std::uint32_t>(scene.meshes.size()));
        scene.meshes.push_back(std::move(frame_mesh));
    }

    hbg::render::RenderObject object;
    object.name = "RiverSurface";
    object.meshIndex = surface_mesh_indices.front();
    object.worldBounds = mesh.bounds;
    stats_.mesh_index = object.meshIndex;
    stats_.object_index = static_cast<std::uint32_t>(scene.objects.size());
    scene.objects.push_back(std::move(object));

    float min_speed = std::numeric_limits<float>::infinity();
    float max_speed = 0.0f;
    std::uint32_t flow_samples = 0;
    for (std::size_t index = 1; index + 1 < graph.axis.size(); ++index) {
        const FlowSample sample = SampleFlow(graph, graph.axis[index]);
        if (!sample.inside) {
            continue;
        }
        const float speed = Length(sample.velocity);
        min_speed = std::min(min_speed, speed);
        max_speed = std::max(max_speed, speed);
        ++flow_samples;
    }
    stats_.flow_samples = flow_samples;
    stats_.min_speed = flow_samples == 0 ? 0.0f : min_speed;
    stats_.max_speed = flow_samples == 0 ? 0.0f : max_speed;

    RiverFoamBinding foam;
    if (material_images != nullptr) {
        if (!ApplyFoamLayer(scene, graph, settings, *material_images, stats_, foam, error)) {
            return false;
        }
    }
    ApplyTerrainDeformation(scene, graph, mesh_settings, settings, stats_);
    RecomputeSceneBounds(scene);
    auto impl = std::make_unique<RiverScene::Impl>();
    impl->settings = settings;
    impl->foam = std::move(foam);
    impl->flow = BuildFlowField(graph);
    impl->foam_random_seed = settings.particles.seed;
    impl->surface_mesh_indices = std::move(surface_mesh_indices);
    impl->surface_object_index = stats_.object_index;
    impl_ = std::move(impl);
    stats_.active = true;
    return true;
}

bool RiverScene::Update(hbg::render::RenderScene& scene, float delta_seconds,
                        const RiverView* view) {
    if (!impl_ || !stats_.active) {
        return false;
    }
    const float dt = std::clamp(delta_seconds, 0.0f, 0.1f);
    if (dt <= 0.0f) {
        return false;
    }
    if (view != nullptr && view->valid()) {
        impl_->view = *view;
    }
    bool updated = false;
    impl_->normal_frame_time += dt;
    stats_.normal_frame_time = impl_->normal_frame_time;
    if (stats_.normal_frame_count > 1 && impl_->surface_object_index < scene.objects.size() &&
        impl_->surface_mesh_indices.size() == stats_.normal_frame_count) {
        const float step = std::max(stats_.normal_frame_step, 1.0e-3f);
        const std::uint32_t frame =
            static_cast<std::uint32_t>(std::floor((impl_->normal_frame_time + 1.0e-4f) / step)) %
            stats_.normal_frame_count;
        if (frame != stats_.normal_frame_index) {
            scene.objects[impl_->surface_object_index].meshIndex =
                impl_->surface_mesh_indices[frame];
            stats_.normal_frame_index = frame;
            updated = true;
        }
    }
    if (!stats_.foam_dynamic) {
        return updated;
    }
    RiverFoamBinding& foam = impl_->foam;
    if (foam.mesh_index >= scene.meshes.size() || foam.target_index == UINT32_MAX ||
        foam.particles.empty()) {
        return false;
    }
    const RiverParticleSettings& particle_settings = impl_->settings.particles;
    const float speed_scale = std::max(particle_settings.speed_scale, 0.0f);
    if (!impl_->foam_screen_space && impl_->view.valid()) {
        std::uint32_t screen_count = 0;
        RiverFoamRandom random(impl_->settings.particles.seed);
        if (SeedRiverFoamScreenSpace(impl_->settings, impl_->flow, impl_->view, foam.particles,
                                     screen_count, random)) {
            impl_->foam_screen_space = true;
            impl_->foam_random_seed = RiverPerlinHash(impl_->settings.particles.seed + 0x9e3779b9U);
            stats_.foam_screen_space = true;
            stats_.foam_screen_particle_count = screen_count;
            updated = true;
        }
    }
    RiverFoamRandom random(impl_->foam_random_seed);
    impl_->foam_random_seed += 0x9e3779b9U;
    if (impl_->foam_screen_space) {
        for (RiverFoamParticle& particle : foam.particles) {
            if (!particle.active) {
                continue;
            }
            particle.age += dt;
            bool respawn = particle.life <= 1.0e-6f || particle.age >= particle.life;
            if (!respawn) {
                const FlowSample sample =
                    SampleFlowField(impl_->flow, particle.position, particle.segment_hint);
                if (!sample.inside) {
                    respawn = true;
                } else {
                    const float speed = Length(sample.velocity);
                    if (speed > 1.0e-6f) {
                        particle.direction = Scale(sample.velocity, 1.0f / speed);
                    }
                    const hbg::Vec2 next =
                        Add(particle.position, Scale(sample.velocity, dt * speed_scale));
                    if (SampleFlowField(impl_->flow, next, particle.segment_hint).inside) {
                        particle.position = next;
                    } else {
                        respawn = true;
                    }
                }
            }
            if (!respawn) {
                hbg::Vec2 screen;
                if (!ProjectRiverPoint(impl_->view,
                                       hbg::Vec3{particle.position.x, impl_->settings.base_height,
                                                 particle.position.y},
                                       screen) ||
                    screen.x < 0.0f || screen.x >= static_cast<float>(impl_->view.viewport_width) ||
                    screen.y < 0.0f ||
                    screen.y >= static_cast<float>(impl_->view.viewport_height)) {
                    respawn = true;
                }
            }
            if (respawn && !RespawnRiverFoamParticle(impl_->settings, impl_->flow, impl_->view,
                                                     random, particle)) {
                particle.active = false;
                particle.fade = 0.0f;
                continue;
            }
            particle.fade = RiverLifecycleFade(particle.age, particle_settings.fade_in_seconds,
                                               particle_settings.active_seconds,
                                               particle_settings.fade_out_seconds);
        }
        std::uint32_t active_count = 0;
        for (const RiverFoamParticle& particle : foam.particles) {
            active_count += particle.active ? 1U : 0U;
        }
        stats_.foam_screen_particle_count = active_count;
    } else {
        for (RiverFoamParticle& particle : foam.particles) {
            particle.age += dt;
            if (particle.life <= 1.0e-6f || particle.age >= particle.life) {
                particle.age =
                    particle.life > 1.0e-6f ? std::fmod(particle.age, particle.life) : 0.0f;
                particle.position = particle.spawn_position;
                particle.segment_hint = particle.spawn_segment;
                particle.direction = particle.spawn_direction;
            }
            const FlowSample sample =
                SampleFlowField(impl_->flow, particle.position, particle.segment_hint);
            if (sample.inside) {
                const float speed = Length(sample.velocity);
                if (speed > 1.0e-6f) {
                    particle.direction = Scale(sample.velocity, 1.0f / speed);
                }
                const hbg::Vec2 next =
                    Add(particle.position, Scale(sample.velocity, dt * speed_scale));
                if (SampleFlowField(impl_->flow, next, particle.segment_hint).inside) {
                    particle.position = next;
                } else {
                    particle.position = particle.spawn_position;
                    particle.segment_hint = particle.spawn_segment;
                    particle.direction = particle.spawn_direction;
                }
            } else {
                particle.position = particle.spawn_position;
                particle.segment_hint = particle.spawn_segment;
                particle.direction = particle.spawn_direction;
            }
            particle.fade = RiverLifecycleFade(particle.age, particle_settings.fade_in_seconds,
                                               particle_settings.active_seconds,
                                               particle_settings.fade_out_seconds);
        }
    }
    if (!WriteRiverFoamMorph(scene, foam, stats_)) {
        return false;
    }
    stats_.foam_updates += 1;
    stats_.foam_time += dt;
    return true;
}

void RiverScene::Close() {
    impl_.reset();
    stats_ = {};
}

} // namespace parcel_hbg
