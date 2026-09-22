#include "feature_placement.hpp"
#include "mcworld/structure_templates.hpp"

#include <cmath>

namespace mcworld::detail {
namespace {
bool replaceable(Block block) { return block == Block::Air || isLeaves(block); }
bool soil(Block block) {
    return block == Block::Grass || block == Block::Dirt || block == Block::Podzol
        || block == Block::CoarseDirt || block == Block::Mycelium;
}
void namedBlock(FeatureWorld& world, BlockPosition p, const std::shared_ptr<const BlockData>& data) {
    world.set(p.x, p.y, p.z, templateMaterial(data->state));
    world.setData(p.x, p.y, p.z, data);
}
} // namespace

ConfiguredFeature straightTreeFeature(TreeShape shape) {
    return [shape](FeatureContext& c, BlockPosition p) {
        auto& random = c.random;
        auto& world = c.world;
        const bool spruce = shape == TreeShape::Spruce, pine = shape == TreeShape::Pine;
        const bool conifer = spruce || pine;
        const int base = pine ? 6 : (spruce || shape == TreeShape::Birch) ? 5 : 4;
        const int first = random.nextInt(pine ? 5 : 3);
        const int height = base + first + random.nextInt(spruce ? 2 : 1);
        const int foliageHeight = spruce ? std::max(4, height - (1 + random.nextInt(2))) : pine ? 3 + random.nextInt(2) : 3;
        const int leafRadius = spruce ? 2 + random.nextInt(2) : pine ? 1 + random.nextInt(std::max(height - foliageHeight + 1, 1)) : 2;
        if (p.y < TerrainChunk::minY + 1 || p.y + height > TerrainChunk::maxY || !soil(world.at(p.x, p.y - 1, p.z))) return false;
        // TwoLayersFeatureSize and TreeFeature's full free-height scan.
        for (int y = 0; y <= height + 1; ++y) {
            const int radius = y < (conifer ? 2 : 1) ? 0 : conifer ? 2 : 1;
            for (int x = -radius; x <= radius; ++x) for (int z = -radius; z <= radius; ++z) {
                const BlockPosition at{p.x + x, p.y + y, p.z + z};
                if (!world.canWrite(at) || !replaceable(world.at(at.x, at.y, at.z))) return false;
            }
        }
        const std::string species = conifer ? "spruce" : shape == TreeShape::Birch ? "birch" : "oak";
        auto log = std::make_shared<BlockData>(BlockData{"minecraft:" + species + "_log[axis=y]", {}});
        std::array<std::shared_ptr<const BlockData>, 8> leaves;
        for (int distance = 1; distance <= 7; ++distance)
            leaves[distance] = std::make_shared<BlockData>(BlockData{"minecraft:" + species + "_leaves[distance=" + std::to_string(distance) + ",persistent=false,waterlogged=false]", {}});
        if (world.at(p.x, p.y - 1, p.z) != Block::Podzol && world.at(p.x, p.y - 1, p.z) != Block::Mycelium)
            world.set(p.x, p.y - 1, p.z, Block::Dirt);
        for (int y = 0; y < height; ++y) namedBlock(world, {p.x, p.y + y, p.z}, log);
        // Foliage offset is sampled after trunk placement, matching FoliagePlacer.
        const int offset = spruce ? random.nextInt(3) : pine ? 1 : 0;
        int radius = spruce ? random.nextInt(2) : 0;
        int maxRadius = 1, minRadius = 0;
        const int bottom = spruce ? -foliageHeight : offset - foliageHeight;
        for (int dy = offset; dy >= bottom; --dy) {
            if (!conifer) radius = std::max(leafRadius - 1 - dy / 2, 0);
            for (int dx = -radius; dx <= radius; ++dx) for (int dz = -radius; dz <= radius; ++dz) {
                const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                if (conifer ? corner && radius > 0 : corner && (random.nextInt(2) == 0 || dy == 0)) continue;
                const BlockPosition at{p.x + dx, p.y + height + dy, p.z + dz};
                if (world.canWrite(at) && replaceable(world.at(at.x, at.y, at.z))) {
                    const int distance = std::clamp(std::abs(dx) + std::abs(dz) + std::max(0, dy + 1), 1, 7);
                    namedBlock(world, at, leaves[distance]);
                }
            }
            if (spruce) {
                if (radius >= maxRadius) { radius = minRadius; minRadius = 1; maxRadius = std::min(maxRadius + 1, leafRadius); }
                else ++radius;
            } else if (pine) {
                if (radius >= 1 && dy == bottom + 1) --radius;
                else if (radius < leafRadius) ++radius;
            }
        }
        return true;
    };
}

ConfiguredFeature blockPileFeature(std::string name) {
    return [name = std::move(name)](FeatureContext& c, BlockPosition p) {
        if (p.y < TerrainChunk::minY + 5) return false;
        const int rx = 2 + c.random.nextInt(2), rz = 2 + c.random.nextInt(2);
        // BlockPos.betweenClosed advances X, then Y, then Z.
        for (int z = p.z - rz; z <= p.z + rz; ++z) for (int y = p.y; y <= p.y + 1; ++y) for (int x = p.x - rx; x <= p.x + rx; ++x) {
            const int dx = p.x - x, dz = p.z - z;
            const float first = c.random.nextFloat() * 10.0F;
            if (dx * dx + dz * dz > first - c.random.nextFloat() * 6.0F && c.random.nextFloat() >= .031) continue;
            if (!c.world.canWrite({x, y, z}) || c.world.at(x, y, z) != Block::Air) continue;
            const auto* below = c.world.dataAt(x, y - 1, z);
            if (below && below->state == "minecraft:dirt_path") { if (!c.random.nextBoolean()) continue; }
            else if (!blocksMotion(c.world.at(x, y - 1, z))) continue;
            std::string state;
            if (name == "pile_hay") state = "minecraft:hay_block[axis=" + std::string(std::array{"x", "y", "z"}[c.random.nextInt(3)]) + "]";
            else if (name == "pile_ice") state = c.random.nextInt(6) == 0 ? "minecraft:blue_ice" : "minecraft:packed_ice";
            else if (name == "pile_pumpkin") state = c.random.nextInt(20) < 19 ? "minecraft:pumpkin" : "minecraft:jack_o_lantern";
            else if (name == "pile_melon") state = "minecraft:melon";
            else state = "minecraft:snow[layers=1]";
            namedBlock(c.world, {x, y, z}, std::make_shared<BlockData>(BlockData{state, {}}));
        }
        return true;
    };
}

bool placePoolFeature(std::string_view name, FeatureContext& context, BlockPosition origin) {
    if (name.starts_with("minecraft:")) name.remove_prefix(10);
    if (name.starts_with("pile_")) return blockPileFeature(std::string(name))(context, origin);
    if (name == "oak" || name == "spruce" || name == "pine") {
        // Village placements apply sapling survival before invoking TreeFeature.
        if (!soil(context.world.at(origin.x, origin.y - 1, origin.z))) return false;
        const auto shape = name == "spruce" ? TreeShape::Spruce : name == "pine" ? TreeShape::Pine : TreeShape::Oak;
        return straightTreeFeature(shape)(context, origin);
    }
    return false;
}
} // namespace mcworld::detail
