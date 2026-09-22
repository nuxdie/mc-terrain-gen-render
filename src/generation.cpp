#include "mcworld/generation.hpp"

#include "legacy_random.hpp"
#include "generation_internal.hpp"
#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace mcworld {
namespace {

using Key = std::pair<int, int>;

constexpr int kReferenceRadius = 8;
constexpr int kFeatureWriteRadius = 1;
constexpr int kDecorationSourceHalo = 1;
constexpr int kTerrainHalo = kFeatureWriteRadius + kDecorationSourceHalo;

enum class Spread { Linear, Triangular };

struct StructureDefinition {
    StructureKind kind;
    int spacing;
    int separation;
    Spread spread;
    int salt;
    float frequency;
    TerrainAdjustment adjustment;
    DecorationStep step;
};

constexpr std::array kStructures{
    StructureDefinition{StructureKind::Village, 34, 8, Spread::Linear, 10387312, 1.0F,
                        TerrainAdjustment::BeardThin, DecorationStep::SurfaceStructures},
    StructureDefinition{StructureKind::Mineshaft, 1, 0, Spread::Linear, 0, 0.004F,
                        TerrainAdjustment::None, DecorationStep::UndergroundStructures},
    StructureDefinition{StructureKind::RuinedPortal, 40, 15, Spread::Linear, 34222645, 1.0F,
                        TerrainAdjustment::None, DecorationStep::SurfaceStructures},
    StructureDefinition{StructureKind::AncientCity, 24, 8, Spread::Linear, 20083232, 1.0F,
                        TerrainAdjustment::BeardBox, DecorationStep::UndergroundDecoration},
};

[[nodiscard]] int checkedBlockOrigin(int chunk) {
    const std::int64_t value = static_cast<std::int64_t>(chunk) * TerrainChunk::width;
    if (value < std::numeric_limits<int>::min() + 1024LL ||
        value > std::numeric_limits<int>::max() - 1024LL) {
        throw std::invalid_argument("Generated area is outside the supported integer grid");
    }
    return static_cast<int>(value);
}

[[nodiscard]] bool cave(Biome biome) {
    return biome == Biome::DripstoneCaves || biome == Biome::LushCaves
        || biome == Biome::SulfurCaves || biome == Biome::DeepDark;
}

[[nodiscard]] bool snowy(Biome biome) {
    using enum Biome;
    return biome == SnowyPlains || biome == IceSpikes || biome == SnowyTaiga || biome == Grove
        || biome == SnowySlopes || biome == FrozenPeaks || biome == JaggedPeaks
        || biome == FrozenRiver || biome == SnowyBeach || biome == FrozenOcean
        || biome == DeepFrozenOcean;
}

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

[[nodiscard]] bool structureBiome(StructureKind kind, Biome biome) {
    switch (kind) {
    case StructureKind::Village:
        // Union of the five HAS_VILLAGE_* tags (including meadow).
        return biome == Biome::Plains || biome == Biome::Meadow || biome == Biome::Desert
            || biome == Biome::Savanna || biome == Biome::SnowyPlains || biome == Biome::Taiga;
    case StructureKind::Mineshaft:
        return biome != Biome::DeepDark;
    case StructureKind::RuinedPortal:
        return biome != Biome::DeepDark;
    case StructureKind::AncientCity:
        return biome == Biome::DeepDark;
    }
    return false;
}

[[nodiscard]] int spreadOffset(detail::LegacyRandom& random, int limit, Spread spread) {
    if (spread == Spread::Triangular) {
        return (random.nextInt(limit) + random.nextInt(limit)) / 2;
    }
    return random.nextInt(limit);
}

[[nodiscard]] bool isPlacementChunk(
    const StructureDefinition& definition,
    std::int64_t seed,
    int chunkX,
    int chunkZ
) {
    if (definition.spacing == 1) {
        auto random = detail::largeFeatureRandom(seed, chunkX, chunkZ);
        return random.nextDouble() < definition.frequency;
    }
    const int gridX = detail::floorDiv(chunkX, definition.spacing);
    const int gridZ = detail::floorDiv(chunkZ, definition.spacing);
    auto random = detail::largeFeatureWithSaltRandom(seed, gridX, gridZ, definition.salt);
    const int limit = definition.spacing - definition.separation;
    const int candidateX = gridX * definition.spacing + spreadOffset(random, limit, definition.spread);
    const int candidateZ = gridZ * definition.spacing + spreadOffset(random, limit, definition.spread);
    return candidateX == chunkX && candidateZ == chunkZ;
}

[[nodiscard]] int surfaceY(const OverworldNoiseRouter& router, int worldX, int worldZ) {
    return std::clamp(static_cast<int>(std::floor(router.samplePreliminarySurface(worldX, worldZ))),
                      TerrainChunk::minY + 5, TerrainChunk::maxY - 16);
}

[[nodiscard]] StructureStart makeStart(
    const StructureDefinition& definition,
    const OverworldNoiseRouter& router,
    int chunkX,
    int chunkZ
) {
    const int centerX = checkedBlockOrigin(chunkX) + 8;
    const int centerZ = checkedBlockOrigin(chunkZ) + 8;
    const int top = surfaceY(router, centerX, centerZ);
    StructureStart start{definition.kind, {chunkX, chunkZ}, definition.adjustment, definition.step, {}};

    switch (definition.kind) {
    case StructureKind::Village:
        start.pieces = {
            {{centerX - 5, top, centerZ - 5, centerX + 5, top + 5, centerZ + 5}, Block::OakPlanks, true, 0},
            {{centerX - 13, top, centerZ - 2, centerX + 13, top, centerZ + 2}, Block::Cobblestone, false, 0},
            {{centerX - 2, top, centerZ - 13, centerX + 2, top, centerZ + 13}, Block::Cobblestone, false, 0},
        };
        break;
    case StructureKind::Mineshaft: {
        auto random = detail::largeFeatureRandom(router.seed(), chunkX, chunkZ);
        const int y = -42 + random.nextInt(44);
        start.pieces = {
            {{centerX - 4, y - 2, centerZ - 4, centerX + 4, y + 3, centerZ + 4}, Block::OakPlanks, true, 0},
            {{centerX - 24, y, centerZ - 1, centerX + 24, y + 2, centerZ + 1}, Block::OakPlanks, true, 0},
            {{centerX - 1, y, centerZ - 24, centerX + 1, y + 2, centerZ + 24}, Block::OakPlanks, true, 0},
        };
        break;
    }
    case StructureKind::RuinedPortal:
        start.pieces = {
            {{centerX - 3, top, centerZ - 1, centerX + 3, top + 7, centerZ + 1}, Block::Bricks, true, 0},
            {{centerX - 5, top - 1, centerZ - 3, centerX + 5, top, centerZ + 3}, Block::MossyCobblestone, false, 0},
        };
        break;
    case StructureKind::AncientCity:
        start.pieces = {
            {{centerX - 14, -52, centerZ - 14, centerX + 14, -43, centerZ + 14}, Block::Deepslate, true, 0},
            {{centerX - 4, -43, centerZ - 4, centerX + 4, -35, centerZ + 4}, Block::Deepslate, true, 0},
        };
        break;
    }
    return start;
}

class StructureIndex {
public:
    StructureIndex(const OverworldNoiseRouter& router, const BiomeSource& biomes, bool enabled)
        : router_(router), biomes_(biomes), enabled_(enabled) {}

    [[nodiscard]] const std::vector<StructureStart>& starts(int chunkX, int chunkZ) const {
        const Key key{chunkX, chunkZ};
        if (const auto found = starts_.find(key); found != starts_.end()) {
            return found->second;
        }
        std::vector<StructureStart> result;
        if (enabled_) {
            const int centerX = checkedBlockOrigin(chunkX) + 8;
            const int centerZ = checkedBlockOrigin(chunkZ) + 8;
            for (const auto& definition : kStructures) {
                if (!isPlacementChunk(definition, router_.seed(), chunkX, chunkZ)) continue;
                const int y = definition.kind == StructureKind::AncientCity
                    ? -48 : surfaceY(router_, centerX, centerZ);
                const Biome biome = biomes_.sample(router_, centerX, y, centerZ);
                if (structureBiome(definition.kind, biome)) {
                    result.push_back(makeStart(definition, router_, chunkX, chunkZ));
                }
            }
        }
        return starts_.emplace(key, std::move(result)).first->second;
    }

    [[nodiscard]] std::vector<StructureReference> references(int targetX, int targetZ) const {
        if (targetX < std::numeric_limits<int>::min() + kReferenceRadius
            || targetX > std::numeric_limits<int>::max() - kReferenceRadius
            || targetZ < std::numeric_limits<int>::min() + kReferenceRadius
            || targetZ > std::numeric_limits<int>::max() - kReferenceRadius) {
            throw std::invalid_argument("Structure-reference neighborhood overflows chunk coordinates");
        }
        (void)checkedBlockOrigin(targetX - kReferenceRadius);
        (void)checkedBlockOrigin(targetX + kReferenceRadius);
        (void)checkedBlockOrigin(targetZ - kReferenceRadius);
        (void)checkedBlockOrigin(targetZ + kReferenceRadius);
        std::vector<StructureReference> result;
        for (int z = targetZ - kReferenceRadius; z <= targetZ + kReferenceRadius; ++z) {
            for (int x = targetX - kReferenceRadius; x <= targetX + kReferenceRadius; ++x) {
                for (const StructureStart& start : starts(x, z)) {
                    const BoundingBox box = start.bounds();
                    if (box.intersectsChunk(targetX, targetZ)) {
                        result.push_back({start.kind, start.source});
                    }
                }
            }
        }
        return result;
    }

    [[nodiscard]] const StructureStart* resolve(const StructureReference& reference) const {
        const auto& source = starts(reference.source.x, reference.source.z);
        const auto found = std::find_if(source.begin(), source.end(), [&](const StructureStart& start) {
            return start.kind == reference.kind;
        });
        return found == source.end() ? nullptr : &*found;
    }

private:
    const OverworldNoiseRouter& router_;
    const BiomeSource& biomes_;
    bool enabled_;
    mutable std::map<Key, std::vector<StructureStart>> starts_;
};

[[nodiscard]] double fastInvSqrt(double value) {
    const double half = 0.5 * value;
    std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    bits = 6910469410427058090ULL - (bits >> 1);
    value = std::bit_cast<double>(bits);
    return value * (1.5 - half * value * value);
}

[[nodiscard]] float buryContribution(float dx, float dy, float dz) {
    const float squared = dx * dx + dy * dy + dz * dz;
    return squared >= 36.0F ? 0.0F : 1.0F - std::sqrt(squared) / 6.0F;
}

} // namespace

float detail::structureBeardContribution(int dx, int dy, int dz, int yToGround) {
    if (dx < -12 || dx >= 12 || dy < -12 || dy >= 12 || dz < -12 || dz >= 12) return 0.0F;
    const double kernelY = static_cast<double>(dy) + 0.5;
    const double kernelSquared = static_cast<double>(dx) * dx + kernelY * kernelY
        + static_cast<double>(dz) * dz;
    const float kernel = static_cast<float>(std::pow(std::exp(1.0), -kernelSquared / 16.0));
    const float offsetY = static_cast<float>(yToGround) + 0.5F;
    const float squared = static_cast<float>(dx) * dx + offsetY * offsetY
        + static_cast<float>(dz) * dz;
    if (squared == 0.0F) return 0.0F;
    const float value = -offsetY * static_cast<float>(fastInvSqrt(squared / 2.0)) / 2.0F;
    return value * kernel;
}

namespace {

class ChunkBeardifier final : public Beardifier {
public:
    ChunkBeardifier(const StructureIndex& index, std::vector<StructureReference> references) {
        for (const auto& reference : references) {
            if (const StructureStart* start = index.resolve(reference);
                start != nullptr && start->adjustment != TerrainAdjustment::None) {
                for (const auto& piece : start->pieces) pieces_.push_back({piece, start->adjustment});
            }
        }
    }

    float sample(double xValue, double yValue, double zValue) const override {
        const int x = static_cast<int>(xValue);
        const int y = static_cast<int>(yValue);
        const int z = static_cast<int>(zValue);
        float result = 0.0F;
        for (const Entry& entry : pieces_) {
            const auto& box = entry.piece.bounds;
            const int dx = std::max(0, std::max(box.minX - x, x - box.maxX));
            const int dz = std::max(0, std::max(box.minZ - z, z - box.maxZ));
            const int groundY = box.minY + entry.piece.groundLevelDelta;
            const int toGround = y - groundY;
            int dy = 0;
            switch (entry.adjustment) {
            case TerrainAdjustment::None: break;
            case TerrainAdjustment::Bury:
            case TerrainAdjustment::BeardThin: dy = toGround; break;
            case TerrainAdjustment::BeardBox: dy = std::max(0, std::max(groundY - y, y - box.maxY)); break;
            case TerrainAdjustment::Encapsulate: dy = std::max(0, std::max(box.minY - y, y - box.maxY)); break;
            }
            switch (entry.adjustment) {
            case TerrainAdjustment::None: break;
            case TerrainAdjustment::Bury:
                result += buryContribution(static_cast<float>(dx), dy / 2.0F, static_cast<float>(dz));
                break;
            case TerrainAdjustment::BeardThin:
            case TerrainAdjustment::BeardBox:
                result += detail::structureBeardContribution(dx, dy, dz, toGround) * 0.8F;
                break;
            case TerrainAdjustment::Encapsulate:
                result += buryContribution(dx / 2.0F, dy / 2.0F, dz / 2.0F) * 0.8F;
                break;
            }
        }
        return result;
    }

    [[nodiscard]] bool empty() const noexcept { return pieces_.empty(); }

private:
    struct Entry { StructurePiece piece; TerrainAdjustment adjustment; };
    std::vector<Entry> pieces_;
};

void updateHeightmap(
    TerrainChunk& chunk,
    std::array<int, 256>& map,
    int localX,
    int y,
    int localZ,
    Block oldBlock,
    Block newBlock,
    bool (*qualifies)(Block)
) {
    int& height = map[localZ * TerrainChunk::width + localX];
    if (qualifies(newBlock)) {
        height = std::max(height, y + 1);
    } else if (qualifies(oldBlock) && height == y + 1) {
        height = y;
        while (height > TerrainChunk::minY && !qualifies(chunk.at(localX, height - 1, localZ))) --height;
    }
}

[[nodiscard]] bool nonAir(Block block) { return block != Block::Air; }
[[nodiscard]] bool floorBlock(Block block) { return blocksMotion(block); }
[[nodiscard]] bool motionBlock(Block block) { return blocksMotion(block) || isFluid(block); }
[[nodiscard]] bool motionNoLeaves(Block block) { return (blocksMotion(block) && !isLeaves(block)) || isFluid(block); }

class GenerationRegion {
public:
    GenerationRegion(std::map<Key, TerrainChunk>& chunks, int centerX, int centerZ, std::int64_t seed)
        : chunks_(chunks), centerX_(centerX), centerZ_(centerZ), zoomSeed_(detail::biomeZoomSeed(seed)) {}

    [[nodiscard]] Block at(int x, int y, int z) const {
        if (y < TerrainChunk::minY || y >= TerrainChunk::maxY) return Block::Air;
        const auto [chunk, localX, localZ] = locate(x, z);
        return chunk->at(localX, y, localZ);
    }

    [[nodiscard]] Biome biomeAt(int x, int y, int z) const {
        const auto quart = detail::zoomedBiomeQuart(zoomSeed_, x, y, z);
        const auto [chunk, localX, localZ] = locate(quart.x * 4, quart.z * 4);
        return chunk->biomeAt(localX, std::clamp(quart.y * 4, TerrainChunk::minY, TerrainChunk::maxY - 1), localZ);
    }

    [[nodiscard]] int worldSurface(int x, int z) const {
        const auto [chunk, localX, localZ] = locate(x, z);
        return chunk->worldSurface[localZ * 16 + localX];
    }

    [[nodiscard]] int oceanFloor(int x, int z) const {
        const auto [chunk, localX, localZ] = locate(x, z);
        return chunk->oceanFloor[localZ * 16 + localX];
    }

    [[nodiscard]] int motionBlocking(int x, int z) const {
        const auto [chunk, localX, localZ] = locate(x, z);
        return chunk->motionBlocking[localZ * 16 + localX];
    }

    void set(int x, int y, int z, Block block) {
        if (y < TerrainChunk::minY || y >= TerrainChunk::maxY) return;
        const int chunkX = detail::floorDiv(x, 16);
        const int chunkZ = detail::floorDiv(z, 16);
        if (std::max(std::abs(chunkX - centerX_), std::abs(chunkZ - centerZ_)) > kFeatureWriteRadius) {
            throw std::out_of_range("Feature write left its 3x3 WorldGenRegion");
        }
        auto [chunk, localX, localZ] = locate(x, z);
        const Block old = chunk->at(localX, y, localZ);
        if (old == block) return;
        chunk->set(localX, y, localZ, block);
        if (isFluid(block)) chunk->fluidPostProcessing.push_back({localX, y, localZ});
        updateHeightmap(*chunk, chunk->worldSurface, localX, y, localZ, old, block, nonAir);
        updateHeightmap(*chunk, chunk->oceanFloor, localX, y, localZ, old, block, floorBlock);
        updateHeightmap(*chunk, chunk->motionBlocking, localX, y, localZ, old, block, motionBlock);
        updateHeightmap(*chunk, chunk->motionBlockingNoLeaves, localX, y, localZ, old, block, motionNoLeaves);
    }

private:
    struct Located { TerrainChunk* chunk; int localX; int localZ; };
    struct ConstLocated { const TerrainChunk* chunk; int localX; int localZ; };

    [[nodiscard]] Located locate(int x, int z) {
        const int chunkX = detail::floorDiv(x, 16);
        const int chunkZ = detail::floorDiv(z, 16);
        auto found = chunks_.find({chunkX, chunkZ});
        if (found == chunks_.end()) throw std::out_of_range("WorldGenRegion chunk is unavailable");
        return {&found->second, x - chunkX * 16, z - chunkZ * 16};
    }

    [[nodiscard]] ConstLocated locate(int x, int z) const {
        const int chunkX = detail::floorDiv(x, 16);
        const int chunkZ = detail::floorDiv(z, 16);
        auto found = chunks_.find({chunkX, chunkZ});
        if (found == chunks_.end()) throw std::out_of_range("WorldGenRegion chunk is unavailable");
        return {&found->second, x - chunkX * 16, z - chunkZ * 16};
    }

    std::map<Key, TerrainChunk>& chunks_;
    int centerX_;
    int centerZ_;
    std::uint64_t zoomSeed_;
};

void placePiece(GenerationRegion& region, const StructurePiece& piece, int centerX, int centerZ) {
    const int minX = std::max(piece.bounds.minX, checkedBlockOrigin(centerX));
    const int minZ = std::max(piece.bounds.minZ, checkedBlockOrigin(centerZ));
    const int maxX = std::min(piece.bounds.maxX, checkedBlockOrigin(centerX) + 15);
    const int maxZ = std::min(piece.bounds.maxZ, checkedBlockOrigin(centerZ) + 15);
    if (minX > maxX || minZ > maxZ) return;
    for (int z = minZ; z <= maxZ; ++z) for (int x = minX; x <= maxX; ++x) {
        for (int y = std::max(piece.bounds.minY, TerrainChunk::minY + 1);
             y <= std::min(piece.bounds.maxY, TerrainChunk::maxY - 1); ++y) {
            const bool shell = x == piece.bounds.minX || x == piece.bounds.maxX
                || y == piece.bounds.minY || y == piece.bounds.maxY
                || z == piece.bounds.minZ || z == piece.bounds.maxZ;
            if (!piece.hollow || shell) region.set(x, y, z, piece.block);
            else if (region.at(x, y, z) != Block::Bedrock) region.set(x, y, z, Block::Air);
        }
    }
}

enum class Feature : std::uint8_t {
    ClayDisk,
    CoalOre,
    IronOre,
    GoldOre,
    RedstoneOre,
    DiamondOre,
    WaterSpring,
    Trees,
    FreezeTopLayer,
    Count,
};

struct FeatureDefinition { Feature feature; DecorationStep step; };
constexpr std::array kFeatures{
    FeatureDefinition{Feature::ClayDisk, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::CoalOre, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::IronOre, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::GoldOre, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::RedstoneOre, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::DiamondOre, DecorationStep::UndergroundOres},
    FeatureDefinition{Feature::WaterSpring, DecorationStep::FluidSprings},
    FeatureDefinition{Feature::Trees, DecorationStep::VegetalDecoration},
    FeatureDefinition{Feature::FreezeTopLayer, DecorationStep::TopLayerModification},
};

[[nodiscard]] bool biomeHasFeature(Biome biome, Feature feature) {
    switch (feature) {
    case Feature::ClayDisk: return !cave(biome) && biome != Biome::DeepDark;
    case Feature::CoalOre:
    case Feature::IronOre:
    case Feature::GoldOre:
    case Feature::RedstoneOre:
    case Feature::DiamondOre: return true;
    case Feature::WaterSpring: return biome != Biome::DeepDark;
    case Feature::Trees: return treeBiome(biome);
    case Feature::FreezeTopLayer: return snowy(biome);
    case Feature::Count: return false;
    }
    return false;
}

void oreBlob(GenerationRegion& region, detail::WorldgenRandom& random, int centerX, int centerZ,
             Block ore, int count, int minY, int maxY, int size) {
    const int originX = checkedBlockOrigin(centerX);
    const int originZ = checkedBlockOrigin(centerZ);
    for (int attempt = 0; attempt < count; ++attempt) {
        int x = originX + random.nextInt(16);
        int y = minY + random.nextInt(maxY - minY + 1);
        int z = originZ + random.nextInt(16);
        for (int block = 0; block < size; ++block) {
            const Block existing = region.at(x, y, z);
            if (existing == Block::Stone || existing == Block::Deepslate || existing == Block::Tuff
                || existing == Block::Granite) {
                region.set(x, y, z, ore);
            }
            x += random.nextInt(3) - 1;
            y += random.nextInt(3) - 1;
            z += random.nextInt(3) - 1;
        }
    }
}

void placeClayDisks(GenerationRegion& region, detail::WorldgenRandom& random, int centerX, int centerZ) {
    const int originX = checkedBlockOrigin(centerX);
    const int originZ = checkedBlockOrigin(centerZ);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int x = originX + random.nextInt(16);
        const int z = originZ + random.nextInt(16);
        const int surface = region.worldSurface(x, z);
        if (surface <= TerrainChunk::minY || region.at(x, surface - 1, z) != Block::Water) continue;
        const int floor = region.oceanFloor(x, z) - 1;
        if (!biomeHasFeature(region.biomeAt(x, floor + 1, z), Feature::ClayDisk)) continue;
        const int radius = 2 + random.nextInt(3);
        for (int dz = -radius; dz <= radius; ++dz) for (int dx = -radius; dx <= radius; ++dx) {
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

void placeSprings(GenerationRegion& region, detail::WorldgenRandom& random, int centerX, int centerZ) {
    const int originX = checkedBlockOrigin(centerX);
    const int originZ = checkedBlockOrigin(centerZ);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int x = originX + random.nextInt(16);
        const int y = TerrainChunk::minY + 8 + random.nextInt(120);
        const int z = originZ + random.nextInt(16);
        if (!biomeHasFeature(region.biomeAt(x, y, z), Feature::WaterSpring)) continue;
        if (detail::canPlaceWaterSpring(region.at(x, y, z), region.at(x, y + 1, z),
                region.at(x, y - 1, z), {region.at(x - 1, y, z), region.at(x + 1, y, z),
                                      region.at(x, y, z - 1), region.at(x, y, z + 1)})) {
            region.set(x, y, z, Block::Water);
        }
    }
}

void placeTrees(GenerationRegion& region, detail::WorldgenRandom& random, int centerX, int centerZ) {
    const int originX = checkedBlockOrigin(centerX);
    const int originZ = checkedBlockOrigin(centerZ);
    const int attempts = 2 + random.nextInt(4);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const int x = originX + random.nextInt(16);
        const int z = originZ + random.nextInt(16);
        const int y = region.worldSurface(x, z);
        if (y < TerrainChunk::minY + 1 || y + 8 >= TerrainChunk::maxY) continue;
        if (!biomeHasFeature(region.biomeAt(x, y, z), Feature::Trees)) continue;
        const Block ground = region.at(x, y - 1, z);
        if (ground != Block::Grass && ground != Block::Dirt && ground != Block::Podzol) continue;
        const int height = 4 + random.nextInt(3);
        bool clear = true;
        for (int trunkY = y; trunkY <= y + height; ++trunkY) {
            clear &= region.at(x, trunkY, z) == Block::Air || isLeaves(region.at(x, trunkY, z));
        }
        if (!clear) continue;
        for (int trunkY = y; trunkY < y + height; ++trunkY) region.set(x, trunkY, z, Block::OakLog);
        for (int dy = -2; dy <= 1; ++dy) {
            const int radius = dy == 1 ? 1 : 2;
            for (int dz = -radius; dz <= radius; ++dz) for (int dx = -radius; dx <= radius; ++dx) {
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

void freezeTopLayer(GenerationRegion& region, int centerX, int centerZ) {
    const int originX = checkedBlockOrigin(centerX);
    const int originZ = checkedBlockOrigin(centerZ);
    for (int x = originX; x < originX + 16; ++x) for (int z = originZ; z < originZ + 16; ++z) {
        const int y = region.motionBlocking(x, z);
        if (!snowy(region.biomeAt(x, y, z))) continue;
        if (y > TerrainChunk::minY && region.at(x, y - 1, z) == Block::Water) {
            region.set(x, y - 1, z, Block::Ice);
        }
        if (y < TerrainChunk::maxY && region.at(x, y, z) == Block::Air
            && blocksMotion(region.at(x, y - 1, z))) {
            region.set(x, y, z, Block::Snow);
        }
    }
}

void placeFeature(Feature feature, GenerationRegion& region, detail::WorldgenRandom& random,
                  int chunkX, int chunkZ) {
    switch (feature) {
    case Feature::ClayDisk: placeClayDisks(region, random, chunkX, chunkZ); break;
    case Feature::CoalOre: oreBlob(region, random, chunkX, chunkZ, Block::CoalOre, 16, 0, 192, 12); break;
    case Feature::IronOre: oreBlob(region, random, chunkX, chunkZ, Block::IronOre, 12, -56, 96, 9); break;
    case Feature::GoldOre: oreBlob(region, random, chunkX, chunkZ, Block::GoldOre, 4, -56, 32, 8); break;
    case Feature::RedstoneOre: oreBlob(region, random, chunkX, chunkZ, Block::RedstoneOre, 8, -63, 16, 8); break;
    case Feature::DiamondOre: oreBlob(region, random, chunkX, chunkZ, Block::DiamondOre, 4, -63, 16, 6); break;
    case Feature::WaterSpring: placeSprings(region, random, chunkX, chunkZ); break;
    case Feature::Trees: placeTrees(region, random, chunkX, chunkZ); break;
    case Feature::FreezeTopLayer: freezeTopLayer(region, chunkX, chunkZ); break;
    case Feature::Count: break;
    }
}

[[nodiscard]] std::array<bool, static_cast<std::size_t>(Biome::DeepDark) + 1> collectBiomes(
    const std::map<Key, TerrainChunk>& chunks,
    int centerX,
    int centerZ
) {
    std::array<bool, static_cast<std::size_t>(Biome::DeepDark) + 1> result{};
    for (int z = centerZ - 1; z <= centerZ + 1; ++z) for (int x = centerX - 1; x <= centerX + 1; ++x) {
        const auto found = chunks.find({x, z});
        if (found == chunks.end()) continue;
        for (Biome biome : found->second.biomes) result[static_cast<std::size_t>(biome)] = true;
    }
    return result;
}

[[nodiscard]] bool anyBiomeSupports(
    const std::array<bool, static_cast<std::size_t>(Biome::DeepDark) + 1>& biomes,
    Feature feature
) {
    for (std::size_t i = 0; i < biomes.size(); ++i) {
        if (biomes[i] && biomeHasFeature(static_cast<Biome>(i), feature)) return true;
    }
    return false;
}

void decorateChunk(
    std::map<Key, TerrainChunk>& chunks,
    const StructureIndex& structures,
    std::int64_t seed,
    int chunkX,
    int chunkZ
) {
    GenerationRegion region(chunks, chunkX, chunkZ, seed);
    const auto references = structures.references(chunkX, chunkZ);
    const auto actualBiomes = collectBiomes(chunks, chunkX, chunkZ);
    detail::WorldgenRandom random(0);
    const std::uint64_t decorationSeed = random.setDecorationSeed(
        seed, checkedBlockOrigin(chunkX), checkedBlockOrigin(chunkZ));

    constexpr int stepCount = static_cast<int>(DecorationStep::TopLayerModification) + 1;
    for (int step = 0; step < stepCount; ++step) {
        int structureIndex = 0;
        for (const auto& definition : kStructures) {
            if (static_cast<int>(definition.step) != step) continue;
            random.setFeatureSeed(decorationSeed, structureIndex++, step);
            for (const auto& reference : references) {
                if (reference.kind != definition.kind) continue;
                if (const StructureStart* start = structures.resolve(reference)) {
                    for (const auto& piece : start->pieces) placePiece(region, piece, chunkX, chunkZ);
                }
            }
        }

        int featureIndex = 0;
        for (const auto& definition : kFeatures) {
            if (static_cast<int>(definition.step) != step) continue;
            if (anyBiomeSupports(actualBiomes, definition.feature)) {
                random.setFeatureSeed(decorationSeed, featureIndex, step);
                placeFeature(definition.feature, region, random, chunkX, chunkZ);
            }
            ++featureIndex;
        }
    }
}

} // namespace

bool BoundingBox::intersectsChunk(int chunkX, int chunkZ) const noexcept {
    const std::int64_t minChunkX = static_cast<std::int64_t>(chunkX) * 16;
    const std::int64_t minChunkZ = static_cast<std::int64_t>(chunkZ) * 16;
    return maxX >= minChunkX && minX <= minChunkX + 15
        && maxZ >= minChunkZ && minZ <= minChunkZ + 15;
}

BoundingBox BoundingBox::inflated(int amount) const {
    if (amount < 0) throw std::invalid_argument("Bounding-box inflation must be non-negative");
    const auto checked = [](std::int64_t value) {
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw std::overflow_error("Inflated bounding box exceeds integer coordinates");
        }
        return static_cast<int>(value);
    };
    return {
        checked(static_cast<std::int64_t>(minX) - amount),
        checked(static_cast<std::int64_t>(minY) - amount),
        checked(static_cast<std::int64_t>(minZ) - amount),
        checked(static_cast<std::int64_t>(maxX) + amount),
        checked(static_cast<std::int64_t>(maxY) + amount),
        checked(static_cast<std::int64_t>(maxZ) + amount),
    };
}

BoundingBox StructureStart::bounds() const {
    if (pieces.empty()) throw std::logic_error("Invalid structure start has no bounds");
    BoundingBox result = pieces.front().bounds;
    for (const auto& piece : pieces) {
        result.minX = std::min(result.minX, piece.bounds.minX);
        result.minY = std::min(result.minY, piece.bounds.minY);
        result.minZ = std::min(result.minZ, piece.bounds.minZ);
        result.maxX = std::max(result.maxX, piece.bounds.maxX);
        result.maxY = std::max(result.maxY, piece.bounds.maxY);
        result.maxZ = std::max(result.maxZ, piece.bounds.maxZ);
    }
    return adjustment == TerrainAdjustment::None ? result : result.inflated(12);
}

const GeneratedChunk& GeneratedArea::at(int chunkX, int chunkZ) const {
    const std::int64_t lastX = static_cast<std::int64_t>(firstChunkX) + width;
    const std::int64_t lastZ = static_cast<std::int64_t>(firstChunkZ) + depth;
    if (chunkX < firstChunkX || static_cast<std::int64_t>(chunkX) >= lastX
        || chunkZ < firstChunkZ || static_cast<std::int64_t>(chunkZ) >= lastZ) {
        throw std::out_of_range("Chunk is outside generated area");
    }
    const std::size_t row = static_cast<std::size_t>(chunkZ - firstChunkZ);
    const std::size_t column = static_cast<std::size_t>(chunkX - firstChunkX);
    return chunks.at(row * static_cast<std::size_t>(width) + column);
}

GeneratedChunk& GeneratedArea::at(int chunkX, int chunkZ) {
    return const_cast<GeneratedChunk&>(std::as_const(*this).at(chunkX, chunkZ));
}

class OverworldWorldGenerator::Impl {
public:
    Impl(const OverworldNoiseRouter& router, GenerationOptions options)
        : router_(router), options_(std::move(options)) {
        if (!options_.terrain.biomes) options_.terrain.biomes = std::make_shared<BiomeSource>();
    }

    GeneratedArea generateArea(int firstX, int firstZ, int width, int depth) {
        if (width <= 0 || depth <= 0) throw std::invalid_argument("Generated area dimensions must be positive");
        const std::int64_t lastX = static_cast<std::int64_t>(firstX) + width - 1;
        const std::int64_t lastZ = static_cast<std::int64_t>(firstZ) + depth - 1;
        if (lastX > std::numeric_limits<int>::max() || lastZ > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("Generated area dimensions overflow chunk coordinates");
        }
        const int terrainHalo = options_.features ? kTerrainHalo : 0;
        const int totalHalo = terrainHalo + kReferenceRadius;
        if (firstX < std::numeric_limits<int>::min() + totalHalo
            || firstZ < std::numeric_limits<int>::min() + totalHalo
            || lastX > std::numeric_limits<int>::max() - totalHalo
            || lastZ > std::numeric_limits<int>::max() - totalHalo) {
            throw std::invalid_argument("Generated area halo overflows chunk coordinates");
        }
        (void)checkedBlockOrigin(firstX - totalHalo);
        (void)checkedBlockOrigin(firstZ - totalHalo);
        (void)checkedBlockOrigin(static_cast<int>(lastX) + totalHalo);
        (void)checkedBlockOrigin(static_cast<int>(lastZ) + totalHalo);

        StructureIndex structures(router_, *options_.terrain.biomes, options_.structures);
        OverworldTerrainGenerator terrain(router_, options_.terrain);
        std::map<Key, TerrainChunk> chunks;
        for (int z = firstZ - terrainHalo; z <= lastZ + terrainHalo; ++z) {
            for (int x = firstX - terrainHalo; x <= lastX + terrainHalo; ++x) {
                const auto references = structures.references(x, z);
                ChunkBeardifier beardifier(structures, references);
                if (beardifier.empty()) chunks.emplace(Key{x, z}, terrain.generate(x, z));
                else chunks.emplace(Key{x, z}, terrain.generate(x, z, beardifier));
            }
        }

        if (options_.features) {
            for (int z = firstZ - kDecorationSourceHalo; z <= lastZ + kDecorationSourceHalo; ++z) {
                for (int x = firstX - kDecorationSourceHalo; x <= lastX + kDecorationSourceHalo; ++x) {
                    decorateChunk(chunks, structures, router_.seed(), x, z);
                }
            }
        }

        GeneratedArea result;
        result.firstChunkX = firstX;
        result.firstChunkZ = firstZ;
        result.width = width;
        result.depth = depth;
        result.chunks.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(depth));
        for (int z = firstZ; z <= lastZ; ++z) for (int x = firstX; x <= lastX; ++x) {
            GeneratedChunk chunk;
            chunk.terrain = std::move(chunks.at({x, z}));
            auto& updates = chunk.terrain.fluidPostProcessing;
            std::erase_if(updates, [&](BlockPosition position) {
                return !isFluid(chunk.terrain.at(position.x, position.y, position.z));
            });
            std::sort(updates.begin(), updates.end(), [](BlockPosition left, BlockPosition right) {
                return std::tie(left.z, left.x, left.y) < std::tie(right.z, right.x, right.y);
            });
            updates.erase(std::unique(updates.begin(), updates.end()), updates.end());
            chunk.starts = structures.starts(x, z);
            chunk.references = structures.references(x, z);
            result.chunks.push_back(std::move(chunk));
        }
        return result;
    }

    std::vector<StructureStart> starts(int x, int z) const {
        StructureIndex structures(router_, *options_.terrain.biomes, options_.structures);
        return structures.starts(x, z);
    }

    std::vector<StructureReference> references(int x, int z) const {
        StructureIndex structures(router_, *options_.terrain.biomes, options_.structures);
        return structures.references(x, z);
    }

private:
    const OverworldNoiseRouter& router_;
    GenerationOptions options_;
};

OverworldWorldGenerator::OverworldWorldGenerator(const OverworldNoiseRouter& router, GenerationOptions options)
    : impl_(std::make_unique<Impl>(router, std::move(options))) {}
OverworldWorldGenerator::~OverworldWorldGenerator() = default;
OverworldWorldGenerator::OverworldWorldGenerator(OverworldWorldGenerator&&) noexcept = default;
OverworldWorldGenerator& OverworldWorldGenerator::operator=(OverworldWorldGenerator&&) noexcept = default;

GeneratedArea OverworldWorldGenerator::generateArea(int firstChunkX, int firstChunkZ, int width, int depth) {
    return impl_->generateArea(firstChunkX, firstChunkZ, width, depth);
}

GeneratedChunk OverworldWorldGenerator::generate(int chunkX, int chunkZ) {
    GeneratedArea area = generateArea(chunkX, chunkZ, 1, 1);
    return std::move(area.chunks.front());
}

std::vector<StructureStart> OverworldWorldGenerator::structureStarts(int chunkX, int chunkZ) const {
    return impl_->starts(chunkX, chunkZ);
}

std::vector<StructureReference> OverworldWorldGenerator::structureReferences(int chunkX, int chunkZ) const {
    return impl_->references(chunkX, chunkZ);
}

} // namespace mcworld
