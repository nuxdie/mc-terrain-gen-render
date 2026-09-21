#pragma once

#include "mcworld/worldgen.hpp"

#include <cstdint>
#include <vector>

namespace mcworld {

struct SurfaceVertex {
    float x{};
    float y{};
    float z{};
    float nx{};
    float ny{};
    float nz{};
};

struct SurfaceMesh {
    std::vector<SurfaceVertex> vertices;

    [[nodiscard]] std::size_t triangleCount() const noexcept {
        return vertices.size() / 3;
    }
};

struct ChunkSurfaceRequest {
    // Output positions are chunk-local in X/Z. Y bounds must lie in [-64, 320].
    int chunkX{0};
    int chunkZ{0};
    int minY{-64};
    int maxY{320};
    float isoLevel{0.0F};
};

[[nodiscard]] SurfaceMesh buildChunkIsosurface(
    const OverworldNoiseRouter& router,
    const ChunkSurfaceRequest& request = {}
);

} // namespace mcworld
