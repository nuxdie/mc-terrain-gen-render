#pragma once

#include "mcworld/worldgen.hpp"
#include <cstdint>

namespace mcworld {

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

// Six-dimensional interval distance plus the offset penalty. Equal distances
// resolve to the first registered point, independent of sampling history.
[[nodiscard]] Biome resolveOverworldBiome(const RouterSample& climate);
// Inputs are block coordinates, snapped down to the quart lattice (also at negatives).
[[nodiscard]] Biome sampleOverworldBiome(const OverworldNoiseRouter& router, int x, int y, int z);

} // namespace mcworld
