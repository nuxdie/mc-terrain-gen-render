#include "feature_placement.hpp"

#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

namespace mcworld::detail {
namespace {
float javaSin(float angle) {
    static const auto table = [] {
        std::array<float, 65536> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = static_cast<float>(std::sin(i * std::numbers::pi * 2.0 / 65536.0));
        return values;
    }();
    return table[static_cast<int>(angle * 10430.378F) & 65535];
}
bool adjacentAir(const FeatureWorld& world, int x, int y, int z) {
    return world.at(x, y - 1, z) == Block::Air || world.at(x, y + 1, z) == Block::Air
        || world.at(x, y, z - 1) == Block::Air || world.at(x, y, z + 1) == Block::Air
        || world.at(x - 1, y, z) == Block::Air || world.at(x + 1, y, z) == Block::Air;
}
} // namespace

ConfiguredFeature oreFeature(std::vector<OreTarget> targets, int size, float discardOnAir) {
    if (size < 0 || size > 64 || discardOnAir < 0 || discardOnAir > 1)
        throw std::invalid_argument("Invalid ore configuration");
    return [=](FeatureContext& c, BlockPosition origin) {
        auto& random = c.random;
        auto& world = c.world;
        const float direction = random.nextFloat() * std::numbers::pi_v<float>;
        const float spread = size / 8.0F;
        const int maxRadius = static_cast<int>(std::ceil((size / 16.0F * 2.0F + 1.0F) / 2.0F));
        const double x0 = origin.x + std::sin(static_cast<double>(direction)) * spread;
        const double x1 = origin.x - std::sin(static_cast<double>(direction)) * spread;
        const double z0 = origin.z + std::cos(static_cast<double>(direction)) * spread;
        const double z1 = origin.z - std::cos(static_cast<double>(direction)) * spread;
        const double y0 = origin.y + random.nextInt(3) - 2;
        const double y1 = origin.y + random.nextInt(3) - 2;
        const int xStart = origin.x - static_cast<int>(std::ceil(spread)) - maxRadius;
        const int yStart = origin.y - 2 - maxRadius;
        const int zStart = origin.z - static_cast<int>(std::ceil(spread)) - maxRadius;
        const int span = 2 * (static_cast<int>(std::ceil(spread)) + maxRadius);
        bool belowSurface = false;
        for (int x = xStart; x <= xStart + span && !belowSurface; ++x)
            for (int z = zStart; z <= zStart + span && !belowSurface; ++z)
                belowSurface = yStart <= world.height(FeatureHeightmap::OceanFloor, x, z);
        if (!belowSurface) return false;

        struct Sphere { double x, y, z, radius; };
        std::vector<Sphere> spheres;
        for (int i = 0; i < size; ++i) {
            const float step = static_cast<float>(i) / size;
            const double scale = random.nextDouble() * size / 16.0;
            spheres.push_back({x0 + step * (x1 - x0), y0 + step * (y1 - y0), z0 + step * (z1 - z0),
                ((javaSin(std::numbers::pi_v<float> * step) + 1.0F) * scale + 1.0) / 2.0});
        }
        for (int i = 0; i < size - 1; ++i) if (spheres[i].radius > 0) {
            for (int j = i + 1; j < size; ++j) if (spheres[j].radius > 0) {
                const double dx = spheres[i].x - spheres[j].x;
                const double dy = spheres[i].y - spheres[j].y;
                const double dz = spheres[i].z - spheres[j].z;
                const double dr = spheres[i].radius - spheres[j].radius;
                if (dr * dr > dx * dx + dy * dy + dz * dz) {
                    (dr > 0 ? spheres[j] : spheres[i]).radius = -1;
                }
            }
        }
        std::set<std::tuple<int, int, int>> tested;
        bool placed = false;
        for (const auto& s : spheres) {
            if (s.radius < 0) continue;
            const int minX = std::max(static_cast<int>(std::floor(s.x - s.radius)), xStart);
            const int minY = std::max(static_cast<int>(std::floor(s.y - s.radius)), yStart);
            const int minZ = std::max(static_cast<int>(std::floor(s.z - s.radius)), zStart);
            const int maxX = std::max(static_cast<int>(std::floor(s.x + s.radius)), minX);
            const int maxY = std::max(static_cast<int>(std::floor(s.y + s.radius)), minY);
            const int maxZ = std::max(static_cast<int>(std::floor(s.z + s.radius)), minZ);
            for (int x = minX; x <= maxX; ++x) {
                const double dx = (x + 0.5 - s.x) / s.radius;
                if (dx * dx >= 1.0) continue;
                for (int y = minY; y <= maxY; ++y) {
                    const double dy = (y + 0.5 - s.y) / s.radius;
                    if (dx * dx + dy * dy >= 1.0) continue;
                    for (int z = minZ; z <= maxZ; ++z) {
                        const double dz = (z + 0.5 - s.z) / s.radius;
                        if (dx * dx + dy * dy + dz * dz >= 1.0 || y < TerrainChunk::minY || y >= TerrainChunk::maxY) continue;
                        if (!tested.emplace(x, y, z).second || !world.canWrite({x, y, z})) continue;
                        const Block block = world.at(x, y, z);
                        for (const auto& target : targets) {
                            if (!target.matches(block)) continue;
                            const bool skipAir = discardOnAir <= 0 || (discardOnAir < 1 && random.nextFloat() >= discardOnAir);
                            if (skipAir || !adjacentAir(world, x, y, z)) {
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
    return diskFeature([block](FeatureContext&, BlockPosition) { return block; }, std::move(target), std::move(radius), halfHeight);
}

ConfiguredFeature diskFeature(std::function<Block(FeatureContext&, BlockPosition)> state, std::function<bool(Block)> target,
    IntProvider radius, int halfHeight) {
    return [=](FeatureContext& c, BlockPosition p) {
        const int r = radius(c.random);
        bool placed = false;
        for (int z = p.z - r; z <= p.z + r; ++z) for (int x = p.x - r; x <= p.x + r; ++x) {
            if ((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z) > r * r) continue;
            for (int y = p.y + halfHeight; y >= p.y - halfHeight; --y) {
                if (c.world.canWrite({x, y, z}) && target(c.world.at(x, y, z))) {
                    c.world.set(x, y, z, state(c, {x, y, z}));
                    placed = true;
                }
            }
        }
        return placed;
    };
}

ConfiguredFeature springFeature(Block fluid, std::function<bool(Block)> validBlocks, bool requiresBelow, int rocks, int holes) {
    return [=](FeatureContext& c, BlockPosition p) {
        auto& w = c.world;
        if (!w.canWrite(p) || !validBlocks(w.at(p.x, p.y + 1, p.z))) return false;
        if (requiresBelow && !validBlocks(w.at(p.x, p.y - 1, p.z))) return false;
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
