// Step 7A of `minecraft-26.3-worldgen.dot`: the standard Overworld
// NoiseRouter and final-density graph.
//
// The function names below follow the diagram's node names, and the code is
// ordered to match the edges:
//
//   keyed noises -> shift -> climate -> terrain splines -> offset/factor/
//   jaggedness -> initial density -> sloped cheese -> {entrances, underground}
//   -> cave choice -> slides -> interpolate + squeeze -> min(.., noodle)
//   -> + beardifier -> final density
//
// Every literal in this file comes from the vanilla Overworld NoiseSettings.
// They are named rather than inlined because the only way to audit this port
// against the Java source is constant by constant. Arithmetic is written to
// match Java's evaluation order: these are floats, so re-associating a product
// or widening an intermediate to double silently changes every world. If you
// touch an expression here, re-run the golden-output comparison.

#include "mcworld/worldgen.hpp"

#include "density_math.hpp"
#include "noise.hpp"
#include "spline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcworld {
namespace {

using detail::clampedGradient;
using detail::halfNegative;
using detail::lerp;
using detail::mapNoise;
using detail::quarterNegative;
using detail::remap;
using detail::squeeze;

// --- World geometry --------------------------------------------------------

constexpr double kWorldMinY = -64.0;
constexpr double kWorldMaxY = 320.0;

// The density graph is evaluated on a coarse lattice and interpolated: 4 blocks
// horizontally by 8 vertically, matching NoiseChunk's cell size. Preliminary
// surface levels are computed per 16x16 column and interpolated the same way.
constexpr int kCellWidth = 4;
constexpr int kCellHeight = 8;
constexpr int kColumnSpacing = 16;

// --- Range choice ----------------------------------------------------------

// Java's DensityFunctions.rangeChoice uses an explicit lower bound of -1e6 to
// mean "effectively unbounded below". Both uses in this graph keep that bound
// verbatim, so a NaN input falls through to the out-of-range branch.
constexpr float kRangeChoiceFloor = -1000000.0F;

[[nodiscard]] bool inRangeChoice(float value, float exclusiveMax) {
    return value >= kRangeChoiceFloor && value < exclusiveMax;
}

// --- Climate ---------------------------------------------------------------

// Climate noises are sampled at quarter resolution, at a position displaced by
// the shift noise, which is what breaks up the otherwise axis-aligned biome
// bands. Java: ShiftedNoise with shiftX/shiftZ and xzScale = 0.25.
constexpr double kClimateScale = 0.25;
constexpr float kShiftMultiplier = 4.0F;

// --- Terrain: offset, factor, jaggedness -----------------------------------

// Sea level sits at density zero, so the offset spline is biased down by
// slightly more than half. Java: add(-0.50375, offsetSpline).
constexpr float kOffsetBias = -0.50375F;

// Depth ramps linearly from solid at the bottom of the world to air at the top,
// before the offset spline displaces it.
constexpr float kDepthAtWorldBottom = 1.5F;
constexpr float kDepthAtWorldTop = -1.5F;

// Where terrain blending is inactive (alpha = 1) these are the values the
// blended-in side contributes; they only matter for upgraded worlds.
constexpr float kUnblendedFactor = 10.0F;
constexpr float kUnblendedJaggedness = 0.0F;

// Jaggedness noise is sampled at very high frequency so it varies block to
// block, and only its positive half is applied at full strength.
constexpr double kJaggedNoiseScale = 1500.0;

// Java: noiseGradientDensity(factor, depth) = 4 * quarterNegative(factor*depth).
constexpr float kGradientDensityScale = 4.0F;

[[nodiscard]] float noiseGradientDensity(float factor, float depth) {
    return kGradientDensityScale * quarterNegative(factor * depth);
}

// --- Spaghetti roughness ---------------------------------------------------

constexpr float kRoughnessModulatorLow = 0.0F;
constexpr float kRoughnessModulatorHigh = -0.1F;
constexpr float kRoughnessBias = -0.4F;

// --- Cave entrances (surface branch) ---------------------------------------

constexpr double kSpaghetti3dRarityScaleXz = 2.0;
constexpr float kSpaghetti3dThicknessLow = -0.065F;
constexpr float kSpaghetti3dThicknessHigh = -0.088F;
constexpr double kCaveEntranceScaleXz = 0.75;
constexpr double kCaveEntranceScaleY = 0.5;
constexpr float kCaveEntranceBias = 0.37F;
// Entrances are widened near the surface so cave mouths actually break through.
constexpr double kEntranceTaperFromY = -10.0;
constexpr double kEntranceTaperToY = 30.0;
constexpr float kEntranceTaperAmount = 0.3F;

// --- Spaghetti 2-D (underground branch) ------------------------------------

constexpr double kSpaghetti2dRarityScaleXz = 2.0;
constexpr float kSpaghetti2dElevationLow = -8.0F;
constexpr float kSpaghetti2dElevationHigh = 8.0F;
constexpr float kSpaghetti2dThicknessLow = -0.6F;
constexpr float kSpaghetti2dThicknessHigh = -1.3F;
// The 2-D tunnels follow a sloping plane through the world rather than a
// constant Y, which is what gives them their long horizontal runs.
constexpr float kSpaghetti2dSlopeAtBottom = 8.0F;
constexpr float kSpaghetti2dSlopeAtTop = -40.0F;
constexpr float kSpaghetti2dThicknessWeight = 0.083F;

// --- Pillars ---------------------------------------------------------------

constexpr double kPillarScaleXz = 25.0;
constexpr double kPillarScaleY = 0.3;
constexpr float kPillarRarenessLow = 0.0F;
constexpr float kPillarRarenessHigh = -2.0F;
constexpr float kPillarThicknessLow = 0.0F;
constexpr float kPillarThicknessHigh = 1.1F;
constexpr float kPillarScale = 2.0F;
// Below this the pillar branch drops out entirely instead of denting the cave.
constexpr float kPillarCutoff = 0.03F;

// --- Cave layers and cheese ------------------------------------------------

constexpr double kCaveLayerScaleY = 8.0;
constexpr float kCaveLayerScale = 4.0F;
constexpr double kCaveCheeseScaleY = 2.0 / 3.0;
constexpr float kCaveCheeseBias = 0.27F;
// Cheese caves are suppressed as terrain thins out towards the surface.
constexpr float kCheeseSurfaceGuardBias = 1.5F;
constexpr float kCheeseSurfaceGuardSlope = 0.64F;
constexpr float kCheeseSurfaceGuardMax = 0.5F;

// --- Cave choice -----------------------------------------------------------

// Above this sloped-cheese value the column is deep enough inside terrain to
// use the full underground cave graph; below it only entrances are carved.
constexpr float kUndergroundThreshold = 1.5625F;
constexpr float kSurfaceEntranceScale = 5.0F;

// --- Slides ----------------------------------------------------------------

// Force air towards the build limit and bedrock towards the world floor.
constexpr double kTopSlideFromY = 240.0;
constexpr double kTopSlideToY = 256.0;
constexpr float kTopSlideTarget = -0.078125F;
constexpr double kBottomSlideFromY = kWorldMinY;
constexpr double kBottomSlideToY = -40.0;
constexpr float kBottomSlideTarget = 0.1171875F;

// --- Post-processing -------------------------------------------------------

constexpr float kPostProcessScale = 0.64F;

// --- Noodle caves ----------------------------------------------------------

// Noodles exist only in this Y band; outside it the toggle is forced negative,
// which selects the "no noodle" branch of the range choice.
constexpr int kNoodleMinY = -60;
constexpr int kNoodleMaxY = 320;
constexpr float kNoodleToggleOff = -1.0F;
constexpr float kNoodleAbsent = 64.0F;
constexpr float kNoodleThicknessLow = -0.05F;
constexpr float kNoodleThicknessHigh = -0.1F;
constexpr double kNoodleRidgeScale = 8.0 / 3.0;
constexpr float kNoodleRidgeWeight = 1.5F;

// --- Preliminary surface ---------------------------------------------------

// Solves the gradient-density equation for the Y where terrain would reach the
// surface, then walks down one cell at a time until the slid density is solid.
constexpr float kSurfaceProbeNumerator = 0.2734375F;
constexpr float kSurfaceProbeFloorY = -40.0F;
constexpr float kSurfaceProbeBias = -0.703125F;
constexpr float kSurfaceProbeClamp = 64.0F;
constexpr float kSurfaceSolidThreshold = 0.390625F;

// --- Grid helpers ----------------------------------------------------------

// Snap down to the lattice, rejecting coordinates that cannot be represented.
// This is the single chokepoint that turns non-finite or absurd inputs into
// std::invalid_argument before any noise or cache is touched.
int floorToGrid(double value, int spacing) {
    const double grid = std::floor(value / spacing) * spacing;
    if (!std::isfinite(grid) || grid < std::numeric_limits<int>::min()
        || grid > std::numeric_limits<int>::max() - spacing) {
        throw std::invalid_argument("Sample coordinate is outside the supported integer grid");
    }
    return static_cast<int>(grid);
}

void requireOnGrid(double value, int spacing) {
    (void)floorToGrid(value, spacing);
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

// The four correlated noodle-cave values, interpolated together so that the
// toggle and the ridges it gates stay consistent within a cell.
struct NoodleSample {
    float toggle{};
    float thickness{};
    float ridgeA{};
    float ridgeB{};
};

[[nodiscard]] float blendSamples(float amount, float from, float to) {
    return lerp(amount, from, to);
}

[[nodiscard]] NoodleSample blendSamples(float amount, const NoodleSample& from, const NoodleSample& to) {
    return {
        lerp(amount, from.toggle, to.toggle),
        lerp(amount, from.thickness, to.thickness),
        lerp(amount, from.ridgeA, to.ridgeA),
        lerp(amount, from.ridgeB, to.ridgeB),
    };
}

// Evaluates a sampler only at the corners of a coarse lattice cell and
// trilinearly interpolates in between, memoizing corners across calls. This is
// both the performance story and part of the output: the interpolation is what
// gives Minecraft terrain its characteristic cell-sized smoothness, so the
// lattice spacing is not a tunable.
template <typename Value>
class LatticeCache {
public:
    LatticeCache(int xzSpacing, int ySpacing) : xzSpacing_(xzSpacing), ySpacing_(ySpacing) {}

    template <typename Sampler>
    [[nodiscard]] Value sample(double x, double y, double z, Sampler&& sampler) {
        const int x0 = floorToGrid(x, xzSpacing_);
        const int y0 = floorToGrid(y, ySpacing_);
        const int z0 = floorToGrid(z, xzSpacing_);
        const int x1 = x0 + xzSpacing_;
        const int y1 = y0 + ySpacing_;
        const int z1 = z0 + xzSpacing_;
        const float tx = static_cast<float>((x - x0) / xzSpacing_);
        const float ty = static_cast<float>((y - y0) / ySpacing_);
        const float tz = static_cast<float>((z - z0) / xzSpacing_);

        const auto corner = [&](int sx, int sy, int sz) -> Value {
            const GridKey key{sx, sy, sz};
            const auto found = values_.find(key);
            if (found != values_.end()) {
                return found->second;
            }
            const Value sampled = sampler(sx, sy, sz);
            values_.emplace(key, sampled);
            return sampled;
        };

        // Corners are bound to locals first: argument evaluation order is
        // unspecified, and keeping the sampler's call order fixed keeps the
        // cache-fill sequence reproducible.
        const Value c000 = corner(x0, y0, z0);
        const Value c100 = corner(x1, y0, z0);
        const Value c010 = corner(x0, y1, z0);
        const Value c110 = corner(x1, y1, z0);
        const Value c001 = corner(x0, y0, z1);
        const Value c101 = corner(x1, y0, z1);
        const Value c011 = corner(x0, y1, z1);
        const Value c111 = corner(x1, y1, z1);

        const Value zLowYLow = blendSamples(tx, c000, c100);
        const Value zLowYHigh = blendSamples(tx, c010, c110);
        const Value zHighYLow = blendSamples(tx, c001, c101);
        const Value zHighYHigh = blendSamples(tx, c011, c111);
        return blendSamples(
            tz,
            blendSamples(ty, zLowYLow, zLowYHigh),
            blendSamples(ty, zHighYLow, zHighYHigh)
        );
    }

private:
    int xzSpacing_;
    int ySpacing_;
    std::unordered_map<GridKey, Value, GridKeyHash> values_;
};

// The 2-D equivalent, for values that vary only per column.
class ColumnCache {
public:
    explicit ColumnCache(int spacing) : spacing_(spacing) {}

    template <typename Sampler>
    [[nodiscard]] float sample(double x, double z, Sampler&& sampler) {
        const int x0 = floorToGrid(x, spacing_);
        const int z0 = floorToGrid(z, spacing_);
        const float tx = static_cast<float>((x - x0) / spacing_);
        const float tz = static_cast<float>((z - z0) / spacing_);

        const auto corner = [&](int sx, int sz) {
            const ColumnKey key{sx, sz};
            const auto found = values_.find(key);
            if (found != values_.end()) {
                return found->second;
            }
            const float value = sampler(sx, sz);
            values_.emplace(key, value);
            return value;
        };

        const float c00 = corner(x0, z0);
        const float c10 = corner(x0 + spacing_, z0);
        const float c01 = corner(x0, z0 + spacing_);
        const float c11 = corner(x0 + spacing_, z0 + spacing_);
        return lerp(tz, lerp(tx, c00, c10), lerp(tx, c01, c11));
    }

private:
    int spacing_;
    std::unordered_map<ColumnKey, float, ColumnKeyHash> values_;
};

// --- Keyed noise streams ---------------------------------------------------
//
// Grouped by the stage of the graph that consumes them. Each entry is
// (resource key, first octave, amplitude per octave); the key is what seeds the
// stream, so renaming one reshuffles that noise for every existing world.

struct ClimateNoises {
    explicit ClimateNoises(std::int64_t seed)
        : shift(seed, "minecraft:offset", -3, {1, 1, 1, 0}),
          temperature(seed, "minecraft:temperature", -10, {1.5, 0, 1, 0, 0, 0}),
          vegetation(seed, "minecraft:vegetation", -8, {1, 1, 0, 0, 0, 0}),
          continentalness(seed, "minecraft:continentalness", -9, {1, 1, 2, 2, 2, 1, 1, 1, 1}),
          erosion(seed, "minecraft:erosion", -9, {1, 1, 0, 1, 1}),
          ridge(seed, "minecraft:ridge", -7, {1, 2, 1, 0, 0, 0}),
          jagged(seed, "minecraft:jagged", -16, std::vector<double>(16, 1.0)) {}

    detail::NormalNoise shift;
    detail::NormalNoise temperature;
    detail::NormalNoise vegetation;
    detail::NormalNoise continentalness;
    detail::NormalNoise erosion;
    detail::NormalNoise ridge;
    detail::NormalNoise jagged;
};

// Surface cave entrances. The roughness pair is also consumed by the
// underground branch, which mixes it into the 2-D spaghetti tunnels.
struct EntranceNoises {
    explicit EntranceNoises(std::int64_t seed)
        : roughness(seed, "minecraft:spaghetti_roughness", -5, {1}),
          roughnessModulator(seed, "minecraft:spaghetti_roughness_modulator", -8, {1}),
          spaghetti3dA(seed, "minecraft:spaghetti_3d_1", -7, {1}),
          spaghetti3dB(seed, "minecraft:spaghetti_3d_2", -7, {1}),
          spaghetti3dRarity(seed, "minecraft:spaghetti_3d_rarity", -11, {1}),
          spaghetti3dThickness(seed, "minecraft:spaghetti_3d_thickness", -8, {1}),
          caveEntrance(seed, "minecraft:cave_entrance", -7, {0.4, 0.5, 1}) {}

    detail::NormalNoise roughness;
    detail::NormalNoise roughnessModulator;
    detail::NormalNoise spaghetti3dA;
    detail::NormalNoise spaghetti3dB;
    detail::NormalNoise spaghetti3dRarity;
    detail::NormalNoise spaghetti3dThickness;
    detail::NormalNoise caveEntrance;
};

struct UndergroundNoises {
    explicit UndergroundNoises(std::int64_t seed)
        : spaghetti2d(seed, "minecraft:spaghetti_2d", -7, {1}),
          spaghetti2dElevation(seed, "minecraft:spaghetti_2d_elevation", -8, {1}),
          spaghetti2dModulator(seed, "minecraft:spaghetti_2d_modulator", -11, {1}),
          spaghetti2dThickness(seed, "minecraft:spaghetti_2d_thickness", -11, {1}),
          caveLayer(seed, "minecraft:cave_layer", -8, {1}),
          caveCheese(seed, "minecraft:cave_cheese", -8, {0.5, 1, 2, 1, 2, 1, 0, 2, 0}),
          pillar(seed, "minecraft:pillar", -7, {1, 1}),
          pillarRareness(seed, "minecraft:pillar_rareness", -8, {1}),
          pillarThickness(seed, "minecraft:pillar_thickness", -8, {1}) {}

    detail::NormalNoise spaghetti2d;
    detail::NormalNoise spaghetti2dElevation;
    detail::NormalNoise spaghetti2dModulator;
    detail::NormalNoise spaghetti2dThickness;
    detail::NormalNoise caveLayer;
    detail::NormalNoise caveCheese;
    detail::NormalNoise pillar;
    detail::NormalNoise pillarRareness;
    detail::NormalNoise pillarThickness;
};

struct NoodleNoises {
    explicit NoodleNoises(std::int64_t seed)
        : toggle(seed, "minecraft:noodle", -8, {1}),
          thickness(seed, "minecraft:noodle_thickness", -8, {1}),
          ridgeA(seed, "minecraft:noodle_ridge_a", -7, {1}),
          ridgeB(seed, "minecraft:noodle_ridge_b", -7, {1}) {}

    detail::NormalNoise toggle;
    detail::NormalNoise thickness;
    detail::NormalNoise ridgeA;
    detail::NormalNoise ridgeB;
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
          climateNoises_(seed),
          entranceNoises_(seed),
          undergroundNoises_(seed),
          noodleNoises_(seed),
          blendedNoise_(seed) {}

    [[nodiscard]] std::int64_t seed() const noexcept {
        return seed_;
    }

    [[nodiscard]] RouterSample sample(double x, double y, double z) const {
        // Validate before evaluating any noise or spline with user coordinates,
        // against the coarsest lattices this call will touch. sampleFinalDensity
        // validates separately, against the finer density lattice, so calling it
        // directly accepts a slightly wider coordinate range near INT_MAX.
        requireOnGrid(x, kColumnSpacing);
        requireOnGrid(y, kCellHeight);
        requireOnGrid(z, kColumnSpacing);

        const Climate climate = sampleClimate(x, z);
        const Terrain terrain = sampleTerrain(x, y, z, climate);

        RouterSample result;
        result.temperature = shiftedNoise(climateNoises_.temperature, x, z);
        result.vegetation = shiftedNoise(climateNoises_.vegetation, x, z);
        result.continentalness = climate.continentalness;
        result.erosion = climate.erosion;
        result.depth = terrain.depth;
        // The router exposes the raw ridge noise: peaksAndValleys() is applied
        // downstream, inside the terrain splines.
        result.ridges = climate.weirdness;
        result.chunkSurfaceLevel = sampleChunkSurfaceLevel(x, z);
        result.finalDensity = sampleFinalDensity(x, y, z);
        return result;
    }

    // final_density = min(post-processed caves, noodle) + beardifier
    [[nodiscard]] float samplePreliminarySurface(int x, int z) const {
        requireOnGrid(x, kColumnSpacing);
        requireOnGrid(z, kColumnSpacing);
        return preliminarySurface(x, z);
    }

    [[nodiscard]] float sampleFinalDensity(double x, double y, double z) const {
        const float post = squeeze(interpolatePost(x, y, z));
        const NoodleSample n = interpolateNoodle(x, y, z);
        const float noodle = inRangeChoice(n.toggle, 0.0F)
            ? kNoodleAbsent
            : n.thickness + kNoodleRidgeWeight * std::max(std::abs(n.ridgeA), std::abs(n.ridgeB));
        return std::min(post, noodle) + beardifier_->sample(x, y, z);
    }

private:
    struct Climate {
        float continentalness{};
        float erosion{};
        float weirdness{};
        float ridges{}; // peaksAndValleys(weirdness)
    };

    struct Terrain {
        float offset{};
        float factor{};
        float depth{};
        float slopedCheese{};
    };

    // --- Climate -----------------------------------------------------------

    [[nodiscard]] float shiftedNoise(const detail::NormalNoise& noise, double x, double z) const {
        const detail::NormalNoise& shift = climateNoises_.shift;
        const float shiftX = kShiftMultiplier * shift.sample(x * kClimateScale, 0.0, z * kClimateScale);
        const float shiftZ = kShiftMultiplier * shift.sample(z * kClimateScale, x * kClimateScale, 0.0);
        return noise.sample(x * kClimateScale + shiftX, 0.0, z * kClimateScale + shiftZ);
    }

    [[nodiscard]] Climate sampleClimate(double x, double z) const {
        Climate climate;
        climate.continentalness = shiftedNoise(climateNoises_.continentalness, x, z);
        climate.erosion = shiftedNoise(climateNoises_.erosion, x, z);
        climate.weirdness = shiftedNoise(climateNoises_.ridge, x, z);
        climate.ridges = detail::peaksAndValleys(climate.weirdness);
        return climate;
    }

    // --- Terrain splines -> sloped cheese ----------------------------------

    [[nodiscard]] Terrain sampleTerrain(double x, double y, double z, const Climate& climate) const {
        const detail::TerrainPoint splinePoint{
            climate.continentalness,
            climate.erosion,
            climate.weirdness,
            climate.ridges,
        };

        // alpha = 1 means "no old-world blending here", which is the only case
        // the default BlendSampler produces.
        const float alpha = blender_->alpha(x, y, z);
        const float rawOffset = kOffsetBias + splines_.offset->sample(splinePoint);
        const float offset = lerp(alpha, blender_->offset(x, y, z), rawOffset);
        const float factor = lerp(alpha, kUnblendedFactor, splines_.factor->sample(splinePoint));
        const float unscaledJaggedness =
            lerp(alpha, kUnblendedJaggedness, splines_.jaggedness->sample(splinePoint));

        const float jaggedNoise =
            climateNoises_.jagged.sample(x * kJaggedNoiseScale, 0.0, z * kJaggedNoiseScale);
        const float jaggedness = unscaledJaggedness * halfNegative(jaggedNoise);

        const float depth =
            clampedGradient(y, kWorldMinY, kWorldMaxY, kDepthAtWorldBottom, kDepthAtWorldTop) + offset;
        const float initialDensity = noiseGradientDensity(factor, depth + jaggedness);
        const float slopedCheese = initialDensity + blendedNoise_.sample(x, y, z);
        return {offset, factor, depth, slopedCheese};
    }

    // --- Cave entrances (surface branch) -----------------------------------

    [[nodiscard]] float spaghettiRoughness(double x, double y, double z) const {
        const float modulator = mapNoise(
            entranceNoises_.roughnessModulator.sample(x, y, z),
            kRoughnessModulatorLow,
            kRoughnessModulatorHigh
        );
        return modulator * (std::abs(entranceNoises_.roughness.sample(x, y, z)) + kRoughnessBias);
    }

    // Step functions that widen or narrow the spaghetti tunnels in bands.
    // Java models these as cubic splines; the vanilla control points are flat,
    // so they reduce exactly to these steps.
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

        // Two decorrelated noises are folded to |n|, so each one contributes a
        // tube where it crosses zero; taking the max intersects them into a
        // connected tunnel network. Rarity rescales the sampling position, so
        // the same noise yields wider or narrower tunnels per region.
        const float rarity = rarity3d(entranceNoises_.spaghetti3dRarity.sample(
            x * kSpaghetti3dRarityScaleXz, y, z * kSpaghetti3dRarityScaleXz
        ));
        const float caveA =
            std::abs(rarity * entranceNoises_.spaghetti3dA.sample(x / rarity, y / rarity, z / rarity));
        const float caveB =
            std::abs(rarity * entranceNoises_.spaghetti3dB.sample(x / rarity, y / rarity, z / rarity));
        const float thickness = mapNoise(
            entranceNoises_.spaghetti3dThickness.sample(x, y, z),
            kSpaghetti3dThicknessLow,
            kSpaghetti3dThicknessHigh
        );
        const float spaghetti = std::clamp(std::max(caveA, caveB) + thickness, -1.0F, 1.0F);

        const float bigEntrance = entranceNoises_.caveEntrance.sample(
                x * kCaveEntranceScaleXz, y * kCaveEntranceScaleY, z * kCaveEntranceScaleXz
            )
            + kCaveEntranceBias
            + clampedGradient(y, kEntranceTaperFromY, kEntranceTaperToY, kEntranceTaperAmount, 0.0F);

        return std::min(bigEntrance, roughness + spaghetti);
    }

    // --- Underground caves -------------------------------------------------

    [[nodiscard]] float spaghetti2d(double x, double y, double z) const {
        const float rarity = rarity2d(undergroundNoises_.spaghetti2dModulator.sample(
            x * kSpaghetti2dRarityScaleXz, y, z * kSpaghetti2dRarityScaleXz
        ));
        const float cave =
            std::abs(rarity * undergroundNoises_.spaghetti2d.sample(x / rarity, y / rarity, z / rarity));
        const float elevation = mapNoise(
            undergroundNoises_.spaghetti2dElevation.sample(x, 0.0, z),
            kSpaghetti2dElevationLow,
            kSpaghetti2dElevationHigh
        );
        const float thickness = mapNoise(
            undergroundNoises_.spaghetti2dThickness.sample(
                x * kSpaghetti2dRarityScaleXz, y, z * kSpaghetti2dRarityScaleXz
            ),
            kSpaghetti2dThicknessLow,
            kSpaghetti2dThicknessHigh
        );

        // Distance from the sloping tunnel plane, cubed so the tunnels stay
        // thin but their walls fall away quickly.
        const float slope = std::abs(elevation
            + clampedGradient(y, kWorldMinY, kWorldMaxY, kSpaghetti2dSlopeAtBottom, kSpaghetti2dSlopeAtTop));
        const float layer = std::pow(slope + thickness, 3.0F);

        return std::clamp(std::max(cave + kSpaghetti2dThicknessWeight * thickness, layer), -1.0F, 1.0F);
    }

    [[nodiscard]] float pillars(double x, double y, double z) const {
        const float raw = (
            kPillarScale * undergroundNoises_.pillar.sample(x * kPillarScaleXz, y * kPillarScaleY, z * kPillarScaleXz)
            + mapNoise(undergroundNoises_.pillarRareness.sample(x, y, z), kPillarRarenessLow, kPillarRarenessHigh)
        ) * std::pow(
            mapNoise(undergroundNoises_.pillarThickness.sample(x, y, z), kPillarThicknessLow, kPillarThicknessHigh),
            3.0F
        );
        return raw >= kPillarCutoff ? raw : kRangeChoiceFloor;
    }

    [[nodiscard]] float underground(double x, double y, double z, float slopedCheese, float entrances) const {
        const float layerNoise = undergroundNoises_.caveLayer.sample(x, y * kCaveLayerScaleY, z);
        const float layer = kCaveLayerScale * layerNoise * layerNoise;
        const float cheese =
            std::clamp(undergroundNoises_.caveCheese.sample(x, y * kCaveCheeseScaleY, z) + kCaveCheeseBias, -1.0F, 1.0F)
            + std::clamp(kCheeseSurfaceGuardBias - kCheeseSurfaceGuardSlope * slopedCheese, 0.0F, kCheeseSurfaceGuardMax);
        const float baseCave = layer + cheese;

        // Every branch here carves, so they combine by taking the minimum;
        // pillars are the one branch that adds material back.
        const float subtraction = std::min(
            std::min(baseCave, entrances),
            spaghetti2d(x, y, z) + spaghettiRoughness(x, y, z)
        );
        return std::max(subtraction, pillars(x, y, z));
    }

    // --- Cave choice -------------------------------------------------------

    [[nodiscard]] float caves(double x, double y, double z) const {
        const Climate climate = sampleClimate(x, z);
        const Terrain terrain = sampleTerrain(x, y, z, climate);
        const float entrances = entrance(x, y, z);

        // Near the surface only entrances are carved, so cheese caves and
        // pillars cannot punch holes through hillsides.
        if (inRangeChoice(terrain.slopedCheese, kUndergroundThreshold)) {
            return std::min(terrain.slopedCheese, kSurfaceEntranceScale * entrances);
        }
        return underground(x, y, z, terrain.slopedCheese, entrances);
    }

    // --- Slides ------------------------------------------------------------

    [[nodiscard]] static float applySlides(double y, float density) {
        const float topFactor = clampedGradient(y, kTopSlideFromY, kTopSlideToY, 1.0F, 0.0F);
        const float top = lerp(topFactor, kTopSlideTarget, density);
        const float bottomFactor = clampedGradient(y, kBottomSlideFromY, kBottomSlideToY, 0.0F, 1.0F);
        return lerp(bottomFactor, kBottomSlideTarget, top);
    }

    // --- Post-processing and noodles ---------------------------------------

    [[nodiscard]] float interpolatePost(double x, double y, double z) const {
        return postCache_.sample(x, y, z, [&](int sx, int sy, int sz) {
            return kPostProcessScale * blender_->applyDensity(sx, sy, sz, applySlides(sy, caves(sx, sy, sz)));
        });
    }

    [[nodiscard]] NoodleSample interpolateNoodle(double x, double y, double z) const {
        return noodleCache_.sample(x, y, z, [&](int sx, int sy, int sz) -> NoodleSample {
            if (sy < kNoodleMinY || sy > kNoodleMaxY) {
                return {kNoodleToggleOff, 0.0F, 0.0F, 0.0F};
            }
            return {
                noodleNoises_.toggle.sample(sx, sy, sz),
                mapNoise(
                    noodleNoises_.thickness.sample(sx, sy, sz), kNoodleThicknessLow, kNoodleThicknessHigh
                ),
                noodleNoises_.ridgeA.sample(sx * kNoodleRidgeScale, sy * kNoodleRidgeScale, sz * kNoodleRidgeScale),
                noodleNoises_.ridgeB.sample(sx * kNoodleRidgeScale, sy * kNoodleRidgeScale, sz * kNoodleRidgeScale),
            };
        });
    }

    // --- Preliminary surface -----------------------------------------------

    [[nodiscard]] float preliminarySurface(int x, int z) const {
        const Climate climate = sampleClimate(x, z);

        // Invert the gradient-density equation at Y = 0 to get a starting
        // guess for where terrain crosses the surface, then search downwards.
        const Terrain terrainAtZero = sampleTerrain(x, 0.0, z, climate);
        const float upperRaw = kSurfaceProbeNumerator / terrainAtZero.factor - terrainAtZero.offset;
        const float upper = std::clamp(
            remap(upperRaw, kDepthAtWorldBottom, kDepthAtWorldTop,
                  static_cast<float>(kWorldMinY), static_cast<float>(kWorldMaxY)),
            kSurfaceProbeFloorY,
            static_cast<float>(kWorldMaxY)
        );

        for (int y = floorToGrid(upper, kCellHeight); y >= static_cast<int>(kWorldMinY); y -= kCellHeight) {
            const Terrain terrain = sampleTerrain(x, y, z, climate);
            // Jaggedness and the 3-D noises are deliberately left out: this is
            // a cheap estimate, not the real surface.
            const float gradientDensity = noiseGradientDensity(terrain.factor, terrain.depth);
            const float probe = std::clamp(
                gradientDensity + kSurfaceProbeBias, -kSurfaceProbeClamp, kSurfaceProbeClamp
            );
            if (applySlides(y, probe) - kSurfaceSolidThreshold > 0.0F) {
                return static_cast<float>(y);
            }
        }
        return static_cast<float>(kWorldMinY);
    }

    [[nodiscard]] float sampleChunkSurfaceLevel(double x, double z) const {
        return surfaceCache_.sample(x, z, [&](int sx, int sz) { return preliminarySurface(sx, sz); });
    }

    std::int64_t seed_;
    std::shared_ptr<const BlendSampler> blender_;
    std::shared_ptr<const Beardifier> beardifier_;
    detail::TerrainSplines splines_;

    ClimateNoises climateNoises_;
    EntranceNoises entranceNoises_;
    UndergroundNoises undergroundNoises_;
    NoodleNoises noodleNoises_;
    detail::BlendedNoise blendedNoise_;

    // Sampling is therefore not const-safe across threads; see the header.
    mutable LatticeCache<float> postCache_{kCellWidth, kCellHeight};
    mutable LatticeCache<NoodleSample> noodleCache_{kCellWidth, kCellHeight};
    mutable ColumnCache surfaceCache_{kColumnSpacing};
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

float OverworldNoiseRouter::samplePreliminarySurface(int x, int z) const {
    return impl_->samplePreliminarySurface(x, z);
}

} // namespace mcworld
