#pragma once

#include "generation_internal.hpp"

#include <functional>
#include <string>

namespace mcworld::detail {

enum class FeatureHeightmap { WorldSurface, OceanFloor, MotionBlocking };

// The executor shares one random stream through modifiers and nested features.
// The top-level biome predicate is retained when a configured feature nests a
// placed feature, as in FeaturePlacer's placement context.
class FeatureWorld {
public:
    virtual ~FeatureWorld() = default;
    virtual Block at(int x, int y, int z) const = 0;
    virtual void set(int x, int y, int z, Block block) = 0;
    virtual Biome biomeAt(int x, int y, int z) const = 0;
    virtual int height(FeatureHeightmap type, int x, int z) const = 0;
    virtual bool canWrite(BlockPosition pos) const = 0;
};

struct FeatureContext {
    FeatureWorld& world;
    WorldgenRandom& random;
    std::function<bool(Biome)> biomeFilter;
};

using IntProvider = std::function<int(WorldgenRandom&)>;
using PositionPredicate = std::function<bool(const FeatureWorld&, BlockPosition)>;
using PositionConsumer = std::function<void(BlockPosition)>;
using PlacementModifier = std::function<void(FeatureContext&, BlockPosition, const PositionConsumer&)>;
using ConfiguredFeature = std::function<bool(FeatureContext&, BlockPosition)>;

struct PlacedFeature {
    ConfiguredFeature feature;
    std::vector<PlacementModifier> modifiers;
    [[nodiscard]] bool place(FeatureContext& context, BlockPosition origin) const;
};

namespace placement {
IntProvider constant(int value);
IntProvider uniform(int minimum, int maximum);
IntProvider triangle(int minimum, int maximum);
IntProvider veryBiasedToBottom(int minimum, int maximum, int inner);
IntProvider countExtra(int base, float chance, int extra);
PlacementModifier count(IntProvider provider);
PlacementModifier rarity(int chance);
PlacementModifier square();
PlacementModifier heightRange(IntProvider provider);
PlacementModifier heightmap(FeatureHeightmap type);
PlacementModifier biome();
PlacementModifier filter(PositionPredicate predicate);
PlacementModifier offset(IntProvider horizontal, IntProvider vertical);
PlacementModifier scan(int directionY, int maxSteps, PositionPredicate target, PositionPredicate allowed);
} // namespace placement

// Stable identity IDs, rather than equality of configured-feature parameters.
// Each source is a biome's ordered feature lists, indexed by decoration step.
using FeatureId = std::size_t;
using BiomeFeatureList = std::array<std::vector<FeatureId>, kDecorationStepCount>;
using FeatureOrder = std::array<std::vector<FeatureId>, kDecorationStepCount>;
[[nodiscard]] FeatureOrder sortFeatures(const std::vector<BiomeFeatureList>& sources);

struct OreTarget {
    std::function<bool(Block)> matches;
    Block block;
};
[[nodiscard]] ConfiguredFeature oreFeature(std::vector<OreTarget> targets, int size, float discardOnAir = 0);
[[nodiscard]] ConfiguredFeature diskFeature(Block block, std::function<bool(Block)> target,
                                           IntProvider radius, int halfHeight);
[[nodiscard]] ConfiguredFeature diskFeature(std::function<Block(FeatureContext&, BlockPosition)> state,
                                           std::function<bool(Block)> target, IntProvider radius, int halfHeight);
[[nodiscard]] ConfiguredFeature springFeature(Block fluid, std::function<bool(Block)> validBlocks,
                                             bool requiresBelow = true, int rocks = 4, int holes = 1);
[[nodiscard]] ConfiguredFeature sequenceFeature(std::vector<PlacedFeature> children);
[[nodiscard]] ConfiguredFeature randomSelectorFeature(std::vector<std::pair<float, PlacedFeature>> children,
                                                     PlacedFeature fallback);

} // namespace mcworld::detail
