// Stage 5 of `minecraft-26.3-worldgen.dot`: STRUCTURE_STARTS,
// STRUCTURE_REFERENCES, and the terrain adaptation those starts feed back into
// stage 7B.
//
// The stage answers three questions, in this order:
//
//   1. does a chunk start a structure?   `isPlacementChunk`
//   2. which variant, and what does it
//      look like?                        `tryVariant` -> `makeStart`
//                                        or `detail::assembleJigsaw`
//   3. which chunks does it reach into?  `StructureIndex::references`
//
// A variant has two possible shapes. With a template catalog loaded it is a
// real jigsaw assembly (src/structure_templates.cpp); without one - or for a
// variant the catalog does not cover - it is `makeStart`'s placeholder
// footprint, which has the right placement and the right terrain adaptation
// but not the right contents. See README for that boundary.
//
// Everything is a pure function of the seed and the chunk position, which is
// what lets `StructureIndex` memoize and lets neighbours be generated in any
// order.

#include "generation_internal.hpp"
#include "mcworld/structure_templates.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace mcworld {
namespace {

// Java's `RandomSpreadType`. Only `Linear` occurs in the catalog below;
// `Triangular` is kept because the placement it belongs to is vanilla API and
// selecting it must not silently fall back to a different spread.
enum class Spread { Linear, Triangular };

// One entry of Java's `StructureSet` placement plus the parts of the structure
// itself that stage 5 needs.
//
// `spacing`/`separation`/`salt`/`spread` are `RandomSpreadStructurePlacement`;
// `spacing == 1` instead selects the per-chunk `frequency` roll that mineshafts
// use. `salt` and the field values are part of the world: changing one moves
// every structure of that kind.
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

// Registration order fixes each structure's index within its decoration step,
// which feeds its feature seed in stage 8. Reordering this table rewrites
// worlds.
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

// Stage 8 iterates structures by `StructureKind` rather than through this
// table, so the two orders have to agree; see `detail::structureStep`.
static_assert(kStructures.size() == kStructureKindCount);
static_assert([] {
    for (std::size_t i = 0; i < kStructures.size(); ++i) {
        if (kStructures[i].kind != static_cast<StructureKind>(i)) return false;
    }
    return true;
}(), "kStructures must be in StructureKind order");

// --- Placement -------------------------------------------------------------

[[nodiscard]] int spreadOffset(detail::LegacyRandom& random, int limit, Spread spread) {
    if (spread == Spread::Triangular) {
        return (random.nextInt(limit) + random.nextInt(limit)) / 2;
    }
    return random.nextInt(limit);
}

// Java: `StructurePlacement.isStructureChunk`.
[[nodiscard]] bool isPlacementChunk(
    const StructureDefinition& definition,
    std::int64_t seed,
    ChunkPosition chunk
) {
    if (definition.spacing == 1) {
        // `RandomSpreadStructurePlacement` degenerates to one roll per chunk.
        auto random = detail::largeFeatureRandom(seed, chunk.x, chunk.z);
        return random.nextDouble() < definition.frequency;
    }
    // One candidate per `spacing x spacing` grid cell, offset within the cell
    // by at most `spacing - separation`, so two structures of a kind are never
    // closer than `separation` chunks.
    const int gridX = detail::floorDiv(chunk.x, definition.spacing);
    const int gridZ = detail::floorDiv(chunk.z, definition.spacing);
    auto random = detail::largeFeatureWithSaltRandom(seed, gridX, gridZ, definition.salt);
    const int limit = definition.spacing - definition.separation;
    const int candidateX = gridX * definition.spacing + spreadOffset(random, limit, definition.spread);
    const int candidateZ = gridZ * definition.spacing + spreadOffset(random, limit, definition.spread);
    return candidateX == chunk.x && candidateZ == chunk.z;
}

// --- Biome eligibility -----------------------------------------------------

// The biome families structure placement is written against. Each is a plain
// membership test on the registered biome list rather than a tag lookup, so
// adding a biome to `Biome` means revisiting the ones it belongs to here.

[[nodiscard]] bool isBadlands(Biome biome) {
    return biome == Biome::Badlands || biome == Biome::ErodedBadlands || biome == Biome::WoodedBadlands;
}

// The oceans are one contiguous run in registration order, which is what makes
// this a range test. The assert is the thing that stops a future insertion
// into `Biome` from silently widening it.
[[nodiscard]] bool isOcean(Biome biome) {
    static_assert(static_cast<int>(Biome::WarmOcean) - static_cast<int>(Biome::Ocean) == 8
        && static_cast<int>(Biome::DripstoneCaves) == static_cast<int>(Biome::WarmOcean) + 1,
        "isOcean assumes Ocean..WarmOcean are contiguous and end the ocean run");
    return biome >= Biome::Ocean && biome <= Biome::WarmOcean;
}

[[nodiscard]] bool isJungle(Biome biome) {
    return biome == Biome::Jungle || biome == Biome::SparseJungle || biome == Biome::BambooJungle;
}

[[nodiscard]] bool isSwamp(Biome biome) {
    return biome == Biome::Swamp || biome == Biome::MangroveSwamp;
}

// Java's `#minecraft:is_mountain` as the ruined portal uses it, which also
// takes in the badlands and the windswept biomes.
[[nodiscard]] bool isMountainous(Biome biome) {
    using enum Biome;
    if (isBadlands(biome)) return true;
    switch (biome) {
    case WindsweptHills:
    case WindsweptForest:
    case WindsweptGravellyHills:
    case SavannaPlateau:
    case WindsweptSavanna:
    case StonyShore:
    case Meadow:
    case CherryGrove:
    case SnowySlopes:
    case FrozenPeaks:
    case JaggedPeaks:
    case StonyPeaks:
        return true;
    default:
        return false;
    }
}

// Whether a variant is allowed to generate in a biome. `Generic` and
// `PortalNether` never are: the first is the unset value, the second belongs
// to a dimension this port does not generate.
[[nodiscard]] bool variantBiome(StructureVariant variant, Biome biome) {
    using enum StructureVariant;
    switch (variant) {
    case VillagePlains: return biome == Biome::Plains || biome == Biome::Meadow;
    case VillageDesert: return biome == Biome::Desert;
    case VillageSavanna: return biome == Biome::Savanna;
    case VillageSnowy: return biome == Biome::SnowyPlains;
    case VillageTaiga: return biome == Biome::Taiga;
    case Mineshaft: return !isBadlands(biome) && biome != Biome::DeepDark;
    case MineshaftMesa: return isBadlands(biome);
    case PortalDesert: return biome == Biome::Desert;
    case PortalJungle: return isJungle(biome);
    case PortalSwamp: return isSwamp(biome);
    case PortalOcean: return isOcean(biome);
    case PortalMountain: return isMountainous(biome);
    // The standard portal is the leftover: everywhere the specialized portals
    // above do not claim.
    case PortalStandard:
        return !isOcean(biome) && !isMountainous(biome) && !isJungle(biome) && !isSwamp(biome)
            && biome != Biome::Desert && biome != Biome::DeepDark;
    case PortalNether: return false;
    case AncientCity: return biome == Biome::DeepDark;
    case Generic: return false;
    }
    return false;
}

// --- Piece lists -----------------------------------------------------------

// Surface height a start is placed at, clamped away from both build limits so
// that pieces stacked on top of it stay inside the world.
[[nodiscard]] int surfaceY(const OverworldNoiseRouter& router, int worldX, int worldZ) {
    return std::clamp(static_cast<int>(std::floor(router.samplePreliminarySurface(worldX, worldZ))),
                      TerrainChunk::minY + 5, TerrainChunk::maxY - 16);
}

// The Y an ancient city is carved out at, well below the surface.
constexpr int kAncientCityY = -48;

// Java: `Structure.generate` down to the piece list. The pieces are footprints
// rather than template assemblies, chosen so that placement, terrain
// adaptation and reference bounds behave like the real structure.
[[nodiscard]] StructureStart makeStart(
    const StructureDefinition& definition,
    const OverworldNoiseRouter& router,
    ChunkPosition chunk
) {
    const auto [centerX, centerZ] = detail::middleBlock(chunk);
    const int top = surfaceY(router, centerX, centerZ);
    StructureStart start{definition.kind, chunk, definition.adjustment, definition.step, {}};

    switch (definition.kind) {
    case StructureKind::Village:
        // A hollow hut plus two crossing roads.
        start.pieces = {
            {{centerX - 5, top, centerZ - 5, centerX + 5, top + 5, centerZ + 5}, Block::OakPlanks, true, 0},
            {{centerX - 13, top, centerZ - 2, centerX + 13, top, centerZ + 2}, Block::Cobblestone, false, 0},
            {{centerX - 2, top, centerZ - 13, centerX + 2, top, centerZ + 13}, Block::Cobblestone, false, 0},
        };
        break;
    case StructureKind::Mineshaft: {
        // Mineshafts pick their own depth rather than following the surface.
        auto random = detail::largeFeatureRandom(router.seed(), chunk.x, chunk.z);
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
            {{centerX - 14, kAncientCityY - 4, centerZ - 14, centerX + 14, kAncientCityY + 5, centerZ + 14},
             Block::Deepslate, true, 0},
            {{centerX - 4, kAncientCityY + 5, centerZ - 4, centerX + 4, kAncientCityY + 13, centerZ + 4},
             Block::Deepslate, true, 0},
        };
        break;
    }
    return start;
}

// --- Start selection -------------------------------------------------------

// The context a start attempt shares across every variant of one kind, so that
// the per-variant function below stays a readable signature.
struct StartAttempt {
    const OverworldNoiseRouter& router;
    const BiomeSource& biomes;
    const StructureTemplateCatalog* templates;
    const StructureDefinition& definition;
    ChunkPosition chunk;
    // The biome at the kind's own generation point, and the Y it was read at.
    Biome biome;
    int generationY;
};

// Java: `Structure.generate` for one candidate variant. `std::nullopt` means
// "not this variant here", which is what makes the weighted selection drop it
// and try the next one.
[[nodiscard]] std::optional<StructureStart> tryVariant(const StartAttempt& attempt, StructureVariant variant) {
    const bool fromTemplates = attempt.templates != nullptr && attempt.templates->starts.contains(variant);
    // A procedural variant is accepted or rejected on the kind's generation
    // point. A template one cannot be: the jigsaw picks its own point, so the
    // biome is checked below, once that point is known.
    if (!fromTemplates && !variantBiome(variant, attempt.biome)) return std::nullopt;

    if (!fromTemplates) {
        StructureStart start = makeStart(attempt.definition, attempt.router, attempt.chunk);
        start.variant = variant;
        if (!start.valid()) return std::nullopt;
        return start;
    }

    StructureStart start{attempt.definition.kind, attempt.chunk, attempt.definition.adjustment,
                         attempt.definition.step, {}};
    // A stream of its own, seeded from the chunk rather than continued from
    // the variant selection, so that the assembled shape does not depend on
    // how many variants were rejected before this one.
    auto random = detail::largeFeatureRandom(attempt.router.seed(), attempt.chunk.x, attempt.chunk.z);
    const BlockPosition origin{detail::chunkMinBlock(attempt.chunk.x), attempt.generationY,
                               detail::chunkMinBlock(attempt.chunk.z)};
    BlockPosition generationPoint{};
    start.pieces = detail::assembleJigsaw(*attempt.templates, variant, origin, random,
        [&](int x, int z) { return surfaceY(attempt.router, x, z); }, &generationPoint);
    if (!start.valid()) return std::nullopt;

    const Biome placed = attempt.biomes.sample(attempt.router, generationPoint.x, generationPoint.y, generationPoint.z);
    if (!variantBiome(variant, placed)) return std::nullopt;
    start.variant = variant;
    return start;
}

// --- Beard kernel ----------------------------------------------------------

// Java: `Mth.fastInvSqrt`. A double-precision Quake inverse square root: one
// bit-twiddling estimate, then one Newton step. The constant is Java's own
// 6910469410427058090 (0x5fe6eb50c7b537aa), which is one more than the
// familiar 0x5fe6eb50c7b537a9 - do not "correct" it.
//
// Beardified density depends on the exact result, so this cannot become
// `1.0 / std::sqrt(value)`.
[[nodiscard]] double fastInvSqrt(double value) {
    const double half = 0.5 * value;
    std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    bits = 6910469410427058090ULL - (bits >> 1);
    value = std::bit_cast<double>(bits);
    return value * (1.5 - half * value * value);
}

// Java: the BURY branch of `Beardifier.compute`. A linear falloff over six
// blocks, which pushes terrain up to swallow the structure.
[[nodiscard]] float buryContribution(float dx, float dy, float dz) {
    const float squared = dx * dx + dy * dy + dz * dz;
    return squared >= 36.0F ? 0.0F : 1.0F - std::sqrt(squared) / 6.0F;
}

// Java's BEARD_KERNEL is scaled by 0.8 where it is applied.
constexpr float kBeardScale = 0.8F;

// Reach of the beard kernel in every axis. The interval is the asymmetric
// [-12, 12) because Java indexes a 24-wide precomputed table.
constexpr int kBeardReach = 12;

} // namespace

float detail::structureBeardContribution(int dx, int dy, int dz, int yToGround) {
    if (dx < -kBeardReach || dx >= kBeardReach || dy < -kBeardReach || dy >= kBeardReach
        || dz < -kBeardReach || dz >= kBeardReach) {
        return 0.0F;
    }
    // Java's BEARD_KERNEL entry, computed as `Math.pow(Math.E, x)`. That is
    // *not* bit-identical to `exp(x)`, and the terrain characterization tests
    // pin these bits, so leave the spelling alone.
    const double kernelY = static_cast<double>(dy) + 0.5;
    const double kernelSquared = static_cast<double>(dx) * dx + kernelY * kernelY
        + static_cast<double>(dz) * dz;
    const float kernel = static_cast<float>(std::pow(std::exp(1.0), -kernelSquared / 16.0));

    // The kernel is weighted by a signed distance to the piece's ground level,
    // so terrain is pulled down above it and pushed up below it. Note the
    // deliberate float/double split: the offsets are float, the reciprocal
    // square root is double.
    const float offsetY = static_cast<float>(yToGround) + 0.5F;
    const float squared = static_cast<float>(dx) * dx + offsetY * offsetY
        + static_cast<float>(dz) * dz;
    if (squared == 0.0F) return 0.0F;
    const float value = -offsetY * static_cast<float>(fastInvSqrt(squared / 2.0)) / 2.0F;
    return value * kernel;
}

// --- Structure index -------------------------------------------------------

namespace detail {
namespace {

// The 17x17 reference scan reads starts up to `kReferenceRadius` chunks away,
// and every one of those converts its position to block coordinates. Checking
// the corners up front means the scan cannot fail halfway through.
void requireSupportedReferenceNeighborhood(ChunkPosition target) {
    constexpr int limit = kReferenceRadius;
    if (target.x < std::numeric_limits<int>::min() + limit
        || target.x > std::numeric_limits<int>::max() - limit
        || target.z < std::numeric_limits<int>::min() + limit
        || target.z > std::numeric_limits<int>::max() - limit) {
        throw std::invalid_argument("Structure-reference neighborhood overflows chunk coordinates");
    }
    requireSupportedChunk({target.x - limit, target.z - limit});
    requireSupportedChunk({target.x + limit, target.z + limit});
}

} // namespace

DecorationStep structureStep(StructureKind kind) {
    return kStructures[static_cast<std::size_t>(kind)].step;
}

const std::vector<StructureVariant>& structureVariants(StructureKind kind) {
    using enum StructureVariant;
    static const std::array<std::vector<StructureVariant>, kStructureKindCount> variants{{
        {VillagePlains, VillageDesert, VillageSavanna, VillageSnowy, VillageTaiga},
        {Mineshaft, MineshaftMesa},
        {PortalStandard, PortalDesert, PortalJungle, PortalSwamp, PortalMountain, PortalOcean, PortalNether},
        {AncientCity}}};
    return variants.at(static_cast<std::size_t>(kind));
}

int selectWeightedStructure(const std::vector<int>& weights, LegacyRandom& random,
                            const std::function<bool(std::size_t)>& generate) {
    std::vector<std::size_t> candidates;
    int total = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] <= 0 || weights[i] > std::numeric_limits<int>::max() - total)
            throw std::invalid_argument("Invalid structure selection weight");
        total += weights[i];
        candidates.push_back(i);
    }
    // A singleton set takes the direct generation path, consuming no draw.
    if (candidates.size() == 1) return generate(0) ? 0 : -1;
    while (!candidates.empty()) {
        int choice = random.nextInt(total);
        auto it = candidates.begin();
        for (; it != candidates.end(); ++it) { choice -= weights[*it]; if (choice < 0) break; }
        const auto selected = *it;
        if (generate(selected)) return static_cast<int>(selected);
        total -= weights[selected];
        candidates.erase(it);
    }
    return -1;
}

StructureIndex::StructureIndex(const OverworldNoiseRouter& router, const BiomeSource& biomes, bool enabled,
    const StructureTemplateCatalog* templates)
    : router_(&router), biomes_(&biomes), enabled_(enabled), templates_(templates) {}

std::int64_t StructureIndex::seed() const {
    return router_->seed();
}

const std::vector<StructureStart>& StructureIndex::starts(ChunkPosition chunk) const {
    if (const auto found = starts_.find(chunk); found != starts_.end()) {
        return found->second;
    }

    std::vector<StructureStart> result;
    if (enabled_) {
        const auto [centerX, centerZ] = middleBlock(chunk);
        for (const StructureDefinition& definition : kStructures) {
            if (!isPlacementChunk(definition, router_->seed(), chunk)) continue;

            // Java checks the biome at the structure's own generation point,
            // which for the ancient city is its fixed depth rather than the
            // surface.
            const int generationY = definition.kind == StructureKind::AncientCity
                ? kAncientCityY
                : surfaceY(*router_, centerX, centerZ);
            const StartAttempt attempt{*router_, *biomes_, templates_, definition, chunk,
                                       biomes_->sample(*router_, centerX, generationY, centerZ), generationY};

            // Every variant of a kind carries the same weight; selection keeps
            // drawing from the ones that are left until one is accepted.
            const auto& variants = structureVariants(definition.kind);
            auto selection = largeFeatureRandom(router_->seed(), chunk.x, chunk.z);
            (void)selectWeightedStructure(std::vector<int>(variants.size(), 1), selection,
                [&](std::size_t index) {
                    std::optional<StructureStart> start = tryVariant(attempt, variants[index]);
                    if (!start) return false;
                    result.push_back(std::move(*start));
                    return true;
                });
        }
    }
    return starts_.emplace(chunk, std::move(result)).first->second;
}

const std::vector<StructureReference>& StructureIndex::references(ChunkPosition target) const {
    if (const auto found = references_.find(target); found != references_.end()) {
        return found->second;
    }
    requireSupportedReferenceNeighborhood(target);

    std::vector<StructureReference> result;
    for (int z = target.z - kReferenceRadius; z <= target.z + kReferenceRadius; ++z) {
        for (int x = target.x - kReferenceRadius; x <= target.x + kReferenceRadius; ++x) {
            for (const StructureStart& start : starts({x, z})) {
                if (start.bounds().intersectsChunk(target.x, target.z)) {
                    result.push_back({start.kind, start.source});
                }
            }
        }
    }
    return references_.emplace(target, std::move(result)).first->second;
}

const StructureStart* StructureIndex::resolve(const StructureReference& reference) const {
    const auto& source = starts(reference.source);
    const auto found = std::find_if(source.begin(), source.end(), [&](const StructureStart& start) {
        return start.kind == reference.kind;
    });
    return found == source.end() ? nullptr : &*found;
}

// --- Beardifier ------------------------------------------------------------

ChunkBeardifier::ChunkBeardifier(
    const StructureIndex& index,
    const std::vector<StructureReference>& references,
    ChunkPosition target
) {
    // Density is accumulated by summing floats, so the order pieces are
    // collected in is part of the result: reference order, then piece order.
    for (const auto& reference : references) {
        const StructureStart* start = index.resolve(reference);
        if (start != nullptr) collect(*start, target);
    }
}

ChunkBeardifier::ChunkBeardifier(const std::vector<StructureStart>& starts, ChunkPosition target) {
    for (const auto& start : starts) collect(start, target);
}

void ChunkBeardifier::collect(const StructureStart& start, ChunkPosition target) {
    if (start.adjustment == TerrainAdjustment::None) return;
    const auto [x, z] = blockOrigin(target);
    for (const auto& piece : start.pieces) {
        if (!piece.bounds.inflated(12).intersectsChunk(target.x, target.z)) continue;
        if (piece.projection != PieceProjection::TerrainMatching)
            adaptations_.push_back({piece.bounds, piece.projection == PieceProjection::NonPool ? 0 : piece.groundLevelDelta, start.adjustment});
        if (piece.projection == PieceProjection::NonPool) continue;
        for (const auto& junction : piece.junctions) {
            if (junction.sourceX > x - 12 && junction.sourceX < x + 27
                && junction.sourceZ > z - 12 && junction.sourceZ < z + 27) junctions_.push_back(junction);
        }
    }
}

float ChunkBeardifier::sample(double xValue, double yValue, double zValue) const {
    const int x = static_cast<int>(xValue);
    const int y = static_cast<int>(yValue);
    const int z = static_cast<int>(zValue);

    float result = 0.0F;
    for (const Adaptation& adaptation : adaptations_) {
        const BoundingBox& box = adaptation.bounds;
        // Horizontal distance to the box, zero inside it.
        const int dx = std::max(0, std::max(box.minX - x, x - box.maxX));
        const int dz = std::max(0, std::max(box.minZ - z, z - box.maxZ));
        const int groundY = box.minY + adaptation.groundLevelDelta;
        // Signed, so the kernel knows whether it is above or below the piece.
        const int toGround = y - groundY;

        switch (adaptation.adjustment) {
        case TerrainAdjustment::None:
            break;
        case TerrainAdjustment::Bury:
            result += buryContribution(static_cast<float>(dx), toGround / 2.0F, static_cast<float>(dz));
            break;
        case TerrainAdjustment::BeardThin:
            // Measures from the ground level in both directions, so the beard
            // tapers rather than filling the box.
            result += structureBeardContribution(dx, toGround, dz, toGround) * kBeardScale;
            break;
        case TerrainAdjustment::BeardBox: {
            const int dy = std::max(0, std::max(groundY - y, y - box.maxY));
            result += structureBeardContribution(dx, dy, dz, toGround) * kBeardScale;
            break;
        }
        case TerrainAdjustment::Encapsulate: {
            const int dy = std::max(0, std::max(box.minY - y, y - box.maxY));
            result += buryContribution(dx / 2.0F, dy / 2.0F, dz / 2.0F) * kBeardScale;
            break;
        }
        }
    }
    for (const auto& junction : junctions_) {
        const int dx = x - junction.sourceX;
        const int dy = y - junction.sourceGroundY;
        const int dz = z - junction.sourceZ;
        result += structureBeardContribution(dx, dy, dz, dy) * .4F;
    }
    return result;
}

} // namespace detail
} // namespace mcworld
