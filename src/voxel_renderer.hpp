#pragma once

#include "mcworld/terrain.hpp"

#include <raylib.h>

#include <cstddef>
#include <cstdint>
#include <vector>
#include <map>

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
    std::size_t solidBlockCount{};
    std::size_t waterBlockCount{};
    std::size_t lavaBlockCount{};

    [[nodiscard]] std::size_t faceCount() const noexcept { return vertices.size() / 6; }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return vertices.size() / 3; }
};

struct SmoothTerrainMesh {
    std::vector<VoxelVertex> vertices;

    [[nodiscard]] std::size_t triangleCount() const noexcept { return vertices.size() / 3; }
};

struct TerrainTextureAtlas {
    Texture2D texture{};
    bool faithful{};
};

// Reuses generated chunks, including the border needed for neighbor visibility.
class VoxelTerrain {
public:
    explicit VoxelTerrain(const mcworld::OverworldNoiseRouter& router) : router_(router), generator_(router) {}

    [[nodiscard]] SmoothTerrainMesh buildSmoothMesh(int chunkX, int chunkZ);
    [[nodiscard]] VoxelMesh buildMesh(int chunkX, int chunkZ);

private:
    [[nodiscard]] const mcworld::TerrainChunk& chunk(int x, int z);

    const mcworld::OverworldNoiseRouter& router_;
    mcworld::OverworldTerrainGenerator generator_;
    std::map<std::pair<int, int>, mcworld::TerrainChunk> chunks_;
};

// Requires an initialized Raylib window/OpenGL context.
[[nodiscard]] Mesh uploadSmoothTerrainMesh(const SmoothTerrainMesh& terrain);
[[nodiscard]] Mesh uploadVoxelMesh(const VoxelMesh& voxels);
[[nodiscard]] TerrainTextureAtlas loadTerrainTextureAtlas();

} // namespace viewer
