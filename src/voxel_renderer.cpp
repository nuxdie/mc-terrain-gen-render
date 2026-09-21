#include "voxel_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace viewer {
namespace {

constexpr int kChunkSize = 16;
constexpr int kMinY = -64;
constexpr int kMaxY = 320;
constexpr int kHeight = kMaxY - kMinY;

struct Occupancy {
    std::array<std::uint8_t, (kChunkSize + 2) * kHeight * (kChunkSize + 2)> solid{};

    [[nodiscard]] bool at(int x, int y, int z) const {
        if (x < -1 || x > kChunkSize || y < kMinY || y >= kMaxY || z < -1 || z > kChunkSize) {
            return false;
        }
        return solid[index(x, y, z)] != 0;
    }

    [[nodiscard]] static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>((((z + 1) * (kChunkSize + 2) + x + 1) * kHeight) + y - kMinY);
    }
};

struct Position {
    float x;
    float y;
    float z;
};

struct Normal {
    float x;
    float y;
    float z;
};

[[nodiscard]] unsigned char colorChannel(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0F, 255.0F));
}

[[nodiscard]] std::array<std::uint8_t, 3> faceColor(const Normal& normal, float y) {
    constexpr Normal light{-0.45F, 0.82F, -0.35F};
    constexpr float lightLength = 0.99869913F;
    const float incidence = (normal.x * light.x + normal.y * light.y + normal.z * light.z) / lightLength;
    const float lighting = 0.42F + 0.58F * std::max(0.0F, incidence);
    const float height = std::clamp((y - static_cast<float>(kMinY)) / static_cast<float>(kHeight), 0.0F, 1.0F);

    // Height and face lighting make the density voxels readable without
    // implying that any Minecraft block material has been assigned.
    const float red = 62.0F + 105.0F * height;
    const float green = 82.0F + 100.0F * height;
    const float blue = 88.0F + 94.0F * height;
    return {
        colorChannel(red * lighting),
        colorChannel(green * lighting),
        colorChannel(blue * lighting),
    };
}

void appendFace(VoxelMesh& mesh, const std::array<Position, 4>& corners, const Normal& normal) {
    constexpr std::array<int, 6> indices{{0, 1, 2, 0, 2, 3}};
    const float centerY = (corners[0].y + corners[2].y) * 0.5F;
    const std::array<std::uint8_t, 3> color = faceColor(normal, centerY);

    for (const int index : indices) {
        const Position& position = corners[static_cast<std::size_t>(index)];
        mesh.vertices.push_back({
            position.x,
            position.y,
            position.z,
            normal.x,
            normal.y,
            normal.z,
            color[0],
            color[1],
            color[2],
        });
    }
}

void appendExposedFaces(VoxelMesh& mesh, const Occupancy& occupancy, int x, int y, int z) {
    const float x0 = static_cast<float>(x);
    const float x1 = x0 + 1.0F;
    const float y0 = static_cast<float>(y);
    const float y1 = y0 + 1.0F;
    const float z0 = static_cast<float>(z);
    const float z1 = z0 + 1.0F;

    if (!occupancy.at(x + 1, y, z)) {
        appendFace(mesh, {{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}}, {1, 0, 0});
    }
    if (!occupancy.at(x - 1, y, z)) {
        appendFace(mesh, {{{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}}, {-1, 0, 0});
    }
    if (!occupancy.at(x, y + 1, z)) {
        appendFace(mesh, {{{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}}}, {0, 1, 0});
    }
    if (!occupancy.at(x, y - 1, z)) {
        appendFace(mesh, {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}}, {0, -1, 0});
    }
    if (!occupancy.at(x, y, z + 1)) {
        appendFace(mesh, {{{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}}, {0, 0, 1});
    }
    if (!occupancy.at(x, y, z - 1)) {
        appendFace(mesh, {{{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}}, {0, 0, -1});
    }
}

} // namespace

VoxelMesh buildVoxelMesh(const mcworld::OverworldNoiseRouter& router, int chunkX, int chunkZ) {
    Occupancy occupancy;
    const std::int64_t originX = static_cast<std::int64_t>(chunkX) * kChunkSize;
    const std::int64_t originZ = static_cast<std::int64_t>(chunkZ) * kChunkSize;

    VoxelMesh mesh;
    // Sample neighboring blocks so adjacent chunks do not emit internal walls.
    for (int z = -1; z <= kChunkSize; ++z) {
        for (int x = -1; x <= kChunkSize; ++x) {
            for (int y = kMinY; y < kMaxY; ++y) {
                const bool solid = router.sampleFinalDensity(
                    static_cast<double>(originX + x) + 0.5,
                    static_cast<double>(y) + 0.5,
                    static_cast<double>(originZ + z) + 0.5
                ) > 0.0F;
                occupancy.solid[Occupancy::index(x, y, z)] = solid ? 1 : 0;
                if (solid && x >= 0 && x < kChunkSize && z >= 0 && z < kChunkSize) {
                    ++mesh.positiveDensityVoxelCount;
                }
            }
        }
    }

    mesh.vertices.reserve(250'000);
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            for (int y = kMinY; y < kMaxY; ++y) {
                if (occupancy.at(x, y, z)) {
                    appendExposedFaces(mesh, occupancy, x, y, z);
                }
            }
        }
    }
    return mesh;
}

Mesh uploadVoxelMesh(const VoxelMesh& voxels) {
    if (voxels.vertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || voxels.vertices.size() > std::numeric_limits<unsigned int>::max() / (3 * sizeof(float))) {
        throw std::runtime_error("Generated voxel mesh exceeds Raylib's vertex limit");
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(voxels.vertices.size());
    mesh.triangleCount = static_cast<int>(voxels.triangleCount());
    mesh.vertices = static_cast<float*>(MemAlloc(voxels.vertices.size() * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(voxels.vertices.size() * 3 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(voxels.vertices.size() * 4 * sizeof(unsigned char)));
    if (!mesh.vertices || !mesh.normals || !mesh.colors) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.colors);
        throw std::runtime_error("Could not allocate viewer voxel mesh buffers");
    }

    for (std::size_t i = 0; i < voxels.vertices.size(); ++i) {
        const VoxelVertex& vertex = voxels.vertices[i];
        mesh.vertices[i * 3] = vertex.x;
        mesh.vertices[i * 3 + 1] = vertex.y;
        mesh.vertices[i * 3 + 2] = vertex.z;
        mesh.normals[i * 3] = vertex.nx;
        mesh.normals[i * 3 + 1] = vertex.ny;
        mesh.normals[i * 3 + 2] = vertex.nz;
        mesh.colors[i * 4] = vertex.red;
        mesh.colors[i * 4 + 1] = vertex.green;
        mesh.colors[i * 4 + 2] = vertex.blue;
        mesh.colors[i * 4 + 3] = 255;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

} // namespace viewer
