// Materials: the `material_context` .. `underground_rule` nodes of `7B` in
// `minecraft-26.3-worldgen.dot`.
//
// Java: `MaterialSystem.buildSurface` with the rule tree from
// `OverworldMaterialRules`. The density fill leaves a chunk of stone, water and
// air; this pass replaces blocks near the surface, inside ore veins and along
// the bedrock floor with the material the rules ask for.
//
// The rules form a strict priority list, and the first match for a block wins:
//
//   1. bedrock floor
//   2. copper ore veins
//   3. iron ore veins
//   4. above the preliminary surface: the biome's surface materials,
//      plus the badlands and frozen-ocean extensions
//   5. underground: sulfur bands and deepslate
//
// Conditions are evaluated against a per-column context (`Column`) and a
// per-block one (`Sample`) that mirror Java's `MaterialRuleContext`. Inclusive
// bounds, `float` versus `double` thresholds and the exact order of the chain
// are all observable in the output; see `WORLDGEN_AUDIT.md`.

#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcworld::detail {
namespace {

using enum Block;
using Opt = std::optional<Block>;

// --- Shared constants ------------------------------------------------------

// "This column has no water above the current block". Java uses a sentinel far
// below the world rather than an optional, and comparisons rely on it.
constexpr int kNoWater = -1000000;

constexpr int kClayBandCount = 192;

// Vertical extent of the bedrock roughness and of the deepslate transition.
constexpr int kBedrockFloorTop = -59;
constexpr int kDeepslateBottom = 0;
constexpr int kDeepslateTop = 8;

// Ore-vein height ranges: copper above, iron below.
constexpr int kCopperMinY = 0;
constexpr int kCopperMaxY = 50;
constexpr int kIronMinY = -60;
constexpr int kIronMaxY = -8;

// Java: `NoiseThresholdCondition` divides its threshold by this before
// comparing against the surface noise.
constexpr float kSurfaceNoiseScale = 8.25F;

// --- Scalar helpers --------------------------------------------------------

[[nodiscard]] float remapClamped(float value, float fromMin, float fromMax, float toMin, float toMax) {
    // Java: `DensityFunctions.clampedMap` builds a clamp -> multiply -> add
    // graph, so the offset is folded in rather than applied to the result.
    const float factor = (toMax - toMin) / (fromMax - fromMin);
    const float offset = toMin - fromMin * factor;
    return std::clamp(value, fromMin, fromMax) * factor + offset;
}

struct NoiseCorner {
    int x;
    int y;
    int z;
    bool operator==(const NoiseCorner&) const = default;
};

struct NoiseCornerHash {
    [[nodiscard]] std::size_t operator()(NoiseCorner corner) const noexcept {
        std::size_t result = 0;
        const auto combine = [&](int coordinate) {
            result ^= std::hash<int>{}(coordinate) + 0x9e3779b9U + (result << 6) + (result >> 2);
        };
        combine(corner.x);
        combine(corner.y);
        combine(corner.z);
        return result;
    }
};

// Trilinear interpolation over the density graph's own lattice. Corner values
// are immutable, so memoizing them avoids resampling the same noise per block.
class InterpolatedNoise {
public:
    InterpolatedNoise(const NormalNoise& noise, double scale, float outside)
        : noise_(noise), scale_(scale), outside_(outside) {}

    [[nodiscard]] float sample(int x, int y, int z) const {
        constexpr int kCellWidth = 4;
        constexpr int kCellHeight = 8;
        constexpr int kLowestCellY = -64;
        constexpr int kHighestCellY = 56;

        const int cellX = floorDiv(x, kCellWidth) * kCellWidth;
        const int cellY = floorDiv(y, kCellHeight) * kCellHeight;
        const int cellZ = floorDiv(z, kCellWidth) * kCellWidth;
        auto corner = [&](int dx, int dy, int dz) {
            const int sampleY = cellY + dy * kCellHeight;
            if (sampleY < kLowestCellY || sampleY > kHighestCellY) {
                return outside_;
            }
            const NoiseCorner key{cellX + dx * kCellWidth, sampleY, cellZ + dz * kCellWidth};
            const auto found = corners_.find(key);
            if (found != corners_.end()) return found->second;
            const float value = noise_.sample(key.x * scale_, key.y * scale_, key.z * scale_);
            corners_.emplace(key, value);
            return value;
        };
        auto lerp = [](float amount, float from, float to) { return from + amount * (to - from); };

        const float tx = (x - cellX) / static_cast<float>(kCellWidth);
        const float ty = (y - cellY) / static_cast<float>(kCellHeight);
        const float tz = (z - cellZ) / static_cast<float>(kCellWidth);

        // Match the density lattice's X, then Y, then Z evaluation order.
        const float c000 = corner(0, 0, 0);
        const float c100 = corner(1, 0, 0);
        const float c010 = corner(0, 1, 0);
        const float c110 = corner(1, 1, 0);
        const float c001 = corner(0, 0, 1);
        const float c101 = corner(1, 0, 1);
        const float c011 = corner(0, 1, 1);
        const float c111 = corner(1, 1, 1);
        const float x00 = lerp(tx, c000, c100);
        const float x10 = lerp(tx, c010, c110);
        const float x01 = lerp(tx, c001, c101);
        const float x11 = lerp(tx, c011, c111);
        return lerp(tz, lerp(ty, x00, x10), lerp(ty, x01, x11));
    }

private:
    const NormalNoise& noise_;
    double scale_;
    float outside_;
    mutable std::unordered_map<NoiseCorner, float, NoiseCornerHash> corners_;
};

// --- Noise sets ------------------------------------------------------------
//
// Each noise is identified by its resource key; the key, first octave and
// amplitudes together define the noise, so none of them can be renamed or
// reordered without regenerating the world.

struct SurfaceNoises {
    explicit SurfaceNoises(std::int64_t seed)
        : main(seed, "minecraft:surface", -6, {1, 1, 1}),
          secondary(seed, "minecraft:surface_secondary", -6, {1, 1, 0, 1}),
          swamp(seed, "minecraft:surface_swamp", -2, {1}),
          patch(seed, "minecraft:small_patch", -3, {3}) {}

    NormalNoise main;      // Surface depth and the banded terracotta windows.
    NormalNoise secondary; // Depth of the sandstone below sand.
    NormalNoise swamp;     // Where swamp water pools above sea level.
    NormalNoise patch;     // Coarse-dirt patches in dappled forest.
};

struct OreNoises {
    explicit OreNoises(std::int64_t seed)
        : veininess(seed, "minecraft:ore_veininess", -8, {1}),
          veinA(seed, "minecraft:ore_vein_a", -7, {1}),
          veinB(seed, "minecraft:ore_vein_b", -7, {1}),
          gap(seed, "minecraft:ore_gap", -5, {1}) {}

    NormalNoise veininess; // Sign picks copper vs. iron; magnitude the richness.
    NormalNoise veinA;     // The two vein noises together carve the vein shape.
    NormalNoise veinB;
    NormalNoise gap; // Punches holes in otherwise solid ore.
};

struct BiomeSurfaceNoises {
    explicit BiomeSurfaceNoises(std::int64_t seed)
        : calcite(seed, "minecraft:calcite", -9, {1, 1, 1, 1}),
          gravel(seed, "minecraft:gravel", -8, {1, 1, 1, 1}),
          powderSnow(seed, "minecraft:powder_snow", -6, {1, 1, 1, 1}),
          packedIce(seed, "minecraft:packed_ice", -7, {1, 1, 1, 1}),
          ice(seed, "minecraft:ice", -4, {1, 1, 1, 1}),
          sulfur(seed, "minecraft:sulfur_cave_gradient", -5, {1, 0, 1}) {}

    NormalNoise calcite;
    NormalNoise gravel;
    NormalNoise powderSnow;
    NormalNoise packedIce;
    NormalNoise ice;
    NormalNoise sulfur; // Also used underground, outside the surface rules.
};

// The badlands pillars and the frozen-ocean icebergs are the same shape: a
// surface noise gates the feature, a pillar noise gives its radius and a roof
// noise caps its height.
struct PillarNoises {
    NormalNoise surface;
    NormalNoise pillar;
    NormalNoise roof;
};

// --- Clay bands ------------------------------------------------------------

// Java: `MaterialRules.ClayBandsProvider`. The terracotta colour for each of
// 192 Y levels, generated once per world from its own seeded stream.
[[nodiscard]] std::array<Block, kClayBandCount> makeClayBands(std::int64_t worldSeed) {
    std::array<Block, kClayBandCount> bands{};
    bands.fill(Terracotta);
    LegacyRandom random(deriveSeed(worldSeed, "minecraft:clay_bands"));

    // Scattered single orange bands. Java advances the index twice per
    // iteration (once by the loop, once by the draw), which is what spaces
    // them out; keep the shape of this loop.
    for (int i = 0; i < kClayBandCount; ++i) {
        i += random.nextInt(5) + 1;
        if (i < kClayBandCount) {
            bands[i] = OrangeTerracotta;
        }
    }

    auto addColoredBands = [&](int minWidth, Block block) {
        const int count = 6 + random.nextInt(10);
        for (int band = 0; band < count; ++band) {
            const int width = minWidth + random.nextInt(3);
            const int start = random.nextInt(kClayBandCount);
            for (int offset = 0; offset < width && start + offset < kClayBandCount; ++offset) {
                bands[start + offset] = block;
            }
        }
    };
    addColoredBands(1, YellowTerracotta);
    addColoredBands(2, BrownTerracotta);
    addColoredBands(1, RedTerracotta);

    // White bands, optionally fringed with light grey, walking upwards.
    const int whiteCount = 9 + random.nextInt(7);
    int level = 0;
    for (int band = 0; band < whiteCount && level < kClayBandCount; ++band) {
        bands[level] = WhiteTerracotta;
        if (level - 1 > 0 && random.bits(1)) {
            bands[level - 1] = LightGrayTerracotta;
        }
        if (level + 1 < kClayBandCount && random.bits(1)) {
            bands[level + 1] = LightGrayTerracotta;
        }
        level += random.nextInt(16) + 4;
    }
    return bands;
}

// --- Rule context ----------------------------------------------------------

// Java: the per-column part of `MaterialRuleContext`.
struct Column {
    int localX;
    int localZ;
    int x; // World coordinates of the column.
    int z;
    float surfaceNoise;
    float secondaryNoise;
    int depth;      // Thickness of the biome's topsoil, in blocks.
    int minSurface; // Lowest Y the surface rules apply to.
    int bandShift;  // Vertical offset into the clay bands.
    bool steep;     // Java: `SteepCondition`, deliberately directional.
    bool banded;    // Surface noise inside one of the banded-terracotta windows.
};

// Java: the per-block part of `MaterialRuleContext`, for a solid block found
// while scanning a column downwards.
struct Sample {
    int y;
    Biome biome;
    int solidAbove; // Solid blocks between this one and the open air above it.

    // Java's `stoneDepthAbove <= 1` / `stoneDepthBelow <= 1`.
    bool isTop = false;
    bool isCeiling = false;
    // Not submerged (Java's `NOT_UNDERWATER`, whose offset is -1).
    bool isDry = false;
    // Close enough to the water surface for the surface rules to apply.
    bool isShallow = false;
    // Within the biome's topsoil thickness.
    bool isUnderTopsoil = false;
    // Java: `hole`, a column whose surface depth collapsed to nothing.
    bool isHole = false;
};

// Which of the surface conditions hold for the block being resolved. Passing
// these as a struct keeps the four booleans from being swapped at a call site.
struct SurfacePlacement {
    bool isTop;     // Topmost solid block of its run.
    bool isCeiling; // Bottom-most solid block of its run.
    bool isDry;     // Not submerged.
    bool isSteep;   // Java: `SteepCondition`.
};

class Materials {
public:
    Materials(const OverworldNoiseRouter& router, BlockBiomeGetter biomes)
        : router_(router),
          biomes_(std::move(biomes)),
          surface_(router.seed()),
          ore_(router.seed()),
          veininess_(ore_.veininess, 1.5, 0),
          veinA_(ore_.veinA, 4, 1),
          veinB_(ore_.veinB, 4, 1),
          biomeSurface_(router.seed()),
          badlands_{{router.seed(), "minecraft:badlands_surface", -6, {1, 1, 1}},
                    {router.seed(), "minecraft:badlands_pillar", -2, {1, 1, 1, 1}},
                    {router.seed(), "minecraft:badlands_pillar_roof", -8, {1}}},
          iceberg_{{router.seed(), "minecraft:iceberg_surface", -6, {1, 1, 1}},
                   {router.seed(), "minecraft:iceberg_pillar", -6, {1, 1, 1, 1}},
                   {router.seed(), "minecraft:iceberg_pillar_roof", -3, {1}}},
          bandOffset_(router.seed(), "minecraft:clay_bands_offset", -8, {1}),
          clayBands_(makeClayBands(router.seed())) {}

    // Runs the rules over every column of the chunk, in place.
    void build(TerrainChunk& chunk, bool veins) {
        for (int x = 0; x < TerrainChunk::width; ++x) {
            for (int z = 0; z < TerrainChunk::width; ++z) {
                buildColumn(chunk, x, z, veins);
            }
        }
    }

    // Java: `MaterialRuleContext.topMaterial`. Evaluates the rules for a single
    // block in isolation, for repairing soil a carver exposed.
    Opt topMaterial(const TerrainChunk& chunk, int x, int y, int z, bool underFluid, bool veins) {
        const int worldX = chunk.chunkX * TerrainChunk::width + x;
        const int worldZ = chunk.chunkZ * TerrainChunk::width + z;

        if (isBedrockFloor(worldX, y, worldZ)) {
            return Bedrock;
        }
        if (veins) {
            if (const Opt vein = oreVein(worldX, y, worldZ)) {
                return vein;
            }
        }

        const Biome biome = biomeAt(chunk, x, y, z);
        const float surfaceNoise = surface_.main.sample(worldX, 0, worldZ);
        const int depth = surfaceDepth(worldX, worldZ, surfaceNoise);
        const int minSurface = minSurfaceY(worldX, worldZ, depth);

        // A single-block column: both stone depths are one, so the block is
        // both top and ceiling. With fluid the water height would be y + 1,
        // which `NOT_UNDERWATER`'s -1 offset still counts as dry.
        (void)underFluid;

        if (y >= minSurface) {
            const bool banded = isBanded(surfaceNoise);
            if (biome == Biome::WoodedBadlands && y >= 97 + 2 * depth) {
                return banded ? CoarseDirt : Grass;
            }
            if (y < kSeaLevel && isSwampWater(biome, worldX, y, worldZ)) {
                return Water;
            }
            if (isBadlands(biome)) {
                if (y >= 256) {
                    return OrangeTerracotta;
                }
                if (y + 1 >= 74 + depth) {
                    return banded ? Terracotta : clayBandAt(y, clayBandShift(worldX, worldZ));
                }
                return RedSandstone;
            }
            if (isFrozenOcean(biome) && depth <= 0) {
                return Air;
            }
            const SurfacePlacement placement{
                .isTop = true, .isCeiling = true, .isDry = true, .isSteep = isSteep(chunk, x, z)};
            return biomeSurfaceMaterial(biome, worldX, y, worldZ, placement, surfaceNoise);
        }

        if (biome == Biome::SulfurCaves) {
            if (const Opt band = sulfurBand(worldX, y, worldZ)) {
                return band;
            }
        }
        if (isDeepslate(worldX, y, worldZ)) {
            return Deepslate;
        }
        return {};
    }

private:
    // --- Column context ----------------------------------------------------

    Biome biomeAt(const TerrainChunk& chunk, int x, int y, int z) const {
        if (biomes_) {
            return biomes_(chunk.chunkX * TerrainChunk::width + x, y, chunk.chunkZ * TerrainChunk::width + z);
        }
        return chunk.biomeAt(x, std::clamp(y, TerrainChunk::minY, TerrainChunk::maxY - 1), z);
    }

    // Thickness of the biome's topsoil for this column, jittered per column.
    [[nodiscard]] int surfaceDepth(int worldX, int worldZ, float surfaceNoise) const {
        LegacyRandom random(positionalSeed(router_.seed(), "minecraft:surface", worldX, 0, worldZ));
        return static_cast<int>(surfaceNoise * 2.75 + 3 + random.nextDouble() * .25);
    }

    // Lowest Y at which the surface rules still apply. Java compares against
    // the preliminary surface level, offset by the column's own depth.
    [[nodiscard]] int minSurfaceY(int worldX, int worldZ, int depth) const {
        return static_cast<int>(std::floor(router_.sample(worldX, 0, worldZ).chunkSurfaceLevel)) + depth - 8;
    }

    [[nodiscard]] int clayBandShift(int worldX, int worldZ) const {
        return static_cast<int>(std::floor(bandOffset_.sample(worldX, 0, worldZ) * 4.0F + .5F));
    }

    [[nodiscard]] Block clayBandAt(int y, int bandShift) const {
        // The offset keeps the index positive for the whole world height.
        return clayBands_[static_cast<std::size_t>((y + bandShift + 384) % kClayBandCount)];
    }

    // Java's three inclusive surface-noise windows that select plain terracotta
    // over a coloured band.
    [[nodiscard]] static bool isBanded(float surfaceNoise) {
        return (surfaceNoise >= -.909 && surfaceNoise <= -.5454) ||
               (surfaceNoise >= -.1818 && surfaceNoise <= .1818) ||
               (surfaceNoise >= .5454 && surfaceNoise <= .909);
    }

    // Java: `SteepCondition` looks at the world-surface gradient, and only in
    // one direction per axis; a slope the other way is not steep.
    [[nodiscard]] static bool isSteep(const TerrainChunk& chunk, int x, int z) {
        constexpr int kLast = TerrainChunk::width - 1;
        const auto height = [&](int localX, int localZ) { return chunk.worldSurface[localZ * TerrainChunk::width + localX]; };
        const int gradientX = height(std::min(x + 1, kLast), z) - height(std::max(x - 1, 0), z);
        const int gradientZ = height(x, std::min(z + 1, kLast)) - height(x, std::max(z - 1, 0));
        return gradientX <= -4 || gradientZ >= 4;
    }

    [[nodiscard]] Column makeColumn(const TerrainChunk& chunk, int x, int z) const {
        Column column{};
        column.localX = x;
        column.localZ = z;
        column.x = chunk.chunkX * TerrainChunk::width + x;
        column.z = chunk.chunkZ * TerrainChunk::width + z;
        column.surfaceNoise = surface_.main.sample(column.x, 0, column.z);
        column.secondaryNoise = surface_.secondary.sample(column.x, 0, column.z);
        column.depth = surfaceDepth(column.x, column.z, column.surfaceNoise);
        column.minSurface = minSurfaceY(column.x, column.z, column.depth);
        column.bandShift = clayBandShift(column.x, column.z);
        column.steep = isSteep(chunk, x, z);
        column.banded = isBanded(column.surfaceNoise);
        return column;
    }

    // --- Priority 1 and 2: bedrock and ore veins ---------------------------

    // Java: a vertical gradient of random hits, solid below `low` and absent
    // above `high`.
    [[nodiscard]] bool gradient(std::string_view key, int x, int y, int z, int low, int high) const {
        if (y <= low) {
            return true;
        }
        if (y >= high) {
            return false;
        }
        LegacyRandom random(positionalSeed(router_.seed(), key, x, y, z));
        return random.nextFloat() < 1.0 - (y - low) / static_cast<double>(high - low);
    }

    [[nodiscard]] bool isBedrockFloor(int x, int y, int z) const {
        return gradient("minecraft:bedrock_floor", x, y, z, TerrainChunk::minY, kBedrockFloorTop);
    }

    [[nodiscard]] bool isDeepslate(int x, int y, int z) const {
        return gradient("minecraft:deepslate", x, y, z, kDeepslateBottom, kDeepslateTop);
    }

    // Java: `OreVeinRule`. Copper veins sit above Y 0, iron veins below -8; the
    // veininess noise decides which of the two a position could belong to, how
    // rich it is, and the vein noises carve its actual shape.
    [[nodiscard]] Opt oreVein(int x, int y, int z) const {
        const bool inAnyRange = (y >= kCopperMinY && y < kCopperMaxY) || (y >= kIronMinY && y < kIronMaxY);
        if (!inAnyRange) {
            return {};
        }

        const float veininess = veininess_.sample(x, y, z);
        const bool copper = veininess > 0;
        const int minY = copper ? kCopperMinY : kIronMinY;
        const int maxY = copper ? kCopperMaxY : kIronMaxY;
        if (y < minY || y >= maxY) {
            return {};
        }

        // Veins fade out towards the top and bottom of their range.
        const float edge = remapClamped(static_cast<float>(std::min(y - minY, maxY - y)), 0, 20, -.2F, 0);
        if (std::abs(veininess) - .4F + edge < 0) {
            return {};
        }
        const float veinShape = std::max(std::abs(veinA_.sample(x, y, z)),
                                         std::abs(veinB_.sample(x, y, z)));
        if (veinShape > .08F) {
            return {};
        }

        LegacyRandom random(positionalSeed(router_.seed(), "minecraft:ore", x, y, z));
        if (random.nextFloat() > .7F) {
            return {};
        }
        const float richness = remapClamped(std::abs(veininess), .4F, .6F, .1F, .3F);
        if (random.nextFloat() < richness && ore_.gap.sample(x, y, z) > -.3F) {
            if (random.nextFloat() < .02F) {
                return copper ? RawCopper : RawIron;
            }
            return copper ? CopperOre : DeepslateIronOre;
        }
        // Inside the vein, but not ore: the filler stone of the vein.
        return copper ? Granite : Tuff;
    }

    // --- Priority 4: biome surface materials -------------------------------

    [[nodiscard]] static bool isBadlands(Biome biome) {
        return biome == Biome::Badlands || biome == Biome::WoodedBadlands || biome == Biome::ErodedBadlands;
    }

    [[nodiscard]] static bool isFrozenOcean(Biome biome) {
        return biome == Biome::FrozenOcean || biome == Biome::DeepFrozenOcean;
    }

    [[nodiscard]] bool isSwampWater(Biome biome, int x, int y, int z) const {
        const bool inRange = (biome == Biome::Swamp && y >= 62) || (biome == Biome::MangroveSwamp && y >= 60);
        return inRange && surface_.swamp.sample(x, 0, z) >= 0;
    }

    [[nodiscard]] Opt sulfurBand(int x, int y, int z) const {
        // First match wins, including the shared 0.4 boundary.
        const float noise = biomeSurface_.sulfur.sample(x, y, z);
        if (noise >= -.4F && noise <= -.1F) {
            return Cinnabar;
        }
        if (noise >= 0 && noise <= .4F) {
            return Sulfur;
        }
        if (noise >= .4F) {
            return Cinnabar;
        }
        return {};
    }

    [[nodiscard]] static SurfacePlacement placement(const Sample& sample, const Column& column, bool isTop) {
        return {.isTop = isTop, .isCeiling = sample.isCeiling, .isDry = sample.isDry, .isSteep = column.steep};
    }

    // The biome-specific part of the surface rules: what a given biome puts on
    // top of (and just below) its terrain.
    [[nodiscard]] Opt biomeSurfaceMaterial(
        Biome biome,
        int x,
        int y,
        int z,
        SurfacePlacement placement,
        float surfaceNoise
    ) const {
        const bool isTop = placement.isTop;
        const bool isCeiling = placement.isCeiling;
        const bool isDry = placement.isDry;
        const bool isSteepSlope = placement.isSteep;

        auto within = [&](const NormalNoise& noise, double low, double high) {
            const float value = noise.sample(x, 0, z);
            return value >= low && value <= high;
        };
        auto noiseAbove = [&](double threshold) { return surfaceNoise >= threshold / kSurfaceNoiseScale; };

        const Block sand = isCeiling ? Sandstone : Sand;
        const Block grit = isCeiling ? Stone : Gravel;

        if (biome == Biome::FrozenPeaks) {
            if (isSteepSlope || within(biomeSurface_.packedIce, isTop ? 0 : -.5, .2)) {
                return PackedIce;
            }
            if (within(biomeSurface_.ice, isTop ? 0 : -.0625, .025)) {
                return Ice;
            }
            if (isDry) {
                return Snow;
            }
        }
        if (biome == Biome::SnowySlopes || biome == Biome::Grove) {
            if (biome == Biome::SnowySlopes && isSteepSlope) {
                return Stone;
            }
            if (isDry && within(biomeSurface_.powderSnow, isTop ? .35 : .45, isTop ? .6 : .58)) {
                return PowderSnow;
            }
            if (biome == Biome::Grove && !isTop) {
                return Dirt;
            }
            if (isDry) {
                return Snow;
            }
        }
        if (biome == Biome::JaggedPeaks) {
            if (!isTop || isSteepSlope) {
                return Stone;
            }
            if (isDry) {
                return Snow;
            }
        }
        if (biome == Biome::StonyPeaks) {
            return within(biomeSurface_.calcite, -.0125, .0125) ? Calcite : Stone;
        }
        if (biome == Biome::StonyShore) {
            return within(biomeSurface_.gravel, -.05, .05) ? grit : Stone;
        }
        if (biome == Biome::WindsweptHills && noiseAbove(1)) {
            return Stone;
        }
        if (biome == Biome::WarmOcean || biome == Biome::Beach || biome == Biome::SnowyBeach || biome == Biome::Desert) {
            return sand;
        }
        if (biome == Biome::DripstoneCaves) {
            return Stone;
        }
        if (biome == Biome::SulfurCaves) {
            return sulfurBand(x, y, z).value_or(Stone);
        }
        if (biome == Biome::MangroveSwamp) {
            return Mud;
        }
        if (biome == Biome::WindsweptSavanna) {
            if (noiseAbove(1.75)) {
                return Stone;
            }
            if (isTop && noiseAbove(-.5)) {
                return CoarseDirt;
            }
        }
        if (biome == Biome::WindsweptGravellyHills) {
            if (noiseAbove(2)) {
                return grit;
            }
            if (noiseAbove(1)) {
                return Stone;
            }
            if (!noiseAbove(-1)) {
                return grit;
            }
        }

        if (isTop) {
            if (biome == Biome::OldGrowthPineTaiga || biome == Biome::OldGrowthSpruceTaiga) {
                if (noiseAbove(1.75)) {
                    return CoarseDirt;
                }
                if (noiseAbove(-.95)) {
                    return Podzol;
                }
            }
            if (biome == Biome::IceSpikes && isDry) {
                return Snow;
            }
            if (biome == Biome::MushroomFields) {
                return Mycelium;
            }
            if (biome == Biome::DappledForest && surface_.patch.sample(x, 0, z) >= 1.2F) {
                return CoarseDirt;
            }
            return isDry ? Grass : Dirt;
        }
        return Dirt;
    }

    // The badlands extension: terracotta bands above, red sand and sandstone
    // below, with white terracotta where the column is shallow.
    [[nodiscard]] Opt badlandsMaterial(const Column& column, const Sample& sample) const {
        const Block grit = sample.isCeiling ? Stone : Gravel;
        const Block clay = clayBandAt(sample.y, column.bandShift);
        const bool aboveBands = sample.y + sample.solidAbove >= 74 + column.depth;

        if (sample.isTop) {
            if (sample.y >= 256) {
                return OrangeTerracotta;
            }
            if (aboveBands) {
                return column.banded ? Terracotta : clay;
            }
            if (sample.isDry) {
                return sample.isCeiling ? RedSandstone : RedSand;
            }
            if (!sample.isHole) {
                return OrangeTerracotta;
            }
            if (sample.isShallow) {
                return WhiteTerracotta;
            }
            return grit;
        }
        if (sample.y + sample.solidAbove >= kSeaLevel - column.depth) {
            return sample.y >= kSeaLevel && !aboveBands ? OrangeTerracotta : clay;
        }
        if (sample.isUnderTopsoil && sample.isShallow) {
            return WhiteTerracotta;
        }
        return {};
    }

    // --- The rule chain ----------------------------------------------------

    // Java: the whole Overworld rule sequence for one block. The first rule
    // that matches wins, so the order of these branches is the priority list
    // from the file header.
    [[nodiscard]] Opt material(const Column& column, const Sample& sample, bool veins) const {
        const int y = sample.y;
        const Biome biome = sample.biome;

        if (isBedrockFloor(column.x, y, column.z)) {
            return Bedrock;
        }
        if (veins) {
            if (const Opt vein = oreVein(column.x, y, column.z)) {
                return vein;
            }
        }

        if (y >= column.minSurface) {
            if (const Opt surface = surfaceMaterial(column, sample)) {
                return surface;
            }
        }

        if (biome == Biome::SulfurCaves) {
            if (const Opt band = sulfurBand(column.x, y, column.z)) {
                return band;
            }
        }
        if (isDeepslate(column.x, y, column.z)) {
            return Deepslate;
        }
        return {};
    }

    // Priority 4, for a block at or above the preliminary surface.
    [[nodiscard]] Opt surfaceMaterial(const Column& column, const Sample& sample) const {
        const int y = sample.y;
        const Biome biome = sample.biome;
        const Block grit = sample.isCeiling ? Stone : Gravel;

        // Wooded badlands grow grass on their high plateaus.
        if (sample.isTop && biome == Biome::WoodedBadlands && y >= 97 + 2 * column.depth) {
            return column.banded ? CoarseDirt : (sample.isDry ? Grass : Dirt);
        }
        // Swamps pool water just above sea level.
        if (sample.isTop && y < kSeaLevel && isSwampWater(biome, column.x, y, column.z)) {
            return Water;
        }
        if (isBadlands(biome)) {
            if (const Opt badlands = badlandsMaterial(column, sample)) {
                return badlands;
            }
        }

        const bool frozenOcean = isFrozenOcean(biome);
        if (sample.isTop && sample.isDry) {
            // A frozen ocean with no topsoil left is open air: the iceberg pass
            // below fills it in.
            if (frozenOcean && sample.isHole) {
                return Air;
            }
            return biomeSurfaceMaterial(biome, column.x, y, column.z, placement(sample, column, true), column.surfaceNoise);
        }

        if (sample.isShallow) {
            if (sample.isTop && frozenOcean && sample.isHole) {
                return Water;
            }
            if (sample.isUnderTopsoil) {
                return biomeSurfaceMaterial(biome, column.x, y, column.z, placement(sample, column, false), column.surfaceNoise);
            }
            // Deeper sand turns to sandstone; deserts keep it up much further.
            const int range = biome == Biome::Desert ? 30 : 6;
            const int extra = static_cast<int>((static_cast<double>(column.secondaryNoise) + 1) / 2 * range);
            const bool sandy = biome == Biome::Desert || biome == Biome::WarmOcean || biome == Biome::Beach ||
                               biome == Biome::SnowyBeach;
            if (sandy && sample.solidAbove <= 1 + column.depth + extra) {
                return Sandstone;
            }
        }

        // Submerged surfaces the rules above did not claim: the sea floor.
        if (sample.isTop) {
            if (biome == Biome::FrozenPeaks || biome == Biome::JaggedPeaks) {
                return Stone;
            }
            if (biome == Biome::WarmOcean || biome == Biome::LukewarmOcean || biome == Biome::DeepLukewarmOcean) {
                return sample.isCeiling ? Sandstone : Sand;
            }
            return grit;
        }
        return {};
    }

    // --- Column pass -------------------------------------------------------

    void buildColumn(TerrainChunk& chunk, int x, int z, bool veins) {
        // Height of the column before this pass touches it. Both terrain
        // extensions below start from it, so it is read once, up front.
        const int surfaceStart = chunk.worldSurface[z * TerrainChunk::width + x];
        const Biome surfaceBiome = biomeAt(chunk, x, surfaceStart, z);

        // The eroded-badlands pillars run first: they add terrain the material
        // rules below then have to see.
        if (surfaceBiome == Biome::ErodedBadlands) {
            addBadlandsPillar(chunk, x, z, surfaceStart);
        }

        // `makeColumn` reads the live world-surface heightmap, so it has to run
        // after the pillar above and before the rules below.
        const Column column = makeColumn(chunk, x, z);
        applyRules(chunk, column, veins);

        if (isFrozenOcean(surfaceBiome)) {
            addIceberg(chunk, column, surfaceBiome, surfaceStart);
        }
    }

    // Scans the column top-down, tracking the state the rules need, and writes
    // whatever the rule chain returns.
    void applyRules(TerrainChunk& chunk, const Column& column, bool veins) {
        const int x = column.localX;
        const int z = column.localZ;

        int solidAbove = 0;
        int waterHeight = kNoWater;
        int runBottom = TerrainChunk::maxY;
        for (int y = TerrainChunk::maxY - 1; y >= TerrainChunk::minY; --y) {
            const Block existing = chunk.at(x, y, z);
            if (existing == Air) {
                solidAbove = 0;
                waterHeight = kNoWater;
                continue;
            }
            if (isFluid(existing)) {
                if (waterHeight == kNoWater) {
                    waterHeight = y + 1;
                }
                continue;
            }

            // Find the bottom of the solid run this block belongs to, so the
            // rules can tell a thin ceiling from deep stone.
            if (runBottom >= y) {
                runBottom = y;
                while (runBottom > TerrainChunk::minY && isSolid(chunk.at(x, runBottom - 1, z))) {
                    --runBottom;
                }
            }
            ++solidAbove;

            Sample sample{};
            sample.y = y;
            sample.biome = biomeAt(chunk, x, y, z);
            sample.solidAbove = solidAbove;
            sample.isTop = solidAbove <= 1;
            sample.isCeiling = y - runBottom + 1 <= 1;
            sample.isDry = waterHeight == kNoWater || y >= waterHeight - 1;
            sample.isShallow = waterHeight == kNoWater || y + solidAbove >= waterHeight - 6 - column.depth;
            sample.isUnderTopsoil = solidAbove <= 1 + column.depth;
            sample.isHole = column.depth <= 0;

            if (const Opt result = material(column, sample, veins)) {
                setWorldgenBlock(chunk, x, y, z, *result);
                if (isFluid(*result)) {
                    chunk.fluidPostProcessing.push_back({x, y, z});
                }
            }
        }
    }

    // --- Terrain extensions ------------------------------------------------

    // Java: the eroded-badlands pillar rule. Raises stone spires above the
    // existing terrain, but only over columns that are not already flooded.
    void addBadlandsPillar(TerrainChunk& chunk, int x, int z, int surfaceStart) {
        const int worldX = chunk.chunkX * TerrainChunk::width + x;
        const int worldZ = chunk.chunkZ * TerrainChunk::width + z;

        const double radius = std::min(std::abs(badlands_.surface.sample(worldX, 0, worldZ) * 8.25),
                                       badlands_.pillar.sample(worldX * .2, 0, worldZ * .2) * 15.0);
        if (radius <= 0) {
            return;
        }
        const double roof = std::abs(badlands_.roof.sample(worldX * .75, 0, worldZ * .75) * 1.5);
        const int top = std::min(TerrainChunk::maxY - 1,
                                 static_cast<int>(std::floor(64 + std::min(radius * radius * 2.5, std::ceil(roof * 50) + 24))));
        if (surfaceStart > top) {
            return;
        }

        // Stop at the first stone (already solid here) or water (flooded).
        for (int y = top; y >= TerrainChunk::minY; --y) {
            const Block block = chunk.at(x, y, z);
            if (block == Stone) {
                break;
            }
            if (block == Water) {
                return;
            }
        }
        for (int y = top; y >= TerrainChunk::minY && chunk.at(x, y, z) == Air; --y) {
            setWorldgenBlock(chunk, x, y, z, Stone);
        }
    }

    // Java: the frozen-ocean iceberg rule. Packed ice below, a few blocks of
    // snow on top, thinning out where the temperature modifier says it melts.
    void addIceberg(TerrainChunk& chunk, const Column& column, Biome biome, int surfaceStart) {
        const int x = column.localX;
        const int z = column.localZ;

        const double radius = std::min(std::abs(iceberg_.surface.sample(column.x, 0, column.z) * 8.25),
                                       iceberg_.pillar.sample(column.x * 1.28, 0, column.z * 1.28) * 15.0);
        if (radius <= 1.8) {
            return;
        }
        const double roof = std::abs(iceberg_.roof.sample(column.x * 1.17, 0, column.z * 1.17) * 1.5);
        double size = std::min(radius * radius * 1.2, std::ceil(roof * 40) + 14);
        if (meltsFrozenOceanIceberg(biome, column.x, column.z)) {
            size -= 2;
        }
        if (size <= 2) {
            return;
        }

        const double top = kSeaLevel + size;
        const double bottom = kSeaLevel - size - 7;
        LegacyRandom random(positionalSeed(router_.seed(), "minecraft:surface", column.x, 0, column.z));
        const int maxSnow = 2 + random.nextInt(4);
        const int snowFloor = 81 + random.nextInt(10);
        int snowDepth = 0;

        const int start = std::min(TerrainChunk::maxY - 1, std::max(surfaceStart, static_cast<int>(top) + 1));
        for (int y = start; y >= std::max(TerrainChunk::minY, column.minSurface); --y) {
            const Block block = chunk.at(x, y, z);
            // Air inside the iceberg freezes; water just below the surface does
            // too, both with gaps so the shape stays ragged.
            const bool freezesAir = block == Air && y < static_cast<int>(top) && random.nextDouble() > .01;
            const bool freezesWater =
                block == Water && y > static_cast<int>(bottom) && y < kSeaLevel && random.nextDouble() > .15;
            if (freezesAir || freezesWater) {
                const bool snow = snowDepth <= maxSnow && y > snowFloor;
                setWorldgenBlock(chunk, x, y, z, snow ? Snow : PackedIce);
                if (snow) {
                    ++snowDepth;
                }
            }
        }
    }

    const OverworldNoiseRouter& router_;
    BlockBiomeGetter biomes_;
    SurfaceNoises surface_;
    OreNoises ore_;
    InterpolatedNoise veininess_;
    InterpolatedNoise veinA_;
    InterpolatedNoise veinB_;
    BiomeSurfaceNoises biomeSurface_;
    PillarNoises badlands_;
    PillarNoises iceberg_;
    NormalNoise bandOffset_;
    std::array<Block, kClayBandCount> clayBands_;
};

} // namespace

void buildMaterials(TerrainChunk& chunk, const OverworldNoiseRouter& router, bool veins, BlockBiomeGetter biomes) {
    Materials(router, std::move(biomes)).build(chunk, veins);
}

TopMaterialRule makeTopMaterialRule(
    const TerrainChunk& chunk,
    const OverworldNoiseRouter& router,
    bool veins,
    BlockBiomeGetter biomes
) {
    // The rule outlives this call and is copied into a `std::function`, so the
    // (noise-heavy) material state is shared rather than copied per call.
    return [materials = std::make_shared<Materials>(router, std::move(biomes)), &chunk, veins](
               int x, int y, int z, bool underFluid) {
        return materials->topMaterial(chunk, x, y, z, underFluid, veins);
    };
}

} // namespace mcworld::detail
