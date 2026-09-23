#pragma once

#include <cstdint>
#include <memory>

namespace mcworld {

class BlendSampler {
public:
    virtual ~BlendSampler() = default;

    [[nodiscard]] virtual float alpha(double x, double y, double z) const;
    [[nodiscard]] virtual float offset(double x, double y, double z) const;
    [[nodiscard]] virtual float applyDensity(double x, double y, double z, float density) const;
};

class Beardifier {
public:
    virtual ~Beardifier() = default;

    [[nodiscard]] virtual float sample(double x, double y, double z) const;
};

struct RouterSample {
    float temperature{};
    float vegetation{};
    float continentalness{};
    float erosion{};
    float depth{};
    float ridges{};
    float chunkSurfaceLevel{};
    float finalDensity{};
};

// The six router values consumed by the multi-noise biome source. Sampling
// this subset avoids evaluating surface level and final density.
struct BiomeClimateSample {
    float temperature{};
    float vegetation{};
    float continentalness{};
    float erosion{};
    float depth{};
    float ridges{};
};

class OverworldNoiseRouter {
public:
    // Sampling populates caches; use a separate router per thread. Injected
    // samplers must remain stable for the lifetime of the router.
    explicit OverworldNoiseRouter(
        std::int64_t seed,
        std::shared_ptr<const BlendSampler> blender = {},
        std::shared_ptr<const Beardifier> beardifier = {}
    );
    ~OverworldNoiseRouter();

    OverworldNoiseRouter(OverworldNoiseRouter&&) noexcept;
    OverworldNoiseRouter& operator=(OverworldNoiseRouter&&) noexcept;
    OverworldNoiseRouter(const OverworldNoiseRouter&) = delete;
    OverworldNoiseRouter& operator=(const OverworldNoiseRouter&) = delete;

    [[nodiscard]] std::int64_t seed() const noexcept;
    [[nodiscard]] RouterSample sample(double x, double y, double z) const;
    [[nodiscard]] BiomeClimateSample sampleBiomeClimate(double x, double y, double z) const;
    [[nodiscard]] float sampleFinalDensity(double x, double y, double z) const;
    [[nodiscard]] float samplePreliminarySurface(int x, int z) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcworld
