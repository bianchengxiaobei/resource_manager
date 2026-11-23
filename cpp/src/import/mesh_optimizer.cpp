#include "parcel/import/mesh_optimizer.hpp"

#include "parcel/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "meshoptimizer.h"

namespace parcel::import {
namespace {

using core::ErrorCode;
using core::MakeError;

struct IndexRange {
    std::size_t offset = 0;
    std::size_t count = 0;
};

core::Result<std::vector<IndexRange>> CollectRanges(const assets::MeshAsset& mesh) {
    if (mesh.indices.size() % 3U != 0) {
        return MakeError(ErrorCode::InvalidArgument, "mesh index count is not a multiple of 3");
    }
    if (mesh.vertices.size() > std::numeric_limits<unsigned int>::max()) {
        return MakeError(ErrorCode::InvalidArgument, "mesh has too many vertices");
    }
    for (std::uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            return MakeError(ErrorCode::InvalidArgument, "mesh index is out of range");
        }
    }

    std::vector<IndexRange> ranges;
    if (mesh.sub_meshes.empty()) {
        ranges.push_back({0, mesh.indices.size()});
        return ranges;
    }
    ranges.reserve(mesh.sub_meshes.size());
    for (const assets::SubMesh& sub_mesh : mesh.sub_meshes) {
        const std::size_t offset = sub_mesh.index_offset;
        const std::size_t count = sub_mesh.index_count;
        if (offset > mesh.indices.size() || count > mesh.indices.size() - offset) {
            return MakeError(ErrorCode::InvalidArgument, "mesh submesh range is invalid");
        }
        if (count % 3U != 0) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh submesh index count is not a multiple of 3");
        }
        ranges.push_back({offset, count});
    }
    return ranges;
}

} // namespace

core::Result<void> OptimizeMesh(assets::MeshAsset& mesh, const MeshOptimizeOptions& options) {
    if (options.optimize_overdraw &&
        (!std::isfinite(options.overdraw_threshold) || options.overdraw_threshold <= 0.0F)) {
        return MakeError(ErrorCode::InvalidArgument, "mesh overdraw threshold is invalid");
    }
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return {};
    }
    static_assert(std::is_trivially_copyable_v<assets::MeshVertex>);
    static_assert(std::is_trivially_copyable_v<assets::SkinVertex>);
    static_assert(offsetof(assets::MeshVertex, position) == 0);
    static_assert(sizeof(assets::MeshVertex) <= 256);
    if (!mesh.skin_vertices.empty() && mesh.skin_vertices.size() != mesh.vertices.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "mesh skin vertex count does not match vertex count");
    }
    if (mesh.morph_targets.size() > assets::kMaxMorphTargets ||
        static_cast<std::uint64_t>(mesh.morph_targets.size()) * mesh.vertices.size() >
            assets::kMaxMorphDeltaVertices) {
        return MakeError(ErrorCode::InvalidArgument, "mesh has too many morph targets");
    }
    if (!mesh.morph_weights.empty() && mesh.morph_weights.size() != mesh.morph_targets.size()) {
        return MakeError(ErrorCode::InvalidArgument,
                         "mesh morph weight count does not match morph target count");
    }
    for (float weight : mesh.morph_weights) {
        if (!std::isfinite(weight)) {
            return MakeError(ErrorCode::InvalidArgument, "mesh morph weight is invalid");
        }
    }
    for (const assets::MorphTarget& target : mesh.morph_targets) {
        if (target.position_deltas.size() != mesh.vertices.size() ||
            (target.has_normals && target.normal_deltas.size() != mesh.vertices.size()) ||
            (target.has_tangents && target.tangent_deltas.size() != mesh.vertices.size()) ||
            (!target.has_normals && !target.normal_deltas.empty()) ||
            (!target.has_tangents && !target.tangent_deltas.empty())) {
            return MakeError(ErrorCode::InvalidArgument,
                             "mesh morph target delta count does not match vertex count");
        }
    }

    auto ranges = CollectRanges(mesh);
    if (!ranges) {
        return std::move(ranges).TakeError();
    }

    if (!mesh.skin_vertices.empty() || !mesh.morph_targets.empty()) {
        for (const IndexRange& range : ranges.Value()) {
            if (range.count == 0) {
                continue;
            }
            std::vector<std::uint32_t> cached(range.count);
            meshopt_optimizeVertexCache(cached.data(), mesh.indices.data() + range.offset,
                                        range.count, mesh.vertices.size());
            if (options.optimize_overdraw) {
                std::vector<std::uint32_t> overdraw(range.count);
                meshopt_optimizeOverdraw(overdraw.data(), cached.data(), range.count,
                                         &mesh.vertices.front().position.x, mesh.vertices.size(),
                                         sizeof(assets::MeshVertex), options.overdraw_threshold);
                cached.swap(overdraw);
            }
            std::copy(cached.begin(), cached.end(),
                      mesh.indices.begin() + static_cast<std::ptrdiff_t>(range.offset));
        }
        return {};
    }

    std::vector<unsigned int> remap(mesh.vertices.size());
    const std::size_t unique_vertices = meshopt_generateVertexRemap(
        remap.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.data(),
        mesh.vertices.size(), sizeof(assets::MeshVertex));
    if (unique_vertices == 0) {
        return MakeError(ErrorCode::InvalidArgument, "mesh has no referenced vertices");
    }

    std::vector<assets::MeshVertex> vertices(unique_vertices);
    meshopt_remapVertexBuffer(vertices.data(), mesh.vertices.data(), mesh.vertices.size(),
                              sizeof(assets::MeshVertex), remap.data());
    std::vector<std::uint32_t> indices(mesh.indices.size());
    meshopt_remapIndexBuffer(indices.data(), mesh.indices.data(), mesh.indices.size(),
                             remap.data());

    for (const IndexRange& range : ranges.Value()) {
        if (range.count == 0) {
            continue;
        }
        std::vector<std::uint32_t> cached(range.count);
        meshopt_optimizeVertexCache(cached.data(), indices.data() + range.offset, range.count,
                                    unique_vertices);
        if (options.optimize_overdraw) {
            std::vector<std::uint32_t> overdraw(range.count);
            meshopt_optimizeOverdraw(overdraw.data(), cached.data(), range.count,
                                     &vertices.front().position.x, unique_vertices,
                                     sizeof(assets::MeshVertex), options.overdraw_threshold);
            cached.swap(overdraw);
        }
        std::copy(cached.begin(), cached.end(),
                  indices.begin() + static_cast<std::ptrdiff_t>(range.offset));
    }

    std::vector<unsigned int> fetch_remap(unique_vertices);
    const std::size_t final_vertices = meshopt_optimizeVertexFetchRemap(
        fetch_remap.data(), indices.data(), indices.size(), unique_vertices);
    std::vector<assets::MeshVertex> fetched(final_vertices);
    meshopt_remapVertexBuffer(fetched.data(), vertices.data(), unique_vertices,
                              sizeof(assets::MeshVertex), fetch_remap.data());
    std::vector<std::uint32_t> remapped_indices(indices.size());
    meshopt_remapIndexBuffer(remapped_indices.data(), indices.data(), indices.size(),
                             fetch_remap.data());

    mesh.vertices = std::move(fetched);
    mesh.indices = std::move(remapped_indices);
    return {};
}

} // namespace parcel::import
