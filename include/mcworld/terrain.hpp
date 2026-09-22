#pragma once

#include "mcworld/biome.hpp"
#include <array>
#include <memory>
#include <vector>

namespace mcworld {

enum class Block : std::uint8_t {
    Air, Stone, Water, Lava, Bedrock, Deepslate, Grass, Dirt, Sand, Sandstone,
    RedSand, RedSandstone, Gravel, Terracotta, WhiteTerracotta, OrangeTerracotta,
    Podzol, CoarseDirt, Mycelium, Mud, Snow, PowderSnow, Ice, PackedIce, Calcite,
    CopperOre, RawCopper, Granite, DeepslateIronOre, RawIron, Tuff, Sulfur, Cinnabar,
    YellowTerracotta, BrownTerracotta, RedTerracotta, LightGrayTerracotta
};
[[nodiscard]] constexpr bool isFluid(Block b) { return b==Block::Water || b==Block::Lava; }
[[nodiscard]] constexpr bool isSolid(Block b) { return b!=Block::Air && !isFluid(b); }
// The 26.3 BLOCKS_MOTION_IN_HEIGHTMAP tag excludes powder snow.
[[nodiscard]] constexpr bool blocksMotion(Block b) { return isSolid(b) && b!=Block::PowderSnow; }

struct BlockPosition { int x{}, y{}, z{}; bool operator==(const BlockPosition&) const = default; };

struct TerrainChunk {
    static constexpr int width=16, minY=-64, maxY=320, height=maxY-minY;
    int chunkX{}, chunkZ{};
    // z, x, y order; heightmaps store first free Y, or minY for empty columns.
    std::vector<Block> blocks = std::vector<Block>(width*width*height, Block::Air);
    std::array<Biome,4*4*(height/4)> biomes{};
    std::array<int,256> worldSurface{}, oceanFloor{}, motionBlocking{}, motionBlockingNoLeaves{};
    std::vector<BlockPosition> fluidPostProcessing; // local X/Z, world Y
    [[nodiscard]] Block at(int x,int y,int z) const;
    void set(int x,int y,int z,Block block);
    [[nodiscard]] Biome biomeAt(int x,int y,int z) const;
    void primeHeightmaps();
};

class BiomeSource {
public:
    virtual ~BiomeSource() = default;
    [[nodiscard]] virtual Biome sample(const OverworldNoiseRouter& router,int x,int y,int z) const;
};

struct TerrainOptions {
    bool aquifers=true;
    bool oreVeins=true;
    bool carvers=true;
    std::shared_ptr<const BiomeSource> biomes;
};

class OverworldTerrainGenerator {
public:
    // Router must outlive generator; one generator/router pair per thread.
    explicit OverworldTerrainGenerator(const OverworldNoiseRouter& router, TerrainOptions options={});
    ~OverworldTerrainGenerator();
    OverworldTerrainGenerator(OverworldTerrainGenerator&&) noexcept;
    OverworldTerrainGenerator& operator=(OverworldTerrainGenerator&&) noexcept;
    [[nodiscard]] TerrainChunk generate(int chunkX,int chunkZ);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mcworld
