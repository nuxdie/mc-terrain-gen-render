#include "spline.hpp"

#include "density_math.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mcworld::detail {
namespace {

using SplinePtr = std::shared_ptr<const CubicSpline>;
using Point = CubicSpline::Point;

Point constant(float location, float value, float derivative = 0.0F) {
    return Point{location, value, {}, derivative};
}

Point nested(float location, SplinePtr value, float derivative = 0.0F) {
    return Point{location, 0.0F, std::move(value), derivative};
}

SplinePtr spline(TerrainCoordinate coordinate, std::vector<Point> points) {
    return std::make_shared<CubicSpline>(coordinate, std::move(points));
}

// --- Mountain ridge geometry (TerrainProvider.mountainContinentalness) -----
//
// A mountain ridge maps the ridges coordinate onto a continentalness-like
// height through a line whose slope and Y intercept both depend on
// `modulation`. `kRidgeHeightScale` and `kRidgeHeightBias` place the ridges
// axis onto that line; `kRiverFloor` is how far the curve may dip where rivers
// are still allowed to cut through.

constexpr float kRidgeHeightBias = 1.17F;
constexpr float kRidgeHeightScale = 0.46082947F;
constexpr float kRiverFloor = -0.2222F;
constexpr float kAllowRiversBelow = -0.7F;

struct MountainRidgeLine {
    float slope{};
    float intercept{};
};

MountainRidgeLine mountainRidgeLine(float modulation) {
    return {1.0F - (1.0F - modulation) * 0.5F, 0.5F * (1.0F - modulation)};
}

float mountainContinentalness(float ridge, float modulation, float allowRiversBelow) {
    const MountainRidgeLine line = mountainRidgeLine(modulation);
    const float adjustedRidgeHeight = (ridge + kRidgeHeightBias) * kRidgeHeightScale;
    const float continentalness = adjustedRidgeHeight * line.slope - line.intercept;
    return ridge < allowRiversBelow
        ? std::max(continentalness, kRiverFloor)
        : std::max(continentalness, 0.0F);
}

// The ridges value at which mountainContinentalness() crosses zero.
float mountainRidgeZero(float modulation) {
    const MountainRidgeLine line = mountainRidgeLine(modulation);
    return line.intercept / (kRidgeHeightScale * line.slope) - kRidgeHeightBias;
}

float slope(float y1, float y2, float x1, float x2) {
    return (y2 - y1) / (x2 - x1);
}

// Whether the ridge curve dips through a central saddle before rising again.
enum class Saddle { Absent, Present };

SplinePtr buildMountainRidge(float modulation, Saddle saddle) {
    const float minValue = mountainContinentalness(-1.0F, modulation, kAllowRiversBelow);
    const float maxValue = mountainContinentalness(1.0F, modulation, kAllowRiversBelow);
    const float zero = mountainRidgeZero(modulation);
    std::vector<Point> points;

    if (-0.65F < zero && zero < 1.0F) {
        const float afterRiver = mountainContinentalness(-0.65F, modulation, kAllowRiversBelow);
        const float beforeRiver = mountainContinentalness(-0.75F, modulation, kAllowRiversBelow);
        const float minDerivative = slope(minValue, beforeRiver, -1.0F, -0.75F);
        const float zeroValue = mountainContinentalness(zero, modulation, kAllowRiversBelow);
        const float maxDerivative = slope(zeroValue, maxValue, zero, 1.0F);
        points = {
            constant(-1.0F, minValue, minDerivative),
            constant(-0.75F, beforeRiver),
            constant(-0.65F, afterRiver),
            constant(zero - 0.01F, zeroValue),
            constant(zero, zeroValue, maxDerivative),
            constant(1.0F, maxValue, maxDerivative),
        };
    } else {
        const float derivative = slope(minValue, maxValue, -1.0F, 1.0F);
        if (saddle == Saddle::Present) {
            points.push_back(constant(-1.0F, std::max(0.2F, minValue)));
            points.push_back(constant(0.0F, lerp(0.5F, minValue, maxValue), derivative));
        } else {
            points.push_back(constant(-1.0F, minValue, derivative));
        }
        points.push_back(constant(1.0F, maxValue, derivative));
    }
    return spline(TerrainCoordinate::Ridges, std::move(points));
}

// Five control points sampled along the ridges axis, from river valleys at
// -1 through to peaks at +1. Derivatives are chosen so the curve stays
// monotone through the valley and flattens out towards the peaks.
SplinePtr ridgeSpline(
    float valley,
    float low,
    float mid,
    float high,
    float peaks,
    float minValleySteepness
) {
    const float d1 = std::max(0.5F * (low - valley), minValleySteepness);
    const float d2 = 5.0F * (mid - low);
    return spline(TerrainCoordinate::Ridges, {
        constant(-1.0F, valley, d1),
        constant(-0.4F, low, std::min(d1, d2)),
        constant(0.0F, mid, d2),
        constant(0.4F, high, 2.0F * (high - mid)),
        constant(1.0F, peaks, 0.7F * (peaks - high)),
    });
}

// The offset spline's erosion layer. One of these is built per
// continentalness band; the fields are the terrain heights that band reaches
// at each erosion level, so a call site reads as a description of a landscape.
struct ErosionOffset {
    float lowValley{};
    float hill{};
    float tallHill{};
    float mountainFactor{};
    float plain{};
    float swamp{};
    bool includeExtremeHills{};
    Saddle saddle{Saddle::Absent};
};

SplinePtr buildErosionOffset(const ErosionOffset& p) {
    const float lowValley = p.lowValley;
    const float hill = p.hill;
    const float tallHill = p.tallHill;
    const float mountainFactor = p.mountainFactor;
    const float plain = p.plain;
    const float swamp = p.swamp;

    const SplinePtr veryLow = buildMountainRidge(lerp(mountainFactor, 0.6F, 1.5F), p.saddle);
    const SplinePtr low = buildMountainRidge(lerp(mountainFactor, 0.6F, 1.0F), p.saddle);
    const SplinePtr mountains = buildMountainRidge(mountainFactor, p.saddle);
    const SplinePtr widePlateau = ridgeSpline(
        lowValley - 0.15F,
        0.5F * mountainFactor,
        0.5F * mountainFactor,
        0.5F * mountainFactor,
        0.6F * mountainFactor,
        0.5F
    );
    const SplinePtr narrowPlateau = ridgeSpline(
        lowValley,
        plain * mountainFactor,
        hill * mountainFactor,
        0.5F * mountainFactor,
        0.6F * mountainFactor,
        0.5F
    );
    const SplinePtr plains = ridgeSpline(lowValley, plain, plain, hill, tallHill, 0.5F);
    const SplinePtr extreme = spline(TerrainCoordinate::Ridges, {
        constant(-1.0F, lowValley),
        nested(-0.4F, plains),
        constant(0.0F, tallHill + 0.07F),
    });
    const SplinePtr swamps = ridgeSpline(-0.02F, swamp, swamp, hill, tallHill, 0.0F);

    std::vector<Point> points = {
        nested(-0.85F, veryLow),
        nested(-0.7F, low),
        nested(-0.4F, mountains),
        nested(-0.35F, widePlateau),
        nested(-0.1F, narrowPlateau),
        nested(0.2F, plains),
    };
    if (p.includeExtremeHills) {
        points.push_back(nested(0.4F, plains));
        points.push_back(nested(0.45F, extreme));
        points.push_back(nested(0.55F, extreme));
        points.push_back(nested(0.58F, plains));
    }
    points.push_back(nested(0.7F, swamps));
    return spline(TerrainCoordinate::Erosion, std::move(points));
}

SplinePtr buildOffsetSpline() {
    // Four continentalness bands, from coastline to deep inland. Inland bands
    // get taller mountains, extreme hills and a saddled ridge profile.
    const SplinePtr beach = buildErosionOffset({
        .lowValley = -0.15F, .hill = 0.0F, .tallHill = 0.0F, .mountainFactor = 0.1F,
        .plain = 0.0F, .swamp = -0.03F, .includeExtremeHills = false, .saddle = Saddle::Absent,
    });
    const SplinePtr low = buildErosionOffset({
        .lowValley = -0.1F, .hill = 0.03F, .tallHill = 0.1F, .mountainFactor = 0.1F,
        .plain = 0.01F, .swamp = -0.03F, .includeExtremeHills = false, .saddle = Saddle::Absent,
    });
    const SplinePtr mid = buildErosionOffset({
        .lowValley = -0.1F, .hill = 0.03F, .tallHill = 0.1F, .mountainFactor = 0.7F,
        .plain = 0.01F, .swamp = -0.03F, .includeExtremeHills = true, .saddle = Saddle::Present,
    });
    const SplinePtr high = buildErosionOffset({
        .lowValley = -0.05F, .hill = 0.03F, .tallHill = 0.1F, .mountainFactor = 1.0F,
        .plain = 0.01F, .swamp = 0.01F, .includeExtremeHills = true, .saddle = Saddle::Present,
    });
    return spline(TerrainCoordinate::Continentalness, {
        constant(-1.1F, 0.044F),
        constant(-1.02F, -0.2222F),
        constant(-0.51F, -0.2222F),
        constant(-0.44F, -0.12F),
        constant(-0.18F, -0.12F),
        nested(-0.16F, beach),
        nested(-0.15F, beach),
        nested(-0.1F, low),
        nested(0.25F, mid),
        nested(1.0F, high),
    });
}

// Shattered bands (windswept/savanna-like) replace the peak-and-extreme-hill
// sub-splines with a much narrower ridge response.
enum class Shattered { No, Yes };

SplinePtr buildErosionFactor(float baseValue, Shattered shattered) {
    const SplinePtr base = spline(TerrainCoordinate::Weirdness, {
        constant(-0.2F, 6.3F),
        constant(0.2F, baseValue),
    });
    std::vector<Point> points = {
        nested(-0.6F, base),
        nested(-0.5F, spline(TerrainCoordinate::Weirdness, {
            constant(-0.05F, 6.3F), constant(0.05F, 2.67F),
        })),
        nested(-0.35F, base),
        nested(-0.25F, base),
        nested(-0.1F, spline(TerrainCoordinate::Weirdness, {
            constant(-0.05F, 2.67F), constant(0.05F, 6.3F),
        })),
        nested(0.03F, base),
    };

    if (shattered == Shattered::Yes) {
        const SplinePtr shatteredWeirdness = spline(TerrainCoordinate::Weirdness, {
            constant(0.0F, baseValue), constant(0.1F, 0.625F),
        });
        const SplinePtr shatteredRidges = spline(TerrainCoordinate::Ridges, {
            constant(-0.9F, baseValue), nested(-0.69F, shatteredWeirdness),
        });
        points.push_back(constant(0.35F, baseValue));
        points.push_back(nested(0.45F, shatteredRidges));
        points.push_back(nested(0.55F, shatteredRidges));
        points.push_back(constant(0.62F, baseValue));
    } else {
        const SplinePtr peaksOnly = spline(TerrainCoordinate::Ridges, {
            nested(0.45F, base), constant(0.7F, 1.56F),
        });
        const SplinePtr extremeHills = spline(TerrainCoordinate::Ridges, {
            nested(-0.7F, base), constant(-0.15F, 1.37F),
        });
        points.push_back(nested(0.05F, peaksOnly));
        points.push_back(nested(0.4F, peaksOnly));
        points.push_back(nested(0.45F, extremeHills));
        points.push_back(nested(0.55F, extremeHills));
        points.push_back(constant(0.58F, baseValue));
    }
    return spline(TerrainCoordinate::Erosion, std::move(points));
}

SplinePtr buildFactorSpline() {
    return spline(TerrainCoordinate::Continentalness, {
        constant(-0.19F, 3.95F),
        nested(-0.15F, buildErosionFactor(6.25F, Shattered::Yes)),
        nested(-0.1F, buildErosionFactor(5.47F, Shattered::Yes)),
        nested(0.03F, buildErosionFactor(5.08F, Shattered::Yes)),
        nested(0.06F, buildErosionFactor(4.69F, Shattered::No)),
    });
}

// --- Jaggedness ------------------------------------------------------------
//
// Jaggedness scales a high-frequency noise that is only applied near peaks, so
// each layer below gates it further: continentalness -> erosion -> ridges ->
// weirdness. A zero factor means "no jaggedness here" and is encoded as a
// constant point rather than a nested spline.

SplinePtr buildWeirdnessJaggedness(float factor) {
    return spline(TerrainCoordinate::Weirdness, {
        constant(-0.01F, 0.63F * factor),
        constant(0.01F, 0.3F * factor),
    });
}

SplinePtr buildRidgeJaggedness(float peakFactor, float highFactor) {
    // The window is expressed in weirdness and converted, so that it tracks
    // peaksAndValleys() rather than duplicating its shape.
    constexpr float kHighWeirdnessStart = 0.4F;
    constexpr float kHighWeirdnessEnd = 0.56666666F;
    const float highStart = peaksAndValleys(kHighWeirdnessStart);
    const float highEnd = peaksAndValleys(kHighWeirdnessEnd);
    const float middle = (highStart + highEnd) * 0.5F;
    return spline(TerrainCoordinate::Ridges, {
        constant(highStart, 0.0F),
        highFactor > 0.0F ? nested(middle, buildWeirdnessJaggedness(highFactor)) : constant(middle, 0.0F),
        peakFactor > 0.0F ? nested(1.0F, buildWeirdnessJaggedness(peakFactor)) : constant(1.0F, 0.0F),
    });
}

SplinePtr buildErosionJaggedness(float peak0, float peak1, float high0, float high1) {
    const SplinePtr ridge0 = buildRidgeJaggedness(peak0, high0);
    const SplinePtr ridge1 = buildRidgeJaggedness(peak1, high1);
    return spline(TerrainCoordinate::Erosion, {
        nested(-1.0F, ridge0),
        nested(-0.78F, ridge1),
        nested(-0.5775F, ridge1),
        constant(-0.375F, 0.0F),
    });
}

SplinePtr buildJaggednessSpline() {
    return spline(TerrainCoordinate::Continentalness, {
        constant(-0.11F, 0.0F),
        nested(0.03F, buildErosionJaggedness(1.0F, 0.5F, 0.0F, 0.0F)),
        nested(0.65F, buildErosionJaggedness(1.0F, 1.0F, 1.0F, 0.0F)),
    });
}

} // namespace

CubicSpline::CubicSpline(TerrainCoordinate coordinate, std::vector<Point> points)
    : coordinate_(coordinate), points_(std::move(points)) {
    if (points_.empty()) {
        throw std::invalid_argument("A cubic spline requires at least one point");
    }
    if (!std::is_sorted(points_.begin(), points_.end(), [](const Point& left, const Point& right) {
            return left.location < right.location;
        })) {
        throw std::invalid_argument("Cubic spline points must be ordered");
    }
}

float CubicSpline::coordinate(const TerrainPoint& point) const {
    switch (coordinate_) {
    case TerrainCoordinate::Continentalness:
        return point.continentalness;
    case TerrainCoordinate::Erosion:
        return point.erosion;
    case TerrainCoordinate::Weirdness:
        return point.weirdness;
    case TerrainCoordinate::Ridges:
        return point.ridges;
    }
    return 0.0F;
}

float CubicSpline::valueAt(const Point& point, const TerrainPoint& input) {
    return point.nested ? point.nested->sample(input) : point.value;
}

// Cubic Hermite interpolation between the bracketing control points, with
// linear extrapolation along the end derivatives outside the point range.
float CubicSpline::sample(const TerrainPoint& input) const {
    const float value = coordinate(input);
    if (value < points_.front().location) {
        const Point& first = points_.front();
        return valueAt(first, input) + first.derivative * (value - first.location);
    }
    if (value >= points_.back().location) {
        const Point& last = points_.back();
        return valueAt(last, input) + last.derivative * (value - last.location);
    }

    const auto upper = std::upper_bound(points_.begin(), points_.end(), value, [](float needle, const Point& point) {
        return needle < point.location;
    });
    const Point& right = *upper;
    const Point& left = *(upper - 1);
    const float distance = right.location - left.location;
    const float amount = (value - left.location) / distance;
    const float leftValue = valueAt(left, input);
    const float rightValue = valueAt(right, input);
    const float a = left.derivative * distance - (rightValue - leftValue);
    const float b = -right.derivative * distance + (rightValue - leftValue);
    return lerp(amount, leftValue, rightValue) + amount * (1.0F - amount) * lerp(amount, a, b);
}

TerrainSplines buildStandardOverworldSplines() {
    return {buildOffsetSpline(), buildFactorSpline(), buildJaggednessSpline()};
}

// Folds weirdness into the "peaks and valleys" ridges coordinate: a triangle
// wave over [-1, 1] that peaks at weirdness = +/-2/3 and bottoms out at 0
// and +/-1, which is what turns a single noise into alternating ridges.
float peaksAndValleys(float weirdness) {
    return -(std::abs(std::abs(weirdness) - 0.6666667F) - 0.33333334F) * 3.0F;
}

} // namespace mcworld::detail
