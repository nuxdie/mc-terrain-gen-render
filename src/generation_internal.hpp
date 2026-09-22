#pragma once

// Internal contract between the parts of stages 5 and 8 of
// `minecraft-26.3-worldgen.dot`:
//
//   5. STRUCTURE_STARTS / STRUCTURE_REFERENCES  -> src/structures.cpp
//   8. FEATURES (pieces + biome decoration)     -> src/decoration.cpp
//   pass orchestration and public value types   -> src/generation.cpp
//
// Nothing here is part of the public API; `include/mcworld/generation.hpp` is.
//
// The stages meet only through what is declared here:
//
//  * `StructureIndex` answers "which starts exist" and "which chunks reference
//    them" for both, memoizing so the same chunk is never derived twice.
//  * `ChunkBeardifier` turns the starts a chunk references into the extra
//    density stage 7B adds while filling that chunk, which is why stage 5 has
//    to run before terrain rather than alongside decoration.
//  * `structureStep` is the one thing stage 8 needs from the stage-5 registry:
//    where a structure sits in the decoration order.
//  * `decorateChunk` runs stage 8 over a mutable 3x3 of already-generated
//    chunks.

#include "legacy_random.hpp"
#include "mcworld/generation.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <functional>
#include <optional>
#include <vector>

namespace mcworld::detail {

// --- Chunk grid ------------------------------------------------------------

// Generated chunks are keyed by position while a pass is in flight. The
// ordering is only used for lookup; no pass depends on iteration order.
using ChunkMap = std::map<ChunkPosition, TerrainChunk>;

// The world X or Z of a chunk's minimum corner.
//
// Throws `std::invalid_argument` unless the chunk, plus the slack every
// stage-5/8 pass needs around it, stays inside `int`. A pass validates its
// outermost chunks once (see `requireSupportedChunk`) and then converts
// freely.
[[nodiscard]] int chunkMinBlock(int chunk);

// `chunkMinBlock` for both axes, which is what block-space passes want.
struct BlockOrigin {
    int x{};
    int z{};
};

[[nodiscard]] BlockOrigin blockOrigin(ChunkPosition chunk);

// Java's `ChunkPos.getMiddleBlockX/Z`: the origin plus 8. Structure placement
// measures its pieces from here.
[[nodiscard]] BlockOrigin middleBlock(ChunkPosition chunk);

// The guard of `chunkMinBlock` without the conversion, for validating the edge
// of a pass before it starts.
void requireSupportedChunk(ChunkPosition chunk);

// Chunk-local X or Z of a world X or Z: Java's `& 15`, spelled so that it
// still holds if the chunk width ever changes.
[[nodiscard]] inline int localBlock(int world) {
    return world - floorDiv(world, TerrainChunk::width) * TerrainChunk::width;
}

// Index of a column in a `TerrainChunk::Heightmap`, from world X and Z.
[[nodiscard]] inline std::size_t columnIndex(int worldX, int worldZ) {
    return static_cast<std::size_t>(localBlock(worldZ) * TerrainChunk::width + localBlock(worldX));
}

// --- Stage 5: structure starts and references ------------------------------

// Chebyshev chunk radius of the STRUCTURE_REFERENCES scan: 8, i.e. 17x17
// source chunks per target.
constexpr int kReferenceRadius = 8;

// The structure starts of stage 5 and the cross-chunk references derived from
// them, memoized per chunk.
//
// Every answer is a pure function of the router, the biome source and the
// enable flag, so callers may ask for chunks in any order and get the same
// world. `router` and `biomes` are borrowed and must outlive the index; the
// memo lives and dies with the instance, which is what bounds its size.
class StructureIndex {
public:
    StructureIndex(const OverworldNoiseRouter& router, const BiomeSource& biomes, bool enabled,
                   const StructureTemplateCatalog* templates = nullptr);

    [[nodiscard]] std::int64_t seed() const;

    // The starts rooted in `chunk`. Empty when structures are disabled.
    [[nodiscard]] const std::vector<StructureStart>& starts(ChunkPosition chunk) const;

    // The starts whose adjusted bounding box reaches into `chunk`, found by
    // scanning the 17x17 source chunks around it.
    [[nodiscard]] const std::vector<StructureReference>& references(ChunkPosition chunk) const;

    // The start a reference points at, or null if its source no longer has
    // one. References and starts are always derived from the same index, so
    // this only returns null for a hand-built reference.
    [[nodiscard]] const StructureStart* resolve(const StructureReference& reference) const;

private:
    const OverworldNoiseRouter* router_;
    const BiomeSource* biomes_;
    bool enabled_;
    const StructureTemplateCatalog* templates_;
    mutable std::map<ChunkPosition, std::vector<StructureStart>> starts_;
    mutable std::map<ChunkPosition, std::vector<StructureReference>> references_;
};

// Java: `Beardifier`, narrowed to the starts one chunk references.
//
// Stage 7B adds this to the final density of every block in that chunk, so it
// is sampled ~98k times per chunk; `empty()` lets the caller skip the whole
// per-block branch when no referenced start adapts terrain.
class ChunkBeardifier final : public Beardifier {
public:
    ChunkBeardifier(const StructureIndex& index, const std::vector<StructureReference>& references, ChunkPosition target);
    ChunkBeardifier(const std::vector<StructureStart>& starts, ChunkPosition target);

    [[nodiscard]] float sample(double x, double y, double z) const override;

    [[nodiscard]] bool empty() const noexcept { return adaptations_.empty() && junctions_.empty(); }

private:
    // Only these three fields of a piece affect density; the piece's block and
    // hollowness belong to stage 8.
    struct Adaptation {
        BoundingBox bounds;
        int groundLevelDelta{};
        TerrainAdjustment adjustment{};
    };

    std::vector<Adaptation> adaptations_;
    std::vector<JigsawJunction> junctions_;
    void collect(const StructureStart& start, ChunkPosition target);
};

// Java: `Beardifier.getBeardContribution` for one piece offset. Separate from
// `ChunkBeardifier` because its exact float bits are pinned by a test.
[[nodiscard]] float structureBeardContribution(int dx, int dy, int dz, int yToGround);

// The decoration step a structure kind is placed in.
//
// Stage 8 walks the kinds in `StructureKind` order, which stage 5 keeps equal
// to its own registration order (there is a static_assert for it). A kind's
// position among those sharing its step is therefore its index for
// `WorldgenRandom::setFeatureSeed`.
[[nodiscard]] DecorationStep structureStep(StructureKind kind);
[[nodiscard]] const std::vector<StructureVariant>& structureVariants(StructureKind kind);
[[nodiscard]] int selectWeightedStructure(const std::vector<int>& weights, LegacyRandom& random,
                                         const std::function<bool(std::size_t)>& generate);
[[nodiscard]] std::vector<StructurePiece> assembleJigsaw(
    const StructureTemplateCatalog& catalog, StructureVariant variant, BlockPosition origin,
    LegacyRandom& random, const std::function<int(int, int)>& surfaceHeight, BlockPosition* generationPoint = nullptr);
[[nodiscard]] std::optional<Block> processStructureBlock(
    const StructureBlock& block, Block existing, const std::vector<TemplateProcessor>& processors,
    std::shared_ptr<const BlockData>* outputData = nullptr);

// --- Stage 8: features -----------------------------------------------------

// How far, in chunks, a feature may write: Java's FEATURES `WorldGenRegion` is
// the centre chunk plus its eight neighbours.
constexpr int kFeatureWriteRadius = 1;

// Runs FEATURES for one chunk: for every decoration step in ordinal order, the
// structure pieces registered to that step and then the biome decoration.
//
// `chunks` must already hold `chunk` and all eight of its neighbours, because
// decoration writes into them; a write past that 3x3 throws
// `std::out_of_range` rather than being dropped.
void decorateChunk(ChunkMap& chunks, const StructureIndex& structures, ChunkPosition chunk);

// MiscOverworldFeatures.SPRING_WATER, restricted to our material palette.
[[nodiscard]] inline bool springRock(Block block) {
    using enum Block;
    return block == Stone || block == Granite || block == Diorite || block == Andesite || block == Deepslate || block == Tuff
        || block == Calcite || block == Dirt || block == Snow || block == PowderSnow
        || block == PackedIce;
}

// SpringFeature counts the four horizontal neighbors and below, not above.
[[nodiscard]] inline bool canPlaceWaterSpring(
    Block current, Block above, Block below, const std::array<Block, 4>& sides
) {
    if (!springRock(above) || !springRock(below)
        || (current != Block::Air && !springRock(current))) return false;
    const int rocks = 1 + std::count_if(sides.begin(), sides.end(), springRock);
    const int holes = std::count(sides.begin(), sides.end(), Block::Air);
    return rocks == 4 && holes == 1;
}

} // namespace mcworld::detail
