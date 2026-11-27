#include "parcel/hbg/heightmap_tile_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace parcel_hbg {
namespace {

constexpr std::size_t kHeaderSize = 20;

std::uint16_t ReadU16(const std::byte* data) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[0])) |
           static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[1]) << 8);
}

std::uint32_t ReadU32(const std::byte* data) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[3])) << 24);
}

float ReadF32(const std::byte* data) {
    const std::uint32_t bits = ReadU32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

bool HeightmapTileCache::Load(std::span<const std::byte> data, std::string* error) {
    Close();
    if (data.size() < kHeaderSize) {
        if (error != nullptr) {
            *error = "heightmap tile cache: header is truncated";
        }
        return false;
    }

    char magic[8] = {};
    std::memcpy(magic, data.data(), sizeof(magic));
    const std::uint32_t width = ReadU32(data.data() + 8);
    const std::uint32_t height = ReadU32(data.data() + 12);
    const float scale = ReadF32(data.data() + 16);
    if (std::memcmp(magic, "HBGHMAP1", 8) != 0 || width == 0 || height == 0 ||
        !std::isfinite(scale) || scale <= 0.0f) {
        if (error != nullptr) {
            *error = "heightmap tile cache: invalid header";
        }
        return false;
    }

    const std::uint64_t sample_count = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t max_samples =
        (static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) - kHeaderSize) /
        sizeof(std::uint16_t);
    if (sample_count > max_samples) {
        if (error != nullptr) {
            *error = "heightmap tile cache: sample data exceeds limits";
        }
        return false;
    }
    const std::uint64_t sample_bytes = sample_count * sizeof(std::uint16_t);
    if (data.size() < kHeaderSize + static_cast<std::size_t>(sample_bytes)) {
        if (error != nullptr) {
            *error = "heightmap tile cache: sample data is truncated";
        }
        return false;
    }

    data_.assign(data.begin(), data.end());
    width_ = width;
    height_ = height;
    data_offset_ = static_cast<std::uint32_t>(kHeaderSize);
    height_scale_ = scale;
    stats_ = {};
    return true;
}

void HeightmapTileCache::Close() {
    data_.clear();
    width_ = 0;
    height_ = 0;
    data_offset_ = 0;
    height_scale_ = 1.0f;
    origin_x_ = 0.0f;
    origin_z_ = 0.0f;
    world_size_ = 0.0f;
    skirt_depth_ = 0.0f;
    stats_ = {};
    known_bounds_.clear();
    tiles_.clear();
    index_.clear();
}

void HeightmapTileCache::SetWorld(float origin_x, float origin_z, float world_size,
                                  float skirt_depth) {
    origin_x_ = origin_x;
    origin_z_ = origin_z;
    world_size_ = world_size;
    skirt_depth_ = skirt_depth;
    known_bounds_.clear();
    tiles_.clear();
    index_.clear();
}

void HeightmapTileCache::SetCapacity(std::size_t capacity) {
    capacity_ = std::max<std::size_t>(capacity, 1);
    while (tiles_.size() > capacity_) {
        index_.erase(tiles_.back().key);
        tiles_.pop_back();
    }
}

std::uint64_t HeightmapTileCache::TileKey(int level, int tx, int ty) {
    return (static_cast<std::uint64_t>(level) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tx)) << 16) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(ty));
}

bool HeightmapTileCache::ReadTexel(int x, int z, std::uint16_t& value) const {
    if (data_.empty() || x < 0 || z < 0 || x >= static_cast<int>(width_) ||
        z >= static_cast<int>(height_)) {
        return false;
    }
    const std::size_t offset =
        static_cast<std::size_t>(data_offset_) +
        (static_cast<std::size_t>(z) * width_ + static_cast<std::size_t>(x)) *
            sizeof(std::uint16_t);
    if (offset + sizeof(std::uint16_t) > data_.size()) {
        return false;
    }
    value = ReadU16(data_.data() + offset);
    return true;
}

bool HeightmapTileCache::SampleHeight(float world_x, float world_z, float& height) const {
    if (data_.empty() || world_size_ <= 0.0f || width_ == 0 || height_ == 0 ||
        !std::isfinite(world_x) || !std::isfinite(world_z)) {
        return false;
    }
    const float u = (world_x - origin_x_) / world_size_;
    const float v = (world_z - origin_z_) / world_size_;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return false;
    }

    const float px = u * static_cast<float>(width_ - 1);
    const float pz = v * static_cast<float>(height_ - 1);
    const int x0 = static_cast<int>(std::floor(px));
    const int z0 = static_cast<int>(std::floor(pz));
    const int x1 = std::min(x0 + 1, static_cast<int>(width_) - 1);
    const int z1 = std::min(z0 + 1, static_cast<int>(height_) - 1);
    const float fx = px - static_cast<float>(x0);
    const float fz = pz - static_cast<float>(z0);

    std::uint16_t h00 = 0;
    std::uint16_t h10 = 0;
    std::uint16_t h01 = 0;
    std::uint16_t h11 = 0;
    if (!ReadTexel(x0, z0, h00) || !ReadTexel(x1, z0, h10) || !ReadTexel(x0, z1, h01) ||
        !ReadTexel(x1, z1, h11)) {
        return false;
    }

    const float top =
        static_cast<float>(h00) + (static_cast<float>(h10) - static_cast<float>(h00)) * fx;
    const float bottom =
        static_cast<float>(h01) + (static_cast<float>(h11) - static_cast<float>(h01)) * fx;
    height = (top + (bottom - top) * fz) * height_scale_;
    return true;
}

bool HeightmapTileCache::ReadBounds(int x0, int z0, int x1, int z1, float& zmin_out,
                                    float& zmax_out) const {
    if (data_.empty() || x0 < 0 || z0 < 0 || x1 < x0 || z1 < z0 || x1 >= static_cast<int>(width_) ||
        z1 >= static_cast<int>(height_)) {
        return false;
    }

    bool any = false;
    float min_height = 0.0f;
    float max_height = 0.0f;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            std::uint16_t value = 0;
            if (!ReadTexel(x, z, value)) {
                return false;
            }
            const float height = static_cast<float>(value) * height_scale_;
            if (!any) {
                min_height = height;
                max_height = height;
                any = true;
            } else {
                min_height = std::min(min_height, height);
                max_height = std::max(max_height, height);
            }
        }
    }

    if (!any) {
        return false;
    }
    zmin_out = min_height - skirt_depth_;
    zmax_out = max_height;
    return true;
}

bool HeightmapTileCache::Query(int level, int tx, int ty, float& zmin, float& zmax) {
    if (data_.empty() || world_size_ <= 0.0f || level < 0 || level >= 31) {
        return false;
    }
    const int count = 1 << level;
    if (tx < 0 || ty < 0 || tx >= count || ty >= count) {
        return false;
    }

    const std::uint64_t key = TileKey(level, tx, ty);
    const auto known = known_bounds_.find(key);
    if (known != known_bounds_.end()) {
        ++stats_.hits;
        const auto found = index_.find(key);
        if (found != index_.end()) {
            tiles_.splice(tiles_.begin(), tiles_, found->second);
        }
        zmin = known->second.first;
        zmax = known->second.second;
        return true;
    }

    const float size = world_size_ / static_cast<float>(count);
    const float ox = origin_x_ + static_cast<float>(tx) * size;
    const float oz = origin_z_ + static_cast<float>(ty) * size;
    const float half = world_size_ * 0.5f;
    int x0 =
        static_cast<int>(std::floor((ox + half) / world_size_ * static_cast<float>(width_ - 1))) -
        1;
    int x1 = static_cast<int>(
                 std::ceil((ox + size + half) / world_size_ * static_cast<float>(width_ - 1))) +
             1;
    int z0 =
        static_cast<int>(std::floor((oz + half) / world_size_ * static_cast<float>(height_ - 1))) -
        1;
    int z1 = static_cast<int>(
                 std::ceil((oz + size + half) / world_size_ * static_cast<float>(height_ - 1))) +
             1;
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(static_cast<int>(width_) - 1, x1);
    z1 = std::min(static_cast<int>(height_) - 1, z1);
    if (!ReadBounds(x0, z0, x1, z1, zmin, zmax)) {
        return false;
    }

    ++stats_.misses;
    ++stats_.loaded_tiles;
    known_bounds_.emplace(key, std::pair<float, float>{zmin, zmax});
    tiles_.push_front({key, zmin, zmax});
    index_.emplace(key, tiles_.begin());
    while (tiles_.size() > capacity_) {
        index_.erase(tiles_.back().key);
        tiles_.pop_back();
        ++stats_.evictions;
    }
    return true;
}

} // namespace parcel_hbg
