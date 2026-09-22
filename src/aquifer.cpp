// Aquifers: the `create_aquifer` / `aquifer_substance` nodes of `7B` in
// `minecraft-26.3-worldgen.dot`.
//
// Java: `Aquifer.NoiseBasedAquifer`. Wherever the density graph leaves a
// position open, the aquifer decides whether it becomes air, water or lava, and
// whether a barrier of stone should hold two neighbouring fluid bodies apart.
//
// The world is divided into a coarse grid; each cell has one jittered center
// block, and that center's fluid speaks for the cell. A position looks at the
// four nearest centers: the nearest one supplies the block, and the runners-up
// decide whether pressure between differing fluid levels seals the gap with
// stone instead.

#include "terrain_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mcworld::detail {
namespace {

// Java: `DimensionType.WAY_BELOW_MIN_Y`. The "this aquifer has no fluid at all"
// sentinel, far enough below the world that no column can reach it.
constexpr int kNoFluid = -32512;

// Below this Y the global fluid is lava rather than water.
constexpr int kGlobalLavaLevel = -54;

// Grid cell size of the aquifer lattice, and the jitter applied to a cell's
// center. The lattice is offset by five blocks horizontally and one vertically
// when a position looks up its cell, which is what breaks up the grid.
constexpr int kCellWidth = 16;
constexpr int kCellHeight = 12;
constexpr int kCenterJitterXZ = 10;
constexpr int kCenterJitterY = 9;
constexpr int kCellOffsetXZ = 5;
constexpr int kCellOffsetY = 1;

// Vertical band the fluid-level spread noise is evaluated in, and the step the
// resulting level snaps to.
constexpr int kFluidLevelBandHeight = 40;
constexpr int kFluidLevelStep = 3;

// Squared-distance window over which two centers count as comparably close.
constexpr double kSimilarityScale = 25.0;

// Below this similarity two differing fluids no longer interact at all, so no
// fluid update needs scheduling.
constexpr double kFlowThreshold = -0.76;

// A position this far above the preliminary surface is plain air: no aquifer.
constexpr int kSurfaceClearance = 12;
constexpr int kSurfaceSlack = 8;

// The columns whose preliminary surface a fluid level consults, in chunks
// relative to the center's own column. Java: `Aquifer.SURFACE_SAMPLING_OFFSETS`.
constexpr int kSurfaceSamplingOffsets[][2]{
    {0, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
    {-1, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};

} // namespace

Aquifer::Aquifer(const OverworldNoiseRouter& router, bool enabled)
    : router_(router),
      enabled_(enabled),
      barrier_(router.seed(), "minecraft:aquifer_barrier", -3, {1}),
      floodedness_(router.seed(), "minecraft:aquifer_fluid_level_floodedness", -7, {1}),
      spread_(router.seed(), "minecraft:aquifer_fluid_level_spread", -5, {1}),
      lava_(router.seed(), "minecraft:aquifer_lava", -1, {1}) {}

int Aquifer::surface(int x, int z) {
    x = floorDiv(x, kQuartSize) * kQuartSize;
    z = floorDiv(z, kQuartSize) * kQuartSize;
    auto [entry, inserted] = surfaces_.try_emplace({x, z}, 0);
    if (inserted) {
        entry->second = static_cast<int>(std::floor(router_.samplePreliminarySurface(x, z)));
    }
    return entry->second;
}

Aquifer::Fluid Aquifer::fluid(int x, int y, int z) {
    const Fluid global = y < kGlobalLavaLevel ? Fluid{kGlobalLavaLevel, Block::Lava} : Fluid{kSeaLevel, Block::Water};

    // Look at the surrounding columns: high above all of them there is no
    // aquifer, and a column whose own surface is submerged forces the global
    // fluid straight through.
    int lowestSurface = std::numeric_limits<int>::max();
    bool submerged = false;
    for (const auto& offset : kSurfaceSamplingOffsets) {
        const int level = surface(x + offset[0] * kCellWidth, z + offset[1] * kCellWidth);
        const int adjusted = level + kSurfaceSlack;
        const bool isOwnColumn = offset[0] == 0 && offset[1] == 0;
        if (isOwnColumn && y - kSurfaceClearance > adjusted) {
            return global;
        }
        if (y + kSurfaceClearance > adjusted || isOwnColumn) {
            const Fluid atSurface = adjusted < kGlobalLavaLevel ? Fluid{kGlobalLavaLevel, Block::Lava}
                                                               : Fluid{kSeaLevel, Block::Water};
            if (atSurface.at(adjusted) != Block::Air) {
                if (isOwnColumn) {
                    submerged = true;
                }
                if (y + kSurfaceClearance > adjusted) {
                    return atSurface;
                }
            }
        }
        lowestSurface = std::min(lowestSurface, level);
    }

    // Deep, heavily eroded terrain is excluded from aquifers entirely.
    const RouterSample climate = router_.sample(x, y, z);
    const bool excluded = climate.erosion < -.225F && climate.depth > .9F;

    int level = kNoFluid;
    if (!excluded) {
        // Columns under water flood more readily, and more so near the surface.
        const double factor = submerged ? 1.0 - std::clamp((lowestSurface + kSurfaceSlack - y) / 64.0, 0.0, 1.0) : 0.0;
        const double floodedness = std::clamp(static_cast<double>(floodedness_.sample(x, y * .67, z)), -1.0, 1.0);
        // Java: `Mth.map(factor, 1, 0, low, high)`; preserve its evaluation order.
        if (floodedness > -.3 + (1.0 - factor) * (.8 - (-.3))) {
            level = global.level;
        } else if (floodedness > -.8 + (1.0 - factor) * (.4 - (-.8))) {
            const int band = floorDiv(y, kFluidLevelBandHeight);
            const float spread = spread_.sample(floorDiv(x, kCellWidth), band * 0.7142857142857143, floorDiv(z, kCellWidth)) * 10.0F;
            const int spreadSteps = static_cast<int>(std::floor(spread / 3.0)) * kFluidLevelStep;
            level = std::min(lowestSurface, band * kFluidLevelBandHeight + 20 + spreadSteps);
        }
    }

    // Deep aquifers that are not already lava can turn into lava.
    Block type = global.type;
    const bool deepEnough = level <= -10 && level != kNoFluid && type != Block::Lava;
    if (deepEnough && std::abs(lava_.sample(floorDiv(x, 64), floorDiv(y, kFluidLevelBandHeight), floorDiv(z, 64))) > .3) {
        type = Block::Lava;
    }
    return {level, type};
}

const Aquifer::Center& Aquifer::center(int x, int y, int z) {
    const auto key = std::tuple{x, y, z};
    if (const auto found = centers_.find(key); found != centers_.end()) {
        return found->second;
    }
    LegacyRandom random(positionalSeed(router_.seed(), "minecraft:aquifer", x, y, z));
    const int blockX = x * kCellWidth + random.nextInt(kCenterJitterXZ);
    const int blockY = y * kCellHeight + random.nextInt(kCenterJitterY);
    const int blockZ = z * kCellWidth + random.nextInt(kCenterJitterXZ);
    return centers_.emplace(key, Center{blockX, blockY, blockZ, fluid(blockX, blockY, blockZ)}).first->second;
}

double Aquifer::pressure(int x, int y, int z, Fluid a, Fluid b, double& noise) {
    // Water meeting lava always seals: that is where the stone shells around
    // underground lava lakes come from.
    const Block typeA = a.at(y);
    const Block typeB = b.at(y);
    if ((typeA == Block::Water && typeB == Block::Lava) || (typeA == Block::Lava && typeB == Block::Water)) {
        return 2;
    }

    const int levelDistance = std::abs(a.level - b.level);
    if (levelDistance == 0) {
        return 0;
    }

    // How far the position sits above the midpoint between the two levels, and
    // how deep inside the overlap of the two bodies it is. Above the midpoint
    // the barrier is thin; below it, it is thicker and shifted down by three.
    const double aboveMidpoint = y + .5 - .5 * (a.level + b.level);
    double edge = levelDistance / 2.0 - std::abs(aboveMidpoint);
    double gradient = 0;
    if (aboveMidpoint > 0) {
        gradient = edge / (edge > 0 ? 1.5 : 2.5);
    } else {
        edge += 3;
        gradient = edge / (edge > 0 ? 3 : 10);
    }

    // Only near the transition is the barrier noise worth sampling; `noise`
    // caches it across the (up to three) pressure tests of one position.
    double barrier = 0;
    if (gradient >= -2 && gradient <= 2) {
        if (std::isnan(noise)) {
            noise = barrier_.sample(x, y * .5, z);
        }
        barrier = noise;
    }
    return 2 * (gradient + barrier);
}

Substance Aquifer::sample(int x, int y, int z, float density) {
    if (density > 0) {
        return {Block::Stone};
    }
    if (y < kGlobalLavaLevel) {
        return {Block::Lava};
    }
    if (!enabled_) {
        return {y < kSeaLevel ? Block::Water : Block::Air};
    }

    // The four nearest cell centers, nearest first. Equal distances keep the
    // earlier candidate, so the scan order over the 2x3x2 neighbourhood is part
    // of the result.
    std::array<const Center*, 4> nearest{};
    std::array<int, 4> distances{};
    distances.fill(std::numeric_limits<int>::max());
    for (int dx = 0; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = 0; dz <= 1; ++dz) {
                const Center& candidate = center(floorDiv(x - kCellOffsetXZ, kCellWidth) + dx,
                                                 floorDiv(y + kCellOffsetY, kCellHeight) + dy,
                                                 floorDiv(z - kCellOffsetXZ, kCellWidth) + dz);
                const int distance = (x - candidate.x) * (x - candidate.x) + (y - candidate.y) * (y - candidate.y) +
                                     (z - candidate.z) * (z - candidate.z);
                for (int rank = 0; rank < 4; ++rank) {
                    if (distance <= distances[rank]) {
                        for (int shift = 3; shift > rank; --shift) {
                            distances[shift] = distances[shift - 1];
                            nearest[shift] = nearest[shift - 1];
                        }
                        distances[rank] = distance;
                        nearest[rank] = &candidate;
                        break;
                    }
                }
            }
        }
    }

    // How comparable two of the ranked centers are: 1 when equally close,
    // falling below 0 once the second is clearly further away.
    auto similarity = [&](int a, int b) { return 1.0 - (distances[b] - distances[a]) / kSimilarityScale; };
    auto differs = [&](int a, int b) { return nearest[a]->fluid != nearest[b]->fluid; };

    const Block block = nearest[0]->fluid.at(y);
    const double firstToSecond = similarity(0, 1);

    // Deep inside one aquifer nothing can hold the fluid back.
    if (firstToSecond <= 0) {
        return {block, firstToSecond >= kFlowThreshold && differs(0, 1)};
    }
    // The top block of global lava always gets a fluid update.
    if (block == Block::Water && y == kGlobalLavaLevel) {
        return {block, true};
    }

    // Near a boundary, pressure between the ranked pairs can seal the position
    // with stone. `barrierNoise` is sampled at most once across the three tests.
    double barrierNoise = std::numeric_limits<double>::quiet_NaN();
    if (density + firstToSecond * pressure(x, y, z, nearest[0]->fluid, nearest[1]->fluid, barrierNoise) > 0) {
        return {Block::Stone};
    }
    const double firstToThird = similarity(0, 2);
    const double secondToThird = similarity(1, 2);
    if (firstToThird > 0 &&
        density + firstToSecond * firstToThird * pressure(x, y, z, nearest[0]->fluid, nearest[2]->fluid, barrierNoise) > 0) {
        return {Block::Stone};
    }
    if (secondToThird > 0 &&
        density + firstToSecond * secondToThird * pressure(x, y, z, nearest[1]->fluid, nearest[2]->fluid, barrierNoise) > 0) {
        return {Block::Stone};
    }

    // Open fluid: schedule an update wherever a differing body is close enough
    // to flow in.
    const bool schedule = differs(0, 1) || (secondToThird >= kFlowThreshold && differs(1, 2)) ||
                          (firstToThird >= kFlowThreshold && differs(0, 2)) ||
                          (firstToThird >= kFlowThreshold && similarity(0, 3) >= kFlowThreshold && differs(0, 3));
    return {block, schedule};
}

} // namespace mcworld::detail
