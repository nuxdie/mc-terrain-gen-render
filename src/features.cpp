// Configured features that shape stone: ore blobs, the disks that line
// shorelines and lake beds, and fluid springs. The features that grow things
// are in src/vegetation.cpp; the placement vocabulary both halves are written
// in is feature_placement.hpp.
//
// These are ports rather than reimplementations. Where an expression looks
// like it could be simplified, it usually cannot: the draw count, the
// float/double split and the iteration order are all observable in the world.

#include "feature_placement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace mcworld::detail {
namespace {

// Java's `Mth.sin`: a 65536-entry lookup table, not `std::sin`. Ore blob radii
// come out of it, so the difference between the table and the real sine is
// part of every ore vein's shape.
[[nodiscard]] float javaSin(float angle) {
    static const auto table = [] {
        std::array<float, 65536> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<float>(std::sin(static_cast<double>(i) * std::numbers::pi * 2.0 / 65536.0));
        }
        return values;
    }();
    return table[static_cast<int>(angle * 10430.378F) & 65535];
}

// Whether a position touches air on any of its six sides. Used by the ores
// that are meant not to be visible in a cave wall.
[[nodiscard]] bool adjacentAir(const FeatureWorld& world, int x, int y, int z) {
    return world.at(x, y - 1, z) == Block::Air || world.at(x, y + 1, z) == Block::Air
        || world.at(x, y, z - 1) == Block::Air || world.at(x, y, z + 1) == Block::Air
        || world.at(x - 1, y, z) == Block::Air || world.at(x + 1, y, z) == Block::Air;
}

// --- Ore blobs -------------------------------------------------------------

// One sphere of an ore blob. A negative radius marks a sphere that another
// sphere swallowed and that is therefore not filled.
struct OreSphere {
    double x{};
    double y{};
    double z{};
    double radius{};
};

// The axis-aligned box the blob is generated in, in world coordinates. The
// span is deliberately the same on all three axes: Java derives one extent and
// reuses it, and the pre-scan below depends on that.
struct OreExtent {
    int startX{};
    int startY{};
    int startZ{};
    int span{};
};

// The line segment a blob is sampled along. Its horizontal endpoints follow
// from the direction, but both Y endpoints are drawn - and Java draws them
// *before* the surface guard rejects the blob, so they cannot be folded into
// the sphere loop below.
struct OreAxis {
    double x0{};
    double y0{};
    double z0{};
    double x1{};
    double y1{};
    double z1{};
};

[[nodiscard]] OreAxis rollAxis(WorldgenRandom& random, BlockPosition origin, int size, float direction) {
    const float spread = size / 8.0F;
    OreAxis axis;
    axis.x0 = origin.x + std::sin(static_cast<double>(direction)) * spread;
    axis.x1 = origin.x - std::sin(static_cast<double>(direction)) * spread;
    axis.z0 = origin.z + std::cos(static_cast<double>(direction)) * spread;
    axis.z1 = origin.z - std::cos(static_cast<double>(direction)) * spread;
    axis.y0 = origin.y + random.nextInt(3) - 2;
    axis.y1 = origin.y + random.nextInt(3) - 2;
    return axis;
}

// Java: `OreFeature.doPlace`. A sphere is sampled at each step along the axis,
// fattest in the middle because the radius is scaled by a half sine.
[[nodiscard]] std::vector<OreSphere> buildSpheres(WorldgenRandom& random, const OreAxis& axis, int size) {
    std::vector<OreSphere> spheres;
    spheres.reserve(static_cast<std::size_t>(std::max(size, 0)));
    for (int i = 0; i < size; ++i) {
        const float step = static_cast<float>(i) / size;
        const double scale = random.nextDouble() * size / 16.0;
        spheres.push_back({axis.x0 + step * (axis.x1 - axis.x0),
                           axis.y0 + step * (axis.y1 - axis.y0),
                           axis.z0 + step * (axis.z1 - axis.z0),
                           ((javaSin(std::numbers::pi_v<float> * step) + 1.0F) * scale + 1.0) / 2.0});
    }
    return spheres;
}

// Java's overlap pass: wherever one sphere entirely contains another, the
// contained one is dropped, so its blocks are not tested twice. Draws nothing.
void absorbContainedSpheres(std::vector<OreSphere>& spheres) {
    const int size = static_cast<int>(spheres.size());
    for (int i = 0; i < size - 1; ++i) {
        if (spheres[i].radius <= 0) continue;
        for (int j = i + 1; j < size; ++j) {
            if (spheres[j].radius <= 0) continue;
            const double dx = spheres[i].x - spheres[j].x;
            const double dy = spheres[i].y - spheres[j].y;
            const double dz = spheres[i].z - spheres[j].z;
            const double dr = spheres[i].radius - spheres[j].radius;
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                // The smaller of the two is the one inside.
                (dr > 0 ? spheres[j] : spheres[i]).radius = -1;
            }
        }
    }
}

// Java: the `OreFeature` guard that stops a blob generating in open air above
// the terrain. True if any column of the extent has its ocean floor at or
// above the blob's lowest block.
[[nodiscard]] bool anyColumnCoversBlob(const FeatureWorld& world, const OreExtent& extent) {
    for (int x = extent.startX; x <= extent.startX + extent.span; ++x) {
        for (int z = extent.startZ; z <= extent.startZ + extent.span; ++z) {
            if (extent.startY <= world.height(FeatureHeightmap::OceanFloor, x, z)) return true;
        }
    }
    return false;
}

} // namespace

ConfiguredFeature oreFeature(std::vector<OreTarget> targets, int size, float discardOnAir) {
    if (size < 0 || size > 64 || discardOnAir < 0 || discardOnAir > 1) {
        throw std::invalid_argument("Invalid ore configuration");
    }
    return [targets = std::move(targets), size, discardOnAir](FeatureContext& c, BlockPosition origin) {
        auto& random = c.random;
        auto& world = c.world;

        // Direction and axis are drawn up front, and are consumed even by a
        // blob the surface guard then rejects. The sphere draws are not.
        const float direction = random.nextFloat() * std::numbers::pi_v<float>;
        const OreAxis axis = rollAxis(random, origin, size, direction);

        const float spread = size / 8.0F;
        const int maxRadius = static_cast<int>(std::ceil((size / 16.0F * 2.0F + 1.0F) / 2.0F));
        const int reach = static_cast<int>(std::ceil(spread)) + maxRadius;
        const OreExtent extent{origin.x - reach, origin.y - 2 - maxRadius, origin.z - reach, 2 * reach};
        if (!anyColumnCoversBlob(world, extent)) return false;

        std::vector<OreSphere> spheres = buildSpheres(random, axis, size);
        absorbContainedSpheres(spheres);

        // Spheres overlap, and Java tests each position once across the whole
        // blob rather than once per sphere - which matters because a test can
        // consume a draw.
        std::set<std::tuple<int, int, int>> tested;
        bool placed = false;
        for (const OreSphere& sphere : spheres) {
            if (sphere.radius < 0) continue;
            const int minX = std::max(static_cast<int>(std::floor(sphere.x - sphere.radius)), extent.startX);
            const int minY = std::max(static_cast<int>(std::floor(sphere.y - sphere.radius)), extent.startY);
            const int minZ = std::max(static_cast<int>(std::floor(sphere.z - sphere.radius)), extent.startZ);
            const int maxX = std::max(static_cast<int>(std::floor(sphere.x + sphere.radius)), minX);
            const int maxY = std::max(static_cast<int>(std::floor(sphere.y + sphere.radius)), minY);
            const int maxZ = std::max(static_cast<int>(std::floor(sphere.z + sphere.radius)), minZ);

            for (int x = minX; x <= maxX; ++x) {
                // The squared-distance test is unrolled per axis so that a
                // whole slab can be skipped as soon as one axis is outside.
                const double dx = (x + 0.5 - sphere.x) / sphere.radius;
                if (dx * dx >= 1.0) continue;
                for (int y = minY; y <= maxY; ++y) {
                    const double dy = (y + 0.5 - sphere.y) / sphere.radius;
                    if (dx * dx + dy * dy >= 1.0) continue;
                    for (int z = minZ; z <= maxZ; ++z) {
                        const double dz = (z + 0.5 - sphere.z) / sphere.radius;
                        if (dx * dx + dy * dy + dz * dz >= 1.0 || y < TerrainChunk::minY || y >= TerrainChunk::maxY) continue;
                        if (!tested.emplace(x, y, z).second || !world.canWrite({x, y, z})) continue;

                        const Block block = world.at(x, y, z);
                        for (const OreTarget& target : targets) {
                            if (!target.matches(block)) continue;
                            // `discardOnAir` is the chance an air-exposed
                            // candidate is dropped. The draw happens only for
                            // a partial chance: 0 never checks exposure and 1
                            // always does, neither consuming randomness.
                            const bool ignoreExposure = discardOnAir <= 0
                                || (discardOnAir < 1 && random.nextFloat() >= discardOnAir);
                            if (ignoreExposure || !adjacentAir(world, x, y, z)) {
                                world.set(x, y, z, target.block);
                                placed = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        return placed;
    };
}

ConfiguredFeature diskFeature(Block block, std::function<bool(Block)> target, IntProvider radius, int halfHeight) {
    return diskFeature([block](FeatureContext&, BlockPosition) { return block; },
                       std::move(target), std::move(radius), halfHeight);
}

// Java: `DiskFeature`. One radius draw, then a solid cylinder scanned top-down
// so that `state` sees the column as the disk has already changed it.
ConfiguredFeature diskFeature(std::function<Block(FeatureContext&, BlockPosition)> state,
                              std::function<bool(Block)> target, IntProvider radius, int halfHeight) {
    return [state = std::move(state), target = std::move(target), radius = std::move(radius), halfHeight](
               FeatureContext& c, BlockPosition p) {
        const int r = radius(c.random);
        bool placed = false;
        for (int z = p.z - r; z <= p.z + r; ++z) {
            for (int x = p.x - r; x <= p.x + r; ++x) {
                if ((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z) > r * r) continue;
                for (int y = p.y + halfHeight; y >= p.y - halfHeight; --y) {
                    if (c.world.canWrite({x, y, z}) && target(c.world.at(x, y, z))) {
                        c.world.set(x, y, z, state(c, {x, y, z}));
                        placed = true;
                    }
                }
            }
        }
        return placed;
    };
}

// Java: `SpringFeature`. A source block in a pocket of rock: the block above
// (and, for water, below) has to be rock, and of the five neighbours that are
// not the ceiling exactly `rocks` are rock and `holes` are air - which is what
// makes a spring pour out of a wall rather than sit in open ground.
ConfiguredFeature springFeature(Block fluid, std::function<bool(Block)> validBlocks, bool requiresBelow,
                                int rocks, int holes) {
    return [fluid, validBlocks = std::move(validBlocks), requiresBelow, rocks, holes](
               FeatureContext& c, BlockPosition p) {
        auto& w = c.world;
        if (!w.canWrite(p) || !validBlocks(w.at(p.x, p.y + 1, p.z))) return false;
        if (requiresBelow && !validBlocks(w.at(p.x, p.y - 1, p.z))) return false;
        // The source may replace air or rock, but never another fluid.
        const Block current = w.at(p.x, p.y, p.z);
        if (current != Block::Air && !validBlocks(current)) return false;

        const std::array neighbors{w.at(p.x - 1, p.y, p.z), w.at(p.x + 1, p.y, p.z),
            w.at(p.x, p.y, p.z - 1), w.at(p.x, p.y, p.z + 1), w.at(p.x, p.y - 1, p.z)};
        if (std::count_if(neighbors.begin(), neighbors.end(), validBlocks) != rocks
            || std::count(neighbors.begin(), neighbors.end(), Block::Air) != holes) return false;

        w.set(p.x, p.y, p.z, fluid);
        return true;
    };
}

} // namespace mcworld::detail
