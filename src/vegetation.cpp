// Configured features that grow things: trees, and the block piles village
// template pools place. Split from src/features.cpp only by subject; both are
// the "what it builds" half of the vocabulary in feature_placement.hpp.
//
// Both features here draw from the shared stream in a fixed order. Where a
// draw is conditional the condition is spelled out, because a draw that
// happens one time too many or too few reshapes everything downstream.

#include "feature_placement.hpp"
#include "mcworld/structure_templates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <deque>
#include <map>
#include <tuple>

namespace mcworld::detail {
namespace {

// What a trunk or a leaf may grow through. Leaves are replaceable so that two
// trees placed next to each other interleave instead of the second failing.
[[nodiscard]] bool replaceable(Block block) {
    return block == Block::Air || block == Block::Plant || isLeaves(block);
}

// What a sapling will take root on.
[[nodiscard]] bool soil(Block block) {
    return block == Block::Grass || block == Block::Dirt || block == Block::Podzol
        || block == Block::CoarseDirt || block == Block::Mycelium;
}

// Writes a block together with the full state it should carry. The `Block` is
// the render/terrain material; `data` is the state the game would store.
void namedBlock(FeatureWorld& world, BlockPosition p, const std::shared_ptr<const BlockData>& data) {
    world.set(p.x, p.y, p.z, templateMaterial(data->state));
    world.setData(p.x, p.y, p.z, data);
}

// --- Trees -----------------------------------------------------------------

[[nodiscard]] bool isConifer(TreeShape shape) {
    return shape == TreeShape::Spruce || shape == TreeShape::Pine;
}

// Java: `TrunkPlacer` and `FoliagePlacer` sizes, which the shape picks between.
struct TreeDimensions {
    int height{};         // trunk logs, from the sapling upwards
    int foliageHeight{};  // how far below the top leaves reach
    int leafRadius{};     // widest leaf ring
};

// Java: the up-front size sampling of `TreeFeature`. It happens before the
// tree is checked for room, so an attempt that cannot fit still consumes
// exactly the randomness a successful one would.
[[nodiscard]] TreeDimensions rollDimensions(TreeShape shape, WorldgenRandom& random) {
    const bool spruce = shape == TreeShape::Spruce;
    const bool pine = shape == TreeShape::Pine;
    const int base = pine ? 6 : (spruce || shape == TreeShape::Birch || shape == TreeShape::Acacia) ? 5 : 4;

    const int first = random.nextInt(pine ? 5 : 3);
    const int height = base + first + random.nextInt(shape == TreeShape::Acacia ? 3 : spruce ? 2 : 1);
    // Oak and birch take a fixed foliage depth and radius, i.e. no draw.
    const int foliageHeight = spruce ? std::max(4, height - (1 + random.nextInt(2)))
        : pine ? 3 + random.nextInt(2)
        : shape == TreeShape::Acacia ? 0 : 3;
    const int leafRadius = spruce ? 2 + random.nextInt(2)
        : pine ? 1 + random.nextInt(std::max(height - foliageHeight + 1, 1))
        : 2;
    return {height, foliageHeight, leafRadius};
}

// Java: `TwoLayersFeatureSize` plus `TreeFeature`'s free-height scan. The
// column has to be clear the whole way up, one block past the trunk, at the
// width the shape needs. Draws nothing.
[[nodiscard]] bool hasRoom(const FeatureWorld& world, BlockPosition p, int height, bool conifer, bool acacia) {
    for (int y = 0; y <= height + 1; ++y) {
        const int radius = y < (conifer ? 2 : 1) ? 0 : (conifer || acacia) ? 2 : 1;
        for (int x = -radius; x <= radius; ++x) {
            for (int z = -radius; z <= radius; ++z) {
                const BlockPosition at{p.x + x, p.y + y, p.z + z};
                const Block block = world.at(at.x, at.y, at.z);
                if (!world.canWrite(at) || (!replaceable(block) && block != Block::OakLog)) return false;
            }
        }
    }
    return true;
}

// The states a tree of one species is built from. Leaves carry their distance
// to the trunk, which is what the game decays them by; index 0 is unused so
// that the array can be indexed by that distance directly.
struct TreeStates {
    std::shared_ptr<const BlockData> log;
    std::array<std::shared_ptr<const BlockData>, 8> leaves;
};

[[nodiscard]] TreeStates treeStates(TreeShape shape) {
    const std::string species = isConifer(shape) ? "spruce" : shape == TreeShape::Birch ? "birch" : shape == TreeShape::Acacia ? "acacia" : "oak";
    TreeStates states;
    states.log = std::make_shared<BlockData>(BlockData{"minecraft:" + species + "_log[axis=y]", {}});
    for (int distance = 1; distance <= 7; ++distance) {
        states.leaves[static_cast<std::size_t>(distance)] = std::make_shared<BlockData>(BlockData{
            "minecraft:" + species + "_leaves[distance=" + std::to_string(distance)
                + ",persistent=false,waterlogged=false]", {}});
    }
    return states;
}

// Java: the `FoliagePlacer` layer loop, walking down from `offset` above the
// trunk top. Radius is recomputed per layer for the broadleaf shapes and
// carried between layers for the conifers, which is what gives spruce its
// stepped skirt and pine its single flare.
void placeFoliage(
    FeatureContext& context, BlockPosition p, TreeShape shape,
    const TreeDimensions& dimensions, const TreeStates& states
) {
    auto& random = context.random;
    auto& world = context.world;
    const bool spruce = shape == TreeShape::Spruce;
    const bool pine = shape == TreeShape::Pine;
    const bool conifer = isConifer(shape);

    // Sampled after the trunk is placed, matching FoliagePlacer's call order.
    const int offset = spruce ? random.nextInt(3) : pine ? 1 : 0;
    int radius = spruce ? random.nextInt(2) : 0;
    int maxRadius = 1;
    int minRadius = 0;
    const int bottom = spruce ? -dimensions.foliageHeight : offset - dimensions.foliageHeight;

    for (int dy = offset; dy >= bottom; --dy) {
        // Integer division truncating towards zero, as in Java; `dy` is
        // negative over most of the loop, so this is not a floor.
        if (!conifer) radius = std::max(dimensions.leafRadius - 1 - dy / 2, 0);
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                // Broadleaf corners are cut at random, which is where the
                // ragged outline comes from - and the draw only happens on a
                // corner, so the short circuit is part of the stream.
                if (conifer ? corner && radius > 0 : corner && (random.nextInt(2) == 0 || dy == 0)) continue;
                const BlockPosition at{p.x + dx, p.y + dimensions.height + dy, p.z + dz};
                if (!world.canWrite(at) || !replaceable(world.at(at.x, at.y, at.z))) continue;
                const int distance = std::clamp(std::abs(dx) + std::abs(dz) + std::max(0, dy + 1), 1, 7);
                namedBlock(world, at, states.leaves[static_cast<std::size_t>(distance)]);
            }
        }
        if (spruce) {
            // Widen a ring at a time, restarting narrow once the skirt has
            // reached `leafRadius`.
            if (radius >= maxRadius) {
                radius = minRadius;
                minRadius = 1;
                maxRadius = std::min(maxRadius + 1, dimensions.leafRadius);
            } else {
                ++radius;
            }
        } else if (pine) {
            // Widen all the way down, then pull in for the lowest layer.
            if (radius >= 1 && dy == bottom + 1) --radius;
            else if (radius < dimensions.leafRadius) ++radius;
        }
    }
}

// ForkingTrunkPlacer plus AcaciaFoliagePlacer. Horizontal directions follow
// Direction.Plane.HORIZONTAL's NORTH/EAST/SOUTH/WEST order, not enum order.
void placeAcacia(FeatureContext& c, BlockPosition origin, int height, const TreeStates& states) {
    constexpr std::array<BlockPosition, 4> directions{{{0,0,-1},{1,0,0},{0,0,1},{-1,0,0}}};
    struct Attachment { BlockPosition position; int radiusOffset; };
    std::vector<Attachment> attachments;
    const int lean = c.random.nextInt(4);
    const int leanHeight = height - c.random.nextInt(4) - 1;
    int leanSteps = 3 - c.random.nextInt(3);
    auto position = origin;
    int endY = TerrainChunk::minY;
    const auto log = [&](BlockPosition p) {
        if (!c.world.canWrite(p) || !replaceable(c.world.at(p.x, p.y, p.z))) return false;
        namedBlock(c.world, p, states.log); return true;
    };
    for (int y = 0; y < height; ++y) {
        position.y = origin.y + y;
        if (y >= leanHeight && leanSteps > 0) {
            position.x += directions[lean].x; position.z += directions[lean].z; --leanSteps;
        }
        if (log(position)) endY = position.y + 1;
    }
    if (endY != TerrainChunk::minY) attachments.push_back({{position.x, endY, position.z}, 1});
    position = origin;
    const int branch = c.random.nextInt(4);
    if (branch != lean) {
        const int branchStart = leanHeight - c.random.nextInt(2) - 1;
        int steps = 1 + c.random.nextInt(3);
        endY = TerrainChunk::minY;
        for (int y = branchStart; y < height && steps > 0; ++y, --steps) {
            if (y < 1) continue;
            position.x += directions[branch].x; position.z += directions[branch].z;
            position.y = origin.y + y;
            if (log(position)) endY = position.y + 1;
        }
        if (endY != TerrainChunk::minY) attachments.push_back({{position.x, endY, position.z}, 0});
    }

    using Key = std::tuple<int, int, int>;
    std::map<Key, int> leaves;
    for (const auto& attachment : attachments) {
        const auto row = [&](int radius, int y) {
            for (int x = -radius; x <= radius; ++x) for (int z = -radius; z <= radius; ++z) {
                const int dx = std::abs(x), dz = std::abs(z);
                if (y == 0 ? (dx > 1 || dz > 1) && dx != 0 && dz != 0 : dx == radius && dz == radius && radius > 0) continue;
                const BlockPosition p{attachment.position.x + x, attachment.position.y + y, attachment.position.z + z};
                if (!c.world.canWrite(p) || !replaceable(c.world.at(p.x, p.y, p.z))) continue;
                namedBlock(c.world, p, states.leaves[7]);
                leaves[{p.x, p.y, p.z}] = 7;
            }
        };
        row(2 + attachment.radiusOffset, -1); row(1, 0); row(1 + attachment.radiusOffset, 0);
    }
    // TreeFeature's leaf-distance propagation follows connected leaves. A
    // Manhattan distance to the root column is incorrect for a forked trunk.
    constexpr std::array<BlockPosition, 6> neighbors{{{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}}};
    std::deque<Key> pending;
    for (auto& [key, distance] : leaves) {
        const auto [x,y,z] = key;
        for (auto d : neighbors) if (c.world.at(x + d.x, y + d.y, z + d.z) == Block::OakLog) {
            distance = 1; pending.push_back(key); break;
        }
    }
    while (!pending.empty()) {
        const auto key = pending.front(); pending.pop_front();
        const int next = leaves.at(key) + 1;
        if (next >= 7) continue;
        const auto [x,y,z] = key;
        for (auto d : neighbors) {
            const Key adjacent{x + d.x, y + d.y, z + d.z};
            const auto found = leaves.find(adjacent);
            if (found != leaves.end() && found->second > next) { found->second = next; pending.push_back(adjacent); }
        }
    }
    for (const auto& [key, distance] : leaves) {
        const auto [x,y,z] = key;
        c.world.setData(x, y, z, states.leaves[distance]);
    }
}

// --- Block piles -----------------------------------------------------------

// Java: the `BlockPileFeature` configurations village pools refer to. Which
// pile it is fixed when the feature is built; the state is still rolled per
// block, because that is where the randomness belongs.
enum class PileKind { Snow, Hay, Ice, Pumpkin, Melon };

[[nodiscard]] PileKind pileKind(std::string_view name) {
    if (name == "pile_hay") return PileKind::Hay;
    if (name == "pile_ice") return PileKind::Ice;
    if (name == "pile_pumpkin") return PileKind::Pumpkin;
    if (name == "pile_melon") return PileKind::Melon;
    // Anything else, including "pile_snow", falls back to a snow layer.
    return PileKind::Snow;
}

[[nodiscard]] std::string pileState(PileKind kind, WorldgenRandom& random) {
    switch (kind) {
    case PileKind::Hay:
        return "minecraft:hay_block[axis=" + std::string(std::array{"x", "y", "z"}[random.nextInt(3)]) + "]";
    case PileKind::Ice:
        return random.nextInt(6) == 0 ? "minecraft:blue_ice" : "minecraft:packed_ice";
    case PileKind::Pumpkin:
        return random.nextInt(20) < 19 ? "minecraft:pumpkin" : "minecraft:jack_o_lantern";
    case PileKind::Melon:
        return "minecraft:melon";
    case PileKind::Snow:
        break;
    }
    return "minecraft:snow[layers=1]";
}

} // namespace

ConfiguredFeature treeFeature(TreeShape shape) {
    return [shape](FeatureContext& c, BlockPosition p) {
        // Rolled first, and unconditionally: the checks below reject the site,
        // not the draws.
        const TreeDimensions dimensions = rollDimensions(shape, c.random);

        if (p.y < TerrainChunk::minY + 1 || p.y + dimensions.height > TerrainChunk::maxY) return false;
        if (!soil(c.world.at(p.x, p.y - 1, p.z))) return false;
        if (!hasRoom(c.world, p, dimensions.height, isConifer(shape), shape == TreeShape::Acacia)) return false;

        const TreeStates states = treeStates(shape);
        // The trunk turns whatever it stands on into dirt, except where the
        // biome's own forest floor already suits.
        const Block below = c.world.at(p.x, p.y - 1, p.z);
        if (below != Block::Podzol && below != Block::Mycelium) {
            c.world.set(p.x, p.y - 1, p.z, Block::Dirt);
        }
        if (shape == TreeShape::Acacia) { placeAcacia(c, p, dimensions.height, states); return true; }
        for (int y = 0; y < dimensions.height; ++y) {
            if (replaceable(c.world.at(p.x, p.y + y, p.z))) namedBlock(c.world, {p.x, p.y + y, p.z}, states.log);
        }
        placeFoliage(c, p, shape, dimensions, states);
        return true;
    };
}

ConfiguredFeature blockPileFeature(std::string name) {
    const PileKind kind = pileKind(name);
    return [kind](FeatureContext& c, BlockPosition p) {
        if (p.y < TerrainChunk::minY + 5) return false;
        const int radiusX = 2 + c.random.nextInt(2);
        const int radiusZ = 2 + c.random.nextInt(2);
        // BlockPos.betweenClosed advances X, then Y, then Z.
        for (int z = p.z - radiusZ; z <= p.z + radiusZ; ++z) {
            for (int y = p.y; y <= p.y + 1; ++y) {
                for (int x = p.x - radiusX; x <= p.x + radiusX; ++x) {
                    const int dx = p.x - x;
                    const int dz = p.z - z;
                    // Two draws shape the blob; the third is a small chance to
                    // keep a block that fell outside it. It is only drawn when
                    // the block *is* outside, so the short circuit is part of
                    // the stream and cannot be flattened.
                    const float first = c.random.nextFloat() * 10.0F;
                    if (dx * dx + dz * dz > first - c.random.nextFloat() * 6.0F && c.random.nextFloat() >= .031) continue;
                    if (!c.world.canWrite({x, y, z}) || c.world.at(x, y, z) != Block::Air) continue;

                    // A pile thins out over a village path, and otherwise
                    // needs something solid underneath.
                    const BlockData* below = c.world.dataAt(x, y - 1, z);
                    if (below != nullptr && below->state == "minecraft:dirt_path") {
                        if (!c.random.nextBoolean()) continue;
                    } else if (!blocksMotion(c.world.at(x, y - 1, z))) {
                        continue;
                    }
                    namedBlock(c.world, {x, y, z},
                               std::make_shared<BlockData>(BlockData{pileState(kind, c.random), {}}));
                }
            }
        }
        return true;
    };
}

bool placePoolFeature(std::string_view name, FeatureContext& context, BlockPosition origin) {
    constexpr std::string_view namespacePrefix = "minecraft:";
    if (name.starts_with(namespacePrefix)) name.remove_prefix(namespacePrefix.size());

    if (name.starts_with("pile_")) return blockPileFeature(std::string(name))(context, origin);
    if (name == "oak" || name == "spruce" || name == "pine" || name == "acacia") {
        // Village placements apply sapling survival before invoking TreeFeature,
        // so the soil check happens here rather than inside the tree - and
        // before any of the tree's own draws.
        if (!soil(context.world.at(origin.x, origin.y - 1, origin.z))) return false;
        const auto shape = name == "spruce" ? TreeShape::Spruce : name == "pine" ? TreeShape::Pine : name == "acacia" ? TreeShape::Acacia : TreeShape::Oak;
        return treeFeature(shape)(context, origin);
    }
    if (name == "patch_cactus") return villagePlantPatch(VillagePlantPatch::Cactus).place(context, origin);
    if (name == "patch_berry_bush") return villagePlantPatch(VillagePlantPatch::Berries).place(context, origin);
    if (name == "patch_taiga_grass") return villagePlantPatch(VillagePlantPatch::TaigaGrass).place(context, origin);
    if (name == "flower_plain") return villagePlantPatch(VillagePlantPatch::PlainsFlowers).place(context, origin);
    // An unported feature element places nothing rather than aborting the
    // structure around it.
    return false;
}

} // namespace mcworld::detail
