#include "parcel/hbg/water_surface.hpp"

#include "asset/mesh.h"
#include "core/math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace parcel_hbg {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kGravity = 9.81f;
constexpr float kCm = 0.23f;
constexpr float kKm = 370.0f;

float Square(float value) { return value * value; }

hbg::Vec4 TransformVec4(const hbg::Mat4& matrix, const hbg::Vec4& value) {
    return hbg::Vec4(matrix.values[0] * value.x + matrix.values[4] * value.y +
                         matrix.values[8] * value.z + matrix.values[12] * value.w,
                     matrix.values[1] * value.x + matrix.values[5] * value.y +
                         matrix.values[9] * value.z + matrix.values[13] * value.w,
                     matrix.values[2] * value.x + matrix.values[6] * value.y +
                         matrix.values[10] * value.z + matrix.values[14] * value.w,
                     matrix.values[3] * value.x + matrix.values[7] * value.y +
                         matrix.values[11] * value.z + matrix.values[15] * value.w);
}

struct ScreenGridDesc {
    int width = 0;
    int height = 0;
};

struct ScreenGridSource {
    hbg::Bounds bounds;
    float baseHeight = 0.0f;
    hbg::Vec2 uvMin{0.0f, 0.0f};
    hbg::Vec2 uvMax{1.0f, 1.0f};
    std::uint32_t materialIndex = 0;
};

bool ConfigureScreenGrid(hbg::asset::Mesh& waterMesh, std::uint16_t viewportWidth,
                         std::uint16_t viewportHeight, ScreenGridDesc* desc,
                         const ScreenGridSource* source) {
    if (viewportWidth == 0 || viewportHeight == 0 || waterMesh.vertices.empty()) {
        return false;
    }
    constexpr float kScreenGridPixels = 8.0f;
    constexpr float kOverscan = 1.25f;
    constexpr int kMaxGridDimension = 1024;
    const int width = std::max(
        static_cast<int>(kOverscan * static_cast<float>(viewportWidth) / kScreenGridPixels), 2);
    const int height = std::max(
        static_cast<int>(kOverscan * static_cast<float>(viewportHeight) / kScreenGridPixels), 2);
    if (width > kMaxGridDimension || height > kMaxGridDimension) {
        return false;
    }

    hbg::Bounds sourceBounds = source != nullptr ? source->bounds : waterMesh.bounds;
    if (!sourceBounds.valid()) {
        sourceBounds = {};
        for (const hbg::asset::MeshVertex& vertex : waterMesh.vertices) {
            sourceBounds.expand(vertex.position);
        }
    }
    if (!sourceBounds.valid()) {
        return false;
    }
    float minU = source != nullptr ? source->uvMin.x : waterMesh.vertices.front().texcoord0.x;
    float maxU = source != nullptr ? source->uvMax.x : minU;
    float minV = source != nullptr ? source->uvMin.y : waterMesh.vertices.front().texcoord0.y;
    float maxV = source != nullptr ? source->uvMax.y : minV;
    if (source == nullptr) {
        for (const hbg::asset::MeshVertex& vertex : waterMesh.vertices) {
            minU = std::min(minU, vertex.texcoord0.x);
            maxU = std::max(maxU, vertex.texcoord0.x);
            minV = std::min(minV, vertex.texcoord0.y);
            maxV = std::max(maxV, vertex.texcoord0.y);
        }
    }
    if (maxU <= minU) {
        minU = 0.0f;
        maxU = 1.0f;
    }
    if (maxV <= minV) {
        minV = 0.0f;
        maxV = 1.0f;
    }

    const std::uint32_t materialIndex =
        source != nullptr
            ? source->materialIndex
            : (waterMesh.submeshes.empty() ? 0u : waterMesh.submeshes.front().materialIndex);
    const float baseHeight =
        source != nullptr ? source->baseHeight : waterMesh.vertices.front().position.y;
    const float minX = sourceBounds.min.x;
    const float maxX = sourceBounds.max.x;
    const float minZ = sourceBounds.min.z;
    const float maxZ = sourceBounds.max.z;
    const float inverseX = maxX > minX ? 1.0f / (maxX - minX) : 0.0f;
    const float inverseZ = maxZ > minZ ? 1.0f / (maxZ - minZ) : 0.0f;

    std::vector<hbg::asset::MeshVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int row = 0; row < height; ++row) {
        const float tz =
            height > 1 ? static_cast<float>(row) / static_cast<float>(height - 1) : 0.0f;
        const float z = minZ + (maxZ - minZ) * tz;
        for (int column = 0; column < width; ++column) {
            const float tx =
                width > 1 ? static_cast<float>(column) / static_cast<float>(width - 1) : 0.0f;
            const float x = minX + (maxX - minX) * tx;
            hbg::asset::MeshVertex vertex;
            vertex.position = hbg::Vec3{x, baseHeight, z};
            vertex.normal = hbg::Vec3{0.0f, 1.0f, 0.0f};
            vertex.tangent = hbg::Vec4{1.0f, 0.0f, 0.0f, -1.0f};
            vertex.texcoord0 = hbg::Vec2{minU + (maxU - minU) * (x - minX) * inverseX,
                                         minV + (maxV - minV) * (z - minZ) * inverseZ};
            vertices.push_back(vertex);
        }
    }

    const std::uint64_t indexCount =
        static_cast<std::uint64_t>(width - 1) * static_cast<std::uint64_t>(height - 1) * 6u;
    if (indexCount > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(indexCount));
    for (int row = 0; row < height - 1; ++row) {
        for (int column = 0; column < width - 1; ++column) {
            const std::uint32_t a = static_cast<std::uint32_t>(row * width + column);
            const std::uint32_t b = a + 1u;
            const std::uint32_t c = a + static_cast<std::uint32_t>(width);
            const std::uint32_t d = c + 1u;
            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }

    waterMesh.vertices = std::move(vertices);
    waterMesh.indices = std::move(indices);
    waterMesh.submeshes.clear();
    hbg::asset::SubMesh submesh;
    submesh.indexOffset = 0;
    submesh.indexCount = static_cast<std::uint32_t>(waterMesh.indices.size());
    submesh.materialIndex = materialIndex;
    submesh.bounds = sourceBounds;
    waterMesh.submeshes.push_back(submesh);
    waterMesh.bounds = sourceBounds;
    waterMesh.hasNormals = true;
    waterMesh.hasTangents = true;
    waterMesh.hasUv0 = true;
    waterMesh.hasUv1 = false;
    waterMesh.hasColors = false;
    if (desc != nullptr) {
        desc->width = width;
        desc->height = height;
    }
    return true;
}

struct OceanSphereHit {
    bool valid = false;
    hbg::Vec3 position{};
};

OceanSphereHit IntersectOceanSphere(const hbg::Vec3& cameraPosition, const hbg::Vec3& rayDirection,
                                    float radius, float seaLevel) {
    OceanSphereHit hit;
    const float rayLength = hbg::length(rayDirection);
    hbg::Vec3 direction = rayDirection;
    if (rayLength > 1.0e-8f) {
        direction = direction * (1.0f / rayLength);
    }
    const float altitude = cameraPosition.y - seaLevel;
    if (radius <= 0.0f || altitude <= 0.0f) {
        return hit;
    }
    const float altitudeRatio = altitude / radius;
    const float onePlus = 1.0f + altitudeRatio;
    const float horizonDown =
        std::sqrt(std::max(altitudeRatio * (2.0f + altitudeRatio), 0.0f)) / onePlus;
    if (direction.y > -horizonDown) {
        hbg::Vec3 horizontal{direction.x, 0.0f, direction.z};
        const float horizontalLength = hbg::length(horizontal);
        horizontal = horizontalLength > 1.0e-8f ? horizontal * (1.0f / horizontalLength)
                                                : hbg::Vec3{0.0f, 0.0f, -1.0f};
        direction = hbg::Vec3{horizontal.x / onePlus, -horizonDown, horizontal.z / onePlus};
    }
    const float b = direction.y * onePlus;
    const float c = altitudeRatio * (2.0f + altitudeRatio);
    const float discriminant = std::max(b * b - c, 0.0f);
    const float denominator = -b + std::sqrt(discriminant);
    if (denominator <= 1.0e-12f) {
        return hit;
    }
    const float distance = radius * c / denominator;
    hit.position = cameraPosition + direction * distance;
    hit.valid = true;
    return hit;
}

hbg::Vec2 OceanSphereSlope(const hbg::Vec3& position, const hbg::Vec3& cameraPosition, float radius,
                           float seaLevel) {
    if (radius <= 0.0f) {
        return hbg::Vec2{0.0f, 0.0f};
    }
    const float centerY = seaLevel - radius;
    const float vertical = position.y - centerY;
    if (vertical <= 1.0e-3f) {
        return hbg::Vec2{0.0f, 0.0f};
    }
    return hbg::Vec2{(position.x - cameraPosition.x) / vertical,
                     (position.z - cameraPosition.z) / vertical};
}

class OceanFFT {
  public:
    using Complex = std::complex<float>;

    struct Settings {
        float tileSize = 5488.0f;
        float amplitude = 1.0f;
        float updateHz = 12.0f;
        float sphereRadius = 0.0f;
        std::uint32_t resolution = 128;
        std::uint32_t seed = 1234;
    };

    bool Init(const Settings& settings) {
        m_settings = settings;
        m_valid = false;
        m_lastUpdateTime = -1.0f;
        m_updateCount = 0;
        const std::uint32_t resolution = m_settings.resolution;
        if (resolution < 16 || resolution > 512 || (resolution & (resolution - 1)) != 0 ||
            m_settings.amplitude < 0.0f || m_settings.updateHz <= 0.0f ||
            m_settings.tileSize <= 0.0f) {
            return false;
        }

        m_mipLevelCount = 1;
        for (std::uint32_t size = resolution; size > 1; size >>= 1) {
            ++m_mipLevelCount;
        }

        m_gridSizes = {m_settings.tileSize, m_settings.tileSize / 14.0f,
                       m_settings.tileSize / 196.0f, m_settings.tileSize / 2744.0f};
        for (std::size_t index = 0; index < m_grids.size(); ++index) {
            Grid& grid = m_grids[index];
            grid.size = m_gridSizes[index];
            const std::size_t count = static_cast<std::size_t>(resolution) * resolution;
            grid.spectrum.assign(count, Complex{});
            grid.omega.assign(count, 0.0f);
            grid.height.assign(count, 0.0f);
            grid.slopeX.assign(count, 0.0f);
            grid.slopeZ.assign(count, 0.0f);
            grid.choppyX.assign(count, 0.0f);
            grid.choppyZ.assign(count, 0.0f);
            GenerateSpectrum(grid);
        }
        m_valid = true;
        return true;
    }

    bool Update(float timeSeconds) {
        if (!m_valid) {
            return false;
        }
        const float interval = 1.0f / std::max(m_settings.updateHz, 0.001f);
        if (m_lastUpdateTime >= 0.0f && timeSeconds >= m_lastUpdateTime &&
            timeSeconds - m_lastUpdateTime < interval) {
            return false;
        }
        m_lastUpdateTime = timeSeconds;
        for (Grid& grid : m_grids) {
            UpdateGrid(grid, timeSeconds);
        }
        ++m_updateCount;
        return true;
    }

    void ApplyToVertices(std::vector<hbg::asset::MeshVertex>& vertices, float baseHeight,
                         const std::vector<hbg::asset::MeshVertex>& baseVertices) const {
        if (!m_valid || baseVertices.size() != vertices.size()) {
            return;
        }
        const float choppyScale = m_settings.amplitude * 2.5f;
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            hbg::asset::MeshVertex& vertex = vertices[index];
            const hbg::Vec3 base = baseVertices[index].position;
            const WaveSample waves = SampleWaves(base.x, base.z);
            hbg::Vec3 normal{-waves.slopeX, 1.0f, -waves.slopeZ};
            const float normalLength = hbg::length(normal);
            vertex.position.x = base.x + waves.choppyX * choppyScale;
            vertex.position.y = baseHeight + waves.height * m_settings.amplitude;
            vertex.position.z = base.z + waves.choppyZ * choppyScale;
            vertex.normal = normalLength > 1.0e-6f ? normal * (1.0f / normalLength)
                                                   : hbg::Vec3{0.0f, 1.0f, 0.0f};
        }
    }

    void ApplyToScreenGrid(std::vector<hbg::asset::MeshVertex>& vertices, float baseHeight,
                           int gridWidth, int gridHeight, const hbg::Mat4& invViewProj,
                           const hbg::Vec3& cameraPosition, const hbg::Vec2& uvScale,
                           const hbg::Vec2& uvOrigin, float sphereRadius) const {
        if (!m_valid || gridWidth < 2 || gridHeight < 2 ||
            vertices.size() !=
                static_cast<std::size_t>(gridWidth) * static_cast<std::size_t>(gridHeight)) {
            return;
        }
        constexpr float kOverscan = 1.25f;
        constexpr float kMaxDistance = 1.0e6f;
        const float denominatorX = static_cast<float>(gridWidth - 1);
        const float denominatorY = static_cast<float>(gridHeight - 1);
        const float choppyScale = m_settings.amplitude * 2.5f;
        std::vector<hbg::Vec3> bases(vertices.size());
        for (int row = 0; row < gridHeight; ++row) {
            const float ndcY =
                2.0f * kOverscan * static_cast<float>(row) / denominatorY - kOverscan;
            for (int column = 0; column < gridWidth; ++column) {
                const float ndcX =
                    2.0f * kOverscan * static_cast<float>(column) / denominatorX - kOverscan;
                const std::size_t index =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(gridWidth) +
                    static_cast<std::size_t>(column);
                const hbg::Vec4 world =
                    TransformVec4(invViewProj, hbg::Vec4{ndcX, ndcY, 1.0f, 1.0f});
                hbg::Vec3 ray{0.0f, 0.0f, -1.0f};
                if (std::abs(world.w) > 1.0e-8f) {
                    const hbg::Vec3 farPoint{world.x / world.w, world.y / world.w,
                                             world.z / world.w};
                    ray = farPoint - cameraPosition;
                    const float rayLength = hbg::length(ray);
                    if (rayLength > 1.0e-8f) {
                        ray = ray * (1.0f / rayLength);
                    }
                }

                hbg::Vec3 base{};
                bool intersects = false;
                if (sphereRadius > 0.0f) {
                    const OceanSphereHit hit =
                        IntersectOceanSphere(cameraPosition, ray, sphereRadius, baseHeight);
                    if (hit.valid) {
                        base = hit.position;
                        intersects = true;
                    }
                }
                if (!intersects && ray.y < -1.0e-5f) {
                    const float distance = (baseHeight - cameraPosition.y) / ray.y;
                    if (distance > 0.0f && distance < kMaxDistance) {
                        base = cameraPosition + ray * distance;
                        base.y = baseHeight;
                        intersects = true;
                    }
                }
                if (!intersects) {
                    hbg::Vec3 horizontal{ray.x, 0.0f, ray.z};
                    const float horizontalLength = hbg::length(horizontal);
                    horizontal = horizontalLength > 1.0e-8f ? horizontal * (1.0f / horizontalLength)
                                                            : hbg::Vec3{0.0f, 0.0f, -1.0f};
                    base = cameraPosition + horizontal * kMaxDistance;
                    base.y = baseHeight;
                }
                bases[index] = base;
            }
        }

        for (int row = 0; row < gridHeight; ++row) {
            const int rowPrev = std::max(row - 1, 0);
            const int rowNext = std::min(row + 1, gridHeight - 1);
            for (int column = 0; column < gridWidth; ++column) {
                const int columnPrev = std::max(column - 1, 0);
                const int columnNext = std::min(column + 1, gridWidth - 1);
                const std::size_t index =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(gridWidth) +
                    static_cast<std::size_t>(column);
                const hbg::Vec3 base = bases[index];
                const auto at = [&](int columnIndex, int rowIndex) {
                    return bases[static_cast<std::size_t>(rowIndex) *
                                     static_cast<std::size_t>(gridWidth) +
                                 static_cast<std::size_t>(columnIndex)];
                };
                const float footprintX = hbg::length(at(columnNext, row) - at(columnPrev, row));
                const float footprintZ = hbg::length(at(column, rowNext) - at(column, rowPrev));
                const float footprint = std::max(footprintX, footprintZ);
                const WaveSample waves = SampleWaves(base.x, base.z, footprint);
                hbg::asset::MeshVertex& vertex = vertices[index];
                const hbg::Vec2 sphereSlope =
                    OceanSphereSlope(base, cameraPosition, sphereRadius, baseHeight);
                const hbg::Vec3 normal{-waves.slopeX + sphereSlope.x, 1.0f,
                                       -waves.slopeZ + sphereSlope.y};
                const float normalLength = hbg::length(normal);
                vertex.position.x = base.x + waves.choppyX * choppyScale;
                vertex.position.y = (sphereRadius > 0.0f ? base.y : baseHeight) +
                                    waves.height * m_settings.amplitude;
                vertex.position.z = base.z + waves.choppyZ * choppyScale;
                vertex.normal = normalLength > 1.0e-6f ? normal * (1.0f / normalLength)
                                                       : hbg::Vec3{0.0f, 1.0f, 0.0f};
                vertex.tangent = hbg::Vec4{1.0f, 0.0f, 0.0f, -1.0f};
                vertex.texcoord0 =
                    hbg::Vec2{base.x * uvScale.x + uvOrigin.x, base.z * uvScale.y + uvOrigin.y};
                vertex.texcoord1 = hbg::Vec2{0.0f, 0.0f};
                vertex.color = hbg::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            }
        }
    }

    std::uint32_t updateCount() const { return m_updateCount; }

  private:
    struct Grid {
        float size = 1.0f;
        std::vector<Complex> spectrum;
        std::vector<float> omega;
        std::vector<float> height;
        std::vector<float> slopeX;
        std::vector<float> slopeZ;
        std::vector<float> choppyX;
        std::vector<float> choppyZ;
        std::array<std::vector<std::vector<float>>, 5> mips;
    };

    struct WaveSample {
        float height = 0.0f;
        float slopeX = 0.0f;
        float slopeZ = 0.0f;
        float choppyX = 0.0f;
        float choppyZ = 0.0f;
    };

    float WaveOmega(float k) const {
        if (k <= std::numeric_limits<float>::epsilon()) {
            return 0.0f;
        }
        return std::sqrt(kGravity * k * (1.0f + Square(k / kKm)));
    }

    float Spectrum(float kx, float ky, bool omnispectrum = false) const {
        const float k = std::sqrt(kx * kx + ky * ky);
        if (k <= std::numeric_limits<float>::epsilon()) {
            return 0.0f;
        }
        const float wind = 5.0f;
        const float seaState = 0.84f;
        const float c = WaveOmega(k) / k;
        const float kp = kGravity * Square(seaState / wind);
        const float cp = WaveOmega(kp) / kp;
        const float z0 = 3.7e-5f * Square(wind) / kGravity * std::pow(wind / cp, 0.9f);
        const float uStar =
            0.41f * wind / std::max(std::log(10.0f / std::max(z0, 1.0e-8f)), 1.0e-6f);
        const float lpm = std::exp(-1.25f * Square(kp / k));
        const float gamma = seaState < 1.0f ? 1.7f : 1.7f + 6.0f * std::log(seaState);
        const float sigma = 0.08f * (1.0f + 4.0f / std::pow(seaState, 3.0f));
        const float gammaTerm =
            std::exp(-1.0f / (2.0f * Square(sigma)) * Square(std::sqrt(k / kp) - 1.0f));
        const float jp = std::pow(gamma, gammaTerm);
        const float fp =
            lpm * jp * std::exp(-seaState / std::sqrt(10.0f) * (std::sqrt(k / kp) - 1.0f));
        const float alphaP = 0.006f * std::sqrt(seaState);
        const float bl = 0.5f * alphaP * cp / c * fp;
        const float alphaM = 0.01f * (uStar < kCm ? 1.0f + std::log(uStar / kCm)
                                                  : 1.0f + 3.0f * std::log(uStar / kCm));
        const float fm = std::exp(-0.25f * Square(k / kKm - 1.0f));
        const float bh = 0.5f * alphaM * kCm / c * fm * lpm;
        if (omnispectrum) {
            return (bl + bh) / (k * Square(k));
        }
        const float a0 = std::log(2.0f) / 4.0f;
        const float ap = 4.0f;
        const float am = 0.13f * uStar / kCm;
        const float delta =
            std::tanh(a0 + ap * std::pow(c / cp, 2.5f) + am * std::pow(kCm / c, 2.5f));
        const float phi = std::atan2(ky, kx);
        return (bl + bh) * (1.0f + delta * std::cos(2.0f * phi)) / (2.0f * kPi * Square(k * k));
    }

    void GenerateSpectrum(Grid& grid) {
        const int resolution = static_cast<int>(m_settings.resolution);
        const float dk = 2.0f * kPi / grid.size;
        std::mt19937 rng(m_settings.seed + static_cast<std::uint32_t>(grid.size));
        std::uniform_real_distribution<float> phaseDistribution(0.0f, 2.0f * kPi);
        const float kMin = kPi / grid.size;
        for (int y = 0; y < resolution; ++y) {
            const int j = y < resolution / 2 ? y : y - resolution;
            for (int x = 0; x < resolution; ++x) {
                const int i = x < resolution / 2 ? x : x - resolution;
                const float kx = static_cast<float>(i) * dk;
                const float ky = static_cast<float>(j) * dk;
                const float magnitude = std::sqrt(kx * kx + ky * ky);
                const std::size_t index =
                    static_cast<std::size_t>(y) * resolution + static_cast<std::size_t>(x);
                grid.omega[index] = WaveOmega(magnitude);
                if (i < 0 || (i == 0 && j <= 0)) {
                    continue;
                }
                float amplitude = 0.0f;
                if (magnitude >= kMin) {
                    amplitude = std::sqrt(std::max(Spectrum(kx, ky), 0.0f) * 0.5f) * dk;
                }
                const Complex value = std::polar(amplitude, phaseDistribution(rng));
                grid.spectrum[index] = value;
                const int mirroredX = (resolution - i) % resolution;
                const int mirroredY = (resolution - j) % resolution;
                grid.spectrum[static_cast<std::size_t>(mirroredY) * resolution + mirroredX] =
                    std::conj(value);
            }
        }
    }

    void InverseFft2(std::vector<Complex>& data) const {
        const int resolution = static_cast<int>(m_settings.resolution);
        const auto inverseFft1 = [resolution](std::vector<Complex>& line) {
            for (int i = 1, j = 0; i < resolution; ++i) {
                int bit = resolution >> 1;
                for (; (j & bit) != 0; bit >>= 1) {
                    j ^= bit;
                }
                j ^= bit;
                if (i < j) {
                    std::swap(line[static_cast<std::size_t>(i)], line[static_cast<std::size_t>(j)]);
                }
            }
            for (int length = 2; length <= resolution; length <<= 1) {
                const float angle = 2.0f * kPi / static_cast<float>(length);
                const Complex step(std::cos(angle), std::sin(angle));
                for (int offset = 0; offset < resolution; offset += length) {
                    Complex weight(1.0f, 0.0f);
                    for (int index = 0; index < length / 2; ++index) {
                        const Complex even = line[static_cast<std::size_t>(offset + index)];
                        const Complex odd =
                            line[static_cast<std::size_t>(offset + index + length / 2)] * weight;
                        line[static_cast<std::size_t>(offset + index)] = even + odd;
                        line[static_cast<std::size_t>(offset + index + length / 2)] = even - odd;
                        weight *= step;
                    }
                }
            }
        };

        std::vector<Complex> scratch(static_cast<std::size_t>(resolution));
        for (int y = 0; y < resolution; ++y) {
            for (int x = 0; x < resolution; ++x) {
                scratch[static_cast<std::size_t>(x)] =
                    data[static_cast<std::size_t>(y) * resolution + x];
            }
            inverseFft1(scratch);
            for (int x = 0; x < resolution; ++x) {
                data[static_cast<std::size_t>(y) * resolution + x] =
                    scratch[static_cast<std::size_t>(x)];
            }
        }
        for (int x = 0; x < resolution; ++x) {
            for (int y = 0; y < resolution; ++y) {
                scratch[static_cast<std::size_t>(y)] =
                    data[static_cast<std::size_t>(y) * resolution + x];
            }
            inverseFft1(scratch);
            for (int y = 0; y < resolution; ++y) {
                data[static_cast<std::size_t>(y) * resolution + x] =
                    scratch[static_cast<std::size_t>(y)];
            }
        }
    }

    void UpdateGrid(Grid& grid, float timeSeconds) const {
        const int resolution = static_cast<int>(m_settings.resolution);
        const float dk = 2.0f * kPi / grid.size;
        std::vector<Complex> work = grid.spectrum;
        std::vector<Complex> slope = grid.spectrum;
        std::vector<Complex> choppy = grid.spectrum;
        for (int y = 0; y < resolution; ++y) {
            const int j = y < resolution / 2 ? y : y - resolution;
            for (int x = 0; x < resolution; ++x) {
                const int i = x < resolution / 2 ? x : x - resolution;
                const std::size_t index =
                    static_cast<std::size_t>(y) * resolution + static_cast<std::size_t>(x);
                const float direction = (i > 0 || (i == 0 && j > 0)) ? 1.0f : -1.0f;
                const float phase = direction * grid.omega[index] * timeSeconds;
                const Complex h = grid.spectrum[index] * Complex(std::cos(phase), std::sin(phase));
                work[index] = h;
                const float kx = static_cast<float>(i) * dk;
                const float kz = static_cast<float>(j) * dk;
                const float k2 = kx * kx + kz * kz;
                Complex slopeSpectrum(0.0f, 0.0f);
                Complex choppySpectrum(0.0f, 0.0f);
                if (k2 > 1.0e-12f) {
                    const float invK = 1.0f / std::sqrt(k2);
                    slopeSpectrum = h * Complex(-kz, kx);
                    choppySpectrum = slopeSpectrum * invK;
                }
                slope[index] = slopeSpectrum;
                choppy[index] = choppySpectrum;
            }
        }
        InverseFft2(work);
        InverseFft2(slope);
        InverseFft2(choppy);
        for (int y = 0; y < resolution; ++y) {
            for (int x = 0; x < resolution; ++x) {
                const std::size_t index =
                    static_cast<std::size_t>(y) * resolution + static_cast<std::size_t>(x);
                grid.height[index] = work[index].real();
                grid.slopeX[index] = slope[index].real();
                grid.slopeZ[index] = slope[index].imag();
                grid.choppyX[index] = choppy[index].real();
                grid.choppyZ[index] = choppy[index].imag();
            }
        }
        BuildMipChain(grid);
    }

    void BuildMipChain(Grid& grid) const {
        const std::array<std::vector<float>*, 5> fields = {&grid.height, &grid.slopeX, &grid.slopeZ,
                                                           &grid.choppyX, &grid.choppyZ};
        const int resolution = static_cast<int>(m_settings.resolution);
        for (std::size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex) {
            std::vector<float>* field = fields[fieldIndex];
            std::vector<std::vector<float>>& mips = grid.mips[fieldIndex];
            mips.resize(m_mipLevelCount);
            mips[0] = *field;
            int sourceResolution = resolution;
            for (std::uint32_t level = 1; level < m_mipLevelCount; ++level) {
                const int targetResolution = std::max(sourceResolution / 2, 1);
                const std::vector<float>& source = mips[level - 1];
                std::vector<float>& target = mips[level];
                target.assign(static_cast<std::size_t>(targetResolution) * targetResolution, 0.0f);
                for (int y = 0; y < targetResolution; ++y) {
                    const int y0 = std::min(y * 2, sourceResolution - 1);
                    const int y1 = std::min(y0 + 1, sourceResolution - 1);
                    for (int x = 0; x < targetResolution; ++x) {
                        const int x0 = std::min(x * 2, sourceResolution - 1);
                        const int x1 = std::min(x0 + 1, sourceResolution - 1);
                        const auto at = [&](int sx, int sy) {
                            return source[static_cast<std::size_t>(sy) * sourceResolution + sx];
                        };
                        target[static_cast<std::size_t>(y) * targetResolution + x] =
                            0.25f * (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1));
                    }
                }
                sourceResolution = targetResolution;
            }
        }
    }

    float Sample(const std::vector<std::vector<float>>& mips, float x, float z, float size,
                 float lod) const {
        if (mips.empty() || mips[0].empty()) {
            return 0.0f;
        }
        const int baseResolution = static_cast<int>(m_settings.resolution);
        const float clampedLod = std::clamp(lod, 0.0f, static_cast<float>(mips.size() - 1));
        const int level0 = static_cast<int>(std::floor(clampedLod));
        const int level1 = std::min(level0 + 1, static_cast<int>(mips.size() - 1));
        const float levelBlend = clampedLod - static_cast<float>(level0);
        const auto sampleLevel = [&](int level) {
            const int resolution = std::max(baseResolution >> level, 1);
            const float u = x / size - std::floor(x / size);
            const float v = z / size - std::floor(z / size);
            const float fx = u * static_cast<float>(resolution);
            const float fz = v * static_cast<float>(resolution);
            const int x0 = static_cast<int>(std::floor(fx));
            const int z0 = static_cast<int>(std::floor(fz));
            const int xi0 = (x0 % resolution + resolution) % resolution;
            const int zi0 = (z0 % resolution + resolution) % resolution;
            const int x1 = (xi0 + 1) % resolution;
            const int z1 = (zi0 + 1) % resolution;
            const float tx = fx - static_cast<float>(x0);
            const float tz = fz - static_cast<float>(z0);
            const std::vector<float>& field = mips[static_cast<std::size_t>(level)];
            const auto at = [&](int sx, int sz) {
                return field[static_cast<std::size_t>(sz) * resolution + sx];
            };
            const float h00 = at(xi0, zi0);
            const float h10 = at(x1, zi0);
            const float h01 = at(xi0, z1);
            const float h11 = at(x1, z1);
            return (h00 + (h10 - h00) * tx) +
                   ((h01 + (h11 - h01) * tx) - (h00 + (h10 - h00) * tx)) * tz;
        };
        const float a = sampleLevel(level0);
        if (level1 == level0) {
            return a;
        }
        return a + (sampleLevel(level1) - a) * levelBlend;
    }

    WaveSample SampleWaves(float x, float z, float footprint = 0.0f) const {
        WaveSample result;
        for (const Grid& grid : m_grids) {
            const float gridTexelSize = grid.size / static_cast<float>(m_settings.resolution);
            const float lod = footprint > gridTexelSize && gridTexelSize > 0.0f
                                  ? std::log2(footprint / gridTexelSize)
                                  : 0.0f;
            result.height += Sample(grid.mips[0], x, z, grid.size, lod);
            result.slopeX += Sample(grid.mips[1], x, z, grid.size, lod);
            result.slopeZ += Sample(grid.mips[2], x, z, grid.size, lod);
            result.choppyX += Sample(grid.mips[3], x, z, grid.size, lod);
            result.choppyZ += Sample(grid.mips[4], x, z, grid.size, lod);
        }
        return result;
    }

    Settings m_settings;
    std::array<Grid, 4> m_grids;
    std::array<float, 4> m_gridSizes{};
    std::uint32_t m_mipLevelCount = 0;
    std::uint32_t m_updateCount = 0;
    float m_lastUpdateTime = -1.0f;
    bool m_valid = false;
};

} // namespace

struct WaterSurface::Impl {
    WaterSurfaceSettings settings;
    std::vector<hbg::asset::MeshVertex> base_vertices;
    std::uint32_t object_index = UINT32_MAX;
    std::uint32_t mesh_index = UINT32_MAX;
    std::uint32_t target_index = UINT32_MAX;
    bool screen_grid = false;
    int grid_width = 0;
    int grid_height = 0;
    hbg::Vec2 uv_scale{1.0f, 1.0f};
    hbg::Vec2 uv_origin{};
    hbg::Mat4 last_inv_view_proj = hbg::Mat4::identity();
    hbg::Vec3 last_camera_position{};
    bool view_valid = false;
    float base_height = 0.0f;
    OceanFFT ocean;
    WaterSurfaceStats stats;
};

WaterSurface::WaterSurface() = default;
WaterSurface::~WaterSurface() = default;
WaterSurface::WaterSurface(WaterSurface&&) noexcept = default;
WaterSurface& WaterSurface::operator=(WaterSurface&&) noexcept = default;

bool WaterSurface::Bind(hbg::render::RenderScene& scene, const WaterSurfaceSettings& settings,
                        std::string* error) {
    Close();
    auto impl = std::make_unique<Impl>();
    for (std::uint32_t index = 0; index < scene.objects.size(); ++index) {
        const hbg::render::RenderObject& object = scene.objects[index];
        if (object.name == "Water" && object.meshIndex < scene.meshes.size()) {
            impl->object_index = index;
            impl->mesh_index = object.meshIndex;
            break;
        }
    }
    if (impl->mesh_index == UINT32_MAX) {
        if (error != nullptr) {
            *error = "water surface: Water object was not found";
        }
        return false;
    }
    hbg::asset::Mesh& mesh = scene.meshes[impl->mesh_index];
    if (mesh.vertices.empty()) {
        if (error != nullptr) {
            *error = "water surface: Water mesh has no vertices";
        }
        return false;
    }
    ScreenGridSource source;
    source.baseHeight = mesh.vertices.front().position.y;
    source.uvMin = mesh.vertices.front().texcoord0;
    source.uvMax = source.uvMin;
    source.materialIndex = mesh.submeshes.empty() ? 0u : mesh.submeshes.front().materialIndex;
    for (const hbg::asset::MeshVertex& vertex : mesh.vertices) {
        source.bounds.expand(vertex.position);
        source.uvMin.x = std::min(source.uvMin.x, vertex.texcoord0.x);
        source.uvMin.y = std::min(source.uvMin.y, vertex.texcoord0.y);
        source.uvMax.x = std::max(source.uvMax.x, vertex.texcoord0.x);
        source.uvMax.y = std::max(source.uvMax.y, vertex.texcoord0.y);
    }
    if (settings.viewport_width != 0 && settings.viewport_height != 0) {
        ScreenGridDesc grid;
        if (ConfigureScreenGrid(mesh,
                                static_cast<std::uint16_t>(
                                    std::min<std::uint32_t>(settings.viewport_width, 65535u)),
                                static_cast<std::uint16_t>(
                                    std::min<std::uint32_t>(settings.viewport_height, 65535u)),
                                &grid, &source)) {
            impl->screen_grid = true;
            impl->grid_width = grid.width;
            impl->grid_height = grid.height;
        }
    }
    impl->settings = settings;
    impl->base_vertices = mesh.vertices;
    impl->base_height = mesh.vertices.front().position.y;
    if (impl->screen_grid && !impl->base_vertices.empty()) {
        float minX = impl->base_vertices.front().position.x;
        float maxX = minX;
        float minZ = impl->base_vertices.front().position.z;
        float maxZ = minZ;
        float minU = impl->base_vertices.front().texcoord0.x;
        float maxU = minU;
        float minV = impl->base_vertices.front().texcoord0.y;
        float maxV = minV;
        for (const hbg::asset::MeshVertex& vertex : impl->base_vertices) {
            minX = std::min(minX, vertex.position.x);
            maxX = std::max(maxX, vertex.position.x);
            minZ = std::min(minZ, vertex.position.z);
            maxZ = std::max(maxZ, vertex.position.z);
            minU = std::min(minU, vertex.texcoord0.x);
            maxU = std::max(maxU, vertex.texcoord0.x);
            minV = std::min(minV, vertex.texcoord0.y);
            maxV = std::max(maxV, vertex.texcoord0.y);
        }
        const float spanX = maxX - minX;
        const float spanZ = maxZ - minZ;
        const float spanU = maxU - minU;
        const float spanV = maxV - minV;
        impl->uv_scale = hbg::Vec2{spanX > 1.0e-6f ? spanU / spanX : 0.0f,
                                   spanZ > 1.0e-6f ? spanV / spanZ : 0.0f};
        impl->uv_origin = hbg::Vec2{minU - minX * impl->uv_scale.x, minV - minZ * impl->uv_scale.y};
    }
    OceanFFT::Settings ocean_settings;
    ocean_settings.tileSize = settings.tile_size;
    ocean_settings.amplitude = settings.amplitude;
    ocean_settings.updateHz = settings.update_hz;
    ocean_settings.sphereRadius = settings.sphere_radius;
    if (!impl->ocean.Init(ocean_settings)) {
        if (error != nullptr) {
            *error = "water surface: FFT ocean initialization failed";
        }
        return false;
    }

    hbg::render::RenderObject& object = scene.objects[impl->object_index];
    const std::vector<float> previous_weights = object.morphWeights;
    hbg::asset::MorphTarget target;
    target.name = "WaterFFT";
    target.positionDeltas.assign(mesh.vertices.size(), hbg::Vec3{});
    target.normalDeltas.assign(mesh.vertices.size(), hbg::Vec3{});
    target.hasNormals = true;
    impl->target_index = static_cast<std::uint32_t>(mesh.morphTargets.size());
    mesh.morphTargets.push_back(std::move(target));
    object.morphWeights.assign(mesh.morphTargets.size(), 0.0f);
    for (std::size_t index = 0; index < previous_weights.size() && index < impl->target_index;
         ++index) {
        object.morphWeights[index] = previous_weights[index];
    }
    object.morphWeights[impl->target_index] = 1.0f;

    impl->stats.active = true;
    impl->stats.object_index = impl->object_index;
    impl->stats.mesh_index = impl->mesh_index;
    impl->stats.vertex_count = static_cast<std::uint32_t>(impl->base_vertices.size());
    impl->stats.screen_grid = impl->screen_grid;
    impl->stats.grid_width = static_cast<std::uint32_t>(impl->grid_width);
    impl->stats.grid_height = static_cast<std::uint32_t>(impl->grid_height);
    impl->stats.base_height = impl->base_height;
    impl->stats.min_height = impl->base_height;
    impl->stats.max_height = impl->base_height;
    impl_ = std::move(impl);
    return true;
}

bool WaterSurface::Update(hbg::render::RenderScene& scene, float time_seconds,
                          const hbg::Mat4* inv_view_proj, const hbg::Vec3* camera_position) {
    if (!impl_ || impl_->mesh_index >= scene.meshes.size()) {
        return false;
    }
    hbg::asset::Mesh& mesh = scene.meshes[impl_->mesh_index];
    if (impl_->target_index >= mesh.morphTargets.size()) {
        return false;
    }
    const bool fft_updated = impl_->ocean.Update(time_seconds);
    const bool view_available = inv_view_proj != nullptr && camera_position != nullptr;
    bool view_changed = false;
    if (impl_->screen_grid && view_available) {
        view_changed = !impl_->view_valid || *camera_position != impl_->last_camera_position ||
                       !std::equal(inv_view_proj->values.begin(), inv_view_proj->values.end(),
                                   impl_->last_inv_view_proj.values.begin());
    }
    if (!fft_updated && !view_changed) {
        impl_->stats.fft_updates = impl_->ocean.updateCount();
        return false;
    }

    std::vector<hbg::asset::MeshVertex> deformed = impl_->base_vertices;
    if (impl_->screen_grid && view_available) {
        impl_->ocean.ApplyToScreenGrid(
            deformed, impl_->base_height, impl_->grid_width, impl_->grid_height, *inv_view_proj,
            *camera_position, impl_->uv_scale, impl_->uv_origin, impl_->settings.sphere_radius);
        impl_->last_inv_view_proj = *inv_view_proj;
        impl_->last_camera_position = *camera_position;
        impl_->view_valid = true;
    } else {
        impl_->ocean.ApplyToVertices(deformed, impl_->base_height, impl_->base_vertices);
    }
    hbg::asset::MorphTarget& target = mesh.morphTargets[impl_->target_index];
    target.positionDeltas.resize(deformed.size());
    target.normalDeltas.resize(deformed.size());
    target.hasNormals = true;
    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();
    float maxDisplacement = 0.0f;
    for (std::size_t index = 0; index < deformed.size(); ++index) {
        const hbg::Vec3 delta = deformed[index].position - impl_->base_vertices[index].position;
        target.positionDeltas[index] = delta;
        target.normalDeltas[index] = deformed[index].normal - impl_->base_vertices[index].normal;
        minHeight = std::min(minHeight, deformed[index].position.y);
        maxHeight = std::max(maxHeight, deformed[index].position.y);
        maxDisplacement = std::max(maxDisplacement, hbg::length(delta));
        mesh.vertices[index].texcoord0 = deformed[index].texcoord0;
        mesh.vertices[index].texcoord1 = deformed[index].texcoord1;
        mesh.vertices[index].tangent = deformed[index].tangent;
        mesh.vertices[index].color = deformed[index].color;
    }
    impl_->stats.fft_updates = impl_->ocean.updateCount();
    impl_->stats.morph_updates += 1;
    impl_->stats.min_height = minHeight;
    impl_->stats.max_height = maxHeight;
    impl_->stats.max_displacement = maxDisplacement;
    return true;
}

void WaterSurface::Close() { impl_.reset(); }

bool WaterSurface::active() const noexcept { return impl_ && impl_->stats.active; }

const WaterSurfaceStats& WaterSurface::stats() const noexcept {
    static const WaterSurfaceStats empty;
    return impl_ ? impl_->stats : empty;
}

} // namespace parcel_hbg
