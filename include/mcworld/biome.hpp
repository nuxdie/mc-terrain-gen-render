#pragma once

// Public API for step 6 of `minecraft-26.3-worldgen.dot`: the multi-noise
// Overworld biome source.

#include "mcworld/worldgen.hpp"

#include <cstddef>
#include <cstdint>

namespace mcworld {

// The biomes the Overworld climate table can produce, in registration order:
// surface biomes first, then rivers, beaches and oceans, then the cave biomes.
enum class Biome : std::uint8_t {
    Plains, SunflowerPlains, SnowyPlains, IceSpikes, Desert, Swamp, MangroveSwamp,
    Forest, FlowerForest, BirchForest, DarkForest, PaleGarden, DappledForest,
    OldGrowthBirchForest, Taiga, SnowyTaiga, OldGrowthPineTaiga, OldGrowthSpruceTaiga,
    Savanna, SavannaPlateau, WindsweptSavanna, WindsweptHills, WindsweptForest,
    WindsweptGravellyHills, Jungle, SparseJungle, BambooJungle, Badlands,
    ErodedBadlands, WoodedBadlands, Meadow, CherryGrove, Grove, SnowySlopes,
    FrozenPeaks, JaggedPeaks, StonyPeaks, River, FrozenRiver, Beach, SnowyBeach,
    StonyShore, MushroomFields, Ocean, DeepOcean, ColdOcean, DeepColdOcean,
    FrozenOcean, DeepFrozenOcean, LukewarmOcean, DeepLukewarmOcean, WarmOcean,
    DripstoneCaves, LushCaves, SulfurCaves, DeepDark
};

// Relied on by code that indexes per-biome tables by enum value; `DeepDark` is
// the last registered biome.
constexpr std::size_t kBiomeCount = static_cast<std::size_t>(Biome::DeepDark) + 1;

// Nearest registered climate point by six-dimensional interval distance, plus
// the offset penalty. Equal distances resolve to the first registered point,
// independent of sampling history.
//
// Throws `std::invalid_argument` if the climate is not finite and in range.
[[nodiscard]] Biome resolveOverworldBiome(const RouterSample& climate);

// Inputs are block coordinates, snapped down to the quart lattice (also at
// negatives). Block-resolution lookups additionally jitter the quart cell; that
// lives with the terrain passes, which are the only callers that need it.
[[nodiscard]] Biome sampleOverworldBiome(const OverworldNoiseRouter& router, int x, int y, int z);

} // namespace mcworld
