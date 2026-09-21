#include "voxel_renderer.hpp"

#include <raymath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace viewer {
namespace {

constexpr int kChunkSize = 16;
constexpr int kMinY = -64;
constexpr int kMaxY = 320;
constexpr int kHeight = kMaxY - kMinY;
constexpr int kAtlasTiles = 5;
constexpr int kFallbackTileSize = 32;

enum class Tile : int {
    GrassTop,
    GrassSide,
    Dirt,
    Stone,
    Deepslate,
};

struct Occupancy {
    std::array<std::uint8_t, kChunkSize * kHeight * kChunkSize> solid{};
    std::array<int, kChunkSize * kChunkSize> surfaceY{};

    [[nodiscard]] bool at(int x, int y, int z) const {
        if (x < 0 || x >= kChunkSize || y < kMinY || y >= kMaxY || z < 0 || z >= kChunkSize) {
            return false;
        }
        return solid[index(x, y, z)] != 0;
    }

    [[nodiscard]] static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(((z * kChunkSize + x) * kHeight) + y - kMinY);
    }

    [[nodiscard]] static std::size_t columnIndex(int x, int z) {
        return static_cast<std::size_t>(z * kChunkSize + x);
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

[[nodiscard]] std::array<std::uint8_t, 3> faceColor(Tile tile, const Normal& normal) {
    constexpr Vector3 lightDirection{-0.45F, 0.82F, -0.35F};
    const Vector3 light = Vector3Normalize(lightDirection);
    const float incidence = normal.x * light.x + normal.y * light.y + normal.z * light.z;
    const float lighting = 0.48F + 0.52F * std::max(0.0F, incidence);

    // grass_block_top.png is grayscale because Minecraft normally biome-tints it.
    const std::array<float, 3> base = tile == Tile::GrassTop
        ? std::array<float, 3>{126.0F, 190.0F, 82.0F}
        : std::array<float, 3>{255.0F, 255.0F, 255.0F};
    return {
        colorChannel(base[0] * lighting),
        colorChannel(base[1] * lighting),
        colorChannel(base[2] * lighting),
    };
}

void appendFace(
    VoxelMesh& mesh,
    const std::array<Position, 4>& corners,
    const Normal& normal,
    Tile tile
) {
    // Stay half a texel inside each atlas tile to prevent neighbouring tiles
    // bleeding onto cube edges.
    constexpr float inset = 0.5F / static_cast<float>(kFallbackTileSize);
    const float tileIndex = static_cast<float>(tile);
    const float u0 = (tileIndex + inset) / static_cast<float>(kAtlasTiles);
    const float u1 = (tileIndex + 1.0F - inset) / static_cast<float>(kAtlasTiles);
    constexpr float v0 = inset;
    constexpr float v1 = 1.0F - inset;
    constexpr std::array<std::array<float, 2>, 4> texcoords{{
        {{0.0F, 1.0F}}, {{0.0F, 0.0F}}, {{1.0F, 0.0F}}, {{1.0F, 1.0F}},
    }};
    constexpr std::array<int, 6> indices{{0, 1, 2, 0, 2, 3}};
    const std::array<std::uint8_t, 3> color = faceColor(tile, normal);

    for (const int index : indices) {
        const Position& position = corners[static_cast<std::size_t>(index)];
        const auto& uv = texcoords[static_cast<std::size_t>(index)];
        mesh.vertices.push_back({
            position.x,
            position.y,
            position.z,
            normal.x,
            normal.y,
            normal.z,
            std::lerp(u0, u1, uv[0]),
            std::lerp(v0, v1, uv[1]),
            color[0],
            color[1],
            color[2],
        });
    }
}

[[nodiscard]] Tile interiorTile(int y, int surfaceY) {
    if (y < 0) {
        return Tile::Deepslate;
    }
    if (y >= surfaceY - 3) {
        return Tile::Dirt;
    }
    return Tile::Stone;
}

void appendExposedFaces(VoxelMesh& mesh, const Occupancy& occupancy, int x, int y, int z) {
    const float x0 = static_cast<float>(x);
    const float x1 = x0 + 1.0F;
    const float y0 = static_cast<float>(y);
    const float y1 = y0 + 1.0F;
    const float z0 = static_cast<float>(z);
    const float z1 = z0 + 1.0F;
    const int surfaceY = occupancy.surfaceY[Occupancy::columnIndex(x, z)];
    const bool surfaceBlock = y == surfaceY;
    const Tile blockTile = interiorTile(y, surfaceY);

    if (!occupancy.at(x + 1, y, z)) {
        appendFace(mesh, {{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}}, {1, 0, 0},
                   surfaceBlock ? Tile::GrassSide : blockTile);
    }
    if (!occupancy.at(x - 1, y, z)) {
        appendFace(mesh, {{{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}}, {-1, 0, 0},
                   surfaceBlock ? Tile::GrassSide : blockTile);
    }
    if (!occupancy.at(x, y + 1, z)) {
        appendFace(mesh, {{{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}}}, {0, 1, 0},
                   surfaceBlock ? Tile::GrassTop : blockTile);
    }
    if (!occupancy.at(x, y - 1, z)) {
        appendFace(mesh, {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}}, {0, -1, 0},
                   surfaceBlock ? Tile::Dirt : blockTile);
    }
    if (!occupancy.at(x, y, z + 1)) {
        appendFace(mesh, {{{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}}, {0, 0, 1},
                   surfaceBlock ? Tile::GrassSide : blockTile);
    }
    if (!occupancy.at(x, y, z - 1)) {
        appendFace(mesh, {{{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}}, {0, 0, -1},
                   surfaceBlock ? Tile::GrassSide : blockTile);
    }
}

[[nodiscard]] Color variedColor(Color base, int x, int y, int tile) {
    const std::uint32_t hash = static_cast<std::uint32_t>(x * 73856093U)
        ^ static_cast<std::uint32_t>(y * 19349663U)
        ^ static_cast<std::uint32_t>(tile * 83492791U);
    const float variation = 0.82F + static_cast<float>(hash & 31U) / 100.0F;
    return {
        colorChannel(static_cast<float>(base.r) * variation),
        colorChannel(static_cast<float>(base.g) * variation),
        colorChannel(static_cast<float>(base.b) * variation),
        255,
    };
}

[[nodiscard]] Image makeFallbackAtlas() {
    constexpr std::array<Color, kAtlasTiles> colors{{
        {150, 150, 150, 255}, {91, 145, 65, 255}, {127, 91, 58, 255},
        {128, 128, 128, 255}, {75, 75, 82, 255},
    }};
    Image atlas = GenImageColor(kFallbackTileSize * kAtlasTiles, kFallbackTileSize, WHITE);
    for (int tile = 0; tile < kAtlasTiles; ++tile) {
        for (int y = 0; y < kFallbackTileSize; ++y) {
            for (int x = 0; x < kFallbackTileSize; ++x) {
                ImageDrawPixel(&atlas, tile * kFallbackTileSize + x, y, variedColor(colors[tile], x, y, tile));
            }
        }
    }
    return atlas;
}

#ifdef MCWORLD_FAITHFUL_TEXTURE_DIR
[[nodiscard]] Image loadFaithfulAtlas() {
    constexpr std::array<const char*, kAtlasTiles> names{{
        "grass_block_top.png",
        "grass_block_side.png",
        "dirt.png",
        "stone.png",
        "deepslate.png",
    }};
    std::array<Image, kAtlasTiles> tiles{};
    int tileSize = 0;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const std::string path = std::string(MCWORLD_FAITHFUL_TEXTURE_DIR) + '/' + names[i];
        tiles[i] = LoadImage(path.c_str());
        if (!IsImageValid(tiles[i]) || tiles[i].width != tiles[i].height
            || (tileSize != 0 && tiles[i].width != tileSize)) {
            for (Image& tile : tiles) {
                if (IsImageValid(tile)) {
                    UnloadImage(tile);
                }
            }
            std::cerr << "Could not load the Faithful 32x block textures; using generated fallback textures.\n";
            return {};
        }
        tileSize = tiles[i].width;
    }

    Image atlas = GenImageColor(tileSize * kAtlasTiles, tileSize, WHITE);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const Rectangle source{0.0F, 0.0F, static_cast<float>(tileSize), static_cast<float>(tileSize)};
        const Rectangle destination{
            static_cast<float>(static_cast<int>(i) * tileSize), 0.0F,
            static_cast<float>(tileSize), static_cast<float>(tileSize),
        };
        ImageDraw(&atlas, tiles[i], source, destination, WHITE);
        UnloadImage(tiles[i]);
    }
    std::cout << "Voxel textures: Faithful 32x (https://faithfulpack.net)\n";
    return atlas;
}
#endif

} // namespace

VoxelMesh buildVoxelMesh(const mcworld::OverworldNoiseRouter& router, int chunkX, int chunkZ) {
    Occupancy occupancy;
    occupancy.surfaceY.fill(kMinY - 1);
    const std::int64_t originX = static_cast<std::int64_t>(chunkX) * kChunkSize;
    const std::int64_t originZ = static_cast<std::int64_t>(chunkZ) * kChunkSize;

    VoxelMesh mesh;
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            for (int y = kMinY; y < kMaxY; ++y) {
                const bool solid = router.sampleFinalDensity(
                    static_cast<double>(originX + x) + 0.5,
                    static_cast<double>(y) + 0.5,
                    static_cast<double>(originZ + z) + 0.5
                ) > 0.0F;
                occupancy.solid[Occupancy::index(x, y, z)] = solid ? 1 : 0;
                if (solid) {
                    ++mesh.solidVoxelCount;
                    occupancy.surfaceY[Occupancy::columnIndex(x, z)] = y;
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
    if (voxels.vertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Generated voxel mesh exceeds Raylib's vertex limit");
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(voxels.vertices.size());
    mesh.triangleCount = static_cast<int>(voxels.triangleCount());
    mesh.vertices = static_cast<float*>(MemAlloc(voxels.vertices.size() * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(voxels.vertices.size() * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(voxels.vertices.size() * 2 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(voxels.vertices.size() * 4 * sizeof(unsigned char)));
    if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.texcoords);
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
        mesh.texcoords[i * 2] = vertex.u;
        mesh.texcoords[i * 2 + 1] = vertex.v;
        mesh.colors[i * 4] = vertex.red;
        mesh.colors[i * 4 + 1] = vertex.green;
        mesh.colors[i * 4 + 2] = vertex.blue;
        mesh.colors[i * 4 + 3] = 255;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

VoxelTextureAtlas loadVoxelTextureAtlas() {
    Image atlas{};
    bool faithful = false;
#ifdef MCWORLD_FAITHFUL_TEXTURE_DIR
    atlas = loadFaithfulAtlas();
    faithful = IsImageValid(atlas);
#endif
    if (!IsImageValid(atlas)) {
        atlas = makeFallbackAtlas();
    }
    Texture2D texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    if (!IsTextureValid(texture)) {
        throw std::runtime_error("Could not upload the voxel texture atlas");
    }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    return {texture, faithful};
}

} // namespace viewer
