// Stage 8 of `minecraft-26.3-worldgen.dot`: FEATURES, i.e. structure blocks
// and biome decoration.
//
// The stage walks every decoration step in ordinal order and, within a step,
// places structure pieces before biome features. Three things make it different
// from the stage-7B passes and are the reasons this file exists:
//
//  * It writes outside the chunk it is decorating. `DecorationRegion` is the
//    mutable 3x3 that bounds those writes, and it keeps every heightmap live
//    because later features read them.
//  * Its randomness is positional, not sequential: each feature reseeds from
//    the chunk's decoration seed plus its own index and step, so one feature
//    cannot shift another's output.
//  * Which features run depends on the biomes actually present in the centre
//    3x3, not on the biome under each candidate position (that is checked
//    again per position, by the features themselves).

#include "generation_internal.hpp"
#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mcworld::detail {
namespace {

// --- Biome predicates ------------------------------------------------------

[[nodiscard]] bool caveBiome(Biome biome) {
    return biome == Biome::DripstoneCaves || biome == Biome::LushCaves
        || biome == Biome::SulfurCaves || biome == Biome::DeepDark;
}

[[nodiscard]] bool snowyBiome(Biome biome) {
    using enum Biome;
    return biome == SnowyPlains || biome == IceSpikes || biome == SnowyTaiga || biome == Grove
        || biome == SnowySlopes || biome == FrozenPeaks || biome == JaggedPeaks
        || biome == FrozenRiver || biome == SnowyBeach || biome == FrozenOcean
        || biome == DeepFrozenOcean;
}

// Biomes whose feature list includes any oak-like tree. The port places one
// tree shape for all of them; see README for that boundary.
[[nodiscard]] bool treeBiome(Biome biome) {
    using enum Biome;
    switch (biome) {
    case Forest:
    case FlowerForest:
    case BirchForest:
    case DarkForest:
    case PaleGarden:
    case DappledForest:
    case OldGrowthBirchForest:
    case Taiga:
    case SnowyTaiga:
    case OldGrowthPineTaiga:
    case OldGrowthSpruceTaiga:
    case WindsweptForest:
    case Jungle:
    case SparseJungle:
    case BambooJungle:
    case WoodedBadlands:
    case Meadow:
    case CherryGrove:
    case Grove:
    case Swamp:
    case MangroveSwamp:
        return true;
    default:
        return false;
    }
}

// Ores are registered on every Overworld biome.
[[nodiscard]] bool everyBiome(Biome) { return true; }

// The deep dark registers no decoration beyond the ancient city.
[[nodiscard]] bool outsideDeepDark(Biome biome) { return biome != Biome::DeepDark; }

// Clay disks need a seabed, which the cave biomes do not have.
[[nodiscard]] bool surfaceBiome(Biome biome) {
    return !caveBiome(biome) && biome != Biome::DeepDark;
}

// The biomes present in a chunk's decoration neighbourhood, as a flag per
// `Biome`. A feature runs when any of them registers it.
using BiomeSet = std::array<bool, kBiomeCount>;

// Java: the biome palette entries collected from the centre 3x3. Missing
// neighbours are skipped rather than generated: the caller is responsible for
// having populated the region.
[[nodiscard]] BiomeSet collectBiomes(const ChunkMap& chunks, ChunkPosition center) {
    BiomeSet result{};
    for (int z = center.z - 1; z <= center.z + 1; ++z) {
        for (int x = center.x - 1; x <= center.x + 1; ++x) {
            const auto found = chunks.find({x, z});
            if (found == chunks.end()) continue;
            for (Biome biome : found->second.biomes) {
                result[static_cast<std::size_t>(biome)] = true;
            }
        }
    }
    return result;
}

[[nodiscard]] bool anyBiome(const BiomeSet& present, bool (*predicate)(Biome)) {
    for (std::size_t biome = 0; biome < present.size(); ++biome) {
        if (present[biome] && predicate(static_cast<Biome>(biome))) return true;
    }
    return false;
}

// --- The write region ------------------------------------------------------

// Java's `WorldGenRegion` for the FEATURES step: the chunk being decorated plus
// its eight neighbours, all writable.
//
// Reads and writes use world X/Z and world Y, like the rest of stage 8 - only
// `TerrainChunk` itself is chunk-local.
class DecorationRegion {
public:
    DecorationRegion(ChunkMap& chunks, ChunkPosition center, std::int64_t seed)
        : chunks_(&chunks), center_(center), biomeZoomSeed_(biomeZoomSeed(seed)) {}

    // Outside the build height there is nothing rather than an error, which is
    // what lets features probe above and below themselves without clamping.
    // Outside the 3x3, however, the caller is at fault, so `chunkAt` throws.
    [[nodiscard]] Block at(int x, int y, int z) const {
        if (y < TerrainChunk::minY || y >= TerrainChunk::maxY) return Block::Air;
        return chunkAt(x, z).at(localBlock(x), y, localBlock(z));
    }

    // Block-resolution biome lookup, jittered like `BiomeManager.getBiome`.
    // The jitter can select a quart cell in a neighbouring chunk, so this
    // resolves the owning chunk from the *cell*, not from the query position.
    [[nodiscard]] Biome biomeAt(int x, int y, int z) const {
        const BlockPosition cell = zoomedBiomeQuart(biomeZoomSeed_, x, y, z);
        const int cellX = cell.x * kQuartSize;
        const int cellZ = cell.z * kQuartSize;
        const int paletteY = std::clamp(cell.y * kQuartSize, TerrainChunk::minY, TerrainChunk::maxY - 1);
        return chunkAt(cellX, cellZ).biomeAt(localBlock(cellX), paletteY, localBlock(cellZ));
    }

    [[nodiscard]] int worldSurface(int x, int z) const { return height(&TerrainChunk::worldSurface, x, z); }
    [[nodiscard]] int oceanFloor(int x, int z) const { return height(&TerrainChunk::oceanFloor, x, z); }
    [[nodiscard]] int motionBlocking(int x, int z) const { return height(&TerrainChunk::motionBlocking, x, z); }

    // Writes one block and keeps the chunk's bookkeeping in step: fluid
    // post-processing and all four heightmaps. A write that changes nothing is
    // dropped before any of that, so re-placing an identical block is free and
    // does not queue duplicate fluid work.
    void set(int x, int y, int z, Block block) {
        if (y < TerrainChunk::minY || y >= TerrainChunk::maxY) return;
        requireInsideRegion(x, z);

        TerrainChunk& chunk = chunkAt(x, z);
        const int localX = localBlock(x);
        const int localZ = localBlock(z);
        const Block previous = chunk.at(localX, y, localZ);
        if (previous == block) return;

        chunk.set(localX, y, localZ, block);
        if (isFluid(block)) chunk.fluidPostProcessing.push_back({localX, y, localZ});
        updateHeightmaps(chunk, localX, y, localZ, previous, block);
    }

private:
    // The heightmaps have to be updated *after* the write, because clearing the
    // top of a column rescans the blocks below it.
    static void updateHeightmaps(
        TerrainChunk& chunk, int localX, int y, int localZ, Block previous, Block block
    ) {
        const auto column = static_cast<std::size_t>(localZ * TerrainChunk::width + localX);
        for (const auto& map : kHeightmaps) {
            int& height = (chunk.*map.column)[column];
            if (map.counts(block)) {
                height = std::max(height, y + 1);
            } else if (map.counts(previous) && height == y + 1) {
                // This block was the top of the column; find the next one down
                // that still qualifies.
                height = y;
                while (height > TerrainChunk::minY
                       && !map.counts(chunk.at(localX, height - 1, localZ))) {
                    --height;
                }
            }
        }
    }

    void requireInsideRegion(int x, int z) const {
        const int chunkX = floorDiv(x, TerrainChunk::width);
        const int chunkZ = floorDiv(z, TerrainChunk::width);
        if (std::max(std::abs(chunkX - center_.x), std::abs(chunkZ - center_.z)) > kFeatureWriteRadius) {
            throw std::out_of_range("Feature write left its 3x3 WorldGenRegion");
        }
    }

    [[nodiscard]] const TerrainChunk& chunkAt(int x, int z) const {
        const ChunkPosition position{floorDiv(x, TerrainChunk::width), floorDiv(z, TerrainChunk::width)};
        const auto found = chunks_->find(position);
        if (found == chunks_->end()) throw std::out_of_range("WorldGenRegion chunk is unavailable");
        return found->second;
    }

    [[nodiscard]] TerrainChunk& chunkAt(int x, int z) {
        return const_cast<TerrainChunk&>(std::as_const(*this).chunkAt(x, z));
    }

    [[nodiscard]] int height(TerrainChunk::Heightmap TerrainChunk::*map, int x, int z) const {
        return (chunkAt(x, z).*map)[columnIndex(x, z)];
    }

    ChunkMap* chunks_;
    ChunkPosition center_;
    std::uint64_t biomeZoomSeed_;
};

// --- Structure pieces ------------------------------------------------------

// Java: `StructurePiece.postProcess`, clipped to the chunk being decorated.
// Pieces are placed once per chunk they overlap, so the clip is what keeps a
// piece spanning a chunk border from being written twice.
void placePiece(DecorationRegion& region, const StructurePiece& piece, ChunkPosition chunk) {
    const auto [originX, originZ] = blockOrigin(chunk);
    const BoundingBox& box = piece.bounds;
    const int minX = std::max(box.minX, originX);
    const int minZ = std::max(box.minZ, originZ);
    const int maxX = std::min(box.maxX, originX + TerrainChunk::width - 1);
    const int maxZ = std::min(box.maxZ, originZ + TerrainChunk::width - 1);
    if (minX > maxX || minZ > maxZ) return;

    // The bedrock floor at `minY` is never overwritten, so the vertical span
    // starts one above it.
    const int minY = std::max(box.minY, TerrainChunk::minY + 1);
    const int maxY = std::min(box.maxY, TerrainChunk::maxY - 1);

    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                const bool onShell = x == box.minX || x == box.maxX
                    || y == box.minY || y == box.maxY
                    || z == box.minZ || z == box.maxZ;
                if (!piece.hollow || onShell) {
                    region.set(x, y, z, piece.block);
                } else if (region.at(x, y, z) != Block::Bedrock) {
                    // Hollow out the interior, but never punch through bedrock
                    // the clip above could not exclude.
                    region.set(x, y, z, Block::Air);
                }
            }
        }
    }
}

// --- Feature placement -----------------------------------------------------

// Blocks an ore vein is allowed to replace: Java's `STONE_ORE_REPLACEABLES`,
// narrowed to the materials this palette produces underground.
[[nodiscard]] bool oreReplaceable(Block block) {
    using enum Block;
    return block == Stone || block == Deepslate || block == Tuff || block == Granite;
}

// Java: an `OreConfiguration` together with the height range and count of the
// placement it is registered with.
struct OreVein {
    Block ore;
    int veinsPerChunk;
    int minY;
    int maxY;
    int blocksPerVein;
};

// The five standard Overworld ore features. Every number is part of the world.
constexpr OreVein kCoalVein{.ore = Block::CoalOre, .veinsPerChunk = 16,
                            .minY = 0, .maxY = 192, .blocksPerVein = 12};
constexpr OreVein kIronVein{.ore = Block::IronOre, .veinsPerChunk = 12,
                            .minY = -56, .maxY = 96, .blocksPerVein = 9};
constexpr OreVein kGoldVein{.ore = Block::GoldOre, .veinsPerChunk = 4,
                            .minY = -56, .maxY = 32, .blocksPerVein = 8};
constexpr OreVein kRedstoneVein{.ore = Block::RedstoneOre, .veinsPerChunk = 8,
                                .minY = -63, .maxY = 16, .blocksPerVein = 8};
constexpr OreVein kDiamondVein{.ore = Block::DiamondOre, .veinsPerChunk = 4,
                               .minY = -63, .maxY = 16, .blocksPerVein = 6};

// Java: `OreFeature`. Each vein is a random walk from a uniformly chosen start,
// replacing the stone-like blocks it passes through. The walk is not clamped to
// the world: steps that leave it read as air and write nothing.
void placeOreVeins(
    DecorationRegion& region, WorldgenRandom& random, ChunkPosition chunk, const OreVein& vein
) {
    const auto [originX, originZ] = blockOrigin(chunk);
    for (int attempt = 0; attempt < vein.veinsPerChunk; ++attempt) {
        int x = originX + random.nextInt(TerrainChunk::width);
        int y = vein.minY + random.nextInt(vein.maxY - vein.minY + 1);
        int z = originZ + random.nextInt(TerrainChunk::width);
        for (int block = 0; block < vein.blocksPerVein; ++block) {
            if (oreReplaceable(region.at(x, y, z))) {
                region.set(x, y, z, vein.ore);
            }
            x += random.nextInt(3) - 1;
            y += random.nextInt(3) - 1;
            z += random.nextInt(3) - 1;
        }
    }
}

// Binds a vein configuration into the uniform placement signature the feature
// catalog stores.
template <OreVein Vein>
void placeOre(DecorationRegion& region, WorldgenRandom& random, ChunkPosition chunk) {
    placeOreVeins(region, random, chunk, Vein);
}

// Java: `DISK_CLAY`. A flat disk in the seabed under open water.
void placeClayDisks(DecorationRegion& region, WorldgenRandom& random, ChunkPosition chunk) {
    constexpr int attempts = 3;
    const auto [originX, originZ] = blockOrigin(chunk);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int x = originX + random.nextInt(TerrainChunk::width);
        const int z = originZ + random.nextInt(TerrainChunk::width);
        const int surface = region.worldSurface(x, z);
        if (surface <= TerrainChunk::minY || region.at(x, surface - 1, z) != Block::Water) continue;

        const int floor = region.oceanFloor(x, z) - 1;
        if (!surfaceBiome(region.biomeAt(x, floor + 1, z))) continue;

        const int radius = 2 + random.nextInt(3);
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dz * dz > radius * radius) continue;
                for (int y = floor - 1; y <= floor + 1; ++y) {
                    const Block block = region.at(x + dx, y, z + dz);
                    if (block == Block::Dirt || block == Block::Sand || block == Block::Gravel) {
                        region.set(x + dx, y, z + dz, Block::Clay);
                    }
                }
            }
        }
    }
}

// Java: `SPRING_WATER`. A single water source in a rock face with exactly one
// opening; see `canPlaceWaterSpring` for the rule.
void placeSprings(DecorationRegion& region, WorldgenRandom& random, ChunkPosition chunk) {
    constexpr int attempts = 16;
    constexpr int lowestY = TerrainChunk::minY + 8;
    constexpr int spanY = 120;
    const auto [originX, originZ] = blockOrigin(chunk);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int x = originX + random.nextInt(TerrainChunk::width);
        const int y = lowestY + random.nextInt(spanY);
        const int z = originZ + random.nextInt(TerrainChunk::width);
        if (!outsideDeepDark(region.biomeAt(x, y, z))) continue;

        const std::array<Block, 4> sides{
            region.at(x - 1, y, z), region.at(x + 1, y, z),
            region.at(x, y, z - 1), region.at(x, y, z + 1),
        };
        if (canPlaceWaterSpring(region.at(x, y, z), region.at(x, y + 1, z),
                                region.at(x, y - 1, z), sides)) {
            region.set(x, y, z, Block::Water);
        }
    }
}

// Java: the oak-like `TreeFeature`s, as one shape. A trunk on soil with a
// two-tier canopy whose corners are randomly cut.
void placeTrees(DecorationRegion& region, WorldgenRandom& random, ChunkPosition chunk) {
    const auto [originX, originZ] = blockOrigin(chunk);
    const int attempts = 2 + random.nextInt(4);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int x = originX + random.nextInt(TerrainChunk::width);
        const int z = originZ + random.nextInt(TerrainChunk::width);
        const int y = region.worldSurface(x, z);
        // Leave room for the trunk and the canopy above it.
        if (y < TerrainChunk::minY + 1 || y + 8 >= TerrainChunk::maxY) continue;
        if (!treeBiome(region.biomeAt(x, y, z))) continue;

        const Block ground = region.at(x, y - 1, z);
        if (ground != Block::Grass && ground != Block::Dirt && ground != Block::Podzol) continue;

        const int height = 4 + random.nextInt(3);
        bool clear = true;
        for (int trunkY = y; trunkY <= y + height; ++trunkY) {
            const Block block = region.at(x, trunkY, z);
            clear &= block == Block::Air || isLeaves(block);
        }
        if (!clear) continue;

        for (int trunkY = y; trunkY < y + height; ++trunkY) {
            region.set(x, trunkY, z, Block::OakLog);
        }
        // Canopy: a 5x5 slab for three layers, then a 3x3 cap one above the
        // trunk. Corners are dropped half the time, which is what gives the
        // silhouette its irregular edge.
        for (int dy = -2; dy <= 1; ++dy) {
            const int radius = dy == 1 ? 1 : 2;
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::abs(dx) == radius && std::abs(dz) == radius && random.nextBoolean()) continue;
                    const int leafY = y + height + dy;
                    const Block existing = region.at(x + dx, leafY, z + dz);
                    if (existing == Block::Air || isLeaves(existing)) {
                        region.set(x + dx, leafY, z + dz, Block::OakLeaves);
                    }
                }
            }
        }
    }
}

// Java: `SnowAndFreezeFeature`. The only feature that covers the whole chunk
// deterministically rather than sampling positions, hence the unused random.
void freezeTopLayer(DecorationRegion& region, WorldgenRandom&, ChunkPosition chunk) {
    const auto [originX, originZ] = blockOrigin(chunk);
    for (int x = originX; x < originX + TerrainChunk::width; ++x) {
        for (int z = originZ; z < originZ + TerrainChunk::width; ++z) {
            const int y = region.motionBlocking(x, z);
            if (!snowyBiome(region.biomeAt(x, y, z))) continue;
            if (y > TerrainChunk::minY && region.at(x, y - 1, z) == Block::Water) {
                region.set(x, y - 1, z, Block::Ice);
            }
            if (y < TerrainChunk::maxY && region.at(x, y, z) == Block::Air
                && blocksMotion(region.at(x, y - 1, z))) {
                region.set(x, y, z, Block::Snow);
            }
        }
    }
}

// --- The feature catalog ---------------------------------------------------

// One entry of Java's per-biome `PlacedFeature` lists, flattened: which step it
// belongs to, which biomes register it, and what it does.
struct FeatureDefinition {
    DecorationStep step;
    // Checked against the biomes present in the decoration neighbourhood, to
    // decide whether the feature runs at all. Features that care about the
    // biome under a specific position check it again themselves.
    bool (*registeredIn)(Biome);
    void (*place)(DecorationRegion&, WorldgenRandom&, ChunkPosition);
};

// Java's `FeatureSorter` output, precomputed by hand.
//
// A feature's index *within its step* is part of its seed, so this table's
// order is part of the generated world: appending is safe, reordering or
// removing an entry is not.
constexpr std::array kFeatures{
    FeatureDefinition{DecorationStep::UndergroundOres, surfaceBiome, placeClayDisks},
    FeatureDefinition{DecorationStep::UndergroundOres, everyBiome, placeOre<kCoalVein>},
    FeatureDefinition{DecorationStep::UndergroundOres, everyBiome, placeOre<kIronVein>},
    FeatureDefinition{DecorationStep::UndergroundOres, everyBiome, placeOre<kGoldVein>},
    FeatureDefinition{DecorationStep::UndergroundOres, everyBiome, placeOre<kRedstoneVein>},
    FeatureDefinition{DecorationStep::UndergroundOres, everyBiome, placeOre<kDiamondVein>},
    FeatureDefinition{DecorationStep::FluidSprings, outsideDeepDark, placeSprings},
    FeatureDefinition{DecorationStep::VegetalDecoration, treeBiome, placeTrees},
    FeatureDefinition{DecorationStep::TopLayerModification, snowyBiome, freezeTopLayer},
};

} // namespace

void decorateChunk(ChunkMap& chunks, const StructureIndex& structures, ChunkPosition chunk) {
    const std::int64_t seed = structures.seed();
    DecorationRegion region(chunks, chunk, seed);
    const std::vector<StructureReference>& references = structures.references(chunk);
    const BiomeSet present = collectBiomes(chunks, chunk);

    // Java: `setDecorationSeed`. One seed per chunk, derived from the world
    // seed and the chunk's origin, that every feature below reseeds from.
    WorldgenRandom random(0); // The initial seed is irrelevant; the next line replaces it.
    const auto [originX, originZ] = blockOrigin(chunk);
    const std::uint64_t decorationSeed = random.setDecorationSeed(seed, originX, originZ);

    for (int stepOrdinal = 0; stepOrdinal < kDecorationStepCount; ++stepOrdinal) {
        const auto step = static_cast<DecorationStep>(stepOrdinal);

        // Structures first. Every kind registered to this step consumes an
        // index whether or not this chunk references one, because the index
        // comes from the registry, not from what is present.
        int indexInStep = 0;
        for (std::size_t kind = 0; kind < kStructureKindCount; ++kind) {
            const auto structure = static_cast<StructureKind>(kind);
            if (structureStep(structure) != step) continue;
            random.setFeatureSeed(decorationSeed, indexInStep++, stepOrdinal);
            for (const StructureReference& reference : references) {
                if (reference.kind != structure) continue;
                if (const StructureStart* start = structures.resolve(reference)) {
                    for (const StructurePiece& piece : start->pieces) {
                        placePiece(region, piece, chunk);
                    }
                }
            }
        }

        // Then biome decoration, indexed the same way: a feature no nearby
        // biome registers still consumes its index, it just does not run.
        indexInStep = 0;
        for (const FeatureDefinition& feature : kFeatures) {
            if (feature.step != step) continue;
            const int index = indexInStep++;
            if (!anyBiome(present, feature.registeredIn)) continue;
            random.setFeatureSeed(decorationSeed, index, stepOrdinal);
            feature.place(region, random, chunk);
        }
    }
}

} // namespace mcworld::detail
