#include "mcworld/generation.hpp"
#include "generation_internal.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FixedBiome final : public mcworld::BiomeSource {
public:
    explicit FixedBiome(mcworld::Biome biome) : biome_(biome) {}
    mcworld::Biome sample(const mcworld::OverworldNoiseRouter&, int, int, int) const override { return biome_; }

private:
    mcworld::Biome biome_;
};

void testStructureMetadata() {
    mcworld::OverworldNoiseRouter router(12345);
    mcworld::GenerationOptions options;
    options.terrain.biomes = std::make_shared<FixedBiome>(mcworld::Biome::Plains);
    mcworld::OverworldWorldGenerator generator(router, options);

    bool village = false;
    bool mineshaft = false;
    bool portal = false;
    mcworld::StructureStart selected;
    mcworld::ChunkPosition villageSource{}, mineshaftSource{}, portalSource{};
    for (int z = -64; z <= 64; ++z) for (int x = -64; x <= 64; ++x) {
        for (const auto& start : generator.structureStarts(x, z)) {
            check(start.valid(), "stored structure starts have pieces");
            check(start.bounds().intersectsChunk(x, z), "structure starts intersect their source chunk");
            village |= start.kind == mcworld::StructureKind::Village;
            mineshaft |= start.kind == mcworld::StructureKind::Mineshaft;
            portal |= start.kind == mcworld::StructureKind::RuinedPortal;
            if (start.kind == mcworld::StructureKind::Village) villageSource = start.source;
            if (start.kind == mcworld::StructureKind::Mineshaft) mineshaftSource = start.source;
            if (start.kind == mcworld::StructureKind::RuinedPortal) portalSource = start.source;
            if (!selected.valid()) selected = start;
        }
    }
    check(village && mineshaft && portal, "built-in random-spread structure sets produce starts");
    check(selected.valid(), "structure scan found a start");
    if (selected.valid()) {
        const auto references = generator.structureReferences(selected.source.x, selected.source.z);
        check(std::ranges::any_of(references, [&](const auto& reference) {
            return reference.kind == selected.kind && reference.source == selected.source;
        }), "source chunk references its intersecting structure start");
    }

    const auto hasStart = [&](mcworld::Biome biome, mcworld::ChunkPosition source, mcworld::StructureKind kind) {
        auto fixed = options;
        fixed.terrain.biomes = std::make_shared<FixedBiome>(biome);
        mcworld::OverworldWorldGenerator world(router, fixed);
        return std::ranges::any_of(world.structureStarts(source.x, source.z),
            [&](const auto& start) { return start.kind == kind; });
    };
    using enum mcworld::Biome;
    using enum mcworld::StructureKind;
    for (const auto biome : {Plains, Meadow, Desert, Savanna, SnowyPlains, Taiga}) {
        check(hasStart(biome, villageSource, Village), "village tag union accepts all six supported biomes");
    }
    for (const auto biome : {Forest, SunflowerPlains, SnowyTaiga, Ocean, DeepDark}) {
        check(!hasStart(biome, villageSource, Village), "village tag union rejects non-village biomes");
    }
    check(hasStart(Ocean, mineshaftSource, Mineshaft), "mineshafts are allowed beneath oceans");
    check(!hasStart(DeepDark, mineshaftSource, Mineshaft), "deep dark rejects mineshaft starts");
    for (const auto biome : {DripstoneCaves, LushCaves, SulfurCaves}) {
        check(hasStart(biome, portalSource, RuinedPortal), "standard portals allow tagged cave biomes");
    }
    check(!hasStart(DeepDark, portalSource, RuinedPortal), "deep dark rejects ruined portals");

    auto terrainOnly = options;
    terrainOnly.features = false;
    terrainOnly.terrain.carvers = false;
    mcworld::OverworldWorldGenerator adapting(router, terrainOnly);
    const auto adapted = adapting.generate(villageSource.x, villageSource.z);
    terrainOnly.structures = false;
    mcworld::OverworldWorldGenerator unadapted(router, terrainOnly);
    const auto plain = unadapted.generate(villageSource.x, villageSource.z);
    mcworld::OverworldTerrainGenerator base(router, terrainOnly.terrain);
    const auto baseline = base.generate(villageSource.x, villageSource.z);
    check(plain.terrain.blocks == baseline.blocks, "disabled stages 5/8 preserve standalone terrain output");
    check(adapted.terrain.blocks != plain.terrain.blocks, "referenced village beardification reaches terrain before decoration");
    check(std::count(adapted.terrain.blocks.begin(), adapted.terrain.blocks.end(), mcworld::Block::OakPlanks) == 0,
          "disabling FEATURES preserves adaptation but skips structure block placement");

    options.structures = false;
    mcworld::OverworldWorldGenerator disabled(router, options);
    check(disabled.structureStarts(0, 0).empty() && disabled.structureReferences(0, 0).empty(),
          "structure option disables starts and references");
}

void testSpringRules() {
    using enum mcworld::Block;
    using mcworld::detail::canPlaceWaterSpring;
    const std::array sides{Stone, Granite, Tuff, Air};
    check(canPlaceWaterSpring(Air, Calcite, Dirt, sides), "spring accepts air origin and valid non-stone rocks");
    check(canPlaceWaterSpring(Deepslate, Stone, Stone, sides), "spring accepts replaceable rock origin");
    check(!canPlaceWaterSpring(Stone, Air, Stone, {Stone, Stone, Stone, Stone}),
          "spring cannot use a ceiling hole even with five solid neighbors");
    check(!canPlaceWaterSpring(Stone, Stone, Air, {Stone, Stone, Stone, Stone}),
          "water spring requires valid rock below");
    check(!canPlaceWaterSpring(Stone, Stone, Stone, {OakPlanks, Stone, Stone, Air}),
          "spring rock count uses configured blocks rather than motion blocking");
    check(!canPlaceWaterSpring(Water, Stone, Stone, sides), "spring does not replace fluid origin");
    check(!canPlaceWaterSpring(Stone, Stone, Stone, {Stone, Stone, Air, Air}), "spring requires exactly one hole");
}

void testBeardKernel() {
    // Java float-bit fixtures from the 26.3 Beardifier/Mth expressions.
    // Large ground offsets exercise BEARD_BOX separately from kernel Y.
    struct Fixture { int x, y, z, ground; std::uint32_t bits; };
    constexpr Fixture fixtures[]{
        {0, 0, 0, 0, 0xbf322b12U},
        {1, -2, 3, -2, 0x3e1018f3U},
        {11, 11, 11, 10000, 0xae57bbbfU},
        {3, 0, 7, 10000, 0xbc97d50aU},
        {-12, -12, -12, -12, 0x2bdaab92U},
        {12, 0, 0, 0, 0U},
        {0, -13, 0, 0, 0U},
    };
    for (const auto& f : fixtures) {
        check(std::bit_cast<std::uint32_t>(mcworld::detail::structureBeardContribution(f.x, f.y, f.z, f.ground)) == f.bits,
              "beard kernel matches Java float bits and asymmetric kernel limits");
    }
}

void testAdjustedBounds() {
    mcworld::StructureStart start;
    start.pieces = {{{0, 0, 0, 15, 5, 15}}, {{16, -2, 4, 20, 8, 10}}};
    const mcworld::BoundingBox raw{0, -2, 0, 20, 8, 15};
    check(start.bounds() == raw, "non-adapting start returns union of piece bounds");
    start.adjustment = mcworld::TerrainAdjustment::BeardThin;
    check(start.bounds() == raw.inflated(12), "adapting start exposes Java's adjusted reference bounds");
    check(start.bounds().intersectsChunk(-1, 0), "adaptation bounds reference neighboring chunk without piece overlap");
    check(!start.bounds().intersectsChunk(-2, 0), "reference bounds stop at the twelve-block padding");
}

void testDecoration() {
    mcworld::OverworldNoiseRouter router(91);
    mcworld::GenerationOptions options;
    options.structures = false;
    options.terrain.aquifers = false;
    options.terrain.oreVeins = false;
    options.terrain.carvers = false;
    options.terrain.biomes = std::make_shared<FixedBiome>(mcworld::Biome::Forest);
    mcworld::OverworldWorldGenerator generator(router, options);
    const auto generated = generator.generate(0, 0);
    const auto& chunk = generated.terrain;

    const auto ore = std::count_if(chunk.blocks.begin(), chunk.blocks.end(), [](mcworld::Block block) {
        using enum mcworld::Block;
        return block == CoalOre || block == IronOre || block == GoldOre
            || block == RedstoneOre || block == DiamondOre;
    });
    const auto trees = std::count(chunk.blocks.begin(), chunk.blocks.end(), mcworld::Block::OakLog)
        + std::count(chunk.blocks.begin(), chunk.blocks.end(), mcworld::Block::OakLeaves);
    check(ore > 0, "underground-ore stage places configured ores");
    check(trees > 0, "vegetal-decoration stage places trees");
    check(std::ranges::all_of(chunk.fluidPostProcessing, [&](mcworld::BlockPosition position) {
        return mcworld::isFluid(chunk.at(position.x, position.y, position.z));
    }), "decoration fluid work items only reference existing fluids");

    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
        int surface = mcworld::TerrainChunk::minY;
        int floor = mcworld::TerrainChunk::minY;
        int motion = mcworld::TerrainChunk::minY;
        int noLeaves = mcworld::TerrainChunk::minY;
        for (int y = mcworld::TerrainChunk::minY; y < mcworld::TerrainChunk::maxY; ++y) {
            const auto block = chunk.at(x, y, z);
            if (block != mcworld::Block::Air) surface = y + 1;
            if (mcworld::blocksMotion(block)) floor = y + 1;
            if (mcworld::blocksMotion(block) || mcworld::isFluid(block)) motion = y + 1;
            if ((mcworld::blocksMotion(block) && !mcworld::isLeaves(block)) || mcworld::isFluid(block)) noLeaves = y + 1;
        }
        const int column = z * 16 + x;
        check(chunk.worldSurface[column] == surface && chunk.oceanFloor[column] == floor
              && chunk.motionBlocking[column] == motion && chunk.motionBlockingNoLeaves[column] == noLeaves,
              "feature writes maintain all heightmaps live");
    }
}

void testAreaValidation() {
    mcworld::OverworldNoiseRouter router(0);
    mcworld::OverworldWorldGenerator generator(router);
    bool rejected = false;
    try { (void)generator.generateArea(0, 0, 0, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "empty generated area rejected");

    mcworld::GeneratedArea area;
    rejected = false;
    try { (void)area.at(0, 0); }
    catch (const std::out_of_range&) { rejected = true; }
    check(rejected, "generated-area access is bounds checked");

    rejected = false;
    try { (void)generator.structureReferences(std::numeric_limits<int>::max(), 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "overflowing structure-reference neighborhood rejected");

    rejected = false;
    try {
        (void)mcworld::BoundingBox{std::numeric_limits<int>::min(), 0, 0, 0, 0, 0}.inflated(1);
    } catch (const std::overflow_error&) { rejected = true; }
    check(rejected, "bounding-box inflation rejects integer overflow");
}

} // namespace

int main() {
    testStructureMetadata();
    testSpringRules();
    testBeardKernel();
    testAdjustedBounds();
    testDecoration();
    testAreaValidation();
    if (failures) std::cerr << failures << " generation checks failed\n";
    else std::cout << "All generation checks passed\n";
    return failures ? 1 : 0;
}
