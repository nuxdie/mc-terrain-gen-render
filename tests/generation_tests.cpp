#include "mcworld/generation.hpp"

#include <algorithm>
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
    options.terrain.biomes = std::make_shared<FixedBiome>(mcworld::Biome::Forest);
    mcworld::OverworldWorldGenerator generator(router, options);

    bool village = false;
    bool mineshaft = false;
    bool portal = false;
    mcworld::StructureStart selected;
    for (int z = -64; z <= 64; ++z) for (int x = -64; x <= 64; ++x) {
        for (const auto& start : generator.structureStarts(x, z)) {
            check(start.valid(), "stored structure starts have pieces");
            check(start.bounds().intersectsChunk(x, z), "structure starts intersect their source chunk");
            village |= start.kind == mcworld::StructureKind::Village;
            mineshaft |= start.kind == mcworld::StructureKind::Mineshaft;
            portal |= start.kind == mcworld::StructureKind::RuinedPortal;
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

    options.structures = false;
    mcworld::OverworldWorldGenerator disabled(router, options);
    check(disabled.structureStarts(0, 0).empty() && disabled.structureReferences(0, 0).empty(),
          "structure option disables starts and references");
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
    testDecoration();
    testAreaValidation();
    if (failures) std::cerr << failures << " generation checks failed\n";
    else std::cout << "All generation checks passed\n";
    return failures ? 1 : 0;
}
