// Step 7B of `minecraft-26.3-worldgen.dot`: chunk terrain generation.
//
// `OverworldTerrainGenerator::Impl::generate` is the spine of that diagram and
// runs its nodes in order:
//
//   biome palette -> aquifer -> density fill -> materials -> carvers
//                -> heightmaps -> fluid post-processing
//
// Each pass is implemented in its own translation unit (see
// `terrain_internal.hpp`); this file owns the chunk container, the pass order
// and the heightmap bookkeeping they share.
//
// Two ordering rules are load-bearing and easy to break by accident:
//
//  * Blocks are visited in Z, X, descending-Y order, and positional seeds use
//    world coordinates, so generating neighbouring chunks in a different order
//    cannot change what a chunk contains.
//  * `worldSurface` is maintained *during* a pass (see `setWorldgenBlock`),
//    because material gradients and carver soil repair read it while writing.
//    The other heightmaps are primed between passes.

#include "terrain_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace mcworld {
namespace {

// Chunk coordinates are validated with room to spare for the carver
// neighbourhood (radius 8 chunks), aquifer probes and interpolation halos, so
// that no pass has to re-check for overflow per block.
constexpr std::int64_t kCoordinateMargin = 512;

// Biome palette dimensions, in quart cells.
constexpr int kPaletteWidth = TerrainChunk::width / detail::kQuartSize;
constexpr int kPaletteHeight = TerrainChunk::height / detail::kQuartSize;

using ProfileClock = std::chrono::steady_clock;

[[nodiscard]] double elapsedSeconds(ProfileClock::time_point started) {
    return std::chrono::duration<double>(ProfileClock::now() - started).count();
}

[[nodiscard]] std::size_t index(int x, int y, int z) {
    if (x < 0 || x >= TerrainChunk::width || z < 0 || z >= TerrainChunk::width ||
        y < TerrainChunk::minY || y >= TerrainChunk::maxY) {
        throw std::out_of_range("Terrain block coordinate outside chunk");
    }
    return static_cast<std::size_t>((z * TerrainChunk::width + x) * TerrainChunk::height + y - TerrainChunk::minY);
}

} // namespace

// --- Chunk storage ---------------------------------------------------------

Block TerrainChunk::at(int x, int y, int z) const {
    return blocks.at(index(x, y, z));
}

void TerrainChunk::set(int x, int y, int z, Block block) {
    const auto i = index(x, y, z);
    blocks.at(i) = block;
    if (!blockData.empty()) blockData.erase(i);
}

void TerrainChunk::setData(int x, int y, int z, std::shared_ptr<const BlockData> data) {
    const auto i = index(x, y, z);
    if (data) blockData[i] = std::move(data);
    else blockData.erase(i);
}

const BlockData* TerrainChunk::dataAt(int x, int y, int z) const {
    const auto it = blockData.find(index(x, y, z));
    return it == blockData.end() ? nullptr : it->second.get();
}

Biome TerrainChunk::biomeAt(int x, int y, int z) const {
    (void)index(x, y, z); // Bounds check only; the palette is quart resolution.
    constexpr int quart = detail::kQuartSize;
    const int cell = (z / quart * kPaletteWidth + x / quart) * kPaletteHeight + (y - minY) / quart;
    return biomes[static_cast<std::size_t>(cell)];
}

void TerrainChunk::primeHeightmaps() {
    for (const auto& map : detail::kHeightmaps) {
        (this->*map.column).fill(minY);
    }
    for (int z = 0; z < width; ++z) {
        for (int x = 0; x < width; ++x) {
            const auto column = static_cast<std::size_t>(z * width + x);
            std::size_t unresolved = detail::kHeightmaps.size();
            // Heightmaps store the first free Y, so the topmost qualifying
            // block found scanning down wins; `minY` marks a column still
            // looking for one.
            for (int y = maxY - 1; y >= minY; --y) {
                const Block block = at(x, y, z);
                for (const auto& map : detail::kHeightmaps) {
                    int& height = (this->*map.column)[column];
                    if (height == minY && map.counts(block)) {
                        height = y + 1;
                        --unresolved;
                    }
                }
                if (unresolved == 0) break;
            }
        }
    }
}

Biome BiomeSource::sample(const OverworldNoiseRouter& router, int x, int y, int z) const {
    return sampleOverworldBiome(router, x, y, z);
}

// --- Generation ------------------------------------------------------------

class OverworldTerrainGenerator::Impl {
public:
    Impl(const OverworldNoiseRouter& router, TerrainOptions options)
        : router_(router), options_(std::move(options)) {
        if (!options_.biomes) {
            options_.biomes = std::make_shared<BiomeSource>();
        }
    }

    TerrainChunk generate(
        int chunkX,
        int chunkZ,
        const Beardifier* structures = nullptr,
        TerrainGenerationProfile* profile = nullptr
    ) {
        if (profile != nullptr) {
            *profile = {};
            profile->chunkCount = 1;
        }
        const int originX = checkedOrigin(chunkX);
        const int originZ = checkedOrigin(chunkZ);

        TerrainChunk chunk;
        chunk.chunkX = chunkX;
        chunk.chunkZ = chunkZ;

        const auto biomeStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        fillBiomePalette(chunk, originX, originZ);
        if (profile != nullptr) profile->biomeSeconds = elapsedSeconds(biomeStarted);

        // The aquifer is shared with the carvers below: both passes have to
        // agree on which fluid body a position belongs to.
        const auto densityStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        detail::Aquifer aquifer(router_, options_.aquifers, chunkX, chunkZ);
        fillDensity(chunk, aquifer, originX, originZ, structures);
        if (profile != nullptr) profile->densitySeconds = elapsedSeconds(densityStarted);

        auto heightmapStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        chunk.primeHeightmaps();
        if (profile != nullptr) profile->heightmapSeconds += elapsedSeconds(heightmapStarted);

        const auto materialStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        detail::buildMaterials(chunk, router_, options_.oreVeins,
                               detail::makeBlockBiomeGetter(chunk, router_, *options_.biomes, true));
        if (profile != nullptr) profile->materialSeconds = elapsedSeconds(materialStarted);

        heightmapStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        chunk.primeHeightmaps();
        if (profile != nullptr) profile->heightmapSeconds += elapsedSeconds(heightmapStarted);

        if (options_.carvers) {
            const auto carverStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
            detail::carve(chunk, router_, *options_.biomes, aquifer, options_.oreVeins);
            if (profile != nullptr) profile->carverSeconds = elapsedSeconds(carverStarted);
        }

        heightmapStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        chunk.primeHeightmaps();
        if (profile != nullptr) profile->heightmapSeconds += elapsedSeconds(heightmapStarted);

        const auto fluidStarted = profile != nullptr ? ProfileClock::now() : ProfileClock::time_point{};
        compactFluidUpdates(chunk);
        if (profile != nullptr) profile->fluidSeconds = elapsedSeconds(fluidStarted);
        return chunk;
    }

private:
    [[nodiscard]] static int checkedOrigin(int chunkCoordinate) {
        const auto origin = static_cast<std::int64_t>(chunkCoordinate) * TerrainChunk::width;
        if (origin < std::numeric_limits<int>::min() + kCoordinateMargin ||
            origin > std::numeric_limits<int>::max() - kCoordinateMargin) {
            throw std::invalid_argument("Terrain chunk is outside supported coordinate range");
        }
        return static_cast<int>(origin);
    }

    // Java: `ProtoChunk.fillBiomesFromNoise`, one entry per 4x4x4 cell.
    void fillBiomePalette(TerrainChunk& chunk, int originX, int originZ) const {
        for (int z = 0; z < kPaletteWidth; ++z) {
            for (int x = 0; x < kPaletteWidth; ++x) {
                for (int y = 0; y < kPaletteHeight; ++y) {
                    chunk.biomes[(z * kPaletteWidth + x) * kPaletteHeight + y] =
                        options_.biomes->sample(router_,
                                                originX + x * detail::kQuartSize,
                                                TerrainChunk::minY + y * detail::kQuartSize,
                                                originZ + z * detail::kQuartSize);
                }
            }
        }
    }

    // Java: `NoiseBasedChunkGenerator.fillFromNoise`. Sample the final density
    // at every block and let the aquifer turn it into a block.
    void fillDensity(
        TerrainChunk& chunk,
        detail::Aquifer& aquifer,
        int originX,
        int originZ,
        const Beardifier* structures
    ) const {
        for (int z = 0; z < TerrainChunk::width; ++z) {
            for (int x = 0; x < TerrainChunk::width; ++x) {
                for (int y = TerrainChunk::maxY - 1; y >= TerrainChunk::minY; --y) {
                    float density = router_.sampleFinalDensity(originX + x, y, originZ + z);
                    if (structures != nullptr) {
                        density += structures->sample(originX + x, y, originZ + z);
                    }
                    const detail::Substance substance = aquifer.sample(originX + x, y, originZ + z, density);
                    chunk.set(x, y, z, substance.block);
                    if (substance.schedule && isFluid(substance.block)) {
                        chunk.fluidPostProcessing.push_back({x, y, z});
                    }
                }
            }
        }
    }

    // Later passes overwrite blocks, so a queued position may no longer hold a
    // fluid, and carvers can queue one twice. Normalize into the chunk's own
    // z/x/y order so the result does not depend on which pass queued what.
    static void compactFluidUpdates(TerrainChunk& chunk) {
        auto& updates = chunk.fluidPostProcessing;
        std::erase_if(updates, [&](BlockPosition p) { return !isFluid(chunk.at(p.x, p.y, p.z)); });
        std::sort(updates.begin(), updates.end(), [](BlockPosition a, BlockPosition b) {
            return std::tie(a.z, a.x, a.y) < std::tie(b.z, b.x, b.y);
        });
        updates.erase(std::unique(updates.begin(), updates.end()), updates.end());
    }

    const OverworldNoiseRouter& router_;
    TerrainOptions options_;
};

OverworldTerrainGenerator::OverworldTerrainGenerator(const OverworldNoiseRouter& router, TerrainOptions options)
    : impl_(std::make_unique<Impl>(router, std::move(options))) {}
OverworldTerrainGenerator::~OverworldTerrainGenerator() = default;
OverworldTerrainGenerator::OverworldTerrainGenerator(OverworldTerrainGenerator&&) noexcept = default;
OverworldTerrainGenerator& OverworldTerrainGenerator::operator=(OverworldTerrainGenerator&&) noexcept = default;

TerrainChunk OverworldTerrainGenerator::generate(int chunkX, int chunkZ) {
    return impl_->generate(chunkX, chunkZ);
}

TerrainChunk OverworldTerrainGenerator::generate(
    int chunkX, int chunkZ, TerrainGenerationProfile& profile
) {
    return impl_->generate(chunkX, chunkZ, nullptr, &profile);
}

TerrainChunk OverworldTerrainGenerator::generate(int chunkX, int chunkZ, const Beardifier& structures) {
    return impl_->generate(chunkX, chunkZ, &structures);
}

TerrainChunk OverworldTerrainGenerator::generate(
    int chunkX, int chunkZ, const Beardifier& structures, TerrainGenerationProfile& profile
) {
    return impl_->generate(chunkX, chunkZ, &structures, &profile);
}

} // namespace mcworld

namespace mcworld::detail {

void setWorldgenBlock(TerrainChunk& chunk, int x, int y, int z, Block block) {
    chunk.set(x, y, z, block);

    int& height = chunk.worldSurface[z * TerrainChunk::width + x];
    if (block != Block::Air) {
        height = std::max(height, y + 1);
    } else if (height == y + 1) {
        // Clearing the topmost block exposes whatever is below it.
        height = y;
        while (height > TerrainChunk::minY && chunk.at(x, height - 1, z) == Block::Air) {
            --height;
        }
    }
}

} // namespace mcworld::detail
