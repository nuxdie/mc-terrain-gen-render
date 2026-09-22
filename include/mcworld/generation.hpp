#pragma once

// Synchronous graph-level implementation of stages 5 and 8 from
// `minecraft-26.3-worldgen.dot`. It deliberately complements the independent
// stage-7B chunk API: decoration needs a mutable 3x3 region, so finalized chunks
// are generated as an area after every source chunk in a one-chunk halo ran.

#include "mcworld/terrain.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mcworld {

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

enum class StructureKind : std::uint8_t {
    Village,
    Mineshaft,
    RuinedPortal,
    AncientCity,
};

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

struct StructurePiece {
    BoundingBox bounds;
    Block block{Block::Stone};
    bool hollow{};
    int groundLevelDelta{};
    bool operator==(const StructurePiece&) const = default;
};

struct StructureStart {
    StructureKind kind{};
    ChunkPosition source;
    TerrainAdjustment adjustment{TerrainAdjustment::None};
    DecorationStep step{DecorationStep::SurfaceStructures};
    std::vector<StructurePiece> pieces;

    [[nodiscard]] bool valid() const noexcept { return !pieces.empty(); }
    [[nodiscard]] BoundingBox bounds() const;
    bool operator==(const StructureStart&) const = default;
};

struct StructureReference {
    StructureKind kind{};
    ChunkPosition source;
    bool operator==(const StructureReference&) const = default;
};

struct GenerationOptions {
    TerrainOptions terrain;
    bool structures = true;
    bool features = true;
};

struct GeneratedChunk {
    TerrainChunk terrain;
    std::vector<StructureStart> starts;
    std::vector<StructureReference> references;
};

class GeneratedArea {
public:
    int firstChunkX{};
    int firstChunkZ{};
    int width{};
    int depth{};
    std::vector<GeneratedChunk> chunks;

    [[nodiscard]] const GeneratedChunk& at(int chunkX, int chunkZ) const;
    [[nodiscard]] GeneratedChunk& at(int chunkX, int chunkZ);
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

    // Finalizes an area independently of prior calls. The implementation runs
    // decoration sources in a canonical Z/X order, including the halo whose
    // features can write into the returned chunks.
    [[nodiscard]] GeneratedArea generateArea(int firstChunkX, int firstChunkZ, int width, int depth);
    [[nodiscard]] GeneratedChunk generate(int chunkX, int chunkZ);

    // Stage-5 metadata without running terrain or decoration.
    [[nodiscard]] std::vector<StructureStart> structureStarts(int chunkX, int chunkZ) const;
    [[nodiscard]] std::vector<StructureReference> structureReferences(int chunkX, int chunkZ) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcworld
