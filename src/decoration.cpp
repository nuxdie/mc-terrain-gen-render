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
#include "feature_placement.hpp"
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

// The deep dark omits the default spring list.
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
class DecorationRegion final : public FeatureWorld {
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

    int height(FeatureHeightmap type, int x, int z) const override {
        switch (type) {
        case FeatureHeightmap::WorldSurface: return worldSurface(x, z);
        case FeatureHeightmap::OceanFloor: return oceanFloor(x, z);
        case FeatureHeightmap::MotionBlocking: return motionBlocking(x, z);
        }
        return TerrainChunk::minY;
    }

    bool canWrite(BlockPosition p) const override {
        return p.y >= TerrainChunk::minY && p.y < TerrainChunk::maxY
            && std::abs(floorDiv(p.x, 16) - center_.x) <= kFeatureWriteRadius
            && std::abs(floorDiv(p.z, 16) - center_.z) <= kFeatureWriteRadius;
    }

    void setData(int x, int y, int z, std::shared_ptr<const BlockData> data) override {
        chunkAt(x, z).setData(localBlock(x), y, localBlock(z), std::move(data));
    }
    const BlockData* dataAt(int x, int y, int z) const override {
        if (y < TerrainChunk::minY || y >= TerrainChunk::maxY) return nullptr;
        return chunkAt(x, z).dataAt(localBlock(x), y, localBlock(z));
    }

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
        if (previous == block) {
            chunk.setData(localX, y, localZ, nullptr);
            return;
        }

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
void placePiece(DecorationRegion& region, const StructurePiece& piece, ChunkPosition chunk, WorldgenRandom& random) {
    const auto [originX, originZ] = blockOrigin(chunk);
    const BoundingBox& box = piece.bounds;
    const int minX = std::max(box.minX, originX);
    const int minZ = std::max(box.minZ, originZ);
    const int maxX = std::min(box.maxX, originX + TerrainChunk::width - 1);
    const int maxZ = std::min(box.maxZ, originZ + TerrainChunk::width - 1);
    if (minX > maxX || minZ > maxZ) return;
    if (!piece.feature.empty()) {
        FeatureContext context{region, random, {}};
        (void)placePoolFeature(piece.feature, context, {box.minX, box.minY, box.minZ});
        return;
    }

    if (piece.templatePiece) {
        for (const auto& block : piece.blocks) {
            const auto [x, y, z] = block.position;
            if (x >= minX && x <= maxX && z >= minZ && z <= maxZ && y > TerrainChunk::minY && y < TerrainChunk::maxY) {
                std::shared_ptr<const BlockData> data;
                if (const auto processed = processStructureBlock(block, region.at(x, y, z), piece.processors, &data)) {
                    region.set(x, y, z, *processed);
                    region.setData(x, y, z, std::move(data));
                }
            }
        }
        return;
    }

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

bool placeTree(FeatureContext& context, BlockPosition origin) {
    const Biome biome = context.world.biomeAt(origin.x, origin.y, origin.z);
    const TreeShape shape = biome == Biome::BirchForest || biome == Biome::OldGrowthBirchForest ? TreeShape::Birch
        : biome == Biome::OldGrowthPineTaiga ? TreeShape::Pine
        : biome == Biome::Taiga || biome == Biome::SnowyTaiga || biome == Biome::OldGrowthSpruceTaiga || biome == Biome::Grove ? TreeShape::Spruce
        : TreeShape::Oak;
    return straightTreeFeature(shape)(context, origin);
}

// Java: `SnowAndFreezeFeature`. The only feature that covers the whole chunk
// deterministically rather than sampling positions, hence the unused random.
bool freezeTopLayer(FeatureContext& context, BlockPosition origin) {
    auto& region = context.world;
    const int originX = origin.x;
    const int originZ = origin.z;
    for (int x = originX; x < originX + TerrainChunk::width; ++x) {
        for (int z = originZ; z < originZ + TerrainChunk::width; ++z) {
            const int y = region.height(FeatureHeightmap::MotionBlocking, x, z);
            const Biome biome = region.biomeAt(x, y, z);
            if (y > TerrainChunk::minY && biomeTemperature(biome, x, y - 1, z) < 0.15F
                && region.at(x, y - 1, z) == Block::Water) {
                region.set(x, y - 1, z, Block::Ice);
            }
            const Block below = region.at(x, y - 1, z);
            if (y < TerrainChunk::maxY && biomeTemperature(biome, x, y, z) < 0.15F
                && biomeHasPrecipitation(biome) && region.at(x, y, z) == Block::Air
                && blocksMotion(below) && below != Block::Ice && below != Block::PackedIce) {
                region.set(x, y, z, Block::Snow);
            }
        }
    }
    return true;
}

// --- The feature catalog ---------------------------------------------------

// One entry of Java's per-biome `PlacedFeature` lists, flattened: which step it
// belongs to, which biomes register it, and what it does.
struct FeatureDefinition {
    std::string name;
    DecorationStep step;
    // Checked against the biomes present in the decoration neighbourhood, to
    // decide whether the feature runs at all. Features that care about the
    // biome under a specific position check it again themselves.
    bool (*registeredIn)(Biome);
    PlacedFeature placed;
};

// Build per-biome identity lists, then run Java's dependency ordering once.
// Changing registration or biome membership can change feature indices/seeds.
struct FeatureCatalog {
    std::vector<FeatureDefinition> features;
    FeatureOrder order;
    FeatureCatalog();
};

FeatureCatalog::FeatureCatalog() {
    using namespace placement;
    const auto addOre = [&](std::string name, Block stoneOre, Block deepOre, int size, float discard,
                            PlacementModifier frequency, IntProvider height, bool (*biomes)(Biome) = everyBiome) {
        std::vector<OreTarget> targets{
            {[](Block b) { return b == Block::Stone || b == Block::Granite || b == Block::Diorite || b == Block::Andesite; }, stoneOre},
            {[](Block b) { return b == Block::Deepslate || b == Block::Tuff; }, deepOre}};
        features.push_back({std::move(name), DecorationStep::UndergroundOres, biomes,
            {oreFeature(std::move(targets), size, discard), {std::move(frequency), square(), heightRange(std::move(height)), biome()}}});
    };
    // BiomeDefaultFeatures.addDefaultUndergroundVariety / addDefaultOres.
    addOre("ore_dirt", Block::Dirt, Block::Dirt, 33, 0, count(constant(7)), uniform(0, 160));
    addOre("ore_gravel", Block::Gravel, Block::Gravel, 33, 0, count(constant(14)), uniform(-64, 319));
    for (const auto& [name, block] : std::array{std::pair{"granite", Block::Granite},
            std::pair{"diorite", Block::Diorite}, std::pair{"andesite", Block::Andesite}}) {
        addOre(std::string("ore_") + name + "_upper", block, block, 64, 0, rarity(6), uniform(64, 128));
        addOre(std::string("ore_") + name + "_lower", block, block, 64, 0, count(constant(2)), uniform(0, 60));
    }
    addOre("ore_tuff", Block::Tuff, Block::Tuff, 64, 0, count(constant(2)), uniform(-64, 0));
    addOre("ore_coal_upper", Block::CoalOre, Block::DeepslateCoalOre, 17, 0, count(constant(30)), uniform(136, 319));
    addOre("ore_coal_lower", Block::CoalOre, Block::DeepslateCoalOre, 17, .5F, count(constant(20)), triangle(0, 192));
    addOre("ore_iron_upper", Block::IronOre, Block::DeepslateIronOre, 9, 0, count(constant(90)), triangle(80, 384));
    addOre("ore_iron_middle", Block::IronOre, Block::DeepslateIronOre, 9, 0, count(constant(10)), triangle(-24, 56));
    addOre("ore_iron_small", Block::IronOre, Block::DeepslateIronOre, 4, 0, count(constant(10)), uniform(-64, 72));
    addOre("ore_gold", Block::GoldOre, Block::DeepslateGoldOre, 9, .5F, count(constant(4)), triangle(-64, 32));
    addOre("ore_gold_lower", Block::GoldOre, Block::DeepslateGoldOre, 9, .5F, count(uniform(0, 1)), uniform(-64, -48));
    addOre("ore_redstone", Block::RedstoneOre, Block::DeepslateRedstoneOre, 8, 0, count(constant(4)), uniform(-64, 15));
    addOre("ore_redstone_lower", Block::RedstoneOre, Block::DeepslateRedstoneOre, 8, 0, count(constant(8)), triangle(-96, -32));
    addOre("ore_diamond", Block::DiamondOre, Block::DeepslateDiamondOre, 4, .5F, count(constant(7)), triangle(-144, 16));
    addOre("ore_diamond_medium", Block::DiamondOre, Block::DeepslateDiamondOre, 8, .5F, count(constant(2)), uniform(-64, -4));
    addOre("ore_diamond_large", Block::DiamondOre, Block::DeepslateDiamondOre, 12, .7F, rarity(9), triangle(-144, 16));
    addOre("ore_diamond_buried", Block::DiamondOre, Block::DeepslateDiamondOre, 8, 1, count(constant(4)), triangle(-144, 16));
    addOre("ore_lapis", Block::LapisOre, Block::DeepslateLapisOre, 7, 0, count(constant(2)), triangle(-32, 32));
    addOre("ore_lapis_buried", Block::LapisOre, Block::DeepslateLapisOre, 7, 1, count(constant(4)), uniform(-64, 64));
    addOre("ore_copper", Block::CopperOre, Block::DeepslateCopperOre, 10, 0, count(constant(16)), triangle(-16, 112),
        [](Biome b) { return b != Biome::DripstoneCaves; });
    addOre("ore_copper_large", Block::CopperOre, Block::DeepslateCopperOre, 20, 0, count(constant(16)), triangle(-16, 112),
        [](Biome b) { return b == Biome::DripstoneCaves; });
    addOre("ore_gold_extra", Block::GoldOre, Block::DeepslateGoldOre, 9, 0, count(constant(50)), uniform(32, 256),
        [](Biome b) { return b == Biome::Badlands || b == Biome::ErodedBadlands || b == Biome::WoodedBadlands; });
    addOre("ore_emerald", Block::EmeraldOre, Block::DeepslateEmeraldOre, 3, 0, count(constant(100)), triangle(-16, 480),
        [](Biome b) { return b == Biome::WindsweptHills || b == Biome::WindsweptForest || b == Biome::WindsweptGravellyHills
            || b == Biome::Meadow || b == Biome::CherryGrove || b == Biome::Grove || b == Biome::SnowySlopes
            || b == Biome::FrozenPeaks || b == Biome::JaggedPeaks || b == Biome::StonyPeaks; });

    const auto wet = filter([](const FeatureWorld& w, BlockPosition p) { return w.at(p.x, p.y, p.z) == Block::Water; });
    const auto soil = [](Block b) { return b == Block::Dirt || b == Block::Grass; };
    features.push_back({"disk_sand", DecorationStep::UndergroundOres, surfaceBiome,
        {diskFeature([](FeatureContext& c, BlockPosition p) {
            return c.world.at(p.x, p.y - 1, p.z) == Block::Air ? Block::Sandstone : Block::Sand;
        }, soil, uniform(2, 6), 2), {count(constant(3)), square(), heightmap(FeatureHeightmap::OceanFloor), wet, biome()}}});
    features.push_back({"disk_clay", DecorationStep::UndergroundOres, surfaceBiome,
        {diskFeature(Block::Clay, [](Block b) { return b == Block::Dirt || b == Block::Clay; }, uniform(2, 3), 1),
         {square(), heightmap(FeatureHeightmap::OceanFloor), wet, biome()}}});
    features.push_back({"disk_gravel", DecorationStep::UndergroundOres, surfaceBiome,
        {diskFeature(Block::Gravel, soil, uniform(2, 5), 2), {square(), heightmap(FeatureHeightmap::OceanFloor), wet, biome()}}});
    features.push_back({"spring_water", DecorationStep::FluidSprings, outsideDeepDark,
        {springFeature(Block::Water, springRock), {count(constant(25)), square(), heightRange(uniform(-64, 192)), biome()}}});
    features.push_back({"spring_lava", DecorationStep::FluidSprings, outsideDeepDark,
        {springFeature(Block::Lava, [](Block b) { return springRock(b) && b != Block::Snow && b != Block::PowderSnow && b != Block::PackedIce; }),
         {count(constant(20)), square(), heightRange(veryBiasedToBottom(-64, 311, 8)), biome()}}});
    features.push_back({"trees", DecorationStep::VegetalDecoration, treeBiome,
        {placeTree, {count(countExtra(10, .1F, 1)), square(), heightmap(FeatureHeightmap::OceanFloor), biome()}}});
    features.push_back({"freeze_top_layer", DecorationStep::TopLayerModification, everyBiome, {freezeTopLayer, {}}});

    std::vector<BiomeFeatureList> lists(kBiomeCount);
    for (std::size_t b = 0; b < lists.size(); ++b) for (FeatureId id = 0; id < features.size(); ++id) {
        if (features[id].registeredIn(static_cast<Biome>(b)))
            lists[b][static_cast<int>(features[id].step)].push_back(id);
    }
    order = sortFeatures(lists);
}

} // namespace

void decorateChunk(ChunkMap& chunks, const StructureIndex& structures, ChunkPosition chunk) {
    const std::int64_t seed = structures.seed();
    DecorationRegion region(chunks, chunk, seed);
    const std::vector<StructureReference>& references = structures.references(chunk);
    const BiomeSet present = collectBiomes(chunks, chunk);
    static const FeatureCatalog catalog;

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
            for (StructureVariant variant : structureVariants(structure)) {
                random.setFeatureSeed(decorationSeed, indexInStep++, stepOrdinal);
                for (const StructureReference& reference : references) {
                    if (reference.kind != structure) continue;
                    if (const StructureStart* start = structures.resolve(reference); start && start->variant == variant) {
                        for (const StructurePiece& piece : start->pieces) {
                            placePiece(region, piece, chunk, random);
                        }
                    }
                }
            }
        }

        // Then biome decoration, indexed the same way: a feature no nearby
        // biome registers still consumes its index, it just does not run.
        indexInStep = 0;
        for (FeatureId id : catalog.order[stepOrdinal]) {
            const auto& feature = catalog.features[id];
            const int index = indexInStep++;
            if (!anyBiome(present, feature.registeredIn)) continue;
            random.setFeatureSeed(decorationSeed, index, stepOrdinal);
            FeatureContext context{region, random, feature.registeredIn};
            (void)feature.placed.place(context, {originX, TerrainChunk::minY, originZ});
        }
    }
}

} // namespace mcworld::detail
