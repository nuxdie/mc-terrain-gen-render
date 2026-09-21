#pragma once

#include "mcworld/worldgen.hpp"

#include <raylib.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace viewer {

struct VoxelVertex {
    float x{};
    float y{};
    float z{};
    float nx{};
    float ny{};
    float nz{};
    float u{};
    float v{};
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
};

struct VoxelMesh {
    std::vector<VoxelVertex> vertices;
    std::size_t solidVoxelCount{};

    [[nodiscard]] std::size_t faceCount() const noexcept { return vertices.size() / 6; }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return vertices.size() / 3; }
};

struct VoxelTextureAtlas {
    Texture2D texture{};
    bool faithful{};
};

// Density is sampled at block centers. Only faces adjacent to air are emitted.
[[nodiscard]] VoxelMesh buildVoxelMesh(const mcworld::OverworldNoiseRouter& router, int chunkX, int chunkZ);

// These functions require an initialized Raylib window/OpenGL context.
[[nodiscard]] Mesh uploadVoxelMesh(const VoxelMesh& voxels);
[[nodiscard]] VoxelTextureAtlas loadVoxelTextureAtlas();

} // namespace viewer
