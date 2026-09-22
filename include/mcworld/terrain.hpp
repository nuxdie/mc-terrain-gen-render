#pragma once

// Public API for step 7B of `minecraft-26.3-worldgen.dot`: generating the
// blocks of an Overworld chunk from the density graph in `worldgen.hpp`.

#include "mcworld/biome.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mcworld {

// The generated-world material palette. This is not the full block-state
// registry: stage 8 plants and structure blocks are represented by their base
// material, without orientation, age, waterlogging or block-entity data.
enum class Block : std::uint8_t {
    Air, Stone, Water, Lava, Bedrock, Deepslate, Grass, Dirt, Sand, Sandstone,
    RedSand, RedSandstone, Gravel, Terracotta, WhiteTerracotta, OrangeTerracotta,
    Podzol, CoarseDirt, Mycelium, Mud, Snow, PowderSnow, Ice, PackedIce, Calcite,
    CopperOre, RawCopper, Granite, DeepslateIronOre, RawIron, Tuff, Sulfur, Cinnabar,
    YellowTerracotta, BrownTerracotta, RedTerracotta, LightGrayTerracotta,
    CoalOre, IronOre, GoldOre, RedstoneOre, DiamondOre, LapisOre, Clay,
    OakLog, OakLeaves, OakPlanks, Cobblestone, MossyCobblestone, Bricks
};

[[nodiscard]] constexpr bool isFluid(Block block) {
    return block == Block::Water || block == Block::Lava;
}

[[nodiscard]] constexpr bool isSolid(Block block) {
    return block != Block::Air && !isFluid(block);
}

[[nodiscard]] constexpr bool isLeaves(Block block) {
    return block == Block::OakLeaves;
}

// The 26.3 `BLOCKS_MOTION_IN_HEIGHTMAP` tag excludes powder snow.
[[nodiscard]] constexpr bool blocksMotion(Block block) {
    return isSolid(block) && block != Block::PowderSnow;
}

// Whether a block counts towards each heightmap, one predicate per entry of
// Java's `Heightmap.Types` that world generation maintains. These are the only
// definition of the four rules: `TerrainChunk::primeHeightmaps` recomputes
// from them and stage-8 feature writes update from them, so the recomputed and
// the incrementally maintained heightmaps cannot drift apart.
[[nodiscard]] constexpr bool countsForWorldSurface(Block block) {
    return block != Block::Air;
}

[[nodiscard]] constexpr bool countsForOceanFloor(Block block) {
    return blocksMotion(block);
}

[[nodiscard]] constexpr bool countsForMotionBlocking(Block block) {
    return blocksMotion(block) || isFluid(block);
}

[[nodiscard]] constexpr bool countsForMotionBlockingNoLeaves(Block block) {
    return (blocksMotion(block) && !isLeaves(block)) || isFluid(block);
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
    // `minY` for a column that has none. Columns are indexed `z * width + x`,
    // the same local X/Z convention as `blocks`.
    using Heightmap = std::array<int, width * width>;
    Heightmap worldSurface{};
    Heightmap oceanFloor{};
    Heightmap motionBlocking{};
    Heightmap motionBlockingNoLeaves{};

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

    // Stage 5 terrain adaptation is chunk-specific. This overload adds its
    // beard density at the same final-density graph point as the router's own
    // injected Beardifier, without changing the terrain-only API above.
    [[nodiscard]] TerrainChunk generate(int chunkX, int chunkZ, const Beardifier& structures);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcworld
