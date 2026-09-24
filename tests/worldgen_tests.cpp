#include "mcworld/worldgen.hpp"
#include "noise.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class ConstantBeardifier final : public mcworld::Beardifier {
public:
    explicit ConstantBeardifier(float value) : value_(value) {}

    float sample(double, double, double) const override {
        return value_;
    }

private:
    float value_;
};

void testDeterminism() {
    mcworld::OverworldNoiseRouter first(123456789);
    mcworld::OverworldNoiseRouter second(123456789);
    constexpr double x = 137.0;
    constexpr double y = 72.0;
    constexpr double z = -291.0;
    check(first.sampleFinalDensity(x, y, z) == second.sampleFinalDensity(x, y, z), "same seed is deterministic");

    const mcworld::RouterSample a = first.sample(x, y, z);
    const mcworld::RouterSample b = second.sample(x, y, z);
    check(a.temperature == b.temperature, "router climate values are deterministic");
    check(a.chunkSurfaceLevel == b.chunkSurfaceLevel, "surface estimate is deterministic");
}

void testBiomeClimateSampling() {
    struct SampleCase {
        std::int64_t seed;
        double x;
        double y;
        double z;
    };
    constexpr std::array cases{
        SampleCase{0, 0, -64, 0},
        SampleCase{0, 24, 80, -56},
        SampleCase{1, -4, -4, -4},
        SampleCase{-1, 1000, 316, -1000},
        SampleCase{123456789, -120, 12, 240},
        SampleCase{987654321, 29999996, 64, -29999996},
    };
    const auto sameBits = [](float left, float right) {
        return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
    };
    for (const auto& testCase : cases) {
        mcworld::OverworldNoiseRouter fullRouter(testCase.seed);
        mcworld::OverworldNoiseRouter climateRouter(testCase.seed);
        const auto full = fullRouter.sample(testCase.x, testCase.y, testCase.z);
        const auto climate = climateRouter.sampleBiomeClimate(testCase.x, testCase.y, testCase.z);
        check(sameBits(climate.temperature, full.temperature), "biome temperature matches full router bits");
        check(sameBits(climate.vegetation, full.vegetation), "biome vegetation matches full router bits");
        check(sameBits(climate.continentalness, full.continentalness), "biome continentalness matches full router bits");
        check(sameBits(climate.erosion, full.erosion), "biome erosion matches full router bits");
        check(sameBits(climate.depth, full.depth), "biome depth matches full router bits");
        check(sameBits(climate.ridges, full.ridges), "biome ridges match full router bits");
    }
}

void testSeedVariationAndBounds() {
    mcworld::OverworldNoiseRouter first(1);
    mcworld::OverworldNoiseRouter second(2);
    bool differs = false;
    for (int z = -64; z <= 64; z += 32) {
        for (int x = -64; x <= 64; x += 32) {
            const float a = first.sampleFinalDensity(x, 64, z);
            const float b = second.sampleFinalDensity(x, 64, z);
            check(std::isfinite(a) && std::isfinite(b), "density samples are finite");
            differs = differs || std::abs(a - b) > 1.0e-5F;
        }
    }
    check(differs, "different seeds alter the density field");

    const float bottom = first.sampleFinalDensity(0, -64, 0);
    const float top = first.sampleFinalDensity(0, 320, 0);
    check(bottom > top, "Overworld slides favor solid terrain at the bottom and air at the top");
}

void testBeardifierInjection() {
    mcworld::OverworldNoiseRouter plain(99);
    mcworld::OverworldNoiseRouter adjusted(99, {}, std::make_shared<ConstantBeardifier>(0.25F));
    const float difference = adjusted.sampleFinalDensity(8, 80, 8) - plain.sampleFinalDensity(8, 80, 8);
    check(std::abs(difference - 0.25F) < 1.0e-6F, "beardifier contribution is added after final-density selection");
}

template <typename Action>
void checkInvalid(Action action, std::string_view message) {
    try {
        action();
        check(false, message);
    } catch (const std::invalid_argument&) {
    }
}

void testNoise() {
    using namespace mcworld::detail;
    const PerlinNoise first(1), second(2);
    check(std::abs(first.sample(0, 0, 0) - second.sample(0, 0, 0)) > 1.0e-5F,
        "noise origin varies by seed rather than being pinned to zero");
    check(first.sample(12.25, -8.5, 19.75) == first.sample(33554444.25, -8.5, 19.75),
        "noise wraps large coordinates before integer conversion");
    check(std::abs(first.sample(12.25, 8.5, 19.75) - first.sample(12.25, 8.5, 19.75, 0.2)) > 1.0e-5F,
        "vertical smearing changes blended noise samples");
    const NormalNoise unit(123, "test", -3, {1, 0, 1});
    const NormalNoise doubled(123, "test", -3, {2, 0, 2});
    for (int i = 0; i < 20; ++i) {
        check(std::abs(doubled.sample(i * 5, 7, -13) - 2 * unit.sample(i * 5, 7, -13)) < 1.0e-6F,
            "octave amplitude modifiers preserve their scale");
    }
    const NormalNoise single(123, "single", 0, {1});
    const PerlinNoise a(deriveSeed(123, "single/a/octave_0"));
    const PerlinNoise b(deriveSeed(123, "single/b/octave_0"));
    const float expected = (5.0F / 6.0F) * (a.sample(3, 4, 5)
        + b.sample(3 * 1.0181268882175227, 4 * 1.0181268882175227, 5 * 1.0181268882175227));
    check(std::abs(single.sample(3, 4, 5) - expected) < 1.0e-6F,
        "single-octave parity normalization matches the Java reference");
}

// Characterization test: these values were recorded from a build that was
// verified, sample by sample, against the pre-refactor implementation. They
// pin the shape of the density graph so that a refactor which accidentally
// re-associates a float expression, reorders a noise key or changes an
// interpolation lattice is caught immediately.
//
// They are NOT a specification: nothing here claims to match Java Minecraft.
// If you deliberately change generation, regenerate the table and say so in
// the commit message. The tolerance absorbs libm differences between
// platforms while staying far tighter than any real graph change.
void testKnownDensities() {
    constexpr float kTolerance = 1.0e-6F;

    struct DensityCase {
        std::int64_t seed;
        double x;
        double y;
        double z;
        float expected;
    };
    constexpr DensityCase densityCases[] = {
        {0LL, 0, 0, 0, 0.017009696F},
        {0LL, 7.5, 64, -13.25, -0.190210447F},
        {0LL, -120, 12, 240, 0.0281047821F},
        {1LL, 33, 96, 33, -0.243007153F},
        {1LL, -8, -32, 17, 0.0955519974F},
        {1LL, 512.5, 200, -512.5, -0.458333343F},
        {123456789LL, 137, 72, -291, 0.00624254346F},
        {123456789LL, 0, 300, 0, -0.0249947906F},
        {-1LL, 64, -60, 64, 0.0505738854F},
        {-1LL, 1000, 150, -1000, -0.458333343F},
        {42LL, 16.25, 40.5, 16.75, 0.00114414725F},
        {42LL, -256, 250, 128, -0.458333343F},
    };
    for (const DensityCase& testCase : densityCases) {
        mcworld::OverworldNoiseRouter router(testCase.seed);
        const float actual = router.sampleFinalDensity(testCase.x, testCase.y, testCase.z);
        check(std::abs(actual - testCase.expected) < kTolerance, "final density matches recorded value");
    }

    struct RouterCase {
        std::int64_t seed;
        mcworld::RouterSample expected;
    };
    // All sampled at (24, 80, -56).
    constexpr RouterCase routerCases[] = {
        {0LL, {-0.329984188F, 0.235893145F, -0.156900778F, 0.150859639F,
               -0.13023448F, 0.180103973F, 50.0F, -0.256850928F}},
        {987654321LL, {-0.959688962F, 0.182327569F, -0.32746923F, -0.453818113F,
                       -0.248750031F, -0.370430291F, 32.0F, -0.37250796F}},
    };
    for (const RouterCase& testCase : routerCases) {
        mcworld::OverworldNoiseRouter router(testCase.seed);
        const mcworld::RouterSample actual = router.sample(24.0, 80.0, -56.0);
        const mcworld::RouterSample& want = testCase.expected;
        check(std::abs(actual.temperature - want.temperature) < kTolerance, "temperature matches recorded value");
        check(std::abs(actual.vegetation - want.vegetation) < kTolerance, "vegetation matches recorded value");
        check(std::abs(actual.continentalness - want.continentalness) < kTolerance, "continentalness matches recorded value");
        check(std::abs(actual.erosion - want.erosion) < kTolerance, "erosion matches recorded value");
        check(std::abs(actual.depth - want.depth) < kTolerance, "depth matches recorded value");
        check(std::abs(actual.ridges - want.ridges) < kTolerance, "ridges matches recorded value");
        check(std::abs(actual.chunkSurfaceLevel - want.chunkSurfaceLevel) < kTolerance, "surface level matches recorded value");
        check(actual.chunkSurfaceLevel == router.sampleChunkSurfaceLevel(24.0, -56.0),
              "focused surface-level sampling matches the full router");
        check(std::abs(actual.finalDensity - want.finalDensity) < kTolerance, "final density matches recorded value");
    }
}

void testInvalidCoordinates() {
    mcworld::OverworldNoiseRouter router(0);
    checkInvalid([&] { (void)router.sample(std::numeric_limits<double>::quiet_NaN(), 0, 0); }, "reject NaN coordinates");
    checkInvalid([&] { (void)router.sampleBiomeClimate(0, std::numeric_limits<double>::infinity(), 0); },
                 "biome climate rejects infinite coordinates");
    checkInvalid([&] { (void)router.sampleFinalDensity(0, std::numeric_limits<double>::infinity(), 0); }, "reject infinite coordinates");
    checkInvalid([&] { (void)router.sampleFinalDensity(1.0e30, 0, 0); }, "reject out-of-range coordinates");
    check(std::isfinite(router.sampleFinalDensity(29999999, 64, -29999999)), "world-border sampling is finite");
}

} // namespace

int main() {
    testDeterminism();
    testBiomeClimateSampling();
    testSeedVariationAndBounds();
    testBeardifierInjection();
    testNoise();
    testKnownDensities();
    testInvalidCoordinates();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All mcworld tests passed\n";
    return EXIT_SUCCESS;
}
