#pragma once

#include "mcworld/generation.hpp"

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
    explicit VoxelTerrain(const mcworld::OverworldNoiseRouter& router, mcworld::GenerationOptions options = {})
        : router_(router), worldGenerator_(router, options), terrainGenerator_(router, options.terrain) {}

    // Finalize requested chunks together so incoming radius-1 feature writes
    // are applied in one canonical decoration order.
    mcworld::GenerationProfile prepareArea(int firstChunkX, int firstChunkZ, int width, int depth);

    // Generate stage-7B terrain without structures or decoration.
    void prepareTerrainArea(int firstChunkX, int firstChunkZ, int width, int depth);

    [[nodiscard]] SmoothTerrainMesh buildSmoothMesh(int chunkX, int chunkZ);
    [[nodiscard]] VoxelMesh buildMesh(int chunkX, int chunkZ);

private:
    [[nodiscard]] const mcworld::TerrainChunk& chunk(int x, int z);

    const mcworld::OverworldNoiseRouter& router_;
    mcworld::OverworldWorldGenerator worldGenerator_;
    // Isolated mesh callers retain the cheap stage-7B fallback. The viewer
    // application calls prepareArea() before meshing finalized stage-8 chunks.
    mcworld::OverworldTerrainGenerator terrainGenerator_;
    std::map<std::pair<int, int>, mcworld::TerrainChunk> chunks_;
};

// Requires an initialized Raylib window/OpenGL context.
[[nodiscard]] Mesh uploadSmoothTerrainMesh(const SmoothTerrainMesh& terrain);
[[nodiscard]] Mesh uploadVoxelMesh(const VoxelMesh& voxels);
[[nodiscard]] TerrainTextureAtlas loadTerrainTextureAtlas();

} // namespace viewer
