#pragma once

// The vocabulary stage 8 is written in: Java's `PlacedFeature`, i.e. a
// configured feature plus the chain of placement modifiers that decides where
// it runs, and the `FeatureSorter` ordering that fixes each feature's index.
//
// Two rules hold throughout and explain most of the shapes below:
//
//  * One random stream runs through a whole placed feature - modifiers,
//    terminal positions and any nested placed feature alike - so a modifier
//    that draws must draw exactly when Java's does.
//  * Placement is depth-first, and a terminal position is never skipped for
//    being redundant: every one of them consumes randomness.
//
// The catalog that uses this vocabulary lives in src/decoration.cpp; the
// configured features themselves are in src/features.cpp (ores, disks,
// springs) and src/vegetation.cpp (trees, piles).

#include "generation_internal.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcworld::detail {

// The heightmaps a placement modifier can anchor to. A chunk keeps a fourth,
// `motionBlockingNoLeaves`, that no placement reads.
enum class FeatureHeightmap { WorldSurface, OceanFloor, MotionBlocking };

// The world as a feature sees it: block reads and writes in world coordinates,
// the live heightmaps, and the write boundary of the region it is running in.
//
// `canWrite` is advisory - a feature is expected to ask before writing outside
// its own column - while the implementation is free to treat an unasked write
// past the boundary as a hard error.
class FeatureWorld {
public:
    virtual ~FeatureWorld() = default;
    virtual Block at(int x, int y, int z) const = 0;
    virtual void set(int x, int y, int z, Block block) = 0;
    virtual Biome biomeAt(int x, int y, int z) const = 0;
    virtual int height(FeatureHeightmap type, int x, int z) const = 0;
    virtual bool canWrite(BlockPosition pos) const = 0;
    // The full block state behind a `Block`, for the features that place named
    // blocks. Worlds that do not track states keep the defaults.
    virtual void setData(int, int, int, std::shared_ptr<const BlockData>) {}
    virtual const BlockData* dataAt(int, int, int) const { return nullptr; }
};

// What flows through a placement chain. `biomeFilter` is the *top-level*
// feature's biome predicate, retained when a configured feature nests a placed
// feature, as in Java's placement context; it is empty for the structure-piece
// features, which are not registered on a biome at all.
struct FeatureContext {
    FeatureWorld& world;
    WorldgenRandom& random;
    std::function<bool(Biome)> biomeFilter;
};

using IntProvider = std::function<int(WorldgenRandom&)>;
using PositionPredicate = std::function<bool(const FeatureWorld&, BlockPosition)>;
using PositionConsumer = std::function<void(BlockPosition)>;
// A modifier receives a position and passes zero, one or many positions on. It
// is a consumer rather than a return value because `count` fans out.
using PlacementModifier = std::function<void(FeatureContext&, BlockPosition, const PositionConsumer&)>;
using ConfiguredFeature = std::function<bool(FeatureContext&, BlockPosition)>;

struct PlacedFeature {
    ConfiguredFeature feature;
    std::vector<PlacementModifier> modifiers;
    // Runs the chain from `origin`. True if any terminal position placed
    // something; the chain is walked in full either way.
    [[nodiscard]] bool place(FeatureContext& context, BlockPosition origin) const;
};

// Java's `PlacementModifier` types, with the `IntProvider`s they take.
//
// Not all of them are reachable from the catalog in src/decoration.cpp: the
// set here is the vocabulary the port supports, so that adding a feature is a
// catalog change rather than a new modifier. `offset` in particular is
// currently exercised only by tests.
namespace placement {
IntProvider constant(int value);
IntProvider uniform(int minimum, int maximum);
IntProvider triangle(int minimum, int maximum);
IntProvider veryBiasedToBottom(int minimum, int maximum, int inner);
IntProvider countExtra(int base, float chance, int extra);

PlacementModifier count(IntProvider provider);
PlacementModifier rarity(int chance);
// A random column within the 16x16 chunk the origin starts at.
PlacementModifier square();
PlacementModifier heightRange(IntProvider provider);
PlacementModifier heightmap(FeatureHeightmap type);
// Gates on the top-level feature's biome predicate; throws if there is none.
PlacementModifier biome();
PlacementModifier filter(PositionPredicate predicate);
PlacementModifier offset(IntProvider horizontal, IntProvider vertical);
// Walks up or down until `target` holds, as long as `allowed` does.
PlacementModifier scan(int directionY, int maxSteps, PositionPredicate target, PositionPredicate allowed);
} // namespace placement

// --- Feature ordering ------------------------------------------------------

// Java's `FeatureSorter`. Features are identified by a stable ID rather than by
// the parameters they were configured with, because two biomes registering the
// same feature have to agree on its index.
using FeatureId = std::size_t;
// One biome's registrations, in its own order, bucketed by decoration step.
using BiomeFeatureList = std::array<std::vector<FeatureId>, kDecorationStepCount>;
// The global order those registrations imply, per step. A feature's position
// here is its index for `WorldgenRandom::setFeatureSeed`, so this is part of
// the generated world.
using FeatureOrder = std::array<std::vector<FeatureId>, kDecorationStepCount>;

// Throws `std::invalid_argument` if the per-biome orders contradict each other,
// which in Java is a datapack error rather than something to resolve silently.
[[nodiscard]] FeatureOrder sortFeatures(const std::vector<BiomeFeatureList>& sources);

// --- Configured features ---------------------------------------------------

// One block form an ore takes, and the materials it replaces to take it. A
// blob tries the targets in order and stops at the first match.
struct OreTarget {
    std::function<bool(Block)> matches;
    Block block;
};

// Java: `OreFeature`. `size` is the blob's sphere count; `discardOnAir` is the
// chance a candidate exposed to air is skipped, which is what buries diamonds.
[[nodiscard]] ConfiguredFeature oreFeature(std::vector<OreTarget> targets, int size, float discardOnAir = 0);

// Java: `DiskFeature`. A flat cylinder of `radius`, `halfHeight` blocks either
// side of the origin, replacing blocks that `target` accepts.
[[nodiscard]] ConfiguredFeature diskFeature(Block block, std::function<bool(Block)> target,
                                           IntProvider radius, int halfHeight);
// The same, with the placed block chosen per position - sand turning to
// sandstone where it would otherwise hang over air.
[[nodiscard]] ConfiguredFeature diskFeature(std::function<Block(FeatureContext&, BlockPosition)> state,
                                           std::function<bool(Block)> target, IntProvider radius, int halfHeight);

// Java: `SpringFeature`. A fluid source in a wall, placed only where exactly
// `rocks` of the five non-top neighbours are solid and `holes` are air.
[[nodiscard]] ConfiguredFeature springFeature(Block fluid, std::function<bool(Block)> validBlocks,
                                             bool requiresBelow = true, int rocks = 4, int holes = 1);

// Java: `RandomFeatureConfiguration` and friends. Children share the parent's
// stream and biome context.
[[nodiscard]] ConfiguredFeature sequenceFeature(std::vector<PlacedFeature> children);
[[nodiscard]] ConfiguredFeature randomSelectorFeature(std::vector<std::pair<float, PlacedFeature>> children,
                                                     PlacedFeature fallback);

enum class TreeShape { Oak, Birch, Spruce, Pine };
// Java: `TreeFeature` with a straight trunk. One shape stands in for each
// species family; see README for that boundary.
[[nodiscard]] ConfiguredFeature straightTreeFeature(TreeShape shape);

// Java: `BlockPileFeature`. `name` selects the pile's material, e.g. "pile_hay".
[[nodiscard]] ConfiguredFeature blockPileFeature(std::string name);

// Runs a template pool's feature element by name, with or without a
// `minecraft:` prefix. Unknown names place nothing and return false, which is
// how an unported feature element degrades rather than aborting a structure.
[[nodiscard]] bool placePoolFeature(std::string_view name, FeatureContext& context, BlockPosition origin);

} // namespace mcworld::detail
