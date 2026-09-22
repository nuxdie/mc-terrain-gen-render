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

const mcworld::TerrainChunk& VoxelTerrain::chunk(int x,int z) {
    auto key=std::pair{x,z};auto it=chunks_.find(key);
    if(it==chunks_.end()) it=chunks_.emplace(key,generator_.generate(x,z)).first;
    return it->second;
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
