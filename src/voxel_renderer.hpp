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

// Reuses generated chunks, including the border needed for neighbor visibility.
class VoxelTerrain {
public:
    explicit VoxelTerrain(const mcworld::OverworldNoiseRouter& router):generator_(router){}
    [[nodiscard]] VoxelMesh buildMesh(int chunkX,int chunkZ);
private:
    [[nodiscard]] const mcworld::TerrainChunk& chunk(int x,int z);
    mcworld::OverworldTerrainGenerator generator_;
    std::map<std::pair<int,int>,mcworld::TerrainChunk> chunks_;
};

// Requires an initialized Raylib window/OpenGL context.
[[nodiscard]] Mesh uploadVoxelMesh(const VoxelMesh& voxels);

} // namespace viewer
