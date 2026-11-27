#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parcel_hbg {

class HeightmapTileCache {
  public:
    struct Stats {
        std::uint32_t hits = 0;
        std::uint32_t misses = 0;
        std::uint32_t evictions = 0;
        std::uint32_t loaded_tiles = 0;
    };

    [[nodiscard]] bool Load(std::span<const std::byte> data, std::string* error = nullptr);
    void Close();
    [[nodiscard]] bool loaded() const noexcept { return !data_.empty(); }

    void SetWorld(float origin_x, float origin_z, float world_size, float skirt_depth);
    void SetCapacity(std::size_t capacity);

    [[nodiscard]] bool Query(int level, int tx, int ty, float& zmin, float& zmax);
    [[nodiscard]] bool SampleHeight(float world_x, float world_z, float& height) const;

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] float height_scale() const noexcept { return height_scale_; }

  private:
    struct Tile {
        std::uint64_t key = 0;
        float zmin = 0.0f;
        float zmax = 0.0f;
    };

    [[nodiscard]] static std::uint64_t TileKey(int level, int tx, int ty);
    [[nodiscard]] bool ReadTexel(int x, int z, std::uint16_t& value) const;
    [[nodiscard]] bool ReadBounds(int x0, int z0, int x1, int z1, float& zmin, float& zmax) const;

    std::vector<std::byte> data_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t data_offset_ = 0;
    float height_scale_ = 1.0f;
    float origin_x_ = 0.0f;
    float origin_z_ = 0.0f;
    float world_size_ = 0.0f;
    float skirt_depth_ = 0.0f;
    std::size_t capacity_ = 64;
    Stats stats_;
    std::unordered_map<std::uint64_t, std::pair<float, float>> known_bounds_;
    std::list<Tile> tiles_;
    std::unordered_map<std::uint64_t, std::list<Tile>::iterator> index_;
};

} // namespace parcel_hbg
