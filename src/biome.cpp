// Step 6 of `minecraft-26.3-worldgen.dot`: the multi-noise Overworld biome
// source.
//
// Java: `OverworldBiomeBuilder` registers ~7,600 `Climate.ParameterPoint`s,
// each an axis-aligned box in the six climate dimensions. A lookup quantizes
// the sampled climate the same way (`(long)(value * 10000)`), then takes the
// nearest box by squared distance, counting a dimension as distance zero when
// the value is inside its interval.
//
// The registration *order* is part of the behaviour. Boxes overlap, and equal
// distances resolve to the first point registered, so the table below is
// written to emit points in the same order as the Java builder even where a
// more compact loop nest would produce the same set. See
// `WORLDGEN_AUDIT.md` for how this tie policy differs from Java's R-tree.

#include "mcworld/biome.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mcworld {
namespace {

using enum Biome;

// --- Climate axes ----------------------------------------------------------

// A closed interval on one climate axis, in sampled (pre-quantization) units.
struct Range {
    float low;
    float high;
};

constexpr Range kFull{-1.0F, 1.0F};

// The standard interval tables. Indices are used directly below, so these
// arrays double as the vocabulary the rule rows are written in.
constexpr Range kTemperatures[]{{-1, -.45F}, {-.45F, -.15F}, {-.15F, .2F}, {.2F, .55F}, {.55F, 1}};
constexpr Range kHumidities[]{{-1, -.35F}, {-.35F, -.1F}, {-.1F, .1F}, {.1F, .3F}, {.3F, 1}};
constexpr Range kErosions[]{{-1, -.78F}, {-.78F, -.375F}, {-.375F, -.2225F}, {-.2225F, .05F},
                            {.05F, .45F}, {.45F, .55F}, {.55F, 1}};
// Inland continentalness only; oceans and mushroom fields are registered apart.
constexpr Range kContinents[]{{-.19F, -.11F}, {-.11F, .03F}, {.03F, .3F}, {.3F, 1}};

constexpr int kTemperatureCount = 5;
constexpr int kHumidityCount = 5;
constexpr int kClimateAxes = 6;

// Climate is sampled per 4x4x4 cell, like the biome palette itself.
constexpr int kQuartSize = 4;

// Java quantizes climate to ten-thousandths and does all comparisons in longs.
constexpr float kQuantizationScale = 10000.0F;

[[nodiscard]] std::int64_t quantize(float value) {
    return static_cast<std::int64_t>(value * kQuantizationScale);
}

// A registered parameter point: one quantized interval per climate axis.
// Axis order is temperature, vegetation, continentalness, erosion, depth,
// weirdness, matching `RouterSample` as read in `resolveOverworldBiome`.
struct Point {
    std::array<std::array<std::int64_t, 2>, kClimateAxes> ranges;
    Biome biome;
};

// --- Biome tables ----------------------------------------------------------
//
// Rows are temperature (cold to hot), columns are humidity (dry to wet). The
// `Variant` tables are used where weirdness is positive.

constexpr Biome kMiddle[kTemperatureCount][kHumidityCount]{
    {SnowyPlains, SnowyPlains, SnowyPlains, SnowyTaiga, Taiga},
    {Plains, Plains, Forest, Taiga, OldGrowthSpruceTaiga},
    {FlowerForest, Plains, Forest, BirchForest, DarkForest},
    {Savanna, Savanna, Forest, Jungle, Jungle},
    {Desert, Desert, Desert, Desert, Desert}};

constexpr Biome kMiddleVariant[kTemperatureCount][kHumidityCount]{
    {IceSpikes, SnowyPlains, SnowyTaiga, SnowyTaiga, Taiga},
    {DappledForest, Plains, Forest, Taiga, OldGrowthPineTaiga},
    {SunflowerPlains, Plains, Forest, OldGrowthBirchForest, DarkForest},
    {Savanna, Savanna, Plains, SparseJungle, BambooJungle},
    {Desert, Desert, Desert, Desert, Desert}};

constexpr Biome kPlateau[kTemperatureCount][kHumidityCount]{
    {SnowyPlains, SnowyPlains, SnowyPlains, SnowyTaiga, SnowyTaiga},
    {Meadow, Meadow, Forest, Taiga, OldGrowthSpruceTaiga},
    {Meadow, Meadow, Meadow, Meadow, PaleGarden},
    {SavannaPlateau, SavannaPlateau, Forest, Forest, Jungle},
    {Badlands, Badlands, Badlands, WoodedBadlands, WoodedBadlands}};

constexpr Biome kPlateauVariant[kTemperatureCount][kHumidityCount]{
    {IceSpikes, SnowyPlains, SnowyPlains, SnowyTaiga, SnowyTaiga},
    {CherryGrove, Meadow, Meadow, Meadow, OldGrowthPineTaiga},
    {CherryGrove, CherryGrove, Forest, BirchForest, PaleGarden},
    {SavannaPlateau, SavannaPlateau, Forest, Forest, Jungle},
    {ErodedBadlands, ErodedBadlands, Badlands, WoodedBadlands, WoodedBadlands}};

// --- Weirdness slices ------------------------------------------------------

// Java splits weirdness into thirteen bands and assigns each one a terrain
// shape. The shape decides which continentalness/erosion rows apply; the sign
// of the band decides between the plain and variant biome tables.
enum class Shape { Mid, High, Peaks, Low, Valley };

constexpr float kWeirdnessEdges[]{-1, -.93333334F, -.7666667F, -.56666666F, -.4F, -.26666668F, -.05F,
                                  .05F, .26666668F, .4F, .56666666F, .7666667F, .93333334F, 1};
constexpr Shape kSliceShapes[]{Shape::Mid, Shape::High, Shape::Peaks, Shape::High, Shape::Mid,
                               Shape::Low, Shape::Valley, Shape::Low, Shape::Mid, Shape::High,
                               Shape::Peaks, Shape::High, Shape::Mid};
constexpr int kSliceCount = static_cast<int>(std::size(kSliceShapes));

// --- Table construction ----------------------------------------------------

// Accumulates parameter points in registration order.
class PointBuilder {
public:
    // Registers one point, with depth given explicitly. Used by the cave
    // entries, which are the only ones that constrain depth to a sub-range.
    void add(Range temperature, Range humidity, Range continent, Range erosion, Range depth, Range weirdness, Biome biome) {
        Point point{};
        point.biome = biome;
        const std::array<Range, kClimateAxes> axes{temperature, humidity, continent, erosion, depth, weirdness};
        for (int axis = 0; axis < kClimateAxes; ++axis) {
            point.ranges[axis] = {quantize(axes[axis].low), quantize(axes[axis].high)};
        }
        points_.push_back(point);
    }

    // Registers a surface biome, which Java emits twice: once at depth 0 (the
    // surface itself) and once at depth 1 (the underground column below it).
    void addSurface(Range temperature, Range humidity, Range continent, Range erosion, Range weirdness, Biome biome) {
        add(temperature, humidity, continent, erosion, {0, 0}, weirdness, biome);
        add(temperature, humidity, continent, erosion, {1, 1}, weirdness, biome);
    }

    [[nodiscard]] std::vector<Point> take() { return std::move(points_); }

private:
    std::vector<Point> points_;
};

// A slice of weirdness, with the helpers its rules are written against.
class Slice {
public:
    Slice(PointBuilder& points, int index)
        : points_(points),
          weirdness_{kWeirdnessEdges[index], kWeirdnessEdges[index + 1]},
          shape_(kSliceShapes[index]),
          variant_(kWeirdnessEdges[index + 1] >= 0) {}

    [[nodiscard]] Shape shape() const { return shape_; }
    // Positive weirdness selects the variant biome tables.
    [[nodiscard]] bool variant() const { return variant_; }

    // Registers a point spanning continentalness rows `[c0, c1]` and erosion
    // rows `[e0, e1]`, across all humidities.
    void addAnyHumidity(Range temperature, int c0, int c1, int e0, int e1, Biome biome) {
        points_.addSurface(temperature, kFull, span(kContinents, c0, c1), span(kErosions, e0, e1), weirdness_, biome);
    }

    void add(int temperature, int humidity, int c0, int c1, int e0, int e1, Biome biome) {
        points_.addSurface(kTemperatures[temperature], kHumidities[humidity],
                           span(kContinents, c0, c1), span(kErosions, e0, e1), weirdness_, biome);
    }

private:
    template <std::size_t N>
    [[nodiscard]] static Range span(const Range (&table)[N], int first, int last) {
        return {table[first].low, table[last].high};
    }

    PointBuilder& points_;
    Range weirdness_;
    Shape shape_;
    bool variant_;
};

// Rivers, frozen rivers and the stony shores that replace them near coasts.
void addValleyRivers(Slice& slice) {
    // Java registers these as nested loops over two temperature groups and
    // three continentalness/erosion regions; the ranges overlap, so the order
    // is what decides which entry wins on a tie.
    for (int region = 0; region < 3; ++region) {
        for (int frozen = 0; frozen < 2; ++frozen) {
            const Range temperature = frozen == 0 ? kTemperatures[0] : Range{kTemperatures[1].low, 1};
            const Biome river = frozen == 0 ? FrozenRiver : River;
            if (region == 0) {
                slice.addAnyHumidity(temperature, 0, 0, 0, 1, slice.variant() ? river : StonyShore);
            } else if (region == 1) {
                slice.addAnyHumidity(temperature, 1, 1, 0, 1, river);
            } else {
                slice.addAnyHumidity(temperature, 0, 3, 2, 5, river);
            }
        }
    }
    slice.addAnyHumidity(kTemperatures[0], 0, 0, 6, 6, FrozenRiver);
    slice.addAnyHumidity({kTemperatures[1].low, 1}, 0, 0, 6, 6, River);
}

// Swamps occupy the flattest erosion row of the low, mid and valley slices.
void addSwamps(Slice& slice) {
    slice.addAnyHumidity({kTemperatures[1].low, kTemperatures[2].high}, 1, 3, 6, 6, Swamp);
    slice.addAnyHumidity({kTemperatures[3].low, kTemperatures[4].high}, 1, 3, 6, 6, MangroveSwamp);
    if (slice.shape() == Shape::Valley) {
        slice.addAnyHumidity(kTemperatures[0], 1, 3, 6, 6, FrozenRiver);
    }
}

// The inland biome grid for one weirdness slice: for every temperature and
// humidity cell, pick the biomes this slice's shape uses, then lay them out
// over the continentalness/erosion rows.
void addInlandGrid(Slice& slice, int temperature, int humidity) {
    const bool variant = slice.variant();
    const Biome middle = variant ? kMiddleVariant[temperature][humidity] : kMiddle[temperature][humidity];
    const Biome plateau = variant ? kPlateauVariant[temperature][humidity] : kPlateau[temperature][humidity];

    const Biome badlands = humidity < 2 ? (variant ? ErodedBadlands : Badlands)
                                        : (humidity < 3 ? Badlands : WoodedBadlands);
    // The hottest temperature row is badlands rather than the middle biome.
    const Biome hot = temperature == 4 ? badlands : middle;
    const Biome slope = temperature >= 3 ? plateau : (humidity <= 1 ? SnowySlopes : Grove);
    const Biome cold = temperature == 0 ? slope : hot;
    const Biome peak = temperature <= 2 ? (variant ? FrozenPeaks : JaggedPeaks)
                                        : (temperature == 3 ? StonyPeaks : badlands);
    const Biome shattered = temperature >= 3 ? middle
                          : (humidity >= 3 ? WindsweptForest
                          : (humidity < 2 && temperature < 2 ? WindsweptGravellyHills : WindsweptHills));
    const Biome beach = temperature == 0 ? SnowyBeach : (temperature == 4 ? Desert : Beach);

    // Windswept savanna replaces the shattered/middle biome on warm, dry,
    // positive-weirdness slices.
    auto windswept = [&](Biome biome) {
        return temperature > 1 && humidity < 4 && variant ? WindsweptSavanna : biome;
    };
    auto put = [&](int c0, int c1, int e0, int e1, Biome biome) {
        slice.add(temperature, humidity, c0, c1, e0, e1, biome);
    };

    switch (slice.shape()) {
    case Shape::Peaks:
        put(0, 3, 0, 0, peak);
        put(0, 1, 1, 1, cold);
        put(2, 3, 1, 1, peak);
        put(0, 1, 2, 3, middle);
        put(2, 3, 2, 2, plateau);
        put(2, 2, 3, 3, hot);
        put(3, 3, 3, 3, plateau);
        put(0, 3, 4, 4, middle);
        put(0, 1, 5, 5, windswept(shattered));
        put(2, 3, 5, 5, shattered);
        put(0, 3, 6, 6, middle);
        break;
    case Shape::High:
        put(0, 0, 0, 1, middle);
        put(1, 1, 0, 0, slope);
        put(2, 3, 0, 0, peak);
        put(1, 1, 1, 1, cold);
        put(2, 3, 1, 1, slope);
        put(0, 1, 2, 3, middle);
        put(2, 3, 2, 2, plateau);
        put(2, 2, 3, 3, hot);
        put(3, 3, 3, 3, plateau);
        put(0, 3, 4, 4, middle);
        put(0, 1, 5, 5, windswept(middle));
        put(2, 3, 5, 5, shattered);
        put(0, 3, 6, 6, middle);
        break;
    case Shape::Mid:
        put(1, 3, 0, 0, slope);
        put(1, 2, 1, 1, cold);
        put(3, 3, 1, 1, temperature == 0 ? slope : plateau);
        put(1, 1, 2, 2, middle);
        put(2, 2, 2, 2, hot);
        put(3, 3, 2, 2, plateau);
        put(0, 1, 3, 3, middle);
        put(2, 3, 3, 3, hot);
        if (!variant) {
            put(0, 0, 4, 4, beach);
            put(1, 3, 4, 4, middle);
        } else {
            put(0, 3, 4, 4, middle);
        }
        put(0, 0, 5, 5, windswept(variant ? middle : beach));
        put(1, 1, 5, 5, windswept(middle));
        put(2, 3, 5, 5, shattered);
        put(0, 0, 6, 6, variant ? middle : beach);
        if (temperature == 0) {
            put(1, 3, 6, 6, middle);
        }
        break;
    case Shape::Low:
        put(1, 1, 0, 1, hot);
        put(2, 3, 0, 1, cold);
        put(1, 1, 2, 3, middle);
        put(2, 3, 2, 3, hot);
        put(0, 0, 3, 4, beach);
        put(1, 3, 4, 4, middle);
        put(0, 0, 5, 5, windswept(variant ? middle : beach));
        put(1, 1, 5, 5, windswept(middle));
        put(2, 3, 5, 5, middle);
        put(0, 0, 6, 6, beach);
        if (temperature == 0) {
            put(1, 3, 6, 6, middle);
        }
        break;
    case Shape::Valley:
        put(2, 3, 0, 1, hot);
        break;
    }
}

std::vector<Point> makePoints() {
    PointBuilder points;

    // Continentalness below the ocean floor: the mushroom islands.
    points.addSurface(kFull, kFull, {-1.2F, -1.05F}, kFull, kFull, MushroomFields);

    // Deep and shallow oceans, by temperature.
    constexpr Biome kOceans[2][kTemperatureCount]{
        {DeepFrozenOcean, DeepColdOcean, DeepOcean, DeepLukewarmOcean, WarmOcean},
        {FrozenOcean, ColdOcean, Ocean, LukewarmOcean, WarmOcean}};
    for (int temperature = 0; temperature < kTemperatureCount; ++temperature) {
        points.addSurface(kTemperatures[temperature], kFull, {-1.05F, -.455F}, kFull, kFull, kOceans[0][temperature]);
        points.addSurface(kTemperatures[temperature], kFull, {-.455F, -.19F}, kFull, kFull, kOceans[1][temperature]);
    }

    // Inland surface biomes, one weirdness slice at a time.
    for (int index = 0; index < kSliceCount; ++index) {
        Slice slice(points, index);
        const Shape shape = slice.shape();
        if (shape == Shape::Mid || shape == Shape::Low) {
            slice.addAnyHumidity(kFull, 0, 0, 0, 2, StonyShore);
        }
        if (shape == Shape::Valley) {
            addValleyRivers(slice);
        }
        if (shape == Shape::Mid || shape == Shape::Low || shape == Shape::Valley) {
            addSwamps(slice);
        }
        for (int temperature = 0; temperature < kTemperatureCount; ++temperature) {
            for (int humidity = 0; humidity < kHumidityCount; ++humidity) {
                addInlandGrid(slice, temperature, humidity);
            }
        }
    }

    // Cave biomes, the only entries restricted to a depth band.
    constexpr Range kCaveDepth{.2F, .9F};
    points.add(kFull, kFull, {.8F, 1}, kFull, kCaveDepth, kFull, DripstoneCaves);
    points.add(kFull, {.7F, 1}, kFull, kFull, kCaveDepth, kFull, LushCaves);
    points.add(kFull, kFull, {-.19F, .55F}, {.45F, 1}, kCaveDepth, {-1.1F, -.85F}, SulfurCaves);
    points.add(kFull, kFull, kFull, {-1, -.375F}, {1.1F, 1.1F}, kFull, DeepDark);

    return points.take();
}

} // namespace

Biome resolveOverworldBiome(const RouterSample& sample) {
    static const std::vector<Point> points = makePoints();

    const std::array<float, kClimateAxes> values{sample.temperature, sample.vegetation, sample.continentalness,
                                                 sample.erosion, sample.depth, sample.ridges};
    std::array<std::int64_t, kClimateAxes> target{};
    for (int axis = 0; axis < kClimateAxes; ++axis) {
        if (!std::isfinite(values[axis]) || std::abs(values[axis]) > kQuantizationScale) {
            throw std::invalid_argument("Invalid biome climate");
        }
        target[axis] = quantize(values[axis]);
    }

    std::int64_t best = std::numeric_limits<std::int64_t>::max();
    Biome result = Plains;
    for (const Point& point : points) {
        std::int64_t distance = 0;
        for (int axis = 0; axis < kClimateAxes; ++axis) {
            // Distance zero inside the interval, else the gap to the nearer edge.
            const auto gap = std::max({point.ranges[axis][0] - target[axis],
                                       target[axis] - point.ranges[axis][1],
                                       std::int64_t{0}});
            distance += gap * gap;
            if (distance >= best) {
                break; // Later axes only add, so this point cannot win.
            }
        }
        // Strictly-less keeps the first registered point on a tie.
        if (distance < best) {
            best = distance;
            result = point.biome;
        }
    }
    return result;
}

Biome sampleOverworldBiome(const OverworldNoiseRouter& router, int x, int y, int z) {
    // Climate is sampled on the quart lattice, flooring towards negative
    // infinity so a block's cell does not flip sign at the origin.
    auto quart = [](int value) { return std::floor(static_cast<double>(value) / kQuartSize) * kQuartSize; };
    return resolveOverworldBiome(router.sample(quart(x), quart(y), quart(z)));
}

} // namespace mcworld
