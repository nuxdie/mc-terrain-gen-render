#include "mcworld/isosurface.hpp"
#include "mcworld/worldgen.hpp"
#include "noise.hpp"

#include <cmath>
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

void testMeshExtraction() {
    mcworld::OverworldNoiseRouter router(0);
    const mcworld::SurfaceMesh mesh = mcworld::buildChunkIsosurface(router);
    check(mesh.vertices.size() % 3 == 0, "surface mesh contains complete triangles");
    check(!mesh.vertices.empty(), "surface extraction produces terrain near sea level");
    for (const mcworld::SurfaceVertex& vertex : mesh.vertices) {
        check(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z), "mesh positions are finite");
        check(std::isfinite(vertex.nx) && std::isfinite(vertex.ny) && std::isfinite(vertex.nz), "mesh normals are finite");
    }
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

class SolidBlend final : public mcworld::BlendSampler {
public:
    float applyDensity(double, double, double, float) const override { return -1000.0F; }
};

class PlaneBeardifier final : public mcworld::Beardifier {
public:
    float sample(double x, double y, double z) const override {
        return static_cast<float>(8 + x * 0.25 + z * 0.125 - y) + 11.0F / 24.0F;
    }
};

void testPlanarMesh() {
    mcworld::OverworldNoiseRouter router(0, std::make_shared<SolidBlend>(), std::make_shared<PlaneBeardifier>());
    // Two adjacent chunks, including negative coordinates and lattice-aligned intersections.
    for (int chunkX : {-1, 0}) {
        const auto mesh = mcworld::buildChunkIsosurface(router, {chunkX, 0, 0, 32});
        check(!mesh.vertices.empty(), "analytic plane produces a mesh");
        const float length = std::sqrt(1.0F + 0.25F * 0.25F + 0.125F * 0.125F);
        for (const auto& v : mesh.vertices) {
            check(std::abs(v.y - (8 + (v.x + chunkX * 16) * 0.25F + v.z * 0.125F)) < 1.0e-5F,
                "mesh vertices lie on the analytic plane");
            check(std::abs(v.nx + 0.25F / length) < 1.0e-5F
                && std::abs(v.ny - 1.0F / length) < 1.0e-5F
                && std::abs(v.nz + 0.125F / length) < 1.0e-5F,
                "central-difference normals remain correct at chunk boundaries");
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); i += 3) {
            const auto& a = mesh.vertices[i];
            const auto& b = mesh.vertices[i + 1];
            const auto& c = mesh.vertices[i + 2];
            const float crossY = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            check(crossY > 0, "plane triangles are nondegenerate and outward-facing");
        }
    }
}

void testInvalidCoordinates() {
    mcworld::OverworldNoiseRouter router(0);
    checkInvalid([&] { (void)router.sample(std::numeric_limits<double>::quiet_NaN(), 0, 0); }, "reject NaN coordinates");
    checkInvalid([&] { (void)router.sampleFinalDensity(0, std::numeric_limits<double>::infinity(), 0); }, "reject infinite coordinates");
    checkInvalid([&] { (void)router.sampleFinalDensity(1.0e30, 0, 0); }, "reject out-of-range coordinates");
    checkInvalid([&] { (void)mcworld::buildChunkIsosurface(router, {std::numeric_limits<int>::max(), 0}); }, "reject overflowing chunks");
    checkInvalid([&] { (void)mcworld::buildChunkIsosurface(router, {0, 0, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}); }, "reject overflowing height ranges");
    checkInvalid([&] { (void)mcworld::buildChunkIsosurface(router, {0, 0, -64, 320, std::numeric_limits<float>::quiet_NaN()}); }, "reject NaN isolevel");
    check(std::isfinite(router.sampleFinalDensity(29999999, 64, -29999999)), "world-border sampling is finite");
}

} // namespace

int main() {
    testDeterminism();
    testSeedVariationAndBounds();
    testBeardifierInjection();
    testMeshExtraction();
    testNoise();
    testPlanarMesh();
    testInvalidCoordinates();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All mcworld tests passed\n";
    return EXIT_SUCCESS;
}
