#include "voxel_renderer.hpp"
#include "terrain_surface_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

[[nodiscard]] std::array<std::uint8_t, 3> faceColor(const Normal& normal, mcworld::Block block) {
    constexpr Normal light{-0.45F, 0.82F, -0.35F};
    constexpr float lightLength = 0.99869913F;
    const float incidence = (normal.x * light.x + normal.y * light.y + normal.z * light.z) / lightLength;
    const float lighting = 0.42F + 0.58F * std::max(0.0F, incidence);
    using enum mcworld::Block;
    std::array<float,3> base{125,125,125};
    switch(block) {
        case Air: break;
        case Stone: base={128,128,128};break;
        case Water: base={40,105,210};break;
        case Lava: base={255,95,15};break;
        case Bedrock: base={45,45,45};break;
        case Deepslate: base={65,67,72};break;
        case Grass: base=normal.y>0?std::array<float,3>{95,155,55}:std::array<float,3>{125,94,62};break;
        case Dirt: base={133,96,67};break;
        case Sand: case Sandstone: base={220,205,145};break;
        case RedSand: case RedSandstone: base={195,104,46};break;
        case Gravel: base={144,136,133};break;
        case Terracotta: base={157,98,75};break;
        case OrangeTerracotta: base={164,84,38};break;
        case WhiteTerracotta: base={210,179,161};break;
        case YellowTerracotta: base={185,133,36};break;
        case BrownTerracotta: base={78,51,36};break;
        case RedTerracotta: base={144,60,46};break;
        case LightGrayTerracotta: base={135,107,98};break;
        case Podzol: case CoarseDirt: base={107,79,40};break;
        case Mycelium: base={124,106,128};break;
        case Mud: base={65,61,64};break;
        case Snow: case PowderSnow: base={240,248,250};break;
        case Ice: case PackedIce: base={140,182,245};break;
        case Calcite: base={222,220,211};break;
        case CopperOre: case RawCopper: base={175,117,77};break;
        case Granite: base={157,109,93};break;
        case DeepslateIronOre: case RawIron: base={146,126,108};break;
        case Tuff: base={111,114,102};break;
        case Sulfur: base={225,202,54};break;
        case Cinnabar: base={180,52,40};break;
    }
    const auto [red,green,blue]=base;
    return {
        colorChannel(red * lighting),
        colorChannel(green * lighting),
        colorChannel(blue * lighting),
    };
}

void appendColoredFace(VoxelMesh& mesh, const std::array<Position, 4>& corners, const Normal& normal, mcworld::Block block) {
    constexpr std::array<int, 6> indices{{0, 1, 2, 0, 2, 3}};
    const std::array<std::uint8_t, 3> color = faceColor(normal, block);

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
    }

    for (const SmoothSample& sample : {a, b, c}) {
        const Normal normal = normalize(sample.gradient * -1.0F);
        const auto color = faceColor(normal, fluid == mcworld::Block::Air ? sample.material : fluid);
        mesh.vertices.push_back({
            sample.position.x,
            sample.position.y,
            sample.position.z,
            normal.x,
            normal.y,
            normal.z,
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
        appendColoredFace(output,corners,normal,occupancy.at(x,y,z));
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

} // namespace

const mcworld::TerrainChunk& VoxelTerrain::chunk(int x, int z) {
    const auto key = std::pair{x, z};
    auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        it = chunks_.emplace(key, generator_.generate(x, z)).first;
    }
    return it->second;
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
    mesh.colors = static_cast<unsigned char*>(MemAlloc(terrain.vertices.size() * 4 * sizeof(unsigned char)));
    if (!mesh.vertices || !mesh.normals || !mesh.colors) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
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

} // namespace viewer
