#include "mcworld/terrain.hpp"
#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

void testClimate() {
    using enum mcworld::Biome;
    mcworld::RouterSample c;
    c.continentalness = -1.1F;
    check(mcworld::resolveOverworldBiome(c) == MushroomFields, "mushroom continentalness");
    c.continentalness = -.6F;
    c.temperature = -.8F;
    check(mcworld::resolveOverworldBiome(c) == DeepFrozenOcean, "cold deep ocean");
    c.temperature = -.45F;
    check(mcworld::resolveOverworldBiome(c) == DeepFrozenOcean, "shared interval boundary uses first registered point");
    c.temperature = -.4499F;
    check(mcworld::resolveOverworldBiome(c) == DeepColdOcean, "quantized temperature crosses interval boundary");
    c.continentalness = -.3F;
    c.temperature = .8F;
    check(mcworld::resolveOverworldBiome(c) == WarmOcean, "warm ocean");
    c.continentalness = .1F;
    c.erosion = .3F;
    c.ridges = -.3F;
    check(mcworld::resolveOverworldBiome(c) == Desert, "hot inland surface");
    c.temperature = 0;
    c.vegetation = .85F;
    c.depth = .5F;
    check(mcworld::resolveOverworldBiome(c) == LushCaves, "humid underground climate");
    c.vegetation = 0;
    c.erosion = -.8F;
    c.depth = 1.1F;
    check(mcworld::resolveOverworldBiome(c) == DeepDark, "deep dark climate");
    c.depth = .5F;
    c.erosion = .8F;
    c.ridges = -.95F;
    check(mcworld::resolveOverworldBiome(c) == SulfurCaves, "26.3 sulfur cave climate");
    c.temperature = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try { (void)mcworld::resolveOverworldBiome(c); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "non-finite climate rejected");
    mcworld::OverworldNoiseRouter router(12345);
    check(mcworld::sampleOverworldBiome(router, -1, -1, -1) ==
          mcworld::resolveOverworldBiome(router.sample(-4, -4, -4)), "negative coordinates floor to quart lattice");
}

void testRandomAndAquifer() {
    mcworld::detail::LegacyRandom random(0);
    check(random.nextInt(1000) == 360 && random.nextInt(1000) == 948, "Java bounded random vector");
    mcworld::detail::LegacyRandom longs(0);
    check(longs.nextLong() == static_cast<std::uint64_t>(-4962768465676381896LL), "Java signed nextLong vector");
    check(mcworld::detail::floorDiv(-1, 16) == -1 && mcworld::detail::floorDiv(-16, 16) == -1,
          "negative aquifer grid division");
    mcworld::OverworldNoiseRouter router(12345);
    mcworld::detail::Aquifer disabled(router, false), first(router, true), second(router, true);
    using enum mcworld::Block;
    check(disabled.sample(0, 62, 0, 0).block == Water, "global water below sea level");
    check(disabled.sample(0, 63, 0, 0).block == Air, "sea level is exclusive");
    check(disabled.sample(0, -55, 0, 0).block == Lava, "global lava below -54");
    check(disabled.sample(0, -54, 0, 0).block == Water, "global lava cutoff is exclusive");
    check(first.sample(0, -60, 0, 1).block == Stone, "positive density beats global lava");
    check(first.sample(0, -55, 0, -1).block == Lava, "enabled aquifer retains global lava");
    struct Probe { int x, y, z; mcworld::detail::Substance substance; };
    std::vector<Probe> probes;
    for (int x = -17; x <= 17; x += 2) for (int y = -53; y <= 65; y += 11)
        probes.push_back({x, y, -x, first.sample(x, y, -x, 0)});
    bool pressureBarrier = false;
    for (auto it = probes.rbegin(); it != probes.rend(); ++it) {
        auto result = second.sample(it->x, it->y, it->z, 0);
        check(result.block == it->substance.block && result.schedule == it->substance.schedule,
              "aquifer decisions independent of cache fill order");
        pressureBarrier |= result.block == Stone;
    }
    check(pressureBarrier, "aquifer pressure can retain solid at zero density");
}

void testChunks() {
    using enum mcworld::Block;
    mcworld::TerrainChunk empty;
    empty.primeHeightmaps();
    check(empty.worldSurface[0] == -64 && empty.oceanFloor[0] == -64, "empty heightmap sentinel");
    empty.set(0, 5, 0, Stone);
    empty.set(0, 8, 0, Water);
    empty.primeHeightmaps();
    check(empty.worldSurface[0] == 9 && empty.oceanFloor[0] == 6, "fluid and floor heightmaps differ");
    bool rejected = false;
    try { empty.set(-1, 0, 0, Stone); } catch (const std::out_of_range&) { rejected = true; }
    check(rejected, "chunk-local access checked");

    mcworld::OverworldNoiseRouter router(12345), independent(12345);
    mcworld::OverworldTerrainGenerator generator(router), other(independent);
    auto a = generator.generate(-1, 0);
    (void)generator.generate(0, 0);
    auto again = generator.generate(-1, 0);
    auto b = other.generate(-1, 0);
    check(a.blocks == b.blocks && a.blocks == again.blocks, "chunk blocks independent of generation order");
    check(a.biomes == b.biomes && a.biomes == again.biomes, "biome palettes deterministic");
    check(a.fluidPostProcessing == b.fluidPostProcessing && a.fluidPostProcessing == again.fluidPostProcessing,
          "fluid update positions deterministic");
    std::size_t deepslate = 0, fluids = 0;
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
        check(a.at(x, -64, z) == Bedrock, "bedrock floor survives carving");
        int surface = -64, floor = -64;
        for (int y = -64; y < 320; ++y) {
            auto block = a.at(x, y, z);
            if (block != Air) surface = y + 1;
            if (mcworld::isSolid(block)) floor = y + 1;
            deepslate += block == Deepslate;
            fluids += mcworld::isFluid(block);
        }
        check(a.worldSurface[z * 16 + x] == surface && a.oceanFloor[z * 16 + x] == floor,
              "heightmaps describe post-carver blocks");
    }
    check(deepslate > 0 && fluids > 0, "terrain contains underground materials and fluids");
    for (auto p : a.fluidPostProcessing) check(mcworld::isFluid(a.at(p.x, p.y, p.z)), "only existing fluids queued");
    rejected = false;
    try { (void)generator.generate(std::numeric_limits<int>::max(), 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "overflowing chunk coordinate rejected");

    mcworld::TerrainOptions options;
    options.carvers = false;
    auto before = mcworld::OverworldTerrainGenerator(router, options).generate(-1, 0);
    check(before.blocks != a.blocks, "carver mask changes terrain");
    for (std::size_t i = 0; i < a.blocks.size(); ++i) {
        if (a.blocks[i] != before.blocks[i]) {
            check(!mcworld::isSolid(a.blocks[i]) || a.blocks[i] == Grass || a.blocks[i] == Mycelium || a.blocks[i] == Dirt,
                  "carvers remove terrain or repair exposed dirt");
        }
    }
}

void testMaterials() {
    using enum mcworld::Block;
    mcworld::OverworldNoiseRouter router(91);
    auto slab = [](mcworld::Biome biome) {
        mcworld::TerrainChunk c;
        c.biomes.fill(biome);
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x)
            for (int y = -64; y <= 180; ++y) c.set(x, y, z,Stone);
        c.primeHeightmaps();
        return c;
    };
    auto desert = slab(mcworld::Biome::Desert);
    mcworld::detail::buildMaterials(desert, router, false);
    check(desert.at(8, 180, 8) == Sand, "desert surface is sand");
    check(std::count(desert.blocks.begin(), desert.blocks.end(), Sandstone) > 0, "sandstone below desert sand");
    auto plains = slab(mcworld::Biome::Plains);
    mcworld::detail::buildMaterials(plains, router, false);
    check(plains.at(8, 180, 8) == Grass && plains.at(8, 179, 8) == Dirt, "plains topsoil sequence");
    check(plains.at(8, -64, 8) == Bedrock && plains.at(8, -20, 8) == Deepslate, "bedrock and underground priority");
    auto sulfur = slab(mcworld::Biome::SulfurCaves);
    mcworld::detail::buildMaterials(sulfur, router, false);
    check(std::count(sulfur.blocks.begin(), sulfur.blocks.end(), Sulfur) > 0 &&
          std::count(sulfur.blocks.begin(), sulfur.blocks.end(), Cinnabar) > 0, "sulfur cave bands");
    std::size_t oreCount = 0;
    for (auto coordinate : std::array<std::array<int, 2>, 4>{{{0, 0}, {20, -12}, {100, 100}, {-45, 50}}}) {
        auto veins = slab(mcworld::Biome::Plains);
        veins.chunkX = coordinate[0];
        veins.chunkZ = coordinate[1];
        mcworld::detail::buildMaterials(veins, router, true);
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) for (int y = -64; y < 320; ++y) {
            auto b = veins.at(x, y, z);
            if (b == CopperOre || b == RawCopper) {
                check(y >= 0 && y < 50, "copper veins respect height range");
                ++oreCount;
            }
            if (b == DeepslateIronOre || b == RawIron) {
                check(y >= -60 && y < -8, "iron veins respect height range");
                ++oreCount;
            }
        }
        check(veins.at(0, -64, 0) == Bedrock, "bedrock has priority over veins");
    }
    check(oreCount > 0, "ore-vein material rules produce ore");
    mcworld::TerrainOptions options;
    options.biomes = std::make_shared<FixedBiome>(mcworld::Biome::Desert);
    options.carvers = false;
    auto fixed = mcworld::OverworldTerrainGenerator(router, options).generate(0, 0);
    check(std::all_of(fixed.biomes.begin(), fixed.biomes.end(), [](auto b) { return b == mcworld::Biome::Desert; }),
          "injected biome provider fills palette");
}
}

int main() {
    testClimate();
    testRandomAndAquifer();
    testChunks();
    testMaterials();
    if (failures) std::cerr << failures << " terrain checks failed\n";
    else std::cout << "All terrain checks passed\n";
    return failures ? 1 : 0;
}
