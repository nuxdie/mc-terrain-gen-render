#include "voxel_renderer.hpp"
#include "terrain_surface_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

namespace viewer {
namespace {

constexpr int kChunkSize = 16;
constexpr int kMinY = -64;
constexpr int kMaxY = 320;
constexpr int kHeight = kMaxY - kMinY;

// Smooth extraction samples the generated block field at integer positions.
// One sample for reconstruction plus one for central-difference normals.
constexpr int kGridMin = -2;
constexpr int kGridMax = kChunkSize + 2;
constexpr int kGridSpan = kGridMax - kGridMin + 1;
constexpr int kGridMinY = kMinY - 2;
constexpr int kGridMaxY = kMaxY + 2;
constexpr int kGridHeight = kGridMaxY - kGridMinY + 1;
constexpr int kAtlasColumns = 8;
constexpr int kFallbackTileSize = 32;

enum class Tile : int {
    Bedrock,
    Calcite,
    Cinnabar,
    CoarseDirt,
    CopperOre,
    Deepslate,
    DeepslateIronOre,
    DeepslateTop,
    Dirt,
    Granite,
    GrassSide,
    GrassTop,
    Gravel,
    Ice,
    Lava,
    LightGrayTerracotta,
    Mud,
    MyceliumSide,
    MyceliumTop,
    OrangeTerracotta,
    PackedIce,
    PodzolSide,
    PodzolTop,
    PowderSnow,
    RawCopper,
    RawIron,
    RedSand,
    RedSandstoneSide,
    RedSandstoneBottom,
    RedSandstoneTop,
    RedTerracotta,
    Sand,
    SandstoneSide,
    SandstoneBottom,
    SandstoneTop,
    Snow,
    Stone,
    Sulfur,
    Terracotta,
    Tuff,
    Water,
    WhiteTerracotta,
    YellowTerracotta,
    BrownTerracotta,
    CoalOre,
    IronOre,
    GoldOre,
    RedstoneOre,
    DiamondOre,
    LapisOre,
    Clay,
    OakLog,
    OakLogTop,
    OakLeaves,
    OakPlanks,
    Cobblestone,
    MossyCobblestone,
    Bricks,
    Diorite,
    Andesite,
    EmeraldOre,
    DeepslateCoalOre,
    DeepslateGoldOre,
    DeepslateRedstoneOre,
    DeepslateDiamondOre,
    DeepslateLapisOre,
    DeepslateCopperOre,
    DeepslateEmeraldOre,
    Count,
};

constexpr int kAtlasTiles = static_cast<int>(Tile::Count);
constexpr int kAtlasRows = (kAtlasTiles + kAtlasColumns - 1) / kAtlasColumns;

struct Occupancy {
    std::array<mcworld::Block, (kChunkSize + 2) * kHeight * (kChunkSize + 2)> blocks{};

    [[nodiscard]] mcworld::Block at(int x, int y, int z) const {
        if (x < -1 || x > kChunkSize || y < kMinY || y >= kMaxY || z < -1 || z > kChunkSize) {
            return mcworld::Block::Air;
        }
        return blocks[index(x, y, z)];
    }

    [[nodiscard]] bool exposed(int x,int y,int z,int dx,int dy,int dz) const {
        auto own=at(x,y,z),neighbor=at(x+dx,y+dy,z+dz);
        return neighbor==mcworld::Block::Air || (mcworld::isSolid(own) && mcworld::isFluid(neighbor)) ||
            (own==mcworld::Block::Lava && neighbor==mcworld::Block::Water);
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

struct SmoothSample {
    Position position;
    Normal gradient;
    float density{};
    mcworld::Block material{mcworld::Block::Air};
};

struct BlockGrid {
    std::array<mcworld::Block, kGridSpan * kGridHeight * kGridSpan> blocks{};

    [[nodiscard]] mcworld::Block at(int x, int y, int z) const {
        if (x < kGridMin || x > kGridMax || y < kGridMinY || y > kGridMaxY ||
            z < kGridMin || z > kGridMax) {
            return mcworld::Block::Air;
        }
        return blocks[index(x, y, z)];
    }

    void set(int x, int y, int z, mcworld::Block block) {
        blocks[index(x, y, z)] = block;
    }

private:
    [[nodiscard]] static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(
            ((z - kGridMin) * kGridSpan + x - kGridMin) * kGridHeight + y - kGridMinY
        );
    }
};

[[nodiscard]] Position operator+(const Position& left, const Position& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Position operator-(const Position& left, const Position& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Position operator*(const Position& value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] Normal operator+(const Normal& left, const Normal& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Normal operator*(const Normal& value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] float dot(const Position& left, const Position& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] float dot(const Position& left, const Normal& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] Position cross(const Position& left, const Position& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] Normal normalize(const Normal& value) {
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (lengthSquared <= 1.0e-12F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return value * (1.0F / std::sqrt(lengthSquared));
}

[[nodiscard]] unsigned char colorChannel(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0F, 255.0F));
}

[[nodiscard]] Tile faceTexture(mcworld::Block block, const Normal& normal) {
    const bool top = normal.y > 0.5F;
    const bool bottom = normal.y < -0.5F;
    using enum mcworld::Block;
    switch (block) {
    case Air: return Tile::Stone;
    case Stone: return Tile::Stone;
    case Water: return Tile::Water;
    case Lava: return Tile::Lava;
    case Bedrock: return Tile::Bedrock;
    case Deepslate: return top || bottom ? Tile::DeepslateTop : Tile::Deepslate;
    case Grass: return top ? Tile::GrassTop : (bottom ? Tile::Dirt : Tile::GrassSide);
    case Dirt: return Tile::Dirt;
    case Sand: return Tile::Sand;
    case Sandstone: return top ? Tile::SandstoneTop : (bottom ? Tile::SandstoneBottom : Tile::SandstoneSide);
    case RedSand: return Tile::RedSand;
    case RedSandstone:
        return top ? Tile::RedSandstoneTop : (bottom ? Tile::RedSandstoneBottom : Tile::RedSandstoneSide);
    case Gravel: return Tile::Gravel;
    case Terracotta: return Tile::Terracotta;
    case WhiteTerracotta: return Tile::WhiteTerracotta;
    case OrangeTerracotta: return Tile::OrangeTerracotta;
    case Podzol: return top ? Tile::PodzolTop : (bottom ? Tile::Dirt : Tile::PodzolSide);
    case CoarseDirt: return Tile::CoarseDirt;
    case Mycelium: return top ? Tile::MyceliumTop : (bottom ? Tile::Dirt : Tile::MyceliumSide);
    case Mud: return Tile::Mud;
    case Snow: return Tile::Snow;
    case PowderSnow: return Tile::PowderSnow;
    case Ice: return Tile::Ice;
    case PackedIce: return Tile::PackedIce;
    case Calcite: return Tile::Calcite;
    case CopperOre: return Tile::CopperOre;
    case RawCopper: return Tile::RawCopper;
    case Granite: return Tile::Granite;
    case DeepslateIronOre: return Tile::DeepslateIronOre;
    case RawIron: return Tile::RawIron;
    case Tuff: return Tile::Tuff;
    case Sulfur: return Tile::Sulfur;
    case Cinnabar: return Tile::Cinnabar;
    case YellowTerracotta: return Tile::YellowTerracotta;
    case BrownTerracotta: return Tile::BrownTerracotta;
    case RedTerracotta: return Tile::RedTerracotta;
    case LightGrayTerracotta: return Tile::LightGrayTerracotta;
    case CoalOre: return Tile::CoalOre;
    case IronOre: return Tile::IronOre;
    case GoldOre: return Tile::GoldOre;
    case RedstoneOre: return Tile::RedstoneOre;
    case DiamondOre: return Tile::DiamondOre;
    case LapisOre: return Tile::LapisOre;
    case Clay: return Tile::Clay;
    case OakLog: return top || bottom ? Tile::OakLogTop : Tile::OakLog;
    case OakLeaves: return Tile::OakLeaves;
    case OakPlanks: return Tile::OakPlanks;
    case Cobblestone: return Tile::Cobblestone;
    case MossyCobblestone: return Tile::MossyCobblestone;
    case Bricks: return Tile::Bricks;
    case Diorite: return Tile::Diorite;
    case Andesite: return Tile::Andesite;
    case EmeraldOre: return Tile::EmeraldOre;
    case DeepslateCoalOre: return Tile::DeepslateCoalOre;
    case DeepslateGoldOre: return Tile::DeepslateGoldOre;
    case DeepslateRedstoneOre: return Tile::DeepslateRedstoneOre;
    case DeepslateDiamondOre: return Tile::DeepslateDiamondOre;
    case DeepslateLapisOre: return Tile::DeepslateLapisOre;
    case DeepslateCopperOre: return Tile::DeepslateCopperOre;
    case DeepslateEmeraldOre: return Tile::DeepslateEmeraldOre;
    }
    return Tile::Stone;
}

[[nodiscard]] std::array<float, 2> atlasCoordinates(Tile tile, float u, float v) {
    constexpr float inset = 0.5F / static_cast<float>(kFallbackTileSize);
    const int index = static_cast<int>(tile);
    const float column = static_cast<float>(index % kAtlasColumns);
    const float row = static_cast<float>(index / kAtlasColumns);
    const float localU = std::lerp(inset, 1.0F - inset, std::clamp(u, 0.0F, 1.0F));
    const float localV = std::lerp(inset, 1.0F - inset, std::clamp(v, 0.0F, 1.0F));
    return {(column + localU) / kAtlasColumns, (row + localV) / kAtlasRows};
}

[[nodiscard]] std::array<std::uint8_t, 3> faceColor(
    const Normal& normal,
    mcworld::Block block,
    Tile tile
) {
    constexpr Normal light{-0.45F, 0.82F, -0.35F};
    constexpr float lightLength = 0.99869913F;
    const float incidence = (normal.x * light.x + normal.y * light.y + normal.z * light.z) / lightLength;
    const float lighting = 0.42F + 0.58F * std::max(0.0F, incidence);
    std::array<float, 3> tint{255.0F, 255.0F, 255.0F};
    if (tile == Tile::GrassTop) {
        tint = {115.0F, 185.0F, 78.0F};
    } else if (tile == Tile::OakLeaves) {
        tint = {119.0F, 171.0F, 47.0F};
    } else if (block == mcworld::Block::Water) {
        tint = {55.0F, 125.0F, 235.0F};
    }
    const auto [red, green, blue] = tint;
    return {
        colorChannel(red * lighting),
        colorChannel(green * lighting),
        colorChannel(blue * lighting),
    };
}

void appendTexturedFace(
    VoxelMesh& mesh,
    const std::array<Position, 4>& corners,
    const Normal& normal,
    mcworld::Block block
) {
    constexpr std::array<int, 6> indices{{0, 1, 2, 0, 2, 3}};
    constexpr std::array<std::array<float, 2>, 4> texcoords{{
        {{0.0F, 1.0F}}, {{0.0F, 0.0F}}, {{1.0F, 0.0F}}, {{1.0F, 1.0F}},
    }};
    const Tile tile = faceTexture(block, normal);
    const std::array<std::uint8_t, 3> color = faceColor(normal, block, tile);

    for (const int index : indices) {
        const Position& position = corners[static_cast<std::size_t>(index)];
        const auto& local = texcoords[static_cast<std::size_t>(index)];
        const auto uv = atlasCoordinates(tile, local[0], local[1]);
        mesh.vertices.push_back({
            position.x,
            position.y,
            position.z,
            normal.x,
            normal.y,
            normal.z,
            uv[0],
            uv[1],
            color[0],
            color[1],
            color[2],
        });
    }
}

[[nodiscard]] SmoothSample interpolate(const SmoothSample& from, const SmoothSample& to) {
    // Shared edges must evaluate in the same order on either side of a chunk.
    if (std::tie(from.position.x, from.position.y, from.position.z) >
        std::tie(to.position.x, to.position.y, to.position.z)) return interpolate(to, from);
    const float amount = std::clamp(from.density / (from.density - to.density), 0.0F, 1.0F);
    return {
        from.position + (to.position - from.position) * amount,
        from.gradient * (1.0F - amount) + to.gradient * amount,
        0.0F,
        from.density > 0.0F ? from.material : to.material,
    };
}

void appendSmoothTriangle(SmoothTerrainMesh& mesh, SmoothSample a, SmoothSample b, SmoothSample c,
                          const Normal& outward, mcworld::Block fluid) {
    Position faceNormal = cross(b.position - a.position, c.position - a.position);
    if (dot(faceNormal, faceNormal) == 0.0F) {
        return;
    }

    if (dot(faceNormal, outward) < 0.0F) {
        std::swap(b, c);
        faceNormal = cross(b.position - a.position, c.position - a.position);
    }

    mcworld::Block material = a.material;
    if (b.material == c.material || b.material == a.material) {
        material = b.material;
    } else if (c.material == a.material) {
        material = c.material;
    }
    if (fluid != mcworld::Block::Air) {
        material = fluid;
    }
    const Normal faceDirection = normalize({faceNormal.x, faceNormal.y, faceNormal.z});
    const Tile tile = faceTexture(material, faceDirection);
    const float ax = std::abs(faceDirection.x);
    const float ay = std::abs(faceDirection.y);
    const float az = std::abs(faceDirection.z);
    const auto projected = [&](const Position& position) {
        if (ay >= ax && ay >= az) return std::array{position.x, position.z};
        if (ax >= az) return std::array{position.z, position.y};
        return std::array{position.x, position.y};
    };
    const auto pa = projected(a.position);
    const auto pb = projected(b.position);
    const auto pc = projected(c.position);
    const float baseU = std::floor(std::min({pa[0], pb[0], pc[0]}));
    const float baseV = std::floor(std::min({pa[1], pb[1], pc[1]}));

    for (const SmoothSample& sample : {a, b, c}) {
        const Normal normal = normalize(sample.gradient * -1.0F);
        const auto local = projected(sample.position);
        const auto uv = atlasCoordinates(tile, local[0] - baseU, local[1] - baseV);
        const mcworld::Block vertexMaterial = fluid == mcworld::Block::Air ? sample.material : fluid;
        const auto color = faceColor(normal, vertexMaterial, faceTexture(vertexMaterial, normal));
        mesh.vertices.push_back({
            sample.position.x,
            sample.position.y,
            sample.position.z,
            normal.x,
            normal.y,
            normal.z,
            uv[0],
            uv[1],
            color[0],
            color[1],
            color[2],
        });
    }
}

void polygonizeTetrahedron(SmoothTerrainMesh& mesh, const std::array<SmoothSample, 4>& corners,
                          mcworld::Block fluid) {
    std::array<const SmoothSample*, 4> solid{};
    std::array<const SmoothSample*, 4> air{};
    std::size_t solidCount = 0;
    std::size_t airCount = 0;
    for (const SmoothSample& corner : corners) {
        if (corner.density > 0.0F) {
            solid[solidCount++] = &corner;
        } else {
            air[airCount++] = &corner;
        }
    }

    if (solidCount == 0 || solidCount == 4) return;
    Position inside{}, outside{};
    for (std::size_t i = 0; i < solidCount; ++i) inside = inside + solid[i]->position;
    for (std::size_t i = 0; i < airCount; ++i) outside = outside + air[i]->position;
    const Position direction = outside * (1.0F / airCount) - inside * (1.0F / solidCount);
    const Normal outward{direction.x, direction.y, direction.z};

    switch (solidCount) {
    case 0:
    case 4:
        return;
    case 1:
        appendSmoothTriangle(
            mesh,
            interpolate(*solid[0], *air[0]),
            interpolate(*solid[0], *air[1]),
            interpolate(*solid[0], *air[2]), outward, fluid
        );
        return;
    case 3:
        appendSmoothTriangle(
            mesh,
            interpolate(*air[0], *solid[0]),
            interpolate(*air[0], *solid[1]),
            interpolate(*air[0], *solid[2]), outward, fluid
        );
        return;
    default: {
        const SmoothSample a = interpolate(*solid[0], *air[0]);
        const SmoothSample b = interpolate(*solid[0], *air[1]);
        const SmoothSample c = interpolate(*solid[1], *air[0]);
        const SmoothSample d = interpolate(*solid[1], *air[1]);
        appendSmoothTriangle(mesh, a, c, b, outward, fluid);
        appendSmoothTriangle(mesh, b, c, d, outward, fluid);
    }
    }
}

constexpr std::array<std::array<int, 3>, 8> kCornerOffsets{{
    {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
    {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}},
}};

constexpr std::array<std::array<int, 4>, 6> kTetrahedra{{
    {{0, 5, 1, 6}}, {{0, 1, 2, 6}}, {{0, 2, 3, 6}},
    {{0, 3, 7, 6}}, {{0, 7, 4, 6}}, {{0, 4, 5, 6}},
}};

void appendExposedFaces(VoxelMesh& mesh, const Occupancy& occupancy, int x, int y, int z) {
    auto appendFace=[&](VoxelMesh& output,const std::array<Position,4>& corners,const Normal& normal) {
        appendTexturedFace(output,corners,normal,occupancy.at(x,y,z));
    };
    const float x0 = static_cast<float>(x);
    const float x1 = x0 + 1.0F;
    const float y0 = static_cast<float>(y);
    const float y1 = y0 + 1.0F;
    const float z0 = static_cast<float>(z);
    const float z1 = z0 + 1.0F;

    if (occupancy.exposed(x,y,z,1,0,0)) {
        appendFace(mesh, {{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}}, {1, 0, 0});
    }
    if (occupancy.exposed(x,y,z,-1,0,0)) {
        appendFace(mesh, {{{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}}, {-1, 0, 0});
    }
    if (occupancy.exposed(x,y,z,0,1,0)) {
        appendFace(mesh, {{{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}}}, {0, 1, 0});
    }
    if (occupancy.exposed(x,y,z,0,-1,0)) {
        appendFace(mesh, {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}}, {0, -1, 0});
    }
    if (occupancy.exposed(x,y,z,0,0,1)) {
        appendFace(mesh, {{{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}}, {0, 0, 1});
    }
    if (occupancy.exposed(x,y,z,0,0,-1)) {
        appendFace(mesh, {{{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}}, {0, 0, -1});
    }
}

[[nodiscard]] Color fallbackColor(Tile tile) {
    switch (tile) {
    case Tile::Bedrock: return {50, 50, 50, 255};
    case Tile::Calcite: return {220, 218, 208, 255};
    case Tile::Cinnabar: return {184, 50, 40, 255};
    case Tile::CoarseDirt: return {108, 78, 43, 255};
    case Tile::CopperOre: return {145, 112, 91, 255};
    case Tile::Deepslate: case Tile::DeepslateTop: return {67, 68, 72, 255};
    case Tile::DeepslateIronOre: return {105, 97, 91, 255};
    case Tile::Dirt: return {133, 96, 67, 255};
    case Tile::Granite: return {157, 109, 93, 255};
    case Tile::GrassSide: return {120, 104, 65, 255};
    case Tile::GrassTop: return {210, 210, 210, 255};
    case Tile::Gravel: return {144, 136, 133, 255};
    case Tile::Ice: return {150, 190, 245, 255};
    case Tile::Lava: return {255, 102, 18, 255};
    case Tile::LightGrayTerracotta: return {135, 107, 98, 255};
    case Tile::Mud: return {65, 61, 64, 255};
    case Tile::MyceliumSide: return {111, 88, 70, 255};
    case Tile::MyceliumTop: return {124, 106, 128, 255};
    case Tile::OrangeTerracotta: return {164, 84, 38, 255};
    case Tile::PackedIce: return {132, 171, 224, 255};
    case Tile::PodzolSide: return {111, 82, 52, 255};
    case Tile::PodzolTop: return {107, 79, 40, 255};
    case Tile::PowderSnow: case Tile::Snow: return {240, 248, 250, 255};
    case Tile::RawCopper: return {157, 91, 65, 255};
    case Tile::RawIron: return {146, 126, 108, 255};
    case Tile::RedSand: return {190, 102, 42, 255};
    case Tile::RedSandstoneSide: case Tile::RedSandstoneBottom: case Tile::RedSandstoneTop:
        return {184, 98, 43, 255};
    case Tile::RedTerracotta: return {144, 60, 46, 255};
    case Tile::Sand: return {220, 205, 145, 255};
    case Tile::SandstoneSide: case Tile::SandstoneBottom: case Tile::SandstoneTop:
        return {216, 202, 151, 255};
    case Tile::Stone: return {128, 128, 128, 255};
    case Tile::Sulfur: return {225, 202, 54, 255};
    case Tile::Terracotta: return {157, 98, 75, 255};
    case Tile::Tuff: return {111, 114, 102, 255};
    case Tile::Water: return {205, 205, 205, 255};
    case Tile::WhiteTerracotta: return {210, 179, 161, 255};
    case Tile::YellowTerracotta: return {185, 133, 36, 255};
    case Tile::BrownTerracotta: return {78, 51, 36, 255};
    case Tile::CoalOre: return {95, 95, 95, 255};
    case Tile::IronOre: return {145, 132, 120, 255};
    case Tile::GoldOre: return {160, 148, 93, 255};
    case Tile::RedstoneOre: return {145, 95, 95, 255};
    case Tile::DiamondOre: return {105, 161, 157, 255};
    case Tile::LapisOre: return {90, 112, 150, 255};
    case Tile::Clay: return {160, 166, 179, 255};
    case Tile::OakLog: return {109, 85, 50, 255};
    case Tile::OakLogTop: return {151, 122, 73, 255};
    case Tile::OakLeaves: return {180, 180, 180, 255};
    case Tile::OakPlanks: return {162, 130, 78, 255};
    case Tile::Cobblestone: return {120, 120, 120, 255};
    case Tile::MossyCobblestone: return {104, 119, 88, 255};
    case Tile::Bricks: return {151, 98, 83, 255};
    case Tile::Diorite: return {188, 188, 188, 255};
    case Tile::Andesite: return {136, 136, 136, 255};
    case Tile::EmeraldOre: return {100, 148, 116, 255};
    case Tile::DeepslateCoalOre: return {55, 56, 60, 255};
    case Tile::DeepslateGoldOre: return {116, 105, 65, 255};
    case Tile::DeepslateRedstoneOre: return {109, 58, 61, 255};
    case Tile::DeepslateDiamondOre: return {65, 125, 124, 255};
    case Tile::DeepslateLapisOre: return {53, 72, 111, 255};
    case Tile::DeepslateCopperOre: return {109, 88, 71, 255};
    case Tile::DeepslateEmeraldOre: return {61, 110, 80, 255};
    case Tile::Count: break;
    }
    return MAGENTA;
}

[[nodiscard]] Image makeFallbackAtlas() {
    Image atlas = GenImageColor(kFallbackTileSize * kAtlasColumns, kFallbackTileSize * kAtlasRows, BLANK);
    for (int tile = 0; tile < kAtlasTiles; ++tile) {
        const Color base = fallbackColor(static_cast<Tile>(tile));
        for (int y = 0; y < kFallbackTileSize; ++y) {
            for (int x = 0; x < kFallbackTileSize; ++x) {
                const std::uint32_t hash = static_cast<std::uint32_t>(x * 73856093U)
                    ^ static_cast<std::uint32_t>(y * 19349663U)
                    ^ static_cast<std::uint32_t>(tile * 83492791U);
                const float variation = 0.88F + static_cast<float>(hash & 15U) / 100.0F;
                const Color color{
                    colorChannel(base.r * variation),
                    colorChannel(base.g * variation),
                    colorChannel(base.b * variation),
                    255,
                };
                ImageDrawPixel(
                    &atlas,
                    (tile % kAtlasColumns) * kFallbackTileSize + x,
                    (tile / kAtlasColumns) * kFallbackTileSize + y,
                    color
                );
            }
        }
    }
    return atlas;
}

#ifdef MCWORLD_FAITHFUL_TEXTURE_DIR
[[nodiscard]] Image loadFaithfulAtlas() {
    constexpr std::array<const char*, kAtlasTiles> names{{
        "bedrock.png", "calcite.png", "cinnabar.png", "coarse_dirt.png", "copper_ore.png",
        "deepslate.png", "deepslate_iron_ore.png", "deepslate_top.png", "dirt.png", "granite.png",
        "grass_block_side.png", "grass_block_top.png", "gravel.png", "ice.png", "lava_still.png",
        "light_gray_terracotta.png", "mud.png", "mycelium_side.png", "mycelium_top.png",
        "orange_terracotta.png", "packed_ice.png", "podzol_side.png", "podzol_top.png",
        "powder_snow.png", "raw_copper_block.png", "raw_iron_block.png", "red_sand.png",
        "red_sandstone.png", "red_sandstone_bottom.png", "red_sandstone_top.png", "red_terracotta.png",
        "sand.png", "sandstone.png", "sandstone_bottom.png", "sandstone_top.png", "snow.png",
        "stone.png", "sulfur.png", "terracotta.png", "tuff.png", "water_still.png",
        "white_terracotta.png", "yellow_terracotta.png", "brown_terracotta.png",
        "coal_ore.png", "iron_ore.png", "gold_ore.png", "redstone_ore.png", "diamond_ore.png",
        "lapis_ore.png", "clay.png", "oak_log.png", "oak_log_top.png", "oak_leaves.png",
        "oak_planks.png", "cobblestone.png", "mossy_cobblestone.png", "bricks.png",
        "diorite.png", "andesite.png", "emerald_ore.png", "deepslate_coal_ore.png",
        "deepslate_gold_ore.png", "deepslate_redstone_ore.png", "deepslate_diamond_ore.png",
        "deepslate_lapis_ore.png", "deepslate_copper_ore.png", "deepslate_emerald_ore.png",
    }};
    std::array<Image, kAtlasTiles> tiles{};
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const std::string path = std::string(MCWORLD_FAITHFUL_TEXTURE_DIR) + '/' + names[i];
        tiles[i] = LoadImage(path.c_str());
        if (!IsImageValid(tiles[i]) || tiles[i].width != kFallbackTileSize
            || tiles[i].height < kFallbackTileSize) {
            for (Image& tile : tiles) {
                if (IsImageValid(tile)) UnloadImage(tile);
            }
            std::cerr << "Could not load the Faithful 32x terrain textures; using generated fallbacks.\n";
            return {};
        }
    }

    Image atlas = GenImageColor(kFallbackTileSize * kAtlasColumns, kFallbackTileSize * kAtlasRows, BLANK);
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const Rectangle source{0.0F, 0.0F, kFallbackTileSize, kFallbackTileSize};
        const Rectangle destination{
            static_cast<float>(static_cast<int>(i) % kAtlasColumns * kFallbackTileSize),
            static_cast<float>(static_cast<int>(i) / kAtlasColumns * kFallbackTileSize),
            kFallbackTileSize,
            kFallbackTileSize,
        };
        ImageDraw(&atlas, tiles[i], source, destination, WHITE);
        UnloadImage(tiles[i]);
    }
    return atlas;
}
#endif

} // namespace

const mcworld::TerrainChunk& VoxelTerrain::chunk(int x, int z) {
    const auto key = std::pair{x, z};
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        it = chunks_.emplace(key, terrainGenerator_.generate(x, z)).first;
    }
    return it->second;
}

void VoxelTerrain::prepareArea(int firstChunkX, int firstChunkZ, int width, int depth) {
    auto area = worldGenerator_.generateArea(firstChunkX, firstChunkZ, width, depth);
    for (auto& generated : area.chunks) {
        const auto key = std::pair{generated.terrain.chunkX, generated.terrain.chunkZ};
        chunks_.insert_or_assign(key, std::move(generated.terrain));
    }
}

SmoothTerrainMesh VoxelTerrain::buildSmoothMesh(int chunkX, int chunkZ) {
    BlockGrid grid;
    for (int z = kGridMin; z <= kGridMax; ++z) {
        for (int x = kGridMin; x <= kGridMax; ++x) {
            const int dx = x < 0 ? -1 : (x >= kChunkSize ? 1 : 0);
            const int dz = z < 0 ? -1 : (z >= kChunkSize ? 1 : 0);
            const mcworld::TerrainChunk& terrain = chunk(chunkX + dx, chunkZ + dz);
            for (int y = kMinY; y < kMaxY; ++y) {
                grid.set(x, y, z, terrain.at(x - dx * kChunkSize, y, z - dz * kChunkSize));
            }
        }
    }

    // Reconstruct a smooth correction from the final blocks, including carvers,
    // barriers and material extensions. The original 7A field is not filtered.
    constexpr int densityHeight = kGridHeight;
    std::vector<float> densities(kGridSpan * kGridSpan * densityHeight);
    std::vector<float> corrections(densities.size());
    const auto densityIndex = [](int x, int y, int z) {
        return ((z - kGridMin) * kGridSpan + x - kGridMin) * densityHeight + y - kGridMinY;
    };
    for (int z = kGridMin; z <= kGridMax; ++z) for (int x = kGridMin; x <= kGridMax; ++x) {
        for (int y = kGridMinY; y <= kGridMaxY; ++y) {
            const float raw = router_.sampleFinalDensity(
                static_cast<double>(chunkX) * 16 + x, y, static_cast<double>(chunkZ) * 16 + z);
            const bool solid = mcworld::isSolid(grid.at(x, y, z));
            densities[densityIndex(x, y, z)] = raw;
            corrections[densityIndex(x, y, z)] = detail::terrainCorrection(raw, solid);
        }
    }
    const auto correctionAt = [&](int x, int y, int z) {
        return corrections[densityIndex(x, y, z)];
    };
    for (int z = -1; z <= 17; ++z) for (int x = -1; x <= 17; ++x) {
        for (int y = kMinY - 1; y <= kMaxY + 1; ++y) {
            densities[densityIndex(x, y, z)] += detail::filteredCorrection(x, y, z, correctionAt);
        }
    }

    SmoothTerrainMesh mesh;
    mesh.vertices.reserve(100'000);
    for (const auto phase : {mcworld::Block::Air, mcworld::Block::Water, mcworld::Block::Lava}) {
        const auto densityAt = [&](int x, int y, int z) {
            if (phase == mcworld::Block::Air) return densities[densityIndex(x, y, z)];
            // Each fluid is its own volume, never merged with rock. This keeps
            // shoreline topology independent of the chunk being extracted.
            return grid.at(x, y, z) == phase ? 1.0F : -1.0F;
        };
        const auto sampleAt = [&](int x, int y, int z) {
            auto material = grid.at(x, y, z);
            if (phase == mcworld::Block::Air && !mcworld::isSolid(material) && densityAt(x, y, z) > 0) {
                // Reconstruction can move the surface into a formerly empty
                // sample. Borrow the nearest actual solid material, not air or
                // fluid, with deterministic ties across chunk boundaries.
                int nearest = 4;
                for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        const auto candidate = grid.at(x + dx, y + dy, z + dz);
                        const int distance = dx * dx + dy * dy + dz * dz;
                        if (mcworld::isSolid(candidate) && distance < nearest) {
                            nearest = distance;
                            material = candidate;
                        }
                    }
                }
            }
            return SmoothSample{
                {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                {
                    densityAt(x + 1, y, z) - densityAt(x - 1, y, z),
                    densityAt(x, y + 1, z) - densityAt(x, y - 1, z),
                    densityAt(x, y, z + 1) - densityAt(x, y, z - 1),
                },
                densityAt(x, y, z),
                material,
            };
        };

        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                for (int y = kMinY; y < kMaxY; ++y) {
                    std::array<SmoothSample, 8> cube{};
                    for (std::size_t corner = 0; corner < cube.size(); ++corner) {
                        const auto& offset = kCornerOffsets[corner];
                        cube[corner] = sampleAt(x + offset[0], y + offset[1], z + offset[2]);
                    }
                    for (const auto& tetrahedron : kTetrahedra) {
                        polygonizeTetrahedron(mesh, {{
                            cube[static_cast<std::size_t>(tetrahedron[0])],
                            cube[static_cast<std::size_t>(tetrahedron[1])],
                            cube[static_cast<std::size_t>(tetrahedron[2])],
                            cube[static_cast<std::size_t>(tetrahedron[3])],
                        }}, phase);
                    }
                }
            }
        }
    }
    return mesh;
}

VoxelMesh VoxelTerrain::buildMesh(int chunkX, int chunkZ) {
    Occupancy occupancy;

    VoxelMesh mesh;
    // Fetch generated neighbors, including materials and carvers, to hide internal faces.
    for (int z = -1; z <= kChunkSize; ++z) {
        for (int x = -1; x <= kChunkSize; ++x) {
            if((x==-1 || x==16) && (z==-1 || z==16)) continue; // corners never queried
            int dx=x<0?-1:(x>=16?1:0),dz=z<0?-1:(z>=16?1:0);
            const auto& terrain=chunk(chunkX+dx,chunkZ+dz);
            for (int y = kMinY; y < kMaxY; ++y) {
                auto block=terrain.at(x-dx*16,y,z-dz*16);
                occupancy.blocks[Occupancy::index(x, y, z)] = block;
                if (x >= 0 && x < kChunkSize && z >= 0 && z < kChunkSize) {
                    if(mcworld::isSolid(block)) ++mesh.solidBlockCount;
                    if(block==mcworld::Block::Water) ++mesh.waterBlockCount;
                    if(block==mcworld::Block::Lava) ++mesh.lavaBlockCount;
                }
            }
        }
    }

    mesh.vertices.reserve(250'000);
    for (int z = 0; z < kChunkSize; ++z) {
        for (int x = 0; x < kChunkSize; ++x) {
            for (int y = kMinY; y < kMaxY; ++y) {
                if (occupancy.at(x, y, z)!=mcworld::Block::Air) {
                    appendExposedFaces(mesh, occupancy, x, y, z);
                }
            }
        }
    }
    return mesh;
}

namespace {

template <typename TerrainMesh>
Mesh uploadTerrainMesh(const TerrainMesh& terrain) {
    if (terrain.vertices.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || terrain.vertices.size() > std::numeric_limits<unsigned int>::max() / (3 * sizeof(float))) {
        throw std::runtime_error("Generated terrain mesh exceeds Raylib's vertex limit");
    }

    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(terrain.vertices.size());
    mesh.triangleCount = static_cast<int>(terrain.triangleCount());
    mesh.vertices = static_cast<float*>(MemAlloc(terrain.vertices.size() * 3 * sizeof(float)));
    mesh.normals = static_cast<float*>(MemAlloc(terrain.vertices.size() * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float*>(MemAlloc(terrain.vertices.size() * 2 * sizeof(float)));
    mesh.colors = static_cast<unsigned char*>(MemAlloc(terrain.vertices.size() * 4 * sizeof(unsigned char)));
    if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.colors) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        MemFree(mesh.texcoords);
        MemFree(mesh.colors);
        throw std::runtime_error("Could not allocate viewer terrain mesh buffers");
    }

    for (std::size_t i = 0; i < terrain.vertices.size(); ++i) {
        const VoxelVertex& vertex = terrain.vertices[i];
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

} // namespace

Mesh uploadSmoothTerrainMesh(const SmoothTerrainMesh& terrain) {
    return uploadTerrainMesh(terrain);
}

Mesh uploadVoxelMesh(const VoxelMesh& voxels) {
    return uploadTerrainMesh(voxels);
}

TerrainTextureAtlas loadTerrainTextureAtlas() {
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
        throw std::runtime_error("Could not upload the terrain texture atlas");
    }
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    return {texture, faithful};
}

} // namespace viewer
