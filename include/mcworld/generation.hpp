#pragma once

// Public API for stages 5 and 8 of `minecraft-26.3-worldgen.dot`:
// STRUCTURE_STARTS/STRUCTURE_REFERENCES, and FEATURES on top of the stage-7B
// terrain in `terrain.hpp`.
//
// This complements rather than replaces `OverworldTerrainGenerator`. A stage-7B
// chunk stands alone, but a finalized chunk does not: decoration writes up to
// one chunk outwards, so a chunk is only finished once all nine of its
// neighbours have decorated. `generateArea` is therefore the primary entry
// point, and it generates the halo those writes come from.

#include "mcworld/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mcworld {

// Java's `GenerationStep.Decoration`. The ordinal of a step is part of every
// feature seed (see `WorldgenRandom::setFeatureSeed`), so this order is part of
// the generated world, not a presentation detail.
enum class DecorationStep : std::uint8_t {
    RawGeneration,
    Lakes,
    LocalModifications,
    UndergroundStructures,
    SurfaceStructures,
    Strongholds,
    UndergroundOres,
    UndergroundDecoration,
    FluidSprings,
    VegetalDecoration,
    TopLayerModification,
};

// Relied on by the decoration loop, which walks every step in ordinal order.
constexpr int kDecorationStepCount = static_cast<int>(DecorationStep::TopLayerModification) + 1;

// The structures this port places. Their order fixes each one's index within
// its decoration step, which also feeds the feature seed.
enum class StructureKind : std::uint8_t {
    Village,
    Mineshaft,
    RuinedPortal,
    AncientCity,
};

enum class StructureVariant : std::uint8_t {
    Generic,
    VillagePlains, VillageDesert, VillageSavanna, VillageSnowy, VillageTaiga,
    Mineshaft, MineshaftMesa,
    PortalStandard, PortalDesert, PortalJungle, PortalSwamp, PortalMountain, PortalOcean, PortalNether,
    AncientCity,
};

enum class PieceProjection : std::uint8_t { NonPool, Rigid, TerrainMatching };

struct JigsawJunction {
    int sourceX{}, sourceGroundY{}, sourceZ{}, deltaY{};
    PieceProjection destinationProjection{PieceProjection::Rigid};
    bool operator==(const JigsawJunction&) const = default;
};

struct StructureBlock {
    BlockPosition position; // world coordinates, unlike TerrainChunk block access
    Block block{Block::Air};
    std::shared_ptr<const BlockData> data{};
    bool operator==(const StructureBlock& other) const {
        return position == other.position && block == other.block
            && ((!data && !other.data) || (data && other.data && *data == *other.data));
    }
};

enum class TemplateProcessorKind : std::uint8_t { Ignore, Rule, Rot };
struct TemplateProcessor {
    TemplateProcessorKind kind{TemplateProcessorKind::Ignore};
    std::vector<Block> inputs; // empty means any template material
    std::vector<Block> locations; // empty means any existing world material
    Block output{Block::Air};
    float probability{1}; // rule match probability, or retained integrity for Rot
    std::vector<std::string> inputNames{};
    std::string outputState{};
    int ruleGroup{}; // nonzero groups implement first-matching RuleProcessor lists
    bool operator==(const TemplateProcessor&) const = default;
};

struct StructureTemplateCatalog;

// Relied on by the stage-8 ordering, which walks every kind.
constexpr std::size_t kStructureKindCount = static_cast<std::size_t>(StructureKind::AncientCity) + 1;

// Java's `TerrainAdjustment`: how a structure deforms the terrain around it
// while stage 7B fills density. `None` leaves terrain untouched, which also
// means the structure contributes no reference padding.
enum class TerrainAdjustment : std::uint8_t {
    None,
    Bury,
    BeardThin,
    BeardBox,
    Encapsulate,
};

struct ChunkPosition {
    int x{};
    int z{};

    bool operator==(const ChunkPosition&) const = default;
    // Lexicographic by (x, z). Used only to key lookups; no generation order
    // depends on it.
    auto operator<=>(const ChunkPosition&) const = default;
};

// Inclusive world-coordinate box, matching Minecraft's structure boxes.
struct BoundingBox {
    int minX{};
    int minY{};
    int minZ{};
    int maxX{};
    int maxY{};
    int maxZ{};

    [[nodiscard]] bool intersectsChunk(int chunkX, int chunkZ) const noexcept;
    [[nodiscard]] BoundingBox inflated(int amount) const;
    bool operator==(const BoundingBox&) const = default;
};

// One box of a structure. `groundLevelDelta` is the offset from the box's
// bottom to the level terrain adaptation treats as the piece's floor.
struct StructurePiece {
    BoundingBox bounds;
    Block block{Block::Stone};
    bool hollow{};
    int groundLevelDelta{};
    PieceProjection projection{PieceProjection::NonPool};
    std::vector<JigsawJunction> junctions{};
    // Template pieces place explicit blocks instead of the procedural box.
    bool templatePiece{};
    std::vector<StructureBlock> blocks{};
    std::vector<TemplateProcessor> processors{};
    std::string feature{};
    bool operator==(const StructurePiece&) const = default;
};

struct StructureStart {
    StructureKind kind{};
    ChunkPosition source;
    TerrainAdjustment adjustment{TerrainAdjustment::None};
    DecorationStep step{DecorationStep::SurfaceStructures};
    std::vector<StructurePiece> pieces;
    StructureVariant variant{StructureVariant::Generic};

    [[nodiscard]] bool valid() const noexcept { return !pieces.empty(); }
    // Union of piece boxes, inflated by 12 for adapting structures, as in
    // StructureStart.getBoundingBox. Individual piece bounds remain unadjusted.
    [[nodiscard]] BoundingBox bounds() const;
    bool operator==(const StructureStart&) const = default;
};

// A chunk's record that a start rooted in `source` reaches into it. Stage 8
// resolves these back to starts to place the pieces that overlap.
struct StructureReference {
    StructureKind kind{};
    ChunkPosition source;
    bool operator==(const StructureReference&) const = default;
};

struct GenerationOptions {
    TerrainOptions terrain;
    // Java's `generateStructures` world option, gating stage 5 entirely:
    // without it there are no starts, no references and no beardification.
    bool structures = true;
    // Whether to run stage 8. Structure starts and their terrain adaptation
    // still happen without it, so `false` yields stage-7B terrain plus stage-5
    // metadata.
    bool features = true;
    // Optional immutable template/pool definitions; see structure_templates.hpp.
    // A configured start pool replaces the corresponding procedural family.
    std::shared_ptr<const StructureTemplateCatalog> templates;
};

// One finalized chunk: its blocks plus the stage-5 metadata a saved chunk
// carries alongside them.
struct GeneratedChunk {
    TerrainChunk terrain;
    std::vector<StructureStart> starts;
    std::vector<StructureReference> references;
};

// A rectangle of finalized chunks, row-major in Z and then X.
struct GeneratedArea {
    int firstChunkX{};
    int firstChunkZ{};
    int width{};
    int depth{};
    std::vector<GeneratedChunk> chunks;

    // Throws `std::out_of_range` outside the area.
    [[nodiscard]] const GeneratedChunk& at(int chunkX, int chunkZ) const;
    [[nodiscard]] GeneratedChunk& at(int chunkX, int chunkZ);
};

// Optional phase-level measurements for one generateArea call. Stage 5 is
// measured where its lazy structure index is queried, preserving generation
// and cache-fill order.
struct GenerationProfile {
    double stage5Seconds{};
    double terrainSeconds{};
    double decorationSeconds{};
    double harvestSeconds{};
    std::size_t terrainChunkCount{};
    std::size_t decorationChunkCount{};
    std::size_t outputChunkCount{};
};

class OverworldWorldGenerator {
public:
    // `router` must outlive the generator. Like the terrain generator, this
    // object and its router are intended for one thread at a time.
    explicit OverworldWorldGenerator(const OverworldNoiseRouter& router, GenerationOptions options = {});
    ~OverworldWorldGenerator();

    OverworldWorldGenerator(OverworldWorldGenerator&&) noexcept;
    OverworldWorldGenerator& operator=(OverworldWorldGenerator&&) noexcept;

    OverworldWorldGenerator(const OverworldWorldGenerator&) = delete;
    OverworldWorldGenerator& operator=(const OverworldWorldGenerator&) = delete;

    // Finalizes an area independently of prior calls: the result depends only
    // on the seed and the options, never on what was generated before.
    //
    // Decoration runs in a canonical Z/X order over the requested chunks *and*
    // the one-chunk halo whose features write back into them, so the returned
    // chunks are complete.
    //
    // Throws `std::invalid_argument` for an empty area, or for one whose halo
    // would leave the supported coordinate grid.
    [[nodiscard]] GeneratedArea generateArea(int firstChunkX, int firstChunkZ, int width, int depth);

    // The profiled overload produces the same area while recording the major
    // generation phases. `profile` is reset at the start of the call.
    [[nodiscard]] GeneratedArea generateArea(
        int firstChunkX, int firstChunkZ, int width, int depth, GenerationProfile& profile
    );

    // One finalized chunk. Equivalent to a 1x1 `generateArea`, and about as
    // expensive: the halo dominates, so prefer `generateArea` for regions.
    [[nodiscard]] GeneratedChunk generate(int chunkX, int chunkZ);

    // Stage-5 metadata without running terrain or decoration. Each call is
    // self-contained, so scanning many chunks this way re-derives the
    // neighbours they share; `generateArea` shares that work internally.
    [[nodiscard]] std::vector<StructureStart> structureStarts(int chunkX, int chunkZ) const;
    [[nodiscard]] std::vector<StructureReference> structureReferences(int chunkX, int chunkZ) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcworld
