// Orchestration for stages 5 and 8 of `minecraft-26.3-worldgen.dot`, plus the
// public value types they share.
//
// `OverworldWorldGenerator::Impl::generateArea` is the spine, and it runs the
// diagram's nodes in order over progressively smaller rectangles:
//
//   stage 5   starts and references          every chunk any later pass reads
//   stage 7B  terrain, beardified            output + 2 chunks
//   stage 8   structure pieces + decoration  output + 1 chunk
//   output    the chunks the caller asked for
//
// Each rectangle is wider than the one it feeds because generation reaches
// outwards: decoration writes one chunk past itself, and a structure's
// reference bounds reach eight. That widening, not the per-pass logic, is what
// makes area generation the primary entry point - see `planArea`.
//
// The stages themselves live in src/structures.cpp and src/decoration.cpp; the
// seam between them is `generation_internal.hpp`.

#include "generation_internal.hpp"
#include "mcworld/structure_templates.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace mcworld {
namespace {

using ProfileClock = std::chrono::steady_clock;

[[nodiscard]] double elapsedSeconds(ProfileClock::time_point started) {
    return std::chrono::duration<double>(ProfileClock::now() - started).count();
}

void addTerrainProfile(TerrainGenerationProfile& total, const TerrainGenerationProfile& chunk) {
    total.biomeSeconds += chunk.biomeSeconds;
    total.densitySeconds += chunk.densitySeconds;
    total.materialSeconds += chunk.materialSeconds;
    total.carverSeconds += chunk.carverSeconds;
    total.heightmapSeconds += chunk.heightmapSeconds;
    total.fluidSeconds += chunk.fluidSeconds;
    total.chunkCount += chunk.chunkCount;
}

// Chunks whose decoration can write into the output.
constexpr int kDecorationSourceHalo = detail::kFeatureWriteRadius;

// Terrain has to exist for every decoration source *and* for the neighbours
// those sources write into.
constexpr int kTerrainHalo = detail::kFeatureWriteRadius + kDecorationSourceHalo;

// An inclusive rectangle of chunks. `last < first` on either axis means empty.
struct ChunkSpan {
    int firstX{};
    int firstZ{};
    int lastX{};
    int lastZ{};
};

// A span `forEachChunk` visits nothing for, used for the passes a disabled
// option skips.
constexpr ChunkSpan kNoChunks{0, 0, -1, -1};

// Visits a span in the canonical Z-then-X order every pass uses. The order is
// not load-bearing - each chunk's content is independent of it - but keeping
// one order everywhere makes traces comparable.
template <typename Visit>
void forEachChunk(const ChunkSpan& span, Visit visit) {
    for (int z = span.firstZ; z <= span.lastZ; ++z) {
        for (int x = span.firstX; x <= span.lastX; ++x) {
            visit(ChunkPosition{x, z});
        }
    }
}

[[nodiscard]] ChunkSpan inflated(const ChunkSpan& span, int chunks) {
    return {span.firstX - chunks, span.firstZ - chunks, span.lastX + chunks, span.lastZ + chunks};
}

// The rectangles one `generateArea` call works over, widest first.
struct AreaPlan {
    ChunkSpan terrain;
    ChunkSpan decoration;
    ChunkSpan output;
};

// Resolves an area request into those rectangles, and does all of the
// coordinate validation for the call in one place: every pass below may then
// convert chunk positions to block coordinates without re-checking.
//
// Throws `std::invalid_argument` for an empty area, or if the widest rectangle
// the call needs - including the structure-reference scan around it, which is
// read but never iterated here - would leave the supported grid.
[[nodiscard]] AreaPlan planArea(int firstX, int firstZ, int width, int depth, bool features) {
    if (width <= 0 || depth <= 0) {
        throw std::invalid_argument("Generated area dimensions must be positive");
    }
    const std::int64_t lastX = static_cast<std::int64_t>(firstX) + width - 1;
    const std::int64_t lastZ = static_cast<std::int64_t>(firstZ) + depth - 1;
    if (lastX > std::numeric_limits<int>::max() || lastZ > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Generated area dimensions overflow chunk coordinates");
    }

    // Without stage 8 nothing writes outside its own chunk, so terrain and
    // output coincide and there is nothing to decorate.
    const int terrainHalo = features ? kTerrainHalo : 0;
    const int totalHalo = terrainHalo + detail::kReferenceRadius;
    if (firstX < std::numeric_limits<int>::min() + totalHalo
        || firstZ < std::numeric_limits<int>::min() + totalHalo
        || lastX > std::numeric_limits<int>::max() - totalHalo
        || lastZ > std::numeric_limits<int>::max() - totalHalo) {
        throw std::invalid_argument("Generated area halo overflows chunk coordinates");
    }

    const ChunkSpan output{firstX, firstZ, static_cast<int>(lastX), static_cast<int>(lastZ)};
    const ChunkSpan reach = inflated(output, totalHalo);
    detail::requireSupportedChunk({reach.firstX, reach.firstZ});
    detail::requireSupportedChunk({reach.lastX, reach.lastZ});

    // Designated, because three same-typed spans are easy to transpose.
    return AreaPlan{
        .terrain = inflated(output, terrainHalo),
        .decoration = features ? inflated(output, kDecorationSourceHalo) : kNoChunks,
        .output = output,
    };
}

// Decoration can queue a fluid position that a later feature overwrote, and can
// queue the same position more than once. Reducing the queue to the distinct
// positions that are still fluid, in a canonical order, makes it a function of
// the chunk's final contents rather than of the order features ran in.
void normalizeFluidUpdates(TerrainChunk& chunk) {
    auto& updates = chunk.fluidPostProcessing;
    std::erase_if(updates, [&](BlockPosition position) {
        return !isFluid(chunk.at(position.x, position.y, position.z));
    });
    std::sort(updates.begin(), updates.end(), [](BlockPosition left, BlockPosition right) {
        return std::tie(left.z, left.x, left.y) < std::tie(right.z, right.x, right.y);
    });
    updates.erase(std::unique(updates.begin(), updates.end()), updates.end());
}

} // namespace

// --- Chunk grid ------------------------------------------------------------

namespace detail {

int chunkMinBlock(int chunk) {
    // The margin covers the halo any single pass adds on top of a validated
    // chunk position, so passes can do plain `int` arithmetic from here on.
    constexpr std::int64_t margin = 1024;
    const std::int64_t value = static_cast<std::int64_t>(chunk) * TerrainChunk::width;
    if (value < std::numeric_limits<int>::min() + margin ||
        value > std::numeric_limits<int>::max() - margin) {
        throw std::invalid_argument("Generated area is outside the supported integer grid");
    }
    return static_cast<int>(value);
}

BlockOrigin blockOrigin(ChunkPosition chunk) {
    return {chunkMinBlock(chunk.x), chunkMinBlock(chunk.z)};
}

BlockOrigin middleBlock(ChunkPosition chunk) {
    constexpr int offset = TerrainChunk::width / 2;
    const BlockOrigin origin = blockOrigin(chunk);
    return {origin.x + offset, origin.z + offset};
}

void requireSupportedChunk(ChunkPosition chunk) {
    (void)blockOrigin(chunk);
}

} // namespace detail

// --- Public value types ----------------------------------------------------

bool BoundingBox::intersectsChunk(int chunkX, int chunkZ) const noexcept {
    const std::int64_t minChunkX = static_cast<std::int64_t>(chunkX) * TerrainChunk::width;
    const std::int64_t minChunkZ = static_cast<std::int64_t>(chunkZ) * TerrainChunk::width;
    return maxX >= minChunkX && minX <= minChunkX + TerrainChunk::width - 1
        && maxZ >= minChunkZ && minZ <= minChunkZ + TerrainChunk::width - 1;
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
    // Java: `StructureStart.getBoundingBox` pads an adapting structure by the
    // beard kernel's reach, so chunks it only deforms still reference it.
    constexpr int adaptationPadding = 12;
    return adjustment == TerrainAdjustment::None ? result : result.inflated(adaptationPadding);
}

const GeneratedChunk& GeneratedArea::at(int chunkX, int chunkZ) const {
    const std::int64_t lastX = static_cast<std::int64_t>(firstChunkX) + width;
    const std::int64_t lastZ = static_cast<std::int64_t>(firstChunkZ) + depth;
    if (chunkX < firstChunkX || static_cast<std::int64_t>(chunkX) >= lastX
        || chunkZ < firstChunkZ || static_cast<std::int64_t>(chunkZ) >= lastZ) {
        throw std::out_of_range("Chunk is outside generated area");
    }
    const auto row = static_cast<std::size_t>(chunkZ - firstChunkZ);
    const auto column = static_cast<std::size_t>(chunkX - firstChunkX);
    return chunks.at(row * static_cast<std::size_t>(width) + column);
}

GeneratedChunk& GeneratedArea::at(int chunkX, int chunkZ) {
    return const_cast<GeneratedChunk&>(std::as_const(*this).at(chunkX, chunkZ));
}

// --- Generation ------------------------------------------------------------

class OverworldWorldGenerator::Impl {
public:
    Impl(const OverworldNoiseRouter& router, GenerationOptions options)
        : router_(router), options_(std::move(options)) {
        if (!options_.terrain.biomes) options_.terrain.biomes = std::make_shared<BiomeSource>();
        if (options_.templates) options_.templates->validate();
    }

    GeneratedArea generateArea(
        int firstX, int firstZ, int width, int depth, GenerationProfile* profile
    ) {
        if (profile != nullptr) *profile = {};
        const AreaPlan plan = planArea(firstX, firstZ, width, depth, options_.features);
        const auto stage5Started = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        const detail::StructureIndex structures = makeStructureIndex();
        if (profile != nullptr) profile->stage5Seconds += elapsedSeconds(stage5Started);
        OverworldTerrainGenerator terrain(router_, options_.terrain);

        // Stage 7B, with stage 5's terrain adaptation folded into the density
        // graph. The beardifier is sampled once per block, so when no
        // referenced start adapts terrain we take the overload that skips it
        // entirely rather than adding a zero ~98k times.
        detail::ChunkMap chunks;
        forEachChunk(plan.terrain, [&](ChunkPosition chunk) {
            const auto structureStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
            const auto& references = structures.references(chunk);
            const detail::ChunkBeardifier beardifier(structures, references, chunk);
            if (profile != nullptr) {
                profile->stage5Seconds += elapsedSeconds(structureStarted);
                ++profile->terrainChunkCount;
            }
            const auto terrainStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
            if (profile != nullptr) {
                TerrainGenerationProfile chunkProfile;
                chunks.emplace(chunk, beardifier.empty()
                    ? terrain.generate(chunk.x, chunk.z, chunkProfile)
                    : terrain.generate(chunk.x, chunk.z, beardifier, chunkProfile));
                profile->terrainSeconds += elapsedSeconds(terrainStarted);
                addTerrainProfile(profile->terrainDetail, chunkProfile);
            } else {
                chunks.emplace(chunk, beardifier.empty()
                    ? terrain.generate(chunk.x, chunk.z)
                    : terrain.generate(chunk.x, chunk.z, beardifier));
            }
        });

        // Stage 8. Every source runs before any output chunk is harvested,
        // because a source's writes land in its neighbours.
        forEachChunk(plan.decoration, [&](ChunkPosition chunk) {
            const auto decorationStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
            detail::decorateChunk(chunks, structures, chunk);
            if (profile != nullptr) {
                profile->decorationSeconds += elapsedSeconds(decorationStarted);
                ++profile->decorationChunkCount;
            }
        });

        const auto harvestStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        GeneratedArea result = harvest(plan, structures, chunks);
        if (profile != nullptr) {
            profile->harvestSeconds = elapsedSeconds(harvestStarted);
            profile->outputChunkCount = result.chunks.size();
        }
        return result;
    }

    [[nodiscard]] std::vector<StructureStart> starts(ChunkPosition chunk) const {
        const detail::StructureIndex structures = makeStructureIndex();
        return structures.starts(chunk);
    }

    [[nodiscard]] std::vector<StructureReference> references(ChunkPosition chunk) const {
        const detail::StructureIndex structures = makeStructureIndex();
        return structures.references(chunk);
    }

private:
    // A fresh index per public call. Its memo makes one call internally
    // consistent and cheap; not sharing it across calls is deliberate, so a
    // long-lived generator does not accumulate every chunk it was ever asked
    // about.
    [[nodiscard]] detail::StructureIndex makeStructureIndex() const {
        return detail::StructureIndex(router_, *options_.terrain.biomes, options_.structures, options_.templates.get());
    }

    [[nodiscard]] static GeneratedArea harvest(
        const AreaPlan& plan,
        const detail::StructureIndex& structures,
        detail::ChunkMap& chunks
    ) {
        GeneratedArea result;
        result.firstChunkX = plan.output.firstX;
        result.firstChunkZ = plan.output.firstZ;
        result.width = plan.output.lastX - plan.output.firstX + 1;
        result.depth = plan.output.lastZ - plan.output.firstZ + 1;
        result.chunks.reserve(static_cast<std::size_t>(result.width)
                              * static_cast<std::size_t>(result.depth));
        forEachChunk(plan.output, [&](ChunkPosition position) {
            GeneratedChunk chunk;
            chunk.terrain = std::move(chunks.at(position));
            normalizeFluidUpdates(chunk.terrain);
            chunk.starts = structures.starts(position);
            chunk.references = structures.references(position);
            result.chunks.push_back(std::move(chunk));
        });
        return result;
    }

    const OverworldNoiseRouter& router_;
    GenerationOptions options_;
};

OverworldWorldGenerator::OverworldWorldGenerator(const OverworldNoiseRouter& router, GenerationOptions options)
    : impl_(std::make_unique<Impl>(router, std::move(options))) {}
OverworldWorldGenerator::~OverworldWorldGenerator() = default;
OverworldWorldGenerator::OverworldWorldGenerator(OverworldWorldGenerator&&) noexcept = default;
OverworldWorldGenerator& OverworldWorldGenerator::operator=(OverworldWorldGenerator&&) noexcept = default;

GeneratedArea OverworldWorldGenerator::generateArea(int firstChunkX, int firstChunkZ, int width, int depth) {
    return impl_->generateArea(firstChunkX, firstChunkZ, width, depth, nullptr);
}

GeneratedArea OverworldWorldGenerator::generateArea(
    int firstChunkX, int firstChunkZ, int width, int depth, GenerationProfile& profile
) {
    return impl_->generateArea(firstChunkX, firstChunkZ, width, depth, &profile);
}

GeneratedChunk OverworldWorldGenerator::generate(int chunkX, int chunkZ) {
    GeneratedArea area = generateArea(chunkX, chunkZ, 1, 1);
    return std::move(area.chunks.front());
}

std::vector<StructureStart> OverworldWorldGenerator::structureStarts(int chunkX, int chunkZ) const {
    return impl_->starts({chunkX, chunkZ});
}

std::vector<StructureReference> OverworldWorldGenerator::structureReferences(int chunkX, int chunkZ) const {
    return impl_->references({chunkX, chunkZ});
}

} // namespace mcworld
