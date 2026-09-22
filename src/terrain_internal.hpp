#pragma once

// Internal contract between the stages of `7B. TERRAIN` in
// `minecraft-26.3-worldgen.dot`, in the order `OverworldTerrainGenerator`
// runs them:
//
//   biome palette (stage 6)  -> src/biome.cpp, src/biome_environment.cpp
//   aquifer + density fill   -> src/aquifer.cpp, src/terrain.cpp
//   materials                -> src/materials.cpp
//   carvers                  -> src/carvers.cpp
//
// Nothing here is part of the public API; `include/mcworld/terrain.hpp` is.
// The passes communicate through `TerrainChunk` and through the aquifer, which
// is shared between the density fill and the carvers so both see the same
// fluid decisions.

#include "legacy_random.hpp"
#include "mcworld/terrain.hpp"
#include "noise.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace mcworld::detail {

// Sea level of the standard Overworld: the first Y that is *not* filled by the
// global fluid. Aquifers and material rules both key off it.
constexpr int kSeaLevel = 63;

// Biome palettes and several noise lattices are stored per 4x4x4 cell.
constexpr int kQuartSize = 4;

// --- Aquifers --------------------------------------------------------------

// What the aquifer decided a block should be, plus whether the position has to
// be revisited by fluid post-processing (Java: `Aquifer.shouldScheduleFluidUpdate`).
struct Substance {
    Block block;
    bool schedule = false;
};

// Java: `Aquifer.NoiseBasedAquifer`. Picks between the generator's default
// block and a local fluid for every position the density graph leaves open.
//
// Sampling mutates internal caches, so an instance belongs to a single chunk on
// a single thread. Results do not depend on the order positions are sampled in;
// `terrain_tests.cpp` pins that down.
class Aquifer {
public:
    Aquifer(const OverworldNoiseRouter& router, bool enabled);

    // Java: `computeSubstance`. `density > 0` is solid terrain; everything else
    // resolves to air or to the fluid of the surrounding aquifer.
    Substance sample(int x, int y, int z, float density);

private:
    // A fluid body: everything below `level` is `type`, everything above is air.
    struct Fluid {
        int level;
        Block type;

        [[nodiscard]] Block at(int y) const { return y < level ? type : Block::Air; }
        bool operator==(const Fluid&) const = default;
    };

    // The aquifer grid is sparse: each cell has one jittered center block whose
    // fluid the whole neighbourhood interpolates between.
    struct Center {
        int x;
        int y;
        int z;
        Fluid fluid;
    };

    // Preliminary surface level of the column containing (x, z), quart-aligned.
    int surface(int x, int z);
    // The fluid body that owns a center block.
    Fluid fluid(int x, int y, int z);
    // The center of one aquifer grid cell, memoized.
    const Center& center(int x, int y, int z);
    // Barrier strength between two fluid bodies; `noise` memoizes the barrier
    // sample within one `sample()` call, which is why it is an in/out parameter.
    double pressure(int x, int y, int z, Fluid a, Fluid b, double& noise);

    const OverworldNoiseRouter& router_;
    bool enabled_;
    NormalNoise barrier_;
    NormalNoise floodedness_;
    NormalNoise spread_;
    NormalNoise lava_;
    std::map<std::tuple<int, int, int>, Center> centers_;
    std::map<std::pair<int, int>, int> surfaces_;
};

// --- Biome queries (stage 6, as seen from the terrain passes) ---------------

// Resolves a biome at *block* resolution, in world coordinates. Material rules
// and carver soil repair ask per block, not per palette cell.
using BlockBiomeGetter = std::function<Biome(int, int, int)>;

// Java: `BiomeManager.obfuscateSeed`. The block-level biome jitter runs off a
// SHA-256 derived seed rather than the world seed itself.
[[nodiscard]] std::uint64_t biomeZoomSeed(std::int64_t seed);

// Java: `BiomeManager.getBiome`. Picks one of the eight surrounding quart cells
// with a deterministic jitter, which is what softens biome borders at block
// resolution. Returns quart coordinates, not block coordinates.
[[nodiscard]] BlockPosition zoomedBiomeQuart(std::uint64_t seed, int x, int y, int z);

// Block-resolution biome lookup backed by `chunk`'s palette, falling back to
// `source` outside it (queries near a chunk border legitimately leave the
// chunk). `clampY` mirrors `ChunkAccess.getNoiseBiome`, which clamps to the
// palette's Y range; carver repair queries do not clamp.
//
// The returned getter memoizes out-of-chunk lookups and is therefore neither
// thread-safe nor copy-cheap; it borrows `chunk`, `router` and `source`, which
// all have to outlive it.
[[nodiscard]] BlockBiomeGetter makeBlockBiomeGetter(
    const TerrainChunk& chunk,
    const OverworldNoiseRouter& router,
    const BiomeSource& source,
    bool clampY
);

// Java: `Biome.TemperatureModifier.FROZEN` above sea level. Warm spots inside a
// frozen ocean melt icebergs down by two blocks.
[[nodiscard]] bool meltsFrozenOceanIceberg(Biome biome, int x, int z);

// --- Materials -------------------------------------------------------------

// Writes a block during worldgen and keeps `TerrainChunk::worldSurface` in step
// with it. Material gradients and carver soil repair read that heightmap while
// the same pass is still writing, so it cannot wait until the pass ends; the
// other heightmaps are primed once per pass.
void setWorldgenBlock(TerrainChunk& chunk, int x, int y, int z, Block block);

// Java: `MaterialSystem.buildSurface`. Runs the Overworld material rules over
// every column of `chunk`, in place. `veins` enables the ore-vein rules.
// `biomes` defaults to the chunk's own palette when empty.
void buildMaterials(TerrainChunk& chunk, const OverworldNoiseRouter& router, bool veins, BlockBiomeGetter biomes = {});

// Java: `MaterialRuleContext.topMaterial`. Evaluates the material rules for a
// single block in isolation, used to repair soil a carver exposed. The `bool`
// argument is whether the block above is a fluid.
using TopMaterialRule = std::function<std::optional<Block>(int, int, int, bool)>;

[[nodiscard]] TopMaterialRule makeTopMaterialRule(
    const TerrainChunk& chunk,
    const OverworldNoiseRouter& router,
    bool veins,
    BlockBiomeGetter biomes = {}
);

// --- Carvers ---------------------------------------------------------------

// One bit per block of a chunk, in `TerrainChunk`'s z, x, y storage order.
using CarvingMask = std::vector<bool>;

[[nodiscard]] constexpr std::size_t carvingMaskIndex(int x, int y, int z) {
    return static_cast<std::size_t>((z * TerrainChunk::width + x) * TerrainChunk::height + y - TerrainChunk::minY);
}

// Java: the cave, extra-underground-cave and canyon carvers of every source
// chunk within radius 8. Geometry only: the mask says which blocks the carvers
// want removed, `applyCarvingMask` decides what actually happens to them.
[[nodiscard]] CarvingMask buildCarvingMask(int chunkX, int chunkZ, std::int64_t seed);

// Java: `WorldCarver.carve` write-back. Walks the mask's vertical runs and asks
// `aquifer` what belongs in each carved position, repairing soil exposed under
// grass or mycelium via `topMaterial`.
void applyCarvingMask(TerrainChunk& chunk, const CarvingMask& mask, Aquifer& aquifer, const TopMaterialRule& topMaterial);

// Builds the mask for `chunk` and applies it.
void carve(TerrainChunk& chunk, const OverworldNoiseRouter& router, const BiomeSource& biomes, Aquifer& aquifer, bool veins);

} // namespace mcworld::detail
