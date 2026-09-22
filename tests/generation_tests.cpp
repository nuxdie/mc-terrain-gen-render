#include "mcworld/generation.hpp"
#include "generation_internal.hpp"
#include "feature_placement.hpp"
#include "mcworld/structure_templates.hpp"
#include "terrain_internal.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>

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
    const auto variantAt = [&](mcworld::Biome biome, mcworld::ChunkPosition source, mcworld::StructureKind kind) {
        auto fixed = options;
        fixed.terrain.biomes = std::make_shared<FixedBiome>(biome);
        mcworld::OverworldWorldGenerator world(router, fixed);
        for (const auto& start : world.structureStarts(source.x, source.z)) if (start.kind == kind) return start.variant;
        return mcworld::StructureVariant::Generic;
    };
    check(variantAt(Desert, villageSource, Village) == mcworld::StructureVariant::VillageDesert,
          "weighted village variants retry until the biome-eligible style generates");
    check(variantAt(Badlands, mineshaftSource, Mineshaft) == mcworld::StructureVariant::MineshaftMesa,
          "badlands select the mesa mineshaft registry entry");
    check(variantAt(WarmOcean, portalSource, RuinedPortal) == mcworld::StructureVariant::PortalOcean,
          "all ocean types select the ocean portal variant");

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

    auto catalog = std::make_shared<mcworld::StructureTemplateCatalog>();
    mcworld::StructureTemplate marker;
    marker.size = {1, 1, 1};
    marker.blocks = {{{0, 0, 0}, mcworld::Block::Bricks}};
    catalog->templates["marker"] = marker;
    catalog->pools["marker_pool"].elements = {{"marker", 1, mcworld::PieceProjection::Rigid}};
    catalog->starts[mcworld::StructureVariant::VillagePlains] = {"marker_pool", 0, 16, 120, false};
    auto templateOptions = options;
    templateOptions.templates = catalog;
    mcworld::OverworldWorldGenerator templateGenerator(router, templateOptions);
    const auto templateStarts = templateGenerator.structureStarts(villageSource.x, villageSource.z);
    check(std::ranges::any_of(templateStarts, [](const auto& start) {
        return start.variant == mcworld::StructureVariant::VillagePlains && start.pieces[0].templatePiece;
    }), "public template catalog replaces the selected structure variant");
    mcworld::detail::ChunkMap region;
    for (int z = villageSource.z - 1; z <= villageSource.z + 1; ++z) for (int x = villageSource.x - 1; x <= villageSource.x + 1; ++x) {
        mcworld::TerrainChunk empty;
        empty.chunkX = x; empty.chunkZ = z;
        empty.biomes.fill(mcworld::Biome::Plains);
        empty.primeHeightmaps();
        region.emplace(mcworld::ChunkPosition{x, z}, std::move(empty));
    }
    mcworld::detail::StructureIndex templateIndex(router, *options.terrain.biomes, true, catalog.get());
    mcworld::detail::decorateChunk(region, templateIndex, villageSource);
    check(region.at(villageSource).at(0, 120, 0) == mcworld::Block::Bricks,
          "stage 8 resolves selected template metadata and places explicit blocks");
    check(region.at(villageSource).worldSurface[0] >= 121, "template writes update live feature heightmaps");

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

class TestFeatureWorld final : public mcworld::detail::FeatureWorld {
public:
    int ceiling = 64;
    mcworld::Biome biome = mcworld::Biome::Plains;
    std::map<std::tuple<int, int, int>, mcworld::Block> edits;
    mcworld::Block at(int x, int y, int z) const override {
        const auto it = edits.find({x, y, z});
        return it == edits.end() ? (y < ceiling ? mcworld::Block::Stone : mcworld::Block::Air) : it->second;
    }
    void set(int x, int y, int z, mcworld::Block b) override { edits[{x, y, z}] = b; }
    mcworld::Biome biomeAt(int, int, int) const override { return biome; }
    int height(mcworld::detail::FeatureHeightmap, int, int) const override { return ceiling; }
    bool canWrite(mcworld::BlockPosition p) const override { return p.y >= -64 && p.y < 320; }
};

void testFeatureSorterAndModifiers() {
    using namespace mcworld;
    using namespace mcworld::detail;
    BiomeFeatureList a{}, b{};
    a[6] = {10, 30}; b[6] = {20, 30};
    auto sorted = sortFeatures({a, b});
    check(sorted[6] == std::vector<FeatureId>({20, 10, 30}), "FeatureSorter uses reverse ordered DFS rather than registration sort");
    b[6] = {30, 10};
    bool rejected = false;
    try { (void)sortFeatures({a, b}); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "FeatureSorter rejects biome dependency cycles");

    TestFeatureWorld world;
    WorldgenRandom random(123), expectedRandom(123);
    FeatureContext context{world, random, [](Biome biome) { return biome == Biome::Plains; }};
    std::vector<BlockPosition> observed, expected;
    PlacedFeature feature{[&](FeatureContext& c, BlockPosition p) {
        p.y = c.random.nextInt(1000); observed.push_back(p); return true;
    }, {placement::count(placement::constant(3)), placement::square(), placement::biome()}};
    for (int i = 0; i < 3; ++i) {
        const int x = -16 + expectedRandom.nextInt(16);
        const int z = -32 + expectedRandom.nextInt(16);
        const int y = expectedRandom.nextInt(1000);
        expected.push_back({x, y, z});
    }
    check(feature.place(context, {-16, 0, -32}) && observed == expected,
          "placement is depth-first and terminals consume the shared stream before the next square");
    check(random.nextLong() == expectedRandom.nextLong(), "placement preserves Java random consumption");
    world.biome = Biome::Desert;
    observed.clear();
    check(!feature.place(context, {}) && observed.empty(), "biome modifier rejects terminal origins");
    world.biome = Biome::Plains;
    PlacedFeature nested{sequenceFeature({feature, feature}), {}};
    check(nested.place(context, {}) && observed.size() == 6, "nested placed features retain root biome context and visit every child");

    std::vector<int> ys;
    PlacedFeature scan{[&](FeatureContext&, BlockPosition p) { ys.push_back(p.y); return true; },
        {placement::scan(-1, 8, [](const FeatureWorld& w, BlockPosition p) { return w.at(p.x, p.y, p.z) == Block::Stone; },
            [](const FeatureWorld& w, BlockPosition p) { return w.at(p.x, p.y, p.z) == Block::Air; })}};
    check(scan.place(context, {0, 70, 0}) && ys == std::vector<int>{63}, "environment scan accepts target at the allowed-search boundary");
}

void testOreReferenceMasks() {
    using namespace mcworld;
    using namespace mcworld::detail;
    // Independent Java source-expression harness: full mask plus the random
    // stream immediately after placement, including air-discard draws.
    struct Fixture { std::int64_t seed; int size; float discard; int ceiling; std::uint64_t hash; std::size_t count; std::uint64_t next; };
    const Fixture fixtures[]{
        {123, 17, 0, 64, 0xdfbff8bda767160bULL, 20, 0x9b5545a2adec030fULL},
        {-91, 64, 0, 64, 0x7ec032024b74bad1ULL, 642, 0xce246410b25a863dULL},
        {987, 12, .7F, 0, 0x01105db78713159fULL, 0, 0xc36ce00f15c854a1ULL},
        {0, 8, 1, 0, 0xd5a60604839cba4dULL, 2, 0x6f0ee668cbab6052ULL},
    };
    for (const auto& fixture : fixtures) {
        TestFeatureWorld world;
        world.ceiling = fixture.ceiling;
        WorldgenRandom random(static_cast<std::uint64_t>(fixture.seed));
        FeatureContext context{world, random, {}};
        auto feature = oreFeature({{[](Block b) { return b == Block::Stone; }, Block::DiamondOre}}, fixture.size, fixture.discard);
        const bool placed = feature(context, {0, 0, 0});
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (int z = -12; z <= 12; ++z) for (int x = -12; x <= 12; ++x) for (int y = -16; y <= 16; ++y) {
            hash ^= world.at(x, y, z) == Block::DiamondOre ? 1 : 0;
            hash *= 0x100000001b3ULL;
        }
        check(hash == fixture.hash && world.edits.size() == fixture.count && placed == (fixture.count > 0),
              "ore ellipsoid, pruning, air discard and iteration match Java mask");
        check(random.nextLong() == fixture.next, "ore placement matches Java random consumption");
    }
}

void testWeightedStructuresAndJigsaws() {
    using namespace mcworld;
    using namespace mcworld::detail;
    LegacyRandom random(123);
    std::vector<std::size_t> attempted;
    const int chosen = selectWeightedStructure({2, 3, 5}, random, [&](std::size_t candidate) {
        attempted.push_back(candidate); return attempted.size() == 3;
    });
    check(chosen >= 0 && attempted.size() == 3, "weighted structure selection retries failed candidates");
    std::sort(attempted.begin(), attempted.end());
    check(attempted == std::vector<std::size_t>{0, 1, 2}, "failed weighted candidates are removed rather than retried");
    LegacyRandom single(17), unchanged(17);
    check(selectWeightedStructure({5}, single, [](std::size_t) { return true; }) == 0
          && single.nextLong() == unchanged.nextLong(), "singleton structure set consumes no selection random");

    StructureTemplateCatalog catalog;
    StructureTemplate root;
    root.size = {3, 3, 3};
    root.blocks = {{{1, 0, 1}, Block::Bricks}};
    root.connectors = {{{2, 1, 1}, TemplateDirection::East, TemplateDirection::Up, "start", "socket", "children"}};
    StructureTemplate child;
    child.size = {3, 3, 3};
    child.blocks = {{{1, 0, 1}, Block::OakPlanks}};
    child.connectors = {{{0, 1, 1}, TemplateDirection::West, TemplateDirection::Up, "socket", "", ""}};
    catalog.templates = {{"root", root}, {"child", child}};
    catalog.pools["start"].elements = {{"root", 1, PieceProjection::Rigid}};
    catalog.pools["children"].elements = {{"child", 1, PieceProjection::Rigid}};
    catalog.starts[StructureVariant::VillagePlains] = {"start", 1, 32, 50, false};
    catalog.validate();
    LegacyRandom assemblyRandom(42), repeatedRandom(42);
    const auto pieces = assembleJigsaw(catalog, StructureVariant::VillagePlains, {0, 0, 0}, assemblyRandom, [](int, int) { return 64; });
    check(pieces.size() == 2, "jigsaw rotation, attachment and free-space allocation assemble child");
    check(pieces == assembleJigsaw(catalog, StructureVariant::VillagePlains, {}, repeatedRandom, [](int, int) { return 64; }),
          "template assembly is deterministic including blocks and junctions");
    if (pieces.size() == 2) {
        check(pieces[0].templatePiece && pieces[1].templatePiece && pieces[0].junctions.size() == 1 && pieces[1].junctions.size() == 1,
              "pool attachment stores template blocks and reciprocal junctions");
        check(pieces[0].junctions[0].sourceGroundY == 51 && pieces[1].junctions[0].sourceGroundY == 51,
              "jigsaw ground deltas feed both junction heights");
    }
    catalog.starts[StructureVariant::VillagePlains].maxDepth = 0;
    LegacyRandom zeroDepth(42);
    check(assembleJigsaw(catalog, StructureVariant::VillagePlains, {}, zeroDepth, [](int, int) { return 64; }).size() == 1,
          "zero-depth jigsaw emits only the start piece");
    catalog.starts[StructureVariant::VillagePlains].maxDepth = 1;
    catalog.pools["children"].elements = {{"", 1, PieceProjection::Rigid}};
    catalog.pools["children"].fallback = "start";
    LegacyRandom terminator(42);
    check(assembleJigsaw(catalog, StructureVariant::VillagePlains, {}, terminator, [](int, int) { return 64; }).size() == 1,
          "EmptyPoolElement terminates a connection instead of trying fallback");
    catalog.pools["children"].elements = {{"missing", 1, PieceProjection::Rigid}};
    bool rejected = false;
    try { catalog.validate(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "unknown template assets fail catalog validation");
}

void testJunctionDensityAndProcessors() {
    using namespace mcworld;
    using namespace mcworld::detail;
    StructureStart start;
    start.adjustment = TerrainAdjustment::BeardThin;
    StructurePiece piece;
    piece.bounds = {0, 50, 0, 15, 55, 15};
    piece.projection = PieceProjection::TerrainMatching;
    start.pieces = {piece};
    check(ChunkBeardifier({start}, {0, 0}).empty(), "terrain-matching pool pieces do not contribute rigid density");
    start.pieces[0].junctions = {{8, 51, 8, 0, PieceProjection::Rigid}, {-12, 51, 8, 0, PieceProjection::Rigid}};
    const ChunkBeardifier beard({start}, {0, 0});
    check(beard.sample(8, 50, 8) == structureBeardContribution(0, -1, 0, -1) * .4F,
          "junction contributes the Java 0.4 weight without a terrain-matching box");
    check(beard.sample(-12, 50, 8) == 0, "junction collection excludes exact chunk-minus-twelve boundary");
    const StructureBlock block{{-17, 60, 32}, Block::Stone};
    const std::vector<TemplateProcessor> processors{
        {TemplateProcessorKind::Rule, {Block::Stone}, {Block::Air}, Block::Bricks, 1},
        {TemplateProcessorKind::Ignore, {Block::Bricks}, {}, Block::Air, 1}};
    check(!processStructureBlock(block, Block::Air, processors), "template processors chain replacement then ignore");
    check(processStructureBlock(block, Block::Dirt, processors) == Block::Stone, "template location predicates inspect existing material");
    const std::vector<TemplateProcessor> rot{{TemplateProcessorKind::Rot, {}, {}, Block::Air, .5F}};
    check(processStructureBlock(block, Block::Air, rot) == processStructureBlock(block, Block::Dirt, rot),
          "template rot randomness is positional rather than chunk visitation dependent");
    check(biomeTemperature(Biome::WindsweptHills, 0, 64, 0) >= .15F
          && biomeTemperature(Biome::WindsweptHills, 0, 250, 0) < .15F,
          "height-adjusted temperatures allow high-elevation snow in otherwise mild biomes");
    check(!biomeHasPrecipitation(Biome::Desert), "dry biomes do not receive surface snow");
}

} // namespace

int main() {
    testStructureMetadata();
    testSpringRules();
    testBeardKernel();
    testAdjustedBounds();
    testDecoration();
    testAreaValidation();
    testFeatureSorterAndModifiers();
    testOreReferenceMasks();
    testWeightedStructuresAndJigsaws();
    testJunctionDensityAndProcessors();
    if (failures) std::cerr << failures << " generation checks failed\n";
    else std::cout << "All generation checks passed\n";
    return failures ? 1 : 0;
}
