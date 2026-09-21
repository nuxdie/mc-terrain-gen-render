#include "mcworld/worldgen.hpp"

#include "noise.hpp"
#include "spline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcworld {
namespace {

float lerp(float amount, float from, float to) {
    return from + amount * (to - from);
}

float clampedGradient(double value, double fromY, double toY, float fromValue, float toValue) {
    const float amount = static_cast<float>(std::clamp((value - fromY) / (toY - fromY), 0.0, 1.0));
    return lerp(amount, fromValue, toValue);
}

float remap(float value, float fromLow, float fromHigh, float toLow, float toHigh) {
    return toLow + (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow);
}

float mapNoise(float value, float low, float high) {
    return value * (high - low) * 0.5F + (high + low) * 0.5F;
}

float halfNegative(float value) {
    return value > 0.0F ? value : value * 0.5F;
}

float quarterNegative(float value) {
    return value > 0.0F ? value : value * 0.25F;
}

float squeeze(float value) {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    return clamped * 0.5F - clamped * clamped * clamped / 24.0F;
}

int floorToGrid(double value, int spacing) {
    const double grid = std::floor(value / spacing) * spacing;
    if (!std::isfinite(grid) || grid < std::numeric_limits<int>::min()
        || grid > std::numeric_limits<int>::max() - spacing) {
        throw std::invalid_argument("Sample coordinate is outside the supported integer grid");
    }
    return static_cast<int>(grid);
}

struct GridKey {
    int x{};
    int y{};
    int z{};

    bool operator==(const GridKey&) const = default;
};

struct GridKeyHash {
    std::size_t operator()(const GridKey& key) const noexcept {
        std::uint64_t hash = static_cast<std::uint32_t>(key.x);
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.y)) << 21U;
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z)) << 42U;
        hash ^= hash >> 30U;
        hash *= 0xbf58476d1ce4e5b9ULL;
        hash ^= hash >> 27U;
        return static_cast<std::size_t>(hash);
    }
};

struct ColumnKey {
    int x{};
    int z{};

    bool operator==(const ColumnKey&) const = default;
};

struct ColumnKeyHash {
    std::size_t operator()(const ColumnKey& key) const noexcept {
        const std::uint64_t value = static_cast<std::uint32_t>(key.x)
            | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z)) << 32U);
        return static_cast<std::size_t>(value ^ (value >> 33U));
    }
};

class IdentityBlendSampler final : public BlendSampler {};
class EmptyBeardifier final : public Beardifier {};

} // namespace

float BlendSampler::alpha(double, double, double) const {
    return 1.0F;
}

float BlendSampler::offset(double, double, double) const {
    return 0.0F;
}

float BlendSampler::applyDensity(double, double, double, float density) const {
    return density;
}

float Beardifier::sample(double, double, double) const {
    return 0.0F;
}

class OverworldNoiseRouter::Impl {
public:
    Impl(
        std::int64_t seed,
        std::shared_ptr<const BlendSampler> blender,
        std::shared_ptr<const Beardifier> beardifier
    )
        : seed_(seed),
          blender_(blender ? std::move(blender) : std::make_shared<IdentityBlendSampler>()),
          beardifier_(beardifier ? std::move(beardifier) : std::make_shared<EmptyBeardifier>()),
          splines_(detail::buildStandardOverworldSplines()),
          shift_(seed, "minecraft:offset", -3, {1, 1, 1, 0}),
          temperature_(seed, "minecraft:temperature", -10, {1.5, 0, 1, 0, 0, 0}),
          vegetation_(seed, "minecraft:vegetation", -8, {1, 1, 0, 0, 0, 0}),
          continentalness_(seed, "minecraft:continentalness", -9, {1, 1, 2, 2, 2, 1, 1, 1, 1}),
          erosion_(seed, "minecraft:erosion", -9, {1, 1, 0, 1, 1}),
          ridge_(seed, "minecraft:ridge", -7, {1, 2, 1, 0, 0, 0}),
          jagged_(seed, "minecraft:jagged", -16, std::vector<double>(16, 1.0)),
          roughness_(seed, "minecraft:spaghetti_roughness", -5, {1}),
          roughnessModulator_(seed, "minecraft:spaghetti_roughness_modulator", -8, {1}),
          spaghetti3dA_(seed, "minecraft:spaghetti_3d_1", -7, {1}),
          spaghetti3dB_(seed, "minecraft:spaghetti_3d_2", -7, {1}),
          spaghetti3dRarity_(seed, "minecraft:spaghetti_3d_rarity", -11, {1}),
          spaghetti3dThickness_(seed, "minecraft:spaghetti_3d_thickness", -8, {1}),
          caveEntrance_(seed, "minecraft:cave_entrance", -7, {0.4, 0.5, 1}),
          spaghetti2d_(seed, "minecraft:spaghetti_2d", -7, {1}),
          spaghetti2dElevation_(seed, "minecraft:spaghetti_2d_elevation", -8, {1}),
          spaghetti2dModulator_(seed, "minecraft:spaghetti_2d_modulator", -11, {1}),
          spaghetti2dThickness_(seed, "minecraft:spaghetti_2d_thickness", -11, {1}),
          caveLayer_(seed, "minecraft:cave_layer", -8, {1}),
          caveCheese_(seed, "minecraft:cave_cheese", -8, {0.5, 1, 2, 1, 2, 1, 0, 2, 0}),
          pillar_(seed, "minecraft:pillar", -7, {1, 1}),
          pillarRareness_(seed, "minecraft:pillar_rareness", -8, {1}),
          pillarThickness_(seed, "minecraft:pillar_thickness", -8, {1}),
          noodle_(seed, "minecraft:noodle", -8, {1}),
          noodleThickness_(seed, "minecraft:noodle_thickness", -8, {1}),
          noodleRidgeA_(seed, "minecraft:noodle_ridge_a", -7, {1}),
          noodleRidgeB_(seed, "minecraft:noodle_ridge_b", -7, {1}),
          blendedNoise_(seed) {}

    [[nodiscard]] std::int64_t seed() const noexcept {
        return seed_;
    }

    [[nodiscard]] RouterSample sample(double x, double y, double z) const {
        // Validate before evaluating any noise or spline with user coordinates.
        (void)floorToGrid(x, 16);
        (void)floorToGrid(y, 8);
        (void)floorToGrid(z, 16);
        const Climate climate = sampleClimate(x, z);
        const Terrain terrain = sampleTerrain(x, y, z, climate);
        RouterSample result;
        result.temperature = shiftedNoise(temperature_, x, z);
        result.vegetation = shiftedNoise(vegetation_, x, z);
        result.continentalness = climate.continentalness;
        result.erosion = climate.erosion;
        result.depth = terrain.depth;
        result.ridges = climate.weirdness;
        result.chunkSurfaceLevel = sampleChunkSurfaceLevel(x, z);
        result.finalDensity = sampleFinalDensity(x, y, z);
        return result;
    }

    [[nodiscard]] float sampleFinalDensity(double x, double y, double z) const {
        const float post = squeeze(interpolatePost(x, y, z));
        const std::array<float, 4> noodleValues = interpolateNoodle(x, y, z);
        const float noodle = noodleValues[0] >= -1000000.0F && noodleValues[0] < 0.0F
            ? 64.0F
            : noodleValues[1] + 1.5F * std::max(std::abs(noodleValues[2]), std::abs(noodleValues[3]));
        return std::min(post, noodle) + beardifier_->sample(x, y, z);
    }

private:
    struct Climate {
        float continentalness{};
        float erosion{};
        float weirdness{};
        float ridges{};
    };

    struct Terrain {
        float offset{};
        float factor{};
        float depth{};
        float jaggedness{};
        float slopedCheese{};
    };

    [[nodiscard]] float shiftedNoise(const detail::NormalNoise& noise, double x, double z) const {
        const float shiftX = 4.0F * shift_.sample(x * 0.25, 0.0, z * 0.25);
        const float shiftZ = 4.0F * shift_.sample(z * 0.25, x * 0.25, 0.0);
        return noise.sample(x * 0.25 + shiftX, 0.0, z * 0.25 + shiftZ);
    }

    [[nodiscard]] Climate sampleClimate(double x, double z) const {
        Climate climate;
        climate.continentalness = shiftedNoise(continentalness_, x, z);
        climate.erosion = shiftedNoise(erosion_, x, z);
        climate.weirdness = shiftedNoise(ridge_, x, z);
        climate.ridges = detail::peaksAndValleys(climate.weirdness);
        return climate;
    }

    [[nodiscard]] Terrain sampleTerrain(double x, double y, double z, const Climate& climate) const {
        const detail::TerrainPoint splinePoint{
            climate.continentalness,
            climate.erosion,
            climate.weirdness,
            climate.ridges,
        };
        const float alpha = blender_->alpha(x, y, z);
        const float rawOffset = -0.50375F + splines_.offset->sample(splinePoint);
        const float offset = lerp(alpha, blender_->offset(x, y, z), rawOffset);
        const float factor = lerp(alpha, 10.0F, splines_.factor->sample(splinePoint));
        const float unscaledJaggedness = lerp(alpha, 0.0F, splines_.jaggedness->sample(splinePoint));
        const float jaggedNoise = jagged_.sample(x * 1500.0, 0.0, z * 1500.0);
        const float jaggedness = unscaledJaggedness * halfNegative(jaggedNoise);
        const float depth = clampedGradient(y, -64.0, 320.0, 1.5F, -1.5F) + offset;
        const float initialDensity = 4.0F * quarterNegative(factor * (depth + jaggedness));
        const float slopedCheese = initialDensity + blendedNoise_.sample(x, y, z);
        return {offset, factor, depth, jaggedness, slopedCheese};
    }

    [[nodiscard]] float spaghettiRoughness(double x, double y, double z) const {
        const float modulator = mapNoise(roughnessModulator_.sample(x, y, z), 0.0F, -0.1F);
        return modulator * (std::abs(roughness_.sample(x, y, z)) - 0.4F);
    }

    [[nodiscard]] static float rarity3d(float value) {
        if (value < -0.5F) return 0.75F;
        if (value < 0.0F) return 1.0F;
        if (value < 0.5F) return 1.5F;
        return 2.0F;
    }

    [[nodiscard]] static float rarity2d(float value) {
        if (value < -0.75F) return 0.5F;
        if (value < -0.5F) return 0.75F;
        if (value < 0.5F) return 1.0F;
        if (value < 0.75F) return 2.0F;
        return 3.0F;
    }

    [[nodiscard]] float entrance(double x, double y, double z) const {
        const float roughness = spaghettiRoughness(x, y, z);
        const float rarity = rarity3d(spaghetti3dRarity_.sample(x * 2.0, y, z * 2.0));
        const float caveA = std::abs(rarity * spaghetti3dA_.sample(x / rarity, y / rarity, z / rarity));
        const float caveB = std::abs(rarity * spaghetti3dB_.sample(x / rarity, y / rarity, z / rarity));
        const float thickness = mapNoise(spaghetti3dThickness_.sample(x, y, z), -0.065F, -0.088F);
        const float spaghetti = std::clamp(std::max(caveA, caveB) + thickness, -1.0F, 1.0F);
        const float bigEntrance = caveEntrance_.sample(x * 0.75, y * 0.5, z * 0.75)
            + 0.37F
            + clampedGradient(y, -10.0, 30.0, 0.3F, 0.0F);
        return std::min(bigEntrance, roughness + spaghetti);
    }

    [[nodiscard]] float spaghetti2d(double x, double y, double z) const {
        const float rarity = rarity2d(spaghetti2dModulator_.sample(x * 2.0, y, z * 2.0));
        const float cave = std::abs(rarity * spaghetti2d_.sample(x / rarity, y / rarity, z / rarity));
        const float elevation = mapNoise(spaghetti2dElevation_.sample(x, 0.0, z), -8.0F, 8.0F);
        const float thickness = mapNoise(spaghetti2dThickness_.sample(x * 2.0, y, z * 2.0), -0.6F, -1.3F);
        const float slope = std::abs(elevation + clampedGradient(y, -64.0, 320.0, 8.0F, -40.0F));
        const float layer = std::pow(slope + thickness, 3.0F);
        return std::clamp(std::max(cave + 0.083F * thickness, layer), -1.0F, 1.0F);
    }

    [[nodiscard]] float pillars(double x, double y, double z) const {
        const float raw = (
            2.0F * pillar_.sample(x * 25.0, y * 0.3, z * 25.0)
            + mapNoise(pillarRareness_.sample(x, y, z), 0.0F, -2.0F)
        ) * std::pow(mapNoise(pillarThickness_.sample(x, y, z), 0.0F, 1.1F), 3.0F);
        return raw >= 0.03F ? raw : -1000000.0F;
    }

    [[nodiscard]] float underground(double x, double y, double z, float slopedCheese, float entrances) const {
        const float layerNoise = caveLayer_.sample(x, y * 8.0, z);
        const float layer = 4.0F * layerNoise * layerNoise;
        const float cheese = std::clamp(caveCheese_.sample(x, y * (2.0 / 3.0), z) + 0.27F, -1.0F, 1.0F)
            + std::clamp(1.5F - 0.64F * slopedCheese, 0.0F, 0.5F);
        const float baseCave = layer + cheese;
        const float subtraction = std::min(
            std::min(baseCave, entrances),
            spaghetti2d(x, y, z) + spaghettiRoughness(x, y, z)
        );
        return std::max(subtraction, pillars(x, y, z));
    }

    [[nodiscard]] float caves(double x, double y, double z) const {
        const Climate climate = sampleClimate(x, z);
        const Terrain terrain = sampleTerrain(x, y, z, climate);
        const float entrances = entrance(x, y, z);
        const float surface = std::min(terrain.slopedCheese, 5.0F * entrances);
        if (terrain.slopedCheese >= -1000000.0F && terrain.slopedCheese < 1.5625F) {
            return surface;
        }
        return underground(x, y, z, terrain.slopedCheese, entrances);
    }

    [[nodiscard]] float slide(double x, double y, double z) const {
        const float topFactor = clampedGradient(y, 240.0, 256.0, 1.0F, 0.0F);
        const float top = lerp(topFactor, -0.078125F, caves(x, y, z));
        const float bottomFactor = clampedGradient(y, -64.0, -40.0, 0.0F, 1.0F);
        return lerp(bottomFactor, 0.1171875F, top);
    }

    template <typename Value, typename Cache, typename Sampler>
    [[nodiscard]] Value interpolate(
        double x,
        double y,
        double z,
        int xzSpacing,
        int ySpacing,
        Cache& cache,
        Sampler&& sampler
    ) const {
        const int x0 = floorToGrid(x, xzSpacing);
        const int y0 = floorToGrid(y, ySpacing);
        const int z0 = floorToGrid(z, xzSpacing);
        const int x1 = x0 + xzSpacing;
        const int y1 = y0 + ySpacing;
        const int z1 = z0 + xzSpacing;
        const float tx = static_cast<float>((x - x0) / xzSpacing);
        const float ty = static_cast<float>((y - y0) / ySpacing);
        const float tz = static_cast<float>((z - z0) / xzSpacing);

        const auto get = [&](int sx, int sy, int sz) -> Value {
            const GridKey key{sx, sy, sz};
            const auto found = cache.find(key);
            if (found != cache.end()) {
                return found->second;
            }
            const Value sampled = sampler(sx, sy, sz);
            cache.emplace(key, sampled);
            return sampled;
        };
        const auto blendValue = [](float amount, const Value& from, const Value& to) -> Value {
            if constexpr (std::is_same_v<Value, float>) {
                return lerp(amount, from, to);
            } else {
                Value output{};
                for (std::size_t i = 0; i < output.size(); ++i) {
                    output[i] = lerp(amount, from[i], to[i]);
                }
                return output;
            }
        };

        const Value c000 = get(x0, y0, z0);
        const Value c100 = get(x1, y0, z0);
        const Value c010 = get(x0, y1, z0);
        const Value c110 = get(x1, y1, z0);
        const Value c001 = get(x0, y0, z1);
        const Value c101 = get(x1, y0, z1);
        const Value c011 = get(x0, y1, z1);
        const Value c111 = get(x1, y1, z1);
        const Value zLowYLow = blendValue(tx, c000, c100);
        const Value zLowYHigh = blendValue(tx, c010, c110);
        const Value zHighYLow = blendValue(tx, c001, c101);
        const Value zHighYHigh = blendValue(tx, c011, c111);
        return blendValue(tz, blendValue(ty, zLowYLow, zLowYHigh), blendValue(ty, zHighYLow, zHighYHigh));
    }

    [[nodiscard]] float interpolatePost(double x, double y, double z) const {
        return interpolate<float>(x, y, z, 4, 8, postCache_, [&](int sx, int sy, int sz) {
            return 0.64F * blender_->applyDensity(sx, sy, sz, slide(sx, sy, sz));
        });
    }

    [[nodiscard]] std::array<float, 4> interpolateNoodle(double x, double y, double z) const {
        return interpolate<std::array<float, 4>>(x, y, z, 4, 8, noodleCache_, [&](int sx, int sy, int sz) {
            if (sy < -60 || sy > 320) {
                return std::array<float, 4>{-1.0F, 0.0F, 0.0F, 0.0F};
            }
            return std::array<float, 4>{
                noodle_.sample(sx, sy, sz),
                mapNoise(noodleThickness_.sample(sx, sy, sz), -0.05F, -0.1F),
                noodleRidgeA_.sample(sx * (8.0 / 3.0), sy * (8.0 / 3.0), sz * (8.0 / 3.0)),
                noodleRidgeB_.sample(sx * (8.0 / 3.0), sy * (8.0 / 3.0), sz * (8.0 / 3.0)),
            };
        });
    }

    [[nodiscard]] float preliminarySurface(int x, int z) const {
        const Climate climate = sampleClimate(x, z);
        const Terrain terrainAtZero = sampleTerrain(x, 0.0, z, climate);
        const float upperRaw = 0.2734375F / terrainAtZero.factor - terrainAtZero.offset;
        const float upper = std::clamp(remap(upperRaw, 1.5F, -1.5F, -64.0F, 320.0F), -40.0F, 320.0F);
        int y = floorToGrid(upper, 8);
        for (; y >= -64; y -= 8) {
            const Terrain terrain = sampleTerrain(x, y, z, climate);
            const float gradientDensity = 4.0F * quarterNegative(terrain.factor * terrain.depth);
            const float probe = std::clamp(gradientDensity - 0.703125F, -64.0F, 64.0F);
            const float top = lerp(clampedGradient(y, 240.0, 256.0, 1.0F, 0.0F), -0.078125F, probe);
            const float slid = lerp(clampedGradient(y, -64.0, -40.0, 0.0F, 1.0F), 0.1171875F, top);
            if (slid - 0.390625F > 0.0F) {
                return static_cast<float>(y);
            }
        }
        return -64.0F;
    }

    [[nodiscard]] float sampleChunkSurfaceLevel(double x, double z) const {
        const int x0 = floorToGrid(x, 16);
        const int z0 = floorToGrid(z, 16);
        const float tx = static_cast<float>((x - x0) / 16.0);
        const float tz = static_cast<float>((z - z0) / 16.0);
        const auto get = [&](int sx, int sz) {
            const ColumnKey key{sx, sz};
            const auto found = surfaceCache_.find(key);
            if (found != surfaceCache_.end()) return found->second;
            const float value = preliminarySurface(sx, sz);
            surfaceCache_.emplace(key, value);
            return value;
        };
        return lerp(tz, lerp(tx, get(x0, z0), get(x0 + 16, z0)), lerp(tx, get(x0, z0 + 16), get(x0 + 16, z0 + 16)));
    }

    std::int64_t seed_;
    std::shared_ptr<const BlendSampler> blender_;
    std::shared_ptr<const Beardifier> beardifier_;
    detail::TerrainSplines splines_;
    detail::NormalNoise shift_;
    detail::NormalNoise temperature_;
    detail::NormalNoise vegetation_;
    detail::NormalNoise continentalness_;
    detail::NormalNoise erosion_;
    detail::NormalNoise ridge_;
    detail::NormalNoise jagged_;
    detail::NormalNoise roughness_;
    detail::NormalNoise roughnessModulator_;
    detail::NormalNoise spaghetti3dA_;
    detail::NormalNoise spaghetti3dB_;
    detail::NormalNoise spaghetti3dRarity_;
    detail::NormalNoise spaghetti3dThickness_;
    detail::NormalNoise caveEntrance_;
    detail::NormalNoise spaghetti2d_;
    detail::NormalNoise spaghetti2dElevation_;
    detail::NormalNoise spaghetti2dModulator_;
    detail::NormalNoise spaghetti2dThickness_;
    detail::NormalNoise caveLayer_;
    detail::NormalNoise caveCheese_;
    detail::NormalNoise pillar_;
    detail::NormalNoise pillarRareness_;
    detail::NormalNoise pillarThickness_;
    detail::NormalNoise noodle_;
    detail::NormalNoise noodleThickness_;
    detail::NormalNoise noodleRidgeA_;
    detail::NormalNoise noodleRidgeB_;
    detail::BlendedNoise blendedNoise_;
    mutable std::unordered_map<GridKey, float, GridKeyHash> postCache_;
    mutable std::unordered_map<GridKey, std::array<float, 4>, GridKeyHash> noodleCache_;
    mutable std::unordered_map<ColumnKey, float, ColumnKeyHash> surfaceCache_;
};

OverworldNoiseRouter::OverworldNoiseRouter(
    std::int64_t seed,
    std::shared_ptr<const BlendSampler> blender,
    std::shared_ptr<const Beardifier> beardifier
)
    : impl_(std::make_unique<Impl>(seed, std::move(blender), std::move(beardifier))) {}

OverworldNoiseRouter::~OverworldNoiseRouter() = default;
OverworldNoiseRouter::OverworldNoiseRouter(OverworldNoiseRouter&&) noexcept = default;
OverworldNoiseRouter& OverworldNoiseRouter::operator=(OverworldNoiseRouter&&) noexcept = default;

std::int64_t OverworldNoiseRouter::seed() const noexcept {
    return impl_->seed();
}

RouterSample OverworldNoiseRouter::sample(double x, double y, double z) const {
    return impl_->sample(x, y, z);
}

float OverworldNoiseRouter::sampleFinalDensity(double x, double y, double z) const {
    return impl_->sampleFinalDensity(x, y, z);
}

} // namespace mcworld
