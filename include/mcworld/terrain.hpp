#pragma once

// Public API for step 7B of `minecraft-26.3-worldgen.dot`: generating the
// blocks of an Overworld chunk from the density graph in `worldgen.hpp`.

#include "mcworld/biome.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mcworld {

// The terrain material palette. This is not the full block-state registry:
// it covers what the density fill, the material rules and the carvers can
// produce, which is everything terrain generation needs.
enum class Block : std::uint8_t {
    Air, Stone, Water, Lava, Bedrock, Deepslate, Grass, Dirt, Sand, Sandstone,
    RedSand, RedSandstone, Gravel, Terracotta, WhiteTerracotta, OrangeTerracotta,
    Podzol, CoarseDirt, Mycelium, Mud, Snow, PowderSnow, Ice, PackedIce, Calcite,
    CopperOre, RawCopper, Granite, DeepslateIronOre, RawIron, Tuff, Sulfur, Cinnabar,
    YellowTerracotta, BrownTerracotta, RedTerracotta, LightGrayTerracotta
};

[[nodiscard]] constexpr bool isFluid(Block block) {
    return block == Block::Water || block == Block::Lava;
}

[[nodiscard]] constexpr bool isSolid(Block block) {
    return block != Block::Air && !isFluid(block);
}

// The 26.3 `BLOCKS_MOTION_IN_HEIGHTMAP` tag excludes powder snow.
[[nodiscard]] constexpr bool blocksMotion(Block block) {
    return isSolid(block) && block != Block::PowderSnow;
}

struct BlockPosition {
    int x{};
    int y{};
    int z{};

    bool operator==(const BlockPosition&) const = default;
};

// One generated chunk.
//
// Coordinates are chunk-local in X and Z (0..15) and world Y, which is the
// convention the whole terrain implementation uses. Chunk boundaries are left
// open: neighbouring chunks are generated independently and agree by
// construction, not by being stitched together.
struct TerrainChunk {
    static constexpr int width = 16;
    static constexpr int minY = -64;
    static constexpr int maxY = 320;
    static constexpr int height = maxY - minY;

    int chunkX{};
    int chunkZ{};

    // Blocks in z, x, y order.
    std::vector<Block> blocks = std::vector<Block>(width * width * height, Block::Air);
    // Biome palette, one entry per 4x4x4 cell, in the same z, x, y order.
    std::array<Biome, 4 * 4 * (height / 4)> biomes{};

    // Heightmaps store the first free Y above the topmost qualifying block, or
    // `minY` for a column that has none.
    std::array<int, 256> worldSurface{};
    std::array<int, 256> oceanFloor{};
    std::array<int, 256> motionBlocking{};
    std::array<int, 256> motionBlockingNoLeaves{};

    // Positions queued for fluid post-processing, in local X/Z and world Y.
    // Queued, not simulated: the caller decides what to do with them.
    std::vector<BlockPosition> fluidPostProcessing;

    [[nodiscard]] Block at(int x, int y, int z) const;
    void set(int x, int y, int z, Block block);
    [[nodiscard]] Biome biomeAt(int x, int y, int z) const;

    // Recomputes every heightmap from the current blocks.
    void primeHeightmaps();
};

// Supplies the biome palette. The default is the standard multi-noise
// Overworld source; override it to pin biomes for tests or previews.
//
// Implementations must be stable: the same position has to resolve to the same
// biome for the lifetime of a generator, including for positions outside the
// chunk being generated.
class BiomeSource {
public:
    virtual ~BiomeSource() = default;

    [[nodiscard]] virtual Biome sample(const OverworldNoiseRouter& router, int x, int y, int z) const;
};

struct TerrainOptions {
    bool aquifers = true;
    bool oreVeins = true;
    bool carvers = true;
    std::shared_ptr<const BiomeSource> biomes;
};

class OverworldTerrainGenerator {
public:
    // `router` must outlive the generator. The router caches as it samples, so
    // use one router/generator pair per thread.
    explicit OverworldTerrainGenerator(const OverworldNoiseRouter& router, TerrainOptions options = {});
    ~OverworldTerrainGenerator();

    OverworldTerrainGenerator(OverworldTerrainGenerator&&) noexcept;
    OverworldTerrainGenerator& operator=(OverworldTerrainGenerator&&) noexcept;

    // Generating a chunk never depends on which chunks were generated before.
    [[nodiscard]] TerrainChunk generate(int chunkX, int chunkZ);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcworld
